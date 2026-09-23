"""경로 해석 — **노드 로컬 자산**과 **관리 store** 를 분리한다 (oam_ha.md §4.0·§5).

관리평면 이중화에서 관리 store(`CimsRuntimeMount` 에서 유도)는 공유 마운트를 가리킨다. 그런데 시크릿·
인증서·CA 는 **볼륨에 두지 않는다** — 개인키를 복제/공유 스토리지에 올리지 않고 노드 로컬
0600 으로 두고 join 이 1회 복사하는 것이 설계다. 따라서 시크릿 경로는 관리 store 에서
유도하면 안 되고, **모듈 설치 트리의 버전무관 runtime**(`modules/oam/runtime`)에서 유도한다.

관리 store 자체의 경로 규칙은 `runtime_store_dir`/`packages_dir` 이 정본이다 — 입력은
마운트 지점 하나이고 store·패키지 경로는 거기서 유도한다.

  modules/oam/runtime/              ← 노드 로컬 (업그레이드 생존)
    ├── _secrets/                   jwt_secret, ca/, agent_mtls/   (0700)
    └── cert/                       server.key, server.crt

  <마운트>/runtime/  (= 관리 store)       ← 공유 store, 리스 보유 노드만 write
    ├── control/ console/ ...             관리 store (file_store)
    └── .owner.json .owner.lock           소유권 리스
"""
from __future__ import annotations

import os

_HERE = os.path.dirname(os.path.abspath(__file__))          # .../oam/src/services


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


def service_log_dir(config: dict = None) -> str:
    """서비스 로그 루트 — 설정값이 있으면 그대로, **비어 있으면 노드 로컬**.

    `ServiceLogging.Dir` 은 보통 공유 스토리지를 가리키지만, **부트스트랩 직후에는 그
    경로가 없는 것이 정상**이다 — 공유 마운트를 붙이는 수단이 콘솔이고, 콘솔은 이 OAM 이
    서빙하기 때문이다. 그래서 패키지 기본값에는 공유 경로를 박지 않고(관리 store 와 같은
    규칙, oam_ha.md §5), 비어 있으면 여기서 노드 로컬로 해석한다. 공유 경로는
    마운트를 붙인 뒤 배포 overlay 가 정한다.

    비운 채로 두면 로깅이 통째로 꺼져 부트스트랩 노드가 아무 기록도 남기지 않는다 —
    그건 진단 통로를 없애는 것이라 로컬로라도 남긴다."""
    d = str(((config or {}).get('ServiceLogging') or {}).get('Dir') or '').strip()
    if d:
        return d
    legacy = str((config or {}).get('ServiceLogDir')
                 or (config or {}).get('MsgLogDir') or '').strip()
    if legacy:
        return legacy
    return os.path.join(local_runtime_dir(config), 'service_log')


def runtime_store_dir(config: dict = None) -> str:
    """관리 store 루트 — **경로 설정값이 정본**이다 (oam_ha.md §4.1).

      · `CimsRuntimeDir` 지정 → 그 경로. NAS 든 로컬 디스크든 운영 상황이 정한다 — 마운트는
        시스템/인프라의 일이고 store 위치는 그냥 설정이다. 패키지 = `{store}/pkg_files`.
      · 비었고 `CimsRuntimeMount` 지정 → `{마운트}/runtime` 로 유도(마운트만 적은 사이트의 기본값).
      · 둘 다 비움 → **노드 로컬**(`modules/oam/runtime`) — 부트스트랩 직후의 정상 상태.

    `CimsRuntimeMount` 는 **선택** 항목이다: 공유 스토리지(NAS) 에 store 를 두는 이중화 사이트가
    "마운트가 빠진 채 기동해 로컬에 두 번째 store 를 만드는" 사고를 막으려고 mount guard
    (`oam_app._assert_runtime_mount`)에 알려 주는 값이지 위치 입력이 아니다. 비우면 검사도 없다.
    실체화(`agents._materialize_deploy_config`)는 여기서 계산한 값을 `config.json` 에 적어
    모듈들이 같은 경로를 보게 한다."""
    cfg = config or {}
    mnt = str(cfg.get('CimsRuntimeMount') or '').strip().rstrip('/')
    explicit = str(cfg.get('CimsRuntimeDir') or '').strip().rstrip('/')
    if explicit:
        return explicit
    if mnt:
        return f'{mnt}/runtime'
    return local_runtime_dir(cfg)


def packages_dir(config: dict = None) -> str:
    """패키지 저장소 — **store 파생값**(`{store}/pkg_files`, oam_ha.md §4.1).

    패키지 파일은 관리 store 의 일부다(레코드는 store 에, 파일만 딴 데 두면 절체한 노드가
    `/agent-bundle.tar.gz` 를 404 로 돌려준다 = agent·모듈 설치/업그레이드 전면 불가)."""
    return os.path.join(runtime_store_dir(config), 'pkg_files')
