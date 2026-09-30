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
| 플랫폼 SDK | Android = SWIG Java 바인딩 + Kotlin 파사드 + Android 접점(AAR). Windows = C API(`cimsue_c.h`, 같은 DLL) + .NET 파사드(C#, P/Invoke) + Windows 접점(관리 코드) | §5·§6 |
| UI | 플랫폼 네이티브(Android Compose, Windows WPF/.NET). SDK 는 UI 프레임워크를 모른다 | §7 |

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
│                    windows/dispatch-desktop (WPF)                        │
├────────────────────────────────────────────────────────────────────────┤
│ ③ 플랫폼 SDK        sdk/android  (Kotlin 파사드 + SWIG Java + Android 접점) │
│                    sdk/windows  (.NET 파사드 + C API + Windows 접점)       │
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
| ③ Windows SDK | `sdk/windows/` | `cimsue.dll`(C++ 코어 + C API `cimsue_c.h`) + `CimsUe.dll`(.NET 파사드) | MSVC / CMake + `dotnet` |
| ④ 앱 | `android/*`, `windows/*` | APK / MSIX | Gradle / `dotnet`(WPF) |

---

## 3. 엔진 — `ext/pjproject` 정본화

**`ext/pjproject` 가 유일한 소스 정본이다.** pjproject 를 수정할 때는 이 트리를 직접 고치고 git 이 이력을
보관한다. psip·opencore-amr 을 `ext/` 에 두고 쓰는 관례와 같다. Android `.so` 빌드 스크립트
(`sdk/android/build-native.sh`)는 upstream clone·패치 적용 단계를 갖지 않고 **이 트리를 NDK 로 빌드하고
SWIG 을 돌린 뒤 산출물을 배치하는 절차만** 담는다(`android/docs/scripts/m1_build_pjsip.sh` 는 위임 스텁).
적용된 패치 인벤토리는 `ext/pjproject/README.CIMS.md`.

| 항목 | 규약 |
|---|---|
| **이벤트 구독 슬롯** | `pjsua_pres.c` 의 CIMS 구독 표(conference·xcap-diff·dialog 공용)는 `PJSUA_CIMS_MAX_SUB`(기본 256) 슬롯 — 관제조작반은 대표번호 + 감시 대상 전원(`monitor_scope=all` 이면 조직 전원) dialog 와 채널·청취 범위 conference, GMS PSI xcap-diff 를 동시에 든다. 넘치면 `PJ_ETOOMANY`(앱 로그 `dialogWatch …: -3 subscribe failed`) — 조직 규모가 크면 config_site 에서 올린다 |
| **config_site.h** | upstream 이 무시하는 파일이므로 `sdk/engine/config_site/{common,android,windows,linux}.h` 로 커밋한다. `common.h` 에 세 플랫폼이 같아야 하는 결정(U10, `PJMEDIA_HAS_SRTP 1`, `PJSIP_HAS_TLS_TRANSPORT 1`, 코덱 표면 축소 — G.711 안전망 유지·AMR-NB/VP8/VP9/Speex 등 off, 이벤트 구독 패치 스위치, SIP 메시지 상한 `PJSIP_MAX_PKT_LEN 65535` — 서버발 그룹 INVITE·conference NOTIFY 는 멤버 수에 비례해 커지고 UDP 등록 단말에는 TCP 로 바꿔 보낼 수 없으므로 UDP 데이터그램 최대 크기를 받는다)을 두고, 플랫폼 파일은 `common.h` 를 include 한 뒤 장치·코덱 백엔드만 정한다(Android=And-Media MediaCodec 코덱·OpenSL/AAudio, Windows=WASAPI·SDL 창 off, Linux=null 장치·영상 off·opencore AMR-WB). 빌드가 `pjlib/include/pj/config_site.h` 에 해당 플랫폼 파일을 `#include` 하는 한 줄을 생성한다 — 플랫폼 파일이 `common.h` 를 상대경로로 include 하므로 복사가 없고 pjproject 트리에는 gitignore 된 한 줄짜리 파일만 생긴다 |
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
                        conference/xcap-diff/dialog SUBSCRIBE·MESSAGE/NOTIFY 라우팅(SDS/roster/dialog-info/기타)·관제
                        (dialogWatch RFC 4235 → join RFC 3911 recvonly + a=ssrc 라벨 → sources, pickup 피처코드, transfer REFER
                        blind/attended) · account_map.cpp 매핑 규칙
    media/              장치 추상(코어는 pjmedia 장치 id 만 다룸) · 호별 rx 레벨/뮤트/listen · SSRC 소스 테이블
                        (U10 산출: 호 안의 소스별 활성·레벨·RFC 5576 label) · 영상 프레임 콜백(창 없음)
    floor/              floor_defs.h(생성) · floor_codec(TS 24.380 §8 RTCP-APP TLV, CMP 코덱과 바이트 호환) ·
                        floor_participant(§6.2.4 상태머신 + UDP 소켓 + Ack keepalive·Revoke Release 재전송·MSN 폐기·
                        요청 시한·Granted Duration 자체 종료·청취 전용) — 원천 android FloorClient.kt
    mcptt/              mcptt_xml — mcptt-info·resource-lists·affiliation-command·긴급 경보(alert-ind, §F.1 요소 순서) 빌더,
                        mcptt-info/conference-info/경보 파서, 지시자 삼중값(`indicator` — true/false/없음)
    mcdata/             sds_codec — TS 24.282 SDS SIGNALLING/DATA PAYLOAD/NOTIFICATION·FD SIGNALLING TLV + multipart(base64) 빌드·파싱
                        (그룹/1:1 은 mcdata-info 의 request-type·request-uri 만 다르다 — `buildGroupSds`/`buildOneToOneSds`,
                         FD = `buildGroupFd`/`buildOneToOneFd`(mcdata-info + FD SIGNALLING 두 파트, FILEURL·Metadata — cspsim·Android 와 같은 바이트),
                         conversation ID 는 그룹당(`conversationIdOf`)·쌍당(`conversationIdOneToOne`, 쌍 정렬)),
                        파싱은 request-type 이 `one-to-one-*` 이면 `groupUri` 를 비운다(1:1 의 request-uri = 받는 사람), FD 선택 IE(0x9x·0xAx·0x21·0x22)는 건너뛴다.
                        Java 호환 conversation id
                        msrp — media plane SDS(TS 24.282 §9.2.3, RFC 4975): 프레이밍(청크 Byte-Range·end-line)·SDP m=message 섹션·
                        발신(signalling·payload SEND 2건, 16 KB 청크 stop-and-wait, Success-Report)·수신(바인딩 SEND → 청크 조립, 파트별 SEND 합성).
                        서버(cmdp)가 passive 라 늘 out-connect, 전송 = net/tls_stream 평문 TCP — 원천 android msrp/·cspsim McDataMsrp
    csc/                csc_client — IdMS OAuth2 PKCE(S256) 로그인·refresh, `/provisioning/me`(services→AccountConfig,
                        dispatch 블록), GMS 그룹 목록, XCAP GET(ETag/304), MCData FD 콘텐츠 서버(`uploadFd` = POST /mcdata/fd
                        octet-stream·group 지정 시 서버 게이트, `downloadFd` = FILEURL 의 경로만 취해 자기 CSC 로 — Bearer 를 다른 호스트로
                        보내지 않음). 공개 헤더 `cimsue/csc.h` — Engine 과 독립, 동기 호출, 자체 JSON 파서(pjlib 비의존)
                        group_doc(GMS 그룹 문서) · cms_doc(CMS user-profile·service-config 해석 + `Capabilities::of`) — 스캔 도구 xml_scan.h 공유
    http/               https_client — ITransport(주입 가능) + OpenSSL 기본 구현(HTTP/1.1, chunked, 신뢰 앵커 PEM)
    net/                tls_stream — TCP(+TLS) 클라이언트 스트림(소켓·핸드셰이크·IP/DNS SAN 검증·leaf 지문) — https_client·계측 링크 공용
    quality/            emodel.h(G.107/G.107.1 단일 정의 — 계측기 워커와 공용) · call_quality(pjmedia RTCP·XR → CallQuality) — ue_voice_quality.md §3
    drive/              drive_session(구동 줄 프로토콜 — cimsue-cli drive·계측 링크 공용, Engine 관찰자) · device_link(시험 모드 TLS 계측 링크)
                        — 공개 cimsue/drive.h, ue_voice_quality.md §5
    util/               json_lite — 최소 JSON 파서(CSC 클라이언트·계측 링크)
    csc/                OAuth2 PKCE(IdMS) · XCAP(GMS 그룹·CMS user-profile/service-config, ETag) ·
                        `/provisioning/me` · `/provisioning/directory` · FD 스토어 — HTTP 전송은 인터페이스(§4.4)
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
| (코어 밖 — 앱 세션 층, §5.3) | `ptt-client/PttController.kt` 의 정책 부분 | 세션 목록·listen policy·affiliation 목표 집합과 재시도·채널 복원·긴급 대상 선택·N2 미강제. 규격 절차(긴급 re-INVITE·경보 메시지·CMS AND 게이트 판정 입력)는 코어로 |

### 4.2 공개 API 모델

API 는 **명령(즉시 `Result`/id 반환, 프로토콜 결과는 이벤트)** · **상태 스냅샷(조회)** · **이벤트(리스너 1개)** 세 갈래다.
플랫폼 SDK 는 이 셋을 각자의 관용구(Kotlin `StateFlow`/`SharedFlow`, C++ 콜백)로 옮기기만 한다.
호 스냅샷(`CallInfo`)은 **첫 이벤트부터 호 종류가 확정**돼 있다 — MCPTT 발신(`joinGroupCall`/`startPrivateCall`)은
`onCallState(outgoing)` 첫 스냅샷에 이미 `isMcptt`·`groupId`·`mcptt{sessionType, privateCall, noFloorCtrl,
emergency, imminentPeril, broadcast}`·`halfDuplex`·`listenOnly` 가 실린다(엔진이 makeCall 콜백 안에서 세션 신원을 투영).
앱은 그 값으로 세션 종류를 파생해도 되고, 이후 스냅샷에서 종류가 바뀌는 일은 없다.

C++ 공개 표면은 `cimsue/engine.h` 의 `Engine` 하나이며 계정·호를 **id 로** 다룬다(`addAccount → accountId`,
`dial → callId`, `answer(callId)` …). 바인딩이 단순하고 수명 문제(콜백 중 객체 삭제)가 없기 때문이다. 아래 표의
`Account`/`Call`/`Group` 같은 객체 구분은 API 의 **의미 묶음**이고, 객체형 파사드(Kotlin `CimsUe.Call` 등)는
플랫폼 SDK 가 id 를 감싸 제공한다. 공개 헤더(`types.h`·`listener.h`·`engine.h`)는 pjsua2 를 include 하지 않는다.

| 객체 | 명령 | 상태 | 이벤트 |
|---|---|---|---|
| `Engine` | `start(EngineConfig)` · `stop()` · `setAudioDevice(capture, playback)` · `addExtraPlayback(dev)` · `handleNetworkChange()` | 장치 목록 | `onLog` · `onAudioDeviceLost` |
| `Provisioning` | `login(user, pw)` · `setAccessToken` · `fetchProfile()` · `fetchDirectory()` · `logout()` | `Profile{services[], dispatch?}` · `Directory` | `onProfile` · `onAuthFailed` |
| `Account` (서비스 kind 당 1) | `register()` · `unregister()` · `refresh()` | `RegState{unregistered, registering, registered(code), failed(reason)}` | `onRegState` |
| `Call` | `dial(uri, {video, emergency})` · `answer({video})` · `reject()` · `hangup()` · `hold/resume` · `mute(on)` · `listen(on)` · `rxLevel(f)` · `sendDtmf` · **`join(targetDialog)`**(RFC 3911, `a=recvonly`) · **`pickup(number?)`**(피처코드·지정 픽업) · **`transfer(target, {attended})`**(REFER) · **`replace(dialog)`**(RFC 3891) | `CallState{outgoing, incoming(remote, calledParty, isPilot), active, held, disconnected(code)}` · `MediaSources[]{ssrc, label, active, level}` · `videoSources[]` | `onCallState` · `onMediaSource` · `onVideoFrame(source, frame)` · `onTransferProgress` |
| `Group` (PTT) | `affiliate(on)` · `joinGroupCall({emergency, imminent, broadcast})` · `leave()` · `startAdhoc(members)` · `startPrivate(peer, {duplex, emergency})` · **`listenGroupCall()`**(recvonly JOIN, §7) · `setPrimary` · `channelVolume(f)`(= `setRxLevel`) · **`setCallCondition(emergency, imminent)`**(진행 중 상향·하향 re-INVITE) · **`sendEmergencyAlert(group, on)`** | `GroupCallState{idle, joining, active(listenOnly), ...}` · roster · affiliated · `CallInfo.condition`(긴급·임박 현재값) | `onGroupCall` · `onRoster`(RFC 4575) · **`onMcpttCondition`** · **`onEmergencyAlert`** |
| `Floor` (그룹콜당 1) | `request(prio)` · `release()` · `queueCancel()` · `mediaFlow(on)` | `FloorState{idle, requesting, granted(duration), taken(speaker, permissionToRequest), queued(pos), denied(cause), revoked}` · `speakers[]`(multi-talker) | `onFloor` |
| `Sds` | `sendGroupText(group, text)`(상한 초과 = MSRP) · **`sendText(peer, text)`**(1:1, `one-to-one-sds`) · `sendGroupFile(group, bytes, name, mime)` · `sendNotification(peer, conv, msg, type)` · `download(url)` | 발신 진행 | `onIncomingSds`(`mediaPlane`) · `onSendResult`(MESSAGE·MSRP) · `onDisposition` |
| `Subscriptions` | `dialogWatch(scope)`(RFC 4235 — 관제 범위) · `conference(group)` · `xcapDiff(psi)` · `presence(uri)` | 감시 dialog 목록 `{dialogId, parties, state, isPilotCall}` | `onDialogList` · `onXcapChanged` |

규약:

- **식별자.** 호는 `callId`, 그룹은 그룹 URI, 감시 dialog 는 RFC 4235 `dialog id`(call-id·tags). 코어가 준
  id 만 되돌려 쓰고 앱이 URI 문자열을 조립하지 않는다([../identifier_model.md](../identifier_model.md)).
- **정책 게이트는 UX 선차단.** CMS user-profile ruleset 인가를 코어가 `Capabilities` 스냅샷으로
  노출하고 앱은 버튼을 숨길 뿐이다. 최종 판정은 서버(403/Floor Deny). 문서를 아직 못 받았으면 게이트를 걸지
  않는다(android_ue_client §7 과 동일). 해석 = `CscClient::fetchUserProfile`·`fetchServiceConfig`(ETag·304 = `notModified`) →
  `UserProfileDoc`(TS 24.484 §8.3.2 — 긴급 대상 EntryType(`entry-info` 모드 + `uri-entry`)·제휴 그룹·N2·ruleset allow-*, **요소가 없으면
  허용**)·`ServiceConfigDoc`(§8.4 — `service-configuration-info` 루트, domain·broadcast-group 계층 수·on-network Resource-Priority
  r-value `mcpttp.15` 형식 — 받으면 앱이 `AccountConfig.rp*` 에 넣는다. 인가 요소는 없다) → `Capabilities::of(up, sc)`(nullptr = 미수신).
  규칙은 코어 한 곳이다 — 긴급 사설콜 = allow-private-call ∧ allow-emergency-private-call(둘 다 user profile). ad hoc 인가는 규격
  `<anyExt><allow-adhoc-group-call>` 과 옛 서버 확장 `<cims:allow-adhoc-group-call>` 을 로컬 이름으로 함께 읽는다.
- **긴급·임박 세션 조건**(TS 24.379 §10.1.1.2.1.3~6, [mcptt_emergency_modes.md](mcptt_emergency_modes.md) §4.2·§4.3). `CallInfo.condition` 이
  그룹의 진행 중 긴급·임박을 이 호에서 본 현재값이다(`mcptt` 는 개시·착신 INVITE 의 값으로 불변). `setCallCondition` = in-dialog
  re-INVITE(multipart mcptt-info 에 **바뀐 지시자만** true/false 명시 + `Resource-Priority` — `AccountConfig.rp*`, 값 정본 = service-config
  OnNetwork *-resource-priority §6.2.8.1.15, 기본 = CSP 의 mcpttp 서열), 보내면서 반영(`Local`) → 2xx `Confirmed` / 4xx~6xx 이전 값 복원
  `Denied`(§6.2.8.1.5 — 미인가 상향 403, 재-INVITE 거절은 호를 끊지 않는다). 서버 재광고(수신 re-INVITE·조인 200 OK 의 mcptt-info) =
  `Advertised` — emergency-ind true 는 임박을 내린다(§10.1.1.2.1.6 1)d)). Floor Request 의 긴급 비트는 현재값을 따른다. 대상 선택·403 뒤
  normal 재발신·경보 정합은 앱 정책이다. 경보 = `sendEmergencyAlert`(MESSAGE mcptt-info `alert-ind`·`mcptt-client-id`(`AccountConfig.mcpttClientId`,
  비면 `urn:uuid:` instanceId)·ICSI 헤더, 제3자 취소 `originated-by`·그룹 긴급 해제 동봉 §12.1.1.2, Request-URI = `AccountConfig.mcpttServerUri` —
  참여 기능 PSI(ue-init-config `MCPTT-Service-Details/Server-URI`, §12.1.1.1 8)), 비면 그룹 URI(CSP 0.2.166 전 서버)) · 수신 `onEmergencyAlert`(§12.1.1.3 —
  `mcptt-calling-group-id` 없으면 `mcptt-request-uri`, 경보 없는 그룹 긴급 통지도 `alertInd 0` 으로).
- **media plane SDS**(TS 24.282 §9.2.3, [mcdata_messaging.md](mcdata_messaging.md) §4.7). `AccountConfig.maxSdsCplaneBytes`(프로비저닝
  `mcdata.maxPayloadSdsCplaneBytes` — `ServiceProfile::toAccount` 가 채운다)를 넘는 **그룹** SDS 는 `sendGroupSds` 가 MSRP 로 보낸다(INVITE
  더미 audio + m=message sendonly actpass → 200 의 cmdp a=path → SEND 2건) — 반환·상관은 C-plane 과 같고 최종 결과가 `onRequestResult`
  method `MSRP` 로 온다. 1:1 은 늘 시그널링 평면(서버 media plane 이 그룹만 받는다). 수신 = `AccountConfig.mcdataMsrp` 가 REGISTER Contact
  `+g.3gpp.icsi-ref` 에 ICSI mcdata.sds 를 합치고(기존 목록에 쉼표로), 서버발 배포 INVITE 는 코어가 받아(m=message active recvonly,
  더미 오디오 inactive) `onSds`(`mediaPlane=true`, 발신자·그룹 = 배포 INVITE 의 mcdata-info)로 낸다. MSRP 호는 앱 호 목록·호 이벤트에
  나오지 않고, 서버 BYE 가 없으면 5 s 뒤 코어가 끊는다. m=message 는 pjsua 가 만든 m=text 슬롯(발신)·포트 0 섹션(수신) 자리에 넣는다
  (미디어 수가 늘면 pjsua `med_prov_cnt` assert).
- **승인 톤 뒤 마이크**(android_ue_client.md «삑 후 말하기»). `EngineConfig.grantMicDelayMs` 만큼 Floor Granted(200 OK 승인 포함) 뒤 마이크
  개방을 미루고, 그 사이 놓거나·회수·시한으로 발언을 잃으면 열지 않는다(톤은 앱이 재생한다). 전이중 사설콜(`mc_no_floor_ctrl`)은 floor 가
  없으므로 `setMuted` 가 앱의 PTT 로컬 게이트다. 호 수신 음량(`setRxLevel`)은 호에 기억되어(`CallInfo.rxLevel`) 오디오가 없거나 재협상으로
  스트림이 바뀌어도 다음 결선에 다시 걸린다.
- **에러 모델.** 명령은 즉시 `Result{ok, reason}` 을 돌려주고(인자·상태 오류), 프로토콜 결과는 이벤트로 온다.
- **망 변경.** 플랫폼은 망 변경(기본 망 전환·끊겼다 복귀)을 `Engine::handleNetworkChange()` 로 **알리기만** 한다. 등록 복구는
  코어가 한다 — TCP/TLS 연결을 닫아(옛 망의 연결을 재사용하지 않게, 새 REGISTER 가 새 연결을 연다) 등록을 켠 계정마다 다시
  등록한다. **앞 등록 트랜잭션이 걸려 있으면 겹쳐 보내지 않는다**(RFC 3261 §10.2) — pjsip 가 `PJSIP_EBUSY` 로 거절하면 표시만
  해 두고, 그 계정의 등록 결과(`onRegState`)가 오면 한 번 더 보낸다(`detail::RegRecovery` — 여러 번 불려도 계정마다 하나).
  일반 등록 경로라 복구 REGISTER 가 실패해도 계정의 자동 재시도(`regConfig.retryIntervalSec`)가 그대로 돈다. UDP 는 닫지
  않는다 — 0.0.0.0 에 묶여 있어 그대로 쓰고, 낡은 Via/Contact 는 rport·Contact 재작성(`natConfig`)이 고친다. pjsua 의 IP 변경
  처리(`Endpoint::handleIpChange`)는 쓰지 않는다 — EBUSY 때 regc 를 부수고 새로 보내 옛 UDP 트랜잭션과 새 REGISTER 가 겹치고,
  IP 변경 모드의 실패는 자동 재시도에서 빠지며, 수신 소켓 재시작 실패 경로가 전송이 빈 regc 를 역참조한다. 진행 중 호는
  건드리지 않는다(§11). `refreshRegistration` 은 망은 그대로인데 등록만 잃은 경우(서버 재기동)의 복구다. Android 는 접점
  `platform.NetworkWatcher` 가 변화를 판정한다(§5.3 — 걸 때의 망을 심어 등록 직후 그 망의 첫 통지는 넘기고, 망 없이 걸었으면
  처음 잡히는 망을 변화로 본다).
- **ABI.** 공개 헤더는 pjsua2 타입을 include 하지 않는다. 구현체는 pImpl.
- **affiliation PUBLISH 의 entity-tag**(RFC 3903). 코어가 EPA 다 — 2xx 의 `SIP-ETag` 를 그룹별로 기억해 다음 `affiliate` 에
  `SIP-If-Match` 로 싣는다. 412 를 받으면 그 ETag 를 버리고(§5 MUST) 같은 요청을 다시 보내지 않으며, `SIP-If-Match` 없는 초기 PUBLISH
  (§4.2)로 한 번 다시 알린다. 앱에는 412 가 올라가지 않고 재발행의 최종 응답이 `affiliate()` 가 돌려준 token 으로 온다(시험
  `AffiliationPublish.StaleEtag412FallsBackToInitialPublish`).
- **일제 통화**(TS 24.379 §4.12, [mcptt_broadcast_group_call.md](mcptt_broadcast_group_call.md) §4.4). `joinGroupCall({broadcast})` 는
  `prearranged` + `<broadcast-ind>true` 로 개시하고 그 단말을 개시자로 둔다 — Floor Request 에 B-bit 를 싣고, Floor Release 뒤
  B-bit Floor Idle 을 받으면 **코어가 호를 해제**한다(TS 24.380 §6.2.4.6.4, 앱 조작 없음). 수신 멤버의 표시는 앱 몫이다
  (`McpttInfo.broadcast`·`FloorEvent.indicator` B-bit·Taken `permission` 0 → `FloorInfo.canRequest=false`).
- **암묵적 발언 요청**(TS 24.380 §14.2.5, [mcptt_broadcast_group_call.md](mcptt_broadcast_group_call.md) R14·U7). `GroupCallOptions.implicitFloorRequest`
  는 개시 INVITE 의 floor SDP 에 `mc_implicit_request`(요청)와 `mc_granted`(200 OK 승인 표시 수용 — 능력, §14.2.4)를 함께 싣고 floor 를 호 성립 전부터
  `Requesting`('U: pending Request', §6.2.4.2.2)으로 둔다. answer 의 `mc_granted` = 승인 → `Speaking`, `mc_implicit_request` 만 = Floor Granted 대기,
  둘 다 없음(진행 중 호 합류 등 — §14.3.5) = 코어가 명시 Floor Request 로 잇는다. 호 성립 전에 `floorRelease` 하면 answer 에서 Floor Release 로
  돌려준다(그 사이 Floor Granted 는 무시). 이어지는 offer(re-INVITE)에는 둘 다 싣지 않는다(§14.5). 누르는 동안 개시하고 말하는 한 버튼 발신용.
- **단말 속성**([mcptt_management_views.md](mcptt_management_views.md) §4.1). `EngineConfig.userAgent` 는
  `userAgentOf(제품, 앱 버전, OS, 모델)` 형식(`CIMS-PTT/1.4.2 (Android 15; SM-S921N)`)으로 앱이 채운다. `userAgentOf` 는 OS·모델을
  comment 규칙(RFC 3261 §25.1)으로 정리한다 — 괄호·역슬래시 제거, 공백·제어 문자 접기, OS 의 `;`(OS·모델 구분자) 제거.
  `AccountConfig.instanceId` = REGISTER Contact `+sip.instance` URN(TS 24.229 §5.1.1.2.1 c) — IMEI 를 아는 단말은 `imeiUrn(imei)`
  (`urn:gsma:imei:TAC-SNR-0` — 셋째 칸은 검사 숫자가 아니라 spare 라 항상 0, RFC 7254 §4.2.3·TS 23.003 §13.8. 입력의 Luhn 은 검증),
  모르면 기기(설치) 고유 `urn:uuid:…`. 모든 transport 에서 REGISTER Contact 파라미터(`regConfig.contactParams`)로 싣고 pjsua outbound
  (RFC 5626)는 끈다 — CSP 는 outbound 를 지원하지 않아 pjsua 가 OUTBOUND_NA 로 두고, 그러면 NAT 로 Contact 를 다시 쓸 때(TCP/TLS
  재연결) outbound 경로의 instance 가 빠진다. 비우면 pjsip 기본값(호스트명 해시 — 기기마다 같을 수 있다, [registration_binding_set.md](registration_binding_set.md) §8).
  두 헬퍼는 C API `cimsue_user_agent_of`·`cimsue_imei_urn`, .NET `Engine.UserAgentOf`·`ImeiUrn` 으로도 노출된다(규칙은 코어 하나).
  Windows 기기 값(OS 판·BIOS 모델·`MachineGuid` 이름 기반 `urn:uuid:` — Android 와 같은 `cims-ue:` 규칙)은 .NET `Platform.DeviceIdentity` 가 모은다.

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
`http::ITransport{request(method, url, headers, body) → Response}` 로 추상한다. 구현은 OpenSSL 하나
(`OpenSslTransport` — pjproject 가 이미 OpenSSL 을 링크하므로 추가 의존이 없다). TLS 트러스트(사설 CA)는
`EngineConfig.trustAnchors`·`CscEndpoint.caPem` 으로 코어에 넘기고 SIP TLS 와 HTTPS 가 같은 앵커를 쓴다.
`EngineConfig.udpNoTcpSwitch`(기본 false)는 pjsip 의 UDP→TCP 승격(RFC 3261 §18.1.1)을 끄는 사이트 옵션 — libInit 전에
`pjsip_cfg()->endpt.disable_tcp_switch` 로 반영한다(sip_tls_signaling.md §3.2a). Kotlin 파사드 `EngineConfig.udpNoTcpSwitch` 동일. C API·.NET 파사드는 미노출(기본값).

**주입은 아직 열려 있지 않다.** 인터페이스가 내부 헤더(`sdk/core/src/http/https_client.h`)에 있고 C API 에도
진입점이 없어(`cimsue_csc_create` 는 endpoint 만 받는다) Windows `.NET`·Android SWIG 어느 쪽도 구현체를
전달하지 못한다. 인증서 정책은 위 신뢰 앵커로 해결되므로 당장 막히는 것은 **프록시 경유**뿐이다. 열려면
인터페이스를 공개 헤더로 올리고 세 바인딩(SWIG director·C API·.NET)에 같이 내야 한다(§11).

### 4.5 미디어 경계

- **장치.** 코어는 pjmedia 장치 id 와 라우트 의미(`earpiece/speaker/headset/bluetooth/extra`)만 다룬다. Android
  의 AudioManager 모드·포커스·블루투스 SCO, Windows 의 WASAPI 엔드포인트 선택은 플랫폼 SDK 몫이다.
- **SSRC 소스.** U10 디먹스가 만든 서브스트림을 코어가 `MediaSources[]` 로 노출한다. 감청 leg 는 RFC 5576
  `a=ssrc … label` 을 파싱해 각 소스에 발신자/착신자 라벨을 붙인다. 믹싱은 pjmedia 안에서 끝나고(브리지 포트 1개)
  앱은 소스별 활성·레벨 표시만 한다(dispatch_center §5.4).
- **영상.** 키프레임 요청은 RTCP PLI 만 쓴다(호 설정 `reqKeyframeMethod` — TS 26.114 §7.3 영상 코덱 제어 = RTCP AVPF PLI/FIR).
  pjsua 기본값의 SIP INFO(RFC 5168 `media_control+xml`)는 서버가 INFO 를 Allow 에 두지 않아 501 로 끝나므로 보내지 않는다 —
  발신·응답·재개(re-INVITE) 설정이 호 설정을 대신하므로 셋 모두에 싣는다. 코어는 창을 열지 않는다. Android 는 Surface 를 받아 pjmedia 렌더러가 직접 그린다 — `Engine::setVideoWindow(void*)`
  (파사드 `CimsUe.setVideoSurface(Surface?)`, SWIG typemap 이 `ANativeWindow_fromSurface` 로 참조 하나를 코어에 넘기고 코어가 결선마다
  렌더러 몫을 따로 잡는다 — 렌더러는 교체·스트림 소멸 때 자기 참조를 푼다). 수신 창은 디코딩 스트림이 렌더러를 만든 뒤에만
  결선·해제한다(`win_in` 무효면 건너뛴다 — pjsua 창 함수가 무효 id 를 단정으로 막아 프로세스가 abort 한다). 영상이 활성되는 호마다 수신 창을 결선하고, 계정
  `videoAutoTransmit` 면 카메라 송신을 연다(START_TRANSMIT, 송신 방향이 없으면 sendrecv 로). 셀프뷰는 카메라를 두 번 열지 않고 엔진
  캡처가 연 Camera2 세션에 출력 Surface 를 더한다(파사드 `setPreviewSurface` → CIMS 패치 `PjCamera2.SetPreviewSurface`). 카메라 전환
  `switchCamera(callId)`(합성 장치 Colorbar 제외, 기본 = 이름에 front), H.264 최우선·인코딩 480x640·15 fps·400/500 kbit/s. 카메라 열거는
  기동 때 한 번이라 `CimsUe.start(cfg, context)` 가 **기동 전에** `PjCameraInfo2.SetCameraManager` 를 넣는다. 코어 제어 스레드(`ue-ctl`)는
  네이티브 스레드라 `FindClass` 가 APK 의 `org.pjsip.PjCamera2` 를 못 찾으므로, pjlib 이 `JNI_OnLoad` 에서 앱 클래스 로더를 기억하고
  영상 장치가 `pj_jni_find_class` 로 찾는다(CIMS 패치). 카메라 도우미 두 파일은 pj 를 싣는 모듈마다 빌드 때 복사한다(S1-UE-ENGINE-SINGLE).
  프레임 콜백(`onVideoFrame`)은 창 없는 렌더 장치 패치가 필요해 후속(§11). 감청 영상 격자 합성은 UI 몫.
- **캡처.** 카메라·마이크 권한과 장치 열기는 플랫폼 SDK 가 하고, 코어는 `setCaptureEnabled` 로 on/off 만 한다 — false = pjsua
  `SPEAKER_ONLY`(캡처 스트림을 열지 않고 재생만, OS 동시 캡처 중재에서 빠진다 — 앱 간 마이크 양보·PTT 유휴), `NO_IMMEDIATE_OPEN`
  동반이라 장치가 닫혀 있으면 모드만 두고, 모드는 장치 선택을 넘어 유지된다. 헤드리스(null 장치)는 상태만 둔다.
- **장치 단 음량·라우트**([ue_audio_level.md](ue_audio_level.md) §2·§6). `setDeviceAudioLevels(speaker, micTargetDbov)` = 스피커 배율
  (slot 0 `adjustRxLevel`) + 마이크 AGC 목표(마이크 배율 1 고정) — 코어가 값을 기억해 게이트 전환·재오픈·호 결선 뒤 다시 건다.
  `setAudioRoute(output, input)` = pjmedia OUTPUT_ROUTE + INPUT_ROUTE(keep, 입력 `Earpiece` = 내장 기본 마이크 고정 — 반이중 무전을
  단말 스피커·수화기로 들을 때). `reopenAudioDevice()` = 장치가 열려 있으면 닫고 곧바로 다시 연다(같은 장치·모드면 pjsua 가 "No changes"
  로 돌아가므로 모드에서 NO_IMMEDIATE_OPEN 만 빼 값이 달라지게 한다) — 출력 장치 소멸 뒤 재생 트랙 뮤트가 남는 단말 대응.
  호별 듣는 크기는 `setRxLevel`(통화 포트 `adjustTxLevel`).

### 4.6 floor 코덱 단일 정의

[mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md) §6 의 결론. 정본 테이블은
`docs/design/features/mcptt_floor_defs.yaml`(opcode·field id·indicator 비트·source/permission/queued purpose·cause 문구)
이고 `scripts/gen_floor_defs.py` 가 코어 헤더 `sdk/core/src/floor/floor_defs.h` 를 생성한다(CMake 가 테이블 변경 시 재생성).
`gen_floor_defs.py --check` 는 생성물 최신성과 `cmp/PMcpttGroup.h`·android `FloorControl.kt`·.NET 파사드 `FloorIndicator`(`Types.cs`)·
`scripts/mcptt_floor_policy_probe.py` 의 상수를 테이블과 대조한다(CMP·Kotlin·.NET 은 생성물이 아니라 대조 대상 — 값이
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
cimsue-cli [계정] group-call <groupId> [--duration S] [--ptt-at S --ptt-len S] [--listen-only] [--emergency] [--broadcast] [--implicit]
                  [--upgrade-at S] [--cancel-at S]   # 진행 중 긴급 상향·하향 re-INVITE — outcome conditions[{cause,emergency,code}]
cimsue-cli [계정] alert <groupId> [--cancel] [--originated-by ID] [--cancel-group-emergency]   # 긴급 경보 MESSAGE
cimsue-cli [계정] sds <groupId> <text>           # 최종 응답까지 — [계정] --cplane-max N 을 넘으면 MSRP(outcome plane=media)
                                                 #   ([계정] --msrp = 서버발 MSRP 배포 수신 광고 — sds-recv 가 plane=media 로 받는다)
cimsue-cli [계정] sds-recv [--duration S]        # 수신 SDS 를 JSON 줄로
계정: --server IP --port N --transport udp|tcp|tls --domain D --msisdn M (--imsi I|--auth-id IMPI)
      (--ha1 HEX32|--password P) [--mcptt-id tel:..] [--affiliate G,..] [--srtp off|optional|required] [--sec tls]
      [--tls-ca PEM] [--json]
```

결과는 stdout 에 JSON 한 줄(`outcome`·`rx_pkts`·`tx_pkts`·`granted`·`taken`·`denied`·`code`), 종료코드 0/2/3/4/5/6/7
(성공/인자/등록/호/미디어 없음/floor 미획득/SDS 실패). `--affiliate` 는 시작 시 PUBLISH(Event: mcptt), 종료 시 de-affiliate.
```
cimsue-cli [계정] dialog-watch <aor> [--duration S]   # RFC 4235 구독 → dialog-info 를 JSON 줄로
cimsue-cli [계정] join <aor> [--duration S]           # 감시 → confirmed dialog 에 INVITE-Join(recvonly) → 수신 RTP·SSRC 라벨
cimsue-cli [계정] pickup [번호] --code <피처코드>      # 그룹/지정 픽업
cimsue-cli [계정] transfer <peer> --to <target>       # peer 와 통화 후 REFER blind (answer --transfer-to 는 착신측 전달)
cimsue-cli --csc-host H --user U --pw P [--no-tls-verify] login          # PKCE 로그인 + /provisioning/me 요약(dispatch members/ptt_targets 포함 — 서버 P2 반영 확인용)
cimsue-cli --csc-host H --user U --pw P --from-profile volte|ptt [--server IP --port N] <command>   # 프로파일로 계정 채움
```

오디오 장치는 null(헤드리스) — 브리지는 돌고 RTP 는 흐른다. 통계는 스트림 소멸 시점(`onStreamDestroyed`)에
보존해 상대가 먼저 끊어도 남는다(`StreamStats.rxJitterUs` = pjmedia rtcp rxStat jitter 평균). 사용 예는 [VERIFICATION_MANUAL.md](../../VERIFICATION_MANUAL.md) 부록.

**구동 모드 `drive`** — 계측기 `real-ue` 풀([test_instrument.md §3.3](test_instrument.md))이 프로세스를 가상 단말처럼 단계별로 구동하는 접점. 엔진을 띄운 채
(등록은 자동으로 하지 않는다) stdin 한 줄 = 명령 하나(공백 토큰), stdout 한 줄 = JSON 이벤트 하나. 명령마다 동기 결과 `result{op,ok,call,code,reason}` 하나를 내고
(dial/group_call/pickup 은 `call` id), 진행은 이벤트로 온다. 시각은 프로세스 안에서 잔다(`rrd_ms`·`srd_ms`·`sdd_ms`·floor `t_us`). 해석기는 코어의
`DriveSession`(공개 `cimsue/drive.h` — Engine 관찰자로 이벤트를 받는다)이고 명령·이벤트 정의도 그 헤더 하나다 — 앱 시험 모드의 계측 링크(`DeviceLink`,
[ue_voice_quality.md §5](ue_voice_quality.md))가 같은 해석기를 TLS 소켓으로 쓴다.
```
cimsue-cli [계정] drive [--sample-file WAV] [--service volte|voip|ptt]
  명령: register | unregister | use <service> | dial <번호|URI> [video] | answer <call> [video] | reject <call> [code] | hangup <call> | hold <call>
        resume <call> | dtmf <call> <digits> | transfer <call> <대상> | group_call <group> [listen] [emergency] [broadcast] [implicit] | floor_request <call>
        floor_release <call> | affiliate <group> on|off | pickup <code> [number] | media mic|sample [<wav>] | stats [call] | quality <call> | quit
  이벤트: ready{version,aor} · reg{service,state,code,reason,expires,rrd_ms} · incoming{call,from,called,video,mcptt,group}
        · call{call,dir,state outgoing|incoming|active|held|disconnected,code,reason,media,mcptt,video,by_us,group,srd_ms|sdd_ms,(disconnected: 통계 + 품질)}
        · floor{call,kind,subtype(TS 24.380 §8.2),t_us,cause,queue_position,duration} · request{method,op,on,code,reason,ms,token}(affiliate PUBLISH)
        · stats{call,통계 + 품질}(활성 호마다 1 초) · quality{call,kind:callTerm|snapshot,품질} · roster · dialog · sds · engine_stopped · exit
  통계 = rx_pkts·tx_pkts·rx_loss·rx_bytes·jitter_us·stats_valid
  품질(call disconnected·stats·quality·명령형 결과 JSON) = codec·discard·loss_pct·discard_pct·jitter_max_ms·remote_loss_pct·remote_jitter_ms·rtd_ms·esd_ms·
        one_way_ms·r_lq·r_cq·mos_lq·mos_cq — Engine::callQuality(ue_voice_quality.md §3), 값 없음 = -1
cimsue-cli [계정] link HOST[:PORT] [--pair-key K] [--link-ca PEM | --link-pin FILE] [--sample-file WAV] [--service S] [--duration S]
  등록 뒤 계측기 워커(Device.Port 7120)에 TLS 로 붙어 hello → 워커 명령을 같은 해석기로 실행(register 는 app_owned 거절). stdout = link{state,detail}
```
MCPTT 착신은 코어가 자동응답(`autoAnswerMcptt`)하므로 `incoming{mcptt:true}` 뒤 `call{dir:in,state:active}` 가 합류 신호다. `disconnected` 이벤트는 그 호의
최종 통계를 함께 싣는다(우리가 끊었으면 `by_us` + `sdd_ms`). 1xx 는 이벤트로 내지 않는다(코어 `onCallState` 는 상태 전이만).

---

## 5. Android SDK (`sdk/android`)

### 5.1 구성

```
sdk/android/
  cimsue/                  Gradle Android Library → cimsue AAR (앱이 쓰는 유일한 공개면)
    src/main/jniLibs/      arm64-v8a/{libcimsue,libc++_shared}.so  (빌드 산출물, 커밋 안 함)
    src/swig/java/         SWIG 생성 Java (com.cims.ue.sdk.jni.*) — 손코드와 소스셋 분리
    src/main/java/com/cims/ue/sdk/
      CimsUe.kt            Kotlin 파사드: 상태 → StateFlow, 이벤트 → SharedFlow/Channel, 명령 → suspend
      CscClient.kt         IdMS PKCE · XCAP(GMS/CMS) · 프로비저닝 · 범용 요청(관리·이력·녹취)
      Types.kt             값 컨테이너(CallInfo·FloorInfo·Profile …)
      TrustAnchors.kt      APK 동봉 루트 CA (Android 에는 OpenSSL 기본 인증서 경로가 없다)
      platform/            Android 접점: AudioRouter(모드·포커스·SCO) · UeForegroundService · SecureStore(Keystore)
  cimsue-engine/           pjsua2 전용 모듈 — org.pjsip.** + libpjsua2.so 의 **유일한 제공처**
  CMakeLists.txt           ext/pjproject 빌드 변수를 읽어 코어+SWIG 을 NDK 로 빌드
  build-native.sh          위를 실행하고 산출물을 AAR 모듈에 배치(`--no-install` 은 배치 생략 — 빌드 확인용)
```

- **엔진은 한 벌이다.** `org.pjsip.**` 과 `libpjsua2.so` 는 `:cimsue-engine` 만 낸다. 예전에
  `android/core/src/pjsua2` 에 커밋돼 있던 생성물은 폐기했다 — 두 벌이면 `ext/pjproject` 패치를 두 곳에
  반영해야 하고 커밋본이 조용히 어긋난다. `S1-UE-ENGINE-SINGLE` 이 이를 정적으로 못박는다.

- **바인딩은 SWIG.** pjsua2 가 이미 SWIG 을 쓰므로 코어도 `cimsue.i` 한 파일로 Java 를 생성한다. 이벤트
  리스너는 director. 손 JNI 는 두지 않는다. 타입맵 본문의 플랫폼 분기는 `%#if`/`%#else`/`%#endif` 로 쓴다 — 맨 `#if` 는
  SWIG 전처리기가 생성 때(호스트) 먼저 평가해 한 갈래만 남긴다(`S1-UE-ANDROID-BIND` 가 잡는다).
- **파사드가 유일한 공개면.** 앱은 `com.cims.ue.sdk.*` 만 import 한다. `com.cims.ue.sdk.jni.*` 와 `org.pjsip.*`
  는 파사드 내부다.
- **Android 접점의 책임.** 오디오 포커스·모드·라우팅(무전/통화 분리 출력 포함), 카메라, Foreground Service
  (등록 유지·wakelock·알림), HW PTT 키 이벤트 전달, Keystore 저장, SSO AccountManager·`/provisioning/me` 캐시,
  부팅 재등록. 이들은 프로토콜을 모른다.

### 5.2 배포

AAR 하나(arm64-v8a). 버전은 코어 버전 + Android 패치 번호(`1.4.0-android.2`). GPL 트랙(pjproject)은
android_ue_client §13 그대로.

### 5.3 기존 `android/` 와의 관계

`android/core` 는 Android Library 이면서 자체 pjsua2 래퍼(`sip/`)·프로비저닝·계정·연락처 저장을 한 모듈에 담고 있고,
`ptt-client` 가 floor·mcdata·csc·mcptt 사본을 따로 가진다. 최종 상태는 다음과 같다.

| 지금 | 최종 |
|---|---|
| `android/core/sip/*` — 자체 pjsua2 래퍼(SipController·CimsCall·CimsAccount·CimsEndpoint·PjLib·CodecConfig) | 없어진다. 앱은 `:cimsue` 파사드(`CimsUe`·`Account`·`Call`)를 쓴다. 이행 기간에는 `:core-sip` 로 떼어 아직 옮기지 않은 앱만 쓴다 |
| `android/core` 나머지(account·provision·config·contacts·calllog·message·device·net·power·boot·ui·`CimsSuite`) | `:core` 에 남는다 — SIP·엔진 의존 없음. 로그인 앱 `android/cims` 도 이것만 쓴다(`libpjsua2.so` 를 싣지 않는다) |
| `android/ptt-client/{floor,mcdata,csc,mcptt}` | 없다 — 코어 `floor/mcdata/csc/mcptt`(P3). 화면 모델만 앱에 남는다(`csc/CscModels.kt` — SDK 값의 투영). floor 비트는 파사드 `FloorIndicator`(생성기 `gen_floor_defs.py --check` 가 대조), CSC 대조(`S1-UE-CSC-XCHECK`)는 `:core` 프로비저닝 경로만 본다 |
| `android/ptt-client/PttController.kt` | 규격 절차는 코어로, 앱 정책(affiliation 목표 집합·재시도·채널 복원·듣기 정책·긴급 대상 선택)은 **앱 세션 층**으로 — 관제 태블릿 `DispatchSession` + 평면 확장(`PttPlane` 등)과 같은 구성, 화면 상태는 ViewModel |
| `volte-client`·`ptt-client` | `implementation(project(":cimsue"))` |
| `android/dispatch-tablet` | 관제조작반 태블릿 앱 (§7) — **구현 완료**, 정본 [android_dispatch_tablet.md](android_dispatch_tablet.md) |

- **한 프로세스에 엔진 하나.** `:core`(동적 `libpjsua2.so`)와 `:cimsue`(pj 정적 링크 `libcimsue.so`)는 같은 pjsip 기호를 각자 내므로
  한 앱이 둘을 같이 쓰지 않는다([android_dispatch_tablet.md](android_dispatch_tablet.md) §2.2). 옮기는 앱은 `:core-sip` 을 끊고 `:cimsue` 만 쓴다.
  앱끼리는 프로세스가 달라 이행 기간에 엔진이 달라도 되고, 앱 간 마이크·라우트 양보(`CimsSuite` 브로드캐스트)는 엔진과 무관하다.
- **코어 `domain/` 은 두지 않는다.** 관제 태블릿(Kotlin)·Windows(.NET) 앱이 이미 각자 세션 층을 가진다 — C++ 도메인 층을 두면 둘 다
  다시 짜야 한다. 나누는 선은 **규격이 정한 절차(메시지·상태머신·응답 해석)는 코어, 무엇을 언제 다시 할지(정책)는 앱**이다.
- **이행 단계** — 각 단계는 기존 앱이 그대로 동작하는 것을 완료 조건으로 한다. 공존 기간의 코드 중복은 대조 검사가 드리프트를 막는다.

| 단계 | 내용 | 완료 조건 |
|---|---|---|
| P0a 코어·파사드 보강(VoLTE 몫) | 파사드 기본값 코어와 일치(`noVad`) · affiliation 412(§4.2) · FD·floor 이벤트 종류·`userAgentOf`/`imeiUrn` 파사드 · 프로파일 `udpNoTcpSwitch` · 마이크 게이트(§4.5 `setCaptureEnabled`) · 영상(수신 Surface·셀프뷰·카메라 전환·H.264 설정·`PjCamera` 클래스를 `:cimsue` 로) · 망 변경 재등록(Android 접점) | `S1-UE-*` PASS · 관제 태블릿 회귀 · `cimsue-cli` S3(등록·1:1·영상·SRTP) |
| P1 `:core` 분리 | `:core`(비 SIP) / `:core-sip`(자체 래퍼, 이행용) — 앱 코드 무변경 | APK 빌드·동작 불변, 로그인 앱에서 `libpjsua2.so` 빠짐 |
| P2 volte-client 전환 | 세션 어댑터 `VoltePhone` 이 기존 래퍼 계약(등록·호 상태 StateFlow·호 명령·영상·캡처 게이트·MESSAGE)을 SDK 로 낸다 — `SipService`(FGS·오디오 모드·라우팅 협조·알림)와 화면은 그대로, 호 상태는 기존과 같은 마지막 호 이벤트 투영, 망 변경은 `NetworkWatcher`. 모듈 `:cimsue` + `:core` | 사내 단말 실기: UDP/TCP/TLS 등록·음성·영상·SRTP·SMS·망 전환·PTT 양보 |
| P0b 코어 보강(PTT 몫) | MSRP 미디어평면 SDS(TS 24.282, RFC 4975) · 긴급 re-INVITE 상향/하향·긴급 재광고 수신·403 긴급 거부·경보(alert-ind) 빌더/파서(TS 24.379) · 승인 톤 뒤 마이크 · 장치 게인(호 수신 음량 기억 — AGC 목표 환산은 앱, [ue_audio_level.md](ue_audio_level.md) §6) · CMS user-profile/service-config 해석 — 코어·Android 파사드 반영(§4.2 규약), C API·.NET 은 Windows 몫 | 단위시험(`mcptt_condition_test`·`msrp_test`·`csc_test` CmsDoc·`floor_participant_test`) + `cimsue-cli` S3(긴급·MSRP) |
| P3 ptt-client 전환 | `PttController` = SDK 세션 + 평면 넷(`PttGroups` 참여·로스터·affiliation·CSC 문서 · `PttFloor` · `PttMessaging` SDS·FD·MSRP · `PttEmergency` SOS·경보·조건), Kotlin 프로토콜 사본(floor·mcdata·msrp·mcptt XML·CSC) 제거·대조 검사 정리(`S1-UE-CSC-XCHECK` = `:core` 프로비저닝만). ViewModel 분리는 후속 | 사내 단말 실기(MF52·W999, 상대 = 계측기 013, g005 · 긴급 = 프로파일 대상 g002): 그룹콜·발언권 인계·HW PTT·화면 꺼짐 착신·긴급 확정/거절·경보·일제 통화·SDS/MSRP 송수신 — FD·깊은 Doze·VoLTE 양보·이어폰 분리는 남음([sdk_port_handoff.md](../../dev/sdk_port_handoff.md) §5) |
| P4 시험 모드(Q4) | `android/core` 공통 진입·설정·링크 서비스·오버레이 → 앱 3종([ue_voice_quality.md](ue_voice_quality.md) §4) | 계측기 실기기 링크·`VOLTE-CALL-DEVICE-*` |
| P5 정리 | `:core-sip` 삭제 · `:cimsue-engine` 존치 결정 · 문서 | `S1` 전체 PASS |

협력업체 단말도 이 앱을 받으므로 전환한 APK 의 협력업체 배포는 단계마다 따로 승인을 받는다.

---

## 6. Windows SDK (`sdk/windows`)

```
sdk/windows/
  CMakeLists.txt           슈퍼빌드 — AMR-WB(deps) → pjproject(자체 CMake) → sdk/core(CIMSUE_SHARED) → sdk/{bin,lib,include}
  deps/{opencore-amrwb,vo-amrwbenc}/   ext/ 소스의 MSVC CMake 래퍼 (upstream 은 autotools 만)
  dotnet/CimsUe/           .NET 파사드(C# 클래스 라이브러리, P/Invoke → cimsue_c) + Windows 접점(관리 코드: 엔드포인트 열거·핫플러그 통지 ·
                           전역 핫키(PTT·응답) · 단일 인스턴스 · 자격 저장(DPAPI) · 자동 시작) + UI 스레드 마샬링
산출물: cimsue.dll + cimsue.lib + include/cimsue/*.h (= sdk/core/include 복사, C++ + cimsue_c.h) + cimsue-cli.exe + CimsUe.dll(.NET)
```

### 6.1 결정

| 항목 | 결정 | 근거 |
|---|---|---|
| 엔진 빌드 | `ext/pjproject` 의 **자체 CMake**(`pjlib` WIN32 분기·WMME/WASAPI/DSHOW 옵션 보유)를 슈퍼빌드가 ExternalProject 로 돈다. `pjproject-vs14.sln` 은 폴백 | 한 빌드 시스템으로 의존성·엔진·코어를 잇고, Linux ExternalProject 와 같은 config_site 생성 방식을 쓴다. vs14.sln 은 AMR·OpenSSL 경로를 손으로 꿰어야 한다 |
| config_site | `sdk/engine/config_site/windows.h` 한 줄 include (Linux 와 동일 규약) | §3 |
| 오디오 백엔드 | **WMME**. 2.16 의 `wasapi_dev.cpp` 는 UWP/Windows Phone 전용(`phoneaudioclient.h`·`Windows::Phone::Media::Devices`, vcxproj 도 `WinDesktop` 제외)이라 데스크톱에서 컴파일되지 않는다 | 실측 지연·핫플러그 문제가 있을 때 데스크톱 WASAPI 백엔드는 §11 과제 |
| 이중 출력(헤드셋+스피커) | 코어 **재생 라우트** API — `addPlaybackRoute(dev)` 가 두 번째 재생 장치를 **재생 전용** `ExtraAudioDevice` 로 브리지에 열고, `setCallRoute(callId, route)` 로 호별 sink 를 고른다. 마이크는 기본 캡처 장치 하나 | pjsua2 `ExtraAudioDevice` 는 원래 캡처+재생을 함께 여는데 두 번째 장치의 마이크는 필요 없고 열면 장치 점유·에코 위험 → 엔진 패치(`recDev == PJMEDIA_AUD_INVALID_DEV` → `PJMEDIA_DIR_PLAYBACK`). 플랫폼 공통 API 라 Android 에서도 무전/통화 분리 출력에 쓸 수 있다 |
| 코어 배포 형태 | **DLL**(`CIMSUE_SHARED`, `cimsue/export.h` 의 `CIMSUE_API` — Engine·Listener·CscClient·toString) + 같은 DLL 이 **C API `cimsue_c.h`** 를 export. pj 라이브러리는 DLL 안에 정적 링크 | C++ 클래스 export 는 같은 MSVC·CRT 전제라 같은 빌드의 `cimsue-cli`·단위시험 전용. 앱(.NET) 은 C API 만 본다 — ABI 가 툴체인·CRT 에 묶이지 않고 P/Invoke 가 그대로 붙는다(§6.4) |
| 영상 | F1·F2 는 음성만(`PJMEDIA_HAS_VIDEO 0`). F3 에서 OpenH264 + DSHOW 캡처 + **CIMS 콜백 렌더 장치**(pjmedia-videodev 패치: 디코드 프레임 → `onVideoFrame`) | 관제 요구(감청·PTT 청취·BLF·픽업·전달)는 전부 음성. pjproject 에 "창 없는 프레임 콜백" 렌더러가 없어 패치가 필요 — 감청 영상 격자(§4.5)는 UI 합성 |
| 인증 | Digest+TLS 만 — `PJSIP_HAS_DIGEST_AKA_AUTH 0` | 관제 소프트폰 가입자 규약(volte_supplementary_services §2)이 USIM 없는 Digest. pjproject CMake third_party 에 milenage 가 없어 켜면 링크 실패 |
| OpenSSL | 외부(vcpkg `openssl` 또는 `CMAKE_PREFIX_PATH`) — SIP TLS·SRTP·코어 HTTPS 가 한 OpenSSL. 런타임 DLL(`libcrypto-3-x64`·`libssl-3-x64`)은 vcpkg applocal 이 실행 파일 옆에 두고 `sdk/bin` 이 함께 담는다 | 레포 vendoring 대상이 아님(서버도 시스템 libssl) |
| CRT | 전 구간 **/MD**(`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL` 을 슈퍼빌드가 명시해 ExternalProject 에 전달) | vcpkg `x64-windows` 트리플릿·.NET 호스트가 동적 CRT. 비워 두면 ExternalProject 쪽이 cl 기본(/MT)이 돼 LNK2038 |
| 엔진 빌드 확정 | pjproject CMake 가 MSVC 에서 그대로 통과(폴백 vs14.sln 불필요). 정적 lib 설치 경로 `bin/`(third_party `bin/pjproject/third_party`), `config_site_sample.h` 는 설치 FILE_SET 에 없어 슈퍼빌드가 보충, pjmedia-codec 의 AMR 자동 링크 pragma(`PJMEDIA_AUTO_LINK_OPENCORE_AMR_LIBS`)는 config_site 에서 끔 | `sdk/windows/CMakeLists.txt` 서두 |
| UI 스택 | **.NET(C#) + WPF** (`net10.0-windows`, LTS). 앱은 `CimsUe.dll`(.NET 파사드) 만 참조하고 네이티브 `cimsue.dll` 은 파사드가 P/Invoke 로 감싼다 | Android 와 같은 구조(네이티브 코어 ↔ 바인딩 ↔ 언어 파사드 ↔ 앱). WPF 는 다중 모니터·고밀도 보드·MVVM 데이터바인딩 관용구가 성숙하고 .NET 데스크톱 접점(CoreAudio COM interop·`RegisterHotKey`·DPAPI `ProtectedData`·명명 Mutex) 이 관리 코드로 닿는다. SDK 는 UI 프레임워크를 모른다 — 파사드는 WPF 를 참조하지 않는다(`SynchronizationContext` 로만 마샬링) |

### 6.2 코어 이식성 (Linux 에서 선행 반영)

코어는 pjlib 추상(`pj_sock_*`·`pj_thread_register`) 위에 있어 대부분 그대로 컴파일된다. 플랫폼 분기는 다음 네 곳이 전부다.

- `src/net/tls_stream.cpp` — BSD 소켓/winsock 차이를 소켓 층(`sock_t`·`closeSock`·`setTimeout`·`WSAStartup`·poll)에서만 흡수. 그 위 TLS·HTTP(`src/http/https_client.cpp`)·계측 링크는 공통.
- `cli/main.cpp` — SIGSEGV 백트레이스(glibc `execinfo`)는 `#ifndef _WIN32`.
- 64비트 정수 — `SdsMessage.timeSec/fileSize`·SDS 5옥텟 시각·요청 token(`affiliate`/`sendRequest` 반환, `RequestResult.token`)은
  `int64_t`(Windows `long` 은 32비트 — `long` 은 공개 헤더에 두지 않는다). floor 소켓 핸들은 `intptr_t`(Win64 `SOCKET`).
- `sdk/core/CMakeLists.txt` — `WIN32` 면 `PJ_WIN32` 정의·`ws2_32 winmm ole32 iphlpapi crypt32` 시스템 라이브러리·`-rdynamic`/`pthread` 생략, `PJ_LIBS` 를 슈퍼빌드가 `-D` 로 준다(config.guess 접미는 autoconf 전용).

### 6.3 관제석 오디오 배치

```
기본 재생 장치(라우트 0) = 헤드셋      ← 통화·감청 Join·1:1
추가 재생 라우트(≥1)   = 데스크 스피커 ← PTT 그룹콜 청취 채널  (Engine::setCallRoute(callId, route))
기본 캡처 장치         = 헤드셋 마이크  (하나만 — floor Granted 에서만 결선)
```

캡처 장치는 하나지만 결선은 **호마다**다 — MCPTT 세션은 각자 floor participant 와 `micOpen` 을 들고 `wireMedia` 가 그 호의
`mic → call` 만 잇고 끊는다(단일 발언 호 가정 없음, bridge 가 한 원천을 여러 sink 로 보낸다). 그래서 관제석의 다중 채널 동시 발언은
별도 코어 API 없이 앱이 대상 세션마다 `floorRequest` 하는 **단말 팬아웃**이다(dispatch_desktop_ui.md §4.1 — 요청한 세션만 해제하는 불변은 앱 몫).

앱은 `audioDevices()` 로 고른 장치 id 를 `setAudioDevices`/`addPlaybackRoute` 에 넘기고, 장치 핫플러그는 .NET 파사드의
`AudioEndpoints`(`IMMNotificationClient` COM interop) 가 감지해 `refreshAudioDevices()` 를 부른다. 두 장치 동시 출력의 지연·에코는
실기 검증 항목(§9·§11).

### 6.4 C API 와 .NET 파사드

Android 의 SWIG Java 바인딩 ↔ Kotlin 파사드 에 대응하는 Windows 의 두 층. C++ 공개 헤더(§4.2)가 바인딩 정본이라는 규칙은 같다 —
C API 는 그 헤더를 **손으로 1:1 평탄화**한 것이며(SWIG 는 C# 대상도 지원하지만 콜백·문자열·수명 규칙을 P/Invoke 관용구로
직접 고정하는 편이 관제 앱 한 곳에는 더 얇다), 새 C++ API 는 C API·파사드에 같은 변경에서 반영한다.
서버 인증서 만료 관측은 이 규약의 예다 — `Engine::tlsPeerExpiry()`(pjsua2 `onTransportState` 의 remote 인증서)·
`CscClient::tlsPeerExpiry()`(OpenSSL 전송의 peer 인증서) → `cimsue_engine_tls_peer_expiry`/`cimsue_csc_tls_peer_expiry`
(`cimsue_tls_peer_expiry_t{valid, not_after_epoch, observed_epoch, days_left, subject, remote}`) → .NET `TlsPeerExpiry`.
Android 앱은 아직 이 코어 위로 이행하지 않아(pjsua2 직접) 같은 규칙을 Kotlin 으로 둔다 — `core` 의 `CimsEndpoint.onTransportState`
·`CimsTls` OkHttp 인터셉터 → `TlsPeerObserver`/`TlsPeerExpiry`(임계 30/7·`worst`). 이행 시 이 자리가 `Engine::tlsPeerExpiry()` 바인딩으로 바뀐다.
관제조작반은 잔여 ≤ 30일이면 요약 띠 경고([sip_tls_signaling.md §8.6.2](sip_tls_signaling.md)).

| 층 | 위치 | 규칙 |
|---|---|---|
| C API `cimsue_c.h` | `sdk/core/include/cimsue/cimsue_c.h`, 구현 `sdk/core/src/c_api.cpp` — `cimsue.dll` 이 export (`CIMSUE_API` + `extern "C"`, x64 `__cdecl`). 프로토콜 로직이 없는 평탄화 층 — 타입 변환과 수명 규약만 둔다 | 불투명 핸들(`cimsue_engine_t*`·`cimsue_csc_t*`), 계정·호·라우트는 코어와 같은 정수 id. 명령은 동기 `cimsue_status_t`(0=성공, 그 외 = C++ `Result::code` 그대로 — 음수 코어·양수 pjsua/HTTP), 사유는 스레드별 `cimsue_last_error()`; id 반환 함수는 -1 이 실패. 상태·이벤트는 **콜백 구조체 한 벌**(`cimsue_listener_t` — `Listener` 가상함수 1:1, `void* user`, NULL 은 무시; `start()` 가 복사하고 기동 중에는 교체하지 않는다) 로 코어 **이벤트 스레드**에서 호출. 문자열은 UTF-8 `const char*` — 코어 소유 문자열·배열은 콜백 인자면 그 콜백 동안, 조회(getter) 산출이면 같은 스레드의 다음 조회까지(스레드별 스냅샷; CSC 산출은 그 핸들의 다음 호출까지) 유효. 구조체(`CallInfo`·`FloorEvent`·`Profile` 등)는 POD 로 평탄화, 배열은 `(ptr, count)`, 참/거짓은 `int32_t`, 열거형 값은 C++ 과 같은 정수. 입력 설정은 `cimsue_*_default()` 로 채운 뒤 덮어쓴다(문자열 NULL = C++ 기본값 유지, 빈 문자열 = 지움). 모든 함수가 `Engine`/`Listener`/`CscClient` 헤더와 같은 이름·순서. 단위시험 `test/c_api_test.cpp`(S1-UE-UNIT — Windows 는 DLL 이 export 하지 않는 내부 심볼까지 시험하므로 `cimsue_test` 가 DLL 대신 코어 오브젝트(`cimsue_objs`)를 직접 링크) |
| .NET 파사드 `CimsUe.dll` | `sdk/windows/dotnet/CimsUe/` (C# 클래스 라이브러리, `net10.0-windows`, `AllowUnsafeBlocks`) | `NativeMethods`(`[DllImport("cimsue")]`·`LibraryImport` 소스 생성)는 internal. 공개면은 Kotlin 파사드와 같은 모델 — `Engine`·`Account`·`Call`·`Group`·`Subscriptions`·`CscClient` 클래스 + `IObservable`/이벤트, 콜백은 `SynchronizationContext.Post` 로 앱 스레드에 마샬링(WPF `Dispatcher` 를 참조하지 않는다). 네이티브 핸들은 `SafeHandle` 로 수명 관리, 콜백 델리게이트는 `GCHandle` 로 고정 |
| Windows 접점 (파사드 안) | `CimsUe/Platform/` | `AudioEndpoints`(`IMMDeviceEnumerator`·`IMMNotificationClient` COM interop) · `HotKeys`(`RegisterHotKey` + 메시지 전용 HWND) · `CredentialStore`(`ProtectedData` DPAPI) · `SingleInstance`(명명 Mutex + 창 활성화) · `AutoStart`(`HKCU\...\Run`) · `DeviceIdentity`(단말 속성 §4.2 — OS 판·BIOS 모델·`MachineGuid` 이름 기반 `urn:uuid:`). 프로토콜·SIP·RTP 는 이 층에 없다(§1 경계 규칙 3) |
| 앱 | `windows/dispatch-desktop/` (WPF, MVVM) | `CimsUe.dll` 만 참조. 배포는 self-contained 폴더 게시 zip(`windows/dispatch-desktop/publish.ps1` — MSVC CRT·서명 dotnet 뮤서 진입점 동봉, dispatch_desktop_ui.md §11 "배포"; MSIX·코드 서명은 향후), `cimsue.dll` 은 `CimsUe` 패키지의 `runtimes/win-x64/native/` 로 동봉 |

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
| 활성 세션 발견 — 전화 그룹원 내선·감시 대상·청취 대상 그룹 목록 | `/provisioning/me` `phoneGroup{members[]}`·`dispatch{members[]{userId,name,volteAor,pttId,extension,groupId}, pttTargets[]{id,uri,name}, etag}` + 응답 `ETag`/`If-None-Match` 304 — 서버가 `monitorScope`/`pttListen` 을 CSP 게이트와 같은 규칙으로 해석한 목록(계약 [android_ue_provisioning.md §3](android_ue_provisioning.md), 구 서버는 빈 배열) | `Profile.dispatch.members/pttTargets` → 앱이 `dialogWatch`·`subscribeConference` 대상으로(그룹원 띠 = `groupId==dispatch.groupId`) | CSC |
| 통합 이력(② PTT 내역·④ 통화 내역·메시지 모니터링) | `GET /provisioning/history?kind=call\|ptt\|message&since=&limit=`(PKCE, 역할 범위 게이트·커서 `since`/`nextSince`, 계약 [android_ue_provisioning.md §3-2](android_ue_provisioning.md)) — 진행 중 상태는 종전 구독(dialog/conference)이 담당 | `CscClient.history(kind, since, limit)`(앱 `HistoryClient` 2.5초 폴링) → `HistoryItem[]` + `nextSince` | CSC |
| PTT 그룹 생성·편집·삭제 (관제사 = authorized user) | GMS XCAP `PUT/DELETE /org.openmobilealliance.groups/users/{xui}/{group}` (문서 = GET 과 같은 OMA list-service + mcpttgi, [mcptt_api.md](../../api/mcptt_api.md) §2), 자격 `ptt.allowCreateGroup`, 목록 `is_owner`, 그룹 uri 정규형 `tel:g-<hex8>`(클라이언트 명명), `If-Match`/412, 오류 본문 `error`(`not_group_owner`·`uri_taken`·`unknown_member`·`etag_mismatch` …) | `CscClient.getGroup/putGroup/deleteGroup` + `GroupDoc`(`toXml`/`parse` — pjlib 비의존 문자열 스캔). 그룹 호 타이머·참가자 정보·MCData 크기 한도 다섯(`hangTimerSec`·`maxDurationSec`·`allowConferenceState`·`maxSdsSize`·`maxAutoRecv`)은 **미기재가 기본값**이고 미기재면 PUT 에 싣지 않는다(서버 기존값 유지) — C++ `GroupDoc::kUnset`(-1), C API `has_*`=0(0 초기화 = 미기재), Kotlin·.NET `null`. 기본값을 실제 값으로 두면 폼이 이 칸을 다루지 않는 앱이 저장할 때마다 콘솔 값을 덮는다. 멤버별 `user-priority` 는 왕복 보존 · `GroupSummary.isOwner` · `Profile.allowGroupCreation`. 실패 `Result.reason` 은 `"putGroup 403: <본문>"` — 앱이 본문 `error` 로 문구 분기(`ResponseText.GroupError`), `uri_taken` 은 id 재생성 1회 재시도. 변경 감지 = `subscribeXcapDiff("sip:gms_psi@…")` → `onMessage(xcap-diff)` | TS 24.481, RFC 4825, RFC 5875 |
| 조직/구성원/VoLTE·PTT 번호 관리·PTT 그룹 관리(관리 범위)·이력 창 조회·녹취 재생 ([관리]·[PTT 그룹]·[이력] 화면) | `/provisioning/me` `dispatch{directoryAdmin, orgCode}` + `/provisioning/directory/{admin,orgs,members,groups}`·`/provisioning/history?until=`·`/provisioning/recordings/{id}…`([android_ue_provisioning.md §3-3/§3-4](android_ue_provisioning.md)) | `CscClient.request(token, method, path, contentType, body, accept, ifMatch, ifNoneMatch) → HttpResult{status, contentType, etag, body(이진)}` — 코어가 모델링하지 않은 CSC 엔드포인트용 범용 요청(인증·전송만 코어, 경로·JSON 은 앱 `ManagementClient`). C API `cimsue_csc_request`(`cimsue_http_result_t` body+len), .NET `CscClient.Request/RequestJson(+Async)` → `HttpResponse`. `DispatchProfile.directoryAdmin/orgCode`(.NET `CanAdminDirectory`) | CSC |
| SRTP·TLS | 접속서비스 `media_srtp`·`sip_transport` | 프로파일 값으로 코어가 자동 협상. 앱 개입 없음 | RFC 4568, TS 33.203 |

앱 화면 구성은 **PTT 채널 중심**의 좌 PTT · 우 일반통화 두 열 — 각 열이 위 운영(① PTT 채널 카드 + PTT 발신·주소록 +
MCData 메시지 / ③ 전화 그룹원 띠·대표번호 대기열·내 통화 + 다이얼패드·주소록 발신 + SMS·LMS) · 아래 실시간 내역
(② PTT 내역 / ④ 일반통화 내역)인 **네 도킹 패널** + **감청 창**(듣기만 하는 세션은 팝업 — A/B 귀속·레벨)이며,
태블릿(가로)과 데스크톱이 같은 패널을 다른 밀도로 배치한다. 화면 상세(캔버스·패널·도킹·조작·배너·핫키·응답 코드
문구 사전·MVVM 구조)는 [dispatch_desktop_ui.md](dispatch_desktop_ui.md) 가 정본이다.

---

## 8. 빌드·패키징

| 대상 | 방법 | 산출물 |
|---|---|---|
| Linux (개발 서버) | 루트 `cmake .. && make` — `pjproject`(ExternalProject, `make pjproject` 단독 가능) → `cimsue` → `cimsue-cli` → `cimsue_test`. `make dist` 에 `cimsue-cli` 를 verify 도구로 포함 | `pkg/pjproject`, `build/bin/cimsue-cli`, `build/lib/libcimsue.a` |
| Android | `sdk/android/build-native.sh` — `config_site.h` 한 줄 생성(→ `android.h`) → `configure-android`(NDK) → `make` → SWIG(pjsua2+cimsue) → `.so`·Java 를 `jniLibs`/소스셋에 배치. 그 뒤 Gradle | `cimsue-android.aar` |
| Windows | `cmake -S sdk/windows -B build-win -A x64` 슈퍼빌드 — AMR-WB 래퍼 → `config_site.h` 한 줄 생성(→ `windows.h`) → pjproject 자체 CMake → `sdk/core`(DLL) → `sdk/{bin,lib,include}`. OpenSSL 은 vcpkg | `cimsue.dll`·`cimsue.lib`·헤더·`cimsue-cli.exe` |

NDK/MSVC 빌드는 개발 서버 밖(WSL2·Windows 머신)에서 수행하고, 이 서버는 Linux 빌드·단위시험·`cimsue-cli`
검증을 담당한다. 산출물(`.so`/`.dll`)은 커밋하지 않는다.

---

## 9. 검증

| stage | 항목 | 내용 |
|---|---|---|
| S1 | `S1-UE-FLOOR-CODEC` | `scripts/gen_floor_defs.py --check`(정의 테이블 ↔ 생성물·CMP·Kotlin·.NET·probe 상수) + `cimsue_test` 의 `FloorXCheck`(코어 빌더 ↔ CMP `ParseFloorMessage`, CMP `BuildFloorMessage` ↔ 코어 decode) |
| S1 | `S1-UE-UNIT` | `build/bin/cimsue_test`(googletest) — config→pjsua2 매핑(IMPI·realm `*`·H(A1)/AKA 우선·TLS 게이트 SRTP·sec-agree 헤더·proxies lr)·대상 정규화·헤더 파싱·재생 라우트 수명(null 장치 엔진 기동 → 라우트 추가/제거 → 종료 순서). 확장: SDP 협상·floor 상태머신·SDS TLV·MSRP·PKCE |
| S3 | `S3-UE-CLI-*` | `cimsue-cli` 로 등록(UDP/TLS/AKA)·1:1(평문·TLS+SRTP)·그룹콜(affiliation PUBLISH ETag·multipart INVITE·로스터 NOTIFY·floor Request→Granted/Taken·발언 RTP 수신·Idle)·SDS 송수신·관제(dialog 구독 early→confirmed→terminated, Join 200 + 감청 RTP + caller/callee SSRC 라벨, 그룹 픽업 `**`, REFER blind 전달 후 전달 대상 RTP)·PTT 청취 — 기존 `S3-SCN-*` 의 cspsim 축과 같은 판정(누적 RTP delta·403/489). 수동 절차는 VERIFICATION_MANUAL 부록, cims-verify 항목 등록은 후속 |
| S1 | `S1-UE-UNIT`(보강) | `AffiliationPublish`(루프백 가짜 ESC — 412 뒤 ETag 폐기·초기 PUBLISH 1회·앱에는 affiliate token 으로 최종 결과 하나) · `EngineCapture`(캡처 게이트 상태·재기동 전이중) · floor 시험의 pjlib 수명 짝(`test/pj_scope.h` — 한 프로세스 전체 실행) |
| 실기기 | Android 코어 점검 | `android/sdk-probe`(개발 도구 — 로그인·계정 없이 `:cimsue` 엔진만 기동, 사내 단말에 깔아도 착신을 가로채지 않는다): 영상 장치 열거(네이티브 제어 스레드에서 `PjCamera2` — 앱 클래스 로더 패치)·H.264 정책·장치 단 음량·라우트·캡처 게이트·재오픈 호출. 결과 줄 `PASS`/`FAIL` 은 화면·logcat `SdkProbe` |
| 실기기 | Android | 태블릿·UNIWA 에서 감청 SSRC 2개 귀속 표시·PTT 청취 버튼 비활성·대표번호 착신 — 와이어 실측 |
| 실기기 | Windows | 재생 라우트 이중 출력(헤드셋+스피커, WMME)·핫플러그 재열거·핫키·감청 영상 격자(F3) |

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
| E. 관제 태블릿 앱 | `android/dispatch-tablet` — §7 다섯 구획. **구현 완료** — 정본 [android_dispatch_tablet.md](android_dispatch_tablet.md). 하단 내비 넷 + [관제] 탭 둘(6패널), [이력]·[PTT 그룹]·[관리]·감청 시트·관제 요약 띠·착신 배너/알림·주소록·발신 시트. 엔진 단일화(`:cimsue-engine` 하나, 커밋 산출물 폐기)와 SWIG 이진 typemap 이 여기서 들어왔다. 실기 확인 = 로그인·등록·그룹콜 floor·SDS·감청(Join)·착신/발신·통화 내역 | 실기기 실측(§9) — 남은 것: [이력]·[PTT 그룹]·[관리] 세 화면이 서버 응답으로 미검증, 무전/통화 분리 출력, 6시간·부팅 상주 |
| F1. Windows 엔진·코어 | `sdk/windows` 슈퍼빌드로 pjproject(WMME)·AMR-WB·`cimsue.dll`·`cimsue-cli.exe` MSVC 빌드 — **빌드 확정**(§6.1 엔진 빌드 확정·CRT 행). 남은 것: WMME 장치 열거 실측 | Windows 에서 `cimsue-cli` 등록·1:1(TLS+SRTP)·그룹콜 floor·Join 이 Linux 와 같은 결과 (S3 실측 전) |
| F2. Windows C API·.NET 파사드·관제 앱 | C API `cimsue_c.h`(§6.4) — **구현·단위시험 반영**(`cimsue.dll` 이 80 함수 export — `cimsue_struct_size` ABI 자기검사 포함, `cimsue_test` 가 슈퍼빌드의 googletest 로 Windows 에서도 돈다) → `sdk/windows/dotnet/CimsUe`(파사드 + 접점: 엔드포인트·핫플러그·핫키·DPAPI·단일 인스턴스 — **구현·단위시험 50건 통과**: ABI 레이아웃 27 구조체 대조·헤드리스 엔진 수명·컨텍스트 마샬링·프로파일 파싱·접점. 네이티브 `cimsue.dll` 은 관리 `CimsUe.dll` 과 이름이 겹치므로 출력·패키지 모두 `runtimes/win-x64/native/` 에 두고 로더가 그곳을 먼저 본다) → `windows/dispatch-desktop`(WPF, §6.1 — **구현·빌드 완료**, [dispatch_desktop_ui.md](dispatch_desktop_ui.md) §11 구조 그대로. 로그인·메인 창 기동 확인, `--ui-preview` 로 로그인 없이 화면 점검) | 파사드로 `cimsue-cli` 와 같은 S3 시나리오 재현, 재생 라우트 이중 출력·핫플러그 실측, 관제 시나리오(BLF→Join→픽업→전달→PTT 청취) 실기 — **앱 실기 시험은 서버(CSC/CSP) 연결 후 일괄** |
| F3. Windows 영상 | `PJMEDIA_HAS_VIDEO 1` + OpenH264 + DSHOW + CIMS 콜백 렌더 장치 패치 → `onVideoFrame` | 감청 영상 격자 실측 |
| G. 기존 앱 전환 | `volte-client` → `ptt-client` 를 파사드로 — §5.3 이행 단계 P0a~P5(VoLTE 먼저) | 단계별 완료 조건(§5.3) |

---

## 11. 미해결 / 향후 과제

- **구독 종료 사유를 앱까지 올린다** — 코어의 구독 셋(`dialogWatch`·`subscribeConference`·`subscribeXcapDiff`)은
  pjsua 의 CIMS 인터셉터(`pjsua_acc.c` `pjsua_cims_conf_subscribe`)를 거쳐 `pjsip_evsub` 로 들어가므로
  **갱신·해지·서버 부여 `Expires` 는 이미 엔진이 처리한다**(`evsub.c` `TIMER_TYPE_UAC_REFRESH`,
  `cims_conf_cb.on_client_refresh = NULL` = 기본 자동 갱신). 빠진 것은 **종료 전달**이다 —
  `pjsua_pres.c` `cims_conf_on_evsub_state` 는 `PJSIP_EVSUB_STATE_TERMINATED` 에서 로그를 남기고 슬롯만
  해제할 뿐 앱에 올리지 않는다. 그래서 서버가 인가 회수로 구독을 끊어도
  (`terminated;reason=rejected|deactivated` — [dispatch_center.md §5.10](dispatch_center.md)) 앱은 화면의 낡은
  행을 비우지 못하고, `deactivated` 의 «즉시 재구독» 권고(RFC 6665 §4.1.3)도 성립하지 않는다. 필요한 것 =
  CIMS 콜백이 사유·자원을 코어로 올리고, 파사드가 `onSubscriptionEnded(event, resource, reason)` 로 공개하며,
  앱이 사유별로 화면 비우기/재구독을 정하는 것. 실기기 회귀가 필요해 별건으로 둔다.
- **Windows 오디오 이중 출력** — 재생 라우트(재생 전용 `ExtraAudioDevice`)의 WMME 지연·에코·장치 점유 실측. WMME 가 부족하면
  데스크톱 WASAPI 백엔드(`IMMDeviceEnumerator`+`IAudioClient` 공유 모드 — 2.16 의 UWP 전용 구현과 별개 파일)를 엔진 패치로 추가.
- **Windows 영상 렌더 경로** — pjproject 에 "창 없는 프레임 콜백" 렌더 장치가 없다. F3 에서 pjmedia-videodev 콜백 장치를
  CIMS 패치로 추가해 `onVideoFrame` 을 채운다(Android 프레임 콜백 선택지와 같은 장치를 공유).
- **Android 영상 경로 선택** — Surface 직결(현행) vs 프레임 콜백(§4.5). 감청 격자 합성이 필요한 관제 태블릿은
  프레임 콜백이 맞고, 1:1 영상 앱은 Surface 직결이 싸다. 파사드가 둘을 다 제공할지 결정.
- **HTTP 전송 주입** — §4.4 의 `http::ITransport` 는 코어 내부에만 있고 주입 통로가 없다(세 플랫폼 공통).
  프록시 경유가 필요해지면 인터페이스를 공개 헤더로 올리고 SWIG director·C API·.NET 에 같이 낸다.
- **C API 생성 자동화** — §6.4 의 C API 는 손 평탄화가 출발점. C++ 헤더가 커지면 SWIG C# 백엔드 또는 헤더 파서 기반
  생성으로 전환할지 F2 종료 시 판단(정본은 어느 쪽이든 C++ 공개 헤더).
- **개발 서버 TLS 인증서** — `build/dist/csp/cert/csp.pem` 이 자가서명·SAN 없음이라 코어의 서버 검증(`tlsVerifyServer`)을
  켠 채로는 등록이 503 `PJSIP_TLS_ECERTVERIF` 로 막힌다(정상 동작). TLS/SRTP 회귀를 검증 켠 채 돌리려면
  sip_tls_signaling.md §8 요건(SAN=도메인/IP)의 인증서를 개발 서버에 발급해야 한다.
- **개발 서버 CSC 프로비저닝 설정** — `Provisioning.Services.*.host` 가 비어 있고 port=15060 이라 `/provisioning/me` 가
  `127.0.0.1:15060` 을 내려준다(실 접속점은 121.161.164.48:5060). 단말 SDK 는 프로파일을 그대로 따르므로 개발 서버
  csc.json 의 서비스 host/port 를 실 값으로 맞춰야 `--from-profile` 만으로 등록된다(cli 는 `--server/--port` 명시로 덮을 수 있다).
- **청취 전용 leg 의 미디어 상태** — 서버가 `a=sendonly` 로 답하므로 pjsua 는 REMOTE_HOLD 로 분류한다. 코어는 recvonly
  leg(Join·PTT 청취)에서 이를 활성으로 다루지만, 감청 leg 의 SSRC 별 활성/레벨은 아직 SDP 라벨만 있고 실시간 값이 없다
  — pjproject 에 U10 서브스트림 관측 API(SSRC 별 수신 활성·레벨)를 추가해야 `MediaSources.active/level` 이 채워진다.
- **호 전달 후 누적 통계** — 전달로 미디어 스트림이 재생성되면 마지막 소멸 스트림의 통계만 남는다(스트림별 누적 합산은 후속).
- **진행 중 조건의 개시자** — 조건 재광고 re-INVITE 는 조건을 건 사용자를 `<mcptt-calling-user-id>` 에 싣는다(TS 24.379 V18.6.0
  §6.3.3.1.6 2)). 코어는 조건만 읽어(§4.2 «긴급·임박 세션 조건») `McpttCondition` 에 개시자가 없다 — 관제 앱 긴급 배너는 진행 중에 걸린
  조건의 개시자를 비운다(`CallInfo.mcptt.callingUserId` 는 호를 세운 INVITE 의 값이다). 청취 leg 는 코어만으로 끝나지 않는다 — CSP 가
  조건 재광고에서 청취 leg 를 빼므로(`PropagateConditionToMembers`) 서버 계약이 함께 필요하다([android_dispatch_tablet.md](android_dispatch_tablet.md) §11).
- **MCPTT 착신 수락의 호 종류별 분리** — `AccountConfig.autoAnswerMcptt` 하나가 그룹콜·사설콜을 함께 자동 수락한다. 규격은 수락 방식을 호 종류별로
  둔다(TS 24.379 §6.2.3 commencement mode — 사설콜·그룹콜 각각 자동/수동). 관제석은 그룹콜 자동 + 사설콜 수동이 맞다 — 둘로 나누면 C API·.NET·Kotlin 에
  같이 낸다. 긴급 사설콜의 전역 표시는 이 분리 뒤에 두 관제 앱이 정한다([dispatch_desktop_ui.md](dispatch_desktop_ui.md) §13,
  [android_dispatch_tablet.md](android_dispatch_tablet.md) §6.2a-1).
- **요청 대상의 `tel:`** — `normalizeTarget`(`account_map.cpp`)은 `sip:`·`sips:`·`tel:` 을 통과시키고 스킴 없는 값만 `sip:<번호>@<도메인>` 으로 만든다.
  `tel:` 은 호스트가 없어 라우팅할 수 없는 Request-URI 라 요청이 나가지 못해, 두 관제 앱은 넘기기 전에 벗긴다(태블릿 `routableTarget`, 데스크톱 `UserPart`).
  코어가 `tel:` 을 도메인 붙은 `sip:` 로 바꿀지(발신·합류·픽업·전달·구독 — 세 바인딩의 요청 경로 전부가 바뀐다), 호출자가 라우팅 가능한 형태로 준다는
  계약으로 적을지는 CSP 의 `tel:` Request-URI 처리를 확인한 뒤 정한다.
- **SDS disposition 통지의 규격 경로** — `sendSdsNotification` 은 원 발신자 AoR 로 SDS NOTIFICATION 한 파트만 보낸다. TS 24.282
  V18.13.0 §12.2.1.1 은 대상 MCData ID 의 `resource-lists` 와 그룹 통지의 `<mcdata-calling-group-id>` 를 요구한다 — 통지 API 가 수신
  SDS 의 그룹·발신자 문맥을 받아야 하고 CSP 통지 처리와 함께 바뀐다([mcdata_messaging.md](mcdata_messaging.md) §7 편차 표).
- **망 전환 중의 호 유지** — `handleNetworkChange` 는 등록만 되살리고 진행 중 호는 그대로 둔다 — 로컬 주소가 바뀌면 그 호의 RTP 가
  끊길 수 있다. 호를 옮기려면 호마다 re-INVITE(미디어 재초기화·Contact/Via 갱신)를 보내야 하고, MCPTT 호의 floor `m=application` 은
  re-INVITE SDP 에도 다시 실린다(`pendingAppSdp`). 넣기 전에 CSP 가 단말발 re-INVITE(VoLTE relay·MCPTT 세션)를 어떻게 다루는지
  확인해야 한다(실기 미확인). 끊긴 호의 서버발 in-dialog 요청은 CSP 가 살아 있는 등록 바인딩으로 다시 찾는다
  ([leg_liveness.md](leg_liveness.md) §6.3). Windows 관제 앱은 아직 `RefreshRegistrations`(계정별 REGISTER)라 `HandleNetworkChange` 로 옮길 몫.
- **remote-init ambient listening·barge-in** — 서버 §10 과제와 함께 코어 API 확장.
- **음성 품질 측정·시험 모드 계측기 링크** — 코어 `quality/`(RTCP-XR·G.107/G.107.1 E-model·`callQuality`)·drive 루프의 코어 이전
  (`drive/` `DriveSession` — stdin/stdout 과 TLS 계측 링크 공용)·`cimsue-cli --link` 는 [ue_voice_quality.md](ue_voice_quality.md) 가 정본(측정 Q1·코어 링크 Q2 구현 반영, Android `DeviceLink` 바인딩 반영 — C API·.NET 바인딩과 앱 시험 모드 Q4 미구현).
- **cspsim 과 `cimsue-cli` 의 역할 분담 장기안** — 시뮬레이터 축(부하·다중 단말)과 실스택 축(정합)의 S3 항목 배분.
- **긴급 상태 머신 전체** — 코어는 세션 단위 조건(현재값·내가 올렸나·응답 대기)만 둔다. TS 24.379 §6.2.8.1 의 경보 상태(MEA 1~4)·
  긴급 그룹콜 상태(MEGC)·Warning 149(경보 미인가 수용) 판정·긴급 사설콜 조건 변경(§11)·ad hoc 긴급(§6.2.8.1.19~)은 후속. 구동 세션
  (`drive/`)의 긴급·경보·MSRP 명령(계측기 real-ue·device 풀)도 후속.
- **1:1 media plane SDS** — 서버 McDataMediaService 가 그룹만 받는다(phase 1). 서버가 1:1 을 받으면 `sendSds` 도 같은 상한으로 가른다.

## 12. 문서 갱신 대상 (구현과 같은 변경에서)

- [android_ue_client.md](android_ue_client.md) §2.2·§9 — 앱 내부 레이어·모듈 구조를 본 문서로 위임.
- [android_ue_m1_pjsip_integration.md](android_ue_m1_pjsip_integration.md) §2 — 빌드 플레이북을 `ext/pjproject` 정본·`build-native.sh` 기준으로 재작성.
- [mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md) §6 — 정의 테이블 파일·생성 규약 확정 반영.
- [dispatch_center.md](dispatch_center.md) §7 단말 행·§8.4 — 코어 API 이름으로 교차 참조.
- `ext/pjproject/README.CIMS.md` — 정본 관계(트리 = SoT).
- [../../VERIFICATION_PROCESS.md](../../VERIFICATION_PROCESS.md) — `S1-UE-*`·`S3-UE-CLI-*` 항목 등록.
