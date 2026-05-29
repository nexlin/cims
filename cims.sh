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
LOG_DIR="$DIST_DIR/log"

mkdir -p "$LOG_DIR"

# ── 색상 ───────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()    { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()      { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()     { echo -e "${RED}[ERROR]${NC} $*" >&2; }
header()  { echo -e "\n${BOLD}$*${NC}"; }

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

    # 기존 값 (재실행 시 default 로 표시)
    local cur_local_ip="" cur_db_password=""
    if [[ -f $cfg_file ]]; then
        cur_local_ip=$(CFG="$cfg_file" python3 -c \
            'import json,os; print(json.load(open(os.environ["CFG"])).get("local_ip",""))' \
            2>/dev/null || true)
        cur_db_password=$(CFG="$cfg_file" python3 -c \
            'import json,os; print(json.load(open(os.environ["CFG"])).get("db_password",""))' \
            2>/dev/null || true)
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
    info "  ./cims.sh start                    # 기동"
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
    VITE_CONSOLE_TARGET=prod npm run build
    cp -r dist "$DIST_DIR/console/"
    ok "cims-console 빌드 완료 (prod target — packaging 메뉴 제외)"
    # TB-Console 은 dev 모드 기반 (vite proxy 필요) → 별도 dist 빌드 불필요.
    # configure.sh 가 .env.tb.local 에 VITE_ADMIN_TARGET=https://127.0.0.1:4419 를 기록하고,
    # cims.sh start tb-console 이 npm run dev -- --mode tb --port 3000 으로 기동한다 (VITE_CONSOLE_TARGET 미설정=dev).

    header "=== Web UI 빌드 (cims-phone) ==="
    cd "$SRC_PHONE"
    npm install --silent
    npm run build
    cp -r dist "$DIST_DIR/phone/"
    ok "cims-phone 빌드 완료"

    # -v 명시 시 source 의 pkg.json version 갱신 — 이후 cims.sh pkg --no-bump 가 그 버전 사용.
    # 변종 (psp/isp/pmp/imp) 은 자기 pkg.json 없음 → base (csp/cmp) 의 pkg.json 만 갱신해도 충분.
    if [[ -n $version ]]; then
        local _comp _pkgf
        for _comp in csp cmp csc cwrtc cspsim agent cims-console cims-phone; do
            _pkgf="$SCRIPT_DIR/$_comp/pkg.json"
            [[ -f $_pkgf ]] && _pkg_write_version "$_pkgf" "$version"
        done
        ok "pkg.json 버전 갱신 → $version (8개 컴포넌트)"
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
    local target_ports=("5060:udp" "5061:tcp" "9000:udp" "9001:udp" "4420:tcp" "4421:tcp" "3001:tcp" "3002:tcp" "8080:tcp" "8443:tcp")
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
        list|describe|run|list-presets|purge-runs|delete-run)
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

# ── 도움말 ─────────────────────────────────────────────────────
usage() {
    cat <<EOF
${BOLD}CIMS 통합 관리 스크립트${NC}

사용법: $(basename "$0") <command> [options]

${BOLD}서비스 명령 (운영 도구는 agent/bin 으로 분리됨):${NC}
  agent/bin/cims-svc  start|stop|restart|status|log [svc]    — lifecycle
  agent/bin/cims-ha   install|config|check|apply|start|stop  — HA (keepalived + systemd)
  agent/bin/cims-health <svc>   — listen probe (keepalived 가 호출)
  agent/bin/cims-notify <svc> ...  — state hook (keepalived 가 호출)
  cims.sh 는 개발 단계 명령만 (build/configure/pkg/sim/verify/sync 등).

${BOLD}TB 2종 (개발 워크플로 — 4419/3000 상시 동작):${NC}
  tb start|stop|restart [csc|console|all]   기본: all
  tb status                                 4419/3000 점유 확인
  tb help                                   자세한 설명
                                  TB-CSC     https://127.0.0.1:4419 (csc-tb.json)
                                  TB-Console http://127.0.0.1:3000 (vite dev, 소스 트리만)

${BOLD}초기 설정 (새 서버 첫 진입 시 한 번):${NC}
  init [--non-interactive]
                       local_ip / db_password 를 .cims/server.local.json 에 저장.
                       env: CIMS_LOCAL_IP, CIMS_DB_PASSWORD

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
                       (기본: all — C++ 제외. 예: ./cims.sh sync csc → agent/bin/cims-svc restart csc)

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
                        Phase 1 서버 모듈 중지 → 4 service-server Test-agent enroll
                        → service-server tarball 업로드 → deployment 생성 →
                        install job 폴링 → 설치 파일 검증 → verify_reports/<ts>_phase3.md
                        (v2 예정: start/health/stop. v3 예정: 4시나리오 자동 실행)

${BOLD}배포 패키지 (Console 업로드용) — [3/3] 단계:${NC}
  pkg [-v X.Y.Z] [--no-bump] [--no-sync] [-m <changelog>] [name...]
                                 configure 완료된 build/dist 를 모듈별 tar.gz 로 패키징.
                                 각 tarball 최상위: meta.json (name/version/설명) +
                                 config_template.json (설정 스키마) + <모듈>/ 파일.
                                 기본: auto-bump patch + source→dist auto-sync.
                                 -v 지정 시 해당 버전 강제 + pkg.json 반영
                                 --no-bump 면 현재 pkg.json 버전 그대로 (재패키징)
                                 --no-sync 면 auto-sync 건너뜀 (옛 dist 그대로 — 디버깅용)
                                 C++ 바이너리 (csp/cmp/cspsim) 는 mtime 검사 후 warn 만.
                                 예: ./cims.sh pkg               # 0.0.3 → 0.0.4 자동 + 동기화
                                     ./cims.sh pkg -v 1.0.0 csp  # csp 만 1.0.0 강제

${BOLD}예시:${NC}
  # [1/3] 빌드 → [2/3] 시험환경 설정 → 기동 (소스 트리)
  $(basename "$0") build
  $(basename "$0") configure --local-ip 192.168.1.10 --db-password secret
  $(dirname "${BASH_SOURCE[0]}")/agent/bin/cims-svc start

  # [3/3] 배포 패키지 생성 (Phase 1 검증 통과 후)
  $(basename "$0") pkg                            # 모든 모듈 auto-bump
  $(basename "$0") pkg -v 1.2.0 csp               # csp 만 1.2.0 강제

  # 배포 서버에서 (dist/ 내부)
  ./cims.sh configure --local-ip 192.168.1.10
  ./agent/bin/cims-svc start

  # 시뮬레이터 (동시 실행)
  $(basename "$0") clean                                  # 데이터 정리
  $(basename "$0") sim -mode ptt -group +82571910001      # 영상 PTT 포그라운드
  $(basename "$0") sim -mode ptt -group +82571910001 --bg # 영상 PTT 백그라운드
  $(basename "$0") sim -mode volte --bg                   # VoLTE 동시 실행

  ./agent/bin/cims-svc stop all
  ./agent/bin/cims-svc status
  ./agent/bin/cims-svc log csp
EOF
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

    # ── CSC Python 소스 (+ OAM Phase 1: 같은 binary, sys.path mount) ──
    if [[ $did_csc -eq 1 ]]; then
        mkdir -p "$DIST_DIR/csc/src" "$DIST_DIR/oam/src"
        # rsync 가 있으면 사용, 없으면 cp -r (목적지 깨끗이)
        if command -v rsync >/dev/null 2>&1; then
            rsync -a --delete-excluded \
                --exclude='__pycache__' --exclude='*.pyc' \
                "$SCRIPT_DIR/csc/src/" "$DIST_DIR/csc/src/"
            rsync -a --delete-excluded \
                --exclude='__pycache__' --exclude='*.pyc' \
                "$SCRIPT_DIR/oam/src/" "$DIST_DIR/oam/src/"
        else
            cp -r "$SCRIPT_DIR/csc/src/." "$DIST_DIR/csc/src/"
            cp -r "$SCRIPT_DIR/oam/src/." "$DIST_DIR/oam/src/"
        fi
        # __pycache__ stale 제거 (PEP 420 namespace 전환에 따른 옛 캐시 잔재)
        find "$DIST_DIR/csc/src" "$DIST_DIR/oam/src" -type d -name __pycache__ \
            -exec rm -rf {} + 2>/dev/null || true
        # config_template.json 도 동기화 (apply_config_template 가 읽는 파일)
        if [[ -f "$SCRIPT_DIR/csc/config/config_template.json" ]]; then
            mkdir -p "$DIST_DIR/csc/config"
            cp -f "$SCRIPT_DIR/csc/config/config_template.json" \
                  "$DIST_DIR/csc/config/config_template.json"
        fi
        # OAM 분리 Phase 2 — pkg.json 동기화 (별도 tarball 등록에 필요)
        if [[ -f "$SCRIPT_DIR/oam/pkg.json" ]]; then
            cp -f "$SCRIPT_DIR/oam/pkg.json" "$DIST_DIR/oam/pkg.json"
        fi
        ok "csc/src + oam/src (+ config_template.json, oam/pkg.json) ← $SCRIPT_DIR"
        n_changed=$((n_changed+1))
    fi

    # ── Agent 바이너리 + 운영 도구 (bin/lib/keepalived/systemd) ──
    if [[ $did_agent -eq 1 ]]; then
        _ensure_agent_vendor_keepalived
        mkdir -p "$DIST_DIR/agent"
        cp -f "$SCRIPT_DIR/agent/cims_agent.py"     "$DIST_DIR/agent/"
        cp -f "$SCRIPT_DIR/agent/install-agent.sh"  "$DIST_DIR/agent/"
        chmod +x "$DIST_DIR/agent/install-agent.sh"
        [[ -f "$SCRIPT_DIR/agent/pkg.json" ]] && cp -f "$SCRIPT_DIR/agent/pkg.json" "$DIST_DIR/agent/"
        # 운영 도구 (cims-svc / cims-ha / cims-health / cims-notify + lifecycle.sh / ha.sh) + vendor deb
        if command -v rsync >/dev/null 2>&1; then
            for sub in bin lib keepalived systemd vendor; do
                [[ -d "$SCRIPT_DIR/agent/$sub" ]] && \
                    rsync -a --delete --exclude='out/' --exclude='ha.json' \
                          "$SCRIPT_DIR/agent/$sub/" "$DIST_DIR/agent/$sub/"
            done
        else
            for sub in bin lib keepalived systemd vendor; do
                [[ -d "$SCRIPT_DIR/agent/$sub" ]] && \
                    { rm -rf "$DIST_DIR/agent/$sub"; cp -r "$SCRIPT_DIR/agent/$sub" "$DIST_DIR/agent/$sub"; }
            done
        fi
        chmod +x "$DIST_DIR/agent/bin/"* 2>/dev/null || true
        chmod +x "$DIST_DIR/agent/lib/"*.sh 2>/dev/null || true
        chmod +x "$DIST_DIR/agent/keepalived/"*.sh 2>/dev/null || true
        ok "agent (+ bin/lib/keepalived/systemd) ← $SCRIPT_DIR/agent"
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
    # VITE_CONSOLE_TARGET=prod — sync 도 배포본 dist 기준 (TB-Console 은 dev 서버 별도)
    if [[ $did_console -eq 1 ]]; then
        ( cd "$SRC_CONSOLE" && VITE_CONSOLE_TARGET=prod npm run build 2>&1 | tail -3 )
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

# agent vendor 자동 채움 — keepalived offline 설치용 deb 6종
# 누락된 패키지만 apt-get download 로 받음 (sudo 불필요). idempotent.
# CIMS_SKIP_VENDOR_FETCH=1 로 끌 수 있음 (인터넷/apt 없는 환경).
_KEEPALIVED_DEPS=(keepalived libmnl0 libnftnl11 libnl-3-200 libnl-genl-3-200 libsnmp40t64)

_ensure_agent_vendor_keepalived() {
    local vendor_dir="$SCRIPT_DIR/agent/vendor/keepalived"
    mkdir -p "$vendor_dir"

    [[ -n "${CIMS_SKIP_VENDOR_FETCH:-}" ]] && return 0

    local missing=() pkg
    for pkg in "${_KEEPALIVED_DEPS[@]}"; do
        compgen -G "$vendor_dir/${pkg}_*.deb" >/dev/null 2>&1 || missing+=("$pkg")
    done
    [[ ${#missing[@]} -eq 0 ]] && return 0

    if ! command -v apt-get &>/dev/null; then
        warn "apt-get 미지원 환경 — agent/vendor/keepalived 누락: ${missing[*]} (수동 채움 필요)"
        return 0
    fi

    info "agent/vendor/keepalived: ${#missing[@]}/${#_KEEPALIVED_DEPS[@]} 누락 → apt-get download (${missing[*]})"
    if ! ( cd "$vendor_dir" && apt-get download "${missing[@]}" >/dev/null 2>&1 ); then
        warn "apt-get download 실패 — vendor 미완성 가능 (인터넷/apt 캐시 확인). CIMS_SKIP_VENDOR_FETCH=1 로 차단"
        return 0
    fi
    ok "agent/vendor/keepalived: ${missing[*]} 자동 채움"
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
    local no_sync=0
    local targets=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -v|--version)   version="$2"; shift 2 ;;
            -m|--changelog) changelog="$2"; shift 2 ;;
            --no-bump)      no_bump=1; shift ;;
            --no-sync)      no_sync=1; shift ;;
            -*) err "알 수 없는 옵션: $1"; return 1 ;;
            *)  targets+=("$1"); shift ;;
        esac
    done
    # default targets — 9 모듈 + 부가 (cwrtc/phone/agent).
    # csp 바이너리는 다용도 → csp/isp/psp 3 tarball (소스/dist 디렉토리는 동일,
    # tarball 이름과 meta.json 의 name 만 분리 — Roles/LocalIp 는 deploy overlay 가 결정).
    # cmp 바이너리도 동일 → cmp/imp/pmp.
    [[ ${#targets[@]} -eq 0 ]] && targets=(cmp pmp imp csp psp isp cwrtc csc oam console phone cspsim agent)

    if [[ ! -d $DIST_DIR ]]; then
        err "dist 디렉토리 없음: $DIST_DIR (먼저 ./cims.sh build)"
        return 1
    fi

    # ── 소스 → dist auto-sync (#15) ───────────────────────────────────────
    # cmd_pkg 가 dist 를 tar 하므로, source 가 변경됐는데 dist 에 미반영이면 옛 코드가
    # tarball 에 박힘. 이 함정에 반복적으로 막힌 회기 이력 (agent 0.0.13/16/20, CSC handler)
    # 으로 인해 자동 sync 를 기본 동작으로. --no-sync 로 끄기 가능.
    # C++ 바이너리 (csp/cmp/cspsim) 는 cmake build 가 별도 → 여기서는 mtime 비교 후 warn.
    if [[ $no_sync -ne 1 && -n "$SRC_CONSOLE" ]]; then
        local -A _sync_set=()
        # pkg-meta / scripts 는 어느 컴포넌트를 패키징하든 항상 동기화 (cims.sh / pkg.json 박힘 방지)
        _sync_set[pkg-meta]=1
        _sync_set[scripts]=1
        local _t
        for _t in "${targets[@]}"; do
            case "$_t" in
                csc|oam) _sync_set[csc]=1 ;;   # OAM 분리 Phase 2 — sync csc 가 oam/src 도 함께
                agent)   _sync_set[agent]=1 ;;
                console) _sync_set[console]=1 ;;
                phone)   _sync_set[phone]=1 ;;
            esac
        done
        local _sync_list=("${!_sync_set[@]}")
        if [[ ${#_sync_list[@]} -gt 0 ]]; then
            info "auto-sync (소스 → dist): ${_sync_list[*]}"
            cmd_sync "${_sync_list[@]}" || warn "auto-sync 일부 실패 — 옛 dist 로 패키징 진행"
        fi
    elif [[ $no_sync -eq 1 ]]; then
        warn "--no-sync 모드: source → dist sync 건너뜀 (옛 dist 로 패키징됨)"
    fi

    # C++ 바이너리 stale 경고 (dist 바이너리가 src 보다 오래된 경우)
    local -A _bin_checked=()
    local _bin_key _bin _src
    for _t in "${targets[@]}"; do
        case "$_t" in
            csp|psp|isp) _bin_key="csp" ;;
            cmp|pmp|imp) _bin_key="cmp" ;;
            cspsim)      _bin_key="cspsim" ;;
            *)           _bin_key="" ;;
        esac
        [[ -z "$_bin_key" || -n "${_bin_checked[$_bin_key]:-}" ]] && continue
        _bin_checked[$_bin_key]=1
        _bin="$DIST_DIR/$_bin_key/bin/$_bin_key"
        _src="$SCRIPT_DIR/$_bin_key/src"
        if [[ -f "$_bin" && -d "$_src" ]]; then
            if find "$_src" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.h' -o -name '*.hpp' \) -newer "$_bin" 2>/dev/null | grep -q .; then
                warn "$_bin_key: dist 바이너리가 src 보다 오래됨 → 'cims.sh build' 후 다시 pkg 권장"
            fi
        fi
    done

    # 컴포넌트별 소스 루트 매핑 — 각 소스 루트의 pkg.json 에서 name/description 를 가져옴
    # (dist/ 밖에서 실행되는 경우만 소스 루트가 있으며, 그 외에는 dist/<comp>/pkg.json 로 fallback)
    _src_root_for() {
        case "$1" in
            csp|psp|isp) echo "$SCRIPT_DIR/csp" ;;   # 동일 csp 바이너리 + 동일 config_template
            cmp|pmp|imp) echo "$SCRIPT_DIR/cmp" ;;   # 동일 cmp 바이너리 + 동일 config_template
            csc)         echo "$SCRIPT_DIR/csc" ;;
            oam)         echo "$SCRIPT_DIR/oam" ;;   # OAM 분리 Phase 2 — 같은 cims-csc 프로세스, 별도 tarball
            cwrtc)       echo "$SCRIPT_DIR/cwrtc" ;;
            console)     echo "$SCRIPT_DIR/cims-console" ;;
            phone)       echo "$SCRIPT_DIR/cims-phone" ;;
            cspsim)      echo "$SCRIPT_DIR/cspsim" ;;
            agent)       echo "$SCRIPT_DIR/agent" ;;
            *)           echo "" ;;
        esac
    }

    # Tarball 안 모듈 디렉토리 이름 — 패키지 정체성 분리: psp/isp/pmp/imp 도
    # 자기 이름의 디렉토리로 들어감. dist 트리는 csp/cmp 한 종 그대로 두고,
    # 변종은 pkg 단계에서 staging 디렉토리 (dist/csp 복사 + 바이너리/config rename)
    # 로 새 디렉토리를 만든 후 tar.
    _src_sub_for() {
        case "$1" in
            *) echo "$1" ;;   # 모든 컴포넌트 자기 이름
        esac
    }

    # 변종 (psp/isp/pmp/imp) 의 base dist 디렉토리 — 같은 ELF 사용
    _base_dist_for() {
        case "$1" in
            psp|isp) echo "csp" ;;
            pmp|imp) echo "cmp" ;;
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

    local t src_sub tar_file build_date pkg_root base_dist stage
    for t in "${targets[@]}"; do
        case "$t" in
            cmp|pmp|imp|csp|psp|isp|cwrtc|csc|oam|console|phone|cspsim|agent)
                src_sub=$(_src_sub_for "$t") ;;
            *) err "알 수 없는 컴포넌트: $t"; continue ;;
        esac

        # 변종 (psp/isp/pmp/imp): staging 에 base dist (csp/cmp) 복사 + 바이너리/config
        # 이름을 변종 이름으로 rename → tar root 가 staging 이 됨. dist/csp 자체는 손대지 않음.
        pkg_root="$DIST_DIR"
        stage=""
        base_dist=$(_base_dist_for "$t")
        if [[ -n "$base_dist" ]]; then
            if [[ ! -d "$DIST_DIR/$base_dist" ]]; then
                warn "skip: $DIST_DIR/$base_dist 없음 (variant=$t base=$base_dist)"; continue
            fi
            stage="$DIST_DIR/.pkgstage.$$.${t}"
            rm -rf "$stage"
            mkdir -p "$stage/$t"
            # base dist 의 내용 그대로 복사 (cp -a 로 권한/심볼릭 보존).
            cp -a "$DIST_DIR/$base_dist/." "$stage/$t/"
            # 바이너리 rename (csp → psp 등).
            [[ -f "$stage/$t/bin/$base_dist" ]] && mv "$stage/$t/bin/$base_dist" "$stage/$t/bin/$t"
            # 시작 스크립트 rename (있을 때만 — csp.sh → psp.sh).
            [[ -f "$stage/$t/bin/$base_dist.sh" ]] && mv "$stage/$t/bin/$base_dist.sh" "$stage/$t/bin/$t.sh"
            # config 파일 rename (configure 후라면 있고, 빌드 직후라면 없음).
            [[ -f "$stage/$t/config/$base_dist.json" ]] && mv "$stage/$t/config/$base_dist.json" "$stage/$t/config/$t.json"
            # cims.sh 도 staging 으로 (tar root 에 포함).
            [[ -f "$DIST_DIR/cims.sh" ]] && cp "$DIST_DIR/cims.sh" "$stage/"
            pkg_root="$stage"
        elif [[ ! -d "$DIST_DIR/$src_sub" ]]; then
            warn "skip: $DIST_DIR/$src_sub 없음 (target=$t src_sub=$src_sub)"; continue
        fi

        # build_date = 컴포넌트 dist 디렉토리 안에서 가장 최근 파일의 mtime (base dist 기준 — staging 은 cp 로 mtime 갱신될 수 있음).
        local _bd_root="$DIST_DIR/${base_dist:-$src_sub}"
        build_date=$(find "$_bd_root" -type f -printf '%T@\n' 2>/dev/null \
                        | sort -nr | head -1 \
                        | xargs -I{} date -u -d @{} +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo "")

        # 소스 루트 pkg.json 에서 description/version 을 읽음 (없으면 dist/<comp>/pkg.json fallback)
        local comp_meta=""
        local src_root; src_root=$(_src_root_for "$t")
        for cand in "$src_root/pkg.json" "$DIST_DIR/$t/pkg.json"; do
            [[ -n $cand && -f $cand ]] && comp_meta="$cand" && break
        done
        [[ -z $comp_meta ]] && warn "$t: pkg.json 없음 — description 공란"

        # 이 모듈의 실제 적용 버전 결정 (explicit > no-bump > auto-bump patch).
        # 변종 (psp/isp/pmp/imp) 은 base (csp/cmp) 의 version 을 read-only 로 따라감 —
        # 9 tarball 이 같은 patch+1 을 3번 누적하지 않도록.
        local effective_no_bump="$no_bump"
        case "$t" in
            psp|isp|pmp|imp) effective_no_bump=1 ;;
        esac
        local comp_ver; comp_ver=$(_resolve_version "$comp_meta" "$version" "$effective_no_bump")
        # pkg.json 에 반영 (base 만 — 변종은 read-only)
        if [[ -n $comp_ver && "$effective_no_bump" != "1" ]]; then
            [[ -n $comp_meta ]] && _pkg_write_version "$comp_meta" "$comp_ver"
            local dist_meta="$DIST_DIR/$t/pkg.json"
            [[ -f $dist_meta && "$dist_meta" != "$comp_meta" ]] && _pkg_write_version "$dist_meta" "$comp_ver"
        fi

        # meta.json 생성 (pkg_root 안에 임시로 작성 → tar 루트에 추가 후 삭제;
        # 변종은 staging, 그 외는 DIST_DIR).
        local tmp_meta="$pkg_root/.pkgmeta.$$.json"
        python3 - "$comp_meta" "$t" "$comp_ver" "$build_date" "$git_sha" "$git_branch" \
                  "$packaged_at" "$packaged_by" "$changelog" <<'PYEOF' > "$tmp_meta"
import sys, json, os
meta_file, name, version, build_date, git_sha, git_branch, packaged_at, packaged_by, changelog = sys.argv[1:]
desc = ""
service = None
ha_capability = None
# 소스 루트 pkg.json 은 단일 컴포넌트 형식: { "name": "...", "description": "...", "ha_capability": "...", "service": {...} }
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
                ha_capability = entry.get("ha_capability")
            # 구(舊) 레지스트리 스키마 (후방 호환)
            elif name in entry and isinstance(entry[name], dict):
                desc = entry[name].get("description", "")
                if isinstance(entry[name].get("service"), dict):
                    service = entry[name]["service"]
                ha_capability = entry[name].get("ha_capability")
    except Exception:
        pass
# csp/cmp 변종은 base description 끝에 역할 suffix 추가 (식별용).
_ROLE_SUFFIX = {
    "psp": " · PSP role (PTT CSCF + PTT-AS)",
    "isp": " · ISP role (IBCF / IP-PBX trunk)",
    "pmp": " · PMP role (PTT RTP/Floor)",
    "imp": " · IMP role (IBCF media)",
}
if name in _ROLE_SUFFIX:
    desc = (desc or "").rstrip() + _ROLE_SUFFIX[name]
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
if ha_capability is not None:
    meta["ha_capability"] = ha_capability
print(json.dumps(meta, indent=2, ensure_ascii=False))
PYEOF

        # config_template.json: v3 (2026-04-22) 부터 소스의 config/ 아래.
        #   tarball 에는 그대로 최상위(/config_template.json) 로 포함 (agents.py 가 루트에서 파싱).
        local tmp_tmpl="$pkg_root/.pkgtmpl.$$.json"
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
        #  packages/    : 배포본 CSC 가 수집한 업로드 tarball (신규 배포에 포함되면 중복 팽창)
        #  packages_tb/ : TB-CSC 가 수집한 업로드 tarball — packages 와 별개 store.
        #                 누락 시 csc tarball 이 GB 단위로 부풀어 S5-CSC-DEPLOY-INSTALL 60s timeout.
        #  packages_trash/ : TB-CSC 삭제 보관소
        #  cdr/         : CDR 산출물
        #  dist/        : 번들러 산출물 이 아닌 상위 dist 와 혼동 방지 (cwrtc/dist 등 없음)
        ( cd "$pkg_root" && \
            tar czf "$tar_file" \
                --exclude="$src_sub/log" \
                --exclude="$src_sub/run" \
                --exclude="$src_sub/cache" \
                --exclude="$src_sub/cache_tb" \
                --exclude="$src_sub/packages" \
                --exclude="$src_sub/packages_tb" \
                --exclude="$src_sub/packages_trash" \
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
        # 변종 staging cleanup
        [[ -n "$stage" && -d "$stage" ]] && rm -rf "$stage"
        local size; size=$(stat -c%s "$tar_file" 2>/dev/null || echo 0)
        ok "$(basename "$tar_file") ($(numfmt --to=iec --suffix=B "$size" 2>/dev/null || echo "${size}B"))"
    done

    # stale 버전 cleanup — 각 component 의 mtime 기준 최신 1개만 보존, 나머지 제거.
    # 배경: verify/lib/items/stage5/_native_steps.py:_latest_tarball() 의 natural-sort 가
    #   잔재 0.0.2 같은 stale tarball 을 선택 → deploy 가 OLD binary 사용.
    # 이 라운드에 패키징한 component 만 정리 (다른 컴포넌트 손대지 않음).
    local _cleaned=0 _latest _stale
    for t in "${targets[@]}"; do
        _latest=$(ls -1t "$out_dir/${t}-"*.tar.gz 2>/dev/null | head -1)
        [[ -z "$_latest" ]] && continue
        while IFS= read -r _stale; do
            [[ "$_stale" == "$_latest" ]] && continue
            rm -f "$_stale" && _cleaned=$((_cleaned+1)) && info "stale 제거: $(basename "$_stale")"
        done < <(ls -1 "$out_dir/${t}-"*.tar.gz 2>/dev/null)
    done
    [[ $_cleaned -gt 0 ]] && ok "stale tarball $_cleaned 개 정리"

    # manifest.json 생성/갱신 — 현재 packages/*.tar.gz 의 SHA256 + size + mtime 기록.
    # Console UI 의 다운로드 라벨 (버전 표시) 과 검증 S6 의 immutability gate 가 이 파일 사용.
    # 검증 S4-PKG-MANIFEST 가 같은 로직으로 만들지만, cmd_pkg 직후에도 항상 fresh 하도록.
    local manifest_path="$out_dir/manifest.json"
    local _git_sha="${git_sha:-}" _git_branch="${git_branch:-}"
    local _host; _host=$(hostname -s 2>/dev/null || echo unknown)
    python3 - "$out_dir" "$manifest_path" "$_git_sha" "$_git_branch" "$_host" <<'PYEOF' \
        && ok "manifest.json 갱신 → $manifest_path" \
        || warn "manifest.json 갱신 실패"
import sys, os, json, hashlib
from datetime import datetime, timezone
out_dir, out_path, git_sha, git_branch, host = sys.argv[1:6]
def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(64*1024), b''):
            h.update(chunk)
    return h.hexdigest()
entries = []
for fn in sorted(os.listdir(out_dir)):
    if not fn.endswith('.tar.gz'): continue
    full = os.path.join(out_dir, fn)
    entries.append({
        'name':   fn,
        'size':   os.path.getsize(full),
        'sha256': sha256(full),
        'mtime':  datetime.fromtimestamp(os.path.getmtime(full), tz=timezone.utc).isoformat(),
    })
manifest = {
    'ts': datetime.now(timezone.utc).astimezone().isoformat(),
    'git': {'branch': git_branch, 'sha': git_sha},
    'host': host,
    'ens_ip': '',
    'packages': entries,
}
with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(manifest, f, ensure_ascii=False, indent=2)
PYEOF

    header "[3/3] 생성된 패키지 (업로드 대상):"
    ls -lh "$out_dir"/*.tar.gz 2>/dev/null | awk '{printf "  %s  %s\n", $5, $9}'
    echo ""
    info "Console 에서 업로드: 배포 관리 → 패키지 → ＋ 업로드 (파일만 선택하면 meta 자동 인식)"
}

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
  target: csc | console | all  (기본: all)

  TB-CSC     — https://127.0.0.1:4419 (csc_app.py + csc-tb.json)
  TB-Console — http://127.0.0.1:3000  (vite dev 서버 — 소스 트리에서만)

  예:
    cims.sh tb status              # 4419 / 3000 확인
    cims.sh tb start csc           # TB-CSC 만 기동
    cims.sh tb restart             # 둘 다 재기동
    cims.sh tb stop console        # TB-Console 만 정지
EOF
            return 0 ;;
        *) err "알 수 없는 tb 동작: $action (start|stop|restart|status|help)"; return 1 ;;
    esac
    case "$target" in
        csc|console|all) ;;
        *) err "알 수 없는 tb 대상: $target (csc|console|all)"; return 1 ;;
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
        if [[ -n $pid ]]; then ok "TB-CSC     pid=$pid  https://127.0.0.1:4419"
        else warn "TB-CSC     미동작  — 'cims.sh tb start csc'"; fi
        pid=$(_tb_port_pid 3000)
        if [[ -n $pid ]]; then ok "TB-Console pid=$pid  http://127.0.0.1:3000"
        else warn "TB-Console 미동작  — 'cims.sh tb start console'"; fi
    }

    case "$action" in
        start)
            [[ $target == csc     || $target == all ]] && _tb_csc_start
            [[ $target == console || $target == all ]] && _tb_console_start
            ;;
        stop)
            [[ $target == csc     || $target == all ]] && _tb_csc_stop
            [[ $target == console || $target == all ]] && _tb_console_stop
            ;;
        restart)
            [[ $target == csc     || $target == all ]] && { _tb_csc_stop; _tb_csc_start; }
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
    sim)       shift; cmd_sim "$@" ;;
    clean)     shift; cmd_clean "${1:-all}" ;;
    reset)     shift; cmd_reset "$@" ;;
    preflight) cmd_preflight ;;
    verify)    shift; cmd_verify "$@" ;;
    pkg)       shift; cmd_pkg "$@" ;;
    sync)      shift; cmd_sync "$@" ;;
    tb)        shift; cmd_tb "$@" ;;
    # 운영 명령 (start/stop/restart/status/log/ha) 은 agent/bin/cims-{svc,ha} 로 이전됨 (Phase 1.B+).
    start|stop|restart|status|log)
        err "운영 명령 '$1' 은 agent/bin/cims-svc 로 이전됨"
        err "  사용: $(dirname "${BASH_SOURCE[0]}")/agent/bin/cims-svc $1 ${2:-}"
        err "  (TB-CSC/TB-Console 은 'cims.sh tb $1' 사용)"; exit 2 ;;
    ha)
        err "ha 명령은 agent/bin/cims-ha 로 이전됨"
        err "  사용: $(dirname "${BASH_SOURCE[0]}")/agent/bin/cims-ha ${2:-help}"; exit 2 ;;
    help|--help|-h) usage ;;
    "") usage ;;
    *) err "알 수 없는 명령: $1"; echo ""; usage; exit 1 ;;
esac
