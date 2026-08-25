#!/bin/bash
# agent/lib/lifecycle.sh _start_csp_variant 하드닝 단위시험 (가짜 csp 바이너리, 샌드박스)
#
# 검증 범위 (7시나리오 16항목):
#   T1  local_nodes 해석 — enabled 리스너 실포트 목록(disabled 제외, TLS→tcp)·primary
#   T2  정상 start — bind 게이트 통과 + pidfile == 포트 소유 worker
#   T3  재fork — 초기 $! 사망 후 child 가 늦게 bind → pidfile 승계
#   T4  pidfile 유실 + 건강한 worker 존재 → 무중단 승계 (멱등 start)
#   T5  반죽음 좀비(own-exe, 미bind) → 정리 후 신규 기동
#   T6  기동 실패(미bind) — 게이트 타임아웃 rc!=0
#   T7  즉사 바이너리 — 타임아웃 전 조기 실패
#
# 실행: tests/agent_lifecycle_csp_start_test.sh   (라이브 무접촉 — 임시 디렉토리 + 155xx 포트)
# 요구: cc(build-essential). 포트는 LC_TEST_PORT(기본 15760, +1/+2 연번) 로 변경 가능.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TESTROOT="$(mktemp -d /tmp/lc_test.XXXXXX)"
PORT="${LC_TEST_PORT:-15760}"

export DIST_DIR="$TESTROOT/dist"
export PID_DIR="$DIST_DIR/run"
export LOG_DIR="$DIST_DIR/log"
export SCRIPT_DIR="$TESTROOT"
RED=""; GREEN=""; YELLOW=""; CYAN=""; BOLD=""; NC=""
info() { echo "[info] $*"; }
ok()   { echo "[ok] $*"; }
warn() { echo "[warn] $*"; }
err()  { echo "[err] $*"; }
header() { echo "[==] $*"; }

cat > "$TESTROOT/fake_csp.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int main(int argc, char**argv){
    const char* mode = getenv("CSP_FAKE_MODE"); if(!mode) mode="normal";
    const char* pstr = getenv("CSP_FAKE_PORT");
    int port = pstr ? atoi(pstr) : 15760;
    if(!strcmp(mode,"die")) return 1;
    if(!strcmp(mode,"refork")){
        pid_t c = fork();
        if(c>0){ sleep(1); _exit(1); }   /* parent = worker abort simulate */
        sleep(2);                        /* child binds late */
    }
    if(strcmp(mode,"nobind")){
        int s=socket(AF_INET,SOCK_DGRAM,0);
        struct sockaddr_in a; memset(&a,0,sizeof a);
        a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=INADDR_ANY;
        if(bind(s,(struct sockaddr*)&a,sizeof a)<0){ perror("bind"); return 2; }
    }
    for(;;) sleep(10);
}
EOF
cc -o "$TESTROOT/fake_csp" "$TESTROOT/fake_csp.c" || { echo "FAIL: fake_csp 컴파일"; exit 1; }

mkdir -p "$DIST_DIR/csp/bin" "$DIST_DIR/csp/config" "$DIST_DIR/config" "$PID_DIR" "$LOG_DIR"
cp "$TESTROOT/fake_csp" "$DIST_DIR/csp/bin/csp"
cat > "$DIST_DIR/csp/config/csp.json" <<'EOF'
{"Setup": {"Sip": {"UdpPort": 5060}}}
EOF
cat > "$DIST_DIR/config/local_nodes.jsonl" <<EOF
{"id": "u1", "bind_ip": "0.0.0.0", "bind_port": $PORT, "protocol": "UDP", "enabled": true, "is_primary": true}
{"id": "t1", "bind_ip": "0.0.0.0", "bind_port": $((PORT+1)), "protocol": "TCP", "enabled": true}
{"id": "x1", "bind_ip": "0.0.0.0", "bind_port": $((PORT+2)), "protocol": "TLS", "enabled": false}
EOF

source "$REPO_ROOT/agent/lib/lifecycle.sh"

export CSP_FAKE_PORT=$PORT
export CIMS_CSP_START_TIMEOUT=6
PASS=0; FAIL=0
verdict() { # $1=name $2=cond(0 ok)
    if [[ "$2" == "0" ]]; then echo "PASS: $1"; PASS=$((PASS+1)); else echo "FAIL: $1"; FAIL=$((FAIL+1)); fi
}
cleanup_procs() {
    pids=$(_pids_by_exe "$DIST_DIR/csp/bin/csp"); [[ -n "$pids" ]] && kill -9 $pids 2>/dev/null
    pkill -9 -f "$TESTROOT/fake_csp" 2>/dev/null
    rm -f "$PID_DIR/csp.pid"
    sleep 0.3
    return 0
}
cleanup_all() { cleanup_procs; rm -rf "$TESTROOT"; }
trap cleanup_all EXIT

echo "=== T1: 헬퍼 — local_nodes 해석 ==="
nodes=$(_csp_local_nodes_path "$DIST_DIR/csp/config/csp.json" csp)
[[ "$nodes" == "$DIST_DIR/config/local_nodes.jsonl" ]]; verdict "T1a local_nodes 경로" $?
ports=$(_csp_listener_ports "$nodes" | tr '\n' ';')
[[ "$ports" == "udp $PORT;tcp $((PORT+1));" ]]; verdict "T1b enabled 포트 목록 (disabled 제외, TLS→tcp)" $?
prim=$(_csp_primary_listener "$nodes")
[[ "$prim" == "udp $PORT" ]]; verdict "T1c primary" $?

echo "=== T2: 정상 start — bind 게이트 + pidfile 일치 ==="
cleanup_procs
export CSP_FAKE_MODE=normal
( _start_csp_variant csp ); rc=$?
verdict "T2a start rc=0" $rc
pid=$(cat "$PID_DIR/csp.pid" 2>/dev/null || echo "")
owner=$(_pid_by_port "$PORT:udp")
[[ -n "$pid" && "$pid" == "$owner" ]]; verdict "T2b pidfile == 포트 소유 worker ($pid/$owner)" $?

echo "=== T3: 재fork — 초기 pid 사망 후 child 가 bind → pidfile 승계 ==="
cleanup_procs
export CSP_FAKE_MODE=refork
( _start_csp_variant csp ); rc=$?
verdict "T3a start rc=0 (초기 pid 사망에도)" $rc
pid=$(cat "$PID_DIR/csp.pid" 2>/dev/null || echo "")
owner=$(_pid_by_port "$PORT:udp")
[[ -n "$pid" && "$pid" == "$owner" ]] && kill -0 "$pid" 2>/dev/null; verdict "T3b pidfile == 재fork worker ($pid/$owner)" $?

echo "=== T4: pidfile 유실 + 건강한 worker 존재 → 승계(무중단) ==="
old_pid="$pid"
rm -f "$PID_DIR/csp.pid"
export CSP_FAKE_MODE=normal
( _start_csp_variant csp ); rc=$?
verdict "T4a start rc=0" $rc
pid=$(cat "$PID_DIR/csp.pid" 2>/dev/null || echo "")
[[ "$pid" == "$old_pid" ]] && kill -0 "$old_pid" 2>/dev/null; verdict "T4b 기존 worker 승계·생존 (old=$old_pid new=$pid)" $?

echo "=== T5: 반죽음 좀비(미bind own-exe) → 정리 후 신규 기동 ==="
cleanup_procs
export CSP_FAKE_MODE=nobind
cd "$DIST_DIR/csp" && bin/csp config/csp.json -n >> "$LOG_DIR/csp.log" 2>&1 &
zombie=$!
sleep 0.5
export CSP_FAKE_MODE=normal
( _start_csp_variant csp ); rc=$?
verdict "T5a start rc=0" $rc
kill -0 "$zombie" 2>/dev/null; z=$?
[[ $z -ne 0 ]]; verdict "T5b 좀비 정리됨 (pid=$zombie)" $?
pid=$(cat "$PID_DIR/csp.pid" 2>/dev/null || echo "")
owner=$(_pid_by_port "$PORT:udp")
[[ -n "$pid" && "$pid" == "$owner" && "$pid" != "$zombie" ]]; verdict "T5c 신규 worker bind ($pid)" $?

echo "=== T6: 기동 실패(미bind) — 게이트 타임아웃 rc=1 ==="
cleanup_procs
export CSP_FAKE_MODE=nobind
t0=$SECONDS
( _start_csp_variant csp ); rc=$?
el=$((SECONDS-t0))
[[ $rc -ne 0 ]]; verdict "T6a start rc!=0" $?
[[ $el -le 10 ]]; verdict "T6b 타임아웃 내 종결 (${el}s)" $?

echo "=== T7: 즉사 바이너리 — 조기 실패 ==="
cleanup_procs
export CSP_FAKE_MODE=die
t0=$SECONDS
( _start_csp_variant csp ); rc=$?
el=$((SECONDS-t0))
[[ $rc -ne 0 ]]; verdict "T7a start rc!=0" $?
[[ $el -le 5 ]]; verdict "T7b 조기 실패 (${el}s < 타임아웃 6s)" $?

echo ""
echo "RESULT: PASS=$PASS FAIL=$FAIL"
[[ $FAIL -eq 0 ]]
