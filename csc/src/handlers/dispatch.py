"""
CIMS 관제 그룹(dispatch group) 관리 REST API — docs/design/features/dispatch_center.md §3·§8.2

관제 그룹 = 픽업 그룹 + (선택) 대표번호 + (선택) 감청 범위. 불변 id(dg-xxxxxxxx)가 곧
volte_subscriptions.pickup_group 값이라 당겨받기·BLF 인가·대표번호 병렬 호출·감청 범위가 한 축을 공유한다.

  GET/POST          /api/v1/dispatch-groups
  GET/PUT/DELETE    /api/v1/dispatch-groups/{id}
  GET/POST          /api/v1/dispatch-groups/{id}/members
  DELETE            /api/v1/dispatch-groups/{id}/members/{user_id}
  PUT               /api/v1/dispatch-groups/{id}/monitor-targets   {target_group_ids:[...]}
  PUT               /api/v1/dispatch-groups/{id}/ptt-targets       {ptt_group_ids:[mcptt_group_id...]}

SoT 는 멤버십(dispatch_group_members)이다 — 멤버 추가/제거 시 가입자 pickup_group 을 group_id/NULL 로
함께 갱신(USER_CHANGED)하고, CSP 에는 DISPATCH_GROUP_CHANGED 로 그룹 재적재를 알린다.
"""

import secrets
from urllib.parse import urlparse, unquote
from pathlib import PurePath

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult
from services import admin_auth
from services.mcptt import notify_csp

_DISPATCH_BASE = '/api/v1/dispatch-groups'

_ALERT_MODES = ('parallel', 'sequential')
_BUSY_MODES = ('skip', 'alert')
_MONITOR_SCOPES = ('none', 'own', 'listed', 'all')
_PTT_LISTEN = ('none', 'listed', 'all')
_LISTEN_VIS = ('hidden', 'visible')

_SCHEMA_ERROR = {'error': 'schema_not_migrated',
                 'detail': 'dispatch_groups table absent — sql/migrate_dispatch_groups.sql not applied'}

_HAS_TABLES = None  # 테이블 프로브 캐시 (프로세스 수명). None=미확인


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


def has_dispatch_tables(cur) -> bool:
    """dispatch_groups 존재 여부 — migrate_dispatch_groups.sql. 한 번 확인 후 캐시."""
    global _HAS_TABLES
    if _HAS_TABLES is None:
        cur.execute("SHOW TABLES LIKE 'dispatch_groups'")
        _HAS_TABLES = cur.fetchone() is not None
    return _HAS_TABLES


def dispatch_group_of_user(cur, user_id: str):
    """가입자가 속한 관제 그룹 id (없으면 None). 테이블 미적용이면 None.
    admin.py 가 pickup_group 직접 편집 409 게이트(derived_from_dispatch_group)에 쓴다."""
    if not has_dispatch_tables(cur):
        return None
    cur.execute("SELECT group_id FROM dispatch_group_members WHERE user_id=%s", (user_id,))
    row = cur.fetchone()
    return row['group_id'] if row else None


def _new_group_id() -> str:
    return 'dg-' + secrets.token_hex(4)


def _dt(v):
    return v.isoformat() if v is not None and hasattr(v, 'isoformat') else v


# ──────────────────────────────────────────────────────────────
#  Handler
# ──────────────────────────────────────────────────────────────

async def handle_dispatch_groups(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """/api/v1/dispatch-groups/* — 조회 monitor+, 생성/변경 operator+, 감청 범위·PTT 청취 편입은 manager+.

    감청(monitor_scope≠none / ptt_listen≠none)은 당사자가 모르는 동작이므로 그 그룹의 범위 변경과 멤버
    편입은 manager 승인으로 제한한다 (dispatch_center.md §5.8). 역할(role)은 **콘솔 계정**(OAM
    console_accounts·내장 admin — 토큰 클레임)에만 있다. 편입되는 가입자(person) 쪽에는 역할 게이트가
    없다 — DB users 는 person 전용이라 role 컬럼이 없고(sql/migrate_users_person_only.sql), 콘솔 계정과
    가입자는 다른 저장소·다른 모듈이다(csc_standalone_module.md 도메인 경계). PTT 청취 자격은 TS 24.484
    ptt_user_profile.allow_ambient_listening 이며 CSP 가 청취 개시 시점에 판정한다(§5.6)."""
    config = kwargs.get('config', {})
    parts = _path_parts(handler_args.full_path, _DISPATCH_BASE)
    group_id = parts[0] if len(parts) > 0 else None
    sub = parts[1] if len(parts) > 1 else None        # members | monitor-targets | ptt-targets
    member_id = parts[2] if len(parts) > 2 else None
    method = handler_args.method.upper()

    payload, err = admin_auth.require_role(handler_args, 'monitor' if method == 'GET' else 'operator')
    if err:
        return err
    is_manager = admin_auth.role_rank(payload.get('role')) >= admin_auth.role_rank('manager')

    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                if not has_dispatch_tables(cur):
                    return HandlerResult(status=400 if method != 'GET' else 200,
                                         body=_SCHEMA_ERROR if method != 'GET' else {'groups': [],
                                                                                    'schema': 'not_migrated'})
                if group_id is None:
                    if method == 'GET':
                        return _list_groups(cur, handler_args)
                    if method == 'POST':
                        return _create_group(cur, handler_args.body, is_manager)
                    return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

                if sub is None:
                    if method == 'GET':
                        return _get_group(cur, group_id)
                    if method == 'PUT':
                        return _update_group(cur, group_id, handler_args.body, is_manager)
                    if method == 'DELETE':
                        return _delete_group(cur, group_id)
                    return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

                if sub == 'members':
                    if member_id is None:
                        if method == 'GET':
                            return _list_members(cur, group_id)
                        if method == 'POST':
                            return _add_member(cur, group_id, handler_args.body, is_manager)
                        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
                    if method == 'DELETE':
                        return _remove_member(cur, group_id, member_id)
                    return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

                if sub == 'monitor-targets' and method == 'PUT':
                    if not is_manager:
                        return HandlerResult(status=403, body={'error': 'manager_required',
                                                               'detail': '감청 대상 변경은 manager 이상'})
                    return _put_monitor_targets(cur, group_id, handler_args.body)
                if sub == 'ptt-targets' and method == 'PUT':
                    if not is_manager:
                        return HandlerResult(status=403, body={'error': 'manager_required',
                                                               'detail': 'PTT 청취 대상 변경은 manager 이상'})
                    return _put_ptt_targets(cur, group_id, handler_args.body)
                if sub in ('monitor-targets', 'ptt-targets'):
                    return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
                return HandlerResult(status=404, body={'error': 'Not Found'})
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


# ──────────────────────────────────────────────────────────────
#  Shaping / validation
# ──────────────────────────────────────────────────────────────

_GROUP_COLS = ("id, name, pilot_id, service_ref, alert_mode, no_answer_sec, busy_members, overflow_target, "
               "monitor_scope, ptt_listen, listen_visibility, org_id, created_at, updated_at")


def _shape(g: dict, members=None, monitor_targets=None, ptt_targets=None):
    g['no_answer_sec'] = int(g.get('no_answer_sec') or 30)
    g['created_at'] = _dt(g.get('created_at'))
    g['updated_at'] = _dt(g.get('updated_at'))
    if members is not None:
        g['members'] = members
    if monitor_targets is not None:
        g['monitor_targets'] = monitor_targets
    if ptt_targets is not None:
        g['ptt_targets'] = ptt_targets
    return g


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


def _pilot_conflict(cur, pilot: str, self_id: str = None):
    """대표번호는 가입 id 주소 공간·다른 대표번호와 겹치면 안 된다 (§8.2) → 409 body 또는 None."""
    if not pilot:
        return None
    for t in ('volte_subscriptions', 'ptt_subscriptions'):
        cur.execute(f"SELECT 1 FROM {t} WHERE id=%s", (pilot,))
        if cur.fetchone():
            return {'error': 'pilot_conflict', 'detail': f'pilot_id {pilot} is a subscriber id ({t})'}
    cur.execute("SELECT id FROM dispatch_groups WHERE pilot_id=%s", (pilot,))
    row = cur.fetchone()
    if row and row['id'] != self_id:
        return {'error': 'pilot_conflict', 'detail': f"pilot_id {pilot} already used by group {row['id']}"}
    return None


def _monitoring(g: dict) -> bool:
    return (g.get('monitor_scope') or 'none') != 'none' or (g.get('ptt_listen') or 'none') != 'none'


def _sync_pickup_group(cur, user_id: str, group_id):
    """멤버십 → volte/ptt 가입자 pickup_group 파생 갱신 (컬럼 존재 테이블만)."""
    for t in ('volte_subscriptions', 'ptt_subscriptions'):
        cur.execute("SHOW COLUMNS FROM %s LIKE 'pickup_group'" % t)
        if cur.fetchone() is None:
            continue
        cur.execute(f"UPDATE {t} SET pickup_group=%s WHERE id=%s", (group_id, user_id))


def _subscriber_exists(cur, user_id: str) -> bool:
    for t in ('volte_subscriptions', 'ptt_subscriptions'):
        cur.execute(f"SELECT 1 FROM {t} WHERE id=%s", (user_id,))
        if cur.fetchone():
            return True
    return False


# ──────────────────────────────────────────────────────────────
#  Group CRUD
# ──────────────────────────────────────────────────────────────

def _list_groups(cur, handler_args):
    q = {}
    try:
        from urllib.parse import parse_qs
        q = {k: v[0] for k, v in parse_qs(urlparse(handler_args.full_path).query).items()}
    except Exception:
        pass
    sql = f"SELECT {_GROUP_COLS} FROM dispatch_groups"
    params = []
    if q.get('org_id'):
        sql += " WHERE org_id=%s"
        params.append(q['org_id'])
    sql += " ORDER BY name, id"
    cur.execute(sql, params)
    groups = cur.fetchall()
    cur.execute("SELECT group_id, user_id, alert_order FROM dispatch_group_members ORDER BY alert_order, user_id")
    mem = {}
    for r in cur.fetchall():
        mem.setdefault(r['group_id'], []).append({'user_id': r['user_id'], 'alert_order': r['alert_order']})
    cur.execute("SELECT group_id, target_group_id FROM dispatch_group_monitor_targets")
    mon = {}
    for r in cur.fetchall():
        mon.setdefault(r['group_id'], []).append(r['target_group_id'])
    cur.execute("SELECT t.group_id, g.mcptt_group_id FROM dispatch_group_ptt_targets t "
                "JOIN ptt_groups g ON g.id=t.ptt_group_id")
    ptt = {}
    for r in cur.fetchall():
        ptt.setdefault(r['group_id'], []).append(r['mcptt_group_id'])
    out = [_shape(g, mem.get(g['id'], []), mon.get(g['id'], []), ptt.get(g['id'], [])) for g in groups]
    return HandlerResult(status=200, body={'groups': out})


def _fetch_group(cur, group_id: str):
    cur.execute(f"SELECT {_GROUP_COLS} FROM dispatch_groups WHERE id=%s", (group_id,))
    g = cur.fetchone()
    if not g:
        return None
    cur.execute("SELECT user_id, alert_order FROM dispatch_group_members WHERE group_id=%s "
                "ORDER BY alert_order, user_id", (group_id,))
    members = cur.fetchall()
    cur.execute("SELECT target_group_id FROM dispatch_group_monitor_targets WHERE group_id=%s", (group_id,))
    mon = [r['target_group_id'] for r in cur.fetchall()]
    cur.execute("SELECT g.mcptt_group_id FROM dispatch_group_ptt_targets t JOIN ptt_groups g ON g.id=t.ptt_group_id "
                "WHERE t.group_id=%s", (group_id,))
    ptt = [r['mcptt_group_id'] for r in cur.fetchall()]
    return _shape(g, members, mon, ptt)


def _get_group(cur, group_id: str):
    g = _fetch_group(cur, group_id)
    if g is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    return HandlerResult(status=200, body=g)


def _create_group(cur, body, is_manager: bool):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    try:
        alert_mode = _enum(body, 'alert_mode', _ALERT_MODES, 'parallel')
        busy = _enum(body, 'busy_members', _BUSY_MODES, 'skip')
        scope = _enum(body, 'monitor_scope', _MONITOR_SCOPES, 'none')
        ptt_listen = _enum(body, 'ptt_listen', _PTT_LISTEN, 'none')
        vis = _enum(body, 'listen_visibility', _LISTEN_VIS, 'hidden')
        no_answer = int(body.get('no_answer_sec') or 30)
    except (ValueError, TypeError) as e:
        return HandlerResult(status=400, body={'error': str(e)})
    if no_answer < 5:
        return HandlerResult(status=400, body={'error': 'no_answer_sec must be >= 5'})
    if (scope != 'none' or ptt_listen != 'none') and not is_manager:
        return HandlerResult(status=403, body={'error': 'manager_required',
                                               'detail': '감청/청취 범위가 있는 그룹 생성은 manager 이상'})
    group_id = _opt_str(body, 'id') or _new_group_id()
    if not group_id.startswith('dg-') or len(group_id) > 64:
        return HandlerResult(status=400, body={'error': "id must start with 'dg-' (max 64)"})
    name = _opt_str(body, 'name') or group_id
    pilot = _opt_str(body, 'pilot_id')
    service_ref = _opt_str(body, 'service_ref')
    overflow = _opt_str(body, 'overflow_target')
    org_id = body.get('org_id')
    org_id = int(org_id) if org_id not in (None, '', 0, '0') else None

    cur.execute("SELECT 1 FROM dispatch_groups WHERE id=%s", (group_id,))
    if cur.fetchone():
        return HandlerResult(status=409, body={'error': 'group_exists', 'detail': group_id})
    conflict = _pilot_conflict(cur, pilot)
    if conflict:
        return HandlerResult(status=409, body=conflict)
    if pilot and not service_ref:
        return HandlerResult(status=400, body={'error': 'service_ref is required when pilot_id is set'})

    cur.execute(
        "INSERT INTO dispatch_groups (id, name, pilot_id, service_ref, alert_mode, no_answer_sec, busy_members, "
        "overflow_target, monitor_scope, ptt_listen, listen_visibility, org_id) "
        "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)",
        (group_id, name, pilot, service_ref, alert_mode, no_answer, busy, overflow, scope, ptt_listen, vis, org_id))
    changed_users = []
    for i, m in enumerate(body.get('members') or []):
        uid = m.get('user_id') if isinstance(m, dict) else m
        uid = (uid or '').strip()
        if not uid or not _subscriber_exists(cur, uid):
            continue
        order = int(m.get('alert_order', i)) if isinstance(m, dict) else i
        cur.execute("INSERT INTO dispatch_group_members (user_id, group_id, alert_order) VALUES (%s,%s,%s) "
                    "ON DUPLICATE KEY UPDATE group_id=VALUES(group_id), alert_order=VALUES(alert_order)",
                    (uid, group_id, order))
        _sync_pickup_group(cur, uid, group_id)
        changed_users.append(uid)
    notify_csp("DISPATCH_GROUP_CHANGED", group_id, "POST")
    for uid in changed_users:
        notify_csp("USER_CHANGED", f"tel:{uid}", "PUT")
    return HandlerResult(status=201, body={'id': group_id})


def _update_group(cur, group_id: str, body, is_manager: bool):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    cur.execute(f"SELECT {_GROUP_COLS} FROM dispatch_groups WHERE id=%s", (group_id,))
    cur_row = cur.fetchone()
    if not cur_row:
        return HandlerResult(status=404, body={'error': 'Group not found'})
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
        if 'monitor_scope' in body:
            scope = _enum(body, 'monitor_scope', _MONITOR_SCOPES, 'none')
            if scope != cur_row['monitor_scope'] and not is_manager:
                return HandlerResult(status=403, body={'error': 'manager_required',
                                                       'detail': '감청 범위 변경은 manager 이상'})
            fields.append("monitor_scope=%s"); values.append(scope)
        if 'ptt_listen' in body:
            pl = _enum(body, 'ptt_listen', _PTT_LISTEN, 'none')
            if pl != cur_row['ptt_listen'] and not is_manager:
                return HandlerResult(status=403, body={'error': 'manager_required',
                                                       'detail': 'PTT 청취 범위 변경은 manager 이상'})
            fields.append("ptt_listen=%s"); values.append(pl)
        if 'listen_visibility' in body:
            fields.append("listen_visibility=%s"); values.append(_enum(body, 'listen_visibility', _LISTEN_VIS, 'hidden'))
        if 'org_id' in body:
            org_id = body.get('org_id')
            fields.append("org_id=%s"); values.append(int(org_id) if org_id not in (None, '', 0, '0') else None)
    except (ValueError, TypeError) as e:
        return HandlerResult(status=400, body={'error': str(e)})
    if fields:
        values.append(group_id)
        cur.execute(f"UPDATE dispatch_groups SET {', '.join(fields)} WHERE id=%s", values)
    notify_csp("DISPATCH_GROUP_CHANGED", group_id, "PUT")
    return HandlerResult(status=200, body={'id': group_id})


def _delete_group(cur, group_id: str):
    cur.execute("SELECT user_id FROM dispatch_group_members WHERE group_id=%s", (group_id,))
    users = [r['user_id'] for r in cur.fetchall()]
    cur.execute("DELETE FROM dispatch_groups WHERE id=%s", (group_id,))
    if cur.rowcount == 0:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    # 멤버 행은 FK CASCADE — 파생 pickup_group 해제(NULL → CSP org 폴백)
    for uid in users:
        _sync_pickup_group(cur, uid, None)
    notify_csp("DISPATCH_GROUP_CHANGED", group_id, "DELETE")
    for uid in users:
        notify_csp("USER_CHANGED", f"tel:{uid}", "PUT")
    return HandlerResult(status=200, body={'id': group_id})


# ──────────────────────────────────────────────────────────────
#  Members — 가입자당 그룹 하나 (§3.2)
# ──────────────────────────────────────────────────────────────

def _list_members(cur, group_id: str):
    cur.execute("SELECT 1 FROM dispatch_groups WHERE id=%s", (group_id,))
    if cur.fetchone() is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    cur.execute("SELECT user_id, alert_order FROM dispatch_group_members WHERE group_id=%s ORDER BY alert_order, user_id",
                (group_id,))
    return HandlerResult(status=200, body={'group_id': group_id, 'members': cur.fetchall()})


def _add_member(cur, group_id: str, body, is_manager: bool):
    if not isinstance(body, dict):
        return HandlerResult(status=400, body={'error': 'JSON body required'})
    user_id = (body.get('user_id') or '').strip()
    if not user_id:
        return HandlerResult(status=400, body={'error': 'user_id is required'})
    cur.execute("SELECT monitor_scope, ptt_listen FROM dispatch_groups WHERE id=%s", (group_id,))
    g = cur.fetchone()
    if g is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    if not _subscriber_exists(cur, user_id):
        return HandlerResult(status=404, body={'error': 'Subscriber not found', 'detail': user_id})
    if _monitoring(g) and not is_manager:
        # 감청 가능 그룹 편입 — 콘솔 manager 승인 (§5.8). 가입자 쪽 역할 게이트는 없다(handle_dispatch_groups 주석).
        return HandlerResult(status=403, body={'error': 'manager_required',
                                               'detail': '감청/청취 그룹 편입은 manager 이상'})
    # 가입자당 그룹 하나 — 다른 그룹 소속이면 이동(이전 그룹도 재적재 통지)
    prev = dispatch_group_of_user(cur, user_id)
    order = int(body.get('alert_order', 0))
    cur.execute("INSERT INTO dispatch_group_members (user_id, group_id, alert_order) VALUES (%s,%s,%s) "
                "ON DUPLICATE KEY UPDATE group_id=VALUES(group_id), alert_order=VALUES(alert_order)",
                (user_id, group_id, order))
    _sync_pickup_group(cur, user_id, group_id)
    notify_csp("DISPATCH_GROUP_CHANGED", group_id, "PUT")
    if prev and prev != group_id:
        notify_csp("DISPATCH_GROUP_CHANGED", prev, "PUT")
    notify_csp("USER_CHANGED", f"tel:{user_id}", "PUT")
    return HandlerResult(status=201, body={'group_id': group_id, 'user_id': user_id, 'moved_from': prev})


def _remove_member(cur, group_id: str, user_id: str):
    cur.execute("DELETE FROM dispatch_group_members WHERE group_id=%s AND user_id=%s", (group_id, user_id))
    if cur.rowcount == 0:
        return HandlerResult(status=404, body={'error': 'Member not found'})
    _sync_pickup_group(cur, user_id, None)
    notify_csp("DISPATCH_GROUP_CHANGED", group_id, "PUT")
    notify_csp("USER_CHANGED", f"tel:{user_id}", "PUT")
    return HandlerResult(status=200, body={'group_id': group_id, 'user_id': user_id})


# ──────────────────────────────────────────────────────────────
#  Targets — monitor_scope=listed / ptt_listen=listed
# ──────────────────────────────────────────────────────────────

def _put_monitor_targets(cur, group_id: str, body):
    if not isinstance(body, dict) or not isinstance(body.get('target_group_ids'), list):
        return HandlerResult(status=400, body={'error': 'target_group_ids (array) required'})
    cur.execute("SELECT 1 FROM dispatch_groups WHERE id=%s", (group_id,))
    if cur.fetchone() is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    targets = [str(t).strip() for t in body['target_group_ids'] if str(t).strip()]
    for t in targets:
        cur.execute("SELECT 1 FROM dispatch_groups WHERE id=%s", (t,))
        if cur.fetchone() is None:
            return HandlerResult(status=400, body={'error': f'unknown target group: {t}'})
    cur.execute("DELETE FROM dispatch_group_monitor_targets WHERE group_id=%s", (group_id,))
    for t in targets:
        cur.execute("INSERT IGNORE INTO dispatch_group_monitor_targets (group_id, target_group_id) VALUES (%s,%s)",
                    (group_id, t))
    notify_csp("DISPATCH_GROUP_CHANGED", group_id, "PUT")
    return HandlerResult(status=200, body={'group_id': group_id, 'target_group_ids': targets})


def _put_ptt_targets(cur, group_id: str, body):
    if not isinstance(body, dict) or not isinstance(body.get('ptt_group_ids'), list):
        return HandlerResult(status=400, body={'error': 'ptt_group_ids (array of mcptt_group_id) required'})
    cur.execute("SELECT 1 FROM dispatch_groups WHERE id=%s", (group_id,))
    if cur.fetchone() is None:
        return HandlerResult(status=404, body={'error': 'Group not found'})
    pks = []
    for gid in body['ptt_group_ids']:
        gid = str(gid).strip()
        if not gid:
            continue
        cur.execute("SELECT id FROM ptt_groups WHERE mcptt_group_id=%s", (gid,))
        r = cur.fetchone()
        if r is None:
            return HandlerResult(status=400, body={'error': f'unknown ptt group: {gid}'})
        pks.append((r['id'], gid))
    cur.execute("DELETE FROM dispatch_group_ptt_targets WHERE group_id=%s", (group_id,))
    for pk, _ in pks:
        cur.execute("INSERT IGNORE INTO dispatch_group_ptt_targets (group_id, ptt_group_id) VALUES (%s,%s)",
                    (group_id, pk))
    notify_csp("DISPATCH_GROUP_CHANGED", group_id, "PUT")
    return HandlerResult(status=200, body={'group_id': group_id, 'ptt_group_ids': [g for _, g in pks]})


CIMS_DISPATCH_HANDLER_LIST = [
    (_DISPATCH_BASE, handle_dispatch_groups, {}),
]


# ── API 문서 (개발자 모드) ──────────────────────────────────────────────────
#  이 모듈이 제공하는 엔드포인트의 자기기술. csc handlers/api_docs.py 가 수집한다.
#  경로/파라미터를 바꾸면 **여기도 같은 커밋에서** 갱신한다.
_AUTH_MONITOR = {'scheme': 'bearer', 'role': 'monitor', 'token_from': 'POST /api/v1/auth/login'}
_AUTH_OPERATOR = {'scheme': 'bearer', 'role': 'operator', 'token_from': 'POST /api/v1/auth/login'}
_AUTH_MANAGER = {'scheme': 'bearer', 'role': 'manager', 'token_from': 'POST /api/v1/auth/login'}
_ERR_COMMON = [
    {'status': 401, 'when': 'Authorization 헤더 없음 / 토큰 만료', 'body': {'error': 'unauthorized'}},
    {'status': 403, 'when': '권한 등급 미달', 'body': {'error': 'forbidden'}},
]
_ERR_SCHEMA = {'status': 400, 'when': 'dispatch_groups 테이블 미적용', 'body': _SCHEMA_ERROR}
_DG_FIELDS = [
    {'name': 'id', 'type': 'string', 'desc': '불변 키 dg-xxxxxxxx — volte_subscriptions.pickup_group 값·상관 키'},
    {'name': 'name', 'type': 'string', 'desc': '표시 이름 (키에 쓰지 않는다)'},
    {'name': 'pilot_id', 'type': 'string|null', 'desc': '대표번호(AoR user part). null=대표번호 없음(순수 당겨받기 그룹)'},
    {'name': 'service_ref', 'type': 'string|null', 'desc': '대표번호 접속서비스 name — 도메인·SRTP 정책 근거'},
    {'name': 'alert_mode', 'type': 'string', 'desc': 'parallel(기본)|sequential — TS 24.239 Flexible Alerting'},
    {'name': 'no_answer_sec', 'type': 'integer', 'desc': '전원 무응답 판정 초 (기본 30, CSP Setup.Sip.Dispatch.ForkRingTimeoutSec 로 clamp)'},
    {'name': 'busy_members', 'type': 'string', 'desc': 'skip(기본)|alert — 통화 중 그룹원 호출 여부'},
    {'name': 'overflow_target', 'type': 'string|null', 'desc': '무응답 넘김 대상(대표번호/내선). null=480'},
    {'name': 'monitor_scope', 'type': 'string', 'desc': 'none(기본)|own|listed|all — 합법감청(dialog 감시·Join) 범위'},
    {'name': 'ptt_listen', 'type': 'string', 'desc': 'none(기본)|listed|all — PTT 그룹콜 청취 범위'},
    {'name': 'listen_visibility', 'type': 'string', 'desc': 'hidden(기본)|visible — PTT 청취 멤버 로스터 노출'},
    {'name': 'org_id', 'type': 'integer|null', 'desc': '소속 조직'},
    {'name': 'members[]', 'type': 'object', 'desc': '{user_id, alert_order} — 가입자당 그룹 하나'},
    {'name': 'monitor_targets[]', 'type': 'string', 'desc': 'monitor_scope=listed 의 대상 그룹 id'},
    {'name': 'ptt_targets[]', 'type': 'string', 'desc': 'ptt_listen=listed 의 대상 PTT 그룹 (mcptt_group_id)'},
]
_DG_EXAMPLE = {'id': 'dg-7f3a91c2', 'name': '관제 1반', 'pilot_id': '7000', 'service_ref': 'volte',
               'alert_mode': 'parallel', 'no_answer_sec': 30, 'busy_members': 'skip', 'overflow_target': None,
               'monitor_scope': 'own', 'ptt_listen': 'none', 'listen_visibility': 'hidden', 'org_id': 1,
               'members': [{'user_id': '+821300000004', 'alert_order': 0}], 'monitor_targets': [], 'ptt_targets': []}

CIMS_DISPATCH_API_DOCS = [
    {'id': 'csc.dispatch-groups.list', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/dispatch-groups',
     'summary': '관제 그룹 목록 (멤버·대상 포함)',
     'params': [{'name': 'org_id', 'in': 'query', 'type': 'integer', 'required': False, 'desc': '조직 필터'}],
     'response': '{groups[]}',
     'response_fields': [{'name': 'groups[].' + f['name'], **{k: v for k, v in f.items() if k != 'name'}}
                         for f in _DG_FIELDS],
     'example': {'groups': [_DG_EXAMPLE]},
     'errors': list(_ERR_COMMON),
     'notes': ['dispatch_groups 테이블 미적용 DB 에서는 빈 목록 + schema=not_migrated 를 돌려준다.'],
     'auth': dict(_AUTH_MONITOR)},
    {'id': 'csc.dispatch-groups.get', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/dispatch-groups/{id}',
     'summary': '관제 그룹 1건',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id (dg-…)'}],
     'response': '그룹 객체', 'response_fields': list(_DG_FIELDS), 'example': dict(_DG_EXAMPLE),
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 그룹', 'body': {'error': 'Group not found'}}],
     'notes': [], 'auth': dict(_AUTH_MONITOR)},
    {'id': 'csc.dispatch-groups.create', 'module': 'csc', 'method': 'POST', 'path': '/api/v1/dispatch-groups',
     'summary': '관제 그룹 생성 (id 미지정 시 dg-<hex8> 발급)',
     'params': [{'name': 'body', 'in': 'body', 'type': 'object', 'required': True,
                 'desc': '{id?, name, pilot_id?, service_ref?(pilot 시 필수), alert_mode?, no_answer_sec?, busy_members?, '
                         'overflow_target?, monitor_scope?, ptt_listen?, listen_visibility?, org_id?, members?[{user_id, alert_order}]}'}],
     'response': '{id}', 'response_fields': [{'name': 'id', 'type': 'string', 'desc': '생성된 그룹 id'}],
     'example': {'id': 'dg-7f3a91c2'},
     'errors': _ERR_COMMON + [
         _ERR_SCHEMA,
         {'status': 403, 'when': 'monitor_scope/ptt_listen≠none 그룹 생성을 operator 가 시도', 'body': {'error': 'manager_required'}},
         {'status': 409, 'when': 'pilot_id 가 가입 id·다른 대표번호와 충돌', 'body': {'error': 'pilot_conflict'}},
         {'status': 409, 'when': 'id 중복', 'body': {'error': 'group_exists'}},
     ],
     'notes': ['성공 시 **201**.', '멤버 편입은 가입자 pickup_group 을 그룹 id 로 파생 갱신하고 USER_CHANGED 를 보낸다.',
               'CSP 에는 DISPATCH_GROUP_CHANGED(uri=그룹 id) 로 재적재를 알린다.'],
     'auth': dict(_AUTH_OPERATOR)},
    {'id': 'csc.dispatch-groups.update', 'module': 'csc', 'method': 'PUT', 'path': '/api/v1/dispatch-groups/{id}',
     'summary': '관제 그룹 수정 (부분 갱신)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id'},
                {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '변경할 필드만'}],
     'response': '{id}', 'response_fields': [{'name': 'id', 'type': 'string', 'desc': '그룹 id'}],
     'example': {'id': 'dg-7f3a91c2'},
     'errors': _ERR_COMMON + [
         _ERR_SCHEMA,
         {'status': 403, 'when': 'monitor_scope/ptt_listen 변경을 operator 가 시도', 'body': {'error': 'manager_required'}},
         {'status': 404, 'when': '없는 그룹', 'body': {'error': 'Group not found'}},
         {'status': 409, 'when': 'pilot_id 충돌', 'body': {'error': 'pilot_conflict'}},
     ],
     'notes': [], 'auth': dict(_AUTH_OPERATOR)},
    {'id': 'csc.dispatch-groups.delete', 'module': 'csc', 'method': 'DELETE', 'path': '/api/v1/dispatch-groups/{id}',
     'summary': '관제 그룹 삭제 (멤버 pickup_group 해제)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id'}],
     'response': '{id}', 'response_fields': [{'name': 'id', 'type': 'string', 'desc': '그룹 id'}],
     'example': {'id': 'dg-7f3a91c2'},
     'errors': _ERR_COMMON + [_ERR_SCHEMA, {'status': 404, 'when': '없는 그룹', 'body': {'error': 'Group not found'}}],
     'notes': ['멤버의 pickup_group 은 NULL 로 돌아간다 (CSP 는 org 폴백).'], 'auth': dict(_AUTH_OPERATOR)},
    {'id': 'csc.dispatch-groups.members.list', 'module': 'csc', 'method': 'GET',
     'path': '/api/v1/dispatch-groups/{id}/members', 'summary': '관제 그룹 멤버 목록',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id'}],
     'response': '{group_id, members[{user_id, alert_order}]}',
     'response_fields': [{'name': 'members[].user_id', 'type': 'string', 'desc': '가입자 id'},
                         {'name': 'members[].alert_order', 'type': 'integer', 'desc': 'sequential 호출·절삭 순서'}],
     'example': {'group_id': 'dg-7f3a91c2', 'members': [{'user_id': '+821300000004', 'alert_order': 0}]},
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 그룹', 'body': {'error': 'Group not found'}}],
     'notes': [], 'auth': dict(_AUTH_MONITOR)},
    {'id': 'csc.dispatch-groups.members.add', 'module': 'csc', 'method': 'POST',
     'path': '/api/v1/dispatch-groups/{id}/members', 'summary': '멤버 추가/이동 (가입자당 그룹 하나)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id'},
                {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '{user_id(필수), alert_order?}'}],
     'response': '{group_id, user_id, moved_from}',
     'response_fields': [{'name': 'moved_from', 'type': 'string|null', 'desc': '다른 그룹에서 이동했으면 이전 그룹 id'}],
     'example': {'group_id': 'dg-7f3a91c2', 'user_id': '+821300000004', 'moved_from': None},
     'errors': _ERR_COMMON + [
         _ERR_SCHEMA,
         {'status': 403, 'when': '감청/청취 그룹 편입을 operator 가 시도', 'body': {'error': 'manager_required'}},
         {'status': 404, 'when': '없는 그룹/가입자'},
     ],
     'notes': ['성공 시 **201**.', '가입자 pickup_group 이 그룹 id 로 갱신된다 — 반영은 다음 REGISTER 갱신부터(등록 바인딩 스냅샷).'],
     'auth': dict(_AUTH_OPERATOR)},
    {'id': 'csc.dispatch-groups.members.remove', 'module': 'csc', 'method': 'DELETE',
     'path': '/api/v1/dispatch-groups/{id}/members/{user_id}', 'summary': '멤버 제거 (pickup_group 해제)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id'},
                {'name': 'user_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '가입자 id (URL 인코딩, +→%2B)'}],
     'response': '{group_id, user_id}', 'response_fields': [],
     'example': {'group_id': 'dg-7f3a91c2', 'user_id': '+821300000004'},
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 멤버', 'body': {'error': 'Member not found'}}],
     'notes': [], 'auth': dict(_AUTH_OPERATOR)},
    {'id': 'csc.dispatch-groups.monitor-targets.put', 'module': 'csc', 'method': 'PUT',
     'path': '/api/v1/dispatch-groups/{id}/monitor-targets', 'summary': '감청 대상 그룹 목록 교체 (monitor_scope=listed)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id'},
                {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '{target_group_ids: [dg-…]}'}],
     'response': '{group_id, target_group_ids[]}', 'response_fields': [],
     'example': {'group_id': 'dg-7f3a91c2', 'target_group_ids': ['dg-0a1b2c3d']},
     'errors': _ERR_COMMON + [{'status': 400, 'when': '없는 대상 그룹'}, {'status': 404, 'when': '없는 그룹'}],
     'notes': ['manager 이상.'], 'auth': dict(_AUTH_MANAGER)},
    {'id': 'csc.dispatch-groups.ptt-targets.put', 'module': 'csc', 'method': 'PUT',
     'path': '/api/v1/dispatch-groups/{id}/ptt-targets', 'summary': 'PTT 청취 대상 그룹 목록 교체 (ptt_listen=listed)',
     'params': [{'name': 'id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '그룹 id'},
                {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '{ptt_group_ids: [mcptt_group_id…]}'}],
     'response': '{group_id, ptt_group_ids[]}', 'response_fields': [],
     'example': {'group_id': 'dg-7f3a91c2', 'ptt_group_ids': ['g001']},
     'errors': _ERR_COMMON + [{'status': 400, 'when': '없는 PTT 그룹'}, {'status': 404, 'when': '없는 그룹'}],
     'notes': ['manager 이상.'], 'auth': dict(_AUTH_MANAGER)},
]
