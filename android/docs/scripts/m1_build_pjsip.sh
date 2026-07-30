#!/bin/bash
# M1.0-2/3/4: pjproject 2.16 clone → config_site.h(경로 C) → configure-android → make → SWIG
# 전제: ~/.m1env (m1_provision.sh 산출)
set -e -o pipefail   # pipefail: `make | tail` 파이프가 make 실패를 가리지 않도록
source ~/.m1env

echo "=== [2-1] pjproject 2.16 clone ==="
cd ~
if [ ! -d ~/pjproject/.git ]; then
  git clone --depth 1 --branch 2.16 https://github.com/pjsip/pjproject.git
fi
cd ~/pjproject
git describe --tags || git rev-parse --short HEAD
# And-Media 오디오 소스 실존 확인 (경로 C 전제)
test -f pjmedia/src/pjmedia-codec/and_aud_mediacodec.cpp && echo "and_aud_mediacodec.cpp OK"

echo "=== [2-2] config_site.h (경로 C — M1 설계서 §2.5 확정본) ==="
cat > pjlib/include/pj/config_site.h <<'EOF'
/* CIMS UE M1 — config_site.h (경로 C: And-Media/MediaCodec 음성+영상) */

/* 1) Android 표준 설정 — 오버라이드보다 먼저 */
#define PJ_CONFIG_ANDROID 1
#include <pj/config_site_sample.h>

/* 2) 영상: And-Media H264, 협상표면 축소 위해 VP8/VP9 제외 */
#define PJMEDIA_HAS_VIDEO              1
#define PJMEDIA_HAS_AND_MEDIA_H264     1
#define PJMEDIA_HAS_AND_MEDIA_VP8      0
#define PJMEDIA_HAS_AND_MEDIA_VP9      0

/* 3) 음성: And-Media AMR-WB 정본, opencore 빌드 제외(중복 등록 방지) */
#define PJMEDIA_HAS_AND_MEDIA_AMRWB    1
#define PJMEDIA_HAS_AND_MEDIA_AMRNB    0
#define PJMEDIA_HAS_OPENCORE_AMRWB_CODEC  0
#define PJMEDIA_HAS_OPENCORE_AMRNB_CODEC  0

/* 4) 내장 SW 음성코덱 최소화 (G.711 은 안전망 유지) */
#define PJMEDIA_HAS_G711_CODEC   1
#define PJMEDIA_HAS_L16_CODEC    0
#define PJMEDIA_HAS_GSM_CODEC    0
#define PJMEDIA_HAS_SPEEX_CODEC  0
#define PJMEDIA_HAS_ILBC_CODEC   0
#define PJMEDIA_HAS_G722_CODEC   0

/* 5) M4 전 보안전송 off (UDP only) */
#define PJMEDIA_HAS_SRTP          0
#define PJSIP_HAS_TLS_TRANSPORT   0

/* 6) NAT: RTP keepalive(empty RTP) — 청취 전용(무송신) 구간에도 주기 송신해
      하향 NAT 매핑·CMP latch 유지 (ue_nat_traversal.md §7.1). 주기=PJMEDIA_STREAM_KA_INTERVAL(기본 5s) */
#define PJMEDIA_STREAM_ENABLE_KA  1
EOF
sha256sum pjlib/include/pj/config_site.h

echo "=== [2-3] AMR-WB codec_setting NULL 크래시 패치 (upstream 2.16 버그) ==="
# and_aud_mediacodec.cpp: AMR 설정 초기화 블록(codec_id==AMRNB||AMRWB 처리)이
# `#if PJMEDIA_HAS_AND_MEDIA_AMRNB` 로만 가드돼, AMRNB=0/AMRWB=1(경로 C) 빌드에서
# 통째로 컴파일 제외 → codec_data->codec_setting=NULL → 첫 RX/TX 미디어 프레임에서
# parse_amr/pack_amr 이 NULL amr_settings_t 역참조(SIGSEGV). 가드를 AMRWB 포함으로 확대.
# 멱등: 이미 패치돼 있으면 skip.
python3 - <<'PYEOF'
import re
p = "pjmedia/src/pjmedia-codec/and_aud_mediacodec.cpp"
src = open(p).read()
anchor = ("#if PJMEDIA_HAS_AND_MEDIA_AMRNB\n"
          "    if (and_media_data->codec_id == AND_AUD_CODEC_AMRNB ||\n"
          "        and_media_data->codec_id == AND_AUD_CODEC_AMRWB)")
fixed  = ("#if PJMEDIA_HAS_AND_MEDIA_AMRNB || PJMEDIA_HAS_AND_MEDIA_AMRWB\n"
          "    if (and_media_data->codec_id == AND_AUD_CODEC_AMRNB ||\n"
          "        and_media_data->codec_id == AND_AUD_CODEC_AMRWB)")
if fixed in src:
    print("  already patched (skip)")
elif anchor in src:
    open(p, "w").write(src.replace(anchor, fixed, 1))
    print("  patched: AMR settings guard -> AMRNB || AMRWB")
else:
    raise SystemExit("  ERROR: AMR guard anchor not found (pjproject layout changed?)")
PYEOF

echo "=== [2-4] H.264 I-frame(IDR) 주기 2초 패치 ==="
# and_vid_mediacodec.cpp: 인코더 i-frame-interval(초)이 KEYFRAME_INTERVAL 매크로에 하드코딩(기본 1초).
# 대역폭/복구 균형 위해 2초로. (and_media_codec 디스크립터의 keyframe_interval 필드는 미사용 dead field
# 이므로 매크로 변경의 유일한 실효과는 인코더 IFR_INTERVAL.) 멱등.
python3 - <<'PYEOF'
p = "pjmedia/src/pjmedia-codec/and_vid_mediacodec.cpp"
src = open(p).read()
if "#define KEYFRAME_INTERVAL       2" in src:
    print("  already patched (skip)")
elif "#define KEYFRAME_INTERVAL       1" in src:
    open(p, "w").write(src.replace("#define KEYFRAME_INTERVAL       1",
                                    "#define KEYFRAME_INTERVAL       2", 1))
    print("  patched: KEYFRAME_INTERVAL 1 -> 2 (2s IDR)")
else:
    raise SystemExit("  ERROR: KEYFRAME_INTERVAL anchor not found (pjproject layout changed?)")
PYEOF

echo "=== [2-5] H.264 발신 비트레이트 상한 500kbps + CBR 패치 ==="
# and_vid_mediacodec.cpp: ①인코더 BIT_RATE 를 500kbps 로 캡(PJSIP 협상 시 해상도 기반 재계산으로
# 과도, 480x640@15 → ~920kbps). ②CBR 모드 — Android min-quality(VQApply)가 VBR 저비트레이트를
# 품질 floor(921kbps)로 강제 상향하는 것을 방지(목표 비트레이트 준수). 멱등.
python3 - <<'PYEOF'
p = "pjmedia/src/pjmedia-codec/and_vid_mediacodec.cpp"
src = open(p).read()
anchor = ("    AMediaFormat_setInt32(vid_fmt, AND_MEDIA_KEY_BIT_RATE,\n"
          "                          param->enc_fmt.det.vid.avg_bps);")
fixed  = ("    {\n"
          "        pj_uint32_t cims_br = param->enc_fmt.det.vid.avg_bps;\n"
          "        if (cims_br == 0 || cims_br > 500000) cims_br = 500000;\n"
          "        AMediaFormat_setInt32(vid_fmt, AND_MEDIA_KEY_BIT_RATE, cims_br);\n"
          "        AMediaFormat_setInt32(vid_fmt, \"bitrate-mode\", 2 /* BITRATE_MODE_CBR */);\n"
          "    }")
if "cims_br" in src:
    print("  already patched (skip)")
elif anchor in src:
    open(p, "w").write(src.replace(anchor, fixed, 1))
    print("  patched: BIT_RATE cap 500kbps + CBR")
else:
    raise SystemExit("  ERROR: BIT_RATE anchor not found (pjproject layout changed?)")
PYEOF

echo "=== [2-6] pjsua2 StreamInfo::fromPj NULL codec-param 크래시 패치 ==="
# call.cpp StreamInfo::fromPj 가 stream_info 의 param(오디오)/codec_param(비디오)을 무가드
# 역참조한다. pjsua 자체 주석(pjsua_media.c)이 "param can be NULL if the stream is
# rejected or disabled" 라고 명시 — SDP 협상 결과에 따라 NULL 이면 on_stream_precreate
# JNI 업콜에서 SIGSEGV(협상 PT 가 로컬 기본 PT 와 다른 조합 등). NULL 이면 복사 생략
# (pjsua2 기본값 유지, 스트림 생성부는 원래 NULL param 폴백 있음). 멱등.
python3 - <<'PYEOF'
p = "pjsip/src/pjsua2/call.cpp"
src = open(p).read()
a_aud = "        audCodecParam.fromPj(*info.info.aud.param);"
f_aud = ("        if (info.info.aud.param)\n"
         "            audCodecParam.fromPj(*info.info.aud.param);")
a_vid = "        vidCodecParam.fromPj(*info.info.vid.codec_param);"
f_vid = ("        if (info.info.vid.codec_param)\n"
         "            vidCodecParam.fromPj(*info.info.vid.codec_param);")
changed = False
if f_aud in src:
    print("  aud: already patched (skip)")
elif a_aud in src:
    src = src.replace(a_aud, f_aud, 1); changed = True
    print("  patched: StreamInfo::fromPj aud param NULL guard")
else:
    raise SystemExit("  ERROR: aud param anchor not found (pjproject layout changed?)")
if f_vid in src:
    print("  vid: already patched (skip)")
elif a_vid in src:
    src = src.replace(a_vid, f_vid, 1); changed = True
    print("  patched: StreamInfo::fromPj vid codec_param NULL guard")
else:
    raise SystemExit("  ERROR: vid codec_param anchor not found (pjproject layout changed?)")
if changed:
    open(p, "w").write(src)
PYEOF

echo "=== [2-7] stream_info.c si->param zero-init 패치 ==="
# get_audio_codec_info_param: si->param 을 ALLOC(비초기화)한 뒤 get_default_param 실패여도
# dir==NONE 이면 성공 반환하는 경로가 있어 param 이 쓰레기값으로 남을 수 있다 → ZALLOC. 멱등.
python3 - <<'PYEOF'
p = "pjmedia/src/pjmedia/stream_info.c"
src = open(p).read()
anchor = "si->param = PJ_POOL_ALLOC_T(pool, pjmedia_codec_param);"
fixed  = "si->param = PJ_POOL_ZALLOC_T(pool, pjmedia_codec_param);"
if fixed in src:
    print("  already patched (skip)")
elif anchor in src:
    open(p, "w").write(src.replace(anchor, fixed, 1))
    print("  patched: si->param ALLOC -> ZALLOC")
else:
    raise SystemExit("  ERROR: si->param alloc anchor not found (pjproject layout changed?)")
PYEOF

echo "=== [2-8] pjsua_txt 비-RTP m=text 슬롯 스트림 생성 스킵 패치 ==="
# 앱이 pjsua 의 m=text 슬롯을 m=application(UDP MCPTT, floor)으로 in-place 교체하면
# pjmedia_txt_stream_info_from_sdp 는 비 RTP transport 라 stream info 를 비운 채(!active)
# 성공 반환하는데, pjsua_txt_channel_update 게이트는 포트!=0 만 봐서 빈 info 로
# on_stream_precreate(SIGABRT: AF=0 주소 print)+스트림 생성까지 진행한다.
# 협상 로컬 m= 라인이 RTP 기반일 때만 text 채널을 만들도록 게이트 강화. 멱등.
python3 - <<'PYEOF'
p = "pjsip/src/pjsua-lib/pjsua_txt.c"
src = open(p).read()
anchor = ("    /* Check if no media is active */\n"
          "    if (local_sdp->media[strm_idx]->desc.port != 0) {")
fixed  = ("    /* Check if no media is active.\n"
          "     * CIMS: RTP 기반으로 협상된 text 스트림만 진행 — 비 RTP application m= 라인\n"
          "     * (floor 슬롯 재사용)은 stream info 가 비어 있어(!active 조기성공) 그대로 진행 시\n"
          "     * on_stream_precreate 에서 AF=0 주소 print abort / 빈 info 스트림 생성이 된다.\n"
          "     */\n"
          "    if (local_sdp->media[strm_idx]->desc.port != 0 &&\n"
          "        PJMEDIA_TP_PROTO_HAS_FLAG(si->proto, PJMEDIA_TP_PROTO_RTP_AVP)) {")
if fixed in src:
    print("  already patched (skip)")
elif anchor in src:
    open(p, "w").write(src.replace(anchor, fixed, 1))
    print("  patched: txt channel gate += RTP_AVP proto check")
else:
    raise SystemExit("  ERROR: txt gate anchor not found (pjproject layout changed?)")
PYEOF

echo "=== [2-9] pjsua2 StreamInfo::fromPj sockaddr print AF 가드 패치 ==="
# fromPj 가 rem_addr/rem_rtcp 를 무가드 pj_sockaddr_print — 주소 미설정(sa_family=0,
# 협상 실패/비 RTP 슬롯)이면 pj_sockaddr_get_addr assert 로 SIGABRT. AF 확인 헬퍼로 치환. 멱등.
python3 - <<'PYEOF'
import re
p = "pjsip/src/pjsua2/call.cpp"
src = open(p).read()
helper = (
"static void cims_print_sockaddr_safe(const pj_sockaddr *a, char *buf, unsigned len)\n"
"{\n"
"    buf[0] = '\\0';\n"
"    if (a->addr.sa_family == PJ_AF_INET || a->addr.sa_family == PJ_AF_INET6)\n"
"        pj_sockaddr_print(a, buf, len, 3);\n"
"}\n\n")
if "cims_print_sockaddr_safe" in src:
    print("  already patched (skip)")
else:
    anchor = "void StreamInfo::fromPj(const pjsua_stream_info &info)"
    if anchor not in src:
        raise SystemExit("  ERROR: fromPj anchor not found")
    src = src.replace(anchor, helper + anchor, 1)
    src, n = re.subn(
        r"pj_sockaddr_print\((&info\.info\.(?:aud|vid|txt)\.rem_(?:addr|rtcp)), straddr, sizeof\(straddr\), 3\);",
        r"cims_print_sockaddr_safe(\1, straddr, sizeof(straddr));",
        src)
    if n < 6:
        raise SystemExit("  ERROR: expected >=6 sockaddr_print sites, got %d" % n)
    open(p, "w").write(src)
    print("  patched: fromPj sockaddr print guard x%d" % n)
PYEOF

echo "=== [2-10] CIMS 무전/통화 분리 라우팅 패치 (android_jni_dev OUTPUT_ROUTE) ==="
# 재생 트랙 단위 출력 장치 강제: OUTPUT_ROUTE 캡 구현(AudioTrack.setPreferredDevice) +
# 라우트 캡 사용 앱(PTT)은 STREAM_MUSIC 생성(통화 중 voice 전략은 per-track 지정 무시 실측) +
# get_cap(keep 저장 보존). VoLTE 통화(수화기)와 PTT 무전(스피커) 동시 분리 라우팅의 전제.
# 멱등: 이미 패치돼 있으면 skip.
if grep -q set_track_preferred_device pjmedia/src/pjmedia-audiodev/android_jni_dev.c; then
  echo "  already patched (skip)"
else
  git apply <<'CIMS_ROUTE_PATCH_EOF'
diff --git a/pjmedia/src/pjmedia-audiodev/android_jni_dev.c b/pjmedia/src/pjmedia-audiodev/android_jni_dev.c
index 65a2cc0..45bdec5 100644
--- a/pjmedia/src/pjmedia-audiodev/android_jni_dev.c
+++ b/pjmedia/src/pjmedia-audiodev/android_jni_dev.c
@@ -410,10 +410,14 @@ static pj_status_t android_get_dev_info(pjmedia_aud_dev_factory *f,
     pj_ansi_strxcpy(info->driver, DRIVER_NAME, sizeof(info->driver));
     info->default_samples_per_sec = 8000;
     info->caps = PJMEDIA_AUD_DEV_CAP_OUTPUT_VOLUME_SETTING |
+                 PJMEDIA_AUD_DEV_CAP_OUTPUT_ROUTE |
                  PJMEDIA_AUD_DEV_CAP_INPUT_SOURCE;
     info->input_count = 1;
     info->output_count = 1;
-    info->routes = PJMEDIA_AUD_DEV_ROUTE_CUSTOM;
+    info->routes = PJMEDIA_AUD_DEV_ROUTE_CUSTOM |
+                   PJMEDIA_AUD_DEV_ROUTE_DEFAULT |
+                   PJMEDIA_AUD_DEV_ROUTE_LOUDSPEAKER |
+                   PJMEDIA_AUD_DEV_ROUTE_EARPIECE;
     
     return PJ_SUCCESS;
 }
@@ -712,7 +716,18 @@ static pj_status_t android_create_stream(pjmedia_aud_dev_factory *f,
     if (stream->dir & PJMEDIA_DIR_PLAYBACK) {
         jthrowable exc;
         jobject track_obj;
-        
+        int stream_type;
+
+        /* CIMS: 출력 라우트 캡을 쓰는 앱(PTT — setOutputRoute keep 저장 시 생성 param 에
+         * 캡 플래그가 병합됨)은 STREAM_MUSIC 으로 생성 — 통화(voice) 전략 트랙은 통화 중
+         * per-track preferred device 가 정책에 무시되어(실측: MTK/QC 공통) 트랙 단위 분리
+         * 라우팅이 불가하지만, 미디어 전략 트랙은 존중된다. 통화 앱(volte)은 라우트 캡을
+         * 쓰지 않으므로 종전대로 STREAM_VOICE_CALL. */
+        if (param->flags & PJMEDIA_AUD_DEV_CAP_OUTPUT_ROUTE)
+            stream_type = 3;    /* STREAM_MUSIC */
+        else
+            stream_type = 0;    /* STREAM_VOICE_CALL */
+
         /* Get pointer to the constructor */
         constructor_method = (*jni_env)->GetMethodID(jni_env,
                                                      stream->track_class,
@@ -722,11 +737,13 @@ static pj_status_t android_create_stream(pjmedia_aud_dev_factory *f,
             status = PJMEDIA_EAUD_SYSERR;
             goto on_error;
         }
-        
+
+        PJ_LOG(4, (THIS_FILE, "Creating audio track, stream type: %d",
+                   stream_type));
         track_obj = (*jni_env)->NewObject(jni_env,
                                           stream->track_class,
                                           constructor_method,
-                                          0, /* STREAM_VOICE_CALL */
+                                          stream_type,
                                           param->clock_rate,
                                           channelOutCfg,
                                           sampleFormat,
@@ -834,17 +851,139 @@ static pj_status_t strm_get_cap(pjmedia_aud_stream *s,
 {
     struct android_aud_stream *strm = (struct android_aud_stream*)s;
     pj_status_t status = PJMEDIA_EAUD_INVCAP;
-    
+
     PJ_ASSERT_RETURN(s && pval, PJ_EINVAL);
-    
+
+    /* CIMS: 출력 라우트 조회 — pjsua 의 update_initial_aud_param 이 open 후 이 get 으로
+     * 저장 캡을 재구성하므로, 미구현이면 keep 저장된 라우트 플래그가 지워져 다음 open 의
+     * 생성 param 에서 사라진다(STREAM_MUSIC 분기·재적용 모두 무효화). */
+    if (cap==PJMEDIA_AUD_DEV_CAP_OUTPUT_ROUTE &&
+        (strm->param.dir & PJMEDIA_DIR_PLAYBACK))
+    {
+        *(pjmedia_aud_dev_route*)pval = strm->param.output_route;
+        return PJ_SUCCESS;
+    }
+
     if (cap==PJMEDIA_AUD_DEV_CAP_OUTPUT_VOLUME_SETTING &&
         (strm->param.dir & PJMEDIA_DIR_PLAYBACK))
     {
     }
-    
+
     return status;
 }
 
+/* CIMS: 재생 AudioTrack 단위 출력 장치 강제 (AudioTrack.setPreferredDevice).
+ * 전역 통신 라우트(AudioManager communication device)와 독립적으로 이 스트림의
+ * 출력만 지정한다 — VoLTE 통화(수화기)와 PTT 무전(스피커) 동시 분리 라우팅용.
+ * want_type: AudioDeviceInfo.TYPE_*(1=수화기, 2=스피커), 0=해제(전역 정책 추종).
+ * 컨텍스트는 ActivityThread.currentApplication() 으로 획득(디바이스 열거용). */
+static pj_bool_t set_track_preferred_device(JNIEnv *env, jobject track,
+                                            int want_type)
+{
+    jclass track_cls = NULL, at_cls = NULL, app_cls = NULL, am_cls = NULL;
+    jobject app = NULL, am = NULL, dev = NULL;
+    jobjectArray devs = NULL;
+    jmethodID set_dev;
+    jboolean jret = JNI_FALSE;
+    pj_bool_t ok = PJ_FALSE;
+
+    track_cls = (*env)->GetObjectClass(env, track);
+    set_dev = (*env)->GetMethodID(env, track_cls, "setPreferredDevice",
+                                  "(Landroid/media/AudioDeviceInfo;)Z");
+    if (!set_dev) goto on_return;
+
+    /* 이미 원하는 장치로 라우팅 중이면 no-op — 주기 재적용(앱 ticker)이 라우트
+     * 플래핑(재생 끊김)을 만들지 않게 한다. 이탈 시에만 아래에서 해제→재설정
+     * 바운스로 재평가를 강제한다(같은 값 재설정은 정책이 무시). */
+    if (want_type != 0) {
+        jmethodID get_routed = (*env)->GetMethodID(env, track_cls,
+            "getRoutedDevice", "()Landroid/media/AudioDeviceInfo;");
+        if (get_routed) {
+            jobject routed = (*env)->CallObjectMethod(env, track, get_routed);
+            if (routed) {
+                jclass r_cls = (*env)->GetObjectClass(env, routed);
+                jmethodID gt = (*env)->GetMethodID(env, r_cls, "getType", "()I");
+                jint t = gt ? (*env)->CallIntMethod(env, routed, gt) : -1;
+                (*env)->DeleteLocalRef(env, r_cls);
+                (*env)->DeleteLocalRef(env, routed);
+                if (t == want_type) {
+                    ok = PJ_TRUE;
+                    goto on_return;
+                }
+            }
+        }
+        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
+        /* 이탈 상태 — 핀 해제 후 재설정(바운스) */
+        (*env)->CallBooleanMethod(env, track, set_dev, NULL);
+        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
+    }
+
+    if (want_type != 0) {
+        jmethodID cur_app, get_sys, get_devs;
+        jstring svc;
+        jsize i, n;
+
+        at_cls = (*env)->FindClass(env, "android/app/ActivityThread");
+        if (!at_cls) goto on_return;
+        cur_app = (*env)->GetStaticMethodID(env, at_cls, "currentApplication",
+                                            "()Landroid/app/Application;");
+        if (!cur_app) goto on_return;
+        app = (*env)->CallStaticObjectMethod(env, at_cls, cur_app);
+        if (!app) goto on_return;
+
+        app_cls = (*env)->GetObjectClass(env, app);
+        get_sys = (*env)->GetMethodID(env, app_cls, "getSystemService",
+                                      "(Ljava/lang/String;)Ljava/lang/Object;");
+        if (!get_sys) goto on_return;
+        svc = (*env)->NewStringUTF(env, "audio");
+        am = (*env)->CallObjectMethod(env, app, get_sys, svc);
+        (*env)->DeleteLocalRef(env, svc);
+        if (!am) goto on_return;
+
+        am_cls = (*env)->GetObjectClass(env, am);
+        get_devs = (*env)->GetMethodID(env, am_cls, "getDevices",
+                                       "(I)[Landroid/media/AudioDeviceInfo;");
+        if (!get_devs) goto on_return;
+        /* 2 = AudioManager.GET_DEVICES_OUTPUTS */
+        devs = (jobjectArray)(*env)->CallObjectMethod(env, am, get_devs, 2);
+        if (!devs) goto on_return;
+
+        n = (*env)->GetArrayLength(env, devs);
+        for (i = 0; i < n; ++i) {
+            jobject d = (*env)->GetObjectArrayElement(env, devs, i);
+            jclass d_cls = (*env)->GetObjectClass(env, d);
+            jmethodID get_type = (*env)->GetMethodID(env, d_cls, "getType",
+                                                     "()I");
+            jint t = get_type ? (*env)->CallIntMethod(env, d, get_type) : -1;
+            (*env)->DeleteLocalRef(env, d_cls);
+            if (t == want_type) {
+                dev = d;
+                break;
+            }
+            (*env)->DeleteLocalRef(env, d);
+        }
+        if (!dev) goto on_return;    /* 대상 장치 없음 */
+    }
+
+    jret = (*env)->CallBooleanMethod(env, track, set_dev, dev);
+    ok = jret ? PJ_TRUE : PJ_FALSE;
+
+on_return:
+    if ((*env)->ExceptionCheck(env)) {
+        (*env)->ExceptionClear(env);
+        ok = PJ_FALSE;
+    }
+    if (dev) (*env)->DeleteLocalRef(env, dev);
+    if (devs) (*env)->DeleteLocalRef(env, devs);
+    if (am_cls) (*env)->DeleteLocalRef(env, am_cls);
+    if (am) (*env)->DeleteLocalRef(env, am);
+    if (app_cls) (*env)->DeleteLocalRef(env, app_cls);
+    if (app) (*env)->DeleteLocalRef(env, app);
+    if (at_cls) (*env)->DeleteLocalRef(env, at_cls);
+    if (track_cls) (*env)->DeleteLocalRef(env, track_cls);
+    return ok;
+}
+
 /* API: set capability */
 static pj_status_t strm_set_cap(pjmedia_aud_stream *s,
                                 pjmedia_aud_dev_cap cap,
@@ -853,9 +992,46 @@ static pj_status_t strm_set_cap(pjmedia_aud_stream *s,
     struct android_aud_stream *stream = (struct android_aud_stream*)s;
     JNIEnv *jni_env = 0;
     pj_bool_t attached;
-    
+
     PJ_ASSERT_RETURN(s && value, PJ_EINVAL);
-    
+
+    /* CIMS: 출력 라우트 — 이 스트림의 AudioTrack 에만 장치를 못박는다(트랙 단위).
+     * LOUDSPEAKER=스피커 강제, EARPIECE=수화기 강제, DEFAULT=해제(전역 정책 추종). */
+    if (cap==PJMEDIA_AUD_DEV_CAP_OUTPUT_ROUTE &&
+        (stream->param.dir & PJMEDIA_DIR_PLAYBACK))
+    {
+        pjmedia_aud_dev_route route = *(const pjmedia_aud_dev_route *)value;
+        int want_type;    /* AudioDeviceInfo.TYPE_* */
+        pj_bool_t ok;
+
+        if (!stream->track)
+            return PJMEDIA_EAUD_INVOP;
+
+        switch (route) {
+        case PJMEDIA_AUD_DEV_ROUTE_LOUDSPEAKER:
+            want_type = 2;    /* TYPE_BUILTIN_SPEAKER */
+            break;
+        case PJMEDIA_AUD_DEV_ROUTE_EARPIECE:
+            want_type = 1;    /* TYPE_BUILTIN_EARPIECE */
+            break;
+        default:
+            want_type = 0;    /* 해제 */
+            break;
+        }
+
+        attached = attach_jvm(&jni_env);
+        ok = set_track_preferred_device(jni_env, stream->track, want_type);
+        detach_jvm(attached);
+
+        PJ_LOG(4, (THIS_FILE, "Output route %d -> track preferred device "
+                   "type %d: %s", route, want_type, ok ? "OK" : "FAILED"));
+        if (ok) {
+            stream->param.output_route = route;
+            return PJ_SUCCESS;
+        }
+        return PJMEDIA_EAUD_SYSERR;
+    }
+
     if (cap==PJMEDIA_AUD_DEV_CAP_OUTPUT_VOLUME_SETTING &&
         (stream->param.dir & PJMEDIA_DIR_PLAYBACK))
     {
CIMS_ROUTE_PATCH_EOF
  echo "  patched: android_jni_dev OUTPUT_ROUTE (per-track routing)"
fi

echo "=== [2-11] AMR 인코더 스톨 워치독 패치 (F-6) ==="
# and_aud_mediacodec.cpp 인코더가 비동기 버퍼 큐 고갈로 영구 고착되는 결함(F-6) 대응:
# ①on_error 콜백이 로그만 남기고 방치 → 코덱 치명 에러 시 버퍼 콜백 중단=영구 기아
# ②encode 에러 경로들이 버퍼 인덱스를 반환하지 않고 이탈 → 유한 풀 점진 소실.
# 수정: 실패 연속 계수+온셋 증거 로그(ENC-STALL, 마지막 on_error 코드 포함) →
# 100연속(≈2s) 시 인코더 재생성(ENC-WATCHDOG) + 에러 경로 인덱스 반환(누수 봉합).
# 멱등: 이미 패치돼 있으면 skip. ([2-3] 적용 후 상태 기준 diff)
if grep -q enc_fail_watchdog pjmedia/src/pjmedia-codec/and_aud_mediacodec.cpp; then
  echo "  already patched (skip)"
else
  git apply <<'CIMS_ENC_WATCHDOG_EOF'
--- a/pjmedia/src/pjmedia-codec/and_aud_mediacodec.cpp
+++ b/pjmedia/src/pjmedia-codec/and_aud_mediacodec.cpp
@@ -165,6 +165,18 @@
                                                     buffer                  */
     pj_atomic_queue_t   *dec_avail_output_buf; /**< Decoder available output
                                                     buffer                  */
+
+    /* CIMS F-6: encoder stall watchdog + first-failure evidence.
+     * A wedged MediaCodec (error state stops the async buffer callbacks, or
+     * error paths losing buffer indices) starves the encode loop forever —
+     * media goes silent while floor control stays up. Track consecutive
+     * encode failures and recreate the encoder when they persist.          */
+    unsigned             enc_fail_streak;   /**< Consecutive encode failures*/
+    unsigned             enc_fail_total;    /**< Encode failures since open */
+    unsigned             enc_restart_cnt;   /**< Watchdog encoder restarts  */
+    int                  enc_err_status;    /**< Last on_error status (enc) */
+    int                  enc_err_action;    /**< Last on_error actionCode   */
+    pj_bool_t            enc_err_seen;      /**< on_error fired for encoder */
 } and_media_private_t;
 
 /* CUSTOM CALLBACKS */
@@ -293,7 +305,15 @@
                          const char *detail)
  {
     and_media_private_t *and_media_data = (and_media_private_t *) userdata;
-     __android_log_print(ANDROID_LOG_INFO, THIS_FILE,
+
+    /* CIMS F-6: keep the last encoder error as first-failure evidence for
+     * the stall watchdog (a fatal error stops the async buffer callbacks). */
+    if (codec == and_media_data->enc) {
+        and_media_data->enc_err_seen = PJ_TRUE;
+        and_media_data->enc_err_status = (int)error;
+        and_media_data->enc_err_action = (int)actionCode;
+    }
+     __android_log_print(ANDROID_LOG_ERROR, THIS_FILE,
                         "[%s] On Media error : err[%d] code[%d] msg[%s]\r\n",
                         (codec==and_media_data->enc)?"encoder":"decoder", error,
                         actionCode, detail);
@@ -485,6 +505,91 @@
     return PJ_SUCCESS;
 }
 
+/* CIMS F-6: recreate a wedged encoder in-place — stop/delete the old codec,
+ * drain stale buffer indices (they belong to the dead codec generation),
+ * create a fresh codec, re-arm async callbacks, configure and start.
+ * Called from the encode path when the stall watchdog fires.
+ */
+static pj_bool_t and_med_restart_encoder(and_media_private_t *codec_data)
+{
+    char const *enc_name =
+                   and_media_codec[codec_data->codec_idx].encoder_name->ptr;
+    and_med_buf_info stale;
+
+    if (codec_data->enc) {
+        AMediaCodec_stop(codec_data->enc);
+        AMediaCodec_delete(codec_data->enc);
+        codec_data->enc = NULL;
+    }
+    while (pj_atomic_queue_get(codec_data->enc_avail_input_buf, &stale) ==
+           PJ_SUCCESS)
+        ;
+    while (pj_atomic_queue_get(codec_data->enc_avail_output_buf, &stale) ==
+           PJ_SUCCESS)
+        ;
+
+    codec_data->enc = AMediaCodec_createCodecByName(enc_name);
+    if (!codec_data->enc) {
+        PJ_LOG(2, (THIS_FILE, "ENC-WATCHDOG: recreate '%s' failed", enc_name));
+        return PJ_FALSE;
+    }
+    if (API_AT_LEAST(28)) {
+        AMediaCodecOnAsyncNotifyCallback async_cb = {&and_med_on_input_avail,
+                                                     &and_med_on_output_avail,
+                                                     &and_med_on_format_changed,
+                                                     &and_med_on_error};
+
+        AMediaCodec_setAsyncNotifyCallback(codec_data->enc, async_cb,
+                                           codec_data);
+    }
+    codec_data->enc_err_seen = PJ_FALSE;
+    return (configure_codec(codec_data, PJ_TRUE) == PJ_SUCCESS)? PJ_TRUE
+                                                               : PJ_FALSE;
+}
+
+/* CIMS F-6: account one encode-path failure and run the stall watchdog.
+ * Logs the stall onset with first-failure evidence (last on_error code —
+ * distinguishes "codec error state" from "buffer index starvation"), then
+ * recreates the encoder after ENC_WATCHDOG_RESTART_AFTER consecutive
+ * failures (~2s at 20ms ptime). Returns PJ_TRUE if a restart succeeded.
+ */
+#define ENC_WATCHDOG_RESTART_AFTER 100
+static pj_bool_t enc_fail_watchdog(and_media_private_t *codec_data,
+                                   const char *where)
+{
+    codec_data->enc_fail_streak++;
+    codec_data->enc_fail_total++;
+    if (codec_data->enc_fail_streak == 1) {
+        PJ_LOG(2, (THIS_FILE, "ENC-STALL onset at %s: total=%u restarts=%u "
+                   "on_error(seen=%d status=%d action=%d)",
+                   where, codec_data->enc_fail_total,
+                   codec_data->enc_restart_cnt,
+                   (int)codec_data->enc_err_seen,
+                   codec_data->enc_err_status, codec_data->enc_err_action));
+    } else if (codec_data->enc_fail_streak % 500 == 0) {
+        PJ_LOG(2, (THIS_FILE, "ENC-STALL ongoing at %s: streak=%u total=%u",
+                   where, codec_data->enc_fail_streak,
+                   codec_data->enc_fail_total));
+    }
+    if (codec_data->enc_fail_streak >= ENC_WATCHDOG_RESTART_AFTER) {
+        pj_bool_t ok;
+
+        codec_data->enc_restart_cnt++;
+        PJ_LOG(2, (THIS_FILE, "ENC-WATCHDOG firing: restart #%u (streak=%u "
+                   "total=%u on_error seen=%d status=%d action=%d)",
+                   codec_data->enc_restart_cnt, codec_data->enc_fail_streak,
+                   codec_data->enc_fail_total, (int)codec_data->enc_err_seen,
+                   codec_data->enc_err_status, codec_data->enc_err_action));
+        ok = and_med_restart_encoder(codec_data);
+        PJ_LOG(2, (THIS_FILE, "ENC-WATCHDOG restart #%u %s",
+                   codec_data->enc_restart_cnt, ok? "OK" : "FAILED"));
+        /* Either way wait a full window before considering another restart */
+        codec_data->enc_fail_streak = 0;
+        return ok;
+    }
+    return PJ_FALSE;
+}
+
 /*
  * Initialize and register Android MediaCodec codec factory to pjmedia endpoint.
  */
@@ -997,6 +1102,12 @@
     codec_data->plc_enabled = (attr->setting.plc != 0);
     and_media_data->clock_rate = attr->info.clock_rate;
 
+/* NOTE(cims): AMR settings init block handles BOTH AMR-NB and AMR-WB (see the
+ * codec_id condition below), but was guarded by AMRNB only. When AMRNB is
+ * disabled and AMRWB enabled (our build), the whole block is compiled out, so
+ * codec_data->codec_setting stays NULL and parse_amr/pack_amr dereference a
+ * NULL amr_settings_t (SIGSEGV on the first RX/TX media frame). Widen the guard
+ * to cover AMRWB too. */
 #if PJMEDIA_HAS_AND_MEDIA_AMRNB || PJMEDIA_HAS_AND_MEDIA_AMRWB
     if (and_media_data->codec_id == AND_AUD_CODEC_AMRNB ||
         and_media_data->codec_id == AND_AUD_CODEC_AMRWB)
@@ -1261,8 +1372,9 @@
         if (pj_atomic_queue_get(queue, &buf_info) != PJ_SUCCESS ||
             buf_info.index < 0)
         {
-            PJ_LOG(4,(THIS_FILE, "Encoder failed to get input Buffer[%d]",
-                      buf_info.index));
+            /* CIMS F-6: was an unconditional per-frame log (spammed forever
+             * on a wedged codec) — now stall accounting + watchdog. */
+            enc_fail_watchdog(codec_data, "in-queue");
             goto on_return;
         }
         input_size = samples_per_frame << 1;
@@ -1276,6 +1388,10 @@
             if (am_status != AMEDIA_OK) {
                 PJ_LOG(4, (THIS_FILE, "Encoder queueInputBuffer return %d",
                            am_status));
+                /* CIMS F-6: index was not accepted by the codec — return it
+                 * to the avail queue so it is not leaked. */
+                pj_atomic_queue_put(queue, &buf_info);
+                enc_fail_watchdog(codec_data, "queueInputBuffer");
                 goto on_return;
             }
         } else {
@@ -1288,6 +1404,11 @@
                                      (unsigned long)output_size,
                                      input_size));
             }
+            /* CIMS F-6: index was never queued to the codec — return it to
+             * the avail queue so it is not leaked (each leaked index
+             * permanently shrinks the finite buffer pool). */
+            pj_atomic_queue_put(queue, &buf_info);
+            enc_fail_watchdog(codec_data, "getInputBuffer");
             goto on_return;
         }
 
@@ -1298,6 +1419,7 @@
         {
             PJ_LOG(4, (THIS_FILE, "Encoder failed to get output Buffer[%d]",
                    buf_info.index));
+            enc_fail_watchdog(codec_data, "out-queue");
             goto on_return;
         }
 
@@ -1308,6 +1430,11 @@
             PJ_LOG(4, (THIS_FILE, "Encoder failed getting output buffer, "
                        "index=%d buffer size=%d, flags %d",
                        buf_info.index, buf_info.size, buf_info.flags));
+            /* CIMS F-6: release the output buffer back to the codec so it
+             * is not leaked (a full output side stops input recycling). */
+            AMediaCodec_releaseOutputBuffer(codec_data->enc, buf_info.index,
+                                            0);
+            enc_fail_watchdog(codec_data, "getOutputBuffer");
             goto on_return;
         }
 
@@ -1315,6 +1442,7 @@
         AMediaCodec_releaseOutputBuffer(codec_data->enc,
                                         buf_info.index,
                                         0);
+        codec_data->enc_fail_streak = 0;    /* CIMS F-6: healthy frame */
         bits_out += buf_info.size;
         tx += buf_info.size;
         pcm_in += samples_per_frame;
CIMS_ENC_WATCHDOG_EOF
  echo "  patched: encoder stall watchdog + buffer-leak plugging (F-6)"
fi

echo "=== [2-12] PTT 유휴 무음 50pps 상향 스트림 제거 패치 ==="
# stream.c put_frame_imp: 브리지 미연결 시 NAT 유지 목적의 zero-PCM 인코딩 분기가
# VAD 비활성(noVad=true) 스트림에선 무음 억제 주체가 없어 유휴 내내 50pps 무음 RTP 를
# 송신한다. VAD 비활성이면 송신 생략+RTP ts 만 전진(수신측 ts 연속성 보존) —
# NAT 바인딩·CMP latch 는 KA(empty RTP 5s, [2-2] PJMEDIA_STREAM_ENABLE_KA)가 유지. 멱등.
if grep -q "stream->vad_enabled)" pjmedia/src/pjmedia/stream.c; then
  echo "  already patched (skip)"
else
  git apply <<'CIMS_IDLE_TX_EOF'
diff --git a/pjmedia/src/pjmedia/stream.c b/pjmedia/src/pjmedia/stream.c
index 3ecb03f..fb5c4f8 100644
--- a/pjmedia/src/pjmedia/stream.c
+++ b/pjmedia/src/pjmedia/stream.c
@@ -1161,7 +1161,8 @@ static pj_status_t put_frame_imp( pjmedia_port *port,
     } else if (frame->type == PJMEDIA_FRAME_TYPE_AUDIO &&
                frame->buf == NULL &&
                c_strm->port.info.fmt.id == PJMEDIA_FORMAT_L16 &&
-               (c_strm->dir & PJMEDIA_DIR_ENCODING))
+               (c_strm->dir & PJMEDIA_DIR_ENCODING) &&
+               stream->vad_enabled)
     {
         pjmedia_frame silence_frame;
 
@@ -1192,6 +1193,24 @@ static pj_status_t put_frame_imp( pjmedia_port *port,
                                          (const void**)&rtphdr,
                                          &rtphdrlen);
 
+    /* CIMS: 브리지 미연결(무전 유휴) zero-PCM 프레임 — VAD 비활성이면 무음 억제
+     * 주체가 없어 위 분기가 유휴 내내 연속 무음 RTP(50pps)를 송신하게 된다(PTT 대기
+     * 대역·배터리 낭비). 인코딩·송신을 생략하고 RTP 타임스탬프만 전진시킨다 —
+     * NAT 바인딩·CMP latch 유지는 상단 KA(empty RTP, PJMEDIA_STREAM_KA_INTERVAL)가
+     * 담당(ue_nat_traversal.md §7.1). */
+    } else if (frame->type == PJMEDIA_FRAME_TYPE_AUDIO &&
+               frame->buf == NULL &&
+               c_strm->port.info.fmt.id == PJMEDIA_FORMAT_L16 &&
+               (c_strm->dir & PJMEDIA_DIR_ENCODING))
+    {
+        process_dtmf_pause(stream);
+
+        status = pjmedia_rtp_encode_rtp( &channel->rtp,
+                                         0, 0,
+                                         0, rtp_ts_len,
+                                         (const void**)&rtphdr,
+                                         &rtphdrlen);
+
     /* Encode audio frame */
     } else if ((frame->type == PJMEDIA_FRAME_TYPE_AUDIO &&
                 frame->buf != NULL) ||
CIMS_IDLE_TX_EOF
  echo "  patched: idle zero-PCM TX suppressed for VAD-off streams"
fi

echo "=== [2-13] conference 이벤트 구독 패치 (RFC 4575 로스터 / RFC 6665) ==="
# MCPTT 그룹콜 참가자 로스터를 정식 구독으로 받는다. 종전엔 CSP 가 통화 dialog 로 보내는
# in-dialog NOTIFY 를 앱이 tsx 원문에서 훔쳐 읽고 스택은 usage 없음으로 500 을 응답했다
# (RFC 6665 는 매칭 구독 없는 NOTIFY 에 481 을 요구 — 편법).
#  ① pjsua_pres.c: "conference" 이벤트 패키지 등록(Allow-Events + Accept:
#     application/conference-info+xml) + UAC 구독 생성/in-dialog 갱신/종료 +
#     수신 NOTIFY 본문을 on_pager2 로 앱 전달(→ pjsua2 Account::onInstantMessage).
#  ② pjsua_acc.c: 앱이 보낸 SUBSCRIBE 중 Event: conference 를 가로채 ①로 넘긴다.
#     앱 API(Account::sendRequest)가 그대로여서 **SWIG 재생성/변경이 없다**.
# 구독 생성·갱신(같은 Call-ID/양측 tag/CSeq+1)·Subscription-State 해석·매칭 없는
# NOTIFY 의 481 응답을 스택이 담당 = 규칙을 앱에서 재구현하지 않는다. 멱등.
if grep -q "pjsua_cims_conf_subscribe" pjsip/src/pjsua-lib/pjsua_acc.c; then
  echo "  already patched (skip)"
else
  git apply <<'CIMS_CONF_EVSUB_EOF'
diff --git a/pjsip/src/pjsua-lib/pjsua_acc.c b/pjsip/src/pjsua-lib/pjsua_acc.c
index 526cb39..816cf3c 100644
--- a/pjsip/src/pjsua-lib/pjsua_acc.c
+++ b/pjsip/src/pjsua-lib/pjsua_acc.c
@@ -1546,6 +1546,58 @@ static void on_send_request(void *request_data, pjsip_event *event)
         (pjsua_var.ua_cfg.cb.on_acc_send_request)(data->acc_id, data->token, event);
 }
 
+/* CIMS: conference 이벤트 구독 (구현 = pjsua_pres.c) */
+pj_status_t pjsua_cims_conf_subscribe(pjsua_acc_id acc_id,
+                                      const pj_str_t *target,
+                                      pj_uint32_t expires);
+
+/* CIMS: 앱이 보낸 SUBSCRIBE 가 conference 이벤트면 evsub 기반 구독으로 넘긴다.
+ *
+ * RFC 6665 구독은 단발 요청이 아니다 — 갱신이 in-dialog(같은 Call-ID·양측 tag·
+ * CSeq+1)여야 하고, 종료·만료·Subscription-State 해석·매칭 없는 NOTIFY 의 481
+ * 응답까지 상태를 유지해야 한다. 일반 out-of-dialog 요청으로 모방하면 갱신마다
+ * 새 구독이 생겨 누적되므로(비규격), 스택의 evsub 프레임워크에 위임한다.
+ * 앱 API(pjsua2 Account::sendRequest)는 그대로여서 SWIG 변경이 없다.
+ *
+ * @return PJ_TRUE 면 여기서 처리됨(호출자는 반환), PJ_FALSE 면 일반 경로.
+ */
+static pj_bool_t cims_intercept_subscribe(pjsua_acc_id acc_id,
+                                          const pj_str_t *dest_uri,
+                                          const pj_str_t *method,
+                                          const pjsua_msg_data *msg_data,
+                                          pj_status_t *p_status)
+{
+    const pjsip_hdr *h;
+    pj_bool_t is_conf = PJ_FALSE;
+    pj_uint32_t expires = PJSIP_EXPIRES_NOT_SPECIFIED;
+
+    if (pj_stricmp2(method, "SUBSCRIBE") != 0 || !msg_data)
+        return PJ_FALSE;
+
+    h = msg_data->hdr_list.next;
+    while (h != &msg_data->hdr_list) {
+        const pjsip_generic_string_hdr *gh =
+                                    (const pjsip_generic_string_hdr*)h;
+
+        if (pj_stricmp2(&h->name, "Event") == 0) {
+            if (gh->hvalue.slen >= 10 &&
+                pj_strnicmp2(&gh->hvalue, "conference", 10) == 0)
+            {
+                is_conf = PJ_TRUE;
+            }
+        } else if (pj_stricmp2(&h->name, "Expires") == 0) {
+            expires = (pj_uint32_t) pj_strtoul(&gh->hvalue);
+        }
+        h = h->next;
+    }
+
+    if (!is_conf)
+        return PJ_FALSE;
+
+    *p_status = pjsua_cims_conf_subscribe(acc_id, dest_uri, expires);
+    return PJ_TRUE;
+}
+
 PJ_DEF(pj_status_t) pjsua_acc_send_request(pjsua_acc_id acc_id,
                                            const pj_str_t *dest_uri,
                                            const pj_str_t *method,
@@ -1566,6 +1618,10 @@ PJ_DEF(pj_status_t) pjsua_acc_send_request(pjsua_acc_id acc_id,
     PJ_ASSERT_RETURN(method, PJ_EINVAL);
     PJ_UNUSED_ARG(options);
 
+    /* CIMS: conference 구독은 evsub 경로로 (위 주석 참조) */
+    if (cims_intercept_subscribe(acc_id, dest_uri, method, msg_data, &status))
+        return status;
+
     PJ_LOG(4,(THIS_FILE, "Account %d sending %.*s request..",
                           acc_id, (int)method->slen, method->ptr));
     pj_log_push_indent();
diff --git a/pjsip/src/pjsua-lib/pjsua_pres.c b/pjsip/src/pjsua-lib/pjsua_pres.c
index 2f3b8b7..ffb8889 100644
--- a/pjsip/src/pjsua-lib/pjsua_pres.c
+++ b/pjsip/src/pjsua-lib/pjsua_pres.c
@@ -2636,6 +2636,430 @@ static pj_status_t enable_unsolicited_mwi(void)
 }
 
 
+/***************************************************************************
+ * CIMS: conference 이벤트 구독 (RFC 4575 로스터 + RFC 6665 구독 프레임워크)
+ *
+ * MCPTT 그룹콜의 참가자 로스터를 정식 구독으로 받는다. 앱(pjsua2)은 SWIG 변경 없이
+ * 기존 Account::sendRequest("SUBSCRIBE", <group AoR>, Event: conference) 를 그대로
+ * 호출하고, pjsua_acc_send_request 가 그 요청을 여기로 넘긴다 — 단발 out-of-dialog
+ * 요청 대신 스택이 관리하는 구독이 생성된다. 구독 생성·in-dialog 갱신(같은 Call-ID /
+ * 양측 tag / CSeq+1)·종료·Subscription-State 해석·매칭 없는 NOTIFY 의 481 응답을
+ * 모두 evsub 프레임워크가 수행하므로 규칙을 앱에서 재구현하지 않는다.
+ *
+ * 수신 NOTIFY 본문은 on_pager2 로 앱에 올린다 → pjsua2 Account::onInstantMessage
+ * (contentType=application/conference-info+xml, fromUri=그룹 AoR=conference focus).
+ */
+
+/* 동시 구독 상한 = 단말이 동시에 편성/참여할 수 있는 채널 수 */
+#define CIMS_CONF_MAX_SUB       16
+
+typedef struct cims_conf_sub
+{
+    pj_bool_t            used;
+    pjsua_acc_id         acc_id;
+    pj_pool_t           *pool;      /**< target 문자열 보관용            */
+    pj_str_t             target;    /**< 구독 자원 = 그룹 AoR            */
+    pjsip_dialog        *dlg;
+    pjsip_evsub         *sub;
+} cims_conf_sub;
+
+static cims_conf_sub cims_conf_subs[CIMS_CONF_MAX_SUB];
+
+/* 이벤트 패키지 등록용 모듈 (mod-mwi 와 같은 역할 — mod_data 슬롯 소유) */
+static pjsip_module mod_cims_conf =
+{
+    NULL, NULL,                         /* prev, next.                  */
+    { "mod-cims-conf", 13 },            /* Name.                        */
+    -1,                                 /* Id                           */
+    PJSIP_MOD_PRIORITY_DIALOG_USAGE,    /* Priority                     */
+    NULL,                               /* load()                       */
+    NULL,                               /* start()                      */
+    NULL,                               /* stop()                       */
+    NULL,                               /* unload()                     */
+    NULL,                               /* on_rx_request()              */
+    NULL,                               /* on_rx_response()             */
+    NULL,                               /* on_tx_request.               */
+    NULL,                               /* on_tx_response()             */
+    NULL,                               /* on_tsx_state()               */
+};
+
+static const pj_str_t STR_CONF_EVENT = { "conference", 10 };
+static const pj_str_t STR_CONF_INFO  = { "application/conference-info+xml", 31 };
+
+
+static cims_conf_sub *cims_conf_find(const pj_str_t *target)
+{
+    unsigned i;
+
+    for (i=0; i<PJ_ARRAY_SIZE(cims_conf_subs); ++i) {
+        if (cims_conf_subs[i].used &&
+            pj_stricmp(&cims_conf_subs[i].target, target) == 0)
+        {
+            return &cims_conf_subs[i];
+        }
+    }
+    return NULL;
+}
+
+
+static void cims_conf_release(cims_conf_sub *cs)
+{
+    pj_pool_t *pool = cs->pool;
+
+    pj_bzero(cs, sizeof(*cs));
+    if (pool)
+        pj_pool_release(pool);
+}
+
+
+/* 구독 상태 전이 — TERMINATED 면 슬롯을 비워 재구독이 가능하게 한다.
+ * (서버의 Subscription-State: terminated, 구독 만료, NOTIFY 실패 모두 여기로 온다)
+ */
+static void cims_conf_on_evsub_state(pjsip_evsub *sub, pjsip_event *event)
+{
+    cims_conf_sub *cs;
+
+    PJ_UNUSED_ARG(event);
+
+    cs = (cims_conf_sub*) pjsip_evsub_get_mod_data(sub, mod_cims_conf.id);
+    if (!cs)
+        return;
+
+    if (pjsip_evsub_get_state(sub) == PJSIP_EVSUB_STATE_TERMINATED) {
+        PJ_LOG(4,(THIS_FILE, "CIMS conference subscription to %.*s terminated",
+                  (int)cs->target.slen, cs->target.ptr));
+        pjsip_evsub_set_mod_data(sub, mod_cims_conf.id, NULL);
+        cims_conf_release(cs);
+    }
+}
+
+
+/* NOTIFY 수신 — 200 OK(evsub 기본)로 응답하고 본문을 앱에 전달.
+ * evsub 가 Event/dialog 매칭·CSeq·Subscription-State 를 이미 검증한 뒤 불린다.
+ */
+static void cims_conf_on_rx_notify(pjsip_evsub *sub,
+                                   pjsip_rx_data *rdata,
+                                   int *p_st_code,
+                                   pj_str_t **p_st_text,
+                                   pjsip_hdr *res_hdr,
+                                   pjsip_msg_body **p_body)
+{
+    cims_conf_sub *cs;
+    pjsip_msg_body *body;
+    pjsip_contact_hdr *contact_hdr;
+    pjsip_media_type *m;
+    pj_str_t from, to, contact, mime, text;
+    char mime_buf[128];
+
+    PJ_UNUSED_ARG(p_st_text);
+    PJ_UNUSED_ARG(res_hdr);
+    PJ_UNUSED_ARG(p_body);
+
+    cs = (cims_conf_sub*) pjsip_evsub_get_mod_data(sub, mod_cims_conf.id);
+    if (!cs || !rdata)
+        return;
+
+    *p_st_code = 200;
+
+    body = rdata->msg_info.msg->body;
+    if (!body || !body->data || body->len == 0)
+        return;                 /* 본문 없는 상태 통지(갱신 확인 등) — 전달 불필요 */
+
+    if (!pjsua_var.ua_cfg.cb.on_pager2)
+        return;
+
+    /* From/To/Contact 는 pjsua_im 의 pager 처리와 같은 방식으로 문자열화한다
+     * (앱이 MESSAGE 수신에서 쓰는 형식과 동일해야 파싱이 일관된다).
+     */
+    from.ptr = (char*) pj_pool_alloc(rdata->tp_info.pool, PJSIP_MAX_URL_SIZE);
+    from.slen = pjsip_uri_print(PJSIP_URI_IN_FROMTO_HDR,
+                                rdata->msg_info.from->uri,
+                                from.ptr, PJSIP_MAX_URL_SIZE);
+    if (from.slen < 1)
+        from = cs->target;
+
+    to.ptr = (char*) pj_pool_alloc(rdata->tp_info.pool, PJSIP_MAX_URL_SIZE);
+    to.slen = pjsip_uri_print(PJSIP_URI_IN_FROMTO_HDR,
+                              rdata->msg_info.to->uri,
+                              to.ptr, PJSIP_MAX_URL_SIZE);
+    if (to.slen < 1)
+        to.slen = 0;
+
+    contact_hdr = (pjsip_contact_hdr*)
+                  pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_CONTACT,
+                                     NULL);
+    if (contact_hdr && contact_hdr->uri) {
+        contact.ptr = (char*) pj_pool_alloc(rdata->tp_info.pool,
+                                            PJSIP_MAX_URL_SIZE);
+        contact.slen = pjsip_uri_print(PJSIP_URI_IN_CONTACT_HDR,
+                                       contact_hdr->uri, contact.ptr,
+                                       PJSIP_MAX_URL_SIZE);
+        if (contact.slen < 1)
+            contact.slen = 0;
+    } else {
+        contact.ptr = (char*)"";
+        contact.slen = 0;
+    }
+
+    m = &body->content_type;
+    mime.ptr = mime_buf;
+    mime.slen = pj_ansi_snprintf(mime_buf, sizeof(mime_buf), "%.*s/%.*s",
+                                 (int)m->type.slen, m->type.ptr,
+                                 (int)m->subtype.slen, m->subtype.ptr);
+    if (mime.slen < 1)
+        mime = STR_CONF_INFO;
+
+    text.ptr = (char*) body->data;
+    text.slen = (pj_ssize_t) body->len;
+
+    PJ_LOG(5,(THIS_FILE, "CIMS conference NOTIFY from %.*s (%d bytes)",
+              (int)from.slen, from.ptr, (int)text.slen));
+
+    (*pjsua_var.ua_cfg.cb.on_pager2)(PJSUA_INVALID_ID, &from, &to, &contact,
+                                     &mime, &text, rdata, cs->acc_id);
+}
+
+
+/* on_client_refresh / on_server_timeout 은 기본 동작(자동 in-dialog 갱신)을 쓴다 */
+static pjsip_evsub_user cims_conf_cb =
+{
+    &cims_conf_on_evsub_state,
+    NULL,                       /* on_tsx_state                         */
+    NULL,                       /* on_rx_refresh (UAS 전용)             */
+    &cims_conf_on_rx_notify,
+    NULL,                       /* on_client_refresh: 기본 = 자동 갱신  */
+    NULL,                       /* on_server_timeout                    */
+};
+
+
+/*
+ * conference 구독 시작/갱신/종료.
+ *  expires > 0                        : 신규 생성 또는 in-dialog 갱신
+ *  expires == 0                       : 구독 해지 (SUBSCRIBE Expires: 0)
+ *  expires == PJSIP_EXPIRES_NOT_SPECIFIED : 패키지 기본값 사용
+ */
+pj_status_t pjsua_cims_conf_subscribe(pjsua_acc_id acc_id,
+                                      const pj_str_t *target,
+                                      pj_uint32_t expires)
+{
+    pjsua_acc *acc;
+    cims_conf_sub *cs;
+    pj_pool_t *tmp_pool = NULL;
+    pj_str_t contact;
+    pjsip_dialog *dlg = NULL;
+    pjsip_evsub *sub = NULL;
+    pjsip_tx_data *tdata;
+    pjsip_tpselector tp_sel;
+    pj_status_t status;
+    unsigned i;
+
+    PJ_ASSERT_RETURN(target && target->slen, PJ_EINVAL);
+    PJ_ASSERT_RETURN(acc_id>=0 && acc_id<(int)PJ_ARRAY_SIZE(pjsua_var.acc),
+                     PJ_EINVAL);
+    PJ_ASSERT_RETURN(pjsua_acc_is_valid(acc_id), PJ_EINVAL);
+
+    acc = &pjsua_var.acc[acc_id];
+
+    /* 기존 구독이 있으면 갱신 또는 해지 — 둘 다 in-dialog 요청이어야 하며,
+     * 앱의 일반 sendRequest 로는 만들 수 없는 부분이다.
+     */
+    cs = cims_conf_find(target);
+    if (cs) {
+        pjsip_dlg_inc_lock(cs->dlg);
+        status = pjsip_evsub_initiate(cs->sub, NULL, expires, &tdata);
+        if (status == PJ_SUCCESS) {
+            pjsua_process_msg_data(tdata, NULL);
+            status = pjsip_evsub_send_request(cs->sub, tdata);
+        }
+        pjsip_dlg_dec_lock(cs->dlg);
+
+        if (status != PJ_SUCCESS) {
+            pjsua_perror(THIS_FILE, "Unable to update conference subscription",
+                         status);
+        } else {
+            PJ_LOG(4,(THIS_FILE, "CIMS conference subscription to %.*s %s",
+                      (int)target->slen, target->ptr,
+                      (expires==0 ? "terminating" : "refreshed")));
+        }
+        return status;
+    }
+
+    /* 구독이 없는데 해지 요청 — 할 일 없음 */
+    if (expires == 0)
+        return PJ_SUCCESS;
+
+    for (i=0; i<PJ_ARRAY_SIZE(cims_conf_subs); ++i) {
+        if (!cims_conf_subs[i].used)
+            break;
+    }
+    if (i == PJ_ARRAY_SIZE(cims_conf_subs)) {
+        PJ_LOG(3,(THIS_FILE, "Too many conference subscriptions (max %d)",
+                  CIMS_CONF_MAX_SUB));
+        return PJ_ETOOMANY;
+    }
+    cs = &cims_conf_subs[i];
+
+    PJ_LOG(4,(THIS_FILE, "Starting CIMS conference subscription to %.*s..",
+              (int)target->slen, target->ptr));
+    pj_log_push_indent();
+
+    /* Contact — 계정에 이미 있으면 그것을 쓴다(MWI 와 동일) */
+    if (acc->contact.slen) {
+        contact = acc->contact;
+    } else {
+        tmp_pool = pjsua_pool_create("tmpconf", 512, 256);
+        status = pjsua_acc_create_uac_contact(tmp_pool, &contact,
+                                              acc_id, &acc->cfg.id);
+        if (status != PJ_SUCCESS) {
+            pjsua_perror(THIS_FILE, "Unable to generate Contact header",
+                         status);
+            goto on_return;
+        }
+    }
+
+    status = pjsip_dlg_create_uac(pjsip_ua_instance(), &acc->cfg.id, &contact,
+                                  target, NULL, &dlg);
+    if (status != PJ_SUCCESS) {
+        pjsua_perror(THIS_FILE, "Unable to create dialog", status);
+        goto on_return;
+    }
+
+    pjsip_dlg_inc_lock(dlg);
+
+    if (acc->cfg.allow_via_rewrite && acc->via_addr.host.slen > 0) {
+        pjsip_dlg_set_via_sent_by(dlg, &acc->via_addr, acc->via_tp);
+    } else if (!pjsua_sip_acc_is_using_stun(acc_id) &&
+               !pjsua_sip_acc_is_using_upnp(acc_id))
+    {
+        pjsip_host_port via_addr;
+        const void *via_tp;
+
+        if (pjsua_acc_get_uac_addr(acc_id, dlg->pool, &acc->cfg.id,
+                                   &via_addr, NULL, NULL,
+                                   &via_tp) == PJ_SUCCESS)
+        {
+            pjsip_dlg_set_via_sent_by(dlg, &via_addr,
+                                      (pjsip_transport*)via_tp);
+        }
+    }
+
+    /* Event 헤더에 id 파라미터를 붙이지 않는다 — 서버 NOTIFY 도 "Event: conference"
+     * 단독이므로 id 없는 매칭(PJSIP_EVSUB_NO_EVENT_ID)이어야 짝이 맞는다.
+     */
+    status = pjsip_evsub_create_uac(dlg, &cims_conf_cb, &STR_CONF_EVENT,
+                                    PJSIP_EVSUB_NO_EVENT_ID, &sub);
+    if (status != PJ_SUCCESS) {
+        pjsua_perror(THIS_FILE, "Error creating conference subscription",
+                     status);
+        pjsip_dlg_dec_lock(dlg);
+        pjsip_dlg_terminate(dlg);
+        goto on_return;
+    }
+
+    pjsua_init_tpselector(acc_id, &tp_sel);
+    pjsip_dlg_set_transport(dlg, &tp_sel);
+
+    if (!pj_list_empty(&acc->route_set))
+        pjsip_dlg_set_route_set(dlg, &acc->route_set);
+
+    if (acc->cred_cnt) {
+        pjsip_auth_clt_set_credentials(&dlg->auth_sess, acc->cred_cnt,
+                                       acc->cred);
+    }
+    pjsip_auth_clt_set_prefs(&dlg->auth_sess, &acc->cfg.auth_pref);
+
+    cs->used   = PJ_TRUE;
+    cs->acc_id = acc_id;
+    cs->pool   = pjsua_pool_create("cimsconf", 512, 256);
+    cs->dlg    = dlg;
+    cs->sub    = sub;
+    pj_strdup_with_null(cs->pool, &cs->target, target);
+
+    pjsip_evsub_set_mod_data(sub, mod_cims_conf.id, cs);
+
+    status = pjsip_evsub_initiate(sub, NULL, expires, &tdata);
+    if (status != PJ_SUCCESS) {
+        pjsua_perror(THIS_FILE, "Unable to create conference SUBSCRIBE",
+                     status);
+        pjsip_evsub_set_mod_data(sub, mod_cims_conf.id, NULL);
+        pjsip_dlg_dec_lock(dlg);
+        pjsip_evsub_terminate(sub, PJ_FALSE);
+        cims_conf_release(cs);
+        goto on_return;
+    }
+
+    pjsua_process_msg_data(tdata, NULL);
+
+    status = pjsip_evsub_send_request(sub, tdata);
+    if (status != PJ_SUCCESS) {
+        pjsua_perror(THIS_FILE, "Unable to send conference SUBSCRIBE", status);
+        pjsip_evsub_set_mod_data(sub, mod_cims_conf.id, NULL);
+        pjsip_dlg_dec_lock(dlg);
+        pjsip_evsub_terminate(sub, PJ_FALSE);
+        cims_conf_release(cs);
+        goto on_return;
+    }
+
+    pjsip_dlg_dec_lock(dlg);
+
+on_return:
+    if (tmp_pool) pj_pool_release(tmp_pool);
+    pj_log_pop_indent();
+    return status;
+}
+
+
+/* 라이브러리 종료 시 남은 구독 정리 (로그아웃→재로그인 사이클 누수 방지) */
+static void cims_conf_shutdown(void)
+{
+    unsigned i;
+
+    for (i=0; i<PJ_ARRAY_SIZE(cims_conf_subs); ++i) {
+        cims_conf_sub *cs = &cims_conf_subs[i];
+
+        if (!cs->used || !cs->sub)
+            continue;
+        pjsip_evsub_set_mod_data(cs->sub, mod_cims_conf.id, NULL);
+        pjsip_evsub_terminate(cs->sub, PJ_FALSE);
+        cims_conf_release(cs);
+    }
+}
+
+
+/* conference 이벤트 패키지 등록 — Allow-Events: conference +
+ * SUBSCRIBE 의 Accept: application/conference-info+xml 자동 부착.
+ * 패키지 미등록이면 pjsip_evsub_create_uac 가 PJSIP_SIMPLE_ENOPKG 로 실패한다.
+ */
+static pj_status_t cims_conf_init(void)
+{
+    pj_str_t accept[1];
+    pj_status_t status;
+
+    if (mod_cims_conf.id != -1)
+        return PJ_SUCCESS;                  /* 이미 등록됨 (재init 대비) */
+
+    pj_bzero(cims_conf_subs, sizeof(cims_conf_subs));
+
+    status = pjsip_endpt_register_module(pjsua_var.endpt, &mod_cims_conf);
+    if (status != PJ_SUCCESS) {
+        pjsua_perror(THIS_FILE, "Unable to register conference module",
+                     status);
+        return status;
+    }
+
+    accept[0] = STR_CONF_INFO;
+    status = pjsip_evsub_register_pkg(&mod_cims_conf, &STR_CONF_EVENT,
+                                      3600, PJ_ARRAY_SIZE(accept), accept);
+    if (status != PJ_SUCCESS) {
+        pjsua_perror(THIS_FILE, "Unable to register conference event package",
+                     status);
+        pjsip_endpt_unregister_module(pjsua_var.endpt, &mod_cims_conf);
+        return status;
+    }
+
+    return PJ_SUCCESS;
+}
+
 
 /***************************************************************************/
 
@@ -2695,6 +3119,9 @@ pj_status_t pjsua_pres_init()
         reset_buddy(i);
     }
 
+    /* CIMS: conference 이벤트 패키지 등록 (실패해도 나머지 기능은 유지) */
+    cims_conf_init();
+
     return status;
 }
 
@@ -2736,6 +3163,9 @@ void pjsua_pres_shutdown(unsigned flags)
     PJ_LOG(4,(THIS_FILE, "Shutting down presence.."));
     pj_log_push_indent();
 
+    /* CIMS: conference 구독 정리 */
+    cims_conf_shutdown();
+
     if (pjsua_var.pres_timer.id != 0) {
         pjsip_endpt_cancel_timer(pjsua_var.endpt, &pjsua_var.pres_timer);
         pjsua_var.pres_timer.id = PJ_FALSE;
CIMS_CONF_EVSUB_EOF
  echo "  patched: conference event subscription (evsub) + sendRequest intercept"
fi

echo "=== [3] configure-android + make (arm64-v8a) ==="
export APP_PLATFORM=28
export TARGET_ABI=arm64-v8a
./configure-android --use-ndk-cflags 2>&1 | tail -30
make dep 2>&1 | tail -3
make clean >/dev/null 2>&1 || true
make -j"$(nproc)" 2>&1 | tail -15
echo "=== native build DONE ==="

echo "=== [4] SWIG Java 바인딩 ==="
cd pjsip-apps/src/swig
make 2>&1 | tail -25

echo "=== [4-검증] 산출물 ==="
JNIDIR=$(find ~/pjproject/pjsip-apps/src/swig -type d -path "*jniLibs/arm64-v8a" | head -1)
echo "jniLibs: $JNIDIR"
ls -la "$JNIDIR"
JAVADIR=$(find ~/pjproject/pjsip-apps/src/swig -type d -path "*org/pjsip/pjsua2" | head -1)
echo "java: $JAVADIR ($(ls "$JAVADIR" | wc -l) files)"
# libc++_shared.so 필수 동봉 — 없으면 NDK sysroot 에서 복사
if [ ! -f "$JNIDIR/libc++_shared.so" ]; then
  CXXSO=$(find "$ANDROID_NDK_ROOT" -path "*aarch64-linux-android/libc++_shared.so" | head -1)
  cp "$CXXSO" "$JNIDIR/" && echo "libc++_shared.so copied from NDK"
fi
file "$JNIDIR/libpjsua2.so" | grep -q aarch64 && echo "ABI=arm64 OK"

echo "=== BUILD ALL DONE ==="
echo "JNIDIR=$JNIDIR"
echo "JAVADIR=$JAVADIR"
