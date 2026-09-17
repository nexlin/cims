"""S1-UE-CSC-XCHECK — CSC(XCAP·IdMS) 경로가 코어와 앱에서 같다.

SDS 와 같은 이유의 공존 방어다(ue_sdk.md §5.3). 경로가 어긋나면 한쪽 단말만 404 를 받는데,
서버는 정상이고 컴파일도 통과하므로 원인을 찾기 어렵다.
"""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ._ue_common import p, skip, done, block

_ID = "S1-UE-CSC-XCHECK"
_NAME = "CSC 경로 대조 (코어 ↔ ptt-client)"

# **경로는 양쪽 모두 여러 파일에 흩어져 있다.** 한 파일만 읽으면 «없다» 가 되어 거짓 FAIL 이 난다 —
# 코어는 인라인 래퍼가 헤더에 있고(`csc.h`), 앱은 XCAP·IdMS 를 ptt-client 가, 프로비저닝을 :core 가 든다.
_CORE = [
    ("sdk", "core", "src", "csc", "csc_client.cpp"),
    ("sdk", "core", "include", "cimsue", "csc.h"),
]
_APP = [
    ("android", "ptt-client", "src", "main", "java", "com", "cims", "ue", "ptt", "csc", "CscClient.kt"),
    ("android", "core", "src", "main", "java", "com", "cims", "ue", "core", "provision",
     "ProvisioningClient.kt"),
]

# 양쪽에 그대로 있어야 하는 경로 조각(TS 24.481/24.484·android_ue_provisioning.md §3).
_PATHS = [
    "/idms/authreq",
    "/idms/tokenreq",
    "/provisioning/me",
    "/org.openmobilealliance.groups/users/",
    "/org.3gpp.mcptt.user-profile/users/",
    "/org.3gpp.mcptt.service-config/users/",
]


def _read_all(repo_root: str, groups) -> "tuple[str, list[str]]":
    """지목한 파일을 이어 붙인다. 없는 파일은 이름만 돌려준다(SKIP 판정용)."""
    import os as _os
    text, missing = [], []
    for parts in groups:
        f = _os.path.join(repo_root, *parts)
        if _os.path.isfile(f):
            text.append(open(f, encoding="utf-8", errors="replace").read())
        else:
            missing.append(_os.path.join(*parts))
    return "\n".join(text), missing


@verify_item(
    id=_ID, stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=60,
    execution_order=65,
)
def unit_ue_csc_xcheck(ctx: VerifyContext) -> ItemResult:
    core, miss_c = _read_all(ctx.repo_root, _CORE)
    app, miss_a = _read_all(ctx.repo_root, _APP)
    if miss_c or miss_a:
        return skip(_ID, _NAME,
                    "대조 대상 없음: " + ", ".join(miss_c + miss_a) +
                    " — 앱이 파사드로 옮겨져 사본이 걷혔으면 이 항목을 지운다(ue_sdk.md §5.3)")

    lines, bad = [], []
    for path in _PATHS:
        in_core, in_app = path in core, path in app
        lines.append(f"{path}  코어={'O' if in_core else 'X'} 앱={'O' if in_app else 'X'}")
        if not (in_core and in_app):
            bad.append(f"{path} (코어={in_core}, 앱={in_app})")

    block(ctx, f"{_ID} — CSC 경로 대조", lines)
    return done(_ID, _NAME, not bad,
                f"경로 {len(_PATHS)}개 일치" if not bad else "어긋남: " + " / ".join(bad))
