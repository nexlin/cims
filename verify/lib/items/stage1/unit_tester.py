"""S1-UNIT-TESTER — 계측기(oam-cims-tester) 계약·핸들러·게이트웨이 SSE 통과 unit test.

① tester_models — 시나리오 3층(토폴로지·시나리오·부하 프로파일) 계약과 워커 메시지 계약.
   `schema/*.json` 이 모델과 어긋나면 워커(C++)가 다른 계약을 검증하게 된다(test_instrument.md §6).
② handlers.tester — RBAC(monitor/operator/manager)·토폴로지 저장은 검증 통과분만·run 색인·보존 스윕·SSE.
③ handlers.gateway — text/event-stream 청크 passthrough 와 requires_base_oam 기록
   (oam_base_service_split.md §5·§10). 통과가 깨지면 라이브 KPI 화면이 5 s 뒤 504 로 끊긴다.
④ tester_target — 피어 풀 시드 파생(도메인·번호 접두 규칙)·트렁크 REGISTER 비밀 해석·CspSeeder apply/restore.
⑤ 네이티브 `build/bin/csim_rtp_dtmf_test` — libcsim RTP 의 RFC 4733 telephone-event 송수신 루프백(빌드돼 있을 때만).
⑥ 네이티브 `build/bin/csim_rtp_media_test` — 미디어 평면(RTP 모드 none/explicit·샘플 송출·정지·hold 정지) 루프백.
"""
from __future__ import annotations

import os
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-TESTER"
_NAME = "계측기 계약/핸들러/오케스트레이터/피어 시드/게이트웨이 SSE unit test + libcsim RTP DTMF·미디어 평면 루프백 (python3 -m unittest tests.test_tester_models tests.test_tester_handler tests.test_tester_run tests.test_tester_target tests.test_gateway_stream · build/bin/csim_rtp_dtmf_test · build/bin/csim_rtp_media_test)"
_MODULES = ("tests.test_tester_models", "tests.test_tester_handler", "tests.test_tester_run", "tests.test_tester_target",
            "tests.test_gateway_stream")
_NATIVES = ("csim_rtp_dtmf_test", "csim_rtp_media_test")


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=53,
)
def unit_tester(ctx: VerifyContext) -> ItemResult:
    missing = [m for m in _MODULES
               if not os.path.isfile(os.path.join(ctx.repo_root, *m.split(".")) + ".py")]
    if missing:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP,
                          detail=f"시험 모듈 없음: {missing}", stage=1)
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
    # 네이티브 RTP 루프백 — 빌드 산출물이 있을 때만(없으면 결과에 표시만, python 시험 판정은 그대로)
    for name in _NATIVES:
        native = os.path.join(ctx.repo_root, "build", "bin", name)
        if os.path.isfile(native) and os.access(native, os.X_OK):
            try:
                nproc = subprocess.run([native], cwd=ctx.repo_root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       timeout=30, text=True)
                nlines = [l for l in nproc.stdout.splitlines() if "RTP STATS" not in l and "Floor recv" not in l]
                full += f"\n[{name}] " + " | ".join(nlines[-2:]) + f" (rc={nproc.returncode})"
                if nproc.returncode != 0:
                    rc = rc or nproc.returncode
            except subprocess.TimeoutExpired:
                full += f"\n[{name}] TIMEOUT"
                rc = rc or -1
        else:
            full += f"\n[{name}] 미빌드(build/bin/{name}) — 건너뜀"
    tail = "\n".join(full.splitlines()[-30:])
    ctx.w(f"## {_ID} — 계측기 unit test")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    ctx.w()
    return ItemResult(
        id=_ID, name=_NAME,
        status=ItemStatus.PASS if rc == 0 else ItemStatus.FAIL,
        detail=tail, stage=1,
    )
