"""단일 권한 판정 — `can(principal, capability, target)` (docs/design/features/mcptt_authorization.md §2·§3).

principal 은 둘, 역할은 하나다:
  ('console', login_id, role_id)   OAM 콘솔 계정 — JWT `sub`/`login_id` + `role` 클레임(= roles.id). 배정은 OAM
                                   file_store console_accounts[].role 이라 여기서는 클레임을 roles 행으로 해석만 한다.
  ('user', users_id)               가입자(person) — role_assignments(principal_type='user') → roles 행.
둘 다 같은 `roles` 행(능력 + 범위)으로 판정한다. 배정이 없으면 능력 전부 none.

능력 이름(§3) → roles 컬럼:
  authz.manage      authz_manage      bool   역할·배정·범위 관리 (내장 admin/manager 만 — 커스텀에 위임 불가 §2.4)
  audit.read        audit_read        bool
  directory.write   directory_write   none|own|all      대상 = 조직 코드 (own = org_id 조직과 그 하위)
  directory.read    directory_read    none|own|all      대상 = 조직 코드
  ptt_group.manage  ptt_group_manage  none|own|scope|all 대상 = PTT 그룹 (own=authorized_user_id, scope=directory_write 범위 안 org_code)
  monitor.call      monitor_call      none|own|listed|all 대상 = 전화 그룹 id (own=배정자의 전화 그룹, listed=role_monitor_targets)
  monitor.ptt       ptt_listen        none|listed|all   대상 = PTT 그룹 (listed=role_ptt_targets)
  history.read      history_read      none|scope|all
  alarm.ack         alarm_ack         bool
  mcptt.control     mcptt_control     bool

roles 테이블 미적용 DB(마이그레이션 전)에서는 내장 4행(BUILTIN_ROLES — sql/migrate_phone_groups_roles.sql 시드와 같은 값)만
있는 것으로 판정해 콘솔 계층(admin/manager/operator/monitor)이 종전대로 동작한다. 가입자 배정은 없다(관제 기능 비활성).

판정 결과의 403 본문 = {"error": "forbidden", "capability": "…", "scope": "…"} — `forbidden()`.
"""

import time
from typing import Optional, Tuple

_CAPS = {
    'authz.manage':     ('authz_manage', None),
    'audit.read':       ('audit_read', None),
    'directory.write':  ('directory_write', 'org'),
    'directory.read':   ('directory_read', 'org'),
    'ptt_group.manage': ('ptt_group_manage', 'ptt_group'),
    'monitor.call':     ('monitor_call', 'phone_group'),
    'monitor.ptt':      ('ptt_listen', 'ptt_group'),
    'history.read':     ('history_read', None),
    'alarm.ack':        ('alarm_ack', None),
    'mcptt.control':    ('mcptt_control', None),
}
CAPABILITIES = tuple(_CAPS)

ROLE_COLS = ('id', 'name', 'builtin', 'authz_manage', 'audit_read', 'directory_write', 'directory_read',
             'ptt_group_manage', 'monitor_call', 'ptt_listen', 'listen_visibility', 'history_read',
             'alarm_ack', 'mcptt_control', 'org_id')
_BOOL_COLS = ('builtin', 'authz_manage', 'audit_read', 'alarm_ack', 'mcptt_control')
# 커스텀(한정 범위) 역할에 켤 수 없는 능력 — 권한 분리 불변 규칙(§2.4). 켜면 400 not_delegable.
NOT_DELEGABLE = ('authz_manage', 'audit_read', 'alarm_ack', 'mcptt_control')

ENUMS = {
    'directory_write': ('none', 'own', 'all'),
    'directory_read': ('none', 'own', 'all'),
    'ptt_group_manage': ('none', 'own', 'scope', 'all'),
    'monitor_call': ('none', 'own', 'listed', 'all'),
    'ptt_listen': ('none', 'listed', 'all'),
    'listen_visibility': ('hidden', 'visible'),
    'history_read': ('none', 'scope', 'all'),
}


def _builtin(id_, name, authz, audit, dw, dr, pgm, hist, ack, ctl):
    return {'id': id_, 'name': name, 'builtin': True, 'authz_manage': authz, 'audit_read': audit,
            'directory_write': dw, 'directory_read': dr, 'ptt_group_manage': pgm,
            'monitor_call': 'none', 'ptt_listen': 'none', 'listen_visibility': 'hidden',
            'history_read': hist, 'alarm_ack': ack, 'mcptt_control': ctl, 'org_id': None}


# 내장 프리셋(§3 매트릭스) — 마이그레이션 시드와 같은 값. 값의 정본은 코드(시드는 ON DUPLICATE KEY UPDATE 로 따라온다).
BUILTIN_ROLES = {
    'admin':    _builtin('admin', '관리자', True, True, 'all', 'all', 'all', 'all', True, True),
    'manager':  _builtin('manager', '운영 관리자', True, True, 'all', 'all', 'all', 'all', True, True),
    'operator': _builtin('operator', '운용자', False, False, 'none', 'all', 'own', 'all', True, True),
    'monitor':  _builtin('monitor', '모니터', False, False, 'none', 'all', 'none', 'all', False, False),
}
BUILTIN_IDS = tuple(BUILTIN_ROLES)

# 관제 프리셋(§3 — 감독 / 관리 / 전체) — 콘솔이 역할 생성 시 초깃값으로 쓴다. 저장은 개별 필드.
PRESETS = {
    'supervisor': {'monitor_call': 'own', 'ptt_listen': 'listed', 'listen_visibility': 'hidden', 'history_read': 'scope'},
    'admin':      {'directory_write': 'own', 'ptt_group_manage': 'scope'},
    'full':       {'directory_write': 'own', 'ptt_group_manage': 'scope', 'monitor_call': 'own',
                   'ptt_listen': 'listed', 'listen_visibility': 'hidden', 'history_read': 'scope'},
}

_DB_CONFIG = None
_HAS_ROLES = None            # roles 테이블 프로브 캐시(프로세스 수명) — None=미확인
_CACHE_TTL = 30.0            # 역할 행 캐시 — 쓰기 경로(handlers/dispatch)가 invalidate() 하고, 안전망으로 TTL
_role_cache = {}             # role_id → (expires, row|None)


def init(config: dict) -> None:
    """startup/reload 시 1회 — CimsDatabase 설정."""
    global _DB_CONFIG
    _DB_CONFIG = (config or {}).get('CimsDatabase') or None
    invalidate()


def invalidate() -> None:
    """역할·대상·배정 쓰기 뒤 캐시 비움(handlers/dispatch 의 쓰기 경로)."""
    _role_cache.clear()


def reset_probe() -> None:
    global _HAS_ROLES
    _HAS_ROLES = None
    invalidate()


def _connect():
    if not _DB_CONFIG:
        return None
    import pymysql
    import pymysql.cursors
    return pymysql.connect(host=_DB_CONFIG.get('Host', '127.0.0.1'), port=int(_DB_CONFIG.get('Port', 3306)),
                           user=_DB_CONFIG.get('User', 'root'), password=_DB_CONFIG.get('Password', ''),
                           database=_DB_CONFIG.get('Db', 'cims'), charset='utf8mb4',
                           cursorclass=pymysql.cursors.DictCursor, connect_timeout=5)


def _rows(cur, cols) -> list:
    """DictCursor/tuple 커서 공용 — SELECT 열 순서를 cols 로 고정해 둘 다 dict 로 읽는다."""
    out = []
    for r in cur.fetchall() or []:
        if isinstance(r, dict):
            out.append({c: r.get(c) for c in cols})
        else:
            out.append(dict(zip(cols, r)))
    return out


def _one(cur, cols) -> Optional[dict]:
    rows = _rows(cur, cols)
    return rows[0] if rows else None


def _is_missing_table(e) -> bool:
    args = getattr(e, 'args', ())
    return bool(args) and args[0] == 1146


def has_roles_tables(cur) -> bool:
    """roles 테이블 존재 여부(sql/migrate_phone_groups_roles.sql) — 한 번 확인 후 캐시."""
    global _HAS_ROLES
    if _HAS_ROLES is None:
        cur.execute("SHOW TABLES LIKE 'roles'")
        _HAS_ROLES = cur.fetchone() is not None
    return _HAS_ROLES


def _norm(row: dict) -> dict:
    out = dict(row)
    for c in _BOOL_COLS:
        out[c] = bool(out.get(c))
    for c, allowed in ENUMS.items():
        v = str(out.get(c) or allowed[0]).lower()
        out[c] = v if v in allowed else allowed[0]
    out['name'] = out.get('name') or ''
    return out


# ── 역할 행 ─────────────────────────────────────────────────────────────────

def load_role(cur, role_id: Optional[str]) -> Optional[dict]:
    """roles 행(정규화) — 캐시. 테이블 미적용이면 내장 4행만."""
    if not role_id:
        return None
    hit = _role_cache.get(role_id)
    if hit and hit[0] > time.monotonic():
        return hit[1]
    row = None
    try:
        if cur is not None and has_roles_tables(cur):
            cur.execute(f"SELECT {', '.join(ROLE_COLS)} FROM roles WHERE id=%s", (role_id,))
            r = _one(cur, ROLE_COLS)
            row = _norm(r) if r else None
        else:
            row = BUILTIN_ROLES.get(role_id)
    except Exception as e:
        if _is_missing_table(e):
            row = BUILTIN_ROLES.get(role_id)
        else:
            raise
    if row is None and role_id in BUILTIN_ROLES:
        row = BUILTIN_ROLES[role_id]          # 시드 누락 방어 — 내장 행은 항상 존재해야 한다
    _role_cache[role_id] = (time.monotonic() + _CACHE_TTL, row)
    return row


def user_role_id(cur, users_id) -> Optional[str]:
    """가입자(person) 의 배정 역할 id — 사람당 하나. 테이블 미적용·미배정이면 None."""
    if users_id is None:
        return None
    try:
        if not has_roles_tables(cur):
            return None
        cur.execute("SELECT role_id FROM role_assignments WHERE principal_type='user' AND principal_id=%s",
                    (str(users_id),))
        r = _one(cur, ('role_id',))
        return r['role_id'] if r else None
    except Exception as e:
        if _is_missing_table(e):
            return None
        raise


def role_of(cur, principal) -> Optional[dict]:
    """principal → roles 행. 콘솔은 JWT role 클레임, 가입자는 role_assignments."""
    if not principal:
        return None
    if principal[0] == 'console':
        return load_role(cur, principal[2] if len(principal) > 2 else None)
    if principal[0] == 'user':
        return load_role(cur, user_role_id(cur, principal[1]))
    return None


def role_targets(cur, role_id: str) -> Tuple[set, set]:
    """(monitor_call=listed 대상 전화 그룹 id 집합, ptt_listen=listed 대상 ptt_groups.id 집합)."""
    if not role_id or not has_roles_tables(cur):
        return set(), set()
    cur.execute("SELECT phone_group_id FROM role_monitor_targets WHERE role_id=%s", (role_id,))
    mon = {r['phone_group_id'] for r in _rows(cur, ('phone_group_id',))}
    cur.execute("SELECT ptt_group_id FROM role_ptt_targets WHERE role_id=%s", (role_id,))
    ptt = {r['ptt_group_id'] for r in _rows(cur, ('ptt_group_id',))}
    return mon, ptt


def effective_ptt_group_manage(role: dict) -> str:
    """PTT 그룹 관리 능력의 유효값 — 관리 범위(directory_write)가 있는 역할은 그 범위의 PTT 그룹도 관리한다
    (dispatch_center.md §3.4: 관리 범위 안 그룹은 소유자가 아니어도 GMS PUT/DELETE 허용). 전환 마이그레이션이 만드는
    role-<그룹 id> 행은 ptt_group_manage 를 싣지 않으므로(none) directory_write 로 scope 를 유도한다."""
    v = role.get('ptt_group_manage') or 'none'
    if v == 'none' and (role.get('directory_write') or 'none') != 'none':
        return 'scope'
    return v


# ── 조직 범위 ────────────────────────────────────────────────────────────────

def org_rows(cur) -> list:
    cur.execute("SELECT id, code, name, parent_id, sort_order FROM organizations")
    return [(r['id'], r['code'] or '', r['name'] or '', r['parent_id'], r['sort_order'] or 0)
            for r in _rows(cur, ('id', 'code', 'name', 'parent_id', 'sort_order'))]


def org_subtree_codes(rows: list, root_id) -> set:
    """root_id 조직과 그 하위 전체의 code 집합(사이클 방어) — handlers/dispatch_directory 와 같은 규칙."""
    children = {}
    code_of = {}
    for oid, code, _name, pid, _so in rows:
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


def org_scope(cur, role: Optional[dict], field: str = 'directory_write') -> Tuple[str, str, Optional[set]]:
    """(범위 enum, 루트 조직 코드, 코드 집합|None=전체). own 인데 org_id 가 없으면 빈 집합(관리 불가)."""
    if not role:
        return 'none', '', set()
    mode = role.get(field) or 'none'
    if mode == 'all':
        return 'all', '', None
    if mode != 'own':
        return 'none', '', set()
    org_id = role.get('org_id')
    if org_id is None:
        return 'own', '', set()
    rows = org_rows(cur)
    code = next((c for oid, c, _n, _p, _s in rows if oid == org_id), '')
    if not code:
        return 'own', '', set()
    return 'own', code, org_subtree_codes(rows, org_id)


def in_codes(codes: Optional[set], org_code: str) -> bool:
    return True if codes is None else (org_code or '') in codes


# ── 판정 ─────────────────────────────────────────────────────────────────────

def console_principal(payload: dict):
    """콘솔 JWT 클레임 → principal. login_id 가 없으면 sub(내장/file_store 계정은 sub=login_id)."""
    p = payload or {}
    return ('console', str(p.get('login_id') or p.get('sub') or ''), p.get('role'))


def user_principal(users_id):
    return ('user', users_id)


def actor(principal) -> str:
    """감사 actor 표기(§2.5) — console:<login_id> / user:<users.id>."""
    if not principal:
        return ''
    return f"{principal[0]}:{principal[1]}"


def _own_phone_group(cur, principal):
    if not principal or principal[0] != 'user':
        return None
    from handlers import dispatch as _pg
    return _pg.phone_group_of_person(cur, principal[1])


def can(principal, capability: str, target=None, cur=None) -> Tuple[bool, str]:
    """(ok, reason). reason = 'ok' | 'no_role' | 'none' | 'denied' | 'out_of_scope' | 'unknown_capability'.

    target(선택) = {'kind': 'org', 'code': …} | {'kind': 'phone_group', 'id': …} |
                   {'kind': 'ptt_group', 'id': ptt_groups.id, 'mcptt_group_id': …, 'org_code': …, 'authorized_user_id': …}
    target 없이 부르면 능력 보유 여부만 본다(전역 능력, 또는 범위 판정을 호출자가 목록으로 하는 경우).
    cur 없이 부르면 자체 연결(init 의 CimsDatabase)."""
    if capability not in _CAPS:
        return False, 'unknown_capability'
    if cur is None:
        conn = _connect()
        if conn is None:
            role = BUILTIN_ROLES.get(principal[2]) if principal and principal[0] == 'console' and len(principal) > 2 else None
            return _decide(None, principal, role, capability, target)
        try:
            with conn:
                with conn.cursor() as c:
                    return _decide(c, principal, role_of(c, principal), capability, target)
        finally:
            conn.close()
    return _decide(cur, principal, role_of(cur, principal), capability, target)


def _decide(cur, principal, role, capability, target) -> Tuple[bool, str]:
    if role is None:
        return False, 'no_role'
    field, kind = _CAPS[capability]
    value = role.get(field)
    if field not in ENUMS:
        return (True, 'ok') if value else (False, 'denied')
    if capability == 'ptt_group.manage':
        value = effective_ptt_group_manage(role)
        if _owns_ptt_group(principal, target):                                      # 소유(§4.1) — 능력 값과 무관
            return True, 'ok'
    if value == 'none':
        return False, 'none'
    if target is None or value == 'all':
        return True, 'ok'
    tk = target.get('kind') if isinstance(target, dict) else None
    if kind == 'org':
        if tk != 'org':
            return False, 'out_of_scope'
        _m, _root, codes = org_scope(cur, role, field)
        return (True, 'ok') if in_codes(codes, target.get('code') or '') else (False, 'out_of_scope')
    if kind == 'phone_group':
        if tk != 'phone_group':
            return False, 'out_of_scope'
        gid = target.get('id')
        if value == 'own':
            own = _own_phone_group(cur, principal)
            return (True, 'ok') if own is not None and gid == own else (False, 'out_of_scope')
        mon, _ptt = role_targets(cur, role['id'])                                   # listed
        return (True, 'ok') if gid in mon else (False, 'out_of_scope')
    if kind == 'ptt_group':
        if tk != 'ptt_group':
            return False, 'out_of_scope'
        if capability == 'monitor.ptt':                                             # listed
            _mon, ptt = role_targets(cur, role['id'])
            return (True, 'ok') if target.get('id') in ptt else (False, 'out_of_scope')
        if _owns_ptt_group(principal, target):                                      # 소유(§4.1) — 범위 값과 무관
            return True, 'ok'
        if value == 'own':
            return False, 'out_of_scope'
        _m, _root, codes = org_scope(cur, role, 'directory_write')                   # scope
        return (True, 'ok') if in_codes(codes, target.get('org_code') or '') else (False, 'out_of_scope')
    return False, 'out_of_scope'


def _owns_ptt_group(principal, target) -> bool:
    """PTT 그룹 소유(authorized user, TS 23.280 — mcptt_authorization.md §4) — 가입자 principal 이 그룹의
    authorized_user_id 와 같으면 역할의 ptt_group_manage 값(own|scope|all)과 무관하게 수정·삭제할 수 있다(§4.1
    "소유자 **또는** 관리 범위"). 소유는 능력이 아니라 규격의 권리라 여기서 단락한다. 신규 생성(target 없음)에는 해당 없다."""
    if not principal or principal[0] != 'user' or not isinstance(target, dict):
        return False
    owner = target.get('authorized_user_id')
    return owner is not None and str(owner) == str(principal[1])


def forbidden(capability: str, scope: str = 'none', **extra):
    """403 본문(§2.3) — 관제 앱 문구 사전이 읽는 토큰."""
    from httpsrv.handler import HandlerResult
    body = {'error': 'forbidden', 'capability': capability, 'scope': scope or 'none'}
    body.update(extra)
    return HandlerResult(status=403, body=body)
