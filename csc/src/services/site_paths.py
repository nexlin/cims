"""사이트 영역 경로 — CSC 가 쓰는 영역(log·recordings·state·content)의 해석 (site_directory_layout.md).

배포본은 OAM 실체화가 base oam 사이트 디렉터리에서 유도한 경로를 config.json 에 넣는다(템플릿 `site_area`):

  ServiceLogging.Dir  log         CSC msg·flow 5분 버킷 `sip/<연>/<월>/<일>/<시>/`
  Recording.Dir       recordings  관제 앱 이력·녹취 범위 판정 원천 `volte/` `ptt/<그룹>/` `message/` `message_direct/`
  State.Dir           state       진행 중 세션 `volte/*.json` `ptt/*.json`
  Content.Dir         content     MCData FD 스토어 `mcdata_fd/` (McDataFd.Dir 로 따로 지정 가능)

값이 비어 있으면(소스 트리 실행·사이트 디렉터리 없는 사이트) **단일 루트 레이아웃**으로 해석한다 —
ems/core/oam/src/services/paths.py 와 같은 규칙이고, 이 모듈이 CSC 안의 유일한 해석 지점이다:

  log = 노드 로컬 `modules/csc/runtime/service_log`   recordings = content = <log>   state = <log>/state
"""
import os

_HERE = os.path.dirname(os.path.abspath(__file__))                   # csc/src/services
_COMPONENT_ROOT = os.path.normpath(os.path.join(_HERE, '..', '..'))  # install_path/csc

SIP_LOG_SUBDIR = 'sip'


def _get(config: dict, dotted: str) -> str:
    """설정값 — 적재된 설정의 중첩 키(`Recording: {Dir}`)와 flat 키(`Recording.Dir`) 둘 다."""
    cfg = config or {}
    v = cfg.get(dotted)
    if v is None or isinstance(v, dict):
        cur = cfg
        for part in dotted.split('.'):
            cur = cur.get(part) if isinstance(cur, dict) else None
            if cur is None:
                break
        v = cur
    if isinstance(v, dict):
        return ''
    s = str(v or '').strip()
    return s.rstrip('/') if len(s) > 1 else s


def log_dir(config: dict) -> str:
    """관측 로그 영역 — `ServiceLogging.Dir` > 노드 로컬 `<모듈>/runtime/service_log`.
    비운 채로 두면 로깅이 통째로 꺼진다 — 진단 통로를 없애지 않도록 로컬로라도 남긴다."""
    return _get(config, 'ServiceLogging.Dir') or \
        os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', '..', 'runtime', 'service_log'))


def sip_log_dir(config: dict) -> str:
    """SIP/Flow 5분 버킷 루트 — `<log>/sip`."""
    return os.path.join(log_dir(config), SIP_LOG_SUBDIR)


def recordings_dir(config: dict) -> str:
    """녹취·통신 기록 영역 — `Recording.Dir` > `<log>`(단일 루트). 녹취 id 는 이 루트 기준 상대 경로다."""
    return _get(config, 'Recording.Dir') or log_dir(config)


def state_dir(config: dict) -> str:
    """휘발성 상태 영역 — `State.Dir` > `<log>/state`(단일 루트)."""
    return _get(config, 'State.Dir') or os.path.join(log_dir(config), 'state')


def content_dir(config: dict) -> str:
    """서비스 콘텐츠 영역 — `Content.Dir` > `<log>`(단일 루트)."""
    return _get(config, 'Content.Dir') or log_dir(config)


def mcdata_fd_dir(config: dict) -> str:
    """MCData FD 스토어 — `McDataFd.Dir`(명시) > `<content>/mcdata_fd`. CMDP 와 같은 경로여야 한다."""
    return _get(config, 'McDataFd.Dir') or os.path.join(content_dir(config), 'mcdata_fd')
