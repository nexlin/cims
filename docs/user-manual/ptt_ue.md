# 09. PTT(MCPTT) 단말 연동 인터페이스 규격

---

## 1. 개요

본 문서는 MCPTT(Mission Critical Push-To-Talk) 단말이 CIMS 서버와 연동하기 위한 프로토콜 규격을 정의한다.

### 1.1 대상

- MCPTT 전용 단말 (3GPP TS 24.379 기반)
- PTT 앱 개발자
- 레거시 PTT 단말 (DTMF 기반 Floor Control)

### 1.2 연동 포인트

| 인터페이스 | 프로토콜 | 포트 | 용도 |
|------------|----------|------|------|
| IdMS 인증 | HTTPS | 4430 | OAuth 2.0 PKCE 단말 인증 |
| GMS 그룹 조회 | HTTPS | 4430 | XCAP 그룹 정보 조회 |
| CMS 설정 조회 | HTTPS | 4430 | XCAP 사용자 설정 조회 |
| SIP 시그널링 | SIP/UDP | 5060 | 등록, 그룹콜 수신, 구독 |
| 오디오 RTP | RTP/UDP | 동적 (SDP m=audio) | 음성 스트림 |
| Floor Control | RTCP APP/UDP | 동적 (SDP m=application) | 발언권 제어 |

### 1.3 단말 부팅 시퀀스

```
1. IdMS 인증 (OAuth 2.0 PKCE) → access_token 획득
2. GMS 그룹 목록 조회 (HTTPS)
3. CMS 사용자 설정 조회 (HTTPS)
4. SIP REGISTER (Digest MD5 인증)
5. SIP SUBSCRIBE (GMS/CMS 변경 구독)
6. 그룹 INVITE 수신 대기 → 자동 응답
```

---

## 2. IdMS 인증 (OAuth 2.0 PKCE)

### 2.1 인증 흐름

```
UE                              IdMS (CSC:4430)
 │                               │
 │ ── POST /idms/authreq ──────► │
 │    client_id: "mcptt-ue"      │
 │    redirect_uri: "..."        │
 │    code_challenge: BASE64URL(SHA256(verifier))
 │    code_challenge_method: S256│
 │                               │
 │ ◄── 302 + auth_code ──────── │
 │                               │
 │ ── POST /idms/tokenreq ─────► │
 │    grant_type: authorization_code
 │    code: <auth_code>          │
 │    code_verifier: <원본 문자열>│
 │                               │
 │ ◄── 200 OK ──────────────── │
 │    access_token: "eyJ..."     │
 │    token_type: "Bearer"       │
 │    expires_in: 3600           │
 │    refresh_token: "ref..."    │
```

### 2.2 PKCE 계산

```
code_verifier  = 43~128자 랜덤 문자열 [A-Za-z0-9-._~]
code_challenge = BASE64URL(SHA256(code_verifier))
```

### 2.3 토큰 갱신

```
POST /idms/tokenreq
grant_type=refresh_token
refresh_token=<이전 refresh_token>
```

### 2.4 토큰 검증

```
GET /idms/introspect
Authorization: Bearer <access_token>
```

**응답:**
```json
{
  "active": true,
  "sub": "+82571900001",
  "mcptt_id": "sip:+82571900001@ptt.csp",
  "exp": 1713024000
}
```

---

## 3. GMS 그룹 정보 조회

### 3.1 그룹 목록

```
GET /org.openmobilealliance.groups/users/sip:+82571900001@ptt.csp/...
Authorization: Bearer <access_token>
```

**응답 (JSON):**
```json
{
  "groups": [
    {
      "group_id": "group_1000",
      "name": "작전 1팀",
      "members": [
        {"mcptt_id": "sip:+82571900001@ptt.csp", "priority": 1},
        {"mcptt_id": "sip:+82571900002@ptt.csp", "priority": 2},
        {"mcptt_id": "sip:+82571900003@ptt.csp", "priority": 3}
      ]
    }
  ]
}
```

### 3.2 그룹 상세 (OMA POC XML)

```
GET /org.openmobilealliance.groups/users/.../group_1000
Accept: application/vnd.oma.poc.groups+xml
```

**응답:**
```xml
<?xml version="1.0" encoding="UTF-8"?>
<group xmlns="urn:oma:xml:poc:list-service">
  <list-service uri="sip:group_1000@ptt.csp">
    <display-name>작전 1팀</display-name>
    <list>
      <entry uri="sip:+82571900001@ptt.csp">
        <display-name>사용자1</display-name>
      </entry>
      <entry uri="sip:+82571900002@ptt.csp">
        <display-name>사용자2</display-name>
      </entry>
    </list>
  </list-service>
</group>
```

---

## 4. CMS 사용자 설정 조회

### 4.1 사용자 프로파일

```
GET /org.3gpp.mcptt.user-profile/users/sip:+82571900001@ptt.csp/...
Authorization: Bearer <access_token>
```

### 4.2 서비스 설정

```
GET /org.3gpp.mcptt.service-config/users/sip:+82571900001@ptt.csp/...
Authorization: Bearer <access_token>
```

---

## 5. SIP 등록

### 5.1 인증 방식

Digest MD5 (VoLTE와 동일 방식)

### 5.2 등록 파라미터

| 항목 | 값 | 비고 |
|------|---|------|
| username | MCPTT ID (예: "+82571900001") | ptt_subscriptions.id |
| auth_id | IMPI (예: "+82571900001@ptt.csp") | ptt_subscriptions.auth_id |
| realm | PTT 도메인 (예: "ptt.csp") | csp.json → Setup.Sip.PttRealm |
| password | PTT 인증 비밀번호 | ptt_subscriptions.passwd |

### 5.3 등록 흐름

```
UE                              CSP (5060)
 │                               │
 │ ── REGISTER ────────────────► │
 │    From: <sip:+82571900001@ptt.csp>
 │    To: <sip:+82571900001@ptt.csp>
 │                               │
 │ ◄── 401 + WWW-Authenticate ── │
 │                               │
 │ ── REGISTER + Authorization ─► │
 │                               │
 │ ◄── 200 OK ──────────────── │
 │                               │
 │  [CSP: 소속 그룹 확인 → 그룹 INVITE 전송]
```

---

## 6. SIP SUBSCRIBE (GMS/CMS 변경 구독)

### 6.1 GMS 구독

```
SUBSCRIBE sip:gms_psi@ptt.csp SIP/2.0
From: <sip:+82571900001@ptt.csp>;tag=xxx
To: <sip:gms_psi@ptt.csp>
Event: xcap-diff
Expires: 3600
Content-Type: application/resource-lists+xml

<?xml version="1.0" encoding="UTF-8"?>
<resource-lists xmlns="urn:ietf:params:xml:ns:resource-lists">
  <list>
    <entry uri="org.openmobilealliance.groups/users/sip:+82571900001@ptt.csp"/>
  </list>
</resource-lists>
```

**응답:** 200 OK + 즉시 NOTIFY (현재 그룹 상태)

### 6.2 CMS 구독

```
SUBSCRIBE sip:cms_psi@ptt.csp SIP/2.0
Event: xcap-diff
Content-Type: application/resource-lists+xml

<?xml version="1.0" encoding="UTF-8"?>
<resource-lists xmlns="urn:ietf:params:xml:ns:resource-lists">
  <list>
    <entry uri="org.3gpp.mcptt.user-profile/users/sip:+82571900001@ptt.csp"/>
    <entry uri="org.3gpp.mcptt.service-config/users/sip:+82571900001@ptt.csp"/>
  </list>
</resource-lists>
```

### 6.3 NOTIFY 수신

그룹 설정 변경 시 CSP가 NOTIFY 전송:

```
NOTIFY sip:+82571900001@ue_ip SIP/2.0
Event: xcap-diff
Content-Type: application/xcap-diff+xml

<?xml version="1.0" encoding="UTF-8"?>
<xcap-diff xmlns="urn:ietf:params:xml:ns:xcap-diff" xcap-root="...">
  <document sel="org.openmobilealliance.groups/...">
    <!-- 변경된 그룹 정보 -->
  </document>
</xcap-diff>
```

단말은 NOTIFY에 200 OK 응답 필수.

---

## 7. 그룹콜 수신 (INVITE)

### 7.1 수신 INVITE 형식

CSP가 등록된 PTT 단말에게 그룹콜 참여 INVITE를 전송한다.

```
INVITE sip:+82571900001@ue_ip SIP/2.0
Content-Type: multipart/mixed; boundary=boundary1

--boundary1
Content-Type: application/vnd.3gpp.mcptt-info+xml

<?xml version="1.0" encoding="UTF-8"?>
<mcptt-info xmlns="urn:3gpp:ns:mcpttInfo:1.0">
  <mcptt-session-identity>sip:group_1000@ptt.csp</mcptt-session-identity>
  <session-type>prearranged</session-type>
  <mcptt-calling-user-identity>
    <mcptt-id>sip:+82571900002@ptt.csp</mcptt-id>
  </mcptt-calling-user-identity>
</mcptt-info>

--boundary1
Content-Type: application/sdp

v=0
o=- 1000 1000 IN IP4 192.168.1.10
s=PTT Session
c=IN IP4 192.168.1.10
t=0 0
m=audio 52000 RTP/AVP 99 0 101
a=rtpmap:99 AMR-WB/16000/1
a=fmtp:99 mode-change-capability=2; max-red=0
a=rtpmap:0 PCMU/8000
a=rtpmap:101 telephone-event/8000
a=fmtp:101 0-15
a=sendrecv
m=application 54000 UDP MCPTT
c=IN IP4 <CMP IP>
a=floorid:0 mstrm:audio
a=fmtp:MCPTT mc_queueing;mc_priority=3
a=mcptt-floor-request-uri:sip:<그룹>@<도메인>
--boundary1--
```

### 7.2 SDP 구성

| m= 라인 | 포트 | 용도 |
|---------|------|------|
| `m=audio` | CMP PTT RTP 포트 (52000~) | 음성 스트림 (PPttTrans._rtpSock) |
| `m=application` | CMP Floor 포트 (54000~) | Floor Control (PPttTrans._floorSock) |

> **중요:** `m=application` 포트는 CMP가 `addGroup` 시 할당한 전용 Floor 포트입니다.
> 단말은 Floor REQUEST/RELEASE를 반드시 이 포트로 전송해야 합니다.
> Audio RTP 포트+1이 아닌, SDP에 명시된 `m=application` 포트를 사용해야 합니다.

### 7.3 단말 응답

```
SIP/2.0 200 OK
Content-Type: application/sdp

v=0
o=- 2000 2000 IN IP4 192.168.1.100
s=PTT Session
c=IN IP4 192.168.1.100
t=0 0
m=audio 30000 RTP/AVP 99 0 101
a=rtpmap:99 AMR-WB/16000/1
a=rtpmap:0 PCMU/8000
a=rtpmap:101 telephone-event/8000
a=fmtp:101 0-15
a=sendrecv
m=application 30001 UDP MCPTT
a=floorid:0 mstrm:audio
a=fmtp:MCPTT mc_queueing
```

단말은 `m=audio`에 자신의 오디오 RTP 포트, `m=application`에 Floor Control 수신 포트를 기재한다.

**fmtp 협상**(TS 24.380 §12.1.2.3) — `a=fmtp:MCPTT` 파라미터가 floor 동작을 정한다.

| 파라미터 | 뜻 | CIMS 단말 |
|---|---|---|
| `mc_queueing` | 대기열(queueing) 지원. 미협상 멤버의 비선점 요청은 **Deny #1** 로 끊긴다 | **송신** — 발언 대기 순번을 표시하고, 버튼을 떼면 0x0E 로 취소 |
| `mc_priority=N` | 이 단말이 요청할 수 있는 **최대** 우선순위. 유효 우선순위는 협상값과 요청값 중 낮은 쪽 | **미송신** — Floor Priority 필드 자체를 안 실으므로, 제어평면이 준 멤버 우선순위가 그대로 유효 우선순위가 된다(협상하면 낮아지기만 한다) |
| `mc_granted` | 호 성립 시 초기 발언권 보유 | **미송신** — 채널 참여는 발언 요청이 아니다. 발언은 항상 PTT down 의 Floor Request 로 시작 |

---

## 8. Floor Control (발언권 제어)

### 8.1 프로토콜

RTCP APP 패킷 (PT=204, name="MCPT") 형식으로 `m=application` 소켓을 통해 교환.

### 8.2 패킷 구조

12바이트 고정 헤더 + floor control 필드들의 **TLV**(TS 24.380 §8.1~8.2). 메시지 타입은
헤더의 **subtype** 이 운반하며, 각 필드는 패딩을 포함해 4옥텟 배수라 모르는 필드는 건너뛴다.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P| subtype | PT=204 (APP) |          length               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|              SSRC (서버 발신은 floor control server SSRC)      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  'M'  |  'C'  |  'P'  |  'T'  |    (name = "MCPT")            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Field ID | Length |          value ...              (+패딩)    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### 8.3 메시지 타입 (subtype)

| subtype | 이름 | 방향 | 주요 필드 |
|--------|------|------|------|
| 0 | Floor Request | UE → CMP | Floor Priority(0)·User ID(6)·Indicator(13) |
| 1 | Floor Granted | CMP → UE | Duration(1)=최대 발언시간·SSRC(14)·Priority(0)·Indicator(13) |
| 2 | Floor Taken | CMP → 화자 외 | Granted Party(4)=MCPTT ID·Permission(5)·Msg Seq(8)·SSRC(14) |
| 3 | Floor Deny | CMP → UE | Reject Cause(2) — 1/3/5/7 |
| 4 | Floor Release | UE → CMP | User ID(6). `0x14` = ack 요구 변종 |
| 5 | Floor Idle | CMP → ALL | Msg Seq(8)·Indicator(13) |
| 6 | Floor Revoke | CMP → 화자 | Reject Cause(2) — 2(발언시간 초과)/4(선점) |
| 8 / 9 | Queue Position Request / Info | UE ↔ CMP | Queue Info(3) |
| 10 | Floor Ack | 양방향 | Source(10)·Message Type(12) |
| 0x0B / 0x0E / 0x0F | Media Flow Control / Queued Requests / Release Multi Talker | — | §8.6 |

subtype 의 첫 비트(0x10)는 **Ack 요구** 변종이다 — 받은 쪽은 Floor Ack 로 회신한다.

### 8.4 Floor 요청/획득 흐름

```
UE                          CMP (Floor 소켓)
 │                           │
 │ ── Floor Pkt ───────────► │  subtype=0 Floor Request
 │    (m=application 포트)    │  User ID [+ Priority · Indicator]
 │                           │  [서열 판정: tier > chair > priority]
 │ ◄── Floor Pkt ──────────── │  subtype=1 Floor Granted
 │    (내 m=application 포트)  │  Duration=허용 발언시간(초)
 │                           │
 │  [이제 음성 전송 가능]      │  → 다른 멤버에게 subtype=2 Floor Taken
 │ ── RTP Audio ───────────► │  (m=audio 포트로, 전체 멤버에 중계)
```

발언 중 **RTP 를 멈추면**(기본 4초) 서버가 발언 종료로 보고 회수하고, **Duration 을 넘겨**
말하면 Floor Revoke(cause 2) 후 짧은 유예 뒤 끊긴다. 단말은 Duration 잔여를 표시하고 마감
직전 스스로 Release 하므로, 정상 동작에서는 cause 2 회수까지 가지 않는다.

다른 사람이 말하는 중이라면 Deny 대신 **대기열**에 들어간다(`mc_queueing` 협상 전제) —
subtype=9 Floor Queue Position Info 로 순번을 받고, 앞이 비면 Floor Granted 로 승급한다.
버튼을 떼서 포기하면 subtype=0x0E(Queued Floor Requests, Purpose=Cancel Request)로 취소한다.
**Floor Release 로는 취소되지 않는다** — 서버는 발언 중이 아닌 leg 의 Release 를 무시한다.

### 8.5 Floor 해제 흐름

```
UE                          CMP
 │                           │
 │ ── Floor Pkt ───────────► │  subtype=4 Floor Release (또는 0x14)
 │                           │  (0x14 면 Floor Ack 회신)
 │ ◄── Floor Pkt ──────────── │  subtype=5 Floor Idle (Msg Seq + Indicator)
 │  [음성 전송 중지]           │  ※ 동시 발언 중 잔여 화자가 있으면
 │                           │     Idle 대신 0x0F(Release Multi Talker)
```

### 8.6 선점 (Preemption)

상위 서열(긴급/임박 > chair > 높은 priority) 요청이 오면 서버는 현재 화자에게 Revoke 를
보내고 **유예(기본 3초) 동안 기존 발언을 계속 중계**하며 Release 를 기다린다. 요청자는 그
사이 대기열 맨 앞에서 기다리다 회수가 끝나면 Granted 를 받는다.

```
현재 화자 UE-B                CMP                  요청자 UE-A (상위 서열)
 │                             │                    │
 │                             │ ◄── Floor Request ─│
 │ ◄── Floor Revoke(cause 4) ─│ ── Queue Pos Info ►│  (대기열 선두)
 │  [Release 로 응답 권장]      │  (유예 중 B 미디어 유지, 미응답 시 재전송)
 │ ── Floor Release ─────────►│                    │
 │                             │ ── Floor Granted ─►│
 │ ◄── Floor Taken(A) ────────│ ─ Taken ─► 화자 외  │
```

동급(같은 tier·priority) 요청은 선점하지 못하고 **대기열**에 들어가거나(SDP `mc_queueing`
협상 시) Floor Deny(cause 1)를 받는다. 참가자가 1명뿐이면 Deny(cause 3)다.

### 8.7 DTMF 기반 Floor (레거시)

RTCP APP를 지원하지 않는 단말은 DTMF 숫자로 대체 가능:

| 동작 | DTMF 숫자 | 설정 |
|------|-----------|------|
| Floor REQUEST | `*` (기본) | cmp.json → DtmfPushDigit |
| Floor RELEASE | `#` (기본) | cmp.json → DtmfReleaseDigit |

DTMF는 RTP 스트림(m=audio) 내 PT=101 (telephone-event)로 전송.
endBit=1인 최종 패킷에서만 동작 트리거.

---

## 9. Conference NOTIFY (참가자 변경 알림)

참가자 로스터는 **구독(RFC 4575 / RFC 6665)으로 받는다.** 채널 조인 시 그룹 AoR 로 구독을 걸고,
이탈 시 `Expires: 0` 으로 해지한다. 갱신은 같은 dialog 안에서 수행해야 한다(같은 Call-ID·양측
tag·CSeq+1) — 새 out-of-dialog SUBSCRIBE 를 보내면 구독이 중복 누적된다.

```
SUBSCRIBE sip:group_1000@ptt.csp SIP/2.0
Event: conference
Accept: application/conference-info+xml
Expires: 3600
```

CSP 는 200 OK 직후 현재 로스터 스냅샷을 1건 보내고, 이후 멤버 변동마다 통지한다. 단말은 각
NOTIFY 에 **200 OK** 로 응답한다.

> 구독을 구현하지 않은 단말에는 통화 dialog 로 같은 본문을 in-dialog NOTIFY 로 보내는 폴백이
> 동작한다(전환기 조치). 이 경우 단말 스택은 매칭 구독이 없어 500/481 을 응답하게 된다.

```
NOTIFY sip:+82571900001@ue_ip SIP/2.0
Event: conference
Subscription-State: active;expires=3600
Content-Type: application/conference-info+xml

<?xml version="1.0" encoding="UTF-8"?>
<conference-info xmlns="urn:ietf:params:xml:ns:conference-info"
  entity="sip:group_1000@ptt.csp"
  state="partial" version="3">
  <users>
    <user entity="tel:+82571900003" state="full">
      <endpoint entity="tel:+82571900003">
        <status>connected</status>
      </endpoint>
    </user>
  </users>
</conference-info>
```

| status 값 | 의미 |
|-----------|------|
| `connected` | 그룹콜 참여 중 |
| `disconnected` | 그룹콜 이탈 |
| `pending` | 초대 전송됨, 응답 대기 중 |

단말은 이 정보로 참가자 목록 UI를 갱신한다.

---

## 10. 그룹콜 퇴장

### 10.1 정상 퇴장

```
UE ── BYE ──► CSP ── 200 OK ──► UE
```

CSP가 CMP leaveGroup 처리 후 잔여 멤버에게 Conference NOTIFY 전송.

### 10.2 비정상 퇴장

네트워크 단절 시 CSP가 10초 주기로 등록 상태 확인 → 미등록 감지 시 자동 정리.

---

## 11. 오디오 스트림

### 11.1 코덱

| Payload Type | 코덱 | 샘플레이트 | 비고 |
|--------------|------|-----------|------|
| 99 | AMR-WB | 16000 | 광대역 (권장) |
| 0 | PCMU | 8000 | 기본 호환 |
| 101 | telephone-event | 8000 | DTMF PTT |

### 11.2 RTP 전송 규칙

- **Floor GRANT 수신 후**에만 오디오 RTP 전송
- **Floor IDLE/REVOKE 수신 시** 즉시 전송 중지
- 수신은 항상 가능 (현재 화자의 오디오)
- 수신 RTP의 SSRC/seq는 CMP가 수신자별로 재작성

### 11.3 미디어 경로

```
UE (화자)                     CMP                        UE (수신자들)
  │                            │                          │
  │ ── RTP(m=audio) ─────────► │                          │
  │    (PPttTrans._rtpSock)    │                          │
  │                            │ ── RTP(수신자별 SSRC) ──► │
  │                            │    (PPttTrans._rtpSock)   │
```

---

## 12. 단말 구현 요구사항

### 12.1 필수 프로토콜

| 프로토콜 | 규격 | 용도 |
|----------|------|------|
| HTTPS | TLS 1.2+ | IdMS/GMS/CMS 통신 |
| SIP/UDP | RFC 3261 | 등록, 통화 시그널링 |
| SDP | RFC 4566 | 미디어 협상 (m=audio + m=application) |
| RTP | RFC 3550 | 오디오 전송 |
| RTCP APP | RFC 3550 | Floor Control (PT=204, name="MCPT") |
| OAuth 2.0 PKCE | RFC 7636 | IdMS 인증 |
| XCAP | RFC 4825 | GMS/CMS 조회 |

### 12.2 필수 코덱

- AMR-WB (16kHz) 또는 PCMU (8kHz) 최소 1개

### 12.3 필수 기능

| 기능 | 설명 |
|------|------|
| Multipart INVITE 파싱 | mcptt-info+xml + SDP 분리 |
| m=application 처리 | Floor Control 전용 소켓 |
| Floor 상태 UI | GRANT/TAKEN/IDLE 상태 표시 |
| 참가자 목록 | Conference NOTIFY 기반 갱신 |
| GMS NOTIFY 처리 | 그룹 변경 실시간 반영 |
| 자동 200 OK | 그룹 INVITE 자동 수락 |

### 12.4 설정 파라미터

| 항목 | 예시 | 설명 |
|------|------|------|
| IdMS URL | https://server:4430 | IdMS 서버 주소 |
| SIP Server IP | 192.168.1.10 | CSP 서버 주소 |
| SIP Server Port | 5060 | SIP 포트 |
| MCPTT ID | +82571900001 | PTT 식별 번호 |
| Auth ID (IMPI) | +82571900001@ptt.csp | SIP 인증 ID |
| Password | secret123 | SIP 인증 비밀번호 |
| PTT Domain | ptt.csp | PTT SIP 도메인 |
| Client ID | mcptt-ue | OAuth 2.0 클라이언트 ID |

---

## 13. 참고 규격

| 규격 | 설명 |
|------|------|
| 3GPP TS 24.379 | MCPTT Call Control |
| 3GPP TS 24.380 | MCPTT Media Plane |
| 3GPP TS 24.481 | MCPTT Group Management |
| 3GPP TS 24.484 | MCPTT Configuration Management |
| 3GPP TS 33.180 | MCPTT Security |
| OMA PoC v2.1 | Push-to-Communicate over Cellular |
| RFC 3261 | SIP |
| RFC 3550 | RTP/RTCP |
| RFC 4575 | Conference Event Package (NOTIFY) |
| RFC 4825 | XCAP |
| RFC 7636 | OAuth 2.0 PKCE |
