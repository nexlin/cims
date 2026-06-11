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
#     --no-systemd     systemd 미사용 (start-oam.sh 생성)
#     --no-start       설치만 하고 기동하지 않음
set -euo pipefail

PREFIX=/opt/cims-agent
PORT=4419
ADMIN_PASS=""
USE_SYSTEMD=1
DO_START=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)     PREFIX="$2"; shift 2 ;;
        --port)       PORT="$2"; shift 2 ;;
        --admin-pass) ADMIN_PASS="$2"; shift 2 ;;
        --no-systemd) USE_SYSTEMD=0; shift ;;
        --no-start)   DO_START=0; shift ;;
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
if [[ $EUID -ne 0 && ! -w "$(dirname "$PREFIX")" ]]; then
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

# ── 레이아웃 (버전 단위 설치 — agent 배포 체계와 동일) ───────────
OAM_ROOT="$PREFIX/oam/$OAM_VER"
CON_ROOT="$PREFIX/console/$CON_VER"
RUNTIME_DIR="$PREFIX/oam/runtime"          # 버전 무관 영속 store (업그레이드 생존)
mkdir -p "$OAM_ROOT" "$CON_ROOT" "$RUNTIME_DIR" "$PREFIX/agent-assets"

info "패키지 전개..."
tar xzf "$OAM_TAR" -C "$OAM_ROOT"
tar xzf "$CON_TAR" -C "$CON_ROOT"
# agent: 에셋으로 전개 — OAM 이 /install-agent.sh, /cims_agent.py 를 여기서 서빙.
# (이 서버의 agent 데몬 자체는 2단계에서 콘솔 install-command 로 enroll/설치)
tar xzf "$AGT_TAR" -C "$PREFIX/agent-assets"
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
CON_DIST="$CON_ROOT/console/dist" JWT_SECRET="$(cat "$JWT_SECRET_FILE")" \
ADMIN_PASS="$ADMIN_PASS" python3 - <<'PYEOF'
import hashlib, json, os
p = os.path.join(os.environ['OAM_ROOT'], 'oam', 'config', 'oam.json')
d = json.load(open(p))
d['Server'] = {'Ip': '0.0.0.0', 'Port': int(os.environ['PORT'])}
d['CimsRuntimeDir'] = os.environ['RUNTIME_DIR']
d.setdefault('Packages', {})['Dir'] = os.path.join(os.environ['RUNTIME_DIR'], 'pkg_files')
d.setdefault('Console', {})['StaticDir'] = os.environ['CON_DIST']
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

# ── 기동 (systemd 또는 start 스크립트) ───────────────────────────
START_CMD="/usr/bin/env python3 -u $OAM_ROOT/oam/src/oam_app.py"
if [[ $USE_SYSTEMD -eq 1 && -d /run/systemd/system && $EUID -eq 0 ]]; then
    cat > /etc/systemd/system/cims-oam.service <<UNIT
[Unit]
Description=CIMS OAM (base management plane: API + Console)
After=network-online.target

[Service]
Type=simple
WorkingDirectory=$OAM_ROOT/oam/src
Environment=CIMS_AGENT_ASSET_DIR=$PREFIX/agent-assets/agent
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
export CIMS_AGENT_ASSET_DIR="$PREFIX/agent-assets/agent"
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

cat <<DONE

────────────────────────────────────────────────────────────
 CIMS base 설치 완료
   콘솔   : https://<이 서버 IP>:$PORT/   (브라우저 인증서 경고는 self-signed 때문)
   로그인 : admin / $([[ -n "$ADMIN_PASS" ]] && echo '<--admin-pass 로 설정한 비밀번호>' || echo '1234  ← 상용에서는 변경 권장 (--admin-pass)')
   다음   : 콘솔 → 시스템 > 시스템/인프라 → ＋시스템 추가 → 각 서버에
            install-command 실행(agent 설치) → 패키지 등록/설치/설정
────────────────────────────────────────────────────────────
DONE
