#!/bin/bash
# agent/lib/lifecycle.sh — CIMS service lifecycle library
#
# 본 파일은 source 후 함수만 노출하는 library — standalone 실행 금지.
# Caller (agent/bin/cims-svc) 가 아래 환경변수와 helpers 를 미리 정의해야 함:
#   변수:    SCRIPT_DIR, DIST_DIR, PID_DIR, LOG_DIR
#   색상:    RED, GREEN, YELLOW, CYAN, BOLD, NC
#   logger:  info(), ok(), warn(), err(), header()
#
# 엔진 순수성: 배포본 계약(dist 의 bin/config/overlay)만 다룬다. 소스 트리
# 전용 동작(vite dev 콘솔 등)은 개발 프론트 cims.sh 의 영역 — 여기 넣지 않는다.

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
    # capability 바이너리는 직접 readlink 가 거부 — _proc_exe 가 root 위임 폴백.
    local expected="$DIST_DIR/$name/bin/$name"
    if [[ -x "$expected" ]]; then
        local exe; exe=$(_proc_exe "$pid"); exe="${exe% (deleted)}"
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
        exe=$(_proc_exe "$pid")
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
        # tarball 풀어 inode 교체된 옛 process 의 exe 는 '/path/to/bin (deleted)'
        # 형태 — raw target 에서 ' (deleted)' suffix 만 trim. capability 바이너리는
        # 직접 readlink 거부 → _proc_exe 가 root 위임 폴백.
        local pid_exe; pid_exe=$(_proc_exe "$pid")
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

# ── 자기 exe 프로세스 헬퍼 ───────────────────────────────────
# lifecycle 는 상대경로(cd 후 bin/<name>)로 기동하므로 cmdline 패턴(pgrep -f 절대경로)
# 으로는 자기 프로세스가 잡히지 않는다 — /proc/<pid>/exe 대조가 정본. deleted-inode
# 프로세스는 제외 (그쪽은 kill_deleted_inode_orphans 담당).
#
# ⚠ 파일 capability 바이너리(csp setcap cap_net_admin — IMS AKA+IPsec)의 프로세스는
# ptrace 접근 검사(대상 caps ⊆ 호출자 caps)로 동일 uid 라도 /proc/<pid>/exe 읽기와
# ss -p 소켓 귀속이 거부된다. 이때는 동봉 cims-priv(sudoers NOPASSWD, 읽기 전용
# 서브커맨드)로 root 위임해 식별을 복원한다 — 위임 불가 환경(sudoers 미구성 dev 등)
# 은 조용히 폴백 없이 종전 동작(미식별=보수적 no-op).
_priv_bin() {
    # sudoers 는 버전 실경로로 등재 — current 심볼릭 경유 호출은 매칭 안 되므로 해소.
    local p; p=$(readlink -f "$SCRIPT_DIR/cims-priv" 2>/dev/null || true)
    [[ -n "$p" && -x "$p" ]] && { echo "$p"; return 0; }
    p=$(readlink -f "$SCRIPT_DIR/bin/cims-priv" 2>/dev/null || true)
    [[ -n "$p" && -x "$p" ]] && echo "$p"
    return 0
}

_proc_exe() {
    # /proc/<pid>/exe 해석 — 직접 읽기 실패 시(capability 바이너리) root 위임.
    local pid="$1" exe
    exe=$(readlink "/proc/$pid/exe" 2>/dev/null || true)
    if [[ -z "$exe" && -d "/proc/$pid" ]]; then
        local priv; priv=$(_priv_bin)
        [[ -n "$priv" ]] && exe=$(sudo -n "$priv" proc-exe "$pid" 2>/dev/null || true)
    fi
    echo "$exe"
    return 0
}

_pids_by_exe() {
    # bash 내장 -ef (device+inode 대조) — 프로세스마다 외부 readlink fork 는 반복
    # 호출(start 게이트 폴링)에서 수 초씩 걸린다. inode 교체된 옛 버전 바이너리는
    # 자연히 불일치 (그쪽은 kill_deleted_inode_orphans 담당).
    local bin="$1" p
    [[ -e "$bin" ]] || return 0
    local found=0
    for p in /proc/[0-9]*; do
        [[ "$p/exe" -ef "$bin" ]] && { echo "${p##*/}"; found=1; }
    done
    # 직접 대조 무소득 → capability 바이너리 가능성 — root 위임 열거
    if [[ $found -eq 0 ]]; then
        local priv; priv=$(_priv_bin)
        [[ -n "$priv" ]] && sudo -n "$priv" proc-pids-of "$bin" 2>/dev/null || true
    fi
    return 0
}

# 자기 exe 로 실행 중인 잔존 프로세스 정리 — SIGTERM → 짧게 wait → 잔존 SIGKILL.
_kill_own_exe_strays() {
    local bin="$1"
    local pids; pids=$(_pids_by_exe "$bin")
    [[ -z "$pids" ]] && return 0
    warn "자기 install 잔존 프로세스 정리: $(basename "$bin") pid=$(echo $pids | tr '\n' ' ')"
    kill $pids 2>/dev/null || true
    local i=1 p left
    while (( i <= 15 )); do
        left=""
        for p in $pids; do kill -0 "$p" 2>/dev/null && left="1"; done
        [[ -z "$left" ]] && break
        sleep 0.2; i=$(( i + 1 ))
    done
    for p in $pids; do
        kill -0 "$p" 2>/dev/null && kill -9 "$p" 2>/dev/null || true
    done
    return 0
}

# ── CSP 계열 실효 리스너 (local_nodes.jsonl) ─────────────────
# CSP 의 SIP 접속점 정본은 csp.json 이 아니라 local_nodes.jsonl 이다 — csp.json 의
# Setup.Sip.UdpPort 는 identity fallback 일 뿐 실제 bind 포트가 아닐 수 있다 (stale
# 5060 을 보고 15060 좀비를 못 죽이던 결함). 해석 순서는 CSP(SipServerSetup.cpp)와
# 동일: Setup.ConfigJsonlDir → install 루트 config/ → 변종 내부 config/.
_csp_local_nodes_path() {
    local cfg="$1" name="$2"
    local d
    d=$("$PYBIN" -c "import json; print(json.load(open('$cfg')).get('Setup',{}).get('ConfigJsonlDir') or '')" 2>/dev/null || echo "")
    local p
    for p in "${d:+$d/local_nodes.jsonl}" "$DIST_DIR/config/local_nodes.jsonl" "$DIST_DIR/$name/config/local_nodes.jsonl"; do
        [[ -n "$p" && -f "$p" ]] && { echo "$p"; return 0; }
    done
    echo ""
    return 0
}

# enabled 리스너 전부를 "<proto> <port>" 줄로 출력 (proto = udp|tcp, TLS→tcp, dedup).
_csp_listener_ports() {
    local jsonl="$1"
    [[ -n "$jsonl" && -f "$jsonl" ]] || return 0
    "$PYBIN" - "$jsonl" <<'PY' 2>/dev/null || true
import json, sys
seen = set()
for line in open(sys.argv[1], encoding="utf-8"):
    line = line.strip()
    if not line: continue
    try: r = json.loads(line)
    except Exception: continue
    if not isinstance(r, dict) or not r.get("enabled"): continue
    try: port = int(r.get("bind_port") or 0)
    except Exception: continue
    if not (0 < port < 65536): continue
    proto = "udp" if str(r.get("protocol") or "UDP").upper() == "UDP" else "tcp"
    if (proto, port) in seen: continue
    seen.add((proto, port))
    print(proto, port)
PY
}

# primary(enabled) 리스너를 "<proto> <port>" 로 출력 — 없으면 첫 enabled, 그것도 없으면 빈 값.
_csp_primary_listener() {
    local jsonl="$1"
    [[ -n "$jsonl" && -f "$jsonl" ]] || return 0
    "$PYBIN" - "$jsonl" <<'PY' 2>/dev/null || true
import json, sys
first = None
for line in open(sys.argv[1], encoding="utf-8"):
    line = line.strip()
    if not line: continue
    try: r = json.loads(line)
    except Exception: continue
    if not isinstance(r, dict) or not r.get("enabled"): continue
    try: port = int(r.get("bind_port") or 0)
    except Exception: continue
    if not (0 < port < 65536): continue
    proto = "udp" if str(r.get("protocol") or "UDP").upper() == "UDP" else "tcp"
    if r.get("is_primary"):
        print(proto, port); break
    if first is None: first = (proto, port)
else:
    if first: print(first[0], first[1])
PY
}

# pidfile 을 실제 실행 worker 로 확정. owner(포트 소유 pid)가 자기 exe 면 그것,
# 아니면 현 pidfile pid 가 자기 exe 로 살아있으면 유지, 둘 다 아니면 own-exe 스캔.
_csp_sync_pidfile() {
    local name="$1" bin="$2" owner="${3:-}"
    local bin_real; bin_real=$(readlink -f "$bin" 2>/dev/null || true)
    if [[ -n "$owner" && -n "$bin_real" ]]; then
        local oexe; oexe=$(_proc_exe "$owner"); oexe="${oexe% (deleted)}"
        if [[ "$oexe" == "$bin_real" ]]; then
            [[ "$(read_pid "$name")" != "$owner" ]] && save_pid "$name" "$owner"
            return 0
        fi
        # 소유자 identity 미해석(위임 불가 환경) — 포트는 우리 설정 포트이므로 소유자 채택
        if [[ -z "$oexe" ]]; then
            [[ "$(read_pid "$name")" != "$owner" ]] && save_pid "$name" "$owner"
            return 0
        fi
    fi
    local cur; cur="$(read_pid "$name")"
    if [[ -n "$cur" && -n "$bin_real" ]] && kill -0 "$cur" 2>/dev/null; then
        local cexe; cexe=$(_proc_exe "$cur"); cexe="${cexe% (deleted)}"
        [[ "$cexe" == "$bin_real" ]] && return 0
    fi
    local adopt; adopt=$(_pids_by_exe "$bin" | head -1)
    [[ -n "$adopt" ]] && save_pid "$name" "$adopt"
    return 0
}

# ── CSP 계열 start 판정 게이트 ───────────────────────────────
# 성공 = 자기 exe worker 생존 + primary 접속점 bind 확인. 판정 시점에 pidfile 을 실제
# worker(포트 소유자)로 확정한다 — 초기 pid($!)가 기동 중 죽고 재fork worker 가 서비스를
# 잇는 경우의 pidfile 고아 방지 (2026-08-25 csp 재기동 불안정의 원인). 포트 판정이
# 불가한 환경(ss 부재/포트 미상)은 1s 생존 판정으로 폴백 (구 동작 — 도구는 베이스
# 이미지 책임).
_csp_start_gate() {
    local name="$1" bin="$2" proto="$3" port="$4" timeout_s="${5:-20}"
    local bin_real; bin_real=$(readlink -f "$bin" 2>/dev/null || true)
    local probe=0
    [[ -n "$port" ]] && command -v ss >/dev/null 2>&1 && probe=1
    local i=0 dead=0 max=$(( timeout_s * 2 ))
    while (( i < max )); do
        local pids; pids=$(_pids_by_exe "$bin")
        if [[ -z "$pids" ]]; then
            # 기동 직후 exec 전 window 는 관대하게 — 1s 지나서도 2s 연속 무프로세스면 조기 실패
            dead=$(( dead + 1 ))
            if (( i >= 2 && dead >= 4 )); then return 1; fi
        else
            dead=0
            if (( probe == 0 )); then
                if (( i >= 2 )); then
                    _csp_sync_pidfile "$name" "$bin" ""
                    return 0
                fi
            else
                local owner; owner=$(_pid_by_port "$port:$proto")
                if [[ -n "$owner" ]]; then
                    local oexe; oexe=$(_proc_exe "$owner"); oexe="${oexe% (deleted)}"
                    # identity 확증 실패(oexe 비어있음 — 위임 불가 환경의 capability
                    # 프로세스)는 성공 취급 — 우리 설정 포트가 bind 된 사실이 우선.
                    if [[ -n "$oexe" && -n "$bin_real" && "$oexe" != "$bin_real" ]]; then
                        warn "포트 $port($proto) 를 다른 프로세스가 점유 (pid=$owner exe=$oexe)"
                        return 1
                    fi
                    _csp_sync_pidfile "$name" "$bin" "$owner"
                    return 0
                fi
            fi
        fi
        sleep 0.5; i=$(( i + 1 ))
    done
    return 1
}

# ── deployment overlay 머지 ──────────────────────────────────
# overlay(flat dotted key) 를 모듈 config(nested json) 에 병합한다. 병합은 target 파일
# 자체에 누적되므로, overlay 에서 **삭제된 키의 전파**를 위해 직전 적용 키 목록을
# 사이드카(<overlay>.applied)에 기록하고, 다음 병합 때 사라진 키를 target 에서 제거한다
# (해당 설정은 모듈의 코드 기본값으로 복귀). 이 목록 없이는 한번 병합된 키가 OAM 설정
# 저장소에서 지워져도 영구 잔존한다.
_apply_overlay_to_module_config() {
    local overlay="$1" target="$2"
    [[ ! -f "$overlay" || ! -f "$target" ]] && return 0
    "$PYBIN" - "$overlay" "$target" <<'PY' 2>/dev/null
import json, os, sys
ov_path, tgt_path = sys.argv[1], sys.argv[2]
applied_path = ov_path + ".applied"
try:
    with open(ov_path, encoding="utf-8") as f: ov = json.load(f)
    with open(tgt_path, encoding="utf-8") as f: tgt = json.load(f)
except Exception:
    sys.exit(0)
if not isinstance(ov, dict) or not isinstance(tgt, dict):
    sys.exit(0)

prev = []
try:
    with open(applied_path, encoding="utf-8") as f: prev = json.load(f)
except Exception:
    prev = []
if not isinstance(prev, list): prev = []

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

def del_path(root, dotted):
    # dotted leaf 삭제 + 비게 된 중간 dict 정리. 경로 부재/타입 불일치는 no-op.
    keys = dotted.split(".")
    stack = []
    cur = root
    for k in keys[:-1]:
        nxt = cur.get(k)
        if not isinstance(nxt, dict): return False
        stack.append((cur, k))
        cur = nxt
    if keys[-1] not in cur: return False
    del cur[keys[-1]]
    for parent, k in reversed(stack):
        if parent.get(k) == {}: del parent[k]
        else: break
    return True

changed = False

# 삭제 전파 — dotted 키만 추적 대상 (OAM deployment config 는 전부 dotted).
#   비 dotted 키는 base config 와 구분이 안 돼 삭제하지 않는다 (기존 동작 유지).
for k in prev:
    if isinstance(k, str) and "." in k and k not in ov:
        if del_path(tgt, k): changed = True

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
    tmp = tgt_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(tgt, f, indent=4, ensure_ascii=False)
    os.replace(tmp, tgt_path)

# 적용 키 목록 갱신 (병합 무변경이어도 기록 — 최초 도입 시점부터 추적 시작)
try:
    tmp = applied_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(sorted(k for k in ov.keys() if "." in k), f)
    os.replace(tmp, applied_path)
except Exception:
    pass
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

    # 실효 리스너 = local_nodes.jsonl (없으면 csp.json Setup.Sip.UdpPort 폴백 — dev 등)
    local nodes; nodes="$(_csp_local_nodes_path "$cfg" "$name")"
    local primary; primary="$(_csp_primary_listener "$nodes")"
    if [[ -z "$primary" ]]; then
        local _fp; _fp=$("$PYBIN" -c "import json; d=json.load(open('$cfg')); print(d['Setup']['Sip']['UdpPort'])" 2>/dev/null || echo 5060)
        primary="udp $_fp"
    fi
    local proto="${primary%% *}" port="${primary##* }"

    # pidfile 이 유실/사망이어도 자기 exe worker 가 primary 접속점을 물고 살아있으면
    # 승계한다 — 죽여서 재기동하면 멀쩡한 서비스가 끊긴다 (worker 재fork 로 pidfile 이
    # 고아가 된 케이스의 멱등 start).
    if [[ -n "$port" ]] && command -v ss >/dev/null 2>&1; then
        local _owner; _owner=$(_pid_by_port "$port:$proto")
        if [[ -n "$_owner" ]]; then
            local _breal; _breal=$(readlink -f "$bin" 2>/dev/null || true)
            local _oexe; _oexe=$(_proc_exe "$_owner"); _oexe="${_oexe% (deleted)}"
            if [[ -n "$_breal" && "$_oexe" == "$_breal" ]]; then
                save_pid "$name" "$_owner"
                warn "$upper 이미 실행 중 — 실행 worker 로 pidfile 승계 (pid=$_owner, ${proto}:$port)"
                return 0
            fi
        fi
    fi

    # 삭제된 바이너리로 떠있는 동일 모듈 좀비(경로 마이그레이션 잔재) 먼저 정리
    kill_deleted_inode_orphans "$name" || true
    # 자기 install 의 좀비만 정리 — 다른 인스턴스 (PSP 127.0.0.3 등) 영향 차단.
    # kill_stray(cmdline 패턴)는 절대경로 직접 실행 케이스, _kill_own_exe_strays(/proc
    # exe 대조)는 lifecycle 상대경로 기동 케이스를 담당.
    kill_stray "$bin" || true
    _kill_own_exe_strays "$bin" || true
    # 실효 리스너 전 포트(UDP/TCP/TLS)의 자기 좀비 정리
    if [[ -n "$nodes" ]]; then
        local lproto lport
        while read -r lproto lport; do
            [[ -z "$lport" ]] && continue
            _kill_own_install_listener "$bin" "$lport" "$lproto" || true
        done < <(_csp_listener_ports "$nodes")
    else
        _kill_own_install_listener "$bin" "$port" "$proto" || true
    fi
    # 정리 직후 primary 포트 release 대기 — kill 직후 소켓 회수 전 bind 경합으로
    # worker 가 abort(terminate)하던 경합창 회피.
    if [[ -n "$port" ]] && command -v ss >/dev/null 2>&1; then
        local _k=1
        while (( _k <= 10 )); do
            [[ -z "$(_pid_by_port "$port:$proto")" ]] && break
            sleep 0.3; _k=$(( _k + 1 ))
        done
    fi

    info "$upper 시작... (primary ${proto}:$port)"
    cd "$DIST_DIR/$name"
    bin/$name config/$name.json -n >> "$LOG_DIR/$name.log" 2>&1 &
    save_pid "$name" $!
    if _csp_start_gate "$name" "$bin" "$proto" "$port" "${CIMS_CSP_START_TIMEOUT:-20}"; then
        ok "$upper 시작 완료 (pid=$(read_pid "$name"), ${proto}:$port bound)"
    else
        err "$upper 시작 실패 — 리스너 ${proto}:$port 미개설/worker 소멸 (timeout ${CIMS_CSP_START_TIMEOUT:-20}s)"
        tail -3 "$LOG_DIR/$name.log" | sed 's/^/  /'
        local _vlog; _vlog=$(ls -t "$DIST_DIR/$name/log/${name}_"*.log 2>/dev/null | head -1)
        [[ -n "$_vlog" ]] && tail -3 "$_vlog" | sed 's/^/  /'
        return 1
    fi
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
    ensure_node_cert csc          # 기동 전 TLS 인증서 보증 (cert.sh)
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
    except: p=4421
print(p)" 2>/dev/null || echo 4421)
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
# OAM(4419) = Agent / HA / 배포 / 검증 책임. CSC(4421 admin + 4430 mcptt) 와 별개 프로세스.
# ══════════════════════════════════════════════════════════════════════════
#  관리평면 설정 자가 복구 — "설정 하나로 콘솔을 잃을 수 없다"
#
#  OAM 은 자기 자신이 복구 통로다. 잘못된 설정으로 기동에 실패하면 그것을 되돌릴 화면이
#  같이 사라져 SSH 없이는 복구가 불가능해진다. 이번 라운드에서 그 형태의 사고가 반복됐다
#  (store 경로 오지정 / 마운트 아닌 경로 / 잘못된 포트·주소). 개별 설정마다 가드를 붙이는
#  방식은 새 설정 키가 생길 때마다 다시 뚫린다 — 그래서 **설정과 무관한 한 곳**에서 막는다:
#
#    기동 성공(/health 200) → 그 설정을 config.json.last-good 으로 승격
#    기동 실패             → last-good 으로 되돌려 1회 재기동
#                            성공하면 콘솔이 살아나 운영자가 설정을 고칠 수 있다
#
#  last-good 은 **성공한 설정만** 담는다(실패 설정이 승격되지 않는다). 되돌린 사실은
#  로그와 마커 파일(config.json.rolled-back)로 남겨 콘솔에서 원인을 볼 수 있게 한다.
# ══════════════════════════════════════════════════════════════════════════
_oam_overlay_path() {              # agent 가 쓰는 배포 overlay (job_update_config 대상)
    local p
    for p in "$DIST_DIR/oam/config.json" "$DIST_DIR/config.json"; do
        [[ -f "$p" ]] && { echo "$p"; return 0; }
    done
    echo ""
}

_oam_promote_last_good() {
    local cfg; cfg="$(_oam_overlay_path)"
    [[ -n "$cfg" ]] || return 0
    # 되돌림 이력은 **새 설정이 성공했을 때만** 해소한다. 되돌린 직후의 재기동은 현재 설정이
    # last-good 과 같으므로 마커를 남겨야 한다 — 안 그러면 그 재기동이 마커를 지워
    # 콘솔 배너가 뜨지 않고, 운영자는 자기 설정이 적용된 줄 안다(실측: 이관 실패가 조용히
    # 되돌려져 그룹은 /NAS, 배포설정은 로컬인 채로 남았다).
    if [[ ! -f "$cfg.last-good" ]] || ! cmp -s "$cfg" "$cfg.last-good"; then
        rm -f "$cfg.rolled-back" 2>/dev/null || true
    fi
    cp -a "$cfg" "$cfg.last-good" 2>/dev/null || true
}

_oam_rollback_last_good() {        # 0=되돌림, 1=되돌릴 것 없음
    local cfg; cfg="$(_oam_overlay_path)"
    [[ -n "$cfg" && -f "$cfg.last-good" ]] || return 1
    if cmp -s "$cfg" "$cfg.last-good"; then
        return 1                   # 이미 last-good — 설정 탓이 아니다
    fi
    cp -a "$cfg" "$cfg.failed-$(date +%Y%m%d-%H%M%S)" 2>/dev/null || true
    cp -a "$cfg.last-good" "$cfg" 2>/dev/null || return 1
    date -Is > "$cfg.rolled-back" 2>/dev/null || true
    return 0
}

start_oam() {
    if is_running oam; then warn "OAM 이미 실행 중 (pid=$(read_pid oam))"; return 0; fi
    [[ ! -f "$DIST_DIR/oam/src/oam_app.py" ]] && err "OAM 소스 없음 (make dist 실행 필요)" && return 1
    ensure_node_cert oam          # 기동 전 TLS 인증서 보증 (cert.sh)

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

# CimsRuntimeDir 는 **자동으로 바꾸지 않는다.**
#   옛 동작은 경로를 못 만들면 조용히 dist_dir/ext_mnt/runtime 으로 바꿨다. 그 결과
#   관리 데이터의 SoT 가 **버전 디렉터리 안**으로 옮겨져, 업그레이드가 그 디렉터리를
#   교체하면서 서버·그룹·배포 기록이 통째로 사라졌다(실측 사고). 게다가 이관 대상이
#   공유 마운트인 구성에서는 "마운트가 잠깐 없다" 는 이유로 store 가 로컬로 이동해
#   절체 시 빈 콘솔이 된다. 경로 문제는 OAM 이 판정하고(mount guard·폴백), 여기서는
#   접근 가능 여부만 알린다.
cur = c.get('CimsRuntimeDir', '')
if cur and not can_mkdir(cur):
    print(f'[warn] CimsRuntimeDir 접근 불가: {cur} '
          f'(마운트/권한 확인 필요 — 경로를 자동 변경하지 않습니다)', flush=True)

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
    # oam_base_service_split §8 — 역할 플래그.
    #   우선순위: env OAM_ROLE(개발 오버라이드) > 배포 설정 Server.Role > all(코드 기본).
    # 배포 설정이 정본인 이유: 옛 구현은 부트스트랩이 만든 systemd drop-in(env)에만 role 이
    # 있어서, drop-in 이 없는 노드에서 HA 승격으로 기동되면 role=all 로 떠 게이트웨이 프록시를
    # 아예 마운트하지 않았다(승격 직후 서비스 API 전면 장애). 배포 설정에 두면 어느 노드에서
    # 기동되든 같은 역할이 된다.
    local oam_role_cfg
    oam_role_cfg=$("$PYBIN" -c "
import json, os
candidates=['$DIST_DIR/oam/config.json', '$DIST_DIR/config.json']
r=None
for ov in candidates:
    if not os.path.isfile(ov): continue
    try:
        f=json.load(open(ov))
        if isinstance(f,dict):
            r=f.get('Server.Role') or (f.get('Server',{}) or {}).get('Role')
            if r: break
    except: pass
print(r or '')" 2>/dev/null || echo "")
    local oam_role="${OAM_ROLE:-${oam_role_cfg:-all}}"
    case "$oam_role" in
        base|all) ;;
        *) warn "알 수 없는 OAM 역할 '$oam_role' — all 로 기동"; oam_role="all" ;;
    esac
    info "OAM (Operation & Management REST API) 시작... (port=$oam_port, role=$oam_role)"
    cd "$DIST_DIR/oam/src"
    "$PYBIN" -u "$DIST_DIR/oam/src/oam_app.py" --role "$oam_role" >> "$LOG_DIR/oam.log" 2>&1 &
    save_pid oam $!
    # D1 (self-upgrade): sleep 1.5 단발 판정 대신 /health 200 까지 폴링(최대 T초).
    # Python OAM 콜드스타트(config+마이그레이션+cert+bind)는 1.5s 를 넘길 수 있어,
    # self-upgrade 시 agent 의 후속 report 가 "아직 안 뜬 신 OAM" 에 닿아 유실되던 문제 방지.
    if _oam_health_gate "$oam_port" "${CIMS_OAM_HEALTH_TIMEOUT:-20}"; then
        ok "OAM 시작 완료 (pid=$(read_pid oam), port=$oam_port, /health 200)"
        _oam_promote_last_good          # 이 설정은 정상 — 복구 기준으로 승격
        return 0
    fi

    err "OAM 시작 실패 — /health 미응답 (${CIMS_OAM_HEALTH_TIMEOUT:-20}s)"
    tail -5 "$LOG_DIR/oam.log" | sed 's/^/  /'

    # ── 자가 복구: 직전 정상 설정으로 1회 되돌려 재기동 ──────────────────
    if ! _oam_rollback_last_good; then
        err "되돌릴 직전 정상 설정이 없습니다 — 설정 문제가 아닐 수 있습니다(로그 확인)"
        return 1
    fi
    warn "설정을 직전 정상값으로 되돌리고 재기동합니다 — 콘솔이 살아나면 설정을 고치세요"
    kill_stray "$DIST_DIR/oam/src/oam_app.py" "$oam_port" tcp
    oam_port=$("$PYBIN" -c "
import json, sys
for p in ['$DIST_DIR/oam/config.json', '$DIST_DIR/config.json', '$DIST_DIR/oam/config/oam.json']:
    try:
        d = json.load(open(p))
    except Exception:
        continue
    v = d.get('Server.Port') or (d.get('Server') or {}).get('Port')
    if v: print(int(v)); sys.exit(0)
print(4419)" 2>/dev/null || echo 4419)
    cd "$DIST_DIR/oam/src"
    "$PYBIN" -u "$DIST_DIR/oam/src/oam_app.py" --role "$oam_role" >> "$LOG_DIR/oam.log" 2>&1 &
    save_pid oam $!
    if _oam_health_gate "$oam_port" "${CIMS_OAM_HEALTH_TIMEOUT:-20}"; then
        warn "OAM 이 **직전 정상 설정**으로 기동됐습니다 (방금 저장한 설정은 "
        warn "  $(_oam_overlay_path).failed-* 로 보관). 콘솔에서 설정을 고쳐 다시 적용하세요."
        return 0
    fi
    err "되돌린 설정으로도 기동 실패 — 설정 외의 문제입니다"
    tail -5 "$LOG_DIR/oam.log" | sed 's/^/  /'
    return 1
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
    ensure_node_cert oam-svc      # 기동 전 TLS 인증서 보증 (cert.sh)
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
    # Console 2형 (엔진은 dist 정적 서빙만 — vite dev 콘솔은 개발 프론트 './cims.sh tb start console'):
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

    if [[ -d "$DIST_DIR/console/dist" ]]; then
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
    if [[ -d "$DIST_DIR/phone/dist" ]]; then
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
    # vite dev 서버(소스 트리) 필요 — 개발 프론트 전용. 엔진(배포본 tarball)에는 소스가 없다.
    err "TB-Console 은 개발 프론트 전용 — './cims.sh tb start console' 사용 (vite dev, 소스 트리)"
    return 1
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
    except: p=4421
print(p)" 2>/dev/null || echo 4421)
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
        console)    echo "8080:tcp" ;;   # dist 정적 서빙 (vite dev 콘솔은 개발 프론트 전용)
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
    # local 주소는 udp/tcp 모두 $4 (State Recv-Q Send-Q Local Peer Process) — 정확 포트
    # 매치 (substring ~ 는 :15060 이 :150601 에도 걸린다).
    local line pid
    if [[ $proto == "udp" ]]; then
        line=$(ss -Hulnp 2>/dev/null | awk -v pt="$port" 'match($4,/:([0-9]+)$/,m) && m[1]==pt {print; exit}')
    else
        line=$(ss -Htlnp 2>/dev/null | awk -v pt="$port" 'match($4,/:([0-9]+)$/,m) && m[1]==pt {print; exit}')
    fi
    [[ -z $line ]] && return
    pid=$(echo "$line" | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
    # 소켓은 보이는데 소유 pid 미표기 = capability 바이너리 프로세스(ss -p 귀속 거부)
    # — root 위임으로 귀속 복원
    if [[ -z $pid ]]; then
        local priv; priv=$(_priv_bin)
        [[ -n "$priv" ]] && pid=$(sudo -n "$priv" port-owner "$proto" "$port" 2>/dev/null | awk '{print $1}' || true)
    fi
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
    # 설치 디렉터리($DIST_DIR/<svc>/)가 있는 모듈만 표시 — 개발서버 build/dist 에는
    # 변종(pmp/imp/psp/isp) 디렉터리가 없어 자동 숨김, 변종 배포본에선 자기 모듈만 표시.
    # cwrtc/phone 은 재설계 예정 — 기동/상태 대상에서 제외 (패키징은 유지).
    local _svc
    for _svc in cmp pmp imp cmdp csp psp isp oam csc console; do
        [[ -d "$DIST_DIR/$_svc" ]] || continue
        status_one "$_svc"
    done
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
# cwrtc/phone 은 재설계 예정 — all 기동/중지/상태 제외 (명시 지정 시에만 개별 동작)
COMPONENTS=(cmp cmdp csp oam csc console)

_start_one() {
    case "$1" in
        # cwrtc/phone 은 재설계 예정 — all 기동/상태 제외 (명시 기동만. stop all 은 잔존 정리 위해 유지)
        all)        start_cmp; start_cmdp; start_csp; sleep 0.5; start_oam; start_csc; start_console ;;
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
            # kill_stray = cmdline 패턴(절대경로 직접 실행), _kill_own_exe_strays =
            # /proc exe 대조(lifecycle 상대경로 기동) — 둘 다 봐야 자기 좀비가 안 남는다.
            [[ -f "$bin" ]] && { kill_stray "$bin" || true; _kill_own_exe_strays "$bin" || true; }
            ;;
        *) return 0 ;;
    esac
}
