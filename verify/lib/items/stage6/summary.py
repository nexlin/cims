"""S6-SUMMARY — 통합 검증 결과 요약 (녹취 / SIP 로그 / ERROR 카운트)."""
from __future__ import annotations

import os
from glob import glob

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


@verify_item(
    id="S6-SUMMARY", stage=6, category="검증",
    name="결과 요약 (녹취/SIP/ERROR 카운트)",
    depends_on=[
        "S6-SCN-VOLTE-VOICE", "S6-SCN-VOLTE-VIDEO",
        "S6-SCN-PTT-VOICE",   "S6-SCN-PTT-VIDEO",
        # 신규 시나리오 (cert-rotate 는 agent 종료 가능 → 마지막에 실행)
        "S6-SCN-SUBSCRIBE",   "S6-SCN-DB-SYNC", "S6-SCN-CERT-ROTATE",
        # 깊이 검증 항목 (depth scenarios)
        "S6-L7-SUBSCRIBE-NOTIFY", "S6-CMP-GROUP-SYNC", "S6-MCPTT-FLOOR-GRANT",
    ],
    presets=["stage6-full", "pipeline-full", "post-deploy"],
    side_effects=["read-only"], timeout_s=10,
    execution_order=100,
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
    ctx.w("## S6-SUMMARY — 결과 요약")
    for line in lines: ctx.w(line)
    ctx.w()
    return ItemResult(
        id="S6-SUMMARY", name="결과 요약",
        status=ItemStatus.PASS, detail="\n".join(lines), stage=6,
    )
