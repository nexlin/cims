#!/usr/bin/env bash
# CIMS Agent 설치 스크립트 (사용자 권한, 현재 디렉토리 설치)
# 정책: systemd --user + linger 단일 운영 — die 시 자동 재기동, host 재기동 시 자동 기동.
#       1 user = 1 agent. 같은 호스트 다중 agent 필요 시 별도 user 로 install.
#
# Mode 1 — fresh install (default):
#   cd /path/to/install
#   curl -k https://<CSC>:4419/install-agent.sh | bash -s -- \
#        --csc-url https://<CSC>:4419 \
#        --enrollment-token <token> \
#        --name <agent-name>
#   ./init.sh                    # sudoers + enroll + systemd unit + enable --now (sudo 비번 1회)
#
# Mode 2 — update (bundle 전체 교체 + sub-script 재생성, enrollment/sudoers/systemd 안 건드림):
#   bash install-agent.sh --update-only \
#        --csc-url https://<CSC>:4419 \
#        --name <agent-name> \
#        --install-dir /opt/cims-agent
#   (호출자가 systemctl --user restart cims-agent.service 책임 — agent self-exit + systemd 자동 재기동)

set -euo pipefail

CSC_URL=""
ENROLL_TOKEN=""
AGENT_NAME="$(hostname)"
MODE="fresh"
INSTALL_DIR_ARG=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --csc-url)           CSC_URL="$2"; shift 2 ;;
        --enrollment-token)  ENROLL_TOKEN="$2"; shift 2 ;;
        --name)              AGENT_NAME="$2"; shift 2 ;;
        --install-dir)       INSTALL_DIR_ARG="$2"; shift 2 ;;
        --update-only)       MODE="update"; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

if [[ "$MODE" == "fresh" ]]; then
    if [[ -z "$CSC_URL" || -z "$ENROLL_TOKEN" ]]; then
        echo "Usage (fresh): $0 --csc-url <URL> --enrollment-token <TOKEN> [--name <NAME>]"
        exit 1
    fi
else
    if [[ -z "$CSC_URL" ]]; then
        echo "Usage (update): $0 --update-only --csc-url <URL> --name <NAME> [--install-dir <DIR>]"
        exit 1
    fi
fi

if [[ $EUID -eq 0 ]]; then
    echo "ERROR: root 로 실행하지 마세요. 서비스 운영 계정으로 실행하세요."
    exit 1
fi

if [[ "$MODE" == "update" && -n "$INSTALL_DIR_ARG" ]]; then
    cd "$INSTALL_DIR_ARG"
fi
INSTALL_DIR="$(pwd)"
STATE_DIR="$INSTALL_DIR/state"
BIN_FILE="$INSTALL_DIR/agent/cims_agent.py"
SUDOERS_FILE="/etc/sudoers.d/cims-priv"

if [[ "$MODE" == "fresh" ]]; then
    echo "==> Installing CIMS Agent (fresh)"
else
    echo "==> Updating CIMS Agent (bundle + sub-script 재생성)"
fi
echo "    dir    : $INSTALL_DIR"
echo "    user   : $USER"
echo "    name   : $AGENT_NAME"

mkdir -p "$STATE_DIR"
chmod 700 "$STATE_DIR"

# agent bundle (tarball) — cims_agent.py + bin/{cims-priv,cims-ha,cims-svc,...} + lib/ + keepalived/ + systemd/
echo "==> Downloading agent bundle"
BUNDLE_TMP="$(mktemp /tmp/cims-agent-bundle.XXXXXX.tar.gz)"
trap 'rm -f "$BUNDLE_TMP"' EXIT
if ! curl -fsSLk "$CSC_URL/agent-bundle.tar.gz" -o "$BUNDLE_TMP"; then
    echo "ERROR: failed to download $CSC_URL/agent-bundle.tar.gz"
    exit 4
fi
if [[ "$MODE" == "fresh" ]]; then
    # fresh: 빈 디렉토리에 직접 풀기.
    if ! tar xzf "$BUNDLE_TMP" -C "$INSTALL_DIR" agent/ 2>/dev/null; then
        tar xzf "$BUNDLE_TMP" -C "$INSTALL_DIR"
    fi
else
    # update: agent.new/ 에 풀고 atomic rename — agent process 동작 중 race 차단.
    rm -rf "$INSTALL_DIR/agent.new" "$INSTALL_DIR/agent.old"
    mkdir -p "$INSTALL_DIR/agent.new"
    if ! tar xzf "$BUNDLE_TMP" -C "$INSTALL_DIR/agent.new" --strip-components=1 agent/ 2>/dev/null; then
        tar xzf "$BUNDLE_TMP" -C "$INSTALL_DIR/agent.new" --strip-components=1
    fi
    if [[ ! -f "$INSTALL_DIR/agent.new/cims_agent.py" ]]; then
        echo "ERROR: agent.new/cims_agent.py not found after extract"
        rm -rf "$INSTALL_DIR/agent.new"
        exit 5
    fi
    [[ -d "$INSTALL_DIR/agent" ]] && mv "$INSTALL_DIR/agent" "$INSTALL_DIR/agent.old"
    mv "$INSTALL_DIR/agent.new" "$INSTALL_DIR/agent"
    rm -rf "$INSTALL_DIR/agent.old"
fi
if [[ ! -f "$BIN_FILE" ]]; then
    echo "ERROR: tarball extracted but $BIN_FILE not found"
    exit 5
fi
chmod 755 "$BIN_FILE"
[[ -d "$INSTALL_DIR/agent/bin" ]] && chmod 755 "$INSTALL_DIR/agent/bin/"*

# ──────────────────────────────────────────────────────────────────────
# setup-sudoers.sh — root 권한으로 한 번 실행 (init.sh 가 자동 호출).
#   (1) /etc/sudoers.d/cims-priv — cims-priv / cims-ha NOPASSWD
#   (2) loginctl enable-linger $USER — host 재기동 시 systemd --user 자동 기동 보장
#   (3) 자동 검증 (cims-priv version / cims-ha 호출 + linger 상태)
# ──────────────────────────────────────────────────────────────────────
SETUP_SUDOERS="$INSTALL_DIR/setup-sudoers.sh"
cat > "$SETUP_SUDOERS" <<EOF
#!/usr/bin/env bash
# CIMS agent 의 ServiceIp / VIP 적용을 위해 cims-priv / cims-ha 를 NOPASSWD 로 sudo 실행 + linger 활성.
# Usage: sudo ./setup-sudoers.sh
set -euo pipefail
if [[ \$EUID -ne 0 ]]; then
    echo "ERROR: sudo 권한이 필요합니다 — 'sudo ./setup-sudoers.sh' 로 실행하세요" >&2
    exit 1
fi

# (1) sudoers
cat > $SUDOERS_FILE <<SUDO_EOF
$USER ALL=(root) NOPASSWD: $INSTALL_DIR/agent/bin/cims-priv *
$USER ALL=(root) NOPASSWD: $INSTALL_DIR/agent/bin/cims-ha *
SUDO_EOF
chmod 440 $SUDOERS_FILE
echo "✓ $SUDOERS_FILE 설치 완료"

# (2) linger — host 재기동 시 user systemd manager 자동 기동 보장
if loginctl show-user "$USER" 2>/dev/null | grep -q '^Linger=yes'; then
    echo "✓ linger 이미 활성"
else
    loginctl enable-linger "$USER"
    echo "✓ linger 활성화 — host 재기동 시 systemd --user 자동 기동"
fi

# (3) 자동 검증
fail=0
if runuser -u $USER -- sudo -n $INSTALL_DIR/agent/bin/cims-priv version >/dev/null 2>&1; then
    echo "✓ cims-priv NOPASSWD 동작 확인"
else
    echo "✗ cims-priv NOPASSWD 검증 실패"
    fail=1
fi
if runuser -u $USER -- sudo -n $INSTALL_DIR/agent/bin/cims-ha --help >/dev/null 2>&1 \\
   || runuser -u $USER -- sudo -n $INSTALL_DIR/agent/bin/cims-ha version >/dev/null 2>&1 \\
   || runuser -u $USER -- sudo -n $INSTALL_DIR/agent/bin/cims-ha 2>&1 | grep -qE "usage:|Usage:"; then
    echo "✓ cims-ha NOPASSWD 동작 확인"
else
    echo "✗ cims-ha NOPASSWD 검증 실패"
    fail=1
fi
if [[ \$fail -eq 0 ]]; then
    echo "✓ 모든 검증 통과 — agent 가 ServiceIp/VIP 적용 시 ip-add / ha apply 자동 수행 가능"
else
    echo "⚠ 일부 검증 실패 — sudoers 또는 wrapper 파일 위치 확인 필요"
    echo "    sudoers : $SUDOERS_FILE"
    echo "    wrapper : $INSTALL_DIR/agent/bin/cims-{priv,ha}"
    exit 2
fi
EOF
chmod 755 "$SETUP_SUDOERS"

# ──────────────────────────────────────────────────────────────────────
# init.sh — 한 번에: sudoers + enroll + systemd unit + enable --now.
#   사용자가 install_command 직후 1줄 실행. sudo 비번은 setup-sudoers.sh 호출 시 1회 prompt.
# ──────────────────────────────────────────────────────────────────────
INIT_SH="$INSTALL_DIR/init.sh"
cat > "$INIT_SH" <<EOF
#!/usr/bin/env bash
# 초기화 — sudoers + linger + enrollment + systemd unit + enable --now 한 번에.
# 이미 모두 끝난 항목은 NO-OP.
set -euo pipefail
cd "\$(dirname "\$0")"

# (1) sudoers + linger
if sudo -nl 2>/dev/null | grep -qF "\$(pwd)/agent/bin/cims-priv"; then
    echo "✓ sudoers 이미 등록됨 — skip"
else
    echo "==> sudoers + linger 등록 (sudo 비번 1회)"
    sudo "\$(pwd)/setup-sudoers.sh"
fi

# (2) enrollment — --enroll-only 로 state.json 생성
if [[ -f state/state.json ]]; then
    echo "✓ 이미 enroll 됨 — skip"
else
    echo "==> Running first-time enroll"
    CIMS_ENROLLMENT_TOKEN="$ENROLL_TOKEN" /usr/bin/python3 ./agent/cims_agent.py \\
        --csc-url "$CSC_URL" \\
        --state-dir "./state" \\
        --name "$AGENT_NAME" \\
        --enroll-only || true
    if [[ ! -f state/state.json ]]; then
        echo "✗ enroll 실패 — token 만료 또는 csc 도달성 확인" >&2
        exit 1
    fi
    echo "✓ enroll 완료 — state.json 생성됨"
fi

# (3) systemd --user unit 작성 + enable --now
export XDG_RUNTIME_DIR="\${XDG_RUNTIME_DIR:-/run/user/\$(id -u)}"
if ! command -v systemctl >/dev/null 2>&1 || ! systemctl --user show-environment >/dev/null 2>&1; then
    echo "✗ systemd --user 사용 불가 환경" >&2
    echo "  CIMS agent 정책: die 시 자동 재기동 + host 재기동 시 자동 기동 — systemd --user + linger 필수" >&2
    exit 1
fi

UNIT_DIR="\${XDG_CONFIG_HOME:-\$HOME/.config}/systemd/user"
UNIT_NAME="cims-agent.service"
UNIT_FILE="\$UNIT_DIR/\$UNIT_NAME"
mkdir -p "\$UNIT_DIR"

# 마이그레이션: 옛 동적 unit (cims-agent-<NAME>.service) 잔재 정리
for OLD in "\$UNIT_DIR"/cims-agent-*.service; do
    [[ -e "\$OLD" ]] || continue
    OLD_NAME="\$(basename "\$OLD")"
    echo "==> legacy unit 정리: \$OLD_NAME (단일 이름 정책 전환)"
    systemctl --user stop    "\$OLD_NAME" 2>/dev/null || true
    systemctl --user disable "\$OLD_NAME" 2>/dev/null || true
    rm -f "\$OLD" "\$UNIT_DIR/default.target.wants/\$OLD_NAME"
done

echo "==> Writing user systemd unit: \$UNIT_FILE"
cat > "\$UNIT_FILE" <<UNIT
[Unit]
Description=CIMS Server Agent (dir=$INSTALL_DIR)
After=network-online.target default.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$INSTALL_DIR
ExecStart=/usr/bin/python3 $BIN_FILE \\\\
    --csc-url $CSC_URL \\\\
    --state-dir $STATE_DIR \\\\
    --name $AGENT_NAME
Restart=always
RestartSec=10

[Install]
WantedBy=default.target
UNIT

systemctl --user daemon-reload
if systemctl --user is-active --quiet "\$UNIT_NAME"; then
    echo "==> agent 재기동 (새 설정 반영)"
    systemctl --user restart "\$UNIT_NAME"
else
    systemctl --user enable --now "\$UNIT_NAME"
fi

echo ""
echo "==> Status:"
systemctl --user --no-pager status "\$UNIT_NAME" || true
echo ""
echo "  로그   : journalctl --user -u \$UNIT_NAME -f"
echo "  제어   : systemctl --user {status|restart|stop} \$UNIT_NAME"
echo "  업데이트: $INSTALL_DIR/update.sh"
echo "  완전제거: $INSTALL_DIR/uninstall.sh"
echo ""
echo "==> 초기화 완료."
EOF
chmod 755 "$INIT_SH"

# ──────────────────────────────────────────────────────────────────────
# update.sh — agent bundle 갱신 + systemctl restart.
# ──────────────────────────────────────────────────────────────────────
UPDATE_SH="$INSTALL_DIR/update.sh"
cat > "$UPDATE_SH" <<EOF
#!/usr/bin/env bash
# 업데이트: csc 의 install-agent.sh --update-only 호출 + systemctl restart.
# state.json (enrollment), sudoers, systemd unit 은 유지 — bundle + sub-script 만 갱신.
# Usage:
#   ./update.sh                   — 확인 prompt 후 업데이트
#   ./update.sh --yes             — prompt skip
set -euo pipefail
cd "\$(dirname "\$0")"

force=0
[[ "\${1:-}" == "--yes" || "\${1:-}" == "-y" ]] && force=1

echo "==> CIMS agent 업데이트"
echo "  csc-url     : $CSC_URL"
echo "  install-dir : \$(pwd)"
echo "  agent name  : $AGENT_NAME"
echo ""
if [[ \$force -ne 1 ]]; then
    read -r -p "진행하시겠습니까? [y/N]: " ans
    case "\${ans,,}" in
        y|yes) ;;
        *) echo "취소됨"; exit 0 ;;
    esac
fi

INSTALLER_TMP="\$(mktemp /tmp/install-agent-update.XXXXXX.sh)"
trap 'rm -f "\$INSTALLER_TMP"' EXIT
echo "→ /install-agent.sh 다운로드"
curl -fsSLk "$CSC_URL/install-agent.sh" -o "\$INSTALLER_TMP"
bash "\$INSTALLER_TMP" --update-only \\
    --csc-url "$CSC_URL" \\
    --name "$AGENT_NAME" \\
    --install-dir "\$(pwd)"

export XDG_RUNTIME_DIR="\${XDG_RUNTIME_DIR:-/run/user/\$(id -u)}"
if systemctl --user is-active --quiet cims-agent.service; then
    echo "→ systemctl --user restart cims-agent.service"
    systemctl --user restart cims-agent.service
    echo "  ✓ 재기동 완료"
else
    echo "(agent 미실행 — 바이너리만 갱신, 기동: systemctl --user start cims-agent)"
fi
echo "✓ 업데이트 완료"
EOF
chmod 755 "$UPDATE_SH"

# ──────────────────────────────────────────────────────────────────────
# uninstall.sh — install 의 완전 대칭 (systemd unit + sudoers + linger + keepalived + 파일).
# ──────────────────────────────────────────────────────────────────────
UNINSTALL_SH="$INSTALL_DIR/uninstall.sh"
cat > "$UNINSTALL_SH" <<EOF
#!/usr/bin/env bash
# 완전 제거 — agent + 모듈(csp/cmp/...) 정지 + systemd unit + sudoers + keepalived + 파일 삭제.
# Usage:
#   ./uninstall.sh                 — 확인 prompt
#   ./uninstall.sh --yes           — 모든 prompt skip (모듈 같이 정리)
#   ./uninstall.sh --keep-modules  — 모듈은 남기고 agent 만 제거
set -euo pipefail
cd "\$(dirname "\$0")"

force=0
keep_modules=0
for arg in "\$@"; do
    case "\$arg" in
        --yes|-y) force=1 ;;
        --keep-modules) keep_modules=1 ;;
    esac
done

echo "이 작업은 CIMS agent (name='$AGENT_NAME') 를 완전히 제거합니다:"
echo "  • 실행 중인 agent process 종료 + user systemd unit 제거"
echo "  • state/, sub-scripts (init/update/uninstall/setup-sudoers), agent/ 삭제"
echo "  • /etc/sudoers.d/cims-priv 제거 (sudo 비번 1회)"
echo "  • cims-ha install 이 깐 keepalived 일괄 제거 (NOPASSWD cims-ha)"
if [[ \$keep_modules -ne 1 ]]; then
    echo "  • agent 가 띄운 모듈 (csp/cmp/cwrtc/csc/console/phone/cspsim) 도 정지 + modules/ 삭제"
fi
echo ""
if [[ \$force -ne 1 ]]; then
    read -r -p "정말 삭제하시겠습니까? [y/N]: " ans
    case "\${ans,,}" in
        y|yes) ;;
        *) echo "취소됨 — 아무것도 안 함"; exit 0 ;;
    esac
fi

# 1. 모듈 process 정지 (install_path scope)
if [[ \$keep_modules -ne 1 ]]; then
    SELF_PID=\$\$
    INSTALL_DIR_ABS="\$(pwd)"
    pids=\$( ( pgrep -af "\$INSTALL_DIR_ABS" 2>/dev/null \\
                | grep -vE "cims_agent\\.py|setup-sudoers\\.sh|init\\.sh|update\\.sh|uninstall\\.sh" \\
                | awk -v self=\$SELF_PID '\$1 != self {print \$1}' ) || true)
    if [[ -n "\$pids" ]]; then
        echo "→ install_path 안 모듈 process 정지:"
        for pid in \$pids; do
            cmd=\$(ps -p "\$pid" -o args= 2>/dev/null | head -c 80)
            echo "    pid=\$pid : \$cmd"
            kill "\$pid" 2>/dev/null || true
        done
        sleep 1
        for pid in \$pids; do
            kill -0 "\$pid" 2>/dev/null && kill -9 "\$pid" 2>/dev/null || true
        done
    else
        echo "→ 정지할 모듈 process 없음 (install_path scope)"
    fi
    if [[ -d ./modules ]]; then
        echo "→ modules/ 디렉토리 삭제"
        rm -rf modules
    fi
    rm -rf packages packages_trash 2>/dev/null || true
fi

# 2-a. user systemd unit 정지 + disable + 삭제 — linger 환경 자동 재기동 차단.
#      단일 이름 (cims-agent.service) + 옛 동적 unit (cims-agent-*.service) 잔재 모두 정리.
export XDG_RUNTIME_DIR="\${XDG_RUNTIME_DIR:-/run/user/\$(id -u)}"
UNIT_DIR="\${XDG_CONFIG_HOME:-\$HOME/.config}/systemd/user"
for OLD in "\$UNIT_DIR"/cims-agent.service "\$UNIT_DIR"/cims-agent-*.service; do
    [[ -e "\$OLD" ]] || continue
    UN="\$(basename "\$OLD")"
    echo "→ user systemd unit 정지 + 삭제 (\$UN)"
    systemctl --user stop    "\$UN" 2>/dev/null || true
    systemctl --user disable "\$UN" 2>/dev/null || true
    rm -f "\$OLD" "\$UNIT_DIR/default.target.wants/\$UN"
done
systemctl --user daemon-reload 2>/dev/null || true

# 2-b. agent process 종료 — systemd 정리 직후라 보통 이미 죽어있음.
if pgrep -f "cims_agent.py.*--name $AGENT_NAME" >/dev/null 2>&1; then
    PID=\$(pgrep -f "cims_agent.py.*--name $AGENT_NAME" | head -1)
    echo "→ agent 잔존 process 종료 (pid=\$PID)"
    kill "\$PID" 2>/dev/null || true
    sleep 1
    kill -0 "\$PID" 2>/dev/null && kill -9 "\$PID" 2>/dev/null || true
fi

# 3. cims-ha uninstall — install 대칭 (keepalived + autoremove deps purge).
if [[ -x ./agent/bin/cims-ha ]] && command -v keepalived >/dev/null 2>&1; then
    echo "→ cims-ha uninstall (keepalived + deps purge)"
    sudo -n ./agent/bin/cims-ha uninstall 2>&1 || \\
        echo "  ⚠ cims-ha uninstall 실패 — 수동 정리: sudo apt-get -y purge keepalived && sudo apt-get -y autoremove --purge"
fi

# 4. sudoers 제거 + linger 해제 안내 (자동 해제는 안 함 — 다른 user service 가능성).
if [[ -x ./agent/bin/cims-priv ]] && sudo -n ./agent/bin/cims-priv version >/dev/null 2>&1; then
    echo "→ /etc/sudoers.d/cims-priv 제거 (sudo 비번 필요)"
    sudo rm -f /etc/sudoers.d/cims-priv && echo "✓ sudoers 파일 삭제"
else
    echo "→ sudoers 파일 미등록 — skip"
fi
if loginctl show-user "\$USER" 2>/dev/null | grep -q '^Linger=yes'; then
    echo "  (참고) linger 가 켜져 있음 — 다른 user service 없으면 해제 권장:"
    echo "         sudo loginctl disable-linger \$USER"
fi

# 5. 잔재 파일 삭제 — sub-scripts + agent/ + state/.
#    옛 nohup 모드 잔재 (run.sh/start.sh/setup-systemd.sh/agent.log) 도 같이 정리.
rm -rf state agent.log run.sh start.sh setup-systemd.sh init.sh update.sh setup-sudoers.sh agent
echo "✓ state + sub-scripts + agent/ 디렉토리 삭제"

echo ""
echo "✓ agent '$AGENT_NAME' 완전 제거 완료"
[[ \$keep_modules -eq 1 ]] && echo "  (모듈은 그대로 — 정리 원하면 ./modules/, packages/ 수동 삭제)"
echo "  install dir (\$(pwd)) 자체 삭제 원하면: rmdir \$(pwd)"
EOF
chmod 755 "$UNINSTALL_SH"

if [[ "$MODE" == "fresh" ]]; then
    echo ""
    echo "════════════════════════════════════════════════════════════════════"
    echo "  ※ 다음 단계 — 1줄 실행"
    echo "════════════════════════════════════════════════════════════════════"
    echo "    $INSTALL_DIR/init.sh"
    echo ""
    echo "  (sudoers + linger 등록 → enrollment → systemd unit 작성 → enable --now)"
    echo "  sudo 비번을 1회 prompt 합니다."
    echo ""
    echo "  (참고) 업데이트  : $INSTALL_DIR/update.sh        (agent 바이너리만 갱신 + restart)"
    echo "  (참고) 완전 제거 : $INSTALL_DIR/uninstall.sh     (systemd unit + sudoers + keepalived + 파일 정리)"
    echo ""
    echo "==> 설치 완료."
else
    echo ""
    echo "==> 업데이트 완료 — bundle + sub-script 재생성."
    echo "    agent process 재기동은 호출자가 처리 (보통 agent self-exit + systemd 자동 재기동)."
fi
