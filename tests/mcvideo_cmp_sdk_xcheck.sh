#!/usr/bin/env bash
# MCVideo 전송 제어 교차 스모크 빌드·실행 — CMP(PMcvControl) ↔ SDK 참여자(tc_participant). 설명 = tests/mcvideo_cmp_sdk_xcheck.cpp 머리.
#   사용: tests/mcvideo_cmp_sdk_xcheck.sh [CMP 바이너리 — 기본 build/bin/cmp]   (선행: make cmp · Linux 코어 빌드의 pkg/pjproject)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMP="${1:-$ROOT/build/bin/cmp}"
PJ="$ROOT/pkg/pjproject"
[ -x "$CMP" ] || { echo "cmp binary not found: $CMP (make cmp)"; exit 2; }
[ -f "$PJ/lib/libpj-x86_64-pc-linux-gnu.a" ] || { echo "pjlib not found under $PJ (Linux 코어 빌드 선행)"; exit 2; }
OUT="$(mktemp -d /tmp/mcv_xcheck_bin_XXXXXX)"
trap 'rm -rf "$OUT"' EXIT
# pj 설정 매크로는 코어 빌드와 같게(PJ_AUTOCONF 등)
DEFS="$(grep -o '\-DPJ[^ ]*' "$ROOT/build/sdk/core/CMakeFiles/cimsue_objs.dir/flags.make" 2>/dev/null | sort -u | tr '\n' ' ' || true)"
g++ -std=c++17 -O1 -I "$ROOT/sdk/core/include" -I "$ROOT/sdk/core/src" -I "$PJ/include" $DEFS \
    "$ROOT/tests/mcvideo_cmp_sdk_xcheck.cpp" "$ROOT/sdk/core/src/mcvideo/tc_participant.cpp" \
    "$ROOT/sdk/core/src/mcvideo/tc_codec.cpp" "$ROOT/sdk/core/src/types.cpp" \
    -L "$PJ/lib" -lpj-x86_64-pc-linux-gnu -lssl -lcrypto -lpthread -o "$OUT/xcheck"
"$OUT/xcheck" "$CMP"
