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
    # Console 3분화:
    #   Dev-Console     : 소스 vite dev, 기본 3001
    #   Test-Console    : build/dist/console/dist serve, 기본 8080 (HTTPS)
    #   배포본 console  : csc-server/console/, deployment overlay 의 Port 로 기동 (기본 8081)
    # overlay port: install_path/config.json (deployment POST 의 config 필드가 저장) 우선, 없으면 기본값.
    local port
    port=$(python3 -c "
import json, os
ov='$DIST_DIR/config.json'
p=None
if os.path.isfile(ov):
    try:
        f=json.load(open(ov))
        if isinstance(f,dict):
            p=f.get('Server.Port') or f.get('Port') or (f.get('Server',{}) or {}).get('Port')
    except: pass
print(p if p else '')" 2>/dev/null)

    if [[ -n "$SRC_CONSOLE" && -d "$SRC_CONSOLE" ]]; then
        [[ -z $port ]] && port=3001
        kill_stray "vite.*cims-console" "$port" tcp
        info "Dev-Console (Admin Web UI, 소스 Vite dev) 시작... (port $port → Test-CSC 4421 proxy)"
        cd "$SRC_CONSOLE"
        npm run dev -- --port "$port" --host >> "$LOG_DIR/console.log" 2>&1 &
        save_pid console $!
        sleep 2
        is_running console && ok "Dev-Console 시작 완료 (pid=$(read_pid console), port=$port)" \
            || { err "Dev-Console 시작 실패"; tail -3 "$LOG_DIR/console.log" | sed 's/^/  /'; }
    elif [[ -d "$DIST_DIR/console/dist" ]]; then
        [[ -z $port ]] && port=8080
        kill_stray "serve dist -l $port" "$port" tcp
        info "Test-Console (Admin Web UI, dist 정적 서빙) 시작... (port $port, HTTPS)"
        cd "$DIST_DIR/console"
        _SSL_KEY="$DIST_DIR/csc/cert/server.key"
        _SSL_CERT="$DIST_DIR/csc/cert/server.crt"
        # 배포본 환경 (csc-server/console) 에서는 csc cert 가 다른 경로 — 그 쪽도 탐색
        [[ ! -f "$_SSL_KEY" && -f "$DIST_DIR/../csc/csc/cert/server.key" ]] && _SSL_KEY="$DIST_DIR/../csc/csc/cert/server.key"
        [[ ! -f "$_SSL_CERT" && -f "$DIST_DIR/../csc/csc/cert/server.crt" ]] && _SSL_CERT="$DIST_DIR/../csc/csc/cert/server.crt"
        if [[ -f "$_SSL_KEY" && -f "$_SSL_CERT" ]]; then
            npx --yes serve dist -l "$port" --ssl-cert "$_SSL_CERT" --ssl-key "$_SSL_KEY" >> "$LOG_DIR/console.log" 2>&1 &
        else
            npx --yes serve dist -l "$port" >> "$LOG_DIR/console.log" 2>&1 &
        fi
        save_pid console $!
        sleep 2
        # npx wrapper 가 종료되며 자식 node serve 가 reparent 되는 경우 — 실제 listener PID 로 갱신
        local _real_pid; _real_pid=$(_pid_by_port "$port:tcp")
        [[ -n $_real_pid ]] && save_pid console "$_real_pid"
        is_running console && ok "Test-Console 시작 완료 (pid=$(read_pid console), port=$port)" \
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
    # npx wrapper 가 종료되며 자식 node serve 가 reparent 되는 경우 — 실제 listener PID 로 갱신
    local _real_pid; _real_pid=$(_pid_by_port "3002:tcp")
    [[ -n $_real_pid ]] && save_pid phone "$_real_pid"
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

stop_console() {
    stop_one console
    # PID 파일 없이 남은 stray 정리 (npx wrapper 종료 후 reparent 된 node serve 등)
    # Dev(3001) / Test(8080) 양쪽 모두 점검 — 모드 무관하게 점유 해제
    kill_stray "vite.*cims-console" 3001 tcp
    kill_stray "serve dist -l 8080" 8080 tcp
}

stop_phone() {
    stop_one phone
    kill_stray "vite.*cims-phone" 3002 tcp
    kill_stray "serve dist -l 3002" 3002 tcp
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
    # console 의 경우 모드별 보조 포트도 확인 (Dev 3001 ↔ Test 8080) — orphan stray 검출용
    if [[ "$name" == "console" ]]; then
        local alt_port
        if [[ -n "$SRC_CONSOLE" && -d "$SRC_CONSOLE" ]]; then alt_port=8080; else alt_port=3001; fi
        local ext_pid; ext_pid="$(_pid_by_port "$alt_port:tcp")"
        if [[ -n $ext_pid ]]; then
            echo -e "  ${YELLOW}●${NC} $(printf '%-12s' "$name")  실행 중(stray)  (pid=$ext_pid, port=$alt_port)"
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
    local keep_processes=0 keep_deployed=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --files) target="files"; shift ;;
            --db)    target="db";    shift ;;
            --all|all) target="all"; shift ;;
            --path)  extra_paths+=("$2"); shift 2 ;;
            --keep-processes) keep_processes=1; shift ;;
            --keep-deployed)  keep_deployed=1; shift ;;   # csc-server/ 등 배포본 보존
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

        if [[ $keep_deployed -eq 1 ]]; then
            info "Phase 2/3 배포 대상 정리 — SKIP (--keep-deployed, csc/csp/cmp/sim-server 보존)"
        else
            info "Phase 2/3 배포 대상 정리 (build/dist/{csc,csp,cmp,sim}-server/, §0.10)..."
            # Test-agent 프로세스부터 종료 (파일 잠금 회피)
            pkill -f "cims_agent.py.*--name csc-server-local" 2>/dev/null || true
            pkill -f "cims_agent.py.*--name csp-server-local" 2>/dev/null || true
            pkill -f "cims_agent.py.*--name cmp-server-local" 2>/dev/null || true
            pkill -f "cims_agent.py.*--name sim-server-local" 2>/dev/null || true
            # 배포본 서비스 프로세스 (csc_app.py, console serve, csp/cmp 바이너리) 도 종료
            # — 4445/4430/8081/5060/9000 등 포트 잠금 해제 (Phase 1 검증 시 mcptt 4430 충돌 방지)
            local _s
            for _s in csc-server csp-server cmp-server sim-server; do
                pkill -f "$DIST_DIR/$_s/" 2>/dev/null || true
            done
            sleep 1
            for _s in csc-server csp-server cmp-server sim-server; do
                [[ -d "$DIST_DIR/$_s" ]] && rm -rf "$DIST_DIR/$_s"
            done
        fi

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
            [[ $keep_deployed -eq 1 ]] && export CIMS_RESET_KEEP_DEPLOYED=1 || unset CIMS_RESET_KEEP_DEPLOYED
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
# 가입자 테이블: 등록/로그아웃 잔류 상태 초기화 (TRUNCATE 안 함, 가입자 정보 보존)
for t in ('volte_subscriptions', 'ptt_subscriptions'):
    if t in existing:
        cur.execute(f"UPDATE `{t}` SET register_time=NULL, logout_time=NULL")
        if cur.rowcount > 0:
            done.append(f"{t} (register/logout NULL, {cur.rowcount}건)")
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

    # Phase 2 배포본 잔존 감지 — Phase 1 의 mcptt(4430) 와 충돌 가능
    info "[Phase 2 배포본 잔존] csc 4445 / console 8081 — 살아있으면 Phase 1 검증 시 충돌"
    local p2_residual=0
    for pp in "4445:tcp:배포본 csc" "8081:tcp:배포본 console"; do
        port="${pp%%:*}"; label="${pp##*:}"; proto="$(echo "$pp" | cut -d: -f2)"
        line=$(ss -Htlnp 2>/dev/null | awk -v p=":$port" '$4 ~ p {print; exit}' || true)
        if [[ -n $line ]]; then
            pid=$(echo "$line" | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2 || true)
            warn "$label (port $port/tcp) 잔존 (pid=${pid:-?}) — './cims.sh reset' 또는 'verify phase2 --stop-after' 권장"
            p2_residual=1
        fi
    done
    [[ $p2_residual -eq 0 ]] && ok "Phase 2 배포본 미동작 (clean)"

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

# ── 검증 (verify) — 단계/항목 단위 실행 ─────────────────────────
# 6단계 (S1~S6) 파이프라인. 모든 stage 의 본체는 verify/lib (cims_verify CLI).
#   S1=정적검사, S2=빌드, S3=스모크, S4=패키지화, S5=로컬배포, S6=통합검증
# 메타 명령 (list / describe / list-presets / run) 은 verify_lib 로 위임.
cmd_verify() {
    local stage="${1:-stage1}"
    shift || true

    case "$stage" in
        list|describe|run|list-presets|purge-runs)
            python3 -m tests.cims_verify "$stage" "$@"; return $? ;;
        stage1|1) python3 -m tests.cims_verify run --stage 1 "$@" ;;
        stage2|2) python3 -m tests.cims_verify run --stage 2 "$@" ;;
        stage3|3) python3 -m tests.cims_verify run --stage 3 "$@" ;;
        stage4|4) python3 -m tests.cims_verify run --stage 4 "$@" ;;
        stage5|5) python3 -m tests.cims_verify run --stage 5 "$@" ;;
        stage6|6) python3 -m tests.cims_verify run --stage 6 "$@" ;;
        *) err "지원하지 않는 stage: $stage (stage1~stage6 지원)"; return 1 ;;
    esac
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
                case "$c" in
                    csc)     stop_csc ;;
                    console) stop_console ;;
                    phone)   stop_phone ;;
                    *)       stop_one "$c" ;;
                esac
            done
            ;;
        tb)
            header "=== TB 3종 중지 ==="
            stop_one tb-agent
            stop_one tb-console
            stop_one tb-csc
            ;;
        csc)     stop_csc ;;
        console) stop_console ;;
        phone)   stop_phone ;;
        *)       stop_one "$1" ;;
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
