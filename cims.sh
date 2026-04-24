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
    # ControlPort: top-level ServerPort 우선, template 스키마(Setup.Listen.ControlPort) fallback
    local ctrl_port
    ctrl_port=$(python3 -c "import json; d=json.load(open('$DIST_DIR/cmp/config/cmp.json')); print(d.get('ServerPort', d.get('Setup',{}).get('Listen',{}).get('ControlPort', 9000)))" 2>/dev/null || echo 9000)
    kill_stray "cmp/bin/cmp" "$ctrl_port" udp
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
    local ws_port; ws_port=$(python3 -c "import json; d=json.load(open('$DIST_DIR/cwrtc/config/cwrtc.json')); print(d['Setup']['WsPort'])" 2>/dev/null || echo 8443)
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
    if is_running csc; then warn "CSC 이미 실행 중 (pid=$(read_pid csc))"; return 0; fi
    [[ ! -f "$DIST_DIR/csc/src/csc_app.py" ]] && err "CSC 소스 없음 (make dist 실행 필요)" && return 1
    # overlay-aware port: install_path/config.json (deployment overlay) 를 먼저 확인
    local csc_port
    csc_port=$(python3 -c "
import json, os
base='$DIST_DIR/csc/config/csc.json'
ov='$DIST_DIR/config.json'
p=None
if os.path.isfile(ov):
    try:
        f=json.load(open(ov))
        if isinstance(f,dict):
            p=f.get('Server.Port') or (f.get('Server',{}) or {}).get('Port')
    except: pass
if not p:
    try: p=json.load(open(base))['Server']['Port']
    except: p=4420
print(p)" 2>/dev/null || echo 4420)
    # DIST_DIR 포함 절대경로 pattern — Phase 1/2 csc 공존 시 상호 kill 방지
    kill_stray "$DIST_DIR/csc/src/csc_app.py" "$csc_port" tcp
    info "CSC (REST API 서버) 시작... (port=$csc_port)"
    cd "$DIST_DIR/csc/src"
    python3 -u "$DIST_DIR/csc/src/csc_app.py" >> "$LOG_DIR/csc.log" 2>&1 &
    save_pid csc $!
    sleep 1.5
    is_running csc && ok "CSC 시작 완료 (pid=$(read_pid csc), port=$csc_port)" || { err "CSC 시작 실패"; tail -3 "$LOG_DIR/csc.log" | sed 's/^/  /'; }
}

start_console() {
    if is_running console; then warn "console 이미 실행 중 (pid=$(read_pid console))"; return 0; fi
    # Console 3분화 (2026-04-24 plan):
    #   Dev-Console     : 소스 (cims-console/) Vite dev, port 3001, proxy → Test-CSC 4421
    #   Test-Console    : build/dist/console/dist 정적 서빙, port 8080 (HTTPS)
    #   배포본 console  : Phase 2/3 csc-server/console/, port 80 (운영, 별도 흐름)
    # 기동 모드는 SRC_CONSOLE 존재 여부로 결정 (개발 트리면 Dev, 배포 트리면 Test).
    # 8080 단독 사용 가능 (블록 A 에서 cwrtc 8080 → 8443 이전 완료).
    if [[ -n "$SRC_CONSOLE" && -d "$SRC_CONSOLE" ]]; then
        kill_stray "vite.*cims-console" 3001 tcp
        info "Dev-Console (Admin Web UI, 소스 Vite dev) 시작... (port 3001 → Test-CSC 4421 proxy)"
        cd "$SRC_CONSOLE"
        npm run dev -- --port 3001 --host >> "$LOG_DIR/console.log" 2>&1 &
        save_pid console $!
        sleep 2
        is_running console && ok "Dev-Console 시작 완료 (pid=$(read_pid console), port=3001)" \
            || { err "Dev-Console 시작 실패"; tail -3 "$LOG_DIR/console.log" | sed 's/^/  /'; }
    elif [[ -d "$DIST_DIR/console/dist" ]]; then
        kill_stray "serve dist -l 8080" 8080 tcp
        info "Test-Console (Admin Web UI, dist 정적 서빙) 시작... (port 8080, HTTPS)"
        cd "$DIST_DIR/console"
        _SSL_KEY="$DIST_DIR/csc/cert/server.key"
        _SSL_CERT="$DIST_DIR/csc/cert/server.crt"
        if [[ -f "$_SSL_KEY" && -f "$_SSL_CERT" ]]; then
            npx --yes serve dist -l 8080 --ssl-cert "$_SSL_CERT" --ssl-key "$_SSL_KEY" >> "$LOG_DIR/console.log" 2>&1 &
        else
            npx --yes serve dist -l 8080 >> "$LOG_DIR/console.log" 2>&1 &
        fi
        save_pid console $!
        sleep 2
        is_running console && ok "Test-Console 시작 완료 (pid=$(read_pid console), port=8080)" \
            || { err "Test-Console 시작 실패"; tail -3 "$LOG_DIR/console.log" | sed 's/^/  /'; }
    else
        err "console 디렉터리 없음. 'cims.sh build' 실행 필요"; return 1
    fi
}

start_phone() {
    if is_running phone; then warn "phone 이미 실행 중 (pid=$(read_pid phone))"; return 0; fi
    # 포트 3002 점유 프로세스(serve 좀비 포함) 먼저 정리
    kill_stray "serve dist -l 3002" 3002 tcp
    if [[ -n "$SRC_PHONE" && -d "$SRC_PHONE" ]]; then
        # 소스 모드: Vite 개발 서버 (API proxy 포함)
        kill_stray "vite.*cims-phone"
        info "phone (MCPTT UE Web) 개발 서버 시작... (port 3002)"
        cd "$SRC_PHONE"
        npm run dev >> "$LOG_DIR/phone.log" 2>&1 &
        save_pid phone $!
    elif [[ -d "$DIST_DIR/phone/dist" ]]; then
        # dist 전용 모드: 정적 서빙 (proxy 없음 — nginx 필요)
        info "phone (MCPTT UE Web) 정적 서빙 시작... (port 3002, HTTPS)"
        cd "$DIST_DIR/phone"
        _SSL_KEY="$DIST_DIR/csc/cert/server.key"
        _SSL_CERT="$DIST_DIR/csc/cert/server.crt"
        if [[ -f "$_SSL_KEY" && -f "$_SSL_CERT" ]]; then
            npx --yes serve dist -l 3002 --ssl-cert "$_SSL_CERT" --ssl-key "$_SSL_KEY" >> "$LOG_DIR/phone.log" 2>&1 &
        else
            npx --yes serve dist -l 3002 >> "$LOG_DIR/phone.log" 2>&1 &
        fi
        save_pid phone $!
    else
        err "phone 디렉터리 없음. 'cims.sh build' 실행 필요"; return 1
    fi
    sleep 2
    is_running phone && ok "phone 시작 완료 (pid=$(read_pid phone))" || { err "phone 시작 실패"; tail -3 "$LOG_DIR/phone.log" | sed 's/^/  /'; }
}

# ── TB (Test-Bed) 3종: TB-CSC(4419) / TB-Console(3000) / TB-agent(9902) ─
# TB 는 검증 Phase 1~3 진행 중 UI 세션 유지용 임시 기동 모듈.
# 검증 대상(cmp/csp/cwrtc/csc/console/phone) 과 달리 `start`/`stop all` 에 포함되지 않음.
# 명시적으로 `cims.sh start tb-csc` / `start tb` 등으로만 조작.

start_tb_csc() {
    if is_running tb-csc; then warn "TB-CSC 이미 실행 중 (pid=$(read_pid tb-csc))"; return 0; fi
    local tb_cfg="$DIST_DIR/csc/config/csc-tb.json"
    [[ ! -f "$tb_cfg" ]] && err "TB-CSC config 없음: $tb_cfg  (./configure.sh 실행)" && return 1
    [[ ! -f "$DIST_DIR/csc/src/csc_app.py" ]] && err "CSC 소스 없음 (make dist 실행 필요)" && return 1
    kill_stray "CIMS_CSC_CONFIG=.*csc-tb.json" 4419 tcp
    info "TB-CSC (4419) 시작..."
    cd "$DIST_DIR/csc/src"
    CIMS_CSC_CONFIG="$tb_cfg" python3 csc_app.py >> "$LOG_DIR/tb-csc.log" 2>&1 &
    save_pid tb-csc $!
    sleep 1.5
    is_running tb-csc && ok "TB-CSC 시작 완료 (pid=$(read_pid tb-csc), port=4419)" \
        || { err "TB-CSC 시작 실패"; tail -3 "$LOG_DIR/tb-csc.log" | sed 's/^/  /'; }
}

start_tb_console() {
    if is_running tb-console; then warn "TB-Console 이미 실행 중 (pid=$(read_pid tb-console))"; return 0; fi
    # dev 모드 기반 (vite proxy 로 /api → TB-CSC 4419 전달). 소스 트리에서만 동작.
    if [[ -z "$SRC_CONSOLE" || ! -d "$SRC_CONSOLE" ]]; then
        err "TB-Console 은 소스 트리에서만 기동 (vite dev proxy 필요). SRC_CONSOLE=$SRC_CONSOLE"
        return 1
    fi
    [[ ! -f "$SRC_CONSOLE/.env.tb.local" ]] && err ".env.tb.local 없음. ./cims.sh configure 실행" && return 1
    kill_stray "vite.*--mode tb" 3000 tcp
    info "TB-Console (Admin Web UI for TB-CSC) dev 서버 시작... (port 3000, mode=tb, target=:4419)"
    cd "$SRC_CONSOLE"
    npm run dev -- --mode tb --port 3000 --host >> "$LOG_DIR/tb-console.log" 2>&1 &
    save_pid tb-console $!
    sleep 3
    is_running tb-console && ok "TB-Console 시작 완료 (pid=$(read_pid tb-console), port=3000)" \
        || { err "TB-Console 시작 실패"; tail -5 "$LOG_DIR/tb-console.log" | sed 's/^/  /'; }
}

start_tb_agent() {
    if is_running tb-agent; then warn "TB-agent 이미 실행 중 (pid=$(read_pid tb-agent))"; return 0; fi
    local agent_py="$DIST_DIR/agent/cims_agent.py"
    [[ ! -f "$agent_py" ]] && err "agent 소스 없음: $agent_py (make dist 실행 필요)" && return 1
    local state_dir="/tmp/cims-tb-agent/state"
    local install_root="/tmp/cims-tb-agent/modules"
    mkdir -p "$state_dir" "$install_root"
    kill_stray "cims_agent.py --csc-url.*:4419" 9902 tcp

    local csc_url="https://127.0.0.1:4419"
    local enroll_opt=""
    # state(session_token) 가 없으면 enrollment token 이 필요.
    if [[ ! -f "$state_dir/agent.json" ]]; then
        if [[ -n "${CIMS_TB_ENROLLMENT_TOKEN:-}" ]]; then
            enroll_opt="--enrollment-token $CIMS_TB_ENROLLMENT_TOKEN"
        else
            # TB-CSC 가 기동되어 있으면 자동 발급 시도 (cims.sh tb-enroll 헬퍼).
            warn "TB-agent state 없음. CIMS_TB_ENROLLMENT_TOKEN 미설정 → 자동 enrollment 시도"
            local tok
            tok="$(_tb_issue_enrollment_token 2>/dev/null || true)"
            if [[ -n "$tok" ]]; then
                enroll_opt="--enrollment-token $tok"
                ok "enrollment token 자동 발급 완료"
            else
                err "자동 발급 실패. 'CIMS_TB_ENROLLMENT_TOKEN=<TOK> cims.sh start tb-agent' 또는 TB-CSC 먼저 기동"
                return 1
            fi
        fi
    fi
    info "TB-agent 시작... (sync 9902, state=$state_dir)"
    cd "$(dirname "$agent_py")"
    CIMS_AGENT_INSTALL_ROOT="$install_root" \
    CIMS_AGENT_SYNC_PORT=9902 \
    python3 "$agent_py" \
        --csc-url "$csc_url" \
        --name tb-agent-local \
        --state-dir "$state_dir" \
        $enroll_opt \
        >> "$LOG_DIR/tb-agent.log" 2>&1 &
    save_pid tb-agent $!
    sleep 2
    is_running tb-agent && ok "TB-agent 시작 완료 (pid=$(read_pid tb-agent))" \
        || { err "TB-agent 시작 실패"; tail -5 "$LOG_DIR/tb-agent.log" | sed 's/^/  /'; }
}

# TB-CSC 에 admin 로그인 → 새 agent 레코드 생성 → enrollment_token 반환.
# 성공 시 token 한 줄을 stdout 에. 실패 시 비어있는 stdout + 비-0 exit.
#
# 이미 name="tb-agent-local" agent 가 있으면 재생성 불가 (409) → 409 시 기존 레코드 삭제 후 재시도.
_tb_issue_enrollment_token() {
    local base="https://127.0.0.1:4419"
    local admin_id="${CIMS_TB_ADMIN_ID:-admin}"
    local admin_pw="${CIMS_TB_ADMIN_PASSWORD:-1234}"
    local name="tb-agent-local"
    # 살아있는지 간단 확인 (aut/login endpoint POST)
    local login_resp; login_resp=$(curl -sk --max-time 3 -X POST "$base/api/v1/auth/login" \
        -H 'Content-Type: application/json' \
        -d "{\"login_id\":\"$admin_id\",\"password\":\"$admin_pw\"}" 2>/dev/null)
    local token; token=$(echo "$login_resp" | python3 -c \
        'import sys,json
try: d=json.load(sys.stdin); print(d.get("access_token") or d.get("token") or "")
except: pass' 2>/dev/null)
    [[ -z "$token" ]] && { echo "login failed: $login_resp" >&2; return 1; }

    local create_resp; create_resp=$(curl -sk -w '\n__HTTP__%{http_code}' \
        -X POST "$base/api/v1/agents" \
        -H "Authorization: Bearer $token" \
        -H 'Content-Type: application/json' \
        -d "{\"name\":\"$name\"}" 2>/dev/null)
    local http="${create_resp##*__HTTP__}"
    local body="${create_resp%$'\n'__HTTP__*}"

    if [[ "$http" == "409" ]]; then
        # 같은 이름 레코드 존재 → id 조회 후 삭제, 재생성.
        local aid; aid=$(curl -sk "$base/api/v1/agents" \
            -H "Authorization: Bearer $token" 2>/dev/null \
            | python3 -c 'import sys,json
d=json.load(sys.stdin); items=d if isinstance(d,list) else d.get("items") or d.get("agents") or []
for r in items:
    if r.get("name")=="'"$name"'": print(r.get("id")); break' 2>/dev/null)
        if [[ -n "$aid" ]]; then
            curl -sk -X DELETE "$base/api/v1/agents/$aid" \
                -H "Authorization: Bearer $token" >/dev/null 2>&1 || true
            create_resp=$(curl -sk -w '\n__HTTP__%{http_code}' \
                -X POST "$base/api/v1/agents" \
                -H "Authorization: Bearer $token" \
                -H 'Content-Type: application/json' \
                -d "{\"name\":\"$name\"}" 2>/dev/null)
            http="${create_resp##*__HTTP__}"
            body="${create_resp%$'\n'__HTTP__*}"
        fi
    fi

    if [[ "$http" != "201" && "$http" != "200" ]]; then
        echo "agent create failed (http=$http): $body" >&2
        return 1
    fi

    local tok aid
    tok=$(echo "$body" | python3 -c \
        'import sys,json
try: d=json.load(sys.stdin); print(d.get("enrollment_token") or "")
except: pass' 2>/dev/null)
    aid=$(echo "$body" | python3 -c \
        'import sys,json
try: d=json.load(sys.stdin); print(d.get("id") or "")
except: pass' 2>/dev/null)
    [[ -z "$tok" ]] && { echo "no enrollment_token in response: $body" >&2; return 1; }

    # agent pending → approved 전환 (heartbeat 전에 approve 안 되어 있으면 거절될 수 있음)
    if [[ -n "$aid" ]]; then
        curl -sk -X POST "$base/api/v1/agents/$aid/approve" \
            -H "Authorization: Bearer $token" >/dev/null 2>&1 || true
    fi

    echo "$tok"
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
    local csc_port
    csc_port=$(python3 -c "
import json, os
base='$DIST_DIR/csc/config/csc.json'
ov='$DIST_DIR/config.json'
p=None
if os.path.isfile(ov):
    try:
        f=json.load(open(ov))
        if isinstance(f,dict):
            p=f.get('Server.Port') or (f.get('Server',{}) or {}).get('Port')
    except: pass
if not p:
    try: p=json.load(open(base))['Server']['Port']
    except: p=4420
print(p)" 2>/dev/null || echo 4420)
    stop_one csc
    # PID 파일 없이 남아있는 스트레이 프로세스도 정리 (DIST_DIR 범위로 한정)
    kill_stray "$DIST_DIR/csc/src/csc_app.py" "$csc_port" tcp
}

# ── 상태 출력 ──────────────────────────────────────────────────
# 컴포넌트별 리스닝 포트 (외부 기동 감지용)
_svc_port_proto() {
    case "$1" in
        cmp)        echo "9000:udp" ;;
        csp)        echo "5060:udp" ;;
        cwrtc)      echo "8443:tcp" ;;
        csc)        echo "4421:tcp" ;;
        # console 은 모드별 포트 분기 — Dev(소스 트리) 3001 / Test(dist 전용) 8080.
        console)
            if [[ -n "$SRC_CONSOLE" && -d "$SRC_CONSOLE" ]]; then echo "3001:tcp"
            else echo "8080:tcp"; fi ;;
        phone)      echo "3002:tcp" ;;
        tb-csc)     echo "4419:tcp" ;;
        tb-console) echo "3000:tcp" ;;
        tb-agent)   echo "9902:tcp" ;;
        *)          echo "" ;;
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
    echo -e "  ${BOLD}[검증 대상]${NC}"
    status_one cmp
    status_one csp
    status_one cwrtc
    status_one csc
    status_one console
    status_one phone
    echo ""
    echo -e "  ${BOLD}[TB (Test-Bed — Phase 2/3 UI 유지용 상시 기동)]${NC}"
    status_one tb-csc
    status_one tb-console
    status_one tb-agent
    echo ""
}

# ── 빌드 ───────────────────────────────────────────────────────
cmd_build() {
    [[ -z "$SRC_CONSOLE" ]] && err "build 명령은 소스 트리에서만 실행 가능" && exit 1
    header "=== C++ 빌드 ==="
    # 3단계 중 1단계: 실제 빌드 + 배포시 필요한 파일을 build/dist 로 복사.
    # 시험환경 설정은 `cims.sh configure`, 패키지화는 `cims.sh pkg` 로 완전 분리.
    # 인자: -j N / -jN / N (병렬 작업 수)
    local jobs=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -j)         shift; jobs="${1:-}"; shift ;;
            -j*)        jobs="${1#-j}"; shift ;;
            [0-9]*)     jobs="$1"; shift ;;
            *)          err "알 수 없는 옵션: $1 (build 단계는 인자 없이 실행)"; return 1 ;;
        esac
    done
    [[ -z "$jobs" ]] && jobs=$(nproc)
    mkdir -p "$SCRIPT_DIR/build"
    cd "$SCRIPT_DIR/build"
    cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo > "$LOG_DIR/cmake.log" 2>&1
    make -j"$jobs" 2>&1 | tee "$LOG_DIR/make.log" | grep -E "^\[|error:|Error" | tail -20
    ok "C++ 빌드 완료"

    header "=== dist 디렉토리 생성 ==="
    make dist 2>&1 | tee -a "$LOG_DIR/make.log" | tail -5
    ok "dist 생성 완료 → $DIST_DIR"

    header "=== Web UI 빌드 (cims-console) ==="
    cd "$SRC_CONSOLE"
    npm install --silent
    npm run build
    cp -r dist "$DIST_DIR/console/"
    ok "cims-console 빌드 완료"
    # TB-Console 은 dev 모드 기반 (vite proxy 필요) → 별도 dist 빌드 불필요.
    # configure.sh 가 .env.tb.local 에 VITE_ADMIN_TARGET=https://127.0.0.1:4419 를 기록하고,
    # cims.sh start tb-console 이 npm run dev -- --mode tb --port 3000 으로 기동한다.

    header "=== Web UI 빌드 (cims-phone) ==="
    cd "$SRC_PHONE"
    npm install --silent
    npm run build
    cp -r dist "$DIST_DIR/phone/"
    ok "cims-phone 빌드 완료"

    echo ""
    ok "[1/3] build 완료 → $DIST_DIR"
    echo ""
    info "다음 단계:"
    info "  [2/3] ./cims.sh configure --local-ip <서버IP> [--db-password <PW>]   # 시험환경 설정"
    info "        ./cims.sh start                                                # Phase 1 기능 검증"
    info "  [3/3] ./cims.sh pkg [-v X.Y.Z]                                       # 배포 패키지화"
}

# ── configure ──────────────────────────────────────────────────
# 3단계 중 2단계: 배포 전 시험 환경 설정. 로컬 네트워크 IP / DB 접속정보 /
# 도메인 / 로그·녹취 경로 등 환경 의존값을 build/dist 의 설정 파일에 반영한다.
cmd_configure() {
    "$SCRIPT_DIR/configure.sh" "$@"
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

# ── 검증용 초기화 (가입자 보존) ───────────────────────────────
# docs/VERIFICATION_PROCESS.md §0.1 초기화 범위에 따름.
# 보존: users, organizations, volte_subscriptions, ptt_subscriptions,
#       ptt_groups, ptt_group_members, user_rejects
# 초기화: 런타임 설정 / 배포 등록 / 세션·로그성 테이블 + 파일 + 프로세스
cmd_reset() {
    local target="all"
    local extra_paths=()
    local keep_processes=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --files) target="files"; shift ;;
            --db)    target="db";    shift ;;
            --all|all) target="all"; shift ;;
            --path)  extra_paths+=("$2"); shift 2 ;;
            --keep-processes) keep_processes=1; shift ;;
            -*) err "알 수 없는 reset 옵션: $1"; return 1 ;;
            *)  shift ;;
        esac
    done

    header "=== 검증 환경 초기화 (가입자 보존, TB 3종 유지) ==="
    info "TB 3종(TB-CSC 4419 / TB-Console 3000 / TB-agent 9902) 은 건드리지 않음."
    [[ $keep_processes -eq 1 ]] && info "--keep-processes: Test-* 프로세스 유지 (서비스 정지/포트 kill 건너뜀)"
    # cims_agent 는 TRUNCATE 하지 않고 name='tb-agent-local' 레코드만 보존 (I1 fix).
    # → reset 후에도 TB-agent 의 session_token 이 유효해 heartbeat 가 401 없이 동작한다.

    if [[ $keep_processes -eq 0 ]]; then
        # 1) 서비스 정지 — DB 연결 정리 + 파일 잠금 해제
        info "서비스 정지 (검증 대상만)..."
        cmd_stop all >/dev/null 2>&1 || true

        # 1-b) 외부 기동 / PID 파일 없는 프로세스도 포트 기반으로 강제 종료
        #      (console npm serve, 다른 경로의 cmp/csp 바이너리 등)
        info "잔존 프로세스 포트 기반 정리..."
        local _rp _port _proto _pids _round
        for _round in 1 2; do
            local _killed=0
            # reset 은 검증 대상만 정리 — TB 포트(4419/3000/9902) 는 제외해 상시 동작 보장
            # console: Dev(3001) + Test(8080) / cwrtc: 8443 (블록 A 이전, 구 8080)
            for _rp in "5060:udp" "5061:tcp" "9000:udp" "9001:udp" "4420:tcp" "4421:tcp" "3001:tcp" "3002:tcp" "8080:tcp" "8443:tcp"; do
                _port="${_rp%%:*}"; _proto="${_rp##*:}"
                if [[ $_proto == "tcp" ]]; then
                    _pids=$(ss -Htlnp 2>/dev/null | awk -v pt=":$_port" '$4 ~ pt {match($0,/pid=([0-9]+)/,m); if(m[1]) print m[1]}' | sort -u || true)
                else
                    _pids=$(ss -Hulnp 2>/dev/null | awk -v pt=":$_port" '$5 ~ pt {match($0,/pid=([0-9]+)/,m); if(m[1]) print m[1]}' | sort -u || true)
                fi
                if [[ -n $_pids ]]; then
                    warn "port $_port/$_proto 점유자 강제 종료: $_pids"
                    if [[ $_round -eq 1 ]]; then
                        kill $_pids 2>/dev/null || true
                    else
                        kill -9 $_pids 2>/dev/null || true
                    fi
                    _killed=1
                fi
            done
            [[ $_killed -eq 0 ]] && break
            sleep 0.5
        done
    fi  # ── /keep_processes==0 (서비스 정지 + 포트 kill) ──

    # 2) 파일 초기화
    if [[ $target == "all" || $target == "files" ]]; then
        info "로그 정리..."
        rm -f "$LOG_DIR"/*.log "$LOG_DIR"/*_*.log 2>/dev/null || true

        info "서비스이력/메시지 로그 정리..."
        rm -rf "$DIST_DIR/ext_mnt/service_log" "$DIST_DIR/ext_mnt/msg_log" 2>/dev/null || true
        mkdir -p "$DIST_DIR/ext_mnt/service_log" "$DIST_DIR/ext_mnt/msg_log"

        info "Agent 설치 경로 정리 (/tmp/cims-agent-*)..."
        rm -rf /tmp/cims-agent-* 2>/dev/null || true

        info "Phase 2/3 배포 대상 정리 (build/dist/{csc,csp,cmp,sim}-server/, §0.10)..."
        # Test-agent 프로세스부터 종료 (파일 잠금 회피)
        pkill -f "cims_agent.py.*--name csc-server-local" 2>/dev/null || true
        pkill -f "cims_agent.py.*--name csp-server-local" 2>/dev/null || true
        pkill -f "cims_agent.py.*--name cmp-server-local" 2>/dev/null || true
        pkill -f "cims_agent.py.*--name sim-server-local" 2>/dev/null || true
        sleep 1
        local _s
        for _s in csc-server csp-server cmp-server sim-server; do
            [[ -d "$DIST_DIR/$_s" ]] && rm -rf "$DIST_DIR/$_s"
        done

        info "발급 인증서 정리 (cert/agent_mtls/issued)..."
        rm -rf "$DIST_DIR/csc/cert/agent_mtls/issued" 2>/dev/null || true
        [[ -n "$SRC_CONSOLE" ]] && rm -rf "$SCRIPT_DIR/csc/cert/agent_mtls/issued" 2>/dev/null || true

        local p
        for p in "${extra_paths[@]+"${extra_paths[@]}"}"; do
            [[ -e "$p" ]] && { info "추가 경로 정리: $p"; rm -rf "$p"; }
        done
        ok "파일 정리 완료"
    fi

    # 3) DB 초기화 (가입자 테이블 보존, 나머지 TRUNCATE)
    if [[ $target == "all" || $target == "db" ]]; then
        local cfg="$DIST_DIR/csp/config/csp.json"
        [[ ! -f $cfg && -n "$SRC_CONSOLE" ]] && cfg="$SCRIPT_DIR/csp/csp.json"
        if [[ ! -f $cfg ]]; then
            warn "csp.json 없음 — DB 초기화 건너뜀"
        else
            info "DB 초기화 (가입자 테이블 보존)..."
            python3 - "$cfg" <<'PY'
import json, sys
try:
    import pymysql
except ImportError:
    print("[ERROR] pymysql 미설치: pip install pymysql"); sys.exit(1)
with open(sys.argv[1]) as f:
    d = json.load(f)
db = d.get('Setup', {}).get('Database', {})
if not db:
    print("[WARN] csp.json 에 Database 섹션 없음"); sys.exit(0)

PRESERVE = {
    'users', 'organizations',
    'volte_subscriptions', 'ptt_subscriptions',
    'ptt_groups', 'ptt_group_members',
    'user_rejects',
}
TRUNCATE = [
    # 모듈 런타임 설정 (대부분 deprecated — 존재 시 제거)
    'sip_service', 'sip_service_listener',
    'csp_listener', 'sip_trunk',
    'routing_rule', 'routing_rule_match',
    'routing_rule_transform', 'routing_access_list', 'csp_config_audit',
    # 배포 등록 (cims_agent 는 TB 레코드 보존 위해 아래서 별도 DELETE)
    'cims_instance', 'cims_package',
    'agent_deployment', 'agent_job', 'agent_metric',
    # 세션/이력/녹취/통계
    'voip_call_logs', 'ptt_call_logs',
    'voip_call_participants', 'ptt_call_participants',
    'recordings', 'recording_segments',
    'stats_daily', 'stats_monthly', 'stats_yearly',
    # IdMS 토큰
    'auth_codes', 'refresh_tokens',
]
# cims_agent: TB-agent(name='tb-agent-local') 레코드만 보존, 나머지 삭제.
# env CIMS_TB_AGENT_NAME 으로 override 가능.
import os as _os
TB_AGENT_NAME = _os.environ.get('CIMS_TB_AGENT_NAME', 'tb-agent-local')

conn = pymysql.connect(
    host=db['Host'], port=int(db.get('Port', 3306)),
    user=db['User'], password=db['Password'], database=db['DbName'],
)
cur = conn.cursor()
cur.execute(
    "SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA=%s",
    (db['DbName'],),
)
existing = {r[0] for r in cur.fetchall()}

cur.execute("SET FOREIGN_KEY_CHECKS=0")
done, skip, blocked = [], [], []
for t in TRUNCATE:
    if t in PRESERVE:
        blocked.append(t); continue
    if t in existing:
        cur.execute(f"TRUNCATE TABLE `{t}`"); done.append(t)
    else:
        skip.append(t)
# cims_agent: TB-agent 레코드는 보존, 나머지만 삭제 (I1)
tb_preserved = 0
if 'cims_agent' in existing:
    cur.execute(
        "DELETE FROM cims_agent WHERE name <> %s",
        (TB_AGENT_NAME,),
    )
    deleted = cur.rowcount
    cur.execute(
        "SELECT COUNT(*) FROM cims_agent WHERE name = %s",
        (TB_AGENT_NAME,),
    )
    tb_preserved = cur.fetchone()[0]
    done.append(f"cims_agent (DELETE {deleted}건, TB 보존 {tb_preserved}건)")
# _deprecated 접미사 테이블도 함께 비움 (migrate_deprecate_* 로 rename 된 것)
for t in sorted(existing):
    if t.endswith('_deprecated'):
        cur.execute(f"TRUNCATE TABLE `{t}`"); done.append(t)
cur.execute("SET FOREIGN_KEY_CHECKS=1")
conn.commit()
conn.close()

print(f"  TRUNCATE/DELETE 완료: {len(done)}건")
for t in done:
    print(f"    - {t}")
if skip:
    print(f"  SKIP (테이블 미존재): {len(skip)}건")
print(f"  보존(가입자): {', '.join(sorted(PRESERVE))}")
if tb_preserved == 0:
    print(f"  [WARN] cims_agent 에 name='{TB_AGENT_NAME}' 레코드 없음 — TB-agent 재-enroll 필요")
PY
            [[ $? -eq 0 ]] && ok "DB 초기화 완료" || err "DB 초기화 실패"
        fi
    fi

    echo ""
}

# ── 사전조건 확인 ─────────────────────────────────────────────
cmd_preflight() {
    header "=== Preflight 체크 ==="

    # 1) ens160 IP
    local ip; ip=$(ip -4 -o addr show ens160 2>/dev/null | awk '{print $4}' | cut -d/ -f1 || true)
    if [[ -n $ip ]]; then
        ok "ens160 IP: $ip"
    else
        err "ens160 인터페이스 없음 — 외부 연동 IP 확인 필요"
    fi

    # 2) Git 상태
    if git -C "$SCRIPT_DIR" rev-parse --git-dir >/dev/null 2>&1; then
        local br sha
        br=$(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")
        sha=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo "?")
        if [[ -z "$(git -C "$SCRIPT_DIR" status --porcelain 2>/dev/null)" ]]; then
            ok "git: $br @ $sha (clean)"
        else
            warn "git: $br @ $sha (uncommitted changes)"
        fi
    fi

    # 3) 포트 점유 확인
    # 검증 대상 포트 — 기동 전엔 "가용" 이어야 정상.
    # TB 포트(4419/3000/9902) 는 반대로 "점유" 되어 있어야 정상 (TB 3종 상시 동작 전제).
    local target_ports=("5060:udp" "5061:tcp" "9000:udp" "9001:udp" "4420:tcp" "4421:tcp" "3001:tcp" "3002:tcp" "8080:tcp" "8443:tcp")
    local tb_ports=("4419:tcp:TB-CSC" "3000:tcp:TB-Console" "9902:tcp:TB-agent")
    local pp port proto line pid label
    info "[검증 대상] 기동 전엔 가용해야 함"
    for pp in "${target_ports[@]}"; do
        port="${pp%%:*}"; proto="${pp##*:}"
        if [[ $proto == "tcp" ]]; then
            line=$(ss -Htlnp 2>/dev/null | awk -v p=":$port" '$4 ~ p {print; exit}' || true)
        else
            line=$(ss -Hulnp 2>/dev/null | awk -v p=":$port" '$5 ~ p {print; exit}' || true)
        fi
        if [[ -n $line ]]; then
            pid=$(echo "$line" | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2 || true)
            warn "port $port/$proto 점유 중 (pid=${pid:-?})"
        else
            ok "port $port/$proto 가용"
        fi
    done
    info "[TB 3종] 상시 기동 중이어야 정상"
    for pp in "${tb_ports[@]}"; do
        port="${pp%%:*}"; label="${pp##*:}"; proto="$(echo "$pp" | cut -d: -f2)"
        line=$(ss -Htlnp 2>/dev/null | awk -v p=":$port" '$4 ~ p {print; exit}' || true)
        if [[ -n $line ]]; then
            pid=$(echo "$line" | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2 || true)
            ok "$label (port $port/tcp) 동작 중 (pid=${pid:-?})"
        else
            warn "$label (port $port/tcp) 미동작 — 'cims.sh start tb' 필요"
        fi
    done

    # 4) DB 연결 확인
    local cfg="$DIST_DIR/csp/config/csp.json"
    [[ ! -f $cfg && -n "$SRC_CONSOLE" ]] && cfg="$SCRIPT_DIR/csp/csp.json"
    if [[ -f $cfg ]]; then
        if python3 - "$cfg" 2>/dev/null <<'PY'
import json, sys
try: import pymysql
except ImportError: sys.exit(2)
with open(sys.argv[1]) as f: d=json.load(f)
db=d['Setup']['Database']
pymysql.connect(host=db['Host'], port=int(db.get('Port',3306)),
                user=db['User'], password=db['Password'],
                database=db['DbName']).close()
PY
        then
            ok "DB 연결 OK"
        else
            warn "DB 연결 실패 또는 pymysql 미설치"
        fi
    else
        warn "csp.json 없음 — DB 점검 건너뜀"
    fi

    echo ""
}

# ── Phase 1 검증 자동화 ──────────────────────────────────────
# docs/VERIFICATION_PROCESS.md Phase 1 (개발 단계 검증) 자동 실행
# 흐름: preflight → reset → build → configure → start → health → 회귀 시나리오 → 리포트
cmd_verify() {
    local phase="${1:-phase1}"
    shift || true
    case "$phase" in
        phase1|1) _verify_phase1 "$@" ;;
        phase2|2) _verify_phase2 "$@" ;;
        phase3|3) _verify_phase3 "$@" ;;
        *) err "지원하지 않는 phase: $phase (phase1|phase2|phase3 지원)"; return 1 ;;
    esac
}

_verify_phase1() {
    [[ -z "$SRC_CONSOLE" ]] && { err "verify phase1 은 소스 트리에서만 실행 가능"; return 1; }

    local skip_build=0 skip_reset=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --skip-build) skip_build=1; shift ;;
            --skip-reset) skip_reset=1; shift ;;
            *) err "알 수 없는 옵션: $1"; return 1 ;;
        esac
    done

    local ts; ts=$(date +%Y%m%d_%H%M%S)
    local report_dir="$SCRIPT_DIR/verify_reports"
    mkdir -p "$report_dir"
    local report="$report_dir/${ts}_phase1.md"
    local ens_ip; ens_ip=$(ip -4 -o addr show ens160 2>/dev/null | awk '{print $4}' | cut -d/ -f1 || true)
    local git_sha; git_sha=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo "?")
    local git_branch; git_branch=$(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")

    header "=== Phase 1 검증 시작 ==="
    info "리포트: $report"
    echo ""

    {
        echo "# Phase 1 Verification Report"
        echo ""
        echo "- Timestamp: $ts"
        echo "- Host: $(hostname 2>/dev/null || echo ?)"
        echo "- ens160 IP: ${ens_ip:-N/A}"
        echo "- Git: $git_branch @ $git_sha"
        [[ $skip_build -eq 1 ]] && echo "- skip-build: yes"
        [[ $skip_reset -eq 1 ]] && echo "- skip-reset: yes"
        echo ""
    } > "$report"

    # 1) Preflight
    echo "## 1. Preflight" >> "$report"
    echo '```' >> "$report"
    cmd_preflight 2>&1 | tee -a "$report" || true
    echo '```' >> "$report"
    echo "" >> "$report"

    # 2) Reset (가입자 보존)
    if [[ $skip_reset -eq 0 ]]; then
        echo "## 2. Reset (가입자 보존)" >> "$report"
        echo '```' >> "$report"
        cmd_reset --all 2>&1 | tee -a "$report" || true
        echo '```' >> "$report"
    else
        echo "## 2. Reset — SKIPPED" >> "$report"
    fi
    echo "" >> "$report"

    # 3) Build (dist 만 — tarball 은 Phase 2 에서 별도 pkg)
    if [[ $skip_build -eq 0 ]]; then
        echo "## 3. Build (dist only)" >> "$report"
        echo '```' >> "$report"
        if ! cmd_build 2>&1 | tail -40 | tee -a "$report"; then
            echo '```' >> "$report"
            echo "**빌드 실패 — 검증 중단**" >> "$report"
            err "빌드 실패. 리포트: $report"
            return 1
        fi
        echo '```' >> "$report"
    else
        echo "## 3. Build — SKIPPED" >> "$report"
    fi
    echo "" >> "$report"

    # 4) Configure (ens160 IP 반영)
    echo "## 4. Configure" >> "$report"
    echo '```' >> "$report"
    if [[ -n "$ens_ip" ]]; then
        cmd_configure --local-ip "$ens_ip" 2>&1 | tail -15 | tee -a "$report" || true
    else
        warn "ens160 IP 없음 — configure 건너뜀"
        echo "(ens160 없음 — 스킵)" >> "$report"
    fi
    echo '```' >> "$report"
    echo "" >> "$report"

    # 5) Start — 전체 모듈 기동 (cmp → csp → cwrtc → csc → console → phone)
    echo "## 5. Start (all)" >> "$report"
    echo '```' >> "$report"
    cmd_start 2>&1 | tee -a "$report" || true
    sleep 2
    cmd_status | tee -a "$report" || true
    echo '```' >> "$report"
    echo "" >> "$report"

    # 5.5) Package auto-upload — reset 로 cims_package 비워진 상태를 tarball 로 채움.
    #      Console 테스트베드 > 모듈관리 에서 버전/템플릿/설정 데이터가 정상 표시되도록.
    echo "## 5.5 Package auto-upload" >> "$report"
    {
        local pkg_dir="$DIST_DIR/packages"
        if [[ ! -d "$pkg_dir" ]]; then
            warn "packages 디렉토리 없음 — pkg upload 스킵"
            echo "(packages 디렉토리 없음)" >> "$report"
        else
            local login_host="${ens_ip:-127.0.0.1}"
            # CSC 로그인 → JWT 획득
            local token
            token=$(curl -sk -X POST "https://${login_host}:4420/api/v1/auth/login" \
                         -H 'Content-Type: application/json' \
                         -d '{"login_id":"admin","password":"1234"}' 2>/dev/null \
                    | python3 -c 'import json,sys
try:
    d=json.load(sys.stdin); print(d.get("token",""))
except Exception: print("")' 2>/dev/null)
            if [[ -z "$token" ]]; then
                warn "CSC admin 로그인 실패 — pkg upload 스킵"
                echo "- admin 로그인 실패" >> "$report"
            else
                echo '```' >> "$report"
                local uploaded=0
                for t in "$pkg_dir"/*.tar.gz; do
                    [[ -f "$t" ]] || continue
                    local fname; fname=$(basename "$t")
                    local code; code=$(curl -sk -o /dev/null -w "%{http_code}" \
                        -X POST "https://${login_host}:4420/api/v1/packages" \
                        -H "Authorization: Bearer $token" \
                        -F "file=@$t;filename=$fname" -F "force=true" 2>/dev/null)
                    if [[ "$code" == "200" || "$code" == "201" ]]; then
                        echo "  [OK]  $fname"; uploaded=$((uploaded+1))
                    else
                        echo "  [FAIL $code] $fname"
                    fi
                done | tee -a "$report"
                echo '```' >> "$report"
                ok "packages 업로드: ${uploaded}건"
            fi
        fi
    }
    echo "" >> "$report"

    # 6) Health check
    echo "## 6. Health check" >> "$report"
    local err_cnt=0 n lf
    for lf in "$LOG_DIR/csp.log" "$LOG_DIR/cmp.log" "$LOG_DIR/csc.log"; do
        [[ -f $lf ]] || continue
        n=$(grep -cE "ERROR|FATAL" "$lf" 2>/dev/null || true); n=${n:-0}
        err_cnt=$(( err_cnt + n ))
    done
    if [[ $err_cnt -eq 0 ]]; then
        ok "ERROR/FATAL: 0"
        echo "- ERROR/FATAL: 0" >> "$report"
    else
        warn "ERROR/FATAL: ${err_cnt}건"
        echo "- ERROR/FATAL: ${err_cnt}건" >> "$report"
        echo '```' >> "$report"
        grep -E "ERROR|FATAL" "$LOG_DIR"/csp.log "$LOG_DIR"/cmp.log "$LOG_DIR"/csc.log 2>/dev/null | head -30 >> "$report" || true
        echo '```' >> "$report"
    fi
    echo "" >> "$report"

    # 7) 회귀 시나리오 (§0.5)
    echo "## 7. Regression Scenarios" >> "$report"
    local sim_ip="${ens_ip:-127.0.0.1}"

    # 7.0 가입자 정보 수집 + v3 access_services.jsonl 시드
    #   v3 (2026-04-22): csp.json 의 Setup.Realm 제거 → access_services.jsonl 이 domain SOT.
    #   verify 시점에 DB subscription.service_ref 를 읽고, 해당 name 의 access_service 가 없으면
    #   자동 시드 (voip-default / ptt-default, domain='csp'). SIGUSR1 로 CSP 재로드.
    local VOIP_USER="" VOIP_PWD="" VOIP_AUTH="" VOIP_DOM=""
    local PTT_USER=""  PTT_PWD=""  PTT_DOM=""   PTT_GROUP=""
    eval "$(python3 - "$DIST_DIR/csp/config/csp.json" "$DIST_DIR/config" 2>/dev/null <<'PY' || true
import json, sys, os, pymysql
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
s = d.get('Setup', {})
cfg_dir = sys.argv[2]  # install_path/config (v3 jsonl 경로)

db = s.get('Database', {})
voip_user = voip_pwd = voip_imsi = voip_ref = ''
ptt_user  = ptt_pwd  = ptt_imsi  = ptt_ref  = ''
ptt_group = ''
try:
    conn = pymysql.connect(host=db['Host'], port=int(db.get('Port',3306)),
                           user=db['User'], password=db['Password'], database=db['DbName'])
    cur = conn.cursor()
    # v3: service_ref + imsi 로 가입자 선택 (service_ref 있는 것 우선)
    cur.execute("SELECT id,passwd,imsi,service_ref FROM volte_subscriptions "
                "WHERE id LIKE '+%' AND passwd<>'' AND service_ref<>'' AND imsi<>'' "
                "ORDER BY id LIMIT 1")
    r = cur.fetchone()
    if r: voip_user, voip_pwd, voip_imsi, voip_ref = r[0], r[1] or '', r[2] or '', r[3] or ''
    cur.execute("SELECT id,passwd,imsi,service_ref FROM ptt_subscriptions "
                "WHERE id LIKE '+%' AND passwd<>'' AND service_ref<>'' AND imsi<>'' "
                "ORDER BY id LIMIT 1")
    r = cur.fetchone()
    if r: ptt_user, ptt_pwd, ptt_imsi, ptt_ref = r[0], r[1] or '', r[2] or '', r[3] or ''
    cur.execute("SELECT id FROM ptt_groups ORDER BY id LIMIT 1")
    r = cur.fetchone()
    if r: ptt_group = r[0]
    conn.close()
except Exception:
    pass

# access_services.jsonl 자동 시드 — reset 시 파일 재작성 (이전 kind 불일치 레코드 제거).
as_path = os.path.join(cfg_dir, 'access_services.jsonl')
existing_names = set()   # v3: 덮어쓰기 모드라 빈 set

import uuid
# v3: voip/ptt domain 분리 — BuildDomainToKindMap 이 한 domain 에 한 kind 만 매핑하도록.
#     subscription 의 imsi@<domain> 이 CSP 에서 Digest realm 과 일치해야 하므로
#     cspsim 에도 동일한 domain 을 넘겨야 함 (아래 VOIP_DOM/PTT_DOM 에 반영).
seeded = []
def seed(name, kind, domain):
    if name in existing_names or not name: return None
    r = {
        'id': uuid.uuid4().hex,
        'name': name,
        'enabled': True,
        'kind': kind,
        'domain': domain,
        'auth_realm': domain,
        'inbound_policy': 'any',
        'allowed_local_node_refs': [],
        'priority': 100,
        'tags': ['verify-seed'],
        'note': 'auto-seeded by cims.sh verify phase1',
        # G6 (2026-04-23): IMS 규격 대응 — OPTIONS/요청 From URI 의 CSP identity.
        #   비우면 CspAddressing::GetServerIdentityForService 가 domain 기반으로 auto-조립.
        'server_identity_uri': f'sip:cspserver@{domain}',
    }
    seeded.append(r)
    existing_names.add(name)
    return r

seed(voip_ref, 'volte', 'ims.mnc033.mcc450.3gppnetwork.org')
seed(ptt_ref,  'ptt',   'ptt.mnc033.mcc450.3gppnetwork.org')

if seeded:
    os.makedirs(cfg_dir, exist_ok=True)
    with open(as_path, 'w') as f:   # v3: 덮어쓰기
        for r in seeded:
            f.write(json.dumps(r, ensure_ascii=False) + '\n')
    # SIGUSR1 to CSP PID + reload 완료 대기 (race 방지)
    import time as _time
    run_dir = os.path.dirname(os.path.dirname(cfg_dir)) + '/run'
    pid_file = os.path.join(run_dir, 'csp.pid')
    try:
        with open(pid_file) as pf:
            pid = int(pf.read().strip())
        os.kill(pid, 10)  # SIGUSR1
        _time.sleep(2)     # CSP 가 ReloadFromJsonl → ServiceMap.Sync 완료까지 대기
    except Exception:
        pass

volte_dom = 'ims.mnc033.mcc450.3gppnetwork.org'
mcptt_dom = 'ptt.mnc033.mcc450.3gppnetwork.org'
voip_auth = f"{voip_imsi}@{volte_dom}" if voip_imsi else ''
ptt_auth  = f"{ptt_imsi}@{mcptt_dom}"  if ptt_imsi  else ''

def shq(v): return "'" + str(v).replace("'", "'\\''") + "'"
print(f"VOIP_USER={shq(voip_user)}")
print(f"VOIP_PWD={shq(voip_pwd)}")
print(f"VOIP_AUTH={shq(voip_auth)}")
print(f"VOIP_DOM={shq(volte_dom)}")
print(f"PTT_USER={shq(ptt_user)}")
print(f"PTT_PWD={shq(ptt_pwd)}")
print(f"PTT_DOM={shq(mcptt_dom)}")
print(f"PTT_GROUP={shq(ptt_group)}")
print(f"SEEDED={shq(str(len(seeded)))}")
PY
)"

    {
        echo "### 7.0 가입자 정보 (DB 기반, 시나리오 파라미터)"
        echo '```'
        echo "VoIP: user=$VOIP_USER  domain=$VOIP_DOM  auth_id=$VOIP_AUTH"
        echo "PTT : user=$PTT_USER   domain=$PTT_DOM   group=$PTT_GROUP"
        echo '```'
        echo ""
    } >> "$report"

    # 7.1 VoIP 2자 통화 (B2BUA)
    echo "### 7.1 VoIP 2자 통화 (B2BUA)" >> "$report"
    echo '```' >> "$report"
    if [[ -z $VOIP_USER ]]; then
        warn "volte_subscriptions 에 유효 가입자 없음 — VoIP 시나리오 스킵"
        echo "(VoIP 가입자 없음 — 스킵)" >> "$report"
    else
        local voip_args=(-no-db -mode volte -scenario call -count 2 -duration 5 -ip "$sim_ip"
                         -user "$VOIP_USER" -domain "$VOIP_DOM" -password "$VOIP_PWD")
        [[ -n $VOIP_AUTH ]] && voip_args+=(-auth_id "$VOIP_AUTH")
        cmd_sim "${voip_args[@]}" 2>&1 | tail -30 | tee -a "$report" || true
    fi
    echo '```' >> "$report"
    echo "" >> "$report"

    # 7.2 PTT 그룹콜 (5 member) — v3: imsi+domain 명시적 전달 (DerivePttAuthId 의존 제거)
    echo "### 7.2 PTT 그룹콜 (5 member)" >> "$report"
    echo '```' >> "$report"
    if [[ -z $PTT_USER || -z $PTT_GROUP ]]; then
        warn "ptt_subscriptions/ptt_groups 데이터 부족 — PTT 시나리오 스킵"
        echo "(PTT 가입자/그룹 없음 — 스킵)" >> "$report"
    else
        # DB 모드로 전환하여 cspsim 이 count 맞는 가입자들을 DB 에서 직접 선택하도록 함.
        # -domain 명시 — cspsim 이 DB 가입자 imsi 와 이 도메인을 결합하여 auth_id 조립.
        local ptt_args=(-mode ptt -scenario group_call -count 5 -duration 10 -ip "$sim_ip"
                        -domain "$PTT_DOM" -group "$PTT_GROUP")
        cmd_sim "${ptt_args[@]}" 2>&1 | tail -30 | tee -a "$report" || true
    fi
    echo '```' >> "$report"
    echo "" >> "$report"

    # 8) 결과 요약
    echo "## 8. 결과 요약" >> "$report"
    local rec_ok rec_zero sip_lines
    rec_ok=$(find "$DIST_DIR/ext_mnt/service_log" -name "seg_*.rtp" -size +0 2>/dev/null | wc -l || true)
    rec_zero=$(find "$DIST_DIR/ext_mnt/service_log" -name "seg_*.rtp" -size 0 2>/dev/null | wc -l || true)
    # ServiceLogging 통합 (2026-04-23): msg_log 가 service_log/YYYY/MM/DD/HH/ 로 이동.
    # *_sip.msg.jsonl / *_cmp.msg.jsonl / *_csc.msg.jsonl / *.flow.jsonl 합산.
    local msg_lines flow_lines
    msg_lines=$(find "$DIST_DIR/ext_mnt/service_log" -maxdepth 5 -name "*.msg.jsonl" -exec cat {} + 2>/dev/null | wc -l || true)
    flow_lines=$(find "$DIST_DIR/ext_mnt/service_log" -maxdepth 5 -name "*.flow.jsonl" -exec cat {} + 2>/dev/null | wc -l || true)
    sip_lines=$(( ${msg_lines:-0} + ${flow_lines:-0} ))
    {
        echo "- 녹취 파일(size>0): ${rec_ok:-0}개"
        echo "- 녹취 파일(0바이트): ${rec_zero:-0}개"
        echo "- SIP/msg 로그 라인: ${sip_lines:-0} (msg=${msg_lines:-0}, flow=${flow_lines:-0})"
        echo "- ERROR/FATAL 누적: ${err_cnt}건"
        echo ""
        echo "### 수동 검증 항목 (Console 접속 필요)"
        echo "- [ ] Console Flow 페이지 nodes 순서"
        echo "- [ ] sesid 일관성 (SIP ↔ CMP)"
        echo "- [ ] CSC 가입자/그룹 CRUD → NOTIFY"
        echo "- [ ] (mTLS 모드) cert rotation e2e"
    } >> "$report"

    header "=== Phase 1 검증 종료 ==="
    info "리포트: $report"
    echo ""
}

# ──────────────────────────────────────────────────────────────
# Phase 2 — TB-CSC(4419) → Test-agent → csc-server/ 배포 체인 검증
#   기능 회귀 반복 X. 배포 메커니즘 자체만 확인 (§0.2 / §0.10).
#   대상 디렉토리: build/dist/csc-server/{agent,csc,console}
#   Test-agent: name=csc-server-local, sync 9903 (TB-agent 9902 와 분리)
# ──────────────────────────────────────────────────────────────
_verify_phase2() {
    [[ -z "$SRC_CONSOLE" ]] && { err "verify phase2 는 소스 트리에서만 실행 가능"; return 1; }

    local skip_build=0 skip_pkg=0 keep_agent=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --skip-build) skip_build=1; shift ;;
            --skip-pkg)   skip_pkg=1;   shift ;;
            --keep-agent) keep_agent=1; shift ;;
            *) err "알 수 없는 옵션: $1"; return 1 ;;
        esac
    done

    local ts; ts=$(date +%Y%m%d_%H%M%S)
    local report_dir="$SCRIPT_DIR/verify_reports"; mkdir -p "$report_dir"
    local report="$report_dir/${ts}_phase2.md"
    local ens_ip; ens_ip=$(ip -4 -o addr show ens160 2>/dev/null | awk '{print $4}' | cut -d/ -f1 || true)
    local git_sha; git_sha=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo "?")
    local git_branch; git_branch=$(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")

    header "=== Phase 2 배포 검증 시작 ==="
    info "리포트: $report"
    echo ""

    {
        echo "# Phase 2 Verification Report"
        echo ""
        echo "- Timestamp: $ts"
        echo "- Host: $(hostname)"
        echo "- ens160 IP: ${ens_ip:-N/A}"
        echo "- Git: $git_branch @ $git_sha"
        echo "- Scope: TB-CSC(4419) → Test-agent(csc-server-local, sync 9903) → csc-server/ 배포"
        echo "- 대상 디렉토리: build/dist/csc-server/{agent,csc,console}"
        [[ $skip_build -eq 1 ]] && echo "- skip-build: yes"
        [[ $skip_pkg -eq 1 ]] && echo "- skip-pkg: yes"
        [[ $keep_agent -eq 1 ]] && echo "- keep-agent: yes"
        echo ""
    } > "$report"

    # TB-CSC 생존 확인
    if ! curl -sk --max-time 3 -o /dev/null "https://127.0.0.1:4419/api/v1/packages"; then
        err "TB-CSC(4419) 접근 불가 — 'cims.sh start tb' 실행 필요"
        echo "**FAIL: TB-CSC 접근 불가 — 검증 중단**" >> "$report"
        return 1
    fi

    # 1) Cleanup — Phase 1 Test-* 는 살려두고 로그/DB/csc-server 만 초기화
    #    cmd_reset --keep-processes 가 다음을 수행:
    #      · LOG_DIR/*.log, service_log/, msg_log/ wipe
    #      · /tmp/cims-agent-* + build/dist/{csc,csp,cmp,sim}-server/ rm -rf
    #      · cims_agent (TB 보존 외 DELETE) + agent_deployment/job/metric TRUNCATE
    #      · 발급 cert (cert/agent_mtls/issued) 정리
    #    verify_reports/ 는 절대 건드리지 않음 (이번 verify 의 report 보존)
    echo "## 1. Cleanup" >> "$report"
    cmd_reset --all --keep-processes >/dev/null 2>&1 || true
    mkdir -p "$DIST_DIR/csc-server/agent/state"
    echo "- cmd_reset --keep-processes 실행 (Phase 1 Test-* 유지, 로그/DB/csc-server 초기화)" >> "$report"
    echo "" >> "$report"

    # 2) Build (옵션)
    if [[ $skip_build -eq 0 ]]; then
        echo "## 2. Build" >> "$report"
        echo '```' >> "$report"
        if ! cmd_build 2>&1 | tail -20 >> "$report"; then
            echo '```' >> "$report"; err "빌드 실패"; return 1
        fi
        echo '```' >> "$report"
    else
        echo "## 2. Build — SKIPPED" >> "$report"
    fi
    echo "" >> "$report"

    # 3) Configure
    echo "## 3. Configure" >> "$report"
    echo '```' >> "$report"
    if [[ -n "$ens_ip" ]]; then
        cmd_configure --local-ip "$ens_ip" 2>&1 | tail -10 >> "$report" || true
    fi
    echo '```' >> "$report"
    echo "" >> "$report"

    # 4) Pkg --no-bump (옵션)
    if [[ $skip_pkg -eq 0 ]]; then
        echo "## 4. Pkg (tarball, --no-bump)" >> "$report"
        echo '```' >> "$report"
        cmd_pkg --no-bump 2>&1 | tail -15 >> "$report" || true
        echo '```' >> "$report"
    else
        echo "## 4. Pkg — SKIPPED" >> "$report"
    fi
    echo "" >> "$report"

    # 5) Admin login → JWT
    local base="https://127.0.0.1:4419"
    local admin_id="${CIMS_TB_ADMIN_ID:-admin}"
    local admin_pw="${CIMS_TB_ADMIN_PASSWORD:-1234}"
    local tok
    tok=$(curl -sk -X POST "$base/api/v1/auth/login" \
        -H 'Content-Type: application/json' \
        -d "{\"login_id\":\"$admin_id\",\"password\":\"$admin_pw\"}" 2>/dev/null \
        | python3 -c 'import sys,json; print(json.load(sys.stdin).get("token",""))' 2>/dev/null)
    if [[ -z $tok ]]; then
        err "admin 로그인 실패"
        echo "**FAIL: admin login**" >> "$report"
        return 1
    fi
    echo "## 5. Admin login OK" >> "$report"
    echo "" >> "$report"

    # 6) Agent 등록 (csc-server-local) — 409 → 삭제 후 재생성
    local aname="csc-server-local"
    local create_resp http body
    create_resp=$(curl -sk -w '\n__HTTP__%{http_code}' -X POST "$base/api/v1/agents" \
        -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
        -d "{\"name\":\"$aname\",\"note\":\"Phase 2 Test-agent\"}")
    http="${create_resp##*__HTTP__}"; body="${create_resp%$'\n'__HTTP__*}"
    if [[ "$http" == "409" ]]; then
        local aid_exist
        aid_exist=$(curl -sk -H "Authorization: Bearer $tok" "$base/api/v1/agents" 2>/dev/null \
            | python3 -c "import sys,json
d=json.load(sys.stdin); items=d if isinstance(d,list) else d.get('items') or d.get('agents') or []
for r in items:
    if r.get('name')=='$aname': print(r.get('id')); break" 2>/dev/null)
        [[ -n $aid_exist ]] && curl -sk -X DELETE -H "Authorization: Bearer $tok" "$base/api/v1/agents/$aid_exist" >/dev/null 2>&1
        create_resp=$(curl -sk -w '\n__HTTP__%{http_code}' -X POST "$base/api/v1/agents" \
            -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
            -d "{\"name\":\"$aname\",\"note\":\"Phase 2 Test-agent\"}")
        http="${create_resp##*__HTTP__}"; body="${create_resp%$'\n'__HTTP__*}"
    fi
    if [[ "$http" != "201" && "$http" != "200" ]]; then
        err "agent 생성 실패 http=$http"
        echo "**FAIL: agent create (http=$http)**" >> "$report"
        return 1
    fi
    local aid enroll_tok
    aid=$(echo "$body" | python3 -c 'import sys,json; print(json.load(sys.stdin).get("id",""))')
    enroll_tok=$(echo "$body" | python3 -c 'import sys,json; print(json.load(sys.stdin).get("enrollment_token",""))')
    curl -sk -X POST -H "Authorization: Bearer $tok" "$base/api/v1/agents/$aid/approve" >/dev/null 2>&1
    echo "## 6. Agent 등록" >> "$report"
    echo "- agent_id: $aid (name=$aname, approved)" >> "$report"
    echo "" >> "$report"

    # 7) Test-agent 기동 + enroll 대기
    local ta_dir="$DIST_DIR/csc-server/agent"
    local ta_log="$LOG_DIR/test-agent-csc-server.log"
    : > "$ta_log"
    CIMS_AGENT_INSTALL_ROOT="$DIST_DIR/csc-server" \
    CIMS_AGENT_SYNC_PORT=9903 \
    nohup python3 "$DIST_DIR/agent/cims_agent.py" \
        --csc-url "$base" \
        --name "$aname" \
        --state-dir "$ta_dir/state" \
        --enrollment-token "$enroll_tok" \
        --heartbeat-sec 3 \
        > "$ta_log" 2>&1 &
    local ta_pid=$!
    echo "## 7. Test-agent 기동" >> "$report"
    echo "- pid=$ta_pid, sync=9903, state-dir=csc-server/agent/state" >> "$report"
    local ready=0 i
    for i in $(seq 1 15); do
        sleep 1
        if mysql -u cims -pcims1234 -Nse \
            "SELECT 1 FROM cims_agent WHERE name='$aname' AND status='online'" cims 2>/dev/null | grep -q 1; then
            ready=1; break
        fi
    done
    if [[ $ready -eq 0 ]]; then
        err "Test-agent enroll 실패 (15s timeout)"
        echo "- FAIL: enroll timeout" >> "$report"
        tail -10 "$ta_log" >> "$report"
        kill $ta_pid 2>/dev/null || true
        return 1
    fi
    echo "- enroll OK (online, heartbeat)" >> "$report"
    echo "" >> "$report"

    # 8) Package upload (csc, console)
    echo "## 8. Package upload" >> "$report"
    local pkg_dir="$DIST_DIR/packages"
    declare -A pkg_id_map
    local name tar resp pid
    for name in csc console; do
        tar=$(ls "$pkg_dir"/${name}-*.tar.gz 2>/dev/null | sort -V | tail -1)
        if [[ -z $tar ]]; then
            echo "- $name: tarball 없음 — SKIP" >> "$report"; continue
        fi
        resp=$(curl -sk -X POST "$base/api/v1/packages" \
            -H "Authorization: Bearer $tok" \
            -F "file=@$tar;filename=$(basename "$tar")" -F "force=true")
        pid=$(echo "$resp" | python3 -c 'import sys,json
try: print(json.load(sys.stdin).get("id",""))
except: pass' 2>/dev/null)
        if [[ -z $pid ]]; then
            err "$name 업로드 실패"
            echo "- $name: FAIL" >> "$report"
            kill $ta_pid 2>/dev/null || true
            return 1
        fi
        pkg_id_map[$name]=$pid
        echo "- $name: package_id=$pid ($(basename "$tar"))" >> "$report"
    done
    echo "" >> "$report"

    # 9) Deployment 생성 (+ config overlay — csc 는 Phase 1 과 포트 충돌 회피 위해 4445)
    echo "## 9. Deployment 생성 (config overlay 포함)" >> "$report"
    declare -A dep_id_map
    local did pname install_path cfg_json
    for name in csc console; do
        pid=${pkg_id_map[$name]:-}
        [[ -z $pid ]] && continue
        install_path="$DIST_DIR/csc-server/$name"
        pname="${name^^}"
        if [[ $name == "csc" ]]; then
            cfg_json='{"Server.Port":4445}'
        else
            cfg_json='{}'
        fi
        resp=$(curl -sk -X POST "$base/api/v1/deployments" \
            -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
            -d "{\"agent_id\":$aid,\"package_id\":$pid,\"install_path\":\"$install_path\",\"process_name\":\"$pname\",\"config\":$cfg_json}")
        did=$(echo "$resp" | python3 -c 'import sys,json
try: print(json.load(sys.stdin).get("id",""))
except: pass' 2>/dev/null)
        if [[ -z $did ]]; then
            err "$name deployment 생성 실패"
            echo "- $name: FAIL" >> "$report"
            kill $ta_pid 2>/dev/null || true
            return 1
        fi
        dep_id_map[$name]=$did
        echo "- $name: deployment_id=$did → $install_path, overlay=$cfg_json" >> "$report"
    done
    echo "" >> "$report"

    # 10) Install jobs queue + poll
    echo "## 10. Install job + 상태 폴링 (최대 60s)" >> "$report"
    for name in csc console; do
        did=${dep_id_map[$name]:-}
        [[ -z $did ]] && continue
        curl -sk -X POST "$base/api/v1/deployments/$did/job" \
            -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
            -d '{"job_type":"install"}' >/dev/null 2>&1
    done
    local all_done=0 elapsed=0
    for i in $(seq 1 30); do
        sleep 2; elapsed=$((elapsed+2))
        local still=0 st
        for name in csc console; do
            did=${dep_id_map[$name]:-}
            [[ -z $did ]] && continue
            st=$(mysql -u cims -pcims1234 -Nse "SELECT status FROM agent_deployment WHERE id=$did" cims 2>/dev/null)
            [[ $st == "pending" || $st == "deploying" ]] && still=1
        done
        if [[ $still -eq 0 ]]; then all_done=1; break; fi
    done
    echo "- 완료 상태: $([[ $all_done -eq 1 ]] && echo "OK (${elapsed}s)" || echo "TIMEOUT (60s)")" >> "$report"
    echo "" >> "$report"

    echo "### 최종 deployment 상태" >> "$report"
    echo '```' >> "$report"
    mysql -u cims -pcims1234 -e \
        "SELECT id, agent_id, package_id, status, install_path, last_job_id FROM agent_deployment" cims >> "$report" 2>&1
    echo '```' >> "$report"
    echo "" >> "$report"

    echo "### job 결과" >> "$report"
    echo '```' >> "$report"
    mysql -u cims -pcims1234 -e \
        "SELECT id, agent_id, job_type, status, result_code FROM agent_job ORDER BY id" cims >> "$report" 2>&1
    echo '```' >> "$report"
    echo "" >> "$report"

    # 11) 설치 파일 검증
    echo "## 11. 설치 파일 검증" >> "$report"
    local verified_ok=1
    for name in csc console; do
        did=${dep_id_map[$name]:-}
        [[ -z $did ]] && continue
        install_path="$DIST_DIR/csc-server/$name"
        if [[ -f "$install_path/meta.json" && -d "$install_path/config" ]]; then
            echo "- [OK] $name: meta.json + config/ 존재 ($install_path)" >> "$report"
        else
            echo "- [FAIL] $name: meta.json 또는 config/ 누락" >> "$report"
            verified_ok=0
        fi
    done
    echo "" >> "$report"

    # 12) config overlay 반영 검증 (install_path/config.json)
    echo "## 12. config overlay 검증 (install_path/config.json)" >> "$report"
    local overlay_ok=1
    local csc_overlay_port
    csc_overlay_port=$(python3 -c "
import json, os
p='$DIST_DIR/csc-server/csc/config.json'
try:
    d=json.load(open(p))
    print(d.get('Server.Port') or (d.get('Server',{}) or {}).get('Port') or '')
except: print('')" 2>/dev/null)
    if [[ "$csc_overlay_port" == "4445" ]]; then
        echo "- [OK] csc/config.json: Server.Port=4445 반영" >> "$report"
    else
        echo "- [FAIL] csc/config.json: overlay 미반영 (실제=$csc_overlay_port)" >> "$report"
        overlay_ok=0
    fi
    echo "" >> "$report"

    # 13) Start job (csc) + 기동 확인
    echo "## 13. Start job (csc) + 포트 4445 LISTEN 대기" >> "$report"
    local csc_did=${dep_id_map[csc]:-}
    local start_ok=0 start_elapsed=0
    if [[ -n $csc_did ]]; then
        curl -sk -X POST "$base/api/v1/deployments/$csc_did/job" \
            -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
            -d '{"job_type":"start"}' >/dev/null 2>&1
        for i in $(seq 1 25); do
            sleep 1; start_elapsed=$((start_elapsed+1))
            if ss -tln 2>/dev/null | awk '{print $4}' | grep -qE '(^|:)4445$'; then
                start_ok=1; break
            fi
        done
    fi
    if [[ $start_ok -eq 1 ]]; then
        echo "- [OK] csc port 4445 LISTEN 확인 (${start_elapsed}s)" >> "$report"
    else
        echo "- [FAIL] csc port 4445 LISTEN 실패 (25s timeout)" >> "$report"
        echo '```' >> "$report"
        mysql -u cims -pcims1234 -e "SELECT id, job_type, status, result_code, SUBSTRING(result_stderr,1,300) AS err FROM agent_job WHERE agent_id=$aid AND job_type='start' ORDER BY id DESC LIMIT 1" cims >> "$report" 2>&1 || true
        echo '```' >> "$report"
    fi
    echo "" >> "$report"

    # 14) Health check job
    echo "## 14. Health check job" >> "$report"
    local health_ok=0 health_result="(not-run)"
    if [[ -n $csc_did && $start_ok -eq 1 ]]; then
        local hresp hjid
        hresp=$(curl -sk -X POST "$base/api/v1/deployments/$csc_did/job" \
            -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
            -d '{"job_type":"health_check"}')
        hjid=$(echo "$hresp" | python3 -c 'import sys,json
try: print(json.load(sys.stdin).get("job_id",""))
except: pass' 2>/dev/null)
        for i in $(seq 1 15); do
            sleep 1
            local row hstatus hrc hout
            row=$(mysql -u cims -pcims1234 -Nse \
                "SELECT status, result_code, COALESCE(result_stdout,'') FROM agent_job WHERE id=$hjid" cims 2>/dev/null)
            hstatus=$(echo "$row" | awk -F'\t' '{print $1}')
            hrc=$(echo "$row" | awk -F'\t' '{print $2}')
            hout=$(echo "$row" | awk -F'\t' '{print $3}')
            if [[ $hstatus == "succeeded" || $hstatus == "failed" ]]; then
                health_result="status=$hstatus rc=$hrc out=$hout"
                if [[ $hstatus == "succeeded" && $hrc == "0" && "$hout" == *"tcp:4445=open"* ]]; then
                    health_ok=1
                fi
                break
            fi
        done
    fi
    echo "- 결과: $health_result" >> "$report"
    echo "- 판정: $([[ $health_ok -eq 1 ]] && echo OK || echo FAIL)" >> "$report"
    echo "" >> "$report"

    # 15) Stop job (cleanup) — Phase 2 csc 프로세스 정리
    echo "## 15. Stop job (cleanup)" >> "$report"
    local stop_ok=0
    if [[ -n $csc_did ]]; then
        curl -sk -X POST "$base/api/v1/deployments/$csc_did/job" \
            -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
            -d '{"job_type":"stop"}' >/dev/null 2>&1
        for i in $(seq 1 10); do
            sleep 1
            if ! ss -tln 2>/dev/null | awk '{print $4}' | grep -qE '(^|:)4445$'; then
                stop_ok=1; break
            fi
        done
        if [[ $stop_ok -eq 1 ]]; then
            echo "- [OK] csc port 4445 해제" >> "$report"
        else
            echo "- [WARN] csc port 4445 여전히 LISTEN (10s timeout)" >> "$report"
        fi
    fi
    echo "" >> "$report"

    # 16) Test-agent 종료 (옵션)
    if [[ $keep_agent -eq 0 ]]; then
        kill $ta_pid 2>/dev/null || true
        sleep 1
        echo "## 16. Test-agent 종료 (pid=$ta_pid)" >> "$report"
    else
        echo "## 16. Test-agent 유지 (pid=$ta_pid, --keep-agent)" >> "$report"
    fi
    echo "" >> "$report"

    # 판정 (v2: install + overlay + start + health 모두 OK 여야 PASS)
    local verdict="PASS"
    [[ $all_done -ne 1 || $verified_ok -ne 1 || $overlay_ok -ne 1 || $start_ok -ne 1 || $health_ok -ne 1 ]] && verdict="FAIL"
    {
        echo "## 판정: $verdict"
        echo ""
        echo "- Agent enroll: OK"
        echo "- Package upload: OK (csc, console)"
        echo "- Install 완료: $([[ $all_done -eq 1 ]] && echo OK || echo TIMEOUT)"
        echo "- 설치 파일 검증: $([[ $verified_ok -eq 1 ]] && echo OK || echo FAIL)"
        echo "- Config overlay: $([[ $overlay_ok -eq 1 ]] && echo OK || echo FAIL)"
        echo "- CSC Start (4445 LISTEN): $([[ $start_ok -eq 1 ]] && echo OK || echo FAIL)"
        echo "- Health check: $([[ $health_ok -eq 1 ]] && echo OK || echo FAIL)"
        echo "- Stop cleanup: $([[ $stop_ok -eq 1 ]] && echo OK || echo WARN)"
        echo ""
        echo "- (console 은 install-only — start/health 는 운영 배포 설계 확정 후 추가)"
    } >> "$report"

    header "=== Phase 2 검증 종료 ==="
    [[ $verdict == "PASS" ]] && ok "Phase 2: PASS" || err "Phase 2: FAIL"
    info "리포트: $report"
    echo ""
}

# ── Phase 3 검증 (v1 install-only) ──────────────────────────
# docs/VERIFICATION_PROCESS.md §3 — 배포 이후 검증
# v1: Phase 1 서버 모듈 중지 → csp/cmp/sim 배포 체인 (agent enroll + install) → 설치 파일 검증
# v2 예정: start/health/stop 자동화
# v3 예정: 4시나리오 자동 실행 (VoLTE/PTT 음성/영상)
_verify_phase3() {
    [[ -z "$SRC_CONSOLE" ]] && { err "verify phase3 은 소스 트리에서만 실행 가능"; return 1; }

    local skip_build=0 skip_pkg=0 keep_agent=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --skip-build) skip_build=1; shift ;;
            --skip-pkg)   skip_pkg=1;   shift ;;
            --keep-agent) keep_agent=1; shift ;;
            *) err "알 수 없는 옵션: $1"; return 1 ;;
        esac
    done

    local ts; ts=$(date +%Y%m%d_%H%M%S)
    local report_dir="$SCRIPT_DIR/verify_reports"; mkdir -p "$report_dir"
    local report="$report_dir/${ts}_phase3.md"
    local ens_ip; ens_ip=$(ip -4 -o addr show ens160 2>/dev/null | awk '{print $4}' | cut -d/ -f1 || true)
    local git_sha; git_sha=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo "?")
    local git_branch; git_branch=$(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")

    header "=== Phase 3 배포 이후 검증 시작 (v1 install-only) ==="
    info "리포트: $report"
    echo ""

    {
        echo "# Phase 3 Verification Report (v1 — install-only)"
        echo ""
        echo "- Timestamp: $ts"
        echo "- Host: $(hostname)"
        echo "- ens160 IP: ${ens_ip:-N/A}"
        echo "- Git: $git_branch @ $git_sha"
        echo "- Scope: Phase 1 서버 모듈 중지 → csp/cmp/sim 배포 체인 (agent enroll + install)"
        echo "- 대상: build/dist/{csp,cmp,sim}-server/{agent,<모듈>}"
        [[ $skip_build -eq 1 ]] && echo "- skip-build: yes"
        [[ $skip_pkg -eq 1 ]] && echo "- skip-pkg: yes"
        [[ $keep_agent -eq 1 ]] && echo "- keep-agent: yes"
        echo ""
    } > "$report"

    # TB-CSC 생존 확인
    if ! curl -sk --max-time 3 -o /dev/null "https://127.0.0.1:4419/api/v1/packages"; then
        err "TB-CSC(4419) 접근 불가 — 'cims.sh start tb' 실행 필요"
        echo "**FAIL: TB-CSC 접근 불가 — 검증 중단**" >> "$report"
        return 1
    fi

    # 1) Phase 1 서버 모듈 중지 + 로그/DB/배포본 wipe (Console/TB 유지)
    echo "## 1. Phase 1 서버 모듈 중지 + Cleanup" >> "$report"
    local _m
    for _m in cmp csp cwrtc phone cspsim; do
        _stop_one "$_m" >/dev/null 2>&1 || true
    done
    # --keep-processes: Console (Dev 3001 or Test 8080) + TB 3종 유지, 로그/DB/배포본 wipe
    cmd_reset --all --keep-processes >/dev/null 2>&1 || true
    local _sdir
    for _sdir in csp-server cmp-server sim-server; do
        mkdir -p "$DIST_DIR/$_sdir/agent/state"
    done
    echo "- Phase 1 cmp/csp/cwrtc/phone/cspsim 중지 완료" >> "$report"
    echo "- cmd_reset --keep-processes 수행 (Console/TB 유지)" >> "$report"
    echo "- 디렉토리 준비: csp-server/ cmp-server/ sim-server/" >> "$report"
    echo "" >> "$report"

    # 2) Build (옵션)
    if [[ $skip_build -eq 0 ]]; then
        echo "## 2. Build" >> "$report"
        echo '```' >> "$report"
        if ! cmd_build 2>&1 | tail -10 >> "$report"; then
            echo '```' >> "$report"; err "빌드 실패"; return 1
        fi
        echo '```' >> "$report"
    else
        echo "## 2. Build — SKIPPED" >> "$report"
    fi
    echo "" >> "$report"

    # 3) Configure
    echo "## 3. Configure" >> "$report"
    echo '```' >> "$report"
    if [[ -n "$ens_ip" ]]; then
        cmd_configure --local-ip "$ens_ip" 2>&1 | tail -6 >> "$report" || true
    fi
    echo '```' >> "$report"
    echo "" >> "$report"

    # 4) Pkg (옵션) — csp/cmp/cspsim tarball 필요
    if [[ $skip_pkg -eq 0 ]]; then
        echo "## 4. Pkg (tarball, --no-bump)" >> "$report"
        echo '```' >> "$report"
        cmd_pkg --no-bump 2>&1 | tail -10 >> "$report" || true
        echo '```' >> "$report"
    else
        echo "## 4. Pkg — SKIPPED" >> "$report"
    fi
    echo "" >> "$report"

    # 5) Admin login
    local base="https://127.0.0.1:4419"
    local admin_id="${CIMS_TB_ADMIN_ID:-admin}"
    local admin_pw="${CIMS_TB_ADMIN_PASSWORD:-1234}"
    local tok
    tok=$(curl -sk -X POST "$base/api/v1/auth/login" \
        -H 'Content-Type: application/json' \
        -d "{\"login_id\":\"$admin_id\",\"password\":\"$admin_pw\"}" 2>/dev/null \
        | python3 -c 'import sys,json; print(json.load(sys.stdin).get("token",""))' 2>/dev/null)
    if [[ -z $tok ]]; then
        err "admin 로그인 실패"
        echo "**FAIL: admin login**" >> "$report"
        return 1
    fi
    echo "## 5. Admin login OK" >> "$report"
    echo "" >> "$report"

    # 6) 3개 Agent 등록 + Test-agent 기동 (csp-server-local/cmp-server-local/sim-server-local)
    echo "## 6. Agent 등록 + Test-agent 기동 (sync 9904/9905/9906)" >> "$report"
    declare -A aid_map enroll_map pid_map sync_port_map
    sync_port_map[csp]=9904
    sync_port_map[cmp]=9905
    sync_port_map[sim]=9906

    local m aname resp_c http body aid enroll_tok ta_dir ta_log aid_exist
    for m in csp cmp sim; do
        aname="${m}-server-local"
        resp_c=$(curl -sk -w '\n__HTTP__%{http_code}' -X POST "$base/api/v1/agents" \
            -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
            -d "{\"name\":\"$aname\",\"note\":\"Phase 3 Test-agent\"}")
        http="${resp_c##*__HTTP__}"; body="${resp_c%$'\n'__HTTP__*}"
        if [[ "$http" == "409" ]]; then
            aid_exist=$(curl -sk -H "Authorization: Bearer $tok" "$base/api/v1/agents" 2>/dev/null \
                | python3 -c "import sys,json
d=json.load(sys.stdin); items=d if isinstance(d,list) else d.get('items') or d.get('agents') or []
for r in items:
    if r.get('name')=='$aname': print(r.get('id')); break" 2>/dev/null)
            [[ -n $aid_exist ]] && curl -sk -X DELETE -H "Authorization: Bearer $tok" "$base/api/v1/agents/$aid_exist" >/dev/null 2>&1
            resp_c=$(curl -sk -w '\n__HTTP__%{http_code}' -X POST "$base/api/v1/agents" \
                -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
                -d "{\"name\":\"$aname\",\"note\":\"Phase 3 Test-agent\"}")
            http="${resp_c##*__HTTP__}"; body="${resp_c%$'\n'__HTTP__*}"
        fi
        if [[ "$http" != "201" && "$http" != "200" ]]; then
            err "$m agent 생성 실패 http=$http"
            echo "- FAIL: $m agent create (http=$http)" >> "$report"
            return 1
        fi
        aid=$(echo "$body" | python3 -c 'import sys,json; print(json.load(sys.stdin).get("id",""))')
        enroll_tok=$(echo "$body" | python3 -c 'import sys,json; print(json.load(sys.stdin).get("enrollment_token",""))')
        curl -sk -X POST -H "Authorization: Bearer $tok" "$base/api/v1/agents/$aid/approve" >/dev/null 2>&1
        aid_map[$m]=$aid
        enroll_map[$m]=$enroll_tok

        ta_dir="$DIST_DIR/${m}-server/agent"
        ta_log="$LOG_DIR/test-agent-${m}-server.log"
        : > "$ta_log"
        CIMS_AGENT_INSTALL_ROOT="$DIST_DIR/${m}-server" \
        CIMS_AGENT_SYNC_PORT=${sync_port_map[$m]} \
        nohup python3 "$DIST_DIR/agent/cims_agent.py" \
            --csc-url "$base" \
            --name "$aname" \
            --state-dir "$ta_dir/state" \
            --enrollment-token "$enroll_tok" \
            --heartbeat-sec 3 \
            > "$ta_log" 2>&1 &
        pid_map[$m]=$!
        echo "- $m: agent_id=$aid, pid=${pid_map[$m]}, sync=${sync_port_map[$m]}" >> "$report"
    done

    # 전 agent enroll 대기
    local all_online=0 i
    for i in $(seq 1 20); do
        sleep 1
        local still=0
        for m in csp cmp sim; do
            aname="${m}-server-local"
            if ! mysql -u cims -pcims1234 -Nse \
                "SELECT 1 FROM cims_agent WHERE name='$aname' AND status='online'" cims 2>/dev/null | grep -q 1; then
                still=1
            fi
        done
        if [[ $still -eq 0 ]]; then all_online=1; break; fi
    done
    if [[ $all_online -eq 0 ]]; then
        err "일부 Test-agent enroll 실패 (20s timeout)"
        echo "- FAIL: enroll timeout" >> "$report"
        for m in csp cmp sim; do
            [[ -n "${pid_map[$m]:-}" ]] && kill ${pid_map[$m]} 2>/dev/null || true
            tail -15 "$LOG_DIR/test-agent-${m}-server.log" >> "$report" 2>/dev/null || true
        done
        return 1
    fi
    echo "- 전 agent enroll OK (online)" >> "$report"
    echo "" >> "$report"

    # 7) Package upload (csp, cmp, cspsim)
    echo "## 7. Package upload (csp / cmp / cspsim)" >> "$report"
    local pkg_dir="$DIST_DIR/packages"
    declare -A pkg_id_map pkg_name_map dir_name_map
    pkg_name_map[csp]=csp
    pkg_name_map[cmp]=cmp
    pkg_name_map[sim]=cspsim
    dir_name_map[csp]=csp
    dir_name_map[cmp]=cmp
    dir_name_map[sim]=sim

    local pkg_fname tar resp pid
    for m in csp cmp sim; do
        pkg_fname=${pkg_name_map[$m]}
        tar=$(ls "$pkg_dir"/${pkg_fname}-*.tar.gz 2>/dev/null | sort -V | tail -1)
        if [[ -z $tar ]]; then
            echo "- $m: tarball 없음 ($pkg_fname-*.tar.gz) — SKIP" >> "$report"; continue
        fi
        resp=$(curl -sk -X POST "$base/api/v1/packages" \
            -H "Authorization: Bearer $tok" \
            -F "file=@$tar;filename=$(basename "$tar")" -F "force=true")
        pid=$(echo "$resp" | python3 -c 'import sys,json
try: print(json.load(sys.stdin).get("id",""))
except: pass' 2>/dev/null)
        if [[ -z $pid ]]; then
            err "$m 업로드 실패"
            echo "- $m: FAIL" >> "$report"
            for mm in csp cmp sim; do [[ -n "${pid_map[$mm]:-}" ]] && kill ${pid_map[$mm]} 2>/dev/null || true; done
            return 1
        fi
        pkg_id_map[$m]=$pid
        echo "- $m: package_id=$pid ($(basename "$tar"))" >> "$report"
    done
    echo "" >> "$report"

    # 8) Deployment 생성
    echo "## 8. Deployment 생성" >> "$report"
    declare -A dep_id_map
    local did pname install_path modname
    for m in csp cmp sim; do
        pid=${pkg_id_map[$m]:-}
        [[ -z $pid ]] && continue
        modname=${dir_name_map[$m]}
        install_path="$DIST_DIR/${m}-server/$modname"
        pname="${pkg_name_map[$m]^^}"
        resp=$(curl -sk -X POST "$base/api/v1/deployments" \
            -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
            -d "{\"agent_id\":${aid_map[$m]},\"package_id\":$pid,\"install_path\":\"$install_path\",\"process_name\":\"$pname\"}")
        did=$(echo "$resp" | python3 -c 'import sys,json
try: print(json.load(sys.stdin).get("id",""))
except: pass' 2>/dev/null)
        if [[ -z $did ]]; then
            err "$m deployment 생성 실패"
            echo "- $m: FAIL" >> "$report"
            for mm in csp cmp sim; do [[ -n "${pid_map[$mm]:-}" ]] && kill ${pid_map[$mm]} 2>/dev/null || true; done
            return 1
        fi
        dep_id_map[$m]=$did
        echo "- $m: deployment_id=$did → $install_path (process=$pname)" >> "$report"
    done
    echo "" >> "$report"

    # 9) Install jobs + poll
    echo "## 9. Install job + 상태 폴링 (최대 60s)" >> "$report"
    for m in csp cmp sim; do
        did=${dep_id_map[$m]:-}
        [[ -z $did ]] && continue
        curl -sk -X POST "$base/api/v1/deployments/$did/job" \
            -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' \
            -d '{"job_type":"install"}' >/dev/null 2>&1
    done
    local all_done=0 elapsed=0
    for i in $(seq 1 30); do
        sleep 2; elapsed=$((elapsed+2))
        local still=0 st
        for m in csp cmp sim; do
            did=${dep_id_map[$m]:-}
            [[ -z $did ]] && continue
            st=$(mysql -u cims -pcims1234 -Nse "SELECT status FROM agent_deployment WHERE id=$did" cims 2>/dev/null)
            [[ $st == "pending" || $st == "deploying" ]] && still=1
        done
        if [[ $still -eq 0 ]]; then all_done=1; break; fi
    done
    echo "- 완료 상태: $([[ $all_done -eq 1 ]] && echo "OK (${elapsed}s)" || echo "TIMEOUT (60s)")" >> "$report"
    echo "" >> "$report"

    echo "### Phase 3 deployment 상태" >> "$report"
    echo '```' >> "$report"
    mysql -u cims -pcims1234 -e \
        "SELECT d.id, a.name AS agent, d.package_id, d.status, d.install_path
         FROM agent_deployment d JOIN cims_agent a ON d.agent_id=a.id
         WHERE a.name IN ('csp-server-local','cmp-server-local','sim-server-local')
         ORDER BY d.id" cims >> "$report" 2>&1
    echo '```' >> "$report"
    echo "" >> "$report"

    # 10) 설치 파일 검증
    echo "## 10. 설치 파일 검증" >> "$report"
    local verified_ok=1
    for m in csp cmp sim; do
        did=${dep_id_map[$m]:-}
        [[ -z $did ]] && continue
        modname=${dir_name_map[$m]}
        install_path="$DIST_DIR/${m}-server/$modname"
        if [[ -f "$install_path/meta.json" && -d "$install_path/config" ]]; then
            echo "- [OK] $m: meta.json + config/ ($install_path)" >> "$report"
        else
            echo "- [FAIL] $m: meta.json 또는 config/ 누락 ($install_path)" >> "$report"
            verified_ok=0
        fi
    done
    echo "" >> "$report"

    # 11) Test-agent 종료 (옵션)
    if [[ $keep_agent -eq 0 ]]; then
        for m in csp cmp sim; do
            [[ -n "${pid_map[$m]:-}" ]] && kill ${pid_map[$m]} 2>/dev/null || true
        done
        sleep 1
        echo "## 11. Test-agent 종료 (pids=${pid_map[csp]:-?},${pid_map[cmp]:-?},${pid_map[sim]:-?})" >> "$report"
    else
        echo "## 11. Test-agent 유지 (--keep-agent)" >> "$report"
        echo "- pids: csp=${pid_map[csp]:-?} cmp=${pid_map[cmp]:-?} sim=${pid_map[sim]:-?}" >> "$report"
    fi
    echo "" >> "$report"

    # 판정 (v1: install + overlay 검증만)
    local verdict="PASS"
    [[ $all_done -ne 1 || $verified_ok -ne 1 ]] && verdict="FAIL"
    {
        echo "## 판정: $verdict (v1 — install-only)"
        echo ""
        echo "- Phase 1 서버 모듈 중지 + reset: OK"
        echo "- Agent enroll (csp/cmp/sim): OK"
        echo "- Package upload: OK (csp / cmp / cspsim)"
        echo "- Install 완료: $([[ $all_done -eq 1 ]] && echo OK || echo TIMEOUT)"
        echo "- 설치 파일 검증: $([[ $verified_ok -eq 1 ]] && echo OK || echo FAIL)"
        echo ""
        echo "- v2 예정: start/health/stop 자동화 (배포본 csp 5060 / cmp 9000 / sim sync 기동)"
        echo "- v3 예정: 4시나리오 자동 실행 (VoLTE 음성/영상, PTT 그룹 음성/영상)"
    } >> "$report"

    header "=== Phase 3 검증 종료 ==="
    [[ $verdict == "PASS" ]] && ok "Phase 3: PASS" || err "Phase 3: FAIL"
    info "리포트: $report"
    echo ""
}

# ── cspsim ─────────────────────────────────────────────────────
cmd_sim() {
    local orig_dir="$PWD"
    local mode="volte" scenario="call" count="" use_db=true
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

        # DB 모드 + domain 미지정: access_services.jsonl 에서 mode(volte|ptt) 매칭 domain 자동 추출.
        # cspsim 이 DB 가입자 imsi + 이 domain 을 결합해 IMPI(auth_id) 조립.
        local as_file="$DIST_DIR/config/access_services.jsonl"
        if [[ -z "$domain" && -f "$as_file" ]]; then
            domain=$(python3 -c "
import json, sys
best = None
for line in open('$as_file'):
    line = line.strip()
    if not line: continue
    try: r = json.loads(line)
    except: continue
    if r.get('kind') != '$mode': continue
    if r.get('enabled') is False: continue
    if best is None or int(r.get('priority', 100)) < int(best.get('priority', 100)):
        best = r
print(best.get('domain','') if best else '')
" 2>/dev/null || true)
            [[ -n "$domain" ]] && info "Domain 자동 감지 (kind=$mode): $domain"
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

${BOLD}3단계 분리: 빌드 → 시험환경 설정 → 패키지화${NC}
  [1/3] build  [-j N]
                       C++ + Web UI 빌드 → build/dist 복사만 수행.
                       환경값 반영 없음 (configure 단계 책임).
  [2/3] configure [options]
                       시험환경 설정. 로컬 네트워크 IP / DB / 도메인 / 로그경로를
                       build/dist 의 설정 파일에 반영 → configure.sh 에 위임.
  [3/3] pkg [-v ...] [--no-bump] [-m ...]
                       배포 tarball 생성 (아래 "배포 패키지" 섹션 참조).

  sync   [targets]     C++ 빌드 없이 Python/스크립트/메타만 dist 로 복사
                       targets: csc | agent | scripts | pkg-meta | console | phone | all
                       (기본: all — C++ 제외. 예: ./cims.sh sync csc && ./cims.sh restart csc)

${BOLD}시뮬레이터:${NC}
  sim [options]
    -mode     volte|ptt
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

${BOLD}검증 절차 (docs/VERIFICATION_PROCESS.md):${NC}
  reset  [--all|--files|--db] [--path <dir>]
                        가입자 테이블 보존 상태로 설정/배포/세션 DB + 파일 + 프로세스 초기화
                        (보존: users, organizations, volte_subscriptions,
                         ptt_subscriptions, ptt_groups, ptt_group_members, user_rejects)
  preflight             사전조건 확인 (ens160 IP, 포트 점유, git 상태, DB 연결)
  verify [phase1] [--skip-build|--skip-reset]
                        Phase 1 전체 자동 실행: preflight → reset → [1/3] build →
                        [2/3] configure(ens160 IP) → start (전체) → health → 회귀 시나리오 → 리포트
                        → verify_reports/<ts>_phase1.md
                        ([3/3] pkg 는 포함 안 함 — Phase 2 에서 별도 실행)
  verify phase2 [--skip-build] [--skip-pkg] [--keep-agent]
                        Phase 2 배포 검증 (v2): csc/console tarball 배포 + start(4445)/health/stop
                        → verify_reports/<ts>_phase2.md (기능 회귀는 반복 X)
  verify phase3 [--skip-build] [--skip-pkg] [--keep-agent]
                        Phase 3 배포 이후 검증 (v1 install-only):
                        Phase 1 서버 모듈 중지 → csp/cmp/sim Test-agent enroll (sync 9904/9905/9906)
                        → csp/cmp/cspsim tarball 업로드 → deployment 생성 →
                        install job 폴링 → 설치 파일 검증 → verify_reports/<ts>_phase3.md
                        (v2 예정: start/health/stop. v3 예정: 4시나리오 자동 실행)

${BOLD}로그:${NC}
  log [cmp|csp|cwrtc|csc|console|phone]

${BOLD}배포 패키지 (Console 업로드용) — [3/3] 단계:${NC}
  pkg [-v X.Y.Z] [--no-bump] [-m <changelog>] [name...]
                                 configure 완료된 build/dist 를 모듈별 tar.gz 로 패키징.
                                 각 tarball 최상위: meta.json (name/version/설명) +
                                 config_template.json (설정 스키마) + <모듈>/ 파일.
                                 기본: auto-bump patch.
                                 -v 지정 시 해당 버전 강제 + pkg.json 반영
                                 --no-bump 면 현재 pkg.json 버전 그대로 (재패키징)
                                 예: ./cims.sh pkg               # 0.0.3 → 0.0.4 자동
                                     ./cims.sh pkg -v 1.0.0 csp  # csp 만 1.0.0 강제

${BOLD}예시:${NC}
  # [1/3] 빌드 → [2/3] 시험환경 설정 → 기동 (소스 트리)
  $(basename "$0") build
  $(basename "$0") configure --local-ip 192.168.1.10 --db-password secret
  $(basename "$0") start

  # [3/3] 배포 패키지 생성 (Phase 1 검증 통과 후)
  $(basename "$0") pkg                            # 모든 모듈 auto-bump
  $(basename "$0") pkg -v 1.2.0 csp               # csp 만 1.2.0 강제

  # 배포 서버에서 (dist/ 내부)
  ./cims.sh configure --local-ip 192.168.1.10
  ./cims.sh start

  # 시뮬레이터 (동시 실행)
  $(basename "$0") clean                                  # 데이터 정리
  $(basename "$0") sim -mode ptt -group +82571910001      # 영상 PTT 포그라운드
  $(basename "$0") sim -mode ptt -group +82571910001 --bg # 영상 PTT 백그라운드
  $(basename "$0") sim -mode volte --bg                   # VoLTE 동시 실행

  $(basename "$0") stop all
  $(basename "$0") status
  $(basename "$0") log csp
EOF
}

# ── 메인 ───────────────────────────────────────────────────────
COMPONENTS=(cmp csp cwrtc csc console phone)

_start_one() {
    case "$1" in
        all)        start_cmp; start_csp; sleep 0.5; start_cwrtc; start_csc; start_console; start_phone ;;
        tb)         start_tb_csc; sleep 0.5; start_tb_console; start_tb_agent ;;
        cmp)        start_cmp ;;
        csp)        start_csp ;;
        cwrtc)      start_cwrtc ;;
        csc)        start_csc ;;
        console)    start_console ;;
        phone)      start_phone ;;
        tb-csc)     start_tb_csc ;;
        tb-console) start_tb_console ;;
        tb-agent)   start_tb_agent ;;
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
            header "=== 전체 중지 (검증 대상만, TB 유지) ==="
            for c in "${COMPONENTS[@]}"; do
                if [[ $c == "csc" ]]; then stop_csc; else stop_one "$c"; fi
            done
            ;;
        tb)
            header "=== TB 3종 중지 ==="
            stop_one tb-agent
            stop_one tb-console
            stop_one tb-csc
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
        # config_template.json 도 동기화 (apply_config_template 가 읽는 파일)
        if [[ -f "$SCRIPT_DIR/csc/config/config_template.json" ]]; then
            mkdir -p "$DIST_DIR/csc/config"
            cp -f "$SCRIPT_DIR/csc/config/config_template.json" \
                  "$DIST_DIR/csc/config/config_template.json"
        fi
        ok "csc/src (+ config_template.json) ← $SCRIPT_DIR/csc"
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
    # 3단계 중 3단계 (패키지화): configure 까지 끝난 build/dist 를 모듈별 tarball 로 묶는다.
    # 출력: build/dist/packages/<name>-<ver>.tar.gz
    # 각 tarball 최상위에 meta.json (name, version, description, build/git/changelog) +
    # config_template.json (설정 스키마) 포함.
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

        # config_template.json: v3 (2026-04-22) 부터 소스의 config/ 아래.
        #   tarball 에는 그대로 최상위(/config_template.json) 로 포함 (agents.py 가 루트에서 파싱).
        local tmp_tmpl="$DIST_DIR/.pkgtmpl.$$.json"
        local tmpl_basename=".pkgtmpl.$$.json"
        local has_template=0
        if [[ -n "$src_root" ]]; then
            local _tmpl_src=""
            if   [[ -f "$src_root/config/config_template.json" ]]; then _tmpl_src="$src_root/config/config_template.json"
            elif [[ -f "$src_root/config_template.json"       ]]; then _tmpl_src="$src_root/config_template.json"   # legacy fallback
            fi
            if [[ -n "$_tmpl_src" ]]; then
                cp "$_tmpl_src" "$tmp_tmpl"
                has_template=1
            fi
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

    header "[3/3] 생성된 패키지 (업로드 대상):"
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
    reset)     shift; cmd_reset "$@" ;;
    preflight) cmd_preflight ;;
    verify)    shift; cmd_verify "$@" ;;
    log)       shift; cmd_log "${1:-csp}" ;;
    pkg)       shift; cmd_pkg "$@" ;;
    sync)      shift; cmd_sync "$@" ;;
    help|--help|-h) usage ;;
    "") usage ;;
    *) err "알 수 없는 명령: $1"; echo ""; usage; exit 1 ;;
esac
