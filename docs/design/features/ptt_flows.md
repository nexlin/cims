# PTT 서비스 케이스 및 메시지 Flow

**작성일:** 2026-04-03
**최종 수정:** 2026-06-03 (3GPP MCPTT 규격전환 완료 — on-demand 호모델 / PUBLISH affiliation / XCAP HTTP / broadcast floor / 개시자 floor 정합)

> **⭐ 2026-06-03 3GPP MCPTT 규격전환 완료 (UE↔CSP / UE↔CSC)** — 호 수명 모델이 `group_type` 으로 분기한다.
>
> | group_type | 모드 | 절차 |
> |---|---|---|
> | `prearranged` | **on-demand** | 발신 UE 의 키업(그룹 INVITE)→affiliate+등록 멤버 fan-out→무활동 해제 (TS 24.379 §10.1) |
> | `chat` | **상시(persistent)** | 상시 세션, 멤버는 affiliation 시 합류, de-affiliate/dereg 시 이탈 (§10.2) |
> | `broadcast` | on-demand + 발신자 floor 독점 | 개시자만 발언, 타 멤버 floor REQUEST 는 CMP 가 REJECT (TS 24.380 §10.3) |
>
> - **REGISTER 는 호에 무영향** (구 always-on 자동초대/기동 시 자동생성 제거). 발신 INVITE 키업이 on-demand 세션 개시 트리거 (`ProcessGroupCall`).
> - **affiliation = SIP PUBLISH** (`application/vnd.3gpp.mcptt-affiliation-command+xml`, TS 24.379 §9 / RFC 3903) → `CCscfModule::RecvRequestPublish` → `ptt_affiliations`. (구 SUBSCRIBE-presence 경로는 호환 유지.)
> - **개시자(originator) 도 CMP floor/RTP 멤버**: `ProcessGroupCall` 이 caller 를 `JOIN_PTT_GROUP`(audio/floor=audio+1) → caller RTP 릴레이 + floor 참여. 200 OK 에 `m=application`(SharedFloorPort) 광고(psip `AddSdp` append, audio-only 호엔 무영향) → 개시자가 floor dest 학습.
> - **broadcast**: `ADD_PTT_GROUP` 에 `group_type`+`initiator_id` 전달 → CMP `handleFloorRequest` 가 개시자 외 floor REQUEST 를 REJECT(`floor.jsonl reason=broadcast`).
> - **신규 그룹 즉시 발신**: `EventIncomingCall` 이 그룹 캐시 미스 시 `LoadFromDb()` lazy-reload (notify 도달 무관 안전망). + csc `notify_csp` GROUP_CHANGED 를 CSP+PSP 양쪽 broadcast.
> - **UE↔CSC XCAP HTTP**: 그룹문서/user-profile/service-config 는 **CSC McpttServer(HTTPS :4430)** 가 서빙. xcap-diff NOTIFY 의 `xcap-root` = `https://{CSC}:{4430}/`(`Setup.Xcap.{Host,Port,Scheme}`, 구 `http://{CSP}:4420` 오지정 교정). UE 는 NOTIFY 수신 → CSC-1 토큰(OAuth2 PKCE) 취득 → 문서 GET(`If-None-Match` 304). [mcptt_api.md](../../api/mcptt_api.md)
>
> **2026-06-02 3GPP 정합 (유지)**
> - 그룹 식별: `ptt_groups.id`=surrogate(키), `mcptt_group_id`=식별자. 멤버 `role`(chair/participant)·`mcptt_id`.
> - INVITE: `mcptt-info+xml` + **`resource-lists+xml`(멤버 로스터)** + SDP. (로스터는 INVITE>8192B 우려 시 생략 → GMS 의존)
> - **chair** = participant floor 항상 선점(TS 24.380). 200 OK 의 `m=application` floor 포트 파싱.
> - **로그/녹취 디렉터리**: `ptt/{id}/{YYYY}/{MM}/{DD}/{HH}/`(시간버킷) + `seg/{NNN}`(100세그 shard) + `floor.jsonl`/`group.json`. [recording.md](recording.md)
> - 그룹 권한/소유(authorized user)·콘솔 RBAC 는 [mcptt_authorization.md](mcptt_authorization.md).

---

## 케이스 목록

### Part A. 운용 설정 (서비스 전)

| # | 케이스 | 설명 |
|---|--------|------|
| A1 | 그룹 생성 | Console에서 PTT 그룹 신규 생성 → CSP/CMP 동기화 |
| A2 | 그룹 설정 변경 | 그룹명, 영상 활성화 등 속성 변경 |
| A3 | 그룹 삭제 | 그룹 삭제 → 관련 세션 정리 |
| A4 | 그룹에 멤버 추가 | 그룹에 새 멤버 편성 |
| A5 | 그룹 멤버 우선순위 변경 | 멤버 플로어 우선순위 수정 |
| A6 | 그룹에서 멤버 제거 | 그룹에서 멤버 해제 |

### Part B. 단말 등록 및 서비스 진입

| # | 케이스 | 설명 |
|---|--------|------|
| B1 | 단말 SIP 등록 | REGISTER → Digest 인증 (호에 무영향 — 자동초대 없음) |
| B1a | affiliation (PUBLISH) | 그룹 URI PUBLISH 로 affiliate → `ptt_affiliations` |
| B1b | on-demand 그룹콜 개시 | 발신 UE 키업(그룹 INVITE) → fan-out (prearranged/broadcast) |
| B2 | GMS/CMS 구독 + XCAP GET | xcap-diff 구독 → NOTIFY → CSC-1 토큰 → 문서 GET |
| B3 | 그룹 세션 수명 | on-demand(키업 시 생성/해제) vs chat(상시) |
| B4 | 단말 등록 해제 | REGISTER Expires=0 → ClearUserCall + de-affiliation |

### Part C. 서비스 중 (통화/플로어)

| # | 케이스 | 설명 |
|---|--------|------|
| C1 | 플로어 요청 및 획득 | 발언권 REQUEST → GRANT |
| C2 | 플로어 해제 | 발언권 RELEASE → IDLE |
| C3 | 플로어 우선순위 선점 | 높은 우선순위 멤버가 발언권 강제 획득 |
| C4 | 멤버 퇴장 (BYE) | 단말 종료 또는 수동 퇴장 |
| C5 | 비정상 퇴장 감지 | 네트워크 단절 → 주기적 감지 → 강제 정리 |

### Part D. 서비스 중 운용 변경 (실시간 반영)

| # | 케이스 | 설명 |
|---|--------|------|
| D1 | 활성 그룹에 멤버 추가 | 신규 멤버 INVITE + 기존 참여자 Conference NOTIFY |
| D2 | 활성 그룹에서 멤버 제거 | 해당 멤버 BYE + 잔여 참여자 Conference NOTIFY |
| D3 | 활성 그룹 멤버 우선순위 변경 | CMP modifygroup → 플로어 우선순위 즉시 반영 |
| D4 | 활성 그룹 설정 변경 | 그룹 속성 변경 → GMS NOTIFY |
| D5 | 활성 그룹 삭제 | 전체 멤버 BYE → CMP removegroup |
| D6 | CSP-CMP 연결 복구 | CMP 재시작 → 그룹 세션 재생성 + 멤버 재초대 |

---

## Part A. 운용 설정 (서비스 전)

### A1. 그룹 생성

```
Console            CSC                 CSP                     CMP
  │                 │                   │                       │
  │ POST /ptt/groups│                   │                       │
  │ {id, name,      │                   │                       │
  │  members:[...]} │                   │                       │
  │ ──────────────► │                   │                       │
  │                 │ [DB] ptt_groups   │                       │
  │                 │      INSERT       │                       │
  │                 │ [DB] ptt_group_   │                       │
  │                 │      members      │                       │
  │                 │      INSERT       │                       │
  │                 │                   │                       │
  │                 │ ── UDP ─────────► │ event: group_change   │
  │                 │                   │ action: POST          │
  │ ◄── 201 ─────── │                   │                       │
  │                 │                   │ [OnGroupConfigChanged] │
  │                 │                   │  LoadFromDb()         │
  │                 │                   │  SyncGroupsState()    │
  │                 │                   │                       │
  │                 │                   │ ── addgroup ────────► │
  │                 │                   │    {group_id,         │
  │                 │                   │     members}          │
  │                 │                   │ ◄── {ip, port} ────── │
  │                 │                   │                       │
  │                 │                   │ ── GMS NOTIFY ──────► 구독자
  │                 │                   │    (xcap-diff)        │
```

### A2. 그룹 설정 변경

```
Console            CSC                 CSP                     CMP
  │                 │                   │                       │
  │ PUT /ptt/groups │                   │                       │
  │ /{id}           │                   │                       │
  │ {name: "new"}   │                   │                       │
  │ ──────────────► │                   │                       │
  │                 │ [DB] UPDATE       │                       │
  │                 │ ── UDP ─────────► │ event: group_change   │
  │ ◄── 200 ─────── │                   │ action: PUT           │
  │                 │                   │                       │
  │                 │                   │ [OnGroupConfigChanged] │
  │                 │                   │  LoadFromDb()         │
  │                 │                   │  해시 비교 → 변경 감지 │
  │                 │                   │                       │
  │                 │                   │ ── GMS NOTIFY ──────► 구독자
```

### A3. 그룹 삭제

```
Console            CSC                 CSP                     CMP
  │                 │                   │                       │
  │ DELETE /ptt/    │                   │                       │
  │  groups/{id}    │                   │                       │
  │ ──────────────► │                   │                       │
  │                 │ [DB] DELETE       │                       │
  │                 │ ── UDP ─────────► │ event: group_change   │
  │ ◄── 200 ─────── │                   │ action: DELETE        │
  │                 │                   │                       │
  │                 │                   │ [SyncGroupsState]     │
  │                 │                   │  DB에 그룹 없음       │
  │                 │                   │ ── GMS NOTIFY ──────► 구독자
  │                 │                   │    (group deleted)    │
  │                 │                   │ ── removegroup ─────► │ RTP 세션 해제
```

### A4. 그룹에 멤버 추가

```
Console            CSC                 CSP                     CMP
  │                 │                   │                       │
  │ POST /ptt/      │                   │                       │
  │  groups/{id}/   │                   │                       │
  │  members        │                   │                       │
  │ {user_id,       │                   │                       │
  │  priority}      │                   │                       │
  │ ──────────────► │                   │                       │
  │                 │ [DB] INSERT       │                       │
  │                 │ ── UDP ─────────► │ event: group_change   │
  │ ◄── 201 ─────── │                   │ action: PUT           │
  │                 │                   │                       │
  │                 │                   │ [OnGroupConfigChanged] │
  │                 │                   │  해시 변경 감지        │
  │                 │                   │ ── modifygroup ─────► │ 우선순위 갱신
  │                 │                   │ ── GMS NOTIFY ──────► 구독자
```

### A5. 그룹 멤버 우선순위 변경

```
Console            CSC                 CSP                     CMP
  │                 │                   │                       │
  │ PUT /ptt/groups │                   │                       │
  │  /{id}/members  │                   │                       │
  │  /{uid}         │                   │                       │
  │ {priority: 2}   │                   │                       │
  │ ──────────────► │                   │                       │
  │                 │ [DB] UPDATE       │                       │
  │                 │ ── UDP ─────────► │ group_change          │
  │ ◄── 200 ─────── │                   │                       │
  │                 │                   │ [OnGroupConfigChanged] │
  │                 │                   │ ── modifygroup ─────► │ 우선순위 갱신
  │                 │                   │ ── GMS NOTIFY ──────► 구독자
```

### A6. 그룹에서 멤버 제거

```
Console            CSC                 CSP                     CMP
  │                 │                   │                       │
  │ DELETE /ptt/    │                   │                       │
  │  groups/{id}/   │                   │                       │
  │  members/{uid}  │                   │                       │
  │ ──────────────► │                   │                       │
  │                 │ [DB] DELETE       │                       │
  │                 │ ── UDP ─────────► │ group_change          │
  │ ◄── 200 ─────── │                   │                       │
  │                 │                   │ [OnGroupConfigChanged] │
  │                 │                   │ ── modifygroup ─────► │ 멤버 제거
  │                 │                   │ ── GMS NOTIFY ──────► 구독자
```

---

## Part B. 단말 등록 및 서비스 진입

### B1. 단말 SIP 등록 (호에 무영향)

```
UE(단말)                CSP
  │                      │
  │ ── REGISTER ──────► │
  │ ◄── 401 Challenge ─ │  (Digest MD5 인증)
  │ ── REGISTER+Auth ─► │
  │ ◄── 200 OK ──────── │  (Expires 3600 강제)
  │                      │
  │  ※ 구 always-on 모델의 "그룹 자동초대" 는 제거됨.
  │     REGISTER 갱신(refresh)은 진행 중인 호/floor 에 무영향
  │     (구 버전은 갱신마다 teardown+재초대 → 밤샘 불안정의 원인이었음).
```

### B1a. affiliation (SIP PUBLISH — TS 24.379 §9)

```
UE                      CSP (CCscfModule)
  │                      │
  │ ── PUBLISH ────────► │  Request-URI: sip:{group}@domain
  │  (그룹 URI)          │  Event: poc-settings, Expires: 3600(affiliate)/0(de-affiliate)
  │                      │  Content-Type: application/vnd.3gpp.mcptt-affiliation-command+xml
  │                      │  Body: <affiliate group="sip:{group}@.."/>
  │                      │ [RecvRequestPublish]
  │                      │  → InsertAffiliation / RemoveAffiliation (ptt_affiliations)
  │ ◄── 200 OK ──────── │  SIP-ETag (RFC 3903)
  │                      │  (active prearranged/chat 세션이면 late-entry InviteMember)
```
> 구 SUBSCRIBE-presence affiliation 경로는 호환을 위해 유지(추가형).

### B1b. on-demand 그룹콜 개시 (prearranged / broadcast — TS 24.379 §10.1/§10.3)

```
발신 UE(개시자)          CSP                          CMP
  │                      │                            │
  │ ── INVITE ─────────► │  Req-URI: sip:{group}@domain (키업)
  │  (그룹 URI, SDP)     │ [EventIncomingCall]
  │                      │  그룹 캐시 미스면 LoadFromDb() (lazy-load 안전망)
  │                      │ [ProcessGroupCall]
  │                      │  ── ADD_PTT_GROUP ───────► │  공유 RTP/Floor 할당
  │                      │     {group_id, members,    │  (group_type=broadcast 면
  │                      │      group_type,           │   initiator_id 동봉)
  │                      │      initiator_id}         │
  │                      │  ◄── {ip,port,floor_port} ─ │
  │ ◄── 200 OK ────────── │  SDP: m=audio {SharedPort} │
  │                      │       m=application {Floor} │  ← 개시자가 floor dest 학습
  │ ── ACK ────────────► │                            │
  │                      │  ── JOIN_PTT_GROUP(caller)► │  개시자도 floor/RTP 멤버
  │                      │     {audio, floor=audio+1} │     (음성 릴레이 + floor 참여)
  │                      │                            │
  │                      │  [fan-out] affiliate+등록 멤버에게 multipart INVITE
  │                      │  ── INVITE ──────────────► 멤버 UE … (B 흐름: 200→JOIN)
  │                      │                            │
  │  ※ 마지막 멤버 이탈 시 prearranged/broadcast 는 REMOVE_PTT_GROUP + 세션 종료.
  │     chat 은 상시 유지.
```

### B2. GMS/CMS 구독 + XCAP 문서 취득 (UE↔CSP NOTIFY + UE↔CSC HTTP)

```
UE                      CSP                          CSC McpttServer(HTTPS :4430)
  │                      │                            │
  │ ── SUBSCRIBE ──────► │  Event: xcap-diff (gms_psi/cms_psi)
  │ ◄── 200 OK ──────── │
  │ ◄── NOTIFY ──────── │  xcap-diff:
  │ ── 200 OK ────────► │   xcap-root="https://{CSC}:4430/"   ← Setup.Xcap.{Host,Port,Scheme}
  │                      │   <document sel="org.openmobilealliance.groups/users/tel:{u}/tel:{group}"/>  (gms, 가입자 그룹별)
  │                      │   <document sel="org.3gpp.mcptt.user-profile/.../user-profile"/>  (cms)
  │                      │   <document sel="org.3gpp.mcptt.service-config/.../service-config"/>
  │                      │                            │
  │ ── HTTPS GET /idms/authreq?..code_challenge(PKCE) ───────────────────────► │  (CSC-1 토큰)
  │ ◄── {code} ──────────────────────────────────────────────────────────────  │
  │ ── HTTPS POST /idms/tokenreq {code, code_verifier} ──────────────────────► │
  │ ◄── {access_token} (Bearer) ─────────────────────────────────────────────  │
  │ ── HTTPS GET {xcap-root}{sel}  Authorization: Bearer .. ─────────────────► │  GMS/CMS 문서
  │ ◄── 200 + XML (Etag) ─────────────────────────────────────────────────────  │
  │ ── HTTPS GET .. If-None-Match: {etag} ──────────────────────────────────► │
  │ ◄── 304 Not Modified ─────────────────────────────────────────────────────  │
```
> 무토큰 GET → 401. xcap-root 구 `http://{CSP}:4420`(라우트 없는 Admin 서버 오지정)을 `https://{CSC}:4430`(McpttServer)로 교정. cspsim 은 `RecvResponse`/NOTIFY 에서 floor·문서 경로를 학습해 동일 흐름 수행.

### B3. 그룹 세션 수명 (on-demand vs chat)

```
prearranged / broadcast (on-demand):
  세션 없음 ──(개시자 키업 INVITE: B1b)──► ADD_PTT_GROUP + 세션 ──(마지막 멤버 이탈)──► REMOVE_PTT_GROUP

chat (상시):
  CheckGroupIntegrity 가 active chat 세션 유지 — affiliate 멤버 합류(InviteMember),
  de-affiliate/dereg 시 이탈. (구 "기동 시 전 그룹 자동 생성(SyncGroupsState proactive)" 은 제거됨.)
```

> 신규 그룹은 `EventIncomingCall` 의 캐시 미스 lazy-reload 로 재기동 없이 즉시 발신 가능. CSC `notify_csp(GROUP_CHANGED)` 는 CSP+PSP 양쪽 broadcast.

### B4. 단말 등록 해제

```
UE                      CSP                          CMP
  │                      │                            │
  │ ── REGISTER ──────► │  Expires: 0                │
  │ ◄── 200 OK ──────── │                            │
  │                      │                            │
  │                      │ [CheckMemberState 10초 주기] │
  │                      │  미등록 멤버 감지           │
  │                      │  StopCall → OnCallTerminated │
  │                      │ ── leavegroup ────────────► │
  │                      │                            │
  │                      │ ── Conference NOTIFY ─────► 잔여 참여자
  │                      │    (user: disconnected)    │
```

---

## Part C. 서비스 중 (통화/플로어)

### C1. 플로어 요청 및 획득

Floor Control은 m=application 전용 소켓(PPttTrans._floorSock)을 통해 처리된다.
레거시 단말은 DTMF(PT=101)로 대체 가능하다.

```
UE-A (발언 요청)         CMP (PPttTrans)             UE-B,C (수신)
  │                      │                            │
  │ ── Floor Pkt ──────► │  op=FLOOR_REQUEST (1)      │
  │  (m=application port)│  [PPttTrans._floorSock 수신]│
  │                      │  → McpttGroup::onFloorPacket()
  │                      │                            │
  │                      │  [우선순위 확인]             │
  │                      │  플로어 미사용 + A가 요청:  │
  │                      │                            │
  │ ◄── Floor Pkt ─────── │  op=FLOOR_GRANT (2)       │
  │  (m=application port)│  speaker_id=A              │
  │                      │  [PPttTrans::sendFloorTo()]│
  │                      │                            │
  │                      │ ── Floor Pkt ────────────► │  op=FLOOR_TAKEN (6)
  │                      │  (각 멤버 floor port로)     │  speaker_id=A
  │                      │                            │
  │ ── RTP Audio ──────► │ ── RTP Forward ──────────► │  A의 음성 → B,C에 전달
  │  (m=audio port)      │  [PPttTrans._rtpSock 수신]  │  (수신자별 SSRC/seq 재작성)
```

**DTMF 대체 (레거시 단말):**
```
UE-A ── RTP (PT=101, digit='*', endBit=1) ──► CMP
  → McpttGroup::onRtpPacket() → DTMF 감지 → handleFloorRequest()
```

### C2. 플로어 해제

```
UE-A (화자)              CMP (PPttTrans)             UE-B,C
  │                      │                            │
  │ ── Floor Pkt ──────► │  op=FLOOR_RELEASE (4)      │
  │  (m=application port)│  [onFloorPacket]           │
  │                      │                            │
  │ ◄── Floor Pkt ─────── │  op=FLOOR_IDLE (5)        │
  │                      │ ── Floor Pkt ────────────► │  op=FLOOR_IDLE (5)
  │                      │  (각 멤버 floor port로)     │
```

### C3. 플로어 우선순위 선점

```
UE-B (낮은 우선순위, 현재 화자)   CMP              UE-A (높은 우선순위)
  │                                │                │
  │  (B가 화자 중)                  │                │
  │                                │                │
  │                                │ ◄── Floor Pkt  │  op=FLOOR_REQUEST (1)
  │                                │  [A > B 우선순위] │
  │                                │                │
  │ ◄── Floor Pkt ─────────────── │  op=FLOOR_REVOKE (7)
  │   (B의 floor port로)           │                │
  │                                │ ── Floor Pkt ─► │  op=FLOOR_GRANT (2)
  │                                │    speaker_id=A │
  │                                │                │
  │                                │ ── FLOOR_TAKEN ► ALL (각 멤버 floor port)
```

### C3b. broadcast 그룹 floor 독점 (TS 24.380 §10.3)

`group_type=broadcast` 그룹은 개시자(initiator)만 발언한다. CMP `handleFloorRequest` 가
요청자 sessionId(=userId) ≠ `_initiatorSessionId` 이면 floor 점유 여부와 무관하게 REJECT.

```
개시자(initiator)         CMP (broadcast group)        비개시자 멤버
  │                      │  (_groupType=broadcast,     │
  │                      │   _initiatorSessionId=개시자)│
  │ ── FLOOR_REQUEST ──► │                            │
  │ ◄── FLOOR_GRANT ──── │  requester==initiator → GRANT
  │                      │                            │
  │                      │ ◄── FLOOR_REQUEST ──────── │
  │                      │  requester!=initiator →    │
  │                      │ ── FLOOR_REJECT ─────────► │  floor.jsonl reason=broadcast
  │ ── RTP Audio ──────► │ ── RTP Forward ──────────► │  개시자 음성만 릴레이
```
> `initiator_id` 는 `ADD_PTT_GROUP` 으로 CSP→CMP 전달(개시자 = `ProcessGroupCall` 의 caller). 개시자는 JOIN_PTT_GROUP 으로 CMP floor 멤버 등록되어 GRANT 가능.

### C4. 멤버 퇴장 (정상 BYE)

```
UE-A (퇴장)              CSP                          CMP
  │                      │                            │
  │ ── BYE ────────────► │                            │
  │ ◄── 200 OK ──────── │                            │
  │                      │                            │
  │                      │ [OnCallTerminated]         │
  │                      │ ── leavegroup ───────────► │  멤버 RTP 스트림 제거
  │                      │    {group_id, session_id}  │
  │                      │                            │
  │                      │ [DB] participant.leave_time 기록
  │                      │                            │
  │                      │ [SendConferenceNotify]     │
  │                      │ ── NOTIFY ──────────────► 잔여 참여자
  │                      │    Event: conference       │
  │                      │    conference-info+xml     │
  │                      │    (user: disconnected)    │
```

### C5. 비정상 퇴장 감지

```
UE-A (네트워크 단절)     CSP                          CMP
  │                      │                            │
  │  ✕ (연결 끊김)       │                            │
  │                      │                            │
  │                      │ [CheckMemberState - 10초 주기]
  │                      │  UE-A가 UserMap에서 사라짐  │
  │                      │  → 활성 콜 존재 감지        │
  │                      │                            │
  │                      │ ── StopCall (BYE 시도) ──► (전달 안됨)
  │                      │ [OnCallTerminated]         │
  │                      │ ── leavegroup ───────────► │
  │                      │                            │
  │                      │ ── Conference NOTIFY ─────► 잔여 참여자
  │                      │    (UE-A: disconnected)    │
```

---

## Part D. 서비스 중 운용 변경 (실시간 반영)

### D1. 활성 그룹에 멤버 추가

```
Console        CSC              CSP                     CMP           기존 UE
  │             │                │                       │              │
  │ POST        │                │                       │              │
  │ members     │                │                       │              │
  │ ──────────► │                │                       │              │
  │             │ [DB INSERT]    │                       │              │
  │             │ ── UDP ──────► │ group_change (PUT)    │              │
  │ ◄── 201 ─── │                │                       │              │
  │             │                │ [OnGroupConfigChanged] │              │
  │             │                │ 1. LoadFromDb()       │              │
  │             │                │ 2. SyncGroupsState()  │              │
  │             │                │    해시 변경 감지      │              │
  │             │                │ ── modifygroup ─────► │ 우선순위 갱신 │
  │             │                │ ── GMS NOTIFY ────────────────────► │
  │             │                │    (xcap-diff)        │              │
  │             │                │                       │              │
  │             │                │ 3. CheckGroupIntegrity()             │
  │             │                │    새 멤버 미참여 감지  │              │
  │             │                │ ── INVITE ──────────► 새 UE          │
  │             │                │    (mcptt-info + SDP) │              │
  │             │                │ ◄── 200 OK ───────── 새 UE          │
  │             │                │ ── joingroup ────────► │ RTP 참여     │
  │             │                │                       │              │
  │             │                │ ── Conference NOTIFY ────────────► │
  │             │                │    Event: conference  │   기존 참여자
  │             │                │    (새 멤버: connected)│   전원에게
```

### D2. 활성 그룹에서 멤버 제거

```
Console        CSC              CSP                     CMP
  │             │                │                       │
  │ DELETE      │                │                       │
  │ member      │                │                       │
  │ ──────────► │                │                       │
  │             │ [DB DELETE]    │                       │
  │             │ ── UDP ──────► │ group_change (PUT)    │
  │ ◄── 200 ─── │                │                       │
  │             │                │ [OnGroupConfigChanged] │
  │             │                │ 1. LoadFromDb()       │
  │             │                │ 2. SyncGroupsState()  │
  │             │                │    ── modifygroup ──► │ 멤버 목록 갱신
  │             │                │                       │
  │             │                │ 3. CheckMemberState() │
  │             │                │    제거된 멤버 감지    │
  │             │                │ ── BYE ─────────────► 제거된 UE
  │             │                │ [OnCallTerminated]    │
  │             │                │ ── leavegroup ──────► │ RTP 제거
  │             │                │                       │
  │             │                │ ── Conference NOTIFY ► 잔여 참여자
  │             │                │    (제거 멤버:         │
  │             │                │     disconnected)     │
```

### D3. 활성 그룹 멤버 우선순위 변경

```
Console        CSC              CSP                     CMP
  │             │                │                       │
  │ PUT member  │                │                       │
  │ {priority}  │                │                       │
  │ ──────────► │                │                       │
  │             │ [DB UPDATE]    │                       │
  │             │ ── UDP ──────► │ group_change          │
  │ ◄── 200 ─── │                │                       │
  │             │                │ ── modifygroup ─────► │ 우선순위 즉시 갱신
  │             │                │                       │ (다음 FLOOR_REQUEST부터
  │             │                │                       │  새 우선순위 적용)
  │             │                │ ── GMS NOTIFY ──────► 구독자
```

### D4. 활성 그룹 설정 변경

```
Console        CSC              CSP                     CMP
  │             │                │                       │
  │ PUT /ptt/   │                │                       │
  │  groups/{id}│                │                       │
  │ ──────────► │                │                       │
  │             │ [DB UPDATE]    │                       │
  │             │ ── UDP ──────► │ group_change          │
  │ ◄── 200 ─── │                │                       │
  │             │                │ [OnGroupConfigChanged] │
  │             │                │ ── GMS NOTIFY ──────► 구독자
  │             │                │    (xcap-diff)        │
```

### D5. 활성 그룹 삭제

```
Console        CSC              CSP                     CMP
  │             │                │                       │
  │ DELETE group│                │                       │
  │ ──────────► │                │                       │
  │             │ [DB DELETE]    │                       │
  │             │ ── UDP ──────► │ group_change (DELETE)  │
  │ ◄── 200 ─── │                │                       │
  │             │                │ [SyncGroupsState]     │
  │             │                │  DB에 없음 → 삭제 감지 │
  │             │                │ ── GMS NOTIFY ──────► 구독자 (deleted)
  │             │                │                       │
  │             │                │ [CheckMemberState]    │
  │             │                │  전체 멤버 BYE 발송   │
  │             │                │ ── leavegroup (각) ──► │
  │             │                │                       │
  │             │                │ ── removegroup ─────► │ RTP 세션 해제
```

### D6. CSP-CMP 연결 복구

CMP 재시작 시 CSP가 감지하여 모든 그룹 세션을 재생성합니다.

```
CSP                                    CMP
 │                                      │
 │  [OnCmpStatusChanged(connected)]    │
 │                                      │
 │  ── addgroup (그룹1) ────────────►  │  공유 RTP 포트 재할당
 │  ◄── {ip, port} ──────────────────  │
 │                                      │
 │  ── addgroup (그룹2) ────────────►  │
 │  ◄── {ip, port} ──────────────────  │
 │                                      │
 │  [CheckGroupIntegrity]              │
 │  등록된 멤버에게 재초대 (INVITE)     │
 │  → 각 멤버 200 OK → joingroup       │
```

---

## 부록

### 프로토콜/포트 요약

| 인터페이스 | 프로토콜 | 포트 | 메시지 |
|-----------|----------|------|--------|
| UE ↔ CSP | SIP/UDP | 5060 | REGISTER, INVITE, BYE, SUBSCRIBE, NOTIFY, **PUBLISH**(affiliation) |
| UE ↔ CSC (XCAP/IdMS) | **HTTPS** | **4430** | CSC-1 토큰(/idms/*) + GMS/CMS 문서 GET (McpttServer) |
| CSP → CMP | UDP JSON | 9000 | ADD_PTT_GROUP(+group_type/initiator_id), modify, remove, JOIN_PTT_GROUP, leave |
| CSC → CSP/PSP | UDP JSON | 4421 | group_change(CSP+PSP broadcast), user_change, stats |
| UE ↔ CMP (Audio) | RTP/UDP | 52000-52018 | PTT 음성 데이터 (PPttTrans._rtpSock) |
| UE ↔ CMP (Floor) | RTCP APP/UDP | 54000-54018 | MCPTT Floor Control (PPttTrans._floorSock) |
| CSP → UE (in-dialog) | SIP NOTIFY | (dialog) | Event: conference, conference-info+xml |
| CSP → UE (out-dialog) | SIP NOTIFY | (subscription) | Event: xcap-diff (xcap-root=https://{CSC}:4430/) |

> **참고:** VoIP 1:1 통화는 별도의 PRtpTrans 풀(50000-50079)을 사용한다.
> PTT와 VoIP 포트 대역이 분리되어 리소스 독립 관리가 가능하다.

### MCPTT Floor Control RTCP APP 코드

| 코드 | 이름 | 방향 | 설명 |
|------|------|------|------|
| 1 | FLOOR_REQUEST | UE → CMP | 발언권 요청 |
| 2 | FLOOR_GRANT | CMP → UE | 발언권 승인 |
| 3 | FLOOR_REJECT | CMP → UE | 발언권 거부 (우선순위 낮음 / broadcast 비개시자) |
| 4 | FLOOR_RELEASE | UE → CMP | 발언권 해제 |
| 5 | FLOOR_IDLE | CMP → ALL | 발언권 없음 (대기) |
| 6 | FLOOR_TAKEN | CMP → ALL | 화자 변경 알림 |
| 7 | FLOOR_REVOKE | CMP → UE | 발언권 강제 회수 (선점) |

### Conference NOTIFY XML 형식 (RFC 4575)

```xml
<?xml version="1.0" encoding="UTF-8"?>
<conference-info xmlns="urn:ietf:params:xml:ns:conference-info"
  entity="sip:+82571910001@ptt.domain"
  state="partial" version="3">
  <users>
    <user entity="tel:+82571900005" state="full">
      <endpoint entity="tel:+82571900005">
        <status>connected</status>
      </endpoint>
    </user>
  </users>
</conference-info>
```

| state 값 | 의미 |
|-----------|------|
| `full` (user) | 새로 추가/갱신된 사용자 |
| `deleted` (user) | 제거된 사용자 |
| `connected` (status) | 통화 참여 중 |
| `disconnected` (status) | 통화 종료/이탈 |
| `pending` (status) | 초대 전송됨, 응답 대기 중 |
