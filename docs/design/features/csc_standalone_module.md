# CSC 완전 독립 모듈화 — 공유 없는 계약 기반 분리

상태: 설계 (2026-06-21) · 선행: [oam_csc_split.md](../oam_csc_split.md) · [oam_base_service_split.md](./oam_base_service_split.md)

## 배경 / 문제

OAM base/service 분리(P0~P5)로 csc 는 게이트웨이 뒤 독립 서비스 모듈이 됐지만,
**분리가 코드 수준에서 끝까지 가지 않았다.** 증상:

1. **`services` 패키지 충돌** — file_store·admin_auth·ha_lookup 등 인프라 모듈이
   csc/src/services 에 물리적으로 살고, 빌드 시 oam 패키지로 복사된다. csc 프로세스가
   oam/src 를 마운트하면 두 `services` 가 충돌 → `__init__.py` 삭제·`sys.path.append`·
   namespace 병합 같은 **뒷수습 해킹**으로 막아 왔다.
2. **도메인 뒤엉킴** — csc 가 oam/src 를 마운트해 oam 의 `handlers.auth`(로그인/토큰발급),
   `handlers.users`(본인 프로파일/`/users/me`)를 **자기 프로세스에서 빌려 서빙**한다.
3. **개념 혼동** — oam 의 "사용자"와 csc 의 "가입자"는 **서로 다른 것**인데 `/api/v1/users`
   접두사를 공유해 뒤섞였다.

### 핵심 통찰: 공유 모듈 = 미래의 lockstep 결합

oam·oam-svc·csc 는 **서로 독립적으로 버전업·배포**된다. 이들이 코드 모듈(예: file_store)을
런타임에 공유하면, 그 공유 모듈이 **lockstep 결합점**이 된다 — 한쪽이 바꾸면 다른 쪽이 강제로
끌려오거나 버전 스큐로 깨진다. **분리의 목적 자체를 무너뜨린다.**

→ 결론: **공유 런타임 모듈을 두지 않는다.** 모듈 간 결합은 **안정 계약(stable contract)** 으로만.

### 핵심 발견 (2026-06-21): 결합 방향은 *역방향* — base·oam-svc 가 csc 를 마운트

분석 결과 실제 구조는 "oam 이 csc 로 복사"가 아니라 **csc/src 가 공유 라이브러리의 정본이고
base(oam_app.py)와 oam-svc(oam_svc_app.py)가 `sys.path` 에 `csc/src` 를 *마운트*** 한다
(cmd_pkg 의 oam←csc 복사는 패키징용 2차 수단). 즉 csc 는 서비스 모듈이면서 동시에
**de-facto 공유 SDK 호스트**다. oam/src/services 는 repo 에서 사실상 비어 있고 csc/src/services 를
빌려 쓴다.

따라서 "csc 완전 분리" = **base·oam-svc 가 csc/src 마운트를 끊는 것**이 핵심이며, 순서는
**소비자(oam·oam-svc)를 먼저 자족화 → 그 다음 csc 가 비도메인 모듈을 버림** 이어야 안전하다
(역순으로 csc 에서 먼저 지우면 base/oam-svc 가 깨진다).

oam→csc 역참조 중 코드가 아닌 **계약으로 바꿔야 할 leak**: `oam_app.py`/`service_control.py` 의
`services.mcptt.notify_csp`·`audit_config_change` (base 가 MCPTT 내부를 직접 호출 — csc API/이벤트
계약으로 전환 대상).

## 도메인 경계 (확정)

| 개념 | 소유 모듈 | 저장소 | 설명 |
|---|---|---|---|
| **사용자(user)** | base/oam | **file_store** (`console_accounts`) | 콘솔/관리 로그인 계정. `/users/me` = 로그인 본인 프로파일 |
| **가입자(subscriber)** | **csc** | **MariaDB** (`volte_subscriptions`·`ptt_subscriptions`) | VoLTE/PTT 단말 가입자. CRUD = csc 도메인 |
| MCPTT(규격) | csc | DB + 자기 file 영역 | mcptt(XCAP·USERS·GROUPS·notify_csp) + idms_storage(IdMS 토큰) |
| 조직/그룹 | csc | DB | org·ptt_groups |
| 통계/녹취/flow/검증 | oam-svc | 파일 | flow_logger 포함 (현재 csc 에 오배치 → oam-svc 로) |
| 배포/HA/패키지/런타임설정 | base/oam | file_store | file_store·ha_lookup·sync_*·service_registry 등 인프라 |

**사용자(file) ≠ 가입자(DB).** 둘은 다른 모듈·다른 저장소. 공유할 이유가 없다.

## 원칙

1. **공유 런타임 모듈 금지.** csc 는 oam/src 를 마운트하지 않는다. 각 모듈은 자기에게
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

## csc 최종 구성

**보유 (csc):**
- `mcptt.py` (규격 MCPTT) · `idms_storage.py` (IdMS 토큰, csc 자기 file)
- `config_cache.py` (CSP 런타임 설정 캐시, DB primary)
- handlers: `admin.py`(가입자 CRUD) · `org.py`(조직) · **`users` 가입자 CRUD 는 admin.py 가 이미 보유**
- **자체 JWT verify** (현 `admin_auth.py` 가 verify 보유 → 자족)
- 자체 vendor: csc 가 쓰는 최소 유틸(file_store-for-idms/config, logger) — **csc 자기 버전**

**제거 (csc 에서):**
- oam/src 마운트 (`_OAM_SRC` glob·`sys.path.append`) — 전체 삭제
- oam `handlers.auth`·`handlers.users`(`/users/me`) 차용 — base 가 처리
- `csp_runtime.py` (RETIRED, 2026-05-19) — 마이그레이션 헬퍼만 scripts 로
- `flow_logger.py` — oam-svc 로 이전 (csc 미사용)
- HA fan-out 인프라(sync_dispatch·sync_txn·drift_sweeper·service_registry·collection_schema·
  alert_log) — csc 라이브 미사용(csp_runtime 전용이었음) → csc 에서 제거, base/oam 잔류
- cims.sh `cmd_pkg` 의 oam→csc `services` 복사 + `__init__.py` 제외 해킹 — 삭제
- `oam_csc_split.md` 의 namespace-merge(handlers/services) 전제 — csc 가 마운트 안 하므로 무효화

## 단계 계획 (phased) — 핵심 발견 반영 (소비자 자족화 우선)

각 단계 끝에 `make dist` + 해당 모듈 standalone import/기동 스모크 게이트.

- **P1 — csc 가 oam 을 안 본다 ✅ (완료)**: csc_app.py 에서 `_OAM_SRC` 마운트·oam handlers
  (auth/users) import 제거. JWT 검증을 자체 services.admin_auth 로. csc 가 자기 handlers
  (admin/org)·services 만으로 기동. 로그인/`/users/me` 는 base 책임(csc 미서빙).
- **P2 — csc 런타임 잔재 정리 ✅ (완료, 무위험분)**: csc_app.py 의 flow_logger vestigial
  init 제거(csc 는 flow API 미서빙, 자기 로깅은 csc_logger). ⚠️ **`__init__.py` 복원·csc 의
  비도메인 모듈 물리 삭제는 P3~P5 이후로 보류** — base·oam-svc 가 아직 csc/src 를 마운트하므로
  지금 지우면 그들이 깨진다.
- **P3a — base 의 mcptt leak 제거 ✅ (완료)**: 계약 확정 — **MCPTT→CSP notify(notify_csp)·
  config audit 는 csc 전용 기능. base 는 mcptt 함수를 절대 쓰지 않는다.** oam-svc 가 MCPTT
  관련 동작이 필요하면 csc 가 규격에 따라 제공하는 **MCPTT API(HTTP)** 를 호출(csc 내부 코드 X).
  - `service_control.py`: `services.mcptt.audit_config_change`(서비스 start/stop 감사 차용) →
    base 자체 `_audit_service_action`(file_store JSONL, `service_control_audit` 도메인)로 대체.
  - `oam_app.py`: mcptt.notify_csp 를 base 공유 라이브러리로 적던 docstring 정정.
  - 검증: base 에 mcptt 내부 호출/ import 0 (notify_csp 는 docstring 설명만).
- **P3b — base(oam) 자족화**: oam 이 필요한 인프라(admin_auth·file_store·ha_lookup·sync_*·
  drift_sweeper·service_registry·collection_schema·alert_log·logger·flow_logger)를 oam 자체
  복사본으로 보유 → `oam_app.py` 의 `csc/src` 마운트 제거. `make dist` + oam standalone import 검증.
- **P4 — oam-svc 자족화**: oam-svc 가 flow_logger/logger 를 자체 보유(또는 oam/src 에서만) →
  `oam_svc_app.py` 의 `csc/src` 마운트 제거. oam-svc standalone 검증.
- **P5 — csc 도메인 축소 (이제 아무도 csc 를 마운트 안 함)**: csc/src/services 에서 비도메인
  모듈(sync_*·drift_sweeper·service_registry·collection_schema·alert_log·flow_logger) 물리 삭제,
  csp_runtime(RETIRED) 정리, `__init__.py` 복원(csc 일반 패키지화). csc/src/services =
  {mcptt, idms_storage, config_cache, file_store, ha_lookup, logger, admin_auth}.
- **P6 — 게이트웨이/콘솔/빌드 정리**: `/users/me`(base)↔`/users`CRUD(csc) 라우팅 정합, 가입자 관리
  UI = 콘솔이 csc API 경유(oam-svc 오케스트레이션), cmd_pkg 의 oam←csc 복사·dual-mount glob·
  namespace 해킹 제거. 각 모듈 패키지가 자기 것만 동봉.

## 비목표 / 주의

- 가입자 DB 스키마 변경은 이 작업 범위 아님 (계약 유지).
- base 의 `/users/me`(콘솔 본인)와 csc 가입자 CRUD 의 `/users` 접두사 충돌 해소 방식(경로 분리 vs
  base 가 /me 만 가로채고 나머지 프록시)은 P3 에서 확정.
- 단일 프로세스 dev 편의(현 sibling 마운트)는 사라진다 — dev 에서도 csc 는 독립 기동.
