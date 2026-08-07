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
    # mgmt 체인 (TB-OAM 4419 → oam + sim 부트스트랩)
    "tok", "aid_csc", "enroll_tok_csc", "ta_pid_csc",       # 05~07
    "pkg_id_oam", "pkg_id_sim",                              # 08
    "dep_id_oam", "dep_id_sim",                              # 09
    "all_install_done_csc",                                  # 10
    "mgmt_start_ok", "mgmt_health_ok", "console_static_ok",  # 13~15
    # modules 체인 (배포본 OAM 4445 — csc 서비스 모듈 + 4 service-server)
    "tok2", "aid2_<id>", "enroll_tok2_<id>",                 # 16, 18
    "ta_pid2_<id>",                                          # 18
    "pkg2_id_csp/cmp/...",                                   # 17
    "dep2_id_csp/cmp/...",                                   # 19
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
from ...common import db as _dbmod   # dev 환경 DB 설정 읽기 (제어평면 조회 아님)


def _natural_key(s: str) -> list:
    """Natural sort key — '1.10' > '1.9' (숫자 청크는 int 비교)."""
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", s)]


def _latest_tarball(pkg_dir: str, prefix: str) -> Optional[str]:
    """$pkg_dir/<prefix>-<ver>.tar.gz 중 natural sort 최고값 (sort -V tail -1 동등).

    `<prefix>-` 바로 뒤가 버전 숫자인 것만 매칭 — 단순 glob 은 형제 컴포넌트를
    삼킨다 (oam-*.tar.gz 가 oam-svc-*, csp-*.tar.gz 가 cspsim-* 을 매칭)."""
    pat = re.compile(rf"^{re.escape(prefix)}-\d")
    cands = [c for c in glob.glob(os.path.join(pkg_dir, f"{prefix}-*.tar.gz"))
             if pat.match(os.path.basename(c))]
    if not cands:
        return None
    return sorted(cands, key=_natural_key)[-1]


_STATE_KEY = "_s5_native"


# TB-CSC 접속 기본값 — env 로 override 가능
_TB_CSC_BASE = "https://127.0.0.1:4419"
_AGENT_NAME_CSC = "mgmt-server"
_DIST_ROOT_CSC = "mgmt-server"
_AGENT_SYNC_PORT_CSC = 9903

# 배포본 관리평면(OAM) — verify 4445 / prod 4419. 콘솔은 OAM 정적 서빙(단일
# 오리진)이라 별도 포트가 없다. csp/cmp 는 두 환경 동일 (5060/9000).
# 포트 SoT 는 csc_http.DEPLOYED_MGMT_PORTS / DEPLOYED_CSC_PORTS.
_TARGET_PORTS = {
    "verify": {"mgmt": csc_http.deployed_mgmt_port("verify")},
    "prod":   {"mgmt": csc_http.deployed_mgmt_port("prod")},
}

# Instance descriptor — 한 entry = 한 service-server 인스턴스. tarball/install/process/
# sync_port/local_ip/listen 의 SoT. step 17~21 + finalize 가 이 list 만 보고
# 동작 — 인스턴스 추가/제거가 자연 일반화. ISP/IMP 는 P2 에서 entry 추가.
#
# 사용자 토폴로지 매핑 (한 server agent 에 여러 인스턴스 공존):
#   VoLTE SIP Server   = CSP + ISP   agent=volte-sip-server   sub-dir={csp,isp}
#   VoLTE Media Server = CMP + IMP   agent=volte-media-server sub-dir={cmp,imp}
#   PTT SIP Server     = PSP         agent=ptt-sip-server     sub-dir=psp
#   PTT Media Server   = PMP         agent=ptt-media-server   sub-dir=pmp
#
# 동일 agent_name 의 변종은 1개 cims_agent 프로세스 + 1개 enroll + sync_port 공유
# (step 18 에서 agent_name 기준 dedup). 각 변종은 별도 deployment + start.
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
    # csc — mgmt 호스트의 서비스 모듈 (oam_csc_split). 배포본 OAM(게이트웨이) 이
    # install 성공 시 JwtSecret 자동 주입 + 가입자/조직 라우트를 self-register
    # 하므로, S6 admin CRUD 는 mgmt 포트(4445) 하나로 게이트웨이 경유 도달한다.
    # 포트 4446 = dev Test-CSC(4421)·mgmt OAM(4445) 과 분리 (SoT: csc_http).
    {"id": "csc",
     "display_name": "Mgmt Service (CSC)",
     "agent_name":   "mgmt-svc-server",
     "tarball": "csc", "dir": "csc", "process": "CSC",
     "sync_port": 9909, "local_ip": "127.0.0.1",
     "listen": (csc_http.deployed_csc_port("verify"), "tcp"),
     "peer_id": None,
     "config_overlay": {
         "Server.Port": csc_http.deployed_csc_port("verify"),
         # dev Test-CSC 의 MCPTT(4430) 와 bind 충돌 회피 — 배포본 스택은 4431.
         # csp/psp 변종의 Setup.Xcap.Port 와 동기 (아래 _INSTANCES overlay).
         "McpttServer.Port": 4431,
         # 그룹/가입자 변경 notify 대상 = 배포본 CSP/PSP (dev 스택 아님).
         "CspNotify.Ip": "127.0.0.1",
         "PspNotify.Ip": "127.0.0.3",
     }},
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
         # XCAP(GMS/CMS) 대상 = 배포본 csc (mgmt 호스트 127.0.0.1:4431).
         "Setup.Xcap.Host": "127.0.0.1",
         "Setup.Xcap.Port": 4431,
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
         # XCAP(GMS/CMS) 대상 = 배포본 csc (mgmt 호스트 127.0.0.1:4431).
         "Setup.Xcap.Host": "127.0.0.1",
         "Setup.Xcap.Port": 4431,
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
    # P2 — IBCF 트렁크. ISP/IMP 는 VoLTE SIP/Media Server 와 같은 agent 에 공존
    #   (`volte-sip-server/{csp,isp}/`, `volte-media-server/{cmp,imp}/`).
    # ISP 는 IBCF role 단독 (CSCF/TAS/PTT_AS=false).
    # bind IP 는 127.0.0.5 — CSP/CMP(127.0.0.1) 와 port 충돌 회피용 loopback alias
    # (`sudo ip addr add 127.0.0.5/8 dev lo` 1회 필요).
    {"id": "isp",
     "display_name": "VoLTE SIP Server (IBCF)",
     "agent_name":   "volte-sip-server",
     "tarball": "isp", "dir": "isp", "process": "ISP",
     "sync_port": 9904, "local_ip": "127.0.0.5", "listen": (5060, "udp"),
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
     "display_name": "VoLTE Media Server (IBCF)",
     "agent_name":   "volte-media-server",
     "tarball": "imp", "dir": "imp", "process": "IMP",
     "sync_port": 9905, "local_ip": "127.0.0.5", "listen": (9000, "udp"),
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
    """target → {mgmt:int}. 알 수 없는 target 은 verify default."""
    return _TARGET_PORTS.get(_target(ctx), _TARGET_PORTS["verify"])


def _deployed_mgmt_base(ctx: VerifyContext) -> str:
    """배포본 관리평면(OAM) API URL — target 의 mgmt 포트로."""
    return f"https://127.0.0.1:{_ports(ctx)['mgmt']}"


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
      - DB: agent_deployment/_job/_metric + cims_agent 모두 TRUNCATE
    --keep-processes: TB-CSC(4419) / TB-Console(3000) 보존.
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
            shell.run_cims_svc(ctx.repo_root, "restart", "tb-csc", timeout=20)
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
def _agent_online(base: str, token: str, name: str) -> bool:
    """GET /agents 에서 name 의 status='online' 확인. 조회 실패 시 False.

    제어평면 SoT 는 runtime store(file_store) 라 DB 테이블로는 알 수 없다
    (db_schema.md — agent/deployment/job 은 OAM file_store 소유). API 가 정본."""
    if not token:
        return False
    for a in csc_http.list_agents(base, token):
        if isinstance(a, dict) and a.get("name") == name:
            return str(a.get("status") or "") == "online"
    return False


def step_07_testagent_spawn(ctx: VerifyContext) -> ItemResult:
    """Step 07 — Test-agent 기동 (sync 9903) + enroll 대기 (15s).

    cims_agent.py 를 nohup 으로 spawn:
      env CIMS_AGENT_INSTALL_ROOT=build/dist/mgmt-server
          CIMS_AGENT_SYNC_PORT=9903
    enroll polling: GET /agents 에서 name=mgmt-server + status='online'.
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
    # verify Test-agent 는 감독(watchdog) 금지 — _PREFIX(build/dist)/run 의 dev
    # pid 파일을 감독 집합에 seed 해 dev 스택(oam 등)을 죽이고/재시작한다.
    env["CIMS_AGENT_NO_SUPERVISE"] = "1"
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
        if _agent_online(_TB_CSC_BASE, _get(ctx, "tok", ""), aname):
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
# S5-CSC-DEPLOY 단계가 다루는 패키지 모듈 — mgmt-server 1 agent 가 oam + sim install.
# oam 이 배포본 스택의 관리평면(standalone OAM, 02_deployment §2.1) — 콘솔 SPA 를
# 정적 동봉·단일 오리진 서빙하므로 console 단독 배포는 없다. csc(서비스 모듈)는
# 배포본 OAM 경유의 modules 체인(_INSTANCES)에서 배포된다 (oam_csc_split).
# sim 은 install-only (Start 없음).
_CSC_PACKAGES = ("oam", "sim")
_CSC_PKG_TARBALL = {"oam": "oam", "sim": "cspsim"}
_CSC_PKG_PROCESS = {"oam": "OAM", "sim": "CSPSIM"}


def step_08_package_upload(ctx: VerifyContext) -> ItemResult:
    """Step 08 — TB-OAM(4419) 에 oam + sim tarball 업로드.

    $DIST_DIR/packages/{oam,cspsim}-*.tar.gz 중 natural-sort 최고값 1개씩.
    POST /api/v1/packages (multipart, force=true) → package_id 추출.
    성공 시 ctx.state["pkg_id_oam"] / ["pkg_id_sim"] 저장.
    한 모듈이라도 tarball 없거나 업로드 실패면 FAIL.
    """
    if already_ran(ctx, 8):
        return get_native_result(ctx, 8)

    tok = _get(ctx, "tok", "")
    if not tok:
        result = ItemResult(
            id="S5-CSC-DEPLOY-PKG-UPLOAD", name="패키지 업로드 (oam + sim)",
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
        id="S5-CSC-DEPLOY-PKG-UPLOAD", name="패키지 업로드 (oam + sim)",
        status=ItemStatus.FAIL if fail else ItemStatus.PASS,
        detail="\n".join(notes) if notes else "no packages",
        stage=5,
    )
    _save(ctx, 8, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 09 — Deployment 생성 (config overlay)
# ─────────────────────────────────────────────────────────────
def _mgmt_overlay(name: str, ports: dict, mgmt_root: str) -> dict:
    """mgmt-server 자식 config overlay. sim 은 overlay 없음.

    oam 은 포트에 더해 runtime store / 패키지 저장소 / 서비스 로그를 자기
    모듈 트리로 격리한다 — 패키징된 oam.json 은 빌드 서버(dev) 경로를 담고
    있어, overlay 없이 기동하면 TB 와 file_store 를 공유하는 사고가 난다."""
    if name == "oam":
        oam_root = os.path.join(mgmt_root, "oam")
        return {
            "Server.Port":        ports["mgmt"],
            "CimsRuntimeDir":     os.path.join(oam_root, "runtime"),
            "Packages.Dir":       os.path.join(oam_root, "packages"),
            "ServiceLogging.Dir": os.path.join(oam_root, "service_log"),
        }
    return {}


def step_09_deployment_create(ctx: VerifyContext) -> ItemResult:
    """Step 09 — oam + sim deployment 생성 (config overlay 포함).

    install_path = $DIST_DIR/mgmt-server/{oam,sim}.
    process_name = OAM / CSPSIM.
    config overlay 로 oam:Server.Port=4445 + runtime/packages/log 격리. sim 은 없음.
    성공 시 ctx.state["dep_id_oam"] / ["dep_id_sim"] 저장.
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
            "config":        _mgmt_overlay(
                name, ports, os.path.join(ctx.dist_dir, _DIST_ROOT_CSC)),
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
# Step 10 — Install job + deployment 상태 폴링
# ─────────────────────────────────────────────────────────────
def _deployment_status(base: str, token: str, did: int) -> Optional[str]:
    """GET /deployments 에서 did 의 status. 조회 실패 시 None.
    (제어평면 SoT = OAM runtime store — DB 테이블이 아니라 API 가 정본)"""
    try:
        d = csc_http.get_json(f"{base}/api/v1/deployments", token=token, timeout=10)
    except Exception:
        return None
    items = d.get("items") if isinstance(d, dict) else d
    for it in (items or []):
        if isinstance(it, dict) and it.get("id") == did:
            st = it.get("status")
            return str(st) if st is not None else None
    return None


def step_10_install_poll(ctx: VerifyContext) -> ItemResult:
    """Step 10 — install job 발행 + deployment 상태 폴링 (최대 60s).

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
    # deployment status: pending|deploying|running|stopped|failed|removed
    # 폴링 종료 조건: pending/deploying 가 사라질 때 (install 완료 또는 실패)
    elapsed = 0
    all_done = False
    final_status: dict = {}
    for _ in range(30):
        time.sleep(2); elapsed += 2
        still = False
        for name, did in deployments:
            st = _deployment_status(_TB_CSC_BASE, tok, did)
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
def _resolve_install_root(base: str) -> str:
    """배포 산출물의 실제 루트(meta.json 이 있는 디렉토리)를 찾는다.

    agent 는 `<base>/<version>/` 에 풀고 `<base>/current` 심볼릭을 건다(업그레이드·
    롤백 통로). 모듈에 따라 `<base>/modules/<pkg>/current` 로 한 겹 더 들어간다.
    구(평탄) 설치본은 `<base>` 자체가 루트 — 셋 다 흡수한다."""
    cur = os.path.join(base, "current")
    if os.path.isdir(cur):
        return cur
    if os.path.isfile(os.path.join(base, "meta.json")):
        return base
    # `<base>/modules/<pkg>/current` (install-only 모듈 등)
    for sub in sorted(glob.glob(os.path.join(base, "modules", "*", "current"))):
        if os.path.isdir(sub):
            return sub
    # 마지막 수단 — meta.json 을 품은 하위 디렉토리 (버전 디렉토리 직접 탐색)
    for meta in sorted(glob.glob(os.path.join(base, "*", "meta.json"))
                       + glob.glob(os.path.join(base, "modules", "*", "*", "meta.json"))):
        return os.path.dirname(meta)
    return base


def step_11_verify_files(ctx: VerifyContext) -> ItemResult:
    """Step 11 — `$DIST_DIR/mgmt-server/{oam,sim}/` 안에 meta.json + config/
    디렉토리가 존재하는지 확인. install job 후 파일 배포 검증.

    이전 step 들 (08/09/10) 의 결과에 의존하지 않음 — install_path 만 기준으로
    파일 시스템 검증. 단독 실행 가능 (재실행 시에도 동일 결과). sim 은 cspsim
    tarball 의 구조상 config/ 가 없을 수 있어 meta.json 만 확인.
    """
    if already_ran(ctx, 11):
        return get_native_result(ctx, 11)

    notes: list = []
    ok_all = True
    # _CSC_PACKAGES = {"oam", "sim"} — agent_name=mgmt-server.
    # cims_agent 가 install 시 tarball top-level 디렉토리 (oam/cspsim) 안에
    # config.json/config/ 를 쓰므로 검증 경로도 변종별 위치를 본다. sim 은
    # cspsim/* 구조 (tarball root="cspsim").
    _TAR_PKG_NAME = {"oam": "oam", "sim": "cspsim"}
    for name in _CSC_PACKAGES:
        install_path = _resolve_install_root(
            os.path.join(ctx.dist_dir, _DIST_ROOT_CSC, name))
        pkg_subdir = _TAR_PKG_NAME[name]
        meta_p = os.path.join(install_path, "meta.json")
        # config/ 는 변종 디렉토리 내부 (cfg_target_dir) 에 위치
        cfg_d_scoped = os.path.join(install_path, pkg_subdir, "config")
        cfg_d_legacy = os.path.join(install_path, "config")
        meta_ok = os.path.isfile(meta_p)
        # sim (cspsim tarball) 은 config/ 없을 수 있음 — meta.json 만 검사
        cfg_required = name != "sim"
        cfg_ok = (not cfg_required) or os.path.isdir(cfg_d_scoped) or os.path.isdir(cfg_d_legacy)
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
# Step 12 — config overlay 반영 검증 (oam/config.json Server.Port=4445)
# ─────────────────────────────────────────────────────────────
def _read_mgmt_port(install_path: str) -> Optional[int]:
    """`<install_path>/oam/config.json` 또는 `<install_path>/config.json` 에서
    `Server.Port` (flat 또는 nested Server.Port) 추출. cims_agent 가 멀티-변종
    install 지원으로 config.json 을 변종 디렉토리 안에 쓸 수 있어 두 위치를 확인.
    파일/JSON 오류 시 None.
    """
    for p in (os.path.join(install_path, "oam", "config.json"),
              os.path.join(install_path, "config.json")):
        try:
            with open(p) as f:
                d = json.load(f)
        except Exception:
            continue
        if not isinstance(d, dict):
            continue
        # 1) flat key "Server.Port"
        val = d.get("Server.Port")
        if val is None:
            # 2) nested {"Server": {"Port": ...}}
            srv = d.get("Server")
            if isinstance(srv, dict):
                val = srv.get("Port")
        if val is None:
            continue
        try:
            return int(val)
        except (TypeError, ValueError):
            continue
    return None


def step_12_verify_overlay(ctx: VerifyContext) -> ItemResult:
    """Step 12 — oam/config.json overlay 반영 (target 의 mgmt 포트 매칭)."""
    if already_ran(ctx, 12):
        return get_native_result(ctx, 12)

    install_path = _resolve_install_root(
        os.path.join(ctx.dist_dir, _DIST_ROOT_CSC, "oam"))
    port = _read_mgmt_port(install_path)
    expected = _ports(ctx)["mgmt"]
    if port == expected:
        result = ItemResult(
            id="S5-CSC-VERIFY-OVERLAY", name="config overlay 반영",
            status=ItemStatus.PASS,
            detail=f"- [OK] oam/config.json: Server.Port={expected} 반영 ({install_path})",
            stage=5,
        )
    else:
        result = ItemResult(
            id="S5-CSC-VERIFY-OVERLAY", name="config overlay 반영",
            status=ItemStatus.FAIL,
            detail=f"- [FAIL] oam/config.json Server.Port 이상 (실제={port}, 기대={expected}, "
                   f"path={install_path}/config.json)",
            stage=5,
        )
    _save(ctx, 12, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 13 — oam Start + 포트 4445 LISTEN
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
    """Step 13 — oam Start job 발행 + LISTEN 대기 (25s, target 의 mgmt 포트).

    `opts.enable_mtls` true 시 시작 직전 csc-tb.json `Agent.MtlsEnabled`
    를 true 로 토글. 후속 신규 enroll agent (csp/cmp/sim) 가 mTLS cert 발급
    받아 S6-SCN-CERT-ROTATE 가 PASS 가능.
    """
    if already_ran(ctx, 13):
        return get_native_result(ctx, 13)

    if (ctx.opts or {}).get("enable_mtls"):
        from ...common.csc_config import set_mtls_enabled
        toggled = set_mtls_enabled(ctx.dist_dir, True)
        _set(ctx, "mtls_toggled", bool(toggled))
        # 주의: TB-OAM (4419) 가 이미 LISTEN 중이라면 캐시 가능성 — 효과 보장은
        # 사전 `cims.sh restart oam` 1회. (배포본 4445 는 신규 시작이라 자동 반영.)

    tok = _get(ctx, "tok", "")
    oam_did = _get(ctx, "dep_id_oam")
    mgmt_port = _ports(ctx)["mgmt"]
    name = f"oam Start ({mgmt_port} LISTEN)"
    if not tok or oam_did is None:
        result = ItemResult(
            id="S5-CSC-RUN-CSC-START", name=name,
            status=ItemStatus.SKIP,
            detail="step 05/09 미실행 — tok/dep_id 없음",
            stage=5,
        )
        _save(ctx, 13, result)
        return result

    status, _ = _post_job(ctx, int(oam_did), "start", timeout=10)
    if status not in (200, 201, 202):
        result = ItemResult(
            id="S5-CSC-RUN-CSC-START", name=name,
            status=ItemStatus.FAIL,
            detail=f"start job 발행 실패 status={status}",
            stage=5,
        )
        _save(ctx, 13, result)
        return result

    waited = _wait_listen(mgmt_port, "tcp", 25)
    if waited >= 0:
        _set(ctx, "mgmt_start_ok", True)
        result = ItemResult(
            id="S5-CSC-RUN-CSC-START", name=name,
            status=ItemStatus.PASS,
            detail=f"- [OK] oam port {mgmt_port} LISTEN ({waited}s)",
            stage=5,
        )
    else:
        _set(ctx, "mgmt_start_ok", False)
        result = ItemResult(
            id="S5-CSC-RUN-CSC-START", name=name,
            status=ItemStatus.FAIL,
            detail=f"- [FAIL] oam port {mgmt_port} LISTEN 실패 (25s timeout)",
            stage=5,
        )
    _save(ctx, 13, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 14 — oam Health check job
# ─────────────────────────────────────────────────────────────
def _agent_job_status(base: str, token: str, aid: int,
                      job_id: int) -> Optional[tuple]:
    """GET /agents/<aid>/jobs/<jid> → (status, result_code, result_stdout).
    조회 실패 시 None. (job SoT = OAM runtime store — API 가 정본)"""
    try:
        d = csc_http.get_json(
            f"{base}/api/v1/agents/{aid}/jobs/{job_id}", token=token, timeout=10,
        )
    except Exception:
        return None
    if not isinstance(d, dict):
        return None
    return (str(d.get("status") or ""), d.get("result_code"),
            str(d.get("result_stdout") or ""))


def step_14_csc_health(ctx: VerifyContext) -> ItemResult:
    """Step 14 — csc Health check job 발행 + job 상태 폴링 (15s).

    PASS 조건: status=succeeded, result_code=0, result_stdout 안 'tcp:<mgmt>=open'.
    """
    if already_ran(ctx, 14):
        return get_native_result(ctx, 14)

    tok = _get(ctx, "tok", "")
    oam_did = _get(ctx, "dep_id_oam")
    if not tok or oam_did is None or not _get(ctx, "mgmt_start_ok"):
        result = ItemResult(
            id="S5-CSC-RUN-CSC-HEALTH", name="oam Health check",
            status=ItemStatus.SKIP,
            detail="step 13 (oam Start) 미실행 / 실패 — health 발행 의미 없음",
            stage=5,
        )
        _save(ctx, 14, result)
        return result

    status, body = _post_job(ctx, int(oam_did), "health_check", timeout=10)
    if status not in (200, 201, 202) or not isinstance(body, dict):
        result = ItemResult(
            id="S5-CSC-RUN-CSC-HEALTH", name="oam Health check",
            status=ItemStatus.FAIL,
            detail=f"health_check 발행 실패 status={status}",
            stage=5,
        )
        _save(ctx, 14, result)
        return result

    job_id = body.get("job_id")
    if job_id is None:
        result = ItemResult(
            id="S5-CSC-RUN-CSC-HEALTH", name="oam Health check",
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
        row = _agent_job_status(
            _TB_CSC_BASE, tok, int(_get(ctx, "aid_csc") or 0), job_id)
        if row and row[0] in ("succeeded", "failed"):
            final_row = row
            break

    if not final_row:
        result = ItemResult(
            id="S5-CSC-RUN-CSC-HEALTH", name="oam Health check",
            status=ItemStatus.FAIL,
            detail=f"agent_job(id={job_id}) 폴링 타임아웃 (15s)",
            stage=5,
        )
        _save(ctx, 14, result)
        return result

    jstatus, rc, stdout = final_row
    mgmt_port = _ports(ctx)["mgmt"]
    ok = (jstatus == "succeeded" and (rc == 0 or rc == "0")
          and f"tcp:{mgmt_port}=open" in stdout)
    _set(ctx, "mgmt_health_ok", bool(ok))
    detail = (
        f"- 결과: status={jstatus} rc={rc} out={stdout[:160]}\n"
        f"- 판정: {'OK' if ok else 'FAIL'}"
    )
    result = ItemResult(
        id="S5-CSC-RUN-CSC-HEALTH", name="oam Health check",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail=detail, stage=5,
    )
    _save(ctx, 14, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 15 — console 정적 서빙 (oam 단일 오리진)
# ─────────────────────────────────────────────────────────────
def step_15_console_static(ctx: VerifyContext) -> ItemResult:
    """Step 15 — 배포본 OAM 이 콘솔 SPA 를 정적 서빙하는지 확인.

    console 은 별도 프로세스가 아니다 — oam-base 가 SPA 를 동봉해 단일 오리진
    (mgmt 포트 HTTPS) 으로 서빙한다 (02_deployment §2.1). GET / 가 200 + HTML
    (SPA 엔트리) 이면 PASS.
    """
    if already_ran(ctx, 15):
        return get_native_result(ctx, 15)

    mgmt_port = _ports(ctx)["mgmt"]
    name = f"console 정적 서빙 (oam {mgmt_port})"
    if not _get(ctx, "mgmt_start_ok"):
        result = ItemResult(
            id="S5-CSC-RUN-CONSOLE-START", name=name,
            status=ItemStatus.SKIP,
            detail="step 13 (oam Start) 미실행 / 실패",
            stage=5,
        )
        _save(ctx, 15, result)
        return result

    import ssl as _ssl
    import urllib.request as _rq
    ctx_ssl = _ssl.create_default_context()
    ctx_ssl.check_hostname = False
    ctx_ssl.verify_mode = _ssl.CERT_NONE
    url = f"{_deployed_mgmt_base(ctx)}/"
    status = 0
    body = ""
    try:
        with _rq.urlopen(url, timeout=10, context=ctx_ssl) as resp:
            status = resp.status
            body = resp.read(4096).decode("utf-8", "replace")
    except Exception as e:
        body = f"{type(e).__name__}: {e}"

    ok = status == 200 and ("<html" in body.lower() or "<!doctype" in body.lower())
    _set(ctx, "console_static_ok", bool(ok))
    if ok:
        result = ItemResult(
            id="S5-CSC-RUN-CONSOLE-START", name=name,
            status=ItemStatus.PASS,
            detail=f"- [OK] GET / → 200, SPA HTML 서빙 ({url})",
            stage=5,
        )
    else:
        result = ItemResult(
            id="S5-CSC-RUN-CONSOLE-START", name=name,
            status=ItemStatus.FAIL,
            detail=f"- [FAIL] GET / → status={status} body[:120]={body[:120]!r} ({url})",
            stage=5,
        )
    _save(ctx, 15, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 16 — 배포본 OAM(4445) admin login
# ─────────────────────────────────────────────────────────────
def step_16_modules_auth(ctx: VerifyContext) -> ItemResult:
    """Step 16 — 배포본 OAM(4445) admin login → tok2.

    oam 이 step 13 에서 mgmt 포트로 LISTEN 중이어야 함. mgmt_start_ok 가
    False 면 SKIP. 관리 계정은 패키징된 oam.json CimsAuth (admin/1234).
    """
    if already_ran(ctx, 16):
        return get_native_result(ctx, 16)

    if not _get(ctx, "mgmt_start_ok"):
        result = ItemResult(
            id="S5-MODULES-DEPLOY-AUTH", name="배포본 OAM admin login",
            status=ItemStatus.SKIP,
            detail="step 13 (oam Start) 미실행 / 실패",
            stage=5,
        )
        _save(ctx, 16, result)
        return result

    base = _deployed_mgmt_base(ctx)
    login_id = os.environ.get("CIMS_TB_ADMIN_ID", "admin")
    pw = os.environ.get("CIMS_TB_ADMIN_PASSWORD", "1234")
    tok2 = csc_http.admin_login(base, login_id, pw, timeout=5)
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-DEPLOY-AUTH", name="배포본 OAM admin login",
            status=ItemStatus.FAIL,
            detail=f"admin login 실패 (base={base} id={login_id})",
            stage=5,
        )
        _save(ctx, 16, result)
        return result

    _set(ctx, "tok2", tok2)
    result = ItemResult(
        id="S5-MODULES-DEPLOY-AUTH", name="배포본 OAM admin login",
        status=ItemStatus.PASS,
        detail=f"base={base} id={login_id} → JWT (len={len(tok2)})",
        stage=5,
    )
    _save(ctx, 16, result)
    return result


# ─────────────────────────────────────────────────────────────
# Step 17 — 모듈 패키지 업로드 (배포본 OAM 4445)
# ─────────────────────────────────────────────────────────────
def step_17_modules_pkg_upload(ctx: VerifyContext) -> ItemResult:
    """Step 17 — 모듈 (csc + csp/psp/cmp/pmp) tarball 을 배포본 OAM(4445) 에
    업로드. pkg2_id_{id} 캐시.
    """
    if already_ran(ctx, 17):
        return get_native_result(ctx, 17)

    tok2 = _get(ctx, "tok2", "")
    if not tok2:
        result = ItemResult(
            id="S5-MODULES-DEPLOY-PKG-UPLOAD",
            name="service-server 패키지 업로드",
            status=ItemStatus.SKIP,
            detail="step 16 (배포본 OAM admin login) 미실행 / 실패",
            stage=5,
        )
        _save(ctx, 17, result)
        return result

    base = _deployed_mgmt_base(ctx)
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
    env["CIMS_AGENT_NO_SUPERVISE"] = "1"   # dev 스택 오염 방지 (step 07 참조)
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


def _all_modules_online(base: str, token: str) -> bool:
    """모든 service-server agent (4 인스턴스) 가 배포본 csc 에서 online 인지."""
    for inst in _INSTANCES:
        if not _agent_online(base, token, inst["agent_name"]):
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

    base = _deployed_mgmt_base(ctx)
    notes: list = []
    register_fail = False

    # agent_name 기준 dedup — 같은 server 의 여러 변종은 1개 cims_agent + sync_port 공유.
    # 첫 인스턴스가 agent register + Test-agent spawn 을 담당. 후속 형제 인스턴스는
    # 같은 aid/ta_pid 를 그대로 복사 (step 19 의 deployment 는 인스턴스별 분기).
    seen_agents: dict = {}   # agent_name → (aid, enroll_tok, ta_pid)
    for inst in _INSTANCES:
        m = inst["id"]
        aname = inst["agent_name"]
        if aname in seen_agents:
            aid, enroll_tok, pid = seen_agents[aname]
            _set(ctx, f"aid2_{m}", aid)
            _set(ctx, f"enroll_tok2_{m}", enroll_tok)
            _set(ctx, f"ta_pid2_{m}", pid)
            notes.append(f"- [OK] {aname}/{m}: 형제 변종 — aid={aid} pid={pid} 공유")
            continue
        aid, enroll_tok, err = _register_one_module_agent(base, tok2, aname)
        if err:
            notes.append(f"- [FAIL] {aname}: {err}")
            register_fail = True
            continue
        _set(ctx, f"aid2_{m}", aid)
        _set(ctx, f"enroll_tok2_{m}", enroll_tok)
        pid, perr = _spawn_one_module_agent(ctx, m, base, aname, enroll_tok)
        if perr:
            notes.append(f"- [FAIL] {aname}: spawn {perr}")
            register_fail = True
            continue
        _set(ctx, f"ta_pid2_{m}", pid)
        seen_agents[aname] = (aid, enroll_tok, pid)
        notes.append(
            f"- [OK] {aname}/{m}: aid={aid} pid={pid} sync={inst['sync_port']}"
        )

    # enroll polling — 모두 online 까지 20s
    online = False
    waited = 0
    if not register_fail:
        for _ in range(20):
            time.sleep(1); waited += 1
            if _all_modules_online(base, tok2):
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

    base = _deployed_mgmt_base(ctx)
    notes: list = []
    fail = False
    for inst in _INSTANCES:
        m = inst["id"]
        aid = _get(ctx, f"aid2_{m}")
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
        overlay = dict(inst.get("config_overlay") or {})
        # csp 변종 — DB 접속 정보를 dev 환경 SoT(dist csp.json)에서 주입.
        # 서버측이 overlay 를 config_template 기본값(Host=127.0.0.1)으로 채우므로,
        # 운영에서 블루프린트가 담당하는 값을 verify 체인에서는 하네스가 담당한다.
        if inst.get("tarball") in ("csp", "psp", "isp"):
            dbc = _dbmod.csp_db_config(ctx.dist_dir)
            if dbc:
                overlay.update({
                    "Setup.Database.Host":     dbc["Host"],
                    "Setup.Database.Port":     int(dbc.get("Port", 3306)),
                    "Setup.Database.User":     dbc["User"],
                    "Setup.Database.Password": dbc["Password"],
                    "Setup.Database.DbName":   dbc["DbName"],
                })
        elif inst.get("tarball") == "csc":
            dbc = _dbmod.csp_db_config(ctx.dist_dir)
            if dbc:
                overlay.update({
                    "CimsDatabase.Host":     dbc["Host"],
                    "CimsDatabase.Port":     int(dbc.get("Port", 3306)),
                    "CimsDatabase.User":     dbc["User"],
                    "CimsDatabase.Password": dbc["Password"],
                    "CimsDatabase.Db":       dbc["DbName"],
                })
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
# Step 20 — 3 install jobs + deployment 상태 폴링
# ─────────────────────────────────────────────────────────────
def step_20_modules_install_poll(ctx: VerifyContext) -> ItemResult:
    """Step 20 — service-server install jobs 발행 + deployment 상태 폴링 60s."""
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

    base = _deployed_mgmt_base(ctx)
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
            st = _deployment_status(base, tok2, did)
            final_status[m] = st or "(unknown)"
            if st in ("pending", "deploying"):
                still = True
        if not still:
            all_done = True; break

    _set(ctx, "all_install_done_modules", bool(all_done))
    # deployment status: pending|deploying|running|stopped|failed|removed
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
def variant_jsonl_dir(dist_dir: str, inst: dict) -> str:
    """변종 인스턴스의 collection jsonl 디렉토리 (csp ConfigJsonlDir fallback 정합).

    버전형 설치(current 통로)는 `<agent>/modules/<dir>/current/config` —
    csp.json(=…/current/csp/config/csp.json) 부모×3 + "/config" 과 일치.
    구(평탄) 설치는 `<agent>/config` fallback."""
    cur = os.path.join(dist_dir, inst["agent_name"], "modules", inst["dir"], "current")
    if os.path.isdir(cur):
        return os.path.join(cur, "config")
    return os.path.join(dist_dir, inst["agent_name"], "config")


def _seed_variant_local_nodes(cfg_dir: str, inst: dict) -> bool:
    """csp 변종의 SIP 리스너 SoT(local_nodes.jsonl) 시드 — non-clobber.

    CSP 는 primary local_node 부재 시 기동을 거부한다(fail-fast). dev 는
    configure.sh 가, 운영은 배포 렌더가 시드하지만 S5 직접 배포 체인은 그 단계가
    없어 하네스가 최초 1회 시드한다. 반환: 시드 수행 여부."""
    path = os.path.join(cfg_dir, "local_nodes.jsonl")
    if os.path.isfile(path):
        return False
    os.makedirs(cfg_dir, exist_ok=True)
    import uuid as _uuid
    row = {"id": _uuid.uuid4().hex, "name": f"access-udp-{inst['id']}",
           "enabled": True, "is_primary": True, "edge": "access",
           "bind_ip": inst["local_ip"], "bind_port": inst["listen"][0],
           "protocol": "UDP", "tags": ["verify-s5-seed"],
           "note": "auto-seeded by cims_verify S5 (step 21)"}
    with open(path, "w") as f:
        f.write(json.dumps(row, ensure_ascii=False) + "\n")
    return True


def step_21_modules_start(ctx: VerifyContext) -> ItemResult:
    """Step 21 — service-server Start (csc 4446/tcp + csp/psp 5060/udp + cmp/pmp 9000/udp)."""
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

    base = _deployed_mgmt_base(ctx)
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
        # csp 변종 — SIP 리스너 SoT(local_nodes) 없으면 기동 거부 → 시작 전 시드
        if inst.get("tarball") in ("csp", "psp", "isp"):
            cfg_dir = variant_jsonl_dir(ctx.dist_dir, inst)
            if _seed_variant_local_nodes(cfg_dir, inst):
                notes.append(f"- [SEED] {aname}/{m}: local_nodes.jsonl → {cfg_dir}")
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
            "- mgmt-server: oam(4445, console 정적 동봉) · sim (install-only)",
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
    # 순서: 모듈(배포본 OAM 경유) 먼저 → mgmt(oam) 나중 — oam 이 배포본 스택의
    # 관리평면이라 먼저 내리면 이후 stop 발행이 갈 곳이 없다.
    notes: list = []
    tok = _get(ctx, "tok", "")
    # 모듈 stop (배포본 OAM 4445 경유) — csc + service-server
    tok2 = _get(ctx, "tok2", "")
    if tok2:
        for inst in _INSTANCES:
            m = inst["id"]
            did = _get(ctx, f"dep2_id_{m}")
            if did is None: continue
            try:
                st, _ = csc_http.post_json(
                    f"{_deployed_mgmt_base(ctx)}/api/v1/deployments/{did}/job",
                    {"job_type": "stop"}, token=tok2, timeout=10,
                )
                notes.append(f"- {inst['agent_name']}: stop 발행 status={st}")
            except Exception as e:
                notes.append(f"- {inst['agent_name']}: stop 발행 예외 {e}")
    # mgmt stop (TB-OAM 4419 경유) — oam/sim. 모듈 stop 뒤에 내린다.
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
    time.sleep(5)

    # Test-agent kill — mgmt-server + service-server agents (agent_name dedup).
    # 같은 agent_name 의 변종은 같은 ta_pid 를 공유하므로 set 으로 중복 제거.
    import signal
    killed = 0
    pid_set: set = set()
    csc_pid = _get(ctx, "ta_pid_csc")
    if csc_pid: pid_set.add(int(csc_pid))
    for inst in _INSTANCES:
        p = _get(ctx, f"ta_pid2_{inst['id']}")
        if p: pid_set.add(int(p))
    total = len(pid_set)
    for pid in pid_set:
        try:
            os.kill(pid, signal.SIGTERM)
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
    job + 상태 폴링) 순차.
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
