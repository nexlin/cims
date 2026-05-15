#!/usr/bin/env bash
# deployment/bin/check-all.sh — 모든 env × scenario 조합 render --check-only 회기.
#
# 사용:
#   ./bin/check-all.sh                 # PASS/FAIL 표 출력, FAIL 있으면 exit 1
#   ./bin/check-all.sh --bundle <dir>  # render 결과 bundle 도 <dir>/<env>__<scn>/ 에 생성

set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RENDER="$SCRIPT_DIR/render.py"

BUNDLE_ROOT=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --bundle) BUNDLE_ROOT="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--bundle <dir>]"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

# env 디렉토리 = _schema 가 아닌, scenarios/ 가 있는 디렉토리
ENVS=()
for d in "$ROOT"/*/; do
    base=$(basename "$d")
    [[ "$base" == _* ]] && continue
    [[ "$base" == bin ]] && continue
    [[ -d "$d/scenarios" ]] || continue
    ENVS+=("$base")
done

# 결과 수집
PASS=0; FAIL=0
RESULTS=()
for env in "${ENVS[@]}"; do
    for scn_file in "$ROOT/$env"/scenarios/*.yaml; do
        [[ -f "$scn_file" ]] || continue
        scn=$(basename "$scn_file" .yaml)
        if [[ -n "$BUNDLE_ROOT" ]]; then
            out="$BUNDLE_ROOT/${env}__${scn}"
            cmd=( "$RENDER" --env "$env" --scenario "$scn" --out "$out" )
        else
            cmd=( "$RENDER" --env "$env" --scenario "$scn" --check-only )
        fi
        if "${cmd[@]}" >/dev/null 2>&1; then
            RESULTS+=("PASS|$env|$scn")
            PASS=$((PASS+1))
        else
            RESULTS+=("FAIL|$env|$scn")
            FAIL=$((FAIL+1))
        fi
    done
done

echo "─────────────────────────────────────────────"
echo " 시나리오 회기 결과 (env × scenario)"
echo "─────────────────────────────────────────────"
printf ' %-6s %-22s %s\n' "결과" "환경" "시나리오"
echo "─────────────────────────────────────────────"
for line in "${RESULTS[@]}"; do
    IFS='|' read -r status env scn <<< "$line"
    if [[ "$status" == "PASS" ]]; then
        printf ' \033[32m%-6s\033[0m %-22s %s\n' "$status" "$env" "$scn"
    else
        printf ' \033[31m%-6s\033[0m %-22s %s\n' "$status" "$env" "$scn"
    fi
done
echo "─────────────────────────────────────────────"
echo " PASS=$PASS  FAIL=$FAIL  total=$((PASS+FAIL))"
[[ -n "$BUNDLE_ROOT" ]] && echo " bundles → $BUNDLE_ROOT/"

exit $(( FAIL > 0 ? 1 : 0 ))
