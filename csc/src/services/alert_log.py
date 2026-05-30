"""
Alert 이력 — 파일 기반 (JSONL, 일별 회전)

저장 경로: {ServiceLogDir}/alerts/YYYY/MM/DD.jsonl

각 라인은 임계값 위반 이벤트 — 발생(open) 또는 해제(close) 1건.
"""

import os
import json
import glob
from datetime import datetime, timedelta
from threading import Lock
from typing import Optional


_write_lock = Lock()


def _alerts_dir(service_log_dir: str) -> str:
    return os.path.join(service_log_dir, 'alerts')


def _file_for(service_log_dir: str, dt: datetime) -> str:
    return os.path.join(
        _alerts_dir(service_log_dir),
        f"{dt.year:04d}",
        f"{dt.month:02d}",
        f"{dt.day:02d}.jsonl",
    )


def record_event(service_log_dir: str, event: dict) -> None:
    """이벤트 1건을 일별 jsonl 에 append. event 에 'ts' 없으면 현재시각으로 채움."""
    if not service_log_dir:
        return
    ts = event.get('ts')
    if not ts:
        ts = datetime.now().isoformat(timespec='seconds')
        event = {**event, 'ts': ts}
    try:
        dt = datetime.fromisoformat(ts)
    except Exception:
        dt = datetime.now()
    path = _file_for(service_log_dir, dt)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    line = json.dumps(event, ensure_ascii=False)
    with _write_lock:
        with open(path, 'a', encoding='utf-8') as f:
            f.write(line + '\n')


def read_recent(service_log_dir: str, days: int = 7,
                type_filter: Optional[str] = None,
                limit: int = 500) -> list:
    """최근 N일치 alert 이벤트를 최신순으로 반환."""
    if not service_log_dir:
        return []
    results = []
    today = datetime.now().date()
    for i in range(days):
        d = today - timedelta(days=i)
        path = _file_for(service_log_dir, datetime(d.year, d.month, d.day))
        if not os.path.exists(path):
            continue
        try:
            with open(path, 'r', encoding='utf-8') as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        ev = json.loads(line)
                    except Exception:
                        continue
                    if type_filter and ev.get('type') != type_filter:
                        continue
                    results.append(ev)
        except Exception:
            pass
    results.sort(key=lambda x: x.get('ts', ''), reverse=True)
    return results[:limit]


def list_types(service_log_dir: str, days: int = 30) -> list:
    """최근 N일 내에 등장한 alert type 목록."""
    types = set()
    if not service_log_dir:
        return []
    today = datetime.now().date()
    for i in range(days):
        d = today - timedelta(days=i)
        path = _file_for(service_log_dir, datetime(d.year, d.month, d.day))
        if not os.path.exists(path):
            continue
        try:
            with open(path, 'r', encoding='utf-8') as f:
                for line in f:
                    try:
                        ev = json.loads(line.strip())
                        t = ev.get('type')
                        if t:
                            types.add(t)
                    except Exception:
                        pass
        except Exception:
            pass
    return sorted(types)


def _iter_events_asc(service_log_dir: str, days: int):
    """최근 N일치 이벤트를 시간순(asc) yield."""
    if not service_log_dir:
        return
    today = datetime.now().date()
    files = []
    for i in range(days):
        d = today - timedelta(days=i)
        path = _file_for(service_log_dir, datetime(d.year, d.month, d.day))
        if os.path.exists(path):
            files.append(path)
    files.sort()  # 일별 파일은 경로 정렬 = 시간 정렬
    for path in files:
        try:
            with open(path, 'r', encoding='utf-8') as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        yield json.loads(line)
                    except Exception:
                        continue
        except Exception:
            continue


def _akey(ev: dict) -> str:
    """활성 알람 식별 키 = code@mo_instance. alarm_id(code@mo@epoch)에서 occurrence epoch 제거.
    구 레코드(alarm_id 없음)는 type 으로 폴백."""
    aid = ev.get('alarm_id')
    if aid:
        return aid.rsplit('@', 1)[0]
    return ev.get('type', '')


def compute_open_state(service_log_dir: str, days: int = 30) -> dict:
    """최근 N일 이벤트 replay → 현재 열린 알람 {akey: alarm_id} 반환 (sweeper 재시작 시드용).

    akey=(code@mo_instance). close 가 잇따른 open 은 덮어쓰고, close 없으면 open 유지.
    """
    open_state: dict = {}
    for ev in _iter_events_asc(service_log_dir, days):
        ak = _akey(ev)
        if not ak:
            continue
        action = ev.get('action')
        if action == 'open':
            open_state[ak] = ev.get('alarm_id') or ak
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
        ak = _akey(ev)
        if not ak:
            continue
        ts = ev.get('ts', '')
        action = ev.get('action')
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
