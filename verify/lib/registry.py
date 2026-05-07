"""검증 항목 registry.

@verify_item 데코레이터로 함수를 등록하면 메타데이터와 함께 전역 dict 에 적재.
CLI 와 backend API 가 이 registry 를 통해 항목 트리·실행을 노출한다.

stage 체계 (S1~S6):
    1: 정적 검사 (lint / format / unit test)
    2: 빌드 (preflight + cmake)
    3: 스모크 (configure + start dev + 1콜 VoIP/PTT)
    4: 패키지화 (tarball + manifest)
    5: 로컬배포 (TB-CSC → 배포본 csc/csp/cmp 체인)
    6: 통합 검증 (VoLTE/PTT 음성·영상)

그룹 (is_group=True) 은 자식만 가지는 부모 항목.
- 부모 함수 본체는 자식만 실행하고 worst child status 로 종합 (runner 가 처리)
- 자식 항목은 parent="<부모 ID>" 로 등록
- UI 표시: 부모 펼침/접힘 + 자식 cascade 선택
"""
from __future__ import annotations

from dataclasses import dataclass, field, asdict
from typing import Any, Callable, Optional


class ItemStatus:
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"
    BLOCKED = "BLOCKED"            # 선행 stage gate 차단으로 진입 불가


# 정렬·필터 편의용 — UI 가 예측 가능한 stage 표시 순서 유지
_STAGE_ORDER = {1: "S1", 2: "S2", 3: "S3", 4: "S4", 5: "S5", 6: "S6"}


@dataclass
class ItemMeta:
    id: str
    stage: int                     # 1~6 (S1~S6)
    category: str                  # 정적 | 환경 | 시나리오 | 배포 | 검증 | ...
    name: str
    is_group: bool = False         # True 면 자식만 갖는 부모 항목
    parent: Optional[str] = None   # 자식 항목의 부모 ID
    depends_on: list = field(default_factory=list)
    presets: list = field(default_factory=list)
    side_effects: list = field(default_factory=list)
    timeout_s: int = 600
    description: str = ""
    # stage 안에서의 실행 순서 힌트. None=알파벳 ID 기본 (대부분 stage 는 depends_on
    # 으로 충분). S5 처럼 deploy 체인이 alphabetical 보다 다른 순서를 요구할 때만
    # 명시. 작은 값일수록 먼저 실행. depends_on 이 있으면 depends_on 이 우선.
    execution_order: Optional[int] = None

    def to_dict(self) -> dict:
        return asdict(self)


@dataclass
class ItemResult:
    """검증 항목 실행 결과 — TestRunner 의 record schema 와 호환."""
    id: str
    name: str
    status: str                    # ItemStatus.PASS / FAIL / SKIP / BLOCKED
    detail: str = ""
    elapsed_ms: int = 0
    stage: int = 0
    children: list = field(default_factory=list)   # 그룹 항목 자식 결과

    def to_dict(self) -> dict:
        return asdict(self)


# ─────────────────────────────────────────────────────────────
# 전역 registry — import 시 채워짐
# ─────────────────────────────────────────────────────────────
_REGISTRY: dict = {}                                # id → (ItemMeta, callable)


def verify_item(
    id: str,
    stage: int,
    category: str,
    name: str,
    is_group: bool = False,
    parent: Optional[str] = None,
    depends_on: Optional[list] = None,
    presets: Optional[list] = None,
    side_effects: Optional[list] = None,
    timeout_s: int = 600,
    description: str = "",
    execution_order: Optional[int] = None,
) -> Callable:
    """검증 항목 등록 데코레이터.

    함수 시그니처: `def fn(ctx: VerifyContext) -> ItemResult` 또는 `bool` 반환.
    is_group=True 인 항목은 runner 가 자식 실행을 자동 처리하므로 본체 함수는
    선택. 데코레이터를 placeholder 함수에 적용해도 무관.

    execution_order: stage 안에서의 실행 순서 힌트 (작은 값이 먼저). None=알파벳
    ID 기본. S5 deploy 체인처럼 alphabetical 이 회귀를 일으키는 경우 명시.
    """
    if stage not in _STAGE_ORDER:
        raise ValueError(f"verify_item invalid stage={stage}; must be 1..6")
    if is_group and parent:
        raise ValueError(f"verify_item {id}: is_group + parent 동시 지정 불가")

    def deco(fn: Callable) -> Callable:
        meta = ItemMeta(
            id=id, stage=stage, category=category, name=name,
            is_group=bool(is_group),
            parent=parent,
            depends_on=list(depends_on or []),
            presets=list(presets or []),
            side_effects=list(side_effects or []),
            timeout_s=timeout_s,
            description=description or (fn.__doc__ or "").strip().split("\n")[0],
            execution_order=execution_order,
        )
        if id in _REGISTRY:
            raise ValueError(f"verify_item duplicate id: {id}")
        _REGISTRY[id] = (meta, fn)
        return fn
    return deco


def get_item(item_id: str) -> Optional[tuple]:
    return _REGISTRY.get(item_id)


def get_items(
    stage: Optional[int] = None,
    preset: Optional[str] = None,
    parent: Optional[str] = None,
    include_children: bool = True,
    include_groups: bool = True,
) -> list:
    """등록 항목을 메타 dict 리스트로 반환.

    Args:
      stage: 특정 stage 만
      preset: 해당 preset 에 포함된 항목만
      parent: 특정 부모의 자식만 (parent ID == ?)
      include_children: False 면 parent 가 있는 자식 제외 (부모/평면만)
      include_groups: False 면 is_group=True 부모 제외
    """
    items = []
    for meta, _fn in _REGISTRY.values():
        if stage is not None and meta.stage != stage:
            continue
        if preset is not None and preset not in meta.presets:
            continue
        if parent is not None and meta.parent != parent:
            continue
        if not include_children and meta.parent:
            continue
        if not include_groups and meta.is_group:
            continue
        items.append(meta)
    items.sort(key=_sort_key)
    return items


def _sort_key(m: ItemMeta) -> tuple:
    """stage 우선, 같은 stage 안에서 execution_order 우선 (None 은 큰 값으로
    fallback → 명시 항목이 미명시 항목보다 먼저), 마지막으로 ID alphabetical.
    """
    return (m.stage, m.execution_order if m.execution_order is not None else 10**6, m.id)


def get_children(parent_id: str) -> list:
    """주어진 부모의 자식 메타를 execution_order/ID 순으로 반환."""
    out = [m for m, _f in _REGISTRY.values() if m.parent == parent_id]
    out.sort(key=_sort_key)
    return out


def get_groups(stage: Optional[int] = None) -> list:
    """is_group=True 인 부모 항목 (옵션: stage 필터)."""
    return [m for m in get_items(stage=stage, include_children=False)
            if m.is_group]


def get_all_metas() -> list:
    return [m for m, _f in _REGISTRY.values()]


def expand_to_leaves(item_ids: list) -> list:
    """선택된 ID list 에서 그룹은 자식으로 펼친 평탄 list 반환.

    - 그룹 ID 가 들어오면 자식 ID 들로 펼침 (그룹 자체는 ID list 에서 제외 — runner 가 별도 처리)
    - 자식 ID 가 들어오면 그대로
    - 평면 ID 는 그대로

    runner 는 expand 후 leaf 만 실행하고, group 결과는 자식 worst 로 합산.
    """
    out: list = []
    seen: set = set()
    for iid in item_ids:
        rec = _REGISTRY.get(iid)
        if rec is None:
            if iid not in seen:
                out.append(iid); seen.add(iid)
            continue
        meta = rec[0]
        if meta.is_group:
            for c in get_children(iid):
                if c.id not in seen:
                    out.append(c.id); seen.add(c.id)
        else:
            if iid not in seen:
                out.append(iid); seen.add(iid)
    return out


def selected_groups(item_ids: list) -> list:
    """선택된 ID list 에서 그룹 ID 만 추출 (자식 결과 합산용)."""
    return [iid for iid in item_ids
            if (rec := _REGISTRY.get(iid)) and rec[0].is_group]


def validate_registry() -> list:
    """auto-import 완료 후 무결성 검증. 문제 list 반환 (빈 list = 정상).

    - 자식의 parent 는 등록된 그룹이어야 함
    - 자식과 부모는 같은 stage 여야 함
    - 그룹은 자식이 1개 이상이어야 함 (UI/runner 의 worst-status 로직 가정)
    - 그룹 자체에 parent 지정 X (이미 데코레이터 단계에서 차단했지만 재확인)
    """
    issues: list = []
    by_id = {m.id: m for m, _f in _REGISTRY.values()}
    for meta, _fn in _REGISTRY.values():
        if meta.parent:
            p = by_id.get(meta.parent)
            if p is None:
                issues.append(f"{meta.id}: parent '{meta.parent}' 가 registry 에 없음")
                continue
            if not p.is_group:
                issues.append(f"{meta.id}: parent '{meta.parent}' 가 is_group=False")
            if p.stage != meta.stage:
                issues.append(
                    f"{meta.id} (stage={meta.stage}) 와 parent {p.id} (stage={p.stage}) "
                    "의 stage 불일치"
                )
        if meta.is_group:
            kids = [m for m, _f in _REGISTRY.values() if m.parent == meta.id]
            if not kids:
                issues.append(f"{meta.id}: is_group=True 인데 자식이 없음")
    return issues


def clear_registry() -> None:
    """테스트용 — 등록 항목 비우기."""
    _REGISTRY.clear()
