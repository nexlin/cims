#!/bin/bash
# 이전 스텁 — pjproject 빌드는 sdk/android/build-native.sh 로 이동했다 (docs/design/features/ue_sdk.md §3·§8).
#   소스 정본은 ext/pjproject 트리 자체이며, 이 스크립트가 하던 upstream clone·[2-N] 패치 적용은 더 이상 없다
#   (패치 인벤토리는 ext/pjproject/README.CIMS.md). 인자는 그대로 전달한다.
echo "[m1_build_pjsip.sh] → sdk/android/build-native.sh 로 위임" >&2
exec "$(cd "$(dirname "$0")/../../.." && pwd)/sdk/android/build-native.sh" "$@"
