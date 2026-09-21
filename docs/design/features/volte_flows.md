# VoLTE(VoIP) 서비스 케이스 및 메시지 Flow

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
| C9 | 실패 안내 | 통화중·무응답·없는 번호 → 183 early media 안내 → 원래 최종 코드 |
| C10 | 보류 음악 | A 의 hold re-INVITE → 피보류 B 에 CMP 보류 음악, resume 에 정지 |

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

### C1a. early media — 18x 의 SDP 도 미디어 앵커를 지난다

착신 측(단말·IP-PBX·MGCF)이 183(또는 180)에 SDP 를 실으면 그 SDP 는 그 다이얼로그의 answer 다(RFC 3264 §5, 신뢰 응답이면
RFC 3262 §5 — 뒤의 200 은 같은 SDP). 링백·안내음(RFC 3960, TS 24.628)이 200 전에 흐르므로 CSP 는 **200 과 같은 앵커링을 18x 에서**
수행한다 — 미디어가 relay 를 우회하지 않고(녹취·SRTP 종단·NAT latch·토폴로지 은닉 유지) B-leg 주소가 발신자에 노출되지 않는다.

```
UE-A                    CSP                    CMP                     B (UE / 트렁크 피어)
  │ ── INVITE (SDP a) ─► │ ── RELAY_ADD peer0=a ─► │                         │
  │                      │ ── INVITE (SDP = relay B측) ─────────────────────► │
  │                      │ ◄── 183 (SDP b) ─────────────────────────────────── │
  │                      │ ── RELAY_MODIFY peer1=b ► │  [B-leg 주소·키 확정]  │
  │ ◄── 183 (SDP = relay A측) │                      │ ◄══ early media RTP ══ │
  │ ◄════════════ early media RTP (relay 경유) ═════ │                         │
  │                      │ ◄── 200 (SDP b) ─────────────────────────────────── │
  │                      │ ── RELAY_MODIFY peer1=b ► │  [같은 선언 — latch·SRTP 컨텍스트 유지]
  │ ◄── 200 (SDP = relay A측) │                      │                         │
```

- 18x 에 SDP 가 없으면(일반 180) 그대로 전달한다 — relay 변경 없음.
- SRTP(SDES) leg: 18x answer 의 `a=crypto` 를 200 과 같은 규칙으로 검증해 CMP 에 내린다. SAVP offer 에 crypto 가 없는/어긋난 18x 는
  **SDP 를 떼고** 전달한다(early media 없음) — 평문 폴백 금지에 따른 호 종료 판정은 확정 answer(200)에서 한다. 200 의 키가 18x 와
  같으면 `RELAY_MODIFY` 에 `media_crypto` 를 싣지 않는다(CMP 컨텍스트 재생성 = replay 창·ROC 소실 방지).
- 대표번호 포크(Flexible Alerting)의 대기 leg 18x 는 TAS 가 소비한다([dispatch_center.md](dispatch_center.md)) — 승자만 200 에서
  relay 에 고정되므로 포크 중에는 early media 를 앵커링하지 않는다.
- 검증: 계측기 `TRUNK-MGCF-EARLY-MEDIA` 의 `early_rtp_pct`(200 전에 발신자가 RTP 를 받았는가 — [test_instrument.md](test_instrument.md) §5).

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

### C9. 실패 안내 — early media 뒤 원래 최종 코드 (TS 24.628, RFC 3960)

B-leg 실패(486·480·408·404·603·5xx)나 CSP 자체 거절(404·484·603)이 정책([announcements.md](announcements.md) §6)에 걸리면 CSP 가 A 에게 **자기가 만든
answer 로 183** 을 내고 CMP 재생기가 안내를 들려준 뒤 **같은 코드**로 끝낸다. 200/BYE 로 바꾸지 않으므로 `end_status`·`end_reason`·통계는 그대로다.

```
UE-A                    CSP                          CMP                       UE-B
  │ ── INVITE ─────────► │ ── RELAY_ADD peer0 ────────► │                          │
  │                      │ ── INVITE ────────────────────────────────────────────► │
  │                      │ ◄── 486 Busy Here ──────────────────────────────────── │
  │                      │ ── RELAY_MODIFY peer0(remote_pt/codec) ─► │            │
  │ ◄── 183 (SDP relay A측 a=sendrecv, P-Early-Media: sendonly)     │            │
  │                      │ ── RELAY_PLAY peer0 [busy_kr 4 s → ann_busy] ────────► │
  │ ◄════════ early media RTP ═══════════════════════ │                          │
  │                      │ ◄── RELAY_PLAY_DONE(completed) ────────────────────── │
  │ ◄── 486 Busy Here ── │ ── RELAY_REMOVE ───────────► │  [DB] end_status=486    │
```

- A 의 CANCEL 이 재생 중 오면 `RELAY_PLAY_STOP` 뒤 487(CDR `announcement.result=cancelled`). 재생 상한(`Setup.Announcement.MaxPlayMs`)이면 그때 최종 코드.
- B 없는 거절(404·484·DND 603)은 RELAY_ADD 를 A 만으로 잡고 같은 절차 — CDR 은 거절 시도로 남고 `announcement{situation, media, played_ms, result}` 가 붙는다.
- 안내가 없는 조건(정책 none·CMP `resource.ann` 미광고·음원 없음·SDES 불일치)은 종전대로 응답 코드만.

### C10. 보류 음악 (TS 24.610 §4.5.2.4)

```
UE-A (보류)             CSP                          CMP                    UE-B (피보류)
  │ ── re-INVITE (a=sendonly) ► │ ── RELAY_MODIFY peer0 ────► │                    │
  │                      │ ── re-INVITE (a=sendonly, relay) ────────────────────► │
  │                      │ ── RELAY_PLAY peer1 [moh_simple, repeat 0] ─────────► │
  │                      │ ◄── 200 (a=recvonly) ────────────────────────────────  │
  │ ◄── 200 (a=recvonly) │                             │ ══ 보류 음악 RTP ══════► │
  │ ── re-INVITE (a=sendrecv) ► │ ── RELAY_PLAY_STOP ──────► │  → 일반 relay 재개    │
```

SDP 는 relay 에 고정돼 있어 재협상 없이 CMP 원천만 바뀐다. `a=inactive` 는 정책이 있으면 B 로 가는 offer 를 `sendonly` 로 고친다. 전달·픽업으로 leg 가
교체되면 먼저 정지한다. 프로파일은 피보류자 접속서비스 `hold_profile`.

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

SoT 는 `service_log/volte/YYYY/MM/DD/HH/.../<call_id>.d/call.json` (CSP `CCallDir` 가 작성).

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
