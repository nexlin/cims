# 단말 음성 품질·계측 링크 — 다른 개발 환경에서 할 일

.48(소스 서버)에서 Q1~Q3 를 구현·실측했고 계측기를 배포했다(정본 [../design/features/ue_voice_quality.md](../design/features/ue_voice_quality.md)).
이 서버에는 Android(NDK·SWIG) 빌드 환경과 Windows(MSVC·dotnet)가 없다. 그래서 **플랫폼 빌드 확인과 Q4(앱 시험 모드)** 를 여기 모았다.
작업 분담은 기존 관례와 같다 — **단말 SDK·Android 단말 앱 = .45 서버**, **관제 앱(Windows 데스크톱·Android 태블릿) = Windows 개발 환경**.

| 커밋 | 내용 |
|---|---|
| `5d71d6b4` | 설계 정본 |
| `8c5f6108` | Q1 코어 측정 — `Engine::callQuality`·RTCP-XR·공용 E-model·C API/.NET/Android 바인딩·`cimsue-cli` 품질 필드 |
| `ef4f345e` | Q2 코어 계측 링크 — 공개 `cimsue/drive.h`(`DriveSession`·`DeviceLink`)·`Engine::setTxSource`·`net/tls_stream`·`cimsue-cli link` |
| `917cafa2` | Q3 계측기 `device` 풀 — 워커 `DeviceHub`(Device.Port **7120**)·컨트롤러·콘솔·libcsim RTCP |

| 환경 | 할 일 | 순서 |
|---|---|---|
| **.45 서버 — 단말 SDK·Android 단말** | §1.1 Android 빌드 확인(A1~A5) · §2 SWIG·Kotlin 파사드(`DeviceLink`) · `:cimsue` AAR 을 관제 태블릿이 쓸 수 있게 · §3.0 `volte-client`·`ptt-client` 의 SDK 이식(시험 모드 선행 조건) | 먼저 — 관제 태블릿이 이 AAR 을 쓴다 |
| **Windows 개발 환경 — 관제 앱** | §1.2 Windows 빌드 확인(W1~W6) · §2 C API·.NET(`DeviceLink`) · §3.2 관제 데스크톱 시험 모드 · §3.1 관제 태블릿 시험 모드(.45 의 Kotlin 파사드 위) | .45 의 §2 Kotlin 파사드 뒤에 태블릿 |
| **.48(이 서버)** | §4.1 계측기 배포 — **완료** · 결과 회신(§4.3)을 받아 설계 문서 반영 | — |

시험 대상 = .48 배포본(CSP SIP UDP `121.161.164.48:15060`, VoLTE 도메인 `volte.cims.example.kr`, PTT 도메인 `ptt.cims.example.kr`).

---

## 1. 선행 확인 — 이미 커밋된 코드의 플랫폼 빌드

아래는 코드가 들어가 있지만 **이 서버에서 컴파일해 보지 못한** 부분이다. Q4 전에 빌드부터 통과시킨다.

### 1.1 Android — .45 서버

| # | 확인 | 파일 |
|---|---|---|
| A1 | **엔진 재빌드** — `sdk/engine/config_site/common.h` 에 `PJMEDIA_HAS_RTCP_XR 1`·`PJMEDIA_STREAM_ENABLE_XR 1` 이 들어갔다. `build-native.sh` 가 pjproject 를 다시 빌드하는지 본다(옛 산출물이 남아 있으면 XR 이 꺼진 채 링크된다) | `sdk/engine/config_site/common.h` |
| A2 | pjsua 패치 `pjsua_call_get_stream_stat_xr` 가 NDK 빌드에서 링크된다 | `ext/pjproject/pjsip/src/pjsua-lib/pjsua_call.c` |
| A3 | **SWIG 생성물** — `CallQuality`·`QualityDirection` 의 Java 게터 이름이 Kotlin 파사드와 맞는다. 특히 `rLq`·`rCq`(Kotlin 은 `getRLq()` → `rLq` 로 푼다고 가정). 생성물에 `SWIGTYPE_p_*` 가 없어야 한다(S1-UE-ANDROID-BIND) | `sdk/android/cimsue/src/main/java/com/cims/ue/sdk/Types.kt`(`CallQuality.of`)·`CimsUe.kt`(`callQuality`·`Call.quality()`) |
| A4 | `Engine::addObserver`·`removeObserver`·`setTxSource` 가 `engine.h` 에 추가돼 SWIG 이 다시 생성한다 — 파사드 노출은 §2 에서 | `sdk/core/include/cimsue/engine.h` |
| A5 | 코어 새 소스(`src/quality/*`·`src/net/tls_stream.cpp`·`src/drive/*`·`src/util/json_lite.h`)가 NDK 로 컴파일된다. `tls_stream.cpp` 는 POSIX `poll` 을 쓴다 | `sdk/core/CMakeLists.txt` |

### 1.2 Windows — Windows 개발 환경

| # | 확인 | 파일 |
|---|---|---|
| W1 | 엔진 재빌드(A1 과 같은 이유 — `sdk/windows` 슈퍼빌드가 `config_site/windows.h` → `common.h`) | 같음 |
| W2 | `src/net/tls_stream.cpp` 의 winsock 갈래 — `WSAPoll`·`POLLRDNORM`·`shutdown(SD_BOTH)`. `https_client.cpp` 가 이 스트림을 쓰도록 바뀌었으므로 **CSC 로그인·프로비저닝·FD 가 그대로 되는지**도 본다 | `sdk/core/src/net/tls_stream.cpp`, `sdk/core/src/http/https_client.cpp` |
| W3 | 공개 클래스 `DriveSession`·`DeviceLink`·`LineSink`·`DeviceLinkListener`·`drive::*` 함수가 `CIMSUE_API` 로 DLL export 된다(`cimsue-cli` 가 링크한다). C4251 경고는 기존과 같은 성격 | `sdk/core/include/cimsue/drive.h` |
| W4 | `cimsue-cli` 의 `link` 명령이 컴파일된다 — `std::signal(SIGINT/SIGTERM)`·`std::atomic` 람다 | `sdk/core/cli/main.cpp` |
| W5 | **.NET 단위시험** — `AbiLayoutTests` 에 `QUALITY_DIRECTION`·`CALL_QUALITY` 구조체 크기 항목, `EngineHeadlessTests` 에 `Quality.Valid == false`·`MosCq == -1` 이 추가됐다. C 구조체와 `NativeStructs.cs` 배치(`double` 정렬 포함)가 같은지 이 시험이 잡는다 | `sdk/windows/dotnet/CimsUe.Tests/*`, `CimsUe/Native/NativeStructs.cs` |
| W6 | `cimsue_test`(googletest) — `quality_test.cpp`·`drive_test.cpp` 가 Windows 에서도 통과한다 | `sdk/core/test/*` |

**`cimsue_test` 한 프로세스 실행** — `FloorParticipant`·`FloorXCheck` 시험이 `pj_init` 만 하고 남겨 두면 뒤의 `EngineRoute`·`CApi.EngineLifecycleHeadless`
가 pjlib "unknown thread" assert 로 죽던 순서 문제는 **고쳤다**(§6). 시험마다 `pj_init`/`pj_shutdown` 을 짝짓는 `sdk/core/test/pj_scope.h` — Windows 도 같은 시험 파일이다.

---

## 2. 바인딩 — `DeviceLink` 를 앱에서 쓰게

코어 C++ 공개 API(`cimsue/drive.h`)는 끝났다. 앱은 **`DeviceLink` 하나와 상태 콜백만** 쓴다. `DriveSession`·`LineSink` 는 링크 안에서 쓰이므로
앱에 노출할 필요가 없다. 제안 모양은 다음과 같고, 이름은 각 층의 관례에 맞춰 조정한다.

```c
/* C API (sdk/core/include/cimsue/cimsue_c.h · src/c_api.cpp) */
typedef enum { CIMSUE_LINK_IDLE, CIMSUE_LINK_CONNECTING, CIMSUE_LINK_CONNECTED, CIMSUE_LINK_DISCONNECTED, CIMSUE_LINK_REFUSED } cimsue_link_state_t;
typedef struct { const char* service; int32_t account_id; const char* aor; const char* msisdn; } cimsue_drive_account_t;
typedef struct {
    const char* host; int32_t port;                 /* 기본 7120 */
    const char* pair_key; int32_t verify_server; const char* ca_pem; const char* pin_file;
    const char* device_id; const char* app; const char* app_version; const char* platform; const char* model;
    int32_t reconnect_max_sec;
    const char* sample_file;                        /* media sample 기본 WAV */
} cimsue_device_link_config_t;
typedef void (CIMSUE_CALL *cimsue_link_state_cb)(void* user, cimsue_link_state_t st, const char* detail);
cimsue_device_link_t* cimsue_device_link_create(cimsue_engine_t* e);
cimsue_status_t cimsue_device_link_start(cimsue_device_link_t*, const cimsue_device_link_config_t*,
                                         const cimsue_drive_account_t* accounts, int32_t n, cimsue_link_state_cb cb, void* user);
void cimsue_device_link_stop(cimsue_device_link_t*);
cimsue_link_state_t cimsue_device_link_state(const cimsue_device_link_t*);
void cimsue_device_link_destroy(cimsue_device_link_t*);
/* 구조체 id 는 CIMSUE_STRUCT_COUNT_ 앞 끝에 덧붙인다 — 기존 번호를 바꾸지 않는다(AbiLayoutTests) */
```

- **C API·.NET — Windows** — `DeviceLink : IDisposable`(`Start(config, accounts)`·`Stop()`·`State`·`event StateChanged(LinkState, string)`). 콜백은 코어 링크
  스레드에서 오므로 UI 스레드로 마샬링한다(기존 Listener 규약과 같다).
- **SWIG(Android) — .45** — `cimsue.i` 에 `%include "cimsue/drive.h"` 를 넣고 `%feature("director") cimsue::DeviceLinkListener;` 를 준다.
  `DriveSession`·`LineSink` 는 `%ignore` 해도 된다. Kotlin 파사드는 `DeviceLink` 래퍼 + `StateFlow<LinkState>` 로.
- **계정 목록** — 앱이 가진 회선(`volte`·`voip`·`ptt`)마다 `{service, accountId, aor, msisdn}` 를 넘긴다. 계측기는 풀의 `service` 로 `use` 한다.
- **수명** — `DeviceLink::start` 는 스레드만 띄우고 곧 돌아온다. `stop` 은 링크를 닫고, 이 링크가 구동한 호만 끊는다. Engine 보다 먼저 멈춘다.

---

## 3. 앱 시험 모드 (Q4)

화면 의미론의 정본은 설계 §4 다. 진입(버전 줄 7회)·표시(배지)·설정 항목·화면 구성은 모두 거기에 있다.
**서버 쪽 권한·허용 플래그는 없다** — 사용자가 시험 모드를 켜고 계측기 주소를 넣는 것이 동의다.

### 3.0 적용 대상 — SDK 를 쓰는 앱만

| 앱 | 엔진 | 시험 모드 |
|---|---|---|
| `android/dispatch-tablet` | libcimsue(`:cimsue`) | **가능** — §3.1 · Windows 개발 환경(.45 의 Kotlin 파사드 위) |
| `windows/dispatch-desktop` | libcimsue(.NET) | **가능** — §3.2 · Windows 개발 환경 |
| `android/volte-client`·`android/ptt-client` | `android/core` 의 자체 pjsua2 래퍼(`SipController`) — libcimsue 아님 | **SDK 이식이 선행**(ue_sdk.md §5.3) · .45. 한 프로세스에 pjsua 는 하나라 두 엔진을 같이 띄울 수 없다. 이식 뒤 시험 모드는 §3.1 과 같은 구성(`android/core` 공통 조각으로 두면 세 앱이 같이 쓴다) |

### 3.1 Android 관제 태블릿 (`android/dispatch-tablet`) — Windows 개발 환경

| 항목 | 위치·방법 |
|---|---|
| 진입 | 설정 시트 `ui/SettingsSheet.kt` 의 버전 줄 7회 연속 탭 → "시험 모드" 메뉴(3회부터 남은 횟수 토스트) |
| 저장 | `session/SettingsStore.kt` — 시험 모드 on/off·계측기 host/port(기본 7120)·연결 키·인증서 확인(끔 = TOFU)·연결 on/off·내보낼 회선 |
| 링크 소유 | `session/DispatchService.kt`(Foreground Service) — 세션과 같은 수명. 화면이 꺼져도 유지. 앱이 등록을 끝낸 뒤 `DeviceLink.start` |
| 설정값 | `deviceId` = `core/device/DeviceIdentity.kt` 와 같은 원천(`+sip.instance`) · `pinFile` = `filesDir/device_link.pin` · 기준 음원 = APK asset 을 `filesDir` 로 복사한 경로 |
| 기준 음원 | `tester/worker/samples/pcm/conv_p59_a.wav`(16 kHz·mono·PCM16 — pjmedia WAV 재생기 조건)를 asset 으로 동봉 |
| 표시 | 하단 내비 위 띠 + 상시 알림에 "시험 모드"·"계측기 연결됨 · <워커>" 배지. 계측기가 구동한 호는 호 화면에 "계측기" 표지 |
| 화면 | 통화 중 오버레이(`CimsUe.callQuality` 1 초) · 호 종료 요약 · 최근 50 호 이력 · 내보내기(JSON — 공유 시트) · [지문 초기화] 버튼(pinFile 삭제) |
| 착신 | 링크가 붙어 있어도 착신은 평소처럼 알린다. 계측기가 `answer` 를 보내면 코어가 받는다 — 앱은 그 호를 평소 호처럼 보여 주면 된다 |

### 3.2 Windows 관제 데스크톱 (`windows/dispatch-desktop`) — Windows 개발 환경

| 항목 | 위치·방법 |
|---|---|
| 진입 | `Shell/SettingsWindow.xaml` 의 버전 표시 7회 클릭 → 시험 모드 영역 |
| 저장 | 기존 설정 저장소(`Services/AppPaths.cs` 경로) — 항목은 §3.1 과 같다 |
| 링크 소유 | 앱 수명(세션 서비스) — 등록 뒤 `DeviceLink.Start` |
| 설정값 | `deviceId` = `Platform.DeviceIdentity`(MachineGuid 기반 `urn:uuid:`) · `pinFile`·기준 음원 = `AppPaths` 데이터 폴더 |
| 표시 | 상단 바 배지 · 관제 요약 띠([dispatch_desktop_ui.md](../design/features/dispatch_desktop_ui.md) §3.5)에 "계측기 연결됨" |
| 화면 | 오버레이는 호 카드의 한 줄(코덱·손실·지터·RTD·MOS-CQ) · 요약·이력·내보내기(저장 대화 상자) |

---

## 4. 시험 절차

### 4.1 계측기 — .48 배포 완료

| 항목 | 값 |
|---|---|
| 배포 | `cims-tester-worker` **0.1.32**(dep 8, 워커 이름 `media01`) · `oam-cims-tester` **0.1.35**(dep 7) · `oam` **0.2.177**(dep 1 — 콘솔 번들에 실기기 풀 화면) |
| 계측기 주소(단말에 넣을 값) | **`121.161.164.48:7120`** (TLS, `0.0.0.0:7120` 수신) |
| 연결 키 | **없음**(`Device.PairKey` 비움 — 아무 단말이나 붙는다). 쓰려면 워커 배포 설정에 넣고 재기동: `scripts/oam-deploy.py config 8 …` |
| 워커 인증서 지문(TOFU) | `8ed1e6355b9fe729fcd380f1da9156734863f1a114fc0fdd28110b4bf8e96f86` — 자체 서명, `/opt/cims-agent/modules/cims-tester-worker/runtime/device.{crt,key}`(업그레이드·재기동에도 같다 — 재기동 뒤 같은 지문 확인) |
| 확인 | 워커 health `devices.listening=true` · 컨트롤러 `GET /api/v1/tester/devices?topology=1` · 이 서버의 `cimsue-cli link 121.161.164.48:7120` 이 붙어 목록에 회선·등록 상태가 보였다 |
| 토폴로지 | tb48 = id 1. `device` 풀은 아직 없다 — 시험할 번호가 정해지면 콘솔 토폴로지 편집기에서 실기기 풀을 놓고 "연결된 단말"에서 [추가] |

- **망** — 단말이 `121.161.164.48:7120/tcp` 에 닿아야 한다(방화벽·Wi-Fi/VPN). 단말이 먼저 붙으므로 단말 쪽 NAT 는 상관없다.
- **앱 없이 먼저** — Windows 나 .45 에서도 `cimsue-cli … link 121.161.164.48:7120` 로 같은 경로를 확인할 수 있다(앱과 같은 코어 링크).

### 4.2 단계별 시험

1. **링크만** — 앱에서 시험 모드를 켜고 계측기 주소와 연결 키를 넣는다. 콘솔 토폴로지 편집기의 실기기 풀 "연결된 단말"에 단말이 보이는지,
   회선별 등록 상태가 맞는지 확인한다(또는 `GET /api/v1/tester/devices`).
2. **풀·계획** — 토폴로지에 `device` 풀(번호 = 앱 회선 번호, `access` = 앱이 등록한 접속점)을 두고 계획 미리보기에 실기기 오류가 없는지 본다.
3. **시나리오** — `VOLTE-CALL-DEVICE-MO`·`-MT`, 단말이 둘이면 `-E2E`(같은 풀 번호 둘). `ht` 는 20 이상을 권장한다(RTD 는 첫 RTCP 교환 뒤에 잡힌다).
4. **기대값 비교** — .48 에서 `cimsue-cli link` 로 잰 기준값: SRD ≈ 1.08 s, 손실 0, RTD 0.6~1.2 ms(같은 LAN), MOS-CQ 4.27(AMR-WB).
   실기기는 무선 구간·음향 경로 때문에 지터·RTD 가 더 크다 — 그 차이가 측정하려는 것이다.
5. **송출** — 풀 `media: sample` 이면 상대 쪽에서 기준 음원이 들리는지 귀로 확인한다(이 서버에서는 들어 볼 수 없었다). `mic` 이면 사람이 말한다.

### 4.3 회신 — .48 로 돌려줄 것

- §1 빌드 결과: 통과 여부. 실패하면 오류 전문.
- §2 바인딩: 최종 API 이름(C·.NET·Kotlin). 설계 §5.3 에 반영한다.
- §4.2 run id 와 요약값(`device_*`·`rtd_ms`·`mos`). 실단말 기준값으로 설계 §7 에 싣는다.
- 기준 음원이 들렸는지, 오버레이 값이 계측기 요약과 맞았는지.

---

## 5. 함정

- **같은 `device_id` 로 두 번 붙으면** 워커가 옛 연결을 닫는다. 그 연결을 쓰던 run 은 `device_link_lost` 로 실패한다. 앱을 두 번 띄우거나
  옛 프로세스가 남아 있으면 이렇게 된다.
- **등록은 앱 것이다.** `register` 단계는 앱 등록 확인일 뿐이라, 앱이 미등록이면 480(`device not registered (app)`)으로 실패한다.
- **워커 인증서가 바뀌면**(파일 삭제·재생성) 단말이 `Refused(pin_mismatch)` 로 멈춘다. 앱 [지문 초기화]로 푼다.
- **WAV 형식** — 기준 음원은 PCM16 WAV 여야 한다(pjmedia 재생기). 16 kHz mono 를 권장한다.
- **run 중 재접속** — 링크가 끊겼다가 다시 붙어도 그 run 의 풀에는 다시 묶이지 않는다(설계 §9 향후 과제).
- **PTT 그룹 세션** — 실기기 하나와 가상 멤버를 한 그룹 세션에 섞지 못한다(멤버 역할 = 같은 풀). 실기기 PTT 는 멤버 전원이 `device` 풀이어야 한다.
- **포트** — 7110 은 컨트롤러의 워커 관측 수신이다. 단말에는 7120(워커 `Device.Port`)을 넣는다.

---

## 6. .45 반영 결과 (→ .48)

| 항목 | 결과 |
|---|---|
| §1.1 A1 엔진 재빌드 | ✅ `build-native.sh` 가 매번 `make clean` 뒤 전체 빌드 — 펼친 `config_site.h` 에 `PJMEDIA_HAS_RTCP_XR 1`·`PJMEDIA_STREAM_ENABLE_XR 1`, `libcimsue.so` 에 `pjmedia_rtcp_xr_*` 링크 |
| A2 `pjsua_call_get_stream_stat_xr` | ✅ NDK 링크(`libcimsue.so` T 기호) |
| A3 SWIG 게터 | ✅ `CallQuality.getRLq()`·`getRCq()`·`getMosCq()` — Kotlin `q.rLq`·`q.rCq` 로 풀린다(파사드 컴파일 통과). `SWIGTYPE_p_*` 0(S1-UE-ANDROID-BIND PASS) |
| A4 `addObserver`·`removeObserver`·`setTxSource` | ✅ SWIG 재생성(`Engine.java`). 파사드 노출은 `setTxSource` 만(관찰자는 링크 내부) |
| A5 코어 새 소스 NDK 컴파일 | ✅ `quality/*`·`net/tls_stream.cpp`·`drive/*` 컴파일, `DeviceLink` 바인딩 뒤 `.so` 에 링크(JNI 진입점 38) |
| §2 SWIG·Kotlin `DeviceLink` | ✅ `cimsue.i` — `%include "cimsue/drive.h"`, `DeviceLinkListener` director, `DriveSession`·`LineSink`·`drive::` `%ignore`, `DriveAccountVector`. 파사드(`sdk/android/cimsue/.../DeviceLink.kt`·`Types.kt`·`CimsUe.kt`) — 이름은 설계 [§5.3](../design/features/ue_voice_quality.md) 에 반영 |
| Kotlin API | `CimsUe.deviceLink(): CimsResult<DeviceLink>` · `DeviceLink.start(DeviceLinkConfig, List<DriveAccount>)`(suspend) · `stop()`(suspend) · `status: StateFlow<LinkStatus>`(`LinkState` IDLE/CONNECTING/CONNECTED/DISCONNECTED/REFUSED + `detail`) · `state` · `close()` · `CimsUe.setTxSource(wav)`. `DeviceLinkConfig` = 코어 설정 + `sampleFile`. `CimsUe.close()` 가 열린 링크를 엔진보다 먼저 해제 |
| `:cimsue` AAR ↔ 관제 태블릿 | ✅ 태블릿은 `project(":cimsue")` — `dispatch-tablet`·`ptt-client`·`volte-client` debug APK 빌드 통과. 단독 AAR 패키징(`:cimsue:assemble*`)은 이 서버에 lint 도구 jar 캐시가 없어 오프라인에서 멈춘다(환경) |
| 단위시험 | `:cimsue` 30(`FacadeMappingTest` 16 — `LinkState` 서수·이름 대조 추가) · `:ptt-client` 53(`FloorClient.kt` 짧은 PTT 탭 수정분 포함 빌드) · `cimsue_test` **73 한 프로세스 PASS**(고치기 전 = 같은 바이너리에서 assert 134 재현) · S1-UE-UNIT·FLOOR-CODEC·ANDROID-BIND·ENGINE-SINGLE·SDS-XCHECK·CSC-XCHECK PASS |
| 실기기 링크 | 미실측 — 앱 시험 모드(Q4, §3) 전이라 앱에서 링크를 켜는 곳이 없다 |
| §3.0 `volte-client`·`ptt-client` SDK 이식 | 미착수 — `android/core` 자체 pjsua2 래퍼·`PttController` 이전 범위(ue_sdk.md §5.3 의 `domain/` 결정 포함)를 정한 뒤 |

