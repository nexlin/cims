# 안내음성·신호음 — 상황별 네트워크 안내(announcement)·링백·보류 음악

> CSP(제어)·CMP(재생)가 통화 상황에 맞는 안내음성·신호음·보류 음악을 단말에 **내보내는** 기능의 정본 설계.
> 규격 = TS 24.628(공통 기본 통신 절차 — early media·안내), TS 24.229 §5.7(AS)·TS 23.228 §4.7(MRFC/MRFP),
> RFC 3960(early media·링백 모델), RFC 5009(P-Early-Media), RFC 4240(netann 의미론), RFC 3326(Reason),
> TS 24.610(HOLD — 피보류자 안내), TS 24.615(통화중대기), TS 24.604(착신전환 CDIV — 181·History-Info RFC 7044·cause RFC 4458),
> RFC 7462(Alert-Info URN), ITU-T E.182(톤·안내 적용 원칙), E.180 Sup.2(한국 신호음). P1(§12 ①~⑦)과 착신전환 안내(§3.5·§12 ⑪)는 구현 반영, 나머지 P2(§11)는 로드맵이다.
>
> 관련: [../modules/cmp.md](../modules/cmp.md) · [../modules/csp.md](../modules/csp.md) · [../../api/cmp_media_api.md](../../api/cmp_media_api.md)
> · [volte_flows.md](volte_flows.md) C1a · [sip_service_model.md](sip_service_model.md) §2-9 · [media_security.md](media_security.md)
> · [ue_nat_traversal.md](ue_nat_traversal.md) · [test_instrument.md](test_instrument.md) §4(샘플 라이브러리 — 형식만 공유).

## 1. 범위와 전제

**문제.** 현행 CSP 는 실패를 SIP 응답 코드로만 돌려주고(486·480·404·484·603 그대로 — `RelayEndStatus`), 보류 re-INVITE 는
통과시키며, 통화중대기는 없다. CMP 는 relay·PTT floor·청취 tap·트랜스코딩·녹취만 있고 **재생 원천이 없다**. 그래서 발신자는
통화중·없는 번호에 아무 소리를 듣지 못하고 피보류자는 무음을 듣는다. VoLTE 단말(GSMA IR.92)은 실패 안내를 망에 기대한다.

**범위(P1).** 1:1 전화 호(VoLTE·유선 VoIP·피어 트렁크)의 ① 실패 안내(통화중·무응답·없는 번호·거절·혼잡) ② 보류 음악(MOH)
③ 정책 모델(전역 프로파일 + 접속서비스별 override, 가입자 링백 확장 자리) ④ 서비스 음원 라이브러리(OAM 등록·CMP 배포).
**P2** = 통화중대기(TS 24.615)·서버 링백(컬러링) 켜기·착신전환 안내(TS 24.604 — 서버측 전환과 함께 구현, §3.5)·믹스 모드·관제 큐/ACD 대기열 안내.

**범위 밖.** PTT 그룹콜(TS 24.379/24.380 은 서버 안내음을 정의하지 않고 floor 톤은 단말 몫), MCData, 영상 안내, 런타임 TTS,
SIP-I 트렁크의 ISUP 안내 인디케이터.

**설계 원칙.**

1. **IMS 분담 그대로** — CSP 가 MRFC(언제·어느 leg·무엇), CMP 가 MRFP(렌더링). CMP 에 SIP 스택(RFC 4240 netann 서버)을 얹지 않고,
   netann 의 의미(play·repeat·delay·duration)를 기존 UDP JSON 계약([cmp_media_api.md](../../api/cmp_media_api.md))의 새 명령으로 담는다.
2. **실패는 실패로 남는다** — 안내는 early media(183+SDP)로 재생하고 **원래 최종 코드를 그대로** 낸다(RFC 3960 §3.1 게이트웨이 모델,
   TS 24.628 §4.5). 200/BYE 로 바꾸는 answer 모델은 통계([sip_statistics.md](sip_statistics.md) 3계층)·CDR 을 왜곡하므로 쓰지 않는다.
3. **경로 하나** — 모든 안내는 relay 세션의 peer leg 에 붙는 재생기다. B leg 가 없는 실패(404 등)도 발신 leg 만 가진 relay 를 잡고 재생한다.
4. **정책은 프로파일** — 상황→동작 표 하나가 프로파일이고, 전역 기본 프로파일 위에 접속서비스가 프로파일을 고른다. 가입자 단위는
   링백 음원 하나만 덧씌우는 자리를 남긴다(§6.3). 일회성 예외 필드를 늘리지 않는다.
5. **음원은 서비스 라이브러리가 정본** — 계측기 샘플 라이브러리([test_instrument.md](test_instrument.md) §4)와 **파일 형식·변환기만 공유**하고,
   카탈로그·저장소·API·UI·수명은 별개다(§7). 둘은 서로의 등록·삭제에 영향을 주지 않는다.

## 2. 상황과 규격 근거

| 상황 | 트리거(CSP 관측) | 규격 | 기본 동작(P1 `default` 프로파일) |
|---|---|---|---|
| `busy` 통화중 | B-leg 최종 486, 또는 Reason Q.850 cause 17 | TS 24.628 §4.5, E.182 §4 | 화중음 4 s → 안내 "통화 중이오니…" → **486** |
| `no_answer` 무응답 | B-leg 480(단말 응답)·408·CSP 링 타이머 만료, cause 18/19 | TS 24.628 | 안내 "전화를 받을 수 없습니다" → **480/408 원코드** |
| `unreachable` 부재 | CSP 판정 480(가입자는 알지만 도달 경로 없음 — 미등록) | TS 24.229 §5.4.3.3, RFC 3261 §21.4.18 | 안내 "전화를 받을 수 없습니다" → **480** |
| `not_found` 없는 번호 | 404·410, cause 1 | TS 24.229 | 안내 "없는 번호입니다" → **404** |
| `invalid` 번호 불완전 | 484(다이얼 플랜 번역 불가) | TS 24.229 §5.4.3.2 | 안내 "없는 번호입니다" → **484** |
| `declined` 거절 | 603(단말 거절·DND·수신거부), cause 21 | TS 24.628 | 화중음 6 s → **603** |
| `congestion` 혼잡 | 5xx(재라우팅 소진)·488(코덱 불일치·변환 슬롯 소진)·CMP 자원 없음, cause 34/42 | E.182 §4.5 | 혼잡음 6 s → **원코드** |
| `forbidden` 차단 | 403(ACL·채널 정책) | — | **안내 없음**(보안 — 응답만) |
| `hold` 보류 | 한 leg 의 re-INVITE offer `a=sendonly`/`a=inactive` | TS 24.610 §4.5.2.4 | 피보류 leg 에 보류 음악 loop, resume(sendrecv) 에 정지 |
| `ringback` 링백 | B-leg 첫 18x(SDP 없음) 수신 | RFC 3960 §3, TS 24.628 | **off**(단말 로컬 링백). 접속서비스 스위치로 서버 링백 — §6 |
| `call_waiting` 통화중대기 | 통화 중 가입자에게 두 번째 INVITE | TS 24.615, RFC 7462 | **P2** — §11 |
| `forwarded` 전환 안내 | 착신 가입자 `forward_id`(CFU) 로 B-leg 를 전환 대상으로 낼 때(서버측 전환, [volte_supplementary_services.md §6A](volte_supplementary_services.md)) | TS 24.604 §4.5.2.6, RFC 7044 | 발신자에 181 → 전환 안내 1회 → **링백음을 전환 대상 응답까지**(`announce_then_tone`) — §3.5 |

**상황 판정 우선순위.** B-leg 최종 응답에 `Reason: Q.850;cause=N` 이 있으면 cause 로 먼저 판정하고(RFC 3326 — 피어·MGCF 가
PSTN 원인을 실어 준다), 없으면 SIP 코드로 판정한다. 판정 표는 `CspAnnouncement::Classify(status, reason)` 하나에 둔다.

**동작(`mode`) 어휘** — E.182 원칙(톤 먼저, 안내는 톤으로 뜻이 전달되지 않을 때):

| mode | 뜻 |
|---|---|
| `none` | 안내 없음 — 응답만(현행 동작) |
| `tone` | 신호음만 `tone_ms` 동안(loop) |
| `announce` | 안내음 1회(`repeat` 회) |
| `tone_then_announce` | 신호음 `tone_ms` → 안내음 |
| `announce_then_tone` | 안내음(`repeat` 회) → 신호음 `tone` 을 **STOP 까지** loop — 전환 안내 뒤 링백(§3.5). 신호음이 비면 안내 뒤 `ringback` 규칙으로 |
| `media` | 임의 음원(`media`, `loop`) — hold·ringback 용 |

## 3. 시그널링 절차

### 3.1 실패 안내 — early media 모델 (B-leg 실패)

```
UE-A                     CSP                          CMP                       UE-B
  │ ── INVITE (SDP a) ─► │ ── RELAY_ADD peer0=a ─────► │                          │
  │                      │ ── INVITE (SDP relay B측) ─────────────────────────────► │
  │                      │ ◄── 486 Busy Here (Reason: Q.850;cause=17) ─────────── │
  │                      │ [Classify → busy, profile → tone_then_announce]        │
  │                      │ ── RELAY_MODIFY peer0 (media_codec = A 협상 코덱) ─► │
  │ ◄── 183 (SDP relay A측, a=sendrecv) ── │            │                          │
  │     P-Early-Media: sendonly            │            │                          │
  │                      │ ── RELAY_PLAY peer0 [busy_kr 4 s, ann_busy] ────────► │
  │ ◄════════ early media RTP (화중음 → 안내음) ═══════ │                          │
  │ ── RTP (폐기) ══════════════════════════════════════► │  [NAT leg latch]        │
  │                      │ ◄── RELAY_PLAY_DONE (completed) ───────────────────── │
  │ ◄── 486 Busy Here ── │ ── RELAY_REMOVE ───────────► │                          │
```

- **183 의 answer 는 CSP 가 만든다**(B answer 가 없다): relay peer0 포트, 코덱 = A offer ∩ 코덱 테이블([csp.md §6.1](../modules/csp.md)) 최상위,
  `telephone-event` echo, SRTP 는 접속서비스 `media_srtp` 규칙 그대로(`MediaSdes` — offer 의 `a=crypto` 검증 + 서버 tx 키 생성, `required` 인데
  offer 에 없으면 안내 없이 원코드). 신뢰 1xx 는 psip 가 offer 의 `Supported: 100rel` 을 보고 RSeq 를 붙인다(`RingCall(callId, 183, rtp)`).
- **방향은 `a=sendrecv`** — `a=sendonly` 로 답하면 단말이 RTP 를 보내지 않아 NAT leg 의 목적지 latch([ue_nat_traversal.md §5](ue_nat_traversal.md))가
  일어나지 않고 안내음이 선언 주소로 나가 유실된다. sendrecv 로 답하고 ingress 는 CMP 가 폐기한다(RFC 3960 §3.1 이 허용). 방향 지시는
  헤더 `P-Early-Media: sendonly`(RFC 5009 — 망→단말 방향만 인가)로 준다. psip 에 18x 부가 헤더 API 를 더한다(§5.3).
- **순서는 `RELAY_MODIFY`(A leg 재생 코덱) → `RELAY_PLAY` → 183+SDP** — 재생기가 거절하면(음원 없음·슬롯 소진) A 에 answer 를 남기지 않고 원코드/원경로로
  폴백한다(SDP 만 받은 단말은 로컬 링백 없이 무음을 듣게 되므로). 183 송신이 실패하면 재생을 STOP 한다. 위 그림의 183·RELAY_PLAY 는 같은 ms 안이다.
- 이미 B 의 18x+SDP 로 early media 가 앵커링된 뒤 실패하면([volte_flows.md](volte_flows.md) C1a) 183 을 다시 내지 않고 RELAY_PLAY 만 한다
  — peer0 의 answer 는 그대로다(B 가 준 SDP 를 CSP 가 A 에게 이미 냈으므로 코덱도 그것이다).
- **최종 응답은 재생 뒤** `RELAY_PLAY_DONE` 또는 `MaxPlayMs` 타이머에서 `StopCall(A, 원코드, Reason)` — `RelayEndStatus` 매핑·Reason 투과 규칙은
  현행 그대로다([csp.md §3.1](../modules/csp.md) `EventCallEnd`). A 의 **CANCEL** 이 재생 중 오면 `RELAY_PLAY_STOP` 뒤 487(psip 기본).
- CMP 가 `resource.ann` 을 광고하지 않거나(`AnnPlayers=0`) RELAY_PLAY 가 `MEDIA_NOT_FOUND`/`ANN_CAPACITY` 로 거절되면 **즉시 원코드**
  — 안내가 실패를 가리지 않는다. 카운터 `ann_fallback` + LOG_ERROR(음원 누락은 알람 §10).
- 통계 무영향: 최종 코드가 그대로라 시도/세션/leg 3계층·성공률 정의가 바뀌지 않는다. CDR(`call.json`)에 `announcement{situation, media[], played_ms}`
  만 더한다(§9).

### 3.2 실패 안내 — B leg 이전 실패 (404·484·480 미등록·603 DND)

`EventIncomingCall` 이후 psip UAS 다이얼로그가 있으면 같은 절차다: `RELAY_ADD`(peer0=A 만, peer1 미확정) → CallMap 에 A 단독 entry → 183 → RELAY_PLAY → 원코드.
거절 지점 = `RejectVoice`(404 미등록·488·500 등)·다이얼 플랜 484·TAS `ApplyTerminationServices`(DND·수신거부 603) 가 `gclsAnnouncement.Reject(callId, rtp, from, to, code, reason, msg)`
를 먼저 부르고, 인수되지 않으면(정책 none·CMP 미지원·SDES 불일치·조립 실패) 종전처럼 `StopCall`. 시도 장부(`VoipCallRejected`)는 인수 여부와 무관하게 남는다.
자체 거절한 UAS 다이얼로그에는 psip 가 `EventCallEnd` 를 올리지 않으므로 최종 응답 뒤의 마감(CDR·DB·`CallMap.Delete`→RELAY_REMOVE·소유권)은 서비스가 직접 한다.

**다이얼로그 이전 거절**(`RecvRequest` 단계 — `ScreenInvite` 603, 다이얼 플랜 484 의 일부)은 early media 를 낼 다이얼로그가 없어 **응답만** 나간다(현행).
판정 지점을 `EventIncomingCall` 로 옮기는 것은 시도 장부 기록 시점과 얽혀 P2(§11).

### 3.3 보류 음악 (TS 24.610)

```
UE-A (보류 주체)         CSP                          CMP                    UE-B (피보류)
  │ ── re-INVITE (a=sendonly) ► │ [EventReInvite: 방향 감지 → hold]                  │
  │                      │ ── re-INVITE (a=sendonly, SDP relay) ────────────────► │
  │                      │ ◄── 200 (a=recvonly) ─────────────────────────────── │
  │ ◄── 200 (a=recvonly) │ ── RELAY_PLAY peer1 [moh_simple, repeat 0] ────────► │
  │                      │                             │ ══ MOH RTP ═══════════► │
  │ ── re-INVITE (a=sendrecv) ► │ [resume]                                         │
  │                      │ ── RELAY_PLAY_STOP peer1 ─► │                          │
  │                      │ ── re-INVITE ─────────────────────────────────────────► │
```

- 감지는 `EventReInvite` 의 offer 방향(psip `ERtpDirection`) — `sendonly`/`inactive` = hold, `sendrecv` = resume(psip 의 세션 갱신 판정은 방향 속성도 비교하므로 주소·포트가 같은 hold re-INVITE 가 "미디어 무변경" 으로 건너뛰어지지 않고, media-list passthrough SDP 를 낼 때도 다이얼로그 방향(`m_eLocalDirection`)으로 audio/video 의 방향 속성을 다시 쓰므로 `HoldCall`·`SetDirection` 재작성이 와이어에 실린다). re-INVITE 자체는 현행대로 통과시킨다
  (B 는 RFC 3264 대로 보류 상태를 안다). SDP 가 relay 에 고정돼 있어 **재협상 없이** CMP 가 원천만 바꾼다.
- `a=inactive` offer 는 프로파일 `hold.mode≠none` 이면 B 로 가는 offer 를 `a=sendonly` 로 재작성한다(TS 24.610 §4.5.2.4 — 안내를 위해 AS 가 방향을 고칠 수 있다).
  `none` 이면 그대로 통과.
- 정지 계기 = resume, 어느 leg 의 종료, REFER 전달(`OnBlindTransfer`/`OnTransfer`) · 픽업 재키잉 — 재생 중인 leg 가 교체되면 CSP 가 STOP 을 먼저 낸다.
- 보류 중 `hold_timeout` 회수는 재생기가 활동을 갱신하므로 일어나지 않는다. 장시간 보류의 회수는 SIP 세션 타이머([leg_liveness.md](leg_liveness.md)) 몫이다.
- 양쪽이 서로 보류하면 leg 마다 독립 재생기다(peer0·peer1 각 1개).

### 3.4 서버 링백 (기본 off)

접속서비스 프로파일의 `ringback.mode=media` 일 때만: B-leg 첫 18x(SDP 없음) 수신 시 3.1 과 같은 183+SDP(A) 를 내고 `RELAY_PLAY peer0 [ringback 음원, repeat 0]`,
B 의 18x+SDP·200 이 오면 STOP 뒤 현행 앵커링(C1a) — B 의 SDP 없는 180 은 **180+SDP(relay A측)** 로 바꿔 전달해 단말이 로컬 링백으로 갈아타지 않게 한다
(RFC 3960 §3.2). 대표번호 포크 중에는 하지 않는다(C1a 와 같은 이유). 음원 결정 = §6.3 가입자(피착신자) → 접속서비스 → 전역.

### 3.5 착신전환 안내 (TS 24.604 CDIV)

착신전환은 **서버측 전환**이다 — 착신 가입자에 `forward_id`(CFU) 가 있으면 CSP 가 B-leg 를 전환 대상으로 낸다(302 리다이렉트를 쓰지 않는다:
안내·History-Info·시도 1건 통계의 삽입 지점이 없다). 전환 판정·연쇄·상한·History-Info 는 [volte_supplementary_services.md §6A](volte_supplementary_services.md) 가 정본이고,
여기는 발신자가 듣는 것만 적는다.

```
UE-A                     CSP                          CMP                       UE-C (전환 대상)
  │ ── INVITE (to B) ───► │ [B.forward_id=C → CDIV]                               │
  │ ◄── 181 Call Is Being Forwarded ── │  (Setup.Sip.Cdiv.Notify181)              │
  │                      │ ── RELAY_ADD peer0=a ─────► │                          │
  │ ◄── 183 (SDP relay A측, P-Early-Media: sendonly) ── │                          │
  │                      │ ── RELAY_PLAY peer0 [ann_forwarded ×1] ─────────────► │
  │                      │ ── INVITE (History-Info: <B>;index=1, <C;cause=302>;index=1.1;mp=1) ─────► │
  │ ◄══ "다른 번호로 연결됩니다" ══ │                             │                          │
  │                      │ ◄── RELAY_PLAY_DONE(completed) ── │  ◄── 180 Ringing ──────────── │
  │ ◄── 180 (같은 SDP) ── │ ── RELAY_PLAY peer0 [ringback_kr, repeat 0] ─────────► │
  │ ◄══ 링백음 ═══════════ │                             │  ◄── 200 OK (SDP c) ───────── │
  │                      │ ── RELAY_PLAY_STOP · RELAY_MODIFY peer1 ────────────► │
  │ ◄── 200 OK ────────── │                                                          │
```

- 순서 = 181(SDP 없음·비신뢰 1xx) → relay·CallMap 을 만든 뒤 **B-leg INVITE 를 내기 직전**에 `OnForwarded(A, B)`: 3.1 과 같은 CSP answer(183+SDP, `a=sendrecv`,
  `P-Early-Media: sendonly`)를 A 에 내고 프로파일 `forwarded` 의 1 단계(안내)를 붙인다. 그래서 C 의 첫 SDP 없는 18x 도 같은 SDP 로 나간다(`OnRingback` 은
  A 에 이미 answer 를 낸 호면 프로파일과 무관하게 true — 단말이 로컬 링백으로 갈아타지 않는다, RFC 3960 §3.2).
- **2 단계** = 1 단계 `RELAY_PLAY_DONE(completed|max)` 에서 `announce_then_tone` 의 `tone` 을 repeat 0 으로(기본 `sys:ringback_kr`), 신호음이 없으면 `ringback` 규칙이
  media 일 때 그 음원으로. 둘 다 없으면 안내 뒤 무음(단말은 SDP 를 받은 상태라 로컬 링백을 내지 않는다 — 배치가 `tone` 을 두는 것이 원칙). 1 단계 완료는 CDR
  `announcement{situation: forwarded}` 로 남는다.
- 정지·교체 = C 의 SDP 있는 18x·200(`OnRingbackEnd`), C 실패(`OnLegFailed` — 같은 SDP 위에서 실패 안내로 교체, 183 재송 없음), A 의 CANCEL(`OnCallEnd`).
- 프로파일 `none`·CMP 미지원·answer 조립 실패면 **181 만** 나가고 호는 그대로 진행한다(안내가 전환을 막지 않는다). `trunk` 프로파일은 none(NNI 는 코드·헤더만).
- 기본 세트에 `sys:ann_forwarded`("전화가 다른 번호로 연결됩니다. 잠시만 기다려 주십시오.")가 추가된다(§7.2).

## 4. CMP — 재생기(ANN function)

### 4.1 계약 (cmp_media_api.md §6.7 예정)

자원 = **player**, 키 `(node, session_id, play_id)`. relay 세션의 peer leg 하나에 붙고 **수명은 세션에 종속**(RELAY_REMOVE·sweeper 회수 = 일괄 정지, 이벤트 없음).
leg 당 동시 재생기 1개 — 같은 leg 에 새 RELAY_PLAY 는 이전 재생기를 교체한다(`RELAY_PLAY_DONE reason=replaced`).

**RELAY_PLAY** (멱등 — 같은 `play_id` 재요청은 진행 중 상태 반환)

| payload | 필수 | 설명 |
|---|---|---|
| `session_id` | O | 대상 relay 세션. 없으면 `NOT_FOUND`(부활 금지) |
| `peer_index` | O | 재생 대상 leg (0=A / 1=B) |
| `play_id` | O | client 명명(세션 내 유일). client 는 시도마다 새 키를 쓴다 |
| `media` | O | 음원 열 — 문자열 id 또는 `{id, repeat, max_ms}` 객체(순서대로 재생). 항목 `repeat 0` = `max_ms` 까지 반복(tone_then_announce 의 신호음 구간). 쉼표 문자열도 수용 |
| `repeat` | - | 전체 시퀀스 반복 수. 기본 1, **0 = STOP 까지 무한**(hold·ringback) |
| `delay_ms` | - | 반복 사이 무음(RFC 4240 `delay`). 기본 0 |
| `max_ms` | - | 총 재생 상한 — 넘으면 `reason=max`. 기본 = CMP `AnnMaxPlayMs` |
| `mode` | - | `replace`(기본 — 재생 중 반대 peer 의 relay 를 이 leg 로 보내지 않음) / `mix`(**예약** — §11 통화중대기 톤 삽입) |

응답 payload: `{ "codec": "AMR-WB/16000", "duration_ms": 9000 }`(시퀀스 1회 길이. repeat 0 이면 `duration_ms: 0`). 같은 `play_id` 재요청(멱등)은 `{codec, played_ms}`.

**RELAY_PLAY_STOP**: `session_id` + `play_id`(없거나 이미 끝났으면 `OK` — 자연 멱등). 응답 payload `{ "played_ms": N }`.

**이벤트 RELAY_PLAY_DONE**(§8 채널 — ack·1 s×5 재전송): `session_id`, `peer_index`, `play_id`, `reason`(`completed`|`max`|`stopped`|`replaced`|`error`),
`played_ms`. `stopped`/`replaced` 는 client 가 낸 명령의 결과라 이벤트를 생략해도 되지만 **통일을 위해 항상 낸다**(CSP 는 무시 가능).

**코덱 선택.** 재생 코덱 = 그 leg 의 `media_codec` 선언([§6.6](../../api/cmp_media_api.md)) → 없으면 `remote_codec`(+`remote_pt`, 정적 PT 0 인 PCMU 도 `remote_codec` 만으로 남는다) →
둘 다 없으면 `BAD_REQUEST`. CSP 는 CSP 가 만든 answer 의 코덱을 `RELAY_ADD`/`RELAY_MODIFY` 의 `remote_pt`/`remote_codec` 으로 그 leg 에 알린다(`media_codec` 은 트랜스코딩 판정을 건드리므로 쓰지 않는다). 파일은 카탈로그의 그 코덱 항목(§7.1) —
없으면 `MEDIA_NOT_FOUND`(런타임 인코딩은 하지 않는다 — 라이브러리가 4 코덱을 모두 만들어 두는 것이 계약). `telephone-event` 는 재생과 무관.

**오류**: `NOT_FOUND`(세션) · `MEDIA_NOT_FOUND`(음원 id 또는 그 코덱 파일 없음 — 신설) · `ANN_CAPACITY`(재생 슬롯 소진 — 신설) · `BAD_REQUEST`(코덱 미확정·mode 미지원).
**자원 광고**: HEARTBEAT/STATS `resource.ann{total,used,media}` — 키 없음 = 미지원(`AnnPlayers=0`), CSP 는 안내를 건너뛴다. STATS `detail.ann[]` = 진행 중 재생기
(session·peer·play_id·media·played_ms) + `detail.ann_catalog{root, ids[], missing[]}`. Flow 로그 `INT ANN_PLAY`/`INT ANN_DONE`(detail = media 열·reason). CORE `ANN_RELOAD` = 카탈로그 재적재(SIGUSR1 과 같다).

### 4.2 내부 구조

- **`PAnnCatalog`** — `AnnouncementDir` 아래 `sys/`·`op/`(·`sub/`) 와 카탈로그 `config/announcements.jsonl`(§7.1) 를 읽어 `id → {코덱 → 프레임 배열}` 로
  **기동 때 메모리에 적재**한다(파일은 수 초 분량이라 전량 상주 — 재생 경로에 디스크 I/O 없음). 재적재 = `SIGUSR1`(CSP 와 같은 관례) 또는 CORE 명령 `ANN_RELOAD`.
  프레임화: PCMU/PCMA/G.722 = 160 B/20 ms(G.722 는 RFC 3551 §4.5.2 클록 8000 표기), AMR-WB = RFC 4867 저장 형식 프레임 1개/20 ms(`PTranscoder::parseAmrWb`
  계열 재사용). 누락 파일·형식 오류는 적재 시 LOG_ERROR + 알람(§10) — 그 항목만 제외하고 기동은 계속한다.
- **`PAnnPlayer`** — leg 하나에 붙는 재생 상태(시퀀스 위치·프레임 인덱스·SSRC·seq·ts·시작 시각·repeat 남은 수). 자기 **SSRC**(난수, 상대 스트림과 다름)·
  leg 의 송신 PT(`ptOut` 또는 `codecDesc.pt`)·시작과 loop 경계에 marker. AMR-WB 는 leg `fmtp` 의 octet-align 으로 페이로드를 만든다(`PTranscoder::buildAmrWb`).
  egress 는 relay 와 같은 소켓·같은 leg SRTP 컨텍스트(`Leg.crypto` protect) — 평문/SRTP 구분 없이 동작.
- **페이싱** — relay 는 epoll 이벤트 구동이라 클록이 없다([cmp.md §8](../modules/cmp.md)). 워커마다 **timerfd 20 ms 하나**를 같은 epoll 에 등록하고, 틱마다
  그 워커의 활성 재생기를 순회해 프레임을 낸다(재생기 수와 무관하게 타이머 1개, 재생기 0 이면 disarm). 별도 스레드를 두지 않는 이유 = `PMediaCrypto`·`Leg` 는
  같은 리액터에서만 만진다는 규약. 틱 지터는 timerfd `TFD_TIMER_ABSTIME` 절대 시각 누적으로 흡수(드리프트 없음).
- **replace 모드** — 재생 중인 leg 로는 반대 peer 의 오디오 RTP 를 relay 하지 않는다(RTCP 는 통과, 영상 무관). 트랜스코더가 붙어 있어도 그 방향 출력은 버린다.
  ingress(재생 leg 가 보내는 RTP)는 현행대로 NAT latch·활동 갱신에 쓰고 상대 leg 로 relay 한다(hold 의 A 는 sendonly 라 실제로는 오지 않는다).
- **녹취·tap 에 안내는 실리지 않는다** — 둘 다 ingress 복사라는 현행 원칙 그대로다. 안내 재생은 세그먼트 메타에도 남기지 않는다(CDR 이 갖는다 §9).
- **활동 갱신** — 재생 틱마다 `touchActivity()` → `orphan_no_rtp`(120 s)·`hold_timeout` 회수가 재생 중에는 일어나지 않는다.
- **자원** — `AnnPlayers`(기본 32) = 동시 재생기 수 상한. 재생 CPU 는 파일 프레임 복사 + SRTP protect 뿐이라 relay 한 방향과 같은 급이다(인코딩 없음).

### 4.3 cmp.json

| 키 | 기본 | 뜻 |
|---|---|---|
| `AnnouncementDir` | `announcements` | 음원 루트(`sys/`·`op/`·`sub/` 하위) — 상대 경로는 install_path 기준 |
| `AnnPlayers` | 32 | 동시 재생기 상한. **0 = 기능 비활성**(`resource.ann` 미광고) |
| `AnnMaxPlayMs` | 60000 | `max_ms` 생략 시 상한(repeat 0 은 예외 — STOP 까지) |
| `AnnNatWaitMs` | 500 | NAT leg 의 재생 시작 지연 상한(latch 전 선언 주소 오송신 방지, §8) |

`config_template.json` 섹션 `announcement`(scope service) + collection `announcements`(운영자 카탈로그 §7.1 — agent `/collection` 이 `config/announcements.jsonl` 에 쓴다. CMP 의 첫 collection).
구현 = `cmp/PAnnCatalog`·`PAnnPlayer`·`PAnnTicker` + `PRtpRelay::annTick`·`startAnn`·`stopAnn` + `PCmpServer::processPlay/processPlayStop/processAnnReload/onAnnDone`.

## 5. CSP — 정책·상태 머신

### 5.1 컴포넌트 `CspAnnouncement.{h,cpp}` (`CCspAnnouncementService`)

ModuleDispatcher 가 소유하는 단일 서비스(공통 기본 통신 절차 — TS 24.628 이 TAS 보조 서비스가 아니라 기본 절차이므로 B2BUA 골격에 둔다).
TAS(hold·전달·픽업·포크)와 IBCF(피어 실패)는 이 서비스를 **호출**한다.

| API | 호출 지점 | 동작 |
|---|---|---|
| `Classify(status, reason) → Situation` | 내부 | §2 판정 표(Reason Q.850 우선). CSP 자체 480 은 `Reject` 가 unreachable 로 바꾼다 |
| `Resolve(situation, profile) → Action` | 내부 | §6 프로파일 해석(그 프로파일 → DefaultProfile → none). 발신자 프로파일은 `ProfileForCaller(caller, msg)`(inbound Route 피어 → RemoteNode, 가입자 → 접속서비스)로 INVITE 때 정해 `CCallInfo::m_strAnnProfile` 에 둔다 |
| `bool OnLegFailed(bCallId, clsB, status, reason)` | `EventCallEnd`(B 실패, 재라우팅 소진 뒤) | true = 인수(호출자는 B entry 만 지운다). 3.1 절차 — A 가 이미 SDP 를 받았으면(B 18x+SDP·링백) 183 생략 |
| `bool Reject(callId, rtp, from, to, status, reason, msg)` | `EventIncomingCall` 거절 지점·TAS 603 | 3.2 절차 — 인수 못 하면 false(호출자가 원코드) |
| `bool OnHold(holderCallId, clsHolder)` / `OnResume(...)` | `EventReInvite` 방향 감지(roles.TAS) | 3.3 — 반환 true 면 inactive 를 sendonly 로 재작성 |
| `bool OnRingback(bCallId, clsB, …)` / `OnRingbackEnd(bCallId, clsB)` | `EventCallRing`(SDP 없는 18x) / 18x+SDP·`EventCallStart` | 3.4 (프로파일 `ringback=media` 일 때만). A 에 이미 CSP answer 를 낸 호(링백·전환 안내)면 프로파일과 무관하게 true |
| `bool OnForwarded(aCallId, bCallId)` | `EventIncomingCall` — 전환 B-leg `StartCall` 직전 | 3.5 — 프로파일 `forwarded` 의 1 단계(안내) + 183+SDP. `OnPlayDone` 이 2 단계(신호음/ringback loop)로 잇는다 |
| `void OnPlayDone(sessionId, peer, playId, reason, playedMs)` | `CCmpClient` 이벤트 dispatch 스레드 | 대기 중 최종 응답 발사(`FinishEarly`) · `forwarded` 1 단계 완료 → CDR + 2 단계 재생(`announce_then_tone` 의 tone 또는 `ringback` 규칙) |
| `void OnCallEnd(callId)` / `OnLegReplaced(relaySessionId)` | `EventCallEnd` 진입부 / TAS 전달·픽업 RELAY_MODIFY 앞 | 재생 회수(STOP) — CANCEL 은 CDR `cancelled` |
| `void Tick()` | CspServer 1 s 루프 | `MaxPlayMs`+3 s 지난 대기 → `timeout` 최종 응답 |

호별 상태는 서비스 안의 맵(`m_mapCalls` A Call-ID → {relay, peer, play_id, situation, 최종 코드·Reason, 시작 시각, media}, `m_mapPlayToCall`, `m_mapHold`) —
CallMap 은 relay 서술자와 `m_strAnnProfile` 만 갖는다. 링백 entry(최종 코드 0)의 `bEarlySent` 가 "CSP 가 만든 183 answer 를 이미 냈다"는 표식이라
뒤따르는 실패 안내가 183 을 다시 내지 않는다. **락 규약**: 맵은 `m_mtx`, SIP·CMP 호출은 락 밖(`StopCall` → `EventCallEnd` → `OnCallEnd` 재진입).

### 5.2 발화 지점 정리 (현행 코드 → 변경)

| 현행 | 변경 |
|---|---|
| `EventCallEnd`: `TryRerouteLeg` 실패 뒤 `StopCall(A, RelayEndStatus(status), reason)` | 사이에 `if (m_clsAnn.OnLegFailed(...)) return;` |
| `EventIncomingCall`: `StopCall(callId, 484/480/603/488…)` 각 지점 | `m_clsAnn.Reject(callId, rtp, code, reason)` |
| `TryRerouteLeg` 소진·`TRANSCODE_CAPACITY` 488·CMP `NO_RESOURCE` | `congestion` 으로 `OnLegFailed`/`Reject` |
| `EventReInvite`: 주소 MODIFY + `SendReInvite` 통과 | 방향 감지 → `OnHold`/`OnResume`(TAS 게이트 `roles.TAS`), inactive→sendonly 재작성 |
| TasModule 전달·픽업의 `RELAY_MODIFY`(4곳) | 앞에 `OnLegReplaced(relay)` — 교체되는 leg 의 보류 음악 정지 |
| TAS `ApplyTerminationServices` 603 | `Reject` 를 먼저(rtp·message 인자 추가) |
| `EventCallRing`: 18x 브릿징 | 프로파일 ringback on 이면 `OnRingback`; SDP 있는 18x·200 은 `OnRingbackEnd` 뒤 현행 C1a |
| `EventIncomingCall`: 착신전환(TAS `ResolveDiversion` → B-leg 를 전환 대상으로) | 181 → relay·CallMap → History-Info 삽입 → `OnForwarded(A, B)` → `StartCall` (§3.5) |
| `CCmpClient` 이벤트 dispatch | `RELAY_PLAY_DONE` → `OnPlayDone` |

### 5.3 psip 변경

- `RingCall(callId, status, rtp, const std::vector<std::pair<std::string,std::string>>& extraHeaders)` — 18x 에 부가 헤더(`P-Early-Media: sendonly`)를 싣는
  오버로드(`SipUserAgentCall.hpp`). 기존 3-인자 시그니처는 빈 헤더 목록으로 위임.
- 서버 answer 의 방향은 `CSipCallRtp::SetDirection(E_RTP_SEND_RECV)` 로 명시(미디어 목록의 `a=` 도 함께 바뀐다).

## 6. 정책 모델 — 프로파일

### 6.1 csp.json `Setup.Announcement`

```json
"Announcement": {
  "Enable": true,
  "MaxPlayMs": 30000,
  "DefaultProfile": "default",
  "Rules": [
    { "profile": "default", "situation": "busy",       "mode": "tone_then_announce", "tone": "sys:busy_kr", "tone_ms": 4000, "media": "sys:ann_busy" },
    { "profile": "default", "situation": "no_answer",  "mode": "announce", "media": "sys:ann_no_answer" },
    { "profile": "default", "situation": "hold",       "mode": "media",    "media": "sys:moh_simple", "loop": true },
    { "profile": "default", "situation": "forwarded",  "mode": "announce_then_tone", "media": "sys:ann_forwarded", "tone": "sys:ringback_kr" },
    { "profile": "trunk",   "situation": "busy",       "mode": "none" }
  ]
}
```

- **프로파일 표 = `Rules` 행의 집합** — 행 하나가 프로파일 하나의 상황 하나(`{profile, situation, mode, tone, tone_ms, media, repeat, loop}`). 콘솔은 `object_list` 로 편집한다
  (맵-오브-맵은 콘솔 필드 형식에 없다). 같은 프로파일에 없는 상황은 `DefaultProfile` 의 값, 그것도 없으면 `none`.
- `Rules` 가 비면(키 없음·빈 배열) **내장 기본 표** — `default`(§2 표의 기본 동작 그대로 — busy 화중음 4 s→안내, no_answer/unreachable 안내, not_found/invalid 없는번호 안내,
  declined 화중음 6 s, congestion 혼잡음 6 s, hold moh_simple loop, **forwarded = announce_then_tone ann_forwarded → ringback_kr**(§3.5), ringback/forbidden/call_waiting none) + `trunk`(전부 none — NNI 관례) +
  `ringback`(서버 링백 스위치 — `ringback: media sys:ringback_kr loop` 한 행, 나머지 상황은 `default` 로 떨어진다. 접속서비스 `announcement_profile: ringback` 이
  §3.4 를 켜는 가장 짧은 길이고, 음원은 §6.3 가입자 값이 덮는다).
- `tone_ms` 는 신호음 loop 길이(신호음 파일은 주기 1~2회 분량이라 CMP 가 항목 `repeat 0 + max_ms` 로 돈다). `media` 의 `repeat` 기본 1, hold/ringback 은 `loop`.
- 검증: 모르는 상황·mode 는 건너뛰거나 `none` 으로 낮추고 ERROR 로그(설정 오류가 통화 장애로 번지지 않게). `Enable=false` 또는 CMP `resource.ann` 미광고 = 전 상황 `none`.
- 콘솔 편집 = `config_template.json` 섹션 `announcement`(scope service). SIGUSR1 재로드(`CCspAnnouncementService::Init`).

### 6.2 접속서비스 override (`access_services`)

| 필드 | 뜻 |
|---|---|
| `announcement_profile` | 이 서비스 가입자가 **발신자**일 때 적용할 프로파일 이름(실패 안내·서버 링백은 발신자가 듣는다). 비면 `DefaultProfile` |
| `hold_profile` | 이 서비스 가입자가 **피보류자**일 때의 프로파일(hold 만 본다). 비면 `announcement_profile` → `DefaultProfile` |

피어(트렁크) leg 가 발신자면 인바운드 Route 의 RemoteNode 정책 `announcement_profile`(예 `trunk` — 피어에는 안내를 내지 않고 코드만 넘기는 것이 NNI 관례,
TS 29.165) 를 본다. 어느 것도 없으면 `DefaultProfile`.

### 6.3 가입자 단위 — 링백 음원

**해석 순서**(ringback 상황): ① 피착신 가입자 `ringback_media` → ② 접속서비스 프로파일 → ③ 전역 기본 프로파일. 스위치는 서비스(프로파일 `ringback` 이 `none` 이면
가입자 값이 있어도 재생하지 않는다), 음원은 가입자.

- 대상은 **`ringback` 음원 하나**(개인 컬러링 — 피착신 가입자가 고른 음원을 발신자가 듣는다). 실패 안내·보류 음악은 사업자 정책이라 가입자 단위로 두지 않는다.
- 자료 = 가입 테이블(`volte_subscriptions`/`voip_subscriptions`) `ringback_media VARCHAR(64) NULL`(`sql/migrate_subscription_ringback.sql`, 재실행 안전). 값 = 라이브러리 음원 id
  `sys:<name>`·`op:<name>`(·`sub:<name>` 예약). CSC `POST/PUT /users/{pid}/{volte|voip}` 의 `ringback_media`(형식 검사, 컬럼 없으면 400 `schema_not_migrated`)가 쓰고
  목록·단건 응답에 실린다. CSP 는 `DbManager` 가 컬럼 존재를 기동 때 확인해 `LoadAllUsers`/`SelectUser` 로 `CspUser::m_strRingbackMedia` 에 읽는다(USER_CHANGED 반영).
- `CCspAnnouncementService::OnRingback` 이 프로파일 동작을 고른 뒤 피착신 `CspUser` 의 값이 있으면 `media` 만 바꾼다. 가입자별 WAV 업로드(`sub:` 등록)는 후속 — 지금은 라이브러리 id 선택.

## 7. 서비스 음원 라이브러리

계측기 샘플 라이브러리와 **형식·변환기만 같고 실체는 별개**다 — 카탈로그·저장소·API·콘솔 화면·삭제가 서로 독립이다.

### 7.1 형식·카탈로그

- 마스터 = 16-bit PCM 16 kHz mono WAV. 변환기 `cims-sample-conv`([test_instrument.md §4](test_instrument.md) — 소스 `tester/sampleconv`, 바이너리는 `oam-svc`
  패키지 `native/` 에도 동봉)가 `pcmu`·`pcma`·`g722`·`amrwb` 4종을 만든다. **안내·신호음·MOH 는 DTX 끔**(무음 구간도 프레임 송출 — 단말 jitter buffer·NAT 바인딩 유지).
  레벨 = 안내 P.56 활성 −26 dBov, 신호음 −16 dBov(≈ −13 dBm0, E.180), MOH −20 dBov.
- 음원 id = `<scope>:<name>` — `sys:`(패키지 동봉 기본 세트, 삭제 불가) · `op:`(운영자 등록) · `sub:<subscription_id>`(가입자 링백 — P2). `name` 은 `[a-z0-9_]{1,40}`.
- 카탈로그는 두 파일이다: CMP 패키지의 `announcements/sys/catalog.jsonl`(동봉 세트, 불변) + `config/announcements.jsonl`(운영자 등록 — OAM 이 agent collection 으로 내린다). CMP 가 둘을 합쳐 적재하고 네임스페이스가 달라 충돌하지 않는다.
- 카탈로그 `announcements.jsonl`(행 = 음원 하나) — CMP collection 이자 OAM 라이브러리의 목록:

```jsonl
{"id":"sys:busy_kr","kind":"tone","description":"화중음 480+620 Hz 0.5/0.5 s","duration_ms":2000,"loop":true,"files":{"amr-wb":"sys/busy_kr.amrwb","pcmu":"sys/busy_kr.pcmu","pcma":"sys/busy_kr.pcma","g722":"sys/busy_kr.g722"},"sha256":{"amr-wb":"…"}}
{"id":"op:ann_holiday","kind":"announcement","description":"휴무일 안내","duration_ms":7200,"loop":false,"files":{…},"registered_at":"2026-09-21T09:00:00Z","registered_by":"console:admin"}
```

`kind` = `tone`|`announcement`|`music`. `sha256` 로 노드 배포 대조(이름+크기가 아니라 내용 — 같은 이름 재등록을 허용하기 위해).

### 7.2 기본 세트 (`sys:`)

레포 `media/announcements/pcm/*.wav`(마스터) + `gen_announcements.py`(변환기 `cims-sample-conv --no-dtx`, 산출물 `media/announcements/sys/*.{pcmu,pcma,g722,amrwb}` + `catalog.jsonl`(sha256 포함) — 커밋) →
CMake `dist` 가 `dist/cmp/announcements/sys/` 에 복사하고, OAM 패키지에는 카탈로그(`announcements/sys_catalog.jsonl`)와 마스터(`announcements/sys/*.wav`, 콘솔 청취)·변환기(`native/cims-sample-conv`)를 넣는다. 마스터는 계측기 `tester/worker/samples/pcm/` 에서 **복사해 독립**시킨다(계측기 샘플 수정이
서비스 음원을 바꾸지 않게).

| id | kind | 내용 |
|---|---|---|
| `sys:dial_kr`·`ringback_kr`·`busy_kr`·`congestion_kr`·`call_waiting_kr` | tone | 한국 신호음(E.180 Sup.2·전기통신설비 기술기준) |
| `sys:ann_connecting`·`ann_hold`·`ann_call_waiting`·`ann_busy`·`ann_no_answer`·`ann_invalid_number`·`ann_forwarded` | announcement | TTS 한국어 안내 7종(`ann_forwarded` = "전화가 다른 번호로 연결됩니다. 잠시만 기다려 주십시오." — 착신전환 §3.5) |
| `sys:moh_simple` | music | 합성 보류 음악 |

### 7.3 OAM — 등록·삭제·배포 (base OAM 소유)

배포 자산 분배(패키지·컬렉션과 같은 평면)라 **base OAM**(`ems/core/oam`)이 소유한다 — agent 토큰·배포 레코드가 base 에 있다. 서비스 모듈(oam-svc)이 아니다.

- **저장소** = 관리 store `<store>/announcements/` : `catalog.jsonl`(op 행) + `op/<name>.wav`(마스터, 청취) + `op/<name>.{pcmu,pcma,g722,amrwb}`. 동봉 세트 표시용 카탈로그·마스터는 OAM 패키지 `announcements/`.
- **API**(`ems/core/oam/src/handlers/announcements.py`, `/api/v1/announcements`) : `GET`(목록 `media[]`, `?nodes=1` 이면 CMP 노드별 보유 상태) · `POST ?id=&kind=&description=&loop=&normalize=&replace=`
  (본문 octet-stream WAV — 게이트웨이가 multipart 를 JSON 으로 환원하므로 계측기와 같은 규약; `services/announcements.register` 가 변환기로 4 코덱 생성, DTX 끔) · `GET /nodes` ·
  `POST /deploy {ids?}` · `GET /{id}` · `GET /{id}/master.wav` · `GET /{id}/files/{codec}` · `DELETE /{id}[?undeploy=1]`. 권한 GET=monitor·POST=operator·DELETE=manager.
  삭제는 참조 검사를 하지 않는다 — 참조 중이던 프로파일의 안내는 `MEDIA_NOT_FOUND` 폴백(응답 코드만)으로 드러난다.
- **배포** = OAM → agent(sync REST) → CMP install_path : 음원 파일은 신설 `PUT /module-file?install_path=&path=announcements/op/<file>`(바이너리 ≤ 64 MB, atomic, `announcements/` 밖·`..` 거부),
  대조는 `GET /module-files?install_path=&dir=announcements/op`(name·size·sha256 — 라이브러리 지문과 비교해 **없거나 다른 파일만** 올린다), 카탈로그는 기존
  `PUT /collection?name=announcements`(`signal:true` → SIGUSR1 → CMP 재적재). CMP 배포 레코드 = package name `cmp`(없으면 process_name `CMP`·설치 경로 `/cmp/`).
  `DELETE /module-file` 로 걷는다(`?undeploy=1`). 노드 상태 = `ok|partial|missing|unreachable`.
- **CMP 쪽 자리** = 배포 레이아웃 `<install_path>/`(버전 디렉토리) 아래 `config/announcements.jsonl`(카탈로그 컬렉션) + `announcements/op/*`(파일). CMP 는 자기 설정 파일
  위치(`<install_path>/cmp/config/cmp.json`)에서 두 층 위를 install_path 로 보고 그 둘을 읽는다(`PCmpServer::annInstallRoot` — `config/`·`cmp/` 가 있는 배포 레이아웃일 때만,
  스크래치 실행은 모듈 자기 `config/announcements.jsonl`·`announcements/` 폴백). 카탈로그 행의 `files` 경로는 `op/<file>`(운영자 루트 기준). agent 는 모듈 **버전 업그레이드 때
  collection jsonl 과 함께 `announcements/` 를 새 버전으로 이어받는다**(둘 중 하나만 옮기면 `MEDIA_NOT_FOUND`).
- **콘솔** `/service/announcements`(CIMS 서비스 팩, 서비스 섹션) : 목록·청취(마스터 WAV 를 인증 fetch → Blob)·WAV 등록(P.56 정규화 권장값 안내 -26·신호음 -16·음악 -20)·
  삭제(노드 파일도 걷음)·[CMP 배포] + 노드별 보유 띠. `Setup.Announcement.Rules` 의 tone/media 는 이 id 를 쓴다.
- 설정 `Announcements.SampleConv`(변환기 경로 — 기본 패키지 `native/`, 개발 트리 `build/bin`).
- `oam-cims-tester` 의 `/api/v1/tester/samples`·`/test/samples` 는 그대로다 — 서로 참조하지 않는다.

## 8. NAT·SRTP·트랜스코딩·HA 상호작용

- **NAT**: 3.1 의 sendrecv answer 로 단말이 RTP 를 보내 latch 가 선행한다. latch 전에 낸 프레임은 선언 주소로 나가 유실될 수 있다 — 발신 leg 가 `remote_nat=1`
  이면 CMP 는 **latch(또는 첫 ingress) 까지 최대 `AnnNatWaitMs`(기본 500 ms) 재생 시작을 미룬다**(프레임을 버리지 않고 시작점을 늦춤). 미도착이면 선언 주소로 시작(`PAnnPlayer` NAT 게이트).
- **SRTP**: leg 컨텍스트로 protect — 키 변경(re-INVITE 재키잉)은 relay 와 같이 세션 재생성, 재생기는 SSRC 유지.
- **트랜스코딩 호**(cmp.md §11): 재생 leg 의 코덱 = 그 leg 선언(`media_codec`) — 변환 유닛과 무관하게 leg 코덱 파일을 낸다.
- **CMP All-Active**(csp.md §3.6): 재생기는 relay 세션과 같은 endpoint(session-sticky). endpoint DEAD 시 현행 능동 종료 경로가 호를 정리한다.
- **CSP HA**: 재생 대기 상태(`final_pending`)는 A-leg 다이얼로그와 함께 사라진다 — standby 는 `RELAY_PLAY_DONE` 을 받아도 호를 모른다(ack 만). 현행 relay 와 같은 한계.

## 9. 관측·통계·CDR

- `call.json`(CallDir) `announcement: { "situation": "busy", "media": "sys:busy_kr,sys:ann_busy", "played_ms": 8760, "result": "completed|max|stopped|cancelled|timeout" }` —
  `VoipCallAnnouncement`. `end_status`·`end_reason` 은 불변(자체 거절은 `VoipCallRejected` 의 시도 기록 위에 붙는다) — 통계 3계층 정의 무변경.
- CSP 로그 `Announcement: <situation> → <action> play=… final=<code>` / `done(<result>, <ms>) → final <code>` / 폴백 ERROR(`RELAY_PLAY rejected`·`RELAY_ADD failed`·`cannot build early answer`).
  서비스 카운터 `m_lStarted`/`m_lFallback`(SipStats·콘솔 노출은 후속).
- CMP STATS: `resource.ann{total,used,media}`, `detail.ann[]`, `detail.ann_catalog{ids, missing}`. Flow 로그 `INT ANN_PLAY`/`ANN_DONE`.
- 계측기: 워커가 **망이 낸 183+SDP** 도 `progressTx` 로 잡아 200·실패 최종 응답 때 수신 RTP(≥ 5 패킷)를 판정한다 → `early_media_pct`·`early_rtp_pct` 가 안내 도달률이 된다
  (`invite` 단계 expect 에 허용). 동봉 시나리오 `VOLTE-ANN-NOTFOUND`(없는 번호 → 404)·`VOLTE-ANN-NO-ANSWER`(착신 480 → 안내 → 480). 별도 `ann_pct` 지표는 두지 않는다.
  **보류 음악** = `hold` 단계가 피보류 단말의 RTP 수신 누계를 기준점으로 적고 `resume`(없으면 `bye`·인스턴스 종료)에서 증분 ≥ 5 패킷을 `moh_rtp_ok` 로 센다 →
  `moh_rtp_pct` = `moh_rtp_ok`/`hold_tx`(보류 주체가 송출 중이면 그 RTP 도 섞이므로 시나리오는 `rtp: explicit` 로 둔다) — `VOLTE-ANN-HOLD-MOH`.
  **서버 링백·가입자 링백** = 픽스처로 발신자 서비스 복제본에 `announcement_profile: ringback`(내장 프로파일)을 주고, 착신자 회선의 `ringback_media`(subscriber 픽스처)로
  음원을 바꾼다 → `VOLTE-ANN-RINGBACK`·`VOLTE-ANN-RINGBACK-SUB`(200 전 `early_media_pct`/`early_rtp_pct`, 음원 선택은 CSP 로그·CDR 로 확인).
  **착신전환 안내** = subscriber 픽스처 `forward_to: <역할>`(served 회선 `forward_id` = 그 역할의 신원) → `VOLTE-ANN-FORWARDED`: 발신자의 181(`cdiv_181_pct` = `cdiv_181_rx`/`invite_tx`)·
  전환 대상 착신 INVITE 의 History-Info(`cdiv_hi_pct` = `cdiv_hi_rx`/`invite_tx` — libcsim `OnIncomingDiverted`)·안내+링백 RTP(`early_media_pct`/`early_rtp_pct`). CDR `diversion{served,target,cause,hops}`.
- 검증 게이트: `S1-UNIT-CMP`(`tests/cmp_ann_player_test.cpp`) · `S3-SCN-ANN`(cspsim 없는 번호 → `=> RTP(` 뒤 ≥ 5 s → `status=404`) · `tests/cmp_smoke_announcement.py`(라이브 CMP).

## 10. 알람 ([../alarm_catalog.csv](../alarm_catalog.csv) 정본)

| code | type | severity | 감지 | 조건 |
|---|---|---|---|---|
| A-PRC-034 | `media_missing` | major | CMP | 카탈로그가 참조하는 파일 누락·형식 오류(적재 때) — mo `<서버>/cmp/ann/catalog`, params `count·first·root`. 해소되면 close |
| A-QOS-002 | `resource_exhausted` | major | CMP | 풀 고갈 알람의 `ann_pool` 감지 행 — 재생기 슬롯(`AnnPlayers`) 전부 사용(`ANN_CAPACITY` 거절) |

`A-COM-007`(CMP 두절)의 영향 문구는 이미 "안내방송" 을 포함한다 — 정의 변경 없음. 음원 등록·삭제·배포의 감사 이벤트는 후속.

## 11. P2 — 후속 상황

- **통화중대기(TS 24.615) — 시그널링 축은 구현**: `EventIncomingCall` 이 착신 가입자의 확립 호(`CCallMap::HasEstablishedCallFor`)를 보면 가입자 B-leg INVITE 에
  `Alert-Info: <urn:alert:service:call-waiting>`(RFC 7462 — 단말이 대기음을 낸다)을 싣고 `CCallInfo::m_bCallWaiting` 을 켠다. 발신자에게는 B 의 SDP 없는 18x 에서
  `OnRingback(…, ANN_SIT_CALL_WAITING)` — 프로파일 `call_waiting`(`mode: media|announce`, 예 `sys:ann_call_waiting` loop) 규칙이 있으면 그것으로, 없으면 `ringback` 규칙으로
  183+SDP 안내/링백을 들려준다(기본 표는 둘 다 none — 배치가 켠다). 피어 착신·PTT 는 대상이 아니다.
  **남은 것** = 단말이 대기음을 못 내는 배치를 위한 **망 in-band 대기음**(활성 통화에 톤을 섞는 `mode=mix` — 디코드·믹스·재인코딩, 변환 슬롯 소비)과 실단말 실측.
- **착신전환(TS 24.604) 후속**: CFU 서버측 전환·`forwarded` 안내는 구현(§3.5·§12 ⑪). 남은 것 = 조건부 전환 CFB/CFNR/CFNL(가입 필드 `forward_busy_id`·`forward_no_reply_id`·`no_reply_sec`,
  B-leg 486/무응답/미등록 때 같은 재타게팅 경로 — History-Info cause 486/408/404) · 전환자(diverting user) 통지(TS 24.604 comm-div-info 이벤트 패키지) · `Privacy: history`.
- **다이얼로그 이전 거절**(3.2)의 판정 지점 이동 · **가입자 링백 WAV 업로드**(`sub:` 등록 — 6.3 은 라이브러리 id 선택까지 구현) · **다국어 세트**(프로파일을 언어별로 두면 된다 — 모델 변경 없음).
- **관제 큐/ACD**([dispatch_center.md §10](dispatch_center.md)): 대표번호 대기열의 "잠시만 기다려 주십시오"·순번 안내는 3.4 의 링백 재생기(repeat 0)에
  시퀀스 교체(RELAY_PLAY 교체 = `replaced`)를 얹는 것이다.
- `mix` 모드·런타임 인코딩(PCM 마스터 → leg 코덱, 변환 슬롯 사용)·영상 안내.

## 12. 구현 상태 (P1)

| 단계 | 구현 | 검증 |
|---|---|---|
| ① 음원 세트 | `media/announcements/`(마스터 12 종 + `gen_announcements.py` → `sys/*.{pcmu,pcma,g722,amrwb}` + `catalog.jsonl`) · CMake `dist/cmp/announcements/sys`, `dist/oam/announcements` | 파일 4 코덱 × 12, sha256 |
| ② CMP 재생기 | `PAnnCatalog`·`PAnnPlayer`·`PAnnTicker`·`PRtpRelay::annTick`·`RELAY_PLAY/_STOP`·`RELAY_PLAY_DONE`·`ANN_RELOAD`·SIGUSR1·`resource.ann`·STATS·A-PRC-034·`ann_pool`·cmp.json 4 키·template 섹션+collection | `S1-UNIT-CMP` 40 항목 pass · `tests/cmp_smoke_announcement.py` pass(150 pkt/3 s·20 ms 페이싱·DONE max/stopped/replaced·AMR-WB octet-aligned·재적재) |
| ③ CSP | `CspAnnouncement`·`Setup.Announcement`(Rules)·접속서비스 2 필드·RemoteNode `announcement_profile`·`CCallInfo::m_strAnnProfile`·발화 지점(EventCallEnd/EventIncomingCall/TAS 603/EventReInvite/EventCallRing/EventCallStart/TAS RELAY_MODIFY 4곳)·psip `RingCall` 헤더 오버로드·CDR `announcement` | 스크래치 CSP+CMP+cspsim 실측(2026-09-21): 없는 번호 → 183+SDP → 6.16 s 안내 → **404**, CANCEL 중 → STOP → 487, 무응답(Timer B) → 안내 6 s → **408**, relay 즉시 회수, CDR `announcement` 기록 |
| ④ 스모크 | `S3-SCN-ANN`(`verify/lib/items/stage3/scn_announcement.py`) | 게이트 등록(dev 스택 실행은 배포 정지창) |
| ⑤ 계측기 | 워커 RING 183+SDP → `progressTx`, 실패 최종 응답에 early RTP 판정, `invite` expect 에 `early_media_pct/early_rtp_pct`, 시나리오 `VOLTE-ANN-NOTFOUND`·`VOLTE-ANN-NO-ANSWER` · 보류 음악 지표 `moh_rtp_pct`(워커 `evalMoh`, `hold`/`resume` expect) + `VOLTE-ANN-HOLD-MOH` · 서버/가입자 링백 `VOLTE-ANN-RINGBACK`·`-RINGBACK-SUB`(subscriber 픽스처 `ringback_media`, 내장 프로파일 `ringback`) | tb48 실측 §9 ⑨·⑩ |
| ⑥ OAM 라이브러리 | base OAM `services/announcements.py`·`handlers/announcements.py`(`/api/v1/announcements`)·agent `/module-files`·`/module-file`(GET/PUT/DELETE)·`_agent_proxy_call raw`·콘솔 `/service/announcements`(`AnnouncementsPage`)·`Announcements.SampleConv` | 서비스 오프라인 시험(등록·중복 409·삭제·sys 보호) pass · 콘솔 tsc pass · 노드 배포는 agent 0.2.103 배포 뒤 실측 |
| ⑦ 문서 | 본 문서 · cmp_media_api §1.1/§5.1/§5.4/§6.7/§8/§9 · cmp.md §1.1/§9/§12 · csp.md §1.1/§3.1/§3.12/§6.0 · volte_flows C9/C10 · sip_service_model §2-2/§2-9 · agent_api Sync REST · alarm_catalog A-PRC-034·A-QOS-002 ann_pool · VERIFICATION_PROCESS S3-SCN-ANN · CLAUDE.md | — |

| ⑧ P2 선반영 | 통화중대기 시그널링(Alert-Info + 발신자 `call_waiting` 안내/링백) · 가입자 링백(`ringback_media` 컬럼·CSC API·CSP 해석) · 감사 이벤트 E-AUD-017(등록·삭제·배포) · CSP 모니터 `MC_SIP_STATS` 에 `ann_started/ann_fallback/ann_active_*` | 빌드·단위시험 pass · 라이브 CSP 로그에 통화중 착신의 `call waiting (Alert-Info)` 판정 확인 |
| ⑨ 라이브 배포 실측 (.48 관리평면, 2026-09-21) | cmp 0.2.93 · csp 0.2.141 · csc 0.2.118 · oam 0.2.152 · agent 0.2.104 · oam-cims-tester 0.1.22 · cims-tester-worker 0.1.17 | 없는 번호 → 183+SDP → 6.16 s 안내 → 404(cspsim) · 계측기 `VOLTE-ANN-NOTFOUND` 4/4 pass · `VOLTE-ANN-NO-ANSWER`(착신 480 → 안내 → 480, early_media/early_rtp 100 %) 4/4 pass · 라이브러리 등록 → `/deploy`(파일 4 + 카탈로그 + SIGUSR1) → CMP STATS `ann_catalog.ids` 에 `op:` 포함(13 media) → `/nodes` presence ok → 재배포 idempotent(pushed []) → `DELETE ?undeploy=1` 로 노드 파일 회수·12 media · 감사 E-AUD-017 3건 |

| ⑩ 보류 음악·링백 실측 (.48 관리평면, 2026-09-22) | csp 0.2.144 · csc 0.2.121 · cmp 0.2.94 · oam-cims-tester 0.1.23 · cims-tester-worker 0.1.19 — 실측이 드러낸 결함 수정 포함: psip 세션 갱신 판정에 방향 속성 추가(hold re-INVITE 가 "미디어 무변경" 으로 건너뛰어졌다) · psip media-list SDP 에 다이얼로그 방향 재작성(`HoldCall` 의 a=sendonly 가 와이어에 없었다) · CSC `/users` 목록 500(ptt 테이블 `ringback_media` SELECT) · CSC PUT 회선의 같은 realm 서비스 이관 passwd 요구 완화 · CSC PUT `ringback_media` 저장 누락 | `VOLTE-ANN-HOLD-MOH` 4/4(`moh_rtp_pct` 100 %, CSP `hold music sys:moh_simple → peer1` → resume 에 stopped 3.98 s) · `TRUNK-PBX-HOLD-RESUME` 4/4(PBX 보류 → 발신 UE 에 MOH 4/4) · `VOLTE-ANN-RINGBACK` pass(발신자 서비스 `announcement_profile: ringback` → 18x+SDP·early RTP 149 패킷 → 200 에 stopped) · `VOLTE-ANN-RINGBACK-SUB` pass(피착신 `ringback_media=sys:ann_connecting` 이 음원을 덮음 — CSP 로그 확인) · 회귀 NOTFOUND·NO-ANSWER·XFER-DENIED·CALL-BASIC pass · cmp 0.2.93→0.2.94 업그레이드 때 `announcements/op/*`·`config/announcements.jsonl` 이어받기 확인(STATS `op:` 포함 13 media → 시험 등록물 undeploy 뒤 12) |

| ⑪ 착신전환 안내 (TS 24.604 CDIV) | 서버측 전환 — TAS `ResolveDiversion`(CFU 연쇄·상한 `Setup.Sip.Cdiv.MaxDiversions`·루프 486·DB 폴백·다이얼 플랜 번역) · 디스패처 재타게팅(181 `Setup.Sip.Cdiv.Notify181`·전환 대상 라우팅 재판정 `DecideOutboundRoute`·B-leg `History-Info`/`Supported: histinfo`·CDR `diversion`) · `CspDiversion`(RFC 7044/4458 순수 헬퍼) · 상황 `forwarded`·모드 `announce_then_tone`·`OnForwarded`·2 단계 링백 · 음원 `sys:ann_forwarded` · 302 경로 제거 · 계측기 `VOLTE-ANN-FORWARDED`(픽스처 `forward_to`, 지표 `cdiv_181_pct`·`cdiv_hi_pct`, libcsim `OnIncomingDiverted`) · CSC PUT 부분 업데이트(dnd/forward_id 키 있을 때만)·`forward_id` 형식 검사 (cmp 0.2.95(`sys:ann_forwarded` 동봉) · csp 0.2.146 · csc 0.2.122 · tester 0.1.24 · worker 0.1.20) | `S1-UNIT-CSP` `tests/csp_diversion_test.cpp` 16 checks · tester 단위시험 52 pass · 콘솔 tsc pass · **tb48 실측(2026-09-22)** `VOLTE-ANN-FORWARDED` 10/10 pass — CSP `CDIV +8213…26 → +8213…39 (hops=1)` → 181(`cdiv_181_pct` 100) → 183+SDP · `sys:ann_forwarded` 5540 ms completed → `ringback sys:ringback_kr(loop)` → 전환 대상 200 에 stopped(early RTP 350 패킷, `early_rtp_pct` 100) · 전환 대상 INVITE 의 `History-Info: <sip:+8213…26@…>;index=1, <sip:+8213…39@…;cause=302>;index=1.1;mp=1`(`cdiv_hi_pct` 100) · CDR `diversion{served,target,cause:302,hops:1}`+`announcement{forwarded, completed}` · 회귀 CALL-BASIC·ANN-NOTFOUND·ANN-RINGBACK·ANN-HOLD-MOH pass. 첫 실측이 드러낸 것 = 구 CMP 에 음원이 없으면 183 을 낸 뒤 RELAY_PLAY 가 거절돼 A 가 무음 → **RELAY_PLAY 수락 뒤 183** 으로 순서 고정(§3.1) |

**남은 것(P1 실측)** = 실단말 hold/링백 청취 확인, `S3-SCN-ANN` 게이트 실행(dev 스택은 배포 모드), 착신전환 라이브 실측(`VOLTE-ANN-FORWARDED`). CSP 안내 카운터는 모니터 `MC_SIP_STATS`(ann_started/ann_fallback/ann_active_*)로 본다 — OAM 은 모니터를 수집하지 않으므로 콘솔 노출은 통계 파이프라인 과제로 남긴다.
안내 없는 구 CMP 와 새 CSP 의 혼용은 `resource.ann` 미광고로 안전하게 폴백한다.
