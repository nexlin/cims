#!/usr/bin/env python3
"""
deployment/bin/apply.py — render 산출 bundle 을 agent install dir 에 배포.

USAGE
  ./apply.py --env <env_dir> --scenario <scn> [--bundle <dir>] [--dry-run]
              [--base <install-base>] [--no-render]

흐름:
  1. render.py 호출 → bundle 생성 (default: ./bundle/<env>__<scn>/) — --no-render 면 skip
  2. bundle/<node>/ 를 install dir 에 복사 (single-host/multi-host 공통, <base> 기본 build/dist):
       <node>/csp.json       → <base>/csp/config/csp.json
       <node>/config/*.jsonl → <base>/csp/config/*.jsonl
       <node>/user/*.json    → <base>/csp/user/*.json
       <node>/cmp.json       → <base>/cmp/config/cmp.json
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


DEFAULT_BASE = "/home/nex/work/cims/build/dist"
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


def _backup(dst: Path) -> None:
    """기존 파일을 .bak 으로 백업. 이미 .bak 가 있어도 덮어씀 (마지막 1회만 보관)."""
    if dst.exists() and dst.is_file():
        shutil.copy2(dst, dst.with_suffix(dst.suffix + ".bak"))


def _copy_dir(src: Path, dst: Path, *, dry_run: bool, do_backup: bool) -> list[tuple[str, str]]:
    moves = []
    if not src.exists():
        return moves
    for f in sorted(src.iterdir()):
        if f.is_file():
            target = dst / f.name
            moves.append((str(f), str(target)))
            if not dry_run:
                dst.mkdir(parents=True, exist_ok=True)
                if do_backup: _backup(target)
                shutil.copy2(f, target)
    return moves


def _csp_root(env: dict, base: Path, node: str, version: str) -> Path:
    """csp install 경로 — single-host/multi-host 공통 <base>/csp.

    (env/node/version 인자는 호출부 시그니처 정합 목적으로 유지.)
    """
    return base / "csp"


def _cmp_root(env: dict, base: Path, node: str, version: str) -> Path:
    """cmp install 경로 — single-host/multi-host 공통 <base>/cmp."""
    return base / "cmp"


def _csp_paths(root: Path) -> tuple[Path, Path, Path]:
    """csp install 의 (csp.json, config_dir, user_dir) 위치 — single/multi-host 공통.

    csp/config/csp.json, csp/config/, csp/user/
    (dev 모드는 jsonlDir 가 csp/config/ 라고 가정 — config_template.json 의 ConfigJsonlDir 와 일치)
    """
    return (root / "config" / "csp.json",
            root / "config",
            root / "user")


def _cmp_json_path(root: Path) -> Path:
    return root / "config" / "cmp.json"


def _apply_csp(env: dict, node: str, bundle_node: Path, base: Path, version: str,
                *, dry_run: bool, do_backup: bool) -> list:
    csp_root = _csp_root(env, base, node, version)
    csp_json_dst, config_dst, user_dst = _csp_paths(csp_root)
    plans: list[tuple[str, str]] = []

    csp_json = bundle_node / "csp.json"
    if csp_json.exists():
        plans.append((str(csp_json), str(csp_json_dst)))
        if not dry_run:
            csp_json_dst.parent.mkdir(parents=True, exist_ok=True)
            if do_backup: _backup(csp_json_dst)
            shutil.copy2(csp_json, csp_json_dst)

    plans += _copy_dir(bundle_node / "config", config_dst, dry_run=dry_run, do_backup=do_backup)
    plans += _copy_dir(bundle_node / "user",   user_dst,   dry_run=dry_run, do_backup=do_backup)
    return plans


def _apply_cmp(env: dict, node: str, bundle_node: Path, base: Path, version: str,
                *, dry_run: bool, do_backup: bool) -> list:
    cmp_root = _cmp_root(env, base, node, version)
    dst = _cmp_json_path(cmp_root)
    plans = []
    cmp_json = bundle_node / "cmp.json"
    if cmp_json.exists():
        plans.append((str(cmp_json), str(dst)))
        if not dry_run:
            dst.parent.mkdir(parents=True, exist_ok=True)
            if do_backup: _backup(dst)
            shutil.copy2(cmp_json, dst)
    return plans


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--env", required=True)
    p.add_argument("--scenario", required=True)
    p.add_argument("--bundle", help="bundle 디렉토리 (기본 ./bundle/<env>__<scn>/)")
    p.add_argument("--dry-run", action="store_true", help="복사 안 하고 plan 만 출력")
    p.add_argument("--no-render", action="store_true", help="render 단계 skip (기존 bundle 재사용)")
    p.add_argument("--base", help=f"install base 경로 (single-host/multi-host 공통, default {DEFAULT_BASE})")
    p.add_argument("--version", default=DEFAULT_VERSION, help=f"패키지 버전 (default: {DEFAULT_VERSION})")
    p.add_argument("--root", help="deployment/ 부모")
    p.add_argument("--backup", action="store_true", help="기존 파일 .bak 으로 백업 후 복사 (마지막 1회분만)")
    p.add_argument("--restore", action="store_true", help="마지막 백업 (.bak) 을 원본으로 복원. apply 안 함")
    p.add_argument("--restart", help="apply 후 restart 호출: 콤마 구분 deployment_id (예: 27,28) 또는 'auto' (CSC API GET 후 자동 매핑)")
    p.add_argument("--skip-restart-if-no-change", action="store_true", help="--backup 과 함께. apply 후 .bak 과 비교하여 변경 0 이면 restart skip")
    p.add_argument("--verify", action="store_true", help="apply (+ restart) 후 verify.py --phase listen 자동 실행")
    p.add_argument("--csc-url", help="CSC API base URL (--restart 시 사용, 기본: env.csc.url)")
    args = p.parse_args()

    here = Path(__file__).resolve().parent
    root = Path(args.root) if args.root else here.parent
    env_path = root / args.env / "env.yaml"
    scn_path = root / args.env / "scenarios" / f"{args.scenario}.yaml"
    bundle = Path(args.bundle) if args.bundle else (Path.cwd() / "bundle" / f"{args.env}__{args.scenario}")

    # --restore: .bak 파일을 원본으로 복원하고 종료
    if args.restore:
        try:
            env = _load_yaml(env_path)
            scn = _load_yaml(scn_path)
        except ApplyError as e:
            sys.stderr.write(f"[error] {e}\n")
            return 2
        return _do_restore(env, scn, args)

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

    # base 결정 (single-host/multi-host 공통 — build/dist 직접 사용)
    kind = (env.get("kind") or "").lower()   # 정보 출력용 (single-host|multi-host)
    base = Path(args.base) if args.base else Path(DEFAULT_BASE)

    csp_hg = _ha_for_pkg(scn, "csp")
    cmp_hg = _ha_for_pkg(scn, "cmp")
    csp_nodes = _members(env, csp_hg) if csp_hg is not None else []
    cmp_nodes = _members(env, cmp_hg) if cmp_hg is not None else []

    mode = "[dry-run]" if args.dry_run else "[apply]"
    print(f"{mode} bundle={bundle}")
    print(f"{mode} base={base}  kind={kind or 'unknown'}")
    print(f"{mode} csp_nodes={csp_nodes} cmp_nodes={cmp_nodes}")

    all_plans: list[tuple[str, str]] = []
    for node in csp_nodes:
        plans = _apply_csp(env, node, bundle / node, base, args.version,
                            dry_run=args.dry_run, do_backup=args.backup)
        all_plans += plans
    for node in cmp_nodes:
        plans = _apply_cmp(env, node, bundle / node, base, args.version,
                            dry_run=args.dry_run, do_backup=args.backup)
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

    # --skip-restart-if-no-change: .bak 과 비교하여 변경된 파일 0 이면 restart skip
    if not args.dry_run and args.skip_restart_if_no_change and args.restart:
        if not args.backup:
            sys.stderr.write("[warn] --skip-restart-if-no-change 는 --backup 필요 — 항상 restart 진행\n")
        else:
            changed = 0
            for _, dst_str in all_plans:
                dst = Path(dst_str)
                bak = dst.with_suffix(dst.suffix + ".bak")
                if not bak.exists():
                    changed += 1   # 신규 파일
                    continue
                try:
                    if bak.read_bytes() != dst.read_bytes():
                        changed += 1
                except Exception:
                    changed += 1   # 비교 실패 시 안전쪽으로 restart
            print(f"\n[change-detect] 변경 파일 {changed}/{len(all_plans)}")
            if changed == 0:
                print("[restart] 변경 없음 — restart skip")
                args.restart = None

    # --restart 옵션: CSC API 로 자동 restart job POST
    if not args.dry_run and args.restart:
        csc_url = args.csc_url or (env.get("csc") or {}).get("url") or "https://127.0.0.1:4419"
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

        # (dep_id, job_type) 튜플 리스트. status 'running' → restart, 외 → start
        jobs: list[tuple[int, str]] = []
        if args.restart.strip().lower() == "auto":
            jobs = _resolve_auto_jobs(env, scn, csp_nodes, cmp_nodes, csc_url, ctx)
            if not jobs:
                print("\n[restart] auto-resolve 결과 매칭된 deployment 없음 — restart skip")
                return 0
            print(f"\n[restart] CSC={csc_url} (auto, status-aware) jobs={jobs}")
        else:
            # 명시 id 리스트 — 모두 restart 로 처리 (옛 호환)
            jobs = [(int(x.strip()), "restart") for x in args.restart.split(",") if x.strip()]
            print(f"\n[restart] CSC={csc_url} deployments={[j[0] for j in jobs]}")

        for dep, job_type in jobs:
            url = f"{csc_url.rstrip('/')}/api/v1/deployments/{dep}/job"
            req = urllib.request.Request(
                url,
                data=json.dumps({"job_type": job_type}).encode(),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            try:
                with urllib.request.urlopen(req, context=ctx, timeout=15) as r:
                    body = r.read().decode("utf-8", errors="replace")[:200]
                    print(f"  dep={dep} ({job_type}) HTTP {r.status} {body}")
            except urllib.error.HTTPError as e:
                print(f"  dep={dep} HTTP {e.code} {e.read().decode('utf-8', errors='replace')[:200]}")
            except Exception as e:
                print(f"  dep={dep} FAIL: {e}")
    elif not args.dry_run:
        print("  ※ csp/cmp 재시작 필요 시: --restart auto 또는 --restart <id,...>")

    # --verify: apply + restart 후 verify.py --phase listen 자동 실행
    if not args.dry_run and args.verify:
        # restart 가 트리거됐으면 csp/cmp 가 새로 뜰 시간 확보
        if args.restart:
            print("\n[verify] restart 후 csp/cmp 기동 대기 (6s)...")
            import time
            time.sleep(6)
        verify_bin = here / "verify.py"
        print(f"[verify] ./bin/verify.py --env {args.env} --scenario {args.scenario} --phase listen")
        rc = subprocess.call([str(verify_bin), "--env", args.env, "--scenario", args.scenario, "--phase", "listen"])
        return rc
    return 0


def _do_restore(env: dict, scn: dict, args) -> int:
    """모든 install dir 의 .bak 파일을 원본으로 복원."""
    base = Path(args.base) if args.base else Path(DEFAULT_BASE)

    csp_hg = _ha_for_pkg(scn, "csp")
    cmp_hg = _ha_for_pkg(scn, "cmp")
    csp_nodes = _members(env, csp_hg) if csp_hg is not None else []
    cmp_nodes = _members(env, cmp_hg) if cmp_hg is not None else []

    restored: list[str] = []
    for node in csp_nodes:
        csp_root = _csp_root(env, base, node, args.version)
        csp_json_dst, config_dst, user_dst = _csp_paths(csp_root)
        for target_dir in [config_dst, user_dst]:
            if not target_dir.exists(): continue
            for bak in target_dir.glob("*.bak"):
                orig = bak.with_suffix("")
                shutil.copy2(bak, orig)
                restored.append(str(orig))
        if csp_json_dst.with_suffix(csp_json_dst.suffix + ".bak").exists():
            shutil.copy2(csp_json_dst.with_suffix(csp_json_dst.suffix + ".bak"), csp_json_dst)
            restored.append(str(csp_json_dst))

    for node in cmp_nodes:
        cmp_root = _cmp_root(env, base, node, args.version)
        dst = _cmp_json_path(cmp_root)
        bak = dst.with_suffix(dst.suffix + ".bak")
        if bak.exists():
            shutil.copy2(bak, dst)
            restored.append(str(dst))

    print(f"[restore] {len(restored)} 파일 복원 완료")
    for r in restored:
        try:
            short = str(Path(r).relative_to(base.parent))
        except ValueError:
            short = r
        print(f"  ← {short}")
    if not restored:
        print("  (백업 파일 없음 — apply --backup 한 적 없거나 이미 복원됨)")
    return 0


def _resolve_auto_jobs(env: dict, scn: dict, csp_nodes: list[str],
                        cmp_nodes: list[str], csc_url: str, ctx) -> list[tuple[int, str]]:
    """env+scenario 의 (agent_id, package_name) → [(deployment.id, job_type)].

    job_type: status='running' → 'restart' (re-bind config), 외 → 'start' (깨우기).
    csp/cmp 만 대상. cspsim 같은 non-daemon 은 제외.
    """
    list_url = f"{csc_url.rstrip('/')}/api/v1/deployments"
    try:
        with urllib.request.urlopen(list_url, context=ctx, timeout=15) as r:
            payload = json.loads(r.read().decode("utf-8"))
    except Exception as e:
        sys.stderr.write(f"[error] CSC GET /deployments 실패: {e}\n")
        return []
    items = payload.get("items") or []

    aid_by_node = {n["id"]: n.get("agent_id") for n in env.get("nodes", []) or []}

    targets: list[tuple[int, str]] = []
    for nid in csp_nodes:
        aid = aid_by_node.get(nid)
        if aid is not None:
            targets.append((aid, "csp"))
    for nid in cmp_nodes:
        aid = aid_by_node.get(nid)
        if aid is not None:
            targets.append((aid, "cmp"))

    jobs: list[tuple[int, str]] = []
    for aid, pkg in targets:
        matches = [
            it for it in items
            if it.get("agent_id") == aid and (it.get("package_name") or "").lower() == pkg
        ]
        if not matches:
            sys.stderr.write(f"[warn] agent_id={aid} package={pkg} deployment 미발견 — skip\n")
            continue
        if len(matches) > 1:
            sys.stderr.write(f"[warn] agent_id={aid} package={pkg} 중복 deployment — 첫 번째만\n")
        d = matches[0]
        status = (d.get("status") or "").lower()
        job_type = "restart" if status == "running" else "start"
        if status not in ("running", "stopped", "installed", "failed"):
            sys.stderr.write(f"[warn] dep={d['id']} status={status!r} (예상 외) — start 시도\n")
        jobs.append((d["id"], job_type))
    return jobs


if __name__ == "__main__":
    sys.exit(main())
