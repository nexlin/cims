#!/usr/bin/env bash
# =============================================================
# CIMS 통합 관리 스크립트
# Usage: ./cims.sh <command> [options]
# =============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── 실행 위치 자동 감지 ────────────────────────────────────────
# 소스 트리에서 실행 시:         DIST_DIR = build/dist/
# dist/ 디렉터리 안에서 실행 시: DIST_DIR = 현재 디렉터리
if [[ -f "$SCRIPT_DIR/CMakeLists.txt" ]]; then
    DIST_DIR="$SCRIPT_DIR/build/dist"
    SRC_CONSOLE="$SCRIPT_DIR/cims-console"
    SRC_PHONE="$SCRIPT_DIR/cims-phone"
else
    DIST_DIR="$SCRIPT_DIR"
    SRC_CONSOLE=""
    SRC_PHONE=""
fi

PID_DIR="$DIST_DIR/run"
LOG_DIR="$DIST_DIR/log"

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
read_pid() { local f; f="$(pidfile "$1")"; [[ -f $f ]] && cat "$f" || echo ""; }
is_running() { local pid; pid="$(read_pid "$1")"; [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null; }

kill_stray() {
    local pattern="$1"
    local port="${2:-}"
    local proto="${3:-udp}"   # udp | tcp

    # 1) 패턴으로 프로세스 종료
    local pids; pids=$(pgrep -f "$pattern" 2>/dev/null || true)
    if [[ -n $pids ]]; then
        warn "기존 프로세스 정리: $pattern (pid=$pids)"
        kill $pids 2>/dev/null || true
        local i=1
        while [[ -n $(pgrep -f "$pattern" 2>/dev/null) ]] && (( i <= 15 )); do
            sleep 0.2; i=$(( i + 1 ))
        done
        pids=$(pgrep -f "$pattern" 2>/dev/null || true)
        [[ -n $pids ]] && kill -9 $pids 2>/dev/null || true
    fi

    # 2) 포트를 점유 중인 프로세스 종료 (PID 파일 없는 좀비 대비)
    if [[ -n $port ]]; then
        local port_pids
        if [[ $proto == "tcp" ]]; then
            port_pids=$(ss -tlnp 2>/dev/null | awk -v pt="$port" 'match($4,/:([0-9]+)$/,m) && m[1]==pt {match($0,/pid=([0-9]+)/,p); if(p[1]) print p[1]}' || true)
        else
            port_pids=$(ss -ulnp 2>/dev/null | awk -v pt="$port" 'match($4,/:([0-9]+)$/,m) && m[1]==pt {match($0,/pid=([0-9]+)/,p); if(p[1]) print p[1]}' || true)
        fi
        if [[ -n $port_pids ]]; then
            warn "포트 $port ($proto) 점유 프로세스 종료: pid=$port_pids"
            kill $port_pids 2>/dev/null || true
            local i=1
            if [[ $proto == "tcp" ]]; then
                while [[ -n $(ss -tlnp 2>/dev/null | awk -v pt="$port" 'match($4,/:([0-9]+)$/,m) && m[1]==pt {print 1}') ]] && (( i <= 20 )); do
                    sleep 0.2; i=$(( i + 1 ))
                done
                port_pids=$(ss -tlnp 2>/dev/null | awk -v pt="$port" 'match($4,/:([0-9]+)$/,m) && m[1]==pt {match($0,/pid=([0-9]+)/,p); if(p[1]) print p[1]}' || true)
            else
                while [[ -n $(ss -ulnp 2>/dev/null | awk -v pt="$port" 'match($4,/:([0-9]+)$/,m) && m[1]==pt {print 1}') ]] && (( i <= 20 )); do
                    sleep 0.2; i=$(( i + 1 ))
                done
                port_pids=$(ss -ulnp 2>/dev/null | awk -v pt="$port" 'match($4,/:([0-9]+)$/,m) && m[1]==pt {match($0,/pid=([0-9]+)/,p); if(p[1]) print p[1]}' || true)
            fi
            [[ -n $port_pids ]] && kill -9 $port_pids 2>/dev/null || true
        fi
    fi
}

# ── 개별 시작 함수 ──────────────────────────────────────────────

start_cmp() {
    if is_running cmp; then warn "CMP 이미 실행 중 (pid=$(read_pid cmp))"; return 0; fi
    [[ ! -f "$DIST_DIR/cmp/bin/cmp" ]] && err "cmp 바이너리 없음: $DIST_DIR/cmp/bin/cmp (make dist 실행 필요)" && return 1
    kill_stray "cmp/bin/cmp"
    info "CMP 시작..."
    cd "$DIST_DIR/cmp"
    bin/cmp config/cmp.json >> "$LOG_DIR/cmp.log" 2>&1 &
    save_pid cmp $!
    sleep 0.8
    is_running cmp && ok "CMP 시작 완료 (pid=$(read_pid cmp))" || { err "CMP 시작 실패"; tail -3 "$LOG_DIR/cmp.log" | sed 's/^/  /'; }
}

start_csp() {
    if is_running csp; then warn "CSP 이미 실행 중 (pid=$(read_pid csp))"; return 0; fi
    [[ ! -f "$DIST_DIR/csp/bin/csp" ]] && err "csp 바이너리 없음 (make dist 실행 필요)" && return 1
    local sip_port; sip_port=$(python3 -c "import json; d=json.load(open('$DIST_DIR/csp/config/csp.json')); print(d['Setup']['Sip']['UdpPort'])" 2>/dev/null || echo 5060)
    kill_stray "csp/bin/csp" "$sip_port"
    info "CSP 시작..."
    cd "$DIST_DIR/csp"
    bin/csp config/csp.json -n >> "$LOG_DIR/csp.log" 2>&1 &
    save_pid csp $!
    sleep 1.0
    is_running csp && ok "CSP 시작 완료 (pid=$(read_pid csp))" || { err "CSP 시작 실패"; tail -3 "$LOG_DIR/csp.log" | sed 's/^/  /'; }
}

start_cwrtc() {
    if is_running cwrtc; then warn "cwrtc 이미 실행 중 (pid=$(read_pid cwrtc))"; return 0; fi
    [[ ! -f "$DIST_DIR/cwrtc/bin/cwrtc" ]] && err "cwrtc 바이너리 없음 (make dist 실행 필요)" && return 1
    local ws_port; ws_port=$(python3 -c "import json; d=json.load(open('$DIST_DIR/cwrtc/config/cwrtc.json')); print(d['Setup']['WsPort'])" 2>/dev/null || echo 8080)
    kill_stray "cwrtc/bin/cwrtc" "$ws_port"
    info "cwrtc (WebRTC 게이트웨이) 시작... (WsPort=$ws_port)"
    cd "$DIST_DIR/cwrtc"
    mkdir -p html
    bin/cwrtc config/cwrtc.json >> "$LOG_DIR/cwrtc.log" 2>&1 &
    save_pid cwrtc $!
    sleep 1.0
    is_running cwrtc && ok "cwrtc 시작 완료 (pid=$(read_pid cwrtc))" || { err "cwrtc 시작 실패"; tail -5 "$LOG_DIR/cwrtc.log" | sed 's/^/  /'; }
}

start_csc() {
    local csc_port; csc_port=$(python3 -c "import json; d=json.load(open('$DIST_DIR/csc/config/csc.json')); print(d['Server']['Port'])" 2>/dev/null || echo 4420)
    if is_running csc; then warn "CSC 이미 실행 중 (pid=$(read_pid csc))"; return 0; fi
    [[ ! -f "$DIST_DIR/csc/src/app.py" ]] && err "CSC 소스 없음 (make dist 실행 필요)" && return 1
    kill_stray "csc/src/app.py" "$csc_port" tcp
    info "CSC (REST API 서버) 시작... (port=$csc_port)"
    cd "$DIST_DIR/csc/src"
    python3 app.py >> "$LOG_DIR/csc.log" 2>&1 &
    save_pid csc $!
    sleep 1.5
    is_running csc && ok "CSC 시작 완료 (pid=$(read_pid csc))" || { err "CSC 시작 실패"; tail -3 "$LOG_DIR/csc.log" | sed 's/^/  /'; }
}

start_console() {
    if is_running console; then warn "console 이미 실행 중 (pid=$(read_pid console))"; return 0; fi
    # 포트 3001 점유 프로세스(serve 좀비 포함) 먼저 정리
    kill_stray "serve dist -l 3001" 3001 tcp
    if [[ -n "$SRC_CONSOLE" && -d "$SRC_CONSOLE" ]]; then
        # 소스 모드: Vite 개발 서버 (API proxy 포함)
        kill_stray "vite.*cims-console"
        info "console (Admin Web UI) 개발 서버 시작... (port 3001)"
        cd "$SRC_CONSOLE"
        npm run dev >> "$LOG_DIR/console.log" 2>&1 &
        save_pid console $!
    elif [[ -d "$DIST_DIR/console/dist" ]]; then
        # dist 전용 모드: 정적 서빙 (proxy 없음 — nginx 필요)
        info "console (Admin Web UI) 정적 서빙 시작... (port 3001, HTTPS)"
        cd "$DIST_DIR/console"
        _SSL_KEY="$DIST_DIR/csc/cert/server.key"
        _SSL_CERT="$DIST_DIR/csc/cert/server.crt"
        if [[ -f "$_SSL_KEY" && -f "$_SSL_CERT" ]]; then
            npx --yes serve dist -l 3001 --ssl-cert "$_SSL_CERT" --ssl-key "$_SSL_KEY" >> "$LOG_DIR/console.log" 2>&1 &
        else
            npx --yes serve dist -l 3001 >> "$LOG_DIR/console.log" 2>&1 &
        fi
        save_pid console $!
    else
        err "console 디렉터리 없음. 'cims.sh build' 실행 필요"; return 1
    fi
    sleep 2
    is_running console && ok "console 시작 완료 (pid=$(read_pid console))" || { err "console 시작 실패"; tail -3 "$LOG_DIR/console.log" | sed 's/^/  /'; }
}

start_phone() {
    if is_running phone; then warn "phone 이미 실행 중 (pid=$(read_pid phone))"; return 0; fi
    # 포트 3000 점유 프로세스(serve 좀비 포함) 먼저 정리
    kill_stray "serve dist -l 3000" 3000 tcp
    if [[ -n "$SRC_PHONE" && -d "$SRC_PHONE" ]]; then
        # 소스 모드: Vite 개발 서버 (API proxy 포함)
        kill_stray "vite.*cims-phone"
        info "phone (MCPTT UE Web) 개발 서버 시작... (port 3000)"
        cd "$SRC_PHONE"
        npm run dev >> "$LOG_DIR/phone.log" 2>&1 &
        save_pid phone $!
    elif [[ -d "$DIST_DIR/phone/dist" ]]; then
        # dist 전용 모드: 정적 서빙 (proxy 없음 — nginx 필요)
        info "phone (MCPTT UE Web) 정적 서빙 시작... (port 3000, HTTPS)"
        cd "$DIST_DIR/phone"
        _SSL_KEY="$DIST_DIR/csc/cert/server.key"
        _SSL_CERT="$DIST_DIR/csc/cert/server.crt"
        if [[ -f "$_SSL_KEY" && -f "$_SSL_CERT" ]]; then
            npx --yes serve dist -l 3000 --ssl-cert "$_SSL_CERT" --ssl-key "$_SSL_KEY" >> "$LOG_DIR/phone.log" 2>&1 &
        else
            npx --yes serve dist -l 3000 >> "$LOG_DIR/phone.log" 2>&1 &
        fi
        save_pid phone $!
    else
        err "phone 디렉터리 없음. 'cims.sh build' 실행 필요"; return 1
    fi
    sleep 2
    is_running phone && ok "phone 시작 완료 (pid=$(read_pid phone))" || { err "phone 시작 실패"; tail -3 "$LOG_DIR/phone.log" | sed 's/^/  /'; }
}

# ── 중지 ───────────────────────────────────────────────────────
stop_one() {
    local name="$1"
    local pid; pid="$(read_pid "$name")"
    if [[ -z $pid ]]; then warn "$name: PID 파일 없음"; return 0; fi
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        local i=1
        while kill -0 "$pid" 2>/dev/null && (( i <= 20 )); do sleep 0.2; i=$(( i + 1 )); done
        kill -0 "$pid" 2>/dev/null && { kill -9 "$pid" 2>/dev/null || true; }
        ok "$name 중지 완료 (pid=$pid)"
    else
        warn "$name: 이미 중지됨 (pid=$pid)"
    fi
    rm -f "$(pidfile "$name")"
}

stop_csc() {
    local csc_port; csc_port=$(python3 -c "import json; d=json.load(open('$DIST_DIR/csc/config/csc.json')); print(d['Server']['Port'])" 2>/dev/null || echo 4420)
    stop_one csc
    # PID 파일 없이 남아있는 스트레이 프로세스도 정리
    kill_stray "csc/src/app.py" "$csc_port" tcp
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
    echo -e "  실행 디렉터리: ${CYAN}$DIST_DIR${NC}"
    echo ""
    status_one cmp
    status_one csp
    status_one cwrtc
    status_one csc
    status_one console
    status_one phone
    echo ""
}

# ── 빌드 ───────────────────────────────────────────────────────
cmd_build() {
    [[ -z "$SRC_CONSOLE" ]] && err "build 명령은 소스 트리에서만 실행 가능" && exit 1
    header "=== C++ 빌드 ==="
    local jobs=${1:-$(nproc)}
    mkdir -p "$SCRIPT_DIR/build"
    cd "$SCRIPT_DIR/build"
    cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo > "$LOG_DIR/cmake.log" 2>&1
    make -j"$jobs" 2>&1 | tee "$LOG_DIR/make.log" | grep -E "^\[|error:|Error" | tail -20
    ok "C++ 빌드 완료"

    header "=== dist 패키지 생성 ==="
    make dist 2>&1 | tee -a "$LOG_DIR/make.log" | tail -5
    ok "dist 생성 완료 → $DIST_DIR"

    header "=== Web UI 빌드 (cims-console) ==="
    cd "$SRC_CONSOLE"
    npm install --silent
    npm run build
    cp -r dist "$DIST_DIR/console/"
    ok "cims-console 빌드 완료"

    header "=== Web UI 빌드 (cims-phone) ==="
    cd "$SRC_PHONE"
    npm install --silent
    npm run build
    cp -r dist "$DIST_DIR/phone/"
    ok "cims-phone 빌드 완료"

    echo ""
    ok "전체 빌드 완료 → $DIST_DIR"
    echo ""
    info "다음 단계: ./configure.sh --local-ip <서버IP> [--db-password <PW>]"
}

# ── configure ──────────────────────────────────────────────────
cmd_configure() {
    if [[ -n "$SRC_CONSOLE" ]]; then
        # Source tree mode
        "$SCRIPT_DIR/configure.sh" "$@"
    else
        # Dist mode
        "$SCRIPT_DIR/configure.sh" "$@"
    fi
}

# ── cspsim ─────────────────────────────────────────────────────
cmd_sim() {
    local mode="voip" scenario="call" count=2
    local user="1001" domain="csp" password="1234" group="1000"
    local server_ip; server_ip=$(python3 -c "import json; d=json.load(open('$DIST_DIR/csp/config/csp.json')); print(d['Setup']['Sip']['LocalIp'])" 2>/dev/null || echo "127.0.0.1")
    local duration=10

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
            -ip)       server_ip="$2"; shift 2 ;;
            *)         break ;;
        esac
    done

    header "=== cspsim 실행 ==="
    info "mode=$mode  scenario=$scenario  count=$count"
    info "server=$server_ip:5060  duration=${duration}s"
    echo ""
    cd "$DIST_DIR/cspsim"
    bin/cspsim \
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
    local clog
    clog=$(ls -t "$LOG_DIR/${name}_"*.log 2>/dev/null | head -1)
    if [[ -n $clog ]]; then
        tail -f "$clog"
    else
        local logfile="$LOG_DIR/${name}.log"
        [[ ! -f $logfile ]] && err "로그 파일 없음: $logfile" && exit 1
        tail -f "$logfile"
    fi
}

# ── 도움말 ─────────────────────────────────────────────────────
usage() {
    cat <<EOF
${BOLD}CIMS 통합 관리 스크립트${NC}

사용법: $(basename "$0") <command> [options]

${BOLD}서비스 명령:${NC}
  start  [cmp|csp|cwrtc|csc|console|phone|all]  서비스 시작 (기본: all)
  stop   [name|all]                               서비스 중지
  restart [name|all]                              재시작
  status                                          상태 확인

${BOLD}빌드 & 설정:${NC}
  build  [-j N]        C++ + Web UI 빌드 후 dist 생성 (소스 트리에서만)
  configure [options]  서버 IP/DB 설정 → configure.sh 에 위임

${BOLD}시뮬레이터:${NC}
  sim [options]
    -mode     voip|ptt
    -scenario register|call|group-call|full
    -count    N
    -ip       IP   (CSP 서버 IP)
    -duration SEC

${BOLD}로그:${NC}
  log [cmp|csp|cwrtc|csc|console|phone]

${BOLD}예시:${NC}
  # 빌드 → 설정 → 시작 (소스 트리)
  $(basename "$0") build
  ./configure.sh --local-ip 192.168.1.10 --db-password secret
  $(basename "$0") start

  # 배포 서버에서 (dist/ 내부)
  ./configure.sh --local-ip 192.168.1.10
  ./cims.sh start

  $(basename "$0") stop all
  $(basename "$0") status
  $(basename "$0") sim -mode ptt -scenario group-call -count 4
  $(basename "$0") log csp
EOF
}

# ── 메인 ───────────────────────────────────────────────────────
COMPONENTS=(cmp csp cwrtc csc console phone)

cmd_start() {
    local target="${1:-all}"
    case "$target" in
        all)     start_cmp; start_csp; sleep 0.5; start_cwrtc; start_csc; start_console; start_phone ;;
        cmp)     start_cmp ;;
        csp)     start_csp ;;
        cwrtc)   start_cwrtc ;;
        csc)     start_csc ;;
        console) start_console ;;
        phone)   start_phone ;;
        *) err "알 수 없는 컴포넌트: $target"; exit 1 ;;
    esac
}

cmd_stop() {
    local target="${1:-all}"
    if [[ $target == "all" ]]; then
        header "=== 전체 중지 ==="
        for c in "${COMPONENTS[@]}"; do
            if [[ $c == "csc" ]]; then stop_csc; else stop_one "$c"; fi
        done
    elif [[ $target == "csc" ]]; then
        stop_csc
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
    start)     shift; header "=== CIMS 시작 ==="; cmd_start "${1:-all}"; echo ""; cmd_status ;;
    stop)      shift; cmd_stop "${1:-all}"; echo ""; cmd_status ;;
    restart)   shift; header "=== CIMS 재시작 ==="; cmd_restart "${1:-all}"; echo ""; cmd_status ;;
    status)    cmd_status ;;
    build)     shift; cmd_build "${1:-}" ;;
    configure) shift; cmd_configure "$@" ;;
    sim)       shift; cmd_sim "$@" ;;
    log)       shift; cmd_log "${1:-csp}" ;;
    help|--help|-h) usage ;;
    "") usage ;;
    *) err "알 수 없는 명령: $1"; echo ""; usage; exit 1 ;;
esac
