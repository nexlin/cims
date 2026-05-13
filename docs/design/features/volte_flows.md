# VoLTE(VoIP) 서비스 케이스 및 메시지 Flow

**작성일:** 2026-04-03
**최종 수정:** 2026-04-13 (CMP VoIP/PTT 핸들러 분리 반영)

---

## 케이스 목록

### Part A. 운용 설정

| # | 케이스 | 설명 |
|---|--------|------|
| A1 | VoIP 가입자 생성 및 구독 추가 | 사용자 생성 → VoIP 번호 할당 |
| A2 | DND (방해금지) 설정 | 특정 사용자의 수신 거부 |
| A3 | 착신전환 설정 | 다른 번호로 착신 전환 |
| A4 | 개별 수신거부 설정 | 특정 발신자 수신 거부 |
| A5 | 가입자/구독 삭제 | 구독 해제 → 사용자 삭제 |

### Part B. 단말 등록

| # | 케이스 | 설명 |
|---|--------|------|
| B1 | SIP 등록 성공 | REGISTER → Digest 인증 → 200 OK |
| B2 | SIP 등록 실패 (인증) | 잘못된 비밀번호 → 403 |
| B3 | 등록 해제 | Expires=0 또는 타임아웃 |

### Part C. 서비스 중 (통화)

| # | 케이스 | 설명 |
|---|--------|------|
| C1 | 기본 1:1 통화 (Proxy) | A→B 발신, 응답, 통화, 종료 |
| C2 | DND 거부 | A→B, B가 DND → 603 Decline |
| C3 | 개별 수신거부 | A→B, B가 A를 거부 → 603 Decline |
| C4 | 착신전환 | A→B, B가 C로 전환 → 302 → A→C |
| C5 | 부재 (미등록) | A→B, B 미등록 → 404 Not Found |
| C6 | 발신자 취소 | A→B, A가 CANCEL → 487 |
| C7 | 수신자 거절 | A→B, B가 BYE/603 → 종료 |
| C8 | RTP 릴레이 통화 | CMP 경유 미디어 중계 |

### Part D. 서비스 중 운용 변경

| # | 케이스 | 설명 |
|---|--------|------|
| D1 | 통화 중 DND 설정 | 이후 새 착신만 거부, 기존 통화 유지 |
| D2 | 통화 중 착신전환 설정 | 이후 새 착신만 전환, 기존 통화 유지 |

---

## Part A. 운용 설정

### A1. VoIP 가입자 생성 및 구독 추가

```
Console            CSC                 CSP
  │                 │                   │
  │ POST /users     │                   │
  │ {name,login_id} │                   │
  │ ──────────────► │ [DB] users INSERT │
  │ ◄── 201 ─────── │                   │
  │                 │                   │
  │ POST /users/    │                   │
  │  {pid}/call     │                   │
  │ {id: MSISDN,    │                   │
  │  auth_id, passwd}│                  │
  │ ──────────────► │ [DB] voip_sub INSERT
  │                 │ ── UDP ─────────► │ event: user_change
  │ ◄── 201 ─────── │                   │ action: POST
  │                 │                   │ [CspUserMap 캐시 갱신]
```

### A2. DND (방해금지) 설정

```
Console            CSC                 CSP
  │ PUT /users/     │                   │
  │  {pid}/call/    │                   │
  │  {msisdn}       │                   │
  │ {dnd: true}     │                   │
  │ ──────────────► │ [DB] UPDATE dnd=1 │
  │                 │ ── UDP ─────────► │ user_change (PUT)
  │ ◄── 200 ─────── │                   │ [CspUser.m_bDnd = true]
```

### A3. 착신전환 설정

```
Console            CSC                 CSP
  │ PUT /users/     │                   │
  │  {pid}/call/    │                   │
  │  {msisdn}       │                   │
  │ {forward_id:    │                   │
  │  "+821000"}     │                   │
  │ ──────────────► │ [DB] UPDATE       │
  │                 │ ── UDP ─────────► │ user_change (PUT)
  │ ◄── 200 ─────── │                   │ [CspUser.m_strForward 설정]
```

### A4. 개별 수신거부 설정

```
Console            CSC                 CSP
  │ PUT /users/     │                   │
  │  {pid}          │                   │
  │ {reject_id:     │                   │
  │  ["+82A..."]}   │                   │
  │ ──────────────► │ [DB] user_rejects INSERT
  │                 │ ── UDP ─────────► │ user_change (PUT)
  │ ◄── 200 ─────── │                   │ [CspUser.m_vecReject 갱신]
```

### A5. 가입자/구독 삭제

```
Console            CSC                 CSP
  │ DELETE /users/  │                   │
  │  {pid}/call/    │                   │
  │  {msisdn}       │                   │
  │ ──────────────► │ [DB] DELETE       │
  │                 │ ── UDP ─────────► │ user_change (DELETE)
  │ ◄── 200 ─────── │                   │ [CspUserMap 캐시 제거]
```

---

## Part B. 단말 등록

### B1. SIP 등록 성공

```
UE-A                    CSP (CSCF)
  │                      │
  │ ── REGISTER ──────► │
  │ ◄── 401 Unauthorized │  WWW-Authenticate:
  │                      │   realm, nonce, qop=auth
  │                      │
  │ ── REGISTER ──────► │  Authorization:
  │    + Digest Auth     │   username, nonce, nc, cnonce, response
  │                      │
  │                      │  [MD5 검증: A1=user:realm:pw, A2=REGISTER:uri]
  │                      │  [response = MD5(A1:nonce:nc:cnonce:qop:A2)]
  │                      │
  │ ◄── 200 OK ──────── │  Contact: <원래 AOR>
  │                      │  Expires: 600
  │                      │
  │                      │  [UserMap에 등록: IP, Port, Transport 저장]
  │                      │  [DB] register_time 갱신
```

### B2. SIP 등록 실패

```
UE-A                    CSP (CSCF)
  │ ── REGISTER ──────► │
  │ ◄── 401 ──────────── │
  │ ── REGISTER+Auth ──► │  [MD5 불일치 또는 사용자 없음]
  │ ◄── 403 Forbidden ── │
```

### B3. 등록 해제

```
UE-A                    CSP (CSCF)
  │ ── REGISTER ──────► │  Expires: 0
  │ ◄── 200 OK ──────── │
  │                      │  [UserMap에서 제거]
  │                      │  [DB] logout_time 갱신
```

---

## Part C. 서비스 중 (통화)

### C1. 기본 1:1 통화 (Proxy 모드)

```
UE-A                    CSP (Proxy)                 UE-B
  │                      │                            │
  │ ── INVITE ─────────► │                            │
  │    To: B@csp         │ [Proxy: Call-ID 유지]      │
  │                      │ [Via 추가, Record-Route]   │
  │                      │                            │
  │ ◄── 100 Trying ───── │                            │
  │                      │ ── INVITE ────────────────► │
  │                      │    (Via: CSP + 원본)       │
  │                      │                            │
  │                      │ ◄── 180 Ringing ────────── │
  │ ◄── 180 Ringing ──── │    (Via 제거)              │
  │                      │                            │
  │                      │ ◄── 200 OK ─────────────── │
  │ ◄── 200 OK ──────── │    [DB] state → active     │
  │                      │                            │
  │ ── ACK ────────────► │ ── ACK ──────────────────► │
  │                      │                            │
  │ ◄══ RTP Audio (직접 또는 CMP 릴레이) ════════════► │
  │                      │                            │
  │ ── BYE ────────────► │ ── BYE ──────────────────► │
  │                      │ ◄── 200 OK ─────────────── │
  │ ◄── 200 OK ──────── │    [DB] state → ended      │
```

### C2. DND 거부

```
UE-A                    CSP                          UE-B (DND)
  │                      │                            │
  │ ── INVITE B ───────► │                            │
  │                      │ [CspUser.isDnd() == true]  │
  │ ◄── 603 Decline ──── │                            │
  │                      │ [DB] end_reason=declined   │
```

### C3. 개별 수신거부

```
UE-A                    CSP                          UE-B (A를 거부)
  │                      │                            │
  │ ── INVITE B ───────► │                            │
  │                      │ [CspUser.isReject(A)==true] │
  │ ◄── 603 Decline ──── │                            │
  │                      │ [DB] end_reason=declined   │
```

### C4. 착신전환

```
UE-A                    CSP (B2BUA)                  UE-C (전환 대상)
  │                      │                            │
  │ ── INVITE B ───────► │                            │
  │                      │ [CspUser.isCallForward()]  │
  │                      │ [B2BUA 모드 전환]           │
  │ ◄── 302 Moved ────── │  Contact: <C@csp>         │
  │                      │                            │
  │ ── INVITE C ───────► │                            │
  │                      │ [일반 Proxy 처리]           │
  │                      │ ── INVITE C ─────────────► │
  │                      │ ◄── 200 OK ─────────────── │
  │ ◄── 200 OK ──────── │                            │
  │                      │                            │
  │ ◄══ RTP ═══════════════════════════════════════► │
```

### C5. 부재 (미등록)

```
UE-A                    CSP
  │                      │
  │ ── INVITE B ───────► │
  │                      │ [UserMap에 B 없음]
  │ ◄── 404 Not Found ── │
  │                      │ [DB] end_reason=error
```

### C6. 발신자 취소

```
UE-A                    CSP                          UE-B
  │                      │                            │
  │ ── INVITE B ───────► │ ── INVITE B ─────────────► │
  │                      │ ◄── 180 Ringing ────────── │
  │ ◄── 180 Ringing ──── │                            │
  │                      │                            │
  │ ── CANCEL ─────────► │ ── CANCEL ────────────────► │
  │ ◄── 200 OK ──────── │ ◄── 200 OK ─────────────── │
  │                      │ ◄── 487 Request Term. ──── │
  │ ◄── 487 ──────────── │                            │
  │                      │ [DB] end_reason=normal     │
```

### C7. 수신자 거절

```
UE-A                    CSP                          UE-B
  │                      │                            │
  │ ── INVITE B ───────► │ ── INVITE B ─────────────► │
  │                      │ ◄── 180 Ringing ────────── │
  │ ◄── 180 Ringing ──── │                            │
  │                      │                            │
  │                      │ ◄── 603 Decline ─────────── │
  │ ◄── 603 Decline ──── │                            │
  │                      │ [DB] end_reason=declined   │
```

### C8. RTP 릴레이 통화 (CMP 경유)

VoIP 통화는 PRtpTrans(4포트 블록: Audio RTP/RTCP + Video RTP/RTCP, 50000~ 대역)을 사용한다.

```
UE-A                    CSP (B2BUA)           CMP              UE-B
  │                      │                     │                │
  │ ── INVITE B ───────► │                     │                │
  │                      │ ── add ───────────► │ PRtpTrans 할당 │
  │                      │ ◄── {ip,port} ───── │ (50000~ 대역)  │
  │                      │                     │                │
  │                      │ ── INVITE B ─────────────────────► │
  │                      │    SDP: CMP relay IP:port          │
  │                      │                     │                │
  │                      │ ◄── 200 OK ────────────────────── │
  │                      │    SDP: B의 IP:port │                │
  │                      │                     │                │
  │ ◄── 200 OK ──────── │                     │                │
  │    SDP: CMP relay    │ ── modify ────────► │ B 주소 등록   │
  │                      │                     │                │
  │ ═══ RTP ═══════════════════════════════════════════════► │
  │    A → CMP relay     │                     │  CMP → B      │
  │ ◄═══════════════════════════════════════════════════════ │
  │    CMP relay → A     │                     │  B → CMP      │
  │                      │                     │                │
  │ ── BYE ────────────► │ ── BYE ──────────────────────────► │
  │                      │ ── remove ────────► │ 세션 해제     │
```

---

## Part D. 서비스 중 운용 변경

### D1. 통화 중 DND 설정

```
Console            CSC              CSP
  │ PUT dnd:true    │                │
  │ ──────────────► │ [DB UPDATE]    │
  │                 │ ── UDP ──────► │ user_change
  │ ◄── 200 ─────── │                │ [CspUser.m_bDnd = true]
  │                 │                │
  │                 │                │ 기존 통화: 영향 없음 (유지)
  │                 │                │ 이후 새 착신: 603 Decline
```

### D2. 통화 중 착신전환 설정

```
Console            CSC              CSP
  │ PUT forward_id  │                │
  │ ──────────────► │ [DB UPDATE]    │
  │                 │ ── UDP ──────► │ user_change
  │ ◄── 200 ─────── │                │ [CspUser.m_strForward 설정]
  │                 │                │
  │                 │                │ 기존 통화: 영향 없음 (유지)
  │                 │                │ 이후 새 착신: 302 Moved → 전환 대상
```

---

## 부록

### Proxy vs B2BUA 판정 로직

```
INVITE 수신
  │
  ├─ To가 PTT 그룹? ──────────── Yes → B2BUA (PTT-AS)
  ├─ To가 트렁크 프리픽스 매칭? ── Yes → B2BUA (IBCF)
  ├─ To 사용자 등록 여부 확인
  │   └─ 미등록? ───────────────── 404 Not Found
  ├─ DND 또는 수신거부? ─────── Yes → 603 Decline
  ├─ 착신전환 설정? ──────────── Yes → B2BUA + 302 Moved
  └─ 위 모두 아님 ──────────── Proxy 모드 (Call-ID 유지)
```

### VoLTE 통화 상태 (call.json 파일)

옛 `voip_call_logs` DB 테이블은 v3(2026-04-22) DROP. 현재 SoT 는 `service_log/volte/YYYY/MM/DD/HH/.../<call_id>.d/call.json` (CSP `CCallDir` 가 작성).

| 상태 | 시점 | 갱신 필드 |
|------|------|---------|
| `ringing` | INVITE 수신 | invite_time |
| `active` | 200 OK 수신 | answer_time |
| `ended` | BYE/CANCEL/에러 | end_time, duration, end_reason |

### 종료 사유 (end_reason)

| SIP status | end_reason | 설명 |
|------------|------------|------|
| 200 | normal | 정상 통화 후 종료 |
| 603 | declined | DND/수신거부 |
| 486 | busy | 통화 중 |
| 400-599 | error | 기타 에러 |
| 487 | normal | 발신자 취소 (CANCEL) |
