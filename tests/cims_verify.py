#!/usr/bin/env python3
"""CIMS 검증 도구 CLI — verify.lib 의 진입점.

명령:
  list [--stage N] [--json]                 — 등록 항목 트리 출력
  list-presets [--json]                     — 프리셋 목록
  describe ITEM_ID [--json]                 — 항목 메타 상세
  run [--stage N] [--items ID,...] [--preset NAME]
       [--json] [--report-dir PATH]
       [--skip-build] [--skip-pkg] [--skip-reset] [--keep-agent]
                                            — 선택 항목 실행

사용 예:
  python3 -m tests.cims_verify list --stage 6 --json
  python3 -m tests.cims_verify run --stage 3
  python3 -m tests.cims_verify run --items S6-ENTRY-CHECK,S6-SEED --json
  python3 -m tests.cims_verify run --preset pipeline-full
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
_REPO_ROOT = os.path.dirname(_THIS_DIR)
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from verify.lib import (                                        # noqa: E402
    registry, runner, reporting, presets as preset_mod, run_store,
    webhook as _webhook,
)
from verify.lib.context import VerifyContext                    # noqa: E402
from verify.lib import items as _items_pkg                      # noqa: F401, E402  (auto-import 트리거)


def _flatten_items_for_record(results: list) -> list:
    """ItemResult list (children 포함) → run_store record items[] (idx 부여)."""
    flat: list = []
    idx = 0
    for r in results:
        idx += 1
        flat.append({
            "id":         (r.id or "")[:64],
            "stage":      int(r.stage or 0),
            "parent_id":  None,
            "is_group":   bool(r.children),
            "name":       (r.name or "")[:255],
            "status":     (r.status or "UNKNOWN")[:16],
            "elapsed_ms": int(r.elapsed_ms or 0),
            "detail":     (r.detail or "")[:2000],
            "idx":        idx,
        })
        for c in (r.children or []):
            idx += 1
            flat.append({
                "id":         (c.id or "")[:64],
                "stage":      int(r.stage or 0),
                "parent_id":  (r.id or "")[:64],
                "is_group":   False,
                "name":       (c.name or "")[:255],
                "status":     (c.status or "UNKNOWN")[:16],
                "elapsed_ms": int(c.elapsed_ms or 0),
                "detail":     (c.detail or "")[:2000],
                "idx":        idx,
            })
    return flat


def _resolve_pkg_manifest_hash(repo_root: str) -> str:
    """build/dist/packages/manifest.json 의 sha256 (S6 immutability gate SoT)."""
    p = os.path.join(repo_root, "build", "dist", "packages", "manifest.json")
    if not os.path.isfile(p):
        return ""
    import hashlib
    h = hashlib.sha256()
    try:
        with open(p, "rb") as f:
            for chunk in iter(lambda: f.read(64 * 1024), b""):
                h.update(chunk)
        return h.hexdigest()
    except Exception:
        return ""


def _record_cli_run(args, ctx, item_ids: list, results: list,
                    verdict: str, totals: dict, elapsed: float, stage: int) -> int:
    """CLI 실행 결과를 verify_runs/ 에 기록. backend `_record_run` 와 동등 schema.

    트리거 type 은 'cli' — backend 의 비동기 job 'user'/'ci' 와 구분. 반환 = 회차 id.
    """
    import socket
    from datetime import datetime
    started_iso  = datetime.fromtimestamp(time.time() - elapsed).isoformat(timespec="milliseconds")
    finished_iso = datetime.fromtimestamp(time.time()).isoformat(timespec="milliseconds")

    if stage:
        scope = f"stage{stage}"
    elif args.preset:
        scope = f"preset:{args.preset}"
    elif args.items:
        scope = "items"
    else:
        scope = "multi"

    record = {
        "id":                0,
        "started_at":        started_iso,
        "finished_at":       finished_iso,
        "elapsed_ms":        int(elapsed * 1000),
        "trigger":           "cli",
        "scope":             scope[:64],
        "selected_ids":      list(item_ids),
        "verdict":           verdict if verdict in ("PASS", "FAIL") else "UNKNOWN",
        "totals":            {
            "total":   int(totals.get("total", 0)),
            "pass":    int(totals.get("pass", 0)),
            "fail":    int(totals.get("fail", 0)),
            "skip":    int(totals.get("skip", 0)),
            "blocked": int(totals.get("blocked", 0)),
        },
        "pkg_manifest_hash": _resolve_pkg_manifest_hash(ctx.repo_root)[:128],
        "git_branch":        (ctx.git_branch or "")[:255],
        "git_sha":           (ctx.git_sha or "")[:40],
        "host":              socket.gethostname()[:64],
        "ens_ip":            ctx.ens_ip or "",
        "report_path":       (ctx.report_path or "")[:1000],
        "job_id":            "",
        "note":              "",
        "items":             _flatten_items_for_record(results),
    }
    rid = run_store.write_run(ctx.repo_root, record)
    record["id"] = rid
    # webhook 발송 — env CIMS_VERIFY_WEBHOOK_URL 설정 시. 실패 silently.
    try:
        _webhook.publish(record)
    except Exception:
        pass
    return rid


_VALID_STAGES = (1, 2, 3, 4, 5, 6)


def _repo_root_from_here() -> str:
    """tests/cims_verify.py → repo root."""
    return _REPO_ROOT


# ─────────────────────────────────────────────────────────────
# 명령 핸들러
# ─────────────────────────────────────────────────────────────
def cmd_list(args: argparse.Namespace) -> int:
    if args.stage:
        # stage 단독 — include_children=True 로 부모/자식 모두 표시
        metas = registry.get_items(stage=args.stage, include_children=True)
    else:
        metas = registry.get_all_metas()
        # get_all_metas 는 미정렬 — registry _sort_key 와 동일하게 정렬
        metas = sorted(metas, key=registry._sort_key)
    if args.json:
        out = {
            "stages": [],
            "presets": preset_mod.list_presets(),
        }
        by_stage: dict = {}
        for m in metas:
            by_stage.setdefault(m.stage, []).append(m.to_dict())
        for s in sorted(by_stage):
            out["stages"].append({"stage": s, "items": by_stage[s]})
        print(json.dumps(out, ensure_ascii=False, indent=2))
    else:
        if not metas:
            print("(등록된 항목 없음)"); return 0
        cur = None
        for m in metas:
            if m.stage != cur:
                cur = m.stage
                print(f"\n[Stage {cur}]")
            cats = f"[{m.category}]" if m.category else ""
            mark = "▼" if m.is_group else (" └" if m.parent else "  ")
            print(f"  {mark} {m.id:36} {cats:8} {m.name}")
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


def cmd_purge_runs(args: argparse.Namespace) -> int:
    """오래된 회차 파일 정리 — verify_runs/YYYY/MM/<id>.json 중 days 일 초과
    삭제. keep_min 으로 최근 N 개는 무조건 보존 (사고 방지).
    """
    repo_root = args.repo_root or _repo_root_from_here()
    days = max(0, int(args.days))
    keep_min = max(0, int(args.keep_min))
    if not args.force and days < 1:
        print(f"--days={days} 는 모든 회차 삭제 — --force 필요", file=sys.stderr)
        return 2
    summary = run_store.purge_older_than(repo_root, days, keep_min=keep_min)
    if args.json:
        print(json.dumps({
            "deleted":      len(summary["deleted"]),
            "kept":         summary["kept"],
            "freed_bytes":  summary["freed_bytes"],
            "removed_dirs": summary["removed_dirs"],
        }, ensure_ascii=False, indent=2))
    else:
        kb = summary["freed_bytes"] / 1024.0
        print(f"삭제: {len(summary['deleted'])} 회차 ({kb:.1f} KB)")
        print(f"보존: {summary['kept']} 회차 (days<{days} or keep_min={keep_min})")
        if summary["removed_dirs"]:
            print(f"빈 디렉토리 정리: {len(summary['removed_dirs'])}")
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
        print(f"Stage:        {meta.stage}")
        print(f"Category:     {meta.category}")
        print(f"Name:         {meta.name}")
        if meta.is_group: print(f"Group:        yes")
        if meta.parent:   print(f"Parent:       {meta.parent}")
        print(f"Depends on:   {', '.join(meta.depends_on) or '-'}")
        print(f"Presets:      {', '.join(meta.presets) or '-'}")
        print(f"Side effects: {', '.join(meta.side_effects) or '-'}")
        print(f"Timeout:      {meta.timeout_s}s")
        if meta.description: print(f"Description:  {meta.description}")
    return 0


def _parse_only_children(raw) -> dict:
    """--only-children 인자(list[str] 또는 None) → {parent_id: [child_id]} 매핑.

    각 항목이 JSON 객체면 그대로 merge, "PARENT=A,B,C" 형식이면 split.
    빈 입력은 {} 반환.
    """
    if not raw:
        return {}
    out: dict = {}
    for spec in raw:
        s = (spec or "").strip()
        if not s:
            continue
        if s.startswith("{"):
            try:
                obj = json.loads(s)
            except Exception as e:
                print(f"--only-children JSON 파싱 실패: {e}", file=sys.stderr); sys.exit(2)
            if not isinstance(obj, dict):
                print(f"--only-children JSON 은 object 여야 함: {s}", file=sys.stderr); sys.exit(2)
            for k, v in obj.items():
                ids = v if isinstance(v, list) else [v]
                out.setdefault(k, []).extend(str(x) for x in ids if x)
        elif "=" in s:
            parent, _, rest = s.partition("=")
            parent = parent.strip()
            ids = [x.strip() for x in rest.split(",") if x.strip()]
            if parent and ids:
                out.setdefault(parent, []).extend(ids)
        else:
            print(f"--only-children 형식 오류 (PARENT=CHILD1,CHILD2 또는 JSON): {s}",
                  file=sys.stderr); sys.exit(2)
    return out


def _resolve_run_selection(args: argparse.Namespace) -> tuple:
    """args → (item_ids, stage). stage 는 리포트 파일명용.

    여러 stage 의 항목이 섞이면 stage=0 ('multi-stage' 리포트).
    """
    items = []
    if args.items:
        items = [s.strip() for s in args.items.split(",") if s.strip()]
        stages = {registry.get_item(i)[0].stage for i in items if registry.get_item(i)}
        stage = stages.pop() if len(stages) == 1 else 0
    elif args.preset:
        items = preset_mod.resolve_preset(args.preset)
        if not items:
            print(f"프리셋 없음: {args.preset}", file=sys.stderr); sys.exit(2)
        stages = {registry.get_item(i)[0].stage for i in items if registry.get_item(i)}
        stage = stages.pop() if len(stages) == 1 else 0
    elif args.stage:
        stage = args.stage
        items = [m.id for m in registry.get_items(stage=stage, include_children=False)]
        if not items:
            print(f"Stage {stage} 에 등록된 항목 없음", file=sys.stderr); sys.exit(2)
    else:
        print("--stage / --items / --preset 중 하나 지정 필요", file=sys.stderr); sys.exit(2)
    return (items, stage)


def cmd_run(args: argparse.Namespace) -> int:
    item_ids, stage = _resolve_run_selection(args)

    repo_root = args.repo_root or _repo_root_from_here()
    opts = {
        "skip_build": bool(args.skip_build),
        "skip_pkg":   bool(args.skip_pkg),
        "skip_reset": bool(args.skip_reset),
        "keep_agent": bool(args.keep_agent),
        "stop_after": bool(args.stop_after),
    }
    only_children = _parse_only_children(args.only_children)
    if only_children:
        opts["only_children"] = only_children
    # --inject-fail — 디버그용 강제 FAIL ID set
    inject = set()
    for spec in (args.inject_fail or []):
        for tok in (spec or "").split(","):
            tok = tok.strip()
            if tok:
                inject.add(tok)
    if inject:
        opts["inject_fail"] = inject
    ctx = VerifyContext.create(repo_root=repo_root, stage=stage, opts=opts,
                               report_dir=args.report_dir)

    scope = (f"Stage {stage} 전체" if stage and not args.items else
             f"선택 {len(item_ids)} 항목 (stage={stage or 'multi'})")
    reporting.write_header(ctx, scope=scope)

    t0 = time.time()
    results = runner.run_items(ctx, item_ids)
    elapsed = time.time() - t0

    verdict = reporting.determine_verdict(results)
    totals = reporting.write_summary(ctx, results, verdict)
    ctx.report_close()

    # 회차 이력 자동 기록 (verify_runs/YYYY/MM/<id>.json) — --no-record 로 비활성.
    run_id = 0
    if not args.no_record:
        try:
            run_id = _record_cli_run(args, ctx, item_ids, results, verdict,
                                     totals, elapsed, stage)
        except Exception as e:
            # 기록 실패는 검증 결과에 영향 X — stderr 경고만.
            print(f"[WARN] verify_runs 기록 실패: {type(e).__name__}: {e}",
                  file=sys.stderr)

    payload = {
        "started_at": ctx.ts,
        "elapsed_s": round(elapsed, 2),
        "selected": item_ids,
        "results": [r.to_dict() for r in results],
        "totals": totals,
        "report_path": ctx.report_path,
        "verdict": verdict,
        "stage": stage,
        "run_id": run_id,
    }
    if args.json:
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        label = f"Stage {stage}" if stage else "multi-stage"
        print(f"\n=== {label} 검증 종료 — 판정: {verdict} ===")
        print(f"리포트: {ctx.report_path}")
        for r in results:
            mark = {"PASS": "✓", "FAIL": "✗", "SKIP": "·",
                    "BLOCKED": "◇"}.get(r.status, "?")
            print(f"  {mark} {r.id:32} {r.status:7}  {r.name}")
        line = (f"\n총 {totals['total']} / PASS {totals['pass']} / "
                f"FAIL {totals['fail']} / SKIP {totals['skip']}")
        if totals.get("blocked"):
            line += f" / BLOCKED {totals['blocked']}"
        print(line)
        print(f"소요: {elapsed:.1f}s")

    return 0 if verdict == "PASS" else 1


# ─────────────────────────────────────────────────────────────
# argparse
# ─────────────────────────────────────────────────────────────
def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="cims_verify",
        description="CIMS 검증 도구 — 항목 단위 메타/실행 (S1~S6 6단계 파이프라인)",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    p_list = sub.add_parser("list", help="등록 항목 트리 출력")
    p_list.add_argument("--stage", type=int, choices=_VALID_STAGES)
    p_list.add_argument("--json", action="store_true")
    p_list.set_defaults(_func=cmd_list)

    p_lp = sub.add_parser("list-presets", help="프리셋 목록")
    p_lp.add_argument("--json", action="store_true")
    p_lp.set_defaults(_func=cmd_list_presets)

    p_purge = sub.add_parser(
        "purge-runs",
        help="verify_runs/ 의 오래된 회차 파일 정리 (retention)",
    )
    p_purge.add_argument(
        "--days", type=int, default=90,
        help="이 일수보다 오래된 회차 삭제 (default: 90)",
    )
    p_purge.add_argument(
        "--keep-min", type=int, default=10,
        help="오래되어도 최근 N 회차는 무조건 보존 (default: 10)",
    )
    p_purge.add_argument(
        "--force", action="store_true",
        help="--days 0 (모두 삭제) 허용",
    )
    p_purge.add_argument("--json", action="store_true")
    p_purge.add_argument("--repo-root", help="cims repo root")
    p_purge.set_defaults(_func=cmd_purge_runs)

    p_desc = sub.add_parser("describe", help="항목 메타 상세")
    p_desc.add_argument("item_id")
    p_desc.add_argument("--json", action="store_true")
    p_desc.set_defaults(_func=cmd_describe)

    p_run = sub.add_parser("run", help="선택 항목 실행")
    p_run.add_argument("--stage", type=int, choices=_VALID_STAGES)
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
    p_run.add_argument(
        "--only-children",
        action="append", default=None,
        help=(
            "부모 항목 자식 ID 필터. 형식: PARENT=CHILD1,CHILD2 "
            "(예: --only-children S5-CSC-DEPLOY=S5-CSC-DEPLOY-INSTALL). "
            "JSON 도 가능: --only-children '{\"S5-CSC-DEPLOY\":[\"...\"]}'. "
            "여러 번 지정 가능."
        ),
    )
    p_run.add_argument(
        "--inject-fail",
        action="append", default=None,
        metavar="ITEM_ID",
        help=(
            "디버그용 강제 FAIL 주입. 지정한 ID 항목은 함수 호출 없이 FAIL 반환. "
            "stage gate / immutability gate 회귀 점검 시 사용. "
            "comma-separated 다중 지정 또는 옵션 반복 가능 "
            "(예: --inject-fail S1-CPP-FORMAT,S2-PREFLIGHT)."
        ),
    )
    p_run.add_argument(
        "--no-record",
        action="store_true",
        help="회차 결과를 verify_runs/ 에 기록하지 않음 (default: 기록함).",
    )
    p_run.set_defaults(_func=cmd_run)

    return p


def main(argv=None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    return args._func(args)


if __name__ == "__main__":
    sys.exit(main())
