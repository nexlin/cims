#!/usr/bin/env bash
# =============================================================
# 50-console.sh — 풀 콘솔 승격 (필요할 때만 oam 재기동)
#
# 왜 필요한가: 가입자·서비스·통계 메뉴는 **oam-svc 에 동봉된 콘솔**로 온다. OAM 은 정적
# 디렉토리를 기동 시 1회만 해석하므로, oam-svc 설치만으로는 그 번들이 서빙되지 않는다
# (매뉴얼 §3.1).
#
# 다만 패키징 방식에 따라 oam 동봉본이 이미 풀 콘솔일 수 있다. 그래서 **단정하지 않고**
# 서빙 중인 번들과 oam-svc 가 가진 번들을 비교해 다를 때만 재기동한다.
#
# 재기동하면 콘솔이 잠깐 끊긴다 — agent 감독이라 자동 복귀한다. job 완료 대기가 아니라
# HTTP 응답 복귀로 판정한다(자기 자신을 재기동하므로 API 가 잠시 끊긴다).
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

tb_load_site
tb_init_dirs
tb_require_python
tb_require_cmds curl

header "=== [50] 풀 콘솔 승격 ==="

tb_ask TB_OAM_URL       "OAM 주소" "https://127.0.0.1:4419"
tb_ask TB_ADMIN_PASS    "콘솔 admin 비밀번호" "" secret
tb_ask TB_INSTALL_PREFIX "설치 경로" "/opt/cims-agent"

# OAM 호출은 화면에 그대로 내보내면서 log/50-console.log 에도 남긴다 — 현장에서
# "무엇을 넣었고 무엇이 거부됐는지" 를 되짚을 기록이 이 단계에는 없었다.
oam() { tb_tee 50-console env TB_ADMIN_PASS="$TB_ADMIN_PASS" TB_STATE_DIR="$TB_STATE_DIR" \
        python3 "$_HERE/../lib/tb_oam.py" --url "$TB_OAM_URL" "$@"; }

info "[1/3] 서빙 중인 콘솔 번들"
served="$(oam console-bundle | tail -1)"
echo "        서빙: $served"

svc_dist="$TB_INSTALL_PREFIX/modules/oam-svc/current/oam-svc/console/dist/assets"
if [[ ! -d "$svc_dist" ]]; then
    warn "oam-svc 동봉 콘솔이 없습니다 ($svc_dist) — 승격할 것이 없습니다"
    ok "[50] 완료 (재기동 불필요)"; exit 0
fi
shopt -s nullglob
svc_js=("$svc_dist"/index-*.js)
shopt -u nullglob
[[ ${#svc_js[@]} -gt 0 ]] || { warn "oam-svc dist 에 index-*.js 가 없습니다"; ok "[50] 완료"; exit 0; }
want="assets/$(basename "${svc_js[0]}")"
echo "        oam-svc: $want"

if [[ "$served" == "$want" ]]; then
    ok "이미 oam-svc 콘솔이 서빙 중입니다 — 재기동 불필요"
    ok "[50] 완료"; exit 0
fi

info "[2/3] oam 재기동 (콘솔이 잠깐 끊깁니다)"
# 자기 자신을 재기동하므로 job 완료 응답을 못 받을 수 있다 — 큐잉만 하고 복귀를 기다린다.
oam job oam restart --timeout 60 || warn "job 응답이 끊겼습니다 (자기 재기동이라 정상) — 복귀를 기다립니다"

info "[3/3] 복귀 대기"
for i in $(seq 1 40); do
    code=$(curl -sk -o /dev/null -w '%{http_code}' "$TB_OAM_URL/" 2>/dev/null || echo 000)
    [[ "$code" =~ ^(200|301|302|401)$ ]] && break
    sleep 3
done
[[ "$code" =~ ^(200|301|302|401)$ ]] || die_hint "OAM 이 복귀하지 않았습니다 (HTTP $code)" \
    "agent 가 감독하므로 잠시 뒤 다시 확인하세요: $TB_OAM_URL"

served2="$(oam console-bundle | tail -1)"
if [[ "$served2" == "$want" ]]; then
    ok "승격 확인 — $served2"
else
    warn "재기동 후에도 번들이 다릅니다 (서빙 $served2 / oam-svc $want)"
    warn "Console.StaticDir 로 명시 지정이 필요한 구성일 수 있습니다 (매뉴얼 §3.1)"
fi
ok "[50] 완료"
