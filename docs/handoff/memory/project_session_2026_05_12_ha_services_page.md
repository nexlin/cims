---
name: 2026-05-12-ha-services-page
description: 서버 + HA 통합 페이지 mock-up. wizard → tree list (서비스 → 서버) → iface-first IP slot + VIP option B 까지 8 라운드 iteration. 10 commits push (acbf474..fe5d49b). 다음 세션 진입 — 사용자 review 후 실제 wiring 또는 추가 iteration.
metadata:
  node_type: memory
  type: project
  originSessionId: 0f0f28b3-7806-4f46-bc85-efea242cdfbd
---

## 결과 (2026-05-12)

`cims-console/src/pages/HaServicesPage.tsx` — 서버 등록 + HA 구성 + 모듈 설치를 한 페이지에서 보는 **mock-up prototype**. 실제 API 호출 없음 (MOCK_NETWORK_IFACES + INITIAL_SERVICES). **10 commits push (origin/main fe5d49b)**.

진입: `http://<console>/deploy/services` ("서버 + HA" 메뉴, deploy 섹션 최상단). 기존 `/deploy/servers`, `/deploy/ha-groups` 는 "(구)" 표시로 유지.

## 사용자 의도 (인용)

> 서버를 만들고 모듈을 설치한다음에 HA 를 구성하는 것 보다 서버를 등록과정에서 HA 가 구성되는게 좋지 않을까? console 에서 서버 & HA 구성 후 모듈을 설치하기 위한 새로운 페이지

> A/S 인 경우 2개 발행, All-active 인 경우 1개씩 계속 발행 (agent 설치 스크립트)

> A/S 로 동작하는 VoLTE SIP Server 를 추가하고 가정하면 리스트에 서버가 1개 생기고 여기서 이름은 'VoLTE SIP Server' 입력 하고, HA 유형을 선택 하면 2개의 서버하 하위 리스트에 2개 생기고 자동으로 이름이 'VoLTE SIP Server-01', 'VoLTE SIP Server-02' 이런식으로 생기게

## Prototype 진화 (8 라운드)

| 라운드 | commit | 설계 변화 |
|---|---|---|
| v0 (wizard) | acbf474 | 4-step wizard (Server → HA → Modules → Review). 사용자 거절 — "설정하기 부적합" |
| v1 (tree list) | 58c3101 | wizard 폐기 → tree list (서비스 노드 + 자식 서버). 인라인 편집 |
| v2 (가로 확대) | e96e607 | 가로 full-width + 서비스 IP 컬럼 추가 (mgmt IP 와 분리) |
| v3 (IP slot 모델) | d2482d2 | IP/VIP 를 slot + iface 선택 모델로. agent 가 iface 보고 |
| v4 (컬럼 통합) | c4899f0 | 서비스 IP + VIP/VRID 한 컬럼으로 통합. iface-first 매핑 + IP up 확인 |
| v5 (iface-row) | 1dcd10a | 인터페이스 선택 X → 모든 iface row 표시. [적용]/[초기화] 버튼 |
| v5.1 | 5bc7dac | 용도 select → text input (자유 입력) |
| v6 (VIP 옵션 B) | a5b47cd | VipPanel option B (멤버별 iface 별도 매핑) + 용도 → iface 자동 resolve |
| v6.1 (style) | fe5d49b | VIP / 서비스 IP panel padding-left 60 정렬 + borderLeft 3px (가벼운 panel) |

## 최종 UX 모델 (v6)

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│ 서비스 (HA 그룹 / 단독)                                                          │
│ ───────────────────────────────────────────────────────────────────────────────  │
│ #1  VoLTE SIP Server          A/S  ─                  ─       [+서버][복사][×]   │
│ │                                                                                │
│ │  ▶ 패키지 설치 (cards: csp / cmp / csc / ... + ha_capability badge)            │
│ │                                                                                │
│ #1.1 VoLTE SIP Server-01      ─    192.168.1.10      [서비스 IP][VIP/VRID]      │
│ #1.2 VoLTE SIP Server-02      ─    192.168.1.11      [서비스 IP][VIP/VRID]      │
└──────────────────────────────────────────────────────────────────────────────────┘
```

### ServiceIpPanel (서버별)

```
┌─[서비스 IP 패널]─────────────────────────────────────────────────┐
│ iface         IP / mask       용도          상태   액션          │
│ eth0          192.168.1.10/24 mgmt           ✓     [적용][초기화] │
│ eth1          10.10.1.10/24   sip-listen     ✓     [적용][초기화] │
│ eth2          (미설정)         (입력)         ─     [적용]         │
└──────────────────────────────────────────────────────────────────┘
```

규칙:
- iface 행은 자동 (운영자 추가/제거 X — agent 가 보고)
- 운영자 편집: IP/mask + 용도 (text input, 자유)
- [적용] = 변경 적용 (mock: 상태 → ✓)
- [초기화] = agent 가 보고한 원래 값으로 되돌리기
- 프로토콜/port 는 모듈 설정 영역이라 panel 에서 제외

### VipPanel (서비스별) — Option B

```
┌─[VIP 패널 — VoLTE SIP Server]──────────────────────────────────────┐
│ # 용도              VIP / mask    상태  VoLTE-SIP-01     -02  액션  │
│ 1 sip-listen ▼     10.10.1.100/24  ─    eth1 ▼ (auto)   eth1 ▼  │
│ 2 (추가 +)                                                        │
└────────────────────────────────────────────────────────────────────┘
```

규칙:
- 용도 select 의 옵션 = 자식 서버들의 ServiceIpRows slot union
- 용도 선택 시 멤버별 iface 자동 매핑 (slot → serverId → iface lookup)
- 멤버 컬럼 dynamic (서버 수만큼)
- A/S 양 노드 의 iface 가 다를 수 있어 멤버별 별도 select 허용 (option B)

## 데이터 모델 (mock)

```typescript
type Service = {
  id: number
  name: string                      // "VoLTE SIP Server"
  haMode: 'standalone' | 'active_standby' | 'all_active'
  servers: ServerNode[]
  vipBindings: VipBinding[]
  packages: number[]
}

type ServerNode = {
  id: number
  name: string                      // "VoLTE SIP Server-01"
  mgmtIp: string                    // immutable, agent enroll
  serviceIpRows: ServiceIpRow[]
}

type ServiceIpRow = {
  iface: string                     // key, immutable (agent-reported)
  ip: string
  mask: number
  slot: string                      // 운영자 자유 입력 (sip-listen, rtp-pool, ...)
  status?: 'pending' | 'ok' | 'error'
}

type VipBinding = {
  bid: number
  slot: string                       // select; options = serviceIpRows slot union
  ip: string
  mask?: number
  status?: 'pending' | 'ok' | 'error'
  memberIfaces: { [serverId: number]: string }  // auto-mapped from slot
}
```

자동 매핑 로직:
```typescript
const slotMap = new Map<string, Map<number, string>>()
for (const srv of servers) {
  for (const r of srv.serviceIpRows) {
    if (!r.slot) continue
    if (!slotMap.has(r.slot)) slotMap.set(r.slot, new Map())
    slotMap.get(r.slot)!.set(srv.id, r.iface)
  }
}
// 용도 변경 시: slotMap.get(slot) 으로 memberIfaces 자동 채움
```

## 다음 세션 진입 — 결정 보류 안건

본 라운드는 **mock data prototype**. 사용자 review 후 결정 필요:

| 결정 안건 | 옵션 |
|---|---|
| **wiring** | (a) MOCK_* 제거 + 실제 API (`/api/v1/agents`, `/api/v1/ha-groups`, `/api/v1/packages`) (b) 추가 iteration |
| **기존 페이지 정리** | (a) /deploy/servers + /deploy/ha-groups 폐기 (b) sub-view 로 유지 |
| **DeploymentCreateModal 통합** | (a) HaServicesPage 의 "패키지 설치" 영역으로 흡수 (b) 별도 modal 유지 |
| **agent 보고 iface** | (a) cims_agent heartbeat 에 `interfaces[]` payload 추가 (b) on-demand query API |
| **slot 영구화** | (a) DB ha_group_members 에 `iface_slots` JSON (b) agent install_path 의 config 에 저장 |
| **install 스크립트 발행** | A/S = 2개, AA = 1개씩 추가 발행. 현재 ServersPage AgentCreateModal 이 1개 → 단일 그룹 단위 발행 UX 추가 |

## 핵심 파일

### 신규
- `cims-console/src/pages/HaServicesPage.tsx` — ~1100줄, 단일 페이지에 모든 컴포넌트 (서비스 row + ServiceIpPanel + VipPanel + 패키지 grid)

### 수정
- `cims-console/src/routes.tsx` — `/deploy/services` route 추가 (최상단, "서버 + HA"). 기존 routes 는 "(구)" 라벨

## Mock data 가정 (실제 wiring 시 교체 대상)

- `MOCK_NETWORK_IFACES` — 서버별 iface list (현재는 모든 서버 공통 eth0/eth1/eth2)
- `INITIAL_SERVICES` — 2 demo 서비스 (VoLTE SIP A/S 2 노드, VoLTE Media AA 3 노드)
- `MOCK_PACKAGES` — 8 모듈 (ha_capability + version)
- VRID 자동 할당 — prototype 에서는 mock 51 고정. 실제 wiring 시 CSC `_allocate_vrid()` 호출

## 검증

- typecheck PASS (`npx tsc --noEmit -p cims-console`)
- LIVE pipeline-full **38/PASS 34/FAIL 0/SKIP 4** — 회기능 무영향 (Console only, backend 무변경)
- 브라우저 click-through: tree expand, IP/용도 편집, [적용]/[초기화], VIP 용도 선택 시 멤버 iface 자동 채움 확인

## 디버깅 메모

- TB-CSC 가 옛 코드 (5월 08일 pid) 실행 중 → `/api/v1/ha-groups` 404. `cims-svc restart tb-csc` 로 해결 (route 등록은 1af137d 의 csc_app.py 변경)
- `effectiveHaMode` / `renderSummary` 의 ternary syntax — `s.ha.kind === 'join'` 조건에서 `s.ha` narrowing 위해 `if` 블록 + return 으로 분리
- 패널 padding-left: ServiceIpPanel `paddingLeft: 60` (이름 컬럼 시작과 일치). VipPanel 동일. 가벼운 panel = borderLeft 3px `#b8d4f5` + background `#fafcfe`

## 관련 메모리

- [[2026-05-12-ha-phase-1-a-1-b]] — 1.A~1.H 결정사항 (VIP keepalived, hybrid state, consistent hash)
- [[2026-05-12-ha-console-ui]] — HaGroupsPage / ha_groups CRUD / job_update_ha 자동 분배 (현 HaServicesPage 의 backend 기반)
- [[2026-05-12-cims-sh]] — 운영 도구 분리 (agent/bin/cims-svc) — HaServicesPage 가 호출할 install script 의 진입점
