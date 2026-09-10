"""
관제 앱(가입자=관제사, PKCE provisioning 토큰) 주체의 조직/구성원/번호·PTT 그룹 관리 —
docs/design/features/dispatch_center.md §3.4 · android_ue_provisioning.md §3-3.

인가 축 = 관제 그룹 속성 `dispatch_groups.directory_admin`(none|own|all, own = 그룹 org_id 조직과 그 하위).
감청 범위(monitor_scope)와 같은 결 — 서버가 enum 을 해석해 **범위 안의 조직 코드 집합**으로 게이트하고 앱은 결과만
받는다. 부여는 콘솔 manager(이 파일은 판정만). 3GPP 규격 밖(가입자 프로비저닝은 MC 서비스 제공자 정책)이라 CIMS
확장이며, 콘솔 관리 API(`/api/v1/organizations`·`/api/v1/users`, 콘솔 토큰)와 **같은 쓰기 코드**(handlers.admin/org)를
호출한다 — 정책·검증(H(A1) 결박, pickup_group 파생 409, AKA)이 두 평면에서 갈라지지 않게. 토큰 realm 은 섞지 않는다.

  GET    /provisioning/directory/admin                      관리 화면 한 벌: scope·services·orgs(범위 안)·members(범위 안)
  POST   /provisioning/directory/orgs                       {code,name,parent,sort}         (parent 는 범위 안 조직 코드)
  PUT    /provisioning/directory/orgs/{code}                {name?,parent?,sort?}
  DELETE /provisioning/directory/orgs/{code}                하위 조직·구성원이 남아 있으면 409 not_empty
  POST   /provisioning/directory/members                    {name,org,title?,loginId?,password?,volte?{..},ptt?{..}}
  PUT    /provisioning/directory/members/{userId}           {name?,org?,title?,loginId?,password?}
  DELETE /provisioning/directory/members/{userId}
  PUT    /provisioning/directory/members/{userId}/volte|ptt {msisdn,imsi?,serviceRef?,sipTransport?,password?} 개설/변경
  DELETE /provisioning/directory/members/{userId}/volte|ptt
  PUT    /provisioning/directory/members/{userId}/ptt/profile {allowCreateGroup?,allowAmbientListening?,...}
  GET    /provisioning/directory/groups                     범위 안 PTT 그룹 목록(관리용 — GMS 멤버 목록과 별개)

오류: 401 invalid_token · 403 insufficient_scope / no_directory_admin(관리 범위 없음) / out_of_scope(범위 밖 조직·구성원) ·
      400 schema_not_migrated(directory_admin 컬럼 미적용) · 그 외는 admin/org 핸들러의 코드 그대로.
감사: 모든 쓰기는 E-AUD-006 config_change(actor=관제사 msisdn, entity=organization|user|subscription|ptt_profile).
"""

import hashlib
import json
import re
from urllib.parse import urlparse, unquote
from pathlib import PurePath
from typing import Optional, Tuple

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult
from handlers import admin as _admin
from handlers import org as _org
from handlers import dispatch as _dispatch
from services import mcptt as _m
from services.mcptt import logger

_BASE = '/provisioning/directory'
_KINDS = {'volte': 'call', 'ptt': 'ptt'}        # 와이어 kind → admin.py svc 키
_TRANSPORTS = ('UDP', 'TCP', 'TLS')
_PROFILE_KEYS = {                               # 와이어 camelCase → ptt_user_profile 컬럼
    'allowEmergencyCall': 'allow_emergency_call', 'allowEmergencyAlert': 'allow_emergency_alert',
    'allowAdhocCall': 'allow_adhoc_call', 'allowEmergencyPrivateCall': 'allow_emergency_private_call',
    'allowAmbientListening': 'allow_ambient_listening', 'allowCreateGroup': 'allow_create_group',
}


def _json(status: int, body, headers=None) -> HandlerResult:
    return HandlerResult(status=status, body=body, media_type='application/json', headers=headers or {})


def _get_db(config: dict):
    return _admin._get_db(config)


def _path_parts(full_path: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(_BASE))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


# ── 인증·신원 ─────────────────────────────────────────────────────────────────

def _auth(args: HandlerArgs) -> Tuple[Optional[dict], Optional[HandlerResult]]:
    """/provisioning/* 와 같은 게이트 — PKCE 토큰 + provisioning scope(빈 scope=레거시 허용)."""
    token = _m.extract_token(args.headers.get('authorization') or args.headers.get('Authorization'))
    if not token:
        return None, _json(401, {"error": "invalid_token"})
    sc = token.get('scope') or []
    if isinstance(sc, str):
        sc = sc.split()
    if sc and _m.SCOPE_PROVISIONING not in sc:
        return None, _json(403, {"error": "insufficient_scope", "required": _m.SCOPE_PROVISIONING})
    return token, None


def caller_identity(cur, token: dict) -> Tuple[str, Optional[int]]:
    """토큰 → (msisdn, users.id). /provisioning/history 와 같은 해석(가입 id → user_id), 없으면 LOGIN_ACCOUNTS."""
    msisdn = _m._msisdn_from_id(token.get('mcptt_id') or token.get('sub') or '')
    for t in ('volte_subscriptions', 'ptt_subscriptions'):
        cur.execute(f"SELECT user_id FROM {t} WHERE id=%s", (msisdn,))
        r = cur.fetchone()
        if r:
            uid = r['user_id'] if isinstance(r, dict) else r[0]
            return msisdn, int(uid)
    return msisdn, _m._token_user_id(token)


# ── 범위 ──────────────────────────────────────────────────────────────────────

def _org_rows(cur) -> list:
    cur.execute("SELECT id, code, name, parent_id, sort_order FROM organizations")
    rows = cur.fetchall()
    out = []
    for r in rows:
        if isinstance(r, dict):
            out.append((r['id'], r['code'] or '', r['name'] or '', r['parent_id'], r['sort_order'] or 0))
        else:
            out.append((r[0], r[1] or '', r[2] or '', r[3], r[4] or 0))
    return out


def org_subtree_codes(org_rows: list, root_id) -> set:
    """root_id 조직과 그 하위 전체의 code 집합(사이클 방어)."""
    children = {}
    code_of = {}
    for oid, code, _name, pid, _so in org_rows:
        code_of[oid] = code
        children.setdefault(pid, []).append(oid)
    out, stack, seen = set(), [root_id], set()
    while stack:
        oid = stack.pop()
        if oid in seen or oid not in code_of:
            continue
        seen.add(oid)
        out.add(code_of[oid])
        stack.extend(children.get(oid, []))
    return out


def admin_scope(cur, user_id) -> Optional[dict]:
    """관제사의 관리 범위 — {groupId, directoryAdmin, orgCode, orgCodes(set|None=전체)}. 없으면 None.
    dispatch_discovery(P2)와 같은 소속 판정(volte 회선 멤버십)에 directory_admin 을 얹는다. 컬럼 미적용 DB = None."""
    if user_id is None or not _dispatch.has_dispatch_tables(cur) or not _dispatch.has_directory_admin_column(cur):
        return None
    cur.execute("SELECT g.id, g.directory_admin, g.org_id FROM dispatch_group_members m "
                "JOIN dispatch_groups g ON g.id=m.group_id "
                "JOIN volte_subscriptions s ON s.id=m.user_id WHERE s.user_id=%s LIMIT 1", (user_id,))
    r = cur.fetchone()
    if not r:
        return None
    gid, da, org_id = (r['id'], r['directory_admin'], r['org_id']) if isinstance(r, dict) else (r[0], r[1], r[2])
    da = da or 'none'
    if da == 'none':
        return None
    rows = _org_rows(cur)
    org_code = next((c for oid, c, _n, _p, _s in rows if oid == org_id), '') if org_id is not None else ''
    if da == 'all':
        return {"groupId": gid, "directoryAdmin": 'all', "orgCode": org_code, "orgCodes": None}
    if org_id is None or not org_code:
        return None                                   # own 인데 조직이 없으면 범위가 비어 관리 불가
    return {"groupId": gid, "directoryAdmin": 'own', "orgCode": org_code, "orgCodes": org_subtree_codes(rows, org_id)}


def in_scope(scope: dict, org_code: str) -> bool:
    codes = scope.get("orgCodes")
    return True if codes is None else (org_code or '') in codes


def _org_by_code(cur, code: str):
    cur.execute("SELECT id, code, name, parent_id, sort_order FROM organizations WHERE code=%s", (code,))
    return cur.fetchone()


# ── 감사 ──────────────────────────────────────────────────────────────────────

def _audit(config, actor: str, ip: str, entity: str, entity_id, action: str, after=None):
    try:
        _m.audit_config_change(config.get('CimsDatabase', {}), actor, ip, entity, entity_id, action, after=after,
                               reason='dispatch_directory')
    except Exception as e:
        logger.log_warning(f"[provisioning/directory] audit: {e}")


# ── 조회(관리 화면 한 벌) ─────────────────────────────────────────────────────

def _services(config) -> dict:
    """접속서비스 후보 — {volte:[{name,domain}], ptt:[...]}: runtime store access_services 우선, 없으면 Provisioning.Services."""
    out = {"volte": [], "ptt": []}
    try:
        from services import ha_lookup as _ha, file_store as _fs
        for r in _fs.load_all(_ha.collection_dir(config, 'access_services')) or []:
            kind = (r.get('kind') or r.get('service_kind') or '').lower()
            kind = 'ptt' if kind in ('ptt', 'mcptt') else 'volte'
            if r.get('name'):
                out[kind].append({"name": r['name'], "domain": (r.get('domain') or '').strip()})
    except Exception as e:
        logger.log_warning(f"[provisioning/directory] access_services lookup failed ({e}) — Provisioning.Services fallback")
    if not out["volte"] and not out["ptt"]:
        svcs = ((config or {}).get('Provisioning') or {}).get('Services') or {}
        for kind in ('volte', 'ptt'):
            s = svcs.get(kind) or {}
            if s.get('domain'):
                out[kind].append({"name": s.get('name') or kind, "domain": s['domain']})
    return out


def _sub_wire(row: dict) -> dict:
    return {"msisdn": row.get('id') or '', "imsi": row.get('imsi') or '', "serviceRef": row.get('service_ref') or '',
            "sipTransport": row.get('sip_transport') or 'UDP', "authScheme": row.get('auth_scheme') or 'digest'}


def _members_in_scope(cur, scope: dict) -> list:
    """범위 안 구성원(person) + volte/ptt 가입 + PTT 프로파일 자격. users.org_id 는 조직 code."""
    has_title = _admin._has_user_column(cur, 'title')
    title_col = ", u.title" if has_title else ""
    cur.execute(f"SELECT u.id, u.name, u.login_id, u.org_id{title_col} FROM users u ORDER BY u.name, u.id")
    people = []
    for r in cur.fetchall():
        org = r.get('org_id') or ''
        if not in_scope(scope, org):
            continue
        people.append({"userId": r['id'], "name": r.get('name') or '', "loginId": r.get('login_id') or '',
                       "org": org, "title": (r.get('title') if has_title else '') or '', "volte": None, "ptt": None})
    if not people:
        return people
    by_id = {p["userId"]: p for p in people}
    ids = list(by_id)
    ph = ",".join(["%s"] * len(ids))
    aka_extra = _admin._aka_select_extra(cur)
    for kind, table in (('volte', 'volte_subscriptions'), ('ptt', 'ptt_subscriptions')):
        cur.execute(f"SELECT id, user_id, service_ref, imsi, sip_transport {aka_extra} FROM {table} "
                    f"WHERE user_id IN ({ph}) ORDER BY id", ids)
        for r in cur.fetchall():
            p = by_id.get(r['user_id'])
            if p is not None and p[kind] is None:            # 회선 여러 개면 첫 번째(관제 앱은 종류당 번호 하나를 관리)
                p[kind] = _sub_wire(r)
    for p in people:
        if p["ptt"]:
            prof = _m.get_user_profile(p["ptt"]["msisdn"]) or {}
            p["ptt"]["profile"] = {k: bool(prof.get(col, False)) for k, col in _PROFILE_KEYS.items()}
    return people


def _orgs_in_scope(cur, scope: dict) -> list:
    rows = _org_rows(cur)
    id2code = {oid: code for oid, code, _n, _p, _s in rows}
    out = []
    for oid, code, name, pid, so in rows:
        if not in_scope(scope, code):
            continue
        parent = id2code.get(pid, '') if pid is not None else ''
        # 범위 루트(own)의 부모는 범위 밖 — 앱이 트리를 그릴 수 있게 부모 코드는 그대로 두되 목록에는 넣지 않는다.
        out.append({"code": code, "name": name, "parent": parent, "sort": so})
    out.sort(key=lambda o: (o["sort"], o["name"]))
    return out


def _admin_view(cur, config, scope: dict) -> dict:
    return {
        "scope": {"groupId": scope["groupId"], "directoryAdmin": scope["directoryAdmin"], "orgCode": scope["orgCode"]},
        "services": _services(config),
        "orgs": _orgs_in_scope(cur, scope),
        "members": _members_in_scope(cur, scope),
    }


def _visible_group_sets(cur, scope: dict, my_uid) -> Tuple[set, set]:
    """(청취 범위 ptt_groups.id 집합 | None=전체, 내 멤버 그룹 ptt_groups.id 집합) — 관제 그룹 ptt_listen(all|listed) 은
    provisioning/me dispatch.pttTargets 와 같은 원천(dispatch_group_ptt_targets), 멤버십은 ptt_group_members(내 PTT 회선)."""
    listen: Optional[set] = set()
    cur.execute("SELECT ptt_listen FROM dispatch_groups WHERE id=%s", (scope["groupId"],))
    r = cur.fetchone()
    mode = (r['ptt_listen'] if isinstance(r, dict) else (r[0] if r else None)) or 'none'
    if mode == 'all':
        listen = None
    elif mode == 'listed':
        cur.execute("SELECT ptt_group_id FROM dispatch_group_ptt_targets WHERE group_id=%s", (scope["groupId"],))
        listen = {(x['ptt_group_id'] if isinstance(x, dict) else x[0]) for x in cur.fetchall()}
    member: set = set()
    if my_uid is not None:
        cur.execute("SELECT gm.group_id FROM ptt_group_members gm JOIN ptt_subscriptions ps ON ps.id=gm.user_id WHERE ps.user_id=%s", (my_uid,))
        member = {(x['group_id'] if isinstance(x, dict) else x[0]) for x in cur.fetchall()}
    return listen, member


def _groups_in_scope(cur, scope: dict, my_uid) -> list:
    """관제사에게 보이는 PTT 그룹 = 관리 범위(org_code 가 범위 안) ∪ 내 소유 ∪ 관제 그룹 청취 범위 ∪ 내 멤버 그룹.
    행마다 canManage(관리 범위 안 또는 내 소유 — GMS PUT/DELETE 게이트 mcptt._admin_manages_group 와 같은 판정)를 실어
    앱이 편집/삭제를 그 행에만 연다. 청취·멤버 그룹은 관제사가 매일 다루는 그룹이라 관리 권한이 없어도 목록에는 보여야 한다
    (조직 미지정 그룹이 own 범위에서 통째로 사라지지 않게). GMS 목록(멤버 그룹)과 별개의 관리용 열거."""
    cur.execute("SELECT id, mcptt_group_id, name, org_code, authorized_user_id, group_type FROM ptt_groups ORDER BY name, mcptt_group_id")
    rows = cur.fetchall()
    ids = [r['id'] for r in rows]
    counts = {}
    if ids:
        cur.execute("SELECT group_id, COUNT(*) AS n FROM ptt_group_members WHERE group_id IN (%s) GROUP BY group_id"
                    % ",".join(["%s"] * len(ids)), ids)
        counts = {r['group_id']: int(r['n']) for r in cur.fetchall()}
    listen, member = _visible_group_sets(cur, scope, my_uid)
    out = []
    for r in rows:
        owner = r.get('authorized_user_id')
        is_owner = my_uid is not None and owner == my_uid
        can_manage = in_scope(scope, r.get('org_code') or '') or is_owner
        in_listen = listen is None or r['id'] in listen
        is_member = r['id'] in member
        if not (can_manage or in_listen or is_member):
            continue
        gid = r['mcptt_group_id']
        grp = _m.GROUPS.get(_m._group_uri(gid)) or {}
        out.append({"id": gid, "uri": _m._group_uri(gid), "name": r.get('name') or gid,
                    "memberCount": counts.get(r['id'], 0), "isOwner": is_owner, "orgCode": r.get('org_code') or '',
                    "sessionType": r.get('group_type') or 'prearranged', "etag": grp.get('etag') or '',
                    "canManage": bool(can_manage), "inListenScope": bool(in_listen), "isMember": is_member})
    return out


# ── 쓰기 ──────────────────────────────────────────────────────────────────────

def _org_write(cur, config, scope, method, code, body, actor, ip) -> HandlerResult:
    if method == 'POST':
        if not isinstance(body, dict):
            return _json(400, {'error': 'JSON body required'})
        new_code = (body.get('code') or '').strip()
        parent = (body.get('parent') or '').strip()
        if not new_code or not (body.get('name') or '').strip():
            return _json(400, {'error': 'code, name required'})
        if _org_by_code(cur, new_code):
            return _json(409, {'error': 'code_exists', 'code': new_code})
        parent_row = _org_by_code(cur, parent) if parent else None
        if parent and not parent_row:
            return _json(400, {'error': 'unknown_parent', 'parent': parent})
        # own 범위는 범위 안 조직의 하위로만 만들 수 있다(루트 신설 = 범위 밖). all 은 루트도 허용.
        if not parent and scope["orgCodes"] is not None:
            return _json(403, {'error': 'out_of_scope', 'detail': 'parent required inside scope'})
        if parent and not in_scope(scope, parent):
            return _json(403, {'error': 'out_of_scope', 'org': parent})
        return None, {'code': new_code, 'name': body.get('name').strip(),
                      'parent_id': parent_row['id'] if parent_row else None, 'sort_order': int(body.get('sort') or 0)}
    row = _org_by_code(cur, code)
    if not row:
        return _json(404, {'error': 'Not found'})
    if not in_scope(scope, code):
        return _json(403, {'error': 'out_of_scope', 'org': code})
    if method == 'PUT':
        if not isinstance(body, dict):
            return _json(400, {'error': 'JSON body required'})
        upd = {}
        if 'name' in body:
            upd['name'] = (body.get('name') or '').strip() or row['name']
        if 'sort' in body:
            upd['sort_order'] = int(body.get('sort') or 0)
        if 'parent' in body:
            parent = (body.get('parent') or '').strip()
            if code == scope["orgCode"] and scope["orgCodes"] is not None:
                return _json(403, {'error': 'out_of_scope', 'detail': 'cannot move the scope root'})
            if parent:
                prow = _org_by_code(cur, parent)
                if not prow:
                    return _json(400, {'error': 'unknown_parent', 'parent': parent})
                if not in_scope(scope, parent):
                    return _json(403, {'error': 'out_of_scope', 'org': parent})
                if parent in org_subtree_codes(_org_rows(cur), row['id']):
                    return _json(400, {'error': 'cyclic_parent'})
                upd['parent_id'] = prow['id']
            else:
                if scope["orgCodes"] is not None:
                    return _json(403, {'error': 'out_of_scope', 'detail': 'root move needs directory_admin=all'})
                upd['parent_id'] = None
        return None, (row['id'], upd)
    if method == 'DELETE':
        if code == scope["orgCode"] and scope["orgCodes"] is not None:
            return _json(403, {'error': 'out_of_scope', 'detail': 'cannot delete the scope root'})
        cur.execute("SELECT COUNT(*) AS n FROM organizations WHERE parent_id=%s", (row['id'],))
        if int(cur.fetchone()['n']) > 0:
            return _json(409, {'error': 'not_empty', 'detail': 'child organizations remain'})
        cur.execute("SELECT COUNT(*) AS n FROM users WHERE org_id=%s", (code,))
        if int(cur.fetchone()['n']) > 0:
            return _json(409, {'error': 'not_empty', 'detail': 'members remain'})
        return None, row['id']
    return _json(405, {'error': 'Method Not Allowed'})


def _sub_body(kind: str, b: dict) -> dict:
    """와이어 {msisdn,imsi,serviceRef,sipTransport,password} → admin.py 가입 본문."""
    msisdn = (b.get('msisdn') or '').strip()
    imsi = (b.get('imsi') or '').strip() or msisdn.lstrip('+')      # USIM 없는 관제 소프트폰 규약: imsi = 가입 id 숫자
    out = {'id': msisdn, 'imsi': imsi}
    if b.get('serviceRef') is not None:
        out['service_ref'] = b.get('serviceRef')
    if b.get('sipTransport'):
        out['sip_transport'] = str(b['sipTransport']).upper()
    if b.get('password'):
        out['passwd'] = b['password']
    return out


async def _member_write(cur, config, scope, method, parts, body, actor, ip, my_uid) -> HandlerResult:
    """parts = (userId?, 'volte'|'ptt'?, 'profile'?)."""
    if method == 'POST' and not parts:
        if not isinstance(body, dict):
            return _json(400, {'error': 'JSON body required'})
        org = (body.get('org') or '').strip()
        if not org or not _org_by_code(cur, org):
            return _json(400, {'error': 'unknown_org', 'org': org})
        if not in_scope(scope, org):
            return _json(403, {'error': 'out_of_scope', 'org': org})
        ub = {'name': body.get('name') or '', 'org_id': org, 'title': body.get('title') or '',
              'login_id': body.get('loginId') or None, 'passwd': body.get('password') or None}
        r = await _admin._create_user(ub, config)
        if r.status != 201:
            return r
        uid = r.body['id']
        _audit(config, actor, ip, 'user', uid, 'create', after={'name': ub['name'], 'org': org})
        for kind, svc in _KINDS.items():
            sb = body.get(kind)
            if isinstance(sb, dict) and (sb.get('msisdn') or '').strip():
                rs = await _admin._add_subscription(str(uid), svc, _sub_body(kind, sb), config)
                if rs.status != 201:
                    return _json(rs.status, dict(rs.body if isinstance(rs.body, dict) else {'error': str(rs.body)},
                                                 userId=uid, kind=kind))
                _audit(config, actor, ip, 'subscription', sb['msisdn'], 'create', after={'kind': kind, 'userId': uid})
        return _json(201, {'userId': uid})

    if not parts:
        return _json(405, {'error': 'Method Not Allowed'})
    user_id = parts[0]
    cur.execute("SELECT id, org_id, name FROM users WHERE id=%s", (user_id,))
    urow = cur.fetchone()
    if not urow:
        return _json(404, {'error': 'User not found'})
    if not in_scope(scope, urow.get('org_id') or ''):
        return _json(403, {'error': 'out_of_scope', 'org': urow.get('org_id') or ''})

    if len(parts) == 1:
        if method == 'PUT':
            if not isinstance(body, dict):
                return _json(400, {'error': 'JSON body required'})
            ub = {}
            if 'name' in body:
                ub['name'] = body.get('name') or ''
            if 'title' in body:
                ub['title'] = body.get('title') or ''
            if 'loginId' in body:
                ub['login_id'] = (body.get('loginId') or '').strip() or None
            if body.get('password'):
                ub['passwd'] = body['password']
            if 'org' in body:
                org = (body.get('org') or '').strip()
                if not org or not _org_by_code(cur, org):
                    return _json(400, {'error': 'unknown_org', 'org': org})
                if not in_scope(scope, org):
                    return _json(403, {'error': 'out_of_scope', 'org': org})
                ub['org_id'] = org
            r = await _admin._update_user(user_id, ub, config)
            if r.status == 200:
                _audit(config, actor, ip, 'user', user_id, 'update', after=ub if 'passwd' not in ub else dict(ub, passwd='***'))
            return r
        if method == 'DELETE':
            if my_uid is not None and int(user_id) == my_uid:
                return _json(409, {'error': 'self_delete'})
            r = await _admin._delete_user(user_id, config)
            if r.status == 200:
                _audit(config, actor, ip, 'user', user_id, 'delete')
            return r
        return _json(405, {'error': 'Method Not Allowed'})

    kind = parts[1]
    if kind not in _KINDS:
        return _json(404, {'error': 'Not Found'})
    svc = _KINDS[kind]
    table = 'volte_subscriptions' if kind == 'volte' else 'ptt_subscriptions'
    cur.execute(f"SELECT id, service_ref, sip_transport FROM {table} WHERE user_id=%s ORDER BY id", (user_id,))
    existing_rows = cur.fetchall()
    existing = [r['id'] for r in existing_rows]

    if len(parts) == 3 and parts[2] == 'profile' and kind == 'ptt':
        if method != 'PUT':
            return _json(405, {'error': 'Method Not Allowed'})
        if not isinstance(body, dict):
            return _json(400, {'error': 'JSON body required'})
        if not existing:
            return _json(404, {'error': 'Subscription not found'})
        msisdn = existing[0]
        cur_prof = dict(_m.get_user_profile(msisdn) or {})
        # 요청에 없는 자격은 현재값 유지 — 선택 컬럼(allow_ambient_listening/allow_create_group)은 값이 있을 때만 싣는다
        #   (컬럼 미적용 DB 에서 admin PUT 이 400 을 내지 않게).
        pb = {}
        for k, col in _PROFILE_KEYS.items():
            if k in body:
                pb[col] = bool(body[k])
            elif col in _admin._OPT_PROFILE_COLS:
                if cur_prof.get(col):
                    pb[col] = True
            else:
                pb[col] = bool(cur_prof.get(col, True))
        for k in ('emergency_group_mode', 'emergency_group_id', 'private_emergency_mode', 'emergency_private_recipient'):
            if cur_prof.get(k) is not None:
                pb[k] = cur_prof[k]
        r = await _admin._put_ptt_profile(user_id, msisdn, pb, config)
        if r.status == 200:
            _audit(config, actor, ip, 'ptt_profile', msisdn, 'update', after={k: pb.get(c, False) for k, c in _PROFILE_KEYS.items()})
            return _json(200, {"msisdn": msisdn, "profile": {k: bool(r.body.get(c, False)) for k, c in _PROFILE_KEYS.items()}})
        return r

    if len(parts) != 2:
        return _json(404, {'error': 'Not Found'})
    if method == 'PUT':
        if not isinstance(body, dict):
            return _json(400, {'error': 'JSON body required'})
        msisdn = (body.get('msisdn') or '').strip()
        if not msisdn:
            return _json(400, {'error': 'msisdn required'})
        sb = _sub_body(kind, body)
        if existing and msisdn in existing:
            r = await _admin._update_subscription(user_id, svc, msisdn, sb, config)
            if r.status == 200:
                _audit(config, actor, ip, 'subscription', msisdn, 'update', after={'kind': kind, 'userId': user_id})
            return r
        # 번호 변경 = 종전 회선 삭제 + 신규 개설(H(A1) 재결박이라 password 필수). 다른 person 의 번호면 409.
        cur.execute(f"SELECT user_id FROM {table} WHERE id=%s", (msisdn,))
        taken = cur.fetchone()
        if taken:
            return _json(409, {'error': 'number_exists', 'msisdn': msisdn})
        if existing and not sb.get('passwd'):
            return _json(400, {'error': 'password required when changing the number (ha1 rebinding)'})
        # 번호 변경 — 요청에 없는 접속서비스·transport 는 종전 회선 값을 물려받는다(앱이 번호만 바꿔도 등록 조건이 유지되게).
        if existing_rows:
            if 'service_ref' not in sb and existing_rows[0].get('service_ref'):
                sb['service_ref'] = existing_rows[0]['service_ref']
            if 'sip_transport' not in sb and existing_rows[0].get('sip_transport'):
                sb['sip_transport'] = existing_rows[0]['sip_transport']
        for old in existing:
            rd = await _admin._delete_subscription(user_id, svc, old, config)
            if rd.status != 200:
                return rd
            _audit(config, actor, ip, 'subscription', old, 'delete', after={'kind': kind, 'userId': user_id, 'replacedBy': msisdn})
        r = await _admin._add_subscription(user_id, svc, sb, config)
        if r.status == 201:
            _audit(config, actor, ip, 'subscription', msisdn, 'create', after={'kind': kind, 'userId': user_id})
        return r
    if method == 'DELETE':
        if not existing:
            return _json(404, {'error': 'Subscription not found'})
        for old in existing:
            r = await _admin._delete_subscription(user_id, svc, old, config)
            if r.status != 200:
                return r
            _audit(config, actor, ip, 'subscription', old, 'delete', after={'kind': kind, 'userId': user_id})
        return _json(200, {'userId': user_id, 'kind': kind, 'deleted': existing})
    return _json(405, {'error': 'Method Not Allowed'})


# ── 핸들러 ────────────────────────────────────────────────────────────────────

# ── 구성원 일괄 가져오기 ─────────────────────────────────────────────────────────────────────────────
#  POST /provisioning/directory/members/import — 행마다 POST members 와 같은 생성 경로(_member_write)를 밟는다: 같은 범위
#  게이트·같은 감사(E-AUD-006)·같은 회선 규약(imsi 비면 번호 숫자, 회선 password 필수). 본문은 text/csv(UTF-8, 머리행) 또는
#  JSON {"rows":[<POST members 본문>…]}. 행 단위 결과를 돌려주고 한 행의 실패가 다른 행을 막지 않는다(콘솔 users/import 와 같은 계약).
_IMPORT_MAX_ROWS = 500
_IMPORT_USER_COLS = {'name': 'name', 'org': 'org', 'orgcode': 'org', 'title': 'title', 'loginid': 'loginId',
                     'login': 'loginId', 'password': 'password', 'passwd': 'password'}
_IMPORT_SUB_COLS = {'msisdn': 'msisdn', 'number': 'msisdn', 'imsi': 'imsi', 'serviceref': 'serviceRef',
                    'service': 'serviceRef', 'siptransport': 'sipTransport', 'transport': 'sipTransport',
                    'password': 'password', 'passwd': 'password'}


def _import_rows_from_csv(text: str):
    """CSV 원문 → POST members 본문 목록. 열 이름은 대소문자·`_`·`-` 무시: name, org, title, login_id, password,
    volte_msisdn, volte_imsi, volte_service_ref, volte_sip_transport, volte_password, ptt_msisdn, ptt_… ."""
    import csv
    import io
    rd = csv.reader(io.StringIO(text.lstrip('\ufeff')))   # BOM 은 첫 열 이름에 붙는다
    header = None
    rows = []
    for raw in rd:
        if not raw or all(not (c or '').strip() for c in raw):
            continue
        if header is None:
            header = [re.sub(r'[\s_\-]', '', (c or '')).strip().lower() for c in raw]
            continue
        body: dict = {}
        for i, col in enumerate(header):
            val = (raw[i] if i < len(raw) else '').strip()
            if not col or not val:
                continue
            if col in _IMPORT_USER_COLS:
                body[_IMPORT_USER_COLS[col]] = val
                continue
            for kind in _KINDS:
                if col.startswith(kind) and col[len(kind):] in _IMPORT_SUB_COLS:
                    body.setdefault(kind, {})[_IMPORT_SUB_COLS[col[len(kind):]]] = val
                    break
        rows.append(body)
    return rows


async def _members_import(cur, config, scope, body, actor, ip, my_uid) -> HandlerResult:
    if isinstance(body, dict):
        rows = body.get('rows')
        if not isinstance(rows, list):
            return _json(400, {'error': 'rows_required'})
    elif isinstance(body, str):
        rows = _import_rows_from_csv(body)
        if not rows:
            return _json(400, {'error': 'empty_csv'})
    else:
        return _json(400, {'error': 'csv_or_json_required'})
    if len(rows) > _IMPORT_MAX_ROWS:
        return _json(413, {'error': 'too_many_rows', 'max': _IMPORT_MAX_ROWS})
    results = []
    created = 0
    for i, row in enumerate(rows, start=1):
        if not isinstance(row, dict) or not (row.get('name') or '').strip():
            results.append({'row': i, 'status': 400, 'error': 'name_required'})
            continue
        r = await _member_write(cur, config, scope, 'POST', (), row, actor, ip, my_uid)
        item = {'row': i, 'status': r.status}
        if isinstance(r.body, dict):
            if r.status == 201:
                item['userId'] = r.body.get('userId')
                created += 1
            else:
                item.update({k: v for k, v in r.body.items() if k in ('error', 'org', 'userId', 'kind', 'detail')})
        results.append(item)
    logger.log_info(f"[provisioning/directory] members/import by {actor}: rows={len(rows)} created={created}")
    return _json(200, {'created': created, 'failed': len(rows) - created, 'results': results})


async def handle_directory_admin(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {}) or {}
    token, err = _auth(handler_args)
    if err:
        return err
    parts = _path_parts(handler_args.full_path)
    method = handler_args.method.upper()
    body = handler_args.body
    is_import = parts[:2] == ['members', 'import']
    if isinstance(body, (bytes, bytearray)):
        ctype = str((getattr(handler_args, 'headers', None) or {}).get('content-type', '')).lower()
        if is_import and 'json' not in ctype:
            body = body.decode('utf-8-sig', errors='replace')    # CSV 원문 — _members_import 가 파싱
        else:
            try:
                body = json.loads(body.decode('utf-8')) if body else None
            except ValueError:
                return _json(400, {'error': 'invalid_json'})
    ip = getattr(handler_args, 'client_ip', '') or ''
    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                msisdn, my_uid = caller_identity(cur, token)
                scope = admin_scope(cur, my_uid)
                if not scope:
                    return _json(403, {'error': 'no_directory_admin'})
                head = parts[0] if parts else ''
                rest = parts[1:]

                if head == 'admin' and not rest:
                    if method != 'GET':
                        return _json(405, {'error': 'Method Not Allowed'})
                    view = _admin_view(cur, config, scope)
                    etag = _m._content_etag_json(view)
                    inm = handler_args.headers.get('if-none-match') or handler_args.headers.get('If-None-Match')
                    if inm and inm == etag:
                        return HandlerResult(status=304, headers={"ETag": etag})
                    logger.log_info(f"[provisioning/directory/admin] {msisdn} scope={scope['directoryAdmin']}/{scope['orgCode']} "
                                    f"orgs={len(view['orgs'])} members={len(view['members'])}")
                    return _json(200, view, headers={"ETag": etag})

                if head == 'groups' and not rest:
                    if method != 'GET':
                        return _json(405, {'error': 'Method Not Allowed'})
                    return _json(200, {"scope": {"directoryAdmin": scope["directoryAdmin"], "orgCode": scope["orgCode"]},
                                       "groups": _groups_in_scope(cur, scope, my_uid)})

                if head == 'orgs':
                    code = rest[0] if rest else ''
                    if len(rest) > 1 or (method == 'POST') != (not rest):
                        return _json(405, {'error': 'Method Not Allowed'})
                    res = _org_write(cur, config, scope, method, code, body, msisdn, ip)
                    if isinstance(res, HandlerResult):
                        return res
                    _none, payload = res
                    if method == 'POST':
                        r = await _org._create_org(payload, config)
                        if r.status == 201:
                            _audit(config, msisdn, ip, 'organization', payload['code'], 'create', after=payload)
                            r = _json(201, {'code': payload['code'], 'id': r.body.get('id')})
                        return r
                    if method == 'PUT':
                        oid, upd = payload
                        if not upd:
                            return _json(400, {'error': 'no updatable fields'})
                        r = await _org._update_org(oid, upd, config)
                        if r.status == 200:
                            _audit(config, msisdn, ip, 'organization', code, 'update', after=upd)
                            r = _json(200, {'code': code})
                        return r
                    r = await _org._delete_org(payload, config)
                    if r.status == 200:
                        _audit(config, msisdn, ip, 'organization', code, 'delete')
                        r = _json(200, {'code': code})
                    return r

                if is_import:
                    if method != 'POST':
                        return _json(405, {'error': 'Method Not Allowed'})
                    return await _members_import(cur, config, scope, body, msisdn, ip, my_uid)
                if head == 'members':
                    return await _member_write(cur, config, scope, method, rest, body, msisdn, ip, my_uid)

                return _json(404, {'error': 'Not Found'})
    except pymysql.Error as e:
        logger.log_error(f"[provisioning/directory] DB error: {e}")
        return _json(503, {'error': 'db_error', 'detail': str(e)})


CSC_DIRECTORY_ADMIN_HANDLER_LIST = [
    ('/provisioning/directory/admin', handle_directory_admin, {}),
    ('/provisioning/directory/orgs', handle_directory_admin, {}),
    ('/provisioning/directory/members', handle_directory_admin, {}),
    ('/provisioning/directory/groups', handle_directory_admin, {}),
]
