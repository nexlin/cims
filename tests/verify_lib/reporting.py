"""검증 결과 리포트 생성 (markdown).

verify_reports/<ts>_phaseN.md 형식 — cims.sh 의 기존 리포트와 호환.
"""
from __future__ import annotations

from typing import Iterable

from .registry import ItemResult, ItemStatus
from .context import VerifyContext


def write_header(ctx: VerifyContext, scope: str = "") -> None:
    ctx.w(f"# Phase {ctx.phase} Verification Report")
    ctx.w()
    ctx.w(f"- Timestamp: {ctx.ts}")
    ctx.w(f"- Host: {_hostname()}")
    ctx.w(f"- ens160 IP: {ctx.ens_ip or 'N/A'}")
    ctx.w(f"- Git: {ctx.git_branch} @ {ctx.git_sha}")
    if scope:
        ctx.w(f"- Scope: {scope}")
    if ctx.skip_build:  ctx.w("- skip-build: yes")
    if ctx.skip_pkg:    ctx.w("- skip-pkg: yes")
    if ctx.skip_reset:  ctx.w("- skip-reset: yes")
    if ctx.keep_agent:  ctx.w("- keep-agent: yes")
    ctx.w()


def write_item_section(ctx: VerifyContext, result: ItemResult, idx: int = 0) -> None:
    """항목 1개의 결과를 한 섹션으로 출력."""
    icon = {"PASS": "[OK]", "FAIL": "[FAIL]", "SKIP": "[SKIP]"}.get(result.status, "[?]")
    title = f"## {idx}. {result.id} — {result.name}" if idx else f"## {result.id} — {result.name}"
    ctx.w(title)
    ctx.w(f"- 결과: {icon} {result.status} ({result.elapsed_ms}ms)")
    if result.detail:
        ctx.w("```")
        for line in result.detail.splitlines():
            ctx.w(line)
        ctx.w("```")
    if result.children:
        ctx.w("| 자식 ID | 항목 | 결과 | 소요(ms) | 상세 |")
        ctx.w("|---|---|---|---|---|")
        for c in result.children:
            cicon = {"PASS": "PASS", "FAIL": "**FAIL**", "SKIP": "SKIP"}.get(c.status, c.status)
            detail = (c.detail or "")[:80].replace("|", "/").replace("\n", " ")
            ctx.w(f"| {c.id} | {c.name} | {cicon} | {c.elapsed_ms} | {detail} |")
    ctx.w()


def write_summary(ctx: VerifyContext, results: list, verdict: str) -> dict:
    """요약 + 판정 라인. 토탈 dict 반환."""
    total = len(results)
    n_pass = sum(1 for r in results if r.status == ItemStatus.PASS)
    n_fail = sum(1 for r in results if r.status == ItemStatus.FAIL)
    n_skip = sum(1 for r in results if r.status == ItemStatus.SKIP)
    ctx.w("## 결과 요약")
    ctx.w(f"- 총 항목: {total}")
    ctx.w(f"- PASS: {n_pass}")
    ctx.w(f"- FAIL: {n_fail}")
    ctx.w(f"- SKIP: {n_skip}")
    ctx.w()
    ctx.w(f"## 판정: {verdict}")
    return {"total": total, "pass": n_pass, "fail": n_fail, "skip": n_skip}


def determine_verdict(results: list) -> str:
    if any(r.status == ItemStatus.FAIL for r in results):
        return "FAIL"
    return "PASS"


def _hostname() -> str:
    import socket
    try: return socket.gethostname()
    except Exception: return "?"
