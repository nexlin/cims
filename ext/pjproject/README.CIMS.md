# ext/pjproject — CIMS 단말 엔진 pjproject (소스 정본)

pjsip/pjproject **2.16** (upstream commit `6cab30c`) 에 CIMS 단말(UE) 패치를 적용한 소스 트리.
psip·opencore-amr 처럼 "수정해서 쓰는 외부 소스는 `ext/` 에 커밋"하는 관례를 따르며,
**단말 SDK `libcimsue` 의 엔진**으로 Linux(개발 서버·`cimsue-cli`)·Android(NDK)·Windows(MSVC)
세 툴체인이 **이 트리 하나**를 빌드한다 — 설계 정본 [docs/design/features/ue_sdk.md](../../docs/design/features/ue_sdk.md) §3.

## 정본(SoT)

- **이 트리가 유일한 소스 정본이다.** pjproject 를 수정할 때는 여기서 직접 고치고 커밋한다. 변경 이력은 git.
- 적용된 CIMS 패치의 내용과 이유는 각 수정 지점의 `CIMS` 주석이 설명한다. 인벤토리:

  | 패치 | 파일 | 요지 |
  |---|---|---|
  | AMR-WB codec_setting NULL 크래시 | `pjmedia/src/pjmedia-codec/and_aud_mediacodec.cpp` | upstream 2.16 버그 — And-Media AMR-WB 열기 시 NULL 역참조 방지 |
  | AMR 인코더 스톨 워치독 (`enc_fail_watchdog`) | 같은 파일 | MediaCodec 인코더 무응답 시 재기동 |
  | H.264 IDR 주기 2초 (`KEYFRAME_INTERVAL 2`) · 발신 비트레이트 상한 500kbps + CBR (`cims_br`) | `pjmedia/src/pjmedia-codec/and_vid_mediacodec.cpp` | 영상 정합·대역 상한 |
  | pjsua2 `StreamInfo::fromPj` NULL codec-param 가드 · sockaddr AF 가드 (`cims_print_sockaddr_safe`) | `pjsip/src/pjsua2/call.cpp` | 협상 실패/비 RTP 슬롯에서의 SIGABRT 방지 |
  | `stream_info.c` si->param zero-init · `pjsua_txt` 비-RTP m=text 슬롯 스트림 생성 스킵 | `pjmedia/src/pjmedia/stream_info.c`, `pjsip/src/pjsua-lib/pjsua_txt.c` | MSRP(m=message/TCP) 슬롯을 RTP 스트림으로 열지 않음 |
  | 무전/통화 분리 라우팅 (`set_track_preferred_device`, OUTPUT_ROUTE) | `pjmedia/src/pjmedia-audiodev/android_jni_dev.c` | Android 전용 — PTT 채널과 통화의 출력 장치 분리 |
  | PTT 유휴 무음 50pps 상향 스트림 제거 (`stream->vad_enabled` 분기) | `pjmedia/src/pjmedia/stream.c` | 브리지 미연결 유휴 시 무음 RTP 송신 생략 — KA 가 NAT 유지 담당 |
  | 이벤트 구독 (`pjsua_cims_conf_subscribe`, `cims_conf_find`) | `pjsip/src/pjsua-lib/pjsua_acc.c`, `pjsua_pres.c` | conference(RFC 4575)·xcap-diff(RFC 5875)·dialog(RFC 4235, 관제 BLF·Join 대상 학습) 구독의 in-dialog 갱신(RFC 6665) — NOTIFY 본문은 on_pager2 로 앱에 전달. 동시 구독 슬롯 `PJSUA_CIMS_MAX_SUB`(기본 256, config_site 재정의) — 관제조작반이 `monitor_scope=all` 이면 조직 전원 dialog + 채널 conference + PSI 를 한 표에 담는다. dialog 패키지는 upstream mod-dlg-event 가 먼저 등록하므로 EPKGEXISTS 를 정상으로 본다 |
  | `ExtraAudioDevice` 재생 전용 모드 (`recDev == PJMEDIA_AUD_INVALID_DEV` → `PJMEDIA_DIR_PLAYBACK`, `cims_play_only`) | `pjsip/src/pjsua2/media.cpp` | 코어 재생 라우트(`Engine::addPlaybackRoute`) — 관제석 헤드셋+스피커 분리 출력에서 두 번째 장치의 마이크를 열지 않음 (ue_sdk.md §6) |
  | Camera2 로컬 셀프뷰 프리뷰 (`PjCamera2.SetPreviewSurface`) | `pjmedia/src/pjmedia-videodev/android/PjCamera2.java` | 앱이 등록한 프리뷰 Surface 를 열린 CameraDevice 의 CaptureSession 에 인코딩 ImageReader 와 함께 출력 target 으로 추가 — 카메라 2중 오픈 없이 영상통화 셀프뷰(PiP). `sdk/android/build-native.sh` 가 `android/core/src/pjsua2/java/org/pjsip/` 로 복사한다 |
  | `Account::sendRequest` 401/407 재인증 재발행 (`cims_send_request_reauth`, `send_request_data.auth_retry`) | `pjsip/src/pjsua-lib/pjsua_acc.c` | out-of-dialog 요청(MESSAGE·PUBLISH)의 챌린지에 계정 자격으로 CSeq+1 재발행 — regc/inv/evsub/pjsua_im 과 달리 이 경로엔 없었다. UDP 등록 단말의 MCData SDS(≈1.6KB)가 RFC 3261 §18.1.1 TCP 승격으로 등록 flow 밖에서 401 받아 유실되던 원인(mcdata_messaging.md §4). 상한 PJSIP_MAX_STALE_COUNT |
  | U10 동시 발언 SSRC 디먹스 (`cims_mt_rx`) | `pjmedia/src/pjmedia/stream.c`, `stream_imp_common.c` | 한 스트림의 SSRC 별 서브스트림(지터버퍼+디코더) → PCM 합산 — mcptt_ue_multitalker_media.md §5. secondary SSRC 도 빈 payload·비협상 PT 는 소비만(AMR 파서 보호 — 감청 tap 의 CMP 자체 SSRC 패킷) |

- `config_site.h` 는 upstream 이 무시하는 파일이라 트리에 없다. 플랫폼별 정본은 `sdk/engine/config_site/{common,android,linux,windows}.h`
  (ue_sdk.md §3) 이며 빌드가 `pjlib/include/pj/config_site.h` 에 해당 플랫폼 파일을 `#include` 하는 한 줄을 생성한다.

## 빌드

| 플랫폼 | 방법 |
|---|---|
| Linux | 루트 CMake `ExternalProject_Add(pjproject)` — `aconfigure` + `make` (ue_sdk.md §8) |
| Android | `sdk/android/build-native.sh` — 이 트리를 `configure-android`(NDK) 로 빌드 + SWIG 후 산출물 배치. 패치 적용 단계는 없다(트리가 정본). `android/docs/scripts/m1_build_pjsip.sh` 는 위임 스텁 |
| Windows | `sdk/windows` 슈퍼빌드가 이 트리의 자체 CMake(`CMakeLists.txt`, WMME 백엔드)를 ExternalProject 로 빌드. `pjproject-vs14.sln` 은 폴백 (ue_sdk.md §6) |

## 경계

- `third_party/srtp`(동봉분)는 단말 엔진 전용이다. 서버(CMP)는 `ext/libsrtp` 독립 vendoring 을 쓰며 서로 링크하지 않는다
  (루트 `CMakeLists.txt` libsrtp 절).
- upstream 갱신은 이 트리 위에서 merge/rebase 로 한다.

> `.gitignore` 의 `**/build/`·`**/Makefile` 패턴에 걸리는 파일이 있으므로 새 파일 추가 시 `git add -f ext/pjproject` 를 쓴다
> (기존 트래킹 파일은 무관).
