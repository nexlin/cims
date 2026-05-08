# 10. CSP (Call Service Platform) 모듈 상세 설계

## 1. 개요

CSP는 CIMS 시스템의 SIP 시그널링 서버로, IMS 기반 역할(CSCF, TAS, PTT-AS, IBCF)을 단일 프로세스에서 수행한다.

### 1.1 핵심 기능

| 기능 | 설명 |
|------|------|
| SIP 등록/인증 | Digest MD5 인증, 가입자 등록 관리 |
| VoIP 1:1 통화 | B2BUA 기반 호 처리, CMP RTP relay 연동 |
| PTT 그룹콜 | 다자 SIP INVITE, CMP 그룹 RTP/Floor 연동 |
| 부가서비스 | DND, 착신전환, 착신거부, 콜픽업 |
| IP-PBX 트렁크 | 외부 SIP 서버 라우팅 (IBCF) |
| 가입자/그룹 관리 | DB primary, JSON fallback |
| 서비스 로깅 | Session-ID 기반 통합 이력, SIP 메시지 로깅 |
| CSC 연동 | UDP JSON으로 설정 변경 실시간 수신 |

### 1.2 프로세스 구성

```
bin/csp <config.json> [-n]
  -n : foreground 실행
  config.json : 설정 파일 경로 (기본: csp.json)
```

---

## 2. 아키텍처

### 2.1 모듈 구조

```
SIP Stack (psip)
  │
  ├─ ISipStackCallBack ──────────────────────────────┐
  │                                                   │
  ▼                                                   ▼
CModuleDispatcher ◄── ISipUserAgentCallBack ── CSipUserAgent (B2BUA)
  │
  ├── CCscfModule    ── REGISTER, SUBSCRIBE, Digest 인증
  ├── CTasModule     ── VoIP B2BUA: DND, 착신전환, 착신거부, 콜픽업
  ├── CPttAsModule   ── PTT 그룹콜 (CGroupCallService 래핑)
  └── CIbcfModule    ── IP-PBX 트렁크 라우팅
```

### 2.2 콜백 등록 순서

SIP 스택에 `[CModuleDispatcher, CSipUserAgent]` 순서로 콜백 등록:

1. **RecvRequest()** → ModuleDispatcher가 먼저 수신
   - REGISTER/SUBSCRIBE → CSCF 직접 처리
   - INVITE → 라우팅 판단 후 B2BUA(CSipUserAgent)로 전달
2. **B2BUA 이벤트** → ModuleDispatcher의 ISipUserAgentCallBack으로 전달
   - EventIncomingCall, EventCallRing, EventCallStart, EventCallEnd 등

### 2.3 역할 기반 활성화

```json
// csp.json
{
  "Setup": {
    "Roles": {
      "CSCF": true,    // 등록/인증/구독
      "TAS": true,     // VoIP 부가서비스
      "PTT_AS": true,  // PTT 그룹콜
      "IBCF": false    // IP-PBX 트렁크
    }
  }
}
```

`Roles` 섹션 미지정 시 전체 역할 활성화 (하위 호환).

---

## 3. 클래스 상세

### 3.1 CModuleDispatcher

**파일:** `ModuleDispatcher.h/.cpp`

중앙 디스패처. 모든 SIP 이벤트를 수신하여 적절한 모듈로 라우팅한다.

**인터페이스 구현:**
- `ISipStackCallBack` — SIP 요청/응답 수신
- `ISipUserAgentCallBack` — B2BUA 호 이벤트 수신
- `ISipStackSecurityCallBack` — IP/UA 접근 제어

**핵심 메서드:**

| 메서드 | 역할 |
|--------|------|
| `RecvRequest(msg)` | SIP 요청 라우팅 (REGISTER→CSCF, INVITE→라우팅 판단) |
| `RecvResponse(msg)` | SIP 응답 로깅 |
| `EventIncomingCall(callId, from, to, rtp)` | B2BUA 착신 이벤트 → 발신 leg 생성 |
| `EventCallRing(callId, statusCode)` | 180/183 브릿징 |
| `EventCallStart(callId, rtp)` | 200 OK 브릿징, ReINVITE 전송 |
| `EventCallEnd(callId, reason)` | 양 leg 종료, CDR 저장, 로그 기록 |
| `SetCallOwner(callId, module)` | 호 소유권 추적 |
| `GetCallOwner(callId)` | 호 담당 모듈 조회 |

**INVITE 라우팅 로직:**

```
RecvRequest(INVITE)
  ├─ PTT 그룹 대상? ──────────→ PTT-AS (SetCallOwner → CPttAsModule)
  ├─ 트렁크 prefix 매칭? ──→ IBCF (SetCallOwner → CIbcfModule)
  ├─ DND 활성화? ──────────→ 603 Decline 응답
  ├─ 착신거부 목록? ────────→ 603 Decline 응답
  ├─ 착신전환 설정? ────────→ 302 Moved Temporarily
  └─ 기본 ─────────────────→ TAS B2BUA (SetCallOwner → CTasModule)
```

**호 소유권 추적:**

```cpp
std::map<std::string, IModule*> m_mapCallOwner;  // CallId → 담당 모듈
```

### 3.2 CCscfModule

**파일:** `CscfModule.h/.cpp`

SIP REGISTER/SUBSCRIBE 처리 및 Digest MD5 인증.

**인증 흐름:**

```
REGISTER 수신
  │
  ├─ Authorization 헤더 없음
  │   └─ 401 Unauthorized + WWW-Authenticate (nonce 생성, NonceMap 저장)
  │
  └─ Authorization 헤더 있음
      ├─ NonceMap에서 nonce 검증
      ├─ CspUserMap에서 사용자 조회 (AuthId 매칭)
      ├─ MD5(A1:nonce:A2) 계산 및 비교
      │
      ├─ 성공 → UserMap에 등록, DB register_time 갱신, 200 OK
      └─ 실패 → 401 Unauthorized (재도전)
```

**Static 헬퍼:**

| 메서드 | 역할 |
|--------|------|
| `AddChallenge(response, realm, nonce)` | WWW-Authenticate 헤더 생성 |
| `SendUnAuthorizedResponse(request)` | 401 응답 전송 |
| `CheckAuthorizationResponse(auth, user)` | MD5 인증 검증 |

**SUBSCRIBE 처리:**

```
SUBSCRIBE (Event: gms 또는 cms)
  │
  ├─ 인증 검증 (Digest)
  ├─ SubscriptionInfo 생성 → SubscriptionManager 저장
  ├─ 200 OK 응답
  └─ 즉시 NOTIFY 전송 (xcap-diff XML 본문)
```

### 3.3 CTasModule

**파일:** `TasModule.h/.cpp`

VoIP 1:1 통화의 B2BUA 처리 및 부가서비스.

**부가서비스:**

| 서비스 | 트리거 | 동작 |
|--------|--------|------|
| DND (착신거부) | `CspUser::m_bDnd == true` | 603 Decline |
| 개별 착신거부 | `CspUser::m_vecReject`에 발신자 포함 | 603 Decline |
| 착신전환 | `CspUser::m_strForward` 설정됨 | 302 Moved Temporarily |
| 콜픽업 | 특수 pickup URI 호출 | 그룹 내 활성 호 연결 |

**B2BUA 호 생성:**

```
EventIncomingCall(incomingCallId, from, to, rtp)
  │
  ├─ Session-ID 생성 (CCallDir)
  ├─ 양 leg CallId ↔ Session-ID 매핑
  ├─ CMP add 명령 → RTP relay 포트 할당
  ├─ CSipUserAgent::CreateCall() → 발신 leg CallId 생성
  ├─ gclsCallMap.Insert(incomingCallId, outgoingCallId, rtp)
  ├─ SetCallOwner(incomingCallId, TAS)
  ├─ SetCallOwner(outgoingCallId, TAS)
  └─ CSipUserAgent::StartCall(outgoingCallId, invite)
```

### 3.4 CPttAsModule / CGroupCallService

**파일:** `PttAsModule.h/.cpp`, `GroupCallService.h/.cpp`

PTT 그룹콜 오케스트레이션.

**그룹 RTP 정보 (GroupRtpInfo):**

CMP에서 할당받은 그룹 미디어 리소스를 추적하는 내부 구조체.

```cpp
struct GroupRtpInfo {
    int iPort;              // CMP 할당 Audio RTP 포트 (PPttTrans)
    int iFloorPort;         // CMP 할당 Floor Control 포트 (PPttTrans)
    int iVideoPort;         // CMP 할당 Video 포트
    std::string strIp;      // CMP RTP IP
    size_t nMemberHash;     // 멤버 구성 해시 (변경 감지용)
    std::string strSessionCallId;  // 세션 발신 Call-ID
    std::string strCallerId;       // 발신자 ID
    bool bVideoEnabled;     // 영상 활성화 여부
    int iConfVersion;       // RFC 4575 conference-info version
};
```

**Floor 포트 전달 흐름:**

```
CMP AddGroup 응답 → {port, floor_port, video_port}
    │
    └→ GroupRtpInfo.iFloorPort에 저장
    │
    └→ WrapMultipartBody() → INVITE SDP m=application {floor_port}
    │
    └→ UE 200 OK 수신 → OnCallStarted()
        │
        └→ JoinGroup(user_floor_port = UE의 m=application 포트)
            │
            └→ CMP McpttGroup::addMember(floorPort=UE floor 포트)
```

**그룹 단위 통일 sesid:**

PTT 그룹 세션에 대한 모든 모듈간 메시지(`ADD_PTT_GROUP`, `JOIN_PTT_GROUP`, `LEAVE_PTT_GROUP`, `REMOVE_PTT_GROUP`)와 SIP INVITE leg 들이 동일 sesid 를 공유하도록 per-group 매핑 유지:

```cpp
class CGroupCallService {
    std::map<std::string, std::string> m_mapGroupSesId;  // group_id → sesid
    std::string GetOrIssueGroupSesId(const std::string& groupId);
    void        RemoveGroupSesId(const std::string& groupId);
};
```

- sesid 형식: `{group_id}::csp::{us_ts}::{counter}` (caller 자리에 group_id)
- 그룹 해체(`REMOVE_PTT_GROUP` 완료) 시 매핑 제거
- Console UI 의 PTT Flow 보기는 이 sesid 로 CSP/CMP 양쪽 로그를 하나의 세션으로 병합

**MCPTT 도메인 per-dialog override:**

PTT INVITE 의 Request-URI / From / To / P-Asserted-Identity 도메인이 Digest realm(AuthRealm) 이나 VoLTE 도메인과 섞이지 않도록 psip `CSipDialog::m_strOverrideDomain` 를 설정. 두 가지 경로:

- `CSipUserAgent::CreateCall(... , overrideDomain)` — InviteMember 시 mcptt 도메인 지정
- `CSipUserAgent::SetCallDomain(callId, domain)` — AcceptCall 이후 leg 에 사후 적용

자세한 설정은 [../features/flow_logging.md](./../features/flow_logging.md) § 7 참고.

**그룹 데이터 (CspPttGroup):**

```cpp
class CspPttGroup {
    std::string _id;           // 그룹 ID
    std::string _name;         // 표시 이름
    std::vector<CspPttUser> _pusers;  // 멤버 목록
    bool _videoEnabled;        // H.264 지원
    int _priority;             // 기본 우선순위
    bool _encryption;          // SRTP 활성화
    bool _emergencyCall;       // 긴급호 허용
};
```

**그룹콜 흐름 (ProcessGroupCall):**

```
INVITE to group@domain
  │
  ├─ 그룹 존재/세션 시간 유효성 확인
  ├─ CMP addGroup → 공유 RTP 포트 + Floor 포트 할당
  │   (record_dir, log_dir 전달)
  ├─ 발신자에게 200 OK (공유 RTP 주소)
  ├─ 매핑: callerId → groupId (m_mapUserCall)
  │
  └─ 각 그룹 멤버에 대해:
      ├─ InviteMember() → Multipart INVITE
      │   ├─ Content-Type: multipart/mixed
      │   ├─ Part 1: SDP (공유 RTP 주소)
      │   └─ Part 2: application/vnd.oma.poc.groups+xml (멤버 목록)
      ├─ 멤버 200 OK 수신 → CMP joinGroup
      └─ 매핑: memberCallId → {groupId, memberId, sessionId}
```

**멤버 생명주기:**

| 이벤트 | 처리 |
|--------|------|
| 멤버 200 OK | CMP joinGroup, DB join_time 기록 |
| 멤버 BYE | CMP leaveGroup, DB leave_time 기록 |
| 멤버 무응답 | 타임아웃 후 스킵 |
| 그룹 설정 변경 | CheckGroupIntegrity() → 멤버 추가/제거 |
| CMP 재연결 | OnCmpStatusChanged() → 그룹 재생성 |

### 3.5 CIbcfModule

**파일:** `IbcfModule.h/.cpp`

외부 IP-PBX 트렁크 라우팅.

**라우팅 판단:**

```cpp
CspSipServer clsSipServer;
if (gclsSipServerMap.SelectRoutePrefix(pszTo, clsSipServer, strTo)) {
    // prefix 매칭 → 해당 트렁크로 B2BUA
    SetCallOwner(pszCallId, &m_clsIbcf);
}
```

**트렁크 설정 (SipServerXml/):**

```xml
<SipServer>
  <Name>IP-PBX</Name>
  <Ip>10.0.0.100</Ip>
  <Port>5060</Port>
  <RoutePrefix>9</RoutePrefix>        <!-- 9로 시작하는 번호 -->
  <IncomingRoute>1234=5678</IncomingRoute>  <!-- 역방향 매핑 -->
</SipServer>
```

### 3.6 CCmpClient

**파일:** `CmpClient.h/.cpp`

CMP(미디어 서버)와 JSON-over-UDP 통신.

**통신 구조:**

```
CCmpClient
  ├─ KeepAliveLoop (30초 주기 heartbeat)
  ├─ RecvLoop (CMP 응답 수신, transId 매칭)
  └─ SendRequestAndWait() (동기 요청, 2초 타임아웃)
```

**주요 명령:**

| 명령 | 용도 | 응답 |
|------|------|------|
| `ADD_SESSION` | VoIP RTP relay 세션 생성 | local_ip, local_port, local_video_port |
| `REMOVE_SESSION` | 세션 해제 | OK |
| `ADD_GROUP` | PTT 그룹 RTP 생성 | ip, port, floor_port, video_port |
| `JOIN_GROUP` | 멤버 그룹 참가 (user_floor_port 포함) | OK |
| `LEAVE_GROUP` | 멤버 그룹 퇴장 | OK |
| `REMOVE_GROUP` | 그룹 해제 | OK |
| `ALIVE` | 연결 상태 확인 | OK |
| `STATS` | CMP 통계 조회 | sessions, groups, rtp_ports 등 |

**Flow 메타 필드 (모든 Session/Group API 파라미터):**

`CCmpClient` 의 Session/Group 메서드는 공통으로 다음 파라미터를 받아 payload 에 `service/sesid/caller/callee` 로 주입:

```cpp
bool AddSession(const std::string& sesid,
                const std::string& service,   // "volte" | "mcptt" | ...
                const std::string& caller,
                const std::string& callee,
                const std::string& sessionId,
                const std::string& remoteIp, int remotePort, ...);
```

- `service` 는 cmd 이름을 기준으로 자동 결정 (ADD_GROUP/JOIN_GROUP/... → mcptt, ADD_SESSION → volte 등) 되지만 명시 인자가 우선.
- 응답 Flow 엔트리는 `Transaction` 에 저장된 caller/callee/sesid/service 를 그대로 상속.

**트랜잭션 처리:**

```
SendRequestAndWait(payload)
  ├─ transId 할당 (m_iNextTransId++)
  ├─ m_mapTransactions[transId] = Transaction
  ├─ UDP 전송
  ├─ condition_variable.wait(2초 타임아웃)
  └─ RecvLoop에서 transId 매칭 → notify
```

**연결 상태 관리:**

```
m_bConnected = false
  │
  ├─ KeepAliveLoop → ALIVE 전송
  │   ├─ 응답 수신 → m_bConnected = true
  │   └─ 타임아웃 → m_bConnected = false
  │       └─ m_fnConnectionCallback(false) → GroupCallService 통보
  │
  └─ 재연결 시 → m_fnConnectionCallback(true)
      └─ GroupCallService::OnCmpStatusChanged() → 그룹 재생성
```

### 3.7 CCallDir

**파일:** `CallDir.h`

Session-ID 기반 서비스 로깅 디렉토리 관리.

**디렉토리 구조:**

```
{ServiceLogDir}/
  ├─ voip/YYYY/MM/DD/HH/{prefix}/{caller}/*.d/
  │   ├─ call.json           (통화 메타데이터)
  │   ├─ participants.jsonl   (참가자 목록)
  │   ├─ session.json         (Session-ID ↔ Call-ID 매핑)
  │   ├─ csp.jsonl            (CSP 이벤트)
  │   └─ cmp.jsonl            (CMP 이벤트)
  │
  └─ ptt/{group_id}/sessions/{session_key}.d/
      ├─ session.json         (세션 메타 + 그룹 스냅샷)
      ├─ events.jsonl          (멤버 참가/퇴장/Floor 이벤트)
      ├─ daily/YYYY-MM-DD.jsonl
      └─ recordings/           (CMP 녹취 파일)
```

**Session-ID 생성:**

```
S{YYYY}{MM}{DD}{HH}{MM}{SS}{microsec}
예: S20260413143256789012
```

**B2BUA 양 leg 매핑:**

```cpp
// 양 leg의 Call-ID를 동일 Session-ID로 매핑
gclsCallDir.MapCallToSession(incomingCallId, sessionId);
gclsCallDir.MapCallToSession(outgoingCallId, sessionId);
// session.json에 기록
gclsCallDir.WriteSessionMapping(sessionId, incomingCallId, outgoingCallId);
```

**call.json 형식:**

```json
{
  "call_id": "abc123",
  "call_type": "voip",
  "initiator": "1001",
  "callee": "1002",
  "state": "active",
  "invite_time": "2026-04-13T12:34:56",
  "answer_time": "2026-04-13T12:34:58",
  "end_time": null,
  "duration": 0,
  "end_reason": null,
  "has_video": false
}
```

### 3.8 SipMessageLogger

**파일:** `SipMessageLogger.h/.cpp`

psip SIP 스택의 ILogCallBack 구현. 모든 SIP TX/RX와 CMP/CSC JSON 메시지를 기록. sesid 발급 및 서비스 분류의 중앙 허브.

**서비스 분류 (도메인 기반):**

`SipServerSetup::Realm` 배열에서 도메인 → 서비스 매핑을 로드한 후 `SetDomainServiceMap()` 으로 주입. SIP From/To 도메인을 맵에서 조회하여 `volte` / `mcptt` / `system` / `console` 중 하나로 분류한다. MCPTT 키워드 fallback 은 제거 — 도메인 매칭 전용.

**sesid API:**

| 메서드 | 역할 |
|--------|------|
| `IssueSesId(caller, module="csp")` | 새 sesid 발급 (`{caller}::{module}::{us_ts}::{counter}`) |
| `GetOrIssueSesId(callId, caller)` | Call-ID 기반 sesid 조회/없으면 발급 |
| `GetSesIdByCallId(callId)` | Call-ID → sesid 조회 (없으면 "") |
| `SetCallSesId(callId, sesid)` | B2BUA leg 간 sesid 명시적 상속 |

**출력 파일:**

```
{MsgLogDir}/csp/{service}/YYYY/MM/DD/HH/
  ├─ {systemId}.flow.jsonl          (서비스별 Flow 요약)
  └─ {systemId}_{node}.msg.jsonl    (원문 저장; node = sip/cmp/csc)
```

> `service` 는 volte/mcptt/system/console. `node` 는 원문 소스(sip/cmp/csc).

**Flow 로그 필드 순서 (빈 키 생략):**

```json
{
  "ts": "12:34:56.123456",
  "service": "volte",
  "caller": "+821357007001",
  "callee": "+821357007002",
  "sesid": "+821357007001::csp::1713340376123456::1",
  "subid": "abc123@1.2.3.4",
  "node": "sip",
  "from": "ue",
  "to": "csp",
  "proto": "SIP",
  "method": "INVITE",
  "detail": "sip:+821357007002@ims.mnc001...",
  "mid": 42,
  "seq": 101,
  "iface": "sip"
}
```

전체 규격·사례는 [../features/flow_logging.md](./../features/flow_logging.md) 참고.

### 3.9 CSubscriptionManager

**파일:** `SubscriptionManager.h/.cpp`

SIP SUBSCRIBE/NOTIFY 다이얼로그 상태 관리.

**구독 타입:**

| 이벤트 | 용도 | 본문 형식 |
|--------|------|-----------|
| gms | 그룹 멤버십 변경 알림 | xcap-diff XML |
| cms | 사용자 설정 변경 알림 | xcap-diff XML |

**SubscriptionInfo:**

```cpp
struct SubscriptionInfo {
    std::string strUserId;         // 가입자 ID
    std::string strSubscriberUri;  // From URI (AoR)
    std::string strFromTag;        // SUBSCRIBE From-tag
    std::string strToTag;          // 서버 To-tag
    std::string strContact;        // NOTIFY 전송 대상
    std::string strCallId;         // SIP 다이얼로그 ID
    std::string strEventType;      // "gms" 또는 "cms"
    int iExpires;                  // 구독 유효기간 (초)
    time_t tStartTime;             // 구독 시작 시각
    int iNotifySeq;                // NOTIFY CSeq 카운터
};
```

**갱신 흐름:**

```
그룹 설정 변경 (CSC → CSP)
  │
  └─ CCscInterface 수신 → GROUP_CHANGED
      ├─ gclsGroupMap.LoadFromDb()  // 그룹 캐시 갱신
      ├─ GroupCallService::OnGroupConfigChanged()
      └─ SubscriptionManager::SendNotifyAll("gms")
          └─ 모든 gms 구독자에게 NOTIFY (xcap-diff XML)
```

### 3.10 CCscInterface

**파일:** `CscInterface.h/.cpp`

CSC(관리 서버)로부터 설정 변경 이벤트를 UDP로 수신.

**수신 포트:** 4421 (UDP)

**이벤트 형식:**

```json
{
  "event": "GROUP_CHANGED",
  "uri": "tel:+821001",
  "action": "PUT",
  "etag": "v2"
}
```

**이벤트 처리:**

| 이벤트 | 처리 |
|--------|------|
| `USER_CHANGED` | CspUserMap 캐시 즉시 갱신/삭제 |
| `GROUP_CHANGED` | 그룹 설정 reload + CMP 동기화 + GMS NOTIFY 발송 |
| `STATS_REQUEST` | CSP 통계 응답 (등록자 수, 활성 호 등) |
| `CSC_RESTART` | DB 전체 재동기화 |

---

## 4. 데이터 관리

### 4.1 가입자 관리 (CspUserMap)

**우선순위:** DB (MariaDB) > JSON 파일 (User/*.json)

**CspUser 구조:**

| 필드 | 타입 | 설명 |
|------|------|------|
| m_strId | string | 사용자 ID (MSISDN) |
| m_strName | string | 표시 이름 |
| m_strAuthId | string | SIP 인증 ID |
| m_strPassWord | string | SIP 인증 비밀번호 |
| m_bDnd | bool | 착신거부 (DND) |
| m_strForward | string | 착신전환 번호 |
| m_vecReject | vector | 개별 착신거부 목록 |
| m_strServiceType | string | "voip", "ptt", "both" |
| m_strOrganizationId | string | 소속 조직 |

**온라인 사용자 (CUserInfo):**

| 필드 | 타입 | 설명 |
|------|------|------|
| m_strIp | string | Contact IP |
| m_iPort | int | Contact 포트 |
| m_eTransport | enum | UDP/TCP/TLS |
| m_iLoginTime | time_t | 등록 시각 |
| m_iLoginTimeout | int | Expires (초) |

### 4.2 그룹 관리 (CGroupMap)

**우선순위:** DB > JSON 파일 (Group/*.json)

60초 주기 자동 reload. CSC GROUP_CHANGED 이벤트 시 즉시 reload.

### 4.3 호 관리 (CCallMap)

```cpp
// B2BUA 양 leg 연결
struct CallMapEntry {
    std::string incomingCallId;   // 착신 leg Call-ID
    std::string outgoingCallId;   // 발신 leg Call-ID
    int rtpPort;                  // CMP relay 포트
    std::string sessionId;        // Session-ID
};
```

### 4.4 DB 스키마 (CDbManager)

**주요 테이블:**

| 테이블 | 용도 |
|--------|------|
| users | 가입자 기본 정보 |
| voip_subscriptions | VoIP 회선 (ID, AuthId, Password, DND, Forward) |
| ptt_subscriptions | PTT 회선 (ID, AuthId, Password) |
| ptt_groups | PTT 그룹 설정 |
| ptt_group_members | 그룹 멤버십 |
| voip_call_logs | VoIP 통화 이력 |
| ptt_call_logs | PTT 그룹콜 이력 |
| recordings | 녹취 메타데이터 |

---

## 5. 스레딩 모델

| 스레드 | 개수 | 역할 |
|--------|------|------|
| Main | 1 | ServiceMain 루프: 주기적 정리, 설정 reload |
| SIP UDP RX | 2 (설정) | SIP UDP 수신 |
| SIP TCP RX | 2 (설정) | SIP TCP 연결 수락 |
| SIP Callback Pool | 5 (설정) | ISipStackCallBack 디스패치 |
| CMP KeepAlive | 1 | CMP heartbeat (30초) |
| CMP RecvLoop | 1 | CMP 응답 수신 |
| CSC Interface | 1 | CSC 이벤트 수신 (TCP 4421) |
| GroupCall Monitor | 1 | 그룹 상태 주기 점검 |
| Monitor Server | 1 | HTTP 모니터링 인터페이스 |

**동기화:** CSipMutex (recursive mutex) 기반. 전역 객체별 독립 잠금.

---

## 6. 설정 (csp.json)

```json
{
  "Setup": {
    "Sip": {
      "LocalIp": "0.0.0.0",
      "LocalPort": 5060,
      "TcpPort": 25061,
      "TlsPort": 5061,
      "AuthRealm": "csp",
      "Realm": [
        { "service": "volte",   "domains": ["ims.mnc001.mcc450.3gppnetwork.org"] },
        { "service": "mcptt",   "domains": ["ptt.mnc001.mcc450.3gppnetwork.org"] },
        { "service": "system",  "domains": ["csp"] },
        { "service": "console", "domains": ["csc"] }
      ],
      "UdpThreadCount": 2,
      "TcpThreadCount": 2,
      "TcpCallBackThreadCount": 5,
      "StackPeriod": 20
    },
    "RtpRelay": {
      "CmpIp": "127.0.0.1",
      "CmpPort": 9000,
      "LocalCmpPort": 9001
    },
    "Roles": {
      "CSCF": true,
      "TAS": true,
      "PTT_AS": true,
      "IBCF": false
    },
    "Database": {
      "DbHost": "127.0.0.1",
      "DbPort": 3306,
      "DbUser": "cims",
      "DbPassword": "cims",
      "DbName": "cims"
    },
    "Log": {
      "LogFolder": "log",
      "MsgLogDir": "msg_log",
      "ServiceLogDir": "service_log",
      "LogMaxSizeMB": 10,
      "LogDebug": false
    },
    "DataFolder": {
      "UserDataFolder": "User",
      "GroupDataFolder": "Group",
      "SipServerFolder": "SipServerXml"
    },
    "ServiceMode": "both",
    "Cdr": {
      "CdrFolder": "cdr"
    },
    "SessionTimeout": 600,
    "RecordEnable": true
  }
}
```

---

## 7. 초기화 순서

```
ServiceMain()
  1. 설정 파일 로드 (SipServerSetup)
  2. 로깅 초기화 (CLog, MsgLogger, CallDir, SipMessageLogger)
  3. SipServerMap 로드 (IP-PBX 트렁크)
  4. CCmpClient 초기화 → CMP 연결
  5. CGroupMap 로드 (DB 또는 파일)
  6. CspUserMap 로드 (DB 또는 파일)
  7. MariaDB 연결 (DbHost 설정 시)
  8. CCscInterface 시작 (TCP 4421)
  9. CModuleDispatcher 시작 (SIP 스택 + 콜백 등록)
  10. Monitor Server 시작
  11. Main Loop:
      ├─ NonceMap 만료 정리
      ├─ UserMap 등록 타임아웃 정리
      ├─ CallMap stale 호 정리
      ├─ SipServerMap 60초 주기 reload
      └─ GroupMap 60초 주기 reload
```

---

## 8. 외부 인터페이스 요약

| 인터페이스 | 프로토콜 | 포트 | 상대 |
|------------|----------|------|------|
| SIP UDP | SIP/UDP | 5060 | UE (단말) |
| SIP TCP | SIP/TCP | 25061 | UE (단말) |
| SIP TLS | SIP/TLS | 5061 | UE (단말) |
| CMP 명령 | JSON/UDP | 9000 | CMP (미디어) |
| CMP 응답 | JSON/UDP | 9001 | CMP (미디어) |
| CSC 이벤트 | JSON/UDP | 4421 | CSC (관리) |
| IP-PBX | SIP/UDP | 5060 | 외부 SIP 서버 |

---

## 9. 관련 문서

- [../features/flow_logging.md](./../features/flow_logging.md) — Flow 로깅, sesid, Realm 배열, 모듈 간 인터페이스의 공통 필드(`service`/`sesid`/`caller`/`callee`) 규격
