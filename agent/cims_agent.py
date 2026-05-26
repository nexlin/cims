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
DEFAULT_HEARTBEAT_SEC = 30
DEFAULT_METRIC_SEC = 60
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
    return info


# mgmt IP — agent 시작 시 csc_url 의 outgoing local IP 로 결정 후 캐시.
# collect_interfaces() 가 이 IP 와 매칭되는 row 에 mgmt=True 플래그를 붙임.
_MGMT_IP: str | None = None


def detect_mgmt_ip(csc_url: str) -> str | None:
    """csc_url 로 가는 outgoing local IP 반환 — 그 IP 의 NIC 이 mgmt (CSC 통신용).
    UDP socket 의 connect 로 routing table 만 평가 (실제 패킷 송신 없음).
    """
    try:
        parsed = urllib.parse.urlparse(csc_url)
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


def collect_metrics() -> dict:
    """CPU/mem/disk percent + load + CIMS 프로세스 목록."""
    m = {}
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
    # processes — CIMS 바이너리
    m["processes"] = []
    for procname in ("csp", "cmp", "csc", "cwrtc"):
        try:
            out = subprocess.run(["pgrep", "-a", procname], capture_output=True, text=True, timeout=2)
            for line in out.stdout.splitlines():
                parts = line.split(maxsplit=1)
                if len(parts) >= 1:
                    m["processes"].append({
                        "name": procname,
                        "pid": int(parts[0]),
                        "cmdline": parts[1] if len(parts) > 1 else "",
                    })
                    break
        except Exception:
            pass
    return m


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
    """params.install_path 가 명시되면 그대로 (단 쓰기 불가 시 cwd fallback — dev/netns 환경).
    명시 안 되면 modules/<m>/<v>/<p>/ 로 조합."""
    explicit = params.get("install_path")
    if explicit:
        # 부모 디렉토리 쓰기 가능 여부 체크 — 디렉토리 자체가 없을 수도 있음
        try:
            if os.path.isdir(explicit) and os.access(explicit, os.W_OK):
                return explicit
            parent = os.path.dirname(explicit) or "/"
            if os.path.isdir(parent) and os.access(parent, os.W_OK):
                return explicit
        except Exception:
            pass
        # 쓰기 불가 — cwd fallback (dev/netns 환경: /opt/cims 권한 없음)
        return os.getcwd()
    return os.path.join(DEFAULT_INSTALL_ROOT, _default_install_subpath(params))


def _write_config_file(install_path: str, config_values: dict) -> str:
    """install_path/config.json 에 설정 값 기록. 경로 반환."""
    cfg_path = os.path.join(install_path, "config.json")
    os.makedirs(install_path, exist_ok=True)
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(config_values or {}, f, ensure_ascii=False, indent=2)
    return cfg_path


def _find_previous_install(module: str, process: str, current_version: str) -> str:
    """같은 모듈의 이전 버전 install_path 찾기 (mtime 최신 1개).

    새 버전 설치 시 기존 config/ 를 이관하기 위한 조회. install_subpath 가
    `<module>/<version>` 으로 단축됨 (옛 `<module>/<version>/<process>` 에서).
    process 파라미터는 signature 호환 위해 유지하지만 미사용.
    """
    del process  # 호환용; install layout 단축 (modules/<module>/<version>/) 으로 미사용.
    module_root = os.path.join(DEFAULT_INSTALL_ROOT, module)
    if not os.path.isdir(module_root): return ""
    candidates = []
    for v in os.listdir(module_root):
        if v == current_version: continue
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


def job_install(params: dict, csc_url: str, session_token: str) -> tuple:
    """PKG 다운로드 + tarball 풀어 install_path 에 설치. config.json 도 함께 기록.

    새 버전이고 같은 모듈/프로세스의 이전 버전이 존재하면
    이전 install_path 의 config/ 와 config.json 을 신규 경로로 복사(자동 이관).

    멀티-변종 install (같은 install_path 에 csp/ 와 isp/ 처럼 sibling 디렉토리 공존)
    지원: tarball 의 단일 top-level 디렉토리만 wipe/backup 범위로 좁힘. 형제 디렉토리
    (예: csp install 시 isp/) 영향 없음.
    """
    pkg_id = params.get("package_id")
    install_path = _resolve_install_path(params)
    if not pkg_id:
        return 1, "", "package_id missing"

    url = f"{csc_url}/api/agent/package/{pkg_id}"
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

    # 이전 버전 config 이관 (같은 모듈/프로세스, 다른 버전)
    migrated = ""
    module  = (params.get("package_name") or "").strip()
    version = (params.get("package_version") or "").strip()
    process = (params.get("process_name") or "").strip().upper() or (module.upper() if module else "")
    if module and process and version:
        prev = _find_previous_install(module, process, version)
        if prev and prev != install_path:
            try:
                prev_cfg = os.path.join(prev, "config")
                new_cfg  = os.path.join(cfg_target_dir, "config")
                if os.path.isdir(prev_cfg) and not os.path.isdir(new_cfg):
                    shutil.copytree(prev_cfg, new_cfg, symlinks=True)
                    migrated = f" (config migrated from {prev})"
                prev_scalar = os.path.join(prev, "config.json")
                new_scalar  = os.path.join(cfg_target_dir, "config.json")
                if os.path.isfile(prev_scalar) and not os.path.isfile(new_scalar) \
                        and not (params.get("config")):
                    shutil.copy2(prev_scalar, new_scalar)
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

    return 0, (f"installed pkg_id={pkg_id} at {install_path} ({len(data)} bytes) "
               f"config={cfg_path}{migrated}"), ""


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


def job_update_config(params: dict, csc_url: str = "", session_token: str = "") -> tuple:
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
    install_path = _resolve_install_path(params)
    if not os.path.isdir(install_path):
        return _sync_ack_and_return(csc_url, session_token, sync_id,
                                    rc=1, err=f"install_path not found: {install_path}")
    try:
        cfg_path = _write_config_file(install_path, params.get("config") or {})
    except Exception as e:
        return _sync_ack_and_return(csc_url, session_token, sync_id,
                                    rc=2, err=f"write config failed: {e}")
    pkg_subdir = _resolve_pkg_subdir(install_path, params)
    _, signaled = _signal_process(install_path, "usr1", pkg_subdir=pkg_subdir)
    return _sync_ack_and_return(csc_url, session_token, sync_id,
                                rc=0, out=f"config updated: {cfg_path} signaled={signaled}")


def job_sync_config(params: dict, csc_url: str, session_token: str) -> tuple:
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
    install_path = _resolve_install_path(params)
    if not install_path or not os.path.isdir(install_path):
        return _sync_ack_and_return(csc_url, session_token, sync_id,
                                    rc=1, err=f"install_path not found: {install_path}")
    if not collection:
        return _sync_ack_and_return(csc_url, session_token, sync_id,
                                    rc=1, err="collection missing")

    # 1) csc 에서 컬렉션 pull
    pull_url = f"{csc_url}/api/agent/csp-config/{collection}"
    status, body = http_get_json(pull_url,
                                 headers={"X-Agent-Token": session_token},
                                 timeout=30)
    if status != 200 or not isinstance(body, dict) or "items" not in body:
        return _sync_ack_and_return(csc_url, session_token, sync_id,
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
        return _sync_ack_and_return(csc_url, session_token, sync_id,
                                    rc=3, err=f"write failed: {e}")

    # 3) SIGUSR1 → 로컬 CSP (PID file 기반 — pkg_subdir 자동 탐색)
    pkg_subdir = _resolve_pkg_subdir(install_path, params)
    _, signaled = _signal_process(install_path, "usr1", pkg_subdir=pkg_subdir)

    msg = (f"sync_config ok: collection={collection} op={op} rows={n} "
           f"etag={etag} signaled={signaled}")
    return _sync_ack_and_return(csc_url, session_token, sync_id,
                                rc=0, out=msg)


def _sync_ack_and_return(csc_url: str, session_token: str, sync_id,
                         *, rc: int, out: str = "", err: str = "") -> tuple:
    """csc 에 ack/nack 보고 후 결과 튜플 반환. csc 호출 실패는 로그만 (rc 유지)."""
    if sync_id is None:
        return rc, out, err
    ack_url = f"{csc_url}/api/agent/sync/{int(sync_id)}/ack"
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

    # cims-ha config + apply — sudoers 화이트리스트의 dev dist canonical 사용
    # ha.json 위치는 install_path 별로 다르므로 --ha-dir 인자로 전달.
    msgs = [f"ha.json updated: {ha_path}"]
    cims_ha = _resolve_cims_ha()
    ha_dir = os.path.dirname(ha_path)
    if cims_ha:
        try:
            r1 = subprocess.run(["sudo", "-n", cims_ha, "--ha-dir", ha_dir, "config"],
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
    return rc, "\n".join(msgs), ""


def job_process_control(params: dict, job_type: str) -> tuple:
    """start/stop/restart — install_path/agent/bin/cims-svc 를 이용해 수행
    (Phase 1.B+, cims.sh 운영 명령 제거).
    cims-svc 에 CIMS_DIST_DIR=install_path 환경변수 전달 → cims-svc 가 install_path
    기준으로 DIST_DIR 결정 (install_path 의 csc/console 시작).
    """
    install_path = _resolve_install_path(params)
    svc = (params.get("process_name") or params.get("service_kind") or "").lower()
    # 우선순위:
    #  1) install_path/agent/bin/cims-svc — 모듈 자체에 운영 도구를 ship 하는 경우 (구식)
    #  2) _AGENT_DIR/bin/cims-svc — 일반 케이스. 에이전트가 자기 옆 bin/cims-svc 사용
    #     (install-agent.sh 가 /opt/cims-agent/agent/bin/ 에 둠).
    #  3) /opt/cims-agent/agent/bin/cims-svc — agent 가 다른 곳에서 실행되는 경우 명시 fallback
    candidates = [
        os.path.join(install_path, "agent", "bin", "cims-svc"),
        os.path.join(_AGENT_DIR, "bin", "cims-svc"),
        "/opt/cims-agent/agent/bin/cims-svc",
    ]
    script = next((c for c in candidates if os.path.isfile(c)), None)
    if not script:
        return 1, "", f"cims-svc not found (install_path={install_path}, agent_dir={_AGENT_DIR})"

    argv = [script, job_type]
    if svc: argv.append(svc)
    env = dict(os.environ)
    env["CIMS_DIST_DIR"] = install_path
    try:
        res = subprocess.run(argv, capture_output=True, text=True, timeout=60,
                              cwd=install_path, env=env)
        return res.returncode, res.stdout[-4000:], res.stderr[-2000:]
    except Exception as e:
        return 2, "", f"exec failed: {e}"


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


def job_upgrade_agent(csc_url: str, session_token: str) -> tuple:
    """새 agent 바이너리를 CSC 에서 받아 자기 자신 교체. 호출자가 종료 처리."""
    src_url = f"{csc_url}/cims_agent.py"
    status, data, meta = http_get_binary(src_url, {"X-Agent-Token": session_token})
    if status != 200 or not data or len(data) < 1024:
        return 1, "", f"download failed status={status} size={len(data) if data else 0}"
    my_path = os.path.abspath(__file__)
    new_path = my_path + ".new"
    try:
        with open(new_path, "wb") as f:
            f.write(data)
        os.chmod(new_path, 0o755)
        os.replace(new_path, my_path)   # atomic
    except Exception as e:
        try: os.unlink(new_path)
        except Exception: pass
        return 2, "", f"replace failed: {e}"
    return 0, f"upgraded {my_path} ({len(data)} bytes) — restarting", ""


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


def execute_job(job: dict, csc_url: str, session_token: str) -> dict:
    jt = job["type"]
    params = job.get("params") or {}
    try:
        if jt == "install":
            rc, out, err = job_install(params, csc_url, session_token)
        elif jt == "upgrade":
            rc, out, err = job_install(params, csc_url, session_token)
        elif jt == "upgrade_agent":
            rc, out, err = job_upgrade_agent(csc_url, session_token)
        elif jt == "agent_restart":
            # agent 자체 self-restart. heartbeat loop 가 execv 처리.
            rc, out, err = 0, "agent restart requested — execv self", ""
        elif jt in ("start", "stop", "restart"):
            rc, out, err = job_process_control(params, jt)
        elif jt == "update_config":
            rc, out, err = job_update_config(params, csc_url, session_token)
        elif jt == "sync_config":
            rc, out, err = job_sync_config(params, csc_url, session_token)
        elif jt == "update_ha":
            rc, out, err = job_update_ha(params)
        elif jt == "apply_ip_config":
            rc, out, err = job_apply_ip_config(params)
        elif jt == "uninstall":
            install_path = params.get("install_path")
            if install_path and os.path.isdir(install_path):
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

def enroll(csc_url: str, enrollment_token: str, state: AgentState, name: str) -> bool:
    info = collect_host_info()
    payload = {"enrollment_token": enrollment_token, **info}
    status, resp = http_post(f"{csc_url}/api/agent/enroll", payload)
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


def rotate_mtls_cert(csc_url: str, state: AgentState) -> bool:
    """CSC 에 새 mTLS cert 발급 요청 → state_dir 에 저장. 성공 시 True.

    발급 성공 후에도 현재 실행 중인 sync REST 서버는 옛 cert 를 메모리에 쥐고 있음.
    호출자(run_loop) 가 프로세스 종료하면 systemd 가 재기동하면서 새 cert 를 읽음.
    """
    status, resp = http_post(f"{csc_url}/api/agent/cert/rotate", {},
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


def run_loop(csc_url: str, state: AgentState, heartbeat_sec: int, metric_sec: int,
             sync_port: int = 0):
    """
    Heartbeat 루프.

    HA 환경 (csc_url 이 VIP_csc 가리킴) 에서 fail-over 가 진행되는 약 3~7초 동안은
    connection refused / timeout 이 발생하므로 짧은 exponential backoff 로 복구 시도.
    정상 회차 sleep 은 heartbeat_sec, 실패 회차는 5s → 10s → 20s → max(heartbeat_sec, 60s).
    """
    # CSC 통신 NIC 식별 — collect_interfaces() 가 mgmt 플래그 부여 시 사용.
    global _MGMT_IP
    _MGMT_IP = detect_mgmt_ip(csc_url)

    next_metric = 0
    fail_count = 0
    max_backoff = max(heartbeat_sec, 60)
    while True:
        try:
            hb_body = {
                "interfaces": collect_interfaces(),
                "routes": collect_routes(),
                "agent_version": AGENT_VERSION,
            }
            if sync_port: hb_body["sync_port"] = sync_port
            status, resp = http_post(f"{csc_url}/api/agent/heartbeat", hb_body,
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
                    if rotate_mtls_cert(csc_url, state):
                        print("[agent] exiting for systemd restart with new cert", flush=True)
                        return 0

                jobs = resp.get("jobs") or []
                for job in jobs:
                    print(f"[agent] exec job id={job['id']} type={job['type']}", flush=True)
                    result = execute_job(job, csc_url, state.session_token)
                    rep_status, rep_body = http_post(f"{csc_url}/api/agent/report", result,
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
                http_post(f"{csc_url}/api/agent/metric", metrics,
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
        time.sleep(sleep_sec)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csc-url", required=True)
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

    state = AgentState(args.state_dir)
    if not state.session_token:
        if not args.enrollment_token:
            print("[agent] first run requires --enrollment-token")
            return 1
        if not enroll(args.csc_url, args.enrollment_token, state, args.name):
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

    return run_loop(args.csc_url, state, args.heartbeat_sec, args.metric_sec, sync_port)


if __name__ == "__main__":
    sys.exit(main() or 0)
