# 08. VoLTE 단말 연동 인터페이스 규격

**작성일:** 2026-04-13

---

## 1. 개요

본 문서는 VoLTE/SIP 단말이 CIMS CSP와 연동하기 위한 프로토콜 규격을 정의한다.

### 1.1 대상

- VoLTE SIP 단말 (소프트폰, IP Phone, VoIP 앱)
- 3rd party SIP UA 개발자

### 1.2 연동 포인트

| 인터페이스 | 프로토콜 | 포트 | 용도 |
|------------|----------|------|------|
| SIP 시그널링 | SIP/UDP | 5060 | 등록, 통화 제어 |
| SIP 시그널링 | SIP/TCP | 25061 | 등록, 통화 제어 (TCP) |
| SIP 시그널링 | SIP/TLS | 5061 | 등록, 통화 제어 (보안) |
| 미디어 (오디오) | RTP/UDP | 동적 (SDP) | 음성 스트림 |
| 미디어 (영상) | RTP/UDP | 동적 (SDP) | 영상 스트림 |
| 미디어 제어 | RTCP/UDP | RTP+1 | RTP 통계/제어 |

---

## 2. SIP 등록 (REGISTER)

### 2.1 인증 방식

Digest MD5 (RFC 2617)

### 2.2 등록 흐름

```
UE                              CSP (5060)
 │                               │
 │ ── REGISTER ────────────────► │
 │    From: <sip:1001@csp>       │
 │    To: <sip:1001@csp>         │
 │    Contact: <sip:1001@ue_ip>  │
 │    Expires: 600               │
 │                               │
 │ ◄── 401 Unauthorized ──────── │
 │    WWW-Authenticate:          │
 │      Digest realm="csp",      │
 │      nonce="abcdef...",       │
 │      qop="auth",             │
 │      algorithm=MD5            │
 │                               │
 │ ── REGISTER ────────────────► │
 │    Authorization:             │
 │      Digest username="1001",  │
 │      realm="csp",            │
 │      nonce="abcdef...",       │
 │      uri="sip:csp",          │
 │      nc=00000001,            │
 │      cnonce="xyz...",         │
 │      qop=auth,               │
 │      response="md5hash..."    │
 │                               │
 │ ◄── 200 OK ──────────────── │
 │    Contact: <sip:1001@ue_ip>  │
 │    Expires: 600               │
```

### 2.3 인증 계산

```
A1 = MD5(username:realm:password)
A2 = MD5(REGISTER:sip:realm)
response = MD5(A1:nonce:nc:cnonce:qop:A2)
```

### 2.4 등록 파라미터

| 항목 | 값 | 비고 |
|------|---|------|
| username | 구독 ID (예: "1001") | voip_subscriptions.auth_id |
| realm | CSP 도메인 (예: "csp") | csp.json → Setup.Sip.Realm |
| password | SIP 인증 비밀번호 | voip_subscriptions.passwd |
| Expires | 600 (권장) | 초 단위, 0이면 등록 해제 |

### 2.5 등록 해제

```
REGISTER sip:csp SIP/2.0
Expires: 0
```

---

## 3. 1:1 음성 통화

### 3.1 발신 (INVITE)

```
UE-A                     CSP                         UE-B
 │                        │                           │
 │ ── INVITE ───────────► │                           │
 │    To: <sip:1002@csp>  │                           │
 │    Content-Type: application/sdp                    │
 │    (SDP offer)         │                           │
 │                        │                           │
 │ ◄── 100 Trying ──────── │                           │
 │                        │ ── INVITE ──────────────► │
 │                        │                           │
 │                        │ ◄── 180 Ringing ────────── │
 │ ◄── 180 Ringing ─────── │                           │
 │                        │                           │
 │                        │ ◄── 200 OK (SDP answer) ── │
 │ ◄── 200 OK ──────────── │                           │
 │                        │                           │
 │ ── ACK ────────────────► │ ── ACK ──────────────► │
 │                        │                           │
 │ ◄═══ RTP 양방향 (CMP relay 경유) ═══════════════► │
 │                        │                           │
 │ ── BYE ────────────────► │ ── BYE ──────────────► │
 │ ◄── 200 OK ──────────── │ ◄── 200 OK ──────────── │
```

### 3.2 SDP 규격

**Offer 예시:**

```
v=0
o=- 12345 12345 IN IP4 192.168.1.100
s=-
c=IN IP4 192.168.1.100
t=0 0
m=audio 30000 RTP/AVP 99 98 0 8 101
a=rtpmap:99 AMR-WB/16000/1
a=fmtp:99 mode-change-capability=2; max-red=0
a=rtpmap:98 AMR/8000/1
a=fmtp:98 mode-change-capability=2; max-red=0
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=rtpmap:101 telephone-event/8000
a=fmtp:101 0-15
a=sendrecv
m=video 30002 RTP/AVP 96
a=rtpmap:96 H264/90000
a=fmtp:96 profile-level-id=42e01f; packetization-mode=1
a=sendrecv
```

### 3.3 지원 코덱

| Payload Type | 코덱 | 샘플레이트 | 비고 |
|--------------|------|-----------|------|
| 99 | AMR-WB | 16000 | 광대역 음성 (권장) |
| 98 | AMR-NB | 8000 | 협대역 음성 |
| 0 | PCMU (G.711 u-law) | 8000 | 기본 |
| 8 | PCMA (G.711 A-law) | 8000 | 기본 |
| 101 | telephone-event | 8000 | DTMF (RFC 4733) |
| 96 | H.264 | 90000 | 영상 (선택) |

### 3.4 RTP 전송

- **방향:** 양방향 (sendrecv)
- **경로:** CMP relay 경유 (CSP가 SDP의 IP:port를 CMP relay 주소로 교체)
- **Symmetric RTP:** CMP가 최초 패킷 수신 시 IP 학습 (NAT 대응)
- **RTCP:** RTP 포트 + 1

### 3.5 DTMF

- RFC 4733 (telephone-event, PT=101)
- 이벤트: 0-9, *, #, A-D
- end bit 설정 필수

---

## 4. 부가서비스

### 4.1 DND (착신거부)

DND가 활성화된 사용자에게 발신 시:

```
UE-A ── INVITE B ──► CSP ── 603 Decline ──► UE-A
```

단말 측 처리 불필요. CSP가 자동 거부.

### 4.2 착신전환

착신전환이 설정된 사용자에게 발신 시:

```
UE-A ── INVITE B ──► CSP ── 302 Moved Temporarily ──► UE-A
                              Contact: <sip:C@csp>

UE-A ── INVITE C ──► CSP ── (정상 통화 흐름)
```

단말은 302 응답의 Contact 헤더로 재발신해야 한다.

### 4.3 개별 수신거부

특정 발신자가 수신거부 목록에 포함된 경우:

```
UE-A(차단됨) ── INVITE B ──► CSP ── 603 Decline ──► UE-A
```

### 4.4 콜픽업

그룹 내 활성 호를 대리 수신 (구현 시):

```
UE-C ── INVITE pickup@csp ──► CSP ── (그룹 내 활성 호 검색 → 연결)
```

---

## 5. 호 종료

### 5.1 정상 종료 (BYE)

어느 쪽이든 BYE 전송 가능:

```
UE ── BYE ──► CSP ── 200 OK ──► UE
```

### 5.2 발신 취소 (CANCEL)

Ringing 중 발신자 취소:

```
UE-A ── CANCEL ──► CSP
UE-A ◄── 200 OK ── CSP
UE-A ◄── 487 Request Terminated ── CSP
```

### 5.3 응답 코드

| SIP 코드 | 의미 | 단말 처리 |
|----------|------|----------|
| 100 | Trying | 대기 |
| 180 | Ringing | 통화 연결음 재생 |
| 200 | OK | 통화 시작, ACK 전송 |
| 302 | Moved | Contact로 재발신 |
| 403 | Forbidden | 인증 실패 |
| 404 | Not Found | 상대방 미등록 |
| 486 | Busy | 상대방 통화 중 |
| 487 | Request Terminated | CANCEL 처리됨 |
| 603 | Decline | 수신 거부 (DND/개별) |

---

## 6. 영상 통화

### 6.1 SDP 구성

음성 + 영상 m= 라인을 포함한 SDP offer:

```
m=audio 30000 RTP/AVP 99 0 101
(오디오 코덱)
m=video 30002 RTP/AVP 96
a=rtpmap:96 H264/90000
a=fmtp:96 profile-level-id=42e01f; packetization-mode=1
a=sendrecv
```

### 6.2 코덱 요구사항

- **H.264 Baseline Profile** (profile-level-id=42e0xx)
- **Packetization-mode=1** (Non-interleaved, FU-A)
- **해상도:** 320x240 ~ 1280x720 (협상)

### 6.3 미디어 포트

- Audio RTP: SDP m=audio 포트
- Audio RTCP: Audio RTP + 1
- Video RTP: SDP m=video 포트
- Video RTCP: Video RTP + 1

---

## 7. 단말 구현 요구사항

### 7.1 필수 프로토콜

| 프로토콜 | 규격 | 용도 |
|----------|------|------|
| SIP/UDP | RFC 3261 | 시그널링 |
| SDP | RFC 4566 | 미디어 협상 |
| RTP/RTCP | RFC 3550 | 미디어 전송 |
| Digest MD5 | RFC 2617 | SIP 인증 |
| DTMF | RFC 4733 | DTMF 전송 |

### 7.2 필수 코덱 (최소 1개)

- AMR-WB (16kHz, 권장)
- PCMU 또는 PCMA (8kHz, 기본)

### 7.3 선택 코덱

- AMR-NB (8kHz)
- H.264 Baseline (영상)

### 7.4 SIP 헤더 요구사항

| 헤더 | 필수 | 설명 |
|------|------|------|
| From | O | 발신자 URI + tag |
| To | O | 수신자 URI |
| Call-ID | O | 고유 호 식별자 |
| CSeq | O | 시퀀스 번호 |
| Via | O | 트랜잭션 라우팅 |
| Contact | O | 직접 연결 가능 URI |
| Max-Forwards | O | 70 (기본) |
| Content-Type | O (INVITE) | application/sdp |
| Expires | O (REGISTER) | 등록 유효 시간 |
| Authorization | O (인증) | Digest 인증 정보 |

### 7.5 설정 파라미터

단말에 설정해야 하는 항목:

| 항목 | 예시 | 설명 |
|------|------|------|
| SIP Server IP | 192.168.1.10 | CSP 서버 주소 |
| SIP Server Port | 5060 | SIP 포트 |
| Transport | UDP / TCP / TLS | 전송 프로토콜 |
| Username | 1001 | SIP 인증 ID (auth_id) |
| Password | 1234 | SIP 인증 비밀번호 |
| Domain/Realm | csp | SIP 도메인 |
| Display Name | 홍길동 | 표시 이름 (선택) |

---

## 8. 참고 규격

| 규격 | 설명 |
|------|------|
| RFC 3261 | SIP: Session Initiation Protocol |
| RFC 2617 | HTTP Digest Authentication |
| RFC 4566 | SDP: Session Description Protocol |
| RFC 3550 | RTP/RTCP |
| RFC 4733 | DTMF (telephone-event) |
| RFC 3264 | Offer/Answer Model with SDP |
