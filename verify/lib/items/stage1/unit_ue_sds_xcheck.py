"""S1-UE-SDS-XCHECK — MCData SDS 코덱이 코어와 앱에서 같은 값을 쓴다.

같은 와이어(TS 24.282)를 코어 C++(`sdk/core/src/mcdata/sds_codec.h`)과 기존 Android 앱
Kotlin(`android/ptt-client/.../mcdata/McDataCodec.kt`)이 **따로 구현하고 있다.** 앱을 파사드로 옮기면
사본이 걷히지만(ue_sdk.md §5.3) 그때까지는 공존한다 — 한쪽만 고치면 단말끼리 메시지를 못 읽는데,
컴파일도 시험도 통과하므로 **현장에서야 드러난다**. 그 드리프트를 정적으로 막는다.
"""
from __future__ import annotations

import os
import re

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ._ue_common import p, skip, done, block

_ID = "S1-UE-SDS-XCHECK"
_NAME = "SDS 코덱 상수 대조 (코어 ↔ ptt-client)"

_CORE = ("sdk", "core", "src", "mcdata", "sds_codec.h")
_KT = ("android", "ptt-client", "src", "main", "java", "com", "cims", "ue", "ptt",
       "mcdata", "McDataCodec.kt")

# 코어 이름 → Kotlin 이름. 값이 다르면 두 단말이 서로의 메시지를 못 읽는다.
_PAIRS = {
    "kMsgSdsSignalling": "MSG_SDS_SIGNALLING",
    "kMsgFdSignalling": "MSG_FD_SIGNALLING",
    "kMsgDataPayload": "MSG_DATA_PAYLOAD",
    "kMsgSdsNotification": "MSG_SDS_NOTIFICATION",
    "kDispReqDelivery": "DISP_REQ_DELIVERY",
}
# 콘텐츠 타입 — 문자열이 어긋나면 상대가 본문을 해석하지 못한다.
_CT = ["application/vnd.3gpp.mcdata-info+xml",
       "application/vnd.3gpp.mcdata-signalling",
       "application/vnd.3gpp.mcdata-payload"]


def _const_int(src: str, pattern: str) -> "int | None":
    m = re.search(pattern, src)
    return int(m.group(1), 0) if m else None


@verify_item(
    id=_ID, stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=60,
    execution_order=64,
)
def unit_ue_sds_xcheck(ctx: VerifyContext) -> ItemResult:
    core_p, kt_p = p(ctx.repo_root, *_CORE), p(ctx.repo_root, *_KT)
    for f in (core_p, kt_p):
        if not os.path.isfile(f):
            return skip(_ID, _NAME, f"{os.path.relpath(f, ctx.repo_root)} 없음 — 사본이 걷혔으면 이 항목을 지운다")
    core = open(core_p, encoding="utf-8", errors="replace").read()
    kt = open(kt_p, encoding="utf-8", errors="replace").read()

    lines, bad = [], []
    for cn, kn in _PAIRS.items():
        cv = _const_int(core, rf"{cn}\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
        kv = _const_int(kt, rf"{kn}\s*=\s*(0x[0-9A-Fa-f]+|\d+)")
        mark = "OK" if (cv is not None and cv == kv) else "어긋남"
        lines.append(f"{cn}={cv} ↔ {kn}={kv}  {mark}")
        if cv is None or cv != kv:
            bad.append(f"{cn}({cv}) != {kn}({kv})")
    for ct in _CT:
        if ct not in kt:
            bad.append(f"콘텐츠 타입 누락: {ct}")
            lines.append(f"{ct}  앱에 없음")
        else:
            lines.append(f"{ct}  OK")

    block(ctx, f"{_ID} — SDS 코덱 대조", lines)
    return done(_ID, _NAME, not bad,
                "코어 ↔ 앱 상수 일치" if not bad else " / ".join(bad))
