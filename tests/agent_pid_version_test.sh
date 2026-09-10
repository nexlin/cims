#!/bin/bash
# agent pid 위치·소유권 단위시험 (F-55) — 라이브 무접촉, 임시 디렉토리 + 가짜 바이너리
#
# 왜: pid 를 버전 폴더 안에 두면 업그레이드가 `current` 를 넘기는 순간 옛 프로세스의 pid 를
# 잃는다. 그러면 stop 이 조용히 "성공" 하고 옛 프로세스가 포트를 쥔 채 남아, 이어지는
# start 가 bind 실패로 죽는다 — "업그레이드했다고 보고되지만 옛 코드가 도는" 상태
# (2026-09-10 csp 두 번 실측). 아래 6건이 그 재발을 막는다.
#
#   T1  PID_DIR 유도       배포본(<prefix>/modules/<mod>/…) → <prefix>/run · dist 트리 → 자기 run
#   T2  버전 전환 후 stop  current 가 새 버전으로 넘어가도 옛 프로세스를 죽인다
#   T3  이행 폴백          pid 가 옛 자리(버전 폴더 안)에만 있어도 찾아 죽인다
#   T4  pid 완전 유실      install 루트 아래 실행 중인 프로세스를 찾아 죽인다
#   T5  남의 모듈 불간섭   다른 모듈 루트의 프로세스는 건드리지 않는다
#   T6  진짜 없음          "중지할 대상 없음" — 살아 있는 것을 놓친 것과 구분한다
#
# 실행: tests/agent_pid_version_test.sh
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d /tmp/pidver_test.XXXXXX)"
trap 'pkill -f "$T/" 2>/dev/null; rm -rf "$T"' EXIT

FAIL=0
chk() { # chk <이름> <실제> <기대>
    if [[ "$2" == "$3" ]]; then echo "  OK   $1"
    else echo "  FAIL $1"; echo "         got =$2"; echo "         want=$3"; FAIL=$((FAIL+1)); fi
}
chk_dead() { if kill -0 "$2" 2>/dev/null; then echo "  FAIL $1 (pid=$2 아직 살아있음)"; FAIL=$((FAIL+1)); else echo "  OK   $1"; fi; }
chk_alive() { if kill -0 "$2" 2>/dev/null; then echo "  OK   $1"; else echo "  FAIL $1 (pid=$2 죽었음)"; FAIL=$((FAIL+1)); fi; }

# ── T1: cims-svc 의 _pid_dir_for (실제 함수를 떼어 와서 시험) ────────────────
sed -n '/^_pid_dir_for() {/,/^}/p' "$REPO_ROOT/agent/bin/cims-svc" > "$T/pid_dir_for.sh"
source "$T/pid_dir_for.sh"
mkdir -p "$T/prefix/modules/csp/0.2.0" "$T/repo/build/dist"
echo "[T1] PID_DIR 유도"
chk "배포본 버전 디렉토리" "$(_pid_dir_for "$T/prefix/modules/csp/0.2.0")" "$T/prefix/run"
ln -sfn 0.2.0 "$T/prefix/modules/csp/current"
chk "배포본 current 심링크"  "$(_pid_dir_for "$T/prefix/modules/csp/current")" "$T/prefix/run"
chk "dist 트리는 종전대로"   "$(_pid_dir_for "$T/repo/build/dist")"            "$T/repo/build/dist/run"

# ── lifecycle.sh 적재 (stub) ────────────────────────────────────────────────
RED=""; GREEN=""; YELLOW=""; CYAN=""; BOLD=""; NC=""
info() { :; }; ok() { echo "    [ok] $*"; }; warn() { echo "    [warn] $*"; }
err() { echo "    [err] $*"; }; header() { :; }
export SCRIPT_DIR="$T"
PYBIN="$(command -v python3)"
source "$REPO_ROOT/agent/lib/lifecycle.sh" >/dev/null 2>&1 || true

# 가짜 모듈 바이너리 — /proc/<pid>/exe 가 모듈 루트 아래를 가리켜야 소유권 판정을 시험할 수
# 있다. coreutils(sleep)는 argv[0] 으로 동작이 갈리는 multi-call 이라 이름을 바꿔 복사하면
# 즉사한다 — 그러면 "죽었다" 가 항상 참이 되어 시험이 통과처럼 보인다. 그래서 직접 만든다.
cat > "$T/fake_mod.c" <<'EOF'
#include <unistd.h>
int main(void) { for (;;) pause(); return 0; }
EOF
cc -O0 -o "$T/fake_mod" "$T/fake_mod.c" 2>/dev/null || {
    echo "  SKIP cc 없음 — 컴파일러가 필요합니다 (build-essential)"; exit 0; }

setup_mod() { # setup_mod <mod> <ver...>
    local mod="$1"; shift
    local v
    for v in "$@"; do
        mkdir -p "$T/prefix/modules/$mod/$v/$mod/bin" "$T/prefix/modules/$mod/$v/run"
        cp "$T/fake_mod" "$T/prefix/modules/$mod/$v/$mod/bin/$mod"
    done
    ln -sfn "$1" "$T/prefix/modules/$mod/current"
    mkdir -p "$T/prefix/run"
}
# 기동 후 /proc 에 올라올 때까지 아주 짧게 기다린다 (exe 링크가 생기기 전 스캔 방지)
# 자식의 stdout/stderr 를 끊는다 — 안 끊으면 $(launch ...) 의 커맨드 치환이 파이프 EOF 를
# 기다리며 영원히 멈춘다(자식이 종료하지 않으므로).
launch() {
    "$T/prefix/modules/$1/$2/$1/bin/$1" >/dev/null 2>&1 &
    local p=$!
    sleep 0.2
    echo "$p"
}

use_mod() { DIST_DIR="$T/prefix/modules/$1/current"; PID_DIR="$T/prefix/run"; LOG_DIR="$DIST_DIR/log"; }

# ── T2: 버전 전환 후에도 stop 이 옛 프로세스를 죽인다 ───────────────────────
echo "[T2] 버전 전환 후 stop"
setup_mod csp 0.1.0 0.2.0
p2="$(launch csp 0.1.0)"; echo "$p2" > "$T/prefix/run/csp.pid"
ln -sfn 0.2.0 "$T/prefix/modules/csp/current"      # 업그레이드가 current 를 넘긴다
use_mod csp
chk "전환 후에도 pid 를 읽는다" "$(read_pid csp)" "$p2"
stop_one csp >/dev/null 2>&1
chk_dead "옛 버전 프로세스가 죽었다" "$p2"

# ── T3: 이행 폴백 — pid 가 옛 자리에만 있는 경우 ────────────────────────────
echo "[T3] 이행 폴백 (pid 가 버전 폴더 안)"
rm -rf "$T/prefix"; setup_mod csp 0.1.0 0.2.0
p3="$(launch csp 0.1.0)"; echo "$p3" > "$T/prefix/modules/csp/0.1.0/run/csp.pid"
ln -sfn 0.2.0 "$T/prefix/modules/csp/current"
use_mod csp
chk "옛 자리 pid 를 찾는다" "$(read_pid csp)" "$p3"
stop_one csp >/dev/null 2>&1
chk_dead "옛 자리 pid 로도 죽인다" "$p3"
chk "옛 pid 파일도 지운다" "$([[ -f "$T/prefix/modules/csp/0.1.0/run/csp.pid" ]] && echo 남음 || echo 삭제됨)" "삭제됨"

# ── T4: pid 파일이 아예 없는 경우 ───────────────────────────────────────────
echo "[T4] pid 완전 유실"
rm -rf "$T/prefix"; setup_mod csp 0.1.0 0.2.0
p4="$(launch csp 0.1.0)"
ln -sfn 0.2.0 "$T/prefix/modules/csp/current"
use_mod csp
chk "pid 파일은 없다" "$(read_pid csp)" ""
chk "실행 프로세스는 찾는다" "$(_pids_under_module_root csp)" "$p4"
stop_one csp >/dev/null 2>&1
chk_dead "찾아서 죽인다" "$p4"

# ── T5: 남의 모듈은 건드리지 않는다 ─────────────────────────────────────────
echo "[T5] 다른 모듈 불간섭"
rm -rf "$T/prefix"; setup_mod csp 0.1.0; setup_mod cmp 0.1.0
p5c="$(launch csp 0.1.0)"; p5m="$(launch cmp 0.1.0)"
use_mod csp
stop_one csp >/dev/null 2>&1
chk_dead  "csp 는 죽고"      "$p5c"
chk_alive "cmp 는 살아있다"  "$p5m"
kill "$p5m" 2>/dev/null

# ── T6: 진짜 아무것도 없을 때 ───────────────────────────────────────────────
echo "[T6] 중지 대상 없음"
rm -rf "$T/prefix"; setup_mod csp 0.1.0
use_mod csp
out="$(stop_one csp 2>&1)"
chk "대상 없음을 그렇게 보고한다" "$(echo "$out" | grep -c '중지할 대상 없음')" "1"

echo
echo "실패 $FAIL 건"
exit $(( FAIL > 0 ? 1 : 0 ))
