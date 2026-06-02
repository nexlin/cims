"""가입자 선택 helper — Phase 1/3 회귀 시드용.

VoLTE/PTT 회귀 시나리오가 사용할 가입자 1명씩 + PTT 그룹 1개 선택.
PTT 는 그룹 멤버 + imsi 숫자 형식 인 가입자 우선 (cspsim auth_id 자동 유도와 일치).
"""
from __future__ import annotations

from . import db as _db


VOLTE_DOMAIN = "ims.mnc033.mcc450.3gppnetwork.org"
MCPTT_DOMAIN = "ptt.mnc033.mcc450.3gppnetwork.org"


def select_subscribers(db_cfg: dict) -> dict:
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
            "SELECT id,passwd,imsi,service_ref FROM volte_subscriptions "
            "WHERE id LIKE '+%' AND passwd<>'' AND service_ref<>'' AND imsi<>'' "
            "ORDER BY id LIMIT 1"
        )
        r = cur.fetchone()
        if r:
            out.update({"voip_user": r[0], "voip_pwd": r[1] or "",
                        "voip_imsi": r[2] or "", "voip_ref": r[3] or ""})

        # PTT: 그룹 멤버 + imsi 숫자 형식 우선
        cur.execute(
            "SELECT s.id, s.passwd, s.imsi, s.service_ref, g.mcptt_group_id AS group_id "
            "FROM ptt_subscriptions s "
            "JOIN ptt_group_members m ON m.user_id = s.id "
            "JOIN ptt_groups g ON g.id = m.group_id "
            "WHERE s.id LIKE '+%' AND s.passwd<>'' AND s.service_ref<>'' "
            "  AND s.imsi REGEXP '^[0-9]+$' "
            "ORDER BY g.mcptt_group_id, m.priority, s.id LIMIT 1"
        )
        r = cur.fetchone()
        if r:
            out.update({"ptt_user": r[0], "ptt_pwd": r[1] or "",
                        "ptt_imsi": r[2] or "", "ptt_ref": r[3] or "",
                        "ptt_group": r[4]})
        else:
            # fallback: 첫 가입자 + 첫 그룹
            cur.execute(
                "SELECT id,passwd,imsi,service_ref FROM ptt_subscriptions "
                "WHERE id LIKE '+%' AND passwd<>'' AND service_ref<>'' AND imsi<>'' "
                "ORDER BY id LIMIT 1"
            )
            r = cur.fetchone()
            if r:
                out.update({"ptt_user": r[0], "ptt_pwd": r[1] or "",
                            "ptt_imsi": r[2] or "", "ptt_ref": r[3] or ""})
            cur.execute("SELECT mcptt_group_id FROM ptt_groups ORDER BY mcptt_group_id LIMIT 1")
            r = cur.fetchone()
            if r:
                out["ptt_group"] = r[0]
    finally:
        try: conn.close()
        except Exception: pass
    return out
