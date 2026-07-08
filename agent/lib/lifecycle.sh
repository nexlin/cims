#!/bin/bash
# agent/lib/lifecycle.sh — CIMS service lifecycle library
#
# 본 파일은 source 후 함수만 노출하는 library — standalone 실행 금지.
# Caller (agent/bin/cims-svc) 가 아래 환경변수와 helpers 를 미리 정의해야 함:
#   변수:    SCRIPT_DIR, DIST_DIR, PID_DIR, LOG_DIR, SRC_CONSOLE, SRC_PHONE
#   색상:    RED, GREEN, YELLOW, CYAN, BOLD, NC
#   logger:  info(), ok(), warn(), err(), header()
#
# cims.sh 의 운영 영역 (line 40~89, 102~198, 200~410, 419~503, 507~596,
# 1368~1379, 1499~1602, 2088~2093) 에서 1:1 이전. 변경 없이 같은 동작 보장.

# ── python 인터프리터 결정 ───────────────────────────────────
# private/air-gapped 호스트엔 `python3` 가 PATH 에 없을 수 있다 (agent 는 절대경로
# 로 구동되므로 무관하지만 lifecycle 의 bare `python3` 호출이 set -e 하에 127 로
# start 를 abort 시킴 — media01 사례). agent(job_process_control)가 자기
# sys.executable 을 CIMS_PYTHON 으로 넘겨주면 그것을 사용, 없으면 PATH 탐색.
PYBIN="${CIMS_PYTHON:-}"
[[ -z "$PYBIN" ]] && PYBIN="$(command -v python3 2>/dev/null || command -v python 2>/dev/null || echo python3)"

# ── PID 파일 헬퍼 ──────────────────────────────────────────────
pidfile() { echo "$PID_DIR/$1.pid"; }
save_pid() { echo "$2" > "$(pidfile "$1")"; }
read_pid() { local f; f="$(pidfile "$1")"; [[ -f $f ]] && cat "$f" || echo ""; }

# is_running: pid 파일 + 살아있는 process + (있다면) exe 경로가 자기 install
# 의 binary 와 일치할 때만 true. stale pid 파일 (kill -0 성공하지만 다른
# 프로세스를 가리키는 경우) 또는 pid 가 재사용된 경우 거짓 양성 방지.
# 재사용/혼동 시 pid 파일을 stale 로 간주하고 삭제한 후 false 반환 — 후속
# _start_*_variant 가 정상 시작 진행하도록.
is_running() {
    local name="$1"
    local pid; pid="$(read_pid "$name")"
    [[ -z $pid ]] && return 1
    if ! kill -0 "$pid" 2>/dev/null; then
        rm -f "$(pidfile "$name")"
        return 1
    fi
    # exe 검증 (optional). $DIST_DIR/<name>/bin/<name> 가 존재하면 그것과 비교.
    local expected="$DIST_DIR/$name/bin/$name"
    if [[ -x "$expected" ]]; then
        local exe; exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
        local want; want=$(readlink -f "$expected" 2>/dev/null || true)
        if [[ -n "$exe" && -n "$want" && "$exe" != "$want" ]]; then
            # pid 가 다른 binary 를 가리킴 — stale
            rm -f "$(pidfile "$name")"
            return 1
        fi
    fi
    return 0
}

# 삭제된 바이너리(install_path 마이그레이션·agent 업그레이드 잔재)로 실행 중인 동일 모듈 좀비만
# 정리. /proc/<pid>/exe 가 "(deleted)" 인 것만 대상 — 건강한 형제 인스턴스는 건드리지 않음.
kill_deleted_inode_orphans() {
    local name="$1"
    local pid exe
    for pid in $(pgrep -x "$name" 2>/dev/null || true); do
        exe=$(readlink "/proc/$pid/exe" 2>/dev/null || true)
        if [[ "$exe" == *"(deleted)" ]]; then
            warn "삭제된 바이너리로 실행 중인 좀비 $name 정리: pid=$pid ($exe)"
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
}

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
    #    ss(iproute2) 부재 시 선택적 정리이므로 건너뜀 — private/최소 호스트에서
    #    set -e 하에 start 가 hard-fail 하지 않도록 (도구는 패키지/베이스 이미지 책임).
    if [[ -n $port ]] && command -v ss >/dev/null 2>&1; then
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

# ── 자기 install 좀비 정리 ───────────────────────────────────
_kill_own_install_listener() {
    local exe_path="$1" port="$2" proto="${3:-udp}"
    [[ -z "$port" || ! -f "$exe_path" ]] && return 0
    # ss(iproute2) 부재 시 선택적 정리 — 건너뜀 (private/최소 호스트에서 start hard-fail 방지)
    command -v ss >/dev/null 2>&1 || return 0
    local exe_real; exe_real=$(readlink -f "$exe_path" 2>/dev/null)
    [[ -z "$exe_real" ]] && return 0
    local port_pids
    if [[ "$proto" == "tcp" ]]; then
        port_pids=$(ss -tlnp 2>/dev/null | awk -v pt="$port" 'match($4,/:([0-9]+)$/,m) && m[1]==pt {match($0,/pid=([0-9]+)/,p); if(p[1]) print p[1]}')
    else
        port_pids=$(ss -ulnp 2>/dev/null | awk -v pt="$port" 'match($4,/:([0-9]+)$/,m) && m[1]==pt {match($0,/pid=([0-9]+)/,p); if(p[1]) print p[1]}')
    fi
    [[ -z "$port_pids" ]] && return 0
    local pid
    local killed_any=0
    for pid in $port_pids; do
        # readlink -f 가 deleted target 또는 권한 부족 시 non-zero 반환 → set -e
        # 회피 위해 || true. tarball 풀어 inode 교체된 옛 process 의 exe 는
        # '/path/to/bin (deleted)' 형태 → readlink -e/-f 모두 비워서 plain readlink
        # 로 raw target 을 받고 ' (deleted)' suffix 만 trim.
        local pid_exe; pid_exe=$(readlink "/proc/$pid/exe" 2>/dev/null || true)
        local pid_exe_trim="${pid_exe% (deleted)}"
        if [[ -n "$pid_exe_trim" && "$pid_exe_trim" == "$exe_real" ]]; then
            warn "stale own-install listener 정리: pid=$pid (port=$port/$proto exe=$pid_exe)"
            kill "$pid" 2>/dev/null || true
            local i=1
            while kill -0 "$pid" 2>/dev/null && (( i <= 15 )); do sleep 0.2; i=$(( i + 1 )); done
            kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
            killed_any=1
        fi
    done
    # process 죽은 후 socket 이 OS 에서 release 되기까지 짧은 대기 — 새 csp/cmp
    # 가 같은 host:port bind 시도 시 race condition 회피 (SO_REUSEADDR 안 켜진
    # SipServer/CmpServer 의 case).
    if [[ $killed_any -eq 1 ]]; then
        local k=1
        while (( k <= 10 )); do
            local check
            if [[ "$proto" == "tcp" ]]; then
                check=$(ss -tlnp 2>/dev/null | awk -v pt="$port" 'match($4,/:([0-9]+)$/,m) && m[1]==pt {print 1; exit}')
            else
                check=$(ss -ulnp 2>/dev/null | awk -v pt="$port" 'match($4,/:([0-9]+)$/,m) && m[1]==pt {print 1; exit}')
            fi
            [[ -z "$check" ]] && break
            sleep 0.3; k=$(( k + 1 ))
        done
    fi
}

# ── deployment overlay 머지 ──────────────────────────────────
_apply_overlay_to_module_config() {
    local overlay="$1" target="$2"
    [[ ! -f "$overlay" || ! -f "$target" ]] && return 0
    "$PYBIN" - "$overlay" "$target" <<'PY' 2>/dev/null
import json, sys
ov_path, tgt_path = sys.argv[1], sys.argv[2]
try:
    with open(ov_path, encoding="utf-8") as f: ov = json.load(f)
    with open(tgt_path, encoding="utf-8") as f: tgt = json.load(f)
except Exception:
    sys.exit(0)
if not isinstance(ov, dict) or not isinstance(tgt, dict):
    sys.exit(0)

def set_path(root, dotted, value):
    cur = root
    keys = dotted.split(".")
    for k in keys[:-1]:
        nxt = cur.get(k)
        if not isinstance(nxt, dict):
            nxt = {}
            cur[k] = nxt
        cur = nxt
    cur[keys[-1]] = value

changed = False
for k, v in ov.items():
    if "." in k:
        set_path(tgt, k, v); changed = True
    elif isinstance(v, dict):
        cur = tgt.setdefault(k, {})
        if not isinstance(cur, dict):
            tgt[k] = dict(v); changed = True
        else:
            for kk, vv in v.items():
                cur[kk] = vv
            changed = True
    else:
        tgt[k] = v; changed = True

if changed:
    with open(tgt_path, "w", encoding="utf-8") as f:
        json.dump(tgt, f, indent=4, ensure_ascii=False)
PY
}

# ── 시작 함수 (variant + 개별) ───────────────────────────────
_start_cmp_variant() {
    # $1 = pid name (cmp/pmp/imp). 각 변종은 자기 dist/<name>/ 디렉토리에서
    # bin/<name> config/<name>.json 으로 시작. dev 모드는 dist/cmp 만 있고
    # _start_cmp_variant cmp 만 호출. install 후 환경은 cims_agent 가 변종 tarball
    # 풀어 install_path/<name>/ 디렉토리 생성 → cims.sh 가 자기 변종 함수 호출.
    local name="$1"
    local upper; upper=$(echo "$name" | tr '[:lower:]' '[:upper:]')
    if is_running "$name"; then warn "$upper 이미 실행 중 (pid=$(read_pid "$name"))"; return 0; fi
    local bin="$DIST_DIR/$name/bin/$name"
    local cfg="$DIST_DIR/$name/config/$name.json"
    [[ ! -f "$bin" ]] && err "$name 바이너리 없음: $bin (make dist 또는 install 필요)" && return 1
    # deployment overlay 머지 (PMP/IMP 의 RtpIp/CspIp 분기). 변종별 overlay 는
    # install_path/<name>/config.json 에 위치 — 한 install_path 에 형제 변종이 공존
    # 해도 서로 다른 overlay 를 가질 수 있도록 cims_agent.py 가 분리 저장.
    # 이전 위치 (install_path/config.json) 도 fallback 으로 봐서 단일-변종 install
    # (dev 모드 등) 와 후방 호환.
    local _overlay="$DIST_DIR/$name/config.json"
    [[ ! -f "$_overlay" ]] && _overlay="$DIST_DIR/config.json"
    # pre-launch 단계(overlay 머지·좀비 정리)는 best-effort — 어떤 이유로든(도구 부재
    # 등) 실패해도 set -e 하에서 모듈 start 를 abort 시키지 않도록 모두 non-fatal.
    _apply_overlay_to_module_config "$_overlay" "$cfg" || true
    local ctrl_port
    ctrl_port=$("$PYBIN" -c "import json; d=json.load(open('$cfg')); print(d.get('ServerPort', d.get('Setup',{}).get('Listen',{}).get('ControlPort', 9000)))" 2>/dev/null || echo 9000)
    # 삭제된 바이너리로 떠있는 동일 모듈 좀비(경로 마이그레이션 잔재) 먼저 정리
    kill_deleted_inode_orphans "$name" || true
    # 자기 install 의 좀비만 정리 — 다른 인스턴스 영향 차단
    kill_stray "$bin" || true
    _kill_own_install_listener "$bin" "$ctrl_port" udp || true
    info "$upper 시작..."
    cd "$DIST_DIR/$name"
    bin/$name config/$name.json >> "$LOG_DIR/$name.log" 2>&1 &
    save_pid "$name" $!
    sleep 0.8
    is_running "$name" && ok "$upper 시작 완료 (pid=$(read_pid "$name"))" || { err "$upper 시작 실패"; tail -3 "$LOG_DIR/$name.log" | sed 's/^/  /'; return 1; }
}

start_cmp() { _start_cmp_variant cmp; }
start_pmp() { _start_cmp_variant pmp; }
start_imp() { _start_cmp_variant imp; }
start_cmdp() { _start_cmp_variant cmdp; }  # MCData media plane — 계약 동일 (bin/cmdp config/cmdp.json)

_start_csp_variant() {
    # $1 = pid name (csp/psp/isp). 각 변종은 자기 dist/<name>/ 디렉토리에서
    # bin/<name> config/<name>.json 으로 시작. dev 모드는 dist/csp 만 있고
    # _start_csp_variant csp 만 호출. install 후 환경은 cims_agent 가 변종 tarball
    # 풀어 install_path/<name>/ 디렉토리 생성 → cims.sh 가 자기 변종 함수 호출.
    local name="$1"
    local upper; upper=$(echo "$name" | tr '[:lower:]' '[:upper:]')
    if is_running "$name"; then warn "$upper 이미 실행 중 (pid=$(read_pid "$name"))"; return 0; fi
    local bin="$DIST_DIR/$name/bin/$name"
    local cfg="$DIST_DIR/$name/config/$name.json"
    [[ ! -f "$bin" ]] && err "$name 바이너리 없음: $bin (make dist 또는 install 필요)" && return 1
    # deployment overlay 머지 (CSP/PSP/ISP 의 Roles/LocalIp 분기). 변종별 overlay 는
    # install_path/<name>/config.json 에 위치 — 한 install_path 에 형제 변종이 공존
    # 해도 서로 다른 overlay 를 가질 수 있도록 cims_agent.py 가 분리 저장.
    # 이전 위치 (install_path/config.json) 도 fallback 으로 봐서 단일-변종 install
    # (dev 모드 등) 와 후방 호환.
    local _overlay="$DIST_DIR/$name/config.json"
    [[ ! -f "$_overlay" ]] && _overlay="$DIST_DIR/config.json"
    # pre-launch 단계는 best-effort — 어떤 이유로든 실패해도 start abort 안 함.
    _apply_overlay_to_module_config "$_overlay" "$cfg" || true
    local sip_port; sip_port=$("$PYBIN" -c "import json; d=json.load(open('$cfg')); print(d['Setup']['Sip']['UdpPort'])" 2>/dev/null || echo 5060)
    # 삭제된 바이너리로 떠있는 동일 모듈 좀비(경로 마이그레이션 잔재) 먼저 정리
    kill_deleted_inode_orphans "$name" || true
    # 자기 install 의 좀비만 정리 — 다른 인스턴스 (PSP 127.0.0.3 등) 영향 차단
    kill_stray "$bin" || true
    _kill_own_install_listener "$bin" "$sip_port" udp || true
    info "$upper 시작..."
    cd "$DIST_DIR/$name"
    bin/$name config/$name.json -n >> "$LOG_DIR/$name.log" 2>&1 &
    save_pid "$name" $!
    sleep 1.0
    is_running "$name" && ok "$upper 시작 완료 (pid=$(read_pid "$name"))" || { err "$upper 시작 실패"; tail -3 "$LOG_DIR/$name.log" | sed 's/^/  /'; return 1; }
}

start_csp() { _start_csp_variant csp; }
start_psp() { _start_csp_variant psp; }
start_isp() { _start_csp_variant isp; }

start_cwrtc() {
    if is_running cwrtc; then warn "cwrtc 이미 실행 중 (pid=$(read_pid cwrtc))"; return 0; fi
    [[ ! -f "$DIST_DIR/cwrtc/bin/cwrtc" ]] && err "cwrtc 바이너리 없음 (make dist 실행 필요)" && return 1
    local ws_port; ws_port=$("$PYBIN" -c "import json; d=json.load(open('$DIST_DIR/cwrtc/config/cwrtc.json')); print(d['Setup']['WsPort'])" 2>/dev/null || echo 8443)
    kill_stray "cwrtc/bin/cwrtc" "$ws_port"
    info "cwrtc (WebRTC 게이트웨이) 시작... (WsPort=$ws_port)"
    cd "$DIST_DIR/cwrtc"
    mkdir -p html
    bin/cwrtc config/cwrtc.json >> "$LOG_DIR/cwrtc.log" 2>&1 &
    save_pid cwrtc $!
    sleep 1.0
    is_running cwrtc && ok "cwrtc 시작 완료 (pid=$(read_pid cwrtc))" || { err "cwrtc 시작 실패"; tail -5 "$LOG_DIR/cwrtc.log" | sed 's/^/  /'; return 1; }
}

start_csc() {
    if is_running csc; then warn "CSC 이미 실행 중 (pid=$(read_pid csc))"; return 0; fi
    [[ ! -f "$DIST_DIR/csc/src/csc_app.py" ]] && err "CSC 소스 없음 (make dist 실행 필요)" && return 1
    # overlay-aware port: deployment overlay 를 먼저 확인.
    # cims_agent 가 변종별로 분리 저장 (install_path/csc/config.json) — fallback 으로
    # legacy 위치 (install_path/config.json) 도 본다.
    local csc_port
    csc_port=$("$PYBIN" -c "
import json, os
base='$DIST_DIR/csc/config/csc.json'
candidates=['$DIST_DIR/csc/config.json', '$DIST_DIR/config.json']
p=None
for ov in candidates:
    if not os.path.isfile(ov): continue
    try:
        f=json.load(open(ov))
        if isinstance(f,dict):
            p=f.get('Server.Port') or (f.get('Server',{}) or {}).get('Port')
            if p: break
    except: pass
if not p:
    try: p=json.load(open(base))['Server']['Port']
    except: p=4420
print(p)" 2>/dev/null || echo 4420)
    # DIST_DIR 포함 절대경로 pattern — Phase 1/2 csc 공존 시 상호 kill 방지
    kill_stray "$DIST_DIR/csc/src/csc_app.py" "$csc_port" tcp
    info "CSC (REST API 서버) 시작... (port=$csc_port)"
    cd "$DIST_DIR/csc/src"
    "$PYBIN" -u "$DIST_DIR/csc/src/csc_app.py" >> "$LOG_DIR/csc.log" 2>&1 &
    save_pid csc $!
    sleep 1.5
    is_running csc && ok "CSC 시작 완료 (pid=$(read_pid csc), port=$csc_port)" || { err "CSC 시작 실패"; tail -3 "$LOG_DIR/csc.log" | sed 's/^/  /'; return 1; }
}

# OAM 분리 Phase 3b — cims@oam.service / cims-svc start oam 으로 동작.
# OAM(4419) = Agent / HA / 배포 / 검증 책임. CSC(4420 admin + 4430 mcptt) 와 별개 프로세스.
start_oam() {
    if is_running oam; then warn "OAM 이미 실행 중 (pid=$(read_pid oam))"; return 0; fi
    [[ ! -f "$DIST_DIR/oam/src/oam_app.py" ]] && err "OAM 소스 없음 (make dist 실행 필요)" && return 1

    # ── oam.json 경로 자가 교정 ───────────────────────────────────────────────
    # make dist 로 생성된 oam.json 은 배포 서버 기준 경로(/home/cims/work/...)를 담는다.
    # 개발 환경(build/dist)에서 그대로 쓰면 PermissionError 로 OAM 이 즉시 종료.
    # 시작 직전 두 필드가 현재 환경에서 쓰기 가능한지 확인하고, 불가능하면 DIST_DIR
    # 상대 기본값으로 자동 교정한다.
    local _oam_cfg="$DIST_DIR/oam/config/oam.json"
    "$PYBIN" -c "
import json, os, sys

cfg_path = '$_oam_cfg'
dist_dir = '$DIST_DIR'

def can_mkdir(p):
    try:
        os.makedirs(p, exist_ok=True)
        return True
    except (PermissionError, OSError):
        return False

try:
    with open(cfg_path) as f:
        c = json.load(f)
except Exception as e:
    sys.exit(0)  # 읽기 실패 시 OAM 자체에 맡김

changed = False

# CimsRuntimeDir
cur = c.get('CimsRuntimeDir', '')
if cur and not can_mkdir(cur):
    new = os.path.join(dist_dir, 'ext_mnt', 'runtime')
    os.makedirs(new, exist_ok=True)
    c['CimsRuntimeDir'] = new
    changed = True
    print(f'[auto-fix] CimsRuntimeDir: {cur} -> {new}', flush=True)

# ServiceLogging.Dir
sl = c.get('ServiceLogging', {})
cur_sl = sl.get('Dir', '')
if cur_sl and not can_mkdir(cur_sl):
    new_sl = os.path.join(dist_dir, 'ext_mnt', 'service_log')
    os.makedirs(new_sl, exist_ok=True)
    sl['Dir'] = new_sl
    c['ServiceLogging'] = sl
    changed = True
    print(f'[auto-fix] ServiceLogging.Dir: {cur_sl} -> {new_sl}', flush=True)

if changed:
    with open(cfg_path, 'w') as f:
        json.dump(c, f, indent=4, ensure_ascii=False)
" 2>/dev/null
    # ─────────────────────────────────────────────────────────────────────────

    local oam_port
    oam_port=$("$PYBIN" -c "
import json, os
base='$DIST_DIR/oam/config/oam.json'
candidates=['$DIST_DIR/oam/config.json', '$DIST_DIR/config.json']
p=None
for ov in candidates:
    if not os.path.isfile(ov): continue
    try:
        f=json.load(open(ov))
        if isinstance(f,dict):
            p=f.get('Server.Port') or (f.get('Server',{}) or {}).get('Port')
            if p: break
    except: pass
if not p:
    try: p=json.load(open(base))['Server']['Port']
    except: p=4419
print(p)" 2>/dev/null || echo 4419)
    kill_stray "$DIST_DIR/oam/src/oam_app.py" "$oam_port" tcp
    # oam_base_service_split §8 — 역할 플래그. 기본 all = 현행 단일프로세스(무변경).
    # 분리 배포(게이트웨이) 노드만 OAM_ROLE=base 로 기동 → 서비스 라우트는 csc/oam-svc 로 프록시.
    local oam_role="${OAM_ROLE:-all}"
    info "OAM (Operation & Management REST API) 시작... (port=$oam_port, role=$oam_role)"
    cd "$DIST_DIR/oam/src"
    "$PYBIN" -u "$DIST_DIR/oam/src/oam_app.py" --role "$oam_role" >> "$LOG_DIR/oam.log" 2>&1 &
    save_pid oam $!
    # D1 (self-upgrade): sleep 1.5 단발 판정 대신 /health 200 까지 폴링(최대 T초).
    # Python OAM 콜드스타트(config+마이그레이션+cert+bind)는 1.5s 를 넘길 수 있어,
    # self-upgrade 시 agent 의 후속 report 가 "아직 안 뜬 신 OAM" 에 닿아 유실되던 문제 방지.
    if _oam_health_gate "$oam_port" "${CIMS_OAM_HEALTH_TIMEOUT:-20}"; then
        ok "OAM 시작 완료 (pid=$(read_pid oam), port=$oam_port, /health 200)"
    else
        err "OAM 시작 실패 — /health 미응답 (${CIMS_OAM_HEALTH_TIMEOUT:-20}s)"
        tail -5 "$LOG_DIR/oam.log" | sed 's/^/  /'
        return 1
    fi
}

# OAM 전용 health-gate: 프로세스 생존 + https://127.0.0.1:<port>/health 200 까지
# 최대 timeout_s 초 폴링. python(urllib, 인증서 무검증)으로 probe — curl 부재 환경 대비.
# OAM /health 는 무인증 200 {"status":"ok"} (httpsrv 내장 라우트).
_oam_health_gate() {
    local port="$1" timeout_s="${2:-20}"
    local i=0
    while [[ $i -lt $((timeout_s * 2)) ]]; do
        if ! is_running oam; then
            # 프로세스가 떠 있지 않으면 잠깐 대기 후 재확인 (start 직후 race)
            sleep 0.5; i=$((i + 1)); continue
        fi
        if "$PYBIN" - "$port" <<'PYHC' >/dev/null 2>&1
import sys, ssl, urllib.request
port = sys.argv[1]
ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
r = urllib.request.urlopen("https://127.0.0.1:%s/health" % port, timeout=2, context=ctx)
sys.exit(0 if r.status == 200 else 1)
PYHC
        then
            return 0
        fi
        sleep 0.5; i=$((i + 1))
    done
    return 1
}

stop_oam() {
    stop_one oam
}

# oam_base_service_split P3 (D5) — oam-svc 독립 서비스 모듈(서비스 관측/녹취/flow/검증).
#   base OAM(게이트웨이) 뒤 loopback(기본 4480) 업스트림. csc 와 동격 독립 프로세스.
#   ⚠️ 추가(additive)·dormant: 기본 desired-state(supervised)에 자동 편입하지 않는다 —
#      분리 배포(--role base) 채택 시점(P5)에 supervised.json 으로 등록. all 모드(단일
#      프로세스)에선 미사용. kill_stray 패턴은 고유 절대경로(oam_svc_app.py)라 oam_app.py/
#      csc_app.py 와 교차 매칭되지 않음(pgrep 자기명중 방지).
start_oam_svc() {
    if is_running oam-svc; then warn "oam-svc 이미 실행 중 (pid=$(read_pid oam-svc))"; return 0; fi
    [[ ! -f "$DIST_DIR/oam-svc/src/oam_svc_app.py" ]] && err "oam-svc 소스 없음 (make dist 실행 필요)" && return 1
    local svc_port
    svc_port=$("$PYBIN" -c "
import json, os
base='$DIST_DIR/oam-svc/config/oam-svc.json'
candidates=['$DIST_DIR/oam-svc/config.json', '$DIST_DIR/config.json']
p=None
for ov in candidates:
    if not os.path.isfile(ov): continue
    try:
        f=json.load(open(ov))
        if isinstance(f,dict):
            p=f.get('Server.Port') or (f.get('Server',{}) or {}).get('Port')
            if p: break
    except: pass
if not p:
    try: p=json.load(open(base))['Server']['Port']
    except: p=4480
print(p)" 2>/dev/null || echo 4480)
    kill_stray "$DIST_DIR/oam-svc/src/oam_svc_app.py" "$svc_port" tcp
    info "oam-svc (서비스 관측/녹취/flow/검증) 시작... (port=$svc_port)"
    cd "$DIST_DIR/oam-svc/src"
    "$PYBIN" -u "$DIST_DIR/oam-svc/src/oam_svc_app.py" >> "$LOG_DIR/oam-svc.log" 2>&1 &
    save_pid oam-svc $!
    sleep 1.5
    is_running oam-svc && ok "oam-svc 시작 완료 (pid=$(read_pid oam-svc), port=$svc_port)" \
        || { err "oam-svc 시작 실패"; tail -3 "$LOG_DIR/oam-svc.log" | sed 's/^/  /'; return 1; }
}

stop_oam_svc() {
    stop_one oam-svc
}

start_console() {
    if is_running console; then warn "console 이미 실행 중 (pid=$(read_pid console))"; return 0; fi
    # Console 3분화:
    #   Dev-Console     : 소스 vite dev, 기본 3001
    #   Test-Console    : build/dist/console/dist serve, 기본 8080 (HTTPS)
    #   배포본 console  : mgmt-server/console/, deployment overlay 의 Port 로 기동 (기본 8081)
    # overlay port: deployment POST 의 config 필드가 저장. cims_agent 가 변종별로
    # 분리 저장 (install_path/console/config.json) — fallback 으로 legacy 위치도 본다.
    local port
    port=$("$PYBIN" -c "
import json, os
candidates=['$DIST_DIR/console/config.json', '$DIST_DIR/config.json']
p=None
for ov in candidates:
    if not os.path.isfile(ov): continue
    try:
        f=json.load(open(ov))
        if isinstance(f,dict):
            p=f.get('Server.Port') or f.get('Port') or (f.get('Server',{}) or {}).get('Port')
            if p: break
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
            || { err "Dev-Console 시작 실패"; tail -3 "$LOG_DIR/console.log" | sed 's/^/  /'; return 1; }
    elif [[ -d "$DIST_DIR/console/dist" ]]; then
        [[ -z $port ]] && port=8080
        kill_stray "serve dist -l $port" "$port" tcp
        info "Test-Console (Admin Web UI, dist 정적 서빙) 시작... (port $port, HTTPS)"
        cd "$DIST_DIR/console"
        _SSL_KEY="$DIST_DIR/csc/cert/server.key"
        _SSL_CERT="$DIST_DIR/csc/cert/server.crt"
        # 배포본 환경 (mgmt-server/console) 에서는 csc cert 가 다른 경로 — 그 쪽도 탐색
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
            || { err "Test-Console 시작 실패"; tail -3 "$LOG_DIR/console.log" | sed 's/^/  /'; return 1; }
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
    is_running phone && ok "phone 시작 완료 (pid=$(read_pid phone))" || { err "phone 시작 실패"; tail -3 "$LOG_DIR/phone.log" | sed 's/^/  /'; return 1; }
}

# ── TB (Test-Bed) 2종: TB-CSC(4419) / TB-Console(3000) ──
start_tb_csc() {
    if is_running tb-csc; then warn "TB-CSC 이미 실행 중 (pid=$(read_pid tb-csc))"; return 0; fi
    local tb_cfg="$DIST_DIR/csc/config/csc-tb.json"
    [[ ! -f "$tb_cfg" ]] && err "TB-CSC config 없음: $tb_cfg  (./configure.sh 실행)" && return 1
    [[ ! -f "$DIST_DIR/csc/src/csc_app.py" ]] && err "CSC 소스 없음 (make dist 실행 필요)" && return 1
    kill_stray "CIMS_CSC_CONFIG=.*csc-tb.json" 4419 tcp
    info "TB-CSC (4419) 시작..."
    cd "$DIST_DIR/csc/src"
    CIMS_CSC_CONFIG="$tb_cfg" "$PYBIN" csc_app.py >> "$LOG_DIR/tb-csc.log" 2>&1 &
    save_pid tb-csc $!
    sleep 1.5
    is_running tb-csc && ok "TB-CSC 시작 완료 (pid=$(read_pid tb-csc), port=4419)" \
        || { err "TB-CSC 시작 실패"; tail -3 "$LOG_DIR/tb-csc.log" | sed 's/^/  /'; return 1; }
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
        || { err "TB-Console 시작 실패"; tail -5 "$LOG_DIR/tb-console.log" | sed 's/^/  /'; return 1; }
}

# ── 중지 함수 ──
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
    csc_port=$("$PYBIN" -c "
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

# ── 상태 / 포트 ──
_svc_port_proto() {
    case "$1" in
        cmp|pmp|imp) echo "9000:udp" ;;
        csp|psp|isp) echo "5060:udp" ;;
        cwrtc)      echo "8443:tcp" ;;
        csc)        echo "4421:tcp" ;;
        oam)        echo "4419:tcp" ;;   # OAM 분리 Phase 3b
        # console 은 모드별 포트 분기 — Dev(소스 트리) 3001 / Test(dist 전용) 8080.
        console)
            if [[ -n "$SRC_CONSOLE" && -d "$SRC_CONSOLE" ]]; then echo "3001:tcp"
            else echo "8080:tcp"; fi ;;
        phone)      echo "3002:tcp" ;;
        tb-csc)     echo "" ;;          # OAM 이 4419 소유 — 포트 공유 오탐 방지; PID 파일로만 감지
        tb-oam)     echo "" ;;
        tb-console) echo "3000:tcp" ;;
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
    status_one pmp
    status_one imp
    status_one csp
    status_one psp
    status_one isp
    status_one cwrtc
    status_one oam
    status_one csc
    status_one console
    status_one phone
    # TB(Test-Bed)는 개발 전용 — 실제 기동 중일 때만 표시 (상용 배포본에선 숨김)
    if is_running tb-csc || is_running tb-console; then
        echo ""
        echo -e "  ${BOLD}[TB (Test-Bed — Phase 2/3 UI 유지용 상시 기동)]${NC}"
        status_one tb-csc
        status_one tb-console
    fi
    echo ""
}

# ── 로그 ──
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

# ── 컴포넌트 dispatcher ──
COMPONENTS=(cmp cmdp csp cwrtc oam csc console phone)

_start_one() {
    case "$1" in
        all)        start_cmp; start_cmdp; start_csp; sleep 0.5; start_cwrtc; start_oam; start_csc; start_console; start_phone ;;
        tb)         start_tb_csc; sleep 0.5; start_tb_console ;;
        cmp)        start_cmp ;;
        pmp)        start_pmp ;;
        imp)        start_imp ;;
        cmdp)       start_cmdp ;;
        csp)        start_csp ;;
        psp)        start_psp ;;
        isp)        start_isp ;;
        cwrtc)      start_cwrtc ;;
        oam)        start_oam ;;     # OAM 분리 Phase 3b
        csc)        start_csc ;;
        oam-svc)   start_oam_svc ;;  # oam_base_service_split P3 — 명시 기동만(all 미포함)
        console)    start_console ;;
        phone)      start_phone ;;
        tb-csc)     start_tb_csc ;;
        tb-console) start_tb_console ;;
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
                    oam)     stop_oam ;;     # OAM 분리 Phase 3b
                    oam-svc) stop_oam_svc ;;
                    console) stop_console ;;
                    phone)   stop_phone ;;
                    *)       stop_one "$c" ;;
                esac
            done
            # P1/P2 배포본 인스턴스 — install 후 dev COMPONENTS 와 별개 프로세스로 동작.
            # _agents 는 cmd_reset 의 목록과 동기 유지. exe/cwd 가 install_path 하위인
            # 프로세스를 enumerate 하여 SIGTERM → 짧게 wait → 잔존 SIGKILL.
            # agent_name 자체 (cims_agent.py) 는 heartbeat 유지를 위해 건드리지 않음.
            # 변종 csp/cmp 도 SIGTERM 에 즉시 종료 안되는 경우 있어 SIGKILL fallback 필수.
            local _stop_agents=(volte-sip-server volte-media-server
                                ptt-sip-server ptt-media-server)
            local _all_pids="" _sa _sa_path _sa_pids _any=0 _p _exe _cwd
            for _sa in "${_stop_agents[@]}"; do
                [[ ! -d "$DIST_DIR/$_sa" ]] && continue
                _sa_path="$DIST_DIR/$_sa"
                _sa_pids=""
                for _p in /proc/[0-9]*; do
                    _exe=$(readlink "$_p/exe" 2>/dev/null || true)
                    _cwd=$(readlink "$_p/cwd" 2>/dev/null || true)
                    _exe="${_exe% (deleted)}"
                    _cwd="${_cwd% (deleted)}"
                    if [[ "$_exe" == "$_sa_path/"* || "$_cwd" == "$_sa_path"* ]]; then
                        # agent 프로세스는 heartbeat 유지 — 제외
                        grep -qa "cims_agent.py" "$_p/cmdline" 2>/dev/null && continue
                        _sa_pids+=" $(basename "$_p")"
                    fi
                done
                if [[ -n $_sa_pids ]]; then
                    [[ $_any -eq 0 ]] && header "=== 배포본 인스턴스 중지 (변종 psp/isp/pmp/imp 포함) ===" && _any=1
                    ok "$_sa: pid$_sa_pids"
                    kill $_sa_pids 2>/dev/null || true
                    _all_pids+=" $_sa_pids"
                fi
            done
            # SIGTERM 후 최대 3s wait, 잔존은 SIGKILL
            if [[ -n $_all_pids ]]; then
                local _i=0 _alive
                while (( _i < 15 )); do
                    _alive=0
                    for _p in $_all_pids; do kill -0 "$_p" 2>/dev/null && { _alive=1; break; }; done
                    [[ $_alive -eq 0 ]] && break
                    sleep 0.2; _i=$((_i+1))
                done
                # 남은 것 SIGKILL
                for _p in $_all_pids; do
                    if kill -0 "$_p" 2>/dev/null; then
                        warn "  pid=$_p SIGTERM 무응답 → SIGKILL"
                        kill -9 "$_p" 2>/dev/null || true
                    fi
                done
            fi
            ;;
        tb)
            header "=== TB 중지 ==="
            stop_one tb-console
            stop_one tb-csc
            ;;
        csc)     stop_csc ;;
        oam)     stop_oam ;;        # OAM 분리 Phase 3b
        oam-svc) stop_oam_svc ;;
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

cmd_restart() {
    # stop → wait → kill_stray (binary 기준 stale process 강제 정리) → start.
    # 가설 (C, 세션 2026-05-15): SIGKILL 후 pid 파일이 race 로 살아남거나, pid
    # 파일은 삭제됐어도 stale process 가 port 점유 중이면 start 가 bind 실패.
    # is_running 강화로 (b) 해소, 추가 kill_stray + sleep 증가로 (c) 해소.
    if [[ $# -eq 0 ]]; then
        cmd_stop all
        _restart_cleanup_strays all
        sleep 3
        cmd_start all
        return
    fi
    cmd_stop "$@"
    local t
    for t in "$@"; do _restart_cleanup_strays "$t"; done
    sleep 3
    cmd_start "$@"
}

# restart 시 stop 후 stale process 한 번 더 정리. binary path 가 있으면
# pgrep -f $bin 으로 stray 정리.
_restart_cleanup_strays() {
    local svc="$1"
    case "$svc" in
        all|tb) return 0 ;;                  # 묶음 처리 — 자체 stop 에서 처리
        csp|psp|isp|cmp|pmp|imp)
            local bin="$DIST_DIR/$svc/bin/$svc"
            [[ -f "$bin" ]] && kill_stray "$bin" || true
            ;;
        *) return 0 ;;
    esac
}
