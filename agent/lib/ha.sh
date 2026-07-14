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
            # idempotent — 이미 설치 + binary 실제 동작 가능하면 short-circuit.
            # `command -v` 만으론 broken state (vendor deps 깨져 exit 127) 식별 불가 → -v 실행 검증.
            local vendor_dir="$SCRIPT_DIR/../vendor/keepalived"
            local base_dir="$SCRIPT_DIR/../vendor/base"
            if command -v keepalived >/dev/null 2>&1 && keepalived -v >/dev/null 2>&1; then
                ok "keepalived already installed: $(keepalived -v 2>&1 | head -1)"
            else
                if command -v keepalived >/dev/null 2>&1; then
                    warn "keepalived binary 존재하지만 실행 실패 (deps 깨짐 가능) — 강제 재설치"
                fi
                # vendor (offline) — private 환경 기본 경로. vendor 없으면 apt fallback.
                if ls "$vendor_dir"/*.deb >/dev/null 2>&1; then
                    info "keepalived offline 설치 (vendor: $vendor_dir, --force-confnew --force-overwrite)"
                    # base 공유 의존성(libmnl0 등)도 함께 — keepalived 가 의존하므로 air-gapped
                    # 에서 같은 dpkg 호출에 포함해 의존성 충족 (base deb 는 uninstall 시 제거 안 함).
                    local _ka_debs=("$vendor_dir"/*.deb)
                    ls "$base_dir"/*.deb >/dev/null 2>&1 && _ka_debs+=("$base_dir"/*.deb)
                    # --force-confnew: 옛 conf 보존 안 함 (cims-ha apply 가 어차피 덮어씀)
                    # --force-overwrite: 다른 package 의 file 과 conflict 시 덮어쓰기 (재설치 안정성)
                    sudo dpkg -i --force-confnew --force-overwrite "${_ka_debs[@]}" || {
                        warn "dpkg -i 실패 — broken deps fix-broken 시도"
                        sudo apt-get -y --fix-broken install || true
                        sudo dpkg -i --force-confnew --force-overwrite "${_ka_debs[@]}" || {
                            err "dpkg -i 재시도 실패"
                            return 1
                        }
                    }
                    ok "keepalived 설치 완료 (vendor): $(keepalived -v 2>&1 | head -1)"
                else
                    info "keepalived 설치 (apt fallback) — sudo + 인터넷 필요"
                    sudo apt-get update
                    sudo apt-get install -y keepalived
                    ok "keepalived 설치 완료 (apt): $(keepalived -v 2>&1 | head -1)"
                fi
            fi
            # uninstall→re-install 시 dpkg 가 conffile 을 .dpkg-new 로 깔아 keepalived start FAILURE.
            # 정상 이름이 비어있으면 mv, 이미 있으면 skip (운영자 검토 필요).
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
            # enabled 인스턴스 0개 (전 서비스 disabled — 예: 배포 없는 멤버 렌더) 이면
            # restart 하지 않고 정지 — vrrp_instance 없는 conf 로 systemctl restart 하면
            # keepalived 가 기동 완료를 알리지 못해 60초+ hang (agent job timeout → 노드가
            # heartbeat 를 못 보내 offline 오판). 이후 인스턴스가 생기는 재렌더가 오면
            # 그때 restart 경로로 복귀.
            if ! grep -q '^vrrp_instance' /etc/keepalived/keepalived.conf; then
                sudo systemctl stop keepalived 2>/dev/null || true
                ok "vrrp_instance 없음 — keepalived 정지 상태 유지 (인스턴스 렌더 시 자동 기동)"
            else
                sudo systemctl restart keepalived
                ok "keepalived + ip_nonlocal_bind 적용 완료 (cold_modules 절체는 cims-notify → cims-svc)"
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
