"""S1-UE-ENGINE-SINGLE — 엔진이 레포에 두 벌로 있지 않다.

pjsua2 산출물(`org.pjsip.**` + `libpjsua2.so`)은 `:cimsue-engine` 하나가 낸다
(android_dispatch_tablet.md §2.2). 예전처럼 `android/core/src/pjsua2` 에 생성물을 커밋해 두면
`ext/pjproject` 패치를 두 곳에 반영해야 하고, 커밋본이 조용히 어긋난다 — 그 어긋남은 런타임에만
드러난다. 그래서 «커밋된 산출물이 없다» 와 «제공처가 하나다» 를 정적으로 못박는다.
"""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ._ue_common import p, done, block

_ID = "S1-UE-ENGINE-SINGLE"
_NAME = "엔진 단일 제공처 (커밋 산출물 부재)"

# 다시 생기면 안 되는 자리 — 과거에 커밋돼 있던 생성물.
_FORBIDDEN = [
    ("android", "core", "src", "pjsua2"),
    ("android", "core", "src", "main", "jniLibs", "arm64-v8a", "libpjsua2.so"),
]
# org.pjsip 을 제공해도 되는 유일한 모듈.
_ENGINE_MOD = ("sdk", "android", "cimsue-engine")


@verify_item(
    id=_ID, stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=60,
    execution_order=63,
)
def unit_ue_engine_single(ctx: VerifyContext) -> ItemResult:
    bad = [os.path.join(*x) for x in _FORBIDDEN if os.path.exists(p(ctx.repo_root, *x))]

    # org.pjsip 자바 소스를 가진 모듈을 센다 — 엔진 모듈 하나여야 한다.
    providers = set()
    for mod in ("android", "sdk/android"):
        root = p(ctx.repo_root, *mod.split("/"))
        if not os.path.isdir(root):
            continue
        for cur, dirs, files in os.walk(root):
            dirs[:] = [d for d in dirs if d not in ("build", ".git", ".gradle")]
            if os.path.basename(cur) == "pjsip" and "org" in cur.split(os.sep):
                rel = os.path.relpath(cur, ctx.repo_root)
                providers.add(rel.split(os.sep + "src" + os.sep)[0])

    engine = os.path.join(*_ENGINE_MOD)
    stray = sorted(x for x in providers if x != engine)

    lines = [f"금지 경로 잔존: {len(bad)}개"] + [f"  - {x}" for x in bad]
    lines += [f"org.pjsip 제공 모듈: {sorted(providers) or '없음'}"]
    block(ctx, f"{_ID} — 엔진 단일화", lines)

    ok = not bad and not stray
    if ok:
        return done(_ID, _NAME, True, f"커밋 산출물 0 · 제공처 {sorted(providers) or '[빌드 전]'}")
    why = []
    if bad:
        why.append("커밋된 엔진 산출물: " + ", ".join(bad))
    if stray:
        why.append("엔진 모듈 밖 org.pjsip: " + ", ".join(stray))
    return done(_ID, _NAME, False, " / ".join(why))
