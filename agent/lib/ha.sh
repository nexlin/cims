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

# 단일 keepalived.conf.tpl + ha.json.services 반복 → out/keepalived.conf
_ha_render_keepalived() {
    local out="$1"
    local tpl="$HA_DIR/keepalived.conf.tpl"
    [[ ! -f $tpl ]] && { err "템플릿 없음: $tpl"; return 1; }

    python3 - "$HA_JSON" "$tpl" "$HA_DIR" "$out" <<'PY'
import json, re, sys
ha_json, tpl_path, ha_dir, out_path = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
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
    "HA_DIR":        ha_dir,
    "CIMS_HOME":     cfg.get("cims_home", "/opt/cims"),
    "CIMS_USER":     cfg.get("cims_user", "cims"),
}

def _build_vip_list(val, default_mask, default_iface):
    """vips[] 배열 (Phase 2) 또는 단일 vip (legacy) → indented multi-line block."""
    iface = val.get("interface") or default_iface
    vips = val.get("vips")
    if isinstance(vips, list) and vips:
        return "\n".join(
            f"        {v.get('ip','')}/{v.get('mask', default_mask)} dev {iface}"
            for v in vips if v.get('ip')
        )
    ip = val.get("vip", "")
    if not ip:
        return ""
    return f"        {ip}/{default_mask} dev {iface}"

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

cmd_ha() {
    local sub="${1:-help}"
    shift || true

    case "$sub" in
        install)
            # idempotent — 이미 설치되어 있으면 short-circuit (agent 가 ha.json 받을 때마다 호출됨).
            if command -v keepalived >/dev/null 2>&1; then
                ok "keepalived already installed: $(keepalived --version 2>&1 | head -1)"
                return 0
            fi
            # vendor (offline) 우선 — private 환경 (인터넷 차단) 에서도 동작.
            # SCRIPT_DIR/../vendor/keepalived/*.deb 가 있으면 dpkg -i 로 설치, 없으면 apt fallback.
            local vendor_dir="$SCRIPT_DIR/../vendor/keepalived"
            if ls "$vendor_dir"/*.deb >/dev/null 2>&1; then
                info "keepalived offline 설치 (vendor: $vendor_dir)"
                sudo dpkg -i "$vendor_dir"/*.deb || {
                    err "dpkg -i 실패 — 의존 부족 시 apt-get -f install 필요"
                    return 1
                }
                ok "keepalived 설치 완료 (vendor): $(keepalived --version 2>&1 | head -1)"
            else
                info "keepalived 설치 (apt fallback) — sudo + 인터넷 필요"
                sudo apt-get update
                sudo apt-get install -y keepalived
                ok "keepalived 설치 완료 (apt): $(keepalived --version 2>&1 | head -1)"
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
            info "/etc/keepalived/keepalived.conf 적용 — sudo 권한 필요"
            sudo cp "$out" /etc/keepalived/keepalived.conf
            info "/etc/systemd/system/cims@.service 적용"
            sudo cp "$unit" /etc/systemd/system/cims@.service
            sudo systemctl daemon-reload
            # enable per-instance — start 는 keepalived notify 가 제어 (cold-spare)
            local svc
            for svc in $(_ha_enabled_services); do
                info "systemctl enable cims@${svc}.service"
                sudo systemctl enable "cims@${svc}.service"
            done
            sudo systemctl restart keepalived
            ok "keepalived + systemd unit 적용 완료 (services start 는 keepalived notify 가 제어)"
            ;;
        start)  sudo systemctl start  keepalived ;;
        stop)   sudo systemctl stop   keepalived ;;
        status) systemctl status keepalived --no-pager || true ;;
        uninstall)
            # agent uninstall 대칭 — install 이 시스템에 깐 것을 모두 제거.
            # keepalived purge + autoremove deps (시스템 다른 곳에서 안 쓰는 것만) + /etc/keepalived/
            # + HA_DIR/out (옛 sudo 호출로 생긴 root 소유 render 잔재).
            if ! command -v keepalived >/dev/null 2>&1; then
                info "keepalived 미설치 — skip"
                sudo rm -rf /etc/keepalived "$HA_DIR/out" 2>/dev/null || true
                return 0
            fi
            # broken deps (예: libsnmp40t64) 가 있으면 후속 purge 가 실패하고 /etc/keepalived rm 도 못함.
            # --fix-broken install 선행 — 시스템에 broken 없으면 NO-OP.
            if sudo dpkg --audit 2>/dev/null | grep -q .; then
                info "broken deps 발견 — apt-get --fix-broken install 자동 선행"
                sudo apt-get -y --fix-broken install || warn "fix-broken install 실패 — purge 단계 실패 시 수동 정리 필요"
            fi
            info "keepalived 제거 (purge + autoremove)"
            sudo systemctl stop keepalived 2>/dev/null || true
            sudo apt-get -y purge keepalived || {
                err "apt-get purge keepalived 실패"
                return 1
            }
            sudo apt-get -y autoremove --purge || true
            sudo rm -rf /etc/keepalived "$HA_DIR/out" 2>/dev/null || true
            ok "keepalived + autoremoved deps + /etc/keepalived + out/ 제거"
            ;;
        help|*)
            cat <<EOF
사용법: cims-ha <subcommand>

  install         keepalived 패키지 설치 (vendor deb 우선, apt fallback)
  uninstall       keepalived + autoremove deps + /etc/keepalived 제거 (install 대칭)
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
