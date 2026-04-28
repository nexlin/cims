"""Phase 2 — 22단계 통합 실행 + 단계별 children 결과.

Phase 2 는 TB-CSC 4419 admin API + deployment 시스템과 강결합되어 있고
22단계가 순차 의존성을 가져 부분 실행이 의미 없음. 1차 마이그레이션은
cims.sh 의 기존 _verify_phase2 본체(676줄)를 wrapping.

단, 사용자 가시성을 위해 legacy 리포트의 "## NN. ..." 헤더를 파싱해
ItemResult.children 22개로 적재. UI/리포트에서 단계별 PASS/SKIP/도달여부 표시.
"""
from __future__ import annotations

import os
import re

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


# Phase 2 의 22단계 — _verify_phase2 가 출력하는 표준 헤더와 매핑
# (헤더 누락 = legacy 가 도달 못함 → SKIP/UNKNOWN 으로 표기)
_P2_STEP_NAMES: dict = {
     1: "Cleanup",                        2: "Build",
     3: "Configure",                      4: "Pkg (tarball)",
     5: "Admin login (TB-CSC 4419)",      6: "Agent 등록 (csc-server-local)",
     7: "Test-agent 기동 + enroll",        8: "Package upload (csc/console)",
     9: "Deployment 생성 (config overlay)", 10: "Install job + 폴링",
    11: "설치 파일 검증",                  12: "config overlay 검증",
    13: "Start job (csc) + 4445 LISTEN", 14: "Health check job",
    15: "Console start + 8081 LISTEN",   16: "배포본 csc(4445) admin login",
    17: "Package upload → 배포본 (csp/cmp/cspsim)",
    18: "Agent 등록 + Test-agent (9904/9905/9906)",
    19: "Deployment 생성 (csp/cmp/cspsim)",
    20: "Install job + 폴링 (csp/cmp/sim)",
    21: "Start (csp/cmp) — sim install-only",
    22: "Stop / 전체 기동 유지",
}


def _parse_step_children(legacy_report: str) -> list:
    """legacy 리포트(.md)에서 ## NN. 헤더 추출 → 22 children. 도달 안한 단계는 SKIP."""
    reached: dict = {}
    if legacy_report and os.path.isfile(legacy_report):
        try:
            with open(legacy_report) as f:
                content = f.read()
        except Exception:
            content = ""
        # 헤더: ## 1. Cleanup  /  ## 2. Build — SKIPPED  등
        pat = re.compile(r"^## (\d{1,2})\.\s+(.+?)$", re.MULTILINE)
        for num, title in pat.findall(content):
            n = int(num)
            is_skip = "SKIPPED" in title.upper() or "— SKIP" in title
            reached[n] = {"title": title.strip(), "skip": is_skip}

    children = []
    for n in range(1, 23):
        std_name = _P2_STEP_NAMES.get(n, f"step {n}")
        if n in reached:
            r = reached[n]
            status = ItemStatus.SKIP if r["skip"] else ItemStatus.PASS
            detail = r["title"]
        else:
            status = ItemStatus.SKIP
            detail = "(헤더 미발견 — legacy 가 도달 못함)"
        children.append(ItemResult(
            id=f"P2-{n:02d}", name=std_name,
            status=status, detail=detail, phase=2,
        ))
    return children


@verify_item(
    id="P2-RUN-ALL",
    phase=2, category="배포",
    name="Phase 2 22단계 통합 실행 (cleanup → build → pkg → install → start/health)",
    presets=["phase2-full"],
    side_effects=["fs-write", "db-truncate", "service-state", "network"],
    timeout_s=900,
)
def run_all(ctx: VerifyContext) -> ItemResult:
    """cims.sh _verify_phase2 본체(legacy) 호출 + 리포트에서 22단계 children 추출."""
    args = ["verify", "phase2", "--legacy"]
    if ctx.skip_build: args.append("--skip-build")
    if ctx.skip_pkg:   args.append("--skip-pkg")
    if ctx.keep_agent: args.append("--keep-agent")
    if ctx.stop_after: args.append("--stop-after")

    rc, out, err = shell.run_cims_sh(ctx.repo_root, *args, timeout=900)
    full = (out + err)
    tail = "\n".join(full.splitlines()[-30:])

    # legacy 가 만든 리포트 경로 추출
    m = re.search(r"(/[\w/.-]+/verify_reports/\d{8}_\d{6}_phase2\.md)", full)
    legacy_report = m.group(1) if m else ""
    children = _parse_step_children(legacy_report)

    n_pass = sum(1 for c in children if c.status == ItemStatus.PASS)
    n_skip = sum(1 for c in children if c.status == ItemStatus.SKIP)

    ctx.w("## P2-RUN-ALL — Phase 2 22단계 통합 실행 (legacy 본체)")
    ctx.w(f"- rc={rc}, 도달 단계={n_pass + n_skip - sum(1 for c in children if 'legacy 가 도달' in c.detail)}/22")
    ctx.w(f"- 단계 PASS: {n_pass}, SKIP: {n_skip}")
    if legacy_report:
        ctx.w(f"- legacy 리포트: `{os.path.basename(legacy_report)}`")
    ctx.w("```")
    for c in children:
        mark = {"PASS": "✓", "FAIL": "✗", "SKIP": "·"}.get(c.status, "?")
        ctx.w(f"  P2-{c.id.split('-')[1]:>3} {mark} {c.status:5} {c.name}")
    ctx.w("```")
    ctx.w("```")
    ctx.w("[stdout 마지막 30줄]")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()

    ok = (rc == 0)
    return ItemResult(
        id="P2-RUN-ALL", name="Phase 2 22단계 통합 실행",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=f"rc={rc}, legacy={os.path.basename(legacy_report) if legacy_report else 'N/A'}, "
               f"단계 PASS {n_pass}/22 (SKIP {n_skip})",
        phase=2,
        children=children,
    )
