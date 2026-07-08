#!/usr/bin/env bash
# =============================================================
# scripts/lib/common.sh — 개발 스크립트 공용 라이브러리
#
# 사용처: cims.sh / configure.sh / cims-verify (source 로 로드).
# dist 에도 함께 복사된다 (cims.sh dist 단계) — dist 의 cims.sh/configure.sh 가
# 소스 트리 없이 단독 동작해야 하므로.
#
# agent/bin/cims-* 와 agent/lib/*.sh 는 의도적으로 이 파일을 쓰지 않는다 —
# 운영 도구는 모듈 tarball 로 단독 배포되는 자기완결 패키지다.
# =============================================================

# 중복 source 가드
[[ -n "${_CIMS_COMMON_SH_LOADED:-}" ]] && return 0
_CIMS_COMMON_SH_LOADED=1

# ── 색상 / 로그 (출력 규약) ────────────────────────────────────
# 규약 — 개발 스크립트(cims.sh/configure.sh/cims-verify)와 운영 엔진(agent/bin·lib,
# 자기완결이라 함수는 중복 정의하되 포맷은 동일)이 공유하는 출력 형식:
#   header "=== <섹션> ==="   섹션 구분 (빈 줄 + 굵게)
#   info/ok/warn/err          [INFO]/[OK]/[WARN]/[ERROR] 프리픽스 (err 는 stderr)
#   단계 표기                 파이프라인 단계는 "[1/3] build" 식으로 메시지에 명시
#   상세 출력                 빌드/설치 등 장문 출력은 $LOG_DIR/*.log 로 돌리고
#                             성공 시 요약 1줄 + 로그 경로, 실패 시에만 tail 노출
# ANSI-C 인용($'...') — echo -e 뿐 아니라 heredoc(cat <<EOF)에서도 렌더링되도록.
RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
CYAN=$'\033[0;36m'; BOLD=$'\033[1m'; NC=$'\033[0m'

info()    { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()      { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()     { echo -e "${RED}[ERROR]${NC} $*" >&2; }
header()  { echo -e "\n${BOLD}$*${NC}"; }

# ── .cims/server.local.json 리더 ──────────────────────────────
# cims_local_cfg_eval <cfg_file> [keys...]
#   저장 설정을 셸 변수 할당문으로 출력한다. eval 로 받아쓴다:
#     eval "$(cims_local_cfg_eval "$cfg" csp_ip db_host ...)"
#   출력 변수: _init_local_ip, _init_db_password (top-level),
#              _init_<key> ("configure" 객체의 각 키, 인자로 지정한 것만)
#   파일이 없거나 파싱 실패 시 아무것도 출력하지 않는다 (호출측 default 유지).
cims_local_cfg_eval() {
    local cfg="$1"; shift || true
    [[ -f "$cfg" ]] || return 0
    CFG="$cfg" KEYS="$*" python3 - 2>/dev/null <<'PY' || true
import json, os, shlex
try:
    d = json.load(open(os.environ["CFG"]))
except Exception:
    d = {}
c = d.get("configure") if isinstance(d.get("configure"), dict) else {}
print("_init_local_ip=%s" % shlex.quote(str(d.get("local_ip") or "")))
print("_init_db_password=%s" % shlex.quote(str(d.get("db_password") or "")))
for k in os.environ["KEYS"].split():
    print("_init_%s=%s" % (k, shlex.quote(str(c.get(k) or ""))))
PY
}

# ── pkg.json 버전 헬퍼 (cims.sh build / scripts/package.sh 공용) ──
_pkg_read_version() {
    local pkg="$1"
    [[ -z $pkg || ! -f $pkg ]] && { echo ""; return; }
    python3 - "$pkg" <<'PY' 2>/dev/null || echo ""
import sys, json
try:
    with open(sys.argv[1], 'r', encoding='utf-8') as f:
        d = json.load(f)
    print(d.get('version', '') if isinstance(d, dict) else '')
except Exception:
    print('')
PY
}

_pkg_bump_patch() {
    local ver="$1"
    [[ -z $ver ]] && ver="0.0.0"
    local major minor patch
    IFS='.' read -r major minor patch <<< "$ver"
    major="${major:-0}"; minor="${minor:-0}"; patch="${patch:-0}"
    # patch 에 숫자 아닌 것이 섞여 있으면 0 으로 리셋 (예: 1.0.0-rc1)
    [[ ! "$patch" =~ ^[0-9]+$ ]] && patch=0
    echo "${major}.${minor}.$((patch+1))"
}

_pkg_write_version() {
    local pkg="$1" new_ver="$2"
    [[ -z $pkg || ! -f $pkg || -z $new_ver ]] && return
    python3 - "$pkg" "$new_ver" <<'PY' 2>/dev/null
import sys, json
p, v = sys.argv[1], sys.argv[2]
try:
    with open(p, 'r', encoding='utf-8') as f:
        d = json.load(f)
except Exception:
    d = {}
if not isinstance(d, dict): d = {}
d['version'] = v
with open(p, 'w', encoding='utf-8') as f:
    json.dump(d, f, indent=2, ensure_ascii=False)
    f.write('\n')
PY
}

# (meta_file, explicit_ver, no_bump) → 실제 적용할 버전
_resolve_version() {
    local meta="$1" explicit="$2" nobump="$3"
    if [[ -n $explicit ]]; then echo "$explicit"; return; fi
    local cur; cur=$(_pkg_read_version "$meta")
    [[ -z $cur ]] && cur="0.0.0"
    if [[ "$nobump" == "1" ]]; then echo "$cur"; return; fi
    _pkg_bump_patch "$cur"
}

