"""S1-CPP-FORMAT — csp/ C++ 코드의 clang-format 검사 (--dry-run --Werror)."""
from __future__ import annotations

import os
import shutil

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


# csp/.clang-format 만 적용 (cmp 는 별도 정책)
_TARGET_DIRS = ["csp"]
_EXTS = (".h", ".hpp", ".cpp", ".c", ".cc")


def _collect_files(repo_root: str) -> list:
    out = []
    for d in _TARGET_DIRS:
        full = os.path.join(repo_root, d)
        if not os.path.isdir(full): continue
        for root, dirs, files in os.walk(full):
            # 빌드 산출물/외부 라이브러리 제외
            dirs[:] = [x for x in dirs
                       if x not in ("build", "ext", "ext_mnt", "third_party",
                                    "node_modules", ".git")]
            for f in files:
                if f.endswith(_EXTS):
                    out.append(os.path.join(root, f))
    return out


@verify_item(
    id="S1-CPP-FORMAT",
    stage=1, category="정적",
    name="C++ clang-format 검사 (--dry-run --Werror, csp/.clang-format)",
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=40,
)
def cpp_format(ctx: VerifyContext) -> ItemResult:
    if not shutil.which("clang-format"):
        msg = ("clang-format 미설치 — `sudo apt install clang-format` 후 "
               "재실행 (CLAUDE.md prerequisite). `--require-cpp-format` "
               "옵션 사용 시 SKIP → FAIL 로 강제 가능.")
        # opts.require_cpp_format true 면 strict 모드로 FAIL.
        require = bool((ctx.opts or {}).get("require_cpp_format"))
        return ItemResult(
            id="S1-CPP-FORMAT", name="C++ clang-format",
            status=ItemStatus.FAIL if require else ItemStatus.SKIP,
            detail=msg, stage=1,
        )
    files = _collect_files(ctx.repo_root)
    if not files:
        return ItemResult(
            id="S1-CPP-FORMAT", name="C++ clang-format",
            status=ItemStatus.SKIP, detail="대상 파일 없음", stage=1,
        )
    cmd = ["clang-format", "--dry-run", "--Werror", "-style=file"] + files
    rc, out, err = shell.run(cmd, cwd=ctx.repo_root, timeout=120)
    full = (out + err).strip()
    tail = "\n".join(full.splitlines()[-30:])
    ctx.w("## S1-CPP-FORMAT — clang-format")
    ctx.w(f"- 검사 파일: {len(files)}개")
    if tail:
        ctx.w("```")
        for line in tail.splitlines(): ctx.w(line)
        ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S1-CPP-FORMAT", name="C++ clang-format",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail or f"OK ({len(files)} files)", stage=1,
    )
