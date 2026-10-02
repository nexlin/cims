# 관제조작반 Android 태블릿 앱 (`android/dispatch-tablet`)

> 관제조작반의 두 번째 플랫폼. **화면 의미론·응답 문구·조작 규약은 Windows 판과 같은 정본**
> ([dispatch_desktop_ui.md](dispatch_desktop_ui.md))을 따르고, 이 문서는 태블릿에서 **갈라지는 것**을 정한다 —
> 코어 바인딩 계약, Kotlin 파사드, Android 접점층, 수명주기 소유권, 태블릿 밀도(같은 문서 §12 확장).
> CIMS 연동은 전부 `libcimsue` 위에 있고 앱은 화면·장치·수명주기만 갖는다([ue_sdk.md](ue_sdk.md) §2).
>
> 코드는 `android/dispatch-tablet`·`sdk/android/cimsue` 에 있다. **기능과 색은 Windows 관제 앱과 같게 간다** — 레일 구성·오른쪽
> 패널(면 위에 겹침)·색 토큰·기본 테마(밝게)가 같고, 동시 발언·긴급 경보·정책 게이트(CMS)·편성 추적·파일(FD)·영상 채널(MCVideo)·[이력]·
> [PTT 그룹]·[관리] 가 같은 규칙으로 선다. 확인 범위는 §9 다(빌드·JVM 단위시험·기기 미리보기 — 실서버·실기 왕복은
> [docs/dev/dispatch_desktop_handoff.md](../../dev/dispatch_desktop_handoff.md) §5).
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
| 화면 배치 | **모바일 앱으로 짠다** — 왼쪽 레일 넷([관제]·[이력]·[PTT 그룹]·[관리]) + 바닥 [설정] · 관제 탭 한 줄([무전\|통화] + 하위 탭) · 한 면은 한 가지 일(관제의 면 여섯 장을 좌우 스와이프로 꿴다 · [통화] 의 왼쪽 고정 칸은 면을 옮겨도 남는다) · 한 대상을 자세히 볼 때만 **오른쪽 사이드 패널**(면 위에 겹침 · 폭은 끌어 조절, §6.3) | 태블릿 본문은 데스크톱 면적의 38% 다. 같은 격자를 줄여 넣으면 어느 칸도 제 몫을 못 한다 |
| 불변 규약 | **포커스(보는 채널) ≠ 발언 대상(말하는 채널)** · 응답 코드 문구 사전(§9) · 식별자는 코어가 준 id | 플랫폼이 달라도 같다 |
| 기존 앱 | **엔진은 이번에 단일화한다** — 기존 앱의 래퍼(`:core-sip`)가 `:cimsue-engine` 의 pjsua2 를 쓰고 커밋된 산출물 22.5MB 를 지운다(§2.2). 앱 로직 전환(파사드 이전·`PttController` 분해)은 이식 완료 후 별도 과제다(§11) | 엔진이 두 벌이면 `ext/pjproject` 패치를 두 곳에 반영해야 하고 커밋본이 조용히 어긋난다. 반면 로직 전환은 동작하는 앱의 회귀 위험을 이식 일정에 싣는다 |

---

## 2. 층 구조와 모듈 경로

```
android/  (Gradle 루트 — settings.gradle.kts)
  :cimsue-engine    → ../sdk/android/cimsue-engine   org.pjsip.** 바인딩 + libpjsua2.so
  :cimsue           → ../sdk/android/cimsue          코어 파사드 + libcimsue.so
  :dispatch-tablet  관제 태블릿 앱   ──→ :cimsue
  :sdk-probe        SDK 엔진 기기 점검(개발 도구, 계정 없음) ──→ :cimsue
  :core             공용 조각(계정·프로비저닝·저장소·전원…) — SIP·엔진 없음
  :core-sip         기존 앱의 자체 pjsua2 래퍼(이행용) ──→ :core + :cimsue-engine
  :ptt-client :volte-client          기존 앱 ──→ :core-sip   (SDK 이식 전 — ue_sdk.md §5.3)
  :cims                              로그인 앱 ──→ :core      (엔진을 싣지 않는다)
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
  build-native-remote.sh WSL 없는 Windows(Git Bash) → ssh 빌드 호스트(VM)에 작업 사본을 LF 트리로 보내 build-native.sh 를
                         맡기고 두 모듈의 생성물을 받아 온다(android/docs/dev_environment_setup.md §4.2). APK 는 Windows Gradle.
                         네이티브 → APK → 단말 설치를 묶은 진입점 = android/tablet.sh(같은 문서 §4.4 — 네이티브는 빌드 입력
                         지문이 바뀌었을 때만 다시 짓는다)
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

- `:cimsue-engine` 이 `org.pjsip.pjsua2` 바인딩을 독점 제공하고, 카메라 도우미 `PjCamera2`·`PjCameraInfo2`(`org/pjsip/`)도
  싣는다 — 기존 앱의 래퍼가 실제로 쓴다(`SipController.kt`·`PjLib.kt`·`CimsCall.kt`).
- 카메라 도우미는 pjmedia Android 영상 장치가 **자기 APK 안에서** FindClass 하는 앱 클래스라, pj 를 정적으로 품는 `:cimsue` 도
  빌드 때 같은 원천에서 복사해 싣는다(커밋 안 함 — `S1-UE-ENGINE-SINGLE` 이 두 모듈만 허용). 코어 제어 스레드(네이티브)에서도
  찾도록 pjlib 이 `JNI_OnLoad` 의 앱 클래스 로더를 쓴다(`pj_jni_find_class`, [ue_sdk.md](ue_sdk.md) §4.5).
- 기존 앱의 래퍼(`sip/` 의 pjsua2 파일 6개)는 `:core-sip` 이 갖고 `api(project(":cimsue-engine"))` 로 엔진을 받는다 — 패키지명은
  그대로(`com.cims.ue.core.sip`)라 앱 손코드는 바뀌지 않는다. `:core` 에는 SIP·엔진이 없다.
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

`onRoster` 는 ① 채널 카드 접속자 줄(로스터 미리보기)과 ② 범위 채널 참가자 수의 유일한 소스라, 불투명하면
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
| PTT 입력 | `HotKeys.cs`(전역 핫키) | `HwPtt` — 측면 하드키 `KeyEvent`(학습·반복, 문자 키보드의 기능 키는 측면 키로 읽지 않는다). 판정(`KeyMapping.classify`·`learnable`)은 Android 를 타지 않아 기기 없이 시험한다(§7). 화면이 꺼진 동안의 벤더 방송은 받지 않는다(§7) | `ptt-client/HwPtt.kt` |
| 자격 저장 | `CredentialStore.cs`(DPAPI) | `SecureStore` — Android Keystore 의 AES/GCM 키로 암호화해 SharedPreferences 에 암호문만 둔다(의존을 늘리지 않으려 androidx.security-crypto 를 쓰지 않는다). 복호화 실패는 예외가 아니라 `null` — 앱이 재로그인을 요구한다. 화면 잠금과 묶지 않는다(부팅 뒤 무인 재등록) | `core/account/` |
| 부팅 재등록 | `AutoStart.cs` | `BootRegister` — `BOOT_COMPLETED` | `core/boot/CimsBootReceiver.kt` |
| 망 복귀 재등록 | `App.xaml.cs` `NetworkChange`(가용성·주소 변화) → `DispatchSession.NoteNetworkChange`(2초 합침·주소 지문) → 코어 `Engine.HandleNetworkChange` | SDK 접점 `NetworkWatcher`(기본 망 콜백 + 판정 `NetworkChangeFilter`, §6.1) → `DispatchSession.handleNetworkChange` → 코어 `Engine::handleNetworkChange`(TCP/TLS 연결 종료·계정별 재등록·앞 등록이 끝난 뒤 한 번 더, [ue_sdk.md](ue_sdk.md) §4.2) | — |
| 등록 유지 | (데스크톱 프로세스 수명) | `UeForegroundService` — 알림·wakelock·**FGS 타입**(§6.1). 대기 중 타입은 **`specialUse`**(subtype 을 매니페스트에 선언), 캡처 중에는 `setMicrophoneActive` 가 **마이크를 더해 다시 승격**한다 — 알림만 다시 그리면 타입은 그대로다. `dataSync` 는 쓰지 않는다: Android 15+ 에서 하루 6시간 누적 제한이 걸리고 `BOOT_COMPLETED` 에서 시작할 수 없어 상주·부팅 재등록이 성립하지 않는다(기존 `ptt-client` 도 `microphone\|specialUse`). 승격 실패는 삼키지 않고 `onForegroundFailed` 로 올린다 | `ptt-client/PttService.kt` |
| 서버 인증서 만료 | `Engine.TlsPeerExpiry` → 배너 | 같은 코어 API(`CimsUe.tlsPeerExpiry`·`CscClient.tlsPeerExpiry`) → 배너 + 설정 «서버 인증서» 행(§6.2a-3) | `core/net/TlsPeerObserver.kt` |
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
- **Doze 는 이 서비스를 건드리지 못해야 한다.** `UeForegroundService` 가 wakelock 을 상시 잡아 CPU 를 재우지
  않고, 앱은 첫 진입의 권한 답 뒤에 배터리 최적화 예외를 요청한다(`sdk/platform/BatteryExemption.requestOnce`,
  시스템 동의 다이얼로그). 예외가 없으면 Doze 가 wakelock 을 무시하므로 둘은 세트다. `:core` 앱들과 같은
  계약([android_ue_client.md §8](android_ue_client.md#8-안드로이드-런타임-설계)).
  세션의 준비·교체는 `DispatchService.sessionFlow` 로 **관측 가능하게** 낸다 — 정적 필드만으로는 화면이
  Service 보다 먼저 서면 재구성이 걸리지 않아 대기 화면에 머무른다. 세션이 바뀌면 화면은 패널 VM 을 버리고
  새 세션에 다시 붙는다(옛 세션을 참조한 채 남지 않게).
- **계정을 올리기 전에 기기 URN·CMS 문서를 받는다.** 기기 URN = SDK 접점 `DeviceIdentity.instanceUrn`(ANDROID_ID 이름 기반 UUID — 데스크톱
  MachineGuid·ptt-client 와 같은 규칙)을 두 계정의 Contact `+sip.instance` 로 쓰고, 그 값을 MCS UE ID 로 UE initial configuration
  (TS 24.484 §7.2.1.1, 로그인 전 문서라 토큰 없음)을 받아 참여 기능 PSI 둘을 PTT 계정에 넣는다 — `mcpttServerUri`(경보 Request-URI)·
  `mcdataServerUri`(disposition 통지 Request-URI). 광고하지 않은 서비스·못 받은 문서는 비워 둔다(코어 전환기 경로). user profile
  (TS 24.484 §8.3)은 `capabilities` 로 낸다(받지 못한 문서는 허용 — UX 선차단, 최종 판정은 서버). 지금 읽는 자격은 [긴급 해제] 다(§6.3a).
- **기동은 로그인 세대로 묶는다.** 수동 로그인은 화면 수명에서 돌고 로그아웃은 Service 수명에서 오므로,
  계정 추가 중에 로그아웃하면 뒤늦은 기동이 계정과 READY 를 다시 게시할 수 있다. 로그아웃이 세대를 올리고
  기동은 세대가 바뀌었으면 게시하지 않는다(부분 생성된 계정도 정리한다).
- **망이 돌아오거나 바뀌면 코어에 알린다 — 등록 복구는 코어가 한다**(`Engine::handleNetworkChange` — 옛 TCP/TLS 연결을
  닫고 등록을 켠 계정마다 다시 등록하며, 앞 등록이 걸려 있으면 끝난 뒤 한 번 더, [ue_sdk.md](ue_sdk.md) §4.2). 태블릿은 망이 자주
  바뀐다(Wi-Fi ↔ LTE, 음영) — 등록 주소가 낡으면 갱신 주기(수 분)까지 서버가 옛 주소로 보내 착신이 사라진다. 계정마다
  REGISTER 만 다시 걸면 옛 망의 연결을 재사용하고, 진행 중 등록이 있으면 `PJSIP_EBUSY`
  로 거절돼 요청이 사라진다. 변화 판정은 SDK 접점 `NetworkWatcher`(`NetworkChangeFilter`)가 한다 = 잃었던 뒤 다시 섰거나 기본 망이
  **다른 망으로** 바뀐 것. 콜백을 걸 때 지금의 망을 심어 두어(`seed`), 등록 직후 그 망의 첫 알림·같은 망의 재알림은 거르고
  **망 없이 기동했으면 처음 서는 망**은 변화로 본다. 바뀐 뒤 늦게 오는 옛 망의 소실은 무시한다.
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

**Activity 는 구성 변경으로 다시 서지 않는다**(`configChanges` — 방향·크기·키보드·내비게이션·밀도·uiMode). 블루투스 PTT 버튼이 HID
키보드로 붙거나 떨어질 때 Activity 가 재생성되면 누르고 있던 발언·잠금 발언이 풀리고 쓰던 글이 사라진다. 테마는 시스템이 아니라 앱
설정을 따르므로 uiMode 변경에도 다시 그릴 것이 없다. [앱 종료] 때 엔진·CSC 핸들은 메인 밖에서 닫는다(등록 해제 응답을 기다리는 동안
화면이 멎지 않게).

**예외 하나가 관제석을 내리지 않는다** — 엔진 이벤트는 건마다 받아(`guarded`) 하나가 던져도 그 종류의 수집(호 상태·floor·로스터…)이
끝나지 않고, 세션·화면 VM·화면 코루틴의 미처리 예외는 로그에 적고 오류 토스트 «내부 오류 — 동작은 계속됩니다» 로 한 번 알린 뒤
이어 간다(`UnhandledGuard`, 토스트는 30초에 하나). 한 조회의 예상 밖 응답으로 프로세스가 죽으면 진행 중인 통화·무전이 전부 끊긴다 —
데스크톱의 기동 뒤 `DispatcherUnhandledException` 처리와 같은 규칙이다.

### 6.2 화면 구조

왼쪽 레일 **넷**([관제]·[이력]·[PTT 그룹]·[관리])과 바닥 [설정]이고 — 데스크톱 레일과 같은 구성이다(dispatch_desktop_ui.md §3.4) —
[관제] 안을 **[무전|통화] 세그먼트 + 하위 탭 한 줄**로 나눈다. 별창은 없고 같은 창의 레이어 전환이다. 보던 면을
떠나지 않고 한 대상(채널·채널 추가·이벤트·주소록)을 자세히 보는 것은 **오른쪽 사이드 패널**이 받는다(§6.3). 요약 띠는 두지 않는다
(§6.10 — 발언 바와 레일·탭 배지가 그 자리를 채운다). 세션을 만드는 조작은 그 세션을 다룰 수 있는 면으로 자동 복귀한다(전화를
받으면 [관제] › [통화] › «통화», 개별 통화를 걸면 [관제] › [무전] › «채널»).

**[감청]·[메시지]를 최상위에 두지 않는 것**이 데스크톱과 갈리는 지점이다. 둘 다 «그 자체로 하는 일» 이 아니라 **무전의 일이거나
통화의 일**이다 — 감청은 통화 leg, 청취는 무전 leg, SDS 는 무전 채널의 대화, SMS 는 전화 축의 대화다. 축을 따로 세우면 같은 것을
두 군데서 찾게 되고, 둘이 어긋나면 어느 쪽이 최신인지 알 수 없다. 그래서 각각 제 모드 안으로 넣는다(§6.5·§6.9a).

배치의 근거와 규칙은 **§6.3**, 시각 토큰은 **§6.3c** 가 정본이다.

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

**`MainViewModel` 의 세션 수집자는 생성자 안에서 돈다** — `Main.immediate` 라, 화면보다 서비스가 먼저 떠 있으면(부팅 자동
기동·알림에서 열기·Activity 재생성) `init` 안에서 곧바로 `rebind → closePanels` 까지 간다. 그 길이 만지는 상태는 `init` 보다
**앞에** 선언한다 — 뒤에 선언한 `val` 은 그때 아직 null 이라 수집이 예외로 죽고, 묶인 세션이 서지 않아 모든 면이 «준비 중» 에
멈춘다(차가운 기동에서는 드러나지 않는다).

### 6.1b 세션 수명 — 자격은 둘, 로그인은 하나

계약은 [dispatch_desktop_ui.md](dispatch_desktop_ui.md) §6 «세션 수명» 이 정본이다(양 플랫폼 공통).
여기서는 구현이 그것을 어떻게 지키는지만 적는다.

- **갱신은 취소돼도 끝까지 간다**(`NonCancellable`) — 서버는 갱신 요청으로 옛 refresh token 을 폐기(회전)한다. 화면이 조회를 끊으며
  ([이력] ◀◀·새로고침) 그 결과를 버리면 다음 갱신이 옛 토큰으로 나가 `invalid_grant` → 통화·무전까지 끊기는 전체 로그아웃이 된다.
  받은 토큰은 그 자리에서 적는다.
- **회전한 refresh token 의 보관** — «자동 로그인» 이고, 같은 로그인이거나 저장분이 방금 쓴 그 토큰일 때 적는다. 갱신 도중
  [앱 종료](자격을 남기는 로그아웃)가 끼어들어도 저장분이 폐기된 토큰으로 남지 않는다. 자격을 지운 로그아웃·다른 사람의 로그인은
  저장분이 달라 적지 않는다.
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
- **자동 로그인** — 로그인 화면은 주소·포트·계정을 마지막 값으로 채우고 «자동 로그인»(기본 켬)을 둔다. 켜져 있으면 refresh token
  만 `SecureStore` 에 둔다(비밀번호·H(A1) 는 저장하지 않는다 — `sipHa1` 은 매 로그인 프로파일에서 받는다). 저장된 토큰은
  **[앱 종료] 뒤에도 남아** 다음 기동(부팅 재등록 포함)이 로그인 화면 없이 이어 로그인한다(`resumeAndStart`). 지우는 것은
  [로그아웃]·자격 만료(`invalid_grant`·401)·자동 로그인을 끄고 한 로그인뿐이다(`logout(forgetLogin)` — 종료는 `false`). 기동 때
  서버에 닿지 않아 이어 로그인하지 못한 것(망 단절·5xx)은 토큰을 남기고 로그인 화면에 사유를 보인다.
- «자동 로그인» 을 **끄면 그 순간부터** 자동으로 들어가지 않는다(`hasSavedLogin` = 설정 켜짐 ∧ 토큰 있음 — 재시도도 그친다).
  저장된 로그인으로 들어가는 동안 로그인 화면은 «저장된 로그인으로 접속 중…» 을 적는다. 고급에는 «서버 인증서 검증» 과
  **«사설 CA PEM»**(앱에 든 루트에 물리지 않은 사이트의 추가 신뢰 앵커 — 검증을 끄지 않고 그 사이트만 믿는다)이 있다.
- 세션 종료 판정 = `401` 또는 `400` + `invalid_grant`|`invalid_token`(대소문자 무시). 토큰은 조회가 없어도 **1분마다 선제 갱신**을
  본다(전화 전용 계정에서도 경고 띠가 미리 선다). 경고 띠 문구 = «서버 자격 갱신 실패 · 이력·관리·PTT 그룹 조회가 곧 막힐 수
  있습니다 — 통화는 계속됩니다. 서버 연결을 확인하세요»(원문 사유는 로그).
- **자동 로그인은 될 때까지 다시 건다**(`DispatchService.autoLogin`) — 부팅 직후에는 Wi-Fi 가 아직 붙지 않았을 수 있고, 한 번
  실패로 그치면 사람이 비밀번호를 넣을 때까지 미등록이라 착신·그룹콜을 전부 놓친다. 5초에서 두 배씩 1분까지 물러나며, 망이
  돌아오면(`NetworkWatcher`) 물러남을 기다리지 않고 곧바로 다시 건다. 그치는 때 = 로그인됨 · 저장된 자격이 없어짐(폐기·만료) ·
  **사람이 로그인을 시도함**(사람이 넣은 자격이 우선이다 — `manualLoginTried`, 로그아웃하면 되돌아간다).
- **로그인 흐름은 한 번에 하나**(`loginMutex`) — 수동 로그인(`loginAndStart`)과 자동 로그인(`resumeAndStart`)이 각각 기동까지
  한 줄로 선다. 겹치면 두 흐름이 저마다 `fetchProfile → start` 를 돌아 계정이 두 벌 올라가고 앞 벌은 로그아웃해도 등록이 남는다.
  기다리는 사이 다른 쪽이 로그인을 끝냈으면 새로 하지 않는다. 수동·자동 로그인 → 기동은 둘 다 **세션 수명에서 돈다** — 화면
  수명에서 돌리면 Activity 가 닫히는 순간, 호출자 수명에서 돌리면 망 전환이 자동 로그인을 다시 거는 순간 취소돼 `STARTING` 에
  멈춘 반쪽 세션이 남거나 갱신 도중 회전한 refresh token 을 잃는다. 호출자의 취소는 기다림만 끝낸다. 그 대신 로그인의 각 단계
  (토큰 발급·갱신 → 프로파일)는 응답을 받은 뒤 로그인 세대를 본다 — 그사이 로그아웃([앱 종료])했으면 비운 상태를 되살리지 않는다.
- **기동은 단계마다 로그인 세대를 본다** — 계정 게시 뒤의 구독·조회(감시 구독 → 그룹 → xcap-diff → 주소록 → 보관 적재 → 세션
  스냅샷)는 몇 초가 걸리고 그동안 셸의 [로그아웃] 이 눌린다. 남은 단계가 그대로 돌면 비운 화면에 앞 사람의 주소록·보관 스레드가
  다시 실리고 다음 로그인이 그것을 제 것 위에 얹는다. 감시 구독 도중의 로그아웃은 실패 알림도 남기지 않는다.
- **계정은 종류마다 하나가 그 축의 계정이다** — PTT 서비스가 둘이어도 `pttAccount`·`myPttId` 는 첫 서비스다(데스크톱
  `PttService`). 나머지는 등록만 하고 로그아웃이 함께 푼다. 등록 실패 토스트는 **이 로그인이 만든 계정**의 것만 띄운다 — 엔진의
  등록 표는 지운 계정의 줄을 남기므로, 재로그인 뒤 첫 스냅샷의 낡은 실패 줄을 새 실패로 읽지 않는다.
- **낡은 CSC 핸들은 메인 밖에서 닫는다**(`closeLater`) — `CscClient.close()` 는 진행 중인 요청이 끝나기를 기다리므로(소켓 동작당
  시한 15초) 메인에서 닫으면 서버가 느릴 때 다시 로그인하는 순간 화면이 그만큼 멈춘다.
- **로그아웃 경계** — 조회 뒤에 상태를 게시하는 곳(`refreshGroups`·`refreshDispatch`·`restoreMessages`·`refreshDirectory`·
  `refreshCms`·이력 폴링·영상 맞춤)은 게시 직전에 로그인 세대를 다시 본다. 받는 사이 로그아웃했으면 버린다 — 앞 사람의 그룹이
  비운 목록에 다시 실리면 다음 로그인이 그것을 «이미 구독한 그룹» 으로 읽어 affiliation·conference 구독을 걸지 않는다(화면은
  멀쩡한데 그룹콜이 오지 않는다). 엔진 이벤트 수집자도 로그아웃 상태에서는 늦게 온 이벤트를 접지 않는다(호를 끊고 등록을
  푸는 동안의 SDS·경보·통화 종료가 다음 사람의 화면에 남지 않게).
- **CSC 직접 호출도 같은 경로** — `refreshGroups`·그룹 문서 PUT/DELETE 가 쓰던 «지금 들고 있는 값»
  (`accessTokenOrNull`)을 없애고 `accessToken()`(유효 토큰)으로 바꿨다. 남겨 두면 또 쓰게 된다.

### 6.2a 착신 — 전역 표면

**착신 카드만으로는 전화를 받을 수 없다.** 카드는 [통화] 화면 안에만 있고 첫 화면은 [무전] 이라,
다른 탭·화면에 있는 동안 걸려 온 전화는 어디에도 보이지 않는다. 아무도 받지 않으면 발신자가 끊고
(CANCEL → 487), CSP 는 `487` 을 «rejected» 로 적어(`csp/CallDir.h` `_ReasonOfStatus`) 이력에 **"거절"** 로
남는다 — 벨도 안 울렸는데 거절한 것처럼 보인다.

표면을 둘 둔다. 둘 다 세션의 **착신 스택**(`DispatchSession.incoming` — 링잉 중인 전화·개별 통화, 최신 위)을 본다.

| 표면 | 조건 | 내용 |
|---|---|---|
| **착신 배너** (`ui/IncomingBanner`) | 앱 화면이 보일 때 | 상단 바 아래, **화면과 무관**(관제·이력·PTT 그룹·관리 전부). 종류별 색(§3.2 — 대표번호 주황·직접 파랑·개별 통화 청록) · 상대(이름 병기) · 경과 1초 갱신 · [응답] [거절]. 여러 착신은 스택 |
| **착신 알림** (`session/IncomingAlert`) | 앱 화면이 **보이지 않을 때**(화면 꺼짐·다른 앱) | 전용 채널(`cimsue-dispatch-incoming`, IMPORTANCE_HIGH) + 전체 화면 인텐트 + 알림 액션 [응답]/[거절]. 등록 유지 알림과 **채널도 id 도 다르다** — 등록 알림은 조용해야 하고 착신 알림은 울려야 한다 |

- **두 표면은 겹치지 않는다.** 화면이 보이면(`UiPresence` — Activity `onStart`~`onStop`) 배너가 말하고 알림은 띄우지 않는다 —
  헤드업 알림이 화면 위쪽을 덮어 배너의 [응답]·[거절] 을 가리고 첫 누름을 삼킨다. 울리는 도중에 화면을 벗어나면 그때 띄우고,
  돌아오면 내린다. **벨소리는 어느 쪽이든 울린다.**
- **벨소리는 채널 소리가 아니라 `Ringtone` 반복 재생**이다. 채널 소리는 한 번 나고 끝나 전화에 맞지 않는다.
  채널 자체는 `setSound(null)`·진동 off 로 두어 두 번 울리지 않게 한다.
- **거절은 486(Busy Here)** 이다. 603(Decline)은 서버가 포크 집합을 통째로 접을 수 있어 대표번호 병렬
  호출(TS 24.239)에서 다른 관제석까지 멈춘다.
- 배너에서 응답하면 **[통화] › «통화» 면으로 돌아간다**(§3.4 자동 복귀) — 보류·전달·DTMF·종료가 그 카드에 있다.
  면까지 옮긴다 — «주소록»·«통화내역» 에 남으면 방금 받은 호의 카드가 보이지 않는다(사람 메뉴 [통화]·행 [재발신] 도 같은
  복귀를 쓴다).
- 매니페스트: `USE_FULL_SCREEN_INTENT`·`VIBRATE`, Activity 에 `showWhenLocked`·`turnScreenOn`.

### 6.2a-1 긴급·임박·경보 — 전역 표면

채널 행의 빨강은 [무전] 목록을 볼 때만 보인다. 다른 화면에 있는 동안 선 긴급 그룹콜을 알리는 것은
**긴급·임박 배너**(`ui/EmergencyBanner`)다 — 착신 배너와 같은 배너 자리(상단 바 아래, 화면과 무관)의 맨 위에
선다([dispatch_desktop_ui.md](dispatch_desktop_ui.md) §3.2 긴급 배너). 착신은 받으면 사라지지만 긴급은 풀릴 때까지
남는 상태라 위에 둔다. 같은 스택에 **긴급 경보 배너**(보라 — 아래)가 선다.

- **스택** = `DispatchSession.alerts`(`alertStack`) — 긴급·임박 세션을 **채널마다 하나**, 최신 위. 대상은 MCPTT
  개별 통화가 아닌 세션(그룹콜·애드혹·청취)이다 — 조건(condition)은 그룹 종류가 아니라 세션의 속성이다
  ([mcptt_emergency_modes.md](mcptt_emergency_modes.md) §1). 개별 통화는 착신 배너가 받는다(데스크톱
  `UpdateEmergencyBanner` 와 같은 경계). 서열은 긴급 › 임박 — 둘이 같이 서면 긴급이다(`SessionItem.alertKind`).
- **내용** = 종류(긴급 / 임박 위험) · 개시자(mcptt-info `mcptt-calling-user-id`, 번호 이름 병기 — 내가 건 긴급·진행 중에
  걸린 조건은 비어 있다, 아래) · 채널 이름 · 경과(조건이 선 때부터, 1초 갱신) · **[긴급 해제]/[임박 해제]**(자격이 있을 때만) ·
  **[채널로 이동]**(그 채널 상세 패널을 열고 포커스를 옮긴다 — `openChannel`).
- **[해제] = 세션 조건 하향**(TS 24.379 §10.1.1.2.1.4·§10.1.1.2.1.5 — `emergency-ind`·`imminentperil-ind` false re-INVITE,
  `cancelCondition`). 서는 조건은 서버 판정과 같은 식이다(`canCancelCondition`): 긴급 = 내가 올린 조건 ∨ user profile
  `allow-cancel-group-emergency`, 임박 = `allow-cancel-imminent-peril`(개시자 예외 없음) — 성립한 세션이고 청취 leg 이 아니며 앞선
  변경이 진행 중이 아닐 때. 받지 못한 user profile 은 허용으로 읽는다. 서버가 거절(403 — 비인가·다른 긴급 사용자 송출 중)하면 코어가
  이전 값으로 되돌리고 토스트가 해제 거절 문구를 낸다. 요청·확정은 ⑤ 에 «긴급 해제 요청»·«긴급 해제» 로 남는다.
- **색** — 긴급 = 연한 빨강 면 + 진한 빨강 글자(`emgSoft`·`emgInk`, [채널로 이동] 은 빨강 채움), 임박 = 주황 채움 + 먹 글자
  (`peril`·`onPeril`). 옅은 주황인 대표번호 착신 배너와 헷갈리지 않게 한다. **닫기가 없다** — 조건이 풀리거나 세션이
  끝나면 스스로 빠진다.
- **긴급 경보**(TS 24.379 §12.1 — SIP MESSAGE `alert-ind`)는 세션 조건과 **별개 신호**다 — 그룹 세션이 없어도 온다. 배너는
  **그룹·발신자마다 하나**(`DispatchSession.alertBanners`, 보라 — `monSoft`·`monInk`): «긴급 경보 · 개시 1021 박현장 (기관)» · 그룹
  이름 · 경과 · **[경보 해제]**(user profile `allow-cancel-emergency-alert` 일 때) · [채널로 이동] · **[닫기]**. 발신자의 취소로
  내려가고, 제3자 취소는 `originated-by` 가 가리키는 **원 경보 발신자**의 배너를 내린다(§12.1.1.3 — `alertEffectOf`). 내 경보의
  에코와 경보 없는 그룹 상태 통지(`alert-ind` 없음)는 배너가 아니다. [경보 해제] 는 경보 취소 MESSAGE 다(남의 경보 = 제3자 취소,
  §12.1.1.2 4)e)) — 서버는 발신자에게 취소를 되돌려 주지 않으므로 요청으로 배너를 내리고, 최종 응답이 403(미인가 — 서버는
  `alert-ind` true 로 경보 유지를 알린다, §12.1.3.2)이면 **되살리고** 이유를 토스트로 알린다(`applyAlertCancelResult`). [닫기] 는 이
  화면의 표시만 내린다(취소 신호를 놓쳤을 때의 탈출구 — 서버의 경보는 그대로다). 경보·해제·해제 요청·거절은 ⑤ 에 남는다.
- **경과·개시/해제**는 세션을 새로 쓰는 모든 길(`upsertSession`·`refreshSessions`)이 지나는 `withAlert` 가
  잇는다. 규칙은 순수 함수 `alertTransition` 이 갖는다 — 같은 종류면 경과를 이어 가고(스냅샷은 호 상태·미디어마다
  다시 온다), 임박 → 긴급 격상은 새 개시, 조건이 내려가면 해제. 개시·해제는 ⑤ 이벤트에 «긴급 개시 · 1003 이순경»·
  «임박 해제» 로 남는다. 세션 종료는 해제가 아니다(«세션 종료» 로 남는다).
- **행·머리도 같은 낱말**을 단다 — 긴급 = 빨강 면 + «긴급», 임박 = 옅은 주황 면 + «임박»(머리는 «임박 위험»).
  색만으로 두지 않는 것은 배너에서 본 채널을 목록에서 같은 말로 바로 찾게 하려는 것이다. 범위 채널 머리도
  긴급을 칠한다. ② 필터 «긴급» 과 정렬은 임박을 포함한다(데스크톱과 같다).
- 애드혹은 편성이 없어 이름이 없다 — id(`adhoc-<번호>-<초>`)를 보이지 않고 «애드혹» 으로 적는다(`groupNameOf`,
  데스크톱 `TitleOf` 와 같다). 표시 전용이다.
- **배너는 두 장까지 편다**(조건 배너와 경보 배너를 합쳐 최신 위 — `alertBannerStack`) — 넘치면 «긴급·임박 n건 더» 줄로 접고(누르면 펼침, 펼친 스택은 높이 상한 안에서 스크롤),
  건수는 늘 보인다. 닫기 없는 배너가 쌓여 본문·착신 배너의 자리를 다 먹으면 [채널로 이동] 을 눌러도 대응할 화면이
  없기 때문이다(`alertFold`). 한 채널에 leg 가 둘이면 처음 본 것이 남는다 — 채널의 긴급은 그때부터다.
- **[무전] › «이벤트» 면은 같은 스택을 목록 위에 고정한다**(데스크톱 ⑤ `Pinned`) — 경과 · «긴급»/«임박» · «<채널> 진행 중» ·
  [채널로]. 필터(«발언» 등)·따라가기와 무관하게 늘 선다 — 개시 행이 흘러 내려가거나 가려져도 풀리지 않은 긴급이 그 면에서
  사라지지 않게. 배너와 같은 스택·같은 경과라 둘이 다른 수를 말하지 않는다(배너가 접은 것도 여기선 다 보인다).
- **[채널로 이동] 은 [무전] › «채널» 면으로 간다** — 그 채널의 카드·행이 보이는 면 옆에 채널 상세 패널이 선다(«메시지»·«이벤트»
  를 보고 있었어도 면을 옮긴다 — `NavState.openChannel`, §6.3. 검색·[PTT 그룹] «채널로» 도 같은 규칙). 범위 채널은 목록의 필터·검색
  전 전부(`ScopedChannelsViewModel.allCards`)에서 찾는다 — 검색어가 가린 채널이 «사라졌습니다» 로 보이지 않게.
- **판정은 코어의 세션 조건**(`CallInfo.condition` — `SessionItem.isEmergency`·`alertKind`)이다. 조건은 호를 세울 때(발신 옵션·착신
  INVITE 의 mcptt-info)의 값으로 시작해, 서버가 진행 중에 보내는 조건 re-INVITE(TS 24.379 V18.6.0 — §6.3.3.1.6 긴급 그룹콜·
  §6.3.3.1.15 임박 위험 그룹콜·§6.3.3.1.10 진행 중 긴급 취소)와 합류 200 OK 의 조건으로 바뀐다([ue_sdk.md](ue_sdk.md) §4.2).
  `CallInfo.mcptt` 는 개시·착신 INVITE 의 값으로 불변이라 판정에 쓰지 않는다. 조건 변화는 호 상태와 **다른 흐름**
  (`condition`, 코어 `onMcpttCondition`)으로 오므로 세션은 거기서도 조건을 옮긴다(`applyCondition` — 조건만 옮기고, 늦게 닿은
  끝난 호의 것은 그 호를 되살리지 않는다). 그래서 진행 중 격상·해제와 «이미 긴급인 그룹에 합류» 가 배너·행·⑤ 에 선다.
  **개시자는 호를 세운 INVITE 가 그 조건을 실었을 때만** 적는다(`SessionItem.alertInitiator`) — 진행 중에 걸린 조건은 재광고가
  개시자를 싣지만(§6.3.3.1.6 2)) 코어 조건에 그 값이 없어 비운다(호 발신자를 개시자로 적지 않는다, [ue_sdk.md](ue_sdk.md) §11).
  청취 채널도 같다 — 서버가 조건 재광고 re-INVITE 를 청취 leg 에도 보낸다(성립 SDP 그대로, [mcptt_emergency_modes.md](mcptt_emergency_modes.md) §4.2).
- **긴급 개별 통화에는 전역 표면이 없다** — 이 스택에 들지 않고(착신 배너의 몫), 착신 배너는 코어가 MCPTT 를 자동
  수락해(`autoAnswerMcptt`) 잠깐만 서며 긴급 여부도 적지 않는다. 다른 화면에 있는 동안 받은 긴급 개별 통화는 ① 목록의
  빨강 카드가 유일한 표시다. 데스크톱과 같은 공백이며 자동 수락 분리(§11)와 함께 정한다.

### 6.2a-2 토스트 — 명령 실패의 사유

누른 것이 안 되면 **왜 안 됐는지**가 보여야 한다(dispatch_desktop_ui.md §3.2 토스트). 발신·당겨받기·감청·합류·청취는
결과가 SIP 응답으로 늦게 오고 화면 어디에도 자리가 없다 — 삼키면 «눌렀는데 아무 일도 없다» 가 된다.

- **자리** = 본문 위에 겹쳐 **우하단**(`ui/NoticeHost`, 셸의 `notices` 자리). 배너처럼 본문을 밀지 않는다 — 실패를 알릴
  때마다 목록이 들썩이면 보던 자리를 잃는다. 최신 위, 최대 6장(`NoticeBoard`).
- **등급** = 오류(서버·엔진이 거절 — 닫을 때까지 남는다) / 경고(앱이 먼저 막은 것 — 상한·계정 없음, 6초) / 정보(6초).
  문장은 사전(§10), 원문 코드·사유는 **▸상세**에 접어 둔다.
- **띄우는 곳**은 세션 한 곳이다(`DispatchSession.report` — 데스크톱 `Show`/`Track`): 명령의 즉시 실패(발신·당겨받기·응답·
  거절·보류·음소거·DTMF·전달·감청 합류·그룹 참여·청취·나가기·floor 요청·채널 머리 [일제 통화] 개시·SDS·SMS), **연결되지 못하고 끝난 내 발신**
  (`noteFailedAttempt` — 호가 끝날 때의 `lastCode`, 영역은 그 호를 만든 동작), **보낸 문자의 최종 거절**(말풍선 ⚠ 와 함께),
  **등록 실패**(실패로 바뀔 때만 — 계정의 자동 재시도가 실패할 때마다 쌓이면 다른 실패를 가린다).
- **세션이 스스로 알리는 것** — 계정 추가·등록 요청 실패(한 계정만 실패하면 셸은 서므로 로그인 화면의 오류 줄이 보이지 않는다),
  로그인 직후 **«관제 데스크 미배정 — 일반 소프트폰 모드»**(그룹원·대기열·감청이 비는 까닭), 전화번호부를 못 받았는데 캐시도 없을
  때, 그룹 목록 조회 실패, 회선 감시 구독 실패. 되풀이되는 실패는 이어지는 동안 **처음 한 번만** 알린다.
- **내가 거둔 호는 실패가 아니다.** 연결 전에 [나가기]·[종료] 를 누르면 SIP 는 CANCEL → `487 Request Terminated` 로 끝난다.
  세션이 «내가 끊은 호» 를 적어 두고(`noteLocalHangup` — 살아 있는 호에만) 그 호의 종료에는 토스트도 «세션 실패 487» 줄도 남기지
  않는다(⑤ 에는 «세션 종료»).
- [채널 추가] 패널의 개별 통화·애드혹 통화는 곧바로 실패하면 **패널이 그 자리에 적는다** — 토스트를 겹치지 않고 사유만 사전 문장이다.
- 등록 재시도는 앱이 따로 하지 않는다 — 코어 계정이 자동 재시도한다(첫 5초, 이후 30초 간격 — `account_map.cpp`
  `regConfig`). 데스크톱의 앱 쪽 백오프를 옮기면 REGISTER 가 두 겹으로 나간다.

### 6.2a-3 서버 인증서 만료 — 전역 표면

서버 leaf 는 잔여 60일에 자동 갱신된다 — **30일 경고가 뜨는 것 자체가 자동 갱신 실패**다([sip_tls_signaling.md](sip_tls_signaling.md)
§8.6.2). 관제사는 고칠 수 없고 운영자에게 알릴 뿐이지만, 매일 앉아 있는 사람이라 폐쇄망에서 가장 확실한 채널이다.

- **관측** = SDK 가 마지막 성공 핸드셰이크에서 본 peer 인증서 — SIP TLS(`CimsUe.tlsPeerExpiry`)·CSC HTTPS
  (`CscClient.tlsPeerExpiry`) 중 **먼저 만료되는 것**(`ServerCert.worst` — 관측 없는 쪽(평문·미접속)은 빠진다). 로그인
  (HTTPS)·TLS 등록 성공(새 핸드셰이크)·1분마다 다시 읽는다(`DispatchSession.serverCert`). 로그아웃하면 비운다 — 엔진은
  로그아웃 뒤에도 살아 있어 앞 접속의 관측을 들고 있다.
- **단계** = 서버 A-PRC-009 와 같은 임계(경고 ≤30일 / 위험 ≤7일·오늘·만료, `ServerCert.WARN_DAYS`·`CRITICAL_DAYS`), 잔여
  일수는 코어와 같은 셈(초 차이 ÷ 86400 의 몫).
- **배너**(`ServerCertBanner`) — 배너 자리(착신·자격 띠 아래), 어느 화면에서나. «서버 인증서 N일 후 만료» + 운영자에게
  그대로 전할 사실(`<host:port>` · subject · 만료일 · 자동 갱신 실패 신호 — 콘솔 알람 A-PRC-009). **닫기가 없다** — 서버가
  갱신되어 임계를 벗어나면 스스로 내린다. 경고 = 옅은 빨강 면 + 빨강 글자, 위험 = 진한 빨강 면(두 테마 모두 — 진한 쪽 토큰이
  테마마다 다르다). 긴급 배너와는 [채널로
  이동] 이 없는 한 줄 제목이라는 모양으로 갈린다. 경고 구간에 드는 순간을 로그에 남긴다. 잔여 일수는 **시간이 가며** 줄어든다 —
  관측이 그대로면 흐름이 다시 내지 않으므로 배너가 1분마다 스스로 다시 판정한다(며칠 켜 둔 관제석에서도 배너가 서고, 경고가
  위험으로 넘어간다).
- **설정 «서버 인증서» 행** — 같은 관측의 상시 표시(읽기 전용): 잔여 >30일 «N일 남음»(보조색) · ≤30일 «N일 후 만료»(호박) ·
  ≤7일·오늘·만료(빨강) + `<host:port> · 만료일`. 관측이 없으면 «—».
- 데스크톱의 관제 요약 띠 배지는 없다 — 요약 띠를 두지 않는다(§6.10). 배너가 모든 화면에 선다.

### 6.2b 전화번호부

로그인 뒤 `GET /provisioning/directory?service=volte` 와 `?service=ptt` 를 받는다. 실패해도 진행한다 —
이름이 없으면 번호로 보일 뿐이고, 여기서 막으면 등록까지 막힌다.

- **캐시** — 받은 전화번호부를 ETag 와 함께 앱 저장소(`directory-cache.json`, 서비스별 `orgs`·`entries`·`etag`)에 둔다
  (데스크톱 `directory-cache.json` 과 같은 규약). 로그인 뒤 **캐시로 먼저 그리고** 서버에는 그 ETag 로 묻는다(If-None-Match —
  304 면 그대로, 200 이면 바꾸고 캐시를 고친다). 캐시가 없으면 켜자마자 몇 초 동안 이름 대신 번호가 보인다. 손상된 캐시는 빈
  것으로 읽는다(서버가 다시 채운다). 이 API 는 사람마다 범위를 두지 않으므로(조직 전부) 캐시가 다른 사람에게 더 보이는 것이
  없다. 로그아웃은 메모리만 비우고 파일은 남긴다 — 다음 로그인도 캐시부터 선다.
- **로컬 CSV** — 서버가 아직 주지 않는 것(외부망 번호·서버에 없는 이름)을 보탠다. 설정 «주소록» [가져오기] 로 고른 파일을
  앱 저장소(`directory.csv`)에 복사해 둔다(원본 접근 권한에 기대지 않는다, [지우기] 로 뺀다). 형식은 데스크톱 `directory.csv`
  와 **같은 파일**이다: `kind,number,name,tags`(kind = ext | external | ptt | group, RFC 4180 따옴표, `#` 주석·머리줄 무시,
  모르는 kind 는 외부망). 전화 주소록에는 ext·external 줄, PTT 주소록에는 ptt 줄을 섞는다(`mergeBook`). **같은 번호가 서버에
  있으면 서버 줄이 이기고** CSV 이름은 빈 곳만 채운다 — CSV 가 서버 가입자를 외부망으로 바꾸지 못한다. CSV 에만 있는
  external 줄은 외부망으로 표시돼 문자·전달의 외부망 판정이 그 줄을 본다(`DirectoryEntry.external`, §6.2e). group 줄과
  `member` 태그는 읽지 않는다 — 태블릿의 그룹은 GMS 가, 그룹원·감시 대상은 서버 `dispatch.members[]` 가 정본이다(데스크톱의
  태그 폴백은 `members[]` 이전 서버를 위한 것이다).

**축이 둘이다. 목록은 가르고 이름은 합친다.**

| | 축 | 쓰임 |
|---|---|---|
| `phoneBook` | `service=volte` — 서버가 **이동(volte) ∪ 유선(voip) 을 합산**해 준다(csc `handle_provisioning_directory`) | 발신 대상 목록 |
| `pttBook` | `service=ptt` | PTT 그룹 멤버 후보·개별 통화 대상 |
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
- **⑥ 은 두 부분이다 — 진행 중 행 + 최근 행**(데스크톱 = [통화] «진행 중 · 관제 그룹» + «기록», dispatch_desktop_ui.md §4.3·§4.4). 최근 행만 두면
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
  - 조작: 링잉 → [지정 픽업], 확립 → **[청취]**(`join(dlg)`). 청취가 붙으면 **그 행이 펴져** 소스 귀속을
    보인다(§6.5). **내 통화도 행으로 보이되
    조작은 없다** — 내 전화는 착신 배너와 «내 통화» 카드에서 다룬다. 최종 인가는 서버다(범위 밖 403).
  - **구독 성립 신호를 행으로 만들지 않는다.** 코어는 dialog 가 하나도 없는 full 스냅샷을 «id·state 가
    빈» `DialogInfo` 로 낸다(`engine.cpp` — "초기 full 스냅샷에 dialog 없음"). 이걸 행으로 세우면 상태
    전이가 영영 오지 않아 «연결 중» 이 무한히 남는다. 그것은 행이 아니라 **그 AoR 에 통화가 없다는
    사실**이므로 남아 있던 행을 치운다. 나아가 행은 **아는 상태**(RFC 4235 trying/proceeding/early/
    confirmed)만 세운다 — 모르는 값은 전이가 오지 않아 사라지지 않는다.
  - 비었으면 «진행 중인 통화 없음» 하나만 적는다 — 구독 성립·NOTIFY 수 같은 진단은 화면에 두지 않는다(관제사가 쓸 정보가
    아니다). dialog SUBSCRIBE 의 최종 거절은 logcat 에 남는다(`applyRequestResult` — 구독 거절과 «통화 없음» 을 가리는 근거).
- **⑥ 통화 내역은 콘솔·[이력] 화면과 같은 축을 든다** — 시작 · 상대(**이름과 번호를 같이**) · 종류 ·
  응답 · 종료 · **통화시간** · **울린 시간**. 통화시간은 **응답~종료**이고 못 받은 호는 0 이다 —
  울린 시간과 섞으면 «부재 3분 통화» 같은 거짓이 나온다. 이름만 두면 누군지는 알아도 다시 걸 수 없어
  번호를 함께 든다(`CallLogRow.label`).
- **⑥ 통화 내역의 권위는 내 세션이다.** 끝난 내 전화는 `applyCallState` 가 남기고, 관제 그룹원(타인)의
  통화만 dialog 이벤트(`applyDialog`)가 남긴다. dialog 로 내 통화까지 남기면 두 가지가 깨진다 —
  ① 관제 역할이 없으면 dialog 구독 자체가 없어 **내 통화가 하나도 안 남고**, ② `members[]` 의 번호가
  내 등록 회선(유선)과 다르면 내 통화가 «타인» 으로 분류돼 내역·오늘 집계에서 빠진다.
- 발신 진입점은 **둘**이다 — «통화» 면의 **다이얼패드**와 탭 줄 **[주소록] 패널**(어느 통화 면에서든 오른쪽에 편다). 데스크톱의
  [▦▾] 팝오버 셋(§4.3)을 접되 열고 닫는 팝오버로 만들지 않았다: 관제석에서 번호를 누르는 일은 상시다. «최근» 은 «통화내역» 면이
  이미 담고 있어 따로 두지 않는다(행 롱프레스 = 사람 메뉴). 주소록은 조직 거르기(«전체» 다음 트리 순서, 하위 포함)·이름/번호 검색이고, 행의 [발신] 은
  곧바로 걸고(패널은 남는다) [문자] 는 «메시지» 면의 그 사람 스레드를 연다. 행 탭은 사람 메뉴다.
- **치는 동안 주소록이 제안한다**(데스크톱 «통화» 머리 번호칸의 제안 팝업) — 입력란 아래에 겹쳐 최대 8명: 이름에 들었거나 친
  숫자가 번호의 어느 표기(저장된 그대로·E.164·국내 로컬)에든 들었으면(`DirectoryBook.suggest` — 치다 만 `010333` 은
  정규형으로 올릴 수 없어 표기마다 본다). 행을 누르면 채우고 [발신] 은 곧바로 건다. 입력란의 포커스를 뺏지 않고(계속 칠 수
  있게), 입력이 주소록 번호와 정확히 맞으면(이름이 이미 입력란 아래에 선다) 접는다. URI(`:`)를 치는 중에는 제안하지 않는다.

**표시용 번호는 국내 표기다.** 홈 국가 번호(프로파일 `countryCode`)는 `+8210…` → `010…` 으로 보인다(`DirectoryBook.displayNumber`·
`localNumber` — 데스크톱 `DirectoryService.DisplayNumber` 와 같은 규칙: `+<cc>` 로 시작하고 그 뒤가 두 자리보다 길 때만). 국제 번호·내선·
그룹 id 는 그대로다. **표시 전용**이다 — 다이얼·비교·목록 키에는 원 번호와 정규형(`normalize`)을 쓴다. 이름이 없는 상대의 이름 자리
(`displayName`)·«번호 이름» 병기(`displayLabel`)·사람 행·주소록·관리 표·그룹 멤버·이력의 이름 아래 번호·상단 바 대표번호가 이 표기다.
«이름이 있는가» 는 `nameOrEmpty` 로 가른다(표시 문자열을 번호와 비교하지 않는다).

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

**④ 스레드 목록 → 대화** — 메시지가 오간 스레드(그룹 + 1:1)를 최근 순으로 세우고, 하나를 고르면 그
대화가 열린다. **휴대폰 메신저와 같은 구성**이고 SDS([무전]›«메시지»)·SMS([통화]›«메시지») 둘 다 같다.

데스크톱은 «포커스 채널을 따라가는» 한 창인데(§4.4), 태블릿에서 그 모양이면 **1:1 스레드를 열고 돌아갈 수
없다** — 사람 메뉴의 «문자» 가 임의 키를 고정하는데(§6.2f) 목록이 없으면 «따라가기» 를 다시 켜서 포커스
채널로 튕기는 것 말고는 길이 없다. 목록이 왼쪽에 상시로 있으면 그 문제가 사라지고, 무엇이 와 있는지도
한눈에 보인다.

- 미읽음은 **받은 것만** 센다(내가 보낸 것은 읽은 것이다). 고르는 순간 읽음으로 닫는다.
- 빈 스레드는 목록에 내지 않는다(읽음 처리 뒤 남은 껍데기).
- 이름은 주소록이 준다 — 없으면 번호 그대로 세운다(§6.2b).

**[＋ 새 대화]** — 목록 머리에 둔다. 이것이 없으면 **먼저 오기를 기다리는 수밖에 없다**: 스레드 목록은
주고받은 것만 담으므로, 처음 보내는 상대는 목록에 없고 사람 메뉴(§6.2f)를 거치지 않고서는 닿을 길이 없다.
메시지 면만 열어서는 아무것도 못 쓰는 상태가 된다.

받는 사람 고르기는 **면 둘이 같은 화면을 쓴다**(`RecipientPicker`) — 하는 일이 «후보를 훑고 하나를 골라
빈 대화를 연다» 로 같고, 다른 것은 후보의 출처뿐이다.

| | [무전] › «메시지» (SDS) | [통화] › «메시지» (SMS) |
|---|---|---|
| 후보 | **편성 그룹 + PTT 주소록 사람** | 전화 주소록 사람 |
| 직접 입력 | 없음 — 편성·주소록 밖으로 보낼 자리가 없다 | **있음** — 방금 개통된 내선은 주소록이 늦게 따라온다 |
| 고르면 | 그 키로 스레드 고정(`openTo`) | 같음 |

**그룹과 사람을 한 목록에 섞지 않는다.** 섹션을 나누고 그룹에는 «그룹» 표를 단다 — 무전 메시지는 그룹으로
보내면 편성 전원이 받고 사람으로 보내면 그 사람만 받는데, 잘못 고르면 되돌릴 수 없다. 이미 대화가 있는
상대도 후보에서 빼지 않는다(골랐을 때 기존 대화가 열리는 것이 맞는 동작이다).

**그룹 대화와 1:1 대화는 화면이 다르다** — 무전 쪽만 해당한다(문자는 1:1 뿐이다).

| | 그룹(단톡방) | 1:1 |
|---|---|---|
| 머리 표 | «그룹 전원» | «1:1» |
| 입력칸 안내 | "그룹 전원에게" | "이 사람에게" |
| 말풍선 | 보낸 사람 이름을 적는다 | 적지 않는다(상대가 하나다) |
| 발신 | `sendGroupSds`(`group-sds`) | `sendSds`(`one-to-one-sds`) |

같은 입력칸에서 «편성 전원에게» 와 «이 사람에게» 가 갈리므로, **보내기 전에** 어디로 가는지 보여야 한다.

**모양** — 왼쪽 **대화 목록(340)**: 머리 «대화 n» · [따라가기 ✓] · [＋ 새 대화], 거르기 [전체·그룹·1:1·안 읽음 n], 줄(68) =
머리글자(그룹 = 둥근 네모, 1:1 = 원) · 이름(미읽음이면 굵게)·«그룹»/«1:1» · 마지막 한 통 미리보기(«나: …»·«이당직: …»(그룹)·«…»
(1:1) — `ThreadChip.last`) · 시각(오늘이 아니면 날짜)·미읽음 수. «안 읽음 n» 의 n 은 안 읽은 **메시지** 합이고, 그 거르기는 고른 대화를
남긴다(읽음이 돼도 목록에서 빠지지 않는다). 고른 줄은 연한 남색 면 + 왼쪽 3dp 남색 띠. 오른쪽 **대화**: 머리(56) = 이름 · «그룹 전원 ·
편성 12»(남색 라벨)·«접속 7» 또는 «1:1» · [채널 정보 ›](그룹만 — 보던 대화 옆에 채널 상세 패널, §6.3) / 말풍선(받은 것 = 옅은 면
왼쪽 + 그룹이면 보낸 사람 이름 위, 보낸 것 = 남색 채움 오른쪽, 시각·전달 상태(✓·✓✓·보내는 중·실패 + [재전송])는 풍선 바깥,
파일이면 풍선 안에 클립·이름·크기·[받기]/[열기] — §6.2e «파일») / 날짜가
바뀌면 «오늘»·«어제»·«9월 20일» 칸 / **빠른 답** [확인했습니다][이동 중][도착했습니다][대기 바랍니다] — 한 번 눌러 곧바로 보낸다 /
[📎](둥근 44 — 보낼 파일 고르기, 무전 메시지만) · 입력칸(알약 44, «그룹 전원에게 (순찰1 · 12명)») · [보내기]. 새 글이 오면 맨 아래로 내린다.
**열어 둔 대화로 온 글은 곧바로 읽음**이다 — «메시지» 면이 보이고 화면이 켜져 있을 때만(`PttMessagesViewModel.setVisible` — 면은 좌우로
넘기는 여섯 장 중 하나라 셸이 알려 준다. 안 보이는 동안 온 글까지 읽음으로 닫으면 배지가 서지 않는다). 레일·[무전]·«메시지» 탭의
안 읽은 수는 **대화 전부의 합**(1:1 포함)이다. 곧바로 실패한 발신도 말풍선은 선다(실패 + [재전송] — 쓴 글이 토스트만 남기고 사라지지 않는다). 문자(SMS) 면도 같은 모양이다(종류
라벨 대신 «문자»·«외부망», 입력칸 옆에 글자 수 — 70자를 넘으면 LMS).

**⑤ «이벤트» 면** — 왼쪽 **거르기(220)**: «종류» 체크(발언·입퇴장·긴급·SDS·영상·오류, 종류마다 건수) · «채널» 체크(이벤트에 나온
채널). **«기타»**(`ActivityKind.NOTE` — 그룹 생성·편집·삭제 · 멤버 확인 전 연결 · 녹취 재생)는 종류 체크로 숨기지 않는다(채널 체크만 탄다). 가운데 **표**: 머리 «이벤트 n» · [새 이벤트 따라가기 ✓](새 줄이 오면 맨 위로) · [이력에서 보기](→ [이력]) · [CSV](거르기와 무관하게
전부, 시간순 — `activityCsv` 시각·채널·종류·내용·긴급, UTF-8 BOM, 저장 자리는 시스템 문서 선택기), 진행 중인 긴급·임박 고정 줄(§6.2a-1),
열 머리(시각 76 · 채널 · 종류 · 내용, 아래 강조 선), 줄(46) = 시각(고정폭) · 채널 · 종류 라벨(긴급 = 빨강 면, 오류 = 빨강 테두리) · 내용.
**줄 탭 = 오른쪽 이벤트 상세 패널**(같은 줄을 다시 누르면 닫힌다 — 이벤트 행에는 세션 안에서 매긴 `ActivityRow.id` 가 있다):
시각·채널·종류·내용 · **앞뒤 이벤트**(같은 채널의 앞 둘·뒤 둘 — `aroundOf`, 지금 보는 것 강조) · [답장](SDS 행 — 그 스레드로) · [채널 열기]
(열 수 있는 채널일 때 — 편성 그룹·살아 있는 세션. 1:1 SDS 행의 키는 사람 번호라 채널이 아니다) · [이력에서 세션 보기 ›].

**줄이 생기는 때**(데스크톱 `DispatchSession` 과 같은 낱말 — 채널 이름은 채널 칸에 선다):

| 때 | 내용 | 종류 |
|---|---|---|
| 내 세션이 목록에 오름 | «개별 통화 · 발신\|착신» · «애드혹 그룹 · n명» · «청취 시작» · «일제 통화 개시»(내가 개시) / «일제 통화 · <개시자>»(수신) — 편성 그룹의 보통 호는 로스터가 «세션 시작 · 참가 n» 으로 남긴다(`sessionStartText`) | 입장 |
| 내 세션이 끝남 | «세션 종료 · m:ss» · «청취 종료 · m:ss» · «개별 통화 종료 · m:ss» · «애드혹 종료 · m:ss · 참가 n» · «일제 통화 종료 · m:ss». 성립하지 못한 것은 길이 대신 «실패 <코드>»(편성 그룹은 «세션 실패 <코드>»), 내가 거둔 것은 실패가 아니다(`sessionEndText`) | 퇴장 |
| 발언이 끝남 | «<화자> 발언 n초» **한 줄**(진행 중 줄은 없다 — 지금 누가 말하는지는 카드가 보인다. `talkEndText`) | 발언 |
| 발언 요청이 안 됨 | 거부·회수·시간 초과 사유. 발언자를 싣지 않는 이벤트(코어 타이머의 요청 시간 초과 T101·앱 쪽 수신 전용 거부)는 지금 말하는 사람의 표시를 지우지 않는다 — 지우면 남의 발언이 일찍 끝난 것으로 적힌다. 승인된 발언 시간이 다해 코어가 스스로 놓으면(T2) «발언 시간 초과 — 발언이 끝났습니다» 한 줄을 남긴다(카드의 사유 줄에는 적지 않는다 — 뒤따르는 Floor Idle 이 곧 지우고 그사이 발언 칩이 «거부» 로 읽힌다) | 오류 |
| 멤버 합류·이탈 | «<이름> 합류\|이탈»(첫 스냅샷·나 제외) | 입장·퇴장 |
| 긴급·임박 | «긴급 격상 요청» → «긴급 개시» / «긴급 해제 요청» → «긴급 해제», 긴급 경보·해제·해제 요청·해제 거절, 미응답 멤버. **요청 줄은 명령보다 먼저** 남긴다(확정이 그 뒤에 선다) | 긴급 |
| 메시지 | 받은 SDS·파일 한 줄(같은 msgId 재수신은 또 적지 않는다) | SDS |

링 버퍼는 500줄이고 **하루 지난 줄은 뺀다**(`ACTIVITY_KEEP_MS` — 표의 시각은 시:분:초뿐이라 어제 줄이 남으면 오늘 줄과 구별되지
않는다. 지난 것은 [이력] 이 날짜로 보인다). [새 이벤트 따라가기] 는 저장된다(설정 `followChannelEvents`).

**⑥ 머리 필터** `[전체|대표번호|부재]` — 데스크톱 «기록» 거르기(`전체|통화|문자|부재|대표번호`, dispatch_desktop_ui.md §4.4)의 통화 쪽 셋이다. 오늘 데스크 칩과 **같은 상태**를 쓴다 —
둘로 나누면 «칩으로 건 필터» 와 «머리로 건 필터» 가 서로를 덮는다.

«대표번호» 는 종류와 **직교한 조회 축**이다(대표번호로 온 부재도 있다). 그래서 집계 칩에는 없고 머리에만
있다. 판정은 `CallLogRow.viaPilot` — 내 호는 `calledParty` 가 찼는지(재타게팅), 감시 대상의 호는 감시
AoR 이 대표번호인지로 본다. 둘을 가르는 이유는 책임이 다르기 때문이다 — 대표번호 호는 그룹 전원이 울리고
누가 받았는지가 따로 있어, 섞어 보면 «내가 놓친 것» 과 «동료가 받은 것» 이 구분되지 않는다.

**③ 그룹원 띠의 탭** = 빠른 발신 입력란에 채움(데스크톱 §4.3 «대기 → 클릭 → 입력란에 채움»), 롱프레스 =
사람 메뉴. 탭으로 곧바로 걸지 않는 것은 오조작 때문이다 — 띠는 상태를 보려고 자주 만진다.

---

### 6.3b 채널 카드의 접속자 줄 — 로스터 미리보기

데스크톱과 같은 네 줄 카드다([dispatch_desktop_ui.md](dispatch_desktop_ui.md) §4.1). 포커스는 카드가 아니라 사이드
패널이 받으므로(§6.3a) **내 채널 카드 전부**가 접속자 줄 한 줄을 갖는다 — 카드 높이 120 안에서 이름·마지막 발언·접속자·
참가/상태 네 줄이 고정이라 카드 수가 늘어도 격자가 흔들리지 않는다. 접속자 줄은 칩이 아니라 글자 한 줄(`이름 · 이름(나) · 이름 +n`)이다.

- **발언자는 접히지 않는다.** 칩이 자리에 다 안 들어가면 `+n` 으로 접는데, 하필 지금 말하는 사람이 접힌
  쪽에 있으면 카드가 «누가 말하는지» 를 못 보여 준다 — 그게 이 줄의 존재 이유다. `rosterPreview` 가
  발언자 → 나 → 나머지(서버 순서) 로 세운다. **같은 등급끼리는 서버 순서를 지킨다** — 갱신마다 칩이
  뒤섞이면 읽을 수 없다.
- **접속한 사람만**(`status == "connected"`). 은닉 청취자는 서버가 로스터에서 빼므로
  ([dispatch_center.md](dispatch_center.md) §5.6 `listen_visibility`) 앱이 따로 거를 것이 없다.
- 번호는 **정규형으로 맞춘다** — 로스터가 `tel:+8210…`, 전화번호부가 `010…` 이라 그대로 비교하면
  «나»·«발언 중» 이 붙지 않는다. 같은 사람이 두 URI 꼴(`tel:`·`sip:`)로 실려 와도 한 사람이다(목록 키가 번호다).
- 라벨은 **이름만**이다(주소록에 없으면 번호) — PTT 번호는 길어 «번호 이름» 으로 병기하면 카드 한 줄에 한 사람도 못 든다.
  번호는 채널 상세의 사람 행 둘째 줄이 보인다.
- **사람 메뉴는 패널의 사람 줄에서 연다**(§6.3a·§6.2f). 카드의 접속자 줄은 글자 한 줄이라 누를 곳이 아니다 — 카드를
  누르면 그 채널의 패널이 열리고, 거기 «접속» 목록의 사람 줄을 누르면 사람 메뉴다(데스크톱이 로스터 칩에서 여는 자리).
**미리보기가 쓰이는 곳은 둘이다.** 내 채널 카드의 접속자 줄은 셋까지 펴고 `+n` 으로 접는다. 채널 상세 패널의 «접속» 은 `max` 를
두지 않고 **전부** 편다(§6.3a — 거기는 높이가 넉넉하다).

- **접힌 사람은 볼 방법이 있어야 한다.** 채널 상세 패널이 **[접속 n | 편성 m]** 로 둘 다 보인다 — 편성 = 전 멤버 × 발언 중·참여·
  미참가(그룹 문서, `detailMembers` — [PTT 그룹] 화면 상세와 같은 판정, §6.12). 데스크톱은 같은 뜻의 자리에서 [PTT 그룹] 상세로
  보낸다(`PttChannelsPanel.xaml` [로스터 전체 +n]). 편성 명단은 패널을 열 때 그 그룹을 [PTT 그룹] 의 선택으로 받아 온다
  (`selectById` — 화면 밖에서 그룹을 지목하므로 **필터·검색을 [전체]로 되돌린 뒤** 고른다. 현재 필터가 그 그룹을 걸러 내면 선택이
  곧바로 해제돼 명단이 비어 보인다).

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
(`phoneBook`/`pttBook`, §6.2b)에 나뉘어 들어온다. 그래서 칩 하나를 눌렀을 때 «이 사람에게 개별 통화도 통화도
걸 수 있다» 를 보이려면 먼저 **사람 단위로 묶어야** 한다. 데스크톱 `PersonActionsViewModel` 의 이식이다.

**묶는 규칙**(`mergePeople` — 순수 함수, 데스크톱과 같다).

- 키는 **이름 + 조직**. 이름이 없으면(서버가 번호만 준 행) 번호가 키다 — 이름 없는 행끼리 합치면 남남이
  한 사람이 된다.
- 회선을 이미 둘 가진 항목에 같은 키가 또 오면 **번호를 키로 따로 세운다.** 동명이인이 같은 조직에 있거나
  한 사람이 회선을 셋 이상 가진 경우인데, 구별 근거가 이름·조직뿐이라 합치면 **엉뚱한 사람에게 개별 통화이
  나간다.** 나누면 목록에 두 줄이 보일 뿐이다.
- **내 회선은 뺀다**(`myLineKeys` — 전화·PTT 둘 다). 한쪽만 빼면 다른 축에서 «나» 가 남아 자기에게 거는
  항목이 보인다.
- 주소록에 없는 상대도 `resolvePerson` 이 **그 번호만 가진 항목을 만들어** 준다 — 빈 메뉴보다 «통화» 하나라도
  있는 편이 낫다. 전화번호부에 있으면 내선, 없으면 PTT 로 본다(데스크톱 `Resolve` 와 같은 판정).

**이 메뉴가 주소록의 기본 동작이다.** 행을 누르면 바로 걸지 않고 메뉴가 뜬다 — 휴대폰 연락처와 같다.
전에는 탭이 곧 발신이었는데, 한 사람에게 할 수 있는 일이 여섯이라 탭 하나를 발신에 고정하면 나머지는
롱프레스를 아는 사람만 쓰게 되고, 목록을 훑다 잘못 눌러 걸리는 사고도 난다. **바로 걸고 싶으면 행
오른쪽의 [📞]** 다 — 빠른 길을 없애지는 않는다.

**행동 여섯을 `MainViewModel` 이 잇는다**(데스크톱도 같은 자리다). 행동마다 가는 화면이 다르므로 메뉴를
띄운 패널이 직접 처리할 수 없다.

| 행동 | 처리 | 비고 |
|---|---|---|
| 통화 | `dial` + [통화] › «통화» | 세션을 만드는 조작은 그 세션을 다루는 화면으로 돌아간다(§6.2) |
| 문자 | `openTo(내선)` + [통화] › «메시지» | SIP MESSAGE — 전화 축(§6.2e) |
| 개별 통화 | `startPrivateCall` + [무전] | |
| 무전 메시지 | `openThread(PTT 번호)` + [무전] › «메시지» | 1:1 SDS. 수신 스레드 키와 같다(`applySds` 가 `groupUri` 가 없으면 보낸 사람 번호를 쓴다) |
| 애드혹에 추가 | `addPick` + [무전] › «채널» + [채널 추가] 패널 — 그 사람을 고름에 **더한다**(토글하지 않는다) | 고름은 `MainViewModel` 이 든다 — 패널을 닫았다 열어도 남는다 |
| 통화 기록 | `setPersonFilter` + [통화] › «통화내역» | 최상위 [이력] 이 아닌 이유는 그쪽이 날짜를 골라 보는 과거 조회라 «이 사람» 축이 없기 때문이다(§6.11) |

**전화 축을 먼저, 무전 축을 그 아래로** 묶는다 — 주소록에서 가장 잦은 것이 통화·문자다. 항목마다 오른쪽에
**어느 번호로 가는지**(내선/PTT 번호)를 흐리게 적는다.

**«문자» 가 둘로 갈리는 것은 의도다.** 회선이 둘인 사람에게는 «문자»(내선, SMS)와 «무전 메시지»(PTT 번호,
SDS)가 같이 뜬다 — 합치면 어느 번호로 나갔는지 알 수 없고 상대가 받는 자리도 다르다. 회선이 하나인
사람에게는 가진 쪽만 뜬다(없는 회선의 버튼은 비활성이 아니라 **아예 그리지 않는다**).

**통화 기록 필터는 종류 필터와 직교한다.** «전체\|대표번호\|부재» 와 나란히 두지 않고 **✕ 로만 푸는 칩**을
따로 세운다 — 종류 칩 사이에 끼우면 «전체» 를 눌렀을 때 사람 필터까지 풀린 줄 알게 된다.

**진입점** — 태블릿에는 우클릭도 `Ctrl+K` 도 없다.

- 사람 메뉴: **롱프레스** — ③ 관제 그룹원 띠 · ③ 내 통화 카드 · ⑥ 통화 내역 행, **탭** — [주소록] 패널의 행 · 채널 추가·채널 상세 패널의 사람 줄(⋮).
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

**지금 상태를 걸기 전에 보인다.** 주소록 줄에는 그 회선의 «통화 중»·«링잉»(`lineStatusOf` — dialog 를 감시하는 회선만 안다), 사용자
패널 줄과 통합 검색의 사람 행에는 «<그룹> 발언»·«<그룹> 참여»(`pttStatusOf` — 로스터에 접속으로 잡힌 그룹, 모르면 «접속»)가 선다.
통합 검색의 사람 행에는 [개별 통화]·[애드혹 그룹]·[SDS]·[통화]·**[문자]**, 그룹 행에는 [채널로] 와 내 소유 그룹이면 **[멤버 추가]**
(→ 편집 폼)가 선다. 주소록 목록에서 나는 뺀다.

### 6.2d 호 목록·경과·동시 통화 — 데스크톱에서 그대로 온 규칙

| 규칙 | 데스크톱 근거 | 태블릿 |
|---|---|---|
| **내 통화는 살아 있는 호만** | `CallDeskViewModel.Rebuild` 가 `Sessions.Where(x => x.IsVolteCall)` 로 카드를 만들고, 끝난 세션은 `Sessions` 에서 빠진다 | `kind == PHONE_CALL && isLive` |
| **경과는 1초 틱이 갱신한다** | `Models/Sessions.cs` `Tick(now)` → `Elapsed = now - (ConnectedAt ?? StartedAt)`, `MainViewModel.Tick` 이 1초마다 부른다 | 세션의 `tick` Flow(살아 있는 세션·dialog 가 있을 때만) 를 목록 Flow 에 물린다 — 계산 속성만으로는 Compose 가 재구성하지 않는다. [무전] 의 내 채널 카드·채널 상세 한 줄 요약도 같은 틱을 읽는다(`PttChannelsViewModel.tick`) |
| **응답하면 기존 통화를 자동 보류** | `Services/DispatchSession.cs` — 호가 Active 가 되면 다른 활성 VolteCall 을 `Hold()`. 설정 `AutoHoldOnAnswer` 기본 **true**(`SettingsStore.cs`) | 같은 규칙·같은 기본값. 끄면 두 통화가 동시에 들려 어느 쪽에 말하는지 알 수 없다 |
| **거절은 486** | `Reject(486)` | 같다. 603 은 서버가 포크 집합을 통째로 접어 다른 관제석까지 멈춘다(TS 24.239) |
| **울리는 카드는 [응답]·[거절]** | `Views/CallModeView.xaml` — `CanAnswer` 면 [응답]·[거절 486], [종료] 는 `CanAnswer` 가 아닐 때만 | 같다 — 착신 배너와 같은 두 동작이 카드에도 선다. 울리는 호에는 끊을 통화가 아직 없어 [종료] 를 두지 않는다 |
| **대기열 행의 [응답]·[당겨받기]** | `QueueItem` — `RingsMe` 면 [응답](이 발신자의 **내** 착신 leg 만), 링잉이면 [당겨받기] = `Pickup(Pilot)` | 같다(`myLegOf`·`QueueItem.pilot`). [응답] 은 직접 착신이 동시에 울려도 그 호를 받지 않는다. [당겨받기] 는 지정 픽업 `<code><대표번호>` 로 이 대표번호의 포크를 고른다([dispatch_center.md](dispatch_center.md) §4.4) — 그룹 픽업은 그룹의 다른 링잉 호를 집을 수 있다 |
| **오늘 데스크 칩은 ⑥ 의 필터다** | 데스크톱은 오늘 집계(`CallDeskViewModel.Today*`)를 «기록» 머리에 수로 두고, 거르기는 상대 목록 칩(`CallRecordsViewModel` — `all\|call\|sms\|missed\|pilot`)이 맡는다 | 칩 값 `all\|missed\|outgoing\|transfer\|monitor`, 규칙 `keepInDesk`. **«응대» 칩은 `all`** 이다. 응대는 목록 대부분이라 거를 이유가 없고 칩 다섯 중 하나는 해제 자리여야 한다. 태블릿은 해제 수단이 칩뿐이라 **같은 칩 재클릭도 해제**로 둔다. 감청은 데스크톱의 `ListenStart`/`ListenEnd` 둘이 태블릿에서 `MONITOR` 하나라 그 한 종류로 판정한다 |

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
녹취·세션 상세 조회의 403 `out_of_scope` 는 **청취 범위** 밖이다 — «청취 범위 밖의 녹취입니다»·«청취 범위 밖의 세션입니다» 로
말한다(같은 토큰이 관리 API 에서는 관리 범위 밖이라 문장이 다르다). 닿지 못한 녹취 조회는 «서버에 닿지 않습니다» 다.

**정책 게이트는 CMS 문서에서 온다 — 5분마다 다시 받는다.** 기동 때 CMS user profile(TS 24.484 §8.3)과 service configuration
(§8.4)을 받아 `Capabilities` 를 세우고(`refreshCms` — ETag, 안 바뀌었으면 304), 그 뒤 5분마다 다시 받는다. 이 값이 개별 통화·
애드혹·긴급 호출의 **선차단**(자격이 없으면 보내지 않고 이유를 말한다 — `allow-private-call`·`allow-emergency-private-call`·
`allow-adhoc-group-call`·`allow-emergency-group-call`)과 [긴급 해제]·[임박 해제]·[경보 해제] 의 자격(§6.2a-1)이다. 받지 못한 문서는
**허용**으로 읽는다 — UX 선차단일 뿐이고 최종 판정은 서버(403)다. service configuration 의 `*-resource-priority` 가 PTT 계정의
Resource-Priority 값이 된다(TS 24.379 §6.2.8.1.15 — 문서에 없으면 코어 기본값). 계정에 실리는 값이라 다음 로그인에 반영된다.

**PTT 계정은 로그인 때 셋을 더 싣는다.** ① 참여 기능 PSI(UE initial configuration, TS 24.484 §7.2 — MCPTT = 경보 Request-URI,
MCData = disposition 통지 Request-URI) ② `mcdataMsrp`(큰 그룹 SDS 는 media plane 으로 보내고 서버발 MSRP 배포도 받는다 — TS 24.282
§9.2.3) ③ 위 Resource-Priority. 엔진의 `User-Agent` 는 `CIMS-Dispatch/<앱 버전> (Android <판>; <모델>)` 이다 — 제품명이 서버의 단말
유형(dispatch) 판정 키라 데스크톱 관제 앱과 같은 이름을 쓴다([mcptt_management_views.md](mcptt_management_views.md) §4.1). UDP→TCP
승격 비활성(`sip.udpNoTcpSwitch`)은 엔진 전역이라 올리는 서비스 중 하나라도 사이트 옵션이면 켠다.

**편성은 서버에서 바뀐다 — 재로그인 없이 따라간다**(`DiscoveryPlane.kt`). 통로는 둘이다.

- **그룹 문서** — GMS PSI 에 xcap-diff 를 구독해 두고(RFC 5875, `subscribeGroupChanges`), 서버가 그룹 변경을 밀면 그룹 목록을 다시
  받는다(`applyXcapDiff` — 연속 통지는 0.5초 합친다). 새 멤버 그룹은 affiliation + conference 구독, 빠진 그룹은 해제.
- **관제 편성**(`/provisioning/me` 의 dispatch) — 60초마다 `If-None-Match` 로 묻는다(`refreshDispatch`). 304 면 끝이고, 그룹원·청취
  대상·범위·대표번호가 바뀌었으면(`dispatchChanged` — 순서는 보지 않는다) 프로파일을 갈아 끼우고 **바뀐 것만** 다시 건다: 빠진 회선은
  dialog 구독을 풀고 그 행을 치우며 새 회선만 구독하고(`rewatch` — 남은 회선은 건드리지 않는다), 그룹 목록을 다시 받고, 토스트로
  «관제 편성이 바뀌었습니다» 를 알린다. 멤버 그룹이 사라지면 이 재조회를 당긴다 — 서버의 청취 대상 목록이 지워진 그룹을 아직 들고
  있어 다시 구독하는 창을 없앤다.
- 감시 구독이 곧바로 실패하면 줄마다 로그하고 관제사에게는 **한 번** 알린다(«회선 감시 구독 실패 n건») — 감시 누락은 조용히 넘기면
  «통화 없음» 처럼 보인다. 대표번호를 먼저 구독한다(대기열이 그 구독 하나에 달렸다 — `watchTargets`).

**내가 연 그룹 통화의 두 알림.** ① 필수 멤버가 응답하지 않은 채 열렸다(TS 24.379 §6.3.3.3 — 서버 INFO `<non-acknowledged-user>`,
`applyNonAcknowledged`) → ⑤ «미응답 멤버 n명 · 이름들» + 토스트. ② 개시 200 OK 가 `P-Answer-State: Unconfirmed` 다(RFC 4964 — 서버가
멤버 확인 전에 받았고 첫 멤버가 붙기 전의 말은 서버가 담았다 전한다, §10.1.1.2.1.1) → ⑤ «멤버 확인 전 연결» 한 번(`noteAnswerState`). 코어는 이 헤더를 적은 뒤 호 상태 이벤트를 한 번 더 낸다(ue_sdk.md §4.2) — 그 이벤트에서 적는다. 성립 0.3초 뒤와 뒤따르는 미디어 이벤트에서도 호 정보를 다시 물어 본다(그 이벤트를 놓쳐도 적힌다).

**로스터는 부분 갱신을 접는다.** conference NOTIFY(RFC 4575)는 전체 스냅샷이거나 부분 갱신이다 — 부분 갱신은 온 사람만 바꾸고
(`disconnected` 는 뺀다) 나머지는 그대로 둔다(`mergeRoster`). 전체로 읽으면 한 명이 들어올 때마다 나머지가 접속 목록에서 사라진다.
내 채널에서는 접속에 들어온·빠진 사람을 ⑤ 에 «<이름> 합류»·«<이름> 이탈» 로 남긴다(`rosterMoves`) — 구독 직후의 첫 스냅샷과
세션이 통째로 서고 끝나는 순간(«세션 시작»·«세션 종료» 한 줄이 말한다)과 나 자신은 적지 않는다.

**서버 통합 이력은 타인의 것만 채운다**(`HistoryFeed.kt` — `GET /provisioning/history`, 2.5초 커서 폴링 · ETag). 청취 범위 그룹의
끝난 PTT 세션과 그룹 SDS 는 ⑤ 에 제 시각으로 끼우고(`addActivityAt`), 감시 대상끼리의 1:1 문자는 ⑥ 에 «문자» 행으로 남긴다. 내가
당사자인 항목은 건너뛴다(로컬 행이 이미 있다). 통화(`kind=call`)는 묻지 않는다 — 감시 대상의 통화는 dialog 이벤트가 끝나는 즉시
⑥ 에 남기므로 또 받으면 두 줄이 된다. 서버가 이 API 를 내지 않거나(404·501·405) 범위가 없으면(403) 조용히 꺼지고, 닿지 않으면 주기를
두 배씩 늘려 30초에서 멈춘다. 폴링은 live 를 대체하지 않는다(§6.7).

**`Engine.calls()` 는 끝난 호도 준다.** 코어는 조회·최종 통계용으로 종료된 호를 64건까지 보존한다
(`engine.cpp` `callInfos`·`pruneFinished`). 화면 복귀마다 부르는 `refreshSessions()` 가 이를 거르지 않으면
**끝난 통화가 «내 통화» 로 되살아난다** — 상태로 거른다. 그때 목록에 있던 호가 스냅샷에서 끝나 있으면(종료 이벤트가 아직 접히지
않았다) **종료로 접고** 간다 — 목록만 갈면 그 호의 통화 내역·세션 종료 줄·실패 사유가 사라진다.

**착신·미디어 이벤트는 지금 상태를 코어에 다시 묻는다**(`applyCallSnapshot`). 이 둘은 호 상태와 다른 흐름으로 와서 끝난 호의 것이
종료 뒤에 접힐 수 있다 — 낡은 스냅샷을 그대로 얹으면 끝난 호가 카드·착신 배너(벨소리)로 되살아난다. 끝났으면 버리고, 살아 있으면 그
최신 값으로 접는다. 받기 전에 끝난 착신(상대가 곧바로 거뒀다)은 세웠다가 종료로 접어 «부재» 줄을 남긴다 — 착신의 세션은 이 이벤트로만
서므로 버리기만 하면 내역에 없다.

**자동 보류는 새 통화가 처음 성립할 때와 내가 보류를 풀 때만 건다**(데스크톱과 같다) — 이미 성립한 호가 다시 활성이 되는 것(상대가 보류를 풀어 줌)에 걸면
내가 말하던 통화가 보류된다. **내가 [거절] 한 착신은 «부재 · 거절»** 이다(받지 않은 호라 부재로 세되 놓친 것과 구분). 서버 이력 폴링이 가져온 발언 가운데
**내가 들어가 있는(청취 포함) 그룹의 것은 건너뛴다** — 내 세션의 floor 가 이미 ⑤ 에 적었다.

**처음부터 confirmed 로 보인 dialog 는 응답된 통화다**(`DialogRow.of`) — 로그인·재구독 때 진행 중이던 통화는 첫 NOTIFY 가
confirmed 다. 그렇게 세우지 않으면 그 통화가 끝날 때 «부재»·«전원 무응답» 으로 적히고 오늘 부재 집계에 든다.

**대표번호 호는 대표번호 dialog 가 정본이다**(데스크톱 `RecordPilotOutcome`, dispatch_center.md §4.4).

| 일 | 규칙 |
|---|---|
| 조작 표시 | 호를 만든 조작(당겨받기·전달·청취…)과 애드혹 참가자는 **명령 반환과 첫 호 이벤트 중 어느 것이 먼저 와도** 맞게 붙는다(`noteOperation` 이 이미 선 세션을 고치고, `newSession` 이 적어 둔 참가자를 싣는다 — 코어는 그 순서를 정하지 않는다) |
| 통화내역 | 대표번호 dialog 가 confirmed 없이 끝나면 **«전원 무응답» 부재 한 줄**, 동료가 받았으면 **«응답 <회선>» 한 줄**(둘 다 데스크의 일 — 오늘 집계에 든다). 내가 받은 호는 내 세션이 남긴다. 포크된 그룹원 leg 과 응답 없이 끝난 **내 포크 leg** 은 적지 않는다 — 받지 않은 회선마다 «부재» 가 서면 한 호가 여러 줄이 되고 동료가 받은 호가 내 부재로 세어진다(`noteDialogEnd`·`isPilotFork` — 포크는 **착신 leg** 이고 대표번호 dialog 가 살아 있는 동안 시작한 것만. 대표번호 호가 끝난 뒤 같은 발신자가 그룹원에게 직통으로 건 호는 따로 남는다) |
| 응답자 | 그룹원 회선이 같은 발신자와 confirmed 되면 그 회선이 포크 승자다(`DialogRow.answeredBy` — 대표번호·회선의 confirmed 순서는 서버가 정하지 않아 양쪽에서 맞물린다) |
| 종료 | terminated 는 **같은 dialog id 의 행 전부**에 적용한다 — 서버가 종료 NOTIFY 를 다른 회선의 entity 로 보내도 «통화 중» 행이 남지 않는다 |
| 대기열 | 응답된 호는 **3초 보이고 내려간다**(누가 받았는지 읽을 시간 — `QUEUE_ANSWERED_KEEP_MS`). 발신자마다 한 줄. 대기열·배지가 «기다리는 호» 만 센다 |
| 진행 중 | 대표번호 dialog 는 행이 아니다(대기열이 말한다) — 그 발신자의 그룹원 leg 에 **[대표]** 를 붙인다. 왼쪽 = 건 사람, 오른쪽 = 받은 사람. 청취 중인 통화가 맨 위, 그다음 링잉 › 최근. [지정 픽업] 은 울리는 **착신** leg 에만(거는 중인 호는 당겨받을 것이 없다) |
| 내 통화 카드 | [대표] = `P-Called-Party-ID` 가 **내 대표번호**일 때(`isPilot`). 보류·음소거·DTMF 는 성립한 통화에만 선다 |
| 그룹원 띠 | [청취] 는 감청 범위(`monitorScope != none`)가 있을 때만. 한 회선에 통화와 링잉이 겹치면 통화를 보인다 |
| 오늘 데스크 | **오늘 끝난** 내 줄에서 센다(`deskTallyOf` — 줄이 설 때와 1분마다, 자정을 넘기면 0 부터). 타인 통화는 빼고 동료가 받은 대표번호 호는 든다 |
| 발신 | 번호칸에 친 것을 풀어 건다(`dialTargetOf`) — 구분자(공백·`-`·괄호)를 빼고, **이름**이면 주소록의 그 사람 번호(한 사람으로 정해질 때만), 주소록에 있는 번호면 **저장된 원 번호**(`010…` 으로 쳐도 가입 id 로 — 주소록(CSV)의 번호에 든 구분자도 뺀다). 내역의 발신 줄은 «통화 종료»·«실패 <코드>»·«취소»(내가 거둠)·«미응답». 번호칸은 **걸렸을 때만** 비운다(실패하면 친 번호가 남는다 — 번호칸의 제안·주소록에서 바로 건 발신도 같다) |
| 감시 구독 | 걸리지 못한 회선은 «구독 중» 에서 빼 두고 편성 재조회 주기(60초)마다 다시 건다(`retryWatch` — 알림은 실패가 이어지는 동안 한 번). 편성 변화 판정에 청취 노출(`listenVisibility`)이 들고, 서비스 능력(`smsGateway`)만 바뀌어도 프로파일을 갈아 끼운다 |

### 6.2e 개별·애드혹 통화 · 설정 · 메시지(SDS·SMS)

**개별·애드혹 통화**(dispatch_desktop_ui.md §4.1) — «채널» 면의 **[채널 추가하기]** 가 여는 **채널 추가 패널**(오른쪽 400)에서 건다.
데스크톱의 팝오버 둘([개별 ▾]·[애드혹 ▾])과 그룹 만들기를 **한 패널**로 접었다: 셋 다 «PTT 주소록에서 사람을 골라 내 채널에 카드 한
장을 더한다» 이고, 다른 것은 «몇 명인가·두고 쓰는가» 뿐이다(개별 통화 = 1명, TS 24.379 private call / 애드혹 통화 = N명, ad hoc group
call / 그룹 추가 = 편성, TS 24.481 — §6.12). **고르기가 먼저**고 무엇을 할지는 아래 줄에서 고른다 — 모드를 먼저 고르게 하면 모드를
바꿀 때마다 고른 사람이 사라진다.

- **아래 줄**(강조 선 위) = «n명 선택»·[긴급](개별·애드혹 둘 다 — emergency)·[전이중(개별)]·[선택 해제] / [개별 통화] · [애드혹
  통화] · [일제 통화] / [그룹 추가 ›]. 자격은 순수 함수 `addChannelActions` 가 정한다 — [개별 통화] = 고른 사람이 **정확히 1명**이거나
  아무도 안 골랐을 때 검색칸에 친 이름·번호, [애드혹 통화]·[그룹 추가 ›] = 1명 이상([그룹 추가] 는 생성 자격도), [일제 통화] = 1명
  이상(누르는 동안은 늘).
- **고름은 패널을 닫아도 남는다**(`MainViewModel.picked`) — 다시 열면 그대로고, 걸리면(세션이 서면) 비우고 패널을 닫는다(새 카드가
  내 채널에 선다). 사람 메뉴 [애드혹에 추가](§6.2f)는 그 사람을 고름에 **더하고**(토글하지 않는다) «채널» 면에 이 패널을 연다.
- **사람 줄** = 고름 네모 · 이름·«PTT 번호 · 조직» · 아는 상태(«접속») · ⋮ 사람 메뉴(무전 메시지 1:1 SDS·통화·문자 …). 주소록에
  등록 여부가 없어 «오프라인» 을 단정하지 않는다.
- **데스크톱 팝오버의 [그룹] 목록은 옮기지 않는다** — 태블릿에서는 «채널» 면 자체가 그 목록이다. 같은 행동이 제 자리에 있다:
  [채널로] = 카드 탭(채널 상세 패널), [메시지] = 패널 [메시지 ›], [편집]·[삭제] = 패널 ⋮, [새 그룹] = [채널 추가하기] › [그룹 추가 ›]
  (§6.3a·§6.12). 패널에 한 벌 더 두면 같은 그룹 목록이 두 곳에 선다.
- **개별 통화는 번호를 직접 넣어도 걸린다**(데스크톱 `PttOriginateViewModel.Start` 와 같은 풀이, `privateTargetOf`) — 고른 사람이
  없으면 검색칸의 이름·번호로 대상을 푼다: 주소록 이름과 정확히 맞으면 그 번호, 번호가 맞으면 그 회선, 주소록에 없어도
  **번호 모양이면 그 번호**(주소록을 못 받았거나 막 개설한 회선 — 목록이 비면 «주소록에 없는 번호입니다 — [개별 통화] 로 겁니다»).
  모르는 이름이면 [개별 통화] 가 꺼진다 — 데스크톱은 입력 그대로 보내 서버 404 로 돌아온다.
- **[일제 통화]** = 고른 사람들에게 애드혹 일제 통화(TS 24.379 §17.2.2.1.1 9) "broadcast adhoc group call") — 채널 상세의
  [일제 통화](§6.3a)와 같은 한 버튼이다: 누르는 동안 개시하고 말하며(개시 INVITE = 암묵적 발언 요청, `GroupCallOptions{members, broadcast,
  implicitFloorRequest}`), 놓으면 끝(성립 전 CANCEL / 뒤 Floor Release → 코어가 호 해제), 잠금 발언이면 누를 때마다 켜고 끈다. 누르는
  동안 고르기·[개별 통화]·[애드혹 통화]·[그룹 추가]·[선택 해제] 는 잠긴다(버튼이 사라지면 제스처가 취소돼 즉시 끝난다) — 놓으면 비우고
  패널을 닫는다(개시 실패면 남긴다). 서버가 ad hoc 의 broadcast-ind 를 받지 않으면 일반 애드혹 그룹 통화로 열리고 ⑤ 에 «일제 통화로
  열리지 않았습니다» 를 남긴다.
- 애드혹 그룹 id 는 **앱이 만든다** — `adhoc-<내 PTT 번호>-<epoch초>`
  ([mcptt_emergency_modes.md](mcptt_emergency_modes.md) §6, `adhoc-`·`priv-` 는 편성 그룹 예약어).
  서버에 편성이 없는 임시 세션이라 채널 영속·affiliation·로스터 구독 대상이 아니고, **참가자 목록도 앱이
  기억한다** — 카드에 «3명» 을 적을 근거가 개설할 때 실어 보낸 목록밖에 없다. 호가 끝나면 지운다.
- 전이중 개별 통화(`mc_no_floor_ctrl`)은 마이크가 늘 열려 있어 **발언 대상이 되지 못하고 음소거로 다룬다**
  (데스크톱 카드 [음소거]와 같다). 두 자격은 정확히 갈린다 — `ChannelCard.canCheck` = 참여 중 + 반이중,
  `canMute` = 참여 중 + 전이중. 반이중에 음소거를 따로 두면 «대상인데 음소거» 라는 모순 상태가 생기고,
  전이중에는 floor 가 없어 음소거가 송출을 멈추는 유일한 수단이다. 음소거는 목록 행(발언 대상 체크 자리의
  마이크 토글)과 채널 머리([음소거] 칩) 두 곳에 서고, 켜지면 경고색으로 채운다 — «말하고 있다고 믿는데 안
  나가는» 상태를 놓치지 않게. 상태의 권위는 코어 스냅샷(`CallInfo.muted`)이다 — 뒤집을 값을 카드 사본이 아니라
  명령 직전의 코어 값에서 읽고 토글끼리 줄을 세운다(연타가 같은 값 둘로 합쳐지지 않게), 명령 뒤 스냅샷을 다시
  당긴다. ③ 통화 카드와 같은 한 경로(`toggleMuted`)다. 코어는 전이중 개별 통화의 음소거를 마이크 결선에
  반영한다(반이중은 floor 가 마이크를 게이트한다 — `Engine::wireMedia`, [ue_sdk.md](ue_sdk.md) §4.2 `setMuted`).

**설정**(§7·§8) — 데스크톱은 별창이지만 태블릿에는 별창이 없다. 레일 바닥의 [설정](상단 바 세션 메뉴에도 있다)이 여는 시트다.
**저장 버튼을 두지 않는다** — 항목마다 즉시 저장·즉시 적용한다. «바꿨는데 저장을 안 눌러서 안 먹은»
상태를 관제석에 만들지 않는다. 오디오 경로처럼 지금 반영해야 하는 것은 `updateSettings` 가 다시 건다.

- **화면 테마** [밝게 | 어둡게](데스크톱 `Theme` 과 같은 값·같은 순서) — 바꾸면 곧바로 전 화면이 바뀐다. 기본은 밝게다(데스크톱과
  같다 — §6.3c). 색은 테마 토큰으로 그려 두 테마에서 같은 뜻을 낸다. 시스템 막대의 바탕도 테마를 따른다(어둡게에서 흰 막대에
  흰 아이콘이 되지 않게).
- **입력칸·버튼은 공용 모양**이다 — 입력칸 = 옅은 면 + 진한 테두리·모서리 6(`cimsFieldColors`·`FieldShape6`), 버튼 = 네모
  (`RectButton`, 모서리 8·먹 글자). Material 기본 모양(알약 테두리 버튼·모서리 4 입력칸)을 그대로 쓰지 않는다(로그인 화면도 같다).
- 접속점(CSC 주소·포트)은 **읽기 전용**이다 — 등록·구독이 붙어 있는 동안 바꾸면 세션이 어긋난다.
  바꾸려면 로그아웃 후 로그인 화면에서 정한다.
- 인증서 검증을 끄면 경고를 띄운다. 로그 수준·검증 여부·사설 CA 는 **로그인 서버(CSC)에는 다음 로그인부터, 통화·무전(SIP 엔진)에는
  앱을 다시 시작한 뒤** 적용된다 — 엔진은 로그아웃으로 내려가지 않는다(서비스 수명).
- `Settings` 는 **관측 가능**해야 한다(`SettingsStore.flow`) — 설정 화면이 바꾼 값을 그 화면이 다시
  그려야 하고, 잠금 발언·자동 보류처럼 다른 화면이 읽는 값도 즉시 따라야 한다.
- **측면 PTT 키 학습**(§7) — [버튼 학습] 뒤 측면 버튼을 한 번 누르면 그 keycode 가 발언 키가 된다(`HwPtt` 영속,
  [기본값] 으로 되돌림). 시트를 닫으면 학습도 끝난다 — 남아 있으면 다음에 누른 아무 키나 발언 키가 된다. 학습 중의
  누름은 발언이 아니다. 뒤로·홈·볼륨·전원은 학습하지 않는다 — 학습을 빠져나갈 키다.

**문자(SMS/LMS)** — [통화] › «메시지» 면. **SDS 와 같은 모양·다른 망**이다.

| | SDS | SMS/LMS |
|---|---|---|
| 자리 | [무전] › «메시지» | [통화] › «메시지» |
| 계정 | PTT | 전화 가족(volte∪voip) |
| 프로토콜 | MCData SDS (TS 24.282) | SIP MESSAGE `text/plain` |
| 스레드 키 | 그룹 id 또는 상대 번호 | **상대 번호만**(1:1 뿐이다) |
| 갈래 | 그룹(편성 전원) / 1:1(그 사람만) | 1:1 뿐 |

- **보관도 스레드도 갈라 둔다**(`Message.kind` = `SDS`\|`SMS`, 세션의 `messages`/`sms` 두 맵).
  PTT 1:1 SDS 의 스레드 키도 번호라 한 맵에 담으면 «무전으로 온 것» 과 «문자로 온 것» 이 한 대화로
  섞인다 — 어느 번호로 답장이 나갔는지 알 수 없게 된다. 사람 메뉴도 같은 이유로 항목을 둘 둔다
  («문자(SDS)» / «문자(SMS)»).
- **보내기 전에 말풍선을 세운다.** 최종 응답은 token 으로 `requestResult` 에서 맞춘다(SDS 와 같은 규약) —
  응답을 기다렸다 세우면 느린 망에서 «눌렀는데 아무 일도 없는» 구간이 생긴다. token 은 발신 명령이 돌아와야 알므로
  **응답이 먼저 올 수 있다** — 발신 명령이 도는 동안 짝을 못 찾은 응답을 들고 있다가, 명령이 돌아오면 그 발신의
  응답을 꺼내 말풍선을 **처음부터 그 상태로** 세운다(전달 확인 회신도 같다 — `TokenLedger`·`sendTracked`). 크기로 버리지
  않고, 도는 발신이 없어지면 남은 것을 버린다(짝이 오지 않을 응답이다). 로그아웃 때 비운다. 명령이 곧바로 실패한 문자는
  처음부터 실패 말풍선이다. 메시지 DB 쓰기는 **한 줄로** 보낸다(보낸 순서대로 — 상태 갱신이 말풍선 저장을 앞지르지 않게).
- **같은 메시지는 한 번만 선다.** 이미 받은 msgId 가 또 오면(상대의 재전송 — 처음 msgId 그대로 — 이나 중복 배달) 말풍선·⑤ 줄을
  또 세우지 않고 처음 것을 둔다(읽음·받은 파일 경로가 덮이지 않는다). 전달 확인만 다시 돌려준다(상대는 그것을 못 받아 다시 보냈다).
- **실패한 발신 말풍선에는 [재전송]** 이 선다(데스크톱 ⚠ 링크, `ResendCore`). 누르면 같은 내용을 같은 갈래로 다시
  보낸다 — SDS 는 스레드 규칙(`sendSdsTo` — 편성 그룹이면 그룹 SDS, 아니면 1:1)으로 **처음의 msgId** 를 실어(앞 발신이 일부에게
  닿았어도 받는 쪽이 같은 메시지로 대조한다 — SDK `sendSds` 의 msgId, [mcdata_messaging.md](mcdata_messaging.md) §5), 문자는 그
  번호로. **같은 말풍선**이 새 token 을 받고(DB 도 같은 행을 고친다 — `updateResend`) 최종 응답은 새 token 으로, 전달 확인
  통지는 그 msgId 로 맞물린다. 새 말풍선을
  세우지 않는다 — 같은 말이 두 번 보이면 두 번 보낸 줄 안다. 다시 보낼 수 있는지는 화면이 쥔 사본이 아니라 **지금의
  말풍선**으로 판정하고 누르는 순간 «보내는 중» 으로 바꾼다(`Resend`) — 사본은 첫 누름 뒤에도 실패로 남아 있어, 발신 명령이
  도는 동안 한 번 더 누르면 두 번 나간다. 곧바로 실패하면 다시 실패 말풍선이고 사유는 토스트다(§6.2a-2).
- **SDS 전달 확인은 받는 쪽이 되돌린다** — 발신자가 delivery 를 요청했으면(disposition 1 delivery·3 both) 받은
  즉시 SDS NOTIFICATION «전달됨»(2)을 되돌린다(`deliveryReplyTo` — 대상 = 받은 SDS 의 보낸 사용자, 그룹 SDS 면 `groupUri` 를 함께 넘긴다,
  [mcdata_messaging.md](mcdata_messaging.md) §3 — 데스크톱 `OnSds` 와 같다). 상대 말풍선의 ✓ 가 이것으로 선다 —
  되돌리지 않으면 상대 화면은 «보냄» 에 멈춘다. 읽음(3) 통지는 보내지 않는다(최소 프로파일, 같은 문서 §7).
  이 발신에는 말풍선이 없어 실패는 로그(`DispatchSds`)로만 남긴다 — 즉시 실패와 token 으로 맞춘 최종 거절 둘 다.
  내 발신 말풍선은 상대가 되돌린 통지로 «전달됨»·«읽음» 이 된다(`updateSendState`).
  경로는 코어가 정한다 — PTT 계정에 MCData PSI(ue-init-config, §6.1)가 있으면 규격형(TS 24.282 V18.13.0 §12.2.1.1 — Request-URI = 참여
  기능 PSI, 대상 MCData ID 의 `resource-lists`, 그룹 SDS 면 `<mcdata-calling-group-id>`), 없으면 원 발신자 AoR 로 1:1 직행(옛 서버 전환기,
  [ue_sdk.md](ue_sdk.md) §4).
- **70자까지 SMS, 넘으면 LMS** 로 글자 수를 적는다(데스크톱 `SmsLimit` 과 같은 값). 앱이 쪼개지 않는다 —
  분할·재조립은 망이 하는 일이다.
- **외부망 번호는 게이트웨이가 없으면 그 스레드에서만** 보내기를 막고 이유를 적는다. 게이트웨이가 있는지는 프로파일이 말한다
  (`capabilities.smsGateway` — `DispatchSession.smsGateway`). 없는데 보내면 거절당하고, 그 거절이 «번호가 틀렸다» 인지 «망이 없다» 인지
  관제사는 알 수 없다(데스크톱 `SendAllowed = (!t.IsExternal || SmsGateway) && CanSms` 와 같은 규칙 — `isSmsBlocked`). 머리의 «외부망»
  라벨은 게이트웨이와 무관하게 선다. **외부망인가의 판정은 앱이 한다** — 서버가 «가입자인가»
  를 알려 주는 통로가 이 경로엔 없어, 전화 주소록(§6.2b)의 서버 줄이면 사이트 안, 로컬 CSV 의 external 줄이면 외부망,
  주소록에 없으면 내선 길이(6자리 이하)만 사이트 안으로 본다. 틀리면 보수적으로 막히는 쪽이다.
- 전화 계정이 없으면 면 전체가 «문자를 쓸 수 없습니다» 다 — 회선이 없는 관제석(PTT 전용)이 있다.

**파일(MCData FD)** — [무전] › «메시지» 의 대화에서 파일을 보내고 받는다([mcdata_messaging.md](mcdata_messaging.md) §4.5, 데스크톱 §4.4 «파일»,
`session/FilePlane.kt`). 그룹 대화면 그룹 FD, 1:1 대화면 1:1 FD — 글과 **같은 스레드 키·같은 판정**(`sendSdsTo` 의 «편성 그룹인가»)이다. 문자(SMS)에는
파일이 없다.

- **보내기** — 입력줄 [📎] 가 시스템 파일 고르개를 연다(여러 개 고를 수 있다, 하나씩 차례로 나간다). 데스크톱의 끌어 놓기는 태블릿에 없다.
  빈 파일과 **상한 50 MB**(서버 `McDataFd.MaxBytes` 기본) 초과는 **읽기 전에** 막고 토스트로 알린다(크기를 알려 주지 않는 제공자는 읽으면서 센다).
  고른 파일은 앱 저장소에 **사본**을 둔다(`files/mcdata/sent` — 원본의 접근 권한에 기대지 않는다, 주소록 CSV 와 같은 판단). 말풍선을 먼저 세우고
  («올리는 중…») **전용 CSC 핸들**로 올린 뒤(`uploadFd` — 그룹이면 `group` 을 실어 서버가 `allow_fd`·멤버십으로 게이트한다. 한 핸들의 요청은
  직렬이라 큰 파일이 이력·관리 조회를 막지 않게 따로 둔다 — `fdCscOrNull`) FD 알림을 보낸다(`sendGroupFd`/`sendFd` — FILEURL·이름·크기·종류).
  401 은 토큰을 강제로 갱신해 한 번 더 보낸다. 최종 응답은 글과 같이 token 으로 맞춘다(«보내는 중» → ✓).
- **재전송** — 실패한 파일 말풍선의 [재전송] 은 같은 말풍선을 갱신한다. FILEURL 이 없으면(못 올렸다) **업로드부터**, 있으면 **알림만** 다시 보낸다
  (같은 파일을 두 번 올리지 않는다). 사본이 사라졌으면 «원본 파일이 없어 다시 보낼 수 없습니다».
- **받기** — 받은 FD 알림(`SdsMessage.fd` — FILEURL + 이름·크기·종류)은 파일 말풍선으로 선다. **자동으로 받지 않는다** — 그룹 파일이 쌓이는
  자리라 관제사가 [받기] 로 고른다. 받기 = `downloadFd`(코어가 FILEURL 의 경로만 취해 **자기 CSC** 로 보낸다 — Bearer 를 다른 호스트로 보내지 않는다)
  → 앱 전용 «받은 파일» 폴더(`files/mcdata/received`)에 저장하고(같은 이름이면 «이름 (n).확장자», 보낸 쪽이 준 이름의 경로 문자·머리의 점은
  걸러 낸다) 곧바로 연다. 받는 동안 한 번 더 눌러도 두 번 받지 않는다(지금의 말풍선으로 판정한다).
- **열기** — 파일 이름이나 [열기] 를 누르면 연결된 앱으로 연다(FileProvider `${applicationId}.fileprovider` + `ACTION_VIEW`, 읽기 권한만 내준다).
  아직 안 받은 수신 파일이면 받기부터 한다. 보낸 말풍선은 앱이 둔 사본을 연다. 열 앱이 없으면 토스트로 알린다.
- **말풍선** — 클립 · 이름 · 크기(«812 B»·«340 KB»·«1.2 MB») · 진행 문구(«올리는 중…»·«받는 중…» + 도는 표시) · [받기] 또는 [열기]. 보낸 파일의
  상태(보내는 중·✓·실패 + [재전송])는 글과 같이 풍선 바깥에 선다. 대화 목록의 미리보기는 파일뿐이면 «파일 <이름>» 이다.
- **파일 자리가 앱 전용인 까닭** — 보관이 로그인 ID 로 격리돼 있으므로(아래) 다른 자리가 기기의 공용 폴더에서 훑어볼 수 있는 곳에 두지 않는다.
  데스크톱의 «폴더에서 보기» 는 그래서 없다 — [열기] 가 그 몫이다. 폴더는 보관 정리를 따른다: 기동 때, **어느 말풍선도 가리키지 않게 된 파일**
  (보관 기간이 지나 말풍선이 지워진 것)을 지운다(`sweepFiles` — 주인과 무관하게 본다, 방금 생긴 파일은 건드리지 않는다).
- 거부 사유는 §10 의 파일 영역 문구다(그룹의 파일 전송 꺼짐·비멤버·서버 한도·보관 만료). 전송은 **세션의 코루틴**에서 돈다 — 화면을 떠나도
  올리던 파일이 «올리는 중…» 에 멈추지 않는다.

**메시지 보관**(§4.4) — `messages.db`(SQLite), 보관 일수는 설정(1~365, 기본 30 — 데스크톱 `MessageRetentionDays`), 기동 때
한 번 정리한다(치는 도중의 값으로 지우지 않게). SDS·SMS 가 같은 표를 `kind` 열로 나눠 쓰고, 파일 말풍선은 같은 행에 파일 열
(`file_name`·`file_url`·`file_type`·`file_size`·`local_path`)을 채운다.
보관하지 않으면 앱을 껐다 켤 때마다 스레드가 통째로 사라지고, **서버에 SDS·SMS 이력 API 가 없으므로 이
로컬 보관이 유일한 근거**다.

- **격리 단위 = 로그인 ID**(`owner` 열 — 데스크톱 `MessageStore.SetOwner` 와 같은 규칙). 관제석은 **자리별 로그인 ID** 를 쓴다(교대해도 같은 ID) —
  한 기기에 다른 자리 ID 로 로그인하면 앞 ID 의 대화가 보이지 않는다. 프로파일을 받은 직후 주인을 정하고(`setOwner` — 프로파일의 `loginId`,
  비면 로그인에 쓴 값) 그 뒤의 적재가 **지금 주인의 것만** 읽는다. 조회·읽음 표시·상태 갱신·새 행은 `owner = ?`, 같은 메시지의 중복 방지(유일
  색인)도 주인 안에서 선다. 보존 정리(`prune`)와 기동 때의 PENDING 마감은 주인과 무관하다(기기의 정책이다). **`owner` 열 이전 판의 행은 그 기기의
  첫 로그인 ID 가 한 번 이어받는다**(자리 기기는 같은 자리 ID 로 로그인한다). 로그인 전에는 아무것도 읽지 않는다. 규칙의 순수 형태는
  `OwnerRule` 이다.
- **Room 을 쓰지 않는다** — 표 하나·질의 몇 개라 애노테이션 처리기와 의존을 늘릴 값이 없다
  (`SettingsStore` 가 DataStore 대신 SharedPreferences 를 쓴 것과 같은 판단).
- **기동 때 잔존 PENDING 을 FAILED 로 닫는다**([mcdata_messaging.md](mcdata_messaging.md) §5,
  데스크톱 `FailPending` 과 같은 규약) — 앱이 죽는 순간 보낸 것은 최종 응답을 받을 길이 없다.
  영원히 «보내는 중» 으로 두면 갔는지 안 갔는지 알 수 없으므로 실패로 닫아 재전송을 유도한다. 닫는 것은 **이번 기동이 시작하기
  전의 것**만이다 — 기동 중(계정이 서고 보관을 적재하기 전)에 보낸 메시지는 응답을 기다리는 중이다. 적재한 보관분과 화면의
  말풍선이 같은 id 면 화면의 것이 최신이다.
- DB 작업은 **IO 의 한 줄로 보낸다**(`storeDispatcher` — 적재·마감·정리도 같은 줄) — 이벤트 처리는 Main 에서 도는데 거기서 디스크를
  만지면 프레임이 밀리고, 줄이 둘이면 저장과 마감이 엇갈린다.
- 로그아웃은 메모리(SDS·문자 스레드 둘 다)만 비우고 **보관은 남긴다** — 같은 로그인 ID 로 다시 로그인하면 스레드가 이어지고,
  다른 ID 로 로그인하면 그 ID 의 것만 다시 읽는다.
- 최종 응답이 바꾸는 발신 상태는 **기다리던(«보내는 중») 행만** 고친다 — token 은 엔진이 뜰 때마다 다시 세므로, 지난 기동의 같은 token
  행을 덮지 않게 화면 쪽 판정과 같은 조건을 건다.

**문자 대화의 규칙** — 대화 키는 번호의 **비교 정규형**이다(`smsKey` — `010…` 으로 보낸 것과 `+8210…` 으로 온 답이 한 대화. 번호 모양일
때만이다 — 영숫자 신원·국제 접두 `00…` 는 원문이 키다(키가 곧 답장 주소다). 원 번호로
보관된 옛 대화는 적재 때 모으고 보관도 고친다). **전화 회선으로 온** `text/plain` 만 문자로 본다(PTT 회선의 평문이 전화 문자 대화에
서지 않는다). 열어 둔 대화로 온 문자는 그 면이 보일 때 곧바로 읽음이다(무전 «메시지» 와 같은 규칙). 대화 머리에는 번호(국내 표기)와
**[전화]** 가 선다. 발신 도메인은 등록한 전화 회선(`phoneService` — 유선 voip 우선)의 것이다.

### 6.3 화면 구조 — 모바일 앱으로 짠다

**데스크톱 격자를 줄여 넣지 않는다.** 데스크톱은 1920×1080 에 모드마다 한 화면([무전] 2×2 · [통화] 통화|기록)을 편다(dispatch_desktop_ui.md §3.1). 태블릿(가로
1280×800)은 그 절반도 안 돼, 같은 격자를 접어 넣으면 채널 카드 세 장·메시지 여섯 줄이 겨우 보여 어느 칸도 제 몫을 못 한다.

**원칙 넷**

1. **한 면은 한 가지 일만 한다.** 한 대상을 더 자세히 볼 때만 오른쪽 사이드 패널 하나가 면 위에 겹쳐 선다(쌓지 않는다).
2. **이동은 좌우 스와이프(관제의 면 여섯 장), 레일·탭은 지름길.** 패널은 스와이프로 열지 않는다.
3. **세로는 가장 모자란 자원이다** — 가로 전용 화면에서 탭을 한 줄 더 쌓으면 그만큼 목록이 준다. 그래서 메뉴는 아래가 아니라
   **왼쪽 레일(폭 80)** 에 두고 탭은 한 줄만 쓴다: 상단 바 64 + 탭 줄 48 + 발언 바 80 을 뺀 **본문 608dp**(시스템 막대가 보이면 그
   몫이 본문에서 빠진다).
4. **예외는 발언 하나.** 관제사는 전화를 받으면서도, 이력을 보면서도 무전한다 — 발언만은 «어느 화면을 보고 있는가» 와 무관한
   조작이므로 모든 화면 하단에 상시로 둔다. 바는 80dp(PTT 버튼 150×64 + 대상 칩 + [모두 해제])로 **줄이지 않는다**.

```
┌────┬──────────────────────────────────────────────────────────┐
│    │ 64  상단 바 — 이름 · 소속·대표번호 · 등록 점 · 감청 중 n · 검색 · 메뉴 │
│ 레 ├──────────────────────────────────────────────────────────┤
│ 일 │ 48  [무전|통화] │ 하위 탭 …       [사용자](무전)·[주소록](통화) │
│    ├─────────────────────────────────────┬────────────────────┤
│ 80 │                                     ┊   사이드 패널 400   │
│    │   면 — 608dp (배치 그대로)            ┊   (면 위에 겹침 ·   │
│    │                                     ┊   왼쪽 가장자리 끌기) │
│    ├─────────────────────────────────────┴────────────────────┤
│    │ 80  발언 바 — PTT 150×64 · 발언 대상 칩 · [모두 해제]          │
└────┴──────────────────────────────────────────────────────────┘
```

**레일 넷 + 바닥 [설정] — 전부 한 번에 누른다**(`AppScreen`, 데스크톱 레일과 같은 구성·같은 순서). 메뉴 안에 메뉴를 두지 않는다 —
무전과 통화는 둘 다 «관제사가 지금 거는 일» 이라 [관제] 하나로 묶고, 나머지는 화면마다 레일 항목 하나다. **[관제]가 첫 화면**이다 —
관제사가 가장 오래 머무는 곳이다.

| 레일 | 안 | 데스크톱 대응 |
|---|---|---|
| **[관제]** | [무전] «채널» / «메시지» / «이벤트» · [통화] 왼쪽 고정 칸(대기열·진행 중·내 통화·그룹원) + «통화» / «메시지» / «통화내역» · [사용자]·[주소록] 패널 | 관제 F1 |
| **[이력]** | 날짜 창 조회 + 녹취 재생 (§6.11) | 이력 F2 |
| **[PTT 그룹]** | 편성 그룹 목록·편집·삭제 (§6.12) | PTT 그룹 F3 |
| **[관리]** | 조직·구성원·번호 (§6.13) — 관리 범위(`dispatch.directoryAdmin`)가 없으면 흐리게 서고, 누르면 이유를 토스트로 말한다. 저장하지 않은 편집이 있으면 빨간 점 | 관리 F4 |
| 바닥 **[설정]** | 설정 시트(§6.2e) — 화면이 아니라 시트라 보던 화면 위에 뜬다 | 설정 |

레일 항목 = 아이콘 + 이름(고른 항목 = 연한 남색 알약 + 남색 글자), [관제] 에 미읽음·응답 대기 수 배지. 상단 바의 **소속 줄**은
«그룹 이름 · 대표 N» 중 있는 것만 적고(그룹 이름이 비면 id, 대표번호는 국내 표기) 데스크가 없으면 «PTT …1234»(어느 회선으로
들어왔는지)다. 등록 점은 계정 순서(PTT · 전화)대로 서고 색이 상태다(등록 녹색 · 등록 중 주황 · 실패 빨강 · 미등록 회색) —
**프로파일에 그 서비스가 있으면** 점을 그린다(계정 추가가 실패한 서비스의 점이 사라지면 «원래 없는 회선» 처럼 보인다). 세션 메뉴
(⋮)의 첫 줄은 **오디오 요약**(«오디오 · 스피커» — 누르면 설정)이다. 감청·청취가 열려 있으면 **«감청 중 n»** 알약이
서고 누르면 그 자리(감청 = [통화] 고정 칸의 그 행, 청취 = 그 채널 상세)로 간다.

**관제 탭 줄(48)** = [무전|통화] 세그먼트(고른 칸 = 남색 채움, 칸마다 미읽음·응답 대기 수) · 그 모드의 하위 탭(고른 탭 = 남색 글자 +
아래 3dp 남색 선, «메시지» 탭에 미읽음 수) · 오른쪽 끝 **목록 버튼** — 무전 **[사용자]**(채널 추가 패널), 통화 **[주소록]**(주소록 패널).
열려 있으면 버튼이 남색으로 채워진다. 세그먼트는 그 모드에서 **보던 면**으로 간다(모드마다 면을 기억한다).

**좌우 스와이프는 관제의 면 여섯 장을 한 줄로 꿴다**(`DISPATCH_PAGES`):

```
무전:채널  메시지  이벤트 │ 통화:통화  메시지  통화내역
   0        1       2    │    3       4       5
   ←──── 무전의 끝 면에서 더 밀면 통화의 첫 면으로 ────→
```

- 탭 줄은 pager **위에 고정**으로 놓인다 — 면을 밀 때 줄은 제자리에 남고 본문만 미끄러진다(`DispatchBody`). 레일(메뉴)은 밀어서
  바꾸지 않는다 — 세로 레일을 가로 손짓으로 넘기는 것은 예측할 수 없는 동작이다.
- **멀리 가는 이동은 쓸고 가지 않는다**(`goTo` — 한 칸이면 미끄러지듯, 멀면 곧바로). 지나치는 면이 자기를 «지금 면» 으로 알려
  이동이 중간에 서는 것을 막는다.
- **정착한 면만 좌표를 알린다**(`currentPage == targetPage`). VM 이 바꾼 면(탭·세그먼트·배너·사람 메뉴 — 요청)은 `PaneRequest` 가
  **닿거나 손가락이 끊거나 새 요청이 대신할 때까지** 들고, 든 동안은 좌표를 알리지 않는다 — 정착하는 순간 옛 면이 요청을 덮지 않게,
  끊긴 뒤에는 손가락이 멈춘 면을 따른다. pager 는 저장 상태를 되살리지 않는다(면의 기억은 VM 의 `NavState` 가 갖는다).
- 순서는 `PttPane`·`CallPane` 의 면 순서를 그대로 잇는다 — 따로 적지 않는다(두 곳에 적으면 면을 늘렸을 때 한쪽만 고친다).
- **[통화] 의 왼쪽 고정 칸은 pager 위에 한 벌만 얹는다**(`FixedColumn` — 통화 면은 그 폭만큼 왼쪽을 비운다). 면마다 그리면 면을 밀 때
  같이 미끄러지고 펼친 감청·열린 전달 칸 같은 칸 안의 상태가 면마다 따로 논다. 통화 면끼리 밀 때는 제자리, 무전 ↔ 통화 이음매에서만
  그 면과 함께 미끄러져 들고 난다(`fixedShift` — pager 위치를 그리기 단계에서만 읽는다).

**오른쪽 사이드 패널(기본 폭 400)** — 보던 면을 떠나지 않고 한 대상을 자세히 본다.

- **면 위에 겹친다(밀어내지 않는다)** — 데스크톱 패널과 같다. 오른쪽에서 밀려 들어오고(220 ms) 밀려 나간다(180 ms). 면의 배치는
  그대로다 — 패널을 여닫을 때 카드·행이 자리를 옮기지 않는다(말하려던 카드가 패널 때문에 움직이면 안 된다). 패널 아래에 가린 면은
  손짓을 받지 않는다(패널 위의 좌우 밀기가 면 넘기기로 새지 않는다).
- **폭은 왼쪽 가장자리를 끌어 바꾼다**(`PanelGrip` — 가장자리 안쪽 16dp 띠). 320dp 에서, 면이 240dp 는 남는 폭까지
  (`panelWidthAfterDrag`). 고른 폭은 설정에 남아(`Settings.panelWidthDp`) 다음에 열 때도, 다음 기동에도 그대로다 — 가린 것을 봐야 하면
  좁히고 명단이 길면 넓힌다.
- **여는 곳** — 채널 카드·타 채널 행 → 채널 상세(§6.3a) · «채널» 면 [채널 추가하기]·탭 줄 [사용자] → 채널 추가 · [통화] 탭 줄 [주소록] → 주소록 ·
  이벤트 행 → 이벤트 상세 · 메시지 머리 [채널 정보 ›] → 채널 상세(보던 대화 옆에) · 긴급 배너 [채널로 이동]·검색·[PTT 그룹]
  «채널로» → [무전] › «채널» 면 + 채널 상세 · 사람 메뉴 [애드혹에 추가] → «채널» 면 + 채널 추가(그 사람을 더해 둔다).
- **같은 대상을 다시 누르면 닫고, 다른 대상이면 내용을 바꾼다**(쌓지 않는다). 패널 **안에서** 들어간 것만 ← 로 돌아간다(채널
  추가 › 그룹 추가).
- **핀으로 고정하면** 탭·모드·레일을 옮겨도 남는다. 아니면 옮길 때 닫힌다. 패널이 닫히면 고정도 풀린다.
- **가장자리 스와이프로 열지 않는다** — 면 넘기기와 겹친다.
- 머리(56) = 종류 라벨(«채널 상세»·«채널 추가»·«이벤트 상세»·«주소록») 또는 ← · 제목 · 고정(핀) · 닫기. 왼쪽 강조 외곽선 + 그림자.

**뒤로가기**는 연 순서의 역순으로 한 겹씩 — ① 패널 안의 한 겹 → ② 패널 → ③ [통화] 의 첫 면 → ④ [무전] → ⑤ [무전] 의 첫 면 →
⑥ [관제]. **보고 있는 메뉴의 것만** 되돌린다 — [이력] 에서 뒤로가기를 눌렀는데 고정해 둔 관제 패널이 조용히
닫히면 화면은 그대로인데 뒤로가기만 한 번 먹힌다. 다 되돌렸으면 **가로채지 않는다**(가로채면 앱을 벗어날 방법이 없어진다).

**규칙은 값 위의 순수 함수가 갖는다** — `NavState.onNav(레일)` · `toPage(면)` · `toMode(모드)` · `togglePanel`·`showPanel`·
`openChannel(id)`·`closePanel`·`togglePin` · `onBack()`. `MainViewModel` 도 `BackHandler` 의 가로채기 판정(`onBack() != null`)도 같은
함수를 쓴다. 조건을 두 곳에 손으로 적으면 반드시 어긋나고, 어긋나는 순간 뒤로가기를 먹고도 화면이 그대로여서 앱을 못 닫는다.

**[무전]의 면 셋**
- «채널» — **왼쪽 내 채널 칸(폭 470 고정, 카드 2열)** · **오른쪽 타 채널 칸(나머지 폭)**. 둘을 나누는 이유는 하는 일이 다르기
  때문이다 — 내 채널은 **말하는 곳**이라 자리가 움직이면 안 되고, 타 채널(청취 범위)은 **훑는 곳**이라 폭을 더 쓴다.
  - **내 채널 카드(120dp) — 데스크톱 카드와 같은 네 줄** = 1줄 상태 점(긴급 빨강 › 임박 주황 › 참여 중 남색 › 진행(미참여) 진회색 ›
    대기 옅은 회색)·**«핀 번호. 이름»**·라벨(«개별»·«애드혹»·«일제»·«긴급»·«임박» — 이름 바로 뒤)·미읽음 수 / 2줄 발언자·사유 /
    3줄 접속자 / 4줄 참가·경과 + 오른쪽 아래 **둥근 조작 하나(36)**.
    - 핀 번호 = 내 채널 안의 **고정 순서**(데스크톱 핀 번호 `Ctrl+n` 과 같은 번호 — 진행 중이라고 위로 올리지 않는다, 멤버 그룹 뒤에
      개별·애드혹이 생성 순). 종류는 이름이 아니라 **라벨**이 말한다(«4. 김반장 [개별]», 애드혹 = 초대한 사람 앞 둘 + n · [애드혹]).
    - 2줄 = «발언 나 00:14»(내 발언 — 녹색 굵게)·«발언 김관제 00:14»·«발언 없음», floor 사유(거부·회수)는 빨강, 참여 전이면
      «대기 · 멤버 12»(남이 진행 중이면 «세션 진행 중»). 3줄 = 나를 먼저 셋까지 + n(개별 통화는 상대, 참여 전은 «미참여»).
      4줄 = «참가 7 · 12:31»(개별 통화는 경과만).
    - 조작: ✓ 발언 대상(참여 중 반이중 — 켜지면 남색 채움) · 음소거(참여 중 전이중 개별 통화 — 켜지면 빨강) · 참여 전 멤버 그룹은 4줄
      왼쪽이 **[참여][긴급]** 이고 오른쪽 ✓ 도 참여다(참여는 곧 단일 발언 대상 — `join` 의 `pendingTargetId`). 서로 모순이라 한 카드에
      같이 서지 않는다(`CardControl`). **✓ 는 여럿 켤 수 있다** — 한 번의 PTT 가 그 전부로 나간다(동시 발언 — 아래 «발언 바»).
    - **카드 탭 = 채널 상세 패널**, ✓ = 말하기(머리의 힌트 «카드 = 상세 열기 · ✓ = 말하기»). 긴급·임박 카드는 테두리 2·면을 그 색으로
      칠하고(데스크톱 §4.1), 패널에 열린 카드는 테두리 2.5 남색 + 연한 남색 면이다.
    - 글자·여백은 시안 값 — 제목 15 굵게 · 2줄 13 · 3·4줄 12 · 줄 사이 4 · 안쪽 여백 위 10·오른쪽 10·아래 8·왼쪽 12(테두리 안에서 잰
      border-box) · ✓ 는 시안의 선 그림(굵기 2.5, 둥근 끝)이다.
  - 마지막 자리 **«+ 채널 추가하기»** 타일(점선 1.5 · «+» 20) = **채널 추가 패널** — 개별 통화·애드혹 통화·그룹 추가를 한 자리에서
    한다(§6.2e·§6.12). 패널이 열려 있으면 타일을 채운다. 탭 줄의 [사용자] 가 같은 패널을 연다(어느 무전 면에서나).
  - **타 채널 칸** = 머리 «타 채널 [청취 가능] n · 동시 청취 x/상한(§6.2e-1) · 검색» · 거르기(전체·활성·긴급·청취 중) · 행(64) =
    상태 점·이름·2줄 · [청취](청록 외곽선)/[청취 중](청록 채움 — 진행 중 세션이 없으면 [청취] 는 흐리다). 폭이 560dp 이상이면 2열 카드,
    좁으면 1열 행이다. 발밑에 «청취는 듣기만 합니다 — 발언하려면 그 채널에 참여해야 합니다» 를 늘 적는다.
- «메시지» — 대화 목록(340) : 대화(§6.9a). 대화는 **그룹(단톡방)이거나 사람(1:1)**이고, [＋ 새 대화] 가 둘 중에서 고르게 한다.
- «이벤트» — 거르기(220) : 표 : 이벤트 상세 패널(§6.9a). 진행 중인 긴급·임박은 표 위에 고정한다(§6.2a-1).

**[통화] = 왼쪽 고정 칸 + 면 셋 + [주소록] 패널**
- **고정 칸**(폭 470 — [무전] 내 채널 칸과 같은 자리라 모드를 바꿔도 나눔선이 제자리다) = 대표번호 대기열 · **진행 중**(감청의 자리,
  §6.5) · 내 통화 · **관제 그룹원 칸**(2열 — 회선 상태 점 + 이름 + 상태, 울리면 [당겨받기]·통화 중이면 [청취]). 지금 벌어지는
  통화라 **어느 통화 면으로 옮겨도 남는다**(`CallStatus`). 칸 전체가 한 번에 스크롤한다(칸 안에 따로 스크롤하는 목록을 두면 감청을
  펼칠 때 아래 구역이 밀려 사라진다). 울리는 호·착신 카드는 주황 테두리 + 옅은 주황 면, 통화 중 카드는 녹색 테두리로 가른다(받을 것이
  먼저 보이게). 보류한 통화 카드는 한 줄로 접힌다. 그룹원 칸의 탭 = 그 번호를 다이얼 입력란에 채우고 «통화» 면으로, 롱프레스 = 사람 메뉴.
- «통화» — **다이얼패드**(면 가운데, 폭 최대 440 · 키 112×60 — 면이 좁으면 폭을 따라 준다) · 입력 중 주소록 제안 · [←][발신] ·
  [픽업(가장 오래 울린 호)]. 거는 일만 한다 — 벌어지는 일은 고정 칸이 든다.
- «메시지» — 전화 축 문자(SMS/LMS), §6.2e. 모양은 무전 «메시지» 와 같다(대화 목록 : 대화 — 문자는 모두 1:1 이라 종류 라벨이 없다).
  면이 600 보다 좁으면 목록·대화를 한 번에 하나 — 목록에서 고르면 대화, 대화 머리 ← 로 목록.
- **[주소록] 패널**(탭 줄 오른쪽, 통화 모드) — 조직 거르기·검색·사람 목록. 거르기 칩은 «전체» 가 늘 처음이고 그다음은 **조직
  트리 순서**(부모 → 자식, 형제는 `sort`·이름 — 관리 화면의 조직 트리와 같은 `flattenOrgs`, 부모가 범위 밖이면 그 노드가 뿌리),
  고르면 그 조직과 하위 전부다. 행 오른쪽 **[발신]**(곧바로 — 패널은 남고 건 통화는 고정 칸
  «내 통화» 에 선다) · **[문자]**(«메시지» 면의 그 사람 스레드로), **행 탭 = 사람 메뉴**(휴대폰 연락처와 같다, §6.2f). 면이 아니라 패널인
  것은 다이얼패드·문자·내역을 보면서 옆에 펴 두고 걸어야 하기 때문이다(고정하면 면을 옮겨도 남는다).
- «통화내역» — **끝난** 통화만. 진행 중은 고정 칸에 있다(지금 벌어지는 일이므로). 면이 640 보다 좁으면 표 대신 두 줄 행(상대·종류·
  시작 / 번호·통화·울림). 최상위 [이력] 과는 다른 것이다 — 그쪽은
  날짜를 골라 보는 과거 조회이고 녹취 재생이 거기 있다. 데스크톱 ⑥ 과 같은 바로가기를 단다: 행의 [재발신]·[문자](상대가 있는 1:1
  통화 — 착신·부재·발신·전달, `CallLogRow.canRedial`. 당겨받기·감청 행에는 없다. 외부망 번호의 [문자] 는 스레드를 열어 막힌 이유를
  적는다 — §6.2e), 머리의 [이력에서 보기](→ [이력]) 와 [CSV](필터와 무관하게 내역 전부·시간순·화면 표와 같은 열 `callLogCsv`,
  UTF-8 BOM — 저장 자리는 시스템 문서 선택기로 사람이 고른다, `rememberCsvExport`). 롱프레스 = 사람 메뉴.

> 발언 대상을 카드에 남기는 것은 «포커스 ≠ 발언 대상» 이라는 불변 때문이다. 여러 채널을 잡아 두고 말하는 조작이므로 채널을
> 하나씩 열어서 지정하게 만들면 안 된다. **카드 탭 = 포커스(«메시지»·«이벤트» 면이 따라간다) + 채널 상세, 카드 ✓ = 발언 대상** —
> 둘은 끝까지 섞지 않는다.

### 6.3a 채널 상세 — 오른쪽 사이드 패널

채널을 «고른 뒤» 하는 일을 모은다. 보던 목록 위에 겹쳐 열리고(§6.3), 같은 카드를 다시 누르면 닫힌다(`ChannelPanel`).

- **머리** = «채널 상세» · 채널 이름 · 고정 · 닫기. 그 아래 한 줄 요약 «멤버 그룹 · 참가 7 · 12:31 · 편성 12 · 발언 김관제»
  (종류 · 참가 · 경과(또는 상태) · 편성 · 발언자) 와 «긴급»/«임박 위험»/«일제 통화» 라벨.
- **조작 줄** — 그 채널이 지금 받아 줄 수 있는 것만 선다(`ChannelHeadUi` 가 자격을 든다):
  - 참여 전 멤버 그룹: [참여] · [긴급 참여] · [일제 통화]. 참여 중: [나가기] · [긴급](진행 중 긴급 상향 — 확인을 한 번 받는다,
    그룹 전원에게 긴급이 선다) 또는 [긴급 해제](해제 자격 = 내가 올린 긴급 ∨ user profile `allow-cancel-group-emergency` — 서버 판정과 같은 식,
    TS 24.379 §6.3.3.1.13.4, `DispatchSession.capabilities`). 비인가·다른 긴급 사용자 송출 중이면 서버가 403 으로 거절하고 토스트는 해제 거절
    문구다(데스크톱 §9 와 같은 문장). 임박 위험만 걸린 채널에는 [임박 해제](자격 = `allow-cancel-imminent-peril` — 개시자 예외
    없음, §6.3.3.1.13.6)가 함께 선다. 판정은 한 곳이다(`canCancelCondition` — 배너의 [해제] 와 같은 식).
    [긴급]·[긴급 참여] 는 긴급 그룹콜 자격(`allow-emergency-group-call`)이 없으면 보내지 않고 이유를 말한다.
    상향·하향은 in-dialog re-INVITE 다(TS 24.379 §10.1.1.2.1.3~5, 코어 `Call.setCondition`) — 서버가 거절하면 코어가 이전 값으로
    되돌리고 토스트가 사유를 적는다(`applyCondition`).
  - [✓ 발언 대상](참여 중 반이중) 또는 [음소거](전이중 개별 통화 — 켜지면 경고색).
  - [메시지 n ›] — 그 그룹의 SDS 대화를 «메시지» 면에 연다. 멤버 그룹만 — 그룹 SDS 는 멤버에게만 열린다(TS 24.282).
  - ⋮ [편집]·[삭제] — 고칠 수 있는 편성 그룹에만 선다: 내 소유(GMS `is_owner`)이거나 서버가 관리 범위로 준 행(`canManage` — 데스크톱
    범위 채널 `CanEdit = IsManageScope || IsOwner`). 관리 목록은 [PTT 그룹] 을 열기 전이면 비어 있어 패널을 열 때 받아 둔다
    (`ensureLoaded`). [편집] 은 [PTT 그룹] 의 그 그룹 편집 폼으로 가고(고치던 폼이 있으면 덮지 않고 그 폼으로 데려가 이유를
    적는다, §6.12), [삭제] 는 한 번 더 묻고 지운 뒤 패널을 닫는다. 개별 통화·애드혹은 편성이 없어 ⋮ 가 없다.
  - 청취 범위 채널: [청취]/[청취 중지] · 청취 음량 막대 · «청취 전용 — 발언 요청 불가». 청취는 `listenOnly` 합류라 서버가 Floor Taken 의
    `permissionToRequest=0` 을 준다 — 그 사실을 **누르기 전에** 알아야 한다([dispatch_center.md](dispatch_center.md) §5.6).
- **[일제 통화]**(TS 24.379 §4.12 — 그룹 종류가 아니라 호 속성, [mcptt_broadcast_group_call.md](mcptt_broadcast_group_call.md) U6) —
  멤버 편성 그룹에 **진행 중 통화가 없을 때만** 켜진다(진행 중 호는 일제 통화로 바꿀 수 없다 — TS 24.379 §10.1.1.3.1.1 15) 는 합류다.
  채팅 그룹은 서버가 broadcast-ind 를 무시한다 — 그룹 종류 `GroupInfo.sessionType` 을 모르면 누를 때 GMS 그룹 문서로 확인해 채운다).
  **누르는 동안 개시하고 말하며 놓으면 끝**(발언 바 PTT 와 같은 제스처, 잠금 발언이면 누를 때마다 켜고 끈다): 개시 INVITE 가 암묵적 발언
  요청이라(TS 24.380 §14.2.5 — `implicitFloorRequest`) PTT 를 따로 누르지 않고, 놓으면 호 성립 전 = CANCEL(말하지 않은 일제 통화는 열지
  않는다) / 뒤 = Floor Release → 서버의 B-bit Floor Idle 에 코어가 호를 해제(§6.2.4.6.4). 개시되면 [나가기] 자리가 생기지만 버튼은 뗄
  때까지 남는다(사라지면 제스처가 취소된다). 누름은 **손가락이 떨어질 때까지**다 — 개시되는 순간 줄의 다른 버튼([참여]·[긴급 참여])이
  바뀌어 버튼이 제자리에서 밀려나는데, 버튼 밖으로 나간 것을 놓음으로 읽으면 가만히 누르고 있어도 일제 통화가 곧바로 끝난다. 그룹 종류 조회 중에 놓으면 개시 직후 끝낸다(`PttChannelsViewModel.broadcast*`).
  개시 실패는 토스트로 알린다(§6.2a-2). **일제 통화용 그룹은 그룹 이름으로 알린다** — 그룹 문서(TS 24.481)에 일제 통화 표시 요소가 없다.
- **일제 통화 표시**: 착신 mcptt-info `broadcast-ind` 또는 floor 메시지 B-bit(`SessionItem.isBroadcast` — 늦게 합류한 leg 은 B-bit 로만
  안다)면 카드·패널에 «일제», 2줄 앞에 «일제 통화 · 발언을 놓으면 종료 / 수신 전용». 수신 멤버(Floor Taken Permission 0)는 발언
  대상이 될 수 없다(`canCheck`). 일제 통화로 연 호의 첫 서버 floor 메시지에 B-bit 가 없으면 서버가 일반 통화로 연 것이라 ⑤ 에 남긴다
  (200 OK 의 승인 표시 `mc_granted` 는 서버 floor 메시지가 아니라 판정에 쓰지 않는다).
- **누가 있나 — [접속 n | 편성 m]** 세그먼트. «접속» = 지금 로스터에 접속한 사람 전부(`rosterPreview` — 발언자 → 나 → 서버 순서, 잘리지
  않는다). «편성» = 그룹 문서의 명단 × 지금 상태(`detailMembers` — 참여·발언 중·미참가, 미참가는 옅게·뒤로). 두 질문(지금 누가 있나 /
  편성이 누구인가)은 다르므로 둘 다 둔다. 개별 통화·애드혹은 편성이 없어 «접속» 만.
- **사람 줄(50)** = 머리글자(이름마다 고정된 여덟 색 중 하나 — 나 = 남색) · 이름(«(나)»·«· 발언 중»)·«PTT 5002 · 관제과 1팀»(PTT 주소록 소속) · «의장» · [개별]·[SDS].
  **줄 탭 = 사람 메뉴**(§6.2f — 통화·문자·기록…). «나» 줄은 누르지 않는다.
- 채널이 사라지면(세션 종료·편성 제외) 패널을 강제로 닫지 않고 그 사실을 적는다 — 보던 것이 말없이 사라지는 편이 더 나쁘다.

**채널 상세의 사람·조작 규칙**

- **개별·애드혹**은 편성이 없어 로스터가 오지 않는다 — «접속» 에 나 + 상대(개별) / 나 + 초대한 사람(애드혹)을 세운다. 거기서 그
  사람에게 [개별]·[SDS] 를 건다.
- **청취자·보류한 멤버**도 «접속» 에 선다(«청취»·«보류» 표기) — 청취자는 역할이 투명(`listen_visibility=visible`)일 때만.
- **[청취]** 는 들을 세션이 있을 때만 눌린다(타 채널 행과 같은 조건 — 끄는 것은 언제나 된다).
- **[긴급 해제]·[임박 해제]** 는 배너와 **같은 경로**(`cancelCondition` — 성립·진행 중 변경·청취 leg 판정 + ⑤ «해제 요청»)다. 진행 중
  **[긴급]** 은 편성 그룹에만 서고(개별·애드혹은 걸 때 긴급으로 건다), 성립한 통화에 앞선 변경이 끝난 뒤에만 보낸다.
- **애드혹 카드 제목** = 초대한 사람 앞 둘 + «+n», 남이 연 것은 «애드혹 · <개시자>»(`adhocTitle` — 전부 «애드혹» 이면 둘을 구별할 수 없다).
- **미참여 멤버 그룹 카드** 2줄 = 진행 중이면 «진행 중 · 참가 n», 대기면 «대기 · 멤버 n · 마지막 hh:mm»(앱이 본 마지막 세션이 끝난 때 —
  `GroupInfo.lastSessionEndMs`). 타 채널 행도 «마지막 세션 hh:mm · 길이» 를 적는다.
- 내 채널 카드의 순서는 **서버(GMS) 목록 순**이다 — 핀 번호가 데스크톱과 같다.
- 채널 상세를 여는 것은 **읽음이 아니다** — 글은 «메시지» 면에서 그 대화가 보일 때 읽음이 된다. 채널을 열면 «메시지» 의 [따라가기] 가
  그 채널의 대화로 옮겨 간다(`focusPane`).
- 고정(핀)한 [사용자] 패널은 발신 뒤에도 남는다(연달아 부른다). 걸면 «채널» 면으로 간다.

### 6.3c 시각 토큰 — 데스크톱 관제 앱과 같은 색

색의 정본은 데스크톱 관제 앱의 테마(`windows/dispatch-desktop/Themes/Light.xaml`·`Dark.xaml`, [dispatch_desktop_ui.md](dispatch_desktop_ui.md)
§3.2 «색»·«시각 언어»)다 — 두 앱이 **같은 역할에 같은 값**을 쓴다(관제사가 자리를 옮겨도 색의 뜻이 같다). 배치는 태블릿 것이다(§6.3).
화면 코드는 16진수를 직접 쓰지 않고 **역할 이름만** 쓴다(`ui/Theme.kt` `Palette`·`Tokens`). 같은 값을 Material3 색 체계에 싣는다 —
메뉴·대화상자·입력칸이 `MaterialTheme.colorScheme` 을 읽으므로 따로 고치지 않은 조각도 같은 톤이 된다.

| 역할 | 토큰 | 밝게 | 어둡게 | 쓰는 곳 |
|---|---|---|---|---|
| 표면 | `paper` / `canvas` / `bar` / `fill` | #FFFFFF / #F8FAFC / #F5F7FC / #EEF1F8 | #161D2C / #111827 / #10172A / #1E2738 | 본문·카드 / 옅은 바탕 / 띠(탭 줄·발언 바) / 채운 면(절 카드·라벨) |
| 레일 | `rail` / `railActive` | #ECEFF8 / #D9DFFA | #0D1322 / #262E5E | 왼쪽 레일 / 고른 항목 알약 |
| 선 | `hair` / `divider` / `line` / `edge` | #EDF0F7 / #E3E7F1 / #C9CFE0 / #AAB3CC | #1F2839 / #2A3346 / #3B4660 / #4A5677 | 행 사이 / 구역 / 컨트롤 테두리 / 강조 외곽선·입력칸 |
| 글자 | `ink` / `ink2` / `muted` / `faint` | #161A2B / #334155 / #5F6F86 / #7D8CA1 | #E6E9F1 / #CBD5E1 / #93A1B8 / #64748B | 기본 / 두 번째 / 메타 / 자리 글자 |
| 브랜드 남색 | `primary` / `primarySoft` / `primaryInk` / `primaryLine` | #4F46E5 / #EEF2FF / #4338CA / #4F46E5 | #5B5FEF / #232A52 / #A5B4FC / #6366F1 | 채운 주 행동·고른 세그먼트·수 배지 / 선택의 연한 면 / 선택 글자 / 선택 외곽선 |
| 발언·통화 녹색 | `talk` / `talkSoft` / `talkInk` / `talkFill` | #16A34A / #EAFBF0 / #15803D / #15803D | #22C55E / #122D1E / #4ADE80 / #15803D | 등록·발언 점 / 연한 면 / 글자 / PTT 발언 중·승인 칩·[발신]·[응답] |
| 긴급 빨강 | `emg` / `emgSoft` / `emgInk` / `emgFill` | #C62828 / #FDECEA / #7A1212 / #C62828 | #F87171 / #361519 / #FCA5A5 / #DC2626 | 긴급 점·외곽선 / 배너·카드 면 / 글자 / 채운 [채널로 이동]·끝내기 |
| 임박·착신 주황 | `peril`+`onPeril` / `ring` / `ringSoft` / `ringInk` | #F59E0B+#161A2B / #D97706 / #FEF5E6 / #92400E | #F59E0B+#161A2B / #FBBF24 / #2B2313 / #FCD34D | 임박 배너 채움·요청 중 PTT / 울림 점 / 연한 면 / 글자 |
| 보류·전달 파랑 | `held` / `heldSoft` / `heldInk` | #2563EB / #E8F0FF / #1D4ED8 | #60A5FA / #172A46 / #93C5FD | 보류·전달 라벨, 잠금 발언 테두리, 직접 착신 배너 |
| 감청·경보 보라 | `mon` / `monSoft` / `monInk` / `monFill` | #7C3AED / #F5F0FF / #6D28D9 / #7C3AED | #A78BFA / #261F40 / #C4B5FD / #7C3AED | 감청 중 표시·인라인 상세, 긴급 경보 배너 |
| 청취·문자·영상 청록 | `listen` / `listenSoft` / `listenInk` / `listenFill` | #0F766E / #E3F6F3 / #0F766E / #0F766E | #2DD4BF / #122D2B / #5EEAD4 / #0F766E | [청취]·청취 중, 문자 라벨, 개별 통화·새 영상 배너 |

규칙은 넷이다. **채운 주 행동·고른 세그먼트·수 배지 = 남색**(먹색으로 채우지 않는다 — 글자색 `ink` 는 채움에 쓰지 않는다) ·
**선택 = 연한 남색 면 + 남색 외곽선·글자**(고른 행 = 왼쪽 3dp 남색 띠, 고른 카드 = 테두리 2.5 남색) · **상태색은 세 값**(기본 = 점·외곽선,
Soft = 연한 면, Ink = 그 위 글자 — 채운 면은 Fill 이고 어둡게에서도 Fill 은 진하게 둔다) · **통화 행동 = 녹색, 끝내기·거절·삭제 = 빨강**.
비활성은 색을 바꾸지 않고 흐리게(불투명도 0.4) 한다.

- **아바타** — 머리글자 원(그룹은 둥근 네모)의 색은 **이름에서 정한 여덟 색 중 하나**다(`avatarHueOf` — 이름의 FNV-1a 해시를 7 로 나눈
  나머지, 빈 이름은 여덟째 회색. 데스크톱 `AvatarHueConverter` 와 같은 식이라 같은 사람이 두 앱에서 같은 색이다).
- **크기** — 레일 80(항목 = 알약 56×32 + 이름 12sp) · 상단 바 64(이름 20sp 굵게) · 탭 줄 48(세그먼트 칸 104 · 하위 탭 112) · 발언 바 80
  (PTT 150×64 반경 12) · 사이드 패널 400(끌어서 320 부터, 머리 56) · 내 채널 칸 470(카드 120 반경 10) · 통화 고정 칸 470 · 타 채널 행 64 ·
  사람 줄 50 · 사용자 줄 48 · 주소록 줄 56 · 대화 줄 68 · 이벤트 표 줄 46 · 다이얼 키 112×60(반경 10) · 배너 64. 알약 버튼 36
  (작게 30·32·크게 44), 네모 버튼(통화 축 조작·표 안 조작) 30·36, 숫자 배지 = 남색 알약 11sp.
- **글자 모양** — Material3 기본은 본문에 줄 간격 24sp·자간 0.5sp 를 얹어 크기만 바꾼 글자도 그 줄 간격을 가진다 — 카드 120 에 네 줄이
  들지 않는다. 그래서 테마 글자 모양 전부를 **자간 0 · 줄 간격 1.35em** 으로 둔다(`CimsTypography`). 크기는 §10a 의 `Type` 단계다.
- **글꼴** — 시스템 글꼴(기기의 Noto Sans CJK)을 쓴다. 시각(이벤트 표·상세·배너 경과)은 고정폭.
- **공용 조각**(`ui/Components.kt`·`ui/Tag.kt`·`ui/BannerBar.kt`) — `PillButton`(종류 `Pill` — 데스크톱 `Pill.*`)·`RectButton`(`Rect` —
  `Rect.*`)·`Label`/`Tag`(`LabelStyle` — `Tag.*`)·`FilterPill`·`CimsFilterChip`·`Segmented`·`CountPill`·`SectionHead`·`StatusDot`/`dotColor`·
  `Initial`·`BannerBar`(`BannerTone`). 화면은 Material `Button`·`FilterChip` 을 직접 쓰지 않는다 — 화면마다 따로 그리면 반경·굵기·색이
  조금씩 어긋나 «같은 것인가» 가 흔들린다.
- **테마** — 밝게·어둡게는 설정을 따른다. **기본은 밝게**(데스크톱과 같다)이고 어둡게는 설정에서 고른다(관제실·차량의 어두운 자리).
  시스템 막대 아이콘도 테마를 따른다. **앱 아이콘**은 남색 바탕에 흰 «CIMS»(적응형 아이콘 — 단색 테마 아이콘 포함, 전경 그림은
  `gen-icon.ps1` 이 그린다)이고 기동 창도 밝은 바탕이다.

**화면 바탕 `bg`**(#F2F4FA / #0B0F17 — 데스크톱 `Brush.Bg`)는 로그인·[PTT 그룹]·[관리] 의 가장 낮은 면이다 — 그 위에 카드(`paper`)가
뜬다. 띠(`bar` — 탭 줄·발언 바)와는 다른 값이다(어둡게에서 «가장 어두운 바닥 + 떠 있는 카드» 대비가 선다).

### 6.4 데스크톱 전용 기능의 처분

| 기능 | 태블릿 | 이유 |
|---|---|---|
| 칸 경계 끌기(`layout.json` `Seams`) | **사이드 패널 폭만** 끈다(§6.3) — 면 안의 칸은 고정 배치 | 태블릿은 면이 한 가지 일만 해 나눌 칸이 없다. 패널은 면을 가리므로 폭을 고르게 한다 |
| 화면 별창(`ScreenWindow`) | **없음** — 레이어 전환 | Android 에 별창 개념이 없다 |
| 감청 창 [창으로 ↗](인라인 확장 옆의 선택) | **없음 — 통화 행의 인라인 확장만** (§6.5) | 창이 여럿일 수 없고, 통화와 감청은 한 몸이다 |
| 트레이 최소화 | **없음** — Foreground Service 알림 | 같은 목적을 알림이 맡는다 |
| 전역 핫키 | **없음** — 하드 키보드를 전제하지 않는다. 모든 조작이 화면에 있다(PTT 는 측면 키) | §7 |

지우는 것은 **데스크톱 어포던스뿐**이고 기능(메뉴 소속·동작·응답 문구)은 건드리지 않는다.

### 6.5 감청·청취 — 축이 아니라 상태다

**최상위에 [감청] 을 두지 않는다.** 감청(통화 Join)과 청취(PTT `listenOnly` 합류)는 그 자체로 하는 일이
아니라 **어떤 통화·어떤 채널에 붙는 상태**다. 축을 세우면 같은 통화가 «진행 중» 과 «감청» 두 곳에 나오고,
행마다 상태가 갈라져 어느 쪽이 최신인지 알 수 없다. 그래서 **켜는 자리·보는 자리·끄는 자리를 하나로** 둔다.

| | 어디에 | 켜기 / 끄기 | 켜져 있을 때 |
|---|---|---|---|
| **VoLTE 감청** | [통화] › «통화» 의 «진행 중» 행 | [청취] / [청취 종료] | 행이 «청취 중» + **그 행이 펴져** 상세 |
| **PTT 청취** | [무전] › «채널» 의 범위 채널 행 | [청취] / [청취 중지] | 태그 «청취 중» + **목록 맨 위로** |

**감청 행의 인라인 상세**(`TapDetail`) — 은닉/투명 · 재생 라우트 · **소스 귀속 두 줄**.
서버가 CMP tap 으로 양 peer 를 **SSRC 2개로 분리 인도**하므로(RFC 3911 Join `a=recvonly` → `RELAY_TAP_*`,
RFC 5576 `a=ssrc … label`, [dispatch_center.md](dispatch_center.md) §5.4) 줄이 둘이다 — 믹싱은 단말이 한다.
서버가 섞어 주면 «누가 말했는지» 가 사라져 귀속이 깨진다. 소스가 갈라져 오지 않는 서버에서는
"수신 중 — 소스 라벨이 아직 없습니다" 한 줄로 그 사실을 적는다(없는 것을 지어내지 않는다).

- **은닉/투명은 서버가 준 역할 속성**이다(`listen_visibility` = `hidden`|`visible`, dispatch_center §5.6 — `visible` 일 때만 «투명»,
  모르면 은닉. `DispatchSession.listenHidden` 한 곳이 판정한다) — 화면이 고르지 않는다.
  적어 두는 이유는 «지금 내 청취가 상대에게 보이는 상태인가» 가 관제사의 행동을 바꾸기 때문이다.
- **청취 중인 범위 채널은 목록 맨 위로** 올린다. 청취는 «켜 두고 잊는» 조작이라 아래에 묻히면 몇 개를
  듣고 있는지 모른 채 상한에 걸린다(§6.2e-1).
- **다른 화면으로 옮겨도 청취는 계속된다.** 끝내는 것은 [청취 종료]·[청취 중지]뿐이다.
- 레벨 미터는 `MediaSource.active` 로만 켠다 — SSRC 별 수신 레벨 관측 API 가 아직 없다(§11).
- **leg 마다 음량 막대**(0~200%, 1.0 = 원음 — 데스크톱 감청·청취 창의 [음량]) — 감청은 행의 인라인 상세, 청취는 그
  채널 상세 패널(`RxLevelRow`). 여러 leg 을 같이 들을 때 하나만 줄이거나 키운다. 반영은 코어 `setRxLevel`(호의 수신
  레벨)이고, 값은 호가 끝날 때까지 세션이 든다(`DispatchSession.rxLevels` — pjsua 가 callId 를 되쓰므로 호가 끝나면 지운다).
  끄는 동안 계속 반영하므로 거절(미디어가 아직 없음)은 토스트로 띄우지 않는다 — 데스크톱도 결과를 보지 않는다.

**타 채널의 정렬·활성** — 긴급(임박 포함) › **청취 중** › 진행 › 대기. «활성» 은 로스터로 아는 세션 ∨ 내가 청취 중(로스터가 아직 안
왔어도 청취 중인 채널은 활성이다). 청취 중인 채널이 일제 통화면 «일제 통화» 가 붙는다(`ScopedCard.broadcast`).

### 6.6 팝오버의 번역

데스크톱의 비모달 `Popup` 을 두 갈래로 나눈다.

| 데스크톱 팝오버 | 태블릿 | 이유 |
|---|---|---|
| 개별·애드혹 통화 구성 | **오른쪽 사이드 패널**(채널 추가 — 그룹 추가와 한 자리, §6.2e) | 보던 채널 면을 떠나지 않고 사람을 고른다 |
| 주소록 | **오른쪽 사이드 패널**(탭 줄 [주소록], §6.2b) | 다이얼패드·문자·내역을 보면서 옆에 펴 두고 건다 |
| 다이얼패드, 문자 스레드 | **[통화] 의 면**(«통화»·«메시지») | 관제석에서 상시로 쓰는 것이라 열고 닫을 표면이 아니다 |
| DTMF 3×4, 전달(blind/attended) | **카드 인라인 확장** | 한 통화에 묶인 조작이라 맥락이 보여야 한다 |
| 채널 편집(`GroupEdit`) | **전면 시트** — [PTT 그룹] 화면과 폼 공유 | 데스크톱은 오른쪽 드로어 |
| 사람 메뉴 | 롱프레스 → 앵커 메뉴 | 우클릭 대응 |
| 통합 검색(Ctrl+K) | 상단 바 검색 → 전면 시트 | 상단 바가 유일한 입구다 |

**전달**(데스크톱 §4.3 전달 팝오버와 같은 두 갈래) — 카드의 [전달] 이 펴는 칸에 대상(내선·번호)과 **그룹원 칩**(나 제외 —
누르면 대상 칸을 채울 뿐이다)을 두고, 버튼을 둘 둔다. 전달 칸·DTMF 칸은 **그 호의 것**이다 — 호가 끝나면 닫고 비운다. 호 번호는
다시 쓰이므로 남겨 두면 다음 호의 카드가 칸이 열린 채로 서고 앞 통화에서 친 대상으로 전달된다.

- **[전달]** = blind — REFER(RFC 3515). 받아들여지면 카드에 «전달 중 → 이순경» 을 적고 ⑥ 에 전달로 남긴다. 이 leg 은
  서버가 넘기면 BYE 로 끝난다.
- **[상담 전달]** = attended — 원 통화를 보류하고 대상에게 상담 호를 먼저 건다(`startConsult`). 상담 호 카드는 «상담»
  배지를 달고 [전달] 대신 **[전달 완결]**(상대가 받은 뒤에만 — 원 통화를 REFER + Replaces 로 넘긴다, RFC 3891,
  `completeConsult`)·**[취소]**(상담 호를 끊고 원 통화의 보류를 푼다, `cancelConsult`)가 선다. 원 통화 카드에는 «전달 중»
  이 적힌다. 상담 호가 끝나면 ⑥ 에 «상담 전달» 로 남는다.
- 상담 호 ↔ 원 통화의 짝은 세션이 든다(`SessionItem.consultFor` — 데스크톱 `ConsultFor`). 세션 이벤트가 발신 결과보다
  먼저 와도 짝이 빠지지 않게 이미 선 세션에도 붙인다(`noteConsult`).

### 6.7 상태 소유

패널 경계가 곧 상태 소유 경계다. Windows 판의 분해를 그대로 따르되, **어느 화면에 놓이는지만** 다르다(§6.3).

| 상태 단위(이 문서의 ①~⑥ — 데스크톱의 자리) | 태블릿의 자리 | 소유 상태 | 소스 |
|---|---|---|---|
| 발언 바 | 모든 화면 하단(상시) | 발언 대상 집합(상한 없음 — 동시 발언, 아래), 대상별 승인/대기/거부, 남은 발언 게이지. 칩 = 그 채널 포커스, 칩의 × = 그 대상 하나만 빼기(요청해 둔 floor 도 푼다 — 데스크톱 칩과 같다) | `onFloor` |
| ① 내 채널([무전] 위 왼쪽) | [무전] › «채널» | 채널 카드 집합, 포커스 | `calls()` + `onGroupCall`·`onRoster` |
| ② 범위 채널(타 채널 — [무전] 위 오른쪽) | [무전] › «채널» | 청취·관리 범위 카드, 필터·검색, 청취 수 | `Profile.dispatch.pttTargets[]` + conference 구독 |
| ③ 일반통화([통화] 왼쪽 «통화») | [통화] 고정 칸 + «통화» | 대표번호 대기열·진행 중·내 통화·그룹원 띠(고정 칸) · 다이얼패드(«통화» 면) | `onDialogInfo`(BLF) + 로컬 호 |
| ④ PTT 메시지([무전] 아래 왼쪽) | [무전] › «메시지» | 스레드·말풍선·disposition | `onSds` + 발신 token 상관 |
| ④′ 문자([통화] «기록» 의 문자 줄기) | [통화] › «메시지» | 스레드·말풍선(SMS/LMS) | `onMessage`(SIP MESSAGE) + 발신 token 상관 |
| ⑤ PTT 이벤트([무전] 아래 오른쪽) | [무전] › «이벤트» | 이벤트 링 버퍼(진행 중 행 없음 — 진행 중 긴급·임박 고정 행만) | `onFloor`·`onGroupCall` + 이력 폴링 |
| ⑥ 통화 내역([통화] «진행 중» + «기록») | [통화] › «통화내역» | 끝난 호 + 오늘 데스크 | dialog 쌍 결합 + 이력 폴링 |
| 감청(«진행 중» 행 확장 + 선택 감청 창) | [통화] 고정 칸 «진행 중» 의 행 안 | 없음 — 세션 목록을 그대로 투영 | `PHONE_MONITOR` 세션 + `MediaSource` |
| 영상 채널(채널 상세 «영상» 절 · «새 영상» 배너 · 카드 «영상 n») | 채널 상세 패널 · 배너 층 · 내 채널 카드 | 영상 호·보는 송출·내 송출 상태(세션 목록과 따로 — §6.14) | `transmission`·`reception` + `transmissionInfo` 스냅샷 |
| 긴급 경보 배너 | 배너 층 | 그룹·발신자마다 하나(§6.2a-1) | `emergencyAlert` |

**UI 는 코어 상태의 투영이다.** 진행 중 상태는 구독(dialog/conference)이 정본이고, 끝난 것만 서버 통합 이력
(`GET /provisioning/history`, 2.5초 커서 폴링)이 채운다 — **폴링이 live 를 대체하지 않는다**.

**동시 발언 = 단말 팬아웃**(dispatch_desktop_ui.md §4.1). 3GPP 에 UE 의 다중 그룹 동시 발언 절차가 없어, 발언 대상(카드 ✓) 세션마다
floor 를 **따로** 요청하고 코어가 승인된 세션마다 같은 마이크를 결선한다(세션별 floor participant). 그래서 대상 집합에 상한이 없고
서버 변경도 없다. 규칙(`TalkRules`·`PttChannelsViewModel`):

- PTT 누름 = 대상 **전부**에 `floorRequest`, 뗌 = **요청해 둔 세션**에 `floorRelease`(지금 대상이 아니라 — 누른 채 대상이 바뀌어도
  옛 채널로 마이크가 남지 않는다). 대상에서 빠진 세션은 그 자리에서 놓는다.
- 발언 바 = PTT 글자 «발언 중» / «발언 n/m»(일부만 승인) / «요청 중» / «잠금», 둘째 줄 «말하세요»·«발언권 요청 중»·«동시 발언 n채널»·
  «다시 누르면 끝»(잠금)·«놓으면 끝납니다»(내가 연 일제 통화). 칩은 대상마다 승인(녹색 채움)·요청/대기(주황)·거부(빨강)를 따로 보인다.
- **남은 발언 게이지**(발언 중에만) = 승인된 대상 중 **가장 적게 남은** 것 + 발언 경과. 기준은 Floor Granted 의 Duration
  (`SessionItem.grantedSec` — 서버가 싣지 않으면 가득 찬 채). 15 % 아래거나 코어가 `TALK_LIMIT` 을 알리면 빨강.
- **잠금 발언**은 요청해 둔 대상이 **전부** 발언·요청·대기에서 벗어나야 풀린다 — 한 채널의 회수·시한이 나머지 채널의 발언을 풀지
  않는다(`TalkRules.lockEnded`). 요청은 갔는데 발언·요청·대기인 카드가 한 번도 서지 않은 채 끝났으면(곧바로 거부) 잠시 뒤
  (2초) 잠금을 푼다 — 풀릴 계기가 없어 «잠금» 이 남으면 다음 누름이 풀기만 한다.
- **«내가 건 호 우선»** — 내가 건 애드혹 그룹 통화·반이중 개별 통화는 세션이 서는 순간 단일 발언 대상이 된다(발신자가 곧 말하려는
  채널). [참여] 도 같다. 그 뒤 관제사가 바꾼 대상은 되돌리지 않는다.
- 요청·해제는 코어가 이벤트 없이 상태만 바꾸므로 명령 뒤에 floor 스냅샷을 당긴다(`pullFloor`) — 칩의 «요청» 이 곧바로 선다.
- 요청·해제는 **한 줄로** 보낸다(`floorCmd`) — 서로 다른 코루틴에서 나가면 짧게 눌렀다 뗄 때 해제가 요청을 앞질러 요청만 남는다.
- **소유를 버릴 때는 놓고 버린다.** 잠금이 끝났다고 읽거나 대상이 정리될 때, 카드가 사라졌어도(멤버에서 빠짐) 코어에 살아 있는
  호면 `floorRelease` 를 보낸다(`isCallAlive`). 끝난 호에는 보내지 않는다 — 코어가 호 id 를 되쓰므로 낡은 id 의 명령은 다른 호에 닿는다.
- **일제 통화 한 버튼의 누름 상태**는 호가 끝나면 풀린다 — 개시가 돌아오기 전에 곧바로 거절된 호(403·404·480)도 푼다. 화면이
  닫히면 놓은 것으로 친다(개시 블록은 세션 수명에서 돌아, 닫힌 뒤 돌아온 호도 끝낸다).
- **편성 그룹의 일제 통화**도 세션이 서면 단일 발언 대상이 된다 — 누르고 말하는 동안 발언 바가 그 채널·남은 발언·«놓으면 끝납니다» 를
  말한다. 남이 연 일제 통화는 ✓ 가 켜지지 않는다(Permission 을 받기 전에도).
- 대상이 **전부** 거부되면 PTT 버튼이 1초 동안 빨강 «거부» 로 선다. 대상 칩을 누르면 그 채널 상세가 열린다.
- 일제 통화가 일반 통화로 열렸으면(첫 서버 floor 메시지에 B-bit 없음) ⑤ 줄과 함께 **경고 토스트**로 알린다.
- 그룹 목록 조회가 실패하면 알린다(«그룹 목록을 받지 못했습니다» — 실패가 이어지는 동안 한 번). 조용히 넘기면 «멤버 그룹이 없습니다» 가
  서버 문제인지 편성 문제인지 가를 수 없다.
- **참여 대기**(`pendingTargetId`)는 그 참여가 성립 없이 끝나면 거둔다 — 남겨 두면 한참 뒤 그 그룹에 선 착신 세션이 지금 말하던
  채널의 발언권을 놓게 한다.

### 6.8 Windows ViewModel ↔ 태블릿 대응

화면이 적은 만큼 VM 도 적다. **합친 곳은 상태가 하나이기 때문**이고, 나눈 곳은 수명이 다르기 때문이다.

| Windows VM | 태블릿 | 비고 |
|---|---|---|
| `MainViewModel` | `MainViewModel` | 최상위 화면·탭 소유 + 화면 VM 보유(세션 교체 시 재바인드) |
| `TalkBarViewModel` | **합침** → `PttChannelsViewModel` | 발언 대상·floor 가 한 상태라 나누면 두 벌이 된다. 바는 `TalkBar` 컴포저블 |
| `PttChannelsViewModel`·`ScopedChannelsViewModel` | 그대로 | |
| `PttOriginateViewModel`·`CallOriginateViewModel` | **합침** → 각 패널 VM | 발신은 패널 안 한 줄이라 VM 을 따로 두지 않았다 |
| `CallDeskViewModel` | 그대로 | 2열 배치(§6.3) |
| `McDataMessagesViewModel` | `PttMessagesViewModel`(SDS·파일 FD) — [무전] › «메시지» | 채널 상세 패널에는 두지 않는다(두 곳에 있으면 «이 채널 것인가» 가 흐려진다, §6.3a). 파일 전송의 몸통은 세션의 `FilePlane`(§6.2e) |
| `SmsMessagesViewModel` | `SmsMessagesViewModel`(SMS) — [통화] › «메시지» | 같은 모양·다른 망. 스레드·보관을 갈라 둔다(§6.2e) |
| — | `RecipientPicker` **신규** | 데스크톱은 «기록» 상대 줄이나 번호칸에서 받는 사람이 정해진다. 태블릿은 면이라 «새 대화» 가 따로 필요하고, SDS·SMS 가 같이 쓴다(§6.9a) |
| `PttActivityViewModel` | 그대로 | `CallActivityViewModel` 은 `CallDeskViewModel.callLog` 로 |
| `SessionHistoryViewModel` | `HistoryViewModel` + `SegmentPlayer` | `MediaElement` → `MediaPlayer`(미디어 스트림) |
| `GroupAdminViewModel`+`GroupEditViewModel` | **합침** → `PttGroupsViewModel`(+`EditForm`) | 편집이 같은 자리의 인라인 폼이라 수명이 하나다(§6.12) |
| `DirectoryAdminViewModel` | `AdminViewModel` | |
| `MonitorWindowViewModel` | **없음** — 감청은 «진행 중» 행의 인라인 확장(`TapDetail`) | 창당 상태가 없고, 축을 세우면 같은 통화가 두 곳에 나온다(§6.5) |
| `DispatchSummaryViewModel` | **없음** — 요약 띠 자체를 두지 않는다 | 발언 바와 레일·탭 배지가 같은 것을 말한다(§6.10) |
| `PersonActionsViewModel` | `PersonMenu`·`SearchSheet` + `MainViewModel.runPersonAction` | VM 을 두지 않았다 — 사람 목록은 ③ VM(`CallDeskViewModel.people`)이 이미 묶고, 메뉴·검색은 자기 상태가 없다(§6.2f) |
| `DeskViewModel` | **없음** — 상단 바가 직접 그린다 | 데스크톱에서는 상단 바 VM(§3.2 — 신원·등록 점등·오디오 요약·감청 중 N)이다. 태블릿은 상태를 가진 쪽(`MainViewModel`·`SettingsStore`)이 그대로 그려 VM 이 따로 필요 없다 |
| `LoginViewModel`·`SettingsViewModel` | `MainViewModel`·`SettingsStore` | 로그인 화면은 상태가 셋뿐이라 합쳤다 |
| `DispatchSession`(+`.McVideo`) | `DispatchSession` + 평면 확장(`PttPlane`·`PhonePlane`·`SmsPlane`·`AlertPlane`·`DiscoveryPlane`·`HistoryFeed`·`FilePlane`·`VideoPlane`) | 세션은 하나, 파일은 평면별. 순수 규칙은 평면마다 떼어 JVM 시험을 단다 |
| `HistoryClient`(폴링) | `HistoryFeed`(세션 평면) | 창 조회는 `ManagementClient.history`(§6.11), 폴링 병합은 세션이 한다(§6.2d) |
| `Notifications`(배너·토스트) | `BannerBar` + `EmergencyBanners`·`IncomingBanners`·`VideoBanners` · `NoticeHost` | 배너는 세션 Flow 의 투영이라 따로 든 목록이 없다 |
| — | `LayoutStore` **없음** | 창 위치·칸 경계가 없다. 사이드 패널 폭만 `SettingsStore` 에 남는다(§6.3) |

### 6.9 저장

| 데스크톱 | 태블릿 |
|---|---|
| `MessageStore`(SQLite) | `MessageStore` — SQLite `messages.db`, 같은 스키마 의미(`kind` = SDS/SMS · 파일 열 · `owner` = 로그인 ID 격리). Room 없이(§6.2e) |
| 받은 파일 `다운로드\CIMS` | 앱 전용 `files/mcdata/received`(받은 파일) · `files/mcdata/sent`(보낸 사본) — FileProvider 로 연다, 보관 정리를 따른다(§6.2e) |
| `SettingsStore`(json) | `SettingsStore` — SharedPreferences(DataStore 를 쓰지 않는다 — §5 `SecureStore` 와 같은 판단) |
| `LayoutStore` | **없음** |
| `directory-cache.json` | 같은 캐시(서버 전화번호부 + ETag, §6.2b) + 가져온 로컬 CSV(`directory.csv`) |
| `ActivityLog`(링 버퍼·CSV) | 같은 링 버퍼(메모리) + CSV 내보내기(⑤·⑥ 머리 [CSV], 시스템 문서 선택기) |
| `AppLog`(7일) | **없음** — logcat 과 코어 로그 수준(설정 «진단»)뿐이다 |
| 자격(DPAPI) | `SecureStore`(Keystore) |

**설정은 바꾸는 즉시 저장하고 다음 기동 때 그대로 선다** — 접속점·로그인 id·자동 로그인·신뢰 앵커·서버 검증·로그 수준·
출력 라우트·선호 이어폰·자동 복귀·잠금 발언·자동 보류·당겨받기 피처코드·동시 청취 상한·메시지 보관 일수·따라가기 둘(«메시지»·«이벤트»)·테마·사이드 패널 폭·영상(카메라·«영상 보내는 중 무전»). 키
의미와 기본값은 데스크톱 `SettingsStore` 와 같다. 따라가기는 사람이 머리의 토글을 누른 것만 남긴다 — 스레드를 직접 열 때
잠시 끄는 것은 남기지 않는다(데스크톱 `ToggleFollow` 와 같다). [따라가기] 를 끄면 **지금 보던 대화**에 머문다(예전에 고른
대화로 돌아가지 않는다 — 다음 발신이 엉뚱한 대화로 나간다). 숫자 설정(동시 청취 상한·보관 일수)은 **칸을 떠날 때**(다른 칸·완료·
시트 닫힘) 범위로 잘라 적는다 — 치는 도중의 값(«30» 에서 한 글자 지운 «3»)을 곧바로 적으면 다음 기동이 그 값으로 보관을 지운다.

주소록 병합·이름 조회의 키는 **E.164 정규형**이다. 내선은 표시 라벨이라 주소로 쓰지 않는다.

### 6.10 관제 요약 띠 — 두지 않는다

데스크톱도 주 창에는 요약 띠가 없고 화면 별창에만 붙인다(dispatch_desktop_ui.md §3.5 — 발언 대상·발언 상태·[PTT]·
대기열·문자 미읽음·내 통화·감청 수·[관제로]). 별창이 없는 태블릿에는 **없다.**

근거는 그 자리가 이미 채워졌다는 것이다 — 발언 바가 모든 화면 하단에 상시로 있고(§6.3), 나머지 수는 레일·탭의
배지가 말한다: 레일 [관제] = 아래 셋의 합 · 관제 탭 줄의 [통화] 칸 = 응답 대기(울리는 착신 + 대표번호 대기열) · [무전] › «메시지»
탭 = 미읽음 SDS · [통화] › «메시지» 탭 = 미읽음 문자 · 레일 [관리] 점 = 저장 안 한 폼 · 상단 바 «감청 중 n» = 열린 감청·청취. 청취 수는 배지가 아니라 «채널» 면 타 채널
머리의 «동시 청취 n/상한» 이다(§6.2e-1 — 상한이 있는 값이라 개수만으로는 부족하다).
띠를 같이 두면 같은 정보를 두 번 그리게 되고, 둘이 어긋나면 어느 쪽이 맞는지 알 수 없다.

서버 인증서 만료 경고(잔여 ≤ 30일, [sip_tls_signaling.md](sip_tls_signaling.md) §8.6.2)는 띠가 없어졌으므로
**설정 화면의 «서버 인증서» 행**과 로그인 시 배너가 표면이다.

### 6.11 [이력] 화면

끝난 통화·무전 세션의 **하루 창 조회 + 녹취 재생**. 기능·낱말·판정은 Windows 관제 앱의 [이력]
([dispatch_desktop_ui.md §4.6](dispatch_desktop_ui.md))과 같다 — 규칙의 정본은 그쪽이고, 여기는 태블릿에서 달라지는 것(배치·터치
조작·재생기)과 구현 구조를 적는다. 서버 계약 [android_ue_provisioning.md](android_ue_provisioning.md) §3-2/§3-2a/§3-4.
진행 중·오늘의 실시간 흐름은 [관제] 가 정본이고 이 화면은 서버가 보관한 사본을 본다(폴링 없음).

```
┌ 도구줄 52 ── [무전|통화] ◀ 날짜 ▶ [오늘]  검색  [전체|무전|영상] [빈 세션 묶기]            ⟳ ┐
├ 시간대 밴드 ─ 시간대별 무전 [세션 수|발언 수]        칸을 누르면 …           [전체 시간] ┤
│               00 01 02 … 23 (24칸 — 값·농도)                                            │
├ 목록 360 ───────────┬ 상세 패널(나머지) ───────────────────────┬ 영상 칸 300 ──────────┤
│ 요약                 │ 머리 · 숫자 칸                            │ (영상이 있는 녹취를   │
│ 09시          3건    │ 통화: 당사자 · 진행                        │  틀 때만 열린다)      │
│ ┌ 카드 ┐ ┌ 카드 ┐    │ 무전: ① 참여자 ② 발언 ③ 이벤트            │                       │
│ …                    ├ 녹취 재생 바(아래 고정) ──────────────────┤                       │
└──────────────────────┴───────────────────────────────────────────┴───────────────────────┘
```

통화도 무전도 **왼쪽 카드 목록(시간대 묶음) + 오른쪽 상세 패널**이다. 목록 칸은 360dp 고정이고 나머지를 상세 패널이 쓴다(본문 1200dp
기준 약 840dp). 영상이 있는 녹취를 틀면 상세 오른쪽에 영상 칸 300dp 가 열려 상세가 그만큼 준다.

**도구줄**
- 종류 = 관제 탭 줄과 같은 **[무전 | 통화]**(같은 순서·같은 모양, 처음은 무전) · 날짜 [◀] 날짜(달력) [▶] [오늘] · 검색(이름·번호·그룹 —
  주소록 이름과 번호의 다른 표기 `010…` ↔ `+8210…` 에도 걸린다. 번호 모양의 질의는 번호의 **어느 표기에든** 들었으면 걸린다 —
  치다 만 `010333` 이 `+821033334444` 에 걸린다(주소록 제안과 같은 규칙)) · 무전만 서비스 거르기 칩 **[전체 | 무전 | 영상]**(받은 항목에
  `service` 축이 실려 있을 때만 보인다) · 무전만 토글 칩 **[빈 세션 묶기]**(기본 켬) · 새로고침.
- 종류·날짜가 바뀌면 자동 조회하고 시간대 필터는 해제된다. **미래 날짜로는 갈 수 없다**(서버 스캔이 48시간 버킷 상한이라 창은 하루다).
- 요약은 목록 칸 맨 위 한 줄이다 — `날짜[ HH시][ 발언 있는 세션] · n건 · 진행중 n · 녹취 n건 · 영상 n건 · 발화 합 … · 빈 세션 n건 → m묶음`
  (0 인 항은 적지 않는다).
- 창 조회마다 로그 한 줄 `history window ptt 2026-10-01: 37 items (mcvideo 3, service axis yes) · query 820 ms`(통화는 `(video n)`) —
  «콘솔에는 있는데 앱에는 없다» 를 서버 응답에서 가른다.

**시간대 밴드** (24칸 — 그날의 분포이자 필터)
- 제목 = 센 것: 통화 «시간대별 통화 횟수»(서버 `hours` — INVITE 시각) · 무전 «시간대별 무전» + 세그먼트 **[세션 수 | 발언 수]**.
  세션 수 = 서버 `hours`(세션 시작 시각, `limit` 절삭 전 전체, 없으면 항목의 축 시각으로 센다) / 발언 수 = 받은 세션의 `turnCount` 를
  **세션 시작 시간대**에 더한 값.
- 칸 = 그 시간대만, 다시 누르거나 [전체 시간] = 해제(그 모드에서 0 인 칸은 누를 수 없다). [발언 수] 에서는 **그 시간대의 발언 있는
  세션만**(`turnCount` > 0 또는 녹취 있음) 남는다. 농도 = 그날 가장 많은 칸 대비 연한 남색(0.08~0.45), 고른 칸 = 남색 테두리 2.5.
- 하루 상한(1000건)을 넘어 받은 세션이 `hours` 보다 적은 시간대는 덜 센 값이라 칸에 **"+"** 를 붙인다.
- **시간대 칸은 서버에 다시 묻는다.** 지역 필터로만 좁히면 앞 시간대가 영영 안 보인다 — 서버는 창 안에서 **최근 `limit`(1000) 건만**
  주는데 `hours` 는 **절삭 전 전체**로 낸다(csc `dispatch_history.finish_rows`). 그래서 밴드에는 건수가 뜨는데 눌러 보면 빈 목록이고,
  잘린 이력과 그 녹취에 닿을 길이 없다. 창을 그 한 시간으로 좁혀 다시 물으면 상한 안에 들어온다. 시간대를 서버가 좁히므로 지역에서
  또 거르지 않는다(경계가 어긋난다). 좁혀 받은 시간대는 [발언 수] 칸도 그 결과로 다시 세어 "+" 가 사라진다. **절삭되면 그렇다고 화면에
  쓴다** — 조용히 일부만 보여 주면 «없는 통화» 로 읽힌다.

**목록 — 카드** (최근이 위, `LazyColumn` 이라 보이는 카드만 만든다)
- **시간대 묶음** 머리 "HH시 … n건"(밴드와 같은 축 — 통화 INVITE·무전 세션 시작). 건수는 줄 수가 아니라 세션 수다(빈 세션 묶음은 묶은 수).
- 카드 = 모서리 10 · 테두리, 고른 카드 = 남색 테두리 2.5 + 연한 남색 면, 긴급 = 빨강 테두리 2 + 연한 빨강 면. 이름이 길면 이름만
  줄이고 라벨·시각은 오른쪽에 붙는다.
- **통화 카드**(두 줄): 결과 태그(응답 회색 · 통화 중/호출 중 초록 · 부재/실패/취소 주황 · 거절/오류 빨강 — 판정은 데스크톱 §4.6) ·
  발신 → 착신 · 영상/긴급/녹취 · 호출 시각 / 통화 시간 · 울림 또는 끝난 이유 · 울림.
- **무전 카드**(세 줄): 상태 점 · 대상(그룹 이름 / 1:1 `A ↔ B` / 애드혹 `A 외 n명`) · 영상/개별/애드혹/전이중/진행 중/긴급/녹취 · 시작 시각 /
  개시·참여 n명 / 길이 · 발언 n회 · 말한 시간. 영상 세션은 «영상» 라벨(청록)과 "길이 · 송출 n회 · 보낸 시간".
- **빈 세션 묶음**: 연달아 나오는 같은 시간대·같은 서비스·같은 그룹·같은 개시자의 빈 세션(`turnCount` 0 이 실려 왔고 녹취 없는 종료 세션)
  2건 이상을 카드 한 장으로 — 뒤에 겹친 카드 두 장 · «빈 세션 n건» · 가장 이른~가장 늦은 시작 시각 · "각 30~31초 · 발언 0회" +
  **[n건 펼치기]**/[접기]. 고르면 가장 최근 세션이 열리고, 펼치면 한 줄씩(시각 범위·길이 · 발언 0회) 고를 수 있다. 펼침은 묶음마다
  기억한다(열쇠 = 시간대 + 가장 이른 세션). 고른 세션이 묶음에 접혀 들어가면 그 묶음 머리가 선택 표시를 받는다 — 선택·상세·재생은 그대로다.

**통화 상세** = 머리(발신 → 착신 · 결과·영상·긴급 태그 · 날짜 시각) → 숫자 칸 넷(울린 시간 · 통화 시간 · 끝난 이유 · 종류) → 당사자
(아바타·이름·번호) → 진행(울림 주황 : 통화 남색 비율 막대 + 호출·응답·종료 시각) → 녹취 재생 바(아래 고정).

**무전 세션 패널** = 머리(대상 · 종류·영상·전이중·진행 중·긴급·floor 정책 태그 · 시작~종료·길이 · [▶ 처음부터]·[정지]) → 숫자 칸(길이 ·
참여 · 발언 회 · 말한 시간 · 있을 때만 최대 동시 발언·녹취 수) → ① 참여자 → ② 발언 → ③ 이벤트 → 녹취 재생 바(아래 고정).
- ① **참여자** = 입퇴장 기록 ∪ 화자. 이름표(아바타 색 = 이름) · «개시» 태그 · 말한 사람은 레인 색 점 + "발언 n회".
- ② **발언** = 화자 레인 위 턴 막대. **막대를 누르면 그 발언부터 재생**한다(재생 바와 같은 헤드, 믹스). **[틈 줄임]**(기본 켬) = 말이
  없는 구간(세션 머리·꼬리 포함)을 짧게 접는다 — 접는 상한 = 발언 시간 합의 5 %(1.5~15초), 그보다 짧은 틈과 발언 구간은 시간에 비례한
  그대로다. 접힌 틈은 레인 전체 높이의 띠 "⋯", 눈금은 접힌 틈 뒤 발언 덩어리가 시작하는 시각. 끄면 시간에 비례한 축이고 눈금은
  벽시계의 고른 간격(1초~1시간 — `axisStepSec`)이다.
- **발언 턴의 원자는 녹취 세그먼트의 화자 구간**(`tracks[].speakers[]`)이다 — 동시 발언이면 같은 시각에 슬롯이 여럿이라 막대도
  여럿이다. 화자 구간은 음성 트랙에서 읽고, 음성 없이 영상만 있는 송출 구간은 영상 트랙에서 읽는다. 트랙이 없는 세그먼트는 세그먼트
  자체가 한 턴이다.
- ③ **이벤트** = floor 중재 op 8종 + 입퇴장 시간순. 층 토글 [발언권 n]·[입퇴장 n], 같은 초의 같은 입퇴장은 한 줄 "A 외 n명 입장",
  op 별 부가 정보(선점·동시·대기 순번·거절 사유·회수 유예).
- 세션 상세·녹취가 오기 전에도 목록 항목만으로 머리와 숫자 칸은 선다(상세 조회의 상태는 숫자 칸 옆에 적는다).
- **영상 세션**(MCVideo 그룹 호 — 항목 `service: mcvideo`, 항목에 없으면 녹취 메타 `service`·세그먼트 `type: mcvideo`)은 같은 패널을
  낱말만 바꿔 쓴다 — 머리 «영상» 라벨 + "chat · 동시 송출 2", 숫자 칸 송출 회·보낸 시간·최대 동시 송출, ② 가 «송출», 이름표 "송출 n회".

**녹취 재생 바** (통화·무전 공용)
- 막대 하나 = 녹취 전체의 벽시계 구간(통화 = 첫 세그먼트 시작~마지막 끝, 무전 = 세션 시간축 — 틈 줄임과 무관하게 시간에 비례한다).
  재생하면 차오르며 `지금 / 전체`(mm:ss)와 그 순간의 실제 시각을 보인다.
- **막대를 누르면 그 지점부터** — 시각을 «그 시각을 담은 세그먼트 + 파일 안 오프셋» 으로 풀어 튼다(같은 세그먼트 안이면 위치만 옮긴다).
- 조작 = [재생/일시정지] · 통화 [−10초]·[+10초] / 무전 [이전 발언]·[다음 발언] · 속도 [1× 1.5× 2×] · [다시 변환](변환 실패 표식을 지우고
  그 자리에서). 무전은 막대 위에 **발언 막대**(화자 레인 색, 겹치는 동시 발언은 두 번째 줄)와 재생 헤드, 조작 줄 끝에
  «지금 <이름> · 발언 #n»/«말 없는 구간», **말 없는 구간 건너뛰기**(기본 켬 — 끄면 세그먼트 사이 빈 시간을 시계로 흘려 다음
  세그먼트에서 이어 튼다). 세그먼트가 끝나면 다음 세그먼트로 이어진다.
- **영상 칸** — 세그먼트에 영상이 있으면(`has_video` 또는 영상 트랙 — 영상 통화 녹취·MCVideo 송출 구간. 서버가 내는 MP4 는 음성 AAC +
  영상 H.264 한 파일이다) 그 세그먼트를 트는 동안 상세 오른쪽에 «녹취 영상» 칸이 열려 영상을 그린다(비율 유지, 머리 [정지]·재생 중
  표시, 아래 `지금 / 전체 · 실제 시각`). 재생 바가 그대로 조작한다(일시정지는 칸을 닫지 않는다). 영상 없는 세그먼트·말 없는 구간으로
  넘어가거나 [정지] 하면 닫힌다. 영상이 이어지는 세그먼트 사이에서는 닫았다 열지 않는다.

**재생기** — `SegmentPlayer`(MediaPlayer 한 개)
- **미디어 스트림**으로 낸다 — 통화·무전과 섞이지 않게. 파일을 그 안의 한 지점에서 열고(`seekTo`), 속도는 `PlaybackParams` 로 건다
  (멈춘 재생기에 속도를 주면 재생이 시작되므로 틀고 있을 때만 건다). 영상은 영상 칸의 `TextureView` 면을 `setSurface` 로 붙여 그린다 —
  영상 세그먼트는 받는 동안 칸을 먼저 열어, 파일을 열 때 면이 이미 붙어 있다.
- VM 이 200 ms 마다 재생기 위치로 헤드를 옮긴다. 화면을 떠나면 재생을 멈추고 6시간 지난 임시 파일을 정리한다.
- **재생할 때마다 서버에 다시 묻는다** — 세그먼트 파일을 캐시로 건너뛰지 않는다. 녹취 재생은 서버가 요청마다 범위를 다시 보고 감사를
  남기는 동작이다(`E-AUD-016 tap_mode=recording`, [dispatch_center.md §5.7b](dispatch_center.md)). 같은 세그먼트 안에서 옮기는 것은
  열린 파일의 위치만 바꾼다. 202(변환 중)는 0.7→1.5초 간격 최대 120초 대기(본문 `status` 로 «녹음 진행 중»·«변환 중» 을 가른다),
  재변환(`retry=1`)은 첫 요청에만 싣는다. 받은 파일은 매번 새 이름으로 쓴다(재생 중 파일을 덮으면 깨진다).

**서버 필드** — 이력 항목은 §3-2 의 종류별 확장 필드를 전부 싣는다: 통화 `callType`·`state`·`inviteTime`/`answerTime`/`endTime`·
`endReason`·`sipStatus`, 무전 `sessionKind`·`startTime`·`groupName`·`memberCount`·`people[]`·`turnCount`(+ 실려 왔는지 `hasTurnCount` —
없으면 0 은 «발언 없음» 이 아니라 «모름» 이다)·`speakerCount`·`totalSpeechMs`·`talkMs`·`maxConcurrent`·`floorControl`·`floorPolicy`·
`maxTalkers`·`service`·`mcvideo{sessionType,maxTransmitters}`(snake_case 도 읽는다). 녹취 메타는 `service` 와 트랙 `kind`(없으면 audio).
서비스 축이 없는 서버에서는 영상 세션이 라벨 없이 무전 세션처럼 서고, 세션을 고르면 녹취 메타로 영상 세션임을 알아 패널 낱말·영상
칸은 맞게 나온다.

**데스크톱과 다른 것**

| 데스크톱 | 태블릿 | 이유 |
|---|---|---|
| 시간대 칸 = 받은 목록을 지역에서 거른다 | 그 한 시간 창을 서버에 다시 묻는다 | 하루 상한에 잘린 시간대의 이력·녹취에 닿는다(위) |
| 목록 1 : 패널 3, 경계 끌기·더블클릭 | 목록 360dp 고정 | 터치에 끌 경계를 두지 않는다 — 카드 한 줄이 드는 폭으로 고정 |
| 마우스를 올리면 툴팁(숫자 칸 풀이·참여자 입퇴장·줄인 틈의 실제 시각) | **누르면** 그 풀이가 숫자 칸 아래 한 줄로 | 터치에는 올림이 없다 |
| 재생 바에 마우스를 올리면 그 지점 시각 말풍선 | 막대를 **짚은 채 끌면** 말풍선이 따라오고 떼면 거기서 튼다 | 같은 일의 터치 조작 |
| 발언 타임라인 확대 = Ctrl+휠·[−][+][1:1], 빈 곳 드래그·Shift+휠로 이동 | [−][+][1:1] 버튼(×1.25, 최대 ×64) + 가로 스크롤 | 핀치는 막대 누름·가로 스크롤과 제스처가 겹친다. 배율을 바꿔도 보던 가운데가 그대로 있게 스크롤을 옮기고, 세션을 바꾸면 ×1 |
| 세그먼트 파일을 로컬에 캐시 | 다른 세그먼트로 옮길 때마다 서버에 다시 묻는다 | 인가·감사가 요청 단위다(위) |
| 영상 칸 372 | 300dp | 본문 폭 |
| 화면 [↗ 별창으로]·`--ui-preview` 개발 스위치·그리기 시간 로그 | 없음(Compose `@Preview` 표본으로 대신) | 창이 하나다 |

**구현** — `ui/history/`: `HistoryScreen.kt`(껍데기·도구줄·밴드·목록) · `HistoryDetail.kt`(통화 상세·세션 패널) · `HistoryPlayerBar.kt`
(재생 바·영상 칸) · `HistoryViewModel.kt`(조회 축·선택·재생 헤드) · `SegmentPlayer.kt`. 판정은 전부 **순수 함수**라 기기 없이 시험한다
(§9): `HistoryRows.kt`(행 표시값·결과 태그·빈 세션 묶음·시간대 묶음·밴드·요약) · `HistoryAxis.kt`(발언 턴·틈 줄임 시간축 `TimeAxis`·
세션 패널·이벤트 묶기) · `HistoryPlayback.kt`(재생 바 `PlayerBar` — 시각 ↔ 세그먼트 + 오프셋). 위치는 전부 **비율(0~1) × 폭**이다 —
폭(dp)에 ms 를 곱하는 정수 셈이 없어 한 시간 넘는 세션에서도 넘치지 않는다. 화면은 `HistoryScreenContent`(순수 컴포저블)가 그리고
세션·VM 없이 Preview 로 선다(`HistoryScreenPreview.kt` — 통화·무전·영상 세션·빈 세션 묶음 표본).

**시간대 칸은 밴드와 같은 축(시작 시각)으로 거른다.** 하루 목록을 다 받았으면(서버 상한 1000 에 안 걸렸으면) 그 목록에서 고른다 —
서버에 다시 묻지 않는다. 하루가 상한에 잘렸을 때만 그 시간대를 서버에 다시 묻는데, 서버는 창을 **종료 시각**으로 자르므로
(`dispatch_history`) 끝을 한 시간 넓혀 받고 시작 시각으로 다시 거른다(그 시간대에 시작해 한 시간 넘게 이어진 것은 빠질 수 있다).
⟳ 와 [이력에서 보기] 는 하루 전체부터 **다시 조회**한다(`refresh` — 고른 시간대는 남는다. 같은 종류로 들어와도 방금 끝난 세션이 보인다).


### 6.12 [PTT 그룹] 화면

레일 [PTT 그룹] — 그룹을 **보고·만들고·고치고·지우는 곳**이다. 데스크톱 [PTT 그룹] 화면([dispatch_desktop_ui.md](dispatch_desktop_ui.md) §4.7)과
같은 기능·같은 시각 언어(알약 버튼 · 각진 필터 칩 · 19px 라벨 · 아바타 · 흰 카드)다. 카드 둘 — 왼쪽 **그룹 목록**(300) : 오른쪽 **선택 그룹 카드**.
카드·폼 조각(흰 카드 · 절 카드 · 이름표 · 입력칸 40 · 스위치 · 띠)은 [관리] 화면과 한 벌을 같이 쓴다(`ui/groups/FormParts.kt` — 데스크톱
`Card`·`FormCard`·`FormLabel`·`Field.Form`·`Switch.Form` 과 같은 모양).

**목록.** 머리 = «그룹 N»(필터 전 전체 수) · [↻] · **[+ 새 그룹]**(남색 알약 — 그룹 생성 자격이 있을 때만) / 검색(그룹 이름·id) / 필터 칩
[전체|멤버|내 소유] / 오류는 머리 아래 연한 빨강 띠. 행(62) = 그룹 아바타(모서리 8, 색 = 이름 해시) · 그룹명 + **관계 라벨**(멤버 = 연한 남색 ›
청취 범위 = 청록 › 소유 › 범위 — 하나만) · 인원 / id · 소속(조직 경로 — PTT 주소록의 조직 트리로 풀고, 트리에 없는 코드는 그대로) ·
**서비스 칩 «음성»·«영상»**. 고른 행 = 연한 남색 면 + 왼쪽 남색 띠.

- 목록 원천 = `GET /provisioning/directory/groups`(관리 범위 ∪ 내 소유 ∪ 청취 범위 ∪ 내 멤버). **보기 전용 행이
  섞인다** — [편집]·[삭제]는 서버가 내려 준 `canManage` 행에만 붙고, 필드가 없는 구 서버는 전부 관리 가능으로 읽는다.
- **관리 범위가 없으면** 그 API 는 403 `no_directory_admin` 이다 — 세션의 **GMS 목록(내 멤버 그룹)** 을 목록으로 쓰고 내 소유만
  고친다(`managedOf` — 데스크톱 `FromSession`). 그룹 생성 자격만 받은 사람도 제가 만든 그룹은 여기서 고친다. 이때 목록은 세션의 그룹을
  따라간다(만들고 지우는 대로).
- **«영상» 칩**은 MCVideo 그룹으로 **알려진** 행에만 선다 — 관리 목록에는 서비스가 없어 내 영상 채널(세션 `GroupInfo.mcVideo` —
  MCVideo user profile)과 열어 본 그룹 문서(`mcvideo` 몫)로만 안다. 문서로 확인한 것은 목록을 다시 받아도 잇는다.
- **선택은 손으로 한다** — 화면에 들어와도 첫 행을 저절로 고르지 않는다. 선택(과 그 그룹 문서)은 채널 상세 패널의 «편성» 목록과
  같은 것이라(§6.3a — `selectById`), 저절로 옮기면 패널이 보던 그룹의 편성이 다른 그룹 것으로 바뀐다.

**상세**(행 한 번 누르기). 머리 = 아바타 46 · 이름 · 라벨([멤버]/관계 · [영상] · [세션 진행 중] · 보기 전용 행은 «보기 전용») · 둘째 줄
id · 소속 · 오른쪽 **[채널로]** · **[편집]**(남색 알약) · **[삭제]**(빨강 외곽선) — 뒤 둘은 `canManage` 행만. 본문 = **정보 칸 여섯**(연한 면 —
소유자(내 것은 "이름(나)", 남의 것은 주소록 이름) · 소속 · 세션 종류 · 그룹 우선순위 · 긴급(긴급 통화·긴급 경보 중 허용된 것) ·
청취 노출(은닉/투명 — 관제 역할의 `listen_visibility`)) · **능력 칩**(MCPTT 음성 · 메시지(SDS) · 파일(FD) · MCVideo chat/편성 · 송출 n ·
암호화 · affiliation 필요 — 문서가 주는 것만) · «멤버 N» / «affiliation M» 과 **멤버 2열** · 바닥 = 목록 범위 안내 한 줄. 문서를 받기 전의
정보 칸은 «…» 이고, 못 받으면 사유가 연한 빨강 띠로 뜬다.

- **[채널로]** 는 [관제] › [무전] 으로 돌아가 그 그룹의 채널 상세 패널을 연다 — 멤버 그룹은 내 채널 카드, 청취 범위 그룹은 타 채널 행.
  **합류하지 않는다**(참여는 그 채널의 [참여] 가 한다). 관리 범위로만 보이는 그룹은 태블릿에 채널이 없어 버튼을 세우지 않는다.
- **[삭제]** 는 확인을 받는다 — 그룹 이름·id·멤버 수("멤버 n명의 단말에서도 사라집니다"), 세션이 돌고 있으면 그 사실도 적는다.
- **멤버 2열**(`detailMembers` — 데스크톱 `GroupAdminViewModel.RefreshDetailMembers` 대응). 채널 카드 3줄이 접은 로스터의 «전체» 가
  여기다(§6.3b). 칸 = 아바타 · 이름 · (나) · [의장] · 번호 / 상태.
  - **명단은 GMS 문서가, 상태는 로스터가** 준다 — `발언 중`(녹색 라벨) / `참여`(녹색 글자) / `미참가`(줄 흐림). 문서에 없고
    로스터에만 있는 사람은 멤버가 아니므로 나오지 않는다(이 표의 질문은 «편성된 사람이 지금 있나» 다).
  - `connected` 와 `listener` 를 **둘 다 «참여»** 로 센다 — 그 자리에 있다는 뜻은 같다. 은닉 청취자는 서버가
    로스터에서 빼므로(dispatch_center.md §5.6) 앱이 거를 것이 없다.
  - **미참가는 뒤로**, 같은 등급끼리는 문서 순서. 갱신마다 줄이 뒤섞이면 읽을 수 없다.
  - 번호 비교는 정규형으로(로스터 `tel:+8210…` vs 문서 `sip:010…`). 발언자는 표시명·번호 둘 다로 맞춰 본다.
  - 문서는 **선택할 때 한 번** 받고(저장 뒤에는 다시 받는다), 로스터·발언자가 바뀌면 같은 문서로 다시 겹친다 — 구성은 XCAP
    변경으로만 바뀌므로 상태 갱신마다 문서를 다시 받지 않는다.

**편집 폼**([편집]·[+ 새 그룹] — 같은 화면의 인라인 폼, 별창 없음). 편집 중에는 목록이 잠기므로(저장·취소로만 나온다) **폼이 본문 폭 전체를
쓴다**: 머리(제목 · «편집 중») / 왼쪽 384 = 속성 **절 카드 넷**(스크롤) · 가운데 = **PTT 주소록** 카드(검색 · **[표시된 n명 추가]** · 행 [＋ 추가]) ·
오른쪽 = **멤버** 카드(멤버 n · 줄마다 **[필수/선택]**·**[의장/참가자]** 토글 — 켜지면 채움 · [×] 빼기) / 바닥 고정 = 오류 + [취소]
[저장 / 그룹 만들기]. 문서를 받는 동안은 본문을 비워 둔다(받기 전에 고친 값이 도착한 문서에 덮이지 않게).

| 절 | 칸 | `GroupDoc` | 범위·기본값 |
|---|---|---|---|
| 기본 | 그룹 이름 · 그룹 id(신규만 편집 — 아래에 `tel:<id>`) · 세션 종류 [편성(prearranged)][채팅(chat)] | `displayName` · `uri` · `sessionType` | 일제 통화는 그룹 종류가 아니라 호 속성이라 선택지에 없다([mcptt_broadcast_group_call.md](mcptt_broadcast_group_call.md) §5) |
| 기본 | 그룹 우선순위 · 최대 참가자 | `priority` · `maxParticipants` | 0~15 · 0 = 무제한 |
| 그룹 호 | 유지 시간(T4) | `hangTimerSec` | 0~3600초, 기본 30, 0 = 미사용(편성 그룹) |
| 그룹 호 | 최대 통화 시간 | `maxDurationSec` | 0~86400초, 기본 3600, 0 = 무제한 |
| 그룹 호 | 시작 최소 응답 | `minNumberToStart` | 0~65535명, 기본 0 = 기다리지 않음(TS 24.379 §6.3.3.3) |
| 그룹 호 | 필수 멤버 대기(TNG1) | `ackTimeoutSec` | 1~300초, 기본 5 |
| 그룹 호 | 필수 멤버 대기가 끝나면 | `ackAction` | 통화 포기(abandon, 기본) / 없이 진행(proceed) |
| 그룹 호 | 참가자 정보 구독 허용 | `allowConferenceState` | 기본 허용 — 끄면 멤버의 conference 구독이 403 |
| 허용 · 한도 | 긴급 그룹콜 · 긴급 경보 · 메시지(SDS) · 파일 전송(FD) · 미디어 암호화 · affiliation 필요 | `emergencyCall` · `emergencyAlert` · `allowSds` · `allowFd` · `encryption` · `requireAffiliation` | 스위치 |
| 허용 · 한도 | 메시지 최대 · 자동 수신 최대 | `maxSdsSize` · `maxAutoRecv` | octet, 기본 10000 · 1048576, 0 = 무제한 |
| 서비스 | MCPTT 음성 | — | 늘 켬(잠김) |
| 서비스 | MCVideo 영상 · 호 방식 · 동시 송출 상한 · 최대 통화 시간(TNG3) · 수신 유지 시간(T5) · 시작 최소 응답 · 그룹 우선순위 · 참가자 정보 구독 · 종단간 보호 | `mcvideo`(`McVideoGroupAttrs`) | chat(기본)/편성 · 1~16 기본 2 · 0~86400 기본 3600 · 0~3600 기본 30 · 0~65535 기본 0 · 0~255(비우면 미기재) · 기본 허용 · **false 명시**(칸 잠김) |
| 멤버 | [필수]/[선택] · [의장]/[참가자] | `GroupMember.required` · `role` | 필수 = 개시자 응답 전에 이 멤버의 응답을 기다린다(TNG1) |

요소·범위·기본값은 콘솔 «서비스 › PTT 그룹» 편집·데스크톱 폼과 같다(TS 24.481 §7.2.2·§7.2.4.2 — dispatch_desktop_ui.md §4.7·§10.6 표).
값의 규칙은 순수 함수(`ui/groups/GroupForm.kt` — `editFormOf`·`groupDocOf`·`mcVideoOf`)가 갖고 화면은 그리기만 한다.

- **미기재 보존.** 그룹 호·한도 칸은 `GroupDoc` 에서 **`null` = 미기재**다. 편집을 열면 GET 값으로 채우고(미기재 칸은 기본값으로 보인다),
  **문서에 없던 칸이 기본값 그대로면 저장 때도 미기재**로 둔다 — 폼을 연 것만으로 서버 값을 명시값으로 굳히지 않는다. 값을 바꾸면
  범위로 자른 값을 싣고, 문서에 있던 칸은 기본값과 같아도 그 값을 싣는다. 숫자 칸은 지우는 도중의 빈 칸을 값으로 되돌려 쓰지 않는다
  (숫자가 되는 순간에만 값을 올리고, 칸을 떠나면 값으로 되돌려 보인다).
- **멤버 우선순위(`user-priority`)는 읽은 값을 되돌린다.** XCAP PUT 은 `<list>` 가 있으면 멤버 전체를 교체하므로, 역할(의장/참가자)을
  바꾸지 않은 멤버는 GET 의 `GroupMember.priority` 를 그대로 보내고, 역할을 바꾼 멤버와 새로 더한 멤버만 역할 기본값(의장 7 · 참가자 5)을
  쓴다. `required`·`mcvideoId` 도 읽은 값을 되돌린다 — 콘솔이 준 멤버별 설정을 관제 앱의 저장이 지우지 않는다.
- **«서비스» 절 — MCVideo 는 켜기만 한다.** 켜기 = `GroupDoc.mcvideo` 객체를 싣는다: 읽은 객체를 고쳐 싣고 폼에 없는 속성(코덱·해상도·
  실시간 모드·긴급 허용)은 읽은 값을 되돌린다. 새로 켜면 코덱은 서버 기본값(AMR-WB · H264)이고 미기재 칸은 위와 같은 규칙이다.
  종단간 보호 둘은 **false 를 명시**해 싣는다(요소가 없으면 켜짐으로 읽힌다 — TS 24.481 §7.2.8, mcvideo.md D7). **끄기는 하지 않는다** —
  서버가 MCVideo `<service>` 없는 PUT 을 «그대로 둠» 으로 읽는다(mcvideo.md §5.1). 이미 켜진 그룹은 스위치가 잠기고 «끄기는 운영
  콘솔에서», 이번 편집에서 켠 것은 저장 전까지 되돌릴 수 있다.
- **동시 발언**(`floor_policy`/`max_talkers`)은 폼에 두지 않는다 — 규격 그룹 문서 요소가 아니라 관리 API 전용 운영 정책이다(mcptt_api.md §2).
- 생성·편집·삭제 = GMS XCAP PUT/DELETE(TS 24.481). 편집은 **열 때의 ETag** 를 If-Match 로 보내 412 를 잡고,
  412 면 사유를 남긴 채 최신 문서로 폼을 다시 연다. 신규 id 충돌(409 `uri_taken`)은 id 를 다시 만들어 한 번만 재시도한다 —
  **성공 문서의 uri 가 정본**이라 저장 뒤 선택은 응답 uri 로 맞춘다.
- 저장·삭제 뒤에는 **새 목록을 받은 다음** 선택을 옮긴다(저장한 그룹의 상세로 — 문서도 다시 받는다). 조회를 쏘고 바로 목록을 읽으면
  옛 값이라 방금 만든 그룹을 못 찾는다. 새 그룹은 만든 뒤 토스트 한 줄로 알린다.
- 멤버 후보는 PTT 전화번호부(`/provisioning/directory?service=ptt`)다. 못 받아도 화면은 서고 이름 대신
  번호가 보인다 — 후보가 없다고 편집을 막지 않는다. 한 명씩 [＋ 추가], 또는 검색으로 좁힌 뒤 **[표시된 n명 추가]** 로
  보이는 후보 전부를 한 번에(데스크톱 `AddAllShown` — 주소록 전부가 아니라 보이는 것만, 후보 상한 200 안). 이미
  멤버인 번호·같은 사람의 다른 표기는 건너뛰고 참가자로 든다(`withEntries`).

**새 그룹 — 두 진입, 한 폼.** 폼·검증·저장은 어느 진입이든 **같은 VM**(`PttGroupsViewModel` — `newGroup`·`addMember`·`save`)이라 두 벌이
되지 않고, 편집 중인 폼이 있으면 다른 진입은 덮지 않고 이유를 적는다.

- **이 화면의 [+ 새 그룹]** — 나를 의장으로 둔 빈 폼(위 인라인 폼)에서 시작한다. 관리 범위가 있으면 데스크 소속 조직에 귀속해 같은
  범위의 다른 관제사에게도 보인다.
- **[관제] › [무전] «채널» 의 [채널 추가하기] 패널 [그룹 추가 ›]** — 사람을 고르는 자리에서 곧바로 묶는다(패널 안에서 한 겹 들어간
  «새 PTT 그룹» — `NewGroupPanel`). 패널은 **줄인 폼**이다: 그룹 이름 · 그룹 id(자동, 만든 뒤 바꿀 수 없다 — 식별자는 불변 id, 이름은
  표시) · 세션 종류 · 멤버 칩(나 = 의장 + 고른 사람, 눌러서 뺀다) · **[▸ 고급 설정]** · [취소][그룹 만들기]. [▸ 고급 설정] 은 **패널 안에서**
  편다 — 우선순위·최대 참가자 + «그룹 호» · «허용·한도» · «서비스» 절이 인라인 폼과 **같은 절**(`GroupFormSections.kt`)로 선다.
  ← 로 채널 추가에 물러나 사람을 더 고르고 돌아오면 쓰던 폼을 이어 쓴다(새로 고른 사람만 더해진다). **이 흐름(채널 추가 ↔ 새
  그룹)을 벗어나면**(패널 닫기·뒤로가기·면 넘기기·레일·착신 응답 같은 자동 이동) 쓰던 새 그룹 폼을 닫는다 — 보이지 않는 폼이
  잠금을 쥐고 남으면 채널 ⋮[편집] 이 거절되고 채널 상세의 «편성» 이 받아지지 않는다. 닫는 것은 **이 패널에서 연 폼**뿐이다 —
  [PTT 그룹] 화면의 [새 그룹] 으로 연 폼은 그 화면의 것이라 그대로 둔다(그 폼이 열려 있으면 여기서는 새로 열지 않고 이유를 적는다).
  **저장 중의 [취소] 는 곧바로 받지 않는다** — 요청은 이미 서버에 닿았을 수 있어, 폼만 닫으면 그룹은 만들어졌는데 알림도 목록 갱신도
  없다. 저장이 끝나면 닫는다(실패면 사유를 토스트로 알리고 닫는다 — 보이지 않는 곳에 오류와 함께 남기지 않는다).
  데스크톱은 [PTT 그룹] 화면으로 넘어가 같은 폼을 이어 쓰지만, 태블릿은 사람을 고르던 «채널» 면을 떠나지 않고 끝낸다. 멤버별
  [필수]·[의장] 은 만든 뒤 이 화면의 [편집] 에서 준다. 저장에 성공하면 폼이 닫히고(패널도 닫힌다) 새 그룹이 곧 «내 채널» 에 선다.
  그룹 생성 자격(`ptt.allowCreateGroup`)이나 관리 범위가 없으면 [그룹 추가] 가 꺼진다(최종 판정은 GMS). 같은 패널의
  [개별 통화]·[애드혹 통화] 는 편성 없이 고른 사람과 곧바로 이야기한다(TS 24.379 private call·ad hoc).
- 채널 상세 패널 ⋮ [편집] 은 이 화면의 편집 폼을 **밖에서** 연다(`editById` — 목록이 아직 없으면 도착한 뒤, 관리할 수 있는 행일
  때만, §6.3a).

검증 = `GroupFormTest`(폼 ↔ 문서 왕복 — 미기재 보존 · 범위 자르기 · 멤버 우선순위/필수/MCVideo ID 보존 · MCVideo 켜기/잠김 · 능력 칩·
소유자·긴급 문구 · 관리 범위 없는 목록) · `DetailMembersTest`(멤버 2열의 구성 × 상태) · `ManagementWireTest`(목록 파서).

**결과는 토스트로도 알린다.** 편집·삭제는 «그룹 편집 완료»·«그룹 삭제 완료», 삭제 실패는 사유와 함께 오류 토스트다 — 채널 상세의 ⋮
에서 지울 때는 이 화면의 목록 머리(오류 띠)가 보이지 않는다. 삭제는 세션 수명에서 돈다(패널이 곧바로 닫힌다). 실패해도 목록은 다시
받는다(이미 지워진 그룹의 줄이 남지 않게). 채널 상세 ⋮ [삭제] 의 확인은 이 화면의 확인과 같은 내용(이름·id·멤버 수·진행 중 세션)이다.
관리 목록이 준 그룹 종류(`prearranged`·`chat`)는 세션 그룹에 적어 둔다(`noteGroupTypes` — 로그인 뒤 한 번 + 목록을 받을 때). 채팅 그룹의
[일제 통화] 가 처음부터 꺼져 있다. 통합 검색의 그룹 행에는 내 소유 그룹이면 [멤버 추가](→ 편집 폼)가 선다.

### 6.13 [관리] 화면

레일 [관리] — 조직·구성원·VoLTE/VoIP/PTT 번호. 데스크톱 [관리] 화면([dispatch_desktop_ui.md](dispatch_desktop_ui.md) §4.5)과 같은 기능·
같은 시각 언어이고, 카드·폼 조각은 [PTT 그룹] 화면과 한 벌이다(§6.12). **세 카드** — 조직 | 구성원 표 | 편집 폼. 서브내비는 두지 않는다
(항목이 하나뿐).

- **조직 카드**(232). 머리 «조직 N» · [+ 새 조직] / 맨 위 **«전체»** 줄(조직 필터 풀기 — 범위 안 구성원 전부, 인원) + 트리(폴더 아이콘 ·
  깊이만큼 들여쓰기 · 그 조직에 직접 속한 인원, 고르면 하위 포함 필터 — 고른 줄 = 연한 남색 면 + 왼쪽 남색 띠, 다시 누르면 풀린다) /
  바닥 = 고른 조직 이름 + [편집][삭제](안 골랐으면 흐림) · 관리 범위 한 줄("관리 범위: 전체 조직" / "… 하위") · "전화 그룹 편성과 역할은
  운영 콘솔에서" 안내. [삭제] 는 확인을 받는다(하위 조직·구성원이 남아 있으면 서버가 거절한다).
- **구성원 표.** 머리 «구성원 N» · "{조직} 하위 포함"/"범위 전체" · «저장하지 않은 변경» 라벨 · [↻] · **[+ 새 구성원]**(남색 알약) / 검색
  (이름·아이디·번호 — 번호는 어느 표기로 쳐도 걸린다: `010…` 로 쳐도 `+8210…` 회선이 나온다) / 열 머리 띠 / 행 = 아바타 · 이름 · 직함 ·
  소속 경로 | VoLTE | VoIP | PTT(회선이 없으면 흐린 "–") | 자격 라벨(«그룹 생성» 남색 · «원격 청취» 청록). 편집 중인 줄 = 연한 남색 면 +
  왼쪽 남색 띠.
- **편집 폼 카드(404)는 폼이 열렸을 때만 선다.** 닫혀 있으면 구성원 표가 그 폭을 받아 번호 열 셋과 자격 열을 펴고, 폼이 열려 표가
  720 보다 좁아지면 번호·자격 열을 접어 이름 줄에 라벨, 소속 아래에 "VoLTE … · VoIP … · PTT …" 한 줄을 둔다(데스크톱의 좁은 표와 같은 규칙).
- **구성원 폼** = 머리(아바타 · 이름(입력을 따라간다) · "구성원 편집"/"새 구성원 등록" · 오른쪽 [삭제]) + **절 카드 넷**: «기본»(이름·직함 ·
  소속 조직 · 로그인 아이디/비밀번호) + **회선 카드 셋 = VoLTE 번호(이동) / VoIP 번호(유선) / PTT 번호**(카드 머리 오른쪽에 저장된 회선
  상태 라벨 «개설됨»(녹색)/«미개설» · 번호 · 접속서비스 · SIP transport · 픽업 그룹(읽기 전용 — 서버가 실어 줄 때만) · SIP 비밀번호) +
  PTT 카드의 자격 스위치(그룹 생성 · 원격 청취 — 청취는 표시만) + 바닥 고정 [취소][저장](스크롤과 무관하게 늘 보인다).
- **조직 폼**도 같은 카드 자리다 — 머리(폴더 아바타 · 이름 · "조직 편집 · 코드"/"새 조직 등록") + 절 카드 «조직»(코드(불변 키 — 신규만
  편집) · 이름 · 상위 조직 · 정렬 순서) + 바닥 [취소][저장]. 조직 폼은 구성원 폼 **위에** 선다 — 고치던 구성원 폼을 버리지 않고,
  조직 폼이 닫히면 그대로 돌아온다. 저장하면 그 조직을 고른 채로 돌아온다.
- **오류 문구**는 폼이 열려 있으면 폼 바닥([저장] 옆), 아니면 구성원 머리의 연한 빨강 띠에 뜬다.
- 저장·삭제가 끝나면 한 벌을 다시 받고, **전화번호부도 다시 받는다** — 주소록·멤버 후보가 방금 바꾼 이름·번호를 따라온다.
  끝난 일은 토스트 한 줄로 알린다.

동작 규칙:

- **앱은 범위 enum 을 해석하지 않는다.** 서버가 걸러 준 조직·구성원만 보이고 쓰기 판정도 서버가 한다.
  화면이 잠기는 유일한 조건은 `dispatch.directoryWrite`(전환기 `directoryAdmin`)가 없는 것이다. 조직 카드의 «관리 범위» 한 줄은
  서버가 준 값을 읽어 줄 뿐이다.
- **조직은 고르는 것이지 치는 것이 아니다.** 상위 조직·소속은 트리를 평탄화한 콤보로 고른다
  (`OrgPicker`/`orgChoices` — 데스크톱 `DirectoryAdminViewModel.OrgParent` 와 같은 구성). 코드를 손으로
  치면 오타가 조용히 다른 조직에 붙거나 저장이 400 으로 떨어진다. 둘을 가른다:
  - **상위 조직**은 편집 중인 조직 **자신과 그 자손을 뺀다** — 고르면 고리가 된다. 서버가 막더라도 고를 수
    있게 두지 않는다(고르고 저장해서 실패하는 UI 는 그 자체가 결함이다). 새 조직은 자손이 없으므로 전부.
  - **범위 밖 값은 «(범위 밖)» 으로 그대로 보인다.** 목록에 없다고 «없음» 으로 그리면 저장할 때 소속이
    조용히 바뀐다 — 관리 범위가 `own` 인 관제사가 범위 밖 조직에 속한 구성원을 여는 경우다.
- **행 한 번 누르기 = 편집.** 그래서 "열림 ≠ 변경" 이다 — 열 때와 달라진 동안에만(구성원 폼이든 조직 폼이든) 레일 [관리] 에 점 배지와
  구성원 머리의 «저장하지 않은 변경» 라벨이 붙고, 변경이 있는 채로 다른 구성원을 누르면 "변경 버림" 확인을 받는다. 편집 중인 그 줄을
  다시 눌러도 입력을 지우지 않는다. 화면 전환은 막지 않는다 — 다른 화면에 다녀와도 폼은 유지된다.
- **새 회선 카드는 접속서비스 첫 후보와 TLS 를 미리 골라 둔다**(`openLine` — 관제 소프트폰 규약. 비워 두면 `voip` 는 서버가 400
  `service_ref required` 로 거절한다). 기존 회선은 저장된 값 그대로 연다. 번호 없이는 아무것도 보내지 않으므로 그 기본값은 «변경» 이
  아니다. 새 구성원은 고른 조직(없으면 트리의 첫 조직)에 둔다.
- 회선 카드 셋(VoLTE·VoIP·PTT). **앱이 내리는 유일한 판단은 «무엇이 바뀌었나»** 다:

  | 상황 | 보내는 것 | 이유 |
  |---|---|---|
  | 번호·서비스·transport 그대로 + 비밀번호 빈칸 | **안 보냄** | 보내면 서버가 H(A1) 재결박을 요구해 저장마다 400 |
  | 번호를 비움 | `DELETE …/{kind}` | 회선 삭제 |
  | 같은 번호 다시 실음 | **저장된 IMSI 를 그대로** | 비우면 서버가 번호 숫자로 채워 "IMSI 변경" 으로 오판 |
  | 번호가 바뀜 | IMSI 를 비우고 PUT | 새 회선이다 |
  | transport 만 바뀜 | PUT(비밀번호 불필요) | H(A1) 과 무관 |

  비밀번호가 **필수**인 것은 새 회선·번호 변경·접속서비스 변경 셋뿐이다(서버가 H(A1) 를 다시 만든다).
- **서버가 400 을 낼 것이 확실한 저장은 보내지 않는다**(`saveBlocker` — 데스크톱 `SaveMember` 의 선검사): 새 회선인데 접속서비스가
  없거나(후보 0건), 새 회선·번호 변경·접속서비스 변경인데 SIP 비밀번호가 없으면 사유를 폼 바닥에 적고 멈춘다. 구성원만 생기고 회선은
  실패하는 부분 저장이 그만큼 준다.
- **회선을 지우는 저장은 먼저 확인을 받는다**(`linesToDelete`) — 번호를 비운 회선의 종류·번호를 적고 "단말 등록이 끊깁니다". 카드에도
  번호를 비운 순간 연한 빨강 띠로 알린다. 구성원 [삭제] 도 확인을 받는다(회선도 함께 지운다).
- **저장은 여러 요청으로 갈라지므로 부분 실패를 견뎌야 한다.** 구성원 생성 → 회선 3종 → PTT 자격이
  각각 별개 요청이다. 앞이 성공하고 뒤가 실패했을 때 폼을 그대로 두면 [저장]을 다시 누를 때 **생성이
  또 나가 같은 사람이 두 번 만들어지고**, 이미 적용된 회선을 다시 보내 «IMSI·서비스 변경» 으로 오판돼
  400 이 난다. 성공한 단계는 **그때그때 폼 기준(`orig`)에 반영**해 재시도가 남은 것만 보내게 한다.
- **PTT 자격은 PTT 회선이 있을 때만 보낸다.** 가입이 없으면 서버가 404 `Subscription not found` 로
  거절하므로(csc `dispatch_directory.py`), 전화 회선만 만든 신규 구성원은 **실제로는 생성됐는데 화면은
  «저장 실패»** 로 남는다.
- 접속서비스 후보는 종류에 맞는 것 + **저장된 값**이다(글자 = "이름 (도메인)"). 저장값이 후보에 없어도 그대로 보인다 — 첫 후보로
  바꿔 넣으면 저장할 때마다 "서비스 변경 → 재결박" 이 난다. 후보가 0건이면 카드에 경고 띠를 내고 개설을 막는다.
- PTT 자격은 **그룹 생성만** 쓴다. 원격 청취는 역할 배정의 결과라 표시만 하고 보내지 않는다
  (서버가 `not_editable` 로 거절한다, [mcptt_authorization.md](mcptt_authorization.md)).
- SIP transport 는 TLS/TCP/UDP/**ANY** 넷. ANY 는 "가입자 override 없음(서버 NULL)" 의 **양방향 명시값**이라
  다른 값에서 되돌릴 수 있다.

검증 = `AdminFormTest`(회선 저장 판정 · 새 회선 기본값 · 열림≠변경 · 저장 선검사 · 지울 회선 · 구성원 검색 · 관리 범위 문구 · 조직
콤보) · `ManagementWireTest`(관리 한 벌 파서 · 접속서비스 글자).

**저장과 조회는 서로 끊지 않는다** — 저장·삭제는 여러 요청이 이어지는 한 묶음(구성원 → 회선들 → PTT 자격)이라, 쓰는 동안의 ⟳ 는
받지 않는다(끊기면 반쪽만 저장된다). 저장이 끝나면 스스로 다시 받는다. **저장 중에는 폼이 그대로다** — 다른 구성원 열기·
[취소]·칸 고치기를 받지 않는다(저장의 각 단계가 지금 폼의 기준을 읽으므로, 그사이 폼이 바뀌면 남은 회선 요청이 다른 사람의
회선을 기준으로 계산돼 나간다). 레일 [관리] 의 점 배지는 폼을 고치는 그 순간 바뀐다.

**구성원 저장 선검사** — 이름과 소속 조직은 필수다(«이름과 소속 조직은 필수입니다» — 범위가 조직으로 정해져 조직 없는 구성원은 관리
밖에 선다). 표의 번호는 국내 표기다(§6.2b).

### 6.14 영상 채널 (MCVideo)

규격 모델·결정의 정본은 [mcvideo.md](mcvideo.md) §7(D5 기본 chat · D6 음성 호와 영상 호 공존 · D8 일반 호 수신 manual · D10 «영상 참여» 없음 ·
D11 음성·영상 송출 분리 · D12 마이크 경합), 관제사 화면 규약의 정본은 [dispatch_desktop_ui.md](dispatch_desktop_ui.md) §10 이다. 태블릿은
**같은 절차·같은 판정·같은 문구**를 쓰고, 여기는 구현 자리와 태블릿에서 갈라지는 것만 적는다.

**MCVideo 는 MCPTT 와 나란한 서비스다**(TS 23.280 §3) — 같은 그룹에서 음성 = MCPTT 그룹 호, 영상 = MCVideo 그룹 호를 따로 든다(TS 24.281
§7.1 — 한 등록을 공유하는 독립 다이얼로그, `CallInfo.service == MCVIDEO`·`isMcptt == false`). 그래서 **영상 호는 세션 목록(`sessions`)에
넣지 않는다** — 카드가 아니라 그 그룹 채널 상세의 «영상» 절에 붙는 호이고, 세션 목록에 두면 통화 카드·착신 배너·통화 내역·감청 수와
«그룹 id 로 무전 세션을 찾는» 자리에 샌다. 호 이벤트(상태·착신·미디어)와 화면 복귀의 스냅샷 재조회는 들머리에서 가른다
(`takeVideoCall`·`resyncVideoCalls` → `videoCalls`). `SessionKind.MC_VIDEO` 는 그 갈림의 이름이다(카드도 시트도 아니다).

| 항목 | 태블릿 | 규격 · 데스크톱 |
|---|---|---|
| 쓸 수 있는 조건 | 기동 때 한 번 정한다(`prepareVideo`) — UE initial configuration 의 MCVideo PSI ∧ MCVideo user profile(404 = 자격 없음). 둘 다면 PTT 계정에 `mcvideoEnabled`·`mcvideoServerUri`·`autoAnswerMcvideo` 를 싣는다(`videoAccount`). 자격이 뒤에 생기면 다음 로그인에, 없어지면 영상 채널은 곧바로 나간다 | TS 24.484 §7.2.2.1·§9.3 · TS 24.281 §7.2.1AA · §10.2 |
| 영상 채널 | user profile 의 그룹 목록 ∩ 내 멤버 그룹 → `GroupInfo.mcVideo` + 연결 상태 `videoChannels`(호 방식·동시 송출 상한·제휴·안내 한 줄). 목록은 로그인·5분 주기·**그룹 목록을 다시 받을 때**(GMS xcap-diff·편성 재조회) 다시 받는다(ETag) | §10.2 |
| MCVideo 제휴 | 영상 채널만 `affiliate(g, true, MCVIDEO)`, 카드 순서로 **N2** 개까지 — 넘는 채널은 영상 절에 한도 안내가 서고 제휴·합류하지 않는다. 켜는 그룹을 먼저 싣고 끄는 그룹을 뒤에 뺀다. 떠나면 영상 호 BYE 뒤에 제휴도 푼다. **PTT 등록이 새로 서면 다시 싣는다**(서버의 제휴는 등록에 묶여 있다 — 현장 앱 `PttVideo` 와 같은 규칙) | TS 24.281 §8.2.1.2 · §10.2 |
| 그룹 문서 | 호 방식(`mcvideo-on-network-invite-members`)·동시 송출 상한을 그룹 문서에서 받는다. 그룹 목록을 다시 받을 때 **GMS 목록 ETag 가 바뀐 채널만** 문서를 다시 받고, 문서 지문(ETag)이 바뀌었으면 합류 물러남을 지우고 제휴를 다시 싣는다 | TS 24.281 §6.3.5.2·§8.2.2.2.3 · §10.2 |
| 합류(D10) | PTT 등록 뒤 chat 채널에 `joinVideoGroupCall(g, {prearranged=false, queueing=true})`, 편성은 초대 자동 수락(앱이 열어 두지 않는다). **N6** 안에서 카드 순서대로. 서지 못한 합류는 10초부터 배로 최대 2분까지 물러나고, 섰다가 끝난 호(TNG3·서버 해제)는 3초 뒤 다시 합류한다. 나가는 것은 그룹이 내 채널에서 빠질 때·자격이 없어질 때·로그아웃뿐 — 채널 상세 [나가기](무전)는 영상 호를 끝내지 않는다 | TS 24.281 §9.2.1.3·§9.2.2 · TS 22.280 R-8.4.2-002 · §10.2 |
| «영상» 절 | 영상 채널의 채널 상세에만(조작 줄 아래·명단 위). 머리 «영상» + 연결 상태 한 줄 → (볼 때만) 영상 칸 → [그만 보기]·[영상 소리]·[영상 보내기] → 내 송출 줄 → «보내는 중 n · 보는 중 1 (한 번에 1개)» 목록([보기]/[바꿔 보기]·«보는 중») | §10.3 |
| 수신(D8) | [보기] = `acceptReception`, [그만 보기] = `endReception`, [바꿔 보기] = 보던 것(다른 채널 포함)을 그만 보고 새것을 본다 — **한 번에 하나**. 자동으로 보지 않는다 | TS 24.581 §6.2.5.3.3·§6.2.5.5 · §10.4 |
| [영상 보내기](D11) | 발언 바 PTT 와 따로인 한 버튼 — 요청 «요청 중… · 취소» · 대기 «대기 n번째 · 대기 취소» · 송출 «보내기 끝»(빨강) · «끝내는 중…». 편성 채널에 영상 호가 없으면 이 버튼이 영상 호를 연다(`prearranged=true, implicitTransmissionRequest=true` — «여는 중… · 취소», 다시 누르면 개시를 거둔다, 실패는 다시 열지 않는다. 여는 사이 멤버의 초대가 먼저 붙었으면 한 그룹에 영상 호는 하나라 내 개시 호는 나가고, 남은 호가 성립해 있으면 거기에 명시 송출 요청으로 잇는다) | TS 24.581 §6.2.4 · TS 24.281 §9.2.1.2.1.1 · §10.3 |
| «새 영상» 배너 | 배너 층(착신 배너 아래 — 긴급·착신 → 새 영상 → 자격·인증서)의 옅은 청록 면 — «새 영상 · 그룹» / «누구(기능 별칭)이 영상을 보냅니다» · 경과 · [보기](그 채널 상세를 열고 그 송출을 본다) · [닫기](배너만). 채널마다 하나, 보기 시작하거나 송출이 끝나면 스스로 빠진다 | TS 24.581 §6.2.5.3.2 · §3.2·§10.3 |
| 카드 «영상 n» | 내 채널 카드 1줄의 청록 라벨 = 그 영상 채널에 보내는 중인 사람 수(없으면 라벨도 없다), 보고 있으면 굵게. 카드 조작은 늘지 않는다 | §10.3 |
| 소리(D6) | 같은 그룹의 영상을 **보는 동안** 그 그룹 무전(MCPTT 호)의 수신 음량 ×0.3, 보기를 끝내면 되돌린다. [영상 소리] = 영상 호 수신 음량 막대(0~200 %) | TS 22.280 R-8.3-002 · §10.5 |
| 마이크 경합(D12) | 설정 «영상 보내는 중 무전» — **음성 우선**(기본: 무전 발언(요청·대기·발언) 동안 영상 송출 호의 음성만 멈춘다 — `setMuted`, 영상은 계속, 절에 «무전 중 — 영상은 계속, 영상 소리는 멈춤») / **영상 우선**(영상을 보내는(요청·대기 포함) 동안 `floorRequest` 가 요청하지 않고 한 번 알린다 — 긴급·임박 채널은 늘 말할 수 있다) | TS 22.280 R-8.3-003·-004 · §10.3 |
| 이벤트 | «이벤트» 면의 종류 **[영상]** — 영상 호 연결·종료(길이), 누구 영상 보내기 시작/끝, 영상 보기·그만 보기·서버 수신 종료, 내 영상 보내기 시작/끝. 보기 실패·송출 회수·영상 호 실패는 [오류] | §10.3 |
| 토스트 | 영상 호 응답(`TextArea.VIDEO` — 403·404·486·480·488·5xx)과 전송 제어 원인(수신 거절 #7·#255·시한, 송출 거절 #1·#3·#5, 회수 #2·#4)은 데스크톱과 같은 문장. chat 자동 합류의 실패는 **연속 실패의 처음만** 토스트로 알리고 그 뒤는 영상 절 안내(«영상 연결 안 됨 — 사유 · n초 뒤 다시»)가 말한다 | §9·§10.4 |
| 로그아웃 | 호를 끊기 **전에** 영상 상태를 비운다 — 끝난 영상 호가 다시 합류하지 않는다 | §10.2 |

**태블릿에서 갈라지는 것.**

- **그림 = 엔진이 Surface 에 곧바로 그린다**(`setVideoSurface` — 데스크톱의 프레임 우편함이 아니다). 엔진의 수신 창은 하나이고 보는 송출도
  한 번에 하나라 맞물린다. 영상 칸은 `TextureView` 다 — 그림을 돌리고 둥근 칸 안에 잘라 넣어야 하는데 SurfaceView 의 그림은 뷰 변환을
  따르지 않는다. 첫 장이 그려질 때까지(송출을 바꾼 뒤 0.4초는 앞 송출의 남은 장으로 본다) «영상 기다리는 중…», 영상 미디어가 열리지 않은
  호(`CallInfo.video` 거짓)는 «영상 미디어가 열리지 않았습니다 — 영상 호 소리만 들립니다».
- **영상 칸의 모양은 사람이 맞춘다.** 엔진이 수신 프레임의 크기를 앱에 알려 주지 않아 그림은 칸에 맞춰 늘어난다. 기본은 단말의 세로
  480×640(3:4 — 칸 180×240), 칸 오른쪽 위 세 단추 = **모양**(세로 송출 ↔ 가로 송출 4:3 — PC 관제 앱은 640×480 으로 보낸다) · **[↺][↻]**
  (90° 씩 — 보내는 단말을 옆으로 들었을 때). 둘 다 **보내는 사람마다 기억**한다(앱이 켜져 있는 동안). 칸을 누르면 패널 폭까지 키우고
  다시 누르면 줄인다.
- **패널 높이**(본문 608) — 영상을 보는 동안은 «영상» 절이 남는 높이의 대부분을 갖고 절 안에서 스크롤하며, 명단은 한 줄쯤 남아
  스크롤된다. 보지 않을 때는 내용만큼(최대 270), 넘치면 절 안에서 스크롤한다 — 어느 쪽도 명단을 화면 밖으로 밀지 않는다.
- **카메라** — 엔진 영상 장치는 기동 때 한 번 열거한다(엔진 기동에 앱 컨텍스트를 넘겨 Camera2 를 붙인다). 엔진은 앞 카메라에서 시작하고
  전환은 `Call.switchCamera()`(호의 영상이 열린 뒤)뿐이라, 설정의 카메라(앞/뒤)는 **송출 허가 때** 맞추고 내 송출 줄의 [카메라 전환] 이
  설정을 함께 바꾼다. 카메라가 없으면 «영상 보내기 — 카메라 없음»(영상 보기는 된다).
- **카메라 권한**은 [영상 보내기] 를 처음 누를 때(요청·편성 영상 호 열기) 묻는다 — 보기만 하는 관제사에게 미리 묻지 않는다. 거절하면
  이유와 기기 설정 경로를 알린다. 매니페스트는 `CAMERA` + `uses-feature camera required=false`.
- **화면 방향** — 가로로 쓰는 태블릿이라 송출을 걸 때와 화면이 돌 때 `setCaptureRotation(화면 회전)` 으로 카메라 프레임을 세운다. 셀프뷰
  (128×96, 거울상 — 보내는 영상은 그대로)는 카메라 미리보기라 화면이 돈 만큼 되돌려 그린다.
- **설정 [영상]** — 카메라(앞/뒤 — 기기에 없는 쪽은 흐리다) · «영상 보내는 중 무전»(음성 우선/영상 우선). 바꾸면 곧바로 저장·적용한다
  (`Settings.videoCamera`·`videoMicPolicy`).
- 툴팁이 없다 — 데스크톱 카드 태그·버튼의 툴팁 글(누가 보내는 중인지, 버튼이 무엇을 하는지)은 «영상» 절의 목록과 연결 상태 한 줄이 받는다.

**구현 자리.** 세션 = `session/VideoPlane.kt`(`DispatchSession` 확장 — 모델 `VideoCall`·`VideoChannel`·`VideoBanner`·`VideoOrient`, 순수 규칙
`VideoRules`·문구 `VideoText`, 상태 `VideoPlaneState`). 맞춤(`syncVideo` — 표시·제휴 → 문서 → 합류)은 **한 줄에서** 돈다(요청이 몰려도
마지막 상태로 한 번), 엔진 명령은 **보낸 순서대로** 간다(«그만 보기 → 보기»·«표면 떼기 → 붙이기» 가 뒤바뀌지 않게). 전송 제어의 권위는
코어 스냅샷(`transmissionInfo`)이다 — 송출·수신 이벤트는 실어 온 값으로 먼저 그리고 스냅샷을 뒤따라 당긴다. 화면 = `ui/ptt/VideoSection.kt`
(`VideoSection` 껍데기 + 순수 `VideoSectionContent`·`videoSectionUi`) · `ui/VideoBanner.kt` · `ui/VideoSettings.kt`. 세션·화면의 다른 파일에는
들머리만 있다 — 계정 설정·호 이벤트 갈림·그룹 목록 갱신 뒤 맞춤·`floorRequest` 의 D12 판정·로그아웃, 채널 상세의 «영상» 자리·카드 라벨·
배너 층·설정 절·이벤트 종류.

검증 = `VideoPlaneTest`(호 종류 갈림 · 게이트 · 합류 물러남과 재합류 판정 · N2·N6 · 호 방식·문서 지문 · D12 두 판정·D6 · 송출 목록
투영·«여는 중» · 배너 수명·문구 · 회전·칸 모양·칸 크기·셀프뷰 회전 · «영상» 절(연결 안내·보내는 사람 없음·카메라 없음·보는 중/바꿔 보기·
[영상 보내기] 상태 글·편성 영상 열기·영상 미디어 없음) · 카드 «영상 n» · 이벤트 종류 · 응답·원인 문구·주격 조사 · 카메라 설정).
기기 미리보기 `--es name video-panel`(«영상» 절이 선 채널 상세 — 영상 칸은 자리 표시) · `video-banner`(«새 영상» 배너 + 카드 «영상 n»).
실서버·실기 확인이 남은 것은 §11 이다.

**수명 규칙** — N2·N6 한도는 **카드 순서**(서버 GMS 목록 순 — 내 채널 카드·데스크톱과 같다)대로 채운다. [보기] 는 한 줄로 처리한다
(`viewLock` — 연달아 눌러도 뒤의 것이 앞 요청의 결과를 본 뒤 판정한다. 요청 중인 송출도 한 자리를 쥔 것으로 보아 «한 번에 하나» 가
지켜진다). **화면을 벗어나면(홈·다른 앱·화면 꺼짐) 보내던 영상을 끝낸다**(`releaseVideoOnBackground` — 1.5초 기다려 그대로일 때.
서비스에 카메라 타입이 없어 OS 가 배경의 카메라를 막으므로, 송출 허가를 쥔 채 두면 수신자는 멈춘 영상을 보고 동시 송출 상한 한
자리를 차지한다). 내가 보내는 채널의 카드에는 **«영상 보내는 중»** 이 선다 — 채널 상세를 닫아도 카메라가 나가는 것이 보인다.
영상 칸의 Surface 는 칸이 사라질 때 엔진에서 뗀 뒤 놓는다.

---

## 7. 입력

**하드 키보드를 전제하지 않는다 — 키보드 단축키를 두지 않는다.** 관제 전용 태블릿에는 키보드가 없다. 데스크톱의 단축키가 하던
일은 모두 화면에 있고, 화면 밖의 입력은 측면 PTT 키 하나다.

| 조작 | 데스크톱 | 태블릿 |
|---|---|---|
| PTT 누름/해제 | `Space`(앱이 앞에 있을 때 · 입력칸 밖 — 조합 키로 바꾸면 전역 핫키) | **측면 하드키**(UNIWA) — 기종마다 keycode 가 달라 설정에서 한 번 눌러 가르친다(§6.2e, 기본값 = 러기드 실측 309 · 측면 키가 기능 키로 오는 단말의 F11). 없으면 발언 바 길게 누름 |
| 응답·픽업·종료·보류·음소거 | F9·F8·F10·F11·F12 | 착신 배너·통화 카드·대기열 행 버튼 |
| 화면 전환 | F1~F4 | 왼쪽 레일·관제 탭·좌우 스와이프 |
| 채널 선택·체크 | `Ctrl+n`·`Ctrl+Shift+n` | 목록 행 탭·[발언 대상] 토글·발언 바 칩의 × |
| 통합 검색 | `Ctrl+K` | 상단 바 검색 |
| 문자 | `Ctrl+M` | [통화] › «메시지» |
| 팝오버 닫기 | `Esc` | 뒤로 제스처·스크림 탭 (애드혹 구성 중 제외) |

**측면 PTT 하드키**(SDK `HwPtt`, §5) — 누름·뗌은 ① VM 이 `HwPtt.pressed` 를 보고 발언 바와 같은 일을 한다(`pttDown`/
`pttUp` — 잠금 발언 설정도 따른다). 키는 **앱 창의 키 이벤트**로 받는다.

- **앱 창** — `MainActivity.dispatchKeyEvent` 가 **창의 어느 것보다 먼저** 받는다. 입력란에 포커스가 있어도 발언은 걸린다.
- **시트·대화상자·메뉴** — 제 창이라 위로 오지 않는다. 앱의 모든 시트·대화상자와 포커스를 갖는 메뉴(사람 메뉴·세션 메뉴·⋮·폼의
  고르기)가 내용에서 `ForwardPttKeys` 를 불러 넘긴다
  (창의 뷰가 쓰지 않은 키 — 누름을 받은 곳이 뗌도 받는다). **모달이 발언을 막으면 안 된다** — 데스크톱 전역 핫키도
  대화상자와 무관하게 먹는다.
- **화면이 꺼진 상태는 고려하지 않는다** — 관제 전용 기기라 화면을 켠 채 쓰고, 꺼졌으면 켠 뒤 쓴다. 그래서 단말이 측면 키를
  알리는 방송(`android.intent.action.PTT.down/up` — 옛 ptt-client 의 `VendorPttReceiver`)은 받지 않는다. 방송은 보낸 앱을
  가릴 수 없어, 받으면 같은 기기의 다른 앱이 발언을 걸 수 있다.

경계:
- 발언 대상은 화면 상태다(§6.7) — 최근 앱에서 지워 화면이 없으면 하드키는 발언하지 않는다(서비스·등록은 남는다).
- 경보(SOS) 키는 관제석에서 쓰지 않는다 — 데스크톱에 대응 조작이 없다. 키는 소비만 한다.
- 로그아웃하면 눌린 상태를 되돌린다 — 다음 사람의 발언이 되지 않게.

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

**장치 고르기**(데스크톱 §7 장치 선택의 번역) — 데스크톱은 역할(헤드셋·스피커·마이크)마다 장치를 **이름으로** 고르고, 고른
장치가 다시 붙으면 그리로 돌아온다(`AutoReturnToPreferredDevice`). Android 통신 경로는 장치 하나를 고르면 출력과 마이크가
**함께** 그 장치로 가므로, 태블릿이 고르는 것은 경로(설정 «소리를 내보낼 곳») + 이어폰이 여럿일 때 **어느 이어폰** 하나다.

- 설정 «오디오» 의 이어폰 칩(연결된 것 — `AudioRouter.headsets`)을 누르면 그 이어폰이 **선호 이어폰**(이름)으로 남고 경로가 그
  종류(유선 = 헤드셋, 무선 = 블루투스)로 옮겨진다. 경로가 헤드셋·블루투스일 때마다 그 종류 중 선호 이어폰을 먼저 고른다
  (`pickHeadset` — 없으면 그 종류의 첫 것). `AudioDeviceInfo.id` 는 재연결·재부팅 때 바뀌어 이름으로 든다.
- **선호 이어폰 자동 복귀**(기본 켬) — 선호 이어폰이 다시 붙고 경로 의도가 이어폰이면 그리로 되돌리고 토스트로 알린다
  (`returnToPreferred`). 사람이 스피커를 고른 뒤에는 되돌리지 않는다 — 의도가 이긴다. 빠질 때는 플랫폼이 기본 장치로 돌아간다.
- **마이크는 따로 고르지 않는다** — 통신 경로는 고른 장치의 마이크를 함께 쓴다(이어폰이면 이어폰 마이크). 별도 마이크를 쓰려면
  코어 캡처 장치 통로가 필요하다(pjmedia Android 백엔드는 장치 하나).

---

## 9. 검증

| stage | 항목 | 내용 |
|---|---|---|
| S1 | `S1-UE-FLOOR-CODEC` | `gen_floor_defs.py --check` + `cimsue_test` floor 교차 검증 |
| S1 | `S1-UE-UNIT` | `build/bin/cimsue_test` — 코어 단위시험 (§3.3 반환형 포함). **1:1 SDS** = `one-to-one-sds` 본문·쌍 정렬 conversation ID 가 cspsim·Kotlin 과 같은 값인지 |
| S1 | `S1-UE-ANDROID-BIND` | SWIG 생성 Java 에 `SWIGTYPE_p_*` 부재 + `javac` 통과 + `cimsue.i` 타입맵 본문에 맨 `#` 지시문 없음(`%#` — 생성 때 한 갈래만 남아 플랫폼 분기가 사라진다) |
| S1 | `S1-UE-ENGINE-SINGLE` | 레포에 커밋된 엔진 산출물 부재(`android/core/src/pjsua2` 없음) + `org.pjsip.**` 제공처가 `:cimsue-engine` 하나 |
| S1 | `S1-UE-CSC-XCHECK` | 코어 `csc_client` 와 `:core` `ProvisioningClient.kt` 의 IdMS·프로비저닝 경로 대조 (로그인 앱·SSO 는 코어를 싣지 않는다) |
| S1 | `S1-UE-TABLET-UNIT` | `./gradlew testDebugUnitTest` — 프로파일 파싱·포커스/발언대상 분리·발언 소유권·관리 와이어 파서(범위·전환기 이름·조직 고리)·회선 저장 판정(§6.13 표)·이력 날짜 창/시간대 밴드/발언 턴·응답 문구 사전·E.164 정규화·**문자 외부망 판정/스레드 정렬/글자 수**(§6.2e)·**새 대화 후보 거르기·사람 축 기록 필터**(§6.9a·§6.2f)·**전이중 음소거 자격·표시와 연타 직렬화**(§6.2e)·**긴급·임박 스택·전이·표시·진행 중 조건 판정·개시자**(§6.2a-1)·**SDS 전달 확인 회신**(§6.2e)·**면 이동 요청 수명**(§6.3)·**발신 token 상관**(§6.2e)·**응답 문구 SIP 영역·토스트 규칙**(§10·§6.2a-2)·**상담 전달 카드 자격**(§6.6)·**개별 통화 대상 풀이**(§6.2e)·**재전송 판정·같은 말풍선 갱신**(§6.2e)·**파일(FD) 크기 낱말·상한·이름 거르기와 겹침 «(n)»·받기 자격·재전송 갈림·폴더 정리·파일 응답 문구, 메시지 보관 주인 규칙**(§6.2e)·**서버 인증서 만료 단계·문구·관측 고르기**(§6.2a-3)·**대기열 [응답] leg·지정 픽업 대상**(§6.2d)·**번호 제안 표기 매칭**(§6.2b)·**⑥ 행 바로가기 자격·CSV 열**(§6.3)·**⑤ 이벤트 CSV 열**(§6.3)·**표시된 후보 일괄 추가**(§6.12)·**타임라인 눈금 간격·통화 결과 태그·카드 행 표시값·빈 세션 묶음·시간대 묶음·발언 수 밴드·틈 줄임 시간축·세션 패널(레인·참여자·숫자 칸)·이벤트 묶기·재생 바 시각↔세그먼트·영상 판정·이력 와이어(서비스 축·발언 수 유무)**(§6.11)·**주소록 CSV 읽기·서버 우선 병합·캐시 왕복**(§6.2b)·**이어폰 고르기·자동 복귀 판정**(§8)·**탐색 규칙(레일·모드·면·사이드 패널 열기/바꾸기/고정·뒤로가기 차례)·관제 면 여섯 장의 차례**(§6.3)·**카드 네 줄(핀 번호·대기/진행 중·미참여·개별 접두·긴급 낱말)·조작 하나·접속자 줄·채널 추가 패널의 사용자 목록(나·외부망 빼기·아는 상태만·거르기)과 아래 줄 자격(`addChannelActions`)·통화 고정 칸 밀림(`fixedShift`)·대화 미리보기·날짜 칸·앞뒤 이벤트**(§6.3·§6.3a·§6.9a·§6.12), `:cimsue` 접점층 — 측면 키 분류(학습·기본값·키보드 기능 키 제외)·학습 제외 키·라우트 어휘·망 변경 판정(걸 때의 망 심기 포함)(§5·§6.1·§7) (JVM, 기기 불필요)·**동시 발언 대상·잠금 발언 해제·남은 발언 게이지**(§6.7)·**긴급 경보 배너 수명(제3자 취소)·조건 해제 자격·배너 스택**(§6.2a-1)·**편성 변화 판정·감시 대상 순서·xcap-diff 판정·로스터 부분 갱신과 합류/이탈·서버 이력 폴링(파서·백오프·꺼짐 판정)·멤버 확인 전 연결·floor 이벤트 문구·대기열 위치**(§6.2d)·**로그인 실패 문구·외부망 문자 게이트·SDK 접두가 붙은 오류 본문**(§6.1b·§6.2e·§10)·**영상 채널 규칙**(`VideoPlaneTest` — §6.14)·**목록 키 유일성**(통화내역 순번 `rowKey` · 로스터의 같은 사람 두 URI 꼴 — 겹치면 화면이 죽는다)·**세션 시작·종료 줄 낱말·표시용 번호(국내 표기)·구성원 저장 선검사**(§6.2b·§6.9a·§6.13) |
| S3 | `S3-UE-CLI-*` | `cimsue-cli` 로 등록·그룹콜·floor·SDS·Join·픽업·전달·PTT 청취 |
| 실기기 | 태블릿 | 감청 SSRC 2개 귀속(진행 중 행의 인라인 상세, §6.5) · PTT 청취 중 버튼 비활성 · 대표번호 착신 · 하드키 누름/해제(앱 창·시트 위, §7)·버튼 학습 · 오디오 분리 출력 · 화면 밀도 · **이력 창 조회/녹취 재생** · **그룹 XCAP PUT/DELETE** · **회선 PUT 재결박** · **문자 송수신**(사이트 안 번호) · **1:1 무전 메시지**(사람 스레드 왕복) · **파일(FD)**(그룹·1:1 보내기 → 상대 [받기] → 열기, 다른 로그인 ID 로 로그인했을 때 앞 ID 의 대화가 보이지 않음) |

기기가 필요한 판정은 실기기 행으로만 둔다. 화면 점검은 두 길이다 — 둘 다 1280×800 dp 가로 태블릿(실기 또는 에뮬레이터 — Android 15
x86_64, ARM 번역으로 arm64 네이티브를 그대로 싣는다)에 debug APK 를 올리고 캡처(`adb exec-out screencap -p`)로 데스크톱 화면과 대조한다.

- **실제 앱** — 로그인해 서버와 함께 본다(로그인 화면·실패 문구·등록·왕복).
- **기기 미리보기**(debug 빌드 전용, `src/debug` `debug.PreviewActivity` — 런처에 나오지 않고 adb 로만 띄운다. release 매니페스트에는
  없다) — 순수 `…Content` 컴포저블에 고정 표본을 넣어 **세션·서버 없이** 화면을 세운다:
  `adb shell am start -n com.cims.ue.dispatch/.debug.PreviewActivity` + `--es name <표본>`(`ui/AppPreview.kt` `DEVICE_PREVIEWS` —
  channels·channel-panel·messages·events·calls·book·sms·log·history-*·groups·group-edit·admin·admin-edit·dark …) 또는 이름 없이
  `--es screen history|groups|admin` · `--es mode call` · `--es panel channel|add|book|event` · `--es banners alerts|incoming|cert|toasts` ·
  `--ez dark true`. 이름 없이 띄운 것은 실제 `NavState` 규칙으로 도는 **조작 가능한** 표본이다(레일·탭·패널 겹침·폭 끌기·테마 전환).

스크린샷 회귀 자동화는 두지 않는다(§11).

---

## 10. 응답 코드 → 화면 문구

[dispatch_desktop_ui.md](dispatch_desktop_ui.md) §9 사전을 그대로 쓴다. 문구를 새로 짓지 않으며,
보완이 필요하면 그 문서를 고친다. 관리·녹취 오류 본문(`error`) 사전도 같다.

- 영역(`TextArea`)은 데스크톱 `ResponseText.Area` 와 같은 축이다 — HTTP API(관리·녹취·그룹·MCData 파일, 본문 `error` 로 세분 —
  `ResponseText.of`)와 SIP 응답(당겨받기·전달·감청 Join·PTT 청취·PTT 참여·개별 통화·애드혹·긴급·SDS·SMS·등록·통화, 상태코드로 —
  `ResponseText.sip`).
- 사전에 없는 SIP 응답은 **원문을 괄호로** 붙인다(«실패 (500 Server Error)») — 조용히 삼키지 않는다. 코드가 없는 실패(앱이
  먼저 막은 것)는 사유를 그대로 쓴다.
- 호를 만든 동작 → 영역은 `ResponseText.areaOf`(데스크톱 `AreaOf`) — 동작을 모르는 MCPTT 발신은 개별 통화/그룹 참여로 본다.

---

## 10a. 밀도 — 글자·라벨의 단일 계약

[dispatch_desktop_ui.md §12](dispatch_desktop_ui.md) 의 두 앱 대응표가 가리키는 글자·라벨 밀도의 정본이다.

이 앱은 데스크톱(1920×1080·마우스)의 화면 의미론을 태블릿(1280×800·터치)으로 옮긴 것이라, 옮기는 동안
**데스크톱 밀도가 그대로 따라왔다.** 한때 `fontSize` 가 281곳에 9~26sp 열세 가지로 흩어져 있었고, 같은 모양의
작은 라벨이 화면마다 따로 정의돼 배경 투명도만 0.16·0.20·0.22 로 달랐다. 팔 길이에서 읽는 관제석 화면에서
그 아래쪽 값들은 작고, 같은 뜻의 표시가 화면마다 다르면 «같은 것인가» 가 흔들린다.

**글자 — 여덟 단계**(`ui/Density.kt` `Type` — 값은 시안의 글자 크기다, §6.3c). 화면 코드는 숫자를 직접 쓰지 않는다.

| 토큰 | 값 | 쓰임 | 흡수한 값 |
|---|---|---|---|
| `micro` | 11 | 배지·보조 메타의 **최소** — 이보다 작게 두지 않는다 | 9 · 10 |
| `meta` | 12 | 칩·표 셀·타임스탬프 | 11 |
| `body` | 13 | 본문·목록 행 | 12 |
| `strong` | 14 | 카드 제목·표 머리 | 13 |
| `title` | 15 | 카드·구역 제목, 목록에서 먼저 읽히는 이름 | 14 · 15 · 16 |
| `head` | 17 | 사이드 패널·대화 머리의 제목 | 17 · 18 · 19 |
| `display` | 20 | 상단 바 이름·로그인 제목·배너·다이얼 키처럼 한 화면에 하나뿐인 것 | 20 |
| `huge` | 26 | 다이얼패드 입력처럼 숫자 자체가 화면인 것 | 26 |

**배율 손잡이 하나** — `Type.SCALE`(기본 1.0). 기기에서 보고 한 번에 키우거나 줄인다. 0.92 면 정비 전 밀도에
가깝다. 단계별로 흩어 고치지 않는 것이 요점이다 — 밀도는 화면마다 정할 것이 아니라 **한 번 정할 것**이다.

**작은 라벨은 한 벌**(`ui/Tag.kt` `Tag`). 배경은 글자색의 옅은 면(단일 투명도), 모서리 4dp, 글자 `micro`.
누를 수 없다 — 누르는 것은 `FilterChip`·`AssistChip` 이고, 그 구분이 «이건 눌러도 되나» 의 답이다.

**URI → 번호도 한 벌**(`session/Uri.kt` `userPart`). 여섯 벌이 있었고 **셋은 서로 다르게 잘랐다**(`;user=phone`
파라미터를 떼는 것과 두는 것, 공백을 다듬는 것과 두는 것). 이 값은 «같은 사람인가» 의 열쇠라(로스터·주소록·
감시 대상 대조) 자르는 법이 갈라지면 같은 사람이 두 사람이 된다. 표기 차이까지 맞추는 것은 그 위의
`DirectoryBook.normalize` 다. 스킴의 `:` 은 `@` **앞의 마지막** 것이다 — `1001@host:5060` 의 `:` 은 포트고 표시 이름에 든 `:` 도
스킴이 아니다. name-addr(`"표시 이름" <sip:…>`)이면 꺾쇠 안만 본다. 숫자가 없는 신원(영숫자 id)은 정규형이 비므로 원문으로
견준다(이름 찾기·로스터 칩·CSV 병합).

**서버 응답의 문자열은 `str()` 로 읽는다**(`MgmtModels.kt`). Android 의 `optString(name, fallback)` 은 값이 JSON null 이면 fallback 이
아니라 글자 «null» 을 돌려준다(JVM 단위시험의 org.json 과 다르다 — 시험으로는 드러나지 않는다). 그대로 쓰면 이름·번호·화자 자리에
«null» 이 선다.

**남은 판단은 기기에서** — 터치 타깃(누를 수 있는 목록 행의 최소 높이)은 «한 화면에 몇 줄이 보이나» 와
맞바꾸는 것이라 화면을 보고 정한다. 관제 표는 읽는 일이 누르는 일보다 잦아 데스크톱처럼 촘촘한 편이 나을 수도
있다. 지금은 손대지 않았다.

## 11. 미해결 / 향후 과제

- **기존 앱 로직 전환** — `volte-client` → `ptt-client` 순으로 파사드로 옮긴다([ue_sdk.md](ue_sdk.md) §5.3 이행 단계 P0a~P5).
  옮기면 Kotlin 의 floor·mcdata·csc 사본 1,772줄이 걷힌다. 엔진 이중화와 커밋 산출물은 이미 걷었고(§2.2), 공존 기간의
  코드 중복은 floor(`gen_floor_defs.py --check`)·SDS·CSC 세 대조 검사가 막는다(§9). `PttController` 의 정책 부분은 코어
  `domain/` 이 아니라 **앱 세션 층**으로 간다 — 이 앱의 `DispatchSession` + 평면 확장 구성이 본보기다.
- **② 타인 세션 섹션** — 서버 계약은 확정·구현됐다([dispatch_center.md](dispatch_center.md) §5.6a).
  코어 `dialogWatch` 가 PTT 계정으로도 구독하고 `<mcptt>` 확장을 이벤트 필드로 내야 앱이 카드를 묶는다.
- **U10 관측** — `MediaSource.active/level` 에 실시간 값이 없다(SDP 라벨만 있다). 감청 상세의 레벨 미터는
  pjproject 에 SSRC 별 수신 관측 API 가 생긴 뒤 켠다.
- **무전/통화 분리 출력(Android)** — 가능함은 실측으로 확인했다(§8). 남은 것은 셋이다.
  ① 코어에 스트림별 출력 경로 통로(`PJMEDIA_AUD_DEV_CAP_OUTPUT_ROUTE` → 라우트 API) — **코어 공개 계약
  변경**이라 별도 단계로 다룬다. ② 유선·USB-C 헤드셋 측정 — SCO/A2DP 배타성이 없어 요구 방향
  (통화=헤드셋)이 될 수 있으나 **미측정**이다. ③ 헤드셋 종류별 정책 표 확정. 측정 도구는
  `android/audio-probe`(일회성 탐침, 앱 빌드와 무관).
- **MCData 파일(FD) 잔여** — 보내기·받기·열기·보관은 §6.2e 대로다. 남은 것은 셋이다. ① 실기 확인(그룹·1:1 왕복, 상한 근처의 큰 파일,
  뷰어 앱으로 열기). ② 진행률 — SDK 의 업로드·다운로드가 진행 콜백을 주지 않아 «올리는 중…»·«받는 중…» 문구와 도는 표시뿐이다.
  ③ 받은 파일을 기기의 공용 폴더로 내보내기 — 지금은 연 뷰어 앱의 저장 기능에 맡긴다.
- **망 전환 중의 통화 유지** — 등록 복구는 코어가 한다(§6.1). 진행 중 호는 건드리지 않는다 — re-INVITE 로 미디어를 새
  주소로 옮기는 호 유지는 서버 처리(VoLTE relay·MCPTT 세션)를 확인한 뒤 정한다([ue_sdk.md](ue_sdk.md) §11). 통화 중
  Wi-Fi ↔ LTE 전환에서 호가 이어지는지는 실기 미확인이다.
- **자동 수락 분리** — `AccountConfig.autoAnswerMcptt` 가 그룹콜·개별 통화 공통이다. 관제석은 그룹콜 자동 +
  개별 통화 수동이 맞아 코어 플래그 분리가 필요하다. 긴급 개별 통화의 전역 표시(§6.2a-1 — 지금은 ① 빨강 카드뿐)도 이것과
  함께 정한다.
- **대표번호로 발신** — 코어 `dial` 에 `P-Preferred-Identity` 헤더 옵션이 필요하다(서버 계약은 확정).
- **외부망 SMS/LMS** — 밖으로 나가는 문자는 **서버 게이트웨이**(IBCF→SMSC / SMPP)가 있어야 한다. 있는지는 프로파일
  `capabilities.smsGateway` 가 말하고 앱은 그 값으로 외부 번호의 보내기를 연다(§6.2e). 남은 것은 «외부망인가» 의 판정이다 — 지금은 앱이
  주소록으로 추정한다(서버 가입자가 아니고 내선 길이도 아니면 외부망). 서버가 가입자 판정을 주면 그 값으로 바꾼다(데스크톱도 같은
  자리에서 같은 추정을 한다).
- **코어 `normalizeTarget` 의 `tel:` 처리** — 지금은 앱이 요청 대상에서 `tel:` 을 벗겨 넘긴다(§6.2c).
  코어가 라우팅 불가한 `tel:` 을 그대로 내는 것이 결함인지, 아니면 호출자가 라우팅 가능한 형태로
  주는 것이 계약인지 정해야 한다. 코어를 고치면 Android·Windows·CLI 의 모든 요청 경로(dial·join·
  pickup·transfer·subscribe)가 함께 바뀌므로 CSP 의 `tel:` Request-URI 처리를 확인한 뒤 결정한다.
- **메시지 보관 주인의 단위** — 지금은 로그인 ID 다(§6.2e «메시지 보관», 데스크톱 [dispatch_desktop_ui.md](dispatch_desktop_ui.md) §4.4 «보관» 과 같다).
  §10 «자리/사람 분리» 와 같은 뿌리다 — 그쪽이 서면 `owner` 는 자리 ID 가 아니라 **점유한 사람**이 된다.
- **Ringtone 반복** — `isLooping` 이 API 28+ 라 그 아래에서는 1초 주기로 다시 트는 폴백을 쓴다.
  minSdk 를 28 로 올리면 걷어낼 수 있다.
- **화면 회귀 자동화** — 데스크톱의 `--ui-preview-shot` 에 해당하는 스크린샷 캡처(Roborazzi 등)는 두지 않는다.
  화면 판정은 실기기가 맡는다.
- **영상 채널(MCVideo) 잔여** — 영상 채널은 §6.14 대로다(코드·단위시험까지 — 실서버·실기 왕복은 확인 전이다). 남은 것:
  ① **실기 확인** — 등록 태그·MCVideo 제휴·chat 합류·편성 초대 자동 수락과 [영상 보내기] 로 열기, [보기]/[바꿔 보기]/[그만 보기], «새 영상»
  배너, D6 음량 줄임, D12 두 정책, 카메라 권한·앞/뒤 전환, 로그아웃 뒤 재합류 없음. 특히 **`TextureView` 의 Surface 에 엔진이 그리는
  것**(현장 앱은 SurfaceView 다)과 그 위의 회전·거울상, 셀프뷰의 방향은 기기에서 봐야 안다.
  ② **가로 화면의 송출 그림** — 단말 인코딩은 세로 480×640 고정이라, 가로로 쓰는 태블릿이 화면 방향으로 세운 그림이 그 틀에 어떻게 드는지
  (여백·잘림) 실기로 확인하고, 필요하면 SDK 에 가로 인코딩 크기를 둔다.
  ③ **프레임 크기·방향 통지** — 엔진이 수신 프레임의 크기와 방향(MTSI CVO — TS 26.114 §7.4.5)을 알려 주면 칸 모양·회전의 기본값을
  거기서 얻고 수동 조정은 그 위에 더한다.
  ④ **화면 밖 송출** — 서비스의 FGS 타입에 카메라가 없고 캡처 중 마이크 승격도 세션 목록(영상 호는 들지 않는다)만 본다 — [영상 보내기] 는
  앱이 화면에 있는 동안의 기능이라, 화면을 벗어나면 앱이 송출을 끝낸다(§6.14). «새 영상» 도 화면 안 배너뿐이다 — 화면이 꺼져 있으면
  새 송출을 모른다(착신처럼 알림을 띄울지는 후속).
  ⑤ **호별 수신 창** — `setVideoSurface` 는 엔진 창 하나다([ue_sdk.md](ue_sdk.md) §11). 영상 벽(여러 송출 동시 보기)·영상 칸 별창은 그 뒤다.
  영상 채널이 둘 이상일 때(D10 — 전부 상시 합류) 엔진이 살아 있는 영상 호마다 같은 창을 붙이므로, **두 번째 채널의 [보기] 가 그 창에
  그려지는지**는 실기로 확인해야 한다.
  ⑦ **영상 호의 마이크 장치** — 영상 호에 합류해 있는 동안 엔진의 소리 장치가 열려 있다(송출 허가 밖에서는 인코더가 멈춰 음성은
  나가지 않는다). Android 의 마이크 사용 표시가 상시 켜지는지·전력 영향은 실기로 확인한다(코어 과제 — 재생만 여는 통로).
  ⑥ 긴급·방송 영상의 automatic 수신 표시 · 영상 가져오기(pull) · 원격 영상 보기(ambient viewing) · 감청 영상 격자는 데스크톱과 함께
  후속([dispatch_desktop_ui.md](dispatch_desktop_ui.md) §10.7).
