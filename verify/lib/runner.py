"""항목 실행 엔진.

run_items(ctx, item_ids):
- 의존성 토폴로지 정렬 후 순차 실행
- 각 항목의 결과를 ItemResult 로 수집
- 의존 항목이 FAIL 이면 후속 항목 SKIP

함수 반환은 ItemResult 또는 bool. bool 인 경우 자동 ItemResult 변환.
"""
from __future__ import annotations

import time
import traceback
from typing import Iterable

from .registry import ItemMeta, ItemResult, ItemStatus, get_item, get_items
from .context import VerifyContext


def _topo_sort(item_ids: list) -> list:
    """선택된 항목들을 depends_on 기반으로 위상 정렬."""
    selected = set(item_ids)
    metas = {}
    for iid in item_ids:
        rec = get_item(iid)
        if rec is None:
            raise ValueError(f"unknown item id: {iid}")
        metas[iid] = rec[0]

    # depends_on 중 선택 set 안에 있는 것만 edge 로 사용 (외부 의존은 무시)
    visited, order = set(), []
    def dfs(iid: str, stack: tuple = ()):
        if iid in visited: return
        if iid in stack:
            raise ValueError(f"cycle detected: {' -> '.join(stack + (iid,))}")
        for dep in metas[iid].depends_on:
            if dep in selected:
                dfs(dep, stack + (iid,))
        visited.add(iid)
        order.append(iid)
    for iid in item_ids:
        dfs(iid)
    return order


def _emit(line: str) -> None:
    """stdout 마커 출력 (backend 가 polling 으로 파싱).

    형식:
      [VERIFY] item-start: <id> idx=<i>/<N> name=<...>
      [VERIFY] item-end:   <id> status=<PASS|FAIL|SKIP> elapsed_ms=<n>
      [VERIFY] child-result: <parent_id>.<child_id> status=<...> elapsed_ms=<n> name=<...>
    """
    print(line, flush=True)


def run_items(ctx: VerifyContext, item_ids: Iterable[str]) -> list:
    """선택 항목 실행. ItemResult 리스트 반환 (실행 순서)."""
    ids = list(item_ids)
    if not ids:
        return []
    ordered = _topo_sort(ids)
    n_total = len(ordered)
    _emit(f"[VERIFY] run-start: total={n_total} ids={','.join(ordered)}")
    results: list = []
    failed: set = set()
    for idx, iid in enumerate(ordered, start=1):
        rec = get_item(iid)
        if rec is None:
            _emit(f"[VERIFY] item-start: {iid} idx={idx}/{n_total} name=unknown")
            _emit(f"[VERIFY] item-end: {iid} status=FAIL elapsed_ms=0")
            results.append(ItemResult(id=iid, name=iid, status=ItemStatus.FAIL,
                                      detail="unknown item", phase=0))
            continue
        meta, fn = rec
        # 의존 항목 중 FAIL 있으면 SKIP
        skip = [d for d in meta.depends_on if d in failed]
        if skip:
            _emit(f"[VERIFY] item-start: {iid} idx={idx}/{n_total} name={meta.name}")
            _emit(f"[VERIFY] item-end: {iid} status=SKIP elapsed_ms=0")
            results.append(ItemResult(
                id=iid, name=meta.name, status=ItemStatus.SKIP,
                detail=f"의존 항목 실패: {','.join(skip)}", phase=meta.phase,
            ))
            failed.add(iid)
            continue
        # 실행
        _emit(f"[VERIFY] item-start: {iid} idx={idx}/{n_total} name={meta.name}")
        t0 = time.time()
        try:
            r = fn(ctx)
            elapsed_ms = int((time.time() - t0) * 1000)
            if isinstance(r, ItemResult):
                if not r.elapsed_ms: r.elapsed_ms = elapsed_ms
                if not r.phase: r.phase = meta.phase
                if not r.id: r.id = meta.id
                if not r.name: r.name = meta.name
            elif isinstance(r, tuple) and len(r) == 2:
                ok, detail = r
                r = ItemResult(
                    id=meta.id, name=meta.name,
                    status=ItemStatus.PASS if ok else ItemStatus.FAIL,
                    detail=str(detail), elapsed_ms=elapsed_ms, phase=meta.phase,
                )
            elif isinstance(r, bool):
                r = ItemResult(
                    id=meta.id, name=meta.name,
                    status=ItemStatus.PASS if r else ItemStatus.FAIL,
                    elapsed_ms=elapsed_ms, phase=meta.phase,
                )
            else:
                r = ItemResult(
                    id=meta.id, name=meta.name, status=ItemStatus.PASS,
                    detail=f"return={type(r).__name__}", elapsed_ms=elapsed_ms,
                    phase=meta.phase,
                )
        except Exception as e:
            elapsed_ms = int((time.time() - t0) * 1000)
            r = ItemResult(
                id=meta.id, name=meta.name, status=ItemStatus.FAIL,
                detail=f"{type(e).__name__}: {e}\n{traceback.format_exc()[-400:]}",
                elapsed_ms=elapsed_ms, phase=meta.phase,
            )
        if r.status == ItemStatus.FAIL:
            failed.add(iid)
        # 자식 결과 stream — 부모 ItemResult 의 children 을 부모 종료 직전에 일괄 emit
        for c in (r.children or []):
            _emit(
                f"[VERIFY] child-result: {iid}.{c.id} "
                f"status={c.status} elapsed_ms={c.elapsed_ms} name={c.name}"
            )
        _emit(f"[VERIFY] item-end: {iid} status={r.status} elapsed_ms={r.elapsed_ms}")
        results.append(r)
    _emit(f"[VERIFY] run-end: total={n_total} pass={sum(1 for r in results if r.status == 'PASS')} fail={sum(1 for r in results if r.status == 'FAIL')} skip={sum(1 for r in results if r.status == 'SKIP')}")
    return results


def resolve_selection(phase: int = None, items: list = None,
                      preset: str = None, presets_db: dict = None) -> list:
    """phase/preset/items 옵션을 단일 항목 ID 리스트로 정규화."""
    if items:
        return list(items)
    if preset and presets_db:
        ids = presets_db.get(preset)
        if not ids:
            raise ValueError(f"unknown preset: {preset}")
        return list(ids)
    if phase is not None:
        return [m.id for m in get_items(phase=phase)]
    raise ValueError("phase / items / preset 중 하나는 지정해야 함")
