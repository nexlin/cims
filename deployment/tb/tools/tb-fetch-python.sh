#!/usr/bin/env bash
# =============================================================
# tb-fetch-python.sh — 동봉용 CPython 재배치 빌드 수집 (인터넷 되는 장비에서 실행)
#
#   ./tb-fetch-python.sh
#
# 산출: deployment/tb/offline/runtime/python-<버전>.tar.gz
#       → 05 단계가 이것을 <설치경로>/runtime/python 에 푼다.
#
# **왜 인터프리터를 동봉하는가.** OAM·CSC 의 동봉 확장이 `*.cpython-314-*.so` 라 CPython
# 3.14 가 있어야 하는데, 배포판이 그것을 주지 않는 경우가 실제로 있다:
#   · Rocky 10.0 / 10.1 — 저장소에 `python3.<버전>` 패키지가 아예 없다 (10.2 부터 생겼다)
#   · 폐쇄망이라 `dnf update` 로 마이너를 올리는 것도 자유롭지 않다
# 배포판 패키지로 받을 수 있으면 그쪽이 더 낫지만(보안 갱신이 따라온다), 받을 수 없는
# 노드에서는 이것이 유일한 길이다. 자세한 것은 docs/design/features/os_portability.md §4.
#
# 받는 것은 python-build-standalone 의 `install_only` 빌드다 — **재배치 가능**(어느 경로에
# 풀어도 동작)하고 glibc 2.17 이상이면 도는 정적 구성이라 배포판을 가리지 않는다.
#
# 버전은 **고정**한다. 자동 최신 추적을 하지 않는 이유: 반입본에 무엇이 들어갔는지가
# 날짜마다 달라지면 현장에서 재현이 안 된다. 올릴 때는 아래 세 값을 함께 고친다.
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

# ── 고정 값 (올릴 때 셋을 함께 고친다) ─────────────────────────
PY_VERSION="3.14.7"
PY_RELEASE="20260901"
PY_SHA256="3959f92825141e04adf44982d3a83ee57af0877e893b0796e04c1468749d9b04"

PY_FILE="cpython-${PY_VERSION}+${PY_RELEASE}-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"
PY_URL="https://github.com/astral-sh/python-build-standalone/releases/download/${PY_RELEASE}/${PY_FILE//+/%2B}"

out="$TB_OFFLINE_DIR/runtime"
dest="$out/python-${PY_VERSION}.tar.gz"
mkdir -p "$out"

header "=== 동봉 인터프리터 수집 — CPython $PY_VERSION ($PY_RELEASE) ==="

# 이미 받아 뒀고 무결하면 다시 받지 않는다 — 36MB 를 매번 내려받을 이유가 없다.
if [[ -f "$dest" ]] && echo "$PY_SHA256  $dest" | sha256sum -c - >/dev/null 2>&1; then
    ok "이미 있습니다 — $dest ($(du -h "$dest" | awk '{print $1}'))"
    exit 0
fi

info "[1/3] 다운로드 → $dest"
command -v curl >/dev/null || die "curl 이 필요합니다"
timeout 300 curl -fsSL -o "$dest.part" "$PY_URL" \
    || die_hint "다운로드 실패: $PY_URL" \
        "인터넷이 되는 장비에서 실행해야 합니다." \
        "막혀 있으면 위 URL 을 다른 경로로 받아 $dest 에 두세요."
mv -f "$dest.part" "$dest"

info "[2/3] 무결성 확인"
echo "$PY_SHA256  $dest" | sha256sum -c - >/dev/null 2>&1 \
    || die_hint "sha256 불일치 — 받은 파일을 쓰지 마세요" \
        "기대: $PY_SHA256" \
        "실제: $(sha256sum "$dest" | awk '{print $1}')"
ok "sha256 일치"

# 푸는 쪽(05 단계)이 버전을 알아야 로그에 찍을 수 있다. 파일명에서 유도하지 않는다 —
# 파일명 규칙이 바뀌면 조용히 틀린 값이 찍힌다.
printf '%s\n' "$PY_VERSION" > "$out/VERSION"

info "[3/3] 요약"
ok "$dest ($(du -h "$dest" | awk '{print $1}'))"
cat <<EOF

  이 파일은 tb-pack.sh 가 반입본에 함께 담는다.
  대상 장비의 05 단계가 <설치경로>/runtime/python 에 풀고, 그 인터프리터로
  agent·OAM·CSC 가 기동한다 (docs/design/features/os_portability.md §4).
EOF
