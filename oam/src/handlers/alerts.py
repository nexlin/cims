"""
CIMS Alert 이력 REST API

Routes:
  GET /api/v1/alerts?days=7&type=&limit=500    최근 alert 이벤트 목록
  GET /api/v1/alerts/types                     최근 30일 alert type 목록
  GET /api/v1/alerts/summary?days=7            type별 통계 + 일별 발생량
  GET /api/v1/alerts/rules                     활성 알림 규칙 + threshold (read-only, config 기반)
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


# 활성 알림 규칙 — service descriptor(service_registry.alert_rules) 구동. sweeper 발화 조건과 1:1.
# descriptor 비었을 때만 하드코딩 fallback (전환 안전망).
def _condition_text(rule: dict) -> str:
    chk = rule.get('check')
    if chk in ('rtp_pct_gte', 'disk_high'):
        return f"≥ {rule.get('threshold')}{rule.get('unit') or ''}"
    if chk == 'db_down':
        return '연결 끊김'
    if chk == 'module_down':
        return '프로세스 중단'
    return '응답 없음'


def _alert_rules(config: dict) -> dict:
    from services import service_registry
    sweep = int(config.get('AlertSweepSec', 30))
    rules = service_registry.alert_rules(config)
    out = []
    for r in rules:
        out.append({
            'type': r.get('type'),
            'severity': r.get('severity', 'warning'),
            'metric': r.get('metric') or r.get('type'),
            'condition': _condition_text(r),
            'threshold': r.get('threshold'),
            'unit': r.get('unit'),
            'scope': r.get('scope') or 'service',
        })
    if not out:   # descriptor 비었을 때 fallback
        rtp_pct = int(config.get('AlertRtpThresholdPct', 80))
        out = [
            {'type': 'csp_down', 'severity': 'critical', 'metric': 'CSP 프로세스', 'condition': '응답 없음', 'threshold': None, 'unit': None},
            {'type': 'cmp_down', 'severity': 'critical', 'metric': 'CMP 프로세스', 'condition': '응답 없음', 'threshold': None, 'unit': None},
            {'type': 'db_down', 'severity': 'critical', 'metric': 'DB 연결', 'condition': '연결 끊김', 'threshold': None, 'unit': None},
            {'type': 'rtp_high', 'severity': 'warning', 'metric': 'RTP 포트 사용률', 'condition': f'≥ {rtp_pct}%', 'threshold': rtp_pct, 'unit': '%'},
        ]
    return {'editable': False, 'sweep_sec': sweep, 'rules': out}


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
        if parts and parts[0] == 'rules':
            return HandlerResult(status=200, body=_alert_rules(config))

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
