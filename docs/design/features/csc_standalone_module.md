# CSC 완전 독립 모듈화 — 공유 없는 계약 기반 분리

상태: **P1~P6 구현 완료 + 브랜치 push + 라이브 검증 (2026-06-22)** · 선행: [oam_csc_split.md](../oam_csc_split.md) · [oam_base_service_split.md](./oam_base_service_split.md)

> 구현: branch `feat/csc-standalone-module` (7커밋 P1~P6, origin push 완료, **PR 미생성·main 미머지**).
> 라이브 검증(dist 새 코드 격리 기동, 운영 oam 4419 무사): csc 단독(4421/4430, 무토큰 401·토큰 200
> DB 가입자 실서빙) · oam-svc 단독(4480, /recordings·/stats 200) — 모두 타 모듈 src 마운트 없이 부팅·서빙.
> oam(base)은 dist 자족 import 검증(4419 운영 점유로 라이브 부팅 생략).
> ▶ 다음 세션: **풀 배포 3프로세스 게이트웨이 E2E**(`make dist` + 부트스트랩/배포) · 콘솔 가입자 UI(csc API 경유) · PR 머지.

## 배경 / 문제

OAM base/service 분리(P0~P5)로 csc 는 게이트웨이 뒤 독립 서비스 모듈이 됐지만,
**분리가 코드 수준에서 끝까지 가지 않았다.** 증상:

1. **`services` 패키지 충돌** — file_store·admin_auth·ha_lookup 등 인프라 모듈이
   csc/src/services 에 물리적으로 살고, 빌드 시 oam 패키지로 복사된다. csc 프로세스가
   ems/core/oam/src 를 마운트하면 두 `services` 가 충돌 → `__init__.py` 삭제·`sys.path.append`·
   namespace 병합 같은 **뒷수습 해킹**으로 막아 왔다.
2. **도메인 뒤엉킴** — csc 가 ems/core/oam/src 를 마운트해 oam 의 `handlers.auth`(로그인/토큰발급),
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
**de-facto 공유 SDK 호스트**다. ems/core/oam/src/services 는 repo 에서 사실상 비어 있고 csc/src/services 를
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

## csc 최종 구성

**보유 (csc):**
- `mcptt.py` (규격 MCPTT) · `idms_storage.py` (IdMS 토큰, csc 자기 file)
- `config_cache.py` (CSP 런타임 설정 캐시, DB primary)
- handlers: `admin.py`(가입자 CRUD) · `org.py`(조직) · **`users` 가입자 CRUD 는 admin.py 가 이미 보유**
- **자체 JWT verify** (현 `admin_auth.py` 가 verify 보유 → 자족)
- 자체 vendor: csc 가 쓰는 최소 유틸(file_store-for-idms/config, logger) — **csc 자기 버전**

**제거 (csc 에서):**
- ems/core/oam/src 마운트 (`_OAM_SRC` glob·`sys.path.append`) — 전체 삭제
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
- **P3b — base(oam) 자족화 ✅ (완료)**: oam 폐포 = services 11개(admin_auth·alert_log·
  collection_schema·config_cache·drift_sweeper·file_store·flow_logger·ha_lookup·logger·
  service_registry·sync_txn) + httpsrv + util 를 **ems/core/oam/src 자체 복사본으로 보유**(repo 커밋,
  csc 와 독립 — 발산 가능). `oam_app.py` 의 `csc/src` 마운트(_CSC_SRC glob)·csc/cert fallback 제거.
  검증: py_compile OK · `sys.path=[ems/core/oam/src, ems/core/oam/vendor]`(csc/src 없음) 격리 import 성공
  (services 11 + httpsrv + util + handlers auth/agents/gateway/service_control/stats 전부 해석).
  ⚠️ cmd_pkg 의 csc→oam 복사는 P6 로 (현재 redundant·무해, P5 후 -f 가드 skip → oam 자체본 생존).
- **P4 — oam-svc 자족화 ✅ (완료)**: oam-svc 가 쓰는 것(flow_logger·logger·handlers recording/
  stats/verification·httpsrv·util)은 전부 ems/core/oam/src 에 있음(P3b) → `oam_svc_app.py` 의 `csc/src`
  마운트(_CSC_SRC) 제거, cert fallback 을 csc/cert→oam/cert 로, docstring 정정. oam-svc 는
  ems/core/oam/src + ems/core/oam/vendor 만 사용. 검증: py_compile OK · `sys.path=[ems/service/oam/src, ems/core/oam/src, ems/core/oam/vendor]`
  (csc/src 없음) 격리 import 성공. (oam-svc↔oam 간 ems/core/oam/src 공유는 OAM 패밀리 내부 — 별도 고려.)
- **P5 — csc 도메인 축소 ✅ (완료, 아무도 csc 를 마운트 안 함)**: csc/src/services 에서 비도메인
  모듈 7개(sync_dispatch·sync_txn·drift_sweeper·service_registry·collection_schema·alert_log·
  flow_logger) + service_descriptors_seed 물리 삭제, `handlers/csp_runtime.py`(RETIRED) 삭제,
  `__init__.py` 복원(csc 일반 패키지화 — 더 이상 namespace 병합 불요). csc/src/services 최종 =
  {mcptt, idms_storage, config_cache, file_store, ha_lookup, logger, admin_auth} 7개.
  검증: 삭제 모듈 코드 참조 0(주석/도메인명만) · py_compile OK · csc standalone import OK
  (services 7 + __init__ 일반패키지, handlers admin3/org1).
- **P6 — 빌드(cmd_pkg) 정리 ✅ (완료)**: cmd_pkg 의 oam staging `_shsrc`(csc→oam httpsrv/util/
  services 복사) 블록 제거 — oam 은 자족(cmd_sync 가 dist/oam/src 동기화). oam-svc auto-sync 가
  csc 블록(=ems/core/oam/src 동기화)도 타도록 + 주석 정정. 각 모듈 패키지가 자기 것만 동봉.
  검증: bash -n OK · `sync csc oam-svc` 후 dist 레이아웃(dist/oam/src/services=11+httpsrv+util,
  dist/csc/src/services=7+__init__, csp_runtime 삭제 전파) · **dist 트리 3모듈 자족 import 전부 OK**
  (csc=csc/src+vendor, oam=ems/core/oam/src+vendor, oam-svc=ems/service/oam/src+ems/core/oam/src+vendor — 모두 csc 비의존).
  - **라우팅(/users/me↔/users)은 이미 정합**: base 가 `/api/v1/users/me` in-process(D8, oam_app),
    게이트웨이가 `/api/v1/users/*` → csc 프록시. 정상 경로(게이트웨이)는 P1 영향 없음.
  - 🔲 **남은 항목(프런트, 비차단)**: 가입자 관리 UI 를 콘솔이 csc API 경유(oam-svc 오케스트레이션)로
    제공 — 콘솔 프로비저닝 워크벤치 재사용. 코드 결합 아님(별도 프런트 작업).

## 비목표 / 주의

- 가입자 DB 스키마 변경은 이 작업 범위 아님 (계약 유지).
- base 의 `/users/me`(콘솔 본인)와 csc 가입자 CRUD 의 `/users` 접두사 충돌 해소 방식(경로 분리 vs
  base 가 /me 만 가로채고 나머지 프록시)은 P3 에서 확정.
- 단일 프로세스 dev 편의(현 sibling 마운트)는 사라진다 — dev 에서도 csc 는 독립 기동.
