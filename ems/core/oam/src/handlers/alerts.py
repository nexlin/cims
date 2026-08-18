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
