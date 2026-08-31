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

# ── 공용 라이브러리 (색상/로그, .cims 리더) ─────────────────────
# 소스 트리·dist 양쪽에서 동작: dist 에는 cims.sh dist 단계가 scripts/lib 를 복사.
source "$SCRIPT_DIR/scripts/lib/common.sh" || {
    echo "[ERROR] scripts/lib/common.sh 없음 — 레포/dist 트리 손상" >&2; exit 1; }

# ── .cims/server.local.json 우선 read (cims.sh init / configure 대화형 결과) ─
# 우선순위: 명시 옵션 > env (CIMS_LOCAL_IP 등) > .cims/server.local.json
# 본 시점엔 env 와 .cims 만 default 로 채워두고, --local-ip 등 명시 옵션이
# 들어오면 아래 인수 파싱 단계에서 덮어쓴다.
# top-level local_ip/db_password 는 cims.sh init 산출, "configure" 객체는
# 본 스크립트 대화형 모드의 저장분 — 재실행/verify 자동 호출 간 멱등성 보장.
_INIT_CFG="${SRC_DIR:-$SCRIPT_DIR}/.cims/server.local.json"
_CFG_SAVED_KEYS="csp_ip psp_ip isp_ip cmp_ip pmp_ip imp_ip cmdp_ip cwrtc_ip csc_host \
db_host db_user volte_domain ptt_domain country_code \
msg_log_dir service_log_dir record_dir"
eval "$(cims_local_cfg_eval "$_INIT_CFG" $_CFG_SAVED_KEYS)"

# ── 기본값 ─────────────────────────────────────────────────────
# LOCAL_IP 의 default 결정 — env > .cims > "" (빈 값이면 인수 파싱 후 abort)
LOCAL_IP="${CIMS_LOCAL_IP:-${_init_local_ip:-}}"
CSP_IP="${_init_csp_ip:-}"
PSP_IP="${_init_psp_ip:-}"    # PSP (PTT 시그널링) — 미설정 시 CSP_IP 따름
ISP_IP="${_init_isp_ip:-}"    # ISP (IBCF 트렁크) — 미설정 시 CSP_IP 따름 (P2)
CMP_IP="${_init_cmp_ip:-}"
PMP_IP="${_init_pmp_ip:-}"    # PMP (PTT 미디어) — 미설정 시 CMP_IP 따름
IMP_IP="${_init_imp_ip:-}"    # IMP (IBCF 미디어) — 미설정 시 CMP_IP 따름 (P2)
CMDP_IP="${_init_cmdp_ip:-}"  # CMDP (MCData 미디어평면) — 미설정 시 CMP_IP 따름
CWRTC_IP="${_init_cwrtc_ip:-}"
CSC_HOST="${_init_csc_host:-}"
DB_HOST="${_init_db_host:-}"
DB_USER="${_init_db_user:-cims}"
DB_PASSWORD="${CIMS_DB_PASSWORD:-${_init_db_password:-cims1234}}"
VOLTE_DOMAIN="${_init_volte_domain:-}"
PTT_DOMAIN="${_init_ptt_domain:-}"
COUNTRY_CODE="${_init_country_code:-}"
IDMS_JWT_SECRET=""
CIMS_JWT_SECRET=""
MSG_LOG_DIR="${_init_msg_log_dir:-}"
SERVICE_LOG_DIR="${_init_service_log_dir:-}"
RECORD_DIR="${_init_record_dir:-}"
INTERACTIVE="auto"   # auto: 인수 없음 + TTY 일 때만 대화형

usage() {
    cat <<EOF
${BOLD}CIMS 배포 설정 스크립트${NC}

사용법: $(basename "$0") [options]

${BOLD}대화형 모드:${NC}
  옵션 없이 터미널(TTY)에서 실행하면 항목별로 기본값을 제시하고 Enter(수락)
  또는 직접 입력으로 진행하는 대화형 wizard 가 동작한다. 입력값은
  .cims/server.local.json 에 저장되어 다음 실행(verify 자동 호출 포함)의
  기본값이 된다.
  -i, --interactive  옵션과 무관하게 대화형 강제
  -y, --defaults     대화형 없이 저장값/기본값으로 즉시 진행

${BOLD}서버 IP:${NC}
  --local-ip   IP    모든 컴포넌트 기본 IP
                     (기본: env CIMS_LOCAL_IP > .cims/server.local.json > 빈 값 → abort.
                      './cims.sh init' 으로 server.local.json 생성 권장)
  --csp-ip     IP    CSP 서버 IP (VoLTE 시그널링: CSCF+TAS)
  --psp-ip     IP    PSP 서버 IP (PTT 시그널링: CSCF+PTT-AS, 미설정 시 CSP_IP)
  --isp-ip     IP    ISP 서버 IP (IBCF 트렁크, 미설정 시 CSP_IP)
  --cmp-ip     IP    CMP 서버 IP (VoLTE 미디어)
  --pmp-ip     IP    PMP 서버 IP (PTT 미디어, 미설정 시 CMP_IP)
  --imp-ip     IP    IMP 서버 IP (IBCF 미디어, 미설정 시 CMP_IP)
  --cmdp-ip    IP    CMDP 서버 IP (MCData 미디어평면 MSRP, 미설정 시 CMP_IP)
  --cwrtc-ip   IP    CWRTC 서버 IP
  --csc-host   HOST  CSC 서버 호스트명/IP

${BOLD}데이터베이스:${NC}
  --db-host    HOST  MariaDB 호스트 (기본: 127.0.0.1)
  --db-user    USER  DB 사용자 (기본: cims)
  --db-password PWD  DB 비밀번호 (기본: cims1234)

${BOLD}도메인:${NC}
  --volte-domain DOM  VoLTE SIP 도메인 / 인증 Realm (기본: ims.mnc001.mcc001.3gppnetwork.org)
  --ptt-domain   DOM  PTT 그룹 통화 SIP 도메인 (기본: volte-domain의 ims→ptt 치환)
  --country-code CC   홈 국가코드(E.164 digits, 단말 번호 로컬 표기용. 기본: 82)

${BOLD}로그/녹취:${NC}
  --msg-log-dir      DIR  메시지 통계 로그 디렉터리 (기본: DIST_DIR/ext_mnt/msg_log)
  --service-log-dir  DIR  서비스 이력/Flow 로그 디렉터리 (기본: DIST_DIR/ext_mnt/service_log)
  --record-dir       DIR  녹취 파일 디렉터리 (기본: DIST_DIR/ext_mnt/recordings)

${BOLD}보안:${NC}
  --idms-secret  SEC  IdMS JWT 시크릿 (기본: 랜덤 생성)
  --cims-secret  SEC  CIMS Admin JWT 시크릿 (기본: 랜덤 생성)

${BOLD}예시:${NC}
  # 단일 서버 배포
  $(basename "$0") --local-ip 192.168.1.10 --db-password mypass \\
                   --volte-domain ims.mnc033.mcc450.3gppnetwork.org

  # 다중 서버 배포
  $(basename "$0") --csp-ip 192.168.1.10 --cmp-ip 192.168.1.11 \\
                   --cwrtc-ip 192.168.1.12 --csc-host 192.168.1.13 \\
                   --db-host 192.168.1.14 --volte-domain ims.mycompany.com
EOF
}

# ── 인수 파싱 ───────────────────────────────────────────────────
_ARGC=$#   # 인수 0개 + TTY → 대화형 (verify 등 자동 호출은 항상 옵션을 전달)
while [[ $# -gt 0 ]]; do
    case "$1" in
        --local-ip)     LOCAL_IP="$2";      shift 2 ;;
        --csp-ip)       CSP_IP="$2";        shift 2 ;;
        --psp-ip)       PSP_IP="$2";        shift 2 ;;
        --isp-ip)       ISP_IP="$2";        shift 2 ;;
        --cmp-ip)       CMP_IP="$2";        shift 2 ;;
        --pmp-ip)       PMP_IP="$2";        shift 2 ;;
        --imp-ip)       IMP_IP="$2";        shift 2 ;;
        --cmdp-ip)      CMDP_IP="$2";       shift 2 ;;
        --cwrtc-ip)     CWRTC_IP="$2";      shift 2 ;;
        --csc-host)     CSC_HOST="$2";      shift 2 ;;
        --db-host)      DB_HOST="$2";       shift 2 ;;
        --db-user)      DB_USER="$2";       shift 2 ;;
        --db-password)  DB_PASSWORD="$2";   shift 2 ;;
        --volte-domain) VOLTE_DOMAIN="$2";  shift 2 ;;
        --ptt-domain)   PTT_DOMAIN="$2";    shift 2 ;;
        --country-code) COUNTRY_CODE="$2";  shift 2 ;;
        --msg-log-dir)      MSG_LOG_DIR="$2";       shift 2 ;;
        --service-log-dir)  SERVICE_LOG_DIR="$2";   shift 2 ;;
        --record-dir)       RECORD_DIR="$2";        shift 2 ;;
        --idms-secret)      IDMS_JWT_SECRET="$2";   shift 2 ;;
        --cims-secret)  CIMS_JWT_SECRET="$2"; shift 2 ;;
        --interactive|-i) INTERACTIVE="yes"; shift ;;
        --defaults|-y)    INTERACTIVE="no";  shift ;;
        --help|-h)      usage; exit 0 ;;
        *) echo "알 수 없는 옵션: $1"; echo ""; usage; exit 1 ;;
    esac
done

# ── 대화형 wizard ───────────────────────────────────────────────
# 인수 없이 TTY 에서 실행 시: 항목별 기본값 제시 → Enter=수락 / 직접 입력.
# verify(S3-CONFIGURE) 등 자동 호출은 항상 --local-ip 를 전달하므로 진입 안 함.
if [[ $INTERACTIVE == "auto" ]]; then
    if (( _ARGC == 0 )) && [[ -t 0 && -t 1 ]]; then INTERACTIVE="yes"; else INTERACTIVE="no"; fi
fi

# ask VAR "라벨" "표시 기본값" — Enter=현재값 유지, 값 입력=변경, '-'=값 비움(파생 기본값 사용)
ask() {
    local __var=$1 __label=$2 __def=$3 __inp
    read -rp "  ${__label} [${__def}]: " __inp
    if [[ $__inp == "-" ]]; then
        printf -v "$__var" '%s' ""
    elif [[ -n $__inp ]]; then
        printf -v "$__var" '%s' "$__inp"
    fi
}

if [[ $INTERACTIVE == "yes" ]]; then
    echo ""
    echo -e "${BOLD}=== CIMS 시험환경 설정 (대화형) ===${NC}"
    info "Enter=기본값 수락, 값 입력=변경, '-' 입력=파생 기본값으로 되돌림"
    echo ""

    # LOCAL_IP — 저장값 없으면 default route src IP 자동 감지 (cims.sh init 과 동일)
    if [[ -z $LOCAL_IP ]]; then
        LOCAL_IP=$(ip route get 8.8.8.8 2>/dev/null | awk '{for (i=1;i<=NF;i++) if ($i=="src") {print $(i+1); exit}}' || true)
        [[ -z $LOCAL_IP ]] && LOCAL_IP=$(hostname -I 2>/dev/null | awk '{print $1}' || true)
    fi
    _prev_local_ip="$LOCAL_IP"
    ask LOCAL_IP "LOCAL_IP (모든 컴포넌트 기본 IP)" "${LOCAL_IP:-직접 입력}"
    LOCAL_IP="${LOCAL_IP:-$_prev_local_ip}"

    read -rp "  컴포넌트별 IP 분리 설정? (CSP/PSP/ISP/CMP/PMP/IMP/CWRTC/CSC) [y/N]: " _yn
    if [[ $_yn =~ ^[yY] ]]; then
        ask CSP_IP   "CSP_IP   (VoLTE 시그널링)"            "${CSP_IP:-$LOCAL_IP}"
        ask PSP_IP   "PSP_IP   (PTT 시그널링, 기본=CSP)"    "${PSP_IP:-${CSP_IP:-$LOCAL_IP}}"
        ask ISP_IP   "ISP_IP   (IBCF 트렁크, 기본=CSP)"     "${ISP_IP:-${CSP_IP:-$LOCAL_IP}}"
        ask CMP_IP   "CMP_IP   (VoLTE 미디어)"              "${CMP_IP:-$LOCAL_IP}"
        ask PMP_IP   "PMP_IP   (PTT 미디어, 기본=CMP)"      "${PMP_IP:-${CMP_IP:-$LOCAL_IP}}"
        ask IMP_IP   "IMP_IP   (IBCF 미디어, 기본=CMP)"     "${IMP_IP:-${CMP_IP:-$LOCAL_IP}}"
        ask CMDP_IP  "CMDP_IP  (MCData MSRP, 기본=CMP)"     "${CMDP_IP:-${CMP_IP:-$LOCAL_IP}}"
        ask CWRTC_IP "CWRTC_IP (WebRTC)"                    "${CWRTC_IP:-$LOCAL_IP}"
        ask CSC_HOST "CSC_HOST (가입자/MCPTT 서버)"         "${CSC_HOST:-$LOCAL_IP}"
    fi

    ask DB_HOST "DB_HOST (MariaDB 호스트)" "${DB_HOST:-127.0.0.1}"
    ask DB_USER "DB_USER"                  "$DB_USER"
    if [[ $DB_PASSWORD == "cims1234" ]]; then _pw_label="cims1234"; else _pw_label="(이전 설정값)"; fi
    ask DB_PASSWORD "DB_PASSWORD" "$_pw_label"

    ask VOLTE_DOMAIN "VOLTE_DOMAIN (SIP 도메인/인증 Realm)" \
        "${VOLTE_DOMAIN:-ims.mnc033.mcc450.3gppnetwork.org}"
    _volte_eff="${VOLTE_DOMAIN:-ims.mnc033.mcc450.3gppnetwork.org}"
    ask PTT_DOMAIN "PTT_DOMAIN (기본=ims→ptt 치환)" \
        "${PTT_DOMAIN:-$(echo "$_volte_eff" | sed 's/^ims\./ptt./')}"
    ask COUNTRY_CODE "COUNTRY_CODE (홈 국가코드, E.164 digits)" "${COUNTRY_CODE:-82}"

    read -rp "  로그/녹취 디렉터리 변경? [y/N]: " _yn
    if [[ $_yn =~ ^[yY] ]]; then
        ask SERVICE_LOG_DIR "SERVICE_LOG_DIR (서비스 이력/Flow 로그)" \
            "${SERVICE_LOG_DIR:-$DIST_DIR/ext_mnt/service_log}"
        ask MSG_LOG_DIR "MSG_LOG_DIR (메시지 통계, 기본=SERVICE_LOG_DIR)" \
            "${MSG_LOG_DIR:-${SERVICE_LOG_DIR:-$DIST_DIR/ext_mnt/service_log}}"
        ask RECORD_DIR "RECORD_DIR (녹취 파일)" "${RECORD_DIR:-$DIST_DIR/ext_mnt/recordings}"
    fi
    info "JWT 시크릿(IdMS/CIMS)은 매 실행 랜덤 생성 — 고정하려면 --idms-secret/--cims-secret"

    # '-' 리셋 등으로 비워졌을 때 안전망 (아래 파생 단계에 재기본값이 없는 항목)
    DB_USER="${DB_USER:-cims}"
    DB_PASSWORD="${DB_PASSWORD:-cims1234}"
fi

# LOCAL_IP 결정 — 명시 옵션 / env / .cims 어느쪽도 채워지지 않은 경우 abort.
# 127.0.0.1 default 를 의도적으로 제거: dev 가 외부 단말 접속 가능 IP 로 bind
# 하도록 강제 + 배포본 (LocalIp 127.0.0.1) 과의 분리 보장.
if [[ -z $LOCAL_IP ]]; then
    err "LOCAL_IP 미지정 — 다음 중 하나로 결정 필요:"
    err "  1) ./cims.sh init   (권장 — .cims/server.local.json 자동 생성)"
    err "  2) --local-ip <IP>  명시 전달"
    err "  3) CIMS_LOCAL_IP=<IP> 환경변수"
    exit 1
fi

# 파생 전 원시값 캡처 — 대화형 답변 저장용. 빈 값("")은 "파생 기본값 사용" 을
# 의미하므로 파생 결과가 아니라 원시값을 저장해야 LOCAL_IP 변경 시 함께 따라간다.
_raw_csp_ip="$CSP_IP";   _raw_psp_ip="$PSP_IP";   _raw_isp_ip="$ISP_IP"
_raw_cmp_ip="$CMP_IP";   _raw_pmp_ip="$PMP_IP";   _raw_imp_ip="$IMP_IP"
_raw_cmdp_ip="$CMDP_IP"
_raw_cwrtc_ip="$CWRTC_IP"; _raw_csc_host="$CSC_HOST"
_raw_db_host="$DB_HOST"; _raw_db_user="$DB_USER"
_raw_volte_domain="$VOLTE_DOMAIN"; _raw_ptt_domain="$PTT_DOMAIN"
_raw_country_code="$COUNTRY_CODE"
_raw_msg_log_dir="$MSG_LOG_DIR"; _raw_service_log_dir="$SERVICE_LOG_DIR"
_raw_record_dir="$RECORD_DIR"

# 미설정 값은 기본값으로
CSP_IP="${CSP_IP:-$LOCAL_IP}"
PSP_IP="${PSP_IP:-$CSP_IP}"
ISP_IP="${ISP_IP:-$CSP_IP}"
CMP_IP="${CMP_IP:-$LOCAL_IP}"
PMP_IP="${PMP_IP:-$CMP_IP}"
IMP_IP="${IMP_IP:-$CMP_IP}"
CMDP_IP="${CMDP_IP:-$CMP_IP}"
CWRTC_IP="${CWRTC_IP:-$LOCAL_IP}"
CSC_HOST="${CSC_HOST:-$LOCAL_IP}"
CSC_IP="$CSC_HOST"            # 템플릿 @CSC_IP@ 별칭 (csp Setup.Csc.Host)
OAM_IP="${OAM_IP:-$LOCAL_IP}" # 템플릿 @OAM_IP@ — 모듈 FM 자기보고 목적지 (이중화 시 관리평면 VIP)
DB_HOST="${DB_HOST:-127.0.0.1}"
VOLTE_DOMAIN="${VOLTE_DOMAIN:-ims.mnc033.mcc450.3gppnetwork.org}"
PTT_DOMAIN="${PTT_DOMAIN:-$(echo "$VOLTE_DOMAIN" | sed 's/^ims\./ptt./')}"
COUNTRY_CODE="${COUNTRY_CODE:-82}"

# 로그/녹취 디렉터리 기본값
SERVICE_LOG_DIR="${SERVICE_LOG_DIR:-$DIST_DIR/ext_mnt/service_log}"
MSG_LOG_DIR="${MSG_LOG_DIR:-$SERVICE_LOG_DIR}"
RECORD_DIR="${RECORD_DIR:-$DIST_DIR/ext_mnt/recordings}"
mkdir -p "$SERVICE_LOG_DIR" "$RECORD_DIR"

# JWT 시크릿 랜덤 생성 (미설정 시)
if [[ -z "$IDMS_JWT_SECRET" ]]; then
    IDMS_JWT_SECRET="$(openssl rand -base64 32 2>/dev/null || echo 'mcptt_jwt_secret_change_me')"
fi
if [[ -z "$CIMS_JWT_SECRET" ]]; then
    CIMS_JWT_SECRET="$(openssl rand -base64 32 2>/dev/null || echo 'cims_jwt_secret_change_me')"
fi
# CSC↔CSP 내부 API shared secret — csc.json InternalApi.Token = csp.json Setup.Csc.InternalToken
#   (POST /internal/aka/av 의 Bearer, sip_access_security.md §8.2). 두 파일이 한 configure 에서 같은 값으로 렌더된다.
if [[ -z "${INTERNAL_TOKEN:-}" ]]; then
    INTERNAL_TOKEN="$(openssl rand -hex 24 2>/dev/null || echo 'csc_internal_token_change_me')"
fi
# AuC KEK — AKA 가입자 K/OPc 보관 암호화 키. JWT 시크릿과 달리 **재생성하면 안 된다**(보관된 K/OPc 를
#   복호할 수 없게 된다) → 기존 csc.json 의 AuC.Kek 를 이어받고, 없을 때만 새로 만든다.
if [[ -z "${AUC_KEK:-}" && -f "$DIST_DIR/csc/config/csc.json" ]]; then
    AUC_KEK="$(python3 -c 'import json,sys; print((json.load(open(sys.argv[1])).get("AuC") or {}).get("Kek") or "")' \
        "$DIST_DIR/csc/config/csc.json" 2>/dev/null || true)"
fi
if [[ -z "${AUC_KEK:-}" ]]; then
    AUC_KEK="$(openssl rand -hex 16 2>/dev/null || echo '')"
fi

echo ""
info "배포 설정:"
echo "  CSP_IP       = $CSP_IP"
echo "  CMP_IP       = $CMP_IP"
echo "  CMDP_IP      = $CMDP_IP"
echo "  CSC_HOST     = $CSC_HOST"
echo "  DB_HOST      = $DB_HOST / $DB_USER"
echo "  VOLTE_DOMAIN = $VOLTE_DOMAIN"
echo "  PTT_DOMAIN   = $PTT_DOMAIN"
echo "  COUNTRY_CODE = +$COUNTRY_CODE"
echo "  MSG_LOG_DIR     = $MSG_LOG_DIR"
echo "  SERVICE_LOG_DIR = $SERVICE_LOG_DIR"
echo "  RECORD_DIR      = $RECORD_DIR"
echo "  DIST_DIR        = $DIST_DIR"
echo ""

# ── 대화형: 최종 확인 + 답변 저장 ───────────────────────────────
# 답변을 .cims/server.local.json 에 저장 → 재실행/verify 자동 호출의 기본값.
# 명시 옵션으로 들어온 비대화 실행은 저장하지 않는다 (verify 의 --psp-ip
# 127.0.0.x 같은 일회성 값이 기본값으로 굳는 것 방지).
if [[ $INTERACTIVE == "yes" ]]; then
    read -rp "이 설정으로 진행할까요? [Y/n]: " _go
    if [[ ${_go:-} =~ ^[nN] ]]; then
        err "사용자 취소 — 설정 파일 변경 없음"
        exit 1
    fi
    _CIMS_DIR="$(dirname "$_INIT_CFG")"
    mkdir -p "$_CIMS_DIR" && chmod 700 "$_CIMS_DIR" 2>/dev/null || true
    CFG_FILE="$_INIT_CFG" LOCAL_IP_VAL="$LOCAL_IP" DB_PWD_VAL="$DB_PASSWORD" \
    RAW_csp_ip="$_raw_csp_ip" RAW_psp_ip="$_raw_psp_ip" RAW_isp_ip="$_raw_isp_ip" \
    RAW_cmp_ip="$_raw_cmp_ip" RAW_pmp_ip="$_raw_pmp_ip" RAW_imp_ip="$_raw_imp_ip" \
    RAW_cmdp_ip="$_raw_cmdp_ip" \
    RAW_cwrtc_ip="$_raw_cwrtc_ip" RAW_csc_host="$_raw_csc_host" \
    RAW_db_host="$_raw_db_host" RAW_db_user="$_raw_db_user" \
    RAW_volte_domain="$_raw_volte_domain" RAW_ptt_domain="$_raw_ptt_domain" \
    RAW_country_code="$_raw_country_code" \
    RAW_msg_log_dir="$_raw_msg_log_dir" RAW_service_log_dir="$_raw_service_log_dir" \
    RAW_record_dir="$_raw_record_dir" \
    KEYS="$_CFG_SAVED_KEYS" python3 - <<'PY'
import json, os
cfg = os.environ["CFG_FILE"]
try:
    data = json.load(open(cfg))
except Exception:
    data = {}
data["local_ip"] = os.environ["LOCAL_IP_VAL"]
data["db_password"] = os.environ["DB_PWD_VAL"]
data["configure"] = {k: os.environ.get("RAW_" + k, "") for k in os.environ["KEYS"].split()}
with open(cfg, "w", encoding="utf-8") as f:
    json.dump(data, f, ensure_ascii=False, indent=2)
    f.write("\n")
os.chmod(cfg, 0o600)
PY
    ok "대화형 설정 저장됨: $_INIT_CFG (다음 실행 기본값)"
    echo ""
fi

# ── 플레이스홀더 치환 함수 ──────────────────────────────────────
apply_template() {
    local src="$1"
    local dst="$2"
    [[ ! -f "$src" ]] && warn "템플릿 없음: $src" && return
    mkdir -p "$(dirname "$dst")"
    sed \
        -e "s|@CSP_IP@|${CSP_IP}|g" \
        -e "s|@CMP_IP@|${CMP_IP}|g" \
        -e "s|@CMDP_IP@|${CMDP_IP}|g" \
        -e "s|@CWRTC_IP@|${CWRTC_IP}|g" \
        -e "s|@CSC_HOST@|${CSC_HOST}|g" \
        -e "s|@CSC_IP@|${CSC_IP}|g" \
        -e "s|@OAM_IP@|${OAM_IP}|g" \
        -e "s|@DB_HOST@|${DB_HOST}|g" \
        -e "s|@DB_USER@|${DB_USER}|g" \
        -e "s|@DB_PASSWORD@|${DB_PASSWORD}|g" \
        -e "s|@VOLTE_DOMAIN@|${VOLTE_DOMAIN}|g" \
        -e "s|@PTT_DOMAIN@|${PTT_DOMAIN}|g" \
        -e "s|@IDMS_JWT_SECRET@|${IDMS_JWT_SECRET}|g" \
        -e "s|@CIMS_JWT_SECRET@|${CIMS_JWT_SECRET}|g" \
        -e "s|@INTERNAL_TOKEN@|${INTERNAL_TOKEN}|g" \
        -e "s|@AUC_KEK@|${AUC_KEK}|g" \
        -e "s|@MSG_LOG_DIR@|${MSG_LOG_DIR}|g" \
        -e "s|@SERVICE_LOG_DIR@|${SERVICE_LOG_DIR}|g" \
        -e "s|@RECORD_DIR@|${RECORD_DIR}|g" \
        -e "s|@DIST_DIR@|${DIST_DIR}|g" \
        "$src" > "$dst"
    ok "생성: $dst"
}

# ── config_template.json → 런타임 config 생성 (A안 통합 렌더러) ─
#   sections[*].fields 의 dotted key 를 중첩 dict 로 구축. deploy_value 가 있으면 그 값을,
#   아니면 default 를 사용한다. 모든 @VAR@ 플레이스홀더는 환경변수로 치환.
apply_config_template() {
    local src="$1"
    local dst="$2"
    [[ ! -f "$src" ]] && warn "템플릿 없음: $src" && return
    mkdir -p "$(dirname "$dst")"
    CSP_IP="$CSP_IP" PSP_IP="$PSP_IP" ISP_IP="$ISP_IP" \
    CMP_IP="$CMP_IP" PMP_IP="$PMP_IP" IMP_IP="$IMP_IP" \
    CMDP_IP="$CMDP_IP" SYSTEM_ID="${SYSTEM_ID:-cmdp_01}" \
    CWRTC_IP="$CWRTC_IP" CSC_HOST="$CSC_HOST" CSC_IP="$CSC_IP" OAM_IP="$OAM_IP" \
    DB_HOST="$DB_HOST" DB_USER="$DB_USER" DB_PASSWORD="$DB_PASSWORD" \
    VOLTE_DOMAIN="$VOLTE_DOMAIN" PTT_DOMAIN="$PTT_DOMAIN" COUNTRY_CODE="$COUNTRY_CODE" \
    IDMS_JWT_SECRET="$IDMS_JWT_SECRET" CIMS_JWT_SECRET="$CIMS_JWT_SECRET" \
    INTERNAL_TOKEN="$INTERNAL_TOKEN" AUC_KEK="$AUC_KEK" \
    MSG_LOG_DIR="$MSG_LOG_DIR" SERVICE_LOG_DIR="$SERVICE_LOG_DIR" \
    RECORD_DIR="$RECORD_DIR" DIST_DIR="$DIST_DIR" \
    python3 - "$src" "$dst" <<'PY'
import json, os, re, sys

SRC, DST = sys.argv[1], sys.argv[2]
ENV_PAT = re.compile(r'@([A-Z_][A-Z0-9_]*)@')

def subst(v):
    if isinstance(v, str):
        return ENV_PAT.sub(lambda m: os.environ.get(m.group(1), m.group(0)), v)
    if isinstance(v, list):
        return [subst(x) for x in v]
    if isinstance(v, dict):
        return {k: subst(x) for k, x in v.items()}
    return v

def set_path(root, dotted, value):
    cur = root
    keys = dotted.split('.')
    for k in keys[:-1]:
        cur = cur.setdefault(k, {})
    cur[keys[-1]] = value

with open(SRC) as f:
    tpl = json.load(f)

out = {}
for section in tpl.get('sections', []):
    for field in section.get('fields', []):
        key = field.get('key')
        if not key:
            continue
        val = field['deploy_value'] if 'deploy_value' in field else field.get('default')
        set_path(out, key, subst(val))

with open(DST, 'w') as f:
    json.dump(out, f, indent=4, ensure_ascii=False)
PY
    ok "생성: $dst"
}

# ── dist 설정 파일 생성 ─────────────────────────────────────────
apply_config_template "$DIST_DIR/cmp/config/config_template.json"          "$DIST_DIR/cmp/config/cmp.json"
apply_config_template "$DIST_DIR/cmdp/config/config_template.json"         "$DIST_DIR/cmdp/config/cmdp.json"
apply_config_template "$DIST_DIR/csp/config/config_template.json"          "$DIST_DIR/csp/config/csp.json"
apply_config_template "$DIST_DIR/csc/config/config_template.json"          "$DIST_DIR/csc/config/csc.json"

# ── base OAM 노드값 → **overlay** (oam_base_service_split §5, 02_deployment 설정 계층) ──
# 콘솔(4419) 로그인 토큰은 base OAM 이 발급하고, 게이트웨이 프록시 뒤의 csc 가
# 같은 시크릿으로 독립 검증한다. csc.json 은 @CIMS_JWT_SECRET@ 로 렌더되므로
# oam 도 같은 값을 가져야 한다.
#
# 노드 종속 값(경로·시크릿·DB)은 **base(oam/config/oam.json)가 아니라 노드 overlay
# (oam/config.json, 평면 점표기)** 에 쓴다 — base 는 패키지에 실려 나가므로 여기 쓰면
# 빌드 머신 값이 배포된다 (S1-CONFIG-PORTABILITY 게이트가 차단). base 는 소스 정본으로
# 복원해 과거 in-place 패치 잔재를 청소한다. oam_app 이 기동 시 overlay 를 병합한다.
if [[ -d "$DIST_DIR/oam/config" ]]; then
    # base 복원 — 소스 트리에서 실행 중일 때만 (배포본 단독 configure 는 base 를 보존)
    if [[ -f "$SCRIPT_DIR/ems/core/oam/config/oam.json" ]]; then
        cp -f "$SCRIPT_DIR/ems/core/oam/config/oam.json"    "$DIST_DIR/oam/config/oam.json"
        cp -f "$SCRIPT_DIR/ems/core/oam/config/oam-tb.json" "$DIST_DIR/oam/config/oam-tb.json" 2>/dev/null || true
    fi
    OAM_OVL="$DIST_DIR/oam/config.json" SECRET="$CIMS_JWT_SECRET" \
        SVC_LOG_DIR="$SERVICE_LOG_DIR" RUNTIME_DIR="$DIST_DIR/ext_mnt/runtime" \
        DBH="$DB_HOST" DBP="${DB_PORT:-3306}" DBU="$DB_USER" DBW="$DB_PASSWORD" DBN="${DB_NAME:-cims}" \
        CSPIP="$CSP_IP" CMPIP="$CMP_IP" \
        python3 - <<'PY'
import json, os
p = os.environ["OAM_OVL"]
try:
    with open(p) as f:
        ovl = json.load(f)
    if not isinstance(ovl, dict):
        ovl = {}
except Exception:
    ovl = {}
ovl["CimsAuth.JwtSecret"] = os.environ["SECRET"]
ovl["ServiceLogging.Dir"] = os.environ["SVC_LOG_DIR"]
ovl["CimsRuntimeDir"] = os.environ["RUNTIME_DIR"]
# stats 핸들러(_get_db)가 읽는 CimsDatabase — 미주입 시 기본 127.0.0.1 로 접속을 시도해
# 외부 DB 구성에서 통계 API 가 MySQL 에러(500)를 반환한다.
ovl["CimsDatabase"] = {
    "Host": os.environ["DBH"], "Port": int(os.environ["DBP"]),
    "User": os.environ["DBU"], "Password": os.environ["DBW"], "Db": os.environ["DBN"],
}
# 서비스 관측 대상(health/알람 sweeper probe) — 미주입 시 기본 127.0.0.1 인데 CSP 의
# CscInterface 는 LOCAL_IP 에 bind 하므로 probe 가 영구 실패해 health=down + 유령
# process_down 알람이 남는다. CMP 는 Endpoints 비면 관측 자체가 비활성.
ovl["CspNotify.Ip"] = os.environ["CSPIP"]
ovl["MediaServer.Endpoints"] = [{"ip": os.environ["CMPIP"], "port": 9000}]
with open(p, "w") as f:
    json.dump(ovl, f, indent=4, ensure_ascii=False)
    f.write("\n")
print("  oam overlay(config.json) CimsAuth.JwtSecret ← csc 와 동일 값 정렬")
print("  oam overlay ServiceLogging.Dir ← " + os.environ["SVC_LOG_DIR"])
print("  oam overlay CimsRuntimeDir ← " + os.environ["RUNTIME_DIR"])
print("  oam overlay CimsDatabase ← %s@%s" % (os.environ["DBU"], os.environ["DBH"]))
PY
fi

# 자동 프로비저닝(/provisioning/me) 서비스 매핑은 csc config_template 의 `provisioning`
# 섹션이 소유한다 — 위 apply_config_template 이 @VOLTE_DOMAIN@/@PTT_DOMAIN@/@COUNTRY_CODE@
# 를 치환해 csc.json 에 함께 기록한다. 분산 배포(configure 미경유)에서는 콘솔
# [패키지 설정] > csc 에서 편집한다.

# ── TB-CSC (4419) overlay: csc.json 을 기반으로 포트/경로만 치환 ─
#   TB 는 검증 Phase 진행 중 UI 세션 유지용 임시 기동 모듈.
#   DB/시크릿/도메인은 Test-CSC(4421, Phase 1 직접 기동본) 와 공유.
#   포트와 파일 경로만 분리한다.
apply_csc_tb_overlay() {
    local base="$1"    # csc.json
    local dst="$2"     # csc-tb.json
    [[ ! -f "$base" ]] && warn "csc base config 없음: $base" && return
    python3 - "$base" "$dst" <<'PY'
import json, sys
BASE, DST = sys.argv[1], sys.argv[2]
with open(BASE) as f:
    c = json.load(f)
c.setdefault('Server', {})['Port'] = 4419
c.setdefault('McpttServer', {})['Port'] = 4431
c.setdefault('Log', {})['File'] = 'log/csc_tb.log'
c.setdefault('Packages', {})['Dir'] = 'packages_tb'
c['Packages']['BackupDir'] = 'packages_tb_trash'
c['ConfigCacheDir'] = 'cache_tb'
# IdMs.KmsClientReqUrl 을 TB-CSC(4419) 로 치환 (admin 포트는 전 환경 4421 —
# 과거 4420 렌더본 호환을 위해 둘 다 수용).
idms = c.setdefault('IdMs', {})
if isinstance(idms.get('KmsClientReqUrl'), str):
    idms['KmsClientReqUrl'] = (idms['KmsClientReqUrl']
                               .replace(':4420/', ':4419/')
                               .replace(':4421/', ':4419/'))
with open(DST, 'w') as f:
    json.dump(c, f, indent=4, ensure_ascii=False)
PY
    ok "생성: $dst (TB-CSC 4419 overlay)"
}
apply_csc_tb_overlay "$DIST_DIR/csc/config/csc.json" "$DIST_DIR/csc/config/csc-tb.json"

# ── 개발환경 SIP 리스너 시드 (local_nodes.jsonl, non-clobber) ───
# CSP 는 SIP 리스너를 local_nodes 컬렉션에서만 생성하고 primary 부재 시 기동을
# 중단한다 (CspServer.cpp fail-fast). 배포 흐름은 deployment/bin/render.py 가
# 보장하지만 dev(build/dist) 는 configure 가 최초 1회 시드한다. 파일이 이미
# 있으면 건드리지 않는다 — UI(local_nodes 콜렉션)/운영 편집 보존.
seed_local_nodes() {
    local dir="$DIST_DIR/config"
    local file="$dir/local_nodes.jsonl"
    if [[ -f $file ]]; then
        info "local_nodes.jsonl 존재 — 시드 생략: $file"
        return 0
    fi
    mkdir -p "$dir"
    # TLS 리스너는 cert 가 있을 때만 시드 — tls_cert_path 빈 TLS 리스너는
    # SSLServerStart 실패로 CSP 기동이 중단된다 (경로는 render.py 와 동일 관례,
    # 프로세스 cwd = dist/csp 기준 상대경로).
    local tls_note=""
    local has_cert=0
    [[ -f "$DIST_DIR/csp/cert/csp.pem" ]] && has_cert=1 && tls_note=" / TLS 5061"
    CSP_IP="$CSP_IP" FILE="$file" HAS_CERT="$has_cert" python3 - <<'PY'
import json, os, uuid
ip = os.environ["CSP_IP"]
def row(name, port, proto, primary=False, cert=""):
    r = {"id": uuid.uuid4().hex, "name": name, "enabled": True,
         "is_primary": primary, "edge": "access", "bind_ip": ip,
         "bind_port": port, "protocol": proto, "tags": ["dev"],
         "note": "seeded by configure.sh"}
    if cert:
        r["tls_cert_path"] = cert
    return r
rows = [row("access-udp", 5060, "UDP", primary=True),
        row("access-tcp", 25061, "TCP")]
if os.environ["HAS_CERT"] == "1":
    rows.append(row("access-tls", 5061, "TLS", cert="cert/csp.pem"))
with open(os.environ["FILE"], "w", encoding="utf-8") as f:
    for r in rows:
        f.write(json.dumps(r, ensure_ascii=False) + "\n")
PY
    ok "생성: $file (UDP 5060 primary / TCP 25061${tls_note} @ $CSP_IP)"
}
seed_local_nodes

# ── 시험 환경 설정 파일 생성 (소스 트리 tests/ 에만) ────────────
# 테스트가 실제 배포 IP/도메인/DB 를 자동으로 사용하도록 한다.
# 하드코딩 드리프트 방지.
if [[ -n "$SRC_DIR" && -f "$SRC_DIR/tests/test_env.json.template" ]]; then
    apply_template "$SRC_DIR/tests/test_env.json.template"                 "$SRC_DIR/tests/test_env.json"
fi

# ── 개발용 Vite env 파일 생성 (소스 트리에서만) ─────────────────
# write_env_if_changed: 기존 파일과 내용이 같으면 mtime 변경 안 함.
# 이유: vite dev server (TB-Console 3000) 가 .env.* 파일을 watch 하여 mtime
# 변경 시 hot reload → React state 모두 reset. configure 가 매 검증 회차에서
# 호출되므로 idempotent 보장 필수.
write_env_if_changed() {
    local path="$1"
    local content="$2"
    # NOTE: `$(cat file)` 은 trailing newline 을 strip 하므로 직접 비교하면
    # `\n` 으로 끝나는 content 와 항상 불일치. cmp 로 바이트 정확 비교.
    if [[ -f "$path" ]] && printf '%s' "$content" | cmp -s - "$path"; then
        return 0  # 변경 없음 — skip (mtime 보존)
    fi
    printf '%s' "$content" > "$path"
    ok "갱신: $path"
}

if [[ -n "$SRC_DIR" ]]; then
    # CSC SSL 여부 자동 감지
    if [[ -f "$DIST_DIR/csc/cert/server.key" && -f "$DIST_DIR/csc/cert/server.crt" ]]; then
        CSC_SCHEME="https"
    else
        CSC_SCHEME="http"
    fi

    # ems/core/console/.env.local  (Vite dev proxy 대상 — Test-CSC 4421, Phase 1)
    write_env_if_changed "$SRC_DIR/ems/core/console/.env.local" "VITE_ADMIN_TARGET=${CSC_SCHEME}://${CSC_HOST}:4421
"

    # ems/core/console/.env.tb.local  (TB-Console 전용, TB-CSC 4419 로 proxy)
    write_env_if_changed "$SRC_DIR/ems/core/console/.env.tb.local" "VITE_ADMIN_TARGET=${CSC_SCHEME}://127.0.0.1:4419
"

    # cwrtc/cims-phone 은 재설계 예정 — env 렌더 제외 (빌드/패키징도 제외).
fi

# ── DB 접속 권한 SQL 생성 ───────────────────────────────────────
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
ok "설정 완료. 서비스 시작: ./agent/bin/cims-svc start"
info "재설정(configure 재실행)이면 JWT 시크릿이 갱신되므로 발급/검증 양쪽을 함께 재시작:"
info "  ./agent/bin/cims-svc restart csp csc oam   (+ 콘솔 재로그인)"

# DB가 별도 서버인 경우 안내
if [[ "$DB_HOST" != "127.0.0.1" && "$DB_HOST" != "localhost" ]]; then
    echo ""
    echo -e "${YELLOW}[참고]${NC} DB 서버(${DB_HOST})에서 접속 권한 부여 필요:"
    echo "       sudo mysql < $DB_GRANT_SQL"
fi
