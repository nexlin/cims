# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

**Prerequisites**: `cmake`, `build-essential`, `libssl-dev`, `git`, `clang-format`

```bash
sudo apt-get install -y cmake build-essential libssl-dev clang-format
```

`clang-format` 은 검증 stage 1 (`S1-CPP-FORMAT`) 의 정적 검사용. 미설치 시 SKIP.

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
               ├─ CCscfModule  — REGISTER, SUBSCRIBE, PUBLISH(affiliation), 인증
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
- **`CCscfModule`** (`CscfModule.h/.cpp`) — REGISTER/SUBSCRIBE/**PUBLISH**(affiliation, `RecvRequestPublish`) 처리, Digest MD5 인증 헬퍼 (static)
- **`IModule`** (`IModule.h`) — 모듈 추상 인터페이스, `EModuleRouteResult` enum
- **`CGroupCallService`** — PTT 그룹콜 오케스트레이션 (on-demand `ProcessGroupCall` 키업 트리거; multipart INVITE: `mcptt-info+xml` + `resource-lists+xml`(멤버 로스터) + SDP; affiliation 게이트·chair role·broadcast initiator·개시자 caller JOIN/200 OK floor 광고)
- **`CSubscriptionManager`** — SIP SUBSCRIBE/NOTIFY 상태 관리 (GMS/CMS)
- **`CspUserMap`** / **`CGroupMap`** — 가입자/그룹 메모리 캐시 (DB primary, JSON fallback)
- **`CCmpClient`** — CMP 연동 (JSON-over-UDP, `record_dir` 전달, **`IssueSessionId()`=relay 세션식별자 cmp_sess_N 발행**, consistent-hash ring 으로 session_id→미디어노드 라우팅)
- **`CCallDir`** (`CallDir.h`) — Session-ID 기반 서비스 로깅 (call.json, participants.jsonl, session.json)
- **`CCallMap`** (`CallMap.h/.cpp`) — Call-ID↔peer 매핑 + **relay descriptor**(`m_strRelaySessionId`/`SesId`/`LocalIp`/`Caller`/`Callee`, B2BUA 양 leg 공유). teardown(`Delete`)이 **session_id 로 `CmpClient::RemoveSession` 직접 호출** — 구 `CRtpMap`(포트단독키)은 멀티 미디어노드에서 포트가 노드별 비유일이라 충돌·누수 → **제거**(`RtpMap.cpp`/`RtpThread.cpp` 삭제, 미디어분리 전 CSP 직접 relay 잔재). answer=`ModifySession(session_id)`, CreateCall 실패 회수=session_id.
- **`SipMessageLogger`** (`SipMessageLogger.h/.cpp`) — ILogCallBack 구현, psip SIP TX/RX + CMP JSON 메시지를 `{ServiceLogDir}/YYYY/MM/DD/HH/{systemId}_{iface}.msg.{mm5}.jsonl`(iface=sip/cmp/csc) + flow `{systemId}.flow.{mm5}.jsonl` 에 기록. **open-per-write(매 줄 open/append/close) + 5분 버킷**(`mm5`=00/05/.../55) — 구 "1시간 핸들 유지" 방식의 `.nfs` 고아·로그삭제 데이터유실·대용량검색 문제 해소. sesid 규칙·계승은 [docs/design/features/flow_logging.md](docs/design/features/flow_logging.md)

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
- **`McpttGroup`** (`PMcpttGroup`) — Group RTP mixing and MCPTT floor control via RTCP APP packets on `m=application` 전용 소켓 (op-codes: REQUEST=1, GRANT=2, REJECT=3, RELEASE=4, IDLE=5, TAKEN=6, REVOKE=7; chair/priority 선점; **broadcast 그룹=개시자(`_initiatorSessionId`) 외 REQUEST REJECT**; 세션 `floor.jsonl` 기록)

VoIP/PTT 리소스 풀 분리: VoIP(`PRtpTrans`, `RtpStartPort`), PTT(`PPttTrans`, `PttRtpStartPort`+`PttFloorStartPort`)

CMP command verbs: `add`, `modify`, `remove`, `ADD_PTT_GROUP`(→floor_port 응답; +`group_type`/`initiator_id`=broadcast floor 독점), `REMOVE_PTT_GROUP`, `JOIN_PTT_GROUP`(+user_floor_port+role), `LEAVE_PTT_GROUP`, `STATS_REQUEST`(→세션/포트풀/sweeper 카운터).

**sweeper(`timeoutLoop`, 60초 주기) = 고아 relay 안전망.** owner(CSP)가 비정상 종료(crash/kill)하면 CSP in-memory CallMap 소실 → relay 가 REMOVE 미수신 고아화 → 이 sweeper 가 유일 회수 수단. RTP **무수신(inactivity)** 시간 기준: 무RTP=`OrphanReclaimSec`(기본120s), RTP수신후=`SessionTimeout`(기본600s, hold/DTX 대비 보수적). 회수 시 `reason`(orphan_no_rtp|hold_timeout) 판정 + 누적 카운터(STATS `leak_reclaim_*`) + `{ServiceLogDir}/leak_reclaim/YYYY/MM/DD/reclaim.jsonl` 기록(콘솔 '누수 회수(sweeper)' 페이지·OAM `/api/v1/stats/leak-reclaims`). RtpMap fix(CSP) 후 정상 환경은 이 카운터 0 이 기대값 — 증가 시 새 누수 신호.

Config: `cmp/cmp.json` (스키마 `cmp/config/config_template.json`; sweeper 키 `OrphanReclaimSec`/`SessionTimeout`).

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

### Key data flow: PTT group call (PTT-AS) — 3GPP MCPTT (on-demand 모델, 2026-06-03 규격전환)
호 수명은 `group_type` 분기: `prearranged`/`broadcast`=on-demand(키업 트리거), `chat`=상시. **REGISTER 는 호 무영향**(구 always-on 자동초대 제거).
1. **affiliation = SIP PUBLISH**(`mcptt-affiliation-command+xml`, TS 24.379 §9) → `CCscfModule::RecvRequestPublish` → `ptt_affiliations`. (구 SUBSCRIBE 경로 호환 유지.)
2. 발신 UE 키업(그룹 `INVITE`) → `EventIncomingCall`(캐시 미스 시 `LoadFromDb()` lazy-load) → `ProcessGroupCall`
3. CSP → CMP `ADD_PTT_GROUP`(`record_dir`=`ptt/{id}`, 멤버 `id:prio:role`, **+`group_type`/`initiator_id`**) → shared RTP/Floor 할당
4. **개시자(caller)도 floor/RTP 멤버**: 200 OK 에 `m=application`(SharedFloorPort) 광고(psip `AddSdp` append) + caller `JOIN_PTT_GROUP`(floor=audio+1) → caller 음성 릴레이 + floor 참여
5. fan-out: affiliate+등록 멤버에게 multipart `INVITE`(`mcptt-info+xml` + `resource-lists+xml` 로스터[>8192B 시 생략] + SDP). 멤버 200 OK → `JOIN_PTT_GROUP`(user_floor_port + role)
6. Audio: `PPttTrans._rtpSock`; floor: `_floorSock`. **chair**=participant 항상 선점. **broadcast**=개시자 외 floor REQUEST REJECT(`reason=broadcast`)
7. 마지막 멤버 이탈 시 on-demand(prearranged/broadcast)는 `REMOVE_PTT_GROUP`+세션 종료, chat 은 상시 유지
8. 멤버 `role`/affiliation/group 식별(surrogate id + mcptt_group_id)은 DB(`ptt_groups`/`ptt_group_members`/`ptt_affiliations`)

### Key data flow: CSC subscriptions + XCAP (UE↔CSP NOTIFY, UE↔CSC HTTP)
1. UE `SUBSCRIBE Event: xcap-diff`(gms/cms) → `CCscfModule` → `CSubscriptionManager` → 200 OK + 즉시 `NOTIFY`
2. NOTIFY xcap-diff body 의 `xcap-root` = **`https://{CSC}:{4430}/`**(`Setup.Xcap.{Host,Port,Scheme}`; 구 `http://{CSP}:4420` 오지정 교정). GMS sel 은 가입자 소속 그룹별 enumerate
3. UE → **CSC McpttServer(HTTPS :4430)**: CSC-1 토큰(OAuth2 PKCE: `/idms/authreq`→`/idms/tokenreq`) → 문서 GET(`Authorization: Bearer`, `If-None-Match`→304). 무토큰 401
4. XCAP 문서 빌더는 `csc/src/services/mcptt.py`(group/user-profile/service-config). [docs/api/mcptt_api.md](docs/api/mcptt_api.md)

### Key data flow: Admin → CSP real-time sync
1. Console UI → `cims_admin.py` CRUD → DB 수정 → `notify_csp()` UDP 전송
2. `CCscInterface` 수신 → `user_change`: `CspUserMap` 캐시 즉시 갱신/삭제
3. `group_change`: 그룹 설정 reload + CMP 동기화 + GMS NOTIFY 발송

### Key data flow: Service logging (separated)

**Service Log** (VoLTE: `service_log/volte/YYYY/MM/DD/HH/.../*.d/`, PTT: `service_log/ptt/{id}/{YYYY}/{MM}/{DD}/{HH}/` 시간버킷):
1. CSP creates dir via `CCallDir` (VoLTE: `.d`; PTT: 그룹 base `ptt/{id}` = ptt_groups.id surrogate)
2. CSP writes VoLTE `call.json`/`participants.jsonl`/`session.json`; PTT `group.json`(base 디스크립터) + 시간버킷 `events.jsonl`
3. CSP passes `record_dir` to CMP (VoLTE=`.d`, PTT=`ptt/{id}` base); CMP 가 PTT 는 기록 시점 wall-clock 으로 `{YYYY}/{MM}/{DD}/{HH}/` 시간버킷 자동 회전
4. CMP writes: VoLTE `seg_*.rtp` in `.d`; PTT `floor.jsonl`/`segments.jsonl` + `seg/{NNN}/seg_NNNN_*`(100세그 shard, 빈 트랙 미생성) in 시간버킷

**SIP/Flow Log** (`{ServiceLogDir}/YYYY/MM/DD/HH/` 하위, **5분 버킷·open-per-write**):
1. `SipMessageLogger` (ILogCallBack) captures all SIP TX/RX from psip stack + CMP JSON messages
2. 파일: 원문 `{systemId}_{iface}.msg.{mm5}.jsonl`(iface=sip/cmp/csc) + 통합 flow `{systemId}.flow.{mm5}.jsonl` (`mm5`=5분 버킷 00~55). 매 줄 open/append/close (핸들 미유지 → 운영 중 로그삭제 시 `.nfs` 고아·데이터유실 방지).
3. flow 엔트리의 `seq`=원문 msg 파일의 줄번호(버킷별 리셋) → 원문 역조회 시 `ts`(HH:MM:SS)로 5분 버킷 도출
4. Flow API(`csc/src/services/flow_logger.py`)가 Call-ID→sesid→동일 sesid 전 엔트리 수집으로 B2BUA 메시지 흐름 재구성(양 leg via Session-ID). reader glob 은 `.msg.jsonl`(구 시간당) + `.msg.{mm5}.jsonl`(신 5분) 모두 매칭

## Verification (S1~S6 pipeline)

상용 배포 전 검증 절차는 6단계 파이프라인 — `verify/lib/` Python 인프라가 본체이고, `cims.sh verify stage<N>` 또는 Console UI (`/testbed/verify-v2`) 가 진입점.

| Stage | 이름 | scope | gate |
|---|---|---|---|
| S1 | 정적 검사 | py_compile / eslint / tsc / clang-format / unit test | 코드 위생 |
| S2 | 빌드 | preflight + cmake build | 컴파일 통과 |
| S3 | 스모크 | configure → start dev → 1콜 VoIP/PTT | 빠른 sanity |
| S4 | 패키지화 | tarball 5개 + manifest.json (SHA-256) | immutability gate |
| S5 | 로컬 배포 | TB-CSC → Test-agent → csc-server → csp/cmp 체인 | 배포 절차 회귀 |
| S6 | 통합 검증 | VoLTE/PTT 음성·영상 + summary | 상용 진입 |

**주요 명령**:
```bash
./cims.sh verify list                    # 등록 항목 트리
./cims.sh verify list-presets            # stage1-full~stage6-full + pipeline-full
./cims.sh verify stage1                  # 특정 stage 실행
./cims.sh verify run --preset pipeline-full
python3 -m unittest tests.test_verify_lib   # 35 unit tests
```

**핵심 파일**:
- `verify/lib/registry.py` — `@verify_item(stage, is_group, parent)` 데코레이터, `validate_registry()` 무결성 검증
- `verify/lib/runner.py` — group/leaf 펼침, BLOCKED status, stdout 마커 (item-start/end, child-result, group-end, run-end)
- `verify/lib/items/stage{1..6}/` — 항목 정의 파일 (자동 import). 30 부모 + 13 자식
- `verify/lib/items/stage5/_legacy.py` — `_verify_phase2` 22단계 1회 호출 + step marker 파싱하여 자식별 결과 분배 어댑터 (향후 Python 포팅 시 자식 함수 본체만 교체)
- `tests/cims_verify.py` — CLI (`--stage` / `--items` / `--preset`)
- `csc/src/handlers/verification.py` — Backend API (`/stages`, `/stages/<N>`, `/run`, `/jobs/<id>`, `/runs`, `/runs/<id>`)
- `cims-console/src/pages/VerificationV2Page.tsx` — Stepper + Accordion + 그룹 cascade + PDF 보고서 (LIVE polling)
- `cims-console/src/pages/VerificationHistoryPage.tsx` — 회차 이력 list + detail modal

**이력 저장**: 파일 기반 (`verify_runs/YYYY/MM/<id>.json`). `verify.lib.run_store` 가 record/list/get/stats 모두 처리. 옛 `verification_run` / `verification_run_item` DB 테이블 의존은 제거됨.

## Configuration Files

| File | Purpose |
|---|---|
| `csp/csp.json` | CSP IP/ports, realm, RTP relay, Roles (CSCF/TAS/PTT_AS/IBCF), DB, log config, **`Setup.Xcap.{Host,Port,Scheme}`**(xcap-diff NOTIFY 의 xcap-root = CSC XCAP 서버, 기본 https:4430) |
| `cmp/cmp.json` | CMP IP, control port, VoIP RTP pool (50000~), PTT RTP pool (52000~), PTT Floor pool (54000~), DTMF PTT digits, **sweeper `SessionTimeout`(600s, got-RTP idle)·`OrphanReclaimSec`(120s, 무RTP idle)** |
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
