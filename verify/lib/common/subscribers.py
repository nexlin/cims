"""가입자 선택 helper — Phase 1/3 회귀 시드용.

VoLTE/PTT 회귀 시나리오가 사용할 가입자 1명씩 + PTT 그룹 1개 선택.
PTT 는 그룹 멤버 + imsi 숫자 형식 인 가입자 우선 (cspsim auth_id 자동 유도와 일치).

**cspsim 은 시작 가입자의 비밀번호 하나로 `-count` 명을 시뮬레이션**하고 번호만 1씩
증가시킨다(auth_id 는 계정별로 파생). 그래서 시작 가입자는 "번호가 연속이고 비밀번호가
같은" 구간에서 골라야 한다 — 일부 계정만 비밀번호가 다르면 2번째 단말부터 REGISTER 403
(digest 불일치)이 되고, 시나리오가 제품 결함처럼 실패한다.
"""
from __future__ import annotations

from . import db as _db


VOLTE_DOMAIN = "ims.mnc033.mcc450.3gppnetwork.org"
MCPTT_DOMAIN = "ptt.mnc033.mcc450.3gppnetwork.org"


def _num(msisdn: str) -> int:
    """'+821300000001' → 821300000001 (cspsim 의 번호 증가 규칙 기준). 실패 시 -1."""
    d = "".join(ch for ch in (msisdn or "") if ch.isdigit())
    return int(d) if d else -1


def pick_start_subscriber(rows: list, count: int) -> tuple:
    """번호가 연속이고 비밀번호가 같은 `count` 명 구간의 **첫 가입자** 행 반환.

    rows: (id, passwd, ...) 튜플 리스트 (id 오름차순).
    조건을 만족하는 구간이 없으면 첫 행을 그대로 돌려준다(종전 동작 — 단말 1개
    시나리오나 소규모 DB 에서는 이 선택이 여전히 유효하다).
    """
    if not rows:
        return ()
    if count <= 1:
        return rows[0]
    for i in range(len(rows) - count + 1):
        win = rows[i:i + count]
        pwd = win[0][1]
        if not pwd or any(r[1] != pwd for r in win):
            continue
        nums = [_num(r[0]) for r in win]
        if nums[0] < 0 or any(nums[j + 1] != nums[j] + 1 for j in range(len(nums) - 1)):
            continue
        return win[0]
    return rows[0]


def select_subscribers(db_cfg: dict, voip_count: int = 1, ptt_count: int = 1) -> dict:
    """회귀 시드용 가입자 선택.

    voip_count/ptt_count 는 그 가입자로 돌릴 시나리오의 cspsim `-count` 다 —
    그 수만큼 연속·동일 비밀번호인 구간의 첫 가입자를 고른다.
    """
    out = {
        "voip_user": "", "voip_pwd": "", "voip_imsi": "", "voip_ref": "",
        "ptt_user":  "", "ptt_pwd":  "", "ptt_imsi":  "", "ptt_ref":  "",
        "ptt_group": "",
    }
    if not db_cfg:
        return out
    try:
        conn = _db.connect(db_cfg)
    except Exception:
        return out
    try:
        cur = conn.cursor()
        cur.execute(
            "SELECT id,passwd,imsi,service_ref,COALESCE(ha1,'') FROM volte_subscriptions "
            "WHERE id LIKE '+%' AND passwd<>'' AND service_ref<>'' AND imsi<>'' "
            "ORDER BY id"
        )
        r = pick_start_subscriber(list(cur.fetchall()), voip_count)
        if r:
            out.update({"voip_user": r[0], "voip_pwd": r[1] or "",
                        "voip_imsi": r[2] or "", "voip_ref": r[3] or "", "voip_ha1": r[4] or ""})

        # PTT: 그룹 멤버 + imsi 숫자 형식 우선
        cur.execute(
            "SELECT s.id, s.passwd, s.imsi, s.service_ref, g.mcptt_group_id AS group_id, COALESCE(s.ha1,'') "
            "FROM ptt_subscriptions s "
            "JOIN ptt_group_members m ON m.user_id = s.id "
            "JOIN ptt_groups g ON g.id = m.group_id "
            "WHERE s.id LIKE '+%' AND s.passwd<>'' AND s.service_ref<>'' "
            "  AND s.imsi REGEXP '^[0-9]+$' "
            "ORDER BY g.mcptt_group_id, m.priority, s.id"
        )
        # 그룹은 종전대로 (mcptt_group_id, priority) 순의 첫 그룹을 쓰고, 그 그룹 안에서만
        # 번호순으로 연속·동일 비밀번호 구간을 찾는다 (cspsim 은 번호를 1씩 올린다).
        rows = list(cur.fetchall())
        first_group = rows[0][4] if rows else None
        r = pick_start_subscriber(sorted((x for x in rows if x[4] == first_group),
                                         key=lambda x: _num(x[0])), ptt_count)
        if r:
            out.update({"ptt_user": r[0], "ptt_pwd": r[1] or "",
                        "ptt_imsi": r[2] or "", "ptt_ref": r[3] or "",
                        "ptt_group": r[4], "ptt_ha1": r[5] or ""})
        else:
            # fallback: 첫 가입자 + 첫 그룹
            cur.execute(
                "SELECT id,passwd,imsi,service_ref,COALESCE(ha1,'') FROM ptt_subscriptions "
                "WHERE id LIKE '+%' AND passwd<>'' AND service_ref<>'' AND imsi<>'' "
                "ORDER BY id"
            )
            r = pick_start_subscriber(list(cur.fetchall()), ptt_count)
            if r:
                out.update({"ptt_user": r[0], "ptt_pwd": r[1] or "",
                            "ptt_imsi": r[2] or "", "ptt_ref": r[3] or "", "ptt_ha1": r[4] or ""})
            cur.execute("SELECT mcptt_group_id FROM ptt_groups ORDER BY mcptt_group_id LIMIT 1")
            r = cur.fetchone()
            if r:
                out["ptt_group"] = r[0]
    finally:
        try: conn.close()
        except Exception: pass
    return out
