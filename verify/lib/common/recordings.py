"""녹취/PTT events 파일 카운트 helper.

cspsim 시나리오의 PASS 판정용 — 시나리오 시작 mtime 이후 만들어진
파일만 카운트하여 모듈 검증 잔여와 격리.

- `count_recordings`: VoIP/PTT 음성·영상 RTP 녹취 (`seg_*.rtp`).
- `count_ptt_events`: PTT 세션 floor/dtmf 이벤트 로그 (`events.jsonl`).
  CMP `ServiceLogging.MediaTypes` 가 RTP 녹취를 포함하지 않는 환경
  (PTT 음성 비녹취 모드)에서 시나리오 정상 진행 검증용.
"""
from __future__ import annotations

import os
from glob import glob

from .service_log import service_log_roots


def _glob_roots(dist_dir: str, *parts: str) -> list:
    """설정된 ServiceLogDir(들) 아래에서 패턴 매칭 — 기본 경로 가정 금지."""
    files = []
    for root in service_log_roots(dist_dir):
        files.extend(glob(os.path.join(root, *parts), recursive=True))
    return files


def count_recordings(dist_dir: str, since: float = 0.0) -> int:
    """ServiceLogDir 아래 `**/seg_*.rtp` 개수.
    `since>0` 이면 mtime >= since 인 파일만 카운트.
    """
    files = _glob_roots(dist_dir, "**", "seg_*.rtp")
    if since <= 0:
        return len(files)
    n = 0
    for f in files:
        try:
            if os.path.getmtime(f) >= since:
                n += 1
        except OSError:
            pass
    return n


def count_ptt_events(dist_dir: str, since: float = 0.0) -> int:
    """ServiceLogDir 아래 `ptt/**/events.jsonl` 개수.
    PTT 시나리오가 floor/dtmf 이벤트를 추가하면 mtime 이 갱신된다.
    `since>0` 이면 mtime >= since 인 파일만 카운트.
    """
    files = _glob_roots(dist_dir, "ptt", "**", "events.jsonl")
    if since <= 0:
        return len(files)
    n = 0
    for f in files:
        try:
            if os.path.getmtime(f) >= since:
                n += 1
        except OSError:
            pass
    return n
