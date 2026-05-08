"""S3-HEALTH — Health check (csp/cmp/csc 로그 ERROR/FATAL 누적)."""
from __future__ import annotations

import os

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


@verify_item(
    id="S3-HEALTH",
    stage=3, category="검증",
    name="Health check (csp/cmp/csc 로그 ERROR/FATAL 누적)",
    depends_on=["S3-START"],
    presets=["stage3-full", "stage3-quick", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=10,
    execution_order=70,
)
def health(ctx: VerifyContext) -> ItemResult:
    log_dir = os.path.join(ctx.dist_dir, "log")
    targets = ["csp.log", "cmp.log", "csc.log"]
    err_total = 0
    samples: list = []
    for fname in targets:
        path = os.path.join(log_dir, fname)
        if not os.path.isfile(path): continue
        try:
            with open(path, "rb") as f:
                for line in f:
                    if b"ERROR" in line or b"FATAL" in line:
                        err_total += 1
                        if len(samples) < 10:
                            samples.append(f"{fname}: {line.decode(errors='replace').rstrip()}")
        except Exception:
            pass
    ctx.w("## S3-HEALTH — Health check")
    ctx.w(f"- ERROR/FATAL 누적: {err_total}건")
    if samples:
        ctx.w("```")
        for s in samples: ctx.w(s[:400])
        ctx.w("```")
    ctx.w()

    detail = f"ERROR/FATAL: {err_total}\n" + "\n".join(samples)
    return ItemResult(
        id="S3-HEALTH", name="Health check",
        status=ItemStatus.PASS if err_total == 0 else ItemStatus.FAIL,
        detail=detail, stage=3,
    )
