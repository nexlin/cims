"""S1-UNIT-OAM-PTT — PTT 세션 읽기 모델(ptt_index) + 세션 이력 진행중 병합 규약 unit test.

  · tests/oam_ptt_index_test.py          인덱스 = 스캔 · 세션이 기록 단위(시간버킷 넘김·같은 시간대 두 세션)
  · tests/oam_ptt_sessions_live_test.py  진행중 세션은 조회 범위와 무관하게 전량 병합(어제 시작해 열린 세션)
두 시험 모두 실서버·녹취 트리 없이 tmp/대체 읽기 모델로 돈다.
"""
from __future__ import annotations

import os
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-OAM-PTT"
_NAME = "OAM PTT 세션 인덱스·이력 병합 unit test (tests/oam_ptt_index_test.py · oam_ptt_sessions_live_test.py)"
_TESTS = ["tests/oam_ptt_index_test.py", "tests/oam_ptt_sessions_live_test.py"]


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=53,
)
def unit_oam_ptt(ctx: VerifyContext) -> ItemResult:
    present = [t for t in _TESTS if os.path.isfile(os.path.join(ctx.repo_root, t))]
    if not present:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP,
                          detail="tests/oam_ptt_*_test.py 없음", stage=1)
    env = dict(os.environ)
    env["PYTHONWARNINGS"] = "ignore::ResourceWarning"
    ok = True
    tails = []
    for t in present:
        try:
            proc = subprocess.run(["python3", t], cwd=ctx.repo_root, env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  timeout=120, text=True)
            rc, out, err = proc.returncode, proc.stdout, proc.stderr
        except subprocess.TimeoutExpired as e:
            rc = -1
            out = e.stdout.decode("utf-8", "replace") if e.stdout else ""
            err = (e.stderr.decode("utf-8", "replace") if e.stderr else "") \
                  + f"\n[TIMEOUT after 120s] {e}"
        full = (out + err).strip()
        tails.append(f"[{t}] rc={rc}\n" + "\n".join(full.splitlines()[-12:]))
        ok = ok and (rc == 0)
    tail = "\n".join(tails)
    ctx.w(f"## {_ID} — OAM PTT 세션 인덱스·이력 병합 unit test")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    ctx.w()
    return ItemResult(id=_ID, name=_NAME,
                      status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail=tail, stage=1)
