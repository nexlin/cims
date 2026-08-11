"""
Alert 이력 — 파일 기반 (JSONL, 일별 회전)

저장 경로: {ServiceLogDir}/alerts/YYYY/MM/DD.jsonl
(append/조회 코어는 daily_jsonl 공용 — event_log 와 공유)

각 라인은 알람 이벤트 — 발생(open)·해제(close)·승인(ack) 1건.
"""

from datetime import datetime, timedelta
from typing import Optional

from services import daily_jsonl

_SUBDIR = 'alerts'


def record_event(service_log_dir: str, event: dict) -> None:
    """이벤트 1건을 일별 jsonl 에 append. event 에 'ts' 없으면 현재시각으로 채움."""
    daily_jsonl.record(service_log_dir, _SUBDIR, event)
    # 라이브 통지(SSE) — 알람 스트림 변경 nudge (alarm_pipeline.md §8.2 P1). best-effort.
    try:
        from services.live_bus import LIVE_BUS
        LIVE_BUS.publish({'stream': 'alerts', 'record': event})
    except Exception:
        pass


def read_recent(service_log_dir: str, days: int = 7,
                type_filter: Optional[str] = None,
                limit: int = 500) -> list:
    """최근 N일치 alert 이벤트를 최신순으로 반환."""
    match = (lambda ev: ev.get('type') == type_filter) if type_filter else None
    return daily_jsonl.read_recent(service_log_dir, _SUBDIR, days=days,
                                   match=match, limit=limit)


def list_types(service_log_dir: str, days: int = 30) -> list:
    """최근 N일 내에 등장한 alert type 목록."""
    return daily_jsonl.list_values(service_log_dir, _SUBDIR, field='type', days=days)


def _iter_events_asc(service_log_dir: str, days: int):
    """최근 N일치 이벤트를 시간순(asc) yield."""
    return daily_jsonl.iter_asc(service_log_dir, _SUBDIR, days)


def _akey(ev: dict) -> str:
    """활성 알람 식별 키 = code@mo_instance. alarm_id(code@mo@epoch)에서 occurrence epoch 제거.
    구 레코드(alarm_id 없음)는 type 으로 폴백."""
    aid = ev.get('alarm_id')
    if aid:
        return aid.rsplit('@', 1)[0]
    return ev.get('type', '')


def compute_open_state(service_log_dir: str, days: int = 30,
                       with_meta: bool = False) -> dict:
    """최근 N일 이벤트 replay → 현재 열린 알람 반환 (sweeper/FM ingest 재시작 시드용).

    akey=(code@mo_instance). close 가 잇따른 open 은 덮어쓰고, close 없으면 open 유지.
    change(severity 변경) 는 열림 유지 + 현재 severity 갱신.
    반환: {akey: alarm_id}. with_meta=True 면
    {akey: {'alarm_id', 'detected_by', 'perceived_severity'}} — 발화 주체별 소유 분리
    (restore_open_state scope)와 재기동 후 change 판정 연속성에 쓴다.
    """
    open_state: dict = {}
    for ev in _iter_events_asc(service_log_dir, days):
        ak = _akey(ev)
        if not ak:
            continue
        action = ev.get('action')
        if action == 'open':
            aid = ev.get('alarm_id') or ak
            if with_meta:
                open_state[ak] = {'alarm_id': aid,
                                  'detected_by': (ev.get('source') or {}).get('detected_by') or '',
                                  'perceived_severity': ev.get('perceived_severity') or ev.get('severity')}
            else:
                open_state[ak] = aid
        elif action == 'change':
            if with_meta and ak in open_state:
                open_state[ak]['perceived_severity'] = \
                    ev.get('perceived_severity') or open_state[ak].get('perceived_severity')
        elif action == 'close':
            open_state.pop(ak, None)
    return open_state


def compute_summary(service_log_dir: str, days: int = 7) -> dict:
    """type 별 통계와 일별 발생량 집계.

    반환:
      {
        'days': N,
        'by_type': [
          {'type': str, 'opens': int, 'resolved': int, 'currently_open': bool,
           'avg_duration_sec': float|None, 'last_ts': str},
          ...
        ],
        'daily': [{'date': 'YYYY-MM-DD', 'opens': int}, ...]   # 오래된 → 최근
      }
    """
    by_type: dict = {}  # akey -> 집계 entry (활성 인스턴스 단위)
    open_ts: dict = {}  # akey -> open_ts (in-flight pair)
    daily: dict = {}    # 'YYYY-MM-DD' -> open count
    today = datetime.now().date()
    for i in range(days):
        d = today - timedelta(days=i)
        daily[d.isoformat()] = 0

    for ev in _iter_events_asc(service_log_dir, days):
        action = ev.get('action')
        if action in ('ack', 'comment'):   # 승인/코멘트는 통계 집계 대상 아님 (open/close 만)
            continue
        ak = _akey(ev)
        if not ak:
            continue
        ts = ev.get('ts', '')
        src = ev.get('source') or {}
        entry = by_type.setdefault(ak, {
            'key': ak,
            'type': ev.get('type'),
            'code': ev.get('code'),
            'mo_instance': src.get('mo_instance') or ak.split('@', 1)[-1],
            'perceived_severity': ev.get('perceived_severity') or ev.get('severity'),
            'opens': 0,
            'resolved': 0,
            'currently_open': False,
            'durations': [],
            'last_ts': '',
        })
        entry['last_ts'] = ts
        if action == 'change':   # severity 변경 — 발생/해소 카운트 없음, 현재 severity 만 갱신
            entry['perceived_severity'] = ev.get('perceived_severity') or entry['perceived_severity']
            continue
        if action == 'open':
            entry['opens'] += 1
            entry['currently_open'] = True
            entry['perceived_severity'] = ev.get('perceived_severity') or ev.get('severity') or entry['perceived_severity']
            open_ts[ak] = ts
            day = ts[:10]
            if day in daily:
                daily[day] += 1
        elif action == 'close':
            entry['resolved'] += 1
            entry['currently_open'] = False
            opened = open_ts.pop(ak, None)
            if opened:
                try:
                    o = datetime.fromisoformat(opened)
                    c = datetime.fromisoformat(ts)
                    sec = (c - o).total_seconds()
                    if sec >= 0:
                        entry['durations'].append(sec)
                except Exception:
                    pass

    out_by_type = []
    for ak, e in sorted(by_type.items()):
        durations = e.pop('durations')
        e['avg_duration_sec'] = round(sum(durations) / len(durations), 1) if durations else None
        out_by_type.append(e)

    daily_sorted = [{'date': k, 'opens': daily[k]} for k in sorted(daily.keys())]
    return {
        'days': days,
        'by_type': out_by_type,
        'daily': daily_sorted,
    }
