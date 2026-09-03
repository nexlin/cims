/* CIMS 단말 엔진 config_site — Windows (MSVC x64). 관제조작반 데스크톱 앱용 (ue_sdk.md §6).
   빌드: pjproject-vs14.sln 빌드 전에 pjlib/include/pj/config_site.h 에 이 파일의 #include 한 줄을 둔다.
   ue_sdk.md §10 F 단계에서 실빌드로 확정한다 — 그 전까지는 설계값. */

#include <pj/config_site_sample.h>

#include "common.h"

/* 영상: 코어는 창을 열지 않고 프레임 콜백만 준다(ue_sdk.md §4.5) — SDL 렌더러 제외 */
#define PJMEDIA_HAS_VIDEO              1
#define PJMEDIA_VIDEO_DEV_HAS_SDL      0

/* 오디오: WASAPI 단일 백엔드. 헤드셋+스피커 분리 출력은 pjsua2 ExtraAudioDevice 로 코어가 처리 */
#define PJMEDIA_AUDIO_DEV_HAS_WASAPI   1
#define PJMEDIA_AUDIO_DEV_HAS_WMME     0

/* 음성: AMR-WB = opencore-amrwb + vo-amrwbenc (MSVC 빌드) */
#define PJMEDIA_HAS_OPENCORE_AMRWB_CODEC  1
#define PJMEDIA_HAS_OPENCORE_AMRNB_CODEC  0
