#!/usr/bin/env bash
# =============================================================
# 10-db-setup.sh — MariaDB 서버 초기 설정 (DB 장비에서 실행)
#
# 하는 일
#   ① 서비스 기동 + 부팅 시 자동 기동
#   ② 접속 주소(bind-address) 설정 — 다른 장비의 모듈이 붙을 수 있게
#   ③ TCP 관리 계정 임시 발급
#
# ③ 이 왜 필요한가: 배포판 기본 MariaDB 는 root@localhost 가 unix_socket 인증이라
# TCP 로 붙을 수 없다. 그런데 다음 단계 db_bootstrap.py 는 TCP 전용이다
# (initial_install.md §1 의 (a) 경로를 자동화한 것). 스키마 적용이 끝나면
# 15 단계가 이 계정을 회수한다.
#
# 설정은 배포판 conffile 을 고치지 않고 drop-in 파일로 넣는다 — 원본 불변.
#
# 멱등: 여러 번 실행해도 안전하다 (계정은 비밀번호만 갱신).
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

tb_load_site
tb_init_dirs

header "=== [10] MariaDB 초기 설정 ==="

command -v mariadbd >/dev/null || die_hint "MariaDB 서버가 설치되지 않았습니다" \
    "먼저 05 단계를 실행하세요: sudo ./tb-install.sh --role db-server"
tb_require_root

# ── 사이트 값 ─────────────────────────────────────────────────
# DB 는 이 장비일 수도, 별도 장비일 수도 있다. 이 스크립트는 "DB 가 올라가는 장비"
# 에서 돌고, 어디서 붙어올 것인지(bind-address / 관리계정 허용 host)를 값으로 받는다.
tb_ask TB_DB_PORT            "DB 포트" "3306"
tb_ask TB_DB_BIND_ADDRESS    "DB 접속 허용 주소 (0.0.0.0=모든 NIC, 127.0.0.1=이 장비만)" "0.0.0.0"
tb_ask TB_DB_ADMIN_USER      "임시 관리 계정명 (스키마 적용용, 끝나면 회수)" "cimsadm"
tb_ask TB_DB_ADMIN_GRANT_HOST "임시 관리 계정 접속 허용 host (스키마를 적용할 장비)" \
    "$([[ "$TB_DB_BIND_ADDRESS" == "127.0.0.1" ]] && echo 127.0.0.1 || echo '%')"

# 앱(모듈) 계정과 **이름이 같으면 안 된다.** 15 단계가 스키마 적용 후 임시 계정을
# `DROP USER '<관리계정>'@'<허용host>'` 로 회수하는데, 두 계정이 같으면 그 DROP 이
# 모듈이 쓸 계정을 지운다 — 설치는 끝난 것처럼 보이고 기동에서 인증만 깨진다.
_app_user="${TB_DB_APP_USER:-cims}"
if [[ "$TB_DB_ADMIN_USER" == "$_app_user" ]]; then
    die_hint "임시 관리 계정명이 앱 계정과 같습니다 ('$TB_DB_ADMIN_USER')" \
        "15 단계가 스키마 적용 후 이 계정을 DROP 하므로, 같은 이름이면 모듈이 쓸" \
        "계정이 함께 사라집니다. 다른 이름을 쓰세요 (기본값: cimsadm)." \
        "이미 tb-site.conf 에 들어갔다면 그 값을 고치세요:" \
        "  sudo sed -i 's/^TB_DB_ADMIN_USER=.*/TB_DB_ADMIN_USER=cimsadm/' $TB_SITE_CONF"
fi

# ── ① 서비스 ──────────────────────────────────────────────────
info "[1/4] 서비스 기동"
# 기동 전에 초기화 여부를 본다 — 시스템 DB 가 없으면 systemd start 는 반드시 실패하고,
# 원인이 로그 깊숙이 있어서 여기서 짚어주는 편이 훨씬 빠르다.
if ! tb_db_initialized; then
    err "시스템 DB 가 없어 MariaDB 를 띄울 수 없습니다"
    tb_db_init_hint
    die "위 복구를 먼저 하세요"
fi
systemctl enable mariadb >/dev/null 2>&1 || true
if ! tb_db_active; then
    tb_run 10-mariadb-start systemctl start mariadb \
        || die_hint "MariaDB 기동 실패" "확인: journalctl -u mariadb -n 50"
fi
# `mariadbd --version` 은 바이너리만 보므로 서버가 죽어도 통과한다 — 실제 상태로 판정한다.
tb_db_active || die_hint "MariaDB 가 active 가 아닙니다 (start 는 오류를 내지 않았습니다)" \
    "확인: systemctl status mariadb / journalctl -u mariadb -n 50"
ok "mariadb active ($(mariadbd --version | sed 's/.*Ver \([^ ]*\).*/\1/'))"

# ── ② bind-address ────────────────────────────────────────────
info "[2/4] 접속 주소 설정 — bind-address=$TB_DB_BIND_ADDRESS, port=$TB_DB_PORT"
conf_d=/etc/mysql/mariadb.conf.d
[[ -d "$conf_d" ]] || conf_d=/etc/my.cnf.d
[[ -d "$conf_d" ]] || die "MariaDB 설정 디렉토리를 찾을 수 없습니다 (/etc/mysql/mariadb.conf.d)"
dropin="$conf_d/99-cims-tb.cnf"

# 배포판 conffile(50-server.cnf)은 건드리지 않는다. 뒤 번호 drop-in 이 이긴다.
new_conf="$(cat <<EOF
# CIMS TB — deployment/tb/steps/10-db-setup.sh 가 생성. 배포판 conffile 은 원본 유지.
[mysqld]
bind-address = $TB_DB_BIND_ADDRESS
port         = $TB_DB_PORT
EOF
)"
if [[ -f "$dropin" ]] && [[ "$(cat "$dropin")" == "$new_conf" ]]; then
    ok "이미 설정됨 ($dropin)"
else
    printf '%s\n' "$new_conf" > "$dropin"
    chmod 644 "$dropin"
    tb_run 10-mariadb-restart systemctl restart mariadb \
        || die_hint "설정 반영 후 MariaDB 재기동 실패" "확인: journalctl -u mariadb -n 50" \
                    "설정 되돌리기: rm $dropin && systemctl restart mariadb"
    tb_db_active || die_hint "재기동 후 MariaDB 가 active 가 아닙니다 — 넣은 설정이 원인일 수 있습니다" \
        "확인: journalctl -u mariadb -n 50" \
        "설정 되돌리기: rm $dropin && systemctl restart mariadb"
    ok "설정 반영 + 재기동 ($dropin)"
fi

# ── ③ 임시 TCP 관리 계정 ──────────────────────────────────────
info "[3/4] 임시 TCP 관리 계정 — ${TB_DB_ADMIN_USER}@${TB_DB_ADMIN_GRANT_HOST}"

_sql_root() {
    # root@localhost = unix_socket 인증 → sudo 로 소켓 접속.
    mariadb --protocol=socket -u root -e "$1" 2>&1
}
if ! _sql_root "SELECT 1" >/dev/null; then
    die_hint "root 소켓 접속 실패 — 이미 초기화된 DB 인 것 같습니다" \
        "root@localhost 에 비밀번호가 걸려 있으면 수동으로 관리 계정을 만들고" \
        "TB_DB_ADMIN_USER / TB_DB_ADMIN_PASS 를 tb-site.conf 에 넣은 뒤 15 단계로 넘어가세요."
fi

admin_pass="$(openssl rand -base64 24 | tr -d '/+=' | cut -c1-20)"
# 비밀번호는 argv 에 싣지 않는다 (/proc/<pid>/cmdline 노출) — stdin 으로만 넘긴다.
if ! mariadb --protocol=socket -u root <<SQL 2>"$TB_LOG_DIR/10-admin.err"
CREATE USER IF NOT EXISTS '$TB_DB_ADMIN_USER'@'$TB_DB_ADMIN_GRANT_HOST' IDENTIFIED BY '$admin_pass';
ALTER USER '$TB_DB_ADMIN_USER'@'$TB_DB_ADMIN_GRANT_HOST' IDENTIFIED BY '$admin_pass';
GRANT ALL PRIVILEGES ON *.* TO '$TB_DB_ADMIN_USER'@'$TB_DB_ADMIN_GRANT_HOST' WITH GRANT OPTION;
FLUSH PRIVILEGES;
SQL
then
    err "관리 계정 발급 실패"; sed -n '1,10p' "$TB_LOG_DIR/10-admin.err" >&2; exit 1
fi
rm -f "$TB_LOG_DIR/10-admin.err"

# 다음 단계(15)가 읽는다. 임시 자격이므로 site.conf 가 아니라 state/ 에 둔다.
state="$TB_STATE_DIR/db-admin.env"
umask 077
cat > "$state" <<EOF
# 15-db-schema.sh 가 읽고, 스키마 적용이 끝나면 이 계정을 회수한다.
TB_DB_ADMIN_USER=$TB_DB_ADMIN_USER
TB_DB_ADMIN_PASS=$admin_pass
TB_DB_ADMIN_GRANT_HOST=$TB_DB_ADMIN_GRANT_HOST
TB_DB_ADMIN_TEMPORARY=1
EOF
chmod 600 "$state"
ok "발급 완료 — 자격은 $state (0600)"

# ── ④ TCP 접속 확인 ───────────────────────────────────────────
info "[4/4] TCP 접속 확인 — 127.0.0.1:$TB_DB_PORT"
if MYSQL_PWD="$admin_pass" mariadb -h 127.0.0.1 -P "$TB_DB_PORT" \
        -u "$TB_DB_ADMIN_USER" -e "SELECT VERSION()" >/dev/null 2>&1; then
    ok "TCP 접속 정상"
else
    warn "127.0.0.1 로는 붙지 못했습니다 — 허용 host 가 '$TB_DB_ADMIN_GRANT_HOST' 라서 정상일 수 있습니다"
    warn "스키마를 적용할 장비에서 15 단계를 실행하세요"
fi

header "[10] 완료"
cat <<EOF
  DB 접속 정보 (다른 장비의 모듈이 쓸 값):
    host = $(hostname -I 2>/dev/null | awk '{print $1}')  (bind=$TB_DB_BIND_ADDRESS)
    port = $TB_DB_PORT

  다음: 15 단계(스키마) — 이 장비에서 하려면
    sudo ./tb-install.sh --role db-schema
EOF
