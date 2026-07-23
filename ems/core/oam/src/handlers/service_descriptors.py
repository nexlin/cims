"""
Service Descriptor CRUD REST API — OAM 플랫폼화 5-4.

서비스(예: CIMS)가 자기 모듈(name/port/proto/controllable)을 등록. OAM 코어의
ha_groups/build/service_control 이 이 descriptor 를 읽어 동작 (하드코딩 제거).

Routes (mounted at /api/v1/service-descriptors):
  GET    /api/v1/service-descriptors           descriptor 목록
  GET    /api/v1/service-descriptors/modules   전 descriptor 병합 모듈맵 (코어 모듈 포함)
  GET    /api/v1/service-descriptors/{id}      descriptor 1건
  PUT    /api/v1/service-descriptors/{id}      저장
  DELETE /api/v1/service-descriptors/{id}      삭제
"""
from urllib.parse import urlparse, unquote
from pathlib import PurePath
import json

from httpsrv.handler import HandlerArgs, HandlerResult
from services import file_store, service_registry


_BASE = '/api/v1/service-descriptors'
_DOMAIN = 'services'


def _parts(full_path: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(_BASE))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _body_dict(handler_args: HandlerArgs):
    b = getattr(handler_args, 'body', None)
    if isinstance(b, dict):
        return b
    if isinstance(b, (bytes, bytearray)):
        try: return json.loads(b.decode('utf-8'))
        except Exception: return None
    if isinstance(b, str):
        try: return json.loads(b)
        except Exception: return None
    return None


async def handle_service_descriptors(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    method = handler_args.method.upper()
    parts = _parts(handler_args.full_path)
    sdir = file_store.domain_dir(config, _DOMAIN)

    try:
        if not parts:
            if method != 'GET':
                return HandlerResult(status=405, body={'error': 'method_not_allowed'})
            return HandlerResult(status=200, body={'services': file_store.load_all(sdir)})

        if parts[0] == 'modules':
            if method != 'GET':
                return HandlerResult(status=405, body={'error': 'method_not_allowed'})
            return HandlerResult(status=200, body={
                'modules': service_registry.all_modules(config),
                'valid': sorted(service_registry.valid_module_names(config)),
                'controllable': sorted(service_registry.controllable_modules(config)),
            })

        if parts[0] == 'data-sources':
            # 전 descriptor 의 data_sources 병합 — 콘솔 shape 위젯이 소스 카탈로그로 소비.
            if method != 'GET':
                return HandlerResult(status=405, body={'error': 'method_not_allowed'})
            return HandlerResult(status=200, body={'data_sources': service_registry.data_sources(config)})

        sid = parts[0]
        if method == 'GET':
            doc = file_store.load(sdir, sid)
            if doc is None:
                return HandlerResult(status=404, body={'error': 'not_found', 'id': sid})
            return HandlerResult(status=200, body=doc)
        if method == 'PUT':
            body = _body_dict(handler_args)
            if not isinstance(body, dict) or not isinstance(body.get('modules'), list):
                return HandlerResult(status=400, body={'error': 'invalid_descriptor — modules[] 필요'})
            doc = {'id': sid, 'label': body.get('label') or sid, 'modules': body['modules']}
            if isinstance(body.get('alert_rules'), list):
                doc['alert_rules'] = body['alert_rules']
            if isinstance(body.get('data_sources'), list):
                doc['data_sources'] = body['data_sources']
            if isinstance(body.get('shareable_apis'), list):
                doc['shareable_apis'] = body['shareable_apis']   # 외부 공유 API 카탈로그 — 보존
            file_store.save(sdir, sid, doc)
            return HandlerResult(status=200, body=file_store.load(sdir, sid))
        if method == 'DELETE':
            ok = file_store.delete(sdir, sid)
            return HandlerResult(status=200, body={'deleted': ok, 'id': sid})
        return HandlerResult(status=405, body={'error': 'method_not_allowed'})
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


CIMS_SERVICE_DESCRIPTORS_HANDLER_LIST = [
    (_BASE, handle_service_descriptors, {}),
]
