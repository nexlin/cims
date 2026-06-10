# CIMS UE — M1(VoLTE 1:1 음성+영상) 종합 설계서

본 문서는 신규 안드로이드 단말 앱의 M1 마일스톤(VoLTE 1:1 음성+영상)에 대한 4개 트랙(빌드 / SipController / 코덱 통합 / M1 시퀀스) 설계를 단일 정본으로 종합한다. 각 트랙의 검증 단계에서 도출된 교정(issues)을 모두 반영했으며, 머신 실측이 필요한 항목은 `verify-on-machine`으로 명시한다.

> 작성 기준일 2026-06-09. 작성자 지식 시점(2026-01) 한계로 PJSIP/NDK/SWIG 버전 의존 사실은 실측 대상으로 표기한다.

---

## 1. 개요와 M1 범위

### 1.1 모노레포·스택 컨텍스트

| 항목 | 값 |
|---|---|
| 모노레포 | `android/core`(공유 라이브러리: PJSIP 래퍼·코덱·SIP·미디어제어), `android/volte-client`(M1 앱), `android/ptt-client`(M2+) |
| 시그널링/미디어 | PJSIP(pjsua2) — SIP + RTP/지터버퍼/AEC/conference bridge |
| 코덱 | 음성 AMR-WB, 영상 H.264 — **모두 And-Media(Android MediaCodec, 경로 C) 확정**(2026-06-10, §4.1) |
| 빌드 baseline | AGP 9.2.1 / Gradle 9.4.1 / Kotlin 2.4.0 / compileSdk 37 / minSdk 26 / JVM target 17 |
| 라이선스 | GPL 공개 |
| 타깃 | UNIWA 러기드/PoC 안드로이드(arm64-v8a, 실기기 보유) |
| 테스트 서버 | CSP SIP `121.161.164.47:15060`(UDP 가정 — 실측), realm/domain `ims.mnc033.mcc450.3gppnetwork.org` |

이미 구현됨: `SipAccountConfig`(serverHost/Port/transport/domain/msisdn/displayName/loginId/authId/password/expiresSec; `aor=sip:msisdn@domain`; `effectiveAuthId`) + `ConfigStore`(SharedPreferences). volte-client 첫 실행 `ConfigScreen` + `HomeScreen`(코덱점검 + 비활성 REGISTER placeholder). **M0 게이트 통과**(실기기 AMR-WB ENC+DEC·H.264 MediaCodec 존재 + AMR-WB 루프백 실시간 처리량 충족).

### 1.2 M1 범위 / 비범위

| 구분 | 내용 |
|---|---|
| **M1 범위** | SIP-over-UDP REGISTER(Digest MD5, qop=auth) → INVITE → 1:1 AMR-WB 음성 → H.264 영상 → BYE. 단말 단독으로 CSP에 붙어 1:1 호 성립. |
| **명시적 비범위(M2+)** | floor control(MCPT RTCP-APP), CSC(OAuth2 PKCE / XCAP), 그룹콜(PTT-AS), affiliation(PUBLISH), 다자/믹싱. **M1 코드·SDP·UI 어디에도 floor/CSC 흔적을 넣지 않는다.** |
| **선행(완료)** | M0 게이트 |

### 1.3 하위 마일스톤

| 단계 | 내용 | 게이트 |
|---|---|---|
| **M1.0** | PJSIP arm64 빌드 + core 통합 + 부팅 스모크 | Endpoint `libStart` 성공 + UDP transport + 코덱 등록 확인 |
| **M1.1** | REGISTER(Digest MD5, qop=auth, realm=domain) | 401→200 왕복 + 자동 재등록 |
| **M1.2** | 1:1 AMR-WB 음성 | 양방향 AMR-WB + 정상 BYE |
| **M1.3** | H.264 영상 | 양방향 H.264 디코드 렌더 |

### 1.4 절대 빌드 제약 (전 트랙 공통)

`org.pjsip.pjsua2`를 참조하는 코틀린/자바 코드는 `libpjsua2.so` + SWIG Java 바인딩이 core에 **실제 투입된 이후(M1.0 완료 후)** 에만 컴파일 대상에 넣는다. 그 전에 넣으면 빌드가 깨진다. 본 문서의 모든 PJSIP 참조 스켈레톤은 **`// PJSIP 통합 후 core 투입`** 라벨을 단다. pjsua2 비참조 순수 코틀린(`SipModels.kt` 등)만 선투입 가능.

---

## 2. PJSIP 안드로이드 빌드 플레이북 (M1.0, Ubuntu)

### 2.1 목표와 산출물

Ubuntu 24.04(WSL2/VM/네이티브 무관)에서 `pjproject`를 `arm64-v8a`용으로 크로스컴파일하여 산출물을 `android/core`에 투입한다. **실행 절차·스크립트·실측 상태의 정본은 [android/docs/M1_pjsip_build_ubuntu.md](../../../android/docs/M1_pjsip_build_ubuntu.md)** (2026-06-10: 개발 PC WSL 미설치 실측 → VMware VM `nex-ubuntu` 에 루트 불필요 프로비저닝 완료. 사용자 Ubuntu 전환 시 동일 스크립트 재사용).

| 산출물 | 빌드 출력 경로(SWIG 기준) | core 배치 경로 |
|---|---|---|
| 네이티브 라이브러리 | `.../swig/java/android/pjsua2/src/main/jniLibs/arm64-v8a/libpjsua2.so` | `core/src/pjsua2/jniLibs/arm64-v8a/libpjsua2.so` |
| **STL 런타임(필수)** | `.../jniLibs/arm64-v8a/libc++_shared.so` | `core/src/pjsua2/jniLibs/arm64-v8a/libc++_shared.so` |
| SWIG Java 바인딩 | `.../swig/java/android/pjsua2/src/main/java/org/pjsip/pjsua2/*.java` | `core/src/pjsua2/java/org/pjsip/pjsua2/*.java` |

> **`libc++_shared.so`는 옵션이 아니라 필수 동봉.** PJSIP는 NDK STL을 shared(`c++_shared`)로 링크하므로 `libpjsua2.so`만 복사하면 앱 실행 즉시 `UnsatisfiedLinkError: libc++_shared.so` 또는 dlopen 실패가 **100% 발생**한다. (`-static-libstdc++`는 PJSIP 권장 경로가 아님 — c++_shared 동봉이 표준.)

부팅 스모크(M1.0) 게이트: `Endpoint.libCreate()/libInit()/libStart()` 성공 + UDP transport 1개 + 작업 스레드 `libRegisterThread()` + **`codecEnum2()` 출력에 AMR-WB 항목 존재**. REGISTER는 범위 밖.

### 2.2 WSL2 / 빌드 도구 / NDK / SWIG / JDK

- **Ubuntu 24.04 (WSL2/VM/네이티브 무관).** 빌드는 전부 **리눅스 ext4(`~/`)** 안에서 수행. NTFS(`/mnt/c` 등) 직접 빌드 금지(심볼릭링크/권한/속도 문제). 산출물만 마지막에 Windows core로 복사. sudo 불가 환경은 **userland 프로비저닝**(JDK tarball + swig deb 추출 + sdkmanager — `android/docs/scripts/m1_provision.sh`)으로 우회 가능(실측 검증).
- 리눅스 패키지: `build-essential autoconf automake libtool pkg-config git curl unzip python3 swig`.
- **JDK 17 / SWIG 4.x**: PJSIP 공식 요구사항이 아니라 **본 프로젝트가 고정한 호스트 빌드 버전**(versionSensitive). JDK17은 앱 JVM target과 정합. SWIG는 apt의 4.x면 통상 동작, 빌드 실패 시 소스 빌드. (구체 하한 "4.0.2"는 출처 불명이라 단정하지 않음.)
- **NDK r28 계열** 권장 — Android 15+ 16KB page-size 기본 대응. **실측 설치: `28.2.13676358`**(sdkmanager 가 28.x 최신 자동 선택). `platforms;android-37`은 SWIG/native 빌드엔 불필요(앱 gradle 빌드는 Windows 호스트 SDK 사용).

```bash
export ANDROID_SDK_ROOT=$HOME/android-sdk
export PATH=$ANDROID_SDK_ROOT/cmdline-tools/latest/bin:$PATH
yes | sdkmanager --licenses
sdkmanager "ndk;28.2.13676358" "platform-tools"
export ANDROID_NDK_ROOT=$ANDROID_SDK_ROOT/ndk/28.2.13676358
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
```

### 2.3 외부 코덱 라이브러리 선빌드 — **경로 C 확정으로 불필요** (2026-06-10)

> **음성=경로 C(And-Media) 확정(§4.1)으로 본 단계는 통째로 삭제된다.** And-Media AMR-WB는 기기 MediaCodec(`audio/amr-wb`)을 PJSIP 내부 코덱으로 구동하므로 외부 코덱 라이브러리 크로스컴파일이 없다. `and_aud_mediacodec.cpp`가 **2.16 태그에 실존**함을 1차 근거로 확인(GitHub API 트리 + raw 소스 grep, 2026-06-10): 코덱 테이블 `{"AMR-WB", "audio/amr-wb", …, 16000, 1, …}`(`:340`), 기본 fmtp `octet-align=1`(`:346`), 인코더 후보 `OMX.google.amrwb.encoder`/`c2.android.amrwb.encoder`(`:220-224`).
>
> (경로 A 폴백 시에만) opencore-amr + vo-amrwbenc arm64 선빌드가 부활한다 — 절차는 git 이력의 본 절 구판(커밋 `b1277d94`) 참조.

### 2.4 pjproject 소스: 2.16 태그 고정

선정 근거: 2.16은 Android MediaCodec 비디오/오디오 코덱 매크로가 안정 적용된 검증된 안정판. 2.17은 비동기 SIP 인증·CMake 개편 등 변경폭이 커 M1 스모크 단계에서 변수 최소화 목적상 **2.16 고정**(M4 SRTP/TLS 진입 시 재평가).

```bash
git clone https://github.com/pjsip/pjproject.git && cd pjproject
git checkout 2.16
git describe --tags     # 2.16 기대
```

무결성: 릴리스 tarball 사용 시 GitHub release 자산 SHA-256 비교·기록(사내 S4 immutability gate 관행).

### 2.5 config_site.h — 코덱 전략

`pjlib/include/pj/config_site.h`를 **신규 생성**한다. 핵심 원칙: **AMR-WB/H264 같은 매크로는 Android 빌드에서 기본 1이라 "켜는" 재선언은 no-op이며, 실제 의미 있는 작업은 "끄는" 쪽**(불필요 코덱 빌드 제외)이다. 또한 우리 오버라이드는 `config_site_sample.h` **include 뒤**에 와야 sample 기본값을 덮어쓴다(순서 의존성).

```c
/* pjlib/include/pj/config_site.h  (신규 생성) */

/* 1) Android 표준 설정 자동 적용 — 반드시 우리 오버라이드보다 먼저 */
#define PJ_CONFIG_ANDROID 1
#include <pj/config_site_sample.h>

/* ── 이하 오버라이드는 sample include '뒤'라야 효력 ───────────────── */

/* 2) 영상: H.264 유지(기본 1, 아래는 명시 확인용 no-op), 협상표면 축소 위해 VP8/VP9 빌드 제외 */
#define PJMEDIA_HAS_VIDEO              1
#define PJMEDIA_HAS_AND_MEDIA_H264     1   /* 기본 1 (no-op, 가독성용) */
#define PJMEDIA_HAS_AND_MEDIA_VP8      0   /* 실효: 빌드 제외 */
#define PJMEDIA_HAS_AND_MEDIA_VP9      0   /* 실효: 빌드 제외 */

/* 3) 음성 AMR-WB = 경로 C(And-Media/MediaCodec) 확정 (2026-06-10, §4.1) */
/*    opencore 계열은 반드시 명시적으로 끈다('AMR-WB/16000/1' 중복 등록 방지) */
#define PJMEDIA_HAS_AND_MEDIA_AMRWB    1   /* 기본 1 (no-op, 정본 음성 명시) */
#define PJMEDIA_HAS_AND_MEDIA_AMRNB    0   /* M1은 WB만 — NB 빌드 제외(협상표면 축소) */
#define PJMEDIA_HAS_OPENCORE_AMRWB_CODEC  0   /* 실효: opencore factory 빌드 제외(중복 방지) */
#define PJMEDIA_HAS_OPENCORE_AMRNB_CODEC  0

/* 4) 내장 SW 음성코덱 최소화 — 협상 표면 축소 */
#define PJMEDIA_HAS_G711_CODEC   1   /* 디버그/상호운용 안전망으로 유지 권장 */
#define PJMEDIA_HAS_L16_CODEC    0
#define PJMEDIA_HAS_GSM_CODEC    0
#define PJMEDIA_HAS_SPEEX_CODEC  0   /* 코덱만 off. AEC(PJMEDIA_HAS_SPEEX_AEC)는 별개 — 건드리지 말 것 */
#define PJMEDIA_HAS_ILBC_CODEC   0
#define PJMEDIA_HAS_G722_CODEC   0

/* 5) M4 전까지 보안전송 최소화: SRTP/TLS off (UDP only) */
#define PJMEDIA_HAS_SRTP          0
#define PJSIP_HAS_TLS_TRANSPORT   0
```

> **중복 등록 함정(경로 C 방향).** `'AMR-WB/16000/1'` factory가 And-Media + opencore **둘 다** 등록되면 `codecSetPriority`(ID 문자열 기반)의 선택이 비결정적이 된다. 경로 C 확정에 따라 **opencore 계열을 0으로 빌드 제외**해 단일 등록을 보장한다(M1.0 게이트: `codecEnum2()`에 AMR-WB 정확히 1개). 어차피 opencore 라이브러리를 선빌드하지 않으므로(§2.3) 링크 자체가 없지만, 매크로도 명시 0으로 이중 안전.
>
> **Speex 비활성 주의:** `PJMEDIA_HAS_SPEEX_CODEC 0`은 speex *코덱*만 끈다. AEC/preprocessing은 speex DSP를 쓰므로 `PJMEDIA_HAS_SPEEX_AEC`는 별개 — AEC가 필요하면 전처리 매크로는 유지.

### 2.6 configure / build / SWIG

```bash
source ~/.m1env   # m1_provision.sh 산출 (ANDROID_NDK_ROOT/JAVA_HOME/SWIG_LIB/PATH)
cd ~/pjproject

# NDK r28은 16KB 정렬 기본 → page-size 수동 플래그 생략 권장.
# (r27 호환이 꼭 필요할 때만 CFLAGS=-D__BIONIC_NO_PAGE_SIZE_MACRO,
#  LDFLAGS="-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384" 추가하고
#  --use-ndk-cflags 와의 CFLAGS 병합 여부를 configure 요약 로그로 확인)
APP_PLATFORM=28 TARGET_ABI=arm64-v8a ./configure-android --use-ndk-cflags

make dep && make clean && make -j$(nproc)     # 최종 make만 -j 권장. make dep는 직렬
```

플래그/주의:
- `APP_PLATFORM=28` — AMR-WB/H264 MediaCodec 경로는 실질적으로 API 28+ 필요. minSdk는 26이지만 코덱 경로는 28+에서 동작 → 런타임 SDK_INT 가드/타깃 정책 필요(Open Questions).
- **오디오 백엔드 정정(중요):** `--with-oboe`/`--with-ssl` 생략 시 기본은 **Android JNI sound device**다. OpenSL ES는 deprecated이고, **AAudio는 오직 Oboe(`--with-oboe`) 경로로만 도달**한다("기본 OpenSL/AAudio"는 양쪽 다 틀림). M1.0 부팅 스모크는 오디오 디바이스 미관여라 게이트에 무해하지만, **M1.2 음성 품질(에코/지연) 진입 전 `--with-oboe`(OBOE_DIR) 추가를 강력 권장**(Oboe=AAudio 래퍼 → 저지연·AEC 친화).
- And-Media 코덱(오디오 AMR-WB/비디오 H264)은 외부 라이브러리 탐지가 아니라 **Android 타깃 빌드에 내장**(NDK `AMediaCodec`) — configure 요약 표기 유무와 무관하게 §2.5 매크로가 정본. 최종 확인은 M1.0 `codecEnum2()`.

```bash
cd pjsip-apps/src/swig && make     # 직렬. SWIG → JNI glue → libpjsua2.so + org.pjsip.pjsua2/*.java
ls java/android/pjsua2/src/main/jniLibs/arm64-v8a/    # libpjsua2.so, libc++_shared.so 동시 확인
```

### 2.7 core 배치 + build.gradle.kts

SWIG 생성물(수백 파일)을 손코드와 섞지 않도록 **별도 소스셋으로 격리**(권장).

```bash
PJ=~/pjproject/pjsip-apps/src/swig/java/android/pjsua2/src/main
DST=/mnt/c/work/cims/android/core/src/pjsua2
mkdir -p $DST/jniLibs/arm64-v8a $DST/java/org/pjsip/pjsua2
cp $PJ/jniLibs/arm64-v8a/libpjsua2.so       $DST/jniLibs/arm64-v8a/
cp $PJ/jniLibs/arm64-v8a/libc++_shared.so   $DST/jniLibs/arm64-v8a/   # 필수
cp $PJ/java/org/pjsip/pjsua2/*.java         $DST/java/org/pjsip/pjsua2/
```

```kotlin
android {
    namespace = "com.cims.ue.core"
    compileSdk = libs.versions.compileSdk.get().toInt()
    defaultConfig {
        minSdk = libs.versions.minSdk.get().toInt()           // 26
        ndk { abiFilters += "arm64-v8a" }                      // 단일 ABI
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    sourceSets {
        getByName("main") {
            java.srcDir("src/pjsua2/java")        // org/pjsip/pjsua2/*.java
            jniLibs.srcDir("src/pjsua2/jniLibs")  // arm64-v8a/{libpjsua2,libc++_shared}.so
        }
    }
    packaging {
        jniLibs { useLegacyPackaging = false }    // 미압축 .so (16KB page 정렬 유지)
    }
}
```

> `packaging.jniLibs.useLegacyPackaging` DSL 유효성은 AGP 9.2.1에서 Gradle sync로 확인(verify-on-machine).

### 2.8 버전관리 정책

`libpjsua2.so`(수 MB) + `libc++_shared.so` + SWIG `.java`(대량)는 빌드 산출물. **Git LFS** 또는 사내 아티팩트 저장소 권장. 최소한 빌드 메타(`pjproject 2.16`, NDK 버전, `config_site.h` 해시, opencore/vo-amrwbenc 버전)를 README에 고정 기록.

### 2.9 트러블슈팅 빠른 표

| 증상 | 원인/조치 |
|---|---|
| `make` 중 NDK 미발견 | `ANDROID_NDK_ROOT` 미설정/오타 |
| SWIG `command not found`/구버전 | `swig -version`; 미달 시 소스 빌드 |
| 앱 실행 `UnsatisfiedLinkError: libpjsua2.so` | abiFilter/jniLibs 경로 불일치, 또는 에뮬레이터 x86_64 |
| `UnsatisfiedLinkError: libc++_shared.so` | **STL 미동봉** — §2.7대로 함께 복사 |
| pjsua2 호출 즉시 abort | 미등록 스레드 호출 → `libRegisterThread`(§3.4) |
| 16KB 기기 로드 실패 | NDK r28 + `useLegacyPackaging=false` |
| `codecEnum2()`에 AMR-WB 없음 | `PJMEDIA_HAS_AND_MEDIA_AMRWB` 미활성 / 기기 API<28 / 기기 amrwb 코덱 부재(§2.5, §4.6) |

---

## 3. SipController 설계 + Kotlin 스켈레톤

### 3.1 위치·파일 구성 (M1.0 완료 후 투입)

패키지 `com.cims.ue.core.sip`.

| 파일 | 책임 | pjsua2 참조 |
|---|---|---|
| `SipModels.kt` | `RegState`/`CallState` 등 UI 모델(순수 Kotlin) | 없음 → **선투입 가능** |
| `PjLib.kt` | `.so` 로드, Endpoint 단일 부팅(`libCreate/Init/transportCreate/Start`), `libRegisterThread` 헬퍼 | 있음 |
| `SipController.kt` | 진입 클래스. config→pjsua2 매핑, register/call/hangup, StateFlow 노출 | 있음 |
| `CimsAccount.kt` | `Account` 서브클래스. `onRegState`/`onIncomingCall` | 있음 |
| `CimsCall.kt` | `Call` 서브클래스. `onCallState`/`onCallMediaState` + 미디어 브리지 | 있음 |

### 3.2 config → pjsua2 매핑 (핵심 — Digest 계약 포함)

> ### ⚠ 발견 1 — Digest username은 msisdn이 아니라 `IMSI@domain`
> 서버 소스(`csp/CscfModule.cpp`) 직독 결과, 서버는 Digest `username == IMSI@service.domain`을 정확히 비교하고 불일치 시 **즉시 403 Forbidden 단발 실패**(`:131-133` `strExpectedUser=imsi+"@"+svc.domain`, `:140` mismatch→`SIP_FORBIDDEN`)한다. 현재 `SipAccountConfig.effectiveAuthId`는 `authId`가 비면 msisdn으로 폴백하므로 그대로 두면 403으로 막힌다.
>
> → **M1.1 최우선 작업**: ConfigScreen에 IMSI 입력 필드를 신설하고 코드에서 `IMSI@domain`을 합성(또는 authId 전체 입력)하며 msisdn 폴백을 제거/경고한다. **AOR(공개ID)=`sip:msisdn@domain`** 과 **Digest username=`IMSI@domain`** 은 서로 다른 값임을 UI/문서에 분리 표기.

서버 강제 Digest 계약(소스 직독 정본):

| 항목 | 서버 강제값 | 근거 |
|---|---|---|
| Challenge / algorithm / qop | `Digest` / `MD5` / `auth` | `CscfModule.cpp:46-57` |
| realm | `EffectiveRealm` = auth_realm 있으면 그것, 없으면 service.domain | `AddChallenge`(`CscfModule.cpp:52-57`)·`EffectiveRealm`(`CspServiceMap.cpp:112`) |
| A1 | `username:realm:password` (RFC 2617) | `:78` |
| response(qop=auth) | `H(A1):nonce:nc:cnonce:qop:H(A2)` | `:87` |
| nonce | 서버 발행·1회성(`NonceMap.Select`) | `:106,:172` |
| **Digest username** | **`IMSI@service.domain`** | **`:130-144`** |
| service_ref(voip 바인딩) | 없으면 REGISTER 거부 | `:110` |

```kotlin
// PJSIP 통합 후 core 투입
private fun buildAccountConfig(c: SipAccountConfig): AccountConfig {
    val ac = AccountConfig()
    ac.idUri = if (c.displayName.isBlank()) c.aor                 // sip:msisdn@domain (공개ID)
               else "\"${c.displayName}\" <${c.aor}>"
    ac.regConfig.registrarUri = "sip:${c.domain}:${c.serverPort};transport=udp"
    ac.regConfig.timeoutSec   = c.expiresSec.toLong()            // 희망값(실제는 서버 응답 추종 — §3.6)
    ac.regConfig.registerOnAdd = true

    val cred = AuthCredInfo(
        /*scheme*/ "digest",
        /*realm */ c.domain,            // challenge realm과 일치시키거나 "*" 권장(§3.3)
        /*user  */ c.digestUsername,    // = IMSI@domain (NOT msisdn)
        /*type  */ 0,                   // PJSIP_CRED_DATA_PLAIN_PASSWD
        /*data  */ c.password,
    )
    ac.sipConfig.authCreds.add(cred)

    // 도메인 DNS 미해석 회피: 실제 서버 IP:port로 route 강제
    ac.sipConfig.proxies.add("sip:${c.serverHost}:${c.serverPort};transport=udp;lr")
    return ac
}
```

### 3.3 라우팅·realm 함정

- **proxies vs registrarUri**: 테스트 도메인(`ims.mnc033...`)은 공인 DNS 미해석 → registrarUri host를 도메인으로 두면 일부 빌드에서 registrarUri 자체 DNS 조회를 먼저 시도해 지연/실패할 수 있다. **`proxies`에 실제 IP:port(`;lr`) route를 강제**하고 idUri/registrarUri host는 규격상 domain 유지. M1.1 1차 시도에서 막히면 차선책으로 registrarUri/idUri host도 서버 IP로 두는 변형을 준비(verify-on-machine).
- **realm**(소스 재검증 2026-06-10): challenge realm = `EffectiveRealm`(auth_realm 우선, 없으면 service.domain — `CspServiceMap.cpp:112`)이고 **username 의 `@` 뒤 domain(=`svc.domain`)과는 별개 변수**다. 핵심: **서버는 클라이언트가 보낸 realm 을 검증하지 않고 그대로 A1=MD5(username:realm:password) 계산에 넣는다**(`CscfModule.cpp:150`). 따라서 서버측은 realm 값이 challenge 와 달라도 인증이 깨지지 않는다 — **무한 401 위험은 순수 PJSIP 클라이언트측 동작**(`AuthCredInfo.realm`이 challenge realm 과 불일치하면 PJSIP 가 그 credential 을 챌린지에 적용하지 않아 Authorization 미전송). 그러므로 `AuthCredInfo.realm = "*"`(challenge realm echo)가 **가장 견고**하다(도메인 하드코딩/오타 위험 제거).
- NAT 환경에서 rport(pjsip 기본 활성)·필요시 STUN 점검.

### 3.4 스레딩 / 객체 수명 규칙 (사고 최빈 영역)

- **단일 전용 스레드 직렬화**: 모든 pjsua2 호출을 `HandlerThread "pj-ctl"` 한 곳으로 직렬화하고, 부팅 직후 그 스레드에서 1회 `libRegisterThread`. UI/코루틴은 이 스레드로 post만. → 미등록 스레드 abort·동시성 둘 다 차단. 콜백 스레드는 PJSIP 스레드이므로 추가 등록 불필요(단 콜백 내 블로킹 금지).
- **GC 방지(필수)**: `Account`/`Call` Java 래퍼는 네이티브 콜백 동안 살아있어야 한다. `account`는 강참조, `Call`은 `ConcurrentHashMap<callId, CimsCall>` 강참조 보관 → `onCallState=DISCONNECTED`에서만 `remove()+delete()`. `AudDevManager`의 capture/playback media와 `getMedia()` 반환은 Endpoint 소유 → 보관 금지, 매 미디어 이벤트마다 재취득.

### 3.5 PjLib 부팅 (M1.0)

```kotlin
// PJSIP 통합 후 core 투입
object PjLib {
    @Volatile private var booted = false
    lateinit var ep: Endpoint; private set

    @Synchronized fun boot(logLevel: Int = 4) {
        if (booted) return
        System.loadLibrary("pjsua2")    // 실 .so 파일명 = libpjsua2.so 확인 후. SWIG static init이
                                        // 이미 로드하면 중복(무해). c++_shared 동봉이 선행 조건.
        ep = Endpoint()
        ep.libCreate()
        val epc = EpConfig().apply {
            uaConfig.userAgent = "CIMS-UE/M1 (pjsua2)"
            logConfig.level = logLevel.toLong()
        }
        ep.libInit(epc)
        ep.transportCreate(pjsip_transport_type_e.PJSIP_TRANSPORT_UDP,
                           TransportConfig().apply { port = 0 })
        ep.libStart()
        booted = true
    }

    fun ensureThread(name: String = Thread.currentThread().name) {
        if (!ep.libIsThreadRegistered()) ep.libRegisterThread(name)   // 메서드 부재 버전은 ThreadLocal 대체
    }

    @Synchronized fun shutdown() {
        if (!booted) return
        runCatching { ep.libDestroy() }; booted = false
    }
}
```

> **SWIG 시그니처 verify-on-machine.** `transportCreate(pjsip_transport_type_e, …)` / `libGetState():pjsua_state` / `libVersion().full` / `libIsThreadRegistered()` 는 SWIG 생성 버전에 따라 갈린다(enum 객체 직접 전달 불가 → `.swigValue()` 필요한 변형, getter 명 등). **core 투입된 실제 `org.pjsip.pjsua2/Endpoint.java` 시그니처를 grep으로 먼저 확인 후 스켈레톤 확정.** 안전 대안: enum이 안 먹으면 `PJSIP_TRANSPORT_UDP.swigValue()`, 버전은 명시적 `getFull()`.

### 3.6 SipController 스켈레톤

```kotlin
// PJSIP 통합 후 core 투입
package com.cims.ue.core.sip

class SipController(private val config: SipAccountConfig) {
    private val _reg = MutableStateFlow<RegState>(RegState.Idle)
    val regState: StateFlow<RegState> = _reg.asStateFlow()
    private val _call = MutableStateFlow<CallState>(CallState.Null)
    val callState: StateFlow<CallState> = _call.asStateFlow()

    private val ctl = HandlerThread("pj-ctl").apply { start() }
    private val h = Handler(ctl.looper)
    private var account: CimsAccount? = null                  // 강참조
    private val calls = ConcurrentHashMap<Int, CimsCall>()    // callId → Call 강참조
    @Volatile var videoEnabled = false                        // M1.3 토글

    private fun onCtl(block: () -> Unit) = h.post {
        PjLib.ensureThread("pj-ctl")
        runCatching(block).onFailure { _reg.value = RegState.Failed(it.message ?: "err") }
    }

    fun register() = onCtl {
        PjLib.boot()
        _reg.value = RegState.Registering
        val acc = CimsAccount(this)
        acc.create(buildAccountConfig(config))                // registerOnAdd=true → REGISTER 발신
        account = acc
    }
    fun unregister() = onCtl { account?.setRegistration(false) }   // de-REGISTER

    fun makeCall(dstNumber: String) = onCtl {                 // M1.2
        val acc = account ?: return@onCtl
        val call = CimsCall(this, acc)
        val prm = CallOpParam(true).apply {
            opt.audioCount = 1L                               // unsigned→long: Long 리터럴
            opt.videoCount = if (videoEnabled) 1L else 0L
        }
        call.makeCall("sip:$dstNumber@${config.domain}", prm)
        calls[call.id] = call
    }
    fun answer(callId: Int) = onCtl {
        calls[callId]?.answer(CallOpParam().apply { statusCode = pjsip_status_code.PJSIP_SC_OK })
    }
    fun reject(callId: Int) = onCtl {
        calls[callId]?.answer(CallOpParam().apply { statusCode = pjsip_status_code.PJSIP_SC_BUSY_HERE })
    }
    fun hangup(callId: Int) = onCtl { calls[callId]?.hangup(CallOpParam()) }

    // ── 콜백 진입점 ──
    internal fun dispatchReg(active: Boolean, code: Int, reason: String) {
        _reg.value = when {
            active && code in 200..299 -> RegState.Registered(code)
            !active                    -> RegState.Unregistered
            else                       -> RegState.Failed("$code $reason")
        }
    }
    internal fun dispatchIncoming(call: CimsCall, from: String) {
        calls[call.id] = call
        _call.value = CallState.Incoming(call.id, from)
    }
    internal fun dispatchCallState(callId: Int, s: CallState) {
        _call.value = s
        if (s is CallState.Disconnected) calls.remove(callId)?.delete()
    }

    fun shutdown() = onCtl {
        calls.values.forEach { runCatching { it.delete() } }; calls.clear()
        account?.let { runCatching { it.delete() } }; account = null
        PjLib.shutdown()
    }
}
```

### 3.7 CimsAccount / CimsCall (콜백 정합 교정 반영)

```kotlin
// PJSIP 통합 후 core 투입
class CimsAccount(private val owner: SipController) : Account() {
    override fun onRegState(prm: OnRegStateParam) {
        // ⚠ 교정: OnRegStateParam에는 .info 가 없다. regIsActive는 getInfo()(AccountInfo)에서.
        val ai = getInfo()                          // 콜백 스레드(PJSIP)에서 호출 → 안전
        owner.dispatchReg(ai.regIsActive, prm.code.swigValue(), prm.reason)  // code는 enum → Int
    }
    override fun onIncomingCall(prm: OnIncomingCallParam) {
        val call = CimsCall(owner, this, prm.callId)
        owner.dispatchIncoming(call, call.getInfo().remoteUri)
        call.answer(CallOpParam().apply { statusCode = pjsip_status_code.PJSIP_SC_RINGING }) // 180만 자동
    }
}

class CimsCall : Call {
    private val owner: SipController
    constructor(o: SipController, acc: Account) : super(acc) { owner = o }
    constructor(o: SipController, acc: Account, cid: Int) : super(acc, cid) { owner = o }

    override fun onCallState(prm: OnCallStateParam) {
        val ci = info
        val mapped = when (ci.state) {
            pjsip_inv_state.PJSIP_INV_STATE_CALLING,
            pjsip_inv_state.PJSIP_INV_STATE_EARLY     -> CallState.Outgoing(id, ci.remoteUri)
            pjsip_inv_state.PJSIP_INV_STATE_CONNECTING,
            pjsip_inv_state.PJSIP_INV_STATE_CONFIRMED -> CallState.Active(id, ci.remoteUri)
            pjsip_inv_state.PJSIP_INV_STATE_DISCONNECTED ->
                CallState.Disconnected(id, ci.lastStatusCode.swigValue(), ci.lastReason)
            else -> return
        }
        owner.dispatchCallState(id, mapped)
    }

    override fun onCallMediaState(prm: OnCallMediaStateParam) {
        val ci = info
        for (i in 0 until ci.media.size.toInt()) {
            val m = ci.media[i]
            if (m.type == pjmedia_type.PJMEDIA_TYPE_AUDIO &&
                m.status == pjsua_call_media_status.PJSUA_CALL_MEDIA_ACTIVE) {
                val aud = AudioMedia.typecastFromMedia(getMedia(i.toLong()))
                val adm = PjLib.ep.audDevManager()
                adm.captureDevMedia.startTransmit(aud)   // mic → 통화
                aud.startTransmit(adm.playbackDevMedia)  // 통화 → spk
            }
            // VIDEO(M1.3): VideoWindow → Surface attach (별도 설계)
        }
    }
}
```

콜백 정합 교정 요약: (1) `onRegState`는 `getInfo().regIsActive` 사용(`OnRegStateParam.info`는 없음). (2) enum→Int 필요 지점은 `.swigValue()` 일관 적용. (3) `CallSetting.audioCount/videoCount`는 unsigned→long 매핑이므로 `1L`/`0L`.

### 3.8 UI 모델 + HomeScreen 연결

```kotlin
// SipModels.kt — pjsua2 비참조, 선투입 가능
sealed interface RegState {
    data object Idle : RegState; data object Registering : RegState
    data class Registered(val code: Int) : RegState
    data object Unregistered : RegState; data class Failed(val reason: String) : RegState
}
sealed interface CallState {
    data object Null : CallState
    data class Outgoing(val id: Int, val remote: String) : CallState
    data class Incoming(val id: Int, val remote: String) : CallState
    data class Active(val id: Int, val remote: String) : CallState
    data class Disconnected(val id: Int, val code: Int, val reason: String) : CallState
}
```

`MainActivity` 비활성 버튼 교체(요지): `remember { SipController(config) }` → `regState.collectAsState()`로 버튼/상태 텍스트 구동, `DisposableEffect { onDispose { controller.shutdown() } }`. RECORD_AUDIO/(M1.3)CAMERA 런타임 권한은 `register()`/`makeCall()` 전 요청, INTERNET은 manifest.

---

## 4. AMR-WB 코덱 통합 + H.264 영상

### 4.1 미디어 경로: 3개 후보로 재정의

원안의 2개(A/B) 비교는 PJSIP의 **제3 경로(C)** 를 누락했다. PJSIP는 MediaCodec을 "내부 코덱"으로 직접 구동하는 빌트인 코덱군(`PJMEDIA_HAS_AND_MEDIA_{AMRNB,AMRWB,H264}`, 모두 기본 1)을 제공한다 — HW 코덱을 쓰면서 **RTP 페이로딩·지터버퍼·페이로더는 PJSIP가 처리**(자체 FU-A 페이로더 신규 구현 불필요).

| 경로 | 코덱 구동 | RTP 페이로딩 | 평가 |
|---|---|---|---|
| **A. PJSIP 내장 SW** | opencore-amr(음성)/openh264(영상) | PJSIP | 음성 최단경로·서버와 동일 라이브러리(동등성). 영상 openh264는 §4.5 제약 |
| **C. PJSIP And-Media** | MediaCodec을 PJSIP 내부 코덱으로 | PJSIP | HW 가속 + PJSIP가 FU-A(mode1) 페이로딩 처리. **영상 1순위** |
| **B. 완전 커스텀** | MediaCodec + 자체 RFC6184 페이로더를 custom pjmedia transport에 결선 | 직접 구현 | 동기/비동기 정합·octet-align 재패킹 부담 → **최후 수단** |

**M1 확정 (2026-06-10, 팀 결정 — 구 "음성=A 권장"을 반전):**
- **음성(M1.2) = 경로 C(And-Media AMR-WB).** 근거: ① **헤드라인 결정 정합** — "OEM MediaCodec 사용 → AMR 특허 노출 완화"(android_ue_client.md §11-12). 경로 A는 GPL APK에 AMR 코덱 소스를 번들해 특허 노출을 재유입. ② **2.16 태그에 `and_aud_mediacodec.cpp` 실존·AMR-WB 완전 등록** 1차 확인(2026-06-10): 코덱 테이블 `{"AMR-WB","audio/amr-wb",…,16000,1,…}`(`:340`), 기본 fmtp `octet-align=1`(`:346`), 인코더 `OMX.google.amrwb.encoder`/`c2.android.amrwb.encoder`(`:220-224`). 표준 codec manager factory 등록이라 pjsua2 `codecEnum2`/`codecSetPriority`/`codecGet·SetParam` 그대로 동작. ③ 타깃 UNIWA 실기기 AMR-WB ENC+DEC 가용 + 루프백 실시간성은 **M0 게이트로 확인 완료**. ④ §2.3 외부 라이브러리 선빌드 단계 통째 삭제 → M1.0 대폭 단순화.
- **영상(M1.3) = 경로 C(And-Media H264).** HW 가속 + PJSIP가 FU-A 패킹 처리 → §4.5의 openh264 single-NAL 제약을 자연 회피. 음성·영상이 단일 And-Media 경로로 통일.
- **경로 A(opencore)는 폴백으로 강등.** 구 권장의 핵심 근거였던 "서버와 동일 라이브러리 = 비트스트림 동등성"은 경로 C에서 상실되므로, **단말 MediaCodec ↔ 서버 opencore/vo-amrwbenc 상호운용을 M1.2 실호 캡처로 검증하는 것이 GO의 전제 게이트**(§5.3). 게이트 실패 시에만 A 재검토(특허 재검토 동반).

> 경로 C 강제를 위해 §2.5대로 `PJMEDIA_HAS_OPENCORE_AMR{WB,NB}_CODEC`을 0으로 빌드 제외해 `'AMR-WB/16000/1'` 단일 등록을 보장한다(중복 등록 시 코덱 선택 비결정). M1.0에서 `codecEnum2()`로 AMR-WB 단일 등록 선확인.

### 4.2 권장 미디어 파이프라인 (M1)

```
                 ┌──────────────────────── PJSUA2 / pjmedia ────────────────────────┐
 마이크 →(AudDev)→ AEC/지터버퍼 →[And-Media AMR-WB ENC(MediaCodec)]→ RTP(pt=99, octet-align=1)→ CSP/CMP relay
 스피커 ←(AudDev)← 지터버퍼 ←[And-Media AMR-WB DEC(MediaCodec)]← RTP ←──────────────────┘
                 │
 카메라 →(VidDev)→ [And-Media H264 ENC(HW)] → PJSIP RTP(FU-A) → relay
 화면   ←(VidDev)← [And-Media H264 DEC(HW)] ← PJSIP RTP
                 └────────────────────────────────────────────────────────────────┘
```
오디오 디바이스는 §2.6대로 M1.2 전 Oboe(AAudio) 권장.

### 4.3 코덱 우선순위/파라미터 (core 통합 후)

```java
// PJSIP 통합 후 core 투입
Endpoint ep = Endpoint.instance();

// 음성: AMR-WB 최우선, 나머지 0. ※ codecId 문자열은 하드코딩 금지 —
//   codecEnum2() 실제 출력값으로 확인 후 사용(And-Media 테이블은 16000/1 — 실표기 verify-on-machine).
ep.codecSetPriority("<AMR-WB codecId>", (short)254);
ep.codecSetPriority("PCMU/8000/1", (short)0);
ep.codecSetPriority("PCMA/8000/1", (short)0);

// AMR-WB fmtp: octet-align=1, mode-set=0,1,2 — enc_fmtp + dec_fmtp 양쪽에 주입
CodecParam cp = ep.codecGetParam("<AMR-WB codecId>");
// cp.setting.dec_fmtp / enc_fmtp 에 octet-align=1; mode-set=0,1,2
// cp.payload type 필드를 99로 set (pjmedia_codec_mgr_set_default_param 등가)
ep.codecSetParam("<AMR-WB codecId>", cp);

// 영상: H.264 최우선
ep.videoCodecSetPriority("H264/97", (short)254);
VidCodecParam vp = ep.getVideoCodecParam("H264/97");
// profile-level-id / packetization-mode 등 서버 협상값 정합
ep.setVideoCodecParam("H264/97", vp);
```

### 4.4 AMR-WB SDP/fmtp 정합 규칙 (교정 반영)

| 파라미터 | 서버 정본 | 처리 위치 | 비고 |
|---|---|---|---|
| payload type | 99 | CodecParam payload type 필드 set(**SDP 재기록 아님**) | `PJMEDIA_RTP_PT_AMRWB` 실수치 verify-on-machine. 서버가 단말 offer의 동적 pt를 수용하면 99 고정이 must는 아님 — 서버 강제 여부 선확인 |
| clock/ch | 16000/1 | 코덱 등록 고정 | |
| **octet-align** | **1** | enc_fmtp + dec_fmtp 양쪽 명시 주입 | And-Media 코덱 테이블 기본 fmtp가 `octet-align=1`(2.16 `:346`)이라 기본 정합 — 그래도 **양쪽 명시 주입 + 와이어샷 게이트 유지**(협상 경로가 수신 SDP 값을 enc/dec 양쪽에 그대로 적용하므로 상대 미광고/0 광고 시 흐트러질 수 있음) |
| mode-set | 0,1,2 | fmtp 주입 | **opencore 인코더는 advertised mode-set 집합 내 최근접 모드 선택** → 양측이 0,1,2 광고하면 인코더 출력도 0,1,2로 수렴(별도 enc 클램프 코드 불필요). 서버가 mode-set 미광고 시 fallback만 verify |
| ptime | 20 | 기본 20ms | 1프레임/패킷 |

> **octet-align 불일치(0 vs 1)는 프레임 바운더리 오해석으로 즉시 음성 깨짐.** M1.2 첫 캡처에서 양 leg `a=fmtp:99 octet-align=1` 확인을 필수 게이트로.

### 4.5 H.264 송신 제약 (확정 사실) → 경로 C로 회피

> ### ⚠ 발견 3 — PJSIP openh264(SW) TX는 single-NAL(packetization-mode 0) 고정
> PJSIP openh264 송신은 remote fmtp와 무관하게 **single NAL(mode 0)** 만 사용하도록 고정되어 있다(interop 목적, 확정 사실). 서버/상대가 mode 1(FU-A)을 요구하면 큰 프레임에서 MTU 초과/단편화 문제. **완화책은 커스텀 페이로더(B)가 아니라 경로 C(And-Media H264) 채택** — PJSIP And-Media 경로가 FU-A 페이로딩을 처리한다. 단 발신 SDP의 `packetization-mode` 협상값과 실제 송신 패킹 정합은 와이어샷 게이트 유지. (And-Media 영상 활성화 절차/매크로는 채택 태그 소스에서 확인 — verify-on-machine.)

### 4.6 영상 인코더 SW/HW 기본값 주의

PJSIP에는 `PJMEDIA_AND_MEDIA_PRIO_SW_VID_ENC`(기본 1)가 있어 **기본은 HW 디코드 + SW 비디오 인코드 경향**(안정성 목적). "HW 인코딩을 쓴다"는 전제가 PJSIP 기본 동작과 어긋날 수 있으므로, HW 인코딩을 실제로 원하면 `PJMEDIA_AND_MEDIA_PRIO_SW_VID_ENC 0`을 명시하거나 기본(SW 인코드)을 수용하고 측정 후 전환. OpenH264 폴백은 "동시 빌드 후 우선순위"가 아니라 **단말 매트릭스별 빌드 분기 또는 런타임 우선순위 정밀 제어**로 다룬다(동시 빌드 시 H264 factory 중복 등록 충돌 가능 — 검증 항목).

### 4.7 폴백 옵션 B 개략 (M2+ 측정 입증 시에만)

핵심 난점 = MediaCodec 비동기/버퍼풀 모델을 PJSIP 동기 20ms `encode/decode` 콜백에 맞추기. 전략: ① 동기 폴링(`AMediaCodec_dequeue*`를 유한 timeout 0~5ms) ② 워밍업 지연 보상(초기 N프레임 0바이트 반환) ③ 1프레임/패킷 강제. octet-align 재패킹(TOC/FT)을 직접 구현해야 하고 벤더 HW 비트스트림 동등성 리스크가 있어 **음성에서 ROI 거의 없음**. factory op(5)+codec op(8) 콜백 구현 + `pjmedia_codec_mgr_register_factory` 등록.

---

## 5. M1.0~M1.3 시퀀스·테스트

### 5.0 네트워크 도달성 점검 (코딩 전 선행)

여기서 막히면 SIP 코드가 멀쩡해도 안 붙는다. **PC(WSL2)에서 sipp/sipsak로 OPTIONS·REGISTER·Digest를 먼저 검증**해 "서버 문제 vs 단말코드 문제"를 분리한 뒤 단말 코드로 넘어간다.

| 점검 | 방법 | 합격 |
|---|---|---|
| SIP UDP 포트 개방 | PC에서 `sipsak`/`sipp`로 OPTIONS → 응답 | 200/405/401 수신 = UDP 왕복 OK |
| 전송 프로토콜 확정 | csp.json listener 확인 + UDP 실측 | **"UDP 가정"을 실측 확정**(versionSensitive). TCP 25061/TLS 5061로 전환 가능(`transport` 지원) |
| service_ref | 운영팀에 IMSI+voip 바인딩+password 사전 등록 | 미비 시 인증과 무관하게 거부(`CscfModule.cpp:110`) |
| NAT/방화벽 | outbound UDP 허용, rport 응답 비대칭 | 양측 동시 Wireshark로 패킷 도달 분리 진단 |

### 5.1 M1.0 — 부팅 스모크

산출물: §2 전체(.so + libc++_shared.so + SWIG Java + config_site.h + `PjLib`/부팅 스모크 버튼).
게이트: 앱이 Endpoint 생성→UDP transport 바인드→`libStart()`→`destroy()`까지 크래시/`UnsatisfiedLinkError` 없이 통과. logcat에 PJSIP 버전 배너 + `SIP UDP transport started`. **+ `codecEnum2()` 출력에 AMR-WB 항목 존재**.
통과 기준: 콜드스타트 10회 연속 무크래시. `.so` ABI=arm64-v8a(`readelf`). minSdk 26 일관(코덱 경로 실질 API는 verify-on-machine).

### 5.2 M1.1 — REGISTER

```
UI        SipController(pj-ctl)     pjsua2/Account          CSP(15060/UDP)
 │ tap ──▶ register()
 │        PjLib.boot()(최초)
 │        _reg=Registering; acc.create(cfg)
 │                    │ REGISTER (no auth) ──────────────────────▶
 │                    │ ◀──── 401 (WWW-Auth: Digest realm,nonce,qop=auth,MD5)
 │                    │ REGISTER (Authorization: username=IMSI@domain, ▶
 │                    │   response=MD5(...), qop=auth, nc, cnonce)
 │                    │ ◀──── 200 OK (Expires: 3600 — 서버 하드코딩)
 │ ◀ Registered(200)  onRegState→getInfo().regIsActive→dispatchReg
```

게이트: `401 → REGISTER(Authorization) → 200 OK`.
통과 기준:
- onRegState `code==200`. 와이어샷 2nd REGISTER `response=`가 서버 계산식 일치(불일치=403, username=IMSI@domain 확인).
- **재등록 주기는 내가 보낸 `expiresSec`가 아니라 서버 200 OK의 `Expires`(서버는 3600 하드코딩, `CscfModule.cpp:274`)를 추종**해 ~50~90% 시점에 자동 재등록. `regConfig.timeoutSec`는 희망값일 뿐 PJSIP가 응답 expires를 자동 추종. **너무 짧은 expires는 423 Interval Too Brief + Min-Expires** 가능(`:220-227`) — 시나리오에 포함.
- un-REGISTER(expires=0) 200 확인.

### 5.3 M1.2 — 1:1 AMR-WB 음성

```
발신 UI   SipController       CimsCall          CSP(B2BUA)          착신
 │ makeCall("1002")
 │        call.makeCall(sip:1002@domain, audio=1)
 │                   │ INVITE (SDP: pt99 AMR-WB octet-align=1; mode-set=0,1,2; 16000/1) ▶
 │                   │                              ▶ INVITE(B-leg) ▶
 │                   │ ◀ 100 Trying (서버 자동)
 │                   │ ◀ 180 Ringing ◀────────────────────── (착신에서 전달, 서버 자동 아님)
 │ ◀ Outgoing(EARLY)
 │                   │ ◀ 200 OK (SDP answer, c=/m=포트=CMP relay) ◀── 착신 answer 200
 │                   │ ACK ▶
 │ ◀ Active(CONFIRMED)  onCallMediaState→ mic↔통화↔spk startTransmit
 │ ◀════════ RTP(AMR-WB pt99) via CMP relay ════════▶
 │ hangup() ──▶ BYE ▶ ──────────────────────────────▶ BYE(B-leg)
 │ ◀ Disconnected  → calls.remove+delete()
```

단말 측 유의: **RTP는 항상 200 OK SDP의 c=/m= 포트(=CMP relay)로 흐른다**(상대 단말 주소 아님). 180은 착신측 전달(서버는 100만 자동) → 즉시응답 시 180 없이 200 도착 가능. INVITE To/요청URI user=상대 msisdn(공개ID), 인증 username=IMSI@domain.

게이트: INVITE→100/180/200→ACK→양방향 AMR-WB RTP→BYE/200.
통과 기준: 연결률(20회) ≥ 95%. 양방향 가청, 와이어샷 **pt=99 / octet-align=1 / mode-set** 정합. 무음/단방향 0건. mouth-to-ear < 250ms(1차). BYE 후 RTP 즉시 정지 + CSP flow 로그 정상 teardown. `pj-ctl` 외 스레드 ep.* 호출 시 abort 부재. 통화 100회 반복 후 `calls` 맵 0(누수 없음).

### 5.4 M1.3 — H.264 영상

산출물: 음성+영상 SDP(`m=video` + `a=rtpmap:<pt> H264/90000` + fmtp `profile-level-id`/`packetization-mode`), 영상 경로 C(And-Media H264 ENC/DEC) ↔ PJSIP RTP 결선, Camera2/CameraX 캡처 + Surface 프리뷰/원격 렌더, 카메라 권한.
게이트: 음성 성립 상태에서 m=video 협상 성공 → 양방향 H.264 RTP → 디코드 렌더.
통과 기준: 양방향 영상 표시. IDR 요청/주기 동작. packetization-mode 정합(와이어샷 SDP fmtp + 실제 FU-A 패킹). 음성 동기 유지. CPU/발열 허용 범위. (HW 인코더 채택 시 §4.6 SW/HW 기본값 확인.)

### 5.5 3중 관측 디버깅

| 레이어 | 도구 | 본다 |
|---|---|---|
| 단말 SIP | PJSIP log level 4~5 + logcat | REGISTER/INVITE 원문, 계산 `response=`, 응답코드, 코덱협상 |
| 서버 CSP flow | `{ServiceLogDir}/…/{systemId}.flow.{mm5}.jsonl` + `*.msg.{mm5}.jsonl`(iface=sip), Call-ID→sesid 양 leg | 401 사유(nonce vs username mismatch), 403 지점(`username mismatch (got/expected)`), CMP relay add/remove |
| Wireshark | 단말(WiFi/tcpdump)+서버 동시, `udp.port==15060 || sip || rtp` | SDP pt/fmtp/octet-align, RTP 방향·CMP 주소 도달, 180/200 누락 |

증상→1차 의심:

| 증상 | 1차 의심 | 확인 |
|---|---|---|
| **403 Forbidden(단발)** | **Digest username ≠ IMSI@domain**(발견1) or service_ref 없음 | CSP `username mismatch (got/expected)`. *무한 401과 다른 즉시 단발 실패* |
| 401 무한 반복 | nonce 만료/재사용, qop/nc/cnonce 누락, realm 불일치 | CSP `nonce not found`, Authorization 헤더 |
| REGISTER 무응답 | UDP 도달성/방화벽/포트오류 | §5.0, 와이어샷 패킷 유무 |
| 단방향/무음 | SDP 주소가 상대단말(CMP 아님) / octet-align 불일치 | 와이어샷 SDP c=/fmtp, RTP 방향 |
| 200 후 RTP 없음 | ACK 누락 / 마이크 권한 / conf-bridge 미결선 | 콜백, 와이어샷 ACK·RTP |
| 영상만 실패 | H.264 packetization-mode/profile 불일치(발견3) | 와이어샷 SDP fmtp, FU-A 단편화 |

---

## 6. 위험 · 버전확인(verify-on-machine) · Open Questions

### 6.1 위험 요약

| # | 위험 | 영향 | 완화 | conf. |
|---|---|---|---|---|
| 1 | Digest username=IMSI@domain 미반영(msisdn 폴백) | M1.1 즉시 403 단발 차단 | 계정모델 정리 최우선, AOR/username 분리, sipp 사전검증 | high |
| 2 | `libc++_shared.so` 미동봉 | 앱 실행 즉시 UnsatisfiedLinkError(100%) | §2.7 필수 동봉 | high |
| 3 | AMR-WB octet-align 불일치(0 vs 1) | M1.2 음성 깨짐/무음 | And-Media 기본 fmtp=octet-align=1(2.16 `:346`) + enc+dec_fmtp 양쪽 강제, 와이어샷 게이트 | medium |
| 4 | `'AMR-WB/16000/1'` 중복 등록(opencore+And-Media 동시 빌드) | 코덱 선택 비결정 | 경로 C 확정 — OPENCORE_AMR{WB,NB}_CODEC=0 빌드 제외, codecEnum2 단일 등록 확인 | high |
| 5 | PJSIP openh264 TX single-NAL 고정 | mode1 요구 시 영상 단편화/실패 | 영상=경로 C(And-Media), SDP packetization-mode 와이어샷 게이트 | high |
| 6 | UDP 방화벽/NAT/symmetric NAT | REGISTER/INVITE 무응답 | §5.0 도달성 선행, rport/keepalive, WiFi 우선 | medium |
| 7 | SWIG 시그니처/enum 버전차(transportCreate/libGetState/onRegState info) | 컴파일 깨짐 | core 투입 .java grep 선확인, `.swigValue()` 대안 | medium |
| 8 | 영상 SW/HW 인코더 기본값(PRIO_SW_VID_ENC=1) | HW 가속 미적용 | 명시 토글 또는 SW 수용 후 측정 전환 | medium |
| 9 | service_ref(voip 바인딩) 미등록 | 인증과 무관하게 REGISTER 거부 | 운영팀 사전 등록, CSP 로그 확인 | high |
| 10 | minSdk 26이나 코덱 경로 실질 API28+ | 26~27 기기 코덱 미동작 | APP_PLATFORM=28 빌드 + SDK_INT 가드 / 28+ 타깃 결정 | high |
| 11 | AMR 특허(GPL과 별개) | 법무 리스크 | 서버가 동일 라이브러리 채택 = 동일 리스크 프로파일. 법무 별도 에스컬레이션 | low |
| 12 | 16KB page 미정렬 .so | Android 15+ 기기 로드 실패 | NDK r28 + useLegacyPackaging=false, 실기기 로드 확인 | medium |
| 13 | 단말 MediaCodec ↔ 서버 opencore/vo-amrwbenc 비트스트림 미세 비호환(경로 C로 동등성 근거 상실) | M1.2 음성 품질/무음 | **M1.2 실호 상호운용 캡처 = GO 전제 게이트**(추정 통과 금지). 실패 시 경로 A 폴백 | high |
| 14 | And-Media 오디오 인코더 미가용 시 폴백 없음(`create_codec` 실패 후 로그만) | 해당 기기 음성 불가 | 타깃 UNIWA 는 M0 게이트로 해소. 기기 매트릭스 확대 시 기기별 `c2.android.amrwb.encoder` 가용성 재확인 | medium |
| 15 | mode-set 클램프 미검증 — M0 스파이크 bitRate 23850(mode 8) vs 운영 SDP mode-set(0,1,2) | 디코더가 미광고 mode 프레임 거부 가능 | M1.2 와이어샷으로 인코더 출력 mode 가 협상 mode-set 내로 수렴하는지 실측(Open Q #5) | medium |

### 6.2 verify-on-machine (버전 의존 — 머신 실측 필요)

- **PJSIP 2.16 빌드 동작/매크로**: ~~And-Media 오디오 2.16 실존~~ → **원격 소스로 확인 완료**(2026-06-10, `and_aud_mediacodec.cpp` + `PJMEDIA_HAS_AND_MEDIA_AMRWB` 기본 1). 잔여: `git checkout 2.16` 후 §2.5 config_site.h 조합(AND_MEDIA_AMRWB=1 + OPENCORE=0)이 실제 빌드에 반영되는지 configure/빌드 로그 확인.
- **NDK 정확 버전 문자열**(`28.0.12916984`)과 16KB page-size 플래그 필요 여부: `sdkmanager --list`.
- **단말 MediaCodec ↔ 서버 opencore/vo-amrwbenc AMR-WB 상호운용**(위험 #13): M1.2 실호 양방향 가청 + mode-set 협상 수렴(M0 스파이크 23850=mode 8 과 운영 mode-set 정렬, 위험 #15) 실측.
- **AMR-WB codecId 문자열**(`AMR-WB/16000/1` 기대)과 **`PJMEDIA_RTP_PT_AMRWB` 실수치**: `codecEnum2()` 출력 + `types.h`.
- **AMR-WB octet-align/mode-set/pt=99 협상**이 실서버와 일치하는지: 실 SDP OFFER/ANSWER 캡처(log level≥5).
- **And-Media 음성/영상 코덱이 SWIG Java(`Endpoint.java`) 레벨에서 실제 열거·제어되는지**: C++ codec manager 등록은 확인 — Java 바인딩 실배열 포함은 core 투입 후 실호출로 확인.
- **SWIG Endpoint/Account/Call 시그니처**(`transportCreate`/`libGetState`/`libVersion`/`libIsThreadRegistered`/`OnRegStateParam`/`AccountInfo.regIsActive`/`CallSetting.*Count` long 매핑): core 투입 `.java` grep.
- **오디오 백엔드**: `--with-oboe` 미사용 시 기본 JNI sound device 동작, M1.2 전 Oboe 추가.
- **AGP 9.2.1 / compileSdk 37**에서 `packaging.jniLibs.useLegacyPackaging` DSL + 별도 소스셋 prebuilt .so/SWIG Java 배치 정합: Gradle sync.
- **CSP 15060 실제 SIP transport(UDP)**: REGISTER 전 sipp/sipsak 실측.
- **libpjsua2.so 동반 .so**(`libc++_shared.so` 등): SWIG 빌드 산출 디렉터리 확인.

### 6.3 Open Questions

1. ~~미디어 경로 최종 확정~~ → **확정(2026-06-10, 팀 결정)**: 음성·영상 모두 **경로 C(And-Media/MediaCodec)**. 2.16 태그 `and_aud_mediacodec.cpp` 실존·AMR-WB 등록 1차 확인 + 헤드라인 특허 완화 정합 + M0 게이트(UNIWA ENC+DEC). 경로 A(opencore)는 M1.2 상호운용 게이트(위험 #13) 실패 시 폴백.
2. **CSP 15060 전송 프로토콜**(UDP/TCP/TLS) 실측 확정 — 현재 "가정".
3. **테스트 계정의 IMSI·service_ref(voip 바인딩)·password**가 서버 DB에 사전 등록되어 있는가? 없으면 운영팀 선행.
4. **AMR-WB pt=99를 서버가 강제하는지** vs 단말 offer의 동적 pt를 수용하는지 — 서버 SDP 템플릿으로 재확인.
5. **mode-set=0,1,2가 인코더 모드까지 강제 수렴**하는지(서버가 mode-set 미광고 시 fallback) + M0 스파이크의 bitRate 23850(mode 8)과의 정합 — 운영 SDP mode-set 범위 내로 정렬.
6. **H.264 협상 정본**(profile-level-id / 해상도 / 비트레이트 / IDR 주기 / packetization-mode) 정의 위치 — M1.3 진입 전 확보.
7. **minSdk 26 기기(API 26~27) 코덱 폴백 정책** — 28+ 타깃 제한 vs G.711/SW 폴백.
8. **M2+ 음성 HW AMR-WB(옵션 B) 필요성 측정 기준**(임계 CPU%/배터리)을 누가 정의할지.