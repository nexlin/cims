"""
API Catalog REST API — 외부(VoLTE/PTT 사용관리 웹 등) 인계용 공유(READ) 엔드포인트 노출.

Service Descriptor 의 shareable_apis[] 를 단일 소스로 집계해:
  GET /api/v1/api-catalog                공유 엔드포인트 목록 (연동/API 탭이 소비)
  GET /api/v1/api-catalog/openapi.json   생성된 OpenAPI 3.0.3 문서 (다른 팀 인계물, 다운로드)

descriptor 는 base-resident(oam_app 이 role 무관 seed)라 role=base/all 동일 동작 — 메타데이터만 읽고
실제 stats/recording 엔드포인트는 건드리지 않는다. shareable_apis 는 READ 전용 인계 계약(내부 운영 API 제외).
"""
from urllib.parse import urlparse
from pathlib import PurePath
from datetime import datetime, timezone

from httpsrv.handler import HandlerArgs, HandlerResult
from services import service_registry


_BASE = '/api/v1/api-catalog'
_CATEGORY_ORDER = ['stats', 'history', 'recording', 'subscriber']


def _parts(full_path: str):
    path = urlparse(full_path).path
    try:
        return tuple(PurePath(path).relative_to(PurePath(_BASE)).parts)
    except ValueError:
        return ()


def _categories(apis: list) -> list:
    present = {(a.get('category') or 'misc') for a in apis}
    ordered = [c for c in _CATEGORY_ORDER if c in present]
    return ordered + sorted(present - set(_CATEGORY_ORDER))


def _build_openapi(config: dict) -> dict:
    """shareable_apis → 최소·유효 OpenAPI 3.0.3. 응답 스키마는 점진 도입(example 있으면 example, 없으면 object)."""
    apis = service_registry.shareable_apis(config)
    paths: dict = {}
    tagset: list = []
    for a in apis:
        p = a.get('path')
        if not p:
            continue
        method = (a.get('method') or 'GET').lower()
        cat = a.get('category') or 'misc'
        if cat not in tagset:
            tagset.append(cat)
        parameters = []
        for q in (a.get('params') or []):
            name = q.get('name')
            if not name:
                continue
            loc = q.get('in') or ('path' if ('{' + name + '}') in p else 'query')
            schema = {'type': q.get('type', 'string')}
            if q.get('enum'):
                schema['enum'] = q['enum']
            parameters.append({
                'name': name, 'in': loc,
                'required': bool(q.get('required')) or loc == 'path',
                'description': q.get('desc', ''),
                'schema': schema,
            })
        if a.get('example') is not None:
            content = {'application/json': {'example': a['example']}}
        else:
            content = {'application/json': {'schema': {'type': 'object'}}}
        paths.setdefault(p, {})[method] = {
            'summary': a.get('summary', ''),
            'operationId': a.get('id'),
            'description': a.get('summary', ''),
            'tags': [cat],
            'parameters': parameters,
            'responses': {'200': {'description': a.get('response_desc') or '성공', 'content': content}},
            'security': [{'bearerAuth': []}],
        }
    return {
        'openapi': '3.0.3',
        'info': {
            'title': 'CIMS 공유 조회 API',
            'version': '1.0.0',
            'description': 'CIMS 가 외부(VoLTE/PTT 사용관리 웹 등)에 공개하는 READ 엔드포인트. 인증: Bearer JWT.',
        },
        'servers': [{'url': '/api/v1'}],
        'tags': [{'name': t} for t in tagset],
        'paths': paths,
        'components': {'securitySchemes': {'bearerAuth': {'type': 'http', 'scheme': 'bearer', 'bearerFormat': 'JWT'}}},
        'security': [{'bearerAuth': []}],
    }


async def handle_api_catalog(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    method = handler_args.method.upper()
    parts = _parts(handler_args.full_path)

    try:
        if method != 'GET':
            return HandlerResult(status=405, body={'error': 'method_not_allowed'})

        if not parts:
            apis = service_registry.shareable_apis(config)
            return HandlerResult(status=200, body={
                'generated_at': datetime.now(timezone.utc).isoformat(),
                'count': len(apis),
                'categories': _categories(apis),
                'endpoints': apis,
            })

        if parts[0] == 'openapi.json':
            return HandlerResult(status=200, body=_build_openapi(config),
                                 headers={'Content-Disposition': 'attachment; filename="cims-openapi.json"'})

        return HandlerResult(status=404, body={'error': 'not_found'})
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


CIMS_API_CATALOG_HANDLER_LIST = [
    (_BASE, handle_api_catalog, {}),
]
