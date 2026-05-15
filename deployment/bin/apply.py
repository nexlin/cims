#!/usr/bin/env python3
"""
deployment/bin/apply.py — render 산출 bundle 을 agent install dir 에 배포.

USAGE
  ./apply.py --env <env_dir> --scenario <scn> [--bundle <dir>] [--dry-run]
              [--base <netns-agents-dir>] [--no-render]

흐름:
  1. render.py 호출 → bundle 생성 (default: ./bundle/<env>__<scn>/) — --no-render 면 skip
  2. bundle/<node>/ 를 install dir 에 복사:
       <node>/csp.json       → <base>/<node>/install/modules/csp/<v>/CSP/csp/config/csp.json
       <node>/config/*.jsonl → <base>/<node>/install/modules/csp/<v>/CSP/config/*.jsonl
       <node>/user/*.json    → <base>/<node>/install/modules/csp/<v>/CSP/csp/user/*.json
       <node>/cmp.json       → <base>/<node>/install/modules/cmp/<v>/CMP/cmp/config/cmp.json
"""

from __future__ import annotations

import argparse
import json
import shutil
import ssl
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.stderr.write("PyYAML 필요\n")
    sys.exit(2)


DEFAULT_BASE = "/home/nex/work/cims/build/dist/netns-agents"
DEFAULT_VERSION = "0.0.1"


class ApplyError(Exception): pass


def _load_yaml(path: Path) -> dict:
    if not path.exists():
        raise ApplyError(f"파일 없음: {path}")
    with path.open() as f:
        return yaml.safe_load(f) or {}


def _ha_for_pkg(scn: dict, pkg: str) -> int | None:
    for dep in scn.get("deployments", []) or []:
        for p in dep.get("packages", []) or []:
            if p.get("name") == pkg:
                return dep.get("ha_group")
    return None


def _members(env: dict, hg_id) -> list[str]:
    for hg in env.get("ha_groups") or []:
        if hg.get("id") == hg_id or hg.get("name") == hg_id:
            return [m["node"] for m in hg.get("members") or []]
    return []


def _copy_dir(src: Path, dst: Path, *, dry_run: bool) -> list[tuple[str, str]]:
    moves = []
    if not src.exists():
        return moves
    for f in sorted(src.iterdir()):
        if f.is_file():
            moves.append((str(f), str(dst / f.name)))
            if not dry_run:
                dst.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, dst / f.name)
    return moves


def _apply_csp(node: str, bundle_node: Path, base: Path, version: str, *, dry_run: bool) -> list:
    csp_root = base / node / "install" / "modules" / "csp" / version / "CSP"
    plans: list[tuple[str, str]] = []

    # csp.json → CSP/csp/config/csp.json
    csp_json = bundle_node / "csp.json"
    if csp_json.exists():
        dst = csp_root / "csp" / "config" / "csp.json"
        plans.append((str(csp_json), str(dst)))
        if not dry_run:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(csp_json, dst)

    # config/*.jsonl → CSP/config/*.jsonl
    plans += _copy_dir(bundle_node / "config", csp_root / "config", dry_run=dry_run)

    # user/*.json → CSP/csp/user/*.json
    plans += _copy_dir(bundle_node / "user", csp_root / "csp" / "user", dry_run=dry_run)

    return plans


def _apply_cmp(node: str, bundle_node: Path, base: Path, version: str, *, dry_run: bool) -> list:
    cmp_root = base / node / "install" / "modules" / "cmp" / version / "CMP"
    plans = []
    cmp_json = bundle_node / "cmp.json"
    if cmp_json.exists():
        dst = cmp_root / "cmp" / "config" / "cmp.json"
        plans.append((str(cmp_json), str(dst)))
        if not dry_run:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(cmp_json, dst)
    return plans


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--env", required=True)
    p.add_argument("--scenario", required=True)
    p.add_argument("--bundle", help="bundle 디렉토리 (기본 ./bundle/<env>__<scn>/)")
    p.add_argument("--dry-run", action="store_true", help="복사 안 하고 plan 만 출력")
    p.add_argument("--no-render", action="store_true", help="render 단계 skip (기존 bundle 재사용)")
    p.add_argument("--base", default=DEFAULT_BASE, help=f"netns-agents 기본 경로 (default: {DEFAULT_BASE})")
    p.add_argument("--version", default=DEFAULT_VERSION, help=f"패키지 버전 (default: {DEFAULT_VERSION})")
    p.add_argument("--root", help="deployment/ 부모")
    p.add_argument("--restart", help="apply 후 CSC API 로 restart job 호출할 deployment_id 리스트 (콤마 구분, 예: 27,28,19,20)")
    p.add_argument("--csc-url", help="CSC API base URL (--restart 시 사용, 기본: env.csc.url)")
    args = p.parse_args()

    here = Path(__file__).resolve().parent
    root = Path(args.root) if args.root else here.parent
    env_path = root / args.env / "env.yaml"
    scn_path = root / args.env / "scenarios" / f"{args.scenario}.yaml"
    bundle = Path(args.bundle) if args.bundle else (Path.cwd() / "bundle" / f"{args.env}__{args.scenario}")
    base = Path(args.base)

    # 1) render
    if not args.no_render:
        render = here / "render.py"
        rc = subprocess.call([str(render), "--env", args.env, "--scenario", args.scenario,
                              "--out", str(bundle)])
        if rc != 0:
            sys.stderr.write("[error] render 실패\n")
            return rc

    if not bundle.exists():
        sys.stderr.write(f"[error] bundle 디렉토리 없음: {bundle} — --no-render 이면 --bundle 지정\n")
        return 2

    # 2) env/scenario 로드해서 ha_group → 노드 매핑
    try:
        env = _load_yaml(env_path)
        scn = _load_yaml(scn_path)
    except ApplyError as e:
        sys.stderr.write(f"[error] {e}\n")
        return 2

    csp_hg = _ha_for_pkg(scn, "csp")
    cmp_hg = _ha_for_pkg(scn, "cmp")
    csp_nodes = _members(env, csp_hg) if csp_hg is not None else []
    cmp_nodes = _members(env, cmp_hg) if cmp_hg is not None else []

    mode = "[dry-run]" if args.dry_run else "[apply]"
    print(f"{mode} bundle={bundle}")
    print(f"{mode} base={base}")
    print(f"{mode} csp_nodes={csp_nodes} cmp_nodes={cmp_nodes}")

    all_plans: list[tuple[str, str]] = []
    for node in csp_nodes:
        plans = _apply_csp(node, bundle / node, base, args.version, dry_run=args.dry_run)
        all_plans += plans
    for node in cmp_nodes:
        plans = _apply_cmp(node, bundle / node, base, args.version, dry_run=args.dry_run)
        all_plans += plans

    for src, dst in all_plans:
        prefix = "  →" if not args.dry_run else "  plan:"
        # bundle/<node>/<rest> 만 표기
        try:
            short_src = str(Path(src).relative_to(bundle.parent))
        except ValueError:
            short_src = src
        try:
            short_dst = str(Path(dst).relative_to(base.parent))
        except ValueError:
            short_dst = dst
        print(f"{prefix} {short_src}  →  {short_dst}")

    print(f"\n{mode} {len(all_plans)} 파일 {'(미적용)' if args.dry_run else '적용 완료'}")

    # --restart 옵션: CSC API 로 자동 restart job POST
    if not args.dry_run and args.restart:
        csc_url = args.csc_url or (env.get("csc") or {}).get("url") or "https://127.0.0.1:4419"
        dep_ids = [int(x.strip()) for x in args.restart.split(",") if x.strip()]
        print(f"\n[restart] CSC={csc_url} deployments={dep_ids}")
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        for dep in dep_ids:
            url = f"{csc_url.rstrip('/')}/api/v1/deployments/{dep}/job"
            req = urllib.request.Request(
                url,
                data=json.dumps({"job_type": "restart"}).encode(),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            try:
                with urllib.request.urlopen(req, context=ctx, timeout=15) as r:
                    body = r.read().decode("utf-8", errors="replace")[:200]
                    print(f"  dep={dep} HTTP {r.status} {body}")
            except urllib.error.HTTPError as e:
                print(f"  dep={dep} HTTP {e.code} {e.read().decode('utf-8', errors='replace')[:200]}")
            except Exception as e:
                print(f"  dep={dep} FAIL: {e}")
    elif not args.dry_run:
        print("  ※ csp/cmp 재시작 필요 시: --restart <dep_id,...> 옵션 또는")
        print("    curl -X POST .../api/v1/deployments/<n>/job -d '{\"job_type\":\"restart\"}'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
