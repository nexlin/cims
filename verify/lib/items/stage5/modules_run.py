"""S5-MODULES-RUN (그룹) — 배포된 csp/cmp 기동."""
from __future__ import annotations

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext
from ...common import pkg_manifest as _pkgm
from ._legacy import get_legacy_results, step_result


@verify_item(
    id="S5-MODULES-RUN", stage=5, category="배포",
    name="csp/cmp Start (5060/udp + 9000/udp LISTEN — sim install-only)",
    is_group=True,
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-start", "network"], timeout_s=180,
    execution_order=60,
)
def modules_run_group(ctx: VerifyContext) -> ItemResult:
    return ItemResult(
        id="S5-MODULES-RUN", name="모듈 기동 (그룹)",
        status=ItemStatus.PASS, stage=5,
    )


@verify_item(
    id="S5-MODULES-RUN-START", stage=5, category="배포",
    name="csp/cmp Start (5060/udp + 9000/udp)",
    parent="S5-MODULES-RUN",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["service-start", "network"], timeout_s=120,
    execution_order=61,
)
def modules_start(ctx: VerifyContext) -> ItemResult:
    by = get_legacy_results(ctx)
    result = step_result(by, [21], "S5-MODULES-RUN-START",
                         "csp/cmp Start (sim install-only)")
    # Immutability marker — Start 가 PASS 일 때만 현재 manifest sha 를
    # .deployed-manifest.json 에 기록해 S6-ENTRY-CHECK 가 매칭 검증.
    if result.status == ItemStatus.PASS:
        sha = _pkgm.write_marker(ctx.dist_dir)
        if sha:
            ctx.w(f"- [marker] .deployed-manifest.json 기록 (manifest_sha={sha[:12]}…)")
        else:
            ctx.w("- [marker] manifest 부재로 marker 미작성")
    return result
