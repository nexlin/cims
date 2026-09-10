#!/usr/bin/env bash
# =============================================================
# 45-config.sh — 패키지 설정 (배포 overlay + CSP 컬렉션)
#
# 콘솔 `[패키지 설정]` 탭과 같은 경로다. 설치(40)와 분리해 둔 이유는 설정을 고쳐 다시
# 넣는 일이 훨씬 자주 생기기 때문이다 — 이 단계만 반복해서 돌릴 수 있다.
#
# **넣을 값은 이 스크립트에 없다.** `pkg_setting.cfg` 가 정본이고 이 스크립트는 그것을
# 읽어 OAM 에 넣는 일만 한다. 현장에서 값을 바꿀 때 스크립트를 고치게 하면 오타 한 번이
# 설치를 깨뜨리고 무엇을 바꿨는지도 남지 않는다.
#
# 설정은 두 갈래고 저장 위치가 다르다 (02_deployment.md §4):
#   ① scalar overlay → 배포 레코드 → update_config job 이 노드의 설정 파일에 렌더
#   ② collection     → 노드의 config/*.jsonl (CSP 는 SIGUSR1 로 즉시 반영)
# 어느 쪽도 패키지 안의 json 을 직접 고치지 않는다.
#
# 컬렉션을 overlay 보다 먼저 넣는다 — CSP 는 primary local_node 가 없으면 기동을
# 중단하므로, 기동(60)이 걸리기 전에 자리를 잡고 있어야 한다.
#
# 멱등: 값이 같으면 그대로 두고 update_config 만 다시 돌린다.
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

tb_load_site
tb_init_dirs
tb_require_python

header "=== [45] 패키지 설정 ==="

# 주소·포트·도메인·DB 접속은 **pkg_setting.cfg 가 정본**이라 여기서 묻지 않는다.
# 이 단계가 밖에서 받아야 하는 것은 넷뿐이다 — OAM 에 접속할 값 둘, 설치 경로,
# 그리고 cfg 에 적어둘 수 없는 DB 비밀번호(반입본 tar 에 실려 나가므로).
tb_ask TB_OAM_URL        "OAM 주소" "https://127.0.0.1:4419"
tb_ask TB_ADMIN_PASS     "콘솔 admin 비밀번호" "" secret
tb_ask TB_INSTALL_PREFIX "설치 경로" "/opt/cims-agent"
tb_ask TB_DB_APP_PASS    "DB 비밀번호" "" secret

# 파생값 — 설치 경로에서 유도된다. pkg_setting.cfg 가 ${STORE} 등으로 참조한다.
STORE="$TB_INSTALL_PREFIX/modules/oam/runtime"
SVCLOG="$STORE/service_log"

# TLS 인증서 — **사이트마다 따로 만든다.** 동봉본(CSP_CERT_BUNDLED)은 레포에 커밋된
# 전 배치 공용 키이고 SAN 도 없다. 버전 디렉토리 밖(관리 store)에 두어 패키지
# 업그레이드에 지워지지 않게 한다. 동봉본을 쓰려면 pkg_setting.cfg 의 tls_cert_path
# 를 ${CSP_CERT_BUNDLED} 로 바꾸면 된다.
CSP_CERT_BUNDLED="$TB_INSTALL_PREFIX/modules/csp/current/csp/cert/csp.pem"
CSP_CERT="$STORE/cert/csp-site.pem"

CFG="${TB_PKG_SETTING:-$TB_ROOT/pkg_setting.cfg}"
[[ -f "$CFG" ]] || die_hint "설정 파일이 없습니다: $CFG" \
    "반입본에 pkg_setting.cfg 가 함께 들어 있어야 합니다." \
    "다른 경로를 쓰려면 TB_PKG_SETTING=<경로> 로 지정하세요."

# OAM 호출은 화면에 그대로 내보내면서 log/45-config.log 에도 남긴다 — 현장에서
# "무엇을 넣었고 무엇이 거부됐는지" 를 되짚을 기록이 이 단계에는 없었다.
oam() { tb_tee 45-config env TB_ADMIN_PASS="$TB_ADMIN_PASS" TB_STATE_DIR="$TB_STATE_DIR" \
        python3 "$_HERE/../lib/tb_oam.py" --url "$TB_OAM_URL" "$@"; }

# ── 사이트 값·파생값을 파서에 넘긴다 ─────────────────────────
# 파일로 넘기는 이유: tb_load_site/tb_site_set 은 export 하지 않으므로 자식이 환경에서
# 읽을 수 없고(#12 와 같은 함정), 비밀번호를 argv 에 두면 `ps` 로 보인다.
vars_file="$TB_STATE_DIR/pkgset-vars.env"
umask 077
# cfg 는 보통 자기 [vars] 만 쓰지만, ${TB_*} 로 site.conf 값을 끌어다 쓸 수도 있게
# 저장돼 있는 값은 전부 넘긴다 (빈 값은 넘기지 않는다 — 미정의로 잡히게 두는 게 낫다).
{
    for k in TB_INSTALL_PREFIX TB_MGMT_IP TB_SIP_IP TB_MEDIA_IP \
             TB_DB_HOST TB_DB_PORT TB_DB_NAME TB_DB_APP_USER TB_DB_APP_PASS \
             TB_SIP_UDP_PORT TB_SIP_TCP_PORT TB_SIP_TLS_PORT \
             TB_PTT_DOMAIN TB_VOLTE_DOMAIN TB_PTT_SERVICE TB_VOLTE_SERVICE; do
        [[ -n "${!k:-}" ]] && printf '%s=%s\n' "$k" "${!k}"
    done
    printf 'STORE=%s\nSVCLOG=%s\nCSP_CERT=%s\nCSP_CERT_BUNDLED=%s\n' \
           "$STORE" "$SVCLOG" "$CSP_CERT" "$CSP_CERT_BUNDLED"
} > "$vars_file"
chmod 600 "$vars_file"

pkgset() { python3 "$_HERE/../lib/tb_pkgset.py" --file "$CFG" --vars "$vars_file" "$@"; }

info "설정 파일: $CFG"
pkgset check

# TLS 인증서 — SAN 에 넣을 주소는 **cfg 가 정본**이라 여기서 읽는다(45 는 주소를 묻지
# 않는다). 이미 있으면 만들지 않는다.
_san_ip="$(pkgset var SIP_IP 2>/dev/null || true)"
[[ -n "$_san_ip" ]] || _san_ip="${TB_SIP_IP:-}"
_svc_owner="$(stat -c %U "$TB_INSTALL_PREFIX" 2>/dev/null || echo root)"
tb_ensure_site_cert "$CSP_CERT" "$_san_ip" "$_svc_owner"

# ha1 결박 대조 — 18 단계는 site.conf 의 도메인으로 H(A1) 을 계산해 저장했다. cfg 의
# 도메인이 그것과 다르면 **등록이 401 로만 실패**해서 원인을 찾기 어렵다. 여기서 짚어준다.
# realm 도 같이 본다 — ha1 = MD5("<imsi>@<domain>:<realm>:<passwd>") 라 도메인과 realm
# 둘 다 결박 재료다. 18 단계는 realm 을 '-'(또는 빈 값)으로 두면 domain 을 쓰므로,
# 그 경우 cfg 의 realm 도 비어 있어야 같은 값이 된다.
_ha1_diff=0
for _pair in "PTT_DOMAIN:TB_PTT_DOMAIN" "VOLTE_DOMAIN:TB_VOLTE_DOMAIN" \
             "PTT_REALM:TB_PTT_REALM"   "VOLTE_REALM:TB_VOLTE_REALM"; do
    _cfg_name="${_pair%%:*}"; _site_name="${_pair##*:}"
    _site="${!_site_name:-}"
    # 18 단계의 '-' 는 "도메인과 동일" 이라는 뜻이다 → cfg 의 빈 값과 같은 의미로 본다.
    [[ "$_site" == "-" ]] && _site=""
    _cfg_val="$(pkgset var "$_cfg_name" 2>/dev/null || true)"
    # realm 은 양쪽이 비어 있는 것이 정상이라, 값이 없어도 대조를 건너뛰지 않는다.
    case "$_cfg_name" in
        *_REALM) [[ -n "${!_site_name+set}" ]] || continue ;;
        *)       [[ -n "$_site" ]] || continue ;;
    esac
    [[ "$_cfg_val" == "$_site" ]] && continue
    warn "$_cfg_name 이 18 단계의 ha1 결박 값과 다릅니다"
    warn "    pkg_setting.cfg = '${_cfg_val}'   /   tb-site.conf = '${_site}'"
    _ha1_diff=1
done
[[ $_ha1_diff -eq 1 ]] && {
    warn "ha1 = MD5(\"<imsi>@<도메인>:<realm>:<비번>\") 이라 어긋나면 **등록이 401 로만**"
    warn "실패합니다. 바꾼 값을 쓸 거면 18 단계부터 다시 돌려 ha1 을 재발급하세요:"
    warn "  sudo ./tb-install.sh --role db-data --reconfigure"
}

# ── ① 컬렉션 ─────────────────────────────────────────────────
mapfile -t _colls < <(pkgset collections)
n=0; total=$(( ${#_colls[@]} ))
for target in "${_colls[@]}"; do
    n=$((n+1))
    mod="${target%%/*}"; name="${target##*/}"
    info "[$n/$((total+1))] $mod 컬렉션 — $name"
    f="$TB_STATE_DIR/coll-$mod-$name.json"
    pkgset collection "$target" > "$f"
    oam collection "$mod" "$name" "$f"
done

# ── ② overlay ────────────────────────────────────────────────
mapfile -t _mods < <(pkgset modules)
info "[$((total+1))/$((total+1))] overlay — ${_mods[*]}"
for mod in "${_mods[@]}"; do
    args=()
    while IFS= read -r kv; do
        [[ -n "$kv" ]] && args+=(--set "$kv")
    done < <(pkgset overlay "$mod")
    [[ ${#args[@]} -gt 0 ]] || { warn "$mod: 넣을 값이 없습니다"; continue; }
    oam config "$mod" "${args[@]}"
done

header "[45] 완료"
cat <<EOF
  값을 바꾸려면 $CFG 를 고친 뒤 이 단계만 다시 돌리세요:
      sudo ./tb-install.sh --role config

  주의 — 저장 결과에 '미저장(템플릿에 없는 키)' 가 보이면 그 키는 해당 모듈
  config_template.json 에 선언이 없어 버려진 것입니다. 콘솔 [패키지 설정] 에서
  실제 반영값을 확인하세요.

  다음: 50(콘솔 승격) → 60(기동)
EOF
