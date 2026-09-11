#!/usr/bin/env bash
# =============================================================
# 60-start.sh — 순서 기동 + 상태 확인
#
# 콘솔 `[패키지 제어]` 탭과 같은 경로. 기동 순서에 이유가 있다:
#   oam-svc  먼저 — 모듈 자기보고 알람(FM ingest)의 수신자다. 없으면 초기 알람이 유실된다.
#   cmp      다음 — CSP 가 미디어 제어를 붙일 대상 (매뉴얼: CMP 를 CSP 보다 먼저)
#   csc      다음 — 가입자/MCPTT 서버
#   csp      마지막 — 단말 접속점
#
# 멱등: 이미 live_state=up 이면 건너뛴다 (start job 은 기동 중이면 409).
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

tb_load_site
tb_init_dirs
tb_require_python

header "=== [60] 기동 ==="

tb_ask TB_OAM_URL    "OAM 주소" "https://127.0.0.1:4419"
tb_ask TB_ADMIN_PASS "콘솔 admin 비밀번호" "" secret
tb_ask TB_START_ORDER "기동 순서" "oam-svc cmp csc csp"

# OAM 호출은 화면에 그대로 내보내면서 log/60-start.log 에도 남긴다 — 현장에서
# "무엇을 넣었고 무엇이 거부됐는지" 를 되짚을 기록이 이 단계에는 없었다.
oam() { tb_tee 60-start env TB_ADMIN_PASS="$TB_ADMIN_PASS" TB_STATE_DIR="$TB_STATE_DIR" \
        python3 "$_HERE/../lib/tb_oam.py" --url "$TB_OAM_URL" "$@"; }

n=0; total=$(echo $TB_START_ORDER | wc -w)
for m in $TB_START_ORDER; do
    n=$((n+1)); info "[$n/$total] $m 기동"
    oam ensure-running "$m"
done

info "상태 확인"
# 요구 대상은 기동 순서에 있는 모듈만이다 — cspsim 처럼 상주하지 않는 배포까지 요구하면
# 판정이 항상 실패한다 (70 단계가 바이너리를 직접 실행하므로 stopped 이 정상).
if ! oam status --require-up $TB_START_ORDER; then
    die_hint "일부 모듈이 running/up 이 아닙니다" \
        "CSP 가 죽어 있으면 local_nodes primary 누락이 가장 흔합니다 (45 단계)." \
        "로그는 두 곳이다 — 모듈이 제 로그를 한 단계 더 깊은 자리에 쓴다:" \
        "  <설치경로>/modules/<모듈>/current/log/           agent 가 받은 stdout (비어 있을 수 있다)" \
        "  <설치경로>/modules/<모듈>/current/<모듈>/log/     모듈 자기 로그 (<모듈>_<날짜>_N.log) ← 이쪽을 본다"
fi

ok "[60] 완료 — 모든 모듈 running/up"
