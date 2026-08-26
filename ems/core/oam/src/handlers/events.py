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


# ── API 문서 (개발자 모드 [API] 배지) — 정본: docs/design/features/api_docs.md ──────────
_AUTH_MONITOR = {'scheme': 'bearer', 'role': 'monitor', 'token_from': 'POST /api/v1/auth/login'}

_ERR_COMMON = [
    {'status': 401, 'when': 'Authorization 헤더 없음 / 토큰 만료', 'body': {'error': 'unauthorized'}},
    {'status': 403, 'when': '권한 등급 미달', 'body': {'error': 'forbidden'}},
]

CIMS_EVENTS_API_DOCS = [
    {'id': 'events.list', 'module': None, 'method': 'GET', 'path': '/api/v1/events',
     'summary': '이벤트(정상 동작 통지) 이력 — 알람이 아닌 상태 변화·조치 기록을 최신순으로',
     'params': [
         {'name': 'days', 'in': 'query', 'type': 'integer', 'required': False,
          'desc': '조회 일수. 기본 7, 허용 1~90 (범위 밖은 잘림)'},
         {'name': 'type', 'in': 'query', 'type': 'string', 'required': False,
          'desc': '이벤트 종류로 필터 (`events.types` 로 값 목록 조회)'},
         {'name': 'kind', 'in': 'query', 'type': 'string', 'required': False,
          'desc': '분류로 필터 — 상태변경 통지(STC) / 감사기록(AUD)'},
         {'name': 'limit', 'in': 'query', 'type': 'integer', 'required': False,
          'desc': '최대 건수. 기본 500, 허용 1~5000'},
     ],
     'response': '{days, count, events[]}',
     'response_fields': [
         {'name': 'days', 'type': 'integer', 'unit': '일', 'desc': '실제 적용된 조회 일수'},
         {'name': 'count', 'type': 'integer', 'unit': '건', 'desc': 'events 배열 길이'},
         {'name': 'events[].ts', 'type': 'string', 'desc': '기록 시각 (ISO8601, 초 단위)'},
         {'name': 'events[].kind', 'type': 'string', 'enum': ['STC', 'AUD'],
          'desc': 'STC=상태변경 통지, AUD=감사기록(누가 무엇을 했나)'},
         {'name': 'events[].type', 'type': 'string', 'desc': '이벤트 종류'},
         {'name': 'events[].code', 'type': 'string', 'desc': '이벤트 코드 (E-XXX-NNN)'},
         {'name': 'events[].message', 'type': 'string', 'desc': '사람이 읽는 한 줄 설명'},
         {'name': 'events[].source.mo_class', 'type': 'string', 'desc': '대상 자원의 클래스'},
         {'name': 'events[].source.mo_instance', 'type': 'string', 'desc': '대상 자원의 **불변 id** 경로'},
         {'name': 'events[].source.detected_by', 'type': 'string', 'desc': '기록 주체'},
         {'name': 'events[].user', 'type': 'string', 'desc': '감사기록(AUD)의 행위자 계정'},
     ],
     'example': {'days': 7, 'count': 1, 'events': [
         {'ts': '2026-01-02T09:20:11', 'kind': 'STC', 'type': 'module_started',
          'code': 'E-PRC-001', 'message': 'csp 기동 완료',
          'source': {'mo_class': 'module', 'mo_instance': 'service/cims/module/csp',
                     'detected_by': 'agent'}}]},
     'errors': list(_ERR_COMMON),
     'notes': ['알람과 **스트림이 분리**돼 있다 — 장애는 `/api/v1/alerts`, 정상 통지는 여기.',
               '일자별 파일을 스캔하므로 days 를 키우면 응답이 느려진다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'events.types', 'module': None, 'method': 'GET', 'path': '/api/v1/events/types',
     'summary': '최근 30일 내 등장한 이벤트 종류 목록 (필터 선택지)',
     'params': [],
     'response': '{types[]}',
     'response_fields': [
         {'name': 'types[]', 'type': 'string', 'desc': '이벤트 종류 — `events.list` 의 type 파라미터에 그대로 쓴다'},
     ],
     'example': {'types': ['module_started', 'config_changed', 'failover']},
     'errors': list(_ERR_COMMON),
     'notes': ['기간은 30일 고정이다(파라미터 없음).'],
     'auth': dict(_AUTH_MONITOR)},
]
