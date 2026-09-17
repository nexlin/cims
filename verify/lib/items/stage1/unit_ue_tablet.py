"""S1-UE-TABLET-UNIT — 관제 태블릿 앱의 JVM 단위시험.

기기 없이 돌아가는 것만 여기 있다(android_dispatch_tablet.md §9) — 프로파일 파싱, 포커스/발언 대상
분리, 발언 소유권, 관리 와이어 파서, 회선 저장 판정, 이력 날짜 창/시간대 밴드/발언 막대, dialog 결합,
응답 문구 사전, E.164 정규화. 기기가 필요한 판정(감청 SSRC 귀속·오디오 분리·화면 밀도)은 실기기 행이다.

Android SDK 가 없으면 SKIP — S1 은 정적 검사 stage 라 모든 개발 장비에 SDK 가 있다고 전제하지 않는다.
"""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ... import shell
from ._ue_common import p, skip, done, block

_ID = "S1-UE-TABLET-UNIT"
_NAME = "관제 태블릿 단위시험 (gradlew testDebugUnitTest)"
_MODULES = ["dispatch-tablet", "cimsue"]


def _sdk_root() -> str:
    for k in ("ANDROID_SDK_ROOT", "ANDROID_HOME"):
        v = os.environ.get(k)
        if v and os.path.isdir(v):
            return v
    return ""


@verify_item(
    id=_ID, stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=1800,
    execution_order=66,
)
def unit_ue_tablet(ctx: VerifyContext) -> ItemResult:
    gradlew = p(ctx.repo_root, "android", "gradlew")
    if not os.path.isfile(gradlew):
        return skip(_ID, _NAME, "android/gradlew 없음")
    if not _sdk_root() and not os.path.isfile(p(ctx.repo_root, "android", "local.properties")):
        return skip(_ID, _NAME,
                    "Android SDK 없음 — ANDROID_SDK_ROOT 설정 또는 android/local.properties 필요 "
                    "(docs/DEV_SERVER_SETUP.md)")

    rc, out, err = shell.run(
        [gradlew, "--offline", "-q", ":dispatch-tablet:testDebugUnitTest", ":cimsue:testDebugUnitTest"],
        cwd=p(ctx.repo_root, "android"), timeout=1800)
    full = (out + err).splitlines()

    # 결과 XML 에서 실제 건수를 센다 — gradle 의 «UP-TO-DATE» 에 속지 않게 산출물을 직접 읽는다.
    import re
    total = fails = errs = 0
    found = []
    for mod, base in (("dispatch-tablet", ("android", "dispatch-tablet")),
                      ("cimsue", ("sdk", "android", "cimsue"))):
        d = p(ctx.repo_root, *base, "build", "test-results", "testDebugUnitTest")
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if not f.endswith(".xml"):
                continue
            src = open(os.path.join(d, f), encoding="utf-8", errors="replace").read(4096)
            def g(name: str) -> int:
                m = re.search(rf'{name}="(\d+)"', src)
                return int(m.group(1)) if m else 0
            total += g("tests"); fails += g("failures"); errs += g("errors")
        found.append(mod)

    block(ctx, f"{_ID} — 태블릿 단위시험", full + [f"집계: {total} tests / {fails} failures / {errs} errors"])
    if rc != 0:
        return done(_ID, _NAME, False, f"gradle rc={rc} · {total} tests / {fails} failures / {errs} errors")
    if not found:
        return skip(_ID, _NAME, "시험 결과 XML 없음 — 빌드가 시험을 돌리지 않았다")
    ok = fails == 0 and errs == 0 and total > 0
    return done(_ID, _NAME, ok, f"{total} tests / {fails} failures / {errs} errors ({'·'.join(found)})")
