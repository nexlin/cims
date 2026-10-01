#!/bin/bash
# 관제 태블릿 앱(android/dispatch-tablet) 빌드·단말 배포 — Windows 개발 PC(Git Bash)의 단일 진입점.
#   네이티브(.so + SWIG Java) = sdk/android/build-native-remote.sh(ssh 빌드 호스트) · APK = Gradle(Windows) · 배포 = adb.
#   절차·전제는 android/docs/dev_environment_setup.md §4 가 정본이고, 이 스크립트는 그 순서를 묶을 뿐이다.
#
# 사용: android/tablet.sh <명령>
#   doctor                 환경 점검 — JDK·SDK·Gradle 데몬·빌드 호스트·네이티브 생성물·APK·단말
#   native [--force] [build-native.sh 인자...]
#                          네이티브 빌드. 빌드 입력이 생성물을 만든 때와 같으면 건너뛴다(--force 또는 인자를 주면 무조건 빌드)
#   apk                    APK — :dispatch-tablet:assembleDebug
#   install [-s 시리얼]    단말에 설치하고 실행한다(단말이 하나면 -s 생략)
#   up [-s 시리얼]         native(필요할 때만) → apk → install
#
# 환경:
#   JAVA_HOME      jlink 가 있는 JDK 17+ (없거나 맞지 않으면 Android Studio 의 JBR)
#   ANDROID_HOME   Android SDK (adb 가 PATH 에 없을 때 platform-tools 를 여기서 찾는다)
#   CIMS_ANDROID_BUILD_HOST·CIMS_ANDROID_BUILD_VMX 등은 build-native-remote.sh 가 읽는다
set -e -o pipefail

ADIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$ADIR/.." && pwd)"
REMOTE="$ROOT/sdk/android/build-native-remote.sh"
STAMP="$ROOT/sdk/android/cimsue/src/main/jniLibs/native-inputs.sha1"
CORE_SO="$ROOT/sdk/android/cimsue/src/main/jniLibs/arm64-v8a/libcimsue.so"
APK="$ADIR/dispatch-tablet/build/outputs/apk/debug/dispatch-tablet-debug.apk"
PKG=com.cims.ue.dispatch
ACTIVITY="$PKG/.ui.MainActivity"
ABI=arm64-v8a                                   # dispatch-tablet/build.gradle.kts 의 abiFilters

die() { echo "!! $*" >&2; exit 1; }

# Gradle 을 돌릴 JDK — AGP 의 JdkImageTransform 이 jlink 를 쓴다(JRE 로는 안 된다).
STUDIO_JBR="/c/Program Files/Android/Android Studio/jbr"
if [ -z "${JAVA_HOME:-}" ] || [ ! -f "$JAVA_HOME/bin/jlink.exe" ]; then
  [ -f "$STUDIO_JBR/bin/jlink.exe" ] && export JAVA_HOME="$STUDIO_JBR"
fi
ADB="$(command -v adb || true)"
[ -n "$ADB" ] || ADB="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}/platform-tools/adb.exe"

# 떠 있는 Gradle 데몬 가운데 jlink 없는 JRE 로 뜬 것의 java.exe 경로 — 다른 IDE 의 Java 확장이 띄운 것이다.
#   데몬 조건이 «Java 21, 벤더 무관»(gradle/gradle-daemon-jvm.properties)이라 CLI 가 그 데몬을 재사용하고, 그러면
#   :cimsue:compileDebugJavaWithJavac 가 «jlink executable … does not exist» 로 떨어진다(dev_environment_setup.md §6).
foreign_daemons() {
  powershell.exe -NoProfile -Command \
    "Get-CimInstance Win32_Process -Filter \"Name='java.exe'\" | Where-Object { \$_.CommandLine -match 'GradleDaemon' } | ForEach-Object { \$_.ExecutablePath }" \
    2>/dev/null | tr -d '\r' | while IFS= read -r exe; do
      [ -n "$exe" ] || continue
      [ -f "$(dirname "$exe")/jlink.exe" ] || echo "$exe"
    done
}

gradle() {
  [ -f "${JAVA_HOME:-}/bin/jlink.exe" ] || die "jlink 가 있는 JDK 가 없다 — JAVA_HOME 을 Android Studio JBR 로"
  if [ -n "$(foreign_daemons)" ]; then
    echo "=== jlink 없는 JRE 로 뜬 Gradle 데몬을 내린다 ==="
    (cd "$ADIR" && ./gradlew.bat --stop | tail -1)
  fi
  (cd "$ADIR" && ./gradlew.bat "$@")
}

native_fresh() {
  [ -f "$STAMP" ] && [ -f "$CORE_SO" ] && [ "$(cat "$STAMP")" = "$("$REMOTE" --inputs-hash)" ]
}

cmd_native() {
  if [ "${1:-}" = "--force" ]; then shift
  elif [ $# = 0 ] && native_fresh; then
    echo "=== 네이티브 생성물이 지금 입력과 같다 — 건너뜀 (다시 지으려면 native --force) ==="; return 0
  fi
  "$REMOTE" "$@"
}

cmd_apk() {
  [ -f "$CORE_SO" ] || die "네이티브 생성물 없음($CORE_SO) — 먼저 native"
  gradle :dispatch-tablet:assembleDebug
  ls -la "$APK"
}

# 설치할 단말 하나를 고른다 — 상태가 device 인 것만.
pick_device() {
  local serial="$1" list s hw seen=" "
  # 무선 디버깅 단말은 mDNS 자동 연결(adb-<시리얼>-…._adb-tls-connect._tcp)과 adb connect <ip:포트> 로 두 번 잡힐 수 있다 —
  #   단말 시리얼(ro.serialno)이 같은 것은 하나로 센다.
  list="$("$ADB" devices | tr -d '\r' | awk 'NR>1 && $2=="device"{print $1}' | while IFS= read -r s; do
            hw="$(MSYS_NO_PATHCONV=1 "$ADB" -s "$s" shell getprop ro.serialno </dev/null | tr -d '\r')"
            case "$seen" in *" $hw "*) ;; *) seen="$seen$hw "; echo "$s" ;; esac
          done)"
  if [ -n "$serial" ]; then
    list="$("$ADB" devices | tr -d '\r' | awk 'NR>1 && $2=="device"{print $1}')"
    echo "$list" | grep -qxF "$serial" || die "단말 $serial 이 연결돼 있지 않다 — $ADB devices"
    echo "$serial"; return
  fi
  case "$(echo "$list" | grep -c .)" in
    0) "$ADB" devices >&2
       die "연결된 단말 없음 — USB 디버깅 또는 무선 디버깅(adb pair <ip:페어링포트> <코드> → adb connect <ip:포트>), dev_environment_setup.md §2" ;;
    1) echo "$list" ;;
    *) echo "$list" >&2; die "단말이 여럿이다 — -s <시리얼>" ;;
  esac
}

cmd_install() {
  local serial=""
  [ "${1:-}" = "-s" ] && { serial="${2:?-s 뒤에 시리얼}"; shift 2; }
  [ -f "$APK" ] || die "APK 없음 — 먼저 apk"
  serial="$(pick_device "$serial")"
  export MSYS_NO_PATHCONV=1                     # adb shell 인자를 Git Bash 가 Windows 경로로 바꾸지 않게
  "$ADB" -s "$serial" shell getprop ro.product.cpu.abilist | grep -q "$ABI" \
    || die "단말 $serial 이 $ABI 가 아니다 — 이 APK 는 $ABI 만 싣는다"
  echo "=== 설치 → $serial ($("$ADB" -s "$serial" shell getprop ro.product.model | tr -d '\r')) ==="
  local out
  if ! out="$("$ADB" -s "$serial" install -r "$(cygpath -w "$APK")" 2>&1)"; then
    echo "$out" >&2
    # 디버그 서명 키는 PC 마다 다르다 — 다른 PC 가 지은 APK 위에는 덮어 설치되지 않는다.
    echo "$out" | grep -q INSTALL_FAILED_UPDATE_INCOMPATIBLE \
      && echo "!! 단말에 다른 키로 서명된 $PKG 가 있다. 지우고 다시: $ADB -s $serial uninstall $PKG  (앱 데이터도 지워진다)" >&2
    exit 1
  fi
  echo "$out" | tail -1
  "$ADB" -s "$serial" shell am start -n "$ACTIVITY" | tail -1
}

cmd_doctor() {
  local ok="  ok " ng="  !! " sdk compile host
  echo "[JDK]"
  if [ -f "${JAVA_HOME:-}/bin/jlink.exe" ]; then echo "$ok$JAVA_HOME — $("$JAVA_HOME/bin/java" -version 2>&1 | head -1)"
  else echo "${ng}jlink 가 있는 JDK 없음 (JAVA_HOME=${JAVA_HOME:-})"; fi
  echo "[Gradle 데몬]"
  if [ -n "$(foreign_daemons)" ]; then foreign_daemons | sed "s/^/${ng}jlink 없는 JRE 데몬(빌드 때 내린다): /"; else echo "${ok}jlink 없는 JRE 로 뜬 데몬 없음"; fi
  echo "[Android SDK]"
  sdk="$(sed -n 's/^sdk\.dir=//p' "$ADIR/local.properties" 2>/dev/null | sed 's/\\\\/\//g; s/\\:/:/')"
  compile="$(sed -n 's/^compileSdk *= *"\(.*\)"/\1/p' "$ADIR/gradle/libs.versions.toml")"
  if [ -n "$sdk" ] && ls -d "$sdk/platforms/android-$compile"* >/dev/null 2>&1; then echo "$ok$sdk — platform $(basename "$(ls -d "$sdk/platforms/android-$compile"* | head -1)")"
  else echo "${ng}compileSdk $compile 플랫폼 없음 (local.properties sdk.dir=${sdk:-없음})"; fi
  echo "[빌드 호스트]"
  host="${CIMS_ANDROID_BUILD_HOST:-nex-ubuntu}"
  if ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" '[ -f ~/.m1env ]' 2>/dev/null; then echo "$ok$host (ssh, ~/.m1env)"
  else echo "$ng$host 에 닿지 않거나 ~/.m1env 없음 — VM 기동은 CIMS_ANDROID_BUILD_VMX='${CIMS_ANDROID_BUILD_VMX:-미설정}'"; fi
  echo "[네이티브 생성물]"
  if [ ! -f "$CORE_SO" ]; then echo "${ng}없음 — native"
  elif native_fresh; then echo "${ok}지금 입력과 같다 ($(date -r "$CORE_SO" '+%m-%d %H:%M'))"
  else echo "${ng}입력이 바뀌었다 — native ($(date -r "$CORE_SO" '+%m-%d %H:%M') 빌드)"; fi
  echo "[APK]"
  if [ -f "$APK" ]; then echo "$ok$(date -r "$APK" '+%m-%d %H:%M') $(du -h "$APK" | cut -f1)"; else echo "${ng}없음 — apk"; fi
  echo "[단말]"
  "$ADB" devices -l | tr -d '\r' | awk 'NR>1 && NF' | sed 's/^/     /'
  "$ADB" devices | tr -d '\r' | awk 'NR>1 && $2=="device"' | grep -q . || echo "${ng}연결된 단말 없음"
}

case "${1:-}" in
  doctor)  shift; cmd_doctor "$@" ;;
  native)  shift; cmd_native "$@" ;;
  apk)     shift; cmd_apk "$@" ;;
  install) shift; cmd_install "$@" ;;
  up)      shift; cmd_native; cmd_apk; cmd_install "$@" ;;
  *) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 2 ;;
esac
