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
    SRC_CONSOLE="$SCRIPT_DIR/ems/core/console"
    SRC_PHONE="$SCRIPT_DIR/cims-phone"
else
    DIST_DIR="$SCRIPT_DIR"
    SRC_CONSOLE=""
    SRC_PHONE=""
fi
LOG_DIR="$DIST_DIR/log"

mkdir -p "$LOG_DIR"

# ── 공용 라이브러리 (색상/로그, .cims 리더) ─────────────────────
source "$SCRIPT_DIR/scripts/lib/common.sh" || {
    echo "[ERROR] scripts/lib/common.sh 없음 — 레포/dist 트리 손상" >&2; exit 1; }

# ── 초기 설정 (init wizard) ────────────────────────────────────
# 새 개발서버 첫 진입 시 한 번 실행. local_ip / db_password 등 환경 의존값을
# .cims/server.local.json 에 저장 → 이후 configure.sh / verify 가 우선 read.
# CIMS_LOCAL_IP / CIMS_DB_PASSWORD 환경변수로 prompt skip 가능 (CI 친화).
cmd_init() {
    local cims_dir="$SCRIPT_DIR/.cims"
    local cfg_file="$cims_dir/server.local.json"
    local non_interactive=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --non-interactive|--ni) non_interactive=1; shift ;;
            -h|--help)
                cat <<EOF
$(basename "$0") init [--non-interactive]
  새 서버 초기 설정 wizard. local_ip + db_password 를 .cims/server.local.json 에 저장.
  환경변수: CIMS_LOCAL_IP, CIMS_DB_PASSWORD (지정 시 prompt skip)
  --non-interactive: tty 없는 환경에서 prompt 없이 자동값/기본값 사용
EOF
                return 0 ;;
            *) err "알 수 없는 옵션: $1"; return 1 ;;
        esac
    done

    header "=== CIMS 초기 설정 ==="

    # 기존 값 (재실행 시 default 로 표시) — 공용 리더 (scripts/lib/common.sh)
    local cur_local_ip="" cur_db_password=""
    if [[ -f $cfg_file ]]; then
        local _init_local_ip="" _init_db_password=""
        eval "$(cims_local_cfg_eval "$cfg_file")"
        cur_local_ip="$_init_local_ip"
        cur_db_password="$_init_db_password"
        info "기존 설정 발견: $cfg_file"
    fi

    # 자동 감지 — default route 의 src IP (인터페이스 이름 무관)
    local detected_ip=""
    detected_ip=$(ip route get 8.8.8.8 2>/dev/null | awk '{for (i=1;i<=NF;i++) if ($i=="src") {print $(i+1); exit}}' || true)
    [[ -z $detected_ip ]] && detected_ip=$(hostname -I 2>/dev/null | awk '{print $1}' || true)

    # local_ip 결정 — env > prompt > 기존값 > 자동감지
    local local_ip="${CIMS_LOCAL_IP:-}"
    if [[ -z $local_ip ]]; then
        local default_ip="${cur_local_ip:-${detected_ip:-}}"
        if (( non_interactive )) || [[ ! -t 0 ]]; then
            local_ip="$default_ip"
            if [[ -z $local_ip ]]; then
                err "local_ip 결정 불가 — 인터랙티브 모드 또는 CIMS_LOCAL_IP env 필요"
                return 1
            fi
            info "비대화 모드 — local_ip=$local_ip 자동 적용"
        else
            local prompt_label
            if [[ -n $default_ip ]]; then prompt_label="local_ip [기본: $default_ip]: ";
            else                          prompt_label="local_ip (자동 감지 실패, 직접 입력 필요): "; fi
            read -rp "$prompt_label" local_ip
            local_ip="${local_ip:-$default_ip}"
        fi
        if [[ -z $local_ip ]]; then
            err "local_ip 빈 값 — abort"
            return 1
        fi
    fi

    # db_password 결정 — env > prompt > 기존값 > "cims1234"
    local db_password="${CIMS_DB_PASSWORD:-}"
    if [[ -z $db_password ]]; then
        local pw_label
        if [[ -n $cur_db_password && $cur_db_password != "cims1234" ]]; then
            pw_label="(이전 설정값)"
        else
            pw_label="cims1234"
        fi
        if (( non_interactive )) || [[ ! -t 0 ]]; then
            db_password="${cur_db_password:-cims1234}"
            info "비대화 모드 — db_password 기본값 적용"
        else
            read -rp "db_password [기본: $pw_label]: " db_password
            db_password="${db_password:-${cur_db_password:-cims1234}}"
        fi
    fi
    [[ $db_password == "cims1234" ]] && \
        warn "기본 비밀번호 'cims1234' 사용 중 — 운영 환경 변경 권장"

    # 저장
    mkdir -p "$cims_dir"
    chmod 700 "$cims_dir" 2>/dev/null || true
    local ts; ts=$(date +%Y-%m-%dT%H:%M:%S%z)
    CFG_FILE="$cfg_file" LOCAL_IP_VAL="$local_ip" DB_PWD_VAL="$db_password" TS_VAL="$ts" \
        python3 <<'PY'
import json, os
cfg = os.environ["CFG_FILE"]
data = {
    "local_ip":      os.environ["LOCAL_IP_VAL"],
    "db_password":   os.environ["DB_PWD_VAL"],
    "_generated_by": "cims.sh init",
    "_generated_at": os.environ["TS_VAL"],
    "_version":      1,
}
with open(cfg, "w", encoding="utf-8") as f:
    json.dump(data, f, ensure_ascii=False, indent=2)
    f.write("\n")
os.chmod(cfg, 0o600)
PY

    ok "초기 설정 저장됨: $cfg_file"
    info "  local_ip: $local_ip"
    info "  db_password: $(echo "$db_password" | sed 's/./*/g')"
    echo ""
    info "다음 단계:"
    info "  ./cims.sh build                    # 빌드"
    info "  ./cims.sh configure                # 시험환경 (server.local.json 자동 read)"
    info "  ./cims.sh start                    # 기동 (agent/bin/cims-svc 위임)"
    echo ""
    info "환경변수 override (CI):"
    info "  CIMS_LOCAL_IP=192.168.1.10 CIMS_DB_PASSWORD=mypw \\"
    info "    ./cims.sh init --non-interactive"
}

# ── 빌드 ───────────────────────────────────────────────────────
cmd_build() {
    [[ -z "$SRC_CONSOLE" ]] && err "build 명령은 소스 트리에서만 실행 가능" && exit 1
    header "=== C++ 빌드 ==="
    # 3단계 중 1단계: 실제 빌드 + 배포시 필요한 파일을 build/dist 로 복사.
    # 시험환경 설정은 `cims.sh configure`, 패키지화는 `cims.sh pkg` 로 완전 분리.
    # 인자:
    #   -j N / -jN / N — 병렬 작업 수
    #   -v X.Y.Z       — 모든 컴포넌트의 pkg.json version 을 갱신 (이후 pkg --no-bump 가 그 버전 사용)
    local jobs=""
    local version=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -j)             shift; jobs="${1:-}"; shift ;;
            -j*)            jobs="${1#-j}"; shift ;;
            -v|--version)   version="$2"; shift 2 ;;
            [0-9]*)         jobs="$1"; shift ;;
            *)              err "알 수 없는 옵션: $1 (build 단계는 -j N / -v X.Y.Z 만)"; return 1 ;;
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

    header "=== Web UI 빌드 (cims-console, prod target) ==="
    cd "$SRC_CONSOLE"
    npm install --silent
    # VITE_CONSOLE_TARGET=prod — 배포본은 packaging 메뉴 숨김 (routes.tsx VISIBLE_SECTIONS)
    # 상세 출력은 로그로 (실패 시에만 tail 노출) — 콘솔엔 결과 요약만.
    if VITE_CONSOLE_TARGET=prod npm run build > "$LOG_DIR/console_build.log" 2>&1; then
        rm -rf "$DIST_DIR/console/dist"
        cp -r dist "$DIST_DIR/console/"
        ok "cims-console 빌드 완료 (prod target — packaging 메뉴 제외, log: $LOG_DIR/console_build.log)"
    else
        err "cims-console 빌드 실패 — $LOG_DIR/console_build.log"
        tail -10 "$LOG_DIR/console_build.log" | sed 's/^/  /'
        exit 1
    fi
    # TB-Console 은 dev 모드 기반 (vite proxy 필요) → 별도 dist 빌드 불필요.
    # configure.sh 가 .env.tb.local 에 VITE_ADMIN_TARGET=https://127.0.0.1:4419 를 기록하고,
    # cims.sh start tb-console 이 npm run dev -- --mode tb --port 3000 으로 기동한다 (VITE_CONSOLE_TARGET 미설정=dev).

    # cwrtc/cims-phone 은 재설계 예정 — 빌드/dist/패키징 제외 (CMakeLists.txt 동기).

    # -v 명시 시 source 의 pkg.json version 갱신 — 이후 cims.sh pkg --no-bump 가 그 버전 사용.
    # 변종 (psp/isp/pmp/imp) 은 자기 pkg.json 없음 → base (csp/cmp) 의 pkg.json 만 갱신해도 충분.
    if [[ -n $version ]]; then
        local _comp _pkgf
        for _comp in csp cmp csc cspsim agent ems/core/console; do
            _pkgf="$SCRIPT_DIR/$_comp/pkg.json"
            [[ -f $_pkgf ]] && _pkg_write_version "$_pkgf" "$version"
        done
        ok "pkg.json 버전 갱신 → $version (6개 컴포넌트)"
    fi

    echo ""
    ok "[1/3] build 완료 → $DIST_DIR${version:+ (v=$version)}"
    echo ""
    info "다음 단계:"
    info "  [2/3] ./cims.sh configure --local-ip <서버IP> [--db-password <PW>]   # 시험환경 설정"
    info "        ./cims.sh start                                                # Phase 1 기능 검증"
    info "  [3/3] ./cims.sh pkg ${version:+[--no-bump 자동]}                              # 배포 패키지화"
}

# ── configure ──────────────────────────────────────────────────
# 3단계 중 2단계: 배포 전 시험 환경 설정. 로컬 네트워크 IP / DB 접속정보 /
# 도메인 / 로그·녹취 경로 등 환경 의존값을 build/dist 의 설정 파일에 반영한다.
cmd_configure() {
    "$SCRIPT_DIR/configure.sh" "$@"
}

# ── up — 로컬 개발 파이프라인 합성 ──────────────────────────────
# build → configure -y → 전체 재시작. 재시작을 전체로 하는 이유:
# configure 재실행이 JWT 시크릿을 갱신하므로 발급(oam)/검증(csc/csp)이
# 함께 새 설정을 읽어야 한다 (부분 재시작 시 토큰 검증 401).
cmd_up() {
    [[ -z "$SRC_CONSOLE" ]] && err "up 명령은 소스 트리에서만 실행 가능" && exit 1
    local skip_build=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --skip-build) skip_build=1; shift ;;
            -h|--help)
                cat <<EOF
$(basename "$0") up [--skip-build]
  로컬 개발 파이프라인 원스톱: [1/3] build → [2/3] configure -y → 전체 재시작.
  --skip-build: 코드 변경 없이 설정만 재반영할 때
  (검증 게이트는 ./cims.sh verify, 개별 재시작은 ./cims.sh restart <svc>)
EOF
                return 0 ;;
            *) err "알 수 없는 옵션: $1 (up 은 --skip-build 만)"; return 1 ;;
        esac
    done
    header "=== CIMS up: build → configure → restart ==="
    if (( skip_build )); then
        info "[1/3] build 생략 (--skip-build)"
    else
        cmd_build
    fi
    cmd_configure -y
    "$SCRIPT_DIR/agent/bin/cims-svc" restart
    header "=== TB (개발 워크플로 — 4419/3000 상시) ==="
    cmd_tb status
    echo ""
    ok "up 완료 — 콘솔: https://<서버IP>:4419 (configure 재실행으로 시크릿 갱신 → 재로그인 필요)"
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
            --keep-deployed)  keep_deployed=1; shift ;;   # mgmt-server/ 등 배포본 보존
            -*) err "알 수 없는 reset 옵션: $1"; return 1 ;;
            *)  shift ;;
        esac
    done

    header "=== 검증 환경 초기화 (가입자 보존, TB 유지) ==="
    info "TB 2종(TB-CSC 4419 / TB-Console 3000) 은 건드리지 않음."
    [[ $keep_processes -eq 1 ]] && info "--keep-processes: Test-* 프로세스 유지 (서비스 정지/포트 kill 건너뜀)"

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
            # reset 은 검증 대상만 정리 — TB 포트(4419/3000) 는 제외해 상시 동작 보장
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
            info "Phase 2/3 배포 대상 정리 — SKIP (--keep-deployed, mgmt-server + service-server 보존)"
        else
            info "Phase 2/3 배포 대상 정리 (build/dist/{mgmt,volte-sip,volte-media,ptt-sip,ptt-media,ibcf-sip,ibcf-media}-server/, §0.10)..."
            # Test-agent 프로세스부터 종료 (파일 잠금 회피)
            # _INSTANCES 의 agent_name 과 동기 — _INSTANCES 에 추가/제거 시 양쪽 갱신 필요.
            # P2 (2026-05-11): ISP/IMP 는 volte-sip-server / volte-media-server 와 한 agent 공유.
            local _agents=(mgmt-server
                           volte-sip-server volte-media-server
                           ptt-sip-server ptt-media-server)
            local _a
            for _a in "${_agents[@]}"; do
                pkill -f "cims_agent.py.*--name $_a" 2>/dev/null || true
            done
            # 배포본 서비스 프로세스 (csc_app.py, console serve, csp/cmp 바이너리) 도 종료
            # — 4445/4430/8081/5060/9000 등 포트 잠금 해제 (Phase 1 검증 시 mcptt 4430 충돌 방지)
            # .prev 디렉토리에서 동작 중인 stale process 도 함께 종료 (race 회피).
            # 네이티브 바이너리 (csp/cmp 등) 의 cmdline 은 'bin/csp config/csp.json' 같이
            # 상대경로 — pkill -f 는 cmdline 매칭이라 못 잡음. /proc/PID/exe 와 /proc/PID/cwd
            # 가 install 경로 하위인 pid 를 직접 enumerate 하여 kill.
            # exe/cwd 가 '(deleted)' suffix 를 가진 stale process 도 잡아야 하므로
            # install dir 존재 여부 가드 없이 항상 enumerate. 동일 path prefix 면 kill.
            _enum_install_pids() {
                local prefix="$1"   # 예: /home/nex/work/cims/build/dist/volte-sip-server
                local pid exe cwd
                for pid in /proc/[0-9]*; do
                    exe=$(readlink "$pid/exe" 2>/dev/null || true)
                    cwd=$(readlink "$pid/cwd" 2>/dev/null || true)
                    # '(deleted)' suffix strip — tarball/rm 후 inode 교체된 케이스
                    exe="${exe% (deleted)}"
                    cwd="${cwd% (deleted)}"
                    if [[ "$exe" == "$prefix/"* || "$cwd" == "$prefix"* ]]; then
                        basename "$pid"
                    fi
                done | sort -u
            }
            local _a_path _pids
            for _a in "${_agents[@]}"; do
                pkill -f "$DIST_DIR/$_a/" 2>/dev/null || true
                pkill -f "$DIST_DIR/$_a\\.prev/" 2>/dev/null || true
                for _a_path in "$DIST_DIR/$_a" "$DIST_DIR/$_a.prev"; do
                    _pids=$(_enum_install_pids "$_a_path")
                    [[ -n $_pids ]] && kill $_pids 2>/dev/null || true
                done
            done
            sleep 1
            for _a in "${_agents[@]}"; do
                for _a_path in "$DIST_DIR/$_a" "$DIST_DIR/$_a.prev"; do
                    _pids=$(_enum_install_pids "$_a_path")
                    [[ -n $_pids ]] && kill -9 $_pids 2>/dev/null || true
                done
            done
            sleep 0.3
            unset -f _enum_install_pids
            # 디렉토리 + .prev 둘 다 삭제 — 다음 deploy 가 stale .prev 와 race 안 하도록.
            for _a in "${_agents[@]}"; do
                [[ -d "$DIST_DIR/$_a" ]]      && rm -rf "$DIST_DIR/$_a"
                [[ -d "$DIST_DIR/$_a.prev" ]] && rm -rf "$DIST_DIR/$_a.prev"
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
# cims_agent 는 전부 삭제 (옛 TB-agent 보존 로직 제거 — 2026-05-11 라운드).

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
# cims_agent: 전체 TRUNCATE (TB-agent 제거 후 매 reset 마다 fresh enroll).
if 'cims_agent' in existing:
    cur.execute("TRUNCATE TABLE cims_agent")
    done.append("cims_agent")
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
    # TB 포트(4419/3000) 는 반대로 "점유" 되어 있어야 정상 (TB 상시 동작 전제).
    # cwrtc(8443)/phone(3002) 은 재설계 예정 — 점검 제외 (reset 의 잔존 정리에는 유지).
    local target_ports=("5060:udp" "5061:tcp" "9000:udp" "9001:udp" "4420:tcp" "4421:tcp" "3001:tcp" "8080:tcp")
    local tb_ports=("4419:tcp:TB-CSC" "3000:tcp:TB-Console")
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
    info "[TB] 상시 기동 중이어야 정상"
    for pp in "${tb_ports[@]}"; do
        port="${pp%%:*}"; label="${pp##*:}"; proto="$(echo "$pp" | cut -d: -f2)"
        line=$(ss -Htlnp 2>/dev/null | awk -v p=":$port" '$4 ~ p {print; exit}' || true)
        if [[ -n $line ]]; then
            pid=$(echo "$line" | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2 || true)
            ok "$label (port $port/tcp) 동작 중 (pid=${pid:-?})"
        else
            warn "$label (port $port/tcp) 미동작 — 'cims.sh tb start' 필요"
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
            warn "$label (port $port/tcp) 잔존 (pid=${pid:-?}) — './cims.sh reset' 권장"
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

# ── 통합 status — 모듈(cims-svc) + TB(4419/3000) 한 화면 ──────
cmd_status_front() {
    local full=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --full) full=1; shift ;;
            -h|--help)
                cat <<EOF
$(basename "$0") status [--full]
  전체 모듈 상태(agent/bin/cims-svc status) + TB 2종(4419/3000)을 한 화면에 출력.
  --full: preflight (포트 점유 / git / DB 사전조건) 요약까지 포함.
EOF
                return 0 ;;
            *) err "알 수 없는 옵션: $1 (status 는 --full 만)"; return 1 ;;
        esac
    done
    "$SCRIPT_DIR/agent/bin/cims-svc" status
    header "=== TB (개발 워크플로 — 4419/3000 상시) ==="
    cmd_tb status
    echo ""
    if (( full )); then cmd_preflight; fi
    return 0
}

# ── 검증 (S1~S6) — ./cims-verify 전용 진입점으로 이전 ──────────
# 본체는 verify/lib (tests/cims_verify.py CLI). dispatch 의 안내 스텁 참조.



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
cur=c.cursor(); cur.execute('SELECT mcptt_group_id FROM ptt_groups ORDER BY mcptt_group_id LIMIT 1')
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

# ── 도움말 ─────────────────────────────────────────────────────
usage() {
    cat <<EOF
${BOLD}CIMS 개발 서버 통합 진입점${NC} — 빌드 → 설정 → 기동 → 확인 → 검증 → 패키징

사용법: $(basename "$0") <command> [options]

역할 분담 (cims.sh 는 개발 프론트 — 본체는 각 정본에 위임):
  운영 엔진  agent/bin/cims-svc   배포본·agent·OAM 이 직접 호출하는 lifecycle 정본
  검증       ./cims-verify        S1~S6 게이트 (소스 트리 전용)
  HA         agent/bin/cims-ha    keepalived + systemd (배포 전용)

${BOLD}[0] 초기 설정 (새 서버 첫 진입 시 한 번):${NC}
  init [--non-interactive]
                       local_ip / db_password 를 .cims/server.local.json 에 저장.
                       env: CIMS_LOCAL_IP, CIMS_DB_PASSWORD

${BOLD}[1/3] 빌드:${NC}
  build [-j N] [-v X.Y.Z]
                       C++ + Web UI 빌드 → build/dist 복사만 수행.
                       환경값 반영 없음 (configure 단계 책임). -v 는 pkg.json 버전 갱신.

${BOLD}[2/3] 시험환경 설정:${NC}
  configure [options]  로컬 네트워크 IP / DB / 도메인 / 로그경로를 build/dist 의
                       설정 파일에 반영 → configure.sh 에 위임.
                       옵션 없이 TTY 에서 실행하면 항목별 기본값 제시형 대화형
                       wizard (Enter=수락, 답변은 .cims/server.local.json 에 저장).
                       -y/--defaults: 대화형 없이 저장값/기본값으로 즉시 진행.

${BOLD}기동/상태/로그 (→ agent/bin/cims-svc 위임):${NC}
  start|stop|restart [svc]
                       svc: all(기본) | cmp|cmdp|csp|oam|csc|console
                       (변종 pmp|imp|psp|isp 는 배포본 전용, cwrtc|phone 은 재설계 예정 — 제외)
  status [--full]      전체 모듈 + TB 상태 (--full: preflight 사전조건 요약 포함)
  log <svc>            로그 tail -f

  up [--skip-build]    원스톱: build → configure -y → 전체 재시작.
                       코드/설정 변경을 로컬 서버에 한 번에 반영할 때 사용.
  sync [targets]       C++ 빌드 없이 Python/스크립트/메타만 dist 로 복사
                       targets: csc | agent | scripts | pkg-meta | console | all
                       (기본: all — C++ 제외. 예: ./cims.sh sync csc && ./cims.sh restart csc)

${BOLD}검증 (docs/VERIFICATION_PROCESS.md — 콘솔 진입: /testbed/verify-v2):${NC}
  verify <cmd>         S1~S6 게이트 (→ ./cims-verify 위임, 소스 트리 전용)
                       stage1~6 | run --preset <NAME> | list | list-presets | describe
  preflight            사전조건 확인 (ens160 IP, 포트 점유, git 상태, DB 연결)
  reset  [--all|--files|--db] [--path <dir>] [--keep-processes] [--keep-deployed]
                       가입자 테이블 보존 상태로 설정/배포/세션 DB + 파일 + 프로세스 초기화
                       (보존: users, organizations, volte_subscriptions,
                        ptt_subscriptions, ptt_groups, ptt_group_members, user_rejects)

${BOLD}[3/3] 배포 패키지 (Console 업로드용):${NC}
  pkg [-v X.Y.Z] [--no-bump] [--no-sync] [-m <changelog>] [name...]
                       configure 완료된 build/dist 를 모듈별 tar.gz 로 패키징.
                       각 tarball 최상위: meta.json (name/version/설명) +
                       config_template.json (설정 스키마) + <모듈>/ 파일.
                       기본: auto-bump patch + source→dist auto-sync.
                       -v 지정 시 해당 버전 강제 + pkg.json 반영
                       --no-bump 면 현재 pkg.json 버전 그대로 (재패키징)
                       --no-sync 면 auto-sync 건너뜀 (옛 dist 그대로 — 디버깅용)
                       C++ 바이너리 (csp/cmp/cspsim) 는 mtime 검사 후 warn 만.

${BOLD}TB 2종 (개발 워크플로 — 4419/3000 상시 동작):${NC}
  tb start|stop|restart|status [oam|csc|console|all]   기본: all (자세히: tb help)
                       TB-OAM     https://127.0.0.1:4419 (oam-tb.json)
                       TB-Console http://127.0.0.1:3000 (vite dev, 소스 트리만)

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

${BOLD}예시 — 개발 루프:${NC}
  $(basename "$0") build && $(basename "$0") configure -y && $(basename "$0") start
  $(basename "$0") up                              # 위 3단계 원스톱
  $(basename "$0") status                          # 모듈 상태 확인
  $(basename "$0") log csp                         # 로그 추적
  $(basename "$0") verify stage1                   # 정적 검사 게이트
  $(basename "$0") pkg                             # 배포 tarball (auto-bump)
  $(basename "$0") pkg -v 1.2.0 csp                # csp 만 1.2.0 강제

  # 배포 서버에서 (dist/ 내부)
  ./cims.sh configure --local-ip 192.168.1.10
  ./cims.sh start

  # 시뮬레이터 (동시 실행)
  $(basename "$0") sim -mode ptt -group +82571910001      # 영상 PTT 포그라운드
  $(basename "$0") sim -mode volte --bg                   # VoLTE 동시 실행
EOF
}


# ── 동기화/패키징 — scripts/{sync,package}.sh 로 분리 ──────────
# 본체는 scripts/ (소스 트리 전용). CLI 계약은 여기(cims.sh sync|pkg|installer)가 유지.
cmd_sync()      { "$SCRIPT_DIR/scripts/sync.sh" "$@"; }
cmd_pkg()       { "$SCRIPT_DIR/scripts/package.sh" pkg "$@"; }
cmd_installer() { "$SCRIPT_DIR/scripts/package.sh" installer "$@"; }

# ── TB-CSC / TB-Console 운영 (개발 워크플로용 4419/3000 상시 동작) ──
# 운영 daemon 은 agent/bin/cims-svc, TB 2종은 여기. dist 트리에서는 csc 만 가능 (console=npm dev 서버).
cmd_tb() {
    local action="${1:-status}"
    [[ $# -ge 1 ]] && shift
    local target="${1:-all}"

    case "$action" in
        start|stop|restart|status) ;;
        help|--help|-h)
            cat <<EOF
$(basename "$0") tb <action> [target]
  action: start | stop | restart | status
  target: oam | csc | console | all  (기본: all = oam + console)

  TB-OAM     — https://127.0.0.1:4419 (oam_app.py + oam-tb.json)   ← Phase 3 기본
  TB-CSC     — https://127.0.0.1:4419 (csc_app.py + csc-tb.json)   ← deprecated, OAM 분리 전 호환
  TB-Console — http://127.0.0.1:3000  (vite dev 서버 — 소스 트리에서만)

  TB-OAM 와 TB-CSC 둘 다 4419 port — 동시 기동 불가. 'tb stop csc && tb start oam' 순서.

  예:
    cims.sh tb status              # 4419 / 3000 확인
    cims.sh tb start oam           # TB-OAM 만 기동
    cims.sh tb restart             # all 재기동 (oam + console)
    cims.sh tb stop console        # TB-Console 만 정지
EOF
            return 0 ;;
        *) err "알 수 없는 tb 동작: $action (start|stop|restart|status|help)"; return 1 ;;
    esac
    case "$target" in
        oam|csc|console|all) ;;
        *) err "알 수 없는 tb 대상: $target (oam|csc|console|all)"; return 1 ;;
    esac

    _tb_port_pid() {
        local port="$1"
        ss -Htlnp 2>/dev/null \
            | awk -v p=":$port" '$4 ~ p {print; exit}' \
            | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2 || true
    }

    _tb_csc_start() {
        local pid; pid=$(_tb_port_pid 4419)
        if [[ -n $pid ]]; then ok "TB-CSC 이미 동작 (pid=$pid, port 4419)"; return 0; fi
        local cfg="$DIST_DIR/csc/config/csc-tb.json"
        local app="$DIST_DIR/csc/src/csc_app.py"
        [[ ! -f $cfg ]] && { err "csc-tb.json 없음: $cfg ('cims.sh build' 후 시도)"; return 1; }
        [[ ! -f $app ]] && { err "csc_app.py 없음: $app ('cims.sh build' 후 시도)"; return 1; }
        warn "TB-CSC 는 deprecated — OAM 분리 후에는 TB-OAM 사용 ('cims.sh tb start oam')"
        info "TB-CSC 기동..."
        ( cd "$DIST_DIR/csc/src" && \
          CIMS_CSC_CONFIG="$cfg" nohup python3 csc_app.py \
              > "$LOG_DIR/tb-csc.log" 2>&1 & echo $! > "$LOG_DIR/tb-csc.pid" )
        sleep 2
        pid=$(_tb_port_pid 4419)
        if [[ -n $pid ]]; then
            ok "TB-CSC LISTEN https://127.0.0.1:4419 (pid=$pid) — log: $LOG_DIR/tb-csc.log"
        else
            err "TB-CSC 기동 실패 — $LOG_DIR/tb-csc.log 확인"; return 1
        fi
    }

    _tb_csc_stop() {
        local pid; pid=$(_tb_port_pid 4419)
        if [[ -z $pid ]]; then ok "TB-CSC 이미 정지 (port 4419 가용)"; rm -f "$LOG_DIR/tb-csc.pid"; return 0; fi
        info "TB-CSC 정지 (pid=$pid)..."
        kill "$pid" 2>/dev/null || true
        sleep 1
        if kill -0 "$pid" 2>/dev/null; then kill -9 "$pid" 2>/dev/null || true; sleep 1; fi
        rm -f "$LOG_DIR/tb-csc.pid"
        ok "TB-CSC 정지"
    }

    _tb_oam_start() {
        local pid; pid=$(_tb_port_pid 4419)
        if [[ -n $pid ]]; then
            # 4419 점유 중 — TB-CSC 일 수도 있음. exe path 로 분기.
            local exe; exe=$(readlink -f /proc/$pid/exe 2>/dev/null || true)
            local cmd; cmd=$(tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null || true)
            if [[ "$cmd" == *"oam_app.py"* ]]; then
                ok "TB-OAM 이미 동작 (pid=$pid, port 4419)"; return 0
            else
                err "port 4419 이 다른 프로세스에 점유 중 (pid=$pid, cmd='$cmd') — 'cims.sh tb stop csc' 먼저"; return 1
            fi
        fi
        local cfg="$DIST_DIR/oam/config/oam-tb.json"
        local app="$DIST_DIR/oam/src/oam_app.py"
        [[ ! -f $cfg ]] && { err "oam-tb.json 없음: $cfg ('cims.sh sync csc' 후 시도)"; return 1; }
        [[ ! -f $app ]] && { err "oam_app.py 없음: $app ('cims.sh sync csc' 후 시도)"; return 1; }
        info "TB-OAM 기동..."
        ( cd "$DIST_DIR/oam/src" && \
          CIMS_OAM_CONFIG="$cfg" nohup python3 oam_app.py \
              > "$LOG_DIR/tb-oam.log" 2>&1 & echo $! > "$LOG_DIR/tb-oam.pid" )
        sleep 2
        pid=$(_tb_port_pid 4419)
        if [[ -n $pid ]]; then
            ok "TB-OAM LISTEN https://127.0.0.1:4419 (pid=$pid) — log: $LOG_DIR/tb-oam.log"
        else
            err "TB-OAM 기동 실패 — $LOG_DIR/tb-oam.log 확인"; return 1
        fi
    }

    _tb_oam_stop() {
        local pid; pid=$(_tb_port_pid 4419)
        if [[ -z $pid ]]; then ok "TB-OAM 이미 정지 (port 4419 가용)"; rm -f "$LOG_DIR/tb-oam.pid"; return 0; fi
        # cmdline 으로 oam_app.py 인지 확인 — csc 면 stop 거부
        local cmd; cmd=$(tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null || true)
        if [[ "$cmd" != *"oam_app.py"* ]]; then
            warn "port 4419 pid=$pid 는 oam_app.py 가 아님 (cmd='$cmd') — 'cims.sh tb stop csc' 사용"; return 0
        fi
        info "TB-OAM 정지 (pid=$pid)..."
        kill "$pid" 2>/dev/null || true
        sleep 1
        if kill -0 "$pid" 2>/dev/null; then kill -9 "$pid" 2>/dev/null || true; sleep 1; fi
        rm -f "$LOG_DIR/tb-oam.pid"
        ok "TB-OAM 정지"
    }

    _tb_console_start() {
        if [[ -z "$SRC_CONSOLE" ]]; then
            warn "TB-Console 은 소스 트리에서만 기동 가능 (dist 트리에는 npm dev 서버 없음)"
            return 0
        fi
        local pid; pid=$(_tb_port_pid 3000)
        if [[ -n $pid ]]; then ok "TB-Console 이미 동작 (pid=$pid, port 3000)"; return 0; fi
        if [[ ! -d "$SRC_CONSOLE/node_modules" ]]; then
            warn "$SRC_CONSOLE/node_modules 없음 — 'npm install' 먼저 필요"; return 1
        fi
        info "TB-Console 기동 (npm run dev --mode tb --port 3000)..."
        ( cd "$SRC_CONSOLE" && \
          nohup npm run dev -- --mode tb --port 3000 \
              > "$LOG_DIR/tb-console.log" 2>&1 & echo $! > "$LOG_DIR/tb-console.pid" )
        # vite 가 listen 까지 첫 빌드 시 수초 — 최대 15초 polling
        local pid_n=""; local i
        for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
            sleep 1
            pid_n=$(_tb_port_pid 3000)
            [[ -n $pid_n ]] && break
        done
        if [[ -n $pid_n ]]; then
            ok "TB-Console LISTEN http://127.0.0.1:3000 (pid=$pid_n) — log: $LOG_DIR/tb-console.log"
        else
            err "TB-Console 기동 실패 (15s timeout) — $LOG_DIR/tb-console.log 확인"; return 1
        fi
    }

    _tb_console_stop() {
        local npm_pid=""
        [[ -f "$LOG_DIR/tb-console.pid" ]] && npm_pid=$(cat "$LOG_DIR/tb-console.pid" 2>/dev/null || true)
        local vite_pid; vite_pid=$(_tb_port_pid 3000)
        if [[ -z $npm_pid && -z $vite_pid ]]; then
            ok "TB-Console 이미 정지 (port 3000 가용)"
            rm -f "$LOG_DIR/tb-console.pid"; return 0
        fi
        info "TB-Console 정지 (npm pid=${npm_pid:-?}, vite pid=${vite_pid:-?})..."
        [[ -n $npm_pid ]] && pkill -P "$npm_pid" 2>/dev/null || true
        [[ -n $npm_pid ]] && kill "$npm_pid" 2>/dev/null || true
        [[ -n $vite_pid ]] && kill "$vite_pid" 2>/dev/null || true
        sleep 1
        local still; still=$(_tb_port_pid 3000)
        if [[ -n $still ]]; then
            kill -9 "$still" 2>/dev/null || true
            sleep 1
        fi
        rm -f "$LOG_DIR/tb-console.pid"
        ok "TB-Console 정지"
    }

    _tb_status() {
        local pid
        pid=$(_tb_port_pid 4419)
        if [[ -n $pid ]]; then
            local cmd; cmd=$(tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null || true)
            if [[ "$cmd" == *"oam_app.py"* ]]; then
                ok "TB-OAM     pid=$pid  https://127.0.0.1:4419"
            elif [[ "$cmd" == *"csc_app.py"* ]]; then
                ok "TB-CSC     pid=$pid  https://127.0.0.1:4419 (deprecated — use TB-OAM)"
            else
                ok "TB(4419)   pid=$pid  cmd=$cmd"
            fi
        else
            warn "TB-OAM     미동작  — 'cims.sh tb start oam'"
        fi
        pid=$(_tb_port_pid 3000)
        if [[ -n $pid ]]; then ok "TB-Console pid=$pid  http://127.0.0.1:3000"
        else warn "TB-Console 미동작  — 'cims.sh tb start console'"; fi
    }

    case "$action" in
        start)
            [[ $target == oam     || $target == all ]] && _tb_oam_start
            [[ $target == csc                       ]] && _tb_csc_start
            [[ $target == console || $target == all ]] && _tb_console_start
            ;;
        stop)
            [[ $target == oam     || $target == all ]] && _tb_oam_stop
            [[ $target == csc                       ]] && _tb_csc_stop
            [[ $target == console || $target == all ]] && _tb_console_stop
            ;;
        restart)
            [[ $target == oam     || $target == all ]] && { _tb_oam_stop; _tb_oam_start; }
            [[ $target == csc                       ]] && { _tb_csc_stop; _tb_csc_start; }
            [[ $target == console || $target == all ]] && { _tb_console_stop; _tb_console_start; }
            ;;
        status)
            _tb_status
            ;;
    esac
}

case "${1:-}" in
    init)      shift; cmd_init "$@" ;;
    build)     shift; cmd_build "$@" ;;
    configure) shift; cmd_configure "$@" ;;
    up)        shift; cmd_up "$@" ;;
    sim)       shift; cmd_sim "$@" ;;
    clean)     shift; cmd_clean "${1:-all}" ;;
    reset)     shift; cmd_reset "$@" ;;
    preflight) cmd_preflight ;;
    # 검증(S1~S6) — 정본은 ./cims-verify (소스 트리 전용). 여기는 개발 프론트 위임.
    verify)
        shift
        if [[ -x "$SCRIPT_DIR/cims-verify" ]]; then
            exec "$SCRIPT_DIR/cims-verify" "$@"
        fi
        err "검증(cims-verify)은 소스 트리 전용 — dist 트리에서는 사용 불가"; exit 2 ;;
    pkg)       shift; cmd_pkg "$@" ;;
    installer) shift; cmd_installer "$@" ;;
    sync)      shift; cmd_sync "$@" ;;
    tb)        shift; cmd_tb "$@" ;;
    # 운영 lifecycle — 정본(엔진)은 agent/bin/cims-svc (agent/OAM/verify 가 직접 호출).
    # 개발 서버 UX 를 위해 여기서 passthrough 위임 (TB 는 'cims.sh tb' 별도).
    # status 만 통합 뷰 (모듈 + TB [+ --full: preflight]).
    status)    shift; cmd_status_front "$@" ;;
    start|stop|restart|log)
        exec "$SCRIPT_DIR/agent/bin/cims-svc" "$@" ;;
    ha)
        err "ha 명령은 agent/bin/cims-ha 로 이전됨"
        err "  사용: $(dirname "${BASH_SOURCE[0]}")/agent/bin/cims-ha ${2:-help}"; exit 2 ;;
    help|--help|-h) usage ;;
    "") usage ;;
    *) err "알 수 없는 명령: $1"; echo ""; usage; exit 1 ;;
esac
