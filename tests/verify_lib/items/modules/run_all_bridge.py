"""run_all.py 9개 단위 테스트 모듈 → verify_lib MODULE-* 항목 brigde.

run_all.py 의 `run_*_tests()` 함수들이 반환하는 dict 형식:
  {module, total, pass, fail, skip, results: [{id, name, status, detail, elapsed_ms}]}

이 결과를 ItemResult 로 변환:
- 부모 항목 status: pass==total 이면 PASS, 그 외 FAIL (skip 만 있으면 SKIP)
- children: 개별 항목들 (CSC-AUTH-01 같은 기존 ID 그대로 보존)

각 모듈은 run_all.py 의 모듈 함수를 직접 import 호출 — subprocess 가 아니므로
Phase 1 환경(Test-CSC 4421 등) 이 떠있어야 정상 동작.
"""
from __future__ import annotations

import os
import sys
import time
from typing import Callable

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


# tests/ 디렉토리를 sys.path 에 추가 (run_all.py 모듈들이 conftest 등 import)
_TESTS_DIR = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))
if _TESTS_DIR not in sys.path:
    sys.path.insert(0, _TESTS_DIR)


def _children_from(results: list, phase: int = 1) -> list:
    """run_all.py 결과의 results[] 를 ItemResult.children 으로 변환."""
    children = []
    for r in results or []:
        children.append(ItemResult(
            id=r.get("id", "?"),
            name=r.get("name", "?"),
            status=r.get("status", "FAIL"),
            detail=(r.get("detail") or "")[:300],
            elapsed_ms=int(r.get("elapsed_ms") or 0),
            phase=phase,
        ))
    return children


def _run_module(module_name: str, fn: Callable, ctx: VerifyContext) -> ItemResult:
    """run_*_tests() 호출 + 결과를 ItemResult 로 wrap."""
    item_id = f"MODULE-{module_name}"
    title = f"{module_name} 모듈 단위 테스트"
    t0 = time.time()
    only = ctx.only_children_for(item_id)
    try:
        d = fn(only=only) if only else fn()
        d = d or {}
    except Exception as e:
        ctx.w(f"### {item_id} — {title}")
        ctx.w(f"- [FAIL] {type(e).__name__}: {e}")
        ctx.w()
        return ItemResult(
            id=item_id, name=title, status=ItemStatus.FAIL,
            detail=f"{type(e).__name__}: {e}",
            elapsed_ms=int((time.time() - t0) * 1000), phase=1,
        )
    total = int(d.get("total") or 0)
    n_pass = int(d.get("pass") or 0)
    n_fail = int(d.get("fail") or 0)
    n_skip = int(d.get("skip") or 0)
    if total == 0:
        status = ItemStatus.SKIP
        detail = f"항목 0건 (total=0)"
    elif n_fail > 0:
        status = ItemStatus.FAIL
        detail = f"PASS {n_pass}/{total} (FAIL {n_fail}, SKIP {n_skip})"
    else:
        status = ItemStatus.PASS
        detail = f"PASS {n_pass}/{total} (SKIP {n_skip})"
    children = _children_from(d.get("results"), phase=1)

    ctx.w(f"### {item_id} — {title}")
    ctx.w(f"- 결과: {status} ({total}항목, PASS {n_pass} / FAIL {n_fail} / SKIP {n_skip})")
    if children:
        ctx.w("```")
        for c in children[:30]:
            mark = {"PASS": "✓", "FAIL": "✗", "SKIP": "·"}.get(c.status, "?")
            ctx.w(f"  {mark} {c.id:18} {c.status:5} {c.name}")
        if len(children) > 30:
            ctx.w(f"  ... ({len(children) - 30}건 추가)")
        ctx.w("```")
    ctx.w()

    return ItemResult(
        id=item_id, name=title, status=status, detail=detail,
        elapsed_ms=int((time.time() - t0) * 1000), phase=1,
        children=children,
    )


# ─────────────────────────────────────────────────────────────
# 9개 모듈 등록 — depends_on=P1-START (서비스가 떠있어야 동작)
# ─────────────────────────────────────────────────────────────
def _make_bridge(module_name: str, import_fn_name: str, module_path: str,
                 timeout_s: int = 600) -> Callable:
    """closure 로 lazy import — verify_lib 가 먼저 로드되어도 conftest deps 충돌 없음."""
    def _impl(ctx: VerifyContext) -> ItemResult:
        try:
            mod = __import__(module_path, fromlist=[import_fn_name])
            fn = getattr(mod, import_fn_name)
        except Exception as e:
            ctx.w(f"### MODULE-{module_name} — import 실패")
            ctx.w(f"- [FAIL] {type(e).__name__}: {e}")
            ctx.w()
            return ItemResult(
                id=f"MODULE-{module_name}", name=f"{module_name} 모듈 단위 테스트",
                status=ItemStatus.FAIL,
                detail=f"import {module_path}.{import_fn_name} 실패: {e}",
                phase=1,
            )
        return _run_module(module_name, fn, ctx)
    return _impl


# 등록 — 각 항목 depends_on=P1-START (Phase 1 모듈 기동 후 실행).
# 단, 사용자가 직접 부분 실행하면 의존성 무시 가능 (이미 실행 중인 환경 가정).
_MODULES = [
    # (display_id, fn name, module path, timeout_s)
    ("CMP",          "run_cmp_tests",              "test_cmp",              300),
    ("CSP",          "run_csp_tests",              "test_csp",              300),
    ("CSC",          "run_csc_tests",              "test_csc",              300),
    ("E2E",          "run_e2e_tests",              "test_e2e",              300),
    ("VOLTE",        "run_volte_tests",            "test_volte_service",    400),
    ("PTT",          "run_ptt_tests",              "test_ptt_service",      400),
    ("MEDIA",        "run_media_tests",            "test_media",            400),
    ("SIP-RUNTIME",  "run_sip_runtime_tests",      "test_sip_runtime",      400),
    ("AGENT-DEPLOY", "run_agent_deployment_tests", "test_agent_deployment", 600),
]


for _disp_id, _fn, _mod, _timeout in _MODULES:
    _impl = _make_bridge(_disp_id, _fn, _mod, _timeout)
    _impl.__name__ = f"_module_{_disp_id.lower().replace('-','_')}"
    verify_item(
        id=f"MODULE-{_disp_id}",
        phase=1, category="모듈",
        name=f"{_disp_id} 단위 테스트 (run_all.py {_mod})",
        depends_on=["P1-START"],
        presets=["phase1-modules"],
        side_effects=["api-call", "db-write"],
        timeout_s=_timeout,
        description=f"tests/{_mod}.py 의 {_fn}() 호출",
    )(_impl)
