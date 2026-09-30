"""S1-UE-ANDROID-BIND — SWIG 바인딩이 앱에 «불투명 타입» 을 새지 않는다.

SWIG 가 C++ 타입을 다루지 못하면 `SWIGTYPE_p_*` 라는 이름의 껍데기 클래스를 낸다. 그것이 생성물에
남았다는 것은 **그 API 를 Java 에서 쓸 수 없다**는 뜻인데, 컴파일은 통과하므로 조용히 지나간다
(ue_sdk.md §5.1 — 앱은 `com.cims.ue.sdk.*` 만 쓰고 손 JNI 는 두지 않는다).

`HttpResult.body` 가 `byte[]` 인지도 함께 본다 — `String` 이면 NUL 에서 잘려 녹취 MP4 가 깨진다
(android_dispatch_tablet.md §3.4 이진 typemap).

`cimsue.i` 의 타입맵 본문에 `%#` 없이 쓴 전처리 지시문(`#if`·`#else`·`#endif` …)도 잡는다 — SWIG 전처리기가
바인딩 생성 때(호스트, `__ANDROID__` 없음) 먼저 평가해 한 갈래만 남기므로, 플랫폼 분기가 생성 코드에서
사라진다(수신 영상 창 `setVideoWindow` 가 늘 NULL 이던 원인 — ue_sdk.md §5.1).
"""
from __future__ import annotations

import os
import re

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ._ue_common import p, skip, done, block

_ID = "S1-UE-ANDROID-BIND"
_NAME = "SWIG 바인딩 불투명 타입 부재 + 이진 본문"
_PKG = ("sdk", "android", "cimsue", "src", "swig", "java", "com", "cims", "ue", "sdk", "jni")
_IFACE = ("sdk", "core", "swig", "cimsue.i")
_DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b")


def _typemap_directives(path: str) -> list[str]:
    """타입맵 본문 `%typemap(...) { ... }` 안의 맨 `#` 지시문(`%#` 아님) — "줄번호: 내용"."""
    bad, depth = [], 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for no, line in enumerate(f, 1):
            s = line.rstrip("\n")
            if depth == 0:
                if s.lstrip().startswith("%typemap(") and s.rstrip().endswith("{"):
                    depth = 1
                continue
            if _DIRECTIVE.match(s):
                bad.append(f"{no}: {s.strip()}")
            depth += s.count("{") - s.count("}")
    return bad


@verify_item(
    id=_ID, stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=60,
    execution_order=62,
)
def unit_ue_android_bind(ctx: VerifyContext) -> ItemResult:
    d = p(ctx.repo_root, *_PKG)
    if not os.path.isdir(d):
        return skip(_ID, _NAME,
                    "SWIG 생성 Java 없음 — `sdk/android/build-native.sh` 후 재실행 "
                    "(생성물은 커밋 대상이다)")
    files = sorted(f for f in os.listdir(d) if f.endswith(".java"))
    if not files:
        return skip(_ID, _NAME, "생성 Java 0개 — 빌드가 산출물을 내지 않았다")

    opaque = [f for f in files if f.startswith("SWIGTYPE_p_")]

    # HttpResult.body 게터가 byte[] 인가 — String 이면 NUL 에서 잘린다.
    body_ok, body_why = True, "HttpResult.body=byte[]"
    hr = os.path.join(d, "HttpResult.java")
    if os.path.isfile(hr):
        src = open(hr, encoding="utf-8", errors="replace").read()
        if "byte[] getBody()" not in src:
            body_ok = False
            body_why = "HttpResult.getBody() 가 byte[] 가 아니다 — 녹취 MP4 가 NUL 에서 잘린다"
    else:
        body_ok, body_why = False, "HttpResult.java 없음"

    iface = p(ctx.repo_root, *_IFACE)
    directives = _typemap_directives(iface) if os.path.isfile(iface) else []

    lines = [f"생성 Java: {len(files)}개", f"불투명 타입(SWIGTYPE_p_*): {len(opaque)}개", body_why,
             f"타입맵 본문 맨 # 지시문: {len(directives)}개 (%# 로 써야 생성 코드에 남는다)"]
    lines += [f"  - {x}" for x in opaque[:10]]
    lines += [f"  - cimsue.i {x}" for x in directives[:10]]
    block(ctx, f"{_ID} — SWIG 바인딩", lines)

    ok = not opaque and body_ok and not directives
    detail = (f"Java {len(files)}개 · 불투명 0 · {body_why} · 타입맵 지시문 0" if ok
              else f"불투명 {len(opaque)}개 " + (", ".join(opaque[:5])) + f" / {body_why}"
                   + (f" / 타입맵 맨 # 지시문 {len(directives)}개 ({directives[0]})" if directives else ""))
    return done(_ID, _NAME, ok, detail)
