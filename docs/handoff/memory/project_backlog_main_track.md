---
name: 2026-05-12 메인 백로그 (이중화 / 설정 UX / Console / 콘솔 분리 / 안정화)
description: 사용자 지정 5개 큰 작업의 진입 가이드. 각 항목별 작업 범위, 영향 컴포넌트, 결정 필요 사항 정리. 다음 세션은 1번부터 차근차근.
type: project
originSessionId: b25db11b-005b-4c51-b4f6-4ca2e1ded329
---
## 우선순위 (사용자 지정 2026-05-12)

기존 백로그 (B1 상용 환경 검증 / WebRTC / S6-SCN-CERT-ROTATE) 보다 **이 5개가 먼저**. 1번부터 순서대로.

1. **Active/Standby 이중화 + All Active**
2. **각 모듈 설정 UX 개선**
3. **Console 관리 기능 개선 (대시보드, 가입자관리, 통계)**
4. **개발 검증용 / 배포용 콘솔 분리**
5. **전체 안정화**

---

## 1. Active/Standby 이중화 + All Active

### 1.0 진행 현황 요약 (2026-05-13 기준 — 2 라운드 완료)

**완료** (✅):

골격 (1 라운드, 2026-05-12):
- Phase 1.A — `docs/design/ha_design.md` 작성 (399줄, 사용자 확정 결정사항 반영)
- Phase 1.B — keepalived 인프라 (`agent/keepalived/` + `cims.sh ha` 6 subcmd)
- Phase 1.D-1 — Redis register replication 골격 (`csp/RedisStore.*` cold-mode no-op + `csp/CspUser.cpp` hook)
- Phase 1.E — CmpClient consistent hash 골격 (`csp/ConsistentHashRing.h` vnode=128 + AddEndpoint/SelectEndpointForSession)
- Phase 1.F — CSC/CSP/PSP A/S VIP-only cold-spare (`agent/systemd/cims-*.service.tpl` + notify systemctl)
- Phase 1.G — cims_agent heartbeat exponential backoff (5→10→20→max 60s)
- Phase 1.H stub — `verify/lib/items/stage6/scn_failover_{csc,csp,cmp}.py` × 3 (SKIP body)

HaServicesPage Phase 1 (2 라운드, 2026-05-13 오전):
- mock 제거 + 실제 API wiring (1239 → 730 lines)
- 스키마: `cims_agent.interfaces_json` / `service_ip_rows_json`, `ha_groups.vip_bindings_json`
- agent: `collect_interfaces()` heartbeat 보고
- backend: enroll/heartbeat persist, ha_groups vip_bindings read/write
- HaGroupsPage 삭제, ServersPage → "서버 Inspector" 리네임

HaServicesPage Phase 2 (3 라운드, 2026-05-13 오후) — **6 sub-items 전부 완료**:
- 2.1 VIP `[적용]` wiring: `_render_ha_for_agent()` vip_bindings → services.*.vips[],
  keepalived.conf.tpl `${VIP_LIST}` multi-VIP, `POST /ha-groups/{id}/apply` endpoint
- 2.2 ServiceIP `[적용]` wiring: `apply_ip_config` agent job (`ip addr add` secondary),
  `POST /agents/{id}/apply-ip-config` endpoint, agent_job ENUM 확장 (`update_ha`+`apply_ip_config`)
- 2.3 `ha_groups.vip` nullable + config_template `ip_scope`/`ip_slot` 메타 + Inspector link
  (`/deploy/servers?agent=<id>` query param 지원)

Phase 1.E-2 (3 라운드, 2026-05-13 오후) — **완료**:
- `SendRequestAndWait(sessionKey, payload, response)` 신규 overload
- `_ResolveEndpoint(key)` — 캐시 hit → ring select → primary fallback
- `_SendOnEndpoint(ep, ...)` — endpoint 별 sendto/sin_addr
- `ReleaseEndpointForKey(key)` — RemoveSession/RemoveGroup 후 cache cleanup
- 9 callers 업데이트 (Add/Modify/Update/Remove Session, Add/Modify/Join/Leave/Remove Group)

Phase 1.D-2 (3 라운드, 2026-05-13 오후) — **완료** (LIVE 활성은 hiredis 설치 + Redis 셋업 후):
- CMakeLists: `find_library(HIREDIS_LIB)` + `find_path(HIREDIS_INCLUDE_DIR)` → `CIMS_HAS_HIREDIS=1`
- `RedisStore.cpp` 본체: `#ifdef CIMS_HAS_HIREDIS` 로 redisConnectWithTimeout/SET/GET/DEL/SCAN
- Setup: `Redis.{Host,Port,Password}` json 파싱
- CspServer.cpp: `gclsRedisStore.Init(...)` CmpClient 직후 호출

**미완** (⚪) — 환경/사용자 의존:

| 항목 | 범위 | 의존 |
|---|---|---|
| **1.H LIVE** 2-node 환경 + S6-SCN-FAILOVER-* stub 활성 | verify + 환경 | 2-node setup 사용자 셋업 후 |
| **DB 이중화 트랙** (별도) | 인프라 | Galera vs Master-Master 사용자 결정 |
| **VIP fail-over RTO 측정** | 측정 도구 | 1.H LIVE 환경 후 |
| **TLS 단말 재핸드셰이크 latency** | 측정 + TLS 시나리오 | TLS 진입 시 |
| **Redis LIVE 검증** | apt + Redis 셋업 | `apt install libhiredis-dev` + Redis Sentinel 또는 단일 노드 셋업 후 |
| **CMP All-Active LIVE 검증** | 2+ CMP 인스턴스 | csp.json 에 `Cmp.Endpoints` 배열 추가 + 2-node CMP 셋업 |

**1번 트랙 자체 dev 가능 작업은 모두 완료**. 다음 진입:
- 환경 의존 LIVE 검증 (사용자 셋업 후)
- 또는 **2번 트랙 (모듈 설정 UX)** 으로 진행 가능

### 1.1 대상 매트릭스

| 컴포넌트 | 이중화 모드 | 근거 |
|---|---|---|
| CIMS 관리서버 (mgmt-server / CSC 4445) | Active/Standby | 단일 관리 endpoint, DB 일관성 |
| VoLTE SIP Server (CSP) | Active/Standby | SIP 다이얼로그 stick (B2BUA session) |
| PTT SIP Server (PSP) | Active/Standby | PTT-AS 그룹 state + CSCF register |
| VoLTE Media Server (CMP) | **All Active** | RTP relay — 호 단위 분산 |
| PTT Media Server (PMP) | **All Active** | RTP mixing + Floor — 그룹 단위 분산 |
| IBCF SIP (ISP) | (CSP 가 hosting 한다는 전제 → CSP 따라감) | volte-sip-server 공존 |
| IBCF Media (IMP) | (CMP 가 hosting → CMP 따라감) | volte-media-server 공존 |

### 1.2 결정 필요 사항 (진입 전 사용자 confirm)

- **VIP 메커니즘**: keepalived (VRRP) / pacemaker / cloud LB / DNS-based fail-over 중 선택
- **fail-over trigger**: heartbeat 타임아웃 기준 (1s / 5s / 10s)
- **세션 인계**: state replication (실시간) vs cold start (재 REGISTER 요구) — VoLTE/PTT 다른 정책 가능
- **DB 이중화**: MariaDB Galera Cluster / Master-Master / 단일 Master + replica
- **All Active media 분배**: SIP server 가 호별 CMP 인스턴스 선택 알고리즘 (round-robin / least-loaded / consistent hash)
- **mgmt-server fail-over**: CSC API endpoint 의 client (Console + Test-agent) 가 어떻게 다른 active 로 redirect 하나

### 1.3 영향 컴포넌트

- `csp/` — SIP B2BUA state, register table 동기화
- `cmp/` — RTP session 분산 + healthcheck
- `csc/src/` — REST API endpoint + DB 연결 풀
- `cims-console/` — fail-over 인지 (다중 endpoint URL 관리)
- `cims_agent.py` — heartbeat 표적 변경 (active CSC 선택)
- `verify/lib/` — fail-over 시나리오 추가 (S6-FAILOVER-* 신규)

### 1.4 시작 지점

`docs/design/` 아래에 `ha_design.md` 신규 작성 권장 — VIP 흐름도 + DB 토폴로지 + state replication 방식을 user 확정 후 코드 작업.

### 1.5 작업 쪼개기 (예상)

- 1.A: HA 설계 문서 + 사용자 확정
- 1.B: keepalived (또는 선택된 방식) 설치 + VIP 설정 자동화
- 1.C: DB Master-Master 또는 Galera 셋업
- 1.D: CSP state replication (active → standby)
- 1.E: CMP All Active 분배 로직
- 1.F: mgmt-server fail-over + Console URL 갱신
- 1.G: cims_agent heartbeat target 자동 전환
- 1.H: verify 시나리오 추가 (S6-FAILOVER-CSP / -CMP / -CSC)

---

## 2. 각 모듈 설정 UX 개선

### 2.1 현황

- `csp.json` / `cmp.json` / `csc.json` 등은 JSON 직접 편집
- Console 의 ServersPage 가 `config_template.json` 기반 form 제공 중
- deployment overlay (install_path/<pkg>/config.json) 가 cims_agent + cims.sh 가 처리

### 2.2 개선 방향 (사용자 확정 필요)

- **기본값 추천**: 흔한 시나리오 (싱글 노드 / HA / 멀티사이트) 별 preset 제공
- **validation**: schema-based field validation, 의존 키 (예: TLS 활성 시 cert 필수) 체크
- **preview**: 변경 사항 적용 전 영향 받는 service / 재기동 필요 여부 표시
- **hot-reload**: SIGUSR1 자동 발송 / cims.sh restart 1-click
- **diff 검토**: 변경 전/후 JSON diff highlight

### 2.3 영향 컴포넌트

- `csp/config_template.json`, `cmp/config_template.json`, `csc/config/config_template.json` — schema 보강 (validation rules 추가)
- `cims-console/src/pages/ServersPage.tsx` — form UI 강화 + diff viewer
- `csc/src/handlers/build.py` 또는 `service_control.py` — preview/hot-reload endpoint
- `agent/cims_agent.py` — `job_update_config` 정합 (이미 존재)

### 2.4 시작 지점

`csc/config/config_template.json` (가장 큰 schema) 부터 review → ServersPage 와 mapping → 어떤 필드가 어떻게 UI 노출 되는지 정리.

---

## 3. Console 관리 기능 개선

### 3.1 현재 페이지 (25개)

```
대시보드:     DashboardPage
가입자관리:   UsersPage, OrganizationsPage, SubscriptionsPage, VolteMsisdnPage, PttMsisdnPage, MembersPage
그룹:         GroupsPage, PttGroupsPage
이력:         CallLogsPage, VolteHistoryPage, PttHistoryPage, RecordingsPage
통계:         StatsPage, StatsMessagesPage
서비스:       ServicesPage, ServersPage, ServiceStatusPage
검증:         VerificationV2Page, VerificationHistoryPage, FlowPage
배포:         PackagesPage (+ deploy/)
기타:         LoginPage, DocsPage
```

### 3.2 개선 우선순위 (사용자 확정 필요)

- **대시보드** — 실시간 KPI (active calls, registered users, system health gauges, 최근 alert)
- **가입자관리** — bulk import/export, search/filter, 인증 정보 변경 흐름 (auth-id 재발급 등)
- **통계** — period 선택 (일/주/월/년) + 차트 (matplotlib? recharts?) + 분류 (VoLTE vs PTT, 시도/성공/실패)
- **알림/모니터링** — 시스템 이상 상태 표시 (CMP 응답 없음, CSP register 폭주 등)

### 3.3 영향 컴포넌트

- `cims-console/src/pages/*.tsx` — 각 페이지 강화
- `csc/src/handlers/stats.py`, `users.py`, `org.py` 등 — API 보강 (집계 query, bulk endpoint)
- DB 추가 인덱스 / 집계 테이블 (성능)

### 3.4 시작 지점

`DashboardPage.tsx` 현재 컨텐츠 → 사용자 wishlist 확정 → backend API 작업 + UI 구현 동시.

---

## 4. 개발 검증용 / 배포용 콘솔 분리

### 4.1 현황

현재 한 codebase (`cims-console`) 가 mode 분기로 동작:
- `mode=tb` — TB-Console (port 3000, proxy → TB-CSC 4419)
- `mode=dev` — Dev-Console (port 3001, proxy → 배포본 CSC 4445)
- `mode=test` — Test-Console (build → port 8080)
- 배포본 console — mgmt-server/console (port 8081)

모든 mode 가 동일한 메뉴 (Packages 메뉴 포함). 운영 배포본에 패키징 UI 는 부적절.

### 4.2 분리 방안 (사용자 확정 필요)

옵션 A: **mode-based menu filtering** (단일 codebase)
- 환경변수 `VITE_CONSOLE_TARGET=dev|prod` 로 메뉴 항목 분기
- 빌드 1개로 두 용도 지원 가능
- 분기 누락 시 운영본에 패키징 노출 위험

옵션 B: **별도 codebase 분리** (`cims-console-dev` / `cims-console-prod`)
- 공통 컴포넌트는 npm workspace / monorepo
- 명확한 경계 + 운영본 사이즈 감소
- 유지보수 비용 ×2

옵션 C: **route-based** (`/dev/*` 와 `/prod/*` 둘 다 한 build 에 존재, 권한으로 접근 제어)
- 단일 build, 런타임 분기
- 코드는 한 곳, 권한 leak 시 보안 이슈

권장: **A** (간단하고 즉시 적용 가능) — 추후 mass 가 너무 커지면 B 로 split.

### 4.3 영향 컴포넌트

- `cims-console/src/router.tsx` 또는 `App.tsx` — route 분기
- `cims-console/src/components/Sidebar.tsx` 또는 menu config — 메뉴 항목 필터
- `vite.config.ts` — mode 별 env 주입
- 빌드 파이프라인 (`make dist` / `cims.sh pkg`) — prod build flag

### 4.4 시작 지점

`cims-console/src/router.tsx` 또는 `App.tsx` 의 라우팅 구성 → 어떤 페이지가 어느 mode 에 속하는지 매핑 → `VITE_CONSOLE_TARGET` 으로 분기.

---

## 5. 전체 안정화

위 1~4 변경을 모두 반영한 후:
- LIVE pipeline-full 회차 누적 (PASS 유지 확인)
- 새 시나리오 (HA fail-over) 추가
- docs/ 정합화 (1~4 의 새 구조 반영)
- VERIFICATION_PROCESS.md, VERIFICATION_MANUAL.md 갱신

이 단계는 1~4 가 어느 정도 완료된 후 진입.

---

## 작업 진입 절차 (다음 세션)

1. **1번 시작** — 사용자가 "1번 진행" 신호 시:
   - 1.0 진행 현황 미완 표 확인 → 다음 sub-item 선택
   - **추천 순서**: HaServicesPage Phase 2 ("[적용]" wiring) → 1.E-2 (CmpClient endpoint) → 1.D-2 (hiredis) → 1.H LIVE
   - DB 이중화는 사용자 결정 (Galera vs Master-Master) 후 별도 진입
   - 사용자 확정 결정사항은 [[project_ha_design_decisions]] 의 표 참조

2. **2~4번은 1번 결정에 의존도 거의 없음** — 사용자가 우선순위 바꿔도 진입 가능

3. **각 항목 진입 전 메모리 확인** — 이 파일 (project_backlog_main_track.md) + 1번 진행 시 [[project_ha_design_decisions]] + [[project_session_2026_05_13_ha_wiring]] 다시 읽고 시작
