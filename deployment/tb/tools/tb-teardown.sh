#!/usr/bin/env bash
# =============================================================
# tb-teardown.sh — 이 서버를 맨바닥으로 되돌린다 (TB 재설치 준비)
#
# tb-install.sh 의 역방향. 05 단계가 "MariaDB 이미 설치됨"으로 건너뛰지 않도록
# DB 서버까지 걷어내는 것이 기본이다 — 즉 05·10 단계를 실측할 수 있는 상태로 만든다.
#
# 하는 일
#   ① CIMS base 철거      — <prefix>/uninstall-base.sh 위임 (agent·모듈·store·sudoers)
#   ② 잔존 프로세스 정리   — C++ 모듈(csp/cmp/cmdp) 은 ① 이 못 잡는다 (아래 주석)
#   ③ 계정 잔재 정리       — linger · user systemd unit · drop-in
#   ④ 서비스 로그 비우기   — NAS 공유 스토리지 (디렉토리·ACL 은 보존)
#   ⑤ MariaDB 제거         — 서버·클라이언트 purge + datadir 삭제
#                            + **AppArmor 프로파일 언로드** (없으면 재설치가 깨진다)
#
# 남기는 것 (의도)
#   · libmariadb3 / libmariadb-dev  이 서버는 빌드 장비다. 지우면 csp CMake configure 가
#                                   중단된다 (CLAUDE.md 전제조건). 반입본에도 -dev 는 없다.
#   · mariadb-common / mysql-common  libmariadb3 의 의존이라 같이 지울 수 없다. conffile 은
#                                   원본 그대로이고 05 단계가 apt 로 만족된 것으로 본다.
#   · mysql 시스템 계정              sysusers 산물. 재설치가 그대로 재사용한다.
#   · IP 별칭(.47~.50) · /mnt/cims 마운트 · cims-svc 그룹 — 호스트 설정이라 재설치가 쓴다.
#   · 서비스 계정 cims               --purge-account 로만 지운다 (NAS ACL·그룹이 여기 걸려 있다)
#
# 멱등 — 이미 없는 것은 조용히 건너뛴다. 그래서 set -e 를 쓰지 않는다.
# =============================================================
set -uo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

PREFIX="${TB_INSTALL_PREFIX:-/opt/cims-agent}"
SVC_USER="${TB_SVC_USER:-cims}"
NAS_LOG_DIR="${TB_NAS_LOG_DIR:-/mnt/cims/service_log}"
DB_DATADIR_CANDIDATES=(/var/lib/mariadb /var/lib/mysql)
AA_PROFILE="${TB_AA_PROFILE:-/etc/apparmor.d/mariadbd}"

YES=0; DRY=0; KEEP_DB=0; KEEP_NAS=0; PURGE_ACCOUNT=0

usage() {
    cat <<EOF
${BOLD}CIMS TB 철거${NC} — 이 서버를 맨바닥으로 되돌린다

  사용법: sudo ./tools/tb-teardown.sh [옵션]

  옵션
    -y, --yes          확인 없이 진행
        --dry-run      무엇을 지울지만 보여준다 (아무것도 안 지운다)
        --keep-db      MariaDB 서버를 남긴다 (cims DB 만 drop → 05 단계는 건너뛰어짐)
        --keep-nas     NAS 서비스 로그($NAS_LOG_DIR)를 남긴다
        --purge-account  서비스 계정 '$SVC_USER' 도 삭제한다 (userdel -r)
    -h, --help

  기본값은 '맨바닥' — MariaDB 서버까지 지워 05·10 단계를 실측할 수 있게 한다.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -y|--yes)        YES=1; shift ;;
        --dry-run)       DRY=1; shift ;;
        --keep-db)       KEEP_DB=1; shift ;;
        --keep-nas)      KEEP_NAS=1; shift ;;
        --purge-account) PURGE_ACCOUNT=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *)               err "알 수 없는 인자: $1"; usage; exit 1 ;;
    esac
done

[[ $DRY -eq 1 ]] || tb_require_root

# dry-run 은 실행 명령을 찍고 넘긴다. 실 실행 경로와 목록이 갈라지지 않게 한 군데로 모은다.
run() {
    if [[ $DRY -eq 1 ]]; then echo "    would: $*"; return 0; fi
    "$@"
}

# ── 무엇이 사라지는지 먼저 보여준다 ───────────────────────────
header "=== 철거 대상 ==="
echo "  설치 루트      $PREFIX $( [[ -d $PREFIX ]] && echo "($(du -sh "$PREFIX" 2>/dev/null | cut -f1))" || echo '(없음)')"
echo "  관리 store     $PREFIX/modules/oam/runtime — 배포 overlay·컬렉션·인증서·토큰 전부"
echo "  실행 중 모듈   $(pgrep -f 'bin/csp |bin/cmp |bin/cmdp |oam_app\.py|oam_svc_app\.py|csc_app\.py|cims_agent\.py' 2>/dev/null | wc -l) 개"
if [[ $KEEP_NAS -eq 1 ]]; then
    echo "  NAS 서비스로그 유지 ($NAS_LOG_DIR)"
else
    echo "  NAS 서비스로그 $NAS_LOG_DIR 내용 비움 ($(du -sh "$NAS_LOG_DIR" 2>/dev/null | cut -f1))"
fi
if [[ $KEEP_DB -eq 1 ]]; then
    echo "  DB             cims 스키마만 drop (MariaDB 서버 유지)"
else
    echo "  DB             MariaDB 서버·클라이언트 purge + datadir 삭제"
fi
echo "  계정 $SVC_USER      $( [[ $PURGE_ACCOUNT -eq 1 ]] && echo '삭제 (userdel -r)' || echo '유지 (linger·unit·drop-in 만 정리)')"
echo
echo "  ${YELLOW}백업하지 않는다${NC} — 가입자·구독은 TB 18 단계가 data/*.csv 로, 배포 설정은"
echo "  45 단계가 다시 심는다. 잃는 것은 서비스 로그의 통계 실데이터다."

if [[ $DRY -eq 0 && $YES -ne 1 ]]; then
    echo
    read -r -p "  계속할까요? [y/N] " _a
    [[ "$_a" == y* || "$_a" == Y* ]] || die "중단"
fi

# ── ① CIMS base 철거 ─────────────────────────────────────────
header "=== [1/5] CIMS base 철거 ==="
if [[ -f "$PREFIX/uninstall-base.sh" ]]; then
    # env -u SUDO_USER 필수. uninstall.sh 는 서비스 계정을 SUDO_USER → 설치 디렉토리 소유자
    # 순으로 정한다. SUDO_USER 를 물려주면 호출자(user01) 의 systemd unit 을 지우고 정작
    # cims 의 agent unit 을 살려 둔다 (initial_install.md §7).
    info "uninstall-base.sh 위임 (env -u SUDO_USER)"
    run env -u SUDO_USER bash "$PREFIX/uninstall-base.sh" --yes || warn "uninstall-base.sh 실패 — 아래 단계가 잔여를 정리한다"
elif [[ -f "$PREFIX/uninstall.sh" ]]; then
    info "uninstall.sh 위임 (base 스크립트 없음 — 콘솔 배포로만 세운 노드)"
    run env -u SUDO_USER bash "$PREFIX/uninstall.sh" --yes || warn "uninstall.sh 실패"
else
    warn "$PREFIX 에 uninstall 스크립트가 없다 — 파일 삭제로만 처리한다"
fi

# ── ② 잔존 프로세스 ──────────────────────────────────────────
header "=== [2/5] 잔존 프로세스 ==="
# C++ 모듈은 ① 이 못 잡는다. uninstall.sh 의 탐색이 `pgrep -af <install_path>` 인데
# cims-svc 가 cwd 를 모듈 디렉토리로 잡고 상대 경로로 exec 하므로 cmdline 에 절대경로가
# 없다 (`bin/csp config/csp.json -n`). csp 는 SIGTERM 을 삼키는 경우가 있어 KILL 까지 간다.
PATTERNS='bin/csp |bin/cmp |bin/cmdp |bin/cspsim |oam_app\.py|oam_svc_app\.py|csc_app\.py|cims_agent\.py'
for sig in TERM KILL; do
    pids=$(pgrep -f "$PATTERNS" 2>/dev/null | tr '\n' ' ')
    [[ -z "${pids// /}" ]] && { ok "잔존 프로세스 없음"; break; }
    for p in $pids; do
        [[ "$p" == "$$" ]] && continue
        info "kill -$sig $p : $(ps -p "$p" -o args= 2>/dev/null | head -c 70)"
        run kill "-$sig" "$p" 2>/dev/null
    done
    [[ $DRY -eq 1 ]] && break
    [[ "$sig" == TERM ]] && sleep 3
done

header "=== [2b/5] 설치 루트 삭제 ==="
if [[ -d "$PREFIX" ]]; then
    info "rm -rf $PREFIX"
    run rm -rf "$PREFIX"
else
    ok "$PREFIX 없음"
fi

# ── ③ 계정 잔재 ──────────────────────────────────────────────
header "=== [3/5] 계정 잔재 ($SVC_USER) ==="
if id "$SVC_USER" >/dev/null 2>&1; then
    SVC_HOME="$(getent passwd "$SVC_USER" | cut -d: -f6)"
    SVC_UID="$(id -u "$SVC_USER")"

    UNIT_DIR="$SVC_HOME/.config/systemd/user"
    if [[ -d "$UNIT_DIR" ]]; then
        for f in "$UNIT_DIR"/cims-agent.service "$UNIT_DIR"/cims-agent-*.service \
                 "$UNIT_DIR"/cims-agent.service.d "$UNIT_DIR"/default.target.wants/cims-agent*.service; do
            [[ -e "$f" ]] || continue
            info "제거 $f"
            run rm -rf "$f"
        done
        run runuser -u "$SVC_USER" -- env XDG_RUNTIME_DIR="/run/user/$SVC_UID" \
            systemctl --user daemon-reload 2>/dev/null
    else
        ok "user systemd unit 없음"
    fi

    if loginctl show-user "$SVC_USER" 2>/dev/null | grep -q '^Linger=yes'; then
        info "linger 해제 ($SVC_USER)"
        run loginctl disable-linger "$SVC_USER"
    else
        ok "linger 꺼짐"
    fi
    run rm -f "/var/lib/systemd/linger/$SVC_USER"

    if [[ $PURGE_ACCOUNT -eq 1 ]]; then
        warn "계정 삭제 — NAS ACL·cims-svc 그룹 구성이 이 계정을 참조하면 재설치 후 다시 잡아야 한다"
        info "userdel -r $SVC_USER"
        run userdel -r "$SVC_USER" 2>/dev/null || warn "userdel 실패 (로그인 세션이 남아 있을 수 있다)"
    fi
else
    ok "계정 $SVC_USER 없음"
fi

if [[ -f /etc/sudoers.d/cims-priv ]]; then
    info "제거 /etc/sudoers.d/cims-priv"
    run rm -f /etc/sudoers.d/cims-priv
else
    ok "/etc/sudoers.d/cims-priv 없음"
fi

# ── ④ NAS 서비스 로그 ────────────────────────────────────────
header "=== [4/5] NAS 서비스 로그 ==="
if [[ $KEEP_NAS -eq 1 ]]; then
    ok "유지 (--keep-nas)"
elif [[ -d "$NAS_LOG_DIR" ]]; then
    # 디렉토리 자체는 남긴다 — setgid·ACL(cims-svc 쓰기)이 여기 걸려 있고, 재설치가 그
    # 권한 구성을 다시 만들지 않는다. 안의 내용만 비운다.
    info "내용 비움 $NAS_LOG_DIR/* ($(du -sh "$NAS_LOG_DIR" 2>/dev/null | cut -f1))"
    if [[ $DRY -eq 1 ]]; then
        echo "    would: rm -rf $NAS_LOG_DIR/{*,.[!.]*}"
    else
        find "$NAS_LOG_DIR" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null
        ok "비움 (디렉토리·권한 유지: $(stat -c '%U:%G %a' "$NAS_LOG_DIR"))"
    fi
else
    ok "$NAS_LOG_DIR 없음"
fi

# ── ⑤ MariaDB ────────────────────────────────────────────────
header "=== [5/5] MariaDB ==="
if [[ $KEEP_DB -eq 1 ]]; then
    # 서버를 남기는 경로 — root 비번이 없어도 되도록 앱 계정으로 스키마만 떨어뜨린다.
    info "cims 스키마만 drop (MariaDB 서버 유지)"
    if [[ $DRY -eq 1 ]]; then
        echo "    would: mariadb -h127.0.0.1 -u<app> -p<app> -e 'DROP DATABASE cims'"
    else
        read -r -p "  DB 계정 (기본 cims): " _du; _du="${_du:-cims}"
        read -r -s -p "  DB 비밀번호: " _dp; echo
        mariadb -h127.0.0.1 -u"$_du" -p"$_dp" -e "DROP DATABASE IF EXISTS cims;" \
            && ok "cims 스키마 drop" || warn "drop 실패 — 계정/비밀번호 확인"
    fi
    warn "05 단계는 'MariaDB 이미 설치됨' 으로 건너뛰어진다 (실측 안 됨)"
else
    run systemctl stop mariadb 2>/dev/null

    # AppArmor 프로파일을 커널에서 걷어낸다 — **이걸 빼면 다음 설치가 반드시 깨진다.**
    # purge 는 /etc/apparmor.d/mariadbd 파일만 지우고 커널에 로드된 프로파일은 남긴다.
    # 그 상태로 재설치하면 패키지 postinst 의 mariadb-install-db 가 mariadbd 를 root 로
    # 띄워 mysql 로 내려가려 할 때 setgid·dac_override 가 거부돼 시스템 테이블 생성이
    # 실패한다. postinst 는 그 호출을 set +e 로 감싸므로 **설치가 성공으로 끝나고**
    # datadir 에 InnoDB 파일만 남는다 — 맨바닥 장비에서는 그 시점에 프로파일이 없어
    # 통과하므로, 이 문제는 철거→재설치 경로에서만 난다.
    if [[ -f "$AA_PROFILE" ]]; then
        info "AppArmor 프로파일 언로드 ($AA_PROFILE)"
        run apparmor_parser -R "$AA_PROFILE" 2>/dev/null \
            || warn "언로드 실패 — 재설치 전에 수동 확인: sudo aa-status | grep maria"
    else
        ok "AppArmor mariadbd 프로파일 파일 없음"
    fi
    # 서버·클라이언트 실행부만 끊는다. libmariadb3/-dev 는 빌드용으로 남긴다 (머리말 참조).
    # 05 단계가 --no-install-recommends 로 mariadb-server·mariadb-client 만 요청하므로
    # plugin-provider·galera 는 반입 저장소에 없어도 되고, 여기서 지워도 재설치가 된다.
    purge=()
    for p in mariadb-server mariadb-server-core mariadb-client mariadb-client-core \
             mariadb-plugin-provider-bzip2 mariadb-plugin-provider-lz4 \
             mariadb-plugin-provider-lzma mariadb-plugin-provider-lzo \
             mariadb-plugin-provider-snappy galera-4; do
        dpkg -s "$p" >/dev/null 2>&1 && purge+=("$p")
    done
    if [[ ${#purge[@]} -gt 0 ]]; then
        info "purge: ${purge[*]}"
        if [[ $DRY -eq 1 ]]; then
            echo "    would: apt-get purge -y ${purge[*]}"
        else
            DEBIAN_FRONTEND=noninteractive tb_run "teardown-apt-purge" \
                apt-get purge -y "${purge[@]}" || warn "purge 실패 — 로그 확인"
        fi
    else
        ok "MariaDB 서버 패키지 없음"
    fi
    # purge 는 datadir 를 남긴다 (Ubuntu 26.04 기본 datadir = /var/lib/mariadb).
    for d in "${DB_DATADIR_CANDIDATES[@]}" /var/log/mysql /run/mysqld; do
        [[ -e "$d" ]] || continue
        info "삭제 $d"
        run rm -rf "$d"
    done
    # TB 10 단계가 만드는 drop-in — 남기면 새 설치가 옛 값을 물고 뜬다.
    [[ -e /etc/mysql/mariadb.conf.d/99-cims-tb.cnf ]] && {
        info "삭제 /etc/mysql/mariadb.conf.d/99-cims-tb.cnf"
        run rm -f /etc/mysql/mariadb.conf.d/99-cims-tb.cnf
    }
fi

# ── 확인 ─────────────────────────────────────────────────────
[[ $DRY -eq 1 ]] && { header "=== dry-run 종료 — 아무것도 지우지 않았다 ==="; exit 0; }

header "=== 확인 ==="
chk() { # <설명> <참이어야 하는 조건 명령...>
    local d="$1"; shift
    if "$@" >/dev/null 2>&1; then ok "$d"; else err "$d — 남아 있다"; fi
}
chk_not() { # <설명> <거짓이어야 하는 조건 명령...>
    local d="$1"; shift
    if "$@" >/dev/null 2>&1; then err "$d — 남아 있다"; else ok "$d"; fi
}
chk "설치 루트 없음 ($PREFIX)"                 test ! -e "$PREFIX"
_left=$(pgrep -f "$PATTERNS" 2>/dev/null | wc -l)
chk "CIMS 프로세스 없음 (잔존 $_left)"         test "$_left" -eq 0
chk "sudoers 없음"                             test ! -e /etc/sudoers.d/cims-priv
chk "linger 없음"                              test ! -e "/var/lib/systemd/linger/$SVC_USER"
if [[ $KEEP_DB -eq 0 ]]; then
    chk_not "mariadbd 없음 (05 단계가 실제로 설치한다)" command -v mariadbd
    chk_not "mariadb 클라이언트 없음"                   command -v mariadb
    chk "datadir 없음"                              test ! -e /var/lib/mariadb
    chk_not "AppArmor mariadbd 프로파일 미로드"     grep -q '^mariadbd ' /sys/kernel/security/apparmor/profiles
fi
chk "libmariadb-dev 유지 (빌드 전제조건)"      dpkg -s libmariadb-dev
echo
info "포트 점검"
ss -lntup 2>/dev/null | awk 'NR==1 || /:(4419|4421|4430|5060|25061|5061|9000|9001|3306)\y/' || true

header "=== 철거 완료 — 재설치 준비됨 ==="
cat <<EOF
  반입본을 풀어서 설치하세요 (레포 워크트리 사본이 아니라 ${BOLD}tar${NC} 를 쓰는 게 맞습니다 —
  tar 안의 db-bootstrap 이 ghost-escape 패치가 들어간 판본입니다):

      mkdir -p ~/tb && tar xzf <반입본>/cims-tb-20260907.tar.gz -C ~/tb
      cd ~/tb/tb && sudo ./tb-install.sh --role all-in-one
EOF
