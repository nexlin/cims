"""계측기 run 오케스트레이터 단위시험 — 가짜 워커(HTTP + 관측 스트림 송신)로 컴파일→풀→run→요약→판정 완주.

Covers:
  - compile: creds 신원 적재·domain 기본값·역할 창(disjoint_from)·워커 2대 배분(로컬 인덱스)·${ht} 바인딩·단발 max_instances
  - driver(단발): 풀 생성·run 시작 순서, 스트림 hello/agg/event 수집(metrics.sqlite·events.jsonl), 워커 stopped 로 종료,
    RFC 6076 요약(SER/SCR/백분위 근사)·expect 판정(pass / 미달 fail)·run.json·색인 verdict
  - 운영자 중단 → aborted, 워커 미응답 → error
  - Hist 백분위 근사(상한 버킷)

가짜 워커는 실제 계약(worker_pool_create/worker_run_start 스키마)으로 검증한 뒤 응답한다 — 워커 C++ 와 같은 문서를 본다.
"""
import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
_TESTER = os.path.join(_REPO, 'ems', 'tester', 'oam')
for _m in [m for m in list(sys.modules) if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_TESTER, 'src'))
sys.path.insert(1, os.path.join(_REPO, 'ems', 'core', 'oam', 'src'))
sys.path.insert(2, os.path.join(_REPO, 'ems', 'core', 'oam', 'vendor'))

S = M = R = C = None
_TMP = None
_CFG = None


def _free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


class FakeWorker:
    """POST /pools·/runs·/runs/{id}/stop·rate, GET /health·/runs/{id}. run 시작 시 스트림에 접속해 레코드를 보낸다."""

    def __init__(self, name, behaviour=None):
        self.name = name
        self.port = _free_port()
        self.pools = []
        self.runs = []
        self.rates = []
        self.stops = []
        self.state = 'idle'
        self.behaviour = behaviour or {}
        outer = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, st, body):
                raw = json.dumps(body).encode()
                self.send_response(st)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def _body(self):
                n = int(self.headers.get('Content-Length') or 0)
                return json.loads(self.rfile.read(n).decode()) if n else {}

            def do_GET(self):
                if self.path == '/health':
                    h = {'worker': outer.name, 'version': 't', 'max_endpoints': 100, 'max_saps': 10,
                         'cpu_pct': 1.0, 'active_endpoints': 0, 'active_run': None,
                         'clock_unix_ms': int(time.time() * 1000), 'pools': []}
                    if outer.behaviour.get('media') is not None:
                        h['media'] = outer.behaviour['media']
                    if outer.behaviour.get('real_ue') is not None:
                        h['real_ue'] = outer.behaviour['real_ue']
                    self._send(200, h)
                elif self.path.startswith('/runs/'):
                    self._send(200, {'state': outer.state, 'counters': {}})
                else:
                    self._send(404, {'error': 'nf'})

            def do_POST(self):
                b = self._body()
                if self.path == '/pools':
                    m, errs = M.validate('worker_pool_create', b)
                    if m is None:
                        self._send(400, {'error': errs}); return
                    outer.pools.append(b)
                    self._send(201, {'pool': b['pool'], 'endpoints': len(b['identities'])})
                elif self.path == '/runs':
                    m, errs = M.validate('worker_run_start', b)
                    if m is None:
                        self._send(400, {'error': errs}); return
                    outer.runs.append(b)
                    outer.state = 'running'
                    threading.Thread(target=outer._stream, args=(b,), daemon=True).start()
                    self._send(202, {'run_id': b['run_id'], 'state': 'prelude'})
                elif self.path.endswith('/rate'):
                    outer.rates.append(b['rate_saps'])
                    self._send(200, b)
                elif self.path.endswith('/stop'):
                    outer.stops.append(b)
                    outer.state = 'stopped'
                    self._send(202, {'state': 'draining'})
                else:
                    self._send(404, {'error': 'nf'})

        self.srv = HTTPServer(('127.0.0.1', self.port), H)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    @property
    def url(self):
        return f'http://127.0.0.1:{self.port}'

    def _stream(self, run):
        host, port = run['stream'].rsplit(':', 1)
        s = socket.create_connection((host, int(port)), timeout=5)
        def send(rec):
            s.sendall((json.dumps(rec) + '\n').encode())
        send({'kind': 'hello', 'worker': self.name, 'version': 't', 't': time.time()})
        t = int(time.time())
        n = int(run.get('max_instances') or 2)
        fail = self.behaviour.get('fail_one', False)
        for i in range(n):
            counters = {'attempts': 1, 'sessions': 1, 'completed': 1, 'legs': 2, 'rtp_rx': 100, 'rtp_lost': 1}
            timers = {'rrd_ms': {'count': 2, 'sum': 8, 'min': 3, 'max': 5, 'buckets': {'5': 2}},
                      'srd_ms': {'count': 1, 'sum': 1500, 'min': 1500, 'max': 1500, 'buckets': {'2000': 1}},
                      'sdd_ms': {'count': 1, 'sum': 2, 'min': 2, 'max': 2, 'buckets': {'2': 1}},
                      'jitter_ms': {'count': 2, 'sum': 20, 'min': 10, 'max': 10, 'buckets': {'10': 2}},
                      'rtp_loss_pct': {'count': 2, 'sum': 0.2, 'min': 0.1, 'max': 0.1, 'buckets': {'1': 2}},
                      'mos': {'count': 2, 'sum': 8.4, 'min': 4.2, 'max': 4.2, 'buckets': {'5': 2}}}
            if fail and i == n - 1:
                counters = {'attempts': 1, 'failed': 1, 'codes.503': 1, 'legs': 2}
                timers = {}
                send({'kind': 'event', 't': time.time(), 'run_id': run['run_id'], 'worker': self.name,
                      'code': 503, 'step': 'invite', 'detail': 'call failed'})
            send({'kind': 'agg', 't': t + i, 'bucket_s': 1, 'run_id': run['run_id'], 'worker': self.name,
                  'counters': counters, 'gauges': {'registered': 2, 'concurrent_sessions': 1}, 'timers': timers})
        send({'kind': 'log', 't': time.time(), 'worker': self.name, 'level': 'info', 'msg': 'run done'})
        time.sleep(0.3)
        if not self.behaviour.get('never_stop'):
            self.state = 'stopped'
        s.close()


def setUpModule():
    global S, M, R, C, _TMP, _CFG
    from services import tester_store as _S, tester_models as _M, tester_run as _R, tester_compile as _C
    S, M, R, C = _S, _M, _R, _C
    _TMP = tempfile.mkdtemp(prefix='tester-run-ut-')
    _CFG = {'CimsRuntimeDir': os.path.join(_TMP, 'runtime'),
            'Tester': {'DataDir': os.path.join(_TMP, 'data'), 'WorkerStreamIp': '127.0.0.1',
                       'WorkerStreamPort': _free_port(), 'WorkerStreamAdvertiseIp': '127.0.0.1'}}
    from services import file_store, lease
    lease.acquire(file_store.runtime_root(_CFG))
    S.init(_TESTER, _CFG)
    R.RUNS.init(_CFG)
    os.makedirs(os.path.join(S.user_scenarios_dir(), 'creds'), exist_ok=True)
    with open(os.path.join(S.user_scenarios_dir(), 'creds', 'ue.jsonl'), 'w') as f:
        for i in range(32):      # 워커마다 8 신원 — 풀이 source.offset 으로 구간을 나눠 쓴다(같은 신원을 두 워커가 쓰면 컴파일 오류)
            f.write(json.dumps({'user': f'+8213000000{i:02d}', 'authId': f'450338213000000{i:02d}', 'ha1': 'ab' * 16}) + '\n')


def tearDownModule():
    R.RUNS.shutdown()
    shutil.rmtree(_TMP, ignore_errors=True)


def _topology(workers):
    """v2(호스트›워커·대상 노드›풀) — 워커마다 UE 풀 `ue_<워커>` + 같은 group volte_ue(시나리오는 pool: volte_ue 로 참조).
    가짜 워커는 전부 127.0.0.1 이라 호스트 하나(hw)에 포트만 다르다."""
    return {'name': 'ut',
            'hosts': {'h1': {'ip': '10.0.0.1'}, 'hw': {'ip': '127.0.0.1'}},
            'workers': [{'name': w.name, 'host': 'hw', 'port': w.port, 'cpus': 1} for w in workers],
            'target': {'name': 'sut', 'kind': 'cims', 'nodes': {
                'csp': {'role': 'sip', 'host': 'h1', 'sip': {'access': {'udp': 5060, 'domains': ['volte.test']}}}}},
            'pools': {f'ue_{w.name}': {'kind': 'ue', 'worker': w.name, 'group': 'volte_ue', 'access': 'csp',
                                       'source': {'creds': 'creds/ue.jsonl', 'offset': 8 * i, 'count': 8}} for i, w in enumerate(workers)}}


def _wait_done(d, timeout=20):
    d.join(timeout)
    return d.state


class Compile(unittest.TestCase):
    def test_roles_workers_bindings(self):
        w1, w2 = FakeWorker('w1'), FakeWorker('w2')
        for w in (w1, w2):
            w.probe = None
        sc, _, _ = S.get_scenario('VOLTE-CALL-BASIC')
        topo_doc = _topology([w1, w2])
        topo = M.Topology.model_validate(topo_doc)
        from services import tester_workers as TW
        ws = TW.discover(topo_doc)
        for w in ws:
            w.probe()
        plan = C.compile_run('r1', sc, topo, topo_doc, None, {'ht': 7}, ws, lambda w: '127.0.0.1:1', 4, None)
        # 역할 pool=volte_ue 는 워커마다 group 으로 해석(ue_w1·ue_w2) — 워커 로컬 풀 8 신원을 caller/callee 가 균등 분할 [0,4)/[4,8)
        self.assertEqual(plan['roles']['caller']['workers'], {'w1': ['ue_w1', 0, 4], 'w2': ['ue_w2', 0, 4]})
        self.assertEqual(plan['roles']['callee']['workers'], {'w1': ['ue_w1', 4, 8], 'w2': ['ue_w2', 4, 8]})
        self.assertEqual(plan['roles']['caller']['total'], 8)
        self.assertEqual(plan['identities'], {'ue_w1': 8, 'ue_w2': 8})
        self.assertEqual(plan['max_instances'], 4)
        self.assertEqual(plan['phases'], {'prelude': [0], 'body': [1, 2, 3, 4], 'epilogue': []})
        hold = [s for s in plan['steps'] if s['step'] == 'media_hold'][0]
        self.assertEqual(hold['seconds'], 7)
        for name, pw in plan['workers'].items():
            run = pw['run']
            self.assertEqual(run['run_id'], 'r1')
            self.assertEqual(sum(len(p['identities']) for p in pw['pools']), 8)   # 워커 풀 신원 전부(분할은 풀 정의가)
            self.assertEqual(run['roles'], {'caller': f'ue_{name}', 'callee': f'ue_{name}'})
            for role, (b, e) in run['role_slices'].items():
                self.assertTrue(0 <= b <= e <= 8)
            self.assertEqual(pw['pools'][0]['identities'][0]['domain'], 'volte.test')
            self.assertEqual(pw['pools'][0]['target_csp']['ip'], '10.0.0.1')
        self.assertEqual(sum(pw['run']['max_instances'] for pw in plan['workers'].values()), 4)

    def test_during_flattened_and_group_resolution(self):
        # media_hold.during → hold 3 → dtmf → hold 7 (마지막 조각이 기대치) · 워커가 하나면 그 워커만 후보
        w1 = FakeWorker('w1')
        topo_doc = _topology([w1])
        topo = M.Topology.model_validate(topo_doc)
        from services import tester_workers as TW
        sc = M.Scenario.model_validate({'id': 'UT-DUR', 'roles': {'a': {'pool': 'volte_ue', 'count': 2}, 'b': {'pool': 'volte_ue', 'disjoint_from': 'a', 'count': 2}},
                                        'flow': [{'step': 'register', 'who': ['a', 'b']}, {'step': 'invite', 'from': 'a', 'to': 'b'},
                                                 {'step': 'answer', 'who': ['b']},
                                                 {'step': 'media_hold', 'seconds': 10, 'expect': {'rtp_loss_pct': {'max': 1}},
                                                  'during': [{'at_s': 3, 'step': 'dtmf', 'from': 'a', 'payload': '12#'}]},
                                                 {'step': 'bye', 'from': 'a'}]})
        plan = C.compile_run('r2', sc, topo, topo_doc, None, {}, TW.discover(topo_doc), lambda w: 'x:1', 1, None)
        kinds = [(s['step'], s.get('seconds'), s['src']) for s in plan['steps']]
        self.assertEqual(kinds, [('register', None, 0), ('invite', None, 1), ('answer', None, 2),
                                 ('media_hold', 3, 3), ('dtmf', None, 3), ('media_hold', 7, 3), ('bye', None, 4)])
        self.assertEqual(plan['steps'][3].get('expect', {}), {})
        self.assertEqual(plan['steps'][5]['expect'], {'rtp_loss_pct': {'max': 1}})
        self.assertNotIn('src', plan['workers']['w1']['run']['steps'][0])      # 워커 계약에는 src 없음
        # 워커가 지원하지 않는 단계(sds_send — MCData 미구현)는 컴파일 오류
        sc2 = M.Scenario.model_validate({'id': 'UT-NS', 'roles': {'a': {'pool': 'volte_ue'}},
                                         'flow': [{'step': 'sds_send', 'from': 'a'}]})
        with self.assertRaises(C.CompileError):
            C.compile_run('r3', sc2, topo, topo_doc, None, {}, TW.discover(topo_doc), lambda w: 'x:1', 1, None)
        # pickup 의 payload(피처코드) 는 ${var} 바인딩으로 준다 — 컴파일이 문자열로 푼다(숫자 문자열도 문자열). 없으면 오류
        sc3, _, _ = S.get_scenario('VOLTE-PICKUP-GROUP')
        with self.assertRaises(C.CompileError):
            C.compile_steps(sc3, {'ht': 3})
        steps3 = C.compile_steps(sc3, {'ht': 3, 'pickup_code': '**'})
        self.assertEqual([s for s in steps3 if s['step'] == 'pickup'][0]['payload'], '**')
        self.assertEqual([s for s in C.compile_steps(sc3, {'ht': 3, 'pickup_code': 77}) if s['step'] == 'pickup'][0]['payload'], '77')

    def test_ptt_group_session(self):
        """그룹 세션(group_call) — 역할은 신원 창을 나누지 않고(전부 풀 전체) multi_roles·service·ptt_group 이 워커 계약으로 간다.
        그룹 자원 = 멤버가 되는 그룹 수(Little 검산의 분모), PTT 단계는 service=ptt 풀에서만."""
        from services import tester_workers as TW, tester_plan as P
        with open(os.path.join(S.user_scenarios_dir(), 'creds', 'ptt.jsonl'), 'w') as f:
            for i in range(7):   # g1 = 4 명 · g2 = 2 명 · 그룹 없는 신원 1
                g = 'g1' if i < 4 else 'g2' if i < 6 else None
                rec = {'user': f'+8214000000{i:02d}', 'authId': f'450338214000000{i:02d}', 'ha1': 'cd' * 16}
                if g:
                    rec['group'] = g
                f.write(json.dumps(rec) + '\n')
        w1 = FakeWorker('w1')
        topo_doc = _topology([w1])
        topo_doc['target']['nodes']['csp']['sip']['access']['domains'] = ['volte.test', 'ptt.test']
        topo_doc['pools']['ptt_ue'] = {'kind': 'ue', 'worker': 'w1', 'service': 'ptt', 'access': 'csp', 'source': {'creds': 'creds/ptt.jsonl'}}
        topo = M.Topology.model_validate(topo_doc)
        sc, _, _ = S.get_scenario('PTT-GROUP-CALL-BASIC')
        plan = C.compile_run('rp', sc, topo, topo_doc, None, {'ht': 3}, TW.discover(topo_doc), lambda w: 'x:1', 1, None)
        run = plan['workers']['w1']['run']
        self.assertEqual(run['multi_roles'], ['listeners'])
        self.assertEqual(run['role_slices'], {'talker': [0, 7], 'listeners': [0, 7]})
        pool = plan['workers']['w1']['pools'][0]
        self.assertEqual((pool['service'], pool['identities'][0]['domain'], pool['identities'][0]['ptt_group']), ('ptt', 'ptt.test', 'g1'))
        self.assertNotIn('ptt_group', pool['identities'][6])
        self.assertEqual([s['step'] for s in run['steps']], ['register', 'group_call', 'floor_request', 'media_hold', 'floor_release', 'bye'])
        gs = plan['group_session']
        self.assertEqual((gs['groups'], gs['usable'], gs['need_members'], gs['members_max']), (2, 2, 2, 4))
        self.assertTrue(plan['roles']['listeners']['multi'])
        # 역할 셋(단일 둘 + multi) → 멤버 3 명 이상인 그룹만 — g2(2 명)는 빠진다
        sc3, _, _ = S.get_scenario('PTT-FLOOR-HANDOVER')
        plan3 = C.compile_run('rp3', sc3, topo, topo_doc, None, {'ht': 3}, TW.discover(topo_doc), lambda w: 'x:1', 1, None)
        self.assertEqual((plan3['group_session']['usable'], plan3['group_session']['need_members']), (1, 3))
        # 계획 미리보기 — Little 검산의 자원은 그룹 수
        pv = P.build_plan(sc3, topo, topo_doc, None, {'ht': 3}, 5, 5.0, probe=False)
        self.assertTrue(pv['ok'], pv['errors'])
        self.assertTrue(any('쓸 수 있는 그룹' in w for w in pv['warnings']), pv['warnings'])
        self.assertTrue(any('그룹 세션' in n for n in pv['notes']))
        # PTT 단계는 service=ptt 풀에서만
        bad = M.Scenario.model_validate({'id': 'UT-PTT-BAD', 'roles': {'a': {'pool': 'volte_ue'}},
                                         'flow': [{'step': 'group_call', 'from': 'a'}]})
        with self.assertRaisesRegex(C.CompileError, 'service=ptt'):
            C.compile_run('rp4', bad, topo, topo_doc, None, {}, TW.discover(topo_doc), lambda w: 'x:1', 1, None)
        # 멤버가 되는 그룹이 없으면 컴파일 오류
        topo_doc['pools']['ptt_ue']['source'] = {'creds': 'creds/ptt.jsonl', 'offset': 6}
        topo2 = M.Topology.model_validate(topo_doc)
        with self.assertRaisesRegex(C.CompileError, 'MCPTT 그룹이 없다'):
            C.compile_run('rp5', sc, topo2, topo_doc, None, {'ht': 3}, TW.discover(topo_doc), lambda w: 'x:1', 1, None)

    def test_media_plane_samples_and_plan(self):
        """샘플 발췌(RunStart.samples)·sample/loop/rtp 의 워커 계약 전달 · 계획 미리보기의 샘플 파일 대조·RTP 상한·모드 경고."""
        from services import tester_workers as TW
        from services import tester_plan as P
        w1 = FakeWorker('w1', {'media': {'rtp_streams': 0, 'max_rtp_streams': 2, 'sample_dir': '/s', 'files': ['rb.pcmu']}})
        topo_doc = _topology([w1])
        topo_doc['media'] = {'samples': {'rb': {'pcmu': 'rb.pcmu', 'pcma': 'rb.pcma', 'amr-wb': 'synthetic'}, 'unused': {'pcmu': 'u.pcmu'}}}
        topo = M.Topology.model_validate(topo_doc)
        flow = [{'step': 'register', 'who': ['a', 'b']},
                {'step': 'invite', 'from': 'a', 'to': 'b', 'media': {'audio': 'pcmu', 'rtp': 'explicit'}},
                {'step': 'answer', 'who': ['b']},
                {'step': 'media_send', 'who': ['a'], 'sample': 'rb', 'loop': False, 'after_ms': 100},
                {'step': 'media_hold', 'seconds': 4, 'during': [{'at_s': 2, 'step': 'media_stop', 'who': ['a']}]},
                {'step': 'bye', 'from': 'a'}]
        roles = {'a': {'pool': 'volte_ue', 'count': 2}, 'b': {'pool': 'volte_ue', 'disjoint_from': 'a', 'count': 2}}
        sc = M.Scenario.model_validate({'id': 'UT-MEDIA', 'roles': roles, 'flow': flow})
        plan = C.compile_run('r5', sc, topo, topo_doc, None, {}, TW.discover(topo_doc), lambda w: 'x:1', 1, None)
        run = plan['workers']['w1']['run']
        self.assertEqual(run['samples'], {'rb': {'pcmu': 'rb.pcmu', 'pcma': 'rb.pcma', 'amr-wb': 'synthetic'}})   # 참조한 것만
        steps = {s['step']: s for s in run['steps']}
        self.assertEqual(steps['invite']['media']['rtp'], 'explicit')
        self.assertEqual((steps['media_send']['sample'], steps['media_send']['loop'], steps['media_send']['after_ms']), ('rb', False, 100))
        self.assertEqual([s['step'] for s in run['steps']][4:7], ['media_hold', 'media_stop', 'media_hold'])
        # 계획 미리보기 — 워커 샘플 디렉터리에 rb.pcma 가 없다 → 오류, 동시 2 인스턴스 × 역할 2 = RTP 4 > 상한 2 → 경고
        out = P.build_plan(sc, topo, topo_doc, None, {}, 2, 5.0)
        self.assertFalse(out['ok'])
        self.assertTrue(any('rb.pcma' in e for e in out['errors']), out['errors'])
        self.assertTrue(any('MaxRtpStreams' in w for w in out['warnings']), out['warnings'])
        self.assertEqual(out['media'], {'modes': ['explicit'], 'uses_rtp': True})
        # 라이브러리에 없는 샘플 = 컴파일 오류
        flow2 = [dict(f) for f in flow]
        flow2[3] = {**flow2[3], 'sample': 'nope'}
        sc2 = M.Scenario.model_validate({'id': 'UT-MEDIA2', 'roles': roles, 'flow': flow2})
        with self.assertRaises(C.CompileError):
            C.compile_run('r6', sc2, topo, topo_doc, None, {}, TW.discover(topo_doc), lambda w: 'x:1', 1, None)
        # 워커가 보유 샘플을 좁혀 선언했는데 그 안에 없다 → 오류
        topo_doc['workers'][0]['media'] = {'samples': ['other']}
        topo3 = M.Topology.model_validate(topo_doc)
        out3 = P.build_plan(sc, topo3, topo_doc, None, {}, 1, None)
        self.assertTrue(any('보유하지 않는다' in e for e in out3['errors']), out3['errors'])
        # 시그널링 전용 — RTP 기대치 경고
        sig = M.Scenario.model_validate({'id': 'UT-SIG', 'roles': roles, 'flow': [
            {'step': 'register', 'who': ['a', 'b']}, {'step': 'invite', 'from': 'a', 'to': 'b', 'media': {'rtp': 'none'}},
            {'step': 'answer', 'who': ['b']}, {'step': 'media_hold', 'seconds': 2, 'expect': {'rtp_loss_pct': {'max': 1}}},
            {'step': 'bye', 'from': 'a'}]})
        del topo_doc['workers'][0]['media']
        out4 = P.build_plan(sig, M.Topology.model_validate(topo_doc), topo_doc, None, {}, 1, None)
        self.assertTrue(out4['ok'], out4['errors'])
        self.assertTrue(any('시그널링 전용' in w for w in out4['warnings']), out4['warnings'])
        self.assertFalse(out4['media']['uses_rtp'])
        w1.close() if hasattr(w1, 'close') else None

    def test_identity_overlap_across_workers_rejected(self):
        # 두 워커 풀이 같은 creds 구간을 쓰면 등록 바인딩이 서로를 덮는다 — 컴파일 오류. offset 으로 나누면 통과(test_roles_workers_bindings)
        from services import tester_workers as TW
        w1, w2 = FakeWorker('w1'), FakeWorker('w2')
        topo_doc = _topology([w1, w2])
        topo_doc['pools']['ue_w2']['source'] = {'creds': 'creds/ue.jsonl', 'offset': 4, 'count': 8}    # [4,12) — ue_w1 [0,8) 과 겹친다
        sc, _, _ = S.get_scenario('VOLTE-CALL-BASIC')
        with self.assertRaises(C.CompileError) as cm:
            C.compile_run('r9', sc, M.Topology.model_validate(topo_doc), topo_doc, None, {'ht': 1}, TW.discover(topo_doc), lambda w: 'x:1', 2, None)
        self.assertIn('겹친다', str(cm.exception))

    def test_db_identity_source(self):
        """source.db — db 노드 접속 + 환경변수 자격으로 H(A1) 보유 가입자를 읽는다(pymysql 은 가짜로 갈아 끼운다)."""
        import sys as _sys, types
        from services import tester_workers as TW
        calls = {}

        class Cur:
            def execute(self, q, args):
                calls['q'], calls['args'] = q, args
            def fetchall(self):
                return [(f'+82130000{i:04d}', f'45033{i:010d}', 'cd' * 16) for i in range(8)]

        class Conn:
            def cursor(self): return Cur()
            def close(self): calls['closed'] = True

        fake = types.ModuleType('pymysql')
        fake.connect = lambda **kw: (calls.update(conn=kw) or Conn())
        keep = _sys.modules.get('pymysql')
        _sys.modules['pymysql'] = fake
        w1 = FakeWorker('w1')
        topo_doc = _topology([w1])
        topo_doc['target']['nodes']['db'] = {'role': 'db', 'host': 'h1', 'db': {'port': 3307, 'name': 'cimsdb', 'user_env': 'UT_DB_USER', 'password_env': 'UT_DB_PASS'}}
        topo_doc['pools']['ue_w1']['source'] = {'db': 'db', 'table': 'volte_subscriptions', 'offset': 16, 'count': 8}
        topo = M.Topology.model_validate(topo_doc)
        sc, _, _ = S.get_scenario('VOLTE-CALL-BASIC')
        try:
            os.environ.pop('UT_DB_USER', None)
            with self.assertRaises(C.CompileError) as cm:       # 자격 환경변수 없음 — 조용히 빈 값으로 접속하지 않는다
                C.compile_run('r10', sc, topo, topo_doc, None, {'ht': 1}, TW.discover(topo_doc), lambda w: 'x:1', 1, None)
            self.assertIn('DB 자격', str(cm.exception))
            os.environ['UT_DB_USER'], os.environ['UT_DB_PASS'] = 'tester', 'pw'
            plan = C.compile_run('r10', sc, topo, topo_doc, None, {'ht': 1}, TW.discover(topo_doc), lambda w: 'x:1', 1, None)
            self.assertEqual((calls['conn']['host'], calls['conn']['port'], calls['conn']['database'], calls['conn']['user']), ('10.0.0.1', 3307, 'cimsdb', 'tester'))
            self.assertEqual(calls['args'], ('UDP', 8, 16))        # 풀 transport 로 접속 가능한 가입자만 · count · offset
            self.assertIn('volte_subscriptions', calls['q'])
            self.assertTrue(calls.get('closed'))
            ident = plan['workers']['w1']['pools'][0]['identities'][0]
            self.assertEqual((ident['user'], ident['domain'], ident['ha1'], ident['auth_id']), ('+821300000000', 'volte.test', 'cd' * 16, '450330000000000'))
        finally:
            os.environ.pop('UT_DB_USER', None); os.environ.pop('UT_DB_PASS', None)
            if keep is not None:
                _sys.modules['pymysql'] = keep
            else:
                _sys.modules.pop('pymysql', None)

    def test_disjoint_needs_room(self):
        # caller.count=8 이 풀 전체를 쓰면 disjoint 인 callee 창이 없다 → CompileError
        sc, _, _ = S.get_scenario('VOLTE-CALL-BASIC')
        sc = sc.model_copy(deep=True)
        sc.roles['caller'].count = 8
        rp = {'caller': 'volte_ue', 'callee': 'volte_ue'}
        with self.assertRaises(C.CompileError):
            C.role_ranges(sc.roles, rp, {'volte_ue': 8})
        # 균등 분할 기본값
        sc2, _, _ = S.get_scenario('VOLTE-CALL-BASIC')
        rr = C.role_ranges(sc2.roles, rp, {'volte_ue': 10})
        self.assertEqual(rr['caller'], ('volte_ue', 0, 5))
        self.assertEqual(rr['callee'], ('volte_ue', 5, 10))

    def test_db_source_rejected(self):
        with self.assertRaises(C.CompileError):
            C.load_identities('p', {'kind': 'ue', 'source': {'db': 'target', 'table': 'volte_subscriptions', 'count': 2}})

    def test_real_ue_pool(self):
        """실단말(real-ue) 풀(§3.3) — PoolCreate kind=real-ue(service·tls_verify·신원), 단계 게이트(REAL_UE_STEPS), 호는 rtp auto 만,
        AKA 신원 거절, 신원 겹침 검사 포함, 계획 미리보기의 RealUe.MaxProcesses 검산."""
        from services import tester_workers as TW, tester_plan as P
        w1 = FakeWorker('w1', behaviour={'real_ue': {'processes': 1, 'max': 2, 'cli': '/opt/w/bin/cimsue-cli'}})
        topo_doc = _topology([w1])
        topo_doc['pools']['real_w1'] = {'kind': 'real-ue', 'worker': 'w1', 'access': 'csp', 'transport': 'udp', 'srtp': 'off', 'tls_verify': True,
                                        'source': {'creds': 'creds/ue.jsonl', 'offset': 8, 'count': 2}}
        topo = M.Topology.model_validate(topo_doc)
        self.assertEqual(topo.pool_service('real_w1'), 'volte')
        ws = TW.discover(topo_doc)
        for w in ws:
            w.probe()
        sc = M.Scenario.model_validate({'id': 'UT-REAL', 'roles': {'real': {'pool': 'real_w1'}, 'callee': {'pool': 'volte_ue', 'count': 2}},
                                        'flow': [{'step': 'register', 'who': ['real', 'callee']}, {'step': 'invite', 'from': 'real', 'to': 'callee'},
                                                 {'step': 'answer', 'who': ['callee'], 'expect': {'real_srd_ms': {'p95': 2000}}},
                                                 {'step': 'media_hold', 'seconds': 3, 'expect': {'real_rtp_loss_pct': {'max': 1}, 'real_mos': {'min': 3.5}}},
                                                 {'step': 'bye', 'from': 'real'}]})
        plan = C.compile_run('rr', sc, topo, topo_doc, None, {}, ws, lambda w: 'x:1', 1, None)
        pools = {p['pool']: p for p in plan['workers']['w1']['pools']}
        self.assertEqual(pools['real_w1']['kind'], 'real-ue')
        self.assertEqual(pools['real_w1']['service'], 'volte')
        self.assertTrue(pools['real_w1']['tls_verify'])
        self.assertEqual([i['user'] for i in pools['real_w1']['identities']], ['+821300000008', '+821300000009'])
        self.assertEqual(plan['roles']['real']['kind'], 'real-ue')
        self.assertEqual(plan['roles']['real']['service'], 'volte')
        self.assertEqual(plan['workers']['w1']['run']['roles'], {'real': 'real_w1', 'callee': 'ue_w1'})
        # 단계 게이트 — refer(실스택이 REFER 응답을 이벤트로 내지 않음)·media_send 는 실단말 행위자 불가, 호의 media.rtp 는 auto 만
        for bad in ([{'step': 'register', 'who': ['real', 'callee']}, {'step': 'invite', 'from': 'callee', 'to': 'real'}, {'step': 'answer', 'who': ['real']},
                     {'step': 'refer', 'from': 'real', 'to': 'callee'}],
                    [{'step': 'register', 'who': ['real', 'callee']}, {'step': 'invite', 'from': 'real', 'to': 'callee', 'media': {'rtp': 'none'}},
                     {'step': 'answer', 'who': ['callee']}, {'step': 'bye', 'from': 'real'}],
                    [{'step': 'register', 'who': ['real', 'callee']}, {'step': 'invite', 'from': 'callee', 'to': 'real'}, {'step': 'answer', 'who': ['real']},
                     {'step': 'media_send', 'who': ['real']}, {'step': 'bye', 'from': 'real'}]):
            sc_bad = M.Scenario.model_validate({'id': 'UT-REAL-BAD', 'roles': {'real': {'pool': 'real_w1'}, 'callee': {'pool': 'volte_ue', 'count': 2}}, 'flow': bad})
            with self.assertRaises(C.CompileError):
                C.compile_run('rb', sc_bad, topo, topo_doc, None, {}, ws, lambda w: 'x:1', 1, None)
        # 같은 신원이 가상 풀과 겹치면 오류(source.offset 이 겹치게)
        topo_doc2 = json.loads(json.dumps(topo_doc))
        topo_doc2['pools']['real_w1']['source']['offset'] = 4
        with self.assertRaises(C.CompileError):
            C.compile_run('ro', sc, M.Topology.model_validate(topo_doc2), topo_doc2, None, {}, ws, lambda w: 'x:1', 1, None)
        # AKA 신원은 cimsue-cli 가 받지 않는다
        with open(os.path.join(S.user_scenarios_dir(), 'creds', 'aka.jsonl'), 'w') as f:
            f.write(json.dumps({'user': '+821399000001', 'authId': '450339000000001', 'k': '00' * 16, 'opc': '11' * 16}) + '\n')
        topo_doc3 = json.loads(json.dumps(topo_doc))
        topo_doc3['pools']['real_w1']['source'] = {'creds': 'creds/aka.jsonl'}
        with self.assertRaises(C.CompileError):
            C.compile_run('ra', sc, M.Topology.model_validate(topo_doc3), topo_doc3, None, {}, ws, lambda w: 'x:1', 1, None)
        # 계획 미리보기 — 실단말 2 + 진행 중 1 > RealUe.MaxProcesses 2 → 오류, 용량 행에 real_ue
        plan_doc = P.build_plan(sc, topo, topo_doc, None, {}, 1, None, probe=True, stream_port=1)
        self.assertFalse(plan_doc['ok'])
        self.assertTrue(any('RealUe.MaxProcesses' in e for e in plan_doc['errors']), plan_doc['errors'])
        self.assertEqual(plan_doc['workers'][0]['capacity']['real_ue'], {'need': 2, 'processes': 1, 'max': 2})
        # STEP_VOCAB.real — 실단말 행위자 가능 단계 표시(편집기 게이트) · 지표 이름
        self.assertTrue(M.STEP_VOCAB['invite']['real'] and not M.STEP_VOCAB['refer']['real'] and not M.STEP_VOCAB['media_send']['real'])
        for n in ('real_srd_ms', 'real_rtp_loss_pct', 'real_jitter_ms', 'real_mos'):
            self.assertIn(n, M.METRIC_NAMES)


class SipDump(unittest.TestCase):
    def test_recorder_writes_ladder_file_and_list(self):
        """워커 `sip` 레코드 → runs/<id>/sip/<call_id>.log (블록 머리 >>>/<<< + 요청·상태 줄) · 목록 · 스키마."""
        rec = R.Recorder('ut-sip-1')
        msg = {'kind': 'sip', 't': 1.0, 'run_id': 'ut-sip-1', 'worker': 'w1', 'call_id': 'abc@host', 'instance': 7, 'messages': [
            {'t': 1700000000.123, 'dir': 'tx', 'transport': 'udp', 'peer': '10.0.0.5:5060', 'text': 'INVITE sip:b@x SIP/2.0\r\nCall-ID: abc@host\r\n\r\n'},
            {'t': 1700000000.456, 'dir': 'rx', 'transport': 'udp', 'peer': '10.0.0.5:5060', 'text': 'SIP/2.0 403 Forbidden\r\nCall-ID: abc@host\r\n\r\n'}]}
        self.assertEqual(M.validate('worker_stream_sip', msg)[1], [])
        rec.on_record(msg)
        rec.on_record({**msg, 'worker': 'w2'})          # 같은 Call-ID 를 다른 워커가 — 한 파일에 이어 적는다
        dumps = R.run_sip_dumps('ut-sip-1')
        self.assertEqual(len(dumps), 1)
        self.assertEqual(dumps[0]['messages'], 4)
        body = open(os.path.join(S.run_dir('ut-sip-1'), 'sip', dumps[0]['call_id'] + '.log'), encoding='utf-8').read()
        self.assertIn('>>> INVITE sip:b@x SIP/2.0', body)
        self.assertIn('<<< SIP/2.0 403 Forbidden', body)
        self.assertIn('· w2', body)
        self.assertNotIn('\r', body)
        rec.on_record({**msg, 'call_id': '', 'messages': []})   # 빈 레코드는 무시
        self.assertEqual(len(R.run_sip_dumps('ut-sip-1')), 1)


class Hist(unittest.TestCase):
    def test_percentile_upper_bound(self):
        h = R.Hist()
        h.merge({'count': 10, 'sum': 100, 'min': 1, 'max': 90, 'buckets': {'5': 5, '50': 4, '100': 1}})
        self.assertEqual(h.percentile(0.5), 5)
        self.assertEqual(h.percentile(0.95), 90)   # 100 버킷 상한 vs max 90 → max
        self.assertEqual(h.to_dict()['mean'], 10)


class Driver(unittest.TestCase):
    def _run(self, workers, req_extra=None, scenario='VOLTE-CALL-BASIC'):
        topo_doc = _topology(workers)
        # caller/callee 가 같은 8 신원 풀을 나눠 쓰도록 count 를 준 사본 시나리오를 운영자 디렉터리에 둔다
        sc, doc, _ = S.get_scenario(scenario)
        doc = json.loads(json.dumps(doc))
        doc['id'] = 'UT-' + scenario
        doc['roles']['caller']['count'] = 4
        doc['roles']['callee']['count'] = 4
        if req_extra and 'flow' in req_extra:
            doc['flow'] = req_extra.pop('flow')
        import yaml
        with open(os.path.join(S.user_scenarios_dir(), 'ut.yaml'), 'w') as f:
            yaml.safe_dump(doc, f, allow_unicode=True)
        rec, errs = S.save_topology(topo_doc)
        self.assertFalse(errs, errs)
        req = M.RunRequest(scenario_id=doc['id'], topology_id=rec['id'], bindings={'ht': 3}, **(req_extra or {}))
        d = R.RUNS.start(req)
        return d

    def test_single_shot_pass(self):
        w = FakeWorker('w1')
        d = self._run([w], {'instances': 3})
        self.assertEqual(_wait_done(d), 'stopped')
        self.assertEqual(d.verdict, 'pass', d.notes + d.expect_results)
        self.assertEqual(len(w.pools), 1)
        self.assertEqual(w.runs[0]['max_instances'], 3)
        self.assertEqual(w.runs[0]['stream'], f"127.0.0.1:{_CFG['Tester']['WorkerStreamPort']}")
        idx = S.get_run(d.run_id)
        self.assertEqual(idx['verdict'], 'pass')
        self.assertEqual(idx['summary']['attempts'], 3)
        self.assertEqual(idx['summary']['ser_pct'], 100.0)
        rd = S.run_dir(d.run_id)
        self.assertTrue(os.path.isfile(os.path.join(rd, 'run.json')))
        self.assertTrue(os.path.isfile(os.path.join(rd, 'metrics.sqlite')))
        import sqlite3
        n = sqlite3.connect(os.path.join(rd, 'metrics.sqlite')).execute('SELECT COUNT(*) FROM agg').fetchone()[0]
        self.assertEqual(n, 3)
        with open(os.path.join(rd, 'run.json')) as f:
            doc = json.load(f)
        self.assertTrue(all(r['ok'] for r in doc['expect_results']), doc['expect_results'])
        self.assertEqual(doc['timers']['srd_ms']['p95'], 1500)   # 상한 2000 버킷이지만 max 1500 로 잘린다
        from handlers.tester import report_markdown, compare_runs
        md = report_markdown(doc)
        self.assertIn('RFC 6076', md)
        self.assertIn('PASS', md)
        # E 단계 — metrics.sqlite → 초 단위 시계열(콘솔 결과 차트) · 자기 자신과의 비교는 회귀 0
        series = R.run_series(d.run_id)
        self.assertEqual(len(series['t']), 3)
        self.assertEqual(sum(series['counters']['attempts']), 3)
        self.assertEqual(series['gauges']['concurrent_sessions'], [1, 1, 1])
        self.assertEqual([x for x in series['timers']['srd_ms']['p95'] if x is not None], [1500, 1500, 1500])
        cmp_ = compare_runs([d.run_id, d.run_id])
        self.assertEqual(cmp_['regressions'], 0)
        self.assertTrue(any(m['metric'] == 'ser_pct' and m['values'] == [100.0, 100.0] for m in cmp_['metrics']))
        self.assertTrue(cmp_['same_scenario'])

    def test_failure_makes_fail_and_events(self):
        w = FakeWorker('w1', {'fail_one': True})
        d = self._run([w], {'instances': 3})
        self.assertEqual(_wait_done(d), 'stopped')
        self.assertEqual(d.verdict, 'fail')
        idx = S.get_run(d.run_id)
        self.assertEqual(idx['summary']['failed'], 1)
        self.assertEqual(idx['summary']['codes'], '503:1')
        with open(os.path.join(S.run_dir(d.run_id), 'events.jsonl')) as f:
            self.assertEqual(len(f.readlines()), 1)

    def test_two_workers_share(self):
        w1, w2 = FakeWorker('w1'), FakeWorker('w2')
        d = self._run([w1, w2], {'instances': 4})
        self.assertEqual(_wait_done(d), 'stopped')
        self.assertEqual(d.verdict, 'pass', d.notes)
        self.assertEqual(w1.runs[0]['max_instances'] + w2.runs[0]['max_instances'], 4)
        self.assertEqual(sorted(S.get_run(d.run_id)['workers']), ['w1', 'w2'])

    def test_operator_stop_aborts(self):
        w = FakeWorker('w1', {'never_stop': True})
        d = self._run([w], {'instances': 2})
        time.sleep(1.0)
        self.assertTrue(R.RUNS.stop(d.run_id))
        self.assertEqual(_wait_done(d, 40), 'stopped')
        self.assertEqual(d.verdict, 'aborted')
        self.assertTrue(w.stops)

    def test_worker_down_is_error(self):
        dead = FakeWorker('dead')
        dead.srv.shutdown()
        dead.srv.server_close()   # 리슨 소켓까지 닫아야 즉시 거부된다(backlog 에 매달리지 않게)
        d = self._run([dead], {'instances': 1})
        self.assertEqual(_wait_done(d), 'stopped')
        self.assertEqual(d.verdict, 'error')
        self.assertTrue(any('워커 미응답' in n for n in d.notes), d.notes)

    def test_only_one_active(self):
        w = FakeWorker('w1', {'never_stop': True})
        d = self._run([w], {'instances': 1})
        time.sleep(0.5)
        with self.assertRaises(RuntimeError):
            self._run([w], {'instances': 1})
        R.RUNS.stop(d.run_id)
        _wait_done(d, 40)


if __name__ == '__main__':
    unittest.main()
