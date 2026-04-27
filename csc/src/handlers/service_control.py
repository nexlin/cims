"""
CSC Service Control API.

  GET /api/v1/services                 — 전체 서비스 상태
  POST /api/v1/services/{name}/start   — 서비스 기동
  POST /api/v1/services/{name}/stop    — 서비스 중지
  POST /api/v1/services/{name}/restart — 재기동

드라이버:
  - cims_sh (기본): /dist 의 cims.sh 를 subprocess 로 실행
  - systemd: systemctl 호출 (환경변수 CIMS_SERVICE_DRIVER=systemd + sudoers 설정 필요)
  - 환경변수 CIMS_SERVICE_DRIVER 로 선택, 없으면 cims_sh

안전장치:
  - CSC 자체 stop 은 경고 응답 (UI 에서 추가 확인 필수)
  - 알 수 없는 서비스명 거부
  - audit 기록
"""

from __future__ import annotations

import asyncio
import json
import os
import shlex
import subprocess
from pathlib import Path
from typing import Optional

from httpsrv.handler import HandlerArgs, HandlerResult
from util.log_util import Logger

from services.mcptt import audit_config_change

logger = Logger()

_SERVICE_BASE = "/api/v1/services"
_ALLOWED = {"cmp", "csp", "cwrtc", "csc", "console", "phone"}
_ACTIONS = {"start", "stop", "restart"}

# cims.sh 위치 탐색
def _find_cims_sh() -> Optional[str]:
    # 우선순위: 환경변수 → /dist/cims.sh (CSC 상대경로) → /usr/local/bin
    env_path = os.environ.get("CIMS_SH_PATH")
    if env_path and os.path.isfile(env_path):
        return env_path
    # CSC 소스에서 build/dist 찾기
    here = Path(__file__).resolve()
    for p in here.parents:
        cand = p / "cims.sh"
        if cand.is_file(): return str(cand)
    # /home/nex/work/cims/build/dist/cims.sh 같은 표준 위치
    for cand in ("/home/nex/work/cims/build/dist/cims.sh",
                 "/opt/cims/cims.sh", "/usr/local/bin/cims.sh"):
        if os.path.isfile(cand): return cand
    return None


def _driver() -> str:
    return os.environ.get("CIMS_SERVICE_DRIVER", "cims_sh").lower()


# TB-CSC 가 자기 config (csc-tb.json, port 4419) 로 띄워진 상태에서 subprocess 가
# 환경을 그대로 상속하면 자식 csc_app.py 도 csc-tb.json 을 읽어 4419/4431 bind 시도 → 충돌.
# Test-CSC / 배포본 csc 는 base csc.json (4421/4445/4420) 을 써야 하므로 TB 전용 env 차단.
_BLOCKED_ENV_KEYS = {"CIMS_CSC_CONFIG", "CIMS_AGENT_SYNC_PORT"}


def _sanitized_env() -> dict:
    return {k: v for k, v in os.environ.items() if k not in _BLOCKED_ENV_KEYS}


async def _run_cmd(argv: list, cwd: Optional[str] = None, timeout: int = 30,
                   env: Optional[dict] = None) -> tuple:
    """subprocess 를 async 로 실행. (returncode, stdout, stderr) 반환."""
    proc = await asyncio.create_subprocess_exec(
        *argv,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        cwd=cwd,
        env=env,
    )
    try:
        stdout, stderr = await asyncio.wait_for(proc.communicate(), timeout=timeout)
    except asyncio.TimeoutError:
        proc.kill()
        return (-1, b"", b"timeout")
    return (proc.returncode, stdout or b"", stderr or b"")


async def _invoke_cims_sh(action: str, service: str) -> HandlerResult:
    script = _find_cims_sh()
    if not script:
        return HandlerResult(status=500,
            body={"error": "cims_sh_not_found", "hint": "Set CIMS_SH_PATH env var"},
            media_type="application/json")
    cwd = str(Path(script).parent)
    argv = ["/bin/bash", script, action, service]
    rc, out, err = await _run_cmd(argv, cwd=cwd, timeout=45, env=_sanitized_env())
    return HandlerResult(
        status=200 if rc == 0 else 500,
        body={
            "driver": "cims_sh",
            "service": service, "action": action,
            "returncode": rc,
            "stdout": out.decode(errors="replace")[-4000:],
            "stderr": err.decode(errors="replace")[-2000:],
        },
        media_type="application/json",
    )


async def _invoke_systemd(action: str, service: str) -> HandlerResult:
    # systemd unit 네이밍 규약: cims-<name>.service
    unit = f"cims-{service}.service"
    argv = ["sudo", "-n", "systemctl", action, unit]
    rc, out, err = await _run_cmd(argv, timeout=30)
    return HandlerResult(
        status=200 if rc == 0 else 500,
        body={
            "driver": "systemd", "unit": unit,
            "service": service, "action": action,
            "returncode": rc,
            "stdout": out.decode(errors="replace"),
            "stderr": err.decode(errors="replace"),
        },
        media_type="application/json",
    )


async def _invoke(action: str, service: str) -> HandlerResult:
    drv = _driver()
    if drv == "systemd": return await _invoke_systemd(action, service)
    return await _invoke_cims_sh(action, service)


def _actor_from_headers(headers: dict) -> str:
    auth = headers.get("Authorization") or headers.get("authorization") or ""
    if auth.lower().startswith("bearer "):
        token = auth[7:].strip()
        try:
            import jwt as _jwt
            d = _jwt.decode(token, options={"verify_signature": False})
            return d.get("sub") or d.get("login_id") or "admin"
        except Exception: pass
    return "admin"


async def handle_services(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get("config", {})
    path = handler_args.full_path.split("?", 1)[0]
    rel = path[len(_SERVICE_BASE):].strip("/")
    parts = rel.split("/") if rel else []
    method = handler_args.method.upper()

    if len(parts) == 0:
        if method != "GET":
            return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")
        # 전체 상태 — cims.sh status 호출
        script = _find_cims_sh()
        if not script:
            return HandlerResult(status=500, body={"error": "cims_sh_not_found"}, media_type="application/json")
        rc, out, err = await _run_cmd(["/bin/bash", script, "status"], cwd=str(Path(script).parent), timeout=10, env=_sanitized_env())
        return HandlerResult(status=200, body={
            "driver": _driver(),
            "output": out.decode(errors="replace"),
            "stderr": err.decode(errors="replace") if rc != 0 else None,
        }, media_type="application/json")

    if len(parts) != 2:
        return HandlerResult(status=400, body={"error": "usage: POST /api/v1/services/{name}/{start|stop|restart}"},
                             media_type="application/json")

    service, action = parts[0], parts[1]
    if service not in _ALLOWED:
        return HandlerResult(status=400, body={"error": "unknown_service", "allowed": sorted(_ALLOWED)},
                             media_type="application/json")
    if action not in _ACTIONS:
        return HandlerResult(status=400, body={"error": "unknown_action", "allowed": sorted(_ACTIONS)},
                             media_type="application/json")
    if method != "POST":
        return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")

    # CSC 자기 stop 경고 (restart 는 허용 — cims.sh 가 감싸서 처리)
    if service == "csc" and action == "stop":
        logger.log_warning("ServiceControl: CSC self-stop requested — UI will disconnect immediately")

    actor = _actor_from_headers(handler_args.headers)
    result = await _invoke(action, service)

    # 감사 로그
    try:
        audit_config_change(
            config.get("CimsDatabase", {}),
            actor, handler_args.client_ip,
            "service", service, action.upper(),
            before=None, after=None, reason=f"driver={_driver()}",
        )
    except Exception as e:
        logger.log_warning(f"ServiceControl: audit failed: {e}")

    logger.log_info(f"ServiceControl: {actor} {action} {service} rc={result.body.get('returncode')}")
    return result


CIMS_SERVICE_CONTROL_HANDLER_LIST = (
    (_SERVICE_BASE, handle_services, {}),
)
