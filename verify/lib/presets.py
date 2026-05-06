"""검증 프리셋 — 항목 묶음 정의.

`cims_verify run --preset <name>` 으로 호출. UI 의 프리셋 버튼도 이 정의를 받아 표시.

stage1-full ~ stage6-full   — 단일 stage 전체 실행
stage3-quick                — sanity 확인용 (configure/start/health 만)
stage6-volte / stage6-ptt   — 분리 시나리오
pipeline-full               — 전체 (S1 → S6)
pre-package                 — 패키지 전 (S1 → S4)
post-deploy                 — 배포 후 (S5 → S6)
"""
from __future__ import annotations

from typing import Callable

from .registry import get_items


def _all_stage(stage: int) -> Callable:
    """주어진 stage 의 부모/평면 항목 ID list (자식은 부모 통해 자동 펼쳐짐)."""
    def _fn() -> list:
        return [m.id for m in get_items(stage=stage, include_children=False)]
    return _fn


def _stages(stage_list: list) -> Callable:
    def _fn() -> list:
        out: list = []
        for s in stage_list:
            out += [m.id for m in get_items(stage=s, include_children=False)]
        return out
    return _fn


_PRESETS: dict = {
    # ── 단일 stage ──
    "stage1-full":    _all_stage(1),
    "stage2-full":    _all_stage(2),
    "stage3-full":    _all_stage(3),
    "stage3-quick":   ["S3-START", "S3-HEALTH"],
    "stage4-full":    _all_stage(4),
    "stage5-full":    _all_stage(5),
    "stage6-full":    _all_stage(6),
    "stage6-volte":   ["S6-ENTRY-CHECK", "S6-SEED",
                       "S6-SCN-VOLTE-VOICE", "S6-SCN-VOLTE-VIDEO", "S6-SUMMARY"],
    "stage6-ptt":     ["S6-ENTRY-CHECK", "S6-SEED",
                       "S6-SCN-PTT-VOICE",   "S6-SCN-PTT-VIDEO",   "S6-SUMMARY"],

    # ── 묶음 ──
    "pipeline-full":  _stages([1, 2, 3, 4, 5, 6]),
    "pre-package":    _stages([1, 2, 3, 4]),
    "post-deploy":    _stages([5, 6]),
}


def list_presets() -> list:
    out = []
    for name in _PRESETS:
        items = resolve_preset(name)
        out.append({"name": name, "items": items})
    return out


def resolve_preset(name: str) -> list:
    p = _PRESETS.get(name)
    if p is None:
        return []
    if callable(p):
        return p()
    return list(p)


def register_preset(name: str, items) -> None:
    _PRESETS[name] = items
