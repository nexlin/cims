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
