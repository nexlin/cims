"""
CSC 자기 API 문서 — 이 모듈이 제공하는 엔드포인트의 자기기술을 그대로 반환한다.

OAM 이 이 엔드포인트를 호출해 자기 수집 결과에 병합한다. 분리 배포(csc 가 OAM 과 다른 서버)에서는
OAM 에 csc 핸들러 코드가 없으므로 import 로는 문서를 얻을 수 없다 — 그래서 **모듈이 자기 문서를
직접 서비스**한다. 정본: docs/design/features/api_docs.md

  GET /api/v1/api-docs → { module: 'csc', count, apis[] }
"""
from httpsrv.handler import HandlerArgs, HandlerResult
from services import admin_auth

from .admin import CIMS_ADMIN_API_DOCS
from .org import CIMS_ORG_API_DOCS


_BASE = '/api/v1/api-docs'


async def handle_api_docs(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    if handler_args.method.upper() != 'GET':
        return HandlerResult(status=405, body={'error': 'method_not_allowed'})
    _payload, err = admin_auth.require_role(handler_args, 'monitor')
    if err:
        return err
    apis = list(CIMS_ADMIN_API_DOCS) + list(CIMS_ORG_API_DOCS)
    return HandlerResult(status=200, body={'module': 'csc', 'count': len(apis), 'apis': apis})


CSC_API_DOCS_HANDLER_LIST = [
    (_BASE, handle_api_docs, {}),
]
