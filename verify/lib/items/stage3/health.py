"""S3-HEALTH — Health check (csp/cmp/csc 로그 ERROR/FATAL 누적).

로그 포맷별 정밀 매칭:
- csp/cmp (C++ psip 포맷): `[YYYY-MM-DD ...] [E|F] [Module] ...` — level letter 정확 매칭
- csc (Python uvicorn): 행 시작 `ERROR:` / `CRITICAL:` — Python logging 포맷

전체 파일이 아닌 tail (last N lines) 만 검사 — 장기 가동 시 메모리 폭증 회피.
"""
from __future__ import annotations

import os
import re
from collections import deque

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


_NATIVE_RE = re.compile(rb"^\[[\d\-: .]+\]\s+\[(E|F)\]")
_PY_RE = re.compile(rb"^(ERROR|CRITICAL):")
_TAIL_LINES = 2000


def _tail_bytes(path: str, n_lines: int) -> list:
    try:
        with open(path, "rb") as f:
            return list(deque(f, maxlen=n_lines))
    except Exception:
        return []


def _scan_native(path: str) -> tuple:
    """C++ psip 로그 — `[E]` / `[F]` 행만 카운트."""
    err = 0
    samples: list = []
    for line in _tail_bytes(path, _TAIL_LINES):
        if _NATIVE_RE.match(line):
            err += 1
            if len(samples) < 10:
                samples.append(
                    f"{os.path.basename(path)}: {line.decode(errors='replace').rstrip()}"
                )
    return err, samples


def _scan_python(path: str) -> tuple:
    """Python uvicorn 로그 — 행 시작 `ERROR:` / `CRITICAL:` 만 카운트."""
    err = 0
    samples: list = []
    for line in _tail_bytes(path, _TAIL_LINES):
        if _PY_RE.match(line):
            err += 1
            if len(samples) < 10:
                samples.append(
                    f"{os.path.basename(path)}: {line.decode(errors='replace').rstrip()}"
                )
    return err, samples


@verify_item(
    id="S3-HEALTH",
    stage=3, category="검증",
    name="Health check (csp/cmp ERROR/FATAL + csc ERROR/CRITICAL, last 2000 lines)",
    depends_on=["S3-START"],
    presets=["stage3-full", "stage3-quick", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=10,
    execution_order=70,
)
def health(ctx: VerifyContext) -> ItemResult:
    log_dir = os.path.join(ctx.dist_dir, "log")
    err_total = 0
    samples: list = []
    for fname in ("csp.log", "cmp.log"):
        path = os.path.join(log_dir, fname)
        if not os.path.isfile(path):
            continue
        n, s = _scan_native(path)
        err_total += n
        samples.extend(s)
    for fname in ("csc.log",):
        path = os.path.join(log_dir, fname)
        if not os.path.isfile(path):
            continue
        n, s = _scan_python(path)
        err_total += n
        samples.extend(s)
    samples = samples[:10]

    ctx.w("## S3-HEALTH — Health check (last 2000 lines per file)")
    ctx.w(f"- ERROR/FATAL/CRITICAL 누적: {err_total}건")
    if samples:
        ctx.w("```")
        for s in samples:
            ctx.w(s[:400])
        ctx.w("```")
    ctx.w()

    detail = f"ERROR/FATAL/CRITICAL: {err_total}\n" + "\n".join(samples)
    return ItemResult(
        id="S3-HEALTH", name="Health check",
        status=ItemStatus.PASS if err_total == 0 else ItemStatus.FAIL,
        detail=detail, stage=3,
    )
