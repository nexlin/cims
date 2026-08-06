"""
stats.py probe 동시성 + TTL 캐시 단위 테스트.

Covers (핸들러 직접 호출 — 서버 미기동):
  - _all_media_stats: 노드별 동시 probe — down 노드 N개 비용이 N×timeout 이 아니라
    max(timeout). 직렬이면 게이트웨이 프록시 타임아웃(5s)을 넘겨 504 가 된다.
  - _all_media_stats: 이벤트 루프 없는 일반 스레드(alarm_sweeper 경로)에서도 동작.
  - _udp_request: attempts 재시도로 단발 데이터그램 유실 복구.
  - _cached: producer '이후' 시각 스탬프(느린 probe 가 자기 TTL 을 갉아먹지 않음),
    single-flight(동시 miss 시 producer 1회), stale-while-revalidate(갱신 중 stale 즉시 반환).

down 노드는 "bind 만 하고 응답하지 않는 UDP 소켓"으로 흉내낸다 — 커널이 포트를 할당하므로
ICMP port-unreachable 없이 실제 timeout 경로를 탄다. (닫힌 포트로 쏘면 즉시 실패해 무의미.)

sys.path 는 ems/core/oam/{src,vendor} — test_put_config_sync.py 와 동일.
"""
import asyncio
import os
import socket
import sys
import threading
import time
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))

from handlers import stats  # noqa: E402


def _dead_udp_node():
    """bind 만 하고 절대 응답하지 않는 UDP 소켓 → (sock, (ip, port))."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    return s, s.getsockname()


class _StatsCacheIsolation(unittest.TestCase):
    """각 테스트는 모듈 전역 캐시를 격리한다."""

    def setUp(self):
        stats._STATS_CACHE.clear()
        stats._CMP_LAST_GOOD.clear()
        getattr(stats, '_INFLIGHT', {}).clear()   # 수정 전 코드에도 붙도록(회귀 검출용)

    tearDown = setUp


class TestMediaProbeConcurrency(_StatsCacheIsolation):

    def test_all_media_stats_probes_nodes_in_parallel(self):
        """down 노드 2개 — 직렬이면 2×(timeout×attempts), 병렬이면 1×."""
        s1, ep1 = _dead_udp_node()
        s2, ep2 = _dead_udp_node()
        self.addCleanup(s1.close)
        self.addCleanup(s2.close)

        timeout_ms, attempts = 300, 2
        config = {'MediaServer': {
            'Endpoints': [f'{ep1[0]}:{ep1[1]}', f'{ep2[0]}:{ep2[1]}'],
            'ProbeTimeoutMs': timeout_ms, 'ProbeAttempts': attempts,
        }}
        budget = (timeout_ms / 1000.0) * attempts   # 노드 1개분 예산 = 0.6s

        started = time.perf_counter()
        out = stats._all_media_stats(config)
        elapsed = time.perf_counter() - started

        self.assertEqual([(n['host'], n['port']) for n in out], [ep1, ep2])  # 순서 보존
        self.assertEqual([n['stats'] for n in out], [{}, {}])               # 둘 다 miss
        # 병렬: ~budget. 직렬: ~2×budget. 1.5× 를 경계로 잡는다.
        self.assertLess(elapsed, budget * 1.5,
                        f"노드 probe 가 직렬로 보인다: {elapsed:.3f}s (예산 {budget:.3f}s)")
        self.assertGreaterEqual(elapsed, budget * 0.8)   # 실제로 timeout 을 탔는지

    def test_all_media_stats_runs_without_event_loop(self):
        """alarm_sweeper 는 일반 스레드에서 호출한다 — asyncio 의존이 없어야 한다."""
        s1, ep1 = _dead_udp_node()
        self.addCleanup(s1.close)
        config = {'MediaServer': {'Endpoints': [f'{ep1[0]}:{ep1[1]}'],
                                  'ProbeTimeoutMs': 100, 'ProbeAttempts': 1}}
        box = {}

        def run():
            try:
                box['out'] = stats._all_media_stats(config)
            except BaseException as e:      # noqa: BLE001
                box['err'] = e

        t = threading.Thread(target=run)
        t.start()
        t.join(timeout=5)
        self.assertNotIn('err', box, f"일반 스레드에서 실패: {box.get('err')!r}")
        self.assertEqual(len(box['out']), 1)

    def test_probe_cmp_keeps_last_good_on_miss(self):
        """probe miss 여도 last_good_ttl 안이면 최근 정상값을 유지한다."""
        s1, ep1 = _dead_udp_node()
        self.addCleanup(s1.close)
        key = f'{ep1[0]}:{ep1[1]}'
        stats._CMP_LAST_GOOD[key] = (time.time(), {'sessions': 7})

        got = stats._probe_cmp(ep1[0], ep1[1], timeout=0.1, attempts=1, last_good_ttl=30.0)
        self.assertEqual(got, {'sessions': 7})

        stats._STATS_CACHE.clear()
        expired = stats._probe_cmp(ep1[0], ep1[1], timeout=0.1, attempts=1, last_good_ttl=0.0)
        self.assertEqual(expired, {})


class TestUdpRetry(_StatsCacheIsolation):

    def test_udp_request_recovers_on_second_attempt(self):
        """첫 데이터그램은 버리고 두 번째에만 응답하는 서버 — attempts=2 로 복구."""
        srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        srv.bind(('127.0.0.1', 0))
        srv.settimeout(5)
        self.addCleanup(srv.close)
        ip, port = srv.getsockname()

        def responder():
            _first, _ = srv.recvfrom(4096)          # 유실 흉내 — 응답 안 함
            _second, addr = srv.recvfrom(4096)
            srv.sendto(b'{"status": "OK"}', addr)

        t = threading.Thread(target=responder, daemon=True)
        t.start()

        got = stats._udp_request(ip, port, {'event': 'STATS_REQUEST'},
                                 timeout=0.4, attempts=2)
        self.assertEqual(got, {'status': 'OK'})

        t.join(timeout=2)

    def test_udp_request_gives_up_after_attempts(self):
        s1, ep1 = _dead_udp_node()
        self.addCleanup(s1.close)
        started = time.perf_counter()
        got = stats._udp_request(ep1[0], ep1[1], {'x': 1}, timeout=0.2, attempts=3)
        elapsed = time.perf_counter() - started
        self.assertEqual(got, {})
        self.assertGreaterEqual(elapsed, 0.6 * 0.8)   # 3회 × 0.2s 를 실제로 소모
        self.assertLess(elapsed, 1.5)


class TestCached(_StatsCacheIsolation):

    def test_entry_is_stamped_after_producer_returns(self):
        """느린 producer 가 자기 TTL 을 갉아먹으면 안 된다.

        producer 실행시간 > TTL 인 경우가 결정적이다 — producer '이전' 시각으로 스탬프하면
        반환 즉시 만료라 다음 호출이 또 probe 한다(down 노드에서 캐시가 무의미해지는 버그).
        """
        orig_ttl = stats._STATS_TTL
        stats._STATS_TTL = 0.5                       # 테스트를 빠르게 — _cached 는 호출시 조회
        self.addCleanup(lambda: setattr(stats, '_STATS_TTL', orig_ttl))
        slow = 0.8                                   # producer 가 TTL 보다 오래 걸린다
        calls = []

        def producer():
            calls.append(1)
            time.sleep(slow)
            return 'v'

        self.assertEqual(stats._cached('k', producer), 'v')

        started = time.perf_counter()
        again = stats._cached('k', producer)          # 곧바로 재호출 → 캐시 히트여야 함
        elapsed = time.perf_counter() - started

        self.assertEqual(again, 'v')
        self.assertEqual(len(calls), 1, "producer 이전 시각으로 스탬프되어 즉시 만료됐다")
        self.assertLess(elapsed, 0.05)

    def test_concurrent_miss_runs_producer_once(self):
        """single-flight — 같은 키를 동시에 miss 해도 producer 는 1회."""
        calls = []
        lock = threading.Lock()

        def producer():
            with lock:
                calls.append(1)
            time.sleep(0.4)
            return 'v'

        results = []
        threads = [threading.Thread(target=lambda: results.append(stats._cached('sf', producer)))
                   for _ in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=5)

        self.assertEqual(len(calls), 1, f"producer 가 {len(calls)}회 실행됐다")
        self.assertEqual(results, ['v'] * 8)

    def test_stale_value_returned_while_refresh_in_flight(self):
        """SWR — 갱신 중인 키에 stale 값이 있으면 timeout 을 물지 않고 즉시 반환."""
        stats._STATS_CACHE['swr'] = (time.time() - stats._STATS_TTL - 1, 'stale')

        gate = threading.Event()

        def slow_producer():
            gate.wait(timeout=5)
            return 'fresh'

        leader = threading.Thread(target=lambda: stats._cached('swr', slow_producer))
        leader.start()
        while 'swr' not in stats._INFLIGHT:            # leader 가 갱신을 선점할 때까지
            time.sleep(0.005)

        started = time.perf_counter()
        got = stats._cached('swr', slow_producer)      # follower
        elapsed = time.perf_counter() - started

        self.assertEqual(got, 'stale')
        self.assertLess(elapsed, 0.05, "follower 가 leader 의 probe 를 기다렸다")

        gate.set()
        leader.join(timeout=5)
        self.assertEqual(stats._STATS_CACHE['swr'][1], 'fresh')

    def test_producer_exception_does_not_wedge_key(self):
        """producer 가 던져도 _INFLIGHT 가 남아 키를 영구 잠그면 안 된다."""
        def boom():
            raise RuntimeError('probe failed')

        with self.assertRaises(RuntimeError):
            stats._cached('boom', boom)
        self.assertNotIn('boom', stats._INFLIGHT)
        self.assertEqual(stats._cached('boom', lambda: 'ok'), {})   # 직전 finally 가 {} 로 스탬프


class TestHealthBudget(_StatsCacheIsolation):

    def test_health_stays_within_gateway_timeout_when_nodes_are_down(self):
        """down 노드 2개 + down CSP 여도 게이트웨이 5s 예산 안에서 응답."""
        s1, ep1 = _dead_udp_node()
        s2, ep2 = _dead_udp_node()
        s3, ep3 = _dead_udp_node()
        for s in (s1, s2, s3):
            self.addCleanup(s.close)

        config = {
            'MediaServer': {'Endpoints': [f'{ep1[0]}:{ep1[1]}', f'{ep2[0]}:{ep2[1]}'],
                            'ProbeTimeoutMs': 500, 'ProbeAttempts': 2},
            'CspNotify': {'Ip': ep3[0], 'Port': ep3[1]},
        }

        started = time.perf_counter()
        res = asyncio.run(stats._health(config))
        elapsed = time.perf_counter() - started

        self.assertEqual(res.status, 200)
        # csp/media/db/counts 는 gather — 최악은 합이 아니라 max(≈1.0s)
        self.assertLess(elapsed, 2.0, f"_health 예산 초과: {elapsed:.3f}s")


if __name__ == '__main__':
    unittest.main(verbosity=2)
