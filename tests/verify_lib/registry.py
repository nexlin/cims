"""검증 항목 registry.

@verify_item 데코레이터로 함수를 등록하면 메타데이터와 함께 전역 dict 에 적재.
CLI 와 backend API 가 이 registry 를 통해 항목 트리·실행을 노출한다.
"""
from __future__ import annotations

from dataclasses import dataclass, field, asdict
from typing import Any, Callable, Optional


class ItemStatus:
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"


@dataclass
class ItemMeta:
    id: str
    phase: int                     # 1 | 2 | 3 (or 0 for module-only items)
    category: str                  # 환경 | 시나리오 | 모듈 | 단계 | 검증
    name: str
    depends_on: list = field(default_factory=list)
    presets: list = field(default_factory=list)
    side_effects: list = field(default_factory=list)
    timeout_s: int = 600
    description: str = ""
    parent: Optional[str] = None   # MODULE-CSC 같은 부모 항목 ID

    def to_dict(self) -> dict:
        return asdict(self)


@dataclass
class ItemResult:
    """검증 항목 실행 결과 — TestRunner 의 record schema 와 호환."""
    id: str
    name: str
    status: str                    # ItemStatus.PASS / FAIL / SKIP
    detail: str = ""
    elapsed_ms: int = 0
    phase: int = 0
    children: list = field(default_factory=list)   # 모듈 항목 자식 결과

    def to_dict(self) -> dict:
        return asdict(self)


# ─────────────────────────────────────────────────────────────
# 전역 registry — import 시 채워짐
# ─────────────────────────────────────────────────────────────
_REGISTRY: dict = {}                                # id → (ItemMeta, callable)


def verify_item(
    id: str,
    phase: int,
    category: str,
    name: str,
    depends_on: Optional[list] = None,
    presets: Optional[list] = None,
    side_effects: Optional[list] = None,
    timeout_s: int = 600,
    description: str = "",
    parent: Optional[str] = None,
) -> Callable:
    """검증 항목 등록 데코레이터.

    함수 시그니처: `def fn(ctx: VerifyContext) -> ItemResult` 또는 `bool` 반환.
    """
    def deco(fn: Callable) -> Callable:
        meta = ItemMeta(
            id=id, phase=phase, category=category, name=name,
            depends_on=list(depends_on or []),
            presets=list(presets or []),
            side_effects=list(side_effects or []),
            timeout_s=timeout_s,
            description=description or (fn.__doc__ or "").strip().split("\n")[0],
            parent=parent,
        )
        if id in _REGISTRY:
            raise ValueError(f"verify_item duplicate id: {id}")
        _REGISTRY[id] = (meta, fn)
        return fn
    return deco


def get_item(item_id: str) -> Optional[tuple]:
    return _REGISTRY.get(item_id)


def get_items(
    phase: Optional[int] = None,
    preset: Optional[str] = None,
    parent: Optional[str] = None,
) -> list:
    """등록 항목을 메타 dict 리스트로 반환. 필터링 옵션."""
    items = []
    for meta, _fn in _REGISTRY.values():
        if phase is not None and meta.phase != phase:
            continue
        if preset is not None and preset not in meta.presets:
            continue
        if parent is not None and meta.parent != parent:
            continue
        items.append(meta)
    items.sort(key=lambda m: m.id)
    return items


def get_all_metas() -> list:
    return [m for m, _f in _REGISTRY.values()]


def clear_registry() -> None:
    """테스트용 — 등록 항목 비우기."""
    _REGISTRY.clear()
