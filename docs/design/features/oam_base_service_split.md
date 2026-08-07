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
> 관련: [csc_standalone_module.md](csc_standalone_module.md) — 각 모듈이 자기 인프라를 자체 보유하고
> **계약(HTTP/JWT/DB)만으로 결합**(I3 단방향 의존을 코드 수준에서 실현).

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
  게이트웨이만 접근하는 **비공개 upstream** — 동거 배치는 loopback(127.0.0.1, 기본),
  모듈이 base 와 다른 호스트에 배치되면(분리 토폴로지) 운영자가 그 모듈 배포 설정
  `Server.GatewayHost` 에 그룹 VIP(권장)/노드 IP 를 명시해 게이트웨이가 그 주소로 프록시
  (self-register/설정 저장 시 자동 재등록, HA 절체는 VIP 가 추종). CORS·방화벽 공개 포트
  추가 없음, nginx 재도입 없음.
- **I2. 콘솔 정적 자산은 base 가 전부 서빙** — 셸 + 전 위젯 청크. 서비스 모듈은 API 만 담당.
- **I3. 의존 방향 단방향** — service → base(인증/형상 조회) 허용, **base → service 의존 금지**.
  서비스 모듈 부재 시 base 정상 동작(해당 서비스 라우트만 503/404).
- **I4. 하위 호환** — 단일 프로세스(현행)로도 계속 기동 가능(`--role all`). 분리는 옵트인.
- **I5. file_store 컬렉션 단일 소유자** — 동일 컬렉션을 두 프로세스가 동시 쓰지 않는다.

---

## 2. 기반 메커니즘

이 설계가 의존하는 인프라:
- 라우팅: `csc/src/httpsrv/controller.py` — 등록 base_path 중 **최장 일치** 디스패치.
  → 세그먼트 prefix 기반 게이트웨이가 추가 프레임워크 없이 성립.
- 핸들러 등록: `ems/core/oam/src/oam_app.py` `add_dynamic_rules([(base_path, fn, kwargs)])`.
- 감독: `agent/lib/lifecycle.sh` `start_oam`(+`kill_stray`), `agent/cims_agent.py`
  watchdog 가 `run/supervised.json {module: install_path}` desired-state 로 auto-restart,
  `oam_app.py --preflight` 교체 전 검증.
- 인증: `CimsAuth.JwtSecret` + `BuiltinAccounts`.

---

## 3. 구조 — 게이트웨이 + 독립 서비스 모듈

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

> **명명(D3):** "service OAM" 단일체는 없다. base 뒤에 csc·svc-mgmt·미래서비스 등
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
| `alerts`(저장·조회 + agent 계열 발화) | base 공통 | `/api/v1/alerts` | 골격 공통(서비스 계열은 oam-svc 가 push) |
| `admin`,`org`(가입자 CRUD) | **csc 모듈** | `/api/v1/subscribers`, `/api/v1/org` | D3 가입자=csc 도메인 |
| (PTT 그룹/affiliation 관리) | **csc 모듈** | `/api/v1/ptt` | D3 |
| `stats` **전체** (health/subscribers/messages/leak + service KPI) | **oam-svc 모듈** | `/api/v1/stats` | 서비스 관측 데이터(CSP/CMP probe·DB·서비스 로그) — 소비자도 svc 콘솔 팩 위젯뿐 |
| `recording`, `flow_logger` | **oam-svc 모듈** | `/api/v1/calls` | 녹취·SIP flow 관측 |
| `verification` | **oam-svc 모듈** | `/api/v1/verification` | S1~S6 검증(testbed) |

> `stats` 는 `/api/v1/stats` 세그먼트 **전체가 oam-svc 귀속** — base 는 stats 를 직접 서빙하지
> 않고(`--role base` 미등록) 게이트웨이 프록시만 한다. 콘솔 URL 은 불변(위젯/페이지 무수정).
> `--role all`(단일 프로세스)에서는 SERVICE 그룹으로 in-process 등록되어 동작 무변경.
> **배포 제약**: base 와 oam-svc 는 이 경계를 맞춘 버전으로 **함께 배포**해야 한다 — stats 미등록
> base + `/api/v1/stats` 라우트 없는 구 oam-svc 조합이면 `/stats/*` 404 (라우트는 oam-svc
> 배포 시 self-register 로 갱신).
> 가입자 CRUD 핸들러는 현재 OAM in-process import(`oam_app.py:118`) → 목표는 **csc 가
> 직접 서빙하고 base 가 프록시**(중복 제거).

> `service_control`(`/api/v1/services`) 은 로컬 호스트의 `cims-svc`(agent 번들 운영 도구)를
> subprocess 로 구동한다. 스크립트 위치 해석: `CIMS_SVC_PATH`(명시 오버라이드) →
> `$CIMS_AGENT_PREFIX/agent/current/bin/cims-svc`(배포 환경 정본 — agent.md §3 prefix 규약,
> agent 가 모듈 기동 시 env 상속으로 전달; `current` 심링크 = systemd/sudoers 와 동일한 버전
> 무관 고정 경로) → 레포/dist 트리 walk-up(`agent/bin`, `agent/current/bin`) fallback.
> 이 API 는 **개발서버(devMode 릴리스 메뉴) 도구** — 배포 환경에서 모듈 생명주기의 정본은
> agent job(`POST /api/v1/deployments/{id}/job` start/stop/restart)이며, cims-svc 직접 제어는
> agent supervised(HA 자동복구)와 이중 제어가 될 수 있어 배포 환경에선 상태조회 용도로만 쓴다.

**알람 sweeper 분리** (`services/alarm_sweeper.py` 공용 코어):
- **서비스 계열**(`csp_down`/`cmp_down`/`db_down`/`rtp_high`, scope≠`agent`) 평가·발화 = **oam-svc**
  (`detected_by='oam-svc'`) — probe 대상·DB 가 oam-svc 설정이므로. `--role all` 에서는 base 가
  대행 평가(`detected_by='oam'`).
- **agent 계열**(`disk_high`/`module_down`, scope=`agent`, heartbeat 메트릭 기반) = **base** 잔류.
- 저장(`alert_log` → `ServiceLogging.Dir`)·조회 API(`/api/v1/alerts`)는 base 소유 불변 — 동거
  노드 전제로 양쪽이 같은 디렉토리에 기록하고, 기동 시 open-state 복원은 소유 계열만
  (`restore_open_state` scope: 서비스=`cims/*` mo, agent=그 외).

base 엔트리포인트 역할 분기:
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
  /api/v1/calls        → oam-svc  (127.0.0.1:4480)
  /api/v1/stats        → oam-svc  (health/subscribers/messages/leak + service KPI 전체)
  /api/v1/verification → oam-svc
  (그 외 /api/v1/*      → base 직접 처리)
```

- `controller.py` **최장 일치** 규칙 덕에 base 고유의 더 구체적 경로(`/api/v1/users/me` 등)는
  base 가 우선 — 충돌 없이 공존. `/api/v1/stats` 는 base 직접 경로가 없어 세그먼트 전체가 프록시.
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
- 하위호환: `common.json` 부재 시 자기 `oam-svc.json` 단독. **base `oam.json` 상속(fallback)은
  없다** — oam-svc 는 자기 설정(배포 overlay `config.json` 또는 `oam-svc.json`)만 읽는 완전
  독립 설정 모듈이다(csp/cmp/csc 와 동일 모델). base 와 공유해야 하는 값(`CimsAuth.JwtSecret`/
  `CimsRuntimeDir`/`Mgmt.Cidr`)은 상속이 아니라 **배포 시 base OAM 이 주입**한다(아래 실체화
  참조). 설정 파일이 하나도 없으면 기동 로그에 명시적 에러를 남기고 preflight 가 실패한다 —
  빠진 설정을 코드 기본값이 조용히 메워 오동작하는 경로를 두지 않는다.
  `oam-svc.json` 을 직접 쓰는 경우(패키지 동봉/dev)는 공유값(특히 base 와 동일해야 하는
  `CimsAuth.JwtSecret`)까지 그 파일에서 채워야 한다(`oam-svc.json.sample` 참조).

### 서비스 관측 설정의 소유 — oam-svc (콘솔 관리)
`CimsDatabase`/`CspNotify`/`MediaServer.Endpoints`/`ServiceLogging` 은 **서비스 관측 설정으로
oam-svc 소유**다. 정규 관리 경로는 콘솔 배포설정 — oam-svc `config_template.json` 의
`db`/`probe`/`logging` 섹션(csp/csc 와 동일 관례) → `PUT /deployments/<id>/config` →
`update_config` job → 배포 overlay(`config.json`). 우선순위:

```
배포 overlay(콘솔 설정, 실체화됨)  >  oam-svc.json(패키지 동봉 시)
```

**`config.json`(배포 overlay)이 완전한 유효 설정이다 — csc/csp/cmp 와 동일.** 이를 보장하는
주체는 콘솔 UI 가 아니라 **백엔드 실체화**(`agents._materialize_deploy_config`)다: OAM 이
install/upgrade/update_config job 을 디스패치할 때 ① `config_template` 전 필드의 `default`
를 base 로 깔고 ② deployment 레코드의 overlay(사용자 변경분)를 병합하고 ③ 게이트웨이 서비스
모듈(meta.gateway.routes 보유)에는 base 소유 공유값(`CimsAuth.JwtSecret`/`CimsRuntimeDir`/
`Mgmt.Cidr`, 비어있으면 `ServiceLogging.Dir`)을 주입해 완전한 config 를 agent 에 전달한다.
deployment **레코드는 sparse overlay(사용자 변경분)로 유지** — template default 가 바뀌면
다음 job 디스패치에서 자동 추종되고, template 에 필드가 늘어도 기존 배포가 재배포/설정저장
시 자동으로 완전한 config.json 을 받는다(빈 default `''`/`[]` 는 '미설정' 시맨틱 보존을 위해
실체화에서 제외). 따라서 `config_template.json` 이 구조·기본값의 SoT이며,
그 `default` 는 **환경 비종속 중립값**(예: `CimsDatabase.Host=127.0.0.1`, `CimsDatabase.User=cims`,
`CspNotify.Ip=127.0.0.1`)으로 두고 실주소는 배포 시 콘솔에서 채운다(레포에 테스트베드 IP 금지).
Python 서비스 모듈(csc·oam-svc)은 C++ 과 달리 base conf 부재를 tolerate 하므로 `make dist`
base conf 생성(`gen_default_config`) 대상이 아니다 — `config.json` 에 의존한다.

**base `oam.json` 은 base 전용 설정만 갖는다** — 서비스 관측 키(`CimsDatabase`/`CspNotify`/
`CmpIp`·`CmpPort`/`MediaServer`)를 두지 않으며, **`--role base` 는 이 키들을 읽지 않는다**
(base 프로세스는 DB 미접속). 예외는 `ServiceLogging` — base 도 agent 계열 알람(alert_log)
저장·조회와 콘솔 flow 기록에 쓰는 공유 키라 `oam.json` 에 남으며, oam-svc 콘솔 설정이 비어
있으면 배포 실체화가 base 값을 주입한다. `--role all`(단일 프로세스 dev/TB)에서 서비스 관측이 필요하면 키를
배포 overlay(`config.json`) 또는 로컬/TB 설정(`oam-tb.json` 등)으로 제공한다 — 레포 `oam.json`
에는 두지 않는다.

`MediaServer.Endpoints`(type `object_list`, `item_schema.fields = [{ip:string}, {port:int}]`)는
`[{ip,port}, ..]` **배열**로 저장·소비된다. 콘솔은 공용 `ObjectListEditor`(ip/port 행 + `＋`로 추가,
최소 1행)로 편집한다(`ModuleConfigModal`·`ModuleConfigEditor` 공유). 레거시 콤마 문자열
`"ip:port, .."`/`["ip:port"]` 도 세 지점에서 `[{ip,port}]` 로 정규화·수용한다: (1) 콘솔 위젯
(`ObjectListEditor` 가 로드 시 `"host:port"` → `{ip,port}` 변환), (2) 백엔드 coerce
(`agents._coerce_list_fields` + 모듈 모드 `modules._coerce_value` 의 `object_list` 분기 =
`_coerce_object_list`), (3) 소비자(`stats._media_endpoints` 가 문자열/`{ip,port}` dict 모두 수용).
**CMP 관측은 전 노드 평가**(AA 다중 노드): 대시보드
health 위젯은 전 노드 probe 집계(up = any 노드 응답, 카운터는 합산)이고, 알람 sweeper 의
`process_down(target=cmp)` 는 endpoint 마다 `mo_instance='cims/cmp/<ip>:<port>'` 로 개별
발화한다. `Endpoints`/`CmpIp` 미설정이면 CMP 관측 비활성(cmp 계열 규칙 skip).

> **주의 — 이름은 같지만 평면이 다르다.** 여기 oam-svc 의 `MediaServer.Endpoints` 는 **관측(STATS
> probe)** 전용이다. CSP 가 실제 relay 세션을 CMP 들에 분배하는 **데이터 평면** 설정은 별개의
> `Setup.MediaServer.Endpoints`(csp `config_template.json`, C++ `SipServerSetup`/`CmpClient` 소비)이며
> 표현형(`object_list [{ip,port}]`)과 편집 위젯은 동일하다. 둘은 독립 설정이므로 다중 CMP 운영 시 양쪽에
> 같은 노드 목록을 넣는다. CSP 데이터평면 상세는 modules/csp.md §3.6.

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
# role 우선순위: env OAM_ROLE(개발 오버라이드) > 배포 설정 Server.Role > all(코드 기본)
start_base_oam()  { "$PYBIN" -u oam_app.py --role "$oam_role" --config base.json >> oam.log 2>&1 & }
start_svc_mgmt()  { kill_stray "svc_mgmt_app.py" "$port" tcp
                    "$PYBIN" -u svc_mgmt_app.py --config services/svc-mgmt.json >> svc-mgmt.log 2>&1 & }
# csc 는 기존 start_csc 유지 (독립 모듈)
```
- `run/supervised.json`: `{ "oam":..., "csc":..., "svc-mgmt":..., ... }` → watchdog 독립 auto-restart.
- **`kill_stray`/watchdog pgrep 은 `--role`/config 인자까지 포함 매칭** → base 오살 방지(과거 pgrep
  자기명중 footgun 재발 차단 — 메모리 다발 기록).
- `--preflight` 각 role 지원(핸들러 import + config 검증).
- **역할의 SoT 는 배포 설정 `Server.Role`**(oam `config_template`, 기본 `base`). 환경변수는
  개발 오버라이드로만 남는다 — systemd drop-in(env)에만 role 이 있으면 그 drop-in 이 없는
  노드에서 HA 승격으로 기동될 때 `role=all` 로 떠 게이트웨이 프록시를 마운트하지 않아
  승격 직후 서비스 API 가 전면 장애난다([oam_ha.md](oam_ha.md) §6.6).
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

## 11. 구성 요소

| 구성 | 내용 |
|---|---|
| 역할 플래그 | `oam_app.py --role {base\|all}`(기본 `all`). `all`=단일프로세스(하위호환), `base`=게이트웨이 프록시 마운트. stats 함수 단위 분리(`handle_stats_service`/`/api/v1/stats/service`). |
| 게이트웨이 | `handlers/gateway.py`: file_store `control/gateway_routes` 라우트 테이블 + aiohttp 프록시(method/body/query/header 화이트리스트 passthrough·ETag/304·Content-Disposition 보존·RFC8594 Deprecation/Sunset) + self-register API `/api/v1/gateway/routes`. upstream host = 배포 설정 `Server.GatewayHost`(운영자 명시, 분리 배치 시 그룹 VIP/노드 IP) → 미지정 시 loopback. https upstream 은 TLS 검증 스킵(self-signed 전제). 라우트 lifecycle: 배포 생성 시 등록, **같은 process 의 마지막 배포 삭제 시에만 deregister**(AS 피어 잔존 시 유지), 개별 배포/그룹 공통 설정 저장으로 실효 host/port 가 바뀌면 재등록, 기동 시 배포↔테이블 정합 self-heal(`reconcile_routes_from_deployments` — 빠진 세그먼트 복구). |
| 설정 분리 | `common.json`/`base.json`(`load_config` 비파괴 fallback, `.sample` 동봉). |
| csc 프록시 | csc 가입자 API 는 게이트웨이 프록시(D3). `--role base` 에서 D8 `/me` 분리(base 가 `/api/v1/users/me` 직접, 나머지 `/users/*`·`/users/import`·`/ptt/groups`·`/organizations` 는 csc 프록시, admin 4421/TCP). loopback-https 업스트림 TLS 검증 스킵(self-signed). **분리 배포는 모듈간 JwtSecret 통일 필수**(common.json §5). |
| svc-mgmt | 독립 모듈 `svc-mgmt/src/svc_mgmt_app.py`(stats/service·recording·flow·verification, loopback 4480, `--preflight`, common.json+services/svc-mgmt.json 로드) + 게이트웨이 svc-mgmt 라우트 + agent `lifecycle.sh start_svc_mgmt/stop_svc_mgmt`(`all` 미포함, 명시 기동만). |
| 콘솔 레이아웃(D1) | 백엔드 `handlers/console_layouts.py`: `GET /console/catalog`(RBAC 필터 + 서비스 가용 annotate D7)·`GET /console/profiles`(role별 템플릿)·`GET/PUT/DELETE /console/layouts/me`(override↔프로파일, PUT 서버측 RBAC 강제=권한밖 403·미존재 400). 도메인 `console_user_layouts`(console, base 소유). 프런트 `api/consoleLayouts.ts` + `pages/MyLayoutPage.tsx`(`/dashboard/my-layout` = 프로파일 picker + 위젯 add/remove/↑↓ + 저장/되돌리기/초기화 + 카탈로그 가용성 배지·미설치 disabled+안내[503 graceful]). |

> **공유 OAM-SDK**: 물리적 `oam-sdk/` 패키지 추출은 향후 과제. 현재 svc_mgmt_app.py 는 공유 모듈
> (csc/src httpsrv·services + ems/core/oam/src handlers)을 sys.path import(de-facto SDK). 물리 추출은
> 빌드/패키징 재작업과 함께.

---

## 12. 영향 파일 (예상)

- `ems/core/oam/src/oam_app.py` — `--role {base|all}` 분기, 핸들러 그룹화, 게이트웨이 등록
- **`oam-sdk/`** *(신규, D5)* — 공유 라이브러리(httpsrv·auth·file_store·핸들러 스캐폴딩) 추출
- **`svc-mgmt/src/svc_mgmt_app.py`** *(신규, D5)* — svc-mgmt 독립 엔트리포인트(OAM-SDK 의존)
- `ems/core/oam/src/handlers/gateway.py` *(신규)* — 라우트 테이블 + 프록시 + self-register API + Sunset alias
- `ems/core/oam/src/handlers/console_layouts.py` *(신규)* — 사용자별 레이아웃 CRUD (D1)
- `ems/core/oam/src/handlers/stats.py` — base(health)/svc-mgmt(KPI) HANDLER_LIST 분리
- `ems/core/oam/config/` — `common.json`/`base.json`/`services/*.json` 도입(+`oam.json` fallback)
- `agent/lib/lifecycle.sh` — `start_base_oam`/`start_svc_mgmt`, `kill_stray` role 매칭
- `agent/cims_agent.py` — `supervised.json` 다중 모듈, watchdog/preflight
- `csc/src/csc_app.py` — 가입자/PTT 관리 API self-register(게이트웨이 등록 메타)
- `csc/src/httpsrv/client.py` — 스트리밍 passthrough 보강(필요 시)
- 콘솔(`ems/core/console/src/...`) — 위젯 카탈로그·레이아웃 store·프로파일/개인화 UI·503 graceful
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

---

## 14. 콘솔 배포·설정 UX

콘솔 배포/설정 UX는 **공통 메커니즘이라 모든 모듈에 일괄 적용**된다.

### 14.1 배포 마법사 — 모듈 이름 + 설명
- `service.functions` 는 사용하지 않는다(`pkg.json` 은 `processes` 만 유지).
- `ServersPage.tsx` 배포 마법사: **"3. 모듈 이름"**, **"4. 설명"**(패키지 meta `description` 읽기전용 표시).
  모듈 테이블 컬럼 **"이름"·"설명"**.
- `csc/pkg.json`·`oam-svc/pkg.json` `description`: csc=가입자·조직·인증·MCPTT(XCAP), 이력/통계/녹취=oam-svc.

### 14.2 설정 편집 — 공통은 그룹 탭, 개별은 서버 탭
설정의 편집 창구가 유효 scope 로 갈린다. **유효 scope = `field.scope ?? section.scope
?? "service"`** — 섹션 안에 공통값·노드별 값이 섞이면 필드에 scope 를 줘 오버라이드한다
(예: csp `media_server` 는 service 지만 `Setup.MediaServer.LocalIp` 는 system).
프론트 `effectiveScope`(`deployment.ts`)·`serviceScopeKeys`·`sectionForScope`
(`ModuleConfigModal.tsx`)와 백엔드 `_effective_scope`/`_service_scope_keys`(`agents.py`)가
같은 규칙을 공유한다.

- **AS 그룹의 공통(service) 설정·컬렉션 = 그룹 탭이 유일한 편집 창구.** 그룹 선택 →
  [패키지 설정](`GroupConfigCompareView`) → 패키지별 탭 → [공통 설정] 편집 폼 +
  공통 컬렉션 탭 + [멤버 비교] 표 + **동기화 스위치**(§14.6).
- **AS 그룹 멤버 서버의 [패키지 설정] 탭(`ModuleConfigModal`) = 서버 개별(system)
  설정·컬렉션만 노출.** 공통 필드는 화면에 없으며, 안내 배너가 그룹 탭으로 유도한다.
  저장은 항상 그 서버에만 적용(`PUT /deployments/{id}/config` — 전파 없음).
- **AA 그룹·standalone 은 동기화 개념이 없다** — 서버 화면에서 전체 섹션·컬렉션 편집.
  AA 그룹의 그룹 탭은 [멤버 비교] 표만 제공(정보성 드리프트 표시).
- `_infra`(Infrastructure) 섹션은 **전부 서버 개별(system)** — 배포 시 configure.sh 가
  `deploy_value` 로 자동 주입하는 서버별 인프라 값이므로 그룹 공통 화면에 노출하지
  않고, 서버 화면의 접힌 "인프라" 블록으로만 보인다. (시크릿 등 멤버 간 동일해야
  하는 인프라 값은 배포 시 자동 주입이 정합을 담당 — §14.3 JwtSecret 주입.)

### 14.3 설치 전(pending) 배포도 설정 가능
- 설정 탭(`AgentConfigTab`)은 `pending` 배포도 포함 → 설치 전에
  DB/notify/시크릿을 미리 지정. 저장값은 **deployment.config overlay** 로 보존됐다가 설치 시
  `<pkg>/config.json` 에 반영(install 경로가 overlay 적용, `agents.py` §config overlay).
- pending 에선 프로세스가 없어 restart 불가 → "저장+재기동" 버튼/배너 숨기고 "설치 시 반영" 안내.
- base↔csc 연동(JwtSecret)은 **배포 시 자동 주입**(`_create_deployment`: pkg meta 에 `gateway.routes`
  존재 + overlay 에 `CimsAuth.JwtSecret` 부재 시 base 시크릿 주입) → 수동 설정 불필요.

### 14.4 모든 설정 노출
- config_template 에 `presets` 는 없다(csc·csp·cmp). 디폴트 SoT = 각 필드 `default`/`deploy_value`.
- UI 에 "추천 설정" 바·"Preset 일괄 적용" 탭·"고급 설정/고급 필드" 토글이 없다
  (`ModuleConfigModal`·`ModuleConfigEditor`). 섹션/필드 `hidden`·`advanced`
  게이팅 없음 → **모든 섹션·필드 항상 노출**(시크릿/경로의 `_infra` 섹션은 "인프라" 배지 + 기본 접힘,
  헤더 클릭으로 펼침 — 숨김 아님).

### 14.5 패키지 업로드 핸들러
- `_dt(val)` 는 file_store 의 ISO 문자열에 대해 `hasattr(val,"isoformat")` 가드 후 변환
  (동일 버전 재업로드 409 conflict 응답 직렬화 시 500 방지, `ems/core/oam/src/handlers/agents.py`).

### 14.6 HA 자동 동기화 — 스위치 + ACTIVE→STANDBY 자동 교정
AS 그룹의 공통 설정 정합은 **그룹×패키지 단위 동기화 스위치**(기본 ON)와 **자동 교정
데몬**이 담당한다. 저장 API 자체에는 HA 전파가 없다.

**실측 ACTIVE 판정** (`ha_lookup.vip_observation`):
- agent 가 heartbeat(기본 2s 주기)로 보고하는 `interfaces[]`(secondary IP 포함)에 그룹
  VIP(`vip_bindings[].ip ∪ vip`)가 붙은 멤버를 찾는다 — agent 수정 없이 기존 데이터 소비.
- **비-stale**(heartbeat ≤90s) 멤버 중 **정확히 1명**이 보유할 때만 ACTIVE 확정.
  0명(VIP 이동 중)·2명(절체 직후 관측 창)·전원 stale → 판정 불가(None).
- `GET /ha-groups` 응답에 `active_agent_id` + 멤버별 `vip_observed`(true/false/null) —
  콘솔 뱃지(`● ACT`/`○ SBY`)가 정적 role 과 별개로 실제 절체를 표시 — 지연은
  agent heartbeat(2s) + 콘솔 폴링(10s) ≈ 최대 12초.
  ServersPage 의 [🔄 실측](sync health-check)은 즉시 재확인용으로 존치.

**동기화 스위치** (`PUT /ha-groups/{gid}/packages/{pkg}/auto-sync {enabled}`, operator):
- `ha_group.auto_sync[pkg]` 영속, **부재 = ON**. AS 그룹만 존재(AA/standalone 은 없음).
- ON 전환 시 즉시 정합 1회(`reconcile_group_package`) — 판정 불가·버전 혼재면 보류 사유를
  응답에 담고 스위퍼가 조건 충족 시 자동 재시도.

**자동 교정** (`agents.py reconcile_group_package` — oam_app `[auto-sync]` 스위퍼가 주기
실행, 기본 60s / 컬렉션은 매 5라운드):
- 대상: AS 그룹 × 스위치 ON 패키지. **ACTIVE 멤버의 overlay 를 기준으로** STANDBY 의
  유효 scope=service 키를 merge, ACTIVE 에 없는 service 키는 제거(기본값 복귀) →
  STANDBY 의 유효 공통값이 ACTIVE 와 정확히 일치. scope=system 키는 절대 건드리지 않음.
- scope=service 컬렉션도 ACTIVE records 기준 복사(hash 동일 시 PUT 생략).
- 교정 시 target 별 update_config job + `sync_txn(op=auto_sync)` — 이력 조회 가능.
- **안전 원칙 — 애매하면 복사하지 않는다**: 스위치 OFF·ACTIVE 판정 불가 → skip,
  버전 불일치 target → deferred(버전이 같아지는 다음 라운드에서 자동 정합).
- 즉시 트리거: 스위치 ON 전환, upgrade/start/restart job 성공(agent_api `_report` 훅 —
  롤링 업그레이드 마지막 단계에서 STANDBY 가 같은 버전으로 올라오는 순간 자동 복사).
- 스위퍼는 확정 ACTIVE 변화(절체)를 로그로 기록한다.

**그룹 공통 설정 저장** (`PUT /ha-groups/{gid}/packages/{pkg}/config`, `ha_groups.py
_put_group_pkg_config`, operator):
- body = `{values, target_deployment_id?, queue_update?}` — values 는 유효 scope=service
  키만(그 외 400 `non_service_keys`).
- **스위치 ON**: target 없이 호출 — 전 멤버 overlay 에 merge + 멤버별 update_config job
  (+`sync_txn(op=group_config)`). 버전 혼재면 409(스위치 OFF 후 멤버별 편집 유도).
  target 지정은 400 — ON 상태의 멤버별 저장은 자동 교정이 곧 되돌리므로 배제.
- **스위치 OFF**: `target_deployment_id` 필수 — 그 멤버에만 저장(업그레이드 창에서 새
  버전 멤버의 설정 경로).

**단일 서버 저장 API** (`PUT /deployments/{id}/config` / `.../collection/{name}`):
- 항상 해당 deployment 에만 저장 + job 1건. 구 body 필드(`sync_keys`/`sync_checked`/
  `propagate_to_ha_peers`)는 어떤 값이 와도 무시 — 피어에는 절대 쓰지 않는다.
- `POST /deployments/{id}/sync`(방향성 복사 — 멤버십·버전 가드, service 마스크)는
  자동 교정·그룹 컬렉션 즉시 전파의 내부 엔진으로 유지.
- `GET /deployments/{id}/config` 응답 `ha` block: `{group_id, group_name, mode,
  members[{deployment_id, agent_id, agent_name, package_version}]}` — standalone 이면
  null. 콘솔이 "AS 멤버 = 개별 설정만" 판단에 사용.

**드리프트 감시**: 그룹 탭 [멤버 비교]의 필드 비교, 컬렉션 GET 의 멤버 hash 비교,
`drift_sweeper`(주기 감시·알람)는 유지 — 스위치 ON 이면 표시된 드리프트를 자동 교정이
곧 해소하고, OFF 면 수동 편집의 참고 정보가 된다. `should_propagate` 는 "동일해야
정상인 컬렉션" 판정(드리프트 감시)에만 쓰인다.

**운영 시나리오 — 롤링 업그레이드** (S1=ACTIVE·S2=STANDBY, V1→V2, 스위치 ON 상태):
1. 그룹 탭에서 동기화 스위치 **OFF**
2. S2 를 V2 로 업그레이드 — 설정은 overlay 승계, 새 키는 템플릿 기본값
3. 그룹 탭 [공통 설정]에서 **멤버=S2 선택** 후 새/변경 항목 수정 (S2 개별 값은 S2 서버
   화면에서) → S2 기동 → 절체(S2 가 VIP 획득 — 콘솔 뱃지가 십수 초 내 반영)
4. 스위치 **ON** — S1 은 아직 V1 이라 정합 보류(버전 가드)
5. S1 을 V2 로 업그레이드 → upgrade 성공 훅 + 스위퍼가 버전 일치 확인 →
   **ACTIVE(S2) 설정이 S1 로 자동 복사** — 수동 동기화 없이 완료
