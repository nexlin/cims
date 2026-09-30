> 서버(.48 — 소스 서버) 개발 과제 — MCPTT **긴급·경보의 관제 경로**와 그 주변. Windows 관제 앱 보완(긴급 조건·경보 배너·CMS 정책 게이트·
> SDS 재전송 msgId·망 전환)과 SDK C API·.NET 노출이 반영·푸시된 뒤, 앱이 서버를 기다리는 자리를 모았다.
> 원문 대조 판본 = TS 24.379 V18.14.0 · TS 24.282 V18.13.0 · TS 24.484 V18.10.0 (Release 18).
> 정본 = [mcptt_emergency_modes.md](../design/features/mcptt_emergency_modes.md) §4.2·§4.3·§10 · [dispatch_desktop_ui.md](../design/features/dispatch_desktop_ui.md) §3.2·§13 ·
> [mcdata_messaging.md](../design/features/mcdata_messaging.md) §7.
> 전부 반영되면 이 문서는 삭제한다(정본 문서의 편차 표·미해결 항목을 현재 상태로 고치는 것까지 — §9).

# MCPTT 긴급·경보 관제 경로 — 서버 과제

## 0. 순서 · 규모

| 순서 | 과제 | 컴포넌트 | 반영 전 단말·앱 동작 |
|---|---|---|---|
| 1 | **E1 긴급·임박 해제 인가와 거절 응답**(§2) | CSP | 개시자 외의 해제 re-INVITE 를 무시하면서 **200 을 돌려준다** — 보낸 단말은 해제 확정(코어 `Confirmed`)으로 보고 그 화면에서만 긴급이 풀린다. 그래서 관제 앱은 지금 **내가 올린 긴급만** [긴급 해제]를 보인다 |
| 2 | **E2 청취 leg 조건 재광고**(§3) | CSP | 청취 중인 관제사는 합류 200 OK 의 조건만 받는다 — 청취 중에 긴급이 걸리거나 풀려도 ② 카드·배너가 모른다 |
| 3 | **E3 경보 취소 인가**(§4) | CSP | 누구의 취소든 통과한다(인가 게이트 없음) — 관제 앱 [경보 해제](제3자 취소)는 user profile 로 선차단만 한다 |
| 4 | **E4 Resource-Priority 정본 = service-config**(§5) | CSP | 멤버 fan-out·조건 재광고가 `mcpttp.15/8/0` 고정 — 단말(SDK·관제 앱)은 service-config 값을 쓰므로 운영자가 CSC 에서 바꾸면 둘이 갈라진다 |
| 5 | E5 TNG2·MESSAGE 경로 긴급 해제(§6) | CSP | 긴급 상태는 권한자 re-INVITE 해제와 세션 종료로만 풀린다 |
| 6 | M1 SDS 전달 확인의 규격 경로(§7) | CSP + SDK 코어(.45) | 통지가 원 발신자 AoR 로 한 파트만 간다 — 같은 서버·같은 SDK 끼리는 ✓✓ 가 선다 |
| 7 | 시험·검증(§8) · 문서 판정(§9) | tester·verify·docs | — |

E1~E4 는 서로 독립이다. E1 이 가장 급하다(관제사가 남의 긴급을 풀었다고 믿는 상태를 만든다).

## 1. 결정 사항 (되묻지 않는다)

- **긴급 그룹 상태 해제의 인가는 규격이 «local policy» 로 둔다** — TS 24.379 §6.3.3.1.13.4 "*the controlling MCPTT function determines, based on local policy
  (e.g if the requester is dispatcher or not, initiator of the MCPTT emergency group call, if other participants exist in the MCPTT session who are in
  emergency state etc), whether the emergency group state cancel request is authorised or not*", 임박 위험은 §6.3.3.1.13.13 같은 문장. 설계 정본은
  **개시자 또는 그룹 authorized user** 다([mcptt_emergency_modes.md](../design/features/mcptt_emergency_modes.md) §4.2 «취소») — 코드가 개시자만 본다(§2 E1-a).
  관제 역할까지 넓힐지는 §2 E1-d 의 결정 항목이다.
- 비인가 요청은 **거절(403)** 이다 — 무시하고 200 을 주면 규격 위반이고 보낸 단말이 해제를 확정으로 본다(§2 E1-b).
- 관제석은 자리별 로그인 ID 를 쓴다(앱 로컬 보관 격리 단위) — 서버 인가 판정의 주체는 그대로 MCPTT ID(가입자)다.

## 2. E1 — 긴급·임박 해제 인가와 거절 응답 (TS 24.379 §10.1.1.4.7 7)·7a)·8), §6.3.3.1.13.4·§6.3.3.1.13.13)

규격 요점(§10.1.1.4.7 «Receipt of a SIP re-INVITE request», controlling):
- 7) `emergency-ind` false 이고 **비인가**(§6.3.3.1.13.4)면 "*shall reject the SIP re-INVITE request with a SIP 403 (Forbidden) response*" — 403 에 mcptt-info
  `<emergency-ind>` **true**, 요청에 `<alert-ind>` false 가 있었고 그 사용자의 경보가 남아 있으면 `<alert-ind>` true 도 싣는다.
- 7a) 인가됐더라도 **다른 사용자가 긴급 상태로 발언 중**이면 같은 403.
- 8) 인가되고 그룹이 긴급 상태면 해제 → 참여자에게 재광고(§6.3.3.1.6) · TNG2 정지 · 참여하지 않은 제휴 멤버에 상태 알림 MESSAGE(§6.3.3.1.11, `emergency-ind` false).
- 임박 위험 해제(`imminentperil-ind` false)도 같은 모양 — 인가는 §6.3.3.1.13.13(local policy).

| # | 위치 | 지금 | 규격 | 고칠 방향 |
|---|---|---|---|---|
| E1-a | `GroupCallService.cpp` `ApplyInCallCondition` 하향 분기(~1433) | `strActor != strMemberId` 면 로그만 남기고 `return` — **개시자만** 인가 | 인가 = local policy(§6.3.3.1.13.4) — 설계 정본은 개시자 ∨ 그룹 authorized user | 인가 판정 함수 하나(`IsConditionCancelAuthorized(group, member)`): 개시자 ∨ `clsGroup` 의 authorized user(`authorized_user_id` → MCPTT ID) (∨ E1-d) |
| E1-b | `ModuleDispatcher.cpp` in-call 조건 엿보기(~506–530) | 상향 비인가만 403(`IsInCallUpgradeAllowed`), 하향은 `ApplyInCallCondition` 이 조용히 무시 → re-INVITE 흐름이 계속돼 **200** | 7)·7a) — 403 + mcptt-info `<emergency-ind>` true(+ 조건부 `<alert-ind>` true), 나머지 단계 건너뜀 | 하향도 판정을 먼저 — 비인가(또는 7a))면 상향 거절과 같은 경로로 403 을 보내되 본문 `emergency-ind` **true**(임박이면 `imminentperil-ind` true). 상향 거절 본문 조립부를 인자(true/false·alert)로 일반화 |
| E1-c | 같은 곳 | 7a) 판정 없음 | 인가돼도 다른 사용자가 긴급 상태로 발언 중이면 403 | CMP floor 상태(현재 발언자·그 tier)로 판정 — CMP 질의가 없으면 CSP 가 아는 `SetFloorTier` 대상과 floor TAKEN 통지로. 어렵다면 편차 표에 사유를 적고 후속 |
| E1-d | 결정 필요 | — | 규격 예시에 "*if the requester is dispatcher*" | 관제 역할(mcptt_authorization.md — `ptt_listen`/관리 범위 안 그룹)까지 넓힐지 결정. 넓히면 CSC 역할 판정(`can(principal, capability, target)`)을 CSP 가 쓸 수 있는 형태(그룹 캐시 필드 또는 새 capability 예: `emergency_cancel`)로 |

- **관제 앱 쪽 후속**(서버 반영 뒤, Windows): `DispatchSession.CanCancelCondition` 을 E1-a 의 판정(내 조건 ∨ 내 소유 그룹 ∨ E1-d)으로 넓히고, 403 은 조건 이벤트
  `Denied` → «긴급 호출 자격이 없습니다» 대신 해제 거절 문구로(`ResponseText` 에 조건 하향 영역). 코어는 이미 403 이면 이전 값으로 되돌린다.
- **개시자 기록**: `m_mapGroupCondActor` 는 상향한 멤버다. 초기 긴급 INVITE 로 선 긴급의 개시자도 같은 맵에 들어가는지 확인(§6.3.3.1.6 재광고의
  `<mcptt-calling-user-id>` = 개시자와 같은 값이어야 한다).

## 3. E2 — 청취 leg 조건 재광고 (TS 24.379 §6.3.3.1.6·§6.3.3.1.10·§6.3.3.1.15)

규격 요점: 조건 재광고 re-INVITE 는 "*each of the other participants in the group call*"(§10.1.1.4.7 8)d)) 에게 가고, SDP 는 "*the media parameters as
currently established with the terminating MCPTT client*"(§6.3.3.1.15 2)). 청취 leg(관제사의 `a=recvonly` 합류 — dispatch_center.md §5.6, TS 24.484
`allow_ambient_listening` 자격)도 세션 참여자이고, 성립한 미디어(서버 쪽 `a=sendonly`)를 그대로 다시 제시하면 된다.

| # | 위치 | 지금 | 고칠 방향 |
|---|---|---|---|
| E2-a | `GroupCallService.cpp` `PropagateConditionToMembers`(~3481) | `if ( kv.second.bListenOnly ) continue;` — 청취 leg 제외 | 청취 leg 도 대상. SDP 는 **그 leg 의 성립 SDP**(`GetLocalCallRtp` — 포트·코덱·SRTP 키·방향 `sendonly`)로 — 멤버용 `GetOrAllocMemberPort` 로 다시 만들지 않는다(청취 leg 는 멤버 포트가 아니다). floor `m=application` 은 청취 leg 가 협상한 대로(없으면 port 0 미러) |
| E2-b | 같은 함수 | — | 은닉 청취(`bListenHidden`)도 보낸다 — 은닉은 로스터 노출 규칙이지 청취자 자신에게 알리지 않는 규칙이 아니다. 로스터·conference NOTIFY 에는 여전히 싣지 않는다 |
| E2-c | 단말 응답 | pjsua 가 자동 200 — psip `EventReInviteResponse`(CSP no-op) | 청취 단말(SDK)은 recvonly 를 유지해 답한다 — SDK 코어는 re-INVITE 의 조건만 읽는다(`onMcpttCondition Advertised`). 실측으로 recvonly 유지 확인 |

- 반영되면 관제 앱 두 벌의 ② 카드·긴급 배너가 청취 중에도 격상·해제를 따라간다(앱 변경 없음 — 코어 조건 이벤트를 이미 쓴다).
- mcptt_emergency_modes.md §10-5 의 선택지 중 «같은 re-INVITE 를 recvonly 그대로» 를 권고한다 — 상태 알림 MESSAGE(§6.3.3.1.11)는 **참여하지 않은** 제휴 멤버의 몫이다.

## 4. E3 — 경보 취소 인가 (TS 24.379 §6.3.3.1.13.3·§12.1.3.2 1))

규격 요점: 취소(`<alert-ind>` false)의 인가 = 요청자 user profile `<allow-cancel-emergency-alert>` true(§6.3.3.1.13.3). 비인가 취소 MESSAGE 는 403 +
mcptt-info `<alert-ind>` true(§12.1.3.2 1)). re-INVITE 에 실린 비인가 취소는 200 + Warning "149 SIP INFO request pending"(§10.1.1.4.7 끝 6)).

| # | 위치 | 지금 | 고칠 방향 |
|---|---|---|---|
| E3-a | `PttAsModule.cpp` `OnEmergencyAlert`(~104) | 인가 게이트는 발령(`bActivate`)만 — "취소는 사용자 게이트 비대상(잔존 경보 정리 경로 보존)" | 취소도 판정 — 단말이 받는 user profile 문서와 같은 값으로: CSC 는 `<allow-cancel-emergency-alert>` 를 발령과 **같은 DB 열 `allow_emergency_alert`** 로 낸다(`mcptt.py` ~1623) → CSP 도 `CspUserProfile.m_bAllowEmergencyAlert` 로 본다. 비인가면 403 + `<alert-ind>` true, 전파 없음 |
| E3-b | 취소 자격의 축 분리 | 발령·취소가 한 열 | §6.3.3.1.13.3 은 취소를 따로 본다(TS 24.484 ruleset 요소도 둘) | 후속 — DB 열(`allow_cancel_emergency_alert`)·CSC 문서·콘솔 편집·CSP 필드를 함께. 관제사에게 취소만 주는 배정(발령 없이)이 이것을 요구한다. 자기 경보 취소도 같은 인가를 본다(§6.3.3.1.13.3 에 예외 없음) |
| E3-c | re-INVITE 동봉 취소 | 확인 필요 | 긴급 해제 re-INVITE 에 `<alert-ind>` false 가 실린 경우(§6.2.8.1.3 NOTE 4) — 비인가면 200 + Warning 149 |

- 관제 앱은 user profile `allow-cancel-emergency-alert` 로 [경보 해제]를 숨긴다(선차단). 서버 게이트가 서면 편차 표(mcptt_emergency_modes.md §4.3 «경보 취소 인가»)를 지운다.

## 5. E4 — Resource-Priority 정본 = service-config (TS 24.379 §6.3.3.1.19)

규격 요점: controlling function 은 긴급·임박·일반 Resource-Priority 의 namespace·priority 를 **service-config** `<OnNetwork>` 의
`<emergency-resource-priority>`·`<imminent-peril-resource-priority>`·(일반) 에서 읽는다("*shall retrieve the value of the <resource-priority-namespace>
element … of the MCPTT service configuration document*").

| # | 위치 | 지금 | 고칠 방향 |
|---|---|---|---|
| E4-a | `GroupCallService.cpp` 멤버 fan-out(~1886)·`PropagateConditionToMembers`(~3522) | `"mcpttp.15"`/`"mcpttp.8"`/`"mcpttp.0"` 고정 | `CCspServiceConfig`(이미 CSC `/internal/mcptt/service-config` 를 받아 floor 타이머를 읽는다)가 RP 세 값도 해석(`CspFloorParams` 옆에 `CspPriorityParams`) — 문서에 없으면 지금 값으로 폴백. 두 곳이 한 함수(`ResourcePriorityOf(iCond)`)를 쓰게 |
| E4-b | 단위시험 | `tests/csp_service_config_test.cpp` | RP 해석(namespace.priority 조립·미기재 폴백) 케이스 추가 |

- 단말은 이미 service-config 값을 쓴다(SDK `ServiceConfigDoc.rp*` → `AccountConfig.rp*`, 관제 앱은 PTT 계정 생성 때 적용).

## 6. E5 — TNG2 · MESSAGE 경로 긴급 해제 (TS 24.379 §6.3.3.1.16·§12.1.3.3)

- **TNG2**(진행 중 긴급 그룹콜 타이머) — 만료 시 긴급 상태 해제 + 참여 멤버 재광고(§6.3.3.1.10) + 참여하지 않은 제휴 멤버 상태 알림 MESSAGE(§6.3.3.1.11).
  mcptt_emergency_modes.md §10-6 그대로. 값의 정본은 service-config(TNG2) — E4 와 같은 해석기에 싣는다.
- **호가 없을 때의 긴급 상태 해제 MESSAGE**(§12.1.3.3) — 경보 취소 MESSAGE 에 동봉된 `<emergency-ind>` false(SDK `sendEmergencyAlert(cancelGroupEmergency=true)`)를
  `OnEmergencyAlert` 가 제휴 멤버 통지에 옮기기만 하고 **그룹의 긴급 상태(`m_mapGroupCondition`)는 건드리지 않는지** 확인 — 규격은 인가(§6.3.3.1.13.4) 뒤
  상태를 풀고, 비인가면 403 + `<emergency-ind>` true(+ 조건부 `<alert-ind>` true).

## 7. M1 — SDS 전달 확인의 규격 경로 (TS 24.282 V18.13.0 §12.2.1.1) — SDK 코어(.45) + CSP

- 지금: SDK `sendSdsNotification` 이 원 발신자 AoR 로 SDS NOTIFICATION 한 파트만 보낸다. 규격은 대상 MCData ID 의 `resource-lists` 와 그룹 통지의
  `<mcdata-calling-group-id>` 를 요구한다([mcdata_messaging.md](../design/features/mcdata_messaging.md) §7 편차 표).
- 나눔: CSP = 규격 통지 수신·중계(요청 URI = MCData 서버 PSI, resource-lists 대상 해석), SDK 코어(.45) = 통지 API 가 수신 SDS 의 그룹·발신자 문맥을
  받아 규격 본문을 만든다. 둘을 같은 창에 올린다(전환기: CSP 가 옛 한 파트 통지도 받는다). 관제 앱은 코어 API 가 바뀌면 인자만 넘긴다.

## 8. 시험 · 검증

| 항목 | 방법 |
|---|---|
| E1 | 계측기 시나리오(예 `PTT-EMERGENCY-CANCEL-AUTHZ`): A 긴급 개시 → B(비개시자·비소유자) 해제 re-INVITE → **403 + emergency-ind true**, 멤버 재광고 없음 · C(그룹 authorized user) 해제 → 200 + 멤버 재광고 false · 7a) 다른 긴급 사용자 발언 중 해제 → 403. `cimsue-cli group-call --cancel-at` 로 재현 가능(코어 `Denied` 확인) |
| E2 | 관제 청취(`listen_only`) 중 멤버 상향·하향 → 청취 leg re-INVITE 수신(SDP recvonly 유지)·`onMcpttCondition Advertised` · 은닉 청취도 수신·로스터 비노출 유지 |
| E3 | `allow_cancel_emergency_alert=false` 사용자 취소 → 403 + alert-ind true·전파 없음 · true 사용자 제3자 취소 → 원 발령자 포함 제휴 멤버 수신(`originated-by`) |
| E4 | CSC `ServiceConfig` RP 변경(예 emergency 14) → CSP 재적재 → fan-out·재광고 `Resource-Priority: mcpttp.14` |
| 회귀 | 계측기 PTT 회귀(`PTT-GROUP-CALL-BASIC`·`FLOOR-HANDOVER`·`BROADCAST`·`LISTEN-ROSTER`)·S1 CSP 단위시험 |

## 9. 문서 판정 갱신 (구현과 같은 커밋)

- [mcptt_emergency_modes.md](../design/features/mcptt_emergency_modes.md) — §4.2 «취소»(인가·403), §4.3 편차 표(경보 취소 인가 행 삭제), §10-3·§10-5·§10-6 을
  현재 상태로(해소된 항목 삭제).
- [dispatch_desktop_ui.md](../design/features/dispatch_desktop_ui.md) §13 «청취 leg 의 조건 변화(서버)»·«긴급 해제 인가(서버)» 삭제, §3.2·§4.1 의 [긴급 해제] 조건 —
  앱 쪽 판정을 넓히는 Windows 변경과 함께.
- [android_dispatch_tablet.md](../design/features/android_dispatch_tablet.md) §6.2a-1 «청취 채널은 코어만으로 안 된다» 문단.
- 이 문서 삭제.

## 10. 참고 — 서버가 아닌 몫 (단말·앱)

| 항목 | 몫 | 상태 |
|---|---|---|
| MCPTT 착신 수락의 호 종류별 분리(그룹콜 자동·개별 통화 수동, TS 24.379 §6.2.3) | SDK 코어(.45) → C API·.NET·Kotlin | 미착수 — ue_sdk.md §11 |
| 경보 Request-URI = 참여 기능 PSI | SDK 코어(ue-init-config 해석 — `<anyExt><MCPTT-Service-Details><Server-URI>`) + 앱 | CSC 는 이미 ue-init-config 를 낸다(`UeInitConfig.ServiceDetails.Mcptt.Enable` 이 켜져 있어야 Server-URI 가 실린다). 앱은 아직 비워 그룹 URI 로 보낸다(CSP 전환기) |
| ② 타인 간 개별·애드혹 세션 | SDK 코어(`dialogWatch` 를 PTT 계정으로 + `<mcptt>` 확장 필드) | 서버 계약 반영됨(dispatch_center.md §5.6a) |
| 대표번호 발신 표시(P-Preferred-Identity) | SDK 코어(발신 헤더 옵션) + 앱 토글 | 서버 반영됨(dispatch_center.md §4.7) |
| 외부망 SMS/LMS 게이트웨이 | 서버 인프라(IBCF→SMSC TS 24.341 또는 SMPP) | 장기 — 앱은 `capabilities.smsGateway` 로 이미 켜고 끈다 |
| Android 관제 태블릿 — Windows 반영분 중 없는 것(경보 배너 `onEmergencyAlert`·[긴급 호출] 조건 상향·SDS 로그인 ID 격리·그룹 폼 칸·멤버 우선순위 보존) | Android(Windows 안정화 뒤) | 코드 미반영 — 긴급 조건 배너·[채널로] 포커스는 태블릿에 이미 있다(android_dispatch_tablet.md §6.2a-1) |
