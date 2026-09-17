"""S1-UNIT-PSIP — psip(SIP 스택) 루프백 단위시험. csp 빌드가 만든 psip 정적 라이브러리에 g++ 로 링크해
127.0.0.1 포트만 쓰는 하네스를 실행한다 (라이브 서비스 무관).

  · tests/psip_leg_dest_test.cpp  서버 발신 in-dialog 요청(BYE·re-INVITE) 목적지 재해석 (leg_liveness.md §6.3) —
                                  A UDP 등록+승격 TCP 닫힘 → UDP 바인딩 도달 · B 콜백 false → 기존 경로 · C Record-Route 제외 ·
                                  D re-INVITE 동일 · E TCP 등록(연결 유지) 무변경 · F TLS 등록(연결 유지) 무변경(자가서명 인증서, openssl CLI)

빌드 산출물(build/csp/psip_build/*.a)이 없으면 SKIP — S2 빌드 뒤 pre-package 프리셋에서 의미가 있다.
"""
from __future__ import annotations

import os
import shutil
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-PSIP"
_NAME = "psip 루프백 단위시험 (tests/psip_leg_dest_test.cpp — in-dialog 목적지 재해석)"
_TESTS = ["tests/psip_leg_dest_test.cpp"]
_PSIP_INC = ["SipUserAgent", "SipStack", "SipParser", "SdpParser", "StunParser", "XmlParser",
             "SipPlatform", "ServerPlatform"]
_PSIP_LIBS = ["libSipUserAgent.a", "libSipStack.a", "libSdpParser.a", "libSipParser.a", "libStunParser.a",
              "libXmlParser.a", "libSipPlatform.a", "libServerPlatform.a"]
_LIB_DIR = "build/csp/psip_build"


def _run(cmd: list, cwd: str, timeout: int):
    try:
        proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=timeout, text=True)
        return proc.returncode, (proc.stdout + proc.stderr).strip()
    except subprocess.TimeoutExpired as e:
        out = (e.stdout.decode("utf-8", "replace") if e.stdout else "") + \
              (e.stderr.decode("utf-8", "replace") if e.stderr else "")
        return -1, (out + f"\n[TIMEOUT after {timeout}s]").strip()


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=300,
    execution_order=55,
)
def unit_psip(ctx: VerifyContext) -> ItemResult:
    root = ctx.repo_root
    present = [t for t in _TESTS if os.path.isfile(os.path.join(root, t))]
    if not present:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP, detail="tests/psip_*_test.cpp 없음", stage=1)
    libs = [os.path.join(root, _LIB_DIR, l) for l in _PSIP_LIBS]
    missing = [l for l in libs if not os.path.isfile(l)]
    if missing:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP,
                          detail=f"psip 정적 라이브러리 없음 ({_LIB_DIR} — csp 빌드 선행): "
                                 + ", ".join(os.path.basename(m) for m in missing), stage=1)
    if shutil.which("g++") is None:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP, detail="g++ 없음", stage=1)

    ok = True
    tails = []
    for t in present:
        exe = os.path.join(root, "build", os.path.splitext(os.path.basename(t))[0])
        cmd = ["g++", "-std=c++17", "-D__LINUX__", "-D_REENTRANT"]
        for inc in _PSIP_INC:
            cmd += ["-I", os.path.join(root, "ext/psip", inc)]
        cmd += [os.path.join(root, t)] + libs + ["-lssl", "-lcrypto", "-lpthread", "-o", exe]
        rc, out = _run(cmd, root, 180)
        if rc != 0:
            tails.append(f"[{t}] build rc={rc}\n" + "\n".join(out.splitlines()[-15:]))
            ok = False
            continue
        rc, out = _run([exe], root, 90)
        tails.append(f"[{t}] rc={rc}\n" + "\n".join(out.splitlines()[-20:]))
        ok = ok and (rc == 0)
    tail = "\n".join(tails)
    ctx.w(f"## {_ID} — psip 루프백 단위시험")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    ctx.w()
    return ItemResult(id=_ID, name=_NAME,
                      status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail=tail, stage=1)
