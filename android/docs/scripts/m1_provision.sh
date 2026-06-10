#!/bin/bash
# M1.0-1: nex-ubuntu 빌드 환경 프로비저닝 (루트/sudo 불필요 — 전부 $HOME 설치)
# 산출: ~/opt/jdk17, ~/opt/swig, ~/android-sdk(+NDK r28), ~/.m1env (이후 단계 source)
set -e
mkdir -p ~/opt ~/dl

echo "=== [0] network ==="
curl -sI --max-time 10 https://dl.google.com >/dev/null && echo "net OK"

echo "=== [1] JDK 17 (Temurin, userland) ==="
if [ ! -x ~/opt/jdk17/bin/java ]; then
  curl -sL -o ~/dl/jdk17.tar.gz \
    "https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/normal/eclipse?project=jdk"
  mkdir -p ~/opt/jdk17
  tar -xzf ~/dl/jdk17.tar.gz -C ~/opt/jdk17 --strip-components=1
fi
export JAVA_HOME=$HOME/opt/jdk17
export PATH=$JAVA_HOME/bin:$PATH
java -version 2>&1 | head -1

echo "=== [2] SWIG (deb 추출, 루트 불필요) ==="
if [ ! -x ~/opt/swig/usr/bin/swig ]; then
  cd ~/dl
  apt-get download swig 2>/dev/null || apt download swig
  # 본체 패키지(버전별 이름)도 함께 — 실패는 무시(단일 패키지 배포일 수 있음)
  apt-get download swig4.2 2>/dev/null || apt-get download swig4.1 2>/dev/null || true
  mkdir -p ~/opt/swig
  for d in ~/dl/swig*.deb; do dpkg -x "$d" ~/opt/swig; done
fi
export PATH=$HOME/opt/swig/usr/bin:$PATH
SWIG_LIB_DIR=$(ls -d ~/opt/swig/usr/share/swig*/ 2>/dev/null | head -1)
export SWIG_LIB=${SWIG_LIB_DIR%/}
swig -version | grep -i version
echo "SWIG_LIB=$SWIG_LIB"
# pcre2 런타임 의존 확인
ldd "$(command -v swig)" | grep -E "not found" && { echo "SWIG 의존 라이브러리 부재"; exit 1; } || echo "swig deps OK"

echo "=== [3] Android cmdline-tools + NDK r28 ==="
export ANDROID_SDK_ROOT=$HOME/android-sdk
if [ ! -x ~/android-sdk/cmdline-tools/latest/bin/sdkmanager ]; then
  curl -sL -o ~/dl/cmdtools.zip \
    "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip"
  mkdir -p ~/android-sdk/cmdline-tools
  unzip -q -o ~/dl/cmdtools.zip -d ~/android-sdk/cmdline-tools
  rm -rf ~/android-sdk/cmdline-tools/latest
  mv ~/android-sdk/cmdline-tools/cmdline-tools ~/android-sdk/cmdline-tools/latest
fi
yes | ~/android-sdk/cmdline-tools/latest/bin/sdkmanager --licenses >/dev/null 2>&1 || true
NDKVER=$(~/android-sdk/cmdline-tools/latest/bin/sdkmanager --list 2>/dev/null \
  | grep -oE 'ndk;28\.[0-9]+\.[0-9]+' | sort -uV | tail -1)
[ -z "$NDKVER" ] && { echo "NDK 28.x 목록 조회 실패"; exit 1; }
echo "선택 NDK: $NDKVER"
~/android-sdk/cmdline-tools/latest/bin/sdkmanager "$NDKVER" "platform-tools" >/dev/null
NDKDIR=$(ls -d ~/android-sdk/ndk/28.* | sort -V | tail -1)
echo "NDK 설치: $NDKDIR"

echo "=== [4] env 저장 (~/.m1env) ==="
cat > ~/.m1env <<EOF
export JAVA_HOME=\$HOME/opt/jdk17
export ANDROID_SDK_ROOT=\$HOME/android-sdk
export ANDROID_NDK_ROOT=$NDKDIR
export SWIG_LIB=$SWIG_LIB
export PATH=\$JAVA_HOME/bin:\$HOME/opt/swig/usr/bin:\$ANDROID_SDK_ROOT/cmdline-tools/latest/bin:\$PATH
EOF
cat ~/.m1env

echo "=== PROVISION DONE ==="
