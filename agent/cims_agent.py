#!/usr/bin/env python3
"""
CIMS Server Agent (P10)

Usage:
  cims_agent.py \
      --oam-url https://oam.example.com:4419 \
      --state-dir ~/.local/state/cims-agent

On first start (no state):
  - reads CIMS_ENROLLMENT_TOKEN env or --enrollment-token flag
  - POST /api/agent/enroll → receives session_token → saves to state

On subsequent starts:
  - reads session_token from state
  - POST /api/agent/heartbeat every 2s (DEFAULT_HEARTBEAT_SEC, --heartbeat-sec;
    OAM 불통 시 5→10→…→60s 지수 backoff) → receives pending jobs → executes

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
_AGENT_DIR = os.path.dirname(os.path.abspath(__file__))


def _resolve_prefix() -> str:
    """설치 루트(prefix, 예: /opt/cims-agent) 도출 — 레이아웃 비의존.

    버전화 레이아웃에서 agent 는 `<prefix>/agent/current/cims_agent.py` (심볼릭 경유)
    로 기동되므로 `_AGENT_DIR` = `<prefix>/agent/current`. dirname 횟수를 고정하면
    flat(`<prefix>/agent`, dev/legacy)과 어긋나므로, 단계 수가 아니라 **`agent`
    디렉토리 컴포넌트의 부모**를 prefix 로 삼는다 (flat/버전화/current 경유 모두 일치).
      1) CIMS_AGENT_PREFIX (systemd 가 주입) 우선
      2) `_AGENT_DIR` 에서 basename == "agent" 인 조상까지 walk-up → 그 부모
    """
    env = os.environ.get("CIMS_AGENT_PREFIX")
    if env:
        return env
    d = _AGENT_DIR
    while d and d != "/" and os.path.basename(d) != "agent":
        d = os.path.dirname(d)
    return os.path.dirname(d) if os.path.basename(d) == "agent" else os.path.dirname(_AGENT_DIR)


_PREFIX = _resolve_prefix()
# 모듈 설치 루트 결정 우선순위:
#   1) CIMS_AGENT_INSTALL_ROOT 환경변수
#   2) <prefix>/modules — 부트스트랩 oam/console 과 동일 루트라야 csc/oam-svc 가
#      형제 oam/src 를 sys.path glob 으로 찾는다(oam_base_service_split gap1).
DEFAULT_INSTALL_ROOT = os.environ.get(
    "CIMS_AGENT_INSTALL_ROOT",
    os.path.join(_PREFIX, "modules"),
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
        "agent_versions": _agent_versions(),   # 롤백 대상 선택용(콘솔 드롭다운)
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


# `ip` 명령이 깨졌을 때(예: libmnl.so.0 누락 → rc=127) 프로세스당 1회만 self-heal.
_DEPS_SELF_HEAL_DONE = False


def _ip_self_heal(ctx: str, detail: str) -> bool:
    """`ip -j <ctx>` 실패 시: 큰 로그 + 1회 base-deps 재설치 self-heal.

    과거 keepalived uninstall 이 공유 의존성 libmnl0 까지 purge → `ip` 가
    'error while loading shared libraries: libmnl.so.0' 로 깨지면 collect_* 가
    조용히 [] 를 반환해 콘솔 네트워크 정보가 비던 버그의 자가복구. dpkg 폭주
    방지를 위해 프로세스당 1회만 시도. 재설치를 수행했으면 True (호출자가 1회 재시도).
    """
    global _DEPS_SELF_HEAL_DONE
    if _DEPS_SELF_HEAL_DONE:
        return False
    _DEPS_SELF_HEAL_DONE = True
    print(f"[agent][net] 'ip -j {ctx}' 실패 ({detail}) — base deps self-heal 시도 (vendor deb 재설치)", flush=True)
    ensure_base_deps()
    return True


def _ip_json(args: list, ctx: str):
    """`ip -j <args>` 실행 → 파싱된 list. 실패 시 1회 self-heal 후 재시도.
    최종 실패면 None 반환(호출자가 [] 처리). 실패를 침묵하지 않고 로그로 남긴다."""
    for attempt in (1, 2):
        try:
            out = subprocess.run(["ip", "-j"] + args, capture_output=True, text=True, timeout=3)
            if out.returncode != 0:
                raise RuntimeError(f"rc={out.returncode} {(out.stderr or '').strip()[:200]}")
            return json.loads(out.stdout or "[]")
        except Exception as e:
            if attempt == 1 and _ip_self_heal(ctx, str(e)):
                continue
            print(f"[agent][net] 'ip -j {ctx}' 수집 실패: {e}", flush=True)
            return None
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
    # -4 flag 를 쓰면 IPv4 없는 NIC 이 출력 자체에서 빠지므로, 전체 family 받고
    # 아래 루프에서 family=='inet' 만 row 로 변환. ip 실패 시 self-heal 후 [] (침묵 금지).
    rows = _ip_json(["addr"], "addr")
    if rows is None:
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
    rows = _ip_json(["route"], "route")
    if rows is None:
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
    2) `<stem>_app.py` cmdline 매칭(-f) — python 데몬(csc/oam/oam-svc). comm 이 python3 라 1)로
       안 잡힘. 패키지명(예: oam-svc)은 하이픈을 포함할 수 있으나 python 엔트리포인트 파일명은
       언더스코어(oam_svc_app.py)이므로 stem 은 하이픈→언더스코어로 정규화한다."""
    script_stem = name.replace("-", "_")
    for argv in (["pgrep", "-ax", name], ["pgrep", "-af", f"{script_stem}_app.py"]):
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=2)
        except Exception:
            continue
        for line in r.stdout.splitlines():
            parts = line.split(maxsplit=1)
            if parts and parts[0].isdigit():
                return int(parts[0]), (parts[1] if len(parts) > 1 else "")
    return None


_HA_NOTIFY_LOG_DIR = os.environ.get("HA_LOG_DIR", "/var/log/cims-ha")
_HA_FLAP_WINDOW_SEC = 600


def _ha_transitions_10m() -> dict:
    """{svc: 최근 10분 keepalived 상태 전이 수} — cims-notify 로그
    (notify_<svc>.log, '<ISO8601> TYPE NAME -> STATE ...') tail 파싱.
    OAM 의 ha_flap 알람(threshold_crossed) 입력. 전이 개별 건은 알람이 아니라
    이벤트(로그)로만 남긴다(alarm_standardization §3.6). 미가독/부재는 조용히 생략."""
    out = {}
    try:
        names = os.listdir(_HA_NOTIFY_LOG_DIR)
    except OSError:
        return out
    now = time.time()
    for fn in names:
        if not (fn.startswith("notify_") and fn.endswith(".log")):
            continue
        svc = fn[len("notify_"):-len(".log")]
        try:
            with open(os.path.join(_HA_NOTIFY_LOG_DIR, fn), "rb") as f:
                f.seek(0, os.SEEK_END)
                f.seek(max(0, f.tell() - 32768))
                lines = f.read().decode("utf-8", "replace").splitlines()
        except OSError:
            continue
        n = 0
        for ln in lines[-200:]:
            if " -> " not in ln:
                continue
            try:
                t = datetime.fromisoformat(ln.split(" ", 1)[0]).timestamp()
            except ValueError:
                continue
            if now - t <= _HA_FLAP_WINDOW_SEC:
                n += 1
        if n:
            out[svc] = n
    return out


_CFG_HASH_CACHE: dict = {}   # config.json path → (mtime, hash) — 2초 주기 재해시 방지


def _cfg_hash_for_module(name: str):
    """modules/<name>/current/<name>/config.json (legacy 는 current/config.json) 의
    canonical hash 12hex — OAM 이 배포기록 실체화본 hash 와 비교해 config_out_of_sync
    알람을 판정. parse→canonical dump(sort_keys) 해시라 들여쓰기·키순서에 불변.
    파일 없음/파싱 실패는 None (보고 생략 → OAM 평가 제외)."""
    base = os.path.join(DEFAULT_INSTALL_ROOT, name, "current")
    for rel in (os.path.join(name, "config.json"), "config.json"):
        p = os.path.join(base, rel)
        try:
            st = os.stat(p)
        except OSError:
            continue
        cached = _CFG_HASH_CACHE.get(p)
        if cached and cached[0] == st.st_mtime:
            return cached[1]
        try:
            with open(p) as f:
                obj = json.load(f)
            h = hashlib.sha256(json.dumps(obj, sort_keys=True, ensure_ascii=False,
                                          separators=(",", ":")).encode("utf-8")).hexdigest()[:12]
        except Exception:
            return None
        _CFG_HASH_CACHE[p] = (st.st_mtime, h)
        return h
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
    # 설치 모듈별 배포 config.json canonical hash — modules[](실행 중만) 와 별개
    # top-level 키: 중지 모듈의 드리프트도 OAM 이 평가할 수 있게.
    try:
        hashes = {}
        for name in _metric_module_names():
            h = _cfg_hash_for_module(name)
            if h:
                hashes[name] = h
        if hashes:
            m["cfg_hashes"] = hashes
    except Exception:
        pass
    # keepalived 전이 카운트 (최근 10분) — OAM ha_flap 알람 입력.
    try:
        ht = _ha_transitions_10m()
        if ht:
            m["ha_transitions"] = ht
    except Exception:
        pass
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
    """install_path 로부터 모듈 루트를 정규화.

    - …/<module>/<version> → …/<module>      (이미 버전 경로 — 기존 설치 유지)
    - …/<module>           → 그대로           (모듈 루트 직접 지정)
    - 그 외 (공유 루트 /opt/cims-agent 등) → <install_path>/modules/<module>
      (02_deployment.md §2 합의 레이아웃: 모듈은 modules/ 폴더 하위로 집결.
       agent/ 트리 밖 sibling 이므로 durability 제약과도 양립.)
    """
    base = (install_path or "").rstrip("/")
    bn = os.path.basename(base)
    if _VERSION_DIR_RE.match(bn) and os.path.basename(os.path.dirname(base)) == module:
        return os.path.dirname(base)
    if bn == module:
        return base
    if bn == "modules":
        return os.path.join(base, module)
    return os.path.join(base, "modules", module)


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


def _flip_current(module_root: str, version_dir: str) -> str:
    """`<module_root>/current` 심볼릭을 version_dir 로 (재)지정 — 활성 버전 통로.

    상대 타겟(basename)으로 걸어 module_root 이동에도 견고하고, tmp→os.replace 로
    원자적 교체(읽는 프로세스가 보는 링크는 항상 완전한 구/신 둘 중 하나).
    반환: current 경로(성공) 또는 빈 문자열(실패)."""
    if not module_root or not version_dir:
        return ""
    cur = os.path.join(module_root, "current")
    ver = os.path.basename(version_dir.rstrip("/"))
    try:
        tmp = cur + ".tmp"
        try:
            os.remove(tmp)
        except OSError:
            pass
        os.symlink(ver, tmp)
        os.replace(tmp, cur)
        return cur
    except Exception as e:
        print(f"[agent] current flip 실패 ({module_root} -> {ver}): {e}", flush=True)
        return ""


def _agent_versions() -> list:
    """설치된 agent 버전 디렉토리 목록(mtime 최신순) — `<prefix>/agent/<ver>/cims_agent.py`
    가 있는 것만. current 심볼릭·잔재는 제외. 롤백 대상 선택(콘솔)·heartbeat 보고용."""
    root = os.path.join(_PREFIX, "agent")
    out = []
    try:
        for nm in os.listdir(root):
            p = os.path.join(root, nm)
            if os.path.isdir(p) and not os.path.islink(p) and _VERSION_DIR_RE.match(nm) \
                    and os.path.isfile(os.path.join(p, "cims_agent.py")):
                try:
                    out.append((os.path.getmtime(p), nm))
                except OSError:
                    pass
    except Exception:
        return []
    out.sort(reverse=True)
    return [nm for _mt, nm in out]


def _module_vdir_from_exe(exe_real: str, module_root: str) -> str:
    """프로세스 exe 실경로에서 module_root 직하의 버전 디렉토리를 추출.

    `current` 통로로 기동해도 `/proc/<pid>/exe` 는 exec 가 심볼릭을 해소한 실제 버전
    inode(예 `<module_root>/0.0.35/<pkg>/bin/<svc>`)를 가리키므로, 거기서 버전
    디렉토리(`<module_root>/0.0.35`)를 복원한다. exe 가 module_root 밖이면 ''
    (legacy 평탄 설치 — 버전 디렉토리 개념 없음)."""
    if not module_root or not exe_real:
        return ""
    root = os.path.realpath(module_root)
    if not exe_real.startswith(root + os.sep):
        return ""
    first = exe_real[len(root) + 1:].split(os.sep, 1)[0]
    return os.path.join(root, first) if first else ""


def _prune_old_versions(module_root: str, keep: int = 3) -> list:
    """모듈 루트의 버전 디렉토리를 mtime 최신 keep 개만 남기고 제거.

    버전 패턴(_VERSION_DIR_RE) 디렉토리만 대상 — legacy 평탄 설치 잔재(bin/,
    config/ 등)·`current` 심볼릭은 절대 건드리지 않음. 현재 `current` 가 가리키는
    버전은 mtime 과 무관하게 보존(롤백으로 구버전이 활성일 때 자해 방지).
    제거 목록 반환 (로그용)."""
    removed = []
    try:
        if not os.path.isdir(module_root):
            return removed
        # current 가 가리키는 실제 버전 디렉토리 — 절대 prune 하지 않음
        cur_real = ""
        try:
            cur_real = os.path.realpath(os.path.join(module_root, "current"))
        except OSError:
            pass
        vers = []
        for nm in os.listdir(module_root):
            p = os.path.join(module_root, nm)
            if os.path.isdir(p) and not os.path.islink(p) and _VERSION_DIR_RE.match(nm):
                try:
                    vers.append((os.path.getmtime(p), p))
                except OSError:
                    pass
        vers.sort(reverse=True)
        for _mt, p in vers[keep:]:
            if os.path.realpath(p) == cur_real:
                continue
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
    # current 심볼릭을 방금 설치한 버전으로 flip — 활성 버전 통로. start/restart 외에
    # cims-health 진실 검사(current/<mod>/config.json)와 cims-notify 절체 기동이 이
    # 경로를 본다. cold standby 는 승격 전까지 start 가 없어 install 이 current 를
    # 만들어 두지 않으면 절체 시점에 통로가 없다. (flip 후 prune: current 타겟은 보호)
    flipped = ""
    if module_root:
        if _flip_current(module_root, install_path):
            flipped = " current->" + os.path.basename(install_path)

    pruned = ""
    if module_root and install_path != legacy_path:
        removed = _prune_old_versions(module_root, keep=3)
        if removed:
            pruned = f" pruned={','.join(os.path.basename(p) for p in removed)}"

    return 0, (f"installed pkg_id={pkg_id} at {install_path} ({len(data)} bytes) "
               f"config={cfg_path}{migrated}{flipped}{pruned}"), ""


def _resolve_pkg_subdir(install_path: str, params: dict) -> str:
    """변종(pkg) 하위 디렉토리 해석 — pid 탐색 순서 및 overlay(config.json) 기록
    위치(<pkg>/config.json = 모듈 바이너리의 _findDeploymentConfig 가 읽는 csp.json
    부모×2) 결정에 사용.

    우선순위: params.pkg_subdir 명시 → params.package_name 디렉토리 존재
    → 파일시스템 자동 탐지 (모듈 구조 단일 디렉토리) → 빈 문자열.
    """
    explicit = (params.get("pkg_subdir") or "").strip()
    if explicit:
        return explicit
    pkg_name = (params.get("package_name") or "").strip()
    if pkg_name and os.path.isdir(os.path.join(install_path, pkg_name)):
        return pkg_name
    # 자동 탐지 — install_path 하위에서 모듈 구조(<d>/config/<d>.json 또는
    # <d>/bin/<d>) 를 갖는 디렉토리가 정확히 1개면 그것. 버전 단위 설치에서
    # 호출자가 package_name 을 빠뜨린 job params (raw 레코드 기반) 방어.
    try:
        cands = [d for d in os.listdir(install_path)
                 if os.path.isfile(os.path.join(install_path, d, "config", f"{d}.json"))
                 or os.path.isfile(os.path.join(install_path, d, "bin", d))]
        if len(cands) == 1:
            return cands[0]
    except Exception:
        pass
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


def job_ha_keepalived(params: dict) -> tuple:
    """keepalived 프로세스 제어 — 수동 절체(스위치오버) 오케스트레이션용.

    Params: { action: 'stop' | 'start' }.
      · stop  = 이 노드 keepalived 정지 → priority-0 advert 로 peer 즉시 승격
                (STOP notify 는 서비스 유지 — 이 노드 모듈은 안 내림).
      · start = keepalived 재기동 → nopreempt 라 BACKUP 복귀 → BACKUP notify 가
                이 노드 cold 모듈 정지.
    상세: ha_service_model.md §7. sudo 미등록(dev)은 graceful skip."""
    action = (params.get("action") or "").strip().lower()
    if action not in ("stop", "start"):
        return 1, "", f"invalid action: {action} (stop|start)"
    cims_ha = _resolve_cims_ha()
    if not cims_ha:
        return 0, "cims-ha not found — skip (no keepalived)", ""
    try:
        r = subprocess.run(["sudo", "-n", cims_ha, action],
                           capture_output=True, text=True, timeout=60)
        out = (r.stdout or "").strip()[-400:]
        if r.returncode == 0:
            return 0, f"cims-ha {action} rc=0 {out}", ""
        err = (r.stderr or r.stdout or "").strip()
        # sudo 미등록(dev)만 graceful — 그 외는 실패.
        if "password is required" in err or "sudo:" in err:
            return 0, f"cims-ha {action} skipped (no sudo, dev): {err[-160:]}", ""
        return 3, out, f"cims-ha {action} rc={r.returncode}: {err[-200:]}"
    except subprocess.TimeoutExpired as e:
        return 4, "", f"cims-ha {action} timeout: {e}"
    except Exception as e:
        return 5, "", f"cims-ha {action} exception: {e}"


def job_update_module_spec(params: dict) -> tuple:
    """modules/<mod>/service.json 갱신 — 모듈 운영 명세 (감시·절체모드·헬스).

    Params: { module: str, spec: dict }. 앱 config.json 과 물리 분리된 별도 파일로,
    agent watchdog(supervision.watchdog)과 제어 게이팅이 참조한다. 버전 트리 밖
    모듈 루트에 두어 업그레이드에 안전(uninstall 시 모듈과 함께 철거).
    상세: ha_service_model.md §3.2."""
    module = (params.get("module") or "").lower().strip()
    spec = params.get("spec")
    if not module:
        return 1, "", "module missing"
    if not isinstance(spec, dict):
        return 1, "", "spec must be dict"
    mod_root = os.path.join(_PREFIX, "modules", module)
    if not os.path.isdir(mod_root):
        # 아직 설치 안 된 모듈 — 디렉토리 없으면 생성해 두면 uninstall 대칭이 깨지므로
        # 스킵(설치 시 재푸시). 성공 반환 (no-op).
        return 0, f"module '{module}' not installed — service.json skip", ""
    path = os.path.join(mod_root, "service.json")
    try:
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(spec, f, ensure_ascii=False, indent=2)
        os.replace(tmp, path)
    except Exception as e:
        return 2, "", f"write service.json failed: {e}"
    return 0, f"service.json updated: {path}", ""


def job_update_ha(params: dict) -> tuple:
    """<prefix>/run/keepalived/ha.json 갱신 + cims-ha config|apply 자동 실행.

    Params:
      - install_path: (무시 — 구 OAM 호환 잔재. 과거엔 이 값의 쓰기불가 cwd-fallback
        이 우연히 flat 레이아웃의 <prefix>/agent/keepalived/ 에 떨어져 동작했으나,
        agent 버전화 이후 그 위치는 템플릿 없는 잔재 디렉토리라 cims-ha config 가
        실패했다 — keepalived 갱신 불능의 원인.)
      - ha_json: dict — OAM 이 ha_groups + members 로부터 render 한 내용

    ha.json 은 버전 트리 밖 <prefix>/run/keepalived/ 에 기록 (agent 업그레이드
    무관 영속 — managed_ips/supervised 와 동일 위치 규칙). 템플릿은 cims-ha 가
    자기 번들(current 경유)에서 찾고, apply 가 health/notify 스크립트와 ha.json 을
    /etc/keepalived/ 에 root 소유로 스테이징한다 (enable_script_security 충족).
    cims-ha apply 는 sudo 권한이 필요. 미등록 환경 (dev 등) 에서는 graceful
    skip — config 까지만 진행하고 apply 실패는 log 만 남기고 성공 반환.
    """
    ha_json = params.get("ha_json") or {}
    if not isinstance(ha_json, dict) or not ha_json:
        return 1, "", "ha_json missing or empty"
    # cims_home/cims_user 는 노드 로컬 사실 — OAM 렌더 값은 placeholder(/opt/cims, cims)
    # 라 실제 설치 루트/실행 계정과 다를 수 있다. agent 자신이 정본으로 덮어쓴다
    # (cims-notify 의 cims-svc 경로·runuser 대상, cims@.service ExecStart 치환에 사용).
    ha_json["cims_home"] = _PREFIX
    try:
        import getpass
        ha_json["cims_user"] = getpass.getuser()
    except Exception:
        pass
    ha_path = os.path.join(_PREFIX, "run", "keepalived", "ha.json")
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
    failed = ""      # 실패 사유 — dev graceful skip(sudo 미등록)과 구분해 정직하게 보고
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
            if r1.returncode != 0:
                failed = "cims-ha config failed"     # render 실패는 환경 무관 진짜 오류
        except Exception as e:
            msgs.append(f"cims-ha config exception: {e}")
            failed = f"cims-ha config exception: {e}"
        try:
            r2 = subprocess.run(["sudo", "-n", cims_ha, "--ha-dir", ha_dir, "apply"],
                                capture_output=True, text=True, timeout=60)
            if r2.returncode != 0:
                err_txt = (r2.stderr or r2.stdout).strip()
                msgs.append(f"cims-ha apply rc={r2.returncode} err={err_txt[-200:]}")
                # sudo 미등록(dev)만 graceful — 그 외 rc!=0 은 실제 적용 실패.
                if "password is required" not in err_txt and "sudo:" not in err_txt:
                    failed = f"cims-ha apply rc={r2.returncode}"
            else:
                msgs.append("cims-ha apply rc=0")
        except subprocess.TimeoutExpired as e:
            # 적용이 hang — keepalived 가 기동/정지 완료를 못 알린 것. 성공으로 위장하면
            # 콘솔이 정상 반영으로 오판하므로 job 실패로 보고한다.
            msgs.append(f"cims-ha apply timeout: {e}")
            failed = "cims-ha apply timeout (keepalived hang 의심)"
        except Exception as e:
            msgs.append(f"cims-ha apply exception (likely no keepalived / no sudo): {e}")
    else:
        msgs.append("cims-ha not found in candidate paths — ha.json only (no apply)")
    if failed:
        return 5, "\n".join(msgs), failed
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
_MANAGED_IPS_FILE = os.path.join(_PREFIX, "run", "managed_ips.json")

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
_NET_TUNING_FILE = os.path.join(_PREFIX, "run", "net_tuning.json")

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

_SUPERVISE_FILE = os.path.join(_PREFIX, "run", "supervised.json")
_WATCHDOG_BACKOFF: dict = {}     # module -> {"ts": float, "fails": int}
SUPERVISE_INTERVAL_SEC = 10

# cold-spare 게이트 — update_ha 가 기록한 ha.json 의 services.*.cold_modules 기준.
_HA_JSON_PATH = os.path.join(_PREFIX, "run", "keepalived", "ha.json")

# ── HA 런타임 상태 (ha_service_model.md §3.2) ─────────────────────────────
#   run/ha/desired.json      노드 오버라이드 {module: 'stopped'} — 서버별 stop 기록
#   run/ha/fail_<mod>        재기동 실패 카운터 (watchdog 기록, cims-health 판독)
#   run/ha/op_grace_<mod>    조작 유예 마커 (제어 job 진입 시 touch — mtime 만 사용)
# agent(cims user)가 쓰고 cims-health(root)가 읽는다. CIMS_HOME(ha.json)==_PREFIX 라
# cims-health 는 <CIMS_HOME>/run/ha/ 로 동일 경로를 유도한다.
_HA_STATE_DIR = os.path.join(_PREFIX, "run", "ha")
_DESIRED_FILE = os.path.join(_HA_STATE_DIR, "desired.json")
_OP_GRACE_SEC = 30               # 조작 유예 창 (restart 순단 흡수 — cims-health 도 동일 상수)


def _ha_state_dir() -> str:
    try:
        os.makedirs(_HA_STATE_DIR, exist_ok=True)
    except Exception:
        pass
    return _HA_STATE_DIR


# ── HA 상태 디렉토리 레이아웃 (ha_service_model.md §5) ────────────────────────
# run/ha/   = 휘발(재부팅 시 초기화 대상) — verdict/role/health/promotion/recovery/
#             operations. (legacy 447fa27d: desired.json·fail_*·op_grace_* 는 run/ha
#             루트에 잔존 — verdict-driven 전환 시 desired 는 state/ha 로 이관.)
# state/ha/ = 영속 — desired/latch (운영자 의도·failover 래치, 재부팅 생존).
# agent(cims)가 소유·생성한다. user systemd 유닛이라 RuntimeDirectory/StateDirectory 를
# 쓸 수 없어(→ /run/user·~/.local/state, root keepalived 와 경로 어긋남) 직접 만든다.
# 교차 사용자 접근은 읽기 전용 방향뿐: role(root 쓰기→cims 읽기), verdict(cims 쓰기→
# root 읽기). _PREFIX 는 tmpfs 가 아니므로 휘발 의미는 기동 시 초기화 + verdict boot_id.
_HA_RUN_DIR = _HA_STATE_DIR                       # _PREFIX/run/ha (기존 상수 재사용)
_HA_PERSIST_DIR = os.path.join(_PREFIX, "state", "ha")
_HA_RUN_SUBDIRS = ("verdict", "role", "health", "promotion", "recovery", "operations")
_HA_PERSIST_SUBDIRS = ("desired", "latch")


def _ensure_ha_dirs() -> None:
    """HA 상태 디렉토리 골격 생성 (idempotent, 기동 1회). 비-systemd·기존 설치본에서도
    경로가 보장되도록 agent 가 자체 생성한다."""
    try:
        for sub in _HA_RUN_SUBDIRS:
            os.makedirs(os.path.join(_HA_RUN_DIR, sub), exist_ok=True)
        for sub in _HA_PERSIST_SUBDIRS:
            os.makedirs(os.path.join(_HA_PERSIST_DIR, sub), exist_ok=True)
    except Exception as e:
        print(f"[agent][ha] 상태 디렉토리 생성 실패(무시): {e}", flush=True)


def _boot_id() -> str:
    """현재 부팅 식별자 — verdict/role 이 재부팅 전 값 재사용을 막는 데 쓴다(§13).
    읽기 실패 시 빈 문자열 (비교 측이 mismatch 로 처리)."""
    try:
        with open("/proc/sys/kernel/random/boot_id") as f:
            return f.read().strip()
    except Exception:
        return ""


# ── HA feature flag (단계적 이행 §18) — env 우선, 기본 legacy(동작 무변경) ──────
#   CIMS_HA_VERDICT_SOURCE = legacy | supervisor   (track_script/판정 주체)
#   CIMS_HA_NOTIFY_MODE    = legacy | role_writer  (cims-notify 동작)
#   CIMS_HA_STAGGER        = 1 | 0                 (개시국면 스태거 유지 여부)
# 현재 단계는 리더만 두고 소비하지 않는다(dormant) — 이후 단계가 게이트로 사용.
def _ha_flags() -> dict:
    vs = (os.environ.get("CIMS_HA_VERDICT_SOURCE") or "legacy").lower()
    # notify_mode 는 명시 override 없으면 verdict_source 를 따른다 — role_writer(notify)
    # 와 reconcile(Supervisor)은 한 세트라, 한 스위치(verdict_source=supervisor)로 함께
    # 켜져야 승격 시 cold 모듈 기동 주체가 끊기지 않는다.
    nm = os.environ.get("CIMS_HA_NOTIFY_MODE")
    if not nm:
        nm = "role_writer" if vs == "supervisor" else "legacy"
    return {"verdict_source": vs, "notify_mode": nm.lower(),
            "stagger": os.environ.get("CIMS_HA_STAGGER", "1") != "0"}


def _write_ha_flags_file() -> None:
    """run/ha/flags.json — cims-notify/cims-health(root bash)가 현재 모드를 읽는 통로.
    agent(cims)가 기동 시 기록. root 컴포넌트는 이 파일로 legacy↔supervisor 를 안다."""
    try:
        os.makedirs(_HA_RUN_DIR, exist_ok=True)
        p = os.path.join(_HA_RUN_DIR, "flags.json")
        tmp = p + ".tmp"
        with open(tmp, "w") as f:
            json.dump(_ha_flags(), f)
        os.replace(tmp, p)
    except Exception as e:
        print(f"[agent][ha] flags.json 기록 실패(무시): {e}", flush=True)


# ══════════════════════════════════════════════════════════════════════════
#  Health Checker (ha_service_model.md §6) — liveness / readiness / preflight
#  검사별 독립 주기·타임아웃으로 실행해 결과를 run/ha/health/<mod>.json 에 캐시한다.
#  HA Evaluator(P3)가 캐시만 읽어 verdict 를 합성한다. 스케줄러 스레드는 flag
#  (verdict_source=supervisor 또는 CIMS_HA_HEALTH=1)일 때만 기동 — legacy 기본에선
#  dormant 라 동작 무변경. 검사 대상·힌트는 ha.json(services)에서 유도(OAM 렌더 재사용).
# ══════════════════════════════════════════════════════════════════════════

_HEALTH_DIR = os.path.join(_HA_RUN_DIR, "health")
_HEALTH_DEFAULTS = {
    "liveness":  {"interval": 2,  "timeout": 2},
    "readiness": {"interval": 3,  "timeout": 2},
    "preflight": {"interval": 10, "timeout": 2},
}
_HEALTH_NEXT: dict = {}     # (module, check) -> next_run epoch


def _read_ha_json() -> dict:
    try:
        with open(_HA_JSON_PATH) as f:
            return json.load(f)
    except Exception:
        return {}


def _health_targets() -> list:
    """ha.json services → 검사 대상 모듈 목록. 모듈 = relevant ∪ cold ∪ health_module.
    port/proto 는 service 레벨 힌트(단일 데몬 또는 health_module 에 적용), config_key 는
    health_module 에만(csc 실효포트 유도)."""
    cfg = _read_ha_json()
    home = cfg.get("cims_home") or _PREFIX
    out, seen = [], set()
    for svc, s in (cfg.get("services") or {}).items():
        hmod = str(s.get("health_module") or "").lower().strip()
        mods = set()
        if hmod:
            mods.add(hmod)
        mods |= {str(m).lower().strip() for m in (s.get("relevant_modules") or []) if str(m).strip()}
        mods |= {str(m).lower().strip() for m in (s.get("cold_modules") or []) if str(m).strip()}
        port = s.get("port")
        proto = (s.get("proto") or "tcp").lower()
        ckey = s.get("health_config_key")
        for m in mods:
            if not m or m in seen:
                continue
            seen.add(m)
            out.append({"module": m, "service": svc, "home": home, "proto": proto,
                        "port": port if (m == hmod or len(mods) == 1) else None,
                        "config_key": ckey if m == hmod else None})
    return out


def _module_config_port(mod: str, home: str, key: str) -> "int | None":
    """모듈 배포 config.json 의 key(flat 우선, nested 수용)에서 실효 포트 — cims-health 와 동일."""
    base = os.path.join(home, "modules", mod, "current")
    for rel in (os.path.join(mod, "config.json"), "config.json"):
        p = os.path.join(base, rel)
        if not os.path.isfile(p):
            continue
        try:
            mc = json.load(open(p))
            v = mc.get(key)
            if v is None:
                cur = mc
                for part in key.split("."):
                    cur = cur.get(part) if isinstance(cur, dict) else None
                    if cur is None:
                        break
                v = cur
            if v and 0 < int(v) < 65536:
                return int(v)
        except Exception:
            pass
        break
    return None


def _port_listening(port: int, proto: str) -> bool:
    flag = "-lnt" if proto == "tcp" else "-lnu"
    try:
        r = subprocess.run(["ss", flag, f"sport = :{port}"],
                           capture_output=True, text=True, timeout=2)
        return any(line.strip() for line in r.stdout.splitlines()[1:])
    except Exception:
        return False


def _mod_installed_local(root: str) -> bool:
    if os.path.exists(os.path.join(root, "current")):
        return True
    try:
        return any(os.path.isdir(os.path.join(root, d)) for d in os.listdir(root))
    except Exception:
        return False


def _run_health_check(mod: str, check: str, t: dict) -> dict:
    st = time.time()
    if check == "liveness":
        up = _pgrep_module(mod) is not None
        r = {"status": "SUCCESS" if up else "FAIL",
             "detail": "process " + ("up" if up else "down")}
    elif check == "readiness":
        port = t.get("port")
        if t.get("config_key"):
            p = _module_config_port(mod, t.get("home") or _PREFIX, t["config_key"])
            if p:
                port = p
        proto = (t.get("proto") or "tcp").lower()
        if not port:
            up = _pgrep_module(mod) is not None      # 포트 미상 → 프로세스 대체 (cims-health 동일)
            r = {"status": "SUCCESS" if up else "FAIL",
                 "detail": "port unknown; process " + ("up" if up else "down")}
        else:
            listening = _port_listening(int(port), proto)
            r = {"status": "SUCCESS" if listening else "FAIL",
                 "detail": f":{port}/{proto} " + ("listening" if listening else "not listening")}
    else:  # preflight
        root = os.path.join(t.get("home") or _PREFIX, "modules", mod)
        if not _mod_installed_local(root):
            r = {"status": "FAIL", "detail": "not installed"}
        else:
            cur = os.path.join(root, "current")
            cfg_ok = any(os.path.isfile(os.path.join(cur, rel))
                         for rel in (os.path.join(mod, "config.json"), "config.json"))
            r = {"status": "SUCCESS", "detail": "installed" + ("" if cfg_ok else "; config?")}
    r["duration_ms"] = int((time.time() - st) * 1000)
    return r


def _health_merge_write(mod: str, updated: dict) -> None:
    path = os.path.join(_HEALTH_DIR, f"{mod}.json")
    data = {}
    try:
        with open(path) as f:
            data = json.load(f)
    except Exception:
        data = {}
    checks = data.get("checks") if isinstance(data.get("checks"), dict) else {}
    checks.update(updated)
    data = {"module": mod, "checks": checks, "updated_at": int(time.time())}
    try:
        os.makedirs(_HEALTH_DIR, exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f)
        os.replace(tmp, path)
    except Exception as e:
        print(f"[agent][health] cache write 실패({mod}): {e}", flush=True)


def health_scheduler_tick() -> None:
    """due 검사만 실행해 캐시 갱신. 각 검사는 자체 timeout 을 갖고, expires_at 은
    interval×3(3회 연속 미갱신이면 stale)."""
    now = time.time()
    for t in _health_targets():
        mod = t["module"]
        updated = {}
        for check in ("liveness", "readiness", "preflight"):
            key = (mod, check)
            if now < _HEALTH_NEXT.get(key, 0):
                continue
            prof = _HEALTH_DEFAULTS[check]
            _HEALTH_NEXT[key] = now + prof["interval"]
            res = _run_health_check(mod, check, t)
            res["checked_at"] = int(now)
            res["expires_at"] = int(now + prof["interval"] * 3)
            updated[check] = res
        if updated:
            _health_merge_write(mod, updated)


def _start_health_scheduler() -> None:
    """Health Scheduler 스레드 — OAM 루프·watchdog 과 독립. flag 활성 시에만 기동."""
    def _loop():
        while True:
            try:
                health_scheduler_tick()
            except Exception as e:
                print(f"[agent][health] tick error: {e}", flush=True)
            time.sleep(1)
    threading.Thread(target=_loop, daemon=True, name="agent-health").start()
    print("[agent][health] Health Scheduler 기동 (liveness/readiness/preflight)", flush=True)


# ══════════════════════════════════════════════════════════════════════════
#  Recovery Supervisor — HA Evaluator (ha_service_model.md §7·§8·§10)
#  role/desired/latch/health 캐시 + ha.json 명세 → 다축 verdict 합성해 run/ha/
#  verdict/<svc>.json 에 기록. shadow 단계: keepalived 미소비(legacy cims-health 유지),
#  모듈 재기동 안 함(legacy watchdog 유지) — verdict 계산·기록·전이 로그만. OAM 루프와
#  독립된 전용 스레드(2초). flag(verdict_source=supervisor 또는 CIMS_HA_SHADOW)일 때만 기동.
# ══════════════════════════════════════════════════════════════════════════

_EVAL_INTERVAL = 2
_VERDICT_TTL = 6
_PROMOTION_GRACE_SEC = 20
_EVAL_SEQ: dict = {}          # svc -> 단조 sequence
_EVAL_PREV_ROLE: dict = {}    # svc -> 직전 role (승격 전이 감지)
_EVAL_PREV_ELIG: dict = {}    # svc -> 직전 eligible (전이 로그)
_EVAL_LATCH: dict = {}        # svc -> bool (shadow: in-memory, 미영속 — 영속 래치는 P5)


def _fail_count_read(mod: str) -> int:
    try:
        with open(_fail_path(mod)) as f:
            return int(f.read().split()[0])
    except Exception:
        return 0


def _local_ips() -> set:
    return {r.get("ip") for r in collect_interfaces() if r.get("ip")}


def _service_vips(s: dict) -> list:
    vips = [str(v.get("ip")).strip() for v in (s.get("vips") or [])
            if isinstance(v, dict) and v.get("ip")]
    if s.get("vip"):
        vips.append(str(s["vip"]).strip())
    return [v for v in vips if v]


def _service_relevant(s: dict) -> list:
    rel = [str(m).lower().strip() for m in (s.get("relevant_modules") or []) if str(m).strip()]
    if rel:
        return rel
    out = []
    if s.get("health_module"):
        out.append(str(s["health_module"]).lower().strip())
    out += [str(m).lower().strip() for m in (s.get("cold_modules") or []) if str(m).strip()]
    return list(dict.fromkeys([m for m in out if m]))


def _current_role(svc: str, s: dict) -> str:
    """role 파일(P4+) 우선, 없으면 VIP 보유로 유도. keepalived 상태 MASTER/BACKUP/FAULT."""
    rf = os.path.join(_HA_RUN_DIR, "role", f"{svc}.json")
    try:
        with open(rf) as f:
            d = json.load(f)
        if d.get("boot_id") == _boot_id():
            return str(d.get("role") or "UNKNOWN").upper()
    except Exception:
        pass
    vips = _service_vips(s)
    if not vips:
        return "UNKNOWN"
    local = _local_ips()
    return "MASTER" if any(v in local for v in vips) else "BACKUP"


def _module_health(mod: str, check: str):
    """health 캐시 판독 → True/False/None(미상·stale). 신선도(expires_at) 확인."""
    try:
        with open(os.path.join(_HEALTH_DIR, f"{mod}.json")) as f:
            d = json.load(f)
        c = (d.get("checks") or {}).get(check)
        if not c or c.get("expires_at", 0) < time.time():
            return None
        return c.get("status") == "SUCCESS"
    except Exception:
        return None


def _restart_limit_for(s: dict) -> dict:
    rl = s.get("restart_limit") if isinstance(s.get("restart_limit"), dict) else {}
    return {"max_fails": int(rl.get("max_fails", 3) or 3),
            "window_sec": int(rl.get("window_sec", 300) or 300)}


def _update_promotion_grace(svc: str, role: str, now: float) -> bool:
    """역할 전이 감지 — MASTER 진입 시 grace 설정(start 전에). grace 활성이면 True."""
    prev = _EVAL_PREV_ROLE.get(svc)
    _EVAL_PREV_ROLE[svc] = role
    pf = os.path.join(_HA_RUN_DIR, "promotion", f"{svc}.json")
    if role == "MASTER" and prev != "MASTER":
        try:
            os.makedirs(os.path.dirname(pf), exist_ok=True)
            tmp = pf + ".tmp"
            with open(tmp, "w") as f:
                json.dump({"service": svc, "state": "PROMOTING", "started_at": int(now),
                           "grace_until": int(now + _PROMOTION_GRACE_SEC),
                           "boot_id": _boot_id()}, f)
            os.replace(tmp, pf)
        except Exception:
            pass
        return True
    if role != "MASTER":
        return False
    try:
        with open(pf) as f:
            d = json.load(f)
        return d.get("boot_id") == _boot_id() and now < d.get("grace_until", 0)
    except Exception:
        return False


def _eval_service(svc: str, s: dict) -> dict:
    now = time.time()
    role = _current_role(svc, s)
    relevant = _service_relevant(s)
    cold = {str(m).lower().strip() for m in (s.get("cold_modules") or [])}
    desired = _load_desired()                       # legacy {mod: 'stopped'} 노드 오버라이드
    rl = _restart_limit_for(s)
    reasons: list = []

    def op_stopped(m):
        return desired.get(m) == "stopped"

    in_grace = _update_promotion_grace(svc, role, now)
    hot = [m for m in relevant if m not in cold]
    cold_rel = [m for m in relevant if m in cold]

    def all_ready(mods, check, label):
        ok = True
        for m in mods:
            if op_stopped(m):
                continue
            v = _module_health(m, check)
            if v is not True:
                reasons.append(f"{label}:{m}:{'unknown' if v is None else 'fail'}")
                ok = False
        return ok

    eligible, state, svc_avail, standby_ready = False, "STARTING", False, False

    if _EVAL_LATCH.get(svc):
        eligible, state = False, "FAILOVER_LATCHED"
        reasons.append("latched")
    elif role in ("BACKUP", "UNKNOWN", "FAULT"):
        # 승격 자격 = hot readiness + cold preflight (cold runtime 정지는 정상)
        eligible = all_ready(hot, "readiness", "hot_readiness") \
            and all_ready(cold_rel, "preflight", "cold_preflight")
        state = "STANDBY_READY" if eligible else "STANDBY_INELIGIBLE"
        standby_ready = eligible
    elif role == "MASTER":
        if in_grace:
            # 승격 grace — cold runtime readiness 제외, hot readiness + cold preflight 만
            eligible = all_ready(hot, "readiness", "hot_readiness") \
                and all_ready(cold_rel, "preflight", "cold_preflight")
            state = "PROMOTING"
        else:
            ok, any_down, any_intent = True, False, False
            for m in relevant:
                if op_stopped(m):
                    any_intent = True
                    continue                             # 의도적 정지 — 절체 사유 아님
                v = _module_health(m, "readiness")
                if v is True:
                    continue
                any_down = True
                fc = _fail_count_read(m)
                if fc >= rl["max_fails"]:
                    ok = False
                    reasons.append(f"exhausted:{m}:{fc}/{rl['max_fails']}")
                else:
                    reasons.append(f"recovering:{m}:{fc}/{rl['max_fails']}")
            eligible = ok
            if not ok:
                state = "FAILOVER_REQUIRED"
                _EVAL_LATCH[svc] = True                   # shadow: in-memory 래치(미영속)
            elif any_intent and not any_down:
                state = "INTENTIONALLY_DOWN"
            elif any_down:
                state = "RECOVERING"
            else:
                state = "ACTIVE_HEALTHY"
            svc_avail = (not any_down) and (not any_intent)
    return {
        "service": svc, "role": role,
        "service_state": state, "vrrp_eligible": bool(eligible),
        "service_available": bool(svc_avail), "standby_ready": bool(standby_ready),
        "in_promotion_grace": bool(in_grace),
        "reason_codes": reasons[:12],
    }


def _verdict_write(svc: str, v: dict) -> None:
    _EVAL_SEQ[svc] = _EVAL_SEQ.get(svc, 0) + 1
    now = time.time()
    v.update({"sequence": _EVAL_SEQ[svc], "boot_id": _boot_id(),
              "updated_at": int(now), "expires_at": int(now + _VERDICT_TTL)})
    p = os.path.join(_HA_RUN_DIR, "verdict", f"{svc}.json")
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        tmp = p + ".tmp"
        with open(tmp, "w") as f:
            json.dump(v, f)
        os.replace(tmp, p)
    except Exception as e:
        print(f"[agent][ha] verdict write 실패({svc}): {e}", flush=True)


def ha_evaluator_tick() -> None:
    cfg = _read_ha_json()
    for svc, s in (cfg.get("services") or {}).items():
        try:
            v = _eval_service(svc, s)
        except Exception as e:
            print(f"[agent][ha] eval error({svc}): {e}", flush=True)
            continue
        _verdict_write(svc, v)
        if _EVAL_PREV_ELIG.get(svc) != v["vrrp_eligible"]:
            _EVAL_PREV_ELIG[svc] = v["vrrp_eligible"]
            print(f"[agent][ha] verdict {svc}: eligible={v['vrrp_eligible']} "
                  f"state={v['service_state']} role={v['role']} "
                  f"reasons={v['reason_codes']}", flush=True)


# ── Recovery Supervisor — role reconcile (ha_service_model.md §7.1) ──────────
# 매 주기 "역할에서 기대되는 모듈 상태 vs 실제"를 비교해 Process Manager 로 start/stop.
# supervisor 모드(verdict_source=supervisor)에서만 활성 — cims-notify(role_writer)가
# 모듈을 직접 안 띄우는 대신 여기서 role 전이를 reconcile 해 cold 모듈을 기동/정지한다.
# 이벤트가 아니라 목표상태 수렴이라 notify/전이 이벤트를 놓쳐도 다음 주기에 복구된다.
_RECONCILE_BACKOFF: dict = {}     # module -> {"ts": float, "fails": int}


def _module_dist_dir(module: str) -> "str | None":
    """모듈 배포 루트(CIMS_DIST_DIR) — current 우선, 없으면 최신 버전 디렉토리
    (cims-notify _mod_dist 와 동일: 한 번도 기동 안 한 cold standby 도 승격 기동 가능)."""
    root = os.path.join(_PREFIX, "modules", (module or "").lower().strip())
    cur = os.path.join(root, "current")
    if os.path.exists(cur):
        return cur
    try:
        subs = [os.path.join(root, d) for d in os.listdir(root)
                if os.path.isdir(os.path.join(root, d))]
        subs.sort(key=os.path.getmtime, reverse=True)
        return subs[0] if subs else None
    except Exception:
        return None


def _ha_managed_modules() -> set:
    """ha.json 전 서비스의 relevant ∪ cold 모듈 — Supervisor 가 lifecycle 을 소유하는
    집합. supervisor 모드에서 legacy watchdog 은 이 집합을 건드리지 않는다(이중 제어 방지)."""
    cfg = _read_ha_json()
    out = set()
    for s in (cfg.get("services") or {}).values():
        out |= set(_service_relevant(s))
        out |= {str(m).lower().strip() for m in (s.get("cold_modules") or []) if str(m).strip()}
    return {m for m in out if m}


def _expected_running(m: str, role: str, cold: set, desired: dict) -> bool:
    """역할·정책 기반 기대 실행 상태 (§7.1)."""
    if desired.get(m) == "stopped":
        return False                       # 운영자 정지 — 기동 안 함
    if role == "MASTER":
        return True                        # hot·cold 모두 실행
    # BACKUP/FAULT/UNKNOWN: hot 만 실행, cold 는 정지가 정상
    return m not in cold


def ha_reconcile_tick() -> None:
    cfg = _read_ha_json()
    desired = _load_desired()
    now = time.time()
    for svc, s in (cfg.get("services") or {}).items():
        role = _current_role(svc, s)
        cold = {str(m).lower().strip() for m in (s.get("cold_modules") or [])}
        managed = set(_service_relevant(s)) | cold
        rl = _restart_limit_for(s)
        for m in managed:
            if not m or m in _NON_DAEMON_MODULES:
                continue
            exp = _expected_running(m, role, cold, desired)
            running = _pgrep_module(m) is not None
            if exp and not running:
                if _in_op_grace(m):
                    continue               # 운영자 제어 job 진행 중 — 손대지 않음
                fc = _fail_count_read(m)
                if fc >= rl["max_fails"]:
                    continue               # 재기동 소진 — Evaluator 가 latch/verdict 처리
                st = _RECONCILE_BACKOFF.setdefault(m, {"ts": 0.0, "fails": 0})
                backoff = min(300, 5 * (2 ** st["fails"]))
                if now - st["ts"] < backoff:
                    continue
                st["ts"] = now; st["fails"] += 1
                _fail_bump(m, rl["window_sec"])
                dist = _module_dist_dir(m)
                if dist:
                    rc, out, err = _run_cims_svc(dist, "start", m)
                    print(f"[agent][ha] reconcile start {m} (role={role}) rc={rc}", flush=True)
            elif (not exp) and running:
                dist = _module_dist_dir(m)
                if dist:
                    _run_cims_svc(dist, "stop", m)
                    print(f"[agent][ha] reconcile stop {m} (role={role})", flush=True)
                _RECONCILE_BACKOFF.pop(m, None)
                _fail_reset(m)
            elif exp and running:
                _RECONCILE_BACKOFF.pop(m, None)     # 정상 — backoff·카운터 리셋
                _fail_reset(m)


def _start_ha_evaluator(reconcile: bool = False) -> None:
    def _loop():
        while True:
            try:
                ha_evaluator_tick()
                if reconcile:
                    ha_reconcile_tick()
            except Exception as e:
                print(f"[agent][ha] evaluator error: {e}", flush=True)
            time.sleep(_EVAL_INTERVAL)
    threading.Thread(target=_loop, daemon=True, name="agent-ha-eval").start()
    print(f"[agent][ha] HA Evaluator 기동 (verdict 계산{', reconcile 활성' if reconcile else ', shadow'})",
          flush=True)


def _load_desired() -> dict:
    try:
        with open(_DESIRED_FILE) as f:
            d = json.load(f)
            return d if isinstance(d, dict) else {}
    except Exception:
        return {}


def _save_desired(d: dict) -> None:
    try:
        _ha_state_dir()
        tmp = _DESIRED_FILE + ".tmp"
        with open(tmp, "w") as f:
            json.dump(d, f)
        os.replace(tmp, _DESIRED_FILE)
    except Exception as e:
        print(f"[agent][ha] desired.json 저장 실패: {e}", flush=True)


def _set_desired(module: str, state: "str | None") -> None:
    """노드 오버라이드 설정. state='stopped' → 기록, None/'running' → 오버라이드 해제.
    서버별 stop = stopped 기록(watchdog·health 제외), 서버별 start = 해제."""
    module = (module or "").lower().strip()
    if not module:
        return
    d = _load_desired()
    if state == 'stopped':
        if d.get(module) != 'stopped':
            d[module] = 'stopped'
            _save_desired(d)
            print(f"[agent][ha] 노드 오버라이드: {module}=stopped (서버별 정지 — 절체 안 함)", flush=True)
    else:
        if module in d:
            d.pop(module, None)
            _save_desired(d)
            print(f"[agent][ha] 노드 오버라이드 해제: {module}", flush=True)


def _desired_stopped(module: str) -> bool:
    return _load_desired().get((module or "").lower().strip()) == 'stopped'


def _touch_op_grace(module: str) -> None:
    """제어 job 진입 시 조작 유예 마커 갱신 — cims-health/watchdog 가 mtime 으로 유예."""
    module = (module or "").lower().strip()
    if not module:
        return
    try:
        _ha_state_dir()
        p = os.path.join(_HA_STATE_DIR, f"op_grace_{module}")
        with open(p, "w") as f:
            f.write(str(int(time.time())))
    except Exception as e:
        print(f"[agent][ha] op_grace 마커 실패({module}): {e}", flush=True)


def _in_op_grace(module: str) -> bool:
    """조작 유예 창 이내인가 — 제어 job 진입 후 _OP_GRACE_SEC 초."""
    module = (module or "").lower().strip()
    if not module:
        return False
    try:
        at = os.path.getmtime(os.path.join(_HA_STATE_DIR, f"op_grace_{module}"))
        return (time.time() - at) < _OP_GRACE_SEC
    except Exception:
        return False


def _fail_path(module: str) -> str:
    return os.path.join(_HA_STATE_DIR, f"fail_{(module or '').lower().strip()}")


def _fail_bump(module: str, window_sec: int) -> int:
    """재기동 실패 카운터 증가 (window_sec 밖이면 리셋). 갱신된 count 반환.
    파일 형식: 'count first_ts' (공백 구분) — cims-health 가 첫 필드를 읽는다."""
    now = int(time.time())
    count, first_ts = 0, now
    try:
        with open(_fail_path(module)) as f:
            parts = f.read().split()
            count = int(parts[0]); first_ts = int(parts[1]) if len(parts) > 1 else now
    except Exception:
        pass
    if now - first_ts > max(1, int(window_sec or 300)):
        count, first_ts = 0, now        # 윈도우 만료 — 리셋
    count += 1
    try:
        _ha_state_dir()
        # 원자적 쓰기 — cims-health(root)가 동시에 읽으므로 torn read 방지.
        p = _fail_path(module); tmp = p + ".tmp"
        with open(tmp, "w") as f:
            f.write(f"{count} {first_ts}")
        os.replace(tmp, p)
    except Exception as e:
        print(f"[agent][ha] fail 카운터 저장 실패({module}): {e}", flush=True)
    return count


def _fail_reset(module: str) -> None:
    try:
        os.remove(_fail_path(module))
    except FileNotFoundError:
        pass
    except Exception:
        pass


def _module_spec_path(module: str) -> str:
    return os.path.join(_PREFIX, "modules", (module or "").lower().strip(), "service.json")


def _load_module_spec(module: str) -> dict:
    """modules/<mod>/service.json — 부재/파싱실패 시 default(watchdog on)."""
    try:
        with open(_module_spec_path(module)) as f:
            d = json.load(f)
            return d if isinstance(d, dict) else {}
    except Exception:
        return {}


def _watchdog_enabled(module: str) -> bool:
    """service.json supervision.watchdog (부재 시 True — 종전 동작 유지)."""
    spec = _load_module_spec(module)
    sup = spec.get("supervision") if isinstance(spec.get("supervision"), dict) else {}
    return bool(sup.get("watchdog", True))


def _ha_restart_limit(svc_cold_module: str) -> dict:
    """ha.json services.* 중 svc_cold_module 을 cold_modules 로 갖는 항목의 restart_limit.
    미상 시 default {max_fails:3, window_sec:300}."""
    default = {"max_fails": 3, "window_sec": 300}
    try:
        with open(_HA_JSON_PATH) as f:
            cfg = json.load(f)
    except Exception:
        return default
    m = (svc_cold_module or "").lower()
    for s in (cfg.get("services") or {}).values():
        cold = [str(x).lower() for x in (s.get("cold_modules") or [])]
        if m in cold:
            rl = s.get("restart_limit") if isinstance(s.get("restart_limit"), dict) else {}
            return {
                "max_fails": int(rl.get("max_fails", default["max_fails"]) or default["max_fails"]),
                "window_sec": int(rl.get("window_sec", default["window_sec"]) or default["window_sec"]),
            }
    return default


def _cold_standby_module(svc: str) -> bool:
    """svc 가 cold-spare 모듈이고 이 노드가 해당 서비스 VIP 를 보유하지 않으면 True.

    True = standby 에선 기동하지 않는다 — start job 은 스킵, watchdog 은 재기동
    보류 (MASTER 승격 시 keepalived notify 가 기동; VIP 취득 후엔 이 게이트가
    False 가 되어 watchdog 이 crash 재기동 백스톱으로 복귀).
    ha.json 부재/파싱 실패/VIP 미정의는 False — 종전(hot) 동작 유지."""
    try:
        with open(_HA_JSON_PATH) as f:
            cfg = json.load(f)
    except Exception:
        return False
    svc = (svc or "").lower()
    vips = []
    for s in (cfg.get("services") or {}).values():
        cold = [str(m).lower() for m in (s.get("cold_modules") or [])]
        if svc not in cold:
            continue
        vips = [str(v.get("ip")) for v in (s.get("vips") or [])
                if isinstance(v, dict) and v.get("ip")]
        if s.get("vip"):
            vips.append(str(s["vip"]))
        break
    if not vips:
        return False
    # keepalived 가 로컬에서 죽어 있으면 HA 는 이 노드를 관리하지 않는 상태
    # (STOP 정책 = 서비스 유지, 운영자 직접 제어) — 게이트를 풀어 watchdog 이
    # crash 백스톱으로 동작한다. 게이트를 유지하면 keepalived 정지 + VIP 부재
    # 조합에서 죽은 모듈을 아무도 못 살리는 사각이 된다.
    try:
        if subprocess.run(["pgrep", "-x", "keepalived"], capture_output=True,
                          timeout=2).returncode != 0:
            return False
    except Exception:
        pass
    local = {r.get("ip") for r in collect_interfaces() if r.get("ip")}
    return not any(v in local for v in vips)


def _find_cims_svc(install_path: str):
    for c in (os.path.join(install_path, "agent", "bin", "cims-svc"),
              os.path.join(_AGENT_DIR, "bin", "cims-svc"),
              os.path.join(_PREFIX, "agent", "current", "bin", "cims-svc")):
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


def _oam_preflight(install_path: str, timeout: int = 30) -> tuple:
    """D3 — OAM self-upgrade 시 구 OAM 을 내리기 전에 신 OAM 패키지가 뜰 수 있는지
    검증. install_path/oam/src/oam_app.py --preflight 를 sub-process 로 실행
    (oam_app.py 가 자기 sys.path/config 를 구성하므로 import·config 오류를 그대로 포착).
    반환 (ok: bool, msg: str)."""
    app = os.path.join(install_path, "oam", "src", "oam_app.py")
    if not os.path.isfile(app):
        return False, f"oam_app.py 없음: {app}"
    try:
        res = subprocess.run([sys.executable, "-u", app, "--preflight"],
                             capture_output=True, text=True, timeout=timeout,
                             cwd=os.path.dirname(app), env=dict(os.environ))
        if res.returncode == 0:
            return True, "preflight ok"
        tail = ((res.stderr or "") + (res.stdout or "")).strip().replace("\n", " ")[-200:]
        return False, f"rc={res.returncode} {tail}"
    except Exception as e:
        return False, f"preflight exec 실패: {e}"


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
    pid_dir = os.path.join(_PREFIX, "run")
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
            # 감독 경로 = 모듈의 current 통로 (없으면 prefix fallback — 구 평탄 설치)
            _cur = os.path.join(DEFAULT_INSTALL_ROOT, svc, "current")
            sup[svc] = _cur if os.path.isdir(_cur) else _PREFIX
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
    # supervisor 모드에선 HA 관리 모듈(relevant∪cold)의 lifecycle 은 Supervisor reconcile
    # 이 소유한다 — legacy watchdog 은 이중 제어를 피하려 그 집합을 건드리지 않는다.
    ha_managed = _ha_managed_modules() if _ha_flags()["verdict_source"] == "supervisor" else set()
    now = time.time()
    for svc, install_path in list(sup.items()):
        if svc in _NON_DAEMON_MODULES:
            continue
        if svc in ha_managed:
            continue     # Supervisor reconcile 소관
        if _pgrep_module(svc):
            _WATCHDOG_BACKOFF.pop(svc, None)     # 정상 — backoff 리셋
            _fail_reset(svc)                     # 실패 카운터 리셋 (연속성 판정 기준)
            continue
        # 모듈 감시 비활성 (service.json supervision.watchdog=false) — 운영자가 감시를
        # 끈 모듈은 재기동하지 않는다.
        if not _watchdog_enabled(svc):
            continue
        # 서버별 정지 오버라이드 — 운영자가 이 노드에서 내린 모듈은 재기동하지 않는다
        # (의도적 정지 ≠ 장애). desired 해제(서버별 start) 시 백스톱 복귀.
        if _desired_stopped(svc):
            _WATCHDOG_BACKOFF.pop(svc, None)
            continue
        # 조작 유예 — 제어 job(restart 등) 진행 중이면 잠시 손대지 않는다.
        if _in_op_grace(svc):
            continue
        # cold-spare standby — 정지가 desired state (notify 가 강등 시 내림).
        # 여기서 재기동하면 keepalived 와 엎치락뒤치락한다. VIP 취득 후엔 게이트가
        # 풀려 crash 재기동 백스톱으로 복귀.
        if _cold_standby_module(svc):
            _WATCHDOG_BACKOFF.pop(svc, None)
            _fail_reset(svc)                     # standby 진입 — 카운터 초기화 (승격 시 신선)
            continue
        st = _WATCHDOG_BACKOFF.setdefault(svc, {"ts": 0.0, "fails": 0})
        backoff = min(300, 5 * (2 ** st["fails"]))
        if now - st["ts"] < backoff:
            continue
        st["ts"] = now
        st["fails"] += 1
        # 재기동 실패 카운터 — cims-health 가 max_fails 초과 시 FAULT(절체) 판정.
        # 로컬 복구를 먼저 시도하고 소진되면 keepalived 가 VIP 를 넘긴다.
        rl = _ha_restart_limit(svc)
        fails = _fail_bump(svc, rl["window_sec"])
        nxt = min(300, 5 * (2 ** st["fails"]))
        print(f"[agent][watchdog] '{svc}' 다운 감지 — 재시작 (시도 {st['fails']}, "
              f"연속실패 {fails}/{rl['max_fails']}, 다음 backoff {nxt}s)", flush=True)
        rc, out, err = _run_cims_svc(install_path, "start", svc)
        tail = (err or out or "").strip().replace("\n", " ")[-160:]
        print(f"[agent][watchdog] '{svc}' start rc={rc} {tail}", flush=True)


# ──────────────────────────────────────────────────────────────
#  D1 — job report 전달 견고화 (특히 OAM self-upgrade 의 restart report)
#    신 OAM 콜드스타트로 일시 불통일 수 있어 짧은 재시도 후, 끝내 실패하면
#    pending 큐(jsonl)에 적재 → 다음 heartbeat 성공 시 flush. 결과 유실 방지.
# ──────────────────────────────────────────────────────────────
_PENDING_REPORT_FILE = os.path.join(_PREFIX, "run", "pending_reports.jsonl")


def _post_report(oam_url: str, token: str, result: dict, timeout: int = 15) -> int:
    st, _ = http_post(f"{oam_url}/api/agent/report", result,
                      headers={"X-Agent-Token": token}, timeout=timeout)
    return st


def _enqueue_pending_report(result: dict) -> None:
    try:
        os.makedirs(os.path.dirname(_PENDING_REPORT_FILE), exist_ok=True)
        with open(_PENDING_REPORT_FILE, "a") as f:
            f.write(json.dumps(result) + "\n")
    except Exception as e:
        print(f"[agent] pending report 적재 실패: {e}", flush=True)


def _flush_pending_reports(oam_url: str, token: str) -> None:
    """미전달 report 재전송 — 전송 성공분만 제거. heartbeat 성공 직후 1회 호출."""
    if not os.path.isfile(_PENDING_REPORT_FILE):
        return
    try:
        with open(_PENDING_REPORT_FILE) as f:
            lines = [ln for ln in f.read().splitlines() if ln.strip()]
    except Exception:
        return
    remaining = []
    for ln in lines:
        try:
            result = json.loads(ln)
        except Exception:
            continue   # 손상 줄은 폐기
        if _post_report(oam_url, token, result) != 200:
            remaining.append(ln)
    try:
        if remaining:
            with open(_PENDING_REPORT_FILE, "w") as f:
                f.write("\n".join(remaining) + "\n")
        else:
            os.unlink(_PENDING_REPORT_FILE)
            print("[agent] pending report 전량 전달 완료", flush=True)
    except Exception:
        pass


def _deliver_report(oam_url: str, token: str, result: dict, retries: int = 4) -> int:
    """report 전달(짧은 지수 backoff 재시도). 끝내 실패 시 pending 큐 적재.
    OAM self-upgrade 의 restart report 가 신 OAM 콜드스타트 창에서 유실되는 것을 방지
    (lifecycle 의 /health gate 와 합쳐 대개 1회 성공)."""
    delay = 1.0
    for _i in range(max(1, retries)):
        st = _post_report(oam_url, token, result)
        if st == 200:
            return st
        time.sleep(min(delay, 4.0))
        delay *= 2
    _enqueue_pending_report(result)
    print(f"[agent] report 전달 실패 — pending 큐 적재 (job_id={result.get('job_id')})", flush=True)
    return 0


def job_process_control(params: dict, job_type: str) -> tuple:
    """start/stop/restart — install_path/agent/bin/cims-svc 를 이용해 수행
    (Phase 1.B+, cims.sh 운영 명령 제거).
    cims-svc 에 CIMS_DIST_DIR=install_path 환경변수 전달 → cims-svc 가 install_path
    기준으로 DIST_DIR 결정 (install_path 의 csc/console 시작).
    """
    # 버전 전환 레이스 방어: deployment 레코드가 stale 이어도 설치 완료된
    # 버전 디렉토리가 있으면 그쪽을 실효 경로로 사용. (install_path = 타겟 버전 디렉토리)
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

    # 조작 유예 마커 — restart 순단·start 기동 시간 동안 cims-health/watchdog 가
    # 이 노드에서 절체/재기동 경쟁을 하지 않도록 (운영자 조작 ≠ 장애).
    _touch_op_grace(svc)
    # 노드 오버라이드 — 서버별 stop = 이 노드에서 의도적 정지(절체 안 함), start/restart
    # = 오버라이드 해제(HA 관리로 복귀). 절체 판정(cims-health)과 watchdog 이 참조.
    if job_type in ("start", "restart"):
        _set_desired(svc, None)
    elif job_type == "stop":
        _set_desired(svc, 'stopped')
        _fail_reset(svc)

    # ── current 심볼릭 모델: 활성 버전 통로 ────────────────────────────────
    #   install_path(버전 디렉토리)가 버전 패턴이면 module_root/current 로 통로화한다.
    #   프로세스는 항상 <module_root>/current 로 기동(CIMS_DIST_DIR) → systemd·모니터링
    #   고정 경로. legacy 평탄 설치(버전 패턴 아님)는 current 없이 install_path 직접.
    _ip = install_path.rstrip("/")
    is_versioned = bool(_VERSION_DIR_RE.match(os.path.basename(_ip)))
    module_root = os.path.dirname(_ip) if is_versioned else ""
    cur_path = os.path.join(module_root, "current") if module_root else ""
    launch_path = cur_path if cur_path else install_path

    # D3 (oam self-upgrade pre-flight): 구 OAM 을 내리기 전에 신 패키지(=타겟 버전
    # 디렉토리)가 뜰 수 있는지 검증. 실패면 아무것도 만지지 않고 반환(구 OAM 유지).
    if svc == "oam" and job_type in ("start", "restart"):
        ok_pf, pf_msg = _oam_preflight(install_path)
        if not ok_pf:
            return 1, "", f"oam preflight 실패 — 구 버전 유지 (kill 안 함): {pf_msg}"

    # start/restart: flip 직전 current 가 가리키던 버전(=직전 활성, 롤백 대상)을 보존하고
    # current 를 타겟 버전으로 flip. stop 은 flip 하지 않는다(도는 것을 그대로 내림).
    prev_vdir = ""
    if module_root and job_type in ("start", "restart"):
        if os.path.islink(cur_path) or os.path.exists(cur_path):
            prev_vdir = os.path.realpath(cur_path)
        _flip_current(module_root, install_path)

    # stale-stop: 타겟 버전 밖에서 도는 인스턴스(구버전)를 먼저 stop → 포트 바인드 충돌
    # 방지. current 통로에선 신·구 명령 경로가 같으므로 /proc/<pid>/exe 실경로로 식별한다.
    #   (구 인스턴스의 pid 파일은 자기 버전 run/ 에 있어 current 기준 stop 으론 안 잡힘 →
    #    그 버전 디렉토리로 직접 stop)
    prev_note = ""
    if module_root and job_type in ("start", "restart"):
        hit = _pgrep_module(svc)
        if hit:
            try:
                exe = os.path.realpath(f"/proc/{hit[0]}/exe")
            except OSError:
                exe = ""
            target_real = os.path.realpath(install_path) + os.sep
            if exe and not exe.startswith(target_real):
                stale_vdir = _module_vdir_from_exe(exe, module_root) or prev_vdir
                if stale_vdir and os.path.realpath(stale_vdir) != os.path.realpath(install_path) \
                        and os.path.isdir(stale_vdir):
                    prc, _po, perr = _run_cims_svc(stale_vdir, "stop", svc)
                    prev_note = (f" (prev-stop {os.path.basename(stale_vdir)} rc={prc}"
                                 f"{' ' + perr.strip()[:120] if prc else ''})")
    elif not module_root:
        # legacy 평탄 설치 — 구 동작 유지(supervised 경로 비교로 prev-stop)
        prev_path = _load_supervised().get(svc)
        if prev_path and prev_path != install_path and os.path.isdir(prev_path):
            prc, _po, perr = _run_cims_svc(prev_path, "stop", svc)
            prev_note = f" (prev-stop {prev_path} rc={prc}{' ' + perr.strip()[:120] if prc else ''})"

    # cold-spare standby — 실제 기동은 스킵 (current flip/prev-stop 은 이미 수행 →
    # 승격 시 notify/watchdog 이 신 버전으로 기동). supervised 에는 desired-state 로
    # 등록해 VIP 취득 후 watchdog 백스톱이 동작하게 한다.
    if job_type in ("start", "restart") and _cold_standby_module(svc):
        _mark_supervised(svc, launch_path)
        return 0, (f"cold standby — '{svc}' 기동 억제 (VIP 미보유; MASTER 승격 시 "
                   f"keepalived notify 가 기동){prev_note}"), ""

    rc, out, err = _run_cims_svc(launch_path, job_type, svc)
    if prev_note:
        out = (out or "") + prev_note

    # D4 (oam self-upgrade rollback): 신 OAM 이 health-gate 안에 못 뜨면(rc≠0) current 를
    # 직전 버전으로 flip-back + 구 버전 start. supervised 는 current 경로 일관.
    if rc != 0 and svc == "oam" and job_type in ("start", "restart") \
            and prev_vdir and os.path.realpath(prev_vdir) != os.path.realpath(install_path) \
            and os.path.isdir(prev_vdir):
        _flip_current(module_root, prev_vdir)
        r_rc, r_out, r_err = _run_cims_svc(cur_path, "start", svc)
        _mark_supervised(svc, cur_path)
        if r_rc == 0:
            return 1, (out or "") + f" [rolled_back→{os.path.basename(prev_vdir)}]", \
                   (err or "") + " rollback ok"
        return 1, (out or ""), (err or "") + f" [rollback start rc={r_rc}: {(r_err or r_out)[:160]}]"

    # 모듈 감독 desired-state 갱신 — start/restart 성공 → 감독 등록(current 통로), stop → 해제.
    sup_path = launch_path
    if rc == 0:
        if job_type in ("start", "restart"):
            _mark_supervised(svc, sup_path)
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
        "csc":     [("tcp", 4421)],
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
    install_dir = _PREFIX
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


def job_rollback_agent(params: dict) -> tuple:
    """agent 롤백 — `<prefix>/agent/current` 를 직전(또는 params.version) 버전으로 flip.

    버전 디렉토리는 prune(최신 3개)까지 보존되므로 다운로드 불요 — 순수 심볼릭 flip.
    systemd ExecStart·sudoers 가 `agent/current` 고정 경로라 flip 만으로 충분.
    성공(rc 0) 시 호출자(run_loop)가 `os.execv` 로 self-restart → 구버전 코드 기동."""
    root = os.path.join(_PREFIX, "agent")
    cur_link = os.path.join(root, "current")
    cur = os.path.basename(os.path.realpath(cur_link)) if os.path.exists(cur_link) else ""
    target = (params.get("version") or "").strip()
    if not target:
        cands = [v for v in _agent_versions() if v != cur]
        target = cands[0] if cands else ""   # 직전(mtime 최신) 버전
    if not target:
        return 1, "", "no rollback target — 설치된 직전 agent 버전 없음 (단일 버전)"
    tdir = os.path.join(root, target)
    if not os.path.isfile(os.path.join(tdir, "cims_agent.py")):
        return 1, "", f"rollback 대상 버전 미설치: {target} ({tdir})"
    if not _flip_current(root, tdir):
        return 1, "", f"agent current flip 실패 (target={target})"
    return 0, f"agent rolled back to {target} (was {cur}) — execv self-restart", ""


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
            # upgrade = 신 파일 설치 + 재기동(신 코드 로드). install 만 하면 구 프로세스가
            #   구 코드를 계속 실행한다(파일만 교체). restart 는 job_process_control 경유 →
            #   oam self-upgrade preflight(D3)/rollback(D4) 안전장치 그대로 적용.
            rc, out, err = job_install(params, oam_url, session_token)
            if rc == 0:
                rc_r, out_r, err_r = job_process_control(params, "restart")
                out = (out or "") + f"\n[upgrade→restart] rc={rc_r} {(out_r or '')[-300:]}"
                if rc_r != 0:
                    rc = rc_r
                    err = (err or "") + f" [restart] {(err_r or '')[-300:]}"
        elif jt == "upgrade_agent":
            rc, out, err = job_upgrade_agent(oam_url, session_token, agent_name)
        elif jt == "rollback_agent":
            # agent 롤백 — current 를 직전/지정 버전으로 flip. heartbeat loop 가 execv 처리.
            rc, out, err = job_rollback_agent(params)
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
        elif jt == "update_module_spec":
            rc, out, err = job_update_module_spec(params)
        elif jt == "ha_keepalived":
            rc, out, err = job_ha_keepalived(params)
        elif jt == "apply_ip_config":
            rc, out, err = job_apply_ip_config(params)
        elif jt == "apply_mounts":
            rc, out, err = job_apply_mounts(params)
        elif jt == "apply_net_tuning":
            rc, out, err = job_apply_net_tuning(params)
        elif jt == "uninstall":
            install_path = params.get("install_path")
            # 감독 해제 (watchdog 가 재시작하지 않도록) + HA 노드 상태 정리
            _mod = (params.get("process_name") or params.get("service_kind") or "").lower()
            _unmark_supervised(_mod)
            _set_desired(_mod, None)   # 오버라이드 잔재 제거 (service.json 은 모듈 트리와 함께 rmtree)
            _fail_reset(_mod)
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
            agent_root = _PREFIX
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


def _ensure_nonlocal_bind() -> None:
    """net.ipv4.ip_nonlocal_bind=1 선행 보장 (idempotent, 1회).

    VIP 를 설정값으로 bind 하는 모듈(csp LocalIp=VIP 등)은 VIP 취득 전에도 기동
    가능해야 한다 — 워크플로가 "start → VIP 적용" 순서라 cims-ha apply 의 설정
    (VIP 적용 시점)만으로는 늦다: 최초 start 가 bind EADDRNOTAVAIL 로 실패해
    watchdog crash-loop 가 된다. sudoers 미등록(dev) 환경은 로그만 남기고 무시."""
    try:
        with open("/proc/sys/net/ipv4/ip_nonlocal_bind") as f:
            if f.read().strip() == "1":
                return
    except Exception:
        pass
    priv = _resolve_cims_priv()
    if not priv:
        print("[agent] cims-priv 미발견 — ip_nonlocal_bind 설정 스킵", flush=True)
        return
    try:
        r = subprocess.run(["sudo", "-n", priv, "net-sysctl",
                            "net.ipv4.ip_nonlocal_bind", "1"],
                           capture_output=True, text=True, timeout=15)
        if r.returncode == 0:
            print("[agent] net.ipv4.ip_nonlocal_bind=1 적용 (VIP 선행 bind 보장)", flush=True)
        else:
            print(f"[agent] ip_nonlocal_bind 설정 실패(무시): rc={r.returncode} "
                  f"{(r.stderr or r.stdout).strip()[-120:]}", flush=True)
    except Exception as e:
        print(f"[agent] ip_nonlocal_bind 설정 예외(무시): {e}", flush=True)


def _ensure_unit_killmode() -> None:
    """user unit 에 KillMode=process drop-in 보장 — agent 재기동(업그레이드 포함)이
    자기 cgroup 의 모듈(cims-svc & 백그라운드 데몬)까지 동반 종료시키지 않게 한다.
    fresh install 의 unit 에는 포함되지만 update.sh(--update-only)는 unit 을 유지
    하므로, 기존 설치본은 agent 가 기동 시 스스로 drop-in 으로 교정한다 (idempotent,
    systemd 미사용(nohup) 환경은 daemon-reload 실패를 무시)."""
    dropin_dir = os.path.join(os.path.expanduser("~"), ".config", "systemd", "user",
                              "cims-agent.service.d")
    dropin = os.path.join(dropin_dir, "10-cims-killmode.conf")
    content = "[Service]\nKillMode=process\n"
    try:
        if os.path.isfile(dropin):
            with open(dropin) as f:
                if f.read() == content:
                    return
        os.makedirs(dropin_dir, exist_ok=True)
        with open(dropin, "w") as f:
            f.write(content)
        subprocess.run(["systemctl", "--user", "daemon-reload"],
                       capture_output=True, timeout=10)
        print("[agent] systemd drop-in 적용: KillMode=process (agent 재기동 시 모듈 동반종료 차단)", flush=True)
    except Exception as e:
        print(f"[agent] KillMode drop-in 적용 실패(무시): {e}", flush=True)


def _sleep_with_supervision(total_sec: float) -> None:
    """total_sec 대기하되 SUPERVISE_INTERVAL_SEC 간격으로 watchdog tick 을 돌린다 —
    heartbeat 대기(정상 heartbeat_sec, OAM 장애 backoff 최대 60s)와 죽은 모듈 감지
    주기를 분리 (OAM 불통이 로컬 복구를 지연시키지 않게)."""
    deadline = time.time() + total_sec
    while True:
        remain = deadline - time.time()
        if remain <= 0:
            return
        time.sleep(min(remain, SUPERVISE_INTERVAL_SEC))
        try:
            supervise_tick()
        except Exception as e:
            print(f"[agent][watchdog] tick error: {e}", flush=True)


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

    _ensure_ha_dirs()                  # HA 상태 디렉토리 골격(run/ha·state/ha) 보장 (1회)
    _write_ha_flags_file()             # run/ha/flags.json — root(cims-notify/health) 가 모드 판독
    # HA Supervisor(Health Scheduler + Evaluator [+ reconcile]) — flag 활성 시에만.
    #   verdict_source=supervisor : 실소비 — reconcile 활성(모듈 lifecycle 주체가 Supervisor)
    #   CIMS_HA_SHADOW=1          : shadow — verdict 계산·로그만(재기동/keepalived 무영향)
    #   CIMS_HA_HEALTH=1          : health 캐시만. legacy 기본에선 전부 미기동 → 동작 무변경.
    _ha_f = _ha_flags()
    if _ha_f["verdict_source"] == "supervisor":
        _start_health_scheduler()
        _start_ha_evaluator(reconcile=True)
    elif os.environ.get("CIMS_HA_SHADOW"):
        _start_health_scheduler()
        _start_ha_evaluator(reconcile=False)
    elif os.environ.get("CIMS_HA_HEALTH"):
        _start_health_scheduler()
    _seed_supervised_from_pidfiles()   # 기존 실행 모듈을 감독 집합에 편입 (1회)
    _ensure_unit_killmode()            # 자기 unit 에 KillMode=process 보장 (기존 설치본 자가 교정, 1회)
    _ensure_nonlocal_bind()            # VIP 선행 bind 보장 — csp(LocalIp=VIP) 가 VIP 적용 전에도 기동 가능 (1회)
    ensure_base_deps()                 # vendor deb 균일 설치(keepalived/nfs/lib) — 실행은 config 제어. ip/네트워크 수집 전제, idempotent
    reapply_managed_ips()              # 재부팅으로 소실된 cims-managed service IP 자력 복원 (1회, OAM 무관)
    reapply_net_tuning()               # 재부팅으로 소실된 RPS(rps_cpus) 자력 복원 (1회, sysctl 은 sysctl.d 가 처리)

    next_metric = 0
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
            # 호스트 사양도 매 heartbeat 동봉 — enroll 시점 스냅샷이 영구 표시되어
            # VM 스펙 변경(코어/메모리 증설) 후 콘솔 정보가 실제와 다르던 문제 교정.
            try:
                _hi = collect_host_info()
                for k in ("hostname", "os_info", "cpu_cores", "memory_mb", "disk_gb",
                          "agent_version", "agent_versions"):
                    if _hi.get(k):
                        hb_body[k] = _hi[k]
            except Exception:
                pass
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
                # D1: 직전에 전달 못 한 job report 가 있으면 (신 OAM 이 이제 떴으니) flush.
                _flush_pending_reports(oam_url, state.session_token)
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
                    # D1: report 전달 견고화 — 재시도 후 실패 시 pending 큐 적재.
                    # (OAM self-upgrade 의 restart 직후엔 신 OAM 콜드스타트 창과 겹칠 수 있음)
                    rep_status = _deliver_report(oam_url, state.session_token, result)
                    print(f"[agent] report status={rep_status} rc={result['result_code']}", flush=True)
                    # upgrade_agent / agent_restart 성공 시 새 코드 image 로 self-exec.
                    # systemd 환경: execv 가 모든 fd close + 같은 PID 로 새 image 실행 (Restart=always 보다 빠름)
                    # nohup 환경 (no-systemd): execv 가 유일한 재기동 경로 — 부모 shell 이 죽었으므로 외부 monitor 없음
                    if job["type"] in ("upgrade_agent", "rollback_agent", "agent_restart") and result["result_code"] == 0:
                        action = {"upgrade_agent": "upgrade", "rollback_agent": "rollback"}.get(job["type"], "restart")
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

        if fail_count == 0:
            sleep_sec = heartbeat_sec
        else:
            sleep_sec = min(5 * (2 ** (fail_count - 1)), max_backoff)
            print(f"[agent] HA backoff sleep {sleep_sec}s (fail_count={fail_count})", flush=True)
        # 모듈 감독은 heartbeat 대기와 무관하게 SUPERVISE_INTERVAL_SEC 주기 유지 —
        # OAM 장애 backoff(최대 60s)가 죽은 모듈 복구까지 함께 지연시키지 않는다.
        _sleep_with_supervision(sleep_sec)


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
    # 시작 배너 — 실행 중 agent 버전 가시화(self-upgrade/execv 후 새 코드 로드 확인 + 운영 디버깅).
    try:
        _av = json.load(open(os.path.join(_AGENT_DIR, "pkg.json"))).get("version", "?")
    except Exception:
        _av = "?"
    print(f"[agent] === CIMS agent start (version={_av}, pid={os.getpid()}) ===", flush=True)

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
