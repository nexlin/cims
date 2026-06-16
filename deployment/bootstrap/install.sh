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
#     --server-name N  OAM 호스트(이 서버) 표시 이름 (기본 hostname)
#     --mgmt-ip IP     관리(mgmt) IP — agent↔OAM 통신 기준 (AgentOamUrl/Mgmt.Cidr; 기본 첫 global IP)
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
SERVER_NAME=""      # OAM 호스트(=이 서버) 표시 이름. 미지정 시 hostname.
MGMT_IP=""          # 관리(mgmt) IP — agent↔OAM 통신 기준. AgentOamUrl/Mgmt.Cidr 에 반영.
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
        --server-name) SERVER_NAME="$2"; shift 2 ;;
        --mgmt-ip)     MGMT_IP="$2"; shift 2 ;;
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
warn() { echo -e "\033[0;33m[WARN]\033[0m  $*" >&2; }
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
        read -r -p "  [1/6] 설치 경로 [$PREFIX]: " _in
        [[ -n "$_in" ]] && PREFIX="$_in"
    fi
    # [2] 콘솔/OAM HTTPS 포트 (단일 오리진 — 콘솔 웹과 API/agent 통신이 같은 포트)
    if [[ $PORT_GIVEN -eq 0 ]]; then
        while :; do
            read -r -p "  [2/6] 콘솔/OAM HTTPS 포트 (웹·API 단일) [$PORT]: " _in
            [[ -z "$_in" ]] && break
            if [[ "$_in" =~ ^[0-9]+$ ]] && (( _in >= 1 && _in <= 65535 )); then
                PORT="$_in"; break
            fi
            echo "      포트는 1~65535 숫자여야 합니다"
        done
    fi
    # [3] 서버 명 (이 OAM 호스트의 표시 이름)
    if [[ -z "$SERVER_NAME" ]]; then
        _name_def=$(hostname -s 2>/dev/null || hostname)
        read -r -p "  [3/6] 서버 명 [$_name_def]: " _in
        SERVER_NAME="${_in:-$_name_def}"
    fi
    # [4] 관리(mgmt) IP — agent↔OAM 통신 기준. 후보 IP 제시.
    if [[ -z "$MGMT_IP" ]]; then
        _cands=$(ip -o -4 addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | tr '\n' ' ' || true)
        [[ -z "$_cands" ]] && _cands=$(hostname -I 2>/dev/null)
        _ip_def=$(echo $_cands | awk '{print $1}')
        echo "        후보 IP: ${_cands:-(감지 실패 — 직접 입력)}"
        read -r -p "  [4/6] 관리(mgmt) IP [${_ip_def:-직접입력}]: " _in
        MGMT_IP="${_in:-$_ip_def}"
    fi
    # [5] admin 비밀번호 최초 등록 (필수 — 미입력 시 반복)
    if [[ -z "$ADMIN_PASS" ]]; then
        while :; do
            read -r -s -p "  [5/6] admin 비밀번호 (최초 등록, 4자 이상): " _p1; echo
            if [[ ${#_p1} -lt 4 ]]; then echo "      4자 이상 입력하세요"; continue; fi
            read -r -s -p "        비밀번호 확인: " _p2; echo
            [[ "$_p1" == "$_p2" ]] && { ADMIN_PASS="$_p1"; break; }
            echo "      일치하지 않습니다 — 다시 입력"
        done
    fi
    # [6] 로컬 agent 자동 설치
    read -r -p "  [6/6] 이 서버의 agent 자동 설치/기동 [Y/n]: " _in
    [[ "$_in" == n* || "$_in" == N* ]] && DO_AGENT=0
    echo ""
    echo "── 설치 요약 ────────────────────────────────────────────────"
    echo "    설치 경로     : $PREFIX"
    echo "    HTTPS 포트    : $PORT  (콘솔 웹 + API + agent 통신 단일 오리진)"
    echo "    서버 명       : $SERVER_NAME"
    echo "    관리(mgmt) IP : ${MGMT_IP:-(미지정)}"
    echo "    admin 비밀번호: (입력됨)"
    echo "    서비스 사용자 : $SVC_USER"
    echo "    로컬 agent    : $([[ $DO_AGENT -eq 1 ]] && echo 설치 || echo 생략)"
    read -r -p "  진행할까요? [Y/n]: " _in
    [[ "$_in" == n* || "$_in" == N* ]] && { echo "중단"; exit 1; }
    echo ""
elif [[ -z "$ADMIN_PASS" ]]; then
    err "admin 비밀번호 미설정 — 기본값(1234)으로 진행합니다. 상용에서는 --admin-pass 필수!"
fi

# 서버명/mgmt IP 기본값 보정 (비대화식·플래그 경로 포함 — 항상 값 보장)
[[ -z "$SERVER_NAME" ]] && SERVER_NAME=$(hostname -s 2>/dev/null || hostname)
if [[ -z "$MGMT_IP" ]]; then
    MGMT_IP=$(ip -o -4 addr show scope global 2>/dev/null | awk 'NR==1{print $4}' | cut -d/ -f1 || true)
    [[ -z "$MGMT_IP" ]] && MGMT_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
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
# 시크릿 격리 (runtime store v2 P1) — 시크릿은 runtime/_secrets/ 0700 에 모은다.
# (데이터 도메인과 분리 → 백업/동기화 범위에서 제외, 권한 최소화)
SECRETS_DIR="$RUNTIME_DIR/_secrets"
mkdir -p "$SECRETS_DIR"; chmod 700 "$SECRETS_DIR"
JWT_SECRET_FILE="$SECRETS_DIR/jwt_secret"
# 마이그레이션 — 구 위치(runtime/.jwt_secret)에 있으면 보존 이동 (기존 토큰 유효 유지).
if [[ ! -f "$JWT_SECRET_FILE" && -f "$RUNTIME_DIR/.jwt_secret" ]]; then
    mv "$RUNTIME_DIR/.jwt_secret" "$JWT_SECRET_FILE"
fi
if [[ ! -f "$JWT_SECRET_FILE" ]]; then
    openssl rand -base64 32 > "$JWT_SECRET_FILE"
fi
chmod 600 "$JWT_SECRET_FILE"
PY=python3 OAM_ROOT="$OAM_ROOT" RUNTIME_DIR="$RUNTIME_DIR" PORT="$PORT" \
JWT_SECRET="$(cat "$JWT_SECRET_FILE")" MGMT_IP="$MGMT_IP" \
ADMIN_PASS="$ADMIN_PASS" python3 - <<'PYEOF'
import hashlib, json, os
p = os.path.join(os.environ['OAM_ROOT'], 'oam', 'config', 'oam.json')
d = json.load(open(p))
port = int(os.environ['PORT'])
d['Server'] = {'Ip': '0.0.0.0', 'Port': port}
# 관리(mgmt) IP — agent↔OAM 통신 기준. AgentOamUrl(콘솔 install-command)·Mgmt.Cidr(/24) 반영.
mgmt = (os.environ.get('MGMT_IP') or '').strip()
if mgmt:
    d['Server']['AgentOamUrl'] = f"https://{mgmt}:{port}"
    d.setdefault('Mgmt', {})['Cidr'] = mgmt.rsplit('.', 1)[0] + '.0/24'
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

# 1) 로컬 agent 먼저 중지 — watchdog 가 OAM/모듈을 재기동하지 못하게 (Restart=always 부활 방지)
#    (OAM 이 agent 감독 대상이므로 OAM 보다 먼저 watchdog 를 끈다)
if id "$SVC_USER" >/dev/null 2>&1; then
    runuser -u "$SVC_USER" -- env XDG_RUNTIME_DIR="/run/user/\$(id -u "$SVC_USER")" \
        systemctl --user disable --now cims-agent.service 2>/dev/null || true
fi
if [[ -f "\$PREFIX/uninstall.sh" ]]; then
    ( cd "\$PREFIX" && bash ./uninstall.sh --yes ) || true
fi

# 2) OAM 중지 (agent/cims-svc·부트스트랩 nohup 프로세스 + 구버전 systemd unit 잔재)
if [[ -f /etc/systemd/system/cims-oam.service ]]; then
    systemctl disable --now cims-oam.service 2>/dev/null || true
    rm -f /etc/systemd/system/cims-oam.service
    systemctl daemon-reload 2>/dev/null || true
    echo "✓ systemd cims-oam.service 제거 (구버전)"
fi
for _pid in \$(pgrep -f "\$PREFIX/modules/oam/.*oam_app.py" 2>/dev/null); do
    kill "\$_pid" 2>/dev/null || true
done

# 3) install.sh 가 기동한 잔여 프로세스 일괄 종료 (oam/agent/모듈 — \$PREFIX 경로 기반)
#    보호 PID = 자기 자신 + 모든 조상 (sudo 래퍼 체인 — grandparent 까지).
#    \$\$/\$PPID 만 제외하면 조부모 sudo 를 죽여 rm 도달 전 自害(Killed)함.
_PROTECT_PIDS=" \$\$ "
_pp=\$\$
while :; do
    _pp=\$(ps -o ppid= -p "\$_pp" 2>/dev/null | tr -d ' ')
    [[ -z "\$_pp" || "\$_pp" == "0" || "\$_pp" == "1" ]] && break
    _PROTECT_PIDS="\$_PROTECT_PIDS\$_pp "
done
_kill_prefix_procs() {
    local sig="\$1" _pid
    for _pid in \$(pgrep -f "\$PREFIX" 2>/dev/null); do
        case "\$_PROTECT_PIDS" in *" \$_pid "*) continue ;; esac
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

# ── _run_as: 서비스 사용자(cims)로 실행 — OAM·agent 모두 cims 소유 프로세스로 ──
#   설계: OAM 은 csp/cmp 등 다른 모듈과 동일하게 agent 의 cims-svc + watchdog 가 감독한다.
#   단 agent enroll 에는 OAM 이 먼저 떠 있어야 하므로, 여기서는 OAM 을 1회 "부트스트랩"
#   기동만 하고 — agent 설치 후 cims-svc 로 인계(pidfile + supervised.json)한다.
#   → root systemd cims-oam.service 는 더 이상 만들지 않는다 (모듈 기동 방식과 일관).
_run_as() {
    if [[ $EUID -eq 0 && "$SVC_USER" != "root" ]]; then
        su - "$SVC_USER" -c "$1"
    else
        bash -c "$1"
    fi
}

# ── OAM 부트스트랩 기동 (agent 인계 전까지의 임시 기동, cims 소유) ──────
cat > "$PREFIX/start-oam.sh" <<SH
#!/usr/bin/env bash
# OAM 부트스트랩 기동 — 정식 감독은 agent watchdog + cims-svc (start oam).
cd "$OAM_ROOT/oam/src"
setsid nohup /usr/bin/env python3 -u "$OAM_ROOT/oam/src/oam_app.py" > "$OAM_ROOT/log/oam_stdout.log" 2>&1 < /dev/null &
SH
chmod +x "$PREFIX/start-oam.sh"
if [[ $DO_START -eq 1 ]]; then
    _run_as "bash '$PREFIX/start-oam.sh'"
    ok "OAM 부트스트랩 기동 (agent 설치 후 cims-svc 감독으로 인계)"
else
    info "--no-start: $PREFIX/start-oam.sh 로 기동하세요"
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
# best-effort: OAM/Console 는 이미 설치·기동 완료. 이 블록의 어떤 단계가 실패해도
# 설치 전체를 무음 중단시키지 않는다 — errexit 를 잠시 해제하고 단계별 진단을 남긴다.
# 갓 (재)기동한 OAM 은 / 가 200 이어도 첫 요청이 일시 실패할 수 있어 각 단계를 재시도한다.
# (구버전 footgun: set -euo pipefail 아래 curl|python 파이프가 nonzero 면 메시지 없이 즉시 exit)
AGENT_STATE="미설치 (--no-agent)"
if [[ $DO_AGENT -eq 1 && $DO_START -eq 1 ]]; then
    info "로컬 agent 등록/설치..."
    set +e
    _HTTP_FILE=$(mktemp)   # _api 가 HTTP status 를 여기 기록 (파이프 subshell 에서도 보존)
    LOGIN_PW="${ADMIN_PASS:-1234}"
    HOSTNM="${SERVER_NAME:-$(hostname -s 2>/dev/null || hostname)}"   # OAM 호스트 표시 이름 (설치 시 입력값)

    # API 호출 헬퍼 — 응답 본문은 stdout, HTTP status code 는 $_HTTP_FILE 에 담는다.
    _api() {
        local method="$1" path="$2" data="${3:-}" auth="${4:-}" bf
        bf=$(mktemp)
        local cargs=(-sk -o "$bf" -w '%{http_code}' -X "$method" -H "Content-Type: application/json")
        [[ -n "$auth" ]] && cargs+=(-H "Authorization: Bearer $auth")
        [[ -n "$data" ]] && cargs+=(-d "$data")
        curl "${cargs[@]}" "https://127.0.0.1:$PORT$path" 2>/dev/null > "$_HTTP_FILE"
        cat "$bf" 2>/dev/null
        rm -f "$bf"
    }
    _http() { cat "$_HTTP_FILE" 2>/dev/null || echo "?"; }
    _jget() { python3 -c "import sys,json
try: print((json.load(sys.stdin) or {}).get('$1','') or '')
except Exception: print('')" 2>/dev/null; }

    # 1) admin 로그인 (transient 대비 최대 6회 재시도)
    TOK=""
    for _i in 1 2 3 4 5 6; do
        TOK=$(_api POST /api/v1/auth/login "{\"login_id\":\"admin\",\"password\":\"$LOGIN_PW\"}" | _jget token)
        [[ -n "$TOK" ]] && break
        sleep 1
    done

    ENROLL_TOKEN=""
    if [[ -z "$TOK" ]]; then
        err "admin 로그인 실패 (HTTP $(_http)) — agent 자동설치 건너뜀. 콘솔에서 수동 설치하세요."
        AGENT_STATE="실패 (로그인 — 콘솔 수동설치)"
    else
        # 2) agent 등록 + enrollment token (신규 생성 → 이름 중복(409)이면 기존 레코드 삭제 후 재생성)
        #    재실행 대비: 같은 이름 레코드가 남아 있으면(이전 설치 잔재) DELETE 후 새로 만든다.
        #    (OAM 의 GET /agents 응답은 {"items":[...]}.) regenerate-token 엔드포인트도
        #    정상 동작하지만(미만료 토큰=409 still_valid / 만료·무토큰=200 재발급) 재설치 시
        #    유효 토큰이 남아 있으면 409 로 새 토큰을 못 받는다 → delete+recreate 가
        #    토큰 상태와 무관히 항상 fresh 토큰을 주는 멱등 경로라 견고(검증된 201 재사용).
        for _i in 1 2 3 4 5 6; do
            ENROLL_TOKEN=$(_api POST /api/v1/agents "{\"name\":\"$HOSTNM\"}" "$TOK" | _jget enrollment_token)
            [[ -n "$ENROLL_TOKEN" ]] && break
            EID=$(_api GET "/api/v1/agents" "" "$TOK" | python3 -c "import sys,json
try:
    d=json.load(sys.stdin)
    ags=(d.get('items') or d.get('agents') or []) if isinstance(d,dict) else d
    print(next((str(a.get('id')) for a in ags if isinstance(a,dict) and a.get('name')=='$HOSTNM'),''))
except Exception: print('')" 2>/dev/null)
            if [[ -n "$EID" ]]; then
                info "기존 등록 agent(id=$EID) 발견 — 삭제 후 재등록"
                _api DELETE "/api/v1/agents/$EID" "" "$TOK" >/dev/null 2>&1
                ENROLL_TOKEN=$(_api POST /api/v1/agents "{\"name\":\"$HOSTNM\"}" "$TOK" | _jget enrollment_token)
                [[ -n "$ENROLL_TOKEN" ]] && break
            fi
            sleep 1
        done
        if [[ -z "$ENROLL_TOKEN" ]]; then
            err "agent 등록 토큰 발급 실패 (HTTP $(_http)) — 콘솔에서 수동으로 서버 추가 후 install-command 실행"
            AGENT_STATE="실패 (토큰 발급 — 콘솔 수동설치)"
        else
            # (_run_as 는 OAM 부트스트랩 기동부에서 이미 정의됨 — 서비스 사용자로 실행)
            # 3) install-agent.sh 다운로드 (최대 6회, HTTP 200 + 비어있지 않은 응답 확인)
            #    ⚠️ /tmp(sticky·world-writable)의 "타인 소유" 파일에 root 가 -o(O_CREAT)하면
            #    fs.protected_regular(=1/2)가 차단(curl rc≠0) → 매번 다운로드 실패.
            #    따라서 PREFIX 하위(cims 소유·non-sticky)로 받는다.
            _IA="$PREFIX/.cims-install-agent.sh"
            _dl_ok=0; _dl_http=000
            for _i in 1 2 3 4 5 6; do
                _dl_http=$(curl -sk -o "$_IA" -w '%{http_code}' "https://127.0.0.1:$PORT/install-agent.sh" 2>/dev/null)
                [[ "$_dl_http" == "200" && -s "$_IA" ]] && { _dl_ok=1; break; }
                sleep 1
            done
            chmod 0644 "$_IA" 2>/dev/null || true   # su - 로 실행할 cims 가 읽을 수 있도록
            if [[ $_dl_ok -ne 1 ]]; then
                err "install-agent.sh 다운로드 실패 (HTTP $_dl_http) — agent 미설치 (콘솔 수동설치)"
                AGENT_STATE="실패 (install-agent.sh 다운로드)"
            elif _run_as "cd '$PREFIX' && bash '$_IA' --oam-url 'https://127.0.0.1:$PORT' --enrollment-token '$ENROLL_TOKEN' --name '$HOSTNM'"; then
                ok "agent 번들 설치 ($PREFIX/agent — 패키지 저장소 서빙본)"
                # OAM 을 cims-svc 로 인계 — 부트스트랩 nohup 을 정식 감독 프로세스로 교체.
                #   start_oam 의 kill_stray 가 부트스트랩 OAM(같은 포트/경로)을 정리하고
                #   pidfile($OAM_ROOT/run/oam.pid)을 남긴다 → 중복기동·고아 방지.
                info "OAM 을 agent 관리(cims-svc)로 인계..."
                # cims-svc 의 상세 상태 출력(=== CIMS 상태 ===/[검증 대상]/[TB] 등 개발용)은
                # 설치 로그로만 남기고 화면엔 결과만 표시한다 (상용 설치 출력 정돈).
                if _run_as "CIMS_DIST_DIR='$OAM_ROOT' CIMS_PYTHON=python3 '$PREFIX/agent/bin/cims-svc' start oam" \
                        >> "$OAM_ROOT/log/oam_handover.log" 2>&1; then
                    ok "OAM cims-svc 감독 전환 완료 (pidfile + watchdog)"
                else
                    warn "OAM cims-svc 인계 실패 — agent watchdog 가 후속 회수 (상세: $OAM_ROOT/log/oam_handover.log)"
                fi
                # agent watchdog 감독 등록: oam → versioned 모듈 경로 (supervise_tick 가 읽음)
                mkdir -p "$PREFIX/run"
                printf '{"oam": "%s"}\n' "$OAM_ROOT" > "$PREFIX/run/supervised.json"
                # run/ 디렉터리째 서비스 사용자 소유로 — agent(cims)가 managed_ips.json 등을
                # 여기 기록한다. (root 가 mkdir 하면 cims 가 파일 생성 불가 → Permission denied)
                chown -R "$SVC_USER":"$(id -gn "$SVC_USER")" "$PREFIX/run" 2>/dev/null || true
                # sudoers + linger 는 root 권한으로 직접 (init.sh 의 sudo 단계 선처리)
                if [[ $EUID -eq 0 && -f "$PREFIX/setup-sudoers.sh" ]]; then
                    CIMS_AGENT_USER="$SVC_USER" bash "$PREFIX/setup-sudoers.sh" >/dev/null 2>&1 || \
                        bash "$PREFIX/setup-sudoers.sh" >/dev/null 2>&1 || true
                fi
                if [[ $USE_SYSTEMD -eq 1 && -d /run/systemd/system ]]; then
                    if _run_as "cd '$PREFIX' && ./init.sh"; then
                        AGENT_STATE="실행 중 (systemd --user cims-agent.service)"
                    else
                        AGENT_STATE="설치됨 — 기동 실패 ($PREFIX/init.sh 수동 실행)"
                    fi
                else
                    # systemd 미사용 — enroll 후 nohup 직접 기동
                    _run_as "cd '$PREFIX' && CIMS_ENROLLMENT_TOKEN='$ENROLL_TOKEN' python3 ./agent/cims_agent.py --oam-url 'https://127.0.0.1:$PORT' --state-dir ./state --name '$HOSTNM' --enroll-only" || true
                    _run_as "cd '$PREFIX' && setsid nohup python3 ./agent/cims_agent.py --oam-url 'https://127.0.0.1:$PORT' --state-dir ./state --name '$HOSTNM' > ./agent-stdout.log 2>&1 < /dev/null &"
                    sleep 3
                    AGENT_STATE="실행 중 (nohup — systemd 미사용 환경)"
                fi
            else
                err "install-agent.sh 실행 실패 — 콘솔에서 수동 설치하세요 (install-command)"
                AGENT_STATE="실패 (install-agent.sh 실행)"
            fi
        fi
    fi
    rm -f "$_HTTP_FILE"
    set -e
elif [[ $DO_AGENT -eq 1 ]]; then
    AGENT_STATE="미기동 (--no-start)"
fi

cat <<DONE

────────────────────────────────────────────────────────────
 CIMS base 설치 완료
   프로세스:
     · oam     : 실행 중 — agent(cims-svc) 감독, API+콘솔 서빙 (HTTPS :$PORT)
     · console : 별도 프로세스 없음 — oam 이 정적 서빙 (위 포트)
     · agent   : $AGENT_STATE
   콘솔   : https://<이 서버 IP>:$PORT/   (브라우저 인증서 경고는 self-signed 때문)
   로그인 : admin / $([[ -n "$ADMIN_PASS" ]] && echo '<--admin-pass 로 설정한 비밀번호>' || echo '1234  ← 상용에서는 변경 권장 (--admin-pass)')
   다음   : 콘솔 → 시스템 > 시스템/인프라 → ＋시스템 추가 → 각 서버에
            install-command 실행(agent 설치) → 패키지 등록/설치/설정
   제거   : sudo $PREFIX/uninstall-base.sh
────────────────────────────────────────────────────────────
DONE
