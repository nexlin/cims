#!/usr/bin/env bash
# =============================================================
# 18-db-seed.sh — 초기 데이터 직접 입력 (조직 · 가입자 · 가입번호 · PTT 그룹)
#
# data/*.csv 를 읽어 DB 에 바로 넣는다. OAM·CSC 가 떠 있지 않아도 되고 콘솔 조작도
# 필요 없다 — 스키마 생성(15) 직후 곧바로 데이터를 채운다.
#
# ha1(SIP Digest 자격)은 이 단계가 계산한다 — MD5("<imsi>@<domain>:<realm>:<passwd>").
# **그 domain/realm 은 CSP `access_services` 의 같은 서비스 행과 일치해야 한다.**
# 어긋나면 CSP 가 계산하는 realm 과 달라 등록이 401 로 실패한다 (initial_install.md §4.2).
#
# 멱등: 자연키 upsert. 두 번 돌려도 행이 늘지 않고 바뀐 값만 갱신된다.
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

tb_load_site
tb_init_dirs
tb_require_python

header "=== [18] 초기 데이터 입력 ==="

DATA_DIR="${TB_DATA_DIR:-$TB_ROOT/data}"
[[ -d "$DATA_DIR" ]] || die_hint "데이터 디렉토리 없음: $DATA_DIR" \
    "반입본의 data/ 에 CSV 를 채워 넣으세요 (템플릿이 함께 들어 있습니다)."

# ── 사이트 값 ─────────────────────────────────────────────────
tb_ask TB_DB_HOST     "DB 호스트" "127.0.0.1"
tb_ask TB_DB_PORT     "DB 포트" "3306"
tb_ask TB_DB_NAME     "DB 이름" "cims"
tb_ask TB_DB_APP_USER "앱(모듈) 계정" "cims"
tb_ask TB_DB_APP_PASS "앱 계정 비밀번호" "" secret

# ha1 결박 재료 — access_services 의 domain/auth_realm 과 같은 값이어야 한다.
# realm 을 비우면 domain 을 쓴다 (CSP EffectiveRealm 규칙과 동일).
tb_ask TB_PTT_DOMAIN   "PTT 도메인 (access_services 의 ptt 행 domain)" "ptt.mnc033.mcc450.3gppnetwork.org"
tb_ask TB_PTT_REALM    "PTT auth realm (비우면 도메인과 동일)" "-"
tb_ask TB_VOLTE_DOMAIN "VoLTE 도메인 (access_services 의 volte 행 domain)" "ims.mnc033.mcc450.3gppnetwork.org"
tb_ask TB_VOLTE_REALM  "VoLTE auth realm (비우면 도메인과 동일)" "-"

# '-' 는 "비움" 표시 — tb_ask 가 빈 값을 허용하지 않아서 쓰는 관례다.
ptt_realm="$TB_PTT_REALM";     [[ "$ptt_realm" == "-" ]] && ptt_realm=""
volte_realm="$TB_VOLTE_REALM"; [[ "$volte_realm" == "-" ]] && volte_realm=""

seed_args=(--data-dir "$DATA_DIR"
           --host "$TB_DB_HOST" --port "$TB_DB_PORT"
           --db "$TB_DB_NAME" --user "$TB_DB_APP_USER"
           --ptt-domain "$TB_PTT_DOMAIN" --ptt-realm "$ptt_realm"
           --volte-domain "$TB_VOLTE_DOMAIN" --volte-realm "$volte_realm")

# ── ① 계획 확인 (dry-run) ─────────────────────────────────────
info "[1/3] 계획 확인 (아무것도 쓰지 않음)"
tb_tee 18-seed env TB_DB_APP_PASS="$TB_DB_APP_PASS" \
        python3 "$_HERE/../lib/tb_seed.py" "${seed_args[@]}" --dry-run \
    || die "계획 단계에서 실패 — CSV 를 고친 뒤 다시 실행하세요"

if [[ "${TB_BATCH:-0}" != "1" ]]; then
    read -r -p "  위 계획대로 넣을까요? [y/N]: " a || die "입력이 끊겼습니다"
    [[ "$a" =~ ^[Yy]$ ]] || die "중단했습니다 (아무것도 쓰지 않았습니다)"
fi

# ── ② 입력 ────────────────────────────────────────────────────
info "[2/3] 입력"
tb_tee 18-seed env TB_DB_APP_PASS="$TB_DB_APP_PASS" \
        python3 "$_HERE/../lib/tb_seed.py" "${seed_args[@]}" \
    || die "입력 실패 — 트랜잭션은 되돌려졌습니다(아무것도 반영 안 됨)"

# ── ③ 확인 ────────────────────────────────────────────────────
info "[3/3] 확인"
if ! tb_tee 18-seed env TB_DB_APP_PASS="$TB_DB_APP_PASS" \
        python3 "$_HERE/../lib/tb_seed.py" \
        --data-dir "$DATA_DIR" --host "$TB_DB_HOST" --port "$TB_DB_PORT" \
        --db "$TB_DB_NAME" --user "$TB_DB_APP_USER" --check-only; then
    die "ha1 이 비어 있는 가입번호가 있습니다 — subscriptions.csv 의 imsi·passwd 를 확인하세요"
fi

header "[18] 완료"
cat <<EOF
  주의 — 이 값들은 CSP·CSC 설정과 맞아야 합니다:
    · subscriptions.csv 의 service_ref  = access_services 의 서비스 이름
    · PTT/VoLTE 도메인                  = access_services 의 domain
      (어긋나면 ha1 이 CSP 계산과 달라져 등록이 401)
  단말 로그인(IdMS) 자격 = users.csv 의 login_id / passwd
EOF
