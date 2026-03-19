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
SIP server implementing registration, unicast calls, and PTT group calls.

- **`CSipServer`** — SIP protocol handler (REGISTER, INVITE, SUBSCRIBE, BYE, etc.)
- **`CGroupCallService`** — PTT group call orchestration; sends multipart INVITE (SDP + `application/vnd.oma.poc.groups+xml`) to each group member
- **`CSubscriptionManager`** — SIP dialog-based SUBSCRIBE/NOTIFY state machine for GMS/CMS (group/service subscriptions per CSC/IMS)
- **`CUserMap`** / **`CGroupMap`** — Runtime registries loaded from `User/*.json` / `Group/*.json`
- **`CmpClient`** — Async JSON-over-UDP client that instructs CMP to create/join/remove RTP sessions

Config: `csp/csp.json`. User/group data live as JSON files in `csp/User/` and `csp/Group/`.

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

### Key data flow: PTT group call
1. cspsim sends `INVITE sip:1000@csp`
2. CSP loads `Group/1000.json`, requests shared RTP group from CMP (`addGroup`)
3. CSP sends multipart `INVITE` to each member (SDP + OMA POC XML with member list)
4. Members respond 200 OK → CSP instructs CMP `joinGroup` per member
5. Audio flows through CMP's `McpttGroup`; floor is controlled by RTCP APP

### Key data flow: CSC subscriptions
1. Client sends `SUBSCRIBE Event: gms` (or `cms`)
2. CSP stores dialog in `CSubscriptionManager`, replies 200 OK
3. CSP immediately sends `NOTIFY` with xcap-diff XML body describing group/service config
4. Subscription state refreshed via Expires header; CSP tracks per-CallId

## Configuration Files

| File | Purpose |
|---|---|
| `csp/csp.json` | CSP IP/ports, realm, RTP relay endpoint, data folder paths, log config |
| `cmp/cmp.json` | CMP IP, control port, RTP port pool, DTMF PTT digits |
| `csp/User/{id}.json` | User credentials, DND flag, call forward/reject rules |
| `csp/Group/{id}.json` | Group name and member list with priorities |
| `csp/SipServerXml/*.xml` | SIP routing rules |

## External Dependencies

Managed by CMake `ExternalProject_Add`; auto-downloaded on first build:
- **psip** / **pasf** — SIP stack and framework (`ext/`)
- **oneTBB** — Intel Threading Building Blocks (downloaded to `ext/oneTBB_down/`, installed to `pkg/`)
- **opencore-amr** / **vo-amrwbenc** — AMR-NB/WB audio codecs (`ext/`)
- **googletest** — Unit testing framework (`ext/googletest/`)
