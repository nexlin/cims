#!/usr/bin/env bash
# =============================================================
# 40-install.sh — 모듈 설치 (배포 레코드 생성 + install job)
#
# 콘솔 `[패키지 설치]` 탭과 같은 경로다. **설치만** 한다 — 설정은 45, 기동은 60.
# 단계를 쪼갠 이유: 설정을 고쳐 다시 넣는 일이 설치보다 훨씬 자주 생긴다.
#
# TB 는 1대 구성이라 agent 는 하나뿐이고, 모듈 4개가 그 위에 올라간다.
# HA 그룹은 만들지 않는다 (ha 미사용).
#
# 프로세스명은 라이브 단일노드 구성과 동일하게 맞춘다 — CSP/CMP/CSC/OAM-SVC.
# 멱등: 같은 버전이 이미 설치돼 있으면 건너뛴다.
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

tb_load_site
tb_init_dirs
tb_require_python

header "=== [40] 모듈 설치 ==="

tb_ask TB_OAM_URL    "OAM 주소" "https://127.0.0.1:4419"
tb_ask TB_ADMIN_PASS "콘솔 admin 비밀번호" "" secret
# cspsim 은 **70 단계 호시험의 전제**다 — 기본에서 빠지면 all-in-one 으로 끝까지 돌려도
# 호시험이 `cspsim 을 찾을 수 없습니다` 로 반드시 막힌다(2026-09-10 실측). 30 단계는 이미
# 등록 대상에 넣고 있었는데 설치 목록에만 없었다. cmdp 는 기본 구성이 아니라 그대로 둔다.
tb_ask TB_MODULES    "설치할 모듈 (공백 구분)" "oam-svc cmp csc csp cspsim"

# OAM 호출은 화면에 그대로 내보내면서 log/40-install.log 에도 남긴다 — 현장에서
# "무엇을 넣었고 무엇이 거부됐는지" 를 되짚을 기록이 이 단계에는 없었다.
oam() { tb_tee 40-install env TB_ADMIN_PASS="$TB_ADMIN_PASS" TB_STATE_DIR="$TB_STATE_DIR" \
        python3 "$_HERE/../lib/tb_oam.py" --url "$TB_OAM_URL" "$@"; }

# 패키지명 → 프로세스명 (라이브 구성과 동일)
proc_of() {
    case "$1" in
        csp) echo CSP ;; cmp) echo CMP ;; csc) echo CSC ;;
        oam-svc) echo OAM-SVC ;; cmdp) echo CMDP ;; cspsim) echo CSPSIM ;;
        oam) die "oam 은 설치 대상이 아닙니다 — 부트스트랩이 소유합니다 (매뉴얼 §3)" ;;
        *) echo "${1^^}" ;;
    esac
}

n=0; total=$(echo $TB_MODULES | wc -w)
for m in $TB_MODULES; do
    n=$((n+1))
    info "[$n/$total] $m 설치"
    oam install "$m" "$(proc_of "$m")"
done

info "설치 상태"
oam status

ok "[40] 완료 — 다음: 45(설정)"
