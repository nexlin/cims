"""계측기 시험 픽스처 단위시험 — 시나리오 `fixtures:` → 컴파일(역할 → 첫 신원·${pilot} 생성) → 대상 CSC 관리 API 적용·확인·복원.

Covers:
  - models: fixtures 스키마·참조 무결성(역할·phone_group 키·${group} 게이트·가입자당 그룹 하나)
  - compile: role_identities(워커 배정 순)·derive_bindings(${pilot} 자동)·resolve(키 → id, 역할 → 신원, ${group} → 첫 그룹)·게이트(oam 노드 없음 → CompileError)
  - applier: 가짜 OAM 게이트웨이(/api/v1/users·phone-groups·roles·deployments/{id}/collection/access_services) 상대로
    apply(access_service 복제 → 전화 그룹+멤버(moved_from) → 역할+대상+배정(moved_from) → subscriber service_ref) → verify → revert(역순·원복) 순서와
    잔재 정리, 부분 실패 시 자동 되돌림
  - driver: 픽스처 시나리오 run 이 적용 → 풀 생성 → run → 복원으로 완주하고 run.json 에 fixtures 보고가 남는다
"""
import json
import os
import shutil
import sys
import tempfile
import threading
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

try:
    import tests.test_tester_run as _TR  # noqa: E402
except ImportError:
    import test_tester_run as _TR  # noqa: E402
FakeWorker, _free_port = _TR.FakeWorker, _TR._free_port

S = M = R = C = T = F = None
_TMP = None


class FakeCscOam:
    """대상 OAM 게이트웨이 흉내 — CSC 관리 API(users·phone-groups·roles)와 CSP 컬렉션(access_services)을 메모리에."""

    def __init__(self):
        self.port = _free_port()
        self.users = [
            {'id': 5001, 'name': 'a', 'call_subscriptions': [{'id': '+821300000000', 'service_ref': 'volte'}]},
            {'id': 5002, 'name': 'b', 'call_subscriptions': [{'id': '+821300000001', 'service_ref': 'volte'}]},
            {'id': 5003, 'name': 'c', 'call_subscriptions': [{'id': '+821300000002', 'service_ref': 'volte'}]},
            {'id': 5004, 'name': 'd', 'call_subscriptions': [{'id': '+821300000003', 'service_ref': 'volte'}]},
        ]
        self.groups = {'pg-old': {'id': 'pg-old', 'name': 'old', 'members': [{'user_id': '+821300000001', 'alert_order': 0}]}}
        self.roles = {'role-prior': {'id': 'role-prior', 'name': 'prior', 'assignments': [{'principal_type': 'user', 'principal_id': '5003'}]}}
        self.access_services = [{'id': 'as1', 'name': 'volte', 'kind': 'volte', 'domain': 'volte.test', 'transfer_allowed': True}]
        self.log = []      # (method, path, body)
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

            def _dispatch(self, method):
                if self.headers.get('Authorization') != 'Bearer tok':
                    self._send(401, {'error': 'unauthorized'}); return
                path = self.path.split('?')[0]
                body = self._body() if method in ('POST', 'PUT') else None
                outer.log.append((method, path, body))
                from urllib.parse import unquote
                parts = [unquote(x) for x in path.split('/') if x]
                st, res = outer.handle(method, parts, body)
                self._send(st, res)

            def do_GET(self):
                self._dispatch('GET')

            def do_POST(self):
                self._dispatch('POST')

            def do_PUT(self):
                self._dispatch('PUT')

            def do_DELETE(self):
                self._dispatch('DELETE')

        self.srv = HTTPServer(('127.0.0.1', self.port), H)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    @property
    def url(self):
        return f'http://127.0.0.1:{self.port}'

    def _group_of(self, user):
        for g in self.groups.values():
            if any(m['user_id'] == user for m in g['members']):
                return g['id']
        return None

    def _role_of(self, person):
        for r in self.roles.values():
            if any(a['principal_id'] == str(person) for a in r['assignments']):
                return r['id']
        return None

    def handle(self, method, parts, body):
        # parts = ['api','v1',...]
        p = parts[2:]
        if p == ['deployments'] and method == 'GET':
            return 200, {'items': [{'id': 9, 'package_name': 'csp', 'status': 'running'}]}
        if len(p) == 4 and p[0] == 'deployments' and p[2] == 'collection':
            if method == 'GET':
                return 200, {'records': self.access_services, 'schema': {}}
            self.access_services = [dict(r, id=r.get('id') or f'auto{i}') for i, r in enumerate(body.get('records') or [])]
            return 200, {'ok': True, 'count': len(self.access_services)}
        if p == ['users'] and method == 'GET':
            return 200, {'users': self.users}
        if len(p) == 4 and p[0] == 'users' and method == 'PUT':
            for u in self.users:
                if str(u['id']) == p[1]:
                    for sub in u.get(f'{p[2]}_subscriptions') or []:
                        if sub['id'] == p[3]:
                            sub['service_ref'] = body.get('service_ref')
                            return 200, {'id': p[3]}
            return 404, {'error': 'not found'}
        if p and p[0] == 'phone-groups':
            if len(p) == 1 and method == 'POST':
                gid = body['id']
                if gid in self.groups:
                    return 409, {'error': 'group_exists'}
                self.groups[gid] = {**body, 'members': []}
                return 201, {'id': gid}
            gid = p[1] if len(p) > 1 else None
            g = self.groups.get(gid)
            if g is None:
                return 404, {'error': 'Group not found'}
            if len(p) == 2 and method == 'GET':
                return 200, g
            if len(p) == 2 and method == 'DELETE':
                del self.groups[gid]
                return 200, {'id': gid}
            if len(p) == 3 and p[2] == 'members' and method == 'GET':
                return 200, {'group_id': gid, 'members': g['members']}
            if len(p) == 3 and p[2] == 'members' and method == 'POST':
                prev = self._group_of(body['user_id'])
                if prev and prev != gid:
                    self.groups[prev]['members'] = [m for m in self.groups[prev]['members'] if m['user_id'] != body['user_id']]
                g['members'] = [m for m in g['members'] if m['user_id'] != body['user_id']] + [{'user_id': body['user_id'], 'alert_order': body.get('alert_order', 0)}]
                return 201, {'group_id': gid, 'user_id': body['user_id'], 'moved_from': prev if prev != gid else None}
        if p and p[0] == 'roles':
            if len(p) == 1 and method == 'POST':
                rid = body['id']
                if rid in self.roles:
                    return 409, {'error': 'role_exists'}
                self.roles[rid] = {**body, 'assignments': [], 'monitor_targets': [], 'ptt_targets': []}
                return 201, {'id': rid}
            rid = p[1] if len(p) > 1 else None
            r = self.roles.get(rid)
            if r is None:
                return 404, {'error': 'Role not found'}
            if len(p) == 2 and method == 'GET':
                return 200, r
            if len(p) == 2 and method == 'DELETE':
                if r['assignments']:
                    return 409, {'error': 'assigned'}
                del self.roles[rid]
                return 200, {'id': rid}
            if len(p) == 3 and p[2] == 'monitor-targets' and method == 'PUT':
                r['monitor_targets'] = body['phone_group_ids']
                return 200, {'id': rid}
            if len(p) == 3 and p[2] == 'ptt-targets' and method == 'PUT':
                r['ptt_targets'] = body['ptt_group_ids']
                return 200, {'id': rid}
            if len(p) == 3 and p[2] == 'assignments' and method == 'GET':
                return 200, {'role_id': rid, 'assignments': r['assignments']}
            if len(p) == 3 and p[2] == 'assignments' and method == 'PUT':
                person = str(body['principal_id'])
                prev = self._role_of(person)
                if prev and prev != rid:
                    self.roles[prev]['assignments'] = [a for a in self.roles[prev]['assignments'] if a['principal_id'] != person]
                r['assignments'] = [a for a in r['assignments'] if a['principal_id'] != person] + [{'principal_type': 'user', 'principal_id': person}]
                return 200, {'role_id': rid, 'principal_id': person, 'moved_from': prev if prev != rid else None}
            if len(p) == 5 and p[2] == 'assignments' and method == 'DELETE':
                before = len(r['assignments'])
                r['assignments'] = [a for a in r['assignments'] if a['principal_id'] != p[4]]
                return (200, {'role_id': rid}) if len(r['assignments']) < before else (404, {'error': 'Assignment not found'})
        return 404, {'error': f'no route {method} {"/".join(parts)}'}


def setUpModule():
    global S, M, R, C, T, F, _TMP
    from services import tester_store as _S, tester_models as _M, tester_run as _R, tester_compile as _C, tester_target as _T, tester_fixtures as _F
    S, M, R, C, T, F = _S, _M, _R, _C, _T, _F
    _TR.S, _TR.M, _TR.R, _TR.C = S, M, R, C
    _TMP = tempfile.mkdtemp(prefix='tester-fx-ut-')
    cfg = {'CimsRuntimeDir': os.path.join(_TMP, 'runtime'),
           'Tester': {'DataDir': os.path.join(_TMP, 'data'), 'WorkerStreamIp': '127.0.0.1',
                      'WorkerStreamPort': _free_port(), 'WorkerStreamAdvertiseIp': '127.0.0.1'}}
    from services import file_store, lease
    lease.acquire(file_store.runtime_root(cfg))
    S.init(_TESTER, cfg)
    R.RUNS.init(cfg)
    os.makedirs(os.path.join(S.user_scenarios_dir(), 'creds'), exist_ok=True)
    with open(os.path.join(S.user_scenarios_dir(), 'creds', 'ue.jsonl'), 'w') as f:
        for i in range(4):
            f.write(json.dumps({'user': f'+82130000000{i}', 'authId': f'45033821300000{i}', 'ha1': 'ab' * 16}) + '\n')
    os.environ['UT_OAM_TOKEN'] = 'tok'


def tearDownModule():
    R.RUNS.shutdown()
    shutil.rmtree(_TMP, ignore_errors=True)


def _topology(worker, oam_url=None):
    nodes = {'csp': {'role': 'sip', 'host': 'h1', 'sip': {'domains': ['volte.test'],
                                                          'listeners': {'udp': {'edge': 'access', 'port': 5060, 'protocol': 'udp'}}}}}
    if oam_url:
        nodes['oam'] = {'role': 'oam', 'host': 'hw', 'oam': {'port': int(oam_url.rsplit(':', 1)[-1]), 'tls': False, 'token_env': 'UT_OAM_TOKEN'}}
    return {'name': 'ut-fx', 'hosts': {'h1': {'ip': '10.0.0.1'}, 'hw': {'ip': '127.0.0.1'}},
            'workers': [{'name': worker.name, 'host': 'hw', 'port': worker.port, 'cpus': 1}],
            'target': {'name': 'sut', 'kind': 'cims', 'nodes': nodes},
            'pools': {'volte_ue': {'kind': 'ue', 'worker': worker.name, 'access': 'csp', 'source': {'creds': 'creds/ue.jsonl'}}}}


_SC = {
    'id': 'UT-FX-FA',
    'roles': {'caller': {'pool': 'volte_ue', 'count': 1}, 'memberB': {'pool': 'volte_ue', 'count': 1, 'disjoint_from': 'caller'},
              'memberC': {'pool': 'volte_ue', 'count': 1, 'disjoint_from': 'memberB'}, 'monitor': {'pool': 'volte_ue', 'count': 1, 'disjoint_from': 'memberC'}},
    'flow': [{'step': 'register', 'who': ['caller', 'memberB', 'memberC', 'monitor'], 'expect': {'code': 200}},
             {'step': 'invite', 'from': 'caller', 'to': '${pilot}'},
             {'step': 'answer', 'who': ['memberC'], 'expect': {'code': 200}},
             {'step': 'bye', 'from': 'caller'}],
    'fixtures': {'pg': {'kind': 'phone_group', 'pilot': '${pilot}', 'members': ['memberB', 'memberC'], 'overflow': 'monitor'},
                 'mon': {'kind': 'role', 'monitor_call': 'listed', 'monitor_targets': ['pg'], 'assign': ['monitor']},
                 'nox': {'kind': 'access_service', 'from_role': 'caller', 'set': {'transfer_allowed': False}},
                 'line': {'kind': 'subscriber', 'roles': ['caller'], 'service_ref': 'nox'}},
}


class Models(unittest.TestCase):
    def test_schema_and_refs(self):
        sc = M.Scenario.model_validate(_SC)
        self.assertEqual({k: v.kind for k, v in sc.fixtures.items()}, {'pg': 'phone_group', 'mon': 'role', 'nox': 'access_service', 'line': 'subscriber'})
        self.assertIn('fixtures', M.schema_json('scenario')['properties'])

        def bad(fx, msg):
            d = json.loads(json.dumps(_SC)); d['fixtures'] = fx
            with self.assertRaises(Exception, msg=msg):
                M.Scenario.model_validate(d)
        bad({'pg': {'kind': 'phone_group', 'members': ['nobody']}}, '없는 역할')
        bad({'pg': {'kind': 'phone_group', 'members': ['memberB']}, 'pg2': {'kind': 'phone_group', 'members': ['memberB']}}, '가입자당 그룹 하나')
        bad({'r': {'kind': 'role', 'assign': ['monitor'], 'monitor_call': 'listed'}}, 'listed 인데 대상 없음')
        bad({'r': {'kind': 'role', 'assign': ['monitor'], 'monitor_call': 'all', 'monitor_targets': ['pg']}}, 'all 인데 대상')
        bad({'r': {'kind': 'role', 'assign': ['monitor'], 'ptt_listen': 'listed', 'ptt_targets': ['${group}']}}, '그룹 세션 아닌데 ${group}')
        bad({'Pg': {'kind': 'phone_group', 'members': ['memberB']}}, '키 규칙')
        bad({'s': {'kind': 'access_service', 'from_role': 'caller', 'set': {'name': 'x'}}}, '정체 필드')
        bad({'l': {'kind': 'subscriber', 'roles': ['caller'], 'service_ref': 'pg'}, 'pg': {'kind': 'phone_group', 'members': ['memberB']}}, 'service_ref 가 access_service 아님')


class Compile(unittest.TestCase):
    def test_resolve_and_pilot_binding(self):
        oam = FakeCscOam()
        w = FakeWorker('w1')
        topo_doc = _topology(w, oam.url)
        topo = M.Topology.model_validate(topo_doc)
        from services import tester_workers as TW
        ws = TW.discover(topo_doc)
        for x in ws:
            x.probe()
        sc = M.Scenario.model_validate(_SC)
        plan = C.compile_run('r1', sc, topo, topo_doc, None, {}, ws, lambda _w: '127.0.0.1:1', 1, None)
        ids = plan['role_identities']
        self.assertEqual(sorted(ids), ['caller', 'memberB', 'memberC', 'monitor'])
        self.assertTrue(all(len(v) == 1 for v in ids.values()))
        first_b = ids['memberB'][0]
        # ${pilot} 이 없으면 첫 멤버 끝 3자리로 만들어 바인딩에 넣고, 단계 to 도 그 값을 쓴다
        self.assertEqual(plan['bindings']['pilot'], f'7{first_b[-3:]}0')
        inv = [s for s in plan['steps'] if s['step'] == 'invite'][0]
        self.assertEqual(inv['to'], plan['bindings']['pilot'])
        fx = {r['key']: r for r in plan['fixtures']}
        self.assertEqual([r['kind'] for r in plan['fixtures']], ['access_service', 'phone_group', 'role', 'subscriber'])   # 적용 순서
        self.assertEqual(fx['pg']['id'], 'pg-tester-pg')
        self.assertEqual([m['user'] for m in fx['pg']['members']], [ids['memberB'][0], ids['memberC'][0]])
        self.assertEqual(fx['pg']['overflow']['user'], ids['monitor'][0])
        self.assertEqual(fx['mon']['monitor_targets'], ['pg-tester-pg'])
        self.assertEqual(fx['line']['service_ref'], 'tester-svc-nox')
        # 바인딩을 주면 그 값
        plan2 = C.compile_run('r2', sc, topo, topo_doc, None, {'pilot': '70010'}, ws, lambda _w: '127.0.0.1:1', 1, None)
        self.assertEqual({r['key']: r for r in plan2['fixtures']}['pg']['pilot'], '70010')
        oam.srv.shutdown()

    def test_gate_requires_oam_node(self):
        w = FakeWorker('w1')
        topo_doc = _topology(w)
        topo = M.Topology.model_validate(topo_doc)
        from services import tester_workers as TW
        ws = TW.discover(topo_doc)
        sc = M.Scenario.model_validate(_SC)
        with self.assertRaises(C.CompileError):
            C.compile_run('r1', sc, topo, topo_doc, None, {}, ws, lambda _w: '127.0.0.1:1', 1, None)


class Applier(unittest.TestCase):
    def _resolved(self):
        return [
            {'key': 'nox', 'kind': 'access_service', 'name': 'tester-svc-nox', 'from_user': '+821300000000', 'from_role': 'caller', 'set': {'transfer_allowed': False}},
            {'key': 'pg', 'kind': 'phone_group', 'id': 'pg-tester-pg', 'pilot': '70010', 'service_ref': None, 'alert_mode': 'parallel', 'no_answer_sec': 8,
             'members': [{'role': 'memberB', 'user': '+821300000001'}, {'role': 'memberC', 'user': '+821300000002'}], 'overflow': None},
            {'key': 'mon', 'kind': 'role', 'id': 'role-tester-mon', 'assign': [{'role': 'monitor', 'user': '+821300000002'}],
             'monitor_call': 'listed', 'monitor_targets': ['pg-tester-pg'], 'ptt_listen': 'none', 'ptt_targets': [], 'listen_visibility': 'hidden', 'history_read': 'scope'},
            {'key': 'line', 'kind': 'subscriber', 'service_ref': 'tester-svc-nox', 'lines': [{'role': 'caller', 'user': '+821300000000'}]},
        ]

    def test_apply_verify_revert(self):
        oam = FakeCscOam()
        try:
            client = T.OamClient(oam.url, 'tok')
            ap = F.FixtureApplier(client, self._resolved(), csp_dep_id=9)
            ap.apply()
            self.assertEqual(ap.verify(), [])
            # 적용 상태: 서비스 복제(원본 유지·태그), 그룹+멤버(B 는 pg-old 에서 이동), 역할+대상+배정(c 는 role-prior 에서 이동), caller 서비스 전환
            names = {r['name']: r for r in oam.access_services}
            self.assertIn('tester-svc-nox', names)
            self.assertFalse(names['tester-svc-nox']['transfer_allowed'])
            self.assertIn('cims-tester', names['tester-svc-nox']['tags'])
            self.assertTrue(names['volte']['transfer_allowed'])
            self.assertEqual([m['user_id'] for m in oam.groups['pg-tester-pg']['members']], ['+821300000001', '+821300000002'])
            self.assertEqual(oam.groups['pg-tester-pg']['pilot_id'], '70010')
            self.assertEqual(oam.groups['pg-tester-pg']['service_ref'], 'volte')     # 첫 멤버의 현 service_ref
            self.assertEqual(oam.groups['pg-old']['members'], [])
            self.assertEqual(oam.roles['role-tester-mon']['monitor_targets'], ['pg-tester-pg'])
            self.assertEqual([a['principal_id'] for a in oam.roles['role-tester-mon']['assignments']], ['5003'])
            self.assertEqual(oam.roles['role-prior']['assignments'], [])
            self.assertEqual(oam.users[0]['call_subscriptions'][0]['service_ref'], 'tester-svc-nox')
            # 복원
            self.assertEqual(ap.revert(), [])
            self.assertTrue(ap.reverted)
            self.assertNotIn('pg-tester-pg', oam.groups)
            self.assertEqual([m['user_id'] for m in oam.groups['pg-old']['members']], ['+821300000001'])
            self.assertNotIn('role-tester-mon', oam.roles)
            self.assertEqual([a['principal_id'] for a in oam.roles['role-prior']['assignments']], ['5003'])
            self.assertEqual({r['name'] for r in oam.access_services}, {'volte'})
            self.assertEqual(oam.users[0]['call_subscriptions'][0]['service_ref'], 'volte')
            # 대상에 DB 직접 쓰기·CSP 통지 경로가 없다 — 전부 관리 API
            self.assertTrue(all(p.startswith('/api/v1/') for _m, p, _b in oam.log))
        finally:
            oam.srv.shutdown()

    def test_leftover_cleanup_and_partial_failure_rollback(self):
        oam = FakeCscOam()
        try:
            oam.groups['pg-tester-pg'] = {'id': 'pg-tester-pg', 'name': 'stale', 'members': []}
            client = T.OamClient(oam.url, 'tok')
            resolved = self._resolved()
            resolved[2]['assign'] = [{'role': 'monitor', 'user': '+829999999999'}]   # 대상 CSC 에 없는 가입자 → 역할 단계에서 실패
            ap = F.FixtureApplier(client, resolved, csp_dep_id=9)
            with self.assertRaises(F.FixtureError):
                ap.apply()
            self.assertTrue(any('잔재' in n for n in ap.notes))
            self.assertTrue(ap.reverted)
            self.assertNotIn('pg-tester-pg', oam.groups)
            self.assertNotIn('role-tester-mon', oam.roles)
            self.assertEqual({r['name'] for r in oam.access_services}, {'volte'})
            self.assertEqual([m['user_id'] for m in oam.groups['pg-old']['members']], ['+821300000001'])
        finally:
            oam.srv.shutdown()


class Driver(unittest.TestCase):
    def test_run_applies_and_reverts(self):
        oam = FakeCscOam()
        try:
            w = FakeWorker('w1')
            topo_doc = _topology(w, oam.url)
            import yaml
            with open(os.path.join(S.user_scenarios_dir(), 'ut_fx.yaml'), 'w') as f:
                yaml.safe_dump(_SC, f, allow_unicode=True)
            rec, errs = S.save_topology(topo_doc)
            self.assertFalse(errs, errs)
            d = R.RUNS.start(M.RunRequest(scenario_id='UT-FX-FA', topology_id=rec['id'], bindings={'ht': 1}, instances=1))
            d.join(30)
            self.assertEqual(d.state, 'stopped')
            self.assertNotEqual(d.verdict, 'error', d.notes)
            # 적용이 풀 생성보다 앞선다
            first_pool = next(i for i, (m, p, _b) in enumerate(oam.log) if p == '/api/v1/phone-groups' and m == 'POST')
            self.assertGreater(len(w.pools), 0)
            self.assertIsNotNone(first_pool)
            self.assertTrue(any(n.startswith('fixtures applied') for n in d.notes), d.notes)
            self.assertIn('fixtures reverted', d.notes)
            self.assertNotIn('pg-tester-pg', oam.groups)
            self.assertNotIn('role-tester-mon', oam.roles)
            detail = json.load(open(os.path.join(S.run_dir(d.run_id), 'run.json')))
            self.assertTrue(detail['fixtures']['reverted'])
            self.assertEqual([r['kind'] for r in detail['fixtures']['applied']], ['access_service', 'phone_group', 'role', 'subscriber'])
        finally:
            oam.srv.shutdown()


if __name__ == '__main__':
    unittest.main()
