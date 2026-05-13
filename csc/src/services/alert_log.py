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
