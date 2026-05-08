"""S3-CONFIGURE — configure --local-ip <ens160>."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


@verify_item(
    id="S3-CONFIGURE",
    stage=3, category="환경",
    name="configure --local-ip <ens160> (csp/cmp/csc/cwrtc/console 설정 재생성)",
    depends_on=["S3-RESET"],
    presets=["stage3-full", "pipeline-full", "pre-package"],
    side_effects=["fs-write"], timeout_s=60,
    execution_order=20,
)
def configure(ctx: VerifyContext) -> ItemResult:
    target_ip = ctx.ens_ip or "127.0.0.1"
    # P1 (PSP/PMP 분리) — 인스턴스별 IP 분리 정보를 _INSTANCES 에서 추출.
    # PSP/PMP 가 127.0.0.3 등 별도 loopback 으로 분기되므로 csc.json 의
    # PspNotify.Ip 가 default (CSP_IP) fallback 되지 않도록 명시적 전달.
    from ..stage5._native_steps import _INSTANCES as _NATIVE_INSTANCES
    extra_args: list = []
    role_to_flag = {"psp": "--psp-ip", "isp": "--isp-ip",
                    "pmp": "--pmp-ip", "imp": "--imp-ip"}
    for inst in _NATIVE_INSTANCES:
        flag = role_to_flag.get(inst.get("id"))
        ip = inst.get("local_ip", "")
        if flag and ip and ip != target_ip:
            extra_args.extend([flag, ip])
    rc, out, err = shell.run_cims_sh(
        ctx.repo_root, "configure", "--local-ip", target_ip, *extra_args,
        timeout=60,
    )
    tail = "\n".join((out + err).splitlines()[-30:])
    label = " ".join(extra_args) if extra_args else ""
    ctx.w(f"## S3-CONFIGURE — configure --local-ip {target_ip} {label}")
    ctx.w("```")
    for line in tail.splitlines(): ctx.w(line)
    ctx.w("```")
    ctx.w()
    ok = (rc == 0)
    return ItemResult(
        id="S3-CONFIGURE", name=f"configure (ip={target_ip})",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=tail, stage=3,
    )
