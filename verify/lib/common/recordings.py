"""녹취 파일 카운트 helper.

cspsim 시나리오의 PASS 판정용 — 시나리오 시작 mtime 이후 만들어진
seg_*.rtp 만 카운트하여 모듈 검증 잔여와 격리.
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
