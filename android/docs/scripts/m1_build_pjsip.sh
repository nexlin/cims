#!/bin/bash
# M1.0-2/3/4: pjproject 2.16 clone → config_site.h(경로 C) → configure-android → make → SWIG
# 전제: ~/.m1env (m1_provision.sh 산출)
set -e
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
