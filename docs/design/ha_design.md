# HA 이중화 설계 — Active/Standby + All Active

> CIMS 운영 환경의 가용성을 위한 이중화 토폴로지 설계. SIP 서버군 (CSP/PSP)
> 과 관리 서버 (CSC) 는 Active/Standby, Media 서버군 (CMP/PMP) 은 All Active.
>
> 운영 모델(무장 판정·절체 조건·모듈 운영 명세·일괄 제어·수동 절체)의 정본은
> [features/ha_service_model.md](features/ha_service_model.md) 이다. §11 은 keepalived
> 인프라·렌더의 현행 동작을 요약하며, 무장/절체 판정의 상세는 그 문서를 따른다.

## 1. 개요

### 1.1 목표

| 지표 | 목표 |
|---|---|
| Mgmt API (CSC) fail-over 시간 | ≤ 5초 (VIP 이전 + Console / cims_agent 재연결 포함) |
| SIP 등록 인계 | active 장애 후 standby 가 신규 REGISTER 수락까지 ≤ 5초 |
| 신규 통화 (call) | fail-over 직후 신규 INVITE 가 5초 내 동작 |
| 진행중 통화 (in-call) | fail-over 시 끊김 허용 (cold dialog 정책) |
| Media (CMP/PMP) | 인스턴스 N 개 중 1 개 장애 시 신규 세션 분산 자동 유지 |

### 1.2 비목표

- **DB 이중화** — MariaDB Galera / Master-Master 등은 별도 트랙. 본 문서
  범위 밖. 이번 단계에서는 단일 DB 가정을 유지하며, DB 장애는 별도
  운영 절차에 위임.
- **지리적 이중화 (multi-site)** — 본 설계는 단일 사이트 (동일 L2 도메인)
  내 노드 2개 전제. WAN fail-over 는 후속 라운드.
- **WebRTC bridge (cwrtc)** — 별도 검증 항목 (사용자 요청 예정) 으로
  본 범위 외.

## 2. 핵심 결정사항

| 항목 | 결정 | 근거 |
|---|---|---|
| VIP 메커니즘 | **keepalived (VRRP)** | Linux 표준, advert 1s + dead 3s 로 ~3초 fail-over, on-prem 친화 |
| A/S standby 모듈 정책 | **기본 cold-spare** (standby 정지, 승격 시 notify 가 기동) + **모듈별 hot 선택** (콘솔 `module_modes`) | cold: split-brain 원천 차단 + 승격 시점에 최신 설정으로 기동 (재기동 필요 설정의 실시간 반영 문제 없음). hot: 절체 기동 지연 제거 — 단 세션/상태 인계가 없어 (register 인계 Redis 미구현) 단말 재등록 의존, 이중 기동에 안전한 모듈만 |
| CSP/PSP state 인계 | **하이브리드** (register=hot, dialog=cold) — register 인계(Redis)는 미구현, 현재는 절체 후 단말 재등록 의존 | UE 재등록 부담 ↓ 목표, 진행중 통화는 끊김 허용 — 복잡도 / 무손실 트레이드오프 |
| CMP/PMP 분배 | **Consistent hash on Session-ID** | 동일 세션 stickiness 보장 + healthcheck 로 unhealthy 노드 ring 제외 |
| DB 이중화 | **별도 트랙** | 본 문서 범위 외 |

## 3. 대상 컴포넌트 매트릭스

| # | 컴포넌트 | 모드 | 포트 | 같은 인스턴스에 hosting |
|---|---|---|---|---|
| 1 | CSC (mgmt-server) | Active/Standby | 4421 admin + 4430 mcptt | (없음) |
| 2 | VoLTE SIP (CSP) | Active/Standby | 5060/UDP, 25061/TCP, 5061/TLS | ISP (IBCF SIP) 공존 |
| 3 | PTT SIP (PSP) | Active/Standby | 5060/UDP | (단일) |
| 4 | VoLTE Media (CMP) | **All Active** | 9000/UDP (control), 50000~ RTP | IMP (IBCF Media) 공존 |
| 5 | PTT Media (PMP) | **All Active** | 9000/UDP (control), 52000~ RTP, 54000~ Floor | (단일) |
| 6 | IBCF SIP (ISP) | (CSP 따라감) | 127.0.0.5:5060 | volte-sip-server 변종 공존 |
| 7 | IBCF Media (IMP) | (CMP 따라감) | 127.0.0.5:9000 | volte-media-server 변종 공존 |

> **CSC 보조 서비스 (TB-CSC 4419, TB-Console 3000)** 는 검증/테스트 용도이며
> 운영 가용성 대상 외. 단일 인스턴스 유지.

## 4. 토폴로지 다이어그램

### 4.1 정상 상태 (Node A active, Node B standby)

```
                            ┌─────────────────┐
                  Console   │  VIP_csc        │   cims_agent
                  ──────────┼─→  10.0.0.100  ←┼──────────
                            └─────────────────┘
                                     │
                       ┌─────────────┴─────────────┐
                       ▼                           ▼
              ┌────────────────┐         ┌────────────────┐
              │   Node A       │         │   Node B       │
              │   10.0.0.11    │         │   10.0.0.12    │
              ├────────────────┤         ├────────────────┤
              │ CSC  ✓ active  │  VRRP   │ CSC  ⏸ standby │
              │ CSP  ✓ active  │ ──────→ │ CSP  ⏸ standby │
              │ PSP  ✓ active  │         │ PSP  ⏸ standby │
              │ CMP  ✓ active  │         │ CMP  ✓ active  │  ← All Active
              │ PMP  ✓ active  │         │ PMP  ✓ active  │  ← All Active
              └────────────────┘         └────────────────┘
                       │                           │
                       └─────┬───────────┬─────────┘
                             ▼           ▼
                         ┌───────────────────┐
                         │  Redis (register) │  ← state replication
                         │  10.0.0.20        │     (sentinel 후속)
                         └───────────────────┘
                                   │
                         ┌───────────────────┐
                         │  MariaDB (단일)    │  ← DB HA 별도 트랙
                         │  10.0.0.30        │
                         └───────────────────┘
```

- **VIP_csc / VIP_csp / VIP_psp** 가 각자 VRRP 그룹으로 별도 운용
  (서비스별 fail-over 독립). Media 는 VIP 없이 양쪽 모두 healthy 면 분산.
- **⏸ standby = cold(기본): 프로세스 정지** — MASTER 승격 시 keepalived notify 가
  기동 (§11.4). 콘솔 절체 조건의 `module_modes` 로 모듈별 **hot**(standby 도 상시
  기동, VIP-only 절체) 선택 가능.
- Redis 는 active 가 write, standby 가 시작 시 + lazy read.
- DB 는 양쪽 active 가 모두 접근 (단일 instance, HA 별도).

### 4.2 Active 장애 시

```
              ┌────────────────┐         ┌────────────────┐
              │   Node A   ✗   │         │   Node B       │
              │   (장애)        │         │   10.0.0.12    │
              ├────────────────┤         ├────────────────┤
              │ CSC  ✗         │  VRRP   │ CSC  ✓ ACTIVE  │  ← 승격
              │ CSP  ✗         │ ←────── │ CSP  ✓ ACTIVE  │  ← VIP 인수
              │ PSP  ✗         │         │ PSP  ✓ ACTIVE  │  ← VIP 인수
              │ CMP  ✗         │         │ CMP  ✓ active  │  ← 유지
              │ PMP  ✗         │         │ PMP  ✓ active  │  ← 유지
              └────────────────┘         └────────────────┘
```

- CSP/PSP standby → active 승격 시 Redis 로부터 register state 복원
- CMP/PMP All Active 분배 — CSP 의 hash ring 에서 unhealthy CMP-A 제외
  → 신규 세션은 CMP-B 로만 분배 (기존 세션은 RTP 진행중이라 새로
  올라온 INVITE 부터 적용)

## 5. fail-over 시나리오

### 5.1 CSC active 장애

1. keepalived 가 advert miss (1s × 3) → 3초 후 VRRP 상태 전이
2. Node B 가 `VIP_csc` 인수 → notify 가 cold 모듈(csc)을 `cims-svc start` 로 기동
   (hot 선택 시 이미 기동 상태 — binding 만 인수). §11.4 참조
3. `Console` 의 다음 API 호출 → 새 ARP 로 Node B 로 라우팅
4. `cims_agent` 의 heartbeat (기본 2s 주기, 실패 시 5→10→…→60s 지수 backoff)
   가 다음 회차에서 자동 재연결 (`session_token` 은 동일 DB share 라 양쪽 유효)
5. **체감 영향**: Console 의 in-flight HTTP 1개 정도 실패 (재시도로 복구),
   cims_agent 는 절체 창 동안 실패한 회차의 backoff (수 초 ~ 최대 60초) 만큼
   unsynced. cold 모듈 기동 수초 추가.

### 5.2 CSP active 장애 (VoLTE/IBCF)

1. keepalived advert miss → 3초 fail-over
2. Node B 가 VIP_csp 인수 → notify 가 cold 모듈(csp)을 기동 (hot 선택 시 이미
   기동 상태 — binding 만 인수) → 단말 다음 REGISTER 가 Node B 로 도착
3. register 인계 (Redis lookup → `CspUserMap` lazy restore) 는 미구현 —
   단말 재등록(주기 REGISTER)으로 복원
4. 진행중 dialog (B2BUA in-call) 는 cold 정책으로 손실 → 단말이 BYE 또는
   재 INVITE 로 복구
5. **체감 영향**: 진행중 통화 끊김, 신규 통화는 재등록 후 가능

### 5.3 PSP active 장애 (PTT)

5.2 와 동일 흐름. 추가로 그룹 통화 mid-session 은 끊김 — 단말 재 INVITE
또는 그룹 재진입 필요.

### 5.4 CMP 일부 노드 장애 (All Active)

1. CSP 가 다음 RTP 세션 할당 시 `addSession` (UDP JSON) 을 hash ring 의
   첫 선택 노드 (CMP-A) 로 송신
2. 5초 timeout 발생 → CSP 가 CMP-A 를 unhealthy 마크 (`unhealthy_until =
   now + 30s`)
3. 같은 Session-ID 의 재시도가 hash ring 의 다음 healthy 노드 (CMP-B) 로
   재할당
4. 이후 30초간 신규 세션은 CMP-B 만 사용 (ring 에서 임시 제외)
5. 30초 후 heartbeat 1회 success 시 healthy 복귀
6. **체감 영향**: 첫 세션 1개 RTT (5초) 지연, 이후 자동 복원

### 5.5 PMP 일부 노드 장애

5.4 와 동일 흐름. 그룹 세션의 경우 active 통화 시 floor 호환 RTP 가 mid-
session 손실되나, 그룹 재진입으로 복구.

## 6. State replication 설계 (Redis)

### 6.1 적용 범위

- **hot** — REGISTER 상태 (`CspUserMap` 의 binding sub-table)
- **cold** (Redis 불사용) — B2BUA dialog (`CCallMap`), SUBSCRIBE (`CSubscriptionManager`),
  PTT group call session (`CGroupCallService` 의 `m_mapCallSession` 등)

### 6.2 Redis key 스키마

```
cims:reg:<aor>                      # SET, TTL = Expires header (예: 3600s)
  └─ JSON {
       contact: "sip:1001@1.2.3.4:5060;transport=udp",
       expires_at: 1715500000,
       auth_nonce: "0a1b2c3d...",
       call_id: "register-call-id",
       cseq: 42,
       updated_by: "node-a"
     }

cims:reg:idx:domain:<domain>        # SADD <aor>, 도메인별 인덱스 (옵션, 통계용)
```

- **aor** = `sip:<user>@<domain>` (대소문자 normalize)
- TTL 은 REGISTER 의 `Expires` 헤더값. 단말이 갱신 안 하면 자동 만료.

### 6.3 write 흐름 (CSP active)

1. 단말 REGISTER 도착 → MD5 Digest 인증 성공 → `CspUserMap::SetBinding`
2. **추가**: Redis `SET cims:reg:<aor> <json> EX <expires>` (best-effort,
   실패 시 warn 로그)
3. SIP 200 OK 응답

### 6.4 read 흐름 (CSP standby → active 승격 후)

1. **부팅 시 일괄 복원**: `SCAN cims:reg:*` → 각 key 값을 `CspUserMap`
   에 in-memory 로 load (RTT 부담 회피 위해 pipeline 사용)
2. **lazy lookup**: 단말 INVITE 도착 시 routing target 의 binding 이
   캐시 미스라면 Redis `GET cims:reg:<aor>` → on-the-fly load

### 6.5 Redis 장애 정책

- Redis connection 실패 → CSP 는 cold 로 강등 작동 (in-memory 캐시만 사용)
- Redis 복구 후 자동 재연결, 그 사이의 신규 REGISTER 는 in-memory 만 반영
  → 다음 fail-over 시 손실. 운영 알람으로 표면화.

### 6.6 Redis 인스턴스 운영

- 단일 Redis 인스턴스, 별도 노드 (10.0.0.20) 또는 mgmt 노드 (10.0.0.11) 에 함께 기동
- (후속) Redis Sentinel 또는 Cluster — register state 손실 허용범위가 작아 별도 검토 후 결정

## 7. Consistent hash 분배 알고리즘 (CMP/PMP)

### 7.1 의사코드

```
class CmpRing:
    def __init__(self, vnodes=128):
        self.vnodes = vnodes
        self.ring = SortedDict()      # hash → cmp_id
        self.unhealthy = {}            # cmp_id → expiry_ts

    def add_cmp(self, cmp_id, host, port):
        for i in range(self.vnodes):
            h = sha1(f"{cmp_id}#{i}").digest_as_uint32()
            self.ring[h] = cmp_id

    def select(self, session_id):
        h = sha1(session_id).digest_as_uint32()
        for cmp_id in self.ring.iter_from(h):
            if not self.is_unhealthy(cmp_id):
                return cmp_id
        return None  # 모든 CMP unhealthy → 에러

    def mark_unhealthy(self, cmp_id):
        self.unhealthy[cmp_id] = now() + 30   # 30초 ring 제외

    def is_unhealthy(self, cmp_id):
        exp = self.unhealthy.get(cmp_id)
        return exp and now() < exp
```

### 7.2 healthcheck 정책

구현된 동작 (`CmpClient.cpp KeepAliveLoop`):

- **연결 단위 heartbeat** — CSP 가 3초 주기로 각 CMP 에 `Alive` (JSON-over-UDP)
  송신. **연속 3회 실패 (≈9초)** 에서만 Disconnected 판정 — 일시적 UDP 타임아웃
  1회로 끊으면 그룹 재수립 + 재INVITE 로 진행 중 PTT 콜이 끊기므로 (부하 시
  간헐 타임아웃 흡수). 복구는 다음 Alive 성공 즉시 (`OnCmpStatusChanged`).
- **동기 요청 타임아웃** — `SendRequestAndWait` 는 100ms × 3회 재시도
  (총 ≈300ms) 후 실패 반환.

미구현 (향후 과제): ring 단위 healthcheck — `ConsistentHashRing` 은
`MarkUnhealthy(node, 30s)`/`unhealthy_until` 스킵·재진입을 지원하지만 CSP 에
호출부가 없다. `addSession` 실패 시 unhealthy 마크 + 다음 ring entry 재시도는
설계만 존재.

### 7.3 가상노드 수 (vnode=128) 선택 근거

- 노드 2개 환경에서 통계적 균형 (편차 < 5%) 보장에 충분
- 노드 추가/제거 시 영향 받는 hash 영역 = 1/N (N=총 vnode 수)
- 128 × cmp 수 = 메모리 무시 가능 (수 KB)

## 8. mgmt-server fail-over UX

VIP 단일 endpoint 라 client 코드 변경 최소:

- **Console** — `ems/core/console/vite.config.ts` 의 `VITE_ADMIN_TARGET` 을
  `VIP_csc` 로 변경. proxy 가 자동으로 새 ARP 따라감.
- **cims_agent** — `--oam-url https://VIP_csc:4419` 으로 기동. heartbeat
  실패 시 backoff + 재시도.
- **DB session_token** — CSC 양쪽이 동일 DB 를 share 하므로 token 검증
  통과. 추가 작업 없음.

standby CSC 가 write API 를 허용하면 split-brain 위험이 있어 **cold-spare**
(A/S 공통 기본값 — §2) 를 적용한다:

- Standby 노드의 csc 프로세스는 정지 상태. keepalived `notify`(cims-notify)가
  MASTER 전이 시 `cims-svc start csc`, BACKUP/FAULT 시 `cims-svc stop csc` 수행
  (ha.json `services.<svc>.cold_modules` 기반 — §11.4). split-brain 원천 차단.
  단점은 fail-over 시 약 1~3초 추가 기동 지연 (start 의 sleep + DB 연결).
- RTO 단축이 필요하면 콘솔 절체 조건의 `module_modes` 에서 csc 를 hot 으로 —
  단, 이중 기동(양쪽 write) 안전성 검토가 선행돼야 한다.

구현: `agent/bin/cims-notify` + `oam ha_groups._render_ha_for_agent`(cold_modules
렌더) + agent `cims-ha config|apply`.

## 9. verify 시나리오 매핑

| 항목 ID | 시나리오 | 검증 절차 |
|---|---|---|
| S6-FAILOVER-CSC | CSC active 강제 종료 → Console 재연결 | active CSC kill → 5초 대기 → Console API 호출 200 확인 |
| S6-FAILOVER-CSP | CSP active 강제 종료 → 단말 REGISTER 복원 | active CSP kill → cspsim REGISTER → Redis lookup hit 확인 |
| S6-FAILOVER-CMP | CMP-A 강제 종료 → 신규 세션 CMP-B 분산 | active CMP-A kill → cspsim 신규 통화 5개 → 모두 CMP-B 도착 확인 |

## 10. 구성 요소별 구현 위치

| 구성 요소 | 주요 영향 파일 |
|---|---|
| keepalived 인프라 자동화 | `agent/keepalived/`, `cims.sh ha` |
| Redis register replication 골격 | `csp/RedisStore.{h,cpp}` (cold-mode no-op) + `csp/CspUser.cpp` SetBinding/DelBinding hook. hiredis 통합은 후속. |
| CMP consistent hash 분배 | `csp/ConsistentHashRing.h` + `csp/CmpClient.{h,cpp}` endpoint vector + AddEndpoint + SelectEndpointForSession |
| A/S cold-spare + 모듈별 hot 선택 | `oam ha_groups.py`(module_specs 정규화 + cold_modules/relevant_modules 렌더), `agent/bin/cims-notify`(role writer — role 파일 기록), `agent/bin/cims-health`(verdict reader — Supervisor verdict 판독 + 비상 밸브), `agent/cims_agent.py`(Recovery Supervisor: Health+Evaluator+reconcile), 콘솔 `ServersPage`(Cold/Hot 토글·유지보수 토글) |
| cims_agent VIP target + backoff | `agent/cims_agent.py:run_loop` exponential backoff (5s→10s→20s→max 60s, 성공 시 reset). VIP target 은 `--csc-url` 인자로 변경. |
| verify 시나리오 | `verify/lib/items/stage6/scn_failover_{csc,csp,cmp}.py` 3개 (ha.json + multi-CMP 감지 시 LIVE 활성 분기). |

## 11. 운영 가이드 — keepalived 인프라

### 11.1 파일 구조

운영 시 cims.sh 는 사용 안 함 — agent 패키지가 자체 운영 도구를 들고감.

```
agent/
├── bin/                                # 운영 진입점 (운영자/cims_agent 가 호출)
│   ├── cims-svc       start|stop|restart|status|log <svc>
│   ├── cims-ha        install|config|check|apply|start|stop|status
│   ├── cims-health    <svc>                      # keepalived vrrp_script 가 호출
│   └── cims-notify    <svc> <TYPE> <NAME> <STATE> <PRIO>   # keepalived notify
├── lib/                                # source-only library (caller 가 setup 후 source)
│   ├── lifecycle.sh                    # service lifecycle 함수 (~400줄)
│   └── ha.sh                           # cmd_ha 본체 + B 통합 render
├── keepalived/
│   ├── ha.json.example                 # 노드별 HA config 예시 (commit 됨)
│   └── keepalived.conf.tpl             # 단일 generic template (services 반복 렌더)
│   # 실제 ha.json 과 out/ 은 번들 밖 <prefix>/run/keepalived/ (update_ha job 이 기록,
│   # agent 버전 트리와 분리). apply 시 cims-health/cims-notify + ha.json 이
│   # /etc/keepalived/{bin/,} 에 root:root 로 스테이징된다.
├── systemd/
│   └── cims@.service.tpl               # systemd instantiated unit (%i = svc slug)
├── cims_agent.py                       # heartbeat daemon
├── install-agent.sh
└── pkg.json
```

신규 서비스 (예: cwrtc HA) 추가 = `ha.json.services` 에 항목 1개 추가만으로 완료.

### 11.2 노드 셋업 순서

```bash
# 노드별 1회 (Node A / Node B 각자):
agent/bin/cims-ha install         # keepalived 패키지 설치 (sudo)
cp agent/keepalived/ha.json.example agent/keepalived/ha.json
# → ha.json 의 node_name / local_ip / peer_ip / initial_state 수정
#   + cims_home (/opt/cims) / cims_user (cims) 노드 환경에 맞춰 수정
#   + services.<svc>.{vrid,vip,priority,port} 노드별 분리

agent/bin/cims-ha config          # ha.json + 단일 tpl → out/keepalived.conf + cims@.service
agent/bin/cims-ha check           # keepalived -t syntax 검증
agent/bin/cims-ha apply           # /etc/keepalived/{bin/,} 스테이징 + keepalived 재기동
                                  # (cims@ instance enable 하지 않음 — §11.4)

agent/bin/cims-ha status          # 동작 확인
```

절체 시 모듈 기동/정지는 keepalived notify(`cims-notify`)가 ha.json
`services.<svc>.cold_modules` 를 보고 `cims-svc` 로 직접 수행한다 (§11.4) —
systemd `cims@` instance 는 enable 하지 않는다.

> **VIP 바인딩 / NIC 매핑 (multi-VIP)** — HA 그룹은 `vip_bindings: [{slot, ip, mask}]` 로
> 망별 다중 VIP 를 한 vrrp_instance 에 둔다. 각 VIP 가 붙을 NIC(`dev`)은
> **VIP 바인딩의 slot 과 동일 용도(slot) 를 가진 멤버 `service_ip_rows` 의 iface** 로 결정
> (`oam ha_groups._render_ha_for_agent`; memberIfaces 명시 시 우선).
> vrrp advert NIC 은 mgmt NIC 자동 선택. **VIP 는 서비스망(예 121.161.164.x/24)에만 둔다 —
> 내부/관리망(10.0.x) VIP 는 불필요**. cims-priv 관리 IP/마운트 영속성은 `modules/agent.md §11` 참조.

### 11.3 health probe 정책

- `agent/bin/cims-health <svc>` — **verdict reader** (단일 모델). rc=0 / rc=1. 자체
  판정(생존/포트/카운터 검사)을 하지 않고, agent 의 Recovery Supervisor 가 계산해
  `${cims_home}/run/ha/verdict/<svc>.json` 에 쓴 `vrrp_eligible` 만 판독한다 — 장애
  감지·로컬 복구·좀비 판정·승격 자격은 전부 Supervisor 소관(정본: ha_service_model.md
  §6~§8). track_script 는 남의 상태를 넘겨짚지 않는다.
  - **stale/무효 처리**: verdict 없음·`boot_id` 불일치·만료(TTL 6s, role=MASTER 만 짧은
    grace) → rc=1(fail-safe: verdict 없음 = 자격 없음). role 은 role 파일에서 읽고, 못
    읽으면 BACKUP 과 동일한 무유예 fail-safe.
  - **비상 밸브**: `run/ha/disabled` 마커(`CIMS_HA_DISABLE`)가 있으면 verdict 를 보지 않고
    무조건 rc=0(PASS) — HA 판정을 얼려 현 VIP 위치를 고정한다(노드 사망=VRRP advert
    소실만 절체). legacy 포트검사로의 폴백은 없다.
- **port/proto 유도**: OAM 렌더(`ha_groups._infer_health_port_proto`)가 그룹 멤버 배포의
  대표 daemon 모듈로 결정. csc 는 실효 admin 포트를 게이트웨이 self-register 와 동일한
  단일 해석(`handlers.agents.effective_server_port`: materialize `Server.Port` →
  pkg `gateway.default_port`)으로 유도 — 콘솔에서 포트를 바꾸면 배포 설정 저장 경로가
  게이트웨이 라우트 재등록 + `update_ha` 재렌더를 함께 큐잉해 자동 추종.
  그 외 모듈은 service descriptor 기본값. 그룹 `failover_options.health.{port,proto}`
  수동 오버라이드가 최우선.
- **무장 게이트 = 서비스 의도(선언적)**: HA 는 **운영자가 의도적으로 running 으로 둔
  모듈만** 관리한다 — record status 유추가 아니라 그룹 record 의 `service_intent`
  (`{module: running|stopped}`) 명시값이 정본이다. 렌더는 `service_intent[m]==running`
  인 모듈만 헬스포트 유도·`cold_modules`·`relevant_modules` 에 반영한다. running 의도
  모듈이 없는 멤버는 entry 가 `enabled:false` 로 렌더되어 vrrp_instance 가 생성되지
  않는다(미개시 — keepalived 정지 유지). 재설치·runtime store 유실·예외로 record 가
  어떻게 되든 의도가 running 이면 무장 유지 → 장애 시 승격이 cold 모듈을 재기동
  (자가 회복). **VIP 적용 시점은 자유** — VIP 와 의도는 독립 축이라 설치/기동 순서와
  무관하게 저장·적용 가능(구 `no_started_modules` 409 게이트 폐지). 의도 전이:
  **일괄 시작/서버별 start → running 승격**, **일괄 중지 → stopped**, **서버별 stop 은
  노드 오버라이드(그룹 의도 불변)라 disarm 하지 않는다.** 구 record 는 최초 로드 시
  record-running 으로 service_intent 를 1회 시드(마이그레이션)한다. 렌더 재전파 트리거
  = 그룹 변이 / 배포 생성·제거 / 실효 upstream 변경 / **start·restart·upgrade job 완료
  (→ 의도 running 승격)·stop·uninstall job 완료**. 선언적 상태 모델·절체 판정 체계의
  정본은 [features/ha_service_model.md](features/ha_service_model.md).
- **절체 판정 (로컬 복구 우선, Supervisor 소유)**: 운영자 조작은 장애가 아니다. 서버별
  stop 은 노드 로컬 `run/ha/desired.json` 에 stopped 오버라이드로 기록되어 Supervisor
  가 절체 사유에서 제외(active 노드여도 절체 안 함)하고 재기동도 하지 않는다. 모듈 crash
  는 Supervisor reconcile 이 먼저 재기동하고, `failover_options.restart_limit`(기본 3회/
  300초)를 초과(exhausted)하거나 좀비(프로세스 생존+readiness 실패+op_grace 경과)면
  Evaluator 가 `eligible=false` + 절체 래치 → track_script(cims-health)가 그 verdict 를
  읽어 FAULT → VIP 이양(로컬 복구 소진 후에만). 절체당한(래치) 노드는 reconcile 이
  hot·cold 모듈을 전부 정지(kill)하고, 운영자 start/restart 로 래치가 풀려야 standby 로
  재합류한다. 유지보수(EXCLUDE_NODE) 노드는 역할 무관 `eligible=false` + 모듈 정지.
  restart/제어 job 진행 중은 `run/ha/op_grace_<mod>` 마커로 유예. 판정 순서 상세:
  ha_service_model.md §7·§8.
- **apply 멱등·무접촉**: `cims-ha apply` 는 스테이징 대상 5종(conf/ha.json/
  cims-health/cims-notify/unit)이 기존 적용본과 동일하면 keepalived 를 건드리지
  않는다. 변경 시에도 가동 중이면 restart 대신 **reload** (VRRP 상태 유지 —
  restart 는 MASTER 를 내렸다 올려 무의미한 절체 유발), 정지 상태면 start. 렌더
  결과 인스턴스가 0개면 **정지 상태 유지** — 인스턴스 없는 conf 로 restart 하면
  keepalived 기동 완료 신호가 없어 60초+ hang → agent heartbeat 가 막혀 offline
  오판되기 때문. apply timeout/실패는 update_ha job 실패로 정직하게 보고된다
  (sudo 미등록 dev 환경의 graceful skip 만 예외).
- **진실 기반 검사 (csc)**: 렌더가 `services.<svc>.health_module/health_config_key`
  힌트를 내리면 (csc 이고 수동 health.port 오버라이드가 없을 때), cims-health 는
  검사 시점에 노드 로컬 배포 설정
  `${cims_home}/modules/<mod>/current/<mod>/config.json` 의 그 키(flat `"Server.Port"`
  우선, nested 수용)에서 포트를 직접 읽어 검사한다. 배포기록과 노드 실파일의 포트가
  드리프트해도 헬스는 모듈이 실제 bind 하는 포트를 보므로 HA 는 흔들리지 않고,
  드리프트 자체는 `CIMS-CFG-001 config_out_of_sync` 알람이 노출한다. 읽기 실패 시
  ha.json port → 내장 default 순 fallback.
- **flap 가시화**: agent 가 notify 로그를 집계한 `metric.ha_transitions`(최근 10분
  전이 수)로 OAM 이 `CIMS-QOS-001`(check=ha_flap, 기본 6회/10분) 알람을 올린다 —
  VIP 가 반복 이동하는 상태가 조용히 지속되지 않게 하는 방어선.
- keepalived `rise=2, fall=2, interval=2s` → 4초 fault 감지, advert 1s + dead 3s
  와 합쳐 ~7초 fail-over.
- vrrp_instance 는 전 노드 `state BACKUP` 시작 — MASTER 는 priority 차등으로 선출
  (state MASTER + nopreempt 는 keepalived 가 nopreempt 를 무시하는 모순 조합).

### 11.4 notify 스크립트 동작 (role writer)

단일 모델에서 `cims-notify` 는 **role writer** — keepalived 역할 전이를
`${cims_home}/run/ha/role/<svc>.json` 에 원자적으로 기록만 하고 종료한다. 모듈
기동/정지·재기동·readiness·절체 판정은 하지 않는다. 그 role 값 변화를 agent 의
Recovery Supervisor(reconcile)가 관측해 역할에서 기대되는 모듈 상태로 수렴시킨다
(MASTER→hot·cold 기동, BACKUP→hot 만·cold 정지, FAULT(래치)→전부 정지). legacy
(cims-notify 가 직접 cold 모듈을 start/stop) 경로는 없다.

상태 전이 매핑 (`cims-notify <svc> <TYPE> <NAME> <STATE> <PRIO>`):

| keepalived state | 동작 |
|---|---|
| MASTER / BACKUP / FAULT | `run/ha/role/<svc>.json` 에 role·sequence·boot_id 기록 (모듈 제어는 Supervisor reconcile) |
| STOP | 변경 없음 — keepalived 자체 종료 시 role 무기록 |

- **role 파일 권한**: cims-notify 는 root(keepalived 자식)로 돌지만 진입 시 `umask 0022`
  로 고정한다. keepalived(v2.3.x)는 umask 0177 로 돌아 자식에 상속되는데, 그대로면
  role 파일이 0600 root-only 로 생성돼 Supervisor(cims)가 못 읽는다. `cims_home` 은
  ha.json 의 값(agent 가 자기 설치 루트로 채움 — OAM 렌더 값은 placeholder)에서 유도해
  root 가 cims 소유 트리의 절대경로에 쓸 수 있게 한다.
- **모듈 기동 컨텍스트**: 모듈 lifecycle 은 keepalived 자식(notify)이 아니라 **agent
  프로세스의 reconcile** 가 `cims-svc` 로 수행한다 — `systemctl stop/restart keepalived`
  (KillMode=control-group)가 모듈을 함께 죽이던 문제가 구조적으로 사라진다. 모듈 배포
  루트는 `modules/<mod>/current` 우선, 없으면(설치만 되고 기동 이력 없는 cold standby)
  최신 버전 디렉토리 fallback.

**VIP 선행 bind (`ip_nonlocal_bind`)** — VIP 를 설정값으로 bind 하는 모듈(csp
`LocalIp=VIP` 등)은 VIP 취득 전에도 기동 가능해야 한다 (워크플로: start → VIP
적용). agent 가 기동 시 `cims-priv net-sysctl net.ipv4.ip_nonlocal_bind 1` 로
선행 보장 (idempotent, `/etc/sysctl.d/99-cims-net-tuning.conf` 영속). `cims-ha
apply` 도 동일 값을 설정하지만 그건 VIP 적용 시점이라 최초 start 에는 늦다 —
agent 선행이 없으면 bind EADDRNOTAVAIL → 재기동 crash-loop.

HA 관리 모듈(relevant∪cold)의 재기동은 Supervisor reconcile 이 소유하고, legacy
watchdog(`supervise_tick`)은 HA 에 속하지 않은 standalone 모듈만 관할한다(이중 제어
방지). watchdog tick 은 heartbeat 대기와 분리된 고정 주기로 돌아 OAM 장애
backoff(최대 60s)가 로컬 복구를 지연시키지 않는다.

모든 전이는 `/var/log/cims-ha/notify_<svc>.log` 에 기록 (디렉토리 755·로그 644 —
비-root 운영자 열람 가능).

### 11.5 cims.sh 와의 관계

cims.sh 는 **개발 단계 도구**:
- 빌드 / 패키징 / 검증 / 시뮬레이터: `cims.sh build / pkg / verify / sim / configure`
- 운영 명령 (`start / stop / restart / status / log / ha`) 은 cims.sh 에서 제거됨

배포본 운영자는 cims.sh 호출 안 함 — agent/bin/cims-* 만 사용.

### 11.6 Console UI 흐름 — HaServicesPage (운영자가 ha.json 직접 편집 X)

운영자는 `/deploy/services` (Console "서버 + HA") 에서 서비스(=HA 그룹/standalone)
단위로 inline 편집 — 각 노드의 ha.json 은 CSC + cims_agent 가 자동 생성/분배.

데이터 모델 (sql/migrate_ha_groups.sql + sql/migrate_ha_services_wiring.sql +
sql/migrate_ha_groups_vip_nullable.sql):
- `ha_groups`: id / name / mode(active_standby|all_active) / **vip (nullable, legacy)** /
  vrid(자동) / vip_mask / auth_pass / note / **vip_bindings_json** (slot 별 VIP + 멤버 iface 매핑)
- `ha_group_members`: group_id + agent_id (`uk_agent` UNIQUE — 1 agent = 1
  group) + priority + role(master|backup)
- `cims_agent`: + **interfaces_json** (heartbeat 보고) + **service_ip_rows_json**
  (운영자 iface→slot 매핑)

> standalone 서비스 = ha_group 미배정 agent (음수 id `-agent.id` 로 frontend 매핑)

### 11.7 적용 흐름 — Apply API + multi-VIP rendering

운영자가 VipPanel / ServiceIpPanel 의 `[적용]` 클릭 시:

| 패널 | 진입점 | 결과 |
|---|---|---|
| VipPanel | `POST /api/v1/ha-groups/{id}/apply` | 멤버 전원에 `update_ha` job 큐잉 — 각 agent 가 ha.json 갱신 + `cims-ha config && apply` (keepalived reload) |
| ServiceIpPanel | `POST /api/v1/agents/{id}/apply-ip-config` | 단일 agent 에 `apply_ip_config` job — `ip addr add <ip>/<mask> dev <iface>` per row (secondary IP, idempotent) |

**multi-VIP rendering** (한 vrrp_instance 에 N VIP):
- `vip_bindings_json` 의 각 binding 이 `services.<group_name>.vips[]` 한 entry
- 동일 VRID 공유, 같은 vrrp_instance 의 `virtual_ipaddress` block 에 N 줄
- agent 별 iface 는 `binding.memberIfaces[agent_id]` 또는 service 의 `interface` field
- 한 group 내 모든 binding 은 같은 iface 사용 (제약 — 다중 iface 필요 시 그룹 분할)

**config_template ip 메타**:
패키지 `config_template.json` 의 field 에 다음 attribute 추가하면 SLOT_MAP hardcoded 대체:
```json
{ "key": "Setup.Sip.LocalIp", "type": "string",
  "ip_scope": "service", "ip_slot": "SIP", "ip_port": 5060, "ip_proto": "udp" }
```
미설정 시 `ems/core/console SLOT_MAP` 의 hardcoded fallback 사용 (csp/cmp/psp 등).

모듈 ha_capability (각 모듈 pkg.json):
- `csp/psp/isp/csc` → `active_standby`
- `cmp/pmp/imp` → `all_active`
- `cwrtc/cspsim/agent/console/phone` → `standalone`

install 정책 (csc/src/handlers/agents.py:_create_deployment):
- ha_group 정의된 agent → ha_capability 가 group.mode 와 일치해야 install OK
  (mismatch 시 400)
- ha_group 미정의 agent → 모든 모듈 install 허용 (워크플로 가이드 — 그룹 정의
  후 install 권장)
- `standalone` 모듈은 어느 그룹/그룹 없음 OK

자동 분배 (ems/core/oam/src/handlers/ha_groups.py + agent/cims_agent.py:job_update_ha):
1. 렌더 트리거 — 그룹 생성 / 멤버 추가·제거 / 그룹 수정 / [▶ 적용], **배포 생성·제거**,
   **start·stop·restart·upgrade·uninstall job 완료** (`enqueue_update_ha_for_agent`),
   배포 설정 저장으로 실효포트가 바뀐 경우. 헬스포트/cold_modules/무장(enabled) 이
   배포 목록·record status 에서 유도되는 파생값이라, 렌더 입력을 바꾸는 변이는 전부
   재렌더를 태운다 (그룹 구성 → 설치 → 서비스 시작 순서 전체에서 자동 추종;
   apply 가 멱등이라 렌더 결과가 같으면 keepalived 무접촉).
   **그룹 이탈 disarm** — 그룹 삭제·멤버 제거(교체 포함)로 그룹에서 빠진 agent 에는
   빈 `services` 의 ha.json 이 푸시되고(`_enqueue_disarm_for_agent`), agent 가
   `cims-ha uninstall` 로 keepalived 를 해제한다 — 이탈 노드에 구 vrid/VIP 무장이
   잔존하지 않는다 (유령 VIP 차단).
   **개시 국면 선착 방지** — AS 그룹이 무장 렌더로 전환되는데 아직 아무 멤버도
   VIP 를 보유하지 않았으면(최초 개시·전면 재기동), **운영자가 start 한(record
   running) 멤버**의 update_ha 를 먼저 큐잉하고 나머지 멤버는 job `not_before`
   (75s)로 지연한다 — start 한 서버가 VIP 를 잡아 그대로 서비스하고, 상대 멤버는
   그 뒤 BACKUP 으로 합류한다. 동시 전파하면 양쪽 keepalived 콜드스타트 선거에서
   유휴 standby 가 구조적으로 선착해(start 실행 노드는 자기 job 을 한 heartbeat
   늦게 회수) Active 를 선점하고 notify 가 운영자가 켠 모듈을 끄는 역전이 난다.
   start 멤버가 없으면 지정 마스터(priority 최대)를 선행. VIP 보유자가 관측되는
   운영 중 재렌더는 지연 없이 동시 전파 (apply 멱등 — 순서 무관)
2. OAM `_enqueue_update_ha_for_members` 가 멤버별 ha.json render → `update_ha` job
   큐잉 (params.ha_json — install_path 는 구 agent 호환 잔재, 신 agent 는 무시)
3. cims_agent heartbeat 시 job 회수 → `job_update_ha`:
   - `<prefix>/run/keepalived/ha.json` 갱신 (버전 트리 밖 — agent 업그레이드 무관)
   - `cims-ha --ha-dir <그 경로> install + config + apply` 자동 실행 (sudo 권한 필요)
     · 템플릿은 실행 중인 cims-ha 번들(`agent/current/keepalived/`)에서 해석
     · apply 가 cims-health/cims-notify + ha.json 을 `/etc/keepalived/{bin/,}` 에
       **root:root 로 스테이징** — keepalived.conf 는 이 고정 경로만 참조.
       버전 디렉토리 비의존 + `enable_script_security`(root 소유 요구) 통과
   - dev / sudo 미등록 시 ha.json 만 갱신 + apply 실패는 log 만 (graceful)

VRID 자동 할당 (51-255 range, ha_groups.uk_vrid UNIQUE). VIP 는 운영자 수동
입력 (네트워크 대역 의존).

### 11.8 fail-over LIVE 검증 환경 — 실 4-agent

2-node fail-over 시나리오 (`S6-SCN-FAILOVER-CSP/CMP/CSC`) 는 **실제 4개 agent**
(ctrl01/ctrl02 = Control A/S, media01/media02 = Media All-Active) 위에서 LIVE
검증한다.

**토폴로지**:

| 노드 | 역할 | HA 모드 | 설치 패키지 |
|---|---|---|---|
| `ctrl01` | Control-Server (active) | A/S | CSP, ISP, PSP (+ OAM/Console 은 mgmt host) |
| `ctrl02` | Control-Server (standby) | A/S | (동일) |
| `media01` | Media-Server | All-Active | CMP, IMP, PMP |
| `media02` | Media-Server | All-Active | (동일) |

**fail-over 트리거 / 검증** (agent sync REST + Console 으로 ssh-free 운영):
- CSP A/S: active(ctrl01) 의 csp 프로세스 kill → keepalived 가 VIP 를 ctrl02 로
  인계 (VRRP advert_int 수백 ms) → 신규 REGISTER 가 동일 VIP 로 ≤수초 내 응답.
- CMP All-Active: media01 kill → 신규 세션이 consistent-hash ring 으로 media02 에 분배.
- 진행 중 호 drop 은 허용 (cold dialog 정책 — 절체 시 신규 세션만 보장).

**S6 FAILOVER 구현 진입점**:
1. `verify/lib/items/stage6/scn_failover_csp.py` — active CSP kill → VIP 인계 후
   신규 REGISTER 응답 검증
2. `scn_failover_cmp.py` — All-Active media01 kill → 신규 세션 hash ring 분배 검증
3. `scn_failover_csc.py` — OAM active kill → Console reconnect 검증

## 12. 미확정 / 추후 검토

- **Redis register replication 구현** — hot 모듈의 register 인계 전제 (현재
  cold-mode no-op, 절체 후 단말 재등록 의존). sentinel/cluster 도입 시점 포함.
- **OAM active/standby file_store 정합** — 두 노드 oam 스토어(배포/그룹/알람
  이력)는 독립이라 절체 후 관리 데이터가 불일치. cold-spare 가 동시 write 는
  막지만 데이터 복제/공유는 별도 설계 필요.
- **cold 절체 RTO 실측** — 승격 → cold 모듈 기동 → 서비스 응답까지 목표(≤5초)
  대비 실측.
- **CMP all-active 시 RTP 포트 충돌** — 양 노드의 RTP pool 이 동일 50000~
  대역이면 NAT/SIP `c=` 라인 IP 가 노드 IP 라 문제 없음 (확인 필요)
- **단말 SIP TLS 재핸드셰이크** — VIP fail-over 후 TLS 세션 인수 불가,
  단말이 재핸드셰이크. TLS 사용 시 latency 평가 필요
- **multi-site (WAN) 이중화** — 본 설계 범위 외, 별도 라운드
