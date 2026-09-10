#!/usr/bin/env bash
# =============================================================
# 30-packages.sh — 패키지 등록 (반입 tarball → OAM 패키지 저장소)
#
# 콘솔 `관리 > 시스템 > 패키지` 업로드와 같은 경로(POST /api/v1/packages)다.
# 원본 tarball 은 읽기만 한다 — 풀거나 고치지 않는다.
#
# oam·agent 는 등록하지 않는다 — 부트스트랩이 seed_packages 로 이미 자기등록해 둔다.
# oam 을 다시 등록·설치하면 자기 자신을 갈아치우려 든다(매뉴얼 §3).
#
# 멱등: 같은 (name, version) 은 force 로 같은 레코드를 갱신한다 (배포 overlay 보존).
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

tb_load_site
tb_init_dirs
tb_require_python

header "=== [30] 패키지 등록 ==="

tb_ask TB_OAM_URL    "OAM 주소" "https://127.0.0.1:4419"
tb_ask TB_ADMIN_PASS "콘솔 admin 비밀번호" "" secret

pkg_dir="$TB_OFFLINE_DIR/packages"
[[ -d "$pkg_dir" ]] || die_hint "반입 패키지 디렉토리 없음: $pkg_dir" \
    "빌드 장비에서: deployment/tb/tools/tb-pack.sh --with-packages"

# 등록 대상 — 서비스 모듈만. cmdp·cspsim 은 있으면 함께 올린다(없어도 정상).
shopt -s nullglob
targets=()
for name in oam-svc csc cmp csp cmdp cspsim; do
    for f in "$pkg_dir/$name"-[0-9]*.tar.gz; do targets+=("$f"); done
done
shopt -u nullglob
[[ ${#targets[@]} -gt 0 ]] || die_hint "등록할 서비스 모듈 tarball 이 없습니다 ($pkg_dir)" \
    "oam-svc·csc·cmp·csp 가 필요합니다 (cmdp·cspsim 은 선택)."

info "[1/2] 업로드 ${#targets[@]}개"
tb_tee 30-packages env TB_ADMIN_PASS="$TB_ADMIN_PASS" TB_STATE_DIR="$TB_STATE_DIR" \
    python3 "$_HERE/../lib/tb_oam.py" --url "$TB_OAM_URL" upload --force "${targets[@]}" \
    || die "패키지 업로드 실패 — 로그: $TB_LOG_DIR/30-packages.log"

info "[2/2] 저장소 확인"
if ! tb_tee 30-packages env TB_ADMIN_PASS="$TB_ADMIN_PASS" TB_STATE_DIR="$TB_STATE_DIR" \
        python3 "$_HERE/../lib/tb_oam.py" --url "$TB_OAM_URL" packages \
            --require oam-svc,csc,cmp,csp; then
    die "필수 모듈이 등록되지 않았습니다 — 반입본의 packages/ 를 확인하세요"
fi

ok "[30] 완료"
