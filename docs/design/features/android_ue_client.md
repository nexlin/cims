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
- **UX 구현**: 톤/진동=`ptt-client/audio/PttFeedback.kt`(ToneGenerator STREAM_VOICE_CALL+Vibrator, 서비스가 컨트롤러에 주입), 발언자 추적=`PttController.speaker: StateFlow<Speaker?>`(내 GRANT/타인 TAKEN, elapsedRealtime 기준).
- **앱 UI 구조**(`ptt-client/ui/`, 시안 `android/assets/pages/` 기준 — 다크 배경+민트 액센트, 토큰=`ui/Theme.kt` `Ct`, 아이콘=`assets/svgs` 변환 VectorDrawable):
  - 라우팅=`ui/AppRoot.kt`(`Nav` sealed: Splash→Home 4탭(주채널/전체채널/메시지/설정)→Channel/Thread 푸시, BackHandler 계층 복귀). 컨트롤러 상태는 `PttUiState` 로 묶어 화면에 전달.
  - **스플래시**(`SplashScreen`)=동심원 로고+기관 로고(`yrt_logo`), CIMS SSO 라 별도 로그인 화면 없음 — 계정 있으면 등록 진행 후 자동 진입, 없으면 "CIMS 로그인 열기".
  - **주채널 탭**(`MainChannelScreen`)=주채널 1개만 전면 배치(카드 박스 없음). 부채널 개념은 없으며 주채널 외 참여 채널은 전체채널 탭에서 확인. 구성(위→아래): 채널명+`P{n}` 배지(TS 24.481 on-network-group-priority, 표시 시 `loadGroupDetail` ETag 캐시 로딩)+긴급 배지+우측 **[주채널 선택] 버튼 → 같은 윈도우 바텀시트**(`ChannelSelectSheet` — 내 그룹 리스트(이름+P배지+주채널/참여 중/미참여)에서 탭 한 번으로 즉시 지정, 화면 이동 없음; 미참여 그룹이면 `joinGroupCall` 로 참여부터 수행 후 `setPrimary`) / 태그칩(음성/구성원 N)+"▶ ○○ 송신" 칩 / 발언 상태 스트립(발언자+경과 타이머 슬림 한 줄) / **영상 패널**(`VideoPanel` — 영상 PTT 대비 소형 영상 자리, 현재 "영상 없음" 플레이스홀더; 우하단 오버레이 원형 아이콘=**오디오 출력**·전체듣기. 오디오 출력은 **스피커폰이 기본**이며 이어폰 미연결 시 탭=스피커폰↔수화기 토글, 이어폰 연결 시(유선/블루투스 — 무선 다중 연결 포함) 탭=`AudioRouteSheet` 바텀시트에서 이어폰(장치별)/스피커폰/수화기 선택. 이어폰 장치 열거·지정=`ptt/audio/AudioRouter`(API 31+ `setCommunicationDevice`/`availableCommunicationDevices`, 이하 BT SCO·유선 자동 라우팅; 스피커폰/수화기는 종전대로 pjsua2 `setOutputRoute`), 선택 영속=`AudioRoutePrefs`(리부팅/재기동 복원), 이어폰 **연결=자동 전환**·해제=남은 이어폰 또는 스피커폰 복귀(`PttService.observeHeadsets`)) / 화면 PTT 바(**터치 단말만** — 하드웨어 PTT 버튼 단말은 표시 없음) / **하단 인라인 채팅**(`InlineChat`: 주채널 그룹 메시지 말풍선 리스트+입력바, `svc.sendGroupMessage` 송신·`markThreadRead`·새 메시지 자동 스크롤, 우상단 아이콘=전체 대화 화면). 입력바=첨부+입력+전송(종이비행기 `ic_send`) — **첨부 버튼**(`AttachButton`, 시스템 포토 피커 사진/동영상)은 메시지 스레드 입력바와 공용이며 미디어 **전송은 서버 업로드 경로 연동 후** 지원(현재 선택 시 안내 토스트). **채널별 수신 음량**=채널 상세의 슬라이더(주채널 화면엔 없음)→`PttController.setChannelVolume`→`SipController.setCallRxLevel`(pjsua2 conference bridge `adjustRxLevel`, 0~2·1=원음; 미디어 재협상 시 리셋되므로 `applyListenPolicy` 에서 재적용). 그룹별 음량은 `ptt/audio/GroupVolumeStore`(SharedPreferences)로 **영속**(리부팅/앱 재기동 유지)하며 **저장값 없는 신규 그룹 기본=최대(2f)**. 하드웨어 음량 키는 통화 스트림 조절(`MainActivity.volumeControlStream=STREAM_VOICE_CALL`). SOS 는 HW 키 전용(화면 SOS 버튼 없음). PTT 바 상태색: 대기=민트/요청=황색/발언=민트+펄스/수신=회색면.
  - **전체채널 탭**(`ChannelsScreen`)=**당겨서 새로고침**(`PullToRefreshBox`, 별도 새로고침 버튼 없음)+채널 행(맨 앞=그룹 이름 첫 자 배지, 이름 옆 주채널 "주"/긴급 소형 사각 배지(`SquareBadge`), 음성/영상·그룹 `P{n}`·구성원 수 태그칩, **발언 중이면 마이크 아이콘+발언자 이름 칩**(그룹 문서 이름 우선, 없으면 번호)/참여 중/가입 배지, 우측 메시지 버튼). 그룹 P·영상 여부는 그룹 문서(TS 24.481)에서 — 목록 진입 시 그룹별 `loadGroupDetail`(ETag 304 캐시라 저비용). 행 탭→**채널 상세**(`ChannelDetailScreen`, 시안 `채널선택화면-상세.png`·`채널상세화면-주채널표시.png`)=헤더(채널명+음성/영상·그룹 `P{n}` 태그칩+CH 번호)+**역할 배너**(고정폭 소형 사각 배지 "주채널"/"일반", **배너 터치=주채널↔일반 토글** — 일반→주채널은 미참여 시 `joinGroupCall` 참여부터, 주채널→일반은 `clearPrimary` 강등(주채널 없는 상태 허용), 안내문도 함께 전환)+채널 상태 카드(구성원 N명·접속 중 M명·발언자, 참여 중이면 수신 음량 슬라이더)+구성원 목록+하단 **참여/나가기 토글 버튼**(미참여=[참여] / 참여 중=[나가기]).
  - **채널 상세 구성원 명부 = TS 24.481 그룹 문서(표준 필드 + CIMS 확장 직함)**: 진입 시 `PttController.loadGroupDetail` 이 GMS XCAP `getGroupDoc`(ETag/If-None-Match 캐시)으로 그룹 문서를 받아 `csc/CscModels.kt GroupDoc.parse` 로 파싱 — 멤버별 `entry uri`(tel: = 전화번호)·`rl:display-name`(이름)·`mcpttgi:participant-type`(chair→"의장" 배지)·`mcpttgi:user-priority`(회색 `P{n}` 배지 — TS 24.481/24.380 규격: **0~255, 클수록 높은 우선순위**, 미지정=최저)·`cims:user-title`(직함 — 이름 옆 보조 표기, 빈 값이면 서버가 요소 생략), 그룹 레벨 `on-network-group-priority`(헤더·전체채널 카드의 `P{n}` 태그칩, 동일 규격 방향)·`mcptt-video`(음성/영상)·`session-type`·`max-participant-count`. 접속/발언 상태는 conference-info(RFC 4575) 참가자·floor(TS 24.380) 화자와 번호 키로 결합해 **접속 중/오프라인** 섹션 분리(각각 우선순위 내림차순 정렬)(미참여 시엔 접속 여부를 알 수 없어 중립 "구성원" 명부). 직함 등 3GPP 미정의 필드는 `<entry>` 의 `##other` lax 확장 지점(TS 24.481/RFC 4826)에 CIMS 전용 네임스페이스(`urn:cims:groupinfo:1.0`)로 싣는다 — 규격 적합 확장이며 표준 단말은 무시한다. 직함 원본은 DB `users.title`. 전화번호 표시는 홈 국가코드 축약(`PttController.fmtNumber`: +82… → 0…, 프로비저닝 countryCode 우선·내 msisdn ITU 유도 폴백 — VoLTE 앱과 동일 방식, 표시 전용).
  - **메시지 탭**(`MessagesScreen`)=스레드 목록(안읽음 배지)→대화(`MessageThreadScreen`)=날짜 구분+말풍선(발신=민트 우측/수신=다크 좌측)+입력바. 발신=`PttController.sendGroupMessage`(SIP MESSAGE, 그룹 URI — CSP fan-out), 수신=`SipController.incomingMessage`→core `MessageStore` 영속(`PttService`), 변경 틱=`PttService.messageTick`. **메시지 삭제**: 스레드 목록에서 행 길게 누름=대화 삭제·헤더 휴지통=전체 삭제(각각 확인 다이얼로그), 대화 화면에서 말풍선 길게 누름=**선택 모드**(탭=토글, 상단바 전환: 선택 수+[전체선택]+휴지통 → 1건/다건 삭제) — 식별=`MessageEntry.key`(peer|time|direction|msgId|text 복합, msgId 는 비보장), 삭제 API=`MessageStore.delete(keys)/clearThread/clearAll`, 서비스 래퍼(`PttService.deleteMessages/deleteThread/deleteAllMessages`)가 첨부 로컬 파일·전송 진행률(`_sendProgress`)도 함께 정리 후 tick 갱신. 미디어 첨부는 후속 과제(서버 경로 미구현).
  - 이력 이벤트 훅=`PttController.onEvent`(`PttEvent`: JOIN/LEAVE/TALK_ME/TALK_OTHER/EMERGENCY*)→`ptt-client/history/HistoryStore.kt`(SharedPreferences JSON, 최대 500건) — 수집만 하며 전용 화면은 없다(통화이력 UI 는 VoLTE 전화앱 영역).
  - **설정 탭**(`SettingsScreen`)=프로필(등록 상태·등록/해제)+통신 설정(스피커 출력 토글=스피커폰↔수화기(이어폰 선택은 주채널 화면)/전체 듣기 토글)+하드웨어 버튼 설정(`KeyConfigOverlay` 열기)+그룹 새로고침·버전.
- **하드웨어 PTT/SOS 버튼**(`HwPtt.kt`): 하드웨어 버튼 단말은 화면 PTT 버튼을 숨기고 안내 문구로 대체, 키 down/up=`MainActivity.dispatchKeyEvent`→pttDown/Up, SOS down=`startEmergency`. **PTT/SOS 키 분류는 `HwPtt.classify(keyCode)`** — 두 측면 버튼은 **별개 keycode**(W999 실측 keyDown 로그: PTT(1번째)=**309**[scan 87, dev3], SOS(2번째)=**310**[scan 231, dev2·gamepad-class])라 keycode 만으로 구분한다. 일반 단말 폴백=F11→PTT/F10→SOS.
  - **버튼 학습(설정)**: 기종마다 측면 키 keycode 가 달라 하드코딩만으로는 신규 단말 커버 불가 → 설정 화면의 **하드웨어 버튼 설정** 오버레이에서 사용자가 "설정" 후 실제 버튼을 눌러 keycode 를 학습·영속(`SharedPreferences` `hw_ptt`, `startLearn`/`consumeLearn`). 학습값이 있으면 `classify` 가 우선 적용, 없으면 내장 기본(309/310+F11/F10). 시스템 네비 키(뒤로/홈/볼륨/전원, `isSystemNav`)는 학습 제외. 🔑설정 UI 는 **별도 Dialog 윈도우가 아니라 같은 Activity 윈도우 안의 오버레이**(`KeyConfigOverlay`)로 그린다 — gamepad-class 측면 키는 별도 Dialog 가 포커스를 잡으면 focus 네비게이션에 소비돼 앱으로 오지 않으므로, Activity 가 키 포커스를 유지해 물리 키가 `dispatchKeyEvent`→`consumeLearn` 으로 유입되게 한다. 실기기 학습 검증: `learned SOS = keycode 310`/`learned PTT = keycode 309`.
  - **존재 감지 3중화**(화면 PTT 버튼 숨김 판단)=①과거 PTT 키 수신 이력·학습값(영속) ②기종 allowlist(UNIWA W999 — GPIO 장치가 앱 InputDevice 열거에 미노출) ③입력장치 PTT/F11 능력 스캔. 컨트롤러 접근은 `PttService.controllerFlow`(StateFlow — 바인드 후 늦게 생성되는 컨트롤러도 UI 재구성). ⚠️일부 러기드 단말은 `persist.log.tag=I` 로 Log.d 전역 차단 — 진단 로그는 Log.i.
- **착신 그룹콜 자동 수락**(ptt_ue.md §12.3): 수신 INVITE 원문에 `mcptt-info` 존재 → `CallState.Incoming.mcptt` → `PttController.autoJoinGroupCall`(floor 소켓 개설+`answerGroupCall` 로 응답 SDP 에 m=application 주입). 전제=**자동 affiliation**(등록 완료/그룹 선택 시 PUBLISH, `Event: mcptt` 헤더 필수 — 없으면 CSP 489 Bad Event). CSP 는 affiliation 된 멤버에게만 그룹 INVITE fan-out. MCPTT 토큰(TS 33.180) 주입은 **서비스 레벨**(`PttService.injectSsoToken` — CIMS 로그인/부팅 autostart 만으로, PTT 앱을 열지 않아도 그룹 조회→선택 그룹 affiliation PUBLISH 까지 진행)과 UI(AppRoot, 앱 오픈 시) 이중 경로 — 토큰 보유 시 서비스 주입은 생략(멱등).
- **참가자 목록**: CSP in-dialog conference NOTIFY(RFC 4575)를 `CimsCall.onCallTsxState` 의 수신 원문에서 파싱(`SipController.conferenceInfo` SharedFlow → `PttController.participants`). ⚠️pjsip 다이얼로그는 evsub 미소유 NOTIFY 에 500 을 응답하지만 invite usage 의 tsx 이벤트로 원문은 전달됨 — 정식 conference 이벤트 구독은 후속 과제.
- **NAT 경로 개방**: ①floor 연결 직후 **Floor Ack(User ID 포함) 1회** 송신 → CMP 가 floor 주소 latch(TAKEN/GRANT 수신 가능) ②PJSIP `PJMEDIA_STREAM_ENABLE_KA=1`(config_site, 재빌드)로 오디오 소켓 keepalive → CMP NAT-KA latch 로 청취 전용 상태에서도 하향 오디오 수신. UAC 발신 응답의 floor 목적지는 `onCallTsxState` 의 200 OK 원문에서 학습(onCallSdpCreated 는 로컬 SDP 생성 시에만 호출됨). floor UDP 송신은 전용 스레드(main 스레드 send 는 NetworkOnMainThreadException).
- **멀티그룹 동시 참여**(TS 22.179 group scanning): 한 단말이 N개 그룹에 동시 참여한다. 그룹별로 독립 SIP 다이얼로그+floor 소켓+FloorClient 를 가지며(`PttController.Session`), UI 는 주채널 탭(주채널 전면 패널)과 전체채널 목록으로 나열한다. **주채널(primary)** 지정은 주채널 탭의 선택 시트·채널 상세의 버튼(`setPrimary` — 나머지 참여 그룹은 일반 참여, 별도 역할 없음), **듣기 정책**(주채널만 / 전체듣기)은 주채널 탭 컨트롤 행·설정 화면 토글로 그룹별 `setCallListen`(하향 오디오 mix on/off)을 적용한다. **PTT 발언은 주채널 세션만** 대상(`pttDown`/`pttUp`). 그룹별 나가기(채널 상세)·참여는 독립적. 발언 상태 패널은 주채널 기준으로 파생하되 비주채널 화자는 `[gNNN]` 태그를 붙인다. per-call 문맥 분리 필수: floor 목적지/conference 정보는 callId 로 구분, SDP 주입은 `CimsCall.pendingAppSdp`(전역이면 그룹 간 혼선).
- **서버측 멀티그룹 정합**(CSP `GroupCallService`): 활성 콜 추적 키가 `(userId, groupId)` — 사용자가 여러 그룹에 동시 참여하는 것을 전제한다. 다른 멤버가 같은 그룹을 개시(originate)해 fan-out INVITE 가 와도, 이미 **라이브 SIP 다이얼로그**(UA 다이얼로그 맵으로 판정 — 개시자 AcceptCall 레그·CSP StartCall 레그 모두 포함)를 가진 멤버는 재초대하지 않는다(과거엔 선참여 멤버를 stale 로 오판해 LEAVE+재INVITE → 단말이 재INVITE 미응답 → CMP 멤버십 이탈하는 좀비 상태였음). `ClearUserCall` 은 해당 사용자의 **모든** 그룹 콜을 정리.
- **긴급(SOS)** ([mcptt_emergency_modes.md](mcptt_emergency_modes.md), TS 24.379): 개시=하드웨어 SOS 키(down 1회) 또는 화면 SOS 버튼(주채널 탭 컨트롤 행, 오발동 방지 **길게 누름**) → `PttController.startEmergency()` — 주채널 통화 중이면 **in-dialog re-INVITE**(mcptt-info `emergency-ind=true`, `SipController.reinviteWithBody`)로 상향, 미참여면 긴급 그룹콜 발신(`joinGroupCall(emergency=true)`). CSP 가 그룹 capability(`ppt_groups.emergency_call`) 미허용이면 normal 로 하향 수용(로그 `emergency not allowed → downgrade`). **해제는 화면 배너에서만**(개시자 전용 — 서버도 비개시자의 취소 re-INVITE 를 무시, `emergency-ind=false` 명시 송신). 긴급 세션의 PTT 발언은 Floor Request 에 **Floor Indicator emergency 비트**(TS 24.380 §8.2.3.13)를 실어 CMP tier 상향·선점. **수신측 표시** 경로 2개: ①미참여 멤버=fan-out INVITE 의 emergency-ind(`CallState.Incoming.emergency`) ②참여 중 멤버=CMP 가 긴급 발언자의 TAKEN 방송에 싣는 emergency 비트(latch — CSP 는 in-call 상향을 re-INVITE fan-out 하지 않음; 취소 역시 전파되지 않아 수신측 latch 는 세션 종료 시 해제). UI=붉은 점멸 배너(그룹/개시자, 개시자에게 해제 버튼)+그룹행 [긴급] 배지·적색 하이라이트+SOS 버튼 활성 점멸, 톤=`PttFeedback.emergencyTone()`(긴 경고음+삼중 진동).
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

- **앱 UI 공통 디자인(3앱 통일)**: CIMS(로그인)·CIMS-Phone(volte-client)·McPTT(ptt-client) 모두 시안(`android/assets/pages/`) 기준 **다크 고정 + 민트 액센트** — 같은 디자인 토큰(`Ct`/`Cl`: Bg #0D1211·Surface #151C1A·Mint #5EE0C0 등, ptt `ui/Theme.kt`·volte `Theme.kt`·cims `LoginActivity` 내장)과 컴팩트 칩 스타일(`chipStyle` — includeFontPadding 제거), 다크 윈도우 배경(`themes.xml`, 콜드 스타트 포함), `yrt_logo`/`ic_launcher` 브랜딩을 공유한다. 통화 기능색(받기=초록/종료·거절=빨강/영상=파랑)은 전화 관습색 유지.
- **CIMS 로그인 앱 UI**(`cims/LoginActivity`, 시안 `로그인화면.png`): 중앙 로고+"CIMS 통합 로그인"+아이디/비밀번호 다크 라운드 필드(포커스=민트 외곽선)+큰 민트 [로그인] 버튼. 실패 시 붉은 외곽선 오류 박스, 재로그인 시 "이미 로그인됨" 칩. **서버 주소/포트는 '서버 설정' 접힘 행**(기본 접힘, 탭하여 펼침).
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
