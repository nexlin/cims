#!/usr/bin/env bash
# =============================================================
# tb-pack.sh — USB 반입본 조립 (빌드 장비에서 실행)
#
#   ./tb-pack.sh                  DB 부트스트랩 자원만 (05·10·15 단계에 필요한 전부)
#   ./tb-pack.sh --with-packages  모듈 tarball(build/dist/packages)도 함께
#   ./tb-pack.sh --tar            조립 후 cims-tb-<날짜>.tar.gz 로 묶기
#
# 원본 불변: 레포 파일을 고치지 않고 offline/ 아래로 **복사**만 한다.
# db_bootstrap.py 는 자기 옆의 cims_schema.sql 과 vendor/pymysql 을 스스로 찾으므로
# (그 탐색 순서가 코드에 있다) 세 개를 같은 디렉토리에 모아두면 폐쇄망에서 그대로 돈다.
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

WITH_PKGS=0; MAKE_TAR=0
for a in "$@"; do
    case "$a" in
        --with-packages) WITH_PKGS=1 ;;
        --tar)           MAKE_TAR=1 ;;
        *) die "알 수 없는 인자: $a" ;;
    esac
done

[[ -n "$TB_REPO_ROOT" && -d "$TB_REPO_ROOT/deployment/db-bootstrap" ]] \
    || die "레포 안에서 실행하세요 (deployment/tb/tools/tb-pack.sh)"

header "=== TB 반입본 조립 ==="

# ── DB 부트스트랩 자원 ────────────────────────────────────────
info "[1/5] DB 부트스트랩 — db_bootstrap.py + cims_schema.sql + pymysql"
dbdir="$TB_OFFLINE_DIR/db-bootstrap"
mkdir -p "$dbdir/vendor"
cp -f "$TB_REPO_ROOT/deployment/db-bootstrap/db_bootstrap.py" "$dbdir/"
cp -f "$TB_REPO_ROOT/deployment/db-bootstrap/README.md"       "$dbdir/" 2>/dev/null || true
cp -f "$TB_REPO_ROOT/sql/cims_schema.sql"                     "$dbdir/"
# --cleanup 을 쓸 때만 필요하지만 24KB 라 같이 넣는다 (현장에서 없으면 곤란한 쪽).
cp -f "$TB_REPO_ROOT/sql/migrate_drop_unused_tables.sql"      "$dbdir/" 2>/dev/null || true
# 옛 스키마(dispatch_groups 계열)로 이미 깐 DB 를 이어 쓸 때만 필요하다. 새로 깔면 안 쓴다 —
# 없으면 현장에서 구할 방법이 없으니 함께 담는다 (TB-INSTALL 4-1 의 [주의]).
cp -f "$TB_REPO_ROOT/sql/migrate_phone_groups_roles.sql"      "$dbdir/" 2>/dev/null || true
rm -rf "$dbdir/vendor/pymysql"
cp -a "$TB_REPO_ROOT/ems/core/oam/vendor/pymysql"             "$dbdir/vendor/"
# 컴파일 캐시는 담지 않는다 — 대상 장비의 파이썬과 무관하고, 옛 바이트코드가 섞이면 혼란만 준다.
find "$dbdir" -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null || true
ok "$dbdir ($(du -sh "$dbdir" | awk '{print $1}'))"

# ── OS 패키지 ─────────────────────────────────────────────────
info "[2/5] OS 패키지(.deb) 확인"
if [[ -d "$TB_OFFLINE_DIR/debs" ]] && compgen -G "$TB_OFFLINE_DIR/debs/*/*.deb" >/dev/null; then
    for d in "$TB_OFFLINE_DIR"/debs/*/; do
        ok "$(basename "$d"): $(ls -1 "$d"/*.deb 2>/dev/null | wc -l)개"
    done
else
    warn "반입 .deb 가 없습니다 — 먼저 수집하세요: ./tb-fetch-debs.sh mariadb"
fi

# ── 모듈 tarball (선택) ───────────────────────────────────────
info "[3/5] 모듈 패키지"
if [[ $WITH_PKGS -eq 1 ]]; then
    src="$TB_REPO_ROOT/build/dist/packages"
    [[ -d "$src" ]] || die_hint "$src 없음" \
        "빌드 장비에서 먼저 만들어야 합니다:" \
        "  ./cims.sh build && cd build && make dist && cd .. && ./cims.sh pkg" \
        "C++ 변경분은 make dist 를 빼먹으면 옛 바이너리가 실립니다."
    mkdir -p "$TB_OFFLINE_DIR/packages"
    rm -f "$TB_OFFLINE_DIR/packages"/*.tar.gz

    # TB 에 필요한 것만 골라 담는다. 이유:
    #  · 변종 모듈(pmp·imp·psp·isp)은 배포본 전용이라 TB 구성에 쓰지 않는다.
    #  · oam·agent 단독 tarball 은 부트스트랩 인스톨러에 동봉돼 있어 중복이다.
    #  · 같은 모듈의 옛 버전이 남아 있으면 반입본만 커지고, 등록 단계가 옛 것도
    #    함께 올려 혼선이 된다 → 모듈별 **최신 1개**만.
    _latest() { ls -1 "$src"/$1-[0-9]*.tar.gz 2>/dev/null | sort -V | tail -1; }
    picked=()
    for name in cims-bootstrap csp cmp csc oam-svc cspsim cmdp; do
        f="$(_latest "$name")"
        [[ -n "$f" ]] && picked+=("$f")
    done
    [[ ${#picked[@]} -gt 0 ]] || die "$src 에 담을 tarball 이 없습니다"
    for f in "${picked[@]}"; do
        cp -f "$f" "$TB_OFFLINE_DIR/packages/"
        printf '   %-34s %s\n' "$(basename "$f")" "$(du -h "$f" | awk '{print $1}')"
    done
    # 필수 확인 — 하나라도 없으면 TB 현장에서 막힌다.
    for need in cims-bootstrap csp cmp csc oam-svc; do
        compgen -G "$TB_OFFLINE_DIR/packages/$need-*.tar.gz" >/dev/null \
            || die_hint "필수 패키지 누락: $need" "./cims.sh pkg $need 를 먼저 실행하세요"
    done
    ok "$TB_OFFLINE_DIR/packages (${#picked[@]}개, $(du -sh "$TB_OFFLINE_DIR/packages" | awk '{print $1}'))"
else
    info "건너뜀 (--with-packages 로 포함)"
fi

# ── 단말 대면 인증서 도구 ─────────────────────────────────────
# 단말은 CIMS Service CA 한 장만 신뢰한다 — 설치 직후의 CSC·CSP 인증서는 발급자가 달라
# 단말이 로그인 단계에서 끊긴다(TB-INSTALL 4-8). `collect` 로 그 노드가 요구하는 SAN 을
# 뽑는 일은 **대상 노드에서** 해야 하므로 이 파일을 함께 반입한다. 발급(issue)은 CA 보관
# 서버에서만 하고, 현장 배치는 거기서 만든 묶음(tgz)으로 한다 — 묶음에 같은 스크립트가
# 들어 있어 이 사본은 SAN 수집·검증용이다.
info "[4/5] 단말 대면 인증서 도구"
if [[ -f "$TB_REPO_ROOT/scripts/service-cert.sh" ]]; then
    mkdir -p "$TB_OFFLINE_DIR/tools"
    cp -f "$TB_REPO_ROOT/scripts/service-cert.sh" "$TB_OFFLINE_DIR/tools/service-cert.sh"
    chmod 755 "$TB_OFFLINE_DIR/tools/service-cert.sh"
    ok "$TB_OFFLINE_DIR/tools/service-cert.sh (SAN 수집·검증용)"
else
    warn "scripts/service-cert.sh 없음 — 단말 대면 인증서 절차(4-8)를 현장에서 쓸 수 없습니다"
fi

# ── 시험 음성 (70 단계 호시험) ────────────────────────────────
# cspsim 패키지에는 미디어가 없다. cspsim 은 이 디렉토리에서 `*_audio.amrwb` 를 모아
# **이름과 무관하게 단말 순번대로 돌려 쓴다**(CspsimMain.cpp: vecAudioFiles[i % 개수]).
# 그래서 필요한 것은 "단말 수만큼의 파일" 이고 파일명·가입 번호는 상관없다.
# 파일이 붙지 않은 세션은 합성음 PCMU(payload 0)로 떨어진다(SimSession.cpp).
info "[5/5] 시험 음성"
_msrc="$TB_REPO_ROOT/tests/media"
# 파일만 — 그 디렉토리에 __pycache__ 같은 하위 디렉토리가 섞여 있다.
if [[ -d "$_msrc" ]] && [[ -n "$(find "$_msrc" -maxdepth 1 -type f -print -quit)" ]]; then
    mkdir -p "$TB_OFFLINE_DIR/media"
    find "$_msrc" -maxdepth 1 -type f -exec cp -f {} "$TB_OFFLINE_DIR/media/" \;
    _na=$(find "$TB_OFFLINE_DIR/media" -maxdepth 1 -name '*_audio.amrwb' | wc -l)
    ok "$TB_OFFLINE_DIR/media ($(ls -1 "$TB_OFFLINE_DIR/media" | wc -l)개, $(du -sh "$TB_OFFLINE_DIR/media" | awk '{print $1}')) — 음성 ${_na}개"
    [[ "$_na" -eq 0 ]] && warn "*_audio.amrwb 가 없습니다 — 70 단계가 전부 PCMU(codec 0)로 떨어집니다"
else
    warn "$_msrc 없음 — 70 단계가 codec(0)=PCMU 로 떨어집니다 (합성음)"
fi

# ── 묶기 ──────────────────────────────────────────────────────
if [[ $MAKE_TAR -eq 1 ]]; then
    out="$TB_REPO_ROOT/build/dist/cims-tb-$(date +%Y%m%d).tar.gz"
    mkdir -p "$(dirname "$out")"
    # 사이트 고유값·임시 자격·로그는 반출하지 않는다.
    tar czf "$out" -C "$(dirname "$TB_ROOT")" \
        --exclude='tb/tb-site.conf' --exclude='tb/state' --exclude='tb/log' \
        --exclude='__pycache__' --exclude='*.pyc' \
        "$(basename "$TB_ROOT")"
    ok "$out ($(du -sh "$out" | awk '{print $1}'))"
    # 무결성 값 — 반입 경로가 FTP 면 ascii 모드 사고를 이것만이 잡는다(TB-INSTALL §2-b).
    # 옆에 .sha256 을 같이 남겨 두면 대상 장비에서 `sha256sum -c` 한 줄로 끝난다.
    ( cd "$(dirname "$out")" && sha256sum "$(basename "$out")" > "$(basename "$out").sha256" )
    ok "sha256: $(awk '{print $1}' "$out.sha256")"
    info "대조는 대상 장비에서: sha256sum -c $(basename "$out").sha256"
fi

header "=== 조립 완료 ==="
cat <<EOF
  옮길 것:       ${out:-$TB_ROOT}  (+ 같은 이름의 .sha256)
  옮기는 방법:   USB 복사 · scp · FTP(반드시 binary 모드)
  대상 장비에서: sha256sum -c <파일>.sha256  →  tar xzf  →  sudo ./tb-install.sh
EOF
