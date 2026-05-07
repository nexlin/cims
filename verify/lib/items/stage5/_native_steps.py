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

import glob
import json
import os
import re
import time
from typing import Optional

from ...registry import ItemResult, ItemStatus
from ...context import VerifyContext
from ... import shell
from ...common import csc_http
from ...common import db as _db


def _natural_key(s: str) -> list:
    """Natural sort key — '1.10' > '1.9' (숫자 청크는 int 비교)."""
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", s)]


def _latest_tarball(pkg_dir: str, prefix: str) -> Optional[str]:
    """$pkg_dir/<prefix>-*.tar.gz 중 natural sort 최고값 (sort -V tail -1 동등)."""
    cands = glob.glob(os.path.join(pkg_dir, f"{prefix}-*.tar.gz"))
    if not cands:
        return None
    return sorted(cands, key=_natural_key)[-1]


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
# Step 08 — Package upload (csc + console)
# ─────────────────────────────────────────────────────────────
_CSC_PACKAGES = ("csc", "console")
"""S5-CSC-DEPLOY 단계가 다루는 패키지 모듈 (탐색 prefix 와 동일)."""


def step_08_package_upload(ctx: VerifyContext) -> ItemResult:
    """Step 08 — TB-CSC(4419) 에 csc + console tarball 업로드.

    $DIST_DIR/packages/{csc,console}-*.tar.gz 중 natural-sort 최고값 1개씩.
    POST /api/v1/packages (multipart, force=true) → package_id 추출.
    성공 시 ctx.state["pkg_id_csc"] / ["pkg_id_console"] 저장.
    한 모듈이라도 tarball 없거나 업로드 실패면 FAIL.
    """
    if already_ran(ctx, 8):
        return get_native_result(ctx, 8)

    tok = _get(ctx, "tok", "")
    if not tok:
        result = ItemResult(
            id="S5-CSC-DEPLOY-PKG-UPLOAD", name="패키지 업로드 (csc + console)",
            status=ItemStatus.SKIP,
            detail="step 05 (admin login) 미실행 / 실패 — JWT 없음",
            stage=5,
        )
        _save(ctx, 8, result)
        return result

    base = _TB_CSC_BASE
    pkg_dir = os.path.join(ctx.dist_dir, "packages")
    notes: list = []
    fail = False
    for name in _CSC_PACKAGES:
        tar = _latest_tarball(pkg_dir, name)
        if not tar:
            notes.append(f"- [FAIL] {name}: tarball 없음 ({pkg_dir}/{name}-*.tar.gz)")
            fail = True
            continue
        try:
            status, body = csc_http.post_multipart(
                f"{base}/api/v1/packages",
                file_path=tar,
                form_fields={"force": "true"},
                token=tok, timeout=120,
            )
        except Exception as e:
            notes.append(f"- [FAIL] {name}: 업로드 예외 {type(e).__name__}: {e}")
            fail = True
            continue
        if status not in (200, 201) or not isinstance(body, dict) or not body.get("id"):
            notes.append(
                f"- [FAIL] {name}: status={status} body={str(body)[:120]}"
            )
            fail = True
            continue
        pkg_id = int(body["id"])
        _set(ctx, f"pkg_id_{name}", pkg_id)
        notes.append(f"- [OK] {name}: package_id={pkg_id} ({os.path.basename(tar)})")

    result = ItemResult(
        id="S5-CSC-DEPLOY-PKG-UPLOAD", name="패키지 업로드 (csc + console)",
        status=ItemStatus.FAIL if fail else ItemStatus.PASS,
        detail="\n".join(notes) if notes else "no packages",
        stage=5,
    )
    _save(ctx, 8, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 09 — Deployment 생성 (config overlay)
# ─────────────────────────────────────────────────────────────
def _csc_overlay(name: str) -> dict:
    """csc-server 자식 config overlay — Phase 1 충돌 회피용 포트 매핑."""
    if name == "csc":     return {"Server.Port": 4445}
    if name == "console": return {"Port": 8081}
    return {}


def step_09_deployment_create(ctx: VerifyContext) -> ItemResult:
    """Step 09 — csc + console deployment 생성 (config overlay 포함).

    install_path = $DIST_DIR/csc-server/{csc,console}.
    process_name = upper-case (CSC / CONSOLE).
    config overlay 로 csc:Server.Port=4445, console:Port=8081 적용.
    성공 시 ctx.state["dep_id_csc"] / ["dep_id_console"] 저장.
    """
    if already_ran(ctx, 9):
        return get_native_result(ctx, 9)

    tok = _get(ctx, "tok", "")
    aid = _get(ctx, "aid_csc")
    if not tok or aid is None:
        result = ItemResult(
            id="S5-CSC-DEPLOY-INSTALL-CREATE", name="deployment 생성",
            status=ItemStatus.SKIP,
            detail="step 05/06 미실행 / 실패 — tok/agent_id 없음",
            stage=5,
        )
        _save(ctx, 9, result)
        return result

    base = _TB_CSC_BASE
    notes: list = []
    fail = False
    for name in _CSC_PACKAGES:
        pkg_id = _get(ctx, f"pkg_id_{name}")
        if pkg_id is None:
            notes.append(f"- [SKIP] {name}: step 08 에서 package_id 미확보")
            fail = True
            continue
        install_path = os.path.join(ctx.dist_dir, "csc-server", name)
        payload = {
            "agent_id":      int(aid),
            "package_id":    int(pkg_id),
            "install_path":  install_path,
            "process_name":  name.upper(),
            "config":        _csc_overlay(name),
        }
        try:
            status, body = csc_http.post_json(
                f"{base}/api/v1/deployments", payload, token=tok, timeout=15,
            )
        except Exception as e:
            notes.append(f"- [FAIL] {name}: 호출 예외 {type(e).__name__}: {e}")
            fail = True
            continue
        if status not in (200, 201) or not isinstance(body, dict) or not body.get("id"):
            notes.append(f"- [FAIL] {name}: status={status} body={str(body)[:120]}")
            fail = True
            continue
        did = int(body["id"])
        _set(ctx, f"dep_id_{name}", did)
        notes.append(
            f"- [OK] {name}: deployment_id={did} → {install_path} "
            f"overlay={payload['config']}"
        )

    result = ItemResult(
        id="S5-CSC-DEPLOY-INSTALL-CREATE", name="deployment 생성",
        status=ItemStatus.FAIL if fail else ItemStatus.PASS,
        detail="\n".join(notes) if notes else "no deployments",
        stage=5,
    )
    _save(ctx, 9, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 10 — Install job + DB 폴링
# ─────────────────────────────────────────────────────────────
def _deployment_status(name: str, did: int, dist_dir: str) -> Optional[str]:
    """agent_deployment.status 조회. 실패 시 None."""
    cfg = _db.csp_db_config(dist_dir)
    if not cfg: return None
    try:
        conn = _db.connect(cfg)
    except Exception:
        return None
    try:
        cur = conn.cursor()
        cur.execute("SELECT status FROM agent_deployment WHERE id=%s", (did,))
        row = cur.fetchone()
        return str(row[0]) if row and row[0] is not None else None
    finally:
        try: conn.close()
        except Exception: pass


def step_10_install_poll(ctx: VerifyContext) -> ItemResult:
    """Step 10 — install job 발행 + agent_deployment 상태 폴링 (최대 60s).

    pending/deploying 이 모두 사라질 때까지 대기. 각 deployment 의 최종 상태를
    detail 에 기록. all_done 여부를 ctx.state["all_install_done_csc"] 에 저장.
    """
    if already_ran(ctx, 10):
        return get_native_result(ctx, 10)

    tok = _get(ctx, "tok", "")
    if not tok:
        result = ItemResult(
            id="S5-CSC-DEPLOY-INSTALL-POLL", name="install job + 폴링",
            status=ItemStatus.SKIP,
            detail="step 05 (admin login) 미실행 — tok 없음",
            stage=5,
        )
        _save(ctx, 10, result)
        return result

    base = _TB_CSC_BASE
    deployments: list = []
    for name in _CSC_PACKAGES:
        did = _get(ctx, f"dep_id_{name}")
        if did is not None:
            deployments.append((name, int(did)))
    if not deployments:
        result = ItemResult(
            id="S5-CSC-DEPLOY-INSTALL-POLL", name="install job + 폴링",
            status=ItemStatus.FAIL,
            detail="step 09 에서 deployment 미확보",
            stage=5,
        )
        _save(ctx, 10, result)
        return result

    # install job 발행
    notes: list = []
    for name, did in deployments:
        try:
            status, _ = csc_http.post_json(
                f"{base}/api/v1/deployments/{did}/job",
                {"job_type": "install"}, token=tok, timeout=10,
            )
        except Exception as e:
            notes.append(f"- [FAIL] {name}: install 발행 예외 {e}")
            continue
        if status not in (200, 201, 202):
            notes.append(f"- [FAIL] {name}: install 발행 status={status}")

    # 폴링 (sleep 2s × 30 = 60s)
    elapsed = 0
    all_done = False
    final_status: dict = {}
    for _ in range(30):
        time.sleep(2); elapsed += 2
        still = False
        for name, did in deployments:
            st = _deployment_status(name, did, ctx.dist_dir)
            final_status[name] = st or "(unknown)"
            if st in ("pending", "deploying"):
                still = True
        if not still:
            all_done = True
            break

    _set(ctx, "all_install_done_csc", bool(all_done))
    for name, did in deployments:
        st = final_status.get(name, "(unknown)")
        ok = st in ("succeeded", "installed")
        notes.append(
            f"- [{'OK' if ok else 'WARN'}] {name}: did={did} status={st}"
        )

    overall = ItemStatus.PASS if all_done and all(
        final_status.get(n) in ("succeeded", "installed") for n, _ in deployments
    ) else ItemStatus.FAIL
    summary = (
        f"all_done={all_done} elapsed={elapsed}s\n"
        + "\n".join(notes)
    )
    result = ItemResult(
        id="S5-CSC-DEPLOY-INSTALL-POLL", name="install job + 폴링",
        status=overall, detail=summary, stage=5,
    )
    _save(ctx, 10, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 11 — 설치 파일 검증 (meta.json + config/)
# ─────────────────────────────────────────────────────────────
def step_11_verify_files(ctx: VerifyContext) -> ItemResult:
    """Step 11 — `$DIST_DIR/csc-server/{csc,console}/` 안에 meta.json + config/
    디렉토리가 존재하는지 확인. install job 후 파일 배포 검증.

    이전 step 들 (08/09/10) 의 결과에 의존하지 않음 — install_path 만 기준으로
    파일 시스템 검증. 단독 실행 가능 (재실행 시에도 동일 결과).
    """
    if already_ran(ctx, 11):
        return get_native_result(ctx, 11)

    notes: list = []
    ok_all = True
    for name in _CSC_PACKAGES:
        install_path = os.path.join(ctx.dist_dir, "csc-server", name)
        meta_p = os.path.join(install_path, "meta.json")
        cfg_d = os.path.join(install_path, "config")
        meta_ok = os.path.isfile(meta_p)
        cfg_ok = os.path.isdir(cfg_d)
        if meta_ok and cfg_ok:
            notes.append(f"- [OK] {name}: meta.json + config/ 존재 ({install_path})")
        else:
            miss: list = []
            if not meta_ok: miss.append("meta.json")
            if not cfg_ok:  miss.append("config/")
            notes.append(f"- [FAIL] {name}: 누락 {','.join(miss)} ({install_path})")
            ok_all = False

    result = ItemResult(
        id="S5-CSC-VERIFY-FILES", name="설치 파일 검증",
        status=ItemStatus.PASS if ok_all else ItemStatus.FAIL,
        detail="\n".join(notes), stage=5,
    )
    _save(ctx, 11, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 12 — config overlay 반영 검증 (csc/config.json Server.Port=4445)
# ─────────────────────────────────────────────────────────────
def _read_csc_port(install_path: str) -> Optional[int]:
    """`<install_path>/config.json` 에서 `Server.Port` (flat 또는 nested
    Server.Port) 추출. 파일/JSON 오류 시 None.
    """
    p = os.path.join(install_path, "config.json")
    try:
        with open(p) as f:
            d = json.load(f)
    except Exception:
        return None
    if not isinstance(d, dict):
        return None
    # 1) flat key "Server.Port"
    val = d.get("Server.Port")
    if val is None:
        # 2) nested {"Server": {"Port": ...}}
        srv = d.get("Server")
        if isinstance(srv, dict):
            val = srv.get("Port")
    if val is None:
        return None
    try:
        return int(val)
    except (TypeError, ValueError):
        return None


def step_12_verify_overlay(ctx: VerifyContext) -> ItemResult:
    """Step 12 — csc/config.json overlay 반영 (Server.Port == 4445)."""
    if already_ran(ctx, 12):
        return get_native_result(ctx, 12)

    install_path = os.path.join(ctx.dist_dir, "csc-server", "csc")
    port = _read_csc_port(install_path)
    if port == 4445:
        result = ItemResult(
            id="S5-CSC-VERIFY-OVERLAY", name="config overlay 반영",
            status=ItemStatus.PASS,
            detail=f"- [OK] csc/config.json: Server.Port=4445 반영 ({install_path})",
            stage=5,
        )
    else:
        result = ItemResult(
            id="S5-CSC-VERIFY-OVERLAY", name="config overlay 반영",
            status=ItemStatus.FAIL,
            detail=f"- [FAIL] csc/config.json Server.Port 이상 (실제={port}, 기대=4445, "
                   f"path={install_path}/config.json)",
            stage=5,
        )
    _save(ctx, 12, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 13 — csc Start + 포트 4445 LISTEN
# ─────────────────────────────────────────────────────────────
def _post_job(ctx: VerifyContext, did: int, job_type: str,
              base: str = _TB_CSC_BASE, timeout: int = 10) -> tuple:
    """POST /deployments/<did>/job {job_type:...}. (status, body) 반환.
    network 예외는 (0, str(e))."""
    tok = _get(ctx, "tok", "")
    try:
        return csc_http.post_json(
            f"{base}/api/v1/deployments/{did}/job",
            {"job_type": job_type}, token=tok, timeout=timeout,
        )
    except Exception as e:
        return (0, f"{type(e).__name__}: {e}")


def _wait_listen(port: int, proto: str, timeout_s: int) -> int:
    """`shell.port_listening` 폴링 (1초 단위). 도달하면 경과초, 실패면 -1."""
    waited = 0
    while waited < timeout_s:
        if shell.port_listening(port, proto):
            return waited
        time.sleep(1); waited += 1
    return -1


def step_13_csc_start(ctx: VerifyContext) -> ItemResult:
    """Step 13 — csc Start job 발행 + 4445 LISTEN 대기 (25s)."""
    if already_ran(ctx, 13):
        return get_native_result(ctx, 13)

    tok = _get(ctx, "tok", "")
    csc_did = _get(ctx, "dep_id_csc")
    if not tok or csc_did is None:
        result = ItemResult(
            id="S5-CSC-RUN-CSC-START", name="csc Start (4445 LISTEN)",
            status=ItemStatus.SKIP,
            detail="step 05/09 미실행 — tok/dep_id 없음",
            stage=5,
        )
        _save(ctx, 13, result)
        return result

    status, _ = _post_job(ctx, int(csc_did), "start", timeout=10)
    if status not in (200, 201, 202):
        result = ItemResult(
            id="S5-CSC-RUN-CSC-START", name="csc Start (4445 LISTEN)",
            status=ItemStatus.FAIL,
            detail=f"start job 발행 실패 status={status}",
            stage=5,
        )
        _save(ctx, 13, result)
        return result

    waited = _wait_listen(4445, "tcp", 25)
    if waited >= 0:
        _set(ctx, "csc_start_ok", True)
        result = ItemResult(
            id="S5-CSC-RUN-CSC-START", name="csc Start (4445 LISTEN)",
            status=ItemStatus.PASS,
            detail=f"- [OK] csc port 4445 LISTEN ({waited}s)",
            stage=5,
        )
    else:
        _set(ctx, "csc_start_ok", False)
        result = ItemResult(
            id="S5-CSC-RUN-CSC-START", name="csc Start (4445 LISTEN)",
            status=ItemStatus.FAIL,
            detail="- [FAIL] csc port 4445 LISTEN 실패 (25s timeout)",
            stage=5,
        )
    _save(ctx, 13, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 14 — csc Health check job
# ─────────────────────────────────────────────────────────────
def _agent_job_status(job_id: int, dist_dir: str) -> Optional[tuple]:
    """agent_job (status, result_code, result_stdout) 행 반환. 없거나 오류 시 None."""
    cfg = _db.csp_db_config(dist_dir)
    if not cfg: return None
    try:
        conn = _db.connect(cfg)
    except Exception:
        return None
    try:
        cur = conn.cursor()
        cur.execute(
            "SELECT status, result_code, COALESCE(result_stdout,'') "
            "FROM agent_job WHERE id=%s", (job_id,),
        )
        row = cur.fetchone()
        if not row:
            return None
        return (str(row[0] or ""), row[1], str(row[2] or ""))
    finally:
        try: conn.close()
        except Exception: pass


def step_14_csc_health(ctx: VerifyContext) -> ItemResult:
    """Step 14 — csc Health check job 발행 + agent_job 폴링 (15s).

    PASS 조건: status=succeeded, result_code=0, result_stdout 안 'tcp:4445=open'.
    """
    if already_ran(ctx, 14):
        return get_native_result(ctx, 14)

    tok = _get(ctx, "tok", "")
    csc_did = _get(ctx, "dep_id_csc")
    if not tok or csc_did is None or not _get(ctx, "csc_start_ok"):
        result = ItemResult(
            id="S5-CSC-RUN-CSC-HEALTH", name="csc Health check",
            status=ItemStatus.SKIP,
            detail="step 13 (csc Start) 미실행 / 실패 — health 발행 의미 없음",
            stage=5,
        )
        _save(ctx, 14, result)
        return result

    status, body = _post_job(ctx, int(csc_did), "health_check", timeout=10)
    if status not in (200, 201, 202) or not isinstance(body, dict):
        result = ItemResult(
            id="S5-CSC-RUN-CSC-HEALTH", name="csc Health check",
            status=ItemStatus.FAIL,
            detail=f"health_check 발행 실패 status={status}",
            stage=5,
        )
        _save(ctx, 14, result)
        return result

    job_id = body.get("job_id")
    if job_id is None:
        result = ItemResult(
            id="S5-CSC-RUN-CSC-HEALTH", name="csc Health check",
            status=ItemStatus.FAIL,
            detail=f"응답에 job_id 누락: {str(body)[:200]}",
            stage=5,
        )
        _save(ctx, 14, result)
        return result

    job_id = int(job_id)
    final_row = None
    for _ in range(15):
        time.sleep(1)
        row = _agent_job_status(job_id, ctx.dist_dir)
        if row and row[0] in ("succeeded", "failed"):
            final_row = row
            break

    if not final_row:
        result = ItemResult(
            id="S5-CSC-RUN-CSC-HEALTH", name="csc Health check",
            status=ItemStatus.FAIL,
            detail=f"agent_job(id={job_id}) 폴링 타임아웃 (15s)",
            stage=5,
        )
        _save(ctx, 14, result)
        return result

    jstatus, rc, stdout = final_row
    ok = (jstatus == "succeeded" and (rc == 0 or rc == "0")
          and "tcp:4445=open" in stdout)
    _set(ctx, "csc_health_ok", bool(ok))
    detail = (
        f"- 결과: status={jstatus} rc={rc} out={stdout[:160]}\n"
        f"- 판정: {'OK' if ok else 'FAIL'}"
    )
    result = ItemResult(
        id="S5-CSC-RUN-CSC-HEALTH", name="csc Health check",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=detail, stage=5,
    )
    _save(ctx, 14, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 15 — console Start + 포트 8081 LISTEN
# ─────────────────────────────────────────────────────────────
def step_15_console_start(ctx: VerifyContext) -> ItemResult:
    """Step 15 — console Start job 발행 + 8081 LISTEN 대기 (25s).

    bash 본체는 console 기동 실패 시 [WARN] 로 표기 (FAIL 이 아닌)지만, 판정에선
    console_start_ok=0 이면 verdict=FAIL 로 처리됨. native 도 동일하게 FAIL 로
    분류 (UI 명확성 위해).
    """
    if already_ran(ctx, 15):
        return get_native_result(ctx, 15)

    tok = _get(ctx, "tok", "")
    console_did = _get(ctx, "dep_id_console")
    if not tok or console_did is None:
        result = ItemResult(
            id="S5-CSC-RUN-CONSOLE-START", name="console Start (8081 LISTEN)",
            status=ItemStatus.SKIP,
            detail="step 05/09 미실행 — tok/dep_id_console 없음",
            stage=5,
        )
        _save(ctx, 15, result)
        return result

    status, _ = _post_job(ctx, int(console_did), "start", timeout=10)
    if status not in (200, 201, 202):
        result = ItemResult(
            id="S5-CSC-RUN-CONSOLE-START", name="console Start (8081 LISTEN)",
            status=ItemStatus.FAIL,
            detail=f"start job 발행 실패 status={status}",
            stage=5,
        )
        _save(ctx, 15, result)
        return result

    waited = _wait_listen(8081, "tcp", 25)
    if waited >= 0:
        _set(ctx, "console_start_ok", True)
        result = ItemResult(
            id="S5-CSC-RUN-CONSOLE-START", name="console Start (8081 LISTEN)",
            status=ItemStatus.PASS,
            detail=f"- [OK] console port 8081 LISTEN ({waited}s)",
            stage=5,
        )
    else:
        _set(ctx, "console_start_ok", False)
        result = ItemResult(
            id="S5-CSC-RUN-CONSOLE-START", name="console Start (8081 LISTEN)",
            status=ItemStatus.FAIL,
            detail="- [FAIL] console port 8081 LISTEN 실패 (25s timeout)",
            stage=5,
        )
    _save(ctx, 15, result)
    return result


# ─────────────────────────────────────────────────────────────
# Composite helpers — verify_item 자식 1개에 여러 step 합산
# ─────────────────────────────────────────────────────────────
_RANK = {ItemStatus.PASS: 0, ItemStatus.SKIP: 1,
         ItemStatus.BLOCKED: 2, ItemStatus.FAIL: 3}


def _summarize(parent_id: str, name: str, results: list) -> ItemResult:
    """worst-status + 합산 elapsed 로 묶어 ItemResult 반환."""
    worst = ItemStatus.PASS
    total_ms = 0
    for r in results:
        if _RANK.get(r.status, 0) > _RANK.get(worst, 0):
            worst = r.status
        total_ms += r.elapsed_ms or 0
    n_pass = sum(1 for r in results if r.status == ItemStatus.PASS)
    n_fail = sum(1 for r in results if r.status == ItemStatus.FAIL)
    n_skip = sum(1 for r in results if r.status == ItemStatus.SKIP)
    parts: list = []
    if n_pass: parts.append(f"PASS {n_pass}")
    if n_fail: parts.append(f"FAIL {n_fail}")
    if n_skip: parts.append(f"SKIP {n_skip}")
    return ItemResult(
        id=parent_id, name=name,
        status=worst, detail=", ".join(parts) or "no children",
        elapsed_ms=total_ms, stage=5, children=list(results),
    )


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
    return _summarize("S5-CSC-DEPLOY-AGENT-ENROLL",
                      "TB-CSC admin login + agent enroll", rs)


def steps_09_10_install(ctx: VerifyContext) -> ItemResult:
    """S5-CSC-DEPLOY-INSTALL 본체 — step 09 (deployment 생성) + step 10 (install
    job + DB 폴링) 순차.
    """
    rs = [
        step_09_deployment_create(ctx),
        step_10_install_poll(ctx),
    ]
    return _summarize("S5-CSC-DEPLOY-INSTALL",
                      "Deployment + Install job + poll", rs)
