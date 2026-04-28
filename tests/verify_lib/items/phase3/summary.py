"""Phase 3 결과 요약 — 녹취/SIP 로그 카운트 + 배포본 ERROR/FATAL 집계."""
from __future__ import annotations

import os
from glob import glob

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


@verify_item(
    id="P3-SUMMARY", phase=3, category="검증",
    name="결과 요약 (녹취/SIP/ERROR 카운트)",
    depends_on=[
        "P3-SCN-VOLTE-VOICE", "P3-SCN-VOLTE-VIDEO",
        "P3-SCN-PTT-VOICE",   "P3-SCN-PTT-VIDEO",
    ],
    presets=["phase3-full"],
    side_effects=["read-only"], timeout_s=10,
)
def summary(ctx: VerifyContext) -> ItemResult:
    """녹취 / SIP msg/flow 라인 / 배포본 csp·cmp ERROR/FATAL 카운트."""
    base = os.path.join(ctx.dist_dir, "ext_mnt", "service_log")
    rec_ok = sum(1 for p in glob(os.path.join(base, "**", "seg_*.rtp"),
                                 recursive=True) if os.path.getsize(p) > 0)
    rec_zero = sum(1 for p in glob(os.path.join(base, "**", "seg_*.rtp"),
                                   recursive=True) if os.path.getsize(p) == 0)

    msg_lines = 0
    for p in glob(os.path.join(base, "**", "*.msg.jsonl"), recursive=True):
        try:
            with open(p, "rb") as f: msg_lines += sum(1 for _ in f)
        except Exception: pass
    flow_lines = 0
    for p in glob(os.path.join(base, "**", "*.flow.jsonl"), recursive=True):
        try:
            with open(p, "rb") as f: flow_lines += sum(1 for _ in f)
        except Exception: pass
    sip_lines = msg_lines + flow_lines

    err_cnt = 0
    log_paths = (
        glob(os.path.join(ctx.dist_dir, "csp-server", "csp", "csp", "log", "csp_*.log")) +
        glob(os.path.join(ctx.dist_dir, "cmp-server", "cmp", "cmp", "log", "cmp_*.log"))
    )
    for p in log_paths:
        try:
            with open(p, "rb") as f:
                for line in f:
                    if b"ERROR" in line or b"FATAL" in line:
                        err_cnt += 1
        except Exception:
            pass

    lines = [
        f"- 녹취 파일(size>0): {rec_ok}개",
        f"- 녹취 파일(0바이트): {rec_zero}개",
        f"- SIP msg/flow 로그 라인: {sip_lines} (msg={msg_lines}, flow={flow_lines})",
        f"- 배포본 csp/cmp ERROR/FATAL: {err_cnt}",
    ]
    ctx.w("## P3-SUMMARY — 결과 요약")
    for line in lines: ctx.w(line)
    ctx.w()
    return ItemResult(
        id="P3-SUMMARY", name="결과 요약",
        status=ItemStatus.PASS, detail="\n".join(lines), phase=3,
    )
