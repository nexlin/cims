#!/bin/bash
# 단말 SDK 엔진(ext/pjproject) Android 네이티브 빌드 — configure-android → make → SWIG Java → 산출물 배치.
#   정본: docs/design/features/ue_sdk.md §3·§8. 소스 정본은 ext/pjproject 트리 자체이며 이 스크립트는
#   패치를 적용하지 않는다(upstream clone·sed 패치 단계 없음). 수정은 트리에서 직접 하고 커밋한다.
#
# 전제(WSL2/Ubuntu 빌드 머신, android_ue_m1_pjsip_integration.md §2.2):
#   ANDROID_NDK_ROOT  NDK 경로 (r27+)
#   OPENSSL_PREFIX    Android arm64 정적 OpenSSL (m1_build_openssl.sh 산출, 기본 ~/opt/openssl-android-arm64)
#   swig, python3, JDK 가 PATH 에 있을 것. ~/.m1env 가 있으면 source 한다(m1_provision.sh 산출).
#
# 사용: sdk/android/build-native.sh [--no-install] [--abi arm64-v8a] [--platform 28]
#   산출물: libpjsua2.so + libc++_shared.so → $OUT_JNI (기본 android/core/src/pjsua2/jniLibs/<abi>)
#           org/pjsip/pjsua2/*.java + PjCamera*.java → $OUT_JAVA (기본 android/core/src/pjsua2/java)
#   ※ B 단계(sdk/core·libcimsue 도입) 이후 산출물 위치는 sdk/android/cimsue 로 옮겨진다(ue_sdk.md §5.1).
set -e -o pipefail
[ -f ~/.m1env ] && source ~/.m1env

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PJ_DIR="$ROOT/ext/pjproject"
CONFIG_SITE="$ROOT/sdk/engine/config_site/android.h"
OUT_JNI_BASE="${OUT_JNI:-$ROOT/android/core/src/pjsua2/jniLibs}"
OUT_JAVA="${OUT_JAVA:-$ROOT/android/core/src/pjsua2/java}"
INSTALL=1
export TARGET_ABI="${TARGET_ABI:-arm64-v8a}"
export APP_PLATFORM="${APP_PLATFORM:-28}"
while [ $# -gt 0 ]; do
  case "$1" in
    --no-install) INSTALL=0 ;;
    --abi) TARGET_ABI="$2"; shift ;;
    --platform) APP_PLATFORM="$2"; shift ;;
    *) echo "unknown arg: $1"; exit 2 ;;
  esac; shift
done

: "${ANDROID_NDK_ROOT:?ANDROID_NDK_ROOT 필요}"
OPENSSL_PREFIX="${OPENSSL_PREFIX:-$HOME/opt/openssl-android-arm64}"
# TLS transport 는 OpenSSL 을 요구한다. 부재 시 configure 가 TLS 를 조용히 끄므로 먼저 확인한다.
test -f "$OPENSSL_PREFIX/lib/libssl.a" || { echo "!! OpenSSL for Android 없음: $OPENSSL_PREFIX (m1_build_openssl.sh 먼저)"; exit 1; }
test -f "$PJ_DIR/pjmedia/src/pjmedia-codec/and_aud_mediacodec.cpp" || { echo "!! ext/pjproject 트리 이상 (and_aud_mediacodec.cpp 없음)"; exit 1; }

cd "$PJ_DIR"
echo "=== [1] config_site.h ← $CONFIG_SITE ==="
printf '#include "%s"\n' "$CONFIG_SITE" > pjlib/include/pj/config_site.h
cat pjlib/include/pj/config_site.h

echo "=== [2] configure-android + make ($TARGET_ABI, platform $APP_PLATFORM) ==="
./configure-android --use-ndk-cflags --with-ssl="$OPENSSL_PREFIX" 2>&1 | tail -30
make dep 2>&1 | tail -3
make clean >/dev/null 2>&1 || true
make -j"$(nproc)" 2>&1 | tail -15
echo "=== native build DONE ==="

echo "=== [3] SWIG Java 바인딩 ==="
cd pjsip-apps/src/swig
# swig/java 의 libpjsua2.so 규칙은 pjsua2_wrap.o 만 의존한다 — 정적 .a 가 바뀌어도 wrap.o 가 최신이면
# make 가 재링크를 건너뛴다(엔진 수정이 .so 에 반영되지 않음). 링크는 수 초라 항상 강제한다.
find java -path "*jniLibs/*/libpjsua2.so" -delete 2>/dev/null || true
make 2>&1 | tail -25

echo "=== [4] 산출물 ==="
JNIDIR=$(find "$PJ_DIR/pjsip-apps/src/swig" -type d -path "*jniLibs/$TARGET_ABI" | head -1)
JAVADIR=$(find "$PJ_DIR/pjsip-apps/src/swig" -type d -path "*org/pjsip/pjsua2" | head -1)
[ -n "$JNIDIR" ] && [ -n "$JAVADIR" ] || { echo "!! SWIG 산출물 없음"; exit 1; }
if [ ! -f "$JNIDIR/libc++_shared.so" ]; then
  CXXSO=$(find "$ANDROID_NDK_ROOT" -path "*aarch64-linux-android/libc++_shared.so" | head -1)
  cp "$CXXSO" "$JNIDIR/" && echo "libc++_shared.so copied from NDK"
fi
file "$JNIDIR/libpjsua2.so" | grep -q aarch64 && echo "ABI=arm64 OK"
ls -la "$JNIDIR"; echo "java: $JAVADIR ($(ls "$JAVADIR" | wc -l) files)"

if [ "$INSTALL" = 1 ]; then
  echo "=== [5] 배치 → $OUT_JNI_BASE/$TARGET_ABI, $OUT_JAVA ==="
  mkdir -p "$OUT_JNI_BASE/$TARGET_ABI" "$OUT_JAVA/org/pjsip/pjsua2"
  cp -f "$JNIDIR"/libpjsua2.so "$JNIDIR"/libc++_shared.so "$OUT_JNI_BASE/$TARGET_ABI/"
  cp -f "$JAVADIR"/*.java "$OUT_JAVA/org/pjsip/pjsua2/"
  cp -f "$(dirname "$JAVADIR")"/PjCamera*.java "$OUT_JAVA/org/pjsip/" 2>/dev/null || true
fi

echo "=== 트리 청결 확인 (정본 트리는 빌드 후에도 변경이 없어야 한다) ==="
cd "$ROOT" && git status --short ext/pjproject | head -20
echo "=== BUILD ALL DONE ==="
