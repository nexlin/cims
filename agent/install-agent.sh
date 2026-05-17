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
BIN_FILE="$INSTALL_DIR/agent/cims_agent.py"
LAUNCHER="$INSTALL_DIR/run.sh"

echo "==> Installing CIMS Agent to current directory"
echo "    dir    : $INSTALL_DIR"
echo "    user   : $USER"
echo "    name   : $AGENT_NAME"

mkdir -p "$STATE_DIR"
chmod 700 "$STATE_DIR"

# agent bundle (tarball) — cims_agent.py + bin/{cims-priv,cims-ha,cims-svc,...} + lib/ + keepalived/ + systemd/
# tarball 안 layout: agent/cims_agent.py, agent/bin/..., agent/lib/..., agent/keepalived/..., agent/systemd/...
echo "==> Downloading agent bundle"
BUNDLE_TMP="$(mktemp /tmp/cims-agent-bundle.XXXXXX.tar.gz)"
trap 'rm -f "$BUNDLE_TMP"' EXIT
if ! curl -fsSLk "$CSC_URL/agent-bundle.tar.gz" -o "$BUNDLE_TMP"; then
    echo "ERROR: failed to download $CSC_URL/agent-bundle.tar.gz"
    exit 4
fi
# extract — agent/ 하위만 INSTALL_DIR 에 풀기 (cims.sh, meta.json 등 최상위 noise 제외)
if ! tar xzf "$BUNDLE_TMP" -C "$INSTALL_DIR" agent/ 2>/dev/null; then
    # 일부 tarball 은 agent/ filter 미지원 — fallback 전체 extract
    tar xzf "$BUNDLE_TMP" -C "$INSTALL_DIR"
fi
if [[ ! -f "$BIN_FILE" ]]; then
    echo "ERROR: tarball extracted but $BIN_FILE not found"
    exit 5
fi
chmod 755 "$BIN_FILE"
[[ -d "$INSTALL_DIR/agent/bin" ]] && chmod 755 "$INSTALL_DIR/agent/bin/"*

# 수동 실행용 런처 (systemd 사용 안 해도 nohup/screen 등으로 띄울 수 있음)
cat > "$LAUNCHER" <<EOF
#!/usr/bin/env bash
set -eu
cd "\$(dirname "\$0")"
export CIMS_ENROLLMENT_TOKEN="\${CIMS_ENROLLMENT_TOKEN:-$ENROLL_TOKEN}"
exec /usr/bin/python3 ./agent/cims_agent.py \\
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
    # unit 이름에 agent 이름 포함 → 같은 유저로 여러 agent 공존 가능
    # 안전한 파일명: 영문/숫자/하이픈 외는 '-' 로 치환
    UNIT_SAFE="$(echo "$AGENT_NAME" | tr -c 'A-Za-z0-9-' '-' | sed 's/-\+/-/g; s/^-//; s/-$//')"
    [[ -z "$UNIT_SAFE" ]] && UNIT_SAFE="default"
    UNIT_NAME="cims-agent-${UNIT_SAFE}.service"
    UNIT_FILE="$UNIT_DIR/$UNIT_NAME"
    mkdir -p "$UNIT_DIR"

    # 레거시 unit 파일(cims-agent.service) 이 다른 agent 를 가리키고 있으면 경고
    LEGACY_UNIT="$UNIT_DIR/cims-agent.service"
    if [[ -f "$LEGACY_UNIT" ]] && ! grep -q "WorkingDirectory=$INSTALL_DIR" "$LEGACY_UNIT" 2>/dev/null; then
        echo "※ 레거시 unit $LEGACY_UNIT 이 다른 경로를 가리킴 — 덮어쓰지 않음"
    fi

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
    # enable --now 는 이미 active 면 재시작 안 함 → 명시적으로 restart
    systemctl --user enable "$UNIT_NAME"
    if systemctl --user is-active --quiet "$UNIT_NAME"; then
        echo "==> 기존 agent 프로세스 재시작 (새 설정 반영)"
        systemctl --user restart "$UNIT_NAME"
    else
        systemctl --user start "$UNIT_NAME"
    fi

    echo "==> Status:"
    systemctl --user --no-pager status "$UNIT_NAME" || true
    echo ""
    echo "로그:   journalctl --user -u $UNIT_NAME -f"
    echo "제어:   systemctl --user {status|restart|stop} $UNIT_NAME"
    if ! loginctl show-user "$USER" 2>/dev/null | grep -q '^Linger=yes'; then
        echo ""
        echo "※ 로그아웃 후에도 자동 기동되려면 (1회):"
        echo "     sudo loginctl enable-linger $USER"
    fi
else
    echo "==> systemd 미사용 모드 (DEV) — enroll + nohup 자동 기동"
    echo ""
    echo "==> Running first-time enroll (3s)"
    (cd "$INSTALL_DIR" && CIMS_ENROLLMENT_TOKEN="$ENROLL_TOKEN" \
        timeout 3 /usr/bin/python3 "$BIN_FILE" \
            --csc-url "$CSC_URL" \
            --state-dir "$STATE_DIR" \
            --name "$AGENT_NAME" ) || true
    if [[ ! -f "$STATE_DIR/state.json" ]]; then
        echo "    ⚠ enroll 실패 — token 확인 후 ./run.sh 로 수동 기동"
    else
        echo "    ✓ enroll 완료 (state.json 생성됨)"
        echo ""
        echo "==> 자동 기동 (nohup ./run.sh)"
        (cd "$INSTALL_DIR" && nohup ./run.sh > agent.log 2>&1 < /dev/null &)
        sleep 1
        AGENT_PID=$(pgrep -af cims_agent.py 2>/dev/null | grep -F -- "--name $AGENT_NAME" | awk '{print $1}' | head -1)
        if [[ -n "$AGENT_PID" ]]; then
            echo "    ✓ agent 기동 (pid=$AGENT_PID, log=$INSTALL_DIR/agent.log)"
            echo "      종료:  kill $AGENT_PID"
        else
            echo "    ⚠ agent 기동 확인 실패 — $INSTALL_DIR/agent.log 확인"
            echo "      수동 기동: cd $INSTALL_DIR && nohup ./run.sh > agent.log 2>&1 &"
        fi
    fi
fi

echo ""
echo "==> Done."
