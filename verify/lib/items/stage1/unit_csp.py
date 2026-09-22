"""S1-UNIT-CSP — CSP 순수 로직 단위시험. csp/ 의 라이브 서비스·소켓과 무관한 함수를 g++ 로 따로 링크해 실행한다.

  · tests/csp_dial_plan_test.cpp   착신 번호 번역(csp/CspDialPlan.cpp — sip_service_model.md §2-10, TS 24.229 §5.4.3.2 · RFC 3966):
                                   국내형→+E.164 · 국제 접두 · 시각 구분자 · 그룹 id/피처코드/긴급번호 비번역 · 접두 없는 숫자열 484 ·
                                   플랜 비활성 · phone-context · sip/tel URI 추출·재작성 (psip SipParser 정적 라이브러리에 링크)
  · tests/csp_diversion_test.cpp   착신전환(csp/CspDiversion.cpp — TS 24.604 §4.5.2.6, RFC 7044 History-Info · RFC 4458 cause):
                                   첫 전환 index=1/1.1 mp=1 cause=302 · 연쇄 · 수신 값 이어 붙이기 · 전환 수(cause 항목) · 항목 분리 · URI 조립

psip 정적 라이브러리(build/csp/psip_build/*.a)가 없으면 SKIP — S2 빌드 뒤 pre-package 프리셋에서 의미가 있다.
"""
from __future__ import annotations

import os
import shutil
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-CSP"
_NAME = "CSP 로직 단위시험 (tests/csp_dial_plan_test.cpp 다이얼 플랜 · csp_diversion_test.cpp 착신전환 History-Info)"
_LIB_DIR = "build/csp/psip_build"
# 시험 → (추가 소스, 링크할 psip 정적 라이브러리)
_TESTS = {
    "tests/csp_dial_plan_test.cpp": (["csp/CspDialPlan.cpp"], ["libSipParser.a", "libSipPlatform.a"]),
    # 착신전환 History-Info(RFC 7044)·cause(RFC 4458) 조립·전환 수 판정 — 순수 문자열(라이브러리 불필요)
    "tests/csp_diversion_test.cpp": (["csp/CspDiversion.cpp"], []),
}
_INCS = ["csp", "ext/psip/SipParser", "ext/psip/SipPlatform"]


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
    execution_order=57,
)
def unit_csp(ctx: VerifyContext) -> ItemResult:
    root = ctx.repo_root
    present = {t: v for t, v in _TESTS.items() if os.path.isfile(os.path.join(root, t))}
    if not present:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP, detail="tests/csp_*_test.cpp 없음", stage=1)
    if shutil.which("g++") is None:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP, detail="g++ 없음", stage=1)
    need = sorted({l for _, libs in present.values() for l in libs})
    missing = [l for l in need if not os.path.isfile(os.path.join(root, _LIB_DIR, l))]
    if missing:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP,
                          detail="psip 정적 라이브러리 없음 (build/csp/psip_build — S2 빌드 선행): " + ", ".join(missing), stage=1)

    ok = True
    tails = []
    for t, (srcs, libs) in present.items():
        exe = os.path.join(root, "build", os.path.splitext(os.path.basename(t))[0])
        cmd = ["g++", "-std=c++17", "-O1"]
        for inc in _INCS:
            cmd += ["-I", os.path.join(root, inc)]
        cmd += [os.path.join(root, t)] + [os.path.join(root, s) for s in srcs]
        cmd += [os.path.join(root, _LIB_DIR, l) for l in libs] + ["-lpthread", "-o", exe]
        rc, out = _run(cmd, root, 120)
        if rc != 0:
            tails.append(f"[{t}] build rc={rc}\n" + "\n".join(out.splitlines()[-15:]))
            ok = False
            continue
        rc, out = _run([exe], root, 60)
        tails.append(f"[{t}] rc={rc}\n" + "\n".join(out.splitlines()[-30:]))
        ok = ok and (rc == 0)
    tail = "\n".join(tails)
    ctx.w(f"## {_ID} — CSP 로직 단위시험")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    ctx.w()
    return ItemResult(id=_ID, name=_NAME, status=ItemStatus.PASS if ok else ItemStatus.FAIL, detail=tail, stage=1)
