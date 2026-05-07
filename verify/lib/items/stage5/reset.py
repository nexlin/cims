"""S5-RESET — Phase 2 step 01 (Cleanup).

** Note **: native Python step 01 (`_native_steps.step_01_cleanup`) 가 인프라로
존재하지만 stage5-full 실행 시 alphabetical 순서로 S5-RESET 이 마지막에 실행되어
이미 deploy 된 csc-server/csp-server/cmp-server/sim-server 디렉토리를 wipe 하는
회귀가 있다 (cmd_reset 의 pkill + rm -rf 가 keep-processes 와 무관히 동작).

따라서 현재는 `_legacy.step_result` 경로를 사용해 _verify_phase2 의 step 01
(deploy 시작 전 cleanup) 결과를 그대로 반환한다. native 사용은 registry 에
execution_order 메타 또는 dependency hint 가 추가된 후 재전환 예정.
"""
from __future__ import annotations

from ...registry import verify_item, ItemResult
from ...context import VerifyContext
from ._legacy import get_legacy_results, step_result


@verify_item(
    id="S5-RESET",
    stage=5, category="배포",
    name="배포본 reset (로그/DB/csc-server wipe — 가입자 보존)",
    presets=["stage5-full", "pipeline-full", "post-deploy"],
    side_effects=["fs-write", "db-truncate"], timeout_s=120,
)
def reset(ctx: VerifyContext) -> ItemResult:
    by = get_legacy_results(ctx)
    return step_result(by, [1], "S5-RESET", "배포본 reset (cleanup)")
