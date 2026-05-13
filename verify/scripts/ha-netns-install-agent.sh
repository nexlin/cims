#!/usr/bin/env bash
# verify/scripts/ha-netns-install-agent.sh <ns> <enrollment-token> <agent-name>
#
# NetNS 안에서 cims_agent install + 백그라운드 실행.
# Console UI 가 발행한 install_command 의 NetNS 진입 wrapper.
#
# 사전 조건:
#   - ha-netns-up.sh 로 ns 구축됨
#   - TB-CSC (또는 운영 CSC) 가 10.0.0.1:4419 에서 listen 중
#   - csc.json 의 Server.PublicUrl = https://10.0.0.1:4419 설정됨
#
# 사용:
#   sudo ./verify/scripts/ha-netns-install-agent.sh ctrl-a <TOKEN> ctrl-01
set -euo pipefail
[[ $EUID -eq 0 ]] || exec sudo "$0" "$@"

NS="${1:?usage: $0 <ns> <token> <agent-name>}"
TOKEN="${2:?token required}"
NAME="${3:?agent-name required}"

CSC_URL="https://10.0.0.1:4419"
# 검증 환경 — repo build/dist/ 하위에 ns 별 분리 설치.
# 운영 환경에서는 /opt/cims/agent/ 등 영구 위치 권장.
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASE_DIR="${REPO_ROOT}/build/dist/netns-agents/${NS}"
INSTALL_DIR="${BASE_DIR}/install"
LOG_FILE="${BASE_DIR}/agent.log"

# ns 존재 확인
if ! ip netns list | grep -q "^${NS}\b"; then
    echo "[error] ns '${NS}' 없음 — ./verify/scripts/ha-netns-up.sh 먼저 실행"
    exit 1
fi

mkdir -p "$INSTALL_DIR"

echo "[${NS}] install 시작 (target=${NAME}, csc=${CSC_URL})"

# install — NetNS 안에서 systemd 비활성 모드
ip netns exec "$NS" bash -c "
    cd '$INSTALL_DIR' && \
    curl -k '${CSC_URL}/install-agent.sh' | bash -s -- \
        --csc-url '${CSC_URL}' \
        --enrollment-token '${TOKEN}' \
        --name '${NAME}' \
        --no-systemd
"

# 백그라운드 실행 (이미 실행 중이면 kill 후 재실행)
ip netns exec "$NS" bash -c "
    pkill -f 'cims_agent.py.*${NAME}' 2>/dev/null || true
    sleep 0.5
    cd '$INSTALL_DIR' && \
    nohup ./run.sh > '$LOG_FILE' 2>&1 &
    sleep 1
    pid=\$(pgrep -f 'cims_agent.py.*${NAME}' | head -1)
    echo \"[${NS}] cims_agent ${NAME} PID=\${pid:-?} → ${LOG_FILE}\"
"

echo "[done] ${NS} ← ${NAME} install + running"
