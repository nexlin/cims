"""이벤트(정상 동작 통지) 이력 — 파일 기반 (JSONL, 일별 회전)

알람(alert_log)과 스트림 분리 (alarm_self_reporting.md §6, alarm_standardization.md §3.6 —
X.730/731 stateChange · X.740 audit). 저장 경로: {ServiceLogDir}/events/YYYY/MM/DD.jsonl

레코드: { ts, type, kind(stateChange|audit), source{mo_class, mo_instance, detected_by},
         message, params }
"""

from typing import Optional

from services import daily_jsonl

_SUBDIR = 'events'


def record_event(service_log_dir: str, event: dict) -> None:
    daily_jsonl.record(service_log_dir, _SUBDIR, event)


def read_recent(service_log_dir: str, days: int = 7,
                type_filter: Optional[str] = None,
                kind_filter: Optional[str] = None,
                limit: int = 500) -> list:
    def _match(ev: dict) -> bool:
        if type_filter and ev.get('type') != type_filter:
            return False
        if kind_filter and ev.get('kind') != kind_filter:
            return False
        return True
    return daily_jsonl.read_recent(service_log_dir, _SUBDIR, days=days,
                                   match=_match, limit=limit)


def list_types(service_log_dir: str, days: int = 30) -> list:
    return daily_jsonl.list_values(service_log_dir, _SUBDIR, field='type', days=days)
