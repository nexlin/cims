# 관제조작반 Android 태블릿 앱 (`android/dispatch-tablet`)

> 관제조작반의 두 번째 플랫폼. **화면 의미론·응답 문구·조작 규약은 Windows 판과 같은 정본**
> ([dispatch_desktop_ui.md](dispatch_desktop_ui.md))을 따르고, 이 문서는 태블릿에서 **갈라지는 것**을 정한다 —
> 코어 바인딩 계약, Kotlin 파사드, Android 접점층, 수명주기 소유권, 태블릿 밀도(같은 문서 §12 확장).
> CIMS 연동은 전부 `libcimsue` 위에 있고 앱은 화면·장치·수명주기만 갖는다([ue_sdk.md](ue_sdk.md) §2).
>
> **설계 정본, 구현 착수 전.** `android/dispatch-tablet` 과 `sdk/android/cimsue` 는 아직 없다.
>
> 관련: [ue_sdk.md](ue_sdk.md)(§4 코어 API·§5 Android SDK·§7 요구↔API·§10 이행 순서),
> [dispatch_center.md](dispatch_center.md)(§5 관제 절차·§8.4 단말 요건),
> [android_ue_provisioning.md](android_ue_provisioning.md)(`/provisioning/me` 계약),
> [volte_supplementary_services.md](volte_supplementary_services.md)(픽업·전달),
> [mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md)(U10).

---

## 1. 범위와 결론

| 결정 | 내용 | 근거 |
|---|---|---|
| 이식 단위 | **화면 의미론과 로직을 이식하고 위젯 트리는 이식하지 않는다** | Windows 앱 로직층(ViewModels·Services·Models 7,999줄)은 `System.Windows` 참조가 2파일뿐이라 의미 보존 재작성이 가능하다. 도킹·별창은 데스크톱 어포던스라 옮기지 않는다 |
| 공개면 | 앱은 `com.cims.ue.sdk.*` 만 쓴다. `com.cims.ue.sdk.jni.*`·`org.pjsip.*` 는 파사드 내부 | ue_sdk.md §5.1 |
| 바인딩 | SWIG 으로 C++ 공개 헤더를 Java 로 낸다. 손 JNI 없음. **출력 인자는 코어 반환형으로 없앤다**(§3.3) | 출력 인자는 바인딩 3종에 모두 군더더기를 만든다 |
| UI | Jetpack Compose, Material 3 | 기존 `android/` 전부 Compose |
| 화면 배치 | **모바일 앱으로 짠다** — 한 화면은 한 가지 일, 이동은 하단 내비 다섯 + 좌우 스와이프, 분할 없음(§6.3) | 태블릿 본문은 데스크톱 면적의 38% 다. 같은 격자를 줄여 넣으면 어느 칸도 제 몫을 못 한다 |
| 불변 규약 | **포커스(보는 채널) ≠ 발언 대상(말하는 채널)** · 응답 코드 문구 사전(§9) · 식별자는 코어가 준 id | 플랫폼이 달라도 같다 |
| 기존 앱 | **엔진은 이번에 단일화한다** — `:core` 가 `:cimsue-engine` 의 pjsua2 를 쓰고 커밋된 산출물 22.5MB 를 지운다(§2.2). 앱 로직 전환(파사드 이전·`PttController` 분해)은 이식 완료 후 별도 과제다(§11) | 엔진이 두 벌이면 `ext/pjproject` 패치를 두 곳에 반영해야 하고 커밋본이 조용히 어긋난다. 반면 로직 전환은 동작하는 앱의 회귀 위험을 이식 일정에 싣는다 |

---

## 2. 층 구조와 모듈 경로

```
android/  (Gradle 루트 — settings.gradle.kts)
  :cimsue-engine    → ../sdk/android/cimsue-engine   org.pjsip.** 바인딩 + libpjsua2.so
  :cimsue           → ../sdk/android/cimsue          코어 파사드 + libcimsue.so
  :dispatch-tablet  관제 태블릿 앱   ──→ :cimsue
  :core             기존 공용 모듈   ──→ :cimsue-engine   (커밋 산출물 제거, §2.2)
  :ptt-client :volte-client :cims    기존 앱 — **로직은 건드리지 않는다**
```

Gradle 루트가 `android/` 이므로 `:sdk:cimsue` 경로는 성립하지 않는다. 두 모듈 모두
`include(":<이름>")` 뒤에 `project(":<이름>").projectDir = file("../sdk/android/<이름>")` 로 위치만 옮긴다 —
소스 동기 빌드가 유지되고 AAR 을 따로 publish 할 필요가 없다.

### 2.1 `sdk/android` 구성

```
sdk/android/
  CMakeLists.txt         NDK 크로스 빌드 래퍼 — sdk/core 를 add_subdirectory 하고
                         PJ_INC / PJ_LIB_PATH / PJ_TARGET / PJ_SYS_LIBS 를 NDK 값으로 준다
                         (sdk/core/CMakeLists.txt 가 예고한 크로스 인자. sdk/windows/CMakeLists.txt 와 같은 역할)
  build-native.sh        ① config_site.h 생성 ② configure-android + make(pjproject)
                         ③ sdk/core → libcimsue.so ④ SWIG — pjsua2 와 cimsue 둘 다 ⑤ 산출물 배치
  cimsue-engine/         Gradle Android Library → cimsue-engine.aar (arm64-v8a)
    src/main/java/org/pjsip/            SWIG pjsua2(306) + PjCamera*(4) — 생성물, 커밋 안 함
    src/main/jniLibs/arm64-v8a/{libpjsua2,libc++_shared}.so             생성물, 커밋 안 함
  cimsue/                Gradle Android Library → cimsue-android.aar (arm64-v8a)
    src/main/jniLibs/arm64-v8a/{libcimsue,libc++_shared}.so             생성물, 커밋 안 함
    src/swig/java/com/cims/ue/sdk/jni/                                   생성물, 커밋 안 함
    src/main/java/com/cims/ue/sdk/
      CimsUe.kt          파사드 — 명령·상태·이벤트 (§4)
      Types.kt           Kotlin 값 타입 (data class)
      CscClient.kt       CSC 파사드 (suspend)
      platform/          Android 접점 (§5)
      http/                     (예약 — 전송 주입은 아직 열려 있지 않다, §3.4)
```

### 2.2 엔진 단일화

`ext/pjproject` 가 엔진의 유일한 소스 정본인데([ue_sdk.md](ue_sdk.md) §3), 산출물은 지금 **두 벌**이다 —
`android/core/src/pjsua2/`(커밋된 Java 310개 + `libpjsua2.so` 13.3MB + `libc++_shared.so` 9.2MB = 22.5MB)와
새 AAR 이 빌드하는 것. 두 벌이면 엔진 패치가 두 곳에 반영돼야 하고 커밋본은 조용히 어긋난다.
**이 이중화를 이번에 끊는다.**

- `:cimsue-engine` 이 `org.pjsip.**` 를 독점 제공한다. `PjCamera2`·`PjCameraInfo2`(`org/pjsip/` 4개)를
  `:core` 가 실제로 쓰므로(`SipController.kt`·`PjLib.kt`·`CimsCall.kt`) `org.pjsip.pjsua2` 만이 아니라
  **`org.pjsip.**` 전체**를 싣는다.
- `:core` 는 `src/pjsua2/` 소스셋을 버리고 `api(project(":cimsue-engine"))` 으로 바꾼다.
  `SipController.kt` 등 **앱·코어 손코드는 한 줄도 바뀌지 않는다** — 같은 클래스가 소스셋 대신 AAR 에서 올 뿐이다.
- 커밋된 `android/core/src/pjsua2/` 를 지운다(312 파일). 이후 엔진 산출물은 커밋하지 않는다(`.gitignore`).
- `build-native.sh` 가 엔진과 코어를 **한 번에** 짓고 두 모듈에 배치한다. 엔진만 필요하면 `--engine-only`.
- 코어의 `cimsue-cli` 는 데스크톱 전용이다(`cli/main.cpp` 의 크래시 백트레이스가 glibc `execinfo` 를 쓰고
  bionic 에는 없다) — `CIMSUE_BUILD_CLI` 가 Android 에서 기본 OFF 다.

`libc++_shared.so` 는 두 AAR 이 각자 싣는다. **어떤 앱도 두 AAR 을 동시에 쓰지 않으므로**(태블릿 → `:cimsue`,
기존 앱 → `:core` → `:cimsue-engine`) 충돌하지 않는다. 장차 한 앱이 둘 다 필요해지면 AGP `pickFirst` 로 받는다.

`libcimsue.so` 와 `libpjsua2.so` 는 각각 pj 를 정적으로 품는다(strip 후 각 11.2MB·10.5MB). 그래서 코어를
쓰는 앱은 엔진 `.so` 를 따로 실을 필요가 없다. 둘을 같이 싣는 앱이 생기면 pj 코드가 두 벌 들어가고
동적 심볼이 겹치므로(둘 다 `pjsip_*` 를 export 한다) 그때는 공유 `libpj.so` 로 가야 한다 — pjproject
빌드 변경이라 지금 하지 않는다. **현재 어떤 앱도 둘을 같이 쓰지 않는다.**

엔진 링크 정보(툴체인·라이브러리 목록·include)는 `sdk/android/CMakeLists.txt` 가 `ext/pjproject/build.mak`
에 **물어본다**(`make` 로 펼쳐 읽는다). 값을 베껴 두면 엔진 구성(코덱 on/off·OpenSSL 경로)이 바뀔 때
조용히 어긋나기 때문이다. 예외가 하나 있다 — `APP_LDLIBS` 는 `pjsua2`(C++ 계층)를 빼고 낸다. 앱이
선택하는 층이라서다. 코어는 pjsua2 위에 있으므로 `PJSUA2_LIB_LDLIB` 를 따로 물어 맨 앞에 붙인다.

### 2.3 층 경계

| 층 | 아는 것 | 모르는 것 |
|---|---|---|
| 코어 `libcimsue` | SIP·RTP/SRTP·floor·MCData·CSC·관제 절차 | UI 프레임워크, Android 클래스 |
| SWIG 바인딩 | 코어 공개 헤더의 모양 | 관용구(Flow·coroutine) |
| Kotlin 파사드 | 코어 API ↔ Kotlin 관용구, 스레드 마샬링 | 화면, 장치 |
| Android 접점 | 장치·입력·수명주기·자격 저장 | **프로토콜** |
| 앱 | 화면·상태 투영 | 와이어 |

접점층에 프로토콜이 새면 층 경계 위반이다.

---

## 3. 코어 바인딩 계약

`sdk/core/swig/cimsue.i` 한 파일이 Java 바인딩의 정본이다. 현재 이 파일은 `types.h`·`listener.h`·`engine.h`
셋만 노출하고, 그 결과 **CSC 평면 전체가 Java 에 없으며** 값 컨테이너가 불투명 포인터로 떨어진다.

### 3.1 노출 범위

`csc.h` 를 `%include` 한다. 태블릿 앱은 로그인·프로비저닝부터 CSC 로 하므로 이것 없이는 기동하지 못한다.
`CscClient` 의 출력 인자(`TokenSet&`·`Profile&`·`GroupDoc&`·`XcapDoc&`·`std::vector<GroupSummary>&`)는
참조 타입이라 SWIG 프록시 객체로 전달·변형되므로 추가 처리가 필요 없다.

### 3.2 신뢰 앵커는 앱이 넘긴다

**Android 에는 OpenSSL 이 읽는 기본 인증서 경로가 없다.** 신뢰 저장소가 Java 층(`X509TrustManager`)에
있기 때문이다. 코어는 `caPem` 이 비면 `SSL_CTX_set_default_verify_paths()` 로 폴백하는데
(`https_client.cpp`), Android 에서 이 경로는 비어 있어 `unable to get local issuer certificate` 로 떨어진다.
**앵커를 비운 채로 두면 로그인부터 막힌다.**

그래서 SDK 가 앵커를 동봉한다 — `TrustAnchors.CA_BUNDLE`(루트 1장, sip_tls_signaling.md §8·§579).
앱은 이것을 `EngineConfig.tlsCaPem` 과 `CscEndpoint.caPem` 양쪽에 넘긴다(SIP TLS 와 HTTPS 가 같은 앵커,
ue_sdk.md §4.4). 사이트 CA·leaf 는 서버가 체인으로 보내므로 APK 는 사이트·서버 주소와 무관하다.

같은 앵커가 `android/core/.../sip/CimsTrustStore.kt` 에도 있다 — 기존 앱이 아직 파사드로 넘어오지
않았기 때문이며(§11), **둘을 함께 고쳐야 한다**. `TrustAnchorTest` 가 파싱·주체·유효기간·들여쓰기를 고정한다.

### 3.3 전송 주입은 아직 없다

`ue_sdk.md` §4.4 는 코어가 HTTP **프로토콜**을 갖고 **전송**은 `IHttpTransport` 로 추상해 플랫폼 SDK 가
OkHttp/WinHTTP 구현을 주입한다고 쓴다. 코어 안에 그 추상(`http::ITransport`)은 실재하지만 **주입 통로는
어느 플랫폼에도 열려 있지 않다**:

- 인터페이스가 내부 헤더 `sdk/core/src/http/https_client.h` 에 있다(공개 헤더 `include/cimsue/` 밖).
- C API 에 진입점이 없다 — `cimsue_csc_create(const cimsue_csc_endpoint_t* ep)` 는 endpoint 만 받는다.
  따라서 Windows `.NET` 파사드도 주입하지 못한다.
- 공개 시그니처 `CscClient(ep, std::shared_ptr<http::ITransport>)` 만 내부 타입을 드러내는데, 바인딩에는
  쓸 수 없는 불투명 핸들만 남으므로 SWIG 에서 지운다.

그래서 세 플랫폼 모두 **기본 OpenSSL 전송**을 쓴다. 사설 CA 는 `EngineConfig.trustAnchors`·`CscEndpoint.caPem`
으로 받으므로 인증서 정책은 주입 없이 해결된다. **프록시 경유는 지금 불가능하다** — 필요해지면 인터페이스를
공개 헤더로 올리고 세 바인딩(SWIG director·C API·.NET)에 같이 내야 한다(§11).

### 3.4 값 컨테이너

| 타입 | 자리 | 처리 |
|---|---|---|
| `std::vector<RosterEntry>` | `listener.h` `onRoster` | `%template(RosterVector)` |
| `std::vector<Talker>` | `types.h` `FloorEvent.talkers`·`FloorInfo.talkers` | `%template(TalkerVector)` |
| `std::vector<GroupSummary>` | `csc.h` `listGroups` | `%template(GroupSummaryVector)` |
| `std::map<std::string,std::string>` | `engine.h` `sendRequest(headers)` | `%include "std_map.i"` + `%template(StringMap)` |

`onRoster` 는 ① 채널 카드 3줄(로스터 미리보기)과 ② 범위 채널 참가자 수의 유일한 소스라, 불투명하면
로스터 기능 전체가 성립하지 않는다.

### 3.5 출력 인자를 코어 반환형으로 없앤다

`Engine::sendGroupSds`·`sendSdsNotification` 은 요청 token 을 `int64_t*` 출력 인자로 준다. SWIG 은 이것을
불투명 핸들(`SWIGTYPE_p_long_long`)로 떨어뜨리는데, token 은 **SDS·SMS 발신 결과 상관의 유일한 키**다 —
최종 응답이 `onRequestResult(MESSAGE, token)` 로 오므로 앱이 이 값으로 보냄/실패를 맞춘다.

코어 공개 헤더가 결과를 **반환**하도록 바꾼다.

```cpp
struct SdsSend { std::string msgId; int64_t token = -1; };   // types.h

SdsSend sendGroupSds(int accountId, const std::string& groupId, const std::string& text,
                     bool requestDelivery = true);
SdsSend sendSdsNotification(int accountId, const std::string& peer, const std::string& convId,
                            const std::string& msgId, int notifType);
```

| 영향 | 내용 |
|---|---|
| 건드리는 곳 | `include/cimsue/{types.h,engine.h}` · `src/engine.cpp` · `src/c_api.cpp` · `cli/main.cpp` · 단위시험 |
| **C ABI** | **바뀌지 않는다.** `cimsue_engine_send_group_sds` 의 출력 인자는 C API 구현이 흡수한다 |
| Windows | **무영향.** .NET 파사드와 `windows/dispatch-desktop` 은 C API 만 부른다 |
| 수렴 | .NET 이 이미 손으로 정의한 `record SdsSend(string MsgId, long Token)` 과 같은 모양이 된다 |

회귀는 Linux 빌드가 잡는다(`c_api.cpp` 가 같은 오브젝트에 들어간다).

---

## 4. Kotlin 파사드

코어 API 는 **명령 / 상태 스냅샷 / 이벤트** 세 갈래다([ue_sdk.md](ue_sdk.md) §4.2). 파사드는 이 셋을
Kotlin 관용구로 옮기기만 하고 **프로토콜 판단을 갖지 않는다**.

| 코어 | Kotlin | 이유 |
|---|---|---|
| 명령 | **`suspend fun`** | 코어의 `runSync` 는 제어 스레드에 넘기고 `fut.get()` 으로 **호출자를 기다린다**(`engine.cpp:61~67`). 그 안에서 동기 I/O 가 일어날 수 있다 — `dial` → `makeCall` → `sip_resolve` 의 `getaddrinfo`. 메인 스레드에서 부르면 ANR 이다 |
| 상태 — 잠금 스냅샷 (`callInfo`·`calls`·`regInfo`) | 동기 조회 `fun` + `StateFlow` | 뮤텍스 읽기뿐이라 어느 스레드에서도 안전하다. 재접속·재구성 후 화면 복원 경로가 이 재조회다 |
| 상태 — 제어 스레드 조회 (`floorInfo`·`streamStats`·`audioDevices`) | **`suspend fun`** | 이름은 조회지만 `runSync` 를 탄다(`engine.cpp:1123`). 명령과 같이 다룬다. **floor 는 이벤트만으로 부족하다** — 코어의 `request`/`release` 는 이벤트 없이 상태를 바꾸는 구간이 있으므로, 앱은 이벤트로 즉시 그리고 뒤이어 스냅샷으로 덮는다(화면 복귀에서도 같이 당긴다) |
| 이벤트 (`Listener` director) | `SharedFlow` / `Channel` | 방출은 코어 **이벤트 스레드**에서 일어나고 **절대 블록하지 않는다** — 그 스레드를 막으면 모든 이벤트가 밀린다. 전부 버려도 되는 것은 아니라 유실 정책을 셋으로 가른다(아래 표) |
| `CscClient` (동기 HTTP) | `suspend fun` (`Dispatchers.IO`) | 코어가 프로토콜을, 파사드가 스레드만 옮긴다 |
| 계정·호 (id) | `Account`/`Call` 이 id 를 감싼다 | 공개 표면은 id 지만 앱 편의를 위해 객체로 준다 |

**이벤트 유실 정책** — `SharedFlow(replay=0)` 는 수집자가 없으면 버퍼도 보존하지 않는다. 종류마다 대가가 달라 한 정책으로 묶지 않는다.

| 정책 | 대상 | 근거 |
|---|---|---|
| ① 버려도 됨 — `DROP_OLDEST` | `log` | 손실이 동작에 영향을 주지 않는다 |
| ② 최신만 맞으면 됨 — `DROP_OLDEST` + 유실 카운터 | `regState`·`incomingCall`·`callState`·`callMedia`·`floor`·`roster`·`dialogInfo` | **권위는 스냅샷**이다(`registrations`·`callIds` + `callInfo()` 재조회) — §6.7 의 "UI 는 코어 상태의 투영" 이 여기서 성립한다. 버려진 수는 `droppedEvents` 로 세고, 0 이 아니면 화면이 스냅샷 재조회로 스스로를 고친다 |
| ③ 버리면 안 됨 — `Channel(UNLIMITED)` | `sds`·`requestResult`·`message` | 되찾을 스냅샷이 없다. SDS 를 놓치면 메시지가 사라지고, `requestResult` 를 놓치면 발신이 "보내는 중" 에 고착된다(token 상관이 유일한 경로). 수집자가 붙기 전 것도 쌓이고 한 번만 소비되므로 **수집자를 하나만** 둔다 |

객체 구분과 이벤트 이름은 .NET 파사드와 **같은 의미 묶음**을 쓴다 — 두 플랫폼 앱이 같은 형태로 읽히게 한다.

| .NET | Kotlin |
|---|---|
| `Engine.Start/Stop/Dispose` | `CimsUe.start/stop/close` |
| `Engine.AudioDevices/SetAudioDevices/AddPlaybackRoute/RemovePlaybackRoute` | 같은 이름 |
| `Engine.TlsPeerExpiry` | `val tlsPeerExpiry: StateFlow<TlsPeerExpiry?>` |
| `event CallStateChanged`·`IncomingCall`·`CallMediaChanged` | `val callState`·`incomingCall`·`callMedia: SharedFlow<CallInfo>` |
| `event FloorChanged`·`RosterChanged`·`DialogInfoReceived`·`SdsReceived`·`RequestCompleted`·`MessageReceived`·`Log` | 같은 이름의 `SharedFlow` |
| `Account.Register/Unregister/RefreshRegistration/Dial/JoinGroupCall/StartPrivateCall/Affiliate/SubscribeConference/SubscribeXcapDiff/SendRequest/DialogWatch/Join/Pickup/SendGroupSds/SendSdsNotification` | 같은 이름의 `fun` |
| `Call.Answer/Reject/Hangup/Hold/Resume/SetMuted/SetListen/SetRxLevel/SendDtmf/LeaveGroupCall/FloorRequest/FloorRelease/FloorQueueCancel/Transfer/TransferAttended/SetRoute` | 같은 이름의 `fun` |
| `CscClient.Login/Refresh/FetchProfile/ListGroups/GetGroup/PutGroup/DeleteGroup/Request/RequestJson/XcapGet/GetUserProfile/GetServiceConfig` | 같은 이름의 `suspend fun` |

`Result<T>` 는 Kotlin 표준 `Result` 와 이름이 겹치므로 파사드 타입을 `CimsResult<T>` 로 두고
`ok`/`code`/`reason` 을 그대로 노출한다.

**director 수명**: 프록시는 파사드의 Kotlin 강참조로 살린다. SWIG 의 `swigTakeOwnership()` 은 네이티브
참조를 `WeakGlobalRef` 로 바꾸고 `swigReleaseOwnership()` 은 `NewGlobalRef` 로 영구 고정하므로
(`java_change_ownership`), **둘 다 부르지 않는다** — 전자는 콜백 중 GC 를 허용하고 후자는 객체 그래프를 누수한다.

**수명 게이트**: `close()` 는 신규 호출을 먼저 막고 진행 중인 JNI 호출이 빠지기를 기다린 뒤 해제한다.
생성 코드는 `delete()` 만 동기화하고 호출은 `swigCPtr` 를 보호 없이 읽으므로, 게이트가 없으면 진행 중인
요청과 종료가 겹쳐 use-after-free 가 된다. 해제 뒤 호출은 `CimsResult.fail(-99, "engine closed")` 로 떨어진다
(막지 않으면 0 포인터 역참조 = 프로세스 크래시). `CscClient` 도 같은 게이트를 쓴다.

**호 세대**: pjsua 는 `callId` 를 순환 재사용한다(`pjsua_call.c` `alloc_call_id`). 종료된 호의 낡은 핸들이
같은 id 를 받은 **다른 호**를 끊지 않도록 핸들이 세대를 들고 있고, 세대가 지나면
`CimsResult.fail(-98, "stale call handle")` 로 떨어진다.

---

## 5. Android 접점층

Windows 접점(`sdk/windows/dotnet/CimsUe/Platform/`)의 책임을 옮긴다. 이 층은 프로토콜을 모른다.
원천 구현이 기존 앱에 있으면 그것을 옮겨 쓴다(복사가 아니라 이동 — 전환 시점에 원본이 걷힌다).

| 책임 | Windows | Android | 원천 |
|---|---|---|---|
| 오디오 라우팅 | `AudioEndpoints.cs`(WASAPI 열거·핫플러그) | `AudioRouter` — AudioManager 모드·포커스·SCO, 무전/통화 분리 출력 | `ptt-client/audio/AudioRouter.kt` |
| PTT 입력 | `HotKeys.cs`(전역 핫키) | `HwPtt` — 측면 하드키 `KeyEvent` + 벤더 브로드캐스트. 판정(`KeyMapping.classify`)은 Android 를 타지 않아 기기 없이 시험한다 | `ptt-client/HwPtt.kt`, `VendorPttReceiver.kt` |
| 자격 저장 | `CredentialStore.cs`(DPAPI) | `SecureStore` — Android Keystore 의 AES/GCM 키로 암호화해 SharedPreferences 에 암호문만 둔다(의존을 늘리지 않으려 androidx.security-crypto 를 쓰지 않는다). 복호화 실패는 예외가 아니라 `null` — 앱이 재로그인을 요구한다. 화면 잠금과 묶지 않는다(부팅 뒤 무인 재등록) | `core/account/` |
| 부팅 재등록 | `AutoStart.cs` | `BootRegister` — `BOOT_COMPLETED` | `core/boot/CimsBootReceiver.kt` |
| 등록 유지 | (데스크톱 프로세스 수명) | `UeForegroundService` — 알림·wakelock·**FGS 타입**(§6.1). 대기 중 타입은 **`specialUse`**(subtype 을 매니페스트에 선언), 캡처 중에는 `setMicrophoneActive` 가 **마이크를 더해 다시 승격**한다 — 알림만 다시 그리면 타입은 그대로다. `dataSync` 는 쓰지 않는다: Android 15+ 에서 하루 6시간 누적 제한이 걸리고 `BOOT_COMPLETED` 에서 시작할 수 없어 상주·부팅 재등록이 성립하지 않는다(기존 `ptt-client` 도 `microphone\|specialUse`). 승격 실패는 삼키지 않고 `onForegroundFailed` 로 올린다 | `ptt-client/PttService.kt` |
| 서버 인증서 만료 | `Engine.TlsPeerExpiry` → 배너 | 같은 코어 API + 설정 "서버 인증서" 행 | `core/net/TlsPeerObserver.kt` |
| 단일 인스턴스 | `SingleInstance.cs` | 해당 없음 — Android 태스크 모델 | — |

PTT 입력원은 두 플랫폼이 다르지만(전역 핫키 / 측면 하드키) **누름·해제의 동작 의미는 같다** —
누름 = 발언 대상 전부에 floor 요청, 해제 = 전부 해제. 하드키가 없는 태블릿은 화면 발언 바가 같은 일을 한다.

---

## 6. 앱 설계

### 6.1 수명주기와 소유권 — Windows 에 없는 축

데스크톱은 프로세스 하나가 앱 수명과 같지만 Android 는 Activity 가 재생성되고(회전·구성 변경) 프로세스가
백그라운드에서 회수될 수 있다. 등록·세션이 화면 수명에 묶이면 안 된다.

```
UeForegroundService  ─ 프로세스 상주. 알림·wakelock. 여기서 CimsUe 엔진을 기동·유지한다
   └ DispatchSession ─ 코어 투영 + 관제 동작 진입점 (Windows Services/DispatchSession.cs 대응)
        · Engine·CscClient 소유, 등록 백오프, 오디오 적용
        · sessions/groups/dialogs 를 StateFlow 로 낸다
   ↑ 관측
 Activity → ViewModel(androidx) → Compose
```

- **`DispatchSession` 은 Service 수명**을 산다. Activity 가 죽었다 살아나도 세션·등록은 유지된다.
  세션의 준비·교체는 `DispatchService.sessionFlow` 로 **관측 가능하게** 낸다 — 정적 필드만으로는 화면이
  Service 보다 먼저 서면 재구성이 걸리지 않아 대기 화면에 머무른다. 세션이 바뀌면 화면은 패널 VM 을 버리고
  새 세션에 다시 붙는다(옛 세션을 참조한 채 남지 않게).
- **기동은 로그인 세대로 묶는다.** 수동 로그인은 화면 수명에서 돌고 로그아웃은 Service 수명에서 오므로,
  계정 추가 중에 로그아웃하면 뒤늦은 기동이 계정과 READY 를 다시 게시할 수 있다. 로그아웃이 세대를 올리고
  기동은 세대가 바뀌었으면 게시하지 않는다(부분 생성된 계정도 정리한다).
- **초기화 절차는 수동·자동이 공유한다.** 그룹 조회·affiliation·conference 구독·dialog 감시는 `start()`
  안에 있다 — 자동 복귀(저장 자격 → `resume` → `start`)에서도 ①② 채널이 서야 하기 때문이다.
- **ViewModel 은 화면 상태만** 갖는다. 서버 상태는 전부 `DispatchSession` 의 Flow 를 접어 만든다.
- **Activity 재생성과 프로세스 회수는 다르다.** 앱이 별도 상태 기계를 갖지 않는다는 규약은 둘 다 같지만
  복원할 수 있는 것이 다르다.
  - *Activity 재생성*(회전·구성 변경): Service 와 `Engine` 이 살아 있으므로 화면은 **스냅샷 재조회**로
    그대로 복원한다 — `calls()` 에서 세션을, 열려 있던 감청도 `listenOnly` 호에서 되살린다.
  - *프로세스 회수 뒤 재시작*: `Engine` 이 새로 생기고 코어의 호 스냅샷은 메모리에만 있으므로
    **이전 `callId`·발언 상태·감청 leg 은 복원 대상이 아니다**. 앱은 저장된 자격으로 재로그인 →
    `/provisioning/me` → 계정 등록 → dialog·conference 재구독까지만 자동으로 되돌리고, 끊긴 세션은
    이력(⑤⑥)에 남긴다. 청취 재합류는 사용자가 다시 지시한다(자동 재합류는 감청을 본인 모르게 되살리는
    것이라 하지 않는다 — [dispatch_center.md](dispatch_center.md) §5 의 감사 원칙).
- 화면 VM 셋(이력·PTT 그룹·관리)은 앱 수명 동안 하나씩 유지해 폼 입력이 탭 전환으로 날아가지 않게 한다.

### 6.2 화면 구조

하단 내비 다섯([무전]·[통화]·[메시지]·[감청]·[더보기])이고, 데스크톱 최상위 넷(dispatch_desktop_ui.md §3.4)
중 [이력]·[PTT 그룹]·[관리]는 [더보기] 안으로 들어간다. 별창은 없고 같은 창의 레이어 전환이다.
요약 띠는 두지 않는다(§6.10 — 발언 바와 내비 배지가 그 자리를 채운다). 세션을 만드는 조작은 그 세션을
다룰 수 있는 화면으로 자동 복귀한다(전화를 받으면 [통화], 사설콜을 걸면 [무전]).

배치의 근거와 실측은 **§6.3** 이 정본이다.

### 6.1a 화면 VM 의 수명 — androidx `ViewModel` 을 쓰지 않는 이유

패널·화면 VM 여덟은 [MainViewModel] 이 소유한다. 이들을 androidx `ViewModel` 로 두고 **생성자로 만들어
필드에 들면 `onCleared()` 가 영영 불리지 않는다** — `ViewModelStore` 가 소유한 것만 그 콜백을 받는다.
그러면 정리(녹취 플레이어 해제·발언 해제)가 죽고 `viewModelScope` 도 취소되지 않아, 소유자가 버린 VM 의
코루틴(`stateIn(Eagerly)`·`collect`)이 계속 돈다. 로그아웃→재로그인을 되풀이하면 세션 Flow 구독이 쌓인다.

그래서 이들은 [ScreenViewModel](`AutoCloseable`)이다. 수명을 명시적으로 든다 — 소유자가 `close()` 를
부르고 그때 스코프가 끊긴다. **진짜 `ViewModel` 은 `MainViewModel` 하나**이고, Activity 의
`ViewModelStore` 가 그것을 소유하므로 그 `onCleared` 가 사슬의 뿌리다.

- **버려질 때의 해제는 세션 스코프로 보낸다.** `close()` 가 곧 자기 스코프를 끊으므로, 거기에 실은
  `floorRelease` 는 나가지 못하고 **마이크가 열린 채 남는다**. 호와 같은 수명인 세션 스코프가 끝까지 보낸다.
- **계정 격리는 «로그인 세대» 로 한다.** 세션 **객체**는 Service 수명이라 로그아웃해도 그대로다 —
  객체 교체만 보고 화면 캐시를 버리면, 다음 사람이 로그인했을 때 **앞 사람이 조회한 관리 목록·이력
  결과·편집 폼이 남는다**(화면들이 «비어야 재조회» 하므로 다시 받지도 않는다). 범위가 좁은 계정에
  앞 계정의 정보가 보이는 **인가 경계 문제**다. `logout()` 이 `loginGeneration` 을 올리고
  `MainViewModel` 이 그것까지 관측해 패널을 닫는다.
- **세션 교체는 Flow 로 처리한다.** VM 게터가 컴포지션 도중 교체(코루틴 취소·플레이어 정지·발언 해제)를
  일으키면 안 된다 — 재구성은 여러 번 일어나고 취소될 수 있다. 게터는 «없으면 만든다» 만 하고(멱등),
  버리는 일은 `DispatchService.sessionFlow` 구독이 한다. 교체가 끝나기 전에는 게터가 null 을 주어
  화면이 한 프레임 «준비 중» 에 머문다.
- **화면이 읽는 파생 값은 관측 가능하거나 순수해야 한다.** VM 의 현재 값을 읽는 헬퍼(`nameOf`·
  `serviceChoices`·`candidates()`)는 Compose 가 그 읽기를 추적하지 못해, 늦게 도착한 주소록이나
  [새로고침]한 접속서비스가 화면에 실리지 않는다. StateFlow 로 올리거나(`candidates`) 화면이 관측 중인
  값에서 계산하는 순수 함수로 바꾼다(`serviceChoicesOf`·`book.nameOf`).

### 6.1b 세션 수명 — 자격은 둘, 로그인은 하나

계약은 [dispatch_desktop_ui.md](dispatch_desktop_ui.md) §6 «세션 수명» 이 정본이다(양 플랫폼 공통).
여기서는 구현이 그것을 어떻게 지키는지만 적는다.

- **선제 갱신** — `noteTokens` 가 `TokenSet.expiresInSec` 로 만료 시각을 잡고, 만료 **60초 전**부터
  `validAccessToken()` 이 갱신한다. `Mutex` 로 한 번만 나간다 — 여러 화면이 동시에 만료를 만나도
  refresh 가 겹치지 않는다. 회전한 refresh token 도 저장한다.
- **401 복구** — 지역 시계만 믿으면 서버가 토큰을 버린 경우를 못 넘는다. `ManagementClient.send` 와
  녹취 오디오(이진 경로)가 401 을 받으면 **강제 갱신 후 1회 재시도**한다. 같은 토큰이 돌아오면
  재시도하지 않는다(갱신이 실패한 것이라 또 401 이다).
- **세션 종료** — `isSessionEnded(code, reason)` 이 `401` 과 `400 invalid_grant` 만 참이다.
  그때 `endSession()` 이 **`logout()` 을 부른다** — SIP 등록 해제·자격 폐기·로그인 화면. 네트워크
  오류(음수 코드)와 5xx 는 **로그아웃하지 않는다**: 관제석을 네트워크 흔들림으로 튕기는 것이 만료로
  튕기는 것보다 나쁘다.
- **경고 띠** — 갱신이 실패하는 동안 `credentialWarning` 이 서고, 착신 배너와 같은 레이어에 뜬다
  (화면 무관). 통화·무전은 H(A1) 이라 멀쩡하므로, 알리지 않으면 관제사는 자격이 죽어 가는 줄 모르고
  조회를 누른 순간에야 안다.
- **CSC 직접 호출도 같은 경로** — `refreshGroups`·그룹 문서 PUT/DELETE 가 쓰던 «지금 들고 있는 값»
  (`accessTokenOrNull`)을 없애고 `accessToken()`(유효 토큰)으로 바꿨다. 남겨 두면 또 쓰게 된다.

### 6.2a 착신 — 전역 표면

**착신 카드만으로는 전화를 받을 수 없다.** 카드는 [통화] 화면 안에만 있고 첫 화면은 [무전] 이라,
다른 탭·화면에 있는 동안 걸려 온 전화는 어디에도 보이지 않는다. 아무도 받지 않으면 발신자가 끊고
(CANCEL → 487), CSP 는 `487` 을 «rejected» 로 적어(`csp/CallDir.h` `_ReasonOfStatus`) 이력에 **"거절"** 로
남는다 — 벨도 안 울렸는데 거절한 것처럼 보인다.

표면을 둘 둔다. 둘 다 세션의 **착신 스택**(`DispatchSession.incoming` — 링잉 중인 전화·사설콜, 최신 위)을 본다.

| 표면 | 조건 | 내용 |
|---|---|---|
| **착신 배너** (`ui/IncomingBanner`) | 앱 화면이 보일 때 | 상단 바 아래, **화면과 무관**(관제·이력·PTT 그룹·관리 전부). 종류별 색(§3.2 — 대표번호 주황·직접 파랑·사설콜 청록) · 상대(이름 병기) · 경과 1초 갱신 · [응답] [거절]. 여러 착신은 스택 |
| **착신 알림** (`session/IncomingAlert`) | 항상 — 화면이 꺼져 있어도 | 전용 채널(`cimsue-dispatch-incoming`, IMPORTANCE_HIGH) + 전체 화면 인텐트 + 알림 액션 [응답]/[거절]. 등록 유지 알림과 **채널도 id 도 다르다** — 등록 알림은 조용해야 하고 착신 알림은 울려야 한다 |

- **벨소리는 채널 소리가 아니라 `Ringtone` 반복 재생**이다. 채널 소리는 한 번 나고 끝나 전화에 맞지 않는다.
  채널 자체는 `setSound(null)`·진동 off 로 두어 두 번 울리지 않게 한다.
- **거절은 486(Busy Here)** 이다. 603(Decline)은 서버가 포크 집합을 통째로 접을 수 있어 대표번호 병렬
  호출(TS 24.239)에서 다른 관제석까지 멈춘다.
- 배너에서 응답하면 **관제 > 일반통화 로 돌아간다**(§3.4 자동 복귀) — 보류·전달·DTMF·종료가 거기 있다.
- 매니페스트: `USE_FULL_SCREEN_INTENT`·`VIBRATE`, Activity 에 `showWhenLocked`·`turnScreenOn`.

### 6.2b 전화번호부

로그인 뒤 `GET /provisioning/directory?service=volte` 와 `?service=ptt` 를 받는다. 실패해도 진행한다 —
이름이 없으면 번호로 보일 뿐이고, 여기서 막으면 등록까지 막힌다.

**축이 둘이다. 목록은 가르고 이름은 합친다.**

| | 축 | 쓰임 |
|---|---|---|
| `phoneBook` | `service=volte` — 서버가 **이동(volte) ∪ 유선(voip) 을 합산**해 준다(csc `handle_provisioning_directory`) | 발신 대상 목록 |
| `pttBook` | `service=ptt` | PTT 그룹 멤버 후보·사설콜 대상 |
| 이름 색인 | 둘을 **합친다** | 카드 제목·⑥ 내역·대기열·착신 배너의 이름 표시 |

- **`service=voip` 로 묻지 않는다.** 그건 유선 가입자만 돌려주므로 전화번호부가 거의 빈다. 관제 회선이
  유선이라고 해서 전화번호부까지 유선만 보면 안 된다 — 관제사는 이동 가입자에게도 전화를 건다.
  전화 가족 축의 어휘는 `volte` 다(관제 그룹원 `volteAor` 와 같다).
- **발신 목록에 PTT 번호를 섞지 않는다.** PTT 번호는 MCPTT 신원이라 전화 계정으로 걸면 닿지 않는다 —
  섞으면 주소록에서 고른 사람에게 전화가 안 걸린다.
- 키는 **E.164 정규형**이다(`DirectoryBook.normalize`) — 서버의 `+8210…` 과 전화번호부의 `010…` 이 한
  사람이 되고, 내선처럼 짧은 번호(≤6자리)는 국가코드를 붙이지 않는다.
- `displayName(uri)` 이 이름을 돌려주므로 카드 제목·⑥ 내역·대기열·착신 배너가 전부 이름으로 보인다.
  `displayLabel(uri)` 는 "1003 이순경" 병기(§3.2 신원 표시).
- **표시와 동작을 가른다**([identifier_model.md](../identifier_model.md)) — ⑥ 내역 행은 표시용 `peer`(이름)와
  다시 걸 때 쓰는 `number` 를 따로 든다. 이름으로 다이얼하면 걸리지 않는다.
- **⑥ 은 두 부분이다 — 진행 중 행 + 최근 행**(dispatch_desktop_ui.md §4.4). 최근 행만 두면
  **감청 진입점이 ③ 그룹원 띠 하나뿐**이 되고, ③ 은 내 전화 그룹(`groupId == dispatch.groupId`)만
  보여 주므로 **관제 범위의 나머지 감시 대상은 `watchAll()` 이 dialog 를 구독해 놓고도 화면에 나올
  자리가 없다** — 보이지도, 감청되지도 않는다.
  - **진행 중 행** = dialog 이벤트를 세션 행으로 결합. 감시 대상 둘이 서로 통화하면 leg 이 둘 오는데
    (Call-ID 가 다르다) 통화는 하나다 — 묶지 않으면 같은 통화를 두 번 감청하게 된다
    (어느 leg 으로도 Join 할 수 있다, [dispatch_center.md](dispatch_center.md) §5.3). 정렬은 링잉 → 시작 역순.
  - **결합 조건은 넷을 모두 만족해야 한다** — ① `watched`↔`remoteIdentity` 가 서로를 가리키고,
    ② 방향이 반대이며(RFC 4235 `direction`), ③ 진행 단계가 같고, ④ 관측 시각이 근접(5초)할 것.
    **상호 번호 일치만으로 묶으면 같은 두 사람의 다른 통화가 섞인다** — A↔B 통화 중에 B 가 다시 걸면
    진행 중 leg(confirmed)과 새 착신 leg(early)이 한 행이 되고, 그 행은 `talking=true` 라 새 착신의
    [지정 픽업]이 사라지고 한쪽의 감청 상태가 다른 호로 번진다. 짝 후보가 둘 이상이면 **추측하지 않고
    홀로 둔다** — 두 줄로 보이는 편이 조작이 사라지는 것보다 낫다.
  - 조작: 링잉 → [지정 픽업], 확립 → **[청취]**(`join(dlg)` → 감청 시트). **내 통화도 행으로 보이되
    조작은 없다** — 내 전화는 착신 배너와 «내 통화» 카드에서 다룬다. 최종 인가는 서버다(범위 밖 403).
  - **구독 성립 신호를 행으로 만들지 않는다.** 코어는 dialog 가 하나도 없는 full 스냅샷을 «id·state 가
    빈» `DialogInfo` 로 낸다(`engine.cpp` — "초기 full 스냅샷에 dialog 없음"). 이걸 행으로 세우면 상태
    전이가 영영 오지 않아 «연결 중» 이 무한히 남는다. 그것은 행이 아니라 **그 AoR 에 통화가 없다는
    사실**이므로 남아 있던 행을 치운다. 나아가 행은 **아는 상태**(RFC 4235 trying/proceeding/early/
    confirmed)만 세운다 — 모르는 값은 전이가 오지 않아 사라지지 않는다.
  - **비었으면 왜 비었는지 쓴다.** 관제 편성은 서버가 정하고(`dispatch.members[]`) 앱은 받은 목록만
    구독하므로, «감시 대상 0» 은 앱 결함이 아니라 편성 문제다. 조용히 비면 둘을 가릴 수 없어
    역할 미배정 / 구성원 미편성 / **편성은 됐는데 전화 주소(`volteAor`)가 빈** 셋을 구분해 적는다.
- **⑥ 통화 내역은 콘솔·[이력] 화면과 같은 축을 든다** — 시작 · 상대(**이름과 번호를 같이**) · 종류 ·
  응답 · 종료 · **통화시간** · **울린 시간**. 통화시간은 **응답~종료**이고 못 받은 호는 0 이다 —
  울린 시간과 섞으면 «부재 3분 통화» 같은 거짓이 나온다. 이름만 두면 누군지는 알아도 다시 걸 수 없어
  번호를 함께 든다(`CallLogRow.label`).
- **⑥ 통화 내역의 권위는 내 세션이다.** 끝난 내 전화는 `applyCallState` 가 남기고, 관제 그룹원(타인)의
  통화만 dialog 이벤트(`applyDialog`)가 남긴다. dialog 로 내 통화까지 남기면 두 가지가 깨진다 —
  ① 관제 역할이 없으면 dialog 구독 자체가 없어 **내 통화가 하나도 안 남고**, ② `members[]` 의 번호가
  내 등록 회선(유선)과 다르면 내 통화가 «타인» 으로 분류돼 내역·오늘 집계에서 빠진다.
- 발신 진입점은 **시트 하나에 탭 셋**(`ui/call/DialSheet` — 다이얼패드 · 주소록 · 최근). 데스크톱의 [▦▾]
  팝오버 셋(§4.3)을 접은 것이다 — 셋 다 하는 일이 «번호를 골라 건다» 로 같다. 고르면 시트가 닫히고 바로
  발신한다. 주소록은 조직 칩(하위 포함)·이름/번호 검색, 최근은 번호가 있는 행만 보인다.

### 6.2c 요청 대상 정규화 — `tel:` 은 라우팅되지 않는다

관제 편성(`/provisioning/me`)이 내려 주는 주소는 **필드마다 형식이 다르다**:

| 필드 | 형식 | 근거 |
|---|---|---|
| `dispatch.pilotId` | `+82210001000` — **스킴 없음**(DB `phone_groups.pilot_id` 그대로) | csc `services/mcptt.py` `"pilotId": r[2]` |
| `dispatch.members[].volteAor` | `tel:+8210…` | 같은 파일 `_tel_uri(vid)` |

그런데 코어의 요청 URI 정규화는 **`tel:` 을 그대로 둔다** — `sdk/core/src/account_map.cpp` `normalizeTarget`
는 `sip:`·`sips:`·`tel:` 을 통과시키고 스킴 없는 값만 `sip:<번호>@<도메인>` 으로 만든다. `tel:` URI 는
호스트가 없어 **라우팅할 수 없는 Request-URI** 라 요청이 나가지 못한다.

**실기에서 이 차이가 그대로 드러났다** — 관제 그룹원 47명을 구독했는데 **스킴 없이 온 대표번호 1건만
성립하고 `tel:` 로 온 구성원 46건은 NOTIFY 를 하나도 못 받았다**. 그래서 ⑥ 진행 중 행이 영영 비고,
감청 버튼이 나올 자리가 없었다.

앱은 코어에 넘기기 전에 `tel:` 을 벗긴다(`routableTarget`). 이미 `sip:`/`sips:` 인 주소는 도메인이
실려 있어 그대로 둔다. **구독(`dialogWatch`)만이 아니라 감청 Join(`Engine::join`)도 같은
`normalizeTarget` 을 타므로 같은 함정에 빠진다** — dialog NOTIFY 의 `entity` 가 `tel:` 이면 Join INVITE 가
못 나간다. 두 경로 모두 정규화한다.

> **남은 판단**: 「`normalizeTarget` 이 라우팅 불가한 `tel:` 을 그대로 내는 것이 코어 결함인가」는
> 별건이다. 코어를 고치면 3개 바인딩의 모든 요청 경로가 함께 바뀌므로, CSP 가 `tel:` Request-URI 를
> 어떻게 다루는지 확인한 뒤 결정한다(§11).

### 6.2c-1 구독 수명 — 갱신은 엔진이, 종료 통지는 아직 없다

앱이 거는 이벤트 구독 셋(`dialogWatch` 감시 회선 / `subscribeConference` PTT 로스터 /
`subscribeXcapDiff` 그룹 문서)은 **엔진이 수명을 진다.** 앱은 `watchAll()`·`refreshGroups()` 를 세션 시작에
한 번 부르면 되고, 주기 재구독을 앱이 돌릴 필요가 없다.

**갱신은 자동이다.** 코어의 `Engine::dialogWatch` 등은 `pj::Account::sendRequest` 로 SUBSCRIBE 를 보내는데,
`Event:` 가 `dialog`·`conference`·`xcap-diff` 이면 pjsua 의 CIMS 인터셉터가 가로채
(`ext/pjproject/.../pjsua_acc.c` `pjsua_cims_conf_subscribe`) **pjsip 이벤트 구독(`pjsip_evsub`)** 으로 만든다.
그래서 —

- 같은 (자원, 이벤트)로 다시 부르면 새 구독이 아니라 **같은 dialog 의 갱신**이다(`pjsip_evsub_initiate`).
  `Expires: 0` 이면 해지다.
- 갱신 시점은 evsub 가 정한다 — **서버가 200 OK 로 부여한 `Expires`** 를 읽어(RFC 6665 §4.2.1.1, notifier 가
  요청보다 짧게 줄 수 있다) 만료 조금 전에 스스로 다시 건다(`evsub.c` `TIMER_TYPE_UAC_REFRESH`,
  `cims_conf_cb` 의 `on_client_refresh = NULL` = 기본 자동 갱신).

**종료 통지는 앱까지 오지 않는다 — 미이행.** 서버가 인가 회수로 구독을 끊어도
(`terminated;reason=rejected|deactivated` — [dispatch_center.md §5.10](dispatch_center.md)) CIMS 의 종료 콜백
(`pjsua_pres.c` `cims_conf_on_evsub_state`)은 로그를 남기고 슬롯만 해제한다. 코어·파사드에 그 사건을 올릴
경로가 없으므로 **앱은 화면의 낡은 행을 비우지 못하고**, `deactivated` 의 «즉시 재구독» 권고(RFC 6665 §4.1.3)도
성립하지 않는다. 회수 자체는 서버가 집행하므로 보안 구멍은 아니다. 해소는 SDK 과제
([ue_sdk.md §11](ue_sdk.md) — 종료 사유를 파사드 콜백으로 공개).

### 6.9a ④ 스레드 칩 · ⑥ 머리 필터

**④ 스레드 칩** — 메시지가 오간 스레드(그룹 + 1:1)를 최근 순으로 한 줄에. **이게 없으면 1:1 스레드를 열고
돌아갈 수 없다** — 사람 메뉴의 «문자(SDS)» 가 임의 키를 고정하는데(§6.2f), 목록이 없으면 «따라가기» 를
다시 켜서 포커스 채널로 튕기는 것 말고는 길이 없다. 미읽음은 **받은 것만** 센다(내가 보낸 것은 읽은 것이다).
빈 스레드는 내지 않는다.

**⑥ 머리 필터** `[전체|대표번호|부재]` — 데스크톱과 같다(§4.4). 오늘 데스크 칩과 **같은 상태**를 쓴다 —
둘로 나누면 «칩으로 건 필터» 와 «머리로 건 필터» 가 서로를 덮는다.

«대표번호» 는 종류와 **직교한 조회 축**이다(대표번호로 온 부재도 있다). 그래서 집계 칩에는 없고 머리에만
있다. 판정은 `CallLogRow.viaPilot` — 내 호는 `calledParty` 가 찼는지(재타게팅), 감시 대상의 호는 감시
AoR 이 대표번호인지로 본다. 둘을 가르는 이유는 책임이 다르기 때문이다 — 대표번호 호는 그룹 전원이 울리고
누가 받았는지가 따로 있어, 섞어 보면 «내가 놓친 것» 과 «동료가 받은 것» 이 구분되지 않는다.

**③ 그룹원 띠의 탭** = 빠른 발신 입력란에 채움(데스크톱 §4.3 «대기 → 클릭 → 입력란에 채움»), 롱프레스 =
사람 메뉴. 탭으로 곧바로 걸지 않는 것은 오조작 때문이다 — 띠는 상태를 보려고 자주 만진다.

---

### 6.3b 채널 행의 로스터 미리보기

데스크톱은 «채널 카드 2줄, **포커스 카드만 3줄**» 이고 3줄이 로스터 칩이다
([dispatch_desktop_ui.md](dispatch_desktop_ui.md) §4.1 «공통 — 채널 카드»). 태블릿도 같다 — 카드 전부에
로스터를 펴면 한 화면에 카드가 들어가지 않는다.

- **발언자는 접히지 않는다.** 칩이 자리에 다 안 들어가면 `+n` 으로 접는데, 하필 지금 말하는 사람이 접힌
  쪽에 있으면 카드가 «누가 말하는지» 를 못 보여 준다 — 그게 이 줄의 존재 이유다. `rosterPreview` 가
  발언자 → 나 → 나머지(서버 순서) 로 세운다. **같은 등급끼리는 서버 순서를 지킨다** — 갱신마다 칩이
  뒤섞이면 읽을 수 없다.
- **접속한 사람만**(`status == "connected"`). 은닉 청취자는 서버가 로스터에서 빼므로
  ([dispatch_center.md](dispatch_center.md) §5.6 `listen_visibility`) 앱이 따로 거를 것이 없다.
- 번호는 **정규형으로 맞춘다** — 로스터가 `tel:+8210…`, 전화번호부가 `010…` 이라 그대로 비교하면
  «나»·«발언 중» 이 붙지 않는다.
- **칩 롱프레스 = 사람 메뉴**(§6.2f). 데스크톱이 로스터 칩에서 사람 메뉴를 여는 자리와 같다.
**미리보기가 쓰이는 곳은 둘이다.** 채널 화면의 [로스터] 면은 `max` 를 두지 않고 **전부** 편다(§6.3a — 거기는
높이가 넉넉하다). 잘리는 것은 데스크톱의 포커스 카드 3줄뿐이고, 태블릿에서 그 자리에 해당하는 것은
**«편성 전원» 으로 가는 문**이다:

- **접힌 사람은 볼 방법이 있어야 한다.** 채널 화면 머리의 **[편성 전원]** 이 [PTT 그룹] 화면 상세로 보낸다
  (§6.12 — 전 멤버 × 발언 중·참여·미참가). 데스크톱도 같은 뜻의 자리에서 같은 곳으로 보낸다
  (`PttChannelsPanel.xaml` [로스터 전체 +n] — 툴팁 «로스터 전체는 [PTT 그룹] 화면 상세»).
  화면 밖에서 그룹을 지목하므로 **필터·검색을 [전체]로 되돌린 뒤** 고른다 — 현재 필터가 그 그룹을 걸러 내면
  선택이 곧바로 해제돼 «눌렀는데 아무 일도 안 일어난» 것이 된다.

---

### 6.2e-1 동시 청취 상한 — 자원이지 화면이 아니다

감청(통화 Join)과 PTT 청취를 **합쳐** 한 번에 몇 개까지 열 수 있는지를 설정으로 죈다(`Settings.maxListen`,
기본 4 — 데스크톱 `MaxMonitorWindows` 와 같은 값·같은 범위 1~16).

**왜 상한이 필요한가.** 화면이 좁아서가 아니다. 청취 leg 하나마다 서버에 CMP tap/멤버가 생기고, 서버에도
세션당 상한이 있다([dispatch_center.md §5.5](dispatch_center.md)). 앱이 무제한으로 열면 어느 순간부터 서버가
486 으로 거절하기 시작하는데, **관제사는 왜 안 되는지 알 수 없다.** 그래서 앱이 먼저 막고 이유를 말한다 —
데스크톱도 같은 자리에서 막는다(`Services/DispatchSession.cs` `JoinMonitor`·PTT 청취).

- **둘을 합쳐 센다** — 상한의 근거가 자원이고, 서버에서는 어느 쪽이든 청취 leg 하나다
  (`DispatchSession.listenCount` = 살아 있는 `PHONE_MONITOR` + `PTT_LISTEN`).
- **진입점 둘 다에서 본다** — `joinMonitor`(통화 감청)·`listenGroup`(PTT 청취).
- **② 머리에 «동시 청취 n/상한»** 을 항상 띄우고 상한에 닿으면 붉게 한다. 누르기 **전에** 보이는 유일한
  자리다(데스크톱 `ScopedChannelsViewModel.ListeningText`).

---

### 6.2f 사람 메뉴·통합 검색 — 회선이 아니라 사람으로

서버 전화번호부는 **사람이 아니라 회선**을 준다. 같은 사람이 PTT 번호와 내선을 따로 갖고 두 축
(`phoneBook`/`pttBook`, §6.2b)에 나뉘어 들어온다. 그래서 칩 하나를 눌렀을 때 «이 사람에게 사설콜도 통화도
걸 수 있다» 를 보이려면 먼저 **사람 단위로 묶어야** 한다. 데스크톱 `PersonActionsViewModel` 의 이식이다.

**묶는 규칙**(`mergePeople` — 순수 함수, 데스크톱과 같다).

- 키는 **이름 + 조직**. 이름이 없으면(서버가 번호만 준 행) 번호가 키다 — 이름 없는 행끼리 합치면 남남이
  한 사람이 된다.
- 회선을 이미 둘 가진 항목에 같은 키가 또 오면 **번호를 키로 따로 세운다.** 동명이인이 같은 조직에 있거나
  한 사람이 회선을 셋 이상 가진 경우인데, 구별 근거가 이름·조직뿐이라 합치면 **엉뚱한 사람에게 사설콜이
  나간다.** 나누면 목록에 두 줄이 보일 뿐이다.
- **내 회선은 뺀다**(`myLineKeys` — 전화·PTT 둘 다). 한쪽만 빼면 다른 축에서 «나» 가 남아 자기에게 거는
  항목이 보인다.
- 주소록에 없는 상대도 `resolvePerson` 이 **그 번호만 가진 항목을 만들어** 준다 — 빈 메뉴보다 «통화» 하나라도
  있는 편이 낫다. 전화번호부에 있으면 내선, 없으면 PTT 로 본다(데스크톱 `Resolve` 와 같은 판정).

**행동 넷을 `MainViewModel` 이 잇는다**(데스크톱도 같은 자리다). 넷 중 셋이 [PTT] 탭 상태를 건드리므로
메뉴를 띄운 ③ 패널이 직접 처리할 수 없다.

| 행동 | 처리 | 비고 |
|---|---|---|
| 통화 | `dial` + 관제 > 일반통화 복귀 | 세션을 만드는 조작은 관제로 돌아간다(§3.4) |
| 사설콜 | `startPrivateCall` + PTT 탭 | |
| 애드혹에 추가 | `seedAdhoc` 씨앗 → PTT 탭 → 발신 시트가 열리며 그 사람을 미리 선택 | 시트는 ① 패널이 소유하는 화면 상태라 밖에서 직접 못 연다 |
| 문자(SDS) | `openThread(번호)` + PTT 탭 | 1:1 스레드는 **이미 동작한다** — `applySds` 가 `groupUri` 가 없으면 보낸 사람 번호를 키로 쓴다. 열 방법만 없었다 |

**문자(SMS)는 두지 않는다.** 데스크톱에는 있지만 외부망 게이트웨이가 서버 과제라(§11) 버튼만 만들 수 없다.

**진입점** — 태블릿에는 우클릭도 `Ctrl+K` 도 없다.

- 사람 메뉴: **롱프레스** — ③ 관제 그룹원 줄 · ③ 내 통화 카드 · ⑥ 통화 내역 행 · 주소록(다이얼 시트) 행.
  데스크톱이 사람 메뉴를 여는 자리와 같다(`CallDesk.MenuRequested` · `CallActivity.MenuRequested` ·
  주소록). 주소록 행은 **탭 = 발신, 롱프레스 = 메뉴** 다 — 주소록은 전화 축이라 탭이 통화인 것이 맞고,
  같은 사람의 PTT 행동은 메뉴로 연다.
  열린 메뉴는 «어느 줄에서 열렸는지» 를 키로 하나만 둔다 — 줄마다 boolean 을 두면 스크롤 재사용 때
  엉뚱한 줄에 붙는다.
- 통합 검색: **상단 바의 돋보기** → 바텀시트. 결과 행은 데스크톱과 같게 행동 버튼을 바로 단다(검색은
  «찾아서 곧바로 건다» 는 동작이다). 상한도 같다 — 사람 12 · 채널 6.

**검색 판정**(`searchDirectory` — 순수 함수). 사람은 이름 또는 **정규형 번호**(`010…` 질의가 `+8210…`
저장값에 걸려야 한다), 채널은 이름 또는 id. 빈 질의는 전부(상한까지) — 열자마자 목록이 보여야 무엇을 찾을
수 있는지 안다.

---

### 6.2d 호 목록·경과·동시 통화 — 데스크톱에서 그대로 온 규칙

| 규칙 | 데스크톱 근거 | 태블릿 |
|---|---|---|
| **내 통화는 살아 있는 호만** | `CallDeskViewModel.Rebuild` 가 `Sessions.Where(x => x.IsVolteCall)` 로 카드를 만들고, 끝난 세션은 `Sessions` 에서 빠진다 | `kind == PHONE_CALL && isLive` |
| **경과는 1초 틱이 갱신한다** | `Models/Sessions.cs` `Tick(now)` → `Elapsed = now - (ConnectedAt ?? StartedAt)`, `MainViewModel.Tick` 이 1초마다 부른다 | 세션의 `tick` Flow(살아 있는 세션·dialog 가 있을 때만) 를 목록 Flow 에 물린다 — 계산 속성만으로는 Compose 가 재구성하지 않는다 |
| **응답하면 기존 통화를 자동 보류** | `Services/DispatchSession.cs` — 호가 Active 가 되면 다른 활성 VolteCall 을 `Hold()`. 설정 `AutoHoldOnAnswer` 기본 **true**(`SettingsStore.cs`) | 같은 규칙·같은 기본값. 끄면 두 통화가 동시에 들려 어느 쪽에 말하는지 알 수 없다 |
| **거절은 486** | `Reject(486)` | 같다. 603 은 서버가 포크 집합을 통째로 접어 다른 관제석까지 멈춘다(TS 24.239) |
| **오늘 데스크 칩은 ⑥ 의 필터다** | `CallDeskPanel.xaml` 의 `DeskFilterCommand` → `CallActivityViewModel.Filter`(`all\|missed\|outgoing\|transfer\|monitor`) | 같은 값·같은 규칙(`keepInDesk`). **«응대» 칩은 `all`** 이다 — 데스크톱 툴팁도 "⑥ 전체" 다. 응대는 목록 대부분이라 거를 이유가 없고 칩 다섯 중 하나는 해제 자리여야 한다. 태블릿은 해제 수단이 칩뿐이라 **같은 칩 재클릭도 해제**로 둔다. 감청은 데스크톱의 `ListenStart`/`ListenEnd` 둘이 태블릿에서 `MONITOR` 하나라 그 한 종류로 판정한다 |

**구독보다 모델을 먼저 게시한다 — 두 곳 다.** 서버는 구독을 받아들이는 즉시 현재 상태를 NOTIFY 로
보낸다. 구독을 먼저 걸면 그 NOTIFY 가 «아직 목록에 없는 대상» 으로 도착해 조용히 버려진다 —
`watchAll`(dialog)과 `refreshGroups`(conference) 둘 다 같은 불변을 지킨다. 어기면 이미 진행 중이던
통화·세션이 빈 상태로 남는다.

**FGS 타입은 매니페스트 선언의 부분집합이어야 한다.** `startForeground` 에 넘기는 값이 매니페스트
`foregroundServiceType` 에 없으면 API 29+ 가 `IllegalArgumentException` 으로 거절해 서비스가 서지
못한다. API 34+ 는 `specialUse`, **API 29~33 은 `dataSync`**(`specialUse` 상수 자체가 API 34 에 생겼다),
캡처 중에는 `microphone` 을 더하므로 셋을 모두 선언한다.

**토큰은 만료를 보고 갱신한다.** CSC access token TTL 은 3600초인데(`csc` `ACCESS_TOKEN_TTL`) 관제석은
며칠씩 떠 있다. 갱신하지 않으면 **한 시간 뒤 관리·이력·그룹 API 가 전부 401** 이 된다 — SIP 등록은
H(A1) 이라 멀쩡해서 겉보기에는 정상이다. 만료 60초 전을 만료로 보고 단일 갱신(Mutex)으로 회전시키며,
회전한 refresh token 도 저장한다.

**녹취는 재생할 때마다 서버에 다시 묻는다.** 받아 둔 파일로 서버 호출을 건너뛰면 청취 범위가 회수된
뒤에도 계속 재생되고(`dispatch_recordings.py` `in_scope` → 403 `out_of_scope` 를 건너뜀) **감사 기록이
남지 않는다**(`E-AUD-016 tap_mode=recording` 은 재생 단위). 합법감청의 인가 경계를 앱 캐시가 대신할
수 없다([dispatch_center.md](dispatch_center.md) §5.7b). 로컬 파일은 이번 요청의 산출을 담는 자리일 뿐이다.

**`Engine.calls()` 는 끝난 호도 준다.** 코어는 조회·최종 통계용으로 종료된 호를 64건까지 보존한다
(`engine.cpp` `callInfos`·`pruneFinished`). 화면 복귀마다 부르는 `refreshSessions()` 가 이를 거르지 않으면
**끝난 통화가 «내 통화» 로 되살아난다** — 상태로 거른다.

### 6.2e 사설콜·애드혹 · 설정 · SDS 보관

**사설콜·애드혹**(dispatch_desktop_ui.md §4.1) — ① 머리의 [사설콜·애드혹]이 시트를 연다. 데스크톱의
팝오버 둘을 **한 시트에 탭 둘**로 접었다(§6.6): 둘 다 «PTT 주소록에서 대상을 골라 세션을 연다» 이고
다른 것은 «몇 명인가» 뿐이다(사설콜 1명, 애드혹 N명).

- **애드혹 구성 중에는 바깥 탭·뒤로 제스처로 닫히지 않는다**(§4.1) — 고른 대상이 말없이 사라지면 안 된다.
- 애드혹 id 는 **앱이 만든다** — `adhoc-<내 PTT 번호>-<epoch초>`
  ([mcptt_emergency_modes.md](mcptt_emergency_modes.md) §6, `adhoc-`·`priv-` 는 편성 그룹 예약어).
  서버에 편성이 없는 임시 세션이라 채널 영속·affiliation·로스터 구독 대상이 아니고, **참가자 목록도 앱이
  기억한다** — 카드에 «3명» 을 적을 근거가 개설할 때 실어 보낸 목록밖에 없다. 호가 끝나면 지운다.
- 전이중 사설콜(`mc_no_floor_ctrl`)은 마이크가 늘 열려 있어 **발언 대상이 되지 못한다**(카드 [음소거]로
  다룬다) — 그 규칙은 `ChannelCard.canCheck` 가 이미 지킨다.

**설정**(§7·§8) — 데스크톱은 별창이지만 태블릿에는 별창이 없다. 상단 바 사람 메뉴에서 여는 시트다.
**저장 버튼을 두지 않는다** — 항목마다 즉시 저장·즉시 적용한다. «바꿨는데 저장을 안 눌러서 안 먹은»
상태를 관제석에 만들지 않는다. 오디오 경로처럼 지금 반영해야 하는 것은 `updateSettings` 가 다시 건다.

- 접속점(CSC 주소·포트)은 **읽기 전용**이다 — 등록·구독이 붙어 있는 동안 바꾸면 세션이 어긋난다.
  바꾸려면 로그아웃 후 로그인 화면에서 정한다.
- 인증서 검증을 끄면 경고를 띄운다. 로그 수준·검증 여부는 엔진 기동 때 읽으므로 다음 로그인부터다.
- `Settings` 는 **관측 가능**해야 한다(`SettingsStore.flow`) — 설정 화면이 바꾼 값을 그 화면이 다시
  그려야 하고, 잠금 발언·자동 보류처럼 다른 화면이 읽는 값도 즉시 따라야 한다.

**SDS 보관**(§4.4) — `messages.db`(SQLite), 기본 30일. 보관하지 않으면 앱을 껐다 켤 때마다 스레드가
통째로 사라지고, **서버에 SDS 이력 API 가 없으므로 이 로컬 보관이 유일한 근거**다.

- **Room 을 쓰지 않는다** — 표 하나·질의 넷이라 애노테이션 처리기와 의존을 늘릴 값이 없다
  (`SettingsStore` 가 DataStore 대신 SharedPreferences 를 쓴 것과 같은 판단).
- **기동 때 잔존 PENDING 을 FAILED 로 닫는다**([mcdata_messaging.md](mcdata_messaging.md) §5,
  데스크톱 `FailPending` 과 같은 규약) — 앱이 죽는 순간 보낸 것은 최종 응답을 받을 길이 없다.
  영원히 «보내는 중» 으로 두면 갔는지 안 갔는지 알 수 없으므로 실패로 닫아 재전송을 유도한다.
- DB 작업은 **IO 로 보낸다** — 이벤트 처리는 Main 에서 도는데 거기서 디스크를 만지면 프레임이 밀린다.
- 로그아웃은 메모리만 비우고 **보관은 남긴다** — 같은 관제석에 다시 로그인하면 스레드가 이어져야 한다.
  사람이 바뀌는 자리의 격리는 별건이다(§11).

### 6.3 화면 구조 — 모바일 앱으로 짠다

**데스크톱 격자를 줄여 넣지 않는다.** 데스크톱은 1920×1080 한 장에 6패널을 동시에 편다
(dispatch_desktop_ui.md §3.1). 태블릿 본문은 가로 1280×800 에서 상단 바 56 + 발언 바 80 + 하단 내비 56 을
뺀 **608dp** 로, 데스크톱 면적의 38% 다. 같은 격자를 그대로 접어 넣었더니 실제로 이렇게 됐다.

| 옛 배치(4분할) | 실측 |
|---|---|
| ① 내 채널 | 307dp 를 반쪽 폭으로 → 카드 한 장 114dp(조작 버튼이 카드 안에 있어 컸다) → **2.7장** |
| ④ PTT 메시지 / ⑤ 이벤트 | 각각 245dp → **6줄** |

관제사가 채널을 훑지도, 기록을 읽지도 못한다. **원인은 둘** — 화면을 동시에 네 칸으로 쪼갠 것과, 행이
조작 버튼을 안고 있어 두 배로 큰 것. 그래서 배치를 다시 짠다.

**원칙 넷**

1. **한 화면은 한 가지 일만 한다.** 동시에 보여야 하는 것만 남기고 나머지는 이동해서 본다.
2. **이동은 하단 내비 + 좌우 스와이프 두 가지로만.** 패널 분할은 쓰지 않는다.
3. **목록은 전체 폭·전체 높이를 쓴다.**
4. **예외는 발언 하나.** 관제사는 전화를 받으면서도, 이력을 보면서도 무전한다 — 발언만은 «어느 화면을
   보고 있는가» 와 무관한 조작이므로 모든 화면 하단에 상시로 둔다. 바는 80dp(PTT 버튼 150×64 + 여백)로
   **줄이지 않는다** — 가장 자주·가장 급하게 누르는 조작이라 목표가 커야 한다.

```
┌──────────────────────────────────────────────────────────┐ 56  상단 바(신원·등록·검색·세션 메뉴)
├──────────────────────────────────────────────────────────┤
│                                                          │
│            한 가지 일 — 전체 폭 · 608dp                   │
│                                                          │
├──────────────────────────────────────────────────────────┤ 80  발언 바 (상시)
│  [무전]  [통화]  [메시지]  [감청]  [더보기]                │ 56  하단 내비
└──────────────────────────────────────────────────────────┘
```

**하단 내비 다섯 — «하는 일» 로 묶는다**(`AppScreen`). 데스크톱 최상위 넷(§3.4 관제·이력·PTT 그룹·관리)은
«관리 축» 으로 묶여 있는데, 태블릿에서는 손이 가는 빈도로 묶는 편이 낫다. 화면 **내용** 은 그대로다.

| 내비 | 내용 | 데스크톱 대응 |
|---|---|---|
| **[무전]** | 채널 목록 한 열 — «내 채널» / «범위 채널» 두 섹션 | ① + ② 를 합침 |
| **[통화]** | 좌우 스와이프 세 면 — 통화 / 그룹원 / 내역 | ③ + ⑥ |
| **[메시지]** | 스레드 칩 + 대화 + 입력(전 채널) | ④ |
| **[감청]** | 열린 감청·청취 전부 | 감청 별창 N개 |
| **[더보기]** | 이력 · PTT 그룹 · 관리 · 설정 | F2·F3·F4 |

**①②를 한 목록으로 합치는 근거** — 둘은 소속이 다를 뿐 같은 종류(채널)다. 나눠 두면 각각 반쪽 폭·반쪽
높이라 세 장씩밖에 못 본다. 합치면 열 몇 장이 보이고, 구분은 섹션 머리로 충분하다. 필터·검색은 그대로
«범위 채널» 섹션에만 붙는다(§6.3 옛 규칙 유지).

**행이 얇은 근거** — 조작 버튼([참여][긴급][나가기])을 행에서 뺐다. 그것들은 채널을 **고른 뒤** 하는 일이라
채널 화면 머리에 있다. 행에 남는 조작은 «고르지 않고도 하는» 둘뿐이다: **발언 대상 지정**(내 채널)과
**청취 토글**(범위 채널). 두 줄 56dp → 608dp 에 **10~11장**.

> 발언 대상을 행에 남기는 것은 «포커스 ≠ 발언 대상» 이라는 불변(아래) 때문이다. 여러 채널을 잡아 두고
> 말하는 조작이므로 채널을 하나씩 열어서 지정하게 만들면 안 된다.

**불변은 그대로다** — 카드 탭 = 포커스(④⑤가 따라간다), 카드 체크 = 발언 대상. 둘은 끝까지 섞지 않는다.
바뀐 것은 포커스가 **화면 자체**가 됐다는 점뿐이다(§6.3b).

**요약 띠는 없앴다.** 관제 밖 화면에 «발언 대상·대기열·미읽음·감청 수» 를 알리던 띠(데스크톱 §3.5)인데,
발언 바가 모든 화면에 상시로 있고 나머지 수는 하단 내비의 배지가 말하므로 같은 정보를 두 번 그리는
자리가 됐다. 감청 수 → [감청] 배지, 미읽음 SDS → [메시지] 배지, 저장 안 한 폼 → [더보기] 점.

**뒤로가기**는 연 순서의 역순으로 한 겹씩 — 채널·더보기의 안쪽을 먼저 닫고, 그다음 첫 화면([무전])으로.
첫 화면에서는 **가로채지 않는다**(가로채면 앱을 벗어날 방법이 없어진다).

### 6.3a [채널] 화면 — 화면 하나가 곧 포커스다

[무전] 목록에서 채널을 누르면 열린다. 데스크톱이 ①②③④⑤ 를 펴 놓고 «포커스» 라는 개념으로 묶던 것을,
태블릿에서는 **보고 있는 채널이 곧 화면**이 되게 한다 — «지금 뭘 보고 있나» 를 헷갈릴 자리가 없다.

- **머리에 조작을 모은다**: 이름 · 참가 · 발언자 / [참여][긴급][나가기] · [발언 대상] · [청취] · [편성 전원].
  목록 행이 얇을 수 있는 이유가 이것이다.
- **아래는 좌우 스와이프 세 면** — [로스터] [메시지] [이벤트]. 셋을 동시에 펴면 각각 6줄이 되어 아무것도
  못 읽는다. 보던 면은 화면을 오가도 남는다.
- **[로스터] 면은 지금 접속한 사람 전부**다(잘리지 않는다). 순서는 목록 미리보기와 같은 규칙을 쓴다
  (`rosterPreview` — 발언자 → 나 → 서버 순서). 같은 규칙을 두 번 적지 않는다.
- «편성됐지만 지금 없는 사람» 은 여기 없다 — 그건 머리의 **[편성 전원]** 이 답한다([PTT 그룹] 상세, §6.12).
  두 질문(지금 누가 있나 / 편성이 누구인가)은 다르므로 둘 다 둔다.
- 채널이 사라지면(세션 종료·편성 제외) 화면을 강제로 닫지 않고 그 사실을 적는다 — 보던 것이 말없이
  사라지는 편이 더 나쁘다.

### 6.4 데스크톱 전용 기능의 처분

| 기능 | 태블릿 | 이유 |
|---|---|---|
| AvalonDock 도킹·크기 조정 | **없음** — 고정 배치 | 마우스 어포던스. 태블릿은 배치가 하나다 |
| 배치 프리셋(`layout.json`) | **없음** | 도킹이 없으면 저장할 배치가 없다 |
| 화면 별창(`ScreenWindow`) | **없음** — 레이어 전환 | Android 에 별창 개념이 없다 |
| 감청 별창 N 개 | **전면 시트 1개 + 내부 세션 전환** (§6.5) | 창이 여럿일 수 없다 |
| 트레이 최소화 | **없음** — Foreground Service 알림 | 같은 목적을 알림이 맡는다 |
| 전역 핫키 | 하드 키보드 있으면 유지, 없으면 화면 조작 | §7 |

지우는 것은 **데스크톱 어포던스뿐**이고 기능(메뉴 소속·동작·응답 문구)은 건드리지 않는다.

### 6.5 감청

감청은 **하단 내비의 한 자리**다(§6.3). 열린 감청·청취를 **카드로 나란히** 담고 전환 조작(세그먼트·스와이프)을
두지 않는 것은 관제사가 «지금 몇 개를 듣고 있는지» 를 한눈에 봐야 하기 때문이다. 열린 수는 그 내비 항목의
배지가 말한다.

전에는 어느 화면 위에나 뜨는 전면 시트였다. 시트는 «잠깐 보고 닫는» 표면인데 감청은 **일하는 동안 계속 열려
있는 것**이라 맞지 않았다 — 시트를 닫으면 청취는 계속되는데 화면에서는 사라져 상태를 놓친다.

- VoLTE 감청 = `a=ssrc … label`(RFC 5576)로 얻은 두 소스를 **줄 둘로 귀속 표시**한다(dispatch_center §5.4).
  소스가 갈라져 오지 않으면 "소스 라벨이 아직 없습니다" 로 한 줄만 보인다 — 없는 것을 지어내지 않는다.
- PTT 청취 = `listenOnly` 그룹콜. 청취 중 발언 버튼은 Floor Taken 의 `permissionToRequest=0` 으로 비활성이고,
  카드에 "발언 요청 불가 — 청취 전용 합류입니다" 를 고정 문구로 둔다.
- **시트를 닫아도 청취는 계속된다.** 끝내는 것은 [청취 종료]뿐이다(데스크톱의 별창 최소화와 같은 의미).
  마지막 하나가 끝나면 시트와 칩이 함께 사라진다.
- 레벨 미터는 `MediaSource.active` 로만 켠다 — SSRC 별 수신 레벨 관측 API 가 아직 없다(§11).

### 6.6 팝오버의 번역

데스크톱의 비모달 `Popup` 을 두 갈래로 나눈다.

| 데스크톱 팝오버 | 태블릿 | 이유 |
|---|---|---|
| 사설콜·애드혹 구성, 주소록, 다이얼패드, 문자 스레드 | **바텀시트** | 내용이 크고 검색·목록이 붙는다 |
| DTMF 3×4, 전달(blind/attended) | **카드 인라인 확장** | 한 통화에 묶인 조작이라 맥락이 보여야 한다 |
| 채널 편집(`GroupEdit`) | **전면 시트** — [PTT 그룹] 화면과 폼 공유 | 데스크톱은 오른쪽 드로어 |
| 사람 메뉴 | 롱프레스 → 앵커 메뉴 | 우클릭 대응 |
| 통합 검색(Ctrl+K) | 상단 바 검색 → 전면 시트 | 하드 키보드가 있으면 Ctrl+K 도 받는다 |

### 6.7 상태 소유

패널 경계가 곧 상태 소유 경계다. Windows 판의 분해를 그대로 따른다.

| 패널 | 소유 상태 | 소스 |
|---|---|---|
| 발언 바 | 발언 대상 집합, 대상별 승인/대기/거부, 최소 잔여 | `onFloor` |
| ① 내 채널 | 채널 카드 집합, 포커스 | `calls()` + `onGroupCall`·`onRoster` |
| ② 범위 채널 | 청취·관리 범위 카드, 필터·검색 | `Profile.dispatch.pttTargets[]` + conference 구독 |
| ③ 일반통화 | 그룹원 띠, 대표번호 대기열, 내 통화, 오늘 데스크 | `onDialogInfo`(BLF) + 로컬 호 |
| ④ PTT 메시지 | 스레드·말풍선·disposition | `onSds` + 발신 token 상관 |
| ⑤ PTT 이벤트 | 이벤트 링 버퍼(진행 중 행 없음) | `onFloor`·`onGroupCall` + 이력 폴링 |
| ⑥ 통화 내역 | 세션 행 + 최근 기록 | dialog 쌍 결합 + 이력 폴링 |

**UI 는 코어 상태의 투영이다.** 진행 중 상태는 구독(dialog/conference)이 정본이고, 끝난 것만 서버 통합 이력
(`GET /provisioning/history`, 2.5초 커서 폴링)이 채운다 — **폴링이 live 를 대체하지 않는다**.

### 6.8 Windows ViewModel ↔ 태블릿 대응

화면이 적은 만큼 VM 도 적다. **합친 곳은 상태가 하나이기 때문**이고, 나눈 곳은 수명이 다르기 때문이다.

| Windows VM | 태블릿 | 비고 |
|---|---|---|
| `MainViewModel` | `MainViewModel` | 최상위 화면·탭 소유 + 화면 VM 보유(세션 교체 시 재바인드) |
| `TalkBarViewModel` | **합침** → `PttChannelsViewModel` | 발언 대상·floor 가 한 상태라 나누면 두 벌이 된다. 바는 `TalkBar` 컴포저블 |
| `PttChannelsViewModel`·`ScopedChannelsViewModel` | 그대로 | |
| `PttOriginateViewModel`·`CallOriginateViewModel` | **합침** → 각 패널 VM | 발신은 패널 안 한 줄이라 VM 을 따로 두지 않았다 |
| `CallDeskViewModel` | 그대로 | 2열 배치(§6.3) |
| `McDataMessagesViewModel`·`SmsMessagesViewModel` | `PttMessagesViewModel`(SDS) — [메시지] 화면과 채널 화면의 «메시지» 면이 같이 쓴다 | SMS/LMS 는 서버 게이트웨이 과제(§11) |
| `PttActivityViewModel` | 그대로 | `CallActivityViewModel` 은 `CallDeskViewModel.callLog` 로 |
| `SessionHistoryViewModel` | `HistoryViewModel` + `SegmentPlayer` | `MediaElement` → `MediaPlayer`(미디어 스트림) |
| `GroupAdminViewModel`+`GroupEditViewModel` | **합침** → `PttGroupsViewModel`(+`EditForm`) | 편집이 같은 자리의 인라인 폼이라 수명이 하나다(§6.12) |
| `DirectoryAdminViewModel` | `AdminViewModel` | |
| `MonitorWindowViewModel` | **없음** — `MonitorScreen` 이 세션을 직접 투영 | 창당 상태가 없다(하단 내비의 한 화면, §6.5) |
| `DispatchSummaryViewModel` | **없음** — 요약 띠 자체를 두지 않는다 | 발언 바와 내비 배지가 같은 것을 말한다(§6.10) |
| `PersonActionsViewModel` | `PersonMenu`·`SearchSheet` + `MainViewModel.runPersonAction` | VM 을 두지 않았다 — 사람 목록은 ③ VM(`CallDeskViewModel.people`)이 이미 묶고, 메뉴·검색은 자기 상태가 없다(§6.2f) |
| `DeskViewModel` | **없음** — 상단 바가 직접 그린다 | 데스크톱에서는 상단 바 VM(§3.2 — 신원·등록 점등·오디오 요약·시계·배치 프리셋)이다. 태블릿은 배치 프리셋이 없고 나머지는 상태를 가진 쪽(`MainViewModel`·`SettingsStore`)이 그대로 그려 VM 이 따로 필요 없다 |
| `LoginViewModel`·`SettingsViewModel` | `MainViewModel`·`SettingsStore` | 로그인 화면은 상태가 셋뿐이라 합쳤다 |
| — | `LayoutStore` **없음** | 도킹 없음 |

### 6.9 저장

| 데스크톱 | 태블릿 |
|---|---|
| `MessageStore`(SQLite) | Room — 같은 스키마 의미(mcdata/sms) |
| `SettingsStore`(json) | DataStore |
| `LayoutStore` | **없음** |
| `directory-cache.json` | 같은 캐시(서버 전화번호부 + ETag) |
| `ActivityLog`(링 버퍼·CSV) | 같은 링 버퍼 |
| `AppLog`(7일) | 같은 규약, 앱 전용 저장소 |
| 자격(DPAPI) | `SecureStore`(Keystore) |

주소록 병합·이름 조회의 키는 **E.164 정규형**이다. 내선은 표시 라벨이라 주소로 쓰지 않는다.

### 6.10 관제 요약 띠 — 두지 않는다

데스크톱은 관제 밖 화면 상단에 요약 띠를 둔다(dispatch_desktop_ui.md §3.5 — 발언 대상·발언 상태·[PTT]·
대기열·문자 미읽음·내 통화·감청 수·[관제로]). 태블릿에는 **없다.**

근거는 그 자리가 이미 채워졌다는 것이다 — 발언 바가 모든 화면 하단에 상시로 있고(§6.3), 나머지 수는 하단
내비의 배지가 말한다: 감청 수 → [감청], 미읽음 SDS → [메시지], 저장 안 한 폼 → [더보기] 점. 띠를 같이 두면
같은 정보를 두 번 그리게 되고, 둘이 어긋나면 어느 쪽이 맞는지 알 수 없다.

서버 인증서 만료 경고(잔여 ≤ 30일, [sip_tls_signaling.md](sip_tls_signaling.md) §8.6.2)는 띠가 없어졌으므로
**설정 화면의 «서버 인증서» 행**과 로그인 시 배너가 표면이다.

### 6.11 [이력] 화면

끝난 통화·PTT 세션의 **하루 창 조회 + 녹취 재생**(dispatch_desktop_ui.md §4.6). 서버 계약
[android_ue_provisioning.md](android_ue_provisioning.md) §3-2/§3-2a/§3-4.

- 도구줄 = 종류 세그먼트 · 날짜 ◀▶[오늘] · 검색 · 요약 · [조회]. 종류·날짜가 바뀌면 자동 조회하고 시간대
  필터는 해제된다. **미래 날짜로는 갈 수 없다**(서버 스캔이 48시간 버킷 상한이라 창은 하루다).
- 시간대 밴드 24칸 = 분포이자 필터. 정본은 서버 `hours` 고, 없으면 항목의 축 시각(통화 INVITE·PTT 세션 시작)
  으로 센다. 건수 0 칸은 누를 수 없다.
- **통화**는 상세가 따로 없어 표가 전체 폭이고, 녹취 있는 행을 고르면 표 아래 녹취 띠만 나온다.
  **PTT** 는 좌(세션 카드 1) : 우(세션 패널 3) — 별창이 없으므로 한 화면에서 나눈다.
- 세션 패널 = 머리 · 지표 띠 · 참여자 · **발언 타임라인**(화자 레인, 막대 클릭 = 그 턴 재생) · 이벤트
  ([발언권 n]·[멤버 n] 층 토글, op 별 부가 정보) · 녹취 띠.
- **발언 턴의 원자는 녹취 세그먼트의 화자 구간**(`tracks[].speakers[]`)이다 — 동시 발언이면 같은 시각에
  슬롯이 여럿이라 막대도 여럿이다. 트랙이 없는(구형) 세그먼트는 세그먼트 자체를 한 턴으로 본다.
  재생은 `(seq, slot)` 으로 건다(`slot=null` = 믹스).
- 녹취는 `MediaPlayer` 하나로 **미디어 스트림**에 낸다 — 통화·무전과 섞이지 않게. 202(변환 중)는
  0.7→1.5초 간격 최대 120초 대기, 재변환은 새 파일 이름으로 받는다(재생 중 파일을 덮으면 깨진다).
  화면을 떠나면 재생을 멈추고 6시간 지난 임시 파일을 정리한다.
- **시간대 칸은 서버에 다시 묻는다.** 지역 필터로만 좁히면 앞 시간대가 영영 안 보인다 — 서버는 창 안에서
  **최근 `limit`(1000) 건만** 주는데 `hours` 는 **절삭 전 전체**로 낸다(csc `dispatch_history.finish_rows`).
  그래서 밴드에는 건수가 뜨는데 눌러 보면 빈 목록이고, 잘린 이력과 그 녹취에 닿을 길이 없다. 창을 그
  한 시간으로 좁혀 다시 물으면 상한 안에 들어온다. **절삭되면 그렇다고 화면에 쓴다** — 조용히 일부만
  보여 주면 «없는 통화» 로 읽힌다. 시간대를 서버가 좁히므로 지역에서 또 거르지 않는다(경계가 어긋난다).
- **타임라인 위치는 Long 으로 곱한다.** `widthDp × offsetMs` 를 Int 로 하면 **56분 지점**에서 Int.MAX 를
  넘어 음수가 되고, 음수 padding 은 Compose 가 예외로 거절해 **한 시간 넘는 세션을 여는 것만으로 앱이
  죽는다**. 결과는 0..width 로 가둔다.
- 확대·축소(Ctrl+휠·×64)는 두지 않는다 — 태블릿에는 그 입력이 없고, 가로 스크롤로 대신한다.

### 6.12 [PTT 그룹] 화면

좌(목록 1) : 우(상세/편집 3). **편집은 같은 자리의 인라인 폼**이고 별창·드로어가 아니다
(dispatch_desktop_ui.md §4.7). 편집 중에는 목록·[↻]·[+ 새 그룹]이 잠긴다 — 저장·취소로만 나온다.

- 목록 원천 = `GET /provisioning/directory/groups`(관리 범위 ∪ 내 소유 ∪ 청취 범위 ∪ 내 멤버). **보기 전용 행이
  섞인다** — [편집]·[삭제]는 서버가 내려 준 `canManage` 행에만 붙고, 필드가 없는 구 서버는 종전대로 전부 관리
  가능으로 읽는다.
- 생성·편집·삭제 = GMS XCAP PUT/DELETE(TS 24.481). 편집은 **열 때의 ETag** 를 If-Match 로 보내 412 를 잡고,
  412 면 최신 문서로 폼을 다시 연다. 신규 id 충돌(409 `uri_taken`)은 id 를 다시 만들어 한 번만 재시도한다 —
  **성공 문서의 uri 가 정본**이라 저장 뒤 선택은 응답 uri 로 맞춘다.
- 저장·삭제 뒤에는 **새 목록을 받은 다음** 선택을 옮긴다. 조회를 쏘고 바로 목록을 읽으면 옛 값이라 방금 만든
  그룹을 못 찾는다.
- 멤버 후보는 PTT 전화번호부(`/provisioning/directory?service=ptt`)다. 못 받아도 화면은 서고 이름 대신
  번호가 보인다 — 후보가 없다고 편집을 막지 않는다.
- **상세에 멤버 전원**을 편다(`detailMembers` — 데스크톱 `GroupAdminViewModel.RefreshDetailMembers` 대응).
  ① 카드 3줄이 접은 로스터의 «전체» 가 여기다(§6.3b).
  - **명단은 GMS 문서가, 상태는 로스터가** 준다 — `발언 중` / `참여` / `미참가`. 문서에 없고 로스터에만 있는
    사람은 멤버가 아니므로 나오지 않는다(이 표의 질문은 «편성된 사람이 지금 있나» 다).
  - `connected` 와 `listener` 를 **둘 다 «참여»** 로 센다 — 그 자리에 있다는 뜻은 같다. 은닉 청취자는 서버가
    로스터에서 빼므로(dispatch_center.md §5.6) 앱이 거를 것이 없다.
  - **미참가는 뒤로**, 같은 등급끼리는 문서 순서. 갱신마다 줄이 뒤섞이면 읽을 수 없다.
  - 번호 비교는 정규형으로(로스터 `tel:+8210…` vs 문서 `sip:010…`). 발언자는 표시명·번호 둘 다로 맞춰 본다.
  - 문서는 **선택할 때 한 번** 받고, 로스터·발언자가 바뀌면 같은 문서로 다시 겹친다 — 구성은 XCAP 변경으로만
    바뀌므로 상태 갱신마다 문서를 다시 받지 않는다.

### 6.13 [관리] 화면

조직 트리 | 구성원 표 | 편집 폼 세 칸(dispatch_desktop_ui.md §4.5). 서브내비는 두지 않는다(항목이 하나뿐).

- **앱은 범위 enum 을 해석하지 않는다.** 서버가 걸러 준 조직·구성원만 보이고 쓰기 판정도 서버가 한다.
  화면이 잠기는 유일한 조건은 `dispatch.directoryWrite`(전환기 `directoryAdmin`)가 없는 것이다.
- **조직은 고르는 것이지 치는 것이 아니다.** 상위 조직·소속은 트리를 평탄화한 콤보로 고른다
  (`OrgPicker`/`orgChoices` — 데스크톱 `DirectoryAdminViewModel.OrgParent` 와 같은 구성). 코드를 손으로
  치면 오타가 조용히 다른 조직에 붙거나 저장이 400 으로 떨어진다. 둘을 가른다:
  - **상위 조직**은 편집 중인 조직 **자신과 그 자손을 뺀다** — 고르면 고리가 된다. 서버가 막더라도 고를 수
    있게 두지 않는다(고르고 저장해서 실패하는 UI 는 그 자체가 결함이다). 새 조직은 자손이 없으므로 전부.
  - **범위 밖 값은 «(범위 밖)» 으로 그대로 보인다.** 목록에 없다고 «없음» 으로 그리면 저장할 때 소속이
    조용히 바뀐다 — 관리 범위가 `own` 인 관제사가 범위 밖 조직에 속한 구성원을 여는 경우다.
- **행 한 번 클릭 = 편집.** 그래서 "열림 ≠ 변경" 이다 — 열 때와 달라진 동안에만 하단 내비 [관리]에 점 배지와
  화면 머리 배지가 붙고, 변경이 있는 채로 다른 구성원을 누르면 "변경 버림" 확인을 받는다. 전환은 막지 않는다.
- 회선 카드 셋(VoLTE·VoIP·PTT). **앱이 내리는 유일한 판단은 «무엇이 바뀌었나»** 다:

  | 상황 | 보내는 것 | 이유 |
  |---|---|---|
  | 번호·서비스·transport 그대로 + 비밀번호 빈칸 | **안 보냄** | 보내면 서버가 H(A1) 재결박을 요구해 저장마다 400 |
  | 번호를 비움 | `DELETE …/{kind}` | 회선 삭제 |
  | 같은 번호 다시 실음 | **저장된 IMSI 를 그대로** | 비우면 서버가 번호 숫자로 채워 "IMSI 변경" 으로 오판 |
  | 번호가 바뀜 | IMSI 를 비우고 PUT | 새 회선이다 |
  | transport 만 바뀜 | PUT(비밀번호 불필요) | H(A1) 과 무관 |

  비밀번호가 **필수**인 것은 새 회선·번호 변경·접속서비스 변경 셋뿐이다(서버가 H(A1) 를 다시 만든다).
- **저장은 여러 요청으로 갈라지므로 부분 실패를 견뎌야 한다.** 구성원 생성 → 회선 3종 → PTT 자격이
  각각 별개 요청이다. 앞이 성공하고 뒤가 실패했을 때 폼을 그대로 두면 [저장]을 다시 누를 때 **생성이
  또 나가 같은 사람이 두 번 만들어지고**, 이미 적용된 회선을 다시 보내 «IMSI·서비스 변경» 으로 오판돼
  400 이 난다. 성공한 단계는 **그때그때 폼 기준(`orig`)에 반영**해 재시도가 남은 것만 보내게 한다.
- **PTT 자격은 PTT 회선이 있을 때만 보낸다.** 가입이 없으면 서버가 404 `Subscription not found` 로
  거절하므로(csc `dispatch_directory.py`), 전화 회선만 만든 신규 구성원은 **실제로는 생성됐는데 화면은
  «저장 실패»** 로 남는다.
- 접속서비스 후보는 종류에 맞는 것 + **저장된 값**이다. 저장값이 후보에 없어도 그대로 보인다 — 첫 후보로
  바꿔 넣으면 저장할 때마다 "서비스 변경 → 재결박" 이 난다. 후보가 0건이면 경고만 내고 개설을 막는다.
- PTT 자격은 **그룹 생성만** 쓴다. 원격 청취는 역할 배정의 결과라 표시만 하고 보내지 않는다
  (서버가 `not_editable` 로 거절한다, [mcptt_authorization.md](mcptt_authorization.md)).
- SIP transport 는 UDP/TCP/TLS/**ANY** 넷. ANY 는 "가입자 override 없음(서버 NULL)" 의 **양방향 명시값**이라
  다른 값에서 되돌릴 수 있다.

---

## 7. 입력

| 조작 | 데스크톱 | 태블릿 |
|---|---|---|
| PTT 누름/해제 | `Ctrl+Space` 전역 핫키 | **측면 하드키**(UNIWA) 우선, 없으면 발언 바 길게 누름 |
| 응답·픽업·종료 | F9·F8·F10 | 배너·카드 버튼 (하드 키보드 있으면 같은 키) |
| 화면 전환 | F1~F4 | 하단 내비 (하드 키보드 있으면 F1~F4) |
| 채널 선택·체크 | `Ctrl+n`·`Ctrl+Shift+n` | 카드 탭·체크박스 |
| 통합 검색 | `Ctrl+K` | 상단 바 검색 |
| 팝오버 닫기 | `Esc` | 뒤로 제스처·스크림 탭 (애드혹 구성 중 제외) |

하드키 누름은 화면이 꺼져 있어도 동작해야 하므로 Foreground Service 가 받는다.

---

## 8. 오디오

관제석은 무전과 통화를 다른 출력으로 낸다([ue_sdk.md](ue_sdk.md) §6.3). 책임은 둘로 나뉜다 —
어느 물리 장치로 낼지는 `AudioRouter`(AudioManager 모드·통신 장치·API 31 미만의 SCO 개시/종료),
어느 호를 어느 라우트로 보낼지는 코어(`addPlaybackRoute`·`setCallRoute`)다.

**분리는 성립한다 — 단 Windows 와 기제가 다르다.** 실측(갤럭시탭 A11 5G · SM-X236N · API 36)으로 확인한
Android 오디오 정책은 다음과 같다.

| 통화(`VOICE_COMMUNICATION`) | 무전(`MEDIA`) | 결과 |
|---|---|---|
| BT 통화(SCO) | 내장 스피커 | 실패 — 잠시 뒤 둘 다 내장 스피커 |
| BT 통화(SCO) | BT 통화(SCO) | 둘 다 헤드셋(갈라지지 않음) |
| BT 통화(SCO) | BT 음악(A2DP) | 무전이 소실 |
| 내장 스피커 | BT 통화(SCO) | 둘 다 소실 |
| **내장 스피커** | **BT 음악(A2DP)** | **갈라진다 — 통화는 스피커, 무전은 헤드셋** |

제약 셋이 맞물린 결과다. ① **SCO 는 통신 오디오를 독점**해 열리는 순간 모든 소리를 끌어간다.
② 헤드셋은 **SCO 와 A2DP 를 동시에 쓰지 못한다**. ③ `MODE_IN_COMMUNICATION` 에서는 **미디어도 통신
오디오로 취급**되므로(볼륨도 통화 스트림) 무전에 스피커를 지정하면 전역 통신 경로까지 스피커로 끌려가
SCO 가 죽는다. A2DP 만이 통신 경로 밖이라 혼자 갈라질 수 있다.

**그래서 방향은 요구(§6.3 통화=헤드셋)와 반대다.** 어느 쪽을 헤드셋으로 낼지는 현장마다 다를 수 있으므로
**설정에서 고르게 한다** — `헤드셋으로 내보낼 것: 통화 | 무전 | 분리 안 함`. 다만 선택지는 자유롭지 않다.
연결된 헤드셋 종류에 따라 실제로 되는 조합만 활성화한다(BT 는 "무전" 만 가능).

**런타임 판정은 불가능하다.** `AudioTrack.getRoutedDevice()` 는 오디오 정책이 *의도한* 장치를 보고할 뿐이라
실제와 어긋난다 — 실측에서 "SCO 로 갔다" 고 보고했지만 소리는 내장 스피커에서 났다. 따라서 앱은 관측이
아니라 **헤드셋 종류별 정책 표**로 판단한다.

코어 쪽 통로는 아직 없다 — pjmedia Android 백엔드는 장치를 하나만 노출하고(`android_get_dev_count` = 1)
`addPlaybackRoute` 로는 물리 출력이 갈라지지 않는다. 스트림별 `PJMEDIA_AUD_DEV_CAP_OUTPUT_ROUTE` 를
라우트 API 에 연결해야 한다(§11).

---

## 9. 검증

| stage | 항목 | 내용 |
|---|---|---|
| S1 | `S1-UE-FLOOR-CODEC` | `gen_floor_defs.py --check` + `cimsue_test` floor 교차 검증 |
| S1 | `S1-UE-UNIT` | `build/bin/cimsue_test` — 코어 단위시험 (§3.3 반환형 포함) |
| S1 | `S1-UE-ANDROID-BIND` | SWIG 생성 Java 에 `SWIGTYPE_p_*` 부재 + `javac` 통과 |
| S1 | `S1-UE-ENGINE-SINGLE` | 레포에 커밋된 엔진 산출물 부재(`android/core/src/pjsua2` 없음) + `org.pjsip.**` 제공처가 `:cimsue-engine` 하나 |
| S1 | `S1-UE-SDS-XCHECK` | 코어 `sds_codec` 과 `ptt-client/mcdata/McDataCodec.kt` 의 TLV 상수·인코딩 대조 (공존 기간 드리프트 방어) |
| S1 | `S1-UE-CSC-XCHECK` | 코어 `csc_client` 와 `ptt-client/csc/CscClient.kt` 의 XCAP 경로·scope·헤더 규약 대조 |
| S1 | `S1-UE-TABLET-UNIT` | `./gradlew testDebugUnitTest` — 프로파일 파싱·포커스/발언대상 분리·발언 소유권·관리 와이어 파서(범위·전환기 이름·조직 고리)·회선 저장 판정(§6.13 표)·이력 날짜 창/시간대 밴드/발언 막대·응답 문구 사전·E.164 정규화 (JVM, 기기 불필요) |
| S3 | `S3-UE-CLI-*` | `cimsue-cli` 로 등록·그룹콜·floor·SDS·Join·픽업·전달·PTT 청취 |
| 실기기 | 태블릿 | 감청 SSRC 2개 귀속 · PTT 청취 중 버튼 비활성 · 대표번호 착신 · 하드키 누름/해제 · 오디오 분리 출력 · 화면 밀도 · **이력 창 조회/녹취 재생** · **그룹 XCAP PUT/DELETE** · **회선 PUT 재결박** |

기기가 필요한 판정은 실기기 행으로만 둔다. 화면 점검은 디버그 시드 모드 + Compose `@Preview` 로 하고,
스크린샷 회귀 자동화는 두지 않는다(§11).

---

## 10. 응답 코드 → 화면 문구

[dispatch_desktop_ui.md](dispatch_desktop_ui.md) §9 사전을 그대로 쓴다. 문구를 새로 짓지 않으며,
보완이 필요하면 그 문서를 고친다. 관리·녹취 오류 본문(`error`) 사전도 같다.

---

## 10a. 밀도 — 글자·라벨의 단일 계약

[dispatch_desktop_ui.md §12](dispatch_desktop_ui.md) 가 «상세는 구현 시 확장한다» 고 남긴 자리다.

이 앱은 데스크톱(1920×1080·마우스)의 화면 의미론을 태블릿(1280×800·터치)으로 옮긴 것이라, 옮기는 동안
**데스크톱 밀도가 그대로 따라왔다.** 한때 `fontSize` 가 281곳에 9~26sp 열세 가지로 흩어져 있었고, 같은 모양의
작은 라벨이 화면마다 따로 정의돼 배경 투명도만 0.16·0.20·0.22 로 달랐다. 팔 길이에서 읽는 관제석 화면에서
그 아래쪽 값들은 작고, 같은 뜻의 표시가 화면마다 다르면 «같은 것인가» 가 흔들린다.

**글자 — 일곱 단계**(`ui/Density.kt` `Type`). 화면 코드는 숫자를 직접 쓰지 않는다.

| 토큰 | 값 | 쓰임 | 흡수한 값 |
|---|---|---|---|
| `micro` | 11 | 배지·보조 메타의 **최소** — 이보다 작게 두지 않는다 | 9 · 10 |
| `meta` | 12 | 칩·표 셀·타임스탬프 | 11 |
| `body` | 13 | 본문·목록 행 | 12 |
| `strong` | 14 | 카드 제목·표 머리 | 13 |
| `title` | 16 | 시트·구역 제목, 목록에서 먼저 읽히는 이름 | 14 · 15 · 16 |
| `head` | 19 | 화면 제목·배너 | 17 · 18 · 19 · 20 |
| `huge` | 26 | 다이얼패드 입력처럼 숫자 자체가 화면인 것 | 26 |

**배율 손잡이 하나** — `Type.SCALE`(기본 1.0). 기기에서 보고 한 번에 키우거나 줄인다. 0.92 면 정비 전 밀도에
가깝다. 단계별로 흩어 고치지 않는 것이 요점이다 — 밀도는 화면마다 정할 것이 아니라 **한 번 정할 것**이다.

**작은 라벨은 한 벌**(`ui/Tag.kt` `Tag`). 배경은 글자색의 옅은 면(단일 투명도), 모서리 4dp, 글자 `micro`.
누를 수 없다 — 누르는 것은 `FilterChip`·`AssistChip` 이고, 그 구분이 «이건 눌러도 되나» 의 답이다.

**URI → 번호도 한 벌**(`session/Uri.kt` `userPart`). 여섯 벌이 있었고 **셋은 서로 다르게 잘랐다**(`;user=phone`
파라미터를 떼는 것과 두는 것, 공백을 다듬는 것과 두는 것). 이 값은 «같은 사람인가» 의 열쇠라(로스터·주소록·
감시 대상 대조) 자르는 법이 갈라지면 같은 사람이 두 사람이 된다. 표기 차이까지 맞추는 것은 그 위의
`DirectoryBook.normalize` 다.

**남은 판단은 기기에서** — 터치 타깃(누를 수 있는 목록 행의 최소 높이)은 «한 화면에 몇 줄이 보이나» 와
맞바꾸는 것이라 화면을 보고 정한다. 관제 표는 읽는 일이 누르는 일보다 잦아 데스크톱처럼 촘촘한 편이 나을 수도
있다. 지금은 손대지 않았다.

## 11. 미해결 / 향후 과제

- **기존 앱 로직 전환** — `ptt-client`·`volte-client` 를 파사드로 옮기면 Kotlin 의 floor·mcdata·csc 사본
  1,772줄이 걷힌다([ue_sdk.md](ue_sdk.md) §5.3). **이식 완료 후 별도 과제**다. 엔진 이중화와 커밋 산출물은
  이 과제에서 이미 걷었고(§2.2), 공존 기간의 코드 중복은 floor(`gen_floor_defs.py --check`)·SDS·CSC 세
  대조 검사가 막는다(§9). 남은 선결 조건은 **`ue_sdk.md` §5.3 이 `PttController` 의 이전 대상지로 지목한
  코어 `domain/` 이 아직 없다**는 것이다 — `sdk/core/src` 에 해당 모듈이 없어, 전환 착수 전에 `domain` 을
  만들지 아니면 그 로직을 앱 계층에 둘지 정해야 한다.
- **다중 채널 동시 발언** — 코어에 발언 대상 집합 API(`setTalkTargets(callIds[])`, 승인된 세션 전부로 같은
  캡처 송출, 대상별 floor 이벤트)가 들어오기 전까지 발언 바는 대상 1개만 허용한다. 3GPP 에 UE 다중 그룹
  동시 발언 절차가 없어 단말 팬아웃으로 푼다(서버 변경 없음). 발언 바·칩·게이지는 집합 기준으로 만들어
  상수 하나로 열리게 둔다.
- **② 타인 세션 섹션** — 서버 계약은 확정·구현됐다([dispatch_center.md](dispatch_center.md) §5.6a).
  코어 `dialogWatch` 가 PTT 계정으로도 구독하고 `<mcptt>` 확장을 이벤트 필드로 내야 앱이 카드를 묶는다.
- **U10 관측** — `MediaSource.active/level` 에 실시간 값이 없다(SDP 라벨만 있다). 감청 시트의 레벨 미터는
  pjproject 에 SSRC 별 수신 관측 API 가 생긴 뒤 켠다.
- **무전/통화 분리 출력(Android)** — 가능함은 실측으로 확인했다(§8). 남은 것은 셋이다.
  ① 코어에 스트림별 출력 경로 통로(`PJMEDIA_AUD_DEV_CAP_OUTPUT_ROUTE` → 라우트 API) — **코어 공개 계약
  변경**이라 별도 단계로 다룬다. ② 유선·USB-C 헤드셋 측정 — SCO/A2DP 배타성이 없어 요구 방향
  (통화=헤드셋)이 될 수 있으나 **미측정**이다. ③ 헤드셋 종류별 정책 표 확정. 측정 도구는
  `android/audio-probe`(일회성 탐침, 앱 빌드와 무관).
- **MCData MSRP·FD** — 코어에 없다. 관제 앱은 쓰지 않으므로 이 앱의 차단 요인은 아니다.
- **자동 수락 분리** — `AccountConfig.autoAnswerMcptt` 가 그룹콜·사설콜 공통이다. 관제석은 그룹콜 자동 +
  사설콜 수동이 맞아 코어 플래그 분리가 필요하다.
- **대표번호로 발신** — 코어 `dial` 에 `P-Preferred-Identity` 헤더 옵션이 필요하다(서버 계약은 확정).
- **외부망 SMS/LMS** — 능력 키(`services[kind].capabilities.smsGateway`)로 버튼 활성만 판단한다.
  게이트웨이는 서버 과제.
- **코어 `normalizeTarget` 의 `tel:` 처리** — 지금은 앱이 요청 대상에서 `tel:` 을 벗겨 넘긴다(§6.2c).
  코어가 라우팅 불가한 `tel:` 을 그대로 내는 것이 결함인지, 아니면 호출자가 라우팅 가능한 형태로
  주는 것이 계약인지 정해야 한다. 코어를 고치면 Android·Windows·CLI 의 모든 요청 경로(dial·join·
  pickup·transfer·subscribe)가 함께 바뀌므로 CSP 의 `tel:` Request-URI 처리를 확인한 뒤 결정한다.
- **SDS 보관의 사용자 격리 — 방향 확정, Windows 선행 대기.** 지금은 관제석 단위로 한 `messages.db` 를 쓴다.
  교대로 사람이 바뀌는 자리에서 **앞 사람의 스레드가 보인다.** 좌표·이름이 오가는 통로라 그냥 둘 수 없다.
  **데스크톱도 같다**(`AppPaths.MessagesDb` — 설치당 하나). 즉 이식이 아니라 **양 플랫폼에 같이 넣을 새
  정책**이고, 태블릿만 먼저 고치면 두 앱이 갈라진다. 그래서 **Windows 구현을 선행**으로 두고 그 뒤에 같은
  설계로 태블릿에 넣는다(사용자 결정).

  확정한 설계 — `messages` 에 `owner` 열(로그인 주체 `login_id`)을 더하고 조회·집계에 `WHERE owner = ?`.
  보존(`prune`)·재기동 시 PENDING 마감은 owner 무관. 남은 판단 하나는 **기존 행(빈 owner)** 의 처리다:
  첫 로그인 때 그 사람에게 1회 귀속(설치당 한 사람만 쓰던 상태이므로) vs 아무에게도 보이지 않되 지우지 않음.
  전자는 업그레이드 직후 자기 대화가 사라지지 않고, 후자는 격리가 더 엄격하다.

  §10 «자리/사람 분리» 와 같은 뿌리다 — 그쪽이 먼저 서면 `owner` 는 자리가 아니라 **점유한 사람**이 된다.
- **Ringtone 반복** — `isLooping` 이 API 28+ 라 그 아래에서는 1초 주기로 다시 트는 폴백을 쓴다.
  minSdk 를 28 로 올리면 걷어낼 수 있다.
- **이력 타임라인 확대·축소** — 데스크톱의 Ctrl+휠 ×64 배율은 태블릿에 그 입력이 없어 가로 스크롤로만 둔다.
  핀치 줌으로 열 수 있으나 막대 클릭(턴 재생)과 제스처가 겹쳐 실기 확인이 필요하다.
- **화면 회귀 자동화** — 데스크톱의 `--ui-preview-shot` 에 해당하는 스크린샷 캡처(Roborazzi 등)는 두지 않는다.
  화면 판정은 실기기가 맡는다.
- **영상** — 감청 영상 격자는 Windows F3 과 함께 후속.
