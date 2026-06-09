# 안드로이드 개발 환경 구성 (Windows + WSL2 + UNIWA)

CIMS 안드로이드 단말 앱(`android/`) 개발을 위한 환경 셋업 가이드.
대상 환경: **Windows 11** + **WSL2(Ubuntu)** + **UNIWA 실기기**.

---

## 0. 한눈에 — 무엇을 어디서

| 작업 | 도구 | 위치 |
|---|---|---|
| 앱(Kotlin/Compose) 빌드·실행 | Android Studio + SDK + JDK 17 | **Windows** |
| MediaCodec 스파이크(M0) 실행 | 위 + UNIWA 단말(adb) | **Windows → 단말** |
| PJSIP `.so` + SWIG Java 빌드 | NDK + SWIG + 빌드도구 | **WSL2(Ubuntu)** |
| 단말 배포(adb) | platform-tools | **Windows** (WSL 불필요) |

> 핵심: **MediaCodec 스파이크는 PJSIP·WSL2 없이** Android Studio만으로 바로 실행된다.
> WSL2는 나중에 PJSIP 네이티브 산출물을 만들 때만 쓰고, 그 `.so`를 Windows 쪽 `core/jniLibs`에 복사한다.

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
> (선택) Android 11+는 **무선 디버깅**(개발자 옵션) 가능. UNIWA가 구버전이면 USB 사용.

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

## 4. WSL2 — PJSIP 네이티브 빌드 툴체인 (스파이크 통과 후)

### 4.1 WSL2 설치
```powershell
wsl --install -d Ubuntu      # 재부팅 후 Ubuntu 초기 설정
```
### 4.2 빌드 도구 + NDK + SWIG
상세 절차/`config_site.h`/빌드 명령은 → **[M0_pjsip_build_wsl2.md](M0_pjsip_build_wsl2.md)**.

### 4.3 파일 위치 (중요)
- 저장소는 Windows에 있고 WSL에서 `/mnt/c/work/cims`로 접근 가능하나, **빌드는 WSL 홈(`~/`)에서** 하는 게 빠르다(`/mnt/c`는 느림).
- PJSIP 빌드 산출물만 Windows 쪽으로 복사:
  ```bash
  cp libpjsua2.so /mnt/c/work/cims/android/core/src/main/jniLibs/arm64-v8a/
  cp -r org/pjsip/pjsua2 /mnt/c/work/cims/android/core/src/main/java/org/pjsip/
  ```
- **adb/단말 배포는 Windows에서** 한다(WSL2의 USB 패스스루는 불필요).

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
| `adb devices`에 단말 없음 | USB 디버깅·케이블·드라이버(Google USB / MTK) 확인 |
| `unauthorized` | 단말에서 RSA 지문 수락(이전 수락 취소: 개발자 옵션 → USB 디버깅 승인 취소 후 재연결) |
| Gradle JDK 오류(CLI) | `JAVA_HOME`을 JBR(17 이상, 21 OK) 경로로 |
| 라이선스 미동의 빌드 실패 | `sdkmanager --licenses` |
| Compose 빌드 오류 | Kotlin 버전 ↔ compose 플러그인 버전 일치 확인(둘 다 2.4.0) |
| WSL `/mnt/c` 빌드 느림 | WSL 홈에서 빌드 후 산출물만 복사 |
