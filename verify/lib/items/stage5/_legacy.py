"""Stage 5 _legacy 어댑터 — cims.sh _verify_phase2 22단계 본체를 1회 호출하고
[VERIFY] step-start/step-end 마커를 파싱하여 ctx.state 에 cache.

각 자식 항목 함수는 `step_result(by_step, [N, M, ...], id, name)` 으로
자기 step 결과를 합산해 ItemResult 를 만든다.

향후 _verify_phase2 의 step segment 를 Python 으로 포팅하면 이 어댑터 대신
각 자식 함수가 직접 작업을 수행하면 됨 — 자식 함수 시그니처 그대로.
"""
from __future__ import annotations

import re
from typing import Optional

from ...registry import ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


_STATE_KEY = "_s5_legacy"

_RE_STEP_START = re.compile(r"^\[VERIFY\] step-start: (\d+) (.+)$")
_RE_STEP_END   = re.compile(r"^\[VERIFY\] step-end: (\d+) status=(\S+) elapsed_ms=(\d+)$")

_STATUS_RANK = {
    ItemStatus.PASS: 0, ItemStatus.SKIP: 1,
    ItemStatus.BLOCKED: 2, ItemStatus.FAIL: 3,
}


def _parse_steps(text: str) -> dict:
    """[VERIFY] step-* 마커 → {step_no(int): {"status", "elapsed_ms", "name"}}."""
    name_by_n: dict = {}
    out: dict = {}
    for raw in text.splitlines():
        m = _RE_STEP_START.match(raw)
        if m:
            n = int(m.group(1))
            name_by_n[n] = m.group(2).strip()
            continue
        m = _RE_STEP_END.match(raw)
        if m:
            n = int(m.group(1))
            out[n] = {
                "status":     m.group(2),
                "elapsed_ms": int(m.group(3)),
                "name":       name_by_n.get(n, ""),
            }
    return out


def get_legacy_results(ctx: VerifyContext) -> dict:
    """_verify_phase2 본체를 1회 호출하고 step 결과 dict 반환 (cached).

    반환 형식:
      {
        N (int): {"status": "PASS|FAIL|SKIP", "elapsed_ms": int, "name": str},
        ...,
        "_rc":   int,         # _verify_phase2 종료 코드
        "_tail": str,         # stdout 마지막 30줄 (디버그)
      }
    """
    cached = ctx.state.get(_STATE_KEY)
    if cached:
        return cached

    # cims.sh 는 stage5 명령으로 호출 (B3 단계에서 등록). --legacy 플래그로 _verify_phase2 본체.
    args = ["verify", "stage5", "--legacy"]
    if ctx.skip_build: args.append("--skip-build")
    if ctx.skip_pkg:   args.append("--skip-pkg")
    if ctx.keep_agent: args.append("--keep-agent")
    if ctx.stop_after: args.append("--stop-after")

    rc, out, err = shell.run_cims_sh(ctx.repo_root, *args, timeout=900)
    full = (out or "") + (err or "")
    by_step = _parse_steps(full)
    by_step["_rc"]   = rc
    by_step["_tail"] = "\n".join(full.splitlines()[-30:])
    ctx.state[_STATE_KEY] = by_step
    return by_step


def step_result(by_step: dict, step_nos: list, item_id: str, name: str,
                stage: int = 5) -> ItemResult:
    """주어진 step 번호들의 결과를 합산해 ItemResult 반환 (worst status)."""
    found = [by_step[n] for n in step_nos if isinstance(n, int) and n in by_step]
    if not found:
        return ItemResult(
            id=item_id, name=name, status=ItemStatus.SKIP,
            detail=f"legacy 가 step {step_nos} 도달 못함 (rc={by_step.get('_rc')})",
            stage=stage,
        )
    worst = ItemStatus.PASS
    total_ms = 0
    parts: list = []
    for n in step_nos:
        s = by_step.get(n)
        if not s: continue
        if _STATUS_RANK.get(s["status"], 0) > _STATUS_RANK.get(worst, 0):
            worst = s["status"]
        total_ms += s["elapsed_ms"]
        parts.append(f"step{n:02d}={s['status']}({s['elapsed_ms']}ms) {s['name']}")
    detail = "\n".join(parts)
    return ItemResult(
        id=item_id, name=name, status=worst,
        elapsed_ms=total_ms, detail=detail, stage=stage,
    )
