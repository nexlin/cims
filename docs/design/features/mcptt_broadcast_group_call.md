# MCPTT 일제 통화 (Broadcast Group Call) — 규격 정합 설계

> 일제 통화 = **개시자 한 명만 말하고 나머지는 듣기만 하며, 개시자의 발언이 끝나면 통화도 끝나는
> 그룹 통화**(3GPP TS 24.379 §4.12). 이 문서는 규격 요구, CIMS 구현의 규격 대비 판정, 컴포넌트별
> 구현 위치(CSP·CMP·CSC·단말)의 정본이다. 발언권 절차의 일반은
> [ptt_flows.md](ptt_flows.md) C1~C3, 긴급·임박 조건은 [mcptt_emergency_modes.md](mcptt_emergency_modes.md) 를 본다.
>
> 근거 규격 판본: TS 24.379 V18.13.0 · TS 24.380 V18.7.0 · TS 24.481 V18.3.0 (Release 18). R13·R14 는 TS 24.379 V18.14.0 · TS 24.380 V18.8.0.
>
> **구현 상태** — 서버(CSP·CMP·CSC·DB·콘솔)·검증(cspsim·계측기·S3)·단말 코어(SDK·Android PTT — §4.4 U1~U5)·관제 앱 Windows 데스크톱(U6)은
> 반영됐다(§2). 단말 코어는 암묵적 발언 요청(R14 — 개시 INVITE 로 발언까지)도 하고(U7), Windows 관제 앱의 [일제 통화] 는 그것을 쓰는 한 버튼이다(U6).
> 관제 앱 Android 태블릿(U6)은 코드만 반영했고 Android 빌드·실기가 남았다. 애드혹(ad hoc) 일제 통화(R13)는 단말·관제 앱 두 벌이 반영했고 CSP 가 남았다.
> 남은 것은 CSP 의 암묵적 발언 요청 해석(R14 편차)·CSP ad hoc 일제 통화(R13)·최소 affiliation 인원 해제(R10 ③)다(§7).

---

## 1. 규격 요구 (Release 18, on-network)

| # | 요구 | 근거 |
|---|---|---|
| R1 | 일제 통화는 **통화(호) 단위 표식**이다. 개시 단말이 그룹 INVITE 의 `application/vnd.3gpp.mcptt-info+xml` 에 `<broadcast-ind>true</broadcast-ind>` 를 싣는다(pre-established session 이면 Refer-To 의 hname "body"). 없으면 일반 그룹 통화다 | TS 24.379 §6.2.8.2, Annex F.1(`broadcast-ind` 의미 11) |
| R2 | 그룹 종류가 아니다 — 사용자는 **자신이 멤버인 어느 MCPTT 그룹에서나** 일제 통화를 개시할 수 있다. user-broadcast group·group-broadcast group(그룹의 그룹)은 Release 18 절차에 없다(인가 설정 요소는 있으나 어떤 절차도 쓰지 않음) | TS 24.379 §4.12 NOTE 1·2 |
| R3 | `<session-type>` 값은 `chat`·`prearranged`·`private`·`first-to-answer`·`ambient-listening`·`adhoc` 뿐이다 — 일제 통화는 `prearranged` + `<broadcast-ind>` 다 | TS 24.379 Annex F.1 (`session-type` 의미 2) |
| R4 | 그룹 문서의 그룹 종류는 `<on-network-invite-members>`(true = 서버가 멤버를 초대하는 prearranged, false = chat)로 표현한다. 그룹 문서에 `session-type` 요소는 없다 | TS 24.481 §7.2.2 a)·§7.2.8, TS 24.379 §6.3.4.1.4 4)a) (`session-type` 을 그룹 종류로 결정) |
| R5 | 발언권: 개시자의 Floor Request 만 중재 로직으로 넘기고, 다른 참가자의 요청은 **호 상태와 무관하게** Floor Deny cause #5(Receive only) | TS 24.380 §6.3.5.3.4, §6.3.5.4.4 |
| R6 | 서버가 보내는 Floor Granted/Taken/Idle/Deny/Revoke 등에 Floor Indicator **B-bit(0x4000, Broadcast group call)**, Floor Taken 의 **Permission to Request the Floor = 0**(Floor Idle 에는 그 필드가 없다 — 메시지 형식이 Message Sequence Number·Track Info·Floor Indicator 뿐) | TS 24.380 §6.3.4.4.2 3d, §6.3.4.3.2, §8.2.8, §8.2.3.15 |
| R7 | 개시자 = 일제 통화를 **개시한** 사용자. 진행 중인 호에 나중에 합류한 참가자는 개시자가 아니다 | TS 24.380 §6.3.5.3.4 ("the initiator of the broadcast group call") |
| R8 | 개시 단말의 Floor Request 는 Floor Indicator 로 호 종류(broadcast)를 표시한다 | TS 24.380 §6.2.4.3.5 1.b |
| R9 | **호 종료**: 개시자가 발언을 놓은 뒤(`U: pending Release`) Floor Idle 을 받으면, 호가 일제 통화로 개시됐으므로 개시 단말은 미디어 송출 완료를 알리고 `Releasing` 으로 간다(= 호 해제, BYE) | TS 24.380 §6.2.4.6.4 6., TS 24.379 §4.12 |
| R10 | 서버 세션 해제 정책(그룹 호 공통): ① T4(Inactivity) 만료 ② 참가자 1명 이하 ③ 최소 affiliation 인원 미달 ④ TNG3(그룹 호 최대 시간) 만료 → controlling function 이 세션 해제. T4 는 floor 가 `G: Floor Idle` 에 들어가면 시작하고, 만료 시 호를 해제할지 T4 를 다시 걸지는 서비스 사업자 정책이다(§6.3.4.3.5). **T4 값은 호 종류별로 한 곳에서 온다** — 그룹 호(일제 통화 포함) = 그룹 문서 `<on-network-hang-timer>`, 개인 호 = service config `<private-call>`/`<hang-time>`, adhoc 그룹 호 = `<adhoc-group-call>`/`<hang-time>`. 일제 통화 전용 값은 없다(일제 전용 타이머 TFB1~TFB3 은 off-network 한정) | TS 24.379 §6.3.8.1, TS 24.380 §6.3.4.3.5 · Table 11.1.3-1(T4 기본 30초), TS 24.481 §7.2.2 o), TS 24.484 §8.4.2.7 3)·49) (`hang-time`) |
| R11 | 일제 통화로 개시된 호의 conference 이벤트 구독은 **480** + `Warning: 105 subscription not allowed in a broadcast group call` | TS 24.379 §10.1.3.4.1 |
| R12 | 긴급·임박과 조합 가능 — Floor Indicator 는 비트 OR(B + D/E). 개시자만 발언한다는 규칙은 긴급에도 그대로다(R5) | TS 24.380 §8.2.3.15 |
| R13 | **애드혹(ad hoc) 일제 통화**도 있다 — 개시 단말이 참가자 목록을 주는 ad hoc 그룹 통화 INVITE 에 `<broadcast-ind>true`("*broadcast adhoc group call*"). 받는 사람 = 목록의 참가자(affiliation 무관). 진행 중인 호를 일제 통화로 바꾸는 절차는 편성·ad hoc 어디에도 없다 — 진행 중 호에 온 INVITE 는 합류다(§10.1.1.3.1.1 15), 긴급 격상 15)f) 와 달리 broadcast 단계 없음) | TS 24.379 §17.2.2.1.1 9)·§17.1, §10.1.1.3.1.1 14)·15) |
| R14 | **개시 INVITE 로 발언까지 — 암묵적 발언 요청**(일제 통화에 한정되지 않는 선택 기능). 단말은 floor SDP offer 에 `mc_implicit_request`(요청, §14.2.5)와 `mc_granted`(200 OK 로 승인 표시를 받을 수 있다는 **능력** — 요청이 아니다, §14.2.4·§12.1.2.2 NOTE 2)를 싣고 호 성립 때 'U: pending Request'(T101)에 든다. 서버는 새 호 개시면 암묵 요청으로 받고(chat 합류·진행 중 prearranged/ad hoc 합류 제외) 응답에 `mc_implicit_request` 를 싣는다(§14.3.5). 200 OK 로 승인하면 answer 에 `mc_granted`(temporary group 세션 제외 — §14.3.4), 승인해도 Floor Granted 메시지는 따로 보낸다(§6.3.4.4.2 1.) — 단말은 answer 의 `mc_granted` 또는 Floor Granted 로 'U: has permission'(§6.2.4.4.2). 이어지는 offer 에는 `mc_granted` 를 싣지 않는다(§14.5) | TS 24.380 §14.2.4·§14.2.5·§14.3.4·§14.3.5·§14.5, §6.2.4.2.2·§6.2.4.4.2, §6.3.4.2.2 3)·§6.3.4.4.2, TS 24.379 §6.4 |

## 2. CIMS 구현의 규격 대비 판정

| 요구 | CIMS 현재 동작 | 근거 | 판정 |
|---|---|---|---|
| R1 | CSP 가 개시 INVITE 의 `<broadcast-ind>` 로 세션 속성을 정한다(`CMcpttInfo::bBroadcast` → `ProcessGroupCall`). 단말은 `GroupCallOptions.broadcast`(SDK)·`joinGroupCall(broadcast=true)`(Android PTT)로 `prearranged` + `<broadcast-ind>true` 를 싣는다 | `csp/McpttInfo.h`, `csp/GroupCallService.cpp` `ProcessGroupCall` · `sdk/core/src/mcptt/mcptt_xml.cpp` · `android/ptt-client/.../McpttXml.kt` | ✅ |
| R2 | 멤버는 어느 편성(prearranged) 그룹에서나 일제 통화를 개시한다 — 그룹 종류는 `prearranged`/`chat` 뿐이다 | `sql/migrate_ptt_groups_broadcast_call.sql`, `csc/src/handlers/admin.py` | ✅ |
| R3 | fan-out mcptt-info = `session-type`(그룹 종류) + `<broadcast-ind>true` | `GroupCallService.cpp` `BuildGroupInfoXml` | ✅ |
| R4 | 그룹 문서의 그룹 종류 = `<on-network-invite-members>`. 단말(SDK `GroupDoc`·Android `CscModels`/`McpttXml`)은 이 요소로 판정하고 없는 옛 문서만 `<mcpttgi:session-type>` 으로 읽으며, SDK 가 쓰는 PUT 본문에는 session-type 을 싣지 않는다. 서버 문서도 `<mcpttgi:session-type>` 을 싣지 않고, XCAP PUT 은 invite-members 만 읽는다(없으면 그룹 종류 불변) | `csc/src/services/mcptt.py` · `sdk/core/src/csc/group_doc.cpp` | ✅ |
| R5 | CMP 가 개시자 외 Floor Request 를 Deny #5 — 긴급 tier 검사보다 먼저. 초기 발언권(SDP `mc_granted` → `grantInitialFloor`)도 같은 판정 — 개시자가 놓은 뒤 T4 사이에 비개시자가 `mc_granted` 로 재합류해도 발언권을 받지 않는다 | `cmp/PMcpttGroup.cpp` `handleFloorRequest`·`grantInitialFloor` | ✅ |
| R6 | Floor Indicator 0x4000·Taken Permission 0. 진행 중 합류한 멤버에게 보내는 Floor Taken(`addMember` 의 화자 통지)도 전원 통지와 같은 필드(`_takenFields` — Granted Party = MCPTT ID·Permission·Message Sequence Number·Indicator·화자 SSRC, 헤더 = 서버 SSRC)이고, 일제 통화·ambient 청취 leg 는 Permission 0 | `cmp/PMcpttGroup.cpp` `_indicatorFor`·`_takenFields`·`broadcastFloorStatus`·`addMember` | ✅ |
| R7 | 세션 속성(개시자·broadcast)은 개시 INVITE 에서 한 번 정한다(CSP 세션 캐시). CMP 도 세션 개시 ADD 에서만 반영. 개시 INVITE 는 캐시를 **선점**(`bPending`)으로 적고 개시자 leg 확립에서 확정한다 — 개시가 실패하면(세션 창·`AcceptCall` 실패·CMP 포트 부족) 선점째 지우고, 선점 중에 같은 그룹에 온 INVITE 는 합류로 처리해 캐시를 덮지 않는다(선점 시한 32 s = INVITE 트랜잭션 64*T1). 일제 통화 **진행 중** 판정(R11)은 확정된 세션만 본다 | `GroupCallService.cpp` `m_mapGroupSession`·`SettlePendingSession`·`IsBroadcastInProgress`, `cmp/PCmpServer.cpp` `processAddGroup` | ✅ |
| R8 | 개시 단말의 Floor Request = Floor Indicator B-bit(긴급 비트와 OR) — 개시자 표식은 세션을 연 쪽에만 둔다 | `sdk/core/src/floor/floor_participant.cpp` `setBroadcastInitiator` · Android `FloorClient.broadcastInitiator` | ✅ |
| R9 | 개시 단말이 Floor Release 를 보낸 뒤(U: pending Release — 손으로 놓음·Granted Duration 자체 종료·Revoke 응답) B-bit Floor Idle 을 받으면 호를 해제한다(BYE). 서버 T4 는 나머지 참가자를 거둔다. pending Release 중에 늦게 온 Floor Granted(짧은 탭)는 Ack 만 하고 상태를 유지하며(§6.2.4.6.8), Release 는 T100 으로 재전송한다(§6.2.4.6.2·§6.2.4.6.3) | SDK `Participant::Callbacks::onBroadcastEnd` → 엔진 hangup · Android `FloorEvent.Idle.broadcastEnd` → `PttController` hangup | ✅ SDK (Android 짧은 탭 처리는 §7) |
| R10 | T4 만료(그룹 `hang_timer_sec` → CMP `PTT_FLOOR_INACTIVITY`)·참가자 1명 이하·TNG3(`max_duration_sec`) 해제. 최소 affiliation 인원 미달은 미구현 | `GroupCallService.cpp` `OnFloorInactivity`·`OnCallTerminated`·`CheckSessionLimits` | ✅ (최소 affiliation 인원 제외) |
| R11 | 480 + Warning 105 — 판정 기준 = 확정된 세션의 broadcast 속성. 권한 재점검 스윕(`AuthzSweepConferenceSubscriptions`)·수락 직후 재검사는 인가만 판정한다(`bAuthzOnly`) — 일시 480 으로 기존 구독을 `rejected` 로 끊으면 단말이 재구독하지 않는다(RFC 6665 §4.1.3) | `GroupCallService.cpp` `CheckConferenceSubscribe` · `AuthzRevoke.cpp` · `CscfModule.cpp` | ✅ |
| R12 | Floor Indicator = tier 비트 OR broadcast 비트, 비개시자 긴급 요청도 Deny #5 | `cmp/PMcpttGroup.cpp` | ✅ |
| R13 | 편성 그룹 호만 일제 통화로 연다 — ad hoc 그룹의 `<broadcast-ind>` 는 무시(INFO 로그 "편성 그룹 호 아님"). 진행 중 세션에 온 `<broadcast-ind>` 는 합류로 처리(규격대로) | `GroupCallService.cpp` `IsOnDemandGroupCall`(`_isAdhoc` 제외)·`ProcessGroupCall` | 단말(SDK — ad hoc INVITE 에 broadcast-ind)·관제 앱(발신 [애드혹] 의 [일제 통화]) ✅ · CSP ❌(§7, 서버 과제 P2) |
| R14 | **단말**: SDK `GroupCallOptions.implicitFloorRequest` 가 offer 에 `mc_implicit_request;mc_granted` 를 싣고(개시 offer 만 — 이어지는 offer 는 뺀다) answer 를 판정한다 — `mc_granted` = 승인, `mc_implicit_request` 만 = Floor Granted 대기, 둘 다 없음 = 명시 Floor Request 로 잇는다(§4.4 U7). **서버**: CSP 는 offer 의 `mc_granted` 를 초기 발언 요청으로 읽어 `PTT_JOIN.granted` 로 넘기고(CMP `grantInitialFloor` → Floor Granted), `mc_implicit_request` 는 읽지 않으며 answer 에 `mc_implicit_request`·`mc_granted` 를 싣지 않는다 — `mc_granted` 는 능력 표시라 요청으로 읽으면 규격대로 싣는 단말이 요청 없이 발언권을 받는다. SDK 는 두 속성을 함께 실어 지금 CSP 에서도 암묵 요청이 동작한다 | `sdk/core/src/floor/floor_participant.cpp`·`engine.cpp`·`mcptt/mcptt_xml.cpp` · `csp/GroupCallService.cpp` `ParseMcpttFmtp` | 단말 ✅ · CSP ⚠ 편차(§7) |

**요약**: 서버(CSP·CMP·CSC)는 규격대로다 — 발언권 평면(초기 발언권·늦은 합류 Taken 포함), 호 단위 일제 표식, 개시자 고정(개시 실패·동시 개시 포함),
구독 480/105 와 권한 스윕의 분리, 해제 정책(최소 affiliation 인원 제외).
단말 코어도 일제 통화 발신·B-bit Floor Request·발언 종료 후 호 해제(R1·R8·R9)·그룹 종류 판정(R4 짝 U5)을 한다(§4.4). 관제 앱 동작(U6)은
Windows 데스크톱이 하고 Android 태블릿이 남아 있다.

## 3. 동작

### 3.1 모델

- **일제 여부는 호 속성**이다. 긴급·임박(`mcptt_emergency_modes.md` §1 "조건은 group_type 이 아니다")과
  같은 원칙 — 그룹 종류(`prearranged`/`chat`)와 직교하는 호 단위 표식이며, CSP 는 **세션을 개시하는
  INVITE 한 번**에서만 결정한다.
- 그룹 종류는 `prearranged`/`chat` 두 가지다. "전 구성원 일제 통화"는 전 구성원을 멤버로 둔 **일반
  prearranged 그룹**에서 관제사가 일제 통화로 개시하는 것이다. 같은 그룹을 일반 그룹 통화로도 쓸 수 있다.
- 개시자는 세션 수명 동안 바뀌지 않는다. 늦은 합류·재참여·청취 leg 은 참가자다.
- **개시 권한은 단말 요청을 따른다.** 그룹 멤버가 `<broadcast-ind>true` 로 개시하면 서버는 별도 인가 없이
  일제 통화로 연다(R2 — 규격에 개시 권한 절차가 없다). 비멤버는 일반 그룹 통화와 같이 403 이다. "관제사만
  일제 통화" 같은 운영 구분은 단말(관제 앱에만 일제 통화 동작을 둠)의 몫이며 서버 역할 capability 로 막지 않는다.

### 3.2 flow

```
관제사 UE(개시)            CSP (controlling)                         CMP
  │ INVITE sip:{group}      │                                          │
  │  mcptt-info:            │ [ProcessGroupCall — 새 세션]              │
  │   session-type=prearranged  멤버 확인(비멤버 403) · 코덱 · SRTP    │
  │   broadcast-ind=true    │ 세션 속성 확정: broadcast=1, initiator=관제사
  │ ───────────────────────►│ ── PTT_GROUP_ADD {broadcast:1, initiator_id, t4} ─►│ 세션 생성 시 1회 고정
  │ ◄── 200 OK ──────────── │                                          │
  │                         │ fan-out INVITE (affiliate+등록 멤버)       │
  │                         │  mcptt-info: session-type=prearranged, broadcast-ind=true
  │ Floor Request(B-bit) ──────────────────────────────────────────────►│ 개시자 → Granted(B-bit)
  │                         │                    멤버 ◄── Floor Taken(B-bit, Permission=0)
  │ RTP ───────────────────────────────────────────────────────────────►│ ── 멤버별 복제 ─►
  │                         │   (멤버가 Floor Request → Deny #5 — 긴급이어도 동일)
  │ Floor Release ─────────────────────────────────────────────────────►│
  │ ◄── Floor Idle(B-bit) ──────────────────────────────────────────────│ T4 시작
  │ [R9: 일제 통화 → Releasing]                                          │
  │ ── BYE ────────────────►│                                          │
  │                         │ ◄── PTT_FLOOR_INACTIVITY (T4 만료) ────── │
  │                         │ [§6.3.8.1 해제 정책] 세션 해제 → 멤버 BYE · PTT_GROUP_REMOVE
```

- 진행 중 세션에 들어오는 INVITE(늦은 합류·재참여)는 `broadcast-ind` 값과 무관하게 **참가자 합류**다 —
  세션 속성·개시자를 바꾸지 않는다. 일반 그룹 통화가 진행 중인 그룹에 `broadcast-ind=true` INVITE 가
  와도 진행 중 호에 합류할 뿐 일제 통화로 바뀌지 않는다(Release 18 에 상향 절차 없음).
- 세션 해제는 개시자 BYE 가 아니라 R10 정책이다. T4 는 **그룹별 설정값**(그룹 문서 `<on-network-hang-timer>`,
  콘솔 그룹 편집 — §4.3)이고, 일제 통화에 쓰는 그룹(예: 전 구성원 그룹)은 그 그룹의 T4 를 따로(짧게) 준다.
  같은 그룹 안에서 호 종류별로 다른 T4 는 두지 않는다(R10 — 그룹 호의 T4 출처는 하나). 방송용 그룹의 T4 가
  길면 그 사이 멤버가 그 그룹에서 PTT 를 눌러도 새 일반 통화가 아니라 진행 중 일제 통화에 수신 전용으로
  합류하므로(늦은 합류), 방송용 그룹은 짧게 둔다.

## 4. 구현 위치

### 4.1 CSP (PTT-AS / controlling MCPTT function)

| 기능 | 구현 |
|---|---|
| `<broadcast-ind>` 파싱 | `CMcpttInfo::bBroadcast`(`csp/McpttInfo.h`) → `ProcessGroupCall`. 편성 그룹 호(prearranged)만 — chat·즉석 세션의 `<broadcast-ind>` 는 무시한다 |
| 세션 속성(개시자·broadcast·T4·TNG3) | 개시 INVITE 에서 한 번 정하는 세션 캐시 `m_mapGroupSession`. CMP 로 가는 모든 `PTT_GROUP_ADD`(개시·녹취 경로·재수립)는 `CmpSessionOf` → `CmpGroupSession` 으로 세션 캐시 값을 싣는다 — 합류·청취 leg 은 바꾸지 못한다 |
| fan-out mcptt-info | `BuildGroupInfoXml` — `session-type` = 그룹 종류(`prearranged`/`chat`), 세션이 broadcast 면 `<broadcast-ind>true</broadcast-ind>` |
| conference 구독 480 / Warning 105 | `CheckConferenceSubscribe` — 판정 기준 = 세션 broadcast 속성 |
| 해제 정책(§6.3.8.1) | `OnFloorInactivity`(CMP `PTT_FLOOR_INACTIVITY` — T4)·`OnCallTerminated`(확립 참가자 1명 이하)·`CheckSessionLimits`(TNG3 = `max_duration_sec`) → `ReleaseGroupSession`. on-demand 세션만(chat 은 상시) |
| dialog 이벤트 `<mcptt>` 확장·녹취 디스크립터(`group.json`) | `broadcast` = 세션 속성(`<mcptt … broadcast="true">`, `"broadcast": true`) |
| 그룹 모델 | `CspPttGroup::_groupType` = `prearranged`/`chat`(+ 내부 `private`/`adhoc`), `_hangTimerSec`·`_maxDurationSec`(`DbManager` 적재) |

### 4.2 CMP

| 기능 | 구현 |
|---|---|
| 세션 속성 | `broadcast`·`initiator_id` 는 세션 개시 ADD(새 그룹, 또는 남은 그룹에 다른 `sesid`)에서만 `PMcpttGroup::setBroadcastSession` — 같은 세션의 재ADD 는 바꾸지 않는다([cmp.md](../modules/cmp.md) PTT_GROUP_ADD 동작 7) |
| T4(Inactivity) | `G: Floor Idle` 진입 시 시작·발언권 요청 시 정지, 만료 시 CSP 로 `PTT_FLOOR_INACTIVITY` — 값 `floor_timers.t4_inactivity`(CSP 가 그룹 `hang_timer_sec` 로 채움) |
| 발언권(R5·R6·R12) | `handleFloorRequest`(개시자 외 Deny #5 — 긴급 tier 검사보다 먼저)·`_indicatorFor`(B-bit)·`broadcastFloorStatus`(Permission 0) — 판정 입력 = 세션 broadcast 플래그 |

### 4.3 CSC · DB · 콘솔

| 기능 | 구현 |
|---|---|
| 그룹 문서 | 그룹 종류 = `<on-network-invite-members>`(prearranged=true, chat=false), `<on-network-hang-timer>`(xs:duration — 그룹 호의 T4)·`<on-network-maximum-duration>`(TNG3). `<session-type>` 요소는 싣지 않고, XCAP PUT 은 invite-members 로 그룹 종류를 읽는다(없으면 불변) — `csc/src/services/mcptt.py` |
| DB | `ptt_groups.group_type` ENUM(`prearranged`,`chat`), `hang_timer_sec`·`max_duration_sec` — `sql/migrate_ptt_groups_broadcast_call.sql`(기존 broadcast 그룹 → `prearranged`)·`sql/cims_schema.sql` |
| 관리 API | `group_type` 허용값 `prearranged`/`chat`, `hang_timer_sec`·`max_duration_sec` — `csc/src/handlers/admin.py`([admin_api.md](../../api/admin_api.md)) |
| 콘솔 | PTT 그룹 편집 — 유형 `prearranged`/`chat`, 유지 시간(T4)·최대 통화 시간 필드, floor 이력 `INACTIVITY` |

### 4.4 단말 (libcimsue SDK · Android · 관제 앱)

| # | 항목 | 대상 | 상태 |
|---|---|---|---|
| U1 | 일제 통화 발신 — 그룹 INVITE mcptt-info 에 `session-type=prearranged` + `<broadcast-ind>true`. SDK `GroupCallOptions.broadcast`(C API `cimsue_group_call_options_t.broadcast` · Kotlin `GroupCallOptions(broadcast)` · .NET `GroupCallOptions.Broadcast`) · `cimsue-cli group-call --broadcast`/drive `group_call <g> broadcast` · Android `PttController.joinGroupCall(broadcast=true)`. `SessionType.BROADCAST` 는 없다. 착신 mcptt-info 의 `<broadcast-ind>` 는 `McpttInfo.broadcast` 로 올린다 | `sdk/core/src/mcptt/mcptt_xml.cpp`·`engine.cpp`·`cli/main.cpp`, `android/ptt-client/.../mcptt/McpttXml.kt`·`PttController.kt` | ✅ |
| U2 | 개시 단말 Floor Request 에 Floor Indicator B-bit (R8) | `sdk/core/src/floor/floor_participant.cpp`, Android `floor/FloorClient.kt` | ✅ |
| U3 | 개시 단말: Floor Release 뒤 B-bit Floor Idle → 호 해제(BYE) (R9) — 채널은 남긴다(Android 는 `leaveGroup` 이 아니라 hangup) | 같은 파일 · 엔진 `onBroadcastEnd` · `PttController` | ✅ |
| U4 | 수신 단말: B-bit → "일제 통화" 표시, Permission 0 → PTT 비활성. SDK 는 `FloorEvent.indicator`·`permission`·`FloorInfo.canRequest`·`McpttInfo.broadcast` 로 앱에 준다(표시는 앱 몫) | Android `ui/MainChannelScreen.kt`(발언 줄 "일제 통화 ·")·`PttController` | ✅ |
| U5 | 그룹 종류 판정을 `<on-network-invite-members>` 로 (G1 짝) — 없는 옛 문서만 session-type 폴백 | `sdk/core/src/csc/group_doc.cpp`, Android `csc/CscModels.kt`·`mcptt/McpttXml.kt` | ✅ |
| U7 | 암묵적 발언 요청(R14) — `GroupCallOptions.implicitFloorRequest`(C API `implicit_floor_request` · .NET `ImplicitFloorRequest` · Kotlin `implicitFloorRequest`) · `cimsue-cli group-call --implicit`/drive `group_call <g> implicit`. floor 는 개시 전부터 `Requesting`('U: pending Request'), 호 성립 전·승인 전에 놓으면 answer 에서 Floor Release(그 사이 온 Floor Granted 는 §6.2.4.6.8 과 같이 무시), 일제 통화 개시자면 이어 오는 B-bit Floor Idle 로 호를 해제한다(U3) | `sdk/core/src/floor/floor_participant.cpp`(`armImplicitRequest`·`onInitialAnswer`)·`engine.cpp`(`startMcptt`·`onCallTsxState`)·`mcptt/mcptt_xml.cpp`(`floorSdp`·`parseFloorFmtp`), 시험 `floor_participant_test`·`csc_test` `FloorSdp.*` | ✅ SDK · Android 앱 미사용 |
| U6 | 관제 앱 — "일제 통화" 동작(선택한 그룹에 U1 로 발신)과 PTT 그룹 편집의 유형 선택지 정리(`broadcast` 제거 — 서버는 이 유형을 받지 않는다). Windows: ① 포커스 카드 3줄 [일제 통화] — 멤버 편성 그룹에 진행 중 세션이 없을 때만(있으면 서버가 합류로만 받는다 §3.2, chat 은 broadcast-ind 무시라 제외 — TS 24.379 §6.2.8.2 는 broadcast-ind 를 prearranged 그룹 호에 싣는다. 그룹 종류는 관리 목록 `sessionType`, 모르면(관리 범위 없음) 개시 때 GMS 그룹 문서의 `on-network-invite-members` 로 확인) → **한 버튼** — 누르는 동안 `JoinGroupCall(Broadcast, ImplicitFloorRequest)`(U7)로 개시하고 말하며, 놓으면 호 성립 전 CANCEL / 성립 뒤 Floor Release(→ U3 호 해제), 잠금 발언이면 누를 때마다 켜고 끔 · 개시 카드 자동 포커스·단일 발언 대상. 일제 통화 판정 = 착신 mcptt-info broadcast-ind 또는 floor B-bit(.NET `FloorIndicator.BroadcastGroup` — 늦게 합류한 leg 은 B-bit 로만 안다, 코어는 Deny·Revoke 의 Floor Indicator 도 상태에 담는다), 수신 멤버(Permission 0)는 발언 대상 체크 불가, 서버가 일반 통화로 연 개시(첫 서버 floor 메시지의 Floor Indicator 에 B-bit 없음)는 경고 — 화면 규약 [dispatch_desktop_ui.md](dispatch_desktop_ui.md) §4 | `windows/dispatch-desktop`(`DispatchSession.BroadcastCallAsync`·`ReleaseBroadcast`·`SessionItem.IsBroadcast`·`ChannelCard.CanBroadcast`·`PttChannelsViewModel.BroadcastDown/Up`·`GroupEditViewModel.SessionTypes`), `android/dispatch-tablet`(`PttPlane.startBroadcast`·`releaseBroadcast`·`SessionItem.isBroadcast`·`ChannelCard.canBroadcast`·`PttChannelsViewModel.broadcast*`·`ChannelScreen.BroadcastHoldButton`·`PttGroupsViewModel.SESSION_TYPES`) — 태블릿은 채널 머리의 [일제 통화](android_dispatch_tablet.md §6.3a) | Windows ✅ · 태블릿 코드 반영(Android 빌드·실기 미확인) |
| U8 | 애드혹 일제 통화(R13) — 관제 앱 발신 [애드혹] 의 [일제 통화](고른 사람들에게, U6 과 같은 한 버튼 — 누르는 동안 팝오버·시트를 닫지 않는다). SDK 는 ad hoc INVITE(resource-lists)에 `<broadcast-ind>` 를 싣는다(`GroupCallOptions{members, broadcast, implicitFloorRequest}`). CSP 가 받기 전에는 일반 애드혹 그룹 통화로 열리고 앱이 «일제 통화로 열리지 않았습니다» 를 알린다 | `windows/dispatch-desktop`(`DispatchSession.StartAdhocBroadcast`·`PttOriginateViewModel.BroadcastDown/Up`), `android/dispatch-tablet`(`PttPlane.startAdhocBroadcast`·`OriginateSheet`) | Windows ✅ · 태블릿 코드 반영(빌드 미확인) · 서버 ❌ |

### 4.5 CSP↔CMP 계약 ([cmp_media_api.md](../../api/cmp_media_api.md))

| 명령 | 필드 |
|---|---|
| `PTT_GROUP_ADD` | `group_type` = `prearranged`/`chat`/`private`, `broadcast`(0/1)·`initiator_id` — 세션 개시 ADD 에서만 유효. 전환기(한 릴리스): 구 CSP 의 `group_type:"broadcast"` 는 `broadcast:1` 로 해석(WARN) |
| `PTT_GROUP_ADD`/`MODIFY` `floor_timers` | `t4_inactivity`(초, 0 = 미사용) |
| 이벤트(CMP→CSP) | `PTT_FLOOR_INACTIVITY {group_id, sesid}` — T4 만료 |

## 5. 결정 사항

- **개시 권한**: 서버 인가 없음 — 멤버 단말의 `<broadcast-ind>` 요청을 따른다(§3.1).
- **T4**: 그룹별 설정(`<on-network-hang-timer>` = `hang_timer_sec`). 일제 통화에 쓰는 그룹은 그 그룹의 T4 를 따로 설정한다(§3.2).
- **일제 통화용 그룹 알림 = 그룹 이름**: 일제 통화에 쓸 수신 집합은 편성(prearranged) 그룹으로 만들고(개시할 관제사도 멤버, 수신자는 그 그룹에 affiliate —
  §6.3.5.5), 멤버 단말에는 그룹 이름(`<display-name>`, 예: "전원 일제")으로 알린다. 그룹 문서(TS 24.481 V18.3.0)에는 일제 통화 표시 요소가 없고
  (`<anyExt>` 는 3GPP 다음 판 자리), group-broadcast/user-broadcast 그룹은 TS 24.379 §4.12 가 현 릴리스에서 따르지 않는다. 통화 단위 표시(착신
  `broadcast-ind`·floor B-bit·Taken Permission 0)는 규격대로 단말이 한다.
- **관제 앱 [일제 통화] = 해당 그룹 카드의 한 버튼**(U6): 진행 중인 통화가 없는 멤버 편성 그룹에서만 활성, 누르는 동안 개시·발언.

## 6. 검증

S3 `S3-SCN-PTT-BROADCAST`(`verify/lib/items/stage3/scn_ptt_broadcast.py` — cspsim `-broadcast` 경로 + 계측기 경로)·계측기
`PTT-GROUP-CALL-BROADCAST`(`ems/tester/oam/scenarios/ptt/group_call_broadcast.yaml` — `group_call` `payload: broadcast`·`check conference_warning_105`).

| id | 시나리오 | 기대 |
|---|---|---|
| BC1 | 편성 그룹에서 `broadcast-ind=true` 개시 | fan-out mcptt-info `prearranged`+`broadcast-ind`, 멤버 Floor Taken B-bit·Permission 0 |
| BC2 | 비개시자 Floor Request (일반·긴급) | Deny #5 |
| BC3 | 진행 중 일제 통화에 다른 멤버가 INVITE 로 합류 후 개시자 재발언 | 개시자 Granted, 합류자 Deny #5 (R7) |
| BC4 | 개시자 발언 종료 | 개시 단말 BYE, T4 만료 후 세션 해제·멤버 BYE |
| BC5 | 일제 통화 중 conference SUBSCRIBE | 480 + Warning 105 |
| BC6 | 같은 그룹에서 `broadcast-ind` 없이 개시 | 일반 그룹 통화(전원 발언 요청 가능) |

## 7. 남은 과제

- **관제 앱 Android 태블릿(U6·U8) 빌드·실기** — Windows 관제 앱 안정화 이후 진행한다(사용자 결정). 채널 머리 [일제 통화]·발신 [애드혹] 의 [일제 통화]·수신 표시·유형 선택지 `broadcast` 제거를 코드로
  반영했다. 이 Windows 개발 PC 는 Android 를 빌드하지 못해 컴파일·JVM 시험(`PttChannelTest` 일제 통화 3건)·실기는 Android 빌드 환경 몫이다.
- **Android 코어의 짧은 탭 처리** — `FloorClient` 에 §6.2.4.6.8(pending Release 중 Granted 무시)·§6.2.4.6.2(T100 재전송)를 반영했다. Android 빌드·실기 확인은 Android 빌드 환경에서 한다.
- **Android PTT 앱 수신 멤버의 일제 통화 판정** — `android/ptt-client` 는 착신 mcptt-info `broadcast-ind` 를 파싱하지 않고 Permission 이 온 Taken 으로만 PTT 를 막는다(서버는 늦은 합류 Taken 에도 Permission 0 을 싣는다 — R6). 관제 태블릿은 SDK `McpttInfo.broadcast`·B-bit 로 판정한다.
- **CSP 암묵적 발언 요청(R14)** — offer `mc_implicit_request` 를 `PTT_JOIN.granted` 로(새 호 개시만 — chat 합류·진행 중 합류 제외, §14.3.5), 받아들이면
  answer 에 `mc_implicit_request`, 200 OK 로 승인하면 `mc_granted`(offer 에 있을 때만 — §14.3.1, temporary group 제외). offer `mc_granted` 만으로는
  발언권을 주지 않는다. 사내 시험 도구 중 offer `mc_granted` 로 초기 발언권을 기대하는 것(`scripts/mcptt_floor_policy_probe.py` [14])은 같은 변경에서
  `mc_implicit_request` 로 옮긴다. 서버 몫(.48) — [docs/dev/server_todo_mcptt_floor_broadcast.md](../../dev/server_todo_mcptt_floor_broadcast.md) P1.
- **CSP 애드혹(ad hoc) 일제 통화(R13)** — ad hoc 그룹 호에서도 `<broadcast-ind>` 로 세션 속성을 정하고(개시자 고정·Deny #5·B-bit 는 편성
  그룹과 같다), 구독 480/105 검사를 ad hoc 가지 앞으로, 일제 세션은 개시자 이탈 시 해제(TS 24.379 §6.3.8.1 3) 로컬 정책). 개시 권한은 추가하지
  않는다(멤버·참가자 누구나). 서버 몫(.48) — [docs/dev/server_todo_mcptt_floor_broadcast.md](../../dev/server_todo_mcptt_floor_broadcast.md) P2. 참고: ad hoc 그룹 ID 는
  규격상 서버가 준다(§17.1 — CIMS 는 단말이 `adhoc-<번호>-<epoch>` 를 만든다, [mcptt_emergency_modes.md](mcptt_emergency_modes.md) §6).
- **최소 affiliation 인원 미달 해제**(R10 ③, TS 24.379 §6.3.8.1 4)) — 그룹 문서 `<on-network-minimum-number-of-affiliated-members>` 와 함께.
- **전환기 종료** — CMP 의 `group_type:"broadcast"` 해석(§4.5)은 모든 사이트의 CSP 가 `broadcast` 필드를 싣는 판으로 올라간 뒤 제거한다.
  단말(SDK `GroupDoc`·Android `CscModels`)의 옛 문서 `<mcpttgi:session-type>` 폴백은 옛 서버와의 호환용이다.
