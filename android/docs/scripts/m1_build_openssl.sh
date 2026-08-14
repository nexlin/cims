#!/bin/bash
# OpenSSL for Android (arm64-v8a) 정적 빌드 — pjproject TLS transport 전제.
#   산출: $HOME/opt/openssl-android-arm64/{include,lib/libssl.a,lib/libcrypto.a}
#   m1_build_pjsip.sh 가 --with-ssl 로 이 경로를 넘긴다.
# 전제: ~/.m1env (ANDROID_NDK_ROOT)
set -e -o pipefail
source ~/.m1env

VER=${OPENSSL_VER:-3.0.15}
PREFIX=${OPENSSL_PREFIX:-$HOME/opt/openssl-android-arm64}

cd ~
if [ ! -f "openssl-$VER.tar.gz" ]; then
  echo "=== [1] OpenSSL $VER 내려받기 ==="
  curl -fsSL -o "openssl-$VER.tar.gz" \
    "https://github.com/openssl/openssl/releases/download/openssl-$VER/openssl-$VER.tar.gz"
fi

echo "=== [2] 압축 해제 ==="
rm -rf "openssl-$VER" && tar xf "openssl-$VER.tar.gz"
cd "openssl-$VER"

echo "=== [3] Configure (android-arm64, API 21, static) ==="
export ANDROID_NDK_ROOT
export PATH="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
# no-shared: .so 배포 없이 pjsua2 네이티브에 정적 링크. no-engine/no-ui-console: 불필요 표면 축소.
./Configure android-arm64 -D__ANDROID_API__=21 --prefix="$PREFIX" \
  no-shared no-tests no-ui-console no-engine

echo "=== [4] build + install ==="
make -j"$(nproc)"
make install_sw

ls -l "$PREFIX"/lib/libssl.a "$PREFIX"/lib/libcrypto.a
echo "=== OpenSSL DONE: $PREFIX ==="
