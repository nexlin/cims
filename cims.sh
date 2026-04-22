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
    [[ ! -f "$DIST_DIR/csc/src/csc_app.py" ]] && err "CSC 소스 없음 (make dist 실행 필요)" && return 1
    kill_stray "csc/src/csc_app.py" "$csc_port" tcp
    info "CSC (REST API 서버) 시작... (port=$csc_port)"
    cd "$DIST_DIR/csc/src"
    python3 csc_app.py >> "$LOG_DIR/csc.log" 2>&1 &
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
    kill_stray "csc/src/csc_app.py" "$csc_port" tcp
}

# ── 상태 출력 ──────────────────────────────────────────────────
# 컴포넌트별 리스닝 포트 (외부 기동 감지용)
_svc_port_proto() {
    case "$1" in
        cmp)     echo "9000:udp" ;;
        csp)     echo "5060:udp" ;;
        cwrtc)   echo "8080:tcp" ;;
        csc)     echo "4420:tcp" ;;
        console) echo "3001:tcp" ;;
        phone)   echo "3000:tcp" ;;
        *)       echo "" ;;
    esac
}

# 포트를 점유 중인 프로세스 PID 반환 (없으면 빈 문자열)
_pid_by_port() {
    local pp="$1" port proto
    port="${pp%%:*}"; proto="${pp##*:}"
    [[ -z $port ]] && return
    # ss -H: no header,  -lnp: listening + numeric + processes
    local line pid
    if [[ $proto == "udp" ]]; then
        line=$(ss -Hulnp 2>/dev/null | awk -v p=":$port" '$5 ~ p {print; exit}')
    else
        line=$(ss -Htlnp 2>/dev/null | awk -v p=":$port" '$4 ~ p {print; exit}')
    fi
    [[ -z $line ]] && return
    pid=$(echo "$line" | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
    echo "$pid"
}

status_one() {
    local name="$1"
    local pid; pid="$(read_pid "$name")"
    if [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null; then
        echo -e "  ${GREEN}●${NC} $(printf '%-12s' "$name")  실행 중  (pid=$pid)"
        return
    fi
    # PID 파일 있는데 프로세스 없음 — 비정상 종료
    if [[ -n $pid ]]; then
        echo -e "  ${YELLOW}●${NC} $(printf '%-12s' "$name")  비정상 종료 (pid=$pid)"
        rm -f "$(pidfile "$name")"
        return
    fi
    # PID 파일 없음 — 포트 리스너로 외부 기동 여부 확인 (B: cims.sh 밖에서 기동된 경우)
    local pp; pp="$(_svc_port_proto "$name")"
    if [[ -n $pp ]]; then
        local ext_pid; ext_pid="$(_pid_by_port "$pp")"
        if [[ -n $ext_pid ]]; then
            echo -e "  ${YELLOW}●${NC} $(printf '%-12s' "$name")  실행 중(외부)  (pid=$ext_pid, port=${pp%:*})"
            return
        fi
    fi
    echo -e "  ${RED}●${NC} $(printf '%-12s' "$name")  중지됨"
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
    # 인자 파싱: "-j N" / "-jN" / "N" / --no-pkg / -v <ver> / -m <msg>
    local jobs=""
    local do_pkg=1
    local pkg_version=""
    local pkg_changelog=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -j)         shift; jobs="${1:-}"; shift ;;
            -j*)        jobs="${1#-j}"; shift ;;
            --no-pkg)   do_pkg=0; shift ;;
            -v|--version) pkg_version="$2"; shift 2 ;;
            -m|--changelog) pkg_changelog="$2"; shift 2 ;;
            [0-9]*)     jobs="$1"; shift ;;
            *)          shift ;;
        esac
    done
    [[ -z "$jobs" ]] && jobs=$(nproc)
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

    # 배포 패키지 자동 생성 (기본 ON, --no-pkg 로 스킵)
    if [[ $do_pkg -eq 1 ]]; then
        echo ""
        header "=== 배포 패키지 생성 (자동) ==="
        local pkg_args=()
        [[ -n $pkg_version   ]] && pkg_args+=(-v "$pkg_version")
        [[ -n $pkg_changelog ]] && pkg_args+=(-m "$pkg_changelog")
        cmd_pkg "${pkg_args[@]}"
    fi

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

# ── 데이터 정리 ───────────────────────────────────────────────
cmd_clean() {
    local target="${1:-all}"

    header "=== 데이터 정리 ==="

    if [[ $target == "all" || $target == "log" ]]; then
        info "로그 정리..."
        rm -f "$LOG_DIR"/cmp.log "$LOG_DIR"/cmp_*.log
        rm -f "$LOG_DIR"/csp.log "$LOG_DIR"/csp_*.log
        rm -f "$LOG_DIR"/cwrtc.log "$LOG_DIR"/csc.log
        ok "로그 정리 완료"
    fi

    if [[ $target == "all" || $target == "data" ]]; then
        info "서비스 이력/녹취/메시지 로그 정리..."
        rm -rf "$DIST_DIR/ext_mnt/service_log"
        rm -rf "$DIST_DIR/ext_mnt/msg_log"
        mkdir -p "$DIST_DIR/ext_mnt/service_log" "$DIST_DIR/ext_mnt/msg_log"
        ok "서비스 데이터 정리 완료"
    fi

    echo ""
}

# ── cspsim ─────────────────────────────────────────────────────
cmd_sim() {
    local orig_dir="$PWD"
    local mode="voip" scenario="call" count="" use_db=true
    local user="" domain="" password="" group=""
    local server_ip; server_ip=$(python3 -c "import json; d=json.load(open('$DIST_DIR/csp/config/csp.json')); print(d['Setup']['Sip']['LocalIp'])" 2>/dev/null || echo "127.0.0.1")
    local duration=10
    local do_clean=false
    local run_bg=false
    local extra_args=()

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
            -no-db)    use_db=false;   shift ;;
            --clean)   do_clean=true;  shift ;;
            --bg)      run_bg=true;    shift ;;
            *)         extra_args+=("$1"); shift ;;
        esac
    done

    # --clean 플래그 시 정리 후 서비스 재시작
    if $do_clean; then
        cmd_clean all
        for svc in csp cmp; do
            if is_running "$svc"; then stop_one "$svc" > /dev/null 2>&1; fi
        done
        sleep 0.5
        start_cmp
        start_csp
        echo ""
    fi

    header "=== cspsim 실행 ==="

    # DB 모드: csp.json에서 가입자 정보 자동 로드
    local db_arg=""
    if $use_db && [[ -f "$DIST_DIR/csp/config/csp.json" ]]; then
        db_arg="-db $DIST_DIR/csp/config/csp.json"
        info "DB 모드: csp.json에서 가입자 정보 자동 로드"

        # PTT 모드에서 그룹 미지정 시 DB에서 첫 번째 그룹 자동 설정
        if [[ $mode == "ptt" && -z "$group" ]]; then
            group=$(python3 -c "
import json, pymysql
d=json.load(open('$DIST_DIR/csp/config/csp.json'))
db=d['Setup']['Database']
c=pymysql.connect(host=db['Host'],port=db['Port'],user=db['User'],password=db['Password'],database=db['DbName'])
cur=c.cursor(); cur.execute('SELECT id FROM ptt_groups ORDER BY id LIMIT 1')
r=cur.fetchone(); print(r[0] if r else ''); c.close()
" 2>/dev/null || true)
            [[ -n "$group" ]] && info "PTT 그룹 자동 감지: $group"
        fi
    fi
    group="${group:-1000}"

    local sim_args=(
        -server_ip "$server_ip"
        -mode "$mode"
        -scenario "$scenario"
        -call_duration "$duration"
        -group "$group"
    )

    # DB 모드일 때 -db 추가, user/domain/password 미지정이면 DB에서 자동
    if [[ -n "$db_arg" ]]; then
        sim_args+=($db_arg)
    fi
    # 명시적 지정된 옵션만 전달
    [[ -n "$count" ]]    && sim_args+=(-count "$count")
    [[ -n "$user" ]]     && sim_args+=(-user "$user")
    [[ -n "$domain" ]]   && sim_args+=(-domain "$domain")
    [[ -n "$password" ]] && sim_args+=(-password "$password")

    # 미디어 파일 미지정 시 기본 미디어 디렉터리 자동 설정 (AMR-WB + H.264)
    # -no_video 가 있으면 비디오 파일이 없는 음성 전용 미디어 경로 사용
    local has_media=false
    local no_video=false
    for arg in "${extra_args[@]+"${extra_args[@]}"}"; do
        case "$arg" in
            -media_file|-media_dir|-video_file) has_media=true ;;
            -no_video|--no-video)               no_video=true ;;
        esac
    done
    if ! $has_media && [[ -d "$DIST_DIR/cspsim/media" ]]; then
        if $no_video && [[ -d "$DIST_DIR/cspsim/media/audio_only" ]]; then
            extra_args+=(-media_dir "$DIST_DIR/cspsim/media/audio_only")
            info "음성 전용 미디어 디렉터리 사용 (-no_video): $DIST_DIR/cspsim/media/audio_only"
        elif $no_video; then
            # 음성 파일만 찾기: *_audio.amrwb 중 첫 파일 선택 (media_file 옵션)
            local audio_file
            audio_file=$(ls "$DIST_DIR/cspsim/media/"*_audio.amrwb 2>/dev/null | head -1)
            if [[ -n "$audio_file" ]]; then
                extra_args+=(-media_file "$audio_file")
                info "음성 전용 미디어 파일 사용 (-no_video): $audio_file"
            fi
        else
            extra_args+=(-media_dir "$DIST_DIR/cspsim/media")
            info "기본 미디어 디렉터리 사용: $DIST_DIR/cspsim/media"
        fi
    fi

    # extra_args 내 경로 옵션(-media_dir, -media_file, -video_file)을 절대경로로 변환
    local resolved_extra=()
    local i=0
    while [[ $i -lt ${#extra_args[@]} ]]; do
        case "${extra_args[$i]}" in
            -media_dir|-media_file|-video_file)
                resolved_extra+=("${extra_args[$i]}")
                i=$((i+1))
                if [[ $i -lt ${#extra_args[@]} ]]; then
                    resolved_extra+=("$(cd "$orig_dir" 2>/dev/null && realpath "${extra_args[$i]}" 2>/dev/null || echo "${extra_args[$i]}")")
                fi
                ;;
            *) resolved_extra+=("${extra_args[$i]}") ;;
        esac
        i=$((i+1))
    done

    info "mode=$mode  scenario=$scenario  server=$server_ip:5060  duration=${duration}s"
    echo ""
    cd "$DIST_DIR/cspsim"

    if $run_bg; then
        bin/cspsim "${sim_args[@]}" "${resolved_extra[@]+"${resolved_extra[@]}"}" >> "$LOG_DIR/cspsim_${mode}_$(date +%H%M%S).log" 2>&1 &
        local sim_pid=$!
        ok "cspsim 백그라운드 실행 (pid=$sim_pid, mode=$mode)"
        info "로그: $LOG_DIR/cspsim_${mode}_$(date +%H%M%S).log"
        return 0
    fi

    bin/cspsim "${sim_args[@]}" "${resolved_extra[@]+"${resolved_extra[@]}"}"

    # 검증 결과 출력
    echo ""
    header "=== 검증 결과 ==="

    # 녹취 파일 확인
    local rec_files; rec_files=$(find "$DIST_DIR/ext_mnt/service_log" -name "seg_*.rtp" -size +0 2>/dev/null | wc -l)
    local rec_zero;  rec_zero=$(find "$DIST_DIR/ext_mnt/service_log" -name "seg_*.rtp" -size 0 2>/dev/null | wc -l)
    if [[ $rec_files -gt 0 ]]; then
        ok "녹취: ${rec_files}개 파일 정상"
        find "$DIST_DIR/ext_mnt/service_log" -name "seg_*.rtp" -size +0 -exec ls -lh {} \; 2>/dev/null | sed 's/^/  /'
    elif [[ $rec_zero -gt 0 ]]; then
        err "녹취: ${rec_zero}개 파일 0바이트"
    else
        warn "녹취: 파일 없음"
    fi

    # CMP 로그에서 Symmetric RTP 확인
    local sym_count; sym_count=$(grep -c "Symmetric RTP" "$LOG_DIR"/cmp*.log 2>/dev/null || true); sym_count=${sym_count:-0}
    [[ $sym_count -gt 0 ]] && info "Symmetric RTP IP 학습: ${sym_count}회"

    echo ""
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
  build  [-j N] [-v X.Y.Z] [-m "note"] [--no-pkg]
                       C++ + Web UI 빌드 → dist → (자동) 배포 패키지 생성
                       -v 생략 시 각 모듈 pkg.json 의 patch 가 auto-bump (예: 0.0.3 → 0.0.4)
                       -v 지정 시 모든 모듈 해당 버전 + pkg.json 에 반영
                       --no-pkg 면 패키지 생성 건너뜀
  sync   [targets]     C++ 빌드 없이 Python/스크립트/메타만 dist 로 복사
                       targets: csc | agent | scripts | pkg-meta | console | phone | all
                       (기본: all — C++ 제외. 예: ./cims.sh sync csc && ./cims.sh restart csc)
  configure [options]  서버 IP/DB 설정 → configure.sh 에 위임

${BOLD}시뮬레이터:${NC}
  sim [options]
    -mode     voip|ptt
    -scenario register|call|group_call|full
    -count    N       (미지정 시 DB 가입자 전체)
    -group    ID      (PTT 그룹 ID, 미지정 시 DB 첫 번째 그룹)
    -ip       IP      (CSP 서버 IP, 미지정 시 csp.json에서)
    -duration SEC
    -no-db            (DB 미사용, 수동 지정 모드)
    --clean           (실행 전 데이터 정리 + 서비스 재시작)
    --bg              (백그라운드 실행, 동시 여러 인스턴스 가능)

${BOLD}데이터 정리:${NC}
  clean [all|log|data]   로그/서비스이력/녹취 삭제 (기본: all)

${BOLD}로그:${NC}
  log [cmp|csp|cwrtc|csc|console|phone]

${BOLD}배포 패키지 (Console 업로드용):${NC}
  pkg [-v X.Y.Z] [--no-bump] [-m <changelog>] [name...]
                                 tar.gz 생성 + meta.json 포함. 기본: auto-bump patch.
                                 -v 지정 시 해당 버전 강제 + pkg.json 반영
                                 --no-bump 면 현재 pkg.json 버전 그대로 (재패키징)
                                 예: ./cims.sh pkg               # 0.0.3 → 0.0.4 자동
                                     ./cims.sh pkg -v 1.0.0 csp  # csp 만 1.0.0 강제

${BOLD}예시:${NC}
  # 빌드 → 설정 → 시작 (소스 트리)
  $(basename "$0") build
  ./configure.sh --local-ip 192.168.1.10 --db-password secret
  $(basename "$0") start

  # 배포 서버에서 (dist/ 내부)
  ./configure.sh --local-ip 192.168.1.10
  ./cims.sh start

  # 시뮬레이터 (동시 실행)
  $(basename "$0") clean                                  # 데이터 정리
  $(basename "$0") sim -mode ptt -group +82571910001      # 영상 PTT 포그라운드
  $(basename "$0") sim -mode ptt -group +82571910001 --bg # 영상 PTT 백그라운드
  $(basename "$0") sim -mode voip --bg                    # VoIP 동시 실행

  $(basename "$0") stop all
  $(basename "$0") status
  $(basename "$0") log csp
EOF
}

# ── 메인 ───────────────────────────────────────────────────────
COMPONENTS=(cmp csp cwrtc csc console phone)

_start_one() {
    case "$1" in
        all)     start_cmp; start_csp; sleep 0.5; start_cwrtc; start_csc; start_console; start_phone ;;
        cmp)     start_cmp ;;
        csp)     start_csp ;;
        cwrtc)   start_cwrtc ;;
        csc)     start_csc ;;
        console) start_console ;;
        phone)   start_phone ;;
        *) err "알 수 없는 컴포넌트: $1"; return 1 ;;
    esac
}

cmd_start() {
    # 여러 이름을 공백/쉼표로 받을 수 있음. 생략 시 all.
    if [[ $# -eq 0 ]]; then _start_one all; return; fi
    local t
    for t in "$@"; do _start_one "$t"; done
}

_stop_one() {
    case "$1" in
        all)
            header "=== 전체 중지 ==="
            for c in "${COMPONENTS[@]}"; do
                if [[ $c == "csc" ]]; then stop_csc; else stop_one "$c"; fi
            done
            ;;
        csc) stop_csc ;;
        *) stop_one "$1" ;;
    esac
}

cmd_stop() {
    if [[ $# -eq 0 ]]; then _stop_one all; return; fi
    local t
    for t in "$@"; do _stop_one "$t"; done
}

cmd_sync() {
    # 소스 트리 → dist 로 Python/스크립트/메타를 복사 (C++ 빌드 없이 빠른 배포).
    # Usage: ./cims.sh sync [csc|agent|scripts|pkg-meta|console|phone|all]
    if [[ -z "$SRC_CONSOLE" ]]; then
        err "sync 명령은 소스 트리에서만 실행 가능 (dist 안에서는 의미 없음)"
        return 1
    fi
    if [[ ! -d $DIST_DIR ]]; then
        err "dist 디렉토리 없음: $DIST_DIR (먼저 ./cims.sh build 한 번 실행)"
        return 1
    fi

    local targets=("$@")
    [[ ${#targets[@]} -eq 0 ]] && targets=(all)

    local did_csc=0 did_agent=0 did_scripts=0 did_pkg=0 did_console=0 did_phone=0
    for t in "${targets[@]}"; do
        case "$t" in
            all) did_csc=1 did_agent=1 did_scripts=1 did_pkg=1 ;;
            csc)       did_csc=1 ;;
            agent)     did_agent=1 ;;
            scripts)   did_scripts=1 ;;
            pkg-meta)  did_pkg=1 ;;
            console)   did_console=1 ;;
            phone)     did_phone=1 ;;
            *) err "알 수 없는 sync 대상: $t"; return 1 ;;
        esac
    done

    local n_changed=0

    # ── CSC Python 소스 ──────────────────────────────────────────
    if [[ $did_csc -eq 1 ]]; then
        mkdir -p "$DIST_DIR/csc/src"
        # rsync 가 있으면 사용, 없으면 cp -r (목적지 깨끗이)
        if command -v rsync >/dev/null 2>&1; then
            rsync -a --delete-excluded \
                --exclude='__pycache__' --exclude='*.pyc' \
                "$SCRIPT_DIR/csc/src/" "$DIST_DIR/csc/src/"
        else
            cp -r "$SCRIPT_DIR/csc/src/." "$DIST_DIR/csc/src/"
        fi
        ok "csc/src ← $SCRIPT_DIR/csc/src"
        n_changed=$((n_changed+1))
    fi

    # ── Agent 바이너리 + install 스크립트 ────────────────────────
    if [[ $did_agent -eq 1 ]]; then
        mkdir -p "$DIST_DIR/agent"
        cp -f "$SCRIPT_DIR/agent/cims_agent.py"     "$DIST_DIR/agent/"
        cp -f "$SCRIPT_DIR/agent/install-agent.sh"  "$DIST_DIR/agent/"
        chmod +x "$DIST_DIR/agent/install-agent.sh"
        [[ -f "$SCRIPT_DIR/agent/pkg.json" ]] && cp -f "$SCRIPT_DIR/agent/pkg.json" "$DIST_DIR/agent/"
        ok "agent ← $SCRIPT_DIR/agent"
        n_changed=$((n_changed+1))
    fi

    # ── 관리 스크립트 (cims.sh, configure.sh) ────────────────────
    if [[ $did_scripts -eq 1 ]]; then
        cp -f "$SCRIPT_DIR/cims.sh"      "$DIST_DIR/cims.sh"      && chmod +x "$DIST_DIR/cims.sh"
        cp -f "$SCRIPT_DIR/configure.sh" "$DIST_DIR/configure.sh" && chmod +x "$DIST_DIR/configure.sh"
        ok "scripts ← cims.sh, configure.sh"
        n_changed=$((n_changed+1))
    fi

    # ── 컴포넌트별 pkg.json (description 소스) ──────────────────
    if [[ $did_pkg -eq 1 ]]; then
        for t in csp cmp csc cwrtc cspsim; do
            [[ -f "$SCRIPT_DIR/$t/pkg.json" ]] && cp -f "$SCRIPT_DIR/$t/pkg.json" "$DIST_DIR/$t/pkg.json" 2>/dev/null || true
        done
        [[ -f "$SCRIPT_DIR/cims-console/pkg.json" ]] && cp -f "$SCRIPT_DIR/cims-console/pkg.json" "$DIST_DIR/console/pkg.json" 2>/dev/null || true
        [[ -f "$SCRIPT_DIR/cims-phone/pkg.json"   ]] && cp -f "$SCRIPT_DIR/cims-phone/pkg.json"   "$DIST_DIR/phone/pkg.json"   2>/dev/null || true
        ok "pkg-meta ← 각 모듈 루트의 pkg.json"
        n_changed=$((n_changed+1))
    fi

    # ── Console 정적 빌드 (Vite) ─────────────────────────────────
    if [[ $did_console -eq 1 ]]; then
        ( cd "$SRC_CONSOLE" && npm run build 2>&1 | tail -3 )
        if [[ -d "$SRC_CONSOLE/dist" ]]; then
            mkdir -p "$DIST_DIR/console"
            rm -rf "$DIST_DIR/console/dist"
            cp -r "$SRC_CONSOLE/dist" "$DIST_DIR/console/dist"
            cp -f "$SRC_CONSOLE/nginx.conf" "$DIST_DIR/console/nginx.conf" 2>/dev/null || true
            ok "console ← cims-console/dist"
        else
            err "cims-console/dist 없음 (빌드 실패?)"
        fi
        n_changed=$((n_changed+1))
    fi

    # ── Phone 정적 빌드 ─────────────────────────────────────────
    if [[ $did_phone -eq 1 ]]; then
        ( cd "$SRC_PHONE" && npm run build 2>&1 | tail -3 )
        if [[ -d "$SRC_PHONE/dist" ]]; then
            mkdir -p "$DIST_DIR/phone"
            rm -rf "$DIST_DIR/phone/dist"
            cp -r "$SRC_PHONE/dist" "$DIST_DIR/phone/dist"
            cp -f "$SRC_PHONE/nginx.conf" "$DIST_DIR/phone/nginx.conf" 2>/dev/null || true
            ok "phone ← cims-phone/dist"
        else
            err "cims-phone/dist 없음 (빌드 실패?)"
        fi
        n_changed=$((n_changed+1))
    fi

    echo ""
    info "sync 완료 ($n_changed 개 대상). 서비스 재기동: ./cims.sh restart <name>"
}

# 버전 유틸리티 — pkg.json 에 저장된 semver 를 읽고/bump/쓰기
_pkg_read_version() {
    local pkg="$1"
    [[ -z $pkg || ! -f $pkg ]] && { echo ""; return; }
    python3 - "$pkg" <<'PY' 2>/dev/null || echo ""
import sys, json
try:
    with open(sys.argv[1], 'r', encoding='utf-8') as f:
        d = json.load(f)
    print(d.get('version', '') if isinstance(d, dict) else '')
except Exception:
    print('')
PY
}

_pkg_bump_patch() {
    local ver="$1"
    [[ -z $ver ]] && ver="0.0.0"
    local major minor patch
    IFS='.' read -r major minor patch <<< "$ver"
    major="${major:-0}"; minor="${minor:-0}"; patch="${patch:-0}"
    # patch 에 숫자 아닌 것이 섞여 있으면 0 으로 리셋 (예: 1.0.0-rc1)
    [[ ! "$patch" =~ ^[0-9]+$ ]] && patch=0
    echo "${major}.${minor}.$((patch+1))"
}

_pkg_write_version() {
    local pkg="$1" new_ver="$2"
    [[ -z $pkg || ! -f $pkg || -z $new_ver ]] && return
    python3 - "$pkg" "$new_ver" <<'PY' 2>/dev/null
import sys, json
p, v = sys.argv[1], sys.argv[2]
try:
    with open(p, 'r', encoding='utf-8') as f:
        d = json.load(f)
except Exception:
    d = {}
if not isinstance(d, dict): d = {}
d['version'] = v
with open(p, 'w', encoding='utf-8') as f:
    json.dump(d, f, indent=2, ensure_ascii=False)
    f.write('\n')
PY
}

# (meta_file, explicit_ver, no_bump) → 실제 적용할 버전
_resolve_version() {
    local meta="$1" explicit="$2" nobump="$3"
    if [[ -n $explicit ]]; then echo "$explicit"; return; fi
    local cur; cur=$(_pkg_read_version "$meta")
    [[ -z $cur ]] && cur="0.0.0"
    if [[ "$nobump" == "1" ]]; then echo "$cur"; return; fi
    _pkg_bump_patch "$cur"
}

cmd_pkg() {
    # 컴포넌트별 배포 tarball 생성 → build/dist/packages/<name>-<ver>.tar.gz
    # 각 tarball 최상위에 meta.json (name, version, description, build/git/changelog) 포함
    #
    # 버전 결정 로직:
    #   1) -v <ver> 지정: 모든 대상 모듈이 그 버전 사용 + pkg.json 업데이트
    #   2) --no-bump:     현재 pkg.json 의 version 그대로 사용 (재패키징)
    #   3) 기본:          pkg.json 의 patch 를 +1 (auto-bump) + pkg.json 업데이트
    local version=""
    local changelog=""
    local no_bump=0
    local targets=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -v|--version)   version="$2"; shift 2 ;;
            -m|--changelog) changelog="$2"; shift 2 ;;
            --no-bump)      no_bump=1; shift ;;
            -*) err "알 수 없는 옵션: $1"; return 1 ;;
            *)  targets+=("$1"); shift ;;
        esac
    done
    [[ ${#targets[@]} -eq 0 ]] && targets=(cmp csp cwrtc csc console phone cspsim agent)

    if [[ ! -d $DIST_DIR ]]; then
        err "dist 디렉토리 없음: $DIST_DIR (먼저 ./cims.sh build)"
        return 1
    fi

    # 컴포넌트별 소스 루트 매핑 — 각 소스 루트의 pkg.json 에서 name/description 를 가져옴
    # (dist/ 밖에서 실행되는 경우만 소스 루트가 있으며, 그 외에는 dist/<comp>/pkg.json 로 fallback)
    _src_root_for() {
        case "$1" in
            csp)     echo "$SCRIPT_DIR/csp" ;;
            cmp)     echo "$SCRIPT_DIR/cmp" ;;
            csc)     echo "$SCRIPT_DIR/csc" ;;
            cwrtc)   echo "$SCRIPT_DIR/cwrtc" ;;
            console) echo "$SCRIPT_DIR/cims-console" ;;
            phone)   echo "$SCRIPT_DIR/cims-phone" ;;
            cspsim)  echo "$SCRIPT_DIR/cspsim" ;;
            agent)   echo "$SCRIPT_DIR/agent" ;;
            *)       echo "" ;;
        esac
    }

    # Git 정보 (가능한 경우)
    local git_sha="" git_branch=""
    if git -C "$SCRIPT_DIR" rev-parse --git-dir >/dev/null 2>&1; then
        git_sha=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo "")
        git_branch=$(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")
    fi
    local packaged_at; packaged_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    local packaged_by="${USER:-unknown}@$(hostname -s 2>/dev/null || echo unknown)"

    local out_dir="$DIST_DIR/packages"
    mkdir -p "$out_dir"

    local t src_sub tar_file build_date
    for t in "${targets[@]}"; do
        case "$t" in
            cmp|csp|cwrtc|csc|console|phone|cspsim|agent) src_sub="$t" ;;
            *) err "알 수 없는 컴포넌트: $t"; continue ;;
        esac
        if [[ ! -d "$DIST_DIR/$src_sub" ]]; then
            warn "skip: $DIST_DIR/$src_sub 없음"; continue
        fi

        # build_date = 컴포넌트 dist 디렉토리 안에서 가장 최근 파일의 mtime
        build_date=$(find "$DIST_DIR/$src_sub" -type f -printf '%T@\n' 2>/dev/null \
                        | sort -nr | head -1 \
                        | xargs -I{} date -u -d @{} +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo "")

        # 소스 루트 pkg.json 에서 description/version 을 읽음 (없으면 dist/<comp>/pkg.json fallback)
        local comp_meta=""
        local src_root; src_root=$(_src_root_for "$t")
        for cand in "$src_root/pkg.json" "$DIST_DIR/$t/pkg.json"; do
            [[ -n $cand && -f $cand ]] && comp_meta="$cand" && break
        done
        [[ -z $comp_meta ]] && warn "$t: pkg.json 없음 — description 공란"

        # 이 모듈의 실제 적용 버전 결정 (explicit > no-bump > auto-bump patch)
        local comp_ver; comp_ver=$(_resolve_version "$comp_meta" "$version" "$no_bump")
        # pkg.json 에 반영 (소스 + dist 둘 다)
        if [[ -n $comp_ver ]]; then
            [[ -n $comp_meta ]] && _pkg_write_version "$comp_meta" "$comp_ver"
            local dist_meta="$DIST_DIR/$t/pkg.json"
            [[ -f $dist_meta && "$dist_meta" != "$comp_meta" ]] && _pkg_write_version "$dist_meta" "$comp_ver"
        fi

        # meta.json 생성 (DIST_DIR 안에 임시로 작성 → tar 루트에 추가 후 삭제)
        local tmp_meta="$DIST_DIR/.pkgmeta.$$.json"
        python3 - "$comp_meta" "$t" "$comp_ver" "$build_date" "$git_sha" "$git_branch" \
                  "$packaged_at" "$packaged_by" "$changelog" <<'PYEOF' > "$tmp_meta"
import sys, json, os
meta_file, name, version, build_date, git_sha, git_branch, packaged_at, packaged_by, changelog = sys.argv[1:]
desc = ""
service = None
# 소스 루트 pkg.json 은 단일 컴포넌트 형식: { "name": "...", "description": "...", "service": {...} }
if meta_file and os.path.isfile(meta_file):
    try:
        with open(meta_file, 'r', encoding='utf-8') as f:
            entry = json.load(f)
        if isinstance(entry, dict):
            # 단일 컴포넌트 스키마
            if "description" in entry:
                desc = entry.get("description", "")
                if isinstance(entry.get("service"), dict):
                    service = entry["service"]
            # 구(舊) 레지스트리 스키마 (후방 호환)
            elif name in entry and isinstance(entry[name], dict):
                desc = entry[name].get("description", "")
                if isinstance(entry[name].get("service"), dict):
                    service = entry[name]["service"]
    except Exception:
        pass
meta = {
    "name": name,
    "version": version,
    "description": desc,
    "build_date": build_date or None,
    "git_sha": git_sha or None,
    "git_branch": git_branch or None,
    "packaged_at": packaged_at,
    "packaged_by": packaged_by,
    "changelog": changelog or "",
}
if service is not None:
    meta["service"] = service
print(json.dumps(meta, indent=2, ensure_ascii=False))
PYEOF

        # config_template.json: 소스 루트에 있으면 tarball 루트에 함께 포함
        local tmp_tmpl="$DIST_DIR/.pkgtmpl.$$.json"
        local tmpl_basename=".pkgtmpl.$$.json"
        local has_template=0
        if [[ -n "$src_root" && -f "$src_root/config_template.json" ]]; then
            cp "$src_root/config_template.json" "$tmp_tmpl"
            has_template=1
        fi

        tar_file="$out_dir/${t}-${comp_ver}.tar.gz"
        info "패키징: $t-$comp_ver  (git=$git_sha/$git_branch)"

        # tar 구성: meta.json(루트) + config_template.json(루트, 있을 때) + <component>/ + cims.sh
        local meta_basename=".pkgmeta.$$.json"
        # 런타임 산출물/상태 디렉토리는 배포에서 제외
        #  log/         : 서비스 로그 (csp/csc 등)
        #  run/         : pid 파일
        #  cache/       : CSC 설정 캐시 (고정값이 아닌 현재 상태)
        #  packages/    : CSC 가 수집한 업로드 tarball (신규 배포에 포함되면 중복 팽창)
        #  dist/        : 번들러 산출물 이 아닌 상위 dist 와 혼동 방지 (cwrtc/dist 등 없음)
        ( cd "$DIST_DIR" && \
            tar czf "$tar_file" \
                --exclude="$src_sub/log" \
                --exclude="$src_sub/run" \
                --exclude="$src_sub/cache" \
                --exclude="$src_sub/packages" \
                --exclude="$src_sub/cdr" \
                --exclude='*.pid' --exclude='*.pyc' \
                --exclude='__pycache__' --exclude='.cache' \
                --transform="s|^$meta_basename\$|meta.json|" \
                --transform="s|^$tmpl_basename\$|config_template.json|" \
                "$meta_basename" \
                $( [[ $has_template -eq 1 ]] && echo "$tmpl_basename" ) \
                "$src_sub" $( [[ -f cims.sh ]] && echo cims.sh ) )
        rm -f "$tmp_meta"
        [[ $has_template -eq 1 ]] && rm -f "$tmp_tmpl"
        local size; size=$(stat -c%s "$tar_file" 2>/dev/null || echo 0)
        ok "$(basename "$tar_file") ($(numfmt --to=iec --suffix=B "$size" 2>/dev/null || echo "${size}B"))"
    done

    header "생성된 패키지 (업로드 대상):"
    ls -lh "$out_dir"/*.tar.gz 2>/dev/null | awk '{printf "  %s  %s\n", $5, $9}'
    echo ""
    info "Console 에서 업로드: 배포 관리 → 패키지 → ＋ 업로드 (파일만 선택하면 meta 자동 인식)"
}

cmd_restart() {
    if [[ $# -eq 0 ]]; then cmd_stop all; sleep 1; cmd_start all; return; fi
    cmd_stop "$@"
    sleep 1
    cmd_start "$@"
}

case "${1:-}" in
    start)     shift; header "=== CIMS 시작 ==="; cmd_start "$@"; echo ""; cmd_status ;;
    stop)      shift; cmd_stop "$@"; echo ""; cmd_status ;;
    restart)   shift; header "=== CIMS 재시작 ==="; cmd_restart "$@"; echo ""; cmd_status ;;
    status)    cmd_status ;;
    build)     shift; cmd_build "$@" ;;
    configure) shift; cmd_configure "$@" ;;
    sim)       shift; cmd_sim "$@" ;;
    clean)     shift; cmd_clean "${1:-all}" ;;
    log)       shift; cmd_log "${1:-csp}" ;;
    pkg)       shift; cmd_pkg "$@" ;;
    sync)      shift; cmd_sync "$@" ;;
    help|--help|-h) usage ;;
    "") usage ;;
    *) err "알 수 없는 명령: $1"; echo ""; usage; exit 1 ;;
esac
