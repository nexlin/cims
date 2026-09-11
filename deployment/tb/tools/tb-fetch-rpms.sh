#!/usr/bin/env bash
# =============================================================
# tb-fetch-rpms.sh — 폐쇄망 반입용 .rpm 수집 (rhel 계열 장비에서 실행)
#
#   ./tb-fetch-rpms.sh mariadb          MariaDB 서버·클라이언트 + 커넥터 + 의존
#   ./tb-fetch-rpms.sh python           OAM·CSC 용 CPython 3.14
#   ./tb-fetch-rpms.sh <패키지명>...     임의 패키지 (산출은 rpms/custom/)
#
# 산출: deployment/tb/offline/rpms/<set>/*.rpm  → 이 디렉토리를 USB 로 옮긴다.
#
# **수집 장비는 대상 장비와 같은 배포판·같은 메이저 버전이어야 한다.** rpm 은 배포판
# 빌드에 맞춰져 있어서, Rocky 10 용을 RHEL 9 에 깔 수 없다. deb 쪽(tb-fetch-debs.sh)은
# 개발 서버가 곧 기준 배포판이라 이 문제가 없었지만, rhel 계열은 빌드 장비가 다른 배포판
# 이므로 **대상 장비 또는 같은 판의 장비에서 수집**한다.
#
# 의존 고르기: dnf 가 스스로 푼다(`--resolve --alldeps`). deb 쪽처럼 우선순위로 걸러내지
# 않는 이유는, dnf 가 대상 디렉토리를 통째로 받아 설치할 때(`dnf install ./*.rpm`) 이미
# 설치된 것은 건너뛰기 때문이다 — 넉넉히 담는 쪽이 안전하다.
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

SET="${1:-}"; shift || true
rest=("$@")

case "$SET" in
    # csp(C++)가 libmariadb.so.3 를 링크한다 — debian 쪽은 csp 패키지가 .deb 로 동봉하지만
    # rpm 동봉물이 아직 없어서 OS 패키지로 받는다 (05 단계의 기본 대상과 같아야 한다).
    #   mysql-selinux 는 mariadb-server 의 **조건부(rich) 의존**이다:
    #     (mysql-selinux >= 1.0.10 if selinux-policy-targeted)
    #   `dnf download --resolve` 는 이런 조건부 의존을 따라가지 않아서 빠진다 — SELinux 가
    #   기본인 rhel 계열에서는 조건이 항상 참이라 없으면 설치가 막힌다(2026-09-11 실측).
    mariadb) PKGS=(mariadb-server mariadb mariadb-connector-c mysql-selinux) ;;
    # OAM·CSC 의 동봉 확장이 cpython-314 ABI 전용이다. 배포판이 3.14 를 주면 그것을 쓴다.
    python)  PKGS=(python3.14) ;;
    "")      die "사용법: ./tb-fetch-rpms.sh <set> [패키지...]   (set: mariadb | python | <패키지명>)" ;;
    *)       PKGS=("$SET" "${rest[@]}"); SET="custom" ;;
esac

command -v dnf >/dev/null || die "dnf 가 없습니다 — rhel 계열 장비에서 실행하세요"
[[ "$(tb_pkg_family)" == "rhel" ]] || warn "이 장비는 rhel 계열이 아닙니다 — 받은 rpm 이 대상과 안 맞을 수 있습니다"

out="$TB_OFFLINE_DIR/rpms/$SET"
mkdir -p "$out"

header "=== .rpm 수집 — set=$SET ($(tb_os_pretty)) ==="
info "대상: ${PKGS[*]}"
warn "대상 장비와 **같은 배포판·같은 메이저 버전**이어야 합니다 — 현재: $(tb_os_pretty) $(uname -m)"

# ── 다운로드 ──────────────────────────────────────────────────
# dnf5(로키 10)와 dnf4 의 옵션이 조금 다르다. 되는 것을 순서대로 시도한다 —
# 어느 것이 먹었는지는 로그에 남는다.
info "[1/3] 다운로드 → $out"
rm -f "$out"/*.rpm
_dl_ok=0
_try_dl() {   # $@ = 통째로 실행할 명령. 성공하면 0.
    if "$@" >/dev/null 2>&1; then ok "$*"; _dl_ok=1; return 0; fi
    warn "실패: $* — 다음 방식으로 재시도"
    return 1
}
_try_dl dnf download --resolve --alldeps --destdir "$out" "${PKGS[@]}" \
  || _try_dl dnf download --resolve --destdir "$out" "${PKGS[@]}" \
  || _try_dl dnf install -y --downloadonly --downloaddir "$out" "${PKGS[@]}" \
  || true

[[ $_dl_ok -eq 1 ]] || die_hint "rpm 다운로드에 실패했습니다" \
    "저장소가 살아 있는지 먼저 보세요: dnf repolist" \
    "dnf download 가 없으면: sudo dnf install -y dnf-plugins-core"

shopt -s nullglob
got=("$out"/*.rpm)
shopt -u nullglob
[[ ${#got[@]} -gt 0 ]] || die "받은 .rpm 이 없습니다 ($out)"

# ── 설치 대상 목록 ────────────────────────────────────────────
# 05 단계가 이 파일을 읽어 "무엇을 깔라고 요청할지" 를 정한다. deb 쪽과 같은 계약이다.
info "[2/4] 설치 대상 목록"
printf '%s\n' "${PKGS[@]}" > "$out/tb-targets.txt"
ok "$out/tb-targets.txt (${#PKGS[@]}개)"

# ── 설치 해결 시뮬레이션 ──────────────────────────────────────
# **여기서 잡지 못하면 폐쇄망 현장에서 잡아야 한다.** `--resolve` 가 놓치는 의존이 있다
# (조건부/rich 의존 — 위 mysql-selinux 사례). 그래서 목록을 만든 뒤 dnf 에게 **실제로
# 풀어 보게** 한다. 네트워크가 살아 있는 이 자리라면 빠진 것을 바로 더 받으면 된다.
#
# `--assumeno` 는 해결까지만 하고 설치 직전에 거절한다 — 이 장비를 바꾸지 않는다.
# 해결 자체는 root 권한을 요구하므로, 비root 로 돌렸으면 건너뛰고 그 사실을 알린다.
info "[3/4] 설치 해결 시뮬레이션"
if [[ $EUID -ne 0 ]]; then
    warn "root 가 아니라 건너뜁니다 — 빠진 의존은 대상 장비에서야 드러납니다"
    warn "  확인하려면: sudo $0 $SET"
else
    sim="$(LC_ALL=C dnf install --assumeno --disablerepo='*' "$out"/*.rpm 2>&1 || true)"
    if grep -qE 'nothing provides|cannot install the best|Problem [0-9]*:?' <<< "$sim"; then
        err "반입 rpm 만으로는 설치가 풀리지 않습니다 — 빠진 의존이 있습니다"
        grep -E 'nothing provides|Problem|requires' <<< "$sim" | head -12 | sed 's/^/        /' >&2
        die_hint "위 줄에서 **빠진 패키지 이름**을 읽어 같은 디렉토리에 더 받으세요" \
            "  dnf download --resolve --alldeps --destdir $out <패키지명>" \
            "받은 뒤 이 스크립트를 다시 돌리면 이 검사가 통과해야 합니다."
    fi
    ok "dnf 가 이 묶음만으로 설치를 풀 수 있습니다"
fi

# ── 요약 ──────────────────────────────────────────────────────
info "[4/4] 요약"
ok "$out — ${#got[@]}개, $(du -sh "$out" | awk '{print $1}')"
ls -1 "$out"/*.rpm | sed 's|.*/|        |'

cat <<EOF

  다음: 이 디렉토리를 반입본에 담아(또는 USB 로 옮겨) 대상 장비에서
    sudo ./tb-install.sh --role db
EOF
