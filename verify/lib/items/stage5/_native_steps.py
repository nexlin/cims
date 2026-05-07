"""Stage 5 native step 구현 — _verify_phase2 의 점진 Python 포팅.

_legacy.py 가 cims.sh _verify_phase2 본체 1회 호출로 22단계를 한꺼번에 처리하는
어댑터 패턴을 대체하기 위한 native Python step 구현 모듈.

각 step 은 self-contained 함수: ctx 에서 필요한 상태를 읽고, ItemResult 반환.
ctx.state["_s5_native"] 에 결과 + 공유 변수 (JWT, agent_id, Test-agent pid 등)
캐시. 동일 step 의 재호출은 cache 로 방지 (idempotent).

마이그레이션 절차 (점진):
  1. 가장 단순/독립적인 step 부터 native 함수로 구현
  2. 해당 step 의 verify_item 자식 함수가 _legacy.step_result() 대신 native 호출
  3. 22 step 모두 포팅되면 _legacy.py 와 cims.sh _verify_phase2 제거

현재 native 구현:
  - step 01 (Cleanup)        — cmd_reset --all --keep-processes
  - step 05 (Admin login)    — TB-CSC(4419) JWT 발급
  - step 06 (Agent register) — csc-server-local agent 생성/재생성 + approve
  - step 07 (Test-agent)     — sync 9903 spawn + cims_agent 테이블 online 대기

미포팅 step 은 _legacy.get_legacy_results 로 위임.

** 알려진 한계 **
  네이티브로 포팅된 step 은 _legacy 가 호출하는 _verify_phase2 안에서도 함께
  실행되므로 (bash 본체는 step 1~22 monolithic) 중복 실행이 발생한다.
  cmd_reset / admin login / agent re-register 모두 idempotent 라 functional
  영향 X. 추후 cims.sh _verify_phase2 에 --skip-step=N,... 플래그 추가 또는
  step 22 모두 포팅 시 cims.sh 본체 제거.

** 공유 상태 구조 **
  ctx.state["_s5_native"] = {
    "results": {step_no: ItemResult},      # step 결과 cache (idempotent)
    "tok":           str,                   # TB-CSC JWT (step 05)
    "aid_csc":       int,                   # csc-server-local agent_id (step 06)
    "enroll_tok_csc": str,                  # enrollment token (step 06)
    "ta_pid_csc":    int,                   # Test-agent pid (step 07)
    ...                                     # 후속 step 추가 시 확장
  }
"""
from __future__ import annotations

import os
import time
from typing import Optional

from ...registry import ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell
from ...common import csc_http
from ...common import db as _db


_STATE_KEY = "_s5_native"


# TB-CSC 접속 기본값 — env 로 override 가능
_TB_CSC_BASE = "https://127.0.0.1:4419"
_AGENT_NAME_CSC = "csc-server-local"
_AGENT_SYNC_PORT_CSC = 9903


def _store(ctx: VerifyContext) -> dict:
    """공유 dict — 없으면 생성."""
    return ctx.state.setdefault(_STATE_KEY, {"results": {}})


def _save(ctx: VerifyContext, step_no: int, result: ItemResult) -> None:
    s = _store(ctx)
    s.setdefault("results", {})[step_no] = result


def already_ran(ctx: VerifyContext, step_no: int) -> bool:
    return step_no in _store(ctx).get("results", {})


def get_native_result(ctx: VerifyContext, step_no: int) -> ItemResult:
    return _store(ctx)["results"][step_no]


def _set(ctx: VerifyContext, key: str, val) -> None:
    _store(ctx)[key] = val


def _get(ctx: VerifyContext, key: str, default=None):
    return _store(ctx).get(key, default)


# ─────────────────────────────────────────────────────────────
# Step 01 — Cleanup (cmd_reset --all --keep-processes)
# ─────────────────────────────────────────────────────────────
def step_01_cleanup(ctx: VerifyContext) -> ItemResult:
    """Step 01 — 검증 환경 초기화 (가입자 보존, TB 3종 유지).

    cims.sh cmd_reset --all --keep-processes 호출:
      - LOG_DIR/*.log + service_log/ + msg_log/ wipe
      - /tmp/cims-agent-* + build/dist/{csc,csp,cmp,sim}-server/ rm -rf
      - 발급 cert (cert/agent_mtls/issued) 정리
      - DB: agent_deployment/_job/_metric TRUNCATE (cims_agent 는 TB 외 DELETE)
    --keep-processes: TB-CSC(4419) / TB-Console(3000) / TB-agent(9902) 보존.
    """
    if already_ran(ctx, 1):
        return get_native_result(ctx, 1)

    rc, out, err = shell.run_cims_sh(
        ctx.repo_root, "reset", "--all", "--keep-processes",
        timeout=120,
    )
    full = (out or "") + (err or "")
    tail = "\n".join(full.splitlines()[-15:])
    status = ItemStatus.PASS if rc == 0 else ItemStatus.FAIL
    detail = f"rc={rc}\n{tail}" if tail else f"rc={rc}"
    result = ItemResult(
        id="S5-RESET", name="배포본 reset (cleanup)",
        status=status, detail=detail, stage=5,
    )
    _save(ctx, 1, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 05 — Admin login (TB-CSC 4419)
# ─────────────────────────────────────────────────────────────
def step_05_admin_login(ctx: VerifyContext) -> ItemResult:
    """Step 05 — TB-CSC(4419) admin login → JWT.

    환경 변수: CIMS_TB_ADMIN_ID (default admin), CIMS_TB_ADMIN_PASSWORD (1234).
    성공 시 ctx.state["_s5_native"]["tok"] 에 JWT 저장.
    """
    if already_ran(ctx, 5):
        return get_native_result(ctx, 5)

    base = _TB_CSC_BASE
    login_id = os.environ.get("CIMS_TB_ADMIN_ID", "admin")
    pw = os.environ.get("CIMS_TB_ADMIN_PASSWORD", "1234")
    tok = csc_http.admin_login(base, login_id, pw, timeout=5)
    if not tok:
        result = ItemResult(
            id="S5-CSC-DEPLOY-AGENT-ENROLL-LOGIN", name="TB-CSC admin login",
            status=ItemStatus.FAIL,
            detail=f"admin login 실패 (base={base} id={login_id}). "
                   "TB-CSC 4419 가 LISTEN 중인지 / 자격증명이 맞는지 확인.",
            stage=5,
        )
        _save(ctx, 5, result)
        return result

    _set(ctx, "tok", tok)
    result = ItemResult(
        id="S5-CSC-DEPLOY-AGENT-ENROLL-LOGIN", name="TB-CSC admin login",
        status=ItemStatus.PASS,
        detail=f"base={base} id={login_id} → JWT (len={len(tok)})",
        stage=5,
    )
    _save(ctx, 5, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 06 — Agent register (csc-server-local)
# ─────────────────────────────────────────────────────────────
def step_06_agent_register(ctx: VerifyContext) -> ItemResult:
    """Step 06 — csc-server-local agent 등록 + approve.

    409 충돌 시 기존 agent DELETE 후 재생성 (idempotent).
    성공 시 ctx.state["_s5_native"]["aid_csc"] / ["enroll_tok_csc"] 저장.
    """
    if already_ran(ctx, 6):
        return get_native_result(ctx, 6)

    tok = _get(ctx, "tok", "")
    if not tok:
        result = ItemResult(
            id="S5-CSC-DEPLOY-AGENT-ENROLL-REGISTER", name="agent 등록",
            status=ItemStatus.SKIP,
            detail="step 05 (admin login) 미실행 / 실패 — JWT 없음",
            stage=5,
        )
        _save(ctx, 6, result)
        return result

    base = _TB_CSC_BASE
    aname = _AGENT_NAME_CSC
    payload = {"name": aname, "note": "S5 Test-agent (native)"}

    # 1) POST /agents
    try:
        status, body = csc_http.post_json(
            f"{base}/api/v1/agents", payload, token=tok, timeout=10,
        )
    except Exception as e:
        result = ItemResult(
            id="S5-CSC-DEPLOY-AGENT-ENROLL-REGISTER", name="agent 등록",
            status=ItemStatus.FAIL,
            detail=f"POST /agents 호출 실패: {type(e).__name__}: {e}",
            stage=5,
        )
        _save(ctx, 6, result)
        return result

    # 2) 409 Conflict — 기존 agent 삭제 후 재시도
    if status == 409:
        prev_id = csc_http.find_agent_id_by_name(base, tok, aname)
        if prev_id:
            csc_http.delete(f"{base}/api/v1/agents/{prev_id}", token=tok)
        try:
            status, body = csc_http.post_json(
                f"{base}/api/v1/agents", payload, token=tok, timeout=10,
            )
        except Exception as e:
            result = ItemResult(
                id="S5-CSC-DEPLOY-AGENT-ENROLL-REGISTER", name="agent 등록",
                status=ItemStatus.FAIL,
                detail=f"409 후 재생성 실패: {type(e).__name__}: {e}",
                stage=5,
            )
            _save(ctx, 6, result)
            return result

    if status not in (200, 201):
        result = ItemResult(
            id="S5-CSC-DEPLOY-AGENT-ENROLL-REGISTER", name="agent 등록",
            status=ItemStatus.FAIL,
            detail=f"POST /agents 실패 status={status} body={str(body)[:200]}",
            stage=5,
        )
        _save(ctx, 6, result)
        return result

    if not isinstance(body, dict):
        result = ItemResult(
            id="S5-CSC-DEPLOY-AGENT-ENROLL-REGISTER", name="agent 등록",
            status=ItemStatus.FAIL,
            detail=f"응답이 JSON 객체 아님: {str(body)[:200]}",
            stage=5,
        )
        _save(ctx, 6, result)
        return result

    aid = body.get("id")
    enroll_tok = body.get("enrollment_token") or ""
    if aid is None or not enroll_tok:
        result = ItemResult(
            id="S5-CSC-DEPLOY-AGENT-ENROLL-REGISTER", name="agent 등록",
            status=ItemStatus.FAIL,
            detail=f"응답에 id/enrollment_token 누락: {str(body)[:200]}",
            stage=5,
        )
        _save(ctx, 6, result)
        return result

    # 3) approve
    try:
        ap_status, _ = csc_http.post_json(
            f"{base}/api/v1/agents/{aid}/approve", {}, token=tok, timeout=5,
        )
    except Exception as e:
        result = ItemResult(
            id="S5-CSC-DEPLOY-AGENT-ENROLL-REGISTER", name="agent 등록",
            status=ItemStatus.FAIL,
            detail=f"approve 호출 실패: {type(e).__name__}: {e}",
            stage=5,
        )
        _save(ctx, 6, result)
        return result
    if ap_status not in (200, 204):
        result = ItemResult(
            id="S5-CSC-DEPLOY-AGENT-ENROLL-REGISTER", name="agent 등록",
            status=ItemStatus.FAIL,
            detail=f"approve 실패 status={ap_status}",
            stage=5,
        )
        _save(ctx, 6, result)
        return result

    _set(ctx, "aid_csc", int(aid))
    _set(ctx, "enroll_tok_csc", str(enroll_tok))
    result = ItemResult(
        id="S5-CSC-DEPLOY-AGENT-ENROLL-REGISTER", name="agent 등록",
        status=ItemStatus.PASS,
        detail=f"aid={aid} name={aname} (approved)",
        stage=5,
    )
    _save(ctx, 6, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 07 — Test-agent spawn + enroll wait
# ─────────────────────────────────────────────────────────────
def _agent_online_in_db(name: str, dist_dir: str) -> bool:
    """cims_agent 테이블에서 status='online' 확인. DB 미접속 시 False."""
    cfg = _db.csp_db_config(dist_dir)
    if not cfg:
        return False
    try:
        conn = _db.connect(cfg)
    except Exception:
        return False
    try:
        cur = conn.cursor()
        cur.execute(
            "SELECT 1 FROM cims_agent WHERE name=%s AND status='online'", (name,),
        )
        return cur.fetchone() is not None
    finally:
        try: conn.close()
        except Exception: pass


def step_07_testagent_spawn(ctx: VerifyContext) -> ItemResult:
    """Step 07 — Test-agent 기동 (sync 9903) + enroll 대기 (15s).

    cims_agent.py 를 nohup 으로 spawn:
      env CIMS_AGENT_INSTALL_ROOT=build/dist/csc-server
          CIMS_AGENT_SYNC_PORT=9903
    enroll polling: cims_agent 테이블에서 name=csc-server-local + status='online'.
    성공 시 ctx.state["_s5_native"]["ta_pid_csc"] 저장.
    """
    if already_ran(ctx, 7):
        return get_native_result(ctx, 7)

    aid = _get(ctx, "aid_csc")
    enroll_tok = _get(ctx, "enroll_tok_csc", "")
    if aid is None or not enroll_tok:
        result = ItemResult(
            id="S5-CSC-DEPLOY-AGENT-ENROLL-SPAWN", name="Test-agent 기동",
            status=ItemStatus.SKIP,
            detail="step 06 (agent register) 미실행 / 실패",
            stage=5,
        )
        _save(ctx, 7, result)
        return result

    base = _TB_CSC_BASE
    aname = _AGENT_NAME_CSC
    sync_port = _AGENT_SYNC_PORT_CSC
    dist = ctx.dist_dir
    ta_dir = os.path.join(dist, "csc-server", "agent")
    state_dir = os.path.join(ta_dir, "state")
    ta_log = os.path.join(ctx.repo_root, "logs", "test-agent-csc-server.log")
    os.makedirs(state_dir, exist_ok=True)
    os.makedirs(os.path.dirname(ta_log), exist_ok=True)

    agent_py = os.path.join(dist, "agent", "cims_agent.py")
    if not os.path.isfile(agent_py):
        result = ItemResult(
            id="S5-CSC-DEPLOY-AGENT-ENROLL-SPAWN", name="Test-agent 기동",
            status=ItemStatus.FAIL,
            detail=f"cims_agent.py 없음: {agent_py} (S4 패키지화 / S5-RESET 후 dist 미생성?)",
            stage=5,
        )
        _save(ctx, 7, result)
        return result

    # spawn
    import subprocess
    env = dict(os.environ)
    env["CIMS_AGENT_INSTALL_ROOT"] = os.path.join(dist, "csc-server")
    env["CIMS_AGENT_SYNC_PORT"] = str(sync_port)
    log_fp = open(ta_log, "w")
    try:
        proc = subprocess.Popen(
            ["python3", agent_py,
             "--csc-url", base,
             "--name", aname,
             "--state-dir", state_dir,
             "--enrollment-token", enroll_tok,
             "--heartbeat-sec", "3"],
            env=env, stdout=log_fp, stderr=subprocess.STDOUT,
            start_new_session=True,
        )
    except Exception as e:
        log_fp.close()
        result = ItemResult(
            id="S5-CSC-DEPLOY-AGENT-ENROLL-SPAWN", name="Test-agent 기동",
            status=ItemStatus.FAIL,
            detail=f"Popen 실패: {type(e).__name__}: {e}",
            stage=5,
        )
        _save(ctx, 7, result)
        return result

    # enroll polling — 최대 15s
    online = False
    waited = 0
    for _ in range(15):
        time.sleep(1); waited += 1
        if _agent_online_in_db(aname, dist):
            online = True
            break

    if not online:
        # spawn 실패면 pid 살려두지 않음
        try: proc.terminate()
        except Exception: pass
        try: log_fp.close()
        except Exception: pass
        # 로그 tail
        tail = ""
        try:
            with open(ta_log) as f:
                tail = "\n".join(f.read().splitlines()[-10:])
        except Exception: pass
        result = ItemResult(
            id="S5-CSC-DEPLOY-AGENT-ENROLL-SPAWN", name="Test-agent 기동",
            status=ItemStatus.FAIL,
            detail=f"enroll timeout (15s, pid={proc.pid}, sync={sync_port})\n{tail}",
            stage=5,
        )
        _save(ctx, 7, result)
        return result

    _set(ctx, "ta_pid_csc", int(proc.pid))
    result = ItemResult(
        id="S5-CSC-DEPLOY-AGENT-ENROLL-SPAWN", name="Test-agent 기동",
        status=ItemStatus.PASS,
        detail=f"pid={proc.pid} sync={sync_port} state-dir={state_dir} "
               f"(enroll {waited}s)",
        stage=5,
    )
    _save(ctx, 7, result)
    return result


# ─────────────────────────────────────────────────────────────
# Composite — S5-CSC-DEPLOY-AGENT-ENROLL (steps 5+6+7)
# ─────────────────────────────────────────────────────────────
def steps_05_06_07_agent_enroll(ctx: VerifyContext) -> ItemResult:
    """S5-CSC-DEPLOY-AGENT-ENROLL 본체 — step 05/06/07 순차 실행 후 worst-status
    합성. 자식 ItemResult 는 result.children 에 첨부 → runner 가 child-result
    마커 자동 emit.
    """
    rs = [
        step_05_admin_login(ctx),
        step_06_agent_register(ctx),
        step_07_testagent_spawn(ctx),
    ]
    rank = {ItemStatus.PASS: 0, ItemStatus.SKIP: 1,
            ItemStatus.BLOCKED: 2, ItemStatus.FAIL: 3}
    worst = ItemStatus.PASS
    total_ms = 0
    for r in rs:
        if rank.get(r.status, 0) > rank.get(worst, 0):
            worst = r.status
        total_ms += r.elapsed_ms or 0
    n_pass = sum(1 for r in rs if r.status == ItemStatus.PASS)
    n_fail = sum(1 for r in rs if r.status == ItemStatus.FAIL)
    n_skip = sum(1 for r in rs if r.status == ItemStatus.SKIP)
    parts = []
    if n_pass: parts.append(f"PASS {n_pass}")
    if n_fail: parts.append(f"FAIL {n_fail}")
    if n_skip: parts.append(f"SKIP {n_skip}")
    return ItemResult(
        id="S5-CSC-DEPLOY-AGENT-ENROLL",
        name="TB-CSC admin login + agent enroll",
        status=worst, detail=", ".join(parts) or "no children",
        elapsed_ms=total_ms, stage=5,
        children=rs,
    )
