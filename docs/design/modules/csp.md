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

**SIP 헤더 주입 (IMS 규격 준수):**

REGISTER 200 OK 와 B2BUA 발신 INVITE 에 `P-Asserted-Identity`(`<sip:user@domain>`)를 주입한다.

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

floor 없는 세션(`floor_control:"off"` — private 멀티)은 CMP 가 `floor_port` 를 주지 않는다.
이때 fan-out SDP 의 floor 라인은 **`m=application 0`**(미사용, RFC 3264 §6)으로 내고
`a=fmtp:MCPTT mc_no_floor_ctrl` 만 에코한다 — 관례 fallback(멤버 audio+1)을 그대로 두면
멤버 RTCP 포트가 floor 로 오광고되어 단말이 그 포트로 floor 연결을 시도한다.

발신자에게 주는 **200 OK answer 도 같은 규칙**을 따른다 — psip `CSipDialog::AddSdp` 는 광고할
floor 포트가 없어도 상대 offer 에 `m=application` 이 있었으면 **포트 0 라인을 반드시 넣는다**
(RFC 3264 §6: answer 의 m= 라인 개수·순서는 offer 와 같아야 하고, 쓰지 않는 스트림은 라인을
지우는 것이 아니라 포트 0 으로 거절한다). 라인을 생략하면 m= 개수가 어긋나 협상을 엄격히
구현한 단말이 answer 를 거부한다. 세션 중 offer(re-INVITE)에도 같은 규칙이 적용된다.

**그룹 단위 통일 sesid:**

PTT 그룹 세션에 대한 모든 모듈간 메시지(`PTT_GROUP_ADD`, `PTT_JOIN`, `PTT_LEAVE`, `PTT_GROUP_REMOVE`)와 SIP INVITE leg 들이 동일 sesid 를 공유하도록 per-group 매핑 유지:

```cpp
class CGroupCallService {
    std::map<std::string, std::string> m_mapGroupSesId;  // group_id → sesid
    std::string GetOrIssueGroupSesId(const std::string& groupId);
    void        RemoveGroupSesId(const std::string& groupId);
};
```

- sesid 형식: `{group_id}::csp::{us_ts}::{counter}` (caller 자리에 group_id)
- 그룹 해체(`PTT_GROUP_REMOVE` 완료) 시 매핑 제거
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
  │   (record_dir 전달)
  ├─ 발신자에게 200 OK (공유 RTP 주소)
  ├─ 매핑: callerId → groupId (m_mapUserCall)
  │
  └─ 각 그룹 멤버에 대해:
      ├─ (affiliation 게이트: require_affiliation 시 affiliate 된 멤버만)
      ├─ InviteMember() → Multipart INVITE
      │   ├─ Content-Type: multipart/mixed
      │   ├─ Part 1: application/vnd.3gpp.mcptt-info+xml
      │   ├─ Part 2: application/resource-lists+xml (멤버 로스터 role/priority; INVITE>8192B 시 생략)
      │   └─ Part 3: SDP (공유 RTP + m=application floor)
      ├─ 멤버 200 OK 수신 → m=application floor 파싱 → CMP PTT_JOIN(role 포함)
      └─ 매핑: memberCallId → {groupId, memberId, sessionId}
```

**MCPTT INVITE 헤더 주입 (3GPP 규격 준수):**

그룹 INVITE 에는 MCPTT 서비스 식별을 위한 헤더를 함께 주입한다.

- `P-Preferred-Service: urn:urn-7:3gpp-service.ims.icsi.mcptt`
- `Accept-Contact` (ICSI ref)
- `Answer-Mode: Auto`

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
  ├─ KeepAliveLoop (3초 주기 HEARTBEAT, 연속 3회 실패 시 Disconnected 판정)
  ├─ RecvLoop (CMP 응답 수신, transId 매칭)
  └─ SendRequestAndWait() (동기 요청, 100ms 대기 × 3회 재전송)
```

**주요 명령** (wire 는 envelope v2 `{hdr, payload}` — 정본
[../../api/cmp_media_api.md](../../api/cmp_media_api.md), payload 필드는 [cmp.md](cmp.md) §3.2):

| 명령 | 용도 | 응답 payload |
|------|------|------|
| `RELAY_ADD` | VoIP RTP relay 세션 생성 (+`remote_nat`/`remote_sig_ip` — leg NAT 지시) | local_ip, local_port(_b), local_video_port(_b) — leg 별 전용 포트 |
| `RELAY_MODIFY` | 세션 remote 주소 갱신 (re-INVITE 등) | 동일 |
| `RELAY_REMOVE` | 세션 해제 | — (hdr.status 만) |
| `PTT_GROUP_ADD` | PTT 그룹 RTP 생성 (+`floor_policy`/`max_talkers` — 동시 발언 정책) | ip, floor_port, member_ports(멤버별 전용 포트 맵) |
| `PTT_GROUP_MODIFY` | 그룹 멤버/우선순위·floor 정책 갱신 | 동일 |
| `PTT_JOIN` | 멤버 그룹 참가 — 2단 멱등: user_ip 없이 선할당 → 주소 갱신 (+`user_nat`/`user_sig_ip`) | ip, port, video_port (멤버 전용) |
| `PTT_LEAVE` | 멤버 그룹 퇴장 | — |
| `PTT_GROUP_REMOVE` | 그룹 해제 | — |
| `PTT_FLOOR_TIER` | 멤버 floor tier 런타임 변경 (emergency/imminent/normal) | — |
| `HEARTBEAT` | 연결 상태 확인 (3초 주기, hdr-only) | resource 요약 (relay/ptt total·used 등) |

> `STATS` 는 CMP 가 처리하는 통계 조회 명령이지만 CSP(CCmpClient)는 송신하지 않는다 —
> OAM(`ems/core/oam` stats 핸들러)과 검증 파이프라인(stage6)이 CMP 9000/UDP 로 직접 조회한다.

**Floor 정책 발행 (동시 발언 — TS 24.380)**

그룹의 동시 발언 정책은 DB `ptt_groups.floor_policy`(`single`/`dual`/`multi`) 와 `max_talkers`
가 원천이고(JSON fallback 은 같은 이름의 키), `CspPttGroup._floorPolicy`/`_maxTalkers` 로 실려
`PTT_GROUP_ADD`/`_MODIFY` payload 에 나간다. floor 절차 자체는 CMP↔UE in-band 라 CSP 는 정책만
전달하고 floor 루프에 들어가지 않는다 ([../features/mcptt_csp_cmp_roadmap_contract.md](../features/mcptt_csp_cmp_roadmap_contract.md) §B.1).

CMP 는 미상 policy 값과 `multi` 의 범위 밖 `max_talkers`(계약 2..8)를 `BAD_REQUEST` 로 거절하며,
거절되면 그룹 생성 자체가 실패해 통화 불가가 된다. 따라서 `CCmpClient::SetFloorPolicy` 가 발행
직전에 검증해 잘못된 설정은 `single` 로 낮춰 보내고 `LOG_ERROR` 로 남긴다 — 설정 오류가 통화
장애로 번지지 않게 하되 조용히 묻히지도 않게 한다.

정책 변경은 `PTT_GROUP_MODIFY` 로 전달된다. 변경 감지는 `ComputeGroupConfigHash`(로스터 +
floor 정책)가 담당하므로, 멤버가 그대로여도 정책만 바꾸면 `SyncGroupsState` 가 MODIFY 를 보낸다.
정원이 줄면 CMP 가 초과 화자를 Revoke 해 상태를 정책에 맞춘다.

**멤버별 floor 협상 전달 (SDP fmtp → PTT_JOIN)**

멤버 SDP(개시자=INVITE offer, fan-out 수신자=200 OK answer)의 `a=fmtp:MCPTT
mc_queueing[;mc_priority=N][;mc_granted]` 를 `CGroupCallService::ParseMcpttFmtp` 가 파싱해
`PTT_JOIN` 의 `queueing`/`max_priority`/`granted` 로 전달한다(`McpttFmtp` 구조체,
[../../api/cmp_media_api.md](../../api/cmp_media_api.md) §7.4). 규칙:

- `fmtp:MCPTT` 부재(레거시 단말) → 세 필드 모두 미전송 — CMP 기본(queueing 1)이 유지되어
  구단말이 깨지지 않는다.
- `fmtp:MCPTT` 는 있는데 `mc_queueing` 이 없으면 `queueing:0` — 미협상 멤버의 비선점 요청은
  CMP 가 Deny #1 (TS 24.380 §6.3.5.4.4).
- 재협상(re-INVITE)도 같은 경로(`OnCallStarted` 멱등 JOIN)로 최신 협상값이 재전달된다.

CSP 자신도 fan-out INVITE offer(`WrapMultipartBody`)와 psip `CSipDialog::AddSdp`(개시자 200 OK
answer)에 `a=fmtp:MCPTT mc_queueing` 을 광고한다.

> multipart body 의 SDP part 는 경계 탐색이 마지막 라인의 CRLF 를 소비하므로, psip
> `GetSipCallRtp` 가 종결 CRLF 를 복원해 추출한다 — 복원하지 않으면 라인 단위 SDP 파서가
> 마지막 라인(단말 INVITE 에서는 대개 `a=fmtp:MCPTT …`)을 버린다.

**Private call (1:1) — TS 24.379 §11.1 on-demand**

mcptt-info `session-type:private` INVITE 를 받으면(타겟=그룹이 아닌 등록 PTT 가입자, 미등록이면
480) 합성 2인 ephemeral 그룹 `priv-<발신>-<착신>` 을 만들어 **기존 그룹콜 경로(ProcessGroupCall
fan-out·CMP 세션·teardown)를 그대로 재사용**한다(`ModuleDispatcher::EventIncomingCall`, 계약
[../features/mcptt_csp_cmp_roadmap_contract.md](../features/mcptt_csp_cmp_roadmap_contract.md) §A.1).

- **affiliation 불요** — `_requireAffiliation=false` 로 멤버십 게이트를 우회한다(상대 MCPTT ID
  직접 지정). `_isAdhoc=true` 라 통화 종료 시 GroupMap 에서 제거된다(ephemeral).
- **floor 유무** — 발신 offer 의 fmtp `mc_no_floor_ctrl`(G17) 협상 시
  `PTT_GROUP_ADD.floor_control:"off"`(full-duplex, `floor_port` 미광고). 기본은 on(2인 floor).
- **싱글/멀티 토커** — 단말이 거는 1:1 은 두 가지다. **싱글**=`floor_control:"on"`(한 번에 한
  명, 2인 floor 절차). **멀티**=규격 `mc_no_floor_ctrl` 세션(`off`)에 단말이 로컬 마이크
  게이트를 얹어 동시 발언을 허용한다 — 서버는 양방향 상시 중계만 하고 발언 중재를 하지
  않는다. 멀티에서도 마이크는 상시 개방이 아니라 단말 PTT 로 여닫는다(단말 책임). 멀티토커를
  floor 절차로 1:1 에 넣는 것은 규격 밖이다(동시 발언은 그룹 전용 `floor_policy` 축).
- `PTT_GROUP_ADD` 에 `group_type:"private"` + `initiator_id`(발신자=초기 발언권 후보) + 멤버
  정확히 2 를 싣는다. private 은 동시성 축을 해석하지 않으므로 `floor_policy` 는 **미전송**.
- **모드 오염 방지** — 잔존 ephemeral 그룹의 floor 모드가 이번 발신과 다르면 재사용하지 않고
  CMP `PTT_GROUP_REMOVE` 후 재생성한다(`EventIncomingCall`).
- **종료 전파와 teardown 완결** — 한쪽이 끊으면 세션 전체가 끝난다(그룹 시맨틱 미적용).
  `OnCallTerminated` 가 잔여 leg 에 BYE 를 보내고 **그 leg 의 teardown 을 직접 재진입 호출**한다.
  psip 은 로컬 `StopCall` 로 끝낸 호에 `EventCallEnd` 를 올리지 않으므로, BYE 만 보내면 마지막
  leg 의 그룹 해제·`PTT_GROUP_REMOVE`·멤버 포트 회수·녹취 마감이 실행되지 않는다(BYE 응답
  유무와 무관해야 한다 — 미응답 단말이 그룹을 붙들면 안 된다). 마지막 leg 처리에서
  `_isAdhoc` 그룹을 GroupMap 에서도 제거한다(de-register 경로와 동일 계약).

**Flow 메타 필드 (모든 Session/Group API 파라미터):**

`CCmpClient` 의 Session/Group 메서드는 공통으로 다음 파라미터를 받고, `_SendOnEndpoint` 가
envelope 조립 시 `sesid/service` 는 hdr 로, `caller/callee` 는 payload 로 배치한다:

```cpp
bool AddSession(const std::string& sesid,
                const std::string& service,   // "volte" | "mcptt" | ...
                const std::string& caller,
                const std::string& callee,
                const std::string& sessionId,
                const std::string& remoteIp, int remotePort, ...);
```

- `service` 는 cmd 이름을 기준으로 자동 결정 (cmd 에 `PTT` 포함 → mcptt, `RELAY` 포함 → volte, 그 외 → system) 되지만 명시 인자가 우선.
- 응답 Flow 엔트리는 `Transaction` 에 저장된 caller/callee/sesid/service 를 그대로 상속.

**트랜잭션 처리:**

```
SendRequestAndWait(payload)
  ├─ transId 할당 (m_iNextTransId++)
  ├─ m_mapTransactions[transId] = Transaction (shared_ptr)
  ├─ 최대 3회 시도: UDP 전송 → condition_variable.wait_for(100ms, 술어=bCompleted)
  │   └─ 무응답이면 동일 trans_id+payload 재전송 (명령이 session_id 기준 멱등이라 안전)
  └─ RecvLoop에서 transId 매칭 → notify (총 대기 ceiling ≈ 300ms)
```

멀티 CMP endpoint(HA) 시 session_id/group_id 기반 consistent hash ring 으로 endpoint 를
sticky 선택하고, 단일 endpoint 환경에서는 primary 를 사용한다.

**연결 상태 관리:**

```
m_bConnected = false
  │
  ├─ KeepAliveLoop (3초 주기) → HEARTBEAT 전송
  │   ├─ 응답 수신 → m_bConnected = true, 실패 카운터 리셋
  │   └─ 타임아웃 → 실패 카운터 증가
  │       └─ 연속 3회 실패 (≈9초 무응답) → m_bConnected = false
  │           └─ m_fnConnectionCallback(false) → GroupCallService 통보
  │
  └─ 재연결 시 → m_fnConnectionCallback(true)
      └─ GroupCallService::OnCmpStatusChanged() → 그룹 재생성
```

단발 HEARTBEAT 타임아웃(부하 시 간헐 발생)으로는 끊김 판정하지 않는다 — 연속 3회 실패에서만
Disconnected 로 전환해 과민 teardown 을 방지한다.

### 3.7 CCallDir

**파일:** `CallDir.h`

Session-ID 기반 서비스 로깅 디렉토리 관리.

**디렉토리 구조:**

```
{ServiceLogDir}/
  ├─ volte/YYYY/MM/DD/HH/{prefix}/{caller}/*.d/
  │   ├─ call.json           (통화 메타데이터)
  │   ├─ participants.jsonl   (참가자 목록)
  │   ├─ session.json         (Session-ID ↔ Call-ID 매핑)
  │   └─ seg_NNNN_*.rtp / seg_NNNN.json / segments.jsonl  (CMP 녹취)
  │
  └─ ptt/{id}/                         # id = ptt_groups.id (surrogate)
      ├─ group.json                     (그룹 디스크립터: id/mcptt_group_id/group_type/members[role]…)
      └─ {YYYY}/{MM}/{DD}/{HH}/         # 시간버킷
          ├─ events.jsonl               (멤버 참가/퇴장)
          ├─ floor.jsonl                (floor GRANT/REVOKE/REJECT/RELEASE/IDLE)
          ├─ segments.jsonl
          └─ seg/{NNN}/seg_NNNN_*.rtp + seg_NNNN.json   (100세그 shard)
```
> 상세 [recording.md](../features/recording.md).

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

**출력 파일** (open-per-write · 5분 버킷):

```
{ServiceLogDir}/YYYY/MM/DD/HH/
  ├─ {systemId}.flow.{mm5}.jsonl         (통합 Flow 요약; mm5=5분 버킷 00/05/.../55)
  └─ {systemId}_{iface}.msg.{mm5}.jsonl  (원문 저장; iface = sip/cmp/csc)
```

- **open-per-write**: 매 줄 `fopen(append)`→write→`fclose`. 시간당 핸들 유지 폐기 → 운영 중 로그삭제 시
  `.nfs` 고아·dead-inode 데이터유실 방지, 대용량 파일 검색 부담 완화.
- **5분 버킷**: `mm5 = (분/5)*5`. 버킷 전환 시 iface `seq`(줄번호) 리셋, 첫 write 가 기존 줄 수를 계수해 이어붙임(재기동 연속성).
- reader(`flow_logger.py`)는 `.msg.jsonl`(구 시간당) + `.msg.{mm5}.jsonl`(신) 모두 glob. 원문 역조회는 flow 엔트리 `ts`→버킷 도출.

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

| 이벤트 | 규격 | 용도 | 본문 형식 |
|--------|------|------|-----------|
| reg | RFC 3680 | 자기 등록 상태(생성/갱신/해제/만료) | reginfo XML |
| affiliation | RFC 3856 (presence) | 제휴 상태 변경 | mcptt-affiliation-info XML |
| conference | RFC 4575 | 그룹 참가자 로스터 | conference-info XML |
| gms | RFC 5875 (xcap-diff) | 그룹 멤버십 변경 알림 | xcap-diff XML |
| cms | RFC 5875 (xcap-diff) | 사용자 설정 변경 알림 | xcap-diff XML |

타입 판별은 `CscfModule` 의 `Event` 헤더 우선 순서를 따른다: `reg` → `affiliation`(Event:presence
또는 Accept 에 mcptt-affiliation-info) → `conference`(Event:conference **또는** Request-URI 가 알려진
그룹 — Event 헤더 없는 구현 호환) → Request-URI 의 gms/cms → 기본값 gms.

⚠️ **갱신(in-dialog refresh) SUBSCRIBE 는 이 판별을 타면 안 된다.** 갱신 요청의 Request-URI 는
자원이 아니라 200 OK 의 Contact(서버 자기 주소)이므로, URI 로 재분류하면 conference 구독이 gms 로
떨어져 엉뚱한 `Event: xcap-diff` NOTIFY 가 나가고 구독자 스택이 481 로 구독을 죽인다. 갱신은
Call-ID 로 기존 구독의 event/resource 를 승계한다(reg/gms/cms 공통).

**SubscriptionInfo:**

```cpp
struct SubscriptionInfo {
    std::string strUserId;         // 가입자 ID
    std::string strSubscriberUri;  // From URI (AoR)
    std::string strFromTag;        // SUBSCRIBE From-tag
    std::string strToTag;          // 서버 To-tag
    std::string strContact;        // NOTIFY 전송 대상
    std::string strCallId;         // SIP 다이얼로그 ID
    std::string strEventType;      // "reg"|"affiliation"|"conference"|"gms"|"cms"
    std::string strResourceId;     // 구독 대상 자원 (conference 는 그룹 ID)
    int iExpires;                  // 구독 유효기간 (초)
    time_t tStartTime;             // 구독 시작 시각
    int iNotifySeq;                // NOTIFY CSeq 카운터
};
```

**구독 종료 사유:**

| 사유 | 계기 | 로그 |
|------|------|------|
| 정상 해지 | `SUBSCRIBE Expires: 0` 수신 | `Subscription Removed` |
| 만료 | `CheckExpired()` 스위퍼 (30초 주기, `tStartTime + iExpires` 경과) | `Subscription Expired` |
| **NOTIFY 최종 실패** | NOTIFY 응답 **481/404/410** 또는 트랜잭션 타임아웃 | `Subscription Reaped` |

NOTIFY 최종 실패 회수는 RFC 6665 §4.2.2 정합 동작이다 — 구독자 dialog 가 사라졌다는 확정 신호를
받고도 구독을 남기면 만료까지 죽은 dialog 로 NOTIFY 가 계속 나간다. 앱을 비정상 종료(force-stop·
크래시·OOM)하면 구 인스턴스 구독이 남는데, CSP 는 NOTIFY 를 **현재 등록 바인딩**으로 보내므로
새 소켓에 중복 NOTIFY 가 도착하고 단말이 481 로 거절한다. 이 481 을 회수 신호로 쓴다.

`RecvResponse`(응답) / `SendTimeout`(타임아웃) 두 `ISipStackCallBack` 훅에서 처리하며, 둘 다
`false` 를 반환해 뒤따르는 콜백의 처리를 막지 않는다. **5xx 는 회수하지 않는다** — 구 APK 호환용
in-dialog 폴백 NOTIFY 가 정상적으로 500 을 주고, 실제 구독자의 5xx 는 일시적 오류일 수 있다.
(폴백 NOTIFY 는 Call-ID 가 구독 맵에 없어 어차피 무시된다. 죽은 **leg** 회수는 별건.)

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

### 4.3 호 관리 (CCallMap) + RTP relay bookkeeping

`CCallMap`(Call-ID 키)이 B2BUA 양 leg 매핑 + **CMP relay descriptor** 를 함께 보유한다(`SetRelayInfo`로 양 leg 에 동일 기록).

```cpp
class CCallInfo {                  // CallMap value (key = Call-ID)
    std::string m_strPeerCallId;   // 상대 leg Call-ID (B2BUA)
    int  m_iPeerRtpPort;           // 이 leg 의 peer 에게 광고할 CMP relay 포트 (leg 별 전용 —
                                   //   A entry=peer1 포트, B entry=peer0 포트)
    // ── relay descriptor: teardown/MODIFY 가 session_id 로 CMP 세션 직접 지목 ──
    std::string m_strRelaySessionId;  // csp_{yyyymmddHHMMSSmmm}_{n} (재시작 포함 전역 유일) ← CmpClient::IssueSessionId
    std::string m_strRelaySesId;      // flow 상관 sesid
    std::string m_strRelayLocalIp;    // CMP relay IP (answer MODIFY/SDP)
    std::string m_strRelayCaller, m_strRelayCallee;
    bool m_bEstablished; time_t m_iLastActivityTime;
};
```

- **teardown**: `CCallMap::Delete(callId)` 가 entry 의 `m_strRelaySessionId` 로 `gclsCmpClient.RemoveSession(session_id)` 직접 호출.
- **answer(200 OK)**: callee 주소를 `gclsCmpClient.ModifySession(session_id, …, peerIdx=1)` 로 CMP 에 MODIFY.
  이때 leg 별 NAT 판정(`CCspServiceMap::EvalMediaNat` — access_services 의
  `media_nat_mode`/`latch_ip_guard`, [ue_nat_traversal.md](../features/ue_nat_traversal.md))을
  동반한다. 발신 leg 는 `EventIncomingCall`(RELAY_ADD)에서 동일 판정.
- **stale 호 정리**: `DeleteTimeout` → `Delete`(bStopPort) → 동일 session_id RemoveSession.

relay bookkeeping 의 키는 **session_id**(`csp_{yyyymmddHHMMSSmmm}_{n}`, 재시작 경계 포함 전역 유일 — ms 타임스탬프+순번이라 재기동 후에도 CMP 잔존 고아와 충돌하지 않는다)다. 멀티 미디어노드(`MediaServer.Endpoints`) 환경에서 같은 포트가 노드별로 유일하지 않으므로, 포트가 아니라 session_id 로 CMP 세션을 지목한다. CSP 비정상 종료 시의 고아 relay 는 CMP sweeper 가 회수(cmp.md §5). (`RtpMap.h` 는 `SOCKET_COUNT_PER_MEDIA` 상수만 잔존.)

### 4.4 DB 스키마 (CDbManager)

**주요 테이블:**

| 테이블 | 용도 |
|--------|------|
| users | 가입자 기본 정보 |
| volte_subscriptions | VoLTE 회선 (ID, AuthId, Password, DND, Forward) |
| ptt_subscriptions | PTT 회선 (ID, AuthId, Password) |
| ptt_groups | PTT 그룹 설정 |
| ptt_group_members | 그룹 멤버십 |
| recordings / recording_segments | 녹취 메타데이터 |

통화 이력은 DB 미적재. 파일 기반 — `service_log/{volte|ptt}/.../<call_id>.d/call.json`. 전체 인벤토리는 [docs/design/db_schema.md](../db_schema.md).

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
    "Media": {
      "Codecs": [
        { "Name": "AMR-WB", "Pt": 96, "Clock": 16000, "Channels": 1, "Fmtp": "octet-align=1", "Ptime": 20 },
        { "Name": "PCMA", "Pt": 8 },
        { "Name": "telephone-event", "Pt": 101, "Fmtp": "0-15" }
      ]
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

### 6.1 SDP 코덱 테이블 (`Setup.Media.Codecs`)

SDP 오퍼/answer 의 오디오 코덱·payload type(PT)·fmtp·우선순위의 정본. CSP 기동 시 psip
`CSipCodecTable` 로 1회 주입된다 (재기동 반영). **비우면 내장 기본 테이블** — AMR-WB(96,
octet-align=1, ptime 20) 최우선 + AMR(98) + PCMU(0)/PCMA(8)/GSM(3)/G723(4)/G729(18) +
telephone-event(101, `0-15`).

| 필드 | 의미 | 기본 |
|---|---|---|
| `Name` | rtpmap encoding name (`AMR-WB`, `PCMA`, …). `telephone-event` 는 DTMF 슬롯으로 분리 취급 | 필수 |
| `Pt` | payload type — 정적 코덱은 RFC 3551 고정 번호(0~34), 동적 코덱은 96~127 (번호는 정책값) | 필수 |
| `Clock` / `Channels` | rtpmap clock rate / 채널 수 (0=미표기) | 8000 / 0 |
| `Fmtp` / `Ptime` | `a=fmtp` 값 (비면 미출력) / `a=ptime` (0=미출력) | — |

동작 원칙 (RFC 3264/3551):

- **배열 순서 = 우선순위.** 첫 엔트리가 서비스 코덱 — PTT 그룹콜 fan-out 오퍼가 이 코덱·PT 로
  나간다. 그룹 leg 별 wire PT 가 서로 달라도(개시자의 비 96 offer, 타사 단말의 비 96 answer)
  CSP 가 PTT_JOIN ② 에 leg 별 PT(`user_pt`/`user_src_pt`/`user_te_pt`/`user_src_te_pt` —
  UE 수신 선언 PT 는 그 leg 의 원격 SDP, UE 송신 PT 는 서버가 그 leg 에 낸 SDP 기준,
  psip `GetRemotePayloadTypes`)를 전달하고 **CMP 가 fan-out egress 에서 leg 별로 PT 를
  재작성**해 정합한다 ([cmp_media_api.md §7.4](../../api/cmp_media_api.md)). 기본 96 은
  실단말(pjsua)의 로컬 AMR-WB PT(pjmedia 동적 PT 재배정) 정렬 실증값.
- **서버가 answerer 일 때 실 PT 는 항상 오퍼 rtpmap echo.** 테이블 PT 는 오퍼에 해당 rtpmap 이
  없을 때의 폴백. 코덱 *선택*은 오퍼∩테이블 중 테이블 우선순위 최상위.
- **인바운드 오퍼의 동적 PT 는 rtpmap 이름으로 식별**한다 (번호 무관 — 예: AMR-WB 를 111 로
  오퍼해도 인식). 정적 PT 는 번호로 식별. 테이블에 없는 코덱만 담긴 오퍼는 PTT 에서 488
  (VoLTE B2BUA 는 media-list passthrough 라 서버가 코덱을 이해하지 못해도 단말끼리 협상 가능
  — 서버 코덱 게이트를 걸지 않는다).

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
