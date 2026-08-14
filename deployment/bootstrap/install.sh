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
#     --port N         OAM bind 포트 (기본 4419)
#     --admin-pass PW  내장 admin 비밀번호 설정 (기본 1234 — 상용은 변경 권장)
#     --server-name N  OAM 호스트(이 서버) 표시 이름 (기본 hostname)
#     --mgmt-ip IP     관리(mgmt) IP — agent↔OAM 통신 기준 (AgentOamUrl/Mgmt.Cidr; 기본 첫 global IP)
#     --no-systemd     systemd 미사용 (start 스크립트 생성)
#     --no-start       설치만 하고 기동하지 않음
#     --no-agent       이 서버의 agent 자동 설치/기동 생략
#     --batch          대화식 입력 생략 (옵션/기본값만 사용 — 자동화용)
#   관리평면 이중화 — 두 번째 노드 합류 (docs/design/features/oam_ha.md §9):
#     --join                  합류 모드 (peer 에서 그룹 공통 신원 수령, OAM 미기동)
#     --peer-url URL          기존 OAM 주소 (예: https://121.161.164.140:4419)
#     --join-token TOKEN      1회용 합류 토큰 (콘솔/API: POST /api/v1/ha/join-token)
#     --runtime-dir DIR       관리 store 경로 (공유 마운트 하위)
#     --runtime-mount DIR     공유 store 마운트 지점 (mount guard 기준)
#   옵션 없이 실행하면 설치 경로/포트/admin 비밀번호를 단계별로 묻는다.
#   제거: sudo <prefix>/uninstall-base.sh [--yes]
#     --user USER      서비스 사용자 (기본: sudo 호출자) — agent/OAM 프로세스 소유자
set -euo pipefail

PREFIX=/opt/cims-agent
PORT=4419          # OAM 실제 bind 포트 (비특권 >=1024 — 비root 프로세스)
ADMIN_PASS=""
SERVER_NAME=""      # OAM 호스트(=이 서버) 표시 이름. 미지정 시 hostname.
MGMT_IP=""          # 관리(mgmt) IP — agent↔OAM 통신 기준. AgentOamUrl/Mgmt.Cidr 에 반영.
USE_SYSTEMD=1
DO_START=1
DO_AGENT=1
BATCH=0
JOIN=0              # 관리평면 합류 모드 (두 번째 OAM 노드)
PEER_URL=""         # 기존 OAM 주소 (신원 수령 + agent enroll 대상)
JOIN_TOKEN=""       # 1회용 합류 토큰
STORE_DIR=""        # 관리 store 경로 (공유 마운트 하위) — CimsRuntimeDir
STORE_MOUNT=""      # 공유 store 마운트 지점 — CimsRuntimeMount (mount guard 기준)
# 서비스 사용자 — sudo 호출자 (agent/OAM 프로세스 소유자. 모듈 설치 경로 쓰기 주체)
SVC_USER="${SUDO_USER:-$(id -un)}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)     PREFIX="$2"; shift 2 ;;
        --port)       PORT="$2"; shift 2 ;;       # OAM bind 포트
        --admin-pass) ADMIN_PASS="$2"; shift 2 ;;
        --server-name) SERVER_NAME="$2"; shift 2 ;;
        --mgmt-ip)     MGMT_IP="$2"; shift 2 ;;
        --no-systemd) USE_SYSTEMD=0; shift ;;
        --no-start)   DO_START=0; shift ;;
        --no-agent)   DO_AGENT=0; shift ;;
        --batch)      BATCH=1; shift ;;
        --join)         JOIN=1; BATCH=1; shift ;;      # 합류는 항상 비대화식
        --peer-url)     PEER_URL="$2"; shift 2 ;;
        --join-token)   JOIN_TOKEN="$2"; shift 2 ;;
        --runtime-dir)  STORE_DIR="$2"; shift 2 ;;
        --runtime-mount) STORE_MOUNT="$2"; shift 2 ;;
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

# ── 권한 가드 — 반드시 "일반 계정에서 sudo 로" 실행 (부분 설치 차단) ──────────
#   이 인스톨러는 sudoers/linger/서비스 IP/소유권 변경 등 root 작업과, agent/OAM 을
#   일반(서비스) 계정 소유로 띄우는 작업을 함께 수행한다. sudo 없이 실행하면 일부
#   단계만 진행돼(부분 설치) 추적이 어렵다 → 권한이 부족하면 즉시 종료한다.
if [[ $EUID -ne 0 ]]; then
    err "root 권한이 필요합니다 — 일반 계정에서 'sudo $0 [옵션]' 으로 다시 실행하세요."
    err "(sudo 없이 실행하면 sudoers·linger·서비스 IP 등 권한 작업이 누락된 채 부분 설치됩니다)"
    exit 1
fi
# 보안 정책: root 계정에서 직접 실행 금지 — 반드시 "일반 계정 + sudo".
#   sudo 는 호출자를 SUDO_USER 에 남긴다. 비어있거나 root 면 = root 로그인(또는 sudo 미경유)
#   → 거부. (root 상시 로그인 운영을 막고, 책임 추적 가능한 일반 계정 경유를 강제)
if [[ -z "${SUDO_USER:-}" || "$SUDO_USER" == "root" ]]; then
    err "보안 정책상 root 계정에서 직접 실행할 수 없습니다 — 일반 계정에서 'sudo $0 [옵션]' 으로 실행하세요."
    exit 1
fi
# 서비스 계정(agent/OAM 프로세스 소유자)도 root 가 되면 안 됨 (--user root 방지).
if [[ "$SVC_USER" == "root" ]]; then
    err "서비스 계정이 root 입니다 — agent/OAM 은 일반 계정 소유로 동작해야 합니다 ('--user <일반계정>')."
    exit 1
fi

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
    # [2] OAM bind 포트 — OAM 이 실제 listen(비root → 1024~65535). 브라우저는 https://<IP>:<포트>.
    #     (443 포트 생략 접속은 시스템/인프라의 포트 redirect 기능으로 별도 — 부트스트랩 영역 아님)
    if [[ $PORT_GIVEN -eq 0 ]]; then
        while :; do
            read -r -p "  [2/6] OAM bind 포트 [$PORT]: " _in
            [[ -z "$_in" ]] && break
            if [[ "$_in" =~ ^[0-9]+$ ]] && (( _in >= 1024 && _in <= 65535 )); then
                PORT="$_in"; break
            fi
            echo "      bind 포트는 1024~65535 (비root 프로세스라 특권 포트 불가)"
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
    echo "    OAM bind 포트 : $PORT  (브라우저: https://<IP>:$PORT)"
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

# ── bind 포트 보정 ───────────────────────────────────────────────────────────
#   OAM 은 비root(cims)라 특권 포트(<1024) 직접 bind 불가 → 4419 로 보정.
#   (443 포트 생략 접속이 필요하면 시스템/인프라의 포트 redirect 기능으로 별도 설정)
if (( PORT < 1024 )); then
    warn "bind 포트 $PORT 는 특권 포트 — 비root OAM 은 직접 bind 불가 → 4419 로 보정."
    PORT=4419
fi

# 권한 체크는 스크립트 상단 가드(반드시 sudo)에서 이미 강제됨 — 여기서는 생략.

# || true: 매치 없을 때 ls(exit 2)+pipefail 이 set -e 로 스크립트를 죽이지 않게.
#   (console 은 oam-base 동봉이라 별도 tarball 미존재 → _latest console 빈 결과 정상)
_latest() { ls -1 "$PKG_DIR"/$1-[0-9]*.tar.gz 2>/dev/null | sort -V | tail -1 || true; }
# oam_base_service_split — console 은 oam-base 패키지에 동봉. 별도 console tarball 은
#   선택(있으면 하위호환으로 별도 모듈 설치, 없으면 oam 동봉 console 서빙).
OAM_TAR=$(_latest oam); AGT_TAR=$(_latest agent); CON_TAR=$(_latest console)
for v in OAM_TAR AGT_TAR; do
    [[ -n "${!v}" ]] || { err "packages/ 에 ${v%_TAR} tarball 없음"; exit 1; }
done
_ver() { basename "$1" .tar.gz | sed 's/^[a-z]*-//'; }
OAM_VER=$(_ver "$OAM_TAR"); AGT_VER=$(_ver "$AGT_TAR")
info "설치 구성: oam-base $OAM_VER (console 동봉) / agent $AGT_VER → $PREFIX (HTTPS :$PORT)"

# ── 레이아웃 (버전 단위 설치 + current 심볼릭 — agent 배포 체계와 동일, 모듈은 modules/ 하위) ──
MODULES_DIR="$PREFIX/modules"
OAM_ROOT="$MODULES_DIR/oam/$OAM_VER"
OAM_CURRENT="$MODULES_DIR/oam/current"     # 활성 버전 통로 (CIMS_DIST_DIR / supervised)
RUNTIME_DIR="$MODULES_DIR/oam/runtime"     # 버전 무관 영속 store (업그레이드 생존)
mkdir -p "$OAM_ROOT" "$RUNTIME_DIR"

info "패키지 전개..."
tar xzf "$OAM_TAR" -C "$OAM_ROOT"
# current 심볼릭 flip (원자적, 상대 타겟) — 이후 agent 배포 체계가 동일 방식으로 인수
ln -sfn "$OAM_VER" "$MODULES_DIR/oam/.current.tmp"
mv -Tf "$MODULES_DIR/oam/.current.tmp" "$OAM_CURRENT"
# agent 는 전개하지 않음 — 설치 에셋(/install-agent.sh, /cims_agent.py,
# /agent-bundle.tar.gz)의 SoT 는 패키지 저장소(seed 자동 등록). 버전별로
# 보관되어 다른 모듈과 동일하게 업데이트/롤백 관리.
mkdir -p "$OAM_ROOT/config" "$OAM_ROOT/run" "$OAM_ROOT/log"

# seed 패키지 — OAM 첫 부팅 시 패키지 저장소 자동 등록 (1단계 산출물도
# 콘솔에서 업데이트 가능한 패키지로 보이도록; 서비스 모듈은 3단계에서 등록)
mkdir -p "$OAM_ROOT/oam/seed_packages"
cp -f "$OAM_TAR" "$AGT_TAR" "$OAM_ROOT/oam/seed_packages/"

# ── TLS 인증서 (self-signed; 재설치·업그레이드 시 보존) ─────────────────────
# 버전무관 runtime 위치에 생성 — oam 버전업 시 새 버전 디렉터리엔 cert 가 없어 평문 기동
# → self-upgrade health-gate(HTTPS) 롤백 / oam-svc 평문→게이트웨이 502 가 났다. runtime 은
# 업그레이드 생존(영속 store) → base/oam-svc 둘 다 _resolve_oam_cert 로 여기서 읽는다.
CERT_DIR="$RUNTIME_DIR/cert"
mkdir -p "$CERT_DIR"
# 구 레이아웃(버전 디렉터리 cert)에서 1회 이관 — 기존 토큰/인증서 유지.
if [[ ( ! -f "$CERT_DIR/server.key" || ! -f "$CERT_DIR/server.crt" ) \
      && -f "$OAM_ROOT/oam/cert/server.key" && -f "$OAM_ROOT/oam/cert/server.crt" ]]; then
    cp -p "$OAM_ROOT/oam/cert/server.key" "$OAM_ROOT/oam/cert/server.crt" "$CERT_DIR/"
    ok "구 cert(버전 디렉터리) → runtime 이관"
fi
if [[ ! -f "$CERT_DIR/server.key" || ! -f "$CERT_DIR/server.crt" ]]; then
    HOSTNM=$(hostname -f 2>/dev/null || hostname)
    # SAN 에 관리 IP 도 포함 — 없으면 OAM 이 기동 시 SAN 부족으로 판단해 그룹 CA 로 재발급한다
    # (동작은 같지만 불필요한 재발급·인증서 교체를 아낀다). VIP 는 이중화 구성 시 추가된다
    # (배포 설정 Server.CertSans → OAM 이 그룹 CA 로 재발급).
    _SAN="DNS:${HOSTNM},IP:127.0.0.1"
    [[ -n "$MGMT_IP" ]] && _SAN="${_SAN},IP:${MGMT_IP}"
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -subj "/CN=${HOSTNM}/O=CIMS" \
        -addext "subjectAltName=${_SAN}" \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" 2>/dev/null
    chmod 600 "$CERT_DIR/server.key"
    ok "self-signed TLS 인증서 생성 (CN=$HOSTNM, SAN=${_SAN}) — 상용 인증서는 $CERT_DIR 에 교체"
else
    ok "기존 TLS 인증서 보존"
fi

# ── 합류 모드: peer 에서 그룹 공통 신원 수령 ─────────────────────────
# 두 노드가 같은 신원(JwtSecret·admin·CA)을 갖는 것이 이중화의 전제다 — 다르면 절체 후
# 전 세션 무효 + 모듈 401. 콘솔 배포 경로로는 `_infra` 값이 전달되지 않아 성립하지 않으므로
# (oam_ha.md §9) 여기서 명시적으로 받아온다. 개인키는 **1회 복사**이며 공유 볼륨에 두지 않는다.
JOIN_IDENTITY=""
if [[ $JOIN -eq 1 ]]; then
    [[ -n "$PEER_URL"   ]] || { err "--join 에는 --peer-url 필수 (기존 OAM 주소)"; exit 1; }
    [[ -n "$JOIN_TOKEN" ]] || { err "--join 에는 --join-token 필수 (POST /api/v1/ha/join-token)"; exit 1; }
    info "peer 에서 그룹 공통 신원 수령... ($PEER_URL)"
    JOIN_IDENTITY=$(curl -fsSk -X POST "$PEER_URL/api/v1/ha/join" \
        -H "Content-Type: application/json" \
        -d "{\"token\":\"$JOIN_TOKEN\",\"node_name\":\"${SERVER_NAME:-$(hostname -s 2>/dev/null || hostname)}\"}" \
        2>/dev/null || true)
    if [[ -z "$JOIN_IDENTITY" ]] || ! echo "$JOIN_IDENTITY" | grep -q '"identity"'; then
        err "신원 수령 실패 — peer 주소/토큰(1회용·만료)을 확인하세요: $PEER_URL"
        exit 1
    fi
    # 신원을 _secrets 에 전개 (JwtSecret / 그룹 CA / mTLS CA) + 설치 파라미터 반영
    JOIN_IDENTITY="$JOIN_IDENTITY" SECRETS_DIR_PRE="$STORE_DIR" \
    LOCAL_RUNTIME="$RUNTIME_DIR" python3 - <<'PYJOIN' || { err "신원 전개 실패"; exit 1; }
import json, os, sys
d = json.loads(os.environ['JOIN_IDENTITY'])['identity']
rt = (d.get('runtime') or {}).get('CimsRuntimeDir') or os.environ.get('SECRETS_DIR_PRE') or ''
if not rt:
    sys.stderr.write('peer 가 CimsRuntimeDir 를 주지 않았고 --runtime-dir 도 없음\n'); sys.exit(1)
# 시크릿·CA 는 **노드 로컬** runtime 에 둔다 — 관리 store(공유 마운트)에 개인키를 올리지
# 않는다(oam_ha.md §5). 관리 store 경로(rt)는 아래 .join_params 로만 전달한다.
sd = os.path.join(os.environ['LOCAL_RUNTIME'], '_secrets')
os.makedirs(sd, mode=0o700, exist_ok=True)
os.chmod(sd, 0o700)
sec = (d.get('auth') or {}).get('JwtSecret') or ''
if sec:
    p = os.path.join(sd, 'jwt_secret')
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f: f.write(sec + '\n')
for sub, keys in (('ca', {'crt': 'ca.crt', 'key': 'ca.key'}),
                  ('agent_mtls', {'ca_crt': 'ca.crt', 'ca_key': 'ca.key',
                                  'client_crt': 'csc_client.crt', 'client_key': 'csc_client.key'})):
    src = d.get(sub) or {}
    if not src: continue
    dd = os.path.join(sd, sub); os.makedirs(dd, mode=0o700, exist_ok=True)
    for k, fn in keys.items():
        pem = src.get(k)
        if not pem: continue
        p = os.path.join(dd, fn)
        mode = 0o600 if fn.endswith('.key') else 0o644
        fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
        with os.fdopen(fd, 'w') as f: f.write(pem)
# 후속 단계가 읽을 파라미터
with open(os.path.join(sd, '.join_params'), 'w') as f:
    json.dump({'port': (d.get('server') or {}).get('Port'),
               'role': (d.get('server') or {}).get('Role') or 'base',
               'agent_oam_url': (d.get('server') or {}).get('AgentOamUrl') or '',
               'cert_sans': (d.get('server') or {}).get('CertSans') or [],
               'mgmt_cidr': (d.get('mgmt') or {}).get('Cidr') or '',
               'runtime_dir': rt,
               'runtime_mount': (d.get('runtime') or {}).get('CimsRuntimeMount') or '',
               'log_dir': (d.get('logging') or {}).get('Dir') or '',
               'accounts': (d.get('auth') or {}).get('BuiltinAccounts') or [],
               'agent': d.get('agent') or {}}, f)
print(f"  신원 전개 완료: {sd} (jwt_secret / ca / agent_mtls)")
PYJOIN
    ok "그룹 공통 신원 수령 완료 — 이 노드는 peer 와 같은 토큰·CA 를 사용"
    # 합류 노드는 OAM 을 기동하지 않는다 — cold standby 이고, 승격 시 agent 의 reconcile 이
    # 볼륨 인수 후 기동한다. 여기서 띄우면 마운트 없이 떠 로컬 store 를 만들 수 있다.
    DO_START=0
fi

# ── oam.json 구성 ────────────────────────────────────────────
# 시크릿 격리 (runtime store v2 P1) — 시크릿은 runtime/_secrets/ 0700 에 모은다.
# (데이터 도메인과 분리 → 백업/동기화 범위에서 제외, 권한 최소화)
SECRETS_DIR="$RUNTIME_DIR/_secrets"
mkdir -p "$SECRETS_DIR"; chmod 700 "$SECRETS_DIR"
JWT_SECRET_FILE="$SECRETS_DIR/jwt_secret"
# oam deployment overlay(upgrade-safe instance config) 임시 출력 — _self_deploy oam 가 읽음.
OAM_OVERLAY_FILE="$SECRETS_DIR/.oam_deploy_overlay.json"
# 마이그레이션 — 구 위치(runtime/.jwt_secret)에 있으면 보존 이동 (기존 토큰 유효 유지).
if [[ ! -f "$JWT_SECRET_FILE" && -f "$RUNTIME_DIR/.jwt_secret" ]]; then
    mv "$RUNTIME_DIR/.jwt_secret" "$JWT_SECRET_FILE"
fi
# 합류 모드에서는 peer 신원(jwt_secret)이 이미 이 파일에 전개돼 있다 — 새로 만들지 않는다.
if [[ ! -f "$JWT_SECRET_FILE" ]]; then
    if [[ $JOIN -eq 1 ]]; then
        err "합류 모드인데 신원(jwt_secret)이 없습니다 — 수령 단계 실패"; exit 1
    fi
    openssl rand -base64 32 > "$JWT_SECRET_FILE"
fi
chmod 600 "$JWT_SECRET_FILE"
PY=python3 OAM_ROOT="$OAM_ROOT" RUNTIME_DIR="$RUNTIME_DIR" PORT="$PORT" \
JWT_SECRET="$(cat "$JWT_SECRET_FILE")" MGMT_IP="$MGMT_IP" \
STORE_DIR="$STORE_DIR" STORE_MOUNT="$STORE_MOUNT" JOIN="$JOIN" \
JOIN_PARAMS_FILE="$SECRETS_DIR/.join_params" \
ADMIN_PASS="$ADMIN_PASS" OAM_OVERLAY_FILE="$OAM_OVERLAY_FILE" python3 - <<'PYEOF'
import hashlib, json, os
p = os.path.join(os.environ['OAM_ROOT'], 'oam', 'config', 'oam.json')
d = json.load(open(p))
port = int(os.environ['PORT'])
join = os.environ.get('JOIN') == '1'
jp = {}
if join:
    try:
        with open(os.environ['JOIN_PARAMS_FILE']) as f:
            jp = json.load(f)
    except Exception:
        jp = {}
    if jp.get('port'):
        port = int(jp['port'])          # peer 와 같은 포트 (게이트웨이/콘솔 주소 일관)
d['Server'] = {'Ip': '0.0.0.0', 'Port': port}
if jp.get('role'):
    d['Server']['Role'] = jp['role']
# 관리(mgmt) IP — agent↔OAM 통신 기준. AgentOamUrl(콘솔 install-command)·Mgmt.Cidr(/24) 반영.
mgmt = (os.environ.get('MGMT_IP') or '').strip()
if mgmt:
    d['Server']['AgentOamUrl'] = f"https://{mgmt}:{port}"
    d.setdefault('Mgmt', {})['Cidr'] = mgmt.rsplit('.', 1)[0] + '.0/24'
if join:
    # 합류 노드는 **peer 와 같은 주소·대역·SAN** 을 쓴다 (VIP 기준).
    if jp.get('agent_oam_url'):
        d['Server']['AgentOamUrl'] = jp['agent_oam_url']
    if jp.get('mgmt_cidr'):
        d.setdefault('Mgmt', {})['Cidr'] = jp['mgmt_cidr']
    sans = list(jp.get('cert_sans') or [])
    if mgmt and mgmt not in sans:
        sans.append(mgmt)               # 이 노드 IP 도 SAN 에 (직접 접속 대비)
    if sans:
        d['Server']['CertSans'] = sans
    if jp.get('log_dir'):
        d.setdefault('ServiceLogging', {})['Dir'] = jp['log_dir']
# 관리 store 경로 — 이중화면 공유 마운트 하위. 미지정 시 노드 로컬 runtime(단일 노드 기본).
store = (os.environ.get('STORE_DIR') or '').strip() or (jp.get('runtime_dir') or '').strip() \
        or os.environ['RUNTIME_DIR']
mount = (os.environ.get('STORE_MOUNT') or '').strip() or (jp.get('runtime_mount') or '').strip()
# 서비스 로그 루트 — **구체값을 반드시 기록한다** (CimsRuntimeDir 과 같은 규칙).
#   비워두면 모듈은 코드 폴백으로 노드 로컬을 쓰는데, **콘솔은 그 폴백을 알 수 없어**
#   템플릿 기본값(사이트 값 = 공유 경로)을 그리게 된다 — 화면과 실제가 갈린다. 설정
#   화면은 실제 적용값이 기준이어야 하므로, 설치 시점에 정해 overlay 에 남긴다.
#   기준은 store 가 아니라 **마운트**다 (oam_ha.md §4.1) — 로그는 store 가 아니라
#   마운트에 붙는 append-only 관측 데이터라, store 이관·스냅샷에 딸려가면 안 된다.
#   마운트를 모르는 첫 부트스트랩(붙이는 수단이 이 OAM 의 콘솔이다)에서는 노드 로컬이고,
#   마운트 후에는 이관이 공유 경로로 바꾼다(`_migrate_shared_store`).
if not (d.get('ServiceLogging') or {}).get('Dir'):
    d.setdefault('ServiceLogging', {})['Dir'] = os.path.join(
        mount or os.environ['RUNTIME_DIR'], 'service_log')
d['CimsRuntimeDir'] = store
if mount:
    d['CimsRuntimeMount'] = mount       # mount guard — 마운트 없으면 기동 거부
d.setdefault('Packages', {})['Dir'] = os.path.join(store, 'pkg_files')
d.setdefault('CimsAuth', {})['JwtSecret'] = os.environ['JWT_SECRET']
if join and jp.get('accounts'):
    d['CimsAuth']['BuiltinAccounts'] = jp['accounts']   # admin 계정도 그룹 공통
ap = os.environ.get('ADMIN_PASS') or ''
if ap and not join:                     # 합류 노드는 peer 계정을 그대로 쓴다
    for a in d['CimsAuth'].get('BuiltinAccounts', []):
        if a.get('login_id') == 'admin':
            a['password_sha256'] = hashlib.sha256(ap.encode()).hexdigest()
            a.pop('password', None)
# oam.json 은 **고치지 않는다** — 패키지 기본값 그대로 두고 위 overlay 가 노드 값을 정한다.
print('  oam.json 무변경 (패키지 기본값) — 노드 값은 config.json overlay 가 정의')

# ── upgrade-safe: 같은 instance 값을 deployment overlay(flat dotted)로도 기록 ──
#   agent 가 config.json 으로 써서 oam.json 위에 적용(load_config) + 버전 간 이관 →
#   oam upgrade 가 패키지 기본 oam.json 으로 덮어써도 포트/시크릿/경로/admin 복원.
ov = {
    'Server.Ip': '0.0.0.0',
    'Server.Port': port,
    'CimsRuntimeDir': d['CimsRuntimeDir'],
    'Packages.Dir': d['Packages']['Dir'],
    'CimsAuth.JwtSecret': d['CimsAuth']['JwtSecret'],
    'CimsAuth.BuiltinAccounts': d['CimsAuth'].get('BuiltinAccounts', []),
}
if d['Server'].get('Role'):
    ov['Server.Role'] = d['Server']['Role']
if d['Server'].get('CertSans'):
    ov['Server.CertSans'] = d['Server']['CertSans']
if d.get('CimsRuntimeMount'):
    ov['CimsRuntimeMount'] = d['CimsRuntimeMount']
# 조건은 **값 존재**로 판정한다 — `if mgmt:` 로 묶으면 합류(join) 모드에서 peer 가 준
# agent_oam_url·mgmt_cidr 이 d 에는 들어가고 overlay 에는 빠져 유실된다(oam.json 을 더 이상
# 쓰지 않으므로 overlay 에 없으면 그대로 사라진다).
if (d.get('Server') or {}).get('AgentOamUrl'):
    ov['Server.AgentOamUrl'] = d['Server']['AgentOamUrl']
if (d.get('Mgmt') or {}).get('Cidr'):
    ov['Mgmt.Cidr'] = d['Mgmt']['Cidr']
if (d.get('ServiceLogging') or {}).get('Dir'):
    ov['ServiceLogging.Dir'] = d['ServiceLogging']['Dir']
ovf = os.environ.get('OAM_OVERLAY_FILE')
if ovf:
    json.dump(ov, open(ovf, 'w'), ensure_ascii=False)

# ── 첫 기동용 overlay 를 **패키지 파일이 아니라 config.json 에** 쓴다 ──────────
#   `oam.json` 은 패키지 기본값이고 노드 값은 overlay(config.json)가 정한다 — 그것이
#   콘솔 설치 경로가 쓰는 메커니즘이고 `load_config()` 도 그렇게 병합한다. 부트스트랩만
#   패키지 파일을 직접 고치면 **같은 버전인데 노드마다 내용이 달라진다**(실측: 부트스트랩
#   노드는 정상, 콘솔 설치 노드는 패키지 기본값 그대로 → 빌드 머신 경로로 기동하다 크래시).
#   두 경로가 같은 메커니즘을 쓰도록 여기서도 overlay 로만 쓴다.
json.dump(ov, open(os.path.join(os.environ['OAM_ROOT'], 'oam', 'config.json'), 'w'),
          ensure_ascii=False, indent=2)
print('  config.json (deployment overlay) 기록 — oam.json 은 패키지 기본값 유지')
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
# 권한 가드 — 제거도 root 작업(systemd/소유권/rm). sudo 없이 실행하면 부분 제거 → 즉시 종료.
if [[ \$EUID -ne 0 ]]; then
    echo "ERROR: root 권한이 필요합니다 — 'sudo \$0 [--yes]' 로 실행하세요." >&2
    exit 1
fi
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
    # install.sh 가 만든 OAM_ROLE=base drop-in 까지 대칭 제거 (디렉터리째).
    #   남기면 다음 설치 때 role 기본값(all)을 가려 "왜 또 base 로 뜨지" 혼란.
    #   이 .d 디렉터리는 install.sh 의 override.conf 전용 — unit 본체는 여기 없음.
    _SVC_HOME="\$(getent passwd "$SVC_USER" 2>/dev/null | cut -d: -f6)"
    if [[ -n "\$_SVC_HOME" && -d "\$_SVC_HOME/.config/systemd/user/cims-agent.service.d" ]]; then
        rm -rf "\$_SVC_HOME/.config/systemd/user/cims-agent.service.d"
        runuser -u "$SVC_USER" -- env XDG_RUNTIME_DIR="/run/user/\$(id -u "$SVC_USER")" \
            systemctl --user daemon-reload 2>/dev/null || true
        echo "✓ OAM_ROLE drop-in 제거 (~/.config/systemd/user/cims-agent.service.d)"
    fi
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
# current 통로로 기동 — 이후 oam 업그레이드 시 이 스크립트가 자동으로 활성 버전을 가리킨다.
cd "$OAM_CURRENT/oam/src"
setsid nohup /usr/bin/env python3 -u "$OAM_CURRENT/oam/src/oam_app.py" > "$OAM_ROOT/log/oam_stdout.log" 2>&1 < /dev/null &
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
# ── 합류 모드 agent 설치 — 대상은 **peer/VIP** OAM (이 노드 OAM 은 cold standby 라 미기동)
if [[ $JOIN -eq 1 && $DO_AGENT -eq 1 ]]; then
    info "agent 설치 (합류 — 대상 OAM: $PEER_URL)..."
    _JOIN_ENROLL=$(python3 -c "
import json
try:
    d=json.load(open('$SECRETS_DIR/.join_params'))
    print((d.get('agent') or {}).get('enrollment_token') or '')
except Exception: print('')" 2>/dev/null)
    if [[ -z "$_JOIN_ENROLL" ]]; then
        err "peer 가 agent enrollment token 을 주지 않았습니다 (같은 이름 agent 가 이미 등록됐을 수 있음)"
        err "  → 콘솔에서 이 서버의 install-command 를 받아 수동 실행하세요"
        AGENT_STATE="실패 (enrollment token 없음 — 콘솔 수동설치)"
    else
        _IA="$PREFIX/.cims-install-agent.sh"
        if curl -fsSk -o "$_IA" "$PEER_URL/install-agent.sh" && [[ -s "$_IA" ]]; then
            chmod 0644 "$_IA" 2>/dev/null || true
            _ia_args=(--oam-url "$PEER_URL" --enrollment-token "$_JOIN_ENROLL"
                      --name "${SERVER_NAME:-$(hostname -s 2>/dev/null || hostname)}"
                      --install-dir "$PREFIX" --svc-user "$SVC_USER")
            if [[ $USE_SYSTEMD -eq 1 && -d /run/systemd/system ]]; then
                # 합류 노드도 OAM 은 role=base — 배포 설정(Server.Role)이 정본이지만
                # drop-in 도 함께 둬서 어느 경로로 기동돼도 같은 역할이 되게 한다.
                _run_as "mkdir -p ~/.config/systemd/user/cims-agent.service.d && printf '[Service]\nEnvironment=OAM_ROLE=%s\n' \"$(python3 -c "
import json
try: print((json.load(open('$SECRETS_DIR/.join_params')).get('role') or 'base'))
except Exception: print('base')" 2>/dev/null)\" > ~/.config/systemd/user/cims-agent.service.d/override.conf" || true
            else
                _ia_args+=(--no-systemd)
            fi
            if bash "$_IA" "${_ia_args[@]}" >> "$OAM_ROOT/log/agent_install.log" 2>&1; then
                ok "agent 설치·enroll 완료 (대상 $PEER_URL)"
                AGENT_STATE="실행 중 (합류 — OAM 은 미기동/cold standby)"
            else
                err "agent 설치 실패 (상세: $OAM_ROOT/log/agent_install.log)"
                AGENT_STATE="실패 (install-agent.sh)"
            fi
        else
            err "install-agent.sh 다운로드 실패 ($PEER_URL) — agent 미설치"
            AGENT_STATE="실패 (install-agent.sh 다운로드)"
        fi
    fi
fi
if [[ $JOIN -eq 0 && $DO_AGENT -eq 1 && $DO_START -eq 1 ]]; then
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

    # 이 OAM 노드에 이미 설치·기동된 base 모듈(oam/console)을 deployment 레코드로 등록한다.
    #   부트스트랩은 oam/console 을 패키지로만 시드하고 agent 만 만들 뿐 deployment 가 없어,
    #   콘솔 "패키지 설치" 목록(=deployment 조회)에 oam/console 이 안 보였다. 여기서 보강.
    #   멱등: 같은 agent+process 의 비-removed deployment 가 있으면 skip.
    #   $1=package_name $2=install_path $3=process_name
    _self_deploy() {
        local pn="$1" ipath="$2" proc="$3" cfg="${4:-}" aid pid exists
        aid=$(_api GET "/api/v1/agents" "" "$TOK" | python3 -c "import sys,json
try:
    d=json.load(sys.stdin); ags=(d.get('items') or []) if isinstance(d,dict) else []
    print(next((str(a.get('id')) for a in ags if isinstance(a,dict) and a.get('name')=='$HOSTNM'),''))
except Exception: print('')" 2>/dev/null)
        pid=$(_api GET "/api/v1/packages" "" "$TOK" | python3 -c "import sys,json
try:
    d=json.load(sys.stdin); ps=(d.get('items') or []) if isinstance(d,dict) else []
    c=sorted([p for p in ps if isinstance(p,dict) and p.get('name')=='$pn'], key=lambda p:str(p.get('version','')))
    print(str(c[-1]['id']) if c else '')
except Exception: print('')" 2>/dev/null)
        [[ -z "$aid" || -z "$pid" ]] && { warn "self-deploy($pn): agent/package id 미확인 — skip"; return 1; }
        exists=$(_api GET "/api/v1/deployments" "" "$TOK" | python3 -c "import sys,json
try:
    d=json.load(sys.stdin); ds=(d.get('items') or []) if isinstance(d,dict) else []
    print('Y' if any(str(x.get('agent_id'))=='$aid' and (x.get('process_name') or x.get('package_name'))=='$proc' and x.get('status')!='removed' for x in ds if isinstance(x,dict)) else '')
except Exception: print('')" 2>/dev/null)
        [[ "$exists" == "Y" ]] && return 0
        # config overlay 동봉(있으면) — upgrade 가 패키지 기본 config 로 덮어써도
        #   deployment overlay(config.json)가 instance 값(포트/시크릿/경로/admin)을 복원한다.
        #   agent 가 config.json 을 버전 간 이관하므로 영속(oam upgrade 안전성).
        local cfg_field=""
        [[ -n "$cfg" ]] && cfg_field=",\"config\":$cfg"
        _api POST "/api/v1/deployments" \
            "{\"agent_id\":$aid,\"package_id\":$pid,\"process_name\":\"$proc\",\"install_path\":\"$ipath\",\"status\":\"running\"$cfg_field}" \
            "$TOK" >/dev/null 2>&1
    }

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
            chmod 0644 "$_IA" 2>/dev/null || true
            # 통일된 설치 경로 — install-agent.sh 를 root 로 직접 실행(=일반 install-command 와 동일 스크립트).
            #   base 는 이미 root → 서비스 계정은 --svc-user 로 명시, 설치 경로는 --install-dir=PREFIX.
            #   install-agent.sh 가 추출 + sudoers + linger + enroll + systemd --user enable 까지 수행
            #   (구 setup-sudoers.sh + init.sh 단계 흡수). systemd 미사용 환경은 --no-systemd → 아래 nohup.
            _ia_args=(--oam-url "https://127.0.0.1:$PORT" --enrollment-token "$ENROLL_TOKEN"
                      --name "$HOSTNM" --install-dir "$PREFIX" --svc-user "$SVC_USER")
            _use_sd=0
            if [[ $USE_SYSTEMD -eq 1 && -d /run/systemd/system ]]; then _use_sd=1; else _ia_args+=(--no-systemd); fi
            # OAM_ROLE=base — base 노드는 게이트웨이. agent 가 OAM 을 --role base 로 기동해야
            #   서비스 모듈(csc/oam-svc)이 self-register 한 라우트를 프록시한다. systemd --user
            #   drop-in 으로 영속(watchdog 재기동에도 유지). install-agent.sh 의 enable 이 픽업.
            if [[ $_use_sd -eq 1 ]]; then
                _run_as "mkdir -p ~/.config/systemd/user/cims-agent.service.d && printf '[Service]\nEnvironment=OAM_ROLE=base\n' > ~/.config/systemd/user/cims-agent.service.d/override.conf" || true
            fi
            if [[ $_dl_ok -ne 1 ]]; then
                err "install-agent.sh 다운로드 실패 (HTTP $_dl_http) — agent 미설치 (콘솔 수동설치)"
                AGENT_STATE="실패 (install-agent.sh 다운로드)"
            elif bash "$_IA" "${_ia_args[@]}" >> "$OAM_ROOT/log/agent_install.log" 2>&1; then
                ok "agent 설치 완료 ($PREFIX/agent — sudoers/linger/enroll 포함)"
                # OAM 을 cims-svc 로 인계 — 부트스트랩 nohup 을 정식 감독 프로세스로 교체.
                #   start_oam 의 kill_stray 가 부트스트랩 OAM(같은 포트/경로)을 정리하고
                #   pidfile($OAM_ROOT/run/oam.pid)을 남긴다 → 중복기동·고아 방지.
                info "OAM 을 agent 관리(cims-svc)로 인계... (role=base, 게이트웨이)"
                if _run_as "OAM_ROLE=base CIMS_DIST_DIR='$OAM_CURRENT' CIMS_PYTHON=python3 '$PREFIX/agent/current/bin/cims-svc' start oam" \
                        >> "$OAM_ROOT/log/oam_handover.log" 2>&1; then
                    ok "OAM cims-svc 감독 전환 완료 (pidfile + watchdog)"
                else
                    warn "OAM cims-svc 인계 실패 — agent watchdog 가 후속 회수 (상세: $OAM_ROOT/log/oam_handover.log)"
                fi
                # agent watchdog 감독 등록: oam → versioned 모듈 경로 (supervise_tick 가 읽음)
                mkdir -p "$PREFIX/run"
                printf '{"oam": "%s"}\n' "$OAM_CURRENT" > "$PREFIX/run/supervised.json"
                chown -R "$SVC_USER":"$(id -gn "$SVC_USER")" "$PREFIX/run" 2>/dev/null || true
                # 이 노드의 base 모듈(oam — console 동봉)을 deployment 로 등록 → 콘솔 "패키지 설치" 목록 노출.
                # oam 은 upgrade-safe overlay(포트/시크릿/경로/admin) 동봉 — upgrade 시 instance config 보존.
                _self_deploy oam     "$OAM_ROOT" oam "$(cat "$OAM_OVERLAY_FILE" 2>/dev/null)" || true
                if [[ $_use_sd -eq 1 ]]; then
                    AGENT_STATE="실행 중 (systemd --user cims-agent.service)"
                else
                    # systemd 미사용 — install-agent.sh 가 enroll 까지만 했으므로 nohup 기동
                    _run_as "cd '$PREFIX' && CIMS_AGENT_PREFIX='$PREFIX' setsid nohup python3 ./agent/current/cims_agent.py --oam-url 'https://127.0.0.1:$PORT' --state-dir ./state --name '$HOSTNM' > ./agent-stdout.log 2>&1 < /dev/null &"
                    sleep 3
                    AGENT_STATE="실행 중 (nohup — systemd 미사용 환경)"
                fi
            else
                err "install-agent.sh 실행 실패 (상세: $OAM_ROOT/log/agent_install.log) — 콘솔에서 수동 설치하세요"
                AGENT_STATE="실패 (install-agent.sh 실행)"
            fi
        fi
    fi
    rm -f "$_HTTP_FILE"
    set -e
elif [[ $DO_AGENT -eq 1 ]]; then
    AGENT_STATE="미기동 (--no-start)"
fi

if [[ $JOIN -eq 1 ]]; then
cat <<JOINDONE

────────────────────────────────────────────────────────────
 CIMS 관리평면 합류(2번째 노드) 설치 완료
   OAM      : **미기동** (cold standby — 승격 시 agent 가 볼륨 인수 후 기동)
   신원      : peer 와 동일 (JwtSecret / admin 계정 / 그룹 CA / mTLS CA)
   agent    : $AGENT_STATE  → 대상 OAM $PEER_URL
   store    : $(python3 -c "
import json
try: d=json.load(open('$SECRETS_DIR/.join_params')); print(d.get('runtime_dir') or '(미지정)')
except Exception: print('(미지정)')" 2>/dev/null)
   마운트    : $(python3 -c "
import json
try: d=json.load(open('$SECRETS_DIR/.join_params')); print(d.get('runtime_mount') or '(미설정 — mount guard 비활성)')
except Exception: print('(미설정)')" 2>/dev/null)

   다음 (콘솔에서):
     1) 공유 store 마운트 확인 — 이 서버에도 상대 노드와 **같은 NAS 경로**가 붙어야 한다
        (콘솔 시스템/인프라 > 마운트 관리 로 추가하면 fstab 에 영속)
     2) HA 그룹에 이 서버를 멤버로 추가 + 공유 store 설정
     3) 이 서버에 oam / oam-svc 패키지 설치 (배포설정은 그룹 공통값이 주입됨)
     4) 그룹 서비스 시작 → VIP 보유 노드에서만 OAM 이 뜬다
     5) 전 agent 를 VIP 로 재지정: POST /api/v1/agents/oam-url
   제거   : sudo $PREFIX/uninstall-base.sh
────────────────────────────────────────────────────────────
JOINDONE
exit 0
fi

cat <<DONE

────────────────────────────────────────────────────────────
 CIMS base 설치 완료
   프로세스:
     · oam     : 실행 중 — agent(cims-svc) 감독, API+콘솔 서빙 (HTTPS :$PORT)
     · console : oam-base 패키지에 동봉 — oam 이 정적 서빙 (위 포트)
     · agent   : $AGENT_STATE
   콘솔   : https://<이 서버 IP>:$PORT/   (브라우저 인증서 경고는 self-signed 때문)
   로그인 : admin / $([[ -n "$ADMIN_PASS" ]] && echo '<--admin-pass 로 설정한 비밀번호>' || echo '1234  ← 상용에서는 변경 권장 (--admin-pass)')
   다음   : 콘솔 → 시스템 > 시스템/인프라 → ＋시스템 추가 → 각 서버에
            install-command 실행(agent 설치) → 패키지 등록/설치/설정
   제거   : sudo $PREFIX/uninstall-base.sh
────────────────────────────────────────────────────────────
DONE
