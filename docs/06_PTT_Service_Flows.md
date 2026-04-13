# PTT 서비스 케이스 및 메시지 Flow

**작성일:** 2026-04-03
**최종 수정:** 2026-04-13 (CMP VoIP/PTT 핸들러 분리 반영)

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
| B1 | 단말 SIP 등록 | REGISTER → Digest 인증 → 그룹 자동 참여 |
| B2 | GMS/CMS 구독 | 그룹 관리 / 사용자 설정 변경 구독 |
| B3 | 그룹 세션 자동 생성 | CSP 기동 시 CMP에 그룹 공유 RTP 세션 생성 |
| B4 | 단말 등록 해제 | REGISTER Expires=0 또는 네트워크 단절 |

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

### B1. 단말 SIP 등록 및 그룹 자동 참여

```
UE(단말)                CSP                          CMP
  │                      │                            │
  │ ── REGISTER ──────► │                            │
  │ ◄── 401 Challenge ─ │  (Digest MD5 인증)         │
  │ ── REGISTER+Auth ─► │                            │
  │ ◄── 200 OK ──────── │                            │
  │                      │                            │
  │                      │ [CheckGroupIntegrity]      │
  │                      │ 소속 그룹에 활성 콜 없음    │
  │                      │                            │
  │ ◄── INVITE ──────── │  multipart/mixed:          │
  │     (그룹 초대)       │  Part1: mcptt-info+xml    │
  │                      │   (session-type,           │
  │                      │    group-id, caller-id)    │
  │                      │  Part2: SDP                │
  │                      │   m=audio {CMP RTP 포트}   │
  │                      │   m=application {CMP Floor} │
  │                      │   (CMP AddGroup 응답의      │
  │                      │    floor_port 사용)         │
  │                      │                            │
  │ ── 180 Ringing ───► │                            │
  │ ── 200 OK ────────► │                            │
  │                      │                            │
  │                      │ [OnCallStarted]            │
  │                      │ ── joingroup ────────────► │  RTP 수신 시작
  │                      │    {group_id, session_id,  │
  │                      │     user_ip, user_port,    │
  │                      │     user_floor_port}       │
  │ ◄── ACK ──────────── │                            │
  │                      │                            │
  │                      │ [SendConferenceNotify]     │
  │                      │ ── NOTIFY ──────────────► 기존 참여자 전원
  │                      │    Event: conference       │
  │                      │    conference-info+xml     │
  │                      │    (user: connected)       │
```

### B2. GMS/CMS 구독

```
UE                      CSP
  │                      │
  │ ── SUBSCRIBE ──────► │  Event: xcap-diff
  │    gms_psi           │  Body: resource-lists (구독할 그룹 문서 목록)
  │ ◄── 200 OK ──────── │
  │ ◄── NOTIFY ──────── │  xcap-diff (초기 상태 통지)
  │ ── 200 OK ────────► │
  │                      │
  │ ── SUBSCRIBE ──────► │  Event: xcap-diff
  │    cms_psi           │  Body: resource-lists (user-profile, service-config)
  │ ◄── 200 OK ──────── │
  │ ◄── NOTIFY ──────── │  xcap-diff (초기 상태 통지)
  │ ── 200 OK ────────► │
  │                      │
  │ ... 설정 변경 발생 시 ...
  │ ◄── NOTIFY ──────── │  xcap-diff (변경 통지)
  │ ── 200 OK ────────► │
```

### B3. 그룹 세션 자동 생성 (CSP 기동 시)

```
CSP                                    CMP
 │  [기동: MonitorLoop → SyncGroupsState]
 │                                      │
 │  ──── addgroup ──────────────────►   │  PPttTrans 풀에서 할당:
 │       {group_id,                     │  Audio RTP (52000~) +
 │        members: "u1:p1,u2:p2,..."}   │  Floor Control (54000~)
 │  ◄─── {ip, port, floor_port} ───    │
 │                                      │
 │  CSP: GroupRtpInfo에 floor_port 저장 │
 │  → INVITE SDP m=application 포트로 사용
 │                                      │
 │  (DB의 모든 그룹에 대해 반복)         │
```

**주기:** 60초마다 DB 리로드 + 멤버 해시 비교로 변경 감지

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
| UE ↔ CSP | SIP/UDP | 5060 | REGISTER, INVITE, BYE, SUBSCRIBE, NOTIFY |
| CSP → CMP | UDP JSON | 9000 | addgroup, modifygroup, removegroup, joingroup, leavegroup |
| CSC → CSP | UDP JSON | 4421 | group_change, user_change, stats |
| UE ↔ CMP (Audio) | RTP/UDP | 52000-52018 | PTT 음성 데이터 (PPttTrans._rtpSock) |
| UE ↔ CMP (Floor) | RTCP APP/UDP | 54000-54018 | MCPTT Floor Control (PPttTrans._floorSock) |
| CSP → UE (in-dialog) | SIP NOTIFY | (dialog) | Event: conference, conference-info+xml |
| CSP → UE (out-dialog) | SIP NOTIFY | (subscription) | Event: xcap-diff |

> **참고:** VoIP 1:1 통화는 별도의 PRtpTrans 풀(50000-50079)을 사용한다.
> PTT와 VoIP 포트 대역이 분리되어 리소스 독립 관리가 가능하다.

### MCPTT Floor Control RTCP APP 코드

| 코드 | 이름 | 방향 | 설명 |
|------|------|------|------|
| 1 | FLOOR_REQUEST | UE → CMP | 발언권 요청 |
| 2 | FLOOR_GRANT | CMP → UE | 발언권 승인 |
| 3 | FLOOR_REJECT | CMP → UE | 발언권 거부 (우선순위 낮음) |
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
