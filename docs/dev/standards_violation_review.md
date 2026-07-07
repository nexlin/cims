# 통신 표준 규격 위반 검토 보고서

**작성일:** 2026-06-22  
**최종 수정:** 2026-06-29 (F-03, F-12 추가 / 런타임 검증 결과 추가)  
**검토 기준:** RFC 3261, RFC 3903, RFC 4575, RFC 2046, RFC 3856, 3GPP TS 24.379, TS 24.380  
**검증 아티팩트:** [standards_violation_review.html](standards_violation_review.html)

---

## 런타임 검증 결과 (2026-06-29)

**테스트 환경:** `make dist` Jun 29 재빌드 → `ptt-test.sh restart` → `cspsim` 4세션 prearranged/broadcast/chat 시나리오  
**로그:** `build/dist/csp/log/csp_20260629_1.log` (L47xxx 이후 구간)  
**CMP 로그:** `build/dist/cmp/log/cmp.log`

| ID | 검증 방법 | 결과 | 근거 |
|---|---|---|---|
| F-02 | CMP 로그 | ✅ 확인 | `addMember floor=45140`, `sendFloor → 45140` (UE별 floorPort로 전송) |
| F-03 | CSP 로그 L47747 | ✅ 확인 | `Contact: <sip:1004@...>;expires=600` |
| F-04 | CSP 로그 L48004 | ✅ 확인 | `SIP-ETag: aff-19f11c709f4ef29f4e5` (세션별 고유 hex) |
| F-05 | CSP 로그 L47985 | ✅ 확인 | `Event: mcptt` in PUBLISH |
| F-06 | 런타임 확인 | ✅ 확인 | `tests/f06_unregistered_subscribe.py` PASS — 미등록 SUBSCRIBE → 401+WWW-Authenticate 수신 |
| F-07 | 런타임 확인 | ✅ 확인 | `tests/f07_stale_nonce.py` PASS — 소비 nonce 재사용 → 401 stale=true 수신. 추가 수정: `RecvRequestRegister()` L250 누락 수정 |
| F-08 | CSP 로그 | ✅ 확인 | `Resource-Priority: mcpttp.6` 6건 (INVITE당 1개, 중복 없음) |
| F-09 | CSP 로그 L49362 | ✅ 확인 | `state="full" version="1"` (첫 NOTIFY) |
| F-10 | CSP 로그 L49364 | ✅ 확인 | `entity="sip:g001@csp"`, `entity="sip:1001@csp"` (sip: URI) |
| F-11 | CMP 소스 확인 | ✅ 확인 | `PMcpttGroup.cpp:61` `version_subtype = 0x80 \| (opcode & 0x1F)` |
| F-12 | CSP 로그 L47748 | ✅ 확인 | `Expires: 600` (요청값 그대로 반환) |
| F-13 | 런타임 확인 | ✅ 확인 | `tests/f13_if_match.py` PASS — 잘못된 ETag → 412, 올바른 ETag → 200 OK 확인 |
| F-14 | CSP 로그 L57239 | ✅ 확인 | `Subscription-State: terminated;reason=timeout` 16건 (4세션×GMS+CMS) |
| F-15 | CSP 로그 L48948 | ✅ 확인 | `a=mcptt-floor-request-uri:sip:g001@csp` in INVITE SDP |
| F-16 | CSP 로그 L48872 | ✅ 확인 | `boundary=mcptt_6a41fdb5ce018d8c` (INVITE별 다른 suffix) |

> **F-13 런타임 재현 방법:** SIPp 등으로 첫 PUBLISH 200 OK의 `SIP-ETag` 값을 변조한 `If-Match`로 두 번째 PUBLISH 전송 → 412 응답 확인

---

## Critical — 즉시 조치 필요

### F-02: Floor broadcast가 잘못된 포트로 전송
- **파일:** `cmp/PMcpttGroup.cpp:737~743`
- **내용:** `broadcastFloorStatus`가 `sendAudioRtcpToAll` 호출 → `peer.port + 1` (audio RTCP 포트) 사용. 유니캐스트(`sendToMember`)는 `peer.floorPort`를 올바르게 쓰는데, 브로드캐스트(TAKEN/IDLE)만 잘못된 포트로 나감.
- **위반:** TS 24.379 §6.2.1.7 / TS 24.380 §8.2
- **영향:** 표준 단말은 `m=application` 포트 외 floor 패킷을 무시 → TAKEN/IDLE 수신 불가 → floor 상태 동기화 실패. cspsim 내부 테스트에서는 묻힘.
- **상태:** ✅ 수정 완료 (2026-06-23) — `sendAudioRtcpToAll` → `sendFloorToAll`, `peer.port+1` → `peer.floorPort`

---

## Major — 상호운용 저하

### F-03: REGISTER 200 OK Contact에 expires 파라미터 없음
- **파일:** `csp/CscfModule.cpp`
- **내용:** REGISTER 200 OK의 Contact 헤더에 `expires` 파라미터 미포함. 일부 단말은 별도 Expires 헤더를 무시하고 Contact의 expires 파라미터만 신뢰함.
- **위반:** RFC 3261 §10.3
- **영향:** 단말이 재등록 타이머를 잘못 설정하거나 미설정
- **상태:** ✅ 수정 완료 (2026-06-26)
  - `csp/CscfModule.cpp` — `clsContact.InsertParam("expires", szExpires)` 추가 후 ContactList에 삽입
  - 응답 형식: `Contact: <sip:1001@...>;expires=600`

---

### F-04: SIP-ETag 초 단위 충돌·예측 가능
- **파일:** `csp/CscfModule.cpp:475`
- **내용:** `SIP-ETag = "aff-{groupId}-{unix_sec}"` — 1초 내 복수 PUBLISH 시 동일 ETag 발급, 값 예측 가능
- **위반:** RFC 3903 §4
- **영향:** PUBLISH refresh 충돌, SIP-If-Match 검증 불신뢰
- **상태:** ✅ 수정 완료 (2026-06-29)
  - `csp/CscfModule.cpp` — `clock_gettime(CLOCK_REALTIME)` 밀리초 + `tv_nsec ^ (uintptr_t)pclsMessage` 랜덤 비트 조합으로 변경
  - 형식: `"aff-{ms_hex}{rand_hex}"` → 동일 초 내 중복 발급 불가, 예측 불가

---

### F-05: PUBLISH Event 헤더 미검증
- **파일:** `csp/CscfModule.cpp`, `cspsim/SimSession.cpp`
- **내용:** PUBLISH 수신 시 Event 헤더값 검증 없음. 3GPP TS 24.379는 `Event: mcptt` 요구. cspsim은 `poc-settings`(구형 PoC 값)를 보내고 있었음.
- **위반:** TS 24.379 §9
- **영향:** 잘못된 Event 헤더의 PUBLISH도 affiliation으로 처리됨
- **상태:** ✅ 수정 완료 (2026-06-26)
  - `csp/CscfModule.cpp` — `RecvRequestPublish()`에 Event 헤더 검증 추가, `Event != "mcptt"` 시 489 Bad Event 응답
  - `cspsim/SimSession.cpp` — `AddHeader("Event", "poc-settings")` → `AddHeader("Event", "mcptt")`

---

### F-06: 미등록 사용자 SUBSCRIBE → 403 응답
- **파일:** `csp/CscfModule.cpp:331`
- **내용:** 미등록 사용자의 SUBSCRIBE 요청에 403 응답. RFC 표준은 401 + Digest 챌린지 요구.
- **위반:** RFC 3261 §22
- **영향:** 표준 단말은 403을 영구 실패로 처리, 재시도 안 함
- **상태:** ✅ 수정 완료 + 런타임 검증 PASS (2026-06-29)
  - `csp/CscfModule.cpp` — `SendResponse(pclsMessage, 403)` → `SendUnAuthorizedResponse(pclsMessage)`
  - **런타임 검증:** `python3 tests/f06_unregistered_subscribe.py` — REGISTER 없이 SUBSCRIBE 전송 → 401+WWW-Authenticate 수신 확인 (수정 전이었다면 403)

---

### F-07: nonce 만료 재도전에 stale=true 없음
- **파일:** `csp/CscfModule.cpp`, `csp/CscfModule.h`
- **내용:** nonce 만료 시 재발급하는 401 응답에 `stale=true` 파라미터 없음
- **위반:** RFC 3261 §22.3
- **영향:** 단말이 nonce 만료를 비밀번호 오류로 오해 → 사용자에게 재인증 팝업 표시
- **상태:** ✅ 수정 완료 + 런타임 검증 PASS (2026-06-29)
  - `csp/CscfModule.h` — `AddChallenge()` / `SendUnAuthorizedResponse()`에 `bool bStale = false` 파라미터 추가
  - `csp/CscfModule.cpp` — `AddChallenge()`: `bStale`이면 `clsChallenge.m_strStale = "true"` 설정
  - `csp/CscfModule.cpp` — `CheckAuthrization()` nonce 만료(`E_AUTH_NONCE_NOT_FOUND`) 경로에서 `SendUnAuthorizedResponse(pclsMessage, "", true)` 호출
  - `csp/CscfModule.cpp:250` — `RecvRequestRegister()` 내 `E_AUTH_NONCE_NOT_FOUND` 경로 누락 수정: `SendUnAuthorizedResponse(pclsMessage)` → `SendUnAuthorizedResponse(pclsMessage, "", true)` (REGISTER 는 `CheckAuthrization()`을 거치지 않아 별도 경로가 필요)
  - **런타임 검증:** `python3 tests/f07_stale_nonce.py` — 3단계 테스트 (401 nonce 수신 → 200 OK nonce 소비 → 소비 nonce 재사용 → 401 stale=true) 모두 PASS 확인

---

### F-08: Resource-Priority 헤더 중복 전송
- **파일:** `csp/GroupCallService.cpp:655~665`
- **내용:** `Resource-Priority: mcpttp.4` + `mcpttp.6` 두 값이 동시에 전송됨
- **위반:** RFC 4412 §3 (단일 namespace당 하나의 값)
- **영향:** 표준 단말이 우선순위를 혼동할 수 있음
- **상태:** ✅ 수정 완료 (2026-06-29)
  - `csp/GroupCallService.cpp` — `mcpttp.6`을 `else` 분기로 이동: emergency=`.4` / imminent=`.2` / normal=`.6` 중 하나만 전송

---

### F-09: conference NOTIFY 초기 state="full" 없음
- **파일:** `csp/GroupCallService.cpp:1157`
- **내용:** conference NOTIFY가 항상 `state="partial"`. 첫 NOTIFY는 `state="full"`이어야 함.
- **위반:** RFC 4575 §4.6
- **영향:** 단말이 참가자 목록을 초기화하지 못하고 누적만 함
- **상태:** ✅ 수정 완료 (2026-06-29)
  - `csp/GroupCallService.cpp` `SendConferenceNotify()` — `iVersion == 1`일 때 `m_mapCallSession` 순회로 전체 멤버를 열거해 `state="full"` body 생성, 이후(`version ≥ 2`)는 기존 `state="partial"` 유지

---

### F-10: conference NOTIFY user entity에 tel: URI 사용
- **파일:** `csp/GroupCallService.cpp:1159`
- **내용:** `<user entity="tel:+82...">` — RFC 4575는 SIP URI 요구
- **위반:** RFC 4575 §5.3
- **영향:** 표준 단말이 참가자 식별 실패 가능
- **상태:** ✅ 수정 완료 (2026-06-29)
  - `csp/GroupCallService.cpp` `SendConferenceNotify()` — `tel:{user}` → `sip:{user}@{mcpttDomain}` 형식으로 변경 (full/partial 양쪽 모두)

---

### F-11: RTCP APP opcode를 subtype 비트가 아닌 app-data에 배치
- **파일:** `cmp/PMcpttGroup.cpp`, `cspsim/RtpThread.cpp`, `cspsim/RtpThreadRecv.hpp`
- **내용:** RTCP APP 패킷의 opcode를 subtype(5비트) 필드가 아닌 app-data 영역에 배치
- **위반:** TS 24.380 §8.2 (RTCP APP subtype 필드 사용 명시)
- **영향:** 표준 단말과의 floor control 완전 호환 불가
- **상태:** ✅ 수정 완료 (2026-06-26)
  - `cmp/PMcpttGroup.cpp` `BuildFloorPacket()` — `version_subtype = 0x80 | (opcode & 0x1F)`, `pkt->opcode = 0`
  - `cmp/PMcpttGroup.cpp` `onFloorPacket()` / `onRtcpPacket()` — `opcode = pkt->version_subtype & 0x1F`
  - `cspsim/RtpThread.cpp` `SendFloorControl()` — `buf[0] = 0x80 | (iOpCode & 0x1F)`, app-data[0] = 0
  - `cspsim/RtpThreadRecv.hpp` `RtpThreadFloorRecv` — `opcode = buf[0] & 0x1F`

---

## Minor — 규격 불일치

### F-12: REGISTER Expires 하드코딩
- **파일:** `csp/CscfModule.cpp`
- **내용:** Expires 헤더 값 3600 하드코딩, 단말 요청값 무시
- **위반:** RFC 3261 §10.3 (서버는 단말 요청값을 협상해야 함)
- **영향:** 단말이 원하는 재등록 주기가 무시됨
- **상태:** ✅ 수정 완료 (2026-06-26)
  - `csp/CscfModule.cpp` — `pclsMessage->GetExpires()` 로 단말 요청값 읽어 `iGrantedExpires` 결정 (요청값 ≤ 3600이면 그대로, 초과 시 3600으로 조정)
  - F-03 수정과 동일한 변수(`iGrantedExpires`) 공유 → Expires 헤더값과 Contact expires 파라미터값 항상 일치

---

### F-13: SIP-If-Match 미검증
- **파일:** `csp/CscfModule.cpp`
- **내용:** PUBLISH refresh/modify 시 SIP-If-Match 헤더 미검증 → 항상 initial PUBLISH로 처리
- **위반:** RFC 3903 §4
- **상태:** ✅ 수정 완료 + 런타임 검증 PASS (2026-06-29)
  - `csp/CscfModule.cpp` 상단 — `s_mapEtag` (key=`userId:groupId`), `s_etagMutex` 추가
  - `RecvRequestPublish()` — SIP-If-Match 있으면 저장 ETag와 비교, 불일치 시 412 반환
  - affiliate 성공 시 ETag 저장·갱신, de-affiliate 시 ETag 삭제
  - **런타임 검증:** `python3 tests/f13_if_match.py` — 잘못된 ETag → 412, 올바른 ETag → 200 OK + 새 ETag 발급 확인

---

### F-14: SUBSCRIBE 해제 시 종료 NOTIFY 미발송
- **내용:** SUBSCRIBE Expires=0(해제) 수신 시 최종 NOTIFY 미발송
- **위반:** RFC 3265 §3.1.4 (구독 종료 시 final NOTIFY 필수)
- **상태:** ✅ 수정 완료 (2026-06-29)
  - `csp/SubscriptionManager.h/.cpp` — `GetSubscriptionByCallId()` 추가 (Call-ID로 단건 조회)
  - `csp/CspServer.cpp` — `SendTerminatedNotify()` 추가 (`Subscription-State: terminated;reason=timeout`, body 없음)
  - `csp/CscfModule.cpp` — `iExpires == 0` 블록 수정: 200 OK → `GetSubscriptionByCallId` → `SendTerminatedNotify` → `RemoveSubscription` 순서로 교체

---

### F-15: SDP m=application에 floor-request-uri 속성 없음
- **파일:** `csp/GroupCallService.cpp`, `csp/GroupCallService.h`
- **내용:** `m=application` SDP에 `a=mcptt-floor-request-uri` 속성 누락
- **위반:** TS 24.379 §C.3
- **영향:** 단말이 floor REQUEST 대상 URI를 알 수 없어 floor REQUEST 실패 가능
- **상태:** ✅ 수정 완료 (2026-06-26)
  - `csp/GroupCallService.h` — `WrapMultipartBody()`에 `const std::string &strGroupUri = ""` 파라미터 추가
  - `csp/GroupCallService.cpp` `WrapMultipartBody()` — `strGroupUri` 비어있지 않으면 SDP에 `a=mcptt-floor-request-uri:{strGroupUri}` 삽입
  - `csp/GroupCallService.cpp` 호출부 — `strGroupUri = "sip:{groupId}@{mcpttDomain}"` 구성 후 전달

---

### F-16: multipart boundary 값이 본문에 등장
- **내용:** multipart body의 boundary 문자열 `"mcptt"`가 body 내용에 등장 가능
- **위반:** RFC 2046 §5.1.1 (boundary는 본문 내 미등장 보장 필요)
- **상태:** ✅ 수정 완료 (2026-06-29)
  - `csp/GroupCallService.cpp` `WrapMultipartBody()` — 고정값 `"mcptt"` → `clock_gettime()` 밀리초 + 포인터 기반 랜덤 hex 조합으로 변경
  - 형식: `"mcptt_{sec_hex8}{rnd_hex8}"` (예: `"mcptt_6ad3f1b2_a9e4c307"`) — body 내 등장 불가

---

## cspsim 로그아웃 플로우 추가 (2026-06-29)

### 배경

실제 단말은 종료 시 아래 순서로 로그아웃을 수행한다:
1. PUBLISH Expires=0 (de-affiliate — 그룹 이탈 알림)
2. SUBSCRIBE Expires=0 × 2 (gms/cms 구독 해제)
3. REGISTER Expires=0 (등록 해제)

기존 `SimSession::Stop()`은 이 과정 없이 BYE + 프로세스 종료만 했다 — CSP 측에서 구독이 자연 만료(3600초)될 때까지 단말이 "여전히 접속 중"으로 인식.

### 수정 내용

**`cspsim/SimSession.h`**
- `SendUnsubscribe()` 선언 추가 — 기존 다이얼로그(Call-ID/From-tag) 재사용 SUBSCRIBE Expires=0
- `Logout()` 선언 추가 — 로그아웃 전체 시퀀스 캡슐화

**`cspsim/SimSession.cpp`**
- `SendUnsubscribe()` 구현 — 기존 `m_strGmsCallId`/`m_strCmsCallId`/From-tag 재사용, `++iSeq`, `Expires: 0`, body 없음
- `Logout()` 구현:
  1. PTT 모드이면 `AffiliateGroup(true)` (de-affiliate PUBLISH, 이미 구현됨)
  2. gms 구독 중이면 `SendUnsubscribe("gms_psi", ...)` → `m_bGmsSubscribed = false`
  3. cms 구독 중이면 `SendUnsubscribe("cms_psi", ...)` → `m_bCmsSubscribed = false`
  4. `m_bNoRegister` 아니고 등록 중이면 REGISTER Expires=0 직접 구성·전송 → `m_bRegistered = false`
     (psip `DeRegister()`는 private이므로 SIP 스택 직접 사용)
- `Stop()` 수정 — BYE 후 `Logout()` 호출 + 300ms 대기(UDP 전송 완료 보장) 후 스택 종료

---

## 수정 우선순위 요약

| 순서 | ID | 파일 | 한줄 요약 | 상태 |
|---|---|---|---|---|
| REGISTER | F-07 | csp/CscfModule.cpp | stale=true 누락 → 단말 오인증 팝업 | ✅ |
| REGISTER | F-03 | csp/CscfModule.cpp | Contact expires 파라미터 누락 → 재등록 타이머 오설정 | ✅ |
| REGISTER | F-12 | csp/CscfModule.cpp | Expires 하드코딩 → 단말 요청값 무시 | ✅ |
| SUBSCRIBE | F-06 | csp/CscfModule.cpp | 미등록 사용자 SUBSCRIBE 403 → 401로 변경 | ✅ |
| SUBSCRIBE | F-14 | csp/CscfModule.cpp | 구독 해제 시 final NOTIFY 미발송 | ✅ |
| PUBLISH | F-05 | csp/CscfModule.cpp + cspsim | Event 헤더 미검증 / poc-settings 오기재 | ✅ |
| PUBLISH | F-04 | csp/CscfModule.cpp | SIP-ETag 예측 가능 → millisec+random으로 변경 | ✅ |
| PUBLISH | F-13 | csp/CscfModule.cpp | SIP-If-Match 미검증 | ✅ |
| INVITE | F-15 | csp/GroupCallService.cpp | SDP floor-request-uri 누락 | ✅ |
| INVITE | F-08 | csp/GroupCallService.cpp | Resource-Priority 중복 전송 | ✅ |
| INVITE | F-16 | csp/GroupCallService.cpp | multipart boundary 충돌 가능 | ✅ |
| 통화 중 | F-02 | cmp/PMcpttGroup.cpp | Floor broadcast 잘못된 포트 → floor 동기화 실패 | ✅ |
| 통화 중 | F-11 | cmp/PMcpttGroup.cpp + cspsim | RTCP APP opcode 위치 오류 → floor control 호환 불가 | ✅ |
| 통화 중 | F-09 | csp/GroupCallService.cpp | conference NOTIFY state="full" 누락 → 참가자 목록 오류 | ✅ |
| 통화 중 | F-10 | csp/GroupCallService.cpp | conference NOTIFY entity tel: URI → SIP URI 변경 | ✅ |
