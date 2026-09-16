"""S1-UNIT-CONSOLE-LAYOUT — 콘솔 레이아웃 영속 계약 + 가용 서비스 판정 unit test.

① 위젯 분해 이후 배치 확장 필드(x/y/config/title)와 seed 세대(seedVersion)가 PUT 왕복에서
   유실되지 않는지 — 유실되면 저장본이 옛 배치를 영구 고정하거나 개편 안내가 매번 다시
   뜬다(console_platform.md §3.3~3.4).
② `installed_services()` 의 가용 판정 — role=all 하이브리드(csc 만 프록시)를 base 로 오판하면
   in-process 인 oam-svc 가 통째로 미가용이 되어 API 문서·위젯이 사라진다(api_docs.md §2).
③ 콘솔 정적 디렉토리 해석(`resolve_console_static_dir`) — 번들은 oam 동봉본 하나다. 서비스
   모듈(oam-svc·oam-cims-tester) 설치 트리가 후보로 되살아나면 팩 조합마다 번들이 생긴다
   (test_instrument.md §7 base 확장 ②).
"""
from __future__ import annotations

import os
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-CONSOLE-LAYOUT"
_NAME = "콘솔 레이아웃 영속·정적 해석 unit test (python3 -m unittest tests.test_console_layouts tests.test_console_static)"
_MODULES = ("tests.test_console_layouts", "tests.test_console_static")


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=52,
)
def unit_console_layouts(ctx: VerifyContext) -> ItemResult:
    missing = [m for m in _MODULES
               if not os.path.isfile(os.path.join(ctx.repo_root, *m.split(".")) + ".py")]
    if missing:
        return ItemResult(
            id=_ID, name=_NAME, status=ItemStatus.SKIP,
            detail=f"시험 모듈 없음: {missing}", stage=1,
        )
    env = dict(os.environ)
    env["PYTHONWARNINGS"] = "ignore::ResourceWarning"
    try:
        proc = subprocess.run(
            ["python3", "-m", "unittest", *_MODULES],
            cwd=ctx.repo_root, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=120, text=True,
        )
        rc, out, err = proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as e:
        rc = -1
        out = e.stdout.decode("utf-8", "replace") if e.stdout else ""
        err = (e.stderr.decode("utf-8", "replace") if e.stderr else "") \
              + f"\n[TIMEOUT after 120s] {e}"
    full = (out + err).strip()
    tail = "\n".join(full.splitlines()[-30:])
    ctx.w(f"## {_ID} — 콘솔 레이아웃 영속 unit test")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id=_ID, name=_NAME,
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=1,
    )
