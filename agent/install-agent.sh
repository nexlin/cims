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

# sudoers 등록은 사용자가 직접 1줄 실행 — install 끝부분에 안내 출력.
# (curl|bash 는 stdin 이 pipe 라 sudo 비번 prompt 처리 까다로움 → 분리)
SUDOERS_FILE="/etc/sudoers.d/cims-priv"
SUDOERS_NEED=1
if [[ -f "$SUDOERS_FILE" ]] && sudo -n cat "$SUDOERS_FILE" 2>/dev/null | grep -qF "$INSTALL_DIR/agent/bin/cims-priv"; then
    SUDOERS_NEED=0
fi

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
    # ────────────────────────────────────────────────────────────────────
    # systemd 미사용 모드 — 설치 / 초기화 / 실행 3 단계로 분리한 sub-script 생성
    # ────────────────────────────────────────────────────────────────────
    INIT_SH="$INSTALL_DIR/init.sh"
    START_SH="$INSTALL_DIR/start.sh"

    cat > "$INIT_SH" <<EOF
#!/usr/bin/env bash
# 2단계: 초기화 — sudoers 등록 + enrollment.
#   (1) /etc/sudoers.d/cims-priv 미등록 시 setup-sudoers.sh 자동 호출 (sudo 비번 prompt)
#   (2) enrollment_token 으로 CSC 와 enroll 교환 → state.json 생성
# 이미 enroll/등록 됐으면 NO-OP.
set -euo pipefail
cd "\$(dirname "\$0")"

# (1) sudoers — sudo -nl 로 sudoers 명단 직접 확인 (sudo timestamp cache 영향 없음)
if sudo -nl 2>/dev/null | grep -qF "\$(pwd)/agent/bin/cims-priv"; then
    echo "✓ sudoers 이미 등록됨 — skip"
else
    echo "==> sudoers 등록 (sudo 비번 1회)"
    sudo "\$(pwd)/setup-sudoers.sh"
fi

# (2) enrollment — --enroll-only 모드 (state.json 만 생성, heartbeat 안 보냄)
if [[ -f state/state.json ]]; then
    echo "✓ 이미 enroll 됨 (state/state.json 존재) — skip"
    exit 0
fi
echo "==> Running first-time enroll"
CIMS_ENROLLMENT_TOKEN="$ENROLL_TOKEN" /usr/bin/python3 ./agent/cims_agent.py \\
    --csc-url "$CSC_URL" \\
    --state-dir "./state" \\
    --name "$AGENT_NAME" \\
    --enroll-only || true
if [[ -f state/state.json ]]; then
    echo "✓ enroll 완료 — state.json 생성됨"
else
    echo "✗ enroll 실패 — token 만료 또는 csc 도달성 확인" >&2
    exit 1
fi
EOF
    chmod 755 "$INIT_SH"

    cat > "$START_SH" <<EOF
#!/usr/bin/env bash
# 3단계: 실행 — nohup 으로 agent 기동 + heartbeat 시작.
# CSC 가 첫 heartbeat 받으면 status 가 자동 online 으로 전환됨.
set -euo pipefail
cd "\$(dirname "\$0")"
if [[ ! -f state/state.json ]]; then
    echo "✗ state.json 없음 — 먼저 './init.sh' 로 enroll 하세요" >&2
    exit 1
fi
if pgrep -f "cims_agent.py.*--name $AGENT_NAME" >/dev/null 2>&1; then
    PID=\$(pgrep -f "cims_agent.py.*--name $AGENT_NAME" | head -1)
    echo "✓ agent 이미 실행 중 (pid=\$PID)"
    exit 0
fi
nohup ./run.sh > agent.log 2>&1 < /dev/null &
sleep 2
PID=\$(pgrep -f "cims_agent.py.*--name $AGENT_NAME" | head -1)
if [[ -n "\$PID" ]]; then
    echo "✓ agent 기동 (pid=\$PID, log=$INSTALL_DIR/agent.log)"
    echo "  → CSC heartbeat 도착 시 status 자동 online 전환"
    echo "  종료: kill \$PID"
else
    echo "✗ agent 기동 실패 — $INSTALL_DIR/agent.log 확인" >&2
    tail -5 "$INSTALL_DIR/agent.log" 2>&1 | sed 's/^/    /' >&2
    exit 1
fi
EOF
    chmod 755 "$START_SH"

    UNINSTALL_SH="$INSTALL_DIR/uninstall.sh"
    cat > "$UNINSTALL_SH" <<EOF
#!/usr/bin/env bash
# 4단계 (선택): 완전 제거 — agent + 모듈(csp/cmp/...) 정지 + 파일 삭제 + sudoers 제거.
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
echo "  • 실행 중인 agent process 종료"
echo "  • state/, agent.log, run.sh, init.sh, start.sh, setup-sudoers.sh, update.sh 삭제"
echo "  • agent/ 디렉토리 삭제 (cims_agent.py, bin/cims-priv, bin/cims-ha 등)"
echo "  • /etc/sudoers.d/cims-priv 제거 (sudo 비번 1회 필요)"
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

# 1. agent 가 이 install_path 에 띄운 모듈만 정지 + modules/ 삭제
#    (cims-svc stop all 은 시스템 전역 scope — dev/외부 process 영향 → 사용 안 함)
#    install_path 가 cmdline 에 포함된 process 만 식별해서 kill.
if [[ \$keep_modules -ne 1 ]]; then
    SELF_PID=\$\$
    INSTALL_DIR_ABS="\$(pwd)"
    # agent 자체 + 이 uninstall.sh 자신은 제외. pgrep / grep 매치 없을 때 exit 1 → pipefail 회피용 || true
    pids=\$( ( pgrep -af "\$INSTALL_DIR_ABS" 2>/dev/null \\
                | grep -vE "cims_agent\\.py|setup-sudoers\\.sh|init\\.sh|start\\.sh|update\\.sh|uninstall\\.sh|run\\.sh" \\
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
    # config overlay / packages 캐시도 같이
    rm -rf packages packages_trash 2>/dev/null || true
fi

# 2. agent process 종료
if pgrep -f "cims_agent.py.*--name $AGENT_NAME" >/dev/null 2>&1; then
    PID=\$(pgrep -f "cims_agent.py.*--name $AGENT_NAME" | head -1)
    echo "→ agent 종료 (pid=\$PID)"
    kill "\$PID" 2>/dev/null || true
    sleep 1
    if kill -0 "\$PID" 2>/dev/null; then
        kill -9 "\$PID" 2>/dev/null || true
    fi
fi

# 3. sudoers 파일 제거 — cims user 는 /etc/sudoers.d 디렉토리 접근 불가 (root 750) 라
#    파일 stat 이 항상 false. 대신 NOPASSWD 동작 여부로 등록 여부 판정 + sudo rm.
if [[ -x ./agent/bin/cims-priv ]] && sudo -n ./agent/bin/cims-priv version >/dev/null 2>&1; then
    echo "→ /etc/sudoers.d/cims-priv 제거 (sudo 비번 필요)"
    sudo rm -f /etc/sudoers.d/cims-priv && echo "✓ sudoers 파일 삭제"
else
    echo "→ sudoers 파일 미등록 (NOPASSWD 동작 안 함) — skip"
fi

# 4. install dir 안 모든 잔재 삭제 (sub-scripts + agent/)
rm -rf state agent.log run.sh init.sh start.sh setup-sudoers.sh update.sh agent
echo "✓ state + sub-scripts + agent/ 디렉토리 삭제"

echo ""
echo "✓ agent '$AGENT_NAME' 완전 제거 완료"
[[ \$keep_modules -eq 1 ]] && echo "  (모듈은 그대로 유지됨 — 정리 원하면 ./modules/, packages/ 수동 삭제)"
echo "  install dir (\$(pwd)) 자체 삭제 원하면: rmdir \$(pwd)"
EOF
    chmod 755 "$UNINSTALL_SH"

    UPDATE_SH="$INSTALL_DIR/update.sh"
    cat > "$UPDATE_SH" <<EOF
#!/usr/bin/env bash
# 업데이트: CSC 에서 최신 agent tarball 다시 받아서 풀고 agent restart.
# state.json (enrollment), sudoers, sub-scripts 는 유지 — agent 바이너리만 교체.
# Usage:
#   ./update.sh                   — 확인 prompt 후 업데이트
#   ./update.sh --yes             — prompt skip
set -euo pipefail
cd "\$(dirname "\$0")"

force=0
[[ "\${1:-}" == "--yes" || "\${1:-}" == "-y" ]] && force=1

CSC_URL="$CSC_URL"
echo "==> CIMS agent 업데이트"
echo "  csc-url     : \$CSC_URL"
echo "  install-dir : \$(pwd)"
echo "  agent name  : $AGENT_NAME"
echo ""
echo "  처리:"
echo "    1) /agent-bundle.tar.gz 다운로드"
echo "    2) agent/ 디렉토리 갱신 (cims_agent.py, bin/, lib/, keepalived/, systemd/)"
echo "    3) 실행 중이면 agent process restart (state.json 유지 → 재enroll 불필요)"
echo "    4) sub-scripts (run.sh/init.sh/start.sh/...) 는 변경 없음"
echo ""
if [[ \$force -ne 1 ]]; then
    read -r -p "진행하시겠습니까? [y/N]: " ans
    case "\${ans,,}" in
        y|yes) ;;
        *) echo "취소됨"; exit 0 ;;
    esac
fi

BUNDLE_TMP="\$(mktemp /tmp/cims-agent-bundle.XXXXXX.tar.gz)"
trap 'rm -f "\$BUNDLE_TMP"' EXIT
echo "→ /agent-bundle.tar.gz 다운로드"
curl -fsSLk "\$CSC_URL/agent-bundle.tar.gz" -o "\$BUNDLE_TMP"

# 새 agent/ 추출 (다른 파일 영향 없음)
echo "→ agent/ 디렉토리 갱신"
if ! tar xzf "\$BUNDLE_TMP" agent/ 2>/dev/null; then
    tar xzf "\$BUNDLE_TMP"
fi
chmod 755 agent/cims_agent.py
[[ -d agent/bin ]] && chmod 755 agent/bin/*

# 실행 중이면 restart
if pgrep -f "cims_agent.py.*--name $AGENT_NAME" >/dev/null 2>&1; then
    PID=\$(pgrep -f "cims_agent.py.*--name $AGENT_NAME" | head -1)
    echo "→ agent restart (pid=\$PID → SIGTERM → start.sh)"
    kill "\$PID"
    sleep 2
    if [[ -x ./start.sh ]]; then
        ./start.sh
    else
        nohup ./run.sh > agent.log 2>&1 < /dev/null &
        sleep 1
        NEW_PID=\$(pgrep -f "cims_agent.py.*--name $AGENT_NAME" | head -1)
        [[ -n "\$NEW_PID" ]] && echo "  ✓ 재기동 (pid=\$NEW_PID)" || echo "  ⚠ 재기동 확인 실패"
    fi
else
    echo "(agent 미실행 — 바이너리만 갱신, 재기동 필요 시 ./start.sh)"
fi
echo "✓ 업데이트 완료"
EOF
    chmod 755 "$UPDATE_SH"
fi

SETUP_SUDOERS="$INSTALL_DIR/setup-sudoers.sh"
cat > "$SETUP_SUDOERS" <<EOF
#!/usr/bin/env bash
# CIMS agent 의 ServiceIp / VIP 적용을 위해 cims-priv / cims-ha 를 NOPASSWD 로 sudo 실행할 수 있도록 sudoers 등록 + 자동 검증.
# Usage: sudo ./setup-sudoers.sh
set -euo pipefail
if [[ \$EUID -ne 0 ]]; then
    echo "ERROR: sudo 권한이 필요합니다 — 'sudo ./setup-sudoers.sh' 로 실행하세요" >&2
    exit 1
fi
cat > $SUDOERS_FILE <<SUDO_EOF
$USER ALL=(root) NOPASSWD: $INSTALL_DIR/agent/bin/cims-priv *
$USER ALL=(root) NOPASSWD: $INSTALL_DIR/agent/bin/cims-ha *
SUDO_EOF
chmod 440 $SUDOERS_FILE
echo "✓ $SUDOERS_FILE 설치 완료"

# 자동 검증 — cims user 로 전환해서 'sudo -n cims-priv version' 호출이 비번 없이 동작하는지 확인.
# (setup-sudoers.sh 자체는 root 로 실행되므로 runuser 로 cims user 권한으로 떨어뜨려 검증)
fail=0
if runuser -u $USER -- sudo -n $INSTALL_DIR/agent/bin/cims-priv version >/dev/null 2>&1; then
    echo "✓ cims-priv NOPASSWD 동작 확인 (cims → sudo → cims-priv version)"
else
    echo "✗ cims-priv NOPASSWD 검증 실패"
    fail=1
fi
if runuser -u $USER -- sudo -n $INSTALL_DIR/agent/bin/cims-ha --help >/dev/null 2>&1 \
   || runuser -u $USER -- sudo -n $INSTALL_DIR/agent/bin/cims-ha version >/dev/null 2>&1 \
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

echo ""
echo "════════════════════════════════════════════════════════════════════"
echo "  ※ 다음 단계 — 순서대로 실행해주세요"
echo "════════════════════════════════════════════════════════════════════"
if [[ "$USE_SYSTEMD" != "yes" ]]; then
    echo "  1. 초기화 (sudoers 등록 + enrollment, sudo 비번 1회):"
    echo "       $INSTALL_DIR/init.sh"
    echo "  2. 실행 (agent 기동 + heartbeat 시작 → 자동 online 전환):"
    echo "       $INSTALL_DIR/start.sh"
    echo ""
    echo "  (참고) 업데이트 :  $INSTALL_DIR/update.sh        (agent 바이너리만 갱신 + restart)"
    echo "  (참고) 완전 제거:  $INSTALL_DIR/uninstall.sh    (모듈 정지 + 파일 삭제 + sudoers 제거)"
fi
echo ""
echo "==> 설치 완료 (Done)."
