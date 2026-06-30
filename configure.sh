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
CYAN='\033[0;36m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BOLD='\033[1m'; NC='\033[0m'
info()   { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()     { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()   { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()    { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# ── .cims/server.local.json 우선 read (cims.sh init 결과) ─────
# 우선순위: 명시 옵션 > env (CIMS_LOCAL_IP 등) > .cims/server.local.json
# 본 시점엔 env 와 .cims 만 default 로 채워두고, --local-ip 등 명시 옵션이
# 들어오면 아래 인수 파싱 단계에서 덮어쓴다.
_INIT_CFG="${SRC_DIR:-$SCRIPT_DIR}/.cims/server.local.json"
_init_local_ip=""
_init_db_password=""
if [[ -f $_INIT_CFG ]]; then
    _init_local_ip=$(CFG="$_INIT_CFG" python3 -c \
        'import json,os; print(json.load(open(os.environ["CFG"])).get("local_ip",""))' \
        2>/dev/null || true)
    _init_db_password=$(CFG="$_INIT_CFG" python3 -c \
        'import json,os; print(json.load(open(os.environ["CFG"])).get("db_password",""))' \
        2>/dev/null || true)
fi

# ── 기본값 ─────────────────────────────────────────────────────
# LOCAL_IP 의 default 결정 — env > .cims > "" (빈 값이면 인수 파싱 후 abort)
LOCAL_IP="${CIMS_LOCAL_IP:-${_init_local_ip:-}}"
CSP_IP=""
PSP_IP=""    # PSP (PTT 시그널링) — 미설정 시 CSP_IP 따름
ISP_IP=""    # ISP (IBCF 트렁크) — 미설정 시 CSP_IP 따름 (P2)
CMP_IP=""
PMP_IP=""    # PMP (PTT 미디어) — 미설정 시 CMP_IP 따름
IMP_IP=""    # IMP (IBCF 미디어) — 미설정 시 CMP_IP 따름 (P2)
CWRTC_IP=""
CSC_HOST=""
DB_HOST=""
DB_USER="cims"
DB_PASSWORD="${CIMS_DB_PASSWORD:-${_init_db_password:-cims1234}}"
VOLTE_DOMAIN=""
PTT_DOMAIN=""
IDMS_JWT_SECRET=""
CIMS_JWT_SECRET=""
MSG_LOG_DIR=""
SERVICE_LOG_DIR=""
RECORD_DIR=""

usage() {
    cat <<EOF
${BOLD}CIMS 배포 설정 스크립트${NC}

사용법: $(basename "$0") [options]

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
  --cwrtc-ip   IP    CWRTC 서버 IP
  --csc-host   HOST  CSC 서버 호스트명/IP

${BOLD}데이터베이스:${NC}
  --db-host    HOST  MariaDB 호스트 (기본: 127.0.0.1)
  --db-user    USER  DB 사용자 (기본: cims)
  --db-password PWD  DB 비밀번호 (기본: cims1234)

${BOLD}도메인:${NC}
  --volte-domain DOM  VoLTE SIP 도메인 / 인증 Realm (기본: ims.mnc001.mcc001.3gppnetwork.org)
  --ptt-domain   DOM  PTT 그룹 통화 SIP 도메인 (기본: volte-domain의 ims→ptt 치환)

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
while [[ $# -gt 0 ]]; do
    case "$1" in
        --local-ip)     LOCAL_IP="$2";      shift 2 ;;
        --csp-ip)       CSP_IP="$2";        shift 2 ;;
        --psp-ip)       PSP_IP="$2";        shift 2 ;;
        --isp-ip)       ISP_IP="$2";        shift 2 ;;
        --cmp-ip)       CMP_IP="$2";        shift 2 ;;
        --pmp-ip)       PMP_IP="$2";        shift 2 ;;
        --imp-ip)       IMP_IP="$2";        shift 2 ;;
        --cwrtc-ip)     CWRTC_IP="$2";      shift 2 ;;
        --csc-host)     CSC_HOST="$2";      shift 2 ;;
        --db-host)      DB_HOST="$2";       shift 2 ;;
        --db-user)      DB_USER="$2";       shift 2 ;;
        --db-password)  DB_PASSWORD="$2";   shift 2 ;;
        --volte-domain) VOLTE_DOMAIN="$2";  shift 2 ;;
        --ptt-domain)   PTT_DOMAIN="$2";    shift 2 ;;
        --msg-log-dir)      MSG_LOG_DIR="$2";       shift 2 ;;
        --service-log-dir)  SERVICE_LOG_DIR="$2";   shift 2 ;;
        --record-dir)       RECORD_DIR="$2";        shift 2 ;;
        --idms-secret)      IDMS_JWT_SECRET="$2";   shift 2 ;;
        --cims-secret)  CIMS_JWT_SECRET="$2"; shift 2 ;;
        --help|-h)      usage; exit 0 ;;
        *) echo "알 수 없는 옵션: $1"; echo ""; usage; exit 1 ;;
    esac
done

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

# 미설정 값은 기본값으로
CSP_IP="${CSP_IP:-$LOCAL_IP}"
PSP_IP="${PSP_IP:-$CSP_IP}"
ISP_IP="${ISP_IP:-$CSP_IP}"
CMP_IP="${CMP_IP:-$LOCAL_IP}"
PMP_IP="${PMP_IP:-$CMP_IP}"
IMP_IP="${IMP_IP:-$CMP_IP}"
CWRTC_IP="${CWRTC_IP:-$LOCAL_IP}"
CSC_HOST="${CSC_HOST:-$LOCAL_IP}"
DB_HOST="${DB_HOST:-127.0.0.1}"
VOLTE_DOMAIN="${VOLTE_DOMAIN:-ims.mnc033.mcc450.3gppnetwork.org}"
PTT_DOMAIN="${PTT_DOMAIN:-$(echo "$VOLTE_DOMAIN" | sed 's/^ims\./ptt./')}"

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
# CSC↔CSP 내부 API shared secret (loopback only + header token 2중 보호용)
if [[ -z "${INTERNAL_TOKEN:-}" ]]; then
    INTERNAL_TOKEN="$(openssl rand -hex 24 2>/dev/null || echo 'csc_internal_token_change_me')"
fi

echo ""
info "배포 설정:"
echo "  CSP_IP       = $CSP_IP"
echo "  CMP_IP       = $CMP_IP"
echo "  CWRTC_IP     = $CWRTC_IP"
echo "  CSC_HOST     = $CSC_HOST"
echo "  DB_HOST      = $DB_HOST / $DB_USER"
echo "  VOLTE_DOMAIN = $VOLTE_DOMAIN"
echo "  PTT_DOMAIN   = $PTT_DOMAIN"
echo "  MSG_LOG_DIR     = $MSG_LOG_DIR"
echo "  SERVICE_LOG_DIR = $SERVICE_LOG_DIR"
echo "  RECORD_DIR      = $RECORD_DIR"
echo "  DIST_DIR        = $DIST_DIR"
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
        -e "s|@VOLTE_DOMAIN@|${VOLTE_DOMAIN}|g" \
        -e "s|@PTT_DOMAIN@|${PTT_DOMAIN}|g" \
        -e "s|@IDMS_JWT_SECRET@|${IDMS_JWT_SECRET}|g" \
        -e "s|@CIMS_JWT_SECRET@|${CIMS_JWT_SECRET}|g" \
        -e "s|@INTERNAL_TOKEN@|${INTERNAL_TOKEN}|g" \
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
    CWRTC_IP="$CWRTC_IP" CSC_HOST="$CSC_HOST" \
    DB_HOST="$DB_HOST" DB_USER="$DB_USER" DB_PASSWORD="$DB_PASSWORD" \
    VOLTE_DOMAIN="$VOLTE_DOMAIN" PTT_DOMAIN="$PTT_DOMAIN" \
    IDMS_JWT_SECRET="$IDMS_JWT_SECRET" CIMS_JWT_SECRET="$CIMS_JWT_SECRET" \
    INTERNAL_TOKEN="$INTERNAL_TOKEN" \
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
apply_config_template "$DIST_DIR/csp/config/config_template.json"          "$DIST_DIR/csp/config/csp.json"
apply_template "$DIST_DIR/cwrtc/config/cwrtc.json.template"                "$DIST_DIR/cwrtc/config/cwrtc.json"
apply_config_template "$DIST_DIR/csc/config/config_template.json"          "$DIST_DIR/csc/config/csc.json"

# ── 자동 프로비저닝(/provisioning/me) 서비스 매핑 주입 (android_ue_provisioning.md §3) ─
#   서비스 kind 별 시그널링 도메인/포트. host 빈값 = 단말이 접속한 CSC Host(올인원 기본).
#   다중 노드면 host 를 CSP/PSP 대표(VIP) 주소로 채운다(여기선 LOCAL_IP, 빈값 유지도 가능).
python3 - "$DIST_DIR/csc/config/csc.json" "$VOLTE_DOMAIN" "$PTT_DOMAIN" <<'PY'
import json, sys
path, volte_dom, ptt_dom = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path) as f: c = json.load(f)
c["Provisioning"] = {"Services": {
    "volte": {"host": "", "port": 15060, "transport": "UDP", "domain": volte_dom},
    "ptt":   {"host": "", "port": 15060, "transport": "UDP", "domain": ptt_dom},
}}
with open(path, "w") as f: json.dump(c, f, indent=4, ensure_ascii=False); f.write("\n")
print("  csc.json Provisioning 주입: volte=%s ptt=%s" % (volte_dom, ptt_dom))
PY

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
# IdMs.KmsClientReqUrl 을 TB-CSC(4419) 로. base csc.json 의 deploy_value
# 는 운영 포트(4420) 를 유지하므로 여기서 4420/4421 둘 다 4419 로 치환.
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

    # cwrtc WSS 여부 자동 감지
    CWRTC_WS_SCHEME="ws"
    if [[ -f "$DIST_DIR/cwrtc/cert/csp.pem" ]]; then
        CWRTC_WS_SCHEME="wss"
    fi

    # cims-phone/.env.local  (Test-CSC 4421 admin + Test-MCPTT 4430 + Test-CWRTC 8443)
    write_env_if_changed "$SRC_DIR/cims-phone/.env.local" "VITE_ADMIN_TARGET=${CSC_SCHEME}://${CSC_HOST}:4421
VITE_MCPTT_TARGET=${CSC_SCHEME}://${CSC_HOST}:4430
VITE_CWRTC_TARGET=${CWRTC_WS_SCHEME}://${CWRTC_IP}:8443
"
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
ok "설정 완료. 서비스 시작: ./cims.sh start"

# DB가 별도 서버인 경우 안내
if [[ "$DB_HOST" != "127.0.0.1" && "$DB_HOST" != "localhost" ]]; then
    echo ""
    echo -e "${YELLOW}[참고]${NC} DB 서버(${DB_HOST})에서 접속 권한 부여 필요:"
    echo "       sudo mysql < $DB_GRANT_SQL"
fi
