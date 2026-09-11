#!/usr/bin/env bash
# =============================================================
# tb-install.sh — TB(폐쇄망) 설치 진입점
#
# 이 서버에 무엇을 설치할지 골라서(또는 --role 로 지정해서) 해당 단계만 돌린다.
# 상용과 같은 분리 배치가 기본이고, 한 장비에 다 올리는 올인원도 고른다.
#
# 사이트마다 다른 값(IP·포트·비밀번호 등)은 tb-site.conf 한 파일에 모인다.
# 없으면 첫 실행 때 물어서 만들고, 두 번째부터는 묻지 않는다.
#
# 설계 원칙
#   - 반입한 원본 패키지(tarball·deb)와 레포 스크립트는 수정하지 않고 호출만 한다.
#   - 모듈 설정은 OAM 배포 오버레이/컬렉션 경로로만 넣는다 (패키지 안 json 직접 편집 금지).
#   - 모든 단계는 멱등 — 실패한 지점부터 다시 돌릴 수 있다.
# =============================================================
set -euo pipefail

TB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/tb-common.sh
source "$TB_ROOT/lib/tb-common.sh"

ROLE=""
FROM=""

usage() {
    cat <<EOF
${BOLD}CIMS TB 설치${NC}

  사용법: sudo ./tb-install.sh [--role <역할>] [옵션]

  역할 (생략하면 메뉴로 고른다) — TB 는 1대 구성(HA 미사용)이라 전부 이 서버에 올린다
    all-in-one    처음부터 끝까지                            (05→10→15→18→20→30→40→45→50→60)

    ── DB ──
    db            DB 설치 + 설정 + 스키마 + 초기데이터        (05 → 10 → 15 → 18)
    db-server     DB 서버 설치·설정만                        (05 → 10)
    db-schema     스키마/테이블 생성만                        (15)
    db-data       초기 데이터 입력만 (data/*.csv)             (18)

    ── 서비스 ──
    oam           관리 서버 부트스트랩 (OAM+콘솔+agent)       (20)
    packages      패키지 등록                                 (30)
    install       모듈 설치                                   (40)
    config        패키지 설정 (overlay + CSP 컬렉션)          (45)
    console       풀 콘솔 승격 (필요할 때만 oam 재기동)        (50)
    start         순서 기동 + 상태 확인                        (60)
    callcheck     호시험 — PTT 그룹호 / VoLTE 호 (매번 조건 질의)  (70)

  옵션
    --from <번호>   그 단계부터 이어서 (예: --from 10)
    --reconfigure   저장된 사이트 값을 다시 묻는다
    --batch         묻지 않는다 (tb-site.conf 에 값이 다 있어야 한다)
    --force-os      OS/파이썬 버전 불일치를 경고만 하고 진행 (권장하지 않음)
    --show          현재 사이트 설정 보기
    -h, --help      이 도움말

  실행 조건 (배포판 이름이 아니라 능력이 조건 — docs/design/features/os_portability.md)
    x86_64 · glibc ${TB_MIN_GLIBC} 이상 · CPython ${TB_REF_PYTHON}
    실측 통과: Ubuntu ${TB_REF_OS_VERSION} · Rocky Linux 10.2
    OS 패키지는 계열별로 갈린다 — debian=offline/debs · rhel=offline/rpms
    (rhel 용 rpm 은 같은 배포판 장비에서 tools/tb-fetch-rpms.sh 로 받는다)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --role)        ROLE="${2:-}"; shift 2 ;;
        --role=*)      ROLE="${1#*=}"; shift ;;
        --from)        FROM="${2:-}"; shift 2 ;;
        --from=*)      FROM="${1#*=}"; shift ;;
        --reconfigure) export TB_RECONFIGURE=1; shift ;;
        --batch)       export TB_BATCH=1; shift ;;
        --force-os)    export TB_FORCE_OS=1; shift ;;
        --show)        tb_load_site
                       [[ -f "$TB_SITE_CONF" ]] || die "사이트 설정이 아직 없습니다 ($TB_SITE_CONF)"
                       header "=== 사이트 설정 ($TB_SITE_CONF) ==="
                       # 비밀값은 가린다.
                       sed -E 's/^(TB_[A-Z_]*(PASS|SECRET|PASSWORD))=.*/\1=********/' "$TB_SITE_CONF"
                       exit 0 ;;
        -h|--help)     usage; exit 0 ;;
        *)             err "알 수 없는 인자: $1"; usage; exit 1 ;;
    esac
done

tb_load_site
tb_init_dirs

# ── 역할 선택 ─────────────────────────────────────────────────
if [[ -z "$ROLE" ]]; then
    [[ "${TB_BATCH:-0}" == "1" ]] && die "--batch 에는 --role 이 필요합니다"
    header "=== 이 서버에 무엇을 설치할까요? ==="
    cat <<EOF
   1) 처음부터 끝까지 (all-in-one)
   ── DB ──
   2) DB 설치+설정+스키마+데이터    3) DB 서버만    4) 스키마만    5) 데이터만
   ── 서비스 ──
   6) 관리 서버(OAM+콘솔)           7) 패키지 등록  8) 모듈 설치
   9) 패키지 설정                  10) 콘솔 승격   11) 기동
  12) 호시험 (PTT / VoLTE)
EOF
    read -r -p "  번호: " sel || die "입력이 끊겼습니다 — --role <역할> 로 지정하세요"
    case "$sel" in
        1) ROLE=all-in-one ;;
        2) ROLE=db ;;        3) ROLE=db-server ;;  4) ROLE=db-schema ;;  5) ROLE=db-data ;;
        6) ROLE=oam ;;       7) ROLE=packages ;;   8) ROLE=install ;;
        9) ROLE=config ;;   10) ROLE=console ;;   11) ROLE=start ;;
       12) ROLE=callcheck ;;
        *) die "잘못된 선택: $sel" ;;
    esac
fi

# ── 역할 → 단계 ───────────────────────────────────────────────
case "$ROLE" in
    all-in-one) STEPS=(05 10 15 18 20 30 40 45 50 60) ;;
    db)         STEPS=(05 10 15 18) ;;
    db-server)  STEPS=(05 10) ;;
    db-schema)  STEPS=(15) ;;
    db-data)    STEPS=(18) ;;
    oam)        STEPS=(20) ;;
    packages)   STEPS=(30) ;;
    install)    STEPS=(40) ;;
    config)     STEPS=(45) ;;
    console)    STEPS=(50) ;;
    start)      STEPS=(60) ;;
    callcheck)  STEPS=(70) ;;
    *) die "알 수 없는 역할: $ROLE (--help 로 목록 확인)" ;;
esac

# --from 으로 앞 단계 건너뛰기
if [[ -n "$FROM" ]]; then
    filtered=()
    for s in "${STEPS[@]}"; do [[ "$s" -ge "$FROM" ]] && filtered+=("$s"); done
    [[ ${#filtered[@]} -gt 0 ]] || die "--from $FROM 에 해당하는 단계가 역할 '$ROLE' 에 없습니다"
    STEPS=("${filtered[@]}")
fi

# 단계 스크립트는 `bash <script>` 로 **별도 프로세스**에서 돈다 — export 하지 않으면
# 그쪽의 `set -u` 가 "바인딩 해제한 변수" 로 죽는다. 단계가 실패 안내에
# "sudo ./tb-install.sh --role $ROLE --from NN" 을 찍으려면 이 값이 필요하다.
export ROLE

header "=== TB 설치 — 역할: $ROLE (단계: ${STEPS[*]}) ==="
info "사이트 설정: $TB_SITE_CONF"
info "로그: $TB_LOG_DIR"

# ── 실행 ──────────────────────────────────────────────────────
n=0
for s in "${STEPS[@]}"; do
    n=$((n + 1))
    script="$(echo "$TB_ROOT/steps/$s-"*.sh)"
    [[ -f "$script" ]] || die "단계 스크립트 없음: steps/$s-*.sh"
    info "── (${n}/${#STEPS[@]}) $(basename "$script")"
    if ! bash "$script"; then
        err "단계 $s 실패 — 고친 뒤 이어서 돌리세요:"
        echo "        sudo ./tb-install.sh --role $ROLE --from $s" >&2
        exit 1
    fi
done

header "=== 역할 '$ROLE' 완료 ==="
