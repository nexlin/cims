# CSC 완전 독립 모듈화 — 공유 없는 계약 기반 분리

선행: [oam_csc_split.md](../oam_csc_split.md) · [oam_base_service_split.md](./oam_base_service_split.md)

CSC 는 OAM base 게이트웨이 뒤의 **독립 서비스 모듈**이다. oam·oam-svc·csc 는 서로
타 모듈의 src 를 마운트하지 않고, 각자 자기에게 필요한 코드를 자체 보유한 채 부팅·서빙한다.
모듈 간 결합은 코드 공유가 아닌 **안정 계약(stable contract)** 으로만 이뤄진다.

## 배경 / 원칙

### 핵심 통찰: 공유 모듈 = lockstep 결합

oam·oam-svc·csc 는 **서로 독립적으로 버전업·배포**된다. 이들이 코드 모듈(예: file_store)을
런타임에 공유하면, 그 공유 모듈이 **lockstep 결합점**이 된다 — 한쪽이 바꾸면 다른 쪽이 강제로
끌려오거나 버전 스큐로 깨진다. **분리의 목적 자체를 무너뜨린다.**

→ 결론: **공유 런타임 모듈을 두지 않는다.** 모듈 간 결합은 **안정 계약** 으로만.

### 결합 방향과 자족화 순서

csc 는 서비스 모듈이면서 동시에 공유 라이브러리(file_store·admin_auth·ha_lookup 등)의 호스트가
될 수 있는 위치다. base(oam_app.py)·oam-svc(oam_svc_app.py)가 csc/src 를 마운트하지 않도록 하려면,
**소비자(oam·oam-svc)를 먼저 자족화 → 그 다음 csc 가 비도메인 모듈을 버림** 순서여야 안전하다
(역순으로 csc 에서 먼저 지우면 base/oam-svc 가 깨진다).

base 는 MCPTT 내부(`services.mcptt.notify_csp`·`audit_config_change`)를 직접 호출하지 않는다 —
MCPTT 관련 동작은 csc 가 규격에 따라 제공하는 **MCPTT API(HTTP)** 로만 접근한다.

## 도메인 경계

| 개념 | 소유 모듈 | 저장소 | 설명 |
|---|---|---|---|
| **사용자(user)** | base/oam | **file_store** (`console_accounts`) | 콘솔/관리 로그인 계정. `/users/me` = 로그인 본인 프로파일 |
| **가입자(subscriber)** | **csc** | **MariaDB** (`volte_subscriptions`·`ptt_subscriptions`) | VoLTE/PTT 단말 가입자. CRUD = csc 도메인 |
| MCPTT(규격) | csc | DB + 자기 file 영역 | mcptt(XCAP·USERS·GROUPS·notify_csp) + idms_storage(IdMS 토큰) |
| 조직/그룹 | csc | DB | org·ptt_groups |
| 통계/녹취/flow/검증 | oam-svc | 파일 | flow_logger 포함 |
| 배포/HA/패키지/런타임설정 | base/oam | file_store | file_store·ha_lookup·sync_*·service_registry 등 인프라 |

**사용자(file) ≠ 가입자(DB).** 둘은 다른 모듈·다른 저장소. 공유할 이유가 없다.

## 원칙

1. **공유 런타임 모듈 금지.** csc 는 ems/core/oam/src 를 마운트하지 않는다. 각 모듈은 자기에게
   필요한 것을 **자체 보유(self-contained)** 하고 독립 버전으로 발산할 수 있다.
2. **계약 기반 결합만.** base ↔ csc 사이는:
   - **HTTP** — base 게이트웨이가 `/api/v1/...subscriber...` 를 csc 로 프록시.
   - **JWT** — 공유 시크릿 + HS256 + 클레임 = **알고리즘 계약**. csc 는 **verify 만**(작고 거의
     안 변함) 자체 보유 → 코드 공유 아닌 계약. 발급(issue)은 base 단독.
   - **DB 스키마** — 가입자 테이블 스키마 = 계약.
3. **데이터 비공유.** csc 데이터(가입자=MariaDB, IdMS/config=csc 자기 file 영역)는 oam
   file_store 데이터와 물리적으로 분리. base/oam 이 가입자 데이터가 필요하면 **csc API 호출**.
4. **csc 는 headless.** UI 없음. 콘솔(base 서빙)이 csc API 를 호출해 가입자 관리 UI 제공.

## 토폴로지: 가입자 관리 UI

```
[Console SPA] (base/oam 가 정적 서빙)
     │  가입자 관리 화면
     ▼
[base 게이트웨이 :4419]  ──proxy──►  [oam-svc :4480]  ──csc API──►  [csc :4421]
                          (서비스관리 평면)              (HTTP/JWT)      └─ MariaDB
```

- 가입자 관리 = **서비스 관리(operational)** 기능 → **oam-svc** 가 csc API 를 이용해 오케스트레이션,
  콘솔이 그 화면을 제공. (또는 게이트웨이가 csc 로 직접 프록시 — 세부는 구현 시 확정.)
- csc 는 데이터/규격 API 만 제공. 본인 프로파일 `/users/me` 는 base 가 처리하되,
  가입자 구독 데이터가 필요하면 csc API 를 호출(직접 DB 접근 안 함).

## 각 모듈 구성 (자족)

각 모듈은 자기에게 필요한 코드만 자체 보유하며, 타 모듈의 src 를 마운트하지 않는다.
패키지 빌드 시 각 모듈은 자기 것만 동봉한다.

**csc (`csc/src`):**
- services 7개: `mcptt`(규격 MCPTT) · `idms_storage`(IdMS 토큰, csc 자기 file) ·
  `config_cache`(CSP 런타임 설정 캐시, DB primary) · `file_store`(idms/config 용) ·
  `ha_lookup` · `logger` · `admin_auth`(**자체 JWT verify** — 발급은 base 단독, csc 는 verify 만)
- handlers: `admin.py`(가입자 CRUD = `users` 가입자 CRUD 포함) · `org.py`(조직)
- 자체 vendor: csc 가 쓰는 최소 유틸 (csc 자기 버전)
- csc 는 oam handlers(`auth`·`users`/`/users/me`)·flow API 를 서빙하지 않는다 — 로그인/`/users/me` 는
  base 책임, flow 는 oam-svc 책임. csc 자기 로깅은 `csc_logger`.

**base/oam (`ems/core/oam/src`):**
- services 11개: admin_auth · alert_log · collection_schema · config_cache · drift_sweeper ·
  file_store · flow_logger · ha_lookup · logger · service_registry · sync_txn — + httpsrv + util.
- 로그인/토큰 발급(`handlers.auth`)·본인 프로파일(`handlers.users` `/users/me`)·게이트웨이·
  service_control 보유. 서비스 start/stop 감사는 base 자체 `_audit_service_action`
  (file_store JSONL, `service_control_audit` 도메인).
- `sys.path = [ems/core/oam/src, ems/core/oam/vendor]` (csc/src 없음).

**oam-svc (`ems/service/oam/src`):**
- 통계/녹취/flow/검증 핸들러(recording/stats/verification) + flow_logger. httpsrv/util/logger 는
  ems/core/oam/src 의 것을 사용.
- `sys.path = [ems/service/oam/src, ems/core/oam/src, ems/core/oam/vendor]` (csc/src 없음).
  (oam-svc↔oam 간 ems/core/oam/src 공유는 OAM 패밀리 내부.)

## 라우팅 (/users/me ↔ /users)

base 가 `/api/v1/users/me`(콘솔 로그인 본인)를 in-process 로 처리하고, 게이트웨이가
`/api/v1/users/*`(가입자 CRUD)를 csc 로 프록시한다. 가입자 관리 UI 는 콘솔이 csc API 를 경유
(oam-svc 오케스트레이션)해 제공 — 콘솔 프로비저닝 워크벤치를 재사용한다(코드 결합 아님).

## 비목표 / 주의

- 가입자 DB 스키마 변경은 이 작업 범위 아님 (계약 유지).
- 단일 프로세스 dev 편의(sibling 마운트)는 없다 — dev 에서도 csc 는 독립 기동.
