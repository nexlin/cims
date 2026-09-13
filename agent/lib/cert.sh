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
# 단말 대면 인증서(CSC 4430·CSP TLS 접속점)도 같은 축이 맡는다 (sip_tls_signaling.md §8.6.1):
#   사이트 CA = 그룹 CA 키 + 개발사 루트의 교차 인증서(`_secrets/ca/ca-cross.crt`). 교차 인증서가
#   있으면 leaf 뒤에 이어붙여 체인(2장)으로 배치하고, 단말은 루트 → 교차 인증서 → leaf 로 검증한다.
#   관리평면은 종전대로 자가서명 `ca.crt` 를 앵커로 쓰므로 두 평면의 앵커는 섞이지 않는다.
#   교차 인증서가 없는 노드는 종전과 같다(그룹 CA 단독 서명 — 관리평면만 유효).
#
# 역할 분리:
#   트러스트 앵커(그룹 CA) : 그룹 자산. 노드 로컬 0600, join 이 피어에 1회 복사(교차 인증서 포함)
#   발급·배치·갱신         : **여기** — 모듈 기동 전 보증 + agent 일일 스윕(`cims-svc cert`)
#   소비                   : 모듈 — 정해진 경로를 읽기만 한다 (관리평면 3모듈 = httpsrv 30초 mtime 감시,
#                            CSP = SIGUSR1 에 같은 경로의 내용 지문 비교로 무중단 재적재)
#
# 이 파일은 agent 배포본과 개발 서버가 **같이** 쓴다 (cims-svc 가 두 경로의 공통 엔진).

# ── 임계 (일) — 단일 정의 (sip_tls_signaling.md §8.6.2) ───────────────────────────
# 갱신 60 > 경고 30 > 위험 7. CSP `CERT_EXPIRY_*`·CSC `CertExpiryProbe`·`service-cert.sh verify`·
# cims-verify S3-HEALTH 가 같은 값을 쓴다 — 갱신 임계가 경고 임계보다 크다는 순서가
# "경고(A-PRC-009 30일)가 뜨는 것 자체가 자동 갱신 실패" 라는 뜻을 만든다.
CERT_RENEW_DAYS=60
CERT_WARN_DAYS=30
CERT_CRIT_DAYS=7
# 서버 leaf 유효기간 — 2년 (§8.1 기간 정책. Apple 825일 상한 안)
CERT_LEAF_DAYS=730

# ── 경로 유도 ────────────────────────────────────────────────────────────────
# 인증서는 모듈들이 찾는 **버전무관** 경로와 같은 규칙으로 유도한다.
#   `<component_root>/../../runtime/cert` = `$DIST_DIR/../runtime/cert`
#     배포본: <prefix>/modules/<mod>/current  → <prefix>/modules/<mod>/runtime/cert
#     개발:   <repo>/build/dist               → <repo>/build/runtime/cert
# 버전 디렉터리에 두면 업그레이드가 그 디렉터리를 갈아치우면서 인증서가 사라진다.
_node_cert_dir() {
    echo "$(dirname "$DIST_DIR")/runtime/cert"
}

# 모듈별 파일 이름. 관리평면 3모듈은 httpsrv 가 읽는 `server.{crt,key}`, CSP 계열은
# `<name>-chain.pem` / `<name>.key` — `service-cert.sh install` 과 같은 경로·이름이라 `local_nodes`
# TLS 행의 tls_cert_path/tls_key_path 계약(§8.6.1 E4)이 수동 배치와 자동 발급에서 같다.
_node_cert_names() {
    case "$1" in
        csp|psp|isp) echo "$1-chain.pem $1.key" ;;
        *)           echo "server.crt server.key" ;;
    esac
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
# 신규 생성은 RSA 3072 (§8.6.1 E6 — 이 키가 사이트 CA 키가 된다. 기존 2048 CA 는 그대로 교차 서명).
_ensure_group_ca() {
    local d crt key
    d=$(_group_ca_dir); crt="$d/ca.crt"; key="$d/ca.key"
    if [[ -f "$crt" && -f "$key" ]]; then echo "$crt $key"; return 0; fi
    mkdir -p "$d" 2>/dev/null || return 1
    chmod 700 "$d" 2>/dev/null || true
    if openssl req -x509 -newkey rsa:3072 -nodes -sha256 -days 3650 \
            -subj '/CN=CIMS-OAM-CA/O=CIMS' \
            -addext 'basicConstraints=critical,CA:TRUE' -addext 'keyUsage=critical,keyCertSign,cRLSign' \
            -keyout "$key" -out "$crt" 2>/dev/null; then
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

# ── 사이트 CA(교차 인증서) ────────────────────────────────────────────────────
# 개발사 오프라인 루트가 그룹 CA 의 공개키에 발급한 교차 인증서 `ca-cross.crt`(subject 는 그룹 CA 와 같다).
# 있고 그룹 CA 와 **같은 공개키**일 때만 사이트 CA 로 인정한다 — 키가 다르면(그룹 CA 재생성 뒤 옛 교차
# 인증서 잔존) 체인이 성립하지 않으므로 없는 것으로 본다(경고는 stderr). stdout = 교차 인증서 경로.
_site_ca_cross() {
    local d cross ca pk1 pk2
    d=$(_group_ca_dir); cross="$d/ca-cross.crt"; ca="$d/ca.crt"
    [[ -f "$cross" && -f "$ca" ]] || return 1
    pk1=$(openssl x509 -in "$cross" -noout -pubkey 2>/dev/null) || return 1
    pk2=$(openssl x509 -in "$ca" -noout -pubkey 2>/dev/null) || return 1
    if [[ -z "$pk1" || "$pk1" != "$pk2" ]]; then
        warn "사이트 CA 교차 인증서($cross)의 공개키가 그룹 CA($ca)와 다르다 — 무시한다. 루트로 다시 교차 서명해야 한다" >&2
        return 1
    fi
    echo "$cross"
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

# 첫 인증서(leaf)의 만료까지 남은 일수. 읽기 실패면 빈 출력 + 실패.
_cert_days_left() {
    local na
    na=$(openssl x509 -in "$1" -noout -enddate 2>/dev/null | cut -d= -f2) || return 1
    [[ -n "$na" ]] || return 1
    echo $(( ( $(date -d "$na" +%s) - $(date +%s) ) / 86400 ))
}

# leaf 의 발급자가 그룹 CA 인가 (issuer == 그룹 CA subject).
_cert_issuer_is_group_ca() {
    local iss subj
    iss=$(openssl x509 -in "$1" -noout -issuer 2>/dev/null | sed 's/^issuer=//')
    subj=$(openssl x509 -in "$(_group_ca_dir)/ca.crt" -noout -subject 2>/dev/null | sed 's/^subject=//')
    [[ -n "$iss" && -n "$subj" && "$iss" == "$subj" ]]
}

# PEM 파일의 n 번째 인증서 블록 / 인증서 장수.
_pem_nth() { awk -v want="$2" '/-----BEGIN CERTIFICATE-----/{n++} n==want{print} /-----END CERTIFICATE-----/{if(n==want) exit}' "$1"; }
_pem_count() { grep -c -- '-----BEGIN CERTIFICATE-----' "$1" 2>/dev/null || echo 0; }

# 체인 파일의 두 번째 장이 교차 인증서인가 — 갱신 계기 ⑤(체인 불일치) 판정.
_chain_has_cross() {
    local chain="$1" cross="$2" fp_want fp_have
    (( $(_pem_count "$chain") >= 2 )) || return 1
    fp_want=$(openssl x509 -in "$cross" -noout -fingerprint -sha256 2>/dev/null) || return 1
    fp_have=$(_pem_nth "$chain" 2 | openssl x509 -noout -fingerprint -sha256 2>/dev/null) || return 1
    [[ -n "$fp_want" && "$fp_want" == "$fp_have" ]]
}

# ── 갱신 상태 기록 ───────────────────────────────────────────────────────────
# agent 가 heartbeat 원시 metric(`cert_renew{module: …}`)으로 실어 보내고 OAM 이 A-PRC-009
# `<서버명>/agent/cert/<module>/renew` 를 파생한다 (alarm_self_reporting.md §2 — agent 는 FM push 를
# 쓰지 않는다). 자리는 pid 파일과 같은 규약의 버전 밖 `run/cert/<svc>.json`.
_cert_state_write() {   # svc ok(1|0) reason days_left cert_path
    local svc="$1" okv="$2" reason="$3" days="$4" crt="$5" d f
    [[ -n "${PID_DIR:-}" ]] || return 0
    d="$PID_DIR/cert"; mkdir -p "$d" 2>/dev/null || return 0
    f="$d/$svc.json"
    printf '{"module":"%s","ok":%s,"reason":"%s","days_left":%s,"cert":"%s","ts":%s}\n' \
        "$svc" "$([[ "$okv" == 1 ]] && echo true || echo false)" "$reason" "${days:-null}" "$crt" "$(date +%s)" \
        > "$f.tmp" 2>/dev/null && mv -f "$f.tmp" "$f" 2>/dev/null || true
}

# 발급·갱신 뒤 소비자 통지. 관리평면 3모듈은 httpsrv 가 파일 변경(mtime·size)을 30초 안에 스스로
# 감지한다. CSP 는 SIGUSR1 을 받아야 Sync 가 돌고, 거기서 같은 경로의 내용 지문이 바뀐 것을 보고
# 무중단 재적재한다(CspListenerManager). 안 떠 있으면 다음 기동이 새 파일을 읽는다.
_cert_post_issue() {
    case "$1" in
        csp|psp|isp)
            command -v read_pid >/dev/null 2>&1 || return 0
            local pid; pid=$(read_pid "$1" 2>/dev/null || true)
            if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
                kill -USR1 "$pid" 2>/dev/null && info "$1: SIGUSR1 — 인증서 무중단 재적재 요청 (pid=$pid)"
            fi ;;
    esac
    return 0
}

# ── 발급 ─────────────────────────────────────────────────────────────────────
# 그룹 CA 로 서명하고, 사이트 CA 교차 인증서가 있으면 leaf 뒤에 이어붙여 체인(2장)으로 만든다(E1).
# 루트는 싣지 않는다 — 단말이 이미 가진 앵커다(§8.2).
#   인자: cert_dir issue_san crt_name key_name allow_selfsigned(0|1)
#   self-signed 폴백은 **인증서가 아예 없을 때만** 호출부가 허용한다(E5) — 단말 대면 모듈에서
#   자가서명은 "유효한 옛 인증서"보다 나쁘다(단말 로그인 불가).
#   **제자리에 쓰지 않는다.** 모듈은 인증서 파일 변경을 감지해 핫리로드하는데, 키와 인증서를
#   순서대로 직접 쓰면 그 사이 "짝이 안 맞는" 창이 생긴다. 새로 만든 뒤 이동해 창을 렌네임 두 번으로
#   줄인다 (모듈 쪽에도 사전 검증이 있어 창에 걸려도 기존 인증서를 유지한다 — 두 겹).
_issue_node_cert() {
    local cert_dir="$1" san="$2" crt_name="${3:-server.crt}" key_name="${4:-server.key}" allow_selfsigned="${5:-0}"
    local host key crt nkey ncrt csr ext ca ca_crt ca_key cross="" rc=1 mode=""
    host=$(hostname -f 2>/dev/null || hostname)
    key="$cert_dir/$key_name"; crt="$cert_dir/$crt_name"
    nkey="$cert_dir/.new.$key_name"; ncrt="$cert_dir/.new.$crt_name"; csr="$cert_dir/.new.csr"

    ca=$(_ensure_group_ca) && [[ -n "$ca" ]] || ca=""
    if [[ -n "$ca" ]]; then
        read -r ca_crt ca_key <<< "$ca"
        cross=$(_site_ca_cross 2>/dev/null) || cross=""
        ext=$(mktemp) || return 1
        printf 'subjectAltName=%s\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' \
               "$san" > "$ext"
        if openssl req -new -newkey rsa:2048 -nodes -sha256 -subj "/CN=${host}/O=CIMS" \
                -keyout "$nkey" -out "$csr" 2>/dev/null \
           && openssl x509 -req -in "$csr" -CA "$ca_crt" -CAkey "$ca_key" \
                -CAcreateserial -days "$CERT_LEAF_DAYS" -sha256 -extfile "$ext" -out "$ncrt" 2>/dev/null; then
            rc=0
            mode="그룹 CA 서명"
            if [[ -n "$cross" ]]; then
                if cat "$cross" >> "$ncrt" 2>/dev/null; then
                    mode="사이트 CA 체인(leaf+교차 인증서)"
                else
                    warn "교차 인증서 이어붙이기 실패 — 발급 중단 ($cross)"; rc=1
                fi
            fi
        else
            warn "그룹 CA 서명 실패 ($ca_crt)"
        fi
        rm -f "$csr" "$ext" 2>/dev/null || true
    fi

    if (( rc != 0 )) && (( allow_selfsigned )); then
        if openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
                -subj "/CN=${host}/O=CIMS" -addext "subjectAltName=${san}" \
                -keyout "$nkey" -out "$ncrt" 2>/dev/null; then
            rc=0
            mode="self-signed — 그룹 CA 불가"
        fi
    fi

    if (( rc != 0 )); then
        rm -f "$nkey" "$ncrt" 2>/dev/null || true
        return 1
    fi
    chmod 600 "$nkey" 2>/dev/null || true
    chmod 644 "$ncrt" 2>/dev/null || true
    mv -f "$nkey" "$key" && mv -f "$ncrt" "$crt" || {
        rm -f "$nkey" "$ncrt" 2>/dev/null || true
        return 1
    }
    ok "노드 TLS 인증서 발급 (${mode}, CN=${host}, SAN=${san}) → ${crt}"
    return 0
}

# ── 진입점 ───────────────────────────────────────────────────────────────────
# 노드 인증서 보증 — 모듈 기동 전(lifecycle.sh)과 agent 일일 스윕(`cims-svc cert`)이 같은 함수를 부른다.
# 갈래:
#   1) 없음                        → 발급 (CA 불가면 self-signed 폴백 — 인증서가 아예 없을 때만)
#   2) 있고 CIMS 발행 + 갱신 계기  → 재발급. 계기 = SAN 부족(VIP 추가·주소 변경 추종) /
#                                    ④ 잔여 ≤ CERT_RENEW_DAYS / ⑤ 교차 인증서가 있는데 체인이 그것을 거치지 않음
#                                    (루트 직서명 leaf·그룹 CA 단독 leaf 를 사이트 CA 체인으로 자동 전환)
#   3) 있고 운영자 인증서          → 손대지 않는다 (SAN 부족이면 경고만)
# 강등 금지(E5): 유효한 기존 인증서가 있으면 재발급 실패 시 기존을 유지하고 갱신 실패로 기록한다.
#   교차 인증서가 없는데 leaf 가 그룹 CA 밖에서 발급된 것(루트 직서명 등)이면 재발급 자체를 하지 않는다 —
#   그룹 CA 단독 leaf 로 바꾸면 단말 로그인이 끊긴다. 이 경우도 갱신 실패(site_ca_missing)다.
# openssl 부재·발급 실패는 경고만 남기고 통과한다 — 모듈 자체 폴백이 뒤를 받치므로 기동을 막지 않는다.
ensure_node_cert() {
    local svc="${1:-}" cert_dir crt_name key_name key crt want_san issue_san miss days="" reason="" cross=""
    cert_dir=$(_node_cert_dir)
    read -r crt_name key_name <<< "$(_node_cert_names "$svc")"
    key="$cert_dir/$key_name"; crt="$cert_dir/$crt_name"

    if ! command -v openssl >/dev/null 2>&1; then
        [[ -f "$key" && -f "$crt" ]] || \
            warn "openssl 없음 — ${svc} TLS 인증서 자동 발급 건너뜀 (평문 기동 가능)"
        return 0
    fi
    mkdir -p "$cert_dir" 2>/dev/null || { warn "cert 디렉토리 생성 실패: $cert_dir"; return 0; }
    want_san=$(_node_cert_san)
    # 발급 SAN = 요구 목록 + 모듈 고정 이름(service-cert.sh issue 와 동일). 요구 목록에는 넣지 않는다 —
    #   기존 인증서에 없다는 이유만으로 재발급을 일으키지 않게.
    issue_san="$want_san"
    case "$svc" in
        csc)         issue_san="${want_san},DNS:csc.cims.local" ;;
        csp|psp|isp) issue_san="${want_san},DNS:csp.cims.local" ;;
    esac
    cross=$(_site_ca_cross) || cross=""

    # 1) 없음 → 발급
    if [[ ! -f "$key" || ! -f "$crt" ]]; then
        if _issue_node_cert "$cert_dir" "$issue_san" "$crt_name" "$key_name" 1; then
            days=$(_cert_days_left "$crt") || days=""
            _cert_state_write "$svc" 1 issued "$days" "$crt"
            _cert_post_issue "$svc"
        else
            warn "TLS 인증서 발급 실패 — ${svc} 가 평문으로 뜰 수 있습니다 ($cert_dir)"
            _cert_state_write "$svc" 0 issue_failed "" "$crt"
        fi
        return 0
    fi

    # 3) 운영자 인증서 → 손대지 않는다
    if ! _cert_cims_managed "$crt"; then
        miss=$(_cert_san_missing "$crt" "$want_san")
        [[ -n "${miss// /}" ]] && \
            warn "${svc}: 인증서 SAN 에 ${miss%% }가 없습니다. 운영자 인증서로 판단해 재발급하지 "\
"않습니다 — 그 주소(VIP 등)로 접속하면 브라우저 경고가 납니다."
        return 0
    fi

    # 2) 갱신 계기 판정
    miss=$(_cert_san_missing "$crt" "$want_san")
    days=$(_cert_days_left "$crt") || days=""
    if [[ -n "${miss// /}" ]]; then
        reason="san"
    elif [[ -n "$days" ]] && (( days <= CERT_RENEW_DAYS )); then
        reason="expiry"
    elif [[ -n "$cross" ]] && ! _chain_has_cross "$crt" "$cross"; then
        reason="chain"
    fi
    if [[ -z "$reason" ]]; then
        _cert_state_write "$svc" 1 ok "$days" "$crt"
        return 0
    fi

    if [[ -z "$cross" ]] && ! _cert_issuer_is_group_ca "$crt"; then
        warn "${svc}: 인증서 갱신 계기(${reason}${days:+, 잔여 ${days}일})가 있으나 사이트 CA 교차 인증서"\
"($(_group_ca_dir)/ca-cross.crt)가 없어 재발급하지 않습니다 — 지금 leaf 는 그룹 CA 밖에서 발급된 단말 대면 "\
"인증서라 그룹 CA 단독 leaf 로 바꾸면 단말이 끊깁니다. 기존 인증서 유지 (A-PRC-009 cert/${svc}/renew)"
        _cert_state_write "$svc" 0 site_ca_missing "$days" "$crt"
        return 0
    fi

    info "${svc}: 인증서 재발급 — 계기=${reason}${miss:+ (SAN 부족: ${miss%% })}${days:+, 잔여 ${days}일}"\
"$([[ -n "$cross" ]] && echo ', 사이트 CA 체인' || echo ', 그룹 CA')"
    if _issue_node_cert "$cert_dir" "$issue_san" "$crt_name" "$key_name" 0; then
        days=$(_cert_days_left "$crt") || days=""
        _cert_state_write "$svc" 1 renewed "$days" "$crt"
        _cert_post_issue "$svc"
    else
        warn "재발급 실패 — 기존 인증서 유지 ($crt)"
        _cert_state_write "$svc" 0 renew_failed "$days" "$crt"
    fi
    return 0
}

# ── 스윕 진입 — `cims-svc cert [svc|all]` ────────────────────────────────────
# agent 가 매일 1회 모듈별로 부른다(E3 — 기동 전 보증만으로는 2년 무재기동 노드의 만료를 못 막는다).
# all = 이 DIST_DIR 에 있는 모듈 (배포본은 모듈당 DIST_DIR 이 따로라 하나씩, 개발 dist 는 넷 전부).
cert_ensure_modules() {
    local svc list=()
    if [[ $# -eq 0 || "$1" == all ]]; then
        for svc in oam oam-svc csc csp psp isp; do [[ -d "$DIST_DIR/$svc" ]] && list+=("$svc"); done
    else
        list=("$@")
    fi
    if (( ${#list[@]} == 0 )); then warn "인증서 보증 대상 모듈 없음 (DIST_DIR=$DIST_DIR)"; return 0; fi
    for svc in "${list[@]}"; do ensure_node_cert "$svc"; done
    return 0
}
