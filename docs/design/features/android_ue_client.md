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
| SUBSCRIBE/NOTIFY(xcap-diff/conference) | △ evsub 프레임워크 | **이벤트 패키지 등록 + 본문(XML) 파싱** |
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
[ Field ID | Length | value(Length) | (string필드는 4B 정렬패딩) ] ... |
```

메시지 타입(subtype) — TS 24.380 Table 8.2.2-1:

| subtype | 메시지 | 방향 |
|---|---|---|
| 0 | Floor Request | UE→서버 |
| 1 | Floor Granted | 서버→UE |
| 2 | Floor Taken | 서버→ALL |
| 3 | Floor Deny | 서버→UE |
| 4 | Floor Release | UE→서버 |
| 5 | Floor Idle | 서버→ALL |
| 6 | Floor Revoke | 서버→화자 |
| 8 | Floor Queue Position Request | UE→서버 |
| 9 | Floor Queue Position Info | 서버→UE |
| 10 | Floor Ack | both |

주요 Field ID(TS 24.380 §8.2.3): 0 Floor Priority · 1 Duration · 2 Reject Cause · 3 Queue Info ·
4 Granted Party's Identity · 5 Permission · 6 User ID · 7 Queue Size · 8 Msg Seq No · 10 Source ·
11 Track Info · 13 Floor Indicator(비트마스크: emergency 0x1000 / imminent 0x0800 …) · 14 SSRC.
구현: `ptt-client/floor/{FloorControl,FloorCodec,FloorClient}.kt` (가변 문자열 필드만 4B 정렬).

### 5.3 단말 Floor 상태머신

```
        REQUEST            ┌─ GRANT ─► [SPEAKING] ── RELEASE ──► [IDLE]
[IDLE] ──────────────►(대기)┤                  ▲                    │
                            └─ REJECT ─► [IDLE]  │ REVOKE(선점)       │
        TAKEN(타인 화자) ─► [LISTENING] ─ IDLE ─►─┘◄── TAKEN(타 화자)──┘
```

- **PTT 버튼 down** → Floor Request 송신(+REQUESTING). GRANT 수신 시 **승인 톤(이중 삑)+진동 재생을 마친 뒤에** mic 개방("삑 후 말하기" — 톤이 그룹으로 송출되지 않게). 3초 내 GRANT/DENY 무응답이면 IDLE 복귀+거부 톤.
- **늦은 GRANT**(버튼을 이미 뗀 뒤 도착) → 즉시 Release 반납(mic 미개방).
- **Revoke/Deny** 수신 → 즉시 mic disconnect + 거부/회수 톤(승인과 구별되는 저음)+진동.
- **Taken** → 발언자 카드에 화자(Granted Party's Identity)+발언 경과시간 표시, LISTENING 중 버튼 누름은 무시(불필요한 REJECT 방지).
- **UX 구현**: 톤/진동=`ptt-client/audio/PttFeedback.kt`(ToneGenerator STREAM_VOICE_CALL+Vibrator, 서비스가 컨트롤러에 주입), 발언자 추적=`PttController.speaker: StateFlow<Speaker?>`(내 GRANT/타인 TAKEN, elapsedRealtime 기준). PTT 화면(`MainActivity`)=상단 등록상태·그룹 카드 + 중앙 발언자 카드(경과 타이머) + 하단 원형 220dp PTT 버튼(IDLE=파랑/REQUESTING=주황/SPEAKING=초록+펄스 링/LISTENING=회색).
- **하드웨어 PTT/SOS 버튼**(`HwPtt.kt`): 하드웨어 버튼 단말은 화면 PTT 버튼을 숨기고 안내 문구로 대체, 키 down/up=`MainActivity.dispatchKeyEvent`→pttDown/Up. **PTT/SOS 키 분류는 `HwPtt.classify(keyCode, scanCode)`** — W999 실측: GPIO 장치(`droi_gpio_keys`)가 두 측면 버튼(scan 68=KEY_F10, scan 87=KEY_F11)을 Generic.kl 에서 **둘 다 keycode 309("PTT")** 로 매핑하므로 keycode 만으로는 구분 불가, scanCode 로 가른다(PTT 버튼=scan 87, SOS 2번째 버튼=scan 68; scanCode 미보고 단말은 PTT 폴백). 일반 단말 폴백=F11→PTT/F10→SOS. Android 13 이라 `input keyevent 309` 주입은 불가(scanCode=0), 커널 경로로만 전달. 감지 3중화=①과거 PTT 키 수신 이력(영속) ②기종 allowlist(UNIWA W999 — GPIO 장치가 앱 InputDevice 열거에 미노출) ③입력장치 F11/309 능력 스캔. 컨트롤러 접근은 `PttService.controllerFlow`(StateFlow — 바인드 후 늦게 생성되는 컨트롤러도 UI 재구성). ⚠️일부 러기드 단말은 `persist.log.tag=I` 로 Log.d 전역 차단 — 진단 로그는 Log.i.
- **착신 그룹콜 자동 수락**(ptt_ue.md §12.3): 수신 INVITE 원문에 `mcptt-info` 존재 → `CallState.Incoming.mcptt` → `PttController.autoJoinGroupCall`(floor 소켓 개설+`answerGroupCall` 로 응답 SDP 에 m=application 주입). 전제=**자동 affiliation**(등록 완료/그룹 선택 시 PUBLISH, `Event: mcptt` 헤더 필수 — 없으면 CSP 489 Bad Event). CSP 는 affiliation 된 멤버에게만 그룹 INVITE fan-out.
- **참가자 목록**: CSP in-dialog conference NOTIFY(RFC 4575)를 `CimsCall.onCallTsxState` 의 수신 원문에서 파싱(`SipController.conferenceInfo` SharedFlow → `PttController.participants`). ⚠️pjsip 다이얼로그는 evsub 미소유 NOTIFY 에 500 을 응답하지만 invite usage 의 tsx 이벤트로 원문은 전달됨 — 정식 conference 이벤트 구독은 후속 과제.
- **NAT 경로 개방**: ①floor 연결 직후 **Floor Ack(User ID 포함) 1회** 송신 → CMP 가 floor 주소 latch(TAKEN/GRANT 수신 가능) ②PJSIP `PJMEDIA_STREAM_ENABLE_KA=1`(config_site, 재빌드)로 오디오 소켓 keepalive → CMP NAT-KA latch 로 청취 전용 상태에서도 하향 오디오 수신. UAC 발신 응답의 floor 목적지는 `onCallTsxState` 의 200 OK 원문에서 학습(onCallSdpCreated 는 로컬 SDP 생성 시에만 호출됨). floor UDP 송신은 전용 스레드(main 스레드 send 는 NetworkOnMainThreadException).
- **멀티그룹 동시 참여**(TS 22.179 group scanning): 한 단말이 N개 그룹에 동시 참여한다. 그룹별로 독립 SIP 다이얼로그+floor 소켓+FloorClient 를 가지며(`PttController.Session`), UI 는 그룹별 카드로 나열한다. **주채널(primary)/부채널(secondary)** 을 행 버튼으로 지정(`setPrimary`/`toggleSecondary`), **듣기 정책**(주·부만 / 전체듣기)은 우하단 벡터 아이콘 토글로 그룹별 `setCallListen`(하향 오디오 mix on/off)을 적용한다. **PTT 발언은 주채널 세션만** 대상(`pttDown`/`pttUp`). 그룹별 나가기(행의 빨간 CallEnd)·참여는 독립적. 발언자 카드는 주채널 기준으로 파생하되 비주채널 화자는 `[gNNN]` 태그를 붙인다. per-call 문맥 분리 필수: floor 목적지/conference 정보는 callId 로 구분, SDP 주입은 `CimsCall.pendingAppSdp`(전역이면 그룹 간 혼선).
- **서버측 멀티그룹 정합**(CSP `GroupCallService`): 활성 콜 추적 키가 `(userId, groupId)` — 사용자가 여러 그룹에 동시 참여하는 것을 전제한다. 다른 멤버가 같은 그룹을 개시(originate)해 fan-out INVITE 가 와도, 이미 **라이브 SIP 다이얼로그**(UA 다이얼로그 맵으로 판정 — 개시자 AcceptCall 레그·CSP StartCall 레그 모두 포함)를 가진 멤버는 재초대하지 않는다(과거엔 선참여 멤버를 stale 로 오판해 LEAVE+재INVITE → 단말이 재INVITE 미응답 → CMP 멤버십 이탈하는 좀비 상태였음). `ClearUserCall` 은 해당 사용자의 **모든** 그룹 콜을 정리.
- **긴급(SOS)** ([mcptt_emergency_modes.md](mcptt_emergency_modes.md), TS 24.379): 개시=하드웨어 SOS 키(down 1회) 또는 화면 SOS 버튼(우하단, 오발동 방지 **길게 누름**) → `PttController.startEmergency()` — 주채널 통화 중이면 **in-dialog re-INVITE**(mcptt-info `emergency-ind=true`, `SipController.reinviteWithBody`)로 상향, 미참여면 긴급 그룹콜 발신(`joinGroupCall(emergency=true)`). CSP 가 그룹 capability(`ppt_groups.emergency_call`) 미허용이면 normal 로 하향 수용(로그 `emergency not allowed → downgrade`). **해제는 화면 배너에서만**(개시자 전용 — 서버도 비개시자의 취소 re-INVITE 를 무시, `emergency-ind=false` 명시 송신). 긴급 세션의 PTT 발언은 Floor Request 에 **Floor Indicator emergency 비트**(TS 24.380 §8.2.3.13)를 실어 CMP tier 상향·선점. **수신측 표시** 경로 2개: ①미참여 멤버=fan-out INVITE 의 emergency-ind(`CallState.Incoming.emergency`) ②참여 중 멤버=CMP 가 긴급 발언자의 TAKEN 방송에 싣는 emergency 비트(latch — CSP 는 in-call 상향을 re-INVITE fan-out 하지 않음; 취소 역시 전파되지 않아 수신측 latch 는 세션 종료 시 해제). UI=붉은 점멸 배너(그룹/개시자, 개시자에게 해제 버튼)+그룹행 [긴급] 배지·적색 하이라이트+SOS 버튼 활성 점멸, 톤=`PttFeedback.emergencyTone()`(긴 경고음+삼중 진동).
- **레거시 폴백**: 서버는 DTMF(PT=101)도 floor 트리거로 허용([ptt_flows.md](ptt_flows.md) C1). 1차 구현은 RTCP-APP(TS 24.380) 정공법 사용.

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

---

## 8. 안드로이드 런타임 설계

> **프로비저닝:** 서버(IP/포트/전송)·계정(도메인/MSISDN/이름/로그인ID/auth_id/비밀번호)은 **하드코딩하지 않고 CIMS SSO 자동 프로비저닝으로 수신·저장**한다. 설정 화면(`SettingsScreen`, 안드로이드 설정 스타일)은 SSO 구성 상태에서 **읽기 전용**이며, 테스트용 **수동 설정 모드**만 편집 허용 — [android_ue_provisioning.md](android_ue_provisioning.md) §5-1. 구현: `core`의 `SipAccountConfig`+`ConfigStore`. SIP 매핑: AOR=`sip:<msisdn>@<domain>`, Digest username=`imsi@domain`(또는 authId 전체 IMPI), Registrar=`serverHost:serverPort`. 비밀번호는 운영 시 EncryptedSharedPreferences/Keystore.

| 항목 | 설계 |
|---|---|
| 등록 유지 | **Foreground Service**(통화/대기 알림) + 부분 wakelock, 배터리 최적화 예외 요청 |
| Doze/네트워크 | FGS 유지 + 등록 refresh(서버 Expires 추종) + **등록 keepalive 보강**: ①PJSIP 등록 실패 자동 재시도(`regConfig.firstRetryIntervalSec=5`/`retryIntervalSec=30`) ②NAT 바인딩 UDP keep-alive(`natConfig.udpKaIntervalSec=15`, contact/via rewrite) ③기본 네트워크 복귀 시 재등록(`ConnectivityManager.registerDefaultNetworkCallback`→`SipController.reregister`) ④앱 포그라운드 복귀 시 재등록(`MainActivity.onResume`→`SipService.poke`) |
| 스레딩 | PJSIP 콜백=워커 스레드 → UI는 main으로 디스패치. **PJSIP 외 스레드에서 호출 시 `Endpoint.libRegisterThread()` 필수** |
| 객체 수명 | `Account`/`Call`/`AudioMedia` 래퍼 GC 방지(강참조 유지) + 명시적 delete |
| **백그라운드 착신** | 기본 전화앱처럼 앱 미실행/화면 꺼짐에도 착신: `SipService` 가 `CallState.Incoming` 에 **CallStyle 착신 알림**(받기/거절 액션) + `fullScreenIntent`(잠금·꺼진 화면이면 통화화면 직행, MainActivity `showWhenLocked`/`turnScreenOn`) + **벨소리 루프**(`RingtoneManager` TYPE_RINGTONE, `isLooping`). "받기"=MainActivity 경유(`EXTRA_ANSWER_CALL_ID`/`EXTRA_ANSWER_VIDEO`, 서비스 연결 후 응답), "거절"=서비스 액션(`ACTION_REJECT`). 서비스 기동 경로 3종: ①CIMS 로그인 직후(오너앱이 `startForegroundService`, exported+signature 권한 `com.cims.ue.permission.CIMS_SUITE`) ②부팅(BootReceiver) ③앱 실행 |
| **착신 영상 응답** | 수신 INVITE 원문 SDP 의 `m=video` 로 영상호 감지(`CallState.Incoming.video`) → 통화화면에 "영상" 응답 버튼(CAMERA 권한 확보 후 `answer(withVideo)`=`opt.videoCount=1`) |
| **로컬 카메라 프리뷰** | 영상 통화 중 우하단 PiP(내 화면): `VideoPreview`(전면 카메라 우선, `VidDevManager.enumDev2` 이름 "front" 매칭) + `SurfaceView.setZOrderMediaOverlay` — 호 종료/shutdown 시 자동 정리 |
| **문자(SIP MESSAGE)** | RFC 3428 page-mode 송수신: 송신=`SipController.sendRequest("MESSAGE")`, 수신=`Account.onInstantMessage`→`SipController.incomingMessage`(SharedFlow)→`SipService` 가 **인박스 저장 + 알림**. 인박스=core `MessageStore`(상대별 스레드·안읽음 카운트, SharedPreferences+JSON). UI=문자 탭(스레드 목록→말풍선 대화·전송, 안읽음 배지) |
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
