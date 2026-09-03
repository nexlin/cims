# 단말 SDK (libcimsue) — C++ 코어 + Android/Windows 플랫폼 SDK + 관제조작반 앱

> CIMS 에 붙는 단말(UE) 소프트웨어의 공통 토대. CIMS 연동(SIP·RTP/SRTP·floor·MCData·CSC HTTPS)은
> **플랫폼 중립 C++ 코어 `libcimsue`** 한 구현으로 두고, 그 위에 **Android SDK**(Kotlin, AAR)와
> **Windows SDK**(C++ DLL + 헤더)를 구분 제공한다. UI(앱)는 SDK 위에 플랫폼별로 만든다.
> 첫 소비자는 **관제조작반 앱**(Windows PC + Android 태블릿)이며, 기존 `android/` 의 VoLTE·PTT 앱도
> 같은 SDK 로 수렴한다.
>
> **구현 순서**: Android 먼저 — 코어와 Android SDK 를 만들어 기존 PTT/VoLTE 앱과 관제 태블릿 앱을
> 올리고, Windows SDK 는 같은 코어·같은 엔진 트리에서 이어서 만든다(§10).
>
> 관련: [dispatch_center.md](dispatch_center.md) §8.4(관제 단말 요건·서버 계약),
> [volte_supplementary_services.md](volte_supplementary_services.md)(픽업·전달),
> [android_ue_client.md](android_ue_client.md)(단말 4평면·프로토콜 매핑 — 본 문서가 모듈 구조를 대체),
> [android_ue_m1_pjsip_integration.md](android_ue_m1_pjsip_integration.md)(pjsua2 콜백·스레딩·코덱 정합 규칙 — 코어에 그대로 승계),
> [mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md)(U10 SSRC 디먹스·floor 코덱 단일 정의),
> [android_ue_provisioning.md](android_ue_provisioning.md)(`/provisioning/me`),
> [media_security.md](media_security.md)(SRTP), [sip_access_security.md](sip_access_security.md)(TLS·AKA·sec-agree),
> [../../user-manual/ue_interface.md](../../user-manual/ue_interface.md)(UE 연동 규격).

---

## 1. 범위와 결론

| 결정 | 내용 | 근거 |
|---|---|---|
| 라이브러리/UI 분리 | CIMS 연동 전부를 SDK 에, 앱은 화면·장치·수명주기만 | 두 플랫폼·세 앱(관제·PTT·VoLTE)이 프로토콜 구현을 공유 |
| 코어 언어 | **C++17**, pjsua2 C++ API 위 | 규격 정합이 걸린 로직(TS 24.380 floor participant·RFC 3911 Join·SRTP·U10 SSRC 디먹스)이 한 구현으로 두 플랫폼에 동일 보장. Linux 에서도 빌드되어 개발 서버 CI·헤드리스 UE 로 S3 검증 가능 |
| 엔진 | `ext/pjproject`(CIMS 패치 적용 2.16) **단일 소스 정본**, Linux/Android/Windows 세 툴체인이 같은 트리를 빌드 | §3 |
| 플랫폼 SDK | Android = SWIG Java 바인딩 + Kotlin 파사드 + Android 접점(AAR). Windows = C++ 헤더 + DLL(C++ UI 전제, C# 은 선택 C API) | §5·§6 |
| UI | 플랫폼 네이티브(Android Compose, Windows C++ UI). SDK 는 UI 프레임워크를 모른다 | §7 |

**경계 규칙 세 개** — 이것만 지키면 두 플랫폼이 다시 갈라지지 않는다.

1. **플랫폼 SDK 와 앱은 pjsua2 를 직접 보지 않는다.** 코어 공개 헤더(`cimsue/*.h`)만 본다.
2. **코어는 "UE 세션 의미"만 노출한다.** `register`·`call`·`join`·`listen`·`floorRequest`·`sendSds` 같은
   명령과 그 상태·이벤트. SIP 메시지·RTP 패킷 개념은 API 에 나오지 않는다.
3. **플랫폼 SDK 는 장치·수명주기·저장·스레드 마샬링만 담당한다.** 오디오 라우팅, 카메라, 백그라운드
   생존, 보안 저장소, UI 스레드 전달.

---

## 2. 층 구조

```
┌────────────────────────────────────────────────────────────────────────┐
│ ④ 앱 (UI)          android/dispatch-tablet · ptt-client · volte-client   │
│                    windows/dispatch-desktop                              │
├────────────────────────────────────────────────────────────────────────┤
│ ③ 플랫폼 SDK        sdk/android  (Kotlin 파사드 + SWIG Java + Android 접점) │
│                    sdk/windows  (C++ 헤더 + DLL, 선택 C API)              │
├────────────────────────────────────────────────────────────────────────┤
│ ② 코어 libcimsue    sdk/core  — C++17, UI 無 · 플랫폼 클래스 無            │
│    sip · media · floor · mcdata · csc · domain · api · cli               │
├────────────────────────────────────────────────────────────────────────┤
│ ① 엔진 pjproject    ext/pjproject (CIMS 패치 적용 2.16) + sdk/engine/     │
│    config_site 플랫폼별 · U10 SSRC 디먹스 · SRTP · TLS · 이벤트 구독 패치    │
└────────────────────────────────────────────────────────────────────────┘
```

| 층 | 위치 | 산출물 | 빌드 |
|---|---|---|---|
| ① 엔진 | `ext/pjproject`, `sdk/engine/` | libpjsua2 (.a/.so/.dll) | Linux: 루트 CMake ExternalProject / Android: NDK `configure-android` / Windows: MSVC |
| ② 코어 | `sdk/core/` | `libcimsue` + `cimsue-cli` | 루트 CMake (Linux), NDK (Android), MSVC (Windows) |
| ③ Android SDK | `sdk/android/` | `cimsue-android.aar` (arm64-v8a) | Gradle |
| ③ Windows SDK | `sdk/windows/` | `cimsue.dll` + `include/` (+ 선택 `cimsue_c.h`) | MSVC / CMake |
| ④ 앱 | `android/*`, `windows/*` | APK / MSI | Gradle / MSVC |

---

## 3. 엔진 — `ext/pjproject` 정본화

**`ext/pjproject` 가 유일한 소스 정본이다.** pjproject 를 수정할 때는 이 트리를 직접 고치고 git 이 이력을
보관한다. psip·opencore-amr 을 `ext/` 에 두고 쓰는 관례와 같다. Android `.so` 빌드 스크립트
(`sdk/android/build-native.sh`)는 upstream clone·패치 적용 단계를 갖지 않고 **이 트리를 NDK 로 빌드하고
SWIG 을 돌린 뒤 산출물을 배치하는 절차만** 담는다(`android/docs/scripts/m1_build_pjsip.sh` 는 위임 스텁).
적용된 패치 인벤토리는 `ext/pjproject/README.CIMS.md`.

| 항목 | 규약 |
|---|---|
| **config_site.h** | upstream 이 무시하는 파일이므로 `sdk/engine/config_site/{common,android,windows,linux}.h` 로 커밋한다. `common.h` 에 세 플랫폼이 같아야 하는 결정(U10, `PJMEDIA_HAS_SRTP 1`, `PJSIP_HAS_TLS_TRANSPORT 1`, 코덱 표면 축소 — G.711 안전망 유지·AMR-NB/VP8/VP9/Speex 등 off, 이벤트 구독 패치 스위치)을 두고, 플랫폼 파일은 `common.h` 를 include 한 뒤 장치·코덱 백엔드만 정한다(Android=And-Media MediaCodec 코덱·OpenSL/AAudio, Windows=WASAPI·SDL 창 off, Linux=null 장치·영상 off·opencore AMR-WB). 빌드가 `pjlib/include/pj/config_site.h` 에 해당 플랫폼 파일을 `#include` 하는 한 줄을 생성한다 — 플랫폼 파일이 `common.h` 를 상대경로로 include 하므로 복사가 없고 pjproject 트리에는 gitignore 된 한 줄짜리 파일만 생긴다 |
| **Linux 빌드** | 루트 `CMakeLists.txt` 의 `ExternalProject_Add(pjproject)` (`option(CIMS_UE_SDK ON)`) — `aconfigure`(`--disable-sound --disable-video`, 서버가 만든 `pkg/opencore-amr`·`pkg/vo-amrwbenc` 링크, `-fPIC`) → `make dep` → `make lib` → `pkg/pjproject` 설치. 코어·`cimsue-cli`·단위시험이 링크한다(`pkg/pjproject/lib/pkgconfig/libpjproject.pc` 의 Libs/Libs.private + opencore·vo-amrwbenc 라이브러리 경로). 코덱은 config_site 로만 끈다 — configure `--disable-speex-codec` 은 third_party/speex 를 빼서 AEC(`echo_common.o`) 링크가 깨진다. pjproject 자체 CMake 는 upstream 이 Linux x86_64 만 시험한 실험 단계라 쓰지 않는다 |
| **Windows 빌드** | 같은 트리의 `pjproject-vs14.sln`(MSVC) 로 빌드한다. pjproject CMake 채택은 upstream 안정화 후 |
| **libsrtp 경계** | 서버(CMP)는 `ext/libsrtp` 독립 vendoring, 단말 엔진은 pjproject 동봉 `third_party/srtp`. 같은 CMake 트리에 들어오므로 타겟 이름·include 경로를 분리하고 서로 링크하지 않는다(루트 CMake 의 기존 주석이 규약) |
| **플랫폼 한정 패치** | `android_jni_dev` 무전/통화 분리 라우팅([2-10])처럼 한 플랫폼 파일 안에 있는 패치는 그대로 둔다. Windows 의 헤드셋·스피커 분리 출력은 pjsua2 `ExtraAudioDevice` 로 코어 레벨에서 푼다(§6) |
| **버전** | pjproject upstream 태그 + CIMS 패치 목록은 `ext/pjproject/README.CIMS.md` 가 기술한다. upstream 갱신은 트리 위에서 merge/rebase 로 한다 |

---

## 4. 코어 `libcimsue`

### 4.1 모듈

```
sdk/core/
  include/cimsue/       공개 헤더 = 플랫폼 SDK·바인딩 생성의 정본 (§4.2)
  src/
    engine.cpp          Endpoint 부팅·계정(REGISTER Digest/AKA·sec-agree)·호(INVITE/BYE/보류)·MCPTT 세션(그룹콜/사설콜
                        multipart INVITE·SDP m=application 주입/학습·a=recvonly 청취·착신 자동 수락)·affiliation PUBLISH·
                        conference/xcap-diff SUBSCRIBE·MESSAGE 송수신 라우팅(SDS/roster/기타) · account_map.cpp 매핑 규칙
                        (확장: Join·Replaces·REFER·dialog RFC 4235 — D 단계)
    media/              장치 추상(코어는 pjmedia 장치 id 만 다룸) · 호별 rx 레벨/뮤트/listen · SSRC 소스 테이블
                        (U10 산출: 호 안의 소스별 활성·레벨·RFC 5576 label) · 영상 프레임 콜백(창 없음)
    floor/              floor_defs.h(생성) · floor_codec(TS 24.380 §8 RTCP-APP TLV, CMP 코덱과 바이트 호환) ·
                        floor_participant(§6.2.4 상태머신 + UDP 소켓 + Ack keepalive·Revoke Release 재전송·MSN 폐기·
                        요청 시한·Granted Duration 자체 종료·청취 전용) — 원천 android FloorClient.kt
    mcptt/              mcptt_xml — mcptt-info·resource-lists·affiliation-command 빌더, mcptt-info/conference-info 파서
    mcdata/             sds_codec — TS 24.282 SDS SIGNALLING/DATA PAYLOAD/NOTIFICATION TLV + multipart(base64) 빌드·파싱,
                        Java 호환 conversation id (확장: MSRP 미디어평면·FD 업/다운로드)
    csc/                OAuth2 PKCE(IdMS) · XCAP(GMS 그룹·CMS user-profile/service-config, ETag) ·
                        `/provisioning/me` · `/provisioning/directory` · FD 스토어 — HTTP 전송은 인터페이스(§4.4)
    domain/             UE 세션 모델(등록·호 목록·그룹/채널·affiliation·긴급/경보·데스크) → 상태 스냅샷 + 이벤트
    http/               IHttpTransport 기본 구현(libcurl+OpenSSL)
  cli/                  cimsue-cli — 헤드리스 UE (Linux; S3 시나리오·cspsim 보완, §9)
  test/                 googletest — floor/mcdata 코덱 교차 검증·SDP·상태머신
```

이식 원천은 `android/` 의 Kotlin 구현이며, 이식 후 Kotlin 은 파사드로 줄어든다(§5.3).

| 코어 모듈 | 원천 (참조 구현) | 승계하는 결정 |
|---|---|---|
| sip | `core/sip/SipController.kt`·`CimsAccount.kt`·`CimsCall.kt` | Digest 매핑·in-dialog 구독 갱신 함정(`cims_conf_find`)·multipart mcptt-info re-INVITE·MSRP INVITE·콜백 정합 |
| floor | `ptt-client/floor/FloorClient.kt`·`FloorCodec.kt`·`FloorControl.kt` | opcode/field/cause 상수 → 단일 정의 테이블(§4.6) |
| mcdata | `ptt-client/mcdata/McDataCodec.kt`·`msrp/MsrpSession.kt`·`MsrpCodec.kt` | SDS TLV·MSRP 프레이밍·FD |
| csc | `ptt-client/csc/CscClient.kt`·`core/provision/ProvisioningClient.kt`·`Pkce.kt` | PKCE S256·XCAP 경로·If-None-Match 304 |
| domain | `ptt-client/PttController.kt` 의 프로토콜 부분 | 세션 목록·listen policy·affiliation·긴급/경보·CMS AND 게이트(user-profile ∧ service-config)·N2 미강제 |

### 4.2 공개 API 모델

API 는 **명령(즉시 `Result`/id 반환, 프로토콜 결과는 이벤트)** · **상태 스냅샷(조회)** · **이벤트(리스너 1개)** 세 갈래다.
플랫폼 SDK 는 이 셋을 각자의 관용구(Kotlin `StateFlow`/`SharedFlow`, C++ 콜백)로 옮기기만 한다.

C++ 공개 표면은 `cimsue/engine.h` 의 `Engine` 하나이며 계정·호를 **id 로** 다룬다(`addAccount → accountId`,
`dial → callId`, `answer(callId)` …). 바인딩이 단순하고 수명 문제(콜백 중 객체 삭제)가 없기 때문이다. 아래 표의
`Account`/`Call`/`Group` 같은 객체 구분은 API 의 **의미 묶음**이고, 객체형 파사드(Kotlin `CimsUe.Call` 등)는
플랫폼 SDK 가 id 를 감싸 제공한다. 공개 헤더(`types.h`·`listener.h`·`engine.h`)는 pjsua2 를 include 하지 않는다.

| 객체 | 명령 | 상태 | 이벤트 |
|---|---|---|---|
| `Engine` | `start(EngineConfig)` · `stop()` · `setAudioDevice(capture, playback)` · `addExtraPlayback(dev)` | 장치 목록 | `onLog` · `onAudioDeviceLost` |
| `Provisioning` | `login(user, pw)` · `setAccessToken` · `fetchProfile()` · `fetchDirectory()` · `logout()` | `Profile{services[], dispatch?}` · `Directory` | `onProfile` · `onAuthFailed` |
| `Account` (서비스 kind 당 1) | `register()` · `unregister()` · `refresh()` | `RegState{unregistered, registering, registered(code), failed(reason)}` | `onRegState` |
| `Call` | `dial(uri, {video, emergency})` · `answer({video})` · `reject()` · `hangup()` · `hold/resume` · `mute(on)` · `listen(on)` · `rxLevel(f)` · `sendDtmf` · **`join(targetDialog)`**(RFC 3911, `a=recvonly`) · **`pickup(number?)`**(피처코드·지정 픽업) · **`transfer(target, {attended})`**(REFER) · **`replace(dialog)`**(RFC 3891) | `CallState{outgoing, incoming(remote, calledParty, isPilot), active, held, disconnected(code)}` · `MediaSources[]{ssrc, label, active, level}` · `videoSources[]` | `onCallState` · `onMediaSource` · `onVideoFrame(source, frame)` · `onTransferProgress` |
| `Group` (PTT) | `affiliate(on)` · `joinGroupCall({emergency, imminent})` · `leave()` · `startAdhoc(members)` · `startPrivate(peer, {duplex, emergency})` · **`listenGroupCall()`**(recvonly JOIN, §7) · `setPrimary` · `channelVolume(f)` · `emergency(on)` · `alert(on)` | `GroupCallState{idle, joining, active(listenOnly), ...}` · roster · affiliated | `onGroupCall` · `onRoster`(RFC 4575) · `onAlert` |
| `Floor` (그룹콜당 1) | `request(prio)` · `release()` · `queueCancel()` · `mediaFlow(on)` | `FloorState{idle, requesting, granted(duration), taken(speaker, permissionToRequest), queued(pos), denied(cause), revoked}` · `speakers[]`(multi-talker) | `onFloor` |
| `Sds` | `sendGroupText(group, text)` · `sendGroupFile(group, bytes, name, mime)` · `sendNotification(peer, conv, msg, type)` · `download(url)` | 발신 진행 | `onIncomingSds` · `onSendResult` · `onDisposition` |
| `Subscriptions` | `dialogWatch(scope)`(RFC 4235 — 관제 범위) · `conference(group)` · `xcapDiff(psi)` · `presence(uri)` | 감시 dialog 목록 `{dialogId, parties, state, isPilotCall}` | `onDialogList` · `onXcapChanged` |

규약:

- **식별자.** 호는 `callId`, 그룹은 그룹 URI, 감시 dialog 는 RFC 4235 `dialog id`(call-id·tags). 코어가 준
  id 만 되돌려 쓰고 앱이 URI 문자열을 조립하지 않는다([../identifier_model.md](../identifier_model.md)).
- **정책 게이트는 UX 선차단.** CMS user-profile ∧ service-config 판정을 코어가 `Capabilities` 스냅샷으로
  노출하고 앱은 버튼을 숨길 뿐이다. 최종 판정은 서버(403/Floor Deny). 문서를 아직 못 받았으면 게이트를 걸지
  않는다(android_ue_client §7 과 동일).
- **에러 모델.** 명령은 즉시 `Result{ok, reason}` 을 돌려주고(인자·상태 오류), 프로토콜 결과는 이벤트로 온다.
- **ABI.** 공개 헤더는 pjsua2 타입을 include 하지 않는다. 구현체는 pImpl.

### 4.3 스레딩·수명 규칙

[android_ue_m1_pjsip_integration.md](android_ue_m1_pjsip_integration.md) §3.4 의 규칙을 코어가 흡수한다.
플랫폼 SDK 는 이 규칙을 알 필요가 없다.

- **명령 직렬화.** 모든 pjsua2 호출은 코어 내부의 제어 스레드 1개(`ue-ctl`, 부팅 시 `libRegisterThread`)로
  직렬화한다. 공개 명령은 어느 스레드에서 불러도 되고 즉시 반환한다.
- **이벤트 전달.** pjsua2 콜백(PJSIP 스레드) 안에서는 상태를 기록하고 이벤트를 큐에 넣기만 한다. 리스너
  호출은 별도 이벤트 스레드에서 하며, 리스너 안에서 코어 명령을 다시 불러도 교착하지 않는다.
- **수명.** `Account`/`Call` 래퍼는 네이티브 콜백 동안 살아 있어야 하므로 코어가 강참조 테이블로 보관하고
  `DISCONNECTED` 에서만 해제한다. 장치 미디어 포트는 보관하지 않고 매 이벤트마다 재취득한다.
- **바인딩 스레드.** SWIG director 콜백은 이벤트 스레드에서 오므로 Android 파사드가 메인 스레드로 마샬링한다.

### 4.4 HTTP 전송 인터페이스

코어가 **프로토콜**(PKCE·Bearer·XCAP 경로·ETag/304·프로비저닝 파싱·FD 업로드)을 소유하고, **전송**은
`IHttpTransport{request(method, url, headers, body) → response}` 로 추상한다. 기본 구현은 `libcurl + OpenSSL`
(pjproject 가 이미 OpenSSL 을 링크하므로 추가 의존은 curl 하나). Android 에서 인증서 정책·프록시 이유로
OkHttp 가 필요하면 파사드가 구현체를 주입한다. TLS 트러스트(사설 CA)는 `EngineConfig.trustAnchors` 로 코어에
넘기고 SIP TLS 와 HTTPS 가 같은 앵커를 쓴다.

### 4.5 미디어 경계

- **장치.** 코어는 pjmedia 장치 id 와 라우트 의미(`earpiece/speaker/headset/bluetooth/extra`)만 다룬다. Android
  의 AudioManager 모드·포커스·블루투스 SCO, Windows 의 WASAPI 엔드포인트 선택은 플랫폼 SDK 몫이다.
- **SSRC 소스.** U10 디먹스가 만든 서브스트림을 코어가 `MediaSources[]` 로 노출한다. 감청 leg 는 RFC 5576
  `a=ssrc … label` 을 파싱해 각 소스에 발신자/착신자 라벨을 붙인다. 믹싱은 pjmedia 안에서 끝나고(브리지 포트 1개)
  앱은 소스별 활성·레벨 표시만 한다(dispatch_center §5.4).
- **영상.** 코어는 창을 열지 않는다. 디코드된 프레임을 `onVideoFrame(source, frame)` 으로 준다(Android 는
  Surface 를 받아 pjmedia 가 직접 그리는 기존 경로를 유지할 수 있다 — 파사드 선택). 감청 영상 격자 합성은 UI 몫.
- **캡처.** 카메라·마이크 권한과 장치 열기는 플랫폼 SDK 가 하고, 코어는 `setCaptureEnabled` 로 on/off 만 한다.

### 4.6 floor 코덱 단일 정의

[mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md) §6 의 결론. 정본 테이블은
`docs/design/features/mcptt_floor_defs.yaml`(opcode·field id·indicator 비트·source/permission/queued purpose·cause 문구)
이고 `scripts/gen_floor_defs.py` 가 코어 헤더 `sdk/core/src/floor/floor_defs.h` 를 생성한다(CMake 가 테이블 변경 시 재생성).
`gen_floor_defs.py --check` 는 생성물 최신성과 `cmp/PMcpttGroup.h`·android `FloorControl.kt`·
`scripts/mcptt_floor_policy_probe.py` 의 상수를 테이블과 대조한다(CMP·Kotlin 은 생성물이 아니라 대조 대상 — 값이
어긋나면 S1 이 막는다). 알고리즘 드리프트는 `cimsue_test` 의 교차 검증이 잡는다 — 코어 빌더 출력을 CMP
`ParseFloorMessage` 로, CMP `BuildFloorMessage` 출력(Granted ack-요구·Taken 리스트·Deny·Queue)을 코어 `decode` 로.

### 4.7 `cimsue-cli`

코어 위의 헤드리스 UE(`sdk/core/cli`, `build/bin/cimsue-cli`). Linux 에서 빌드되며 등록·1:1 호(→ 그룹콜·floor·
SDS·Join·픽업으로 확장)를 명령행으로 구동한다. cspsim 은 서버 검증용 시뮬레이터로 그대로 두고, `cimsue-cli` 는
**실제 단말 스택**으로 같은 S3 시나리오를 한 번 더 확인하는 축이다(§9).

```
cimsue-cli [계정] register [--hold S]            # 200 OK → (hold) → de-REGISTER
cimsue-cli [계정] call <번호|sip:URI> [--duration S] [--video]
cimsue-cli [계정] answer [--duration S]          # 착신 대기 → 200 → 상대 BYE 또는 duration (MCPTT 착신은 자동 수락)
cimsue-cli [계정] group-call <groupId> [--duration S] [--ptt-at S --ptt-len S] [--listen-only] [--emergency]
cimsue-cli [계정] sds <groupId> <text>           # MESSAGE 최종 응답까지
cimsue-cli [계정] sds-recv [--duration S]        # 수신 SDS 를 JSON 줄로
계정: --server IP --port N --transport udp|tcp|tls --domain D --msisdn M (--imsi I|--auth-id IMPI)
      (--ha1 HEX32|--password P) [--mcptt-id tel:..] [--affiliate G,..] [--srtp off|optional|required] [--sec tls]
      [--tls-ca PEM] [--json]
```

결과는 stdout 에 JSON 한 줄(`outcome`·`rx_pkts`·`tx_pkts`·`granted`·`taken`·`denied`·`code`), 종료코드 0/2/3/4/5/6/7
(성공/인자/등록/호/미디어 없음/floor 미획득/SDS 실패). `--affiliate` 는 시작 시 PUBLISH(Event: mcptt), 종료 시 de-affiliate.
오디오 장치는 null(헤드리스) — 브리지는 돌고 RTP 는 흐른다. 통계는 스트림 소멸 시점(`onStreamDestroyed`)에
보존해 상대가 먼저 끊어도 남는다. 사용 예는 [VERIFICATION_MANUAL.md](../../VERIFICATION_MANUAL.md) 부록.

---

## 5. Android SDK (`sdk/android`)

### 5.1 구성

```
sdk/android/
  cimsue/                  Gradle Android Library → cimsue-android.aar
    src/main/jniLibs/      arm64-v8a/{libcimsue,libpjsua2,libc++_shared}.so  (빌드 산출물, 커밋 안 함)
    src/swig/              SWIG 생성 Java (com.cims.ue.sdk.jni.*) — 손코드와 소스셋 분리
    src/main/java/com/cims/ue/sdk/
      CimsUe.kt            Kotlin 파사드: 코어 상태 → StateFlow, 이벤트 → SharedFlow, 명령 → fun (메인 스레드 마샬링)
      platform/            Android 접점: AudioRouter(모드·포커스·SCO) · CameraCapture · UeForegroundService 헬퍼 ·
                           SecureStore(Keystore/EncryptedSharedPreferences) · SsoAccount(AccountManager) · BootRegister
      http/                OkHttpTransport (IHttpTransport 구현, 선택)
  build-native.sh          ext/pjproject + sdk/core 를 NDK 로 빌드 → jniLibs + SWIG Java
```

- **바인딩은 SWIG.** pjsua2 가 이미 SWIG 을 쓰므로 코어도 `cimsue.i` 한 파일로 Java 를 생성한다. 이벤트
  리스너는 director. 손 JNI 는 두지 않는다.
- **파사드가 유일한 공개면.** 앱은 `com.cims.ue.sdk.*` 만 import 한다. `com.cims.ue.sdk.jni.*` 와 `org.pjsip.*`
  는 파사드 내부다.
- **Android 접점의 책임.** 오디오 포커스·모드·라우팅(무전/통화 분리 출력 포함), 카메라, Foreground Service
  (등록 유지·wakelock·알림), HW PTT 키 이벤트 전달, Keystore 저장, SSO AccountManager·`/provisioning/me` 캐시,
  부팅 재등록. 이들은 프로토콜을 모른다.

### 5.2 배포

AAR 하나(arm64-v8a). 버전은 코어 버전 + Android 패치 번호(`1.4.0-android.2`). GPL 트랙(pjproject)은
android_ue_client §13 그대로.

### 5.3 기존 `android/` 와의 관계

`android/core` 는 Android Library 이면서 pjsua2 SWIG·SipController·프로비저닝·계정·연락처 저장을 한 모듈에
담고 있고, `ptt-client` 가 floor·mcdata·csc 를 따로 가진다. 최종 상태는 다음과 같다.

| 지금 | 최종 |
|---|---|
| `android/core` 의 pjsua2 SWIG + `.so` | `sdk/android/cimsue` 안으로 이동(엔진은 코어가 링크) |
| `android/core/sip/*` (SipController 등) | 코어 `sip` 로 이식. Kotlin 은 `CimsUe` 파사드 |
| `android/ptt-client/{floor,mcdata,csc}` | 코어 `floor/mcdata/csc` 로 이식 |
| `android/ptt-client/PttController.kt` | 프로토콜 부분은 코어 `domain`, UI 상태 부분은 앱 ViewModel 로 분해 |
| `android/core/{account,provision,contacts,calllog,message,config}` 저장·SSO | `sdk/android/platform` (Android 접점) 또는 앱 |
| `android/cims` (SSO 로그인 앱) | 유지 — `sdk/android` 의 `SsoAccount` 를 사용 |
| `volte-client`·`ptt-client` | `implementation(project(":sdk:cimsue"))` 로 전환 |
| 신규 `android/dispatch-tablet` | 관제조작반 태블릿 앱 (§7) |

---

## 6. Windows SDK (`sdk/windows`)

```
sdk/windows/
  include/cimsue/          코어 공개 헤더 (sdk/core/include 와 동일 — 설치 복사)
  cimsue.dll / cimsue.lib  코어 + 엔진 정적 링크 (x64)
  cimsue_c.h (선택)        C API — UI 를 C#(WPF/WinUI 3) 으로 만들 때만 P/Invoke 용으로 생성
  platform/                Windows 접점: WASAPI 엔드포인트 열거·선택 · ExtraAudioDevice 로 헤드셋+스피커 분리 출력 ·
                           전역 핫키(PTT·응답) · 단일 인스턴스 · 자격 저장(DPAPI) · 자동 시작
```

- UI 는 **C++**(Qt 또는 WinUI 3 C++/WinRT) 를 전제로 하며 이 경우 헤더가 곧 SDK 이고 추가 바인딩이 없다.
  C# UI 를 택하면 C API 한 겹만 추가된다.
- 영상은 §4.5 프레임 콜백을 UI 가 그린다(SDL 창 off). 감청 영상 격자 합성은 UI.
- 오디오는 pjmedia WASAPI 백엔드. 관제석의 헤드셋(통화)·스피커(PTT 채널 청취) 동시 출력은 `ExtraAudioDevice`
  로 두 번째 재생 장치를 열어 채널별로 연결한다 — 실기 검증 항목(§11).
- 배포는 MSI(DLL·헤더는 zip/NuGet). 코드 서명·설치 정책은 운영 몫.

---

## 7. 관제조작반 앱 — 요구 ↔ 코어 API

[dispatch_center.md](dispatch_center.md) §8.4 와 [volte_supplementary_services.md](volte_supplementary_services.md)
가 요구하는 관제 단말 능력을 코어 API 로 대응시킨다. 두 플랫폼 앱은 같은 API 를 쓰며 화면만 다르다.

| 관제 요구 | 서버 계약 | 코어 API | 규격 |
|---|---|---|---|
| 자기 데스크 인지(그룹·대표번호·감청/청취 범위·은닉 여부) | `/provisioning/me` `dispatch{groupId, groupName, pilotId, monitorScope, pttListen, listenVisibility}` | `Provisioning.Profile.dispatch` | CSC |
| 대표번호 착신 표시·데스크 UI | `P-Called-Party-ID` = 대표번호 | `CallState.incoming.isPilot / calledParty` | RFC 3455 |
| 그룹 범위 통화 목록(BLF) | dialog 이벤트, 인가 범위 `monitor_scope` | `Subscriptions.dialogWatch(scope)` → `onDialogList` | RFC 4235 |
| 링잉 대표번호 호 당겨받기·지정 픽업 | 피처코드 + 대상 번호, 403/404/489 | `Call.pickup(number?)` | TS 24.239 |
| 호 전달(blind/attended) | REFER, `transfer_allowed` | `Call.transfer(target, {attended})` | RFC 3515 |
| 통화 청취 합류 | INVITE `Supported: join` + `a=recvonly` + SDES | `Call.join(dialogId)` | RFC 3911 |
| 감청 두 화자 귀속 표시 | SSRC 2개 분리 인도 + `a=ssrc … label` | `MediaSources[]{label=A/B, level, active}` (U10 공용) | RFC 5576, TS 33.108 |
| 감청 영상 격자 | 영상 SSRC 2개 | `videoSources[]` + `onVideoFrame` → UI 격자 | — |
| PTT 그룹콜 청취 | 그룹 AoR 로 recvonly INVITE → `PTT_JOIN recv_only=1` | `Group.listenGroupCall()` → `GroupCallState.active(listenOnly=true)` | TS 24.379 |
| 청취 중 PTT 버튼 비활성 | Floor Taken `Permission to Request the Floor=0` | `FloorState.taken.permissionToRequest=false` (앱은 버튼 disable) | TS 24.380 |
| 비멤버 sendrecv 거절 | 403 | `onGroupCall(failed 403)` 표시 | — |
| SRTP·TLS | 접속서비스 `media_srtp`·`sip_transport` | 프로파일 값으로 코어가 자동 협상. 앱 개입 없음 | RFC 4568, TS 33.203 |

앱 화면 구성은 [dispatch_center.md](dispatch_center.md) §5·§7 의 관제사 동작에 대응하는 **dialog 보드·대표번호
데스크·감청 패널(A/B 귀속·레벨)·PTT 청취 채널·SDS·디렉터리** 다섯 구획이며, 태블릿(가로)과 데스크톱이 같은
구획을 다른 밀도로 배치한다. 화면 상세는 앱 구현 시 UI 문서로 분리한다.

---

## 8. 빌드·패키징

| 대상 | 방법 | 산출물 |
|---|---|---|
| Linux (개발 서버) | 루트 `cmake .. && make` — `pjproject`(ExternalProject, `make pjproject` 단독 가능) → `cimsue` → `cimsue-cli` → `cimsue_test`. `make dist` 에 `cimsue-cli` 를 verify 도구로 포함 | `pkg/pjproject`, `build/bin/cimsue-cli`, `build/lib/libcimsue.a` |
| Android | `sdk/android/build-native.sh` — `config_site.h` 한 줄 생성(→ `android.h`) → `configure-android`(NDK) → `make` → SWIG(pjsua2+cimsue) → `.so`·Java 를 `jniLibs`/소스셋에 배치. 그 뒤 Gradle | `cimsue-android.aar` |
| Windows | `sdk/engine/config_site/windows.h` 복사 → `pjproject-vs14.sln` → `sdk/windows` CMake/MSVC | `cimsue.dll`·헤더 |

NDK/MSVC 빌드는 개발 서버 밖(WSL2·Windows 머신)에서 수행하고, 이 서버는 Linux 빌드·단위시험·`cimsue-cli`
검증을 담당한다. 산출물(`.so`/`.dll`)은 커밋하지 않는다.

---

## 9. 검증

| stage | 항목 | 내용 |
|---|---|---|
| S1 | `S1-UE-FLOOR-CODEC` | `scripts/gen_floor_defs.py --check`(정의 테이블 ↔ 생성물·CMP·Kotlin·probe 상수) + `cimsue_test` 의 `FloorXCheck`(코어 빌더 ↔ CMP `ParseFloorMessage`, CMP `BuildFloorMessage` ↔ 코어 decode) |
| S1 | `S1-UE-UNIT` | `build/bin/cimsue_test`(googletest) — config→pjsua2 매핑(IMPI·realm `*`·H(A1)/AKA 우선·TLS 게이트 SRTP·sec-agree 헤더·proxies lr)·대상 정규화·헤더 파싱. 확장: SDP 협상·floor 상태머신·SDS TLV·MSRP·PKCE |
| S3 | `S3-UE-CLI-*` | `cimsue-cli` 로 등록(UDP/TLS/AKA)·1:1(평문·TLS+SRTP)·그룹콜(affiliation PUBLISH ETag·multipart INVITE·로스터 NOTIFY·floor Request→Granted/Taken·발언 RTP 수신·Idle)·SDS 송수신·Join·픽업·PTT 청취 — 기존 `S3-SCN-*` 의 cspsim 축과 같은 판정(누적 RTP delta·403/489). 수동 절차는 VERIFICATION_MANUAL 부록, cims-verify 항목 등록은 후속 |
| 실기기 | Android | 태블릿·UNIWA 에서 감청 SSRC 2개 귀속 표시·PTT 청취 버튼 비활성·대표번호 착신 — 와이어 실측 |
| 실기기 | Windows | WASAPI 이중 출력·핫키·감청 영상 격자 |

---

## 10. 이행 순서 (Android 먼저)

앱을 끊지 않고 아래 순서로 코어를 채운다. 각 단계는 앞 단계 산출물 위에서 기존 PTT/VoLTE 앱이 그대로
동작하는 것을 완료 조건으로 한다.

| 단계 | 내용 | 완료 조건 |
|---|---|---|
| A. 엔진 정본화 | `ext/pjproject` 를 SoT 로, `sdk/engine/config_site/*` 커밋, 빌드 스크립트에서 clone/패치 단계 제거, 루트 CMake ExternalProject | Linux 에서 libpjsua2 빌드, 기존 Android `.so` 재생산 일치 |
| B. 코어 골격 + sip/media | `sdk/core` 생성, 공개 헤더 §4.2, `ue-ctl` 스레딩, Account/Call(등록·1:1·영상·SRTP·TLS·AKA), SWIG `cimsue.i`, `cimsue-cli` 등록/1:1 | `S3-UE-CLI` 등록·1:1 PASS. volte-client 가 파사드로 전환 |
| C. floor + mcdata + 구독 | 정의 테이블(§4.6)·floor participant·SDS/MSRP·conference/xcap-diff 구독·그룹콜/affiliation/긴급 | `S1-UE-FLOOR-CODEC`·`S3-UE-CLI` 그룹콜/floor/SDS PASS. ptt-client 가 파사드로 전환, PttController 분해 |
| D. csc + domain + 관제 API | PKCE/XCAP/프로비저닝(dispatch 블록)·`Capabilities`·dialogWatch·join·pickup·transfer·listenGroupCall·`MediaSources` 라벨 | `S3-UE-CLI` Join/픽업/PTT 청취 PASS |
| E. 관제 태블릿 앱 | `android/dispatch-tablet` — §7 다섯 구획 | 실기기 실측(§9) |
| F. Windows SDK·앱 | `sdk/windows` + `windows/dispatch-desktop` — 코어 변경 없이 플랫폼 접점·UI 만 | WASAPI 이중 출력·감청 격자 실측 |

---

## 11. 미해결 / 향후 과제

- **Windows 오디오 이중 출력** — `ExtraAudioDevice` 로 헤드셋+스피커 동시 출력의 지연·에코 실측.
- **Android 영상 경로 선택** — Surface 직결(현행) vs 프레임 콜백(§4.5). 감청 격자 합성이 필요한 관제 태블릿은
  프레임 콜백이 맞고, 1:1 영상 앱은 Surface 직결이 싸다. 파사드가 둘을 다 제공할지 결정.
- **C API 필요 여부** — Windows UI 스택 확정 후(C++ 이면 불필요).
- **개발 서버 TLS 인증서** — `build/dist/csp/cert/csp.pem` 이 자가서명·SAN 없음이라 코어의 서버 검증(`tlsVerifyServer`)을
  켠 채로는 등록이 503 `PJSIP_TLS_ECERTVERIF` 로 막힌다(정상 동작). TLS/SRTP 회귀를 검증 켠 채 돌리려면
  sip_tls_signaling.md §8 요건(SAN=도메인/IP)의 인증서를 개발 서버에 발급해야 한다.
- **remote-init ambient listening·barge-in** — 서버 §10 과제와 함께 코어 API 확장.
- **cspsim 과 `cimsue-cli` 의 역할 분담 장기안** — 시뮬레이터 축(부하·다중 단말)과 실스택 축(정합)의 S3 항목 배분.

## 12. 문서 갱신 대상 (구현과 같은 변경에서)

- [android_ue_client.md](android_ue_client.md) §2.2·§9 — 앱 내부 레이어·모듈 구조를 본 문서로 위임.
- [android_ue_m1_pjsip_integration.md](android_ue_m1_pjsip_integration.md) §2 — 빌드 플레이북을 `ext/pjproject` 정본·`build-native.sh` 기준으로 재작성.
- [mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md) §6 — 정의 테이블 파일·생성 규약 확정 반영.
- [dispatch_center.md](dispatch_center.md) §7 단말 행·§8.4 — 코어 API 이름으로 교차 참조.
- `ext/pjproject/README.CIMS.md` — 정본 관계(트리 = SoT).
- [../../VERIFICATION_PROCESS.md](../../VERIFICATION_PROCESS.md) — `S1-UE-*`·`S3-UE-CLI-*` 항목 등록.
