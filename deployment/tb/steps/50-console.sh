#!/usr/bin/env bash
# =============================================================
# 50-console.sh — 서비스 메뉴 확인 (콘솔은 번들 하나, 재기동 없음)
#
# 콘솔 번들은 oam 패키지에 동봉된 **하나**다 — 코어 + 서비스 팩(가입자·서비스·통계) + 계측기
# 팩을 전부 담고 있다. 어느 팩의 메뉴가 보이는지는 번들이 아니라 **설치된 서비스**가 정한다:
# 서비스 모듈이 install 시 게이트웨이에 self-register 하면 `GET /api/v1/console/catalog` 의
# `installed_services` 에 나타나고, 콘솔 셸이 그 집합으로 nav 섹션(`requiresService`)을 켠다.
# 재로그인·oam 재기동 없이 다음 조회에서 메뉴가 나타난다.
#
# 이 단계는 그래서 **판정만** 한다 — oam-svc 가 등록돼 있지 않으면 40 단계(install) 가 끝나지
# 않은 것이므로 여기서 멈춘다(60 단계 기동 전에 원인이 보이게).
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

tb_load_site
tb_init_dirs
tb_require_python

header "=== [50] 서비스 메뉴 확인 ==="

tb_ask TB_OAM_URL       "OAM 주소" "https://127.0.0.1:4419"
tb_ask TB_ADMIN_PASS    "콘솔 admin 비밀번호" "" secret

# OAM 호출은 화면에 그대로 내보내면서 log/50-console.log 에도 남긴다.
oam() { tb_tee 50-console env TB_ADMIN_PASS="$TB_ADMIN_PASS" TB_STATE_DIR="$TB_STATE_DIR" \
        python3 "$_HERE/../lib/tb_oam.py" --url "$TB_OAM_URL" "$@"; }

info "[1/2] 서빙 중인 콘솔 번들 (oam 동봉본)"
echo "        $(oam console-bundle | tail -1)"

info "[2/2] 게이트웨이에 등록된 서비스 — 콘솔이 이 집합으로 서비스 메뉴를 켠다"
oam console-services --require oam-svc
ok "oam-svc 등록 확인 — 관리>구성 · 운용>서비스 · 운용>성능 메뉴가 콘솔에 보인다"
ok "[50] 완료"
