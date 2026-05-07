"""Stage 5 native step 구현 — 옛 `_verify_phase2` (bash) 22단계의 Python 포팅.

각 step 은 self-contained 함수: ctx 에서 필요한 상태를 읽고, ItemResult 반환.
ctx.state["_s5_native"] 에 결과 + 공유 변수 (JWT, agent_id, Test-agent pid 등)
캐시. 동일 step 의 재호출은 cache 로 방지 (idempotent).

** Step 매핑 ** (S5 verify_item ↔ step_NN):
  S5-RESET                          → step_01_cleanup
  S5-CSC-DEPLOY-AGENT-ENROLL        → step_05/06/07 합성
  S5-CSC-DEPLOY-PKG-UPLOAD          → step_08
  S5-CSC-DEPLOY-INSTALL             → step_09/10 합성
  S5-CSC-VERIFY-FILES               → step_11
  S5-CSC-VERIFY-OVERLAY             → step_12
  S5-CSC-RUN-CSC-START              → step_13
  S5-CSC-RUN-CSC-HEALTH             → step_14
  S5-CSC-RUN-CONSOLE-START          → step_15
  S5-MODULES-DEPLOY-AUTH            → step_16
  S5-MODULES-DEPLOY-PKG-UPLOAD      → step_17
  S5-MODULES-DEPLOY-AGENT-ENROLL    → step_18
  S5-MODULES-DEPLOY-INSTALL         → step_19/20 합성
  S5-MODULES-RUN-START              → step_21
  S5-FINALIZE                       → step_22
  (step 02/03/04 — Build/Configure/Pkg — 는 S2/S3/S4 가 이미 흡수)

** 공유 상태 구조 **
  ctx.state["_s5_native"] = {
    "results": {step_no: ItemResult},   # step 결과 cache (idempotent)
    # csc 체인 (TB-CSC 4419)
    "tok", "aid_csc", "enroll_tok_csc", "ta_pid_csc",       # 05~07
    "pkg_id_csc", "pkg_id_console",                          # 08
    "dep_id_csc", "dep_id_console",                          # 09
    "all_install_done_csc",                                  # 10
    "csc_start_ok", "csc_health_ok", "console_start_ok",     # 13~15
    # modules 체인 (배포본 csc 4445)
    "tok2", "aid_csp/cmp/sim", "enroll_tok_csp/cmp/sim",     # 16, 18
    "ta_pid_csp/cmp/sim",                                    # 18
    "pkg2_id_csp/cmp/sim",                                   # 17
    "dep2_id_csp/cmp/sim",                                   # 19
    "all_install_done_modules", "modules_start_ok",          # 20, 21
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

# 배포본 csc — verify 환경 기본 포트 (4445/8081). 운영 환경 (4420/80) 도
# ctx.opts["target"]="prod" 로 분기 가능. csp/cmp 는 두 환경 동일 (5060/9000).
_TARGET_PORTS = {
    "verify": {"csc": 4445, "console": 8081},
    "prod":   {"csc": 4420, "console": 80},
}

_MODULES = ("csp", "cmp", "sim")
_TARBALL_PREFIX = {"csp": "csp", "cmp": "cmp", "sim": "cspsim"}
_DIR_NAME       = {"csp": "csp", "cmp": "cmp", "sim": "sim"}
_AGENT_SYNC_PORT_MOD = {"csp": 9904, "cmp": 9905, "sim": 9906}
# step 21 — sim 은 install-only (Start 안 함). proto = udp.
_LISTEN_PORTS = {"csp": (5060, "udp"), "cmp": (9000, "udp")}


def _target(ctx: VerifyContext) -> str:
    return ((ctx.opts or {}).get("target") or "verify")


def _ports(ctx: VerifyContext) -> dict:
    """target → {csc:int, console:int}. 알 수 없는 target 은 verify default."""
    return _TARGET_PORTS.get(_target(ctx), _TARGET_PORTS["verify"])


def _deployed_csc_base(ctx: VerifyContext) -> str:
    """배포본 csc API URL — target 의 csc 포트로."""
    return f"https://127.0.0.1:{_ports(ctx)['csc']}"


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

    `opts.enable_mtls` true 시 admin login 직전에 csc-tb.json 토글 +
    cims.sh restart tb-csc 자동 실행. step_01 의 cims.sh reset 이 csc-tb.json
    을 config_template (false) 로 재설치하므로 reset 이후 + agent enroll
    이전 시점에 토글 + tb-csc 재시작이 효과를 가짐.
    """
    if already_ran(ctx, 5):
        return get_native_result(ctx, 5)

    if (ctx.opts or {}).get("enable_mtls"):
        from ...common.csc_config import set_mtls_enabled
        toggled = set_mtls_enabled(ctx.dist_dir, True)
        if toggled:
            shell.run_cims_sh(ctx.repo_root, "restart", "tb-csc", timeout=20)
            import time as _t
            _t.sleep(2)  # tb-csc LISTEN 안정화

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
def _csc_overlay(name: str, ports: dict) -> dict:
    """csc-server 자식 config overlay — target 의 포트 매핑."""
    if name == "csc":     return {"Server.Port": ports["csc"]}
    if name == "console": return {"Port": ports["console"]}
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
    ports = _ports(ctx)
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
            "config":        _csc_overlay(name, ports),
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
    # agent_deployment.status enum: pending|deploying|running|stopped|failed|removed
    # 폴링 종료 조건: pending/deploying 가 사라질 때 (install 완료 또는 실패)
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
    # 정상 완료 상태: running/stopped (install 후 자동 start 여부에 따라 분기)
    # 실패 상태: failed/removed
    for name, did in deployments:
        st = final_status.get(name, "(unknown)")
        ok = st in ("running", "stopped")
        notes.append(
            f"- [{'OK' if ok else 'WARN'}] {name}: did={did} status={st}"
        )

    overall = ItemStatus.PASS if all_done and all(
        final_status.get(n) in ("running", "stopped") for n, _ in deployments
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
    """Step 12 — csc/config.json overlay 반영 (target 의 csc 포트 매칭)."""
    if already_ran(ctx, 12):
        return get_native_result(ctx, 12)

    install_path = os.path.join(ctx.dist_dir, "csc-server", "csc")
    port = _read_csc_port(install_path)
    expected = _ports(ctx)["csc"]
    if port == expected:
        result = ItemResult(
            id="S5-CSC-VERIFY-OVERLAY", name="config overlay 반영",
            status=ItemStatus.PASS,
            detail=f"- [OK] csc/config.json: Server.Port={expected} 반영 ({install_path})",
            stage=5,
        )
    else:
        result = ItemResult(
            id="S5-CSC-VERIFY-OVERLAY", name="config overlay 반영",
            status=ItemStatus.FAIL,
            detail=f"- [FAIL] csc/config.json Server.Port 이상 (실제={port}, 기대={expected}, "
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
    """Step 13 — csc Start job 발행 + LISTEN 대기 (25s, target 의 csc 포트).

    `opts.enable_mtls` true 시 csc 시작 직전 csc-tb.json `Agent.MtlsEnabled`
    를 true 로 토글. 후속 신규 enroll agent (csp/cmp/sim) 가 mTLS cert 발급
    받아 S6-SCN-CERT-ROTATE 가 PASS 가능. csc 메모리에는 enroll 시점에
    반영되므로 이후 재시작 불필요.
    """
    if already_ran(ctx, 13):
        return get_native_result(ctx, 13)

    if (ctx.opts or {}).get("enable_mtls"):
        from ...common.csc_config import set_mtls_enabled
        toggled = set_mtls_enabled(ctx.dist_dir, True)
        _set(ctx, "mtls_toggled", bool(toggled))
        # 주의: TB-CSC (4419) 가 이미 LISTEN 중이라면 csc-tb.json 캐시 가능성.
        # 효과 보장하려면 사용자가 사전에 `cims.sh restart csc` 1회 실행 권장.
        # (배포본 csc-server 4445 는 step_13 의 start job 으로 신규 시작이라
        #  토글이 자동 반영.)

    tok = _get(ctx, "tok", "")
    csc_did = _get(ctx, "dep_id_csc")
    csc_port = _ports(ctx)["csc"]
    name = f"csc Start ({csc_port} LISTEN)"
    if not tok or csc_did is None:
        result = ItemResult(
            id="S5-CSC-RUN-CSC-START", name=name,
            status=ItemStatus.SKIP,
            detail="step 05/09 미실행 — tok/dep_id 없음",
            stage=5,
        )
        _save(ctx, 13, result)
        return result

    status, _ = _post_job(ctx, int(csc_did), "start", timeout=10)
    if status not in (200, 201, 202):
        result = ItemResult(
            id="S5-CSC-RUN-CSC-START", name=name,
            status=ItemStatus.FAIL,
            detail=f"start job 발행 실패 status={status}",
            stage=5,
        )
        _save(ctx, 13, result)
        return result

    waited = _wait_listen(csc_port, "tcp", 25)
    if waited >= 0:
        _set(ctx, "csc_start_ok", True)
        result = ItemResult(
            id="S5-CSC-RUN-CSC-START", name=name,
            status=ItemStatus.PASS,
            detail=f"- [OK] csc port {csc_port} LISTEN ({waited}s)",
            stage=5,
        )
    else:
        _set(ctx, "csc_start_ok", False)
        result = ItemResult(
            id="S5-CSC-RUN-CSC-START", name=name,
            status=ItemStatus.FAIL,
            detail=f"- [FAIL] csc port {csc_port} LISTEN 실패 (25s timeout)",
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
    csc_port = _ports(ctx)["csc"]
    ok = (jstatus == "succeeded" and (rc == 0 or rc == "0")
          and f"tcp:{csc_port}=open" in stdout)
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
    """Step 15 — console Start job 발행 + LISTEN 대기 (25s, target 의 console 포트).

    target=prod 시 80 (운영 — cap_net_bind 또는 reverse proxy 필요).
    """
    if already_ran(ctx, 15):
        return get_native_result(ctx, 15)

    tok = _get(ctx, "tok", "")
    console_did = _get(ctx, "dep_id_console")
    cport = _ports(ctx)["console"]
    name = f"console Start ({cport} LISTEN)"
    if not tok or console_did is None:
        result = ItemResult(
            id="S5-CSC-RUN-CONSOLE-START", name=name,
            status=ItemStatus.SKIP,
            detail="step 05/09 미실행 — tok/dep_id_console 없음",
            stage=5,
        )
        _save(ctx, 15, result)
        return result

    status, _ = _post_job(ctx, int(console_did), "start", timeout=10)
    if status not in (200, 201, 202):
        result = ItemResult(
            id="S5-CSC-RUN-CONSOLE-START", name=name,
            status=ItemStatus.FAIL,
            detail=f"start job 발행 실패 status={status}",
            stage=5,
        )
        _save(ctx, 15, result)
        return result

    waited = _wait_listen(cport, "tcp", 25)
    if waited >= 0:
        _set(ctx, "console_start_ok", True)
        result = ItemResult(
            id="S5-CSC-RUN-CONSOLE-START", name=name,
            status=ItemStatus.PASS,
            detail=f"- [OK] console port {cport} LISTEN ({waited}s)",
            stage=5,
        )
    else:
        _set(ctx, "console_start_ok", False)
        result = ItemResult(
            id="S5-CSC-RUN-CONSOLE-START", name=name,
            status=ItemStatus.FAIL,
            detail=f"- [FAIL] console port {cport} LISTEN 실패 (25s timeout)",
            stage=5,
        )
    _save(ctx, 15, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 16 — 배포본 csc(4445) admin login
# ─────────────────────────────────────────────────────────────
def step_16_modules_auth(ctx: VerifyContext) -> ItemResult:
    """Step 16 — 배포본 csc(4445) admin login → tok2.

    csc 가 step 13 에서 4445 로 LISTEN 중이어야 함. csc_start_ok 가 False 면 SKIP.
    DB 는 TB-CSC 와 공유 — admin/1234 동일.
    """
    if already_ran(ctx, 16):
        return get_native_result(ctx, 16)

    if not _get(ctx, "csc_start_ok"):
        result = ItemResult(
            id="S5-MODULES-DEPLOY-AUTH", name="배포본 csc admin login",
            status=ItemStatus.SKIP,
            detail="step 13 (csc Start 4445) 미실행 / 실패",
            stage=5,
        )
        _save(ctx, 16, result)
        return result

    base = _deployed_csc_base(ctx)
    login_id = os.environ.get("CIMS_TB_ADMIN_ID", "admin")
    pw = os.environ.get("CIMS_TB_ADMIN_PASSWORD", "1234")
    tok2 = csc_http.admin_login(base, login_id, pw, timeout=5)
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-DEPLOY-AUTH", name="배포본 csc admin login",
            status=ItemStatus.FAIL,
            detail=f"admin login 실패 (base={base} id={login_id})",
            stage=5,
        )
        _save(ctx, 16, result)
        return result

    _set(ctx, "tok2", tok2)
    result = ItemResult(
        id="S5-MODULES-DEPLOY-AUTH", name="배포본 csc admin login",
        status=ItemStatus.PASS,
        detail=f"base={base} id={login_id} → JWT (len={len(tok2)})",
        stage=5,
    )
    _save(ctx, 16, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 17 — csp/cmp/cspsim 패키지 업로드 (배포본 csc 4445)
# ─────────────────────────────────────────────────────────────
def step_17_modules_pkg_upload(ctx: VerifyContext) -> ItemResult:
    """Step 17 — 3 모듈 (csp/cmp/sim) tarball 을 배포본 csc(4445) 에 업로드.

    sim 의 tarball prefix 는 'cspsim' (TARBALL_PREFIX 매핑). pkg2_id_{m} 캐시.
    """
    if already_ran(ctx, 17):
        return get_native_result(ctx, 17)

    tok2 = _get(ctx, "tok2", "")
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-DEPLOY-PKG-UPLOAD",
            name="csp/cmp/cspsim 패키지 업로드",
            status=ItemStatus.SKIP,
            detail="step 16 (배포본 csc admin login) 미실행 / 실패",
            stage=5,
        )
        _save(ctx, 17, result)
        return result

    base = _deployed_csc_base(ctx)
    pkg_dir = os.path.join(ctx.dist_dir, "packages")
    notes: list = []
    fail = False
    for m in _MODULES:
        prefix = _TARBALL_PREFIX[m]
        tar = _latest_tarball(pkg_dir, prefix)
        if not tar:
            notes.append(f"- [FAIL] {m}: tarball 없음 ({prefix}-*.tar.gz)")
            fail = True
            continue
        try:
            status, body = csc_http.post_multipart(
                f"{base}/api/v1/packages",
                file_path=tar, form_fields={"force": "true"},
                token=tok2, timeout=120,
            )
        except Exception as e:
            notes.append(f"- [FAIL] {m}: 업로드 예외 {type(e).__name__}: {e}")
            fail = True
            continue
        if status not in (200, 201) or not isinstance(body, dict) or not body.get("id"):
            notes.append(f"- [FAIL] {m}: status={status} body={str(body)[:120]}")
            fail = True
            continue
        pkg_id = int(body["id"])
        _set(ctx, f"pkg2_id_{m}", pkg_id)
        notes.append(f"- [OK] {m}: package_id={pkg_id} ({os.path.basename(tar)})")

    result = ItemResult(
        id="S5-MODULES-DEPLOY-PKG-UPLOAD",
        name="csp/cmp/cspsim 패키지 업로드",
        status=ItemStatus.FAIL if fail else ItemStatus.PASS,
        detail="\n".join(notes), stage=5,
    )
    _save(ctx, 17, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 18 — 3 agent 등록 + 3 Test-agent spawn + 전 agent enroll 폴링
# ─────────────────────────────────────────────────────────────
def _register_one_module_agent(base: str, tok: str, aname: str) -> tuple:
    """단일 module agent 재생성 + approve. (aid, enroll_tok, error_msg) 반환.

    error_msg 가 비어있으면 성공. 기존 동명 agent 는 DELETE 후 POST.
    """
    prev = csc_http.find_agent_id_by_name(base, tok, aname)
    if prev:
        csc_http.delete(f"{base}/api/v1/agents/{prev}", token=tok)
    try:
        st, body = csc_http.post_json(
            f"{base}/api/v1/agents",
            {"name": aname, "note": "S5 modules native"},
            token=tok, timeout=10,
        )
    except Exception as e:
        return (None, "", f"POST /agents 예외 {type(e).__name__}: {e}")
    if st not in (200, 201) or not isinstance(body, dict) or body.get("id") is None:
        return (None, "", f"POST /agents status={st} body={str(body)[:120]}")
    aid = int(body["id"])
    enroll_tok = str(body.get("enrollment_token") or "")
    if not enroll_tok:
        return (None, "", "응답에 enrollment_token 누락")
    try:
        ap_st, _ = csc_http.post_json(
            f"{base}/api/v1/agents/{aid}/approve", {}, token=tok, timeout=5,
        )
    except Exception as e:
        return (None, "", f"approve 예외 {type(e).__name__}: {e}")
    if ap_st not in (200, 204):
        return (None, "", f"approve status={ap_st}")
    return (aid, enroll_tok, "")


def _spawn_one_module_agent(ctx: VerifyContext, m: str, base: str,
                             aname: str, enroll_tok: str) -> tuple:
    """단일 module Test-agent spawn. (pid, error_msg). cims_agent.py 부재 시
    error 반환. log 는 LOG_DIR/test-agent-<m>-server.log."""
    dist = ctx.dist_dir
    sync_port = _AGENT_SYNC_PORT_MOD[m]
    ta_dir = os.path.join(dist, f"{m}-server", "agent")
    state_dir = os.path.join(ta_dir, "state")
    ta_log = os.path.join(ctx.repo_root, "logs", f"test-agent-{m}-server.log")
    os.makedirs(state_dir, exist_ok=True)
    os.makedirs(os.path.dirname(ta_log), exist_ok=True)

    agent_py = os.path.join(dist, "agent", "cims_agent.py")
    if not os.path.isfile(agent_py):
        return (None, f"cims_agent.py 없음: {agent_py}")

    import subprocess
    env = dict(os.environ)
    env["CIMS_AGENT_INSTALL_ROOT"] = os.path.join(dist, f"{m}-server")
    env["CIMS_AGENT_SYNC_PORT"] = str(sync_port)
    log_fp = open(ta_log, "w")
    try:
        proc = subprocess.Popen(
            ["python3", agent_py,
             "--csc-url", base, "--name", aname,
             "--state-dir", state_dir,
             "--enrollment-token", enroll_tok,
             "--heartbeat-sec", "3"],
            env=env, stdout=log_fp, stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        return (proc.pid, "")
    except Exception as e:
        try: log_fp.close()
        except Exception: pass
        return (None, f"Popen 실패: {type(e).__name__}: {e}")


def _all_modules_online(dist_dir: str) -> bool:
    """3 module agent 모두 cims_agent.status='online' 인지."""
    for m in _MODULES:
        if not _agent_online_in_db(f"{m}-server-local", dist_dir):
            return False
    return True


def step_18_modules_agent_enroll(ctx: VerifyContext) -> ItemResult:
    """Step 18 — 3 module agent register + 3 Test-agent spawn + enroll 폴링 20s.

    개별 모듈 등록/spawn 이 실패해도 다른 모듈은 진행. 마지막에 3 모두 online
    이어야 PASS.
    """
    if already_ran(ctx, 18):
        return get_native_result(ctx, 18)

    tok2 = _get(ctx, "tok2", "")
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-DEPLOY-AGENT-ENROLL",
            name="3 모듈 agent + Test-agent",
            status=ItemStatus.SKIP,
            detail="step 16 미실행 — tok2 없음", stage=5,
        )
        _save(ctx, 18, result)
        return result

    base = _deployed_csc_base(ctx)
    notes: list = []
    register_fail = False
    for m in _MODULES:
        aname = f"{m}-server-local"
        aid, enroll_tok, err = _register_one_module_agent(base, tok2, aname)
        if err:
            notes.append(f"- [FAIL] {m}: {err}")
            register_fail = True
            continue
        _set(ctx, f"aid_{m}", aid)
        _set(ctx, f"enroll_tok_{m}", enroll_tok)
        pid, perr = _spawn_one_module_agent(ctx, m, base, aname, enroll_tok)
        if perr:
            notes.append(f"- [FAIL] {m}: spawn {perr}")
            register_fail = True
            continue
        _set(ctx, f"ta_pid_{m}", pid)
        notes.append(
            f"- [OK] {m}: aid={aid} pid={pid} sync={_AGENT_SYNC_PORT_MOD[m]}"
        )

    # enroll polling — 모두 online 까지 20s
    online = False
    waited = 0
    if not register_fail:
        for _ in range(20):
            time.sleep(1); waited += 1
            if _all_modules_online(ctx.dist_dir):
                online = True; break

    if not register_fail and online:
        notes.append(f"- 전 agent enroll: OK ({waited}s)")
        status = ItemStatus.PASS
    elif not register_fail and not online:
        notes.append(f"- 전 agent enroll: TIMEOUT (20s)")
        status = ItemStatus.FAIL
    else:
        notes.append("- 일부 모듈 register/spawn 실패 — enroll polling 미실행")
        status = ItemStatus.FAIL

    result = ItemResult(
        id="S5-MODULES-DEPLOY-AGENT-ENROLL",
        name="3 모듈 agent + Test-agent",
        status=status, detail="\n".join(notes), stage=5,
    )
    _save(ctx, 18, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 19 — 3 deployment 생성 (csp/cmp/sim, overlay 없음)
# ─────────────────────────────────────────────────────────────
def step_19_modules_deployment_create(ctx: VerifyContext) -> ItemResult:
    """Step 19 — csp/cmp/sim 3 deployment 생성. overlay 없음. dep2_id_{m} 캐시.

    install_path = $DIST_DIR/{m}-server/{dir_name[m]}, process_name = upper.
    """
    if already_ran(ctx, 19):
        return get_native_result(ctx, 19)

    tok2 = _get(ctx, "tok2", "")
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-DEPLOY-INSTALL-CREATE", name="3 모듈 deployment 생성",
            status=ItemStatus.SKIP,
            detail="step 16 미실행 — tok2 없음", stage=5,
        )
        _save(ctx, 19, result)
        return result

    base = _deployed_csc_base(ctx)
    notes: list = []
    fail = False
    for m in _MODULES:
        aid = _get(ctx, f"aid_{m}")
        pkg_id = _get(ctx, f"pkg2_id_{m}")
        if aid is None or pkg_id is None:
            notes.append(f"- [SKIP] {m}: aid/pkg_id 미확보 (step 17/18 실패)")
            fail = True
            continue
        modname = _DIR_NAME[m]
        install_path = os.path.join(ctx.dist_dir, f"{m}-server", modname)
        pname = _TARBALL_PREFIX[m].upper()    # CSP/CMP/CSPSIM
        payload = {
            "agent_id":     int(aid),
            "package_id":   int(pkg_id),
            "install_path": install_path,
            "process_name": pname,
        }
        try:
            st, body = csc_http.post_json(
                f"{base}/api/v1/deployments", payload, token=tok2, timeout=15,
            )
        except Exception as e:
            notes.append(f"- [FAIL] {m}: 호출 예외 {type(e).__name__}: {e}")
            fail = True
            continue
        if st not in (200, 201) or not isinstance(body, dict) or not body.get("id"):
            notes.append(f"- [FAIL] {m}: status={st} body={str(body)[:120]}")
            fail = True
            continue
        did = int(body["id"])
        _set(ctx, f"dep2_id_{m}", did)
        notes.append(f"- [OK] {m}: deployment_id={did} → {install_path} (process={pname})")

    result = ItemResult(
        id="S5-MODULES-DEPLOY-INSTALL-CREATE", name="3 모듈 deployment 생성",
        status=ItemStatus.FAIL if fail else ItemStatus.PASS,
        detail="\n".join(notes), stage=5,
    )
    _save(ctx, 19, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 20 — 3 install jobs + DB 폴링
# ─────────────────────────────────────────────────────────────
def step_20_modules_install_poll(ctx: VerifyContext) -> ItemResult:
    """Step 20 — csp/cmp/sim install jobs 발행 + agent_deployment 상태 폴링 60s."""
    if already_ran(ctx, 20):
        return get_native_result(ctx, 20)

    tok2 = _get(ctx, "tok2", "")
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-DEPLOY-INSTALL-POLL", name="3 모듈 install + 폴링",
            status=ItemStatus.SKIP,
            detail="step 16 미실행 — tok2 없음", stage=5,
        )
        _save(ctx, 20, result)
        return result

    deployments: list = []
    for m in _MODULES:
        did = _get(ctx, f"dep2_id_{m}")
        if did is not None:
            deployments.append((m, int(did)))
    if not deployments:
        result = ItemResult(
            id="S5-MODULES-DEPLOY-INSTALL-POLL", name="3 모듈 install + 폴링",
            status=ItemStatus.FAIL,
            detail="step 19 에서 deployment 미확보", stage=5,
        )
        _save(ctx, 20, result)
        return result

    base = _deployed_csc_base(ctx)
    notes: list = []
    for m, did in deployments:
        try:
            st, _ = csc_http.post_json(
                f"{base}/api/v1/deployments/{did}/job",
                {"job_type": "install"}, token=tok2, timeout=10,
            )
        except Exception as e:
            notes.append(f"- [FAIL] {m}: install 발행 예외 {e}")
            continue
        if st not in (200, 201, 202):
            notes.append(f"- [FAIL] {m}: install 발행 status={st}")

    elapsed = 0
    all_done = False
    final_status: dict = {}
    for _ in range(30):
        time.sleep(2); elapsed += 2
        still = False
        for m, did in deployments:
            st = _deployment_status(m, did, ctx.dist_dir)
            final_status[m] = st or "(unknown)"
            if st in ("pending", "deploying"):
                still = True
        if not still:
            all_done = True; break

    _set(ctx, "all_install_done_modules", bool(all_done))
    # agent_deployment enum: pending|deploying|running|stopped|failed|removed
    # 정상 완료: running/stopped. 실패: failed/removed.
    for m, did in deployments:
        st = final_status.get(m, "(unknown)")
        ok = st in ("running", "stopped")
        notes.append(f"- [{'OK' if ok else 'WARN'}] {m}: did={did} status={st}")

    overall = ItemStatus.PASS if all_done and all(
        final_status.get(m) in ("running", "stopped") for m, _ in deployments
    ) else ItemStatus.FAIL
    summary = f"all_done={all_done} elapsed={elapsed}s\n" + "\n".join(notes)
    result = ItemResult(
        id="S5-MODULES-DEPLOY-INSTALL-POLL", name="3 모듈 install + 폴링",
        status=overall, detail=summary, stage=5,
    )
    _save(ctx, 20, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 21 — csp/cmp Start + LISTEN (sim install-only)
# ─────────────────────────────────────────────────────────────
def step_21_modules_start(ctx: VerifyContext) -> ItemResult:
    """Step 21 — csp 5060/udp + cmp 9000/udp Start. sim 은 install-only."""
    if already_ran(ctx, 21):
        return get_native_result(ctx, 21)

    tok2 = _get(ctx, "tok2", "")
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-RUN-START", name="csp/cmp Start (sim install-only)",
            status=ItemStatus.SKIP,
            detail="step 16 미실행 — tok2 없음", stage=5,
        )
        _save(ctx, 21, result)
        return result

    base = _deployed_csc_base(ctx)
    notes: list = []
    started: list = []
    fail = False
    for m, (port, proto) in _LISTEN_PORTS.items():
        did = _get(ctx, f"dep2_id_{m}")
        if did is None:
            notes.append(f"- [FAIL] {m}: dep2_id 없음 (step 19 실패)")
            fail = True; continue
        try:
            st, _ = csc_http.post_json(
                f"{base}/api/v1/deployments/{did}/job",
                {"job_type": "start"}, token=tok2, timeout=10,
            )
        except Exception as e:
            notes.append(f"- [FAIL] {m}: start 발행 예외 {e}")
            fail = True; continue
        if st not in (200, 201, 202):
            notes.append(f"- [FAIL] {m}: start 발행 status={st}")
            fail = True; continue
        started.append((m, port, proto))

    for m, port, proto in started:
        waited = _wait_listen(port, proto, 20)
        if waited >= 0:
            notes.append(f"- [OK] {m}: port {port}/{proto} LISTEN ({waited}s)")
        else:
            notes.append(f"- [FAIL] {m}: port {port}/{proto} LISTEN 실패 (20s)")
            fail = True

    _set(ctx, "modules_start_ok", not fail)

    # csp ↔ cmp control connection 안정화 대기 (default max 150s).
    # csp 시작 직후 cmp 와의 첫 heartbeat 응답까지 ~120s 소요. 이 wait 가
    # 없으면 후속 S6 PTT 시나리오의 InviteMember 가 'Failed to get/alloc
    # Shared Port' 로 실패 (csp 가 cmp 에 ADD_PTT_GROUP 보낼 수 없음).
    # `CIMS_VERIFY_CMP_WAIT_S=0` 환경변수로 wait 자체 비활성 (unit test 용).
    cmp_wait_s = int(os.environ.get("CIMS_VERIFY_CMP_WAIT_S", "150"))
    if not fail and cmp_wait_s > 0:
        import time as _t
        from glob import glob as _glob
        log_glob = os.path.join(ctx.dist_dir, "csp-server", "csp", "csp", "log",
                                "csp_*.log")
        deadline = _t.time() + cmp_wait_s
        csp_cmp_connected = False
        while _t.time() < deadline:
            for p in _glob(log_glob):
                try:
                    with open(p, "rb") as f:
                        if b"OnCmpStatusChanged: Connected" in f.read():
                            csp_cmp_connected = True
                            break
                except OSError:
                    pass
            if csp_cmp_connected:
                break
            _t.sleep(3)
        notes.append(
            f"- csp ↔ cmp control connection: "
            f"{'CONNECTED' if csp_cmp_connected else 'TIMEOUT'}"
        )

    # Immutability marker — Start PASS 시 manifest sha 기록 (S6-ENTRY-CHECK 가 매칭).
    if not fail:
        try:
            from ...common import pkg_manifest as _pkgm
            sha = _pkgm.write_marker(ctx.dist_dir)
            if sha:
                notes.append(
                    f"- [marker] .deployed-manifest.json 기록 (manifest_sha={sha[:12]}…)"
                )
        except Exception as e:
            notes.append(f"- [marker] 기록 실패: {e}")

    result = ItemResult(
        id="S5-MODULES-RUN-START", name="csp/cmp Start (sim install-only)",
        status=ItemStatus.FAIL if fail else ItemStatus.PASS,
        detail="\n".join(notes), stage=5,
    )
    _save(ctx, 21, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 22 — Stop / 기동 유지 (--stop-after)
# ─────────────────────────────────────────────────────────────
def step_22_finalize(ctx: VerifyContext) -> ItemResult:
    """Step 22 — `stop_after=True` 면 모든 deployment stop + Test-agent 4개 kill.
    기본 (False) 은 정보성 — 4 ports 기동 유지 (Phase 3 진입 가능).
    """
    if already_ran(ctx, 22):
        return get_native_result(ctx, 22)

    if not ctx.stop_after:
        notes = [
            "- 전체 기동 유지 (기본)",
            "- csc(4445) · console(8081) · csp(5060/udp) · cmp(9000/udp)",
            "- Test-agent 4개 (sync 9903~9906) heartbeat 유지",
            "- sim 은 install-only (cspsim 단발 실행 — Phase 3 cmd_sim 경유)",
        ]
        result = ItemResult(
            id="S5-FINALIZE", name="배포 마무리 (기동 유지)",
            status=ItemStatus.PASS, detail="\n".join(notes), stage=5,
        )
        _save(ctx, 22, result)
        return result

    # --stop-after: 모든 deployment stop + Test-agent kill
    notes: list = []
    # csc/console stop (TB-CSC 4419 경유)
    tok = _get(ctx, "tok", "")
    if tok:
        for k in ("csc", "console"):
            did = _get(ctx, f"dep_id_{k}")
            if did is None: continue
            try:
                st, _ = csc_http.post_json(
                    f"{_TB_CSC_BASE}/api/v1/deployments/{did}/job",
                    {"job_type": "stop"}, token=tok, timeout=10,
                )
                notes.append(f"- {k}: stop 발행 status={st}")
            except Exception as e:
                notes.append(f"- {k}: stop 발행 예외 {e}")
    # csp/cmp stop (배포본 csc 4445 경유) — sim 은 install-only 이므로 stop 무의미
    tok2 = _get(ctx, "tok2", "")
    if tok2:
        for m in ("csp", "cmp"):
            did = _get(ctx, f"dep2_id_{m}")
            if did is None: continue
            try:
                st, _ = csc_http.post_json(
                    f"{_deployed_csc_base(ctx)}/api/v1/deployments/{did}/job",
                    {"job_type": "stop"}, token=tok2, timeout=10,
                )
                notes.append(f"- {m}: stop 발행 status={st}")
            except Exception as e:
                notes.append(f"- {m}: stop 발행 예외 {e}")
    time.sleep(5)

    # Test-agent kill
    import signal
    killed = 0
    pid_keys = [("ta_pid_csc", "csc-server-local")]
    pid_keys += [(f"ta_pid_{m}", f"{m}-server-local") for m in _MODULES]
    for k, _aname in pid_keys:
        pid = _get(ctx, k)
        if not pid: continue
        try:
            os.kill(int(pid), signal.SIGTERM)
            killed += 1
        except Exception:
            pass
    notes.append(f"- Test-agent kill: {killed}/4")

    result = ItemResult(
        id="S5-FINALIZE", name="배포 마무리 (--stop-after)",
        status=ItemStatus.PASS, detail="\n".join(notes), stage=5,
    )
    _save(ctx, 22, result)
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


def steps_19_20_modules_install(ctx: VerifyContext) -> ItemResult:
    """S5-MODULES-DEPLOY-INSTALL 본체 — step 19 + 20 합성."""
    rs = [
        step_19_modules_deployment_create(ctx),
        step_20_modules_install_poll(ctx),
    ]
    return _summarize("S5-MODULES-DEPLOY-INSTALL",
                      "3 모듈 deployment + install + poll", rs)
