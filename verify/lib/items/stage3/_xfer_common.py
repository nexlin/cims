"""S3 호 전달·당겨받기 공용 헬퍼 (volte_supplementary_services.md §5·§6).

같은 org 의 VOIP 가입자 3명(A,B,C)을 골라 cspsim `-creds` 인자로 만들고, cspsim 이
시나리오 말미에 출력하는 누적 수신 RTP delta 라인을 파싱한다. 판정 기준이 RTP delta 인
이유: 전달/픽업 후 살아남는 상대는 착신/재-INVITE leg 라 m_bInCall 플래그가 UAS 경로에서
신뢰되지 않는다 — 실제 미디어가 재고정된 relay 로 흐르는지가 정본 신호다.

당겨받기 그룹 축: dev DB 에 pickup_group 컬럼이 없으면 org 폴백이므로(EffectivePickupGroup)
같은 org 3명이면 픽업이 성립한다. 컬럼이 있어도 값이 비면 동일하게 org 폴백.
"""
from __future__ import annotations

import json
import os
import re
import tempfile

from ...common import db as _db

# scn_srtp 와 동일 상수 (common.subscribers)
VOLTE_DOMAIN = "ims.mnc033.mcc450.3gppnetwork.org"


def select_trio(dist_dir: str) -> tuple:
    """같은 org 의 UDP/digest VOIP 가입자 3명 → (creds[list of dict], org). 부족하면 ([], "")."""
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
            if len(rows) >= 3:
                creds = [{"user": r[0], "authId": r[1] or "", "ha1": r[2] or ""} for r in rows[:3]]
                return creds, org
        return [], ""
    finally:
        try:
            conn.close()
        except Exception:
            pass


def trio_cred_args(creds: list, tag: str) -> list:
    """3명 creds → cspsim 자격 인자. JSONL 파일 기록 후 ["-no-db","-creds",path,"-users_from_creds"]."""
    path = os.path.join(tempfile.gettempdir(), f"cims_verify_creds_{tag}.jsonl")
    with open(path, "w", encoding="utf-8") as f:
        for c in creds:
            f.write(json.dumps({"user": c["user"], "authId": c.get("authId", ""), "ha1": c.get("ha1", "")},
                               ensure_ascii=False) + "\n")
    return ["-no-db", "-creds", path, "-users_from_creds"]


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


# 미디어 흐름 임계 — AMR-WB 20ms = 50 pps. 관찰 창(기본 4s) 중 재고정 leg 로 흘렀다면 수백 건.
#   보수적으로 40(≈0.8s) 이상을 "흐름", 20 미만을 "무흐름(드롭)" 으로 본다.
FLOW_MIN = 40
DROP_MAX = 20
