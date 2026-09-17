"""계측기 라이브 이벤트 버스 — run 상태 변화·워커 헬스·1초 요약을 SSE 구독자에게 fan-out.

oam/src `services.live_bus.LiveBus` 를 그대로 재사용한다(크로스-스레드 핸드오프 규약 동일).
프레임 = {"stream": "runs|workers|agg", "record": {...}} — 콘솔은 stream 으로 분기한다.
"""
from services.live_bus import LiveBus

TESTER_BUS = LiveBus()


def publish(stream: str, record: dict) -> None:
    TESTER_BUS.publish({'stream': stream, 'record': record})
