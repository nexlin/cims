"""S1-UNIT-OAM-STATS — OAM SIP 통계 서비스축 판정 + 미디어 통계 프로브 unit test.

  · tests/test_oam_stats_classify.py  access_services domain→kind → 서비스축(volte|ptt) — 유선 voip 는 전화 계열 volte 합산,
                                      Request-URI→To→From 판정 순서(sip_statistics.md §3.1)
  · tests/test_stats_probe.py         CMP/노드 프로브 병렬·타임아웃·last-good 캐시(실서버 없이 죽은 UDP 소켓으로)
"""
from __future__ import annotations

import os
import subprocess

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

_ID = "S1-UNIT-OAM-STATS"
_NAME = "OAM SIP 통계 서비스축·프로브 unit test (tests/test_oam_stats_classify.py · test_stats_probe.py)"
_TESTS = ["tests/test_oam_stats_classify.py", "tests/test_stats_probe.py"]


@verify_item(
    id=_ID,
    stage=1, category="정적",
    name=_NAME,
    presets=["stage1-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=120,
    execution_order=54,
)
def unit_oam_stats(ctx: VerifyContext) -> ItemResult:
    present = [t for t in _TESTS if os.path.isfile(os.path.join(ctx.repo_root, t))]
    if not present:
        return ItemResult(id=_ID, name=_NAME, status=ItemStatus.SKIP,
                          detail="tests/test_oam_stats_classify.py · test_stats_probe.py 없음", stage=1)
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
    ctx.w(f"## {_ID} — OAM SIP 통계 서비스축·프로브 unit test")
    ctx.w("```")
    for line in tail.splitlines():
        ctx.w(line)
    ctx.w("```")
    ctx.w()
    return ItemResult(id=_ID, name=_NAME,
                      status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                      detail=tail, stage=1)
