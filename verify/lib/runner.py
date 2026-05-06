"""항목 실행 엔진.

run_items(ctx, item_ids):
- 그룹 ID 는 자식으로 펼친 후 leaf 만 실행
- 의존성 토폴로지 정렬 후 순차 실행 (stage 우선 안정 정렬)
- 의존 항목이 FAIL 이면 후속 항목 SKIP (개별 의존)
- **Stage gate**: stage N 에 FAIL 1개라도 발생 시 stage > N 의 모든 leaf 자동 BLOCKED
  (`stage_gate=False` 로 비활성 가능 — 단일 stage 실행 시에는 무의미)
- 그룹은 자식 결과의 worst-status 로 합산하여 ItemResult 반환

stdout 마커 형식 (backend 가 polling 으로 파싱):
  [VERIFY] run-start: total=<N> ids=<csv>
  [VERIFY] item-start: <id> stage=<N> idx=<i>/<N> name=<...>
  [VERIFY] item-end:   <id> status=<PASS|FAIL|SKIP|BLOCKED> elapsed_ms=<n>
  [VERIFY] child-result: <parent_id>.<child_id> status=<...> elapsed_ms=<n> name=<...>
  [VERIFY] group-end:  <parent_id> status=<...> child_count=<n>
  [VERIFY] stage-blocked: stage=<M> reason=stage<N>-FAIL count=<n>
  [VERIFY] run-end: total=<N> pass=<n> fail=<n> skip=<n> blocked=<n>
"""
from __future__ import annotations

import time
import traceback
from typing import Iterable, Optional

from .registry import (
    ItemMeta, ItemResult, ItemStatus, get_item, get_items,
    expand_to_leaves, selected_groups, get_children,
)
from .context import VerifyContext


# worst status 우선순위 (group 합산용) — 큰 값이 worst
_STATUS_RANK = {
    ItemStatus.PASS:    0,
    ItemStatus.SKIP:    1,
    ItemStatus.BLOCKED: 2,
    ItemStatus.FAIL:    3,
}


def _topo_sort(item_ids: list) -> list:
    """선택된 항목들을 depends_on 기반으로 위상 정렬. 평면 leaf 만 대상.

    동일 우선순위(의존성 무관) 항목은 (stage, id) 안정 순서로 진입 → stage 단위
    실행이 stage 번호 오름차순으로 인접 배치되어 stage gate 가 일관되게 동작.
    """
    selected = set(item_ids)
    metas = {}
    for iid in item_ids:
        rec = get_item(iid)
        if rec is None:
            raise ValueError(f"unknown item id: {iid}")
        metas[iid] = rec[0]

    # stage·id 기준 1차 정렬 → DFS 순서를 결정적으로
    stable_ids = sorted(item_ids, key=lambda x: (metas[x].stage, x))

    visited, order = set(), []
    def dfs(iid: str, stack: tuple = ()):
        if iid in visited: return
        if iid in stack:
            raise ValueError(f"cycle detected: {' -> '.join(stack + (iid,))}")
        for dep in sorted(
            metas[iid].depends_on,
            key=lambda d: (metas[d].stage if d in metas else 0, d),
        ):
            if dep in selected:
                dfs(dep, stack + (iid,))
        visited.add(iid)
        order.append(iid)
    for iid in stable_ids:
        dfs(iid)
    return order


def _emit(line: str) -> None:
    """stdout 마커 출력 (backend 가 polling 으로 파싱)."""
    print(line, flush=True)


def _run_one(ctx: VerifyContext, meta: ItemMeta, fn) -> ItemResult:
    """단일 leaf 항목 실행 → ItemResult 반환."""
    t0 = time.time()
    try:
        r = fn(ctx)
        elapsed_ms = int((time.time() - t0) * 1000)
        if isinstance(r, ItemResult):
            if not r.elapsed_ms: r.elapsed_ms = elapsed_ms
            if not r.stage:      r.stage = meta.stage
            if not r.id:         r.id = meta.id
            if not r.name:       r.name = meta.name
            return r
        if isinstance(r, tuple) and len(r) == 2:
            ok, detail = r
            return ItemResult(
                id=meta.id, name=meta.name,
                status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                detail=str(detail), elapsed_ms=elapsed_ms, stage=meta.stage,
            )
        if isinstance(r, bool):
            return ItemResult(
                id=meta.id, name=meta.name,
                status=ItemStatus.PASS if r else ItemStatus.FAIL,
                elapsed_ms=elapsed_ms, stage=meta.stage,
            )
        return ItemResult(
            id=meta.id, name=meta.name, status=ItemStatus.PASS,
            detail=f"return={type(r).__name__}", elapsed_ms=elapsed_ms,
            stage=meta.stage,
        )
    except Exception as e:
        elapsed_ms = int((time.time() - t0) * 1000)
        return ItemResult(
            id=meta.id, name=meta.name, status=ItemStatus.FAIL,
            detail=f"{type(e).__name__}: {e}\n{traceback.format_exc()[-400:]}",
            elapsed_ms=elapsed_ms, stage=meta.stage,
        )


def _summarize_group(parent_meta: ItemMeta, child_results: list) -> ItemResult:
    """자식 결과 list → 그룹 ItemResult 합성 (worst status, 합산 elapsed)."""
    worst = ItemStatus.PASS
    total_ms = 0
    for c in child_results:
        if _STATUS_RANK.get(c.status, 0) > _STATUS_RANK.get(worst, 0):
            worst = c.status
        total_ms += c.elapsed_ms or 0
    n_pass = sum(1 for c in child_results if c.status == ItemStatus.PASS)
    n_fail = sum(1 for c in child_results if c.status == ItemStatus.FAIL)
    n_skip = sum(1 for c in child_results if c.status == ItemStatus.SKIP)
    n_blk  = sum(1 for c in child_results if c.status == ItemStatus.BLOCKED)
    detail_parts = []
    if n_pass: detail_parts.append(f"PASS {n_pass}")
    if n_fail: detail_parts.append(f"FAIL {n_fail}")
    if n_skip: detail_parts.append(f"SKIP {n_skip}")
    if n_blk:  detail_parts.append(f"BLOCKED {n_blk}")
    return ItemResult(
        id=parent_meta.id, name=parent_meta.name,
        status=worst, detail=", ".join(detail_parts) or "no children",
        elapsed_ms=total_ms, stage=parent_meta.stage,
        children=list(child_results),
    )


def run_items(ctx: VerifyContext, item_ids: Iterable[str], stage_gate: bool = True) -> list:
    """선택 항목 실행. ItemResult 리스트 반환 (실행 순서, 그룹은 자식 합산 후 1건).

    그룹 ID 가 입력에 포함되면 자식으로 펼친 leaf 만 실행되고, 그룹 자체는
    자식 결과 모음으로 종합되어 결과 list 에 1번 등장한다.

    Args:
      stage_gate: True (기본) — stage N 에 FAIL 1개라도 발생 시 stage>N 의
        leaf 는 실행 없이 BLOCKED 처리. False — 의존성 SKIP 만.
    """
    raw_ids = list(item_ids)
    if not raw_ids:
        return []

    # 그룹 → leaf 펼치기. 입력에 그룹이 있으면 그룹도 별도로 추적
    groups = selected_groups(raw_ids)
    leaves = expand_to_leaves(raw_ids)
    ordered = _topo_sort(leaves)
    n_total = len(ordered)
    _emit(f"[VERIFY] run-start: total={n_total} ids={','.join(ordered)}")

    leaf_results: dict = {}                            # iid → ItemResult
    failed: set = set()
    failed_stages: set = set()                         # stage gate — FAIL 발생 stage
    blocked_stage_counts: dict = {}                    # stage gate 통계 emit 용
    for idx, iid in enumerate(ordered, start=1):
        rec = get_item(iid)
        if rec is None:
            _emit(f"[VERIFY] item-start: {iid} stage=0 idx={idx}/{n_total} name=unknown")
            _emit(f"[VERIFY] item-end: {iid} status=FAIL elapsed_ms=0")
            leaf_results[iid] = ItemResult(
                id=iid, name=iid, status=ItemStatus.FAIL,
                detail="unknown item", stage=0,
            )
            failed.add(iid)
            continue
        meta, fn = rec
        # Stage gate — 옛 stage 가 FAIL 이면 후속 stage 자동 BLOCKED
        if stage_gate and failed_stages and meta.stage > min(failed_stages):
            blocker = min(s for s in failed_stages if s < meta.stage)
            _emit(f"[VERIFY] item-start: {iid} stage={meta.stage} idx={idx}/{n_total} name={meta.name}")
            _emit(f"[VERIFY] item-end: {iid} status=BLOCKED elapsed_ms=0")
            leaf_results[iid] = ItemResult(
                id=iid, name=meta.name, status=ItemStatus.BLOCKED,
                detail=f"선행 Stage {blocker} 실패로 차단",
                stage=meta.stage,
            )
            blocked_stage_counts[meta.stage] = blocked_stage_counts.get(meta.stage, 0) + 1
            # 부모 그룹에도 child-result 마커 발행
            if meta.parent and meta.parent in groups:
                _emit(
                    f"[VERIFY] child-result: {meta.parent}.{meta.id} "
                    f"status=BLOCKED elapsed_ms=0 name={meta.name}"
                )
            continue
        # 의존 항목 중 FAIL 이 있으면 SKIP
        skip = [d for d in meta.depends_on if d in failed]
        if skip:
            _emit(f"[VERIFY] item-start: {iid} stage={meta.stage} idx={idx}/{n_total} name={meta.name}")
            _emit(f"[VERIFY] item-end: {iid} status=SKIP elapsed_ms=0")
            leaf_results[iid] = ItemResult(
                id=iid, name=meta.name, status=ItemStatus.SKIP,
                detail=f"의존 항목 실패: {','.join(skip)}", stage=meta.stage,
            )
            failed.add(iid)
            continue

        _emit(f"[VERIFY] item-start: {iid} stage={meta.stage} idx={idx}/{n_total} name={meta.name}")
        # 자식 항목인 경우 — 부모(group)에게 child-result 마커도 발행
        result = _run_one(ctx, meta, fn)
        # ItemResult 안의 children 도 streaming (e.g. legacy P5 group 안에서 step 22개 추출)
        for c in (result.children or []):
            _emit(
                f"[VERIFY] child-result: {iid}.{c.id} "
                f"status={c.status} elapsed_ms={c.elapsed_ms} name={c.name}"
            )
        # 자식 항목이라면 부모 그룹 child-result 마커
        if meta.parent and meta.parent in groups:
            _emit(
                f"[VERIFY] child-result: {meta.parent}.{meta.id} "
                f"status={result.status} elapsed_ms={result.elapsed_ms} name={meta.name}"
            )
        _emit(f"[VERIFY] item-end: {iid} status={result.status} elapsed_ms={result.elapsed_ms}")
        if result.status == ItemStatus.FAIL:
            failed.add(iid)
            failed_stages.add(meta.stage)
        leaf_results[iid] = result

    # 결과 list 구성: 입력 순서를 따르되, 그룹은 자식 결과 합산
    results: list = []
    visited_groups: set = set()
    for iid in raw_ids:
        rec = get_item(iid)
        if rec is None:
            r = leaf_results.get(iid)
            if r is not None:
                results.append(r)
            continue
        meta = rec[0]
        if meta.is_group:
            if iid in visited_groups: continue
            visited_groups.add(iid)
            child_metas = get_children(iid)
            child_rs = [leaf_results[c.id] for c in child_metas if c.id in leaf_results]
            grp = _summarize_group(meta, child_rs)
            _emit(
                f"[VERIFY] group-end: {iid} status={grp.status} "
                f"child_count={len(child_rs)}"
            )
            results.append(grp)
        else:
            r = leaf_results.get(iid)
            if r is not None:
                results.append(r)

    # stage gate 차단 통계 — 사용자가 한눈에 확인
    if blocked_stage_counts and failed_stages:
        first_failed = min(failed_stages)
        for s in sorted(blocked_stage_counts.keys()):
            _emit(
                f"[VERIFY] stage-blocked: stage={s} "
                f"reason=stage{first_failed}-FAIL count={blocked_stage_counts[s]}"
            )

    n_pass = sum(1 for r in results if r.status == ItemStatus.PASS)
    n_fail = sum(1 for r in results if r.status == ItemStatus.FAIL)
    n_skip = sum(1 for r in results if r.status == ItemStatus.SKIP)
    n_blk  = sum(1 for r in results if r.status == ItemStatus.BLOCKED)
    _emit(
        f"[VERIFY] run-end: total={len(results)} pass={n_pass} "
        f"fail={n_fail} skip={n_skip} blocked={n_blk}"
    )
    return results


def resolve_selection(stage: Optional[int] = None,
                      items: Optional[list] = None,
                      preset: Optional[str] = None,
                      presets_db: Optional[dict] = None) -> list:
    """stage/preset/items 옵션을 단일 항목 ID 리스트로 정규화."""
    if items:
        return list(items)
    if preset and presets_db:
        ids = presets_db.get(preset)
        if not ids:
            raise ValueError(f"unknown preset: {preset}")
        return list(ids)
    if stage is not None:
        # stage 안의 부모/평면 항목만 (자식은 부모 통해 자동 펼쳐짐)
        return [m.id for m in get_items(stage=stage, include_children=False)]
    raise ValueError("stage / items / preset 중 하나는 지정해야 함")
