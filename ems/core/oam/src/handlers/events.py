"""
CIMS 이벤트(정상 동작 통지) 이력 REST API — 알람과 스트림 분리 (alarm_self_reporting.md §6)

Routes:
  GET /api/v1/events?days=7&type=&kind=&limit=500   최근 이벤트 목록
  GET /api/v1/events/types                          최근 30일 이벤트 type 목록
"""

from urllib.parse import urlparse, unquote
from pathlib import PurePath

from httpsrv.handler import HandlerArgs, HandlerResult
from services import event_log


_BASE = '/api/v1/events'


def _path_parts(full_path: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(_BASE))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _service_log_dir(config: dict) -> str:
    sl = config.get('ServiceLogging', {})
    return sl.get('Dir', '') or config.get('ServiceLogDir', config.get('MsgLogDir', ''))


async def handle_events(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    if handler_args.method.upper() != 'GET':
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})
    # 전 엔드포인트 인증 (파이프라인 §7)
    from handlers.auth import require_auth
    _token, autherr = require_auth(handler_args)
    if autherr:
        return autherr
    base = _service_log_dir(config)
    parts = _path_parts(handler_args.full_path)
    qs = handler_args.query_params or {}

    def qp(name, default=None):
        v = qs.get(name)
        return v if v not in (None, '') else default

    try:
        if parts and parts[0] == 'types':
            return HandlerResult(status=200, body={'types': event_log.list_types(base, days=30)})

        days = max(1, min(int(qp('days', '7')), 90))
        limit = max(1, min(int(qp('limit', '500')), 5000))
        events = event_log.read_recent(base, days=days, type_filter=qp('type'),
                                       kind_filter=qp('kind'), limit=limit)
        return HandlerResult(status=200, body={
            'days': days,
            'count': len(events),
            'events': events,
        })
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


CIMS_EVENTS_HANDLER_LIST = [
    (_BASE, handle_events, {}),
]
