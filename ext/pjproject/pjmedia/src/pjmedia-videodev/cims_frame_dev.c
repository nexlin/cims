/*
 * CIMS — 창 없는 프레임 콜백 렌더 장치 (cims_frame_dev.h 참고, ue_sdk.md §4.5·§6.1 «영상»).
 *
 * 렌더 전용 장치 하나("CIMS frame sink", driver "CIMS"). 받는 형식은 BGRA 하나라 vid_port 가 디코더 출력(I420)을
 * 변환기(libyuv)로 바꿔 put_frame 에 넘긴다. put_frame 은 프레임을 콜백으로 넘기기만 한다 — 창·그리기·크기 조정은
 * 앱(WPF WriteableBitmap)의 몫이다. 창 핸들(OUTPUT_WINDOW)은 앱이 고른 토큰이고 장치는 그 값을 해석하지 않는다.
 */
#include <pjmedia-videodev/videodev_imp.h>
#include <pjmedia-videodev/cims_frame_dev.h>
#include <pj/assert.h>
#include <pj/log.h>
#include <pj/pool.h>
#include <pj/string.h>

#if defined(PJMEDIA_HAS_VIDEO) && PJMEDIA_HAS_VIDEO != 0 && \
    defined(PJMEDIA_VIDEO_DEV_HAS_CIMS_FRAME) && \
    PJMEDIA_VIDEO_DEV_HAS_CIMS_FRAME != 0

#define THIS_FILE               "cims_frame_dev.c"
#define DEFAULT_CLOCK_RATE      90000
#define DEFAULT_WIDTH           640
#define DEFAULT_HEIGHT          480
#define DEFAULT_FPS             15

/* 등록된 콜백 — 렌더 스트림이 없을 때만 바뀐다(헤더 계약) */
static pjmedia_cims_frame_cb  g_cb;
static void                  *g_user;

struct cims_factory
{
    pjmedia_vid_dev_factory      base;
    pj_pool_t                   *pool;
    pj_pool_factory             *pf;
    pjmedia_vid_dev_info         info;
};

struct cims_stream
{
    pjmedia_vid_dev_stream       base;
    pjmedia_vid_dev_param        param;
    pj_pool_t                   *pool;
    pj_bool_t                    running;
};

static pj_status_t cims_factory_init(pjmedia_vid_dev_factory *f);
static pj_status_t cims_factory_destroy(pjmedia_vid_dev_factory *f);
static pj_status_t cims_factory_refresh(pjmedia_vid_dev_factory *f);
static unsigned    cims_factory_get_dev_count(pjmedia_vid_dev_factory *f);
static pj_status_t cims_factory_get_dev_info(pjmedia_vid_dev_factory *f,
                                             unsigned index,
                                             pjmedia_vid_dev_info *info);
static pj_status_t cims_factory_default_param(pj_pool_t *pool,
                                              pjmedia_vid_dev_factory *f,
                                              unsigned index,
                                              pjmedia_vid_dev_param *param);
static pj_status_t cims_factory_create_stream(pjmedia_vid_dev_factory *f,
                                              pjmedia_vid_dev_param *param,
                                              const pjmedia_vid_dev_cb *cb,
                                              void *user_data,
                                              pjmedia_vid_dev_stream **p);

static pj_status_t cims_stream_get_param(pjmedia_vid_dev_stream *s,
                                         pjmedia_vid_dev_param *param);
static pj_status_t cims_stream_get_cap(pjmedia_vid_dev_stream *s,
                                       pjmedia_vid_dev_cap cap, void *value);
static pj_status_t cims_stream_set_cap(pjmedia_vid_dev_stream *s,
                                       pjmedia_vid_dev_cap cap,
                                       const void *value);
static pj_status_t cims_stream_put_frame(pjmedia_vid_dev_stream *s,
                                         const pjmedia_frame *frame);
static pj_status_t cims_stream_start(pjmedia_vid_dev_stream *s);
static pj_status_t cims_stream_stop(pjmedia_vid_dev_stream *s);
static pj_status_t cims_stream_destroy(pjmedia_vid_dev_stream *s);

static pjmedia_vid_dev_factory_op factory_op =
{
    &cims_factory_init,
    &cims_factory_destroy,
    &cims_factory_get_dev_count,
    &cims_factory_get_dev_info,
    &cims_factory_default_param,
    &cims_factory_create_stream,
    &cims_factory_refresh
};

static pjmedia_vid_dev_stream_op stream_op =
{
    &cims_stream_get_param,
    &cims_stream_get_cap,
    &cims_stream_set_cap,
    &cims_stream_start,
    NULL,
    &cims_stream_put_frame,
    &cims_stream_stop,
    &cims_stream_destroy
};

PJ_DEF(void) pjmedia_cims_frame_dev_set_callback(pjmedia_cims_frame_cb cb,
                                                 void *user_data)
{
    g_user = user_data;
    g_cb = cb;
}

/* videodev.c 가 등록한다 */
pjmedia_vid_dev_factory* pjmedia_cims_frame_factory(pj_pool_factory *pf)
{
    struct cims_factory *f;
    pj_pool_t *pool;

    pool = pj_pool_create(pf, "cims frame", 1000, 1000, NULL);
    f = PJ_POOL_ZALLOC_T(pool, struct cims_factory);
    f->pf = pf;
    f->pool = pool;
    f->base.op = &factory_op;
    return &f->base;
}

static pj_status_t cims_factory_init(pjmedia_vid_dev_factory *f)
{
    struct cims_factory *cf = (struct cims_factory*)f;
    pjmedia_vid_dev_info *di = &cf->info;

    pj_bzero(di, sizeof(*di));
    pj_ansi_strxcpy(di->name, "CIMS frame sink", sizeof(di->name));
    pj_ansi_strxcpy(di->driver, "CIMS", sizeof(di->driver));
    di->dir = PJMEDIA_DIR_RENDER;
    di->has_callback = PJ_FALSE;            /* 수동 — vid_port 가 put_frame 으로 민다 */
    di->caps = PJMEDIA_VID_DEV_CAP_FORMAT |
               PJMEDIA_VID_DEV_CAP_OUTPUT_WINDOW |
               PJMEDIA_VID_DEV_CAP_OUTPUT_HIDE |
               PJMEDIA_VID_DEV_CAP_OUTPUT_RESIZE |
               PJMEDIA_VID_DEV_CAP_OUTPUT_WINDOW_FLAGS;
    di->fmt_cnt = 1;
    pjmedia_format_init_video(&di->fmt[0], PJMEDIA_FORMAT_BGRA,
                              DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_FPS, 1);

    PJ_LOG(4, (THIS_FILE, "CIMS frame sink initialized (BGRA)"));
    return PJ_SUCCESS;
}

static pj_status_t cims_factory_destroy(pjmedia_vid_dev_factory *f)
{
    struct cims_factory *cf = (struct cims_factory*)f;
    pj_pool_safe_release(&cf->pool);
    return PJ_SUCCESS;
}

static pj_status_t cims_factory_refresh(pjmedia_vid_dev_factory *f)
{
    PJ_UNUSED_ARG(f);
    return PJ_SUCCESS;
}

static unsigned cims_factory_get_dev_count(pjmedia_vid_dev_factory *f)
{
    PJ_UNUSED_ARG(f);
    return 1;
}

static pj_status_t cims_factory_get_dev_info(pjmedia_vid_dev_factory *f,
                                             unsigned index,
                                             pjmedia_vid_dev_info *info)
{
    struct cims_factory *cf = (struct cims_factory*)f;

    PJ_ASSERT_RETURN(index == 0, PJMEDIA_EVID_INVDEV);
    pj_memcpy(info, &cf->info, sizeof(*info));
    return PJ_SUCCESS;
}

static pj_status_t cims_factory_default_param(pj_pool_t *pool,
                                              pjmedia_vid_dev_factory *f,
                                              unsigned index,
                                              pjmedia_vid_dev_param *param)
{
    struct cims_factory *cf = (struct cims_factory*)f;

    PJ_ASSERT_RETURN(index == 0, PJMEDIA_EVID_INVDEV);
    PJ_UNUSED_ARG(pool);

    pj_bzero(param, sizeof(*param));
    param->dir = PJMEDIA_DIR_RENDER;
    param->rend_id = index;
    param->cap_id = PJMEDIA_VID_INVALID_DEV;
    param->flags = PJMEDIA_VID_DEV_CAP_FORMAT;
    param->clock_rate = DEFAULT_CLOCK_RATE;
    pj_memcpy(&param->fmt, &cf->info.fmt[0], sizeof(param->fmt));
    return PJ_SUCCESS;
}

static pj_status_t cims_factory_create_stream(pjmedia_vid_dev_factory *f,
                                              pjmedia_vid_dev_param *param,
                                              const pjmedia_vid_dev_cb *cb,
                                              void *user_data,
                                              pjmedia_vid_dev_stream **p)
{
    struct cims_factory *cf = (struct cims_factory*)f;
    struct cims_stream *strm;
    pj_pool_t *pool;

    PJ_UNUSED_ARG(cb);
    PJ_UNUSED_ARG(user_data);
    PJ_ASSERT_RETURN(param->dir == PJMEDIA_DIR_RENDER, PJ_EINVAL);
    if (param->fmt.id != PJMEDIA_FORMAT_BGRA)
        return PJMEDIA_EVID_BADFORMAT;

    pool = pj_pool_create(cf->pf, "cims frame strm", 512, 512, NULL);
    PJ_ASSERT_RETURN(pool != NULL, PJ_ENOMEM);
    strm = PJ_POOL_ZALLOC_T(pool, struct cims_stream);
    strm->pool = pool;
    pj_memcpy(&strm->param, param, sizeof(*param));
    /* 창 핸들(토큰)은 생성 인자로 와도(OUTPUT_WINDOW) 그대로 둔다 — 아니면 NULL(버림) */
    if (!(param->flags & PJMEDIA_VID_DEV_CAP_OUTPUT_WINDOW))
        pj_bzero(&strm->param.window, sizeof(strm->param.window));
    strm->base.op = &stream_op;
    *p = &strm->base;
    return PJ_SUCCESS;
}

static pj_status_t cims_stream_get_param(pjmedia_vid_dev_stream *s,
                                         pjmedia_vid_dev_param *pi)
{
    struct cims_stream *strm = (struct cims_stream*)s;

    PJ_ASSERT_RETURN(strm && pi, PJ_EINVAL);
    pj_memcpy(pi, &strm->param, sizeof(*pi));
    pi->flags |= PJMEDIA_VID_DEV_CAP_OUTPUT_WINDOW |
                 PJMEDIA_VID_DEV_CAP_OUTPUT_HIDE |
                 PJMEDIA_VID_DEV_CAP_OUTPUT_RESIZE;
    pi->disp_size = strm->param.fmt.det.vid.size;
    return PJ_SUCCESS;
}

static pj_status_t cims_stream_get_cap(pjmedia_vid_dev_stream *s,
                                       pjmedia_vid_dev_cap cap, void *pval)
{
    struct cims_stream *strm = (struct cims_stream*)s;

    PJ_ASSERT_RETURN(strm && pval, PJ_EINVAL);
    switch (cap) {
    case PJMEDIA_VID_DEV_CAP_FORMAT:
        pj_memcpy(pval, &strm->param.fmt, sizeof(strm->param.fmt));
        return PJ_SUCCESS;
    case PJMEDIA_VID_DEV_CAP_OUTPUT_WINDOW:
        pj_memcpy(pval, &strm->param.window, sizeof(strm->param.window));
        return PJ_SUCCESS;
    case PJMEDIA_VID_DEV_CAP_OUTPUT_HIDE:
        *(pj_bool_t*)pval = strm->param.window_hide;
        return PJ_SUCCESS;
    case PJMEDIA_VID_DEV_CAP_OUTPUT_RESIZE:
        *(pjmedia_rect_size*)pval = strm->param.fmt.det.vid.size;
        return PJ_SUCCESS;
    case PJMEDIA_VID_DEV_CAP_OUTPUT_WINDOW_FLAGS:
        *(unsigned*)pval = strm->param.window_flags;
        return PJ_SUCCESS;
    default:
        return PJMEDIA_EVID_INVCAP;
    }
}

static pj_status_t cims_stream_set_cap(pjmedia_vid_dev_stream *s,
                                       pjmedia_vid_dev_cap cap,
                                       const void *pval)
{
    struct cims_stream *strm = (struct cims_stream*)s;

    PJ_ASSERT_RETURN(strm && pval, PJ_EINVAL);
    switch (cap) {
    case PJMEDIA_VID_DEV_CAP_FORMAT: {
        /* 해상도 변경(상대 카메라 회전·인코더 크기 변경) — vid_port 가 변환기를 새 크기로 다시 만든 뒤 알린다 */
        const pjmedia_format *fmt = (const pjmedia_format*)pval;
        if (fmt->id != PJMEDIA_FORMAT_BGRA)
            return PJMEDIA_EVID_BADFORMAT;
        pj_memcpy(&strm->param.fmt, fmt, sizeof(*fmt));
        return PJ_SUCCESS;
    }
    case PJMEDIA_VID_DEV_CAP_OUTPUT_WINDOW:
        /* 토큰만 기억한다 — 다음 put_frame 부터 이 값으로 넘긴다 */
        pj_memcpy(&strm->param.window, pval, sizeof(strm->param.window));
        return PJ_SUCCESS;
    case PJMEDIA_VID_DEV_CAP_OUTPUT_HIDE:
        strm->param.window_hide = *(const pj_bool_t*)pval;
        return PJ_SUCCESS;
    case PJMEDIA_VID_DEV_CAP_OUTPUT_RESIZE:
        return PJ_SUCCESS;                  /* 표시 크기는 앱이 정한다 */
    case PJMEDIA_VID_DEV_CAP_OUTPUT_WINDOW_FLAGS:
        strm->param.window_flags = *(const unsigned*)pval;
        return PJ_SUCCESS;
    default:
        return PJMEDIA_EVID_INVCAP;
    }
}

static pj_status_t cims_stream_put_frame(pjmedia_vid_dev_stream *s,
                                         const pjmedia_frame *frame)
{
    struct cims_stream *strm = (struct cims_stream*)s;
    pjmedia_cims_frame_cb cb = g_cb;
    void *user = g_user;
    void *token = strm->param.window.info.window;
    const pjmedia_rect_size *sz = &strm->param.fmt.det.vid.size;
    pjmedia_cims_frame f;

    if (!strm->running || !cb || !token || strm->param.window_hide)
        return PJ_SUCCESS;
    if (frame->type != PJMEDIA_FRAME_TYPE_VIDEO || !frame->buf || !sz->w || !sz->h)
        return PJ_SUCCESS;
    if (frame->size < (pj_size_t)sz->w * sz->h * 4)
        return PJ_SUCCESS;                  /* 변환 전 크기 전환 사이의 한 장 — 버린다 */

    f.data = frame->buf;
    f.width = sz->w;
    f.height = sz->h;
    f.stride = sz->w * 4;
    f.size = (pj_size_t)f.stride * f.height;
    f.fmt_id = PJMEDIA_FORMAT_BGRA;
    f.ts = frame->timestamp;
    (*cb)(user, token, &f);
    return PJ_SUCCESS;
}

static pj_status_t cims_stream_start(pjmedia_vid_dev_stream *s)
{
    struct cims_stream *strm = (struct cims_stream*)s;
    strm->running = PJ_TRUE;
    return PJ_SUCCESS;
}

static pj_status_t cims_stream_stop(pjmedia_vid_dev_stream *s)
{
    struct cims_stream *strm = (struct cims_stream*)s;
    strm->running = PJ_FALSE;
    return PJ_SUCCESS;
}

static pj_status_t cims_stream_destroy(pjmedia_vid_dev_stream *s)
{
    struct cims_stream *strm = (struct cims_stream*)s;

    PJ_ASSERT_RETURN(strm != NULL, PJ_EINVAL);
    cims_stream_stop(s);
    pj_pool_release(strm->pool);
    return PJ_SUCCESS;
}

#endif  /* PJMEDIA_VIDEO_DEV_HAS_CIMS_FRAME */
