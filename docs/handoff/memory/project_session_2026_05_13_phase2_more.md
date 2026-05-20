---
name: project-session-2026-05-13-phase2-more
description: "2026-05-13 오후 — 백로그 1 Phase 2 / 1.E-2 / 1.D-2 + 백로그 2 ModuleConfigModal UX + 백로그 4 콘솔 분리 + 백로그 5 회차. 7 commits, 5 dev-feasible 작업 모두 종료."
metadata: 
  node_type: memory
  type: project
  originSessionId: d333570e-ab44-4282-ba17-b1ccea1215d2
---

# 2026-05-13 오후 — 백로그 1 Phase 2 + 1.E-2 + 1.D-2 + 백로그 2 + 4 + 5

## 진입 컨텍스트

오전 세션 (project_session_2026_05_13_ha_wiring.md) HaServicesPage Phase 1 wiring 완료. 오후 세션 시작 시 사용자가 백로그 정합화 요청 → §1.0 진행 현황 표 신설 → "순서대로 진행" 신호.

## 처리 결과 (commits + 백로그)

### 백로그 1 — A/S 이중화 (dev 환경 작업 완료)

**Phase 2** — HaServicesPage 의 mock 운영자 액션을 실제 백엔드 wiring (5 sub-items):

1. **2.1 VIP `[적용]`** (`5724dc5`)
   - `_render_ha_for_agent()` 정리 — `vip_bindings` → `services.<group_name>.vips[]` 매핑
   - keepalived 템플릿 `${VIP_LIST}` (한 vrrp_instance 다중 virtual_ipaddress)
   - `ha.sh _build_vip_list()` helper
   - `POST /api/v1/ha-groups/{id}/apply` endpoint — 데이터 변경 없이 update_ha 강제 큐잉

2. **2.2 ServiceIP `[적용]`** (`bfa9b49`)
   - 신규 agent job `apply_ip_config` (`cims_agent.py:job_apply_ip_config`)
   - `ip addr add <ip>/<mask> dev <iface>` (idempotent, "File exists" SKIP)
   - secondary IP 만 (primary 변경 안 함 — mgmt 끊김 방지)
   - `POST /api/v1/agents/{id}/apply-ip-config` endpoint
   - DB migration: `agent_job.job_type` ENUM 에 `update_ha` (Phase 1.B 누락) + `apply_ip_config` 추가

3. **2.3 정리** (`32ce9fb`)
   - `ha_groups.vip` NULL 허용 (sql/migrate_ha_groups_vip_nullable.sql)
   - `config_template` field 에 `ip_scope/ip_slot/ip_port/ip_proto` 메타
   - HaServicesPage `extractIpSlotsFromTemplate(p)` — 패키지 메타 우선, SLOT_MAP fallback
   - ServerInspector link — `/deploy/servers?agent=<id>` query param 지원

4. docs 정합화 (`b6b74f4`) — admin_api.md / ha_design.md §11.7 / VERIFICATION_MANUAL.md

**Phase 1.E-2** — CmpClient session-sticky multi-endpoint (`5d12680`):
- `SendRequestAndWait(sessionKey, payload, response)` 신규 overload
- `_ResolveEndpoint(key)` — 캐시 hit → ring select → primary fallback
- `_SendOnEndpoint(ep, ...)` — endpoint 별 sendto/sin_addr
- `ReleaseEndpointForKey(key)` — RemoveSession/RemoveGroup 후 cache cleanup
- 9 callers 업데이트 (Add/Modify/Update/Remove × Session/Group)
- 단일 endpoint 동작 동일성 보장 — Init 가 primary 1개를 ring 에 자동 등록

**Phase 1.D-2** — hiredis 통합 (`5724dc5` 시점 이후 commit, 정확히 `b00nlb108` background commit 의 결과):
- `find_library(HIREDIS_LIB)` + `find_path(HIREDIS_INCLUDE_DIR)` 감지 시 `CIMS_HAS_HIREDIS=1`
- `RedisStore.cpp` 본체 — `redisConnectWithTimeout(2s) + AUTH + PING` + `SET/GET/DEL/SCAN`
- `SipServerSetup` 에 `Redis.{Host,Port,Password}` 필드
- `CspServer.cpp` 가 `gclsRedisStore.Init(...)` CmpClient Init 직후 호출
- 미설치 환경: cold-mode (build PASS, stub fallback)

**잔여** (환경/사용자 의존):
- 1.H LIVE — 2-node 환경 + S6-SCN-FAILOVER-* stub 활성 + RTO ~3s 측정
- Redis LIVE — `apt install libhiredis-dev` + Redis 셋업
- CMP All-Active LIVE — 2+ CMP 인스턴스
- DB 이중화 — Galera vs Master-Master 사용자 결정

### 백로그 2 — 모듈 설정 UX (dev 완료)

`ModuleConfigModal.tsx` 에 4 건 UX 개선 (`dd0cd2f`, `694a8d5`):
- **ChangeSummaryPanel** — 변경 사항 (#N) 카드, 옛값→새값 테이블, 🔁/⚡ 분류, 펼침/접힘, 전체 초기화, 행별 ↺ 버튼
- **per-field ↺ reset** — 변경된 필드 input 옆 (initial 값 tooltip)
- **"저장 + 재기동" 버튼** — deployment 모드 + restart_required 시 노출, save 직후 `deploymentApi.queueJob('restart')`
- **required + min/max validation** — 저장 전 검증, 빈 필드/범위 초과 시 toast reject

**잔여**: preset (패키지 config_template 컨텐츠 authoring 필요)

### 백로그 4 — 콘솔 분리 (완료)

(`710c6c5`):
- `RouteSection` 에 `prodHidden?: boolean` field
- `release` (패키징) section 에 `prodHidden=true`
- `VITE_CONSOLE_TARGET` env 읽어서 `IS_PROD_CONSOLE` 결정 (default 'dev')
- `VISIBLE_SECTIONS = SECTIONS.filter(s => !IS_PROD_CONSOLE || !s.prodHidden)`
- `FLAT_ROUTES = VISIBLE_SECTIONS.flatMap(s => s.routes)` — prod 에서 /release/* 라우트 차단
- Sidebar: SECTIONS → VISIBLE_SECTIONS
- cims.sh: cmd_build + cmd_pkg console 빌드에 `VITE_CONSOLE_TARGET=prod` 환경변수 주입

검증: prod 빌드 bundle 에 `VITE_CONSOLE_TARGET:\`prod\`` baked in 확인.

### 백로그 3 — Console 관리 (미진입, wishlist 필요)

DashboardPage 는 이미 KPI/health/active calls 보유 (5초 폴링). 추가 wishlist 가 광범위 (실시간 차트 / alert 이력 / 가입자 bulk import / 통계 기간 선택 등) — 사용자 우선순위 확정 후 진입.

### 백로그 5 — 안정화

LIVE pipeline-full 회차:
- backlog 1-4 변경 회기능 무영향 확인 (S1/S2/S3/S4 PASS)
- S5-MODULES-RUN-START 환경 race (volte-sip-server 127.0.0.1:5060 LISTEN 20s timeout) — pre-existing flakiness, memory `feedback_ptt_later` + `bdca6b5` 시점 변동성 잔존

### 추가 fix commits

- `6269f6b` — toggleExpand unused-expressions lint + agents.py local import
- `dd0cd2f` style — clang-format CmpClient
- `694a8d5` style — clang-format RedisStore

## 변경 파일 (이번 라운드)

```
agent/cims_agent.py                       — collect_interfaces + job_apply_ip_config
agent/keepalived/keepalived.conf.tpl      — ${VIP_LIST} multi-VIP
agent/lib/ha.sh                            — _build_vip_list helper + INTERFACE per-service override
cims-console/src/api/deployment.ts        — ServiceIpRow/NetIface + applyIpConfig
cims-console/src/api/ha_groups.ts         — VipBinding + apply
cims-console/src/components/Sidebar.tsx   — VISIBLE_SECTIONS
cims-console/src/components/module/ModuleConfigModal.tsx — 154 lines (UX 4건)
cims-console/src/pages/HaServicesPage.tsx — applyVip/applyServiceIp + ServerInspector link
cims-console/src/pages/ServersPage.tsx    — ?agent=<id> query param
cims-console/src/routes.tsx               — prodHidden + VISIBLE_SECTIONS
cims.sh                                    — VITE_CONSOLE_TARGET=prod
csp/CMakeLists.txt                         — hiredis find + link
csp/CmpClient.{h,cpp}                      — session-sticky multi-endpoint
csp/CspServer.cpp                          — RedisStore.Init
csp/RedisStore.cpp                         — hiredis 본체 (CIMS_HAS_HIREDIS)
csp/SipServerSetup.{h,cpp}                 — Redis 설정 파싱
csc/src/handlers/agents.py                 — apply-ip-config endpoint + _agent_to_json json 정리
csc/src/handlers/ha_groups.py              — vip nullable + apply endpoint + multi-VIP render
docs/api/admin_api.md                      — Phase 2 API 명세
docs/design/ha_design.md                   — §11.7 Phase 2 multi-VIP rendering
docs/VERIFICATION_MANUAL.md                — Console UI 표 (deploy/services 등)
docs/user-manual/deployment_workflow.md    — primary 흐름 변경
sql/migrate_agent_job_types.sql            — NEW (update_ha + apply_ip_config)
sql/migrate_ha_groups_vip_nullable.sql     — NEW
```

## 다음 세션 진입 후보

| 옵션 | 메모 |
|---|---|
| 백로그 3 — Console 관리 wishlist 결정 후 | DashboardPage 강화 (차트 / alert 이력) 등 우선순위 사용자 확정 필요 |
| 백로그 1 LIVE | 2-node 환경, Redis 셋업, libhiredis-dev 설치, CMP 멀티 인스턴스 |
| 백로그 2 preset | config_template 에 presets 컨텐츠 authoring |
| 차후 트랙 | 메인 5개 안정화 충분 가정 시 B1 / WebRTC / CERT-ROTATE |

## 관련 메모리

- [[project_backlog_main_track]] — 5 백로그 가이드 + §1.0 진행 현황
- [[project_session_2026_05_13_ha_wiring]] — 오전 세션 (HaServicesPage Phase 1)
- [[project_ha_design_decisions]] — 1.A~1.H 결정사항 + 영향 파일
- [[project_session_2026_05_12_ha_services_page]] — mock UI 8 라운드 (전 세션)
