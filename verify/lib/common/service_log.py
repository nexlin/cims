"""서비스 로그·녹취 경로 해석 helper — 사이트 디렉터리 영역(docs/design/features/site_directory_layout.md).

SIP flow·msg 는 **로그 영역**(`ServiceLogging.Dir` — 개발 기본 `<dist>/ext_mnt/log`, 그 아래 `sip/<연/월/일/시>/`),
녹취·세션 이력은 **녹취 영역**(`Recording.Dir` — 개발 기본 `<dist>/ext_mnt/recordings`)에 쌓인다. 경로는
`configure --site-dir/--service-log-dir/--record-dir` 로 바뀌고 운영 서버는 공유 NAS 경로를 쓰므로
(원격 CMP 와 OAM 이 같은 경로를 봐야 한다), 기본값을 가정하면 검증이 "파일 없음"으로 오판한다 — 서비스는
정상인데 카운터만 빈 디렉터리를 보는 상황. 그래서 dist 설정에서 실제 경로를 읽는다.
"""
from __future__ import annotations

import json
import os

# 설정에서 영역 경로를 읽을 대상 (CMP=녹취 기록 주체, CSP=세션이력/flow)
_CONFIGS = (
    ("cmp", "config", "cmp.json"),
    ("csp", "config", "csp.json"),
)


def _dir_from_config(path: str, section: str) -> str:
    """설정 파일에서 `<section>.Dir`(ServiceLogging·Recording) 추출. 없으면 ''."""
    try:
        with open(path) as f:
            cfg = json.load(f)
    except Exception:
        return ""
    # csp.json 은 최상위가 "Setup" 래퍼
    for scope in (cfg, cfg.get("Setup") or {}):
        if not isinstance(scope, dict):
            continue
        sl = scope.get(section)
        if isinstance(sl, dict) and sl.get("Dir"):
            return str(sl["Dir"])
    return ""


def _roots(dist_dir: str, section: str, default: str) -> list:
    roots = []
    for parts in _CONFIGS:
        d = _dir_from_config(os.path.join(dist_dir, *parts), section)
        if d and d not in roots:
            roots.append(d)
    if default not in roots:
        roots.append(default)
    return roots


def service_log_roots(dist_dir: str) -> list:
    """로그 영역 루트 후보 (존재하는 것만, 중복 제거) — SIP msg·flow 는 그 아래 `sip/`.

    설정된 경로를 우선하고 기본 경로(`<dist>/ext_mnt/log`)를 함께 둔다. S5 배포본 미디어 인스턴스
    (cmp/pmp/imp)는 사이트 경로를 주입받지 않으면 자기 트리의 상대 `service_log` 에 기록하므로 그 경로들도
    포함한다 (S6 시나리오의 녹취 delta 가 배포본 스택에서 잡히도록 — 단일 루트 레이아웃이라 녹취도 그 아래).
    """
    import glob as _glob
    roots = _roots(dist_dir, "ServiceLogging", os.path.join(dist_dir, "ext_mnt", "log"))
    # 배포본 (S5 verify 스택) — <agent>/modules/<mod>/current/<mod>/service_log
    for d in _glob.glob(os.path.join(dist_dir, "*-server", "modules", "*",
                                     "current", "*", "service_log")):
        rp = os.path.realpath(d)
        if rp not in [os.path.realpath(r) for r in roots]:
            roots.append(d)
    return [p for p in roots if os.path.isdir(p)]


def recording_roots(dist_dir: str) -> list:
    """녹취 영역 루트 후보 (존재하는 것만) — `volte/`·`ptt/<그룹>/` 이 그 아래.

    설정된 `Recording.Dir` 을 우선하고 기본 경로(`<dist>/ext_mnt/recordings`)와 로그 영역 루트(단일 루트
    레이아웃에서는 녹취가 로그 루트 아래다)를 함께 둔다."""
    roots = _roots(dist_dir, "Recording", os.path.join(dist_dir, "ext_mnt", "recordings"))
    for r in service_log_roots(dist_dir):
        if os.path.realpath(r) not in [os.path.realpath(x) for x in roots]:
            roots.append(r)
    return [p for p in roots if os.path.isdir(p)]
