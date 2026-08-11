"""In-process 알람/이벤트 실시간 브로커 (alarm_pipeline.md §8.2 P1 — 라이브 통지 백엔드).

writer(FM ingest·sweeper 스레드)가 `publish()` 하고, SSE 소비자(HTTP asyncio 루프)가
구독한다. writer 와 소비자는 같은 프로세스지만 서로 다른 스레드/이벤트루프라, 각 구독자는
자기 asyncio 루프와 Queue 를 등록하고 publish 는 `loop.call_soon_threadsafe()` 로 크로스-스레드
안전 핸드오프한다. 큐가 가득 차면(소비자 지연) 최신을 위해 조용히 드롭한다 — 콘솔은 이 신호를
"변경 발생" nudge 로 받아 `/alerts`·`/events` 를 재조회하므로 개별 레코드 유실은 무해하다.
"""

import threading


def _safe_put(queue, record) -> None:
    # 루프 스레드에서 실행 — 큐가 가득 차면 드롭(무해, nudge 성격).
    try:
        queue.put_nowait(record)
    except Exception:
        pass


class LiveBus:
    def __init__(self):
        self._lock = threading.Lock()
        self._subs = {}          # sid -> (loop, queue)
        self._seq = 0

    def subscribe(self, loop, queue) -> int:
        with self._lock:
            self._seq += 1
            sid = self._seq
            self._subs[sid] = (loop, queue)
            return sid

    def unsubscribe(self, sid: int) -> None:
        with self._lock:
            self._subs.pop(sid, None)

    def publish(self, record: dict) -> None:
        """writer 스레드에서 호출 — 전 구독자 큐에 fan-out (크로스-스레드 안전)."""
        with self._lock:
            subs = list(self._subs.values())
        for loop, queue in subs:
            try:
                loop.call_soon_threadsafe(_safe_put, queue, record)
            except Exception:
                pass   # 루프 종료 등 — 무시

    def subscriber_count(self) -> int:
        with self._lock:
            return len(self._subs)


# 프로세스 단일 인스턴스 — writer/소비자가 공유.
LIVE_BUS = LiveBus()
