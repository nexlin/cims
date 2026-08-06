#!/usr/bin/env bash
# =============================================================
# scripts/sync.sh — 소스 → build/dist 증분 동기화
# cims.sh sync 의 위임 대상 (CLI 계약은 cims.sh 가 유지).
# 소스 트리 전용 — dist 에서는 동작하지 않는다.
# =============================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$REPO_ROOT/scripts/lib/common.sh" || {
    echo "[ERROR] scripts/lib/common.sh 없음" >&2; exit 1; }
[[ -f "$REPO_ROOT/CMakeLists.txt" ]] || { err "소스 트리 전용 명령 (dist 에서 실행 불가)"; exit 1; }

# cims.sh 헤더와 동일한 환경 (이동 코드가 그대로 참조)
SCRIPT_DIR="$REPO_ROOT"
DIST_DIR="$SCRIPT_DIR/build/dist"
SRC_CONSOLE="$SCRIPT_DIR/ems/core/console"
SRC_PHONE="$SCRIPT_DIR/cims-phone"

cmd_sync() {
    # 소스 트리 → dist 로 Python/스크립트/메타를 복사 (C++ 빌드 없이 빠른 배포).
    # Usage: ./cims.sh sync [csc|agent|scripts|pkg-meta|console|all]
    if [[ -z "$SRC_CONSOLE" ]]; then
        err "sync 명령은 소스 트리에서만 실행 가능 (dist 안에서는 의미 없음)"
        return 1
    fi
    if [[ ! -d $DIST_DIR ]]; then
        err "dist 디렉토리 없음: $DIST_DIR (먼저 ./cims.sh build 한 번 실행)"
        return 1
    fi

    local targets=("$@")
    [[ ${#targets[@]} -eq 0 ]] && targets=(all)

    local did_csc=0 did_agent=0 did_scripts=0 did_pkg=0 did_console=0 did_oamsvc=0
    for t in "${targets[@]}"; do
        case "$t" in
            all) did_csc=1 did_agent=1 did_scripts=1 did_pkg=1 did_oamsvc=1 ;;
            csc)       did_csc=1 ;;
            oam-svc)  did_oamsvc=1 ;;
            agent)     did_agent=1 ;;
            scripts)   did_scripts=1 ;;
            pkg-meta)  did_pkg=1 ;;
            console)   did_console=1 ;;
            phone)     err "phone(cims-phone) 은 재설계 예정 — sync 대상에서 제외됨"; return 1 ;;
            *) err "알 수 없는 sync 대상: $t"; return 1 ;;
        esac
    done

    local n_changed=0

    # ── CSC Python 소스 (+ OAM Phase 1: 같은 binary, sys.path mount) ──
    if [[ $did_csc -eq 1 ]]; then
        mkdir -p "$DIST_DIR/csc/src" "$DIST_DIR/oam/src"
        # rsync 가 있으면 사용, 없으면 cp -r (목적지 깨끗이)
        if command -v rsync >/dev/null 2>&1; then
            rsync -a --delete-excluded \
                --exclude='__pycache__' --exclude='*.pyc' \
                "$SCRIPT_DIR/csc/src/" "$DIST_DIR/csc/src/"
            rsync -a --delete-excluded \
                --exclude='__pycache__' --exclude='*.pyc' \
                "$SCRIPT_DIR/ems/core/oam/src/" "$DIST_DIR/oam/src/"
        else
            cp -r "$SCRIPT_DIR/csc/src/." "$DIST_DIR/csc/src/"
            cp -r "$SCRIPT_DIR/ems/core/oam/src/." "$DIST_DIR/oam/src/"
        fi
        # __pycache__ stale 제거 (PEP 420 namespace 전환에 따른 옛 캐시 잔재)
        find "$DIST_DIR/csc/src" "$DIST_DIR/oam/src" -type d -name __pycache__ \
            -exec rm -rf {} + 2>/dev/null || true
        # config_template.json 도 동기화 (apply_config_template 가 읽는 파일)
        if [[ -f "$SCRIPT_DIR/csc/config/config_template.json" ]]; then
            mkdir -p "$DIST_DIR/csc/config"
            cp -f "$SCRIPT_DIR/csc/config/config_template.json" \
                  "$DIST_DIR/csc/config/config_template.json"
        fi
        # OAM 분리 Phase 2 — pkg.json 동기화 (별도 tarball 등록에 필요)
        if [[ -f "$SCRIPT_DIR/ems/core/oam/pkg.json" ]]; then
            cp -f "$SCRIPT_DIR/ems/core/oam/pkg.json" "$DIST_DIR/oam/pkg.json"
        fi
        # OAM 분리 Phase 3 — oam/config (oam.json / oam-tb.json) 동기화
        # + oam_base_service_split §7 — base 모드 활성화 템플릿(common/base/services 샘플) 동봉.
        #   운영자가 production 노드에서 .sample→실파일 rename 으로 --role base 전환할 수 있게.
        if [[ -d "$SCRIPT_DIR/ems/core/oam/config" ]]; then
            mkdir -p "$DIST_DIR/oam/config"
            # oam.json 은 configure 가 JwtSecret/ServiceLogging.Dir/CimsDatabase 를 패치한
            # 산출물 — dist 에 이미 있으면 보존(non-clobber). 소스본으로 덮어쓰면 csc 와
            # JWT 시크릿이 어긋나 gateway 401, 통계 API 는 DB 기본값(127.0.0.1)으로 500.
            for _f in "$SCRIPT_DIR/ems/core/oam/config/"*.json; do
                [[ -e "$_f" ]] || continue
                if [[ "$(basename "$_f")" == "oam.json" && -f "$DIST_DIR/oam/config/oam.json" ]]; then
                    continue
                fi
                cp -f "$_f" "$DIST_DIR/oam/config/" 2>/dev/null || true
            done
            cp -f "$SCRIPT_DIR/ems/core/oam/config/"*.sample "$DIST_DIR/oam/config/" 2>/dev/null || true
            if [[ -d "$SCRIPT_DIR/ems/core/oam/config/services" ]]; then
                mkdir -p "$DIST_DIR/oam/config/services"
                cp -f "$SCRIPT_DIR/ems/core/oam/config/services/"* "$DIST_DIR/oam/config/services/" 2>/dev/null || true
            fi
        fi
        # OAM 녹취 변환툴(ffmpeg/ffprobe) vendor 자동 채움 — 빌드 시 정적 바이너리 다운로드.
        # 패키지에 동봉되어 air-gapped 런타임에서 별도 설치 없이 녹취 재생(raw RTP→mp4) 가능.
        _ensure_oam_vendor_ffmpeg
        # 자동 배포 (auto_deployment.md) — AGENT phase 가 대상 노드로 밀어넣는
        # install-agent.sh 를 oam 패키지에 동봉한다. 배포본에는 레포의 agent/ 가 없으므로
        # 이게 빠지면 agent 자동 설치가 불가능하다. 항상 agent 소스 원본에서 갱신.
        mkdir -p "$DIST_DIR/oam/assets"
        cp -f "$SCRIPT_DIR/agent/install-agent.sh" "$DIST_DIR/oam/assets/install-agent.sh"
        chmod +x "$DIST_DIR/oam/assets/install-agent.sh"
        # Phase 4 vendor: private 환경 (인터넷 없음) 대응 — csc/vendor + oam/vendor 동기화.
        # csc/requirements.txt, oam/requirements.txt 도 함께.
        # 소스 경로가 컴포넌트마다 다르다: csc 는 레포 직하, oam 은 ems/core/oam.
        # (옛 코드가 둘 다 $SCRIPT_DIR/<comp>/vendor 로 봐서 oam vendor 동기화가
        #  조용히 건너뛰어지고 있었다 — dist 의 oam/vendor 는 옛 사본이 남은 것.)
        for _comp in csc oam; do
            local _vsrc
            case "$_comp" in
                oam) _vsrc="$SCRIPT_DIR/ems/core/oam" ;;
                *)   _vsrc="$SCRIPT_DIR/$_comp" ;;
            esac
            if [[ -d "$_vsrc/vendor" ]]; then
                mkdir -p "$DIST_DIR/$_comp/vendor"
                if command -v rsync >/dev/null 2>&1; then
                    rsync -a --delete-excluded --exclude='__pycache__' --exclude='*.pyc' \
                        "$_vsrc/vendor/" "$DIST_DIR/$_comp/vendor/"
                else
                    cp -r "$_vsrc/vendor/." "$DIST_DIR/$_comp/vendor/"
                fi
            fi
            if [[ -f "$_vsrc/requirements.txt" ]]; then
                cp -f "$_vsrc/requirements.txt" "$DIST_DIR/$_comp/requirements.txt"
            fi
        done
        ok "csc/src + oam/src (+ config, pkg.json, vendor, requirements.txt) ← $SCRIPT_DIR"
        n_changed=$((n_changed+1))
    fi

    # ── oam-svc 독립 모듈 (oam_base_service_split D5; csc_standalone_module.md P4/P6) ──
    #    thin 앱: 자기 src + config + pkg.json 만 동봉. 공유 라이브러리(oam/vendor·oam/src)는
    #    런타임에 같은 노드의 oam 설치본을 sys.path glob 으로 mount → tarball 에 복제하지 않음.
    #    csc/src 는 더 이상 마운트하지 않음(P4) — csc 와 코드 비공유, 계약(HTTP/JWT/DB)만.
    if [[ $did_oamsvc -eq 1 ]]; then
        mkdir -p "$DIST_DIR/oam-svc/src" "$DIST_DIR/oam-svc/config"
        if command -v rsync >/dev/null 2>&1; then
            rsync -a --delete-excluded --exclude='__pycache__' --exclude='*.pyc' \
                "$SCRIPT_DIR/ems/service/oam/src/" "$DIST_DIR/oam-svc/src/"
        else
            cp -r "$SCRIPT_DIR/ems/service/oam/src/." "$DIST_DIR/oam-svc/src/"
        fi
        find "$DIST_DIR/oam-svc/src" -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
        [[ -d "$SCRIPT_DIR/ems/service/oam/config" ]] && \
            cp -f "$SCRIPT_DIR/ems/service/oam/config/"*.json "$SCRIPT_DIR/ems/service/oam/config/"*.sample "$DIST_DIR/oam-svc/config/" 2>/dev/null
        [[ -f "$SCRIPT_DIR/ems/service/oam/pkg.json" ]] && \
            cp -f "$SCRIPT_DIR/ems/service/oam/pkg.json" "$DIST_DIR/oam-svc/pkg.json"
        ok "oam-svc/src (+ config, pkg.json) ← $SCRIPT_DIR"
        n_changed=$((n_changed+1))
    fi

    # ── Agent 바이너리 + 운영 도구 (bin/lib/keepalived/systemd) ──
    if [[ $did_agent -eq 1 ]]; then
        _ensure_agent_vendor_keepalived
        mkdir -p "$DIST_DIR/agent"
        cp -f "$SCRIPT_DIR/agent/cims_agent.py"     "$DIST_DIR/agent/"
        cp -f "$SCRIPT_DIR/agent/install-agent.sh"  "$DIST_DIR/agent/"
        chmod +x "$DIST_DIR/agent/install-agent.sh"
        [[ -f "$SCRIPT_DIR/agent/pkg.json" ]] && cp -f "$SCRIPT_DIR/agent/pkg.json" "$DIST_DIR/agent/"
        # 운영 도구 (cims-svc / cims-ha / cims-health / cims-notify + lifecycle.sh / ha.sh) + vendor deb
        if command -v rsync >/dev/null 2>&1; then
            for sub in bin lib keepalived systemd vendor; do
                [[ -d "$SCRIPT_DIR/agent/$sub" ]] && \
                    rsync -a --delete --exclude='out/' --exclude='ha.json' \
                          "$SCRIPT_DIR/agent/$sub/" "$DIST_DIR/agent/$sub/"
            done
        else
            for sub in bin lib keepalived systemd vendor; do
                [[ -d "$SCRIPT_DIR/agent/$sub" ]] && \
                    { rm -rf "$DIST_DIR/agent/$sub"; cp -r "$SCRIPT_DIR/agent/$sub" "$DIST_DIR/agent/$sub"; }
            done
        fi
        chmod +x "$DIST_DIR/agent/bin/"* 2>/dev/null || true
        chmod +x "$DIST_DIR/agent/lib/"*.sh 2>/dev/null || true
        chmod +x "$DIST_DIR/agent/keepalived/"*.sh 2>/dev/null || true
        ok "agent (+ bin/lib/keepalived/systemd) ← $SCRIPT_DIR/agent"
        n_changed=$((n_changed+1))
    fi

    # ── 관리 스크립트 (cims.sh, configure.sh + scripts/lib) ─────
    if [[ $did_scripts -eq 1 ]]; then
        cp -f "$SCRIPT_DIR/cims.sh"      "$DIST_DIR/cims.sh"      && chmod +x "$DIST_DIR/cims.sh"
        cp -f "$SCRIPT_DIR/configure.sh" "$DIST_DIR/configure.sh" && chmod +x "$DIST_DIR/configure.sh"
        # 공용 라이브러리 — dist 의 cims.sh/configure.sh 가 source (자립 실행 필수).
        # sync.sh/package.sh 도 복사 — dist 에서 호출 시 "소스 트리 전용" 명확 에러.
        mkdir -p "$DIST_DIR/scripts/lib"
        cp -f "$SCRIPT_DIR/scripts/lib/common.sh" "$DIST_DIR/scripts/lib/common.sh"
        cp -f "$SCRIPT_DIR/scripts/sync.sh" "$SCRIPT_DIR/scripts/package.sh" "$DIST_DIR/scripts/" && \
            chmod +x "$DIST_DIR/scripts/sync.sh" "$DIST_DIR/scripts/package.sh"
        ok "scripts ← cims.sh, configure.sh, scripts/{lib/common,sync,package}.sh"
        n_changed=$((n_changed+1))
    fi

    # ── 컴포넌트별 pkg.json (description 소스) ──────────────────
    if [[ $did_pkg -eq 1 ]]; then
        for t in csp cmp csc cspsim; do
            [[ -f "$SCRIPT_DIR/$t/pkg.json" ]] && cp -f "$SCRIPT_DIR/$t/pkg.json" "$DIST_DIR/$t/pkg.json" 2>/dev/null || true
        done
        [[ -f "$SCRIPT_DIR/ems/core/console/pkg.json" ]] && cp -f "$SCRIPT_DIR/ems/core/console/pkg.json" "$DIST_DIR/console/pkg.json" 2>/dev/null || true
        ok "pkg-meta ← 각 모듈 루트의 pkg.json"
        n_changed=$((n_changed+1))
    fi

    # ── Console 정적 빌드 (Vite) — base/svc 분리 ─────────────────
    # VITE_CONSOLE_TARGET=prod — sync 도 배포본 dist 기준 (TB-Console 은 dev 서버 별도)
    # 콘솔 소스 = ems/core/console(공통, Vite 루트) + ems/service/console(서비스 팩, @svc).
    # 두 벌 빌드:
    #   svc(full): base 메뉴 + 서비스 메뉴 전부 → dist/console/dist (oam-svc 패키지 동봉)
    #   base     : base 메뉴만 (VITE_CONSOLE_PROFILE=base, @svc manifest DCE 제거)
    #              → dist/console/dist-base → oam-base 패키지 동봉 (부트스트랩 기본 UI)
    # oam(base 게이트웨이)은 동봉 base 를 기본 서빙하다가, oam-svc(동봉 svc 콘솔)이
    # 배포되면 console_static.resolve 가 그쪽(svc=full)을 우선 서빙 → 자동 승격.
    if [[ $did_console -eq 1 ]]; then
        mkdir -p "$DIST_DIR/console"
        # svc 팩(ems/service/console)이 core 루트 밖이라 bare import(react 등) 해석을 위해
        # core 의 node_modules 를 svc 디렉토리에 symlink (idempotent; node_modules 는 git 제외).
        if [[ -d "$SRC_CONSOLE/node_modules" ]]; then
            ln -sfn ../../core/console/node_modules "$SCRIPT_DIR/ems/service/console/node_modules" 2>/dev/null || true
        fi
        # 1) svc(full) — base + 서비스 메뉴
        ( cd "$SRC_CONSOLE" && VITE_CONSOLE_TARGET=prod npm run build 2>&1 | tail -3 )
        if [[ -d "$SRC_CONSOLE/dist" ]]; then
            rm -rf "$DIST_DIR/console/dist"
            cp -r "$SRC_CONSOLE/dist" "$DIST_DIR/console/dist"
            cp -f "$SRC_CONSOLE/nginx.conf" "$DIST_DIR/console/nginx.conf" 2>/dev/null || true
            ok "console(svc=base+서비스) ← cims-console/dist"
        else
            err "cims-console/dist 없음 (svc 빌드 실패?)"
        fi
        # 2) base — base 메뉴만 (oam-base 동봉용)
        ( cd "$SRC_CONSOLE" && VITE_CONSOLE_TARGET=prod VITE_CONSOLE_PROFILE=base npm run build 2>&1 | tail -3 )
        if [[ -d "$SRC_CONSOLE/dist" ]]; then
            rm -rf "$DIST_DIR/console/dist-base"
            cp -r "$SRC_CONSOLE/dist" "$DIST_DIR/console/dist-base"
            ok "console-base(base 메뉴만) ← cims-console/dist"
        else
            err "cims-console/dist 없음 (base 빌드 실패?)"
        fi
        n_changed=$((n_changed+1))
    fi

    # phone(cims-phone) 은 재설계 예정 — sync 대상에서 제외 (빌드/패키징도 제외).

    echo ""
    info "sync 완료 ($n_changed 개 대상). 서비스 재기동: ./cims.sh restart <name>"
}

# OAM 녹취 변환툴(ffmpeg/ffprobe) vendor 자동 채움 — 빌드 시 정적 바이너리 다운로드.
# 녹취 재생(raw RTP→mp4 변환)에 필요. air-gapped 런타임 설치 회피 위해 패키지에 동봉.
# idempotent (ffmpeg+ffprobe 둘 다 있으면 skip). CIMS_SKIP_VENDOR_FETCH=1 로 끔.
# 소스: 정적 빌드(amd64). CIMS_FFMPEG_URL 로 사내 미러/다른 빌드 지정 가능.
# 결과: oam/vendor/bin/{ffmpeg,ffprobe} (oam_app 이 자동 탐지, .gitignore 처리됨).
_FFMPEG_STATIC_URL_DEFAULT="https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz"

_ensure_oam_vendor_ffmpeg() {
    local bin_dir="$SCRIPT_DIR/ems/core/oam/vendor/bin"
    mkdir -p "$bin_dir"

    [[ -n "${CIMS_SKIP_VENDOR_FETCH:-}" ]] && return 0
    # 이미 둘 다 실행 가능하면 skip (idempotent)
    [[ -x "$bin_dir/ffmpeg" && -x "$bin_dir/ffprobe" ]] && return 0

    if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
        warn "curl/wget 없음 — oam/vendor/bin ffmpeg 자동 다운로드 불가 (수동 채움 필요)"
        return 0
    fi

    local url="${CIMS_FFMPEG_URL:-$_FFMPEG_STATIC_URL_DEFAULT}"
    local tmp; tmp="$(mktemp -d)"
    local tarball="$tmp/ffmpeg-static.tar.xz"
    info "OAM vendor: ffmpeg 정적 빌드 다운로드 중 ($url) ..."
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 2 -o "$tarball" "$url" 2>/dev/null \
            || { warn "ffmpeg 다운로드 실패 ($url) — 패키지에 변환툴 미포함. CIMS_FFMPEG_URL 로 미러 지정 가능."; rm -rf "$tmp"; return 0; }
    else
        wget -q -O "$tarball" "$url" \
            || { warn "ffmpeg 다운로드 실패 ($url) — 패키지에 변환툴 미포함. CIMS_FFMPEG_URL 로 미러 지정 가능."; rm -rf "$tmp"; return 0; }
    fi

    # 압축 해제 (.tar.xz / .tar.gz 모두 시도) 후 ffmpeg/ffprobe 추출
    if ! tar -xf "$tarball" -C "$tmp" 2>/dev/null; then
        warn "ffmpeg tarball 해제 실패 ($tarball)"; rm -rf "$tmp"; return 0
    fi
    local f found
    for f in ffmpeg ffprobe; do
        found="$(find "$tmp" -type f -name "$f" 2>/dev/null | head -1)"
        if [[ -n "$found" ]]; then
            cp -f "$found" "$bin_dir/$f" && chmod +x "$bin_dir/$f"
        fi
    done
    rm -rf "$tmp"

    if [[ -x "$bin_dir/ffmpeg" ]]; then
        local v; v="$("$bin_dir/ffmpeg" -version 2>/dev/null | head -1)"
        ok "OAM vendor: ffmpeg 동봉 완료 → $bin_dir (${v:-ffmpeg})"
        [[ -x "$bin_dir/ffprobe" ]] || warn "ffprobe 추출 실패 — 길이검출 fallback 사용(재생엔 영향 적음)"
    else
        warn "ffmpeg 추출 실패 — 패키지에 변환툴 미포함"
    fi
}

# agent vendor 자동 채움 — keepalived offline 설치용 deb 6종
# 누락된 패키지만 apt-get download 로 받음 (sudo 불필요). idempotent.
# CIMS_SKIP_VENDOR_FETCH=1 로 끌 수 있음 (인터넷/apt 없는 환경).
# keepalived 전용 deps (uninstall 시 purge 대상). libmnl0 은 keepalived 와 iproute2(`ip`)
# 가 공유하는 base 의존성이라 vendor/base 로 분리 — keepalived uninstall 이 purge 해도
# `ip` 가 깨지지 않도록 (vendor/base/README.md 참조).
_KEEPALIVED_DEPS=(keepalived libnftnl11 libnl-3-200 libnl-genl-3-200 libsnmp40t64)
# OS base 공유 의존성 — 모든 노드 필요, uninstall 시 제거하지 않음. libmnl0 = `ip` 의존성.
_BASE_DEPS=(libmnl0)

# vendor 서브디렉터리에 누락된 deb 를 apt-get download 로 채움 (sudo 불필요, idempotent).
_ensure_agent_vendor_dir() {
    local sub="$1"; shift
    local deps=("$@")
    local vendor_dir="$SCRIPT_DIR/agent/vendor/$sub"
    mkdir -p "$vendor_dir"

    [[ -n "${CIMS_SKIP_VENDOR_FETCH:-}" ]] && return 0

    local missing=() pkg
    for pkg in "${deps[@]}"; do
        compgen -G "$vendor_dir/${pkg}_*.deb" >/dev/null 2>&1 || missing+=("$pkg")
    done
    [[ ${#missing[@]} -eq 0 ]] && return 0

    if ! command -v apt-get &>/dev/null; then
        warn "apt-get 미지원 환경 — agent/vendor/$sub 누락: ${missing[*]} (수동 채움 필요)"
        return 0
    fi

    info "agent/vendor/$sub: ${#missing[@]}/${#deps[@]} 누락 → apt-get download (${missing[*]})"
    if ! ( cd "$vendor_dir" && apt-get download "${missing[@]}" >/dev/null 2>&1 ); then
        warn "apt-get download 실패 — vendor 미완성 가능 (인터넷/apt 캐시 확인). CIMS_SKIP_VENDOR_FETCH=1 로 차단"
        return 0
    fi
    ok "agent/vendor/$sub: ${missing[*]} 자동 채움"
}

_ensure_agent_vendor_keepalived() {
    _ensure_agent_vendor_dir keepalived "${_KEEPALIVED_DEPS[@]}"
    _ensure_agent_vendor_dir base       "${_BASE_DEPS[@]}"
}

# 버전 유틸리티 — pkg.json 에 저장된 semver 를 읽고/bump/쓰기

cmd_sync "$@"
