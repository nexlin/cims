---
name: project-session-2026-05-13-ha-wiring
description: 2026-05-13 세션 — HaServicesPage mock 제거 + 실제 API wiring 4 task 완료. interfaces_json + service_ip_rows_json + vip_bindings_json 스키마 확장 + agent heartbeat 보고.
metadata: 
  node_type: memory
  type: project
  originSessionId: d333570e-ab44-4282-ba17-b1ccea1215d2
---

# 2026-05-13 세션 — HaServicesPage wiring + 기존 페이지 정리

## 진입 컨텍스트

전 세션 (`fe5d49b`) HaServicesPage UI mock 완료. 이번 세션 mock-up 2건 미세 조정 후 실제 wiring 4 task 일괄 진행.

## 처리 결과

### Task #1 — HaServicesPage 실제 wiring ✅

**스키마 확장** (`sql/migrate_ha_services_wiring.sql`):
- `cims_agent.interfaces_json TEXT` — agent heartbeat 보고 인터페이스 (JSON: `[{name,ip,mask,hint?}]`)
- `cims_agent.service_ip_rows_json TEXT` — 운영자 iface→slot 매핑 (서버 단위)
- `ha_groups.vip_bindings_json TEXT` — VIP slot bindings (그룹 단위, memberIfaces 포함)

**핵심 결정**:
- agent iface 보고 메커니즘: **heartbeat payload** (eventual consistency, 30s 주기) — on-demand API 보다 단순
- slot 영구화 위치: **cims_agent 단위** (HA member 와 무관) — standalone agents 도 동일 모델 적용 가능

**Agent** (`agent/cims_agent.py`):
- `collect_interfaces()` 추가 — `ip -j -4 addr` parse 해서 `[{name,ip,mask}]` 반환
- `collect_host_info()` 결과에 `interfaces` 포함 → enroll 시 보고
- run_loop heartbeat body 에 `interfaces` 포함 → 매 heartbeat 갱신

**Backend handlers**:
- `agent_api.py _enroll/_heartbeat`: `interfaces_json` 컬럼 갱신 (`COALESCE(%s, interfaces_json)` 로 nullable)
- `agents.py _agent_to_json`: `interfaces`, `service_ip_rows` 필드 응답 (JSON parse)
- `agents.py _update_agent`: `service_ip_rows` body field → `service_ip_rows_json` 저장
- `ha_groups.py _list_groups/_get_group`: `vip_bindings` 응답
- `ha_groups.py _create_group/_update_group`: `vip_bindings` body → `vip_bindings_json` 저장

**Frontend** (`ems/core/console/`):
- `api/deployment.ts`: `NetIface`, `ServiceIpRow` type 추가, `Agent.interfaces/service_ip_rows` 필드, `updateAgent` body 에 `service_ip_rows` 옵션
- `api/ha_groups.ts`: `VipBinding` type, `HaGroup.vip_bindings`, `HaGroupInput.vip_bindings`
- `pages/HaServicesPage.tsx`: 전체 rewrite (1239 → 730 lines, mock data 제거)
  - `INITIAL_SERVICES`, `MOCK_PACKAGES`, `DEMO_IFACES*` 완전 제거
  - `haGroupsApi.list() + deploymentApi.listAgents/listPackages/listDeployments` 병렬 load (10s 주기 polling)
  - services 빌드: `haGroups → ServiceRow`, ha_group 미배정 agent → standalone ServiceRow (id = -agent.id)
  - createService: A/S = `createAgent×2 + ha_groups.create({members:2})`, AA = `createAgent×1 + ha_groups.create({members:1})`, standalone = `createAgent×1` 만
  - addServer: AA 만 활성 — `createAgent + addMember(group)`
  - regenerateToken: `deleteAgent + createAgent + addMember` (HA group 인 경우)
  - updateServer(serviceIpRows): `updateAgent({service_ip_rows})`
  - updateService(vipBindings): `haGroupsApi.update({vip_bindings})`
  - updatePackageIds: add → `createDeployment` per member, remove → `deleteDeployment`
  - `pendingTokens` Map<agent_id, {token, cmd}> 로 신규 발급 install command 보관

**Hardcoded SLOT_MAP** (`HaServicesPage.tsx` 내):
- 패키지 이름 → IpSlot[] 매핑 (csc/csp/psp/cmp/pmp/cwrtc/console/phone)
- 추후 task: 패키지 `config_template.json` 에 `ip_scope` / `ip_slot` attribute 도입 → SLOT_MAP 제거

**미완성** (의도적 — v1 wiring 범위 외):
- VipPanel "[적용]" / ServiceIpPanel "[적용]" 버튼 → 실제 keepalived reload / agent ip-config 적용 API 없음 (저장만)
- ha_groups.vip 단일 필드 — `'0.0.0.0'` placeholder 로 둠 (VipBindings 가 멀티 slot 대체)
- 패키지 process_name 커스터마이즈 — `pkg.name.toUpperCase()` 기본값 (advanced 운영자는 ServersPage 사용)

### Task #2 — 기존 페이지 정리 ✅

**결정**:
- `/deploy/services` (HaServicesPage) — primary entry (defaultPath 으로 승격)
- `/deploy/servers` (ServersPage) — "서버 Inspector" 로 리네임. deployment lifecycle (install/start/stop/configure), metrics, advanced 운영용 deep tool 로 유지.
- `/deploy/ha-groups` (HaGroupsPage) — **삭제** (기능 HaServicesPage 로 완전 흡수)

**변경**:
- `routes.tsx` defaultPath: `/deploy/packages` → `/deploy/services`
- `HaGroupsPage.tsx` 파일 삭제

### Task #3 — DeploymentCreateModal 통합 ✅

**결정**: 통합 안 함 (각자 역할 분리).
- ServersPage 의 DeploymentCreateModal: per-agent 세부 설정 (process_name, service_functions[]) 운영자 advanced UX 유지
- HaServicesPage 의 PackagesArea: 단순 bulk picker (체크박스 → 멤버 전체 일괄 deploy). 운영자가 세부 조정 필요할 때만 ServersPage Inspector 진입.

코드 변경 없음.

### Task #4 — AgentCreateModal install UX ✅

**결정**: 현재 HaServicesPage 의 per-server 자동 발급 흐름이 mock 디자인 intent 그대로 충족.
- 그룹 생성 시 멤버 수만큼 `createAgent` 자동 호출 (A/S=2, AA=1, standalone=1)
- 응답의 `enrollment_token` / `install_command` 을 `pendingTokens` map 에 보관
- 각 server row 의 "📋 복사" 버튼이 해당 token 의 install command 를 clipboard 에 복사
- 그룹 단위 통합 "show all commands" modal 은 v2 으로 보류 (필요 시 운영자 피드백 기반 추가)

코드 변경 없음.

## 변경 파일

```
agent/cims_agent.py                       (interfaces 수집 + 보고)
ems/core/console/src/api/deployment.ts        (NetIface, ServiceIpRow types)
ems/core/console/src/api/ha_groups.ts         (VipBinding type)
ems/core/console/src/pages/HaServicesPage.tsx (full rewrite, 730 lines)
ems/core/console/src/pages/HaGroupsPage.tsx   (삭제)
ems/core/console/src/routes.tsx               (defaultPath, HaGroupsPage 제거, 서버 Inspector 리네임)
csc/src/handlers/agent_api.py             (interfaces_json persist)
csc/src/handlers/agents.py                (interfaces/service_ip_rows in JSON + PUT)
csc/src/handlers/ha_groups.py             (vip_bindings_json 저장/응답)
sql/migrate_ha_services_wiring.sql        (NEW — 컬럼 3개 추가)
```

## 검증 상태

- TypeScript: `tsc --noEmit` PASS
- Python: ast.parse PASS
- Migration: 로컬 DB 적용 완료 (`cims_agent` 17 컬럼 → 19, `ha_groups` 10 → 11)
- API smoke: `GET /api/v1/ha-groups` `{"groups":[]}`, `GET /api/v1/agents` 5 agents 응답 with `interfaces:null, service_ip_rows:null`
- CSC + TB-CSC 재기동 완료
- LIVE pipeline-full: **미실행** (HaServicesPage 변경은 회기능에 영향 없음 가정)

## 잔여 / 다음 세션 진입

1. **사용자 브라우저 검증** — `/deploy/services` 에서 5 standalone agents 표시 확인, 새 서비스 생성 (A/S/AA/standalone) 시도, vip_bindings 저장 확인
2. **agent 재시작 후 interfaces 수집 확인** — service 의 server row 펼침 시 인터페이스 list 보여야 함
3. **docs 정합화**:
   - `README.md` / `user-manual/deployment_workflow.md` — `/deploy/services` 새 primary entry, ha-groups 메뉴 제거
   - `api/admin_api.md` — agent / ha-groups 응답 새 필드 (`interfaces`, `service_ip_rows`, `vip_bindings`)
   - `design/modules/csc.md` — HaServicesPage 데이터 모델 매핑
4. **ServerInspector 진입점** — HaServicesPage 에서 server row 더블 클릭 → ServersPage 로 jump (선택)
5. **VipPanel/ServiceIpPanel "[적용]" 실제 wiring** — keepalived reload + agent ip-config API
6. **메인 백로그 1번 이후 진입**: A/S 이중화 LIVE (S6-SCN-FAILOVER-* stub 활성), 1.D-2 hiredis 통합, 1.E-2 CmpClient endpoint 분배

## 운영자 사용 흐름 (wiring 후)

1. `/deploy/services` 진입 — 기존 standalone agents 자동 표시
2. `+ 시스템 추가` → 이름 / 모드 (A/S / AA / Standalone) 선택 → `생성`
3. A/S: 2개 agent + ha_group 자동 생성, install command 2개 발급
4. 운영자가 install command 를 각 서버에서 실행 → agent enroll → 자동 online
5. enroll 후 interfaces 정보가 row 에 채워짐 → "📡 인터페이스 N개" 버튼 활성
6. 펼침 → 각 iface 의 IP / 용도 입력 → 자동 저장 (`updateAgent`)
7. "📡 VIP" 펼침 → 용도 선택 → VIP IP 입력 → 멤버별 iface 자동 매핑 (override 가능) → 자동 저장 (`haGroupsApi.update`)
8. "+ 패키지 추가" → 모드 일치 패키지 체크박스 → 자동으로 모든 멤버에 deployment 생성

## 관련 참조

- 직전 세션 (mock UI): [[project_session_2026_05_12_ha_services_page]]
- HA 설계: [[project_ha_design_decisions]]
- 배포 데이터 모델: [[project_deployment_arch]]
- CSC dist sync 주의: [[feedback_csc_dist_sync]]
