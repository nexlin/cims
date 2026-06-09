# CIMS 안드로이드 단말(UE) 클라이언트

CIMS 서버(CSP/CMP/CSC)에 직접 붙는 안드로이드 네이티브 단말 앱. `cspsim`처럼 **SIP/RTP를 직접** 말한다(웹폰 `cims-phone`/`cwrtc` 게이트웨이 경유가 아님).

스택: **PJSIP**(시그널링 + 미디어 파이프라인) + **안드로이드 MediaCodec**(AMR-WB 음성 / H.264 영상).
상세 설계: [../docs/design/features/android_ue_client.md](../docs/design/features/android_ue_client.md)

## 구성

| 디렉터리 | 내용 |
|---|---|
| `volte-client/` | VoLTE 1:1 SIP 소프트폰 (전화 앱) |
| `ptt-client/` | MCPTT 그룹 PTT 앱 (affiliation·floor·그룹·CSC) |
| `core/` | **공유 Android Library**: PJSIP 래퍼·MediaCodec 코덱(AMR-WB/H.264)·SIP 등록/INVITE/RTP·미디어제어. volte-client·ptt-client가 의존 |

> **결정:** 공유 코드는 `core`(Android Library 모듈)에 두고 두 앱이 의존한다.
> PTT = VoLTE(core) + affiliation/floor/group/CSC. 두 앱은 별도 APK로 배포하되 같은 `core`에 의존.

## 빌드 / 실행 (M0)

요구: **Android Studio(Windows)** + **UNIWA 실기기**. 개발 환경 처음 구성은 → [docs/dev_environment_setup.md](docs/dev_environment_setup.md). PJSIP `.so` 빌드(WSL2)는 → [docs/M0_pjsip_build_wsl2.md](docs/M0_pjsip_build_wsl2.md).

1. Android Studio에서 `android/` 폴더 열기 → Gradle sync(wrapper 자동 생성).
   - CLI라면: `gradle wrapper --gradle-version 9.4.1` 후 `./gradlew :volte-client:assembleDebug`
2. UNIWA 단말 USB 연결(개발자 모드/USB 디버깅) → `volte-client` 실행.
3. **[코덱 가용성]** — 이 기기의 AMR-WB/H.264 인코더·디코더(SW/HW) 목록 확인.
4. **[AMR-WB 스파이크]** — AMR-WB 인코드→디코드 루프백 처리량 측정 → 실시간 예산 충족 여부 판정(**M0 게이트**).

> `gradlew`/`gradle-wrapper.jar`은 Android Studio가 처음 열 때 생성한다(저장소에는 `gradle-wrapper.properties`만 포함).

## 상태

**M0 진행 중.** Gradle 멀티모듈 골격(`:core`/`:volte-client`) + MediaCodec 스파이크 제공.
다음: 스파이크 결과 확인(UNIWA) → PJSIP 빌드(WSL2) → `core` 통합 → **M1(VoLTE 1:1 음성+영상)**.
마일스톤 M0~M4는 [설계 문서](../docs/design/features/android_ue_client.md) §10 참조.
