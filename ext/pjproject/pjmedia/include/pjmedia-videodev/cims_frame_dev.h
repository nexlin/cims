/*
 * CIMS — 창 없는 프레임 콜백 렌더 장치 (ue_sdk.md §4.5·§6.1 «영상»).
 *
 * pjproject 의 렌더 장치는 전부 자기 창(SDL·OpenGL·Metal …)에 그린다. 관제 데스크톱(WPF)은 앱 화면 안의 칸에
 * 영상을 넣어야 하므로 디코드된 프레임을 그대로 앱 콜백으로 넘기는 렌더 장치를 둔다 — 픽셀 형식은 BGRA 하나
 * (32 bpp, 바이트 순서 B,G,R,A — WPF PixelFormats.Bgr32/Bgra32 와 같다). 디코더 출력(I420)은 vid_port 가
 * 변환기(libyuv I420ToARGB)로 바꿔 넘긴다.
 *
 * 창 핸들 = 앱이 정한 토큰(pjsua_vid_win_set_win → PJMEDIA_VID_DEV_CAP_OUTPUT_WINDOW 의 handle.window). 토큰이 NULL 인
 * 렌더러의 프레임은 버린다 — 코어가 수신 창마다 그 호를 가리키는 토큰을 건다.
 *
 * 빌드 스위치 PJMEDIA_VIDEO_DEV_HAS_CIMS_FRAME (config_site — Windows 만 1).
 */
#ifndef __PJMEDIA_VIDEODEV_CIMS_FRAME_DEV_H__
#define __PJMEDIA_VIDEODEV_CIMS_FRAME_DEV_H__

#include <pjmedia-videodev/videodev.h>

PJ_BEGIN_DECL

/** 콜백으로 넘기는 프레임 한 장 — data 는 콜백이 끝날 때까지만 유효하다. */
typedef struct pjmedia_cims_frame
{
    const void          *data;      /**< 첫 줄의 첫 화소(위 → 아래)          */
    pj_size_t            size;      /**< stride × height                     */
    unsigned             width;     /**< 화소                                */
    unsigned             height;
    unsigned             stride;    /**< 한 줄 바이트 수(= width × 4)        */
    pjmedia_format_id    fmt_id;    /**< PJMEDIA_FORMAT_BGRA                 */
    pj_timestamp         ts;        /**< 프레임 시각(영상 클럭 90 kHz)       */
} pjmedia_cims_frame;

/**
 * 프레임 콜백 — 렌더 스트림의 put_frame 에서(영상 회의 브리지 클럭 스레드) 곧바로 불린다. 오래 붙잡지 말 것
 * (복사하고 돌아간다). token = 그 렌더러에 걸린 창 핸들(handle.window).
 */
typedef void (*pjmedia_cims_frame_cb)(void *user_data, void *token,
                                      const pjmedia_cims_frame *frame);

/**
 * 프레임 콜백 등록(NULL = 해제). 렌더 스트림이 하나도 없을 때 부른다 — 기동 직후(호가 생기기 전)·종료 뒤(라이브러리
 * 파괴 뒤). 스트림이 도는 동안의 켜고 끄기는 창 토큰(NULL = 버림)으로 한다 — 그래서 전달 경로에 잠금이 없다.
 */
PJ_DECL(void) pjmedia_cims_frame_dev_set_callback(pjmedia_cims_frame_cb cb,
                                                  void *user_data);

PJ_END_DECL

#endif /* __PJMEDIA_VIDEODEV_CIMS_FRAME_DEV_H__ */
