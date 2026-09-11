#!/usr/bin/env bash
# =============================================================
# tb-firewall.sh — 단말이 붙는 포트를 방화벽에 연다 (rhel 계열 전용 관문)
#
#   sudo ./tools/tb-firewall.sh            계획만 보여주고 묻는다
#   sudo ./tools/tb-firewall.sh --yes      묻지 않고 적용
#   sudo ./tools/tb-firewall.sh --dry-run  계획만 (적용 안 함)
#
# **숫자를 박아 넣지 않는다.** 설정이 바뀌면 이 스크립트를 다시 돌리기만 하면 되도록,
# 값은 매번 이 장비에서 읽는다:
#
#   SIP·OAM 포트  tb-site.conf → pkg_setting.cfg 기본값 순서로 찾는다
#   RTP 대역      배포된 cmp.json 의 시작포트 + 풀 크기로 **계산**한다
#
# 블록 크기(stride)만은 코드 상수라 여기 적는다 — cmp/PCmpServer.cpp·config_template.json
# 의 help 가 정본이다. 바뀌면 여기도 같이 고친다:
#   VoIP relay  호당 8 포트      (RtpPoolSize help)
#   PTT 멤버    멤버당 stride 2  (PttMemberPoolSize help — audio/video 각 1)
#   PTT floor   그룹당 stride 2  (PttRtpPoolSize help)
#   tap(감청)   tap 당 4 포트    (TapPoolSize help)
#
# **여는 것은 단말이 실제로 쓰는 포트뿐이다.** 내부 API·제어 채널(csc 4421, csp 9000,
# cmp 9001, DB 3306)은 열지 않는다 — 여는 목록이 곧 노출면이다. 목록에 없는데 외부
# 주소로 듣고 있는 포트가 있으면 **알려만 준다**(자동으로 열지 않는다).
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

# 이 도구는 lib/tb-common.sh 의 tb_firewall_allow 에 기댄다. 키트가 이 도구보다 옛 판이면
# **계획을 다 보여준 뒤 마지막 줄에서** "명령어를 찾을 수 없음" 으로 죽는다 — 그러면 원인이
# 안 보인다(2026-09-11 실측). 시작할 때 짚는다.
if ! declare -F tb_firewall_allow >/dev/null 2>&1; then
    die_hint "lib/tb-common.sh 에 tb_firewall_allow 가 없습니다 — 키트가 이 도구보다 옛 판입니다" \
        "둘은 같은 판이어야 합니다. 새 반입본을 풀거나 lib/tb-common.sh 만 같은 판으로 바꾸세요." \
        "확인: grep -c tb_firewall_allow $TB_ROOT/lib/tb-common.sh   (0 이면 옛 판)"
fi

YES=0; DRY=0
for a in "$@"; do
    case "$a" in
        --yes|-y)  YES=1 ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
        *) die "알 수 없는 인자: $a" ;;
    esac
done

tb_load_site

header "=== 단말 접속 포트 개방 ==="

if [[ "$(tb_pkg_family)" != "rhel" ]]; then
    ok "debian 계열 — ufw 는 기본 꺼짐이라 할 일이 없습니다"
    command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | head -1
    exit 0
fi

# ── 값 찾기 ───────────────────────────────────────────────────
# 사이트 값이 있으면 그것, 없으면 pkg_setting.cfg 의 기본값. 둘 다 없으면 규격 기본값.
_cfg_var() {   # $1=키  — pkg_setting.cfg 의 `KEY = VALUE` 한 줄
    local f="$TB_ROOT/pkg_setting.cfg" v=""
    [[ -f "$f" ]] || return 1
    v="$(awk -F= -v k="$1" '$1 ~ "^[ \t]*"k"[ \t]*$" {gsub(/[ \t]/,"",$2); print $2; exit}' "$f")"
    [[ -n "$v" ]] && echo "$v"
}
_pick() {      # $1=사이트변수명 $2=cfg키 $3=최후기본값
    local sv="${!1:-}"
    [[ -n "$sv" ]] && { echo "$sv"; return; }
    _cfg_var "$2" 2>/dev/null || echo "$3"
}

OAM_PORT="$(_pick TB_OAM_PORT OAM_PORT 4419)"
SIP_UDP="$(_pick TB_SIP_UDP_PORT SIP_UDP 5060)"
SIP_TCP="$(_pick TB_SIP_TCP_PORT SIP_TCP 25061)"
SIP_TLS="$(_pick TB_SIP_TLS_PORT SIP_TLS 5061)"
CSC_UE_PORT="${TB_CSC_UE_PORT:-4430}"     # 단말 대면 HTTPS (4-8 인증서와 짝)

# ── cmp.json 에서 미디어 대역 계산 ────────────────────────────
prefix="${TB_INSTALL_PREFIX:-/opt/cims-agent}"
cmp_json=""
for c in "$prefix/modules/cmp/current/cmp/config/cmp.json" \
         "$prefix/modules/cmp/current/config/cmp.json"; do
    [[ -f "$c" ]] && { cmp_json="$c"; break; }
done

media=()          # "설명|시작-끝/udp"
if [[ -n "$cmp_json" ]]; then
    info "미디어 대역 근거: $cmp_json"
    while IFS='|' read -r label spec; do
        [[ -n "$spec" ]] && media+=("$label|$spec")
    done < <(python3 - "$cmp_json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding='utf-8'))

def get(key, default):
    """중첩 dict 어디에 있든 key 를 찾는다 — cmp.json 의 형태가 판마다 조금씩 다르다."""
    stack = [d]
    while stack:
        o = stack.pop()
        if isinstance(o, dict):
            if key in o and isinstance(o[key], (int, str)) and str(o[key]).isdigit():
                return int(o[key])
            stack.extend(o.values())
        elif isinstance(o, list):
            stack.extend(o)
    return default

# (시작키, 풀크기키, 블록크기, 설명) — 블록 크기는 코드 상수(머리말 참조)
for start_k, pool_k, stride, label in (
        ('RtpStartPort',      'RtpPoolSize',        8, 'VoLTE/VoIP 미디어'),
        ('PttRtpStartPort',   'PttMemberPoolSize',  2, 'PTT 멤버 오디오'),
        ('PttVideoStartPort', 'PttMemberPoolSize',  2, 'PTT 멤버 영상'),
        ('PttFloorStartPort', 'PttRtpPoolSize',     2, 'PTT floor 제어'),
        ('TapStartPort',      'TapPoolSize',        4, '감청 tap')):
    start = get(start_k, 0)
    pool  = get(pool_k, 0)
    if start <= 0 or pool <= 0:
        continue                       # 0 = 그 기능 비활성 (TapPoolSize=0 등)
    end = start + pool * stride - 1
    print(f"{label} ({start_k} {start} + {pool_k} {pool} × {stride})|{start}-{end}/udp")
PY
    )
else
    warn "cmp.json 을 찾지 못했습니다 ($prefix/modules/cmp/...) — 미디어 대역을 계산할 수 없습니다"
    warn "  모듈 설치(40 단계) 뒤에 다시 돌리세요. 지금은 시그널링 포트만 엽니다."
fi

# ── 계획 ──────────────────────────────────────────────────────
plan=()
plan_note=()
_add() { plan+=("$2"); plan_note+=("$1"); }

_add "콘솔·API (이미 20 단계가 열었을 수 있다)" "$OAM_PORT/tcp"
_add "CSC 단말 대면 HTTPS (4-8 인증서와 짝 — 막히면 로그인부터 실패)" "$CSC_UE_PORT/tcp"
[[ "$SIP_UDP" != "0" ]] && _add "SIP UDP"  "$SIP_UDP/udp"
[[ "$SIP_TCP" != "0" ]] && _add "SIP TCP"  "$SIP_TCP/tcp"
[[ "$SIP_TLS" != "0" ]] && _add "SIP TLS"  "$SIP_TLS/tcp"
for m in "${media[@]+"${media[@]}"}"; do
    _add "${m%%|*}" "${m#*|}"
done

# 포트를 앞에 둔다 — 한글은 글자 폭이 바이트 수와 달라서 뒤에 두면 열이 어긋난다.
echo
printf '  %-18s %s\n' "포트" "용도"
printf '  %-18s %s\n' "------------------" "----"
for i in "${!plan[@]}"; do
    printf '  %-18s %s\n' "${plan[$i]}" "${plan_note[$i]}"
done
echo
info "열지 않는 것: csc 내부 API·csp 제어(9000)·cmp 제어(9001)·DB(3306) — 내부 채널"

# ── 목록에 없는데 외부로 듣고 있는 것 알림 ────────────────────
# 자동으로 열지 않는다. "듣고 있으니 연다" 로 하면 내부 API 까지 노출된다.
if command -v ss >/dev/null 2>&1; then
    # 계획에는 `50000-50159/udp` 같은 **범위**가 섞여 있다. 범위를 펴지 않고 문자열로
    # 맞추면 그 안의 포트가 전부 "목록에 없다"로 잡힌다.
    _in_plan() {
        local port="$1" spec lo hi
        for spec in "${plan[@]}"; do
            spec="${spec%%/*}"
            if [[ "$spec" == *-* ]]; then
                lo="${spec%%-*}"; hi="${spec##*-}"
                (( port >= lo && port <= hi )) && return 0
            elif [[ "$spec" == "$port" ]]; then
                return 0
            fi
        done
        return 1
    }
    extra=()
    while read -r p; do
        [[ "$p" =~ ^[0-9]+$ ]] || continue
        _in_plan "$p" || extra+=("$p")
    done < <(ss -tulnH 2>/dev/null \
             | awk '{print $5}' | grep -vE '^(127\.0\.0\.1|\[::1\])' \
             | sed 's/.*://' | sort -un)
    if [[ ${#extra[@]} -gt 0 ]]; then
        echo
        # 길면 앞쪽만 — 목록을 다 쏟아내면 읽지 않게 된다.
        _show=("${extra[@]:0:12}")
        _more=""
        [[ ${#extra[@]} -gt 12 ]] && _more=" … 외 $(( ${#extra[@]} - 12 ))개"
        warn "목록에 없는데 외부 주소로 듣고 있는 포트: ${_show[*]}$_more"
        warn "  내부 채널이면 그대로 두는 것이 맞습니다(그래서 자동으로 열지 않습니다)."
        warn "  단말이 써야 하는 포트가 섞여 있으면 알려 주세요 — 전체: ss -tulnH | grep -v 127.0.0.1"
    fi
fi

[[ $DRY -eq 1 ]] && { echo; info "--dry-run — 적용하지 않았습니다"; exit 0; }

if [[ $YES -ne 1 ]]; then
    echo
    read -r -p "  위 포트를 열까요? [y/N]: " _in
    [[ "$_in" == y* || "$_in" == Y* ]] || { echo "  중단"; exit 1; }
fi

tb_require_root
echo
tb_firewall_allow "${plan[@]}"

echo
info "현재 열린 포트"
firewall-cmd --list-ports | tr ' ' '\n' | sed 's/^/        /'
cat <<EOF

  바깥 장비에서 확인하는 것이 확실하다:
    curl -sk -o /dev/null -w '%{http_code}\\n' https://<이 서버 IP>:$OAM_PORT/
  단말을 붙일 거면 인증서 절차(TB-INSTALL 4-8)가 남아 있다 —
  포트가 열려도 CIMS Service CA 인증서가 없으면 로그인 단계에서 TLS 가 끊긴다.
EOF
