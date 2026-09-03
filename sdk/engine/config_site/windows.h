/* CIMS 단말 엔진 config_site — Windows 데스크톱 (MSVC x64, Win32 API). 관제조작반 데스크톱 앱용 (ue_sdk.md §6).
   빌드: sdk/windows 슈퍼빌드가 pjproject 자체 CMake(ext/pjproject/CMakeLists.txt) 를 돌리기 전에
   pjlib/include/pj/config_site.h 에 이 파일의 #include 한 줄을 둔다(pjproject-vs14.sln 도 같은 파일을 읽는다).
   F1(엔진·cimsue-cli) 실빌드에서 확정한다 — 그 전까지는 설계값. */

#include <pj/config_site_sample.h>

#include "common.h"

/* 영상: F1 은 음성만(감청·PTT 청취·BLF·픽업·전달 전부 음성). F3 에서 1 로 올리고 OpenH264(코덱)·DSHOW(캡처)·
   CIMS 콜백 렌더 장치(프레임 → onVideoFrame, ue_sdk.md §4.5) 를 함께 켠다. SDL 창은 어느 단계에도 쓰지 않는다. */
#define PJMEDIA_HAS_VIDEO              0
#define PJMEDIA_VIDEO_DEV_HAS_SDL      0
#define PJMEDIA_VIDEO_DEV_HAS_DSHOW    0

/* 오디오: WMME 단일 백엔드. pjproject 2.16 의 wasapi_dev.cpp 는 UWP/Windows Phone 전용(phoneaudioclient.h·
   Windows::Phone::Media::Devices — vcxproj 도 WinDesktop 에서 제외)이라 데스크톱에서는 컴파일되지 않는다.
   헤드셋+스피커 분리 출력은 코어의 재생 라우트(pjsua2 ExtraAudioDevice 재생 전용 패치) 가 WMME 장치 2개를 연다.
   WASAPI 데스크톱 백엔드는 WMME 지연·핫플러그가 실측에서 문제될 때의 후속 과제(ue_sdk.md §11). */
#define PJMEDIA_AUDIO_DEV_HAS_WMME     1
#define PJMEDIA_AUDIO_DEV_HAS_WASAPI   0

/* 음성: AMR-WB = opencore-amrwb(디코드) + vo-amrwbenc(인코드) — sdk/windows/deps 의 CMake 래퍼로 MSVC 빌드 */
#define PJMEDIA_HAS_OPENCORE_AMRWB_CODEC  1
#define PJMEDIA_HAS_OPENCORE_AMRNB_CODEC  0

/* 인증: 관제 소프트폰 가입자 규약은 Digest+TLS(volte_supplementary_services.md) — AKA(milenage) 는 Windows 엔진에서 제외.
   pjproject CMake 의 third_party 에 milenage 가 없어 켜면 링크가 깨진다. */
#define PJSIP_HAS_DIGEST_AKA_AUTH      0

/* AMR-WB 라이브러리는 CMake 가 imported target(OpenCoreAMRWB::/VisualOnAMRWBEnc::) 으로 링크한다. pjmedia-codec 의 MSVC 자동 링크
   (#pragma comment(lib, "libopencore-amrwb.a") — gcc 산출물 이름 전제) 는 끈다. 켜 두면 pjsua 앱 링크에서 LNK1104. */
#define PJMEDIA_AUTO_LINK_OPENCORE_AMR_LIBS 0
