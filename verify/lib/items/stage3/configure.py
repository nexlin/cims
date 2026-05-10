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
    # ens_ip 빈 값 시 즉시 FAIL — 127.0.0.1 fallback 은 dev 가 외부 단말 접속 불가
    # + 배포본 (LocalIp 127.0.0.1) 과 즉각 충돌하므로 무방비 fallback 차단.
    if not ctx.ens_ip:
        msg = (
            "local_ip 미결정 — dev 모듈을 외부 접속 가능한 IP 로 bind 할 수 없음.\n"
            "다음 중 하나로 결정 필요:\n"
            "  1) ./cims.sh init   (권장 — .cims/server.local.json 자동 생성)\n"
            "  2) CIMS_LOCAL_IP=<IP> env 전달\n"
            "  3) default route 의 src IP 자동 감지 가능한 환경에서 실행"
        )
        ctx.w("## S3-CONFIGURE — local_ip 미결정으로 FAIL")
        for line in msg.splitlines(): ctx.w(f"- {line}")
        ctx.w()
        return ItemResult(
            id="S3-CONFIGURE", name="configure (local_ip 미결정)",
            status=ItemStatus.FAIL, detail=msg, stage=3,
        )
    target_ip = ctx.ens_ip
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
