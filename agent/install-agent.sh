#!/usr/bin/env bash
# CIMS Agent 설치 스크립트 (사용자 권한, 현재 디렉토리 설치)
# 정책: systemd --user + linger 단일 운영 — die 시 자동 재기동, host 재기동 시 자동 기동.
#       1 user = 1 agent. 같은 호스트 다중 agent 필요 시 별도 user 로 install.
#
# Mode 1 — fresh install (일반 계정에서 sudo 필수 — root 직접 실행 금지):
#   curl -fsSLk https://<OAM>:4419/install-agent.sh -o install-agent.sh
#   sudo bash install-agent.sh --oam-url https://<OAM>:4419 \
#        --enrollment-token <token> --name <agent-name> \
#        [--install-dir /opt/cims-agent]   # 미지정 시 /opt/cims-agent
#   → sudoers + linger + enroll + systemd --user + enable --now 까지 한 번에 (init.sh 불필요).
#   (agent 자체는 서비스 계정의 systemd --user 로 동작 — sudo 호출자(SUDO_USER) 또는 --svc-user)
#
# Mode 2 — update (bundle 교체 + sub-script 재생성; 서비스 계정으로 실행, root 아님):
#   bash install-agent.sh --update-only \
#        --oam-url https://<OAM>:4419 --name <agent-name> --install-dir /opt/cims-agent
#   (호출자가 systemctl --user restart cims-agent.service 책임 — agent self-exit + systemd 자동 재기동)
#
# 호환성: --csc-url 도 동작 (deprecated alias). Phase 3b 이후 OAM 분리 — agent 는 OAM(4419) 과 통신.

set -euo pipefail

OAM_URL=""
ENROLL_TOKEN=""
AGENT_NAME="$(hostname)"
MODE="fresh"
INSTALL_DIR_ARG=""
SVC_USER_ARG=""
NO_SYSTEMD=0   # fresh 에서 systemd --user 단계 생략(호출자가 기동) — base install.sh --no-systemd 용

while [[ $# -gt 0 ]]; do
    case "$1" in
        --oam-url)           OAM_URL="$2"; shift 2 ;;
        --csc-url)           OAM_URL="$2"; shift 2 ;;   # deprecated alias (호환성)
        --enrollment-token)  ENROLL_TOKEN="$2"; shift 2 ;;
        --name)              AGENT_NAME="$2"; shift 2 ;;
        --install-dir)       INSTALL_DIR_ARG="$2"; shift 2 ;;
        --svc-user)          SVC_USER_ARG="$2"; shift 2 ;;   # fresh 서비스 계정 명시(미지정 시 SUDO_USER)
        --no-systemd)        NO_SYSTEMD=1; shift ;;          # fresh: systemd --user 생략(호출자 nohup 기동)
        --update-only)       MODE="update"; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

if [[ "$MODE" == "fresh" ]]; then
    if [[ -z "$OAM_URL" || -z "$ENROLL_TOKEN" ]]; then
        echo "Usage (fresh): $0 --oam-url <URL> --enrollment-token <TOKEN> [--name <NAME>]"
        exit 1
    fi
else
    if [[ -z "$OAM_URL" ]]; then
        echo "Usage (update): $0 --update-only --oam-url <URL> --name <NAME> [--install-dir <DIR>]"
        exit 1
    fi
fi

# ── 권한 + 서비스 계정 결정 (모드별 정책) ─────────────────────────────────
#   fresh = 설치: 일반 계정에서 sudo 필수(root 직접 금지) — base install.sh 와 동일 정책.
#   update = 자가업그레이드: 서비스 계정(non-root)으로 실행(파일 교체만, 권한작업 없음).
if [[ "$MODE" == "fresh" ]]; then
    if [[ $EUID -ne 0 ]]; then
        # 토큰 명령(curl|bash, 비root)으로 실행됨 → 설치(sudo 필요)는 하지 않는다.
        # install-agent.sh 를 현재 디렉터리에 내려받고 sudo 실행 명령만 안내한다.
        # (다운로드는 sudo 불필요 — "토큰 실행이 sudo 를 요구"하지 않게.)
        _DEST="./install-agent.sh"
        if curl -fsSLk "$OAM_URL/install-agent.sh" -o "$_DEST" 2>/dev/null && [[ -s "$_DEST" ]]; then
            chmod +x "$_DEST" 2>/dev/null || true
            # install.sh 래퍼 생성 — 등록 토큰/URL/이름을 박아, 설치는 'sudo ./install.sh' 1줄로.
            # (토큰을 다시 입력할 필요 없음 — 토큰 명령은 다운로드만, 설치만 sudo.)
            cat > ./install.sh <<WRAP
#!/usr/bin/env bash
# CIMS agent 설치 — 'sudo ./install.sh' 로 실행하세요 (등록 토큰/URL/이름 내장).
#   실행하면 설치 디렉터리를 묻습니다(엔터 시 /opt/cims-agent). 비대화 지정: --install-dir /경로
if [[ \$EUID -ne 0 ]]; then
    echo "ERROR: 'sudo ./install.sh' 로 실행하세요 (설치는 root 권한 필요)." >&2
    exit 1
fi
exec bash "\$(cd "\$(dirname "\$0")" && pwd)/install-agent.sh" \\
    --oam-url $(printf '%q' "$OAM_URL") \\
    --enrollment-token $(printf '%q' "$ENROLL_TOKEN") \\
    --name $(printf '%q' "$AGENT_NAME") "\$@"
WRAP
            chmod +x ./install.sh
            echo ""
            echo "✓ 다운로드 완료 ($(pwd)) — install-agent.sh + install.sh 생성"
            echo ""
            echo "  이제 설치는 1줄:   sudo ./install.sh"
            echo "  (설치 중 설치 디렉터리를 묻습니다 — 엔터 시 기본 /opt/cims-agent)"
            echo ""
            exit 0
        fi
        echo "ERROR: install-agent.sh 다운로드 실패 — $OAM_URL/install-agent.sh 확인" >&2
        exit 1
    fi
    SVC_USER="${SVC_USER_ARG:-${SUDO_USER:-}}"
    if [[ -z "$SVC_USER" || "$SVC_USER" == "root" ]]; then
        echo "ERROR: 서비스 계정을 알 수 없습니다(root 직접 실행?) — 일반 계정에서 sudo 로 실행하거나 '--svc-user <계정>' 을 지정하세요." >&2
        exit 1
    fi
    if ! id "$SVC_USER" >/dev/null 2>&1; then
        echo "ERROR: 서비스 계정이 존재하지 않습니다: $SVC_USER" >&2
        exit 1
    fi
else
    if [[ $EUID -eq 0 ]]; then
        echo "ERROR: --update-only 는 서비스 계정으로 실행하세요 (root 아님)." >&2
        exit 1
    fi
    SVC_USER="$(id -un)"
fi
SVC_GROUP="$(id -gn "$SVC_USER" 2>/dev/null || echo "$SVC_USER")"
SVC_UID="$(id -u "$SVC_USER")"
SVC_HOME="$(getent passwd "$SVC_USER" 2>/dev/null | cut -d: -f6 || true)"
SVC_HOME="${SVC_HOME:-/home/$SVC_USER}"

# ── 설치 디렉터리 ────────────────────────────────────────────────────────
#   fresh: --install-dir 또는 기본 /opt/cims-agent (root 라 어디든 생성+소유권 부여).
#   update: --install-dir 또는 현재 디렉터리(cwd).
if [[ "$MODE" == "fresh" ]]; then
    INSTALL_DIR="$INSTALL_DIR_ARG"
    if [[ -z "$INSTALL_DIR" ]]; then
        _def="/opt/cims-agent"
        if [[ -t 0 ]]; then
            # 대화형(sudo ./install.sh) — 설치 디렉터리를 묻는다(엔터 시 기본값).
            read -r -p "설치 디렉터리 [$_def]: " _in
            INSTALL_DIR="${_in:-$_def}"
        else
            INSTALL_DIR="$_def"   # 비대화형(파이프/자동화·base install.sh) — 기본값
        fi
    fi
    mkdir -p "$INSTALL_DIR" || { echo "ERROR: 설치 디렉터리 생성 실패: $INSTALL_DIR" >&2; exit 1; }
else
    [[ -n "$INSTALL_DIR_ARG" ]] && cd "$INSTALL_DIR_ARG"
    INSTALL_DIR="$(pwd)"
fi
cd "$INSTALL_DIR"
STATE_DIR="$INSTALL_DIR/state"
BIN_FILE="$INSTALL_DIR/agent/cims_agent.py"
SUDOERS_FILE="/etc/sudoers.d/cims-priv"

if [[ "$MODE" == "fresh" ]]; then
    echo "==> Installing CIMS Agent (fresh)"
else
    echo "==> Updating CIMS Agent (bundle + sub-script 재생성)"
fi
echo "    dir    : $INSTALL_DIR"
echo "    user   : $SVC_USER"
echo "    name   : $AGENT_NAME"

mkdir -p "$STATE_DIR"
chmod 700 "$STATE_DIR"

# agent bundle (tarball) — cims_agent.py + bin/{cims-priv,cims-ha,cims-svc,...} + lib/ + keepalived/ + systemd/
echo "==> Downloading agent bundle"
BUNDLE_TMP="$(mktemp /tmp/cims-agent-bundle.XXXXXX.tar.gz)"
trap 'rm -f "$BUNDLE_TMP"' EXIT
if ! curl -fsSLk "$OAM_URL/agent-bundle.tar.gz" -o "$BUNDLE_TMP"; then
    echo "ERROR: failed to download $OAM_URL/agent-bundle.tar.gz"
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
#   (2) loginctl enable-linger $SVC_USER — host 재기동 시 systemd --user 자동 기동 보장
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
$SVC_USER ALL=(root) NOPASSWD: $INSTALL_DIR/agent/bin/cims-priv *
$SVC_USER ALL=(root) NOPASSWD: $INSTALL_DIR/agent/bin/cims-ha *
SUDO_EOF
chmod 440 $SUDOERS_FILE
echo "✓ $SUDOERS_FILE 설치 완료"

# (2) linger — host 재기동 시 user systemd manager 자동 기동 보장
if loginctl show-user "$SVC_USER" 2>/dev/null | grep -q '^Linger=yes'; then
    echo "✓ linger 이미 활성"
else
    loginctl enable-linger "$SVC_USER"
    echo "✓ linger 활성화 — host 재기동 시 systemd --user 자동 기동"
fi

# (3) 자동 검증
fail=0
if runuser -u $SVC_USER -- sudo -n $INSTALL_DIR/agent/bin/cims-priv version >/dev/null 2>&1; then
    echo "✓ cims-priv NOPASSWD 동작 확인"
else
    echo "✗ cims-priv NOPASSWD 검증 실패"
    fail=1
fi
if runuser -u $SVC_USER -- sudo -n $INSTALL_DIR/agent/bin/cims-ha --help >/dev/null 2>&1 \\
   || runuser -u $SVC_USER -- sudo -n $INSTALL_DIR/agent/bin/cims-ha version >/dev/null 2>&1 \\
   || runuser -u $SVC_USER -- sudo -n $INSTALL_DIR/agent/bin/cims-ha 2>&1 | grep -qE "usage:|Usage:"; then
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
echo "  oam-url     : $OAM_URL"
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
curl -fsSLk "$OAM_URL/install-agent.sh" -o "\$INSTALLER_TMP"
bash "\$INSTALLER_TMP" --update-only \\
    --oam-url "$OAM_URL" \\
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
# Usage (root 권한 필수 — root 계정 또는 sudo):
#   sudo ./uninstall.sh                 — 확인 prompt
#   sudo ./uninstall.sh --yes           — 모든 prompt skip (모듈 같이 정리)
#   sudo ./uninstall.sh --keep-modules  — 모듈은 남기고 agent 만 제거
set -euo pipefail
cd "\$(dirname "\$0")"

# 권한 가드 — 제거는 root 권한 필수 (root 계정 또는 sudo). 일반 계정 직접 실행 거부.
#   (sudoers/keepalived 제거·파일 삭제 등 root 작업 → 권한 없이 부분 제거되는 것 방지)
if [[ \$EUID -ne 0 ]]; then
    echo "ERROR: 제거는 root 권한이 필요합니다 — 'sudo \$0 \$*' 또는 root 계정으로 실행하세요." >&2
    exit 1
fi

# 서비스 사용자(agent 소유자) 식별 — sudo 호출자 우선, 없으면 설치 디렉터리 소유자.
# systemd --user / linger 는 이 사용자 세션 기준이므로 root 가 아니라 이 사용자로 처리한다.
SVC_USER="\${SUDO_USER:-}"
if [[ -z "\$SVC_USER" || "\$SVC_USER" == "root" ]]; then
    SVC_USER="\$(stat -c %U . 2>/dev/null || echo root)"
fi
SVC_UID="\$(id -u "\$SVC_USER" 2>/dev/null || echo 0)"
SVC_HOME="\$(getent passwd "\$SVC_USER" 2>/dev/null | cut -d: -f6 || true)"
SVC_HOME="\${SVC_HOME:-/home/\$SVC_USER}"
# 서비스 사용자 컨텍스트에서 systemctl --user 실행 (root 에서 runuser 로 진입).
_user_systemctl() {
    runuser -u "\$SVC_USER" -- env XDG_RUNTIME_DIR="/run/user/\$SVC_UID" systemctl --user "\$@" 2>/dev/null || true
}

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
    # 보호 대상 = 자기 자신 + 모든 조상 PID (sudo / uninstall-base.sh 등 호출 트리 自害 방지).
    #   구버전은 self(uninstall.sh)만 제외 + grep 'uninstall\\.sh' 라 'uninstall-base.sh' 래퍼를
    #   매칭해 죽였고 → rm 도달 전 종료되어 /opt/cims-agent 가 남았음.
    _protect=" \$SELF_PID "
    _pp=\$SELF_PID
    while :; do
        _pp=\$(ps -o ppid= -p "\$_pp" 2>/dev/null | tr -d ' ')
        [[ -z "\$_pp" || "\$_pp" == "0" || "\$_pp" == "1" ]] && break
        _protect="\$_protect\$_pp "
    done
    INSTALL_DIR_ABS="\$(pwd)"
    pids=\$( ( pgrep -af "\$INSTALL_DIR_ABS" 2>/dev/null \\
                | grep -vE "cims_agent\\.py|setup-sudoers\\.sh|init\\.sh|update\\.sh|uninstall" \\
                | awk '{print \$1}' ) || true)
    _keep=""
    for _pid in \$pids; do
        case "\$_protect" in *" \$_pid "*) continue ;; esac
        _keep="\$_keep \$_pid"
    done
    pids="\$_keep"
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
#      root 로 실행되므로 서비스 사용자(\$SVC_USER) 세션 컨텍스트로 진입해 처리.
#      단일 이름 (cims-agent.service) + 옛 동적 unit (cims-agent-*.service) 잔재 모두 정리.
UNIT_DIR="\$SVC_HOME/.config/systemd/user"
for OLD in "\$UNIT_DIR"/cims-agent.service "\$UNIT_DIR"/cims-agent-*.service; do
    [[ -e "\$OLD" ]] || continue
    UN="\$(basename "\$OLD")"
    echo "→ user systemd unit 정지 + 삭제 (\$UN, user=\$SVC_USER)"
    _user_systemctl stop    "\$UN"
    _user_systemctl disable "\$UN"
    rm -f "\$OLD" "\$UNIT_DIR/default.target.wants/\$UN"
done
_user_systemctl daemon-reload

# 2-b. agent process 종료 — systemd 정리 직후라 보통 이미 죽어있음.
if pgrep -f "cims_agent.py.*--name $AGENT_NAME" >/dev/null 2>&1; then
    PID=\$(pgrep -f "cims_agent.py.*--name $AGENT_NAME" | head -1)
    echo "→ agent 잔존 process 종료 (pid=\$PID)"
    kill "\$PID" 2>/dev/null || true
    sleep 1
    kill -0 "\$PID" 2>/dev/null && kill -9 "\$PID" 2>/dev/null || true
fi

# 3. cims-ha uninstall — install 대칭 (keepalived + autoremove deps purge). (root 직접)
if [[ -x ./agent/bin/cims-ha ]] && command -v keepalived >/dev/null 2>&1; then
    echo "→ cims-ha uninstall (keepalived + deps purge)"
    ./agent/bin/cims-ha uninstall 2>&1 || \\
        echo "  ⚠ cims-ha uninstall 실패 — 수동 정리: apt-get -y purge keepalived && apt-get -y autoremove --purge"
fi

# 4. sudoers 제거 + linger 해제 안내 (자동 해제는 안 함 — 다른 user service 가능성). (root 직접)
if [[ -f /etc/sudoers.d/cims-priv ]]; then
    echo "→ /etc/sudoers.d/cims-priv 제거"
    rm -f /etc/sudoers.d/cims-priv && echo "✓ sudoers 파일 삭제"
else
    echo "→ sudoers 파일 미등록 — skip"
fi
if loginctl show-user "\$SVC_USER" 2>/dev/null | grep -q '^Linger=yes'; then
    echo "  (참고) linger(\$SVC_USER) 가 켜져 있음 — 다른 user service 없으면 해제 권장:"
    echo "         loginctl disable-linger \$SVC_USER"
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
    # 설치 디렉터리 소유권 → 서비스 계정 (root 로 풀었으므로). 생성된 sub-scripts·state/ 포함.
    chown -R "$SVC_USER":"$SVC_GROUP" "$INSTALL_DIR"

    # ── 구 init.sh 흡수 — sudoers + linger + enroll + systemd --user enable --now ──
    #    설치가 sudo(root)로 실행되므로 권한 작업은 직접, 사용자 세션 작업
    #    (enroll/systemd --user)은 runuser 로 서비스 계정 컨텍스트에서 수행.

    # (1) sudoers + linger (root 직접 — setup-sudoers.sh 가 sudoers/linger/검증 수행)
    echo "==> sudoers + linger 등록"
    bash "$SETUP_SUDOERS"

    # linger 직후 사용자 런타임 디렉터리(/run/user/UID)가 뜰 때까지 잠깐 대기
    XRD="/run/user/$SVC_UID"
    for _i in $(seq 1 20); do [[ -d "$XRD" ]] && break; sleep 0.3; done
    _user_sc() { runuser -u "$SVC_USER" -- env XDG_RUNTIME_DIR="$XRD" systemctl --user "$@"; }

    # (2) enrollment — 서비스 계정으로 state.json 생성
    if [[ -f "$STATE_DIR/state.json" ]]; then
        echo "✓ 이미 enroll 됨 — skip"
    else
        echo "==> first-time enroll (user=$SVC_USER)"
        runuser -u "$SVC_USER" -- env CIMS_ENROLLMENT_TOKEN="$ENROLL_TOKEN" \
            /usr/bin/python3 "$BIN_FILE" --oam-url "$OAM_URL" \
            --state-dir "$STATE_DIR" --name "$AGENT_NAME" --enroll-only || true
        if [[ ! -f "$STATE_DIR/state.json" ]]; then
            echo "✗ enroll 실패 — token 만료 또는 OAM 도달성 확인" >&2
            exit 1
        fi
        echo "✓ enroll 완료 — state.json 생성됨"
    fi

    # (3) systemd --user unit 작성 + enable --now (서비스 계정 세션)
    #     --no-systemd (base install.sh 의 systemd 미사용 환경) 시 enroll 까지만 하고 기동은 호출자.
    if [[ $NO_SYSTEMD -eq 1 ]]; then
        echo ""
        echo "✓ 설치 + enroll 완료 — systemd --user 생략(--no-systemd). 기동은 호출자(nohup)가 수행."
        echo "  완전제거: sudo $INSTALL_DIR/uninstall.sh   (root 권한 필요)"
        exit 0
    fi
    if ! command -v systemctl >/dev/null 2>&1 || ! _user_sc show-environment >/dev/null 2>&1; then
        echo "✗ systemd --user 사용 불가 (XDG_RUNTIME_DIR=$XRD) — linger/세션 확인 필요." >&2
        echo "  CIMS agent 정책: die 시 자동 재기동 + host 재기동 시 자동 기동 — systemd --user + linger 필수." >&2
        echo "  (systemd 없는 환경이면 --no-systemd 로 enroll 까지만 수행하고 nohup 으로 기동하세요.)" >&2
        exit 1
    fi
    USER_UNIT_DIR="$SVC_HOME/.config/systemd/user"
    runuser -u "$SVC_USER" -- mkdir -p "$USER_UNIT_DIR"   # 사용자 소유로 디렉터리 생성
    # 옛 동적 unit (cims-agent-*.service) 잔재 정리
    for OLD in "$USER_UNIT_DIR"/cims-agent-*.service; do
        [[ -e "$OLD" ]] || continue
        ON="$(basename "$OLD")"
        echo "==> legacy unit 정리: $ON"
        _user_sc stop "$ON" 2>/dev/null || true
        _user_sc disable "$ON" 2>/dev/null || true
        rm -f "$OLD" "$USER_UNIT_DIR/default.target.wants/$ON"
    done
    cat > "$USER_UNIT_DIR/cims-agent.service" <<UNIT
[Unit]
Description=CIMS Server Agent (dir=$INSTALL_DIR)
After=network-online.target default.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$INSTALL_DIR
ExecStart=/usr/bin/python3 $BIN_FILE --oam-url $OAM_URL --state-dir $STATE_DIR --name $AGENT_NAME
Restart=always
RestartSec=10

[Install]
WantedBy=default.target
UNIT
    chown "$SVC_USER":"$SVC_GROUP" "$USER_UNIT_DIR/cims-agent.service"
    _user_sc daemon-reload
    if _user_sc is-active --quiet cims-agent.service; then
        echo "==> agent 재기동 (새 설정 반영)"
        _user_sc restart cims-agent.service
    else
        _user_sc enable --now cims-agent.service
    fi

    echo ""
    echo "✓ 설치 완료 — agent 기동 (systemd --user, user=$SVC_USER, dir=$INSTALL_DIR)"
    echo "  상태 : sudo -u $SVC_USER XDG_RUNTIME_DIR=$XRD systemctl --user status cims-agent.service"
    echo "  로그 : sudo -u $SVC_USER XDG_RUNTIME_DIR=$XRD journalctl --user -u cims-agent.service -f"
    echo "  업데이트: $INSTALL_DIR/update.sh   (서비스 계정으로 실행)"
    echo "  완전제거: sudo $INSTALL_DIR/uninstall.sh   (root 권한 필요)"
else
    echo ""
    echo "==> 업데이트 완료 — bundle + sub-script 재생성."
    echo "    agent process 재기동은 호출자가 처리 (보통 agent self-exit + systemd 자동 재기동)."
fi
