#!/usr/bin/env bash
# =============================================================
# 15-db-schema.sh — DB · 앱 계정 · 스키마 생성 (initial_install.md §1)
#
# 하는 일
#   ① db_bootstrap.py 를 비대화식으로 호출 — DB(utf8mb4) + 앱 계정 + 통합 스키마
#   ② 앱 계정으로 실제 접속해 테이블 수 확인 (모듈이 쓸 자격 그대로 검증)
#   ③ 10 단계가 만든 임시 관리 계정 회수
#
# DB 장비에서 실행해도 되고, DB 가 별도 장비면 관리망이 닿는 장비에서 실행해도 된다
# (db_bootstrap.py 가 TCP 접속이므로). 별도 장비에서 돌릴 때는 10 단계가 만든
# state/db-admin.env 를 함께 옮기거나, 관리 계정 자격을 물어보는 대로 입력한다.
#
# 원본 불변: db_bootstrap.py 와 sql/cims_schema.sql 은 고치지 않고 그대로 호출한다.
# 비밀번호는 argv 에 싣지 않고 환경변수로만 넘긴다 (db_bootstrap 이 지원).
#
# 멱등: db_bootstrap.py 자체가 CREATE ... IF NOT EXISTS 계열이라 재실행 안전하다.
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

tb_load_site
tb_init_dirs
tb_require_python

header "=== [15] DB · 계정 · 스키마 ==="

# ── 반입본 위치 ───────────────────────────────────────────────
bootstrap_py="$(tb_find_offline db-bootstrap/db_bootstrap.py \
                    deployment/db-bootstrap/db_bootstrap.py || true)"
[[ -n "$bootstrap_py" ]] || die_hint "db_bootstrap.py 를 찾을 수 없습니다" \
    "반입본에 포함시키세요 — 빌드 장비에서:" \
    "  deployment/tb/tools/tb-pack.sh" \
    "그러면 offline/db-bootstrap/ 에 db_bootstrap.py + cims_schema.sql + pymysql 이 담깁니다."

schema_sql="$(tb_find_offline db-bootstrap/cims_schema.sql sql/cims_schema.sql || true)"
[[ -n "$schema_sql" ]] || die "cims_schema.sql 을 찾을 수 없습니다 (반입본 확인)"

# ── 사이트 값 ─────────────────────────────────────────────────
# DB 위치는 바뀔 수 있다 — 이 장비에 있든 별도 장비에 있든 같은 스크립트가 돈다.
tb_ask TB_DB_HOST       "DB 호스트 (이 장비면 127.0.0.1)" "127.0.0.1"
tb_ask TB_DB_PORT       "DB 포트" "3306"
tb_ask TB_DB_NAME       "DB 이름" "cims"
tb_ask TB_DB_APP_USER   "앱(모듈) 계정" "cims"
tb_ask TB_DB_APP_PASS   "앱 계정 비밀번호" "" secret
tb_ask TB_DB_GRANT_HOST "앱 계정 접속 허용 host (%=모든 호스트)" "%"

# DB 처리 방식 — 이미 돌고 있는 DB 를 그대로 쓰는 경우가 실제로 있다.
#   create : DB·앱계정을 만든다(없으면). 관리 계정이 필요하다. 초도 설치의 기본.
#   reuse  : 기존 DB·계정을 그대로 쓰고 **스키마만 보정**한다. 관리 계정이 필요 없다.
# reuse 를 두는 이유는 계정 보호다 — db_bootstrap 은 앱 계정에 ALTER USER 로 비밀번호를
# 다시 심는데(코드 159행), 돌고 있는 DB 에 그러면 모듈들이 일제히 인증 실패한다.
# reuse 는 앱 계정명을 비워 넘겨 계정 단계를 건너뛴다(db_bootstrap 의 `if appusr:` 분기).
# 스키마는 전부 CREATE TABLE IF NOT EXISTS + INSERT IGNORE 라 재적용이 무해하다.
tb_ask TB_DB_MODE "DB 처리 방식 (create=새로 만든다 / reuse=기존 DB·계정 그대로, 스키마만)" "create"
case "$TB_DB_MODE" in
    create|reuse) ;;
    *) die "TB_DB_MODE 는 create 또는 reuse 여야 합니다 (현재: $TB_DB_MODE)" ;;
esac

admin_state="$TB_STATE_DIR/db-admin.env"

if [[ "$TB_DB_MODE" == "reuse" ]]; then
    # 앱 계정 자체가 자기 DB 에 대해 전권(ALL PRIVILEGES ON <db>.*)을 갖는다는 전제 —
    # initial_install.md §1 의 (b) 경로다. 별도 관리 계정을 발급하지 않는다.
    info "모드 reuse — 기존 DB·계정 유지, 스키마만 보정 (관리 계정 미사용)"
    TB_DB_ADMIN_USER="$TB_DB_APP_USER"
    TB_DB_ADMIN_PASS="$TB_DB_APP_PASS"
    TB_DB_ADMIN_TEMPORARY=0
    SKIP_APP_USER=1
elif [[ -f "$admin_state" ]]; then
    # shellcheck disable=SC1090
    source "$admin_state"
    info "관리 계정: ${TB_DB_ADMIN_USER}@${TB_DB_ADMIN_GRANT_HOST:-?} (10 단계 발급본)"
else
    warn "10 단계의 관리 계정 정보가 없습니다 ($admin_state)"
    warn "이미 있는 DB 를 쓰거나 DB 가 별도 장비인 경우입니다 — 자격을 입력하세요"
    tb_ask TB_DB_ADMIN_USER "DB 관리자(생성 권한) 계정" "root"
    tb_ask TB_DB_ADMIN_PASS "DB 관리자 비밀번호" "" secret
    TB_DB_ADMIN_TEMPORARY=0
fi
[[ -n "${TB_DB_ADMIN_PASS:-}" ]] || die "관리 계정 비밀번호가 비어 있습니다"

# 관리 비밀번호는 tb-site.conf 에 남기지 않는다(임시 자격) — tb_ask 가 저장했으면 지운다.
if [[ "${TB_DB_ADMIN_TEMPORARY:-0}" != "0" ]] && grep -q '^TB_DB_ADMIN_PASS=' "$TB_SITE_CONF" 2>/dev/null; then
    sed -i '/^TB_DB_ADMIN_PASS=/d' "$TB_SITE_CONF"
fi

# ── ① db_bootstrap.py ─────────────────────────────────────────
# 비밀번호는 argv 에 싣지 않고 환경변수로 넘긴다 (db_bootstrap 이 지원 — CIMS_DB_*).
bs_args=(--yes --host "$TB_DB_HOST" --port "$TB_DB_PORT"
         --admin-user "$TB_DB_ADMIN_USER" --db "$TB_DB_NAME"
         --grant-host "$TB_DB_GRANT_HOST" --schema "$schema_sql")
if [[ "${SKIP_APP_USER:-0}" == "1" ]]; then
    # --app-user 를 주지 않고 환경변수를 빈 값으로 → 계정 생성/비번 변경 단계를 건너뛴다.
    info "[1/3] 스키마 보정만 — ${TB_DB_ADMIN_USER}@${TB_DB_HOST}:${TB_DB_PORT}/${TB_DB_NAME} (계정 미변경)"
    export CIMS_DB_APP_USER=""
else
    info "[1/3] DB · 앱 계정 · 스키마 — ${TB_DB_ADMIN_USER}@${TB_DB_HOST}:${TB_DB_PORT}"
    bs_args+=(--app-user "$TB_DB_APP_USER")
fi
if ! CIMS_DB_ADMIN_PASS="$TB_DB_ADMIN_PASS" CIMS_DB_APP_PASS="$TB_DB_APP_PASS" \
     python3 "$bootstrap_py" "${bs_args[@]}" 2>&1 | tee "$TB_LOG_DIR/15-db-bootstrap.log"
then
    die_hint "db_bootstrap.py 실패 — 로그: $TB_LOG_DIR/15-db-bootstrap.log" \
        "관리자 접속 실패라면 db_bootstrap 은 TCP 전용임을 유념하세요." \
        "root@localhost 가 unix_socket 인증이면 10 단계를 먼저 실행하세요."
fi

# ── ② 앱 계정으로 실제 접속 검증 ──────────────────────────────
# 모듈(csp/csc/oam)이 쓸 자격 그대로 붙어본다 — grant-host 를 잘못 준 경우가 여기서 잡힌다.
info "[2/3] 앱 계정 접속 검증 — ${TB_DB_APP_USER}@${TB_DB_HOST}:${TB_DB_PORT}/${TB_DB_NAME}"
verify_out="$(
  BP="$bootstrap_py" H="$TB_DB_HOST" P="$TB_DB_PORT" U="$TB_DB_APP_USER" \
  PW="$TB_DB_APP_PASS" D="$TB_DB_NAME" python3 - <<'PY' 2>&1
import os, sys
# pymysql 은 db_bootstrap 과 같은 vendor 후보 경로에서 찾는다 (폐쇄망 — pip 없음).
here = os.path.dirname(os.path.abspath(os.environ['BP']))
for c in (os.path.join(here, 'vendor'),
          os.path.join(here, '..', '..', 'ems', 'core', 'oam', 'vendor'),
          os.path.join(here, '..', '..', 'csc', 'vendor'),
          '/opt/cims-agent/modules/oam/current/oam/vendor'):
    if os.path.isdir(os.path.join(c, 'pymysql')):
        sys.path.insert(0, os.path.abspath(c)); break
try:
    import pymysql
except Exception as e:
    print("SKIP pymysql 없음 (%s)" % e); raise SystemExit(0)
try:
    cn = pymysql.connect(host=os.environ['H'], port=int(os.environ['P']),
                         user=os.environ['U'], password=os.environ['PW'],
                         database=os.environ['D'], charset='utf8mb4')
except Exception as e:
    print("FAIL %s" % e); raise SystemExit(1)
with cn.cursor() as cur:
    cur.execute("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=%s",
                (os.environ['D'],))
    n = cur.fetchone()[0]
    cur.execute("SELECT @@version")
    v = cur.fetchone()[0]
cn.close()
print("OK 테이블 %d개 · MariaDB %s" % (n, v))
raise SystemExit(0 if n > 0 else 2)
PY
)" && verify_rc=0 || verify_rc=$?

case "${verify_rc:-1}" in
    0) case "$verify_out" in
           SKIP*) warn "$verify_out — 접속 검증을 건너뜁니다" ;;
           *)     ok "$verify_out" ;;
       esac ;;
    2) err "$verify_out"; die "스키마가 비어 있습니다 — db_bootstrap 로그를 확인하세요" ;;
    *) err "$verify_out"
       die_hint "앱 계정으로 붙지 못했습니다" \
           "허용 host 가 '$TB_DB_GRANT_HOST' 인데 이 장비에서 붙고 있는지 확인하세요." \
           "모듈이 다른 장비에 있으면 % 또는 해당 대역이어야 합니다." ;;
esac

# ── ③ 임시 관리 계정 회수 ────────────────────────────────────
if [[ "${TB_DB_ADMIN_TEMPORARY:-0}" == "1" ]]; then
    # 마지막 안전망 — 아래 DROP 이 앱 계정을 지우는 일은 없어야 한다. 10 단계가 이미
    # 막지만, tb-site.conf 를 손으로 고쳐 두 이름이 같아진 경우가 여기로 온다.
    if [[ "$TB_DB_ADMIN_USER" == "$TB_DB_APP_USER" ]]; then
        warn "임시 관리 계정과 앱 계정이 같은 이름입니다 ('$TB_DB_ADMIN_USER') — 회수를 건너뜁니다"
        warn "    DROP 하면 모듈이 쓸 계정이 사라집니다. 관리 권한은 손으로 회수하세요:"
        warn "    REVOKE ALL PRIVILEGES ON *.* FROM '$TB_DB_ADMIN_USER'@'$TB_DB_ADMIN_GRANT_HOST';"
        rm -f "$admin_state" 2>/dev/null || true
        ok "[15] 완료"
        exit 0
    fi
    info "[3/3] 임시 관리 계정 회수 — ${TB_DB_ADMIN_USER}@${TB_DB_ADMIN_GRANT_HOST}"
    if BP="$bootstrap_py" H="$TB_DB_HOST" P="$TB_DB_PORT" \
       U="$TB_DB_ADMIN_USER" PW="$TB_DB_ADMIN_PASS" GH="$TB_DB_ADMIN_GRANT_HOST" \
       python3 - <<'PY'
import os, sys
here = os.path.dirname(os.path.abspath(os.environ['BP']))
for c in (os.path.join(here, 'vendor'),
          os.path.join(here, '..', '..', 'ems', 'core', 'oam', 'vendor'),
          os.path.join(here, '..', '..', 'csc', 'vendor'),
          '/opt/cims-agent/modules/oam/current/oam/vendor'):
    if os.path.isdir(os.path.join(c, 'pymysql')):
        sys.path.insert(0, os.path.abspath(c)); break
import pymysql
cn = pymysql.connect(host=os.environ['H'], port=int(os.environ['P']),
                     user=os.environ['U'], password=os.environ['PW'],
                     charset='utf8mb4', autocommit=True)
with cn.cursor() as cur:
    cur.execute("DROP USER IF EXISTS '%s'@'%s'" % (os.environ['U'], os.environ['GH']))
cn.close()
PY
    then
        rm -f "$admin_state"
        ok "회수 완료 (state/db-admin.env 삭제)"
    else
        warn "회수 실패 — 수동으로 지우세요:"
        echo "        sudo mariadb -e \"DROP USER '$TB_DB_ADMIN_USER'@'$TB_DB_ADMIN_GRANT_HOST';\""
    fi
else
    info "[3/3] 임시 계정 없음 — 회수 건너뜀"
fi

header "[15] 완료"
cat <<EOF
  모듈 설정에 넣을 DB 값 (콘솔 [패키지 설정] · blueprint):
    host = $TB_DB_HOST   port = $TB_DB_PORT   db = $TB_DB_NAME
    user = $TB_DB_APP_USER   password = (tb-site.conf 의 TB_DB_APP_PASS)

  다음: 관리 서버(OAM) 설치 — 20 단계 (준비 중)
EOF
