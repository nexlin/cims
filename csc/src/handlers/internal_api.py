"""내부 토폴로지 API — CSP(시그널링) ↔ CSC(설정 서버), admin 서버(4421) · `/api/v1` 밖.

  GET /internal/mcptt/endpoint
    Authorization: Bearer <InternalApi.Token>
  200 {"xcap_root": "https://host:4430/", "mcptt_port": 4430, "public_url_configured": true|false}
  401 토큰 불일치 · 503 auc_disabled(토큰 미설정)

CSP 는 이 값을 xcap-diff NOTIFY 의 `xcap-root` 와 MCData FD 다운로드 URL base 로 쓴다.

  GET /internal/mcptt/service-config
    Authorization: Bearer <InternalApi.Token>   [If-None-Match: <ETag>]
  200 application/vnd.3gpp.mcptt-service-config+xml (ETag) · 304 · 401 · 503

MCPTT 서버(CSP)가 service configuration 문서(TS 24.484 §8.4)를 받는 경로 — Annex A.2.3(MCPTT 서버가 CMS 에서
service-config 을 받고 변경을 통지받는다)의 서버 간 취득. 단말이 받는 문서와 같은 XML 이다. 변경 통지는
SERVICE_CONFIG_CHANGED(UDP)로 가고 CSP 가 다시 받는다. CSP 는 floor 타이머·카운터를 CMP 로 전달한다.
단말이 문서를 받는 주소의 정본은 CSC(`McpttServer.PublicUrl`) 한 곳 — CSP 에는 이 주소를
적는 설정이 없다(과거 `Setup.Xcap.*` 는 폐기). 관리자 JWT 가 아니라 모듈 간 공유 토큰이며,
`/internal/aka/av` 와 같은 인증 규약을 쓴다.
"""
from __future__ import annotations

import hmac

from httpsrv.handler import HandlerArgs, HandlerResult
from services.auc import auc
from services import mcptt as _mcptt
from services.mcptt import logger as _logger

ENDPOINT_PATH = "/internal/mcptt/endpoint"
SERVICE_CONFIG_PATH = "/internal/mcptt/service-config"


def _bearer(headers: dict) -> str:
    for k, v in (headers or {}).items():
        if str(k).lower() == "authorization":
            v = str(v or "")
            return v[7:].strip() if v.lower().startswith("bearer ") else ""
    return ""


def _authorize(handler_args: HandlerArgs):
    """모듈 간 공유 토큰 검사 — 통과면 None, 아니면 오류 응답."""
    if handler_args.method.upper() != "GET":
        return HandlerResult(status=405, body={"error": "Method Not Allowed"})
    token = auc.internal_token()
    if not token:
        return HandlerResult(status=503, body={"error": "auc_disabled",
                                               "detail": "InternalApi.Token not configured"})
    if not hmac.compare_digest(_bearer(handler_args.headers), token):
        return HandlerResult(status=401, body={"error": "unauthorized"})
    return None


async def handle_mcptt_endpoint(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    deny = _authorize(handler_args)
    if deny:
        return deny

    xcap_root = _mcptt.public_xcap_root(handler_args)
    configured = bool(_mcptt._MCPTT_PUBLIC_URL)
    if not configured:
        _logger.log_info(f"[topology] mcptt endpoint (요청 Host 유도) → {xcap_root} "
                         f"— 다중 노드/VIP 구성은 McpttServer.PublicUrl 설정 권장")
    return HandlerResult(status=200, body={
        "xcap_root": xcap_root,
        "mcptt_port": _mcptt._MCPTT_PORT,
        "public_url_configured": configured,
    })


async def handle_mcptt_service_config(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    deny = _authorize(handler_args)
    if deny:
        return deny
    xml, etag = _mcptt.get_service_config_xml(None)
    inm = ""
    for k, v in (handler_args.headers or {}).items():
        if str(k).lower() == "if-none-match":
            inm = str(v or "")
    if inm and inm == etag:
        return HandlerResult(status=304)
    return HandlerResult(status=200, body=xml, media_type="application/vnd.3gpp.mcptt-service-config+xml",
                         headers={"Etag": etag})


CSC_INTERNAL_HANDLER_LIST = [
    (ENDPOINT_PATH, handle_mcptt_endpoint, {}),
    (SERVICE_CONFIG_PATH, handle_mcptt_service_config, {}),
]
