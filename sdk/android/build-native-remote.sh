#!/bin/bash
# 단말 SDK Android 네이티브 빌드를 ssh 로 닿는 Linux 빌드 호스트에 맡기고 산출물을 이 트리로 받아 온다.
#   WSL 이 없는 Windows 개발 PC(Git Bash)에서 VMware VM 등으로 build-native.sh 를 돌리는 경로
#   — android/docs/dev_environment_setup.md §4.2, docs/design/features/android_dispatch_tablet.md §2.1. 빌드 절차 자체는 build-native.sh 가 정본이다.
#
# 보내는 것 = 작업 사본 그대로(커밋 안 한 수정·새 파일 포함, .gitignore 제외). 임시 인덱스로 트리를 만들어
#   원격 저장소에 push 하고 원격은 그 커밋으로 checkout 한다 — ① Windows 작업 사본은 core.autocrlf 로 CRLF 라
#   configure 스크립트가 그대로는 돌지 않는데, git 트리는 LF 다 ② 지운 파일이 원격에 남지 않는다 ③ 두 번째부터는
#   바뀐 것만 간다 ④ build-native.sh 끝의 «정본 트리 청결 확인»(git status)이 원격에서도 뜻을 갖는다.
#   이 PC 의 저장소에는 참조 없는 커밋 하나만 남는다(gc 대상).
#
# 받아 오는 것 = build-native.sh 가 두 AAR 모듈에 배치한 생성물(커밋하지 않는 것 — .gitignore):
#   sdk/android/cimsue-engine/src/main/{jniLibs,java/org}   sdk/android/cimsue/src/{main/jniLibs,swig/java}
#
# 환경:
#   CIMS_ANDROID_BUILD_HOST  ssh 호스트(기본 nex-ubuntu — ~/.ssh/config 별칭, 키 인증)
#   CIMS_ANDROID_BUILD_DIR   원격 작업 디렉터리(기본 cims-android-src, 원격 홈 기준)
#   CIMS_ANDROID_BUILD_VMX   설정하면 VMware vmrun 으로 VM 을 먼저 띄운다(nogui, 이미 떠 있으면 그대로)
# 원격 전제(한 번): build-native.sh 의 전제 그대로 — ~/.m1env(JDK·SWIG·NDK, android/docs/scripts/m1_provision.sh) +
#   Android arm64 정적 OpenSSL. OpenSSL 이 없으면 여기서 android/docs/scripts/m1_build_openssl.sh 를 먼저 돌린다.
#
# 사용: sdk/android/build-native-remote.sh [--sync-only] [build-native.sh 인자...]
#   --sync-only   보내기만 한다(원격에서 직접 build-native.sh 를 돌릴 때)
set -e -o pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="${CIMS_ANDROID_BUILD_HOST:-nex-ubuntu}"
RDIR="${CIMS_ANDROID_BUILD_DIR:-cims-android-src}"
SYNC_ONLY=0
if [ "${1:-}" = "--sync-only" ]; then SYNC_ONLY=1; shift; fi

# 네이티브 빌드가 읽는 경로 전부 — build-native.sh·sdk/android·sdk/core CMake 의 입력(floor·MCVideo 전송 제어 정의 생성기 포함)과 OpenSSL 스크립트.
PATHS=(.gitignore ext/pjproject sdk/core sdk/engine sdk/android
       scripts/gen_floor_defs.py docs/design/features/mcptt_floor_defs.yaml
       scripts/gen_mcvideo_tc_defs.py docs/design/features/mcvideo_tc_defs.yaml android/docs/scripts)
# 받아 올 생성물 — build-native.sh [5]·[7] 의 배치 위치.
OUTS=(sdk/android/cimsue-engine/src/main/jniLibs sdk/android/cimsue-engine/src/main/java/org
      sdk/android/cimsue/src/main/jniLibs sdk/android/cimsue/src/swig/java)

rsh() { ssh -o BatchMode=yes "$HOST" "$@"; }
export GIT_SSH_COMMAND="${GIT_SSH_COMMAND:-ssh -o BatchMode=yes}"

if [ -n "${CIMS_ANDROID_BUILD_VMX:-}" ]; then
  VMRUN="${VMRUN:-vmrun}"
  command -v "$VMRUN" >/dev/null || VMRUN="/c/Program Files (x86)/VMware/VMware Workstation/vmrun.exe"
  if ! "$VMRUN" list | grep -qiF "$(basename "$CIMS_ANDROID_BUILD_VMX")"; then
    echo "=== VM 기동: $CIMS_ANDROID_BUILD_VMX ==="
    "$VMRUN" -T ws start "$CIMS_ANDROID_BUILD_VMX" nogui
  fi
fi
echo "=== [0] 빌드 호스트 $HOST ==="
for _ in $(seq 1 30); do rsh -o ConnectTimeout=5 true 2>/dev/null && break; sleep 5; done
rsh true || { echo "!! $HOST 에 ssh 로 닿지 않는다"; exit 1; }

echo "=== [1] 작업 사본 → 트리 (임시 인덱스, LF) ==="
cd "$ROOT"
TMP_INDEX="$(git rev-parse --git-path index.android-remote)"
cp -f "$(git rev-parse --git-path index)" "$TMP_INDEX"       # 기존 모드(실행 비트)를 잇는다 — core.fileMode=false 인 작업 사본
trap 'rm -f "$TMP_INDEX"' EXIT
GIT_INDEX_FILE="$TMP_INDEX" git add -A -- "${PATHS[@]}"
TREE=$(GIT_INDEX_FILE="$TMP_INDEX" git write-tree)
COMMIT=$(git commit-tree "$TREE" -m "android native build snapshot")
echo "tree $TREE → commit $COMMIT"

echo "=== [2] 원격 저장소 $HOST:$RDIR 로 push ==="
rsh "set -e; mkdir -p '$RDIR'; cd '$RDIR'; [ -d .git ] || git init -q"
git push -q -f "ssh://$HOST/~/$RDIR" "$COMMIT:refs/heads/snapshot"
# 생성물(.gitignore 대상 — 빌드 디렉터리·배치된 .so/Java)은 남겨 두고 추적 파일만 이 커밋으로 맞춘다.
rsh "set -e; cd '$RDIR'; git -c advice.detachedHead=false checkout -q -f --detach snapshot; git clean -q -fd -- ${PATHS[*]}"

[ "$SYNC_ONLY" = 1 ] && { echo "=== --sync-only — 원격: cd ~/$RDIR && sdk/android/build-native.sh ==="; exit 0; }

echo "=== [3] 원격 전제 — OpenSSL(Android arm64) ==="
rsh "set -e; [ -f ~/.m1env ] || { echo '!! ~/.m1env 없음 — android/docs/scripts/m1_provision.sh 먼저'; exit 1; }
     . ~/.m1env; P=\${OPENSSL_PREFIX:-\$HOME/opt/openssl-android-arm64}
     if [ -f \"\$P/lib/libssl.a\" ]; then echo \"OpenSSL: \$P\"; else bash '$RDIR/android/docs/scripts/m1_build_openssl.sh' 2>&1 | tail -5; fi"

echo "=== [4] 원격 build-native.sh $* ==="
rsh "cd '$RDIR' && bash sdk/android/build-native.sh $*"

echo "=== [5] 생성물 ← $HOST ==="
rm -rf "${OUTS[@]}"
rsh "cd '$RDIR' && tar cf - ${OUTS[*]} 2>/dev/null" | tar xf - -C "$ROOT"
for d in sdk/android/cimsue-engine/src/main/jniLibs/arm64-v8a sdk/android/cimsue/src/main/jniLibs/arm64-v8a; do
  [ -d "$d" ] && ls -la "$d"
done
echo "SWIG Java: engine $(find sdk/android/cimsue-engine/src/main/java/org -name '*.java' 2>/dev/null | wc -l) · cimsue $(find sdk/android/cimsue/src/swig/java -name '*.java' 2>/dev/null | wc -l)"
echo "=== REMOTE NATIVE BUILD DONE — 다음: cd android && ./gradlew :dispatch-tablet:assembleDebug ==="
