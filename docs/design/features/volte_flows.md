# VoLTE(VoIP) 서비스 케이스 및 메시지 Flow

---

## 케이스 목록

### Part A. 운용 설정

| # | 케이스 | 설명 |
|---|--------|------|
| A1 | VoIP 가입자 생성 및 구독 추가 | 사용자 생성 → VoIP 번호 할당 |
| A2 | 착신 차단 — 전체 설정 | 회선의 모든 착신 거절 (TS 24.611 ICB) |
| A3 | 착신전환 설정 | 다른 번호로 착신 전환 |
| A4 | 착신 차단 — 지정 번호 설정 | 사람 단위로 특정 발신 번호 거절 (ICB `cp:identity`) |
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
| C2 | 착신 차단 — 전체 | A→B, B 회선이 `icb_all` → 603 Decline |
| C3 | 착신 차단 — 지정 번호 | A→B, B 의 지정 번호에 A → 603 Decline |
| C4 | 착신전환 | A→B, B가 C로 전환 → 서버측 전환(181 + History-Info) → A↔C (TS 24.604) |
| C5 | 부재 (미등록) | A→B, B 미등록 → 404 Not Found |
| C6 | 발신자 취소 | A→B, A가 CANCEL → 487 |
| C7 | 수신자 거절 | A→B, B가 BYE/603 → 종료 |
| C8 | RTP 릴레이 통화 | CMP 경유 미디어 중계 |
| C9 | 실패 안내 | 통화중·무응답·없는 번호 → 183 early media 안내 → 원래 최종 코드 |
| C10 | 보류 음악 | A 의 hold re-INVITE → 피보류 B 에 CMP 보류 음악, resume 에 정지 |
| C11 | 통화 중 영상 전환 | A 의 영상 추가 re-INVITE → B 의 수락/거절이 A 의 결과(CSP 는 B 의 답까지 A 의 응답을 미룬다) |

### Part D. 서비스 중 운용 변경

| # | 케이스 | 설명 |
|---|--------|------|
| D1 | 통화 중 착신 차단 설정 | 이후 새 착신만 거절, 기존 통화 유지 |
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

### A2. 착신 차단 — 전체 설정

```
Console            CSC                 CSP
  │ PUT /users/     │                   │
  │  {pid}/call/    │                   │
  │  {msisdn}       │                   │
  │ {icb_all: true} │                   │
  │ ──────────────► │ [DB] UPDATE       │
  │                 │  icb_all=1        │
  │                 │ ── UDP ─────────► │ user_change (PUT)
  │ ◄── 200 ─────── │                   │ [CspUser.m_bIcbAll = true]
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

### A4. 착신 차단 — 지정 번호 설정

```
Console            CSC                 CSP
  │ PUT /users/     │                   │
  │  {pid}          │                   │
  │ {icb_identities:│                   │
  │  ["+82A..."]}   │                   │
  │ ──────────────► │ [DB] icb_identities 교체
  │                 │ ── UDP ─────────► │ user_change (PUT) — 그 사람의 전화 회선마다
  │ ◄── 200 ─────── │                   │ [CspUser.m_vecIcbIdentities 갱신]
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

### C2. 착신 차단 — 전체

```
UE-A                    CSP                          UE-B (icb_all)
  │                      │                            │
  │ ── INVITE B ───────► │                            │
  │                      │ [IncomingBarredBy = all]   │
  │ ◄── 183+SDP ──────── │ 거절 안내(declined) 재생   │
  │ ◄── 603 Decline ──── │                            │
  │                      │ [DB] end_reason=declined   │
```

### C3. 착신 차단 — 지정 번호

```
UE-A                    CSP                          UE-B (지정 번호 A)
  │                      │                            │
  │ ── INVITE B ───────► │                            │
  │                      │ [IncomingBarredBy(A)       │
  │                      │   = identity]              │
  │ ◄── 183+SDP ──────── │ 거절 안내(declined) 재생   │
  │ ◄── 603 Decline ──── │                            │
  │                      │ [DB] end_reason=declined   │
```

- 판정 = `CspUser::IncomingBarredBy(from)`(전체 ∨ 지정 번호 일치 — [volte_supplementary_services.md §6B](volte_supplementary_services.md), 로그 `TAS: Rejected (ICB all|identity)`).
  TAS `ApplyTerminationServices` 가 거절하면 거절 안내 `declined`(화중음)를 early media 로 들려준 뒤 603 그대로 끝낸다([announcements.md §3.2](announcements.md)).
  착신 차단은 가입 조건이라 착신자의 등록 여부와 무관하게 이 한 곳에서 판정한다. 착신전환(C4)이 걸린 회선이어도 착신 차단이 우선한다.
- 검증: 계측기 `VOLTE-ICB-ALL`(603 + 안내, CFU 가 걸려도 603)·`VOLTE-ICB-IDENTITY`(지정 번호 발신자만 603).

### C4. 착신전환

```
UE-A                    CSP (B2BUA · TAS)            UE-C (전환 대상)
  │                      │                            │
  │ ── INVITE B ───────► │                            │
  │                      │ [CTasModule::ResolveDiversion — B 의 forward_id(CFU)]
  │ ◄── 181 Call Is Being Forwarded ──                │  (SDP 없음, Setup.Sip.Cdiv.Notify181)
  │ ◄── 183 + SDP ────── │  전환 안내(announce_then_tone) │
  │                      │ ── INVITE C ─────────────► │  History-Info: <B>;index=1, <C;cause=302>;index=1.1;mp=1
  │                      │ ◄── 200 OK ─────────────── │
  │ ◄── 200 OK ──────── │                            │
  │                      │                            │
  │ ◄══ RTP (CMP relay) ══════════════════════════► │
```
- 302 리다이렉트를 쓰지 않는다 — 전환은 서버가 같은 호 안에서 한다(TS 24.604 CDIV, B 는 INVITE 를 받지 않는다).
  조건부 전환(CFB/CFNR/CFNL/CFNRc)·상한·루프(486)·전환 안내는 [volte_supplementary_services.md §6A](volte_supplementary_services.md)·
  [announcements.md §3.5](announcements.md).

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
- B 없는 거절(404·484·착신 차단 603)은 RELAY_ADD 를 A 만으로 잡고 같은 절차 — CDR 은 거절 시도로 남고 `announcement{situation, media, played_ms, result}` 가 붙는다.
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

### C11. 통화 중 영상 전환 (RFC 3264 §8.1 추가 · §8.2 제거)

```
UE-A                    CSP                          CMP                    UE-B
  │ ── re-INVITE (m=audio, m=video P) ► │ ── RELAY_MODIFY peer0 (video P) ► │      │
  │ ◄── 100 Trying ──────│ ── re-INVITE (m=audio, m=video relay) ──────────────► │
  │                      │ ◄── 100 Trying ─────────────────────────────────────── │  [B 사용자에게 묻는다]
  │                      │ ◄── 200 (m=video Q = 수락 / m=video 0 = 거절) ──────── │
  │                      │ ── RELAY_MODIFY peer1 (video Q / 0) ► │              │
  │ ◄── 200 (m=video relay / m=video 0) │                       │              │
  │ ── ACK ─────────────►│ ── ACK ─────────────────────────────────────────────► │
```

- **스트림 구성을 바꾸는 re-offer**(활성 audio·video·application 이 생기거나 없어짐 — psip `IsStreamSetChangeReInvite`)는 상대 단말이 받아들이는지가
  결과다. CSP 는 스택의 자동 200(기존 로컬 선언)을 미루고(`HoldReInviteAnswer` — 100 만), 상대 leg 로 전달한 re-INVITE 의 최종 응답을 relay 주소로
  돌려준다(`EventReInviteResponse` → `ForwardHeldReInviteAnswer`, RFC 3261 §14.2). 상대가 거절한 스트림은 port 0 그대로 간다 — relay 치환
  은 port 0 줄을 relay 포트로 바꾸지 않는다(바꾸면 제거가 추가로 바뀐다, RFC 3264 §8.2). 실패 응답(488·491 등)은 같은 코드로, 전달이 안
  되면 500. 미룬 동안 A 가 보낸 다음 re-INVITE 는 500 + Retry-After(§14.2), 상대가 끝내 답하지 않으면 40 s 뒤 500(`CheckHeldReInvite` — 전달한
  re-INVITE 트랜잭션 시한 32 s 뒤).
- **relay 포트는 미디어 종류로 정한다**(psip `SetRelayIpPort` — CMP `PRtpRelay` 소켓 배치 audio = base, video = base + 2). 통화 중 더한 영상 줄은
  m 줄 뒤에 붙으므로(예 audio·text·video — pjsua 는 실시간 문자 `m=text` 를 기본으로 제안한다) 순서로 매기면 영상이 relay 에 없는 포트로 간다.
  relay 가 중계하지 않는 스트림(text·message 등)은 port 0(거절, RFC 3264 §6). VoLTE relay 의 모든 SDP 전달 지점(첫 offer·answer·18x·PRACK·re-INVITE·
  전달·픽업·감청·안내)이 같다.
- 보류·해제(방향만)·주소 변경·세션 갱신은 relay 가 미디어를 고정하므로 종전대로 자동 200 으로 끝낸다(C10).
- 단말의 묻기·답(수락 = 영상을 받는 answer, 거절 = m=video port 0)은 [ue_sdk.md](ue_sdk.md) §4.5 «통화 중 영상 전환».

---

## Part D. 서비스 중 운용 변경

### D1. 통화 중 착신 차단 설정

```
Console            CSC              CSP
  │ PUT icb_all:true│                │
  │ ──────────────► │ [DB UPDATE]    │
  │                 │ ── UDP ──────► │ user_change
  │ ◄── 200 ─────── │                │ [CspUser.m_bIcbAll = true]
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
  │                 │                │ 이후 새 착신: 서버측 전환(C4) → 전환 대상
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
  ├─ 착신 차단(전체·지정 번호)? ─ Yes → 603 Decline (착신전환보다 우선)
  ├─ 착신전환 설정? ──────────── Yes → B2BUA + 서버측 전환(181·History-Info, C4)
  └─ 위 모두 아님 ──────────── Proxy 모드 (Call-ID 유지)
```

### VoLTE 통화 상태 (call.json 파일)

SoT 는 녹취 영역 `{Recording.Dir}/volte/YYYY/MM/DD/HH/.../<call_id>.d/call.json` (CSP `CCallDir` 가 작성 — [site_directory_layout.md](site_directory_layout.md)).

| 상태 | 시점 | 갱신 필드 |
|------|------|---------|
| `ringing` | INVITE 수신 | invite_time |
| `active` | 200 OK 수신 | answer_time |
| `ended` | BYE/CANCEL/에러 | end_time, duration, end_reason |

### 종료 사유 (end_reason)

| SIP status | end_reason | 설명 |
|------------|------------|------|
| 200 | normal | 정상 통화 후 종료 |
| 603 | declined | 단말 거절·착신 차단(ICB 전체/지정 번호) |
| 486 | busy | 통화 중 |
| 400-599 | error | 기타 에러 |
| 487 | normal | 발신자 취소 (CANCEL) |
