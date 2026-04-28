"""
P10 — Agent / Package / Deployment 통합 검증.

로컬 단일 서버에서:
  1. /api/v1/agents 로 Agent 생성 → enrollment_token 발급
  2. 실제 agent 프로세스를 서브프로세스로 기동 → enroll → heartbeat
  3. 패키지 업로드 (임시 tarball)
  4. Deployment 생성 → install job 큐잉 → agent 가 수행
  5. 상태 running 확인, 정리
"""
from __future__ import annotations

import base64
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))

from conftest import CscClient, TestRunner, CSC_BASE

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
AGENT_SCRIPT = os.path.join(REPO_ROOT, "agent", "cims_agent.py")

TEST_AGENT_NAME  = "_vp10_agent_local"
TEST_PKG_NAME    = "_vp10_helloapp"
TEST_PKG_VERSION = "0.0.1"


def _build_dummy_tarball() -> bytes:
    """install 검증용 최소 tarball — run.sh 한 줄만 들어있다."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tf:
        content = b"#!/bin/sh\necho 'hello from p10'\n"
        info = tarfile.TarInfo("run.sh")
        info.size = len(content); info.mode = 0o755
        tf.addfile(info, io.BytesIO(content))
    return buf.getvalue()


def _cleanup(c: CscClient):
    for a in (c.get("/api/v1/agents").get("items") or []):
        if a.get("name") == TEST_AGENT_NAME:
            c.delete(f"/api/v1/agents/{a['id']}")
    for p in (c.get("/api/v1/packages").get("items") or []):
        if p.get("name") == TEST_PKG_NAME and p.get("version") == TEST_PKG_VERSION:
            c.delete(f"/api/v1/packages/{p['id']}")


def run_agent_deployment_tests(only=None):
    run = TestRunner("test_agent_deployment", only_ids=only)
    c = CscClient()
    r = c.login()
    if not c.token:
        print(f"  [\033[31mFAIL\033[0m] login: {r}")
        return run.summary()

    _cleanup(c)

    state_dir = tempfile.mkdtemp(prefix="cims-agent-test-")
    install_root = tempfile.mkdtemp(prefix="cims-deploy-test-")
    agent_proc: subprocess.Popen | None = None

    created_agent_id: int | None = None
    created_pkg_id: int | None = None
    created_deploy_id: int | None = None

    def t01_create_agent():
        nonlocal created_agent_id
        r = c.post("/api/v1/agents", {"name": TEST_AGENT_NAME, "note": "p10 local test"})
        if r.get("_status") != 201:
            return False, f"status={r.get('_status')} body={r}"
        if not r.get("enrollment_token"):
            return False, "enrollment_token 누락"
        created_agent_id = r["id"]
        t01_create_agent.token = r["enrollment_token"]
        return True, f"id={created_agent_id} token=…{r['enrollment_token'][-8:]}"

    def t02_agent_enroll_and_heartbeat():
        nonlocal agent_proc
        env = os.environ.copy()
        env["CIMS_ENROLLMENT_TOKEN"] = t01_create_agent.token
        agent_proc = subprocess.Popen(
            [sys.executable, "-u", AGENT_SCRIPT,
             "--csc-url", CSC_BASE,
             "--state-dir", state_dir,
             "--heartbeat-sec", "2",
             "--metric-sec", "3"],
            env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        # 1) 우선 enroll 대기 (status pending + last_heartbeat 채워짐)
        deadline = time.time() + 10
        while time.time() < deadline:
            a = c.get(f"/api/v1/agents/{created_agent_id}")
            if a.get("last_heartbeat"):
                break
            time.sleep(0.4)
        else:
            return False, "enroll/heartbeat 전이 안됨"
        # 2) 승인 후 online 대기
        r = c.post(f"/api/v1/agents/{created_agent_id}/approve", {})
        if r.get("_status") not in (200, 204):
            return False, f"approve 실패: {r}"
        deadline = time.time() + 10
        while time.time() < deadline:
            a = c.get(f"/api/v1/agents/{created_agent_id}")
            if a.get("status") == "online":
                return True, f"status={a.get('status')} hb={a.get('last_heartbeat')}"
            time.sleep(0.5)
        return False, f"승인 후 online 미전이 (현재={a.get('status')})"

    def t03_upload_package():
        nonlocal created_pkg_id
        data = _build_dummy_tarball()
        b64 = base64.b64encode(data).decode("ascii")
        r = c.post("/api/v1/packages", {
            "name": TEST_PKG_NAME, "version": TEST_PKG_VERSION,
            "description": "p10 local test package",
            "file_base64": b64,
        })
        if r.get("_status") != 201:
            return False, f"status={r.get('_status')} body={r}"
        created_pkg_id = r["id"]
        expected_sha = hashlib.sha256(data).hexdigest()
        if r.get("sha256") != expected_sha:
            return False, f"SHA256 불일치: got={r.get('sha256')} exp={expected_sha}"
        return True, f"id={created_pkg_id} size={r.get('file_size')}"

    def t04_create_deployment():
        nonlocal created_deploy_id
        r = c.post("/api/v1/deployments", {
            "agent_id": created_agent_id,
            "package_id": created_pkg_id,
            "service_kind": "csp",
            "install_path": install_root,
        })
        if r.get("_status") != 201:
            return False, f"status={r.get('_status')} body={r}"
        created_deploy_id = r["id"]
        return True, f"id={created_deploy_id}"

    def t05_queue_install_and_verify():
        r = c.post(f"/api/v1/deployments/{created_deploy_id}/job",
                   {"job_type": "install"})
        if r.get("_status") not in (200, 201, 202) or not r.get("job_id"):
            return False, f"job 큐잉 실패: {r}"
        deadline = time.time() + 20
        while time.time() < deadline:
            d = c.get(f"/api/v1/deployments/{created_deploy_id}")
            status = d.get("status")
            if status in ("running", "stopped"):
                marker = os.path.join(install_root, "run.sh")
                if os.path.exists(marker):
                    return True, f"deploy_status={status}, run.sh 존재"
                return False, f"status={status} 인데 run.sh 미존재"
            if status == "failed":
                return False, f"deployment failed: {d.get('note')}"
            time.sleep(0.8)
        return False, f"timeout, last_status={d.get('status')}"

    def t06_revoke_and_cleanup():
        if agent_proc and agent_proc.poll() is None:
            agent_proc.terminate()
            try: agent_proc.wait(3)
            except subprocess.TimeoutExpired: agent_proc.kill()
        if created_deploy_id:
            c.delete(f"/api/v1/deployments/{created_deploy_id}")
        if created_pkg_id:
            c.delete(f"/api/v1/packages/{created_pkg_id}")
        if created_agent_id:
            c.delete(f"/api/v1/agents/{created_agent_id}")
        shutil.rmtree(state_dir, ignore_errors=True)
        shutil.rmtree(install_root, ignore_errors=True)
        return True, "정리 완료"

    try:
        run.run("P10.1", "Agent 등록 + enrollment token 발급", t01_create_agent)
        run.run("P10.2", "Agent 프로세스 enroll + heartbeat + online 전이", t02_agent_enroll_and_heartbeat)
        run.run("P10.3", "패키지 업로드 + SHA256 검증", t03_upload_package)
        run.run("P10.4", "Deployment 생성", t04_create_deployment)
        run.run("P10.5", "install job 큐잉 + agent 실행 + 상태 전이", t05_queue_install_and_verify)
    finally:
        run.run("P10.6", "정리", t06_revoke_and_cleanup)
    return run.summary()


if __name__ == "__main__":
    s = run_agent_deployment_tests()
    print(json.dumps(s, indent=2, ensure_ascii=False))
    sys.exit(0 if s["fail"] == 0 else 1)
