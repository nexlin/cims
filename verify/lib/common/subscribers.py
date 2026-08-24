"""가입자 선택 helper — Phase 1/3 회귀 시드용.

VoLTE/PTT 회귀 시나리오가 사용할 가입자 창(window) + PTT 그룹 1개 선택.
PTT 는 그룹 멤버 + imsi 숫자 형식 인 가입자 우선 (cspsim auth_id 자동 유도와 일치).

cspsim 은 `-user` 시작번호부터 번호를 1씩 올려 `-count` 명을 전개한다. 자격은 단말별
자격 파일(`-creds`, [cred_args] 가 생성 — sip_access_security.md §4.7)로 각자 H(A1)/비번을
주는 것이 정본이라, 창 조건은 "번호 연속 + 전원 ha1 보유"면 충분하다. ha1 이 없는
구 DB 에서는 종전 조건("전원 동일한 비밀번호" — cspsim 이 `-password` 하나를 공유)으로
폴백한다 — 일부 계정만 비밀번호가 다르면 2번째 단말부터 REGISTER 403 (digest 불일치)이
되고, 시나리오가 제품 결함처럼 실패한다.
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
    """번호가 연속이고 자격이 일관된 `count` 명 창(window)의 행 리스트 반환.

    rows: (id, passwd, ...) 튜플 리스트 (id 오름차순). ha1_idx = 행에서 ha1 컬럼 위치.
    자격 일관 = 전원 ha1 보유(-creds 자격 파일 경로) **또는** 전원 동일한 비어있지 않은
    비밀번호(-password 공유 과도기 경로). 조건을 만족하는 구간이 없으면 첫 행 1개짜리
    창을 돌려준다(종전 동작 — 단말 1개 시나리오나 소규모 DB 에서는 이 선택이 여전히
    유효하고, count 미달 창은 [cred_args] 가 -password 폴백으로 처리한다).
    """
    if not rows:
        return []
    if count <= 1:
        return [rows[0]]
    for i in range(len(rows) - count + 1):
        win = rows[i:i + count]
        pwd = win[0][1]
        all_ha1 = all(r[ha1_idx] for r in win)
        same_pwd = bool(pwd) and all(r[1] == pwd for r in win)
        if not (all_ha1 or same_pwd):
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
    return [{"user": r[0], "authId": r[2] or "",
             "ha1": r[ha1_idx] or "", "password": r[1] or ""} for r in win]


def cred_args(state: dict, kind: str, count: int) -> list:
    """cspsim 자격 인자 — 단말별 자격 파일(-creds) 우선, 과도기 폴백은 -password.

    kind: "VOIP" | "PTT" (seed 가 ctx.state 에 실은 접두사 — {kind}_CREDS/{kind}_PWD).
    창이 count 명을 덮고 전원이 자격(ha1 또는 passwd)을 가지면 JSONL 자격 파일을 써서
    ["-no-db", "-creds", path] 반환 — cspsim 이 -user 시작번호부터 CLI 전개하며 단말별
    자격을 쓴다("같은 비밀번호 구간" 의존 소멸, sip_access_security.md §4.7).
    -no-db 를 함께 반환하는 이유: DB 모드는 -user 를 무시하고 DB 첫 N 행을 쓰므로
    시드 창과 어긋난다 — -creds 는 항상 CLI 전개와 짝이다.
    부족하면 종전 ["-password", pwd] (DB 모드/공유 비밀번호 과도기 경로).
    """
    creds = state.get(f"{kind}_CREDS") or []
    if len(creds) >= count and all(c.get("ha1") or c.get("password") for c in creds[:count]):
        path = os.path.join(tempfile.gettempdir(), f"cims_verify_creds_{kind.lower()}.jsonl")
        with open(path, "w", encoding="utf-8") as f:
            for c in creds[:count]:
                f.write(json.dumps({"user": c["user"], "authId": c.get("authId", ""),
                                    "ha1": c.get("ha1", ""), "password": c.get("password", "")},
                                   ensure_ascii=False) + "\n")
        return ["-no-db", "-creds", path]
    return ["-password", state.get(f"{kind}_PWD", "")]


def select_subscribers(db_cfg: dict, voip_count: int = 1, ptt_count: int = 1) -> dict:
    """회귀 시드용 가입자 선택.

    voip_count/ptt_count 는 그 가입자로 돌릴 시나리오의 cspsim `-count` 다 —
    그 수만큼 연속·동일 비밀번호인 구간의 첫 가입자를 고른다.
    """
    out = {
        "voip_user": "", "voip_pwd": "", "voip_imsi": "", "voip_ref": "",
        "ptt_user":  "", "ptt_pwd":  "", "ptt_imsi":  "", "ptt_ref":  "",
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
        # 자격 필터: passwd 또는 ha1 — 평문 소거(sip_access_security.md §4.7 ⑤) 후에는
        # ha1 만 남으므로 passwd<>'' 단독 조건이면 시드가 전멸한다.
        cur.execute(
            "SELECT id,passwd,imsi,service_ref,COALESCE(ha1,'') FROM volte_subscriptions "
            "WHERE id LIKE '+%' AND (passwd<>'' OR COALESCE(ha1,'')<>'') "
            "  AND service_ref<>'' AND imsi<>'' "
            "ORDER BY id"
        )
        win = pick_start_window(list(cur.fetchall()), voip_count, ha1_idx=4)
        if win:
            r = win[0]
            out.update({"voip_user": r[0], "voip_pwd": r[1] or "",
                        "voip_imsi": r[2] or "", "voip_ref": r[3] or "", "voip_ha1": r[4] or "",
                        "voip_creds": _window_creds(win, ha1_idx=4)})

        # PTT: 그룹 멤버 + imsi 숫자 형식 우선
        cur.execute(
            "SELECT s.id, s.passwd, s.imsi, s.service_ref, g.mcptt_group_id AS group_id, COALESCE(s.ha1,'') "
            "FROM ptt_subscriptions s "
            "JOIN ptt_group_members m ON m.user_id = s.id "
            "JOIN ptt_groups g ON g.id = m.group_id "
            "WHERE s.id LIKE '+%' AND (s.passwd<>'' OR COALESCE(s.ha1,'')<>'') "
            "  AND s.service_ref<>'' "
            "  AND s.imsi REGEXP '^[0-9]+$' "
            "ORDER BY g.mcptt_group_id, m.priority, s.id"
        )
        # 그룹은 종전대로 (mcptt_group_id, priority) 순의 첫 그룹을 쓰고, 그 그룹 안에서만
        # 번호순으로 연속·자격 일관 창을 찾는다 (cspsim 은 번호를 1씩 올린다).
        rows = list(cur.fetchall())
        first_group = rows[0][4] if rows else None
        win = pick_start_window(sorted((x for x in rows if x[4] == first_group),
                                       key=lambda x: _num(x[0])), ptt_count, ha1_idx=5)
        if win:
            r = win[0]
            out.update({"ptt_user": r[0], "ptt_pwd": r[1] or "",
                        "ptt_imsi": r[2] or "", "ptt_ref": r[3] or "",
                        "ptt_group": r[4], "ptt_ha1": r[5] or "",
                        "ptt_creds": _window_creds(win, ha1_idx=5)})
        else:
            # fallback: 첫 가입자 + 첫 그룹
            cur.execute(
                "SELECT id,passwd,imsi,service_ref,COALESCE(ha1,'') FROM ptt_subscriptions "
                "WHERE id LIKE '+%' AND (passwd<>'' OR COALESCE(ha1,'')<>'') "
                "  AND service_ref<>'' AND imsi<>'' "
                "ORDER BY id"
            )
            win = pick_start_window(list(cur.fetchall()), ptt_count, ha1_idx=4)
            if win:
                r = win[0]
                out.update({"ptt_user": r[0], "ptt_pwd": r[1] or "",
                            "ptt_imsi": r[2] or "", "ptt_ref": r[3] or "", "ptt_ha1": r[4] or "",
                            "ptt_creds": _window_creds(win, ha1_idx=4)})
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
