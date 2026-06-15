# Runtime Store v2 — 모듈/버전 귀속 네임스페이스 설계

> 2026-06-15 작성. [runtime_store_design.md](runtime_store_design.md)(평면 file-store 원설계)의
> **개정안**. 본 문서는 설계 합의를 위한 것이며, 구현은 합의 후 단계 진행한다.
> 관련: [oam_csc_split.md](oam_csc_split.md) · [features/sip_runtime_config.md](features/sip_runtime_config.md) ·
> [features/package_and_template.md](features/package_and_template.md)

## 1. 배경 / 문제

현재 OAM runtime store 는 `{CimsRuntimeDir}/<domain>/` **단일 평면 네임스페이스**에 모든
데이터를 둔다. 실측(media02, CSP 미배포 노드):

```
/opt/cims-agent/modules/oam/runtime/
  .jwt_secret
  agents/ deployments/ jobs/ metrics/ packages/ pkg_files/ ha_groups/   ← OAM 관리평면
  console_accounts/ console_layouts/ console_menu/                       ← OAM 콘솔
  csp_listener/ sip_service/ sip_trunk/ routing_rule/ routing_access_list/ csp_sync_txn/ services/
                                        ← CSP 모듈 컬렉션 (OAM 디렉터리 안에 타 모듈 데이터)
```

### 확인된 문제

1. **모듈 간 이름 충돌(잠재)** — 컬렉션 보유 모듈은 현재 csp(9)뿐(cmp/imp/psp/isp/csc=0).
   어느 모듈이 같은 이름 컬렉션(`rules`/`routes`/`services` 등)을 추가하면 같은 전역
   디렉터리에서 레코드가 섞인다. `(owner, name)` 유일성을 강제하는 장치 없음.
2. **소유권 역전** — CSP 컬렉션 SoT 가 `modules/oam/runtime/` (OAM 디렉터리) 안에 존재.
   모듈 데이터가 OAM 소유 공간에 종속됨.
3. **라이프사이클 미결합** — `file_store.domain_dir()` 이 매 호출 `makedirs` → **읽기만 해도**
   디렉터리 생성. CSP 미배포 노드(media02)에도 `csp_listener/` 등이 read 부수효과로 생김.
   배포 여부와 무관하게 도메인이 materialize 된다.
4. **버전 귀속 불일치** — SoT 는 버전 무관(`modules/oam/runtime/<col>`), 적용본은 버전 귀속
   (`modules/<m>/<ver>/config/<col>.jsonl`), schema(`config_template.json`)도 버전 종속.
   모듈 버전 간 **스키마 변경** 시 단일 SoT 가 어느 버전 모양인지 모호 → 버전 불일치 위험.
5. **시크릿 노출면** — `.jwt_secret` 이 데이터와 동일 권한/백업/동기화 범위에 평면 위치.
6. **래퍼/페이로드 config 중복** — `modules/<m>/<ver>/config/` (래퍼)와
   `modules/<m>/<ver>/<pkg>/config/` (페이로드) 양쪽에 config 존재 → SoT 모호.

## 2. 현재 설정 동기화 절차 (개정 대상 — 참고)

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

## 3. 목표 레이아웃 (v2)

```
/opt/cims-agent/                          # PREFIX
├── agent/                                # agent 번들 (in-place 교체)
├── state/  run/                          # agent 고유 상태/런타임
└── modules/
    ├── oam/
    │   ├── <ver>/                        # 버전단위 설치 (페이로드 + meta.json)
    │   └── runtime/                      # ── OAM 자기 데이터 (버전 무관) ──
    │       ├── _secrets/                 #    .jwt_secret 등 — 0700, 동기화/백업 제외
    │       ├── control/                  #    agents/ deployments/ jobs/ metrics/ packages/ pkg_files/ ha_groups/ sync_txn/
    │       └── console/                  #    accounts/ layouts/ menu/
    │
    ├── csp/
    │   ├── <ver>/                        # 버전단위 설치
    │   │   ├── config/<col>.jsonl        #    적용본(이 버전용) — 기존 유지
    │   │   └── csp/...                    #    페이로드
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

## 5. 단계 (Phase)

| P | 내용 | 상태 | 커밋 |
|---|---|---|---|
| P1 | 시크릿 격리 `runtime/_secrets/` (.jwt_secret) 0700 | ✅ 완료 | `98fbe620` (+라이브 .49 적용) |
| P2 | OAM 자기 데이터 카테고리화 `control/`·`console/` | ✅ 완료 | `5861b0c6` (라이브 dev 이행, 카운트 보존) |
| P3 | 모듈 prefix `modules/<m>/runtime/collections/` + 레지스트리 유일성 | ✅ 완료 | `a05d6761` |
| P4 | 라이프사이클 결합(읽기 무생성·uninstall prune) | ✅ 완료 | `ce3168a2` |
| P5 | 버전/schema 정합(schema_version 메타 + 업그레이드 마이그레이션 훅) | ✅ 완료 | `1210d0a2` |

**구현 순서**: P1 → P3 → P4 → P2 → P5 (전부 dev 라이브 검증). 핵심 헬퍼:
`file_store.domain_dir`(P2 카테고리) · `ha_lookup.collection_dir`/`migrate_flat_collections`/
`prune_module_collections`(P3·P4) · `services/collection_schema.py`(P5).

> ⚠️ 프로덕션 정합: 컬렉션 경로(P3/P4)·schema(P5)는 csc·OAM 공유 코드라 **deployed csc/oam
> 패키지 재배포로 정합** 필요(현재 컬렉션 데이터 0건이라 기능 영향 없음). dev 활성 plane
> (repo OAM)은 이미 P1~P5 라이브.

## 6. 미결 / 검토 필요

- service-scope(그룹 공유) vs system+all_active(노드별 상이) 의 SoT 표현: 후자는
  `modules/<m>/runtime/collections/<col>/<deployment_id>/` 처럼 **인스턴스 키**가 필요한가?
- 모듈 runtime 의 HA 동기화 주체: OAM(공유 스토리지) vs 모듈 자체 fan-out 의 경계 재정의.
- 롤백 시 SoT(버전 무관)와 적용본(버전 귀속) 의 schema 역호환 보장 범위.
