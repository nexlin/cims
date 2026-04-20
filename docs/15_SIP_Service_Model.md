# 15. SIP 서비스 모델 (Service-Trunk 주축)

CSP 를 **서비스(Service) 중심** 으로 재구성한 P7 단계의 설계 문서. Listener 는 infrastructure, Trunk 는 peering 수단, Service 는 비즈니스 경계.

---

## 1. 개념

```
┌──────────────────────────────────────────────────────────────┐
│                  Service (비즈니스 경계)                      │
│   domain: ims.mnc001.mcc001.3gppnetwork.org                   │
│   kind:   voip | ptt | ibcf | system | console                │
│   auth_realm: (Digest challenge 용 — NULL 이면 domain 상속)   │
│                                                               │
│   ├── Subscribers (UE 직접 접속)                              │
│   │    └ voip_subscriptions.service_id / ptt_subscriptions    │
│   │    └ REGISTER 시 (imsi@domain) 로 Digest 인증             │
│   │                                                           │
│   ├── Trunks (원격 peer 서버 — outbound)                      │
│   │    └ sip_trunk.service_id                                 │
│   │    └ failover_priority 순으로 alive 트렁크 자동 선택      │
│   │                                                           │
│   └── Listeners (inbound — 선택적 제한)                       │
│        └ inbound_policy='any'(기본) → 모든 리스너에서 수신   │
│        └ inbound_policy='restricted' → sip_service_listener 만│
└──────────────────────────────────────────────────────────────┘
```

Listener 는 "누가 나에게 연결할 수 있는 포트" 이고, Trunk 는 "내가 연결하는 외부 서버", Service 는 "같은 도메인을 공유하는 가입자+피어링 묶음".

---

## 2. 인증 흐름 (IMSI 정규화)

### 저장 형태

```sql
voip_subscriptions:
  id:         "+821357007001"
  user_id:    3
  service_id: 1                    ← P7 신규
  imsi:       "450033100000001"    ← P7 신규 (user 파트)
  passwd:     "123456"
  -- auth_id: 레거시 fallback (IMSI 정규화 완료 후 P8 에서 제거 예정)
```

### REGISTER 시

```
1. UE → REGISTER
    Authorization: Digest username="450033100000001@ims.mnc001.mcc001.3gppnetwork.org"
                   realm="ims.mnc001.mcc001.3gppnetwork.org"
                   response=<MD5 계산값>

2. CSP (CscfModule):
    2-1. From URI user → voip_subscriptions 에서 조회
    2-2. service_id=0 이면 REJECT (결정 #2)
    2-3. sip_service 에서 service_id=1 조회 → domain, auth_realm
    2-4. 기대 username = imsi + "@" + service.domain
         = "450033100000001@ims.mnc001.mcc001..."
    2-5. UE 의 username 과 완전 일치 확인
    2-6. HA1 = MD5(username : realm : password) — UE 와 동일 계산
    2-7. 일치 → 200 OK
```

**효과**: 서비스 도메인 변경 시 `sip_service.domain` 한 줄만 UPDATE. 가입자 전체 migration 불필요.

---

## 3. 엔티티 관계

```
sip_service (id, name, kind, domain, auth_realm, inbound_policy, priority, ...)
    │
    ├── 1:N → voip_subscriptions.service_id
    ├── 1:N → ptt_subscriptions.service_id
    ├── 1:N → sip_trunk.service_id
    └── 1:N → sip_service_listener.listener_id  (N:M via link table)
                └── csp_listener.id
```

### 스키마 발췌

```sql
CREATE TABLE sip_service (
    id              INT PRIMARY KEY AUTO_INCREMENT,
    name            VARCHAR(64) UNIQUE,
    kind            ENUM('voip','ptt','ibcf','system','console'),
    domain          VARCHAR(255),
    auth_realm      VARCHAR(255) DEFAULT NULL,
    inbound_policy  ENUM('any','restricted') DEFAULT 'any',
    priority        INT DEFAULT 100,
    enabled         TINYINT(1) DEFAULT 1,
    ...
);

ALTER TABLE voip_subscriptions
    ADD COLUMN service_id INT,
    ADD COLUMN imsi VARCHAR(32);

ALTER TABLE ptt_subscriptions
    ADD COLUMN service_id INT,
    ADD COLUMN imsi VARCHAR(32);

ALTER TABLE sip_trunk
    ADD COLUMN service_id INT,
    ADD COLUMN failover_priority INT DEFAULT 100;

CREATE TABLE sip_service_listener (
    service_id INT, listener_id INT,
    PRIMARY KEY (service_id, listener_id)
);
```

---

## 4. Inbound 처리 시나리오

### Case A — 로컬 UE REGISTER

```
UE → REGISTER sip:alice@ims.mnc001...
        │
        ▼ (Listener 5060 수신)
  CscfModule
    → From host "ims.mnc001..." 에 매칭되는 service = volte-main
    → voip_subscriptions.service_id = 1 인 가입자 탐색
    → imsi + "@" + service.domain 으로 Digest 검증
    → 200 OK + Contact 저장
```

### Case B — 로컬 onnet 통화 (같은 service 내)

```
UE A → INVITE sip:+82571900002@ims.mnc001...
        │
        ▼
  ModuleDispatcher
    → To URI host 매칭 service = volte-main
    → voip_subscriptions WHERE service_id=volte-main 에서 수신자 탐색
    → B-leg INVITE (Contact IP 로) — 트렁크 경유 안 함
```

### Case C — offnet 통화 (라우팅 규칙)

```
UE A → INVITE sip:+82100000000@ims.kt.co.kr
        │
        ▼
  RouteEngine
    → match: to_uri_host suffix "kt.co.kr"
    → target.mode = "service"
    → target.service_id = 3 (volte-kt-peering)
        │
        ▼
  TrunkManager.GetTrunksByService(3)
    → alive=true + failover_priority 순 정렬
    → 첫 번째 alive 트렁크 선택 (모두 dead 면 첫 번째 시도)
        │
        ▼
  선택된 트렁크로 B-leg INVITE
```

### Case D — 외부 트렁크에서 incoming

```
KT IMS → INVITE sip:+821357007001@ims.mnc001...
        │
        ▼ (Listener 5060 수신)
  ModuleDispatcher
    → To URI host = "ims.mnc001..." → service = volte-main
    → voip_subscriptions 에서 피호출자 탐색
    → B-leg INVITE (로컬 UE Contact 로)
```

---

## 5. 회복탄력성 관점

P1 에서 구축한 3층 캐시를 그대로 계승:

```
DB sip_service (master)
    ↕ (주기 sync + on-change notify)
CSC 메모리 + csc/cache/services.json
    ↕ (CSP HTTP pull + 로컬 스냅샷)
CSP 메모리 (CspServiceMap) + csp/cache/services.json
```

DB 또는 CSC 장애 시에도 CSP 는 로컬 파일에서 서비스 정의 로드하여 SIP 인증/라우팅 계속 수행.

---

## 6. Hot-reload 이벤트

`SERVICE_CHANGED` UDP notify 수신 시:
```cpp
gclsCspConfigCache.RefreshEntity(CACHE_SERVICE);
gclsServiceMap.Sync();
```

이후 모든 신규 REGISTER/INVITE 는 새 서비스 정의로 평가. 이미 등록된 UE 는 다음 re-REGISTER 때까지 이전 상태 유지.

---

## 7. Console UI

**SIP 서비스 페이지** (`SipServicesPage.tsx`)

- 목록: id, name, kind, domain, auth_realm, inbound_policy, priority, enabled
- 폼 편집:
  - kind 드롭다운 (voip/ptt/ibcf/system/console)
  - domain (URI 매칭 + IMPI 조립용)
  - auth_realm (선택 — 비우면 domain)
  - inbound_policy (any/restricted)
  - restricted 선택 시 listener 체크박스 다중선택

**기존 페이지와의 연동 (P8 예정)**:
- 가입자 편집 → service 드롭다운
- 트렁크 편집 → service 드롭다운
- 라우팅 규칙 target.mode 에 "service" 옵션 추가

---

## 8. 관련 파일

### CSP (C++)

| 파일 | 역할 |
|------|------|
| `csp/CspServiceMap.{h,cpp}` | 서비스 캐시 + domain/id 조회 |
| `csp/CscfModule.cpp` | Digest 검증에 service.domain + imsi 사용 |
| `csp/CspUser.h` | `m_iServiceId`, `m_strImsi` 필드 추가 |
| `csp/DbManager.cpp` | subscription 로딩에 service_id/imsi 컬럼 추가 |
| `csp/CspTrunkManager.{h,cpp}` | `GetTrunksByService(id)` 추가 |
| `csp/CspRouteEngine.{h,cpp}` | `target.mode="service"` 지원 |
| `csp/CspConfigCache.{h,cpp}` | `CACHE_SERVICE` 엔티티 추가 |
| `csp/CscInterface.cpp` | `SERVICE_CHANGED` 이벤트 수신 |

### CSC (Python)

| 파일 | 역할 |
|------|------|
| `csc/.../csc_config_cache.py` | ENTITIES 에 "service" 추가 |
| `csc/.../cims_csp_runtime.py` | `/api/v1/csp/services` CRUD |

### Console UI

| 파일 | 역할 |
|------|------|
| `cims-console/src/api/cspRuntime.ts` | `SipService`/`SipServiceInput` 타입 + CRUD |
| `cims-console/src/pages/SipServicesPage.tsx` | 서비스 관리 페이지 |

### 스키마

| 파일 | 내용 |
|------|------|
| `sql/migrate_sip_service.sql` | sip_service + ALTER subscriptions/trunks + seed |

---

## 9. 마이그레이션

```bash
sudo mysql cims < sql/migrate_sip_service.sql
```

수행 내용:
1. `sip_service` 테이블 생성 + 기본 2개 서비스 seed (volte-main, mcptt-main)
2. `voip_subscriptions`/`ptt_subscriptions` 에 `service_id`/`imsi` 컬럼 추가
3. 기존 auth_id 패턴 `<숫자>@<domain>` 에서 IMSI 추출 + 도메인 기반 서비스 자동 매핑
4. `sip_trunk` 에 `service_id`/`failover_priority` 컬럼 추가 (수동 매핑 필요)
5. `sip_service_listener` 링크 테이블 생성 (restricted 정책용)

---

## 10. 미구현/후속

| 항목 | 대상 |
|------|------|
| 가입자 Console UI 에 service 드롭다운 | P7.8 |
| 트렁크/라우팅 rule 편집에 service 선택 | P7.8 |
| `auth_id` 컬럼 제거 (IMSI 정규화 완료 후) | P8 |
| Inbound policy=restricted 의 listener 매칭 런타임 적용 | P8 |
| cross-service call routing (서비스 간 호) | P9 |
