"""S6-ENTRY-CHECK — 통합 검증 진입 조건.

체크 항목:
1. Stage 5 결과물 LISTEN (csc/console + _INSTANCES 의 시그널링/미디어 인스턴스)
2. Immutability gate — packages/manifest.json sha == .deployed-manifest.json sha
   (S5 배포 이후 패키지 재빌드 시 mismatch → FAIL, S5 부터 재배포 필요)

target=verify (default) → csc=4445, console=8081
target=prod             → csc=4420, console=80
시그널링/미디어 포트 (5060/9000) 는 LocalIp 별로 분리 — _INSTANCES.local_ip 매칭.
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell
from ...common import pkg_manifest as _pkgm
from ..stage5._native_steps import _INSTANCES as _NATIVE_INSTANCES


_TARGET_PORTS = {
    "verify": {"csc": 4445, "console": 8081},
    "prod":   {"csc": 4420, "console": 80},
}


def _required_ports(ctx: VerifyContext) -> list:
    """csc/console + _INSTANCES (listen 있는 entry) 의 (port, proto, host, label)."""
    target = (ctx.opts or {}).get("target") or "verify"
    p = _TARGET_PORTS.get(target, _TARGET_PORTS["verify"])
    out = [
        (p["csc"],     "tcp", "",            "배포본 csc"),
        (p["console"], "tcp", "",            "배포본 console"),
    ]
    for inst in _NATIVE_INSTANCES:
        listen = inst.get("listen")
        if not listen:
            continue
        port, proto = listen
        host = inst.get("local_ip", "")
        out.append((port, proto, host, f"배포본 {inst['id']}"))
    return out


@verify_item(
    id="S6-ENTRY-CHECK",
    stage=6, category="환경",
    name="진입 조건 체크 (LISTEN + manifest immutability)",
    presets=["stage6-full", "pipeline-full", "post-deploy"],
    side_effects=["read-only"],
    timeout_s=10,
    execution_order=10,
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

    # (1) 포트 LISTEN — target 의 csc/console 포트 + _INSTANCES 의 시그널링/미디어
    target = (ctx.opts or {}).get("target") or "verify"
    ports = _required_ports(ctx)
    lines.append(f"### (1) Stage 5 결과물 LISTEN (target={target}, n={len(ports)})")
    for port, proto, host, label in ports:
        listening = shell.port_listening(port, proto, host=host)
        host_lab = f"{host}:" if host else ""
        mark = "[OK]" if listening else "[FAIL]"
        lines.append(f"- {mark} {label} ({host_lab}{port}/{proto}) {'LISTEN' if listening else '미기동'}")
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
                   "  ./cims-verify stage4   # 새 manifest 산출\n"
                   "  ./cims-verify stage5   # 새 패키지로 재배포\n"
                   "  ./cims-verify stage6   # 통합 검증 진입")
    return ItemResult(
        id="S6-ENTRY-CHECK", name="진입 조건 체크",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=detail, stage=6,
    )
