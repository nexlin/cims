"""공유 store 접근 불가 알람(A-PRC-028) — agent 감지 + OAM 평가.

실측 사고(2026-09-10)가 근거다: NAS 마운트가 죽자 그것을 쓰는 oam-svc 가 통째로 물려
(uninterruptible D — SIGKILL 도 안 들었다) **아무 알람도 열리지 않은 채** 성능 메뉴가
게이트웨이 504 만 뱉었다. 그 구간을 알람으로 드러내는 것이 이 검사의 대상이다.

두 축을 지킨다.
  ① 감지기가 고장난 자원에 물리지 않는다 — probe 는 워커 + 데드라인, 초과는 판정
     (`unresponsive`). 인라인으로 부르면 감지기까지 멈춰 무알람이 된다.
  ② 판정이 HA 승격 자격과 같은 함수를 쓴다 — 갈리면 "알람은 없는데 승격 부적격" 이 된다.

sys.path 는 agent/ 와 ems/core/oam/{src,vendor} — 두 쪽을 한 검사에서 본다.
"""
import os
import sys
import tempfile
import threading
import time
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, 'ems', 'core', 'oam', 'src'))
sys.path.insert(0, os.path.join(_REPO, 'ems', 'core', 'oam', 'vendor'))

from services import service_registry                              # noqa: E402


def _agent_module():
    """agent/cims_agent.py 를 모듈로 적재 (import 부작용 없이 함수만 쓴다)."""
    import importlib.util
    path = os.path.join(_REPO, 'agent', 'cims_agent.py')
    spec = importlib.util.spec_from_file_location('cims_agent_under_test', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestStoreProbeDeadline(unittest.TestCase):
    """probe 는 멈춘 마운트에서도 **호출자를 붙들지 않는다**."""

    @classmethod
    def setUpClass(cls):
        cls.agent = _agent_module()

    def setUp(self):
        self.agent._STORE_PROBE_CACHE.clear()
        self.agent._STORE_PROBE_INFLIGHT.clear()

    def test_not_mounted(self):
        self.agent._shared_store_mounted = lambda mp: False
        ok, reason = self.agent._shared_store_ready('/mnt/nowhere')
        self.assertFalse(ok)
        self.assertEqual(reason, 'not_mounted')

    def test_writable_is_ok(self):
        with tempfile.TemporaryDirectory() as td:
            self.agent._shared_store_mounted = lambda mp: True
            ok, reason = self.agent._shared_store_ready(td)
        self.assertTrue(ok)
        self.assertEqual(reason, 'ok')

    def test_hung_mount_returns_unresponsive_without_blocking(self):
        """write 가 영원히 안 끝나도 데드라인 안에 `unresponsive` 로 돌아온다.

        멈춘 NFS(hard)의 write 를 흉내낸다 — 실제 사고에서 이 호출이 안 돌아와서
        감지기가 같이 죽었고, 그래서 알람이 하나도 열리지 않았다."""
        release = threading.Event()
        self.addCleanup(release.set)          # 워커를 남기지 않는다

        def _hang(mp):
            release.wait(30)
            return (True, 'ok')

        self.agent._shared_store_mounted = lambda mp: True
        self.agent._store_write_probe = _hang
        self.agent._STORE_PROBE_DEADLINE = 0.3

        t0 = time.time()
        ok, reason = self.agent._shared_store_ready('/mnt/hung')
        elapsed = time.time() - t0

        self.assertFalse(ok)
        self.assertEqual(reason, 'unresponsive')
        self.assertLess(elapsed, 3.0, '데드라인을 넘겨 호출자가 붙들렸다')

    def test_hung_mount_spawns_one_worker_only(self):
        """물린 워커는 죽일 수 없다 — tick 마다 새로 띄우면 스레드가 무한히 쌓인다."""
        release = threading.Event()
        self.addCleanup(release.set)
        started = []

        def _hang(mp):
            started.append(1)
            release.wait(30)
            return (True, 'ok')

        self.agent._shared_store_mounted = lambda mp: True
        self.agent._store_write_probe = _hang
        self.agent._STORE_PROBE_DEADLINE = 0.2

        for _ in range(5):
            self.agent._shared_store_ready('/mnt/hung', force=True)
        self.assertEqual(len(started), 1, f'마운트당 워커 1개여야 한다 (실제 {len(started)})')

    def test_metric_reports_store_state(self):
        """metric.store 로 실린다 — verdict reasons[:6] 절단에 의존하지 않는다."""
        with tempfile.TemporaryDirectory() as td:
            self.agent._shared_store_mounted = lambda mp: True
            self.agent._read_ha_json = lambda: {
                'services': {'g1': {'shared_store': {'mount_point': td}}}}
            st = self.agent.collect_store_state()
        self.assertEqual(st.get('path'), td)
        self.assertTrue(st.get('ok'))
        self.assertEqual(st.get('reason'), 'ok')

    def test_no_shared_store_means_no_judgement(self):
        """공유 store 미구성(단일 노드)은 판정 대상이 아니다 — 빈 값."""
        self.agent._read_ha_json = lambda: {'services': {}}
        self.assertEqual(self.agent.collect_store_state(), {})


class TestStoreAlarmRule(unittest.TestCase):
    """OAM 평가 규칙 — 카탈로그 정의(A-PRC-028)와 일치."""

    def _rule(self):
        for r in service_registry._CORE_ALERT_RULES:
            if r.get('check') == 'store_unavailable':
                return r
        self.fail('store_unavailable 규칙이 없다')

    def test_rule_matches_catalog_definition(self):
        r = self._rule()
        self.assertEqual(r['code'], 'A-PRC-028')
        self.assertEqual(r['type'], 'dependency_unavailable')
        self.assertEqual(r['perceived_severity'], 'major')   # agent 감지행 severity
        self.assertEqual(r['scope'], 'agent')                # store 를 쓰지 않는 관측자
        self.assertEqual(r['mo_class'], 'host')

    def test_message_renders_reason_and_path(self):
        """운영자가 화면만 보고 '어디가 왜' 를 알아야 한다."""
        r = self._rule()
        msg = r['msg_open'].format(mo='ctrl01/store', reason='unresponsive',
                                   path='/mnt/cims')
        self.assertIn('unresponsive', msg)
        self.assertIn('/mnt/cims', msg)

    def test_alarm_map_has_check(self):
        m = service_registry._ALERT_CLASS_DEFAULTS.get('store_unavailable')
        self.assertIsNotNone(m, '_ALERT_CLASS_DEFAULTS 에 store_unavailable 없음')
        self.assertEqual(m['code'], 'A-PRC-028')


if __name__ == '__main__':
    unittest.main(verbosity=2)


class TestFailoverEvidence(unittest.TestCase):
    """절체 판정의 **근거를 남기는가** — 동작이 아니라 관측을 지킨다.

    실측 사고(2026-09-10 16:48): `zombie:oam` 으로 절체·영구 래치까지 갔는데, 그 한 번의
    readiness 가 왜 실패했는지 사후에 알 수 없었다 — 판정 상세는 health 캐시에만 있고
    3초마다 덮어써지기 때문이다. 원인 규명이 추정에서 멈춘 이유가 이것이라, 다음에는
    사실로 답할 수 있게 두 곳에 근거를 남긴다.
    """

    @classmethod
    def setUpClass(cls):
        cls.agent = _agent_module()

    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.addCleanup(self._td.cleanup)
        self.agent._HEALTH_DIR = os.path.join(self._td.name, 'health')
        os.makedirs(self.agent._HEALTH_DIR, exist_ok=True)

    def _write(self, mod, check, status, detail, ms=10, at=1000):
        self.agent._health_merge_write(mod, {check: {
            'status': status, 'detail': detail, 'duration_ms': ms,
            'checked_at': at, 'expires_at': at + 9}})

    def _read(self, mod):
        import json as _json
        with open(os.path.join(self.agent._HEALTH_DIR, f'{mod}.json')) as f:
            return _json.load(f)

    def test_failure_detail_survives_overwrite(self):
        """실패 사유는 다음 tick 이 덮어써도 `recent` 에 남는다."""
        self._write('oam', 'readiness', 'FAIL', ':4419 listening but timeout', ms=2013, at=100)
        for i in range(5):                         # 이후 성공이 계속 들어와도
            self._write('oam', 'readiness', 'SUCCESS', ':4419/tcp listening', at=200 + i)
        d = self._read('oam')
        self.assertEqual(d['checks']['readiness']['status'], 'SUCCESS')   # 현재 값은 최신
        rows = d['recent']['readiness']
        fails = [r for r in rows if r['status'] == 'FAIL']
        self.assertEqual(len(fails), 1, '실패 사유가 사라졌다')
        self.assertIn('timeout', fails[0]['detail'])
        self.assertEqual(fails[0]['ms'], 2013, '응답 지연 시간이 보존돼야 원인을 가른다')

    def test_success_streak_does_not_flood_history(self):
        """성공 연속은 이력을 채우지 않는다 — 실패 흔적이 밀려나면 의미가 없다."""
        for i in range(50):
            self._write('oam', 'readiness', 'SUCCESS', 'ok', at=300 + i)
        rows = self._read('oam')['recent']['readiness']
        self.assertLessEqual(len(rows), 2, f'성공만으로 이력이 {len(rows)}개 쌓였다')

    def test_slow_success_is_recorded(self):
        """성공이어도 **느리면** 남긴다 — 판정 임계(2초)에 다가가는 것이 전조다."""
        self._write('oam', 'readiness', 'SUCCESS', 'ok', ms=1500, at=400)
        rows = self._read('oam')['recent']['readiness']
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['ms'], 1500)

    def test_latch_freezes_health_snapshot(self):
        """래치가 서는 순간의 health 를 래치 파일에 박제한다 (사후 분석의 유일한 근거)."""
        import json as _json
        self._write('oam', 'readiness', 'FAIL', ':4419 listening but timeout', ms=2013, at=500)
        self.agent._HA_PERSIST_DIR = os.path.join(self._td.name, 'persist')
        self.agent._EVAL_LATCH.clear()
        self.agent._latch_set('g1', ['zombie:oam'])
        with open(self.agent._latch_path('g1')) as f:
            latch = _json.load(f)
        self.assertEqual(latch['reasons'], ['zombie:oam'])
        snap = latch.get('health_at_decision') or {}
        self.assertIn('oam', snap, '판정 시점 health 가 안 박혔다')
        self.assertEqual(snap['oam']['checks']['readiness']['status'], 'FAIL')
        self.assertIn('timeout', snap['oam']['checks']['readiness']['detail'])


class TestLoopLagWatch(unittest.TestCase):
    """이벤트 루프가 막히면 **그 사실과 그 순간의 스택**이 로그에 남는가.

    `/health` 는 `return {"status":"ok"}` 한 줄이라 느려질 이유가 루프 차단뿐인데,
    지금까지 서버 쪽에는 "늦게라도 200 을 줬다"는 기록만 남아 판정 실패(=절체)의 원인을
    댈 수 없었다. 지연을 직접 재고, 판정 임계를 넘기면 전 스레드 스택을 떠서 남긴다.
    """

    def _server(self, warn_sec):
        import io
        sys.path.insert(0, os.path.join(_REPO, 'ems', 'core', 'oam', 'src'))
        sys.path.insert(0, os.path.join(_REPO, 'ems', 'core', 'oam', 'vendor'))
        from httpsrv.server import HttpServer
        hits, verbose = [], []

        class _L:
            def log_warning(self, m): hits.append(m)
            def log_verbose(self, m): verbose.append(m)

        srv = object.__new__(HttpServer)
        srv._logger = _L()
        srv._LAG_WARN_SEC = warn_sec
        srv._LAG_DEBUG_SEC = 0.05
        srv._LAG_DUMP_COOLDOWN_SEC = 60
        return srv, hits, verbose, io

    def test_blocked_loop_is_reported_with_stack(self):
        import asyncio
        srv, hits, _v, io = self._server(warn_sec=0.5)

        async def main():
            t = asyncio.create_task(srv._watch_loop_lag())
            await asyncio.sleep(1.2)        # 워치독이 대기에 들어간 뒤
            time.sleep(1.5)                 # 루프를 동기 호출로 막는다(사고 재현)
            await asyncio.sleep(0.5)
            t.cancel()

        err = io.StringIO()
        old, sys.stderr = sys.stderr, err
        try:
            asyncio.run(main())
        finally:
            sys.stderr = old

        self.assertTrue(hits, '루프가 1.5초 막혔는데 경고가 없다')
        self.assertIn('지연', hits[0])
        self.assertIn('thread dump', err.getvalue(), '막힌 순간의 스택이 안 남았다')

    def test_quiet_loop_stays_silent(self):
        """평시에는 아무것도 남기지 않는다 — 관측 강화가 로그 오염이 되면 안 된다."""
        import asyncio
        srv, hits, verbose, _io = self._server(warn_sec=0.5)

        async def main():
            t = asyncio.create_task(srv._watch_loop_lag())
            await asyncio.sleep(1.3)
            t.cancel()

        asyncio.run(main())
        self.assertEqual(hits, [])
        self.assertEqual(verbose, [])


class TestClearLatchEndpoint(unittest.TestCase):
    """절체 래치 해제 — 운영자가 **판정만** 되돌릴 수단이 있는가.

    종전에는 해제 수단이 모듈 start/restart 뿐이라, 판정을 지우려면 실제 기동이라는
    부작용을 감수하거나 노드에 직접 들어가야 했다(콘솔 배너의 `[홀드 해제]` 는 버튼 없이
    문구뿐이었다). 판정을 되돌리는 것과 프로세스를 켜는 것은 다른 일이다.
    """

    def setUp(self):
        import tempfile as _tf
        from services import file_store, lease
        self._td = _tf.TemporaryDirectory()
        self.addCleanup(self._td.cleanup)
        self.config = {"CimsRuntimeDir": self._td.name}
        self.fs = file_store
        lease.acquire(file_store.runtime_root(self.config))   # 관리 store 는 단일 writer
        self.addCleanup(lease.release)
        g = {"id": 1, "name": "g1", "mode": "active_standby",
             "members": [{"agent_id": 10, "priority": 100}, {"agent_id": 11, "priority": 90}]}
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), 1, g)
        for aid, latched in ((10, True), (11, False)):
            file_store.save(file_store.domain_dir(self.config, "agents"), aid, {
                "id": aid, "name": f"n{aid}",
                "ha_state": {"g1": {"role": "FAULT", "latched": latched,
                                    "reasons": ["zombie:oam"] if latched else []}}})

    def _call(self, body):
        import asyncio
        from handlers.ha_groups import _clear_latch
        return asyncio.run(_clear_latch(1, body, self.config))

    def _jobs(self):
        return [j for j in self.fs.load_all(self.fs.domain_dir(self.config, "jobs"))
                if j.get("job_type") == "ha_clear_holds"]

    def test_clears_only_latched_members_by_default(self):
        """대상을 안 주면 **실제로 래치가 걸린 멤버만** — 응답이 무엇을 했는지 말해야 한다."""
        r = self._call({})
        self.assertEqual(r.status, 202)
        self.assertEqual([j["agent_id"] for j in r.body["jobs"]], [10])
        self.assertEqual([j.get("agent_id") for j in self._jobs()], [10])

    def test_explicit_member(self):
        r = self._call({"agent_id": 11})
        self.assertEqual(r.status, 202)
        self.assertEqual([j["agent_id"] for j in r.body["jobs"]], [11])

    def test_rejects_non_member(self):
        self.assertEqual(self._call({"agent_id": 99}).status, 404)

    def test_no_latched_member_is_409_not_silent_success(self):
        """풀 것이 없으면 조용한 성공이 아니라 사유를 준다 — 운영자가 오해하면 안 된다."""
        from services import file_store
        a = file_store.load(file_store.domain_dir(self.config, "agents"), 10)
        a["ha_state"]["g1"]["latched"] = False
        file_store.save(file_store.domain_dir(self.config, "agents"), 10, a)
        r = self._call({})
        self.assertEqual(r.status, 409)
        self.assertEqual(r.body["error"], "no_latched_member")

    def test_rejects_non_as_group(self):
        from services import file_store
        g = file_store.load(file_store.domain_dir(self.config, "ha_groups"), 1)
        g["mode"] = "all_active"
        file_store.save(file_store.domain_dir(self.config, "ha_groups"), 1, g)
        r = self._call({})
        self.assertEqual(r.status, 409)
        self.assertEqual(r.body["error"], "not_active_standby")

    def test_does_not_start_modules(self):
        """해제는 판정만 되돌린다 — start/restart job 을 만들면 안 된다."""
        self._call({})
        kinds = {j.get("job_type") for j in
                 self.fs.load_all(self.fs.domain_dir(self.config, "jobs"))}
        self.assertEqual(kinds, {"ha_clear_holds"}, f"기동 job 이 섞였다: {kinds}")
