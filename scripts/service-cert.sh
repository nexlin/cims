#!/usr/bin/env bash
# =============================================================
# scripts/service-cert.sh — 단말 대면(Service CA) 서버 인증서 발급·배치·검증
#
# 배경: 단말(Android APK·Windows 관제조작반·cimsue-cli)은 **CIMS Service CA 한 장**만
# 신뢰한다. 새 노드에 패키지를 설치하면 CSC 는 agent 가 그룹 CA(CIMS-OAM-CA)로 자동
# 발급한 인증서를, CSP 는 동봉 자가서명(cert/csp.pem)을 쓰므로 단말이 TLS 핸드셰이크에서
# 거절한다 — 로그인(HTTPS 4430)부터 막히고 CSC 로그에는 흔적도 남지 않는다
# (sip_tls_signaling.md §8.1). 이 스크립트는 그 노드용 leaf 를 Service CA 로 발급해
# **묶음(bundle)** 으로 만들고, 현장에서 배치·검증하는 절차를 한 파일로 제공한다.
#
# CA 개인키는 CA 보관 서버를 떠나지 않는다 — `issue` 만 CA 키를 쓰고, 묶음에는 CA **인증서**
# 와 노드 leaf/키만 들어간다. 묶음에는 이 스크립트 자신이 복사되므로 현장에서는 묶음만
# 있으면 된다(소스 트리·네트워크 불필요, openssl 만 있으면 된다).
#
# 서브커맨드 (실행 위치):
#   collect [--prefix P]                 대상 노드 — 필요한 SAN 목록 출력 (agent 자동 발급 규칙과 동일)
#   issue   --ip IP [--vip IP]           CA 보관 서버 — ssh 로 대상 노드의 HOST/SAN 을 자동 수집해 leaf 2장(csc·csp)
#                                        발급 → /home/cims/certs/cert-init-<host>/ + tgz (있으면 덮어씀). ssh 불가 시 --host/--san
#   install [--prefix P] [--bundle D]    대상 노드 — runtime/cert 배치(백업 동반) + SAN 사전 검사 + 핫리로드 확인
#   csp-node [--oam URL] [--port N]      대상 노드 — OAM API 로 CSP local_nodes TLS 행의 tls_cert_path/tls_key_path 를
#                                        배치 경로로 저장(콘솔 저장과 동일 경로, SIGUSR1 무중단). 행이 없으면 --port 로 생성
#   verify  --ip IP [--csc-port N] [--csp-port N] [--ca FILE]
#                                        어디서든 — 체인 전송·발급자·IP 신원·음성 대조군 판정
#   push    --ssh user@host [--prefix P] CA 서버 → 대상 노드 scp + install (ssh 가 열려 있을 때의 편의)
#
# SAN 이 agent 요구 목록(`agent/lib/cert.sh` _node_cert_san: hostname·127.0.0.1·노드 IPv4 전부·
# VIP·AgentOamUrl host·CertSans)의 **상위집합**이어야 재기동 때 그룹 CA 인증서로 덮이지
# 않는다 — `collect` 가 그 목록을 뽑고 `install` 이 배치 전에 대조해 부족하면 거절한다.
#
# 이 파일은 묶음으로 단독 배포되므로 scripts/lib/common.sh 를 source 하지 않는다
# (출력 규약은 같다).
# =============================================================
set -uo pipefail

RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
CYAN=$'\033[0;36m'; BOLD=$'\033[1m'; NC=$'\033[0m'
info()   { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()     { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()   { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()    { echo -e "${RED}[ERROR]${NC} $*" >&2; }
header() { echo -e "\n${BOLD}$*${NC}"; }
die()    { err "$@"; exit 1; }

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]:-$0}")"
SCRIPT_DIR="$(dirname "$SCRIPT_PATH")"

DEFAULT_PREFIX=/opt/cims-agent/modules          # 배포본. 개발 레이아웃은 build/dist/<server>
DEFAULT_CA_DIR=/home/cims/certs
DEFAULT_CA_NAME=cims-service-ca
DEFAULT_DAYS=730
DEFAULT_CSC_PORT=4430
DEFAULT_CSP_PORT=15061

usage() {
    cat <<'USAGE'
사용법: service-cert.sh <collect|issue|install|verify|push> [옵션]

  collect [--prefix P]
      대상 노드에서 실행. 필요한 SAN 목록(agent 자동 발급 규칙과 동일)을 출력한다.
      출력의 HOST/SAN 을 issue 에 그대로 넘긴다.

  issue --ip IP [--vip IP] [--days N]
      CA 보관 서버에서 실행. 보통은 이것으로 끝이다 — ssh 로 대상 노드의 hostname·주소를 읽어 발급하고,
      /home/cims/certs/cert-init-<host>/ 와 cert-init-<host>.tgz 를 만든다(있으면 덮어쓴다).
        --vip IP        HA 대표 주소를 쓰면 함께 넣는다 (여러 개면 쉼표)
        --days N        유효기간(기본 730)
        --runbook FILE  노드 전용 절차 문서를 묶음에 RUNBOOK-<host>.md 로 동봉
      드물게 쓰는 것: --host H --san LIST (ssh 불가 시 collect 출력을 수동 전달) · --extra-san LIST(IP:/DNS: 접두)
      · --cn IP · --ca-dir D · --ca-name N · --ssh-user U · --out DIR · --keep(덮어쓰기 금지)

  install [--prefix P] [--bundle D] [--csc-only|--csp-only] [--skip-san-check]
          [--ip IP] [--wait N|--no-wait]
      대상 노드에서 서비스 계정으로 실행(묶음 안의 이 스크립트). 배치 전 노드 SAN 을 대조하고,
      CSC runtime/cert 를 백업 후 교체, CSP runtime/cert 에 체인·키를 둔다. CSC 핫리로드를 기다린다.
      --prefix 기본 /opt/cims-agent/modules (개발 레이아웃은 build/dist/<server>).

  csp-node [--oam URL(https://127.0.0.1:4419)] [--user U(admin)] [--port N] [--bind-ip IP] [--prefix P]
           [--deployment ID] [--dry-run]
      대상 노드에서 install 뒤에 실행. OAM 에 로그인해(비밀번호는 프롬프트 또는 CIMS_OAM_PASSWORD) csp 배포의
      local_nodes 를 읽고, TLS 행의 tls_cert_path/tls_key_path 를 배치 경로로 바꿔 저장한다 — 콘솔에서 저장하는
      것과 같은 API 라 SIGUSR1 로 무중단 반영된다. TLS 행이 없으면 --port 로 새 행(access-tls)을 만든다.
      --dry-run 은 바뀔 내용만 보여 주고 저장하지 않는다. 끝나면 그 포트로 verify 를 돌린다.

  verify [--ip IP] [--csc-port N(4430)] [--csp-port N(15061, 0=생략)] [--ca FILE] [--wait N]
      어느 장비에서든 실행. 묶음 디렉터리 안의 스크립트로 돌리면 --ip 는 묶음에서 읽는다.
      포트마다 체인 2장 전송·발급자=Service CA·IP 신원 OK·틀린 이름 거절을 판정한다. FAIL 이 하나라도 있으면 종료코드 1.

  push --ssh user@host [--prefix P] [--bundle D|tgz] [--remote-dir D] [--csp-port N]
      CA 서버에서 대상 노드로 묶음을 scp 하고 원격 install 을 실행한 뒤 verify 한다.
USAGE
}

# ── 공용 ─────────────────────────────────────────────────────────────────────
_need() { command -v "$1" >/dev/null 2>&1 || die "$1 이 필요합니다"; }

# 콤마 목록 정규화 — 공백 제거·중복 제거(순서 유지)
_san_norm() {
    local out="" e
    IFS=',' read -ra _e <<< "$1"
    for e in "${_e[@]}"; do
        e="${e// /}"; [[ -z "$e" ]] && continue
        [[ ",$out," == *",$e,"* ]] && continue
        out="${out:+$out,}$e"
    done
    echo "$out"
}

# 인증서의 SAN 을 "IP:x,DNS:y" 형식으로
_cert_san() {
    openssl x509 -in "$1" -noout -ext subjectAltName 2>/dev/null \
        | tail -n +2 | tr ',' '\n' | sed -E 's/^ *//; s/^IP Address:/IP:/' \
        | grep -E '^(IP|DNS):' | paste -sd, -
}

# 필요한 SAN 중 인증서에 없는 항목
_san_missing() {
    local have want="$2" e miss=""
    have=",$(_cert_san "$1"),"
    IFS=',' read -ra _w <<< "$want"
    for e in "${_w[@]}"; do
        [[ -z "$e" ]] && continue
        [[ "$have" == *",$e,"* ]] || miss="${miss:+$miss,}$e"
    done
    echo "$miss"
}

_first_ip() {   # SAN 목록에서 127.0.0.1 이 아닌 첫 IP
    local e; IFS=',' read -ra _e <<< "$1"
    for e in "${_e[@]}"; do
        [[ "$e" == IP:* && "$e" != IP:127.* ]] && { echo "${e#IP:}"; return 0; }
    done
    return 1
}

_days_left() {  # 인증서 만료까지 남은 일수
    local na; na=$(openssl x509 -in "$1" -noout -enddate 2>/dev/null | cut -d= -f2) || return 1
    echo $(( ( $(date -d "$na" +%s) - $(date +%s) ) / 86400 ))
}

# ── 노드 SAN 계산 ─────────────────────────────────────────────────────────────
# agent 의 cert.sh 가 있으면 **그 함수를 그대로** 써서 규칙 어긋남을 없앤다. 없으면 같은
# 입력(hostname·127.0.0.1·IPv4 전부·ha.json VIP·oam AgentOamUrl/CertSans)으로 계산한다.
_agent_cert_lib() {
    local prefix="$1" c
    for c in /opt/cims-agent/agent/current/lib/cert.sh \
             "$(dirname "$prefix")/agent/current/lib/cert.sh" \
             "$(dirname "$prefix")/agent/lib/cert.sh" \
             "$SCRIPT_DIR/../agent/lib/cert.sh"; do
        [[ -f "$c" ]] && { echo "$c"; return 0; }
    done
    return 1
}

_node_san_fallback() {
    local prefix="$1" host san ip
    host=$(hostname -f 2>/dev/null || hostname)
    san="DNS:${host},IP:127.0.0.1"
    while read -r ip; do
        [[ -z "$ip" || "$ip" == 127.* ]] && continue
        san="$san,IP:$ip"
    done < <(ip -4 -o addr show scope global 2>/dev/null | awk '{split($4,a,"/"); print a[1]}')
    local extra
    extra=$(PREFIX="$prefix" python3 - 2>/dev/null <<'PY'
import glob, ipaddress, json, os
from urllib.parse import urlparse
prefix = os.environ["PREFIX"]; out = []
for ha in (os.path.join(prefix, "..", "run", "keepalived", "ha.json"),
           "/opt/cims-agent/run/keepalived/ha.json"):
    try: cfg = json.load(open(ha))
    except Exception: continue
    for s in (cfg.get("services") or {}).values():
        for v in (s.get("vips") or []):
            if isinstance(v, dict) and v.get("ip"): out.append("IP:" + v["ip"])
        if s.get("vip"): out.append("IP:" + s["vip"])
    break
srv = {}
for p in (os.path.join(prefix, "oam", "current", "oam", "config", "oam.json"),
          os.path.join(prefix, "oam", "current", "oam", "config.json")):
    try: d = json.load(open(p))
    except Exception: continue
    if isinstance(d.get("Server"), dict): srv.update(d["Server"])
    for k, v in d.items():
        if k.startswith("Server."): srv[k.split(".", 1)[1]] = v
cand = []
h = urlparse(str(srv.get("AgentOamUrl") or "")).hostname
if h: cand.append(h)
ex = srv.get("CertSans")
if isinstance(ex, str): ex = ex.split(",")
if isinstance(ex, list): cand += [str(x).strip() for x in ex]
for c in cand:
    c = (c or "").strip()
    if not c: continue
    try: ipaddress.ip_address(c); out.append("IP:" + c)
    except ValueError: out.append("DNS:" + c)
print(",".join(out))
PY
)
    _san_norm "$san,$extra"
}

node_required_san() {   # stdout: SAN 목록. stderr: 출처
    local prefix="$1" lib san
    if lib=$(_agent_cert_lib "$prefix"); then
        san=$( DIST_DIR="$prefix/csc/current"; PYBIN=python3
               ok() { :; }; warn() { :; }; info() { :; }
               # shellcheck disable=SC1090
               source "$lib" 2>/dev/null && _node_cert_san 2>/dev/null )
        if [[ -n "$san" ]]; then
            echo "agent cert.sh ($lib)" >&2; _san_norm "$san"; return 0
        fi
    fi
    echo "자체 계산 (agent cert.sh 미발견)" >&2
    _node_san_fallback "$prefix"
}

# ── collect ──────────────────────────────────────────────────────────────────
cmd_collect() {
    local prefix="$DEFAULT_PREFIX"
    while (($#)); do case "$1" in
        --prefix) prefix="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "collect: 알 수 없는 옵션 $1" ;;
    esac; done
    local host san src
    host=$(hostname -f 2>/dev/null || hostname)
    san=$(node_required_san "$prefix" 2>/tmp/.sc_src.$$); src=$(cat /tmp/.sc_src.$$; rm -f /tmp/.sc_src.$$)
    info "SAN 출처: $src" >&2
    echo "HOST=$host"
    echo "SAN=$san"
    echo
    echo "# CA 보관 서버에서:"
    echo "#   scripts/service-cert.sh issue --host $host --san \"$san\""
}

# ── issue ────────────────────────────────────────────────────────────────────
cmd_issue() {
    _need openssl
    local host="" san="" cn="" ca_dir="$DEFAULT_CA_DIR" ca_name="$DEFAULT_CA_NAME" days="$DEFAULT_DAYS"
    local extra="" out="" force=1 ip="" ssh_user="${USER:-cims}" vip="" runbook=""
    while (($#)); do case "$1" in
        --ip) ip="$2"; shift 2 ;;
        --vip) vip="$2"; shift 2 ;;
        --runbook) runbook="$2"; shift 2 ;;
        --keep) force=0; shift ;;
        --ssh-user) ssh_user="$2"; shift 2 ;;
        --host) host="$2"; shift 2 ;;
        --san) san="$2"; shift 2 ;;
        --cn) cn="$2"; shift 2 ;;
        --ca-dir) ca_dir="$2"; shift 2 ;;
        --ca-name) ca_name="$2"; shift 2 ;;
        --days) days="$2"; shift 2 ;;
        --extra-san) extra="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --force) force=1; shift ;;                 # 기본값이라 있어도 무해
        -h|--help) usage; exit 0 ;;
        *) die "issue: 알 수 없는 옵션 $1" ;;
    esac; done
    # --ip 만 받은 경우: 대상 노드에 ssh 로 collect 를 돌려 HOST/SAN 을 얻는다 (읽기 전용 — 파일을 만들지 않는다)
    if [[ -n "$ip" && -z "$san" ]]; then
        _need ssh
        info "대상 노드 $ssh_user@$ip 에서 SAN 수집 중(ssh)…"
        local got
        got=$(timeout 30 ssh -o BatchMode=yes -o ConnectTimeout=6 "$ssh_user@$ip" 'bash -s -- collect' < "$SCRIPT_PATH" 2>/dev/null)
        local rhost rsan
        rhost=$(sed -n 's/^HOST=//p' <<< "$got" | head -1); rsan=$(sed -n 's/^SAN=//p' <<< "$got" | head -1)
        if [[ -n "$rhost" && -n "$rsan" ]]; then
            [[ -n "$host" && "$host" != "$rhost" ]] && warn "--host $host 와 노드 hostname $rhost 가 다르다 — 노드 값을 쓴다"
            host="$rhost"; san="$rsan"
            ok "수집: HOST=$host SAN=$san"
        elif [[ -n "$host" ]]; then
            warn "ssh 수집 실패 — --host 와 --ip 만으로 최소 SAN 을 만든다. 노드에 IP 가 더 있으면 install 이 거절하니 그때 출력된 명령으로 재발급한다"
            san="IP:$ip,IP:127.0.0.1,DNS:$host"
        else
            die "ssh 로 $ssh_user@$ip 에 접속할 수 없다. 대상 노드에서 'bash service-cert.sh collect' 를 직접 돌린 뒤 --host/--san 으로 넘기거나, 최소한 --host 를 함께 주라"
        fi
        [[ -n "$cn" ]] || cn="$ip"
    fi
    [[ -n "$host" && -n "$san" ]] || die "issue: --ip IP (ssh 자동 수집) 또는 --host/--san (수동) 이 필요하다"
    if [[ -n "$vip" ]]; then                      # --vip 1.2.3.4[,5.6.7.8] → IP: 접두어를 붙여 SAN 에 합류
        local v; IFS=',' read -ra _v <<< "$vip"
        for v in "${_v[@]}"; do v="${v// /}"; [[ -z "$v" ]] && continue; extra="${extra:+$extra,}IP:$v"; done
        info "VIP 추가: $vip"
    fi
    # 단말이 접속하는 IP 가 SAN 에 없으면(NAT 등) 추가한다 — 신원 검사는 접속 주소 기준이다
    if [[ -n "$ip" && ",$san," != *",IP:$ip,"* ]]; then info "SAN 에 IP:$ip 추가(접속 주소)"; san="$san,IP:$ip"; fi
    local ca_crt="$ca_dir/$ca_name.crt" ca_key="$ca_dir/$ca_name.key"
    [[ -f "$ca_crt" ]] || die "CA 인증서 없음: $ca_crt"
    [[ -r "$ca_key" ]] || die "CA 개인키 없음/읽기 불가: $ca_key (CA 보관 서버에서 실행해야 한다)"
    [[ -w "$ca_dir" ]] || die "CA 디렉터리에 쓰기 권한 필요(시리얼 파일): $ca_dir"

    san=$(_san_norm "$san")
    # agent 요구의 고정 항목 두 개는 빠져 있으면 채운다 — 없으면 재기동 때 덮인다.
    [[ ",$san," == *",DNS:$host,"* ]] || { info "SAN 에 DNS:$host 추가"; san="$san,DNS:$host"; }
    [[ ",$san," == *",IP:127.0.0.1,"* ]] || { info "SAN 에 IP:127.0.0.1 추가"; san="$san,IP:127.0.0.1"; }
    [[ -n "$cn" ]] || cn=$(_first_ip "$san") || cn="$host"
    [[ -n "$out" ]] || out="$ca_dir/cert-init-$host"
    if [[ -e "$out" ]]; then
        (( force )) || die "출력 디렉터리가 이미 있습니다: $out (--keep 을 빼면 덮어쓴다)"
        rm -rf "$out" "$out.tgz"
    fi
    umask 077
    mkdir -p "$out/csc" "$out/csp" || die "출력 디렉터리 생성 실패: $out"

    local ca_subject ca_days
    ca_subject=$(openssl x509 -in "$ca_crt" -noout -subject | sed 's/^subject=//')
    ca_days=$(_days_left "$ca_crt")
    (( ca_days > days )) || warn "CA 만료($ca_days 일)가 leaf 유효기간($days 일)보다 이르다 — 체인 PEM 의 만료 알람은 CA 기준으로 먼저 울린다"

    header "=== Service CA 발급: $host (CN=$cn, ${days}일) ==="
    info "CA: $ca_subject (남은 ${ca_days}일)"
    info "SAN: $san${extra:+,$extra}"

    local m san_m tmp chain key leaf
    tmp=$(mktemp -d) || die "mktemp 실패"
    for m in csc csp; do
        san_m=$(_san_norm "$san,DNS:$m.cims.local${extra:+,$extra}")
        key="$tmp/$m.key"; leaf="$tmp/$m.crt"
        printf 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=%s\n' \
               "$san_m" > "$tmp/$m.cnf"
        openssl req -newkey rsa:2048 -sha256 -nodes -keyout "$key" -out "$tmp/$m.csr" \
                -subj "/C=KR/O=CIMS/CN=$cn" >/dev/null 2>&1 \
            || { rm -rf "$tmp"; die "$m: 키/CSR 생성 실패"; }
        openssl x509 -req -in "$tmp/$m.csr" -CA "$ca_crt" -CAkey "$ca_key" -CAcreateserial \
                -out "$leaf" -days "$days" -sha256 -extfile "$tmp/$m.cnf" >/dev/null 2>&1 \
            || { rm -rf "$tmp"; die "$m: CA 서명 실패"; }
        openssl verify -CAfile "$ca_crt" "$leaf" >/dev/null 2>&1 \
            || { rm -rf "$tmp"; die "$m: 발급 직후 체인 검증 실패"; }
        [[ "$(openssl x509 -in "$leaf" -noout -modulus)" == "$(openssl rsa -in "$key" -noout -modulus 2>/dev/null)" ]] \
            || { rm -rf "$tmp"; die "$m: 키↔인증서 불일치"; }
        case "$m" in
            csc) chain="$out/csc/server.crt";   cp "$key" "$out/csc/server.key" ;;
            csp) chain="$out/csp/csp-chain.pem"; cp "$key" "$out/csp/csp.key" ;;
        esac
        cat "$leaf" "$ca_crt" > "$chain"
        chmod 644 "$chain"
        ok "$m: leaf 발급 → $(basename "$chain") (체인 2장) / SAN=$san_m"
    done
    local not_after; not_after=$(openssl x509 -in "$out/csc/server.crt" -noout -enddate | cut -d= -f2)
    rm -rf "$tmp"

    cp "$ca_crt" "$out/$ca_name.crt"; chmod 644 "$out/$ca_name.crt"
    cp "$SCRIPT_PATH" "$out/service-cert.sh"; chmod 755 "$out/service-cert.sh"
    # 값에 공백이 든다(CA subject·만료일) — source 가능하도록 %q 로 인용해 기록
    printf '%s=%q\n' HOST "$host" CN "$cn" SAN "$san" DAYS "$days" ISSUED_AT "$(date -Is)" \
           CA_NAME "$ca_name" CA_SUBJECT "$ca_subject" NOT_AFTER "$not_after" > "$out/bundle.env"
    _write_readme "$out" "$host" "$san" "$ca_subject" "$not_after" "$ca_name" "$cn"
    if [[ -n "$runbook" ]]; then                  # 노드 전용 runbook(설치 담당 확인 항목 등)을 함께 싣는다
        [[ -f "$runbook" ]] || die "runbook 파일 없음: $runbook"
        cp "$runbook" "$out/RUNBOOK-$host.md" && chmod 600 "$out/RUNBOOK-$host.md"
        info "runbook 동봉: RUNBOOK-$host.md"
    fi
    chmod 700 "$out"

    local tgz; tgz="$(cd "$(dirname "$out")" && pwd)/$(basename "$out").tgz"
    tar czf "$tgz" -C "$(dirname "$out")" "$(basename "$out")" || die "tgz 생성 실패"
    chmod 600 "$tgz"

    header "=== 완료 ==="
    echo "  묶음 디렉터리 : $out"
    echo "  묶음 tgz      : $tgz  (현장 반입용)"
    echo "  leaf 만료     : $not_after"
    echo
    echo "  다음: tgz 를 대상 노드로 옮겨  tar xzf $(basename "$tgz") && bash $(basename "$out")/service-cert.sh install  (README.txt 참조)"
    echo "        키 파일은 scp/USB 로만 옮기고 채팅·문서에 붙이지 않는다."
}

_write_readme() {
    local out="$1" host="$2" san="$3" ca_subject="$4" not_after="$5" ca_name="$6" cn="$7"
    local b="cert-init-$host"
    cat > "$out/README.txt" <<README
CIMS 단말 대면 TLS 인증서 묶음 — $host
발급 $(date +%F) / 발급자 $ca_subject / leaf 만료 $not_after
SAN: $san (+ DNS:csc.cims.local / DNS:csp.cims.local)
단말 접속 주소(검증 기본값): $cn

이 묶음은 노드 $host 용 서버 인증서(CSC·CSP)와 CA 인증서를 담고 있다. CA 개인키는 없다.
단말(Android APK·Windows 관제조작반)은 이 CA 만 신뢰한다. 패키지 설치 후 이 인증서를
배치하지 않으면 단말이 로그인 단계(HTTPS 4430)에서 TLS 거절로 막힌다 — CSC 로그에는
아무 흔적도 남지 않으므로 서버에서는 원인을 볼 수 없다.

파일
  csc/server.crt        CSC 인증서 체인(leaf + CA)      → <prefix>/csc/runtime/cert/server.crt
  csc/server.key        CSC 개인키 (600)                → <prefix>/csc/runtime/cert/server.key
  csp/csp-chain.pem     CSP 인증서 체인(leaf + CA)      → <prefix>/csp/runtime/cert/csp-chain.pem
  csp/csp.key           CSP 개인키 (600)                → <prefix>/csp/runtime/cert/csp.key
  $ca_name.crt   CA 인증서 — verify 와 Windows 관제조작반 CA PEM 경로용
  service-cert.sh       이 절차를 수행하는 스크립트 (install / csp-node / verify)
  <prefix> = 배포본 /opt/cims-agent/modules (개발 레이아웃은 build/dist/<server>, --prefix 로 지정)

══════════════════════════════════════════════════════════════════════════════
현장 절차 — 패키지 설치가 모두 끝난 시점부터
══════════════════════════════════════════════════════════════════════════════

0. 설치 담당에게 받아 둘 것
   - CSC 가 떠 있는지          (노드에서  ss -lnt | grep 4430  에 나오면 됨)
   - CSP TLS 포트 번호          (5061 또는 15061 — 3·4 단계의 <TLS 포트>)
   - 콘솔 admin 비밀번호        (3 단계 자동 저장용. 없으면 3-(b) 수동)
   - 노드 hostname·IP 가 위 SAN 과 같은지 (재설치로 바뀌었으면 2 단계에서 거절 → 재발급)
   - 단말이 로그인할 계정이 이 노드의 CSC 에 있는지 (DB 가 별개면 새로 넣어야 한다)

1. 묶음 반입 — 이 tgz 를 대상 노드로 옮긴다 (키가 들어 있다: scp/USB 만, 채팅·메일 금지)
     scp $b.tgz cims@<노드 IP>:~/

2. 인증서 배치 — 대상 노드에서 서비스 계정(cims)으로
     tar xzf $b.tgz
     bash $b/service-cert.sh install                 # 개발 레이아웃: --prefix <경로>
   출력 판독
     [OK] SAN 대조 통과 … [OK] CSC 가 새 인증서를 서빙한다   → 성공, 3 으로
     [WARN] CSC 핫리로드 미반영                              → 콘솔에서 CSC 재시작 요청 후 4 로
     [ERROR] SAN 에 이 노드가 요구하는 항목이 없다 (exit 2)   → 배치 안 됨. 주소가 바뀐 것.
        CA 서버에서  service-cert.sh issue --ip <노드 IP>  로 재발급 → 1 부터 다시
   하는 일: CSC 는 기존 server.{crt,key} 를 .bak.<시각> 으로 백업하고 교체(30초 내 핫리로드).
           CSP 는 runtime/cert 에 체인·키를 둔다 — 3 단계가 있어야 CSP 가 그 파일을 쓴다.

3. CSP 에 경로 알리기 — 둘 중 하나
   (a) 자동 (권장): 같은 노드에서, 콘솔 admin 비밀번호를 프롬프트에 입력
         bash $b/service-cert.sh csp-node --dry-run    # 바뀔 행·경로 미리보기 (저장 없음)
         bash $b/service-cert.sh csp-node              # 저장 + SIGUSR1 무중단 반영 + CSP TLS verify
       콘솔 저장 버튼과 같은 OAM API 를 호출한다. "TLS 행이 없다" 면  --port <TLS 포트>  를 붙인다.
       OAM 이 다른 주소면  --oam https://<IP>:4419
   (b) 수동: 콘솔 > [패키지 설정] > csp > local_nodes 의 protocol=TLS 행에 절대경로 저장
         tls_cert_path = <prefix>/csp/runtime/cert/csp-chain.pem
         tls_key_path  = <prefix>/csp/runtime/cert/csp.key

4. 서버측 최종 확인 — 어느 장비에서든(openssl 필요). 전부 PASS 여야 한다.
     bash $b/service-cert.sh verify --csp-port <TLS 포트>     # --ip 는 묶음에서 읽는다($cn)
   "틀린 이름 거절" 은 서버가 신원 검사를 집행하는지 보는 음성 대조군 — 통과해 버리면 FAIL 이다.
   그 다음 콘솔에서 CSC 를 한 번 재시작하고 같은 명령을 다시 돌린다. 발급자가 그대로
   "CIMS Service CA" 면 agent 가 덮어쓰지 않는다는 뜻 — 여기까지가 인증서 작업의 완료 지점.

5. 단말
   - 앱에서 로그아웃 → 로그인 화면 서버 주소 = $cn → 로그인. 재기동만으로는 새 서버를 받지 않는다.
   - APK 는 서버 검증이 켜진 빌드(2026-08-19 이후). 단말 시계 자동 동기(틀리면 유효기간 검사 실패).
   - 그룹 통화·PTT 는 같은 서버에 등록된 단말끼리만 된다 — 시험 단말은 전부 함께 옮긴다.
   - Windows 관제조작반: "서버 인증서 검증" 켬 + CA PEM 경로 = 이 묶음의 $ca_name.crt
   증상 판독
     로그인 즉시 실패 + CSC 로그에 /idms/authreq 없음   → 인증서. 4 단계 verify 재확인
     401 / 403                                        → 계정·비밀번호 (인증서 아님)
     로그인·프로비저닝 OK, REGISTER 실패               → 포트 불일치 (CSC 프로비저닝 포트 ↔ CSP local_nodes)
     TLS 로 바꾸면 등록 503 PJSIP_TLS_ECERTVERIF       → CSP 인증서 미교체 (3 단계 미실행)

6. 마무리·원복
   - 시험이 끝나면 단말 서버 주소를 원래 서버로 되돌린다(로그아웃 → 주소 변경 → 로그인).
   - 원복: CSC 는 <prefix>/csc/runtime/cert/server.{crt,key}.bak.<시각> 을 되돌리면 30초 내 반영.
           CSP 는 local_nodes 경로를 원래 값으로 (csp-node 실행 전 --dry-run 출력의 왼쪽 값).

주의
  - server.key / csp.key 는 600 유지. 채팅·문서에 붙이지 않는다.
  - 인증서만 바뀌는 교체는 CSC·CSP 모두 무중단. bind 주소·포트가 바뀌면 CSP 재기동이 필요하다.
  - 만료 30일 전 A-PRC-009 warning, 7일 전 critical. 갱신은 같은 절차(issue → install → csp-node)다.
정본: docs/design/features/sip_tls_signaling.md §8 · docs/user-manual/initial_install.md §4.5
README
}

# ── install ──────────────────────────────────────────────────────────────────
_load_bundle() {
    local b="$1"
    [[ -f "$b/bundle.env" ]] || return 1
    # shellcheck disable=SC1091
    source "$b/bundle.env"
    [[ -f "$b/csc/server.crt" && -f "$b/csc/server.key" && -f "$b/csp/csp-chain.pem" && -f "$b/csp/csp.key" ]]
}

_fp() { openssl x509 -in "$1" -noout -fingerprint -sha256 2>/dev/null | cut -d= -f2; }

_served_fp() {   # ip port → 서빙 중인 leaf fingerprint (접속 실패면 빈 문자열)
    timeout 6 openssl s_client -connect "$1:$2" </dev/null 2>/dev/null \
        | openssl x509 -noout -fingerprint -sha256 2>/dev/null | cut -d= -f2
}

cmd_install() {
    _need openssl
    local prefix="$DEFAULT_PREFIX" bundle="$SCRIPT_DIR" only="" skip_san=0 ip="" wait_s=60
    while (($#)); do case "$1" in
        --prefix) prefix="$2"; shift 2 ;;
        --bundle) bundle="$2"; shift 2 ;;
        --csc-only) only=csc; shift ;;
        --csp-only) only=csp; shift ;;
        --skip-san-check) skip_san=1; shift ;;
        --ip) ip="$2"; shift 2 ;;
        --wait) wait_s="$2"; shift 2 ;;
        --no-wait) wait_s=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "install: 알 수 없는 옵션 $1" ;;
    esac; done
    prefix="${prefix%/}"
    _load_bundle "$bundle" || die "묶음이 아닙니다(bundle.env·csc/·csp/ 필요): $bundle"
    [[ -d "$prefix" ]] || die "prefix 디렉터리 없음: $prefix (--prefix 로 지정. 배포본=/opt/cims-agent/modules, 개발=build/dist/<server>)"
    prefix=$(readlink -f "$prefix")     # local_nodes 에 넣을 값은 절대경로여야 한다

    header "=== 단말 대면 인증서 배치: $HOST → $prefix ==="
    info "발급 $ISSUED_AT / 만료 $NOT_AFTER / CN=$CN"

    local here; here=$(hostname -f 2>/dev/null || hostname)
    [[ "$here" == "$HOST" ]] || warn "이 노드 hostname($here)이 묶음의 HOST($HOST)와 다르다 — 다른 노드용 묶음이면 중단하라"

    # SAN 사전 대조 — 부족하면 agent 가 재기동 때 덮어쓰므로 여기서 막는다
    local want src miss
    want=$(node_required_san "$prefix" 2>/tmp/.sc_src.$$); src=$(cat /tmp/.sc_src.$$; rm -f /tmp/.sc_src.$$)
    miss=$(_san_missing "$bundle/csc/server.crt" "$want")
    if [[ -n "$miss" ]]; then
        if (( skip_san )); then
            warn "SAN 부족(무시됨, --skip-san-check): $miss — CSC 재기동 시 그룹 CA 인증서로 덮일 수 있다"
        else
            err "인증서 SAN 에 이 노드가 요구하는 항목이 없다: $miss"
            err "  (요구 목록 출처: $src)"
            err "  그대로 두면 agent 가 재기동 때 그룹 CA 인증서로 덮어쓴다. CA 서버에서 다시 발급하라:"
            err "    scripts/service-cert.sh issue --host $here --san \"$want\" --force"
            exit 2
        fi
    else
        ok "SAN 대조 통과 ($src)"
    fi

    local ts; ts=$(date +%Y%m%d%H%M%S)
    local csc_dir="$prefix/csc/runtime/cert" csp_dir="$prefix/csp/runtime/cert"

    if [[ "$only" != csp ]]; then
        if [[ ! -d "$csc_dir" ]]; then
            mkdir -p "$csc_dir" && chmod 700 "$csc_dir" || die "생성 실패: $csc_dir"
            info "CSC cert 디렉터리 생성: $csc_dir (csc 는 이 자리를 버전 디렉터리보다 먼저 본다)"
        fi
        [[ -w "$csc_dir" ]] || die "쓰기 권한 없음: $csc_dir (서비스 계정으로 실행)"
        if [[ -f "$csc_dir/server.crt" ]]; then
            cp -p "$csc_dir/server.crt" "$csc_dir/server.crt.bak.$ts"
            [[ -f "$csc_dir/server.key" ]] && { cp -p "$csc_dir/server.key" "$csc_dir/server.key.bak.$ts"; chmod 600 "$csc_dir/server.key.bak.$ts"; }
            info "기존 CSC 인증서 백업: server.{crt,key}.bak.$ts ($(openssl x509 -in "$csc_dir/server.crt.bak.$ts" -noout -issuer 2>/dev/null))"
        fi
        # 키 → 인증서 순으로, 각각 임시파일 뒤 rename (짝 안 맞는 창 최소화. csc 는 적용 전 사전 검증도 한다)
        install -m 600 "$bundle/csc/server.key" "$csc_dir/.new.key" && mv -f "$csc_dir/.new.key" "$csc_dir/server.key" || die "CSC 키 배치 실패"
        install -m 644 "$bundle/csc/server.crt" "$csc_dir/.new.crt" && mv -f "$csc_dir/.new.crt" "$csc_dir/server.crt" || die "CSC 인증서 배치 실패"
        ok "CSC: $csc_dir/server.{crt,key} 교체"
    fi

    if [[ "$only" != csc ]]; then
        mkdir -p "$csp_dir" || die "생성 실패: $csp_dir"
        chmod 700 "$csp_dir" 2>/dev/null || true
        install -m 644 "$bundle/csp/csp-chain.pem" "$csp_dir/csp-chain.pem" || die "CSP 체인 배치 실패"
        install -m 600 "$bundle/csp/csp.key" "$csp_dir/csp.key" || die "CSP 키 배치 실패"
        ok "CSP: $csp_dir/csp-chain.pem · csp.key 배치"
    fi

    # CSC 핫리로드 확인 — 서빙 중인 leaf 지문이 배치본과 같아지면 반영
    if [[ "$only" != csp && "$wait_s" -gt 0 ]]; then
        local want_fp got="" p e cands=()
        want_fp=$(_fp "$bundle/csc/server.crt")
        [[ -n "$ip" ]] && cands+=("$ip")
        IFS=',' read -ra _e <<< "$SAN"; for e in "${_e[@]}"; do [[ "$e" == IP:* ]] && cands+=("${e#IP:}"); done
        local reach=""
        for p in "${cands[@]}"; do
            got=$(_served_fp "$p" "$DEFAULT_CSC_PORT"); [[ -n "$got" ]] && { reach="$p"; break; }
        done
        if [[ -z "$reach" ]]; then
            info "CSC $DEFAULT_CSC_PORT 에 접속되지 않는다 — 미기동이면 기동 시 이 인증서로 뜬다"
        else
            info "CSC 핫리로드 대기(최대 ${wait_s}s, $reach:$DEFAULT_CSC_PORT)…"
            local t=0
            while (( t < wait_s )); do
                got=$(_served_fp "$reach" "$DEFAULT_CSC_PORT")
                [[ "$got" == "$want_fp" ]] && break
                sleep 5; t=$((t+5))
            done
            if [[ "$got" == "$want_fp" ]]; then
                ok "CSC 가 새 인증서를 서빙한다 (${t}s)"
            else
                warn "CSC 핫리로드 미반영(${wait_s}s) — 콘솔에서 CSC 를 재시작하라. 재시작 후 verify 로 확인"
            fi
        fi
    fi

    header "=== 다음 단계 ==="
    if [[ "$only" != csc ]]; then
        echo "  1) CSP 에 경로 알리기 — 자동:  bash $bundle/service-cert.sh csp-node      (콘솔 admin 비밀번호 필요, --dry-run 으로 미리보기)"
        echo "     또는 콘솔 > [패키지 설정] > csp > local_nodes 의 TLS 행에 직접 저장:"
        echo "       tls_cert_path = $csp_dir/csp-chain.pem"
        echo "       tls_key_path  = $csp_dir/csp.key"
    fi
    echo "  2) 검증:  bash $bundle/service-cert.sh verify --csp-port <CSP TLS 포트>      (기본 --ip $CN --csc-port $DEFAULT_CSC_PORT)"
    echo "  3) CSC 재시작 후 verify 재실행 — 발급자가 유지돼야 한다"
}

# ── csp-node ─────────────────────────────────────────────────────────────────
# CSP 는 CSC 와 달리 인증서 파일을 스스로 찾지 않는다 — local_nodes TLS 행의 tls_cert_path/
# tls_key_path 가 가리키는 파일을 쓴다(정본 = OAM 의 컬렉션, docs/api/collection_api.md).
# 콘솔의 저장 버튼이 부르는 PUT /api/v1/deployments/{id}/collection/local_nodes 를 그대로 호출한다:
# OAM 이 템플릿 스키마로 검증 → agent 가 jsonl 원자 쓰기 → CSP 에 SIGUSR1 → 소켓 유지한 채 인증서 교체.
# 파일을 직접 고치지 않는 이유: 다음 설정 push 때 OAM 값으로 되돌아간다.
cmd_csp_node() {
    _need curl; _need python3
    local oam="https://127.0.0.1:4419" user="admin" port="" bind_ip="" prefix="$DEFAULT_PREFIX" dep="" dry=0 bundle="$SCRIPT_DIR"
    while (($#)); do case "$1" in
        --oam) oam="${2%/}"; shift 2 ;;
        --user) user="$2"; shift 2 ;;
        --port) port="$2"; shift 2 ;;
        --bind-ip) bind_ip="$2"; shift 2 ;;
        --prefix) prefix="${2%/}"; shift 2 ;;
        --deployment) dep="$2"; shift 2 ;;
        --dry-run) dry=1; shift ;;
        --bundle) bundle="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "csp-node: 알 수 없는 옵션 $1" ;;
    esac; done
    _load_bundle "$bundle" || die "묶음이 아닙니다(bundle.env 필요): $bundle"
    prefix=$(readlink -f "$prefix" 2>/dev/null || echo "$prefix")
    local cert="$prefix/csp/runtime/cert/csp-chain.pem" key="$prefix/csp/runtime/cert/csp.key"
    if (( ! dry )); then
        [[ -f "$cert" && -f "$key" ]] || die "배치된 CSP 인증서가 없다: $cert — 먼저 install 을 돌린다"
    fi
    [[ -n "$bind_ip" ]] || bind_ip="$CN"

    header "=== CSP local_nodes 갱신: $oam ==="
    local pw="${CIMS_OAM_PASSWORD:-}"
    if [[ -z "$pw" ]]; then
        read -r -s -p "콘솔 $user 비밀번호: " pw </dev/tty; echo
    fi
    [[ -n "$pw" ]] || die "비밀번호가 비어 있다"

    # 로그인 (관리 API 는 그룹 CA 인증서라 -k. 단말 대면 검증과는 별개 평면)
    local tok
    tok=$(LOGIN_USER="$user" LOGIN_PW="$pw" python3 -c 'import json,os;print(json.dumps({"login_id":os.environ["LOGIN_USER"],"password":os.environ["LOGIN_PW"]}))' \
          | curl -sk --max-time 10 -X POST "$oam/api/v1/auth/login" -H 'Content-Type: application/json' --data-binary @- \
          | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("token",""))
except Exception: print("")')
    [[ -n "$tok" ]] || die "OAM 로그인 실패 ($oam, $user) — 주소·비밀번호 확인"
    ok "OAM 로그인"

    # csp 배포 찾기
    if [[ -z "$dep" ]]; then
        dep=$(curl -sk --max-time 10 -H "Authorization: Bearer $tok" "$oam/api/v1/deployments" | python3 -c '
import json,sys
d=json.load(sys.stdin); items=d if isinstance(d,list) else (d.get("items") or d.get("deployments") or [])
ids=[str(x.get("id")) for x in items if (x.get("package_name") or x.get("module") or "")=="csp"]
print(" ".join(ids))')
        [[ -n "$dep" ]] || die "csp 배포를 찾지 못했다 — 팀원이 csp 를 설치했는지 확인"
        if [[ "$dep" == *" "* ]]; then die "csp 배포가 여럿이다($dep) — --deployment ID 로 지정"; fi
    fi
    info "csp 배포 id=$dep"

    # 현재 컬렉션 → 새 records 계산 (python 이 판단·출력, 셸은 전달만)
    local cur plan
    cur=$(curl -sk --max-time 15 -H "Authorization: Bearer $tok" "$oam/api/v1/deployments/$dep/collection/local_nodes")
    plan=$(CUR="$cur" CERT="$cert" KEY="$key" BIND="$bind_ip" PORT="$port" python3 - <<'PYEOF'
import json, os, sys
try:
    d = json.loads(os.environ["CUR"])
except Exception:
    print("ERR:컬렉션 응답을 읽지 못했다"); sys.exit(0)
if "records" not in d:
    print("ERR:" + json.dumps(d, ensure_ascii=False)[:300]); sys.exit(0)
recs = d["records"]; cert = os.environ["CERT"]; key = os.environ["KEY"]
bind = os.environ["BIND"]; port = os.environ["PORT"]
tls = [r for r in recs if str(r.get("protocol", "")).upper() == "TLS" and (r.get("edge") or "access") == "access"]
if len(tls) > 1:
    m = [r for r in tls if r.get("bind_ip") == bind] or tls
    tls = m[:1]
    print("WARN:TLS 행이 여럿 — %s 를 갱신" % tls[0].get("name"))
before = None
if tls:
    row = tls[0]
    before = {k: row.get(k) for k in ("name", "bind_ip", "bind_port", "tls_cert_path", "tls_key_path")}
    row["tls_cert_path"] = cert; row["tls_key_path"] = key
    row["enabled"] = True
    if row.get("edge") is None: row["edge"] = "access"
    action = "update"
else:
    if not port:
        print("ERR:TLS 행이 없다 — --port <TLS 포트> 를 주면 access-tls 행을 만든다"); sys.exit(0)
    row = {"name": "access-tls", "enabled": True, "edge": "access", "bind_ip": bind,
           "bind_port": int(port), "protocol": "TLS", "tls_cert_path": cert, "tls_key_path": key}
    recs.append(row); action = "create"
print("PLAN:" + json.dumps({"action": action, "before": before,
      "after": {k: row.get(k) for k in ("name", "bind_ip", "bind_port", "tls_cert_path", "tls_key_path")},
      "port": row.get("bind_port"), "records": recs}, ensure_ascii=False))
PYEOF
)
    local err; err=$(sed -n 's/^ERR://p' <<< "$plan" | head -1); [[ -n "$err" ]] && die "$err"
    local w; w=$(sed -n 's/^WARN://p' <<< "$plan" | head -1); [[ -n "$w" ]] && warn "$w"
    local planj; planj=$(sed -n 's/^PLAN://p' <<< "$plan")
    [[ -n "$planj" ]] || die "계획 계산 실패"
    local tls_port; tls_port=$(python3 -c 'import json,sys;print(json.load(sys.stdin)["port"])' <<< "$planj")
    PLANJ="$planj" python3 - <<'PYEOF'
import json, os
p = json.loads(os.environ["PLANJ"])
b, a = p["before"], p["after"]
print("  동작      : " + ("기존 TLS 행 갱신" if p["action"] == "update" else "TLS 행 신규 생성 (access-tls)"))
print("  행        : %s  %s:%s" % (a["name"], a["bind_ip"], a["bind_port"]))
if b:
    print("  cert_path : %s  →  %s" % (b.get("tls_cert_path") or "(비움)", a["tls_cert_path"]))
    print("  key_path  : %s  →  %s" % (b.get("tls_key_path") or "(비움)", a["tls_key_path"]))
else:
    print("  cert_path : %s" % a["tls_cert_path"]); print("  key_path  : %s" % a["tls_key_path"])
PYEOF
    if (( dry )); then info "--dry-run: 저장하지 않았다"; return 0; fi

    # PUT (전체 치환 + SIGUSR1)
    local resp
    resp=$(PLANJ="$planj" python3 -c 'import json,os;p=json.loads(os.environ["PLANJ"]);print(json.dumps({"records":p["records"],"signal":True}))' \
           | curl -sk --max-time 30 -X PUT "$oam/api/v1/deployments/$dep/collection/local_nodes" \
               -H "Authorization: Bearer $tok" -H 'Content-Type: application/json' --data-binary @-)
    local rc=0
    RESP="$resp" python3 - <<'PYEOF' || rc=1
import json, os, sys
raw = os.environ["RESP"]
try: r = json.loads(raw)
except Exception: print("RESULT:ERR:응답 해석 실패: " + raw[:300]); sys.exit(1)
if r.get("ok"):
    sig = r.get("signaled") or []
    print("RESULT:OK:저장 완료 — 행 %s개, SIGUSR1 → pid %s" % (r.get("count"), sig if sig else "(CSP 미기동 — 기동 시 반영)"))
else:
    print("RESULT:ERR:저장 실패: " + json.dumps(r, ensure_ascii=False)[:600]); sys.exit(1)
PYEOF
    (( rc )) && die "local_nodes 저장에 실패했다 — 콘솔에서 직접 저장하라 (README 2-(b))"
    ok "local_nodes 저장 — TLS 행 ${bind_ip}:${tls_port} 이 배치 경로를 가리킨다 (SIGUSR1 무중단 반영)"

    header "=== CSP TLS 검증 ($bind_ip:$tls_port) ==="
    sleep 2
    "$SCRIPT_PATH" verify --ip "$bind_ip" --csc-port 0 --csp-port "$tls_port" --wait 20 \
        --ca "$( [[ -f "$bundle/$CA_NAME.crt" ]] && echo "$bundle/$CA_NAME.crt" || echo "$DEFAULT_CA_DIR/$DEFAULT_CA_NAME.crt" )"
}

# ── verify ───────────────────────────────────────────────────────────────────
_VF_FAIL=0
_res() {   # PASS|FAIL|WARN  label  detail
    case "$1" in
        PASS) echo -e "  ${GREEN}PASS${NC}  $2${3:+  — $3}" ;;
        WARN) echo -e "  ${YELLOW}WARN${NC}  $2${3:+  — $3}" ;;
        *)    echo -e "  ${RED}FAIL${NC}  $2${3:+  — $3}"; _VF_FAIL=1 ;;
    esac
}

_verify_port() {   # ip port label ca
    local ip="$1" port="$2" label="$3" ca="$4" raw n leaf issuer ca_subj out days
    header "[$label] $ip:$port"
    raw=$(timeout 8 openssl s_client -connect "$ip:$port" -showcerts </dev/null 2>/dev/null)
    n=$(grep -c "BEGIN CERTIFICATE" <<< "$raw")
    if (( n == 0 )); then
        _res FAIL "접속/TLS" "연결 실패 또는 TLS 아님"; return
    fi
    if (( n >= 2 )); then _res PASS "체인 전송" "${n}장"; else _res FAIL "체인 전송" "1장 — CA 가 이어붙지 않았다(체인 PEM 이 아니다)"; fi
    leaf=$(awk '/BEGIN CERTIFICATE/{f=1} f{print} /END CERTIFICATE/{exit}' <<< "$raw")
    issuer=$(openssl x509 -noout -issuer <<< "$leaf" 2>/dev/null | sed 's/^issuer=//')
    ca_subj=$(openssl x509 -in "$ca" -noout -subject | sed 's/^subject=//')
    if [[ "$issuer" == "$ca_subj" ]]; then _res PASS "발급자" "$issuer"; else _res FAIL "발급자" "$issuer (기대: $ca_subj) — 단말이 거절한다"; fi
    out=$(timeout 8 openssl s_client -connect "$ip:$port" -CAfile "$ca" -verify_return_error -verify_ip "$ip" -brief </dev/null 2>&1)
    if grep -q "Verification: OK" <<< "$out"; then _res PASS "체인+IP 신원($ip)"; else _res FAIL "체인+IP 신원($ip)" "$(grep -m1 -E 'verify error|Verification|error' <<< "$out")"; fi
    out=$(timeout 8 openssl s_client -connect "$ip:$port" -CAfile "$ca" -verify_return_error -verify_hostname wrong.example -brief </dev/null 2>&1)
    if grep -q "Verification: OK" <<< "$out"; then _res FAIL "틀린 이름 거절(음성 대조군)" "통과해 버림 — 신원 검사 미집행"; else _res PASS "틀린 이름 거절(음성 대조군)"; fi
    days=$(openssl x509 -noout -enddate <<< "$leaf" 2>/dev/null | cut -d= -f2)
    days=$(( ( $(date -d "$days" +%s) - $(date +%s) ) / 86400 ))
    if (( days > 30 )); then _res PASS "leaf 만료" "${days}일 남음"; elif (( days > 0 )); then _res WARN "leaf 만료" "${days}일 남음 — A-PRC-009 임계"; else _res FAIL "leaf 만료" "만료됨"; fi
}

cmd_verify() {
    _need openssl
    local ip="" csc_port="$DEFAULT_CSC_PORT" csp_port="$DEFAULT_CSP_PORT" ca="" wait_s=0
    while (($#)); do case "$1" in
        --ip) ip="$2"; shift 2 ;;
        --csc-port) csc_port="$2"; shift 2 ;;
        --csp-port) csp_port="$2"; shift 2 ;;
        --ca) ca="$2"; shift 2 ;;
        --wait) wait_s="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "verify: 알 수 없는 옵션 $1" ;;
    esac; done
    if [[ -z "$ip" && -f "$SCRIPT_DIR/bundle.env" ]]; then
        # shellcheck disable=SC1091
        ( source "$SCRIPT_DIR/bundle.env"; echo "$CN" ) > /tmp/.sc_cn.$$ 2>/dev/null; ip=$(cat /tmp/.sc_cn.$$); rm -f /tmp/.sc_cn.$$
        [[ -n "$ip" ]] && info "--ip 생략 — 묶음의 접속 주소 $ip 를 쓴다"
    fi
    [[ -n "$ip" ]] || die "verify: --ip 필수 (단말이 접속하는 주소. 묶음 디렉터리에서 실행하면 자동)"
    if [[ -z "$ca" ]]; then
        for c in "$SCRIPT_DIR/$DEFAULT_CA_NAME.crt" "$DEFAULT_CA_DIR/$DEFAULT_CA_NAME.crt"; do [[ -f "$c" ]] && { ca="$c"; break; }; done
    fi
    [[ -f "$ca" ]] || die "CA 인증서를 찾을 수 없다 (--ca FILE)"
    local t=0
    while :; do
        _VF_FAIL=0
        header "=== 단말 대면 TLS 검증: $ip (CA: $(openssl x509 -in "$ca" -noout -subject | sed 's/^subject=//')) ==="
        (( csc_port > 0 )) && _verify_port "$ip" "$csc_port" "CSC HTTPS" "$ca"
        (( csp_port > 0 )) && _verify_port "$ip" "$csp_port" "CSP SIP/TLS" "$ca"
        (( _VF_FAIL == 0 )) && break
        (( t >= wait_s )) && break
        info "FAIL 있음 — ${wait_s}s 까지 재시도 (${t}s)"; sleep 5; t=$((t+5))
    done
    echo
    if (( _VF_FAIL )); then err "검증 실패 항목이 있다 — 단말이 붙지 않는다"; return 1; fi
    ok "전 항목 PASS — 단말(Service CA 앵커)이 이 주소로 접속할 수 있다"
}

# ── push ─────────────────────────────────────────────────────────────────────
cmd_push() {
    _need ssh; _need scp
    local target="" prefix="$DEFAULT_PREFIX" bundle="" remote_dir=/tmp csp_port=0 extra=()
    while (($#)); do case "$1" in
        --ssh) target="$2"; shift 2 ;;
        --prefix) prefix="$2"; shift 2 ;;
        --bundle) bundle="$2"; shift 2 ;;
        --remote-dir) remote_dir="$2"; shift 2 ;;
        --csp-port) csp_port="$2"; shift 2 ;;
        --skip-san-check|--no-wait) extra+=("$1"); shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "push: 알 수 없는 옵션 $1" ;;
    esac; done
    [[ -n "$target" ]] || die "push: --ssh user@host 필수"
    [[ -n "$bundle" ]] || bundle="$SCRIPT_DIR"
    local tgz name
    if [[ -f "$bundle" && "$bundle" == *.tgz ]]; then
        tgz="$bundle"; name=$(basename "$bundle" .tgz)
    elif _load_bundle "$bundle"; then
        name=$(basename "$(readlink -f "$bundle")")
        tgz=$(mktemp --suffix=.tgz); tar czf "$tgz" -C "$(dirname "$(readlink -f "$bundle")")" "$name" || die "tgz 생성 실패"
    else
        die "묶음 디렉터리 또는 tgz 를 --bundle 로 지정하라"
    fi
    local host="${target#*@}"
    header "=== push → $target ($remote_dir/$name, prefix=$prefix) ==="
    scp -q "$tgz" "$target:$remote_dir/$name.tgz" || die "scp 실패"
    ssh "$target" "cd '$remote_dir' && rm -rf '$name' && tar xzf '$name.tgz' && chmod 600 '$name.tgz' && bash '$name/service-cert.sh' install --prefix '$prefix' ${extra[*]}" \
        || die "원격 install 실패"
    header "=== 원격 검증 ($host) ==="
    "$SCRIPT_PATH" verify --ip "$host" --csc-port "$DEFAULT_CSC_PORT" --csp-port "$csp_port" \
        --ca "$( [[ -f "$bundle/$DEFAULT_CA_NAME.crt" ]] && echo "$bundle/$DEFAULT_CA_NAME.crt" || echo "$DEFAULT_CA_DIR/$DEFAULT_CA_NAME.crt" )"
}

# ── 진입 ─────────────────────────────────────────────────────────────────────
case "${1:-}" in
    collect) shift; cmd_collect "$@" ;;
    issue)   shift; cmd_issue "$@" ;;
    install) shift; cmd_install "$@" ;;
    csp-node) shift; cmd_csp_node "$@" ;;
    verify)  shift; cmd_verify "$@" ;;
    push)    shift; cmd_push "$@" ;;
    -h|--help|help|"") usage; [[ -n "${1:-}" ]] && exit 0 || exit 1 ;;
    *) err "알 수 없는 서브커맨드: $1"; usage; exit 1 ;;
esac
