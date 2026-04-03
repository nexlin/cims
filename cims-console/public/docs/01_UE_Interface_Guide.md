# CIMS UE 연동 인터페이스 규격 및 사용 설명서

## 1. 개요

CIMS는 3GPP MCPTT(Mission Critical Push-To-Talk) 규격 기반의 VoIP/PTT 서버 시스템입니다.
UE(단말)는 SIP 프로토콜로 CSP와 시그널링하고, RTP로 CMP와 미디어를 교환합니다.

본 문서는 **실제 MCPTT/VoIP 단말(UE)**이 CIMS 서버와 연동하기 위한 인터페이스 규격을 정의합니다.

```
┌──────────┐     SIP (UDP/TCP/TLS)        ┌──────────┐    UDP JSON (9000)     ┌──────────┐
│ VoIP UE  │◄────────────────────────────►│   CSP    │◄──────────────────────►│   CMP    │
│ (전화 앱) │     RTP/RTCP                 │ (SIP서버) │                        │ (미디어)  │
└──────────┘                              └────┬─────┘                        └──────────┘
                                               │
┌──────────┐     SIP (UDP/TCP/TLS)             │
│ MCPTT UE │◄──────────────────────────────────┘
│ (PTT 앱)  │     RTP/RTCP + RTCP APP (Floor Control)
│          │◄──────────────────────────────────────────────────────────────►│   CMP    │
└──────────┘                                                               └──────────┘

※ MCPTT 단말은 3GPP TS 24.379/380 규격에 따라 SIP 시그널링 및 RTCP Floor 제어를 수행합니다.
※ CSC(IdMS/GMS)와의 HTTPS 연동은 MCPTT 인증 및 그룹 관리에 사용됩니다.
```

### 1.1 참조 규격

| 규격 | 내용 |
|------|------|
| 3GPP TS 24.379 | MCPTT 호 제어 (SIP 기반 그룹/개인 통화) |
| 3GPP TS 24.380 | MCPTT 미디어 처리 (RTP, RTCP APP Floor 제어) |
| 3GPP TS 24.481 | MCPTT GMS (그룹 관리 서비스, OMA XDM 기반) |
| 3GPP TS 24.484 | MCPTT CMS (설정 관리 서비스, 사용자 프로파일) |
| 3GPP TS 33.180 | MCPTT 보안 (IdMS OAuth2 PKCE 인증, KMS 키관리) |
| RFC 3261 | SIP 기본 프로토콜 |
| RFC 3550 | RTP/RTCP 프로토콜 |
| RFC 4579 | SIP 컨퍼런스 (isfocus) |

---

## 2. VoLTE/SIP 단말 연동 (전화 UE App)

### 2.1 SIP 등록 (REGISTER)

단말은 CSP에 SIP REGISTER로 등록합니다. 최초 요청은 401 Unauthorized로 거부되며,
응답에 포함된 nonce를 사용하여 Digest 인증 헤더를 구성한 후 재전송합니다.

| 항목 | 값 |
|------|-----|
| 프로토콜 | SIP/2.0 (UDP 5060 / TCP 25061 / TLS 5061) |
| 인증 | Digest Authentication (MD5) |
| Realm | CSP 설정 파일의 realm 값 (예: `csp`) |
| Expires | 3600초 (기본) |

**1단계: 초기 REGISTER 요청 (인증 없이):**
```
REGISTER sip:csp SIP/2.0
Via: SIP/2.0/UDP 192.168.0.100:5060;branch=z9hG4bK-524287-1-0
Max-Forwards: 70
From: <sip:+821012345678@csp>;tag=abc123def456
To: <sip:+821012345678@csp>
Call-ID: 1-12345@192.168.0.100
CSeq: 1 REGISTER
Contact: <sip:+821012345678@192.168.0.100:5060;transport=udp>
Expires: 3600
User-Agent: CIMS-UE/1.0
Content-Length: 0
```

**401 Unauthorized 응답 (nonce 포함):**
```
SIP/2.0 401 Unauthorized
Via: SIP/2.0/UDP 192.168.0.100:5060;branch=z9hG4bK-524287-1-0
From: <sip:+821012345678@csp>;tag=abc123def456
To: <sip:+821012345678@csp>;tag=srv001
Call-ID: 1-12345@192.168.0.100
CSeq: 1 REGISTER
WWW-Authenticate: Digest realm="csp", nonce="7f4e2a8b3c1d5e9f", algorithm=MD5, qop="auth"
Content-Length: 0
```

**2단계: Digest 인증 포함 REGISTER 재전송:**
```
REGISTER sip:csp SIP/2.0
Via: SIP/2.0/UDP 192.168.0.100:5060;branch=z9hG4bK-524287-1-1
Max-Forwards: 70
From: <sip:+821012345678@csp>;tag=abc123def456
To: <sip:+821012345678@csp>
Call-ID: 1-12345@192.168.0.100
CSeq: 2 REGISTER
Contact: <sip:+821012345678@192.168.0.100:5060;transport=udp>
Expires: 3600
Authorization: Digest username="+821012345678", realm="csp", nonce="7f4e2a8b3c1d5e9f", uri="sip:csp", response="a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6", algorithm=MD5, qop=auth, nc=00000001, cnonce="0a1b2c3d"
User-Agent: CIMS-UE/1.0
Content-Length: 0
```

> **Digest response 계산:**
> ```
> HA1 = MD5(username:realm:password)
> HA2 = MD5(REGISTER:sip:csp)
> response = MD5(HA1:nonce:nc:cnonce:qop:HA2)
> ```

**200 OK 성공 응답:**
```
SIP/2.0 200 OK
Via: SIP/2.0/UDP 192.168.0.100:5060;branch=z9hG4bK-524287-1-1
From: <sip:+821012345678@csp>;tag=abc123def456
To: <sip:+821012345678@csp>;tag=srv002
Call-ID: 1-12345@192.168.0.100
CSeq: 2 REGISTER
Contact: <sip:+821012345678@192.168.0.100:5060;transport=udp>;expires=3600
Expires: 3600
Content-Length: 0
```

### 2.2 1:1 음성 통화 (VoIP Call)

**발신 흐름:**
```
UE-A                    CSP                     CMP                    UE-B
 │                       │                       │                       │
 │── INVITE (SDP) ──────►│                       │                       │
 │                       │── add (JSON/UDP) ────►│                       │
 │                       │◄── OK (port) ─────────│                       │
 │                       │── INVITE (SDP) ──────────────────────────────►│
 │◄── 100 Trying ────────│                       │                       │
 │                       │◄── 180 Ringing ───────────────────────────────│
 │◄── 180 Ringing ───────│                       │                       │
 │                       │◄── 200 OK (SDP) ──────────────────────────────│
 │◄── 200 OK (SDP) ──────│                       │                       │
 │── ACK ───────────────►│── ACK ───────────────────────────────────────►│
 │                       │                       │                       │
 │◄═══════════════════════════ RTP 양방향 ═══════════════════════════════►│
 │                       │                       │                       │
 │── BYE ───────────────►│── BYE ───────────────────────────────────────►│
 │◄── 200 OK ────────────│◄── 200 OK ────────────────────────────────────│
```

**INVITE 요청 (발신 UE-A):**
```
INVITE sip:+821098765432@csp SIP/2.0
Via: SIP/2.0/UDP 192.168.0.100:5060;branch=z9hG4bK-524287-2-0
Max-Forwards: 70
From: <sip:+821012345678@csp>;tag=call001
To: <sip:+821098765432@csp>
Call-ID: inv-20260331-001@192.168.0.100
CSeq: 1 INVITE
Contact: <sip:+821012345678@192.168.0.100:5060>
Content-Type: application/sdp
Content-Length: 245

v=0
o=UE-A 1234567890 1234567890 IN IP4 192.168.0.100
s=-
c=IN IP4 192.168.0.100
t=0 0
m=audio 40000 RTP/AVP 99 96 0 8
a=rtpmap:99 AMR-WB/16000/1
a=fmtp:99 mode-set=0,1,2; octet-align=1
a=rtpmap:96 opus/48000/2
a=fmtp:96 useinbandfec=1; minptime=20
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=ptime:20
a=sendrecv
```

**200 OK 응답 (착신 UE-B 수락 후 CSP가 CMP 포트 삽입):**
```
SIP/2.0 200 OK
Via: SIP/2.0/UDP 192.168.0.100:5060;branch=z9hG4bK-524287-2-0
From: <sip:+821012345678@csp>;tag=call001
To: <sip:+821098765432@csp>;tag=resp001
Call-ID: inv-20260331-001@192.168.0.100
CSeq: 1 INVITE
Contact: <sip:+821098765432@192.168.0.200:5060>
Content-Type: application/sdp
Content-Length: 198

v=0
o=csp 0 0 IN IP4 192.168.0.2
s=-
c=IN IP4 192.168.0.2
t=0 0
m=audio 50000 RTP/AVP 99
a=rtpmap:99 AMR-WB/16000/1
a=fmtp:99 mode-set=0,1,2; octet-align=1
a=ptime:20
a=sendrecv
```

**지원 코덱 목록:**

| Payload Type | 코덱 | 클럭 레이트 | 설명 |
|-------------|------|-----------|------|
| 99 | AMR-WB | 16000 | 광대역 음성 (기본) |
| 96 | opus | 48000 | 광대역 음성 코덱 |
| 0 | PCMU | 8000 | G.711 u-law |
| 8 | PCMA | 8000 | G.711 A-law |
| 97 | H264 | 90000 | 영상 (profile 42e01f) |
| 101 | telephone-event | 8000 | DTMF 이벤트 |

### 2.3 착신 거부 / 착신 전환

| 기능 | 설정 위치 | 동작 |
|------|----------|------|
| DND (방해금지) | 가입자 관리 콘솔 | 모든 착신 거부 (486 Busy) |
| 착신전환 | 가입자 관리 콘솔 `forward_id` | 지정 번호로 INVITE 전달 |
| 착신거부 목록 | 가입자 관리 콘솔 `reject_id` | 특정 발신번호 차단 |

---

## 3. PTT 단말 연동 (PTT UE App)

### 3.1 MCPTT 인증 흐름 (IdMS)

PTT 단말은 SIP 등록 전 IdMS(Identity Management Server)로 OAuth2 PKCE 인증을 수행합니다.

```
UE                          CSC (IdMS, 포트 4430)
 │                              │
 │── GET /idms/authreq ────────►│   (user_name, password, code_challenge, ...)
 │◄── {code} ───────────────────│
 │                              │
 │── POST /idms/tokenreq ─────►│   (code, code_verifier)
 │◄── {access_token, ...} ─────│
 │                              │
 │── GET /gms/users/{me} ─────►│   (Authorization: Bearer <token>)
 │◄── [그룹 목록] ──────────────│
```

**1단계: 인증 코드 요청 (GET /idms/authreq)**

전체 URL:
```
GET https://<서버IP>:4430/idms/authreq?response_type=code&client_id=MCPTT_UE&redirect_uri=https%3A%2F%2Flocalhost%2Fcallback&scope=openid%20mcptt&state=xyz123&code_challenge=E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM&code_challenge_method=S256&user_name=tel%3A%2B82571900001&user_password=123456
```

| 파라미터 | 값 | 설명 |
|---------|-----|------|
| `response_type` | `code` | 고정 |
| `client_id` | `MCPTT_UE` | 클라이언트 식별자 |
| `redirect_uri` | `https://localhost/callback` | 리다이렉트 URI |
| `scope` | `openid mcptt` | 요청 스코프 |
| `state` | `xyz123` | CSRF 방지 토큰 |
| `code_challenge` | `E9Melhoa2OwvFrE...` | SHA256(code_verifier)의 Base64URL |
| `code_challenge_method` | `S256` | 고정 |
| `user_name` | `tel:+82571900001` | 사용자 전화번호 (URL 인코딩) |
| `user_password` | `123456` | 사용자 비밀번호 |

**인증 코드 응답:**
```json
{
  "code": "SplxlOBeZQQYbYS6WxSbIA",
  "state": "xyz123"
}
```

**2단계: 토큰 교환 (POST /idms/tokenreq)**
```
POST https://<서버IP>:4430/idms/tokenreq
Content-Type: application/x-www-form-urlencoded

grant_type=authorization_code&code=SplxlOBeZQQYbYS6WxSbIA&client_id=MCPTT_UE&redirect_uri=https%3A%2F%2Flocalhost%2Fcallback&code_verifier=dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk
```

| 파라미터 | 값 | 설명 |
|---------|-----|------|
| `grant_type` | `authorization_code` | 고정 |
| `code` | `SplxlOBeZQQYbYS6WxSbIA` | 1단계에서 받은 코드 |
| `client_id` | `MCPTT_UE` | 클라이언트 식별자 |
| `redirect_uri` | `https://localhost/callback` | 1단계와 동일해야 함 |
| `code_verifier` | `dBjftJeZ4CVP-mB92...` | PKCE 원문 (43~128자 랜덤 문자열) |

**토큰 응답:**
```json
{
  "access_token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJ0ZWw6Kzgyc...",
  "token_type": "Bearer",
  "refresh_token": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "id_token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJjaW1zIiwi...",
  "expires_in": 3600,
  "scope": "openid mcptt"
}
```

> **PKCE 검증:** 서버는 `SHA256(code_verifier)`를 Base64URL 인코딩한 값과 `code_challenge`를 비교합니다.

### 3.2 그룹 정보 조회 (GMS)

**내 그룹 목록:**
```
GET https://<서버IP>:4430/org.openmobilealliance.groups/users/tel%3A%2B82571900001
Authorization: Bearer eyJhbGciOiJIUzI1NiIs...
Accept: application/json
```

**응답:**
```json
[
  {
    "uri": "tel:+82571910001",
    "display_name": "Alpha그룹",
    "etag": "etag_+82571910001",
    "member_count": 3
  },
  {
    "uri": "tel:+82571910002",
    "display_name": "Bravo그룹",
    "etag": "etag_+82571910002",
    "member_count": 5
  }
]
```

**그룹 상세 (OMA POC XML):**
```
GET https://<서버IP>:4430/org.openmobilealliance.groups/users/tel%3A%2B82571900001/tel%3A%2B82571910001
Accept: application/vnd.oma.poc.groups+xml
Authorization: Bearer eyJhbGciOiJIUzI1NiIs...
```

**응답:**
```xml
<?xml version="1.0" encoding="UTF-8"?>
<resource-lists xmlns="urn:ietf:params:xml:ns:resource-lists"
  xmlns:rl="urn:ietf:params:xml:ns:resource-lists"
  xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0"
  xmlns:pocgi="urn:oma:xml:poc:list-service">
  <list-service uri="tel:+82571910001">
    <display-name xml:lang="en-us">Alpha그룹</display-name>
    <list>
      <entry uri="tel:+82571900001">
        <rl:display-name>테스트001</rl:display-name>
        <mcpttgi:user-priority>0</mcpttgi:user-priority>
      </entry>
      <entry uri="tel:+821030432632">
        <rl:display-name>관리자</rl:display-name>
        <mcpttgi:user-priority>0</mcpttgi:user-priority>
      </entry>
      <entry uri="tel:+82571900002">
        <rl:display-name>테스트002</rl:display-name>
        <mcpttgi:user-priority>1</mcpttgi:user-priority>
      </entry>
    </list>
  </list-service>
</resource-lists>
```

### 3.3 PTT 그룹 통화 흐름

```
UE-A(발신)          CSP                CMP              UE-B(수신)        UE-C(수신)
  │                  │                  │                  │                 │
  │─ INVITE grp ────►│                  │                  │                 │
  │                  │─ addGroup ──────►│                  │                 │
  │                  │◄─ OK(port) ──────│                  │                 │
  │◄─ 200 OK(SDP) ──│                  │                  │                 │
  │                  │─ INVITE(multipart: SDP + XML) ─────►│                 │
  │                  │─ INVITE(multipart: SDP + XML) ──────────────────────►│
  │                  │◄─ 200 OK(SDP) ──────────────────────│                 │
  │                  │─ joinGroup(B) ──►│                  │                 │
  │                  │◄─ 200 OK(SDP) ───────────────────────────────────────│
  │                  │─ joinGroup(C) ──►│                  │                 │
  │                  │                  │                  │                 │
  │═══ RTCP FLOOR_REQUEST ════════════►│                  │                 │
  │◄══ RTCP FLOOR_GRANT ══════════════│                  │                 │
  │                  │                  │─ FLOOR_TAKEN ───►│                 │
  │                  │                  │─ FLOOR_TAKEN ────────────────────►│
  │═══ RTP Audio/Video ═══════════════►│                  │                 │
  │                  │                  │═══ RTP ═════════►│                 │
  │                  │                  │═══ RTP ══════════════════════════►│
```

**발신자 INVITE (그룹 호출):**
```
INVITE sip:+82571910001@csp SIP/2.0
Via: SIP/2.0/UDP 192.168.0.100:5060;branch=z9hG4bK-grp-001
From: <sip:+82571900001@csp>;tag=ptt001
To: <sip:+82571910001@csp>
Call-ID: grp-20260331-001@192.168.0.100
CSeq: 1 INVITE
Contact: <sip:+82571900001@192.168.0.100:5060>
Content-Type: application/sdp
Content-Length: 150

v=0
o=UE-A 0 0 IN IP4 192.168.0.100
s=-
c=IN IP4 192.168.0.100
t=0 0
m=audio 40000 RTP/AVP 99
a=rtpmap:99 AMR-WB/16000
a=sendrecv
```

**CSP가 멤버에게 보내는 Multipart INVITE (3GPP TS 24.379):**
```
INVITE sip:+82571900002@csp SIP/2.0
Via: SIP/2.0/UDP 192.168.0.2:5060;branch=z9hG4bK-srv-grp-001
From: <sip:+82571910001@csp>;tag=grpsrv001
To: <sip:+82571900002@csp>
Call-ID: grp-member-001@192.168.0.2
CSeq: 1 INVITE
Contact: <sip:+82571910001@192.168.0.2:5060>
Content-Type: multipart/mixed;boundary=mcptt
Content-Length: 680

--mcptt
Content-Type: application/vnd.3gpp.mcptt-info+xml

<?xml version="1.0" encoding="UTF-8"?>
<mcpttinfo xmlns="urn:3gpp:ns:mcpttInfo:1.0">
  <mcptt-Params>
    <session-type>prearranged</session-type>
    <mcptt-request-uri>tel:+82571910001</mcptt-request-uri>
    <mcptt-calling-user-id>tel:+82571900001</mcptt-calling-user-id>
    <mcptt-calling-group-id>tel:+82571910001</mcptt-calling-group-id>
  </mcptt-Params>
</mcpttinfo>

--mcptt
Content-Type: application/sdp

v=0
o=csp 0 0 IN IP4 192.168.0.2
s=-
c=IN IP4 192.168.0.2
t=0 0
m=audio 50000 RTP/AVP 99
a=rtpmap:99 AMR-WB/16000
a=sendrecv
m=application 50001 UDP MCPTT
a=floorid:0 mstrm:audio
a=fmtp:MCPTT mc_queueing;mc_priority=3
--mcptt--
```

> **SDP m=application 라인:** Floor control용 RTCP APP 포트는 Audio RTP 포트 + 1 (위 예시에서 50001)입니다. `a=floorid:0 mstrm:audio`는 Floor가 audio 미디어 스트림에 연결됨을 표시합니다.

### 3.4 Floor Control (발언권 제어)

RTCP APP 패킷 (포트: RTP포트 + 1)으로 제어합니다.

**패킷 구조 (총 20+ 바이트):**
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|  subtype |   PT=204    |           length              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          SSRC                                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   name = "MCPT" (0x4D435054)                                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  opcode  | id_len |       reserved (0x0000)                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|            speaker_id (가변 길이, 4바이트 정렬 패딩)            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**필드 설명:**

| 필드 | 크기 | 설명 |
|------|------|------|
| `V` | 2 bits | RTP 버전 = 2 |
| `P` | 1 bit | 패딩 = 0 |
| `subtype` | 5 bits | 0 |
| `PT` | 8 bits | 204 (RTCP APP 고정) |
| `length` | 16 bits | 전체 32비트 워드 수 - 1 (네트워크 바이트 순서) |
| `SSRC` | 32 bits | 송신자의 SSRC (CMP가 joinGroup 시 할당, 1000번부터 순차) |
| `name` | 4 bytes | ASCII `"MCPT"` (0x4D 0x43 0x50 0x54) |
| `opcode` | 8 bits | Floor 동작 코드 (아래 표 참조) |
| `id_len` | 8 bits | speaker_id 문자열의 바이트 길이 (0이면 없음) |
| `reserved` | 16 bits | 예약 (0x0000) |
| `speaker_id` | 가변 | 화자 session ID 문자열 (UTF-8, 4바이트 정렬 패딩) |

**실제 패킷 Hex 예시 (FLOOR_REQUEST, SSRC=1001, speaker_id="+82571900001"):**
```
80 CC 00 07    # V=2, P=0, subtype=0, PT=204, length=7 (8 words = 32 bytes)
00 00 03 E9    # SSRC = 1001
4D 43 50 54    # name = "MCPT"
01 0E 00 00    # opcode=1(REQUEST), id_len=14, reserved=0
2B 38 32 35    # "+825"
37 31 39 30    # "7190"
30 30 30 31    # "0001"
00 00 00 00    # padding (4바이트 정렬)
```

**Opcode 정의 (7종):**

| Opcode | 이름 | 방향 | SSRC 용도 | speaker_id | 설명 |
|--------|------|------|-----------|------------|------|
| 1 | FLOOR_REQUEST | UE→CMP | 요청자 SSRC | 요청자 session ID | PTT 버튼 누름 (발언권 요청) |
| 2 | FLOOR_GRANT | CMP→요청자 | 승인된 SSRC | 승인된 session ID | 발언권 승인 (요청자에게만 전송) |
| 3 | FLOOR_REJECT | CMP→요청자 | 거부된 SSRC | 거부된 session ID | 발언권 거부 (우선순위 낮음) |
| 4 | FLOOR_RELEASE | UE→CMP | 해제자 SSRC | 해제자 session ID | PTT 버튼 해제 (발언권 반납) |
| 5 | FLOOR_IDLE | CMP→ALL | 0 | 빈 문자열 | 발언권 해제됨 (모든 멤버에게 브로드캐스트) |
| 6 | FLOOR_TAKEN | CMP→ALL | 화자 SSRC | 화자 session ID | 발언권 점유됨 (화자 정보 포함, 모든 멤버에게) |
| 7 | FLOOR_REVOKE | CMP→현재 화자 | 현재 화자 SSRC | 현재 화자 session ID | 강제 발언권 회수 (우선순위 선점 시) |

**우선순위 선점 규칙:**
- 우선순위 값이 **낮을수록** 높은 우선순위 (0 = 최고)
- 동일 우선순위는 선점 불가 → FLOOR_REJECT
- 현재 화자보다 높은 우선순위 요청 시: 현재 화자에게 REVOKE → 요청자에게 GRANT → 전체에게 TAKEN
- 화자가 그룹에서 나가면 자동으로 FLOOR_IDLE 브로드캐스트

**Floor 상태 머신:**
```
                    REQUEST (floor free)
        ┌─────────────────────────────────────┐
        │                                     ▼
    ┌───────┐   RELEASE / Owner Left    ┌──────────┐
    │ IDLE  │◄──────────────────────────│  TAKEN   │
    └───────┘                           └──────────┘
        │                                     ▲
        │         REQUEST (preempt)           │
        │    ┌──────────────────────────┐     │
        │    │  REVOKE(old) → GRANT(new)│─────┘
        │    └──────────────────────────┘
        │                                ┌──────────┐
        └── REQUEST (floor busy, low) ──►│ REJECT   │
                                         └──────────┘
```

### 3.5 SIP SUBSCRIBE/NOTIFY (그룹 상태 구독)

```
UE                          CSP
 │                           │
 │── SUBSCRIBE ─────────────►│  Event: gms (또는 cms)
 │◄── 200 OK ────────────────│
 │◄── NOTIFY ────────────────│  Body: xcap-diff XML (그룹 설정 변경)
 │── 200 OK ────────────────►│
```

**SUBSCRIBE 요청 예시:**
```
SUBSCRIBE sip:+82571900001@csp SIP/2.0
Via: SIP/2.0/UDP 192.168.0.100:5060;branch=z9hG4bK-sub-001
From: <sip:+82571900001@csp>;tag=sub001
To: <sip:+82571900001@csp>
Call-ID: sub-gms-001@192.168.0.100
CSeq: 1 SUBSCRIBE
Contact: <sip:+82571900001@192.168.0.100:5060>
Event: gms
Expires: 3600
Accept: application/xcap-diff+xml
Content-Length: 0
```

**NOTIFY 응답 (xcap-diff XML):**
```
NOTIFY sip:+82571900001@192.168.0.100:5060 SIP/2.0
Via: SIP/2.0/UDP 192.168.0.2:5060;branch=z9hG4bK-ntf-001
From: <sip:+82571900001@csp>;tag=sub001srv
To: <sip:+82571900001@csp>;tag=sub001
Call-ID: sub-gms-001@192.168.0.100
CSeq: 1 NOTIFY
Subscription-State: active;expires=3600
Event: gms
Content-Type: application/xcap-diff+xml
Content-Length: 350

<?xml version="1.0" encoding="UTF-8"?>
<xcap-diff xmlns="urn:ietf:params:xml:ns:xcap-diff"
           xcap-root="https://csp:4430/org.openmobilealliance.groups/">
  <document doc-selector="users/tel%3A%2B82571900001"
            new-etag="etag_20260331_001"
            previous-etag="">
  </document>
</xcap-diff>
```

---

## 4. MCPTT UE 단말의 서비스 연동

MCPTT 단말(UE)은 통화 이전에 아래 서비스에 연동하여 인증, 그룹 정보, 사용자 프로파일을 취득합니다.
이 절차는 3GPP TS 33.180(IdMS), TS 24.481(GMS), TS 24.484(CMS)에 정의되어 있습니다.

### 4.1 전체 부팅 흐름

```
MCPTT UE                 CSC (IdMS/GMS/CMS)                 CSP
  │                           │                               │
  │ ① IdMS 인증 (OAuth2 PKCE)  │                               │
  │── GET /idms/authreq ─────►│                               │
  │◄── {code} ────────────────│                               │
  │── POST /idms/tokenreq ──►│                               │
  │◄── {access_token} ───────│                               │
  │                           │                               │
  │ ② GMS 그룹 조회             │                               │
  │── GET /gms/users/{me} ──►│                               │
  │◄── [그룹 목록] ─────────── │                               │
  │── GET /gms/.../grp_uri ──►│                               │
  │◄── 그룹 XML (멤버 정보) ────│                               │
  │                           │                               │
  │ ③ CMS 프로파일 조회          │                               │
  │── GET /cms/user-profile ─►│                               │
  │◄── 사용자 프로파일 XML ──────│                               │
  │                           │                               │
  │ ④ SIP 등록                  │                               │
  │── SIP REGISTER ───────────────────────────────────────────►│
  │◄── 200 OK ─────────────────────────────────────────────────│
  │                           │                               │
  │ ⑤ SIP SUBSCRIBE (그룹 상태) │                               │
  │── SUBSCRIBE Event:gms ────────────────────────────────────►│
  │◄── 200 OK ─────────────────────────────────────────────────│
  │◄── NOTIFY (그룹 변경) ──────────────────────────────────────│
```

### 4.2 IdMS 인증 (3GPP TS 33.180 — OAuth2 PKCE)

MCPTT 단말은 서비스 이용 전 IdMS에서 액세스 토큰을 발급받아야 합니다.

**① Authorization Request:**
```
GET https://<CSC>:4430/idms/authreq
    ?user_name=tel%3A%2B82571900001        ← MCPTT ID (tel URI, URL 인코딩)
    &user_password=123456                  ← 사용자 비밀번호
    &client_id=MCPTT_UE                    ← 클라이언트 식별자 (고정)
    &redirect_uri=https%3A%2F%2F<UE>/callback  ← 리다이렉트 URI
    &code_challenge=rPzO_zKms...           ← Base64URL(SHA256(code_verifier))
    &code_challenge_method=S256            ← PKCE 방식 (고정)
    &scope=openid+mcptt                    ← 요청 범위
    &state=omoD62dmYRyUhjp3                ← CSRF 방지 상태값

응답 (200):
{
  "Location": "https://<UE>/callback",
  "code": "3ce2dae6-5da9-4126-b587-6135d3937193",
  "state": "omoD62dmYRyUhjp3"
}
```

**② Token Request:**
```
POST https://<CSC>:4430/idms/tokenreq
Content-Type: application/x-www-form-urlencoded

grant_type=authorization_code
&code=3ce2dae6-5da9-4126-b587-6135d3937193
&redirect_uri=https://<UE>/callback
&client_id=MCPTT_UE
&code_verifier=test123456789012345678901234567890123456789012

응답 (200):
{
  "access_token": "eyJhbGciOiJIUzI1NiIs...",   ← GMS/CMS 호출에 사용
  "refresh_token": "a1b2c3d4-e5f6-...",          ← 토큰 갱신용
  "id_token": "eyJ...",                          ← MCPTT ID 포함 JWT
  "expires_in": 3600                             ← 유효 시간 (초)
}
```

> **토큰 유효 기간:** auth_code=60초, access_token=1시간, refresh_token=7일

### 4.3 GMS 그룹 조회 (3GPP TS 24.481)

**내 그룹 목록 조회:**
```
GET https://<CSC>:4430/org.openmobilealliance.groups/users/tel%3A%2B82571900001
Authorization: Bearer <access_token>

응답 (200):
[
  {
    "uri": "tel:+82571910001",
    "display_name": "Alpha그룹",
    "etag": "etag_+82571910001",
    "member_count": 3
  }
]
```

**그룹 상세 조회 (OMA POC XML):**
```
GET https://<CSC>:4430/org.openmobilealliance.groups/users/tel%3A%2B82571900001/tel%3A%2B82571910001
Accept: application/vnd.oma.poc.groups+xml
Authorization: Bearer <access_token>

응답:
<?xml version="1.0" encoding="UTF-8"?>
<resource-lists xmlns="urn:ietf:params:xml:ns:resource-lists"
  xmlns:rl="urn:ietf:params:xml:ns:resource-lists"
  xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0">
  <list-service uri="tel:+82571910001">
    <display-name xml:lang="en-us">Alpha그룹</display-name>
    <list>
      <entry uri="tel:+82571900001">
        <rl:display-name>테스트001</rl:display-name>
        <mcpttgi:on-network-required/>
        <mcpttgi:user-priority>0</mcpttgi:user-priority>
      </entry>
      <entry uri="tel:+821030432632">
        <rl:display-name>관리자</rl:display-name>
        <mcpttgi:user-priority>0</mcpttgi:user-priority>
      </entry>
      <entry uri="tel:+82571900002">
        <rl:display-name>테스트002</rl:display-name>
        <mcpttgi:user-priority>1</mcpttgi:user-priority>
      </entry>
    </list>
    <mcpttgi:mcptt-video>true</mcpttgi:mcptt-video>
  </list-service>
</resource-lists>
```

### 4.4 CMS 사용자 프로파일 (3GPP TS 24.484)

```
GET https://<CSC>:4430/org.3gpp.mcptt.user-profile/users/tel%3A%2B82571900001
Authorization: Bearer <access_token>

응답:
<?xml version="1.0" encoding="UTF-8"?>
<mcptt-user-profile xmlns="urn:3gpp:ns:mcpttUserProfile:1.0"
  user-profile-index="1">
  <Name>
    <display-name xml:lang="en">테스트001</display-name>
  </Name>
  <Common>
    <MCPTTUserID>tel:+82571900001</MCPTTUserID>
    <PrivateCallList>
      <PrivateCallEntry>
        <UserURI>tel:+82571900002</UserURI>
      </PrivateCallEntry>
    </PrivateCallList>
  </Common>
</mcptt-user-profile>
```

### 4.5 SIP SUBSCRIBE/NOTIFY (그룹 상태 구독)

단말은 SIP SUBSCRIBE로 그룹 상태 변경을 구독합니다.

**SUBSCRIBE 요청:**
```
SUBSCRIBE sip:+82571910001@csp SIP/2.0
From: <sip:+82571900001@csp>;tag=sub001
To: <sip:+82571910001@csp>
Call-ID: sub-001@192.168.0.100
CSeq: 1 SUBSCRIBE
Event: gms
Accept: application/xcap-diff+xml
Expires: 3600
Contact: <sip:+82571900001@192.168.0.100:5060>
Content-Length: 0
```

**200 OK 응답 후 NOTIFY:**
```
NOTIFY sip:+82571900001@192.168.0.100:5060 SIP/2.0
From: <sip:+82571910001@csp>;tag=srv001
To: <sip:+82571900001@csp>;tag=sub001
Event: gms
Subscription-State: active;expires=3600
Content-Type: application/xcap-diff+xml

<?xml version="1.0" encoding="UTF-8"?>
<xcap-diff xmlns="urn:ietf:params:xml:ns:xcap-diff"
  xcap-root="https://csc:4430/org.openmobilealliance.groups/">
  <document doc-selector="users/tel:+82571910001"
    new-etag="etag_20260331_001" />
</xcap-diff>
```

> 단말은 NOTIFY의 `new-etag`과 로컬 캐시의 etag을 비교하여 변경된 그룹 정보만 GMS에서 다시 조회합니다.

---

## 5. 단말 구현 요구사항 요약

### 5.1 필수 프로토콜

| 인터페이스 | 프로토콜 | 포트 | 용도 |
|-----------|---------|------|------|
| CSP ↔ UE | SIP/UDP (RFC 3261) | 5060 | 등록, 통화, 구독 |
| CSP ↔ UE | SIP/TLS | 5061 | 암호화 시그널링 |
| CMP ↔ UE | RTP/RTCP (RFC 3550) | 동적 할당 | 오디오/비디오 미디어 |
| CMP ↔ UE | RTCP APP (3GPP TS 24.380) | RTP+1 | Floor 제어 |
| CSC ↔ UE | HTTPS | 4430 | IdMS/GMS/CMS/KMS |

### 5.2 필수 코덱

| 코덱 | PT | Rate | 용도 | 비고 |
|------|-----|------|------|------|
| AMR-WB | 99 | 16000 | 음성 (필수) | MCPTT 기본 코덱 |
| opus | 111 | 48000 | 음성 (선택) | 광대역 |
| H264 | 97 | 90000 | 영상 | profile 42e01f |
| telephone-event | 101 | 8000 | DTMF | RFC 4733 |

### 5.3 설정 파일 참조

| 파일 | 용도 |
|------|------|
| `csp/csp.json` | CSP IP/포트, realm, RTP relay 주소, 로그 설정 |
| `cmp/cmp.json` | CMP IP, 제어 포트, RTP 포트 풀, DTMF PTT 설정 |
| `csc/config/csc.json` | CSC API 포트, DB 연결, JWT 설정, MCPTT 서버 설정 |
