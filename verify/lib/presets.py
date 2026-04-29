"""검증 프리셋 — 항목 묶음 정의.

`cims_verify run --preset <name>` 으로 호출. UI 의 프리셋 버튼도 이 정의를 받아 표시.

프리셋은 항목 ID 의 정적 리스트. 동적 결정이 필요하면 "phase{N}-full" 처럼
get_items(phase=N) 결과 전체를 반환하는 함수형 프리셋도 지원.
"""
from __future__ import annotations

from typing import Callable, Optional

from .registry import get_items


# ─────────────────────────────────────────────────────────────
# 동적 프리셋 (함수) — registry 가 채워진 후 평가
# ─────────────────────────────────────────────────────────────
def _all_phase(phase: int) -> Callable:
    def _fn() -> list:
        return [m.id for m in get_items(phase=phase)]
    return _fn


# 프리셋 정의 — 정적 ID 리스트 또는 동적 함수
_PRESETS: dict = {
    "phase1-full":     _all_phase(1),                  # 전체 (환경 + 시나리오)
    "phase1-quick":    ["P1-PREFLIGHT", "P1-HEALTH"],  # 빠른 sanity
    "phase2-full":     _all_phase(2),
    "phase3-full":     _all_phase(3),
    "phase3-volte":    ["P3-ENTRY-CHECK", "P3-SEED",
                        "P3-SCN-VOLTE-VOICE", "P3-SCN-VOLTE-VIDEO", "P3-SUMMARY"],
    "phase3-ptt":      ["P3-ENTRY-CHECK", "P3-SEED",
                        "P3-SCN-PTT-VOICE",  "P3-SCN-PTT-VIDEO", "P3-SUMMARY"],
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
