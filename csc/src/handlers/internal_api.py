"""내부 토폴로지 API — CSP(시그널링) ↔ CSC(설정 서버), admin 서버(4421) · `/api/v1` 밖.

  GET /internal/mcptt/endpoint
    Authorization: Bearer <InternalApi.Token>
  200 {"xcap_root": "https://host:4430/", "mcptt_port": 4430, "public_url_configured": true|false}
  401 토큰 불일치 · 503 auc_disabled(토큰 미설정)

CSP 는 이 값을 xcap-diff NOTIFY 의 `xcap-root` 와 MCData FD 다운로드 URL base 로 쓴다.
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


def _bearer(headers: dict) -> str:
    for k, v in (headers or {}).items():
        if str(k).lower() == "authorization":
            v = str(v or "")
            return v[7:].strip() if v.lower().startswith("bearer ") else ""
    return ""


async def handle_mcptt_endpoint(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    if handler_args.method.upper() != "GET":
        return HandlerResult(status=405, body={"error": "Method Not Allowed"})
    token = auc.internal_token()
    if not token:
        return HandlerResult(status=503, body={"error": "auc_disabled",
                                               "detail": "InternalApi.Token not configured"})
    if not hmac.compare_digest(_bearer(handler_args.headers), token):
        return HandlerResult(status=401, body={"error": "unauthorized"})

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


CSC_INTERNAL_HANDLER_LIST = [
    (ENDPOINT_PATH, handle_mcptt_endpoint, {}),
]
