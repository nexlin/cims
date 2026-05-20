---
name: project_cims_overview
description: CIMS architecture — modular IMS CSP with CSCF/TAS/PTT-AS/IBCF roles. R1~R8 psip+CSP refactoring 완결 (2026-04-23).
type: project
originSessionId: c03edf8f-dcb9-4028-aabf-f7046cd0610b
---
CIMS is a 3-tier PTT/VoIP server: **CSP** (SIP signaling), **CMP** (RTP media), **cspsim** (test client).

**2026-04-23 Update — psip + CSP multi-listener refactoring R1~R8 완결 (feature/sip-console-runtime).**
상세 진행/상태는 `project_phase_status.md`, 설계 근거는 `project_psip_csp_refactor.md` 참조.

## Components
- `csp/` — C++ SIP server (CSP) — IMS 모듈형 아키텍처 (CSCF + TAS + PTT-AS + IBCF)
- `cmp/` — C++ RTP media server (CMP)
- `cspsim/` — C++ endpoint simulator
- `cwrtc/` — WebRTC bridge (ws://127.0.0.1:8080)
- `csc/bin/csc_pihttp/` — Python HTTP API server (admin + MCPTT)
- `cims-console/` — React admin Web UI (port 3001, targets admin API 4420)
- `cims-phone/` — React MCPTT soft-phone UE UI (port 3000, targets MCPTT API 4430)

## CSP Module Architecture (completed 2026-04-02)
CSP is a single binary with four IMS roles, enabled/disabled via `csp.json` Roles config:

- **CModuleDispatcher** — central dispatcher, replaces old monolithic CSipServer
  - Callback order: [ModuleDispatcher, CSipUserAgent] — proxy mode intercepts before B2BUA
- **CCscfModule** — REGISTER, SUBSCRIBE, auth, Proxy INVITE (Call-ID preserved)
- **CTasModule** — VoIP supplementary services (DND, call forward, call reject, call pickup)
- **CPttAsModule** — PTT group call orchestration (wraps CGroupCallService)
- **CIbcfModule** — IP-PBX trunk routing via SipServerMap

VoIP calls use **Proxy mode** (Call-ID preserved) by default. B2BUA fallback for PTT, trunk, and call forward scenarios.

## CSC → CSP Real-time Sync (completed 2026-04-02)
- `cims_admin.py` calls `notify_csp()` after CRUD → UDP to CSP port 4421
- CSP `CCscInterface` handles `user_change` (cache reload/delete) and `group_change` (group reload + CMP sync)

## CSC Python Server Split
- Admin Server port 4420: CIMS Web API (auth, users, subscriptions, groups, call-logs)
- MCPTT Server port 4430: IdMS/GMS/CMS/KMS (3GPP MCPTT standards)
- Config: `csc/bin/csc_pihttp/config/csc.json`

## Key Design Decisions
- MCPTT IdMS uses OAuth2 PKCE (RFC 7636) + JWT
- MariaDB: `csc_idms` (auth), `cims` (users, subscriptions, groups, call_logs)
- CSP user cache: DB primary, JSON file fallback, lazy-load on REGISTER
- CSP group data: DB primary (60s polling + event-driven reload), JSON fallback
