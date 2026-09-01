"""S3 호 전달·당겨받기 공용 헬퍼 (volte_supplementary_services.md §5·§6).

같은 org 의 VOIP 가입자 N명(A,B,C[,D])을 골라 cspsim `-creds` 인자로 만들고, cspsim 이
시나리오 말미에 출력하는 판정 마커(누적 수신 RTP delta, `pickup_status=`, `dialog_sub_status=`,
`refer_status=`, `SUBSCRIBE-EVENT result`)를 파싱한다. 미디어 판정이 RTP delta 인 이유:
전달/픽업 후 살아남는 상대는 착신/재-INVITE leg 라 m_bInCall 플래그가 UAS 경로에서 신뢰되지
않는다 — 실제 미디어가 재고정된 relay 로 흐르는지가 정본 신호다.

당겨받기 그룹 축(§5.1): `pickup_group` 컬럼이 있으면 PickupGroupFixture 가 A,B,C 에 같은 그룹,
D 에 다른 그룹을 명시 부여(DB UPDATE + CSP USER_CHANGED)해 **실컬럼 축**으로 검증하고 종료 시
원값을 복원한다(자기복원 — 공유 DB 안전). 컬럼이 없으면(마이그레이션 미적용) 전원 org 폴백이라
같은 org 3명이면 픽업이 성립하고, 그룹 경계(403/404) 검사는 SKIP 으로 보고한다.
"""
from __future__ import annotations

import json
import os
import re
import tempfile
import time

from ...common import db as _db
from ...common.subscribers import (
    has_column, get_pickup_group, set_pickup_group,
)
from ...common.csp_notify import notify_csp_event

# scn_srtp 와 동일 상수 (common.subscribers)
VOLTE_DOMAIN = "ims.mnc033.mcc450.3gppnetwork.org"
VOLTE_TABLE = "volte_subscriptions"


def select_same_org(dist_dir: str, n: int = 3) -> tuple:
    """같은 org 의 UDP/digest VOIP 가입자 n명 → (creds[list of dict], org). 부족하면 ([], "")."""
    cfg = _db.csp_db_config(dist_dir)
    if not cfg:
        return [], ""
    try:
        conn = _db.connect(cfg)
    except Exception:
        return [], ""
    try:
        cur = conn.cursor()
        cur.execute(
            "SELECT u.org_id, s.id, s.imsi, COALESCE(s.ha1,'') "
            "FROM volte_subscriptions s JOIN users u ON u.id = s.user_id "
            "WHERE s.id LIKE '+%' AND COALESCE(s.ha1,'')<>'' AND s.service_ref<>'' AND s.imsi<>'' "
            "  AND COALESCE(s.sip_transport,'') IN ('','UDP') AND COALESCE(s.auth_scheme,'digest')='digest' "
            "ORDER BY u.org_id, s.id"
        )
        by_org: dict = {}
        for org, sid, imsi, ha1 in cur.fetchall():
            by_org.setdefault(org or "", []).append((sid, imsi, ha1))
        for org, rows in by_org.items():
            if len(rows) >= n:
                creds = [{"user": r[0], "authId": r[1] or "", "ha1": r[2] or ""} for r in rows[:n]]
                return creds, org
        return [], ""
    finally:
        try:
            conn.close()
        except Exception:
            pass


def select_trio(dist_dir: str) -> tuple:
    return select_same_org(dist_dir, 3)


def trio_cred_args(creds: list, tag: str) -> list:
    """creds → cspsim 자격 인자. JSONL 파일 기록 후 ["-no-db","-creds",path,"-users_from_creds"].
    cspsim 은 파일 순서대로 sessions[0..] 을 전개하므로 (A,B,C) 배치는 creds 순서로 정한다."""
    path = os.path.join(tempfile.gettempdir(), f"cims_verify_creds_{tag}.jsonl")
    with open(path, "w", encoding="utf-8") as f:
        for c in creds:
            f.write(json.dumps({"user": c["user"], "authId": c.get("authId", ""), "ha1": c.get("ha1", "")},
                               ensure_ascii=False) + "\n")
    return ["-no-db", "-creds", path, "-users_from_creds"]


def notify_user_changed(csp_ip: str, user: str) -> None:
    """DB 플립을 CSP 가입자 캐시에 즉시 반영 (CscInterface USER_CHANGED → ReloadFromDb).
    4421 은 primary local_node bind_ip 에 바인딩되므로 CSP 접속 IP 로 보낸다."""
    notify_csp_event("USER_CHANGED", uri=f"tel:{user}", action="PUT", ip=csp_ip)


class PickupGroupFixture:
    """가입자별 pickup_group 명시 부여 + 자기복원 컨텍스트.

    assignments: {user: group}. 컬럼이 없으면 아무것도 바꾸지 않고 `active=False`(org 폴백 축).
    """

    def __init__(self, dist_dir: str, csp_ip: str, assignments: dict):
        self.db_cfg = _db.csp_db_config(dist_dir)
        self.csp_ip = csp_ip
        self.assignments = dict(assignments)
        self.active = False
        self.axis = "org 폴백(pickup_group 컬럼 부재)"
        self._orig: dict = {}

    def __enter__(self):
        try:
            if not has_column(self.db_cfg, VOLTE_TABLE, "pickup_group"):
                return self
        except Exception as e:  # DB 접근 실패 — 폴백 축으로 계속 (항목이 SKIP/FAIL 을 판단)
            self.axis = f"org 폴백(컬럼 확인 실패: {type(e).__name__})"
            return self
        for user, group in self.assignments.items():
            self._orig[user] = get_pickup_group(self.db_cfg, VOLTE_TABLE, user)
            set_pickup_group(self.db_cfg, VOLTE_TABLE, user, group)
            notify_user_changed(self.csp_ip, user)
        self.active = True
        self.axis = "pickup_group 컬럼(명시 부여)"
        time.sleep(0.5)  # ReloadFromDb 반영 여유
        return self

    def __exit__(self, *exc):
        for user, orig in self._orig.items():
            try:
                set_pickup_group(self.db_cfg, VOLTE_TABLE, user, orig)
                notify_user_changed(self.csp_ip, user)
            except Exception:
                pass
        return False


_DELTA_RE = re.compile(r"RTP recv delta over \d+s: A=\+(\d+) B=\+(\d+) C=\+(\d+)")


def parse_recv_delta(text: str):
    """cspsim 출력에서 'RTP recv delta ... A=+X B=+Y C=+Z' → (X,Y,Z). 없으면 None (시나리오 미완)."""
    last = None
    for line in text.splitlines():
        m = _DELTA_RE.search(line)
        if m:
            last = m
    if not last:
        return None
    return int(last.group(1)), int(last.group(2)), int(last.group(3))


def parse_marker_int(text: str, key: str):
    """cspsim 결과 마커의 `key=<int>` 마지막 값. 없으면 None."""
    last = None
    for m in re.finditer(rf"\b{re.escape(key)}=(\d+)", text):
        last = int(m.group(1))
    return last


# 미디어 흐름 임계 — AMR-WB 20ms = 50 pps. 관찰 창(기본 4s) 중 재고정 leg 로 흘렀다면 수백 건.
#   보수적으로 40(≈0.8s) 이상을 "흐름", 20 미만을 "무흐름(드롭)" 으로 본다.
FLOW_MIN = 40
DROP_MAX = 20


def fmt_checks(checks: list) -> str:
    """(name, ok|None, detail) 목록 → 결과 detail 문자열. ok=None 은 SKIP."""
    def tag(ok):
        return "SKIP" if ok is None else ("PASS" if ok else "FAIL")
    return "\n".join(f"{tag(ok)} {name}: {dt}" for name, ok, dt in checks)


def emit_checks(ctx, checks: list) -> bool:
    """체크 목록 출력 + 전체 판정(SKIP 제외 전부 PASS 면 True)."""
    for name, ok, detail in checks:
        tag = "SKIP" if ok is None else ("PASS" if ok else "FAIL")
        ctx.w(f"- [{tag}] {name} — {detail}")
    return all(ok for _, ok, _ in checks if ok is not None)
