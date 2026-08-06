"""서비스 로그(ServiceLogDir) 경로 해석 helper.

녹취·세션이력·SIP flow 는 모두 **설정된** ServiceLogDir 아래에 쌓인다. 기본값은
`<dist>/ext_mnt/service_log` 지만 `configure --service-log-dir` 로 바꿀 수 있고,
실제로 개발/운영 서버는 공유 NAS 경로(예: `/mnt/cims/log48`)를 쓴다
(docs/design/features/recording.md §3.6.1 — 원격 CMP 와 OAM 이 같은 경로를 봐야 한다).

경로를 기본값으로 가정하면 검증이 "파일 없음"으로 오판한다 — 서비스는 정상인데
카운터만 빈 디렉터리를 보는 상황. 그래서 dist 설정에서 실제 경로를 읽는다.
"""
from __future__ import annotations

import json
import os

# 설정에서 ServiceLogging.Dir 를 읽을 대상 (CMP=녹취 기록 주체, CSP=세션이력/flow)
_CONFIGS = (
    ("cmp", "config", "cmp.json"),
    ("csp", "config", "csp.json"),
)


def _dir_from_config(path: str) -> str:
    """설정 파일에서 ServiceLogging.Dir 추출. 없으면 ''."""
    try:
        with open(path) as f:
            cfg = json.load(f)
    except Exception:
        return ""
    # csp.json 은 최상위가 "Setup" 래퍼
    for scope in (cfg, cfg.get("Setup") or {}):
        if not isinstance(scope, dict):
            continue
        sl = scope.get("ServiceLogging")
        if isinstance(sl, dict) and sl.get("Dir"):
            return str(sl["Dir"])
    return ""


def service_log_roots(dist_dir: str) -> list:
    """서비스 로그 루트 후보 (존재하는 것만, 중복 제거).

    설정된 경로를 우선하고 기본 경로(`<dist>/ext_mnt/service_log`)를 함께 둔다 —
    설정을 바꾸기 전 남은 산출물도 계속 집계되도록.
    """
    roots = []
    for parts in _CONFIGS:
        d = _dir_from_config(os.path.join(dist_dir, *parts))
        if d and d not in roots:
            roots.append(d)
    default = os.path.join(dist_dir, "ext_mnt", "service_log")
    if default not in roots:
        roots.append(default)
    return [p for p in roots if os.path.isdir(p)]
