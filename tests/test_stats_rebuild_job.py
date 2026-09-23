"""재집계는 **접수하고 뒤에서 돈다** (F-47) + 읽은 구간의 0 건은 0 으로 낸다 (F-49 배선).

재집계는 원본을 날마다 통째로 다시 훑는 작업이라 63일이면 분 단위로 걸린다. 그런데 응답은
게이트웨이 프록시를 지나야 하고 거기 한도가 5초다 — 동기로 내면 **작업은 끝까지 도는데 화면엔
504** 가 뜬다. 운영자는 실패로 읽고 다시 누른다(9/3 실측). 그래서 설치·검증과 같은 규약으로
바꿨다: 202 접수 → job_id → 진행률 폴링.

이 파일이 지키는 것:
  ① POST 는 **기다리지 않는다** — 작업이 도는 중에도 즉시 202 와 job_id 가 나온다
  ② 진행 중 또 부르면 409 + 진행 중인 job_id (두 번 눌러도 일이 두 배가 되지 않는다)
  ③ 진행률·결과·실패가 job 에 남고, 모르는 job_id 는 404
  ④ 진행 상태는 monitor 도 볼 수 있고, 실행은 admin 만 할 수 있다
  ⑤ `_calls_stats` 가 커버리지로 `ensure_svc` 를 정한다 — 빠진 날이 있으면 0 을 지어내지 않는다

sys.path 는 ems/core/oam/{src,vendor} — test_stats_probe.py 와 동일.
"""
import asyncio
import os
import sys
import threading
import time
import unittest
import unittest.mock

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))

from handlers import stats  # noqa: E402


class _JobIsolation(unittest.TestCase):
    def setUp(self):
        stats._REBUILD_JOBS.clear()

    tearDown = setUp

    def _start(self, frm='2026-09-01', to='2026-09-03', login='admin'):
        return stats._calls_rebuild_start({}, frm, to, login)

    def _wait_done(self, job_id, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            if stats._REBUILD_JOBS[job_id]['state'] != 'running':
                return stats._REBUILD_JOBS[job_id]
            time.sleep(0.01)
        self.fail(f'{job_id} 가 {timeout}초 안에 끝나지 않았다')


class RebuildAcceptsAndRunsBehind(_JobIsolation):

    def test_작업이_도는_중에도_즉시_접수증이_나온다(self):
        """게이트웨이 한도(5초)의 절반도 쓰지 않는다 — 작업이 오래 걸려도."""
        gate = threading.Event()

        def _slow(frm, to, on_day=None):
            gate.wait(3.0)          # 아직 끝나지 않은 작업을 흉내낸다
            return 7

        with unittest.mock.patch.object(stats.stats_rollup, 'enabled', lambda: True), \
             unittest.mock.patch.object(stats.stats_rollup, 'rebuild_range', _slow):
            t0 = time.time()
            r = self._start()
            elapsed = time.time() - t0
            self.assertEqual(r.status, 202, '접수는 202 — 완료가 아니다')
            self.assertLess(elapsed, 1.0, f'접수에 {elapsed:.2f}초나 걸렸다')
            self.assertEqual(r.body['state'], 'running')
            self.assertTrue(r.body['job_id'].startswith('rb-'))
            self.assertEqual(r.body['days_total'], 3)
            gate.set()
            self._wait_done(r.body['job_id'])

    def test_진행률이_하루마다_올라간다(self):
        def _progress(frm, to, on_day=None):
            on_day('2026-09-01', 1, 4)
            on_day('2026-09-02', 2, 9)
            return 9

        with unittest.mock.patch.object(stats.stats_rollup, 'enabled', lambda: True), \
             unittest.mock.patch.object(stats.stats_rollup, 'rebuild_range', _progress):
            r = self._start()
            job = self._wait_done(r.body['job_id'])
        self.assertEqual(job['state'], 'done')
        self.assertEqual(job['days_done'], 2)
        self.assertEqual(job['last_day'], '2026-09-02')
        self.assertEqual(job['buckets'], 9)

    def test_진행_중_또_부르면_409_와_진행_중인_job(self):
        """두 번 눌러도 같은 일을 두 번 하지 않는다."""
        gate = threading.Event()
        with unittest.mock.patch.object(stats.stats_rollup, 'enabled', lambda: True), \
             unittest.mock.patch.object(stats.stats_rollup, 'rebuild_range',
                                        lambda f, t, on_day=None: (gate.wait(3.0), 1)[1]):
            first = self._start()
            second = self._start()
            self.assertEqual(second.status, 409)
            self.assertEqual(second.body['error'], 'rebuild_in_progress')
            self.assertEqual(second.body['job_id'], first.body['job_id'])
            gate.set()
            self._wait_done(first.body['job_id'])

    def test_끝난_뒤에는_다시_걸_수_있다(self):
        with unittest.mock.patch.object(stats.stats_rollup, 'enabled', lambda: True), \
             unittest.mock.patch.object(stats.stats_rollup, 'rebuild_range',
                                        lambda f, t, on_day=None: 3):
            first = self._start()
            self._wait_done(first.body['job_id'])
            second = self._start()
            self.assertEqual(second.status, 202)
            self.assertNotEqual(second.body['job_id'], first.body['job_id'])
            self._wait_done(second.body['job_id'])

    def test_작업이_터지면_failed_로_남는다(self):
        """조용히 사라지면 운영자는 끝난 줄 안다."""
        def _boom(frm, to, on_day=None):
            raise RuntimeError('원본 읽기 실패')

        with unittest.mock.patch.object(stats.stats_rollup, 'enabled', lambda: True), \
             unittest.mock.patch.object(stats.stats_rollup, 'rebuild_range', _boom):
            r = self._start()
            job = self._wait_done(r.body['job_id'])
        self.assertEqual(job['state'], 'failed')
        self.assertIn('원본 읽기 실패', job['error'])

    def test_아무것도_안_만들면_이유를_남긴다(self):
        """단일 writer 를 못 잡으면 rebuild_range 는 조용히 0 을 낸다 — 그 침묵을 드러낸다."""
        with unittest.mock.patch.object(stats.stats_rollup, 'enabled', lambda: True), \
             unittest.mock.patch.object(stats.stats_rollup, 'rebuild_range',
                                        lambda f, t, on_day=None: 0):
            r = self._start()
            job = self._wait_done(r.body['job_id'])
        self.assertEqual(job['state'], 'done')
        self.assertTrue(job['note'], '0 건으로 끝난 이유가 있어야 한다')

    def test_롤업이_꺼져_있으면_409(self):
        with unittest.mock.patch.object(stats.stats_rollup, 'enabled', lambda: False):
            r = self._start()
        self.assertEqual(r.status, 409)
        self.assertEqual(r.body['error'], 'rollup_disabled')

    def test_날짜_형식이_틀리면_400(self):
        with unittest.mock.patch.object(stats.stats_rollup, 'enabled', lambda: True):
            r = self._start('2026-13-40', '2026-13-41')
        self.assertEqual(r.status, 400)

    def test_거꾸로_준_구간은_바로잡는다(self):
        with unittest.mock.patch.object(stats.stats_rollup, 'enabled', lambda: True), \
             unittest.mock.patch.object(stats.stats_rollup, 'rebuild_range',
                                        lambda f, t, on_day=None: 0):
            r = self._start('2026-09-05', '2026-09-01')
            self.assertEqual((r.body['from'], r.body['to']), ('2026-09-01', '2026-09-05'))
            self.assertEqual(r.body['days_total'], 5)
            self._wait_done(r.body['job_id'])


class RebuildStatusQuery(_JobIsolation):

    def _seed(self, job_id='rb-test-1', state='done'):
        stats._REBUILD_JOBS[job_id] = {
            'job_id': job_id, 'state': state, 'from': '2026-09-01', 'to': '2026-09-01',
            'days_total': 1, 'days_done': 1, 'last_day': '2026-09-01', 'buckets': 12,
            'started_at': 0.0, 'ended_at': 1.0, 'requested_by': 'admin',
            'error': '', 'note': '', '_secret': 'x',
        }

    def test_job_id_로_상태를_본다(self):
        self._seed()
        r = stats._calls_rebuild_status('rb-test-1')
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body['buckets'], 12)
        self.assertNotIn('_secret', r.body, '내부 키는 내지 않는다')

    def test_모르는_job_은_404(self):
        r = stats._calls_rebuild_status('rb-없는것')
        self.assertEqual(r.status, 404)

    def test_생략하면_최근_목록과_진행중_표시(self):
        self._seed('rb-old', 'done')
        self._seed('rb-now', 'running')
        r = stats._calls_rebuild_status('')
        self.assertEqual(r.body['running'], 'rb-now')
        self.assertEqual([j['job_id'] for j in r.body['jobs']], ['rb-now', 'rb-old'],
                         '최근 것부터')

    def test_보관은_최근_N건(self):
        with unittest.mock.patch.object(stats.stats_rollup, 'enabled', lambda: True), \
             unittest.mock.patch.object(stats.stats_rollup, 'rebuild_range',
                                        lambda f, t, on_day=None: 1):
            for i in range(stats._REBUILD_KEEP + 3):
                r = stats._calls_rebuild_start({}, '2026-09-01', '2026-09-01')
                end = time.time() + 5.0
                while stats._REBUILD_JOBS[r.body['job_id']]['state'] == 'running':
                    if time.time() > end:
                        self.fail('job 이 끝나지 않았다')
                    time.sleep(0.01)
        self.assertLessEqual(len(stats._REBUILD_JOBS), stats._REBUILD_KEEP)


class RebuildRouting(_JobIsolation):
    """경로·메서드·권한이 실제로 갈리는지 — 함수가 맞아도 배선이 틀리면 아무것도 안 된다."""

    def _call(self, method, path, role='admin', qs=None):
        from httpsrv.handler import HandlerArgs
        import services.admin_auth as aa

        def _fake_require_role(handler_args, min_role):
            from handlers.stats import HandlerResult as HR
            rank = {'monitor': 1, 'operator': 2, 'manager': 3, 'admin': 4}
            if rank.get(role, 0) < rank.get(min_role, 0):
                return None, HR(status=403, body={'error': '권한이 부족합니다'})
            return {'login_id': role}, None

        args = HandlerArgs(method=method, full_path=path, client_ip='127.0.0.1',
                           client_port=1, query_params=qs or {}, headers={})
        with unittest.mock.patch.object(aa, 'require_role', _fake_require_role), \
             unittest.mock.patch.object(stats.stats_rollup, 'enabled', lambda: True), \
             unittest.mock.patch.object(stats.stats_rollup, 'rebuild_range',
                                        lambda f, t, on_day=None: 1):
            return asyncio.run(stats.handle_stats(args, {'config': {}}))

    def test_POST_는_접수한다(self):
        r = self._call('POST', '/api/v1/stats/calls/rebuild',
                       qs={'date': '2026-09-01'})
        self.assertEqual(r.status, 202)
        self.assertEqual(r.body['from'], '2026-09-01')

    def test_GET_은_목록을_준다(self):
        r = self._call('GET', '/api/v1/stats/calls/rebuild', role='monitor')
        self.assertEqual(r.status, 200)
        self.assertIn('jobs', r.body)

    def test_GET_은_job_id_를_받는다(self):
        r = self._call('POST', '/api/v1/stats/calls/rebuild', qs={'date': '2026-09-01'})
        job_id = r.body['job_id']
        self._wait_done(job_id)
        r2 = self._call('GET', f'/api/v1/stats/calls/rebuild/{job_id}', role='monitor')
        self.assertEqual(r2.status, 200)
        self.assertEqual(r2.body['job_id'], job_id)

    def test_monitor_는_실행할_수_없다(self):
        r = self._call('POST', '/api/v1/stats/calls/rebuild', role='monitor',
                       qs={'date': '2026-09-01'})
        self.assertEqual(r.status, 403)

    def test_날짜를_안_주면_400(self):
        r = self._call('POST', '/api/v1/stats/calls/rebuild')
        self.assertEqual(r.status, 400)

    def test_PUT_은_405(self):
        r = self._call('PUT', '/api/v1/stats/calls/rebuild', qs={'date': '2026-09-01'})
        self.assertEqual(r.status, 405)


class EnsureSvcWiring(unittest.TestCase):
    """`_calls_stats` 가 **커버리지로** 0 채움을 정한다 — 못 본 구간은 0 이라고 말하지 않는다."""

    def _run(self, cov):
        seen = {}

        def _agg(rows, gran, svc, include_msg=False, ensure_svc=False):
            seen['ensure'] = ensure_svc
            return [], {}

        with unittest.mock.patch.object(stats.stats_rollup, 'read_range_filled',
                                        lambda *a, **k: ([], cov)), \
             unittest.mock.patch.object(stats.stats_rollup, 'aggregate', _agg), \
             unittest.mock.patch.object(stats.stats_rollup, 'fill_buckets',
                                        lambda b, g, f, t: b):
            asyncio.run(stats._calls_stats({}, '2026-09-17 00:00:00',
                                           '2026-09-17 23:59:59', '1h', 'volte'))
        return seen['ensure']

    def test_읽었으면_0_으로_채운다(self):
        self.assertTrue(self._run({'rollup': 3, 'scanned': 0, 'missing': 0}))
        self.assertTrue(self._run({'rollup': 0, 'scanned': 1, 'missing': 0}))

    def test_빠진_날이_있으면_채우지_않는다(self):
        self.assertFalse(self._run({'rollup': 3, 'scanned': 0, 'missing': 1}))

    def test_아무것도_못_읽었으면_채우지_않는다(self):
        self.assertFalse(self._run({'rollup': 0, 'scanned': 0, 'missing': 0}))


if __name__ == '__main__':
    unittest.main(verbosity=2)
