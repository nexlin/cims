"""S6-ENTRY-CHECK — 통합 검증 진입 조건.

체크 항목:
1. Stage 5 결과물 4포트 LISTEN (csc/console/csp/cmp)
2. Immutability gate — packages/manifest.json sha == .deployed-manifest.json sha
   (S5 배포 이후 패키지 재빌드 시 mismatch → FAIL, S5 부터 재배포 필요)
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell
from ...common import pkg_manifest as _pkgm


_REQUIRED_PORTS = [
    (4445, "tcp", "배포본 csc"),
    (8081, "tcp", "배포본 console"),
    (5060, "udp", "배포본 csp"),
    (9000, "udp", "배포본 cmp"),
]


@verify_item(
    id="S6-ENTRY-CHECK",
    stage=6, category="환경",
    name="진입 조건 체크 (LISTEN + manifest immutability)",
    presets=["stage6-full", "pipeline-full", "post-deploy"],
    side_effects=["read-only"],
    timeout_s=10,
)
def entry_check(ctx: VerifyContext) -> ItemResult:
    """포트 LISTEN + manifest immutability 매칭 검증.

    immutability 가 깨졌다 함은: 마지막 S5 배포 이후 사용자가 S2/S4 를 다시
    돌려 packages/*.tar.gz 가 갱신됐고, 따라서 LISTEN 중인 모듈은 옛 패키지로
    실행 중이라는 뜻이다. S6 시나리오는 새 패키지를 검증하는 것이 의도이므로
    FAIL 처리하고 사용자가 S5 부터 재배포 하도록 유도한다.
    """
    ctx.w("## S6-ENTRY-CHECK — 진입 조건 체크")
    ok = True
    lines: list = []

    # (1) 포트 LISTEN
    lines.append("### (1) Stage 5 결과물 4포트 LISTEN")
    for port, proto, label in _REQUIRED_PORTS:
        listening = shell.port_listening(port, proto)
        mark = "[OK]" if listening else "[FAIL]"
        lines.append(f"- {mark} {label} (port {port}/{proto}) {'LISTEN' if listening else '미기동'}")
        if not listening: ok = False

    # (2) Immutability gate — manifest sha 매칭
    lines.append("")
    lines.append("### (2) Immutability gate — manifest sha 매칭")
    imm_ok, cur, dep, imm_detail = _pkgm.immutability_check(ctx.dist_dir)
    mark = "[OK]" if imm_ok else "[FAIL]"
    lines.append(f"- {mark} {imm_detail}")
    if cur:
        lines.append(f"  · 현재 manifest sha: `{cur}`")
    if dep:
        lines.append(f"  · 배포 marker sha:   `{dep}`")
    if not imm_ok: ok = False

    for line in lines:
        ctx.w(line)
    ctx.w()
    detail = "\n".join(lines)
    if not ok:
        detail += ("\n\n[복구 절차]\n"
                   "  ./cims.sh verify stage4   # 새 manifest 산출\n"
                   "  ./cims.sh verify stage5   # 새 패키지로 재배포\n"
                   "  ./cims.sh verify stage6   # 통합 검증 진입")
    return ItemResult(
        id="S6-ENTRY-CHECK", name="진입 조건 체크",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=detail, stage=6,
    )
