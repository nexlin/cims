# 단말 SDK 이식 — .45 진행분과 다른 환경에 넘길 것

`volte-client`·`ptt-client` 를 SDK(`:cimsue`) 위로 옮기는 이행([../design/features/ue_sdk.md](../design/features/ue_sdk.md) §5.3 — VoLTE 먼저, 코어 `domain/` 없음,
정책은 앱 세션 층)의 .45 진행분이다. 작업 분담은 기존 관례와 같다 — **단말 SDK·Android 단말 앱 = .45**, **관제 앱(Windows 데스크톱·Android 태블릿)·
C API·.NET = Windows 개발 환경**.

## 1. P0a 코어·파사드 보강 (.45 반영)

| 항목 | 반영 | 확인 |
|---|---|---|
| 파사드 기본값 | Kotlin `EngineConfig.noVad` 기본 `true` — 코어·.NET 과 같게(침묵 중에도 RTP 연속 송신 — NAT flow 유지) | `FacadeMappingTest` |
| affiliation 412 | RFC 3903 §5 — 412 를 낸 ETag 폐기(MUST)·같은 요청 재전송 없음(MUST NOT)·`SIP-If-Match` 없는 초기 PUBLISH 한 번(SHOULD). 앱에는 412 가 올라가지 않고 재발행의 최종 응답이 `affiliate()` token 으로 온다 | `AffiliationPublish`(루프백 ESC, 수정 전 코드로 실패 재현) · .45 실서버 `cimsue-cli --affiliate g005`(계측기 전용 신원 013) — 조건 없는 PUBLISH 200 + `SIP-ETag` → de-affiliate `SIP-If-Match` 200 |
| 파사드 | `FloorEvent.kind`(`FloorEventKind`) · FD(`CscClient.uploadFd/downloadFd`, `Account.sendGroupFd/sendFd`) · `CimsUe.userAgentOf/imeiUrn` · `ServiceProfile.udpNoTcpSwitch`(코어 프로파일 해석 포함) | `FacadeMappingTest`·`Csc.ParseProfile` |
| 캡처 게이트 | `Engine::setCaptureEnabled`(pjsua `SPEAKER_ONLY`+`NO_IMMEDIATE_OPEN`, 장치 선택을 넘어 유지) · 파사드 `CimsUe.setCaptureEnabled` | `EngineCapture` · MF52 점검 앱(모드 3↔2) |
| 장치 단 음량·라우트·재오픈 | `setDeviceAudioLevels(speaker, micTargetDbov)`(코어가 기억해 게이트 전환·재오픈·호 결선 뒤 재적용) · `setAudioRoute(output, input)` · `reopenAudioDevice()`(열려 있으면 실제로 닫고 다시 연다) | MF52 점검 앱(호출) — 호 중 음량·라우트는 P2/P3 실기 |
| 영상 | `setVideoWindow`(Android Surface → `ANativeWindow` 참조를 코어가 소유, 결선마다 렌더러 몫) · `switchCamera` · `videoDevices` · H.264 최우선 480x640·15 fps·400/500 k · 계정 기본 카메라 = 전면 · `videoAutoTransmit` 면 START_TRANSMIT · 파사드 `setVideoSurface`·`setPreviewSurface`(PjCamera2)·`start(cfg, context)`(CameraManager) | MF52 점검 앱 — 카메라 2대 열거·H.264 정책. 수신 렌더·송신은 P2 실기 |
| 카메라 클래스 탐색 | 코어 제어 스레드는 네이티브 스레드라 `FindClass` 가 APK 의 `org.pjsip.PjCamera2` 를 못 찾는다 → pjlib CIMS 패치 `pj_jni_find_class`(`JNI_OnLoad` 에서 앱 클래스 로더를 기억, FindClass 실패 때 사용) · `android_dev.c` 가 사용. 기존 `libpjsua2` 경로는 FindClass 가 먼저 성공해 동작 불변 | MF52 — `android_dev.c: Android video capture initialized with 2 device(s)` |
| 카메라 도우미 배치 | `build-native.sh` [7] 이 `PjCamera2.java`·`PjCameraInfo2.java` 를 `:cimsue` swig 소스셋에 복사(커밋 안 함) · `S1-UE-ENGINE-SINGLE` = `org.pjsip.pjsua2` 제공처 하나 + 카메라 도우미는 pj 를 싣는 두 모듈 | S1 PASS·음성 대조(엉뚱한 파일 FAIL) |
| 망 변경 재등록 | Android 접점 `platform.NetworkWatcher`(기본 네트워크가 바뀌면 앱 동작 — 보통 `refreshRegistration`, 등록 직후 지금 망 통지는 넘김) | `PlatformTest`(판정 `NetworkChangeFilter`) |
| 기기 점검 앱 | `android/sdk-probe` — 로그인·계정 없이 엔진만(사내 단말에 깔아도 착신을 가로채지 않는다) | MF52 전 항목 PASS |

`cimsue_test` 75(한 프로세스) · `S1-UE-*` 6 + `S1-PY-SYNTAX` PASS · `:cimsue` 31·`:ptt-client` 53 단위시험 · APK 4종(태블릿·PTT·VoLTE·점검) 빌드.
관제 태블릿 실기 회귀는 하지 않았다(관제 계정 로그인이 Windows 쪽 시험 계정 등록과 겹친다).

### 1.1 기존 앱 음질 조작 → SDK 대응 ([ue_audio_level.md](../design/features/ue_audio_level.md))

| 기존 앱(`android/core-sip` `SipController`) | SDK | 비고 |
|---|---|---|
| 엔진 slot 0 AGC(-26 dBov)·리미터·Speex AGC 끔 | 같음 | 같은 엔진 트리(`ext/pjproject` conference.c·`config_site/common.h`) — 코드 이전 없음 |
| `setDeviceAudioBoost(spk, mic)` — slot 0 `adjustRxLevel(spk)`·`setCaptureAgc(true, -26 + 20·log10(mic))`·마이크 배율 1 | `CimsUe.setDeviceAudioLevels(spk, 목표)` | 슬라이더 → 목표 환산은 앱. 재오픈·게이트 전환 뒤 재적용은 코어가 한다 |
| `setCaptureEnabled` | `CimsUe.setCaptureEnabled` | 같은 pjsua 모드 |
| `setAudioRoute` — 출력 + 입력 EARPIECE 고정(스피커·수화기) | `CimsUe.setAudioRoute(output, input)` | 고정 규칙(무전만, 전이중은 DEFAULT)은 앱 |
| `bounceSndDev` | `CimsUe.reopenAudioDevice` | §3 확인 필요 |
| `setCallRxLevel` — 통화 `adjustTxLevel` | `Call.setRxLevel` | 방향 같다(§2) |
| `EpConfig.medConfig.noVad = true` | `EngineConfig.noVad`(기본 true) | |
| `ProximityScreenLock`·`AudioRouter`(AudioManager·`ensureRxVolume`)·`gain_wiring` 저장값 판 | 앱·`:core`(비 SIP)에 남는다 | P1 에서 `:core` 에 그대로 |

## 1.2 P1 `:core` 분리 (.45 반영)

- `:core` = SIP·엔진 없는 공용 조각, `:core-sip` = 기존 앱의 pjsua2 래퍼 6개(`SipController`·`CimsCall`·`CimsAccount`·`CimsEndpoint`·`PjLib`·`CodecConfig`,
  패키지명 그대로 — 앱 손코드 무변경) + `api(:core)`·`api(:cimsue-engine)`. `volte-client`·`ptt-client` → `:core-sip`, 로그인 앱 `cims` → `:core`.
  `CimsTrustStore` 는 `:core` 에 남고 모듈 경계를 넘으므로 `public`.
- APK: `cims` 에 `libpjsua2.so` 없음, `volte-client`·`ptt-client` 는 `libpjsua2.so`, `dispatch-tablet`·`sdk-probe` 는 `libcimsue.so`.
- **MF52 회귀(사내 단말, 분리 뒤 3종 재설치)** — VoLTE·PTT 기동·등록 200·PTT affiliation PUBLISH(g001~g003)·카메라 초기화(2대, 엔진 패치 뒤 기존 경로
  그대로)·크래시/클래스 오류 없음. 이 설치로 09-29 커밋분 단말 변경(일제 통화 U1~U5·단말 속성 V3 — `User-Agent: CIMS-VoLTE/0.1.0-M0 (Android 15; MF52)`·
  `+sip.instance` UUID URN·짧은 PTT 탭 floor 수정)도 MF52 에 들어갔다 — 통화·발언 실기는 아직.

## 1.3 P2 `volte-client` 전환 (.45 반영)

- `VoltePhone`(volte-client) — 기존 `SipController` 계약(등록·호 상태 StateFlow·발신/응답/거절/종료/음소거·영상 Surface/셀프뷰/카메라 전환·캡처 게이트·
  MESSAGE 발신과 token 상관·수신)을 SDK 로 낸다. 호 상태는 기존과 같은 "마지막 호 이벤트" 투영(코어 매핑도 UAC 만 Outgoing·CONNECTING/CONFIRMED=Active 로
  같다). `SipService`·`MainActivity` 는 계약 그대로(망 변경은 `NetworkWatcher`). 모듈 = `:cimsue` + `:core`(`:core-sip` 없음) — APK 에 `libcimsue.so` 하나.
- **MF52 실기(사내 단말, 계측기 전용 신원 013 을 상대로 — `cimsue-cli`)**:

  | 시험 | 결과 |
  |---|---|
  | 등록 | UDP 200 · `User-Agent: CIMS-VoLTE/0.1.0-M0 (Android 15; MF52)` · `+sip.instance` UUID URN · Contact 재작성 · 카메라 2대 · 코덱 AMR-WB 최우선 · H.264 정책 |
  | 착신(013 → MF52, 화면 [받기]) | 200 · RTP 양방향 손실 0 · 013 측 E-model MOS-CQ 4.25(AMR-WB)·RTD 21 ms·지터 최대 4.6 ms · 통화 오디오 모드 확보·수화기 라우팅 · BYE 뒤 `MODE_NORMAL` 복귀 |
  | 발신(MF52 키패드 `01300000013` → E.164 `+821300000013`) | 180/200 · 손실 0 · MOS-CQ 4.24·RTD 15 ms · BYE 정상 |
  | 발신 취소 | CANCEL → 200 → 487 · 모드 복귀 |
  | 문자 수신(013 → MF52 MESSAGE) | 200 · 스레드·미읽음 배지 |
  | 문자 발신(MF52 → 013) | MESSAGE 200(CSP 캡처 = 013 Contact 로 중계·200) · 말풍선 ✓(token 상관) |
  | TLS(설정에서 TLS · 15061 로 바꿔 시험 뒤 UDP 로 원복) | 동봉 CA 로 서버 인증서 검증 · sec-agree(`Security-Client: tls` → 401 `Security-Server` → `Security-Verify`) → 200 · TLS 착신 200·손실 0·MOS-CQ 4.25·BYE(TLS) |

  MF52 송신은 `ptime=40`(프레임 2개/패킷 — Android MediaCodec AMR-WB 기본 `frm_per_pkt = 2`, 엔진 공통이라 기존 앱과 같다). 크래시 0.
- **아직(실기 미확인)**: 영상 통화(상대 영상 단말 필요 — 렌더·송신·셀프뷰·카메라 전환), SRTP(이 계정은 미디어 SRTP 정책 off), 통화 중 PTT 마이크 양보(PTT 발언), 망 전환 재등록
  (Wi-Fi 를 끄면 무선 디버깅이 끊긴다), 착신 알림 [받기](화면 꺼짐·잠금). 코덱 정책 차이 = SDK 는 PCMU 도 둔다(기존은 PCMA 만) — AMR-WB 가 먼저라 협상 결과 같음.

## 2. Windows 개발 환경에 넘길 것

- **C API·.NET 미노출 코어 API** — `setCaptureEnabled`/`captureEnabled` · `setDeviceAudioLevels` · `setAudioRoute`(`AudioRoute`) · `reopenAudioDevice` ·
  `setVideoWindow`/`switchCamera`/`videoDevices`(`VideoDeviceInfo`) · `ServiceProfile.udpNoTcpSwitch` · `kMicAgcTargetDbov`. 구조체 id 는 `CIMSUE_STRUCT_COUNT_`
  앞 끝에 덧붙인다(`AbiLayoutTests`). `setVideoWindow(void*)` 는 참조 수를 세지 않는 창(HWND)도 받는다 — 참조 처리는 Android 만.
- **코어 동작 변화(Windows 앱에도 적용)** — affiliation 412 는 코어가 초기 PUBLISH 로 한 번 다시 알린다. 앱이 412 로 재시도하던 코드가 있으면 필요 없다.
- **관제 태블릿(Android)** — ① 다음 빌드부터 VAD 꺼짐(파사드 기본값이 코어와 같아짐 — 침묵 중에도 RTP) ② 망 변경 재등록이 없다 → `NetworkWatcher` 로
  계정마다 `refreshRegistration` ③ `FloorEvent.kind` 로 Denied/Revoked·코어 시한 구분(지금 `rawType` 판정 대체 가능) ④ FD 파사드가 생겼다.

## 3. 확인 필요 — 기존 PTT 앱 `bounceSndDev`

코드 읽기 결과(실기 미확인): `pjsua_set_snd_dev2` 는 장치·모드가 같으면 장치가 열려 있거나 `NO_IMMEDIATE_OPEN` 일 때 "No changes" 로 돌아간다.
기존 `bounceSndDev` 는 매번 같은 모드(`NO_IMMEDIATE_OPEN` 포함)를 넘기므로 장치를 다시 열지 않고 장치 단 음량만 다시 걸었을 가능성이 있다.
BT·이어폰 소멸 뒤 뮤트 고착 복구([android_ue_client.md](../design/features/android_ue_client.md))가 실제로 무엇으로 풀렸는지는 P3 실기에서
(PTT 수신 중 BT·유선 이어폰 제거) SDK `reopenAudioDevice`(실제 재오픈)와 함께 확인한다.

## 4. P0b 코어 보강 — PTT 몫 (.45 반영)

규약은 [ue_sdk.md](../design/features/ue_sdk.md) §4.2(«정책 게이트»·«긴급·임박 세션 조건»·«media plane SDS»·«승인 톤 뒤 마이크»)가 정본이다.

| 항목 | 반영 | 확인 |
|---|---|---|
| CMS 해석 | `UserProfileDoc`·`ServiceConfigDoc`(TS 24.484 — 긴급 대상 EntryType·제휴 그룹·N2·ruleset allow-*, 요소 없음 = 허용) · `Capabilities::of`(AND 규칙 한 곳) · `CscClient::fetchUserProfile/fetchServiceConfig`(304 = `notModified`) · Kotlin `CscClient.fetch*`·`Capabilities.of` | `CmsDoc.*` 3건 · `Csc.FetchCmsDocsParseAndNotModified` |
| 긴급 상향·하향 | `Engine::setCallCondition` — re-INVITE(바뀐 지시자만 명시 + Resource-Priority) · `CallInfo.condition` · `onMcpttCondition(Local→Confirmed/Denied, Advertised)` · Floor Request 긴급 비트 = 현재값 · Kotlin `Call.setCondition`·`condition` flow | `McpttCondition.UpgradeDeniedConfirmedAndAdvertised`(루프백 서버 — 403 복원·200 확정·서버 하향 재광고) · .45 실서버 `cimsue-cli group-call g005 --upgrade-at 4` → **403 → Denied, 호 유지·이어 발언권 획득**(g005 그룹 능력 `emergency_call` off — CSP `denied (group capability)`) |
| 긴급 경보 | `sendEmergencyAlert`(mcptt-client-id·ICSI 헤더·제3자 취소·그룹 긴급 해제 동봉) · `onEmergencyAlert` · `AccountConfig.mcpttClientId` · Kotlin `Account.sendEmergencyAlert`·`emergencyAlert` flow(유실 없음) | `McpttXml.AlertBuildAndParse` · .45 `cimsue-cli alert g005` 200 → CSP `alert_sent fanout=2` → 014 `onEmergencyAlert` 수신 · 취소 200(`alert_cancelled`) |
| 승인 톤 뒤 마이크 | `EngineConfig.grantMicDelayMs`(floor participant 가 지연 개방, 발언을 잃으면 열지 않음) · 전이중 사설콜은 `setMuted` = PTT 로컬 게이트 | `FloorParticipant.MicOpensAfterGrantDelayAndNotAfterEarlyRelease` |
| 게인 | 호 수신 음량 `CallInfo.rxLevel` 기억·재결선마다 적용(오디오 없어도 성공). AGC 목표 환산은 앱(ue_audio_level.md §6 결정 그대로) | 전체 회귀 |
| MSRP | `mcdata/msrp`(프레이밍·청크·발신·수신) · `sendGroupSds` 상한 초과 → MSRP(`onRequestResult` method `MSRP`) · 서버발 배포 수신 → `onSds(mediaPlane)` · `AccountConfig.maxSdsCplaneBytes`(`toAccount` 가 채움)·`mcdataMsrp`(REGISTER Contact ICSI 합치기) · MSRP 호는 앱 호 목록 밖 · Kotlin 필드 | `Msrp.*` 3건(40 KB 3청크 발신·2청크 배포 수신) · .45 실서버 013 `sds g005 <2408 B> --cplane-max 1500` → **media 200**, CSP `SDS via MSRP fanout=2`(014·MF52, 둘 다 `closed ok=1`) → 014 `onSds media=1` 2408 B |
| cimsue-cli | `group-call --upgrade-at/--cancel-at`(outcome `conditions`) · `alert` · `--cplane-max`·`--msrp` · `sds` outcome `plane` | 위 실측 |

- 검증: `cimsue_test` 85/85 한 프로세스, S1-UE 6항목 PASS(`UNIT`·`FLOOR-CODEC`·`ANDROID-BIND`·`ENGINE-SINGLE`·`SDS-XCHECK`·`CSC-XCHECK`),
  `build-native.sh` + gradle(`volte-client`·`sdk-probe`·`dispatch-tablet` APK, `:cimsue` 단위 31건) 성공.
- **실측 함정** — MSRP 발신 뒤 엔진을 곧바로 내리면 발신 leg BYE 가 cmdp 의 수신 통지보다 먼저 CSP 에 닿아 배포가 버려진다
  (`MSRP_MSG_RECEIVED for unknown session — dup/late, ignore`). 코어는 서버 BYE 를 5 s 기다리고, cli `sds` 는 media 면 6 s 기다린다.
  서버 쪽 보완은 [server45_handoff.md](server45_handoff.md) §9.
- **긴급 확정 경로의 실서버 실측은 남았다** — 계측기 그룹 g005 의 그룹 능력 `emergency_call` 이 꺼져 있어 상향이 403 으로 끝난다.
  Confirmed·서버 재광고(Advertised)는 루프백 시험으로만 확인했다.

### 4.1 Windows 개발 환경에 넘길 것 (P0b)

- **C API·.NET 미노출** — `setCallCondition` · `sendEmergencyAlert` · `onMcpttCondition`/`onEmergencyAlert`(+ `McpttCondition`·`ConditionCause`·
  `EmergencyAlert`) · `CallInfo.condition`·`rxLevel` · `SdsMessage.mediaPlane` · `AccountConfig.mcpttClientId`·`rp*`·`maxSdsCplaneBytes`·`mcdataMsrp` ·
  `EngineConfig.grantMicDelayMs` · CMS `UserProfileDoc`·`ServiceConfigDoc`·`Capabilities`·`fetchUserProfile/fetchServiceConfig`. 구조체 필드는 끝에
  덧붙이고 `AbiLayoutTests` 로 크기 대조.
- **코어 동작 변화(Windows 앱에도 적용)** — ① `sendGroupSds` 가 `AccountConfig.maxSdsCplaneBytes` 를 넘으면 MSRP 로 가고 최종 결과가
  `onRequestResult` method `MSRP` 로 온다(token 상관은 그대로 — method 로 MESSAGE 를 거르는 앱은 고쳐야 한다) ② `setRxLevel` 이 오디오가 없어도
  성공하고 값을 기억한다(앱의 재적용 루프는 필요 없다) ③ 전이중 사설콜에서 `setMuted` 가 적용된다(예전에는 무시).

## 5. 다음 (.45)

W999 설치·영상 실측(§1.3 미실측 잔여) → P3 `ptt-client`(실기 그룹 = g005 — W999 +82500000001·MF52 +82500000002 추가됨, 상대 = 계측기 013~022).
