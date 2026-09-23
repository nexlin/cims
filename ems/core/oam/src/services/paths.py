"""경로 해석 — **노드 로컬 자산**과 **사이트 디렉터리**를 분리한다 (oam_ha.md §4.0·§5,
site_directory_layout.md).

시크릿·인증서·CA 는 **사이트 디렉터리(공유 스토리지일 수 있다)에 두지 않는다** — 개인키를 복제/공유
스토리지에 올리지 않고 노드 로컬 0600 으로 두고 join 이 1회 복사하는 것이 설계다. 따라서 시크릿
경로는 사이트 설정에서 유도하지 않고 **모듈 설치 트리의 버전무관 runtime**(`modules/oam/runtime`)에서
유도한다.

  modules/oam/runtime/              ← 노드 로컬 (업그레이드 생존)
    ├── _secrets/                   jwt_secret, ca/, agent_mtls/   (0700)
    └── cert/                       server.key, server.crt

사이트 데이터는 **성격별 영역** 여덟 개로 나눈다. 운영자가 적는 값은 사이트 루트 `CimsSiteDir` 하나이고
영역 경로는 거기서 유도한다. 영역 키를 따로 적으면 그 값이 이긴다(예: 녹취만 다른 볼륨).

  <CimsSiteDir>/
    runtime/     관리 store            CimsRuntimeDir      control/ collections/ console/ …
    packages/    패키지 저장소          Packages.Dir        <모듈>-<버전>.tar.gz (+ .trash/)
    content/     서비스 콘텐츠          Content.Dir         announcements/ mcdata_fd/
    recordings/  녹취·통신 기록          Recording.Dir       volte/ ptt/<그룹>/ message/ message_direct/
    log/         관측 로그             ServiceLogging.Dir  sip/<연/월/일/시>/ alerts/ events/ fm_catalog/ leak_reclaim/
    stats/       통계·색인             Stats.Dir           1m/ 1h/ 1d/ 1M/ ptt_index/ ptt_attempts/
    state/       휘발성 상태            State.Dir           volte/ ptt/ (진행 중 세션) · 잠금 · write probe
    tester/      계측기 DataDir        (Tester.DataDir)    topologies/ scenarios/ runs/ samples/

`CimsSiteDir` 가 비어 있으면 **단일 루트 레이아웃**이다 — 옛 사이트·부트스트랩 기본값과 같은 위치를
그대로 유도한다(업그레이드만으로 경로가 옮겨지지 않는다):

  store    = CimsRuntimeDir > <CimsRuntimeMount>/runtime > 노드 로컬 modules/oam/runtime
  packages = <store>/pkg_files          log   = ServiceLogging.Dir > <노드 로컬 runtime>/service_log
  content·recordings = <log>            stats = <log>/stats           state = <log>/state

모듈은 자기 영역의 루트만 설정으로 받고 그 아래 고정 하위 이름만 쓴다. 이 파일이 영역 유도의 유일한
구현이다 — 실체화(`agents._materialize_deploy_config`)가 여기서 계산한 값을 각 모듈 `config.json` 에
적고, 부트스트랩(`install.sh`)도 이 모듈을 불러 같은 값을 쓴다.
"""
from __future__ import annotations

import os

_HERE = os.path.dirname(os.path.abspath(__file__))          # .../oam/src/services

# 영역 → base oam 배포설정 키 (flat dotted — 배포 overlay·config.json 의 형태)
AREA_KEYS = {
    'runtime': 'CimsRuntimeDir',
    'packages': 'Packages.Dir',
    'content': 'Content.Dir',
    'recordings': 'Recording.Dir',
    'log': 'ServiceLogging.Dir',
    'stats': 'Stats.Dir',
    'state': 'State.Dir',
}
# 영역 → 사이트 디렉터리 아래 이름
AREAS = ('runtime', 'packages', 'content', 'recordings', 'log', 'stats', 'state', 'tester')
# 영역을 정하는 입력 키 전부 — 이 중 하나라도 있으면 그 설정이 사이트 레이아웃을 정한다.
LAYOUT_KEYS = ('CimsSiteDir', 'CimsRuntimeMount') + tuple(AREA_KEYS.values())

# log 영역 안 — 모듈 공통 SIP/Flow 5분 버킷 (`sip/<연>/<월>/<일>/<시>/<sysid>*.{msg,flow}.<mm5>.jsonl`)
SIP_LOG_SUBDIR = 'sip'


def _norm(v) -> str:
    s = str(v or '').strip()
    return s.rstrip('/') if len(s) > 1 else s


def cfg_get(config: dict, dotted: str) -> str:
    """설정값 — 배포 overlay 의 flat 키(`Recording.Dir`)와 적재된 설정의 중첩 키(`Recording: {Dir}`) 둘 다."""
    cfg = config or {}
    if dotted in cfg and not isinstance(cfg.get(dotted), dict):
        return _norm(cfg.get(dotted))
    cur = cfg
    for part in dotted.split('.'):
        if not isinstance(cur, dict):
            return ''
        cur = cur.get(part)
    return '' if isinstance(cur, dict) else _norm(cur)


def local_runtime_dir(config: dict = None) -> str:
    """노드 로컬 버전무관 runtime 루트.

    우선순위: `CimsLocalRuntimeDir`(명시) → 모듈 트리 유도(`modules/oam/runtime`).
    dev(레포 직접 실행)에서는 `ems/core/oam/runtime` 이 된다 — 의도한 동작."""
    d = (config or {}).get('CimsLocalRuntimeDir')
    if d:
        return d
    # services → src → oam → <ver> → modules/oam  ⇒ modules/oam/runtime
    return os.path.normpath(os.path.join(_HERE, '..', '..', '..', '..', 'runtime'))


def secrets_dir(config: dict = None, create: bool = True) -> str:
    """시크릿 격리 디렉토리(0700) — **노드 로컬**. 볼륨/공유 스토리지에 두지 않는다."""
    d = os.path.join(local_runtime_dir(config), '_secrets')
    if create:
        try:
            os.makedirs(d, mode=0o700, exist_ok=True)
            os.chmod(d, 0o700)
        except Exception:
            pass
    return d


def site_dir(config: dict = None) -> str:
    """사이트 루트(`CimsSiteDir`). 빈 문자열 = 단일 루트 레이아웃."""
    return cfg_get(config, 'CimsSiteDir')


def runtime_store_dir(config: dict = None) -> str:
    """관리 store 루트 — `CimsRuntimeDir` > `<site>/runtime` > `<CimsRuntimeMount>/runtime` > 노드 로컬.

    `CimsRuntimeMount` 는 **선택** 항목이다: 공유 스토리지(NAS) 에 store 를 두는 이중화 사이트가
    "마운트가 빠진 채 기동해 로컬에 두 번째 store 를 만드는" 사고를 막으려고 mount guard
    (`oam_app._assert_runtime_mount`)에 알려 주는 값이지 위치 입력이 아니다(사이트 디렉터리가 있으면
    위치는 그쪽이 정한다). 비우면 검사도 없다."""
    explicit = cfg_get(config, 'CimsRuntimeDir')
    if explicit:
        return explicit
    site = site_dir(config)
    if site:
        return f'{site}/runtime'
    mnt = cfg_get(config, 'CimsRuntimeMount')
    if mnt:
        return f'{mnt}/runtime'
    return local_runtime_dir(config)


def packages_dir(config: dict = None) -> str:
    """패키지 저장소 — `Packages.Dir` > `<site>/packages` > `<store>/pkg_files`.

    절체한 노드도 같은 파일을 봐야 한다 — 사이트 디렉터리는 멤버 공통이고, 단일 루트 레이아웃에서는
    store 의 일부다(`/agent-bundle.tar.gz` 404 방지, oam_ha.md §4.1)."""
    explicit = cfg_get(config, 'Packages.Dir')
    if explicit:
        return explicit
    site = site_dir(config)
    if site:
        return f'{site}/packages'
    return os.path.join(runtime_store_dir(config), 'pkg_files')


def packages_backup_dir(config: dict = None) -> str:
    """삭제한 패키지 보관소 — `Packages.BackupDir` > (사이트) `<packages>/.trash` > `<store>/pkg_files_trash`."""
    explicit = cfg_get(config, 'Packages.BackupDir')
    if explicit:
        return explicit
    if site_dir(config):
        return os.path.join(packages_dir(config), '.trash')
    return os.path.join(runtime_store_dir(config), 'pkg_files_trash')


def service_log_dir(config: dict = None) -> str:
    """관측 로그 영역 — `ServiceLogging.Dir` > `<site>/log` > 노드 로컬 `runtime/service_log`.

    비운 채로 두면 로깅이 통째로 꺼져 부트스트랩 노드가 아무 기록도 남기지 않는다 — 그건 진단
    통로를 없애는 것이라 로컬로라도 남긴다."""
    explicit = cfg_get(config, 'ServiceLogging.Dir')
    if explicit:
        return explicit
    site = site_dir(config)
    if site:
        return f'{site}/log'
    return os.path.join(local_runtime_dir(config), 'service_log')


def sip_log_dir(config: dict = None) -> str:
    """SIP/Flow 5분 버킷 루트 — `<log>/sip` (모듈 공통 msg·flow jsonl)."""
    return os.path.join(service_log_dir(config), SIP_LOG_SUBDIR)


def _area_or(config: dict, area: str, legacy: str) -> str:
    explicit = cfg_get(config, AREA_KEYS[area])
    if explicit:
        return explicit
    site = site_dir(config)
    if site:
        return f'{site}/{area}'
    return legacy


def recordings_dir(config: dict = None) -> str:
    """녹취·통신 기록 영역 — `Recording.Dir` > `<site>/recordings` > `<log>`(단일 루트)."""
    return _area_or(config, 'recordings', service_log_dir(config))


def content_dir(config: dict = None) -> str:
    """서비스 콘텐츠 영역 — `Content.Dir` > `<site>/content` > `<log>`(단일 루트)."""
    return _area_or(config, 'content', service_log_dir(config))


def stats_dir(config: dict = None) -> str:
    """통계·색인 영역 — `Stats.Dir` > `<site>/stats` > `<log>/stats`(단일 루트)."""
    return _area_or(config, 'stats', os.path.join(service_log_dir(config), 'stats'))


def state_dir(config: dict = None) -> str:
    """휘발성 상태 영역 — `State.Dir` > `<site>/state` > `<log>/state`(단일 루트)."""
    return _area_or(config, 'state', os.path.join(service_log_dir(config), 'state'))


def tester_dir(config: dict = None) -> str:
    """계측기 DataDir 기본값 — `<site>/tester`. 단일 루트 레이아웃이면 빈 값(계측기 모듈 기본값)."""
    site = site_dir(config)
    return f'{site}/tester' if site else ''


_AREA_FN = {
    'runtime': runtime_store_dir,
    'packages': packages_dir,
    'content': content_dir,
    'recordings': recordings_dir,
    'log': service_log_dir,
    'stats': stats_dir,
    'state': state_dir,
    'tester': tester_dir,
}


def area_dir(config: dict, area: str) -> str:
    fn = _AREA_FN.get(area)
    return fn(config) if fn else ''


def area_dirs(config: dict = None) -> dict:
    """영역 → 경로 전부 (빈 값 = 그 레이아웃에서 해당 없음)."""
    return {a: area_dir(config, a) for a in AREAS}


def layout_values(config: dict) -> dict:
    """base oam `config.json` 에 구체값으로 적는 유도 경로(flat 키) — 실체화와 부트스트랩이 같은 모양을
    쓰게 하는 단일 정의(모양이 갈리면 설치 직후부터 설정 불일치 알람이 뜬다)."""
    out = {}
    for area, key in AREA_KEYS.items():
        v = area_dir(config, area)
        if v:
            out[key] = v
    if cfg_get(config, 'Packages.BackupDir') or site_dir(config):
        out['Packages.BackupDir'] = packages_backup_dir(config)
    return out


def decides_layout(config: dict) -> bool:
    """이 설정(보통 base oam 배포 overlay)이 사이트 레이아웃을 정하는 입력을 하나라도 갖는가."""
    return any(cfg_get(config, k) for k in LAYOUT_KEYS)
