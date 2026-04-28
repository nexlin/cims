"""Phase 3 진입 조건 체크 — Phase 2 배포본 4포트 LISTEN."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


_REQUIRED_PORTS = [
    (4445, "tcp", "배포본 csc"),
    (8081, "tcp", "배포본 console"),
    (5060, "udp", "배포본 csp"),
    (9000, "udp", "배포본 cmp"),
]


@verify_item(
    id="P3-ENTRY-CHECK",
    phase=3, category="환경",
    name="진입 조건 체크 (Phase 2 결과물 4포트 LISTEN)",
    presets=["phase3-full"],
    side_effects=["read-only"],
    timeout_s=10,
)
def entry_check(ctx: VerifyContext) -> ItemResult:
    """배포본 csc(4445) / console(8081) / csp(5060/udp) / cmp(9000/udp) LISTEN 확인."""
    ctx.w("## P3-ENTRY-CHECK — 진입 조건 체크 (Phase 2 결과물 4포트 LISTEN)")
    ok = True
    lines = []
    for port, proto, label in _REQUIRED_PORTS:
        listening = shell.port_listening(port, proto)
        mark = "[OK]" if listening else "[FAIL]"
        lines.append(f"- {mark} {label} (port {port}/{proto}) {'LISTEN' if listening else '미기동'}")
        if not listening: ok = False
    for line in lines:
        ctx.w(line)
    ctx.w()
    detail = "\n".join(lines)
    if not ok:
        detail += ("\n\n[Phase 2 미완료] 다음 명령으로 선행 실행:\n"
                   "  ./cims.sh verify phase2 --skip-build --skip-pkg")
    return ItemResult(
        id="P3-ENTRY-CHECK", name="진입 조건 체크",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=detail, phase=3,
    )
