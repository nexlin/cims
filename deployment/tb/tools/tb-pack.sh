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
info "[1/4] DB 부트스트랩 — db_bootstrap.py + cims_schema.sql + pymysql"
dbdir="$TB_OFFLINE_DIR/db-bootstrap"
mkdir -p "$dbdir/vendor"
cp -f "$TB_REPO_ROOT/deployment/db-bootstrap/db_bootstrap.py" "$dbdir/"
cp -f "$TB_REPO_ROOT/deployment/db-bootstrap/README.md"       "$dbdir/" 2>/dev/null || true
cp -f "$TB_REPO_ROOT/sql/cims_schema.sql"                     "$dbdir/"
# --cleanup 을 쓸 때만 필요하지만 24KB 라 같이 넣는다 (현장에서 없으면 곤란한 쪽).
cp -f "$TB_REPO_ROOT/sql/migrate_drop_unused_tables.sql"      "$dbdir/" 2>/dev/null || true
rm -rf "$dbdir/vendor/pymysql"
cp -a "$TB_REPO_ROOT/ems/core/oam/vendor/pymysql"             "$dbdir/vendor/"
find "$dbdir/vendor" -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null || true
ok "$dbdir ($(du -sh "$dbdir" | awk '{print $1}'))"

# ── OS 패키지 ─────────────────────────────────────────────────
info "[2/4] OS 패키지(.deb) 확인"
if [[ -d "$TB_OFFLINE_DIR/debs" ]] && compgen -G "$TB_OFFLINE_DIR/debs/*/*.deb" >/dev/null; then
    for d in "$TB_OFFLINE_DIR"/debs/*/; do
        ok "$(basename "$d"): $(ls -1 "$d"/*.deb 2>/dev/null | wc -l)개"
    done
else
    warn "반입 .deb 가 없습니다 — 먼저 수집하세요: ./tb-fetch-debs.sh mariadb"
fi

# ── 모듈 tarball (선택) ───────────────────────────────────────
info "[3/4] 모듈 패키지"
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

# ── 시험 음성 (70 단계 호시험) ────────────────────────────────
# cspsim 패키지에는 미디어가 없다. 없으면 코덱이 PCMU(0)로 떨어져 호는 서지만 녹취
# 재생이 안 된다 — 설치 실패가 아니라 **시험 자료 결손**이라 경고만 하고 계속한다.
info "[4/4] 시험 음성"
_msrc="$TB_REPO_ROOT/tests/media"
# 파일만 — 그 디렉토리에 __pycache__ 같은 하위 디렉토리가 섞여 있다.
if [[ -d "$_msrc" ]] && [[ -n "$(find "$_msrc" -maxdepth 1 -type f -print -quit)" ]]; then
    mkdir -p "$TB_OFFLINE_DIR/media"
    find "$_msrc" -maxdepth 1 -type f -exec cp -f {} "$TB_OFFLINE_DIR/media/" \;
    ok "$TB_OFFLINE_DIR/media ($(ls -1 "$TB_OFFLINE_DIR/media" | wc -l)개, $(du -sh "$TB_OFFLINE_DIR/media" | awk '{print $1}'))"
    # 파일명이 **가입 번호와 대응**한다(8050001000004_audio.amrwb). 짝이 없는 번호는
    # 그 번호만 PCMU 로 떨어지므로, 현장에서 헤매지 않게 여기서 미리 알려 준다.
    _csv="$TB_ROOT/data/subscriptions.csv"
    if [[ -f "$_csv" ]]; then
        _miss=""
        while IFS=, read -r _num _login _kind _rest; do
            [[ -z "$_num" || "$_num" == \#* || "$_num" == "number" ]] && continue
            compgen -G "$TB_OFFLINE_DIR/media/${_num}_audio."* >/dev/null \
                || _miss+=" $_num($_kind)"
        done < "$_csv"
        [[ -n "$_miss" ]] && warn "음성 없는 번호:$_miss — 그 번호는 codec(0)=PCMU (VoLTE 는 예정된 범위)"
    fi
else
    warn "$_msrc 없음 — 70 단계가 codec(0)=PCMU 로 떨어집니다 (녹취 재생 불가)"
fi

# ── 묶기 ──────────────────────────────────────────────────────
if [[ $MAKE_TAR -eq 1 ]]; then
    out="$TB_REPO_ROOT/build/dist/cims-tb-$(date +%Y%m%d).tar.gz"
    mkdir -p "$(dirname "$out")"
    # 사이트 고유값·임시 자격·로그는 반출하지 않는다.
    tar czf "$out" -C "$(dirname "$TB_ROOT")" \
        --exclude='tb/tb-site.conf' --exclude='tb/state' --exclude='tb/log' \
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
