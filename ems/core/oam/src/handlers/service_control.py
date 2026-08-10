"""
CSC Service Control API.

  GET /api/v1/services                 — 전체 서비스 상태
  POST /api/v1/services/{name}/start   — 서비스 기동
  POST /api/v1/services/{name}/stop    — 서비스 중지
  POST /api/v1/services/{name}/restart — 재기동

드라이버:
  - cims_svc (기본): /dist/agent/bin/cims-svc 를 subprocess 로 실행 (운영 도구)
  - systemd: systemctl 호출 (환경변수 CIMS_SERVICE_DRIVER=systemd + sudoers 설정 필요).
    unit 네이밍은 cims@<svc>.service (instantiated)
  - 환경변수 CIMS_SERVICE_DRIVER 로 선택, 없으면 cims_svc

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

from services import service_registry

logger = Logger()


def _audit_service_action(config: dict, actor: str, actor_ip: str,
                          service: str, action: str, reason: str = "") -> None:
    """서비스 제어 감사 — 이벤트 스트림(event_log, kind=audit)에 기록
    (alarm_self_reporting.md §6 — 구 {CimsRuntimeDir}/service_control_audit JSONL 흡수).
    콘솔 '알람·이벤트 이력 > 이벤트' 탭과 GET /events 로 조회된다."""
    try:
        from services import event_log
        sl = (config or {}).get('ServiceLogging', {})
        base = sl.get('Dir', '') or (config or {}).get(
            'ServiceLogDir', (config or {}).get('MsgLogDir', ''))
        event_log.record_event(base, {
            'type': 'service_control', 'kind': 'audit',
            'source': {'mo_class': 'software', 'mo_instance': f'cims/{service}',
                       'detected_by': 'oam'},
            'message': f"{actor}({actor_ip}) {service} {action}",
            'params': {'actor': actor, 'actor_ip': actor_ip, 'action': action,
                       'reason': (reason or '')[:512] or None},
        })
    except Exception as e:
        logger.log_warning(f"service_control audit: {e}")

_SERVICE_BASE = "/api/v1/services"
# fallback — service descriptor 미존재 시. 평상시엔 registry.controllable_modules() 사용.
_ALLOWED = {"cmp", "csp", "cwrtc", "csc", "console", "phone"}
_ACTIONS = {"start", "stop", "restart"}

# cims-svc 위치 탐색 (agent/bin/cims-svc — 운영 lifecycle 도구)
def _find_cims_svc() -> Optional[str]:
    """우선순위:
      1) CIMS_SVC_PATH — 명시 오버라이드.
      2) $CIMS_AGENT_PREFIX/agent/current/bin/cims-svc — 배포 환경 정본 규약(agent.md §3).
         agent 가 모듈을 env 상속으로 기동하므로 배포된 base 는 이 env 를 이미 가진다.
         current 심링크 = systemd/sudoers 와 동일한 버전 무관 고정 경로.
      3) __file__ 부모 walk-up — 레포/dist 개발 트리(agent/bin) + prefix 직접 실행
         (agent/current/bin, env 부재 대비) fallback."""
    env_path = os.environ.get("CIMS_SVC_PATH")
    if env_path and os.path.isfile(env_path):
        return env_path
    prefix = os.environ.get("CIMS_AGENT_PREFIX")
    if prefix:
        cand = Path(prefix) / "agent" / "current" / "bin" / "cims-svc"
        if cand.is_file(): return str(cand)
    here = Path(__file__).resolve()
    for p in here.parents:
        for cand in (p / "agent" / "bin" / "cims-svc",
                     p / "agent" / "current" / "bin" / "cims-svc"):
            if cand.is_file(): return str(cand)
    return None


def _driver() -> str:
    return os.environ.get("CIMS_SERVICE_DRIVER", "cims_svc").lower()


# TB-CSC 가 자기 config (csc-tb.json, port 4419) 로 띄워진 상태에서 subprocess 가
# 환경을 그대로 상속하면 자식 csc_app.py 도 csc-tb.json 을 읽어 4419/4431 bind 시도 → 충돌.
# Test-CSC / 배포본 csc 는 base csc.json (4421/4445) 을 써야 하므로 TB 전용 env 차단.
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


async def _invoke_cims_svc(action: str, service: str) -> HandlerResult:
    script = _find_cims_svc()
    if not script:
        return HandlerResult(status=500,
            body={"error": "cims_svc_not_found",
                  "hint": "Set CIMS_SVC_PATH or CIMS_AGENT_PREFIX env var"},
            media_type="application/json")
    cwd = str(Path(script).parent)
    argv = ["/bin/bash", script, action, service]
    rc, out, err = await _run_cmd(argv, cwd=cwd, timeout=45, env=_sanitized_env())
    return HandlerResult(
        status=200 if rc == 0 else 500,
        body={
            "driver": "cims_svc",
            "service": service, "action": action,
            "returncode": rc,
            "stdout": out.decode(errors="replace")[-4000:],
            "stderr": err.decode(errors="replace")[-2000:],
        },
        media_type="application/json",
    )


async def _invoke_systemd(action: str, service: str) -> HandlerResult:
    # systemd unit 네이밍 규약: cims@<svc>.service (instantiated unit, Phase 1.F+)
    unit = f"cims@{service}.service"
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
    return await _invoke_cims_svc(action, service)


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
        # 전체 상태 — agent/bin/cims-svc status 호출
        script = _find_cims_svc()
        if not script:
            return HandlerResult(status=500,
                body={"error": "cims_svc_not_found",
                      "hint": "Set CIMS_SVC_PATH or CIMS_AGENT_PREFIX env var"},
                media_type="application/json")
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
    allowed = service_registry.controllable_modules(config) or _ALLOWED
    if service not in allowed:
        return HandlerResult(status=400, body={"error": "unknown_service", "allowed": sorted(allowed)},
                             media_type="application/json")
    if action not in _ACTIONS:
        return HandlerResult(status=400, body={"error": "unknown_action", "allowed": sorted(_ACTIONS)},
                             media_type="application/json")
    if method != "POST":
        return HandlerResult(status=405, body={"error": "method_not_allowed"}, media_type="application/json")

    # CSC 자기 stop 경고 (restart 는 허용 — cims-svc 가 감싸서 처리)
    if service == "csc" and action == "stop":
        logger.log_warning("ServiceControl: CSC self-stop requested — UI will disconnect immediately")

    actor = _actor_from_headers(handler_args.headers)
    result = await _invoke(action, service)

    # 감사 로그 (base 자체 — mcptt 비의존)
    _audit_service_action(
        config, actor, handler_args.client_ip,
        service, action.upper(), reason=f"driver={_driver()}",
    )

    logger.log_info(f"ServiceControl: {actor} {action} {service} rc={result.body.get('returncode')}")
    return result


CIMS_SERVICE_CONTROL_HANDLER_LIST = (
    (_SERVICE_BASE, handle_services, {}),
)
