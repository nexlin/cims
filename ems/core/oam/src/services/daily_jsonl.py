"""일별 회전 JSONL 스토어 공용 코어 — {root}/{subdir}/YYYY/MM/DD.jsonl

alert_log(알람)·event_log(이벤트)가 공유하는 append/조회 헬퍼.
각 라인은 free-form JSON 레코드 1건 — 'ts'(ISO8601) 없으면 기록 시각으로 채운다.
"""

import os
import json
from datetime import datetime, timedelta
from threading import Lock
from typing import Callable, Optional

_write_lock = Lock()


def _file_for(root: str, subdir: str, dt: datetime) -> str:
    return os.path.join(root, subdir, f"{dt.year:04d}", f"{dt.month:02d}", f"{dt.day:02d}.jsonl")


def record(root: str, subdir: str, event: dict) -> None:
    """레코드 1건을 일별 jsonl 에 append. 'ts' 없으면 현재시각으로 채움."""
    if not root:
        return
    ts = event.get('ts')
    if not ts:
        ts = datetime.now().isoformat(timespec='seconds')
        event = {**event, 'ts': ts}
    try:
        dt = datetime.fromisoformat(ts)
    except Exception:
        dt = datetime.now()
    path = _file_for(root, subdir, dt)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    line = json.dumps(event, ensure_ascii=False)
    with _write_lock:
        with open(path, 'a', encoding='utf-8') as f:
            f.write(line + '\n')


def iter_asc(root: str, subdir: str, days: int):
    """최근 N일치 레코드를 시간순(asc) yield."""
    if not root:
        return
    today = datetime.now().date()
    files = []
    for i in range(days):
        d = today - timedelta(days=i)
        path = _file_for(root, subdir, datetime(d.year, d.month, d.day))
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


def read_recent(root: str, subdir: str, days: int = 7,
                match: Optional[Callable[[dict], bool]] = None,
                limit: int = 500) -> list:
    """최근 N일치 레코드를 최신순으로 반환. match 는 레코드 필터 callable."""
    results = []
    for ev in iter_asc(root, subdir, days):
        if match and not match(ev):
            continue
        results.append(ev)
    results.sort(key=lambda x: x.get('ts', ''), reverse=True)
    return results[:limit]


def list_values(root: str, subdir: str, field: str = 'type', days: int = 30) -> list:
    """최근 N일 내에 등장한 필드 값 목록."""
    values = set()
    for ev in iter_asc(root, subdir, days):
        v = ev.get(field)
        if v:
            values.add(v)
    return sorted(values)
