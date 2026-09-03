/* CIMS 단말 엔진 config_site — Linux (개발 서버·CI). libcimsue 단위시험과 헤드리스 UE `cimsue-cli` 용.
   빌드: 루트 CMakeLists.txt 의 ExternalProject(pjproject) 가 pjlib/include/pj/config_site.h 에 이 파일의
   #include 한 줄을 생성한다. 오디오 장치는 configure --disable-sound → null 장치(헤드리스). */

#include <pj/config_site_sample.h>

#include "common.h"

/* 영상 없음 — 헤드리스. 영상 정합(H.264 SRTP 등)은 cspsim 축이 담당한다. */
#define PJMEDIA_HAS_VIDEO              0

/* 음성: AMR-WB = opencore-amrwb(디코드) + vo-amrwbenc(인코드) — 서버 빌드가 pkg/ 에 이미 만든 것을
   configure --with-opencore-amr / --with-opencore-amrwbenc 로 링크한다. AMR-NB 제외. */
#define PJMEDIA_HAS_OPENCORE_AMRWB_CODEC  1
#define PJMEDIA_HAS_OPENCORE_AMRNB_CODEC  0
