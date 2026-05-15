#!/usr/bin/env bash
# deployment/bin/sync-agent.sh — agent 코드/바이너리를 LIVE 환경에 동기화.
#
# 사용:
#   ./sync-agent.sh                # 모든 자원 (lifecycle.sh + csp + cmp 바이너리)
#   ./sync-agent.sh --lifecycle    # lifecycle.sh 만
#   ./sync-agent.sh --bins         # csp/cmp 바이너리만
#   ./sync-agent.sh --nodes ctrl-a,ctrl-b   # 특정 노드만
#
# 동작:
#   - lifecycle.sh: agent/lib/ → build/dist/agent/lib/ + 각 install dir
#   - csp 바이너리: build/bin/csp → 각 csp install dir (install 명령 atomic, busy 회피)
#   - cmp 바이너리: build/bin/cmp → 각 cmp install dir (동일)

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST="$REPO/build/dist"
NETNS="$DIST/netns-agents"

DO_LIFECYCLE=1
DO_BINS=1
NODES=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --lifecycle) DO_BINS=0; shift ;;
        --bins)      DO_LIFECYCLE=0; shift ;;
        --nodes)     NODES="$2"; shift 2 ;;
        -h|--help)
            sed -n '1,15p' "$0" | tail -14; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

# 노드 목록 결정
if [[ -n "$NODES" ]]; then
    IFS=',' read -ra ALL_NODES <<< "$NODES"
else
    ALL_NODES=()
    for d in "$NETNS"/*/; do
        [[ -d "$d" ]] || continue
        ALL_NODES+=("$(basename "$d")")
    done
fi

# 1) lifecycle.sh 동기화
if [[ $DO_LIFECYCLE -eq 1 ]]; then
    SRC="$REPO/agent/lib/lifecycle.sh"
    [[ -f "$SRC" ]] || { echo "[error] $SRC 없음" >&2; exit 3; }
    echo "── lifecycle.sh 동기화 ──"
    cp "$SRC" "$DIST/agent/lib/lifecycle.sh"
    echo "  build/dist/agent/lib/lifecycle.sh ✓"
    for ns in "${ALL_NODES[@]}"; do
        DST="$NETNS/$ns/install/agent/lib/lifecycle.sh"
        if [[ -d "$NETNS/$ns/install/agent/lib" ]]; then
            cp "$SRC" "$DST" && echo "  $ns ✓" || echo "  $ns FAIL"
        else
            echo "  $ns (agent 미설치) skip"
        fi
    done
fi

# 2) 바이너리 동기화 (atomic install — text file busy 회피)
sync_binary() {
    local module="$1"     # csp | cmp
    local upper="${module^^}"
    local src="$REPO/build/bin/$module"
    [[ -x "$src" ]] || return 0
    local synced=0
    for ns in "${ALL_NODES[@]}"; do
        local dst="$NETNS/$ns/install/modules/$module/0.0.1/$upper/$module/bin/$module"
        if [[ -f "$dst" ]]; then
            install -m 755 "$src" "$dst" && { echo "  $ns/$module ✓ ($(stat -c %s "$dst") bytes)"; synced=$((synced+1)); }
        fi
    done
    [[ $synced -gt 0 ]] || echo "  $module 적용 노드 없음"
}

if [[ $DO_BINS -eq 1 ]]; then
    echo "── 바이너리 atomic install ──"
    sync_binary csp
    sync_binary cmp
fi

echo ""
echo "✓ 동기화 완료"
echo "  다음: ./bin/apply.py --env <e> --scenario <s> --backup --restart auto"
