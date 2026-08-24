"""가입자 선택 helper — Phase 1/3 회귀 시드용.

VoLTE/PTT 회귀 시나리오가 사용할 가입자 창(window) + PTT 그룹 1개 선택.
PTT 는 그룹 멤버 + imsi 숫자 형식 인 가입자 우선 (cspsim auth_id 자동 유도와 일치).

cspsim 은 `-user` 시작번호부터 번호를 1씩 올려 `-count` 명을 전개한다. 자격은 단말별
자격 파일(`-creds`, [cred_args] 가 생성 — sip_access_security.md §4.7)로 각자 H(A1) 을
주는 것이 정본이라, 창 조건은 "번호 연속 + 전원 ha1 보유"다. DB 에 평문 passwd 는 없다
(컬럼 DROP — §4.7 ⑥): ha1 이 빈 가입자는 시드 대상이 아니다.
"""
from __future__ import annotations

import json
import os
import tempfile

from . import db as _db


VOLTE_DOMAIN = "ims.mnc033.mcc450.3gppnetwork.org"
MCPTT_DOMAIN = "ptt.mnc033.mcc450.3gppnetwork.org"


def _num(msisdn: str) -> int:
    """'+821300000001' → 821300000001 (cspsim 의 번호 증가 규칙 기준). 실패 시 -1."""
    d = "".join(ch for ch in (msisdn or "") if ch.isdigit())
    return int(d) if d else -1


def pick_start_window(rows: list, count: int, ha1_idx: int) -> list:
    """번호가 연속이고 전원 ha1 을 가진 `count` 명 창(window)의 행 리스트 반환.

    rows: (id, imsi, ...) 튜플 리스트 (id 오름차순). ha1_idx = 행에서 ha1 컬럼 위치.
    조건을 만족하는 구간이 없으면 첫 행 1개짜리 창을 돌려준다(단말 1개 시나리오나 소규모
    DB 에서는 이 선택이 여전히 유효하고, count 미달은 [cred_args] 가 빈 인자로 드러낸다).
    """
    if not rows:
        return []
    if count <= 1:
        return [rows[0]]
    for i in range(len(rows) - count + 1):
        win = rows[i:i + count]
        if not all(r[ha1_idx] for r in win):
            continue
        nums = [_num(r[0]) for r in win]
        if nums[0] < 0 or any(nums[j + 1] != nums[j] + 1 for j in range(len(nums) - 1)):
            continue
        return win
    return [rows[0]]


def _window_creds(win: list, ha1_idx: int) -> list:
    """창 행 → 단말별 자격 dict 리스트 (seed 가 ctx.state 의 {KIND}_CREDS 로 적재).

    authId = DB imsi(bare) — cspsim 이 -domain 을 붙여 IMPI 로 조립한다.
    """
    return [{"user": r[0], "authId": r[1] or "", "ha1": r[ha1_idx] or ""} for r in win]


def cred_args(state: dict, kind: str, count: int) -> list:
    """cspsim 자격 인자 — 단말별 자격 파일(-creds).

    kind: "VOIP" | "PTT" (seed 가 ctx.state 에 실은 접두사 — {kind}_CREDS).
    창이 count 명을 덮고 전원이 ha1 을 가지면 JSONL 자격 파일을 써서 ["-no-db", "-creds", path]
    반환 — cspsim 이 -user 시작번호부터 CLI 전개하며 단말별 자격을 쓴다(sip_access_security.md §4.7).
    -no-db 를 함께 반환하는 이유: DB 모드는 -user 를 무시하고 DB 첫 N 행을 쓰므로 시드 창과
    어긋난다 — -creds 는 항상 CLI 전개와 짝이다.
    부족하면 빈 리스트 — 자격 없는 전개는 cspsim 이 REGISTER 를 보내지 않아 항목이 실패로 드러난다
    (평문 -password 폴백은 없다: DB 에 passwd 가 없다, §4.7 ⑥).
    """
    creds = state.get(f"{kind}_CREDS") or []
    if len(creds) >= count and all(c.get("ha1") for c in creds[:count]):
        path = os.path.join(tempfile.gettempdir(), f"cims_verify_creds_{kind.lower()}.jsonl")
        with open(path, "w", encoding="utf-8") as f:
            for c in creds[:count]:
                f.write(json.dumps({"user": c["user"], "authId": c.get("authId", ""),
                                    "ha1": c.get("ha1", "")}, ensure_ascii=False) + "\n")
        return ["-no-db", "-creds", path]
    return []


def select_subscribers(db_cfg: dict, voip_count: int = 1, ptt_count: int = 1) -> dict:
    """회귀 시드용 가입자 선택.

    voip_count/ptt_count 는 그 가입자로 돌릴 시나리오의 cspsim `-count` 다 —
    그 수만큼 연속·전원 ha1 보유인 구간의 첫 가입자를 고른다.
    """
    out = {
        "voip_user": "", "voip_imsi": "", "voip_ref": "", "voip_ha1": "",
        "ptt_user":  "", "ptt_imsi":  "", "ptt_ref":  "", "ptt_ha1":  "",
        "ptt_group": "", "voip_creds": [], "ptt_creds": [],
    }
    if not db_cfg:
        return out
    try:
        conn = _db.connect(db_cfg)
    except Exception:
        return out
    try:
        cur = conn.cursor()
        # 자격 필터: ha1 — 평문 passwd 컬럼은 없다(sip_access_security.md §4.7 ⑥).
        cur.execute(
            "SELECT id,imsi,service_ref,COALESCE(ha1,'') FROM volte_subscriptions "
            "WHERE id LIKE '+%' AND COALESCE(ha1,'')<>'' "
            "  AND service_ref<>'' AND imsi<>'' "
            "ORDER BY id"
        )
        win = pick_start_window(list(cur.fetchall()), voip_count, ha1_idx=3)
        if win:
            r = win[0]
            out.update({"voip_user": r[0], "voip_imsi": r[1] or "", "voip_ref": r[2] or "",
                        "voip_ha1": r[3] or "", "voip_creds": _window_creds(win, ha1_idx=3)})

        # PTT: 그룹 멤버 + imsi 숫자 형식 우선
        cur.execute(
            "SELECT s.id, s.imsi, s.service_ref, g.mcptt_group_id AS group_id, COALESCE(s.ha1,'') "
            "FROM ptt_subscriptions s "
            "JOIN ptt_group_members m ON m.user_id = s.id "
            "JOIN ptt_groups g ON g.id = m.group_id "
            "WHERE s.id LIKE '+%' AND COALESCE(s.ha1,'')<>'' "
            "  AND s.service_ref<>'' "
            "  AND s.imsi REGEXP '^[0-9]+$' "
            "ORDER BY g.mcptt_group_id, m.priority, s.id"
        )
        # 그룹은 종전대로 (mcptt_group_id, priority) 순의 첫 그룹을 쓰고, 그 그룹 안에서만
        # 번호순으로 연속·전원 ha1 창을 찾는다 (cspsim 은 번호를 1씩 올린다).
        rows = list(cur.fetchall())
        first_group = rows[0][3] if rows else None
        win = pick_start_window(sorted((x for x in rows if x[3] == first_group),
                                       key=lambda x: _num(x[0])), ptt_count, ha1_idx=4)
        if win:
            r = win[0]
            out.update({"ptt_user": r[0], "ptt_imsi": r[1] or "", "ptt_ref": r[2] or "",
                        "ptt_group": r[3], "ptt_ha1": r[4] or "",
                        "ptt_creds": _window_creds(win, ha1_idx=4)})
        else:
            # fallback: 첫 가입자 + 첫 그룹
            cur.execute(
                "SELECT id,imsi,service_ref,COALESCE(ha1,'') FROM ptt_subscriptions "
                "WHERE id LIKE '+%' AND COALESCE(ha1,'')<>'' "
                "  AND service_ref<>'' AND imsi<>'' "
                "ORDER BY id"
            )
            win = pick_start_window(list(cur.fetchall()), ptt_count, ha1_idx=3)
            if win:
                r = win[0]
                out.update({"ptt_user": r[0], "ptt_imsi": r[1] or "", "ptt_ref": r[2] or "",
                            "ptt_ha1": r[3] or "", "ptt_creds": _window_creds(win, ha1_idx=3)})
            cur.execute("SELECT mcptt_group_id FROM ptt_groups ORDER BY mcptt_group_id LIMIT 1")
            r = cur.fetchone()
            if r:
                out["ptt_group"] = r[0]
    finally:
        try: conn.close()
        except Exception: pass
    return out


# ─────────────────────────────────────────────────────────────
# 채널 정책 프로브용 — sip_transport 정책 읽기/쓰기 (S3 게이트 검증)
# ─────────────────────────────────────────────────────────────
def get_transport_policy(db_cfg: dict, table: str, user: str):
    """`table`(volte_subscriptions|ptt_subscriptions) 의 user 행 sip_transport 반환.

    행이 없으면 None, 값이 NULL 이면 '' 반환 (구분 필요 — 복원 시 NULL 로 되돌린다).
    """
    if not db_cfg or not user:
        return None
    conn = _db.connect(db_cfg)
    try:
        cur = conn.cursor()
        cur.execute(f"SELECT sip_transport FROM {table} WHERE id=%s", (user,))
        row = cur.fetchone()
        if row is None:
            return None
        return row[0] if row[0] is not None else ""
    finally:
        try: conn.close()
        except Exception: pass


def set_transport_policy(db_cfg: dict, table: str, user: str, value) -> bool:
    """user 행 sip_transport 를 value 로 설정. value=None 이면 NULL 로 되돌린다."""
    if not db_cfg or not user:
        return False
    conn = _db.connect(db_cfg)
    try:
        cur = conn.cursor()
        if value is None or value == "":
            cur.execute(f"UPDATE {table} SET sip_transport=NULL WHERE id=%s", (user,))
        else:
            cur.execute(f"UPDATE {table} SET sip_transport=%s WHERE id=%s", (value, user))
        return cur.rowcount >= 0
    finally:
        try: conn.close()
        except Exception: pass
