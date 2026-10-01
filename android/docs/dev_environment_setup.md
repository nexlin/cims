# 안드로이드 개발 환경 구성 (Windows + WSL2 + UNIWA)

CIMS 안드로이드 단말 앱(`android/`) 개발을 위한 환경 셋업 가이드.
대상 환경: **Windows 11** + **Linux 빌드 환경**(WSL2 또는 ssh 로 닿는 VM — §4) + **UNIWA 실기기**.

---

## 0. 한눈에 — 무엇을 어디서

| 작업 | 도구 | 위치 |
|---|---|---|
| 앱(Kotlin/Compose) 빌드·실행 | Android Studio + SDK + JDK 17 | **Windows** |
| MediaCodec 스파이크(M0) 실행 | 위 + UNIWA 단말(adb) | **Windows → 단말** |
| 엔진·코어 `.so` + SWIG Java 빌드 | NDK + SWIG + 빌드도구 | **Linux**(WSL2 또는 ssh 빌드 호스트 — §4) |
| 단말 배포(adb) | platform-tools | **Windows** (WSL 불필요) |

> 핵심: **MediaCodec 스파이크는 PJSIP·WSL2 없이** Android Studio만으로 바로 실행된다.
> Linux 는 네이티브 산출물을 만들 때만 쓰고, 산출물은 `build-native.sh` 가 두 SDK 모듈(`sdk/android/cimsue-engine`·`sdk/android/cimsue`)에 배치한다(§4).

---

## 1. Windows — Android Studio + SDK + JDK

### 1.1 설치
- **Android Studio** 최신 안정판 설치 (JetBrains Runtime **JDK 17** 내장 → Studio 빌드엔 별도 JDK 불필요).
- 최초 실행 마법사에서 **Android SDK** 기본 설치 진행.

### 1.2 SDK Manager (Settings → Languages & Frameworks → Android SDK)
- **SDK Platforms**: `libs.versions.toml`의 `compileSdk` 값에 해당하는 SDK Platform 체크 (현재 **API 37**).
  - UNIWA 단말이 더 낮은 버전이어도 무관(런타임은 단말 버전, 컴파일 SDK는 별개).
- **SDK Tools**:
  - `Android SDK Build-Tools` (최신)
  - `Android SDK Platform-Tools` ← **adb** 포함(필수)
  - `Android SDK Command-line Tools (latest)`
  - (선택) `NDK (Side by side)` + `CMake` — PJSIP는 WSL2에서 빌드하므로 Windows NDK는 **선택**.
- 라이선스 동의: 필요 시 `sdkmanager --licenses`.

### 1.3 환경변수 (Windows)
```
ANDROID_HOME = C:\Users\<사용자>\AppData\Local\Android\Sdk
PATH += %ANDROID_HOME%\platform-tools        (adb)
PATH += %ANDROID_HOME%\cmdline-tools\latest\bin   (sdkmanager, 선택)
```
- CLI에서 `gradlew`를 직접 쓸 경우 `JAVA_HOME`을 **JBR(17 이상; 최신 Studio는 21)** 경로 또는 별도 JDK 17로 지정. (Gradle 실행 JDK는 21이어도 OK — 컴파일 타깃 17과 별개)

### 1.4 확인
```powershell
adb --version
sdkmanager --list   # (cmdline-tools 설치 시)
```

---

## 2. UNIWA 단말 연결

### 2.1 단말에서
1. **설정 → 휴대전화 정보 → 빌드번호 7회 탭** → 개발자 옵션 활성화
2. **설정 → 개발자 옵션 → USB 디버깅 ON**
3. USB 케이블로 PC 연결 → 단말에 뜨는 **"USB 디버깅 허용"(RSA 지문)** 수락

### 2.2 Windows 드라이버
- 대부분 표준 ADB로 인식. 미인식 시:
  - `Google USB Driver`(SDK Manager) 설치, 또는
  - UNIWA(보통 MediaTek SoC) **MTK USB/ADB 드라이버** 설치
- 장치 관리자에서 ADB Interface로 잡히는지 확인.

### 2.3 확인
```powershell
adb devices
# 결과에 <serial>  device  로 떠야 정상
#  - unauthorized → 단말에서 RSA 지문 수락
#  - 목록에 없음 → 케이블/드라이버/USB디버깅 확인
```
### 2.4 무선 디버깅 (Android 11+)
USB 없이 같은 Wi-Fi 망에서 붙는다. 단말 **개발자 옵션 → 무선 디버깅 ON**.
```powershell
# ① 페어링 — PC 마다 한 번. 단말 [페어링 코드로 기기 페어링] 창이 보여 주는 주소·포트와 6자리 코드
adb pair <단말 IP>:<페어링 포트> <코드>
# ② 연결 — 무선 디버깅 화면 위쪽의 «IP 주소 및 포트»(페어링 포트와 다르고, 무선 디버깅을 껐다 켜면 바뀐다)
adb connect <단말 IP>:<포트>
adb devices          # <단말 IP>:<포트>  device
```
- 페어링 안 된 PC 의 `adb connect` 는 포트가 열려 있어도 `failed to connect` 로 떨어진다.
- 같은 망의 무선 디버깅 단말과 지금 포트는 `adb mdns services` 가 보여 준다(`_adb-tls-connect._tcp`).
- UNIWA 가 구버전이면 USB 를 쓴다.

---

## 3. 프로젝트 열기 & M0 스파이크 실행

1. Android Studio → **Open** → `C:\work\cims\android` 선택.
2. **Gradle sync** 자동 실행 → `gradlew`/`gradle-wrapper.jar` 생성됨
   (저장소엔 `gradle-wrapper.properties`만 포함).
   - sync 시 **AGP/Gradle/Kotlin 업그레이드 제안**이 뜰 수 있음 → 수락하거나 `gradle/libs.versions.toml` 버전 조정.
3. 상단 기기 선택에서 **UNIWA** 선택 → `volte-client` **Run(▶)**.
4. 앱에서:
   - **[코덱 가용성]** → AMR-WB/H.264 인코더·디코더(SW/HW) 목록
   - **[AMR-WB 스파이크]** → 인코드+디코드 처리량이 실시간 예산 내인지(**M0 게이트**)

### CLI 대안
```powershell
cd C:\work\cims\android
gradle wrapper --gradle-version 8.11.1   # 최초 1회(시스템 gradle 필요) — 또는 Studio가 생성
.\gradlew :volte-client:installDebug      # 단말에 설치
adb shell am start -n com.cims.ue.volte/.MainActivity
```

---

## 4. 네이티브 빌드(Linux) — 엔진·코어 `.so` + SWIG Java

엔진(`ext/pjproject`)·코어(`sdk/core`)의 Android 산출물은 **Linux 에서** `sdk/android/build-native.sh` 가 짓고
두 AAR 모듈(`sdk/android/cimsue-engine`·`sdk/android/cimsue`)에 배치한다(생성물은 커밋하지 않는다 —
[android_dispatch_tablet.md](../../docs/design/features/android_dispatch_tablet.md) §2.1·§2.2). APK 는 그 뒤 Windows 에서 Gradle 로 짓는다(§4.3).
Linux 전제(한 번) = JDK 17·SWIG 4.2·NDK r27+(`scripts/m1_provision.sh` → `~/.m1env`) + Android arm64 정적 OpenSSL
(`scripts/m1_build_openssl.sh`) + make·gcc·cmake 3.22+·python3·perl·file·git — 절차 정본은 [M1_pjsip_build_ubuntu.md](M1_pjsip_build_ubuntu.md).

### 4.1 WSL2 가 있는 PC
```powershell
wsl --install -d Ubuntu      # 관리자 + 재부팅
```
WSL 홈(`~/`)에 트리를 두고 `sdk/android/build-native.sh` 를 돌린다. `/mnt/c` 는 느리고, Windows 작업 사본은 `core.autocrlf` 로
CRLF 라 configure 스크립트가 그대로 돌지 않는다.

### 4.2 WSL 이 없는 PC — ssh 로 닿는 Linux 빌드 호스트(VM)
Git Bash 에서 `sdk/android/build-native-remote.sh` 하나로 끝난다 — 작업 사본(커밋 안 한 수정·새 파일 포함)을 LF 트리로 만들어
빌드 호스트의 `~/cims-android-src` 로 push → 거기서 `build-native.sh` → 두 모듈의 생성물을 이 트리로 받아 온다. 빌드 호스트에
OpenSSL 이 없으면 먼저 짓는다.
```bash
# 호스트 = ~/.ssh/config 별칭(키 인증). VMX 를 주면 VMware VM 을 nogui 로 먼저 띄운다.
export CIMS_ANDROID_BUILD_HOST=nex-ubuntu
export CIMS_ANDROID_BUILD_VMX='C:\work\vms\Ubuntu 64-bit.vmx'
sdk/android/build-native-remote.sh               # build-native.sh 인자(--engine-only 등)는 그대로 넘긴다
sdk/android/build-native-remote.sh --sync-only   # 보내기만
```
첫 실행은 OpenSSL·pjproject·코어를 모두 짓는다. 다음부터는 바뀐 파일만 보내고, 빌드는 build-native.sh 규칙대로 다시 한다.

### 4.3 APK (Windows)
```powershell
cd C:\work\cims\android
$env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr'   # Gradle 데몬 = JDK 21 (gradle/gradle-daemon-jvm.properties)
.\gradlew.bat :dispatch-tablet:assembleDebug        # → dispatch-tablet\build\outputs\apk\debug\dispatch-tablet-debug.apk
.\gradlew.bat :cimsue:testDebugUnitTest :dispatch-tablet:testDebugUnitTest   # S1-UE-TABLET-UNIT
.\gradlew.bat :dispatch-tablet:installDebug         # 단말 설치(adb)
```
**adb/단말 배포는 Windows 에서** 한다(빌드 호스트에 USB 를 넘길 필요 없음).

### 4.4 한 번에 — `android/tablet.sh` (Git Bash)
§4.2·§4.3 과 단말 설치를 묶은 관제 태블릿 앱의 진입점이다.
```bash
android/tablet.sh doctor              # 환경 점검 — JDK·SDK·Gradle 데몬·빌드 호스트·네이티브 생성물·APK·단말
android/tablet.sh up                  # native(필요할 때만) → apk → install
android/tablet.sh native [--force]    # 네이티브만 (build-native.sh 인자를 주면 그대로 넘긴다)
android/tablet.sh apk                 # APK 만
android/tablet.sh install [-s 시리얼] # 설치하고 실행 — 단말이 하나면 -s 생략, 무선이면 시리얼 = <IP>:<포트>
```
- **네이티브는 입력이 바뀌었을 때만 짓는다.** `build-native-remote.sh` 가 전체 빌드를 마칠 때 빌드 입력(§4.2 가 보내는 경로들의 작업 사본)의
  지문을 `sdk/android/cimsue/src/main/jniLibs/native-inputs.sha1` 에 남기고, `tablet.sh` 가 지금 지문(`build-native-remote.sh --inputs-hash`)과
  대조한다 — Kotlin 만 고친 뒤의 `up` 은 Gradle 과 설치만 돈다. 무조건 다시 지으려면 `native --force`.
- `JAVA_HOME` 이 없거나 jlink 없는 JRE 면 Android Studio JBR 을 쓰고, jlink 없는 JRE 로 뜬 Gradle 데몬이 있으면 빌드 전에 내린다(§6).
- 무선 디버깅 단말이 mDNS 자동 연결과 `adb connect` 로 두 번 잡혀도 단말 시리얼이 같으면 하나로 센다(`-s` 없이 설치된다).
- 설치 전에 단말 ABI(arm64-v8a)를 확인한다. `INSTALL_FAILED_UPDATE_INCOMPATIBLE` = 단말에 다른 PC 의 디버그 키로 서명된 앱이 있다
  (디버그 키는 PC 마다 다르다) — `adb uninstall com.cims.ue.dispatch` 뒤 다시 설치한다(앱 데이터가 지워진다).

---

## 5. 버전 정합 (현재 스캐폴드 기준)

| 항목 | 값 | 비고 |
|---|---|---|
| Gradle 실행 JDK | **17+** | Studio JBR 내장(최신=21). 바꿀 필요 없음 |
| 컴파일 타깃(jvmTarget) | **17** | `build.gradle.kts` — 실행 JDK와 별개 |
| AGP | 9.2.1 | `libs.versions.toml` |
| Gradle | 9.4.1 | `gradle-wrapper.properties` |
| Kotlin | 2.4.0 | + compose 플러그인 |
| compileSdk / targetSdk | 37 | `libs.versions.toml` |
| minSdk | 26 | UNIWA Android 버전 확정 시 재조정 |

> UNIWA 모델/Android 버전 확정되면: API ≥ 26이면 그대로, 더 낮으면 `minSdk` 하향.

---

## 6. 트러블슈팅

| 증상 | 조치 |
|---|---|
| sync 시 AGP/Gradle 버전 오류 | 제안 수락 또는 `libs.versions.toml`/wrapper 버전 정합 |
| `:cimsue:compileDebugJavaWithJavac` — `JdkImageTransform` … `jlink executable … does not exist` | 다른 IDE(예: VS Code 계열 Java 확장)가 띄운 Gradle 데몬이 jlink 없는 JRE 로 떠 있고 CLI 가 그 데몬을 재사용했다(데몬 조건 = «Java 21, 벤더 무관»). `.\gradlew.bat --stop` 뒤 `JAVA_HOME`=Android Studio JBR 로 다시 빌드 |
| `adb devices`에 단말 없음 | USB 디버깅·케이블·드라이버(Google USB / MTK) 확인 |
| `unauthorized` | 단말에서 RSA 지문 수락(이전 수락 취소: 개발자 옵션 → USB 디버깅 승인 취소 후 재연결) |
| Gradle JDK 오류(CLI) | `JAVA_HOME`을 JBR(17 이상, 21 OK) 경로로 |
| 라이선스 미동의 빌드 실패 | `sdkmanager --licenses` |
| Compose 빌드 오류 | Kotlin 버전 ↔ compose 플러그인 버전 일치 확인(둘 다 2.4.0) |
| WSL `/mnt/c` 빌드 느림 | WSL 홈에서 빌드 후 산출물만 복사 |
