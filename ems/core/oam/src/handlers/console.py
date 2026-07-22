"""
CIMS Console layout/menu 영속 REST API — OAM 플랫폼화 5-3 step2.

page = 위젯 배치(layout), nav = 메뉴 트리(menu) 를 file_store 에 저장.
저장본이 없으면 200 + sentinel({found:false}) → 프론트가 매니페스트 seed 로 fallback (코드 기본값 유지).
(404 로 응답하면 브라우저가 매 페이지마다 콘솔에 에러를 찍으므로 — 미저장은 정상 흐름이라 200.)

Routes (mounted at /api/v1/console):
  GET    /api/v1/console/layouts          저장된 layout id 목록
  GET    /api/v1/console/layouts/{id}     layout 1건 (없으면 200 {found:false} → seed fallback)
  PUT    /api/v1/console/layouts/{id}     layout 저장
  DELETE /api/v1/console/layouts/{id}     layout 삭제 (seed 로 리셋)
  GET    /api/v1/console/menu             nav 메뉴 (없으면 200 {found:false} → manifest seed)
  PUT    /api/v1/console/menu             nav 메뉴 저장
"""
from urllib.parse import urlparse, unquote
from pathlib import PurePath
import json

from httpsrv.handler import HandlerArgs, HandlerResult
from services import file_store


_BASE = '/api/v1/console'
_LAYOUTS = 'console_layouts'
_MENU = 'console_menu'
_MENU_KEY = 'default'


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


async def handle_console(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    method = handler_args.method.upper()
    parts = _parts(handler_args.full_path)

    try:
        # ── /console/layouts[/<id>] ──
        if parts and parts[0] == 'layouts':
            ldir = file_store.domain_dir(config, _LAYOUTS)
            if len(parts) == 1:
                if method != 'GET':
                    return HandlerResult(status=405, body={'error': 'method_not_allowed'})
                docs = file_store.load_all(ldir)
                return HandlerResult(status=200, body={'layouts': [
                    {'id': d.get('id'), 'title': d.get('title'),
                     'widget_count': len(d.get('widgets', [])), 'update_time': d.get('update_time')}
                    for d in docs
                ]})
            lid = parts[1]
            if method == 'GET':
                doc = file_store.load(ldir, lid)
                if doc is None:
                    # 미저장 = 정상 흐름(프론트가 seed fallback). 404 면 브라우저가 콘솔에 에러로
                    # 찍어 모든 페이지마다 빨간 로그가 남으므로, 200 + sentinel(widgets 없음)로 응답.
                    return HandlerResult(status=200, body={'id': lid, 'found': False})
                return HandlerResult(status=200, body=doc)
            if method == 'PUT':
                body = _body_dict(handler_args)
                if not isinstance(body, dict) or not isinstance(body.get('widgets'), list):
                    return HandlerResult(status=400, body={'error': 'invalid_layout — widgets[] 필요'})
                doc = {'id': lid, 'title': body.get('title'), 'widgets': body['widgets']}
                if isinstance(body.get('gap'), (int, float)):   # 카드 간 간격(px) 보존
                    doc['gap'] = body['gap']
                file_store.save(ldir, lid, doc)
                return HandlerResult(status=200, body=file_store.load(ldir, lid))
            if method == 'DELETE':
                ok = file_store.delete(ldir, lid)
                return HandlerResult(status=200, body={'deleted': ok, 'id': lid})
            return HandlerResult(status=405, body={'error': 'method_not_allowed'})

        # ── /console/menu ──
        if parts and parts[0] == 'menu':
            mdir = file_store.domain_dir(config, _MENU)
            if method == 'GET':
                doc = file_store.load(mdir, _MENU_KEY)
                if doc is None:
                    # 미저장 = 코드 SECTIONS 사용(정상). 404 콘솔 에러 방지 위해 200 + sentinel.
                    return HandlerResult(status=200, body={'found': False})
                return HandlerResult(status=200, body=doc)
            if method == 'PUT':
                body = _body_dict(handler_args)
                if not isinstance(body, dict) or not isinstance(body.get('items'), list):
                    return HandlerResult(status=400, body={'error': 'invalid_menu — items[] 필요'})
                doc = {'items': body['items']}
                # 메뉴 편집 v2 (2026-06-11): 커스텀 그룹/페이지 + 영역(운용/관리 그룹핑) 편집.
                if isinstance(body.get('custom_sections'), list):
                    doc['custom_sections'] = body['custom_sections']
                if isinstance(body.get('areas'), list):
                    doc['areas'] = body['areas']
                file_store.save(mdir, _MENU_KEY, doc)
                return HandlerResult(status=200, body=file_store.load(mdir, _MENU_KEY))
            return HandlerResult(status=405, body={'error': 'method_not_allowed'})

        return HandlerResult(status=404, body={'error': 'not_found'})
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


CIMS_CONSOLE_HANDLER_LIST = [
    (_BASE, handle_console, {}),
]
