"""S1-UNIT-CMP — CMP 미디어 유닛 단위시험. 레포의 pkg/ 정적 라이브러리(AMR-WB 디코더 opencore-amrwb · 인코더 vo-amrwbenc — ExternalProject)에
g++ 로 링크해 실행한다 (라이브 서비스·소켓 무관).

  · tests/cmp_transcoder_test.cpp  피어 leg 트랜스코더(cmp/PTranscoder.cpp — cmp.md §11) — RFC 4867 프레이밍(octet-aligned/bandwidth-efficient/
                                   자동 판정) · G.711→AMR-WB(PT·timestamp ×2·SSRC·seq·marker, 10 ms ×2 → 1 프레임, mode-set) · AMR-WB→G.711
                                   (160 B 패킷, 다중 프레임 → 패킷 여럿) · 1 kHz 사인 왕복 상관 ≥ 0.8 · telephone-event timestamp 재작성 · 지원 쌍

AMR 라이브러리(pkg/opencore-amr, pkg/vo-amrwbenc-0.1.3)가 없으면 SKIP — S2 빌드(ExternalProject) 뒤 pre-package 프리셋에서 의미가 있다.
"""
from __future__ import annotations

import os
import shutil
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-CMP"
_NAME = "CMP 미디어 유닛 단위시험 (tests/cmp_transcoder_test.cpp — 피어 leg G.711↔AMR-WB 트랜스코더)"
_TESTS = ["tests/cmp_transcoder_test.cpp"]
_SRCS = ["cmp/PTranscoder.cpp"]
_INCS = ["cmp", "pkg/opencore-amr/include/opencore-amrwb", "pkg/vo-amrwbenc-0.1.3/include/vo-amrwbenc"]
_LIBS = ["pkg/opencore-amr/lib/libopencore-amrwb.a", "pkg/vo-amrwbenc-0.1.3/lib/libvo-amrwbenc.a"]


def _run(cmd: list, cwd: str, timeout: int):
    try:
        proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, text=True)
        return proc.returncode, (proc.stdout + proc.stderr).strip()
    except subprocess.TimeoutExpired as e:
        out = (e.stdout.decode("utf-8", "replace") if e.stdout else "") + (e.stderr.decode("utf-8", "replace") if e.stderr else "")
        return -1, (out + f"\n[TIMEOUT after {timeout}s]").strip()


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=180,
    execution_order=56,
)
def unit_cmp(ctx: VerifyContext) -> ItemResult:
    root = ctx.repo_root
    present = [t for t in _TESTS if os.path.isfile(os.path.join(root, t))]
    if not present:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP, detail="tests/cmp_*_test.cpp 없음", stage=1)
    libs = [os.path.join(root, l) for l in _LIBS]
    missing = [l for l in libs if not os.path.isfile(l)]
    if missing:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP,
                          detail="AMR 정적 라이브러리 없음 (pkg/ — ExternalProject 빌드 선행): " + ", ".join(os.path.basename(m) for m in missing), stage=1)
    if shutil.which("g++") is None:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP, detail="g++ 없음", stage=1)

    ok = True
    tails = []
    for t in present:
        exe = os.path.join(root, "build", os.path.splitext(os.path.basename(t))[0])
        cmd = ["g++", "-std=c++17", "-O1"]
        for inc in _INCS:
            cmd += ["-I", os.path.join(root, inc)]
        cmd += [os.path.join(root, t)] + [os.path.join(root, s) for s in _SRCS] + libs + ["-lm", "-o", exe]
        rc, out = _run(cmd, root, 120)
        if rc != 0:
            tails.append(f"[{t}] build rc={rc}\n" + "\n".join(out.splitlines()[-15:]))
            ok = False
            continue
        rc, out = _run([exe], root, 60)
        tails.append(f"[{t}] rc={rc}\n" + "\n".join(out.splitlines()[-30:]))
        ok = ok and (rc == 0)
    tail = "\n".join(tails)
    ctx.w(f"## {_ID} — CMP 미디어 유닛 단위시험")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    ctx.w()
    return ItemResult(id=_ID, name=_NAME, status=ItemStatus.PASS if ok else ItemStatus.FAIL, detail=tail, stage=1)
