# 안내음성·신호음 — 상황별 네트워크 안내(announcement)·링백·보류 음악

> CSP(제어)·CMP(재생)가 통화 상황에 맞는 안내음성·신호음·보류 음악을 단말에 **내보내는** 기능의 정본 설계.
> 규격 = TS 24.628(공통 기본 통신 절차 — early media·안내), TS 24.229 §5.7(AS)·TS 23.228 §4.7(MRFC/MRFP),
> RFC 3960(early media·링백 모델), RFC 5009(P-Early-Media), RFC 4240(netann 의미론), RFC 3326(Reason),
> TS 24.610(HOLD — 피보류자 안내), TS 24.615(통화중대기), RFC 7462(Alert-Info URN), ITU-T E.182(톤·안내 적용 원칙),
> E.180 Sup.2(한국 신호음). 구현 상태는 §12 이행 순서가 정본이다 — P1 착수 전.
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
**P2** = 통화중대기(TS 24.615)·서버 링백(컬러링) 켜기·착신전환 안내(TS 24.604)·믹스 모드·관제 큐/ACD 대기열 안내.

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
| `forwarded` 전환 안내 | 착신전환 실행 | TS 24.604 §4.5.2.x | **P2** — 현행 302 모델에는 삽입 지점이 없다(§11) |

**상황 판정 우선순위.** B-leg 최종 응답에 `Reason: Q.850;cause=N` 이 있으면 cause 로 먼저 판정하고(RFC 3326 — 피어·MGCF 가
PSTN 원인을 실어 준다), 없으면 SIP 코드로 판정한다. 판정 표는 `CspAnnouncement::Classify(status, reason)` 하나에 둔다.

**동작(`mode`) 어휘** — E.182 원칙(톤 먼저, 안내는 톤으로 뜻이 전달되지 않을 때):

| mode | 뜻 |
|---|---|
| `none` | 안내 없음 — 응답만(현행 동작) |
| `tone` | 신호음만 `tone_ms` 동안(loop) |
| `announce` | 안내음 1회(`repeat` 회) |
| `tone_then_announce` | 신호음 `tone_ms` → 안내음 |
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
- 이미 B 의 18x+SDP 로 early media 가 앵커링된 뒤 실패하면([volte_flows.md](volte_flows.md) C1a) 183 을 다시 내지 않고 RELAY_PLAY 만 한다
  — peer0 의 answer 는 그대로다(B 가 준 SDP 를 CSP 가 A 에게 이미 냈으므로 코덱도 그것이다).
- **최종 응답은 재생 뒤** `RELAY_PLAY_DONE` 또는 `MaxPlayMs` 타이머에서 `StopCall(A, 원코드, Reason)` — `RelayEndStatus` 매핑·Reason 투과 규칙은
  현행 그대로다([csp.md §3.1](../modules/csp.md) `EventCallEnd`). A 의 **CANCEL** 이 재생 중 오면 `RELAY_PLAY_STOP` 뒤 487(psip 기본).
- CMP 가 `resource.ann` 을 광고하지 않거나(`AnnPlayers=0`) RELAY_PLAY 가 `MEDIA_NOT_FOUND`/`ANN_CAPACITY` 로 거절되면 **즉시 원코드**
  — 안내가 실패를 가리지 않는다. 카운터 `ann_fallback` + LOG_ERROR(음원 누락은 알람 §10).
- 통계 무영향: 최종 코드가 그대로라 시도/세션/leg 3계층·성공률 정의가 바뀌지 않는다. CDR(`call.json`)에 `announcement{situation, media[], played_ms}`
  만 더한다(§9).

### 3.2 실패 안내 — B leg 이전 실패 (404·484·480 미등록·603 DND)

`EventIncomingCall` 이후 psip UAS 다이얼로그가 있으면 같은 절차다: `RELAY_ADD`(peer0=A 만, peer1 미확정 `0.0.0.0:0`) → 183 → RELAY_PLAY → 원코드.
현행 `StopCall(callId, code)` 호출 지점(다이얼 플랜 484·미등록 480·private call 480·`ApplyTerminationServices` 603)을 `m_clsAnn.Reject(callId, rtp, code, reason)`
로 바꾼다 — 정책이 `none` 이거나 조건이 안 맞으면 그 안에서 종전처럼 `StopCall`.

**다이얼로그 이전 거절**(`RecvRequest` 단계 — `ScreenInvite` 603, 다이얼 플랜 484 의 일부)은 early media 를 낼 다이얼로그가 없다. 안내 대상에 넣으려면
판정 지점을 `EventIncomingCall` 로 옮겨야 하며 이는 시도 장부 기록 시점과 얽히므로 P1 에서는 **응답만**(현행)으로 두고 P2 에서 옮긴다(§11).

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

- 감지는 `EventReInvite` 의 offer 방향(psip `ERtpDirection`) — `sendonly`/`inactive` = hold, `sendrecv` = resume. re-INVITE 자체는 현행대로 통과시킨다
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
| `media` | O | 음원 id 배열 — 순서대로 재생(`["sys:busy_kr", "sys:ann_busy"]`). 항목은 `{id, repeat?, max_ms?}` 객체도 허용 |
| `repeat` | - | 전체 시퀀스 반복 수. 기본 1, **0 = STOP 까지 무한**(hold·ringback) |
| `delay_ms` | - | 반복 사이 무음(RFC 4240 `delay`). 기본 0 |
| `max_ms` | - | 총 재생 상한 — 넘으면 `reason=max`. 기본 = CMP `AnnMaxPlayMs` |
| `mode` | - | `replace`(기본 — 재생 중 반대 peer 의 relay 를 이 leg 로 보내지 않음) / `mix`(**예약** — §11 통화중대기 톤 삽입) |

응답 payload: `{ "codec": "AMR-WB/16000", "duration_ms": 9000 }`(시퀀스 1회 길이. repeat 0 이면 `duration_ms: 0`).

**RELAY_PLAY_STOP**: `session_id` + `play_id`(없거나 이미 끝났으면 `OK` — 자연 멱등). 응답 payload `{ "played_ms": N }`.

**이벤트 RELAY_PLAY_DONE**(§8 채널 — ack·1 s×5 재전송): `session_id`, `peer_index`, `play_id`, `reason`(`completed`|`max`|`stopped`|`replaced`|`error`),
`played_ms`. `stopped`/`replaced` 는 client 가 낸 명령의 결과라 이벤트를 생략해도 되지만 **통일을 위해 항상 낸다**(CSP 는 무시 가능).

**코덱 선택.** 재생 코덱 = 그 leg 의 `media_codec` 선언([§6.6](../../api/cmp_media_api.md)) → 없으면 `remote_codec`/`remote_pt` 로 추정(코덱 테이블 이름) →
둘 다 없으면 `BAD_REQUEST`. CSP 는 안내 전용 세션(3.2)의 RELAY_ADD/MODIFY 에 **항상 `media_codec`** 을 싣는다. 파일은 카탈로그의 그 코덱 항목(§7.1) —
없으면 `MEDIA_NOT_FOUND`(런타임 인코딩은 하지 않는다 — 라이브러리가 4 코덱을 모두 만들어 두는 것이 계약). `telephone-event` 는 재생과 무관.

**오류**: `NOT_FOUND`(세션) · `MEDIA_NOT_FOUND`(음원 id 또는 그 코덱 파일 없음 — 신설) · `ANN_CAPACITY`(재생 슬롯 소진 — 신설) · `BAD_REQUEST`(코덱 미확정·mode 미지원).
**자원 광고**: HEARTBEAT/STATS `resource.ann{total,used}` — 키 없음 = 미지원(`AnnPlayers=0`), CSP 는 안내를 건너뛴다. STATS `detail.ann[]` = 진행 중 재생기
(session·peer·media·played_ms). Flow 로그 `INT ANN_PLAY`/`INT ANN_DONE`(detail = media 열·reason).

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

`config_template.json` 섹션 `announcement`(scope service) + collection `announcements`(카탈로그 §7.1 — CMP 의 첫 collection).

## 5. CSP — 정책·상태 머신

### 5.1 컴포넌트 `CspAnnouncement.{h,cpp}` (`CCspAnnouncementService`)

ModuleDispatcher 가 소유하는 단일 서비스(공통 기본 통신 절차 — TS 24.628 이 TAS 보조 서비스가 아니라 기본 절차이므로 B2BUA 골격에 둔다).
TAS(hold·전달·픽업·포크)와 IBCF(피어 실패)는 이 서비스를 **호출**한다.

| API | 호출 지점 | 동작 |
|---|---|---|
| `Classify(status, reason) → Situation` | 내부 | §2 판정 표(Reason Q.850 우선) |
| `Resolve(situation, accessService, calleeUser) → Action` | 내부 | §6 프로파일 해석(가입자 → 접속서비스 → 전역) |
| `bool OnLegFailed(aCallId, info, status, reason)` | `EventCallEnd`(B 실패, 재라우팅 소진 뒤) | true = 안내 인수(호출자는 StopCall 하지 않는다). 3.1 절차 |
| `void Reject(callId, rtp, status, reason)` | `EventIncomingCall` 계열의 거절 지점 | 3.2 절차 — 정책 `none` 이면 그 자리에서 StopCall |
| `void OnHold(info, heldPeerIdx)` / `OnResume(info)` | TAS `EventReInvite` 방향 감지 | 3.3 |
| `void OnRingback(info)` / `OnRingbackEnd(info)` | `EventCallRing`(SDP 없는 첫 18x) / 18x+SDP·200 | 3.4 (프로파일 on 일 때만) |
| `void OnPlayDone(sessionId, playId, reason)` | `CCmpClient` 이벤트 dispatch 스레드 | 대기 중 최종 응답 발사 |
| `void OnCancel(callId)` / `OnLegReplaced(info, peerIdx)` | CANCEL·전달·픽업 | STOP |
| `void Tick()` | 1 s 틱 | `MaxPlayMs` 만료 → 최종 응답 |

호별 상태는 `CCallInfo::m_clsAnn { eState(idle|early_sent|playing|final_pending), strPlayId, eSituation, iFinalStatus, strFinalReason, tStart, vecMedia }`.
`early_sent` 는 "CSP 가 만든 183 answer 를 이미 냈다"는 표식이라 3.1·3.4 가 공유한다(링백 재생 뒤 실패 안내로 이어질 때 183 을 다시 내지 않는다).

### 5.2 발화 지점 정리 (현행 코드 → 변경)

| 현행 | 변경 |
|---|---|
| `EventCallEnd`: `TryRerouteLeg` 실패 뒤 `StopCall(A, RelayEndStatus(status), reason)` | 사이에 `if (m_clsAnn.OnLegFailed(...)) return;` |
| `EventIncomingCall`: `StopCall(callId, 484/480/603/488…)` 각 지점 | `m_clsAnn.Reject(callId, rtp, code, reason)` |
| `TryRerouteLeg` 소진·`TRANSCODE_CAPACITY` 488·CMP `NO_RESOURCE` | `congestion` 으로 `OnLegFailed`/`Reject` |
| `EventReInvite`: 주소 MODIFY + `SendReInvite` 통과 | 방향 감지 → `OnHold`/`OnResume`(TAS 게이트 `roles.TAS`), inactive→sendonly 재작성 |
| `EventCallRing`: 18x 브릿징 | 프로파일 ringback on 이면 `OnRingback`; SDP 있는 18x·200 은 `OnRingbackEnd` 뒤 현행 C1a |
| `CCmpClient` 이벤트 dispatch | `RELAY_PLAY_DONE` → `OnPlayDone` |

### 5.3 psip 변경

- `RingCall(callId, status, rtp, extraHeaders)` — 18x 에 부가 헤더(`P-Early-Media`) 를 싣는 오버로드. 기존 시그니처 유지.
- `CSipCallRtp` 의 로컬 방향을 answer 로 낼 때 `sendrecv` 명시(현행 기본과 같다 — 확인만).
- 변경은 `ext/psip` 스냅샷 + 단위시험(S1-UNIT-PSIP).

## 6. 정책 모델 — 프로파일

### 6.1 csp.json `Setup.Announcement`

```json
"Announcement": {
  "Enable": true,
  "MaxPlayMs": 30000,
  "DefaultProfile": "default",
  "Profiles": {
    "default": {
      "ringback":     { "mode": "none" },
      "busy":         { "mode": "tone_then_announce", "tone": "sys:busy_kr", "tone_ms": 4000, "media": "sys:ann_busy" },
      "no_answer":    { "mode": "announce", "media": "sys:ann_no_answer" },
      "unreachable":  { "mode": "announce", "media": "sys:ann_no_answer" },
      "not_found":    { "mode": "announce", "media": "sys:ann_invalid_number" },
      "invalid":      { "mode": "announce", "media": "sys:ann_invalid_number" },
      "declined":     { "mode": "tone", "tone": "sys:busy_kr", "tone_ms": 6000 },
      "congestion":   { "mode": "tone", "tone": "sys:congestion_kr", "tone_ms": 6000 },
      "forbidden":    { "mode": "none" },
      "hold":         { "mode": "media", "media": "sys:moh_simple", "loop": true },
      "call_waiting": { "mode": "none" }
    },
    "trunk": {
      "busy": { "mode": "none" }, "no_answer": { "mode": "none" }, "declined": { "mode": "none" },
      "hold": { "mode": "none" }
    }
  }
}
```

- 프로파일 = 상황 → `{mode, tone, tone_ms, media, repeat, loop}`. 상황 키가 없으면 `DefaultProfile` 의 값, 그것도 없으면 `none`.
- `tone_ms` 는 신호음 loop 길이(신호음 파일은 주기 1~2회 분량이라 CMP 가 `repeat 0 + max_ms` 로 돈다). `media` 의 `repeat` 기본 1.
- `Enable=false` 또는 CMP `resource.ann` 미광고 = 전 상황 `none`.
- 콘솔 편집은 `config_template.json` 섹션 `announcement`(scope service) — `Setup.Announcement.*`. 프로파일 객체는 JSON 편집 필드.

### 6.2 접속서비스 override (`access_services`)

| 필드 | 뜻 |
|---|---|
| `announcement_profile` | 이 서비스 가입자가 **발신자**일 때 적용할 프로파일 이름(실패 안내·서버 링백은 발신자가 듣는다). 비면 `DefaultProfile` |
| `hold_profile` | 이 서비스 가입자가 **피보류자**일 때의 프로파일(hold 만 본다). 비면 `announcement_profile` → `DefaultProfile` |

피어(트렁크) leg 가 발신자면 인바운드 Route 의 RemoteNode 정책 `announcement_profile`(예 `trunk` — 피어에는 안내를 내지 않고 코드만 넘기는 것이 NNI 관례,
TS 29.165) 를 본다. 어느 것도 없으면 `DefaultProfile`.

### 6.3 가입자 단위 — 링백 음원 (확장 자리, P1 은 훅만)

**해석 순서**(모든 상황 공통): ① 가입자 항목 → ② 접속서비스 프로파일 → ③ 전역 기본 프로파일. P1 에서 ① 이 값을 갖는 상황은 없고 구조만 둔다.

- 대상은 **`ringback` 음원 하나**(개인 컬러링 — 피착신 가입자가 고른 음원을 발신자가 듣는다). 실패 안내·MOH 는 사업자 정책이라 가입자 단위로 두지 않는다.
- 자료 자리 = 가입 테이블(`volte_subscriptions`/`voip_subscriptions`) `ringback_media VARCHAR(64) NULL`(음원 id `sub:<sid>`/`op:<id>`/`sys:<id>`) — **P2 마이그레이션**.
  CSP 는 `CspUser` 에 실어 `Resolve` ① 에서 읽는다(`CspUser.m_strRingbackMedia`, 비면 건너뜀). 콘솔·CSC 프로비저닝(`POST /users/{pid}/ringback` — WAV 업로드 →
  라이브러리 `sub:` 등록 §7 → 컬럼 기록)은 P2.
- 접속서비스 `ringback.mode=media` 가 켜져 있어야 ① 이 의미를 갖는다(스위치는 서비스, 음원은 가입자).

## 7. 서비스 음원 라이브러리

계측기 샘플 라이브러리와 **형식·변환기만 같고 실체는 별개**다 — 카탈로그·저장소·API·콘솔 화면·삭제가 서로 독립이다.

### 7.1 형식·카탈로그

- 마스터 = 16-bit PCM 16 kHz mono WAV. 변환기 `cims-sample-conv`([test_instrument.md §4](test_instrument.md) — 소스 `tester/sampleconv`, 바이너리는 `oam-svc`
  패키지 `native/` 에도 동봉)가 `pcmu`·`pcma`·`g722`·`amrwb` 4종을 만든다. **안내·신호음·MOH 는 DTX 끔**(무음 구간도 프레임 송출 — 단말 jitter buffer·NAT 바인딩 유지).
  레벨 = 안내 P.56 활성 −26 dBov, 신호음 −16 dBov(≈ −13 dBm0, E.180), MOH −20 dBov.
- 음원 id = `<scope>:<name>` — `sys:`(패키지 동봉 기본 세트, 삭제 불가) · `op:`(운영자 등록) · `sub:<subscription_id>`(가입자 링백 — P2). `name` 은 `[a-z0-9_]{1,40}`.
- 카탈로그 `announcements.jsonl`(행 = 음원 하나) — CMP collection 이자 OAM 라이브러리의 목록:

```jsonl
{"id":"sys:busy_kr","kind":"tone","description":"화중음 480+620 Hz 0.5/0.5 s","duration_ms":2000,"loop":true,"files":{"amr-wb":"sys/busy_kr.amrwb","pcmu":"sys/busy_kr.pcmu","pcma":"sys/busy_kr.pcma","g722":"sys/busy_kr.g722"},"sha256":{"amr-wb":"…"}}
{"id":"op:ann_holiday","kind":"announcement","description":"휴무일 안내","duration_ms":7200,"loop":false,"files":{…},"registered_at":"2026-09-21T09:00:00Z","registered_by":"console:admin"}
```

`kind` = `tone`|`announcement`|`music`. `sha256` 로 노드 배포 대조(이름+크기가 아니라 내용 — 같은 이름 재등록을 허용하기 위해).

### 7.2 기본 세트 (`sys:`)

레포 `media/announcements/pcm/*.wav`(마스터) + `gen_announcements.py`(변환 — 계측기 `gen_samples.py` 와 같은 `convert()` 규약, TTS 캐시 규약 동일) →
CMake 가 `build/dist/cmp/announcements/sys/` 와 카탈로그 `sys` 행을 만든다. 마스터는 계측기 `tester/worker/samples/pcm/` 에서 **복사해 독립**시킨다(계측기 샘플 수정이
서비스 음원을 바꾸지 않게).

| id | kind | 내용 |
|---|---|---|
| `sys:dial_kr`·`ringback_kr`·`busy_kr`·`congestion_kr`·`call_waiting_kr` | tone | 한국 신호음(E.180 Sup.2·전기통신설비 기술기준) |
| `sys:ann_connecting`·`ann_hold`·`ann_call_waiting`·`ann_busy`·`ann_no_answer`·`ann_invalid_number` | announcement | TTS 한국어 안내 6종 |
| `sys:moh_simple` | music | 합성 보류 음악 |

### 7.3 OAM — 등록·삭제·배포

- **저장소** = OAM 관리 store `announcements/`(file_store 도메인 — [../runtime_store_design.md](../runtime_store_design.md) §1) : `catalog.jsonl` + `op/<name>.{pcmu,pcma,g722,amrwb}` + 마스터 `op/<name>.wav`(청취·재변환용).
- **API**(`oam-svc`, `/api/v1/announcements`) : `GET`(목록 + 노드별 배포 상태) · `POST ?id=&kind=&description=`(본문 octet-stream WAV — 게이트웨이가 multipart 를
  JSON 으로 환원하므로 계측기와 같은 규약) · `DELETE /{id}`(`op:` 만, 프로파일이 참조 중이면 409 `in_use`) · `GET /{id}/file/{codec}`(청취) · `POST /sync`(전 CMP 노드 배포).
- **배포** = OAM → agent(sync REST) → CMP install_path : 카탈로그는 기존 `PUT /collection?install_path=&name=announcements`(jsonl — [sip_runtime_config.md §5](sip_runtime_config.md)),
  음원 파일은 **신설 `PUT /module-file?install_path=&path=announcements/op/<file>`**(바이너리, 크기 상한 8 MB, atomic rename, `path` 는 install_path 아래
  `announcements/` 로 고정 — 다른 경로 거부) → 끝에 agent `/signal SIGUSR1` 로 CMP 재적재. 삭제도 같은 경로(`DELETE /module-file`). 노드 상태는 CMP STATS
  `detail.ann_catalog{ids, missing}` 로 대조한다.
- **콘솔** `/service/announcements`(서비스 그룹) : 목록·청취(브라우저는 마스터 WAV 재생)·WAV 등록·삭제·노드 배포 상태·[배포]. 프로파일에서 음원을 고르는
  선택기는 `Setup.Announcement` 편집 화면이 이 API 로 목록을 읽는다.
- `oam-cims-tester` 의 `/api/v1/tester/samples`·`/test/samples` 는 그대로다 — 서로 참조하지 않는다.

## 8. NAT·SRTP·트랜스코딩·HA 상호작용

- **NAT**: 3.1 의 sendrecv answer 로 단말이 RTP 를 보내 latch 가 선행한다. latch 전에 낸 프레임은 선언 주소로 나가 유실될 수 있다 — 발신 leg 가 `remote_nat=1`
  이면 CMP 는 **latch(또는 첫 ingress) 까지 최대 `AnnNatWaitMs`(기본 500 ms) 재생 시작을 미룬다**(프레임을 버리지 않고 시작점을 늦춤). 미도착이면 선언 주소로 시작.
- **SRTP**: leg 컨텍스트로 protect — 키 변경(re-INVITE 재키잉)은 relay 와 같이 세션 재생성, 재생기는 SSRC 유지.
- **트랜스코딩 호**(cmp.md §11): 재생 leg 의 코덱 = 그 leg 선언(`media_codec`) — 변환 유닛과 무관하게 leg 코덱 파일을 낸다.
- **CMP All-Active**(csp.md §3.6): 재생기는 relay 세션과 같은 endpoint(session-sticky). endpoint DEAD 시 현행 능동 종료 경로가 호를 정리한다.
- **CSP HA**: 재생 대기 상태(`final_pending`)는 A-leg 다이얼로그와 함께 사라진다 — standby 는 `RELAY_PLAY_DONE` 을 받아도 호를 모른다(ack 만). 현행 relay 와 같은 한계.

## 9. 관측·통계·CDR

- `call.json`(CallDir) `announcement: { "situation": "busy", "media": ["sys:busy_kr","sys:ann_busy"], "played_ms": 8760, "result": "completed|max|cancelled|fallback" }`.
  `end_status`·`end_reason` 은 불변 — 통계 3계층 정의 무변경. hold 는 세션 이력 이벤트(`hold_music_start/stop`)로 남긴다.
- CSP 카운터(SipStats): `ann_started{situation}`·`ann_fallback{cause}`(CMP 미지원·MEDIA_NOT_FOUND·ANN_CAPACITY·SRTP 불일치). 콘솔 `/stats/volte` 에 "안내 재생" 행.
- CMP STATS: `resource.ann`, `detail.ann[]`, `detail.ann_catalog`. Flow 로그 `INT ANN_PLAY`/`ANN_DONE`.
- 계측기: `early_rtp_pct` 는 이미 있다(200 전 RTP). 추가 지표 `ann_pct`(최종 실패 응답 **전에** early media 를 받은 실패 시도 비율)·`ann_ms`(183→최종 사이 수신 RTP 지속).

## 10. 알람 (카탈로그 채번은 [../alarm_catalog.csv](../alarm_catalog.csv) 에서 — 아래는 제안)

| 제안 code | type | severity | 감지 | 조건 |
|---|---|---|---|---|
| A-PRC-034 | `media_missing` | major | CMP | 카탈로그가 참조하는 파일 누락·형식 오류(적재 시), 또는 프로파일이 참조한 id 의 `MEDIA_NOT_FOUND` 반복(1분 5회) — mo `cmp/ann/<id>` |
| A-QOS-030 | `capacity_threshold` | minor~major | CMP | `resource.ann` used/total 80 %/95 %, `ANN_CAPACITY` 발생 = major |
| E-STC-013 | 이벤트 | info | OAM | 음원 등록·삭제·배포(감사 — actor·id) |

`A-COM-007`(CMP 두절)의 영향 문구는 이미 "안내방송" 을 포함한다 — 정의 변경 없음.

## 11. P2 — 후속 상황

- **통화중대기(TS 24.615)**: CSP 가 착신 가입자의 활성 호(CallMap)를 보고 두 번째 INVITE 를 판정 → 착신 UE 로 가는 INVITE 에
  `Alert-Info: <urn:alert:service:call-waiting>`(RFC 7462 — 단말이 대기음을 낸다) → 발신자에게 180 + 프로파일 `call_waiting`(`sys:ann_call_waiting` 또는 링백).
  단말이 대기음을 못 내는 배치를 위한 **망 in-band 대기음**은 활성 통화에 톤을 섞는 `mode=mix`(디코드·믹스·재인코딩 — 변환 슬롯 소비) 가 선행돼야 한다.
- **착신전환 안내(TS 24.604)**: 현행 302 리다이렉트 모델은 안내 삽입 지점이 없다. 서버측 전환(B2BUA 가 새 B leg 를 낸다)으로 바꿀 때 `forwarded` 상황을 넣는다.
- **다이얼로그 이전 거절**(3.2)의 판정 지점 이동 · **가입자 링백**(6.3 컬럼·프로비저닝·콘솔) · **다국어 세트**(프로파일을 언어별로 두면 된다 — 모델 변경 없음).
- **관제 큐/ACD**([dispatch_center.md §10](dispatch_center.md)): 대표번호 대기열의 "잠시만 기다려 주십시오"·순번 안내는 3.4 의 링백 재생기(repeat 0)에
  시퀀스 교체(RELAY_PLAY 교체 = `replaced`)를 얹는 것이다.
- `mix` 모드·런타임 인코딩(PCM 마스터 → leg 코덱, 변환 슬롯 사용)·영상 안내.

## 12. 이행 순서 (P1)

| 단계 | 내용 | 검증 |
|---|---|---|
| ① 음원 세트 | `media/announcements/` 마스터 + `gen_announcements.py` + CMake dist(`cmp/announcements/sys/`, 카탈로그 sys 행) | 파일 4 코덱 × 12 종, sha256 |
| ② CMP 재생기 | `PAnnCatalog`·`PAnnPlayer`·워커 timerfd·`RELAY_PLAY/_STOP`·`RELAY_PLAY_DONE`·`resource.ann`·`ANN_RELOAD`/SIGUSR1·cmp.json 3 키·template 섹션+collection | `S1-UNIT-CMP` `cmp_ann_player_test`(프레이밍·20 ms 페이싱·SRTP egress·replace·시퀀스/repeat/max) |
| ③ CSP 정책·상태 머신 | `CspAnnouncement`·`Setup.Announcement`·접속서비스 2 필드·RemoteNode `announcement_profile`·발화 지점 5곳·psip `RingCall` 오버로드·CDR | `S1-UNIT-CSP` 판정표·해석 순서 |
| ④ 스모크 | cspsim: 통화중(486)·없는 번호(404)·보류 MOH·CANCEL 중단·CMP `AnnPlayers=0` 폴백 | `S3-SCN-ANN-BUSY`·`-NOTFOUND`·`-HOLD-MOH`·`-FALLBACK` |
| ⑤ 계측기 | 시나리오 `VOLTE-ANN-BUSY`(기대 `ann_pct≥99`, 최종 486)·`VOLTE-HOLD-MOH`(피보류 UE `rtp_rx>0` during hold)·`TRUNK-PBX-BUSY-NOANN`(trunk 프로파일 — 안내 없음) | tb48 실측 |
| ⑥ OAM 라이브러리 | `oam-svc` `services/announcements.py`·API·agent `/module-file`·콘솔 `/service/announcements`·`Setup.Announcement` 편집 | `S1-UNIT-OAM` 등록/삭제/in_use 409/배포 대조 |
| ⑦ 문서 | cmp_media_api §6.7·§5.1·§8·§9 / cmp.md §12 / csp.md §3.1·§3.12·§6 / volte_flows C9(실패 안내)·C10(MOH) / sip_service_model §2-2·§2-9 / agent_api `/module-file` / alarm_catalog 채번 / CLAUDE.md 개요 | — |

패키지 버전은 CSP·CMP·oam-svc·agent 가 함께 오른다(라이브 배포는 정지창 — 안내 없는 구 CMP 와 새 CSP 의 혼용은 `resource.ann` 미광고로 안전하게 폴백한다).
