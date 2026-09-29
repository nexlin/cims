# MCPTT 일제 통화 (Broadcast Group Call) — 규격 정합 설계

> 일제 통화 = **개시자 한 명만 말하고 나머지는 듣기만 하며, 개시자의 발언이 끝나면 통화도 끝나는
> 그룹 통화**(3GPP TS 24.379 §4.12). 이 문서는 규격 요구, CIMS 구현의 규격 대비 판정, 컴포넌트별
> 구현 위치(CSP·CMP·CSC·단말)의 정본이다. 발언권 절차의 일반은
> [ptt_flows.md](ptt_flows.md) C1~C3, 긴급·임박 조건은 [mcptt_emergency_modes.md](mcptt_emergency_modes.md) 를 본다.
>
> 근거 규격 판본: TS 24.379 V18.13.0 · TS 24.380 V18.7.0 · TS 24.481 V18.3.0 (Release 18).
>
> **구현 상태** — 서버(CSP·CMP·CSC·DB·콘솔)·검증(cspsim·계측기·S3)·단말 코어(SDK·Android PTT — §4.4 U1~U5)는 반영됐다(§2).
> 남은 것은 관제 앱(U6 — Windows 데스크톱·Android 태블릿)·최소 affiliation 인원 해제(R10 ③)다(§7).

---

## 1. 규격 요구 (Release 18, on-network)

| # | 요구 | 근거 |
|---|---|---|
| R1 | 일제 통화는 **통화(호) 단위 표식**이다. 개시 단말이 그룹 INVITE 의 `application/vnd.3gpp.mcptt-info+xml` 에 `<broadcast-ind>true</broadcast-ind>` 를 싣는다(pre-established session 이면 Refer-To 의 hname "body"). 없으면 일반 그룹 통화다 | TS 24.379 §6.2.8.2, Annex F.1(`broadcast-ind` 의미 11) |
| R2 | 그룹 종류가 아니다 — 사용자는 **자신이 멤버인 어느 MCPTT 그룹에서나** 일제 통화를 개시할 수 있다. user-broadcast group·group-broadcast group(그룹의 그룹)은 Release 18 절차에 없다(인가 설정 요소는 있으나 어떤 절차도 쓰지 않음) | TS 24.379 §4.12 NOTE 1·2 |
| R3 | `<session-type>` 값은 `chat`·`prearranged`·`private`·`first-to-answer`·`ambient-listening`·`adhoc` 뿐이다 — 일제 통화는 `prearranged` + `<broadcast-ind>` 다 | TS 24.379 Annex F.1 (`session-type` 의미 2) |
| R4 | 그룹 문서의 그룹 종류는 `<on-network-invite-members>`(true = 서버가 멤버를 초대하는 prearranged, false = chat)로 표현한다. 그룹 문서에 `session-type` 요소는 없다 | TS 24.481 §7.2.2 a)·§7.2.8, TS 24.379 §6.3.4.1.4 4)a) (`session-type` 을 그룹 종류로 결정) |
| R5 | 발언권: 개시자의 Floor Request 만 중재 로직으로 넘기고, 다른 참가자의 요청은 **호 상태와 무관하게** Floor Deny cause #5(Receive only) | TS 24.380 §6.3.5.3.4, §6.3.5.4.4 |
| R6 | 서버가 보내는 Floor Granted/Taken/Idle/Deny/Revoke 등에 Floor Indicator **B-bit(0x4000, Broadcast group call)**, Floor Taken·Floor Idle 의 **Permission to Request the Floor = 0** | TS 24.380 §6.3.4.4.2, §6.3.5.x, §8.2.3.15 |
| R7 | 개시자 = 일제 통화를 **개시한** 사용자. 진행 중인 호에 나중에 합류한 참가자는 개시자가 아니다 | TS 24.380 §6.3.5.3.4 ("the initiator of the broadcast group call") |
| R8 | 개시 단말의 Floor Request 는 Floor Indicator 로 호 종류(broadcast)를 표시한다 | TS 24.380 §6.2.4.3.5 1.b |
| R9 | **호 종료**: 개시자가 발언을 놓은 뒤(`U: pending Release`) Floor Idle 을 받으면, 호가 일제 통화로 개시됐으므로 개시 단말은 미디어 송출 완료를 알리고 `Releasing` 으로 간다(= 호 해제, BYE) | TS 24.380 §6.2.4.6.4 6., TS 24.379 §4.12 |
| R10 | 서버 세션 해제 정책(그룹 호 공통): ① T4(Inactivity) 만료 ② 참가자 1명 이하 ③ 최소 affiliation 인원 미달 ④ TNG3(그룹 호 최대 시간) 만료 → controlling function 이 세션 해제. T4 는 floor 가 `G: Floor Idle` 에 들어가면 시작하고, 만료 시 호를 해제할지 T4 를 다시 걸지는 서비스 사업자 정책이다(§6.3.4.3.5). **T4 값은 호 종류별로 한 곳에서 온다** — 그룹 호(일제 통화 포함) = 그룹 문서 `<on-network-hang-timer>`, 개인 호 = service config `<private-call>`/`<hang-time>`, adhoc 그룹 호 = `<adhoc-group-call>`/`<hang-time>`. 일제 통화 전용 값은 없다(일제 전용 타이머 TFB1~TFB3 은 off-network 한정) | TS 24.379 §6.3.8.1, TS 24.380 §6.3.4.3.5 · Table 11.1.3-1(T4 기본 30초), TS 24.481 §7.2.2 o), TS 24.484 §8.4.2.7 3)·49) (`hang-time`) |
| R11 | 일제 통화로 개시된 호의 conference 이벤트 구독은 **480** + `Warning: 105 subscription not allowed in a broadcast group call` | TS 24.379 §10.1.3.4.1 |
| R12 | 긴급·임박과 조합 가능 — Floor Indicator 는 비트 OR(B + D/E). 개시자만 발언한다는 규칙은 긴급에도 그대로다(R5) | TS 24.380 §8.2.3.15 |

## 2. CIMS 구현의 규격 대비 판정

| 요구 | CIMS 현재 동작 | 근거 | 판정 |
|---|---|---|---|
| R1 | CSP 가 개시 INVITE 의 `<broadcast-ind>` 로 세션 속성을 정한다(`CMcpttInfo::bBroadcast` → `ProcessGroupCall`). 단말은 `GroupCallOptions.broadcast`(SDK)·`joinGroupCall(broadcast=true)`(Android PTT)로 `prearranged` + `<broadcast-ind>true` 를 싣는다 | `csp/McpttInfo.h`, `csp/GroupCallService.cpp` `ProcessGroupCall` · `sdk/core/src/mcptt/mcptt_xml.cpp` · `android/ptt-client/.../McpttXml.kt` | ✅ |
| R2 | 멤버는 어느 편성(prearranged) 그룹에서나 일제 통화를 개시한다 — 그룹 종류는 `prearranged`/`chat` 뿐이다 | `sql/migrate_ptt_groups_broadcast_call.sql`, `csc/src/handlers/admin.py` | ✅ |
| R3 | fan-out mcptt-info = `session-type`(그룹 종류) + `<broadcast-ind>true` | `GroupCallService.cpp` `BuildGroupInfoXml` | ✅ |
| R4 | 그룹 문서의 그룹 종류 = `<on-network-invite-members>`. 단말(SDK `GroupDoc`·Android `CscModels`/`McpttXml`)은 이 요소로 판정하고 없는 옛 문서만 `<mcpttgi:session-type>` 으로 읽으며, SDK 가 쓰는 PUT 본문에는 session-type 을 싣지 않는다. 서버 문서도 `<mcpttgi:session-type>` 을 싣지 않고, XCAP PUT 은 invite-members 만 읽는다(없으면 그룹 종류 불변) | `csc/src/services/mcptt.py` · `sdk/core/src/csc/group_doc.cpp` | ✅ |
| R5 | CMP 가 개시자 외 Floor Request 를 Deny #5 — 긴급 tier 검사보다 먼저 | `cmp/PMcpttGroup.cpp` `handleFloorRequest` | ✅ |
| R6 | Floor Indicator 0x4000·Permission 0 | `cmp/PMcpttGroup.cpp` `_indicatorFor`·`broadcastFloorStatus` | ✅ |
| R7 | 세션 속성(개시자·broadcast)은 개시 INVITE 에서 한 번 정한다(CSP 세션 캐시). CMP 도 세션 개시 ADD 에서만 반영 | `GroupCallService.cpp` `m_mapGroupSession`, `cmp/PCmpServer.cpp` `processAddGroup` | ✅ |
| R8 | 개시 단말의 Floor Request = Floor Indicator B-bit(긴급 비트와 OR) — 개시자 표식은 세션을 연 쪽에만 둔다 | `sdk/core/src/floor/floor_participant.cpp` `setBroadcastInitiator` · Android `FloorClient.broadcastInitiator` | ✅ |
| R9 | 개시 단말이 Floor Release 를 보낸 뒤(U: pending Release — 손으로 놓음·Granted Duration 자체 종료·Revoke 응답) B-bit Floor Idle 을 받으면 호를 해제한다(BYE). 서버 T4 는 나머지 참가자를 거둔다 | SDK `Participant::Callbacks::onBroadcastEnd` → 엔진 hangup · Android `FloorEvent.Idle.broadcastEnd` → `PttController` hangup | ✅ |
| R10 | T4 만료(그룹 `hang_timer_sec` → CMP `PTT_FLOOR_INACTIVITY`)·참가자 1명 이하·TNG3(`max_duration_sec`) 해제. 최소 affiliation 인원 미달은 미구현 | `GroupCallService.cpp` `OnFloorInactivity`·`OnCallTerminated`·`CheckSessionLimits` | ✅ (최소 affiliation 인원 제외) |
| R11 | 480 + Warning 105 — 판정 기준 = 세션 broadcast 속성 | `GroupCallService.cpp` `CheckConferenceSubscribe` | ✅ |
| R12 | Floor Indicator = tier 비트 OR broadcast 비트, 비개시자 긴급 요청도 Deny #5 | `cmp/PMcpttGroup.cpp` | ✅ |

**요약**: 서버(CSP·CMP·CSC)는 규격대로다 — 발언권 평면, 호 단위 일제 표식, 개시자 고정, 해제 정책(최소 affiliation 인원 제외).
단말 코어도 일제 통화 발신·B-bit Floor Request·발언 종료 후 호 해제(R1·R8·R9)·그룹 종류 판정(R4 짝 U5)을 한다(§4.4). 관제 앱 동작(U6)이 남아 있다.

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
| U6 | 관제 앱 — "일제 통화" 동작(선택한 그룹에 U1 로 발신)과 PTT 그룹 편집의 유형 선택지 정리(`broadcast` 제거 — 서버는 이 유형을 받지 않는다) | `windows/dispatch-desktop`(`GroupEditViewModel.SessionTypes`·`GroupAdminViewModel.SessionTypeText`), `android/dispatch-tablet`(`PttPlane.joinGroupCall`·`PttGroupsViewModel.SESSION_TYPES`) | 미구현 |

### 4.5 CSP↔CMP 계약 ([cmp_media_api.md](../../api/cmp_media_api.md))

| 명령 | 필드 |
|---|---|
| `PTT_GROUP_ADD` | `group_type` = `prearranged`/`chat`/`private`, `broadcast`(0/1)·`initiator_id` — 세션 개시 ADD 에서만 유효. 전환기(한 릴리스): 구 CSP 의 `group_type:"broadcast"` 는 `broadcast:1` 로 해석(WARN) |
| `PTT_GROUP_ADD`/`MODIFY` `floor_timers` | `t4_inactivity`(초, 0 = 미사용) |
| 이벤트(CMP→CSP) | `PTT_FLOOR_INACTIVITY {group_id, sesid}` — T4 만료 |

## 5. 결정 사항

- **개시 권한**: 서버 인가 없음 — 멤버 단말의 `<broadcast-ind>` 요청을 따른다(§3.1).
- **T4**: 그룹별 설정(`<on-network-hang-timer>` = `hang_timer_sec`). 일제 통화에 쓰는 그룹은 그 그룹의 T4 를 따로 설정한다(§3.2).

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

- **관제 앱(U6)** — Windows 데스크톱·Android 태블릿 모두 일제 통화 동작이 없고, PTT 그룹 편집 유형 선택지에 `broadcast` 가 남아 있다
  (선택해도 서버는 invite-members 로만 그룹 종류를 읽으므로 편성 그룹이 된다). 두 앱은 Windows 개발 환경에서 빌드한다.
- **최소 affiliation 인원 미달 해제**(R10 ③, TS 24.379 §6.3.8.1 4)) — 그룹 문서 `<on-network-minimum-number-of-affiliated-members>` 와 함께.
- **전환기 종료** — CMP 의 `group_type:"broadcast"` 해석(§4.5)은 모든 사이트의 CSP 가 `broadcast` 필드를 싣는 판으로 올라간 뒤 제거한다.
  단말(SDK `GroupDoc`·Android `CscModels`)의 옛 문서 `<mcpttgi:session-type>` 폴백은 옛 서버와의 호환용이다.
