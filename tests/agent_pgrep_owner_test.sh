#!/bin/bash
# agent 모듈 프로세스 판정의 설치 트리 소유 검사 단위시험 — 라이브 무접촉, 임시 prefix + 가짜 python 데몬
#
# 왜: `_pgrep_module` 이 이름(`<stem>_app.py`)만으로 잡으면 같은 호스트에 동거하는 소스 트리의
# dev OAM 이 배포본 `oam` 으로 오귀속된다 — 배포본을 내려도 live_state=up 이라 업그레이드가
# module_running 409 로 막힌다(2026-09-18·09-22 실측). 아래 4건이 그 재발을 막는다.
#
#   T1  트리 안 프로세스      supervised.json 의 install_path(current 심볼릭) 아래에서 도는 데몬 → 잡힌다
#   T2  트리 밖 프로세스      같은 이름의 데몬이 설치 루트 밖(dev src)에서만 돌면 → 없음
#   T3  동거                  둘 다 돌면 트리 안 pid 만 돌려준다
#   T4  legacy(루트 미상)     supervised.json 에 없는 모듈은 이름만으로 잡는다(종전 동작)
#
# 실행: tests/agent_pgrep_owner_test.sh
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d /tmp/pgrepown_test.XXXXXX)"
trap 'pkill -f "$T/" 2>/dev/null; rm -rf "$T"' EXIT

FAIL=0
chk() { # chk <이름> <실제> <기대>
    if [[ "$2" == "$3" ]]; then echo "  OK   $1"
    else echo "  FAIL $1 — 실제='$2' 기대='$3'"; FAIL=1; fi
}

# 배포본 트리: <prefix>/modules/foo/1.0.0/foo/src/foo_app.py , current → 1.0.0
VDIR="$T/modules/foo/1.0.0"
mkdir -p "$VDIR/foo/src" "$T/run" "$T/dev/src" "$T/agent"
cat > "$VDIR/foo/src/foo_app.py" <<'PY'
import time
while True: time.sleep(1)
PY
cp "$VDIR/foo/src/foo_app.py" "$T/dev/src/foo_app.py"
ln -s "$VDIR" "$T/modules/foo/current"
echo "{\"foo\": \"$T/modules/foo/current\"}" > "$T/run/supervised.json"

probe() { # probe <module> → pid 또는 none
    CIMS_AGENT_PREFIX="$T" CIMS_AGENT_INSTALL_ROOT="$T/modules" \
    python3 -c "import sys; sys.path.insert(0, '$REPO_ROOT/agent'); import cims_agent as a
h = a._pgrep_module('$1'); print(h[0] if h else 'none')" 2>/dev/null
}

# T1 — 트리 안
( cd "$VDIR/foo/src" && exec python3 foo_app.py ) & IN=$!
sleep 0.3
chk "T1 트리 안 프로세스 → 잡힘" "$(probe foo)" "$IN"
kill $IN 2>/dev/null; wait $IN 2>/dev/null

# T2 — 트리 밖만
( cd "$T/dev/src" && exec python3 foo_app.py ) & OUT=$!
sleep 0.3
chk "T2 트리 밖 프로세스만 → none" "$(probe foo)" "none"

# T3 — 동거
( cd "$VDIR/foo/src" && exec python3 foo_app.py ) & IN=$!
sleep 0.3
chk "T3 동거 → 트리 안 pid" "$(probe foo)" "$IN"
kill $IN 2>/dev/null; wait $IN 2>/dev/null

# T4 — legacy: supervised.json 에 없는 모듈(bar)은 이름만으로
cp "$T/dev/src/foo_app.py" "$T/dev/src/bar_app.py"
( cd "$T/dev/src" && exec python3 bar_app.py ) & BAR=$!
sleep 0.3
chk "T4 루트 미상 모듈 → 이름만으로 잡힘" "$(probe bar)" "$BAR"
kill $BAR $OUT 2>/dev/null; wait $BAR $OUT 2>/dev/null

[[ $FAIL -eq 0 ]] && echo "PASS agent_pgrep_owner_test (4/4)" || { echo "FAIL agent_pgrep_owner_test"; exit 1; }
