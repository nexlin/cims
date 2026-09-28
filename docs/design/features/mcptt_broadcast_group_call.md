# MCPTT 일제 통화 (Broadcast Group Call) — 규격 정합 설계

> 일제 통화 = **개시자 한 명만 말하고 나머지는 듣기만 하며, 개시자의 발언이 끝나면 통화도 끝나는
> 그룹 통화**(3GPP TS 24.379 §4.12). 이 문서는 규격 요구, CIMS 구현의 규격 대비 판정, 규격에 맞추기
> 위한 컴포넌트별 보완 항목(CSP·CMP·CSC·단말)의 정본이다. 발언권 절차의 일반은
> [ptt_flows.md](ptt_flows.md) C1~C3, 긴급·임박 조건은 [mcptt_emergency_modes.md](mcptt_emergency_modes.md) 를 본다.
>
> 근거 규격 판본: TS 24.379 V18.13.0 · TS 24.380 V18.7.0 · TS 24.481 V18.3.0 (Release 18).

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
| R1 | 일제 여부를 **그룹 속성** `ptt_groups.group_type='broadcast'` 로 정한다. CSP·SDK·Android 어디에도 `<broadcast-ind>` 파싱·송신이 없다 | `csp/McpttInfo.h:15`, `sql/cims_schema.sql:183`, `sdk/core/src/mcptt/mcptt_xml.cpp:29` | ✗ |
| R2 | broadcast 유형 그룹에서만 일제 통화가 되고, 그 그룹은 일반 통화를 할 수 없다 | `csp/GroupCallService.cpp:436` (`clsGroup._groupType` 전달) | ✗ |
| R3 | fan-out INVITE 의 mcptt-info 에 `session-type=broadcast`(규격 밖 값)를 싣는다 | `csp/GroupCallService.cpp:2648-2655` | ✗ |
| R4 | GMS 그룹 문서에 규격에 없는 `<mcpttgi:session-type>` 을 3GPP 네임스페이스로 싣고, `<on-network-invite-members>` 는 chat 그룹에도 항상 true | `csc/src/services/mcptt.py:1282`, `:1294` | ✗ |
| R5 | CMP 가 개시자 외 Floor Request 를 Deny #5 — 긴급 tier 검사보다 먼저 | `cmp/PMcpttGroup.cpp:860-873` | ✅ |
| R6 | Floor Indicator 0x4000·Permission 0 | `cmp/PMcpttGroup.cpp:1025-1035`, `:1798-1825` | ✅ |
| R7 | CMP 는 broadcast·개시자를 세션 개시 ADD 에서만 받는다(같은 세션 재ADD 는 무시). **CSP 는 진행 중 세션에 합류하는 INVITE 의 합류자를 `initiator_id` 로 PTT_GROUP_ADD 재전송(녹취 경로 설정 시)하고, 캐시 `strCallerId` 도 합류자·청취자로 바꾼다** | `csp/GroupCallService.cpp:444-461`, `cmp/PCmpServer.cpp` `processAddGroup` | △ (CSP 결함 — P2·P3) |
| R8 | SDK 의 Floor Request 에 broadcast 비트 없음 | `sdk/core/src/floor/floor_participant.cpp` | ✗ (단말) |
| R9 | 개시 단말의 발언 종료 후 호 해제 처리 없음 — 세션이 멤버가 끊을 때까지 남고, 개시자가 다시 눌러 이어서 말할 수 있다 | SDK·Android 에 broadcast Floor Idle 처리 없음 | ✗ (단말) |
| R10 | CMP 는 T4 를 갖췄다(`floor_timers.t4_inactivity` → `PTT_FLOOR_INACTIVITY`). CSP 는 T4 를 싣지 않고 이벤트를 소비하지 않으며, on-demand 세션은 **확립 leg 이 0** 이 될 때만 해제(규격은 1명 이하). 그룹 문서의 `<mcpttgi:on-network-hang-time>3</...>`(요소명 불일치 — 규격은 `on-network-hang-timer`)은 고정값이며 어디서도 쓰지 않는다 | `csp/GroupCallService.cpp:996`, `:2063`, `csc/src/services/mcptt.py` 그룹 문서 | ✗ (CSP·CSC) |
| R11 | 480 + Warning 105 — 단 판정 기준이 호가 아니라 그룹 유형 | `csp/GroupCallService.cpp:2157-2160` | △ (R1 과 함께 기준 교체) |
| R12 | Floor Indicator = tier 비트 OR broadcast 비트, 비개시자 긴급 요청도 Deny #5 | `cmp/PMcpttGroup.cpp:862`, `:1025-1035` | ✅ |

**요약**: 발언권 평면(R5·R6·R12)은 규격대로다. 호를 무엇으로 일제 통화라고 부르는가(R1~R4),
개시자 고정(R7), 호 종료·세션 해제(R9·R10)가 규격과 다르다.

## 3. 규격 정합 동작 (목표)

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
  콘솔 그룹 편집 — §4.3 G2)이고, 일제 통화에 쓰는 그룹(예: 전 구성원 그룹)은 그 그룹의 T4 를 따로(짧게) 준다.
  같은 그룹 안에서 호 종류별로 다른 T4 는 두지 않는다(R10 — 그룹 호의 T4 출처는 하나). 방송용 그룹의 T4 가
  길면 그 사이 멤버가 그 그룹에서 PTT 를 눌러도 새 일반 통화가 아니라 진행 중 일제 통화에 수신 전용으로
  합류하므로(늦은 합류), 방송용 그룹은 짧게 둔다.

## 4. 보완 항목

### 4.1 CSP (PTT-AS / controlling MCPTT function)

| # | 항목 | 대상 |
|---|---|---|
| P1 | mcptt-info 파서에 `<broadcast-ind>` 추가, `session-type` 허용값을 규격 6종으로 | `csp/McpttInfo.h` |
| P2 | 세션 속성(`broadcast`, `initiator`)을 **새 세션일 때만** 확정 — `HasActiveLeg()` 가 거짓인 개시 INVITE. 합류·청취 leg 은 캐시 개시자(`strCallerId`)를 바꾸지 않는다 | `CGroupCallService::ProcessGroupCall` (`GroupCallService.cpp:404-461`) |
| P3 | 기존 세션 재ADD(녹취 경로·재수립)에는 `initiator_id`/broadcast 를 싣지 않는다 — 세션 속성의 권한자는 CSP 이고 CMP 는 생성 시 1회만 받는다(§4.2 M1) | `GroupCallService.cpp:444-453`, `InviteMember` `:1167-1179`, `CCmpClient::AddGroup` |
| P4 | fan-out mcptt-info: `session-type` 은 그룹 종류(`prearranged`/`chat`), 일제 통화면 `<broadcast-ind>true</broadcast-ind>` 추가 | `BuildGroupInfoXml` (`GroupCallService.cpp:2643`) |
| P5 | conference 구독 480/Warning 105 판정을 그룹 유형 → **세션 broadcast 속성**으로 | `GroupCallService.cpp:2157` |
| P6 | 세션 해제 정책 §6.3.8.1: CMP `PTT_FLOOR_INACTIVITY`(T4 만료) 수신 시 해제, 확립 참가자 **1명 이하**에서 해제(현재 0명). TNG3·최소 affiliation 인원은 같은 정책 훅에 둔다 | 세션 종료 판정 (`GroupCallService.cpp:996`, `:2063`) |
| P7 | dialog 이벤트 `<mcptt>` 확장(CIMS 네임스페이스)과 녹취 디스크립터(`group.json`)의 `broadcast` 를 그룹 유형이 아닌 세션 속성에서 채운다 | `GroupCallService.cpp:2414-2424`, `BuildGroupDescriptor` |
| P8 | `CspPttGroup::_groupType` 값 도메인을 `prearranged`/`chat`(+ 내부 `private`/`adhoc`)로 | `csp/CspPttGroup.h:99` |

### 4.2 CMP

| # | 항목 | 대상 |
|---|---|---|
| M1 | broadcast·개시자는 **그룹(세션) 생성 시 1회** 설정 — 기존 그룹에 대한 PTT_GROUP_ADD 는 이 값을 바꾸지 않는다. 필드는 `group_type` 에서 분리해 `broadcast:1` 로(긴급 tier 와 같은 호 속성 축) | `PCmpServer::processAddGroup` (`PCmpServer.cpp:1868`, `:1952-1956`), `PMcpttGroup::setBroadcast` |
| M2 | T4(Inactivity): `G: Floor Idle` 진입 시 시작, 발언권 요청 시 정지, 만료 시 CSP 로 `PTT_FLOOR_INACTIVITY` 이벤트. 값은 `floor_timers.t4_inactivity`(CSP 가 그룹 문서 hang-timer 로 채움) | `PMcpttGroup::tickFloorTimers`, `_advanceFloorOrIdle` |
| M3 | Deny #5 · Permission 0 · B-bit(R5·R6·R12)는 현행 유지 — 판정 입력만 세션 broadcast 플래그로 | `PMcpttGroup.cpp:860`, `:1025` |

### 4.3 CSC (GMS · DB · 콘솔)

| # | 항목 | 대상 |
|---|---|---|
| G1 | 그룹 문서에서 비표준 `<mcpttgi:session-type>` 제거, 그룹 종류는 `<on-network-invite-members>`(prearranged=true, chat=false)로 | `csc/src/services/mcptt.py:1265-1300` (생성), `:2278-2297` (XCAP PUT 파싱) |
| G2 | 규격 요소명으로 정정하고 **그룹별 설정값**으로 싣는다 — `<on-network-hang-timer>`(xs:duration, 예 `PT30S`, = 그 그룹 호의 T4)·`<on-network-maximum-duration>`(TNG3, invite-members=true 면 필수 — §7.2.7). 그룹별 값이 없으므로 `ptt_groups` 컬럼(hang timer·최대 시간)과 관리 API·콘솔 그룹 편집 필드를 추가한다. 현재는 `on-network-hang-time` 고정 3·`on-network-max-duration` 고정 3600 | 같은 파일, `sql/cims_schema.sql`, `csc/src/handlers/admin.py`, 콘솔 그룹 편집 |
| G3 | `ptt_groups.group_type` ENUM 에서 `broadcast` 제거 + 마이그레이션(기존 broadcast 그룹 → `prearranged`). 관리 API·콘솔의 유형 선택지에서 제거 | `sql/cims_schema.sql:183`, 신규 `sql/migrate_*.sql`, `csc/src/handlers/admin.py:1580`, `:1682`, 콘솔 `api/groups.ts`·그룹 편집 |

### 4.4 단말 (libcimsue SDK · Android · 관제 앱)

| # | 항목 | 대상 |
|---|---|---|
| U1 | 일제 통화 발신 API — 그룹 INVITE mcptt-info 에 `session-type=prearranged` + `<broadcast-ind>true` | `sdk/core/src/mcptt/mcptt_xml.cpp`, `android/ptt-client/.../mcptt/McpttXml.kt`(`SessionType.BROADCAST` 제거) |
| U2 | 개시 단말 Floor Request 에 Floor Indicator B-bit (R8) | `sdk/core/src/floor/floor_participant.cpp`, Android `FloorControl.kt` |
| U3 | 개시 단말: 발언 종료 뒤 B-bit Floor Idle 수신 → 호 해제(BYE) (R9) | 같은 파일 |
| U4 | 수신 단말: B-bit → "일제 통화" 표시, Permission 0 → PTT 비활성 (Android 는 구현, SDK·관제 앱 확인) | `PttController.kt:207`, SDK FloorEvent |
| U5 | 그룹 종류 판정을 `<on-network-invite-members>` 로 (G1 짝) | `sdk/core/src/csc/group_doc.cpp:127`, `:187`, Android `CscModels.kt:82` |
| U6 | 관제 앱 "일제 통화" 동작 — 선택한 그룹에 U1 로 발신 | `windows/dispatch-desktop`, `android/dispatch-tablet` |

### 4.5 CSP↔CMP 계약 변경 ([cmp_media_api.md](../../api/cmp_media_api.md))

| 명령 | 변경 |
|---|---|
| `PTT_GROUP_ADD` | `group_type` = `prearranged`/`chat`/`private`. 신규 `broadcast` (0/1). `initiator_id`·`broadcast` 는 그룹 생성 ADD 에서만 유효 |
| `PTT_GROUP_ADD`/`MODIFY` `floor_timers` | `t4_inactivity`(초, 0 = 미사용) 추가 |
| 이벤트 (CMP→CSP) | `PTT_FLOOR_INACTIVITY {group_id, sesid}` — T4 만료 |

## 5. 결정 사항

- **개시 권한**: 서버 인가 없음 — 멤버 단말의 `<broadcast-ind>` 요청을 따른다(§3.1).
- **T4**: 그룹별 설정(`<on-network-hang-timer>`). 일제 통화에 쓰는 그룹은 그 그룹의 T4 를 따로 설정한다(§3.2, G2).

## 6. 검증 (S3)

| id | 시나리오 | 기대 |
|---|---|---|
| BC1 | 일반 그룹에서 `broadcast-ind=true` 개시 | fan-out mcptt-info `prearranged`+`broadcast-ind`, 멤버 Floor Taken B-bit·Permission 0 |
| BC2 | 비개시자 Floor Request (일반·긴급) | Deny #5 |
| BC3 | 진행 중 일제 통화에 다른 멤버가 INVITE 로 합류 후 개시자 재발언 | 개시자 Granted, 합류자 Deny #5 (R7) |
| BC4 | 개시자 발언 종료 | 개시 단말 BYE, T4 만료 후 세션 해제·멤버 BYE |
| BC5 | 일제 통화 중 conference SUBSCRIBE | 480 + Warning 105 |
| BC6 | 같은 그룹에서 `broadcast-ind` 없이 개시 | 일반 그룹 통화(전원 발언 요청 가능) |

계측기 경로는 [test_instrument.md](test_instrument.md) 의 `group_call` 단계에 broadcast 페이로드를 추가해
같은 시나리오를 돌린다.

## 7. 구현 순서 (개발 서버)

작업 단위(WP)는 아래 순서로 한 커밋씩 진행한다 — 앞 WP 가 뒤 WP 의 계약·스키마를 만든다. 각 WP 는 같은 변경에서
해당 문서를 갱신하고(CLAUDE.md "코드 + 문서 동시 갱신"), 건드린 모듈의 `pkg.json` patch 를 올린다(커밋 제목에
`(csp 0.2.x · cmp 0.2.y)` 표기). WP 마다 §7.8 의 빌드·검증을 돈다.

| 순서 | WP | 선행 이유 |
|---|---|---|
| 1 | CMP — broadcast 호 속성·개시자 고정·T4 | CSP 가 쓸 계약(`broadcast`·`t4_inactivity`·`PTT_FLOOR_INACTIVITY`)을 먼저 연다. 구 CSP 와도 동작(전환기 해석) |
| 2 | DB · CSC — 그룹별 T4·최대 시간, 그룹 문서 요소 | CSP 가 새 컬럼을 읽기 전에 스키마가 있어야 한다 |
| 3 | CSP — `<broadcast-ind>`·세션 속성·해제 정책 | 1·2 위에서 동작 |
| 4 | 콘솔 — 그룹 편집 | 2 의 API 필드 |
| 5 | 단말 — 일제 통화 발신·B-bit·호 해제 | 3 의 서버 동작 위에서 시험 |
| 6 | 검증 — cspsim·계측기·S3 항목 | 1~5 |
| 7 | 문서 현행화 | 끝 |

### 7.1 WP1 — CMP: 호 속성 broadcast · 개시자 고정 · T4 (M1~M3)

| 파일 | 변경 |
|---|---|
| `cmp/PCmpServer.cpp` `processAddGroup` | payload `broadcast`(0/1) 파싱. **새 그룹 분기(`_groups` 미존재, ~1868)에서만** `setBroadcastSession(broadcast, initiator_id)`, 기존 그룹 분기(~1952-1956)는 broadcast·initiator 를 무시. 전환기(구 CSP 혼재 — 한 릴리스)에는 `group_type:"broadcast"` 도 `broadcast:1` 로 해석하고 WARN 로그 |
| 같은 파일 floor_timers 파싱(~1728) | `t4_inactivity`(초, 0 = 미사용, 범위 0..3600) → `setFloorTimers` |
| `cmp/PMcpttGroup.h/.cpp` | `_groupType` 과 분리된 `bool _broadcast` + `_initiatorSessionId`. `setBroadcast()` → 생성 시 1회용 `setBroadcastSession()`. `handleFloorRequest`(~862)·`_indicatorFor`(~1025)·Permission 0(~1798) 판정 입력을 `_broadcast` 로 |
| 같은 파일 — T4 | `setFloorTimers(..., t4)`. `_advanceFloorOrIdle` 이 화자가 비어 IDLE 로 갈 때(~1655) `_t4SinceUsec` 무장, `_grantFloorTo`(~1264)·floor 요청 수신 시 해제, `tickFloorTimers`(T7 처리 ~1701 옆)에서 만료 → 콜백 1회 후 재무장(§6.3.4.3.5 — 해제 여부는 CSP) |
| `cmp/PCmpServer.cpp` | 그룹 생성 시 inactivity 콜백 연결(`onFloorTalkers` 연결 ~1856 옆) → `emitEvent("PTT_FLOOR_INACTIVITY", {group_id}, sesid, "mcptt")` |
| 시험 | `tests/cmp_smoke_broadcast.py` 신설(`tests/cmp_smoke_floor_single.py` 모델) — 기존 그룹 재ADD 에 다른 initiator 를 실어도 개시자 유지, 비개시자 요청 Deny #5, `t4_inactivity=2` 뒤 `PTT_FLOOR_INACTIVITY` 수신 |
| 문서 | [cmp.md](../modules/cmp.md) PTT_GROUP_ADD 표·동작 7, [cmp_media_api.md](../../api/cmp_media_api.md) 필드 표·§8 이벤트 표 |

### 7.2 WP2 — DB · CSC (G1~G3)

| 파일 | 변경 |
|---|---|
| 신규 `sql/migrate_ptt_groups_broadcast_call.sql` | 멱등(information_schema 확인 + PREPARE/EXECUTE, `migrate_ptt_allow_create_group.sql` 관례). `ptt_groups` 에 `hang_timer_sec INT NOT NULL DEFAULT 30`(T4)·`max_duration_sec INT NOT NULL DEFAULT 3600`(TNG3) 추가 → `group_type='broadcast'` 행을 `prearranged` 로 바꾸고 `hang_timer_sec=3` → ENUM 을 `('prearranged','chat')` 로 축소. 머리 주석에 이 문서 링크 |
| `sql/cims_schema.sql`(~183)·[db_schema.md](../db_schema.md) | 같은 최종 스키마 |
| `csc/src/services/mcptt.py` 그룹 문서 생성(~1265-1300) | `<on-network-invite-members>` = `group_type=='prearranged'`, `<on-network-hang-timer>PT{n}S</...>`, `<on-network-maximum-duration>PT{n}S</...>`(규격 요소명). 비표준 `<mcpttgi:session-type>` 은 **WP5 단말(U5)이 들어갈 때까지 prearranged/chat 값으로만** 유지(전환기 — Android·SDK 가 아직 이 요소로 그룹 종류를 읽는다), WP5 에서 제거 |
| 같은 파일 XCAP PUT 파싱(~2278-2297) | 그룹 종류를 `on-network-invite-members` 로(없으면 session-type 폴백 — 전환기), hang-timer·maximum-duration(xs:duration) 파싱 |
| `csc/src/handlers/admin.py`(~1580 `_create_group`, ~1682 `_update_group`) | `group_type` 허용값 `prearranged`/`chat`, `hang_timer_sec`·`max_duration_sec` 필드(범위 검사) — GROUP_CHANGED 통지는 기존 경로 |
| 시험 | `tests/test_csc_gms_group_crud.py` 에 그룹 문서 요소·XCAP PUT 왕복 케이스 추가 |
| 문서 | [mcptt_api.md](../../api/mcptt_api.md) 그룹 문서, [admin_api.md](../../api/admin_api.md) 그룹 API |

### 7.3 WP3 — CSP: `<broadcast-ind>` · 세션 속성 · 해제 정책 (P1~P8)

| 파일 | 변경 |
|---|---|
| `csp/McpttInfo.h` | `CMcpttInfo::bBroadcast`(`<broadcast-ind>` — `_McpttIndTrue` 재사용), `strSessionType` 허용값 주석을 규격 6종으로 |
| `csp/ModuleDispatcher.cpp` (~848-853, ~996, ~1127) | `clsMi.bBroadcast` 를 `ProcessGroupCall(..., iMcpttCond, bBroadcastInd)` 로 전달 |
| `csp/GroupCallService.h` `GroupRtpInfo`(~273) | 세션 속성 `bool bBroadcast`, `std::string strInitiator` — `strCallerId` 는 개시자 의미로만 |
| `csp/GroupCallService.cpp` `ProcessGroupCall` | `bActiveSession == false`(~358)일 때만 `bBroadcast`/`strInitiator` 확정. 기존 세션 재ADD(~444-453)는 initiator·broadcast 를 싣지 않는다. ~455-461 의 `strCallerId` 덮어쓰기 제거(합류·청취 leg) |
| `csp/CmpClient.cpp/.h` `AddGroup`(~718) | 인자 `bool bBroadcast` → payload `broadcast:1`. `group_type` 은 `prearranged`/`chat`/`private` 만. `floor_timers.t4_inactivity` = 그룹 `_hangTimerSec` |
| `GroupCallService.cpp` `InviteMember`(~1167-1179) | 재생성 경로는 캐시 세션 속성 사용 |
| `GroupCallService.cpp` `BuildGroupInfoXml`(~2643) | `session-type` = 그룹 종류, 세션이 broadcast 면 `<broadcast-ind>true</broadcast-ind>` |
| `GroupCallService.cpp` conference 구독 판정(~2157) | 그룹 유형 → 세션 `bBroadcast` |
| `csp/CmpClient.cpp` 이벤트 디스패치(~1327 `PTT_GROUP_ABORTED` 옆) | `PTT_FLOOR_INACTIVITY` → `CGroupCallService::OnFloorInactivity(group)` |
| `GroupCallService.cpp` 해제 정책 | 신규 `ReleaseGroupSession(group, reason)` — 확립 leg 전원 BYE + pending CANCEL + `RemoveGroup` + sesid 정리(`TerminateGroupLocal` ~1896 의 leg 순회 재사용). 호출 = T4 만료(on-demand 세션만, chat 제외)·확립 참가자 **1명 이하**(~996, ~2063 의 0명 판정을 on-demand 에서 1명 이하로) |
| `csp/CspPttGroup.h`(~99)·`csp/DbManager.cpp`(~500) | `_groupType` 도메인 `prearranged`/`chat`, `_hangTimerSec`·`_maxDurationSec` 로드, `ComputeGroupConfigHash` 에 포함 |
| dialog `<mcptt>`(~2414)·`BuildGroupDescriptor` | broadcast 를 세션 속성에서 |
| 문서 | [ptt_flows.md](ptt_flows.md) 요약 표·B5·B6·C3b, [csp.md](../modules/csp.md) PTT-AS 절 |

### 7.4 WP4 — 콘솔

| 파일 | 변경 |
|---|---|
| `ems/core/console/src/api/groups.ts`(`Group` ~10-41) | `group_type` 유니온에서 broadcast 제거, `hang_timer_sec`·`max_duration_sec` |
| `ems/service/console/src/pages/PttGroupsWorkbenchPage.tsx`(`GroupDrawer` ~196, 유형 선택 ~312) | 유형 선택지에서 broadcast 제거, "유지 시간(T4, 초)"·"최대 통화 시간(초)" 입력 — 규칙은 `ems/core/console/CLAUDE.md`·`ems/service/console/CLAUDE.md` |

### 7.5 WP5 — 단말 (U1~U6)

| 파일 | 변경 |
|---|---|
| `sdk/core/src/mcptt/mcptt_xml.cpp`·그룹 호 발신 API·`cimsue-cli group-call --broadcast` | 일제 통화 발신 → `<broadcast-ind>true` |
| `sdk/core/src/floor/floor_participant.cpp` | 개시자 Floor Request 에 B-bit(R8), 발언 종료 뒤 B-bit Floor Idle → 호 해제(R9), 수신 B-bit → FloorEvent |
| `sdk/core/src/csc/group_doc.cpp`(~127, ~187) | 그룹 종류를 `on-network-invite-members` 로 → 그 뒤 CSC 그룹 문서의 `<mcpttgi:session-type>` 제거(WP2 전환기 종료) |
| `android/ptt-client` `McpttXml.kt`·`PttController.kt`·`CscModels.kt` | 같은 변경(`SessionType.BROADCAST` 제거) — Android 빌드는 개발 서버가 아니라 Windows(Android Studio)·WSL NDK. `S1-UE-SDS-XCHECK`/`S1-UE-CSC-XCHECK` 가 코어와 Kotlin 을 교차 검사한다 |
| `windows/dispatch-desktop`·`android/dispatch-tablet` | "일제 통화" 동작(선택 그룹에 U1 발신) — Windows 빌드 |

### 7.6 WP6 — 검증 시나리오

| 대상 | 변경 |
|---|---|
| cspsim (`cspsim/SimSession.*`, `CspsimMain.cpp`) | `-broadcast` 플래그 — `SetEmergency`/`m_iEmergencyCond` 패턴대로 multipart mcptt-info 에 `<broadcast-ind>true</broadcast-ind>`(현재 cspsim 은 `-emergency`/`-imminent`/`-adhoc` 일 때만 mcptt-info 를 싣는다, `SimSession.cpp` ~1625-1660) |
| 계측기 (`ems/tester/oam/src/services/tester_models.py` ~810 `GROUP_CALL_MODES`, `tester/worker`, libcsim) | `group_call` `payload: broadcast` — 모델 변경 후 `ems/tester/oam/bin/gen-schemas`, `tests/test_tester_models.py`. 번들 시나리오 `ems/tester/oam/scenarios/ptt/group_call_broadcast.yaml`(BC1~BC4·BC6 — BC3 = 두 번째 `group_call` 단계가 다른 멤버의 합류) |
| S3 항목 | `verify/lib/items/stage3/scn_ptt_broadcast.py` — `@verify_item(id="S3-SCN-PTT-BROADCAST", stage=3, depends_on=["S3-SEED"], side_effects=["sim-call"])`, cspsim 경로 + 계측기 경로(`common.tester`) |

### 7.7 WP7 — 문서 현행화

이 문서 §2 판정을 ✅ 로, [mcptt_standard_conformance.md](mcptt_standard_conformance.md) C8·R1 행과 CLAUDE.md 인덱스의
"(설계 정본, 미구현)" 을 현재 상태로 고친다.

### 7.8 빌드 · 반영 · 검증 (개발 서버, 레포 루트)

```bash
# 빌드 — CSP/CMP/cspsim 만 (서비스는 build/dist 에서 돈다; make dist 는 설정 json 을 덮지 않는다)
cd build && make -j$(nproc) csp cmp cspsim && make dist && cd ..
./cims.sh restart cmp csp                       # CMP 먼저

# DB 마이그레이션 (ALTER 는 root — 앱 계정 cims 는 DML 만)
sudo mysql cims < sql/migrate_ptt_groups_broadcast_call.sql

# CSC 반영
./cims.sh sync csc && ./cims.sh restart csc

# SDK (Linux 빌드) · floor 정의 drift
cd build && make -j$(nproc) cimsue cimsue-cli cimsue_test && cd .. && ./build/bin/cimsue_test
python3 scripts/gen_floor_defs.py --check
```

| 검사 | 명령 |
|---|---|
| CMP floor 단위 | `g++ -std=c++17 -Icmp -Iext/pasf/include tests/cmp_floor_codec_test.cpp cmp/PFloorCodec.cpp -o /tmp/floorcodec && /tmp/floorcodec` · `python3 tests/cmp_smoke_broadcast.py`(WP1 신설) |
| CSC 단위 | `python3 -m unittest tests.test_csc_gms_group_crud` 또는 `./cims-verify run --items S1-UNIT-CSC` |
| S3 (`--items` 는 의존 항목을 끌어오지 않는다 — S3-SEED 를 함께 지정) | `./cims-verify run --items S3-SEED,S3-SCN-PTT-SMOKE,S3-SCN-PTT-LISTEN,S3-SCN-PTT-BROADCAST` |
| 수동 일제 통화 | `./cims.sh sim -mode ptt -scenario group_call -count 4 -group <mcptt_group_id> -duration 10 -broadcast` (`-call_duration` 이 아니라 `-duration`) |
| 선점 회귀 | `./cims.sh sim -mode ptt -scenario group_call -count 3 -group <gid> -duration 10 -preempt -preempt_by 2` |
| 계측기 | `./cims.sh sync oam-cims-tester` 후 `python3 ems/tester/oam/bin/cims-tester --url https://127.0.0.1:4419 --token $TOK run PTT-GROUP-CALL-BROADCAST --topology <name>` |

| 확인 위치 | 경로 |
|---|---|
| floor 판정(GRANT / DENY reason=broadcast / REVOKE) | `find build/dist/ext_mnt/service_log/ptt -name floor.jsonl -mmin -10` |
| 서비스 로그 | `./cims.sh log csp` · `./cims.sh log cmp` · `build/dist/log/<svc>.log` |
| CMP flow(`PTT_FLOOR_INACTIVITY` 이벤트) | `{ServiceLogDir}/YYYY/MM/DD/HH/cmp_01_mcptt.flow.jsonl` |
