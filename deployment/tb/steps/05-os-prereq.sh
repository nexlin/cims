#!/usr/bin/env bash
# =============================================================
# 05-os-prereq.sh — OS 전제조건 확인 + 반입 .deb 설치 (폐쇄망)
#
# 하는 일
#   ① OS/아키텍처가 기준(Ubuntu 26.04 / x86_64)인지 확인 — 아니면 이유를 말하고 중단
#   ② 필수 명령 존재 확인 (python3 / tar / openssl, curl·ssh 는 경고)
#   ③ 반입한 .deb 설치 — 기본은 MariaDB 서버 (offline/debs/mariadb/)
#
# 왜 이 단계가 있나: 폐쇄망엔 apt repo 가 없다. 프로젝트는 이미 같은 문제를
# .deb 동봉 + 오프라인 설치로 풀고 있다 (csp/vendor, agent/vendor) — 그 관례를 따른다.
#
# 설치 방식은 **반입 디렉토리를 로컬 apt 저장소로 붙이는** 쪽이다. `dpkg -i *.deb` 는
# 대체 제공자(예: opensysusers ↔ systemd 의 sysusers)가 함께 들어오면 충돌로 실패하고
# 설치 순서도 스스로 풀지 못한다. apt 에 "mariadb-server 를 깔아라" 라고 시키면
# 이미 있는 것은 건너뛰고, 필요한 것만, 맞는 순서로 깐다. Packages 색인이 없으면
# dpkg 직접 설치로 폴백한다.
#
# 네트워크는 쓰지 않는다 — Dir::Etc::sourceparts=/dev/null 로 시스템 저장소를 배제한다.
#
# 멱등: 이미 설치돼 있으면 apt 가 "already the newest version" 으로 끝낸다.
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

DEB_SET="${1:-mariadb}"     # offline/debs/<set>/ 의 이름

header "=== [05] OS 전제조건 ==="

tb_require_arch
tb_require_os
tb_require_python
tb_require_cmds tar openssl

# curl·ssh 는 이후 단계(OAM 확인·자동 배포)에서 쓴다. 미리 알려주면 현장에서 두 번
# 왕복하지 않는다.
for c in curl ssh; do
    command -v "$c" >/dev/null || warn "$c 없음 — 이후 단계에서 필요합니다 (반입 목록에 추가)"
done

# ── 이미 설치돼 있으면 끝 ─────────────────────────────────────
# 명령 존재만 보고 넘기면 안 된다 — **패키지는 설치됐고 시스템 DB 만 없는 반쪽 상태**가
# 실제로 생긴다(철거 후 재설치에서 AppArmor 가 mariadb-install-db 를 막을 때). 그때
# 여기서 통과시키면 10 단계가 뜨지 않는 DB 를 붙잡고 헤맨다.
if [[ "$DEB_SET" == "mariadb" ]] && command -v mariadbd >/dev/null && command -v mariadb >/dev/null; then
    if ! tb_db_initialized; then
        err "MariaDB 는 설치돼 있으나 초기화되지 않았습니다"
        tb_db_init_hint
        die "위 복구를 먼저 하세요 (또는 tools/tb-teardown.sh 로 걷어내고 다시 설치)"
    fi
    ok "MariaDB 서버·클라이언트 이미 설치됨 ($(mariadbd --version | sed 's/.*Ver \([^ ]*\).*/\1/'))"
    ok "시스템 DB 확인 — datadir: $(tb_db_datadir)"
    exit 0
fi

deb_dir="$TB_OFFLINE_DIR/debs/$DEB_SET"
[[ -d "$deb_dir" ]] || die_hint "반입 .deb 디렉토리 없음: $deb_dir" \
    "빌드 장비(개발 서버)에서 아래로 수집해 USB 로 옮기세요:" \
    "  deployment/tb/tools/tb-fetch-debs.sh $DEB_SET" \
    "그러면 deployment/tb/offline/debs/$DEB_SET/ 에 담깁니다."

shopt -s nullglob
debs=("$deb_dir"/*.deb)
shopt -u nullglob
[[ ${#debs[@]} -gt 0 ]] || die "「$deb_dir」 에 .deb 가 없습니다"

# 설치를 요청할 패키지 이름. 수집 단계가 남긴 목록을 쓰고, 없으면 set 이름으로 유도한다.
targets=()
if [[ -f "$deb_dir/tb-targets.txt" ]]; then
    while read -r t; do [[ -n "$t" ]] && targets+=("$t"); done < "$deb_dir/tb-targets.txt"
fi
if [[ ${#targets[@]} -eq 0 ]]; then
    case "$DEB_SET" in
        mariadb) targets=(mariadb-server mariadb-client) ;;
        *)       targets=() ;;
    esac
fi

tb_require_root
tb_init_dirs

# ── dpkg 가 깨끗한지 먼저 ─────────────────────────────────────
# dpkg 가 반쯤 멈춘 상태(--configure 미완, "in a mess" 패키지)에서는 apt 가 **무엇을 해도
# 거부**한다. 그런데 그 사실이 여기서 드러나지 않고 한참 뒤 의존 오류 목록으로만 보여서
# "반입 deb 가 부족한가" 로 오해하게 된다(2026-09-10 실측: libwrap0 가 mess 상태였는데
# `mariadb-server Depends libwrap0` 연쇄로만 나타났다). 먼저 짚고 멈춘다.
_audit="$(dpkg --audit 2>/dev/null || true)"
if [[ -n "$_audit" ]]; then
    err "dpkg 가 깨끗하지 않습니다 — apt 는 이 상태에서 아무것도 설치하지 못합니다"
    echo "$_audit" | sed 's/^/        /' >&2
    die_hint "먼저 dpkg 상태를 정리하세요" \
        "  sudo dpkg --configure -a" \
        "그래도 'in a mess' 가 남으면 그 패키지를 다시 설치해야 합니다:" \
        "  sudo apt-get install --reinstall <패키지>            (인터넷 되는 장비)" \
        "  폐쇄망이면 반입한 .deb 로: sudo dpkg -i offline/debs/*/<패키지>_*.deb" \
        "정리 뒤 이어서: sudo ./tb-install.sh --role $ROLE --from 05"
fi
ok "dpkg 상태 깨끗함"

# ── ③ 설치 ────────────────────────────────────────────────────
installed=0
# 로컬 저장소는 **debs/* 전체**를 붙인다 — 설치 대상은 이 set 이지만, apt 는 시스템 전체의
# 의존 상태를 먼저 검사하므로 **관계없는 패키지의 의존이 비어 있으면 이 설치까지 거부**한다.
# (실측: linux-tools 의 libnl-3-200 이 없어 MariaDB 설치가 `Unmet dependencies` 로 막혔다.)
# 그 구멍을 메울 deb 를 `debs/apt-repair/` 같은 다른 set 으로 반입해도 apt 가 함께 보게 한다.
# 색인(Packages)이 있는 디렉토리만 — 없는 것을 넣으면 apt update 가 그 줄에서 실패한다.
repo_dirs=()
for _d in "$TB_OFFLINE_DIR"/debs/*/; do
    [[ -f "${_d}Packages" || -f "${_d}Packages.gz" ]] && repo_dirs+=("${_d%/}")
done

if [[ ${#repo_dirs[@]} -gt 0 ]] && [[ ${#targets[@]} -gt 0 ]] \
   && command -v apt-get >/dev/null; then
    info "[1/2] 오프라인 apt 저장소로 설치 — ${targets[*]} (${#debs[@]}개 .deb, 저장소 ${#repo_dirs[@]}곳)"

    # apt 의 file:// 취득은 **_apt 사용자로 권한을 낮춰** 수행된다. 키트를 홈 아래에 풀면
    # (Ubuntu 기본 홈은 0750) _apt 가 디렉토리를 통과하지 못해
    #   Could not open file .../Packages - open (13: Permission denied)
    # 로 색인을 못 읽고, apt 는 쓸 수 있는 소스가 없는 상태로 의존을 풀지 못한다(실측
    # 2026-09-10, /home/<계정> 0750). 키트를 **어디에 풀었는지에 의존하지 않도록** 색인과
    # .deb 를 _apt 가 읽을 수 있는 자리로 옮겨 붙인다. 홈 권한을 바꾸지 않는 쪽을 택했다 —
    # 설치 스크립트가 사용자 홈의 접근 권한을 넓히는 것은 부수효과가 너무 크다.
    stage_root="/var/tmp/tb-offline-debs.$$"
    install -d -m 755 "$stage_root"
    staged=()
    for _d in "${repo_dirs[@]}"; do
        _s="$stage_root/$(basename "$_d")"
        install -d -m 755 "$_s"
        cp -f "$_d"/*.deb "$_s"/ 2>/dev/null || true
        cp -f "$_d"/Packages "$_d"/Packages.gz "$_s"/ 2>/dev/null || true
        chmod 644 "$_s"/* 2>/dev/null || true
        staged+=("$_s")
    done

    listf="$TB_STATE_DIR/offline-$DEB_SET.list"
    : > "$listf"
    for _d in "${staged[@]}"; do
        printf 'deb [trusted=yes] file://%s ./\n' "$_d" >> "$listf"
    done
    # sourceparts=/dev/null → 시스템 저장소(네트워크) 배제. List-Cleanup=0 → 기존 색인 보존.
    apt_opts=(-o "Dir::Etc::sourcelist=$listf"
              -o Dir::Etc::sourceparts=/dev/null
              -o APT::Get::List-Cleanup=0
              -o Acquire::Languages=none)
    if tb_run "05-apt-$DEB_SET" apt-get "${apt_opts[@]}" update \
       && DEBIAN_FRONTEND=noninteractive tb_run "05-apt-$DEB_SET" \
            apt-get "${apt_opts[@]}" install -y --no-install-recommends "${targets[@]}"
    then
        installed=1
        ok "apt 설치 완료"
    else
        warn "apt 경로 실패 — dpkg 직접 설치로 재시도합니다 (로그: $TB_LOG_DIR/05-apt-$DEB_SET.log)"
    fi
    rm -f "$listf"
    rm -rf "$stage_root"
fi

if [[ $installed -eq 0 ]]; then
    # dpkg 직접 설치는 **기본으로 하지 않는다.**
    #
    # dpkg 는 의존을 해소하지 못하고 순서대로 풀다 중간에서 멈춘다. 그러면 시스템이
    # **반쯤 설치된 상태**로 남는다 — 실측(2026-09-10, media02): perl 계열이 버전이 섞여
    # (libperl5.40 0.3 / perl-modules 0.1 / perl 없음) `dpkg was interrupted` 로 잠기고,
    # 이후 apt 조작이 전부 거부됐다. **깔끔한 실패를 망가진 시스템으로 바꾸는 것**이라
    # 이득보다 손해가 크다. MariaDB 의존 폐쇄집합에는 opensysusers 처럼 systemd 와 충돌하는
    # 것도 들어와서 성공 확률 자체가 낮다.
    #
    # 정공법은 apt 가 왜 실패했는지 고치는 것이다 — 부족한 .deb 를 반입에 추가하거나,
    # 대상 장비의 깨진 의존을 `debs/apt-repair/` 로 메운다.
    if [[ "${TB_ALLOW_DPKG_FALLBACK:-0}" != "1" ]]; then
        die_hint "apt 경로가 실패했습니다 — dpkg 직접 설치는 기본으로 시도하지 않습니다" \
            "로그에서 **맨 앞의 Err: 줄**과 unmet dependencies 를 함께 보세요:" \
            "  $TB_LOG_DIR/05-apt-$DEB_SET.log" \
            "  · 'not installable'          = 어디에도 없다 → 반입에 추가해야 한다" \
            "  · 'not going to be installed' = 있는데 다른 원인의 연쇄다" \
            "부족한 패키지는 빌드 장비에서 추가 수집:" \
            "  deployment/tb/tools/tb-fetch-debs.sh <패키지명>" \
            "대상 장비의 의존이 이미 깨져 있으면 apt-repair 세트를 반입하세요:" \
            "  deployment/tb/tools/tb-fetch-debs.sh apt-repair" \
            "그래도 dpkg 로 밀어붙이려면(반쯤 설치될 수 있습니다):" \
            "  sudo TB_ALLOW_DPKG_FALLBACK=1 ./tb-install.sh --role db --from 05"
    fi
    warn "TB_ALLOW_DPKG_FALLBACK=1 — dpkg 직접 설치를 시도합니다 (반쯤 설치될 수 있습니다)"
    info "[1/2] dpkg 직접 설치 — ${#debs[@]}개 ($DEB_SET)"
    # 한 번에 넘겨 서로의 의존을 dpkg 가 해소하게 한다. 개별 설치는 순서 문제로 실패한다.
    if ! DEBIAN_FRONTEND=noninteractive tb_run "05-dpkg-$DEB_SET" \
            dpkg -i --force-confnew "${debs[@]}"; then
        die_hint "설치 실패 — 의존 부족 또는 대체 제공자 충돌" \
            "로그의 'dependency problems' / 'conflicting' 줄을 보고 판단하세요:" \
            "  $TB_LOG_DIR/05-dpkg-$DEB_SET.log" \
            "반쯤 설치됐을 수 있습니다 — 먼저 이것부터:" \
            "  sudo dpkg --configure -a" \
            "부족한 패키지는 빌드 장비에서 추가 수집:" \
            "  deployment/tb/tools/tb-fetch-debs.sh <패키지명>"
    fi
fi

# ── 설치 확인 ─────────────────────────────────────────────────
info "[2/2] 설치 확인"
case "$DEB_SET" in
    mariadb)
        command -v mariadbd >/dev/null || die "mariadbd 가 없습니다 — 설치가 완료되지 않았습니다"
        command -v mariadb  >/dev/null || die "mariadb 클라이언트가 없습니다 (10 단계가 이 명령을 씁니다)"
        ok "MariaDB: $(mariadbd --version | sed 's/.*Ver \([^ ]*\).*/\1/')"
        # apt 는 postinst 가 조용히 실패해도 성공으로 끝난다 — 산출물을 직접 본다.
        if ! tb_db_initialized; then
            err "설치는 됐으나 시스템 DB 가 만들어지지 않았습니다"
            tb_db_init_hint
            die "이 상태로는 10 단계가 DB 를 띄울 수 없습니다"
        fi
        ok "시스템 DB 확인 — datadir: $(tb_db_datadir)"
        ;;
    *)  ok "$DEB_SET 설치 완료" ;;
esac

ok "[05] 완료"
