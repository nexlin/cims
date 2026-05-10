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
import shutil
import signal
import subprocess
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
_AGENT_NAME_CSC = "mgmt-server"
_DIST_ROOT_CSC = "mgmt-server"
_AGENT_SYNC_PORT_CSC = 9903

# 배포본 csc — verify 환경 기본 포트 (4445/8081). 운영 환경 (4420/80) 도
# ctx.opts["target"]="prod" 로 분기 가능. csp/cmp 는 두 환경 동일 (5060/9000).
_TARGET_PORTS = {
    "verify": {"csc": 4445, "console": 8081},
    "prod":   {"csc": 4420, "console": 80},
}

# Instance descriptor — 한 entry = 한 service-server 인스턴스. tarball/install/process/
# sync_port/local_ip/listen 의 SoT. step 17~21 + finalize 가 이 list 만 보고
# 동작 — 인스턴스 추가/제거가 자연 일반화. ISP/IMP 는 P2 에서 entry 추가.
#
# 사용자 토폴로지 매핑:
#   VoLTE SIP Server  = CSP (P2: + ISP)        agent=volte-sip-server
#   VoLTE Media Server = CMP (P2: + IMP)       agent=volte-media-server
#   PTT SIP Server    = PSP                    agent=ptt-sip-server
#   PTT Media Server  = PMP                    agent=ptt-media-server
#
# CIMS 관리 서버 (mgmt-server) 는 별도 — TB-CSC chain 으로 csc + console + sim
# (+ 향후 cwrtc/phone) 을 모두 install. _CSC_PACKAGES 참조.
#
# 필드:
#   id              — 모듈 식별자 (소문자, dep2_id_<id> 등 캐시 키, 내부 사용)
#   display_name    — UI 표시명 ("VoLTE SIP Server" 등)
#   agent_name      — cims_agent.name + dist root 디렉토리 (kebab-case)
#   tarball         — packages/<tarball>-*.tar.gz 의 prefix
#   dir             — install_path 의 leaf (dist/{agent_name}/{dir})
#   process         — CSC API 의 process_name (대문자)
#   sync_port       — Test-agent sync port (CIMS_AGENT_SYNC_PORT)
#   local_ip        — 인스턴스의 LocalIp (csp.json/cmp.json overlay)
#   listen          — (port, proto)
#   peer_id         — 짝 미디어/시그널링 인스턴스 id (csp↔cmp, psp↔pmp). 없으면 None.
#   config_overlay  — deployment payload 의 config 필드 (dotted-key dict).
#                     csp 변종은 Roles + LocalIp + MediaServer 분기, cmp 변종은
#                     RtpIp + CspIp 분기. cims.sh start_*_variant 가 시작 직전
#                     install_path/config.json 을 모듈 csp.json/cmp.json 에 머지.
_INSTANCES = [
    {"id": "csp",
     "display_name": "VoLTE SIP Server",
     "agent_name":   "volte-sip-server",
     "tarball": "csp", "dir": "csp", "process": "CSP",
     "sync_port": 9904, "local_ip": "127.0.0.1", "listen": (5060, "udp"),
     "peer_id": "cmp",
     "config_overlay": {
         "Setup.Roles.CSCF":   True,
         "Setup.Roles.TAS":    True,
         "Setup.Roles.PTT_AS": False,    # PTT_AS 는 PSP 가 담당
         "Setup.Roles.IBCF":   False,    # IBCF 는 ISP (P2) 가 담당
         "Setup.Sip.LocalIp":         "127.0.0.1",
         "Setup.MediaServer.Host":    "127.0.0.1",
         "Setup.MediaServer.LocalIp": "127.0.0.1",
         # dev csp (9001) 와 충돌 회피 — 배포본 CSP 는 9011 사용.
         "Setup.MediaServer.LocalPort": 9011,
     }},
    {"id": "psp",
     "display_name": "PTT SIP Server",
     "agent_name":   "ptt-sip-server",
     "tarball": "psp", "dir": "psp", "process": "PSP",
     "sync_port": 9907, "local_ip": "127.0.0.3", "listen": (5060, "udp"),
     "peer_id": "pmp",
     "config_overlay": {
         "Setup.Roles.CSCF":   True,     # PTT 가입자 register 처리
         "Setup.Roles.TAS":    False,    # VoLTE B2BUA 는 CSP 가 담당
         "Setup.Roles.PTT_AS": True,
         "Setup.Roles.IBCF":   False,
         "Setup.Sip.LocalIp":         "127.0.0.3",
         "Setup.MediaServer.Host":    "127.0.0.3",  # PMP control port
         "Setup.MediaServer.LocalIp": "127.0.0.3",
         # 인스턴스별 LocalPort 분리 — dev csp(9001)/CSP(9011) 와 충돌 회피.
         "Setup.MediaServer.LocalPort": 9012,
     }},
    {"id": "cmp",
     "display_name": "VoLTE Media Server",
     "agent_name":   "volte-media-server",
     "tarball": "cmp", "dir": "cmp", "process": "CMP",
     "sync_port": 9905, "local_ip": "127.0.0.1", "listen": (9000, "udp"),
     "peer_id": "csp",
     "config_overlay": {
         "ServerIp": "127.0.0.1",   # 9000 control port host-specific bind
         "RtpIp":    "127.0.0.1",
         "CspIp":    "127.0.0.1",
     }},
    {"id": "pmp",
     "display_name": "PTT Media Server",
     "agent_name":   "ptt-media-server",
     "tarball": "pmp", "dir": "pmp", "process": "PMP",
     "sync_port": 9908, "local_ip": "127.0.0.3", "listen": (9000, "udp"),
     "peer_id": "psp",
     "config_overlay": {
         "ServerIp": "127.0.0.3",   # PMP 9000 host-specific bind (CMP 와 분리)
         "RtpIp":    "127.0.0.3",
         "CspIp":    "127.0.0.3",   # PSP IP
     }},
    # P2 — IBCF 트렁크 분리. ISP 는 IBCF role 단독 (CSCF/TAS/PTT_AS=false).
    # 별도 loopback 127.0.0.5 사용 — `sudo ip addr add 127.0.0.5/8 dev lo` 1회 필요.
    # 라우팅 정책 (routing_policies.jsonl / routes.jsonl) seed 는 별도 round (P3).
    {"id": "isp",
     "display_name": "IBCF SIP Server",
     "agent_name":   "ibcf-sip-server",
     "tarball": "isp", "dir": "isp", "process": "ISP",
     "sync_port": 9909, "local_ip": "127.0.0.5", "listen": (5060, "udp"),
     "peer_id": "imp",
     "config_overlay": {
         "Setup.Roles.CSCF":   False,   # IBCF 단독 — 가입자 register 미수행
         "Setup.Roles.TAS":    False,
         "Setup.Roles.PTT_AS": False,
         "Setup.Roles.IBCF":   True,
         "Setup.Sip.LocalIp":         "127.0.0.5",
         "Setup.MediaServer.Host":    "127.0.0.5",
         "Setup.MediaServer.LocalIp": "127.0.0.5",
         # 인스턴스별 LocalPort 분리 — dev csp(9001)/CSP(9011)/PSP(9012) 와 충돌 회피.
         "Setup.MediaServer.LocalPort": 9013,
     }},
    {"id": "imp",
     "display_name": "IBCF Media Server",
     "agent_name":   "ibcf-media-server",
     "tarball": "imp", "dir": "imp", "process": "IMP",
     "sync_port": 9910, "local_ip": "127.0.0.5", "listen": (9000, "udp"),
     "peer_id": "isp",
     "config_overlay": {
         "ServerIp": "127.0.0.5",   # IMP 9000 host-specific bind (CMP/PMP 와 분리)
         "RtpIp":    "127.0.0.5",
         "CspIp":    "127.0.0.5",   # ISP IP
     }},
]


def _instance(iid: str) -> dict:
    for inst in _INSTANCES:
        if inst["id"] == iid:
            return inst
    raise KeyError(iid)


def _module_ids() -> tuple:
    return tuple(inst["id"] for inst in _INSTANCES)


def _install_root(dist_dir: str, inst: dict) -> str:
    """`<dist_dir>/<agent_name>/` — install_path 의 server-level 부모."""
    return os.path.join(dist_dir, inst["agent_name"])


def _install_path(dist_dir: str, inst: dict) -> str:
    """`<dist_dir>/<agent_name>/` — 변종 tarball 의 install 위치.

    cims_agent 가 tarball 풀면 안의 `<variant>/` 디렉토리 (psp/, isp/, pmp/, imp/)
    가 그대로 풀려서 `<dist>/<agent_name>/<variant>/...` 가 됨. 즉 `inst["dir"]`
    는 tarball 안 디렉토리 이름 = variant 이름과 동일 (server level + tarball
    내부 디렉토리 = 자기 이름). 따라서 install_path 는 server level 로 두고,
    sub-dir 접근은 호출처에서 `inst["dir"]` 와 join.
    """
    return os.path.join(dist_dir, inst["agent_name"])


# 호환 dict — 기존 사용처 (step 17~22) 가 point-lookup 으로 _INSTANCES 의 필드를
# 가져오는 경로. 추후 helper 로 직접 교체해도 의미 동일.
_MODULES = _module_ids()
_TARBALL_PREFIX = {inst["id"]: inst["tarball"] for inst in _INSTANCES}
_DIR_NAME = {inst["id"]: inst["dir"] for inst in _INSTANCES}
_AGENT_SYNC_PORT_MOD = {inst["id"]: inst["sync_port"] for inst in _INSTANCES}
_AGENT_NAME_MOD = {inst["id"]: inst["agent_name"] for inst in _INSTANCES}
# 모든 인스턴스 listen != None (sim 은 _INSTANCES 에서 제거됨).
_LISTEN_PORTS = {inst["id"]: inst["listen"]
                 for inst in _INSTANCES if inst.get("listen")}


def _target(ctx: VerifyContext) -> str:
    return ((ctx.opts or {}).get("target") or "verify")


def _prepare_test_agent_slot(sync_port: int, name: str, state_dir: str) -> list:
    """spawn 직전 stale 상태를 정리. 반환=정리 노트 (detail 에 포함).

    pipeline-full 은 RESET 을 포함하지 않으므로 직전 회차의 test-agent 가
    sync_port 에 LISTEN 중일 수 있다. 또한 state_dir 에 이전 agent_id 가
    남아있으면 cims_agent.py 가 'resumed' 로 옛 ID 로 heartbeat → 새로 발급된
    deployment 가 다른 agent 에 매칭되어 install-poll 60s 타임아웃.

    1) `--name <name>` 패턴의 cims_agent.py 프로세스 종료 (pkill -f 동등)
    2) sync_port LISTEN 점유 프로세스 종료
    3) state_dir wipe — 새로 enroll 하도록 강제
    """
    notes: list = []

    # 1) 같은 --name 의 cims_agent 프로세스 정리 (pkill -f 동등)
    killed_name: list = []
    try:
        out = subprocess.run(
            ["pgrep", "-f", f"cims_agent.py.*--name {name}"],
            capture_output=True, text=True, timeout=5,
        ).stdout
        for line in out.split():
            try:
                pid = int(line)
                os.kill(pid, signal.SIGTERM)
                killed_name.append(pid)
            except Exception:
                pass
    except Exception:
        pass
    if killed_name:
        notes.append(f"  · stale agent (--name {name}) killed: pid={killed_name}")

    # 2) sync_port 점유 정리
    killed_port = _kill_listener_on_port(sync_port)
    if killed_port:
        notes.append(f"  · stale listener on :{sync_port} killed: pid={killed_port}")

    # 1)/2) 정리 후 짧은 grace
    if killed_name or killed_port:
        time.sleep(0.5)

    # 3) state_dir wipe — 'resumed: id=…' 로 옛 ID 인계 차단
    try:
        if os.path.isdir(state_dir):
            shutil.rmtree(state_dir, ignore_errors=True)
            notes.append(f"  · state_dir wiped: {state_dir}")
    except Exception:
        pass
    os.makedirs(state_dir, exist_ok=True)
    return notes


def _kill_listener_on_port(port: int) -> Optional[int]:
    """TCP 0.0.0.0:<port> LISTEN 프로세스 종료. 반환=죽인 pid (없으면 None)."""
    try:
        out = subprocess.run(
            ["ss", "-tlnp"], capture_output=True, text=True, timeout=5,
        ).stdout
    except Exception:
        return None
    pid_to_kill: Optional[int] = None
    for line in out.splitlines():
        # 정확히 :PORT 형태(끝 또는 공백)인지 — :990 패턴이 9903 등 매치 방지
        if not re.search(rf":{port}\b", line):
            continue
        m = re.search(r"pid=(\d+)", line)
        if not m:
            continue
        pid_to_kill = int(m.group(1))
        break
    if pid_to_kill is None:
        return None
    try:
        os.kill(pid_to_kill, signal.SIGTERM)
    except Exception:
        return pid_to_kill
    # SIGKILL fallback (2s 안에 안 죽으면)
    for _ in range(8):
        time.sleep(0.25)
        try:
            os.kill(pid_to_kill, 0)
        except ProcessLookupError:
            return pid_to_kill
        except Exception:
            return pid_to_kill
    try:
        os.kill(pid_to_kill, signal.SIGKILL)
    except Exception:
        pass
    return pid_to_kill


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

    # loopback alias 사전 확인 — _INSTANCES 의 분리 IP (PSP/PMP 등) 가 있으면
    # `ip addr add` 검증/추가. 모든 인스턴스가 127.0.0.1 인 (legacy) mode 는 no-op.
    # sudo -n 권한 없어도 cleanup PASS 유지 — step_21 LISTEN 검증 단계에서 명확
    # 실패하도록.
    from ...common import loopback as _lb
    alias_notes: list = []
    for ip in _lb.required_aliases(_INSTANCES):
        st_, msg = _lb.ensure_alias(ip)
        alias_notes.append(f"  · [{st_}] {msg}")
    if alias_notes:
        detail = (detail + "\n[loopback alias]\n" + "\n".join(alias_notes))

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
# Step 06 — Agent register (mgmt-server)
# ─────────────────────────────────────────────────────────────
def step_06_agent_register(ctx: VerifyContext) -> ItemResult:
    """Step 06 — mgmt-server agent 등록 + approve.

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
      env CIMS_AGENT_INSTALL_ROOT=build/dist/mgmt-server
          CIMS_AGENT_SYNC_PORT=9903
    enroll polling: cims_agent 테이블에서 name=mgmt-server + status='online'.
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
    ta_dir = os.path.join(dist, _DIST_ROOT_CSC, "agent")
    state_dir = os.path.join(ta_dir, "state")
    ta_log = os.path.join(ctx.repo_root, "logs", f"test-agent-{_AGENT_NAME_CSC}.log")
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

    # 직전 회차 잔존 정리 (stale agent 프로세스 + sync_port + state_dir).
    # pipeline-full 이 RESET 을 포함하지 않는 케이스를 명시적으로 흡수.
    cleanup_notes = _prepare_test_agent_slot(sync_port, aname, state_dir)

    # spawn
    env = dict(os.environ)
    env["CIMS_AGENT_INSTALL_ROOT"] = os.path.join(dist, _DIST_ROOT_CSC)
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
    detail = (f"pid={proc.pid} sync={sync_port} state-dir={state_dir} "
              f"(enroll {waited}s)")
    if cleanup_notes:
        detail += "\n" + "\n".join(cleanup_notes)
    result = ItemResult(
        id="S5-CSC-DEPLOY-AGENT-ENROLL-SPAWN", name="Test-agent 기동",
        status=ItemStatus.PASS,
        detail=detail,
        stage=5,
    )
    _save(ctx, 7, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 08 — Package upload (csc + console)
# ─────────────────────────────────────────────────────────────
# S5-CSC-DEPLOY 단계가 다루는 패키지 모듈 — mgmt-server 1 agent 가 csc + console + sim
# 모두 install. sim 은 install-only (Start 없음 — step_13/14 등은 csc/console 만 처리).
# tarball prefix 와 deployment process_name 매핑은 _CSC_PKG_TARBALL / _CSC_PKG_PROCESS.
_CSC_PACKAGES = ("csc", "console", "sim")
_CSC_PKG_TARBALL = {"csc": "csc", "console": "console", "sim": "cspsim"}
_CSC_PKG_PROCESS = {"csc": "CSC", "console": "CONSOLE", "sim": "CSPSIM"}


def step_08_package_upload(ctx: VerifyContext) -> ItemResult:
    """Step 08 — TB-CSC(4419) 에 csc + console + sim tarball 업로드.

    $DIST_DIR/packages/{csc,console,cspsim}-*.tar.gz 중 natural-sort 최고값 1개씩.
    POST /api/v1/packages (multipart, force=true) → package_id 추출.
    성공 시 ctx.state["pkg_id_csc"] / ["pkg_id_console"] / ["pkg_id_sim"] 저장.
    한 모듈이라도 tarball 없거나 업로드 실패면 FAIL.
    """
    if already_ran(ctx, 8):
        return get_native_result(ctx, 8)

    tok = _get(ctx, "tok", "")
    if not tok:
        result = ItemResult(
            id="S5-CSC-DEPLOY-PKG-UPLOAD", name="패키지 업로드 (csc + console + sim)",
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
        prefix = _CSC_PKG_TARBALL[name]
        tar = _latest_tarball(pkg_dir, prefix)
        if not tar:
            notes.append(f"- [FAIL] {name}: tarball 없음 ({pkg_dir}/{prefix}-*.tar.gz)")
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
        id="S5-CSC-DEPLOY-PKG-UPLOAD", name="패키지 업로드 (csc + console + sim)",
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
    """mgmt-server 자식 config overlay — target 의 포트 매핑. sim 은 overlay 없음."""
    if name == "csc":     return {"Server.Port": ports["csc"]}
    if name == "console": return {"Port": ports["console"]}
    return {}


def step_09_deployment_create(ctx: VerifyContext) -> ItemResult:
    """Step 09 — csc + console + sim deployment 생성 (config overlay 포함).

    install_path = $DIST_DIR/mgmt-server/{csc,console,sim}.
    process_name = CSC / CONSOLE / CSPSIM.
    config overlay 로 csc:Server.Port=4445, console:Port=8081 적용. sim 은 overlay 없음.
    성공 시 ctx.state["dep_id_csc"] / ["dep_id_console"] / ["dep_id_sim"] 저장.
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
        install_path = os.path.join(ctx.dist_dir, _DIST_ROOT_CSC, name)
        payload = {
            "agent_id":      int(aid),
            "package_id":    int(pkg_id),
            "install_path":  install_path,
            "process_name":  _CSC_PKG_PROCESS[name],
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
    """Step 11 — `$DIST_DIR/mgmt-server/{csc,console,sim}/` 안에 meta.json + config/
    디렉토리가 존재하는지 확인. install job 후 파일 배포 검증.

    이전 step 들 (08/09/10) 의 결과에 의존하지 않음 — install_path 만 기준으로
    파일 시스템 검증. 단독 실행 가능 (재실행 시에도 동일 결과). sim 은 cspsim
    tarball 의 구조상 config/ 가 없을 수 있어 meta.json 만 확인.
    """
    if already_ran(ctx, 11):
        return get_native_result(ctx, 11)

    notes: list = []
    ok_all = True
    for name in _CSC_PACKAGES:
        install_path = os.path.join(ctx.dist_dir, _DIST_ROOT_CSC, name)
        meta_p = os.path.join(install_path, "meta.json")
        cfg_d = os.path.join(install_path, "config")
        meta_ok = os.path.isfile(meta_p)
        # sim (cspsim tarball) 은 config/ 없을 수 있음 — meta.json 만 검사
        cfg_required = name != "sim"
        cfg_ok = (not cfg_required) or os.path.isdir(cfg_d)
        if meta_ok and cfg_ok:
            tag = "meta.json" + (" + config/" if cfg_required else "")
            notes.append(f"- [OK] {name}: {tag} 존재 ({install_path})")
        else:
            miss: list = []
            if not meta_ok: miss.append("meta.json")
            if cfg_required and not cfg_ok:  miss.append("config/")
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

    install_path = os.path.join(ctx.dist_dir, _DIST_ROOT_CSC, "csc")
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


def _wait_listen(port: int, proto: str, timeout_s: int, host: str = "") -> int:
    """`shell.port_listening` 폴링 (1초 단위). 도달하면 경과초, 실패면 -1.
    host 가 주어지면 host:port 정확 매칭 (0.0.0.0/* 도 허용)."""
    waited = 0
    while waited < timeout_s:
        if shell.port_listening(port, proto, host=host):
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
        # (배포본 mgmt-server 4445 는 step_13 의 start job 으로 신규 시작이라
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
# Step 17 — service-server 패키지 업로드 (배포본 csc 4445)
# ─────────────────────────────────────────────────────────────
def step_17_modules_pkg_upload(ctx: VerifyContext) -> ItemResult:
    """Step 17 — 4 service-server 모듈 (csp/psp/cmp/pmp) tarball 을
    배포본 csc(4445) 에 업로드. pkg2_id_{id} 캐시.
    """
    if already_ran(ctx, 17):
        return get_native_result(ctx, 17)

    tok2 = _get(ctx, "tok2", "")
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-DEPLOY-PKG-UPLOAD",
            name="service-server 패키지 업로드",
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
    for inst in _INSTANCES:
        m = inst["id"]
        prefix = inst["tarball"]
        tar = _latest_tarball(pkg_dir, prefix)
        if not tar:
            notes.append(f"- [FAIL] {inst['agent_name']}: tarball 없음 ({prefix}-*.tar.gz)")
            fail = True
            continue
        try:
            status, body = csc_http.post_multipart(
                f"{base}/api/v1/packages",
                file_path=tar, form_fields={"force": "true"},
                token=tok2, timeout=120,
            )
        except Exception as e:
            notes.append(f"- [FAIL] {inst['agent_name']}: 업로드 예외 {type(e).__name__}: {e}")
            fail = True
            continue
        if status not in (200, 201) or not isinstance(body, dict) or not body.get("id"):
            notes.append(f"- [FAIL] {inst['agent_name']}: status={status} body={str(body)[:120]}")
            fail = True
            continue
        pkg_id = int(body["id"])
        _set(ctx, f"pkg2_id_{m}", pkg_id)
        notes.append(f"- [OK] {inst['agent_name']}: package_id={pkg_id} ({os.path.basename(tar)})")

    result = ItemResult(
        id="S5-MODULES-DEPLOY-PKG-UPLOAD",
        name="service-server 패키지 업로드",
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
    error 반환. log 는 LOG_DIR/test-agent-<agent_name>.log.

    spawn 직전 stale agent/포트/state_dir 정리 (csc 와 동일 — pipeline-full 이
    RESET 미포함이라 직전 회차 test-agent 가 sync_port 점유 가능).
    """
    dist = ctx.dist_dir
    inst = _instance(m)
    sync_port = inst["sync_port"]
    install_root = _install_root(dist, inst)
    ta_dir = os.path.join(install_root, "agent")
    state_dir = os.path.join(ta_dir, "state")
    ta_log = os.path.join(ctx.repo_root, "logs", f"test-agent-{aname}.log")
    os.makedirs(os.path.dirname(ta_log), exist_ok=True)

    agent_py = os.path.join(dist, "agent", "cims_agent.py")
    if not os.path.isfile(agent_py):
        return (None, f"cims_agent.py 없음: {agent_py}")

    _prepare_test_agent_slot(sync_port, aname, state_dir)

    env = dict(os.environ)
    env["CIMS_AGENT_INSTALL_ROOT"] = install_root
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
    """모든 service-server agent (4 인스턴스) 가 cims_agent.status='online' 인지."""
    for inst in _INSTANCES:
        if not _agent_online_in_db(inst["agent_name"], dist_dir):
            return False
    return True


def step_18_modules_agent_enroll(ctx: VerifyContext) -> ItemResult:
    """Step 18 — service-server agent register + Test-agent spawn + enroll 폴링 20s.

    개별 모듈 등록/spawn 이 실패해도 다른 모듈은 진행. 마지막에 모두 online 이어야 PASS.
    P1: 4 인스턴스 (volte-sip-server / volte-media-server / ptt-sip-server / ptt-media-server).
    """
    if already_ran(ctx, 18):
        return get_native_result(ctx, 18)

    tok2 = _get(ctx, "tok2", "")
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-DEPLOY-AGENT-ENROLL",
            name="service-server agent + Test-agent",
            status=ItemStatus.SKIP,
            detail="step 16 미실행 — tok2 없음", stage=5,
        )
        _save(ctx, 18, result)
        return result

    base = _deployed_csc_base(ctx)
    notes: list = []
    register_fail = False
    for inst in _INSTANCES:
        m = inst["id"]
        aname = inst["agent_name"]
        aid, enroll_tok, err = _register_one_module_agent(base, tok2, aname)
        if err:
            notes.append(f"- [FAIL] {aname}: {err}")
            register_fail = True
            continue
        _set(ctx, f"aid_{m}", aid)
        _set(ctx, f"enroll_tok_{m}", enroll_tok)
        pid, perr = _spawn_one_module_agent(ctx, m, base, aname, enroll_tok)
        if perr:
            notes.append(f"- [FAIL] {aname}: spawn {perr}")
            register_fail = True
            continue
        _set(ctx, f"ta_pid_{m}", pid)
        notes.append(
            f"- [OK] {aname}: aid={aid} pid={pid} sync={inst['sync_port']}"
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
        name="service-server agent + Test-agent",
        status=status, detail="\n".join(notes), stage=5,
    )
    _save(ctx, 18, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 19 — service-server deployment 생성 (csp/psp/cmp/pmp)
# ─────────────────────────────────────────────────────────────
def step_19_modules_deployment_create(ctx: VerifyContext) -> ItemResult:
    """Step 19 — 4 service-server deployment 생성. config overlay 포함.
    dep2_id_{id} 캐시. install_path = $DIST_DIR/<agent_name>/<dir>.
    """
    if already_ran(ctx, 19):
        return get_native_result(ctx, 19)

    tok2 = _get(ctx, "tok2", "")
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-DEPLOY-INSTALL-CREATE", name="service-server deployment 생성",
            status=ItemStatus.SKIP,
            detail="step 16 미실행 — tok2 없음", stage=5,
        )
        _save(ctx, 19, result)
        return result

    base = _deployed_csc_base(ctx)
    notes: list = []
    fail = False
    for inst in _INSTANCES:
        m = inst["id"]
        aid = _get(ctx, f"aid_{m}")
        pkg_id = _get(ctx, f"pkg2_id_{m}")
        if aid is None or pkg_id is None:
            notes.append(f"- [SKIP] {inst['agent_name']}: aid/pkg_id 미확보 (step 17/18 실패)")
            fail = True
            continue
        install_path = _install_path(ctx.dist_dir, inst)
        pname = inst["process"]
        payload = {
            "agent_id":     int(aid),
            "package_id":   int(pkg_id),
            "install_path": install_path,
            "process_name": pname,
        }
        overlay = inst.get("config_overlay") or {}
        if overlay:
            payload["config"] = overlay
        try:
            st, body = csc_http.post_json(
                f"{base}/api/v1/deployments", payload, token=tok2, timeout=15,
            )
        except Exception as e:
            notes.append(f"- [FAIL] {inst['agent_name']}: 호출 예외 {type(e).__name__}: {e}")
            fail = True
            continue
        if st not in (200, 201) or not isinstance(body, dict) or not body.get("id"):
            notes.append(f"- [FAIL] {inst['agent_name']}: status={st} body={str(body)[:120]}")
            fail = True
            continue
        did = int(body["id"])
        _set(ctx, f"dep2_id_{m}", did)
        notes.append(f"- [OK] {inst['agent_name']}: deployment_id={did} → {install_path} (process={pname})")

    result = ItemResult(
        id="S5-MODULES-DEPLOY-INSTALL-CREATE", name="service-server deployment 생성",
        status=ItemStatus.FAIL if fail else ItemStatus.PASS,
        detail="\n".join(notes), stage=5,
    )
    _save(ctx, 19, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 20 — 3 install jobs + DB 폴링
# ─────────────────────────────────────────────────────────────
def step_20_modules_install_poll(ctx: VerifyContext) -> ItemResult:
    """Step 20 — service-server install jobs 발행 + agent_deployment 상태 폴링 60s."""
    if already_ran(ctx, 20):
        return get_native_result(ctx, 20)

    tok2 = _get(ctx, "tok2", "")
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-DEPLOY-INSTALL-POLL", name="service-server install + 폴링",
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
            id="S5-MODULES-DEPLOY-INSTALL-POLL", name="service-server install + 폴링",
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
        id="S5-MODULES-DEPLOY-INSTALL-POLL", name="service-server install + 폴링",
        status=overall, detail=summary, stage=5,
    )
    _save(ctx, 20, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 21 — csp/cmp Start + LISTEN (sim install-only)
# ─────────────────────────────────────────────────────────────
def step_21_modules_start(ctx: VerifyContext) -> ItemResult:
    """Step 21 — 4 service-server Start (csp/psp 5060/udp + cmp/pmp 9000/udp)."""
    if already_ran(ctx, 21):
        return get_native_result(ctx, 21)

    tok2 = _get(ctx, "tok2", "")
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-RUN-START", name="service-server Start",
            status=ItemStatus.SKIP,
            detail="step 16 미실행 — tok2 없음", stage=5,
        )
        _save(ctx, 21, result)
        return result

    base = _deployed_csc_base(ctx)
    notes: list = []
    started: list = []  # (id, agent_name, port, proto, host)
    fail = False
    for inst in _INSTANCES:
        listen = inst.get("listen")
        if not listen:
            continue
        m = inst["id"]; aname = inst["agent_name"]
        port, proto = listen; host = inst.get("local_ip", "")
        did = _get(ctx, f"dep2_id_{m}")
        if did is None:
            notes.append(f"- [FAIL] {aname}: dep2_id 없음 (step 19 실패)")
            fail = True; continue
        try:
            st, _ = csc_http.post_json(
                f"{base}/api/v1/deployments/{did}/job",
                {"job_type": "start"}, token=tok2, timeout=10,
            )
        except Exception as e:
            notes.append(f"- [FAIL] {aname}: start 발행 예외 {e}")
            fail = True; continue
        if st not in (200, 201, 202):
            notes.append(f"- [FAIL] {aname}: start 발행 status={st}")
            fail = True; continue
        started.append((m, aname, port, proto, host))

    for m, aname, port, proto, host in started:
        waited = _wait_listen(port, proto, 20, host=host)
        host_label = f"{host}:" if host else ""
        if waited >= 0:
            notes.append(f"- [OK] {aname}: {host_label}{port}/{proto} LISTEN ({waited}s)")
        else:
            notes.append(f"- [FAIL] {aname}: {host_label}{port}/{proto} LISTEN 실패 (20s)")
            fail = True

    _set(ctx, "modules_start_ok", not fail)

    # csp↔cmp + psp↔pmp control connection 안정화 대기 (default max 150s).
    # 시그널링 시작 직후 미디어와의 첫 heartbeat 응답까지 ~120s 소요. 이 wait 가
    # 없으면 후속 S6 PTT 시나리오의 InviteMember 가 'Failed to get/alloc
    # Shared Port' 로 실패. `CIMS_VERIFY_CMP_WAIT_S=0` 으로 wait 자체 비활성 (unit test).
    cmp_wait_s = int(os.environ.get("CIMS_VERIFY_CMP_WAIT_S", "150"))
    if not fail and cmp_wait_s > 0:
        import time as _t
        from glob import glob as _glob
        # csp/psp 시그널링 측 인스턴스 기준 — 자기 install_path 의 csp_*.log 에
        # OnCmpStatusChanged: Connected 가 찍혀야 짝 미디어 (cmp/pmp) 와 연결 OK.
        sig_pairs = [(inst, inst.get("peer_id"))
                     for inst in _INSTANCES
                     if inst.get("listen") and inst["dir"] in ("csp", "psp")]
        deadline = _t.time() + cmp_wait_s
        connected: dict = {inst["id"]: False for inst, _ in sig_pairs}
        while _t.time() < deadline:
            for inst, peer in sig_pairs:
                if connected[inst["id"]]:
                    continue
                # tarball 안 디렉토리 = inst["dir"] (csp/psp/isp 등 변종 이름).
                # 로그 파일명 prefix 는 csp ELF 가 그대로 csp_*.log 로 기록 (binary
                # 이름이 변종으로 rename 되어도 PalogLogger 의 prefix 가 코드 상수).
                log_glob = os.path.join(
                    _install_path(ctx.dist_dir, inst), inst["dir"], "log", "csp_*.log",
                )
                for p in _glob(log_glob):
                    try:
                        with open(p, "rb") as f:
                            if b"OnCmpStatusChanged: Connected" in f.read():
                                connected[inst["id"]] = True
                                break
                    except OSError:
                        pass
            if all(connected.values()):
                break
            _t.sleep(3)
        for inst, peer in sig_pairs:
            tag = "CONNECTED" if connected[inst["id"]] else "TIMEOUT"
            peer_inst = _instance(peer) if peer else None
            peer_label = peer_inst["agent_name"] if peer_inst else "(no peer)"
            notes.append(f"- {inst['agent_name']} ↔ {peer_label} control connection: {tag}")

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
        id="S5-MODULES-RUN-START", name="service-server Start",
        status=ItemStatus.FAIL if fail else ItemStatus.PASS,
        detail="\n".join(notes), stage=5,
    )
    _save(ctx, 21, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 22 — Stop / 기동 유지 (--stop-after)
# ─────────────────────────────────────────────────────────────
def step_22_finalize(ctx: VerifyContext) -> ItemResult:
    """Step 22 — `stop_after=True` 면 모든 deployment stop + Test-agent kill.
    기본 (False) 은 정보성 — 모든 ports 기동 유지 (Phase 3 진입 가능).
    """
    if already_ran(ctx, 22):
        return get_native_result(ctx, 22)

    if not ctx.stop_after:
        listen_lines = [
            f"- {inst['agent_name']} ({inst['display_name']}): "
            f"{inst['local_ip']}:{inst['listen'][0]}/{inst['listen'][1]}"
            for inst in _INSTANCES if inst.get("listen")
        ]
        notes = [
            "- 전체 기동 유지 (기본)",
            "- mgmt-server: csc(4445) · console(8081) · sim (install-only)",
            *listen_lines,
            "- Test-agent (1 mgmt + service-server agents) heartbeat 유지",
        ]
        result = ItemResult(
            id="S5-FINALIZE", name="배포 마무리 (기동 유지)",
            status=ItemStatus.PASS, detail="\n".join(notes), stage=5,
        )
        _save(ctx, 22, result)
        return result

    # --stop-after: 모든 deployment stop + Test-agent kill
    notes: list = []
    # mgmt-server modules stop (TB-CSC 4419 경유) — csc/console/sim
    tok = _get(ctx, "tok", "")
    if tok:
        for k in _CSC_PACKAGES:
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
    # service-server stop (배포본 csc 4445 경유)
    tok2 = _get(ctx, "tok2", "")
    if tok2:
        for inst in _INSTANCES:
            m = inst["id"]
            did = _get(ctx, f"dep2_id_{m}")
            if did is None: continue
            try:
                st, _ = csc_http.post_json(
                    f"{_deployed_csc_base(ctx)}/api/v1/deployments/{did}/job",
                    {"job_type": "stop"}, token=tok2, timeout=10,
                )
                notes.append(f"- {inst['agent_name']}: stop 발행 status={st}")
            except Exception as e:
                notes.append(f"- {inst['agent_name']}: stop 발행 예외 {e}")
    time.sleep(5)

    # Test-agent kill — mgmt-server + service-server agents
    import signal
    killed = 0
    total = 1 + len(_INSTANCES)
    pid_keys = [("ta_pid_csc", _AGENT_NAME_CSC)]
    pid_keys += [(f"ta_pid_{inst['id']}", inst["agent_name"]) for inst in _INSTANCES]
    for k, _aname in pid_keys:
        pid = _get(ctx, k)
        if not pid: continue
        try:
            os.kill(int(pid), signal.SIGTERM)
            killed += 1
        except Exception:
            pass
    notes.append(f"- Test-agent kill: {killed}/{total}")

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
