"""S2-PREFLIGHT — gate 정책 (Python 자체 검사).

`cims.sh preflight` 는 정보성 CLI (warn 만). 검증 파이프라인용 BLOCK 정책은
verify lib 에 격리해서 직접 검사.

BLOCK 조건:
1. local_ip 미결정 (env CIMS_LOCAL_IP > .cims/server.local.json > default route)
2. DB 연결 실패 또는 pymysql 미설치
3. TB 3종 (4419 TB-CSC / 3000 TB-Console / 9902 TB-agent) 미동작
4. 검증 대상 포트 점유 — 단 cims 외부 프로세스가 점유 시에만 BLOCK.
   cims 자체 (csp/psp/cmp/pmp/cspsim/cwrtc/csc_app.py/cims_agent.py 등) 가
   점유 중이면 dev+배포본 동시 운용 design 정상 상태로 간주.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
from typing import List, Tuple

from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext


_TB_PORTS = [
    (4419, "tcp", "TB-CSC"),
    (3000, "tcp", "TB-Console"),
    (9902, "tcp", "TB-agent"),
]
_TARGET_PORTS = [
    (5060, "udp"), (5061, "tcp"), (9000, "udp"), (9001, "udp"),
    (4420, "tcp"), (4421, "tcp"), (3001, "tcp"), (3002, "tcp"),
    (8080, "tcp"), (8443, "tcp"),
]
_CIMS_NATIVE_BINS = {"csp", "psp", "isp", "cmp", "pmp", "imp", "cspsim", "cwrtc"}
_CIMS_CMD_MARKERS = ("csc_app.py", "cims_agent.py", "/build/dist/", "/cims-console/")


def _is_cims_pid(pid: int, repo_root: str) -> bool:
    """pid 의 실행 파일/cwd 가 cims 관련인지 판정."""
    if pid <= 0:
        return False
    real_root = os.path.realpath(repo_root)
    # 1) cwd 가 repo_root 하위 — cims-console/cims-phone vite 등 모든 monorepo
    #    하위 프로세스 catch (가장 일반화된 케이스).
    try:
        cwd = os.readlink(f"/proc/{pid}/cwd")
        if cwd == real_root or cwd.startswith(real_root + os.sep):
            return True
    except OSError:
        pass
    # 2) exe 가 repo_root 하위 (build/bin 등 직접 실행 native 포함).
    try:
        exe = os.readlink(f"/proc/{pid}/exe")
    except OSError:
        return False
    if exe.startswith(real_root + os.sep):
        return True
    base = os.path.basename(exe)
    if base in _CIMS_NATIVE_BINS:
        return True
    # 3) python/node wrapper — cmdline 에 cims 마커 포함 시.
    if base.startswith("python") or base == "node":
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                cmdline = f.read().replace(b"\x00", b" ").decode("utf-8", "replace")
            return any(m in cmdline for m in _CIMS_CMD_MARKERS)
        except OSError:
            return False
    return False


def _port_holders(port: int, proto: str) -> List[int]:
    """ss 로 해당 port 의 listener pid list 반환."""
    flag = "-Htlnp" if proto == "tcp" else "-Hulnp"
    try:
        out = subprocess.check_output(
            ["ss", flag], stderr=subprocess.DEVNULL, timeout=5,
        ).decode("utf-8", "replace")
    except Exception:
        return []
    pids: List[int] = []
    pat = re.compile(r"pid=(\d+)")
    # ss Local Address 컬럼은 `<host>:<port>` 또는 `*:<port>`. 선두 `\b` 는
    # `*` 같은 비-word 시작과 안 맞으므로 제거. 끝 `\b` 만 두면 `:30001` 이
    # `:3000` 에 false-match 하는 것을 막을 수 있음.
    addr_re = re.compile(rf"\S*:{port}\b")
    for line in out.splitlines():
        if not addr_re.search(line):
            continue
        for m in pat.finditer(line):
            pids.append(int(m.group(1)))
    return pids


def _check_local_ip(repo_root: str) -> Tuple[bool, str]:
    if os.environ.get("CIMS_LOCAL_IP"):
        return True, f"env CIMS_LOCAL_IP={os.environ['CIMS_LOCAL_IP']}"
    cfg = os.path.join(repo_root, ".cims", "server.local.json")
    if os.path.isfile(cfg):
        try:
            with open(cfg, "r", encoding="utf-8") as f:
                ip = (json.load(f).get("local_ip", "") or "").strip()
            if ip:
                return True, f".cims/server.local.json: {ip}"
        except Exception:
            pass
    try:
        out = subprocess.check_output(
            ["ip", "route", "get", "8.8.8.8"],
            stderr=subprocess.DEVNULL, timeout=2,
        ).decode("utf-8", "replace")
        m = re.search(r"\bsrc\s+(\d+\.\d+\.\d+\.\d+)", out)
        if m:
            return True, f"default route src: {m.group(1)}"
    except Exception:
        pass
    return False, "local_ip 미결정 — env/`/.cims/server.local.json`/default route 모두 실패"


def _check_db(repo_root: str) -> Tuple[bool, str]:
    cfg = os.path.join(repo_root, "build", "dist", "csp", "config", "csp.json")
    if not os.path.isfile(cfg):
        cfg = os.path.join(repo_root, "csp", "csp.json")
    if not os.path.isfile(cfg):
        return False, "csp.json 미존재 — DB 정보 없음"
    try:
        import pymysql  # type: ignore
    except ImportError:
        return False, "pymysql 미설치 — `pip install pymysql` 필요"
    try:
        with open(cfg, "r", encoding="utf-8") as f:
            d = json.load(f)
        db = d["Setup"]["Database"]
        conn = pymysql.connect(
            host=db["Host"], port=int(db.get("Port", 3306)),
            user=db["User"], password=db["Password"],
            database=db["DbName"], connect_timeout=3,
        )
        conn.close()
        return True, f"DB OK ({db['Host']}:{db.get('Port',3306)}/{db['DbName']})"
    except Exception as e:
        return False, f"DB 연결 실패: {type(e).__name__}: {e}"


def _check_tb_ports() -> Tuple[bool, List[str]]:
    msgs: List[str] = []
    all_ok = True
    for port, proto, label in _TB_PORTS:
        pids = _port_holders(port, proto)
        if pids:
            msgs.append(f"  - {label} ({port}/{proto}): pid={pids[0]} (OK)")
        else:
            msgs.append(f"  - {label} ({port}/{proto}): **미동작**")
            all_ok = False
    return all_ok, msgs


def _check_target_ports(repo_root: str) -> Tuple[bool, List[str]]:
    msgs: List[str] = []
    foreign_block = False
    for port, proto in _TARGET_PORTS:
        pids = _port_holders(port, proto)
        if not pids:
            msgs.append(f"  - {port}/{proto}: 가용")
            continue
        for pid in pids:
            if _is_cims_pid(pid, repo_root):
                msgs.append(f"  - {port}/{proto}: cims pid={pid} 점유 (OK)")
            else:
                msgs.append(f"  - {port}/{proto}: **외부 pid={pid} 점유 — BLOCK**")
                foreign_block = True
    return (not foreign_block), msgs


@verify_item(
    id="S2-PREFLIGHT",
    stage=2, category="환경",
    name="preflight (local_ip / DB / TB 3종 / 검증 포트 gate)",
    presets=["stage2-full", "pipeline-full", "pre-package"],
    side_effects=["read-only"], timeout_s=30,
    execution_order=10,
)
def preflight(ctx: VerifyContext) -> ItemResult:
    blocks: List[str] = []
    lines: List[str] = []

    ok, info = _check_local_ip(ctx.repo_root)
    lines.append(f"- local_ip: {'OK' if ok else 'FAIL'} — {info}")
    if not ok:
        blocks.append("local_ip 미결정")

    ok, info = _check_db(ctx.repo_root)
    lines.append(f"- DB: {'OK' if ok else 'FAIL'} — {info}")
    if not ok:
        blocks.append("DB 점검 실패")

    ok, tb_msgs = _check_tb_ports()
    lines.append(f"- TB 3종: {'OK' if ok else 'FAIL'}")
    lines.extend(tb_msgs)
    if not ok:
        blocks.append("TB 3종 미동작")

    ok, port_msgs = _check_target_ports(ctx.repo_root)
    lines.append(f"- 검증 포트: {'OK' if ok else 'FAIL'} (cims 외부만 BLOCK)")
    lines.extend(port_msgs)
    if not ok:
        blocks.append("외부 프로세스 포트 점유")

    ctx.w("## S2-PREFLIGHT — preflight (gate)")
    for line in lines:
        ctx.w(line)
    if blocks:
        ctx.w("- **BLOCK 사유**:")
        for b in blocks:
            ctx.w(f"  - {b}")
    ctx.w()

    detail = "\n".join(
        lines + ([f"BLOCK: {'; '.join(blocks)}"] if blocks else [])
    )
    return ItemResult(
        id="S2-PREFLIGHT", name="preflight",
        status=ItemStatus.FAIL if blocks else ItemStatus.PASS,
        detail=detail, stage=2,
    )
