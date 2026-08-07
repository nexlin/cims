#!/bin/bash
# agent/lib/ha.sh — CIMS HA (keepalived + systemd unit) library
#
# 본 파일은 source 후 함수만 노출하는 library — standalone 실행 금지.
# Caller (agent/bin/cims-ha) 가 아래 환경변수와 helpers 를 미리 정의해야 함:
#   변수:    HA_DIR, HA_OUT, HA_JSON, HA_UNIT_DIR
#   logger:  info(), ok(), warn(), err(), header()
#
# B 통합:
#   - keepalived 는 단일 template (`keepalived.conf.tpl`) 의 vrrp_instance 블록을
#     Python rendering 에서 services 반복.
#   - systemd 는 단일 instantiated unit (`cims@.service.tpl`) — %i 가 svc slug.
#   - 신규 서비스 추가 = ha.json 의 `services.<svc>` 항목 추가 1줄.

_ha_check_config() {
    if [[ ! -f $HA_JSON ]]; then
        err "HA config 없음: $HA_JSON"
        err "  → $HA_DIR/ha.json.example 을 ha.json 으로 복사 후 노드별 값 수정"
        return 1
    fi
    return 0
}

# ── dpkg 안전 실행 (install 전용) ────────────────────────────────────────
# 두 함정을 함께 막는다 (둘 다 실서버에서 관측된 경로):
#  ① dpkg frontend 락 — 우분투 unattended-upgrade 와 겹치면 dpkg -i 가 즉시 실패한다.
#     실패를 그대로 흘리면 keepalived 없이 config/apply 가 진행돼 "VIP 적용 성공"인데
#     VIP 주인이 없는 상태가 된다 → 바운드 재시도로 락이 풀릴 창을 준다.
#  ② deb postinst 의 데몬 자동기동 — 이 시점엔 /etc/keepalived/keepalived.conf 가 아직
#     없다(우분투 패키지는 .sample 만 제공, 실제 conf 는 `cims-ha apply` 가 나중에 놓는다).
#     그대로 기동되면 "no configuration to run" → Type=notify ready 신호 없음 →
#     systemd 90s start 타임아웃. 그 사이 agent job 스레드가 묶여 heartbeat 가 끊긴다.
#     policy-rc.d(exit 101)로 dpkg 구간의 서비스 기동만 차단한다 (표준 메커니즘).
# 락 경합 재시도는 **짧게** — 이 함수는 agent job 스레드를 붙잡고 있고, 그 동안 같은
# 노드의 다른 job(패키지 install 등)이 큐에서 대기한다(실측: 70초 블로킹 → 콘솔이 오래
# deploying 으로 보임). 락 경합은 일시적이므로 빨리 실패하고 다음 update_ha 가 재시도하는
# 편이 낫다 (keepalived 가 이미 깔린 뒤에는 short-circuit 이라 비용이 없다).
# 락 대기 — 우분투 `unattended-upgrade` 는 한 번 돌면 **수 분**간 dpkg 를 점유한다
# (실측: 14:21:48~14:28:41, 약 7분). 15초(3×5s)만 기다리고 포기하면 그 창에 걸린 설치는
# 매번 실패한다. 총 대기를 100초로 늘리고, 그래도 못 잡으면 다음 회차·주기 재시도에 맡긴다.
_DPKG_LOCK_TRIES=5
_DPKG_LOCK_WAIT=20

# policy-rc.d 는 **우리가 만든 것만** 지운다는 표시를 파일 안에 남긴다.
_HA_POLICY_RC_MARK="# cims-ha managed (dpkg 구간 데몬 자동기동 차단)"
_HA_POLICY_RC_PATH="/usr/sbin/policy-rc.d"

# 남아 있으면 **그 호스트의 모든 패키지 서비스 자동기동이 차단**된다. 정상 종료 경로의
# _ha_policy_rc_off 만으로는 부족하다 — 상위(agent)가 타임아웃으로 프로세스를 kill 하면
# 정리가 실행되지 않는다(SIGKILL 은 trap 불가). 그래서 3중으로 막는다:
#   ① trap (EXIT/TERM/INT/HUP) — 실패·중단 경로 회수
#   ② install 진입 시 **우리 것이면 stale 제거** (전 회차가 kill 된 경우)
#   ③ agent 기동 시 1회 정리 (cims_agent._cleanup_stale_policy_rc)
_ha_policy_rc_stale_clear() {
    [[ -e "$_HA_POLICY_RC_PATH" ]] || return 0
    if grep -qF "$_HA_POLICY_RC_MARK" "$_HA_POLICY_RC_PATH" 2>/dev/null; then
        warn "이전 회차가 남긴 policy-rc.d 발견 — 제거 (서비스 자동기동 차단 해제)"
        sudo rm -f "$_HA_POLICY_RC_PATH" 2>/dev/null || true
    fi
}

# ── 설치 중 서비스 기동 억제 — **유닛 단위 mask** ─────────────────────────
# policy-rc.d(전역)에서 mask(유닛 단위)로 바꾼 이유:
#   · 폭발 반경 — 잔재가 남아도 keepalived 하나만 영향. policy-rc.d 는 그 호스트의
#     **모든** 패키지 서비스 자동기동을 조용히 막는다(실측 사고).
#   · 관측 가능성 — `systemctl is-enabled keepalived` → `masked` 로 즉시 보인다.
#   · 실패가 정직함 — mask 잔재는 "Unit is masked" 로 드러난다.
# postinst 의 `deb-systemd-invoke` 는 masked 유닛을 건너뛰고 정상 종료한다(표준 동작).
_HA_MASK_UNIT="keepalived"

_ha_mask_on() {
    _HA_MASK_OWNED=0
    if [[ "$(systemctl is-enabled "$_HA_MASK_UNIT" 2>/dev/null)" == "masked" ]]; then
        warn "$_HA_MASK_UNIT 가 이미 masked — 이전 회차 잔재로 보고 그대로 사용"
        _HA_MASK_OWNED=1     # 우리가 정리한다 (남겨두면 apply 의 start 가 실패)
    else
        sudo systemctl mask "$_HA_MASK_UNIT" >/dev/null 2>&1 || return 0
        _HA_MASK_OWNED=1
        info "설치 구간 서비스 기동 억제 (systemctl mask $_HA_MASK_UNIT)"
    fi
    # 중단·실패 경로 회수. SIGKILL 은 못 잡으므로 (a) install 진입 시 위 재사용,
    # (b) apply 의 선행 unmask, (c) agent 기동 자가복구가 보완한다.
    trap '_ha_mask_off' EXIT TERM INT HUP
}

_ha_mask_off() {
    [[ "${_HA_MASK_OWNED:-0}" == "1" ]] || return 0
    sudo systemctl unmask "$_HA_MASK_UNIT" >/dev/null 2>&1 || true
    _HA_MASK_OWNED=0
    trap - EXIT TERM INT HUP
}

# apply 가 start 하기 전에 호출 — mask 잔재가 있으면 풀어준다(우리 통제 경로에서 확실히 정리).
_ha_unmask_if_masked() {
    if [[ "$(systemctl is-enabled "$_HA_MASK_UNIT" 2>/dev/null)" == "masked" ]]; then
        sudo systemctl unmask "$_HA_MASK_UNIT" >/dev/null 2>&1 \
            && warn "masked 잔재 해제 ($_HA_MASK_UNIT) — 설치 중단으로 남았던 것"
    fi
}

# ── 패키지 상태 판정 (상태 기반, 멱등) ────────────────────────────────────
_ha_pkg_status() {
    dpkg-query -W -f='${Status}' "$1" 2>/dev/null || echo "not-installed"
}
_ha_pkg_ok() {
    [[ "$(_ha_pkg_status "$1")" == "install ok installed" ]]
}
# iF (half-configured / Failed-config) — 재설치가 아니라 configure 완료가 정답.
_ha_pkg_half_configured() {
    case "$(_ha_pkg_status "$1")" in
        *" half-configured"|*" half-installed"|*"config-files"*) return 0 ;;
    esac
    [[ "$(dpkg -l "$1" 2>/dev/null | awk '/^i/{print $1; exit}')" == "iF" ]]
}

# 남은 configure 를 마무리. 락 경합이면 시도하지 않는다(2 반환).
_ha_dpkg_configure() {
    local out rc
    out=$(_cims_dpkg dpkg --configure -a 2>&1); rc=$?
    if [[ $rc -ne 0 ]] && printf '%s' "$out" | grep -qiE 'lock|frontend'; then
        warn "dpkg 락 경합 — configure 복구 보류"
        return 2
    fi
    printf '%s\n' "$out" | tail -4
    [[ $rc -eq 0 ]] && return 0
    return 1
}

# ── 설치 실패 백오프 ──────────────────────────────────────────────────────
# 같은 실패를 매 update_ha 마다 반복하면 job 이 수십 초씩 묶여 **같은 노드의 패키지
# 설치가 큐에서 대기**한다(실측: deploying 이 길게 유지). systemd StartLimit /
# k8s CrashLoopBackOff 와 같은 취지로 억제 창을 둔다. 억제 중에도 job 은 **실패**로
# 보고한다 — 조용한 성공은 "keepalived 없이 VIP 적용 성공" 사고로 되돌아가는 길이다.
_HA_INSTALL_BACKOFF_SEC=${CIMS_HA_INSTALL_BACKOFF_SEC:-300}
_ha_install_fail_file() { echo "${HA_DIR:-/tmp}/.keepalived_install_fail"; }
_ha_install_fail_mark() {
    date +%s | sudo tee "$(_ha_install_fail_file)" >/dev/null 2>&1 || true
}
_ha_install_fail_clear() {
    sudo rm -f "$(_ha_install_fail_file)" 2>/dev/null || true
}
_ha_install_backoff_left() {
    local f ts now
    f="$(_ha_install_fail_file)"
    [[ -f $f ]] || { echo 0; return; }
    ts=$(cat "$f" 2>/dev/null || echo 0); now=$(date +%s)
    local left=$(( _HA_INSTALL_BACKOFF_SEC - (now - ts) ))
    (( left > 0 )) && echo "$left" || echo 0
}
_ha_install_backoff_active() {
    [[ "$(_ha_install_backoff_left)" != "0" ]]
}

# ── cims 내부 dpkg 직렬화 ────────────────────────────────────────────────
# agent job worker 가 **레인 2개**(module/ha)로 병렬 실행되므로, keepalived 설치(ha 레인)와
# NFS/CIFS 클라이언트 설치(apply_mounts, module 레인)가 동시에 dpkg 를 잡을 수 있다.
# dpkg 는 동시 실행이 불가하므로 둘 중 하나가 실패한다 → 우리 쪽 호출끼리는 먼저 줄을 세운다.
# (외부 unattended-upgrade 와의 경합은 별개로 아래 재시도·backoff 가 담당한다.)
# dpkg 는 동시 실행 불가라 **락을 잡은 채로** 실행해 줄을 세운다(대기만 하고 놓으면 무의미).
_CIMS_DPKG_LOCK="/var/lock/cims-dpkg.lock"

_cims_dpkg() {                     # _cims_dpkg <cmd...> — 락 보유 상태로 실행
    # cims-ha 는 `sudo -n cims-ha` 로 호출되어 이미 root 다. 그때는 sudo 를 덧붙이지
    # 않는다 — sudoers 허용목록은 cims-priv/cims-ha 두 항목뿐이라 새 sudo 대상을
    # 늘리지 않는 편이 안전하다(비root 실행은 기존 sudo 경로와 동일하게 동작).
    local pre=()
    [[ ${EUID:-$(id -u)} -eq 0 ]] || pre=(sudo)
    if command -v flock >/dev/null 2>&1; then
        "${pre[@]}" flock -w 300 "$_CIMS_DPKG_LOCK" "$@"
    else
        "${pre[@]}" "$@"
    fi
}

# dpkg -i 를 락 경합에 견디게 실행. rc 0 이면 성공, 아니면 마지막 rc.
#   $@ = deb 파일들
# 반환: 0=성공 / 2=**락 경합으로 포기**(상위는 fix-broken 등 추가 dpkg 를 시도하지 말 것) /
#       그 외=dpkg rc (실제 설치 오류 → 상위가 fix-broken 시도 가능)
# vendor deb 들이 요구하는 의존성 중 **호스트에도 없고 vendor 에도 없는** 패키지 목록.
# 하나라도 있으면 offline 설치는 성립하지 않는다 — 조용히 실패하고 무한 재시도하는 대신
# 사유를 남기고 apt 경로로 넘긴다(실측: `libsnmp-base` 누락으로 keepalived 가 영영 설치되지
# 않아 VIP 가 뜨지 않았고, cold 모듈인 관리평면이 어느 노드에서도 기동하지 못했다).
_ha_vendor_missing_deps() {           # $@ = deb 파일들 — stdout 에 누락 패키지명(공백 구분)
    local have_pkgs="" d dep name
    for d in "$@"; do
        name=$(dpkg-deb -f "$d" Package 2>/dev/null) && have_pkgs+=" $name"
    done
    local missing=""
    for d in "$@"; do
        dep=$(dpkg-deb -f "$d" Depends 2>/dev/null)
        [[ -z "$dep" ]] && continue
        # "pkg (>= 1.2), other | alt" → 첫 대안의 패키지명만
        while IFS= read -r one; do
            one="${one%%|*}"; one="${one%%(*}"
            one="$(echo "$one" | tr -d '[:space:]')"
            [[ -z "$one" ]] && continue
            [[ " $have_pkgs " == *" $one "* ]] && continue
            [[ " $missing " == *" $one "* ]] && continue
            dpkg-query -W -f='${Status}' "$one" 2>/dev/null | grep -q "install ok installed" && continue
            missing+=" $one"
        done < <(echo "$dep" | tr ',' '\n')
    done
    echo "${missing# }"
}

_ha_dpkg_install() {
    local try rc out
    for ((try = 1; try <= _DPKG_LOCK_TRIES; try++)); do
        out=$(_cims_dpkg dpkg -i --force-confnew --force-overwrite "$@" 2>&1)
        rc=$?
        [[ $rc -eq 0 ]] && { printf '%s\n' "$out" | tail -3; return 0; }
        # 락 판정은 **정확한 문구**로만. 옛 패턴 `lock|frontend|being used by` 는 부분일치라
        # dpkg 의 일반 오류(예: "...blocked...", 패키지명에 lock 포함)도 락으로 오인했고,
        # 그러면 rc=2(외부 락)로 분류돼 **마커 없이 무한 재시도**한다(실측: keepalived 가
        # 영영 설치되지 않아 VIP 가 뜨지 않았고 cold 관리평면이 어디서도 기동 못 함).
        if printf '%s' "$out" | grep -qiE 'dpkg frontend lock|Could not get lock|lock .* (is )?held by|dpkg status database is locked|another process (is )?using'; then
            warn "dpkg 락 경합 (시도 $try/$_DPKG_LOCK_TRIES) — ${_DPKG_LOCK_WAIT}s 후 재시도"
            sleep "$_DPKG_LOCK_WAIT"
            continue
        fi
        printf '%s\n' "$out" | tail -5
        return $rc
    done
    err "dpkg 락이 ${_DPKG_LOCK_TRIES}회 재시도 동안 풀리지 않음 (apt.systemd.daily/unattended-upgrade 확인)"
    err "  마지막 dpkg 출력: $(printf '%s' "$out" | tail -3 | tr '\n' ' ')"
    return 2
}

# uninstall→re-install 시 dpkg 가 conffile 을 .dpkg-new 로 깔아 keepalived start FAILURE.
# 정상 이름이 비어있으면 mv, 이미 있으면 skip (운영자 검토 필요).
_ha_post_install_fixups() {
    local newf orig
    for newf in /etc/keepalived/*.dpkg-new; do
        [[ -e $newf ]] || continue
        orig="${newf%.dpkg-new}"
        if [[ -e $orig ]]; then
            info ".dpkg-new 잔재 발견 — 정상 파일 이미 있음, skip: $newf"
        else
            sudo mv "$newf" "$orig" && ok ".dpkg-new 정상 이름으로 이동: $orig"
        fi
    done
}

# cims-health/cims-notify 스테이징 경로 — `cims-ha apply` 가 root:root 로 복사.
# 버전 디렉토리(agent/<ver>/bin) 대신 이 고정 경로를 keepalived.conf 가 참조 —
# agent 업그레이드(current flip)에 안전 + enable_script_security(root 소유 요구) 통과.
HA_STAGE_BIN="/etc/keepalived/bin"

# 단일 keepalived.conf.tpl + ha.json.services 반복 → out/keepalived.conf
_ha_render_keepalived() {
    local out="$1"
    # 템플릿은 실행 중인 번들(cims-ha 와 같은 버전 트리)이 정본. --ha-dir 로 ha.json
    # 위치가 분리된 경우(agent 의 update_ha job — run/keepalived/) 그 디렉토리에는
    # 템플릿이 없으므로 번들 fallback 이 필수.
    local tpl="$HA_DIR/keepalived.conf.tpl"
    [[ ! -f $tpl ]] && tpl="$SCRIPT_DIR/../keepalived/keepalived.conf.tpl"
    [[ ! -f $tpl ]] && { err "템플릿 없음: $HA_DIR 및 $SCRIPT_DIR/../keepalived"; return 1; }

    python3 - "$HA_JSON" "$tpl" "$HA_STAGE_BIN" "$out" <<'PY'
import json, re, sys
ha_json, tpl_path, bin_dir, out_path = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
cfg = json.load(open(ha_json))

required_top = ["node_name", "interface", "local_ip", "peer_ip", "initial_state",
                "vip_mask", "auth_pass"]
missing = [k for k in required_top if k not in cfg]
if missing:
    sys.stderr.write(f"ha.json missing keys: {missing}\n"); sys.exit(2)

services = cfg.get("services") or {}
if not services:
    sys.stderr.write("ha.json.services 비어있음\n"); sys.exit(3)

tpl = open(tpl_path).read()
# 템플릿은 두 section 으로 구성:
#   {{HEADER}}        ← global_defs 등 한 번만
#   {{PER_SERVICE}}   ← vrrp_script + vrrp_instance 블록 (services 반복)
# 두 section 모두 ${VAR} placeholder 사용.

def split_sections(t):
    header_marker = "{{PER_SERVICE_BEGIN}}"
    footer_marker = "{{PER_SERVICE_END}}"
    if header_marker not in t or footer_marker not in t:
        return t, ""
    head_part, rest = t.split(header_marker, 1)
    body_part, _ = rest.split(footer_marker, 1)
    return head_part, body_part

header_part, body_part = split_sections(tpl)

def render(s, mapping):
    return re.sub(r'\$\{([A-Z_]+)\}',
                  lambda m: mapping.get(m.group(1), m.group(0)),
                  s)

common = {
    "NODE_NAME":     cfg["node_name"],
    "INTERFACE":     cfg["interface"],
    "LOCAL_IP":      cfg["local_ip"],
    "PEER_IP":       cfg["peer_ip"],
    "INITIAL_STATE": cfg["initial_state"],
    "VIP_MASK":      str(cfg["vip_mask"]),
    "AUTH_PASS":     cfg["auth_pass"],
    "BIN_DIR":       bin_dir,
    "CIMS_HOME":     cfg.get("cims_home", "/opt/cims"),
    "CIMS_USER":     cfg.get("cims_user", "cims"),
}

def _build_vip_list(val, default_mask, default_iface):
    """vips[] 배열 (Phase 2) 또는 단일 vip (legacy) → indented multi-line block.

    Phase 4 fix: 각 vip 의 dev (NIC) 가 명시되어 있으면 그것 우선 — 다중 망에
    걸친 multi-VIP (외부망 121.x VIP + 내부망 10.0.x VIP 등) 지원.
    """
    iface = val.get("interface") or default_iface
    vips = val.get("vips")
    if isinstance(vips, list) and vips:
        return "\n".join(
            f"        {v.get('ip','')}/{v.get('mask', default_mask)} dev {v.get('dev') or iface}"
            for v in vips if v.get('ip')
        )
    ip = val.get("vip", "")
    if not ip:
        return ""
    return f"        {ip}/{default_mask} dev {iface}"

def _failover_opts(val, iface):
    """val.failover_options → keepalived placeholder dict.

    csc 가 미전송 (옛 record) 시 defaults 와 동일하게 채움 — 호환성.
    """
    fo = val.get("failover_options") or {}
    health = fo.get("health") or {}
    # keepalived advert_int 는 float OK (예: 0.5). 정수면 "1", 소수면 "0.5" 형태.
    try:
        ai = float(fo.get("advert_int", 1))
        if not (0.5 <= ai <= 5):
            ai = 1.0
    except (TypeError, ValueError):
        ai = 1.0
    advert_int = str(int(ai)) if ai == int(ai) else f"{ai:g}"
    preempt = fo.get("preempt", "nopreempt")
    preempt_delay = int(fo.get("preempt_delay", 0) or 0)
    if preempt == "preempt":
        preempt_line = f"preempt_delay         {preempt_delay}" if preempt_delay > 0 else "preempt"
    else:
        preempt_line = "nopreempt"
    track_iface_block = ""
    if fo.get("track_interface"):
        track_iface_block = f"    track_interface {{\n        {iface}\n    }}"
    return {
        "ADVERT_INT":            advert_int,
        "HEALTH_INTERVAL":       str(int(health.get("interval", 2) or 2)),
        "HEALTH_FALL":           str(int(health.get("fall", 2) or 2)),
        "HEALTH_RISE":           str(int(health.get("rise", 2) or 2)),
        "HEALTH_TIMEOUT":        str(int(health.get("timeout", 3) or 3)),
        "PREEMPT_LINE":          preempt_line,
        "TRACK_INTERFACE_BLOCK": track_iface_block,
    }

out = [render(header_part, common)]
for svc, val in services.items():
    if not val.get("enabled"):
        continue
    svc_iface = val.get("interface") or common["INTERFACE"]
    svc_map = dict(common)
    svc_map.update({
        "SVC":         svc,
        "SVC_UPPER":   svc.upper(),
        "VRID":        str(val.get("vrid", 0)),
        "VIP":         val.get("vip", ""),
        "VIP_LIST":    _build_vip_list(val, common["VIP_MASK"], svc_iface),
        "INTERFACE":   svc_iface,
        "PRIORITY":    str(val.get("priority", 100)),
        "PORT":        str(val.get("port", 0)),
        "PROTO":       val.get("proto", "udp"),
        "BIND_IP":     val.get("bind_ip", ""),
        "UNIT":        val.get("unit", f"cims@{svc}.service"),
    })
    svc_map.update(_failover_opts(val, svc_iface))
    out.append(render(body_part, svc_map))

content = "".join(out)
open(out_path, "w").write(content)
PY
}

# 단일 cims@.service.tpl → out/cims@.service (인스턴스 별 unit 은 systemd 가 %i 처리)
_ha_render_unit() {
    local out="$1"
    local tpl="$HA_UNIT_DIR/cims@.service.tpl"
    [[ ! -f $tpl ]] && { err "템플릿 없음: $tpl"; return 1; }

    python3 - "$HA_JSON" "$tpl" > "$out" <<'PY'
import json, re, sys
ha_json, tpl_path = sys.argv[1], sys.argv[2]
cfg = json.load(open(ha_json))
mapping = {
    "CIMS_HOME": cfg.get("cims_home", "/opt/cims"),
    "CIMS_USER": cfg.get("cims_user", "cims"),
}
content = open(tpl_path).read()
content = re.sub(r'\$\{([A-Z_]+)\}',
                 lambda m: mapping.get(m.group(1), m.group(0)),
                 content)
sys.stdout.write(content)
PY
}

_ha_enabled_services() {
    python3 -c "
import json
cfg = json.load(open('$HA_JSON'))
for svc, val in cfg.get('services', {}).items():
    if val.get('enabled'):
        print(svc)
" 2>/dev/null
}

# apply 가 'systemctl enable cims@<svc>.service' 로 만든 instance enable 심볼릭링크 중
# 현재 대상(인자로 전달)에 없는 것 = 옛 slug 잔재 (예: cims@Control-Server.service).
# 인자 없이 호출하면 모든 cims@ instance 를 disable (uninstall 시 전체 정리).
# template (/etc/systemd/system/cims@.service) 자체는 건드리지 않음 — uninstall 만 제거.
_ha_prune_stale_instances() {
    local keep=" $* "                       # space-padded 현재 svc 목록
    local link inst seen=" "
    for link in /etc/systemd/system/cims@*.service \
                /etc/systemd/system/*.wants/cims@*.service; do
        [[ -L $link ]] || continue
        inst=$(basename "$link"); inst="${inst#cims@}"; inst="${inst%.service}"
        [[ -z $inst ]] && continue
        [[ $seen == *" $inst "* ]] && continue      # 두 위치 중복 방지
        seen+="$inst "
        if [[ $keep != *" $inst "* ]]; then
            info "stale HA instance 정리: cims@${inst}.service disable"
            sudo systemctl disable "cims@${inst}.service" 2>/dev/null || true
        fi
    done
}

cmd_ha() {
    local sub="${1:-help}"
    shift || true

    case "$sub" in
        install)
            # ── 멱등 설치 (상태 기반 판정) ─────────────────────────────────────
            # 판정을 `keepalived -v` **실행 성공**으로 하면 안 된다: 공유 라이브러리가 갱신되는
            # 순간에 실패해 "강제 재설치" 로 넘어가고, 그 재설치가 락 경합에 걸려 매 회차
            # 90초를 태우는 루프가 된다(실측 사고). 표준(구성관리 도구의 package 리소스)대로
            # **패키지 관리자 상태**로 판정하고, 실행 가능 여부는 별도 헬스체크로 분리한다.
            local vendor_dir="$SCRIPT_DIR/../vendor/keepalived"
            local base_dir="$SCRIPT_DIR/../vendor/base"
            _ha_policy_rc_stale_clear   # 구버전(policy-rc.d 방식)이 남긴 전역 차단 파일 회수
            if _ha_pkg_ok keepalived; then
                ok "keepalived 설치 정상 (dpkg: install ok installed)"
                _ha_install_fail_clear
                keepalived -v >/dev/null 2>&1 \
                    || warn "패키지는 정상인데 실행 실패 — 의존성 확인 필요(설치 재시도는 하지 않음)"
            elif _ha_install_backoff_active; then
                # 반복 실패 억제 — systemd StartLimit / k8s CrashLoopBackOff 계열.
                # 조용히 성공으로 넘기지 않는다(그러면 keepalived 없이 VIP 적용 성공으로
                # 보고되는 옛 사고로 회귀). 실패로 반환하되 이유를 명시한다.
                err "직전 설치 실패로 재시도 억제 중 ($(_ha_install_backoff_left)s 남음) — 원인 해결 후 재시도"
                return 1
            else
                # half-configured(iF) 는 **재설치가 아니라 설정 완료**가 정답이다.
                if _ha_pkg_half_configured keepalived; then
                    info "keepalived 가 half-configured(iF) — dpkg --configure 로 복구 시도"
                    _ha_mask_on          # 복구 중 postinst 가 서비스를 켜지 못하게
                    if _ha_dpkg_configure; then
                        _ha_mask_off
                        _ha_install_fail_clear
                        ok "keepalived 설정 완료 복구 (재설치 불필요)"
                        _ha_post_install_fixups
                        return 0
                    fi
                    _ha_mask_off
                    warn "configure 복구 실패 — 재설치 경로로 진행"
                fi
                # vendor (offline) — private 환경 기본 경로. vendor 없으면 apt fallback.
                # 설치 구간을 mask 로 감싼다 — postinst 가 conf(vrrp_instance) 없이 keepalived 를
                # 기동해 systemd start 타임아웃(90s) → configure 실패 → iF 로 갇히는 경로 차단.
                # policy-rc.d(전역) 대신 **유닛 단위 mask** — 잔재가 남아도 폭발 반경이
                # keepalived 하나이고 `systemctl is-enabled` 로 즉시 관측된다.
                _ha_mask_on
                if ls "$vendor_dir"/*.deb >/dev/null 2>&1; then
                    info "keepalived offline 설치 (vendor: $vendor_dir, --force-confnew --force-overwrite)"
                    # base 공유 의존성(libmnl0 등)도 함께 — keepalived 가 의존하므로 air-gapped
                    # 에서 같은 dpkg 호출에 포함해 의존성 충족 (base deb 는 uninstall 시 제거 안 함).
                    local _ka_debs=("$vendor_dir"/*.deb)
                    ls "$base_dir"/*.deb >/dev/null 2>&1 && _ka_debs+=("$base_dir"/*.deb)
                    # --force-confnew: 옛 conf 보존 안 함 (cims-ha apply 가 어차피 덮어씀)
                    # --force-overwrite: 다른 package 의 file 과 conflict 시 덮어쓰기 (재설치 안정성)
                    # 의존성 사전 점검 — 누락이 있으면 offline 은 실패가 확정이다.
                    local _miss; _miss=$(_ha_vendor_missing_deps "${_ka_debs[@]}")
                    if [[ -n "$_miss" ]]; then
                        warn "vendor 세트에 없는 의존성: $_miss — apt 로 먼저 채운다"
                        _cims_dpkg apt-get -o DPkg::Lock::Timeout=100 update >/dev/null 2>&1 || true
                        _cims_dpkg apt-get -o DPkg::Lock::Timeout=100 -y install $_miss \
                            || warn "의존성 apt 설치 실패($_miss) — dpkg 시도는 계속"
                    fi
                    local _rc_dpkg=0
                    _ha_dpkg_install "${_ka_debs[@]}" || _rc_dpkg=$?
                    if [[ $_rc_dpkg -eq 2 ]]; then
                        # 락 경합 — fix-broken 도 같은 락을 요구하므로 시도하지 않는다(옛 동작은
                        # DPkg::Lock::Timeout=60 으로 60초를 더 기다려 job 을 90초대로 늘렸다).
                        # 다음 update_ha 가 자연 재시도한다.
                        err "dpkg 락 경합 — 이번 회차 설치 보류 (다음 회차 재시도)"
                        _ha_mask_off
                        # **backoff 를 걸지 않는다.** 외부 락(unattended-upgrade 등)은 몇 분이면
                        # 풀리는 일시 조건이고, 우리 실패가 아니다. 여기에 backoff 를 걸었더니
                        # 그 5분 동안 시도가 봉인되고, 만료 시점엔 이미 update_ha 이벤트가
                        # 소진돼 **아무도 다시 시도하지 않는** 영구 미설치가 됐다(실측 회귀).
                        # 재시도는 아래 install-ensurer(주기 루프)와 다음 update_ha 가 담당한다.
                        return 1
                    elif [[ $_rc_dpkg -ne 0 ]]; then
                        warn "dpkg -i 실패(rc=$_rc_dpkg) — broken deps fix-broken 시도"
                        _cims_dpkg apt-get -o DPkg::Lock::Timeout=100 -y --fix-broken install || true
                        if ! _ha_dpkg_install "${_ka_debs[@]}"; then
                            # **apt 로 최종 재시도** — vendor 가 있다고 apt 를 포기하면,
                            # vendor 세트가 불완전할 때 영영 설치되지 않는다(실측). 저장소가
                            # 닿는 환경이면 여기서 끝난다.
                            warn "vendor 설치 실패 — apt 로 재시도"
                            _cims_dpkg apt-get -o DPkg::Lock::Timeout=100 update >/dev/null 2>&1 || true
                            if ! _cims_dpkg apt-get -o DPkg::Lock::Timeout=100 -y install keepalived; then
                                err "keepalived 설치 실패 — vendor·apt 모두 실패"
                                [[ -n "$_miss" ]] && err "  누락 의존성: $_miss (vendor 세트 보강 필요)"
                                _ha_mask_off
                                _ha_install_fail_mark
                                return 1
                            fi
                        fi
                    fi
                    _ha_mask_off
                    # 설치 후 상태 확인 — 패키지 상태가 정본, 실행 가능 여부는 부가 정보.
                    if ! _ha_pkg_ok keepalived; then
                        err "설치 후에도 패키지 상태 비정상 ($(_ha_pkg_status keepalived))"
                        _ha_install_fail_mark
                        return 1
                    fi
                    _ha_install_fail_clear
                    ok "keepalived 설치 완료 (vendor): $(keepalived -v 2>&1 | head -1)"
                else
                    info "keepalived 설치 (apt fallback) — sudo + 인터넷 필요"
                    sudo apt-get -o DPkg::Lock::Timeout=100 update || warn "apt-get update 실패 — 캐시로 진행"
                    if ! _cims_dpkg apt-get -o DPkg::Lock::Timeout=100 -y install keepalived; then
                        err "apt-get install keepalived 실패"
                        _ha_mask_off
                        _ha_install_fail_mark
                        return 1
                    fi
                    _ha_mask_off
                    if ! _ha_pkg_ok keepalived; then
                        err "설치 후에도 패키지 상태 비정상 ($(_ha_pkg_status keepalived))"
                        _ha_install_fail_mark
                        return 1
                    fi
                    _ha_install_fail_clear
                    ok "keepalived 설치 완료 (apt): $(keepalived -v 2>&1 | head -1)"
                fi
            fi
            # 부팅 자동기동(enable)은 **끄지 않는다.** 재부팅 후 VIP 자력 복구가 그 경로에
            # 의존한다 — update_ha 재렌더는 그룹/멤버 변경·배포·서비스 제어·enroll 시점에만
            # 큐잉되고 **단순 재부팅은 트리거가 아니다**(세션 토큰 유지 = re-enroll 아님).
            # 설정 없이 부팅 기동해 유닛이 failed 로 남는 것은 무해하다(패키지 상태를
            # 오염시키는 것은 configure 단계뿐이고, 다음 apply 가 정리한다).
            _ha_post_install_fixups
            ;;
        config)
            _ha_check_config || return 1
            mkdir -p "$HA_OUT"
            info "rendering keepalived (단일 tpl, services 반복)"
            _ha_render_keepalived "$HA_OUT/keepalived.conf" || return 1
            info "rendering systemd unit (cims@.service)"
            _ha_render_unit "$HA_OUT/cims@.service" || return 1
            ok "HA config 생성: $HA_OUT/keepalived.conf + $HA_OUT/cims@.service"
            ;;
        check)
            local out="$HA_OUT/keepalived.conf"
            [[ ! -f $out ]] && { err "config 미생성: $out — 먼저 'cims-ha config' 실행"; return 1; }
            if ! command -v keepalived &>/dev/null; then
                warn "keepalived 미설치 — syntax 검증 SKIP. 'cims-ha install' 후 재시도"
                return 0
            fi
            info "syntax 검증: keepalived -t -f $out"
            sudo keepalived -t -f "$out" && ok "syntax OK" || { err "syntax 실패"; return 1; }
            ;;
        apply)
            local out="$HA_OUT/keepalived.conf"
            local unit="$HA_OUT/cims@.service"
            [[ ! -f $out ]] && { err "config 미생성: $out — 먼저 'cims-ha config' 실행"; return 1; }
            [[ ! -f $unit ]] && { err "unit 미생성: $unit"; return 1; }
            # **keepalived 가 없으면 apply 는 성공이 아니다.** 옛 동작은 파일만 배치하고
            # rc=0 을 냈고, 콘솔에는 "VIP 적용 성공" 인데 실제로는 VIP 주인이 없어 cold 인
            # 관리평면이 어느 노드에서도 뜨지 못했다(실측). 설치 실패를 여기서 다시 드러낸다.
            if ! _ha_pkg_ok keepalived; then
                err "keepalived 미설치 — apply 불가 (패키지 상태: $(_ha_pkg_status keepalived))"
                err "  VIP 주인이 없으면 cold 모듈(관리평면 포함)이 어느 노드에서도 기동하지 않습니다."
                return 1
            fi
            # 변경 감지 — 스테이징 대상 5종이 기존 적용본과 전부 동일하면 keepalived
            # 무접촉. 배포/서비스(start/stop) 이벤트마다 재렌더가 전파되므로 apply 가
            # 멱등이어야 잦은 호출이 VRRP 상태(MASTER/VIP)를 흔들지 않는다.
            local _hachanged=0 _pair _src _dst
            for _pair in "$out:/etc/keepalived/keepalived.conf" \
                         "$HA_JSON:/etc/keepalived/ha.json" \
                         "$SCRIPT_DIR/cims-health:$HA_STAGE_BIN/cims-health" \
                         "$SCRIPT_DIR/cims-notify:$HA_STAGE_BIN/cims-notify" \
                         "$unit:/etc/systemd/system/cims@.service"; do
                _src="${_pair%%:*}"; _dst="${_pair#*:}"
                cmp -s "$_src" "$_dst" 2>/dev/null || { _hachanged=1; break; }
            done

            # health/notify 스크립트 + ha.json 스테이징 — root:root 고정 경로.
            #   · conf 의 script/notify 가 ${HA_STAGE_BIN} 을 참조 (버전 트리 비의존)
            #   · root 소유 + group-write 없음 → enable_script_security 통과
            #     (agent 배포 트리는 비-root 소유라 keepalived 가 "insecure" 로 비활성화)
            #   · cims-health 는 자기 위치 기준 ../ha.json lookup → 함께 스테이징
            info "health/notify 스크립트 스테이징: $HA_STAGE_BIN (root:root)"
            sudo install -d -m 755 -o root -g root /etc/keepalived "$HA_STAGE_BIN"
            sudo install -m 755 -o root -g root \
                "$SCRIPT_DIR/cims-health" "$SCRIPT_DIR/cims-notify" "$HA_STAGE_BIN/"
            sudo install -m 644 -o root -g root "$HA_JSON" /etc/keepalived/ha.json
            info "/etc/keepalived/keepalived.conf 적용 — sudo 권한 필요"
            sudo cp "$out" /etc/keepalived/keepalived.conf
            info "/etc/systemd/system/cims@.service 적용"
            sudo cp "$unit" /etc/systemd/system/cims@.service
            sudo systemctl daemon-reload
            # cims@ instance enable 하지 않음 — 절체 시 모듈 제어는 cims-notify 가
            # ha.json services.<svc>.cold_modules 를 보고 cims-svc 로 직접 수행
            # (systemd 유닛 경유 폐지 — 그룹명 slug 가 lifecycle 모듈명과 달라 항상
            # 실패하던 경로. 단일 lifecycle 경로로 일원화). 옛 enable 잔재는 전부 정리.
            _ha_prune_stale_instances
            # VIP 를 보유하지 않은 BACKUP 노드도 VIP 로 bind 가능해야 fail-over 즉시 처리
            # (hot 모듈 — standby 도 기동 유지 — 이 VIP 로 listen 하는 경우). private 환경: apt/외부 의존 없이
            # sysctl 로 직접 설정 + /etc/sysctl.d 영구화. agent 가 sudo 로 cims-ha 를 실행하므로
            # 별도 수동 sudo 불요 — "HA install/update(apply) 시 자동" 충족.
            info "net.ipv4.ip_nonlocal_bind=1 설정 (VIP backup bind 전제)"
            echo 'net.ipv4.ip_nonlocal_bind = 1' | sudo tee /etc/sysctl.d/99-cims-ha.conf >/dev/null
            sudo sysctl -w net.ipv4.ip_nonlocal_bind=1 >/dev/null 2>&1 || true
            # enabled 인스턴스 0개 (전 서비스 disabled — 미개시 그룹/배포 없는 멤버) 이면
            # 정지 유지 — vrrp_instance 없는 conf 로 systemctl restart 하면 keepalived 가
            # 기동 완료를 알리지 못해 60초+ hang (agent job timeout → heartbeat 끊겨
            # offline 오판). 인스턴스가 있으면: 변경 없음 → 무접촉 / 변경 → 가동 중이면
            # reload (VRRP 상태 유지 — restart 는 MASTER 를 내렸다 올려 무의미한 절체 유발)
            # / 정지 상태면 start.
            # 설치가 중단돼 mask 가 남아 있으면 start 가 "Unit is masked" 로 실패한다 —
            # 우리 통제 경로인 여기서 확실히 정리한다.
            _ha_unmask_if_masked
            if ! grep -q '^vrrp_instance' /etc/keepalived/keepalived.conf; then
                sudo systemctl stop keepalived 2>/dev/null || true
                ok "vrrp_instance 없음 — keepalived 정지 상태 유지 (서비스 개시/인스턴스 렌더 시 자동 기동)"
            elif [[ $_hachanged -eq 0 ]] && systemctl is-active --quiet keepalived; then
                ok "변경 없음 — keepalived 무접촉 (이미 적용된 구성)"
            elif systemctl is-active --quiet keepalived; then
                sudo systemctl reload keepalived
                ok "keepalived reload — 구성 변경 반영 (VRRP 상태 유지, cold_modules 절체는 cims-notify → cims-svc)"
            else
                sudo systemctl start keepalived
                ok "keepalived 기동 + ip_nonlocal_bind 적용 완료 (cold_modules 절체는 cims-notify → cims-svc)"
            fi
            ;;
        start)  sudo systemctl start  keepalived ;;
        stop)   sudo systemctl stop   keepalived ;;
        status) systemctl status keepalived --no-pager || true ;;
        uninstall)
            # agent uninstall 대칭 — install 이 시스템에 깐 것을 모두 제거.
            # 정책: vendor offline 으로 깔린 keepalived + deps 는 apt repo 와 버전 불일치라
            #       apt-get purge 가 broken deps 만든다 (libsnmp40t64 vendor only 등).
            #       → vendor *.deb 의 package list 추출 후 dpkg -P 로 직접 제거 (apt 안 거침).
            local vendor_dir="$SCRIPT_DIR/../vendor/keepalived"
            local pkgs=()
            # base 공유 의존성 denylist — keepalived 와 함께 쓰이지만 iproute2(`ip`)
            # 등 OS base 도 의존하므로 절대 purge 하지 않는다. (vendor/base 로 분리했어도
            # 혹시 keepalived dir 에 남아있을 경우의 방어. libmnl0 제거 → `ip` 깨짐 재발 차단.)
            local _base_keep=" libmnl0 "
            if ls "$vendor_dir"/*.deb >/dev/null 2>&1; then
                local deb pkg
                for deb in "$vendor_dir"/*.deb; do
                    pkg=$(dpkg-deb -f "$deb" Package 2>/dev/null)
                    [[ -z $pkg ]] && continue
                    if [[ "$_base_keep" == *" $pkg "* ]]; then
                        info "base 공유 의존성 보존 (purge 제외): $pkg"
                        continue
                    fi
                    pkgs+=("$pkg")
                done
            fi
            # vendor list 없으면 keepalived 만 — apt 설치 시나리오 fallback.
            [[ ${#pkgs[@]} -eq 0 ]] && pkgs=(keepalived)

            # systemd HA instance enable 심볼릭링크 전체 disable + template 제거 (apply 대칭).
            _ha_prune_stale_instances        # 인자 없음 → 모든 cims@ instance disable
            sudo rm -f /etc/systemd/system/cims@.service 2>/dev/null || true
            sudo systemctl daemon-reload 2>/dev/null || true

            if ! command -v keepalived >/dev/null 2>&1 && ! dpkg -s "${pkgs[0]}" >/dev/null 2>&1; then
                info "keepalived 미설치 — skip"
                sudo rm -rf /etc/keepalived "$HA_DIR/out" 2>/dev/null || true
                return 0
            fi
            info "keepalived service stop"
            sudo systemctl stop keepalived 2>/dev/null || true
            sudo systemctl disable keepalived 2>/dev/null || true

            info "dpkg -P (vendor packages: ${pkgs[*]})"
            # --force-all: deps chain 무시하고 강제 제거. broken state 의 host 도 정리 가능.
            if sudo dpkg -P --force-all "${pkgs[@]}" 2>&1 | tail -5; then
                ok "vendor packages purged via dpkg -P"
            else
                warn "dpkg -P 일부 실패 — apt-get fallback (마지막 수단)"
                sudo apt-get -y --fix-broken install 2>/dev/null || true
                sudo apt-get -y purge "${pkgs[@]}" 2>/dev/null || true
            fi
            sudo rm -rf /etc/keepalived "$HA_DIR/out" 2>/dev/null || true
            ok "keepalived + vendor deps + /etc/keepalived + out/ 제거"
            ;;
        help|*)
            cat <<EOF
사용법: cims-ha <subcommand>

  install         keepalived 패키지 설치 (vendor deb 우선, apt fallback)
  uninstall       keepalived + deps + /etc/keepalived + cims@ instance/template 제거 (install 대칭)
  config          ha.json + 템플릿 → out/{keepalived.conf, cims@.service} 생성 (dry-run)
  check           keepalived -t syntax 검증
  apply           out/* → /etc/keepalived/ + /etc/systemd/system/ + daemon-reload +
                  systemctl enable cims@<svc>.service + keepalived 재시작
  start|stop      systemctl start|stop keepalived
  status          systemctl status keepalived

설정 파일: $HA_JSON
  예시:        $HA_DIR/ha.json.example
  systemd:     $HA_UNIT_DIR/cims@.service.tpl
EOF
            ;;
    esac
}
