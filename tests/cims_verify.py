#!/usr/bin/env python3
"""CIMS 검증 도구 CLI — verify_lib 의 진입점.

명령:
  list [--phase N] [--json]                 — 등록 항목 트리 출력
  list-presets [--json]                     — 프리셋 목록
  describe ITEM_ID [--json]                 — 항목 메타 상세
  run [--phase N] [--items ID,...] [--preset NAME]
       [--json] [--report-dir PATH]
       [--skip-build] [--skip-pkg] [--skip-reset] [--keep-agent]
                                            — 선택 항목 실행

사용 예:
  python3 -m tests.cims_verify list --phase 3 --json
  python3 -m tests.cims_verify run --phase 3
  python3 -m tests.cims_verify run --items P3-ENTRY-CHECK,P3-SEED --json
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path


# tests 디렉토리를 sys.path 에 추가 (단독 실행 시)
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

from verify_lib import (                                        # noqa: E402
    registry, runner, reporting, presets as preset_mod,
)
from verify_lib.context import VerifyContext                    # noqa: E402
from verify_lib import items as _items_pkg                      # noqa: F401, E402  (auto-import 트리거)


def _repo_root_from_here() -> str:
    """tests/cims_verify.py → repo root."""
    return os.path.dirname(_THIS_DIR)


# ─────────────────────────────────────────────────────────────
# 명령 핸들러
# ─────────────────────────────────────────────────────────────
def cmd_list(args: argparse.Namespace) -> int:
    metas = registry.get_items(phase=args.phase) if args.phase \
            else registry.get_all_metas()
    metas = sorted(metas, key=lambda m: (m.phase, m.id))
    if args.json:
        out = {
            "phases": [],
            "presets": preset_mod.list_presets(),
        }
        # phase 별 그룹화
        by_phase: dict = {}
        for m in metas:
            by_phase.setdefault(m.phase, []).append(m.to_dict())
        for p in sorted(by_phase):
            out["phases"].append({"phase": p, "items": by_phase[p]})
        print(json.dumps(out, ensure_ascii=False, indent=2))
    else:
        if not metas:
            print("(등록된 항목 없음)"); return 0
        cur_phase = None
        for m in metas:
            if m.phase != cur_phase:
                cur_phase = m.phase
                print(f"\n[Phase {cur_phase}]")
            cats = f"[{m.category}]" if m.category else ""
            print(f"  {m.id:24} {cats:8} {m.name}")
        print(f"\n프리셋:")
        for p in preset_mod.list_presets():
            print(f"  {p['name']:20} ({len(p['items'])} items)")
    return 0


def cmd_list_presets(args: argparse.Namespace) -> int:
    presets = preset_mod.list_presets()
    if args.json:
        print(json.dumps(presets, ensure_ascii=False, indent=2))
    else:
        for p in presets:
            print(f"{p['name']:20} ({len(p['items'])} items)")
            for iid in p["items"]:
                print(f"  - {iid}")
    return 0


def cmd_describe(args: argparse.Namespace) -> int:
    rec = registry.get_item(args.item_id)
    if rec is None:
        print(f"항목 없음: {args.item_id}", file=sys.stderr)
        return 1
    meta = rec[0]
    if args.json:
        print(json.dumps(meta.to_dict(), ensure_ascii=False, indent=2))
    else:
        print(f"ID:           {meta.id}")
        print(f"Phase:        {meta.phase}")
        print(f"Category:     {meta.category}")
        print(f"Name:         {meta.name}")
        print(f"Depends on:   {', '.join(meta.depends_on) or '-'}")
        print(f"Presets:      {', '.join(meta.presets) or '-'}")
        print(f"Side effects: {', '.join(meta.side_effects) or '-'}")
        print(f"Timeout:      {meta.timeout_s}s")
        if meta.parent: print(f"Parent:       {meta.parent}")
        if meta.description: print(f"Description:  {meta.description}")
    return 0


def _resolve_run_selection(args: argparse.Namespace) -> tuple:
    """args → (item_ids, phase). phase 는 리포트 파일명용 (선택 항목들의 다수결)."""
    items = []
    if args.items:
        items = [s.strip() for s in args.items.split(",") if s.strip()]
        # phase 추정 — 항목들의 phase 중 가장 작은 값 (보통 동일 phase)
        phases = {registry.get_item(i)[0].phase for i in items if registry.get_item(i)}
        phase = min(phases) if phases else (args.phase or 0)
    elif args.preset:
        items = preset_mod.resolve_preset(args.preset)
        if not items:
            print(f"프리셋 없음: {args.preset}", file=sys.stderr); sys.exit(2)
        phases = {registry.get_item(i)[0].phase for i in items if registry.get_item(i)}
        phase = min(phases) if phases else (args.phase or 0)
    elif args.phase:
        phase = args.phase
        items = [m.id for m in registry.get_items(phase=phase)]
        if not items:
            print(f"Phase {phase} 에 등록된 항목 없음", file=sys.stderr); sys.exit(2)
    else:
        print("--phase / --items / --preset 중 하나 지정 필요", file=sys.stderr); sys.exit(2)
    return (items, phase)


def cmd_run(args: argparse.Namespace) -> int:
    item_ids, phase = _resolve_run_selection(args)

    repo_root = args.repo_root or _repo_root_from_here()
    opts = {
        "skip_build": bool(args.skip_build),
        "skip_pkg":   bool(args.skip_pkg),
        "skip_reset": bool(args.skip_reset),
        "keep_agent": bool(args.keep_agent),
        "stop_after": bool(args.stop_after),
    }
    ctx = VerifyContext.create(repo_root=repo_root, phase=phase, opts=opts,
                               report_dir=args.report_dir)

    # 헤더
    reporting.write_header(
        ctx, scope=f"선택 {len(item_ids)} 항목 실행 (phase={phase})",
    )

    t0 = time.time()
    results = runner.run_items(ctx, item_ids)
    elapsed = time.time() - t0

    # 항목별 섹션은 각 항목 내부에서 ctx.w() 로 직접 작성하도록 위임 — 여기는 요약만
    verdict = reporting.determine_verdict(results)
    totals = reporting.write_summary(ctx, results, verdict)
    ctx.report_close()

    # JSON 결과 출력
    payload = {
        "started_at": ctx.ts,
        "elapsed_s": round(elapsed, 2),
        "selected": item_ids,
        "results": [r.to_dict() for r in results],
        "totals": totals,
        "report_path": ctx.report_path,
        "verdict": verdict,
        "phase": phase,
    }
    if args.json:
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        print(f"\n=== Phase {phase} 검증 종료 — 판정: {verdict} ===")
        print(f"리포트: {ctx.report_path}")
        for r in results:
            mark = {"PASS": "✓", "FAIL": "✗", "SKIP": "·"}.get(r.status, "?")
            print(f"  {mark} {r.id:24} {r.status:5}  {r.name}")
        print(f"\n총 {totals['total']} / PASS {totals['pass']} / FAIL {totals['fail']} / SKIP {totals['skip']}")
        print(f"소요: {elapsed:.1f}s")

    return 0 if verdict == "PASS" else 1


# ─────────────────────────────────────────────────────────────
# argparse
# ─────────────────────────────────────────────────────────────
def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="cims_verify",
        description="CIMS 검증 도구 — 항목 단위 메타/실행",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    p_list = sub.add_parser("list", help="등록 항목 트리 출력")
    p_list.add_argument("--phase", type=int, choices=[1, 2, 3])
    p_list.add_argument("--json", action="store_true")
    p_list.set_defaults(_func=cmd_list)

    p_lp = sub.add_parser("list-presets", help="프리셋 목록")
    p_lp.add_argument("--json", action="store_true")
    p_lp.set_defaults(_func=cmd_list_presets)

    p_desc = sub.add_parser("describe", help="항목 메타 상세")
    p_desc.add_argument("item_id")
    p_desc.add_argument("--json", action="store_true")
    p_desc.set_defaults(_func=cmd_describe)

    p_run = sub.add_parser("run", help="선택 항목 실행")
    # --phase / --items / --preset 우선순위: items > preset > phase
    # (UI 가 --phase 3 + --items 로 보낼 수 있도록 mutually_exclusive 해제)
    p_run.add_argument("--phase", type=int, choices=[1, 2, 3])
    p_run.add_argument("--items", help="comma-separated 항목 ID 리스트")
    p_run.add_argument("--preset", help="프리셋 이름")
    p_run.add_argument("--json", action="store_true")
    p_run.add_argument("--report-dir", help="verify_reports 경로 override")
    p_run.add_argument("--repo-root", help="cims repo root (기본: 자동 탐색)")
    p_run.add_argument("--skip-build", action="store_true")
    p_run.add_argument("--skip-pkg",   action="store_true")
    p_run.add_argument("--skip-reset", action="store_true")
    p_run.add_argument("--keep-agent", action="store_true")
    p_run.add_argument("--stop-after", action="store_true")
    p_run.set_defaults(_func=cmd_run)

    return p


def main(argv=None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    return args._func(args)


if __name__ == "__main__":
    sys.exit(main())
