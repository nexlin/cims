#!/usr/bin/env bash
# agent/lib/cert.sh — 노드 TLS 인증서 수명주기 (lifecycle.sh 가 source)
#
# **발급은 모듈이 아니라 lifecycle 엔진의 책임이다** (oam_ha.md §5.2). 관리평면 모듈
# (oam/oam-svc/csc)은 전부 HTTPS 로 뜨는 것이 전제다 — agent 의 health-gate 가 HTTPS
# 전용이고, 게이트웨이는 업스트림을 https 로 등록한다. 그런데 발급 주체가 oam 자신이면
# **부트스트랩 순환**이 생긴다: oam 은 자기 기동 끝자락에 인증서를 만들므로, 그 사이에
# 뜬 oam-svc 는 cert 를 못 찾고 평문으로 bind 한 뒤 다시 확인하지 않는다. 그 결과
# 게이트웨이의 모든 서비스·성능 API 가 `RECORD_LAYER_FAILURE` 로 죽는다 (실측: 첫 승격
# 노드에서 oam-svc 가 oam 보다 2초 먼저 떠 그대로 재현. 기동 순서는 set 순회라 절체마다
# 달라진다).
#
# 역할 분리:
#   트러스트 앵커(그룹 CA) : 그룹 자산. 노드 로컬 0600, join 이 피어에 1회 복사
#   발급·배치              : **여기** — 모듈 기동 전, 필요 SAN 을 담아 CA 로 서명
#   소비                   : 모듈 — 정해진 경로를 읽기만 한다
#
# 이 파일은 agent 배포본과 개발 서버가 **같이** 쓴다 (cims-svc 가 두 경로의 공통 엔진).

# ── 경로 유도 ────────────────────────────────────────────────────────────────
# 인증서는 모듈들이 찾는 **버전무관** 경로와 같은 규칙으로 유도한다.
#   `<component_root>/../../runtime/cert` = `$DIST_DIR/../runtime/cert`
#     배포본: <prefix>/modules/<mod>/current  → <prefix>/modules/<mod>/runtime/cert
#     개발:   <repo>/build/dist               → <repo>/build/runtime/cert
# 버전 디렉터리에 두면 업그레이드가 그 디렉터리를 갈아치우면서 인증서가 사라진다.
_node_cert_dir() {
    echo "$(dirname "$DIST_DIR")/runtime/cert"
}

# 그룹 CA·oam 설정은 **노드 단위**라 oam 모듈 트리에서 찾는다 (services/paths.py
# secrets_dir 과 같은 자리 — join 이 여기에 CA 를 심는다).
#   배포본: <prefix>/modules/oam/current   ← 어느 모듈을 띄우든 같은 곳
#   개발:   <repo>/build/dist
_oam_module_root() {
    local m="$(dirname "$(dirname "$DIST_DIR")")/oam/current"
    if [[ -d "$m" ]]; then echo "$m"; else echo "$DIST_DIR"; fi
}

_group_ca_dir() {
    echo "$(dirname "$(_oam_module_root)")/runtime/_secrets/ca"
}

# ── 그룹 CA ──────────────────────────────────────────────────────────────────
# 없으면 만든다. **join 이 심어둔 CA 가 있으면 그대로 쓴다** — 두 노드가 같은 CA 여야
# 절체로 노드가 바뀌어도 브라우저가 CA 하나만 신뢰하면 된다. 성공 시 "crt key" 를 출력.
_ensure_group_ca() {
    local d crt key
    d=$(_group_ca_dir); crt="$d/ca.crt"; key="$d/ca.key"
    if [[ -f "$crt" && -f "$key" ]]; then echo "$crt $key"; return 0; fi
    mkdir -p "$d" 2>/dev/null || return 1
    chmod 700 "$d" 2>/dev/null || true
    if openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
            -subj '/CN=CIMS-OAM-CA/O=CIMS' -keyout "$key" -out "$crt" 2>/dev/null; then
        chmod 600 "$key" 2>/dev/null || true
        # 로그는 **stderr 로** — 이 함수의 stdout 은 "crt key" 반환값이다. 섞이면 호출부가
        # 로그 문구를 경로로 읽어 CA 서명이 조용히 self-signed 로 내려간다.
        ok "그룹 CA 생성: $d — 두 번째 노드에는 join 이 이 CA 를 복사한다" >&2
        echo "$crt $key"
        return 0
    fi
    rm -f "$crt" "$key" 2>/dev/null || true
    return 1
}

# ── 필요 SAN ─────────────────────────────────────────────────────────────────
# hostname · loopback · 노드 IPv4 · HA VIP · oam 설정의 접속 주소(AgentOamUrl)·CertSans.
#   VIP 를 처음부터 넣는 이유: 빠지면 접속 주소가 SAN 에 없어 브라우저 경고가 나고,
#   나중에 재발급하면 이미 뜬 모듈은 옛 인증서를 계속 서빙한다(핫리로드 없음).
_node_cert_san() {
    local host san ip vip
    host=$(hostname -f 2>/dev/null || hostname)
    san="DNS:${host},IP:127.0.0.1"
    while read -r ip; do
        [[ -z "$ip" || "$ip" == 127.* ]] && continue
        [[ ",$san," == *",IP:${ip},"* ]] && continue
        san="${san},IP:${ip}"
    done < <(ip -4 -o addr show scope global 2>/dev/null | awk '{split($4,a,"/"); print a[1]}')

    # HA VIP — agent 가 update_ha 로 기록한 ha.json(버전 트리 밖). 없으면 건너뛴다.
    #   배포본: <prefix>/modules/<mod>/current → <prefix>/run/…  (두 단계 위)
    #   개발:   <repo>/build/dist             → <repo>/run/…     (한 단계 위)
    local ha
    for ha in "$(dirname "$(dirname "$DIST_DIR")")/../run/keepalived/ha.json" \
              "$(dirname "$DIST_DIR")/../run/keepalived/ha.json" \
              "/opt/cims-agent/run/keepalived/ha.json"; do
        [[ -f "$ha" ]] || continue
        while read -r vip; do
            [[ -z "$vip" ]] && continue
            [[ ",$san," == *",IP:${vip},"* ]] && continue
            san="${san},IP:${vip}"
        done < <("${PYBIN:-python3}" -c "
import json, sys
try:
    cfg = json.load(open(sys.argv[1]))
except Exception:
    raise SystemExit(0)
for s in (cfg.get('services') or {}).values():
    for v in (s.get('vips') or []):
        if isinstance(v, dict) and v.get('ip'):
            print(v['ip'])
    if s.get('vip'):
        print(s['vip'])
" "$ha" 2>/dev/null)
        break
    done

    # oam 배포 설정 — 접속 주소(AgentOamUrl 의 host) + 운영자 지정 CertSans.
    # overlay(config.json, 평면 점표기)가 정본이고 없으면 패키지 기본값을 본다.
    local oam_root entry
    oam_root=$(_oam_module_root)
    while read -r entry; do
        [[ -z "$entry" ]] && continue
        [[ ",$san," == *",${entry},"* ]] && continue
        san="${san},${entry}"
    done < <("${PYBIN:-python3}" -c "
import ipaddress, json, os, sys
from urllib.parse import urlparse
root = sys.argv[1]
srv, out = {}, []
for p in (os.path.join(root, 'oam', 'config', 'oam.json'),
          os.path.join(root, 'oam', 'config.json')):
    try:
        d = json.load(open(p))
    except Exception:
        continue
    if not isinstance(d, dict):
        continue
    s = d.get('Server')
    if isinstance(s, dict):
        srv.update(s)
    for k, v in d.items():                      # 평면 점표기 overlay
        if k.startswith('Server.'):
            srv[k.split('.', 1)[1]] = v
cand = []
aou = str(srv.get('AgentOamUrl') or '').strip()
if aou:
    try:
        h = urlparse(aou).hostname
        if h:
            cand.append(h)
    except Exception:
        pass
extra = srv.get('CertSans')
if isinstance(extra, str):
    extra = extra.split(',')
if isinstance(extra, list):
    cand += [str(x).strip() for x in extra]
for c in cand:
    c = (c or '').strip()
    if not c:
        continue
    try:
        ipaddress.ip_address(c)
        out.append('IP:' + c)
    except ValueError:
        out.append('DNS:' + c)
print('\n'.join(out))
" "$oam_root" 2>/dev/null)
    echo "$san"
}

# ── 기존 인증서 판정 ─────────────────────────────────────────────────────────
# CIMS 가 만든 인증서인가 — 운영자가 넣은 상용 인증서는 절대 건드리지 않기 위한 기준.
_cert_cims_managed() {
    local info
    info=$(openssl x509 -in "$1" -noout -subject -issuer 2>/dev/null) || return 1
    [[ "$info" == *"O=CIMS"* || "$info" == *"O = CIMS"* || "$info" == *"CIMS-OAM-CA"* ]]
}

# 필요한 SAN 중 인증서에 없는 것들을 출력 (없으면 빈 출력).
_cert_san_missing() {
    local crt="$1" want="$2" have e miss=""
    have=$(openssl x509 -in "$crt" -noout -ext subjectAltName 2>/dev/null)
    IFS=',' read -ra _w <<< "$want"
    for e in "${_w[@]}"; do
        case "$e" in
            IP:*)  [[ "$have" == *"IP Address:${e#IP:}"* ]] || miss+="${e} " ;;
            DNS:*) [[ "$have" == *"DNS:${e#DNS:}"* ]]       || miss+="${e} " ;;
        esac
    done
    echo "$miss"
}

# ── 발급 ─────────────────────────────────────────────────────────────────────
# 그룹 CA 로 서명한다. CA 를 못 만들면 self-signed 로 내려간다 — 기동을 막지 않는 것이
# 우선이고, 그 경우 브라우저 경고만 감수한다.
#   **제자리에 쓰지 않는다.** 모듈은 인증서 파일 변경을 감지해 핫리로드하는데(§5.2),
#   키와 인증서를 순서대로 직접 쓰면 그 사이 "짝이 안 맞는" 창이 생긴다. 새로 만든 뒤
#   이동해 창을 렌네임 두 번으로 줄인다 (모듈 쪽에도 사전 검증이 있어 창에 걸려도
#   기존 인증서를 유지한다 — 두 겹).
_issue_node_cert() {
    local cert_dir="$1" san="$2" host key crt nkey ncrt csr ext ca ca_crt ca_key rc=1
    host=$(hostname -f 2>/dev/null || hostname)
    key="$cert_dir/server.key"; crt="$cert_dir/server.crt"
    nkey="$cert_dir/.new.key"; ncrt="$cert_dir/.new.crt"; csr="$cert_dir/.new.csr"

    ca=$(_ensure_group_ca) && [[ -n "$ca" ]] || ca=""
    if [[ -n "$ca" ]]; then
        read -r ca_crt ca_key <<< "$ca"
        ext=$(mktemp) || return 1
        printf 'subjectAltName=%s\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' \
               "$san" > "$ext"
        if openssl req -new -newkey rsa:2048 -nodes -subj "/CN=${host}/O=CIMS" \
                -keyout "$nkey" -out "$csr" 2>/dev/null \
           && openssl x509 -req -in "$csr" -CA "$ca_crt" -CAkey "$ca_key" \
                -CAcreateserial -days 825 -extfile "$ext" -out "$ncrt" 2>/dev/null; then
            rc=0
        else
            warn "그룹 CA 서명 실패 — self-signed 로 대체합니다"
        fi
        rm -f "$csr" "$ext" 2>/dev/null || true
    fi

    if (( rc != 0 )); then
        if openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
                -subj "/CN=${host}/O=CIMS" -addext "subjectAltName=${san}" \
                -keyout "$nkey" -out "$ncrt" 2>/dev/null; then
            rc=0
            ca=""                       # 로그 문구 구분용 (self-signed)
        fi
    fi

    if (( rc != 0 )); then
        rm -f "$nkey" "$ncrt" 2>/dev/null || true
        return 1
    fi
    chmod 600 "$nkey" 2>/dev/null || true
    mv -f "$nkey" "$key" && mv -f "$ncrt" "$crt" || {
        rm -f "$nkey" "$ncrt" 2>/dev/null || true
        return 1
    }
    ok "노드 TLS 인증서 발급 ($([[ -n "$ca" ]] && echo '그룹 CA 서명' || echo 'self-signed'), CN=${host}, SAN=${san})"
    return 0
}

# ── 진입점 ───────────────────────────────────────────────────────────────────
# 기동 전 노드 인증서 보증. 세 갈래다:
#   1) 없음                      → 발급
#   2) 있고 SAN 부족 + CIMS 발행 → 재발급 (VIP 추가·주소 변경 추종)
#   3) 있고 운영자 인증서        → 손대지 않는다 (SAN 부족이면 경고만)
# openssl 부재·발급 실패는 경고만 남기고 통과한다 — 모듈 자체 폴백이 뒤를 받치므로
# 기동을 막지 않는다.
ensure_node_cert() {
    local svc="${1:-}" cert_dir key crt san miss
    cert_dir=$(_node_cert_dir)
    key="$cert_dir/server.key"; crt="$cert_dir/server.crt"

    if ! command -v openssl >/dev/null 2>&1; then
        [[ -f "$key" && -f "$crt" ]] || \
            warn "openssl 없음 — ${svc} TLS 인증서 자동 발급 건너뜀 (평문 기동 가능)"
        return 0
    fi
    mkdir -p "$cert_dir" 2>/dev/null || { warn "cert 디렉토리 생성 실패: $cert_dir"; return 0; }
    san=$(_node_cert_san)

    if [[ ! -f "$key" || ! -f "$crt" ]]; then
        _issue_node_cert "$cert_dir" "$san" || \
            warn "TLS 인증서 발급 실패 — ${svc} 가 평문으로 뜰 수 있습니다 ($cert_dir)"
        return 0
    fi

    miss=$(_cert_san_missing "$crt" "$san")
    [[ -z "${miss// /}" ]] && return 0

    if _cert_cims_managed "$crt"; then
        info "${svc}: 인증서 SAN 부족(${miss%% }) — 그룹 CA 로 재발급"
        _issue_node_cert "$cert_dir" "$san" || \
            warn "재발급 실패 — 기존 인증서 유지 ($cert_dir)"
    else
        warn "${svc}: 인증서 SAN 에 ${miss%% }가 없습니다. 운영자 인증서로 판단해 재발급하지 "\
"않습니다 — 그 주소(VIP 등)로 접속하면 브라우저 경고가 납니다."
    fi
    return 0
}
