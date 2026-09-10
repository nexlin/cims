"""
CIMS 전화 그룹(phone group)·역할(role) 관리 REST API — docs/design/features/dispatch_center.md §3·§8.2,
docs/design/features/mcptt_authorization.md §2·§3, docs/api/admin_api.md §6.7·§6.8.

전화 그룹 = 픽업 그룹 + (선택) 대표번호 — **유선 전화의 일반 기능**이며 관제 권한과 무관하다. 불변 id(pg-xxxxxxxx,
전환 전 dg-… 유지)가 곧 *_subscriptions.pickup_group 값이라 당겨받기·BLF·대표번호 병렬 호출이 한 축을 공유한다.
역할 = 능력 + 범위 한 행(roles) — 감청(monitor_call)·PTT 청취(ptt_listen)·이력/녹취·관리 범위(directory_write)는
여기 있고, 사람(person)은 역할 하나에 배정된다(role_assignments). 콘솔 계정의 배정은 OAM console_accounts[].role 이다.

  GET/POST          /api/v1/phone-groups                                directory.read / directory.write
  GET/PUT/DELETE    /api/v1/phone-groups/{id}
  GET/POST          /api/v1/phone-groups/{id}/members                   {user_id, alert_order?}
  DELETE            /api/v1/phone-groups/{id}/members/{user_id}

  GET/POST          /api/v1/roles                                       전부 authz.manage (콘솔 manager 이상)
  GET/PUT/DELETE    /api/v1/roles/{id}                                  내장 4행은 읽기 전용(403 builtin)
  PUT               /api/v1/roles/{id}/monitor-targets                  {phone_group_ids:[pg-…]}
  PUT               /api/v1/roles/{id}/ptt-targets                      {ptt_group_ids:[mcptt_group_id…]}
  GET/PUT           /api/v1/roles/{id}/assignments                      {principal_type:'user', principal_id:<users.id>}
  DELETE            /api/v1/roles/{id}/assignments/{principal_type}/{principal_id}

SoT 는 멤버십(phone_group_members)이다 — 멤버 추가/제거/그룹 삭제 시 그 회선이 속한 person 의 volte·ptt **전 회선**
pickup_group 을 유효 그룹(effective_phone_group)으로 재계산하고 값이 바뀐 회선마다 USER_CHANGED 를 보낸다. CSP 에는
PHONE_GROUP_CHANGED(uri=그룹 id, DELETE=제거·그 외 단건 재적재)·ROLE_CHANGED(uri=역할 id 또는 배정 person id — 전량
재적재)로 알린다. 청취 자격(ptt_user_profile.allow_ambient_listening)은 역할 배정의 결과로 여기서 동기한다(§2.4).
판정은 services/authz.can() 하나 — 관제 앱 관리 API(handlers/dispatch_directory)도 같은 전화 그룹 쓰기 코드를 부른다.
"""

import secrets
from urllib.parse import urlparse, unquote, parse_qs
from pathlib import PurePath

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult
from services import admin_auth
from services import authz
from services.mcptt import notify_csp

_PG_BASE = '/api/v1/phone-groups'
_ROLE_BASE = '/api/v1/roles'

_ALERT_MODES = ('parallel', 'sequential')
_BUSY_MODES = ('skip', 'alert')

_SCHEMA_ERROR = {'error': 'schema_not_migrated',
                 'detail': 'phone_groups/roles tables absent — sql/migrate_phone_groups_roles.sql not applied'}

_HAS_TABLES = None  # phone_groups 테이블 프로브 캐시 (프로세스 수명). None=미확인
_SUB_TABLES = ('volte_subscriptions', 'ptt_subscriptions')


def _get_db(config: dict):
    db = config.get('CimsDatabase', {})
    return pymysql.connect(
        host=db.get('Host', '127.0.0.1'),
        port=int(db.get('Port', 3306)),
        user=db.get('User', 'root'),
        password=db.get('Password', ''),
        database=db.get('Db', 'cims'),
        charset='utf8mb4',
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=True,
    )


def _path_parts(full_path: str, base: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _query(handler_args) -> dict:
    try:
        return {k: v[0] for k, v in parse_qs(urlparse(handler_args.full_path).query).items()}
    except Exception:
        return {}


def has_phone_group_tables(cur) -> bool:
    """phone_groups 존재 여부 — migrate_phone_groups_roles.sql. 한 번 확인 후 캐시."""
    global _HAS_TABLES
    if _HAS_TABLES is None:
        cur.execute("SHOW TABLES LIKE 'phone_groups'")
        _HAS_TABLES = cur.fetchone() is not None
    return _HAS_TABLES


def _cell(row, key: str, idx: int = 0):
    """DictCursor/tuple 커서 공용 — 아래 멤버십 조회는 mcptt(tuple 커서)·authz 도 부른다."""
    if row is None:
        return None
    return row[key] if isinstance(row, dict) else row[idx]


# ── 멤버십·파생 (§3.2) — admin.py(pickup_group 409 게이트)·services/authz(own 범위)·mcptt(discovery)가 쓴다 ──

def phone_group_of_user(cur, user_id: str):
    """가입(회선) id 자신의 멤버십 전화 그룹 id (없으면 None). 테이블 미적용이면 None.
    멤버 이동(이전 그룹 통지)처럼 **멤버 행** 자체를 묻는 곳에 쓴다. 귀속 판정은 effective_phone_group."""
    if not has_phone_group_tables(cur):
        return None
    cur.execute("SELECT group_id FROM phone_group_members WHERE user_id=%s", (user_id,))
    return _cell(cur.fetchone(), 'group_id')


def _person_of(cur, user_id: str):
    """가입 id → person(users.id). 어느 가입 테이블에도 없으면 None."""
    for t in _SUB_TABLES:
        cur.execute(f"SELECT user_id FROM {t} WHERE id=%s", (user_id,))
        row = cur.fetchone()
        if row:
            return _cell(row, 'user_id')
    return None


def phone_group_of_person(cur, person_id):
    """person 의 전화 그룹 id — 그 사람의 어느 회선이든 멤버십이 있으면 그 그룹(가입자당 그룹 하나 §3.2;
    여럿이면 alert_order·회선 id 순 첫째로 결정적). 없으면 None. 테이블 미적용이면 None."""
    if person_id is None or not has_phone_group_tables(cur):
        return None
    cur.execute("SELECT m.group_id FROM phone_group_members m WHERE m.user_id IN ("
                "SELECT id FROM volte_subscriptions WHERE user_id=%s UNION "
                "SELECT id FROM ptt_subscriptions WHERE user_id=%s) "
                "ORDER BY m.alert_order, m.user_id LIMIT 1", (person_id, person_id))
    return _cell(cur.fetchone(), 'group_id')


def effective_phone_group(cur, user_id: str):
    """회선의 유효 전화 그룹 = 자기 멤버십 → 없으면 같은 person 의 다른 회선 멤버십(파생). 전화 그룹은 person
    귀속이라 관제사의 PTT 회선은 멤버(대표번호 포크 대상)가 아니어도 VoLTE 회선의 그룹을 물려받는다. CSP
    EffectiveGroupOf(멤버 색인 → pickup_group)가 같은 답을 내도록 pickup_group 파생값의 정의이며(§3.2·§5.6),
    admin.py 의 pickup_group 직접 편집 409 게이트(derived_from_phone_group)가 쓴다."""
    own = phone_group_of_user(cur, user_id)
    if own is not None or not has_phone_group_tables(cur):
        return own
    return phone_group_of_person(cur, _person_of(cur, user_id))


def _sync_pickup_group(cur, user_id: str) -> list:
    """멤버십 변경 뒤 파생값 재계산 — user_id 가 속한 person 의 volte/ptt **전 회선** pickup_group 을
    effective_phone_group 으로 맞춘다(컬럼 존재 테이블만). 값이 바뀐 회선 id 목록을 돌려준다(USER_CHANGED 대상 —
    CSP 는 회선별 사용자 캐시로 pickup_group 을 든다). 멤버 행 뒤에 호출한다(INSERT/DELETE 반영 상태를 읽는다).

    관제사의 PTT 회선을 멤버로 넣지 않고 파생으로 잇는 이유: 멤버 행은 대표번호 포크 대상(CSP ResolveForkTargets —
    서비스 구분 없이 등록 멤버 전원)이라 PTT 앱까지 울린다. 반면 PTT 세션 가시성(§5.6a)의 "자기 그룹원" 판정과
    PTT 회선 dialog 인가 규칙 1 은 SIP 신원(PTT id)으로 EffectiveGroupOf 를 묻는다."""
    person = _person_of(cur, user_id)
    changed = []
    for t in _SUB_TABLES:
        cur.execute("SHOW COLUMNS FROM %s LIKE 'pickup_group'" % t)
        if cur.fetchone() is None:
            continue
        if person is None:
            cur.execute(f"SELECT id, pickup_group FROM {t} WHERE id=%s", (user_id,))
        else:
            cur.execute(f"SELECT id, pickup_group FROM {t} WHERE user_id=%s", (person,))
        for row in cur.fetchall():
            eff = effective_phone_group(cur, row['id'])
            if (row['pickup_group'] or None) != eff:
                cur.execute(f"UPDATE {t} SET pickup_group=%s WHERE id=%s", (eff, row['id']))
                changed.append(row['id'])
    return changed


def _changed_users(*ids_lists) -> list:
    """USER_CHANGED 통지 대상 — 멤버 행이 바뀐 회선 + 파생값이 바뀐 회선, 순서 유지·중복 제거."""
    out = []
    for ids in ids_lists:
        for uid in ids:
            if uid and uid not in out:
                out.append(uid)
    return out


def _subscriber_exists(cur, user_id: str) -> bool:
    for t in _SUB_TABLES:
        cur.execute(f"SELECT 1 FROM {t} WHERE id=%s", (user_id,))
        if cur.fetchone():
            return True
    return False


def _new_group_id() -> str:
    return 'pg-' + secrets.token_hex(4)


def _new_role_id() -> str:
    return 'role-' + secrets.token_hex(4)


def _dt(v):
    return v.isoformat() if v is not None and hasattr(v, 'isoformat') else v


def _enum(body, key, allowed, default):
    v = body.get(key, default)
    if v is None or v == '':
        v = default
    v = str(v).strip().lower()
    if v not in allowed:
        raise ValueError(f"{key} must be one of {'|'.join(allowed)}")
    return v


def _opt_str(body, key):
    v = body.get(key)
    if v is None:
        return None
    v = str(v).strip()
    return v or None


def _opt_org(body):
    org_id = body.get('org_id')
    return int(org_id) if org_id not in (None, '', 0, '0') else None


def _org_code_of(cur, org_id) -> str:
    if org_id is None:
        return ''
    cur.execute("SELECT code FROM organizations WHERE id=%s", (org_id,))
    r = cur.fetchone()
    return (r['code'] if r else '') or ''


def _out_of_scope(org_code: str, detail: str = None):
    body = {'error': 'out_of_scope', 'org': org_code or ''}
    if detail:
        body['detail'] = detail
    return HandlerResult(status=403, body=body)


def _audit(config, actor: str, ip: str, entity: str, entity_id, action: str, after=None, reason: str = 'console'):
    """E-AUD-006 config_change — actor 는 principal 표기(console:<login> / user:<users.id>, §2.5). 실패는 쓰기를 막지 않는다."""
    try:
        from services import mcptt as _m
        _m.audit_config_change((config or {}).get('CimsDatabase', {}), actor, ip, entity, entity_id, action,
                               after=after, reason=reason)
    except Exception:
        pass


# ──────────────────────────────────────────────────────────────
#  전화 그룹 — 공용 쓰기 코드 (콘솔 /api/v1/phone-groups · 관제 앱 /provisioning/directory/phone-groups)
#    org_codes: None=전 조직, set=범위 안 조직 코드(directory_write=own). 범위 밖은 403 out_of_scope.
# ──────────────────────────────────────────────────────────────

_GROUP_COLS = ("id, name, pilot_id, service_ref, alert_mode, no_answer_sec, busy_members, overflow_target, "
               "org_id, created_at")


def _shape(g: dict, members=None):
    g['no_answer_sec'] = int(g.get('no_answer_sec') or 30)
    g['created_at'] = _dt(g.get('created_at'))
    if members is not None:
        g['members'] = members
    return g


def _pilot_conflict(cur, pilot: str, self_id: str = None):
    """대표번호는 가입 id 주소 공간·다른 대표번호와 겹치면 안 된다 (§8.2) → 409 body 또는 None."""
    if not pilot:
        return None
    for t in _SUB_TABLES:
        cur.execute(f"SELECT 1 FROM {t} WHERE id=%s", (pilot,))
        if cur.fetchone():
            return {'error': 'pilot_conflict', 'detail': f'pilot_id {pilot} is a subscriber id ({t})'}
    cur.execute("SELECT id FROM phone_groups WHERE pilot_id=%s", (pilot,))
    row = cur.fetchone()
    if row and row['id'] != self_id:
        return {'error': 'pilot_conflict', 'detail': f"pilot_id {pilot} already used by group {row['id']}"}
    return None


def _group_org_in_scope(cur, org_codes, org_id) -> bool:
    return authz.in_codes(org_codes, _org_code_of(cur, org_id))


def _person_org_in_scope(cur, org_codes, user_id: str) -> bool:
    """멤버로 넣는 회선의 person 소속 조직도 범위 안이어야 한다(범위 밖 구성원을 내 그룹에 끌어오지 못하게)."""
    if org_codes is None:
        return True
    person = _person_of(cur, user_id)
    if person is None:
        return False
    cur.execute("SELECT org_id FROM users WHERE id=%s", (person,))
    r = cur.fetchone()
    return authz.in_codes(org_codes, (r['org_id'] if r else '') or '')


def _fetch_group_row(cur, group_id: str):
    cur.execute(f"SELECT {_GROUP_COLS} FROM phone_groups WHERE id=%s", (group_id,))
    return cur.fetchone()


def pg_list(cur, org_codes=None, org_id=None) -> list:
    """그룹 목록(멤버 포함). org_codes 가 있으면 범위 안 조직의 그룹만, org_id 는 콘솔 필터."""
    sql = f"SELECT {_GROUP_COLS} FROM phone_groups"
    params = []
    if org_id not in (None, ''):
        sql += " WHERE org_id=%s"
        params.append(org_id)
    sql += " ORDER BY name, id"
    cur.execute(sql, params)
    groups = cur.fetchall()
    cur.execute("SELECT group_id, user_id, alert_order FROM phone_group_members ORDER BY alert_order, user_id")
    mem = {}
    for r in cur.fetchall():
        mem.setdefault(r['group_id'], []).append({'user_id': r['user_id'], 'alert_order': r['alert_order']})
    out = []
    if org_codes is not None:
        id2code = {oid: code for oid, code, _n, _p, _s in authz.org_rows(cur)}
        groups = [g for g in groups if authz.in_codes(org_codes, id2code.get(g.get('org_id'), ''))]
    for g in groups:
        out.append(_shape(g, mem.get(g['id'], [])))
    return out


def pg_get(cur, group_id: str):
    g = _fetch_group_row(cur, group_id)
    if not g:
        return None
    cur.execute("SELECT user_id, alert_order FROM phone_group_members WHERE group_id=%s ORDER BY alert_order, user_id",
                (group_id,))
    return _shape(g, cur.fetchall())


def pg_create(cur, body, org_codes=None) -> HandlerResult:
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    try:
        alert_mode = _enum(body, 'alert_mode', _ALERT_MODES, 'parallel')
        busy = _enum(body, 'busy_members', _BUSY_MODES, 'skip')
        no_answer = int(body.get('no_answer_sec') or 30)
        org_id = _opt_org(body)
    except (ValueError, TypeError) as e:
        return HandlerResult(status=400, body={'error': str(e)})
    if no_answer < 5:
        return HandlerResult(status=400, body={'error': 'no_answer_sec must be >= 5'})
    group_id = _opt_str(body, 'id') or _new_group_id()
    if not group_id.startswith('pg-') or len(group_id) > 64:
        return HandlerResult(status=400, body={'error': "id must start with 'pg-' (max 64)"})
    name = _opt_str(body, 'name') or group_id
    pilot = _opt_str(body, 'pilot_id')
    service_ref = _opt_str(body, 'service_ref')
    overflow = _opt_str(body, 'overflow_target')
    if org_id is not None and not _org_code_of(cur, org_id):
        return HandlerResult(status=400, body={'error': 'unknown_org', 'org_id': org_id})
    if not _group_org_in_scope(cur, org_codes, org_id):
        return _out_of_scope(_org_code_of(cur, org_id), 'group org must be inside the directory_write scope')

    cur.execute("SELECT 1 FROM phone_groups WHERE id=%s", (group_id,))
    if cur.fetchone():
        return HandlerResult(status=409, body={'error': 'group_exists', 'detail': group_id})
    conflict = _pilot_conflict(cur, pilot)
    if conflict:
        return HandlerResult(status=409, body=conflict)
    if pilot and not service_ref:
        return HandlerResult(status=400, body={'error': 'service_ref is required when pilot_id is set'})
    for m in body.get('members') or []:
        uid = ((m.get('user_id') if isinstance(m, dict) else m) or '').strip()
        if uid and _subscriber_exists(cur, uid) and not _person_org_in_scope(cur, org_codes, uid):
            return _out_of_scope('', f'member {uid} belongs to an org outside the scope')

    cur.execute(
        "INSERT INTO phone_groups (id, name, pilot_id, service_ref, alert_mode, no_answer_sec, busy_members, "
        "overflow_target, org_id) VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s)",
        (group_id, name, pilot, service_ref, alert_mode, no_answer, busy, overflow, org_id))
    changed_users = []
    for i, m in enumerate(body.get('members') or []):
        uid = m.get('user_id') if isinstance(m, dict) else m
        uid = (uid or '').strip()
        if not uid or not _subscriber_exists(cur, uid):
            continue
        order = int(m.get('alert_order', i)) if isinstance(m, dict) else i
        cur.execute("INSERT INTO phone_group_members (user_id, group_id, alert_order) VALUES (%s,%s,%s) "
                    "ON DUPLICATE KEY UPDATE group_id=VALUES(group_id), alert_order=VALUES(alert_order)",
                    (uid, group_id, order))
        changed_users = _changed_users(changed_users, [uid], _sync_pickup_group(cur, uid))
    notify_csp("PHONE_GROUP_CHANGED", group_id, "POST")
    for uid in changed_users:
        notify_csp("USER_CHANGED", f"tel:{uid}", "PUT")
    return HandlerResult(status=201, body={'id': group_id})


def pg_update(cur, group_id: str, body, org_codes=None) -> HandlerResult:
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    cur_row = _fetch_group_row(cur, group_id)
    if not cur_row:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if not _group_org_in_scope(cur, org_codes, cur_row.get('org_id')):
        return _out_of_scope(_org_code_of(cur, cur_row.get('org_id')))
    fields, values = [], []
    try:
        if 'name' in body:
            fields.append("name=%s"); values.append(_opt_str(body, 'name') or group_id)
        if 'pilot_id' in body:
            pilot = _opt_str(body, 'pilot_id')
            conflict = _pilot_conflict(cur, pilot, group_id)
            if conflict:
                return HandlerResult(status=409, body=conflict)
            fields.append("pilot_id=%s"); values.append(pilot)
        if 'service_ref' in body:
            fields.append("service_ref=%s"); values.append(_opt_str(body, 'service_ref'))
        if 'alert_mode' in body:
            fields.append("alert_mode=%s"); values.append(_enum(body, 'alert_mode', _ALERT_MODES, 'parallel'))
        if 'no_answer_sec' in body:
            n = int(body.get('no_answer_sec') or 30)
            if n < 5:
                return HandlerResult(status=400, body={'error': 'no_answer_sec must be >= 5'})
            fields.append("no_answer_sec=%s"); values.append(n)
        if 'busy_members' in body:
            fields.append("busy_members=%s"); values.append(_enum(body, 'busy_members', _BUSY_MODES, 'skip'))
        if 'overflow_target' in body:
            fields.append("overflow_target=%s"); values.append(_opt_str(body, 'overflow_target'))
        if 'org_id' in body:
            org_id = _opt_org(body)
            if org_id is not None and not _org_code_of(cur, org_id):
                return HandlerResult(status=400, body={'error': 'unknown_org', 'org_id': org_id})
            if not _group_org_in_scope(cur, org_codes, org_id):
                return _out_of_scope(_org_code_of(cur, org_id), 'target org must be inside the scope')
            fields.append("org_id=%s"); values.append(org_id)
    except (ValueError, TypeError) as e:
        return HandlerResult(status=400, body={'error': str(e)})
    if fields:
        values.append(group_id)
        cur.execute(f"UPDATE phone_groups SET {', '.join(fields)} WHERE id=%s", values)
    notify_csp("PHONE_GROUP_CHANGED", group_id, "PUT")
    return HandlerResult(status=200, body={'id': group_id})


def pg_delete(cur, group_id: str, org_codes=None) -> HandlerResult:
    cur_row = _fetch_group_row(cur, group_id)
    if not cur_row:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if not _group_org_in_scope(cur, org_codes, cur_row.get('org_id')):
        return _out_of_scope(_org_code_of(cur, cur_row.get('org_id')))
    cur.execute("SELECT user_id FROM phone_group_members WHERE group_id=%s", (group_id,))
    users = [r['user_id'] for r in cur.fetchall()]
    cur.execute("DELETE FROM phone_groups WHERE id=%s", (group_id,))
    if cur.rowcount == 0:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    # 멤버 행은 FK CASCADE(role_monitor_targets 도) — 파생 pickup_group 재계산(같은 person 의 PTT 회선 포함, 남는 멤버십 없으면 NULL)
    changed = list(users)
    for uid in users:
        changed = _changed_users(changed, _sync_pickup_group(cur, uid))
    authz.invalidate()
    notify_csp("PHONE_GROUP_CHANGED", group_id, "DELETE")
    for uid in changed:
        notify_csp("USER_CHANGED", f"tel:{uid}", "PUT")
    return HandlerResult(status=200, body={'id': group_id})


def pg_members(cur, group_id: str, org_codes=None) -> HandlerResult:
    cur_row = _fetch_group_row(cur, group_id)
    if cur_row is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if not _group_org_in_scope(cur, org_codes, cur_row.get('org_id')):
        return _out_of_scope(_org_code_of(cur, cur_row.get('org_id')))
    cur.execute("SELECT user_id, alert_order FROM phone_group_members WHERE group_id=%s ORDER BY alert_order, user_id",
                (group_id,))
    return HandlerResult(status=200, body={'group_id': group_id, 'members': cur.fetchall()})


def pg_add_member(cur, group_id: str, body, org_codes=None) -> HandlerResult:
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    user_id = (body.get('user_id') or '').strip()
    if not user_id:
        return HandlerResult(status=400, body={'error': 'user_id is required'})
    cur_row = _fetch_group_row(cur, group_id)
    if cur_row is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if not _group_org_in_scope(cur, org_codes, cur_row.get('org_id')):
        return _out_of_scope(_org_code_of(cur, cur_row.get('org_id')))
    if not _subscriber_exists(cur, user_id):
        return HandlerResult(status=404, body={'error': 'Subscriber not found', 'detail': user_id})
    if not _person_org_in_scope(cur, org_codes, user_id):
        return _out_of_scope('', f'subscriber {user_id} belongs to an org outside the scope')
    # 가입자당 그룹 하나 — 다른 그룹 소속이면 이동(이전 그룹도 재적재 통지). 멤버십 편입에 역할 게이트는 없다(권한이 아니다 §3.1).
    prev = phone_group_of_user(cur, user_id)
    order = int(body.get('alert_order', 0))
    cur.execute("INSERT INTO phone_group_members (user_id, group_id, alert_order) VALUES (%s,%s,%s) "
                "ON DUPLICATE KEY UPDATE group_id=VALUES(group_id), alert_order=VALUES(alert_order)",
                (user_id, group_id, order))
    changed = _changed_users([user_id], _sync_pickup_group(cur, user_id))
    notify_csp("PHONE_GROUP_CHANGED", group_id, "PUT")
    if prev and prev != group_id:
        notify_csp("PHONE_GROUP_CHANGED", prev, "PUT")
    for uid in changed:
        notify_csp("USER_CHANGED", f"tel:{uid}", "PUT")
    return HandlerResult(status=201, body={'group_id': group_id, 'user_id': user_id, 'moved_from': prev})


def pg_remove_member(cur, group_id: str, user_id: str, org_codes=None) -> HandlerResult:
    cur_row = _fetch_group_row(cur, group_id)
    if cur_row is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if not _group_org_in_scope(cur, org_codes, cur_row.get('org_id')):
        return _out_of_scope(_org_code_of(cur, cur_row.get('org_id')))
    cur.execute("DELETE FROM phone_group_members WHERE group_id=%s AND user_id=%s", (group_id, user_id))
    if cur.rowcount == 0:
        return HandlerResult(status=404, body={'error': 'Member not found'})
    changed = _changed_users([user_id], _sync_pickup_group(cur, user_id))
    notify_csp("PHONE_GROUP_CHANGED", group_id, "PUT")
    for uid in changed:
        notify_csp("USER_CHANGED", f"tel:{uid}", "PUT")
    return HandlerResult(status=200, body={'group_id': group_id, 'user_id': user_id})


def dispatch_phone_group(cur, method: str, parts: tuple, body, org_codes=None, org_filter=None) -> HandlerResult:
    """경로 조각(그룹 id / members / user_id) → 전화 그룹 CRUD. 두 façade(콘솔·관제 앱)가 같은 라우팅을 쓴다.
    조회는 org_codes 로 걸러 주고 쓰기는 각 함수가 범위를 판정한다."""
    group_id = parts[0] if len(parts) > 0 else None
    sub = parts[1] if len(parts) > 1 else None
    member_id = parts[2] if len(parts) > 2 else None
    if group_id is None:
        if method == 'GET':
            return HandlerResult(status=200, body={'groups': pg_list(cur, org_codes, org_filter)})
        if method == 'POST':
            return pg_create(cur, body, org_codes)
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
    if sub is None:
        if method == 'GET':
            g = pg_get(cur, group_id)
            if g is None:
                return HandlerResult(status=404, body={'error': 'Group not found'})
            if not _group_org_in_scope(cur, org_codes, g.get('org_id')):
                return _out_of_scope(_org_code_of(cur, g.get('org_id')))
            return HandlerResult(status=200, body=g)
        if method == 'PUT':
            return pg_update(cur, group_id, body, org_codes)
        if method == 'DELETE':
            return pg_delete(cur, group_id, org_codes)
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
    if sub == 'members':
        if member_id is None:
            if method == 'GET':
                return pg_members(cur, group_id, org_codes)
            if method == 'POST':
                return pg_add_member(cur, group_id, body, org_codes)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
        if method == 'DELETE':
            return pg_remove_member(cur, group_id, member_id, org_codes)
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
    return HandlerResult(status=404, body={'error': 'Not Found'})


# ──────────────────────────────────────────────────────────────
#  역할 — /api/v1/roles (전부 authz.manage)
# ──────────────────────────────────────────────────────────────

def _shape_role(r: dict, monitor_targets=None, ptt_targets=None, assignments=None):
    out = dict(r)
    out['created_at'] = _dt(out.get('created_at'))
    if monitor_targets is not None:
        out['monitor_targets'] = monitor_targets
    if ptt_targets is not None:
        out['ptt_targets'] = ptt_targets
    if assignments is not None:
        out['assignment_count'] = assignments
    return out


def _role_row(cur, role_id: str):
    cur.execute(f"SELECT {', '.join(authz.ROLE_COLS)}, created_at FROM roles WHERE id=%s", (role_id,))
    r = cur.fetchone()
    if not r:
        return None
    n = dict(r)
    n.update(authz._norm(r))
    return n


def _role_targets_wire(cur, role_id: str):
    cur.execute("SELECT phone_group_id FROM role_monitor_targets WHERE role_id=%s ORDER BY phone_group_id", (role_id,))
    mon = [r['phone_group_id'] for r in cur.fetchall()]
    cur.execute("SELECT g.mcptt_group_id FROM role_ptt_targets t JOIN ptt_groups g ON g.id=t.ptt_group_id "
                "WHERE t.role_id=%s ORDER BY g.mcptt_group_id", (role_id,))
    ptt = [r['mcptt_group_id'] for r in cur.fetchall()]
    return mon, ptt


def role_list(cur) -> HandlerResult:
    cur.execute(f"SELECT {', '.join(authz.ROLE_COLS)}, created_at FROM roles ORDER BY builtin DESC, id")
    rows = [dict(r, **authz._norm(r)) for r in cur.fetchall()]
    cur.execute("SELECT role_id, phone_group_id FROM role_monitor_targets ORDER BY phone_group_id")
    mon = {}
    for r in cur.fetchall():
        mon.setdefault(r['role_id'], []).append(r['phone_group_id'])
    cur.execute("SELECT t.role_id, g.mcptt_group_id FROM role_ptt_targets t JOIN ptt_groups g ON g.id=t.ptt_group_id "
                "ORDER BY g.mcptt_group_id")
    ptt = {}
    for r in cur.fetchall():
        ptt.setdefault(r['role_id'], []).append(r['mcptt_group_id'])
    cur.execute("SELECT role_id, COUNT(*) AS n FROM role_assignments GROUP BY role_id")
    cnt = {r['role_id']: int(r['n']) for r in cur.fetchall()}
    out = [_shape_role(r, mon.get(r['id'], []), ptt.get(r['id'], []), cnt.get(r['id'], 0)) for r in rows]
    return HandlerResult(status=200, body={'roles': out})


def role_get(cur, role_id: str) -> HandlerResult:
    r = _role_row(cur, role_id)
    if r is None:
        return HandlerResult(status=404, body={'error': 'Role not found'})
    mon, ptt = _role_targets_wire(cur, role_id)
    cur.execute("SELECT COUNT(*) AS n FROM role_assignments WHERE role_id=%s", (role_id,))
    n = int(cur.fetchone()['n'])
    return HandlerResult(status=200, body=_shape_role(r, mon, ptt, n))


def _role_fields(body: dict, base: dict = None):
    """본문 → 저장 필드(enum 검증 + 위임 불가 능력 400 not_delegable). base 가 있으면(PUT) 없는 키는 그대로."""
    for c in authz.NOT_DELEGABLE:
        if body.get(c):
            raise _NotDelegable(c)
    out = dict(base or {})
    for c, allowed in authz.ENUMS.items():
        if c in body or base is None:
            out[c] = _enum(body, c, allowed, out.get(c) or allowed[0])
    if 'org_id' in body or base is None:
        out['org_id'] = _opt_org(body)
    if 'name' in body or base is None:
        out['name'] = _opt_str(body, 'name') or out.get('name') or ''
    return out


class _NotDelegable(Exception):
    pass


def role_create(cur, body) -> HandlerResult:
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    merged = dict(body)
    preset = (body.get('preset') or '').strip().lower()
    if preset:
        if preset not in authz.PRESETS:
            return HandlerResult(status=400, body={'error': f"preset must be one of {'|'.join(authz.PRESETS)}"})
        for k, v in authz.PRESETS[preset].items():
            merged.setdefault(k, v)
    try:
        f = _role_fields(merged)
    except _NotDelegable as e:
        return HandlerResult(status=400, body={'error': 'not_delegable', 'field': str(e),
                                               'detail': 'authz_manage/audit_read/alarm_ack/mcptt_control are builtin-only (mcptt_authorization.md §2.4)'})
    except (ValueError, TypeError) as e:
        return HandlerResult(status=400, body={'error': str(e)})
    role_id = _opt_str(body, 'id') or _new_role_id()
    if role_id in authz.BUILTIN_IDS or not role_id.startswith('role-') or len(role_id) > 64:
        return HandlerResult(status=400, body={'error': "id must start with 'role-' (max 64)"})
    if f['org_id'] is not None and not _org_code_of(cur, f['org_id']):
        return HandlerResult(status=400, body={'error': 'unknown_org', 'org_id': f['org_id']})
    cur.execute("SELECT 1 FROM roles WHERE id=%s", (role_id,))
    if cur.fetchone():
        return HandlerResult(status=409, body={'error': 'role_exists', 'detail': role_id})
    cur.execute(
        "INSERT INTO roles (id, name, builtin, authz_manage, audit_read, directory_write, directory_read, ptt_group_manage, "
        "monitor_call, ptt_listen, listen_visibility, history_read, alarm_ack, mcptt_control, org_id) "
        "VALUES (%s,%s,0,0,0,%s,%s,%s,%s,%s,%s,%s,0,0,%s)",
        (role_id, f['name'] or role_id, f['directory_write'], f['directory_read'], f['ptt_group_manage'],
         f['monitor_call'], f['ptt_listen'], f['listen_visibility'], f['history_read'], f['org_id']))
    authz.invalidate()
    notify_csp("ROLE_CHANGED", role_id, "POST")
    return HandlerResult(status=201, body={'id': role_id})


def role_update(cur, role_id: str, body) -> HandlerResult:
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    cur_row = _role_row(cur, role_id)
    if cur_row is None:
        return HandlerResult(status=404, body={'error': 'Role not found'})
    if cur_row.get('builtin'):
        return HandlerResult(status=403, body={'error': 'builtin', 'detail': 'builtin roles are read-only'})
    try:
        f = _role_fields(body, cur_row)
    except _NotDelegable as e:
        return HandlerResult(status=400, body={'error': 'not_delegable', 'field': str(e)})
    except (ValueError, TypeError) as e:
        return HandlerResult(status=400, body={'error': str(e)})
    if f['org_id'] is not None and not _org_code_of(cur, f['org_id']):
        return HandlerResult(status=400, body={'error': 'unknown_org', 'org_id': f['org_id']})
    cur.execute(
        "UPDATE roles SET name=%s, directory_write=%s, directory_read=%s, ptt_group_manage=%s, monitor_call=%s, "
        "ptt_listen=%s, listen_visibility=%s, history_read=%s, org_id=%s WHERE id=%s",
        (f['name'] or role_id, f['directory_write'], f['directory_read'], f['ptt_group_manage'], f['monitor_call'],
         f['ptt_listen'], f['listen_visibility'], f['history_read'], f['org_id'], role_id))
    authz.invalidate()
    # 청취 범위가 생기거나 없어지면 배정자 전원의 자격을 다시 맞춘다(§2.4 — 자격은 배정의 결과)
    changed = []
    if (cur_row.get('ptt_listen') != 'none') != (f['ptt_listen'] != 'none'):
        cur.execute("SELECT principal_id FROM role_assignments WHERE role_id=%s AND principal_type='user'", (role_id,))
        for r in cur.fetchall():
            changed += sync_ambient_listening(cur, r['principal_id'])
    notify_csp("ROLE_CHANGED", role_id, "PUT")
    return HandlerResult(status=200, body={'id': role_id, 'ambient_synced': changed})


def role_delete(cur, role_id: str) -> HandlerResult:
    cur_row = _role_row(cur, role_id)
    if cur_row is None:
        return HandlerResult(status=404, body={'error': 'Role not found'})
    if cur_row.get('builtin'):
        return HandlerResult(status=403, body={'error': 'builtin', 'detail': 'builtin roles are read-only'})
    cur.execute("SELECT COUNT(*) AS n FROM role_assignments WHERE role_id=%s", (role_id,))
    if int(cur.fetchone()['n']) > 0:
        return HandlerResult(status=409, body={'error': 'assigned', 'detail': 'unassign all principals first'})
    cur.execute("DELETE FROM roles WHERE id=%s", (role_id,))          # 대상 표는 FK CASCADE
    authz.invalidate()
    notify_csp("ROLE_CHANGED", role_id, "DELETE")
    return HandlerResult(status=200, body={'id': role_id})


def role_put_monitor_targets(cur, role_id: str, body) -> HandlerResult:
    if not isinstance(body, dict) or not isinstance(body.get('phone_group_ids'), list):
        return HandlerResult(status=400, body={'error': 'phone_group_ids (array) required'})
    cur_row = _role_row(cur, role_id)
    if cur_row is None:
        return HandlerResult(status=404, body={'error': 'Role not found'})
    if cur_row.get('builtin'):
        return HandlerResult(status=403, body={'error': 'builtin'})
    targets = []
    for t in body['phone_group_ids']:
        t = str(t).strip()
        if not t or t in targets:
            continue
        cur.execute("SELECT 1 FROM phone_groups WHERE id=%s", (t,))
        if cur.fetchone() is None:
            return HandlerResult(status=400, body={'error': f'unknown phone group: {t}'})
        targets.append(t)
    cur.execute("DELETE FROM role_monitor_targets WHERE role_id=%s", (role_id,))
    for t in targets:
        cur.execute("INSERT IGNORE INTO role_monitor_targets (role_id, phone_group_id) VALUES (%s,%s)", (role_id, t))
    authz.invalidate()
    notify_csp("ROLE_CHANGED", role_id, "PUT")
    return HandlerResult(status=200, body={'id': role_id, 'phone_group_ids': targets})


def role_put_ptt_targets(cur, role_id: str, body) -> HandlerResult:
    if not isinstance(body, dict) or not isinstance(body.get('ptt_group_ids'), list):
        return HandlerResult(status=400, body={'error': 'ptt_group_ids (array of mcptt_group_id) required'})
    cur_row = _role_row(cur, role_id)
    if cur_row is None:
        return HandlerResult(status=404, body={'error': 'Role not found'})
    if cur_row.get('builtin'):
        return HandlerResult(status=403, body={'error': 'builtin'})
    pks = []
    for gid in body['ptt_group_ids']:
        gid = str(gid).strip()
        if not gid or any(g == gid for _pk, g in pks):
            continue
        cur.execute("SELECT id FROM ptt_groups WHERE mcptt_group_id=%s", (gid,))
        r = cur.fetchone()
        if r is None:
            return HandlerResult(status=400, body={'error': f'unknown ptt group: {gid}'})
        pks.append((r['id'], gid))
    cur.execute("DELETE FROM role_ptt_targets WHERE role_id=%s", (role_id,))
    for pk, _ in pks:
        cur.execute("INSERT IGNORE INTO role_ptt_targets (role_id, ptt_group_id) VALUES (%s,%s)", (role_id, pk))
    authz.invalidate()
    notify_csp("ROLE_CHANGED", role_id, "PUT")
    return HandlerResult(status=200, body={'id': role_id, 'ptt_group_ids': [g for _, g in pks]})


def sync_ambient_listening(cur, person_id) -> list:
    """청취 자격 동기(§2.4) — person 의 PTT 전 회선 ptt_user_profile.allow_ambient_listening 을
    (배정 역할의 ptt_listen ≠ none) 으로 맞춘다. 컬럼 미적용 DB 는 no-op. 값이 바뀐 회선(msisdn) 목록을 돌려주고,
    그 회선마다 프로파일 캐시(user-profile 문서)를 갱신하고 USER_CHANGED 를 보낸다(CSP 는 프로파일에서 자격을 읽는다)."""
    cur.execute("SHOW COLUMNS FROM ptt_user_profile LIKE 'allow_ambient_listening'")
    if cur.fetchone() is None:
        return []
    role = authz.load_role(cur, authz.user_role_id(cur, person_id))
    want = 1 if role and (role.get('ptt_listen') or 'none') != 'none' else 0
    cur.execute("SELECT id FROM ptt_subscriptions WHERE user_id=%s ORDER BY id", (person_id,))
    lines = [r['id'] for r in cur.fetchall()]
    changed = []
    for msisdn in lines:
        cur.execute("SELECT allow_ambient_listening FROM ptt_user_profile WHERE ptt_id=%s", (msisdn,))
        row = cur.fetchone()
        if row is None and want == 0:
            continue                                   # 행 없음 = 자격 없음(기본 0) — 만들 이유가 없다
        if row is not None and int(row['allow_ambient_listening'] or 0) == want:
            continue
        cur.execute("INSERT INTO ptt_user_profile (ptt_id, allow_ambient_listening, update_time) VALUES (%s,%s,NOW()) "
                    "ON DUPLICATE KEY UPDATE allow_ambient_listening=VALUES(allow_ambient_listening), update_time=NOW()",
                    (msisdn, want))
        changed.append(msisdn)
    for msisdn in changed:
        try:
            from services import mcptt as _m
            prof = dict(_m.get_user_profile(msisdn) or {})
            prof['allow_ambient_listening'] = bool(want)
            _m.update_user_profile_cache(msisdn, prof)
        except Exception:
            pass
        notify_csp("USER_CHANGED", f"tel:{msisdn}", "PUT")
    return changed


def role_assignments_get(cur, role_id: str) -> HandlerResult:
    if _role_row(cur, role_id) is None:
        return HandlerResult(status=404, body={'error': 'Role not found'})
    cur.execute("SELECT principal_type, principal_id FROM role_assignments WHERE role_id=%s "
                "ORDER BY principal_type, principal_id", (role_id,))
    rows = cur.fetchall()
    out = []
    for r in rows:
        name = ''
        if r['principal_type'] == 'user':
            cur.execute("SELECT name FROM users WHERE id=%s", (r['principal_id'],))
            u = cur.fetchone()
            name = (u['name'] if u else '') or ''
        out.append({'principal_type': r['principal_type'], 'principal_id': r['principal_id'], 'name': name})
    # 콘솔 계정의 배정은 OAM file_store console_accounts[].role — CSC 는 그 store 를 읽지 않으므로 여기에는 user 행만 실린다.
    return HandlerResult(status=200, body={'role_id': role_id, 'assignments': out})


def _principal_from_body(body):
    if not isinstance(body, dict):
        return None, HandlerResult(status=400, body={'error': 'JSON body required'})
    ptype = (body.get('principal_type') or 'user').strip().lower()
    pid = str(body.get('principal_id') or '').strip()
    if ptype != 'user':
        return None, HandlerResult(status=400, body={'error': 'invalid_principal_type',
                                                     'detail': "console accounts are assigned via OAM PUT /api/v1/console-accounts/{login_id} role"})
    if not pid:
        return None, HandlerResult(status=400, body={'error': 'principal_id is required'})
    return (ptype, pid), None


def role_assign(cur, role_id: str, body) -> HandlerResult:
    pr, err = _principal_from_body(body)
    if err:
        return err
    ptype, pid = pr
    if _role_row(cur, role_id) is None:
        return HandlerResult(status=404, body={'error': 'Role not found'})
    cur.execute("SELECT id, name FROM users WHERE id=%s", (pid,))
    u = cur.fetchone()
    if not u:
        return HandlerResult(status=404, body={'error': 'User not found', 'detail': pid})
    cur.execute("SELECT role_id FROM role_assignments WHERE principal_type=%s AND principal_id=%s", (ptype, pid))
    r = cur.fetchone()
    prev = r['role_id'] if r else None
    cur.execute("INSERT INTO role_assignments (principal_type, principal_id, role_id) VALUES (%s,%s,%s) "
                "ON DUPLICATE KEY UPDATE role_id=VALUES(role_id)", (ptype, pid, role_id))
    authz.invalidate()
    synced = sync_ambient_listening(cur, pid)
    notify_csp("ROLE_CHANGED", pid, "PUT")
    return HandlerResult(status=200, body={'role_id': role_id, 'principal_type': ptype, 'principal_id': pid,
                                           'moved_from': prev if prev != role_id else None, 'ambient_synced': synced})


def role_unassign(cur, role_id: str, ptype: str, pid: str) -> HandlerResult:
    ptype = (ptype or '').strip().lower()
    if ptype != 'user':
        return HandlerResult(status=400, body={'error': 'invalid_principal_type'})
    cur.execute("DELETE FROM role_assignments WHERE role_id=%s AND principal_type=%s AND principal_id=%s",
                (role_id, ptype, pid))
    if cur.rowcount == 0:
        return HandlerResult(status=404, body={'error': 'Assignment not found'})
    authz.invalidate()
    synced = sync_ambient_listening(cur, pid)
    notify_csp("ROLE_CHANGED", pid, "DELETE")
    return HandlerResult(status=200, body={'role_id': role_id, 'principal_type': ptype, 'principal_id': pid,
                                           'ambient_synced': synced})


# ──────────────────────────────────────────────────────────────
#  Handlers (콘솔 토큰)
# ──────────────────────────────────────────────────────────────

def _console_gate(handler_args, cur, capability: str):
    """콘솔 JWT → principal → can(). (principal, role, err)."""
    payload, err = admin_auth.require_role(handler_args, 'monitor')      # 인증 + 콘솔 계정(로그인 가능 계층)
    if err:
        return None, None, err
    principal = authz.console_principal(payload)
    role = authz.role_of(cur, principal)
    ok, reason = authz.can(principal, capability, cur=cur)
    if not ok:
        return principal, role, authz.forbidden(capability, reason)
    return principal, role, None


def _wrap_scope_error(r: HandlerResult, capability: str, role) -> HandlerResult:
    """공용 쓰기 코드의 403 out_of_scope 를 콘솔 계약(forbidden {capability, scope}) 으로."""
    if r.status == 403 and isinstance(r.body, dict) and r.body.get('error') == 'out_of_scope':
        field = authz._CAPS[capability][0]
        return authz.forbidden(capability, (role or {}).get(field) or 'none', org=r.body.get('org', ''))
    return r


async def handle_phone_groups(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """/api/v1/phone-groups/* — 조회 directory.read, 쓰기 directory.write(범위 안 조직의 그룹). 감사 E-AUD-006(phone_group)."""
    config = kwargs.get('config', {})
    parts = _path_parts(handler_args.full_path, _PG_BASE)
    method = handler_args.method.upper()
    capability = 'directory.read' if method == 'GET' else 'directory.write'
    ip = getattr(handler_args, 'client_ip', '') or ''
    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                if not has_phone_group_tables(cur):
                    payload, err = admin_auth.require_role(handler_args, 'monitor')
                    if err:
                        return err
                    return HandlerResult(status=400 if method != 'GET' else 200,
                                         body=_SCHEMA_ERROR if method != 'GET' else {'groups': [], 'schema': 'not_migrated'})
                principal, role, err = _console_gate(handler_args, cur, capability)
                if err:
                    return err
                _mode, _root, org_codes = authz.org_scope(cur, role, authz._CAPS[capability][0])
                r = dispatch_phone_group(cur, method, parts, handler_args.body, org_codes, _query(handler_args).get('org_id'))
                r = _wrap_scope_error(r, capability, role)
                if method != 'GET' and r.status in (200, 201):
                    gid = parts[0] if parts else (r.body or {}).get('id')
                    action = {'POST': 'create', 'PUT': 'update', 'DELETE': 'delete'}.get(method, method.lower())
                    if len(parts) > 1:
                        action = f"member_{'add' if method == 'POST' else 'remove'}"
                    _audit(config, authz.actor(principal), ip, 'phone_group', gid, action,
                           after=handler_args.body if isinstance(handler_args.body, dict) else None)
                return r
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


async def handle_roles(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """/api/v1/roles/* — 전부 authz.manage(내장 admin/manager). 감사 E-AUD-006(role | role_assignment)."""
    config = kwargs.get('config', {})
    parts = _path_parts(handler_args.full_path, _ROLE_BASE)
    role_id = parts[0] if len(parts) > 0 else None
    sub = parts[1] if len(parts) > 1 else None          # monitor-targets | ptt-targets | assignments
    method = handler_args.method.upper()
    body = handler_args.body
    ip = getattr(handler_args, 'client_ip', '') or ''
    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                if not authz.has_roles_tables(cur):
                    payload, err = admin_auth.require_role(handler_args, 'manager')
                    if err:
                        return err
                    if method == 'GET' and role_id is None:
                        return HandlerResult(status=200, body={'roles': [_shape_role(r, [], [], 0) for r in authz.BUILTIN_ROLES.values()],
                                                               'schema': 'not_migrated'})
                    return HandlerResult(status=400, body=_SCHEMA_ERROR)
                principal, _role, err = _console_gate(handler_args, cur, 'authz.manage')
                if err:
                    return err
                actor = authz.actor(principal)

                if role_id is None:
                    if method == 'GET':
                        return role_list(cur)
                    if method == 'POST':
                        r = role_create(cur, body)
                        if r.status == 201:
                            _audit(config, actor, ip, 'role', r.body['id'], 'create', after=body)
                        return r
                    return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

                if sub is None:
                    if method == 'GET':
                        return role_get(cur, role_id)
                    if method == 'PUT':
                        r = role_update(cur, role_id, body)
                        if r.status == 200:
                            _audit(config, actor, ip, 'role', role_id, 'update', after=body)
                        return r
                    if method == 'DELETE':
                        r = role_delete(cur, role_id)
                        if r.status == 200:
                            _audit(config, actor, ip, 'role', role_id, 'delete')
                        return r
                    return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

                if sub in ('monitor-targets', 'ptt-targets'):
                    if method != 'PUT' or len(parts) > 2:
                        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
                    r = role_put_monitor_targets(cur, role_id, body) if sub == 'monitor-targets' \
                        else role_put_ptt_targets(cur, role_id, body)
                    if r.status == 200:
                        _audit(config, actor, ip, 'role', role_id, sub.replace('-', '_'), after=body)
                    return r

                if sub == 'assignments':
                    if len(parts) == 2:
                        if method == 'GET':
                            return role_assignments_get(cur, role_id)
                        if method == 'PUT':
                            r = role_assign(cur, role_id, body)
                            if r.status == 200:
                                _audit(config, actor, ip, 'role_assignment', f"{r.body['principal_type']}:{r.body['principal_id']}",
                                       'assign', after={'role_id': role_id, 'moved_from': r.body.get('moved_from')})
                            return r
                        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
                    if len(parts) == 4 and method == 'DELETE':
                        r = role_unassign(cur, role_id, parts[2], parts[3])
                        if r.status == 200:
                            _audit(config, actor, ip, 'role_assignment', f"{parts[2]}:{parts[3]}", 'unassign',
                                   after={'role_id': role_id})
                        return r
                    return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
                return HandlerResult(status=404, body={'error': 'Not Found'})
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


CIMS_DISPATCH_HANDLER_LIST = [
    (_PG_BASE, handle_phone_groups, {}),
    (_ROLE_BASE, handle_roles, {}),
]


# ── API 문서 (개발자 모드) ──────────────────────────────────────────────────
#  이 모듈이 제공하는 엔드포인트의 자기기술. csc handlers/api_docs.py 가 수집한다.
#  경로/파라미터를 바꾸면 **여기도 같은 커밋에서** 갱신한다.
_AUTH_MONITOR = {'scheme': 'bearer', 'role': 'monitor', 'token_from': 'POST /api/v1/auth/login',
                 'capability': 'directory.read'}
_AUTH_WRITE = {'scheme': 'bearer', 'role': 'manager', 'token_from': 'POST /api/v1/auth/login',
               'capability': 'directory.write'}
_AUTH_AUTHZ = {'scheme': 'bearer', 'role': 'manager', 'token_from': 'POST /api/v1/auth/login',
               'capability': 'authz.manage'}
_ERR_COMMON = [
    {'status': 401, 'when': 'Authorization 헤더 없음 / 토큰 만료', 'body': {'error': 'unauthorized'}},
    {'status': 403, 'when': '능력 없음 / 범위 밖', 'body': {'error': 'forbidden', 'capability': '…', 'scope': '…'}},
]
_ERR_SCHEMA = {'status': 400, 'when': 'phone_groups/roles 테이블 미적용', 'body': _SCHEMA_ERROR}
_PG_FIELDS = [
    {'name': 'id', 'type': 'string', 'desc': '불변 키 pg-xxxxxxxx(전환 전 dg-… 유지) — *_subscriptions.pickup_group 값·상관 키'},
    {'name': 'name', 'type': 'string', 'desc': '표시 이름 (키에 쓰지 않는다)'},
    {'name': 'pilot_id', 'type': 'string|null', 'desc': '대표번호(AoR user part). null=대표번호 없음(순수 당겨받기 그룹)'},
    {'name': 'service_ref', 'type': 'string|null', 'desc': '대표번호 접속서비스 name(유선 VoIP) — 도메인·SRTP 정책 근거'},
    {'name': 'alert_mode', 'type': 'string', 'desc': 'parallel(기본)|sequential — TS 24.239 Flexible Alerting'},
    {'name': 'no_answer_sec', 'type': 'integer', 'desc': '전원 무응답 판정 초 (기본 30, CSP Setup.Sip.Dispatch.ForkRingTimeoutSec 로 clamp)'},
    {'name': 'busy_members', 'type': 'string', 'desc': 'skip(기본)|alert — 통화 중 그룹원 호출 여부'},
    {'name': 'overflow_target', 'type': 'string|null', 'desc': '무응답 넘김 대상(대표번호/내선). null=480'},
    {'name': 'org_id', 'type': 'integer|null', 'desc': '소속 조직 — directory_write=own 범위 판정 축'},
    {'name': 'members[]', 'type': 'object', 'desc': '{user_id, alert_order} — 가입자당 그룹 하나(유선 회선만)'},
]
_PG_EXAMPLE = {'id': 'pg-7f3a91c2', 'name': '관제 1반', 'pilot_id': '+821310001000', 'service_ref': 'voip',
               'alert_mode': 'parallel', 'no_answer_sec': 30, 'busy_members': 'skip', 'overflow_target': None, 'org_id': 1,
               'members': [{'user_id': '+821310001001', 'alert_order': 0}]}
_ROLE_FIELDS = [
    {'name': 'id', 'type': 'string', 'desc': '불변 키 — 내장 admin|manager|operator|monitor, 커스텀 role-xxxxxxxx'},
    {'name': 'name', 'type': 'string', 'desc': '표시 이름'},
    {'name': 'builtin', 'type': 'boolean', 'desc': '내장 프리셋(읽기 전용)'},
    {'name': 'authz_manage', 'type': 'boolean', 'desc': '역할·배정·범위 관리 — 내장 admin/manager 만(커스텀 400 not_delegable)'},
    {'name': 'audit_read', 'type': 'boolean', 'desc': 'E-AUD 감사 이벤트 열람(커스텀 불가)'},
    {'name': 'directory_write', 'type': 'string', 'desc': 'none|own|all — 조직/구성원/번호/전화 그룹 관리 범위(own=org_id 하위)'},
    {'name': 'directory_read', 'type': 'string', 'desc': 'none|own|all'},
    {'name': 'ptt_group_manage', 'type': 'string', 'desc': 'none|own|scope|all — PTT 그룹 CRUD(own=본인 소유, scope=directory_write 범위)'},
    {'name': 'monitor_call', 'type': 'string', 'desc': 'none|own|listed|all — 통화 감청·세션 관측·통화 이력/녹취 범위'},
    {'name': 'ptt_listen', 'type': 'string', 'desc': 'none|listed|all — PTT 청취·conference 구독·PTT 이력/녹취 범위'},
    {'name': 'listen_visibility', 'type': 'string', 'desc': 'hidden|visible — PTT 청취 로스터 노출'},
    {'name': 'history_read', 'type': 'string', 'desc': 'none|scope|all'},
    {'name': 'alarm_ack', 'type': 'boolean', 'desc': '알람 ack(커스텀 불가)'},
    {'name': 'mcptt_control', 'type': 'boolean', 'desc': 'MCPTT 관제(커스텀 불가)'},
    {'name': 'org_id', 'type': 'integer|null', 'desc': 'own 범위의 루트'},
    {'name': 'monitor_targets[]', 'type': 'string', 'desc': 'monitor_call=listed 대상 전화 그룹 id'},
    {'name': 'ptt_targets[]', 'type': 'string', 'desc': 'ptt_listen=listed 대상 PTT 그룹(mcptt_group_id)'},
    {'name': 'assignment_count', 'type': 'integer', 'desc': '배정(user) 수'},
]
_ROLE_EXAMPLE = {'id': 'role-1a2b3c4d', 'name': '관제 1조 전체', 'builtin': False, 'authz_manage': False, 'audit_read': False,
                 'directory_write': 'own', 'directory_read': 'none', 'ptt_group_manage': 'scope', 'monitor_call': 'own',
                 'ptt_listen': 'listed', 'listen_visibility': 'hidden', 'history_read': 'scope', 'alarm_ack': False,
                 'mcptt_control': False, 'org_id': 3, 'monitor_targets': [], 'ptt_targets': ['g002'], 'assignment_count': 2}

CIMS_DISPATCH_API_DOCS = [
    {'id': 'csc.phone-groups.list', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/phone-groups',
     'summary': '전화 그룹 목록 (멤버 포함)',
     'params': [{'name': 'org_id', 'in': 'query', 'type': 'integer', 'required': False, 'desc': '조직 필터'}],
     'response': '{groups[]}',
     'response_fields': [{'name': 'groups[].' + f['name'], **{k: v for k, v in f.items() if k != 'name'}} for f in _PG_FIELDS],
     'example': {'groups': [_PG_EXAMPLE]}, 'errors': list(_ERR_COMMON),
     'notes': ['phone_groups 테이블 미적용 DB 에서는 빈 목록 + schema=not_migrated 를 돌려준다.',
               'directory_read=own 이면 범위 안 조직의 그룹만.'],
     'auth': dict(_AUTH_MONITOR)},
    {'id': 'csc.phone-groups.get', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/phone-groups/{id}',
     'summary': '전화 그룹 1건',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id (pg-…)'}],
     'response': '그룹 객체', 'response_fields': list(_PG_FIELDS), 'example': dict(_PG_EXAMPLE),
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 그룹', 'body': {'error': 'Group not found'}}],
     'notes': [], 'auth': dict(_AUTH_MONITOR)},
    {'id': 'csc.phone-groups.create', 'module': 'csc', 'method': 'POST', 'path': '/api/v1/phone-groups',
     'summary': '전화 그룹 생성 (id 미지정 시 pg-<hex8> 발급)',
     'params': [{'name': 'body', 'in': 'body', 'type': 'object', 'required': True,
                 'desc': '{id?, name, pilot_id?, service_ref?(pilot 시 필수 — 유선 VoIP 서비스), alert_mode?, no_answer_sec?, '
                         'busy_members?, overflow_target?, org_id?, members?[{user_id, alert_order}]}'}],
     'response': '{id}', 'response_fields': [{'name': 'id', 'type': 'string', 'desc': '생성된 그룹 id'}],
     'example': {'id': 'pg-7f3a91c2'},
     'errors': _ERR_COMMON + [_ERR_SCHEMA,
                              {'status': 409, 'when': 'pilot_id 가 가입 id·다른 대표번호와 충돌', 'body': {'error': 'pilot_conflict'}},
                              {'status': 409, 'when': 'id 중복', 'body': {'error': 'group_exists'}}],
     'notes': ['성공 시 **201**.',
               '멤버 편입은 가입자 pickup_group 을 그룹 id 로 파생 갱신하고 USER_CHANGED 를 보낸다 — 같은 person 의 다른 회선(관제사 PTT 회선)도 물려받는다(멤버 행은 유선 회선 하나).',
               'CSP 에는 PHONE_GROUP_CHANGED(uri=그룹 id) 로 재적재를 알린다.'],
     'auth': dict(_AUTH_WRITE)},
    {'id': 'csc.phone-groups.update', 'module': 'csc', 'method': 'PUT', 'path': '/api/v1/phone-groups/{id}',
     'summary': '전화 그룹 수정 (부분 갱신)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id'},
                {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '변경할 필드만'}],
     'response': '{id}', 'response_fields': [{'name': 'id', 'type': 'string', 'desc': '그룹 id'}],
     'example': {'id': 'pg-7f3a91c2'},
     'errors': _ERR_COMMON + [_ERR_SCHEMA, {'status': 404, 'when': '없는 그룹', 'body': {'error': 'Group not found'}},
                              {'status': 409, 'when': 'pilot_id 충돌', 'body': {'error': 'pilot_conflict'}}],
     'notes': [], 'auth': dict(_AUTH_WRITE)},
    {'id': 'csc.phone-groups.delete', 'module': 'csc', 'method': 'DELETE', 'path': '/api/v1/phone-groups/{id}',
     'summary': '전화 그룹 삭제 (멤버 pickup_group 해제)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id'}],
     'response': '{id}', 'response_fields': [{'name': 'id', 'type': 'string', 'desc': '그룹 id'}],
     'example': {'id': 'pg-7f3a91c2'},
     'errors': _ERR_COMMON + [_ERR_SCHEMA, {'status': 404, 'when': '없는 그룹', 'body': {'error': 'Group not found'}}],
     'notes': ['멤버(와 같은 person 의 파생 회선)의 pickup_group 은 NULL 로 돌아간다(픽업·BLF 축 없음). 역할의 monitor 대상에서도 빠진다(FK CASCADE).'],
     'auth': dict(_AUTH_WRITE)},
    {'id': 'csc.phone-groups.members.list', 'module': 'csc', 'method': 'GET',
     'path': '/api/v1/phone-groups/{id}/members', 'summary': '전화 그룹 멤버 목록',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id'}],
     'response': '{group_id, members[{user_id, alert_order}]}',
     'response_fields': [{'name': 'members[].user_id', 'type': 'string', 'desc': '가입자(유선 회선) id'},
                         {'name': 'members[].alert_order', 'type': 'integer', 'desc': 'sequential 호출·절삭 순서'}],
     'example': {'group_id': 'pg-7f3a91c2', 'members': [{'user_id': '+821310001001', 'alert_order': 0}]},
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 그룹', 'body': {'error': 'Group not found'}}],
     'notes': [], 'auth': dict(_AUTH_MONITOR)},
    {'id': 'csc.phone-groups.members.add', 'module': 'csc', 'method': 'POST',
     'path': '/api/v1/phone-groups/{id}/members', 'summary': '멤버 추가/이동 (가입자당 그룹 하나)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id'},
                {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '{user_id(필수), alert_order?}'}],
     'response': '{group_id, user_id, moved_from}',
     'response_fields': [{'name': 'moved_from', 'type': 'string|null', 'desc': '다른 그룹에서 이동했으면 이전 그룹 id'}],
     'example': {'group_id': 'pg-7f3a91c2', 'user_id': '+821310001001', 'moved_from': None},
     'errors': _ERR_COMMON + [_ERR_SCHEMA, {'status': 404, 'when': '없는 그룹/가입자'}],
     'notes': ['성공 시 **201**.', '멤버십은 권한이 아니다 — 감청·청취는 역할(/api/v1/roles)로 따로 배정한다.',
               '가입자 pickup_group 이 그룹 id 로 갱신된다 — 같은 person 의 PTT 회선도 파생으로 물려받는다. 픽업 축 반영은 다음 REGISTER 갱신부터.'],
     'auth': dict(_AUTH_WRITE)},
    {'id': 'csc.phone-groups.members.remove', 'module': 'csc', 'method': 'DELETE',
     'path': '/api/v1/phone-groups/{id}/members/{user_id}', 'summary': '멤버 제거 (pickup_group 해제)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id'},
                {'name': 'user_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '가입자 id (URL 인코딩, +→%2B)'}],
     'response': '{group_id, user_id}', 'response_fields': [],
     'example': {'group_id': 'pg-7f3a91c2', 'user_id': '+821310001001'},
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 멤버', 'body': {'error': 'Member not found'}}],
     'notes': [], 'auth': dict(_AUTH_WRITE)},
    {'id': 'csc.roles.list', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/roles',
     'summary': '역할 목록 (내장 4 + 커스텀, 대상·배정 수 포함)', 'params': [],
     'response': '{roles[]}',
     'response_fields': [{'name': 'roles[].' + f['name'], **{k: v for k, v in f.items() if k != 'name'}} for f in _ROLE_FIELDS],
     'example': {'roles': [_ROLE_EXAMPLE]}, 'errors': list(_ERR_COMMON),
     'notes': ['roles 테이블 미적용 DB 에서는 내장 4행 + schema=not_migrated.'], 'auth': dict(_AUTH_AUTHZ)},
    {'id': 'csc.roles.create', 'module': 'csc', 'method': 'POST', 'path': '/api/v1/roles',
     'summary': '역할 생성 (id 미지정 시 role-<hex8>)',
     'params': [{'name': 'body', 'in': 'body', 'type': 'object', 'required': True,
                 'desc': '{id?, name, preset?(supervisor|admin|full), directory_write?, directory_read?, ptt_group_manage?, '
                         'monitor_call?, ptt_listen?, listen_visibility?, history_read?, org_id?}'}],
     'response': '{id}', 'response_fields': [{'name': 'id', 'type': 'string', 'desc': '생성된 역할 id'}],
     'example': {'id': 'role-1a2b3c4d'},
     'errors': _ERR_COMMON + [_ERR_SCHEMA,
                              {'status': 400, 'when': 'authz_manage/audit_read/alarm_ack/mcptt_control 를 켬', 'body': {'error': 'not_delegable'}},
                              {'status': 409, 'when': 'id 중복', 'body': {'error': 'role_exists'}}],
     'notes': ['성공 시 **201**.', 'preset 은 초깃값 — 본문 필드가 우선한다.'], 'auth': dict(_AUTH_AUTHZ)},
    {'id': 'csc.roles.get', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/roles/{id}', 'summary': '역할 1건',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '역할 id'}],
     'response': '역할 객체', 'response_fields': list(_ROLE_FIELDS), 'example': dict(_ROLE_EXAMPLE),
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 역할', 'body': {'error': 'Role not found'}}],
     'notes': [], 'auth': dict(_AUTH_AUTHZ)},
    {'id': 'csc.roles.update', 'module': 'csc', 'method': 'PUT', 'path': '/api/v1/roles/{id}', 'summary': '역할 수정 (부분 갱신)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '역할 id'},
                {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '변경할 필드만'}],
     'response': '{id, ambient_synced[]}', 'response_fields': [],
     'example': {'id': 'role-1a2b3c4d', 'ambient_synced': []},
     'errors': _ERR_COMMON + [{'status': 403, 'when': '내장 역할', 'body': {'error': 'builtin'}},
                              {'status': 400, 'when': '위임 불가 능력', 'body': {'error': 'not_delegable'}},
                              {'status': 404, 'when': '없는 역할'}],
     'notes': ['ptt_listen 이 none↔그 외로 바뀌면 배정자 전원의 allow_ambient_listening 을 다시 맞춘다.'], 'auth': dict(_AUTH_AUTHZ)},
    {'id': 'csc.roles.delete', 'module': 'csc', 'method': 'DELETE', 'path': '/api/v1/roles/{id}', 'summary': '역할 삭제',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '역할 id'}],
     'response': '{id}', 'response_fields': [], 'example': {'id': 'role-1a2b3c4d'},
     'errors': _ERR_COMMON + [{'status': 403, 'when': '내장 역할', 'body': {'error': 'builtin'}},
                              {'status': 409, 'when': '배정이 남아 있음', 'body': {'error': 'assigned'}},
                              {'status': 404, 'when': '없는 역할'}],
     'notes': [], 'auth': dict(_AUTH_AUTHZ)},
    {'id': 'csc.roles.monitor-targets.put', 'module': 'csc', 'method': 'PUT', 'path': '/api/v1/roles/{id}/monitor-targets',
     'summary': '감청 대상 전화 그룹 목록 교체 (monitor_call=listed)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '역할 id'},
                {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '{phone_group_ids: [pg-…]}'}],
     'response': '{id, phone_group_ids[]}', 'response_fields': [],
     'example': {'id': 'role-1a2b3c4d', 'phone_group_ids': ['pg-0a1b2c3d']},
     'errors': _ERR_COMMON + [{'status': 400, 'when': '없는 전화 그룹'}, {'status': 404, 'when': '없는 역할'}],
     'notes': [], 'auth': dict(_AUTH_AUTHZ)},
    {'id': 'csc.roles.ptt-targets.put', 'module': 'csc', 'method': 'PUT', 'path': '/api/v1/roles/{id}/ptt-targets',
     'summary': 'PTT 청취 대상 그룹 목록 교체 (ptt_listen=listed)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '역할 id'},
                {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '{ptt_group_ids: [mcptt_group_id…]}'}],
     'response': '{id, ptt_group_ids[]}', 'response_fields': [],
     'example': {'id': 'role-1a2b3c4d', 'ptt_group_ids': ['g002']},
     'errors': _ERR_COMMON + [{'status': 400, 'when': '없는 PTT 그룹'}, {'status': 404, 'when': '없는 역할'}],
     'notes': [], 'auth': dict(_AUTH_AUTHZ)},
    {'id': 'csc.roles.assignments.list', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/roles/{id}/assignments',
     'summary': '배정 목록', 'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '역할 id'}],
     'response': '{role_id, assignments[{principal_type, principal_id, name}]}', 'response_fields': [],
     'example': {'role_id': 'role-1a2b3c4d', 'assignments': [{'principal_type': 'user', 'principal_id': '5020', 'name': '관제1석'}]},
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 역할'}],
     'notes': ['콘솔 계정(console)의 배정은 OAM console_accounts[].role — 여기에는 user 행만.'], 'auth': dict(_AUTH_AUTHZ)},
    {'id': 'csc.roles.assignments.put', 'module': 'csc', 'method': 'PUT', 'path': '/api/v1/roles/{id}/assignments',
     'summary': '배정 (사람당 역할 하나 — 다른 역할에서 이동)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '역할 id'},
                {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': "{principal_type:'user', principal_id:<users.id>}"}],
     'response': '{role_id, principal_type, principal_id, moved_from, ambient_synced[]}', 'response_fields': [],
     'example': {'role_id': 'role-1a2b3c4d', 'principal_type': 'user', 'principal_id': '5020', 'moved_from': None, 'ambient_synced': ['+82510001001']},
     'errors': _ERR_COMMON + [{'status': 400, 'when': "principal_type≠'user'", 'body': {'error': 'invalid_principal_type'}},
                              {'status': 404, 'when': '없는 역할/사용자'}],
     'notes': ['ptt_listen≠none 이면 대상 person 의 PTT 회선 allow_ambient_listening=1 동기(해제 시 0) + ROLE_CHANGED(uri=person id).'],
     'auth': dict(_AUTH_AUTHZ)},
    {'id': 'csc.roles.assignments.delete', 'module': 'csc', 'method': 'DELETE',
     'path': '/api/v1/roles/{id}/assignments/{principal_type}/{principal_id}', 'summary': '배정 해제',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '역할 id'},
                {'name': 'principal_type', 'in': 'path', 'type': 'string', 'required': True, 'desc': 'user'},
                {'name': 'principal_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': 'users.id'}],
     'response': '{role_id, principal_type, principal_id, ambient_synced[]}', 'response_fields': [],
     'example': {'role_id': 'role-1a2b3c4d', 'principal_type': 'user', 'principal_id': '5020', 'ambient_synced': ['+82510001001']},
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 배정', 'body': {'error': 'Assignment not found'}}],
     'notes': [], 'auth': dict(_AUTH_AUTHZ)},
]
