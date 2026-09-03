/* CIMS 단말 엔진 config_site — Android (NDK arm64-v8a). 경로 C: And-Media(MediaCodec) 음성+영상.
   빌드: sdk/android/build-native.sh 가 pjlib/include/pj/config_site.h 에 이 파일의 #include 한 줄을 생성한다. */

/* 1) Android 표준 설정 — 공통/오버라이드보다 먼저 */
#define PJ_CONFIG_ANDROID 1
#include <pj/config_site_sample.h>

#include "common.h"

/* 2) 영상: And-Media H264. VP8/VP9 는 협상 표면 축소를 위해 빌드 제외 */
#define PJMEDIA_HAS_VIDEO              1
#define PJMEDIA_HAS_AND_MEDIA_H264     1
#define PJMEDIA_HAS_AND_MEDIA_VP8      0
#define PJMEDIA_HAS_AND_MEDIA_VP9      0

/* 3) 음성: And-Media AMR-WB 정본. AMR-NB 제외, opencore factory 빌드 제외(중복 등록 방지) */
#define PJMEDIA_HAS_AND_MEDIA_AMRWB    1
#define PJMEDIA_HAS_AND_MEDIA_AMRNB    0
#define PJMEDIA_HAS_OPENCORE_AMRWB_CODEC  0
#define PJMEDIA_HAS_OPENCORE_AMRNB_CODEC  0
