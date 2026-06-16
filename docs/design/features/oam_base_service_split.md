# OAM 게이트웨이 + 서비스 모듈 아키텍처 (base/service 분리)

> 배경: OAM(`oam_app.py`)은 콘솔 정적 서빙(단일 HTTPS:4419 오리진) + 콘솔이 호출하는
> `/api/v1/*` REST 백엔드 + agent/HA/패키지 제어면을 **한 프로세스**가 모두 담당한다.
> 서비스 종속 기능이 늘면서, "공통/인프라(base)"와 "서비스 전용"을 분리하고 base 를
> **API 게이트웨이**로 삼아 단일 오리진을 유지하면서 **다양한 서비스 모듈을 그 뒤에 수용**하자는
> 것이 본 설계다.
>
> 설계 결정(사용자 확정):
> - **D1** 콘솔 위젯 = full 번들 전달 + **사용자별 프로파일/위젯 구성 레이어**.
> - **D2** 라우팅 = **서비스별 최상위 경로 세그먼트 + 게이트웨이 라우트 테이블 + 서비스 self-register**.
> - **D3** **csc 는 OAM 이 아니라 독립 서비스 모듈**(가입자+PTT). base 는 csc 를 업스트림으로 프록시.
>         → "단일 service OAM" 개념 폐기, **게이트웨이 뒤 N개 독립 모듈**로 일반화.
> - **D4** 설정 = **공통 config + 서비스별 `<service>.json` 분리**.
>
> 선행: [[oam_self_upgrade]](감독·preflight), 부트스트랩 base/full 콘솔 프로파일.

---

## 1. 목적과 불변식

### 목적
1. **장애 격리** — 서비스 모듈의 버그/누수/지연이 base(로그인·시스템관리·콘솔 서빙)를 끌어내리지
   않는다. 서비스 모듈이 죽어도 운영자는 콘솔에 로그인해 시스템을 관리할 수 있다.
2. **독립 수명주기** — 각 서비스 모듈을 base 무중단으로 롤링 업그레이드/재기동/롤백한다.
3. **확장성** — 새 서비스(미래의 VoLTE 관리, 외부 연동 등)를 게이트웨이 코어 수정 없이 추가한다.
4. **응집** — "서비스 위젯 ↔ 그 위젯의 API ↔ 그 서비스 config" 를 한 모듈 경계 안에 모은다.
5. **자원 격리** — 무거운 서비스 stats 집계/대용량 녹취 스트리밍이 base 의 auth·agent heartbeat
   경로를 블록하지 않는다.

### 불변식 (보존)
- **I1. 단일 공개 오리진** — 브라우저/관제는 오직 **base OAM 4419(HTTPS)** 만 본다. 서비스 모듈은
  **loopback(127.0.0.1) 비공개 포트**. CORS·방화벽 포트 추가 없음, nginx 재도입 없음.
- **I2. 콘솔 정적 자산은 base 가 전부 서빙** — 셸 + 전 위젯 청크. 서비스 모듈은 API 만 담당.
- **I3. 의존 방향 단방향** — service → base(인증/형상 조회) 허용, **base → service 의존 금지**.
  서비스 모듈 부재 시 base 정상 동작(해당 서비스 라우트만 503/404).
- **I4. 하위 호환** — 단일 프로세스(현행)로도 계속 기동 가능(`--role all`). 분리는 옵트인.
- **I5. file_store 컬렉션 단일 소유자** — 동일 컬렉션을 두 프로세스가 동시 쓰지 않는다.

---

## 2. 현행 구조 (as-is)

```
브라우저 ──HTTPS:4419──▶ oam_app.py (단일 프로세스)
                          ├─ console_static : SPA dist 서빙 (base_path "/")
                          ├─ /api/v1/* 핸들러 17종
                          └─ file_store
csc_app.py (별도 프로세스, 4421 admin + 4430 mcptt/XCAP)  ← 이미 독립 모듈로 운영 중
```

코드 근거:
- 라우팅: `csc/src/httpsrv/controller.py:158-175` — 등록 base_path 중 **최장 일치** 디스패치.
  → 세그먼트 prefix 기반 게이트웨이가 추가 프레임워크 없이 성립.
- 핸들러 등록: `oam/src/oam_app.py:384~` `add_dynamic_rules([(base_path, fn, kwargs)])`.
- **조건부 로딩 선례**: `oam_app.py:118-128` — csc 측 `handlers.admin`/`org` 를 try/except 선택 로드,
  미설치 시 graceful. (D3 의 게이트웨이 프록시로 정식화할 하이브리드.)
- 감독: `agent/lib/lifecycle.sh:340-366` `start_oam`(+`kill_stray`), `agent/cims_agent.py:1675~`
  watchdog 가 `run/supervised.json {module: install_path}` desired-state 로 auto-restart,
  `oam_app.py --preflight`(`:1721`) 교체 전 검증.
- 인증: `oam.json` `CimsAuth.JwtSecret` + `BuiltinAccounts` 존재.

---

## 3. 목표 구조 (to-be) — 게이트웨이 + 독립 서비스 모듈

```
                           ┌─────────────────────────────────────────────────────┐
브라우저/관제 ─HTTPS:4419─▶ │ base OAM  (게이트웨이 + 공통 관리)                     │
                           │  • 콘솔 정적 서빙 (full 번들: 셸 + 전 위젯)            │
                           │  • 공통 /api/v1/*  (auth·agents·HA·modules·console·    │
                           │     console_accounts·user-layouts·external_systems…)  │
                           │  • 게이트웨이: /api/v1/<service>/* ─라우트테이블─┐      │
                           │  • file_store: control/·console/(+ 사용자 레이아웃)    │
                           └────────────────────────────────────────────────┼──────┘
                                       loopback HTTP (127.0.0.1:특정포트)      │
              ┌───────────────────────────────┬───────────────────────────────┘
              ▼                               ▼                              ▼
   ┌──────────────────────┐    ┌──────────────────────────┐    ┌──────────────────────┐
   │ csc (독립 모듈)       │    │ svc-mgmt (서비스관리 모듈)  │    │ <future service>     │
   │ 4421/4430            │    │ 127.0.0.1:44xx            │    │ 127.0.0.1:44yy       │
   │ /api/v1/subscribers  │    │ /api/v1/calls (녹취·flow)  │    │ /api/v1/<seg>/...    │
   │ /api/v1/ptt          │    │ /api/v1/stats/service     │    │                      │
   │ + 가입자/XCAP 본체    │    │ /api/v1/verification      │    │                      │
   │ + 자기 config·위젯    │    │ + 자기 config·위젯         │    │ + 자기 config·위젯    │
   └──────────────────────┘    └──────────────────────────┘    └──────────────────────┘
```

요점:
- **base OAM = 게이트웨이 + 공통 관리.** 단일 공개 진입점(4419), 콘솔 전 자산 서빙, 공통 API,
  그리고 `/api/v1/<service>/*` 를 라우트 테이블로 업스트림에 프록시.
- **서비스 모듈 = 독립 프로세스**(csc 처럼). 각자 자기 경로 세그먼트·자기 config·자기 위젯을
  소유하고, 설치 시 게이트웨이에 자기 라우트를 **self-register**.
- base 엔트리포인트는 단일 `oam_app.py` + `--role {base|all}`. `all` = 현행 단일프로세스(I4).

> **명명 정정(D3):** "service OAM" 단일체는 없다. base 뒤에 csc·svc-mgmt·미래서비스 등
> **여러 독립 모듈**이 병렬로 선다. 각 모듈은 OAM 의 일부가 아니라 게이트웨이의 업스트림이다.

> **공유 SDK(D5):** 각 모듈은 **독립 아티팩트/엔트리포인트**(base=`oam_app.py`, csc=`csc_app.py`,
> svc-mgmt=`svc_mgmt_app.py`)이고, 공통 코드(httpsrv·auth·file_store·핸들러 스캐폴딩)는 **공유
> 라이브러리 OAM-SDK** 로 추출해 각자 import 한다. 바이너리(단일 binary+role flag) 공유가 아니라
> **라이브러리 공유**가 모듈 독립성을 지키는 표준.

---

## 4. 모듈 경계와 핸들러 귀속

현 17개 핸들러를 **공통(base 직접 처리)** 와 **서비스 모듈(업스트림)** 로 귀속.

| 핸들러 | 귀속 | 경로(목표) | 근거 |
|---|---|---|---|
| `auth`, `users(/me)` | base 공통 | `/api/v1/auth`, `/api/v1/users/me` | 로그인 부트스트랩 필수 |
| `console`, `console_accounts` | base 공통 | `/api/v1/console*` | 콘솔 메뉴·계정 |
| `console_static` | base 공통 | `/` | I2 정적 단일 서빙 |
| **user-layouts** *(신규)* | base 공통 | `/api/v1/console/layouts` | D1 사용자별 구성 저장 |
| `agents`,`agent_api`,`modules`,`ha_groups` | base 공통 | `/api/v1/{agents,modules,ha}` | 인프라 제어면 |
| `build`,`service_descriptors`,`service_control` | base 공통 | `/api/v1/{build,services}` | 배포 오케스트레이션 |
| `external_systems` | base 공통 | `/api/v1/external-systems` | 외부 형상(대시보드) |
| `alerts`(프레임워크) | base 공통 | `/api/v1/alerts` | 골격 공통(서비스는 push) |
| `stats(/health)` | base 공통 | `/api/v1/stats/health` | 노드 헬스/형상 |
| `admin`,`org`(가입자 CRUD) | **csc 모듈** | `/api/v1/subscribers`, `/api/v1/org` | D3 가입자=csc 도메인 |
| (PTT 그룹/affiliation 관리) | **csc 모듈** | `/api/v1/ptt` | D3 |
| `stats(/service/voip\|ptt)` | **svc-mgmt 모듈** | `/api/v1/stats/service` | CSP/CMP KPI 관측 |
| `recording`, `flow_logger` | **svc-mgmt 모듈** | `/api/v1/calls` | 녹취·SIP flow 관측 |
| `verification` | **svc-mgmt 모듈** | `/api/v1/verification` | S1~S6 검증(testbed) |

> `stats` 는 base(health)·svc-mgmt(KPI) 로 **핸들러 함수 단위 분리**(모듈 파일은 공유, HANDLER_LIST
> 만 둘로). 가입자 CRUD 핸들러는 현재 OAM in-process import(`oam_app.py:118`) → 목표는 **csc 가
> 직접 서빙하고 base 가 프록시**(중복 제거).

base 엔트리포인트 역할 분기 (Phase 0, 무동작변경):
```python
role = args_dict.get('role', 'all')          # base | all
if role in ('base', 'all'):
    for L in BASE_HANDLER_LISTS: admin_server.add_dynamic_rules(_bind(L))
    register_console_static(...)
if role == 'all':
    for L in LEGACY_INPROCESS_SERVICE_LISTS: admin_server.add_dynamic_rules(_bind(L))  # 하위호환
if role == 'base':
    register_gateway(route_table)            # 서비스는 업스트림 프록시
```
`--role all` = 현행과 100% 동일. 분리 배포에서만 `--role base` + 독립 서비스 모듈.

---

## 5. 게이트웨이 — 서비스별 세그먼트 라우팅 (D2)

### 라우팅 모델
**각 서비스가 `/api/v1/<service>/` 최상위 세그먼트 하나를 소유**하고, 게이트웨이는 그 세그먼트로
업스트림을 고른다. (Kong/nginx/Traefik 의 path-prefix 라우팅과 동일.)

```
라우트 테이블 (게이트웨이 보유):
  /api/v1/subscribers  → csc      (127.0.0.1:4421)
  /api/v1/org          → csc
  /api/v1/ptt          → csc
  /api/v1/calls        → svc-mgmt (127.0.0.1:44xx)
  /api/v1/stats/service→ svc-mgmt
  /api/v1/verification → svc-mgmt
  (그 외 /api/v1/*      → base 직접 처리)
```

- `controller.py` **최장 일치** 규칙 덕에 base 고유의 더 구체적 경로(`/api/v1/stats/health`)는
  base 가 우선, `/api/v1/stats/service/*` 는 svc-mgmt 로 프록시 — 충돌 없이 공존.
- **self-register**: 서비스 모듈 설치 시 자기 매니페스트의 `routes`(+upstream 주소)를 게이트웨이
  라우트 테이블(file_store `control/gateway_routes`)에 등록. 새 서비스 = 테이블 한 줄, **코어 무수정**.
- 미등록/`Enabled:false`/업스트림 부재 → 게이트웨이가 503(I3).

### 경로 재배치 비용
기존 흩어진 경로(`/api/v1/recordings`,`/api/v1/flow`…)를 서비스 세그먼트 아래로 모으는
일회성 작업 필요(예: `/api/v1/calls/recordings`,`/api/v1/calls/flow`). **콘솔은 canonical 도입
릴리스에서 전량 이행**(콘솔·API 동시 배포→스큐 없음). 구 경로는 **deprecation alias** 로만 한시
유지하되 게이트웨이가 응답에 **`Deprecation: true` + `Sunset: <date>` 헤더(RFC 8594)** 부착 + 호출
로깅, 차기 major(`/api/v2`)에서 제거 (D6). alias 실수요자 = 향후 northbound EMS/관제(캐시 클라이언트).

### 프록시 핸들러
`HandlerArgs`(`handler.py:21` — `full_path`/`method`/`query_params`/`headers`/`body`)를 loopback 으로
passthrough:
- method/body/query 전달 + 헤더 화이트리스트(`Authorization`,`Content-Type`,`If-None-Match`…)
- 응답 status/headers/body passthrough — ETag/304, `Content-Disposition`(녹취 다운로드) 보존
- **대용량 응답(녹취 mp4/세그먼트)은 청크 스트리밍** — 전체 버퍼링 금지(메모리·지연)
- 타임아웃 기본 5s, 스트리밍 경로는 별도 장타임아웃
- 구현은 `csc/src/httpsrv/client.py` 재사용

### 인증 공유
- 전 모듈이 동일 `CimsAuth.JwtSecret`(공통 config) 로드 → **각 모듈이 토큰 독립 검증**(base 에
  되묻지 않음). 게이트웨이는 `Authorization` 헤더 전달만.
- 서비스 모듈은 loopback bind(I1) → 프록시 우회 외부 접근 차단.

---

## 6. 콘솔 — full 번들 + 사용자별 구성 (D1)

### 전달층 (b)
- **full 콘솔 번들 1개**에 모든 위젯/페이지를 정적 자산으로 빌드·포함. base 가 단독 서빙(I2).
- 위젯은 **카탈로그(레지스트리)** 로 노출: `{id, title, area(운용/관리), requires_service, default_size}`.

### 구성층 (사용자별, 서버 저장)
```
ConsoleLayout (console account 1개당 1레코드, file_store control/console_layouts/<account>):
  {
    "base_profile": "operator",            // 운용자/관리자/감시자/... 템플릿 id
    "menus":  [ ...영역(운용/관리) 편집 결과... ],
    "pages":  [ {"slug":"/custom/myptt", "widgets":[...]} ],   // 커스텀 페이지
    "widgets": { "dashboard": ["w-call-kpi","w-node-health", ...] },  // 배치/추가/삭제
    "overrides_from_profile": true
  }
```
- **기본 프로파일 템플릿**: `operator|admin|monitor|...` — 시작 메뉴+위젯 세트. 최초 로그인 시 선택
  (또는 admin 이 계정에 할당). 기존 base/full 콘솔 프로파일·메뉴편집·`/custom/<slug>` 인프라 재사용.
- **개인화**: 위젯 추가/삭제/배치, 커스텀 페이지, 영역 편집을 프로파일 위에 레이어. "프로파일로
  초기화" 가능.
- **서버 저장**(file_store, base 소유 `console/` 도메인) → 기기·세션 넘어 따라감. localStorage 아님.
- **위젯 가용성 = 서비스 설치/상태에 종속**: `requires_service` 의 업스트림이 미설치/503 이면 위젯이
  빈 화면이 아니라 **"서비스 일시 불가/미설치"** 명시(장애격리 UX). 사용자가 추가하려 할 때도
  미설치 서비스 위젯은 "설치 후 사용 가능"으로 표기.
- **카탈로그 = (설치된 서비스) ∩ (RBAC 허용) 교집합으로 서버 필터링** (D7): 사용자가 구성 가능한
  위젯 목록을 base 가 사용자 role 로 필터해 내려준다. **단, 위젯이 호출하는 모든 API 는
  게이트웨이/서비스가 RBAC 를 서버측 강제** — 레이아웃이 위젯을 숨기는 것에 보안을 의존하지 않는다
  (심층방어). 레이아웃은 표현 선호, 권한 결정은 서버 권위로 직교.

### 흐름
```
로그인 → base: GET /api/v1/console/layouts/me → {base_profile + overrides}
       → 셸이 카탈로그와 병합해 렌더, 각 위젯은 자기 서비스 API(게이트웨이 경유) 호출
       → 편집 시 PUT /api/v1/console/layouts/me (base 가 저장)
```

---

## 7. 설정 — 공통 + 서비스별 분리 (D4)

```
config/
  common.json            # 전 모듈 공유: JwtSecret, CimsDatabase, Mgmt, CimsRuntimeDir,
                         #   ServiceLogging, BuiltinAccounts (read-only 공유)
  base.json              # base 전용: Server(0.0.0.0:4419), Packages, gateway 기본값
  services/
    csc.json             # csc 전용: bind(127.0.0.1:4421), XCAP, CspNotify, 자기 라우트
    svc-mgmt.json        # svc-mgmt 전용: bind(127.0.0.1:44xx), MediaServer endpoints, 라우트
    <future>.json
```
- **base** 로드 = `common.json` + `base.json` + 게이트웨이 라우트 테이블(설치된 서비스 매니페스트에서
  수집).
- **각 서비스 모듈** 로드 = `common.json` 의 공유항목(JwtSecret/DB/RuntimeDir, read-only) + 자기
  `services/<svc>.json`.
- 장점: 새 서비스 = `services/<svc>.json` 추가 + self-register, **공통/타서비스 설정 무영향**(D4 의도).
- 하위호환: `common.json` 부재 시 기존 단일 `oam.json` 에서 키를 읽는 fallback 유지.

### file_store 소유권 (I5)
| 도메인/컬렉션 | 소유 |
|---|---|
| `control/*`(agents·HA·modules·packages·gateway_routes), `console/*`(layouts·accounts·menu) | **base** |
| `modules/csc/runtime/*`(가입자·PTT·affiliation) | **csc** |
| `modules/svc-mgmt/runtime/*`(verify_runs 등) | **svc-mgmt** |
| `external_systems`, `service_descriptors`, `alerts`(저장) | **base** |
원칙: 쓰기 소유자 단 하나. 서비스가 base 데이터 필요 시 read-only 또는 base API(역방향) 사용, 쓰기 금지.

---

## 8. 프로세스 수명주기 (agent 감독)

`start_oam` 과 대칭으로 각 서비스 모듈을 supervised 등록. **각 모듈은 자기 독립 엔트리포인트**로
기동(D5: csc=`csc_app.py`, svc-mgmt=`svc_mgmt_app.py`). 공통 코드는 공유 OAM-SDK 로 import,
바이너리는 공유하지 않는다.

`agent/lib/lifecycle.sh`:
```sh
start_base_oam()  { "$PYBIN" -u oam_app.py --role "${OAM_ROLE:-all}" --config base.json >> oam.log 2>&1 & }
start_svc_mgmt()  { kill_stray "svc_mgmt_app.py" "$port" tcp
                    "$PYBIN" -u svc_mgmt_app.py --config services/svc-mgmt.json >> svc-mgmt.log 2>&1 & }
# csc 는 기존 start_csc 유지 (독립 모듈)
```
- `run/supervised.json`: `{ "oam":..., "csc":..., "svc-mgmt":..., ... }` → watchdog 독립 auto-restart.
- **`kill_stray`/watchdog pgrep 은 `--role`/config 인자까지 포함 매칭** → base 오살 방지(과거 pgrep
  자기명중 footgun 재발 차단 — 메모리 다발 기록).
- `--preflight` 각 role 지원(핸들러 import + config 검증).
- 기동 순서: **base 먼저(게이트웨이·인증) → 서비스 모듈.** 서비스 지연/실패 시 base 정상, 해당
  라우트만 503(I3).

---

## 9. 장애 격리 시나리오 (검증 기준)

| 시나리오 | 기대 동작 |
|---|---|
| 서비스 모듈 crash/OOM | base 생존: 로그인·시스템관리·heartbeat 정상. 해당 위젯만 "불가". watchdog 재기동 → 자동 복구 |
| 서비스 모듈 지연/무한루프 | 프록시 타임아웃 → 503, base 경로 무영향 |
| 서비스 미설치(부트스트랩 1~2단계) | 라우트 미등록 → 503, base 단독 정상. 위젯 "미설치" 표기 |
| 서비스 모듈 업그레이드 | base 무중단; 프록시 503 창 → 신버전 기동 후 자동 정상 |
| 새 서비스 추가 | 매니페스트 self-register → 라우트 한 줄 추가, 게이트웨이/타서비스 무재기동 |
| base OAM 재기동 | 전체 콘솔 일시 중단(공개 진입점이라 불가피) |

---

## 10. 버전 계약

- 교차 의존 = **service → base 최소 버전**. 서비스 매니페스트에 `requires.base_oam >= X.Y.Z` 선언,
  base 가 self-register 시 대조 → 불일치 경고/거부.
- 위젯과 그 API 는 같은 서비스 모듈이라 모듈 내부에서 이미 정합(스큐 없음).
- 호환 매트릭스를 릴리스 노트에 명시.

---

## 11. 도입 단계 (점진·하위호환)

| Phase | 내용 | 위험 |
|---|---|---|
| **P0** ✅ | 핸들러 BASE/SERVICE 그룹화 + `--role {base\|all}` 플래그(기본 `all`). stats 함수 단위 분리(`handle_stats_service`/`/api/v1/stats/service`). **동작 0 변경**(라우트 테이블 diff: `all`=현행 + `/api/v1/stats/service` 전용 핸들러만 추가, 핸들러 선택 무변경; `base`=서비스 라우트 부재). `oam_app.py`·`handlers/stats.py`. | 낮음 |
| **P1** ✅ | 게이트웨이(`handlers/gateway.py`: file_store `control/gateway_routes` 라우트 테이블 + aiohttp 프록시[method/body/query/header 화이트리스트 passthrough·ETag/304·Content-Disposition 보존·RFC8594 Deprecation/Sunset·loopback 업스트림 강제 I1] + self-register API `/api/v1/gateway/routes`) + `common.json`/`base.json` 분리(`load_config` 비파괴 fallback, `.sample` 동봉). `--role base` 에서만 프록시 마운트(all 은 in-process). 기본은 여전히 단일프로세스(`all`). | 낮음 |
| **P2** ✅ | csc 가입자 API in-process import → **게이트웨이 프록시로 전환**(D3). `--role base` 에서 D8 `/me` 분리(base 가 `/api/v1/users/me` 직접, 나머지 `/users/*`·`/users/import`·`/ptt/groups`·`/organizations` 는 csc 프록시) + 라우트 시드를 csc 실경로(admin 4421/TCP)로 정정 + loopback-https 업스트림 TLS 검증 스킵(self-signed). **dev 1노드 E2E PASS**(격리포트 24419/24421, 라이브 DB: 로그인→/me=base[`builtin:true`]·/users·/users/2·/organizations=csc 프록시 200, gateway health 3 route alive). ⚠️분리 배포는 모듈간 JwtSecret 통일 필수(common.json §5; 현 dist 는 oam≠csc 시크릿). all 모드는 in-process 유지(production 무변경). | 중 |
| **P3** ✅ | svc-mgmt 독립 모듈 `svc-mgmt/src/svc_mgmt_app.py`(stats/service·recording·flow·verification, loopback 4480, `--preflight`, common.json+services/svc-mgmt.json 로드) + 게이트웨이 svc-mgmt 라우트 시드 + `services/svc-mgmt.json.sample` + agent `lifecycle.sh start_svc_mgmt/stop_svc_mgmt`(dormant: `all` 미포함, 명시 기동만, kill_stray 고유 절대경로 매칭). **dev E2E PASS**(격리 24419/24480: /stats/health=base-local·/stats/service·/verification·/recordings=svc-mgmt 프록시 200, longest-match /stats vs /stats/service 공존). ⚠️**공유 OAM-SDK = 물리적 `oam-sdk/` 패키지 추출은 후속(P3.x)**: 현재는 svc_mgmt_app.py 가 기존 공유 모듈(csc/src httpsrv·services + oam/src handlers)을 sys.path import(oam↔csc 가 csc/src 공유하던 패턴과 동일 = de-facto SDK). 물리 추출은 빌드/패키징 재작업과 함께. | 중 |
| **P4** ✅ | 콘솔 D1. **백엔드** `handlers/console_layouts.py`: `GET /console/catalog`(RBAC 필터 + 서비스 가용 annotate D7) · `GET /console/profiles`(role별 템플릿) · `GET/PUT/DELETE /console/layouts/me`(override↔프로파일, **PUT 서버측 RBAC 강제**=권한밖 403·미존재 400). 도메인 `console_user_layouts`(console, base 소유). 설치판정=게이트웨이 라우트∪in-process. **프런트** `api/consoleLayouts.ts` + `pages/MyLayoutPage.tsx`(`/dashboard/my-layout` = 프로파일 picker + 위젯 add/remove/↑↓ + 저장/되돌리기/초기화 + 카탈로그 가용성 배지·미설치 disabled+안내[503 graceful] + override/profile 표시). **dev E2E PASS**(admin/monitor 카탈로그 RBAC·svc 가용 flag·layouts CRUD·403/400/401) + `tsc -b`·`vite build`·eslint PASS(브라우저 실측은 미수행; reorder 는 ↑↓ — dnd 고도화 여지). | 중 |
| **P5** | production(4노드) 적용, base/full 콘솔 단계 정합, §9 장애격리 검증 | 중 |

P0~P1 동안 운영은 단일 프로세스 그대로.

---

## 12. 영향 파일 (예상)

- `oam/src/oam_app.py` — `--role {base|all}` 분기, 핸들러 그룹화, 게이트웨이 등록
- **`oam-sdk/`** *(신규, D5)* — 공유 라이브러리(httpsrv·auth·file_store·핸들러 스캐폴딩) 추출
- **`svc-mgmt/src/svc_mgmt_app.py`** *(신규, D5)* — svc-mgmt 독립 엔트리포인트(OAM-SDK 의존)
- `oam/src/handlers/gateway.py` *(신규)* — 라우트 테이블 + 프록시 + self-register API + Sunset alias
- `oam/src/handlers/console_layouts.py` *(신규)* — 사용자별 레이아웃 CRUD (D1)
- `oam/src/handlers/stats.py` — base(health)/svc-mgmt(KPI) HANDLER_LIST 분리
- `oam/config/` — `common.json`/`base.json`/`services/*.json` 도입(+`oam.json` fallback)
- `agent/lib/lifecycle.sh` — `start_base_oam`/`start_svc_mgmt`, `kill_stray` role 매칭
- `agent/cims_agent.py` — `supervised.json` 다중 모듈, watchdog/preflight
- `csc/src/csc_app.py` — 가입자/PTT 관리 API self-register(게이트웨이 등록 메타)
- `csc/src/httpsrv/client.py` — 스트리밍 passthrough 보강(필요 시)
- 콘솔(`cims-console/src/...`) — 위젯 카탈로그·레이아웃 store·프로파일/개인화 UI·503 graceful
- 문서 — 본 문서 + CLAUDE.md OAM 절 갱신

---

## 13. 결정 (표준 관점 권고 확정)

> 선택 기준 = 구현 난이도가 아니라 **업계 표준/규범**. 각 항목에 따르는 표준을 명시한다.

### D5. svc-mgmt 엔트리포인트 = **독립 아티팩트 + 공유 OAM-SDK 라이브러리**
- **결정**: svc-mgmt 는 `oam_app.py --role svc-mgmt` 재사용이 아니라 **자기 엔트리포인트·자기
  패키지·자기 버전**을 갖는 독립 모듈(csc 가 `csc_app.py` 인 것과 동일)로 만든다. 공통 코드
  (httpsrv·auth·file_store·핸들러 스캐폴딩)는 **공유 라이브러리(OAM-SDK)** 로 추출해 base·csc·
  svc-mgmt 가 각자 import.
- **근거(표준)**: 12-factor / microservice 패키징 — "독립 배포 단위는 독립 아티팩트". 단일 바이너리
  + 역할 플래그는 release·버전·배포를 한 덩어리로 묶어 모듈 독립성(D3)을 무효화한다. 공통은
  **바이너리 공유가 아니라 라이브러리 공유**로 해결하는 것이 정석.
- `--role {base|all}` 플래그는 **base 의 하위호환(현행 단일프로세스)용으로만** 잔존, 서비스 모듈엔
  쓰지 않는다.

### D6. 경로 재배치 = **canonical 즉시 이행 + RFC 8594 Sunset 기반 한시 alias**
- **결정**: 신규 canonical 경로(`/api/v1/<service>/...`)를 도입하는 **같은 릴리스에서 콘솔을 전량
  canonical 로 이행**(콘솔·API 동시 배포라 스큐 없음). 구 경로는 **deprecation alias** 로만 한시 유지하되
  응답에 **`Deprecation: true` + `Sunset: <date>` 헤더**(RFC 8594) 부착 + 호출 로깅, 차기 major
  (`/api/v2` 또는 명시 sunset)에서 제거.
- **근거(표준)**: REST API 진화의 표준 deprecation 절차(IETF Deprecation/Sunset 헤더). "조용한 alias
  무기한 유지"는 안티패턴 — 만료일·헤더·체인지로그로 명시 회수. alias 의 실수요자는 콘솔이 아니라
  **향후 northbound EMS/관제**(별도 캐시 클라이언트)이므로 표준 sunset 통지가 특히 중요.

### D7. 프로파일/위젯 = **RBAC(인가)와 레이아웃(표현)의 직교 + 서버측 강제**
- **결정**: RBAC 는 **무엇에 접근 가능한가**(인가, 권위 모델), 프로파일/레이아웃은 **기본으로 무엇을
  보는가**(표현 선호)로 **직교 분리**. 사용자가 구성 가능한 위젯 카탈로그 = **(설치된 서비스) ∩
  (RBAC 허용 리소스)** 의 교집합으로 **서버가 필터링**하고, **모든 API 는 게이트웨이/서비스에서
  RBAC 를 서버측 강제**한다(레이아웃이 위젯을 숨기는 것에 보안을 의존하지 않음). 기본 프로파일
  템플릿은 기존 5 role(admin/manager/operator/monitor/user)에 **정렬된 큐레이션 레이아웃**.
- **근거(표준)**: 최소권한 + 심층방어(defense-in-depth) — 클라이언트(UI) 필터는 편의일 뿐, 권한
  결정은 서버 권위. 표현(레이아웃)과 인가(RBAC)의 분리는 IAM 정석.

### D8. 가입자 API = **identity-plane(base) vs resource-plane(csc) 분리**
- **결정**: `/api/v1/auth/*`·`/api/v1/console/accounts`·**`/api/v1/users/me`(= 인증된 콘솔 운용자
  본인 신원/프로파일)** 는 **base(identity plane)** 유지. **가입자(telecom subscriber) CRUD 전부는
  csc(`/api/v1/subscribers/*`, resource plane)** 로 이관. 현 OAM in-process import(가입자 핸들러)는 제거.
- **근거(표준)**: 관심사 분리 — **`/me` 는 "인증 주체 자신"이라 IdP(base)의 책임**이지 피관리
  도메인 리소스가 아니다. 가입자는 시스템이 관리하는 도메인 엔티티이므로 서비스(csc) 소유. 메모리
  기록의 DB 분리(콘솔계정→OAM `console_accounts`, DB `users`=person 전용)와 정확히 일치.
