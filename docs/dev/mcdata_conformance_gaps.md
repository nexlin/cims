# MCData 규격 정합 보완 목록

MCData(SDS·FD) 구현을 3GPP 규격 원문과 대조해 **규격과 다른 지점**을 모은 목록이다. 고친 것은 없다 — 찾아서 적은 것이다.
이미 문서에 적힌 편차와 통째 미구현 기능은 여기 다시 싣지 않는다(§0 «이미 있는 판정»). MCData 정합 상태의 정본은
[mcdata_messaging.md](../design/features/mcdata_messaging.md) 이고, 이 목록의 항목이 반영되면 그 문서를 갱신하고 여기서 지운다.

## 0. 범위·방법·표기

**대조 기준**

| 규격 | 판 | 범위 |
|---|---|---|
| TS 24.282 | V18.13.0 | Warning(§4.9) · 공통 절차(§6.2·§6.3) · 등록·서비스 인가(§7) · 제휴(§8) · SDS(§9.2.1~§9.2.3) · FD(§10.2.1~§10.2.4) · 송수신 제어(§11) · disposition(§12.2·§12.4) · 메시지 형식(§15) · 긴급 경보(§16) |
| TS 24.481 | V19.3.0 | 그룹 문서의 MCData 요소(§7.2.2 · §7.2.4.2 의미 절) |
| TS 24.484 | V20.0.0 | UE initial configuration `MCData-Service-Details`(§7.2) · MCData user profile·service configuration 요소(§10.3) |

코드 = `main` `8cd89604` 에 handoff §14·§16 반영분을 얹은 트리(이 목록과 같은 변경 묶음). 서버(CSP·CMDP·CSC)와 단말(SDK `sdk/core`·현장 앱
`android/ptt-client`·관제 앱 두 벌)을 함께 봤다. 줄 번호는 그 트리 기준이다. 전부 **코드 읽기**다 — 실서버·실기·와이어 캡처로 확인한 항목은 없다.

**이미 있는 판정 (여기 싣지 않음)** — mcdata_messaging.md §7 편차 표 전부(TLV base64 CTE · 그룹 URI 직행 · 1:1 상대 AoR 직행·게이트 없음 ·
480/404/500 · media plane 그룹만 · FD 콘텐츠 서버 고정 경로·URI 탐색 없음 · FD NOTIFICATION 미사용 · ICSI 특성 태그 대신 Content-Type 판별 ·
성공 200 · E2E 미적용 · READ·InReplyTo 미사용 · disposition 옛 형식 전환기 · 집계(TDC1) 없음 · 더미 `m=audio` · 하이브리드 배포 · a=path 광고용 ·
c-plane 임계 이중 설정) · 같은 문서 §8 잔여 과제 · [mcptt_standard_conformance.md](../design/features/mcptt_standard_conformance.md) R4(MSRP relay·MSRPS,
MCData 서비스 설정 문서) · [mcx_identity_scope.md](../design/features/mcx_identity_scope.md) §10(MCData XCAP 문서·사용자 단위 MCData 자격) ·
[mcx_e2e_security.md](../design/features/mcx_e2e_security.md)(DPPK·페이로드 보호).
MCPTT 와 뿌리가 같은 것(제휴 클라이언트 단위·비제휴 멤버 초대·서비스 인가·숫자 그룹 ID 표기)은 [mcptt_conformance_gaps.md](mcptt_conformance_gaps.md) 의
항목을 가리키고, 여기에는 MCData 쪽 증상만 싣는다.

**급**

| 급 | 뜻 |
|---|---|
| **A** | 우리 단말·서버끼리도 오동작하거나, 인가·보안·운영에 구멍이 난다 |
| **B** | 규격 단말·규격 서버와 붙이면 그 절차가 성립하지 않는다 |
| **C** | 규격의 shall 과 다르지만 영향이 작거나 받아들이는 쪽이 넓은 것 |
| **D** | 권고(should·may)·정의 누락 |

**확인** — ◎ 규격 원문 줄과 코드 줄을 둘 다 직접 읽어 확인 · ○ 한쪽은 직접 읽고 다른 쪽은 문서·간접 근거 · △ 실측이나 추가 원문 확인이 있어야 확정.

**대상** — CSP·CMDP·CSC = 서버 · SDK = `sdk/core` · 현장 = `android/ptt-client` · 관제 = `windows/dispatch-desktop` + `android/dispatch-tablet`.

## 1. 요약

| 영역 | 항목 | A | B | C | D |
|---|---|---|---|---|---|
| 등록·서비스 인가 (REG) | 4 | — | 3 | 1 | — |
| 제휴·배포 대상 (AFF) | 3 | 1 | 1 | 1 | — |
| SDS — 시그널링 평면 (SDS) | 10 | — | 4 | 3 | 3 |
| SDS — 미디어 평면 (MSRP) | 6 | — | 2 | 3 | 1 |
| disposition 통지 (DISP) | 1 | — | — | 1 | — |
| 파일 배포 (FD) | 3 | — | — | 2 | 1 |
| 설정 문서 (CFG) | 1 | — | — | 1 | — |
| 응답 코드·Warning (WRN) | 3 | — | — | 3 | — |
| **계** | **31** | **1** | **10** | **15** | **5** |

확인 수준 — ◎ 26 · ○ 2 · △ 3.

읽는 순서 — §2(먼저 볼 것) → §3(영역별 전체) → §4(미구현 목록에 빠진 기능) → §5(문서 정정) → §6(묶음과 순서).

## 2. 먼저 볼 것

급 A 와, 급 B 가운데 한두 줄로 끝나면서 규격 단말과의 연동을 통째로 막는 것이다. 번호는 §3 의 항목 번호.

**한두 줄로 끝나는 것**

| 항목 | 내용 |
|---|---|
| SDS-9 | disposition 상관 색인을 fan-out 뒤에 적는다 — 빠른 DELIVERED 가 216 으로 거절될 수 있다(실측 전 △) |

**인가·보안 구멍**

| 항목 | 내용 |
|---|---|
| AFF-2 | `require_affiliation=false` 그룹은 제휴하지 않은(해제한) 멤버에게도 SDS·FD 를 배포한다(MCPTT AFF-11 과 같은 뿌리) |

**규격 단말 연동을 통째로 막는 것**

| 항목 | 내용 |
|---|---|
| SDS-1 · CFG-1 | 규격형 SDS(Request-URI = 참여 기능 PSI, 대상 = 본문)를 받지 못한다 — CSP 는 To 로만 대상을 정하고 `<mcdata-request-uri>` 를 읽지 않는다. ue-init-config 에 MCData PSI 를 광고하면 규격 단말의 SDS 가 404 가 된다 |
| SDS-2 · MSRP-2 | 서버가 수신자에게 내는 MESSAGE·INVITE 의 mcdata-info 를 고쳐 쓰지 않는다 — 규격 단말은 그룹(`<mcdata-calling-group-id>`)·발신자(`<mcdata-calling-user-id>`)를 몰라 스레드도 disposition 통지도 못 만든다 |
| AFF-1 | MCData 제휴 PUBLISH(`mcdataPresInfo`)를 MCPTT 제휴로 읽어 MCPTT 제휴 집합을 교체한다 |
| REG-1 | SDK REGISTER Contact 에 MCData ICSI·특성 태그(`icsi.mcdata`·`g.3gpp.mcdata.sds`·`.fd`)가 없다 |

## 3. 영역별 목록

### 3.1 등록·서비스 인가 (REG) — TS 24.282 §7

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| REG-1 | B | SDK | §7.2.1 1)~3) — REGISTER Contact 에 `g.3gpp.icsi-ref` `…icsi.mcdata`, SDS 지원이면 `g.3gpp.mcdata.sds` + `…icsi.mcdata.sds`, FD 지원이면 `g.3gpp.mcdata.fd` + `…icsi.mcdata.fd` (shall) | `mcdataMsrp` 일 때 icsi-ref 목록에 `mcdata.sds` 하나만 더한다 — 기본 ICSI·특성 태그·FD 없음(`sdk/core/src/account_map.cpp:93`·`:109-113`) | 규격 IMS 코어의 iFC 가 MCData 서버로 넘기지 않고, 규격 서버는 이 단말을 MCData 클라이언트로 보지 않는다. 우리 CSP 는 이 값을 «MSRP 수신 가능» 표시로만 쓴다 | ◎ |
| REG-2 | C | CSP | §7.2.1 · NOTE 1 — 서비스별 특성 태그로 MCData 클라이언트·SDS/FD 지원을 가른다. 태그를 뺀 재등록 = MCData 로그오프 | 첫 Contact 의 첫 `+g.3gpp.icsi-ref` 값에 «mcdata» 문자열이 있으면 MSRP 배포 대상(`csp/UserMap.cpp:195-200`) — `mcdata.fd`·기본 ICSI 만 실은 단말도 MSRP INVITE 를 받는다. `g.3gpp.mcdata.sds` 태그는 보지 않는다 | FD 만 지원하는 규격 단말에 SDS 미디어 평면 INVITE 가 간다. MCData 로그오프 개념이 없다(MCPTT REG-4 와 같은 결) | ◎ |
| REG-3 | B | CSP | §7.3.2 — 제3자 REGISTER 의 mcdata-info `<mcdata-access-token>`·`<mcdata-client-id>` 로 서비스 인가·바인딩(MCData ID·client ID·IMPU). §9.2.2.3.1 3)·§12.2.2.1 3) — 바인딩이 없으면 404 + `141 user unknown to the participating function` | `mcdata-access-token`·141 을 다루는 코드가 CSP 에 없다. 발신자 = From user, 미등록이어도 Digest 신원으로 처리(PUBLISH 는 `csp/CscfModule.cpp:1712` 로그 그대로) | 규격 단말이 재인가를 시작할 신호가 없다. mcx_identity_scope.md §10 은 MCPTT 토큰 검증만 향후 과제로 적었다(MCPTT REG-3 과 같은 뿌리) | ◎ |
| REG-4 | B | CSP | §7.2.1A·§7.2.2~§7.2.3 · §7.3.3~§7.3.6 — `Event: poc-settings` PUBLISH(P-Preferred-Service `…icsi.mcdata`, Expires 4294967295/0)와 그 SUBSCRIBE 로 서비스 인가·설정 | PUBLISH 는 Event 가 `mcptt`·`presence` 밖이면 489(`csp/CscfModule.cpp:1719-1724`), SUBSCRIBE 도 489(`:1376-1383`) | PUBLISH 로 서비스 인가하는 규격 MCData 단말은 489 를 인가 실패로 본다(MCPTT REG-2 와 같은 뿌리) | ◎ |

### 3.2 제휴·배포 대상 (AFF) — TS 24.282 §8 · §6.3.4 · §6.3.5

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| AFF-1 | B | CSP | §8.2.2 3)·6) · §8.4.1.2 — MCData 제휴는 MCData 서비스의 것(P-Preferred-Service `…icsi.mcdata`, pidf 확장 ns `urn:3gpp:ns:mcdataPresInfo:1.0`), MCPTT 제휴와 별개 | `Event: presence` PUBLISH 의 서비스를 MCVideo/그 밖 둘로만 가른다(`csp/CscfModule.cpp:108-111`·`:1739-1740`) — MCData PUBLISH 는 MCPTT 제휴로 들어가 «집합 교체» 규칙으로 MCPTT 제휴 행을 바꾸고, NOTIFY 는 MCPTT 형이다. SDS 배포도 MCPTT 제휴 표를 본다(`csp/McDataGates.cpp:69-71`) | 같은 client ID 를 쓰는 규격 단말이 MCData 제휴만 갱신하면 MCPTT 제휴가 풀린다. MCData 만 해제할 방법이 없다 | ◎ |
| AFF-2 | A | CSP·CSC | §6.3.4 — «The MCData server shall only send MCData messages to affiliated group members» | `require_affiliation` 이 꺼진 그룹은 제휴 여부를 보지 않고 멤버 전원에게 배포(`csp/McDataGates.cpp:69`). C-plane·media plane 공용 | 제휴를 해제한 멤버도 그 그룹의 SDS·FD 를 받는다. mcdata_messaging.md §4 «비affiliated 멤버는 규격상 배포 대상이 아니다» 와 다르다(MCPTT AFF-11 과 같은 뿌리) | ◎ |
| AFF-6 | C | CSP | §6.3.5 — 제휴·배포는 MCData **client** 단위 | 멤버마다 `gclsUserMap.Select`→`GetCallRoute` 한 경로로만 보낸다(`csp/McDataAsModule.cpp:90-96`) — 바인딩 집합에서 고른 하나([registration_binding_set.md](../design/features/registration_binding_set.md)) | 한 MCData ID 의 단말 둘(관제 데스크톱 + 태블릿 등)이면 한쪽만 받는다(MCPTT AFF-4 와 같은 뿌리) | ○ |

### 3.3 SDS — 시그널링 평면 (SDS) — TS 24.282 §6.2.2.1 · §6.2.4.1 · §9.2.1 · §9.2.2 · §15

CSP 는 참여 기능과 제어 기능을 겸한다.

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| SDS-1 | B | CSP | §6.2.4.1 4) — Request-URI = 참여 기능 PSI. §9.2.2.2.1 3)b)ii) — 그룹 = `<mcdata-request-uri>`, 2)a) — 1:1 대상 = resource-lists. §9.2.2.3.1 4) — 참여 기능은 본문으로 제어 기능을 정한다 | 대상 = To user(psip `SipUserAgentMessage.hpp:31`) → 그룹이면 MCDATA-AS(`csp/McDataAsModule.cpp:33`), 아니면 1:1 전달(`csp/ModuleDispatcher.cpp:2669-2679`). `<mcdata-request-uri>` 는 파싱만 하고 읽는 곳이 없다(`csp/McDataCodec.cpp:295`) | To 가 PSI 인 규격 단말의 그룹·1:1 SDS 가 404. mcdata_messaging.md §7 «서버는 양쪽 모두 수용» 과 다르다(규격 단말의 To 값은 실측) | ◎ |
| SDS-2 | B | CSP | §9.2.2.4.1.1 5) — 수신자별 mcdata-info `<mcdata-request-uri>` = 수신자 MCData ID, 그룹이면 `<mcdata-calling-group-id>` = 그룹. §9.2.2.3.1 12) — `<mcdata-calling-user-id>` = 발신자. 7)·8) — P-Asserted-Identity = 제어 기능 PSI, P-Asserted-Service | fan-out 은 받은 본문·Content-Type 을 그대로 복사(`csp/McDataAsModule.cpp:94`, psip `SendSms` — 헤더 추가 없음). request-uri 는 그룹으로 남고 calling-group-id·calling-user-id 가 없다. 우리 SDK 는 request-uri·From 으로 보정한다(`sdk/core/src/mcdata/sds_codec.cpp:339-343`, `engine.cpp:1784`) | 규격 단말은 그룹·발신자를 알 수 없어 스레드를 못 묶고, §12.2.1.1 의 통지 대상(`<mcdata-calling-user-id>`)이 없어 DELIVERED 를 못 보낸다 | ◎ |
| SDS-3 | B | SDK | §9.2.2.2.1 3)b)iv) · §10.2.4.2.1 3)b)iii) — 그룹 SDS·FD 의 mcdata-info 에 `<mcdata-client-id>` (shall) | mcdata-info = `request-type`·`mcdata-request-uri` 둘뿐(`sdk/core/src/mcdata/sds_codec.cpp:155-163`) | 규격 제어 기능이 §6.3.5 제휴 판정(클라이언트 단위)을 못 한다(MCPTT GCC-3 과 같은 결) | ◎ |
| SDS-4 | C | CSP | §9.2.2.4.2 2) — mcdata-info·mcdata-signalling·mcdata-payload 가 없으면 403 + `199 expected MIME bodies not in the request` | `text/plain` 과 signalling 파트 없는 multipart 를 «text» 로 보고 본문 전체를 payload 삼아 그대로 fan-out(`csp/McDataAsModule.cpp:46-49`·`:101`) | 구버전 앱 호환(§6 배포 순서)이 편차 표·제거 조건 없이 남아 있다. 형식이 깨진 본문도 그룹 전원에게 간다 | ◎ |
| SDS-5 | D | CSP | §9.2.2.4.2 5)·6) — `<request-type>`(one-to-one-sds·group-sds·ad-hoc-group-sds)로 절차를 가른다 | request-type 을 읽지 않는다 — To 가 그룹이면 그룹 절차(`csp/McDataAsModule.cpp:33`) | `one-to-one-sds` 본문을 그룹 URI 로 보내면 그룹 배포 | ◎ |
| SDS-6 | C | SDK | §9.2.1.2 7)·8) — Application ID·Extended application ID 가 있으면 사용자용이 아니다(알리지 않음, 모르는 값이면 버림). 표 15.1.2.1-1 순서 = 0x21 → 0x22 → 8- → 7D… | SDS 파서는 8-·0x21 만 알고 0x22 에서 멈춘다(`sdk/core/src/mcdata/sds_codec.cpp:352-357`) | 앱 대상 SDS(명령·위치 등)가 사용자 말풍선으로 보이고, 0x22 뒤의 disposition 요청을 잃어 수신 확인이 안 간다 | ◎ |
| SDS-7 | D | SDK | §15.2.13 · §6.2.2.1 3) — Payload content type TEXT·BINARY·HYPERLINKS·FILEURL·LOCATION·CODED TEXT, payload 여러 개. TEXT charset = 단말 설정 또는 그룹 `<mcdata-default-charset>` | TEXT·FILEURL 만 읽고 나머지는 버린다, TEXT 가 여럿이면 마지막 것만(`sdk/core/src/mcdata/sds_codec.cpp:393-407`). charset 은 UTF-8 고정 | 규격 단말의 HYPERLINKS·CODED TEXT·LOCATION SDS 가 빈 메시지가 된다 | ◎ |
| SDS-8 | B | CSP·SDK | TS 24.481 §7.2 — MCData group ID 는 그룹 문서의 `list-service uri` 와 같은 값 | 숫자 그룹 ID 를 SDK `"tel:" + groupId`(`sdk/core/src/engine.cpp:3454`), CSP `_TelOf`(`csp/McDataAsModule.cpp:122-124`)·`_TelUriOf`(`csp/McDataMediaService.cpp:114-117`)가 `tel:123` 으로, 그룹 문서는 `tel:+123`(`csc/src/services/mcptt.py:261-270`), FILEURL 폴백은 `sip:<gid>@<도메인>`(`csp/McDataMediaService.cpp:410-416`) | 규격 단말은 같은 그룹으로 묶지 못한다. 우리 앱도 `bareId` 가 `+123`/`123` 으로 갈려 스레드가 나뉠 수 있다 — 실측(MCPTT GCS-17 과 같은 뿌리) | △ |
| SDS-9 | C | CSP | §9.2.2.4.2 4) — 제어 기능은 대화·메시지 ID 를 저장해 통지와 상관한다(§12.2.3 4)·5)) | 그룹 SDS 는 fan-out 루프를 다 돈 뒤에 색인을 적는다(`csp/McDataAsModule.cpp:87-104` → `csp/McDataGates.cpp:80-81`). 1:1 은 전달 전에 적는다(`csp/ModuleDispatcher.cpp:2688-2692`). MESSAGE 는 다중 스레드로 처리된다(psip `RecvMessageRequest(iThreadId…)`) | 멤버가 많은 그룹에서 먼저 받은 단말의 DELIVERED 가 색인보다 먼저 오면 403 216 — 발신자 ✓ 누락. 실측으로 확정되면 A | △ |
| SDS-10 | D | SDK | §9.2.1.1 1) · §11.1 2)·5) · §9.2.2.2.1 3)a) — 단말은 보내기 전에 송신 권한·그룹 크기 상한·`AllowedSDS` 를 보고 거절한다 | `sendGroupSds` 는 c-plane 임계만 본다(`sdk/core/src/engine.cpp:3447-3453`) — 그룹 문서 `allowSds`·`maxSdsSize` 를 읽어 두고(`sdk/core/src/csc/group_doc.cpp:256-271`) 쓰지 않는다 | 서버 거절(403·413)에 맡긴다 | ◎ |

### 3.4 SDS — 미디어 평면 (MSRP) — TS 24.282 §9.2.3 (TS 24.582 미확인)

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| MSRP-1 | B | SDK | §9.2.3.2.3 1)~4)·8)·9) — Contact 에 `g.3gpp.mcdata.sds`·icsi-ref, Accept-Contact 둘(특성 태그·ICSI), P-Preferred-Service, mcdata-info(`group-sds`·`<mcdata-request-uri>`·`<mcdata-client-id>`), Request-URI = PSI | Accept-Contact 는 icsi-ref 하나, 본문은 SDP 만, Request-URI = 그룹(`sdk/core/src/engine.cpp:3404-3411`). MSRP 호에는 Contact 특성 태그를 걸지 않는다(SDP 주입만 — `:1151-1166`) | 규격 서버는 그룹을 본문에서 찾으므로 성립하지 않는다. mcdata_messaging.md §7 «mcdata-info 도 포함» 은 미디어 평면에서 틀리다 | ◎ |
| MSRP-2 | B | CSP | §9.2.3.4.3 4)~11) — 배포 INVITE 에 Accept-Contact 둘, Referred-By, Contact = 세션 식별자 + isfocus·특성 태그, mcdata-info `<mcdata-request-uri>` = 수신자·`<mcdata-calling-group-id>` = 그룹, P-Asserted-Identity = 제어 기능 PSI, P-Asserted-Service-Id | Accept-Contact icsi-ref 하나·`P-Preferred-Service`·`Answer-Mode: Auto`(`csp/McDataMediaService.cpp:471-477`), mcdata-info 의 request-uri = 그룹·calling-group-id 없음(`:124-152`) | 규격 단말이 배포를 «SDS over media plane» 으로 판별(§6.2.1.2)하지 못하고 그룹을 모른다 | ◎ |
| MSRP-3 | C | CSP·CMDP | §9.2.3.4.4 끝 — 제어 기능은 첫 멤버의 200 을 받은 뒤 개시자에게 200(§6.3.7.1.23) | cmdp 수신 세션을 잡자마자 개시자에게 200(`csp/McDataMediaService.cpp:214-244`) — 수신자 INVITE 는 MSRP 수신을 마친 뒤 연다(종단·재배포) | 받을 단말이 하나도 없어도 개시자는 성공으로 본다. 종단 설계는 §4.7 에 있으나 편차 표에 없다 | ○ |
| MSRP-4 | C | SDK·CSP | §9.2.3.2.3 끝 — 단말이 전송 결과에 따라 BYE + `Reason: SIP;cause=200;text="transmission succeeded"`(실패면 cause=480 "transmission failed") | 서버가 배포를 마친 뒤 BYE(`csp/McDataMediaService.cpp:364-375`), SDK 는 5 s 안에 BYE 가 없을 때만 Reason 없이 끊는다(`sdk/core/src/engine.cpp:3340-3349`) | 규격 서버는 단말 BYE 의 Reason 으로 결과를 안다 — 우리 SDK 의 실패가 서버에 닿지 않는다 | ◎ |
| MSRP-5 | D | CSP | §9.2.3.4.4 3) — Accept-Contact 에 `g.3gpp.mcdata.sds`·ICSI 가 없으면 403 | SDP 의 `m=message …MSRP` 만 보고 받는다(`csp/ModuleDispatcher.cpp:1048`, `csp/McDataMediaService.cpp:41-48`) | 받아들이는 쪽이 넓다. 우리 SDK 가 태그를 다 싣지 않아(MSRP-1) 지금 켜면 우리 단말이 막힌다 — MSRP-1 뒤에 | ◎ |
| MSRP-6 | C | CSP | §6.2.2.2 · §10.2.4.4.1 — FD 는 `request-type` `group-fd`, FD SIGNALLING 의 disposition 요청은 원본대로 | FILEURL 폴백 본문이 `request-type` `group-sds` 에 FD SIGNALLING PAYLOAD 를 싣고, 원 SDS 의 disposition 요청을 버린다(`csp/McDataCodecBuild.cpp:59-110`) | 폴백 수신자에게서 DELIVERED 가 오지 않아 발신자 ✓ 가 빠진다. 규격 단말은 request-type 과 본문이 어긋난 요청을 받는다 | ◎ |

### 3.5 disposition 통지 (DISP) — TS 24.282 §12.2

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| DISP-1 | C | CSP | §12.2.2.1 5)·6) — UNDELIVERED 통지는 참여 기능이 메시지를 보관해 TD1 뒤 재전달하고 중계하지 않는다. DELIVERED 가 오면 보관분 삭제 | 통지 유형을 보지 않고 원 발신자에게 중계한다(`csp/McDataAsModule.cpp:184-226`) | 저장 공간 부족 등으로 못 받은 단말에 재전달이 없다. §4 의 재전달 기능이 없는 것과 한 자리 | ◎ |

### 3.6 파일 배포 (FD) — TS 24.282 §10.2 · §11.2

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| FD-1 | C | CSP·CSC | §10.2.4.4.2 7)b) · §6.7.3 — FILEURL 의 파일이 media storage function 에 없으면 403 `212`. 확인 = 제어 기능이 그 URL 에 HTTP HEAD(access token), 404 면 없음 | CSP 는 URL 이 콘텐츠 서버 base(`Setup.McData.FdUrlBase`·CSC PublicUrl)의 `/mcdata/fd/<id>` 인지만 본다(`csp/McDataGates.cpp` `McDataFdPayloadCheck` → `McDataFdUrlIsOurs`) — HEAD 를 보내지 않는다. CSC 콘텐츠 서버는 HEAD 가 없고(405) FD URL 을 요청 Host 헤더로 만든다(`csc/src/services/mcdata_fd.py`) | 없는 id 의 URL 도 배포된다(수신자 GET 이 404). 단말이 PublicUrl 과 다른 이름으로 CSC 에 붙으면 업로드 URL 이 base 와 달라 212 가 된다 | ◎ |
| FD-4 | C | CSP·SDK·현장 | §11.2 · §10.2.4.4.1 5) — 제어 기능이 파일 크기 ≤ 그룹 `<mcdata-on-network-max-data-size-auto-recv>`(1:1 = `<max-data-size-auto-recv-bytes>`)면 Mandatory download IE 를 넣는다. §10.2.1.2.2 — 단말은 그 IE 로 자동 다운로드 | CSP 는 IE 를 넣지 않는다. SDK 는 Mandatory download IE(0xA-)를 건너뛰고(`sdk/core/src/mcdata/sds_codec.cpp:376`) 앱이 그룹 문서 값과 Metadata 크기로 직접 정한다(`android/ptt-client/…/PttService.kt:556`) | 규격 단말은 우리 서버에서 자동 다운로드하지 않고, 우리 앱은 발신자가 요구한 필수 다운로드를 무시한다. 1:1 FD 는 자동 수신 기준이 없다 | ◎ |
| FD-7 | D | SDK·CSP | §15.2.17 — Metadata = RFC 5547 `file-selector-attr`(name·size·type·hash) + file-date·file-availability·file-description | `name:"…" size:N type:…` 만, `file-selector:` 접두·hash·availability 없음(`sdk/core/src/mcdata/sds_codec.cpp:137-138`, `csp/McDataCodecBuild.cpp:67`) | 엄격한 규격 파서가 크기를 못 읽으면 FD-4 자동 수신 판정이 어긋난다. RFC 5547 원문 미대조 | △ |

### 3.8 설정 문서 (CFG) — TS 24.484 §7.2

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| CFG-1 | C | CSC·CSP | TS 24.484 §7.2.2.1 14) · TS 24.282 §4.2.1 — `MCData-Service-Details/Server-URI` = 참여 MCData 기능 PSI. 단말은 SDS·FD·제휴·서비스 인가를 모두 이 PSI 로 | 기본 off(`csc/src/services/mcptt.py:206`·`:1913-1922`). mcdata_messaging.md §7 은 «모든 사이트 CSP 0.2.180 이상이면 광고» 로 적었는데, CSP 가 그 PSI 로 받는 것은 disposition 통지뿐이다 — SDS 는 404(SDS-1), MSRP INVITE 는 그룹이 아니라 403(`csp/McDataMediaService.cpp:194-197`) | 적힌 조건대로 광고하면 규격 단말의 MCData 가 전부 막힌다. 광고 조건에 SDS-1·MSRP 수용이 빠졌다 | ◎ |

### 3.9 긴급 경보 (EMG) — TS 24.282 §16

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|

### 3.10 응답 코드·Warning (WRN) — TS 24.282 §4.9

| # | 급 | 대상 | 규격 | CIMS 지금 | 영향 | 확인 |
|---|---|---|---|---|---|---|
| WRN-1 | C | CSP | §9.2.2.4.2 6)e) · §9.2.3.4.4 7)c) · §10.2.4.4.2 12)c) — 비멤버는 403 + `116 user is not part of the MCData group` | Warning 없는 403(`csp/McDataGates.cpp:57-61`, C-plane·media plane 공용). disposition 경로는 116 을 싣는다(`csp/McDataAsModule.cpp:179`) | 같은 판정이 경로마다 다르다. 단말이 403 의 사유를 가르지 못한다 | ◎ |
| WRN-2 | C | CSP | §9.2.2.4.2 6)f) · §9.2.3.4.4 7)d) — SDS 꺼짐 403 + `206 short data service not allowed for this group`. §10.2.4.4.2 12)d) — FD 꺼짐 403 + `213 file distribution not allowed for this group` | Warning 없는 403(`csp/McDataGates.cpp:50-54`) | «권한 없음» 과 «그룹에서 꺼짐» 을 가르지 못한다 | ◎ |
| WRN-3 | C | CSP | §9.2.2.4.2 6)i)iii) · §11.1 5) — 그룹 SDS 크기 초과는 403 + `217 user not authorised for SDS communications on this group identity due to message size` | 413(`csp/McDataAsModule.cpp:78-83`). media plane 은 cmdp 의 MSRP 413 뒤 BYE | 규격 단말은 413 을 일반 실패로 본다 — 받아들이는 쪽은 넓다 | ◎ |

## 4. 미구현 기능 목록에 빠진 것

mcdata_messaging.md §8 · mcptt_standard_conformance.md R4 · mcx_identity_scope.md §10 은 MCData 의 미구현 기능을 나열하는데 아래는 그 목록에 없다. 항목을 더한다.

| 기능 | 규격 | 지금 |
|---|---|---|
| MCData 서비스 인가 — REGISTER `<mcdata-access-token>`·바인딩·다중 단말·141 | TS 24.282 §7.3.2 | 없음 (REG-3) — mcx_identity_scope.md §10 은 MCPTT 토큰 검증만 적었다 |
| 서비스 설정 PUBLISH·구독(poc-settings) | §7.2.2~§7.2.4 · §7.3.3~§7.3.6 | 489 (REG-4) |
| MCData 제휴(서비스별 제휴 표·`mcdataPresInfo` NOTIFY·제휴 구독·암묵 제휴) | §8 | MCPTT 제휴로 읽는다 (AFF-1) |
| SDS 세션(one-to-one·group SDS session) | §9.2.4 | 없음 |
| 애드혹 그룹 SDS(`ad-hoc-group-sds`) · functional alias 대상(300 Multiple Choices) · regroup·TGI(`<associated-group-id>`) | §9.2.2.2.1 3A) · §9.2.2.4.2 5)b)ii)·6)b)·6)l) | 없음 |
| UNDELIVERED 재전달(TD1) | §12.2.2.1 5)·6) | 중계만 (DISP-1) |
| 파일 가용 시간(TDC2 · `<default-file-availability>`·`<max-file-availability>`) · FD NETWORK NOTIFICATION(만료) | §10.2.4.4.2 9)·13)·14) · §12.4 | 없음 — 잔여 과제 «retention/purge» 와 같은 자리지만 통지 절차는 적히지 않았다 |
| FD HTTP 종료(FD HTTP TERMINATION) · 통신 해제 | §6.2.2.4 · §10.2.4.4.2 11)·17) · §13 | 없음 |
| 연기한 FD 목록 조회(DEFERRED DATA REQUEST·RESPONSE) | §11.3 | 없음 |
| Enhanced Status · 위치 보고 · pre-established session · IP connectivity · MBMS/MBS 배포 | §14 · §17 · §18 · §7.2.1 4) · §9.2.6·§10.2.6 | 없음 |

## 5. 문서 정정

문서가 «정합» 이라고 적었거나 사실로 적은 것이 코드·규격과 다른 곳이다. 코드를 고치든 편차로 남기든, 문서의 서술은 바로잡아야 한다.

| 문서 | 적힌 것 | 실제 | 항목 |
|---|---|---|---|
| mcdata_messaging.md §7 «라우팅» 행 | 그룹 URI 직행(mcdata-info 도 포함) · «서버는 양쪽 모두 수용» | CSP 는 `<mcdata-request-uri>` 를 읽지 않아 PSI 형 SDS 는 404. 미디어 평면 INVITE 에는 mcdata-info 가 없다 | SDS-1 · MSRP-1 |
| 같은 문서 §4 | «미참여(비affiliated) 멤버는 규격상 배포 대상이 아니다» · «require_affiliation 그룹은 affiliate 멤버만» | 그 밖 그룹은 비제휴 멤버에게도 배포한다. 규격은 모든 그룹에서 제휴 멤버만 | AFF-2 |
| 같은 문서 §4 게이트 표 | «controlling function 검사, TS 24.282 §9.2.2» — 403 · 403 · 413 | 규격 응답 = 403 + 206 · 403 + 116 · 403 + 217 | WRN-1~3 |
| 같은 문서 §4.7 · `csp/McDataAsModule.cpp:52` | 203 거절 근거 «TS 24.282 §9.2.2 step 8» | §9.2.2.3.1 8) | — |
| mcptt_standard_conformance.md C4e · `csp/McpttInfo.h:179` · `csp/McDataAsModule.cpp:58` | Warning 형식 «TS 24.282 §4.4» | V18 의 Warning 은 §4.9(§4.4 = Emergency Alerts) | — |
| mcdata_messaging.md §2 표 `max_auto_recv` · §4.5 | «수신 단말 파일 자동 다운로드 임계» · «수신 앱: 그룹문서 max-data-size-auto-recv 이내면 자동 다운로드» | 규격 의미는 서버가 Mandatory download 를 붙이는 임계(§11.2, TS 24.481 §7.2.4.2), 단말은 그 IE 를 따른다 | FD-4 |
| mcdata_messaging.md §3 · §5 | 코덱 = `android/ptt-client/…/mcdata/McDataCodec.kt`(+`McDataCodecTest.kt`), `PttService.threadKeyOf`·`sendGroupAttachment`·`mcdata/msrp/MsrpSession`·`debug.cims.msrp.*` | 그 파일·함수가 없다. 현장 앱은 SDK(`libcimsue` `mcdata/sds_codec`·`mcdata/msrp`, `PttMessaging.kt`)로 옮겼다 — `android/core-sip` 의 `makeMsrpInvite`·`acceptMsrpCall` 만 남았다 | — |
| mcdata_messaging.md §7 «disposition 통지 — 단말» | 전환기 종료 = 모든 사이트 CSP 0.2.180 이상이면 CSC 에서 MCData 를 광고 | 광고하면 규격 단말의 SDS·MSRP·제휴도 그 PSI 로 온다 — CSP 는 SDS·MSRP 를 받지 못한다 | CFG-1 |
| mcx_identity_scope.md §10 | MCData XCAP 문서는 «규격 MCData 클라이언트가 요구할 때» | MCData user profile 이 없으면 규격 단말은 `<allow-transmit-data>` 없음 = 1:1 송신 금지(§11.1 1))로 읽고, 콘텐츠 서버 주소(`<MCDataContentServerURI>`, §10.2.2.1)를 모른다 — 규격 단말의 1:1 SDS·FD 전부의 전제다 | — |

## 6. 묶음과 순서 (권고)

같은 자리를 고치는 것끼리 묶었다. 앞 묶음일수록 손이 적게 들고 영향이 크다.

| # | 묶음 | 항목 | 몫 |
|---|---|---|---|
| 2 | **인가·보안** — FILEURL 파일 존재 확인(212 HEAD), 제휴 멤버만 배포 | FD-1 · AFF-2 | .45 CSP |
| 3 | **응답 코드·Warning** — MCPTT 묶음 5 와 한 묶음(같은 `McpttWarning`) | WRN-1~3 · SDS-4 | .45 CSP |
| 4 | **서버가 내는 본문 규격화** — 수신자별 mcdata-info(request-uri = 수신자, calling-group-id, calling-user-id), PAI·P-Asserted-Service, MSRP 배포 INVITE 헤더, 폴백 FD 본문 | SDS-2 · MSRP-2 · MSRP-6 · SDS-8 | .45 CSP |
| 5 | **규격형 요청 수용** — 서버가 PSI 형·그룹 URI 형을 둘 다 받는 전환기를 먼저 둔다. 서버 쪽 검사(MSRP-5)는 SDK 뒤 | SDS-1 · SDS-5 · CFG-1 → SDK: REG-1 · SDS-3 · MSRP-1 · SDS-10 | .45 CSP → SDK |
| 6 | **FD Metadata 형식** — `file-selector:` 접두·hash (SDK 는 규격형 업로드·Location 사용과 함께 — U05) | FD-7 | .45 SDK · CSP |
| 7 | **자동 수신** — CSP 가 Mandatory download 를 붙이고 SDK·앱은 그 IE 를 따른다 | FD-4 | .45 CSP·SDK → 앱 |
| 8 | **제휴 서비스 분리** — MCData 제휴 표·`mcdataPresInfo`, 클라이언트 단위. MCPTT 묶음 8 과 한 묶음 | AFF-1 · AFF-6 · REG-2 | .45 CSP·SDK |
| 9 | **서비스 인가·설정** — MCPTT REG 묶음과 한 묶음 | REG-3 · REG-4 · §4 앞 두 줄 | .45 CSP·SDK |
| 10 | **수신 파서** — Application ID·Extended application ID·content type·charset | SDS-6 · SDS-7 | .45 SDK |
| 11 | **미디어 평면 수명** — 첫 멤버 응답 뒤 200, 단말 BYE + Reason. TS 24.582 확보 뒤 | MSRP-3 · MSRP-4 | .45 CSP·CMDP·SDK |
| 12 | **나머지** — 색인 순서(실측 뒤), UNDELIVERED | SDS-9 · DISP-1 | .45 CSP |

**Windows 몫(관제 앱 두 벌)** — SDK·서버가 정해진 뒤 맞춘다.

- FD-4 — 데스크톱은 자동 다운로드를 하지 않으므로([받기]) Mandatory download 가 오면 바로 받는 규칙만 더한다. 태블릿은 그룹 문서 값 대신 IE 를 따른다.
- SDS-6·SDS-7 — 앱 대상 SDS(Application ID)는 말풍선으로 그리지 않는다, HYPERLINKS·LOCATION 표시.
- 묶음 3 이 들어오면 SDS·FD 거절 문구 사전(116·206·213·217·120·198).
- 묶음 6 이 들어오면 FD 업로드 결과의 URL 출처(Location)만 바뀐다 — 앱 코드는 SDK `uploadFd` 를 그대로 쓴다.

## 7. 보지 못한 것

- **TS 24.582 (MCData media plane)** — 원문이 없다. MSRP 항목은 TS 24.282 시그널링 절차만으로 판정했고, cmdp 의 MSRP 프레이밍(RFC 4975 청크·Success-/Failure-Report·REPORT)·signalling/payload 를 SEND 두 건으로 나눠 보내는 방식·in-band disposition 은 대조하지 않았다.
- **TS 23.282 (MCData stage 2)** — 규격 폴더에 없다. 종단(store-and-forward) 배포의 근거 대조(MSRP-3)는 하지 못했다.
- **TS 24.483 (MO)·TS 29.582(연동)·TS 33.180 MCData 절(DPPK·SPK·서명)** — E2E 미적용이 문서에 있어 보지 않았다.
- **RFC 원문** — RFC 5547(file-selector ABNF)·RFC 4975·RFC 6135·RFC 3841 은 폴더에 없어 기억으로 판정했다(FD-7 은 △).
- **TS 24.484 MCData user profile·service configuration 요소별 대조** — 문서 자체를 서빙하지 않아(로드맵) 요소마다 보지 않았다. 결과는 §5 의 mcx_identity_scope.md 줄에 적었다.
- **off-network(§9.3·§12.3·§16.3)·MBMS/MBS** — 범위 밖.
- **From 신원 결박** — 게이트가 From user 를 발신자로 쓰는데, 그 신원과 등록 flow 의 결박은 MCPTT 와 같은 경로(`EventIncomingRequestAuth`)라 보지 않았다.
- **실행 확인** — 모든 항목이 코드 읽기다. △ 항목(SDS-8·SDS-9·FD-7)과 SDS-1(규격 단말이 To 에 무엇을 싣는지)·AFF-6(다중 단말 배포)은 실서버로 재현해 확정한다.
- **cspsim·계측기(libcsim `McDataSds`·`McDataMsrp`)·`tests/msrp_sds_client.py`** — 시험 도구의 송신 형태는 보지 않았다. 서버 쪽 검사를 켜면(묶음 2·3·5) 도구도 함께 맞춰야 한다.
- **콘솔** — 그룹 편집의 MCData 칸·메시지 이력 화면.
