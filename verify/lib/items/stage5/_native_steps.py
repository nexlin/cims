"""Stage 5 native step 구현 — _verify_phase2 의 점진 Python 포팅.

_legacy.py 가 cims.sh _verify_phase2 본체 1회 호출로 22단계를 한꺼번에 처리하는
어댑터 패턴을 대체하기 위한 native Python step 구현 모듈.

각 step 은 self-contained 함수: ctx 에서 필요한 상태를 읽고, ItemResult 반환.
ctx.state["_s5_native"] 에 결과를 캐시해 동일 step 의 재호출을 방지.

마이그레이션 절차 (점진):
  1. 가장 단순/독립적인 step 부터 native 함수로 구현 (현재: step 01)
  2. 해당 step 의 verify_item 자식 함수가 _legacy.step_result() 대신 native 호출
  3. 22 step 모두 포팅되면 _legacy.py 와 cims.sh _verify_phase2 제거

현재 native 구현:
  - step 01 (Cleanup) — cmd_reset --all --keep-processes

미포팅 step (2~22) 은 _legacy.get_legacy_results 로 위임.

** 알려진 한계 **
  네이티브로 포팅된 step 은 _legacy 가 호출하는 _verify_phase2 안에서도 함께
  실행되므로 (bash 본체는 step 1~22 monolithic) 중복 실행이 발생한다.
  cmd_reset 은 idempotent 라 functional 영향 X. 추후 cims.sh _verify_phase2 에
  --skip-step=N,... 플래그를 추가해 중복 제거 예정.
"""
from __future__ import annotations

from ...registry import ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell


_STATE_KEY = "_s5_native"
"""ctx.state 안의 native step 결과 cache: {step_no: ItemResult}."""


def _save(ctx: VerifyContext, step_no: int, result: ItemResult) -> None:
    cache = ctx.state.setdefault(_STATE_KEY, {})
    cache[step_no] = result


def already_ran(ctx: VerifyContext, step_no: int) -> bool:
    return step_no in ctx.state.get(_STATE_KEY, {})


def get_native_result(ctx: VerifyContext, step_no: int) -> ItemResult:
    return ctx.state[_STATE_KEY][step_no]


# ─────────────────────────────────────────────────────────────
# Step 01 — Cleanup (cmd_reset --all --keep-processes)
# ─────────────────────────────────────────────────────────────
def step_01_cleanup(ctx: VerifyContext) -> ItemResult:
    """Step 01 — 검증 환경 초기화 (가입자 보존, TB 3종 유지).

    cims.sh cmd_reset --all --keep-processes 호출 — 다음 작업 수행:
      - LOG_DIR/*.log + service_log/ + msg_log/ wipe
      - /tmp/cims-agent-* + build/dist/{csc,csp,cmp,sim}-server/ rm -rf
      - 발급 cert (cert/agent_mtls/issued) 정리
      - DB: agent_deployment/_job/_metric TRUNCATE
        (cims_agent 는 TB 보존 record 외 DELETE)

    --keep-processes: TB-CSC(4419) / TB-Console(3000) / TB-agent(9902) 보존.
    """
    if already_ran(ctx, 1):
        return get_native_result(ctx, 1)

    rc, out, err = shell.run_cims_sh(
        ctx.repo_root, "reset", "--all", "--keep-processes",
        timeout=120,
    )
    full = (out or "") + (err or "")
    tail_lines = full.splitlines()[-15:]
    tail = "\n".join(tail_lines)
    status = ItemStatus.PASS if rc == 0 else ItemStatus.FAIL
    detail = f"rc={rc}\n{tail}" if tail else f"rc={rc}"
    result = ItemResult(
        id="S5-RESET", name="배포본 reset (cleanup)",
        status=status, detail=detail, stage=5,
    )
    _save(ctx, 1, result)
    return result
