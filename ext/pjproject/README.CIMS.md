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
  | 무전/통화 분리 라우팅 (`set_preferred_device`, OUTPUT_ROUTE) · 송화 마이크 고정 (INPUT_ROUTE EARPIECE → `AudioRecord.setPreferredDevice` 내장 기본 마이크, 후면 `back` 제외) | `pjmedia/src/pjmedia-audiodev/android_jni_dev.c` | Android 전용 — PTT 채널과 통화의 출력 장치 분리, 스피커 출력 시 정책이 고르는 후면 마이크 대신 송화구 마이크(ue_audio_level.md §3). INPUT_ROUTE·INPUT_SOURCE 는 같은 캡 비트 — 값의 ROUTE_CUSTOM 으로 구분. 재생 전용 스트림에서도 값을 받아 keep 저장을 유지 |
  | PTT 유휴 무음 50pps 상향 스트림 제거 (`stream->vad_enabled` 분기) | `pjmedia/src/pjmedia/stream.c` | 브리지 미연결 유휴 시 무음 RTP 송신 생략 — KA 가 NAT 유지 담당 |
  | 이벤트 구독 (`pjsua_cims_conf_subscribe`, `cims_conf_find`) | `pjsip/src/pjsua-lib/pjsua_acc.c`, `pjsua_pres.c` | conference(RFC 4575)·xcap-diff(RFC 5875)·dialog(RFC 4235, 관제 BLF·Join 대상 학습) 구독의 in-dialog 갱신(RFC 6665) — NOTIFY 본문은 on_pager2 로 앱에 전달. 동시 구독 슬롯 `PJSUA_CIMS_MAX_SUB`(기본 256, config_site 재정의) — 관제조작반이 `monitor_scope=all` 이면 조직 전원 dialog + 채널 conference + PSI 를 한 표에 담는다. dialog 패키지는 upstream mod-dlg-event 가 먼저 등록하므로 EPKGEXISTS 를 정상으로 본다 |
  | `ExtraAudioDevice` 재생 전용 모드 (`recDev == PJMEDIA_AUD_INVALID_DEV` → `PJMEDIA_DIR_PLAYBACK`, `cims_play_only`) | `pjsip/src/pjsua2/media.cpp` | 코어 재생 라우트(`Engine::addPlaybackRoute`) — 관제석 헤드셋+스피커 분리 출력에서 두 번째 장치의 마이크를 열지 않음 (ue_sdk.md §6) |
  | Camera2 로컬 셀프뷰 프리뷰 (`PjCamera2.SetPreviewSurface`) | `pjmedia/src/pjmedia-videodev/android/PjCamera2.java` | 앱이 등록한 프리뷰 Surface 를 열린 CameraDevice 의 CaptureSession 에 인코딩 ImageReader 와 함께 출력 target 으로 추가 — 카메라 2중 오픈 없이 영상통화 셀프뷰(PiP). `sdk/android/build-native.sh` 가 `android/core/src/pjsua2/java/org/pjsip/` 로 복사한다 |
  | `Account::sendRequest` 401/407 재인증 재발행 (`cims_send_request_reauth`, `send_request_data.auth_retry`) | `pjsip/src/pjsua-lib/pjsua_acc.c` | out-of-dialog 요청(MESSAGE·PUBLISH)의 챌린지에 계정 자격으로 CSeq+1 재발행 — regc/inv/evsub/pjsua_im 과 달리 이 경로엔 없었다. UDP 등록 단말의 MCData SDS(≈1.6KB)가 RFC 3261 §18.1.1 TCP 승격으로 등록 flow 밖에서 401 받아 유실되던 원인(mcdata_messaging.md §4). 상한 PJSIP_MAX_STALE_COUNT |
  | U10 동시 발언 SSRC 디먹스 (`cims_mt_rx`) | `pjmedia/src/pjmedia/stream.c`, `stream_imp_common.c` | 한 스트림의 SSRC 별 서브스트림(지터버퍼+디코더) → PCM 합산 — mcptt_ue_multitalker_media.md §5. secondary SSRC 도 빈 payload·비협상 PT 는 소비만(AMR 파서 보호 — 감청 tap 의 CMP 자체 SSRC 패킷) |
  | 음성 레벨 — 마이크 AGC · 피크 리미터 (`cims_level.h`, `pjmedia_conf_set_rx_agc`, `pjsua_conf_set_rx_agc`, pjsua2 `AudDevManager::setCaptureAgc`) | `pjmedia/src/pjmedia/cims_level.h`·`conference.c`, `pjmedia/include/pjmedia/{config,conference}.h`, `pjsip/src/pjsua-lib/pjsua_aud.c`, `pjsip/{include,src}/pjsua2/media.*` | slot 0 마이크 rx 에 P.56 활성 레벨 AGC(목표 -26 dBov, 음성 게이트·원단 게이트, `PJMEDIA_CONF_CIMS_MIC_AGC`), 게인을 곱하는 포트 rx·tx 경로 끝의 하드 클립을 프레임 look-ahead 리미터(-1 dBFS, `PJMEDIA_CONF_CIMS_LIMITER`)로 교체 — 단말마다 34 dB 넘게 다른 마이크 레벨·배율 누적 포화 대책. 정본 docs/design/features/ue_audio_level.md |
  | 영상 스트림 keep-alive 즉시 송출 (`PJSUA_CALL_VID_STRM_SEND_KEEPALIVE`, `pjmedia_vid_stream_send_keep_alive`) | `pjsip/include/pjsua-lib/pjsua.h`, `pjsip/src/pjsua-lib/pjsua_vid.c`, `pjmedia/{include/pjmedia/vid_stream.h,src/pjmedia/vid_stream.c}` | 영상 keep-alive 는 인코딩 경로(put_frame)에서만 나가 송출 정지(STOP_TRANSMIT) 중인 수신 전용 참가자의 NAT 매핑이 풀린다 — PTT 그룹 영상 청취자(CMP 는 멤버 영상 포트를 그 멤버가 보낸 패킷으로 latch)를 위해 코어가 주기적으로 부른다(ue_sdk.md §4.5). 연산은 enum 끝에 붙여 pjsua2 `vidSetStream` 이 그대로 넘긴다 |
  | RTCP-XR 통계 조회 (`pjsua_call_get_stream_stat_xr`) | `pjsip/src/pjsua-lib/pjsua_call.c`, `pjsip/include/pjsua-lib/pjsua.h` | pjsua 가 dump 에서만 읽던 `pjmedia_stream_get_stat_xr`(RFC 3611 VoIP Metrics)를 API 로 — 코어 `callQuality` 의 입력(ue_voice_quality.md §3). 빌드 스위치는 config_site `common.h` `PJMEDIA_HAS_RTCP_XR`·`PJMEDIA_STREAM_ENABLE_XR` |
  | 창 없는 프레임 렌더 장치 (`cims_frame_dev.c` «CIMS frame sink», `pjmedia_cims_frame_dev_set_callback`, `PJMEDIA_VIDEO_DEV_HAS_CIMS_FRAME`) | `pjmedia/{include,src}/pjmedia-videodev/cims_frame_dev.*`, `videodev.c`(렌더 장치 맨 앞 등록)·`config.h`(기본 0), 빌드 목록 셋(`pjmedia/build/Makefile`·`pjmedia_videodev.vcxproj(.filters)`·`pjmedia/CMakeLists.txt`) | Windows 관제 앱 영상(ue_sdk.md §4.5) — BGRA 프레임(디코더 I420 → libyuv)을 콜백으로, 창 핸들 = 코어가 고른 토큰(NULL = 버림). 콜백 등록·해제는 렌더 스트림이 없을 때(기동 직후·파괴 뒤) — 전달 경로에 잠금 없음 |
  | CMake 영상 코덱 칸 (`PJMEDIA_HAS_OPENH264_CODEC`·`PJMEDIA_HAS_VPX_CODEC`) | `pjmedia/include/pjmedia-codec/config_auto.h.cm` | CMake 가 값을 정해 두고 틀에 칸이 없어 늘 미정의 — OpenH264 를 켜도 등록되지 않았다(autoconf 는 CFLAGS -D 로 넘긴다) |
  | CMake DirectShow BaseClasses | `pjmedia/CMakeLists.txt`(`PJMEDIA_WITH_VIDEODEV_DSHOW` 면 `third_party/BaseClasses` 소스·include 를 pjmedia-videodev 에), `third_party/BaseClasses/streams.h`(min/max) | upstream CMake 의 TODO — dshowclasses.cpp 가 `streams.h` 를 못 찾았다. pjlib 설정 헤더(os_auto.h)가 NOMINMAX 를 정의해 windows.h min/max 가 없어 BaseClasses 쓰는 두 매크로를 둔다 |
  | OpenH264 IDR 주기 2 초 (`uiIntraPeriod`) | `pjmedia/src/pjmedia-codec/openh264.cpp` | And-Media `KEYFRAME_INTERVAL 2` 와 같은 값 — 그룹 영상 수신자는 송출 중간에 붙고(MCVideo [보기]·MCPTT 늦은 합류) MCPTT 영상은 CMP 가 PLI 를 넘기지 않는다. upstream 0 = 요청에만 |
  | pjsua2 `CimsPjCfg` — UDP→TCP 승격 스위치 (`setDisableTcpSwitch`, SWIG 노출) | `pjsip/{include,src}/pjsua2/cims_cfg.*`, `pjsip/include/pjsua2.hpp`, 빌드 목록 셋(`pjsip/build/Makefile`·`pjsua2_lib.vcxproj`·`pjsip/CMakeLists.txt`) | 단말 스위치 `sip.udpNoTcpSwitch` — `pjsip_cfg()->endpt.disable_tcp_switch`(registration_binding_set.md §4.1b). pjsua2 에 파일을 더하면 빌드 목록 셋에 모두 올린다 — `pjsua2.hpp` 가 include 하므로 CMake 설치 FILE_SET 에 빠지면 Windows 코어 컴파일이 깨진다 |

- `config_site.h` 는 upstream 이 무시하는 파일이라 트리에 없다. 플랫폼별 정본은 `sdk/engine/config_site/{common,android,linux,windows}.h`
  (ue_sdk.md §3) 이며 빌드가 `pjlib/include/pj/config_site.h` 에 해당 플랫폼 파일을 `#include` 하는 한 줄을 생성한다.

## 빌드

| 플랫폼 | 방법 |
|---|---|
| Linux | 루트 CMake `ExternalProject_Add(pjproject)` — `aconfigure` + `make` (ue_sdk.md §8) |
| Android | `sdk/android/build-native.sh` — 이 트리를 `configure-android`(NDK) 로 빌드 + SWIG 후 산출물 배치. 패치 적용 단계는 없다(트리가 정본). `android/docs/scripts/m1_build_pjsip.sh` 는 위임 스텁 |
| Windows | `sdk/windows` 슈퍼빌드가 이 트리의 자체 CMake(`CMakeLists.txt`, WMME 백엔드 · 영상 = OpenH264(vcpkg)·DirectShow·libyuv·CIMS 프레임 렌더)를 ExternalProject 로 빌드. `pjproject-vs14.sln` 은 폴백 (ue_sdk.md §6) |

## 경계

- `third_party/srtp`(동봉분)는 단말 엔진 전용이다. 서버(CMP)는 `ext/libsrtp` 독립 vendoring 을 쓰며 서로 링크하지 않는다
  (루트 `CMakeLists.txt` libsrtp 절).
- upstream 갱신은 이 트리 위에서 merge/rebase 로 한다.

> `.gitignore` 의 `**/build/`·`**/Makefile` 패턴에 걸리는 파일이 있으므로 새 파일 추가 시 `git add -f ext/pjproject` 를 쓴다
> (기존 트래킹 파일은 무관).
