#!/usr/bin/env bash
# =============================================================
# deployment/tb/lib/tb-common.sh — TB 설치 스크립트 공용 라이브러리
#
# 자기완결이다 — scripts/lib/common.sh 를 쓰지 않는다. TB 반입본(USB)은 소스 트리
# 없이 단독 동작해야 하므로 agent/bin/cims-* 와 같은 관례를 따른다.
# 출력 포맷(info/ok/warn/err/header, "[1/3]" 단계 표기)만 공유한다.
# =============================================================

[[ -n "${_TB_COMMON_SH_LOADED:-}" ]] && return 0
_TB_COMMON_SH_LOADED=1

# ── 색상 / 로그 ────────────────────────────────────────────────
RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
CYAN=$'\033[0;36m'; BOLD=$'\033[1m'; NC=$'\033[0m'

info()   { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()     { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()   { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()    { echo -e "${RED}[ERROR]${NC} $*" >&2; }
header() { echo -e "\n${BOLD}$*${NC}"; }
die()    { err "$*"; exit 1; }

# 해소 방법을 함께 알려주고 중단한다 — 폐쇄망에서 "뭘 반입해야 하는지" 가 핵심 정보다.
die_hint() {
    err "$1"; shift
    for l in "$@"; do echo "        $l" >&2; done
    exit 1
}

# ── 경로 ───────────────────────────────────────────────────────
# TB_ROOT = deployment/tb (소스 트리) 또는 반입본 루트. 두 경우 다 이 파일의 상위다.
TB_ROOT="${TB_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
TB_SITE_CONF="${TB_SITE_CONF:-$TB_ROOT/tb-site.conf}"
TB_STATE_DIR="${TB_STATE_DIR:-$TB_ROOT/state}"
TB_LOG_DIR="${TB_LOG_DIR:-$TB_ROOT/log}"

# 반입본 자원. 소스 트리에서 실행하면 레포 경로로 폴백한다.
TB_OFFLINE_DIR="${TB_OFFLINE_DIR:-$TB_ROOT/offline}"
TB_REPO_ROOT="$(cd "$TB_ROOT/../.." 2>/dev/null && pwd || true)"

# tb_find_offline <상대경로> [레포_상대경로...]
#   반입본 → 레포 순으로 찾아 절대경로를 출력한다. 없으면 빈 문자열.
tb_find_offline() {
    local rel="$1"; shift || true
    local c
    for c in "$TB_OFFLINE_DIR/$rel"; do
        [[ -e "$c" ]] && { (cd "$(dirname "$c")" && printf '%s/%s\n' "$PWD" "$(basename "$c")"); return 0; }
    done
    for rel in "$@"; do
        c="$TB_REPO_ROOT/$rel"
        [[ -n "$TB_REPO_ROOT" && -e "$c" ]] && { (cd "$(dirname "$c")" && printf '%s/%s\n' "$PWD" "$(basename "$c")"); return 0; }
    done
    return 1
}

tb_init_dirs() {
    mkdir -p "$TB_STATE_DIR" "$TB_LOG_DIR"
    chmod 700 "$TB_STATE_DIR" 2>/dev/null || true
}

# ── 사이트 설정 (tb-site.conf) ─────────────────────────────────
# TB 마다 다른 값은 전부 이 파일 하나에 모인다. 없으면 질의해서 만든다.
# 파일은 셸 할당문 목록이라 편집기로 직접 고쳐도 된다.
tb_load_site() {
    if [[ -f "$TB_SITE_CONF" ]]; then
        # shellcheck disable=SC1090
        source "$TB_SITE_CONF"
        _TB_SITE_LOADED=1
    fi
}

# tb_site_set <KEY> <VALUE> — 메모리 + 파일 동시 반영 (멱등 upsert)
tb_site_set() {
    local key="$1" val="$2"
    printf -v "$key" '%s' "$val" 2>/dev/null || eval "$key=\$val"
    mkdir -p "$(dirname "$TB_SITE_CONF")"
    if [[ -f "$TB_SITE_CONF" ]] && grep -q "^${key}=" "$TB_SITE_CONF"; then
        local tmp; tmp="$(mktemp)"
        # 값에 어떤 문자가 와도 안전하도록 python 으로 인용해 치환한다.
        KEY="$key" VAL="$val" SRC="$TB_SITE_CONF" python3 - > "$tmp" <<'PY'
import os, shlex, re
key, val, src = os.environ['KEY'], os.environ['VAL'], os.environ['SRC']
line = '%s=%s' % (key, shlex.quote(val))
out, done = [], False
for ln in open(src, encoding='utf-8').read().splitlines():
    if re.match(r'^%s=' % re.escape(key), ln):
        out.append(line); done = True
    else:
        out.append(ln)
if not done:
    out.append(line)
print('\n'.join(out))
PY
        cat "$tmp" > "$TB_SITE_CONF"; rm -f "$tmp"
    else
        [[ -f "$TB_SITE_CONF" ]] || {
            {
                echo "# tb-site.conf — 이 TB 사이트의 설정값 (스크립트가 생성·갱신)"
                echo "# 직접 편집해도 된다. 비밀값이 들어가므로 0600 유지."
                echo "# 다시 질의받으려면: tb-install.sh --reconfigure"
                echo
            } > "$TB_SITE_CONF"
        }
        KEY="$key" VAL="$val" python3 -c \
            'import os,shlex;print("%s=%s"%(os.environ["KEY"],shlex.quote(os.environ["VAL"])))' \
            >> "$TB_SITE_CONF"
    fi
    chmod 600 "$TB_SITE_CONF" 2>/dev/null || true
}

# tb_ask <KEY> <질문> [기본값] [secret]
#   이미 값이 있으면 묻지 않는다(멱등). TB_RECONFIGURE=1 이면 현재값을 기본값으로 다시 묻는다.
#   비대화식(TB_BATCH=1)에서 값도 기본값도 없으면 중단한다 — 반쪽 설치 방지.
tb_ask() {
    local key="$1" label="$2" default="${3:-}" secret="${4:-}"
    local cur="${!key:-}"

    if [[ -n "$cur" && "${TB_RECONFIGURE:-0}" != "1" ]]; then
        tb_site_set "$key" "$cur"
        return 0
    fi
    [[ -n "$cur" ]] && default="$cur"

    if [[ "${TB_BATCH:-0}" == "1" ]]; then
        [[ -n "$default" ]] || die "비대화식인데 $key 값이 없습니다 — tb-site.conf 에 넣거나 대화식으로 실행하세요"
        tb_site_set "$key" "$default"
        return 0
    fi

    # 입력이 tty 가 아니거나(파이프·EOF) 닫히면 read 가 실패한다 — 되묻는 루프에
    # 갇히지 않게 그 즉시 중단한다.
    local v
    if [[ "$secret" == "secret" ]]; then
        local shown="(입력 없으면 현재값 유지)"; [[ -z "$default" ]] && shown="(필수)"
        while :; do
            read -r -s -p "  $label $shown: " v || die "입력이 끊겼습니다 ($key) — tb-site.conf 에 값을 넣고 --batch 로 실행하세요"
            echo
            [[ -z "$v" && -n "$default" ]] && { v="$default"; break; }
            [[ -z "$v" ]] && { warn "필수 값입니다"; continue; }
            local v2
            read -r -s -p "  $label (확인): " v2 || die "입력이 끊겼습니다 ($key)"
            echo
            [[ "$v" == "$v2" ]] && break
            warn "값이 일치하지 않습니다 — 다시 입력하세요"
        done
    else
        local suffix=""; [[ -n "$default" ]] && suffix=" [$default]"
        while :; do
            read -r -p "  $label$suffix: " v || die "입력이 끊겼습니다 ($key) — tb-site.conf 에 값을 넣고 --batch 로 실행하세요"
            v="${v:-$default}"
            [[ -n "$v" ]] && break
            warn "필수 값입니다"
        done
    fi
    tb_site_set "$key" "$v"
}

# tb_ask_always <KEY> <질문> [기본값] [secret]
#   저장된 값이 있어도 **매번 묻는다** (현재값이 기본값). 시험 조건처럼 실행마다 바꾸는
#   값에 쓴다. 설치 값에는 tb_ask(멱등)를 쓴다 — 되물으면 오타 한 번에 설치가 어긋난다.
#   TB_BATCH=1 이면 tb_ask 와 같이 저장된 값으로 조용히 진행한다.
tb_ask_always() {
    local _save="${TB_RECONFIGURE:-}" _rc=0
    TB_RECONFIGURE=1
    tb_ask "$@" || _rc=$?
    if [[ -n "$_save" ]]; then TB_RECONFIGURE="$_save"; else unset TB_RECONFIGURE; fi
    return $_rc
}

# ── 전제조건 검사 ──────────────────────────────────────────────
TB_REF_OS_ID="ubuntu"
TB_REF_OS_VERSION="26.04"
TB_REF_PYTHON="3.14"

tb_os_id()      { . /etc/os-release 2>/dev/null; echo "${ID:-unknown}"; }
tb_os_version() { . /etc/os-release 2>/dev/null; echo "${VERSION_ID:-unknown}"; }
tb_os_pretty()  { . /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}"; }

# 이 배포본은 Ubuntu 26.04 / CPython 3.14 를 겨냥해 빌드돼 있다:
#   - OAM·CSC 의 vendor 확장이 cpython-314 ABI 전용 (netifaces 는 무조건 import)
#   - csp/cmp 는 glibc·libstdc++·libssl.so.3·libmariadb.so.3 에 링크
#   - OS 의존 라이브러리 동봉물이 .deb (agent 가 dpkg 로 설치)
# 그래서 다른 배포판에서는 "설정을 맞추는" 문제가 아니라 재빌드 문제가 된다.
tb_require_os() {
    local id ver
    id="$(tb_os_id)"; ver="$(tb_os_version)"
    if [[ "$id" == "$TB_REF_OS_ID" && "$ver" == "$TB_REF_OS_VERSION" ]]; then
        ok "OS: $(tb_os_pretty) ($(uname -m))"
        return 0
    fi
    if [[ "${TB_FORCE_OS:-0}" == "1" ]]; then
        warn "OS 불일치: $(tb_os_pretty) — 기준은 ${TB_REF_OS_ID} ${TB_REF_OS_VERSION} (--force-os 로 계속)"
        return 0
    fi
    die_hint "OS 불일치 — 이 배포본은 ${TB_REF_OS_ID} ${TB_REF_OS_VERSION} 전용입니다 (현재: $(tb_os_pretty))" \
        "OAM·CSC 의 동봉 확장이 CPython ${TB_REF_PYTHON} 전용 바이너리이고," \
        "csp/cmp 는 이 배포판의 glibc·libssl 에 링크돼 있습니다." \
        "다른 배포판(Rocky 등)에서는 재빌드가 필요합니다 — 별도 과제." \
        "그래도 진행하려면: --force-os"
}

tb_require_arch() {
    local m; m="$(uname -m)"
    [[ "$m" == "x86_64" ]] || die "아키텍처 불일치: $m (동봉 바이너리는 x86_64 전용)"
}

# python3 이 필요한 최소 조건 — DB 단계는 순수 파이썬(pymysql)이라 버전을 가리지 않는다.
tb_require_python() {
    command -v python3 >/dev/null || die_hint "python3 없음" \
        "Ubuntu 26.04 최소 설치에는 python3 가 있습니다. 확인: dpkg -l python3-minimal"
}

# OAM·CSC 가 올라갈 노드에서만 요구한다 (agent 는 순수 파이썬이라 무관).
tb_require_python314() {
    tb_require_python
    local v; v="$(python3 -c 'import sys;print("%d.%d"%sys.version_info[:2])')"
    [[ "$v" == "$TB_REF_PYTHON" ]] && { ok "python3 $v"; return 0; }
    if [[ "${TB_FORCE_OS:-0}" == "1" ]]; then
        warn "python3 $v — OAM·CSC 동봉 확장은 ${TB_REF_PYTHON} 전용입니다 (--force-os 로 계속)"
        return 0
    fi
    die_hint "python3 $v — OAM·CSC 는 CPython ${TB_REF_PYTHON} 가 필요합니다" \
        "동봉 확장(netifaces·pydantic_core 등)이 cpython-314 ABI 전용이라," \
        "버전이 다르면 OAM 이 기동 자체를 못 합니다."
}

tb_require_cmds() {
    local missing=() c
    for c in "$@"; do command -v "$c" >/dev/null || missing+=("$c"); done
    [[ ${#missing[@]} -eq 0 ]] && return 0
    die_hint "필수 명령 없음: ${missing[*]}" \
        "폐쇄망이라 apt 가 안 되므로 해당 .deb 를 USB 로 반입해 offline/debs/ 에 두고" \
        "05-os-prereq.sh 를 먼저 실행하세요."
}

tb_require_root() {
    [[ ${EUID} -eq 0 ]] || die "root 권한이 필요합니다 — sudo 로 실행하세요"
}

# 이 스크립트는 sudo 경유 실행을 전제한다 (부트스트랩 install.sh 와 같은 가드).
tb_require_sudo_caller() {
    tb_require_root
    [[ -n "${SUDO_USER:-}" ]] || die_hint "root 직접 로그인으로 실행됐습니다" \
        "일반 계정에서 'sudo $0' 로 실행하세요 — 서비스 계정 판정에 SUDO_USER 를 씁니다."
}

# ── 로컬 주소 판정 (DB 가 이 장비인지) ────────────────────────
tb_is_local_addr() {
    local a="$1"
    [[ "$a" == "127.0.0.1" || "$a" == "localhost" || "$a" == "::1" ]] && return 0
    ip -o addr show 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | grep -qx "$a"
}

# ── 사이트 TLS 인증서 ─────────────────────────────────────────
# tb_ensure_site_cert <경로> <SAN 주소> [소유자]
#   반입본에 동봉된 csp.pem 은 **레포에 커밋된 전 배치 공용 키**다(csp/csp.pem →
#   CMakeLists 가 dist/csp/cert/ 로 복사). 그대로 쓰면 모든 TB 가 같은 개인키를
#   공유한다. 사이트마다 따로 만들어 그 위험을 없앤다.
#   동봉본은 SAN 이 없어 단말이 신원 검증을 켜면 반드시 실패한다 — 여기서 만드는
#   것에는 SAN(호스트명·127.0.0.1·SIP 주소)을 넣는다.
#   형식은 동봉본과 같은 **cert+key 결합 PEM** 이라 tls_key_path 는 계속 비운다.
#   이미 있으면 만들지 않는다 — 재실행이 키를 갈아치우면 단말이 붙어 있는 동안
#   TLS 세션이 끊긴다.
tb_ensure_site_cert() {
    local path="$1" san_ip="$2" owner="${3:-}"
    if [[ -s "$path" ]]; then
        ok "TLS 인증서 재사용: $path"
        return 0
    fi
    tb_require_cmds openssl
    mkdir -p "$(dirname "$path")"
    local tmp; tmp="$(mktemp -d)" || die "임시 디렉토리 생성 실패"
    local host; host="$(hostname -f 2>/dev/null || hostname)"
    local san="DNS:${host},IP:127.0.0.1"
    [[ -n "$san_ip" && "$san_ip" != "127.0.0.1" ]] && san="${san},IP:${san_ip}"
    if ! openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
            -subj "/CN=csp/O=CIMS" -addext "subjectAltName=${san}" \
            -keyout "$tmp/key.pem" -out "$tmp/crt.pem" 2>"$tmp/err"; then
        err "$(cat "$tmp/err" 2>/dev/null | tail -3)"
        rm -rf "$tmp"
        die "TLS 인증서 생성 실패 (openssl)"
    fi
    umask 077
    cat "$tmp/crt.pem" "$tmp/key.pem" > "$path"
    rm -rf "$tmp"
    chmod 600 "$path"
    [[ -n "$owner" ]] && { chown "$owner" "$path" 2>/dev/null || warn "소유자 지정 실패: $owner"; }
    ok "TLS 인증서 생성: $path"
    ok "  CN=csp  SAN=${san}  유효 3650일  0600 ${owner:-$(id -un)}"
}

# ── MariaDB 상태 판정 ─────────────────────────────────────────
# `mariadbd --version` 은 **바이너리가 있으면** 답한다 — 서버가 죽어 있어도 통과하므로
# 기동 확인에 쓰면 안 된다. 실제 판정은 systemd 의 active 상태로 한다.
tb_db_active() { systemctl is-active --quiet mariadb; }

# 시스템 DB(mysql 스키마) 초기화 여부. datadir 은 배포판마다 다르다 —
# Ubuntu 26.04 부터 /var/lib/mariadb 가 기본이고 그 전에는 /var/lib/mysql 이다.
# 초기화가 안 된 상태에서는 mariadbd 가 권한 테이블을 못 열고 매번 죽는다.
# 응답 없는 마운트(NAS/NFS)에서 멈추지 않는 크기 재기.
#
# 죽은 NFS 마운트에서 `du`·`stat` 은 **무한정 블록**한다(uninterruptible I/O — Ctrl+C 도
# 안 먹는다). 철거 스크립트의 `--dry-run` 이 "무엇을 지울지" 만 찍다가 NAS 크기를 재려고
# 멈춘 적이 있다(2026-09-10 실측, media02). **미리보기가 멈추는 것은 어떤 이유로도 안 되므로**
# 시간 제한을 두고, 넘으면 크기 대신 그 사실을 돌려준다.
tb_du_safe() {
    local path="$1" secs="${2:-5}" out
    [[ -e "$path" ]] || { echo "없음"; return 0; }
    out="$(timeout "$secs" du -sh "$path" 2>/dev/null | cut -f1)"
    if [[ -z "$out" ]]; then
        echo "응답없음(${secs}s 초과 — 마운트 확인)"
    else
        echo "$out"
    fi
    return 0
}

# 경로가 살아 있는 마운트인지 — 죽은 NFS 를 만지기 전에 먼저 묻는다.
tb_path_alive() {
    local path="$1" secs="${2:-5}"
    timeout "$secs" ls -d "$path" >/dev/null 2>&1
}

tb_db_datadir() {
    local d
    for d in /var/lib/mariadb /var/lib/mysql; do
        [[ -d "$d/mysql" ]] && { echo "$d"; return 0; }
    done
    for d in /var/lib/mariadb /var/lib/mysql; do
        [[ -d "$d" ]] && { echo "$d"; return 1; }
    done
    return 1
}
tb_db_initialized() { tb_db_datadir >/dev/null; }

# 반쪽 초기화(패키지는 설치됐고 시스템 DB 만 없는 상태)의 복구 안내.
# 철거 후 재설치에서 AppArmor 프로파일이 커널에 남아 mariadb-install-db 가
# setgid/dac_override 거부로 실패할 때 생긴다 (tb-teardown.sh 가 프로파일을 걷는다).
tb_db_init_hint() {
    # tb_db_datadir 는 "디렉토리는 있고 초기화만 안 됨" 을 **경로를 찍고 rc=1** 로 알린다.
    # `|| echo` 로 받으면 두 줄이 이어붙어 복구 명령의 --datadir 이 깨진다(실측) — 값이
    # 비었을 때만 폴백한다.
    local dd; dd="$(tb_db_datadir 2>/dev/null)"; dd="${dd:-/var/lib/mariadb}"
    cat >&2 <<EOF
        시스템 DB(mysql 스키마)가 없습니다 — datadir: $dd
        패키지 postinst 의 mariadb-install-db 가 실패하면 이 상태가 됩니다
        (postinst 가 set +e 로 감싸 성공으로 끝나기 때문에 조용히 지나갑니다).
        AppArmor 프로파일이 커널에 남아 있으면 그것이 원인입니다. 복구:
          sudo apparmor_parser -C -r /etc/apparmor.d/mariadbd
          sudo mariadb-install-db --datadir=$dd --user=mysql --rpm --cross-bootstrap --skip-test-db --disable-log-bin
          sudo apparmor_parser -r /etc/apparmor.d/mariadbd
          sudo systemctl start mariadb
EOF
}

# ── 로그 ───────────────────────────────────────────────────────
# tb_tee <로그이름> <명령...> — 화면에 그대로 내보내면서 로그에도 남긴다.
#   tb_run 은 실패할 때만 출력을 보여준다(장문 apt 로그 같은 것). 반대로 진행 내용을
#   사람이 보면서 판단해야 하는 단계(가입자 입력 계획, 패키지 업로드 목록)는 화면이
#   필요하고, 그러면서도 현장에서 되짚을 기록이 남아야 한다.
#
#   `cmd | tee` 의 종료코드는 tee 것이라 실패가 삼켜진다 — PIPESTATUS[0] 로 **명령의**
#   코드를 돌려준다. `local -` 로 셸 옵션을 함수 안에 가둬, 호출 문맥이 `|| die` 든
#   맨 호출이든 pipefail/errexit 때문에 PIPESTATUS 를 못 읽는 일이 없게 한다.
tb_tee() {
    local name="$1"; shift
    local -                       # 아래 set +e 는 이 함수 안에서만
    set +e +o pipefail
    mkdir -p "$TB_LOG_DIR"
    local lf="$TB_LOG_DIR/${name}.log"
    # 헤더의 명령줄에서 비밀값은 가린다 — 로그가 반출물에 섞여도 새지 않게.
    local shown
    shown="$(printf '%s' "$*" | sed -E 's/([A-Za-z_]*(PASS|PASSWORD|SECRET|TOKEN)[A-Za-z_]*)=[^ ]*/\1=********/g')"
    printf '\n=== %s — %s ===\n' "$(date '+%F %T')" "$shown" >> "$lf"
    "$@" 2>&1 | tee -a "$lf"
    return "${PIPESTATUS[0]}"
}

# tb_run <로그이름> <명령...> — 장문 출력은 로그로 돌리고 실패 시에만 tail 노출.
tb_run() {
    local name="$1"; shift
    mkdir -p "$TB_LOG_DIR"
    local lf="$TB_LOG_DIR/${name}.log"
    # rc 는 명령 **바로 뒤**에서 받는다. `if "$@"; then return 0; fi` 뒤의 `$?` 는
    # 그 if 문 자체의 상태(=0)라 실패 코드가 사라진다 — 그러면 호출부의
    # `tb_run … || die` 가 발동하지 않아 실패한 단계가 그대로 다음으로 넘어간다.
    local rc=0
    "$@" >>"$lf" 2>&1 || rc=$?
    [[ $rc -eq 0 ]] && return 0
    err "실패: $* (rc=$rc)"
    echo "  ── $lf (마지막 20줄) ──" >&2
    tail -20 "$lf" >&2
    return $rc
}
