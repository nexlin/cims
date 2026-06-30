# M1.0 — PJSIP 안드로이드 빌드 가이드 (Ubuntu)

**대상:** `android/core` 에 투입할 PJSIP(pjsua2) 네이티브 라이브러리 + SWIG Java 바인딩 빌드.
**정본 설계:** [docs/design/features/android_ue_m1_pjsip_integration.md](../../docs/design/features/android_ue_m1_pjsip_integration.md) §2 (빌드 플레이북) — 본 문서는 그 실행 절차 + **이 프로젝트 실측 상태**(2026-06-10).
**코덱 방침:** 음성·영상 모두 **경로 C(And-Media/MediaCodec)** 확정 — opencore-amr/vo-amrwbenc **선빌드 불필요**(설계서 §2.3/§4.1, 2026-06-10 결정).

> 구판 `M0_pjsip_build_wsl2.md`(WSL2 전제 + 커스텀 코덱팩토리 계획)는 본 문서로 대체. 구판은 git 이력 참조.

---

## 0. 환경 — 요구사항과 실측

**요구:** Ubuntu 24.04 x86_64 (WSL2 / VM / 네이티브 무관), 디스크 ~10GB, 인터넷.
빌드는 반드시 **리눅스 ext4(`~/`)** 안에서 수행 — NTFS(`/mnt/c` 등) 직접 빌드 금지(심볼릭링크/권한/속도). 산출물만 마지막에 Windows `android/core` 로 복사.

**이 프로젝트 실측 (2026-06-10):**

| 항목 | 실측 결과 |
|---|---|
| 개발 PC(Windows 11) WSL | **미설치** (`wsl --install` 필요 — 관리자 권한 + 재부팅) |
| 채택 대안 | VMware VM **nex-ubuntu** = `C:\work\vms\Ubuntu 64-bit.vmx` (Ubuntu 24.04.4, 8코어/16GB, ssh config `nex-ubuntu` → 192.168.199.129) |
| VM sudo | 비밀번호 필요 → **루트 불필요(userland) 프로비저닝**으로 우회(§1) |
| 프로비저닝 상태 | **완료** — JDK 17.0.19 / SWIG 4.2.0 / NDK `28.2.13676358` / `~/.m1env` |
| 후속 방침 | 사용자가 개발환경을 Ubuntu 로 별도 전환 예정 — 본 가이드·스크립트는 환경 무관(멱등) 재사용 가능 |

VM 시작/정지(헤드리스):
```powershell
& "C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe" start "C:\work\vms\Ubuntu 64-bit.vmx" nogui
& "C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe" stop  "C:\work\vms\Ubuntu 64-bit.vmx" soft
```
> 주의: `Documents\Virtual Machines\` 하위의 Ubuntu 24.04.3 VM 2개는 nex-ubuntu 가 **아니다**(MAC 불일치 실측). nex-ubuntu 정본은 `C:\work\vms\`.

---

## 1. 프로비저닝 — `scripts/m1_provision.sh` (루트/sudo 불필요)

전부 `$HOME` 에 설치하므로 sudo 가 없어도 된다. 멱등(재실행 안전).

| 구성요소 | 방식 | 위치 |
|---|---|---|
| JDK 17 | Temurin tarball (api.adoptium.net) | `~/opt/jdk17` |
| SWIG 4.2 | `apt download swig` + `dpkg -x` (루트 불필요) | `~/opt/swig` |
| Android cmdline-tools + **NDK r28** | dl.google.com zip + `sdkmanager`(28.x 최신 자동 선택) | `~/android-sdk` |
| 환경 변수 | 이후 모든 단계가 `source ~/.m1env` | `~/.m1env` |

실행(Windows 호스트에서):
```bash
scp android/docs/scripts/m1_provision.sh nex-ubuntu:~/
ssh nex-ubuntu 'bash ~/m1_provision.sh'
```

전제(시스템 패키지): `gcc/make(build-essential), git, curl, unzip, python3` — Ubuntu 기본 또는 sudo 1회 설치. nex-ubuntu 에는 **이미 있음**(실측). `autoconf/libtool 은 불필요` — pjproject 가 사전 생성 `aconfigure` 를 동봉.

실측 메모:
- noble 의 swig 4.2.0 deb 는 lib 디렉터리가 `/usr/share/swig4.0` (버전명과 다름 — **정상**, `swig -swiglib` 컴파일 기본값과 일치, 실동작 검증 완료).
- sudo 가 가능한 환경이면 `sudo apt-get install -y build-essential git curl unzip swig openjdk-17-jdk` 로 1·2행을 대체해도 된다(이 경우 `~/.m1env` 의 JAVA_HOME/SWIG 경로는 시스템 것으로 조정).

---

## 2. 빌드 — `scripts/m1_build_pjsip.sh` (clone → config_site.h → make → SWIG)

**상태: 미실행** (다음 단계). 스크립트가 하는 일:

1. `git clone --depth 1 --branch 2.16 pjproject` + `and_aud_mediacodec.cpp` 실존 확인(경로 C 전제)
2. `pjlib/include/pj/config_site.h` 생성 — **설계서 §2.5 경로 C 확정본 그대로**(AND_MEDIA_AMRWB=1 / AMRNB=0 / OPENCORE_AMR{WB,NB}=0 / H264=1 / VP8·9=0 / G711 안전망 / SRTP·TLS=0). 생성 후 sha256 출력(빌드 메타 기록용)
3. `APP_PLATFORM=28 TARGET_ABI=arm64-v8a ./configure-android --use-ndk-cflags` → `make dep && make -j$(nproc)`
4. `pjsip-apps/src/swig` 에서 `make`(직렬) → `libpjsua2.so` + `org/pjsip/pjsua2/*.java`
5. 산출물 검증: arm64 ABI 확인 + **`libc++_shared.so` 동봉**(없으면 NDK sysroot 에서 자동 복사 — 미동봉 시 앱 즉시 `UnsatisfiedLinkError`, 설계서 위험 #2)

실행:
```bash
scp android/docs/scripts/m1_build_pjsip.sh nex-ubuntu:~/
ssh nex-ubuntu 'bash ~/m1_build_pjsip.sh'   # 8코어 기준 수 분~십수 분
```

---

## 3. 산출물 core 투입 (설계서 §2.7)

SWIG 생성물(수백 파일)은 손코드와 섞지 않게 **별도 소스셋** `core/src/pjsua2/` 에 격리:

```bash
# Windows 호스트에서 (경로는 빌드 로그의 JNIDIR/JAVADIR 출력 참조)
scp nex-ubuntu:'~/pjproject/pjsip-apps/src/swig/java/android/pjsua2/src/main/jniLibs/arm64-v8a/*.so' \
    android/core/src/pjsua2/jniLibs/arm64-v8a/
scp nex-ubuntu:'~/pjproject/pjsip-apps/src/swig/java/android/pjsua2/src/main/java/org/pjsip/pjsua2/*.java' \
    android/core/src/pjsua2/java/org/pjsip/pjsua2/
```

`core/build.gradle.kts`: `ndk.abiFilters += "arm64-v8a"`, `sourceSets`(java.srcDir/jniLibs.srcDir), `packaging.jniLibs.useLegacyPackaging = false`(16KB page 정렬) — 설계서 §2.7 코드 그대로. AGP 9.2.1 DSL 유효성은 Gradle sync 로 확인(verify-on-machine).

빌드 메타(README 또는 커밋 메시지에 고정 기록): pjproject 태그(2.16) / NDK(`28.2.13676358`) / config_site.h sha256 / SWIG(4.2.0).

---

## 4. M1.0 게이트 (설계서 §5.1)

앱이 `Endpoint libCreate→libInit→UDP transport→libStart→destroy` 를 크래시/`UnsatisfiedLinkError` 없이 통과 + logcat 에 PJSIP 버전 배너 + **`codecEnum2()` 출력에 AMR-WB 정확히 1개**(중복 등록 없음 — 경로 C 게이트). 콜드스타트 10회 연속. 실기기(UNIWA) 필요.

## 빌드 메타 (산출물 재현용)

| 항목 | 값 |
|---|---|
| pjproject | **2.16** (`git checkout 2.16`) |
| NDK | **28.2.13676358** (r28c) — arm64-v8a, `--use-ndk-cflags`, APP_PLATFORM=28 |
| SWIG | **4.1.0** (4.4.0 와 4.1.0 동시 추출 시 4.1.0 사용 — 2.16 호환 안전) |
| JDK(빌드 호스트) | 17(네이티브/SWIG), 21(gradle 데몬) |
| `config_site.h` sha256 | `de61f562955ef4d5456c9e03be8754b96e3b4f7999cfb4159484a1bab00a9de7` (경로 C 확정본) |
| 산출물 | `libpjsua2.so`(arm64, Android 28) + `libc++_shared.so` + SWIG Java 306파일 → `core/src/pjsua2/` |

## 진행 스냅샷

- [x] **M1.0-1** 빌드 환경 프로비저닝(`~/.m1env`: JDK17/JDK21/SWIG4.1/NDK r28)
- [x] **M1.0-2~4** pjproject 2.16 + config_site.h(경로 C) + make(arm64) + SWIG Java
- [x] **M1.0-5** 산출물 core 투입(`core/src/pjsua2/{jniLibs,java}`) + gradle 소스셋/abiFilter/packaging
- [x] **M1.0-6** PjLib/SipController/CimsAccount/CimsCall + SipService(FGS) + UI 작성 → `:volte-client:assembleDebug` **APK 빌드 성공**(arm64 .so 동봉 확인)
- [ ] **M1.0-게이트** 실기기(UNIWA) 부팅 스모크: `libStart` + UDP transport + `codecEnum2` AMR-WB 1개 (off-box, 기기 필요)
- [ ] **M1.1** REGISTER(Digest) 실서버 — 실기기 + 라이브 CSP(15060)

> **SWIG 시그니처 실측 교정(설계서 verify-on-machine 해소):** 이 SWIG 빌드의 enum 은 Java enum 이
> 아니라 `public final static int` 상수 → **`swigValue()` 없음**(설계 스켈레톤 전제와 다름). 정수
> 상수를 직접 쓴다. `transportCreate(int, TransportConfig)`, `OnRegStateParam.getCode():int`,
> SWIG 벡터(`CodecInfoVector2`/`CallMediaInfoVector`)는 `AbstractList` 상속이라 **`.size` 프로퍼티 +
> `[i]` 인덱싱**(`.size()` 호출 불가). 코드는 이 실측값 기준으로 작성됨.

## 빌드 환경 메모 (gradle 데몬)

`gradle/gradle-daemon-jvm.properties` 는 Android Studio 가 JBR(vendor=jetbrains) 21 로 생성하는데,
이는 IDE 번들 전용이라 **headless/CI 에서 자동 프로비저닝 불가**(foojay 에 JBR URL 없음). 본 레포는
이식성을 위해 **벤더 무관 `toolchainVersion=21`** 로 둔다(임의 JDK 21 데몬 허용). headless 빌드:
`gradle ... -Dorg.gradle.java.installations.paths=<JDK21경로>`.

트러블슈팅 빠른 표는 설계서 §2.9.
