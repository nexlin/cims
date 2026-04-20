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
import time
import traceback
import urllib.parse
import urllib.request
from datetime import datetime

DEFAULT_STATE_DIR = os.environ.get(
    "CIMS_AGENT_STATE",
    os.path.expanduser("~/.local/state/cims-agent"),
)
DEFAULT_INSTALL_ROOT = os.environ.get("CIMS_AGENT_INSTALL_ROOT", "/opt/cims")
DEFAULT_HEARTBEAT_SEC = 30
DEFAULT_METRIC_SEC = 60
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

def job_install(params: dict, csc_url: str, session_token: str) -> tuple:
    """PKG 다운로드 + tarball 풀어 install_path 에 설치."""
    pkg_id = params.get("package_id")
    install_path = params.get("install_path") or \
                    os.path.join(DEFAULT_INSTALL_ROOT,
                                  params.get("service_kind") or f"pkg-{pkg_id}")
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

    return 0, f"installed pkg_id={pkg_id} at {install_path} ({len(data)} bytes)", ""


def job_process_control(params: dict, job_type: str) -> tuple:
    """start/stop/restart — install_path/cims.sh 을 이용해 수행."""
    install_path = params.get("install_path") or \
                    os.path.join(DEFAULT_INSTALL_ROOT,
                                  params.get("service_kind") or "")
    svc = (params.get("service_kind") or "").lower()
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
    """간단한 헬스 체크 — 설정된 포트에 TCP/UDP 연결 시도."""
    svc = params.get("service_kind", "").lower()
    probes = {
        "csp":     [("udp", 5060)],
        "cmp":     [("udp", 9000)],
        "csc":     [("tcp", 4420)],
        "cwrtc":   [("tcp", 8080)],
        "console": [("tcp", 3001)],
        "phone":   [("tcp", 3000)],
    }.get(svc, [])
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


def execute_job(job: dict, csc_url: str, session_token: str) -> dict:
    jt = job["type"]
    params = job.get("params") or {}
    try:
        if jt == "install":
            rc, out, err = job_install(params, csc_url, session_token)
        elif jt == "upgrade":
            rc, out, err = job_install(params, csc_url, session_token)
        elif jt in ("start", "stop", "restart"):
            rc, out, err = job_process_control(params, jt)
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
    state.save()
    print(f"[agent] enrolled: id={state.agent_id} name={state.name} status={resp.get('status')}")
    return True


def run_loop(csc_url: str, state: AgentState, heartbeat_sec: int, metric_sec: int):
    next_metric = 0
    while True:
        try:
            status, resp = http_post(f"{csc_url}/api/agent/heartbeat", {},
                                     headers={"X-Agent-Token": state.session_token})
            if status == 401:
                print("[agent] session token revoked; exiting")
                return 1
            if status == 200:
                jobs = resp.get("jobs") or []
                for job in jobs:
                    print(f"[agent] exec job id={job['id']} type={job['type']}")
                    result = execute_job(job, csc_url, state.session_token)
                    rep_status, rep_body = http_post(f"{csc_url}/api/agent/report", result,
                                                      headers={"X-Agent-Token": state.session_token})
                    print(f"[agent] report status={rep_status} rc={result['result_code']}")

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

    return run_loop(args.csc_url, state, args.heartbeat_sec, args.metric_sec)


if __name__ == "__main__":
    sys.exit(main() or 0)
