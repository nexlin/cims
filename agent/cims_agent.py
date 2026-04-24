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
AGENT_VERSION = "0.1.0"


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
    return info


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
    """설치 경로 결정: modules/<module>/<version>/<process>/

    - module:  params["package_name"] (없으면 pkg-<id>)
    - version: params["package_version"] (없으면 "unknown")
    - process: params["process_name"] > service_kind(legacy) > module 대문자
    """
    module = (params.get("package_name") or f"pkg-{params.get('package_id','unknown')}").strip()
    version = (params.get("package_version") or "unknown").strip()
    process = (params.get("process_name") or params.get("service_kind") or "").strip()
    if not process:
        process = module.upper()
    return os.path.join(module, version, process)


def _resolve_install_path(params: dict) -> str:
    """params.install_path 가 명시되면 그대로. 아니면 modules/<m>/<v>/<p>/ 로 조합."""
    explicit = params.get("install_path")
    if explicit:
        return explicit
    return os.path.join(DEFAULT_INSTALL_ROOT, _default_install_subpath(params))


def _write_config_file(install_path: str, config_values: dict) -> str:
    """install_path/config.json 에 설정 값 기록. 경로 반환."""
    cfg_path = os.path.join(install_path, "config.json")
    os.makedirs(install_path, exist_ok=True)
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(config_values or {}, f, ensure_ascii=False, indent=2)
    return cfg_path


def _find_previous_install(module: str, process: str, current_version: str) -> str:
    """같은 모듈/프로세스의 이전 버전 install_path 찾기 (mtime 최신 1개).

    새 버전 설치 시 기존 config/ 를 이관하기 위한 조회.
    """
    module_root = os.path.join(DEFAULT_INSTALL_ROOT, module)
    if not os.path.isdir(module_root): return ""
    candidates = []
    for v in os.listdir(module_root):
        if v == current_version: continue
        p = os.path.join(module_root, v, process)
        if os.path.isdir(p):
            try: candidates.append((os.path.getmtime(p), p))
            except OSError: pass
    if not candidates: return ""
    candidates.sort(reverse=True)
    return candidates[0][1]


def job_install(params: dict, csc_url: str, session_token: str) -> tuple:
    """PKG 다운로드 + tarball 풀어 install_path 에 설치. config.json 도 함께 기록.

    새 버전이고 같은 모듈/프로세스의 이전 버전이 존재하면
    이전 install_path 의 config/ 와 config.json 을 신규 경로로 복사(자동 이관).
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
    # 기존 내용 백업
    backup = install_path + ".prev"
    if os.path.isdir(install_path) and os.listdir(install_path):
        try:
            if os.path.isdir(backup): shutil.rmtree(backup)
            shutil.copytree(install_path, backup, symlinks=True, dirs_exist_ok=True)
        except Exception:
            pass

    tar_path = os.path.join(install_path, "_pkg.tar.gz")
    with open(tar_path, "wb") as f:
        f.write(data)

    try:
        with tarfile.open(tar_path, "r:gz") as tf:
            tf.extractall(install_path)
    except Exception as e:
        return 4, "", f"untar failed: {e}"
    finally:
        try: os.unlink(tar_path)
        except Exception: pass

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
                new_cfg  = os.path.join(install_path, "config")
                if os.path.isdir(prev_cfg) and not os.path.isdir(new_cfg):
                    shutil.copytree(prev_cfg, new_cfg, symlinks=True)
                    migrated = f" (config migrated from {prev})"
                prev_scalar = os.path.join(prev, "config.json")
                new_scalar  = os.path.join(install_path, "config.json")
                if os.path.isfile(prev_scalar) and not os.path.isfile(new_scalar) \
                        and not (params.get("config")):
                    shutil.copy2(prev_scalar, new_scalar)
            except Exception as e:
                return 6, "", f"config migration failed: {e}"

    # 설정 파일 기록 (params.config 가 있으면 사용자의 값, 없으면 빈 dict — 이관된 파일 유지)
    cfg_path = ""
    try:
        new_scalar = os.path.join(install_path, "config.json")
        if params.get("config") or not os.path.isfile(new_scalar):
            cfg_path = _write_config_file(install_path, params.get("config") or {})
        else:
            cfg_path = new_scalar
    except Exception as e:
        return 5, "", f"write config failed: {e}"

    # config/ 디렉토리 기본 생성 (collection 저장소)
    os.makedirs(os.path.join(install_path, "config"), exist_ok=True)

    return 0, (f"installed pkg_id={pkg_id} at {install_path} ({len(data)} bytes) "
               f"config={cfg_path}{migrated}"), ""


def job_update_config(params: dict) -> tuple:
    """install_path/config.json 만 재기록. 프로세스 재시작은 수동."""
    install_path = _resolve_install_path(params)
    if not os.path.isdir(install_path):
        return 1, "", f"install_path not found: {install_path}"
    try:
        cfg_path = _write_config_file(install_path, params.get("config") or {})
    except Exception as e:
        return 2, "", f"write config failed: {e}"
    return 0, f"config updated: {cfg_path}", ""


def job_process_control(params: dict, job_type: str) -> tuple:
    """start/stop/restart — install_path/cims.sh 을 이용해 수행."""
    install_path = _resolve_install_path(params)
    svc = (params.get("process_name") or params.get("service_kind") or "").lower()
    script = os.path.join(install_path, "cims.sh")
    if not os.path.isfile(script):
        # 후보: /home/nex/work/cims/build/dist/cims.sh (개발환경)
        for cand in ("/home/nex/work/cims/build/dist/cims.sh",):
            if os.path.isfile(cand):
                script = cand
                break
    if not os.path.isfile(script):
        return 1, "", f"cims.sh not found (install_path={install_path})"

    argv = [script, job_type]
    if svc: argv.append(svc)
    try:
        res = subprocess.run(argv, capture_output=True, text=True, timeout=60,
                              cwd=os.path.dirname(script))
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


def _signal_process(install_path: str, sig_name: str) -> tuple:
    """install_path 안의 run/<*.pid> 파일을 읽어 SIGUSR1(기본)/SIGHUP 전송."""
    sig = signal.SIGUSR1 if sig_name == "usr1" else signal.SIGHUP
    pid_dir = os.path.join(install_path, "run")
    if not os.path.isdir(pid_dir):
        # fallback: install_path 안에서 *.pid
        pid_dir = install_path
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
        elif jt in ("start", "stop", "restart"):
            rc, out, err = job_process_control(params, jt)
        elif jt == "update_config":
            rc, out, err = job_update_config(params)
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
    next_metric = 0
    while True:
        try:
            hb_body = {}
            if sync_port: hb_body["sync_port"] = sync_port
            status, resp = http_post(f"{csc_url}/api/agent/heartbeat", hb_body,
                                     headers={"X-Agent-Token": state.session_token})
            if status == 401:
                print("[agent] session token revoked; exiting")
                return 1
            if status == 200:
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
                    # upgrade_agent 성공 시 자기 자신 종료 → systemd Restart=always 가 새 바이너리로 재기동
                    if job["type"] == "upgrade_agent" and result["result_code"] == 0:
                        print("[agent] upgrade done — exiting for systemd restart", flush=True)
                        return 0

            if time.time() >= next_metric:
                metrics = collect_metrics()
                http_post(f"{csc_url}/api/agent/metric", metrics,
                          headers={"X-Agent-Token": state.session_token})
                next_metric = time.time() + metric_sec
        except Exception as e:
            print(f"[agent] loop error: {e}")
        time.sleep(heartbeat_sec)


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

    sync_port = 0
    if args.sync_port > 0:
        try:
            sync_port = start_sync_server(state, args.state_dir, args.sync_port)
        except Exception as e:
            print(f"[agent] sync server start failed: {e}", flush=True)

    return run_loop(args.csc_url, state, args.heartbeat_sec, args.metric_sec, sync_port)


if __name__ == "__main__":
    sys.exit(main() or 0)
