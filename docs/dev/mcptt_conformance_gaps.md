# MCPTT 규격 정합 보완 목록

PTT(MCPTT) 서비스 구현을 3GPP 규격 원문과 대조해 **규격과 다른 지점**을 모은 목록이다. 고친 것은 없다 — 찾아서 적은 것이다.
이미 문서에 적힌 편차와 통째 미구현 기능은 여기 다시 싣지 않는다(§0 «이미 있는 판정»). 정합 상태의 정본은
[mcptt_standard_conformance.md](../design/features/mcptt_standard_conformance.md) 이고, 이 목록의 항목이 반영되면 그 문서를 갱신하고 여기서 지운다.

## 0. 범위·방법·표기

**대조 기준**

| 규격 | 판 | 범위 |
|---|---|---|
| TS 24.379 | V19.8.0 | 호 제어 — 등록·서비스 인가(§7) · 제휴(§9) · 그룹 호(§6·§10) · 개별 호(§11) · 경보(§12) · 애드혹(§17) |
| TS 24.380 | V19.3.0 | 발언권 제어 — 서버(§6.3) · 단말(§6.2) · 메시지(§8) · SDP(§4.3·§14) |
| TS 24.481 | V19.3.0 | 그룹 관리 — 절차(§6) · 그룹 문서(§7.2) |
| TS 24.484 | V19.6.0 | 설정 관리 — UE initial configuration(§7.2) · user profile(§8.3) · service configuration(§8.4) |
| TS 24.482 · TS 33.180 | V19.1.0 · V19.4.0 | 신원 관리 · 부록 B(토큰) |
| TS 23.379 | V19.11.0 | 호 모델(chat 그룹) |

코드 = `main` `4bd4c086`. 서버(CSP·CMP·CSC)와 단말(SDK `sdk/core`·현장 앱 `android/ptt-client`·관제 앱 두 벌)을 함께 봤다. MCData·MCVideo 는 보지 않았다.
전부 **코드 읽기**다 — 실서버·실기·와이어 캡처로 확인한 항목은 없다.

**이미 있는 판정 (여기 싣지 않음)** — mcptt_standard_conformance.md §0-R(미구현 로드맵)·C1·C2·C4g·C9·R2-1·R5 편차 ·
[mcptt_timers.md](../design/features/mcptt_timers.md) §7 D1~D10 · [server45_handoff.md](server45_handoff.md) §14(제휴 Expires 3600·구형 `Event: mcptt` PUBLISH·Warning 미노출)·§16 ·
[mcptt_emergency_modes.md](../design/features/mcptt_emergency_modes.md) 편차 표 · [mcptt_broadcast_group_call.md](../design/features/mcptt_broadcast_group_call.md) §7 ·
[mcx_identity_scope.md](../design/features/mcx_identity_scope.md) · [mcx_e2e_security.md](../design/features/mcx_e2e_security.md).

**급**

| 급 | 뜻 |
|---|---|
| **A** | 우리 단말·서버끼리도 오동작하거나, 인가·보안·운영에 구멍이 난다 |
| **B** | 규격 단말·규격 서버와 붙이면 그 절차가 성립하지 않는다 |
| **C** | 규격의 shall 과 다르지만 영향이 작거나 받아들이는 쪽이 넓은 것 |
| **D** | 권고(should·may)·정의 누락 |

**확인** — ◎ 규격 원문 줄과 코드 줄을 둘 다 직접 읽어 확인 · ○ 한쪽은 직접 읽고 다른 쪽은 감사 보고의 인용 · △ 실측이나 추가 원문 확인이 있어야 확정.

**대상** — CSP·CMP·CSC = 서버 · SDK = `sdk/core` · 현장 = `android/ptt-client` · 관제 = `windows/dispatch-desktop` + `android/dispatch-tablet`.

## 1. 요약

| 영역 | 항목 | A | B | C | D |
|---|---|---|---|---|---|
| 등록·서비스 인가 (REG) | 0 | — | — | — | — |
| 제휴 (AFF) | 10 | 6 | 4 | — | — |
| 그룹 호 — 서버 (GCS) | 11 | 7 | 2 | 2 | — |
| 개별 호 (PRV) | 1 | — | 1 | — | — |
| 애드혹 그룹 호 (ADH) | 1 | — | 1 | — | — |
| 긴급·임박·경보 (EMG) | 2 | 1 | 1 | — | — |
| 발언권 — 서버 (FCS) | 19 | 2 | 9 | 5 | 3 |
| 발언권 SDP 협상 (SDP) | 2 | — | 2 | — | — |
| 그룹 문서·GMS (GMS) | 6 | — | 6 | — | — |
| 설정 문서·CMS (CMS) | 2 | — | 1 | 1 | — |
| **계** | **54** | **16** | **27** | **8** | **3** |

확인 수준 — ◎ 28 · ○ 18 · △ 8.

읽는 순서 — §2(먼저 볼 것) → §3(영역별 전체) → §4(미구현 목록에 빠진 기능) → §5(문서 정정) → §6(묶음과 순서).

## 2. 먼저 볼 것

급 A 와, 급 B 가운데 한두 줄로 끝나면서 규격 단말과의 연동을 통째로 막는 것이다. 번호는 §3 의 항목 번호.

**한두 줄로 끝나는 것**

| 항목 | 내용 |
|---|---|
| FCS-21 | T2 제외 대상이 코드(긴급만)와 문서 세 곳(긴급·임박)이 다르다 |
| FCS-20 | Floor Granted 의 Duration 이 늘 T2 다 — T2 에서 빼 준 긴급 화자도 단말이 그 시각에 스스로 끊는다 |

**인가·보안 구멍**

| 항목 | 내용 |
|---|---|

**호 모델이 규격과 다른 것 (결정이 필요)**

| 항목 | 내용 |
|---|---|
| GCS-1·GCS-2 | chat 그룹 — 규격은 «초대 없이 각자 합류» 인데 서버가 제휴 멤버를 10초마다 INVITE 로 끌어들이고, 혼자 남아도 세션을 유지한다 |
| GCS-3·GCS-4 | 편성 그룹 호 — 다른 멤버가 합류할 때마다 빠져 있던 멤버를 다시 초대하면서, 정작 규격이 요구하는 «새로 제휴한 단말 초대(late call entry)» 는 하지 않는다 |
| AFF-5 | 제휴 행의 클라이언트 키가 경로마다 달라 암시적 제휴 그룹은 해제 PUBLISH 가 먹지 않는다 |

## 3. 영역별 목록

### 3.1 등록·서비스 인가 (REG) — TS 24.379 §7

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|

### 3.2 제휴 (AFF) — TS 24.379 §9

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| AFF-2 | A | CSP | §9.2.2.2.3 13)·15) — Expires 0 은 **그 클라이언트**의 목록만 해제 (shall) | `RemoveAffiliationsByUser` — 그 사용자의 모든 클라이언트 행(암시적 제휴 포함)을 지운다 — `csp/CscfModule.cpp:1934-1948` | 한 MCPTT ID 가 단말 둘이면 한 단말의 전체 해제가 다른 단말의 초대를 끊는다 | ◎ |
| AFF-4 | B | CSP | §6.3.6 3. · §9.2.2.2.11 2) — 제휴는 사용자 × **클라이언트**(mcptt-info `<mcptt-client-id>`) 단위로 판정 (shall) | `IsAffiliated(group, user)` 가 client_id 를 보지 않는다 — `csp/DbManager.cpp:1022-1038`. 개시 검사 `csp/GroupCallService.cpp:951`, fan-out `:1491`·`:1572` | 제휴하지 않은 단말로 개시·합류가 되고 초대가 간다(한 사용자 한 단말이면 증상 없음) | ◎ |
| AFF-5 | A | CSP | §9.2.2.2.12 1) · §9.2.2.2.15 2) · §9.2.2.2.3 10) — 암묵·암시·명시 제휴가 같은 client information entry 를 다룬다 | 행의 client 키가 경로마다 다르다 — 긴급·chat 암묵 제휴 = 등록 Contact URI(`csp/GroupCallService.cpp:954-957`), 설정 그룹 암시 제휴 = poc-settings PUBLISH 의 `<mcptt-client-id>`(`_ApplyImplicitAffiliations` — 서비스 인가 때, S25), 구형 PUBLISH = Contact, 규격형 = tuple id | 암시 제휴 그룹은 한 단말이 키 둘로 보일 수 있고(client ID ≠ tuple id 면), 해제 PUBLISH 가 자기 키 행만 지워 초대가 계속 온다 | △ |
| AFF-6 | A | CSP | §9.2.2.2.14 — 암묵적 제휴를 부른 요청이 거절되면 그 제휴를 지운다 (shall) | 세션 생성 전에 제휴를 적고(`csp/GroupCallService.cpp:953-960`) 뒤의 거절(세션 시간 창 403 `:1069-1085`·CMP 자원 실패·개시 게이트 480)에서 되돌리지 않는다 — 이 파일에 `RemoveAffiliation` 호출이 없다 | 거절된 긴급·chat 시도 뒤 3600초 동안 제휴로 남아 남의 그룹 호·경보·SDS 를 받는다 | ◎ |
| AFF-7 | A | CSP·CSC | §9.2.2.2.4 — 구독 동안 제휴 정보의 변화를 통지 (shall) | 통지는 PUBLISH·암시·암묵 제휴 때만. 시간 만료는 SQL 필터로만 사라지고, 멤버 제거는 호만 끊고 제휴 행을 남긴다(`csp/GroupCallService.cpp:2843-2886`), 그룹 삭제는 FK cascade | 구독 단말이 만료·멤버 제거·그룹 삭제로 내려간 제휴를 모른다. 제거된 멤버가 만료 전까지 «제휴» 로 남는다 | △ |
| AFF-8 | A | CSP | §9.2.1.2 — 제휴 변경은 PUBLISH. §9.2.2.3.3 5) — 멤버가 아니면 403 | `SUBSCRIBE`(Event presence, R-URI = 그룹)가 멤버십 검사 없이 제휴를 만들고(`csp/CscfModule.cpp:1463-1472`) 해지 때 지운다(`:1450-1455`) | 비멤버가 임의 그룹에 제휴 행을 만든다. 구독만 하려던 단말의 제휴가 바뀐다 | ◎ |
| AFF-9 | B | CSP | §9.2.1.3 5) · §10.1.3.2 6) — Expires 0 = 현재 상태 조회(fetch). RFC 6665 는 상태를 담은 NOTIFY 를 요구 | Expires 0 은 해지로만 처리 — 기존 구독이 있을 때만 본문 없는 종료 NOTIFY, 새 다이얼로그의 fetch 에는 NOTIFY 가 없다 — `csp/CscfModule.cpp:1422-1457`. 전 이벤트(제휴·conference·xcap-diff) 공통 | 조회 전용 SUBSCRIBE 가 결과를 못 받는다 | ◎ |
| AFF-10 | B | CSP | §9.2.1.6 · §9.2.2.3.9~10 — 그룹 동적 데이터 구독(mcptt-info `<mcptt-request-uri>` = 그룹 ID + simple-filter) | `Event: presence` 면 본문을 보지 않고 구독자 자신의 제휴 문서를 낸다 — `csp/CscfModule.cpp:1308-1322` | 규격 단말이 그룹 상태(긴급·임박·호 진행·제휴 멤버)를 구독하면 200 과 엉뚱한 문서를 받는다. 미구현 목록에도 없다(§4) | ◎ |
| AFF-11 | A | CSP·CSC | §6.3.5.5 — «shall only invite affiliated group members» | `require_affiliation=false` 그룹은 제휴하지 않은 멤버도 초대한다 — `csp/GroupCallService.cpp:1489-1494`. 스위치는 그룹 문서의 `<mcpttgi:on-network-require-affiliation>`(규격에 없는 요소, GMS-10) | 그 그룹에서는 해제한 멤버도 호·경보를 받는다. conformance C4g 는 이를 «정합» 으로 적었다 | ◎ |
| AFF-12 | B | SDK·현장·관제 | §9.2.1.1 · §9.2.1.3 — 단말은 제휴 상태 구독(presence)의 NOTIFY 로 제휴의 성공·거절을 안다 | 코어에 presence SUBSCRIBE·pidf NOTIFY 해석이 없다. 앱은 PUBLISH 2xx 를 제휴 확정으로 본다 — `sdk/core/src/engine.cpp:2936-2955`. ue_sdk.md §4 표의 `presence(uri)` API 는 없다 | 규격 서버의 200 은 접수일 뿐이다(거절·N2 축소는 NOTIFY 로만). 우리 서버에서도 서버 쪽 제휴 소실을 403 120 전까지 모른다 | ◎ |

### 3.3 그룹 호 — 서버 (GCS) — TS 24.379 §6.3 · §10.1

CSP 는 참여 기능과 제어 기능을 겸한다.

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| GCS-1 | A | CSP | TS 23.379 §10.6.2.3.1.2.1 — chat 그룹은 «초대 없이 각자 합류», 개설이 다른 멤버 초대로 이어지지 않는다. 이탈 = 단말 BYE(§10.1.2.2.3.1) | chat 합류 INVITE 뒤 나머지 제휴 멤버 전원에 INVITE(`csp/GroupCallService.cpp:1567-1579`), 10초 스윕이 등록·제휴 멤버를 계속 초대하고 세션이 없으면 서버가 만든다(`:2919-2985`). 서버가 연 세션의 `<mcptt-calling-user-id>` 는 그룹 ID 다 | 제휴만 해도 끌려 들어가고 BYE 로 나가도 다시 초대된다(나가려면 de-affiliate). 문서끼리도 어긋난다 — broadcast 문서 R4 «chat = 서버가 초대하지 않음» | ◎ |
| GCS-2 | A | CSP | §6.3.8.1 2) — chat 그룹 호도 참가자가 1명 이하면 해제 (shall) | 잔여 1 leg 해제는 on-demand(편성·애드혹)만 — `csp/GroupCallService.cpp:3327-3334`. chat 은 마지막 leg 뒤에도 CMP 그룹·세션을 남긴다 | 혼자 남은 단말이 세션에 묶인다(발언 요청은 Deny #3) | ◎ |
| GCS-3 | A | CSP | §10.1.1.4.2 — 멤버 초대는 14) «호가 진행 중이 아닐 때» 만. 15) 진행 중 합류는 r) 에서 끝난다 | 합류 수락 뒤 그대로 fan-out 루프로 내려가 leg 이 없는 제휴 멤버 전원에 INVITE — `csp/GroupCallService.cpp:1557-1579` (`fnAnswer` 가 0 을 돌려주는 일반 합류) | 스스로 나갔거나 거절한 멤버가 남의 합류 때마다 자동 응답 INVITE 로 다시 끌려온다. ptt_flows.md B6 «서버는 개시 시 fan-out 만» 과 다르다 | ◎ |
| GCS-4 | A | CSP | §10.1.1.4.6 — 새로 제휴했거나 통화권에 돌아온 단말을 진행 중 그룹 호에 초대 (shall) | 제휴 PUBLISH 경로는 기록·NOTIFY 만 한다. 주석은 «스윕이 한다» 고 적었지만(`csp/CscfModule.cpp:1678`) 스윕은 chat 이 아니면 초대 전에 돌아간다 — `csp/GroupCallService.cpp:2969-2972` | 호 도중 로그인·제휴한 규격 단말은 그 호에 못 들어간다. 우리 앱은 conference NOTIFY 를 보고 스스로 합류해 가려진다. ptt_flows.md 는 «late entry 는 UE 주도 = 규격 모델» 이라 적었으나 규격은 서버 초대다 | ◎ |
| GCS-12 | B | CSP | §6.3.3.4 — conference NOTIFY 에 P-Asserted-Identity(제어 기능 PSI)·P-Preferred-Service·mcptt-info 본문(`<mcptt-calling-group-id>`·`<mcptt-request-uri>`) (shall) | 헤더는 Event·Subscription-State·Contact 뿐, 본문은 conference-info 하나 — `csp/CspServer.cpp:1016-1039` | 규격 단말·참여 기능이 NOTIFY 를 그룹·대상 사용자에 묶을 근거가 없다 | ◎ |
| GCS-13 | C | CSP | §6.3.3.4 — `<conference-info entity>` = MCPTT group ID, `<user entity>` = MCPTT ID | `sip:<id>@<PTT 도메인>` — `csp/GroupCallService.cpp:3891-3905`. 같은 서버가 mcptt-info·pidf 에서는 `tel:` 표기를 쓴다 | ID 를 문자열로 대조하는 규격 단말은 로스터를 자기 목록과 맞추지 못한다 | ◎ |
| GCS-16 | B | CSP | §6.3.3.1.2 9)·10) — 받은 INVITE 의 Answer-Mode·Priv-Answer-Mode 를 그대로 옮긴다 (shall) | 초대 INVITE 에 무조건 `Answer-Mode: Auto` — `csp/GroupCallService.cpp:2506-2507` (개별 호 착신도 이 함수). 받은 헤더를 읽는 코드가 없다 | 헤더를 따르는 규격 단말은 개별 호를 벨 없이 자동 응답한다. 짝 = PRV-4 | ◎ |
| GCS-17 | A | CSP | §10.1.1.4.1.1 4)b) — `<mcptt-calling-group-id>` = 그룹 ID | `"tel:" + 그룹 id` — `csp/GroupCallService.cpp:3975`. MCPTT group ID 규칙은 숫자뿐이면 `tel:+<id>`(`csp/McpttInfo.h:400-408`, CSC `_group_uri`) | 숫자뿐인 그룹 ID 는 INVITE 의 그룹 ID 와 그룹 문서·제휴 문서의 ID 가 달라진다(`g001` 형은 무관) | ○ |
| GCS-18 | A | CSP·psip | §6.3.3.1.1 2) — 음성 스트림의 미디어 속성은 받은 offer 의 것 | 멤버 offer·개시자 answer 의 fmtp·ptime 이 서버 코덱 표 값(AMR-WB `octet-align=1`)이다 — `csp/GroupCallService.cpp:2407-2409`. 개시 게이트는 코덱 이름만 본다(`:885-893`) | 개시자가 대역 효율 모드나 다른 mode-set 으로 offer 하면 선언과 실제 페이로드가 어긋난다 — CMP 의 leg 별 형식 변환 유무를 확인해야 확정 | △ |
| GCS-21 | C | CSP | §6.3.8.1 2) — 참가자 1명 이하면 해제 | 초대 대상이 0 이어도(멤버 미등록·미제휴) 곧바로 200 — `csp/GroupCallService.cpp:1557-1558`. «잔여 1 leg» 판정은 leg 이 끝날 때만 돈다 | 혼자 연 호가 T4 까지 남는다(그동안 Deny #3) | △ |
| GCS-22 | A | CSP | §10.1.1.4.4.3 — 제휴 해제 등으로 단말을 세션에서 내보낼 때 BYE | 해제 PUBLISH 는 DB 삭제·NOTIFY 만 한다(`csp/CscfModule.cpp:1798-1802`·`:1934-1948`). `CheckMemberState` 는 멤버십만 본다 | 제휴를 해제한 단말이 그 호에 계속 남아 미디어를 받는다. ptt_flows.md 는 «de-affiliate 시 이탈» 이라 적었다 | ○ |

### 3.4 그룹 호 — 단말 (GCC) — TS 24.379 §6.2 · §10.1

남은 항목 없음 — 단말 그룹 호 요청(등록 태그·개시·chat·재합류·conference 구독)의 정본은 [ue_sdk.md](../design/features/ue_sdk.md) §4.2.

### 3.5 개별 호 (PRV) — TS 24.379 §11.1

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| PRV-7 | B | CSP | §11.1.1.4.2 — SDP 없는 응답(거절 최종 응답 등)은 본문째 개시자에게 | 개시자 180 의 PAI·착신 Warning, 거절 최종 응답의 멤버 Warning 은 옮긴다(S08). 멤버 응답의 본문(mcptt-info 등)은 옮기지 않는다 | 착신 측이 본문으로 준 정보가 발신 단말에 닿지 않는다 | ○ |

### 3.6 애드혹 그룹 호 (ADH) — TS 24.379 §17

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| ADH-8 | B | CSP | §17.4.2.1.1 — 멤버 초대의 `<session-type>adhoc` (Annex F.1) | SDK 개시는 `adhoc` 로 싣는다(명단을 실은 개시 — `sdk/core/src/engine.cpp` `startMcptt`). **CSP 멤버 INVITE 는 `prearranged`** — `csp/ModuleDispatcher.cpp` → `csp/GroupCallService.cpp` | 규격 단말이 초대받은 애드혹 호를 편성 그룹 호로 다룬다(그룹 문서를 찾는다) | ◎ |

### 3.7 긴급·임박 위험·경보 (EMG) — TS 24.379 §6.2.8 · §6.3.3.1.13~20 · §12

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| EMG-3 | A | CSP | §9.2.2.3.3 14) → §12.1.3.4 — 새로 제휴한 단말에 진행 중 경보를 MESSAGE 로 알린다 (shall) | 경보 캐시는 호 경로에서만 읽는다(`csp/GroupCallService.cpp:1327`·`:1344`·`:2002`). 제휴 경로는 NOTIFY 만 낸다(`csp/CscfModule.cpp:2028-2041`) | 경보 뒤에 로그인하거나 망 복귀로 제휴를 다시 실은 단말·관제석은 그 경보를 모른다 | ◎ |
| EMG-17 | B | CSP·CMP | TS 24.380 §6.3.5.3.9 · §6.3.5.4.8 — 긴급 격상 re-INVITE 의 `mc_implicit_request` 는 암묵적 발언 요청이다 | 암묵 요청은 새 세션 개시에서만 받는다 — `csp/GroupCallService.cpp:176-179`. 격상은 tier 만 바꾼다(`csp/CmpClient.cpp:874-885`) | 규격 단말은 격상 직후 Floor Granted 를 기다리지만 오지 않는다 | △ |

### 3.8 발언권 — 서버 (FCS) — TS 24.380 §6.3 · §8

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| FCS-4 | B | CMP·SDK | §8.2.3.14 — Floor Ack 의 Message Type 은 5비트 subtype 의 첫 비트를 0 으로 | 양쪽 다 ack 요구 비트를 넣어 싣는다 — `cmp/PMcpttGroup.cpp:1523-1524`, `sdk/core/src/floor/floor_participant.cpp:311`. 단위시험도 그 값을 기대한다(`tests/cmp_floor_codec_test.cpp:200-211`) | 규격 상대는 Ack 를 자기 메시지와 맞추지 못해 재전송한다. conformance F1/F2 · android_ue_client U1 은 정합으로 적었다 | ◎ |
| FCS-5 | B | CSP·CMP | §6.3.4.4.2 3)a)i · §8.2.3.8 — Granted Party's Identity·User ID = MCPTT ID(URI) | CMP 는 `user_uri` 가 없으면 가입자 번호를 싣는데(`cmp/PMcpttGroup.cpp:1180-1184`), CSP 의 MCPTT 합류 명령은 `user_uri` 를 보내지 않는다(MCVideo 경로만 — `csp/CmpClientMcvideo.cpp:113`) | Floor Taken 의 화자 신원이 번호 문자열이다. conformance F5 는 «`PTT_JOIN.user_uri` 로 받는다» 고 적었으나 MCPTT 경로는 늘 폴백이다 | ◎ |
| FCS-6 | B | CMP·CSP·SDK | §6.3.4.4.2 1)f — Audio SSRC 는 서버가 만든 전역 유일 값이고 발언자는 그 값을 RTP 에 쓴다(단말 §6.2.4.4.2 7. · `mc_ssrc` §14.4) | 필드 값 = 단말이 floor 헤더에 쓴 SSRC 를 되실은 것(`cmp/PMcpttGroup.cpp:991-995`·`:1239`). 하향 RTP 는 수신자별 SSRC 로 다시 쓴다(`:1845-1848`). SDK 는 Granted 의 SSRC·`mc_ssrc` 를 읽지 않는다 | Floor Taken·List of SSRCs 로 수신 스트림을 화자에 귀속할 수 없다(동시 발언에서 규격 단말은 화자 구분 불가) | ○ |
| FCS-7 | B | CMP | §6.3.5.4.6 2) · §6.3.5.3.8 — 발언권 없는 참가자의 RTP 에 Floor Revoke `#3`(No permission to send a Media Burst) | 중계만 하지 않는다(통지 없음) — `cmp/PMcpttGroup.cpp:663-665`. cause #3 상수가 없다 | 승인됐다고 오인했거나 강제 회수된 단말이 계속 송신해도 알 길이 없다(조용한 무음 발언) | ◎ |
| FCS-8 | B | CMP | §8.2.3.5 — 대기 중이 아니면 Queue Position Info = 254 | 미대기 = 0 을 싣는다. 우선순위도 대기 요청의 값이 아니라 멤버 기본값 — `cmp/PMcpttGroup.cpp:1560-1578` | 규격 단말이 «0번째 대기» 로 읽어 `U: queued` 로 들어간다 | ◎ |
| FCS-10 | B | CMP·SDK | §6.3.6.3.2 — dual floor 의 Floor Taken 은 Granted Party 하나 + G-bit + Audio SSRC. §6.3.6.3.6 — override 종료는 Floor Idle + G-bit. 단말 §6.2.4.5.8 — G-bit Taken 을 받은 발언자는 발언을 유지, §6.2.4.5.3 1.c — Release 에 G-bit | CMP 는 화자가 둘이면 multi-talker 형식(목록 필드, 0x0F 로 종료 통지)으로 보낸다 — `cmp/PMcpttGroup.cpp:1774-1784`. SDK 는 «목록에 내가 있나» 로만 판정하고 Release 에 G-bit 를 싣지 않는다(`floor_participant.cpp:372-378`·`:103`) | 사내끼리는 맞는다. 규격 상대와 dual floor 가 성립하지 않는다 | ○ |
| FCS-11 | C | CMP·CSP·SDK | §6.3.4.4.2 3)f — Floor Indicator 는 호 종류(긴급·임박·일제). 단말 §6.2.4.3.5 1.b — 임박 위험 호의 요청에 E-bit | CMP 는 멤버 tier 로 비트를 정하고 Floor Idle 은 늘 Normal(`cmp/PMcpttGroup.cpp:977-1003`). 받은 요청의 Indicator 는 쓰지 않는다(`:762-785`). SDK 는 임박 E-bit 를 싣지 않는다(`floor_participant.cpp:96`) | 긴급 호에서 일반 멤버 발언의 Taken·Idle 이 Normal 로 나간다. conformance F2 «tier 로 승격» 은 코드와 다르고 emergency_modes §10 과도 모순이다 | ◎ |
| FCS-12 | B | CMP·SDK | 표 8.2.2.1-1 — Floor Revoke Request(subtype 7), §6.3.5.4.15. Revoke cause `#7` | 미지 subtype 으로 버린다 — `cmp/PMcpttGroup.cpp:399-415`. 정의 정본(`mcptt_floor_defs.yaml`)에 subtype 7·cause 7·취소 결과 1·4·255 가 없다 | 관제 단말의 «남의 발언 회수» 요청이 사라진다. 미구현 목록에도 없다(§4) | ◎ |
| FCS-13 | B | CMP·CSP·CSC | §6.3.2.2 — audio cut-in 그룹은 새 요청마다 현 화자를 회수하고 큐잉이 없다(그룹 문서 `<mcptt-on-network-audio-cut-in>`) | cut-in 입력이 없다. 선점은 늘 서열 비교 — `cmp/PMcpttGroup.cpp:962-973` | 그 설정을 가진 그룹·단말과 동작이 다르다. 미구현 목록에 없다(§4) | ○ |
| FCS-14 | B | CMP·CSP·CSC | §6.3.4.3.3 1)b — 그룹 문서 `<on-network-recvonly>` 멤버는 Deny `#5`. §6.3.5.4.4 — 동시 발언은 `<multi-talker-allowed>` 멤버만 | Deny #5 는 청취 leg·일제 통화 비개시자뿐이다. csp·csc 에 수신 전용 멤버 속성이 없다 | 수신 전용 멤버를 둘 수 없고, 동시 발언 그룹은 누구나 동시에 말한다 | ○ |
| FCS-15 | C | CMP·SDK | §6.3.4.4.2 3)a)iii — Floor Taken 에 화자 위치. 단말 §6.2.4.3.5 1.c — Floor Request 에 Location(없으면 type 0 «Not provided») | 필드 19·20 정의가 양쪽에 없다 | 위치 관리 미구현(R4)과 별개로 메시지 필드가 빠진다 | ○ |
| FCS-16 | C | CMP·SDK | §8.1.1 — 한 IP 패킷에 발언권 메시지가 여럿 올 수 있다 | 양쪽 다 RTCP length 를 쓰지 않고 데이터그램 끝까지 한 메시지로 읽는다 — `cmp/PFloorCodec.cpp:104-131`, `sdk/core/src/floor/floor_codec.cpp:106-128` | 묶어 보내는 상대의 둘째 메시지가 버려진다 | ○ |
| FCS-17 | D | CMP | §6.3.5.2.2 — 합류 때 발언자가 없으면 Floor Idle (should) | 발언자가 있을 때의 Floor Taken 만 — `cmp/PMcpttGroup.cpp:285-293` | 늦게 합류한 단말이 유휴 상태와 순번 기준점을 모른다 | ◎ |
| FCS-18 | C | CMP | §6.3.4.4.6 5) — 동시 발언 그룹은 마지막 화자가 끝날 때도 Floor Release Multi Talker | 화자가 0 이 되면 Floor Idle 만 — `cmp/PMcpttGroup.cpp:1044-1054` | Idle 이 가므로 실해는 작다 | ○ |
| FCS-19 | C | CMP·CSC | §6.3.4.3.2·§6.3.4.4.9 — C7·C20 은 1 부터 세어 상한까지. 표 11.2.3-1 C7 기본 10 | 재송신 잔여 = 상한 그대로(첫 송신 포함 상한 + 1 회), C7 기본 3 — `cmp/PMcpttGroup.cpp:1622`·`:1226` | 재송신 1회 초과(무해). 타이머 문서 편차 표에 더할 것 | △ |
| FCS-20 | A | CMP | §8.2.3.3 — Duration = 그 발언자에게 허용된 시간(초) | 늘 T2 값을 싣는다 — `cmp/PMcpttGroup.cpp:1238`. T2 에서 빼 준 긴급 화자에게도 같은 값이 가고, T2 = 0(무제한)이면 0 이 간다 | 우리 SDK 는 Duration 마감 직전에 스스로 Release 한다 — 긴급 화자가 T2 에 스스로 끊는다(제외 정책이 무력). 규격 단말은 0 을 «0초» 로 읽을 수 있다 | ○ |
| FCS-21 | A | CMP·문서 | §6.3.4.4.5 — T2 는 발언자마다(제외는 로컬 정책) | 코드는 긴급 tier 만 제외(`tierOf(owner) < TIER_EMERGENCY` — `cmp/PMcpttGroup.cpp:1724`)라 임박 화자는 T2 에 회수된다 | conformance F4·cmp_media_api §7.7·mcptt_timers §5.2 는 «긴급·임박 제외», emergency_modes §3.1 은 «임박은 적용» — 문서끼리 모순 | ◎ |
| FCS-23 | D | CMP·CSP | §6.3.4.4.2 3)a)i — 화자 신원은 «privacy 를 요청하지 않았을 때» | 조건 없이 싣는다. 합류 명령에 privacy 입력이 없다 | CSP 가 privacy 요청 절차를 받는지부터 확인 필요 | △ |
| FCS-24 | D | CMP | §8.2.15 — Queued Floor Requests 결과 메시지의 요청자 신원·목록 부호화 | Cancel Result 에 신원 필드가 없고 목록 항목 길이를 1옥텟으로 다룬다 — `cmp/PMcpttGroup.cpp:1451-1457`·`:1487-1496` | 규격 그림(PDF 원본)으로 필드 폭을 다시 확인할 것 | △ |

### 3.9 발언권 — 단말 (FCC) — TS 24.380 §6.2

메시지 형식의 단말 쪽 편차는 서버와 같은 뿌리라 §3.8 에 함께 적었다(FCS-4·6·10·11·12·15·16).

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|

### 3.10 발언권 SDP 협상 (SDP) — TS 24.380 §4.3 · §14 · TS 24.379 §6.2.1 · §6.3.3.1.1

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| SDP-1 | B | CSP | TS 24.380 표 4.3.3.1-1 — `m=application <port> udp MCPTT` (proto = "udp") | CSP offer 가 `UDP MCPTT` + 규격에 없는 `a=floorid:0 mstrm:audio`(`csp/GroupCallService.cpp` floor SDP · psip `CSipDialog::AddSdp`). SDK 는 규격 표기다(U04) | proto 를 대소문자 구분으로 대조하는 상대는 제어 채널을 못 알아본다 | ◎ |
| SDP-2 | B | SDK·CSP·CMP | §4.3.3.1 · §14.2.7 · §14.3.8 — `mc_floor_ssrc`(다중화를 지원하면 필수): 상대는 그 값을 floor 메시지의 RTCP 헤더 SSRC 로 쓴다 | 레포 어디에도 `mc_floor_ssrc`·`mc_ssrc` 처리가 없다. 단말 헤더 SSRC = MCPTT ID 해시(`sdk/core/src/engine.cpp:160-163`), 서버 = 그룹당 순번(`cmp/PMcpttGroup.cpp:67`) | 한 포트에 여러 세션의 제어 채널을 다중화하는 상대는 메시지를 세션에 못 묶는다 | ◎ |

### 3.11 그룹 문서·GMS (GMS) — TS 24.481

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| GMS-2 | B | SDK | §6.3.2.2.1 — 생성 PUT 의 XUI = group creation XUI. §6.3.2.2.2 — 409 `<uniqueness-failure>` 의 `<alt-value>` 로 다시 | SDK 는 본인 XUI 의 tree 에 클라이언트가 정한 ID 로 PUT 한다(`sdk/core/include/cimsue/csc.h` `groupPath`). CSC 는 생성 XUI 경로와 alt-value 를 낸다(`_gms_create_by_creation_xui`) | 우리 SDK 는 규격 GMS 에 그룹을 만들 수 없다(우리 CSC 와는 동작) | ○ |
| GMS-3 | B | CSC | §6.3.4.2.1 → RFC 4825 §7.1 — PUT 은 문서를 **교체**한다 | 준 필드만 UPDATE 하고 `<list>` 가 없으면 멤버를 유지한다 — `mcptt.py:2687-2697` | 요소를 지워 기본값으로 되돌려도 반영되지 않고 200 이 온다. mcptt_api.md 는 이를 동작으로만 적었다 | ○ |
| GMS-8 | B | CSC·SDK·관제 | §7.2.8 — 요소가 없을 때: `on-network-invite-members` = false(chat), `allow-MCPTT-emergency-alert` = false, `on-network-allow-conference-state` = false, group-priority = 최저 | XCAP 생성 기본값 = prearranged·경보 허용·conference 허용·우선순위 5(`mcptt.py:2633-2640`). SDK 구조체 기본 = prearranged·긴급 호/경보 허용(`sdk/core/include/cimsue/csc.h:145-148`) | 요소를 생략한 규격 문서가 반대 뜻으로 만들어진다(권한 확대). SDK 는 규격 GMS 문서를 반대로 읽는다. mcptt_api.md 의 «conference-state 기본 true» 는 규격과 반대다 | ◎ |
| GMS-14 | B | CSP | §6.3.13.3.2.2 — xcap-diff SUBSCRIBE 의 신원은 mcptt-info `<mcptt-access-token>`, 구독 대상은 resource-lists. RFC 5875 §4.6 — NOTIFY 의 `sel` 은 구독한 URI 와 같아야 한다 | (본문 resource-lists 는 읽는다 — 분류·`sel` = 구독한 entry, S19.) 신원 = From(등록·Digest)이고 `<mcptt-access-token>` 은 읽지 않는다 — 토큰 검증 경로가 CSP 에 없다 | 토큰 없이도 구독이 선다 · 토큰의 MCPTT ID 와 SIP 신원이 다른 구성(규격 단말)에서 인가 대상이 어긋난다 | ○ |
| GMS-15 | B | CSP·CSC | RFC 5874 — `new-etag` = 변경 뒤 문서의 ETag, 삭제는 `previous-etag` 만. RFC 5875 §4.7 — 앞 NOTIFY 의 200 전에 다음 NOTIFY 를 보내지 않는다 | `new-etag` 가 `init`·`etag_<gid>`·`change_<ts>`·빈 값이고 HTTP ETag(내용 해시)와 다르다(삭제 통지는 `previous-etag` 만 — S19). 그룹마다 NOTIFY 를 연달아 보낸다(앞 NOTIFY 의 200 을 기다리지 않는다) — `csp/CspServer.cpp` `BuildXcapDiffBody`·`SendInitialNotify`·`SendGroupDocNotify` | `new-etag` 를 캐시와 견주는 단말은 늘 불일치이거나 «변경 없음» 으로 읽는다 | ○ |
| GMS-16 | B | 관제 | §6.3.13.2.1 — 단말의 구독: resource-lists 본문 · mcptt-info 의 access token · P-Preferred-Service · Contact icsi-ref · Request-URI = 설정된 PSI | SDK(`Engine::subscribeXcapDiff(…, XcapDiffSubscription, …)` — 엔진 구독 경로가 본문·헤더를 싣는다)·현장 앱은 규격형으로 구독한다. **관제 앱 두 벌은 본문 없는 구독**이고 PSI 가 `sip:gms_psi@<도메인>` 고정이다(`DispatchSession.cs`, `DiscoveryPlane.kt`) — 바인딩은 있다(C API `cimsue_engine_subscribe_xcap_diff_documents`·.NET `Account.SubscribeXcapDiff(psi, documents, token, on)`·`UeInitConfigDoc.GmsUri`) | 규격 GMS·CMS 는 관제 앱의 구독을 받지 않거나 무엇을 통지할지 모른다 | ○ |

### 3.12 설정 문서·CMS (CMS) — TS 24.484

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| CMS-5 | B | CSP | §8.3.2.12 — user profile 문서의 변경을 구독자에게 통지 | (그룹 멤버십 변경은 CSP 가 멤버의 cms 구독에 user profile 통지를 낸다 — S19.) `UserProfile.*` 설정 재적재는 CSC 가 `USER_PROFILE_CONFIG_CHANGED`(uri·etag 없음)를 낸다(`csc/src/services/mcptt.py` `apply_config`). **CSP 가 이 이벤트를 받지 않는다** — cms 구독자 전원에게 자기 user profile 문서의 sel 을 통지해야 한다 | 운영자가 user profile 기본값(`UserProfile.*`)을 바꾸면 구독한 단말이 재로그인 전까지 모른다 | ○ |
| CMS-13 | C | 관제 | §5.1 · §4.2.2.1.1 — 단말은 설정 문서의 변경을 구독한다 | Windows 관제 앱은 gms 축만 구독하고 CMS 는 5분 폴링 — `DispatchSession.cs:515-556` | 인가 변경이 최대 5분 늦게 버튼에 반영된다 | ○ |

### 3.13 신원 관리 (IDM) — TS 24.482 · TS 33.180 부록 B

남은 항목 없음 — 정본 [mcx_identity_scope.md](../design/features/mcx_identity_scope.md).

## 4. 미구현 기능 목록에 빠진 것

통째 미구현 기능은 mcptt_standard_conformance.md §0-R(R1·R2·R4-2)에 있다 — 이 목록을 만들며 찾은 것도 그 표로 옮겼다.

## 5. 문서 정정

문서가 «정합» 이라고 적었거나 사실로 적은 것이 코드·규격과 다른 곳이다. 코드를 고치든 편차로 남기든, 문서의 서술은 바로잡아야 한다.

| 문서 | 적힌 것 | 실제 | 항목 |
|---|---|---|---|
| mcptt_standard_conformance.md C1 | `Expires: 0` = 그 사용자의 제휴 전부 해제 | 규격은 그 클라이언트의 것만 | AFF-2 |
| 같은 문서 C4g | «그 밖의 그룹은 멤버십이 곧 affiliation» | 규격은 제휴 멤버만 초대 | AFF-11 |
| `csp/CscfModule.cpp:764` 주석 | 암시적 제휴 근거 «§7.3.2 13)» | 그 단계는 §7.3.3·§7.3.4 에 있다 — 문서(mcptt_standard_conformance.md C9)와 CSC 주석은 고쳤다. 주석은 그 파일을 고치는 WP 가 | — |
| 같은 문서 F1/F2 | Floor Ack = Source + Message Type 정합 · 받은 Indicator 의 긴급·임박 비트는 tier 로 승격 | Message Type 에 ack 비트가 섞인다 · 받은 Indicator 는 쓰지 않는다 | FCS-4·FCS-11 |
| 같은 문서 F4 · cmp_media_api.md §7.7 · mcptt_timers.md §5.2 | T2 에서 긴급·임박 화자 제외 | 코드는 긴급만(emergency_modes §3.1 과는 일치) | FCS-21 |
| 같은 문서 F5 | MCPTT ID 는 `PTT_JOIN.user_uri` | MCPTT 경로는 `user_uri` 를 보내지 않는다 | FCS-5 |
| 같은 문서 §0 S3 | xcap-diff SUBSCRIBE/NOTIFY 정합 | 신원 = From(토큰 미검증)·`new-etag` 불일치 | GMS-14·GMS-15 |
| ptt_flows.md B4·B6 | 제휴 PUBLISH 도식의 `Event: poc-settings` · late entry 는 «UE 주도 = 규격 모델» · «서버는 개시 시 fan-out 만» · «de-affiliate 시 이탈» | poc-settings 는 서비스 인가·설정(제휴는 `Event: presence`) · 규격은 서버 초대 · 합류 때마다 재초대 · 해제해도 leg 유지 | GCS-3·GCS-4·GCS-22 |
| mcptt_broadcast_group_call.md R4 | chat = 서버가 초대하지 않음 | 서버가 10초마다 초대 | GCS-1 |
| android_ue_client.md U1 | Floor Ack 정합 | Ack 의 Message Type 에 ack 요구 비트가 섞인다 | FCS-4 |
| ue_sdk.md §4 | API 표의 `presence(uri)` | 그런 API 가 없다 | AFF-12 |
| mcptt_api.md | `on-network-allow-conference-state` 기본 true | 규격 기본 false | GMS-8 |

## 6. 묶음과 순서 (권고)

같은 자리를 고치는 것끼리 묶었다. 앞 묶음일수록 손이 적게 들고 영향이 크다.

| # | 묶음 | 항목 | 몫 |
|---|---|---|---|
| 1 | **문서 값 한두 줄** — 문서 셋의 T2 제외 대상 | FCS-21(문서) | .48 |
| 2 | **발언권 메시지 정합** — Ack 의 Message Type · 미대기 Queue Position 254 · Granted Duration | FCS-4 · FCS-8 · FCS-20 | .45 SDK·CMP |
| 5 | **Warning 코드** — 거절 사유를 규격 코드로. handoff §14 K1(SDK 가 Warning 을 올림)과 한 묶음 | PRV-7(본문) | .45 CSP·SDK → Windows(문구 사전) |
| 7 | **SDK 요청 규격화(남은 것)** — 애드혹 멤버 INVITE, xcap-diff 구독 본문. 등록 태그·그룹/개별 호·MCData 요청·재합류(세션 식별자)·conference 구독(세션 식별자 — S27 의 짝)은 규격형이 됐다(S17 + U04·U05 묶음 A~C · GCC-8 · GCC-7) — 그룹 호 Request-URI 검사(GCS-9·GCS-10)는 S18 로 켰다 | ADH-8(CSP 멤버 INVITE) · GMS-16 · SDP-1 | .45 SDK·CSP |
| 8 | **제휴를 클라이언트 단위로** — 행 키 통일, 클라이언트 단위 해제·판정, 암묵 제휴 취소. handoff §14 K3·S2 와 한 묶음 | AFF-2~AFF-8 · AFF-12 · EMG-3 | .45 CSP·SDK |
| 9 | **호 모델 결정** — chat 그룹(초대 없이 합류·1명 이하 해제)과 편성 그룹의 재초대·late call entry·제휴 해제 시 이탈. 규격대로 바꿀지, 편차로 남기고 사유를 적을지 정한다 | GCS-1~GCS-4 · GCS-22 · AFF-11 | 결정 → .45 CSP |
| 10 | **그룹 문서 집행** — 수신 전용 | FCS-14 | .45 CSP·CMP·CSC |
| 11 | **XCAP 규격 주소·절차** — global tree·문서 이름·교체 PUT·오류 형식·구독 본문·etag | GMS-3 · GMS-14~GMS-16 | .45 CSC·CSP·SDK |
| 12 | **발언권 메시지 형식** — MCPTT ID·Audio SSRC·Indicator·dual floor·Location·Revoke Request, SDP `mc_floor_ssrc` | FCS-5~FCS-7 · FCS-10~FCS-16 · SDP-2 | .45 CMP·CSP·SDK |

**Windows 몫(관제 앱 두 벌)** — SDK·서버가 정해진 뒤 맞춘다.

- 착신 거절 — 코어가 MC 서비스 초대의 `reject()`(0·480·486·603)·받기 전 `hangup()` 을 480 + Warning 110 으로 낸다(GCC-6 반영 — 앱 코드 변경 없음). 문구 사전에 110 을.
- EMG-8 — 긴급 개시·상향·경보의 대상 그룹 판정(전용 긴급 그룹 — TS 24.379 §6.2.8.1.8 1)a) · §12.1.1.1 4)a)i). 현장 앱은 `EmergencyRules.targetGroup` 으로 반영).
- GMS-11·GMS-18 — 그룹 편집 폼: 정원 0(무제한)을 되돌릴 수 있게(서버는 0 이면 요소를 싣지 않는다), 우선순위 0~255(서버가 범위 밖 400 — 앱의 0~15 절단을 없앤다).
- CMS-13 — CMS 변경 구독(지금은 5분 폴링).
- 묶음 4·5 가 들어오면 Capabilities 게이트와 응답 문구 사전.

## 7. 보지 못한 것

- **MCData(TS 24.282)·MCVideo(TS 24.281·24.581)** — 각자의 목록: [mcdata_conformance_gaps.md](mcdata_conformance_gaps.md) · [mcvideo_conformance_gaps.md](mcvideo_conformance_gaps.md).
- **통째 미구현 절** — pre-established session, call-back, first-to-answer, 원격 개시, ambient listening, regroup, functional alias, MBMS, off-network, 위치 관리, 긴급 개별 호의 단말 절차 세부, 애드혹 긴급·임박.
- **floor SRTCP(TS 24.380 §13)·KMS(TS 33.180 본문)** — placeholder 로 문서에 있어 내부를 읽지 않았다. 토큰 교환·파트너 도메인(부록 B.7~B.9)도.
- **OMA XDM Group·RFC 원문 일부** — 그룹 문서의 OMA 스키마 시퀀스, RFC 6665·OIDC Core 의 해당 절은 규격 폴더에 없어 대조하지 못했다(AFF-9 의 근거 일부).
- **실행 확인** — 모든 항목이 코드 읽기다. △ 표시 항목과 AFF-5(제휴 행 키)는 실서버로 재현해 확정한다.
- **cspsim·계측기(libcsim)** — 시험 도구의 MCPTT 송신 형태는 보지 않았다. 서버는 규격 형식만 받으므로(결정 K4 — 등록 없는 요청 404 141·Accept-Contact 403·session-type 404 117/118) 도구도 함께 맞춰야 한다 — 개별 호(PRV-1)는 S17 부터 규격형만 받는데 도구에는 개별 호 경로가 없다.
- **콘솔·현장 앱 화면 규칙** — 인가 요소가 늘면(묶음 4) 콘솔 가입자 프로파일 칸이 따라가야 한다.
