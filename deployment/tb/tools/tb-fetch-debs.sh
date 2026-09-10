#!/usr/bin/env bash
# =============================================================
# tb-fetch-debs.sh — 폐쇄망 반입용 .deb 수집 (인터넷 되는 빌드 장비에서 실행)
#
#   ./tb-fetch-debs.sh mariadb          MariaDB 서버·클라이언트 + 의존
#   ./tb-fetch-debs.sh apt-repair       깨진 apt 를 되살리는 최소 집합 (아래 참조)
#   ./tb-fetch-debs.sh mariadb --all    required/important 까지 전부
#
# 산출: deployment/tb/offline/debs/<set>/*.deb  → 이 디렉토리를 USB 로 옮긴다.
#
# 의존 고르기: apt-cache 로 재귀 의존을 뽑고, **모든 시스템에 반드시 있는 것**
# (Priority = required/important)만 제외한다 — libc6·zlib1g 류를 반입해봐야
# 용량만 늘고 의미가 없다.
#
# ⚠ `standard` 는 제외하지 않는다. Debian 정책의 `standard` 는 "표준 설치에 포함"
# 이지 "모든 설치에 포함"이 아니다 — 최소·클라우드 이미지에는 없다. 수집 장비(개발
# 서버)에는 있고 대상 장비에는 없어서, 같은 반입본이 한 서버에서는 되고 다른 서버에서는
# 05 단계가 `lsof/psmisc/rsync ... not installable` 로 막혔다(2026-09-10 실측).
# 제외 기준을 장비 상태가 아니라 정책 등급으로 두는 것이 요점이다.
#
# 추정이 틀리면 대상 장비의 05 단계가 부족한 패키지 이름을 정확히 알려주므로,
# 그 이름으로 다시 수집하면 된다.
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

SET="${1:-}"; shift || true
ALL=0
rest=()
for a in "$@"; do
    if [[ "$a" == "--all" ]]; then ALL=1; else rest+=("$a"); fi
done

case "$SET" in
    mariadb) PKGS=(mariadb-server mariadb-client) ;;
    # apt 가 잠긴 시스템을 되살리는 집합. 한 패키지의 의존이 비어 있으면 apt 는 **관계없는
    # 설치까지 전부 거부**하므로(전체 의존 상태를 먼저 검사한다), 폐쇄망에서는 이걸 못 받아와
    # 05 단계에서 영구히 막힌다. keepalived 를 지울 때 libnl 이 함께 걷혀 가는 경우가 대표적.
    apt-repair) PKGS=(libnl-3-200 libnl-genl-3-200 libsnmp40t64) ;;
    "")      die "사용법: ./tb-fetch-debs.sh <set> [--all]   (set: mariadb | <패키지명>...)" ;;
    # 임의 패키지 수집 — 첫 인자도 패키지명으로 취급하고 산출은 debs/custom/ 에 담는다.
    *)       PKGS=("$SET" "${rest[@]}"); SET="custom" ;;
esac

command -v apt-cache >/dev/null || die "apt 계열 배포판에서 실행하세요"

out="$TB_OFFLINE_DIR/debs/$SET"
mkdir -p "$out"

header "=== .deb 수집 — set=$SET ($(tb_os_pretty)) ==="
info "대상: ${PKGS[*]}"

# ── 의존 폐쇄집합 ─────────────────────────────────────────────
info "[1/4] 재귀 의존 계산"
# set -e 주의: apt-cache 는 가상 패키지에서 비0 을 낸다 — 대입이 실패하면 셸이 죽으므로
# 모든 apt-cache 대입에 || true 를 붙인다.
closure="$(apt-cache depends --recurse --no-recommends --no-suggests \
              --no-conflicts --no-breaks --no-replaces --no-enhances \
              "${PKGS[@]}" 2>/dev/null \
           | grep -v '^ ' | grep -v '^<' | sed 's/:.*//' | sort -u || true)"

# 정확버전(`=`)으로 묶인 짝은 **우선순위와 무관하게 함께** 담는다.
#
# `perl` 은 `perl-base (= <같은 버전>)` 과 `libperl5.40 (= …)` 을 요구한다. perl-base 는
# Priority=required 라 위 규칙이 제외하는데, 대상 장비의 perl-base 가 조금이라도 다른
# 버전이면(실측: 담은 것 0.3 / 대상 0.2) apt 가 절대 풀 수 없다:
#   perl Depends perl-base (= 0.3) but none of the choices are installable
# 이런 lock-step 짝은 "기본 설치에 있으니 됐다" 는 가정이 성립하지 않는다.
_locked=()
while read -r p; do
    [[ -z "$p" ]] && continue
    while read -r dep; do
        [[ -n "$dep" ]] && _locked+=("$dep")
    done < <(apt-cache show "$p" 2>/dev/null \
             | awk -F': ' '/^Depends: /{print $2; exit}' \
             | tr ',' '\n' | grep '(= ' | awk '{print $1}' || true)
done <<< "$closure"
_locked_uniq="$(printf '%s\n' "${_locked[@]+"${_locked[@]}"}" | sort -u)"

want=()
skipped=()
while read -r p; do
    [[ -z "$p" ]] && continue
    prio="$(apt-cache show "$p" 2>/dev/null | awk -F': ' '/^Priority: /{print $2; exit}' || true)"
    # 실체 없는 가상 패키지(awk·debconf-2.0 등)는 받을 것이 없다 — 조용히 건너뛴다.
    [[ -z "$prio" ]] && { skipped+=("$p(가상)"); continue; }
    if [[ $ALL -eq 0 && "$prio" =~ ^(required|important)$ ]] \
       && ! grep -qx -- "$p" <<< "$_locked_uniq"; then
        skipped+=("$p($prio)")
        continue
    fi
    want+=("$p")
done <<< "$closure"

[[ ${#want[@]} -gt 0 ]] || die "수집 대상이 없습니다 — 패키지명을 확인하세요"
info "수집 ${#want[@]}개 / 제외 ${#skipped[@]}개 (배포판 기본 설치 추정)"
[[ ${#skipped[@]} -gt 0 ]] && echo "        제외: ${skipped[*]}"

# ── 다운로드 ──────────────────────────────────────────────────
info "[2/4] 다운로드 → $out"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=()
for p in "${want[@]}"; do
    ( cd "$tmp" && apt-get download "$p" >/dev/null 2>&1 ) || fail+=("$p")
done
shopt -s nullglob
got=("$tmp"/*.deb)
shopt -u nullglob
[[ ${#got[@]} -gt 0 ]] || die "다운로드된 .deb 가 없습니다 (apt-get update 후 재시도)"
cp -f "${got[@]}" "$out/"

[[ ${#fail[@]} -gt 0 ]] && warn "받지 못한 패키지: ${fail[*]} (가상 패키지일 수 있음 — 무해)"

# ── apt 색인 + 설치 대상 목록 ────────────────────────────────
# 대상 장비의 05 단계가 이 디렉토리를 로컬 apt 저장소로 붙인다. 색인이 있으면 apt 가
# "이미 있는 것은 건너뛰고, 필요한 것만, 맞는 순서로" 깐다 — dpkg -i 로는 대체 제공자
# 충돌(예: opensysusers ↔ systemd sysusers)에서 실패한다.
info "[3/4] apt 색인 생성"
printf '%s\n' "${PKGS[@]}" > "$out/tb-targets.txt"
if command -v dpkg-scanpackages >/dev/null; then
    ( cd "$out" && dpkg-scanpackages -m . /dev/null 2>/dev/null > Packages ) \
        && gzip -9cf "$out/Packages" > "$out/Packages.gz" \
        && ok "Packages 색인 $(grep -c '^Package: ' "$out/Packages")개"
elif command -v apt-ftparchive >/dev/null; then
    ( cd "$out" && apt-ftparchive packages . > Packages ) \
        && gzip -9cf "$out/Packages" > "$out/Packages.gz" \
        && ok "Packages 색인 (apt-ftparchive)"
else
    warn "dpkg-scanpackages 없음 (dpkg-dev 패키지) — 색인 없이 반입되며 대상 장비는 dpkg 직접 설치로 폴백합니다"
fi

# ── 요약 ──────────────────────────────────────────────────────
info "[4/4] 요약"
sz="$(du -sh "$out" | awk '{print $1}')"
ok "$out — $(ls -1 "$out"/*.deb | wc -l)개, $sz"
ls -1 "$out"/*.deb | sed 's|.*/|        |'

cat <<EOF

  다음: 이 디렉토리를 USB 로 옮기고, 대상 장비에서
    sudo ./tb-install.sh --role db
EOF
