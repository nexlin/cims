"""S1-UE-ENGINE-SINGLE — 엔진이 레포에 두 벌로 있지 않다.

pjsua2 산출물(`org.pjsip.pjsua2.**` + `libpjsua2.so`)은 `:cimsue-engine` 하나가 낸다
(android_dispatch_tablet.md §2.2). 예전처럼 `android/core/src/pjsua2` 에 생성물을 커밋해 두면
`ext/pjproject` 패치를 두 곳에 반영해야 하고, 커밋본이 조용히 어긋난다 — 그 어긋남은 런타임에만
드러난다. 그래서 «커밋된 산출물이 없다» 와 «제공처가 하나다» 를 정적으로 못박는다.

카메라 도우미(`org.pjsip.PjCamera2`·`PjCameraInfo2` — pjmedia Android 영상 장치가 FindClass 하는 앱 클래스)는
pj 를 싣는 .so 가 **자기 APK 안에서** 찾아야 하므로 pj 를 싣는 두 모듈(`:cimsue-engine`·`:cimsue`)이 빌드 때
같은 원천(`ext/pjproject/pjmedia/src/pjmedia-videodev/android`)에서 복사한다 — 커밋하지 않으므로 어긋날 사본이 없다.
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
# org.pjsip.pjsua2 를 제공해도 되는 유일한 모듈.
_ENGINE_MOD = ("sdk", "android", "cimsue-engine")
# 카메라 도우미(org/pjsip/PjCamera*.java)를 빌드 때 복사해 싣는 모듈 — pj 를 싣는 두 .so 의 짝.
_CAMERA_MODS = (("sdk", "android", "cimsue-engine"), ("sdk", "android", "cimsue"))


@verify_item(
    id=_ID, stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=60,
    execution_order=63,
)
def unit_ue_engine_single(ctx: VerifyContext) -> ItemResult:
    bad = [os.path.join(*x) for x in _FORBIDDEN if os.path.exists(p(ctx.repo_root, *x))]

    # org.pjsip 자바 소스를 가진 모듈을 센다 — 바인딩(org/pjsip/pjsua2)은 엔진 모듈 하나, 카메라 도우미는 pj 를 싣는 두 모듈.
    providers, camera = set(), set()
    stray_files = []
    for mod in ("android", "sdk/android"):
        root = p(ctx.repo_root, *mod.split("/"))
        if not os.path.isdir(root):
            continue
        for cur, dirs, files in os.walk(root):
            dirs[:] = [d for d in dirs if d not in ("build", ".git", ".gradle")]
            parts = cur.split(os.sep)
            if "org" not in parts or "pjsip" not in parts:
                continue
            modrel = os.path.relpath(cur, ctx.repo_root).split(os.sep + "src" + os.sep)[0]
            if os.path.basename(cur) == "pjsua2" and parts[-2] == "pjsip":
                providers.add(modrel)
            elif os.path.basename(cur) == "pjsip":
                for f in files:
                    if f.startswith("PjCamera") and f.endswith(".java"):
                        camera.add(modrel)
                    else:
                        stray_files.append(os.path.join(os.path.relpath(cur, ctx.repo_root), f))

    engine = os.path.join(*_ENGINE_MOD)
    camera_ok = {os.path.join(*m) for m in _CAMERA_MODS}
    stray = sorted(x for x in providers if x != engine) + sorted(x for x in camera if x not in camera_ok) + sorted(stray_files)

    lines = [f"금지 경로 잔존: {len(bad)}개"] + [f"  - {x}" for x in bad]
    lines += [f"org.pjsip.pjsua2 제공 모듈: {sorted(providers) or '없음'}",
              f"카메라 도우미(PjCamera*) 모듈: {sorted(camera) or '없음'}"]
    block(ctx, f"{_ID} — 엔진 단일화", lines)

    ok = not bad and not stray
    if ok:
        return done(_ID, _NAME, True, f"커밋 산출물 0 · 제공처 {sorted(providers) or '[빌드 전]'}")
    why = []
    if bad:
        why.append("커밋된 엔진 산출물: " + ", ".join(bad))
    if stray:
        why.append("허용 밖 org.pjsip: " + ", ".join(stray))
    return done(_ID, _NAME, False, " / ".join(why))
