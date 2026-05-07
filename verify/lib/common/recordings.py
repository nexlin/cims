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


def count_recordings(dist_dir: str, since: float = 0.0) -> int:
    """`{dist_dir}/ext_mnt/service_log/**/seg_*.rtp` 개수.
    `since>0` 이면 mtime >= since 인 파일만 카운트.
    """
    files = glob(os.path.join(dist_dir, "ext_mnt", "service_log",
                              "**", "seg_*.rtp"), recursive=True)
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
    """`{dist_dir}/ext_mnt/service_log/ptt/**/events.jsonl` 개수.
    PTT 시나리오가 floor/dtmf 이벤트를 추가하면 mtime 이 갱신된다.
    `since>0` 이면 mtime >= since 인 파일만 카운트.
    """
    files = glob(os.path.join(dist_dir, "ext_mnt", "service_log",
                              "ptt", "**", "events.jsonl"), recursive=True)
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
