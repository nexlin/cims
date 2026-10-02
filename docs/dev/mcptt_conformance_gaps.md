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
| 등록·서비스 인가 (REG) | 4 | — | 3 | — | 1 |
| 제휴 (AFF) | 11 | 6 | 4 | 1 | — |
| 그룹 호 — 서버 (GCS) | 22 | 9 | 7 | 5 | 1 |
| 그룹 호 — 단말 (GCC) | 10 | — | 8 | — | 2 |
| 개별 호 (PRV) | 9 | 4 | 4 | 1 | — |
| 애드혹 그룹 호 (ADH) | 10 | 4 | 4 | 1 | 1 |
| 긴급·임박·경보 (EMG) | 17 | 6 | 3 | 8 | — |
| 발언권 — 서버 (FCS) | 20 | 2 | 9 | 6 | 3 |
| 발언권 — 단말 (FCC) | 1 | — | — | 1 | — |
| 발언권 SDP 협상 (SDP) | 3 | 1 | 2 | — | — |
| 그룹 문서·GMS (GMS) | 18 | 3 | 13 | 2 | — |
| 설정 문서·CMS (CMS) | 13 | 2 | 5 | 5 | 1 |
| 신원 관리 (IDM) | 9 | 4 | 1 | 3 | 1 |
| **계** | **147** | **41** | **63** | **33** | **10** |

확인 수준 — ◎ 80 · ○ 53 · △ 14.

읽는 순서 — §2(먼저 볼 것) → §3(영역별 전체) → §4(미구현 목록에 빠진 기능) → §5(문서 정정) → §6(묶음과 순서).

## 2. 먼저 볼 것

급 A 와, 급 B 가운데 한두 줄로 끝나면서 규격 단말과의 연동을 통째로 막는 것이다. 번호는 §3 의 항목 번호.

**한두 줄로 끝나는 것**

| 항목 | 내용 |
|---|---|
| CMS-4 | MCPTT service configuration 에 `<signalling-protection>` 이 없다 — 규격 기본값이 «켜짐» 이라 규격 단말은 mcptt-info 를 암호화·서명해 보낸다. `false` 두 줄을 명시하면 된다(MCVideo 문서는 이미 그렇게 한다) |
| GMS-7 | 그룹 문서에 `<protect-media>`·`<protect-floor-control-signalling>` 이 없다 — 기본값이 «GMK 필수» 다. `false` 명시 |
| GMS-13 | 그룹 문서 산출에 XML 이스케이프가 없다 — 그룹·멤버 이름에 `&`·`<` 가 있으면 문서가 깨진다 |
| GMS-11 | 그룹 문서가 정원 0(무제한)을 10 으로 내고, 관제 앱이 그룹을 저장하면 그 10 이 DB 에 굳는다 → MCVideo 호가 10명에서 486 |
| CMS-7 | UE initial configuration 의 PLMN 유도가 MNC 앞자리 0 을 전부 지운다(`mnc008` → 무효 PLMN) |
| FCS-21 | T2 제외 대상이 코드(긴급만)와 문서 세 곳(긴급·임박)이 다르다 |
| FCS-20 | Floor Granted 의 Duration 이 늘 T2 다 — T2 에서 빼 준 긴급 화자도 단말이 그 시각에 스스로 끊는다 |

**인가·보안 구멍**

| 항목 | 내용 |
|---|---|
| EMG-1 | 긴급 경보 MESSAGE 가 발신자의 그룹 멤버십을 보지 않는다 — 비멤버가 임의 그룹에 경보를 내고 남의 경보를 취소할 수 있다 |
| PRV-2 | 개별 호 발신 인가(`<allow-private-call>`)가 문서에도 서버 판정에도 없다 — 개별 호를 막을 설정이 없다 |
| SDP-3 | offer 의 `mc_priority` 를 상한 없이 받아 Floor Priority 로 선점 서열을 올릴 수 있다 |
| IDM-1 | PTT 가입이 없는 전화 계정에도 MC scope 토큰·MCPTT user profile·KMS 키가 나간다 |
| IDM-3·IDM-4 | 클라이언트 등록·`redirect_uri` 필수 검사가 없다(PKCE 만이 방어선) |
| ADH-3 | 애드혹 그룹 호 참가자 수 상한이 없다 — INVITE 한 건으로 전원 fan-out |

**호 모델이 규격과 다른 것 (결정이 필요)**

| 항목 | 내용 |
|---|---|
| GCS-1·GCS-2 | chat 그룹 — 규격은 «초대 없이 각자 합류» 인데 서버가 제휴 멤버를 10초마다 INVITE 로 끌어들이고, 혼자 남아도 세션을 유지한다 |
| GCS-3·GCS-4 | 편성 그룹 호 — 다른 멤버가 합류할 때마다 빠져 있던 멤버를 다시 초대하면서, 정작 규격이 요구하는 «새로 제휴한 단말 초대(late call entry)» 는 하지 않는다 |
| AFF-5 | 제휴 행의 클라이언트 키가 경로마다 달라 암시적 제휴 그룹은 해제 PUBLISH 가 먹지 않는다 |
| PRV-3 | 전이중 개별 호(floor 없음)의 SDP 표기가 SDK·CSP·규격 세 쪽이 서로 다르다 |

## 3. 영역별 목록

### 3.1 등록·서비스 인가 (REG) — TS 24.379 §7

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| REG-1 | B | SDK | §7.2.1 — REGISTER Contact 에 `g.3gpp.mcptt` 와 MCPTT ICSI 의 `g.3gpp.icsi-ref` (shall) | Contact 태그는 `+sip.instance`·mcvideo·mcdata.sds 뿐이고 앱 세 벌 모두 `contactParams` 를 주지 않는다 — `sdk/core/src/account_map.cpp:88-113` | 규격 IMS 코어의 iFC 가 MCPTT 서버로 넘기지 않고, 서버 fan-out 의 `Accept-Contact …require;explicit` 과 맞지 않아 착신이 오지 않는다. handoff §12.6 C12 의 «앱은 싣는다» 는 틀렸다 | ◎ |
| REG-2 | B | CSP·SDK | §7.3.3~§7.3.6 · §7.2.2~§7.2.4 — `Event: poc-settings` PUBLISH(서비스 인가·서비스 설정·Expires 0 로그오프)와 SUBSCRIBE 를 받는다 (shall) | PUBLISH 는 Event 가 `mcptt`·`presence` 밖이면 489(`csp/CscfModule.cpp:1697-1703`), SUBSCRIBE 도 489(`:1353-1361`). SDK 는 보내지 않는다 | 규격 단말은 489 를 «인가 실패» 로 본다. Answer-Mode·선택한 user profile index 가 서버에 닿지 않는다. 우리 SDK 는 규격 서버에서 착신 480(146) | ◎ |
| REG-3 | B | CSP | §10.1.1.3.1.1 2a) 등 — 서비스 인가 바인딩이 없으면 404 + Warning `141 user unknown to the participating function` (shall) | 등록이 없어도 가입자면 Digest 만으로 INVITE·PUBLISH·SUBSCRIBE 를 처리한다 — `csp/ModuleDispatcher.cpp:740-772`, `csp/CscfModule.cpp:1686-1691`. 141 문구가 CSP 에 없다 | 규격 단말이 재인가를 시작할 신호가 없다 | ○ |
| REG-4 | D | CSP | §7.1 · §7.2.1 NOTE 1 — MCPTT 태그를 뺀 재-REGISTER 는 MCPTT 로그오프다 | MCPTT 사용자 여부 = 가입 종류(`csp/CscfModule.cpp:1194`). Contact 태그로 가르는 것은 MCData·MCVideo 뿐(`csp/UserMap.cpp:193-206`) | MCVideo·MCData 를 남기고 MCPTT 만 로그오프할 수 없다 | ○ |

### 3.2 제휴 (AFF) — TS 24.379 §9

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| AFF-2 | A | CSP | §9.2.2.2.3 13)·15) — Expires 0 은 **그 클라이언트**의 목록만 해제 (shall) | `RemoveAffiliationsByUser` — 그 사용자의 모든 클라이언트 행(암시적 제휴 포함)을 지운다 — `csp/CscfModule.cpp:1934-1948` | 한 MCPTT ID 가 단말 둘이면 한 단말의 전체 해제가 다른 단말의 초대를 끊는다 | ◎ |
| AFF-3 | C | CSP | §9.2.2.2.3 8)a) — 200 OK 에 Expires (shall) | 해제(Expires 0)·entity 불일치의 200 에 Expires 가 없다 — `csp/CscfModule.cpp:1947`·`:1962`, 구형 `:1827` | RFC 3903 게시자가 해제 확인을 못 한다(우리 SDK 는 읽지 않는다) | ◎ |
| AFF-4 | B | CSP | §6.3.6 3. · §9.2.2.2.11 2) — 제휴는 사용자 × **클라이언트**(mcptt-info `<mcptt-client-id>`) 단위로 판정 (shall) | `IsAffiliated(group, user)` 가 client_id 를 보지 않는다 — `csp/DbManager.cpp:1022-1038`. 개시 검사 `csp/GroupCallService.cpp:951`, fan-out `:1491`·`:1572` | 제휴하지 않은 단말로 개시·합류가 되고 초대가 간다(한 사용자 한 단말이면 증상 없음) | ◎ |
| AFF-5 | A | CSP | §9.2.2.2.12 1) · §9.2.2.2.15 2) · §9.2.2.2.3 10) — 암묵·암시·명시 제휴가 같은 client information entry 를 다룬다 | 행의 client 키가 경로마다 다르다 — 긴급·chat 암묵 제휴 = 등록 Contact URI(`csp/GroupCallService.cpp:954-957`), 설정 그룹 암시 제휴 = client-id > `+sip.instance` > Contact(`csp/CscfModule.cpp:756-793`), 구형 PUBLISH = Contact(`:1799`), 규격형 = tuple id(`:1989`) | 암시 제휴 그룹은 한 단말이 tuple 둘로 보이고, 해제 PUBLISH 가 자기 키 행만 지워 초대가 계속 온다. 지워져도 재등록 때 되살아난다(`:1194`) | △ |
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
| GCS-5 | A | CSP | §6.3.5.5 — 초대는 `<on-network-max-participant-count>` 까지(필수 멤버 우선). §10.1.1.4.2 15)d) — 정원이 찼으면 486 + `122 too many participants` (shall) | `_maxMembers` 는 읽기만 하고 MCPTT 호에서 쓰지 않는다(`csp/GroupCallService.cpp:2743`·`4021` — 지문·이력). 집행은 MCVideo 만. 그룹 문서 쪽 값 문제는 GMS-11 | 그룹 문서가 알린 정원과 무관하게 전원이 들어온다 | ◎ |
| GCS-6 | B | CSP·CSC | §10.1.1.3.1.1 5) · §17.3.2.1.1 6) — `<MaxSimultaneousCallsN6>` 를 넘으면 486 + `103 maximum simultaneous MCPTT group calls reached` (shall) | N6·103 검사는 MCVideo 에만 있다(`csp/McVideoCallService.cpp:638-644`). CSC 는 MCPTT user profile 에 N6 = 1 을 싣는다(`csc/src/services/mcptt.py:1713`) | 문서는 «동시 1호», 서버는 무제한(다중 채널 동시 참여가 전제). 문서를 따르는 규격 단말은 둘째 그룹 호를 스스로 막는다 | ◎ |
| GCS-7 | B | CSP | §6.3.5.2 2) — 그룹이 없으면 404 + `113 group document does not exist` (shall) | Warning 없는 403 — `csp/ModuleDispatcher.cpp:1090-1097` (`RejectPtt`) | 단말이 «권한 없음» 과 «그룹 없음(삭제·오설정)» 을 가르지 못한다 | ◎ |
| GCS-8 | B | CSP | §6.3.5.2 5)b) — 비멤버 403 + `116 user is not part of the MCPTT group`. 재합류는 `121` (§10.1.1.4.5.1 6)) | Warning 없는 403 — `csp/GroupCallService.cpp:873-880` | 403 의 사유(비멤버·미제휴 120·긴급 미인가)를 가를 수 없다. SDK 가 Warning 을 올려도(handoff §14 K1) 116 은 오지 않는다 | ◎ |
| GCS-9 | B | CSP | §6.3.5.2 5)c)·d) — `<session-type>` 이 그룹 종류와 다르면 404 + `117`(편성 그룹)·`118`(chat 그룹) (shall) | session-type 은 `private` 분기에만 쓴다. 그룹 경로는 값을 넘기지 않는다 — `csp/ModuleDispatcher.cpp:1053-1069` | 그룹 문서가 낡은 단말을 바로잡을 기회가 없다. 짝 = GCC-4(SDK 는 늘 prearranged) | ○ |
| GCS-10 | C | CSP | §10.1.1.4.2 3) — Accept-Contact 에 `g.3gpp.mcptt`·MCPTT icsi-ref 가 없으면 403 (chat 은 Contact `isfocus` 도) | MCPTT 경로에 검사가 없다(MCVideo 만 — `csp/McVideoCallService.cpp:667-675`) | 받아들이는 쪽이 넓다. 우리 SDK 가 헤더를 싣지 않아(GCC-1) 지금 켜면 우리 단말이 막힌다 — GCC-1 뒤에 | ○ |
| GCS-11 | C | CSP | §10.1.1.4.2 15)j) — 진행 중 호 합류의 200 OK 에 Warning `123 MCPTT session already exists` (shall) | 합류는 Warning 없이 답한다 — `csp/GroupCallService.cpp:1558`·`:1227` | 단말이 «내가 연 호» 와 «진행 중이던 호» 를 200 으로 가르지 못한다 | ○ |
| GCS-12 | B | CSP | §6.3.3.4 — conference NOTIFY 에 P-Asserted-Identity(제어 기능 PSI)·P-Preferred-Service·mcptt-info 본문(`<mcptt-calling-group-id>`·`<mcptt-request-uri>`) (shall) | 헤더는 Event·Subscription-State·Contact 뿐, 본문은 conference-info 하나 — `csp/CspServer.cpp:1016-1039` | 규격 단말·참여 기능이 NOTIFY 를 그룹·대상 사용자에 묶을 근거가 없다 | ◎ |
| GCS-13 | C | CSP | §6.3.3.4 — `<conference-info entity>` = MCPTT group ID, `<user entity>` = MCPTT ID | `sip:<id>@<PTT 도메인>` — `csp/GroupCallService.cpp:3891-3905`. 같은 서버가 mcptt-info·pidf 에서는 `tel:` 표기를 쓴다 | ID 를 문자열로 대조하는 규격 단말은 로스터를 자기 목록과 맞추지 못한다 | ◎ |
| GCS-14 | B | CSP | §10.1.3.3 2) — Request-URI 가 진행 중 세션 식별자가 아니면 404 + `137 the indicated group call does not exist`. 구독자 = 그 세션의 참가자. 200 OK Contact = 세션 식별자 | R-URI user = 그룹 ID 로만 읽는다(`gr` 토큰·세션 유무·참가 여부를 보지 않음) — `csp/CscfModule.cpp:1303-1344`. 세션이 끝나도 구독이 남는다 | 지난 세션 식별자로 온 구독이 다음 세션 로스터에 붙는다. ptt_flows.md 는 «구독은 참여보다 오래 산다» 를 설계로 적었다 — 편차로 올릴 것 | ○ |
| GCS-15 | C | CSP·psip | §6.3.2.2.3 5)·6) — 멤버 INVITE 의 `Supported` 에 `tdialog`·`norefersub` (shall) | 멤버 INVITE 에는 `timer`(+100rel)뿐 — `csp/GroupCallService.cpp:2499-2516` | 그룹 세션 REFER 를 쓰지 않아 실해는 작다 | ○ |
| GCS-16 | B | CSP | §6.3.3.1.2 9)·10) — 받은 INVITE 의 Answer-Mode·Priv-Answer-Mode 를 그대로 옮긴다 (shall) | 초대 INVITE 에 무조건 `Answer-Mode: Auto` — `csp/GroupCallService.cpp:2506-2507` (개별 호 착신도 이 함수). 받은 헤더를 읽는 코드가 없다 | 헤더를 따르는 규격 단말은 개별 호를 벨 없이 자동 응답한다. 짝 = PRV-4 | ◎ |
| GCS-17 | A | CSP | §10.1.1.4.1.1 4)b) — `<mcptt-calling-group-id>` = 그룹 ID | `"tel:" + 그룹 id` — `csp/GroupCallService.cpp:3975`. MCPTT group ID 규칙은 숫자뿐이면 `tel:+<id>`(`csp/McpttInfo.h:400-408`, CSC `_group_uri`) | 숫자뿐인 그룹 ID 는 INVITE 의 그룹 ID 와 그룹 문서·제휴 문서의 ID 가 달라진다(`g001` 형은 무관) | ○ |
| GCS-18 | A | CSP·psip | §6.3.3.1.1 2) — 음성 스트림의 미디어 속성은 받은 offer 의 것 | 멤버 offer·개시자 answer 의 fmtp·ptime 이 서버 코덱 표 값(AMR-WB `octet-align=1`)이다 — `csp/GroupCallService.cpp:2407-2409`. 개시 게이트는 코덱 이름만 본다(`:885-893`) | 개시자가 대역 효율 모드나 다른 mode-set 으로 offer 하면 선언과 실제 페이로드가 어긋난다 — CMP 의 leg 별 형식 변환 유무를 확인해야 확정 | △ |
| GCS-19 | A | CSP·CSC | §6.3.5.2 5)a) — 그룹 문서에 `<on-network-disabled>` 가 있으면 403 + `115 group is disabled` (shall) | DB `on_network` 값이 그룹 문서에도(산출 없음) 호 제어에도(`_onNetwork` 는 지문·이력만 — `csp/GroupCallService.cpp:2742`·`4020`) 닿지 않는다 | 콘솔에서 on-network 를 꺼도 그룹 호가 된다 | ◎ |
| GCS-20 | D | CSP | §10.1.1.4.2 1) — 자원 부족은 500(+Retry-After) | CMP 포트 확보 실패·세션 시간 창 밖이 모두 Warning 없는 403 — `csp/GroupCallService.cpp:1069-1085`·`:1562-1565` | 일시적 문제를 단말이 «금지» 로 읽어 재시도하지 않는다 | ○ |
| GCS-21 | C | CSP | §6.3.8.1 2) — 참가자 1명 이하면 해제 | 초대 대상이 0 이어도(멤버 미등록·미제휴) 곧바로 200 — `csp/GroupCallService.cpp:1557-1558`. «잔여 1 leg» 판정은 leg 이 끝날 때만 돈다 | 혼자 연 호가 T4 까지 남는다(그동안 Deny #3) | △ |
| GCS-22 | A | CSP | §10.1.1.4.4.3 — 제휴 해제 등으로 단말을 세션에서 내보낼 때 BYE | 해제 PUBLISH 는 DB 삭제·NOTIFY 만 한다(`csp/CscfModule.cpp:1798-1802`·`:1934-1948`). `CheckMemberState` 는 멤버십만 본다 | 제휴를 해제한 단말이 그 호에 계속 남아 미디어를 받는다. ptt_flows.md 는 «de-affiliate 시 이탈» 이라 적었다 | ○ |

### 3.4 그룹 호 — 단말 (GCC) — TS 24.379 §6.2 · §10.1

현장 앱·관제 앱 두 벌은 MCPTT SIP 를 직접 만들지 않는다 — 전부 SDK 경유라 아래는 세 앱 공통이다. 개별 호·애드혹 개시도 같은 함수(`startMcptt`)다.

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| GCC-1 | B | SDK | §10.1.1.2.1.1 5)·6)·7)·10) — Accept-Contact 둘(`g.3gpp.mcptt`, MCPTT icsi-ref — require;explicit) · `P-Preferred-Service` · Request-URI = 참여 기능 PSI (shall) | INVITE 에 더하는 헤더는 긴급·임박의 `Resource-Priority` 하나, Request-URI = `sip:<그룹>@<도메인>` — `sdk/core/src/engine.cpp:2656-2682`. 같은 파일의 MCVideo 호·경보 MESSAGE 는 싣는다 | 규격 IMS 코어·참여 기능이 MCPTT 요청으로 라우팅·판정하지 못한다. Request-URI 는 conformance C4h 가 «구형 단말 양립» 으로 적은 것 — 그 구형이 우리 SDK 다 | ◎ |
| GCC-2 | B | SDK | §10.1.1.2.1.1 4) · §6.2.3.1.1 3)·4) — 개시 INVITE·착신 180/200 의 Contact 에 `g.3gpp.mcptt`·MCPTT icsi-ref (shall) | 호별 Contact 태그는 MCVideo 호에만 건다(`sdk/core/src/engine.cpp:1150`·`:1590`). MCPTT 는 계정 Contact 그대로 | 규격 제어 기능은 200 OK Contact 의 태그를 쓴다 | ◎ |
| GCC-3 | B | SDK | §10.1.1.2.1.1 14)c) — mcptt-info 에 `<mcptt-client-id>`. NOTE 2 — 단말은 발신자 MCPTT ID 를 본문에 싣지 않는다 | `<mcptt-client-id>` 가 없고 `<mcptt-calling-user-id>` 를 싣는다 — `sdk/core/src/mcptt/mcptt_xml.cpp:36-52`. client-id 는 경보 MESSAGE 에만 있다 | 서버가 클라이언트를 가를 값이 없다(AFF-4 의 전제) | ◎ |
| GCC-4 | B | SDK | §10.1.2.2.1.1 13)a) — chat 합류는 `<session-type>chat`. §17.2.2.1.1 10)a) — 애드혹은 `adhoc` | `isPrivate ? "private" : "prearranged"` — `sdk/core/src/engine.cpp:2666`. `GroupCallOptions` 에 호 종류가 없다(그룹 종류는 `GroupDoc.sessionType` 으로 이미 안다) | 규격 서버에서 chat 합류가 404(118). 짝 = GCS-9 | ◎ |
| GCC-5 | D | SDK | §6.2.1 2)d) · §6.2.2 3)e) — m=audio 에 `i=speech` | `i=` 는 MCVideo SDP 에만 넣는다 — `sdk/core/src/engine.cpp:271` | 규격 서버·PCC 의 미디어 성분 판별 | ◎ |
| GCC-6 | B | SDK·관제 | §10.1.1.2.1.2 7)·8) — 착신 수락 방식은 INVITE 의 Answer-Mode 와 단말 설정으로 정한다. §6.2.3.2.1 1) — 거절은 480 + Warning `110 user declined the call invitation` (shall) | 판정은 계정 설정 `autoAnswerMcptt` 하나(`sdk/core/src/engine.cpp:1520`) — Answer-Mode·Priv-Answer-Mode 를 읽지 않는다. 코어 `reject` 는 상태 코드만 받고 관제 앱은 486 을 쓴다(`windows/dispatch-desktop/Services/DispatchSession.cs:1592`, 태블릿 `PhonePlane.kt:580`) | `Answer-Mode: Manual` 초대에도 곧바로 200. 거절이 «통화 중» 과 구분되지 않는다 | ◎ |
| GCC-7 | B | SDK | §10.1.3.2 2)~5)·8) — conference SUBSCRIBE: Request-URI = 세션 식별자 · P-Preferred-Service · Accept-Contact · Expires 4294967295 · mcptt-info `<mcptt-request-uri>` = 그룹 ID (shall) | `Event: conference`·`Expires: 3600` 만, Request-URI = 그룹 URI, 본문 없음 — `sdk/core/src/engine.cpp:3011-3022`. 앱은 세션 밖의 제휴 그룹 전체를 구독한다 | 규격 서버에서 로스터를 못 받는다. handoff §14.1 의 «구독 3600초 갱신은 규격대로» 는 conference 구독에는 맞지 않는다 | ◎ |
| GCC-8 | B | SDK | §10.1.1.2.4.1 — 재합류 INVITE 의 Request-URI = 세션 식별자 (shall) | MCPTT 발신은 늘 그룹 URI — `sdk/core/src/engine.cpp:2682`. `CallInfo.sessionUri` 는 MCVideo 호에서만 채운다 | 끝난 세션에 «재합류» 하면 404 대신 새 세션이 열린다 | ◎ |
| GCC-9 | D | SDK | §10.1.1.4.1.1 4)b) · Annex F.1.3 — 착신 그룹은 `<mcptt-calling-group-id>` | 그룹 = From URI 의 user — `sdk/core/src/engine.cpp:1511` (`callingGroupId` 는 해석만 한다. MCVideo 착신은 우선 쓴다) | From 이 그룹이 아닌 규격 서버에서 다른 채널로 세션이 선다 | ○ |
| GCC-10 | B | SDK·CSC·CSP | §6.2.1 2)b) — 그룹 문서 `<preferred-voice-encodings>` 가 있으면 그 코덱으로 offer | SDK 는 요소를 읽지 않고, CSC 는 싣지 않는데, CSP 는 서비스 코덱이 offer 에 없으면 488(`csp/GroupCallService.cpp:882-903`) | 규격 단말이 서버가 요구하는 코덱을 알 규격 경로가 없다 | ○ |

### 3.5 개별 호 (PRV) — TS 24.379 §11.1

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| PRV-1 | B | CSP·SDK | §11.1.1.2.1.1 9) — 착신자는 `application/resource-lists+xml` 본문. §11.1.1.3.1.1 8) — 없으면 403 + `145 unable to determine called party` (shall) | CSP 는 착신자를 Request-URI 또는 `<mcptt-request-uri>` 에서만 잡는다(`csp/ModuleDispatcher.cpp:944-958`). SDK 는 resource-lists 를 싣지 않는다(`sdk/core/src/engine.cpp:2676-2681` — 애드혹만) | 규격 단말(R-URI = PSI + resource-lists)의 개별 호가 480 이 된다. conformance C4h 는 «개별 통화 = `<mcptt-request-uri>`, 정합» 이라 적었다 | ◎ |
| PRV-2 | A | CSC·CSP·SDK | §11.1.1.3.1.1 10)·11) — `<allow-private-call>` 이 없거나 false 면 403 + `107`, 목록 밖 상대면 `144`. TS 24.484 표 8.3.2.7-7 — 요소가 없으면 false | user profile 에 요소가 없고(CMS-3) CSP 는 프로파일을 보지 않는다(csp 에 107·144 없음). SDK 는 «요소 없음 = 허용» 으로 읽는다(`sdk/core/src/csc/cms_doc.cpp:17-21`) | 개별 호 발신을 막을 설정이 없고 PrivateCallList 밖 상대에게도 걸린다. 규격 단말은 이 문서로는 개별 호를 미인가로 읽는다 | ◎ |
| PRV-3 | A | CSP·SDK | §11.1.2.2 — floor 없는 개별 호는 offer 에 `m=application` 을 싣지 않는다. `mc_no_floor_ctrl` 은 pre-established session 용(TS 24.380 §14.2.6) | CSP 는 fmtp `mc_no_floor_ctrl` 로만 floor off 를 정한다(`csp/ModuleDispatcher.cpp:961-964`). SDK 발신 전이중은 `m=application` 을 싣지 않고(`sdk/core/src/engine.cpp:2646-2655`), 착신 판정은 문자열 `mc_no_floor_ctrl`(`sdk/core/src/mcptt/mcptt_xml.cpp:227`) | SDK 가 건 전이중 호를 CSP 가 floor 있는 호로 세운다. 규격 단말·서버와도 서로 반대로 읽는다. 실호 증상은 실측 필요 | ○ |
| PRV-4 | B | CSP·SDK | §11.1.1.2.1.1 14) — 발신 단말이 Answer-Mode(Auto·Manual) 또는 Priv-Answer-Mode 를 싣는다. §11.1.1.3.1.1 11)·18) — 인가 403 `125`·`126`·`143` | SDK 는 헤더를 싣지 않고, CSP 는 읽지 않으며 착신에 늘 `Answer-Mode: Auto`(GCS-16). 인가 요소도 없다(CMS-3) | 수동 수락·강제 자동 응답 요청이 동작하지 않는다 | ◎ |
| PRV-5 | A | CSP·CSC | §11.1.1.4.1 10) · §6.3.8.2 2) — 개별 호 최대 통화 시간(service configuration `<private-call>` `<max-duration-with/without-floor-control>`) | 최대 시간 검사는 편성 그룹 호만 — `csp/GroupCallService.cpp:305-311`. 문서에 `<private-call>` 요소가 없다 | 개별 호는 한쪽이 끊을 때까지 이어진다(T4 도 돌지 않는다 — 타이머 D5 와 한 묶음) | ○ |
| PRV-6 | C | CSP | §11.1.1.3.2 7) — 착신자의 바인딩이 없으면 404 | 미등록 = 480 — `csp/ModuleDispatcher.cpp:945-958` | 없는 MCPTT ID 와 일시 부재를 가르지 못한다 | ◎ |
| PRV-7 | B | CSP | §6.3.3.2.3.1 2) — 개시자에게 가는 응답에 P-Asserted-Identity. §11.1.1.3.1.1 — 받은 180 의 Warning 을 옮긴다. §11.1.1.4.2 — SDP 없는 응답은 본문째 전달 | 개시자 180 은 헤더 없이 낸다(`csp/GroupCallService.cpp:594`). 거절 최종 응답에는 CSP 의 112 만 실리고 멤버 응답의 Warning 은 200 OK 에만 쓴다(`:439-447`·`:540-548`) | 착신 측이 준 사유(110·127 등)가 발신 단말에 닿지 않는다 | ○ |
| PRV-8 | A | CSC·CSP | §11.1.1.3.2 8)·9) — 착신 인가(`<allow-private-call-participation>`, IncomingPrivateCallList) 403 `127`·`159` | 착신 검사는 등록 여부뿐. 문서에 요소가 없다 | 개별 호 착신을 막을 설정이 없다 | ○ |
| PRV-9 | B | CSP·CMP | TS 24.380 §14.3.2 — `mc_queueing` 은 큐잉을 지원할 때만 answer 에 싣는다 | 개별 호는 CMP 가 큐를 끄는데(`cmp/PMcpttGroup.cpp:97-99`) CSP 는 answer·착신 offer 에 `mc_queueing` 을 싣는다(`csp/GroupCallService.cpp:181-186`·`:4095`) | 큐잉을 협상한 규격 단말이 Queue Position 대신 Deny #1 을 받는다 | ○ |

### 3.6 애드혹 그룹 호 (ADH) — TS 24.379 §17

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| ADH-1 | B | CSC·CSP·SDK | §17.2.2.1.1 — service configuration 에 `<allow-adhoc-group-call-support>` 가 없으면 단말은 개시하지 않는다. §17.4.2.2 5) — 서버는 403 + `186`. TS 24.484 §8.4.2.6 — `<adhoc-group-call>` 이 없으면 «미지원» | 문서에 `<adhoc-group-call>` 이 없다(`csc/src/services/mcptt.py:1845-1855`). 스위치는 csp.json `PttAdhocEnabled` 뿐이고 SDK 는 user profile 만 본다 | 규격 단말은 CIMS 에서 애드혹 호를 개시하지 않는다. 타이머 D5·D6 과 같은 요소 — 함께 넣는다 | ◎ |
| ADH-2 | A | CSP | §17.4.2.2 10) — 서버가 애드혹 그룹 ID 를 만들고 200 OK `<mcptt-calling-group-id>` 로 돌려준다 | 그룹 id = Request-URI user(`clsAdhoc._id = pszTo`) — `csp/ModuleDispatcher.cpp:1010-1043`. 규격형(R-URI = PSI)이면 모든 애드혹 호가 PSI 이름 하나로 모인다 | 두 번째 동시 호가 «비멤버 403» 이거나 남의 세션에 합류한다. ID 서버 부여 미구현(이미 문서에 있음)의 실제 증상 | ◎ |
| ADH-3 | A | CSP·CSC | §17.4.2.2 6) — 초대 인원이 `<max-no-participants>` 를 넘으면 403 + `189` | resource-lists 전원을 상한 없이 멤버로 넣는다 — `csp/ModuleDispatcher.cpp:1011-1039` | INVITE 한 건으로 수백 명 fan-out | ◎ |
| ADH-4 | B | CSP | §17.3.2.1.1 9) — 미인가 403 + `185`. 4) — `184` | Warning 없는 403 — `csp/ModuleDispatcher.cpp:1015-1022`. 프로파일 조회 실패면 통과. 스위치가 꺼져 있으면 «없는 그룹» 403 | 단말이 «권한 없음»·«시스템 미지원»·«없는 그룹» 을 가르지 못한다 | ◎ |
| ADH-5 | B | CSC·CSP | §17.3.2.1.2 6) — `<allow-adhoc-group-call-participation>` 이 없거나 false 면 403 + `188` | 문서에 요소가 없고 명단의 누구든 초대한다 | 규격대로 읽으면 이 문서로는 아무도 애드혹 호에 참가할 수 없다 | ◎ |
| ADH-6 | A | CSP | §17.4.5.1.1 — 참가자 변경 re-INVITE(resource-lists `method=INVITE/BYE`), 미인가 403 + `190` | 그룹 세션 re-INVITE 는 긴급·임박·경보 지시자만 보고 그 밖은 200 — `csp/ModuleDispatcher.cpp:512-531` | 규격 단말의 참가자 추가·제거가 성공 응답을 받고 반영되지 않는다 | ○ |
| ADH-7 | A | CSP·SDK | §17.2.3.1.1 — 호 해제는 BYE + `Reason: SIP;cause=200;text="User requested release"`. §6.3.3.2.4 3A) — 서버는 전원 해제 | SDK BYE 에 Reason 이 없고 CSP 는 읽지 않는다. 개시자가 끊어도 일제 통화가 아니면 나머지를 유지한다 — `csp/GroupCallService.cpp:3297-3335` | 개시자가 애드혹 호를 끝낼 수 없다(본인만 나간다) | ○ |
| ADH-8 | B | CSP·SDK | §17.2.2.1.1 10)a) · §17.4.2.1.1 — `<session-type>adhoc` (Annex F.1) | SDK 개시도 CSP 멤버 INVITE 도 `prearranged` 로 싣는다 — `sdk/core/src/engine.cpp:2666`, `csp/ModuleDispatcher.cpp:1028` → `csp/GroupCallService.cpp:3966` | 규격 단말이 애드혹 호를 편성 그룹 호로 다룬다(그룹 문서를 찾는다) | ◎ |
| ADH-9 | C | CSP | §17.4.2.2 12)a) — resource-lists 의 항목 전부 | `tel:` 뒤 숫자·`+` 만 뽑는다 — `csp/McpttInfo.h:471-489` | MCPTT ID 가 `sip:` 형인 단말의 명단이 비어 보인다(CIMS 가 배포하는 ID 는 `tel:` 형) | ○ |
| ADH-10 | D | CSP | §6.3.8.1 2) — «참가자 1명 이하» 해제 대상은 편성·일제·chat (애드혹은 목록에 없다) | 애드혹 세션도 잔여 1 leg 이면 BYE — `csp/GroupCallService.cpp:3327-3334` | 둘이 남은 호에서 한쪽이 끊기면 재합류(§17.2.5)가 404 | △ |

### 3.7 긴급·임박 위험·경보 (EMG) — TS 24.379 §6.2.8 · §6.3.3.1.13~20 · §12

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| EMG-1 | A | CSP | §12.1.2.1 3) — 미제휴 발신자는 암묵적 제휴. §12.1.3.1 4)b)i) — 자격(멤버)이 없으면 403 + `120 user is not affiliated to this group` (shall) | 발령 인가는 그룹 능력 ∧ 사용자 프로파일뿐, 취소는 프로파일뿐 — 멤버십·제휴를 보지 않고 제휴도 적지 않는다 — `csp/PttAsModule.cpp:121-133`·`:147-183`, `csp/GroupCallService.cpp:1745-1757` | 등록된 PTT 가입자면 비멤버도 임의 그룹에 경보를 내고 `originated-by` 로 남의 경보를 취소할 수 있다. 미제휴 멤버는 경보 뒤에도 그 그룹 통지를 못 받는다 | ◎ |
| EMG-2 | A | CSP | §6.3.3.1.13.1 1)a)i) — `<EmergencyAlert>` entry 가 DedicatedGroup 이면 경보 대상이 그 그룹이어야 인가 | 경보 인가에 대상 일치 판정이 없다(`csp/GroupCallService.cpp:1745-1751`). 호 인가는 본다(`:1643`). CSC 문서는 긴급 그룹 미지정이면 `allow-activate-emergency-alert` 를 false 로 내는데(`csc/src/services/mcptt.py:1751`) 서버는 받는다 | 전용 긴급 그룹 사용자가 다른 그룹에 낸 경보가 퍼진다. emergency_modes 문서의 «콜·경보 공통 판정» 과 다르다 | ◎ |
| EMG-3 | A | CSP | §9.2.2.3.3 14) → §12.1.3.4 — 새로 제휴한 단말에 진행 중 경보를 MESSAGE 로 알린다 (shall) | 경보 캐시는 호 경로에서만 읽는다(`csp/GroupCallService.cpp:1327`·`:1344`·`:2002`). 제휴 경로는 NOTIFY 만 낸다(`csp/CscfModule.cpp:2028-2041`) | 경보 뒤에 로그인하거나 망 복귀로 제휴를 다시 실은 단말·관제석은 그 경보를 모른다 | ◎ |
| EMG-4 | B | CSC | TS 24.484 §8.3.2.1 11)xvi) `<allow-imminent-peril-call>` · TS 24.379 §6.2.8.1.8 — true 가 아니면 임박 위험 호는 미인가 | user profile ruleset 에 요소가 없다 — `csc/src/services/mcptt.py:1746-1753` (`<ImminentPerilCall>` entry 는 싣는다) | 규격 단말은 임박 위험 호를 요청하지 않는다. SDK 는 «없으면 허용» 이라 드러나지 않는다 (CMS-3 과 한 묶음) | ◎ |
| EMG-5 | B | CSP | §6.3.3.1.14 · §10.1.1.4.2 10)·11) — 미인가 긴급·임박 개시의 403 에 mcptt-info(`emergency-ind` false + `alert-ind` false / `imminentperil-ind` false) (shall) | 개시 INVITE 는 본문 없는 403 — `csp/GroupCallService.cpp:931-941`. re-INVITE 경로만 본문을 싣는다 | 단말이 긴급 미인가 403 을 다른 403 과 가르지 못한다(현장 앱은 403 이면 모두 일반 호로 다시 건다) | ◎ |
| EMG-6 | A | SDK | §10.1.1.2.1.6 3)b)ii)B) — 받은 재광고의 `<alert-ind>` false 에서 `<originated-by>` 가 나면 MEA 1(경보 없음) | 재광고에서 `emergency-ind`·`imminentperil-ind` 만 읽는다 — `sdk/core/src/engine.cpp:1292-1296`. `McpttInfo` 에 alert·originatedBy 가 없다 | CSP 는 «경보 취소 + 긴급 해제» 를 참여 leg 에 re-INVITE 로만 알린다 — 참여 단말은 경보 배너가 남고 내 경보의 제3자 취소도 모른다 | ◎ |
| EMG-7 | C | SDK | §6.2.8.1.4 2) — 임박 상향의 2xx 에 Warning `149` 가 있으면 임박으로 확정하지 않는다. §6.2.8.1.13 — 뒤따르는 INFO 의 지시자 | 2xx 면 무조건 Confirmed — `sdk/core/src/engine.cpp:1195-1203`. INFO 는 미응답 멤버만 해석한다 | 긴급 진행 중 그룹에 임박 상향을 보내면 표시·Floor Request 비트가 어긋난다(경합 때만) | △ |
| EMG-8 | A | 현장·관제 | §6.2.8.1.8 1)a) · §12.1.1.1 4)a)i)A) — DedicatedGroup 이면 긴급·경보 대상은 그 그룹 | 현장 앱 통화 중 SOS 는 현재 세션 그룹으로 경보 + 상향(`android/ptt-client/…/PttEmergency.kt:30-41`). 관제 앱은 `allow-emergency-group-call` 만 본다(`DispatchSession.cs:1657-1669`, `PttPlane.kt:140-187`) | 전용 그룹 사용자가 다른 그룹 통화 중 SOS 를 누르면 상향은 403, 경보는 EMG-2 때문에 그 그룹에 퍼진다 | ○ |
| EMG-9 | A | 현장 | §12.1.1.1 — 경보 MESSAGE 가 4xx·5xx·6xx 면 MEA 1 로. §12.1.1.2 — 취소 실패면 경보 유지 | 403 만 되돌린다(`if (r.code != 403) return true`) — `android/ptt-client/…/PttEmergency.kt:218-236` | 발령이 404·480·5xx 로 실패해도 내 경보 표시가 남고, 취소가 실패해도 표시는 내려간다 | ◎ |
| EMG-10 | C | CSP | §6.3.3.1.11 — 상태가 바뀌면 제휴 멤버에 통지 | 세션이 끝나면 긴급·임박 상태를 지우기만 한다 — `csp/GroupCallService.cpp:125-134` | 비참여 제휴 단말과 BYE 로 나간 단말이 그룹을 계속 긴급으로 본다(«그룹 긴급 상태의 수명» 편차의 부작용) | △ |
| EMG-11 | C | CSP | §6.3.3.1.11 5) — 경보 팬아웃 MESSAGE 에 P-Asserted-Identity(제어 기능 PSI) | 헤더는 Accept-Contact 둘 + P-Asserted-Service 뿐 — `csp/PttAsModule.cpp:213-216`. 상태 통지 쪽은 싣는다 | 규격 단말·중간 노드에 사용자 발신 MESSAGE 로 보인다 | ◎ |
| EMG-12 | C | CSP | §12.1.3.1 2) — Accept-Contact 에 MCPTT icsi-ref 가 없으면 403 | mcptt-info 지시자 유무만으로 경보 경로에 넣는다 — `csp/ModuleDispatcher.cpp:2658-2662` | 받아들이는 쪽이 넓다 | ○ |
| EMG-13 | C | CSP | §6.3.3.1.7 6)c)·d) — 긴급 fan-out INVITE 에 `<alert-ind>`(경보가 아니면 false), 임박이었으면 `<imminentperil-ind>` false | `emergency-ind` true 만 싣는다 — `csp/GroupCallService.cpp:3978-3983` | 지시자 조합을 검증하는 수신 구현과 어긋난다 | ◎ |
| EMG-14 | C | CSP | §6.3.3.1.15 5)b) — 임박 해제 재광고에 `<emergency-ind>` false + `<imminentperil-ind>` false | `imminentperil-ind` false 만 — `csp/GroupCallService.cpp:2045-2047` | 작다 | ◎ |
| EMG-15 | C | CSP | §10.1.1.4.7 4) — 미인가 임박 상향은 403 | 긴급 진행 중이면 인가 판정 전에 200 + 149 로 답한다 — `csp/GroupCallService.cpp:1950-1958` | 결과 상태는 같다(임박 불수용) | ◎ |
| EMG-16 | C | CSP | §10.1.1.4.2 15)g)iii) — 임박 진행 중 그룹에 임박 표시로 합류하면 다른 제휴 멤버에 통지 | INVITE 합류의 «새 표시 통지» 는 긴급만 — `csp/GroupCallService.cpp:1050-1057` (re-INVITE 경로에는 있다) | 두 번째 임박 사용자를 다른 멤버가 모른다 | ◎ |
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
| FCS-22 | C | CMP | §6.3.4.4.7 2)f — 선점 요청자에게 Queue Position Info 는 **그 단말이 큐잉을 협상했을 때** | 그룹 플래그 `_queueEnable` 로 정한다 — `cmp/PMcpttGroup.cpp:887` | 협상하지 않은 단말이 위치 통지를 받는다 | ◎ |
| FCS-23 | D | CMP·CSP | §6.3.4.4.2 3)a)i — 화자 신원은 «privacy 를 요청하지 않았을 때» | 조건 없이 싣는다. 합류 명령에 privacy 입력이 없다 | CSP 가 privacy 요청 절차를 받는지부터 확인 필요 | △ |
| FCS-24 | D | CMP | §8.2.15 — Queued Floor Requests 결과 메시지의 요청자 신원·목록 부호화 | Cancel Result 에 신원 필드가 없고 목록 항목 길이를 1옥텟으로 다룬다 — `cmp/PMcpttGroup.cpp:1451-1457`·`:1487-1496` | 규격 그림(PDF 원본)으로 필드 폭을 다시 확인할 것 | △ |

### 3.9 발언권 — 단말 (FCC) — TS 24.380 §6.2

메시지 형식의 단말 쪽 편차는 서버와 같은 뿌리라 §3.8 에 함께 적었다(FCS-4·6·10·11·12·15·16).

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| FCC-5 | C | SDK | §8.2.3.10 — Message Sequence Number 는 Taken·Idle 묶음을 잇는 값 | Taken·Idle 공용 카운터로 «되돌아간 번호» 를 버린다 — `floor_participant.cpp:83-87`·`:313-319` | 번호를 따로 세거나 다시 매기는 서버에서 정상 메시지가 버려진다. android_ue_client U7 은 정합으로 적었다 | △ |

### 3.10 발언권 SDP 협상 (SDP) — TS 24.380 §4.3 · §14 · TS 24.379 §6.2.1 · §6.3.3.1.1

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| SDP-1 | B | SDK·CSP | TS 24.380 표 4.3.3.1-1 — `m=application <port> udp MCPTT` (proto = "udp") | `UDP MCPTT` + 규격에 없는 `a=floorid:0 mstrm:audio` — `sdk/core/src/mcptt/mcptt_xml.cpp:317-323`, CSP offer 도 같다 | proto 를 대소문자 구분으로 대조하는 상대는 제어 채널을 못 알아본다. conformance C4 는 이 표기를 «정합» 으로 적었다 | ◎ |
| SDP-2 | B | SDK·CSP·CMP | §4.3.3.1 · §14.2.7 · §14.3.8 — `mc_floor_ssrc`(다중화를 지원하면 필수): 상대는 그 값을 floor 메시지의 RTCP 헤더 SSRC 로 쓴다 | 레포 어디에도 `mc_floor_ssrc`·`mc_ssrc` 처리가 없다. 단말 헤더 SSRC = MCPTT ID 해시(`sdk/core/src/engine.cpp:160-163`), 서버 = 그룹당 순번(`cmp/PMcpttGroup.cpp:67`) | 한 포트에 여러 세션의 제어 채널을 다중화하는 상대는 메시지를 세션에 못 묶는다 | ◎ |
| SDP-3 | A | CSP·CMP·SDK | §14.2.3 — 초대 offer 의 `mc_priority` = 그룹 문서 `<user-priority>`. §14.3.3 — 제어 기능 answer = min(offer, user-priority, 계층 수), 단말 answer 는 받은 값을 되돌린다. §14.3.1 — answer 는 offer 에 없던 파라미터를 싣지 않는다 | 초대 offer 는 상수 `mc_priority=3`(`csp/GroupCallService.cpp:51`), 개시자 answer 에는 없다(`:181-186`). offer 의 `mc_priority` 원값을 상한 없이 `max_priority` 로 CMP 에 준다(`:771-773`). SDK 착신 answer 는 늘 `mc_queueing` 만(`sdk/core/src/engine.cpp:1516`) | 단말이 offer 에 큰 값을 적고 같은 Floor Priority 로 요청하면 그룹 문서 우선순위와 무관하게 선점 서열이 오른다. MCVideo 쪽은 min(offer, roster) 가 구현돼 있다 | ○ |

### 3.11 그룹 문서·GMS (GMS) — TS 24.481

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| GMS-1 | B | CSC·SDK | §6.2.2.2 · §7.2.10.2 — 그룹 ID 로 찾는 문서 = global tree `…/global/byGroupID/<그룹 ID>` | 라우트는 `/org.openmobilealliance.groups/users` 하나, 경로의 XUI 가 토큰과 다르면 403 — `csc/src/services/mcptt.py:3837`·`:2793-2796`. SDK 도 `users/{본인}/{그룹 URI}`(`sdk/core/include/cimsue/csc.h:388`) | 규격 단말·타 서버의 조회가 404. 우리 SDK 는 규격 GMS 에서 남이 만든 그룹 문서를 못 읽는다 | ◎ |
| GMS-2 | B | CSC·SDK | §6.3.2.2.1 — 생성 PUT 의 XUI = group creation XUI. §6.3.2.3 c) — `<list-service uri>` 가 정책과 다르면 409 `<uniqueness-failure>` + `<alt-value>` | 그룹 ID 는 경로 마지막 조각에서 뽑고 본문 `uri` 는 읽지 않는다(`mcptt.py:2836`). CSC 가 ue-init-config 로 알리는 `<group-creation-XUI>` 기본값(XCAP root URL — `:1905`)으로 PUT 하면 403. 오류는 400·409 JSON | 규격 단말은 CIMS 에 그룹을 만들 수 없다. GMS 가 그룹 ID 를 정해 주는 절차가 없다 | ○ |
| GMS-3 | B | CSC | §6.3.4.2.1 → RFC 4825 §7.1 — PUT 은 문서를 **교체**한다 | 준 필드만 UPDATE 하고 `<list>` 가 없으면 멤버를 유지한다 — `mcptt.py:2687-2697` | 요소를 지워 기본값으로 되돌려도 반영되지 않고 200 이 온다. mcptt_api.md 는 이를 동작으로만 적었다 | ○ |
| GMS-4 | C | CSC | RFC 4825 §8.2.2·§8.2.5 — 비정형·스키마·제약 위반 = 409 + `application/xcap-error+xml`, MIME 불일치 = 415 | 400 JSON(`invalid_group_document`·`unknown_member`), Content-Type 검사 없음 — `mcptt.py:2834-2893` | 규격 XDMC 가 원인을 가리지 못한다 | ○ |
| GMS-5 | B | CSC | §5.2 · §6.3.6.3~§6.3.12.3 — 요소·속성 단위 XCAP(node selector) (GMS shall) | 경로를 `/` 로 잘라 마지막 조각을 그룹 URI 로 쓴다 — `mcptt.py:2787`·`:2813` | 규격 단말의 멤버 한 명 추가·삭제가 404/400. 문서 전체 PUT 만 통한다 | ○ |
| GMS-6 | B | CSC·SDK | §6.3.16 — 그룹 문서의 기본 조회는 «멤버 제외»(HTTP POST + GMOP 본문) | POST 분기가 없다(GET·PUT·DELETE 만). SDK 는 늘 전체 GET | 규격 단말의 기본 조회가 실패한다. 우리 단말은 큰 그룹도 매번 멤버 전체를 받는다 | ◎ |
| GMS-7 | B | CSC | §7.2.8 — `<protect-media>`·`<protect-floor-control-signalling>` 은 **요소가 없으면 true**(GMK 필수·floor 보호) | MCPTT 몫에 두 요소가 없다(규격 밖 `<mcpttgi:on-network-encryption>` 만) — `mcptt.py:1463-1486`. MCVideo 몫은 false 를 명시한다(`csc/src/services/mcvideo.py:400-402`) | 규격 단말은 모든 CIMS 그룹을 보호 필수 그룹으로 읽는다(GMK 가 없어 호를 못 열거나 floor 보호를 요구) | ◎ |
| GMS-8 | B | CSC·SDK·관제 | §7.2.8 — 요소가 없을 때: `on-network-invite-members` = false(chat), `allow-MCPTT-emergency-alert` = false, `on-network-allow-conference-state` = false, group-priority = 최저 | XCAP 생성 기본값 = prearranged·경보 허용·conference 허용·우선순위 5(`mcptt.py:2633-2640`). SDK 구조체 기본 = prearranged·긴급 호/경보 허용(`sdk/core/include/cimsue/csc.h:145-148`) | 요소를 생략한 규격 문서가 반대 뜻으로 만들어진다(권한 확대). SDK 는 규격 GMS 문서를 반대로 읽는다. mcptt_api.md 의 «conference-state 기본 true» 는 규격과 반대다 | ◎ |
| GMS-9 | B | CSC·CSP | §7.2.2 · §7.2.8 — `<rule>` 은 조건(`<identity>`·`<is-list-member>`)에 맞는 신원에 action 을 준다. `<allow-initiate-conference>`·`<join-handling>` | PUT 해석이 문서 안 첫 action 값을 그룹 전역 값으로 읽고(`mcptt.py:2581-2583`), 개시·합류 action 은 읽지 않고 늘 true 로 낸다(`:1499-1500`) | 특정 사용자에게만 긴급 호·경보를 준 문서가 전 멤버 허용으로 바뀐다. «합류만 가능» 을 담을 수 없다 | ○ |
| GMS-10 | B | CSC·SDK | §7.2.4.2 — `urn:3gpp:ns:mcpttGroupInfo:1.0` 스키마에 없는 요소 | 규격 이름공간 `mcpttgi` 로 자체 요소 다섯을 싣는다 — `on-network-require-affiliation`(`mcptt.py:1478`)·`on-network-require-talker-id`(`:1484`)·`on-network-encryption`(`:1486`)·`org-code`(`:1531`)·`authorized-user`(`:1536`). 선언해 둔 `cims:` 이름공간은 `user-title` 에만 쓴다 | 규격 단말은 무시한다. 우리 SDK 가 규격 GMS 에 PUT 하면 스키마 위반이다 — `cims:` 로 옮긴다 | ○ |
| GMS-11 | A | CSC·관제 | §7.2.8 — `<on-network-max-participant-count>` = 세션 최대 참가자 | 0(무제한)을 10 으로 낸다 — `max_count = group.get('max_members') or 10`(`mcptt.py:1450`). 관제 앱은 읽은 값을 되돌려 저장하고(`GroupEditViewModel.cs:180`·`:278`), SDK 는 0 이면 요소를 생략해 0 으로 되돌릴 길이 없다 | 관제 앱에서 그룹을 한 번 저장하면 정원이 10 으로 굳는다 → MCVideo 호가 10명에서 486(122). MCPTT 미집행은 GCS-5 | ◎ |
| GMS-12 | C | CSC | §7.2.12.1 — `<list>` 읽기는 `<on-network-allow-getting-member-list>` 가 있는 규칙의 신원만(없으면 false) | action 목록에 요소가 없고 멤버면 `<list>` 전체를 준다 — `mcptt.py:1498-1504`·`:2820-2830` | 요소로 판단하는 규격 단말은 «명단 열람 불가» 로 본다. 명단을 숨기는 그룹을 만들 수 없다 | ◎ |
| GMS-13 | A | CSC | RFC 4825 — 문서는 well-formed XML | 그룹 이름·멤버 이름·직함·조직 코드를 이스케이프 없이 넣는다 — `mcptt.py:1407`·`:1413`·`:1433`·`:1531` (같은 파일의 다른 문서는 `html.escape` 를 쓴다) | 이름에 `&`·`<` 가 있으면 그 그룹 문서 전체가 깨진다 | ◎ |
| GMS-14 | B | CSP | §6.3.13.3.2.2 — xcap-diff SUBSCRIBE 의 신원은 mcptt-info `<mcptt-access-token>`, 구독 대상은 resource-lists. RFC 5875 §4.6 — NOTIFY 의 `sel` 은 구독한 URI 와 같아야 한다 | 신원 = From, 분류 = Request-URI 문자열에 "gms"·"cms" 가 있나(`csp/CscfModule.cpp:1349-1364`), 본문은 읽지 않는다. `sel` 은 고정 형식(`csp/CspServer.cpp:696-711`) — CMS 구독도 같다 | 규격 단말이 구독한 문서와 NOTIFY 의 `sel` 이 맞지 않는다. 토큰 없이도 구독이 선다. conformance S3 은 «정합» 으로 적었다 | ○ |
| GMS-15 | B | CSP·CSC | RFC 5874 — `new-etag` = 변경 뒤 문서의 ETag, 삭제는 `previous-etag` 만. RFC 5875 §4.7 — 앞 NOTIFY 의 200 전에 다음 NOTIFY 를 보내지 않는다 | `new-etag` 가 `init`·`etag_<gid>`·`change_<ts>`·빈 값이고 HTTP ETag(내용 해시)와 다르다. 삭제에도 `new-etag` 를 싣는다. 그룹마다 NOTIFY 를 연달아 보낸다 — `csp/CspServer.cpp:700-711`·`:1284-1289`·`:1409-1416` | `new-etag` 를 캐시와 견주는 단말은 늘 불일치이거나 «변경 없음» 으로 읽는다. 삭제를 변경으로 읽는다 | ○ |
| GMS-16 | B | SDK·관제·현장 | §6.3.13.2.1 — 단말의 구독: resource-lists 본문 · mcptt-info 의 access token · P-Preferred-Service · Contact icsi-ref · Request-URI = 설정된 PSI | `Event: xcap-diff`·`Expires` 만 싣고 본문이 없다 — `sdk/core/src/engine.cpp:3024-3031`. PSI 는 ue-init-config 의 `<GMS-URI>` 가 아니라 `sip:gms_psi@<도메인>` 고정(`DispatchSession.cs:517`, `DiscoveryPlane.kt:20`) | 규격 GMS·CMS 는 이 구독을 받지 않거나 무엇을 통지할지 모른다 | ○ |
| GMS-17 | B | SDK | TS 24.379 §6.3.5.4 3)·§6.3.5.3 2) — 개시·합류 인가는 `<is-list-member>` 조건 + `<allow-initiate-conference>`·`<join-handling>` 규칙. TS 24.481 §7.2.2 — MCData 그룹의 entry 는 `<mcdata-mcdata-id>` | SDK 가 만드는 PUT 본문의 규칙에 조건·개시/합류 action 이 없고 entry 에 `<mcdata-mcdata-id>` 가 없다 — `sdk/core/src/csc/group_doc.cpp:113-122`·`:182-196` | 규격 GMS 에 저장되면 누구도 그 그룹 호를 개시·합류할 인가가 없다(우리 서버는 문서를 다시 만들어 가려진다) | ○ |
| GMS-18 | A | CSC·관제 | §7.2.4.2 — `priorityType` 0~255 | 서버는 범위를 검사하지 않고(`mcptt.py:2579`), 관제 앱은 0~15 로 잘라 저장한다(`GroupEditViewModel.cs:278`, 태블릿 `GroupForm.kt:48`) | 콘솔이 16 이상으로 둔 우선순위가 앱 저장 때 15 로 바뀐다. 범위 밖 값이 문서로 나간다 | ○ |

### 3.12 설정 문서·CMS (CMS) — TS 24.484

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| CMS-1 | B | CSC·CSP·SDK | §8.4.2.8·§8.4.2.9 — service configuration 은 global 문서, 이름 `service-config.xml` (`…/org.3gpp.mcptt.service-config/global/…/service-config.xml`) | 라우트는 `/org.3gpp.mcptt.service-config/users` 하나, `users/{xui}/service-config` + 본인 검사 — `csc/src/services/mcptt.py:3841`. SDK·CSP NOTIFY `sel` 도 같은 경로. MCVideo 문서는 global 로 서빙한다(`:3843`) | 규격 단말의 GET 이 404 — Resource-Priority·타이머·신호 보호 설정을 못 받는다 | ◎ |
| CMS-2 | B | CSC·CSP·SDK | §8.3.1A · §8.3.2.8 — user profile 문서 = `…/users/sip:MCPTTID/mcptt-user-profile-<index>.xml` | 핸들러가 `/user-profile` 문자열로 XUI 를 자른다 — `mcptt.py:2955-2967`. 규격 이름이면 `tel:` XUI 는 403 | 규격 단말의 user profile 조회가 실패한다 | ◎ |
| CMS-3 | A | CSC·CSP·SDK | §8.3.2.1 11) · 표 8.3.2.7 — ruleset 의 인가 요소. `<allow-private-call>` 등은 **요소가 없으면 false** | actions 에 일곱 요소 + anyExt 둘뿐이다 — `mcptt.py:1744-1762`. 없는 것: `allow-private-call`·`allow-private-call-to-any-user`·`allow-manual-commencement`·`allow-automatic-commencement`·`allow-force-auto-answer`·`allow-imminent-peril-call`·`allow-cancel-private-emergency-call`·`allow-private-call-participation`·`allow-adhoc-group-call-participation`·`allow-to-modify-adhoc-group-call-participants-info`. SDK 는 «없음 = 허용» 으로 읽는다(`sdk/core/src/csc/cms_doc.cpp:17-21`) | 규격 단말은 개별 호·임박 위험 호를 미인가로 본다. 운영자가 사용자별로 끌 수단이 없다(앱의 게이트는 늘 통과). 서버 판정은 PRV-2·PRV-4·PRV-8·EMG-4·ADH-5·ADH-6 | ◎ |
| CMS-4 | B | CSC·CSP | §8.4.2.6 — `<signalling-protection>` 의 `<confidentiality-protection>`·`<integrity-protection>` 은 **기본 true**. TS 24.379 §6.6.2.3.1 — 요소가 없으면 단말은 mcptt-info 를 암호화한다 | `<on-network>` 에 요소가 없다 — `mcptt.py:1835-1857`. MCVideo 문서는 false 를 명시한다(`mcvideo.py:654-689`). CSP 는 암호화된 요소를 해석하지 못한다(`csp/McpttInfo.h:41-94`) | 규격 단말이 `mcptt-request-uri`·`mcptt-client-id` 등을 CSK 로 암호화·서명해 보내면 CSP 가 빈 값으로 읽는다(403 `140 unable to decrypt` 응답도 없다) | ◎ |
| CMS-5 | B | CSC·CSP | §8.3.2.12 — user profile 문서의 변경을 구독자에게 통지 | 문서의 `<MCPTTGroupInfo>`·`<ImplicitAffiliations>`·`<PrivateCallList>` 는 그룹 멤버십에서 나오는데, 그룹 변경은 `GROUP_CHANGED`(gms 구독자 통지)만 낸다. `UserProfile.*` 설정 재적재도 통지가 없다(`mcptt.py:364-369`) | user profile 을 그룹 목록의 원천으로 쓰는 규격 단말은 편성 변경을 재로그인 전까지 모른다 | ○ |
| CMS-6 | C | CSC | §8.3.2.1 11)xxxviii) — 청취 인가 요소는 anyExt 의 `<allow-request-remote-initiated-ambient-listening>`·`<allow-request-locally-initiated-ambient-listening>` | `<allow-ambient-listening>` 을 규격 이름공간으로 싣는다 — `mcptt.py:1753`. 이 이름은 TS 24.484·24.379·24.481 어디에도 없다 | 규격 단말은 모르는 요소다. mcptt_authorization.md·dispatch_center.md·CLAUDE.md 가 «TS 24.484 `allow_ambient_listening`» 으로 인용한다 — 자체 확장(`cims:`)으로 옮기거나 규격 요소 이름으로 | ◎ |
| CMS-7 | A | CSC | §7.2.2.6 — `<HPLMN PLMN>` 은 유효한 PLMN 코드 | 도메인에서 유도할 때 MNC 의 앞자리 0 을 전부 지운다 — `plmn = m.group(2) + m.group(1).lstrip('0')`(`mcptt.py:1899`). `mnc008.mcc450` → `4508`, `mnc012.mcc310` → `31012` | MNC 가 0X·00X 인 사업자 도메인에서 무효하거나 다른 PLMN 을 광고한다(`mnc033` 처럼 0 이 하나면 맞는다) | ◎ |
| CMS-8 | C | CSC | §8.3.2.1 — `<entry>` 는 `index` 속성을 가진다. 10)b) — `<MCPTTGroupInfo>` 하나 | entry 에 `index` 가 없고(`mcptt.py:1631-1635`) 그룹이 없으면 `<MCPTTGroupInfo>` 를 뺀다(`:1727-1728`) | `index` 를 키로 읽는 단말에서 entry 해석 실패 가능 | ○ |
| CMS-9 | B | CSC | §5.2 — CMS 는 문서 생성·수정·삭제와 요소 단위 절차를 지원 | user profile·service config 핸들러가 메서드를 가리지 않는다 — PUT·DELETE 에도 200 + 문서 — `mcptt.py:2945-3017` | 규격 CMC 의 수정이 «성공한 것처럼» 보인다 | ○ |
| CMS-10 | C | CSC | TS 24.482 A.2.3 — Bearer 가 없으면 403 | 401 + `WWW-Authenticate: Bearer` — `mcptt.py:1322-1327` | RFC 6750 관행과는 맞다. 401·403 분기가 규격과 반대 | ○ |
| CMS-11 | C | CSC | §7.2.1.1 — 만들어 준 UE initial configuration 의 `<mcptt-UE-id>` = 그 단말의 UE ID | 어떤 XUI 로 와도 같은 전역 문서이고 요소가 없다 — `mcptt.py:1925-1956` | UE ID 일치를 확인하는 단말이면 거절 가능 | ○ |
| CMS-12 | D | CSC | TS 24.482 A.2.1.2 — 단말은 home HTTP proxy 로 XCAP 을 보낸다 | `<http-proxy>` 기본값이 빈 문자열 — `mcptt.py:199`·`:1941` | 프록시 주소로만 보내는 단말은 접속할 곳이 없다(공개 base URL 을 넣으면 해소) | △ |
| CMS-13 | C | 관제 | §5.1 · §4.2.2.1.1 — 단말은 설정 문서의 변경을 구독한다 | Windows 관제 앱은 gms 축만 구독하고 CMS 는 5분 폴링 — `DispatchSession.cs:515-556` | 인가 변경이 최대 5분 늦게 버튼에 반영된다 | ○ |

### 3.13 신원 관리 (IDM) — TS 24.482 · TS 33.180 부록 B

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| IDM-1 | A | CSC | TS 24.482 §4.1 — access token 은 그 사용자가 인가된 서비스로 scope 가 정해진다. `mcptt_id` = MCPTT 사용자의 ID | MCPTT ID 를 ptt → volte → voip 순 첫 회선에서 만든다 — `csc/src/services/mcptt.py:634-641`. scope 의 사용자 검사는 MCVideo 뿐 | 전화 전용 가입자가 `3gpp:mc:ptt_*` scope 와 `mcptt_id`(전화번호) 토큰으로 CMS·GMS·KMS 를 연다(KMS 키 프로비저닝 포함) | ◎ |
| IDM-2 | C | CSC | TS 33.180 B.4.2.2 — `response_type`·`client_id`·`state`·`acr_values` REQUIRED | `acr_values` 는 읽지 않고, `client_id` 가 없으면 `MCPTT_UE` 로 채우며, `state`·`response_type` 이 없어도 통과 — `mcptt.py:2097-2107` | 받아들이는 쪽이 넓다(적합성 시험의 거절 케이스 실패) | ◎ |
| IDM-3 | A | CSC | B.3 — 클라이언트는 IdM 서버에 등록돼 있어야 하고 `client_id` 는 등록 값과 같아야 한다 | 등록 저장소가 없다 — 아무 `client_id` 나 받는다. `redirect_uri` 허용 목록은 전역 하나이고 비면 전부 허용 | 임의 클라이언트가 임의 redirect 로 코드·토큰을 받는다 | ○ |
| IDM-4 | A | CSC | B.4.2.4 — 토큰 요청의 `redirect_uri`·`client_id` REQUIRED, `redirect_uri` 는 인증 요청과 같아야 한다 | 없으면 통과(«있으면» 만 대조) — `mcptt.py:2293-2326` | 코드 가로채기 방어 한 겹이 없다 | ○ |
| IDM-5 | A | SDK | B.11.1 — 단말은 ID token 을 검증한다. B.4.2.3 — `state` 가 다르면 코드를 버린다 | `id_token` 은 보관만 하고(iss·aud·exp·nonce 검사 없음) `state` 를 응답과 비교하지 않는다 — `sdk/core/src/csc/csc_client.cpp:111-136` | 다른 사용자·발급자의 응답을 거르지 못한다(TLS 검증에만 기댄다) | ○ |
| IDM-6 | B | CSC | B.2.2.1 — 토큰은 JSON web **digital signature** 프로파일(예시 RS256) | HS256(공유 비밀 MAC)뿐 — `mcptt.py:1221`·`:1236`·`:3151` | 단말·분리 배치된 리소스 서버가 서명을 검증할 수 없다. RS256 만 받는 규격 단말은 ID token 검증에서 실패한다 | ◎ |
| IDM-7 | D | CSC | B.5.3 — refresh 때 계정 유효성을 다시 보고 아니면 회수 (RECOMMENDED) | refresh 는 저장된 값으로 바로 재발급한다. 계정 삭제·비밀번호 변경이 refresh token 을 회수하지 않는다 — `mcptt.py:2370-2431` | 삭제·정지된 가입자가 refresh 수명(기본 7일) 동안 토큰을 받는다 | ○ |
| IDM-8 | C | CSC | B.4.2.5 · RFC 6749 §5.1 — 토큰 응답에 `Cache-Control: no-store` | 토큰 응답 200 에 헤더가 없다 — `mcptt.py:2360-2367`·`:2424-2431` | 중간 캐시에 토큰이 남을 수 있다 | ○ |
| IDM-9 | C | CSC | B.12 — IdM·CMS 구간 TLS 필수 | 인증서 파일이 없으면 평문 HTTP 로 뜬다(로그 한 줄) — `csc/src/csc_app.py:309-345` | 인증서가 빠진 배포에서 자격·토큰이 평문으로 오간다(lifecycle 엔진이 인증서를 보증하는 배포에서는 나지 않는다) | △ |

## 4. 미구현 기능 목록에 빠진 것

mcptt_standard_conformance.md §0-R 은 통째 미구현 기능을 나열하는데 아래는 그 표에 없다. 항목을 더한다.

| 기능 | 규격 | 지금 |
|---|---|---|
| 서비스 설정 PUBLISH·구독(poc-settings — Answer-Mode·선택한 user profile) | TS 24.379 §7.2.2~§7.2.4 · §7.3.3~§7.3.6 | 489 (REG-2) |
| 협상 모드 제휴 변경(타인 제휴 MESSAGE) · 규칙 기반 제휴 | §9.2.1.4·§9.2.1.5 · §9.2.1.7 | 없음 |
| 그룹 동적 데이터 구독(그룹 상태·호 진행·제휴 멤버) | §9.2.1.6 · §9.2.2.3.9~10 | 제휴 구독으로 잘못 받는다 (AFF-10) |
| XML 기밀성·무결성 보호(mcptt-info 요소 암호화·서명) | §4.8 · §6.6 | 처리 코드 없음 — 문서가 «꺼짐» 을 알려야 한다 (CMS-4) |
| 우선순위 공유 · MCPTT gateway server | §6.7 · §5.5·§6.8 | 없음 |
| 호 없는 임박 위험 상태 해제 MESSAGE | §10.1.6 | CSP MESSAGE 분기에 없어 보인다(응답 코드 미확인) |
| 애드혹 그룹 긴급 경보 | §12.1A | 없음 |
| 애드혹 참가자 변경 · 기준 기반 참가자 결정 | §17.2.6·§17.4.5 · §17.3.6·§17.4.6 | 200 무동작 (ADH-6) |
| 원격 긴급 발언 요청 트리거 | §18 | 없음 |
| Floor Revoke Request(남의 발언 회수) | TS 24.380 §6.2.4.3.10 · §6.3.5.4.15 | 버린다 (FCS-12) |
| audio cut-in 그룹 · 수신 전용 멤버 · 동시 발언 허용 목록 | TS 24.380 §6.3.2.2 · TS 24.481 §7.2.2 | 없음 (FCS-13·FCS-14) |
| GMS — global tree(byGroupID) · 요소 단위 XCAP · 멤버 제외 조회(POST) | TS 24.481 §6.2.2.2 · §6.3.6~§6.3.12 · §6.3.16 | 없음 (GMS-1·5·6) |
| CMS — 문서 생성·수정·삭제 · 요소 단위 절차 | TS 24.484 §6.3.2~§6.3.12 | GET 만(다른 메서드에 200 — CMS-9) |

## 5. 문서 정정

문서가 «정합» 이라고 적었거나 사실로 적은 것이 코드·규격과 다른 곳이다. 코드를 고치든 편차로 남기든, 문서의 서술은 바로잡아야 한다.

| 문서 | 적힌 것 | 실제 | 항목 |
|---|---|---|---|
| mcptt_standard_conformance.md C1 | `Expires: 0` = 그 사용자의 제휴 전부 해제 | 규격은 그 클라이언트의 것만 | AFF-2 |
| 같은 문서 C4 | `m=application … UDP MCPTT` + `a=floorid:0 mstrm:audio` 를 TS 24.380 §12 정합으로 | 규격 표는 `udp`, `floorid` 는 규격에 없다 | SDP-1 |
| 같은 문서 C4g | «그 밖의 그룹은 멤버십이 곧 affiliation» | 규격은 제휴 멤버만 초대 | AFF-11 |
| 같은 문서 C4h | 개별 통화 대상 = `<mcptt-request-uri>`, §11.1.1.2.1.1 정합 | 규격은 resource-lists | PRV-1 |
| 같은 문서 C9 · CSP 주석 | 암시적 제휴 근거 «§7.3.2 13)» | 그 단계는 §7.3.3·§7.3.4 에 있다 | — |
| 같은 문서 F1/F2 | Floor Ack = Source + Message Type 정합 · 받은 Indicator 의 긴급·임박 비트는 tier 로 승격 | Message Type 에 ack 비트가 섞인다 · 받은 Indicator 는 쓰지 않는다 | FCS-4·FCS-11 |
| 같은 문서 F4 · cmp_media_api.md §7.7 · mcptt_timers.md §5.2 | T2 에서 긴급·임박 화자 제외 | 코드는 긴급만(emergency_modes §3.1 과는 일치) | FCS-21 |
| 같은 문서 F5 | MCPTT ID 는 `PTT_JOIN.user_uri` | MCPTT 경로는 `user_uri` 를 보내지 않는다 | FCS-5 |
| 같은 문서 §0 S3 | xcap-diff SUBSCRIBE/NOTIFY 정합 | 본문 미해석·`sel` 고정·`new-etag` 불일치 | GMS-14·GMS-15 |
| 같은 문서 §0 S4 · §3 CMS | service-config·user-profile 정합 | 문서 주소·이름이 규격과 다르고 필수 뜻을 가진 요소가 빠졌다 | CMS-1~CMS-4 |
| ptt_flows.md B4·B6 | 제휴 PUBLISH 도식의 `Event: poc-settings` · late entry 는 «UE 주도 = 규격 모델» · «서버는 개시 시 fan-out 만» · «de-affiliate 시 이탈» | poc-settings 는 489 · 규격은 서버 초대 · 합류 때마다 재초대 · 해제해도 leg 유지 | REG-2·GCS-3·GCS-4·GCS-22 |
| mcptt_broadcast_group_call.md R4 | chat = 서버가 초대하지 않음 | 서버가 10초마다 초대 | GCS-1 |
| server45_handoff.md §12.6 C12 | REGISTER Contact 의 MCPTT 태그 — «앱은 싣는다» | 앱도 싣지 않는다 | REG-1 |
| server45_handoff.md §14.1 | 구독의 3600초 갱신은 규격대로 | conference·제휴·그룹 동적 데이터 구독의 규격 값은 4294967295 | GCC-7 |
| mcptt_emergency_modes.md §2·§5 · §4.2 | 경보 인가 = 콜과 공통 판정 · 미인가 403 은 §6.3.3.1.14 대로 | 경보는 대상 일치·멤버십을 보지 않는다 · 개시 403 에 본문이 없다 | EMG-1·EMG-2·EMG-5 |
| android_ue_client.md U1·U7 | Floor Ack 정합 · Message Sequence Number 폐기 정합 | FCS-4 · FCC-5 | — |
| ue_sdk.md §4 | API 표의 `presence(uri)` | 그런 API 가 없다 | AFF-12 |
| mcptt_api.md | `on-network-allow-conference-state` 기본 true · «정원보다 많으면 400» | 규격 기본 false · XCAP 경로에는 그 검사가 없다 | GMS-8·GMS-11 |
| mcptt_authorization.md · dispatch_center.md · db_schema.md · CLAUDE.md | «TS 24.484 `allow_ambient_listening`» | 규격에 없는 요소 이름 | CMS-6 |
| csp.md · mcptt_csp_cmp_roadmap_contract.md · `sdk/core/include/cimsue/engine.h` 주석 | floor 없는 개별 호 = `mc_no_floor_ctrl` | on-demand 는 «m=application 없음» — SDK 발신은 실제로 그렇게 보내 CSP 판정과 어긋난다 | PRV-3 |

## 6. 묶음과 순서 (권고)

같은 자리를 고치는 것끼리 묶었다. 앞 묶음일수록 손이 적게 들고 영향이 크다.

| # | 묶음 | 항목 | 몫 |
|---|---|---|---|
| 1 | **문서 값 한두 줄** — 규격 기본값이 «켜짐» 인 요소를 명시, 깨진 산출 수정 | CMS-4 · GMS-7 · GMS-13 · GMS-11 · CMS-7 · FCS-21(문서) | .45 CSC |
| 2 | **발언권 메시지 정합** — Ack 의 Message Type · 미대기 Queue Position 254 · Granted Duration | FCS-4 · FCS-8 · FCS-20 | .45 SDK·CMP |
| 3 | **인가 구멍** | EMG-1 · EMG-2 · SDP-3 · ADH-3 · IDM-1 · IDM-3 · IDM-4 · IDM-5 | .45 CSP·CMP·CSC·SDK |
| 4 | **user profile 인가 요소와 서버 판정** — 요소를 싣고(없음 = false), CSP 가 본다, SDK 의 «없음 = 허용» 을 뒤집는다 | CMS-3 · PRV-2 · PRV-4 · PRV-8 · EMG-4 · ADH-5 | .45 CSC·CSP·SDK → Windows(콘솔 칸은 .45) |
| 5 | **Warning 코드** — 거절 사유를 규격 코드로. handoff §14 K1(SDK 가 Warning 을 올림)과 한 묶음 | GCS-7 · GCS-8 · GCS-11 · REG-3 · ADH-4 · PRV-6 · PRV-7 · EMG-5 | .45 CSP·SDK → Windows(문구 사전) |
| 6 | **service configuration 요소** — `<private-call>`·`<adhoc-group-call>`. 타이머 D5·D6 과 같은 자리 | ADH-1 · ADH-3 · PRV-5 | .45 CSC·CSP |
| 7 | **SDK 요청 규격화** — REGISTER Contact 태그, INVITE 헤더·Request-URI(PSI)·`<mcptt-client-id>`·session-type, 개별 호 resource-lists, 재합류 세션 식별자, conference·xcap-diff SUBSCRIBE. 서버가 양쪽을 받는 전환기를 먼저 둔다(PRV-1) — 서버 쪽 검사(GCS-9·GCS-10)는 SDK 뒤 | REG-1 · GCC-1~GCC-4 · GCC-7 · GCC-8 · PRV-1 · ADH-8 · GMS-16 · SDP-1 | .45 SDK·CSP |
| 8 | **제휴를 클라이언트 단위로** — 행 키 통일, 클라이언트 단위 해제·판정, 암묵 제휴 취소. handoff §14 K3·S2 와 한 묶음 | AFF-2~AFF-8 · AFF-12 · EMG-3 | .45 CSP·SDK |
| 9 | **호 모델 결정** — chat 그룹(초대 없이 합류·1명 이하 해제)과 편성 그룹의 재초대·late call entry·제휴 해제 시 이탈. 규격대로 바꿀지, 편차로 남기고 사유를 적을지 정한다 | GCS-1~GCS-4 · GCS-22 · AFF-11 | 결정 → .45 CSP |
| 10 | **그룹 문서 집행** — 정원·on-network-disabled·수신 전용·N6 | GCS-5 · GCS-6 · GCS-19 · FCS-14 | .45 CSP·CMP·CSC |
| 11 | **XCAP 규격 주소·절차** — global tree·문서 이름·교체 PUT·오류 형식·구독 본문·etag | GMS-1~GMS-6 · GMS-14~GMS-17 · CMS-1 · CMS-2 · CMS-9 | .45 CSC·CSP·SDK |
| 12 | **발언권 메시지 형식** — MCPTT ID·Audio SSRC·Indicator·dual floor·Location·Revoke Request, SDP `mc_floor_ssrc`·`mc_priority` | FCS-5~FCS-7 · FCS-10~FCS-16 · SDP-2 · SDP-3 | .45 CMP·CSP·SDK |

**Windows 몫(관제 앱 두 벌)** — SDK·서버가 정해진 뒤 맞춘다.

- GCC-6 — 거절을 480 + Warning 110 으로(코어 `reject` 가 Warning 을 받게 된 뒤).
- EMG-8 — 긴급 개시·상향·경보의 대상 그룹 판정(전용 긴급 그룹).
- GMS-11·GMS-18 — 그룹 편집 폼: 정원 0(무제한)을 되돌릴 수 있게, 우선순위 범위.
- CMS-13 — CMS 변경 구독(지금은 5분 폴링).
- 묶음 4·5 가 들어오면 Capabilities 게이트와 응답 문구 사전.

## 7. 보지 못한 것

- **MCData(TS 24.282)·MCVideo(TS 24.281·24.581)** — 각자의 목록: [mcdata_conformance_gaps.md](mcdata_conformance_gaps.md) · [mcvideo_conformance_gaps.md](mcvideo_conformance_gaps.md).
- **통째 미구현 절** — pre-established session, call-back, first-to-answer, 원격 개시, ambient listening, regroup, functional alias, MBMS, off-network, 위치 관리, 긴급 개별 호의 단말 절차 세부, 애드혹 긴급·임박.
- **floor SRTCP(TS 24.380 §13)·KMS(TS 33.180 본문)** — placeholder 로 문서에 있어 내부를 읽지 않았다. 토큰 교환·파트너 도메인(부록 B.7~B.9)도.
- **OMA XDM Group·RFC 원문 일부** — 그룹 문서의 OMA 스키마 시퀀스, RFC 6665·OIDC Core 의 해당 절은 규격 폴더에 없어 대조하지 못했다(AFF-9·IDM-6 의 근거 일부).
- **실행 확인** — 모든 항목이 코드 읽기다. △ 표시 항목과 PRV-3(전이중 개별 호)·AFF-5(제휴 행 키)는 실서버로 재현해 확정한다.
- **cspsim·계측기(libcsim)** — 시험 도구의 MCPTT 송신 형태는 보지 않았다. 서버 쪽 검사를 켜면(GCS-9·GCS-10·REG-3·PRV-1) 도구도 함께 맞춰야 한다.
- **콘솔·현장 앱 화면 규칙** — 인가 요소가 늘면(묶음 4) 콘솔 가입자 프로파일 칸이 따라가야 한다.
