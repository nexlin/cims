#!/usr/bin/env bash
# =============================================================
# tb-agent-install-cmd.sh — agent 설치 명령을 OAM 에서 받아 찍는다
#
# 20 단계의 agent 설치가 실패했을 때(예: linger 직후 systemd --user 경합) 다시 깔기 위한
# 명령을 얻는다. install-agent.sh 는 **enrollment 토큰을 필수 인자로** 받는데, 그 토큰은
# OAM 의 agent 레코드에 있다 — 손으로 만들 수 없다.
#
#   sudo ./tools/tb-agent-install-cmd.sh            # 명령만 출력
#   sudo ./tools/tb-agent-install-cmd.sh --run      # 출력한 명령을 그대로 실행
#
# 토큰이 만료됐으면 재발행(--regen)이 필요하다. 재발행은 그 노드의 기존 세션을 무효화하지
# 않는다 — enroll 되지 않은 새 설치를 위한 값이다.
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

RUN=0; REGEN=0
for a in "$@"; do
    case "$a" in
        --run)   RUN=1 ;;
        --regen) REGEN=1 ;;
        *) die "알 수 없는 인자: $a  (--run | --regen)" ;;
    esac
done

tb_load_site
tb_ask TB_OAM_URL    "OAM 주소" "https://127.0.0.1:${TB_OAM_PORT:-4419}"
tb_ask TB_ADMIN_PASS "콘솔 admin 비밀번호" "" secret
tb_require_python

cmd="$(TB_OAM_URL="$TB_OAM_URL" TB_ADMIN_PASS="$TB_ADMIN_PASS" TB_LIB="$_HERE/../lib" \
       TB_REGEN="$REGEN" python3 - <<'PY'
import os, sys, json
sys.path.insert(0, os.environ['TB_LIB'])
import tb_oam

o = tb_oam.Oam(os.environ['TB_OAM_URL'], os.environ['TB_ADMIN_PASS'])
a = o.agent(os.environ.get('TB_AGENT_NAME') or None)
aid = a['id']

if os.environ.get('TB_REGEN') == '1':
    # 재발행은 enrollment_token 만 바꾼다 — agent_token(세션)·status·HA 소속은 보존되므로
    # 이미 붙어 있는 agent 를 끊지 않는다(OAM _regenerate_token 확인).
    # 기존 토큰이 아직 유효하면 409 로 거부한다 — 그건 "그걸 그대로 쓰라"는 뜻이라 정상이다.
    try:
        o.req('POST', f'/api/v1/agents/{aid}/regenerate-token', {})
        print('  토큰 재발행', file=sys.stderr)
    except SystemExit:
        print('  기존 토큰이 아직 유효합니다 — 그것을 그대로 씁니다', file=sys.stderr)

r = o.req('GET', f'/api/v1/agents/{aid}/install-command')
cmd = (r or {}).get('install_command')
if not cmd:
    why = (r or {}).get('install_command_error') or 'unknown'
    print(f"  토큰을 쓸 수 없습니다 ({why}) — --regen 으로 재발행하세요", file=sys.stderr)
    raise SystemExit(2)
print(cmd)
PY
)" || die "명령을 받지 못했습니다"

header "=== agent 설치 명령 ==="
echo "  $cmd"
echo

if [[ $RUN -eq 1 ]]; then
    tb_require_root
    info "실행합니다"
    # shellcheck disable=SC2086
    bash -c "$cmd"
else
    info "그대로 실행하려면 --run 을 붙이거나 위 줄을 복사해 sudo 로 실행하세요"
fi
