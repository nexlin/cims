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
./bin/cspsim -server_ip 127.0.0.1 -count 2 -user 1001 -domain csp -password 1234 -mode voip -scenario call -call_duration 5

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
               ├─ CCscfModule  — REGISTER, SUBSCRIBE, 인증, Proxy INVITE (Call-ID 유지)
               ├─ CTasModule   — VoIP 부가서비스: DND, 착신전환, 착신거부, 콜픽업
               ├─ CPttAsModule — PTT 그룹콜 (GroupCallService 래핑)
               └─ CIbcfModule  — IP-PBX 트렁크 라우팅
```

#### 콜백 순서와 Proxy/B2BUA 분기
`[CModuleDispatcher, CSipUserAgent]` 순서로 등록.
- **Proxy 모드**: 일반 VoIP 호(DND/착신전환 없음) → ModuleDispatcher가 직접 처리, Call-ID 유지, Via/Record-Route 추가
- **B2BUA 모드**: PTT 그룹콜, 트렁크, 착신전환 → CSipUserAgent로 전달, 새 Call-ID 생성

#### 핵심 클래스
- **`CModuleDispatcher`** (`ModuleDispatcher.h/.cpp`) — 중앙 디스패처, 콜 소유권 추적, 모든 SIP 이벤트 라우팅
- **`CCscfModule`** (`CscfModule.h/.cpp`) — REGISTER/SUBSCRIBE 처리, Digest MD5 인증 헬퍼 (static)
- **`IModule`** (`IModule.h`) — 모듈 추상 인터페이스, `EModuleRouteResult` enum
- **`CGroupCallService`** — PTT 그룹콜 오케스트레이션 (multipart INVITE: SDP + `application/vnd.oma.poc.groups+xml`)
- **`CSubscriptionManager`** — SIP SUBSCRIBE/NOTIFY 상태 관리 (GMS/CMS)
- **`CspUserMap`** / **`CGroupMap`** — 가입자/그룹 메모리 캐시 (DB primary, JSON fallback)
- **`CCmpClient`** — CMP 연동 (JSON-over-UDP)

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
- **`PRtpHandler`** — Per-session RTP forwarding (pthread-based); allocates ports from a pool (default 50000–50019)
- **`McpttGroup`** — Group RTP mixing and MCPTT floor control via RTCP APP packets (op-codes: REQUEST=1, GRANT=2, RELEASE=4, IDLE=5)

CMP command verbs: `add`, `modify`, `remove`, `addGroup`, `removeGroup`, `joinGroup`, `leaveGroup`.

Config: `cmp/cmp.json`.

### cspsim (`cspsim/`) — Endpoint Simulator
Automated SIP/RTP client for load and functional testing.

- **`SimSession`** — Single SIP UA: registers, subscribes, makes/receives calls, sends RTP
- **`CspsimMain`** — Spawns N parallel SimSessions, drives scenarios, collects statistics

### Key data flow: VoIP call (Proxy mode — CSCF)
1. A sends `INVITE sip:B@csp` → `CModuleDispatcher::RecvRequest()` (1st callback)
2. CSCF checks: B is registered, no DND/forward → **Proxy mode**
3. CSP copies INVITE, adds Via + Record-Route, forwards to B (Call-ID preserved)
4. B responds 180/200 → CSP removes top Via, forwards to A
5. RTP flows directly between A and B (or via CMP relay)

### Key data flow: VoIP call (B2BUA mode — TAS/IBCF)
1. A sends `INVITE sip:B@csp` → Proxy declines (DND/forward/trunk) → `CSipUserAgent` (2nd callback)
2. `EventIncomingCall()` → TAS checks DND/forward → B2BUA `CreateCall()` (new Call-ID)
3. CSP bridges two independent SIP dialogs via `CCallMap`

### Key data flow: PTT group call (PTT-AS)
1. CSP `CheckGroupIntegrity()` detects group change → PTT-AS initiates session
2. CSP requests shared RTP group from CMP (`addGroup`)
3. CSP sends multipart `INVITE` to each member (SDP + OMA POC XML with member list)
4. Members respond 200 OK → CSP instructs CMP `joinGroup` per member
5. Audio flows through CMP's `McpttGroup`; floor is controlled by RTCP APP

### Key data flow: CSC subscriptions (CSCF)
1. Client sends `SUBSCRIBE Event: gms` (or `cms`)
2. `CCscfModule` stores dialog in `CSubscriptionManager`, replies 200 OK
3. CSP immediately sends `NOTIFY` with xcap-diff XML body
4. Subscription state refreshed via Expires header

### Key data flow: Admin → CSP real-time sync
1. Console UI → `cims_admin.py` CRUD → DB 수정 → `notify_csp()` UDP 전송
2. `CCscInterface` 수신 → `user_change`: `CspUserMap` 캐시 즉시 갱신/삭제
3. `group_change`: 그룹 설정 reload + CMP 동기화 + GMS NOTIFY 발송

## Configuration Files

| File | Purpose |
|---|---|
| `csp/csp.json` | CSP IP/ports, realm, RTP relay, Roles (CSCF/TAS/PTT_AS/IBCF), DB, log config |
| `cmp/cmp.json` | CMP IP, control port, RTP port pool, DTMF PTT digits |
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
