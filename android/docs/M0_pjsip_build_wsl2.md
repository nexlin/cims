# M0 — PJSIP 안드로이드 빌드 가이드 (WSL2)

**대상:** `android/core` 에 넣을 PJSIP(pjsua2) 네이티브 라이브러리 + SWIG Java 바인딩 빌드.
**호스트:** Windows + **WSL2(Ubuntu)**.
**시점:** AMR-WB MediaCodec 스파이크(가용성/지연) 확인 후 진행. (스파이크는 PJSIP 불필요)

> 버전 숫자는 환경/최신 안정판에 맞게 조정한다. 핵심 절차만 고정.

---

## 0. 방침 (코덱)

코덱은 **안드로이드 MediaCodec**(AMR-WB 음성 / H.264 영상)을 쓰므로, PJSIP 내장 코덱은 최소화한다.
- 음성 AMR-WB: PJSIP 내장 opencore-amr **비활성** → 추후 **커스텀 `pjmedia_codec_factory`**(MediaCodec 연동)로 등록.
- 영상 H.264: PJSIP의 Android MediaCodec 경로(`PJMEDIA_HAS_ANDROID_MEDIACODEC`) 사용.

---

## 1. WSL2 / 빌드 도구

```bash
# WSL2 Ubuntu 셸에서
sudo apt-get update
sudo apt-get install -y build-essential git python3 swig unzip wget openjdk-17-jdk
```

## 2. Android SDK / NDK

Android Studio가 설치한 NDK를 WSL에서 직접 쓰거나(권장: 경로 마운트), WSL에 cmdline-tools로 설치한다.

```bash
# 예: cmdline-tools 로 NDK 설치
export ANDROID_SDK_ROOT=$HOME/android-sdk
# (sdkmanager 설치 후)
sdkmanager "platforms;android-35" "ndk;26.3.11579264" "platform-tools"
export ANDROID_NDK_ROOT=$ANDROID_SDK_ROOT/ndk/26.3.11579264
```

> NDK는 r26/r27 계열 권장. PJSIP 버전과의 호환은 릴리스 노트 확인.

## 3. PJSIP 소스

```bash
git clone https://github.com/pjsip/pjproject.git
cd pjproject
git checkout 2.15   # 최신 안정 태그 사용 (예시)
```

## 4. `config_site.h`

`pjlib/include/pj/config_site.h` 생성:

```c
#define PJ_CONFIG_ANDROID 1
#include <pj/config_site_sample.h>

/* 코덱: MediaCodec 사용 → PJSIP 내장 음성 코덱 최소화 */
#define PJMEDIA_HAS_OPENCORE_AMR        0   /* AMR 은 MediaCodec 커스텀 팩토리로 */
#define PJMEDIA_HAS_G7221_CODEC         0
#define PJMEDIA_HAS_ILBC_CODEC          0
#define PJMEDIA_HAS_SPEEX_CODEC         0   /* AEC용 speex 가 필요하면 유지 검토 */

/* 영상: Android MediaCodec H.264 */
#define PJMEDIA_HAS_VIDEO               1
#define PJMEDIA_HAS_ANDROID_MEDIACODEC  1

/* SRTP/TLS 는 M4(TLS) 전까지 최소화 가능 */
```

> AEC(에코제거)를 PJSIP 쪽에서 쓸지(speex/webrtc) 단말 플랫폼 AEC를 쓸지는 M1에서 결정 → 그에 맞춰 토글.

## 5. 네이티브 빌드 (ABI: arm64-v8a)

```bash
cd pjproject
export ANDROID_NDK_ROOT=$ANDROID_NDK_ROOT
export TARGET_ABI=arm64-v8a
./configure-android --use-ndk-cflags
make dep && make clean && make
```

> 다른 ABI(`armeabi-v7a`)가 필요하면 `TARGET_ABI` 바꿔 재빌드.

## 6. SWIG Java 바인딩 (pjsua2)

```bash
cd pjsip-apps/src/swig
make
# 산출물:
#  - java/android/.../libpjsua2.so
#  - java/org/pjsip/pjsua2/*.java
```

## 7. `android/core` 에 배치

```
android/core/src/main/jniLibs/arm64-v8a/libpjsua2.so      ← 네이티브
android/core/src/main/java/org/pjsip/pjsua2/*.java         ← SWIG Java
```

(여러 ABI면 `jniLibs/<abi>/` 각각.)

## 8. 검증

`android/core` 빌드 후, 간단한 Kotlin 호출로 PJSIP 초기화가 되는지 확인:

```kotlin
import org.pjsip.pjsua2.*
val ep = Endpoint()
ep.libCreate()
val cfg = EpConfig()
ep.libInit(cfg)
// ... TransportConfig(UDP 5060) 생성 → libStart()
```

> 비-PJSIP 스레드에서 호출 시 `Endpoint.libRegisterThread("name")` 선행 필수.

---

## 다음 (M1 연동 포인트)

- `core` 의 `SipController` 가 pjsua2 `Account`/`Call` 로 REGISTER/INVITE 수행.
- **커스텀 AMR-WB `pjmedia_codec_factory`** 등록(JNI에서 MediaCodec 호출) → SDP fmtp `octet-align=1; mode-set=0,1,2` 정합.
- `m=application` (MCPTT floor) SDP 주입/파싱은 PTT(M2). VoLTE(M1)에는 불필요.
- 전체 mouth-to-ear 지연 측정(M1).
