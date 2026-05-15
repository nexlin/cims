#!/usr/bin/env python3
"""
deployment/bin/verify.py — scenario.yaml 의 verify entry 자동 실행 (Phase 5).

USAGE
  ./verify.py --env <env_dir> --scenario <scn> [--phase listen|smoke|failover|all]
              [--smoke-name <n>] [--cspsim-dir <path>]

PHASES
  listen    : env.nodes 위에서 expected_listen 의 ip:port:proto 가 LISTEN 인지
  smoke     : verify.smoke 항목을 'at' 노드 (netns) 에서 실행 + expect regex 매칭
  failover  : verify.failover[].action 실행 → wait_sec → expect_vip_owner 확인
              → followup_smoke 재실행
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.stderr.write("PyYAML 필요: pip install pyyaml\n")
    sys.exit(2)


class VerifyError(Exception):
    """검증 인프라 자체 오류 (실패 ≠ 예외)."""


def _c(code: str, s: str) -> str:
    return f"\033[{code}m{s}\033[0m" if sys.stderr.isatty() else s


def _pass(s: str) -> str: return _c("32", s)
def _fail(s: str) -> str: return _c("31", s)
def _dim(s: str) -> str:  return _c("2", s)


# ─────────────────────────────────────────────────────────────
# 입력
# ─────────────────────────────────────────────────────────────

def _load_yaml(path: Path) -> dict:
    if not path.exists():
        raise VerifyError(f"파일 없음: {path}")
    with path.open("r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def _node_by_id(env: dict, node_id: str) -> dict:
    for n in env.get("nodes", []) or []:
        if n["id"] == node_id:
            return n
    raise VerifyError(f"node '{node_id}' 가 env.nodes 에 없음")


# ─────────────────────────────────────────────────────────────
# 실행 헬퍼
# ─────────────────────────────────────────────────────────────

_SUDO_PREFIX: list[str] | None = None   # 시작 시 1회 확정


def _detect_sudo() -> list[str] | None:
    """passwordless 또는 SUDO_ASKPASS 사용 가능한 sudo 옵션을 반환. 없으면 None."""
    if subprocess.run(["sudo", "-n", "true"], capture_output=True).returncode == 0:
        return ["sudo", "-n"]
    if os.environ.get("SUDO_ASKPASS"):
        return ["sudo", "-A"]
    return None


def _run(cmd: list[str], *, timeout: int = 60) -> tuple[int | None, str, str]:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return None, "", "timeout"


def _ns_run(netns: str, shell_cmd: str, *, as_user: str | None = None, timeout: int = 60):
    if not _SUDO_PREFIX:
        return None, "", "sudo unavailable"
    if as_user:
        cmd = [*_SUDO_PREFIX, "ip", "netns", "exec", netns,
               "sudo", "-u", as_user, "bash", "-c", shell_cmd]
    else:
        cmd = [*_SUDO_PREFIX, "ip", "netns", "exec", netns, "bash", "-c", shell_cmd]
    return _run(cmd, timeout=timeout)


# ─────────────────────────────────────────────────────────────
# Phase: expected_listen
# ─────────────────────────────────────────────────────────────

def _check_listen_one(env: dict, item: dict) -> tuple[bool, str]:
    node = _node_by_id(env, item["node"])
    netns = node.get("netns")
    if not netns:
        return False, f"node '{node['id']}' 에 netns 없음 (kind=netns 가 아님?)"

    proto = item["proto"].upper()
    ip = item["ip"]
    port = int(item["port"])

    flag = "-u" if proto == "UDP" else "-t"   # TLS 는 TCP 위
    rc, out, err = _ns_run(netns, f"ss -Hln {flag} sport = :{port}", timeout=5)
    if rc != 0:
        return False, f"ss 실패: {err.strip()[:80]}"

    target_a = f"{ip}:{port}"
    target_b = f"0.0.0.0:{port}"           # 0.0.0.0 bind 도 매칭
    target_c = f"*:{port}"
    for line in out.splitlines():
        if target_a in line or target_b in line or target_c in line:
            return True, line.strip()
    return False, f"미발견 ({target_a} {proto})"


def phase_listen(env: dict, scn: dict) -> tuple[bool, list]:
    items = (scn.get("verify") or {}).get("expected_listen") or []
    if not items:
        return True, []
    print(f"\n=== Phase: expected_listen ({len(items)} 항목) ===")
    results = []
    for it in items:
        ok, msg = _check_listen_one(env, it)
        tag = _pass("PASS") if ok else _fail("FAIL")
        line = f"  [{tag}] {it['node']:8s} {it['ip']:14s}:{it['port']:<6} {it['proto']:4s}"
        print(f"{line}  {_dim(msg[:100])}")
        results.append((ok, it, msg))
    return all(r[0] for r in results), results


# ─────────────────────────────────────────────────────────────
# Phase: smoke
# ─────────────────────────────────────────────────────────────

def _build_cspsim_cmd(args: dict) -> str:
    parts = ["./bin/cspsim"]
    for k, v in args.items():
        if isinstance(v, bool):
            if v:
                parts.append(f"-{k}")
        else:
            parts.append(f"-{k}")
            parts.append(str(v))
    return " ".join(shlex.quote(p) for p in parts)


def _run_smoke_one(env: dict, item: dict, *, cspsim_dir: str) -> tuple[bool, str, str]:
    at = item.get("at")
    if not at:
        return False, "", "smoke.at 누락"
    node = _node_by_id(env, at)
    netns = node.get("netns")
    if not netns:
        return False, "", f"node '{at}' netns 없음"

    cmd = item.get("cmd", "cspsim")
    args = item.get("args") or {}

    if cmd == "cspsim":
        cli = _build_cspsim_cmd(args)
        shell = f"cd {shlex.quote(cspsim_dir)} && {cli} 2>&1"
        rc, out, err = _ns_run(netns, shell, as_user="nex", timeout=item.get("timeout", 90))
    elif cmd == "ping":
        host = args.get("host") or args.get("target")
        if not host:
            return False, "", "ping.host 누락"
        rc, out, err = _ns_run(netns, f"ping -c 2 -W 1 {shlex.quote(host)}", timeout=10)
    elif cmd == "shell":
        rc, out, err = _ns_run(netns, args.get("script", ""), timeout=item.get("timeout", 60))
    else:
        return False, "", f"지원 안 함 cmd={cmd}"

    if rc is None:
        return False, out, "timeout"

    expect = item.get("expect")
    if expect:
        if not re.search(expect, out):
            return False, out, f"expect={expect!r} 매칭 실패"
    elif rc != 0:
        return False, out, f"rc={rc}"
    return True, out, ""


def phase_smoke(env: dict, scn: dict, *, only_name: str | None = None,
                cspsim_dir: str) -> tuple[bool, list]:
    items = (scn.get("verify") or {}).get("smoke") or []
    if only_name:
        items = [i for i in items if i.get("name") == only_name]
    if not items:
        if only_name:
            print(f"\n[warn] smoke '{only_name}' 항목 없음")
        return True, []
    print(f"\n=== Phase: smoke ({len(items)} 항목) ===")
    results = []
    for it in items:
        ok, out, msg = _run_smoke_one(env, it, cspsim_dir=cspsim_dir)
        tag = _pass("PASS") if ok else _fail("FAIL")
        print(f"  [{tag}] {it.get('name', '?'):25s} {_dim(msg[:100])}")
        if not ok and out:
            for line in out.splitlines()[-8:]:
                print(f"        {_dim(line[:110])}")
        results.append((ok, it, out, msg))
    return all(r[0] for r in results), results


# ─────────────────────────────────────────────────────────────
# Phase: failover
# ─────────────────────────────────────────────────────────────

def _vip_owner(env: dict, vip_ip: str) -> str | None:
    for node in env.get("nodes", []) or []:
        netns = node.get("netns")
        if not netns:
            continue
        rc, out, _ = _ns_run(netns, f"ip -4 addr show 2>/dev/null | grep -wF {shlex.quote(vip_ip)} || true", timeout=5)
        if rc == 0 and vip_ip in out:
            return node["id"]
    return None


def _scenario_vip(env: dict) -> str | None:
    for hg in env.get("ha_groups") or []:
        for vip in hg.get("vips") or []:
            if vip.get("net") == "svc":
                return vip.get("ip")
    return None


def phase_failover(env: dict, scn: dict, *, cspsim_dir: str) -> tuple[bool, list]:
    items = (scn.get("verify") or {}).get("failover") or []
    if not items:
        return True, []
    vip_ip = _scenario_vip(env)
    print(f"\n=== Phase: failover ({len(items)} 항목) — VIP={vip_ip} ===")
    results = []
    for it in items:
        name = it.get("name", "?")
        before = _vip_owner(env, vip_ip) if vip_ip else None
        action = it.get("action", "").strip()
        if action:
            rc, _, err = _run(["bash", "-c", action], timeout=30)
            if rc != 0:
                print(f"  [{_fail('FAIL')}] {name:20s} action 실패: {err.strip()[:120]}")
                results.append((False, it))
                continue
        time.sleep(it.get("wait_sec", 5))
        after = _vip_owner(env, vip_ip) if vip_ip else None
        expect_owner = it.get("expect_vip_owner")
        owner_ok = (after == expect_owner)
        tag = _pass("PASS") if owner_ok else _fail("FAIL")
        print(f"  [{tag}] {name:20s} VIP owner {before} → {after} (expect={expect_owner})")

        followup_ok = True
        fname = it.get("followup_smoke")
        if owner_ok and fname:
            print(f"        ↳ followup smoke '{fname}'")
            _, sm = phase_smoke(env, scn, only_name=fname, cspsim_dir=cspsim_dir)
            followup_ok = all(r[0] for r in sm)
        results.append((owner_ok and followup_ok, it))
    return all(r[0] for r in results), results


# ─────────────────────────────────────────────────────────────
# main
# ─────────────────────────────────────────────────────────────

def _default_cspsim_dir(env: dict) -> str:
    for node in env.get("nodes") or []:
        if node.get("role_hint", "").startswith("sim"):
            return f"/home/nex/work/cims/build/dist/netns-agents/{node['id']}/install/modules/cspsim/0.0.1/CSPSIM/cspsim"
    return "/home/nex/work/cims/build/dist/netns-agents/sim-a/install/modules/cspsim/0.0.1/CSPSIM/cspsim"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--env", required=True)
    p.add_argument("--scenario", required=True)
    p.add_argument("--phase", choices=["listen", "smoke", "failover", "all"], default="all")
    p.add_argument("--smoke-name", help="smoke 중 특정 항목만 실행")
    p.add_argument("--cspsim-dir", help="cspsim install dir (기본: sim 노드 자동 추론)")
    p.add_argument("--root", help="deployment/ 부모 (기본 자동)")
    args = p.parse_args()

    here = Path(__file__).resolve().parent
    root = Path(args.root) if args.root else here.parent
    try:
        env = _load_yaml(root / args.env / "env.yaml")
        scn = _load_yaml(root / args.env / "scenarios" / f"{args.scenario}.yaml")
    except VerifyError as e:
        sys.stderr.write(f"[error] {e}\n")
        return 2

    # sudo 가용성 점검 (netns exec 필요)
    global _SUDO_PREFIX
    _SUDO_PREFIX = _detect_sudo()
    if not _SUDO_PREFIX:
        sys.stderr.write(
            "[error] netns 진입에 sudo 가 필요합니다. 다음 중 하나로 준비하세요:\n"
            "  (1) sudo -v   # 비밀번호 입력 후 timestamp 갱신\n"
            "  (2) SUDO_ASKPASS=/path/to/askpass 환경변수\n"
            "  (3) passwordless sudo 설정\n"
        )
        return 2

    cspsim_dir = args.cspsim_dir or _default_cspsim_dir(env)

    overall = True
    try:
        if args.phase in ("listen", "all"):
            ok, _ = phase_listen(env, scn); overall = overall and ok
        if args.phase in ("smoke", "all"):
            ok, _ = phase_smoke(env, scn, only_name=args.smoke_name, cspsim_dir=cspsim_dir)
            overall = overall and ok
        if args.phase in ("failover", "all"):
            ok, _ = phase_failover(env, scn, cspsim_dir=cspsim_dir)
            overall = overall and ok
    except VerifyError as e:
        sys.stderr.write(f"\n[error] {e}\n")
        return 2

    print()
    print("=" * 60)
    print(f"OVERALL: {_pass('PASS') if overall else _fail('FAIL')}")
    return 0 if overall else 1


if __name__ == "__main__":
    sys.exit(main())
