# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

**Prerequisites**: `cmake`, `build-essential`, `libssl-dev`, `git`

```bash
sudo apt-get install -y cmake build-essential libssl-dev
```

**Build** (out-of-source, from repo root):
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

The first build downloads and compiles external dependencies (oneTBB, opencore-amr, vo-amrwbenc, googletest). Binaries land in `build/bin/`.

**Distribution package**:
```bash
cd build && make dist
# Output: build/dist/ (organized by component)
```

## Running

Start CMP (media server) before CSP (call server):

```bash
# Media provider
./bin/cmp ../cmp/cmp.json

# Call control server (foreground with -n, background via csp.sh)
./bin/csp ../csp/csp.json -n
```

**Simulator** (test client):
```bash
# VoIP call test (2 sessions)
./bin/cspsim -server_ip 127.0.0.1 -count 2 -user 1001 -domain csp -password 1234 -mode volte -scenario call -call_duration 5

# PTT group call test (4 sessions)
./bin/cspsim -server_ip 127.0.0.1 -count 4 -user 1001 -domain csp -password 1234 -mode ptt -group 1000 -scenario group_call -call_duration 10
```

Interactive cspsim commands: `s` (stats), `c` (call), `g` (group call), `t`/`r` (PTT push/release), `sub` (subscribe), `q` (quit).

**Test environment** is pre-configured in `test_run/` with 4 users (1001–1004) and group 1000.

## Architecture

CIMS is a 3-tier PTT/VoIP server: **CSP** handles SIP signaling, **CMP** manages RTP media, **cspsim** simulates endpoints.

```
cspsim  ←─ SIP (UDP 5060 / TCP 25061 / TLS 5061) ──→  CSP
                                                         │
                                               UDP JSON (port 9001)
                                                         │
                                                        CMP  ←─ RTP ─→ clients
```

### CSP (`csp/`) — Call Service Platform
IMS 역할 기반 모듈형 SIP 서버. 단일 프로세스에서 CSCF + TAS + PTT-AS + IBCF 역할을 설정 기반으로 활성화/비활성화.

#### 모듈 구조

```
SIP (5060) → CModuleDispatcher (ISipStackCallBack + ISipUserAgentCallBack)
               ├─ CCscfModule  — REGISTER, SUBSCRIBE, 인증
               ├─ CTasModule   — VoIP B2BUA 호 처리: DND, 착신전환, 착신거부, 콜픽업
               ├─ CPttAsModule — PTT 그룹콜 (GroupCallService 래핑)
               └─ CIbcfModule  — IP-PBX 트렁크 라우팅
```

#### 콜백 순서 (B2BUA 전용)
`[CModuleDispatcher, CSipUserAgent]` 순서로 등록.
- **모든 VoIP 호**: ModuleDispatcher가 REGISTER/SUBSCRIBE만 직접 처리, INVITE는 CSipUserAgent(B2BUA)로 전달
- **B2BUA**: 새 Call-ID 생성, CMP 경유 RTP relay, Session-ID로 양 leg 매핑

#### 핵심 클래스
- **`CModuleDispatcher`** (`ModuleDispatcher.h/.cpp`) — 중앙 디스패처, 콜 소유권 추적, 모든 SIP 이벤트 라우팅
- **`CCscfModule`** (`CscfModule.h/.cpp`) — REGISTER/SUBSCRIBE 처리, Digest MD5 인증 헬퍼 (static)
- **`IModule`** (`IModule.h`) — 모듈 추상 인터페이스, `EModuleRouteResult` enum
- **`CGroupCallService`** — PTT 그룹콜 오케스트레이션 (multipart INVITE: SDP + `application/vnd.oma.poc.groups+xml`)
- **`CSubscriptionManager`** — SIP SUBSCRIBE/NOTIFY 상태 관리 (GMS/CMS)
- **`CspUserMap`** / **`CGroupMap`** — 가입자/그룹 메모리 캐시 (DB primary, JSON fallback)
- **`CCmpClient`** — CMP 연동 (JSON-over-UDP, `record_dir` 전달)
- **`CCallDir`** (`CallDir.h`) — Session-ID 기반 서비스 로깅 (call.json, participants.jsonl, session.json)
- **`SipMessageLogger`** (`SipMessageLogger.h/.cpp`) — ILogCallBack 구현, psip SIP TX/RX + CMP JSON 메시지를 `{MsgLogDir}/csp/sip/YYYY/MM/DD/HH/sip.jsonl`에 기록

#### 역할 설정 (`csp.json`)
```json
{
  "Setup": {
    "Roles": {
      "CSCF": true,
      "TAS": true,
      "PTT_AS": true,
      "IBCF": false
    }
  }
}
```
`Roles` 섹션 미지정 시 모든 역할 활성화 (하위 호환).

Config: `csp/csp.json`. User/group data: DB (MariaDB) primary, `csp/User/` / `csp/Group/` JSON fallback.

### CMP (`cmp/`) — Component Media Provider
RTP relay and floor control server, controlled entirely via JSON commands over UDP from CSP.

- **`CmpServer`** — Listens on UDP (default port 9000), dispatches commands
- **`PRtpTrans`** — VoIP RTP 핸들러: 4포트 블록 (Audio RTP/RTCP + Video RTP/RTCP), 포트 50000~ 대역
- **`PPttTrans`** — PTT 전용 핸들러: Audio RTP(52000~) + Floor Control(54000~) 독립 소켓
- **`McpttGroup`** — Group RTP mixing and MCPTT floor control via RTCP APP packets on `m=application` 전용 소켓 (op-codes: REQUEST=1, GRANT=2, RELEASE=4, IDLE=5)

VoIP/PTT 리소스 풀 분리: VoIP(`PRtpTrans`, `RtpStartPort`), PTT(`PPttTrans`, `PttRtpStartPort`+`PttFloorStartPort`)

CMP command verbs: `add`, `modify`, `remove`, `addGroup`(→floor_port 응답), `removeGroup`, `joinGroup`(+user_floor_port), `leaveGroup`.

Config: `cmp/cmp.json`.

### cspsim (`cspsim/`) — Endpoint Simulator
Automated SIP/RTP client for load and functional testing.

- **`SimSession`** — Single SIP UA: registers, subscribes, makes/receives calls, sends RTP
- **`CspsimMain`** — Spawns N parallel SimSessions, drives scenarios, collects statistics

### Key data flow: VoIP call (B2BUA — all calls via CMP)
1. A sends `INVITE sip:B@csp` → `CModuleDispatcher::RecvRequest()` → forwards to `CSipUserAgent`
2. `EventIncomingCall()` → TAS checks DND/forward → B2BUA `CreateCall()` (new Call-ID for B-leg)
3. CSP generates **Session-ID**, maps both leg Call-IDs → same Session-ID → same `.d` directory
4. CSP sends CMP `add` for RTP relay, bridges two independent SIP dialogs via `CCallMap`
5. `session.json` in `.d` directory stores `{session_id, call_ids: [leg_a, leg_b]}` for correlation
6. SIP stack sends only 100 Trying; 180 Ringing is forwarded from callee (no auto-180)
7. RTP always flows through CMP relay

### Key data flow: PTT group call (PTT-AS)
1. CSP `CheckGroupIntegrity()` detects group change → PTT-AS initiates session
2. CSP requests shared RTP group from CMP (`addGroup` with `record_dir`)
3. CMP allocates shared RTP port
4. CSP sends multipart `INVITE` to each member (SDP + OMA POC XML with member list)
5. Members respond 200 OK → CSP extracts `m=application` port → CMP `joinGroup` (user_floor_port 포함)
6. Audio flows through CMP `PPttTrans._rtpSock`; floor controlled via `PPttTrans._floorSock` (m=application 전용)

### Key data flow: CSC subscriptions (CSCF)
1. Client sends `SUBSCRIBE Event: gms` (or `cms`)
2. `CCscfModule` stores dialog in `CSubscriptionManager`, replies 200 OK
3. CSP immediately sends `NOTIFY` with xcap-diff XML body
4. Subscription state refreshed via Expires header

### Key data flow: Admin → CSP real-time sync
1. Console UI → `cims_admin.py` CRUD → DB 수정 → `notify_csp()` UDP 전송
2. `CCscInterface` 수신 → `user_change`: `CspUserMap` 캐시 즉시 갱신/삭제
3. `group_change`: 그룹 설정 reload + CMP 동기화 + GMS NOTIFY 발송

### Key data flow: Service logging (separated)

**Service Log** (`service_log/{type}/YYYY/MM/DD/HH/.../*.d/`):
1. CSP creates session directory via `CCallDir`
2. CSP writes `call.json` (call metadata), `participants.jsonl`, `session.json` (Session-ID + Call-ID mapping)
3. CSP passes `record_dir` to CMP via `add`/`addgroup` JSON command
4. CMP writes recording files (`raw_a.rtp`, `raw_b.rtp`, `seg_*.rtp`) to session directory

**SIP Log** (`msg_log/csp/sip/YYYY/MM/DD/HH/sip.jsonl`):
1. `SipMessageLogger` (ILogCallBack) captures all SIP TX/RX from psip stack + CMP JSON messages
2. Each line includes Call-ID, method, from/to, direction, full message text
3. Flow API searches `sip.jsonl` by Call-ID to reconstruct B2BUA message flow (both legs via Session-ID)

## Configuration Files

| File | Purpose |
|---|---|
| `csp/csp.json` | CSP IP/ports, realm, RTP relay, Roles (CSCF/TAS/PTT_AS/IBCF), DB, log config |
| `cmp/cmp.json` | CMP IP, control port, VoIP RTP pool (50000~), PTT RTP pool (52000~), PTT Floor pool (54000~), DTMF PTT digits |
| `csp/User/{id}.json` | User credentials, DND flag, call forward/reject rules (DB fallback) |
| `csp/Group/{id}.json` | Group name and member list with priorities (DB fallback) |
| `csp/SipServerXml/*.xml` | SIP routing rules (IP-PBX trunk, IBCF) |
| `csc/bin/csc_pihttp/config/csc.json` | CSC admin/MCPTT server config, DB, CspNotify endpoint |

## External Dependencies

Managed by CMake `ExternalProject_Add`; auto-downloaded on first build:
- **psip** / **pasf** — SIP stack and framework (`ext/`)
- **oneTBB** — Intel Threading Building Blocks (downloaded to `ext/oneTBB_down/`, installed to `pkg/`)
- **opencore-amr** / **vo-amrwbenc** — AMR-NB/WB audio codecs (`ext/`)
- **googletest** — Unit testing framework (`ext/googletest/`)
