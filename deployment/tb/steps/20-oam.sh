#!/usr/bin/env bash
# =============================================================
# 20-oam.sh — 관리 서버 부트스트랩 (OAM + 콘솔 + 로컬 agent)
#
# 반입한 cims-bootstrap-<버전>.tar.gz 의 install.sh 를 **그대로 호출**한다.
# 자동 배포 엔진은 자기 자신을 띄울 수 없으므로(닭-달걀) 이 단계만 부트스트랩 경로다
# — auto_deployment.md §1 의 "범위 밖" 이 여기다.
#
# TB 는 1대 구성이라 이 서버가 관리 서버이면서 서비스 노드다. install.sh 가 로컬 agent 를
# 설치·enroll 까지 하므로, 이후 단계는 그 agent 하나에 모듈을 얹는다.
#
# 멱등: 이미 OAM 이 응답하면 건너뛴다. 재설치는 매뉴얼 §7 의 철거 경로를 먼저 쓴다.
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

tb_load_site
tb_init_dirs

header "=== [20] 관리 서버 부트스트랩 (OAM + 콘솔 + agent) ==="

tb_require_arch
tb_require_os
tb_require_python314      # OAM·CSC 는 CPython 3.14 전용 확장을 쓴다
tb_require_cmds tar openssl curl
tb_require_sudo_caller

# ── 사이트 값 ─────────────────────────────────────────────────
tb_ask TB_MGMT_IP       "관리 IP (agent↔OAM 기준 · 인증서 SAN · 단말이 보는 주소)" \
    "$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)"
tb_ask TB_OAM_PORT      "OAM/콘솔 포트" "4419"
tb_ask TB_SERVICE_USER  "서비스 계정 (프로세스 소유자)" "cims"
tb_ask TB_INSTALL_PREFIX "설치 경로" "/opt/cims-agent"
tb_ask TB_ADMIN_PASS    "콘솔 admin 비밀번호" "" secret

TB_OAM_URL="https://${TB_MGMT_IP}:${TB_OAM_PORT}"
tb_site_set TB_OAM_URL "$TB_OAM_URL"

# ── 이미 떠 있으면 건너뛴다 ──────────────────────────────────
code=$(curl -sk -o /dev/null -w '%{http_code}' "https://127.0.0.1:${TB_OAM_PORT}/" 2>/dev/null || echo 000)
if [[ "$code" =~ ^(200|301|302|401)$ ]]; then
    ok "OAM 이 이미 응답합니다 (HTTP $code) — 부트스트랩 건너뜀"
    exit 0
fi

# ── 서비스 계정 확인 ─────────────────────────────────────────
# install.sh 가 `su - <계정>` 으로 전환하므로 로그인 셸과 home 이 필요하다.
if ! id "$TB_SERVICE_USER" >/dev/null 2>&1; then
    die_hint "서비스 계정 '$TB_SERVICE_USER' 가 없습니다" \
        "만들고 다시 실행하세요 (로그인 셸 + home 필요):" \
        "  sudo useradd -m -s /bin/bash $TB_SERVICE_USER"
fi
svc_home="$(getent passwd "$TB_SERVICE_USER" | cut -d: -f6)"
svc_shell="$(getent passwd "$TB_SERVICE_USER" | cut -d: -f7)"
[[ -d "$svc_home" ]] || die "서비스 계정 '$TB_SERVICE_USER' 의 home($svc_home)이 없습니다"
[[ "$svc_shell" =~ (nologin|false)$ ]] && die "서비스 계정 '$TB_SERVICE_USER' 에 로그인 셸이 없습니다 ($svc_shell)"
ok "서비스 계정: $TB_SERVICE_USER (home=$svc_home)"

# ── 부트스트랩 tarball ───────────────────────────────────────
shopt -s nullglob
bs=("$TB_OFFLINE_DIR"/packages/cims-bootstrap-*.tar.gz)
shopt -u nullglob
[[ ${#bs[@]} -gt 0 ]] || die_hint "부트스트랩 tarball 이 없습니다 ($TB_OFFLINE_DIR/packages/)" \
    "빌드 장비에서: ./cims.sh pkg && cd build && make dist" \
    "그 뒤: deployment/tb/tools/tb-pack.sh --with-packages"
# 여러 개면 최신 버전
bs_tar="$(printf '%s\n' "${bs[@]}" | sort -V | tail -1)"
info "[1/3] 전개 — $(basename "$bs_tar")"
stage="$TB_STATE_DIR/bootstrap"
rm -rf "$stage"; mkdir -p "$stage"
tar xzf "$bs_tar" -C "$stage"
inst="$stage/cims-bootstrap/install.sh"
[[ -x "$inst" ]] || die "install.sh 를 찾을 수 없습니다 ($inst)"

# ── 설치 ──────────────────────────────────────────────────────
# --batch: 문답 생략(자동화용). --admin-pass 는 argv 로만 받는 인자다 — 이 프로세스가
# 잠깐 ps 에 노출되지만 같은 값이 tb-site.conf(0600)에 이미 있고, 대안(대화식 입력)은
# 자동화를 깬다. 로그에는 남기지 않는다.
# 콘솔·API 는 브라우저와 다른 노드의 agent 가 붙는 포트다 — rhel 계열이면 여기서 연다.
# (우분투는 ufw 가 꺼져 있어 아무 일도 하지 않는다.)
tb_firewall_allow "${TB_OAM_PORT}/tcp"

info "[2/3] install.sh 실행 — mgmt=$TB_MGMT_IP port=$TB_OAM_PORT user=$TB_SERVICE_USER"
if ! "$inst" --batch \
        --prefix "$TB_INSTALL_PREFIX" \
        --port "$TB_OAM_PORT" \
        --mgmt-ip "$TB_MGMT_IP" \
        --user "$TB_SERVICE_USER" \
        --admin-pass "$TB_ADMIN_PASS" 2>&1 | tee "$TB_LOG_DIR/20-install.log"
then
    die_hint "부트스트랩 실패 — 로그: $TB_LOG_DIR/20-install.log" \
        "agent 설치 단계라면: $TB_INSTALL_PREFIX/modules/oam/current/log/agent_install.log" \
        "OAM 인계 단계라면:   $TB_INSTALL_PREFIX/modules/oam/current/log/oam_handover.log"
fi

# ── 확인 ──────────────────────────────────────────────────────
info "[3/3] 확인"
for i in $(seq 1 20); do
    code=$(curl -sk -o /dev/null -w '%{http_code}' "https://127.0.0.1:${TB_OAM_PORT}/" 2>/dev/null || echo 000)
    [[ "$code" =~ ^(200|301|302|401)$ ]] && break
    sleep 3
done
[[ "$code" =~ ^(200|301|302|401)$ ]] || die_hint "OAM 이 응답하지 않습니다 (HTTP $code)" \
    "로그: $TB_INSTALL_PREFIX/modules/oam/current/log/"
ok "OAM HTTP $code — $TB_OAM_URL"

procs=$(ps -eo user,args | grep -E 'oam_app\.py|cims_agent\.py' | grep -v grep || true)
[[ -n "$procs" ]] || die "oam_app / cims_agent 프로세스가 보이지 않습니다"
echo "$procs" | awk '{printf "        %s %s %s\n", $1, $2, $3}'

# agent 가 enroll 됐는지 — 이후 단계가 이 agent 에 모듈을 얹는다.
TB_ADMIN_PASS="$TB_ADMIN_PASS" TB_STATE_DIR="$TB_STATE_DIR" \
    python3 "$_HERE/../lib/tb_oam.py" --url "https://127.0.0.1:${TB_OAM_PORT}" status \
    || die "OAM API 조회 실패 — agent enroll 을 확인하세요"

# ── oam 자기배포(self-deploy) 확인 ────────────────────────────
# install.sh 는 **agent 가 등록된 뒤에** `oam` 배포 레코드를 스스로 만든다(self-deploy).
# agent 설치가 실패하면 그 단계가 `agent id 미확인 — skip` 으로 조용히 넘어가고, 결과는
# 한참 뒤 콘솔 모듈 목록에 `oam` 이 없는 것으로만 드러난다(2026-09-10 media02 실측).
# 레코드가 없으면 콘솔에서 oam 을 업그레이드·재기동할 수 없고, HA 는 관리 store 위치의
# 정본을 잃는다(관리평면 판정이 `oam` 배포를 본다). 서비스는 이미 돌고 있으므로 중단이
# 아니라 경고로 짚는다.
_chk_oam="$TB_STATE_DIR/.chk-oam-deploy.py"
cat > "$_chk_oam" <<'CHKEOF'
import os, sys
sys.path.insert(0, os.environ['TB_LIB'])
import tb_oam
o = tb_oam.Oam(os.environ['TB_OAM_URL'], os.environ['TB_ADMIN_PASS'])
print('Y' if o.deployment('oam') else '')
CHKEOF
_have_oam="$(TB_ADMIN_PASS="$TB_ADMIN_PASS" TB_STATE_DIR="$TB_STATE_DIR" \
    TB_OAM_URL="https://127.0.0.1:${TB_OAM_PORT}" TB_LIB="$_HERE/../lib" \
    python3 "$_chk_oam" 2>/dev/null || true)"
rm -f "$_chk_oam"
if [[ "$_have_oam" != "Y" ]]; then
    warn "oam 배포 레코드가 없습니다 — install.sh 의 self-deploy 가 건너뛰어졌습니다"
    warn "  OAM 프로세스는 돌고 있습니다. 레코드가 없으면 콘솔에서 oam 업그레이드·재기동을"
    warn "  할 수 없고, HA 구성 시 관리 store 위치의 정본이 없습니다."
    warn "  agent 설치가 실패했었다면 먼저 그것부터:"
    warn "    sudo ./tools/tb-agent-install-cmd.sh --regen --run"
    warn "  그 뒤 콘솔에서 채웁니다: 패키지 설치 → [+ 모듈 추가] → oam (설치 경로 $TB_INSTALL_PREFIX)"
else
    ok "oam 배포 레코드 확인"
fi

header "[20] 완료"
cat <<EOF
  콘솔: $TB_OAM_URL  (admin / tb-site.conf 의 TB_ADMIN_PASS)
  설치 경로: $TB_INSTALL_PREFIX
  관리 store: $TB_INSTALL_PREFIX/modules/oam/runtime

  다음: 30(패키지 등록) → 40(설치) → 45(설정) → 50(콘솔) → 60(기동)
EOF
