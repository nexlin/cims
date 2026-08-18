#!/bin/bash
# agent/lib/pkgstate.sh — dpkg 패키지 상태·무결성 판정 + cims 내부 dpkg 직렬화
#
# 본 파일은 source 후 함수만 노출하는 library — standalone 실행 금지.
# cims-ha(HA 레인)와 cims-priv(agent 기동)가 **같은 패키지를 설치·판정**하므로 규칙이
# 갈라지면 한쪽만 고친 버그가 반대편에서 재발한다. 판정과 직렬화를 여기 하나로 둔다.

# ── 패키지 판정 3단 ───────────────────────────────────────────────────────
#   _pkg_status    dpkg 등록 상태 문자열
#   _pkg_ok        "install ok installed" — **등록** 여부만
#   _pkg_files_ok  dpkg -V 에 missing 없음 — 패키지 소유 파일이 실제로 존재
#   _pkg_healthy   위 둘 다
#
# 멱등 short-circuit 을 _pkg_ok 만으로 하면 안 된다: "등록은 됐는데 파일이 지워진"
# 상태를 영원히 복구하지 못한다(실측 사고 — /etc/keepalived 통삭제로 패키지 conffile
# keepalived.config-opts 가 사라졌는데 dpkg 상태는 계속 `ii` 라 재설치가 skip 됐고,
# keepalived 가 "Unable to read build config options file" 로 기동 불가였다).
_pkg_status()        { dpkg-query -W -f='${Status}' "$1" 2>/dev/null || echo "not-installed"; }
_pkg_ok()            { [[ "$(_pkg_status "$1")" == "install ok installed" ]]; }
# dpkg -V 는 두 가지를 함께 낸다:
#   missing     c /etc/foo      ← 파일 자체가 없음
#   ??5?????? c /etc/bar        ← 있지만 내용이 바뀜(체크섬 불일치)
# 우리가 고쳐야 하는 것은 **누락뿐**이다. 체크섬 불일치까지 "이상"으로 보면 운영자나
# 배포가 의도적으로 고친 conffile(예: /etc/default/nfs-common, /etc/default/keepalived)
# 이 매 기동 "미완비" 로 판정돼 재설치가 돌고, 그 수정이 --force-confnew 로 덮인다.
# 비용은 패키지당 수십 ms — 매 기동 판정에 넣어도 부담이 없다.
_pkg_files_missing() { dpkg -V "$1" 2>/dev/null | awk '$1 == "missing"'; }
_pkg_files_ok()      { [[ -z "$(_pkg_files_missing "$1")" ]]; }
_pkg_healthy()       { _pkg_ok "$1" && _pkg_files_ok "$1"; }

# ── cims 내부 dpkg 직렬화 ────────────────────────────────────────────────
# agent job worker 가 **레인 2개**(module/ha)로 병렬 실행되고, 여기에 agent 기동 시의
# cims-priv base-deps 설치까지 겹칠 수 있다. dpkg 는 동시 실행이 불가하므로 우리 쪽
# 호출끼리는 먼저 줄을 세운다 (외부 unattended-upgrade 와의 경합은 각 호출부의
# 재시도·backoff 가 담당). **락을 잡은 채로** 실행한다 — 대기만 하고 놓으면 무의미.
#
# 이 래퍼를 우회한 dpkg/apt 호출이 사고를 만든다: uninstall 경로가 `sudo dpkg -P` 를
# 직접 불러 install 레인과 경합 → purge 는 락에 막혀 실패했는데 뒤이은 파일 삭제만
# 성공해 패키지가 반쪽 상태로 남았다(실측). dpkg·apt 는 **반드시** 이 함수를 경유한다.
_CIMS_DPKG_LOCK="/var/lock/cims-dpkg.lock"

_cims_dpkg() {                     # _cims_dpkg <cmd...> — 락 보유 상태로 실행
    # root 로 실행 중이면 sudo 를 덧붙이지 않는다 — sudoers 허용목록은 cims-priv/cims-ha
    # 두 항목뿐이라 새 sudo 대상을 늘리지 않는 편이 안전하다.
    local pre=()
    [[ ${EUID:-$(id -u)} -eq 0 ]] || pre=(sudo)
    if command -v flock >/dev/null 2>&1; then
        "${pre[@]}" flock -w 300 "$_CIMS_DPKG_LOCK" "$@"
    else
        "${pre[@]}" "$@"
    fi
}

# ── 누락 파일 복구 ────────────────────────────────────────────────────────
# 패키지는 등록돼 있는데 소유 파일이 없는 상태의 정답은 **재설치가 아니라 conffile 복원**
# 이다. --force-confmiss 가 "관리자가 지운 conffile" 을 되돌린다 (dpkg 는 기본적으로
# 삭제를 관리자 의도로 보고 복원하지 않는다). 일반 파일은 재unpack 으로 함께 복구된다.
#   $1   = 패키지명
#   $2.. = 그 패키지를 담은 deb 파일들 (vendor 세트)
# 반환: 0=복구 성공(무결) / 1=복구 실패
_pkg_repair_from_deb() {
    local pkg="$1"; shift
    [[ $# -gt 0 ]] || return 1
    _cims_dpkg dpkg -i --force-confmiss --force-confnew --force-overwrite "$@" >/dev/null 2>&1 || true
    _pkg_healthy "$pkg"
}
