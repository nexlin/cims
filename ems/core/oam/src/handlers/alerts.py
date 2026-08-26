"""
CIMS Alert 이력 REST API

Routes:
  GET  /api/v1/alerts?days=7&type=&limit=500   최근 alert 이벤트 목록
  GET  /api/v1/alerts/types                    최근 30일 alert type 목록
  GET  /api/v1/alerts/summary?days=7           type별 통계 + 일별 발생량
  GET  /api/v1/alerts/rules                    활성 알림 규칙 + threshold (read-only, config 기반)
  GET  /api/v1/alerts/catalog                  알람 클래스 카탈로그 (rule + 모듈 자기보고)
  POST /api/v1/alerts/ack {alarm_id}           알람 승인 (32.111 acknowledgeAlarms)
  POST /api/v1/alerts/comment {alarm_id,text}  알람 코멘트 (32.111 setComment)
"""

from urllib.parse import urlparse, unquote
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
_SEV_ABBR = {'minor': 'min', 'major': 'maj', 'critical': 'crit'}


def _condition_text(rule: dict) -> str:
    chk = rule.get('check')
    ths = rule.get('thresholds')
    if isinstance(ths, dict) and ths:
        # 단계 임계 — 낮은 단계부터 "80(min)/90(maj)/95(crit)%" 형태로
        unit = rule.get('unit') or ''
        parts = sorted(((v, s) for s, v in ths.items()), key=lambda x: x[0])
        return '≥ ' + '/'.join(f"{v}({_SEV_ABBR.get(s, s)})" for v, s in parts) + unit
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
            'code': r.get('code'),
            'perceived_severity': r.get('perceived_severity') or r.get('severity', 'warning'),
            'severity': r.get('perceived_severity') or r.get('severity', 'warning'),  # 구 reader 호환
            'event_type': r.get('event_type'),
            'probable_cause': r.get('probable_cause'),
            'mo_class': r.get('mo_class'),
            'mo_instance': r.get('mo_instance'),
            'metric': r.get('metric') or r.get('type'),
            # target/check — 같은 code 의 probe 규칙(csp/cmp)을 화면에서 구분할 유일한 축.
            'target': r.get('target'),
            'check': r.get('check'),
            'condition': _condition_text(r),
            'threshold': r.get('threshold'),
            'thresholds': r.get('thresholds'),
            'unit': r.get('unit'),
            'effect': r.get('effect'),
            'recommended_action': r.get('recommended_action'),
            'scope': r.get('scope') or 'service',
        })
    return {'editable': False, 'sweep_sec': sweep, 'rules': out}


def _service_log_dir(config: dict) -> str:
    sl = config.get('ServiceLogging', {})
    d = sl.get('Dir', '')
    if not d:
        d = config.get('ServiceLogDir', config.get('MsgLogDir', ''))
    return d


def _parse_body(handler_args):
    import json as _json
    b = getattr(handler_args, 'body', None)
    if isinstance(b, dict):
        return b
    if isinstance(b, (bytes, bytearray)):
        try: return _json.loads(b.decode('utf-8'))
        except Exception: return {}
    if isinstance(b, str):
        try: return _json.loads(b)
        except Exception: return {}
    return {}


def _sse_stream() -> HandlerResult:
    """SSE(text/event-stream) 응답 — live_bus 구독 → 변경 레코드를 프레임으로 흘린다.

    각 프레임 `data: {"stream":"alerts|events","record":{...}}`. 20초 무변경 시 `: ping`
    하트비트로 연결 유지 + 죽은 연결 감지. 클라이언트 절단 시 generator 취소 → 구독 해제.
    """
    import asyncio
    import json as _json
    from starlette.responses import StreamingResponse
    from services.live_bus import LIVE_BUS

    loop = asyncio.get_running_loop()
    queue: asyncio.Queue = asyncio.Queue(maxsize=1000)
    sid = LIVE_BUS.subscribe(loop, queue)

    async def gen():
        try:
            yield b': connected\n\n'
            while True:
                try:
                    rec = await asyncio.wait_for(queue.get(), timeout=20)
                    yield f'data: {_json.dumps(rec, ensure_ascii=False)}\n\n'.encode('utf-8')
                except asyncio.TimeoutError:
                    yield b': ping\n\n'   # 하트비트 — keep-alive + 절단 감지
        finally:
            LIVE_BUS.unsubscribe(sid)

    resp = StreamingResponse(gen(), media_type='text/event-stream', headers={
        'Cache-Control': 'no-cache',
        'X-Accel-Buffering': 'no',   # nginx/게이트웨이 버퍼링 방지(스트리밍 통과)
        'Connection': 'keep-alive',
    })
    return HandlerResult(response=resp)


async def handle_alerts(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    method = handler_args.method.upper()
    base = _service_log_dir(config)
    parts = _path_parts(handler_args.full_path)
    # query string 은 full_path 가 아니라 query_params dict 로 전달된다 (이미 URL-decode).
    qs = handler_args.query_params or {}

    # 전 엔드포인트 인증 (파이프라인 §7) — 콘솔은 공용 api client 가 토큰을 동봉한다.
    from handlers.auth import require_auth
    token, autherr = require_auth(handler_args)
    if autherr:
        return autherr

    # 알람 승인(ack)/코멘트 — P1 라이프사이클 (32.111 acknowledgeAlarms/setComment).
    # alarm_id 에 '/'(mo_instance) 있어 body 로 전달.
    if method == 'POST' and parts and parts[0] in ('ack', 'comment'):
        try:
            body = _parse_body(handler_args)
            aid = (body.get('alarm_id') or '').strip()
            if not aid:
                return HandlerResult(status=400, body={'error': 'alarm_id required'})
            # actor 필수 — X.740 감사추적은 주체 불명(폴백 기명)을 허용하지 않는다.
            user = (token or {}).get('login_id')
            if not user:
                return HandlerResult(status=401, body={'error': '토큰에 사용자 식별이 없습니다'})
            from datetime import datetime as _dt
            akey = aid.rsplit('@', 1)[0]            # code@mo_instance
            code = akey.split('@', 1)[0]
            mo = akey.split('@', 1)[1] if '@' in akey else ''
            ts = _dt.now().isoformat(timespec='seconds')
            if parts[0] == 'comment':
                text = (body.get('text') or '').strip()
                if not text:
                    return HandlerResult(status=400, body={'error': 'text required'})
                alert_log.record_event(base, {
                    'ts': ts, 'alarm_id': aid, 'code': code, 'action': 'comment',
                    'comment': text[:500], 'comment_user': user, 'comment_time': ts,
                    'source': {'mo_instance': mo}, 'message': f'{user} 코멘트: {text[:500]}',
                })
                return HandlerResult(status=200, body={'ok': True, 'alarm_id': aid,
                                                       'comment_user': user, 'comment_time': ts})
            alert_log.record_event(base, {
                'ts': ts, 'alarm_id': aid, 'code': code, 'action': 'ack',
                'ack_state': 'acknowledged', 'ack_user': user, 'ack_time': ts,
                'source': {'mo_instance': mo}, 'message': f'{user} 승인',
            })
            return HandlerResult(status=200, body={'ok': True, 'alarm_id': aid, 'ack_user': user, 'ack_time': ts})
        except Exception as e:
            return HandlerResult(status=500, body={'error': str(e)})

    if method != 'GET':
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

    # 라이브 스트림(SSE) — 알람/이벤트 변경을 실시간 push (alarm_pipeline.md §8.2 P1).
    # 콘솔 대시보드 store 가 이 신호를 받아 /alerts·/events 를 즉시 재조회한다(라이브).
    # 인증은 위 require_auth 에서 이미 강제(fetch+ReadableStream 이 Authorization 헤더 동봉).
    if parts and parts[0] == 'stream':
        return _sse_stream()

    def qp(name, default=None):
        v = qs.get(name)
        return v if v not in (None, '') else default

    try:
        if parts and parts[0] == 'rules':
            return HandlerResult(status=200, body=_alert_rules(config))

        if parts and parts[0] == 'catalog':
            # 알람 클래스 카탈로그 (code 별 정의) — X.733/32.111 표준화.
            # OAM 평가 규칙(origin=rule) + 모듈 자기보고 등록분(origin=module:<module>,
            # alarm_self_reporting.md §4 — file_store 보존본이라 모듈 다운 중에도 표시).
            from services import service_registry, fm_ingest
            catalog = [{**c, 'origin': 'rule'} for c in service_registry.alarm_catalog(config)]
            seen = {c['code'] for c in catalog}
            for row in fm_ingest.module_catalogs(_service_log_dir(config)):
                for a in (row.get('alarms') or []):
                    code = a.get('code')
                    if not code or code in seen:
                        continue
                    seen.add(code)
                    catalog.append({
                        'code': code, 'type': a.get('type'),
                        'perceived_severity': a.get('perceived_severity'),
                        'event_type': a.get('event_type'),
                        'probable_cause': a.get('probable_cause'),
                        'mo_class': a.get('mo_class'), 'metric': a.get('metric'),
                        'effect': a.get('effect'),
                        'recommended_action': a.get('recommended_action'),
                        'origin': f"module:{row.get('module') or row.get('node')}",
                    })
            return HandlerResult(status=200, body={'catalog': catalog})

        if parts and parts[0] == 'types':
            return HandlerResult(status=200, body={'types': alert_log.list_types(base, days=30)})

        if parts and parts[0] == 'summary':
            sdays = max(1, min(int(qp('days', '7')), 90))
            return HandlerResult(status=200, body=alert_log.compute_summary(base, days=sdays))

        days = max(1, min(int(qp('days', '7')), 90))
        limit = max(1, min(int(qp('limit', '500')), 5000))
        type_filter = qp('type')

        events = alert_log.read_recent(base, days=days, type_filter=type_filter, limit=limit)
        # 표시용 이름 부착 — mo_instance 는 불변 id 루트(`a<id>`/`g<id>`)라 그대로는
        # 못 읽는다. 조회 시점에 해석하므로 이름을 바꿔도 **과거 레코드까지 현재 이름**
        # 으로 보인다(식별은 id, 표시는 이름 — 표준화 §3.4(b) DN + userLabel).
        try:
            from services import alarm_sweeper
            _label = alarm_sweeper.build_mo_label_resolver(config)
            for ev in events:
                src = ev.get('source')
                if isinstance(src, dict) and src.get('mo_instance'):
                    src['mo_label'] = _label(src['mo_instance'])
        except Exception:
            pass
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


# ── API 문서 (개발자 모드 [API] 배지) — 정본: docs/design/features/api_docs.md ──────────
# 조회·카탈로그만 선언한다. 평가 규칙(`/alerts/rules` — 임계·주기)은 내부 설정이라 제외,
# 승인/코멘트(POST)·SSE 스트림도 제외(변이·내부 전송 수단).
# `module=None` = **base 상주** — oam_app 이 base_rules 에 등록하므로 role=base 에서도 살아있다.
# (oam-svc 로 적으면 base 배포에서 가용 판정에 걸려 배지가 통째로 사라진다.)
_AUTH_MONITOR = {'scheme': 'bearer', 'role': 'monitor', 'token_from': 'POST /api/v1/auth/login'}

_ERR_COMMON = [
    {'status': 401, 'when': 'Authorization 헤더 없음 / 토큰 만료', 'body': {'error': 'unauthorized'}},
    {'status': 403, 'when': '권한 등급 미달', 'body': {'error': 'forbidden'}},
]

# 알람 레코드 1건의 공통 필드 — 목록 응답의 events[] 에 그대로 실린다 (X.733/32.111 정합).
_ALARM_FIELDS = [
    {'name': 'events[].ts', 'type': 'string', 'desc': '레코드 기록 시각 (ISO8601, 초 단위)'},
    {'name': 'events[].action', 'type': 'string', 'enum': ['open', 'close', 'change', 'ack', 'comment'],
     'desc': '발생/해소/심각도 변경/승인/코멘트 — 한 알람의 생애가 여러 레코드로 남는다'},
    {'name': 'events[].alarm_id', 'type': 'string',
     'desc': '알람 인스턴스 식별자 `code@mo_instance@epoch`. 앞 두 마디가 활성 알람 키'},
    {'name': 'events[].code', 'type': 'string', 'desc': '알람 코드 (A-XXX-NNN) — 카탈로그의 키'},
    {'name': 'events[].type', 'type': 'string', 'desc': '조건 클래스 (process_down/connection_lost/threshold_crossed 등)'},
    {'name': 'events[].perceived_severity', 'type': 'string',
     'enum': ['critical', 'major', 'minor', 'warning', 'indeterminate', 'cleared'],
     'desc': '인지 심각도 (X.733). 구 레코드는 severity 로도 실린다'},
    {'name': 'events[].event_type', 'type': 'string', 'desc': 'X.733 event type (communicationsAlarm 등)'},
    {'name': 'events[].probable_cause', 'type': 'string', 'desc': 'X.733 추정 원인'},
    {'name': 'events[].message', 'type': 'string', 'desc': '사람이 읽는 한 줄 설명'},
    {'name': 'events[].source.mo_class', 'type': 'string', 'desc': '대상 자원의 클래스 (service/node/module 등)'},
    {'name': 'events[].source.mo_instance', 'type': 'string',
     'desc': '대상 자원의 **불변 id** 경로 — 식별은 항상 이 값으로 한다'},
    {'name': 'events[].source.mo_label', 'type': 'string',
     'desc': '표시용 이름 — 조회 시점에 현재 이름으로 해석해 붙인다(과거 레코드도 현재 이름으로 보인다)'},
    {'name': 'events[].source.detected_by', 'type': 'string', 'desc': '감지 주체 (OAM 규칙 / 자기보고 모듈)'},
    {'name': 'events[].raised_time', 'type': 'string', 'desc': '발생 시각 (32.111 alarmRaisedTime)'},
    {'name': 'events[].clear_time', 'type': 'string', 'desc': '해소 시각 (alarmClearedTime)'},
    {'name': 'events[].change_time', 'type': 'string', 'desc': '심각도 변경 시각 (alarmChangedTime)'},
    {'name': 'events[].trend_indication', 'type': 'string', 'enum': ['moreSevere', 'lessSevere'],
     'desc': 'change 레코드에 동반되는 추세'},
    {'name': 'events[].threshold_info', 'type': 'object',
     'desc': '임계 계열 알람의 관측치 {observed, threshold, unit}'},
    {'name': 'events[].effect', 'type': 'string', 'desc': '영향'},
    {'name': 'events[].recommended_action', 'type': 'string', 'desc': '권고 조치'},
    {'name': 'events[].ack_state', 'type': 'string', 'enum': ['acknowledged', 'unacknowledged'],
     'desc': '승인 상태 (P1 라이프사이클)'},
]

_EX_ALARM = {
    'ts': '2026-01-02T09:15:00', 'action': 'open',
    'alarm_id': 'A-PRC-001@service/cims/module/csp@1767312900',
    'code': 'A-PRC-001', 'type': 'process_down', 'perceived_severity': 'critical',
    'event_type': 'processingErrorAlarm', 'probable_cause': 'softwareError',
    'message': 'csp 프로세스 응답 없음',
    'source': {'mo_class': 'module', 'mo_instance': 'service/cims/module/csp',
               'mo_label': 'CIMS / CSP', 'detected_by': 'oam.rule'},
    'raised_time': '2026-01-02T09:15:00',
}

CIMS_ALERTS_API_DOCS = [
    {'id': 'alerts.list', 'module': None, 'method': 'GET', 'path': '/api/v1/alerts',
     'summary': '알람 이력 — 발생/해소/심각도 변경/승인 레코드를 최신순으로 (활성 알람도 여기서 파생)',
     'params': [
         {'name': 'days', 'in': 'query', 'type': 'integer', 'required': False,
          'desc': '조회 일수. 기본 7, 허용 1~90 (범위 밖은 잘림)'},
         {'name': 'type', 'in': 'query', 'type': 'string', 'required': False,
          'desc': '조건 클래스로 필터 (`alerts.types` 로 값 목록 조회)'},
         {'name': 'limit', 'in': 'query', 'type': 'integer', 'required': False,
          'desc': '최대 건수. 기본 500, 허용 1~5000'},
     ],
     'response': '{days, count, events[]}',
     'response_fields': [
         {'name': 'days', 'type': 'integer', 'unit': '일', 'desc': '실제 적용된 조회 일수'},
         {'name': 'count', 'type': 'integer', 'unit': '건', 'desc': 'events 배열 길이'},
     ] + _ALARM_FIELDS,
     'example': {'days': 7, 'count': 1, 'events': [dict(_EX_ALARM)]},
     'errors': list(_ERR_COMMON),
     'notes': ['**활성 알람은 별도 엔드포인트가 아니다** — open 후 close 가 없는 alarm_id 가 활성이다.',
               '한 알람의 생애가 여러 레코드로 나뉜다(open→change→ack→close). 인스턴스 병합 키는 '
               'alarm_id 의 앞 두 마디(`code@mo_instance`).',
               '일자별 파일을 스캔하므로 days 를 키우면 응답이 느려진다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'alerts.summary', 'module': None, 'method': 'GET', 'path': '/api/v1/alerts/summary',
     'summary': '알람 집계 — 인스턴스별 발생/해소 횟수·평균 지속시간 + 일별 발생량',
     'params': [
         {'name': 'days', 'in': 'query', 'type': 'integer', 'required': False,
          'desc': '집계 일수. 기본 7, 허용 1~90'},
     ],
     'response': '{days, by_type[], daily[]}',
     'response_fields': [
         {'name': 'days', 'type': 'integer', 'unit': '일', 'desc': '실제 적용된 집계 일수'},
         {'name': 'by_type[].key', 'type': 'string', 'desc': '집계 단위 키 `code@mo_instance` (활성 인스턴스 단위)'},
         {'name': 'by_type[].type', 'type': 'string', 'desc': '조건 클래스'},
         {'name': 'by_type[].code', 'type': 'string', 'desc': '알람 코드'},
         {'name': 'by_type[].mo_instance', 'type': 'string', 'desc': '대상 자원의 불변 id 경로'},
         {'name': 'by_type[].perceived_severity', 'type': 'string', 'desc': '최신 심각도'},
         {'name': 'by_type[].opens', 'type': 'integer', 'unit': '건', 'desc': '기간 내 발생 횟수'},
         {'name': 'by_type[].resolved', 'type': 'integer', 'unit': '건', 'desc': '기간 내 해소 횟수'},
         {'name': 'by_type[].currently_open', 'type': 'boolean', 'desc': '지금 활성인가'},
         {'name': 'by_type[].avg_duration_sec', 'type': 'number', 'unit': '초',
          'desc': '발생→해소 평균 지속시간. 해소된 짝이 없으면 null'},
         {'name': 'by_type[].last_ts', 'type': 'string', 'desc': '마지막 레코드 시각'},
         {'name': 'daily[].date', 'type': 'string', 'desc': 'YYYY-MM-DD (오래된 → 최근)'},
         {'name': 'daily[].opens', 'type': 'integer', 'unit': '건', 'desc': '그 날 발생 건수'},
     ],
     'example': {'days': 7,
                 'by_type': [{'key': 'A-PRC-001@service/cims/module/csp', 'type': 'process_down',
                              'code': 'A-PRC-001', 'mo_instance': 'service/cims/module/csp',
                              'perceived_severity': 'critical', 'opens': 2, 'resolved': 2,
                              'currently_open': False, 'avg_duration_sec': 184.0,
                              'last_ts': '2026-01-02T09:18:04'}],
                 'daily': [{'date': '2026-01-01', 'opens': 0}, {'date': '2026-01-02', 'opens': 2}]},
     'errors': list(_ERR_COMMON),
     'notes': ['승인/코멘트 레코드는 집계 대상이 아니다 (발생/해소만 센다).',
               'daily 는 요청 일수만큼 **빈 날도 0 으로** 채워 돌려준다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'alerts.catalog', 'module': None, 'method': 'GET', 'path': '/api/v1/alerts/catalog',
     'summary': '알람 코드 사전 — code 별 정의(심각도·원인·영향·권고 조치)',
     'params': [],
     'response': '{catalog[]}',
     'response_fields': [
         {'name': 'catalog[].code', 'type': 'string', 'desc': '알람 코드 (A-XXX-NNN) — 알람 레코드의 code 와 짝'},
         {'name': 'catalog[].type', 'type': 'string', 'desc': '조건 클래스'},
         {'name': 'catalog[].perceived_severity', 'type': 'string', 'desc': '기본 인지 심각도'},
         {'name': 'catalog[].event_type', 'type': 'string', 'desc': 'X.733 event type'},
         {'name': 'catalog[].probable_cause', 'type': 'string', 'desc': 'X.733 추정 원인'},
         {'name': 'catalog[].mo_class', 'type': 'string', 'desc': '대상 자원 클래스'},
         {'name': 'catalog[].metric', 'type': 'string', 'desc': '임계 계열이면 관측 지표명'},
         {'name': 'catalog[].effect', 'type': 'string', 'desc': '영향'},
         {'name': 'catalog[].recommended_action', 'type': 'string', 'desc': '권고 조치'},
         {'name': 'catalog[].origin', 'type': 'string',
          'desc': "정의 출처 — 'rule'(OAM 평가 규칙) 또는 'module:<모듈>'(모듈 자기보고 등록분)"},
     ],
     'example': {'catalog': [
         {'code': 'A-PRC-001', 'type': 'process_down', 'perceived_severity': 'critical',
          'event_type': 'processingErrorAlarm', 'probable_cause': 'softwareError',
          'mo_class': 'module', 'effect': '해당 모듈 기능 중단',
          'recommended_action': '프로세스 상태·로그 확인 후 재기동', 'origin': 'rule'}]},
     'errors': list(_ERR_COMMON),
     'notes': ['모듈 자기보고 정의는 file_store 보존본이라 **그 모듈이 내려가 있어도** 목록에 남는다.',
               '같은 code 가 양쪽에 있으면 OAM 규칙 정의가 이긴다.'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'alerts.types', 'module': None, 'method': 'GET', 'path': '/api/v1/alerts/types',
     'summary': '최근 30일 내 등장한 알람 조건 클래스 목록 (필터 선택지)',
     'params': [],
     'response': '{types[]}',
     'response_fields': [
         {'name': 'types[]', 'type': 'string', 'desc': '조건 클래스 — `alerts.list` 의 type 파라미터에 그대로 쓴다'},
     ],
     'example': {'types': ['process_down', 'connection_lost', 'threshold_crossed']},
     'errors': list(_ERR_COMMON),
     'notes': ['기간은 30일 고정이다(파라미터 없음).'],
     'auth': dict(_AUTH_MONITOR)},
]
