"""경로 해석 — **노드 로컬 자산**과 **관리 store** 를 분리한다 (oam_ha.md §4.0·§5).

관리평면 이중화에서 `CimsRuntimeDir`(관리 store)은 공유 마운트를 가리킨다. 그런데 시크릿·
인증서·CA 는 **볼륨에 두지 않는다** — 개인키를 복제/공유 스토리지에 올리지 않고 노드 로컬
0600 으로 두고 join 이 1회 복사하는 것이 설계다. 따라서 시크릿 경로는 `CimsRuntimeDir` 에서
유도하면 안 되고, **모듈 설치 트리의 버전무관 runtime**(`modules/oam/runtime`)에서 유도한다.

  modules/oam/runtime/              ← 노드 로컬 (업그레이드 생존)
    ├── _secrets/                   jwt_secret, ca/, agent_mtls/   (0700)
    └── cert/                       server.key, server.crt

  <shared>/runtime/  (= CimsRuntimeDir)   ← 공유 store, 리스 보유 노드만 write
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
