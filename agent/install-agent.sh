#!/usr/bin/env bash
# CIMS Agent 설치 스크립트 (사용자 권한, 현재 디렉토리 설치)
# Usage:
#   cd /path/to/install    # ← 여기에 설치됨
#   curl -k https://<CSC>:4420/install-agent.sh | bash -s -- \
#        --csc-url https://<CSC>:4420 \
#        --enrollment-token <token> \
#        --name <agent-name>
#
# 설치 내용 (모두 현재 디렉토리 기준):
#   - ./cims_agent.py              (에이전트 바이너리)
#   - ./state/                     (state.json 보관)
#   - ./run.sh                     (포그라운드 수동 실행용 런처)
#   - ~/.config/systemd/user/cims-agent.service  (선택적 — 자동 기동)
#
# 부팅 시 자동 기동하려면 (로그인 없이도):
#   sudo loginctl enable-linger $USER     # 한 번만

set -euo pipefail

CSC_URL=""
ENROLL_TOKEN=""
AGENT_NAME="$(hostname)"
USE_SYSTEMD="auto"   # auto|yes|no

while [[ $# -gt 0 ]]; do
    case "$1" in
        --csc-url)           CSC_URL="$2"; shift 2 ;;
        --enrollment-token)  ENROLL_TOKEN="$2"; shift 2 ;;
        --name)              AGENT_NAME="$2"; shift 2 ;;
        --no-systemd)        USE_SYSTEMD="no"; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

if [[ -z "$CSC_URL" || -z "$ENROLL_TOKEN" ]]; then
    echo "Usage: $0 --csc-url <URL> --enrollment-token <TOKEN> [--name <NAME>] [--no-systemd]"
    exit 1
fi

if [[ $EUID -eq 0 ]]; then
    echo "ERROR: root 로 실행하지 마세요. 서비스 운영 계정으로 실행하세요."
    exit 1
fi

INSTALL_DIR="$(pwd)"
STATE_DIR="$INSTALL_DIR/state"
BIN_FILE="$INSTALL_DIR/cims_agent.py"
LAUNCHER="$INSTALL_DIR/run.sh"

echo "==> Installing CIMS Agent to current directory"
echo "    dir    : $INSTALL_DIR"
echo "    user   : $USER"
echo "    name   : $AGENT_NAME"

mkdir -p "$STATE_DIR"
chmod 700 "$STATE_DIR"

# agent 바이너리 (local dev 에서 복사하거나 CSC 에서 다운로드)
if [[ -f "$(dirname "$0")/cims_agent.py" ]]; then
    cp "$(dirname "$0")/cims_agent.py" "$BIN_FILE"
else
    curl -fsSLk "$CSC_URL/cims_agent.py" -o "$BIN_FILE"
fi
chmod 755 "$BIN_FILE"

# 수동 실행용 런처 (systemd 사용 안 해도 nohup/screen 등으로 띄울 수 있음)
cat > "$LAUNCHER" <<EOF
#!/usr/bin/env bash
set -eu
cd "\$(dirname "\$0")"
export CIMS_ENROLLMENT_TOKEN="\${CIMS_ENROLLMENT_TOKEN:-$ENROLL_TOKEN}"
exec /usr/bin/python3 ./cims_agent.py \\
    --csc-url "$CSC_URL" \\
    --state-dir "./state" \\
    --name "$AGENT_NAME"
EOF
chmod 755 "$LAUNCHER"

# systemd --user 사용 여부 결정
if [[ "$USE_SYSTEMD" == "auto" ]]; then
    if command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; then
        USE_SYSTEMD="yes"
    else
        USE_SYSTEMD="no"
    fi
fi

if [[ "$USE_SYSTEMD" == "yes" ]]; then
    UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
    UNIT_FILE="$UNIT_DIR/cims-agent.service"
    mkdir -p "$UNIT_DIR"

    echo "==> Writing user systemd unit: $UNIT_FILE"
    cat > "$UNIT_FILE" <<EOF
[Unit]
Description=CIMS Server Agent (dir=$INSTALL_DIR)
After=network-online.target default.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$INSTALL_DIR
Environment=CIMS_ENROLLMENT_TOKEN=$ENROLL_TOKEN
ExecStart=/usr/bin/python3 $BIN_FILE \\
    --csc-url $CSC_URL \\
    --state-dir $STATE_DIR \\
    --name $AGENT_NAME
Restart=always
RestartSec=10

[Install]
WantedBy=default.target
EOF

    systemctl --user daemon-reload
    systemctl --user enable --now cims-agent.service

    echo "==> Status:"
    systemctl --user --no-pager status cims-agent || true
    echo ""
    echo "로그:   journalctl --user -u cims-agent -f"
    echo "제어:   systemctl --user {status|restart|stop} cims-agent"
    if ! loginctl show-user "$USER" 2>/dev/null | grep -q '^Linger=yes'; then
        echo ""
        echo "※ 로그아웃 후에도 자동 기동되려면 (1회):"
        echo "     sudo loginctl enable-linger $USER"
    fi
else
    echo "==> systemd 미사용 모드 — 수동 기동 필요"
    echo "    포그라운드:   ./run.sh"
    echo "    백그라운드:   nohup ./run.sh > agent.log 2>&1 &"
    # 첫 enroll 만 수행해서 state.json 생성 — 이후 token 은 재사용 불가
    echo ""
    echo "==> Running first-time enroll (3s)"
    (cd "$INSTALL_DIR" && CIMS_ENROLLMENT_TOKEN="$ENROLL_TOKEN" \
        timeout 3 /usr/bin/python3 "$BIN_FILE" \
            --csc-url "$CSC_URL" \
            --state-dir "$STATE_DIR" \
            --name "$AGENT_NAME" ) || true
    if [[ -f "$STATE_DIR/state.json" ]]; then
        echo "    ✓ enroll 완료 (state.json 생성됨)"
    else
        echo "    ⚠ enroll 실패 — token 확인 후 ./run.sh 로 수동 기동"
    fi
fi

echo ""
echo "==> Done."
