# 안드로이드 VoLTE/PTT 단말(UE) 클라이언트 설계

> **핵심 결정 요약**
>
> CIMS 서버(CSP/CMP/CSC)에 직접 붙는 **신규 안드로이드 네이티브 단말 앱**을 개발한다.
>
> | 항목 | 결정 |
> |---|---|
> | SIP 시그널링 + 미디어 파이프라인 | **PJSIP** (REGISTER/INVITE/SUBSCRIBE/PUBLISH + RTP/지터버퍼/AEC/conference bridge) |
> | 코덱 | **안드로이드 MediaCodec** — 음성 **AMR-WB**, 영상 **H.264** (OEM 라이선스 코덱 활용 → 코덱 특허 노출 완화) |
> | MCPTT Floor Control | PJSIP 밖 **별도 UDP 소켓**에서 `"MCPT"` RTCP-APP 직접 구현 |
> | CSC 설정 플레인 | PJSIP 무관 — **HTTPS(OkHttp) + OAuth2 PKCE + XCAP** 직접 구현 |
> | "VoLTE" 의미 | 통신사 IMS 무선 연동이 **아님**. CSP에 붙는 **SIP 소프트폰(전화 앱)** 을 가리킴 |
>
> **가장 큰 기술 리스크:** MediaCodec AMR-WB의 실시간(20ms 프레임) 지연 → **M0에서 스파이크로 먼저 검증.**
> (영상 H.264는 PJSIP에 Android MediaCodec 경로가 이미 있어 저위험.)
>
> 본 앱은 `cspsim`(C++ 레퍼런스 클라이언트)과 **동일하게 SIP/RTP를 직접** 말한다. `cims-phone`/`cwrtc`(WebRTC 게이트웨이 경유) 경로는 사용하지 않으며, 설정 플레인 TS 코드(`cims-phone/src/api/*`)는 참고 구현으로만 활용한다. 레거시 `ext/psip/AndroidSipStack`(psip JNI 포팅)은 본 결정으로 대체된다.

---

## 1. 개요 및 범위

### 1.1 목적

안드로이드 단말에서 동작하는 **VoLTE(1:1 VoIP) + MCPTT(그룹 PTT)** 클라이언트를 개발한다. 단말은 3GPP MCPTT 규격(TS 24.379/380/481/484/33.180)에 따라 CSP와 SIP 시그널링, CMP와 RTP/RTCP(+Floor) 미디어, CSC와 HTTPS(인증/설정)를 수행한다.

### 1.2 범위

| 구분 | 포함(In) | 제외(Out) |
|---|---|---|
| 통화 | VoLTE 1:1 음성/영상, MCPTT 그룹콜(prearranged/broadcast/chat) | 통신사 IMS/eSIM 연동, 회선교환(CS) 폴백 |
| 제어 | Digest 등록, affiliation, floor control, 그룹/프로파일 조회 | KMS 기반 E2E 암호화(MIKEY-SAKKE) — 후속 |
| 미디어 | AMR-WB 음성, H.264 영상, AEC/지터버퍼 | opus/PCMU 등은 협상 호환만(우선순위 하위) |
| 부가 | emergency/imminent-peril, conference 멤버 상태 표시 | off-network(ProSe/직접통신) |

### 1.3 "VoLTE" 정의(중요)

본 문서의 "VoLTE"는 통신사 무선망 IMS 연동이 아니라, **CSP를 향한 SIP 소프트폰**을 의미한다(정본: [01_UE_Interface_Guide.md](../../../ems/core/console/public/docs/01_UE_Interface_Guide.md) §2). 3자 앱이 단말 IMS 스택을 직접 사용하는 캐리어 VoLTE는 범위 밖이다.

### 1.4 참조

| 문서 | 내용 |
|---|---|
| [01_UE_Interface_Guide.md](../../../ems/core/console/public/docs/01_UE_Interface_Guide.md) | **정본 UE 연동 규격** (메시지/SDP/floor 패킷 hex/CSC) |
| [volte_flows.md](volte_flows.md) | VoLTE 케이스·메시지 흐름 |
| [ptt_flows.md](ptt_flows.md) | PTT 케이스·메시지 흐름(on-demand/affiliation/floor/broadcast) |
| [mcptt_api.md](../../api/mcptt_api.md) | CSC IdMS/GMS/CMS/KMS API |
| [mcptt_emergency_modes.md](mcptt_emergency_modes.md) | emergency/imminent-peril/alert/ad-hoc |
| [recording.md](recording.md) | 서비스 로그/녹취 디렉터리(서버측) |
| 3GPP TS 24.379/380/481/484/33.180 | MCPTT 호제어/미디어/GMS/CMS/보안 |

---

## 2. 전체 아키텍처

### 2.1 단말이 말하는 4개 평면

```
                         ┌───────────────────────────────────────────┐
   ┌───────────┐  SIP(UDP5060/TLS5061)  │ CSP  │ REGISTER·INVITE·SUBSCRIBE·NOTIFY·PUBLISH
   │           │◄──────────────────────►│(CSCF │
   │  안드로이드 │                        │ /TAS │
   │   단말 App │  RTP/RTCP (음성/영상)   │/PTT) │
   │  (UE)     │◄──────────────────────►┌──────┤
   │           │  RTCP-APP "MCPT" floor  │ CMP  │ 음성 52000~ / floor 54000~ / VoIP 50000~
   │           │◄──────────────────────►│(미디어)│
   │           │  HTTPS 4430             ├──────┤
   │           │◄──────────────────────►│ CSC  │ IdMS(OAuth2 PKCE)·GMS·CMS(XCAP)
   └───────────┘                         └──────┘
```

| 평면 | 상대 | 전송 | 담당 모듈(앱 내부) |
|---|---|---|---|
| 시그널링 | CSP | SIP/UDP 5060 (TLS 5061) | **PJSIP** |
| 미디어(음성/영상) | CMP | RTP/RTCP | **PJSIP 파이프라인 + MediaCodec 코덱** |
| Floor 제어 | CMP | RTCP-APP/UDP (m=application) | **자체 FloorClient(별도 소켓)** |
| 설정/인증 | CSC | HTTPS 4430 | **자체 CscClient(OkHttp)** |

### 2.2 앱 내부 레이어

```
┌──────────────────────────────────────────────────────────────┐
│ UI (Jetpack Compose)  전화 화면 · PTT 화면 · 그룹/멤버 · 설정     │
├──────────────────────────────────────────────────────────────┤
│ Domain / ViewModel    호 상태머신 · 발언권 상태 · 그룹 모델       │
├───────────────┬───────────────┬───────────────┬──────────────┤
│ SipController │ FloorClient   │ CscClient     │ MediaCtrl    │
│ (PJSIP 래퍼)  │ (MCPT RTCP-APP│ (OAuth2 PKCE  │ (코덱/장치/   │
│               │  별도 UDP)    │  + XCAP HTTPS)│  AEC 설정)   │
├───────────────┴───────────────┴───────────────┴──────────────┤
│ PJSIP (pjsua2, JNI/SWIG)  + MediaCodec 코덱 팩토리(AMR-WB/H264) │
├──────────────────────────────────────────────────────────────┤
│ Foreground Service (등록 유지 · wakelock · 알림 · 오디오 포커스)  │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. 기술 스택 & 핵심 설계 분기 (MediaCodec ↔ PJSIP)

PJSIP 미디어 파이프라인은 **[RTP/RTCP]→[지터버퍼]→[AEC]→[conference bridge]→[코덱]** 이다. 본 설계는 **코덱 레이어만 MediaCodec으로 교체**하고 나머지는 PJSIP를 그대로 쓴다. 교체 지점은 PJSIP 정식 확장점인 **codec factory**.

```
   PJSIP 유지(품질 핵심)               교체(코덱만)
 ┌────────────────────────────┐   ┌─────────────────────┐
 │ RTP·지터버퍼·AEC·conf bridge │──→│ pjmedia_codec_factory│
 │ SDP 협상·패킷화             │←──│  = Android MediaCodec │
 └────────────────────────────┘   └─────────────────────┘
```

| | 음성 AMR-WB | 영상 H.264 |
|---|---|---|
| PJSIP 기본 지원 | ✅ **And-Media 오디오 경로 존재** — `and_aud_mediacodec.cpp`(2.16), `PJMEDIA_HAS_AND_MEDIA_AMRWB` 기본 1 → **커스텀 `pjmedia_codec_factory` 불필요** | ✅ Android MediaCodec 비디오 경로 존재(`and_media`) |
| 위험도 | 낮음 — And-Media 내장 + M0 게이트(UNIWA ENC+DEC·실시간성) 통과. 잔여 = 서버 opencore 와의 상호운용(M1.2 실호 게이트) | 낮음 |
| 리스크 | MediaCodec 비동기 API 의 20ms 정합은 PJSIP And-Media 구현이 처리 — 단말 단 검증은 M0 로 완료 | 영상은 버퍼링 지연 허용폭이 커서 궁합 좋음 |
| 폴백 | M1.2 상호운용 게이트 실패 시 opencore-amr(경로 A, 특허 재검토 동반) | 디바이스 미지원 시 SW 인코더 |

> **설계 원칙:** AMR-WB MediaCodec의 실시간성은 **M0 스파이크로 정량 측정**(end-to-end mouth-to-ear 지연, 프레임 드랍)한 뒤 본구현 진입. 측정 기준 미달 시 폴백 결정.

### 3.1 코덱 파라미터(서버 정합)

- 음성: `AMR-WB/16000/1`, SDP fmtp **`octet-align=1; mode-set=0,1,2`** (서버 200 OK와 일치, [01_UE_Interface_Guide.md](../../../ems/core/console/public/docs/01_UE_Interface_Guide.md) §2.2). PT=99.
- 영상: `H264/90000`, `profile-level-id` 서버 협상값 추종, `packetization-mode=1`.
- 코덱 우선순위: AMR-WB 최상위, 그 외(opus/PCMU/PCMA)는 호환용 하위.

---

## 4. 프로토콜 매핑 — PJSIP가 주는 것 vs 직접 구현

| 기능 | PJSIP | 직접 구현 |
|---|---|---|
| REGISTER + Digest MD5/qop=auth | ✅ 자동(regc + auth_clt) | 계정/크리덴셜 설정만 |
| INVITE/SDP offer-answer/RTP | ✅ inv_session + pjmedia | — |
| AMR-WB/H.264 인코드·디코드 | △ 파이프라인은 ✅ | **MediaCodec 코덱 팩토리** |
| TLS 5061 / SRTP / ICE | ✅ | 인증서/설정 |
| SUBSCRIBE/NOTIFY(conference) | ✅ evsub UAC (native `conference` 패키지 등록, [2-13]) | 본문(XML) 파싱 |
| SUBSCRIBE/NOTIFY(xcap-diff) | △ evsub 프레임워크 | **이벤트 패키지 등록 + 본문(XML) 파싱** |
| PUBLISH(affiliation) | △ publishc 전송 | **`mcptt-affiliation-command+xml` 본문** |
| multipart INVITE(mcptt-info+resource-lists+SDP) | △ `pjsip_multipart` 파싱 | **MCPTT XML 스키마 해석** |
| MCPTT Floor(`"MCPT"` RTCP-APP) | ❌ | **FloorClient(별도 UDP)** |
| CSC OAuth2 PKCE + XCAP(HTTPS) | ❌(SIP 아님) | **CscClient(OkHttp + PKCE)** |

### 4.1 등록/통화 흐름(요약, 상세는 volte_flows/ptt_flows)

```
[VoLTE] REGISTER→401→REGISTER+Digest→200 │ INVITE(SDP)→100/180→200(SDP)→ACK │ RTP(CMP relay) │ BYE
[PTT]   (CSC 인증·그룹조회) → REGISTER → PUBLISH(affiliate)
        → 키업 INVITE(group URI) → 200(SDP: m=audio + m=application floor)
        → floor REQUEST/GRANT → RTP 음성 → RELEASE
```

---

## 5. m=application / Floor Control 설계 (가장 프로젝트-특화)

PJSIP의 SDP 협상기는 표준 audio/video만 생성/이해한다. MCPTT의 `m=application <port> UDP MCPTT` + `a=floorid:0 mstrm:audio` 는 PJSIP이 다루지 않으므로 **하이브리드**로 처리한다.

### 5.1 처리 방식

```
① 자체 UDP 소켓(FloorClient) 개설 → 로컬 floor 포트 확보
② PJSIP 송신 SDP에 m=application 라인 주입
   (pjsip_module on_tx_request/on_tx_response 또는 PJSUA2 SDP 생성 콜백)
③ 수신 SDP(200 OK/INVITE)의 m=application 포트 파싱
   → CMP floor 목적지 학습 (※ "RTP+1" 고정 가정 금지 — 반드시 SDP에서 파싱)
④ 발언권 Request/Release 송신, Granted/Deny/Idle/Taken/Revoke 수신 → 상태머신 반영
```

### 5.2 패킷 포맷 (`"MCPT"` RTCP-APP, **정본: 3GPP TS 24.380 §8**)

> **규격 정합 결정:** 단말은 **TS 24.380 규격**대로 구현한다. 메시지 타입은 RTCP-APP **subtype(5비트)**
> 에 싣고, 본문은 `Field ID(8) + Length(8) + value` **TLV 필드**의 나열이다. (서버 CMP 의 현행 구현은
> `opcode/id_len/speaker_id` 자체 **simplified** 포맷이라 규격과 다르며 — interop 위해 **CMP 를 TS 24.380
> 으로 정렬하는 별도 작업** 필요. UE Interface Guide §3.4 는 그 simplified 포맷을 문서화한 것.)

```
0               1               2               3
V=2 P subtype | PT=204        | length(32bit words-1)         |   ← subtype = 메시지 타입
SSRC (floor participant)                                       |
name = "MCPT" (0x4D435054)                                     |
[ Field ID | Length | value(Length) | 4B 정렬패딩 ] ...              |
```

메시지 타입(subtype) — TS 24.380 Table 8.2.2-1:

| subtype | 메시지 | 방향 |
|---|---|---|
| 0 | Floor Request | UE→서버 |
| 1 | Floor Granted | 서버→UE |
| 2 | Floor Taken | 서버→화자 외 |
| 3 | Floor Deny | 서버→UE |
| 4 | Floor Release | UE→서버 |
| 5 | Floor Idle | 서버→ALL |
| 6 | Floor Revoke | 서버→화자 |
| 8 | Floor Queue Position Request | UE→서버 |
| 9 | Floor Queue Position Info | 서버→UE |
| 10 | Floor Ack | both |
| 0x0B | Unicast Media Flow Control | UE→서버 |
| 0x0E | Queued Floor Requests | both |
| 0x0F | Floor Release Multi Talker | 서버→UE |

subtype 의 **첫 비트(0x10)는 "Ack 요구"** 변종이다(§8.2.2) — 받은 쪽은 Floor Ack(Source+Message
Type)로 회신해야 한다. 서버(CMP)와 단말(`FloorCodec`/`FloorClient`) 모두 이를 처리한다: 수신
subtype 에서 비트를 걷어내 기본 타입으로 다루고(`FloorMessage.type`), 요구가 있었으면 상태 처리
**전에** Floor Ack(Source=0 floor participant + Message Type=대상 subtype)를 회신한다. 송신은
양쪽 모두 ack 를 요구하지 않는다(도달 보장은 서버 재송신 타이머 T7/T8/T20).

주요 Field ID(TS 24.380 §8.2.3): 0 Floor Priority · 1 Duration · 2 Reject Cause · 3 Queue Info ·
4 Granted Party's Identity · 5 Permission · 6 User ID · 7 Queue Size · 8 Msg Seq No · 10 Source ·
11 Track Info · 13 Floor Indicator(비트마스크: emergency 0x1000 / imminent 0x0800 …) · 14 SSRC ·
15/16 List of Granted Users/SSRCs(동시 발언) · 21~23 Queued Floor Requests · 24 Media Flow Control.
구현: `ptt-client/floor/{FloorControl,FloorCodec,FloorClient}.kt` — **모든 필드가 패딩 포함
4옥텟 배수**(§8.1.3)라 미지 필드도 건너뛴다.

> **서버 정합에 따른 단말 과제는 [§5.4](#54-서버-규격-정합에-따른-단말-구현-요구사항-ts-24380)에
> 항목(U1~U18)으로 정리했다** — Taken 의 신규 필드(Permission·MSN·SSRC), 동시 발언 리스트,
> ack 요구 변종, Revoke 응답, SDP fmtp 협상, floor SRTCP 등. 서버측 정본은
> [mcptt_standard_conformance.md](mcptt_standard_conformance.md) §1 이다.

### 5.3 단말 Floor 상태머신

```
        REQUEST            ┌─ GRANT ─► [SPEAKING] ── RELEASE ──► [IDLE]
[IDLE] ──────────────►(대기)┤                  ▲                    │
                            └─ REJECT ─► [IDLE]  │ REVOKE(선점)       │
        TAKEN(타인 화자) ─► [LISTENING] ─ IDLE ─►─┘◄── TAKEN(타 화자)──┘
```

- **PTT 버튼 down** → Floor Request 송신(+REQUESTING). GRANT 수신 시 **승인 톤(이중 삑)+진동 재생을 마친 뒤에** mic 개방("삑 후 말하기" — 톤이 그룹으로 송출되지 않게). 3초 내 GRANT/DENY 무응답이면 IDLE 복귀+거부 톤.
- **늦은 GRANT**(버튼을 이미 뗀 뒤 도착) → 즉시 Release 반납(mic 미개방).
- **Revoke** 수신 → 즉시 mic disconnect + 회수 톤·진동 + **Floor Release 회신**(§6.2.4.5.4, `FloorClient` 가 800ms×2 재전송). 서버는 Release 를 받는 즉시 다음 화자를 승급시킨다 — 회신이 없으면 유예 T3(3초)를 매번 소모한다. dual floor 의 G-bit 는 회수 통지에 실려 온 것을 그대로 되싣는다. 서버가 T8(1초)로 Revoke 를 재전송하면 Release 만 다시 보내고 사용자 알림은 1회만 낸다.
- **Deny** 수신 → 즉시 mic disconnect + 거부 톤(승인과 구별되는 저음)+진동. cause 별 문구는 `FloorCause.REJECT`(#1 다른 참가자 점유 / #3 1인 세션 / #5 수신 전용 / #7 큐 포화).
- **발언 시간 제한**(Granted Duration=서버 T2) → 잔여 시간을 발언 스트립에 표시하고, 마감 5초 전 알림 톤·진동, 마감 300ms 전 **스스로 Release**. 초과하면 서버가 Revoke #2(Media burst too long)로 끊는다.
- **Permission to Request the Floor**(Floor Taken 필드, §8.2.3.7) = 0 인 leg(broadcast 그룹·ambient 청취)은 PTT 버튼을 비활성("청취 전용 채널") — 눌러도 Deny 만 돌아온다.
- **대기열**(Floor Deny 대신 Queue Position Info 수신, `mc_queueing` 협상 전제) → PTT 바·발언 스트립에 "대기 N번째"(황색). **버튼을 계속 누르고 있으면 순번을 기다리고, 떼면 Queued Floor Requests(0x0E, 대상 목록 없음)로 자기 대기 요청을 취소**한다 — Floor Release 는 발언 중이 아닌 leg 에서 무시되므로 그것만으로는 유령 대기자가 남는다. 서버/의장이 지운 경우의 Cancel Notification 도 같은 경로로 처리한다.
- **Message Sequence Number**(Taken/Idle) 역전·중복은 폐기한다. 단 직전 64개 안쪽으로 되돌아간 것만 — 그보다 멀리 뒤로 간 값은 서버측 카운터 초기화로 보고 새 기준으로 재동기한다(폐기하면 floor 표시가 영영 얼어붙는다).
- **Taken** → 발언자 카드에 화자(Granted Party's Identity)+발언 경과시간 표시, LISTENING 중 버튼 누름은 무시(불필요한 REJECT 방지).
- **동시 발언**(dual/multi) → Taken 은 화자 **집합 전체**(List of Granted Users/SSRCs)를 싣고, 한 명이 끝나면 Idle 대신 0x0F 로 그 화자만 알린다. 단말은 집합을 들고 증분 갱신한다(Taken=전체·0x0F=한 명 제거·Idle=비움). ⚠️**뒤에 승급한 화자의 Taken 은 먼저 말하던 나에게도 오고 그 목록에 내가 있다** — 목록에 내가 있으면 SPEAKING 을 유지해야 내 마이크가 닫히지 않는다. 화자별 SSRC 는 `Session.talkerSsrc` 에 남겨 SSRC 별 재생(U10)의 입력으로 쓴다. UI 는 화자가 2명 이상이면 발언 스트립에 전원 명단, 목록·칩에는 "외 N".
- **UX 구현**: 톤/진동=`ptt-client/audio/PttFeedback.kt`(ToneGenerator STREAM_VOICE_CALL+Vibrator, 서비스가 컨트롤러에 주입), 발언자 추적=`PttController.speaker: StateFlow<Speaker?>`(내 GRANT/타인 TAKEN, elapsedRealtime 기준).
- **앱 UI 구조**(`ptt-client/ui/`, 시안 `android/assets/pages/` 기준 — 다크 배경+민트 액센트, 토큰=`ui/Theme.kt` `Ct`, 아이콘=`assets/svgs` 변환 VectorDrawable):
  - 라우팅=`ui/AppRoot.kt`(`Nav` sealed: Splash→Home 4탭(주채널/전체채널/메시지/설정)→Channel/Thread 푸시, BackHandler 계층 복귀). 컨트롤러 상태는 `PttUiState` 로 묶어 화면에 전달.
  - **스플래시**(`SplashScreen`)=동심원 로고+기관 로고(`yrt_logo`), CIMS SSO 라 별도 로그인 화면 없음 — 계정 있으면 등록 진행 후 자동 진입. 미로그인이면 MainActivity 진입 시점에 CIMS 로그인 화면으로 전환([android_ue_provisioning.md](android_ue_provisioning.md) §1-1)되므로, 스플래시의 "CIMS 로그인 열기" 버튼은 CIMS 앱 미설치 폴백이다.
  - **주채널 탭**(`MainChannelScreen`)=주채널 1개만 전면 배치(카드 박스 없음). 부채널 개념은 없으며 주채널 외 참여 채널은 전체채널 탭에서 확인. 구성(위→아래): 채널명+`P{n}` 배지(TS 24.481 on-network-group-priority, 표시 시 `loadGroupDetail` ETag 캐시 로딩)+긴급 배지+우측 **[주채널 선택] 버튼 → 같은 윈도우 바텀시트**(`ChannelSelectSheet` — 내 그룹 리스트(이름+P배지+주채널/참여 중/미참여)에서 탭 한 번으로 즉시 지정, 화면 이동 없음; 미참여 그룹이면 `joinGroupCall` 로 참여부터 수행 후 `setPrimary`) / 태그칩(음성/구성원 N)+"▶ ○○ 송신" 칩 / 발언 상태 스트립(발언자+경과 타이머 슬림 한 줄) / **영상 패널**(`VideoPanel` — 영상 PTT 대비 소형 영상 자리, 현재 "영상 없음" 플레이스홀더; 우하단 오버레이 원형 아이콘=**오디오 출력**·전체듣기. 오디오 출력은 **스피커폰이 기본**이며 이어폰 미연결 시 탭=스피커폰↔수화기 토글, 이어폰 연결 시(유선/블루투스 — 무선 다중 연결 포함) 탭=`AudioRouteSheet` 바텀시트에서 이어폰(장치별)/스피커폰/수화기 선택. 이어폰 장치 열거·지정=`ptt/audio/AudioRouter`(API 31+ `setCommunicationDevice`/`availableCommunicationDevices`, 이하 BT SCO·유선 자동 라우팅; 스피커폰/수화기는 종전대로 pjsua2 `setOutputRoute`), 선택 영속=`AudioRoutePrefs`(리부팅/재기동 복원), 이어폰 **연결=자동 전환**·해제=남은 이어폰 또는 스피커폰 복귀(`PttService.observeHeadsets`) — 해제(장치 소멸) 시에는 폴백 라우팅 후 **사운드 장치 재오픈**(`SipController.bounceSndDev`, 재생 트랙 재생성)도 수행한다: 일부 단말(MF52/A15 실측)이 라우팅 중이던 출력 장치가 사라지는 순간 재생 트랙에 시스템 뮤트(streamVolume)를 건 채 재라우팅 후에도 해제하지 않아 무전이 무음으로 고착되는데, 볼륨 변경으로는 안 풀리고 트랙 재생성만이 뮤트 평가를 리셋한다) / 화면 PTT 바(**터치 단말만** — 하드웨어 PTT 버튼 단말은 표시 없음) / **하단 인라인 채팅**(`InlineChat`: 주채널 그룹 메시지 말풍선 리스트+입력바, `svc.sendGroupMessage` 송신·`markThreadRead`·새 메시지 자동 스크롤, 우상단 아이콘=전체 대화 화면). 입력바=첨부+입력+전송(종이비행기 `ic_send`) — **첨부 버튼**(`AttachButton`, 시스템 포토 피커 사진/동영상)은 메시지 스레드 입력바와 공용이며 미디어 **전송은 서버 업로드 경로 연동 후** 지원(현재 선택 시 안내 토스트). **채널별 수신 음량**=채널 상세의 슬라이더(주채널 화면엔 없음)→`PttController.setChannelVolume`→`SipController.setCallRxLevel`(pjsua2 conference bridge `adjustRxLevel`, 0~2·1=원음; 미디어 재협상 시 리셋되므로 `applyListenPolicy` 에서 재적용). 그룹별 음량은 `ptt/audio/GroupVolumeStore`(SharedPreferences)로 **영속**(리부팅/앱 재기동 유지)하며 **저장값 없는 신규 그룹 기본=최대(2f)**. 하드웨어 음량 키는 무전 재생 스트림 조절(`MainActivity.volumeControlStream=STREAM_MUSIC` — 무전 재생 트랙 축, 분리 라우팅 참조). SOS 는 HW 키 전용(화면 SOS 버튼 없음). PTT 바 상태색: 대기=민트/요청=황색/발언=민트+펄스/수신=회색면.
  - **전체채널 탭**(`ChannelsScreen`)=**당겨서 새로고침**(`PullToRefreshBox`, 별도 새로고침 버튼 없음)+채널 행(맨 앞=그룹 이름 첫 자 배지, 이름 옆 주채널 "주"/긴급 소형 사각 배지(`SquareBadge`), 음성/영상·그룹 `P{n}`·구성원 수 태그칩, **발언 중이면 마이크 아이콘+발언자 이름 칩**(그룹 문서 이름 우선, 없으면 번호)/참여 중/가입 배지, 우측 메시지 버튼). 그룹 P·영상 여부는 그룹 문서(TS 24.481)에서 — 목록 진입 시 그룹별 `loadGroupDetail`(ETag 304 캐시라 저비용). 행 탭→**채널 상세**(`ChannelDetailScreen`, 시안 `채널선택화면-상세.png`·`채널상세화면-주채널표시.png`)=헤더(채널명+음성/영상·그룹 `P{n}` 태그칩+CH 번호)+**역할 배너**(고정폭 소형 사각 배지 "주채널"/"일반", **배너 터치=주채널↔일반 토글** — 일반→주채널은 미참여 시 `joinGroupCall` 참여부터, 주채널→일반은 `clearPrimary` 강등(주채널 없는 상태 허용), 안내문도 함께 전환)+채널 상태 카드(구성원 N명·접속 중 M명·발언자, 참여 중이면 수신 음량 슬라이더)+구성원 목록+하단 **참여/나가기 토글 버튼**(미참여=[참여] / 참여 중=[나가기]).
  - **채널 상세 구성원 명부 = TS 24.481 그룹 문서(표준 필드 + CIMS 확장 직함)**: 진입 시 `PttController.loadGroupDetail` 이 GMS XCAP `getGroupDoc`(ETag/If-None-Match 캐시)으로 그룹 문서를 받아 `csc/CscModels.kt GroupDoc.parse` 로 파싱 — 멤버별 `entry uri`(tel: = 전화번호)·`rl:display-name`(이름)·`mcpttgi:participant-type`(chair→"의장" 배지)·`mcpttgi:user-priority`(회색 `P{n}` 배지 — TS 24.481/24.380 규격: **0~255, 클수록 높은 우선순위**, 미지정=최저)·`cims:user-title`(직함 — 이름 옆 보조 표기, 빈 값이면 서버가 요소 생략), 그룹 레벨 `on-network-group-priority`(헤더·전체채널 카드의 `P{n}` 태그칩, 동일 규격 방향)·`mcptt-video`(음성/영상)·`session-type`·`max-participant-count`. 접속/발언 상태는 conference-info(RFC 4575) 참가자·floor(TS 24.380) 화자와 번호 키로 결합해 **접속 중/오프라인** 섹션 분리(각각 우선순위 내림차순 정렬)(미참여 시엔 접속 여부를 알 수 없어 중립 "구성원" 명부). 직함 등 3GPP 미정의 필드는 `<entry>` 의 `##other` lax 확장 지점(TS 24.481/RFC 4826)에 CIMS 전용 네임스페이스(`urn:cims:groupinfo:1.0`)로 싣는다 — 규격 적합 확장이며 표준 단말은 무시한다. 직함 원본은 DB `users.title`. 전화번호 표시는 홈 국가코드 축약(`PttController.fmtNumber`: +82… → 0…, 프로비저닝 countryCode 우선·내 msisdn ITU 유도 폴백 — VoLTE 앱과 동일 방식, 표시 전용).
  - **메시지 탭**(`MessagesScreen`)=스레드 목록(안읽음 배지)→대화(`MessageThreadScreen`)=날짜 구분+말풍선(발신=민트 우측/수신=다크 좌측)+입력바. 발신=`PttController.sendGroupMessage`(SIP MESSAGE, 그룹 URI — CSP fan-out), 수신=`SipController.incomingMessage`→core `MessageStore` 영속(`PttService`), 변경 틱=`PttService.messageTick`. **메시지 삭제**: 스레드 목록에서 행 길게 누름=대화 삭제·헤더 휴지통=전체 삭제(각각 확인 다이얼로그), 대화 화면에서 말풍선 길게 누름=**선택 모드**(탭=토글, 상단바 전환: 선택 수+[전체선택]+휴지통 → 1건/다건 삭제) — 식별=`MessageEntry.key`(peer|time|direction|msgId|text 복합, msgId 는 비보장), 삭제 API=`MessageStore.delete(keys)/clearThread/clearAll`, 서비스 래퍼(`PttService.deleteMessages/deleteThread/deleteAllMessages`)가 첨부 로컬 파일·전송 진행률(`_sendProgress`)도 함께 정리 후 tick 갱신. 미디어 첨부는 후속 과제(서버 경로 미구현).
  - 이력 이벤트 훅=`PttController.onEvent`(`PttEvent`: JOIN/LEAVE/TALK_ME/TALK_OTHER/EMERGENCY*)→`ptt-client/history/HistoryStore.kt`(SharedPreferences JSON, 최대 500건) — 수집만 하며 전용 화면은 없다(통화이력 UI 는 VoLTE 전화앱 영역).
  - **설정 탭**(`SettingsScreen`)=프로필(등록 상태·등록/해제)+통신 설정(전체 듣기 토글/**스피커·마이크 게인 슬라이더** — 출력 라우트(스피커/수화기/이어폰) 선택은 주채널 화면의 오디오 출력 아이콘으로 일원화)+하드웨어 버튼 설정(`KeyConfigOverlay` 열기)+**백그라운드 PTT 버튼**(접근성 키 필터 활성 상태 표시+접근성 설정 바로가기)+그룹 새로고침·버전.
  - **통화 오디오 모드·게인**: 통화 성립 시 `AudioRouter.setInCall(true)`=`MODE_IN_COMMUNICATION`+voice-call 스트림 최대(전 통화 종료 시 NORMAL 복원) — VoIP 라우팅·음량의 전제. 스피커폰/수화기는 pjsua2 `setOutputRoute` 에 더해 **AudioManager 직접 제어 병행**(`AudioRouter.setSpeakerphone`, API 31+ `setCommunicationDevice(BUILTIN_SPEAKER)`/이하 `isSpeakerphoneOn`) — 일부 단말 오디오 백엔드가 pjsua 라우팅을 무시(스피커폰 설정에도 수화기 출력)하는 실측 편차 대응. **무전 게인**=장치단 conference bridge slot0 gain(`SipController.setDeviceAudioBoost`: 스피커=playbackDevMedia `adjustTxLevel`, 마이크=captureDevMedia `adjustRxLevel`) — 시스템 음량 최대에서도 디지털 레벨이 낮은 단말 보정. 설정 탭 슬라이더 ×1.0~×3.0(0.1 단위, 기본 ×1.5), `AudioRoutePrefs` 영속, 통화 중 변경 즉시 반영, 전 통화 종료 시 1.0 원복. 채널별 수신 음량(0~2 슬라이더)과 곱으로 적용. 두 가지 함정을 코드가 상쇄한다: ①**스트림 볼륨 인덱스는 장치별** — `setInCall` 의 음량 최대화는 그 순간 라우팅된 장치 축에만 적용되므로(스피커/BT 축이 낮은 채 남아 무전이 작게 들림, 실측) 라우트 적용마다 `AudioRouter.ensureRxVolume()` 이 현재 장치 축(STREAM_MUSIC)을 확보한다(적용 직후+800ms 재시도 — 스트림 재라우팅 지연 대응) ②**boost 는 snd dev (재)오픈마다 slot0 레벨이 초기화**되고 캡처 게이트로 마이크가 닫힌 동안은 captureDevMedia 가 없어 mic 축 적용이 실패하므로, 저장값을 캡처 게이트 전환(`setCaptureEnabled`)·재오픈(`bounceSndDev`) 직후 재적용하며 두 축을 개별 runCatching 으로 적용한다(한 블록이면 뒤 축이 조용히 유실).
  - **마이크 캡처 게이트(반이중)·스위트 핸드오프**: Android 12+ 는 일반 앱 두 개의 **동시 캡처를 허용하지 않고 한쪽에 무음을 배달**한다(concurrent capture arbitration) — PTT 앱이 유휴 중에도 캡처를 쥐면 volte 통화 송신이 통째로 무음이 되는 실측 장애의 근원. 대책 2중:
    ①**PTT 캡처 게이트**(`SipController.setCaptureEnabled` → pjsua2 `AudDevManager.setSndDevMode`): 유휴/청취=`SPEAKER_ONLY|NO_IMMEDIATE_OPEN`(재생만 유지, AudioRecord 자체를 열지 않음 — 서비스가 `register()` 직후 기본 적용), 발언 구간(pttDown~UP/DENY/REVOKE/타임아웃, `PttController.setTalkCapture`)만 전이중. snd dev 가 열려 있으면 즉시 재오픈(~100ms, conference 결선은 브리지에 생존), 닫혀 있으면 모드만 저장돼 다음 on-demand 오픈에 적용.
    ②**마이크 핸드오프**(`CimsSuite.ACTION_MIC_YIELD/RESUME`, CIMS_SUITE 서명 권한 명시적 브로드캐스트): PTT 발언 시작 시 volte 에 YIELD → volte 는 **통화 중일 때만** 캡처를 해제(`setCaptureEnabled(false)` — 재생 유지, 통화 상대는 발언 동안 무음 청취)하고 40s 워치독·통화 종료 시 잔존 양보를 자동 해제, RESUME/발언 종료 시 전이중 복귀. PTT 발언 시작은 FGS microphone 승격(`elevateForCall`)도 함께 시도(부팅 자동시작 specialUse 대비). → **"VoLTE 통화 + PTT 청취 동시"는 상시 가능**(재생은 믹스), 마이크만 발언 주체가 결정적으로 획득.
  - **통화/무전 분리 라우팅(트랙 단위)**: "통화=수화기(기본), 무전=스피커(기본)" 를 **동시에** 성립시킨다 — 무전은 통화 중에도 크게 들려야 한다는 운용 요구. pjproject 패치(`android_jni_dev.c`, 정본=`~/m1_build_pjsip.sh` §2-6) 3요소: ①`PJMEDIA_AUD_DEV_CAP_OUTPUT_ROUTE` 구현 — 재생 `AudioTrack.setPreferredDevice` 로 **이 스트림만** 장치 고정(스피커/수화기/해제, 컨텍스트는 ActivityThread.currentApplication) ②라우트 캡을 keep 저장하는 앱(PTT — `setOutputRoute`, volte 는 미사용)은 재생 트랙을 **STREAM_MUSIC** 으로 생성 — voice 전략 트랙은 통화 중 per-track 지정이 정책에 무시됨(MTK/QC 공통 실측), 미디어 전략은 존중 ③`get_cap` 구현(미구현 시 pjsua `update_initial_aud_param` 이 keep 저장을 지움). 앱 측: PTT `setAudioRoute` 는 pjsua 부팅 후(register 뒤) 재적용해 keep 저장 확보, 통화(양보) 중 **주기 재핀**(4s, 해제→재설정 바운스 — 같은 값 재설정은 no-op 이고 통화 수립기 정책 재라우팅이 핀을 반복 이탈시키므로 지속 재적용으로 수렴), 무전 볼륨 축=STREAM_MUSIC(음량 키·setInCall 볼륨 확보). **각 앱 토글은 각자 소리만 제어**: volte 토글=통화 경로, PTT 토글=무전 경로(통화 중에도 동작) — UI 일관성 자동 성립. ⚠️통화 중 무전이 스피커로 나오면 통화 마이크로 유입될 수 있음(상대가 무전 소리를 들을 수 있음 — 무전 운용상 수용).
  - **출력 라우팅 양보(역방향 핸드오프)**(`CimsSuite.ACTION_ROUTE_YIELD/RESUME`) — 통화(전역) 라우트 소유권 정리: PTT 는 무전 특성상 **communication device 요청(스피커폰/이어폰)과 오디오 모드(MODE_IN_COMMUNICATION, 세션 보유 중 상시)** 를 유지하는데, 단말 오디오 브로커는 통화 라우팅 요청을 **모드 소유자 기준으로 매칭하고 그 소유자를 전역 모드값이 NORMAL→IN_COMMUNICATION 으로 바뀌는 에지에서만 기록**한다(W999/MTK13·MF52/QC15 공통 실측 — 소유자만 바뀌고 값이 유지되면 miss, 겹친 전이는 코레이싱 유실). 따라서 PTT 가 모드를 쥔 채로는 volte 의 수화기/스피커 요청이 전부 무시된다(증상: 단말별 "모두 스피커폰"/"모두 수화기"). 대책 — 에지를 명시적으로 만들어 소유권을 주고받는다:
    - volte 통화 시작: ROUTE_YIELD 송신(통화 중 5분 주기 재송신) → PTT `AudioRouter.yieldRoute`=자기 요청 해제+**모드 반납**+재적용 억제(선택 상태 유지) → volte 는 **전역 모드가 NORMAL 로 떨어진 것을 폴링(100ms×20)으로 확인 후 claim**(`claimInCallAudio` — 에지에서 브로커에 volte 가 기록됨) → 기본 라우팅(수화기) 명시 적용.
    - 통화 중 volte 는 **명시 장치 요청 유지**(`applyRoute`: 수화기=이어폰>BUILTIN_EARPIECE/스피커폰=BUILTIN_SPEAKER, 레거시 `isSpeakerphoneOn` 병행 — 수화기 전환은 레거시 off 먼저) + **적용 검증·자가 재에지**(`verifyRoute`: 적용 0.8s 후 `getCommunicationDevice` 가 의도와 다르면 모드 반납→150ms→재claim 으로 에지 재생성, 통화당 2회 — claim 에지가 타이밍 경쟁으로 유실된 경우 자기 치유). 이어폰 연결/해제는 `AudioDeviceCallback` 재적용.
    - 통화 종료: volte 해제(모드 NORMAL 에지) 후 **300ms 지연** RESUME → PTT `setAudioRoute` 재적용(새 에지로 PTT 재기록) + PTT 측 복귀 검증·자가 재에지(`verifyResumeRoute`, 스피커폰 의도 시 1.2s 후 판정 2회). RESUME 유실(volte 사망)은 11분 워치독이 복귀.
- **하드웨어 PTT/SOS 버튼**(`HwPtt.kt`): 하드웨어 버튼 단말은 화면 PTT 버튼을 숨기고 안내 문구로 대체. **PTT/SOS 키 분류는 `HwPtt.classify(keyCode)`** — 두 측면 버튼은 **별개 keycode**(W999 실측 keyDown 로그: PTT(1번째)=**309**[scan 87, dev3], SOS(2번째)=**310**[scan 231, dev2·gamepad-class])라 keycode 만으로 구분한다. 일반 단말 폴백=F11→PTT/F10→SOS. 키 수신 경로 3중(어느 경로든 동일하게 pttDown/Up·`startEmergency` 로 유입, 마이크 점유도 동일 — down=`setTalkCapture(true)` 획득/up=해제):
  - **접근성 키 필터(`PttKeyService`) — 정본 경로**: `AccessibilityService`+`FLAG_REQUEST_FILTER_KEY_EVENTS` 로 시스템 전역 KeyEvent 를 수신 → **앱이 백그라운드(다른 앱/홈 화면)여도 측면 버튼 동작**. 선탑재 PoC 앱(Corget)과 동일 방식. PTT/SOS 키는 여기서 소비하므로 전면일 때도 Activity 와 이중 처리되지 않고, 버튼 학습도 같은 규칙으로 처리한다. 프로세스 사망 시 키 down 으로 `PttService` 재기동(해당 눌림은 유실). **설정→접근성에서 "CIMS PTT 버튼" 1회 활성화 필요** — 설정 탭 "백그라운드 PTT 버튼" 행이 상태 표시+설정 화면 바로가기. (adb: `settings put secure enabled_accessibility_services <기존값>:com.cims.ue.ptt/.PttKeyService`)
  - **Activity 폴백**: 접근성 미활성 시 `MainActivity.dispatchKeyEvent`(전면 전용)→pttDown/Up, SOS down=`startEmergency`.
  - **벤더 브로드캐스트 폴백(`VendorPttReceiver`)**: 러기드 프레임워크의 전역 방송 관례(`android.intent.action.PTT.down/up`, Motorola Solutions 계열 — W999 선탑재 Corget 의 수신 액션에서 확인) 수신. 접근성 연결 중/직전 1.5s 내 실제 키 이벤트 처리 시 무시(같은 눌림 이중 트리거 방지), up 미방송 벤더 대비 35s 워치독으로 발언 강제 해제. 매니페스트 정적(사망 시 재기동용)+`PttService` 동적(실행 중 배달 보장) 이중 등록, 300ms 디바운스로 1회 처리.
  - 백그라운드 발언의 FGS microphone 승격(API 34+ while-in-use)은 기존 `elevateForCall` 경로가 처리 — `SYSTEM_ALERT_WINDOW`(상태 배지 오버레이) 보유로 백그라운드 마이크 접근 면제 대상.
  - **버튼 학습(설정)**: 기종마다 측면 키 keycode 가 달라 하드코딩만으로는 신규 단말 커버 불가 → 설정 화면의 **하드웨어 버튼 설정** 오버레이에서 사용자가 "설정" 후 실제 버튼을 눌러 keycode 를 학습·영속(`SharedPreferences` `hw_ptt`, `startLearn`/`consumeLearn`). 학습값이 있으면 `classify` 가 우선 적용, 없으면 내장 기본(309/310+F11/F10). 시스템 네비 키(뒤로/홈/볼륨/전원, `isSystemNav`)는 학습 제외. 🔑설정 UI 는 **별도 Dialog 윈도우가 아니라 같은 Activity 윈도우 안의 오버레이**(`KeyConfigOverlay`)로 그린다 — gamepad-class 측면 키는 별도 Dialog 가 포커스를 잡으면 focus 네비게이션에 소비돼 앱으로 오지 않으므로, Activity 가 키 포커스를 유지해 물리 키가 `dispatchKeyEvent`→`consumeLearn` 으로 유입되게 한다. 실기기 학습 검증: `learned SOS = keycode 310`/`learned PTT = keycode 309`.
  - **존재 감지 3중화**(화면 PTT 버튼 숨김 판단)=①과거 PTT 키 수신 이력·학습값(영속) ②기종 allowlist(UNIWA W999 — GPIO 장치가 앱 InputDevice 열거에 미노출) ③입력장치 PTT/F11 능력 스캔. 컨트롤러 접근은 `PttService.controllerFlow`(StateFlow — 바인드 후 늦게 생성되는 컨트롤러도 UI 재구성). ⚠️일부 러기드 단말은 `persist.log.tag=I` 로 Log.d 전역 차단 — 진단 로그는 Log.i.
- **착신 그룹콜 자동 수락**(ptt_ue.md §12.3): 수신 INVITE 원문에 `mcptt-info` 존재 → `CallState.Incoming.mcptt` → `PttController.autoJoinGroupCall`(floor 소켓 개설+`answerGroupCall` 로 응답 SDP 에 m=application 주입). 전제=**자동 affiliation**. CSP 는 affiliation 된 멤버에게만 그룹 INVITE fan-out(`require_affiliation` 그룹). MCPTT 토큰(TS 33.180) 주입은 **서비스 레벨**(`PttService.injectSsoToken` — CIMS 로그인/부팅 autostart 만으로, PTT 앱을 열지 않아도 그룹 조회→affiliation PUBLISH 까지 진행)과 UI(AppRoot, 앱 오픈 시) 이중 경로 — 토큰 보유 시 서비스 주입은 생략(멱등).
- **자동 affiliation 상태 머신**(TS 24.379 §9, `PttController`): **희망 집합=편성 채널 전체**(CSC 그룹 목록+영속 참여 채널+선택 채널)를 서버 확인 기반으로 유지한다 — 로그인(등록)만으로 전 채널의 fan-out 수신이 성립.
  - **발행 트리거**: 등록 성공·그룹 목록 적재·채널 선택/키업 시 `affiliateAll`/`ensureAffiliated` — 확정이 신선하면(잔여 수명 > TTL 절반) 생략, 같은 그룹 in-flight 중복 억제.
  - **응답 기반 확정**: PUBLISH(`Event: mcptt` 필수 — 없으면 CSP 489)는 token 으로 최종 응답과 상관(`Account.onSendRequest` → `SipController.sendReqResults`). 2xx 에서만 [affiliated] 확정+만료 기록 — **송신만으로 성공 처리하지 않는다**(과거 낙관 기록이 403 후 영구 미재시도 사고의 원인).
  - **실패 재시도**: 403(비멤버 — 그룹 편성이 PUBLISH 보다 늦는 레이스 포함)·오류는 지수 백오프(30s→60s→120s→240s, cap 300s) 재시도. 무응답은 40s 후 pending 회수(주기 루프가 재발행). 백오프 대기 중인 그룹은 주기 루프가 발행을 생략한다(두 경로 중복 발사 억제).
  - **403 = 등록 소실 대응**: 서버가 등록을 잃으면(CSP 재기동 등) PUBLISH 는 `not registered` 403 으로 **시간이 지나도 낫지 않는다** → 백오프와 별개로 `SipController.refreshRegistration()`(60s 스로틀)로 **즉시 등록 갱신**을 트리거한다. 미조치 시 단말 자체 갱신 시점(Expires ≈1h)까지 제휴·fan-out 공백(= require_affiliation 그룹에서 무전 불가)이 이어진다. 등록이 서버에서 사라졌다면 **구독도 함께 사라졌다** — `refreshRegistration()` 은 성공 시 pjsua 계정 상태를 Registered 에서 내리지 않아 `regState` 전이 기반 정리가 돌지 않으므로, 이 지점에서 구독 확인 상태(conference/gms)도 함께 비워 다음 `syncRosterSubs()`(60s 주기·조인·그룹목록 적재)가 재발행하게 한다. ⚠️`register()` 는 Account 재생성이라 프로세스 내 PJSIP 재부팅 지뢰 — 등록 갱신에는 쓰지 않는다. 남은 갭: 등록 소실을 PUBLISH 시점(TTL 절반)에야 감지 — 능동 감지(짧은 Expires·OPTIONS·reg-event 구독)는 후속.
  - **주기 갱신**: 60s 루프가 잔여 수명 TTL(Expires 3600) 절반 미만인 그룹을 재-PUBLISH — 만료 방치로 fan-out 이 조용히 죽는 것 방지. de-affiliate(Expires:0)는 명시 호출 시에만.
- **참여 채널 자동 복원**(`ChannelStore` — `ptt_channels` SharedPreferences): 참여 "의도"(joined 목록+주채널)를 영속화해 프로세스 재시작(강제종료·재설치·리부팅) 후 등록 완료 시 1회 재조인한다(재로그인 경로는 서버 fan-out INVITE 가 먼저 올 수 있어 3s 양보). 서버/네트워크 사정으로 세션이 끊겨도 지우지 않으며, **사용자가 명시적으로 나가면 제거**(재조인 의도 해제) — 로그아웃 시에는 `SuiteLogoutReceiver` 가 `clear`. 🔑복원 1회 플래그는 **스토어 배선 확인 뒤에** 소모하고, 스토어가 늦게 주입되면 setter 가 복원을 재트리거한다 — force-stop 후 접근성(PttKeyService) 리바인드가 프로세스를 **헤드리스**(UI·서비스 미배선)로 먼저 살리면 등록·제휴는 진행되지만 스토어가 없어, 플래그를 먼저 세우면 이후 사용자가 앱을 열어도 복원이 영구 스킵된다.
- **참가자 목록 = conference 정식 구독(RFC 4575 / RFC 6665)**: `PttController.subscribeRoster` 가 그룹 AoR 로 `SUBSCRIBE (Event: conference)` 를 보내고, CSP 는 그 구독 dialog 로 로스터 NOTIFY 를 보낸다.
  - **구독 상태는 서버 확인 기반으로 관리한다** — affiliation 의 `affiliated` 와 같은 원칙.
    SUBSCRIBE 를 보냈다는 사실만으로 "구독 중"으로 취급하면, 서버가 구독을 잃고(CSP 재기동 =
    in-memory 구독 소멸) 단말이 등록 끊김을 관측하지 못한 경우 멱등 가드가 재발행을 영구히 막아
    **로스터·편성 push 가 앱 재시작 전까지 얼어붙는다**(실측). 확인 신호는 **NOTIFY 도착**이다
    (네이티브가 SUBSCRIBE 응답을 앱에 올려주지 않고, CSP 는 구독 수락 직후 초기 NOTIFY 를 항상
    보낸다 — conference 는 로스터가 비어도, gms 는 그룹별로). 따라서 `confirmedRosters`(NOTIFY 로
    확인) 와 `pendingRosters`(발행 시각) 를 분리하고, 확인 대기가 시한을 넘기면 다음 트리거가
    재발행한다. 확인 판정은 **그룹 AoR 발신 NOTIFY 경로만** 근거로 삼는다 — 통화 다이얼로그로 오는
    in-dialog 폴백 NOTIFY 는 구독의 증거가 아니다.
  - **재확인은 주기적으로 한다 — 구독 소멸은 감지할 수 없다.** native evsub 이 in-dialog 갱신 중
    481 을 받아 구독을 접어도 앱에는 통지가 없다. 따라서 `SUB_REASSERT_MS`(10분)마다 SUBSCRIBE 를
    다시 던진다: 살아 있는 구독은 native `cims_conf_find` 가 in-dialog 갱신으로 흡수하고(CSP 는
    갱신에도 `SendInitialNotify` 를 보내므로 확인 시각이 갱신된다), 죽은 구독은 새로 만들어진다 —
    감지 없이 수렴한다. affiliation 을 TTL 절반마다 재-PUBLISH 하는 것과 같은 형태다.
  - **세션 종료 시점에는 확인을 무효화한다.** 그룹콜 세션이 끝나는 순간은 서버측 구독이 사라진 채
    발견된 실측 지점이다(단말은 구독이 살아 있다고 믿는데 서버엔 없어 로스터가 죽는다 — 그 상태의
    단말은 통화 dialog in-dialog 폴백으로만 로스터를 받으므로, **자기가 마지막으로 이탈하면 leg 이
    사라져 아무 통지도 못 받고 자기 화면에 자신이 접속 중으로 남는다**). 그래서 세션 종료 경로에서
    해당 그룹의 확인을 무효화해(`invalidateRosterConfirm`) 바로 뒤의 `syncRosterSubs()` 가 즉시
    재확인하게 한다 — 해지가 아니며, 살아 있으면 in-dialog 갱신으로 흡수된다. 재확인 주기(10분)를
    기다리면 그 사이 로스터가 죽은 채 남는다.
  - ⚠️**구독 복구는 등록 복구에 종속된다.** CSP 는 미등록 사용자의 SUBSCRIBE 를 401 로 거절한다
    (`CscfModule::RecvRequestSubscribe`). CSP 재기동은 등록과 구독을 동시에 날리므로, 재확인이
    돌아도 등록이 살아나기 전에는 401 이다. 즉 실질 복구 시한은 **등록 소실 감지 latency**(현재
    제휴 PUBLISH 의 TTL 절반 ≈30분)가 지배한다 — 위 "403 = 등록 소실 대응" 참조. 미등록 중의
    재확인 실패는 10분 뒤 재시도 또는 403 경로의 상태 초기화로 흡수된다.
  - **구독 대상 = 참여 채널이 아니라 제휴(편성) 채널 전체.** 등록 완료·그룹 목록 적재·60s 제휴 루프에서 `syncRosterSubs()` 가 희망 제휴 집합과 구독 집합의 차이만 맞춘다(신규 구독 / 빠진 채널 `Expires: 0`). 따라서 **채널을 이탈해도 구독은 유지**되고, 참여하지 않은 채널의 접속 인원도 계속 보인다. 해지는 편성에서 빠지거나 등록이 끊길 때만.
  - **미조인 채널 로스터**는 세션이 없으므로 `PttController.rosterMap`(→ `channelRosters` StateFlow)에 담아 목록/상세 화면이 소비한다. 참여 중인 채널은 세션 `participants` 와 같은 값이다. ⚠️"본인은 항상 접속"은 **참여 중일 때만** 적용한다 — 미조인 채널에 자신을 넣으면 참여하지도 않은 채널에 내가 있는 것으로 보인다.
  - ⚠️**멱등 필수**: 등록·제휴·조인이 각자 구독을 트리거하므로 가드가 없으면 같은 그룹에 SUBSCRIBE 가 동시에 두 번 나가 서버에 구독이 중복 생성된다(실측). native 의 `cims_conf_find` 가 URI 로 기존 구독을 찾아 in-dialog 갱신하지만, 첫 구독이 테이블에 등록되기 전 두 번째 호출이 들어오면 경합한다 → 앱이 1회만 발행한다. 단 그 가드는 **발행 후 확인까지의 창**에만 걸린다 — 확인(NOTIFY) 없이 `SUB_CONFIRM_TIMEOUT_MS`(15s) 가 지나면 재발행 대상으로 되돌린다. 단말은 **200 OK** 로 응답하고 본문은 `Account.onInstantMessage` → `SipController.incomingMessage`(contentType=`application/conference-info+xml`, fromUri=그룹 AoR=focus) 로 올라와 그룹 키로 세션을 찾아 반영한다. 구독 생성·**in-dialog 갱신**·종료·매칭 없는 NOTIFY 의 481 응답은 native pjsip evsub 이 담당하므로(빌드 패치 [2-13]) 앱은 "언제 어느 그룹을 구독할지"만 정한다 — `Account::sendRequest` 를 그대로 쓰기 때문에 **SWIG 인터페이스 변경이 없다**. ⚠️구독은 단발 트랜잭션이 아니어서 결과가 `sendReqResults` 로 오지 않는다(확인 신호 = NOTIFY 도착).
- **설정 변경 push = XCAP 구독 2축(RFC 5875 xcap-diff)**: 등록 완료 시 서버 PSI 두 곳으로
  `SUBSCRIBE (Event: xcap-diff)` 각 1건 — `sip:gms_psi@<domain>`(편성)과 `sip:cms_psi@<domain>`
  (사용자 프로파일·시스템 설정). `PttController.subscribeXcap(kind, on)` 하나가 두 축을 다루고
  확인/재확인 상태도 축별로 관리한다(`xcapConfirmedAt`/`xcapPendingAt` — 로스터 구독과 같은 규율:
  확인 신호는 그 축의 NOTIFY 도착, 15s 무확인이면 재발행, 10분마다 재확인). CSP 는 **SUBSCRIBE 의
  Request-URI** 로 축을 가르므로(`CscfModule` 의 gms/cms 판별) PSI 이름 자체가 계약이다.
  NOTIFY 본문은 **"어느 문서가 바뀌었고 새 ETag 는 무엇"뿐**이라(2단 구조) 앱은 `onXcapDiff` 에서
  `sel` 로 축을 갈라 실제 문서를 XCAP HTTP GET 한다(전부 ETag 캐시):
  - `org.openmobilealliance.groups/...` → 마지막 `tel:` 세그먼트가 바뀐 그룹 → **`loadGroups()` +
    `loadGroupDetail(그룹)`**. `loadGroups()` 는 이어서 `affiliateAll()`·`syncRosterSubs()` 까지
    부르므로 **새로 편성된 채널이 제휴·로스터 구독까지 자동으로 따라온다**.
  - `org.3gpp.mcptt.user-profile/...` → **`loadUserProfile()`** · `org.3gpp.mcptt.service-config/...`
    → **`loadServiceConfig()`**. CSP 는 cms 축 NOTIFY 에 두 sel 을 항상 함께 싣고 구독 수락 직후의
    초기 NOTIFY 에도 싣는다 — 그래서 **구독 성립만으로 두 설정 문서가 즉시 적재**된다.
  구독 확인(어느 축의 NOTIFY 인가)은 **본문이 아니라 notifier 신원**(From = 서버 PSI)으로 판정한다 —
  변경 문서가 없는 NOTIFY 는 빈 `xcap-diff` 라 본문만으로는 축을 알 수 없다.
  native 는 conference 와 **같은 evsub 기계**를 쓴다(빌드 패치 [2-13] 이 두 패키지를 함께 등록 —
  구독 식별은 (자원 URI, 이벤트 패키지) 쌍). 축이 둘이어도 PSI 가 다르므로 별개 dialog 다.
  ⚠️**편성(gms) 축은 즉시가 아니다** — CSP 의 통지원이 `SyncGroupsState()`(**60초 주기** 그룹 재적재의
  멤버 해시 비교)라 편성 변경 반영까지 최대 60초 걸린다. CSC → CSP 즉시 경로(UDP `GROUP_CHANGED`
  → `OnGroupConfigChanged`)가 따로 있으나 현재 도달하지 않는다(아래 미해결 참조).
  **cms 축은 즉시**다 — 프로파일 PUT 이 `USER_CHANGED` 를 보내고 CSP `SendSipNotify` 가 그 자리에서
  구독자에게 NOTIFY 한다(주기 비교가 개입하지 않는다).
- **폴백(구 버전 서버·구독 미구현 단말 혼재용)**: CSP 가 구독 없는 leg 에 보내는 통화 dialog in-dialog NOTIFY 는 `CimsCall.onCallTsxState` 의 수신 원문에서 파싱한다(`SipController.conferenceInfo` SharedFlow). pjsip 은 evsub 미소유 NOTIFY 에 500 을 응답하지만 invite usage 의 tsx 이벤트로 원문이 전달된다. 본문이 항상 full 스냅샷이라 두 경로가 겹쳐도 결과가 같다.
- **NAT 경로 개방** (요건 정본: [ue_nat_traversal.md §7.1](ue_nat_traversal.md#71-ue-구현-요건-ptt)): PTT 는 발언 중에만 상향이 흐르므로, 청취 전용 상태의 하향(floor 알림·오디오)은 단말이 각 소켓의 NAT 매핑을 열고 **유지**해야 성립한다. ①floor — 연결 직후 1회 + 주기 15s **Floor Ack(User ID 포함)** 송신(`FloorClient` 내장 keepalive, `connectRemote` 시 시작) → CMP 가 User ID 로 멤버를 식별해 floor 주소 latch(TAKEN/GRANT 수신 가능). ②오디오 — PJSIP RTP keepalive(RFC 6263, empty RTP 주기 5s): `m1_build_pjsip.sh` 가 생성하는 config_site.h 의 `PJMEDIA_STREAM_ENABLE_KA=1`(pjsip 기본값 0 — CIMS 빌드가 활성) → CMP 목적지 latch. UAC 발신 응답의 floor 목적지는 `onCallTsxState` 의 200 OK 원문에서 학습(onCallSdpCreated 는 로컬 SDP 생성 시에만 호출됨). floor UDP 송신은 전용 스레드(main 스레드 send 는 NetworkOnMainThreadException).
- **멀티그룹 동시 참여**(TS 22.179 group scanning): 한 단말이 N개 그룹에 동시 참여한다. 그룹별로 독립 SIP 다이얼로그+floor 소켓+FloorClient 를 가지며(`PttController.Session`), UI 는 주채널 탭(주채널 전면 패널)과 전체채널 목록으로 나열한다. **주채널(primary)** 지정은 주채널 탭의 선택 시트·채널 상세의 버튼(`setPrimary` — 나머지 참여 그룹은 일반 참여, 별도 역할 없음), **듣기 정책**(주채널만 / 전체듣기)은 주채널 탭 컨트롤 행·설정 화면 토글로 그룹별 `setCallListen`(하향 오디오 mix on/off)을 적용한다. **PTT 발언은 주채널 세션만** 대상(`pttDown`/`pttUp`). 그룹별 나가기(채널 상세)·참여는 독립적. 발언 상태 패널은 주채널 기준으로 파생하되 비주채널 화자는 `[gNNN]` 태그를 붙인다. per-call 문맥 분리 필수: floor 목적지/conference 정보는 callId 로 구분, SDP 주입은 `CimsCall.pendingAppSdp`(전역이면 그룹 간 혼선).
- **서버측 멀티그룹 정합**(CSP `GroupCallService`): 활성 콜 추적 키가 `(userId, groupId)` — 사용자가 여러 그룹에 동시 참여하는 것을 전제한다. 다른 멤버가 같은 그룹을 개시(originate)해 fan-out INVITE 가 와도, 이미 **라이브 SIP 다이얼로그**(UA 다이얼로그 맵으로 판정 — 개시자 AcceptCall 레그·CSP StartCall 레그 모두 포함)를 가진 멤버는 재초대하지 않는다(과거엔 선참여 멤버를 stale 로 오판해 LEAVE+재INVITE → 단말이 재INVITE 미응답 → CMP 멤버십 이탈하는 좀비 상태였음). `ClearUserCall` 은 해당 사용자의 **모든** 그룹 콜을 정리.
- **긴급(SOS)** ([mcptt_emergency_modes.md](mcptt_emergency_modes.md), TS 24.379): 개시=하드웨어 SOS 키(down 1회) 또는 화면 SOS 버튼(주채널 탭 컨트롤 행, 오발동 방지 **길게 누름**) → `PttController.startEmergency()` — 주채널 통화 중이면 **in-dialog re-INVITE**(mcptt-info `emergency-ind=true`, `SipController.reinviteWithBody`)로 상향, 미참여면 긴급 그룹콜 발신(`joinGroupCall(emergency=true)`). CSP 가 그룹 capability(`ppt_groups.emergency_call`) 미허용이면 normal 로 하향 수용(로그 `emergency not allowed → downgrade`). **해제는 화면 배너에서만**(개시자 전용 — 서버도 비개시자의 취소 re-INVITE 를 무시, `emergency-ind=false` 명시 송신). 긴급 세션의 PTT 발언은 Floor Request 에 **Floor Indicator emergency 비트**(TS 24.380 §8.2.3.13)를 실어 CMP tier 상향·선점. **수신측 표시** 경로 2개: ①미참여 멤버=fan-out INVITE 의 emergency-ind(`CallState.Incoming.emergency`) ②참여 중 멤버=CMP 가 긴급 발언자의 TAKEN 방송에 싣는 emergency 비트(latch — CSP 는 in-call 상향을 re-INVITE fan-out 하지 않음; 취소 역시 전파되지 않아 수신측 latch 는 세션 종료 시 해제). UI=붉은 점멸 배너(그룹/개시자, 개시자에게 해제 버튼)+그룹행 [긴급] 배지·적색 하이라이트+SOS 버튼 활성 점멸, 톤=`PttFeedback.emergencyTone()`(긴 경고음+삼중 진동).
- **레거시 폴백**: 서버는 DTMF(PT=101)도 floor 트리거로 허용([ptt_flows.md](ptt_flows.md) C1). 1차 구현은 RTCP-APP(TS 24.380) 정공법 사용.

### 5.4 서버 규격 정합에 따른 단말 구현 요구사항 (TS 24.380)

서버(CMP)는 TS 24.380 V17.7.0 원문 대조로 floor 규격 정합을 마쳤다(정본
[mcptt_standard_conformance.md](mcptt_standard_conformance.md) §1). 그 결과 **서버가 새로 보내는
필드·메시지**와 **서버가 단말에 기대하는 동작**이 생겼다. 아래는 그 델타를 단말 관점에서
모은 목록이다 — 상태는 `android/ptt-client/` 기준이며, 서버는 모두 반영·검증 완료다.
✅=반영 · △=부분(소비처 대기 또는 확인 필요) · ✗=미구현.

#### (A) 상호운용 필수 — 없으면 발언이 끊기거나 상태가 어긋난다

| # | 요구사항 | 규격 | 서버 동작 | 단말 현재 |
|---|---|---|---|---|
| U1 | **Ack 요구 변종 처리** — 수신 subtype 의 첫 비트(0x10)를 걷어내 기본 타입으로 처리하고 **Floor Ack**(Source(10)=0 floor participant + Message Type(12)=대상 subtype)로 회신 | §8.2.2, §8.2.13 | 수신 처리·회신 구현. 송신은 ack 비트 0 | ✅ `FloorCodec.decode` 가 `FloorMsgType.op()` 로 기본 타입 + `ackRequired` 분리, `FloorClient.handle` 이 상태 처리 **전에** `FloorCodec.ackOf` 로 회신 |
| U2 | **Floor Revoke 에 Floor Release 로 응답** — 회수 통지를 받으면 mic 차단 + **Release 송신**, T100(권장 재전송)으로 도달 보장. G-bit(dual)가 서 있으면 Release 에도 G-bit | §6.2.4.5.4 | Revoke 후 T3(기본 3초) 유예 동안 Release 를 기다리고, 그 사이 T8(1초)로 Revoke 재전송. 미응답이면 강제 회수 | ✅ `FloorClient.sendRevokeRelease` — 즉시 Release + 800ms×2 재전송(서버 T3 3초 창 안), 수신 indicator 의 G-bit 되싣기, Revoke 재전송에는 Release 만 재송신(알림 1회) |
| U3 | **Granted Duration 준수** — Duration(1)=이번 발언 허용 시간(T2). 잔여시간 표시·임박 알림, 초과 전 자체 종료 | §6.3.4.4.2-1a, §6.3.4.4.4 | 초과 시 Revoke **cause #2**(Media burst too long) | ✅ `PttController.armTalkLimit` — 발언 스트립 "남은 N초"(마지막 10초 경고색), 마감 5초 전 `PttFeedback.talkLimitTone`, 마감 300ms 전 자체 Release. Duration 0(서버 `FloorStopTalkSec=0`)은 무제한으로 해석 |
| U4 | **발언 중 RTP 연속성** — 발언 중 상향이 T1(기본 4초) 이상 끊기면 서버가 발언 종료로 보고 회수 | §6.3.4.4.3 | T1 만료 시 **Revoke 없이** 회수 후 IDLE/0x0F | △ DTX·홀드 구간에서 무음 지속 시 회수될 수 있음 — 확인 필요 |
| U5 | **Deny cause 구분 UI** — #1 다른 참가자 점유 / #3 1인 세션 / #5 수신 전용 / #7 큐 포화 | §8.2.6.2 | 상황별로 정확히 구분해 송신 | △ `FloorCause.REJECT` 매핑 문구를 상태줄에 그대로 표시 — 한글화·행동 안내는 후속 |

#### (B) Floor Taken 신규 필드 — 표시·정합에 직접 쓰인다

| # | 요구사항 | 규격 | 서버 동작 | 단말 현재 |
|---|---|---|---|---|
| U6 | **Permission to Request the Floor(5)** — 0 이면 발언 요청 불가(broadcast 그룹·ambient 청취 leg) → PTT 버튼 비활성 | §8.2.3.7, §6.3.4.4.2-3d | broadcast=0, ambient(recv_only) leg 에는 0 변형을 따로 송신 | ✅ `GroupCallState.canRequestFloor` — 화면 PTT 바 비활성("청취 전용 채널")·`pttDown` 조기 차단. 값이 실려 올 때만 갱신(미포함 = 종전 유지) |
| U7 | **Message Sequence Number(8)** — Taken/Idle 의 순서 식별. 역전·중복 수신 시 오래된 것 폐기 | §8.2.3.10 | Taken/Idle 마다 +1(65535 순환) | ✅ `FloorClient.isStaleSeq` — 직전 64개 안쪽으로 되돌아간 Taken/Idle 만 폐기. 더 멀리 뒤로 간 값은 서버 카운터 초기화로 보고 재동기(폐기하면 표시가 영구 정지) |
| U8 | **SSRC 필드(14)** — 화자의 RTP SSRC. **헤더 SSRC 는 서버 SSRC** 이므로 화자 식별에 쓰면 안 된다 | §8.2.5, §8.2.9 | Granted/Taken 에 화자 SSRC 를 필드로 실음(단말이 보낸 SSRC 를 학습해 되싣는다) | △ `FloorMessage.speakerSsrc` 로 파싱해 `FloorEvent.Taken` 으로 전달(헤더 SSRC 는 화자 식별에 쓰지 않는다). 실제 소비처는 U10(SSRC 별 재생) |
| U9 | **Granted Party(4) = MCPTT ID(URI)** — 표시 시 URI 를 사용자 이름으로 매핑 | §8.2.3.8 | `PTT_JOIN.user_uri` 가 있으면 URI, 없으면 가입자 번호 | △ 문자열 그대로 표시(`sameUser` 가 URI/번호 혼용을 흡수) |

#### (C) 동시 발언(dual/multi-talker) — 규격상 **믹싱은 단말 몫**

TS 24.380 §6.2.4.3.4 NOTE: *"RTP media packets can be received from multiple sources … The MCPTT
client can differentiate between the different sources using the **SSRC** … How the **media mixer in
the MCPTT client** mixes the different RTP media stream sources is out of scope."* — 서버는
media distributor 로서 화자별 스트림을 **슬롯별 SSRC** 로 분리해 그대로 보낸다(믹싱 없음).

| # | 요구사항 | 규격 | 서버 동작 | 단말 현재 |
|---|---|---|---|---|
| U10 | **SSRC 별 병렬 수신·로컬 믹싱** — 같은 RTP 포트로 오는 N개 스트림을 SSRC 로 갈라 지터버퍼·디코더를 병렬 구동하고 합성 재생 | §4.2.2, §6.2.4.3.4 | 슬롯별 egress SSRC(슬롯0=종전 고정, 슬롯N=별도 공간)로 분리 송신 | ✅ 구현 — pjproject 패치 `[2-14]`(`stream.c` 내부 SSRC 디먹스, `get_frame` PCM 합산, RTP 무활동 회수). 네이티브가 도착 SSRC 로 서브스트림을 자동 생성/회수하므로 SWIG·앱 불변(`Session.talkerSsrc` 는 발언 스트립 UI 에만 사용). 실기기 3대 실호 검증 대기(WSL2 빌드). 정본 = [mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md) |
| U11 | **List of Granted Users(15) + List of SSRCs(16)** — 동시 발언 시 Taken 이 싣는 화자 목록. 순서가 서로 대응한다 | §8.2.3.17~18, §6.3.4.4.7a | 화자 2명 이상이면 단일 Taken 에 두 리스트를 실어 송신 | ✅ `FloorMessage.talkers` 가 리스트(없으면 Granted Party+SSRC)에서 화자 집합을 만들고, `FloorClient.talkers` StateFlow + `GroupCallState.talkers` 로 UI 까지 전달. 발언 스트립·채널 목록·구성원 명부가 전원을 표시 |
| U12 | **Floor Release Multi Talker(0x0F) 수신** — 동시 발언 중 한 화자의 종료 통지. 해당 화자만 목록/재생에서 제거(Idle 아님) | §8.2.14 | 잔여 화자가 있으면 Idle 대신 0x0F 를 나머지에게 송신. **단말이 이 subtype 을 보내면 규격 위반이라 서버가 무시** | ✅ `FloorEvent.TalkerLeft` — User ID(없으면 SSRC)로 그 화자만 집합에서 제거하고, 잔여 화자가 있으면 LISTENING 유지(내가 남아 있으면 SPEAKING). 송신은 하지 않는다 |
| U13 | **Floor Indicator I-bit(0x0080)/G-bit(0x0200)** — multi-talker/dual floor 표시. 화자 목록 UI 제공 권장 | §8.2.3.15, §6.2.4.3.3-4 | 정책·화자 수에 따라 설정 | ✅ `GroupCallState.floorIndicator` 보관, 화자 2명 이상이면 발언 스트립에 전원 명단(+G-bit 면 "동시 발언(우선)"), 목록/칩에는 "외 N" |

#### (D) 협상·부가 기능

| # | 요구사항 | 규격 | 서버 동작 | 단말 현재 |
|---|---|---|---|---|
| U14 | **SDP fmtp 협상** — `m=application` 섹션에 `a=fmtp:MCPTT mc_queueing;mc_priority=N[;mc_granted]` 를 실어야 큐잉·우선순위 상한·초기 발언권이 성립한다 | §12.1.2.3, §6.3.5.4.4 | CSP 가 멤버 SDP(개시자=offer, 수신자=answer)의 fmtp 를 파싱해 `PTT_JOIN` 의 `queueing`/`max_priority`/`granted` 로 CMP 에 전달(`CGroupCallService::ParseMcpttFmtp`). fmtp:MCPTT 부재(레거시 단말)는 미전송 — CMP 기본(queueing 1) 유지. fmtp 는 있는데 `mc_queueing` 이 없으면 `queueing:0` — 비선점 요청은 **Deny #1** | ✅ 단말은 `PttController.floorSdp` 로 offer·answer 양쪽에 **`a=fmtp:MCPTT mc_queueing`** 송신(CSP·cspsim 도 psip `AddSdp` 로 같은 속성 광고). `mc_priority`·`mc_granted` 는 **의도적으로 미송신**(U15 / 채널 참여≠발언 요청) |
| U15 | **Floor Priority(0) 송신** — `mc_priority` 를 협상한 단말만 의미가 있다. 협상값과 요청값 중 **낮은 쪽**이 유효 우선순위이고, 미포함이면 서버 기본값 | §6.3.5.4.4-1a | `PTT_JOIN.max_priority` 가 있는 멤버만 요청값으로 낮춘다. **미협상 멤버의 우선순위 필드는 무시**(기본값 유지) | ✅ Floor Priority 필드를 **싣지 않는다**(`FloorCodec.request(priority = null)` 기본) — 서버 기본값이 유효 우선순위가 되어 U14 를 구현해도 0 으로 깎이지 않는다. 실제 요청 우선순위를 쓸 때만 명시 전달 |
| U16 | **Queue Position Info(9)** — 대기 위치 표시, 필요 시 Queue Position Request(8) 조회. 취소는 Queued Floor Requests(0x0E) | §8.2.11~8.2.12, §8.2.15 | 큐 진입·변동 시 위치 통지, 재요청에도 **위치 유지**. 목록 없는 0x0E 취소 = 요청자 본인 요청만 제거 | ✅ `GroupCallState.queuePosition` → PTT 바·발언 스트립에 "대기 N번째"(황색), 버튼을 떼면 `FloorClient.cancelQueuedRequest`(0x0E, 목록 없음) 로 취소 + Cancel Result/Notification 수신 처리. Queue Position Request(8) 폴링은 불필요(서버가 변동마다 통지) |
| U17 | **Unicast Media Flow Control(0x0B)** — 화면 꺼짐·데이터 절약 시 자기 하향 미디어 중단/재개 요청(Media Flow(24) MSB=1 재개) | §8.2.16 | 중단 요청한 leg 로 audio/video 미송신 | ✗ 미구현 |
| U18 | **floor SRTCP(CSK)** — TS 33.180 키로 floor RTCP 보호. 유니캐스트는 **클라이언트별 CSK** | TS 33.180 §9.4 | `PTT_JOIN.floor_crypto` 로 멤버별 키 수용(그룹 키도 지원). 키 배포는 CSC KMS 연동 대기 | ✗ 평문 floor |

#### 남은 순서

**floor 평면과 미디어 평면(U10) 을 반영했다** — U1·U2·U3·U6·U7·U11·U12·U13·U14·U15·U16 완료,
U8 은 파싱·전달까지(소비처가 U10), U10 은 네이티브 반영(실기기 검증 대기)
(`floor/{FloorControl,FloorCodec,FloorClient}.kt` · `PttController` · `ui/*.kt` ·
`audio/PttFeedback.kt`). 코덱 계약은 `ptt-client/src/test/java/.../FloorCodecTest.kt` 가 지킨다 —
ack 요구 변종, Floor Ack 의 Source/Message Type, Priority 생략, Release G-bit,
Taken 신규 필드(Permission·MSN·SSRC), 화자 집합 파생, 0x0F, 대기열 취소, 미지 필드 건너뛰기.

1. **U10 실호 검증** — 네이티브 반영은 끝났다(패치 `[2-14]`). WSL2 빌드로 `.so` 를 투입한 뒤
   실기기 3대(A·B 발언·C 청취)로 dual/multi 를 검증한다(현재 서버측만 CMP 프로브로 검증 —
   [../../VERIFICATION_MANUAL.md](../../VERIFICATION_MANUAL.md) 「floor 정책 시험」).
2. **U4·U5** — 발언 중 RTP 연속성 확인(DTX·홀드 구간에서 T1 회수 여부 실측)과 Deny cause 문구 한글화.
3. **U17·U18** — 화면 꺼짐 시 하향 미디어 중단(0x0B), floor SRTCP. U18 은 CSC KMS 가 키를
   내려준 뒤에 의미가 있다.

U10 의 문제 정의·선택지 비교·구현 설계와, floor 코덱 공유/정의 단일화 검토는
[mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md) 가 정본이다. 요지만 적으면 —
막힌 것은 믹싱이 아니라 **디먹스**다(conference bridge 는 이미 멀티그룹 오디오를 믹싱하고 있고,
화자 집합·화자별 SSRC 도 floor 평면이 넘겨준다). CMP 가 화자 슬롯마다 다른 SSRC 를 **같은 RTP
포트**로 보내는데 `pjmedia_stream` 은 스트림당 SSRC 가 하나라, 두 SSRC 가 섞이면 지터버퍼가 계속
리셋된다. 권고안은 pjproject 에 **SSRC 디먹스 전송 어댑터**를 넣는 것이며, **pjproject 소스와
안드로이드 빌드 환경이 있는 서버**에서 진행한다.

---

## 6. 미디어 파이프라인 ↔ PTT 매핑

PJSIP **conference bridge**의 슬롯 연결로 발언권/믹싱을 표현한다.

```
PTT down(GRANT): 🎤mic 슬롯 ──connect──► 통화 stream  (송신 개시)
PTT up(RELEASE): 🎤mic 슬롯 ──disconnect─ 통화 stream  (송신 중단)
수신 항상:        통화 stream ──connect──► 🔊spk 슬롯   (타 화자 청취)
```

- **AEC**: 안드로이드 오디오 장치(Oboe/OpenSL) + 플랫폼 AEC 또는 PJSIP AEC 중 택1 — M0에서 에코/이중통화 품질로 결정.
- **VoLTE(전이중)**: mic 슬롯 상시 connect. **PTT(반이중)**: 발언권에 따라 토글.
- 그룹 수신 믹싱은 CMP가 멤버별 SSRC/seq 재작성 후 전달하므로 단말은 단일 스트림 수신으로 처리.

---

## 7. CSC 설정 플레인 설계 (HTTPS, PJSIP 무관)

흐름(정본: [ptt_flows.md](ptt_flows.md) B2, [mcptt_api.md](../../api/mcptt_api.md)):

```
① OAuth2 PKCE: GET /idms/authreq(code_challenge S256) → {code}
              POST /idms/tokenreq(code, code_verifier) → {access_token(Bearer), refresh_token}
② GMS:  GET /org.openmobilealliance.groups/users/{me}        → 그룹 목록(JSON)
        GET .../{me}/{group}                                  → 그룹 상세(OMA POC XML)
③ CMS:  GET /org.3gpp.mcptt.user-profile/users/{me}/...       → 프로파일 XML
        GET /org.3gpp.mcptt.service-config/users/{me}/...     → 서비스설정 XML
④ 갱신: SIP SUBSCRIBE(xcap-diff) → NOTIFY(new-etag) → If-None-Match 재조회(304 캐시)
```

- 구현: **OkHttp + Kotlin**, PKCE(SHA-256/Base64URL), ETag 캐시. 참고 구현: `cims-phone/src/api/{idms,gms,cms}.ts`.
- 부팅 순서: **CSC 인증·그룹조회 → SIP REGISTER → SUBSCRIBE → (키업) PUBLISH/INVITE**.
- 식별자 연계: GMS의 `tel:{group}` → SIP `sip:{group}@domain`(INVITE/PUBLISH Req-URI), 프로파일 MCPTT ID → From/To.
- **CMS 문서 소비 (TS 24.484)** — 두 문서는 성격이 다르다. 사용자별 인가는 `user-profile` 의
  `ruleset`, 시스템 전역 정책은 `service-config` 이며, **게이트는 AND** 로 겹친다(시스템 정책이
  사용자 인가를 넓히지는 못한다는 규격 취지). 취득 계기는 cms 구독 NOTIFY(즉시)와 `loadGroups()`
  (구독이 없거나 죽었을 때의 폴백) 둘이고, 둘 다 `If-None-Match` 로 304 를 받는다.

| 문서 | 앱 상태 | 소비하는 값 |
|---|---|---|
| `user-profile` (사용자별) | `PttController.userProfile` | SOS 대상 결정(`entry-info` = DedicatedGroup / UseCurrentlySelectedGroup + `uri-entry` 전용 긴급그룹), `allow-emergency-group-call`, `allow-activate-emergency-alert`, `cims:allow-adhoc-group-call` |
| `service-config` (시스템 전역) | `PttController.serviceConfig` | `allow-private-call`(1:1 **발신**), `allow-emergency-call`, `allow-alert`, on-network `allow-transmit-request`(floor 요청), `max-on-network-affiliations-N2` |

- 정책 편집(서버): DB `mcptt_service_config` 단일 행이 SoT 이고, 콘솔 **구성 > MCPTT 정책**
  (`PUT /api/v1/mcptt/service-config`, manager+)에서 바꾼다. 사용자별 인가는 가입자 화면의
  user-profile 이다 — 단말은 두 축을 AND 로 본다.
- 게이트 지점: `startPrivateCall` · `startEmergency` · `sendAlert(activate=true)` · `pttDown` ·
  `startAdhocCall`. 전부 **UX 선차단**이고 최종 판정은 서버(403 / Floor Deny)다. 문서를 아직 받지
  못했으면 게이트를 걸지 않는다(`svcAllows` 가 null 을 허용으로 본다) — 설정 취득 실패가 기능
  정지로 번지지 않게.
- **착신·경보 취소는 막지 않는다** — 서버가 이미 성립시킨 세션을 단말이 거절하면 정책 판정이 두
  곳으로 갈린다. 이미 걸린 경보의 회수도 항상 허용한다.
- `N2`(동시 제휴 상한)는 **강제하지 않고 경고 로그만** 남긴다 — 앱이 잘라내면 어느 채널의 fan-out 을
  버릴지 정책 없이 결정하는 셈이다. 상한 집행은 서버 몫.

---

## 8. 안드로이드 런타임 설계

> **프로비저닝:** 서버(IP/포트/전송)·계정(도메인/MSISDN/이름/로그인ID/auth_id/비밀번호)은 **하드코딩하지 않고 CIMS SSO 자동 프로비저닝으로 수신·저장**한다. 설정 화면(`SettingsScreen`, 안드로이드 설정 스타일)은 SSO 구성 상태에서 **읽기 전용**이며, 테스트용 **수동 설정 모드**만 편집 허용 — [android_ue_provisioning.md](android_ue_provisioning.md) §5-1. 구현: `core`의 `SipAccountConfig`+`ConfigStore`. SIP 매핑: AOR=`sip:<msisdn>@<domain>`, Digest username=`imsi@domain`(또는 authId 전체 IMPI), Registrar=`serverHost:serverPort`. 비밀번호는 운영 시 EncryptedSharedPreferences/Keystore.

- **앱 UI 공통 디자인(3앱 통일)**: CIMS(로그인)·CIMS-Phone(volte-client)·McPTT(ptt-client) 모두 시안(`android/assets/pages/`) 기준 **다크 고정 + 민트 액센트** — 같은 디자인 토큰(`Ct`/`Cl`: Bg #0D1211·Surface #151C1A·Mint #5EE0C0 등, ptt `ui/Theme.kt`·volte `Theme.kt`·cims `LoginActivity` 내장)과 컴팩트 칩 스타일(`chipStyle` — includeFontPadding 제거), 다크 윈도우 배경(`themes.xml`, 콜드 스타트 포함), `yrt_logo`/`ic_launcher` 브랜딩을 공유한다. 통화 기능색(받기=초록/종료·거절=빨강/영상=파랑)은 전화 관습색 유지.
- **CIMS 로그인 앱 UI**(`cims/LoginActivity`, 시안 `로그인화면.png`): 중앙 로고+"CIMS 통합 로그인"+아이디/비밀번호 다크 라운드 필드(포커스=민트 외곽선)+큰 민트 [로그인] 버튼. 실패 시 붉은 외곽선 오류 박스. 로그인 상태면 "\<ID> 로그인됨" 칩 + 적색 외곽선 [로그아웃] 버튼(확인 다이얼로그 — 스위트 연동 종료, [android_ue_provisioning.md](android_ue_provisioning.md) §1-1). **서버 주소/포트는 '서버 설정' 접힘 행**(기본 접힘, 탭하여 펼침).
- **CIMS-Phone 홈 UI**(`volte-client/MainActivity`): 하단 내비 5탭(연락처/**통화이력**/키패드/문자/설정, PTT 앱과 같은 다크 바+민트 활성+문자 안읽음 뱃지 `DarkBottomNav`). **통화이력 탭**(시안 `통화이력.png`)=헤더(CIMS 라벨+제목+내 이름·번호)+**필터 칩**(전체/수신/발신/부재중)+**일자 섹션**(오늘/어제/yyyy.MM.dd)+행(좌 아이콘 박스: 발신=민트·부재중=적·영상=캠 아이콘 / 이름+음성·영상 태그칩+일시 / 우 ↑발신(민트)·↓수신·부재중(적)+**통화시간** mm:ss, 미연결 "—"). 행 탭=같은 유형(음성/영상)으로 재발신. **삭제 = 행 좌 스와이프(한 건, 빨간 배경+휴지통) 또는 선택 모드**(헤더 [선택]·행 길게 누름 → 행별 체크 원+[전체 선택/해제](현재 필터 결과 기준)+[선택 삭제 (n)]) — 한번에 전체 지우기 버튼은 없음(`CallLogStore.remove/removeAll`). 통화 기록은 core `CallLogStore`(`CallEntry`: number/type/time/**durationSec/video**)에 **종료 시점 1건 기록** — 발신은 `doDial` 시점에 미결 상태를 확정(즉시 실패 호는 StateFlow 컨플레이션으로 Outgoing 상태가 UI 에 관측되지 않을 수 있음), 수신은 Incoming 관측, 연결 시각(Active)부터 통화시간 측정, Incoming 미연결=부재중. 통화 화면(음성/수신)=상단 민트 상태 라벨+이니셜 아바타+큰 번호(영상 전체화면 UI 는 기존 유지).
- **전역 상태 아이콘 배지**(core `ui/StatusIconOverlay.kt`, `TYPE_APPLICATION_OVERLAY` — '다른 앱 위에 표시' 권한, 미허용 시 조용히 생략·양 앱 1회 안내 다이얼로그): 상태바 바로 아래에 **아이콘만** 있는 원형 배지(반투명 다크 원+아이콘, 텍스트 없음) — 상태는 tint 색(등록됨=초록/연결 중·대기=황색/해제=회색/실패=적색). 두 앱이 동시에 떠도 겹치지 않게 중앙 기준 자리를 나눔: **CIMS-Phone=전화 아이콘(좌 -22dp), McPTT=PTT 아이콘(우 +22dp)**. 갱신 주체=각 등록유지 서비스(`SipService`/`PttService` regState 관찰, main 스레드), 터치 통과(FLAG_NOT_TOUCHABLE). McPTT 는 `SYSTEM_ALERT_WINDOW` 매니페스트 선언 필수(미선언 시 canDrawOverlays 항상 false).

| 항목 | 설계 |
|---|---|
| 등록 유지 | **Foreground Service**(통화/대기 알림) + 부분 wakelock, 배터리 최적화 예외 요청 |
| Doze/네트워크 | FGS 유지 + 등록 refresh(서버 Expires 추종) + **등록 keepalive 보강**: ①PJSIP 등록 실패 자동 재시도(`regConfig.firstRetryIntervalSec=5`/`retryIntervalSec=30`) ②NAT 바인딩 UDP keep-alive(`natConfig.udpKaIntervalSec=15`, contact/via rewrite) ③기본 네트워크 복귀 시 재등록(`ConnectivityManager.registerDefaultNetworkCallback`→`SipController.reregister`) ④앱 포그라운드 복귀 시 재등록(`MainActivity.onResume`→`SipService.poke`) |
| 스레딩 | PJSIP 콜백=워커 스레드 → UI는 main으로 디스패치. **PJSIP 외 스레드에서 호출 시 `Endpoint.libRegisterThread()` 필수** |
| 객체 수명 | `Account`/`Call`/`AudioMedia` 래퍼 GC 방지(강참조 유지) + 명시적 delete |
| **백그라운드 착신** | 기본 전화앱처럼 앱 미실행/화면 꺼짐에도 착신: `SipService` 가 `CallState.Incoming` 에 **CallStyle 착신 알림**(받기/거절 액션) + `fullScreenIntent`(잠금·꺼진 화면이면 통화화면 직행, MainActivity `showWhenLocked`/`turnScreenOn`) + **벨소리 루프**(`RingtoneManager` TYPE_RINGTONE, `isLooping`). "받기"=MainActivity 경유(`EXTRA_ANSWER_CALL_ID`/`EXTRA_ANSWER_VIDEO`, 서비스 연결 후 응답), "거절"=서비스 액션(`ACTION_REJECT`). 서비스 기동 경로 3종: ①CIMS 로그인 직후(오너앱이 `startForegroundService`, exported+signature 권한 `com.cims.ue.permission.CIMS_SUITE`) ②부팅(BootReceiver) ③앱 실행. **FGS 타입 승격 정책(API 34+)**: 등록 유지=`specialUse`, `microphone` 승격은 **응답 후(Active)/발신(Outgoing)만** — `Incoming`(응답 전, 백그라운드) 승격은 플랫폼이 금지(`ForegroundServiceStartNotAllowedException`)라 시도하지 않고, 승격 실패도 `runCatching` 으로 `specialUse` 유지(통화 UI 포그라운드의 while-in-use 마이크로 동작). 서비스 코루틴 scope 에 `CoroutineExceptionHandler` — collector 예외로 프로세스가 죽지 않게(PTT 앱 동일) |
| **착신 영상 응답** | 수신 INVITE 원문 SDP 의 `m=video` 로 영상호 감지(`CallState.Incoming.video`) → 통화화면에 "영상" 응답 버튼(CAMERA 권한 확보 후 `answer(withVideo)`=`opt.videoCount=1`) |
| **로컬 카메라 프리뷰** | 영상 통화 중 우하단 PiP(내 화면): `VideoPreview`(전면 카메라 우선, `VidDevManager.enumDev2` 이름 "front" 매칭) + `SurfaceView.setZOrderMediaOverlay` — 호 종료/shutdown 시 자동 정리 |
| **발신번호 E.164 정규화** | 가입자 정본(IMPU/조회 키)은 E.164 — 키패드 로컬 표기("013…")를 그대로 보내면 CSP 404. `SipService.makeCall`/`sendMessage` 가 발신 직전 core `toE164`(cc=프로비저닝 countryCode, 폴백=내 msisdn ITU 유도 `countryCodeOf`)로 정규화 — 키패드/통화이력/연락처 공통. "+" 시작·비숫자(단축번호 등)는 그대로 |
| **문자(SIP MESSAGE)** | RFC 3428 page-mode 송수신: 송신=`SipController.sendRequest("MESSAGE")`(대상·저장 peer 모두 E.164 정규화 — 수신 스레드와 합치), 수신=`Account.onInstantMessage`→`SipController.incomingMessage`(SharedFlow)→`SipService` 가 **인박스 저장 + 알림**. 인박스=core `MessageStore`(상대별 스레드·안읽음 카운트, SharedPreferences+JSON). UI=문자 탭(스레드 목록→말풍선 대화·전송, 안읽음 배지) |
| 권한 | RECORD_AUDIO, CAMERA(영상), POST_NOTIFICATIONS, USE_FULL_SCREEN_INTENT·VIBRATE(착신), FOREGROUND_SERVICE(_MICROPHONE), 네트워크 |
| 오디오 포커스 | AudioManager 포커스 + 통화 라우팅(스피커/리시버/BT) |
| 하드웨어 PTT | 러기드 단말의 물리 PTT 키 매핑(KeyEvent/벤더 인텐트) — 옵션, M2+ |
| UX 모드 | VoLTE=전이중 다이얼러, PTT=반이중 푸시투토크(발언권 표시/대기열) |

---

## 9. 빌드 / 모듈 구조

### 9.1 코드 위치 (확정: 모노레포 `android/`, 공유 `core` + 클라이언트 2개)

```
cims/
  android/
    core/           ← 공유 Android Library: PJSIP 래퍼·MediaCodec 코덱(AMR-WB/H264)
                       ·SIP 등록/INVITE/RTP·미디어제어. PJSIP .so+SWIG Java 도 여기. (두 앱이 의존)
    volte-client/   ← VoLTE 1:1 SIP 소프트폰 (전화 앱) — core 의존
    ptt-client/     ← MCPTT 그룹 PTT 앱 — core 의존 + floor/affiliation/group/CSC
```

- **`core`**(공유): PJSIP 빌드 산출물(`.so`)+SWIG Java, `SipController`(등록/INVITE/RTP/SDP), MediaCodec 코덱 팩토리(AMR-WB/H.264), 미디어 제어(AEC/장치), 공통 모델.
- **`volte-client`**: `app`(Compose UI/Service/ViewModel) — core 기반 1:1 음성·영상.
- **`ptt-client`**: `app` + `floor`(MCPT RTCP-APP) + `csc`(OAuth2 PKCE+XCAP) + `group`/`affiliation` — core 기반.

> PTT = VoLTE(core) + affiliation/floor/group/CSC. 두 앱은 별도 APK로 배포하되 공유 `core`에 의존 → 중복·이중 유지보수 제거.

### 9.2 PJSIP 빌드

- `configure-android`(NDK) → `make dep && make`. **`config_site.h`**: `PJMEDIA_HAS_AND_MEDIA_AMRWB 1`(음성 정본) + `PJMEDIA_HAS_OPENCORE_AMR{WB,NB}_CODEC 0`(중복 등록 방지), And-Media H264(영상), 불필요 코덱 off — 정본은 [M1 설계서](android_ue_m1_pjsip_integration.md) §2.5.
- **SWIG** → `org.pjsip.pjsua2.*` Java + `libpjsua2.so`. Gradle 모듈로 패키징.
- ABI: `arm64-v8a`(필수) + 필요시 `armeabi-v7a`.
- 음성 AMR-WB는 PJSIP **내장 And-Media 코덱**(MediaCodec 구동, 2.16)을 사용 — 커스텀 코덱 팩토리 불필요.

---

## 10. 마일스톤 (리스크 큰 것 먼저, 차근차근)

| | 내용 | 검증 게이트 |
|---|---|---|
| **M0** 기반·리스크 | PJSIP 안드로이드 빌드(config_site.h+SWIG+Gradle) + **AMR-WB MediaCodec 코덱 팩토리 지연 스파이크(실기기/UNIWA)** + H.264 MediaCodec 가용성 확인 | mouth-to-ear 지연/드랍이 통화 가능 수준인지 정량 판정 |
| **M1** VoLTE 1:1 (음성+영상) | REGISTER(Digest)→발/착신 INVITE/SDP→**AMR-WB 음성 + H.264 영상** 양방향 RTP, BYE/상태머신 | 실서버와 1:1 음성·영상 통화 성공 |
| **M2** PTT 그룹콜 | affiliation(PUBLISH)+키업 그룹 INVITE(멀티파트 파싱)+FloorClient(MCPT)+그룹 음성/영상 | 그룹 PTT 발언권+음성, 선점/REJECT 동작 |
| **M3** CSC 설정 | IdMS OAuth2 PKCE + GMS/CMS XCAP + SUBSCRIBE/NOTIFY(xcap-diff) | 토큰·그룹·프로파일 취득 및 변경 반영 |
| **M4** 고급·배포 | emergency/imminent-peril, conference 멤버 표시, **하드웨어 PTT 키(UNIWA)**, **TLS 5061**, UX/패키징 | 긴급호·멤버상태·TLS·배포 |

> VoLTE 1:1(M1, 음성+영상)이 공통 SIP/미디어 코어를 가장 빨리 세우고, PTT(M2)가 그 위에 얹힌다. (영상은 음성과 함께 M1부터, TLS·긴급·하드웨어 PTT 키는 M4.)
>
> **M1 상세 설계(빌드 플레이북·SipController·코덱팩토리·시퀀스):** [android_ue_m1_pjsip_integration.md](android_ue_m1_pjsip_integration.md)

---

## 11. 리스크 & 대응

| 리스크 | 영향 | 대응 |
|---|---|---|
| **MediaCodec AMR-WB 실시간 지연** | 통화 품질 | M0 스파이크 측정 → 미달 시 opencore-amr 폴백(특허 재검토) |
| m=application SDP 주입/파싱 | floor 불가 | PJSIP 모듈/콜백으로 주입, 수신 SDP 파싱(고정 +1 금지) |
| PJSIP↔JNI 스레드/수명 | 크래시 | libRegisterThread 규약, 래퍼 강참조 |
| Doze/배터리 | 등록 끊김 | FGS+wakelock+최적화 예외 |
| PJSIP 라이선스 | 배포 | **GPL 공개 트랙** — 앱을 GPL로 배포(소스 공개)하여 충족(§12) |
| AMR/H.264 특허 | 배포 | MediaCodec(OEM) 사용으로 완화, IP 검토 병행 |

---

## 12. 라이선스 / 특허 체크리스트(법무 전달용)

| # | 항목 | 확인 내용 |
|---|---|---|
| 1 | PJSIP 라이선스 | **GPL 공개 트랙 확정** → Teluu 상용 라이선스 불요. 대신 **앱 전체를 GPL로 배포(소스 공개) 의무 준수** — Google Play 배포 시 소스 제공 경로 마련 |
| 2 | AMR-WB/NB 특허 | MediaCodec(OEM 라이선스) 사용 전제. 잔존 특허/관할별 로열티 검토 |
| 3 | H.264 특허 | MediaCodec(OEM) 사용. Via LA 등 검토 |
| 4 | 번들 컴포넌트 | OpenSSL/libsrtp/Speex 등 permissive 확인, **영상 시 ffmpeg(LGPL/GPL) 배제** 권장 |

---

## 13. 확정 사항 / 미해결 항목

확정 사항:

- **코드 위치**: 모노레포 `android/`, 공유 `core`(Android Library, PJSIP·코덱·SIP/미디어) + 클라이언트 2개(`android/volte-client`, `android/ptt-client`). 두 앱이 `project(':core')` 의존.
- **마일스톤 순서**: VoLTE 먼저(M1), 이후 PTT(M2).
- **TLS 시점**: 초기 **UDP 5060**, TLS 5061은 **이후(M4)**.
- **영상 범위**: 음성+영상 **함께**(M1부터).
- **PJSIP 라이선스 트랙**: **GPL 공개**(앱을 GPL로 배포 → Teluu 상용 라이선스 불요, 대신 GPL 소스 공개 의무 준수).

미해결:

- **타깃 단말**: **UNIWA 러기드/PoC 지향 안드로이드 단말**(변경 가능). 정확 모델/Android 버전 확정 시 minSdk·arm64 여부·MediaCodec(AMR-WB/H.264) 가용성·지연 재확인. **특정 모델 하드코딩 금지.** UNIWA는 보통 하드웨어 PTT 키 보유 → M4에서 매핑.
