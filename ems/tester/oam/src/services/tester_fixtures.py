"""시험 픽스처 — 시나리오 `fixtures:` 를 대상의 **운영 프로비저닝 경로**로 적용·확인·복원 (test_instrument.md §4 픽스처).

전화 그룹·픽업 그룹·역할·가입 서비스는 운영 데이터다. 상용에서는 운영자가 콘솔·관제 앱 → CSC 관리 API 로 만들고 CSC 가 단일 쓰기 주체로
DB 에 쓰고 CSP 에 통지한다(dispatch_center.md §8, sip_access_security.md P1). 계측기도 같은 경로만 쓴다 — 대상 oam 노드(게이트웨이)
경유 `/api/v1/phone-groups`·`/api/v1/roles`·`/api/v1/users`, 접속서비스 변종은 CSP 컬렉션 `access_services`(OAM 컬렉션 API, 피어 시드와
같은 방식). DB 직접 쓰기·CSP 내부 통지는 없다.

수명 = run 하나: `resolve()`(컴파일 — 역할 → 계획의 첫 신원, ${pilot} 생성) → `FixtureApplier.apply()`(풀 생성 **전** — 픽업 축은 다음
REGISTER 부터 반영) → `verify()` → run → `revert()`(역순, 이동한 멤버·종전 역할 배정·service_ref 원복, 만든 그룹·역할·서비스 삭제).
같은 키의 잔재(이전 run 중단)는 적용 전에 지우고 노트에 남긴다. id 규약 = `pg-tester-<키>` · `role-tester-<키>` · `tester-svc-<키>`.
"""
from __future__ import annotations

import re
import time
import urllib.parse
from typing import Dict, List, Optional, Tuple

from services.tester_models import Scenario, Topology
from services.tester_target import OamClient, TargetError, resolve_token

_BIND = re.compile(r'^\$\{(\w+)\}$')
SEED_TAG = 'cims-tester'
_KIND_PATH = {'volte': 'call', 'voip': 'voip', 'ptt': 'ptt'}    # 풀 service → CSC /users/{person}/{kind}/{msisdn} 의 kind


class FixtureError(Exception):
    pass


def pg_id(key: str) -> str:
    return f'pg-tester-{key}'


def role_id(key: str) -> str:
    return f'role-tester-{key}'


def svc_name(key: str) -> str:
    return f'tester-svc-{key}'


def default_pilot(member_user: str) -> str:
    """가입 id(E.164 +…)와 겹치지 않는 짧은 내선형 대표번호 — 첫 그룹원 번호 끝 3자리로."""
    digits = re.sub(r'\D', '', member_user or '') or '000'
    return f'7{digits[-3:]}0'


def derive_bindings(scenario: Scenario, role_first: Dict[str, dict], bindings: Dict[str, object]) -> Dict[str, object]:
    """phone_group.pilot 이 ${var} 이고 바인딩에 없으면 만들어 준다(시나리오 to: "${var}" 가 같은 값을 쓴다)."""
    out: Dict[str, object] = {}
    for key, f in scenario.fixtures.items():
        if f.kind != 'phone_group' or not f.pilot:
            continue
        m = _BIND.match(f.pilot)
        if m and m.group(1) not in bindings and m.group(1) not in out:
            first = role_first.get(f.members[0]) or {}
            out[m.group(1)] = default_pilot(str(first.get('user') or ''))
    return out


def resolve(scenario: Scenario, role_first: Dict[str, dict], bindings: Dict[str, object],
            first_group: Optional[str]) -> List[dict]:
    """픽스처 → 적용 계획(역할 이름을 계획의 첫 신원으로, 키를 id 로). 컴파일 시점(플랜 미리보기에도 실림). 비밀 없음."""
    def user_of(role: str) -> str:
        ident = role_first.get(role)
        if not ident or not ident.get('user'):
            raise FixtureError(f'픽스처가 참조한 역할 {role!r} 에 배정된 신원이 없다')
        return str(ident['user'])

    def bind(v: Optional[str]) -> Optional[str]:
        if v is None:
            return None
        m = _BIND.match(v)
        if not m:
            return v
        if m.group(1) not in bindings:
            raise FixtureError(f'픽스처 바인딩 ${{{m.group(1)}}} 값이 없다')
        return str(bindings[m.group(1)])

    out: List[dict] = []
    order = {'access_service': 0, 'phone_group': 1, 'role': 2, 'subscriber': 3}
    for key, f in sorted(scenario.fixtures.items(), key=lambda kv: (order[kv[1].kind], kv[0])):
        if f.kind == 'access_service':
            out.append({'key': key, 'kind': f.kind, 'name': svc_name(key), 'from_user': user_of(f.from_role),
                        'from_role': f.from_role, 'set': dict(f.set)})
        elif f.kind == 'phone_group':
            out.append({'key': key, 'kind': f.kind, 'id': pg_id(key),
                        'members': [{'role': r, 'user': user_of(r)} for r in f.members],
                        'pilot': bind(f.pilot), 'service_ref': f.service_ref, 'alert_mode': f.alert_mode,
                        'no_answer_sec': f.no_answer_sec,
                        'overflow': ({'role': f.overflow, 'user': user_of(f.overflow)} if f.overflow else None)})
        elif f.kind == 'role':
            targets = []
            for t in f.ptt_targets:
                if _BIND.match(t):
                    if not first_group:
                        raise FixtureError(f'fixtures.{key}.ptt_targets=${{group}} — 계획에 그룹 세션의 첫 그룹이 없다')
                    targets.append(first_group)
                else:
                    targets.append(t)
            vis = bind(f.listen_visibility)
            if vis not in ('hidden', 'visible'):
                raise FixtureError(f'fixtures.{key}.listen_visibility={vis!r} — hidden|visible 이어야 한다')
            out.append({'key': key, 'kind': f.kind, 'id': role_id(key),
                        'assign': [{'role': r, 'user': user_of(r)} for r in f.assign],
                        'monitor_call': f.monitor_call, 'monitor_targets': [pg_id(k) for k in f.monitor_targets],
                        'ptt_listen': f.ptt_listen, 'ptt_targets': targets,
                        'listen_visibility': vis, 'history_read': f.history_read})
        elif f.kind == 'subscriber':
            fields = {}
            if f.service_ref is not None:
                fields['service_ref'] = svc_name(f.service_ref) if (f.service_ref in scenario.fixtures) else f.service_ref
            if f.ringback_media is not None:
                fields['ringback_media'] = f.ringback_media
            if f.forward_to is not None:
                fields['forward_id'] = user_of(f.forward_to)   # 착신전환 대상 = 역할의 첫 신원(CSP 가 다이얼 플랜으로 번역)
            if f.forward_busy_to is not None:
                fields['forward_busy_id'] = user_of(f.forward_busy_to)
            if f.forward_no_reply_to is not None:
                fields['forward_no_reply_id'] = user_of(f.forward_no_reply_to)
            if f.no_reply_sec is not None:
                fields['forward_no_reply_sec'] = f.no_reply_sec
            if f.forward_not_logged_in_to is not None:
                fields['forward_not_logged_in_id'] = user_of(f.forward_not_logged_in_to)
            if f.forward_not_reachable_to is not None:
                fields['forward_not_reachable_id'] = user_of(f.forward_not_reachable_to)
            out.append({'key': key, 'kind': f.kind, 'fields': fields, 'service_ref': fields.get('service_ref'),
                        'lines': [{'role': r, 'user': user_of(r)} for r in f.roles]})
    return out


def summarize(resolved: List[dict]) -> List[str]:
    """노트·미리보기 한 줄 요약."""
    rows = []
    for r in resolved:
        if r['kind'] == 'phone_group':
            rows.append(f"{r['id']}: 멤버 {[m['user'] for m in r['members']]}" + (f" 대표번호 {r['pilot']}" if r.get('pilot') else '')
                        + (f" {r['alert_mode']}/{r['no_answer_sec']}s" if r.get('pilot') else '')
                        + (f" overflow {r['overflow']['user']}" if r.get('overflow') else ''))
        elif r['kind'] == 'role':
            rows.append(f"{r['id']}: monitor_call={r['monitor_call']} ptt_listen={r['ptt_listen']}({','.join(r['ptt_targets']) or '-'}) "
                        f"visibility={r['listen_visibility']} → {[a['user'] for a in r['assign']]}")
        elif r['kind'] == 'subscriber':
            rows.append(' '.join(f'{k}={v}' for k, v in r['fields'].items()) + f" ← {[x['user'] for x in r['lines']]}")
        elif r['kind'] == 'access_service':
            rows.append(f"access_service {r['name']} = {r['from_user']} 의 서비스 복제 + {r['set']}")
    return rows


class FixtureApplier:
    """resolve() 결과를 대상에 적용·확인·복원. 실패한 적용은 그 자리에서 되돌린다(부분 적용 금지)."""

    def __init__(self, client: OamClient, resolved: List[dict], csp_dep_id: Optional[int] = None):
        self.client = client
        self.resolved = resolved
        self.csp_dep_id = csp_dep_id
        self.undo: List[Tuple] = []          # (종류, 인자…) 역순 실행
        self.notes: List[str] = []
        self.applied: List[dict] = []
        self.errors: List[str] = []
        self._users: Optional[Dict[str, dict]] = None   # 번호 → {person, kind, service_ref}
        self.reverted: Optional[bool] = None

    # ── 생성 ──
    @classmethod
    def for_run(cls, topology: Topology, resolved: List[dict], requester_token: Optional[str] = None) -> Optional['FixtureApplier']:
        if not resolved:
            return None
        if topology.target.kind != 'cims':
            raise TargetError('픽스처는 대상 kind=cims(CSC 관리 API) 에서만 적용할 수 있다')
        oam = topology.oam_ref()
        if oam is None:
            raise TargetError('픽스처 적용에는 대상 oam 노드(CSC 관리 API 게이트웨이)가 필요하다')
        client = OamClient(oam.url, resolve_token(oam, requester_token))
        dep = None
        if any(r['kind'] == 'access_service' for r in resolved):
            dep = client.find_csp_deployment(oam.csp_deployment_id)
        return cls(client, resolved, dep)

    # ── HTTP ──
    def _call(self, method: str, path: str, body=None, ok=(200, 201)) -> dict:
        st, res = self.client._req(method, path, body)
        if st not in ok:
            raise FixtureError(f'{method} {path} → {st} {res.get("error") if isinstance(res, dict) else res}'
                               + (f' ({res.get("detail")})' if isinstance(res, dict) and res.get('detail') else ''))
        return res if isinstance(res, dict) else {}

    def _exists(self, path: str) -> bool:
        st, _ = self.client._req('GET', path)
        return st == 200

    @staticmethod
    def _q(v: str) -> str:
        return urllib.parse.quote(v, safe='')

    # ── 가입자 → person ──
    def users(self) -> Dict[str, dict]:
        if self._users is None:
            res = self._call('GET', '/api/v1/users')
            m: Dict[str, dict] = {}
            for u in res.get('users') or []:
                for kind in ('call', 'voip', 'ptt'):
                    for sub in u.get(f'{kind}_subscriptions') or []:
                        num = str(sub.get('id') or '')
                        if num:
                            m[num] = {'person': u.get('id'), 'kind': kind, 'service_ref': sub.get('service_ref'),
                                      'ringback_media': sub.get('ringback_media'), 'forward_id': sub.get('forward_id'),
                                      'forward_busy_id': sub.get('forward_busy_id'), 'forward_no_reply_id': sub.get('forward_no_reply_id'),
                                      'forward_no_reply_sec': sub.get('forward_no_reply_sec'),
                                      'forward_not_logged_in_id': sub.get('forward_not_logged_in_id'),
                                      'forward_not_reachable_id': sub.get('forward_not_reachable_id')}
                            m[re.sub(r'\D', '', num)] = m[num]
            self._users = m
        return self._users

    def line(self, user: str) -> dict:
        u = self.users().get(user) or self.users().get(re.sub(r'\D', '', user))
        if not u:
            raise FixtureError(f'가입자 {user} 가 대상 CSC 에 없다')
        return u

    # ── 적용 ──
    def apply(self) -> List[dict]:
        try:
            for r in self.resolved:
                getattr(self, f'_apply_{r["kind"]}')(r)
                self.applied.append(r)
        except (FixtureError, TargetError) as e:
            self.errors.append(str(e))
            self.revert()
            raise FixtureError(f'픽스처 적용 실패({self.errors[0]}) — 되돌림 {"완료" if self.reverted else "실패: " + "; ".join(self.errors[1:])}')
        time.sleep(1.0)   # CSC → CSP 통지(PHONE_GROUP_CHANGED·ROLE_CHANGED·USER_CHANGED) 반영 여유
        return self.applied

    def _apply_access_service(self, r: dict) -> None:
        base_ref = self.line(r['from_user']).get('service_ref')
        if not base_ref:
            raise FixtureError(f"{r['from_user']} 의 service_ref 가 비어 있어 복제 원본을 정할 수 없다")
        recs = self.client.get_collection(self.csp_dep_id, 'access_services')
        base = next((x for x in recs if x.get('name') == base_ref), None)
        if base is None:
            raise FixtureError(f'대상 CSP access_services 에 {base_ref!r} 레코드가 없다')
        new = {k: v for k, v in base.items() if k != 'id'}
        new['name'] = r['name']
        new['tags'] = sorted(set(list(new.get('tags') or []) + [SEED_TAG]))
        new.update(r['set'])
        merged = [x for x in recs if x.get('name') != r['name']] + [new]
        self.client.put_collection(self.csp_dep_id, 'access_services', merged, signal=True)
        self.undo.append(('access_service', r['name']))
        time.sleep(1.0)   # SIGUSR1 reload 여유 — 뒤의 subscriber 픽스처가 이 서비스명을 쓴다

    def _apply_phone_group(self, r: dict) -> None:
        path = f"/api/v1/phone-groups/{self._q(r['id'])}"
        if self._exists(path):
            self._call('DELETE', path)
            self.notes.append(f"픽스처 잔재 {r['id']} 삭제(이전 run 미복원)")
        body = {'id': r['id'], 'name': f"cims-tester {r['key']}", 'alert_mode': r['alert_mode'],
                'no_answer_sec': r['no_answer_sec'], 'busy_members': 'skip'}
        if r.get('pilot'):
            body['pilot_id'] = r['pilot']
            body['service_ref'] = r.get('service_ref') or self.line(r['members'][0]['user']).get('service_ref')
        if r.get('overflow'):
            body['overflow_target'] = r['overflow']['user']
        self._call('POST', '/api/v1/phone-groups', body)
        self.undo.append(('phone_group', r['id'], []))
        moved = self.undo[-1][2]
        for i, m in enumerate(r['members']):
            res = self._call('POST', f'{path}/members', {'user_id': m['user'], 'alert_order': i})
            if res.get('moved_from'):
                moved.append((m['user'], res['moved_from']))

    def _apply_role(self, r: dict) -> None:
        path = f"/api/v1/roles/{self._q(r['id'])}"
        if self._exists(path):
            for a in (self._call('GET', f'{path}/assignments').get('assignments') or []):
                self._call('DELETE', f"{path}/assignments/{a['principal_type']}/{self._q(str(a['principal_id']))}", ok=(200, 404))
            self._call('DELETE', path)
            self.notes.append(f"픽스처 잔재 {r['id']} 삭제(이전 run 미복원)")
        self._call('POST', '/api/v1/roles', {'id': r['id'], 'name': f"cims-tester {r['key']}", 'monitor_call': r['monitor_call'],
                                             'ptt_listen': r['ptt_listen'], 'listen_visibility': r['listen_visibility'],
                                             'history_read': r['history_read']})
        self.undo.append(('role', r['id'], []))
        moved = self.undo[-1][2]
        if r['monitor_targets']:
            self._call('PUT', f'{path}/monitor-targets', {'phone_group_ids': r['monitor_targets']})
        if r['ptt_targets']:
            self._call('PUT', f'{path}/ptt-targets', {'ptt_group_ids': r['ptt_targets']})
        for a in r['assign']:
            person = self.line(a['user'])['person']
            res = self._call('PUT', f'{path}/assignments', {'principal_type': 'user', 'principal_id': person})
            moved.append((person, res.get('moved_from')))

    def _apply_subscriber(self, r: dict) -> None:
        for ln in r['lines']:
            u = self.line(ln['user'])
            path = f"/api/v1/users/{u['person']}/{u['kind']}/{self._q(ln['user'])}"
            self._call('PUT', path, dict(r['fields']))
            # 복원값 = 바꾼 필드의 종전 값(없던 값은 빈 문자열 — CSC 가 NULL/프로파일 기본으로 되돌린다)
            self.undo.append(('subscriber', path, {k: (u.get(k) if u.get(k) is not None else (0 if k == 'forward_no_reply_sec' else '')) for k in r['fields']}))

    # ── 확인 ──
    def verify(self) -> List[str]:
        """적용 결과를 읽어 대조 — 어긋나면 사유 목록(비면 정상)."""
        bad = []
        for r in self.applied:
            if r['kind'] == 'phone_group':
                got = {m.get('user_id') for m in (self._call('GET', f"/api/v1/phone-groups/{self._q(r['id'])}/members").get('members') or [])}
                want = {m['user'] for m in r['members']}
                if got != want:
                    bad.append(f"{r['id']} 멤버 {sorted(got)} ≠ {sorted(want)}")
            elif r['kind'] == 'role':
                got = {str(a.get('principal_id')) for a in (self._call('GET', f"/api/v1/roles/{self._q(r['id'])}/assignments").get('assignments') or [])}
                want = {str(self.line(a['user'])['person']) for a in r['assign']}
                if got != want:
                    bad.append(f"{r['id']} 배정 {sorted(got)} ≠ {sorted(want)}")
        return bad

    # ── 복원 ──
    def revert(self) -> List[str]:
        errs: List[str] = []
        for entry in reversed(self.undo):
            try:
                kind = entry[0]
                if kind == 'subscriber':
                    _, path, orig = entry
                    self._call('PUT', path, dict(orig))
                elif kind == 'role':
                    _, rid, moved = entry
                    path = f'/api/v1/roles/{self._q(rid)}'
                    for person, prior in moved:
                        self._call('DELETE', f'{path}/assignments/user/{self._q(str(person))}', ok=(200, 404))
                        if prior:
                            self._call('PUT', f'/api/v1/roles/{self._q(prior)}/assignments', {'principal_type': 'user', 'principal_id': person})
                    self._call('DELETE', path, ok=(200, 404))
                elif kind == 'phone_group':
                    _, gid, moved = entry
                    self._call('DELETE', f'/api/v1/phone-groups/{self._q(gid)}', ok=(200, 404))
                    for user, prior in moved:
                        self._call('POST', f'/api/v1/phone-groups/{self._q(prior)}/members', {'user_id': user})
                elif kind == 'access_service':
                    _, name = entry
                    recs = self.client.get_collection(self.csp_dep_id, 'access_services')
                    self.client.put_collection(self.csp_dep_id, 'access_services', [x for x in recs if x.get('name') != name], signal=True)
            except (FixtureError, TargetError) as e:
                errs.append(str(e))
        self.undo = []
        self.reverted = not errs
        self.errors.extend(errs)
        return errs

    def report(self) -> dict:
        return {'applied': [{k: v for k, v in r.items()} for r in self.applied], 'reverted': self.reverted,
                'notes': list(self.notes), 'errors': list(self.errors)}
