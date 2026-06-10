#!/usr/bin/env python3
"""
CIMS Server Agent (P10)

Usage:
  cims_agent.py \
      --csc-url https://csc.example.com:4420 \
      --state-dir ~/.local/state/cims-agent

On first start (no state):
  - reads CIMS_ENROLLMENT_TOKEN env or --enrollment-token flag
  - POST /api/agent/enroll → receives session_token → saves to state

On subsequent starts:
  - reads session_token from state
  - POST /api/agent/heartbeat every 30s → receives pending jobs → executes

Job types (minimal P10 implementation):
  - install: download pkg → untar → save to install_path
  - start / stop / restart: systemd-like process control (uses local cims.sh)
  - update_config: apply config file change
  - collect_log: gather last N KB of logs
  - health_check: check port/process status

Security note:
  Initial version uses X-Agent-Token session token over HTTPS.
  P10.5 will upgrade to mutual TLS (client cert issued at enroll).
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import http.server
import json
import os
import platform
import re
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tarfile
import threading
import time
import traceback
import urllib.parse
import urllib.request
from datetime import datetime

DEFAULT_STATE_DIR = os.environ.get(
    "CIMS_AGENT_STATE",
    os.path.expanduser("~/.local/state/cims-agent"),
)
# 설치 루트 결정 우선순위:
#   1) CIMS_AGENT_INSTALL_ROOT 환경변수
#   2) <agent 바이너리 디렉토리>/modules    ← 권장 (agent 설치 디렉토리 기준 체계적 배치)
# 이전 기본값 /opt/cims 는 root 권한 필요했는데 user-mode 설치와 맞지 않음.
_AGENT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_INSTALL_ROOT = os.environ.get(
    "CIMS_AGENT_INSTALL_ROOT",
    os.path.join(_AGENT_DIR, "modules"),
)
DEFAULT_HEARTBEAT_SEC = 2
DEFAULT_METRIC_SEC = 2
DEFAULT_SYNC_PORT = 9900
def _read_pkg_version() -> str:
    """install dir 의 pkg.json 에서 version 읽기 — 패키지 버전과 자동 동기화.
    실패 시 'unknown' (런타임 자체는 계속 동작)."""
    try:
        import os as _os
        here = _os.path.dirname(_os.path.abspath(__file__))
        with open(_os.path.join(here, "pkg.json")) as f:
            return json.load(f).get("version", "unknown")
    except Exception:
        return "unknown"

AGENT_VERSION = _read_pkg_version()


# ──────────────────────────────────────────────────────────────
#  HTTP helper (urllib, TLS verify=False for dev)
# ──────────────────────────────────────────────────────────────

def http_post(url: str, data: dict, headers: dict = None, timeout: int = 15) -> tuple:
    body = json.dumps(data).encode("utf-8")
    hdrs = {"Content-Type": "application/json"}
    if headers: hdrs.update(headers)
    req = urllib.request.Request(url, data=body, headers=hdrs, method="POST")
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try: body = json.loads(e.read().decode("utf-8"))
        except Exception: body = {"error": f"HTTP {e.code}"}
        return e.code, body
    except Exception as e:
        return 0, {"error": str(e)}


def http_get_json(url: str, headers: dict = None, timeout: int = 30) -> tuple:
    req = urllib.request.Request(url, headers=headers or {}, method="GET")
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try: body = json.loads(e.read().decode("utf-8"))
        except Exception: body = {"error": f"HTTP {e.code}"}
        return e.code, body
    except Exception as e:
        return 0, {"error": str(e)}


def http_get_binary(url: str, headers: dict = None, timeout: int = 60) -> tuple:
    req = urllib.request.Request(url, headers=headers or {}, method="GET")
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            data = resp.read()
            meta = {k: v for k, v in resp.getheaders()}
            return resp.status, data, meta
    except Exception as e:
        return 0, None, {"error": str(e)}


# ──────────────────────────────────────────────────────────────
#  State management (session token persistence)
# ──────────────────────────────────────────────────────────────

class AgentState:
    def __init__(self, state_dir: str):
        self.dir = state_dir
        os.makedirs(self.dir, exist_ok=True)
        self.path = os.path.join(self.dir, "state.json")
        self._load()

    def _load(self):
        if os.path.exists(self.path):
            try:
                with open(self.path, "r") as f:
                    d = json.load(f)
                self.agent_id = d.get("agent_id")
                self.session_token = d.get("session_token")
                self.name = d.get("name")
            except Exception:
                self.agent_id = None; self.session_token = None; self.name = None
        else:
            self.agent_id = None; self.session_token = None; self.name = None

    def save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"agent_id": self.agent_id, "session_token": self.session_token,
                       "name": self.name}, f)
        os.chmod(tmp, 0o600)
        os.replace(tmp, self.path)


# ──────────────────────────────────────────────────────────────
#  Host info collection
# ──────────────────────────────────────────────────────────────

def collect_host_info() -> dict:
    info = {
        "hostname": socket.gethostname(),
        "os_info": f"{platform.system()} {platform.release()}",
        "agent_version": AGENT_VERSION,
    }
    try:
        info["cpu_cores"] = os.cpu_count() or 0
    except Exception:
        info["cpu_cores"] = 0
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    kb = int(line.split()[1])
                    info["memory_mb"] = kb // 1024
                    break
    except Exception:
        info["memory_mb"] = 0
    try:
        total, used, free = shutil.disk_usage("/")
        info["disk_gb"] = total // (1 << 30)
    except Exception:
        info["disk_gb"] = 0
    info["interfaces"] = collect_interfaces()
    info["routes"] = collect_routes()
    info["mounts"] = collect_mounts()
    return info


# mgmt IP — agent 시작 시 oam_url 의 outgoing local IP 로 결정 후 캐시.
# collect_interfaces() 가 이 IP 와 매칭되는 row 에 mgmt=True 플래그를 붙임.
_MGMT_IP: str | None = None


def detect_mgmt_ip(oam_url: str) -> str | None:
    """oam_url 로 가는 outgoing local IP 반환 — 그 IP 의 NIC 이 mgmt (CSC 통신용).
    UDP socket 의 connect 로 routing table 만 평가 (실제 패킷 송신 없음).
    """
    try:
        parsed = urllib.parse.urlparse(oam_url)
        host = parsed.hostname
        port = parsed.port or (443 if parsed.scheme == "https" else 80)
        if not host:
            return None
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((host, port))
            return s.getsockname()[0]
        finally:
            s.close()
    except Exception:
        return None


def collect_interfaces() -> list:
    """ip -j addr 로 IPv4 인터페이스 list 수집.
    한 iface 의 primary + secondary IP 모두 별도 row 로 추출 (VIP 보유 여부 추적용).
    IP 가 없는 NIC 도 ip='' mask=0 row 로 1건 추가 — Console 의 ServiceIpPanel
    에서 빈 NIC 도 후보로 노출 (운영자가 거기에 IP 부여 가능). lo (loopback) 만 제외.
    CSC 통신 NIC 은 mgmt=True 플래그. HaServicesPage 의 서비스 IP / VIP 설정 시
    운영자에게 후보 iface 제공 + ServiceIpRow/VipBinding 의 status 매칭에도 사용.

    cims-priv ip-add 가 부여한 label '<iface>:cims' 이 있는 IP 는 managed=True 로
    표시 — UI 에서 이런 IP 만 [삭제] 허용 (외부 IP 보호).
    """
    try:
        # -4 flag 를 쓰면 IPv4 없는 NIC 이 출력 자체에서 빠지므로, 전체 family 받고
        # 아래 루프에서 family=='inet' 만 row 로 변환.
        out = subprocess.run(["ip", "-j", "addr"],
                             capture_output=True, text=True, timeout=3)
        if out.returncode != 0:
            return []
        rows = json.loads(out.stdout or "[]")
    except Exception:
        return []
    result = []
    for r in rows:
        name = r.get("ifname")
        if not name or name == "lo":
            continue
        ipv4_rows = []
        for a in (r.get("addr_info") or []):
            if a.get("family") != "inet":
                continue
            ip = a.get("local")
            if not ip:
                continue
            row = {
                "name": name,
                "ip":   ip,
                "mask": int(a.get("prefixlen") or 0),
            }
            if _MGMT_IP and ip == _MGMT_IP:
                row["mgmt"] = True
            label = a.get("label") or ""
            # cims-priv ip-add 가 부여한 label 패턴. iface 이름이 11자 이상이면 label
            # 생략됨 (cims-priv 정책) — 그 경우 managed=False 로 식별 불가.
            if label.endswith(":cims"):
                row["managed"] = True
            ipv4_rows.append(row)
        if ipv4_rows:
            result.extend(ipv4_rows)
        else:
            # IPv4 미할당 NIC — UI 의 ServiceIp 후보로만 노출.
            result.append({"name": name, "ip": "", "mask": 0})
    return result


def collect_routes() -> list:
    """ip -j route 로 IPv4 route list 전체 수집.

    플래그:
      - is_default : default route (dst='default' 또는 '0.0.0.0/0') — readonly
      - kernel_auto: protocol=kernel (subnet 자동 생성 route) — readonly
      - managed    : cims-managed iface (label '<iface>:cims' IP 보유 NIC) 위
                     사용자-추가 specific route — [삭제] 허용
    위 셋 모두 아닌 외부 specific route 도 표시 (readonly).
    """
    try:
        out = subprocess.run(["ip", "-j", "route"],
                             capture_output=True, text=True, timeout=3)
        if out.returncode != 0:
            return []
        rows = json.loads(out.stdout or "[]")
    except Exception:
        return []
    managed_devs = set()
    for i in collect_interfaces():
        if i.get("managed") and i.get("name"):
            managed_devs.add(i["name"])
    result = []
    for r in rows:
        dst = r.get("dst") or ""
        dev_ = r.get("dev") or ""
        gw   = r.get("gateway") or ""
        protocol = r.get("protocol") or ""
        if not dst:
            continue
        is_default  = dst in ("default", "0.0.0.0/0")
        kernel_auto = protocol == "kernel"
        row = {"dst": dst, "via": gw, "dev": dev_}
        if is_default: row["is_default"] = True
        if kernel_auto: row["kernel_auto"] = True
        # specific user route 만 managed 판정 (kernel/default 는 자동 생성/system).
        if dev_ in managed_devs and not is_default and not kernel_auto:
            row["managed"] = True
        result.append(row)
    return result


def collect_mounts() -> list:
    """cims-managed 마운트(fstab 의 '# cims-managed' 라인) + 현재 마운트 여부 보고.
    Console MountPanel 이 desired(여기) + status(mounted) 표시."""
    result = []
    try:
        mounted = set()
        with open("/proc/mounts") as f:
            for ln in f:
                p = ln.split()
                if len(p) >= 2:
                    mounted.add(p[1])
        with open("/etc/fstab") as f:
            for ln in f:
                if "cims-managed" not in ln or ln.lstrip().startswith("#"):
                    continue
                s = ln.split()
                if len(s) < 3:
                    continue
                result.append({
                    "source":  s[0],
                    "target":  s[1],
                    "fstype":  s[2],
                    "options": s[3] if len(s) > 3 else "",
                    "mounted": s[1] in mounted,
                })
    except Exception:
        pass
    return result


_PROC_CPU_CACHE: dict = {}  # {pid: (utime+stime jiffies, sample_ts)}
_NET_DEV_CACHE: dict = {}   # {iface: (rx_bytes, tx_bytes, sample_ts)}
_HOST_CPU_CACHE: dict = {}  # {"prev": (total_jiffies, idle_jiffies)}
try:
    _CLK_TCK = os.sysconf("SC_CLK_TCK")
except Exception:
    _CLK_TCK = 100


def _proc_cpu_pct(pid: int) -> float | None:
    """/proc/<pid>/stat 의 utime+stime 두 sample 차이로 CPU% 계산.
    첫 호출 시 sample 만 저장하고 None 반환 — 다음 호출부터 값 산출."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().split()
        # field 13 = utime, 14 = stime (man proc(5))
        cur_j = int(fields[13]) + int(fields[14])
        cur_ts = time.time()
    except Exception:
        _PROC_CPU_CACHE.pop(pid, None)
        return None
    prev = _PROC_CPU_CACHE.get(pid)
    _PROC_CPU_CACHE[pid] = (cur_j, cur_ts)
    if not prev:
        return None
    prev_j, prev_ts = prev
    elapsed = cur_ts - prev_ts
    if elapsed <= 0:
        return None
    used_sec = (cur_j - prev_j) / _CLK_TCK
    return round(used_sec / elapsed * 100, 1)


def _host_cpu_pct() -> float | None:
    """/proc/stat 의 aggregate cpu 라인 두 sample 차이로 호스트 CPU 사용률(%) 산출.
    첫 호출 시 sample 만 저장하고 None 반환 — 다음 호출부터 값. (psutil 무의존, private 환경 대비)"""
    try:
        with open("/proc/stat") as f:
            for line in f:
                if line.startswith("cpu "):
                    parts = [int(x) for x in line.split()[1:]]
                    break
            else:
                return None
    except Exception:
        return None
    # user nice system idle iowait irq softirq steal ...
    idle = parts[3] + (parts[4] if len(parts) > 4 else 0)   # idle + iowait
    total = sum(parts)
    prev = _HOST_CPU_CACHE.get("prev")
    _HOST_CPU_CACHE["prev"] = (total, idle)
    if not prev:
        return None
    prev_total, prev_idle = prev
    dt = total - prev_total
    di = idle - prev_idle
    if dt <= 0:
        return None
    return round((dt - di) / dt * 100, 1)


def _proc_rss_mb(pid: int) -> int | None:
    """/proc/<pid>/status 의 VmRSS (kB) → MB."""
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) // 1024
    except Exception:
        pass
    return None


def collect_per_iface() -> list:
    """/proc/net/dev 의 각 NIC RX/TX bytes/errors. lo 제외.
    rx_rate/tx_rate (Bps) 는 직전 sample 과의 차이로 산출 — 첫 호출 시 None."""
    rows = []
    try:
        with open("/proc/net/dev") as f:
            lines = f.readlines()
    except Exception:
        return rows
    cur_ts = time.time()
    for line in lines[2:]:
        if ":" not in line:
            continue
        name, rest = line.split(":", 1)
        name = name.strip()
        if name == "lo":
            continue
        parts = rest.split()
        if len(parts) < 16:
            continue
        rx_bytes = int(parts[0]); rx_errors = int(parts[2])
        tx_bytes = int(parts[8]); tx_errors = int(parts[10])
        row = {"name": name, "rx_bytes": rx_bytes, "tx_bytes": tx_bytes,
               "rx_errors": rx_errors, "tx_errors": tx_errors}
        prev = _NET_DEV_CACHE.get(name)
        _NET_DEV_CACHE[name] = (rx_bytes, tx_bytes, cur_ts)
        if prev:
            prev_rx, prev_tx, prev_ts = prev
            elapsed = cur_ts - prev_ts
            if elapsed > 0:
                row["rx_rate"] = int((rx_bytes - prev_rx) / elapsed)
                row["tx_rate"] = int((tx_bytes - prev_tx) / elapsed)
        rows.append(row)
    return rows


# 가상/의사 파일시스템 — mount별 사용률에서 제외 (실제 저장장치만).
_VIRTUAL_FSTYPES = {
    "proc", "sysfs", "tmpfs", "devtmpfs", "devpts", "cgroup", "cgroup2",
    "mqueue", "hugetlbfs", "debugfs", "tracefs", "securityfs", "pstore",
    "bpf", "configfs", "fusectl", "ramfs", "autofs", "binfmt_misc",
    "overlay", "squashfs", "nsfs", "fuse.gvfsd-fuse", "fuse.portal",
}


def collect_per_mount() -> list:
    """/proc/mounts 순회 — 실제 블록 디바이스 마운트별 사용률.
    가상 fs(tmpfs/proc/sys 등)와 중복 device 는 제외. statvfs 로 total/used/pct 산출."""
    rows = []
    seen_dev = set()
    try:
        with open("/proc/mounts") as f:
            lines = f.readlines()
    except Exception:
        return rows
    for line in lines:
        parts = line.split()
        if len(parts) < 3:
            continue
        dev, mount, fstype = parts[0], parts[1], parts[2]
        if fstype in _VIRTUAL_FSTYPES:
            continue
        # 실제 디바이스(/dev/...)만 — bind mount/중복 device 제외.
        if not dev.startswith("/dev/"):
            continue
        if dev in seen_dev:
            continue
        seen_dev.add(dev)
        try:
            st = os.statvfs(mount)
            total = st.f_blocks * st.f_frsize
            free  = st.f_bavail * st.f_frsize
            used  = total - free
            if total <= 0:
                continue
            rows.append({
                "mount": mount,
                "device": dev,
                "total": total,
                "used": used,
                "pct": round(used / total * 100, 1),
            })
        except Exception:
            continue
    return rows


# 전환 안전망(설치 루트 enumerate 실패 시) 기본 데몬 집합.
_DEFAULT_METRIC_MODULES = ("csp", "cmp", "csc", "cwrtc")
# 자기 이름/`<name>_app.py` 로 식별 불가한 모듈 — module 보고에서 제외 (오탐 방지).
#   agent = 자신(liveness 는 heartbeat/online 으로 판정), console = nginx/vite (프로세스명 무관).
_NON_DAEMON_MODULES = {"agent", "console"}


def _metric_module_names() -> list:
    """metric 의 modules 보고 대상 = 설치된 모듈(modules/<module>/) ∪ 기본 집합.
    OAM 의 module_down alert 가 이 실행 집합과 deployment(status=running) 를 비교하므로,
    설치된 모듈을 빠짐없이 보고해야 isp 등 기본 집합 밖 모듈의 오탐(false down)을 막는다."""
    names = set(_DEFAULT_METRIC_MODULES)
    try:
        for nm in os.listdir(DEFAULT_INSTALL_ROOT):
            if os.path.isdir(os.path.join(DEFAULT_INSTALL_ROOT, nm)):
                names.add(nm)
    except Exception:
        pass
    # supervised.json 의 모듈도 포함 — install_path 가 agent 트리 밖(예: /opt/cims-agent/isp)이라
    # DEFAULT_INSTALL_ROOT listdir 로 안 잡히는 모듈(isp/psp)의 false module_down 방지 (경로 독립).
    try:
        with open(_SUPERVISE_FILE) as f:
            names.update(json.load(f).keys())
    except Exception:
        pass
    return sorted(names - _NON_DAEMON_MODULES)


def _pgrep_module(name: str):
    """모듈 프로세스 (pid, cmdline) 1개 반환, 없으면 None.
    1) comm 정확 매칭(-x) — C++ 바이너리(csp/cmp/isp/cwrtc). 비앵커 매칭은 'isp' 가
       'networkd-dispatcher' 에 오매칭되므로 반드시 -x.
    2) `<name>_app.py` cmdline 매칭(-f) — python 데몬(csc/oam). comm 이 python3 라 1)로 안 잡힘."""
    for argv in (["pgrep", "-ax", name], ["pgrep", "-af", f"{name}_app.py"]):
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=2)
        except Exception:
            continue
        for line in r.stdout.splitlines():
            parts = line.split(maxsplit=1)
            if parts and parts[0].isdigit():
                return int(parts[0]), (parts[1] if len(parts) > 1 else "")
    return None


def collect_metrics() -> dict:
    """CPU/mem/disk percent + load + per-iface RX/TX + CIMS module pid/cpu/mem."""
    m = {}
    # host CPU% (/proc/stat delta) — psutil 무의존.
    m["cpu_pct"] = _host_cpu_pct()
    try:
        with open("/proc/loadavg") as f:
            m["load_avg"] = f.read().strip().split()[0:3]
            m["load_avg"] = ",".join(m["load_avg"])
    except Exception:
        m["load_avg"] = ""
    # mem
    try:
        total = avail = 0
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):     total = int(line.split()[1])
                elif line.startswith("MemAvailable:"): avail = int(line.split()[1])
        if total > 0: m["mem_pct"] = round((total - avail) / total * 100, 1)
    except Exception: pass
    # disk /
    try:
        total, used, free = shutil.disk_usage("/")
        m["disk_pct"] = round(used / total * 100, 1)
    except Exception: pass
    # per-iface RX/TX
    m["per_iface"] = collect_per_iface()
    # mount별 disk 사용률 (실제 블록 디바이스만)
    m["mounts"] = collect_per_mount()
    # modules — 실행 중 모듈 (pid/cpu/mem) + 기존 processes 유지 (호환).
    m["processes"] = []
    m["modules"]   = []
    seen_pids = set()
    for procname in _metric_module_names():
        hit = _pgrep_module(procname)
        if not hit:
            continue
        pid, cmd = hit
        if pid in seen_pids:
            continue
        seen_pids.add(pid)
        if not m["processes"]:
            m["processes"].append({"name": procname, "pid": pid, "cmdline": cmd})
        m["modules"].append({
            "name": procname, "pid": pid,
            "cpu_pct": _proc_cpu_pct(pid),
            "mem_mb":  _proc_rss_mb(pid),
        })
    # 사라진 pid 캐시 정리 — 메모리 누수 방지.
    live = {x["pid"] for x in m["modules"]}
    for stale_pid in list(_PROC_CPU_CACHE.keys()):
        if stale_pid not in live:
            _PROC_CPU_CACHE.pop(stale_pid, None)
    return m


# ──────────────────────────────────────────────────────────────
#  On-demand health check (sync REST /health-check)
# ──────────────────────────────────────────────────────────────

def _health_check_ha() -> dict:
    """keepalived service 상태 + VIP 부여 여부 + journal tail."""
    out = {"keepalived_installed": False, "keepalived_active": False, "vips": []}
    # binary 존재 확인 — purge 후엔 systemctl 만으론 (unit cache) 미설치 판별 불가.
    try:
        r = subprocess.run(["which", "keepalived"], capture_output=True, text=True, timeout=2)
        if r.returncode == 0 and r.stdout.strip():
            out["keepalived_installed"] = True
    except Exception as e:
        out["error"] = str(e); return out
    if not out["keepalived_installed"]:
        return out  # 미설치 — 검사 끝 (verdict=healthy)
    # service status
    try:
        rc = subprocess.run(["systemctl", "is-active", "keepalived"],
                            capture_output=True, text=True, timeout=3)
        out["keepalived_active"] = (rc.stdout.strip() == "active")
    except FileNotFoundError:
        return out
    except Exception as e:
        out["error"] = str(e); return out
    # ip addr 에서 secondary (VIP) 식별 — keepalived 가 add 한 VIP 는 보통 secondary 플래그.
    try:
        r = subprocess.run(["ip", "-j", "addr"], capture_output=True, text=True, timeout=3)
        ifaces = json.loads(r.stdout or "[]")
        for f in ifaces:
            iname = f.get("ifname") or ""
            if iname == "lo": continue
            for a in (f.get("addr_info") or []):
                if a.get("family") != "inet": continue
                flags = (a.get("flags") or [])
                # secondary 플래그가 있거나 keepalived label 이 있는 IP
                if "secondary" in flags or (a.get("label") or "").endswith(":vrrp"):
                    out["vips"].append({"iface": iname, "ip": a.get("local"),
                                         "mask": a.get("prefixlen")})
    except Exception as e:
        out["ip_addr_error"] = str(e)
    # systemctl status (sudo 불필요, 마지막 log lines + Active state 포함)
    try:
        r = subprocess.run(["systemctl", "status", "keepalived", "--no-pager", "-n", "15"],
                            capture_output=True, text=True, timeout=3)
        # status 는 OK 면 0, inactive/failed 면 3 등 — stdout 은 항상 채워짐.
        if r.stdout:
            out["journal_tail"] = [line for line in r.stdout.splitlines()
                                   if line.strip()][-15:]
    except Exception:
        pass
    return out


def _health_check_modules() -> list:
    """모듈 프로세스 상태 (설치된 데몬) + RSS, CPU%, uptime."""
    rows = []
    for procname in _metric_module_names():
        try:
            hit = _pgrep_module(procname)
            if not hit:
                rows.append({"name": procname, "running": False})
                continue
            pid, _cmd = hit
            row = {"name": procname, "running": True, "pid": pid,
                   "cpu_pct": _proc_cpu_pct(pid),
                   "mem_mb":  _proc_rss_mb(pid)}
            # uptime — /proc/<pid>/stat 의 starttime 으로 계산
            try:
                with open(f"/proc/{pid}/stat") as f:
                    fields = f.read().split()
                starttime_j = int(fields[21])  # field 22
                with open("/proc/uptime") as f:
                    uptime = float(f.read().split()[0])
                row["uptime_sec"] = int(uptime - starttime_j / _CLK_TCK)
            except Exception:
                pass
            rows.append(row)
        except Exception as e:
            rows.append({"name": procname, "error": str(e)})
    return rows


def run_health_check(scope: str = "all") -> dict:
    """on-demand health check — keepalived/VIP + 모듈 상태 + 기본 시스템 메트릭.
    scope: 'ha' | 'modules' | 'all'."""
    from datetime import datetime as _dt
    result = {"ts": _dt.now().isoformat(timespec='seconds'),
              "agent_version": AGENT_VERSION,
              "hostname": socket.gethostname()}
    if scope in ("ha", "all"):
        result["ha"] = _health_check_ha()
    if scope in ("modules", "all"):
        result["modules"] = _health_check_modules()
    if scope == "all":
        # 기본 metric 도 같이 — 모달에서 spinner 안 띄우고 한 번에 노출.
        m = collect_metrics()
        result["metrics"] = {
            "cpu_pct": m.get("cpu_pct"),
            "mem_pct": m.get("mem_pct"),
            "disk_pct": m.get("disk_pct"),
            "load_avg": m.get("load_avg"),
            "per_iface": m.get("per_iface"),
        }
    # 종합 verdict — healthy / partial / broken
    issues = []
    if scope in ("ha", "all") and result.get("ha"):
        ha = result["ha"]
        if ha.get("keepalived_installed") and not ha.get("keepalived_active"):
            issues.append("keepalived inactive")
    if scope in ("modules", "all") and result.get("modules"):
        for m in result["modules"]:
            if m.get("running") is False:
                pass  # 모듈 미배포는 정상
            elif m.get("error"):
                issues.append(f"{m.get('name')}: {m.get('error')}")
    result["verdict"] = "healthy" if not issues else ("partial" if len(issues) <= 1 else "broken")
    result["issues"] = issues
    return result


# ──────────────────────────────────────────────────────────────
#  Job executors
# ──────────────────────────────────────────────────────────────

def _default_install_subpath(params: dict) -> str:
    """설치 경로 결정: modules/<module>/<version>/

    - module:  params["package_name"] (없으면 pkg-<id>)
    - version: params["package_version"] (없으면 "unknown")

    예전엔 `<module>/<version>/<process>/` 였지만 tarball top-dir 와 항상 같은
    이름 (csp 패키지 → tarball top=csp, process=csp) 으로 중복이라 process 단계
    제거. 결과: tarball 풀면 `modules/csp/0.0.3/csp/` (top dir 가 마지막).
    """
    module = (params.get("package_name") or f"pkg-{params.get('package_id','unknown')}").strip()
    version = (params.get("package_version") or "unknown").strip()
    return os.path.join(module, version)


def _resolve_install_path(params: dict) -> str:
    """params.install_path 가 명시되면 그대로 (단 쓰기 불가 시 cwd fallback — dev 환경).
    명시 안 되면 modules/<m>/<v>/<p>/ 로 조합."""
    explicit = params.get("install_path")
    if explicit:
        # 가장 가까운 존재하는 조상 디렉토리의 쓰기 가능 여부 체크 — 버전 디렉토리
        # (<module>/<version>) 는 2 depth 까지 미존재일 수 있음.
        try:
            probe = explicit
            while probe and probe != "/" and not os.path.isdir(probe):
                probe = os.path.dirname(probe) or "/"
            if probe and os.path.isdir(probe) and os.access(probe, os.W_OK):
                return explicit
        except Exception:
            pass
        # 쓰기 불가 — cwd fallback (dev 환경: /opt/cims 권한 없음)
        return os.getcwd()
    return os.path.join(DEFAULT_INSTALL_ROOT, _default_install_subpath(params))


# 버전 디렉토리 판별 — "0.0.35", "1.2", "1.2.3-rc1" 등. 모듈 잔재 디렉토리
# (bin/config/lib 등) 와 절대 겹치지 않도록 선행 숫자+점 형태만 인정.
_VERSION_DIR_RE = re.compile(r"^\d+(\.\d+){1,3}([.\-+][0-9A-Za-z.\-+]+)?$")


def _module_root_of(install_path: str, module: str) -> str:
    """install_path 로부터 모듈 루트(/…/<module>) 를 정규화.

    - …/<module>/<version> → …/<module>      (이미 버전 경로)
    - …/<module>           → 그대로           (durability 표준 경로)
    - 그 외 (legacy 공유 루트 /opt/cims-agent 등) → <install_path>/<module>
    """
    base = (install_path or "").rstrip("/")
    bn = os.path.basename(base)
    if _VERSION_DIR_RE.match(bn) and os.path.basename(os.path.dirname(base)) == module:
        return os.path.dirname(base)
    if bn == module:
        return base
    return os.path.join(base, module)


def _versioned_install_path(params: dict) -> tuple:
    """버전 단위 설치 경로 결정 → (target_path, module_root, legacy_path).

    target  = <module_root>/<version>  — 버전별 병렬 설치 (롤백 단위)
    legacy  = params.install_path 해석 결과 (이전 라이브 경로; config 이관 원천)
    module/version 미상이면 버전화 불가 → target=legacy (구 동작 유지).
    """
    legacy = _resolve_install_path(params)
    module = (params.get("package_name") or "").strip()
    version = (params.get("package_version") or "").strip()
    if not module or not version or not _VERSION_DIR_RE.match(version):
        return legacy, "", legacy
    root = _module_root_of(legacy, module)
    return os.path.join(root, version), root, legacy


def _runtime_install_path(params: dict) -> str:
    """start/restart/update_config 등 런타임 작업의 실효 install_path.

    deployment 레코드가 아직 구 경로(비버전)를 가리켜도, 같은 모듈/버전의
    버전 디렉토리가 이미 설치돼 있으면 그쪽을 우선 — upgrade(설치)와 restart 가
    한 배치로 큐잉돼 record 갱신 전에 도착하는 stale-params 레이스 방어."""
    target, _root, legacy = _versioned_install_path(params)
    if target != legacy and os.path.isdir(target):
        return target
    return legacy


def _prune_old_versions(module_root: str, keep: int = 3) -> list:
    """모듈 루트의 버전 디렉토리를 mtime 최신 keep 개만 남기고 제거.

    버전 패턴(_VERSION_DIR_RE) 디렉토리만 대상 — legacy 평탄 설치 잔재(bin/,
    config/ 등)는 절대 건드리지 않음. 제거 목록 반환 (로그용)."""
    removed = []
    try:
        if not os.path.isdir(module_root):
            return removed
        vers = []
        for nm in os.listdir(module_root):
            p = os.path.join(module_root, nm)
            if os.path.isdir(p) and _VERSION_DIR_RE.match(nm):
                try:
                    vers.append((os.path.getmtime(p), p))
                except OSError:
                    pass
        vers.sort(reverse=True)
        for _mt, p in vers[keep:]:
            try:
                shutil.rmtree(p)
                removed.append(p)
            except Exception:
                pass
    except Exception:
        pass
    return removed


def _write_config_file(install_path: str, config_values: dict) -> str:
    """install_path/config.json 에 설정 값 기록. 경로 반환."""
    cfg_path = os.path.join(install_path, "config.json")
    os.makedirs(install_path, exist_ok=True)
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(config_values or {}, f, ensure_ascii=False, indent=2)
    return cfg_path


def _find_previous_install(module_root: str, current_version: str) -> str:
    """같은 모듈 루트의 이전 버전 install_path 찾기 (mtime 최신 1개).

    새 버전 설치 시 기존 config/ 를 이관하기 위한 조회. 버전 패턴 디렉토리만
    후보 (legacy 평탄 잔재 배제)."""
    if not module_root or not os.path.isdir(module_root): return ""
    candidates = []
    for v in os.listdir(module_root):
        if v == current_version or not _VERSION_DIR_RE.match(v): continue
        p = os.path.join(module_root, v)
        if os.path.isdir(p):
            try: candidates.append((os.path.getmtime(p), p))
            except OSError: pass
    if not candidates: return ""
    candidates.sort(reverse=True)
    return candidates[0][1]


def _detect_tar_pkg_subdir(tar_path: str) -> str:
    """tarball 안 top-level 디렉토리 이름을 반환. 단일 디렉토리 아니면 빈 문자열.

    cims.sh pkg 산출 tarball 구조: meta.json + cims.sh + config_template.json + <pkg_name>/.
    `<pkg_name>` 가 유일한 디렉토리 entry. 이 함수는 단일 변종 install 을 install_path
    전체가 아닌 install_path/<pkg_name>/ 로 좁히기 위한 정보 제공.
    """
    try:
        with tarfile.open(tar_path, "r:gz") as tf:
            dirs = set()
            for m in tf.getmembers():
                first = m.name.split("/", 1)[0]
                if first in ("", ".", ".."):
                    continue
                if "/" in m.name or m.isdir():
                    dirs.add(first)
                if len(dirs) > 3:
                    return ""
            if len(dirs) == 1:
                return next(iter(dirs))
    except Exception:
        pass
    return ""


def job_install(params: dict, oam_url: str, session_token: str) -> tuple:
    """PKG 다운로드 + tarball 풀어 **버전 단위 경로** 에 설치. config.json 도 함께 기록.

    설치 경로 = <module_root>/<package_version>/ (예: /opt/cims-agent/csp/0.0.36).
    버전별 병렬 설치 → 롤백 = deployment.install_path 를 이전 버전 디렉토리로 전환.
    stdout 의 `at <path> (` 를 OAM report 훅이 파싱해 deployment.install_path 갱신.

    config 이관: 이전 라이브 경로(params.install_path; legacy 평탄 설치 포함) 또는
    최신 sibling 버전 디렉토리에서 config/(HA 동기 collection jsonl) + <pkg>/config.json
    (deployment overlay) 을 신규 버전 디렉토리로 복사 — collection 은 항상
    해당 모듈/해당 버전의 config 에 귀속된다.

    설치 후 오래된 버전 디렉토리는 최신 3개만 유지(prune); legacy 평탄 잔재
    (bin/, config/ 등 비버전 엔트리)는 건드리지 않는다.
    """
    pkg_id = params.get("package_id")
    install_path, module_root, legacy_path = _versioned_install_path(params)
    if not pkg_id:
        return 1, "", "package_id missing"

    url = f"{oam_url}/api/agent/package/{pkg_id}"
    status, data, meta = http_get_binary(url, {"X-Agent-Token": session_token})
    if status != 200 or not data:
        return 2, "", f"download failed status={status} meta={meta.get('error','')}"

    sha_expected = meta.get("X-Package-Sha256") or meta.get("x-package-sha256")
    sha_actual   = hashlib.sha256(data).hexdigest()
    if sha_expected and sha_actual != sha_expected:
        return 3, "", f"sha256 mismatch expected={sha_expected} got={sha_actual}"

    os.makedirs(install_path, exist_ok=True)

    # tarball 먼저 저장 — top-level dir 검출에 필요.
    tar_path = os.path.join(install_path, "_pkg.tar.gz")
    with open(tar_path, "wb") as f:
        f.write(data)

    pkg_subdir = _detect_tar_pkg_subdir(tar_path)
    # 단일 변종 tarball (csp/psp/isp/cmp/pmp/imp 등) 이면 sibling 디렉토리 영향 차단.
    # csc/console/cspsim 등도 단일 root 라 동일 동작 — install_path 가 mgmt-server 같은
    # multi-pkg agent 면 scoped, single-pkg agent 면 결과적으로 install_path 와 동일.
    if pkg_subdir:
        scope = os.path.join(install_path, pkg_subdir)
        backup = scope + ".prev"
    else:
        # 단일-root 감지 실패 — legacy 전체 wipe (안전 fallback).
        scope = install_path
        backup = install_path + ".prev"

    # 기존 내용 백업 (scope 만)
    if os.path.isdir(scope) and os.listdir(scope):
        try:
            if os.path.isdir(backup): shutil.rmtree(backup)
            shutil.copytree(scope, backup, symlinks=True, dirs_exist_ok=True)
        except Exception:
            pass

    # 실행 중인 바이너리 위로 tar 풀면 ETXTBSY ('Text file busy') 발생 →
    # untar 전체 fail. 사전에 scope 하위 모든 파일을 unlink 하면 OS 가 기존
    # inode 는 그대로 두고 새 파일을 교체 작성 → 실행 중인 프로세스에 영향 없이
    # untar 성공. 디렉터리 구조 자체는 보존하고 파일만 정리. tar 가 모든 파일을
    # 새로 뽑으므로 자료 손실 없음.
    if os.path.isdir(scope):
        for root, _dirs, files in os.walk(scope):
            for fname in files:
                p = os.path.join(root, fname)
                try:
                    if os.path.islink(p) or os.path.isfile(p):
                        os.unlink(p)
                except Exception:
                    pass

    try:
        with tarfile.open(tar_path, "r:gz") as tf:
            tf.extractall(install_path)
    except Exception as e:
        return 4, "", f"untar failed: {e}"
    finally:
        try: os.unlink(tar_path)
        except Exception: pass

    # 변종별 config.json 스코프. 한 install_path 에 형제 변종 (csp/isp) 공존 시
    # install_path/config.json 한 파일을 공유하면 last-write-wins 로 첫 변종 overlay
    # 가 덮어써짐 → cims.sh 가 wrong overlay 로 시작. 따라서 pkg_subdir 가 있으면
    # config.json 도 변종 디렉토리 내부에 쓴다 (install_path/<pkg>/config.json).
    # cims.sh start_*_variant 는 같은 경로에서 overlay 를 읽도록 갱신.
    cfg_target_dir = os.path.join(install_path, pkg_subdir) if pkg_subdir else install_path

    # 이전 설치본 config 이관 — 원천 우선순위:
    #   1) params.migrate_from (OAM 이 명시한 구 경로)
    #   2) legacy_path (deployment 레코드의 이전 install_path — 평탄 설치 포함)
    #   3) 최신 sibling 버전 디렉토리
    migrated = ""
    version = (params.get("package_version") or "").strip()
    if install_path != legacy_path:
        src = ""
        for cand in ((params.get("migrate_from") or "").strip(), legacy_path,
                     _find_previous_install(module_root, version)):
            if cand and cand != install_path and os.path.isdir(cand):
                src = cand
                break
        if src:
            try:
                # ① HA 동기 collection jsonl → 신규 버전의 install_path/config/
                #    (CSP jsonlDir = csp.json 부모×3 = 버전 디렉토리/config — 버전 귀속)
                src_col = os.path.join(src, "config")
                dst_col = os.path.join(install_path, "config")
                if os.path.isdir(src_col):
                    os.makedirs(dst_col, exist_ok=True)
                    for fn in os.listdir(src_col):
                        if not fn.endswith(".jsonl"):
                            continue
                        s = os.path.join(src_col, fn)
                        d = os.path.join(dst_col, fn)
                        if os.path.isfile(s) and not os.path.exists(d):
                            shutil.copy2(s, d)
                            migrated += f" +{fn}"
                # ② deployment overlay (<pkg>/config.json; legacy 는 root config.json)
                #    params.config 가 오면 그 값이 SoT — 이관 생략.
                if not params.get("config"):
                    for rel in ((os.path.join(pkg_subdir, "config.json") if pkg_subdir else ""),
                                "config.json"):
                        if not rel:
                            continue
                        s = os.path.join(src, rel)
                        d = os.path.join(cfg_target_dir, "config.json")
                        if os.path.isfile(s) and not os.path.isfile(d):
                            shutil.copy2(s, d)
                            migrated += " +overlay"
                            break
                if migrated:
                    migrated = f" (migrated from {src}:{migrated})"
            except Exception as e:
                return 6, "", f"config migration failed: {e}"

    # 설정 파일 기록 (params.config 가 있으면 사용자의 값, 없으면 빈 dict — 이관된 파일 유지)
    cfg_path = ""
    try:
        new_scalar = os.path.join(cfg_target_dir, "config.json")
        if params.get("config") or not os.path.isfile(new_scalar):
            cfg_path = _write_config_file(cfg_target_dir, params.get("config") or {})
        else:
            cfg_path = new_scalar
    except Exception as e:
        return 5, "", f"write config failed: {e}"

    # config/ 디렉토리 기본 생성 — 두 위치 모두.
    #  install_path/<pkg>/config/ : 변종 내부 (collection 저장소 — config_template.json 등)
    #  install_path/config/       : 서버 레벨 (CSP/CMP ELF 의 jsonlDir fallback 이 시작 시
    #                               자동 추정하는 위치 — install_path/config/<jsonl> 가 SoT).
    # 후자 누락 시 CSP 가 jsonlDir=(none) 으로 init → SIGUSR1 reload 도 무력화 (in-memory
    # jsonlDir 가 빈 채라 재탐색 안 함). 따라서 두 디렉토리 모두 install 시점에 미리 생성.
    os.makedirs(os.path.join(cfg_target_dir, "config"), exist_ok=True)
    os.makedirs(os.path.join(install_path, "config"), exist_ok=True)

    # 오래된 버전 디렉토리 prune (최신 3개 유지; 방금 설치본이 mtime 최신이므로
    # 직전 버전 2개까지 롤백 가능). legacy 평탄 잔재는 비대상.
    pruned = ""
    if module_root and install_path != legacy_path:
        removed = _prune_old_versions(module_root, keep=3)
        if removed:
            pruned = f" pruned={','.join(os.path.basename(p) for p in removed)}"

    return 0, (f"installed pkg_id={pkg_id} at {install_path} ({len(data)} bytes) "
               f"config={cfg_path}{migrated}{pruned}"), ""


def _resolve_pkg_subdir(install_path: str, params: dict) -> str:
    """multi-pkg agent 의 변종별 pid 디렉토리 분리에 사용. **config.json 위치 결정자
    아님** — config.json 의 overlay 위치는 CSP 의 _findDeploymentConfig
    (SipServerSetup.cpp:147-166) 가 csp.json 부모×2 = install_path/config.json 으로
    고정. 본 함수는 _signal_process 의 pid 탐색 순서에만 사용.

    우선순위: params.pkg_subdir 명시 → params.package_name 디렉토리 존재 → 빈 문자열.
    """
    explicit = (params.get("pkg_subdir") or "").strip()
    if explicit:
        return explicit
    pkg_name = (params.get("package_name") or "").strip()
    if pkg_name and os.path.isdir(os.path.join(install_path, pkg_name)):
        return pkg_name
    return ""


def job_update_config(params: dict, oam_url: str = "", session_token: str = "") -> tuple:
    """install_path/config.json 재기록 + 모듈 SIGUSR1 reload 트리거.

    CSP 의 reload 로직 (CspServer.cpp:336-354) 이 g_reloadFlag 를 폴링하여 csp.json
    재파싱 + 9 JSONL Sync 수행. **부트스트랩 필드** (Setup.Sip.LocalIp,
    UdpThreadCount 등) 는 이미 bound 된 socket / thread pool 에 반영 안 됨 — UI
    에서 별도 안내 (재기동 필요). pid 파일 없거나 권한 없으면 silently skip
    (stdout 의 signaled=[] 로 보고).

    config.json 은 install_path 의 root 에 직접 쓴다 — SipServerSetup 의 overlay
    탐색이 csp.json 부모×2 (= install_path) 의 config.json 을 봄. multi-pkg agent
    의 변종별 분리는 _signal_process 의 pid 탐색에서만 의미 가짐.

    HA fan-out: params.sync_id 가 있으면 csc 에 ack/nack 보고.
    """
    sync_id = params.get("sync_id")
    install_path = _runtime_install_path(params)
    if not os.path.isdir(install_path):
        return _sync_ack_and_return(oam_url, session_token, sync_id,
                                    rc=1, err=f"install_path not found: {install_path}")
    pkg_subdir = _resolve_pkg_subdir(install_path, params)
    # overlay 는 모듈 바이너리가 읽는 위치(<pkg>/config.json = csp.json 부모×2) 에
    # 기록 — pkg_subdir 없는 단일-루트 설치는 install_path 직하 (구 동작과 동일).
    cfg_dir = os.path.join(install_path, pkg_subdir) if pkg_subdir else install_path
    try:
        cfg_path = _write_config_file(cfg_dir, params.get("config") or {})
    except Exception as e:
        return _sync_ack_and_return(oam_url, session_token, sync_id,
                                    rc=2, err=f"write config failed: {e}")
    _, signaled = _signal_process(install_path, "usr1", pkg_subdir=pkg_subdir)
    return _sync_ack_and_return(oam_url, session_token, sync_id,
                                rc=0, out=f"config updated: {cfg_path} signaled={signaled}")


def job_sync_config(params: dict, oam_url: str, session_token: str) -> tuple:
    """HA fan-out: csc 의 컬렉션 jsonl 을 pull → install_path/config/<col>.jsonl 에
    atomic write → 로컬 CSP 에 SIGUSR1 → csc 에 ack 보고.

    params: {
      sync_id:     int,
      collection:  "csp_listener" | "sip_trunk" | "routing_rule" |
                   "routing_access_list" | "sip_service",
      op:          "CREATE" | "UPDATE" | "DELETE",
      row_id:      int,
      install_path: str,
      deployment_id: int,
      ha_group_id:   int,
    }

    Returns (rc, stdout, stderr). rc=0 이면 csc 에 status=ack 보고됨.
    Pull/Write/Signal 어느 단계 실패해도 rc≠0 + csc 에 status=nack 보고.
    """
    sync_id    = params.get("sync_id")
    collection = params.get("collection") or ""
    op         = params.get("op") or "UPDATE"
    # 버전 단위 설치: install_path 는 활성 버전 디렉토리 — collection jsonl 은
    # 항상 해당 모듈/해당 버전의 config/ 에 귀속 (버전별 config 분리 요구).
    install_path = _runtime_install_path(params)
    if not install_path or not os.path.isdir(install_path):
        return _sync_ack_and_return(oam_url, session_token, sync_id,
                                    rc=1, err=f"install_path not found: {install_path}")
    if not collection:
        return _sync_ack_and_return(oam_url, session_token, sync_id,
                                    rc=1, err="collection missing")

    # 1) csc 에서 컬렉션 pull
    pull_url = f"{oam_url}/api/agent/csp-config/{collection}"
    status, body = http_get_json(pull_url,
                                 headers={"X-Agent-Token": session_token},
                                 timeout=30)
    if status != 200 or not isinstance(body, dict) or "items" not in body:
        return _sync_ack_and_return(oam_url, session_token, sync_id,
                                    rc=2, err=f"pull failed: status={status} body={body}")
    items = body.get("items") or []
    etag  = body.get("etag") or ""

    # 2) install_path/config/<collection>.jsonl 에 atomic write (+ .bak)
    cfg_dir  = os.path.join(install_path, "config")
    jsonl_path = os.path.join(cfg_dir, f"{collection}.jsonl")
    try:
        os.makedirs(cfg_dir, exist_ok=True)
        if os.path.isfile(jsonl_path):
            bak = jsonl_path + ".bak"
            try:
                shutil.copy2(jsonl_path, bak)
            except Exception:
                pass
        n = _write_jsonl_atomic(jsonl_path, items)
    except Exception as e:
        return _sync_ack_and_return(oam_url, session_token, sync_id,
                                    rc=3, err=f"write failed: {e}")

    # 3) SIGUSR1 → 로컬 CSP (PID file 기반 — pkg_subdir 자동 탐색)
    pkg_subdir = _resolve_pkg_subdir(install_path, params)
    _, signaled = _signal_process(install_path, "usr1", pkg_subdir=pkg_subdir)

    msg = (f"sync_config ok: collection={collection} op={op} rows={n} "
           f"etag={etag} signaled={signaled}")
    return _sync_ack_and_return(oam_url, session_token, sync_id,
                                rc=0, out=msg)


def _sync_ack_and_return(oam_url: str, session_token: str, sync_id,
                         *, rc: int, out: str = "", err: str = "") -> tuple:
    """csc 에 ack/nack 보고 후 결과 튜플 반환. csc 호출 실패는 로그만 (rc 유지)."""
    if sync_id is None:
        return rc, out, err
    ack_url = f"{oam_url}/api/agent/sync/{int(sync_id)}/ack"
    payload = {"status": "ack" if rc == 0 else "nack"}
    if err:
        payload["error"] = err
    try:
        st, _ = http_post(ack_url, payload,
                          headers={"X-Agent-Token": session_token},
                          timeout=10)
        ack_note = f" ack_status={st}"
    except Exception as e:
        ack_note = f" ack_failed={e}"
    if rc == 0:
        return rc, out + ack_note, err
    return rc, out, err + ack_note


def job_update_ha(params: dict) -> tuple:
    """install_path/agent/keepalived/ha.json 갱신 + cims-ha config|apply 자동 실행.

    Params:
      - install_path: install root (예: /opt/cims/mgmt-server)
      - ha_json: dict — CSC 가 ha_groups + members 로부터 render 한 내용

    cims-ha apply 는 sudo 권한이 필요. 미등록 환경 (dev 등) 에서는 graceful
    skip — config 까지만 진행하고 apply 실패는 log 만 남기고 성공 반환.
    """
    install_path = _resolve_install_path(params)
    ha_json = params.get("ha_json") or {}
    if not isinstance(ha_json, dict) or not ha_json:
        return 1, "", "ha_json missing or empty"
    ha_path = os.path.join(install_path, "agent", "keepalived", "ha.json")
    try:
        os.makedirs(os.path.dirname(ha_path), exist_ok=True)
        with open(ha_path, "w", encoding="utf-8") as f:
            json.dump(ha_json, f, ensure_ascii=False, indent=2)
    except Exception as e:
        return 2, "", f"write ha.json failed: {e}"

    # services 비면 keepalived 의도적 미사용 (all_active + VIP null + vip_bindings=[]) —
    # 기존 설치본 있으면 cims-ha uninstall 으로 정리 (health-check 가 inactive issue 안 잡도록).
    services = ha_json.get("services") or {}
    if not services:
        msgs = [f"ha.json updated: {ha_path}",
                "ha.json.services empty — keepalived intentionally disabled"]
        cims_ha = _resolve_cims_ha()
        ha_dir_local = os.path.dirname(ha_path)
        if cims_ha:
            try:
                r = subprocess.run(["sudo", "-n", cims_ha, "--ha-dir", ha_dir_local, "uninstall"],
                                    capture_output=True, text=True, timeout=120)
                msgs.append(f"cims-ha uninstall rc={r.returncode}"
                           + (f" err={(r.stderr or r.stdout).strip()[-200:]}" if r.returncode != 0 else ""))
            except Exception as e:
                msgs.append(f"cims-ha uninstall exception: {e}")
        return 0, "\n".join(msgs), ""

    # cims-ha install + config + apply — sudoers 화이트리스트의 dev dist canonical 사용
    # ha.json 위치는 install_path 별로 다르므로 --ha-dir 인자로 전달.
    # install 은 keepalived 미설치 시 vendor deb 으로 자동 설치 (idempotent: ha.sh 내부 short-circuit).
    msgs = [f"ha.json updated: {ha_path}"]
    cims_ha = _resolve_cims_ha()
    ha_dir = os.path.dirname(ha_path)
    if cims_ha:
        try:
            r0 = subprocess.run(["sudo", "-n", cims_ha, "--ha-dir", ha_dir, "install"],
                                capture_output=True, text=True, timeout=120)
            msgs.append(f"cims-ha install rc={r0.returncode}"
                       + (f" err={(r0.stderr or r0.stdout).strip()[-200:]}" if r0.returncode != 0 else ""))
        except Exception as e:
            msgs.append(f"cims-ha install exception: {e}")
        try:
            # config 는 read-only render — sudo 불필요. 이전엔 sudo 로 호출되어 out/ 결과물이
            # root 소유로 생성 → uninstall.sh 가 그 디렉토리 못 지우는 비대칭 발생. 해소.
            r1 = subprocess.run([cims_ha, "--ha-dir", ha_dir, "config"],
                                capture_output=True, text=True, timeout=30)
            msgs.append(f"cims-ha config rc={r1.returncode}"
                       + (f" err={(r1.stderr or r1.stdout).strip()[-200:]}" if r1.returncode != 0 else ""))
        except Exception as e:
            msgs.append(f"cims-ha config exception: {e}")
        try:
            r2 = subprocess.run(["sudo", "-n", cims_ha, "--ha-dir", ha_dir, "apply"],
                                capture_output=True, text=True, timeout=60)
            msgs.append(f"cims-ha apply rc={r2.returncode}"
                       + (f" err={(r2.stderr or r2.stdout).strip()[-200:]}" if r2.returncode != 0 else ""))
        except Exception as e:
            msgs.append(f"cims-ha apply exception (likely no keepalived / no sudo): {e}")
    else:
        msgs.append("cims-ha not found in candidate paths — ha.json only (no apply)")
    return 0, "\n".join(msgs), ""


def _resolve_cims_ha() -> "str | None":
    """cims-ha wrapper 절대경로 — _resolve_cims_priv 와 같은 우선순위 패턴.
    sudoers 화이트리스트의 dev dist canonical 이 우선 → install_path 별 ha.json 은 --ha-dir 로 분리.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    cands = [
        "/usr/local/bin/cims-ha",                                # prod canonical
        "/home/nex/work/cims/build/dist/agent/bin/cims-ha",      # dev dist (sudoers 등재)
        os.path.join(here, "bin", "cims-ha"),                    # install_path 의 local copy
    ]
    for p in cands:
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return None


def _resolve_cims_priv() -> "str | None":
    """cims-priv wrapper 절대경로 — sudoers 화이트리스트와 일치해야 호출 가능.

    우선순위: prod canonical → dev dist canonical (sudoers 등재 경로) → install_path 의 local copy.
    dev 환경에서는 install_path 의 cims-priv 가 sudoers 에 없으므로 dist canonical 이 우선.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    cands = [
        "/usr/local/bin/cims-priv",                              # prod canonical
        "/home/nex/work/cims/build/dist/agent/bin/cims-priv",    # dev dist (sudoers 등재)
        os.path.join(here, "bin", "cims-priv"),                  # install_path 의 local copy (prod 의 /opt/...)
        os.path.join(here, "cims-priv"),                         # legacy 단일 파일 layout
    ]
    for p in cands:
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return None


def job_apply_ip_config(params: dict) -> tuple:
    """service_ip_rows[] / routes[] → ip-add/ip-del/route-add/route-del 분기 실행.

    Params:
      - service_ip_rows: [{op:'add'|'del', iface, ip, mask, slot?}, ...]
                        op 미지정 시 'add' (backward compat).
      - routes:         [{op:'add'|'del', dst, via, dev}, ...] (optional)

    안전 정책:
      - secondary IP 만 add — primary 변경 안 함 (mgmt 끊김 방지)
      - lo / CSC 통신 NIC 의 row 는 거부 (자기 단절 방지)
      - del 은 NIC 의 해당 IP 가 cims-label 갖고 있어야 허용 — 외부에서 부여한
        IP 는 보호 (운영자 명시 의도만 변경 가능)
      - route 의 dev 는 cims-managed iface (label 가진 IP 보유) 여야 변경 허용
      - default route 변경 금지 (cims-priv 단에서 거부)
      - 'already present' / 'not present' 는 정상 (idempotent)

    반환: (rc, stdout, stderr). rc=0 = 모든 row 성공 또는 idempotent skip.
    """
    rows  = params.get("service_ip_rows") or []
    routes = params.get("routes") or []
    if not isinstance(rows, list): rows = []
    if not isinstance(routes, list): routes = []
    if not rows and not routes:
        return 0, "no rows to apply", ""

    # 현재 NIC 상태 — mgmt/lo 식별 + managed IP 식별 (ip del 검증용).
    cur_ifaces = collect_interfaces()
    mgmt_ifaces: set[str] = set()
    managed_ip: set[tuple[str, str]] = set()   # (iface, ip)
    for i in cur_ifaces:
        if i.get("mgmt"):
            mgmt_ifaces.add(i["name"])
        if i.get("managed") and i.get("ip"):
            managed_ip.add((i["name"], i["ip"]))

    msgs = []
    fail_count = 0

    priv = _resolve_cims_priv()
    if priv is None and (rows or routes):
        return 1, "[FAIL] cims-priv not found", ""

    for r in rows:
        op    = (r.get("op") or "add").lower()
        iface = r.get("iface")
        ip    = r.get("ip")
        mask  = r.get("mask")
        if not iface or not ip or not mask:
            msgs.append(f"skip (incomplete): {r}")
            continue
        if iface == "lo":
            msgs.append(f"[DENY] {iface}: lo 변경 불가"); fail_count += 1; continue
        if iface in mgmt_ifaces:
            msgs.append(f"[DENY] {iface}: mgmt NIC (CSC 통신) 변경 불가 — 자기 단절 방지")
            fail_count += 1; continue
        if op not in ("add", "del"):
            msgs.append(f"[DENY] {iface} {ip}: op '{op}' 미지원"); fail_count += 1; continue
        if op == "del" and (iface, ip) not in managed_ip:
            msgs.append(f"[DENY] {iface} -= {ip}: 외부 IP (cims-label 없음) — 보호")
            fail_count += 1; continue
        cidr = f"{ip}/{mask}"
        verb = "ip-add" if op == "add" else "ip-del"
        try:
            res = subprocess.run(["sudo", "-n", priv, verb, iface, cidr],
                                 capture_output=True, text=True, timeout=10)
            sym = "+=" if op == "add" else "-="
            if res.returncode == 0:
                combined = (res.stdout or "") + (res.stderr or "")
                if "already present" in combined or "not present" in combined:
                    msgs.append(f"[SKIP] {iface} {sym} {cidr}")
                else:
                    msgs.append(f"[OK]   {iface} {sym} {cidr}")
            else:
                fail_count += 1
                err = (res.stderr or res.stdout or "").strip()[-200:]
                msgs.append(f"[FAIL] {iface} {sym} {cidr}: rc={res.returncode} err={err}")
        except Exception as e:
            fail_count += 1
            msgs.append(f"[FAIL] {iface} {op} {cidr}: {e}")

    for r in routes:
        op  = (r.get("op") or "add").lower()
        dst = r.get("dst")
        via = r.get("via")
        dev_ = r.get("dev")
        if not dst or not via or not dev_:
            msgs.append(f"skip route (incomplete): {r}"); continue
        if op not in ("add", "del"):
            msgs.append(f"[DENY] route {dst}: op '{op}' 미지원"); fail_count += 1; continue
        # mgmt NIC 의 route 는 거부 (CSC 통신 단절 방지). 그 외 모든 NIC 의 모든 dst (default 포함) 허용.
        if dev_ == "lo":
            msgs.append(f"[DENY] route {dst} dev {dev_}: lo 변경 불가"); fail_count += 1; continue
        if dev_ in mgmt_ifaces:
            msgs.append(f"[DENY] route {dst} dev {dev_}: mgmt NIC — 자기 단절 방지로 거부")
            fail_count += 1; continue
        verb = "route-add" if op == "add" else "route-del"
        try:
            res = subprocess.run(["sudo", "-n", priv, verb, dst, via, dev_],
                                 capture_output=True, text=True, timeout=10)
            sym = "+=" if op == "add" else "-="
            if res.returncode == 0:
                combined = (res.stdout or "") + (res.stderr or "")
                if "already present" in combined or "not present" in combined:
                    msgs.append(f"[SKIP] route {sym} {dst} via {via} dev {dev_}")
                else:
                    msgs.append(f"[OK]   route {sym} {dst} via {via} dev {dev_}")
            else:
                fail_count += 1
                err = (res.stderr or res.stdout or "").strip()[-200:]
                msgs.append(f"[FAIL] route {sym} {dst}: rc={res.returncode} err={err}")
        except Exception as e:
            fail_count += 1
            msgs.append(f"[FAIL] route {op} {dst}: {e}")

    rc = 0 if fail_count == 0 else 1
    # 적용 후 현재 cims-managed IP/route 를 로컬 스냅샷 — 부팅 시 reapply_managed_ips 가 복원.
    _snapshot_managed_ips()
    return rc, "\n".join(msgs), ""


def job_apply_mounts(params: dict) -> tuple:
    """mounts[] → cims-priv mount-add/mount-del. fstab 에 기록되어 재부팅에도 유지(OS 자동 마운트).

    Params: mounts: [{op:'add'|'del', fstype, source, target, options?}, ...]
    네트워크 FS 는 cims-priv 가 _netdev,nofail 강제 — 마운트 실패가 부팅을 막지 않음.
    """
    mounts = params.get("mounts") or []
    if not isinstance(mounts, list) or not mounts:
        return 0, "no mounts to apply", ""
    priv = _resolve_cims_priv()
    if priv is None:
        return 1, "[FAIL] cims-priv not found", ""
    msgs = []
    fail = 0
    for m in mounts:
        op     = (m.get("op") or "add").lower()
        target = (m.get("target") or "").strip()
        if not target:
            msgs.append(f"skip (no target): {m}"); continue
        if op == "add":
            fstype  = (m.get("fstype") or "").strip()
            source  = (m.get("source") or "").strip()
            options = (m.get("options") or "defaults").strip()
            if not fstype or not source:
                msgs.append(f"[DENY] {target}: fstype/source 필요"); fail += 1; continue
            cmd = ["sudo", "-n", priv, "mount-add", fstype, source, target, options]
        elif op == "del":
            cmd = ["sudo", "-n", priv, "mount-del", target]
        else:
            msgs.append(f"[DENY] {target}: op '{op}' 미지원"); fail += 1; continue
        try:
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=40)
            out = ((res.stdout or "") + (res.stderr or "")).strip()
            if res.returncode == 0:
                msgs.append(f"[OK]   {op} {target}: {out}")
            else:
                fail += 1
                msgs.append(f"[FAIL] {op} {target}: rc={res.returncode} {out[-200:]}")
        except Exception as e:
            fail += 1
            msgs.append(f"[FAIL] {op} {target}: {e}")
    return (0 if fail == 0 else 1), "\n".join(msgs), ""


# ──────────────────────────────────────────────────────────────
#  Service IP 영속 — agent-managed (cims-label '<iface>:cims') IP 는 cims-priv
#  ip-add (런타임 ip addr) 라 재부팅에 소실된다. OAM 가 부팅 직후 unreachable 일 수
#  있으므로(실제 사례: reboot 후 OAM 미기동 → csp DB IP 10.0.1.45 소실 → 전체 장애),
#  마지막 적용 상태를 로컬에 스냅샷하고 부팅 시 자력 재적용한다 (OAM 연결 무관).
# ──────────────────────────────────────────────────────────────
_MANAGED_IPS_FILE = os.path.join(os.path.dirname(_AGENT_DIR), "run", "managed_ips.json")

def _snapshot_managed_ips() -> None:
    """현재 cims-managed IP + managed route 를 로컬 저장 (desired-state)."""
    try:
        ips = [{"iface": i["name"], "ip": i["ip"], "mask": i.get("mask") or 24}
               for i in collect_interfaces() if i.get("managed") and i.get("ip")]
        routes = [{"dst": r["dst"], "via": r.get("via") or "", "dev": r.get("dev") or ""}
                  for r in collect_routes() if r.get("managed")]
        os.makedirs(os.path.dirname(_MANAGED_IPS_FILE), exist_ok=True)
        with open(_MANAGED_IPS_FILE, "w", encoding="utf-8") as f:
            json.dump({"ips": ips, "routes": routes}, f, ensure_ascii=False)
    except Exception as e:
        print(f"[agent][ip] managed_ips snapshot 실패: {e}", flush=True)

def reapply_managed_ips() -> None:
    """부팅 1회 — 저장된 cims-managed IP/route 재적용 (idempotent, OAM 무관).

    스냅샷 파일이 없으면(이 기능 최초 도입/신규 호스트) 현재 상태를 시드 저장만 하고
    재적용은 생략 — 현재 IP 는 이미 올라와 있고, 이후 재부팅부터 복원 대상이 된다.
    """
    if not os.path.exists(_MANAGED_IPS_FILE):
        _snapshot_managed_ips()
        print("[agent][ip] managed_ips seed snapshot 생성 (이후 부팅부터 복원)", flush=True)
        return
    try:
        with open(_MANAGED_IPS_FILE, encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:
        print(f"[agent][ip] managed_ips.json 읽기 실패: {e}", flush=True)
        return
    ips    = data.get("ips") or []
    routes = data.get("routes") or []
    if not ips and not routes:
        return
    rc, _out, _err = job_apply_ip_config({
        "service_ip_rows": [{**x, "op": "add"} for x in ips],
        "routes":          [{**r, "op": "add"} for r in routes],
    })
    print(f"[agent][ip] boot reapply — {len(ips)} ip / {len(routes)} route, rc={rc}", flush=True)


# ──────────────────────────────────────────────────────────────
#  네트워크 튜닝 — 서버별 RPS(rps_cpus) + sysctl(net.core.*) 적용.
#  sysctl 은 /etc/sysctl.d/99-cims-net-tuning.conf 로 영속(부팅 시 systemd 적용)이지만
#  RPS(rps_cpus)는 sysfs 라 재부팅에 소실 → managed_ips 와 동일하게 스냅샷 후 부팅 재적용.
#  근거: 단일 NIC 큐 + RPS off 시 RX softirq 가 IRQ 코어 1개에 집중 → 고RTP 시 ksoftirqd
#  포화 → 네트워크 stall. RPS 로 softirq 를 여러 코어로 분산.
# ──────────────────────────────────────────────────────────────
_NET_TUNING_FILE = os.path.join(os.path.dirname(_AGENT_DIR), "run", "net_tuning.json")

def job_apply_net_tuning(params: dict) -> tuple:
    """서버별 네트워크 튜닝 적용. sysctl 은 영속(sysctl.d), RPS 는 스냅샷+부팅 재적용.

    Params: {
      "sysctl": {"net.core.netdev_max_backlog": 5000, "net.core.netdev_budget": 600, ...},
      "rps":    [{"iface": "ens4", "cpus": "ff"}, ...]   # cpus = 16진 비트마스크, "0"=비활성
    }
    """
    sysctl = params.get("sysctl") or {}
    rps    = params.get("rps") or []
    priv = _resolve_cims_priv()
    if priv is None:
        return 1, "[FAIL] cims-priv not found", ""
    msgs = []; fail = 0
    for key, val in (sysctl.items() if isinstance(sysctl, dict) else []):
        cmd = ["sudo", "-n", priv, "net-sysctl", str(key), str(val)]
        try:
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
            out = ((res.stdout or "") + (res.stderr or "")).strip()
            if res.returncode == 0: msgs.append(f"[OK]   sysctl {key}={val}: {out}")
            else: fail += 1; msgs.append(f"[FAIL] sysctl {key}={val}: rc={res.returncode} {out[-200:]}")
        except Exception as e:
            fail += 1; msgs.append(f"[FAIL] sysctl {key}: {e}")
    for r in (rps if isinstance(rps, list) else []):
        iface = (r.get("iface") or "").strip(); cpus = str(r.get("cpus") or "").strip()
        if not iface or not cpus:
            msgs.append(f"[DENY] rps {r}: iface/cpus 필요"); fail += 1; continue
        cmd = ["sudo", "-n", priv, "net-rps", iface, cpus]
        try:
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
            out = ((res.stdout or "") + (res.stderr or "")).strip()
            if res.returncode == 0: msgs.append(f"[OK]   rps {iface}={cpus}: {out}")
            else: fail += 1; msgs.append(f"[FAIL] rps {iface}={cpus}: rc={res.returncode} {out[-200:]}")
        except Exception as e:
            fail += 1; msgs.append(f"[FAIL] rps {iface}: {e}")
    # 성공 시 desired-state 스냅샷 (부팅 RPS 재적용용)
    if fail == 0:
        _snapshot_net_tuning(params)
    return (0 if fail == 0 else 1), "\n".join(msgs) or "no net tuning to apply", ""

def _snapshot_net_tuning(params: dict) -> None:
    try:
        os.makedirs(os.path.dirname(_NET_TUNING_FILE), exist_ok=True)
        with open(_NET_TUNING_FILE, "w", encoding="utf-8") as f:
            json.dump({"sysctl": params.get("sysctl") or {}, "rps": params.get("rps") or []},
                      f, ensure_ascii=False)
    except Exception as e:
        print(f"[agent][net] net_tuning snapshot 실패: {e}", flush=True)

def ensure_base_deps() -> None:
    """vendor 동봉 deb(keepalived·nfs·lib류)를 전 노드에 균일 설치(air-gapped).
    원칙: 설치는 모든 노드 동일, 실행(서비스 기동)만 config(역할)가 제어 — 기능 미사용
    노드의 의존성 누락(예: media 서버 libmnl.so.0 없어 ip 깨짐) 류 버그 원천 차단.
    cims-priv 가 idempotent(이미 정상이면 skip)하므로 부팅 시 1회 호출(정상 노드 no-op).
    collect_interfaces/route + ip-add 의 전제라 IP 복원보다 먼저 수행."""
    priv = _resolve_cims_priv()
    if priv is None:
        return
    try:
        res = subprocess.run(["sudo", "-n", priv, "ensure-base-deps"],
                             capture_output=True, text=True, timeout=120)
        out = ((res.stdout or "") + (res.stderr or "")).strip()
        print(f"[agent][deps] ensure-base-deps rc={res.returncode} {out[:200]}", flush=True)
    except Exception as e:
        print(f"[agent][deps] ensure-base-deps 실패: {e}", flush=True)

def reapply_net_tuning() -> None:
    """부팅 1회 — 저장된 RPS 재적용 (sysctl 은 sysctl.d 가 이미 부팅 시 적용).

    스냅샷 없으면(최초 도입) skip. RPS 만 재적용(rps_cpus 는 sysfs 라 휘발).
    """
    if not os.path.exists(_NET_TUNING_FILE):
        return
    try:
        with open(_NET_TUNING_FILE, encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:
        print(f"[agent][net] net_tuning.json 읽기 실패: {e}", flush=True)
        return
    rps = data.get("rps") or []
    if not rps:
        return
    rc, _out, _err = job_apply_net_tuning({"rps": rps})   # sysctl 은 sysctl.d 가 처리 → RPS 만
    print(f"[agent][net] boot reapply RPS — {len(rps)} iface, rc={rc}", flush=True)


# ──────────────────────────────────────────────────────────────
#  모듈 감독 (watchdog) — 죽은 모듈 auto-restart
#
#  desired-state = supervised.json { module: install_path }.
#    - job start/restart 성공 → 등록, stop/uninstall → 해제 (의도적 stop 존중).
#    - agent 기동 시 install_path/run/*.pid (= agent-managed start 표식) 에서 seed
#      (watchdog 도입 이전부터 떠 있던 모듈 편입. build/dist 등 비-agent 인스턴스는
#       이 run/ 에 pid 파일이 없어 자동 제외).
#  watchdog tick (heartbeat 루프, OAM 연결 무관 로컬):
#    supervised 모듈이 pgrep 으로 안 잡히면 cims-svc start 재시작. 지수 backoff
#    (5→10→…→300s) 로 crash-loop 폭주 방지, 정상 감지 시 backoff 리셋.
#  CIMS_AGENT_NO_SUPERVISE=1 로 비활성화.
# ──────────────────────────────────────────────────────────────

_SUPERVISE_FILE = os.path.join(os.path.dirname(_AGENT_DIR), "run", "supervised.json")
_WATCHDOG_BACKOFF: dict = {}     # module -> {"ts": float, "fails": int}
SUPERVISE_INTERVAL_SEC = 10


def _find_cims_svc(install_path: str):
    for c in (os.path.join(install_path, "agent", "bin", "cims-svc"),
              os.path.join(_AGENT_DIR, "bin", "cims-svc"),
              "/opt/cims-agent/agent/bin/cims-svc"):
        if os.path.isfile(c):
            return c
    return None


def _run_cims_svc(install_path: str, action: str, svc: str, timeout: int = 60) -> tuple:
    """cims-svc <action> <svc> 실행. CIMS_DIST_DIR + CIMS_PYTHON(에이전트 인터프리터,
    private 호스트의 python3-PATH 부재 대비) 전달."""
    script = _find_cims_svc(install_path)
    if not script:
        return 1, "", f"cims-svc not found (install_path={install_path}, agent_dir={_AGENT_DIR})"
    env = dict(os.environ)
    env["CIMS_DIST_DIR"] = install_path
    env["CIMS_PYTHON"] = sys.executable
    try:
        res = subprocess.run([script, action, svc], capture_output=True, text=True,
                             timeout=timeout, cwd=install_path, env=env)
        return res.returncode, res.stdout[-4000:], res.stderr[-2000:]
    except Exception as e:
        return 2, "", f"exec failed: {e}"


def _load_supervised() -> dict:
    try:
        with open(_SUPERVISE_FILE) as f:
            d = json.load(f)
            return d if isinstance(d, dict) else {}
    except Exception:
        return {}


def _save_supervised(d: dict) -> None:
    try:
        os.makedirs(os.path.dirname(_SUPERVISE_FILE), exist_ok=True)
        tmp = _SUPERVISE_FILE + ".tmp"
        with open(tmp, "w") as f:
            json.dump(d, f)
        os.replace(tmp, _SUPERVISE_FILE)
    except Exception as e:
        print(f"[agent][watchdog] supervised 저장 실패: {e}", flush=True)


def _mark_supervised(svc: str, install_path: str) -> None:
    if not svc or svc in _NON_DAEMON_MODULES:
        return
    d = _load_supervised()
    if d.get(svc) != install_path:
        d[svc] = install_path
        _save_supervised(d)
        print(f"[agent][watchdog] 감독 등록: {svc} ({install_path})", flush=True)


def _unmark_supervised(svc: str) -> None:
    if not svc:
        return
    d = _load_supervised()
    if svc in d:
        d.pop(svc, None)
        _WATCHDOG_BACKOFF.pop(svc, None)
        _save_supervised(d)
        print(f"[agent][watchdog] 감독 해제: {svc}", flush=True)


def _seed_supervised_from_pidfiles() -> None:
    """agent 기동 시 1회 — install_path/run/*.pid 의 모듈을 감독 집합에 편입.
    pid 파일은 agent 의 cims-svc start 만 만들므로 (build/dist 등 별도 인스턴스는
    자기 run/ 사용) 정확히 agent-managed 모듈만 잡힌다."""
    install_path = os.path.dirname(_AGENT_DIR)   # 예: /opt/cims-agent
    pid_dir = os.path.join(install_path, "run")
    if not os.path.isdir(pid_dir):
        return
    sup = _load_supervised()
    changed = False
    try:
        for fn in os.listdir(pid_dir):
            if not fn.endswith(".pid"):
                continue
            svc = fn[:-4]
            if svc in _NON_DAEMON_MODULES or svc in sup:
                continue
            sup[svc] = install_path
            changed = True
    except Exception:
        return
    if changed:
        _save_supervised(sup)
        print(f"[agent][watchdog] pid 파일에서 감독 seed: {sorted(sup)}", flush=True)


def supervise_tick() -> None:
    """supervised 모듈 중 죽은 것(pgrep 미검출)을 backoff 와 함께 재시작."""
    if os.environ.get("CIMS_AGENT_NO_SUPERVISE"):
        return
    sup = _load_supervised()
    if not sup:
        return
    now = time.time()
    for svc, install_path in list(sup.items()):
        if svc in _NON_DAEMON_MODULES:
            continue
        if _pgrep_module(svc):
            _WATCHDOG_BACKOFF.pop(svc, None)     # 정상 — backoff 리셋
            continue
        st = _WATCHDOG_BACKOFF.setdefault(svc, {"ts": 0.0, "fails": 0})
        backoff = min(300, 5 * (2 ** st["fails"]))
        if now - st["ts"] < backoff:
            continue
        st["ts"] = now
        st["fails"] += 1
        nxt = min(300, 5 * (2 ** st["fails"]))
        print(f"[agent][watchdog] '{svc}' 다운 감지 — 재시작 (시도 {st['fails']}, 다음 backoff {nxt}s)", flush=True)
        rc, out, err = _run_cims_svc(install_path, "start", svc)
        tail = (err or out or "").strip().replace("\n", " ")[-160:]
        print(f"[agent][watchdog] '{svc}' start rc={rc} {tail}", flush=True)


def job_process_control(params: dict, job_type: str) -> tuple:
    """start/stop/restart — install_path/agent/bin/cims-svc 를 이용해 수행
    (Phase 1.B+, cims.sh 운영 명령 제거).
    cims-svc 에 CIMS_DIST_DIR=install_path 환경변수 전달 → cims-svc 가 install_path
    기준으로 DIST_DIR 결정 (install_path 의 csc/console 시작).
    """
    # 버전 전환 레이스 방어: deployment 레코드가 stale 이어도 설치 완료된
    # 버전 디렉토리가 있으면 그쪽을 실효 경로로 사용.
    install_path = _runtime_install_path(params)
    svc = (params.get("process_name") or params.get("service_kind") or "").lower()
    # Phase 4 fix: svc 빈 경우 명시 에러. cims-svc 가 svc 인자 없이 호출되면
    # default 'all' fallback → 단일 모듈 install 환경에서 cmp/csp 못 찾아 fail.
    # deployment 의 process_name 필드 누락이 원인 — server 측에서 자동 추론 도입
    # (agents.py _create_deployment) 했지만, agent 측에서도 safety net.
    if not svc:
        return 1, "", (
            "process_name 누락 — deployment.process_name 필드 필수 "
            f"(install_path={install_path}, job_type={job_type})"
        )
    # 버전 단위 설치 전환: 현재 감독(supervised) 중인 인스턴스가 다른 경로
    # (구 버전 디렉토리 / legacy 평탄 설치) 에서 떠 있으면 먼저 그 경로에서 stop.
    # 미수행 시 신 버전 start 가 포트 바인드 충돌로 fail-fast (구 프로세스는
    # exe 경로가 달라 lifecycle 의 kill_stray/own-listener 정리에 안 잡힘).
    prev_note = ""
    prev_path = _load_supervised().get(svc)
    if prev_path and prev_path != install_path and os.path.isdir(prev_path):
        prc, _pout, perr = _run_cims_svc(prev_path, "stop", svc)
        prev_note = f" (prev-stop {prev_path} rc={prc}{' ' + perr.strip()[:120] if prc else ''})"
        if job_type == "stop":
            _unmark_supervised(svc)
    # 우선순위:
    #  1) install_path/agent/bin/cims-svc — 모듈 자체에 운영 도구를 ship 하는 경우 (구식)
    #  2) _AGENT_DIR/bin/cims-svc — 일반 케이스. 에이전트가 자기 옆 bin/cims-svc 사용
    #     (install-agent.sh 가 /opt/cims-agent/agent/bin/ 에 둠).
    #  3) /opt/cims-agent/agent/bin/cims-svc — agent 가 다른 곳에서 실행되는 경우 명시 fallback
    rc, out, err = _run_cims_svc(install_path, job_type, svc)
    if prev_note:
        out = (out or "") + prev_note
    # 모듈 감독 desired-state 갱신 — start/restart 성공 → 감독 등록, stop → 해제.
    # (watchdog 가 supervised 집합의 죽은 모듈을 auto-restart)
    if rc == 0:
        if job_type in ("start", "restart"):
            _mark_supervised(svc, install_path)
        elif job_type == "stop":
            _unmark_supervised(svc)
    return rc, out, err


def job_health_check(params: dict) -> tuple:
    """간단한 헬스 체크 — 설정된 포트에 TCP/UDP 연결 시도.

    포트 결정 우선순위:
      1) params.port (정수, 단일 포트 override)
      2) params.config 의 scalar overlay — 모듈별 키에서 추출
         - csc/console: Server.Port / Port / ServerPort (+ console은 nginx PORT)
         - csp: Setup.Sip.UdpPort
         - cmp: ServerPort / Setup.Listen.ControlPort
      3) 기본값 (하드코딩)
    """
    svc = (params.get("process_name") or params.get("service_kind") or "").lower()
    cfg = params.get("config") or {}
    # flat dot-path overlay 지원: {"Server.Port": 4430} 도 해석
    def _flat(key: str):
        if key in cfg: return cfg[key]
        cur = cfg
        for p in key.split("."):
            if not isinstance(cur, dict) or p not in cur: return None
            cur = cur[p]
        return cur
    override = params.get("port")
    if override is None:
        if svc in ("csc", "tb-csc"):
            override = _flat("Server.Port") or _flat("Port")
        elif svc == "console":
            override = _flat("Port") or _flat("ConsolePort")
        elif svc == "csp":
            override = _flat("Setup.Sip.UdpPort")
        elif svc == "cmp":
            override = _flat("ServerPort") or _flat("Setup.Listen.ControlPort")
        elif svc == "cwrtc":
            override = _flat("Port")
        elif svc == "phone":
            override = _flat("Port")
    probes_default = {
        "csp":     [("udp", 5060)],
        "cmp":     [("udp", 9000)],
        "csc":     [("tcp", 4420)],
        "cwrtc":   [("tcp", 8080)],
        "console": [("tcp", 3001)],
        "phone":   [("tcp", 3000)],
    }.get(svc, [])
    if override and probes_default:
        proto = probes_default[0][0]
        probes = [(proto, int(override))]
    else:
        probes = probes_default
    results = []
    for proto, port in probes:
        s = socket.socket(socket.AF_INET,
                          socket.SOCK_DGRAM if proto == "udp" else socket.SOCK_STREAM)
        s.settimeout(2)
        try:
            if proto == "tcp":
                s.connect(("127.0.0.1", port))
                results.append(f"tcp:{port}=open")
            else:
                # UDP: 소켓 bind 시도해서 포트 사용 여부 확인
                try:
                    s.bind(("127.0.0.1", port))
                    results.append(f"udp:{port}=free")   # bind 성공 = 프로세스 없음
                except OSError:
                    results.append(f"udp:{port}=inuse")  # 사용 중 = 프로세스 있음
        except Exception as e:
            results.append(f"{proto}:{port}=fail({e})")
        finally:
            s.close()
    return 0, " ".join(results), ""


def job_upgrade_agent(oam_url: str, session_token: str, agent_name: str) -> tuple:
    """install-agent.sh --update-only 호출 — bundle 전체 + sub-script 일괄 교체.
    INSTALL_DIR 은 cims_agent.py 위치 기반 추론 (<INSTALL_DIR>/agent/cims_agent.py).
    성공 시 호출자가 self-exec → systemd 재기동."""
    install_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src_url = f"{oam_url}/install-agent.sh"
    status, data, meta = http_get_binary(src_url, {"X-Agent-Token": session_token})
    if status != 200 or not data or len(data) < 1024:
        return 1, "", f"download install-agent.sh failed status={status} size={len(data) if data else 0}"
    installer_path = f"/tmp/cims-install-agent-update.{os.getpid()}.sh"
    try:
        with open(installer_path, "wb") as f:
            f.write(data)
        os.chmod(installer_path, 0o755)
        cmd = ["bash", installer_path, "--update-only",
               "--csc-url",     oam_url,
               "--name",        agent_name,
               "--install-dir", install_dir]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        out = p.stdout or ""
        err = p.stderr or ""
        if p.returncode != 0:
            return (p.returncode or 1), out, err or f"installer rc={p.returncode}"
        return 0, out + f"\n[upgrade] install-agent.sh --update-only OK (install_dir={install_dir})", err
    except Exception as e:
        return 2, "", f"upgrade failed: {e}\n{traceback.format_exc()}"
    finally:
        try: os.unlink(installer_path)
        except Exception: pass


# ──────────────────────────────────────────────────────────────
#  Sync REST server (CSC → Agent 단일 방향 요청/응답)
#
#  Endpoints (인증: X-Agent-Token == state.session_token):
#    GET  /health
#    GET  /collection?install_path=<path>&name=<name>
#    PUT  /collection?install_path=<path>&name=<name>  body={records:[...]}
#    POST /signal?install_path=<path>&sig=usr1|hup
# ──────────────────────────────────────────────────────────────

def _ensure_self_signed_cert(state_dir: str) -> tuple:
    """state_dir/agent.crt, agent.key 생성 (이미 있으면 그대로 반환)."""
    crt = os.path.join(state_dir, "agent.crt")
    key = os.path.join(state_dir, "agent.key")
    if os.path.isfile(crt) and os.path.isfile(key):
        return crt, key
    hostname = socket.gethostname() or "cims-agent"
    # openssl 으로 RSA 2048 + self-signed 10년
    cmd = ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
           "-days", "3650", "-subj", f"/CN={hostname}",
           "-keyout", key, "-out", crt]
    subprocess.run(cmd, check=True, capture_output=True)
    os.chmod(key, 0o600)
    return crt, key


def _read_jsonl(path: str) -> list:
    if not os.path.isfile(path): return []
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line: continue
            try: out.append(json.loads(line))
            except Exception: pass
    return out


def _write_jsonl_atomic(path: str, records: list) -> int:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        for r in records:
            f.write(json.dumps(r, ensure_ascii=False))
            f.write("\n")
    os.replace(tmp, path)
    return len(records)


def _signal_process(install_path: str, sig_name: str, pkg_subdir: str = "") -> tuple:
    """install_path 의 *.pid 를 읽어 SIGUSR1(기본)/SIGHUP 전송.

    탐색 순서: pkg_subdir/run/ → install_path/run/ → install_path/ (각각 *.pid 매칭).
    multi-pkg agent 의 경우 pkg_subdir 가 명시되면 해당 변종의 pid 만 신호 받음.
    """
    sig = signal.SIGUSR1 if sig_name == "usr1" else signal.SIGHUP
    candidates = []
    if pkg_subdir:
        candidates.append(os.path.join(install_path, pkg_subdir, "run"))
    candidates.extend([os.path.join(install_path, "run"), install_path])
    pid_dir = next((d for d in candidates if os.path.isdir(d)), install_path)
    found = []
    for n in os.listdir(pid_dir):
        if n.endswith(".pid"):
            try:
                with open(os.path.join(pid_dir, n)) as f:
                    pid = int(f.read().strip())
                os.kill(pid, sig)
                found.append(pid)
            except Exception:
                pass
    return (0, found) if found else (1, [])


class _Handler(http.server.BaseHTTPRequestHandler):
    # 서브클래스에서 _state 에 AgentState 주입
    _state: "AgentState" = None     # type: ignore

    def _respond(self, code: int, body):
        payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _auth_ok(self) -> bool:
        tok = self.headers.get("X-Agent-Token") or ""
        return bool(tok) and tok == (self._state.session_token or "")

    def log_message(self, fmt, *args):   # noqa: N802
        # stderr 소음 줄이기
        return

    def _parse_query(self):
        u = urllib.parse.urlparse(self.path)
        return u.path, urllib.parse.parse_qs(u.query)

    def _read_body_json(self) -> dict:
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0: return {}
        try: return json.loads(self.rfile.read(n).decode("utf-8"))
        except Exception: return {}

    # GET
    def do_GET(self):  # noqa: N802
        if not self._auth_ok():
            return self._respond(401, {"error": "unauthorized"})
        path, q = self._parse_query()
        if path == "/health":
            return self._respond(200, {"ok": True, "agent_id": self._state.agent_id,
                                        "version": AGENT_VERSION})
        if path == "/health-check":
            scope = (q.get("scope") or ["all"])[0]
            return self._respond(200, run_health_check(scope))
        if path == "/collection":
            install_path = (q.get("install_path") or [""])[0]
            name = (q.get("name") or [""])[0]
            if not install_path or not name:
                return self._respond(400, {"error": "install_path and name required"})
            fpath = os.path.join(install_path, "config", f"{name}.jsonl")
            records = _read_jsonl(fpath)
            return self._respond(200, {"records": records, "file": fpath})
        return self._respond(404, {"error": "not_found"})

    # PUT
    def do_PUT(self):  # noqa: N802
        if not self._auth_ok():
            return self._respond(401, {"error": "unauthorized"})
        path, q = self._parse_query()
        if path == "/collection":
            install_path = (q.get("install_path") or [""])[0]
            name = (q.get("name") or [""])[0]
            if not install_path or not name:
                return self._respond(400, {"error": "install_path and name required"})
            body = self._read_body_json()
            records = body.get("records")
            if not isinstance(records, list):
                return self._respond(400, {"error": "records must be array"})
            fpath = os.path.join(install_path, "config", f"{name}.jsonl")
            n = _write_jsonl_atomic(fpath, records)
            # 자동 reload 시그널 (옵션)
            signaled = []
            if body.get("signal", True):
                rc, pids = _signal_process(install_path, "usr1")
                signaled = pids
            return self._respond(200, {"ok": True, "count": n, "file": fpath,
                                        "signaled": signaled})
        return self._respond(404, {"error": "not_found"})

    # POST
    def do_POST(self):  # noqa: N802
        if not self._auth_ok():
            return self._respond(401, {"error": "unauthorized"})
        path, q = self._parse_query()
        if path == "/signal":
            install_path = (q.get("install_path") or [""])[0]
            sig = (q.get("sig") or ["usr1"])[0]
            if not install_path:
                return self._respond(400, {"error": "install_path required"})
            rc, pids = _signal_process(install_path, sig)
            return self._respond(200 if rc == 0 else 404,
                                  {"ok": rc == 0, "signaled": pids})
        if path == "/apply-ip-config":
            body = self._read_body_json()
            rows = body.get("service_ip_rows") or []
            routes = body.get("routes") or []
            if not isinstance(rows, list):
                return self._respond(400, {"error": "service_ip_rows must be array"})
            if not isinstance(routes, list):
                return self._respond(400, {"error": "routes must be array"})
            rc, out, err = job_apply_ip_config({"service_ip_rows": rows, "routes": routes})
            return self._respond(200,
                                  {"ok": rc == 0, "rc": rc,
                                   "stdout": out, "stderr": err,
                                   "rows": len(rows), "routes": len(routes)})
        if path == "/apply-mounts":
            body = self._read_body_json()
            mounts = body.get("mounts") or []
            if not isinstance(mounts, list):
                return self._respond(400, {"error": "mounts must be array"})
            rc, out, err = job_apply_mounts({"mounts": mounts})
            return self._respond(200,
                                  {"ok": rc == 0, "rc": rc,
                                   "stdout": out, "stderr": err, "mounts": len(mounts)})
        return self._respond(404, {"error": "not_found"})


class _ThreadingHTTPSServer(http.server.ThreadingHTTPServer):
    daemon_threads = True


def start_sync_server(state: AgentState, state_dir: str, port: int) -> int:
    """별도 스레드에서 HTTPS 서버 기동. 성공 시 실제 바인딩 포트 반환.

    enroll 응답에 mTLS cert 가 포함되어 있으면 해당 cert 로 CERT_REQUIRED 모드 동작.
    없으면 self-signed 로 X-Agent-Token 헤더 인증만 수행.
    """
    mtls_crt = os.path.join(state_dir, "agent_mtls.crt")
    mtls_key = os.path.join(state_dir, "agent_mtls.key")
    mtls_ca  = os.path.join(state_dir, "agent_mtls_ca.crt")
    use_mtls = os.path.isfile(mtls_crt) and os.path.isfile(mtls_key) and os.path.isfile(mtls_ca)

    if use_mtls:
        crt, key = mtls_crt, mtls_key
    else:
        crt, key = _ensure_self_signed_cert(state_dir)

    class H(_Handler):
        pass
    H._state = state   # type: ignore

    srv = _ThreadingHTTPSServer(("0.0.0.0", port), H)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=crt, keyfile=key)
    if use_mtls:
        ctx.verify_mode = ssl.CERT_REQUIRED
        ctx.load_verify_locations(mtls_ca)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    bound_port = srv.server_address[1]

    t = threading.Thread(target=srv.serve_forever, daemon=True, name="agent-sync")
    t.start()
    mode = "mTLS" if use_mtls else "self-signed"
    print(f"[agent] sync REST listening on 0.0.0.0:{bound_port} (HTTPS, {mode})", flush=True)
    return bound_port


def execute_job(job: dict, oam_url: str, session_token: str, agent_name: str) -> dict:
    jt = job["type"]
    params = job.get("params") or {}
    try:
        if jt == "install":
            rc, out, err = job_install(params, oam_url, session_token)
        elif jt == "upgrade":
            rc, out, err = job_install(params, oam_url, session_token)
        elif jt == "upgrade_agent":
            rc, out, err = job_upgrade_agent(oam_url, session_token, agent_name)
        elif jt == "agent_restart":
            # agent 자체 self-restart. heartbeat loop 가 execv 처리.
            rc, out, err = 0, "agent restart requested — execv self", ""
        elif jt in ("start", "stop", "restart"):
            rc, out, err = job_process_control(params, jt)
        elif jt == "update_config":
            rc, out, err = job_update_config(params, oam_url, session_token)
        elif jt == "sync_config":
            rc, out, err = job_sync_config(params, oam_url, session_token)
        elif jt == "update_ha":
            rc, out, err = job_update_ha(params)
        elif jt == "apply_ip_config":
            rc, out, err = job_apply_ip_config(params)
        elif jt == "apply_mounts":
            rc, out, err = job_apply_mounts(params)
        elif jt == "apply_net_tuning":
            rc, out, err = job_apply_net_tuning(params)
        elif jt == "uninstall":
            install_path = params.get("install_path")
            # 감독 해제 (watchdog 가 재시작하지 않도록)
            _unmark_supervised((params.get("process_name") or params.get("service_kind") or "").lower())
            # 버전 디렉토리(…/<module>/<version>) 면 모듈 루트 전체 제거 —
            # uninstall 은 모듈 deployment 자체의 철거이므로 병렬 버전도 함께.
            module = (params.get("package_name") or "").strip()
            if install_path and module:
                base = install_path.rstrip("/")
                if _VERSION_DIR_RE.match(os.path.basename(base)) and \
                        os.path.basename(os.path.dirname(base)) == module:
                    install_path = os.path.dirname(base)
            # 안전 가드: agent 자신을 포함하는 경로(공유 루트 /opt/cims-agent 등)는
            # rmtree 금지 — legacy 공유 install_path deployment 의 uninstall 이
            # agent/형제 모듈까지 파괴하는 사고 방지. pkg 디렉토리만 제거.
            agent_root = os.path.dirname(_AGENT_DIR)
            if install_path and os.path.isdir(install_path) and \
                    os.path.realpath(install_path) in (os.path.realpath(agent_root),
                                                       os.path.realpath(_AGENT_DIR)):
                sub = os.path.join(install_path, module) if module else ""
                if sub and os.path.isdir(sub):
                    shutil.rmtree(sub, ignore_errors=True)
                    rc, out, err = 0, f"removed {sub} (shared-root guard)", ""
                else:
                    rc, out, err = 1, "", f"refuse rmtree shared root: {install_path}"
            elif install_path and os.path.isdir(install_path):
                shutil.rmtree(install_path, ignore_errors=True)
                rc, out, err = 0, f"removed {install_path}", ""
            else:
                rc, out, err = 0, "nothing to remove", ""
        elif jt == "health_check":
            rc, out, err = job_health_check(params)
        elif jt == "collect_log":
            log_path = params.get("log_path") or "/var/log/cims/csp.log"
            if os.path.isfile(log_path):
                with open(log_path, "rb") as f:
                    f.seek(-65000, 2) if os.path.getsize(log_path) > 65000 else f.seek(0)
                    rc, out, err = 0, f.read().decode(errors="replace"), ""
            else:
                rc, out, err = 1, "", f"log not found: {log_path}"
        else:
            rc, out, err = 2, "", f"unknown_job_type: {jt}"
    except Exception as e:
        rc = 99
        out = ""
        err = f"exception: {e}\n{traceback.format_exc()}"
    return {
        "job_id": job["id"],
        "status": "succeeded" if rc == 0 else "failed",
        "result_code": rc,
        "stdout": out,
        "stderr": err,
    }


# ──────────────────────────────────────────────────────────────
#  Main loop
# ──────────────────────────────────────────────────────────────

def enroll(oam_url: str, enrollment_token: str, state: AgentState, name: str) -> bool:
    info = collect_host_info()
    payload = {"enrollment_token": enrollment_token, **info}
    status, resp = http_post(f"{oam_url}/api/agent/enroll", payload)
    if status != 200 or not resp.get("session_token"):
        print(f"[agent] enroll failed: status={status} body={resp}")
        return False
    state.agent_id = resp["agent_id"]
    state.session_token = resp["session_token"]
    state.name = resp.get("name") or name

    # CSC 가 mTLS cert 를 발급했으면 state_dir 에 저장 (sync REST 서버에서 사용)
    mtls = resp.get("mtls")
    if isinstance(mtls, dict) and mtls.get("server_cert") and mtls.get("server_key"):
        try:
            srv_crt = os.path.join(state.dir, "agent_mtls.crt")
            srv_key = os.path.join(state.dir, "agent_mtls.key")
            ca_crt  = os.path.join(state.dir, "agent_mtls_ca.crt")
            with open(srv_crt, "w") as f: f.write(mtls["server_cert"])
            with open(srv_key, "w") as f: f.write(mtls["server_key"])
            if mtls.get("ca_cert"):
                with open(ca_crt, "w") as f: f.write(mtls["ca_cert"])
            os.chmod(srv_key, 0o600)
            print(f"[agent] mTLS cert installed: {srv_crt}")
        except Exception as e:
            print(f"[agent] mTLS cert save failed: {e}", flush=True)

    state.save()
    print(f"[agent] enrolled: id={state.agent_id} name={state.name} status={resp.get('status')}")
    return True


def rotate_mtls_cert(oam_url: str, state: AgentState) -> bool:
    """CSC 에 새 mTLS cert 발급 요청 → state_dir 에 저장. 성공 시 True.

    발급 성공 후에도 현재 실행 중인 sync REST 서버는 옛 cert 를 메모리에 쥐고 있음.
    호출자(run_loop) 가 프로세스 종료하면 systemd 가 재기동하면서 새 cert 를 읽음.
    """
    status, resp = http_post(f"{oam_url}/api/agent/cert/rotate", {},
                             headers={"X-Agent-Token": state.session_token})
    if status != 200 or not isinstance(resp, dict):
        print(f"[agent] cert rotate failed: status={status} body={resp}", flush=True)
        return False
    mtls = resp.get("mtls") or {}
    if not (mtls.get("server_cert") and mtls.get("server_key")):
        print("[agent] cert rotate: missing fields in response", flush=True)
        return False
    try:
        srv_crt = os.path.join(state.dir, "agent_mtls.crt")
        srv_key = os.path.join(state.dir, "agent_mtls.key")
        ca_crt  = os.path.join(state.dir, "agent_mtls_ca.crt")
        with open(srv_crt, "w") as f: f.write(mtls["server_cert"])
        with open(srv_key, "w") as f: f.write(mtls["server_key"])
        if mtls.get("ca_cert"):
            with open(ca_crt, "w") as f: f.write(mtls["ca_cert"])
        os.chmod(srv_key, 0o600)
        print(f"[agent] mTLS cert rotated: expires_at={resp.get('expires_at','?')}", flush=True)
        return True
    except Exception as e:
        print(f"[agent] cert rotate save failed: {e}", flush=True)
        return False


def run_loop(oam_url: str, state: AgentState, heartbeat_sec: int, metric_sec: int,
             sync_port: int = 0):
    """
    Heartbeat 루프.

    HA 환경 (oam_url 이 VIP_csc 가리킴) 에서 fail-over 가 진행되는 약 3~7초 동안은
    connection refused / timeout 이 발생하므로 짧은 exponential backoff 로 복구 시도.
    정상 회차 sleep 은 heartbeat_sec, 실패 회차는 5s → 10s → 20s → max(heartbeat_sec, 60s).
    """
    # CSC 통신 NIC 식별 — collect_interfaces() 가 mgmt 플래그 부여 시 사용.
    global _MGMT_IP
    _MGMT_IP = detect_mgmt_ip(oam_url)

    _seed_supervised_from_pidfiles()   # 기존 실행 모듈을 감독 집합에 편입 (1회)
    ensure_base_deps()                 # vendor deb 균일 설치(keepalived/nfs/lib) — 실행은 config 제어. ip/네트워크 수집 전제, idempotent
    reapply_managed_ips()              # 재부팅으로 소실된 cims-managed service IP 자력 복원 (1회, OAM 무관)
    reapply_net_tuning()               # 재부팅으로 소실된 RPS(rps_cpus) 자력 복원 (1회, sysctl 은 sysctl.d 가 처리)

    next_metric = 0
    next_supervise = 0
    fail_count = 0
    max_backoff = max(heartbeat_sec, 60)
    while True:
        try:
            hb_body = {
                "interfaces": collect_interfaces(),
                "routes": collect_routes(),
                "mounts": collect_mounts(),
                "agent_version": AGENT_VERSION,
            }
            if sync_port: hb_body["sync_port"] = sync_port
            status, resp = http_post(f"{oam_url}/api/agent/heartbeat", hb_body,
                                     headers={"X-Agent-Token": state.session_token})
            if status == 401:
                print("[agent] session token revoked; exiting")
                return 1
            if status == 200:
                if fail_count > 0:
                    print(f"[agent] heartbeat recovered after {fail_count} failures", flush=True)
                fail_count = 0
                # CSC 가 cert rotation 지시 → 새 cert 받아 저장 후 프로세스 종료 (systemd 재기동)
                if resp.get("cert_rotate"):
                    print("[agent] cert rotation requested by CSC", flush=True)
                    if rotate_mtls_cert(oam_url, state):
                        print("[agent] exiting for systemd restart with new cert", flush=True)
                        return 0

                jobs = resp.get("jobs") or []
                for job in jobs:
                    print(f"[agent] exec job id={job['id']} type={job['type']}", flush=True)
                    result = execute_job(job, oam_url, state.session_token, state.name or "")
                    rep_status, rep_body = http_post(f"{oam_url}/api/agent/report", result,
                                                      headers={"X-Agent-Token": state.session_token})
                    print(f"[agent] report status={rep_status} rc={result['result_code']}", flush=True)
                    # upgrade_agent / agent_restart 성공 시 새 코드 image 로 self-exec.
                    # systemd 환경: execv 가 모든 fd close + 같은 PID 로 새 image 실행 (Restart=always 보다 빠름)
                    # nohup 환경 (no-systemd): execv 가 유일한 재기동 경로 — 부모 shell 이 죽었으므로 외부 monitor 없음
                    if job["type"] in ("upgrade_agent", "agent_restart") and result["result_code"] == 0:
                        action = "upgrade" if job["type"] == "upgrade_agent" else "restart"
                        print(f"[agent] {action} done — execv self", flush=True)
                        try:
                            os.execv(sys.executable, [sys.executable] + sys.argv)
                        except Exception as e:
                            print(f"[agent] execv 실패 ({e}) — exit (외부 supervisor 필요)", flush=True)
                            return 0
            else:
                fail_count += 1

            if time.time() >= next_metric:
                metrics = collect_metrics()
                http_post(f"{oam_url}/api/agent/metric", metrics,
                          headers={"X-Agent-Token": state.session_token})
                next_metric = time.time() + metric_sec
        except Exception as e:
            fail_count += 1
            print(f"[agent] loop error (fail_count={fail_count}): {e}")

        # 모듈 감독 — OAM 연결과 무관하게 로컬에서 죽은 supervised 모듈 재시작
        if time.time() >= next_supervise:
            try:
                supervise_tick()
            except Exception as e:
                print(f"[agent][watchdog] tick error: {e}", flush=True)
            next_supervise = time.time() + SUPERVISE_INTERVAL_SEC

        if fail_count == 0:
            sleep_sec = heartbeat_sec
        else:
            sleep_sec = min(5 * (2 ** (fail_count - 1)), max_backoff)
            print(f"[agent] HA backoff sleep {sleep_sec}s (fail_count={fail_count})", flush=True)
        time.sleep(sleep_sec)


def main():
    ap = argparse.ArgumentParser()
    # Phase 3b: --oam-url 신규. --csc-url 도 deprecated alias 로 받음 (옛 agent 호환).
    ap.add_argument("--oam-url", dest="oam_url")
    ap.add_argument("--csc-url", dest="oam_url")  # deprecated alias
    ap.add_argument("--enrollment-token", default=os.environ.get("CIMS_ENROLLMENT_TOKEN"))
    ap.add_argument("--name", default=socket.gethostname())
    ap.add_argument("--state-dir", default=DEFAULT_STATE_DIR)
    ap.add_argument("--heartbeat-sec", type=int, default=DEFAULT_HEARTBEAT_SEC)
    ap.add_argument("--metric-sec", type=int, default=DEFAULT_METRIC_SEC)
    ap.add_argument("--sync-port", type=int,
                    default=int(os.environ.get("CIMS_AGENT_SYNC_PORT", DEFAULT_SYNC_PORT)),
                    help="동기 REST 서버 포트 (0 = 비활성)")
    ap.add_argument("--enroll-only", action="store_true",
                    help="enrollment 만 수행 후 종료 (heartbeat / sync server 시작 안 함)")
    args = ap.parse_args()
    if not args.oam_url:
        print("[agent] --oam-url (또는 --csc-url) 필수")
        return 1

    state = AgentState(args.state_dir)
    if not state.session_token:
        if not args.enrollment_token:
            print("[agent] first run requires --enrollment-token")
            return 1
        if not enroll(args.oam_url, args.enrollment_token, state, args.name):
            return 2
    else:
        print(f"[agent] resumed: id={state.agent_id} name={state.name}")

    if args.enroll_only:
        print("[agent] enroll-only mode — exiting after enrollment (no heartbeat sent)")
        return 0

    sync_port = 0
    if args.sync_port > 0:
        try:
            sync_port = start_sync_server(state, args.state_dir, args.sync_port)
        except Exception as e:
            print(f"[agent] sync server start failed: {e}", flush=True)

    return run_loop(args.oam_url, state, args.heartbeat_sec, args.metric_sec, sync_port)


if __name__ == "__main__":
    sys.exit(main() or 0)
