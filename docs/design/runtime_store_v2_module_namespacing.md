# Runtime Store v2 — 모듈/버전 귀속 네임스페이스 설계

> [runtime_store_design.md](runtime_store_design.md)(평면 file-store 원설계)를 모듈/버전 귀속
> 네임스페이스로 개정한 현행 레이아웃이다.
> 관련: [oam_csc_split.md](oam_csc_split.md) · [features/sip_runtime_config.md](features/sip_runtime_config.md) ·
> [features/package_and_template.md](features/package_and_template.md)

## 1. 설계 원칙

OAM runtime store 는 모듈/버전 귀속 네임스페이스를 따른다. 평면 단일 네임스페이스
(`{CimsRuntimeDir}/<domain>/`)가 야기하던 다음 결함을 회피하기 위함이다.

1. **모듈 간 이름 충돌 방지** — 여러 모듈이 같은 이름 컬렉션(`rules`/`routes`/`services` 등)을
   가질 수 있으므로, 컬렉션 SoT 를 모듈 네임스페이스로 분리하고 `(owner, name)` 유일성을 강제한다.
2. **소유권 정합** — 모듈 컬렉션 SoT 는 그 모듈의 소유 공간(`modules/<owner>/runtime/`)에 둔다.
   OAM 디렉터리에 타 모듈 데이터를 두지 않는다.
3. **라이프사이클 결합** — 모듈 runtime 디렉터리는 그 모듈 배포 시에만 생성한다. 읽기만으로는
   디렉터리를 만들지 않는다(읽기 부수효과 생성 제거).
4. **버전 귀속/스키마 정합** — SoT 레코드(또는 도메인)에 schema_version 메타를 두어 적용 대상
   모듈 버전과 대조한다. config_template(schema)은 패키지 버전에 동봉한다.
5. **시크릿 격리** — `.jwt_secret` 등은 데이터와 분리된 권한/백업/동기화 범위(`_secrets/`)에 둔다.
6. **래퍼/페이로드 config 단일화** — 모듈 바이너리가 읽는 위치를 SoT 로 명확화하고 래퍼 config
   중복을 제거한다.

## 2. 설정 동기화 절차

CIMS 설정은 두 종류이며 적용 경로가 다르다.

### A. 모듈 스칼라 설정 (csp.json/cmp.json) — `update_config`
1. 콘솔 `PUT /deployments/{id}/config {config:{k:v}}`
2. OAM: deployment 레코드에 값 저장(SoT) → `ha_lookup.should_propagate` 로 HA 멤버 결정
3. 멤버별 `update_config` job + `sync_txn` 1건
4. agent `job_update_config`: `install_path/<pkg>/config.json` 재기록(버전 디렉터리) → 모듈 SIGUSR1 reload → ack

### B. 컬렉션 (local_nodes/routes/sip_service…) — fan-out / `sync_config`
1. 콘솔 `PUT /deployments/{id}/collection/{name} {records:[...]}`
2. OAM: config_template schema 검증 → SoT 저장 `modules/oam/runtime/<collection>/` **(버전 무관)**
3. fan-out 대상 = `_COLLECTION_OWNER[col]` 모듈의 HA-group deployment 들 (scope 규칙)
4. agent `PUT /collection`(push) 또는 `job_sync_config`(pull `/api/agent/csp-config/<col>`):
   `install_path/config/<col>.jsonl` atomic write(버전 디렉터리) → SIGUSR1 → `sync_txn` ack
5. GET 시 멤버 간 records hash 비교로 **drift 감지**

### HA 전파 규칙 (`should_propagate`)
| scope | mode | 전파 |
|---|---|---|
| service | (any) | 항상 전 멤버 (그룹 공유) |
| system | active_standby | 전파 (VIP, 양 노드 동일) |
| system | all_active | 전파 안 함 (노드별 다른 값=정상) |
| (any) | standalone | 단일 노드 |

### 업그레이드/롤백 시 설정 승계
- 설치/업그레이드 시 agent 가 **직전 버전 디렉터리의 config(jsonl + `<pkg>/config.json`)를
  새 버전 디렉터리로 마이그레이션** → 모듈 측 설정은 **버전 귀속**.
- 롤백 = deployment 가 이전 버전 디렉터리를 가리키게 전환(그 버전 config 그대로 생존).

## 3. 디렉토리 레이아웃

```
/opt/cims-agent/                          # PREFIX (CIMS_AGENT_PREFIX)
├── agent/
│   ├── <ver>/                            # agent 번들 (버전단위, 최신 3개)
│   └── current -> <ver>                  # 활성 버전 심볼릭
├── state/  run/                          # agent 고유 상태/런타임 (버전 밖, 영속)
└── modules/
    ├── oam/
    │   ├── <ver>/                        # 버전단위 설치 (페이로드 + meta.json)
    │   ├── current -> <ver>              # 활성 버전 심볼릭 (CIMS_DIST_DIR 통로)
    │   └── runtime/                      # ── OAM 자기 데이터 (버전 무관) ──
    │       ├── _secrets/                 #    .jwt_secret 등 — 0700, 동기화/백업 제외
    │       ├── control/                  #    agents/ deployments/ jobs/ metrics/ packages/ pkg_files/ ha_groups/ sync_txn/
    │       └── console/                  #    accounts/ layouts/ menu/
    │
    ├── csp/
    │   ├── <ver>/                        # 버전단위 설치
    │   │   ├── config/<col>.jsonl        #    적용본(이 버전용) — 기존 유지
    │   │   └── csp/...                    #    페이로드
    │   ├── current -> <ver>              # 활성 버전 심볼릭 (CIMS_DIST_DIR / jsonlDir 통로)
    │   └── runtime/                      # ── CSP 모듈 데이터 (버전 무관 영속, csp 소유) ──
    │       └── collections/
    │           └── <collection>/         #    local_nodes/ remote_nodes/ routes/ rules/ ...
    │               ├── _schema_version   #    이 SoT 가 준수하는 모듈/스키마 버전
    │               └── <id>.json
    │
    ├── cmp/ ...  csc/ ...  console/ ...   # 동일 패턴 (컬렉션 있는 모듈만 runtime/collections)
```

### 핵심 규칙

1. **모듈 네임스페이스** — 컬렉션 SoT 는 `modules/<owner>/runtime/collections/<name>/`.
   `_COLLECTION_OWNER`(collection→owner)로 경로 자동 산출. csp 의 `rules` 와 cmp 의 `rules` 분리.
   레지스트리에 **`(owner, name)` 유일성 + 예약 카테고리명 충돌 검사** 추가.

2. **OAM 자기 데이터 카테고리화** — `runtime/{_secrets, control, console}`. 백업·동기화·권한을
   카테고리별 정책으로 분리.

3. **라이프사이클 결합** — `modules/<m>/runtime/` 는 **그 모듈 배포 시 생성·미배포면 부재·
   uninstall 시 prune**. `domain_dir` 을 읽기(`create=False`, mkdir 안 함)/쓰기 분리하여
   **읽기 부수효과 생성 제거**(미배포 모듈 조회 = `[]`).

4. **버전 귀속/스키마 정합** — SoT 레코드(또는 도메인)에 `schema_version`(=모듈 버전 계열) 메타.
   - 적용 대상 모듈 버전과 대조 → 불일치 시 경고/차단.
   - 업그레이드 설정 승계 단계에 **schema 마이그레이션 훅**(구→신 records 변환) 추가.
   - config_template(schema)은 패키지 버전에 동봉 → 검증은 **대상 버전 schema** 기준.

5. **래퍼/페이로드 config 단일화** — 모듈 바이너리가 읽는 위치(`<pkg>/config.json`,
   `config/<col>.jsonl`)를 SoT 로 명확화, 래퍼 config 중복 제거.

## 4. 구현 영향 / 호환

- `file_store`: `domain_dir(config, domain, create=True)` 시그니처 확장(읽기는 `create=False`).
  도메인 문자열은 `os.path.join` 이라 `modules/<m>/runtime/collections/<col>` 중첩 그대로 지원
  → 핵심 코어 변경 최소.
- `collection_domain(owner, name) -> "modules/<owner>/runtime/collections/<name>"` 헬퍼 추가,
  `csp_runtime`/`config_cache`/`sync_dispatch`/`_put_deployment_collection` 호출부 교체.
- 배포 핸들러(`_create_deployment`/`_delete_deployment`)에 모듈 runtime provision/prune 훅.
- **무중단 이행**: 읽기 시 v2 경로 우선 → 없으면 구 평면 경로 fallback. 1회 rename 이행
  스크립트 + 미배포 모듈 빈 잔재 디렉터리 제거.

핵심 헬퍼: `file_store.domain_dir`(카테고리) · `ha_lookup.collection_dir`/`migrate_flat_collections`/
`prune_module_collections`(컬렉션 경로·라이프사이클) · `services/collection_schema.py`(버전/schema 정합).

## 5. 미결 / 검토 필요

- service-scope(그룹 공유) vs system+all_active(노드별 상이) 의 SoT 표현: 후자는
  `modules/<m>/runtime/collections/<col>/<deployment_id>/` 처럼 **인스턴스 키**가 필요한가?
- 모듈 runtime 의 HA 동기화 주체: OAM(공유 스토리지) vs 모듈 자체 fan-out 의 경계 재정의.
- 롤백 시 SoT(버전 무관)와 적용본(버전 귀속) 의 schema 역호환 보장 범위.
