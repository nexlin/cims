# PTT 서비스 케이스 및 메시지 Flow

> **현재 동작 요약 (3GPP MCPTT — UE↔CSP / UE↔CSC)** — 호 수명 모델이 `group_type` 으로 분기한다.
>
> | group_type | 모드 | 절차 |
> |---|---|---|
> | `prearranged` | **on-demand** | 발신 UE 의 키업(그룹 INVITE)→affiliate+등록 멤버 fan-out→무활동 해제 (TS 24.379 §10.1) |
> | `chat` | **상시(persistent)** | 상시 세션, 멤버는 affiliation 시 합류, de-affiliate/dereg 시 이탈 (§10.2) |
> | `broadcast` | on-demand + 발신자 floor 독점 | 개시자만 발언, 타 멤버 floor REQUEST 는 CMP 가 Deny #5(Receive only) (TS 24.380 §6.3.5.4.4) |
>
> - **REGISTER 는 호에 무영향**. 발신 INVITE 키업이 on-demand 세션 개시 트리거 (`ProcessGroupCall`).
> - **affiliation = SIP PUBLISH** (`application/vnd.3gpp.mcptt-affiliation-command+xml`, TS 24.379 §9 / RFC 3903) → `CCscfModule::RecvRequestPublish` → `ptt_affiliations`. SUBSCRIBE-presence affiliation 경로도 호환을 위해 동작한다.
> - **개시자(originator) 도 CMP floor/RTP 멤버**: `ProcessGroupCall` 이 caller 를 `PTT_JOIN`(audio/floor=audio+1) → caller RTP 릴레이 + floor 참여. 200 OK 에 `m=application`(SharedFloorPort) 광고(psip `AddSdp` append, audio-only 호엔 무영향) → 개시자가 floor dest 학습.
> - **broadcast**: `PTT_GROUP_ADD` 에 `group_type`+`initiator_id` 전달 → CMP `handleFloorRequest` 가 개시자 외 floor REQUEST 를 REJECT(`floor.jsonl reason=broadcast`).
> - **신규 그룹 즉시 발신**: `EventIncomingCall` 이 그룹 캐시 미스 시 `LoadFromDb()` lazy-reload (notify 도달 무관 안전망). csc `notify_csp` GROUP_CHANGED 를 CSP+PSP 양쪽 broadcast.
> - **UE↔CSC XCAP HTTP**: 그룹문서/user-profile/service-config 는 **CSC McpttServer(HTTPS :4430)** 가 서빙. xcap-diff NOTIFY 의 `xcap-root` = `https://{CSC}:{4430}/`(`Setup.Xcap.{Host,Port,Scheme}`). UE 는 NOTIFY 수신 → CSC-1 토큰(OAuth2 PKCE) 취득 → 문서 GET(`If-None-Match` 304). [mcptt_api.md](../../api/mcptt_api.md)
>
> **3GPP 정합 세부**
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

> 번호 = **3GPP 정규 서비스 진입 순서**. CIMS 실제 순서는 `B2 → B4 → B1+B3(lazy)` (본문 주석 참조).

| # | 케이스 | 설명 |
|---|--------|------|
| B1 | MCPTT 사용자 인증 (CSC-1) | OIDC/PKCE → `access_token` (규격: 서비스 진입 첫 자격증명. CIMS: XCAP 전용·lazy) |
| B2 | 단말 SIP 등록 (+service authz) | REGISTER → Digest MD5 (호 무영향. 규격은 토큰 제시 service authz) |
| B3 | 구성 취득 (GMS/CMS) | xcap-diff 구독 → NOTIFY → XCAP 문서 GET (Bearer) |
| B4 | affiliation (PUBLISH) | 그룹 URI PUBLISH → `ptt_affiliations` (※규격 전제: B1·B3 선행) |
| B5 | on-demand 그룹콜 개시 | 발신 UE 키업(그룹 INVITE) → fan-out (prearranged/broadcast) |
| B6 | 그룹 세션 수명 | on-demand(키업 시 생성/해제) vs chat(상시) |
| B7 | 단말 등록 해제 | REGISTER Expires=0 → ClearUserCall + de-affiliation |

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

> **절 번호 = 3GPP(TS 33.180 §5 / TS 23.379) 정규 서비스 진입 순서.** CIMS 실제 구현 순서는
> 이와 달라 — `B2(REGISTER/Digest) → B4(affiliation) → B1+B3(토큰+XCAP, lazy 취득)`. CIMS 는
> SIP 를 Digest MD5 로 인증하고 OIDC access_token 을 XCAP HTTP 전용으로만 쓰므로 규격의
> "토큰·구성 우선" 순서를 단순화했다. 각 절의 **"CIMS 실제 시점"** 주석 참조.

### B1. MCPTT 사용자 인증 (CSC-1 OIDC/PKCE — TS 33.180 §5 / TS 24.482)

규격상 **서비스 진입의 첫 단계**. UE 가 IdMS(CSC McpttServer)에서 OIDC Authorization Code +
PKCE 로 `access_token` 을 취득 — 이 토큰이 이후 B2(service authorization)·B3(XCAP HTTP) 양쪽의
자격증명이 된다.

```
UE                                              CSC McpttServer(HTTPS :4430)
  │ ── HTTPS GET /idms/authreq?..code_challenge(PKCE,S256) ──────────────────► │
  │ ◄── {code} ──────────────────────────────────────────────────────────────  │
  │ ── HTTPS POST /idms/tokenreq {code, code_verifier} ──────────────────────► │
  │ ◄── {access_token, id_token, refresh_token} (Bearer) ────────────────────  │
```
> PKCE 핸드셰이크(authreq`code_challenge`→code→tokenreq`code_verifier`→token)는 RFC 7636 /
> OIDC 규격에 정합. 무토큰 XCAP GET → 401.
> **CIMS 실제 시점:** CIMS 는 SIP 를 Digest(B2)로 인증하고 이 토큰을 **XCAP 전용**으로만 쓰므로,
> 실제로는 여기서 선취득하지 않고 **B3 의 XCAP GET 직전에 lazy 취득**한다.

### B2. 단말 SIP 등록 (+ service authorization — 호에 무영향)

규격: SIP 등록 후 access_token(B1)을 제시해 MCPTT service authorization 수행.
**CIMS 실제 시점:** SIP 인증을 **Digest MD5** 로 대체하고 OIDC 토큰을 SIP 에 제시하지 않음
(토큰 기반 service authorization 미사용 — 단순화).

```
UE(단말)                CSP
  │                      │
  │ ── REGISTER ──────► │
  │ ◄── 401 Challenge ─ │  (Digest MD5 인증)
  │ ── REGISTER+Auth ─► │
  │ ◄── 200 OK ──────── │  (Expires 3600 강제)
  │                      │
  │  ※ REGISTER 는 그룹 호를 자동초대하지 않는다.
  │     REGISTER 갱신(refresh)은 진행 중인 호/floor 에 무영향.
```

### B3. 구성 취득 (GMS/CMS — xcap-diff 구독 + XCAP 문서 GET; UE↔CSP NOTIFY + UE↔CSC HTTP)

규격: affiliate 가능 그룹 목록·user-profile·service-config 취득(B4 affiliation 의 근거). access_token
(B1)을 Bearer 로 제시.

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
  │ ── HTTPS GET {xcap-root}{sel}  Authorization: Bearer {access_token(B1)} ──► │  GMS/CMS 문서
  │ ◄── 200 + XML (Etag) ─────────────────────────────────────────────────────  │
  │ ── HTTPS GET .. If-None-Match: {etag} ──────────────────────────────────► │
  │ ◄── 304 Not Modified ─────────────────────────────────────────────────────  │
```
> 무토큰 GET → 401. xcap-root = `https://{CSC}:4430`(McpttServer). cspsim 은 `RecvResponse`/NOTIFY 에서 floor·문서 경로를 학습해 동일 흐름 수행.
> **CIMS 실제 시점:** SUBSCRIBE/NOTIFY(SIP)는 Digest 세션으로 동작하며, access_token 은 위 XCAP
> GET **직전에 B1 의 /idms PKCE 로 lazy 취득**한다(토큰의 유일 소비처가 XCAP).

### B4. affiliation (SIP PUBLISH — TS 24.379 §9)

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
**기록 판정 (침묵 실패 금지)** — `InsertAffiliation` 은 `mcptt_group_id → ptt_groups.id`
조회를 **먼저** 수행하고 그 결과로 성공/실패를 판정한다. 한 방 `INSERT ... SELECT ... FROM
ptt_groups WHERE mcptt_group_id=..` 은 그룹을 못 찾으면 **에러 없이 0행**을 써서 호출자가
"제휴 등록됨" 으로 오판한다(DB 는 비었는데 로그만 affiliate — 제휴 소실 추적 불가).
`mysql_affected_rows` 로도 구분되지 않는다: `ON DUPLICATE KEY UPDATE` 는 갱신값이 기존과
같으면(같은 초 재발행·PUBLISH 재전송) 0 을 반환해 "미발견" 과 "무변경" 이 겹친다.
호출자는 실패 시 `[Affiliation/PUBLISH] affiliate 미기록 ... — DB 미반영` ERROR 를 남긴다.

**회수 로그** — de-REGISTER 시 `UpdateLogoutTime` 이 그 가입자의 **전 그룹 제휴를 한 번에**
삭제한다(TS 24.379 §9: 제휴는 등록에 묶인다). 이것이 제휴를 일괄 삭제하는 유일한 경로이므로
`[Affiliation] de-register 회수 user=.. rows=N` 으로 흔적을 남긴다 — 무로그였던 탓에 "제휴
테이블이 비었다" 를 조사할 때 지운 주체를 특정할 수 없었다(binlog·general_log 도 off).

> **규격 전제조건 (3GPP):** affiliation 은 ① 인증(B1 토큰) → ② service authorization(B2) →
> ③ 구성취득(B3, GMS group docs = affiliate 가능 그룹의 근거) **이후** 단계다(TS 33.180 §5 /
> TS 23.379). 사용자는 자신이 멤버인 그룹에만 affiliate 할 수 있고(TS 24.481/24.379), 그
> 멤버십·그룹 URI 는 GMS 문서(B3)에서 얻는다. 따라서 "토큰·구성 없는 affiliation" 은 규격상
> 성립하지 않는다.
>
> **CIMS 구현 차이(의도된 단순화):** CIMS 는 SIP 를 Digest(B2)로 인증하고 affiliation 인가를
> **SIP 신원 + DB 멤버십(`ptt_group_members`)** 으로 판정하므로 PUBLISH 에 OIDC 토큰이 불필요.
> 그 결과 CIMS 실제 순서는 `B2(REGISTER) → B4(affiliation) → B1+B3(토큰+XCAP, lazy)` 로 규격의
> 토큰·구성 우선 순서를 단순화한 것이다. 규격 완전 정합 시 ①토큰 선취득(B1) ②그룹구성(B3) 취득
> 후 affiliation 하도록 순서를 재배치해야 한다.
>
> ⚠️ **SIP 세부 확인 필요:** 위 `Event: poc-settings` 는 OMA PoC 레거시 값으로 보인다.
> TS 24.379 §9 affiliation 은 (a) 사용자 **자신**의 affiliation status change 와 (b) 권한자에
> 의한 affiliation-command(`application/vnd.3gpp.mcptt-affiliation-command+xml`)가 구분되므로,
> `Event` 헤더·본문 content-type 을 TS 24.379 §9.2.1 대조로 확정할 것(현재 값은 미검증).
> SUBSCRIBE-presence affiliation 경로도 호환을 위해 동작한다(추가형).

### B5. on-demand 그룹콜 개시 (prearranged / broadcast — TS 24.379 §10.1/§10.3)

```
발신 UE(개시자)          CSP                          CMP
  │                      │                            │
  │ ── INVITE ─────────► │  Req-URI: sip:{group}@domain (키업)
  │  (그룹 URI, SDP)     │ [EventIncomingCall]
  │                      │  그룹 캐시 미스면 LoadFromDb() (lazy-load 안전망)
  │                      │ [ProcessGroupCall]
  │                      │  ── PTT_GROUP_ADD ───────► │  공유 RTP/Floor 할당
  │                      │     {group_id, members,    │  (group_type=broadcast 면
  │                      │      group_type,           │   initiator_id 동봉)
  │                      │      initiator_id}         │
  │                      │  ◄── {ip,port,floor_port} ─ │
  │ ◄── 200 OK ────────── │  SDP: m=audio {SharedPort} │
  │                      │       m=application {Floor} │  ← 개시자가 floor dest 학습
  │ ── ACK ────────────► │                            │
  │                      │  ── PTT_JOIN(caller)► │  개시자도 floor/RTP 멤버
  │                      │     {audio, floor=audio+1} │     (음성 릴레이 + floor 참여)
  │                      │                            │
  │                      │  [fan-out] affiliate+등록 멤버에게 multipart INVITE
  │                      │  ── INVITE ──────────────► 멤버 UE … (B 흐름: 200→JOIN)
  │                      │                            │
  │  ※ 마지막 확립 멤버 이탈 시 prearranged/broadcast 는 PTT_GROUP_REMOVE + 세션 종료.
  │     chat 은 상시 유지.
```

### B6. 그룹 세션 수명 (on-demand vs chat)

```
prearranged / broadcast (on-demand):
  세션 없음 ──(개시자 키업 INVITE: B5)──► PTT_GROUP_ADD + 세션 ──(마지막 확립 멤버 이탈)──► PTT_GROUP_REMOVE

chat (상시):
  CheckGroupIntegrity(10s 스윕)가 active chat 세션 유지 — affiliate 멤버 합류(InviteMember),
  de-affiliate/dereg 시 이탈.
```

- 세션 활성·마지막 이탈 판정은 **확립된 leg(200 OK 수신)만** 센다. 미응답 pending INVITE 는 세션을
  붙들지 못하며, 세션 해제 시 잔존 pending 초대는 CANCEL 된다 — 미응답 재초대 dialog 가 "활성 세션"을
  자가 재생산해 전원 이탈 후에도 REMOVE 가 밀리는 좀비 세션 방지.
- **서버 주도 주기 재초대는 chat 전용.** prearranged/broadcast 의 late entry/복구는 UE 주도
  (사용자 재참여 버튼·앱 자동 재조인, TS 24.379 모델) — 서버는 개시 시 fan-out(B5)만 수행한다.
- **재조인 시 옛 leg 정리**: 멤버가 BYE 없이 죽은 뒤 새 INVITE 로 재참여하면 같은 `(사용자,그룹)`의
  옛 leg 가 세션 맵에 고아로 남아 참가자 명단에 **중복 표기**된다 → 개시자 경로가 옛 leg 의 SIP
  다이얼로그를 정리한다. ⚠️이때 **CMP LEAVE 는 보내지 않는다** — 멤버 키가 `(group, user)` 라
  방금 JOIN 한 자기 멤버십·포트까지 회수되어 미디어가 끊긴다.
- **참가자 명단(conference NOTIFY)도 확립 leg 만** 싣는다 — 아직 200 OK 가 오지 않은 fan-out 초대
  대상이 "참여 중"으로 표시되는 것을 막는다. in-dialog 폴백 발송도 확립 leg 에만 한다(다이얼로그
  없는 leg 로는 보낼 수 없다). 반면 **구독 경로 발송은 leg 유무와 무관**하다 — 아래 통지 대상 규칙 참조.

#### 참가자 로스터 통지 경로 (RFC 4575 / RFC 6665)

`CGroupCallService::SendConferenceNotify` 는 로스터 스냅샷 1건을 만들어 **멤버 단위로 경로를
갈라** 발송한다.

| 대상 | 경로 | 단말 응답 |
|---|---|---|
| `Event: conference` 구독자 | 구독 dialog (`SendConferenceNotifyToSubscribers` → `SendNotifyToSubscriber`) | 200 OK |
| 구독 없는 확립 leg | 통화 dialog in-dialog NOTIFY (폴백) | 구독 usage 없음 → 500 (무해, 재전송 중단) |

구독자가 있으면 폴백 전체를 생략하던 방식은 구독 구현/미구현 단말이 섞인 채널에서 미구현
단말의 명단을 멈추게 하므로, 구독 경로로 통지한 **사용자 집합만** 폴백에서 제외한다.
폴백 가지는 전환기 조치이며 전 단말이 구독을 구현하면 제거한다.

구독 취급 규칙 — 어긋나면 구독자 스택이 NOTIFY 를 481 로 거절해 구독이 조용히 죽는다:

- **자원·이벤트는 Call-ID 로 물려받는다.** 갱신(refresh) SUBSCRIBE 의 Request-URI 는 자원이
  아니라 200 OK 의 Contact(= 서버 자기 주소)다. URI 로 이벤트 종류를 다시 유도하면 conference
  구독이 gms 로 재분류돼 엉뚱한 `Event: xcap-diff` NOTIFY 가 나간다 (RFC 6665 §4.1.2.2 상 갱신은
  자원·이벤트를 바꿀 수 없다). reg/gms/cms 갱신에도 같은 규칙이 적용된다.
- **To tag 는 최초 구독의 값을 유지한다** — 갱신 200 OK 와 후속 NOTIFY 의 notifier tag 가 바뀌면
  구독자 dialog 와 remote tag 가 어긋난다.
- **notifier 신원은 이벤트별로 고정** — conference 는 그룹 AoR(= conference focus), reg 는 가입자
  AoR, affiliation 은 `mcptt_psi`, gms/cms 는 각 PSI. 종료(terminated) NOTIFY 도 같은 신원·같은
  `Event` 값을 쓴다.
- **conference 구독은 제휴(affiliation)를 바꾸지 않는다.** 그룹 자원을 Request-URI 로 쓰지만
  제휴와 무관한 로스터 열람이다 — 여기서 제휴를 등록/해제하면 그룹콜 이탈 시의 구독 해지가
  제휴까지 지워 fan-out 이 조용히 끊긴다. 제휴 변경은 PUBLISH(TS 24.379 §9)와
  presence(affiliation-info) 구독 경로만 수행한다.
- 변경 인자 없는 **순수 스냅샷**(구독 수락 직후 초기 NOTIFY)에는 이탈자 엔트리를 싣지 않는다 —
  `entity="sip:@domain"` 인 빈 참가자가 단말 명단에 유령으로 뜬다.
- **구독은 참여보다 오래 산다 — 통지 대상은 "잔여 참가자"가 아니라 "구독자"다.** 단말은 채널을
  이탈해도 구독을 유지하고 미조인 채널까지 구독하므로, 확립 leg 이 0 이 되는 **마지막 멤버 이탈도
  반드시 통지**해야 구독자의 로스터가 빈 상태로 수렴한다. 통지를 생략하면 구독자는 마지막으로 받은
  스냅샷(= 그 이탈자가 아직 접속 중)을 무한히 들고 있게 되며, 서버는 이미 세션을 해제했으므로
  자연 정정도 없다. 통지는 **세션 teardown 전에** 발송한다 — `BuildConferenceInfoBody` 가
  `m_mapGroupRtp` 의 `iConfVersion` 을 증가시키므로, 맵을 지운 뒤 부르면 `version` 이 0 으로
  되돌아가 수신 스택이 stale 로 버릴 수 있다.
  이탈 통지는 BYE(`OnCallTerminated`)와 등록 해제·타임아웃(`ClearUserCall`) 양쪽이 동일 계약으로
  수행한다.

> 신규 그룹은 `EventIncomingCall` 의 캐시 미스 lazy-reload 로 재기동 없이 즉시 발신 가능. CSC `notify_csp(GROUP_CHANGED)` 는 CSP+PSP 양쪽 broadcast.

### B7. 단말 등록 해제

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
  │ ── Floor Pkt ──────► │  subtype=0 Floor Request   │
  │  (m=application port)│  [PPttTrans._floorSock 수신]│
  │                      │  → McpttGroup::onFloorPacket()
  │                      │                            │
  │                      │  [우선순위 확인]             │
  │                      │  플로어 미사용 + A가 요청:  │
  │                      │                            │
  │ ◄── Floor Pkt ─────── │  subtype=1 Floor Granted  │
  │  (m=application port)│  Duration=T2, SSRC=A      │
  │                      │  [PPttTrans::sendFloorTo()]│
  │                      │                            │
  │                      │ ── Floor Pkt ────────────► │  subtype=2 Floor Taken
  │                      │  (화자 외 각 멤버 floor port)│  Granted Party=A(MCPTT ID)
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
  │ ── Floor Pkt ──────► │  subtype=4(또는 0x14) Release│
  │  (m=application port)│  [onFloorPacket]           │
  │                      │  (0x14 면 Floor Ack 회신)   │
  │ ◄── Floor Pkt ─────── │  subtype=5 Floor Idle     │
  │                      │ ── Floor Pkt ────────────► │  subtype=5 Floor Idle
  │                      │  (각 멤버 floor port로)     │  MSN + Indicator

동시 발언(dual/multi) 중이라 **잔여 화자가 있으면 Idle 대신** 나머지 참가자에게
Floor Release Multi Talker(0x0F)를 보낸다. 화자가 RELEASE 없이 조용해지면 T1(기본 4초)
만료로 같은 처리를 한다(Revoke 를 보내지 않는다 — TS 24.380 §6.3.4.4.3).
```

### C3. 플로어 우선순위 선점

선점은 즉시 교체가 아니라 **'G: pending Floor Revoke'**(TS 24.380 §6.3.4.5)를 거친다 —
회수 통지 후 T3(기본 3초) 동안 기존 화자의 미디어를 계속 중계하며 Floor Release 를 기다리고,
요청자는 그 사이 **대기열 맨 앞**에서 기다린다.

```
UE-B (낮은 우선순위, 현재 화자)   CMP              UE-A (높은 우선순위)
  │                                │                │
  │  (B가 화자 중)                  │                │
  │                                │ ◄── Floor Pkt  │  subtype=0 Floor Request
  │                                │  [A > B 서열]   │
  │ ◄── Floor Pkt ─────────────── │  subtype=6 Revoke (cause #4 pre-empted)
  │   (B의 floor port로)           │ ── Floor Pkt ─► │  subtype=9 Queue Position Info
  │                                │  (T3 유예: B 미디어 계속 중계, T8 마다 Revoke 재전송)
  │ ── Floor Pkt ──────────────►  │  subtype=4 Floor Release (또는 T3 만료)
  │                                │ ── Floor Pkt ─► │  subtype=1 Floor Granted
  │                                │                │
  │                                │ ── Floor Taken ► 화자 외 전원
```

### C3b. broadcast 그룹 floor 독점 (TS 24.380 §6.3.5.4.4)

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
> `initiator_id` 는 `PTT_GROUP_ADD` 으로 CSP→CMP 전달(개시자 = `ProcessGroupCall` 의 caller). 개시자는 PTT_JOIN 으로 CMP floor 멤버 등록되어 GRANT 가능.

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
| CSP → CMP | UDP JSON | 9000 | PTT_GROUP_ADD(+group_type/initiator_id)/MODIFY/REMOVE, PTT_JOIN/LEAVE, PTT_FLOOR_TIER |
| CSC → CSP/PSP | UDP JSON | 4421 | group_change(CSP+PSP broadcast), user_change, stats |
| UE ↔ CMP (Audio) | RTP/UDP | 52000-52018 | PTT 음성 데이터 (PPttTrans._rtpSock) |
| UE ↔ CMP (Floor) | RTCP APP/UDP | 54000-54018 | MCPTT Floor Control (PPttTrans._floorSock) |
| CSP → UE (subscription) | SIP NOTIFY | (구독 dialog) | Event: conference, conference-info+xml |
| CSP → UE (in-dialog) | SIP NOTIFY | (통화 dialog) | Event: conference — 구독 없는 단말용 폴백 |
| CSP → UE (out-dialog) | SIP NOTIFY | (subscription) | Event: xcap-diff (xcap-root=https://{CSC}:4430/) |

> **참고:** VoIP 1:1 통화는 별도의 PRtpTrans 풀(50000-50079)을 사용한다.
> PTT와 VoIP 포트 대역이 분리되어 리소스 독립 관리가 가능하다.

### MCPTT Floor Control RTCP APP 코드

메시지 타입은 RTCP APP 의 **5비트 subtype** 이다 (TS 24.380 Table 8.2.2.1-1). subtype 의
첫 비트(0x10)가 서면 "Ack 요구" 변종이고, 수신자는 Floor Ack(0x0A)로 회신한다.

| subtype | 이름 | 방향 | 설명 |
|------|------|------|------|
| 0 | Floor Request | UE → CMP | 발언권 요청 |
| 1 | Floor Granted | CMP → UE | 발언권 승인 (Duration=남은 T2) |
| 2 | Floor Taken | CMP → 화자 외 | 발언 중인 화자 통지 (동시 발언이면 화자 목록) |
| 3 | Floor Deny | CMP → UE | 발언권 거부 (Reject Cause) |
| 4 | Floor Release | UE → CMP | 발언권 해제 (`0x14` = ack 요구 변종) |
| 5 | Floor Idle | CMP → ALL | 발언자 없음 |
| 6 | Floor Revoke | CMP → UE | 발언권 회수 통지 (선점 #4 / 발언시간 초과 #2) |
| 8 / 9 | Floor Queue Position Request / Info | UE↔CMP | 큐 위치 조회 / 응답 |
| 10 | Floor Ack | 양방향 | ack 요구 메시지 확인 |
| 0x0B | Unicast Media Flow Control | UE → CMP | 자기 하향 미디어 중단/재개 |
| 0x0E | Queued Floor Requests | 양방향 | 대기 요청 취소/결과/통지 |
| 0x0F | Floor Release Multi Talker | CMP → 잔여 화자 외 | 동시 발언 중 한 화자의 발언 종료 |

> 전체 필드·타이머 규약은 [../modules/cmp.md](../modules/cmp.md) 「Floor Control 패킷」과
> [mcptt_standard_conformance.md](mcptt_standard_conformance.md) §1 이 정본이다.

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
