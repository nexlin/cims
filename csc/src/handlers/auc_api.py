"""내부 AV API — CSP(S-CSCF 역할) ↔ CSC(HSS/AuC 역할), Cx MAR/MAA 상당 (sip_access_security.md §8.2).

  POST /internal/aka/av
    Authorization: Bearer <InternalApi.Token>          (csc.json — configure 가 csp.json 과 같은 값으로 렌더)
    {"msisdn": "+82…", "service": "volte"|"ptt"|"", "rand": "<hex32>", "auts": "<hex28>"}
      rand/auts 는 재동기 때만 — 직전 챌린지의 RAND 와 단말의 AUTS.
  200 {"scheme":"aka","msisdn","service","resynced","av":{"rand","autn","xres","ck","ik"}}   (hex)
  401 토큰 불일치 · 404 unknown_subscriber · 409 scheme_mismatch/keys_not_provisioned ·
  422 auts_invalid · 500 key_material(KEK 불일치) · 503 auc_disabled/schema_not_migrated
K/OPc 는 어떤 응답에도 실리지 않는다. 관리자 JWT 가 아니라 모듈 간 공유 토큰이다 — 콘솔 경로가 아니다.
"""
from __future__ import annotations

import hmac

from httpsrv.handler import HandlerArgs, HandlerResult
from services.auc import auc
from services.mcptt import logger as _logger

AV_PATH = "/internal/aka/av"


def _bearer(headers: dict) -> str:
    for k, v in (headers or {}).items():
        if str(k).lower() == "authorization":
            v = str(v or "")
            return v[7:].strip() if v.lower().startswith("bearer ") else ""
    return ""


async def handle_av(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    if handler_args.method.upper() != "POST":
        return HandlerResult(status=405, body={"error": "Method Not Allowed"})
    token = auc.internal_token()
    if not token:
        return HandlerResult(status=503, body={"error": "auc_disabled", "detail": "InternalApi.Token not configured"})
    if not hmac.compare_digest(_bearer(handler_args.headers), token):
        return HandlerResult(status=401, body={"error": "unauthorized"})

    data = handler_args.body
    if not isinstance(data, dict):
        return HandlerResult(status=400, body={"error": "JSON body required"})
    msisdn = str(data.get("msisdn") or "").strip()
    if not msisdn:
        return HandlerResult(status=400, body={"error": "msisdn required"})
    service = str(data.get("service") or "").strip().lower()
    rand_hex = str(data.get("rand") or "").strip()
    auts_hex = str(data.get("auts") or "").strip()

    from handlers.admin import _get_db
    try:
        with _get_db(kwargs.get("config", {})) as conn:
            out = auc.issue(conn, msisdn, service, rand_hex, auts_hex)
    except auc.AucError as e:
        if e.status >= 500:
            _logger.log_error(f"[auc] av {msisdn}: {e.code} {e.detail}")
        else:
            _logger.log_info(f"[auc] av {msisdn}: {e.status} {e.code}")
        return HandlerResult(status=e.status, body={"error": e.code, "detail": e.detail})
    except Exception as e:
        _logger.log_error(f"[auc] av {msisdn}: db error {e}")
        return HandlerResult(status=503, body={"error": "db_error", "detail": str(e)})
    _logger.log_info(f"[auc] av issued msisdn={msisdn} service={out['service']} resynced={out['resynced']}")
    return HandlerResult(status=200, body=out)


CSC_AUC_HANDLER_LIST = [
    (AV_PATH, handle_av, {}),
]
