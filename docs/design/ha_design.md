# HA 이중화 설계 — Active/Standby + All Active

> 버전: 1.0 (2026-05-12)
>
> CIMS 운영 환경의 가용성을 위한 이중화 토폴로지 설계. SIP 서버군 (CSP/PSP)
> 과 관리 서버 (CSC) 는 Active/Standby, Media 서버군 (CMP/PMP) 은 All
> Active. 본 문서는 메인 백로그 1번 (Phase 1.A) 의 deliverable 이며
> 후속 1.B ~ 1.H 의 코드 작업을 위한 합의 기준이다.

## 1. 개요

### 1.1 배경

현재 (2026-05-12 기준) CIMS 는 모든 컴포넌트가 단일 인스턴스 가정으로
구현되어 있다. P2 라운드에서 IBCF (ISP/IMP) 가 VoLTE 인스턴스와 공존
하도록 토폴로지가 정리되었으나, 노드 하나가 죽으면 서비스가 중단된다.
이를 보완하기 위해 노드 2개를 활용한 이중화 도입이 필요하다.

### 1.2 목표

| 지표 | 목표 |
|---|---|
| Mgmt API (CSC) fail-over 시간 | ≤ 5초 (VIP 이전 + Console / cims_agent 재연결 포함) |
| SIP 등록 인계 | active 장애 후 standby 가 신규 REGISTER 수락까지 ≤ 5초 |
| 신규 통화 (call) | fail-over 직후 신규 INVITE 가 5초 내 동작 |
| 진행중 통화 (in-call) | fail-over 시 끊김 허용 (cold dialog 정책) |
| Media (CMP/PMP) | 인스턴스 N 개 중 1 개 장애 시 신규 세션 분산 자동 유지 |

### 1.3 비목표

- **DB 이중화** — MariaDB Galera / Master-Master 등은 별도 트랙. 본 문서
  범위 밖. 이번 단계에서는 단일 DB 가정을 유지하며, DB 장애는 별도
  운영 절차에 위임.
- **지리적 이중화 (multi-site)** — 본 설계는 단일 사이트 (동일 L2 도메인)
  내 노드 2개 전제. WAN fail-over 는 후속 라운드.
- **WebRTC bridge (cwrtc)** — 별도 검증 항목 (사용자 요청 예정) 으로
  본 범위 외.

## 2. 사용자 확정 결정사항 (2026-05-12)

| 항목 | 결정 | 근거 |
|---|---|---|
| VIP 메커니즘 | **keepalived (VRRP)** | Linux 표준, advert 1s + dead 3s 로 ~3초 fail-over, on-prem 친화 |
| CSP/PSP state 인계 | **하이브리드** (register=hot, dialog=cold) | UE 재등록 부담 ↓, 진행중 통화는 끊김 허용 — 복잡도 / 무손실 트레이드오프 |
| CMP/PMP 분배 | **Consistent hash on Session-ID** | 동일 세션 stickiness 보장 + healthcheck 로 unhealthy 노드 ring 제외 |
| DB 이중화 | **별도 트랙** | 본 문서 범위 외 |

## 3. 대상 컴포넌트 매트릭스

| # | 컴포넌트 | 모드 | 포트 | 같은 인스턴스에 hosting |
|---|---|---|---|---|
| 1 | CSC (mgmt-server) | Active/Standby | 4420 admin + 4430 mcptt | (없음) |
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
                         │  10.0.0.20        │     (sentinel 1.D-2)
                         └───────────────────┘
                                   │
                         ┌───────────────────┐
                         │  MariaDB (단일)    │  ← DB HA 별도 트랙
                         │  10.0.0.30        │
                         └───────────────────┘
```

- **VIP_csc / VIP_csp / VIP_psp** 가 각자 VRRP 그룹으로 별도 운용
  (서비스별 fail-over 독립). Media 는 VIP 없이 양쪽 모두 healthy 면 분산.
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
2. Node B 가 `VIP_csc` 인수, `csc` 프로세스가 4420/4430 binding 시작 (이미
   기동 상태였다면 binding 만 인수 — 자세한 운영 시퀀스는 1.F)
3. `Console` 의 다음 API 호출 → 새 ARP 로 Node B 로 라우팅
4. `cims_agent` 의 heartbeat (30s 주기) 가 다음 회차에서 자동 재연결
   (`session_token` 은 동일 DB share 라 양쪽 유효)
5. **체감 영향**: Console 의 in-flight HTTP 1개 정도 실패 (재시도로 복구),
   cims_agent 는 최대 30초 사이 unsynced

### 5.2 CSP active 장애 (VoLTE/IBCF)

1. keepalived advert miss → 3초 fail-over
2. Node B 의 `csp` 프로세스 (이미 standby 모드로 기동) 가 VIP_csp 인수
   → 단말 다음 REGISTER 가 Node B 로 도착
3. Node B 는 Redis 에서 register state lookup → `CspUserMap` 에 lazy
   restore → 200 OK 응답
4. 진행중 dialog (B2BUA in-call) 는 cold 정책으로 손실 → 단말이 BYE 또는
   재 INVITE 로 복구
5. **체감 영향**: 진행중 통화 끊김, 신규 통화는 5초 내 가능

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

- **Phase 1 (1.D-1)**: 단일 Redis 인스턴스, 별도 노드 (10.0.0.20) 또는
  mgmt 노드 (10.0.0.11) 에 함께 기동
- **Phase 2 (1.D-2, 후속)**: Redis Sentinel 또는 Cluster — 단, register
  state 손실 허용범위 작아서 별도 검토 후 결정

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

- **active timeout** — `addSession` (UDP JSON request) 5초 응답 없음 →
  unhealthy 마크 + 다음 ring entry 로 재시도 (같은 세션 ID, 같은 hash
  지만 unhealthy 스킵)
- **passive heartbeat** — CSP 가 매 30초 각 CMP 로 `ping` (lightweight
  JSON command) 송신, 응답 있으면 healthy 유지
- **재진입** — `unhealthy_until` 만료 후 첫 ping success 시 ring 복귀

### 7.3 가상노드 수 (vnode=128) 선택 근거

- 노드 2개 환경에서 통계적 균형 (편차 < 5%) 보장에 충분
- 노드 추가/제거 시 영향 받는 hash 영역 = 1/N (N=총 vnode 수)
- 128 × cmp 수 = 메모리 무시 가능 (수 KB)

## 8. mgmt-server fail-over UX

VIP 단일 endpoint 라 client 코드 변경 최소:

- **Console** — `ems/core/console/vite.config.ts` 의 `VITE_ADMIN_TARGET` 을
  `VIP_csc` 로 변경. proxy 가 자동으로 새 ARP 따라감.
- **cims_agent** — `--csc-url https://VIP_csc:4420` 으로 기동. heartbeat
  실패 시 backoff + 재시도 (1.G 추가).
- **DB session_token** — CSC 양쪽이 동일 DB 를 share 하므로 token 검증
  통과. 추가 작업 없음.

다만 standby CSC 가 write API 를 허용할 경우 split-brain 위험이 있어
1.F 에서 다음 중 결정 → **(1) VIP-only cold-spare 채택 (2026-05-12)**:

1. **VIP-only mode (채택)** — Standby 노드의 `cims-csc.service` 는 stopped
   상태. keepalived 의 `notify` 스크립트가 MASTER 전이 시 `systemctl start
   cims-csc`, BACKUP/FAULT 시 `systemctl stop cims-csc` 수행.
   → 단순, split-brain 원천 차단. 단점은 fail-over 시 약 1~3초 추가 기동
   지연 (cims.sh start csc 의 sleep + DB 연결).
2. (대안) Always-on standby + write freeze — 후속 라운드에서 RTO 단축
   필요 시 (2) 로 전환 가능. CSC 의 read endpoint 만 응답하고 write 는 412
   반환하는 모드 추가 작업.
3. (기각) Both active — DB HA 결정에 종속, 권장 안 함.

**구현 (1.F 완료)**: `agent/keepalived/notify_*.sh` + `agent/systemd/
cims-{csc,csp,psp}.service.tpl` + `cims.sh ha config|apply` 가 systemd
unit 도 함께 다룸.

## 9. verify 시나리오 매핑

후속 1.H 에서 신규 추가 예정 (현재는 stub):

| 항목 ID | 시나리오 | 검증 절차 |
|---|---|---|
| S6-FAILOVER-CSC | CSC active 강제 종료 → Console 재연결 | active CSC kill → 5초 대기 → Console API 호출 200 확인 |
| S6-FAILOVER-CSP | CSP active 강제 종료 → 단말 REGISTER 복원 | active CSP kill → cspsim REGISTER → Redis lookup hit 확인 |
| S6-FAILOVER-CMP | CMP-A 강제 종료 → 신규 세션 CMP-B 분산 | active CMP-A kill → cspsim 신규 통화 5개 → 모두 CMP-B 도착 확인 |

## 10. 후속 작업 분해 (1.B ~ 1.H)

| 단계 | 작업 | 주요 영향 파일 | 예상 규모 |
|---|---|---|---|
| 1.A | 본 설계 문서 | `docs/design/ha_design.md` (이 파일) | (완료 2026-05-12) |
| 1.B | keepalived 인프라 자동화 | `agent/keepalived/`, `cims.sh ha` | (완료 2026-05-12) |
| 1.D-1 | Redis register replication 골격 (stub) | ✅ `csp/RedisStore.{h,cpp}` 신규 (cold-mode no-op) + `csp/CspUser.cpp` SetBinding/DelBinding hook. hiredis 통합은 1.D-2 에서. | (완료 2026-05-12) |
| 1.D-2 | hiredis 통합 + Redis Sentinel/Cluster | RedisStore 본체 hiredis 구현 + CMakeLists link + Sentinel 평가 | (미정) |
| 1.E | CMP consistent hash 분배 (골격) | ✅ `csp/ConsistentHashRing.h` 신규 + `csp/CmpClient.{h,cpp}` endpoint vector + AddEndpoint + SelectEndpointForSession. SendRequestAndWait 의 endpoint 분배 활성은 1.E-2 (caller 인터페이스 확장). | (완료 2026-05-12) |
| 1.F | CSC/CSP/PSP active/standby 모드 (VIP-only cold-spare) | `agent/systemd/cims-*.service.tpl`, `agent/keepalived/notify_*.sh`, `cims.sh ha config|apply` | (완료 2026-05-12) |
| 1.G | cims_agent VIP target + backoff | ✅ `agent/cims_agent.py:run_loop` exponential backoff (5s→10s→20s→max 60s, 성공 시 reset). VIP target 은 `--csc-url` 인자만 변경. | (완료 2026-05-12) |
| 1.H | verify 시나리오 stub | ✅ `verify/lib/items/stage6/scn_failover_{csc,csp,cmp}.py` 3개 (SKIP body, ha.json + multi-CMP 감지 시 LIVE 활성 분기). | (완료 2026-05-12 — stub) |

총량 예상: 본 설계 확정 후 1.B → 1.H 순서로 진행, 각 단계 독립 PR.

## 11. 운영 가이드 — keepalived 인프라 (1.B)

### 11.1 파일 구조 (B 옵션 통합 + 운영 도구 분리)

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
│   ├── ha.json                         # 실제 노드 config (.gitignore, 노드별 분리)
│   ├── keepalived.conf.tpl             # 단일 generic template (services 반복 렌더)
│   └── out/                            # `cims-ha config` 생성 결과 (.gitignore)
│       ├── keepalived.conf
│       └── cims@.service
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
agent/bin/cims-ha apply           # /etc/keepalived/ + /etc/systemd/system/ 적용 +
                                  # systemctl enable cims@{csc,csp,psp}.service (start 안 함)

agent/bin/cims-ha status          # 동작 확인
```

cims@<svc>.service 는 `enable` 만 — `start` 는 keepalived notify 가 제어. 부팅 시
standby 가 자기 자신을 띄우는 일 없음.

> **VIP 바인딩 / NIC 매핑 (multi-VIP)** — HA 그룹은 `vip_bindings: [{slot, ip, mask}]` 로
> 망별 다중 VIP 를 한 vrrp_instance 에 둔다. 각 VIP 가 붙을 NIC(`dev`)은
> **VIP 바인딩의 slot 과 동일 용도(slot) 를 가진 멤버 `service_ip_rows` 의 iface** 로 결정
> (`oam ha_groups._render_ha_for_agent`; memberIfaces 명시 시 우선). 망(role) 모델은 폐지됨.
> vrrp advert NIC 은 mgmt NIC 자동 선택. **VIP 는 서비스망(예 121.161.164.x/24)에만 둔다 —
> 내부/관리망(10.0.x) VIP 는 불필요**(과거 internal VIP 가 부팅마다 NIC 점유해 콘솔 IP 편집을
> 막던 문제로 제거). cims-priv 관리 IP/마운트 영속성은 `modules/agent.md §11` 참조.

### 11.3 health probe 정책

- `agent/bin/cims-health <svc>` — ha.json `services.<svc>.{port, proto, bind_ip}`
  lookup 후 `ss -ln{t,u}` 로 binding 확인. rc=0 / rc=1.
- 신규 서비스 추가 시 `services.<svc>.{port,proto}` 만 추가 — probe script 추가 불필요.
- keepalived `rise=2, fall=2, interval=2s` → 4초 fault 감지, advert 1s + dead 3s
  와 합쳐 ~7초 fail-over.

### 11.4 notify 스크립트 동작

상태 전이 매핑 (`cims-notify <svc> <TYPE> <NAME> <STATE> <PRIO>`):

| keepalived state | 동작 |
|---|---|
| MASTER  | `systemctl start cims@<svc>.service` — VIP 인수 후 서비스 기동 |
| BACKUP  | `systemctl stop cims@<svc>.service` — 강등 시 서비스 정지 (cold-spare) |
| FAULT   | `systemctl stop cims@<svc>.service` — health probe fail 시 자기 정지 |
| STOP    | 변경 없음 — keepalived 자체 종료 시 서비스 그대로 유지 |

모든 전이는 `/var/log/cims-ha/notify_<svc>.log` 에 기록.

1.D-1 도입 시: CSP/PSP 의 MASTER 승격에서 `cims-svc start csp` (cims@csp.service
의 ExecStart) 가 기동 시 Redis 에서 register state 를 일괄 복원 — notify 스크립트
변경 없음.

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

### 11.7 Phase 2 적용 흐름 — Apply API + multi-VIP rendering

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

**config_template ip 메타** (HaServicesPage Phase 2.3):
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

자동 분배 (csc/src/handlers/ha_groups.py + agent/cims_agent.py:job_update_ha):
1. 운영자 Console 에서 그룹 생성 / 멤버 추가 / 멤버 제거 / 그룹 수정
2. CSC `_enqueue_update_ha_for_members` 가 멤버별 ha.json render → `agent_job`
   테이블에 `update_ha` job INSERT (params: install_path + ha_json)
3. cims_agent heartbeat 시 job 회수 → `job_update_ha`:
   - `install_path/agent/keepalived/ha.json` 갱신
   - `agent/bin/cims-ha config + apply` 자동 실행 (sudo 권한 필요)
   - dev / sudo 미등록 시 ha.json 만 갱신 + apply 실패는 log 만 (graceful)

VRID 자동 할당 (51-255 range, ha_groups.uk_vrid UNIQUE). VIP 는 운영자 수동
입력 (네트워크 대역 의존).

### 11.8 fail-over LIVE 검증 환경 — 실 4-agent

2-node fail-over 시나리오 (`S6-SCN-FAILOVER-CSP/CMP/CSC`) 는 **실제 4개 agent**
(ctrl01/ctrl02 = Control A/S, media01/media02 = Media All-Active) 위에서 LIVE
검증한다. (옛 단일-호스트 NetNS 시뮬 환경은 폐기됨 — 실 인프라로 일원화.)

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
- 진행 중 호 drop 은 허용 (Hot Standby — Standby 도 모듈 기동 유지 정책).

**S6 FAILOVER 구현 진입점**:
1. `verify/lib/items/stage6/scn_failover_csp.py` — active CSP kill → VIP 인계 후
   신규 REGISTER 응답 검증
2. `scn_failover_cmp.py` — All-Active media01 kill → 신규 세션 hash ring 분배 검증
3. `scn_failover_csc.py` — OAM active kill → Console reconnect 검증

## 12. 미확정 / 추후 검토

- **Redis sentinel / cluster 도입 시점** — 1.D-1 안정화 후 register
  손실 허용범위 재평가
- **CSC active/standby 모드의 hot vs cold spare** — 1.F 에서 운영
  요구사항 (RTO) 재확정
- **CMP all-active 시 RTP 포트 충돌** — 양 노드의 RTP pool 이 동일 50000~
  대역이면 NAT/SIP `c=` 라인 IP 가 노드 IP 라 문제 없음 (확인 필요)
- **단말 SIP TLS 재핸드셰이크** — VIP fail-over 후 TLS 세션 인수 불가,
  단말이 재핸드셰이크. TLS 사용 시 latency 평가 필요
- **multi-site (WAN) 이중화** — 본 설계 범위 외, 별도 라운드
