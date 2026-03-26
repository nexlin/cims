#!/usr/bin/env bash
# =============================================================
# CIMS 배포 설정 스크립트
# 사용법: ./configure.sh [options]
#
# 소스 트리에서: ./configure.sh --local-ip 192.168.1.10
# dist 디렉토리에서: ./configure.sh --local-ip 192.168.1.10
# =============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Auto-detect: running from dist/ or source tree
if [[ -f "$SCRIPT_DIR/csp/bin/csp" ]]; then
    DIST_DIR="$SCRIPT_DIR"
    SRC_DIR=""   # no source dirs when running from dist
else
    DIST_DIR="$SCRIPT_DIR/build/dist"
    SRC_DIR="$SCRIPT_DIR"
fi

# ── 색상 ───────────────────────────────────────────────────────
CYAN='\033[0;36m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BOLD='\033[1m'; NC='\033[0m'
info()   { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()     { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()   { echo -e "${YELLOW}[WARN]${NC}  $*"; }

# ── 기본값 ─────────────────────────────────────────────────────
LOCAL_IP="127.0.0.1"
CSP_IP=""
CMP_IP=""
CWRTC_IP=""
CSC_HOST=""
DB_HOST=""
DB_USER="cims"
DB_PASSWORD="cims1234"
SIP_DOMAIN="ims.mnc001.mcc001.3gppnetwork.org"
VOIP_DOMAIN="ims.mnc001.mcc001.3gppnetwork.org"
PTT_DOMAIN="ptt.mnc001.mcc001.3gppnetwork.org"
IDMS_JWT_SECRET=""
CIMS_JWT_SECRET=""

usage() {
    cat <<EOF
${BOLD}CIMS 배포 설정 스크립트${NC}

사용법: $(basename "$0") [options]

${BOLD}서버 IP:${NC}
  --local-ip   IP    모든 컴포넌트 기본 IP (기본: 127.0.0.1)
  --csp-ip     IP    CSP 서버 IP
  --cmp-ip     IP    CMP 서버 IP
  --cwrtc-ip   IP    CWRTC 서버 IP
  --csc-host   HOST  CSC 서버 호스트명/IP

${BOLD}데이터베이스:${NC}
  --db-host    HOST  MariaDB 호스트 (기본: local-ip)
  --db-user    USER  DB 사용자 (기본: cims)
  --db-password PWD  DB 비밀번호 (기본: cims1234)

${BOLD}도메인:${NC}
  --sip-domain   DOM  SIP 인증 Realm / 기본 도메인 (기본: cims.local)
  --voip-domain  DOM  VoIP 통화 SIP 도메인 (기본: sip-domain 값 사용)
  --ptt-domain   DOM  PTT 그룹 통화 SIP 도메인 (기본: ptt.cims.local)
  --mcptt-domain DOM  MCPTT 앱 도메인 (기본: mcptt.local)

${BOLD}보안:${NC}
  --idms-secret  SEC  IdMS JWT 시크릿 (기본: 랜덤 생성)
  --cims-secret  SEC  CIMS Admin JWT 시크릿 (기본: 랜덤 생성)

${BOLD}예시:${NC}
  # 단일 서버 배포
  $(basename "$0") --local-ip 192.168.1.10 --db-password mypass --sip-domain ims.mycompany.com

  # 다중 서버 배포
  $(basename "$0") --csp-ip 192.168.1.10 --cmp-ip 192.168.1.11 \\
                   --cwrtc-ip 192.168.1.12 --csc-host 192.168.1.13 \\
                   --db-host 192.168.1.14 --sip-domain ims.mycompany.com
EOF
}

# ── 인수 파싱 ───────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --local-ip)     LOCAL_IP="$2";      shift 2 ;;
        --csp-ip)       CSP_IP="$2";        shift 2 ;;
        --cmp-ip)       CMP_IP="$2";        shift 2 ;;
        --cwrtc-ip)     CWRTC_IP="$2";      shift 2 ;;
        --csc-host)     CSC_HOST="$2";      shift 2 ;;
        --db-host)      DB_HOST="$2";       shift 2 ;;
        --db-user)      DB_USER="$2";       shift 2 ;;
        --db-password)  DB_PASSWORD="$2";   shift 2 ;;
        --sip-domain)   SIP_DOMAIN="$2";    shift 2 ;;
        --voip-domain)  VOIP_DOMAIN="$2";   shift 2 ;;
        --ptt-domain)   PTT_DOMAIN="$2";    shift 2 ;;
        --idms-secret)  IDMS_JWT_SECRET="$2"; shift 2 ;;
        --cims-secret)  CIMS_JWT_SECRET="$2"; shift 2 ;;
        --help|-h)      usage; exit 0 ;;
        *) echo "알 수 없는 옵션: $1"; echo ""; usage; exit 1 ;;
    esac
done

# 미설정 값은 local-ip로
CSP_IP="${CSP_IP:-$LOCAL_IP}"
# VoipDomain 미설정 시 SipDomain과 동일
VOIP_DOMAIN="${VOIP_DOMAIN:-$SIP_DOMAIN}"
CMP_IP="${CMP_IP:-$LOCAL_IP}"
CWRTC_IP="${CWRTC_IP:-$LOCAL_IP}"
CSC_HOST="${CSC_HOST:-$LOCAL_IP}"
DB_HOST="${DB_HOST:-127.0.0.1}"

# JWT 시크릿 랜덤 생성 (미설정 시)
if [[ -z "$IDMS_JWT_SECRET" ]]; then
    IDMS_JWT_SECRET="$(openssl rand -base64 32 2>/dev/null || echo 'mcptt_jwt_secret_change_me')"
fi
if [[ -z "$CIMS_JWT_SECRET" ]]; then
    CIMS_JWT_SECRET="$(openssl rand -base64 32 2>/dev/null || echo 'cims_jwt_secret_change_me')"
fi

echo ""
info "배포 설정:"
echo "  CSP_IP      = $CSP_IP"
echo "  CMP_IP      = $CMP_IP"
echo "  CWRTC_IP    = $CWRTC_IP"
echo "  CSC_HOST    = $CSC_HOST"
echo "  DB_HOST     = $DB_HOST / $DB_USER"
echo "  SIP_DOMAIN  = $SIP_DOMAIN (auth realm)"
echo "  VOIP_DOMAIN = $VOIP_DOMAIN"
echo "  PTT_DOMAIN  = $PTT_DOMAIN"
echo "  DIST_DIR    = $DIST_DIR"
echo ""

# ── 플레이스홀더 치환 함수 ──────────────────────────────────────
apply_template() {
    local src="$1"
    local dst="$2"
    [[ ! -f "$src" ]] && warn "템플릿 없음: $src" && return
    mkdir -p "$(dirname "$dst")"
    sed \
        -e "s|@CSP_IP@|${CSP_IP}|g" \
        -e "s|@CMP_IP@|${CMP_IP}|g" \
        -e "s|@CWRTC_IP@|${CWRTC_IP}|g" \
        -e "s|@CSC_HOST@|${CSC_HOST}|g" \
        -e "s|@DB_HOST@|${DB_HOST}|g" \
        -e "s|@DB_USER@|${DB_USER}|g" \
        -e "s|@DB_PASSWORD@|${DB_PASSWORD}|g" \
        -e "s|@SIP_DOMAIN@|${SIP_DOMAIN}|g" \
        -e "s|@VOIP_DOMAIN@|${VOIP_DOMAIN}|g" \
        -e "s|@PTT_DOMAIN@|${PTT_DOMAIN}|g" \
        -e "s|@IDMS_JWT_SECRET@|${IDMS_JWT_SECRET}|g" \
        -e "s|@CIMS_JWT_SECRET@|${CIMS_JWT_SECRET}|g" \
        "$src" > "$dst"
    ok "생성: $dst"
}

# ── dist 설정 파일 생성 ─────────────────────────────────────────
# Dist mode: templates are *.template files alongside the configs
apply_template "$DIST_DIR/cmp/config/cmp.json.template"                    "$DIST_DIR/cmp/config/cmp.json"
apply_template "$DIST_DIR/csp/config/csp.json.template"                    "$DIST_DIR/csp/config/csp.json"
apply_template "$DIST_DIR/cwrtc/config/cwrtc.json.template"                "$DIST_DIR/cwrtc/config/cwrtc.json"
apply_template "$DIST_DIR/csc/config/csc.json.template"                    "$DIST_DIR/csc/config/csc.json"

# ── 개발용 Vite env 파일 생성 (소스 트리에서만) ─────────────────
if [[ -n "$SRC_DIR" ]]; then
    # CSC SSL 여부 자동 감지
    if [[ -f "$DIST_DIR/csc/cert/server.key" && -f "$DIST_DIR/csc/cert/server.crt" ]]; then
        CSC_SCHEME="https"
    else
        CSC_SCHEME="http"
    fi

    # cims-console/.env.local  (Vite dev proxy 대상)
    cat > "$SRC_DIR/cims-console/.env.local" <<EOF
VITE_ADMIN_TARGET=${CSC_SCHEME}://${CSC_HOST}:4420
EOF
    ok "생성: $SRC_DIR/cims-console/.env.local"

    # cims-phone/.env.local
    cat > "$SRC_DIR/cims-phone/.env.local" <<EOF
VITE_MCPTT_TARGET=${CSC_SCHEME}://${CSC_HOST}:4430
VITE_CWRTC_TARGET=ws://${CWRTC_IP}:8080
EOF
    ok "생성: $SRC_DIR/cims-phone/.env.local"
fi

# ── DB 접속 권한 SQL 생성 ───────────────────────────────────────
# DB가 별도 서버일 경우, 아래 SQL을 DB 서버에서 실행해야 합니다.
DB_GRANT_SQL="$DIST_DIR/sql/grant_db_access.sql"
mkdir -p "$(dirname "$DB_GRANT_SQL")"
cat > "$DB_GRANT_SQL" <<EOF
-- CIMS DB 접속 권한 부여
-- DB 서버(${DB_HOST})에서 실행: sudo mysql < grant_db_access.sql
USE mysql;
GRANT SELECT, INSERT, UPDATE, DELETE ON cims.* TO '${DB_USER}'@'${LOCAL_IP}' IDENTIFIED BY '${DB_PASSWORD}';
GRANT SELECT, INSERT, UPDATE, DELETE ON cims.* TO '${DB_USER}'@'localhost'    IDENTIFIED BY '${DB_PASSWORD}';
FLUSH PRIVILEGES;
EOF
ok "생성: $DB_GRANT_SQL"

echo ""
ok "설정 완료. 서비스 시작: ./cims.sh start"

# DB가 별도 서버인 경우 안내
if [[ "$DB_HOST" != "127.0.0.1" && "$DB_HOST" != "localhost" ]]; then
    echo ""
    echo -e "${YELLOW}[참고]${NC} DB 서버(${DB_HOST})에서 접속 권한 부여 필요:"
    echo "       sudo mysql < $DB_GRANT_SQL"
fi
