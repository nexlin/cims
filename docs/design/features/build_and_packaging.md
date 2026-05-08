# 빌드 / 패키징 워크플로우

> 버전: 1.0 (2026-05-09)
>
> CIMS 코드를 빌드 → 변종 12개 tarball → 다운로드까지 한 화면에서 수행하는
> 워크플로우. 콘솔 메뉴 **"패키징"** (`/release/package`) + CLI `cims.sh
> build` / `cims.sh pkg` 가 동등한 진입점.

## 1. 컴포넌트 / 변종

```
8 base 컴포넌트                               + 4 변종            = 12 tarball
─────────────────────────────────             ───────────
csp     CSP (VoLTE/PTT/IBCF SIP)              psp  PSP role (PTT)
cmp     CMP (RTP relay)                       isp  ISP role (IBCF)
cwrtc   WebRTC bridge                         pmp  PMP role (PTT 미디어)
csc     Admin API + UI backend                imp  IMP role (IBCF 미디어)
console Web UI (vite)
phone   Web 단말 (vite)
cspsim  SIP/RTP 시뮬레이터
agent   배포/프로세스 제어 데몬
```

- **base** = 자기 소스 트리 + `pkg.json` 보유. 빌드 (`cmake`/`make`/`npm`) 결과가 `build/dist/<comp>/` 로 흘러간다.
- **변종 (psp/isp/pmp/imp)** = base (csp / cmp) 와 동일한 ELF + `config_template.json` 을 그대로 쓰는 패키지 정체성. tarball 안 디렉토리/바이너리/`config/<m>.json`/`<m>.sh` 만 변종 이름으로 rename. 배포 시 deployment overlay (`install_path/config.json`) 가 Roles/LocalIp/Port 분기.
- 변종은 자기 `pkg.json` 이 없고 base 의 version 을 read-only 로 따라간다 (auto-bump 누적 방지).

## 2. CLI 흐름 (소스 트리 안에서만)

```bash
# 1. 빌드 (C++ + cmake + make + npm 빌드 + dist 디렉토리 생성)
./cims.sh build                       # 현재 pkg.json 버전 그대로
./cims.sh build -v 0.0.10             # 8 base 컴포넌트의 pkg.json version 일괄 갱신
./cims.sh build -j 8                  # 병렬

# 2. 시험환경 설정 (옵션, S3/S6 의존)
./cims.sh configure --local-ip <ens160>

# 3. 패키지화 (build/dist → packages/*.tar.gz 12개 + manifest.json)
./cims.sh pkg --no-bump               # 위에서 결정된 버전 그대로 (권장)
./cims.sh pkg                         # 옛: pkg.json patch +1 자동 (변종간 drift 위험)
./cims.sh pkg -v 0.0.10               # explicit (-v 우선)
./cims.sh pkg csp psp isp             # 묶음 산출 (3종 동시)
```

> **빌드 시점 버전 결정 모델 (2026-05-08~)**: 패키지 단계의 auto-bump 가
> 변종 12종의 patch +1 누적 위험이 있어 폐기. 빌드 단계에서 `-v X.Y.Z` 로
> 모든 base pkg.json 을 동기화한 뒤 `pkg --no-bump` 로 그대로 산출하는 흐름이
> 현재 정책. 콘솔 ▶ 버튼이 이 모델로 묶여 있다.

## 3. 콘솔 통합 — `/release/package`

좌측 메뉴 "패키징" → "패키징". 카드 그리드 8장 (base 8 컴포넌트):

```
┌─ csp  [critical?]                                   ┐
│ ¹ 설정    [템플릿] [설정]                          │   2 col × 2 row 그리드
│ ² 실행    [on/off] pid=...  [▶|■] [↻]              │
│ ³ 다운로드 [⤓ csp v0.0.10] [⤓ psp v0.0.10] [⤓ isp]│   변종 inline
└────────────────────────────────────────────────────┘
```

- **카드 키 = 프로세스 (`ServiceName`)** = `cims.sh` 가 인식하는 base 모듈명.
- **`packageVariants`** = ³ 다운로드 영역에 노출되는 패키지 산출물. `csp` 카드는 `[csp, psp, isp]`, `cmp` 카드는 `[cmp, pmp, imp]`. 그 외는 단일.
- `hasProcess=false` 카드 (cspsim, agent) 는 ² 실행 영역 비활성 ("원격 — 로컬 실행 없음"), ³ 다운로드만 활성.

### 3.1 헤더 액션

```
패키징 [git=f4d9 manifest=abcd…]   [ v X.Y.Z 입력 ] [▶ 빌드 & 패키징] [🗑 정리] [↻]
```

| 버튼 | 동작 |
|---|---|
| **▶ 빌드 & 패키징** | `POST /api/v1/build/release` — 한 job 으로 `cims.sh build [-v X.Y.Z] && cims.sh pkg --no-bump` 실행. 5~15분 소요. 진행 stdout 은 우측 터미널 패널에 실시간 폴링 (1.5s) |
| **🗑 정리** | `POST /api/v1/build/clean` — `build/dist/packages/*.tar.gz` + `manifest.json` 삭제. 빌드 결과 (`build/dist/<comp>/`) 는 유지 |
| **↻** | manifest + service status + packages 동시 새로고침 |
| **버전 input** | 빈 입력 = 현재 pkg.json 버전 유지. 비어 있지 않으면 `cims.sh build -v <X.Y.Z>` 로 모든 base pkg.json 갱신. 정규식: `[0-9A-Za-z._+\-]{1,64}` |
| **manifest 칩** | `git=<sha7>` + `manifest=<sha8>…` (mouseover 로 ts/ens_ip/full sha 표시) |

### 3.2 카드 액션

| 영역 | 동작 |
|---|---|
| **¹ 설정 [템플릿]** | 현재 가장 최신 등록된 패키지의 `config_template.json` JSON 편집 모달 (POST `/api/v1/packages/<id>` `config_template`) |
| **¹ 설정 [설정]** | 모듈 설정 (deployment 의 scalar overlay). `ModuleConfigModal` 이 열리고, 저장 시 `needsRestart` 표시 |
| **² 실행 [▶/■]** | `POST /api/v1/services/<name>/<start|stop>` — `critical=true` (csc) 는 추가 confirm |
| **² 실행 [↻]** | `restart` |
| **³ 다운로드 [⤓ <variant> v<x>]** | `GET /api/v1/build/packages/<m>` — 가장 최신 tarball 다운로드. 라벨에 모듈명 + 버전 (실수 방지) |
| **터미널 패널 (우측 2/5)** | activeJob 진행 중에는 job stdout 폴링, 그 외엔 마지막 module act 출력. PASS/FAIL verdict 색 라벨. ANSI escape 제거 |

### 3.3 backend handler — `csc/src/handlers/build.py`

```
POST /api/v1/build/run                  → cims.sh build [-v <ver>]
POST /api/v1/build/pkg                  → cims.sh pkg [-v <ver>] [--no-bump] <m>...
POST /api/v1/build/release              → cims.sh build [-v <ver>] && cims.sh pkg --no-bump
POST /api/v1/build/clean                → packages/*.tar.gz + manifest.json 삭제
GET  /api/v1/build/jobs/<job_id>        → 진행 상태 + stdout tail (100 lines)
GET  /api/v1/build/manifest             → packages/manifest.json + _self_sha256
GET  /api/v1/build/packages             → manifest.packages[] (없으면 디렉토리 스캔)
GET  /api/v1/build/packages/<module>    → tarball octet-stream
```

- `_VALID_MODULES = (cmp, pmp, imp, csp, psp, isp, cwrtc, csc, console, phone, cspsim, agent)` — 12종 화이트리스트.
- 동시 실행 가드: module-level `asyncio.Lock` — 진행 중 추가 요청은 409.
- 인자 검증: `version` 정규식 `[0-9A-Za-z._+\-]{1,64}` (shell injection 방지).
- 소스 루트 결정: `init(repo_root)` 가 cims.sh + CMakeLists.txt search-up → dist 안에서 띄워진 csc 라도 진짜 소스 트리에서 build 명령 실행.

## 4. 산출물 레이아웃

```
build/
├── dist/                              # 빌드 결과 (정리 시 보존)
│   ├── csp/   cmp/   cwrtc/  csc/    console/  phone/  cspsim/  agent/
│   ├── cims.sh                       # 공통 launcher
│   └── packages/                     # 패키지 산출물 (정리 시 삭제)
│       ├── csp-0.0.10.tar.gz
│       ├── psp-0.0.10.tar.gz         ← csp staging rename
│       ├── isp-0.0.10.tar.gz
│       ├── cmp-0.0.10.tar.gz
│       ├── pmp-0.0.10.tar.gz
│       ├── imp-0.0.10.tar.gz
│       ├── cwrtc-0.0.10.tar.gz
│       ├── csc-0.0.10.tar.gz
│       ├── console-0.0.10.tar.gz
│       ├── phone-0.0.10.tar.gz
│       ├── cspsim-0.0.10.tar.gz
│       ├── agent-0.0.10.tar.gz
│       └── manifest.json             ← cmd_pkg 끝에서 자동 생성
└── ...
```

각 tarball:

```
<m>-<ver>.tar.gz
├── meta.json                  ← name/version/desc/build_date/git/service
├── config_template.json       ← UI 렌더링 스키마 (csp/cmp/csc 만)
├── <m>/                       ← 모듈 자기 디렉토리 (변종은 rename 됨)
│   ├── bin/<m> [+ <m>.sh]
│   ├── config/<m>.json [+ collection jsonl]
│   └── ...
└── cims.sh                    ← 공통 launcher
```

## 5. manifest.json — immutability gate 의 SoT

`cmd_pkg` 끝에서 inline python heredoc 으로 자동 생성:

```json
{
  "ts": "2026-05-09T...",
  "git": { "branch": "...", "sha": "f4d90ef" },
  "host": "nex@nex-ubuntu",
  "ens_ip": "",
  "packages": [
    { "name": "csp-0.0.10.tar.gz", "size": 8123456,
      "sha256": "abcd...", "mtime": "2026-05-09T..." },
    ...
  ]
}
```

`GET /api/v1/build/manifest` 응답 시 `_self_sha256` (manifest 파일 자체 SHA-256) inject. S5 가 PASS 시 `build/dist/.deployed-manifest.json` 에 이 값을 기록 → S6-ENTRY-CHECK 가 매칭 검증 (배포 후 재패키지화 시 mismatch 로 즉시 차단).

검증 S4-PKG-MANIFEST 가 동일 로직을 따로 수행해서 manifest 가 항상 fresh 하도록 보장 (CLI 와 verify 양쪽 다 동등).

## 6. csc.json `Packages.Dir` — 배포 업로드 디렉토리

CSC 가 콘솔에서 업로드받은 패키지를 보관하는 디렉토리 (default `<csc-root>/packages/`). 빌드 산출물 `build/dist/packages/` 와 **별개**:

| 디렉토리 | 누가 쓰는가 | 어디에 |
|---|---|---|
| `build/dist/packages/` | `cims.sh pkg` 산출 / 콘솔 다운로드 (`/release/package`) | `csc/src/handlers/build.py` 의 `_DIST_PKG_DIR` |
| `csc.json:Packages.Dir` | 사용자 업로드 (배포 메뉴 `/deploy/packages`) | `csc/src/handlers/agents.py:_create_package` |

코드 레벨 완전 분리 — 같은 변수명을 공유하지 않는다. `Packages.Dir` 는 2026-05-08 (`f4d90ef`) 부터 csc 카드의 ¹ 설정 모달 "패키지 저장소" 그룹에서 사용자가 직접 편집 가능 (옛 `"hidden": true` 제거).

## 7. 흔한 함정

- **빈 버전 input 으로 ▶**: 현재 pkg.json 버전 그대로 패키징. 새 컴포넌트 추가했다면 `pkg.json` 의 `version` 이 다른 컴포넌트와 어긋난 채 산출될 수 있음 → `-v` 명시 권장.
- **0.0.10 vs 0.0.2 alphabetical sort**: `tarballByModule` (`ServicesPage.tsx`) 가 manifest packages 를 alphabetical 정렬 시 frontend 마지막 entry 가 잘못된 버전 가능. 큰 버전 패치 시 보강 필요 (백로그).
- **🗑 정리 후 ▶ 다시**: cmake 캐시는 살아있으므로 빌드 자체는 빠름. 다만 `csc/packages/` (배포 업로드본) 와는 무관 — 그쪽은 별도 메뉴에서 관리.
- **dist 안에서 띄운 csc 가 build 명령 거부**: `cmd_build` 가 `[[ -z "$SRC_CONSOLE" ]]` 체크 — 소스 트리에서만 동작. backend handler 의 `init()` 도 search-up 으로 진짜 소스 루트를 잡음.

## 8. 관련 문서

- `design/02_deployment.md` — Agent / Package / Deployment 데이터 모델
- `design/features/package_and_template.md` — 패키지 포맷 + config_template.json
- `VERIFICATION_PROCESS.md` — S4 (패키지화) + S6 immutability gate
- `api/admin_api.md` — 배포 메뉴 (`/deploy/packages`) 의 CSC API
- 콘솔 코드: `cims-console/src/pages/ServicesPage.tsx` + `cims-console/src/api/build.ts`
- 백엔드: `csc/src/handlers/build.py`
- CLI: `cims.sh:cmd_build` (line 696~) / `cims.sh:cmd_pkg` (line 1671~)
