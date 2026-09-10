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
    """관리 store 루트 — **마운트 지점 하나에서 유도**한다 (oam_ha.md §4.1).

    운영자가 정하는 값은 `CimsRuntimeMount` 하나다:

      · 지정 → store = `{마운트}/runtime`, 패키지 = `{store}/pkg_files`
      · 비움 → **노드 로컬**(`modules/oam/runtime`) — 부트스트랩 직후의 정상 상태다
        (공유 스토리지를 붙이는 수단이 이 OAM 이 서빙하는 콘솔이므로, 설치 시점에는
        마운트가 없는 것이 당연하다)

    `CimsRuntimeDir` 은 그 **유도 결과**다. 실체화(`agents._materialize_deploy_config`)가
    계산해 `config.json` 에 적어 넣으므로 모듈들은 종전대로 그 키를 읽으면 되고, 여기서는
    두 경우에만 명시값을 존중한다:

      · 마운트가 없는 구성 — 노드 로컬 경로를 실체화가 채워 준 값(또는 dev/시험 override)
      · 마운트 하위의 **다른** 경로를 store 로 쓰던 전환기 사이트 — 유도값으로 덮으면
        OAM 이 빈 경로를 store 로 잡아 관리 데이터를 통째로 잃은 것처럼 보인다.
        (정규화는 이관(`POST /ha-groups/{id}/shared-store/migrate`)이 한다)

    마운트 밖을 가리키는 명시값은 **무시**한다 — 마운트를 바꾼 뒤 옛 경로가 남아 있으면
    mount guard 가 기동을 거부하는데(store 가 마운트 하위가 아님), 그 값은 이미 유효하지
    않은 유도 결과이지 운영자의 선택이 아니다."""
    cfg = config or {}
    mnt = str(cfg.get('CimsRuntimeMount') or '').strip().rstrip('/')
    explicit = str(cfg.get('CimsRuntimeDir') or '').strip().rstrip('/')
    if mnt:
        derived = f'{mnt}/runtime'
        if explicit and explicit != derived and \
                (explicit == mnt or explicit.startswith(mnt + '/')):
            return explicit
        return derived
    return explicit or local_runtime_dir(cfg)


def packages_dir(config: dict = None) -> str:
    """패키지 저장소 — **store 파생값**(`{store}/pkg_files`, oam_ha.md §4.1).

    패키지 파일은 관리 store 의 일부다(레코드는 store 에, 파일만 딴 데 두면 절체한 노드가
    `/agent-bundle.tar.gz` 를 404 로 돌려준다 = agent·모듈 설치/업그레이드 전면 불가)."""
    return os.path.join(runtime_store_dir(config), 'pkg_files')
