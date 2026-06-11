#!/usr/bin/env bash
# CIMS 부트스트랩 인스톨러 — 상용(Private) 망 1단계 설치
#
# 서비스 모듈(csp/cmp/csc 등)과 무관하게 base 운영 평면(OAM + Console + Agent
# 에셋)만 설치·기동한다. 이후 절차는 모두 콘솔에서:
#   2) 시스템/서버 구성 (각 서버 agent 설치 — 콘솔의 install-command)
#   3) 패키지 등록 (서비스 모듈 + oam/console/agent 업데이트 패키지)
#   4) 패키지 설치   5) 패키지 설정
#
# 동작:
#   - /opt/cims-agent/{oam,console}/<버전>/ 의 "버전 단위 설치" 레이아웃으로 배치
#     (이후 콘솔/agent 의 업그레이드·롤백 체계와 동일 — 2~4단계에서 자연 인수)
#   - OAM 이 콘솔(SPA 정적)과 API 를 단일 HTTPS 오리진(:4419)으로 서빙
#   - self-signed TLS 인증서·JwtSecret 자동 생성 (재설치 시 기존 보존)
#   - 동봉 패키지(oam/console/agent)를 seed_packages 로 배치 → OAM 첫 부팅 시
#     패키지 저장소에 자동 등록 (콘솔 패키지 목록·/install-agent.sh 즉시 동작)
#   - systemd 등록(cims-oam.service) 또는 --no-systemd 시 start 스크립트 생성
#
# 사용:
#   sudo ./install.sh [옵션]
#     --prefix DIR     설치 루트 (기본 /opt/cims-agent)
#     --port N         OAM HTTPS 포트 (기본 4419)
#     --admin-pass PW  내장 admin 비밀번호 설정 (기본 1234 — 상용은 변경 권장)
#     --no-systemd     systemd 미사용 (start 스크립트 생성)
#     --no-start       설치만 하고 기동하지 않음
#     --no-agent       이 서버의 agent 자동 설치/기동 생략
#     --batch          대화식 입력 생략 (옵션/기본값만 사용 — 자동화용)
#   옵션 없이 실행하면 설치 경로/포트/admin 비밀번호를 단계별로 묻는다.
#   제거: sudo <prefix>/uninstall-base.sh [--yes]
#     --user USER      서비스 사용자 (기본: sudo 호출자) — agent/OAM 프로세스 소유자
set -euo pipefail

PREFIX=/opt/cims-agent
PORT=4419
ADMIN_PASS=""
USE_SYSTEMD=1
DO_START=1
DO_AGENT=1
BATCH=0
# 서비스 사용자 — sudo 호출자 (agent/OAM 프로세스 소유자. 모듈 설치 경로 쓰기 주체)
SVC_USER="${SUDO_USER:-$(id -un)}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)     PREFIX="$2"; shift 2 ;;
        --port)       PORT="$2"; shift 2 ;;
        --admin-pass) ADMIN_PASS="$2"; shift 2 ;;
        --no-systemd) USE_SYSTEMD=0; shift ;;
        --no-start)   DO_START=0; shift ;;
        --no-agent)   DO_AGENT=0; shift ;;
        --batch)      BATCH=1; shift ;;
        --user)       SVC_USER="$2"; shift 2 ;;
        -h|--help)    grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "알 수 없는 옵션: $1"; exit 1 ;;
    esac
done

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_DIR="$HERE/packages"

info() { echo -e "\033[0;36m[INFO]\033[0m  $*"; }
ok()   { echo -e "\033[0;32m[OK]\033[0m    $*"; }
err()  { echo -e "\033[0;31m[ERROR]\033[0m $*" >&2; }

# ── 전제 확인 ─────────────────────────────────────────────────
for c in python3 tar openssl; do
    command -v "$c" >/dev/null || { err "$c 필요 — 설치 후 재시도"; exit 1; }
done

# ── 대화식 초기 설정 (tty + --batch 미지정 시) ────────────────────
#    각 항목은 명령행 옵션으로 지정했으면 건너뛴다.
PORT_GIVEN=0; PREFIX_GIVEN=0
for _a in "$@"; do :; done   # (옵션 파싱은 위에서 완료 — 지정 여부는 기본값 비교로 판단)
[[ "$PREFIX" != "/opt/cims-agent" ]] && PREFIX_GIVEN=1
[[ "$PORT" != "4419" ]] && PORT_GIVEN=1

if [[ $BATCH -eq 0 ]] && { [[ -t 0 ]] || [[ -n "${CIMS_INSTALL_FORCE_INTERACTIVE:-}" ]]; }; then
    echo ""
    echo "── CIMS base 초기 설정 (Enter = 기본값) ─────────────────────"
    # [1] 설치 경로
    if [[ $PREFIX_GIVEN -eq 0 ]]; then
        read -r -p "  [1/4] 설치 경로 [$PREFIX]: " _in
        [[ -n "$_in" ]] && PREFIX="$_in"
    fi
    # [2] 콘솔/OAM HTTPS 포트 (단일 오리진 — 콘솔 웹과 API/agent 통신이 같은 포트)
    if [[ $PORT_GIVEN -eq 0 ]]; then
        while :; do
            read -r -p "  [2/4] 콘솔/OAM HTTPS 포트 (웹·API 단일) [$PORT]: " _in
            [[ -z "$_in" ]] && break
            if [[ "$_in" =~ ^[0-9]+$ ]] && (( _in >= 1 && _in <= 65535 )); then
                PORT="$_in"; break
            fi
            echo "      포트는 1~65535 숫자여야 합니다"
        done
    fi
    # [3] admin 비밀번호 최초 등록 (필수 — 미입력 시 반복)
    if [[ -z "$ADMIN_PASS" ]]; then
        while :; do
            read -r -s -p "  [3/4] admin 비밀번호 (최초 등록, 4자 이상): " _p1; echo
            if [[ ${#_p1} -lt 4 ]]; then echo "      4자 이상 입력하세요"; continue; fi
            read -r -s -p "        비밀번호 확인: " _p2; echo
            [[ "$_p1" == "$_p2" ]] && { ADMIN_PASS="$_p1"; break; }
            echo "      일치하지 않습니다 — 다시 입력"
        done
    fi
    # [4] 로컬 agent 자동 설치
    read -r -p "  [4/4] 이 서버의 agent 자동 설치/기동 [Y/n]: " _in
    [[ "$_in" == n* || "$_in" == N* ]] && DO_AGENT=0
    echo ""
    echo "── 설치 요약 ────────────────────────────────────────────────"
    echo "    설치 경로     : $PREFIX"
    echo "    HTTPS 포트    : $PORT  (콘솔 웹 + API + agent 통신 단일 오리진)"
    echo "    admin 비밀번호: (입력됨)"
    echo "    서비스 사용자 : $SVC_USER"
    echo "    로컬 agent    : $([[ $DO_AGENT -eq 1 ]] && echo 설치 || echo 생략)"
    read -r -p "  진행할까요? [Y/n]: " _in
    [[ "$_in" == n* || "$_in" == N* ]] && { echo "중단"; exit 1; }
    echo ""
elif [[ -z "$ADMIN_PASS" ]]; then
    err "admin 비밀번호 미설정 — 기본값(1234)으로 진행합니다. 상용에서는 --admin-pass 필수!"
fi

# 권한 체크 — 대화식 입력으로 확정된 PREFIX 기준
if [[ $EUID -ne 0 && ! -w "$(dirname "$PREFIX")" && ! -w "$PREFIX" ]]; then
    err "root 권한 필요 (또는 $PREFIX 쓰기 가능해야 함) — sudo 로 실행"
    exit 1
fi

_latest() { ls -1 "$PKG_DIR"/$1-*.tar.gz 2>/dev/null | sort -V | tail -1; }
OAM_TAR=$(_latest oam); CON_TAR=$(_latest console); AGT_TAR=$(_latest agent)
for v in OAM_TAR CON_TAR AGT_TAR; do
    [[ -n "${!v}" ]] || { err "packages/ 에 ${v%_TAR} tarball 없음"; exit 1; }
done
_ver() { basename "$1" .tar.gz | sed 's/^[a-z]*-//'; }
OAM_VER=$(_ver "$OAM_TAR"); CON_VER=$(_ver "$CON_TAR"); AGT_VER=$(_ver "$AGT_TAR")
info "설치 구성: oam $OAM_VER / console $CON_VER / agent $AGT_VER → $PREFIX (HTTPS :$PORT)"

# ── 레이아웃 (버전 단위 설치 — agent 배포 체계와 동일, 모듈은 modules/ 하위) ──
MODULES_DIR="$PREFIX/modules"
OAM_ROOT="$MODULES_DIR/oam/$OAM_VER"
CON_ROOT="$MODULES_DIR/console/$CON_VER"
RUNTIME_DIR="$MODULES_DIR/oam/runtime"     # 버전 무관 영속 store (업그레이드 생존)
mkdir -p "$OAM_ROOT" "$CON_ROOT" "$RUNTIME_DIR"

info "패키지 전개..."
tar xzf "$OAM_TAR" -C "$OAM_ROOT"
tar xzf "$CON_TAR" -C "$CON_ROOT"
# agent 는 전개하지 않음 — 설치 에셋(/install-agent.sh, /cims_agent.py,
# /agent-bundle.tar.gz)의 SoT 는 패키지 저장소(seed 자동 등록). 버전별로
# 보관되어 다른 모듈과 동일하게 업데이트/롤백 관리.
mkdir -p "$OAM_ROOT/config" "$OAM_ROOT/run" "$OAM_ROOT/log"

# seed 패키지 — OAM 첫 부팅 시 패키지 저장소 자동 등록 (1단계 산출물도
# 콘솔에서 업데이트 가능한 패키지로 보이도록; 서비스 모듈은 3단계에서 등록)
mkdir -p "$OAM_ROOT/oam/seed_packages"
cp -f "$OAM_TAR" "$CON_TAR" "$AGT_TAR" "$OAM_ROOT/oam/seed_packages/"

# ── TLS 인증서 (self-signed; 재설치 시 보존) ─────────────────────
CERT_DIR="$OAM_ROOT/oam/cert"
mkdir -p "$CERT_DIR"
if [[ ! -f "$CERT_DIR/server.key" || ! -f "$CERT_DIR/server.crt" ]]; then
    HOSTNM=$(hostname -f 2>/dev/null || hostname)
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -subj "/CN=${HOSTNM}/O=CIMS" \
        -addext "subjectAltName=DNS:${HOSTNM},IP:127.0.0.1" \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" 2>/dev/null
    chmod 600 "$CERT_DIR/server.key"
    ok "self-signed TLS 인증서 생성 (CN=$HOSTNM) — 상용 인증서는 $CERT_DIR 에 교체"
else
    ok "기존 TLS 인증서 보존"
fi

# ── oam.json 구성 ────────────────────────────────────────────
JWT_SECRET_FILE="$RUNTIME_DIR/.jwt_secret"
if [[ ! -f "$JWT_SECRET_FILE" ]]; then
    openssl rand -base64 32 > "$JWT_SECRET_FILE"
    chmod 600 "$JWT_SECRET_FILE"
fi
PY=python3 OAM_ROOT="$OAM_ROOT" RUNTIME_DIR="$RUNTIME_DIR" PORT="$PORT" \
JWT_SECRET="$(cat "$JWT_SECRET_FILE")" \
ADMIN_PASS="$ADMIN_PASS" python3 - <<'PYEOF'
import hashlib, json, os
p = os.path.join(os.environ['OAM_ROOT'], 'oam', 'config', 'oam.json')
d = json.load(open(p))
d['Server'] = {'Ip': '0.0.0.0', 'Port': int(os.environ['PORT'])}
d['CimsRuntimeDir'] = os.environ['RUNTIME_DIR']
d.setdefault('Packages', {})['Dir'] = os.path.join(os.environ['RUNTIME_DIR'], 'pkg_files')
d.setdefault('CimsAuth', {})['JwtSecret'] = os.environ['JWT_SECRET']
ap = os.environ.get('ADMIN_PASS') or ''
if ap:
    for a in d['CimsAuth'].get('BuiltinAccounts', []):
        if a.get('login_id') == 'admin':
            a['password_sha256'] = hashlib.sha256(ap.encode()).hexdigest()
            a.pop('password', None)
json.dump(d, open(p, 'w'), ensure_ascii=False, indent=4)
open(p, 'a').write('\n')
print('  oam.json 구성 완료')
PYEOF

# ── uninstall 스크립트 생성 (install 의 대칭 — 언제든 단독 실행 가능) ──────
#    agent 설치본의 자체 uninstall.sh(agent+모듈+sudoers)와 이름이 겹치지 않게
#    uninstall-base.sh 로 생성하고, 있으면 그쪽에 위임 후 base 잔여를 정리한다.
cat > "$PREFIX/uninstall-base.sh" <<UNINST
#!/usr/bin/env bash
# CIMS base(OAM+Console+Agent) 완전 제거 — install.sh 가 생성.
#   sudo ./uninstall-base.sh [--yes]
set -uo pipefail
PREFIX="$PREFIX"
YES=0; [[ "\${1:-}" == "--yes" || "\${1:-}" == "-y" ]] && YES=1
echo "다음을 제거합니다:"
echo "  • OAM/Console 서비스 (systemd cims-oam.service 또는 start-oam 프로세스)"
echo "  • 이 서버의 agent + 배포된 모듈 (agent 의 uninstall.sh 위임)"
echo "  • \$PREFIX 전체 (패키지 저장소/runtime 포함)"
if [[ \$YES -ne 1 ]]; then
    read -r -p "계속할까요? [y/N] " _a
    [[ "\$_a" == y* || "\$_a" == Y* ]] || { echo "중단"; exit 1; }
fi

# 1) OAM 서비스 중지/해제
if [[ -f /etc/systemd/system/cims-oam.service ]]; then
    systemctl disable --now cims-oam.service 2>/dev/null || true
    rm -f /etc/systemd/system/cims-oam.service
    systemctl daemon-reload 2>/dev/null || true
    echo "✓ systemd cims-oam.service 제거"
else
    # start-oam.sh(nohup) 경로 — oam_app 프로세스 종료
    for _pid in \$(pgrep -f "\$PREFIX/modules/oam/.*oam_app.py" 2>/dev/null); do
        kill "\$_pid" 2>/dev/null || true
    done
fi

# 2) 로컬 agent + 모듈 — agent 설치본의 uninstall.sh 에 위임 (unit/sudoers/모듈 정리)
if [[ -f "\$PREFIX/uninstall.sh" ]]; then
    ( cd "\$PREFIX" && bash ./uninstall.sh --yes ) || true
fi
# agent 의 user systemd unit 이 남아있으면 직접 정지/해제 (Restart=always 부활 방지)
if id "$SVC_USER" >/dev/null 2>&1; then
    runuser -u "$SVC_USER" -- env XDG_RUNTIME_DIR="/run/user/\$(id -u "$SVC_USER")" \
        systemctl --user disable --now cims-agent.service 2>/dev/null || true
fi

# 3) install.sh 가 기동한 잔여 프로세스 일괄 종료 (oam/agent/모듈 — \$PREFIX 경로 기반)
_kill_prefix_procs() {
    local sig="\$1" _pid
    for _pid in \$(pgrep -f "\$PREFIX" 2>/dev/null); do
        # 자기 자신/부모(sudo 래퍼) 제외 — cmdline 에 PREFIX 가 포함되므로
        [[ "\$_pid" == "\$\$" || "\$_pid" == "\$PPID" ]] && continue
        kill "-\$sig" "\$_pid" 2>/dev/null || true
    done
}
_kill_prefix_procs TERM
sleep 2
_kill_prefix_procs KILL

# 4) base 잔여 전체 삭제
rm -rf "\$PREFIX"
echo "✓ CIMS base 제거 완료 (\$PREFIX — 관련 프로세스 종료 포함)"
UNINST
chmod +x "$PREFIX/uninstall-base.sh"

# 서비스 사용자 소유 — 이후 agent 가 modules/ 에 설치/업그레이드를 수행하므로
# 전체 트리를 서비스 사용자 소유로 (root 로 만든 디렉토리 교정).
chown -R "$SVC_USER":"$(id -gn "$SVC_USER")" "$PREFIX" 2>/dev/null || true

# ── 기동 (systemd 또는 start 스크립트) ───────────────────────────
START_CMD="/usr/bin/env python3 -u $OAM_ROOT/oam/src/oam_app.py"
if [[ $USE_SYSTEMD -eq 1 && -d /run/systemd/system && $EUID -eq 0 ]]; then
    cat > /etc/systemd/system/cims-oam.service <<UNIT
[Unit]
Description=CIMS OAM (base management plane: API + Console)
After=network-online.target

[Service]
Type=simple
User=$SVC_USER
WorkingDirectory=$OAM_ROOT/oam/src
ExecStart=$START_CMD
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
UNIT
    systemctl daemon-reload
    systemctl enable cims-oam.service >/dev/null 2>&1 || true
    if [[ $DO_START -eq 1 ]]; then
        systemctl restart cims-oam.service
        ok "systemd cims-oam.service 기동"
    else
        info "--no-start: 'systemctl start cims-oam' 으로 기동하세요"
    fi
else
    cat > "$PREFIX/start-oam.sh" <<SH
#!/usr/bin/env bash
cd "$OAM_ROOT/oam/src"
exec setsid nohup $START_CMD > "$OAM_ROOT/log/oam_stdout.log" 2>&1 < /dev/null &
SH
    chmod +x "$PREFIX/start-oam.sh"
    if [[ $DO_START -eq 1 ]]; then
        bash "$PREFIX/start-oam.sh"; disown 2>/dev/null || true
        ok "OAM 기동 (start 스크립트: $PREFIX/start-oam.sh)"
    else
        info "--no-start: $PREFIX/start-oam.sh 로 기동하세요"
    fi
fi

# ── 헬스 체크 ────────────────────────────────────────────────
if [[ $DO_START -eq 1 ]]; then
    for i in $(seq 1 20); do
        sleep 1
        code=$(curl -sk -o /dev/null -w '%{http_code}' "https://127.0.0.1:$PORT/" 2>/dev/null || echo 000)
        [[ "$code" == "200" ]] && break
    done
    if [[ "$code" == "200" ]]; then
        ok "콘솔 서빙 확인 (https://127.0.0.1:$PORT/ → 200)"
    else
        err "기동 확인 실패 (http $code) — 로그: $OAM_ROOT/oam/log/"
        exit 1
    fi
fi

# ── 로컬 agent 설치/기동 (이 서버도 콘솔에서 관리되도록) ────────────
AGENT_STATE="미설치 (--no-agent)"
if [[ $DO_AGENT -eq 1 && $DO_START -eq 1 ]]; then
    info "로컬 agent 등록/설치..."
    LOGIN_PW="${ADMIN_PASS:-1234}"
    TOK=$(curl -sk -X POST -H "Content-Type: application/json"           -d "{\"login_id\":\"admin\",\"password\":\"$LOGIN_PW\"}"           "https://127.0.0.1:$PORT/api/v1/auth/login" |           python3 -c "import sys,json;print(json.load(sys.stdin).get('token',''))" 2>/dev/null)
    HOSTNM=$(hostname -s 2>/dev/null || hostname)
    ENROLL_TOKEN=""
    if [[ -n "$TOK" ]]; then
        ENROLL_TOKEN=$(curl -sk -X POST -H "Authorization: Bearer $TOK"             -H "Content-Type: application/json" -d "{\"name\":\"$HOSTNM\"}"             "https://127.0.0.1:$PORT/api/v1/agents" |             python3 -c "import sys,json;print(json.load(sys.stdin).get('enrollment_token',''))" 2>/dev/null)
    fi
    if [[ -z "$ENROLL_TOKEN" ]]; then
        err "agent 등록 토큰 발급 실패 — 콘솔에서 수동으로 서버 추가 후 install-command 실행"
        AGENT_STATE="실패 (콘솔에서 수동 설치)"
    else
        _run_as() {  # 서비스 사용자로 실행 (root 인 경우 su)
            if [[ $EUID -eq 0 && "$SVC_USER" != "root" ]]; then
                su - "$SVC_USER" -c "$1"
            else
                bash -c "$1"
            fi
        }
        curl -sk "https://127.0.0.1:$PORT/install-agent.sh" -o /tmp/cims-install-agent.sh
        _run_as "cd '$PREFIX' && bash /tmp/cims-install-agent.sh --oam-url 'https://127.0.0.1:$PORT' --enrollment-token '$ENROLL_TOKEN' --name '$HOSTNM'" && ok "agent 번들 설치 ($PREFIX/agent — 패키지 저장소 서빙본)"
        # sudoers + linger 는 root 권한으로 직접 (init.sh 의 sudo 단계 선처리)
        if [[ $EUID -eq 0 && -f "$PREFIX/setup-sudoers.sh" ]]; then
            CIMS_AGENT_USER="$SVC_USER" bash "$PREFIX/setup-sudoers.sh" >/dev/null 2>&1 ||                 bash "$PREFIX/setup-sudoers.sh" >/dev/null 2>&1 || true
        fi
        if [[ $USE_SYSTEMD -eq 1 && -d /run/systemd/system ]]; then
            if _run_as "cd '$PREFIX' && ./init.sh"; then
                AGENT_STATE="실행 중 (systemd --user cims-agent.service)"
            else
                AGENT_STATE="설치됨 — 기동 실패 ($PREFIX/init.sh 수동 실행)"
            fi
        else
            # systemd 미사용 — enroll 후 nohup 직접 기동
            _run_as "cd '$PREFIX' && CIMS_ENROLLMENT_TOKEN='$ENROLL_TOKEN'                 python3 ./agent/cims_agent.py --oam-url 'https://127.0.0.1:$PORT'                 --state-dir ./state --name '$HOSTNM' --enroll-only" || true
            _run_as "cd '$PREFIX' && setsid nohup python3 ./agent/cims_agent.py                 --oam-url 'https://127.0.0.1:$PORT' --state-dir ./state --name '$HOSTNM'                 > ./agent-stdout.log 2>&1 < /dev/null &"
            sleep 3
            AGENT_STATE="실행 중 (nohup — systemd 미사용 환경)"
        fi
    fi
elif [[ $DO_AGENT -eq 1 ]]; then
    AGENT_STATE="미기동 (--no-start)"
fi

cat <<DONE

────────────────────────────────────────────────────────────
 CIMS base 설치 완료
   프로세스:
     · oam     : 실행 중 — API + 콘솔 동시 서빙 (HTTPS :$PORT)
     · console : 별도 프로세스 없음 — oam 이 정적 서빙 (위 포트)
     · agent   : $AGENT_STATE
   콘솔   : https://<이 서버 IP>:$PORT/   (브라우저 인증서 경고는 self-signed 때문)
   로그인 : admin / $([[ -n "$ADMIN_PASS" ]] && echo '<--admin-pass 로 설정한 비밀번호>' || echo '1234  ← 상용에서는 변경 권장 (--admin-pass)')
   다음   : 콘솔 → 시스템 > 시스템/인프라 → ＋시스템 추가 → 각 서버에
            install-command 실행(agent 설치) → 패키지 등록/설치/설정
   제거   : sudo $PREFIX/uninstall-base.sh
────────────────────────────────────────────────────────────
DONE
