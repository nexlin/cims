"""S6-CMP-GROUP-SYNC — admin → CSP → CMP roster 동기화 검증.

흐름:
  1. 배포본 csc(target=verify→4445, prod→4420) admin login.
  2. 임시 PTT 그룹 (gid="verify-cmp-<ms>") POST /api/v1/ptt/groups.
     csc → notify_csp(UDP) → CSP → CMP `addGroup` 전파.
  3. 1~5 초 폴링: CMP 9000/UDP `STATS_REQUEST` 응답의
     `response.group_details[].group_id` 에 신규 gid 등장 확인.
  4. cleanup: DELETE 임시 그룹.

S6-SCN-DB-SYNC 가 CSP 로그에서 GROUP_CHANGED 라인을 보는 반면, 본 항목은
CMP 가 실제로 group roster 를 업데이트했는지 deterministic 응답으로 본다.
새 backend endpoint 추가 없이 CMP 가 이미 노출하는 control-port 명령만 활용.
"""
from __future__ import annotations

import os
import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common import csc_http
from ...common.cmp_client import cmp_request
from ._helpers import target_ip


_TARGET_CSC_PORTS = {"verify": 4445, "prod": 4420}


def _deployed_csc_base(ctx: VerifyContext) -> str:
    target = (ctx.opts or {}).get("target") or "verify"
    port = _TARGET_CSC_PORTS.get(target, 4445)
    return f"https://127.0.0.1:{port}"


def _stats_has_gid(resp: dict, gid: str) -> bool:
    if not isinstance(resp, dict):
        return False
    inner = resp.get("response")
    if not isinstance(inner, dict):
        return False
    details = inner.get("group_details") or []
    if not isinstance(details, list):
        return False
    for g in details:
        if isinstance(g, dict) and g.get("group_id") == gid:
            return True
    return False


@verify_item(
    id="S6-CMP-GROUP-SYNC", stage=6, category="시나리오",
    name="CMP roster sync (admin → CSP → CMP STATS group_details)",
    depends_on=["S6-SCN-DB-SYNC"],
    presets=["stage6-full", "pipeline-full", "post-deploy"],
    side_effects=["db-write", "network"], timeout_s=20,
    execution_order=81,
)
def scn_cmp_group_sync(ctx: VerifyContext) -> ItemResult:
    notes: list = []
    base = _deployed_csc_base(ctx)
    login_id = os.environ.get("CIMS_TB_ADMIN_ID", "admin")
    pw = os.environ.get("CIMS_TB_ADMIN_PASSWORD", "1234")
    # PTT 그룹 동기화는 PMP 미디어를 검증 (CSP 의 PTT_AS 가 PSP 로 분리된 P1 토폴로지).
    # ctx.state["CMP_IP"]/CMP_PORT 가 명시적 주어지지 않으면 _INSTANCES 의 pmp 사용.
    cmp_ip = ctx.state.get("CMP_IP") or target_ip("pmp", "127.0.0.1")
    cmp_port = int(ctx.state.get("CMP_PORT") or 9000)

    try:
        tok = csc_http.admin_login(base, login_id, pw, timeout=5)
    except Exception as e:
        return _skip(ctx, f"csc({base}) login 예외: {type(e).__name__}: {e}")
    if not tok:
        return _skip(ctx, f"csc({base}) login 실패 — S5 미실행?")
    notes.append(f"- login: {base} OK")

    # CMP 사전 reachability — 응답 못 받으면 SKIP
    pre = cmp_request({"cmd": "STATS_REQUEST", "sesid": "verify-precheck"},
                      ip=cmp_ip, port=cmp_port, timeout=1.0)
    if pre is None:
        return _skip(ctx, f"CMP {cmp_ip}:{cmp_port} STATS 응답 없음 — 미기동/방화벽?")
    notes.append(f"- CMP {cmp_ip}:{cmp_port} STATS 응답 OK "
                 f"(groups_before={(pre.get('response') or {}).get('groups')})")

    # 멤버는 빈 list — CMP roster 생성/등록 검증이 목적이고,
    # csc admin POST 의 members 형식은 dict-list 라 단순 string-list 는 거부.
    gid = f"verify-cmp-{int(time.time() * 1000)}"
    payload = {"id": gid, "name": gid, "members": []}

    try:
        st, body = csc_http.post_json(
            f"{base}/api/v1/ptt/groups", payload, token=tok, timeout=10,
        )
    except Exception as e:
        return _skip(ctx, f"POST /ptt/groups 예외: {type(e).__name__}: {e}")
    if st not in (200, 201):
        return _fail(ctx, f"POST /ptt/groups status={st} body={str(body)[:200]}")
    notes.append(f"- 임시 그룹 추가: gid={gid} status={st} members=0")

    found = False
    last_resp = None
    poll_max = 10  # PSP→PMP roster sync 가 1~2s 이내 일반적이지만 인스턴스 분리
                   # 환경에서는 GROUP_CHANGED notify → PSP 처리 → CmpClient 발송
                   # 으로 chain 이 길어져 5s 내 못 잡는 회차 발견. 10s 로 안전 마진.
    try:
        for i in range(poll_max):
            time.sleep(1)
            last_resp = cmp_request(
                {"cmd": "STATS_REQUEST", "sesid": f"verify-poll-{i}"},
                ip=cmp_ip, port=cmp_port, timeout=1.0,
            )
            if _stats_has_gid(last_resp or {}, gid):
                found = True
                notes.append(f"- CMP STATS 매칭: poll #{i+1} ({i+1}s 경과)")
                break
        if not found:
            inner = (last_resp or {}).get("response") or {}
            notes.append(
                f"- CMP STATS 미매칭 ({poll_max}s) — groups={inner.get('groups')} "
                f"detail_len={len(inner.get('group_details') or [])}"
            )
    finally:
        try:
            del_st = csc_http.delete(f"{base}/api/v1/ptt/groups/{gid}", token=tok)
            notes.append(f"- 임시 그룹 삭제: gid={gid} status={del_st}")
        except Exception as e:
            notes.append(f"- [WARN] cleanup 실패: {type(e).__name__}: {e}")

    ctx.w("### S6-CMP-GROUP-SYNC — CMP roster 동기화")
    for n in notes:
        ctx.w(n)
    ctx.w()
    return ItemResult(
        id="S6-CMP-GROUP-SYNC", name="CMP roster sync",
        status=ItemStatus.PASS if found else ItemStatus.FAIL,
        stage=6, detail="\n".join(notes),
    )


def _skip(ctx: VerifyContext, reason: str) -> ItemResult:
    ctx.w("### S6-CMP-GROUP-SYNC — SKIP")
    ctx.w(f"- {reason}")
    ctx.w()
    return ItemResult(
        id="S6-CMP-GROUP-SYNC", name="CMP roster sync",
        status=ItemStatus.SKIP, stage=6, detail=reason,
    )


def _fail(ctx: VerifyContext, msg: str) -> ItemResult:
    ctx.w("### S6-CMP-GROUP-SYNC — FAIL")
    ctx.w(f"- {msg}")
    ctx.w()
    return ItemResult(
        id="S6-CMP-GROUP-SYNC", name="CMP roster sync",
        status=ItemStatus.FAIL, stage=6, detail=msg,
    )
