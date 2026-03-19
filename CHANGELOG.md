# CIMS 변경 이력

## 2026-03-19

### 1. IMS REGISTER 호환성 수정 (`csp/SipServerRegister.hpp`)

**배경**
실 IMS 단말(LG U+ 4G/LTE)의 SIP REGISTER는 RFC 2617 qop=auth Digest를 사용하고,
200 OK에 P-Associated-URI 헤더를 요구합니다. 기존 코드는 qop 없는 단순 MD5만 지원하여
인증 실패가 발생했습니다.

**변경 내용**
- `AddChallenge()`: WWW-Authenticate에 `qop="auth"` 추가
- `CheckAuthorizationResponse()`: qop=auth 분기 추가 (`HA1:nonce:nc:cnonce:qop:HA2`)
- `CheckAuthorization()`: Credential에서 qop/nc/cnonce 추출 후 검증에 전달
- REGISTER 200 OK: `P-Associated-URI: <sip:user@realm>` 헤더 추가 (IMS 단말 등록 완료 처리 필수)
- `RecvRequestRegister()`: org_id 기반 그룹 자동 참가(`InviteMember`) 로직 제거

---

### 2. 가입자 / 그룹 데이터 파일 재구성 (`csp/User/`, `csp/Group/`)

**배경**
실 운용 환경에 맞게 가입자 번호 체계(E.164 MSISDN)와 인증 ID(IMPI) 분리,
그룹 데이터 구조를 표준에 맞게 재정비합니다.

**변경 내용**

| 항목 | 변경 전 | 변경 후 |
|------|---------|---------|
| 파일명(키) | 정수 ID (`1001.json`) | E.164 MSISDN (`+821357007001.json`) |
| Digest 인증 ID 필드 | `username` | `auth_id` |
| IMS 단말 auth_id | (없음) | `{IMSI}@ims.mnc033.mcc450.3gppnetwork.org` |
| PTT 단말 auth_id | (없음) | `45033{MSISDN숫자}@ptt.mnc033.mcc450.3gppnetwork.org` |
| 그룹 멤버십 | org_id 기반 자동 참가 | `Group/*.json` 명시적 멤버 목록만 사용 |

**생성 파일**
- IMS 단말 10개: `+821357007001.json` ~ `+821357007010.json`
- PTT 단말 10개: `+82571900001.json` ~ `+82571900010.json`
- PTT 그룹: `+82571910001.json` (멤버 +82571900001~5), `+82571910002.json` (멤버 6~10)

**관련 소스 수정**
- `csp/CspUser.cpp` — `_loadUserFromFile()`: `username` 필드를 `auth_id`로 변경

---

### 3. MCPTT INVITE 형식 수정 (`csp/GroupCallService.cpp`, `.h`)

**배경**
3GPP TS 24.379 기반 MCPTT 단말 연동 분석 결과, CSP가 생성하던 INVITE가
OMA POC XML 형식과 멀티파트 순서에서 표준과 불일치하여 단말이 거부하는 문제가 있었습니다.

**변경 내용**
- `BuildGroupInfoXml()`: OMA POC XML → 3GPP `application/vnd.3gpp.mcptt-info+xml` 형식으로 변경
  ```xml
  <mcpttinfo xmlns="urn:3gpp:ns:mcpttInfo:1.0">
    <mcptt-Params>
      <session-type>prearranged</session-type>
      <mcptt-request-uri>tel:{member_id}</mcptt-request-uri>
      <mcptt-calling-user-id>tel:{group_id}</mcptt-calling-user-id>
      <mcptt-calling-group-id>tel:{group_id}</mcptt-calling-group-id>
    </mcptt-Params>
  </mcpttinfo>
  ```
- `WrapMultipartBody()`: 멀티파트 순서 수정 — mcptt-info+xml 먼저, SDP 두 번째; floor control SDP 추가
  ```
  m=application {port} UDP MCPTT
  a=floorid:0 mstrm:audio
  ```
- `InviteMember()`: To 헤더를 그룹 PSI로 변경, `Accept-Contact: *;+g.3gpp.mcptt;require;explicit`, `P-Called-Party-ID`, `Resource-Priority: mcpttp.6` 헤더 추가

---

### 4. 단말 시뮬레이터 보완 (`cspsim/`)

**배경**
실 IMS/PTT 단말 동작(E.164 번호, IMPI 인증, 180 Ringing)과 시뮬레이터를 일치시킵니다.

**변경 파일**

- `cspsim/SimSession.cpp`
  - `EventIncomingCall()` PTT 모드: `RingCall(180)` + 200ms delay → `AcceptCall()` 순서 추가 (기존: 즉시 AcceptCall)
  - `ParseAndLogMcpttInfo()` 추가: 수신 INVITE의 mcptt-info+xml에서 session-type, mcptt-request-uri 등 파싱·출력
  - `RecvRequest()`: INVITE 수신 시 mcptt-info+xml 파싱 후 SipUserAgent에 처리 위임 (`return false`)

- `cspsim/CspsimMain.cpp`
  - `-user` 인자: 정수 외 E.164(+로 시작) MSISDN 지원
  - `-auth_id` 인자 추가: Digest 인증 ID 명시 지정
  - PTT + E.164 자동 유도: `+82571900001` → `4503382571900001@{domain}`
  - 다중 단말 시 순번 증가도 E.164 형식 유지

---

### 5. 프로젝트 가이드 추가 (`CLAUDE.md`)

빌드 명령, 실행 방법, 3-tier 아키텍처, 설정 파일 목록, 외부 의존성을 문서화한
Claude Code 용 프로젝트 안내 파일 추가.

---

## 2026-03-18

### 1. PTT INVITE 멀티파트 본문 (GroupCallService)

**배경**
LTE-R CSC-단말 연동 규격서 v1.5에 따라 PTT 단말로 INVITE 전송 시
SDP 외에 PTT 그룹 정보를 `application/vnd.oma.poc.groups+xml` 형식의 XML로 함께 전달해야 합니다.

**변경 파일**
- `csp/GroupCallService.h` — `BuildGroupInfoXml()`, `WrapMultipartBody()` 헬퍼 선언
- `csp/GroupCallService.cpp`
  - `BuildGroupInfoXml()`: `urn:oma:xml:poc:list-service` / `urn:3gpp:ns:mcpttGroupInfo:1.0` 네임스페이스 기반 XML 생성 (멤버 목록, 우선순위, on-network 속성 포함)
  - `WrapMultipartBody()`: 기존 SDP + 그룹 XML을 `multipart/mixed;boundary=ptt_boundary_1` 로 결합
  - `InviteMember()`: INVITE에 multipart 본문, `P-Access-Network-Info`, `Resource-Priority`, `isfocus` Contact 헤더 추가

---

### 2. CSC SUBSCRIBE/NOTIFY 보완 (SubscriptionManager, SipServer, CspServer)

**배경**
GMS(CSC-2) / CMS(CSC-4) SUBSCRIBE를 올바른 SIP 다이얼로그로 관리하고
NOTIFY를 다이얼로그 헤더에 맞게 전송하도록 보완합니다.

**변경 파일**
- `csp/SubscriptionManager.h` / `csp/SubscriptionManager.cpp`
  - `SubscriptionInfo` 구조체 확장: `strUserId`, `strSubscriberUri`, `strFromTag`, `strToTag`, `strContact`, `strCallId`, `strEventType`, `iNotifySeq` 추가
  - 맵 키를 ResourceUri 에서 **CallId** 로 변경 — 다이얼로그 단위 관리
  - `GetSubscriptionsByUser(userId, eventType)` — 사용자/타입별 구독 목록 조회
  - `IncrementNotifySeq(callId)` — 스레드-세이프 CSeq 증가
  - `RemoveSubscription(callId)` — CallId 기반 삭제
- `csp/SipServer.cpp`
  - SUBSCRIBE 핸들러 재작성: From-tag 올바른 추출(`SelectParamValue(SIP_TAG)`), GMS/CMS 요청 URI 구분, Expires=0 해지 처리, `SipMakeTag` 서버 To-tag 생성, 200 OK 후 InitialNotify 전송
- `csp/CspServer.cpp`
  - `SendSipNotify()` 재작성: `GetSubscriptionsByUser()`로 구독자 조회 → 다이얼로그 헤더 적용 NOTIFY 전송
  - `SendInitialNotify()`: 구독 성공 시 xcap-diff 초기 NOTIFY
  - xcap-diff 본문: GMS → `org.openmobilealliance.groups/users/tel:{user}/tel:{group}`, CMS → user-profile + service-config sel 경로

---

### 3. 단말 시뮬레이터 구현 (cspsim)

**배경**
VoIP 단말과 PTT 단말 동작을 모의하는 시뮬레이터를 구현하여
기능 및 성능 테스트가 가능하도록 합니다.

**변경 파일**
- `cspsim/SimSession.h`
  - `SimStats` 구조체: atomic 카운터 (등록 성공/실패, GMS/CMS 구독, NOTIFY 수신, 통화 성공/종료, 평균 응답시간)
  - `ESimScenario` 열거형: REGISTER / SUBSCRIBE / CALL / GROUP_CALL / FULL
  - `SimSession`: `ISipStackCallBack` 상속으로 SUBSCRIBE/NOTIFY 직접 처리
- `cspsim/SimSession.cpp`
  - `Start()`: SIP 스택 시작, `AddCallBack(this)` 등록, RTP 스레드 생성
  - `SendSubscribe()`: Via/From/To/Contact/Body 완전한 SUBSCRIBE 메시지 구성 및 전송 (GMS/CMS 구분 body)
  - `RecvRequest()`: NOTIFY 수신 → 200 OK 응답 → `HandleNotify()` 처리
  - `RecvResponse()`: SUBSCRIBE 200 응답 시 CallId로 GMS/CMS 구분 → 구독 완료 플래그 설정
  - `HandleNotify()`: xcap-diff 본문에서 `sel` / `new-etag` 추출 및 출력
  - `EventIncomingCall()`: PTT 모드 즉시 자동응답, VoIP 모드 3초 링 후 응답
  - `EventCallStart/End()`: 통화 시간 계측 및 통계 집계
- `cspsim/CspsimMain.cpp`
  - CLI 인자: `-server_ip`, `-server_port`, `-local_ip`, `-local_port`, `-count`, `-user`, `-domain`, `-password`, `-mode voip|ptt`, `-group`, `-scenario`, `-call_duration`, `-interval`, `-verbose`
  - `RunScenario()`: 별도 스레드에서 등록 대기 → SUBSCRIBE → 페어 통화 또는 그룹통화 자동 실행
  - `PrintStats()`: 전체 세션 통계 집계 및 출력
  - 대화형 명령: `s`(통계), `c`(통화), `g`(그룹통화), `t/r`(PTT 발언권), `sub`(구독), `e`(종료), `q`(종료)

---

### 4. 버그 수정

| 위치 | 증상 | 원인 | 수정 |
|------|------|------|------|
| `cspsim/SimSession.cpp` | 세션 2개 이상 시 세그폴트 | `Stop()`에서 `Final()` 호출 → OpenSSL 전역 상태 이중 해제 | `Final()` 호출 제거 |
| `cspsim/CspsimMain.cpp` | 통화 즉시 종료 | `sleep(iCallDuration)` 이 SIP 스택 시그널에 의해 interrupt | `for usleep(100ms)` 루프로 교체 |
| `cspsim/SimSession.cpp` | CMS SUBSCRIBED 카운트 0 | GMS/CMS Call-ID가 `time(NULL)` 기반으로 동일 생성 | Call-ID에 PSI 이름 포함 (`sub_gms_psi_...` / `sub_cms_psi_...`) |
| `csp/SipServerUserAgent.hpp` | 그룹통화 발신자에게 응답 없음 | `ProcessGroupCall` 성공 후 발신자에게 200 OK 미전송 | `AcceptCall(pszCallId, sharedRtp)` 추가 |
| `csp/GroupCallService.cpp` | 그룹통화 404 응답 | CMP Connected 전에 AddGroup 호출 → 포트 0 캐시 후 재사용 | 포트 > 0 조건 확인 후 재시도 |
| `cmp/CmpServer.cpp` | AddGroup 항상 포트 0 반환 | 기존 그룹 조회(`else` 브랜치)에서 `sharedSession` 포트 미반환 | `group->getSharedSession()->getLocalPort()` 추가 |
| `cmp/McpttGroup.h` | `getSharedSession()` 없음 | — | `getSharedSession() const` 인라인 접근자 추가 |

---

### 5. 테스트 환경 구성 (`test_run/`)

```
test_run/
├── csp.json          # CSP 설정 (IP: 192.168.199.129:5060, Realm: csp)
├── cmp.json          # CMP 설정 (RTP pool 20, Port: 9000)
├── User/
│   ├── 1001.json     # 사용자: 1001, passwd: 1234
│   ├── 1002.json
│   ├── 1003.json
│   └── 1004.json
├── Group/
│   └── 1000.json     # PTT 그룹: Sales Team (멤버: 1001~1004)
└── route/            # SIP 서버 라우팅 (기존)
```

### 6. 테스트 실행 방법

```bash
cd test_run

# 서버 시작
./bin/cmp cmp.json > log/cmp.log 2>&1 &
./bin/csp csp.json -n > /dev/null 2>&1 &

# VoIP 통화 테스트 (2개 단말, 페어 통화)
cspsim -server_ip 192.168.199.129 -count 2 -user 1001 \
       -domain csp -password 1234 -mode voip \
       -scenario call -call_duration 5

# PTT 그룹통화 테스트 (4개 단말)
cspsim -server_ip 192.168.199.129 -count 4 -user 1001 \
       -domain csp -password 1234 -mode ptt \
       -group 1000 -scenario group-call -call_duration 10
```

### 7. 테스트 결과

**VoIP 2단말 통화**
```
Registered   : 2 / 2  (Avg 1ms)
Call OK/End  : 1 / 1
Avg Setup    : 3002ms  (3초 링 후 응답)
```

**PTT 4단말 그룹통화**
```
Registered   : 4 / 4  (Avg 1ms)
GMS/CMS Sub  : 4 / 4
Group Call   : EventCallStart OK, RTP(127.0.0.1:50076)
Avg Setup    : 1ms
```
