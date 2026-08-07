#!/usr/bin/env bash
# =============================================================
# scripts/package.sh — 배포 패키징 (모듈 tarball + manifest) / 부트스트랩 인스톨러
# cims.sh pkg | installer 의 위임 대상 (CLI 계약은 cims.sh 가 유지).
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

# cmd_pkg 의 auto-sync 는 분리된 sync.sh 로 위임 (이동 코드 원형 유지용 shim)
cmd_sync() { "$REPO_ROOT/scripts/sync.sh" "$@"; }

cmd_installer() {
    # 상용 부트스트랩 인스톨러 조립 — base 운영평면(oam + console + agent)만 동봉.
    # 서비스 종속 모듈(csp/cmp/csc/...)은 제외 — 3단계(콘솔 패키지 등록)에서 별도 반입.
    # 산출: build/dist/packages/cims-bootstrap-<oam버전>.tar.gz
    local out_dir="$DIST_DIR/packages"
    local _latest
    _latest() { ls -1 "$out_dir"/$1-[0-9]*.tar.gz 2>/dev/null | sort -V | tail -1; }
    local oam_tar agt_tar
    oam_tar=$(_latest oam); agt_tar=$(_latest agent)
    local miss=()
    [[ -z "$oam_tar" ]] && miss+=(oam)
    [[ -z "$agt_tar" ]] && miss+=(agent)
    if [[ ${#miss[@]} -gt 0 ]]; then
        err "installer 조립 불가 — 패키지 없음: ${miss[*]} (./cims.sh pkg ${miss[*]} 먼저)"
        return 1
    fi
    local oam_ver; oam_ver=$(basename "$oam_tar" .tar.gz | sed 's/^oam-//')
    local stage="$DIST_DIR/.installer.$$"
    rm -rf "$stage"
    mkdir -p "$stage/cims-bootstrap/packages"
    cp -f "$SCRIPT_DIR/deployment/bootstrap/install.sh" "$stage/cims-bootstrap/install.sh"
    chmod +x "$stage/cims-bootstrap/install.sh"
    cp -f "$oam_tar" "$agt_tar" "$stage/cims-bootstrap/packages/"

    # ── oam_base_service_split — console 은 oam-base 패키지에 동봉(별도 console 모듈 폐기) ──
    # 부트스트랩은 oam(+동봉 console, 항상 full) + agent 만 시드. base/full 프로파일 빌드 폐기 —
    # 위젯 노출은 런타임 카탈로그(D1/D7: 설치된 서비스 ∩ RBAC)가 게이팅한다.
    cat > "$stage/cims-bootstrap/README.md" <<EOR
# CIMS 부트스트랩 인스톨러 (base 운영평면)

상용(Private) 망 1단계 설치 — 서비스 모듈 없이 OAM + Console + Agent 에셋만.

    sudo ./install.sh                # /opt/cims-agent, HTTPS :4419
    sudo ./install.sh --admin-pass '<비밀번호>' --port 4419

설치 후: https://<서버IP>:4419/ 접속 (admin) →
  2) 시스템/서버 구성 (각 서버 install-command 로 agent 설치)
  3) 패키지 등록 (서비스 모듈 + 본 인스톨러 구성요소 업데이트 패키지)
  4) 패키지 설치  5) 패키지 설정

콘솔은 oam-base 패키지에 동봉(항상 full). 서비스 메뉴/위젯은 런타임 카탈로그가
설치된 서비스 모듈(csc/oam-svc 등) ∩ 사용자 RBAC 로 게이팅 — 서비스 모듈을
설치하면 해당 위젯이 자동 노출된다(별도 console 패키지 불요).

동봉: $(basename "$oam_tar") (console 포함) / $(basename "$agt_tar")
EOR
    local out="$out_dir/cims-bootstrap-${oam_ver}.tar.gz"
    ( cd "$stage" && tar czf "$out" cims-bootstrap )
    rm -rf "$stage"
    local size; size=$(stat -c%s "$out" 2>/dev/null || echo 0)
    ok "부트스트랩 인스톨러: $(basename "$out") ($(numfmt --to=iec --suffix=B "$size" 2>/dev/null || echo ${size}B))"
}

cmd_pkg() {
    # 3단계 중 3단계 (패키지화): configure 까지 끝난 build/dist 를 모듈별 tarball 로 묶는다.
    # 출력: build/dist/packages/<name>-<ver>.tar.gz
    # 각 tarball 최상위에 meta.json (name, version, description, build/git/changelog) +
    # config_template.json (설정 스키마) 포함.
    #
    # 버전 결정 로직:
    #   1) -v <ver> 지정: 모든 대상 모듈이 그 버전 사용 + pkg.json 업데이트
    #   2) --no-bump:     현재 pkg.json 의 version 그대로 사용 (재패키징)
    #   3) 기본:          pkg.json 의 patch 를 +1 (auto-bump) + pkg.json 업데이트
    local version=""
    local changelog=""
    local no_bump=0
    local no_sync=0
    local targets=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -v|--version)   version="$2"; shift 2 ;;
            -m|--changelog) changelog="$2"; shift 2 ;;
            --no-bump)      no_bump=1; shift ;;
            --no-sync)      no_sync=1; shift ;;
            -*) err "알 수 없는 옵션: $1"; return 1 ;;
            *)  targets+=("$1"); shift ;;
        esac
    done
    # default targets — csp 바이너리는 다용도 → csp/isp/psp 3 tarball (소스/dist 디렉토리는 동일,
    # tarball 이름과 meta.json 의 name 만 분리 — Roles/LocalIp 는 deploy overlay 가 결정).
    # cmp 바이너리도 동일 → cmp/imp/pmp.
    # oam_base_service_split — console 은 oam-base 패키지에 동봉(별도 모듈 폐기). 명시 시만 단독 패키징.
    # cwrtc/phone 은 재설계 예정 — 빌드/dist/패키징 제외 (CMakeLists.txt 동기).
    [[ ${#targets[@]} -eq 0 ]] && targets=(cmp pmp imp cmdp csp psp isp csc oam oam-svc cspsim agent)

    if [[ ! -d $DIST_DIR ]]; then
        err "dist 디렉토리 없음: $DIST_DIR (먼저 ./cims.sh build)"
        return 1
    fi

    # ── 소스 → dist auto-sync (#15) ───────────────────────────────────────
    # cmd_pkg 가 dist 를 tar 하므로, source 가 변경됐는데 dist 에 미반영이면 옛 코드가
    # tarball 에 박힘. 이 함정에 반복적으로 막힌 회기 이력 (agent 0.0.13/16/20, CSC handler)
    # 으로 인해 자동 sync 를 기본 동작으로. --no-sync 로 끄기 가능.
    # C++ 바이너리 (csp/cmp/cspsim) 는 cmake build 가 별도 → 여기서는 mtime 비교 후 warn.
    if [[ $no_sync -ne 1 && -n "$SRC_CONSOLE" ]]; then
        local -A _sync_set=()
        # pkg-meta / scripts 는 어느 컴포넌트를 패키징하든 항상 동기화 (cims.sh / pkg.json 박힘 방지)
        _sync_set[pkg-meta]=1
        _sync_set[scripts]=1
        local _t
        for _t in "${targets[@]}"; do
            case "$_t" in
                csc) _sync_set[csc]=1 ;;   # OAM 분리 Phase 2 — sync csc 가 oam/src 도 함께
                oam) _sync_set[csc]=1; _sync_set[console]=1 ;;  # oam-base: csc 블록이 oam/src(자체 httpsrv/util/services)도 동기화 + console 동봉 (oam 은 자족 — csc 코드 미동봉)
                oam-svc) _sync_set[oam-svc]=1; _sync_set[csc]=1; _sync_set[console]=1 ;;  # oam-svc = thin(자기 src) + svc(full) console 동봉; csc 블록이 oam/src 동기화 → 런타임/dev import 가능
                agent)   _sync_set[agent]=1 ;;
                console) _sync_set[console]=1 ;;
            esac
        done
        local _sync_list=("${!_sync_set[@]}")
        if [[ ${#_sync_list[@]} -gt 0 ]]; then
            info "auto-sync (소스 → dist): ${_sync_list[*]}"
            cmd_sync "${_sync_list[@]}" || warn "auto-sync 일부 실패 — 옛 dist 로 패키징 진행"
        fi
    elif [[ $no_sync -eq 1 ]]; then
        warn "--no-sync 모드: source → dist sync 건너뜀 (옛 dist 로 패키징됨)"
    fi

    # C++ 바이너리 stale 경고 (dist 바이너리가 src 보다 오래된 경우)
    local -A _bin_checked=()
    local _bin_key _bin _src
    for _t in "${targets[@]}"; do
        case "$_t" in
            csp|psp|isp) _bin_key="csp" ;;
            cmp|pmp|imp) _bin_key="cmp" ;;
            cspsim)      _bin_key="cspsim" ;;
            *)           _bin_key="" ;;
        esac
        [[ -z "$_bin_key" || -n "${_bin_checked[$_bin_key]:-}" ]] && continue
        _bin_checked[$_bin_key]=1
        _bin="$DIST_DIR/$_bin_key/bin/$_bin_key"
        _src="$SCRIPT_DIR/$_bin_key/src"
        if [[ -f "$_bin" && -d "$_src" ]]; then
            if find "$_src" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.h' -o -name '*.hpp' \) -newer "$_bin" 2>/dev/null | grep -q .; then
                warn "$_bin_key: dist 바이너리가 src 보다 오래됨 → 'cims.sh build' 후 다시 pkg 권장"
            fi
        fi
    done

    # 컴포넌트별 소스 루트 매핑 — 각 소스 루트의 pkg.json 에서 name/description 를 가져옴
    # (dist/ 밖에서 실행되는 경우만 소스 루트가 있으며, 그 외에는 dist/<comp>/pkg.json 로 fallback)
    _src_root_for() {
        case "$1" in
            csp|psp|isp) echo "$SCRIPT_DIR/csp" ;;   # 동일 csp 바이너리 + 동일 config_template
            cmp|pmp|imp) echo "$SCRIPT_DIR/cmp" ;;   # 동일 cmp 바이너리 + 동일 config_template
            cmdp)        echo "$SCRIPT_DIR/cmdp" ;;  # MCData media plane (MSRP)
            csc)         echo "$SCRIPT_DIR/csc" ;;
            oam)         echo "$SCRIPT_DIR/ems/core/oam" ;;   # OAM 분리 Phase 2 — 같은 cims-csc 프로세스, 별도 tarball
            oam-svc)    echo "$SCRIPT_DIR/ems/service/oam" ;;  # oam_base_service_split D5 — base 게이트웨이 뒤 독립 서비스 모듈
            console)     echo "$SCRIPT_DIR/ems/core/console" ;;
            cspsim)      echo "$SCRIPT_DIR/cspsim" ;;
            agent)       echo "$SCRIPT_DIR/agent" ;;
            *)           echo "" ;;
        esac
    }

    # Tarball 안 모듈 디렉토리 이름 — 패키지 정체성 분리: psp/isp/pmp/imp 도
    # 자기 이름의 디렉토리로 들어감. dist 트리는 csp/cmp 한 종 그대로 두고,
    # 변종은 pkg 단계에서 staging 디렉토리 (dist/csp 복사 + 바이너리/config rename)
    # 로 새 디렉토리를 만든 후 tar.
    _src_sub_for() {
        case "$1" in
            *) echo "$1" ;;   # 모든 컴포넌트 자기 이름
        esac
    }

    # 변종 (psp/isp/pmp/imp) 의 base dist 디렉토리 — 같은 ELF 사용
    _base_dist_for() {
        case "$1" in
            psp|isp) echo "csp" ;;
            pmp|imp) echo "cmp" ;;
            *)       echo "" ;;
        esac
    }

    # Git 정보 (가능한 경우)
    local git_sha="" git_branch=""
    if git -C "$SCRIPT_DIR" rev-parse --git-dir >/dev/null 2>&1; then
        git_sha=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo "")
        git_branch=$(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")
    fi
    local packaged_at; packaged_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    local packaged_by="${USER:-unknown}@$(hostname -s 2>/dev/null || echo unknown)"

    local out_dir="$DIST_DIR/packages"
    mkdir -p "$out_dir"

    local t src_sub tar_file build_date pkg_root base_dist stage
    for t in "${targets[@]}"; do
        case "$t" in
            cmp|pmp|imp|cmdp|csp|psp|isp|csc|oam|oam-svc|console|cspsim|agent)
                src_sub=$(_src_sub_for "$t") ;;
            cwrtc|phone) err "$t: 재설계 예정 — 빌드/패키징 제외됨"; continue ;;
            *) err "알 수 없는 컴포넌트: $t"; continue ;;
        esac

        # 변종 (psp/isp/pmp/imp): staging 에 base dist (csp/cmp) 복사 + 바이너리/config
        # 이름을 변종 이름으로 rename → tar root 가 staging 이 됨. dist/csp 자체는 손대지 않음.
        pkg_root="$DIST_DIR"
        stage=""
        base_dist=$(_base_dist_for "$t")
        if [[ -n "$base_dist" ]]; then
            if [[ ! -d "$DIST_DIR/$base_dist" ]]; then
                warn "skip: $DIST_DIR/$base_dist 없음 (variant=$t base=$base_dist)"; continue
            fi
            stage="$DIST_DIR/.pkgstage.$$.${t}"
            rm -rf "$stage"
            mkdir -p "$stage/$t"
            # base dist 의 내용 그대로 복사 (cp -a 로 권한/심볼릭 보존).
            cp -a "$DIST_DIR/$base_dist/." "$stage/$t/"
            # 바이너리 rename (csp → psp 등).
            [[ -f "$stage/$t/bin/$base_dist" ]] && mv "$stage/$t/bin/$base_dist" "$stage/$t/bin/$t"
            # 시작 스크립트 rename (있을 때만 — csp.sh → psp.sh).
            [[ -f "$stage/$t/bin/$base_dist.sh" ]] && mv "$stage/$t/bin/$base_dist.sh" "$stage/$t/bin/$t.sh"
            # config 파일 rename (configure 후라면 있고, 빌드 직후라면 없음).
            [[ -f "$stage/$t/config/$base_dist.json" ]] && mv "$stage/$t/config/$base_dist.json" "$stage/$t/config/$t.json"
            # cims.sh 도 staging 으로 (tar root 에 포함).
            [[ -f "$DIST_DIR/cims.sh" ]] && cp "$DIST_DIR/cims.sh" "$stage/"
            pkg_root="$stage"
        elif [[ ! -d "$DIST_DIR/$src_sub" ]]; then
            warn "skip: $DIST_DIR/$src_sub 없음 (target=$t src_sub=$src_sub)"; continue
        fi

        # ── oam: P6 (csc_standalone_module.md) — oam 은 자족(self-contained).
        #    httpsrv/util/services 를 oam/src 자체 보유(cmd_sync 가 rsync) → csc/src 복사 폐지.
        #    런타임도 csc/src 마운트 안 함(P3b). 서비스 코드(mcptt/idms 등)는 csc 패키지에만.
        if [[ "$t" == "oam" ]]; then
            stage="$DIST_DIR/.pkgstage.$$.${t}"
            rm -rf "$stage"
            mkdir -p "$stage"
            cp -a "$DIST_DIR/oam" "$stage/oam"
            find "$stage/oam/src" -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null
            # 콘솔 base/svc 분리 (백엔드 oam-base/oam-svc 와 대칭) —
            #   oam-base 패키지엔 **base 메뉴 콘솔(dist-base)** 만 동봉.
            #   oam 이 <root>/oam/console/dist 를 서빙(console_static.resolve 의 번들 후보).
            #   svc(full=base+서비스) 콘솔은 **oam-svc 패키지에 동봉**(아래 oam-svc 블록) →
            #   oam-svc 배포 시 base OAM resolver 가 그쪽을 우선 서빙(자동 승격).
            #   (dist-base 미존재 시 svc full 로 폴백 → 구 동작 호환)
            local _condist="$DIST_DIR/console/dist-base"
            [[ -d "$_condist" ]] || _condist="$DIST_DIR/console/dist"
            [[ -d "$_condist" ]] || _condist="$SRC_CONSOLE/dist"
            if [[ -d "$_condist" ]]; then
                rm -rf "$stage/oam/console"
                mkdir -p "$stage/oam/console"
                cp -a "$_condist" "$stage/oam/console/dist"
                find "$stage/oam/console" -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null
            else
                warn "oam: console dist 미발견($_condist) — 콘솔 미동봉 패키지(서빙 비활성)"
            fi
            pkg_root="$stage"
        fi

        # ── oam-svc: 콘솔 base/svc 분리 — svc(full=base+서비스) 콘솔을 oam-svc 패키지에 동봉.
        #    base OAM resolver 가 배포된 oam-svc 의 console/dist 를 oam-base 동봉 base 콘솔보다
        #    우선 서빙 → oam-svc 배포 시 콘솔이 자동으로 풀 메뉴로 승격(백엔드 분리와 대칭).
        if [[ "$t" == "oam-svc" ]]; then
            stage="$DIST_DIR/.pkgstage.$$.${t}"
            rm -rf "$stage"
            mkdir -p "$stage"
            cp -a "$DIST_DIR/oam-svc" "$stage/oam-svc"
            find "$stage/oam-svc/src" -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null
            local _svcdist="$DIST_DIR/console/dist"
            [[ -d "$_svcdist" ]] || _svcdist="$SRC_CONSOLE/dist"
            if [[ -d "$_svcdist" ]]; then
                rm -rf "$stage/oam-svc/console"
                mkdir -p "$stage/oam-svc/console"
                cp -a "$_svcdist" "$stage/oam-svc/console/dist"
                find "$stage/oam-svc/console" -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null
            else
                warn "oam-svc: svc console dist 미발견($_svcdist) — 콘솔 미동봉"
            fi
            pkg_root="$stage"
        fi

        # build_date = 컴포넌트 dist 디렉토리 안에서 가장 최근 파일의 mtime (base dist 기준 — staging 은 cp 로 mtime 갱신될 수 있음).
        local _bd_root="$DIST_DIR/${base_dist:-$src_sub}"
        build_date=$(find "$_bd_root" -type f -printf '%T@\n' 2>/dev/null \
                        | sort -nr | head -1 \
                        | xargs -I{} date -u -d @{} +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo "")

        # 소스 루트 pkg.json 에서 description/version 을 읽음 (없으면 dist/<comp>/pkg.json fallback)
        local comp_meta=""
        local src_root; src_root=$(_src_root_for "$t")
        for cand in "$src_root/pkg.json" "$DIST_DIR/$t/pkg.json"; do
            [[ -n $cand && -f $cand ]] && comp_meta="$cand" && break
        done
        [[ -z $comp_meta ]] && warn "$t: pkg.json 없음 — description 공란"

        # 이 모듈의 실제 적용 버전 결정 (explicit > no-bump > auto-bump patch).
        # 변종 (psp/isp/pmp/imp) 은 base (csp/cmp) 의 version 을 read-only 로 따라감 —
        # 9 tarball 이 같은 patch+1 을 3번 누적하지 않도록.
        local effective_no_bump="$no_bump"
        case "$t" in
            psp|isp|pmp|imp) effective_no_bump=1 ;;
        esac
        local comp_ver; comp_ver=$(_resolve_version "$comp_meta" "$version" "$effective_no_bump")
        # pkg.json 에 반영 (base 만 — 변종은 read-only)
        if [[ -n $comp_ver && "$effective_no_bump" != "1" ]]; then
            [[ -n $comp_meta ]] && _pkg_write_version "$comp_meta" "$comp_ver"
            local dist_meta="$DIST_DIR/$t/pkg.json"
            [[ -f $dist_meta && "$dist_meta" != "$comp_meta" ]] && _pkg_write_version "$dist_meta" "$comp_ver"
        fi

        # meta.json 생성 (pkg_root 안에 임시로 작성 → tar 루트에 추가 후 삭제;
        # 변종은 staging, 그 외는 DIST_DIR).
        local tmp_meta="$pkg_root/.pkgmeta.$$.json"
        python3 - "$comp_meta" "$t" "$comp_ver" "$build_date" "$git_sha" "$git_branch" \
                  "$packaged_at" "$packaged_by" "$changelog" <<'PYEOF' > "$tmp_meta"
import sys, json, os
meta_file, name, version, build_date, git_sha, git_branch, packaged_at, packaged_by, changelog = sys.argv[1:]
desc = ""
service = None
ha_capability = None
gateway = None
src_pkg = {}
# 소스 루트 pkg.json 은 단일 컴포넌트 형식: { "name": "...", "description": "...", "ha_capability": "...", "service": {...}, "gateway": {...} }
if meta_file and os.path.isfile(meta_file):
    try:
        with open(meta_file, 'r', encoding='utf-8') as f:
            entry = json.load(f)
        if isinstance(entry, dict):
            # 단일 컴포넌트 스키마
            if "description" in entry:
                desc = entry.get("description", "")
                if isinstance(entry.get("service"), dict):
                    service = entry["service"]
                ha_capability = entry.get("ha_capability")
                if isinstance(entry.get("gateway"), dict):
                    gateway = entry["gateway"]
                src_pkg = entry
            # 구(舊) 레지스트리 스키마 (후방 호환)
            elif name in entry and isinstance(entry[name], dict):
                desc = entry[name].get("description", "")
                if isinstance(entry[name].get("service"), dict):
                    service = entry[name]["service"]
                ha_capability = entry[name].get("ha_capability")
                if isinstance(entry[name].get("gateway"), dict):
                    gateway = entry[name]["gateway"]
                src_pkg = entry[name]
    except Exception:
        pass
# csp/cmp 변종은 base description 끝에 역할 suffix 추가 (식별용).
_ROLE_SUFFIX = {
    "psp": " · PSP role (PTT CSCF + PTT-AS)",
    "isp": " · ISP role (IBCF / IP-PBX trunk)",
    "pmp": " · PMP role (PTT RTP/Floor)",
    "imp": " · IMP role (IBCF media)",
}
if name in _ROLE_SUFFIX:
    desc = (desc or "").rstrip() + _ROLE_SUFFIX[name]
meta = {
    "name": name,
    "version": version,
    "description": desc,
    "build_date": build_date or None,
    "git_sha": git_sha or None,
    "git_branch": git_branch or None,
    "packaged_at": packaged_at,
    "packaged_by": packaged_by,
    "changelog": changelog or "",
}
if service is not None:
    meta["service"] = service
if ha_capability is not None:
    meta["ha_capability"] = ha_capability
if gateway is not None:
    meta["gateway"] = gateway      # self-register: 모듈 선언 라우트(세그먼트) — OAM 이 배포 시 등록

# pkg.json 의 **나머지 선언도 그대로 싣는다** (passthrough).
#   옛 구현은 service/ha_capability/gateway 만 옮기는 화이트리스트라, 뒤에 추가된 선언이
#   조용히 사라졌다 — `shared_identity` 가 빠져 콘솔로 설치한 oam 에 그룹 공통 신원
#   (JwtSecret·CimsRuntimeDir)이 주입되지 않았고, 패키지 기본 경로(빌드 머신 경로)로
#   기동하다 죽어 절체가 실패했다(실측). 새 선언을 추가할 때마다 이 스크립트를 고쳐야
#   하는 구조 자체가 결함이므로, 여기서 정한 필드만 빼고 전부 넘긴다.
_OWNED = {"name", "version", "description", "build_date", "git_sha", "git_branch",
          "packaged_at", "packaged_by", "changelog", "service", "ha_capability", "gateway"}
for _k, _v in (src_pkg or {}).items():
    if _k not in _OWNED and _k not in meta:
        meta[_k] = _v
print(json.dumps(meta, indent=2, ensure_ascii=False))
PYEOF

        # config_template.json: v3 (2026-04-22) 부터 소스의 config/ 아래.
        #   tarball 에는 그대로 최상위(/config_template.json) 로 포함 (agents.py 가 루트에서 파싱).
        local tmp_tmpl="$pkg_root/.pkgtmpl.$$.json"
        local tmpl_basename=".pkgtmpl.$$.json"
        local has_template=0
        if [[ -n "$src_root" ]]; then
            local _tmpl_src=""
            if   [[ -f "$src_root/config/config_template.json" ]]; then _tmpl_src="$src_root/config/config_template.json"
            elif [[ -f "$src_root/config_template.json"       ]]; then _tmpl_src="$src_root/config_template.json"   # legacy fallback
            fi
            if [[ -n "$_tmpl_src" ]]; then
                cp "$_tmpl_src" "$tmp_tmpl"
                has_template=1
            fi
        fi

        tar_file="$out_dir/${t}-${comp_ver}.tar.gz"
        info "패키징: $t-$comp_ver  (git=$git_sha/$git_branch)"

        # tar 구성: meta.json(루트) + config_template.json(루트, 있을 때) + <component>/ + cims.sh
        local meta_basename=".pkgmeta.$$.json"
        # 런타임 산출물/상태 디렉토리는 배포에서 제외
        #  log/         : 서비스 로그 (csp/csc 등)
        #  run/         : pid 파일
        #  cache/       : CSC 설정 캐시 (고정값이 아닌 현재 상태)
        #  packages/    : 배포본 CSC 가 수집한 업로드 tarball (신규 배포에 포함되면 중복 팽창)
        #  packages_tb/ : TB-CSC 가 수집한 업로드 tarball — packages 와 별개 store.
        #                 누락 시 csc tarball 이 GB 단위로 부풀어 S5-CSC-DEPLOY-INSTALL 60s timeout.
        #  packages_trash/ : TB-CSC 삭제 보관소
        #  cdr/         : CDR 산출물
        #  dist/        : 번들러 산출물 이 아닌 상위 dist 와 혼동 방지 (cwrtc/dist 등 없음)
        ( cd "$pkg_root" && \
            tar czf "$tar_file" \
                --exclude="$src_sub/log" \
                --exclude="$src_sub/run" \
                --exclude="$src_sub/cache" \
                --exclude="$src_sub/cache_tb" \
                --exclude="$src_sub/packages" \
                --exclude="$src_sub/packages_tb" \
                --exclude="$src_sub/packages_trash" \
                --exclude="$src_sub/cdr" \
                --exclude='*.pid' --exclude='*.pyc' \
                --exclude='__pycache__' --exclude='.cache' \
                --transform="s|^$meta_basename\$|meta.json|" \
                --transform="s|^$tmpl_basename\$|config_template.json|" \
                "$meta_basename" \
                $( [[ $has_template -eq 1 ]] && echo "$tmpl_basename" ) \
                "$src_sub" $( [[ -f cims.sh ]] && echo cims.sh ) )
        rm -f "$tmp_meta"
        [[ $has_template -eq 1 ]] && rm -f "$tmp_tmpl"
        # 변종 staging cleanup
        [[ -n "$stage" && -d "$stage" ]] && rm -rf "$stage"
        local size; size=$(stat -c%s "$tar_file" 2>/dev/null || echo 0)
        ok "$(basename "$tar_file") ($(numfmt --to=iec --suffix=B "$size" 2>/dev/null || echo "${size}B"))"
    done

    # stale 버전 cleanup — 각 component 의 mtime 기준 최신 1개만 보존, 나머지 제거.
    # 배경: verify/lib/items/stage5/_native_steps.py:_latest_tarball() 의 natural-sort 가
    #   잔재 0.0.2 같은 stale tarball 을 선택 → deploy 가 OLD binary 사용.
    # 이 라운드에 패키징한 component 만 정리 (다른 컴포넌트 손대지 않음).
    local _cleaned=0 _latest _stale
    for t in "${targets[@]}"; do
        _latest=$(ls -1t "$out_dir/${t}-"[0-9]*.tar.gz 2>/dev/null | head -1)
        [[ -z "$_latest" ]] && continue
        while IFS= read -r _stale; do
            [[ "$_stale" == "$_latest" ]] && continue
            rm -f "$_stale" && _cleaned=$((_cleaned+1)) && info "stale 제거: $(basename "$_stale")"
        done < <(ls -1 "$out_dir/${t}-"[0-9]*.tar.gz 2>/dev/null)
    done
    [[ $_cleaned -gt 0 ]] && ok "stale tarball $_cleaned 개 정리"

    # manifest.json 생성/갱신 — 현재 packages/*.tar.gz 의 SHA256 + size + mtime 기록.
    # Console UI 의 다운로드 라벨 (버전 표시) 과 검증 S6 의 immutability gate 가 이 파일 사용.
    # 검증 S4-PKG-MANIFEST 가 같은 로직으로 만들지만, cmd_pkg 직후에도 항상 fresh 하도록.
    local manifest_path="$out_dir/manifest.json"
    local _git_sha="${git_sha:-}" _git_branch="${git_branch:-}"
    local _host; _host=$(hostname -s 2>/dev/null || echo unknown)
    python3 - "$out_dir" "$manifest_path" "$_git_sha" "$_git_branch" "$_host" <<'PYEOF' \
        && ok "manifest.json 갱신 → $manifest_path" \
        || warn "manifest.json 갱신 실패"
import sys, os, json, hashlib
from datetime import datetime, timezone
out_dir, out_path, git_sha, git_branch, host = sys.argv[1:6]
def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(64*1024), b''):
            h.update(chunk)
    return h.hexdigest()
entries = []
for fn in sorted(os.listdir(out_dir)):
    if not fn.endswith('.tar.gz'): continue
    full = os.path.join(out_dir, fn)
    entries.append({
        'name':   fn,
        'size':   os.path.getsize(full),
        'sha256': sha256(full),
        'mtime':  datetime.fromtimestamp(os.path.getmtime(full), tz=timezone.utc).isoformat(),
    })
manifest = {
    'ts': datetime.now(timezone.utc).astimezone().isoformat(),
    'git': {'branch': git_branch, 'sha': git_sha},
    'host': host,
    'ens_ip': '',
    'packages': entries,
}
with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(manifest, f, ensure_ascii=False, indent=2)
PYEOF

    header "[3/3] 생성된 패키지 (업로드 대상):"
    ls -lh "$out_dir"/*.tar.gz 2>/dev/null | awk '{printf "  %s  %s\n", $5, $9}'
    echo ""
    info "Console 에서 업로드: 배포 관리 → 패키지 → ＋ 업로드 (파일만 선택하면 meta 자동 인식)"

    # 상용 부트스트랩 인스톨러 자동 조립 — oam-base(console 동봉) + agent tarball 이
    # 준비된 경우에만 (개별 모듈 pkg 호출 시에는 보통 미충족 → skip).
    if ls "$out_dir"/oam-[0-9]*.tar.gz "$out_dir"/agent-*.tar.gz >/dev/null 2>&1; then
        cmd_installer || warn "부트스트랩 인스톨러 조립 실패 (개별 패키지는 정상)"
    fi
}


case "${1:-pkg}" in
    pkg)       shift || true; cmd_pkg "$@" ;;
    installer) shift || true; cmd_installer "$@" ;;
    *) err "알 수 없는 명령: $1 (pkg | installer)"; exit 1 ;;
esac
