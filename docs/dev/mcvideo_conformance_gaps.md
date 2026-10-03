# MCVideo 규격 정합 보완 목록

MCVideo(그룹 영상) 구현을 3GPP 규격 원문과 대조해 **규격과 다른 지점**을 모은 목록이다. 고친 것은 없다 — 찾아서 적은 것이다.
이미 문서에 적힌 편차·결정과 통째 미구현(V8) 기능은 여기 다시 싣지 않는다(§0 «이미 있는 판정»). 설계 정본은
[mcvideo.md](../design/features/mcvideo.md) 이고, 이 목록의 항목이 반영되면 그 문서(§5·§9)를 갱신하고 여기서 지운다.
MCPTT 쪽 같은 뿌리의 결함은 [mcptt_conformance_gaps.md](mcptt_conformance_gaps.md) 에 있다 — MCVideo 가 코드 경로를 따로 가진 것만 여기에 싣고 짝 항목 번호를 적는다.

## 0. 범위·방법·표기

**대조 기준**

| 규격 | 판 | 범위 |
|---|---|---|
| TS 24.281 | V18.14.0 | 호 제어 — 등록·서비스 인가(§7) · 제휴(§8) · 그룹 호(§6.2·§6.3·§9.2) · Warning(§4.4) · mcvideo-info(부록 F.1) |
| TS 24.581 | V18.8.0 | 전송·수신 제어 — 참여자(§6.2.4·§6.2.5) · 서버(§6.3.4~§6.3.7) · 메시지(§9) · 타이머·카운터(§11) · SDP(§4.3·§12·§14) |
| TS 24.481 | V19.3.0 | 그룹 문서 MCVideo 몫(§7.2.2·§7.2.8) |
| TS 24.484 | V20.0.0 | UE initial configuration(§7.2) · MCVideo user profile(§9.3) · service configuration(§9.4) · UE configuration(§9.2) |
| TS 23.281 | V18.12.0 | 필요한 곳만 |

코드 = `main` `8cd89604` 에 handoff §14·§16 반영분을 얹은 트리(이 목록과 같은 변경 묶음). 서버(CSP·CMP·CSC)와 단말(SDK `sdk/core`·현장 앱 `android/ptt-client`·관제 앱 두 벌)·
골든(`tests/fixtures/mcvideo/`)을 함께 봤다. 전부 **코드 읽기**다 — 실서버·실기·와이어 캡처로 확인한 항목은 없다.

**이미 있는 판정 (여기 싣지 않음)** — mcvideo.md §1.3~§1.6(문서·SDP 규약) · §5.2.1 표 · §5.3.1 «규격을 읽은 방식» · §5.4 참여자 구현 ①~⑤·«송출 SSRC»·«영상 없는 엔진 빌드» ·
§7 D1~D12 · §9 판본·불일치 메모 · §6 V8 후속 · [mcvideo_dev_plan.md](mcvideo_dev_plan.md) §7 R1~R7·§8 · [ue_sdk.md](../design/features/ue_sdk.md) §4.6 편차 표 ·
[cmp_media_api.md](../api/cmp_media_api.md) §7.9 · [mcx_identity_scope.md](../design/features/mcx_identity_scope.md)(CSP 토큰 검증 §10) ·
[mcx_e2e_security.md](../design/features/mcx_e2e_security.md)(GMK·CSK·전송 제어 SRTCP 키 — placeholder).

**급**

| 급 | 뜻 |
|---|---|
| **A** | 우리 단말·서버끼리도 오동작하거나, 인가·보안·운영에 구멍이 난다 |
| **B** | 규격 단말·규격 서버와 붙이면 그 절차가 성립하지 않는다 |
| **C** | 규격의 shall 과 다르지만 영향이 작거나 받아들이는 쪽이 넓은 것 |
| **D** | 권고(should·may)·정의 누락 |

**확인** — ◎ 규격 원문 줄과 코드 줄을 둘 다 직접 읽어 확인 · ○ 한쪽은 직접 읽고 다른 쪽은 추론·간접 확인 · △ 실측이나 추가 원문 확인이 있어야 확정.

**대상** — CSP·CMP·CSC = 서버 · SDK = `sdk/core` · 현장 = `android/ptt-client` · 관제 = `windows/dispatch-desktop` + `android/dispatch-tablet` · 앱 = 셋 다 · 콘솔 = `ems/service/console`.

## 1. 요약

| 영역 | 항목 | A | B | C | D |
|---|---|---|---|---|---|
| 등록·서비스 인가 (VREG) | 1 | — | — | — | 1 |
| 제휴 (VAFF) | 6 | 1 | 1 | 4 | — |
| 그룹 호 — 서버 (VGC) | 5 | — | 1 | 4 | — |
| 그룹 호 — 단말 (VGU) | 2 | — | — | — | 2 |
| 송출 제어 — 서버 (TCS) | 3 | — | — | 1 | 2 |
| 송출 제어 — 단말 (TCU) | 1 | — | — | 1 | — |
| 그룹 문서 (VGMS) | 1 | — | 1 | — | — |
| **계** | **19** | **1** | **3** | **10** | **5** |

확인 수준 — ◎ 9 · ○ 8 · △ 2.

읽는 순서 — §2(먼저 볼 것) → §3(영역별 전체) → §4(미구현 목록에 빠진 기능) → §5(문서 정정) → §6(묶음과 순서).

## 2. 먼저 볼 것

급 A 와, 급 B 가운데 규격 단말과의 연동을 통째로 막거나 손이 적게 드는 것이다. 번호는 §3 의 항목 번호.

**급 A — 우리끼리도 난다**

| 항목 | 내용 |
|---|---|
| VAFF-1 | 제휴 `Expires: 0` 이 그 클라이언트가 아니라 사용자의 모든 클라이언트 제휴를 지운다 — 같은 MCVideo ID 의 다른 단말이 prearranged 초대에서 빠진다(MCPTT AFF-2 와 같은 코드) |

**규격 단말·서버와 붙이면 막히는 것**

| 항목 | 내용 |
|---|---|
| VGC-2 | late call entry 가 없다 — 개시 뒤에 제휴·재등록한 멤버는 진행 중 prearranged 영상 호를 받지 못한다 |
| VAFF-2 | 제휴 판정이 사용자 단위다 — 제휴하지 않은 클라이언트로 개시·합류가 되고, 제휴한 단말 대신 마지막 등록 단말이 초대된다 |
| VGMS-1 | 그룹 문서 PUT 이 빠진 MCVideo 요소를 «기존값 유지» 로 읽는다 — 규격 기본값(없음 = chat·보호 켬·conference 불허)과 반대로 저장된다 |

**한두 줄로 끝나는 것**

| 항목 | 내용 |
|---|---|

## 3. 영역별 목록

### 3.1 등록·서비스 인가 (VREG) — TS 24.281 §7

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| VREG-4 | D | CSP | §7.1 · §7.3.5 3) NOTE «Removal of MCVideo service settings includes removal of all group affiliations» | poc-settings Expires 0(§7.3.5)은 설정·바인딩과 그 사용자의 MCVideo 제휴를 지운다(S25 단계 A). 태그를 뺀 재-REGISTER 는 서비스 인가 바인딩·바인딩 능력만 지우고 `mcvideo_affiliations` 행은 등록 해제 때만 지운다(`csp/DbManager.cpp:564-571`) | 로그오프한 클라이언트가 제휴 NOTIFY·N2 계산에 남는다(초대 대상 선별은 `m_bMcVideo` 가 걸러 영향 없음). 규격의 해제 계기는 poc-settings Expires 0 이라 REGISTER 로그오프에 같은 뜻을 줄지는 원문 재확인 | △ |

### 3.2 제휴 (VAFF) — TS 24.281 §8

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| VAFF-1 | A | CSP | §8.2.2.2.3 15)·16) — Expires 0 은 **그 클라이언트**(pidf tuple id)의 제휴 목록만 해제 | `Expires: 0` = `RemoveAffiliationsByUser` — 그 사용자의 모든 클라이언트 행(설정 그룹·chat 암묵 제휴 포함)을 지운다(`csp/CscfModule.cpp:1977-1991`, `csp/DbManager.cpp:1117-1124`). MCPTT AFF-2 와 같은 코드 | 한 MCVideo ID 를 두 클라이언트에서 쓰면(§6.8) 한쪽의 전체 해제가 다른 쪽 제휴를 지워 prearranged 초대가 끊긴다. mcvideo.md §5.5 는 이 동작을 사실로 적고 앱이 피해 가게 했지만 편차로 적지 않았다 | ◎ |
| VAFF-2 | B | CSP | §6.3.6 3. · §8.2.2.2.11 2) — 제휴 판정은 MCVideo ID × **클라이언트**(INVITE `<mcvideo-client-id>`) | `IsAffiliated(group, user, McVideo)` 가 client_id 를 보지 않는다(`csp/DbManager.cpp:1023-1039`). 개시·합류 판정 `csp/McVideoCallService.cpp:648`, prearranged 팬아웃은 제휴 사용자의 등록 바인딩 하나로 보낸다(`:791`·`:425`) | 제휴하지 않은 클라이언트로 개시·합류가 되고, 제휴한 단말이 아니라 마지막 등록 단말이 초대된다(MCPTT AFF-4 의 MCVideo 경로) | ◎ |
| VAFF-4 | C | CSP | §8.2.2.2.13 · §8.2.2.2.14 — 암묵 제휴는 2xx 뒤에 «affiliated» + 통지, 요청이 거절되면 그 항목을 지운다 | chat 합류의 암묵 제휴를 정원 검사 직후 기록·NOTIFY 하고(`csp/McVideoCallService.cpp:721-729`) 뒤의 수락 실패 500(`:817-824`)에서 되돌리지 않는다 — 이 파일에 `RemoveAffiliation` 이 없다 | 실패한 합류의 제휴가 남아 N2 를 먹고(다음 합류 486 102), 구독자는 성립 전 제휴를 본다(MCPTT AFF-6 의 MCVideo 경로) | ◎ |
| VAFF-5 | C | CSP | §8.2.2.2.12 1) — 암묵 제휴의 클라이언트 = INVITE `<mcvideo-client-id>`. 명시·암묵·설정 그룹 제휴가 같은 client information entry 를 다룬다 | 행 client 키가 경로마다 다르다 — chat 암묵 = `<mcvideo-client-id>`, 없으면 **사용자 id**(`csp/McVideoCallService.cpp:722`) · 설정 그룹 = poc-settings PUBLISH 의 `<mcvideo-client-id>`(S25) · PUBLISH = tuple id > Contact | client-id 없는 INVITE 로 생긴 행은 클라이언트 단위 해제(`:2081`)로 지워지지 않고 NOTIFY tuple id 가 사용자 id 로 나간다(MCPTT AFF-5 와 같은 결). 우리 SDK 는 client-id 를 싣는다(골든 03) | ○ |
| VAFF-7 | C | CSP | §8.2.2.3.8 — 제휴 적격 = 그룹 존재 · 멤버 · MCVideo 그룹 | 그룹 변경(멤버 제거·MCVideo 서비스 끔·그룹 삭제)이 `mcvideo_affiliations` 행에 닿지 않는다 — 그룹 동기화는 MCPTT 만 본다(`csp/GroupCallService.cpp:2757-`), MCVideo 쪽 처리기가 없다 | 빠진 멤버·MCVideo 를 끈 그룹의 제휴가 NOTIFY·N2 에 남는다(MCPTT AFF-7 의 MCVideo 경로) | ○ |
| VAFF-8 | C | SDK·앱 | §8.2.1.1 · §8.2.1.3 — 제휴 결과는 상태 결정 SUBSCRIBE(`Event: presence`, MCVideo ICSI, Expires 2^32-1)의 NOTIFY 로 안다 | MCVideo 제휴 SUBSCRIBE 가 없다 — PUBLISH 200 만 보고 제휴됐다고 여긴다(`sdk/core/src/engine.cpp:3123-3161`). 현장 앱 주석도 «결과는 NOTIFY 로만 온다» 고 적는다(`android/ptt-client/src/main/java/com/cims/ue/ptt/PttVideo.kt:160-162`) | 서버가 N2 로 줄인 그룹·MCVideo 를 끈 그룹·자격 없음을 단말이 모른다. 관제 앱 두 벌은 N2 를 자체 계산으로 대신한다 | ◎ |

### 3.3 그룹 호 — 서버 (VGC) — TS 24.281 §6.3 · §9.2.1.3~4 · §9.2.2.3~4

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| VGC-2 | B | CSP | §9.2.1.4.6 — 새로 제휴했거나 돌아온 클라이언트를 진행 중 prearranged 호에 초대(late call entry) (shall) | MCVideo 제휴 PUBLISH(`csp/CscfModule.cpp:2060-2087`)·설정 그룹 암묵 제휴(`:873-883`) 어디에도 진행 중 세션 초대가 없다. 팬아웃은 새 세션 개시 때 한 번(`csp/McVideoCallService.cpp:786-815`) | 개시 뒤에 제휴·재등록한 멤버는 그 호를 받지 못한다 — 세션 식별자를 모르면 재합류도 못 한다(MCPTT GCS-4 의 MCVideo 경로) | ◎ |
| VGC-8 | C | CSP | §6.3.3.1.1 1)a)·2) — 초대 offer 의 영상 미디어 = 개시자 offer 의 그 m-line(미디어 속성 전부), 진행 중 세션이면 그 세션이 쓰는 스트림 | 초대 offer 를 코덱 테이블 AMR-WB + psip 고정 H.264 로 짓는다(`csp/McVideoCallService.cpp:436-445`) — 개시자 offer 의 profile-level-id·해상도·`a=` 속성이 옮겨지지 않는다 | 개시자와 초대 멤버의 영상 형식이 다를 수 있다(CMP 는 트랜스코딩하지 않는다) | ○ |
| VGC-10 | C | CSP·CSC | §9.2.3.4.1 — MCVideo 세션 식별자로 온 conference 구독은 그 MCVideo 세션의 것이고, 그룹 문서 `mcvideo-on-network-allow-conference-state` 로 판정(불허 403 `138`) | `Event: conference` 분기가 서비스를 가르지 않는다 — R-URI 사용자부(포커스 Contact 의 사용자부 = 그룹 id)로 MCPTT conference 구독이 된다(`csp/CscfModule.cpp:1364-1366`). `bAllowConferenceState` 는 적재만 한다. CSC 는 그 요소를 기본 true 로 광고한다(`csc/src/services/mcvideo.py:46`·`:441-442`) | 규격 단말이 MCVideo 세션 참가자 정보를 구독하면 같은 그룹 id 의 MCPTT 로스터를 받거나 4xx 로 끝난다 | △ |
| VGC-11 | C | CSP | §6.8 · §8.2.2.2.2 — 한 MCVideo ID 의 여러 클라이언트는 따로 참가한다 | 같은 멤버의 새 INVITE 를 «BYE 없는 재합류» 로 보고 옛 leg 를 끝낸다(`csp/McVideoCallService.cpp:755-769`) — 클라이언트를 가리지 않는다. CMP 멤버 키도 (그룹, 사용자)다 | 같은 신원의 두 번째 단말이 합류하면 첫 단말이 끊긴다 | ○ |
| VGC-12 | C | CSP | §9.2.1.4.4.3 — 제휴 해제 등으로 참가자를 세션에서 뺄 때 BYE | 제휴 해제 PUBLISH(`csp/CscfModule.cpp:2080-2084`)·멤버 제거·MCVideo 서비스 끔이 진행 중 MCVideo leg 에 닿지 않는다 — `McVideoCallService` 에 그룹·제휴 변경 처리기가 없다 | 그룹에서 빠진 멤버가 영상 호에 남아 보내고 받는다(MCPTT GCS-22 의 MCVideo 경로) | ○ |

### 3.4 그룹 호 — 단말 (VGU) — TS 24.281 §6.2 · §9.2.1.2 · §9.2.2.2

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| VGU-5 | D | 관제 | §9.2.1.2.1.1 · §9.2.2.2.1.1 첫 단락 — 그룹 문서 `<preconfigured-group-use-only>` true 면 호를 열지 않고 알린다 | SDK(`GroupDoc.preconfiguredGroupUseOnly` — Kotlin 포함)·현장 앱(`CallRules.groupUsable`)은 따른다. **관제 앱 두 벌은 보지 않는다**(C API `preconfigured_group_use_only`·.NET `GroupDoc.PreconfiguredGroupUseOnly` 는 있다) | 관제 앱에서는 사전 구성 전용 그룹에도 개시 INVITE 가 나간다(서버도 403 `167` 을 하지 않는다) | ○ |
| VGU-6 | D | 관제 | §9.2.1.2.4.1 — 사용자 요청·커버리지 복귀 때 세션 식별자로 재합류 INVITE | SDK 는 `VideoGroupCallOptions.sessionUri` 를 지원한다. 현장 앱은 망 끊김으로 잃은 prearranged 호를 한 번 재합류한다(`CallRules.rejoinLostSession`). 관제 앱 두 벌은 쓰지 않는다 | 관제 앱은 망이 끊긴 뒤 prearranged 영상 호로 «보기만» 돌아갈 수 없다(서버 late call entry 도 없다 — VGC-2) | ○ |

### 3.5 개별·그 밖의 호 (VPRV) — TS 24.281 부록 F.1.3 · §10~§15

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|

### 3.6 송출 제어 — 서버 (TCS) — TS 24.581 §6.3.4 · §6.3.5 · §9 · §11.1.3

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| TCS-2 | C | CMP·SDK | §6.3.4.4.2 3g · §6.3.7.3.3 3 · §6.3.7.4.5 5 — Transmission Request 에 Functional Alias 가 있으면 Media Transmission Notification 에 싣는다 (shall) | `_sendNotification` 이 별칭을 싣지 않고 송출 기록(`Tx`)에 자리도 없다(`cmp/PMcvControl.cpp:498-506`, `cmp/PMcvControl.h:162-176`). SDK Transmission Request 도 별칭을 보내지 않는다(`sdk/core/src/mcvideo/tc_codec.cpp:164-169`) | 앱의 «영상 n» 목록이 별칭 칸을 그리지만(`android/ptt-client/.../ui/VideoViews.kt:171`) 늘 비어 있고, 규격 단말이 보낸 별칭도 버려진다 | ◎ |
| TCS-8 | D | CMP | §6.3.5.7.3 NOTE — 허가 없이 계속 보내는 참여자를 포기할 때는 호에서 내보내기를 권고 | 5회 재송신 뒤 Idle/Taken 으로 되돌리고, 다음 payload 가 오면 다시 Revoked #3 — 회수가 끝없이 되풀이된다(`cmp/PMcvControl.cpp:901-911`) | 무허가 송출 단말이 남는다. mcvideo.md 는 허가된 송출의 포기만 적었다 | ◎ |
| TCS-10 | D | CSP·CMP·CSC | §6.3.4.3.3 1b · §6.3.4.4.7A 1b — `<on-network-recvonly>` 멤버의 송출 요청은 거절 #5 · §14.3.3 1. — 그 멤버 answer 에 `mc_priority` 없음 | CMP 는 JOIN `recv_only` 를 받지만 CSP 가 `CmpMcvMemberDecl::bRecvOnly` 를 채우는 곳이 없다(`csp/CmpClientMcvideo.cpp:121` 은 읽기만). CSP 그룹 모델·그룹 문서에도 그 요소가 없고, answer 는 offer 에 있으면 늘 `mc_priority` 를 싣는다(`csp/McVideoInfo.h:273-277`) | 수신 전용 멤버를 둘 수 없다(MCPTT FCS-14 와 같은 뿌리). cmp_media_api.md §7.9 의 #5 경로는 쓰이지 않는다 | ◎ |

### 3.7 송출 제어 — 단말 (TCU) — TS 24.581 §6.2.4 · §9 · §11.1.1

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| TCU-1 | C | 관제 | 표 11.1.1 — T100~T104 는 MCVideo service configuration `<tc-timers-counters-R14>` 값 | 코어·Kotlin 파사드·현장 앱은 싣는다(`Account.setTcTimers` ← `PttGroups.loadMcVideoServiceConfig`). C API(`tc_timers`·`cimsue_engine_set_tc_timers`)·.NET(`AccountConfig.TcTimers`·`Account.SetTcTimers`·`McVideoServiceConfigDoc.TcTimers`)도 있다. **관제 앱 두 벌이 문서를 받아 싣지 않는다** | 관제 앱에서는 서버가 바꾼 단말 타이머가 반영되지 않는다(서버 쪽 짝 TCS-1) | ○ |

### 3.8 수신 제어 — 서버 (RCS) — TS 24.581 §6.3.6 · §6.3.7

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|

### 3.9 수신 제어 — 단말 (RCU) — TS 24.581 §6.2.5

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|

### 3.10 SDP (VSDP) — TS 24.281 §6.2.1·§6.2.2·§6.3.3 · TS 24.581 §12 · §14

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|

### 3.11 그룹 문서 (VGMS) — TS 24.481 §7.2

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| VGMS-1 | B | CSC | §7.2.8 — 요소가 없을 때의 뜻: `mcvideo-on-network-invite-members` 없음 = false(chat) · `mcvideo-protect-media`·`-transmission-control` 없음 = true · `mcvideo-on-network-group-priority` 없음 = 최저 · `mcvideo-on-network-allow-conference-state` 없음 = false | XCAP PUT 해석이 빠진 MCVideo 요소를 None 으로 두고(`csc/src/services/mcvideo.py:499-519`) 쓰기는 기존값(새 그룹은 `GROUP_ATTR_DEFAULTS`)을 유지한다(`:230`). 기본값 `allow_conference_state` 는 True(`:46`). 보호 요소가 빠진 문서(규격상 true)를 거절하지 않는다 | 요소를 생략한 규격 문서를 PUT 하면 반대 뜻(prearranged 유지·보호 꺼짐·conference 허용)으로 저장되고 GET 이 PUT 과 달라진다 | ◎ |

### 3.12 설정 문서·CMS (VCMS) — TS 24.484 §7.2 · §9

남은 항목 없음 — 두 MCVideo 문서의 변경 통지는 [mcptt_api.md](../api/mcptt_api.md) «변경 통지».

## 4. 미구현 기능 목록에 빠진 것

통째 미구현 기능은 mcvideo.md §6(V8 행과 그 아래 «V8 에 더해» 표)에 있다 — 이 목록을 만들며 찾은 것도 그 표로 옮겼다.

## 5. 문서 정정

문서가 «정합» 이라고 적었거나 사실로 적은 것이 코드·규격과 다른 곳이다. 코드를 고치든 편차로 남기든, 문서의 서술은 바로잡아야 한다.

| 문서 | 적힌 것 | 실제 | 항목 |
|---|---|---|---|
| mcvideo.md §5.3.1 Transmission Request 행 · cmp_media_api.md §7.9 `recv_only` | 그룹 문서 `<on-network-recvonly>` 면 거절 #5 | 그룹 문서에 그 요소가 없고 CSP 가 보내지 않는다 | TCS-10 |
| mcvideo.md §5.5 PTT 단말 | «빈 집합 = Expires 0 = 그 사용자 제휴 전부 해제» 를 피하는 이유로 적음 | 그 서버 동작이 규격(클라이언트 단위 해제)과 다르다 — 편차로 적거나 서버를 고친다 | VAFF-1 |
| mcvideo.md §5.5 PTT 단말 | «영상 n» 목록(이름·기능 별칭·경과) | 별칭이 서버에서 전달되지 않아 늘 비어 있다 | TCS-2 |
| `csp/CscfModule.cpp:830` 주석 · `:1885-1889` 문서 주석 | «N2 상한은 … 적용하지 않는다» | MCVideo 는 설정 그룹·PUBLISH 모두 N2 를 적용한다(`:859-867`·`:2038-2059`) — 주석이 MCPTT 몫만 맞다 | — |

## 6. 묶음과 순서 (권고)

같은 자리를 고치는 것끼리 묶었다. 앞 묶음일수록 손이 적게 들고 영향이 크다.

| # | 묶음 | 항목 | 몫 |
|---|---|---|---|
| 4 | **제휴를 클라이언트 단위로** — 행 키 통일, 클라이언트 단위 해제·판정, 암묵 제휴 취소·완료 시점. MCPTT 묶음 8(AFF-1~8)과 한 작업 | VAFF-1 · VAFF-2 · VAFF-4 · VAFF-5 · VGC-11 | .45 CSP |
| 5 | **진행 중 호와 제휴·그룹 변경** — late call entry, 제휴 해제·멤버 제거·서비스 끔 → BYE, 제휴 행 정리. MCPTT 묶음 9·10 의 결정과 같이 | VGC-2 · VGC-12 · VAFF-7 · VAFF-6(결정) | 결정 → .45 CSP |
| 6 | **service configuration 값 결선(단말)** — 관제 앱이 MCVideo service config 를 받아 계정 타이머로 싣는다(현장 앱·Kotlin 파사드는 한다, C API·.NET 바인딩도 있다) | TCU-1 | .45 SDK·win |
| 7 | **코덱 선호의 한 줄기** — 그룹 선호 = 서버 집행 코덱(CSC·콘솔이 검증). 초대 offer 가 그 값을 쓴다(단말 offer 는 지원 encoding 을 전부 싣는다) | VGC-8 | .48 CSP |
| 8 | **그룹 문서 PUT 해석** — 없음의 뜻(§7.2.8) | VGMS-1 · VGC-10(CSC 기본값 몫) | .45 CSC |
| 9 | **단말 호 절차** — 제휴 상태 구독, preconfigured-group-use-only, 재합류 UI | VGU-5 · VGU-6 · VAFF-8 | .45 SDK·현장 → Windows(관제 앱) |
| 10 | **송출 제어 서버 세부** — 무허가 송출 포기, 별칭 전달 | TCS-2 · TCS-8 | .45 CMP(·SDK 별칭) |
| 12 | **서버 사유 코드·신원 세부** — 로그오프 바인딩 판정·제휴 정리, conference 구독 서비스 분리 | VREG-4 · VGC-10 | .48 CSP |
| 13 | **수신 전용 멤버** — 그룹 모델·문서 `<on-network-recvonly>` → CSP JOIN `recv_only`·answer `mc_priority` 생략. MCPTT FCS-14 와 같이 | TCS-10 | .45 CSC·CSP |

**Windows 몫(관제 앱 두 벌)** — SDK·서버가 정해진 뒤 맞춘다.

- 원치 않는 MCVideo 초대 — 받기 전 `reject()`·`hangup()` 은 코어가 480 + Warning 110 으로 낸다(VGU-3 반영). 태블릿은 `autoAnswerMcvideo` 를 끄고 받을지 먼저 정한다(현장 앱 `CallRules.acceptVideoInvitation` 과 같은 규칙 — 자동 200 뒤 BYE 를 없앤다).
- VAFF-8 — N2 자체 계산 대신 제휴 상태 NOTIFY 로.
- VGU-6 — 재합류(세션 식별자)로 prearranged 영상 호에 «보기만» 돌아가기.
- 묶음 6·7 이 들어오면 service config 타이머·그룹 선호 코덱 반영.

## 7. 보지 못한 것

- **V8·통째 미구현 절** — 긴급·임박·경보·방송·1:1·pull·push·ambient viewing·ad hoc·pre-established·MBMS·off-network(TS 24.581 §7·§9.3~§9.5)·non-controlling 기능(§6.5)·Track Info. 거절 사유만 봤다.
- **전송 제어 SRTCP 키 유도(TS 24.581 §13)·KMS·GMK** — mcx_e2e_security.md 에 placeholder 로 있어 내부를 읽지 않았다. 단말은 `protect-media` true 를 무시한다(D7 범위).
- **OMA list-service 실 스키마** — `tests/fixtures/mcvideo/xsd/aux-*` 는 CIMS 가 만든 보조 틀이다. 그룹 문서의 자식 순서(`list`·`ruleset`·`supported-services`·mcpttgi 확장 위치)는 원문 스키마로 검증하지 못했다.
- **XDM collection·여러 user profile** — 디렉터리 GET, `<Pre-selected-indication>` 동작.
- **영상 RTCP 동기** — CMP 가 송출자 SR 을 수신자에게 옮기지 않아(mcvideo.md B6) 단말의 음성·영상 동기(RFC 3550 §6.4.1)에 영향이 있을 수 있다. TS 24.581 밖이라 항목으로 올리지 않았다.
- **멤버 포트 배치·SDP 표 4.3.3.1-1 대조** — 하지 않았다.
- **Windows .NET/C API 바인딩 내부·태블릿 `VideoPlane.kt`·현장 `ui/VideoViews.kt` 화면** — grep 수준으로만 봤다.
- **cimsue-cli drive·libcsim(계측기)** — 시험 도구의 MCVideo 송신 형태는 보지 않았다. 서버가 서비스 인가 바인딩으로 판정하므로(S25) 도구도 poc-settings 인가를 보내야 한다(VAFF-2 도 같은 결).
- **실행 확인** — 모든 항목이 코드 읽기다. 단위시험·스모크는 돌리지 않았다. 확정하려면 실측이 필요하다:
  - VGC-10 — MCVideo ICSI 를 단 conference SUBSCRIBE 가 MCPTT 로스터를 받는지.
  - 와이어 — pjsip 이 실제로 골든과 같은 Contact·`i=`·multipart 를 내는지.
