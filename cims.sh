#!/usr/bin/env bash
# =============================================================
# CIMS 통합 관리 스크립트
# Usage: ./cims.sh <command> [options]
# =============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build/bin"
RUN_DIR="$SCRIPT_DIR/test_run"
CSC_DIR="$SCRIPT_DIR/csc/bin/csc_pihttp/src"
CLIENT_DIR="$SCRIPT_DIR/csc_client"

PID_DIR="$RUN_DIR/run"
LOG_DIR="$RUN_DIR/log"

mkdir -p "$PID_DIR" "$LOG_DIR"

# ── 색상 ───────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()    { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()      { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()     { echo -e "${RED}[ERROR]${NC} $*" >&2; }
header()  { echo -e "\n${BOLD}$*${NC}"; }

# ── PID 파일 헬퍼 ──────────────────────────────────────────────
pidfile() { echo "$PID_DIR/$1.pid"; }

save_pid() { echo "$2" > "$(pidfile "$1")"; }

read_pid() {
    local f; f="$(pidfile "$1")"
    [[ -f $f ]] && cat "$f" || echo ""
}

is_running() {
    local pid; pid="$(read_pid "$1")"
    [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null
}

# 프로세스명으로 stray 프로세스 강제 종료 후 포트 해제 대기
kill_stray() {
    local pattern="$1"
    local port="${2:-}"
    local pids; pids=$(pgrep -f "$pattern" 2>/dev/null || true)
    if [[ -n $pids ]]; then
        warn "기존 프로세스 정리: $pattern (pid=$pids)"
        kill $pids 2>/dev/null || true
        # 프로세스 종료 대기 (최대 3초)
        local i=1
        while [[ -n $(pgrep -f "$pattern" 2>/dev/null) ]] && (( i <= 15 )); do
            sleep 0.2; i=$(( i + 1 ))
        done
        # 포트가 지정된 경우 포트 해제까지 추가 대기
        if [[ -n $port ]]; then
            i=1
            while ss -tlnp | grep -q ":${port} " && (( i <= 10 )); do
                sleep 0.2; i=$(( i + 1 ))
            done
        fi
    fi
}

# ── 개별 시작 함수 ──────────────────────────────────────────────

start_cmp() {
    if is_running cmp; then warn "CMP 이미 실행 중 (pid=$(read_pid cmp))"; return 0; fi
    kill_stray "build/bin/cmp"
    info "CMP 시작..."
    cd "$RUN_DIR"
    "$BUILD_DIR/cmp" cmp.json > "$LOG_DIR/cmp.log" 2>&1 &
    save_pid cmp $!
    sleep 0.8
    is_running cmp && ok "CMP 시작 완료 (pid=$(read_pid cmp))" || { err "CMP 시작 실패"; tail -3 "$LOG_DIR/cmp.log" | sed 's/^/  /'; }
}

start_csp() {
    if is_running csp; then warn "CSP 이미 실행 중 (pid=$(read_pid csp))"; return 0; fi
    local sip_port; sip_port=$(python3 -c "import json; d=json.load(open('$RUN_DIR/csp.json')); print(d['Setup']['Sip']['UdpPort'])" 2>/dev/null || echo 5060)
    kill_stray "build/bin/csp" "$sip_port"
    info "CSP 시작..."
    cd "$RUN_DIR"
    "$BUILD_DIR/csp" csp.json -n > "$LOG_DIR/csp.log" 2>&1 &
    save_pid csp $!
    sleep 1.0
    is_running csp && ok "CSP 시작 완료 (pid=$(read_pid csp))" || { err "CSP 시작 실패"; tail -3 "$LOG_DIR/csp.log" | sed 's/^/  /'; }
}

start_csc() {
    if is_running csc; then warn "CSC 이미 실행 중 (pid=$(read_pid csc))"; return 0; fi
    kill_stray "$CSC_DIR/app.py"
    info "CSC (REST API 서버) 시작..."
    cd "$CSC_DIR"
    python3 "$CSC_DIR/app.py" >> "$LOG_DIR/csc.log" 2>&1 &
    save_pid csc $!
    sleep 1.5
    is_running csc && ok "CSC 시작 완료 (pid=$(read_pid csc))" || { err "CSC 시작 실패"; tail -3 "$LOG_DIR/csc.log" | sed 's/^/  /'; }
}

start_cwrtc() {
    if is_running cwrtc; then warn "cwrtc 이미 실행 중 (pid=$(read_pid cwrtc))"; return 0; fi
    local ws_port; ws_port=$(python3 -c "import json; d=json.load(open('$RUN_DIR/cwrtc.json')); print(d['Setup']['WsPort'])" 2>/dev/null || echo 8080)
    kill_stray "build/bin/cwrtc" "$ws_port"
    info "cwrtc (WebRTC 게이트웨이) 시작... (WsPort=$ws_port)"
    cd "$RUN_DIR"
    mkdir -p "$RUN_DIR/html"   # doc root 없으면 경고 메시지 방지
    # 로그는 시작 시 새로 씀 (이전 실패 로그 누적 방지)
    "$BUILD_DIR/cwrtc" cwrtc.json > "$LOG_DIR/cwrtc.log" 2>&1 &
    save_pid cwrtc $!
    sleep 1.0
    is_running cwrtc && ok "cwrtc 시작 완료 (pid=$(read_pid cwrtc))" || { err "cwrtc 시작 실패"; tail -5 "$LOG_DIR/cwrtc.log" | sed 's/^/  /'; }
}

start_client() {
    if is_running client; then warn "csc_client 이미 실행 중 (pid=$(read_pid client))"; return 0; fi
    kill_stray "vite.*csc_client"
    info "csc_client (Web UI) 시작..."
    cd "$CLIENT_DIR"
    npm run dev >> "$LOG_DIR/client.log" 2>&1 &
    save_pid client $!
    sleep 2
    is_running client && ok "csc_client 시작 완료 (pid=$(read_pid client))" || { err "csc_client 시작 실패"; tail -3 "$LOG_DIR/client.log" | sed 's/^/  /'; }
}

# ── 중지 ───────────────────────────────────────────────────────

stop_one() {
    local name="$1"
    local pid; pid="$(read_pid "$name")"
    if [[ -z $pid ]]; then warn "$name: PID 파일 없음"; return 0; fi
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        local i=1
        while kill -0 "$pid" 2>/dev/null && (( i <= 20 )); do
            sleep 0.2
            i=$(( i + 1 ))
        done
        kill -0 "$pid" 2>/dev/null && { kill -9 "$pid" 2>/dev/null || true; }
        ok "$name 중지 완료 (pid=$pid)"
    else
        warn "$name: 이미 중지됨 (pid=$pid)"
    fi
    rm -f "$(pidfile "$name")"
}

# ── 상태 출력 ──────────────────────────────────────────────────

status_one() {
    local name="$1"
    local pid; pid="$(read_pid "$name")"
    if [[ -z $pid ]]; then
        echo -e "  ${RED}●${NC} $(printf '%-12s' "$name")  중지됨"
    elif kill -0 "$pid" 2>/dev/null; then
        echo -e "  ${GREEN}●${NC} $(printf '%-12s' "$name")  실행 중  (pid=$pid)"
    else
        echo -e "  ${YELLOW}●${NC} $(printf '%-12s' "$name")  비정상 종료 (pid=$pid)"
        rm -f "$(pidfile "$name")"
    fi
}

cmd_status() {
    header "=== CIMS 상태 ==="
    status_one cmp
    status_one csp
    status_one csc
    status_one cwrtc
    status_one client
    echo ""
}

# ── 빌드 ───────────────────────────────────────────────────────

cmd_build() {
    header "=== 빌드 시작 ==="
    local jobs=${1:-$(nproc)}
    mkdir -p "$SCRIPT_DIR/build"
    cd "$SCRIPT_DIR/build"
    cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$RUN_DIR" > "$LOG_DIR/cmake.log" 2>&1
    make -j"$jobs" 2>&1 | tee "$LOG_DIR/make.log" | grep -E "^\[|error:|Error" | tail -20
    ok "빌드 완료 → $BUILD_DIR"

    header "=== Web UI 빌드 ==="
    cd "$CLIENT_DIR"
    npm install --silent
    npm run build
    ok "Web UI 빌드 완료"
}

# ── cspsim ─────────────────────────────────────────────────────

cmd_sim() {
    # 기본값
    local mode="voip" scenario="call" count=2
    local user="1001" domain="csp" password="1234" group="1000"
    local server_ip; server_ip=$(python3 -c "import json; d=json.load(open('$RUN_DIR/csp.json')); print(d['Setup']['Sip']['LocalIp'])" 2>/dev/null || echo "127.0.0.1")
    local duration=10

    # 파라미터 파싱
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -mode)     mode="$2";     shift 2 ;;
            -scenario) scenario="$2"; shift 2 ;;
            -count)    count="$2";    shift 2 ;;
            -user)     user="$2";     shift 2 ;;
            -domain)   domain="$2";   shift 2 ;;
            -password) password="$2"; shift 2 ;;
            -group)    group="$2";    shift 2 ;;
            -duration) duration="$2"; shift 2 ;;
            -ip)       server_ip="$2";shift 2 ;;
            *)         break ;;
        esac
    done

    header "=== cspsim 실행 ==="
    info "mode=$mode  scenario=$scenario  count=$count  user=$user  domain=$domain"
    info "server=$server_ip:5060  duration=${duration}s"
    echo ""
    cd "$RUN_DIR"
    "$BUILD_DIR/cspsim" \
        -server_ip "$server_ip" \
        -count "$count" \
        -user "$user" \
        -domain "$domain" \
        -password "$password" \
        -mode "$mode" \
        -group "$group" \
        -scenario "$scenario" \
        -call_duration "$duration" \
        "$@"
}

# ── 로그 보기 ──────────────────────────────────────────────────

cmd_log() {
    local name="${1:-csp}"
    # csp/cwrtc/cspsim: CLog가 {name}_YYYYMMDD_N.log 형식으로 기록
    # cmp/csc/client: stdout 리디렉션 파일 사용
    local clog
    clog=$(ls -t "$LOG_DIR/${name}_"*.log 2>/dev/null | head -1)
    if [[ -n $clog ]]; then
        tail -f "$clog"
    else
        local logfile="$LOG_DIR/${name}.log"
        [[ ! -f $logfile ]] && err "로그 파일 없음: $logfile (${name}_YYYYMMDD_N.log 도 없음)" && exit 1
        tail -f "$logfile"
    fi
}

# ── 도움말 ─────────────────────────────────────────────────────

usage() {
    cat <<EOF
${BOLD}CIMS 통합 관리 스크립트${NC}

사용법: $(basename "$0") <command> [options]

${BOLD}서비스 명령:${NC}
  start [cmp|csp|csc|cwrtc|client|all]   서비스 시작 (기본: all)
  stop  [cmp|csp|csc|cwrtc|client|all]   서비스 중지 (기본: all)
  restart [name|all]                      재시작
  status                                  상태 확인

${BOLD}빌드:${NC}
  build [-j N]                            C++ + Web UI 빌드

${BOLD}시뮬레이터:${NC}
  sim [options]                           cspsim 실행
    -mode     voip|ptt        (기본: voip)
    -scenario register|call|group-call|full  (기본: call)
    -count    N               단말 수 (기본: 2)
    -user     ID              시작 사용자 ID (기본: 1001)
    -duration SEC             통화 시간 초 (기본: 10)
    -ip       IP              CSP 서버 IP (기본: csp.json에서)

${BOLD}로그:${NC}
  log [cmp|csp|csc|cwrtc|client]         로그 실시간 보기 (기본: csp)

${BOLD}예시:${NC}
  $(basename "$0") start                          # 전체 시작
  $(basename "$0") start csp                      # CSP만 시작
  $(basename "$0") stop all                       # 전체 중지
  $(basename "$0") status                         # 상태 확인
  $(basename "$0") sim -mode ptt -scenario group-call -count 4
  $(basename "$0") log csp
EOF
}

# ── 메인 ───────────────────────────────────────────────────────

COMPONENTS=(cmp csp csc cwrtc client)

cmd_start() {
    local target="${1:-all}"
    case "$target" in
        all)    start_cmp; start_csp; sleep 0.5; start_csc; start_cwrtc; start_client ;;
        cmp)    start_cmp ;;
        csp)    start_csp ;;
        csc)    start_csc ;;
        cwrtc)  start_cwrtc ;;
        client) start_client ;;
        *) err "알 수 없는 컴포넌트: $target"; exit 1 ;;
    esac
}

cmd_stop() {
    local target="${1:-all}"
    if [[ $target == "all" ]]; then
        header "=== 전체 중지 ==="
        for c in "${COMPONENTS[@]}"; do stop_one "$c"; done
    else
        stop_one "$target"
    fi
}

cmd_restart() {
    local target="${1:-all}"
    cmd_stop "$target"
    sleep 1
    cmd_start "$target"
}

case "${1:-}" in
    start)   shift; header "=== CIMS 시작 ==="; cmd_start "${1:-all}";  echo ""; cmd_status ;;
    stop)    shift; cmd_stop "${1:-all}"; echo ""; cmd_status ;;
    restart) shift; header "=== CIMS 재시작 ==="; cmd_restart "${1:-all}"; echo ""; cmd_status ;;
    status)  cmd_status ;;
    build)   shift; cmd_build "${1:-}" ;;
    sim)     shift; cmd_sim "$@" ;;
    log)     shift; cmd_log "${1:-csp}" ;;
    help|--help|-h) usage ;;
    "")      usage ;;
    *)       err "알 수 없는 명령: $1"; echo ""; usage; exit 1 ;;
esac
