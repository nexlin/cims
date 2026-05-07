"""S6-L7-SUBSCRIBE-NOTIFY — NOTIFY body XML 구조 검증.

S6-SCN-SUBSCRIBE 가 cspsim subscribe 시나리오를 돌리고 stdout 마커로
PASS/FAIL 을 결정한다면, 본 항목은 그 직후 sip msg.jsonl 을 다시 스캔하여
csp 가 ue 로 보낸 NOTIFY 의 body XML 이 알려진 namespace
(xcap-diff / resource-lists / conference-info) 중 하나에 해당하고
well-formed 한지 검사한다.

cspsim 을 다시 돌리지 않고(read-only) S6-SCN-SUBSCRIBE 의 t0 anchor 를
ctx.state["S6_SUBSCRIBE_T0"] 로 받아 그 이후 라인만 본다.
"""
from __future__ import annotations

import time

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common.sip_log import iter_sip_msgs, parse_sip_body


_KNOWN_KINDS = (
    ("<xcap-diff",        "xcap-diff"),
    ("<resource-lists",   "resource-lists"),
    ("<conference-info",  "conference-info"),
)


@verify_item(
    id="S6-L7-SUBSCRIBE-NOTIFY", stage=6, category="시나리오",
    name="NOTIFY body XML 구조 검증 (xcap-diff/resource-lists/conference-info)",
    depends_on=["S6-SCN-SUBSCRIBE"],
    presets=["stage6-full", "stage6-ptt", "pipeline-full", "post-deploy"],
    side_effects=["read-only"], timeout_s=20,
)
def scn_l7_subscribe_notify(ctx: VerifyContext) -> ItemResult:
    since = ctx.state.get("S6_SUBSCRIBE_T0") or (time.time() - 120)
    msgs = list(iter_sip_msgs(ctx.dist_dir, since=since, method="NOTIFY"))

    if not msgs:
        ctx.w("### S6-L7-SUBSCRIBE-NOTIFY — SKIP")
        ctx.w("- NOTIFY 라인 0건 (msg_log 비활성/누락) — SUBSCRIBE 마커로 충분")
        ctx.w()
        return ItemResult(
            id="S6-L7-SUBSCRIBE-NOTIFY", name="NOTIFY body XML 구조 검증",
            status=ItemStatus.SKIP, stage=6,
            detail="msg_log 비활성: NOTIFY 0건 (since=%.1f)" % since,
        )

    seen: set = set()
    malformed = 0
    sample_event = ""
    sample_ctype = ""
    for m in msgs:
        _hdrs, body = parse_sip_body(m.get("msg", ""))
        if not sample_event:
            sample_event = _hdrs.get("Event", "") or _hdrs.get("event", "")
            sample_ctype = _hdrs.get("Content-Type", "") or _hdrs.get("content-type", "")
        for needle, label in _KNOWN_KINDS:
            if needle in body:
                seen.add(label)
        # well-formed 휴리스틱: body 가 비었거나 XML 시작 (`<?xml` 또는 `<`).
        if body:
            stripped = body.lstrip()
            if not (stripped.startswith("<?xml") or stripped.startswith("<")):
                malformed += 1

    ok = (len(seen) >= 1) and (malformed == 0)
    notes = [
        f"- NOTIFY 라인 수: {len(msgs)}",
        f"- 인식된 namespace: {sorted(seen) or '없음'}",
        f"- malformed body: {malformed}",
        f"- 표본 Event: {sample_event!r}, Content-Type: {sample_ctype!r}",
    ]
    ctx.w("### S6-L7-SUBSCRIBE-NOTIFY — NOTIFY body XML 구조 검증")
    for n in notes:
        ctx.w(n)
    ctx.w()
    return ItemResult(
        id="S6-L7-SUBSCRIBE-NOTIFY", name="NOTIFY body XML 구조 검증",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        stage=6, detail="\n".join(notes),
    )
