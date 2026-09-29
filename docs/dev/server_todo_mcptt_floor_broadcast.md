> 서버(.48 — 소스 서버) 개발 과제 — MCPTT **암묵적 발언 요청**(개시 INVITE 로 발언까지)과 **애드혹 일제 통화**.
> 단말(SDK `GroupCallOptions.implicitFloorRequest`)과 Windows 관제 앱(그룹 카드·발신 [애드혹] 의 [일제 통화] 한 버튼)은 반영·푸시됐다(79a18bc7·3d3ca6ce).
> Android 관제 태블릿은 코드만 반영했고 Windows 안정화 이후 진행한다(서버 과제와 무관).
> 원문 대조 판본 = TS 24.379 V18.14.0 · TS 24.380 V18.8.0 · TS 24.481 V18.3.0 · TS 24.484 V18.10.0 (Release 18).
> 정본 = [mcptt_broadcast_group_call.md](../design/features/mcptt_broadcast_group_call.md) R13·R14·§7 · [mcptt_standard_conformance.md](../design/features/mcptt_standard_conformance.md) C4.
> 전부 반영되면 이 문서는 삭제한다(정본 판정을 ✅ 로 바꾸는 것까지 — §5).

# MCPTT 암묵적 발언 요청 · 애드혹 일제 통화 — 서버 과제

## 0. 순서 · 규모

| 순서 | 과제 | 컴포넌트 | 반영 전 단말·앱 동작 |
|---|---|---|---|
| 1 | **P1 암묵적 발언 요청 해석**(§2 I1~I3) | CSP | SDK 가 `mc_implicit_request;mc_granted` 를 함께 실어 지금 CSP(offer `mc_granted` → 초기 발언권)에서도 누르는 즉시 발언된다. 반영 뒤 규격 경로(`mc_implicit_request`)로 같은 결과 |
| 2 | **P2 애드혹 일제 통화**(§3 B1~B3) | CSP(CMP 확인만) | 일반 애드혹 그룹 통화로 열리고 관제 앱이 «일제 통화로 열리지 않았습니다» 를 알린다 |
| 3 | P3 시험 도구·검증(§4) | scripts·tester·verify | — |
| 4 | P4 문서 판정 갱신(§5) — 구현과 같은 커밋 | docs | — |

P1·P2 는 서로 독립이다. P2 의 한 버튼 동작(누르는 동안 발언)은 P1 없이도 지금 CSP 에서 된다(위 1행).

## 1. 결정 사항 (되묻지 않는다)

- **일제 통화 개시 권한은 서버가 인가하지 않는다** — 편성 그룹은 멤버, 애드혹은 기존 개시 자격(`allow_adhoc_call`·`Setup.PttAdhocEnabled`)만.
  요구 규격(TS 22.280 [R-5.2.1-001] "*authorized … as determined by the MCX Service Administrator*")은 있으나 구조·구현 규격(TS 23.379 Annex A,
  TS 24.379 §4.12, TS 24.484)에 인가 요소·절차가 없다. 운영 통제 = 그룹 편성 + 관제 앱 버튼 배치.
- **그룹 종류에 broadcast 를 되살리지 않는다** — 일제 통화는 호 속성이다(TS 24.379 §4.12 "*any MCPTT group that the MCPTT user is part of*",
  TS 24.481 에 "broadcast" 요소 없음). 일제 통화용 그룹은 편성(prearranged) 그룹으로 만들고 그룹 이름(`<display-name>`)으로 알린다.
- **진행 중인 호는 일제 통화로 바꾸지 않는다** — 진행 중 호에 온 INVITE 는 합류다(TS 24.379 §10.1.1.3.1.1 15), 긴급 격상 15)f) 와 달리 broadcast 단계 없음).
- 관제 앱 명칭(개별/애드혹)은 화면 문자열이다 — 서버·와이어 무관.

## 2. P1 — 암묵적 발언 요청 (TS 24.380 §14.2.4·§14.2.5·§14.3.1·§14.3.4·§14.3.5·§14.5·§12.1.2.2, §6.3.4.2.2 3)·§6.3.4.4.2, TS 24.379 §6.4)

규격 요점:
- **요청은 `mc_implicit_request`**(§14.2.5 "*shall include … when a SIP request shall be interpreted as an implicit floor request*", TS 24.379 §6.4).
- **offer 의 `mc_granted` 는 능력 표시다** — "200 OK 로 승인 표시를 받을 수 있다"(§14.2.4 "*shall include … when it is acceptable for the MCPTT client
  to receive a granted indication in the SIP 200 (OK) response*"), "*does not indicate an actual request for the floor*"(§12.1.2.2 NOTE 2). 이어지는 offer 에는 싣지 않는다(§14.5).
- 서버는 새 호 개시면 암묵 요청으로 받고 응답에 `mc_implicit_request` 를 싣는다 — chat 합류·진행 중 prearranged/ad hoc 합류는 제외(§14.3.5). 되돌림은 승인 뜻이 아니다(§12.1.2.2 NOTE 4).
- 200 OK 로 승인을 알리면 answer 에 `mc_granted`(선택 "may", temporary group 세션은 금지 — §14.3.4). 승인해도 Floor Granted 메시지는 보낸다(§6.3.4.4.2 1.).
- answer 에는 offer 에 없던 파라미터를 싣지 않는다(§14.3.1).

| # | 위치 | 지금 | 규격 | 고칠 방향 |
|---|---|---|---|---|
| I1 | CSP `ParseMcpttFmtp`(`GroupCallService.cpp` ~258) → `PTT_JOIN.granted` | offer 의 `mc_granted` 를 초기 발언 요청으로 읽는다. `mc_implicit_request` 는 읽지 않는다 | offer 의 `mc_granted` = "200 OK 로 승인 표시를 받을 수 있다"는 **능력**(§14.2.4 "shall include … when it is acceptable…"), "*does not indicate an actual request for the floor*"(§12.1.2.2 NOTE 2). 요청 = `mc_implicit_request`(§14.2.5, TS 24.379 §6.4) | `McpttFmtp` 에 `iImplicit` — offer `mc_implicit_request` 가 `PTT_JOIN.granted`, `mc_granted` 는 answer 조립용 능력으로만 |
| I2 | CSP `ProcessGroupCall` 합류 경로 | 합류 INVITE 의 fmtp 도 그대로 `granted` 로 간다 | "*…unless the MCPTT client is joining a chat group call or an ongoing pre-arranged call or adhoc group call*"(§14.3.5) | 새 세션 개시(편성·ad hoc·개별 호)일 때만 `granted` |
| I3 | CSP 개시자 answer(`GroupCallService.cpp` ~2945 `sdpFloor`, psip `CSipDialog::AddSdp`) | `mc_queueing;mc_priority=3` 고정 | 받아들이면 응답에 `mc_implicit_request`(§14.3.5, 승인 뜻은 아님 — §12.1.2.2 NOTE 4). 200 OK 로 승인을 알리면 `mc_granted`, temporary group 세션은 금지(§14.3.4). offer 에 없던 파라미터는 싣지 않는다(§14.3.1 — 지금 `mc_priority=3` 은 offer 와 무관) | 받아들임 → `mc_implicit_request` 되돌림. `mc_granted` 는 선택("may") — (a) 싣지 않고 CMP 의 Floor Granted 로만 알림(CMP 무변경) 또는 (b) `PTT_JOIN` 응답에 초기 발언권 결과를 받아 승인이면 `mc_granted`. 파라미터는 offer 에 있던 것만 |

- **CMP 는 바꿀 것 없다**(I3 (b) 를 고르면 `PTT_JOIN` 응답 필드 하나) — `grantInitialFloor` 가 Floor Granted 를 보내는 것은 §6.3.4.4.2 1. 그대로이고,
  일제 세션의 개시자 검사(비개시자 초기 발언권 거부)도 그대로 쓴다.
- **호환**: SDK 는 두 속성을 함께 실어 지금 CSP 와 고친 CSP 양쪽에서 동작한다. Android PTT 앱(`PttController`)·cspsim·libcsim 은 offer 에
  `mc_granted` 를 싣지 않는다(grep 확인) — I1 로 바뀌는 단말 동작은 없다. 규격대로 `mc_granted` 를 싣는 3rd-party 단말이 요청 없이 발언권을 받던 편차가 없어진다.

## 3. P2 — 애드혹 일제 통화 (TS 24.379 §17.2.2.1.1 9)·§17.1·§6.3.8.1·§10.1.3.4.1)

규격 요점: 개시 단말이 참가자 목록을 주는 ad hoc 그룹 통화 INVITE 에 `<broadcast-ind>true`("*if the MCPTT user has requested the origination of a
broadcast adhoc group call, the MCPTT client shall comply with the procedures in clause 6.2.8.2*"). 받는 사람 = 목록의 참가자(affiliation 무관).
SDK 는 이미 싣는다(`GroupCallOptions{members, broadcast, implicitFloorRequest}` → resource-lists + mcptt-info `<broadcast-ind>`).

| # | 위치 | 지금 | 규격 | 고칠 방향 |
|---|---|---|---|---|
| B1 | CSP `ProcessGroupCall` 세션 속성(`GroupCallService.cpp` ~498) | `bBroadcast = bBroadcastInd && IsOnDemandGroupCall(clsGroup)` — `IsOnDemandGroupCall` 이 `_isAdhoc` 를 뺀다(INFO "편성 그룹 호 아님") | §17.2.2.1.1 9) — ad hoc 그룹 통화 INVITE 의 `<broadcast-ind>` | 일제 가능 = 편성 on-demand **또는 ad hoc**(`_isAdhoc && _groupType != "private"` — 개별(private) 호 제외). 도우미 하나로(`IsOnDemandGroupCall` 의 다른 쓰임 — T4·해제 — 은 그대로) |
| B2 | CSP `CheckConferenceSubscribe`(~2292) | ad hoc 가지(`CanObserveEphemeral`)가 일제 480/105 검사보다 먼저 return | §10.1.3.4.1 — "*a group call initiated as a broadcast group call*"(ad hoc 포함) | `IsBroadcastInProgress` 검사를 ad hoc 가지 앞으로(`bAuthzOnly` 규칙 그대로) |
| B3 | 세션 해제 | ad hoc T4 = 0(`CmpSessionOf` 는 편성 그룹만 hang-timer) — 개시자가 BYE 해도 수신자가 둘 이상이면 세션이 남는다(TNG3 까지) | §6.3.8.1 1) T4 는 ad hoc 에도, 3) "*the initiator of the group call leaves*" = 로컬 정책 해제(편성·ad hoc) | 일제 세션은 **개시자 이탈 시 해제**(3) 로컬 정책 — 일제 통화는 개시자 송출이 끝나면 호도 끝, §4.12). 편성 그룹 일제에도 같이 걸면 수신자가 T4 를 기다리지 않는다. 대안 = ad hoc T4 를 TS 24.484 `<adhoc-group-call>/<hang-time>` 로 |
| B4 | 인가 | 추가 인가 없음 — ad hoc 은 기존 `allow_adhoc_call`·`Setup.PttAdhocEnabled` 만 | 일제 통화 개시 인가는 stage 3 에 없다(TS 24.379 §4.12, TS 24.484 에 요소 없음) | **그대로**(사용자 결정 — 서버는 멤버·참가자 누구나. 운영 통제는 그룹 편성·관제 앱 버튼 배치) |
| B5 | CMP | 개시 ADD 의 `broadcast:1`·`initiator_id` 로 개시자 고정·비개시자 Deny #5·B-bit·Taken Permission 0 — 그룹 종류와 무관 | §6.3.5.3.4 | 변경 없음 예상 — ad hoc 세션에서 확인만 |

## 4. P3 — 시험 도구·검증

| # | 위치 | 지금 | 규격 | 고칠 방향 |
|---|---|---|---|---|
| I4 | 사내 시험 도구 | `scripts/mcptt_floor_policy_probe.py` [14] 가 offer `mc_granted` 로 초기 발언권을 기대한다 | — | `mc_implicit_request` 로 옮긴다(cspsim·libcsim·Android `PttController` 는 offer 에 `mc_granted` 를 싣지 않음 — grep 확인) |
| B6 | 검증 | 없음 | — | 계측기 시나리오(ad hoc `group_call` + `payload: broadcast` — `PTT-GROUP-CALL-BROADCAST` 의 ad hoc 판), S3 `S3-SCN-PTT-BROADCAST` 에 ad hoc 경우 |

확인 절차(.48, 구현 뒤):
- 편성 그룹 암묵 요청: `cimsue-cli [계정] group-call <g> --broadcast --implicit --ptt-at 0 --ptt-len 3` → 명시 Floor Request 없이 Granted(단말 로그에
  `floor implicit request not accepted` 가 없어야 함), answer 에 `mc_implicit_request`, 놓은 뒤 `broadcast_released:true`.
- 진행 중 세션에 같은 명령 → answer 에 `mc_implicit_request` 없음 → 단말이 명시 Floor Request(§14.3.5 합류 제외).
- 애드혹 일제: Windows 관제 앱 발신 [애드혹] 에서 두 명 고르고 [일제 통화] 누름 → 받는 단말 «일제»·발언 불가(Deny #5), 앱에 «일제 통화로 열리지 않았습니다» 가 **없어야** 함,
  놓으면 개시 단말 BYE → (B3 정책이면) 수신자 세션도 즉시 해제. 받는 단말의 conference SUBSCRIBE 는 480 + Warning 105(B2).

## 5. P4 — 문서 판정 갱신 (구현과 같은 커밋에서)

| # | 위치 | 지금 | 규격 | 고칠 방향 |
|---|---|---|---|---|
| I5 | 문서 | `cmp_media_api.md` §7.4 `granted`·`mcptt_standard_conformance.md` F5·C4·`csp.md` 「멤버별 floor 협상 전달」이 `granted` = "fmtp `mc_granted` 협상" | — | "암묵적 발언 요청(offer `mc_implicit_request`)을 받아들임" 으로. 정본 R14 판정 ⚠ → ✅ |
| B7 | 문서 | 정본 R13 판정 «CSP ❌»·U8 «서버 ❌»·§7 항목 | — | 판정 ✅, §7 에서 뺀다. `mcptt_standard_conformance.md` R1 «Broadcast adhoc group call» △ → ✓, C4 편차 bullet 삭제 |

## 6. 후속 (이번 범위 밖 — 규격 편차로 남은 것)

- **ad hoc 그룹 ID** — 규격은 서버가 준다(TS 24.379 §17.1 "*The MCPTT group ID of the adhoc group call is provided by the MCPTT server*"). CIMS 는 단말이
  `adhoc-<번호>-<epoch>` 를 만든다([mcptt_emergency_modes.md](../design/features/mcptt_emergency_modes.md) §6). 단말·서버 동시 변경.
- **ad hoc mcptt-info `session-type`** — 규격 값은 `adhoc`(Annex F.1), CIMS 단말은 `prearranged` + resource-lists 를 싣는다. 위와 함께.
- **최소 affiliation 인원 미달 해제**(TS 24.379 §6.3.8.1 4) — ad hoc 제외) — 그룹 문서 `<on-network-minimum-number-of-affiliated-members>` 와 함께.
- **CMP 전환기 제거** — `group_type:"broadcast"` 해석은 모든 사이트의 CSP 가 `broadcast` 필드를 싣는 판으로 올라간 뒤 제거한다.
