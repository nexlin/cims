"""S3 관제 회귀 공용 픽스처 — 전화 그룹 + 역할 시드·자기복원 (dispatch_center.md §3·§8.1·§9).

전화 그룹(`phone_groups`/`phone_group_members` — 픽업 축 + 대표번호)과 역할(`roles`/`role_assignments`/
`role_monitor_targets`/`role_ptt_targets` — 감청·청취 범위)을 **DB 에 직접 시드**하고 CSP 에 UDP 통지
(`PHONE_GROUP_CHANGED`/`ROLE_CHANGED`/`USER_CHANGED`)로 캐시를 맞춘 뒤, 종료 시 시드한 것을 전부 지우고
멤버 회선의 종전 그룹·pickup_group·person 의 종전 역할 배정을 되돌린다(공유 DB 안전).

역할 배정의 principal 은 **person(users.id)** 이다 — 회선이 아니다. 픽스처는 회선 id 로 받은 배정 대상을
`volte_subscriptions`/`ptt_subscriptions.user_id` 로 person 에 풀어 배정하고, CSP 는 그 person 의 전 회선으로
펼친다(§3.5). person 이 없는 회선(user_id NULL)은 배정할 수 없어 픽스처가 비활성(reason)으로 끝난다.

스키마 프로브(전환기 공존):
  · `phone_groups` + `role_assignments` 있음 → 신 스키마(SCHEMA_ROLES)
  · 없고 `dispatch_groups` 있음         → 전환 전 스키마(SCHEMA_DISPATCH) — 관제 그룹 한 엔티티에 범위 열을
                                          실어 같은 의미로 시드(역할 배정 회선 = 그 그룹 멤버)
  · 둘 다 없음                          → active=False, reason = NO_TABLE_REASON (항목은 SKIP)
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field

from ...common import db as _db
from ...common.subscribers import has_column
from ...common.csp_notify import notify_csp_event

SCHEMA_ROLES = "roles"        # phone_groups + roles (migrate_phone_groups_roles.sql)
SCHEMA_DISPATCH = "dispatch"  # dispatch_groups (전환 전 — migrate_dispatch_groups.sql)

NO_TABLE_REASON = "phone_groups/roles 테이블 부재 (migrate_phone_groups_roles.sql 미적용)"

_SUB_TABLES = ("volte_subscriptions", "ptt_subscriptions")


def _has_table(cur, table: str) -> bool:
    cur.execute("SHOW TABLES LIKE %s", (table,))
    return cur.fetchone() is not None


def _one(row, key: str):
    """pymysql 튜플/딕셔너리 커서 양쪽에서 첫 컬럼 값."""
    if row is None:
        return None
    return row[0] if isinstance(row, tuple) else row.get(key)


def probe_schema(db_cfg: dict) -> tuple:
    """(schema, reason). schema = SCHEMA_ROLES | SCHEMA_DISPATCH | None(reason 에 사유)."""
    if not db_cfg:
        return None, "DB 설정 없음 (csp.json Setup.Database)"
    try:
        conn = _db.connect(db_cfg)
    except Exception as e:
        return None, f"DB 확인 실패: {type(e).__name__}"
    try:
        with conn.cursor() as cur:
            pg, ra = _has_table(cur, "phone_groups"), _has_table(cur, "role_assignments")
            if pg and ra:
                return SCHEMA_ROLES, ""
            if pg or ra:
                return None, ("phone_groups/roles 스키마 일부만 존재 (%s 부재) — migrate_phone_groups_roles.sql 재적용"
                              % ("role_assignments" if pg else "phone_groups"))
            if _has_table(cur, "dispatch_groups"):
                return SCHEMA_DISPATCH, ""
            return None, NO_TABLE_REASON
    finally:
        conn.close()


def person_of(db_cfg: dict, line: str):
    """회선 id → person(users.id) 문자열. 회선 없음·user_id NULL 이면 None."""
    conn = _db.connect(db_cfg)
    try:
        with conn.cursor() as cur:
            return _person_of(cur, line)
    finally:
        conn.close()


def _person_of(cur, line: str):
    for table in _SUB_TABLES:
        cur.execute(f"SELECT user_id FROM {table} WHERE id=%s", (line,))
        r = cur.fetchone()
        if r is not None:
            v = _one(r, "user_id")
            return None if v is None else str(v)
    return None


def _line_table(cur, line: str):
    """회선 id 가 든 가입 테이블 이름(volte/ptt). 없으면 None."""
    for table in _SUB_TABLES:
        cur.execute(f"SELECT 1 FROM {table} WHERE id=%s", (line,))
        if cur.fetchone() is not None:
            return table
    return None


@dataclass
class RoleSpec:
    """역할 1개 + 배정 대상 회선 — 신 스키마는 roles/role_assignments, 전환 전 스키마는 관제 그룹의 범위 열.

    monitor_targets = 전화 그룹 id 목록(monitor_call=listed), ptt_targets = mcptt_group_id 목록(ptt_listen=listed —
    ptt_groups.id 로 풀어 넣는다). lines = 배정할 회선(→ person). 전환 전 스키마에서는 lines 가 그 그룹 멤버가 된다.
    """
    id: str
    lines: list = field(default_factory=list)
    monitor_call: str = "none"
    monitor_targets: list = field(default_factory=list)
    ptt_listen: str = "none"
    ptt_targets: list = field(default_factory=list)
    listen_visibility: str = "hidden"


class DispatchFixture:
    """전화 그룹 1개(선택) + 역할 1개(선택) 시드 + 자기복원 컨텍스트.

    group_id 가 비면 신 스키마에서는 전화 그룹을 만들지 않는다(역할만 — 감시자·청취자는 그룹 소속이 필요 없다).
    전환 전 스키마에서는 역할이 관제 그룹의 속성이라 `group_id or role.id` 로 관제 그룹을 만들고 역할 배정
    회선을 멤버로 넣는다. members 는 멤버 행 + 회선의 pickup_group(CSC 파생값과 같음) 을 함께 시드한다.
    unassign = "역할 없음" 을 판정하는 회선 — 그 person 의 기존 배정을 픽스처 동안 걷어 두었다가 복원한다(신 스키마;
    전환 전 스키마는 멤버십이 곧 범위라 그룹 범위 열 none 으로 충분). 테이블 부재·person 없음 등이면 active=False +
    reason (시드 도중 실패는 그때까지 것을 되돌린다).
    """

    def __init__(self, dist_dir: str, csp_ip: str, group_id: str = "", *, pilot: str = "", members=(),
                 overflow: str = "", no_answer_sec: int = 8, alert_mode: str = "parallel",
                 role: RoleSpec | None = None, unassign=()):
        self.db_cfg = _db.csp_db_config(dist_dir)
        self.csp_ip = csp_ip
        self.group_id = group_id
        self.pilot = pilot
        self.members = list(members)
        self.overflow = overflow
        self.no_answer_sec = no_answer_sec
        self.alert_mode = alert_mode
        self.role = role
        self.unassign = list(unassign)
        self.schema = None
        self.active = False
        self.reason = ""
        self.persons: dict = {}            # 배정 회선 → person id (로그·판정 보조)
        # 복원용 원값
        self._orig_member: dict = {}       # line → (group_id, alert_order) | None
        self._orig_pickup: dict = {}       # line → (table, value)
        self._orig_assign: dict = {}       # person → prior role_id | None
        self._touched_groups: set = set()  # 멤버를 뺏어 온 종전 그룹 — 재적재 통지 대상
        self._seeded_group = ""
        self._seeded_role = ""
        self._pickup_col: dict = {}        # table → pickup_group 컬럼 유무

    # ── 진입/종료 ──
    def __enter__(self):
        self.schema, self.reason = probe_schema(self.db_cfg)
        if self.schema is None:
            return self
        try:
            conn = _db.connect(self.db_cfg)
            try:
                with conn.cursor() as cur:
                    if self.schema == SCHEMA_ROLES:
                        self._seed_roles_schema(cur)
                    else:
                        self._seed_dispatch_schema(cur)
            finally:
                conn.close()
        except Exception as e:
            self.reason = f"시드 실패: {type(e).__name__}: {e}"
            self._restore()
            return self
        self._notify(seeded=True)
        self.active = True
        time.sleep(0.5)  # CSP 재적재 여유
        return self

    def __exit__(self, *exc):
        if self.active:
            self._restore()
            self._notify(seeded=False)
            self.active = False
        return False

    # ── 공통 조각 ──
    def _legacy_group_id(self) -> str:
        return self.group_id or (self.role.id if self.role else "")

    def _legacy_members(self) -> list:
        out = list(self.members)
        for line in (self.role.lines if self.role else []):
            if line not in out:
                out.append(line)
        return out

    def _has_pickup_col(self, table: str) -> bool:
        if table not in self._pickup_col:
            try:
                self._pickup_col[table] = has_column(self.db_cfg, table, "pickup_group")
            except Exception:
                self._pickup_col[table] = False
        return self._pickup_col[table]

    def _set_pickup(self, cur, line: str, group_id: str) -> None:
        """회선의 pickup_group 을 그룹 id 로 (원값 보관). CSC 가 멤버십에서 파생하는 값과 같다."""
        table = _line_table(cur, line)
        if not table or not self._has_pickup_col(table):
            return
        cur.execute(f"SELECT pickup_group FROM {table} WHERE id=%s", (line,))
        self._orig_pickup[line] = (table, _one(cur.fetchone(), "pickup_group"))
        cur.execute(f"UPDATE {table} SET pickup_group=%s WHERE id=%s", (group_id, line))

    def _seed_members(self, cur, member_table: str, group_id: str, members: list) -> None:
        for i, line in enumerate(members):
            cur.execute(f"SELECT group_id, alert_order FROM {member_table} WHERE user_id=%s", (line,))
            r = cur.fetchone()
            if r is None:
                self._orig_member[line] = None
            else:
                prior = (r[0], r[1]) if isinstance(r, tuple) else (r["group_id"], r["alert_order"])
                self._orig_member[line] = prior
                if prior[0] and prior[0] != group_id:
                    self._touched_groups.add(prior[0])
            cur.execute(f"INSERT INTO {member_table} (user_id, group_id, alert_order) VALUES (%s,%s,%s) "
                        "ON DUPLICATE KEY UPDATE group_id=VALUES(group_id), alert_order=VALUES(alert_order)",
                        (line, group_id, i))
            self._set_pickup(cur, line, group_id)

    def _ptt_group_pk(self, cur, mcptt_group_id: str):
        cur.execute("SELECT id FROM ptt_groups WHERE mcptt_group_id=%s", (mcptt_group_id,))
        return _one(cur.fetchone(), "id")

    # ── 신 스키마: phone_groups + roles ──
    def _seed_roles_schema(self, cur) -> None:
        for line in self.unassign:  # "역할 없음" 판정 회선 — person 의 기존 배정을 걷어 둔다
            person = _person_of(cur, line)
            if person is None or person in self._orig_assign:
                continue
            self.persons[line] = person
            cur.execute("SELECT role_id FROM role_assignments WHERE principal_type='user' AND principal_id=%s", (person,))
            self._orig_assign[person] = _one(cur.fetchone(), "role_id")
            cur.execute("DELETE FROM role_assignments WHERE principal_type='user' AND principal_id=%s", (person,))
        if self.group_id:
            cur.execute("DELETE FROM phone_groups WHERE id=%s", (self.group_id,))  # 잔재(이전 회차 중단) 정리
            cur.execute(
                "INSERT INTO phone_groups (id, name, pilot_id, service_ref, alert_mode, no_answer_sec, busy_members, "
                "overflow_target) VALUES (%s,%s,%s,%s,%s,%s,'skip',%s)",
                (self.group_id, f"verify {self.group_id}", self.pilot or None, "volte" if self.pilot else None,
                 self.alert_mode, self.no_answer_sec, self.overflow or None))
            self._seeded_group = self.group_id
            self._seed_members(cur, "phone_group_members", self.group_id, self.members)
        r = self.role
        if r is None:
            return
        cur.execute("DELETE FROM roles WHERE id=%s", (r.id,))  # 대상·배정 CASCADE
        cur.execute(
            "INSERT INTO roles (id, name, builtin, monitor_call, ptt_listen, listen_visibility, history_read) "
            "VALUES (%s,%s,0,%s,%s,%s,'scope')",
            (r.id, f"verify {r.id}", r.monitor_call, r.ptt_listen, r.listen_visibility))
        self._seeded_role = r.id
        for pg in r.monitor_targets:
            cur.execute("INSERT IGNORE INTO role_monitor_targets (role_id, phone_group_id) VALUES (%s,%s)", (r.id, pg))
        for grp in r.ptt_targets:
            pk = self._ptt_group_pk(cur, grp)
            if pk is None:
                raise ValueError(f"ptt_groups 에 {grp} 없음 (role_ptt_targets)")
            cur.execute("INSERT IGNORE INTO role_ptt_targets (role_id, ptt_group_id) VALUES (%s,%s)", (r.id, pk))
        for line in r.lines:
            person = _person_of(cur, line)
            if person is None:
                raise ValueError(f"회선 {line} 의 person(users.id) 없음 — 역할 배정 불가")
            self.persons[line] = person
            if person in self._orig_assign:
                continue
            cur.execute("SELECT role_id FROM role_assignments WHERE principal_type='user' AND principal_id=%s", (person,))
            self._orig_assign[person] = _one(cur.fetchone(), "role_id")
            cur.execute("INSERT INTO role_assignments (principal_type, principal_id, role_id) VALUES ('user',%s,%s) "
                        "ON DUPLICATE KEY UPDATE role_id=VALUES(role_id)", (person, r.id))

    # ── 전환 전 스키마: dispatch_groups (범위 열 동반) ──
    def _seed_dispatch_schema(self, cur) -> None:
        gid = self._legacy_group_id()
        if not gid:
            return
        r = self.role
        cur.execute("DELETE FROM dispatch_groups WHERE id=%s", (gid,))
        cur.execute(
            "INSERT INTO dispatch_groups (id, name, pilot_id, service_ref, alert_mode, no_answer_sec, busy_members, "
            "overflow_target, monitor_scope, ptt_listen, listen_visibility) VALUES (%s,%s,%s,%s,%s,%s,'skip',%s,%s,%s,%s)",
            (gid, f"verify {gid}", self.pilot or None, "volte" if self.pilot else None, self.alert_mode,
             self.no_answer_sec, self.overflow or None,
             r.monitor_call if r else "none", r.ptt_listen if r else "none", r.listen_visibility if r else "hidden"))
        self._seeded_group = gid
        self._seed_members(cur, "dispatch_group_members", gid, self._legacy_members())
        if r is None:
            return
        for pg in r.monitor_targets:
            cur.execute("INSERT IGNORE INTO dispatch_group_monitor_targets (group_id, target_group_id) VALUES (%s,%s)",
                        (gid, pg))
        for grp in r.ptt_targets:
            pk = self._ptt_group_pk(cur, grp)
            if pk is None:
                raise ValueError(f"ptt_groups 에 {grp} 없음 (dispatch_group_ptt_targets)")
            cur.execute("INSERT IGNORE INTO dispatch_group_ptt_targets (group_id, ptt_group_id) VALUES (%s,%s)", (gid, pk))

    # ── 복원 ──
    def _restore(self) -> None:
        try:
            conn = _db.connect(self.db_cfg)
        except Exception:
            return
        legacy = self.schema == SCHEMA_DISPATCH
        group_table = "dispatch_groups" if legacy else "phone_groups"
        member_table = "dispatch_group_members" if legacy else "phone_group_members"

        def run(sql, args):
            try:
                with conn.cursor() as cur:
                    cur.execute(sql, args)
            except Exception:
                pass  # 한 문장 실패가 나머지 복원을 막지 않게

        try:
            # 배정 → 종전 역할로 되돌리거나 제거 (역할 삭제 전에 — CASCADE 로 종전 배정까지 잃지 않게)
            for person, prior in self._orig_assign.items():
                if prior:
                    run("INSERT INTO role_assignments (principal_type, principal_id, role_id) VALUES ('user',%s,%s) "
                        "ON DUPLICATE KEY UPDATE role_id=VALUES(role_id)", (person, prior))
                else:
                    run("DELETE FROM role_assignments WHERE principal_type='user' AND principal_id=%s", (person,))
            if self._seeded_role:
                run("DELETE FROM roles WHERE id=%s", (self._seeded_role,))  # 대상 CASCADE
            if self._seeded_group:
                run(f"DELETE FROM {group_table} WHERE id=%s", (self._seeded_group,))  # 멤버·대상 CASCADE
            for line, prior in self._orig_member.items():
                if prior and prior[0]:
                    run(f"INSERT IGNORE INTO {member_table} (user_id, group_id, alert_order) VALUES (%s,%s,%s)",
                        (line, prior[0], prior[1] or 0))
            for line, (table, value) in self._orig_pickup.items():
                run(f"UPDATE {table} SET pickup_group=%s WHERE id=%s", (value, line))
        finally:
            conn.close()

    # ── CSP 통지 ──
    def _notify(self, seeded: bool) -> None:
        action = "POST" if seeded else "DELETE"
        ip = self.csp_ip
        if self.schema == SCHEMA_DISPATCH:
            gid = self._legacy_group_id()
            if gid:
                notify_csp_event("DISPATCH_GROUP_CHANGED", uri=gid, action=action, ip=ip)
            for prior in self._touched_groups:
                notify_csp_event("DISPATCH_GROUP_CHANGED", uri=prior, action="PUT", ip=ip)
            lines = self._legacy_members()
        else:
            if self.group_id:
                notify_csp_event("PHONE_GROUP_CHANGED", uri=self.group_id, action=action, ip=ip)
            for prior in self._touched_groups:
                notify_csp_event("PHONE_GROUP_CHANGED", uri=prior, action="PUT", ip=ip)
            if self.role is not None or self._orig_assign:  # 배정 변경 = 역할 맵 전량 재적재 (uri 는 로그용)
                notify_csp_event("ROLE_CHANGED", uri=self.role.id if self.role else "", action=action, ip=ip)
            lines = self.members
        for line in lines:  # pickup_group 파생값 → 회선 캐시 재적재
            notify_csp_event("USER_CHANGED", uri=f"tel:{line}", action="PUT", ip=ip)
