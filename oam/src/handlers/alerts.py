"""
CIMS Alert 이력 REST API

Routes:
  GET /api/v1/alerts?days=7&type=&limit=500    최근 alert 이벤트 목록
  GET /api/v1/alerts/types                     최근 30일 alert type 목록
  GET /api/v1/alerts/summary?days=7            type별 통계 + 일별 발생량
"""

from urllib.parse import urlparse, parse_qs, unquote
from pathlib import PurePath

from httpsrv.handler import HandlerArgs, HandlerResult
from services import alert_log


_BASE = '/api/v1/alerts'


def _path_parts(full_path: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(_BASE))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _service_log_dir(config: dict) -> str:
    sl = config.get('ServiceLogging', {})
    d = sl.get('Dir', '')
    if not d:
        d = config.get('ServiceLogDir', config.get('MsgLogDir', ''))
    return d


async def handle_alerts(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    if handler_args.method.upper() != 'GET':
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

    base = _service_log_dir(config)
    parts = _path_parts(handler_args.full_path)
    qs = parse_qs(urlparse(handler_args.full_path).query)

    def qp(name, default=None):
        vals = qs.get(name)
        return unquote(vals[0]) if vals else default

    try:
        if parts and parts[0] == 'types':
            return HandlerResult(status=200, body={'types': alert_log.list_types(base, days=30)})

        if parts and parts[0] == 'summary':
            sdays = max(1, min(int(qp('days', '7')), 90))
            return HandlerResult(status=200, body=alert_log.compute_summary(base, days=sdays))

        days = max(1, min(int(qp('days', '7')), 90))
        limit = max(1, min(int(qp('limit', '500')), 5000))
        type_filter = qp('type')

        events = alert_log.read_recent(base, days=days, type_filter=type_filter, limit=limit)
        return HandlerResult(status=200, body={
            'days': days,
            'count': len(events),
            'events': events,
        })
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


CIMS_ALERTS_HANDLER_LIST = [
    (_BASE, handle_alerts, {}),
]
