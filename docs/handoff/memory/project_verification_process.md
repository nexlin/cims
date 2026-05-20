---
name: CIMS 검증 3단계 프로세스 (Phase 1/2/3)
description: docs/VERIFICATION_PROCESS.md 요약. 2026-04-24 저녁 재정리 — Phase 3 를 "배포본 기능 회귀 재수행" 으로 재정의. 기존 "Phase 2/3 = 배포 메커니즘만" 원칙은 Phase 2 한정으로 축소.
type: project
originSessionId: 11577459-2194-4246-a25b-bcf3d20b6c95
---
**원본 문서**: `docs/VERIFICATION_PROCESS.md` (SSOT) — 상세는 원본 참조.
**이 메모리의 역할**: 세션 시작 시 빠르게 재인식.

## ⚠ 핵심 원칙 (2026-04-24 재정의)

- **Phase 1** — 배포 **전** 검증. 기능 회귀 본진. `build/dist/<모듈>/` 직접 기동.
- **Phase 2** — 배포 **과정** 검증. tarball → TB-CSC → Test-agent 배포 메커니즘만. **기능 회귀 반복 X**.
- **Phase 3** — 배포 **이후** 검증. **배포본에서 기본 검증 4시나리오 + 보완 사항 재수행**. 환경 의존 버그 검출.
  - 기존 "배포 체인만, REGISTER 1건 smoke" 정의 폐기 (2026-04-24 docs 재정리).
- Phase 2 에서 기능 이슈 발견 → Phase 1 회귀. Phase 3 에서 기능 이슈 → Phase 1 재현 여부로 분기 (재현: 코드 이슈 / 미재현: 배포 경로·환경 의존 이슈).

## TB 3종 (상시 동작)

| 서비스 | 포트 | 역할 |
|---|---|---|
| **TB-CSC** | **4419** | 패키지/에이전트/배포/검증 실행 관리 |
| **TB-Console** | **3000** | TB-CSC UI (vite dev `--mode tb`). dist 서빙 미지원. |
| **TB-agent** | sync **9902** | TB-CSC 에 enroll. `/tmp/cims-tb-agent/` |

재기동 규칙: Phase 진행 중 내리지 않음. 번들/바이너리 해시 갱신 시에만 자동 재기동. DB 공유 (`cims`).

## Phase 1 Test-\* (직접 기동본, `build/dist/<모듈>/`)

| 서비스 | 포트 | 비고 |
|---|---|---|
| Test-CSC | 4421 | (4420→4421 전환 4f53b7d) |
| Dev-Console | 3001 | 소스 vite dev. Test-CSC 4421 proxy. (e0c44a7) |
| Test-Console | 8080 | dist HTTPS serve. (e0c44a7, cwrtc 8443 이전 후 단독 — d90a08c) |
| Test-CSP | 5060·5061·25061 | SIP |
| Test-CMP | 9000 + RTP 풀 | 미디어 relay |
| Test-CWRTC | **8443 WSS** | (8080→8443 이전 d90a08c) |
| Test-Phone | 3002 | MCPTT UE Web |
| Test-CSPSIM | 9000 | 시험 중에만 |

## Phase 2/3 배포본 (`build/dist/<x>-server/<모듈>/`)

| 서비스 | 포트 | 비고 |
|---|---|---|
| 배포본 csc (`csc-server/csc/`) | 4420 | Phase 2 배포, Phase 3 주체 |
| 배포본 console (`csc-server/console/`) | 80 | cap_net_bind or reverse proxy — 후속 설계 |
| 배포본 csp/cmp/sim (`<x>-server/<모듈>/`) | 운영 | Phase 3 배포 체인 |
| Test-agent sync | **9903** | per-host agent (`csc-server-local` 등) |
| `verify phase2` csc start overlay | **4445** | Phase 1/배포본과 공존 검증용 |

명명: 배포본은 접두 없음 (`csc` / `console` / `csp` / `cmp` / `sim`). 직접 기동본은 `Test-*` 접두.
`Test-agent` ≠ `TB-agent`: Test-agent 는 per-host, TB-agent 는 sync 9902 로 상시 동작.

## Phase 별 요약

### Phase 1 — 배포 전 검증 (8단계)
1. 빌드 (`cims.sh build`) — 전 모듈 (agent/csc/csp/cmp/cwrtc/console/phone/simulator) warning/error 0
2. 복사/sync (`cims.sh sync <target>`) — Python/스크립트 변경 빠른 반영
3. 설정 (`cims.sh configure --local-ip <ens160>`) — 모든 모듈 config 재생성
4. 초기화 (`cims.sh reset`) — 가입자 보존, 그 외 DB/로그/서비스로그/녹취/배포본 디렉토리 wipe
5. 실행 (`cims.sh start`) — CMP → CSP → CWRTC → CSC → Console → Phone 순
6. 검증 — 기본 4시나리오 + Phase 1 추가 3시나리오 + 개발/보완 사항
7. 리포팅 — `verify_reports/<ts>_phase1.md`
8. **모듈 유지** — 사용자 추가 시험 가능하도록 기동 상태 그대로
- 자동: `cims.sh verify phase1`

### Phase 2 — 배포 과정 + 환경 구축 (22단계, 재설계 70ad7ec)
**목적**: tarball → 배포 체인 → **전 모듈(csc/console/csp/cmp/cspsim) install + start**. Phase 3 진입 조건 충족 상태로 종료.

- **csc-server (TB-CSC 4419 경유)**: csc-server-local agent (sync 9903) → csc/console tarball 업로드 → install → csc Start(**4445** overlay) + Health + console Start(**8081** overlay)
- **csp/cmp/sim-server (배포본 csc 4445 경유)**: 3개 Test-agent (sync 9904/9905/9906, `--csc-url 4445`) → csp/cmp/cspsim tarball 업로드 (배포본 csc) → install → csp(**5060/udp**) + cmp(**9000/udp**) Start (sim install-only)
- **데이터 wipe 는 Phase 2 시작 시만** — `cmd_reset --keep-processes`
- **종료 시 기본**: 전 배포본 기동 유지. `--stop-after` 지정 시 전체 정리.
- 자동: `cims.sh verify phase2 [--skip-build] [--skip-pkg] [--stop-after]`

### Phase 3 — 배포 이후 검증 (6단계)

**진입 조건**: Phase 1 **서버 모듈 중지** (csp/cmp/cwrtc/phone/cspsim), Console (3001 or 8080) 은 유지 (UI 세션용). TB 3종 상시 유지. Phase 2 완료.

1. csc → csp/cmp/sim 배포 체인 완성 (`csp-server` / `cmp-server` / `sim-server` agent enroll + 모듈 설치)
2. 배포본 모듈 설정 (Console 모듈관리 → scalar overlay + collection)
3. 로그/DB wipe (`cims.sh reset`) — 로그/DB 공유 구조상 같이 정리
4. 배포본 실행 — csc → csp → cmp → sim → console:80
5. 검증 — **기본 4시나리오 재수행** (배포본에서) + 개발/보완 사항 재확인
6. 리포팅 — `verify_reports/<ts>_phase3.md`
### Phase 3 — 서비스 검증 전용 (재설계 70ad7ec, 2026-04-25)
**목적**: Phase 2 에서 배포·기동된 환경에서 **4시나리오만** 검증. 배포/agent enroll 없음. 데이터 wipe 없음.

- 진입 조건 체크: 4포트 LISTEN (csc 4445 + console 8081 + csp 5060 + cmp 9000) — 미충족 시 즉시 FAIL + "Phase 2 선행" 안내
- 시나리오 준비: 가입자 정보 + 배포본 csp jsonlDir (`csp-server/csp/config/`) 에 access_services.jsonl 시드 + SIGUSR1 reload
- **4시나리오 실행** (cspsim → 배포본 csp 5060):
  · 3.1 VoLTE 음성 2자 (-no_video)
  · 3.2 VoLTE 영상 2자 (video 포함)
  · 3.3 PTT 그룹 음성 5인 (-no_video)
  · 3.4 PTT 그룹 영상 5인
- 판정: seg_*.rtp 녹취 +1 이상 → PASS
- 종료 시 전체 유지 (wipe/stop 없음)
- 자동: `cims.sh verify phase3` (옵션 없음)
- **UDP ss 파싱 함정** (acb47bb): `ss -uln` Local Address 는 **$4** ($5 는 Peer). TCP 는 $4 로 정확.
- **tarball stale 함정**: `--skip-pkg` 시 tarball 속 config (LocalIp 등) 가 과거 configure 값 → start 시 UDP bind 실패. ens160 IP 변경 후엔 configure + pkg 재실행 필수.
- **pipefail + grep -c 함정** (806da88): `grep -c` 가 0 매칭 시 exit 1. `set -euo pipefail` 환경에서 `cat ... | grep -c | ...` 형태는 abort 유발. array-based guard 또는 `|| true` 로 catch 필수.

## 기본 검증 4시나리오 (Phase 1/3 공통)

가입자 정보 기반. Console 에서 실행.

1. **VoLTE 음성 2자 통화** (B2BUA) — 녹취 `seg_*.rtp`, 양 leg `sesid` 일치, `session.json` `call_ids` 쌍
2. **VoLTE 영상 2자 통화** (B2BUA) — (1) + 영상 RTP m-line, 양방향 비디오
3. **PTT 그룹 음성 통화** (5인) — multipart INVITE (SDP + OMA POC XML), Conference NOTIFY, floor port, `m=application` 분리, 플로어 요청/그랜트
4. **PTT 그룹 영상 통화** (5인) — (3) + 영상 stream + 그룹 영상 분배

## Phase 1 전용 추가 시나리오 (Phase 3 에서는 생략)

- **CSC 가입자/그룹 변경 → NOTIFY** — admin API CRUD → `notify_csp` → 캐시 갱신 + GMS/CMS NOTIFY
- **SUBSCRIBE/NOTIFY** (IdMS/GMS/CMS) — TB-Console Flow 페이지 nodes 순서
- **(mTLS 모드) Cert rotation** — `cert_rotate_pending=1` → heartbeat → agent rotate → exit → 재기동

## 초기화 정책

- 보존: 가입자 DB · TB 3종 · TB-agent 레코드
- 초기화: 런타임/배포/세션/이력 DB · `build/dist/log/*.log` · `ext_mnt/{service_log,msg_log}/` · `build/dist/*-server/` · 발급 cert · 검증 대상 프로세스
- `cims.sh reset` — 전체 초기화 (TB 제외)
- `cims.sh reset --keep-processes` (e0c44a7) — Phase 1 모듈 프로세스 유지, 데이터만 wipe. `verify phase2` 내부 사용.
- `cims.sh reset --keep-deployed` (9f4d80f) — `build/dist/{csc,csp,cmp,sim}-server/` 보존. Phase 3 가 Phase 2 결과물(csc-server) 살리기 위해 `--keep-processes --keep-deployed` 로 호출.

## Console Phase 1/2/3 UI (65f19c2)

VerificationPage (`/testbed/verify`) 상단에 통합 검증 UI.

### 백엔드 (`csc/src/handlers/verification.py`)
- `POST /api/v1/verification/phases/<N>` (N=1/2/3) — subprocess `cims.sh verify phase<N>`
  · body: `{skip_build, skip_pkg, keep_agent}`
  · 반환: `{phase, verdict, returncode, report_path, report_ts, stdout_tail, argv}`
- `GET /api/v1/verification/phases/<N>/latest-report`
- `GET /api/v1/verification/phases/<N>/reports` (최대 50, 최근순)
- Timeout: phase1=900s, phase2=360s, phase3=600s
- **중요 fix**: async handler 에서 `subprocess.run` 은 uvicorn 이벤트 루프 block → verify script 의 self-call (TB-CSC 4419 curl) 실패. `asyncio.to_thread` worker 필수.
- **repo root 탐색 개선**: tests_dir fallback 이 엉뚱한 `build/dist/tests` (존재 안 함) 로 지정될 수 있음 → tests_dir 상위 6단계 올라가며 `cims.sh + CMakeLists.txt` 공존 찾기. env `CIMS_REPO_ROOT` override.

### 프론트엔드 (`cims-console/src/pages/VerificationPage.tsx`)
- Phase 1/2/3 탭 + 옵션 체크박스 + 실행 버튼
- 결과: 판정 (PASS/FAIL 컬러), returncode, stdout_tail, 자동 리포트 로드
- 기존 run_all.py 세밀 검증은 하단 "Phase 1 상세 검증" 으로 분리 유지

## 알려진 함정

- `cims.sh pkg` patch +1 자동 → 고정 시 `--no-bump`
- `make dist` 후 `configure.sh` 재실행 필수
- localhost 외부 접근 불가 — `ens160` IP 필수
- cspsim REGISTER: `-auth_id "IMSI@domain"` 필수
- TB-CSC mTLS 모드: `Agent.MtlsEnabled: true` overlay 필수
- 같은 호스트 다중 agent: `CIMS_AGENT_SYNC_PORT` env 주입
- Agent cert rotate 는 `exit(0)` 만 — 재기동은 systemd
- TB-Console 은 vite dev 모드 전용 (dist serving 미지원)
- **verify phase2/3 `--skip-pkg` 함정**: `sync all` + `pkg --no-bump` 없으면 tarball 속 스크립트 stale
- **cwrtc.json LocalIp stale 함정**: ens160 IP 변경 시 configure 재실행 없으면 SIP UA bind 실패
- **async handler + subprocess.run 함정** (65f19c2): uvicorn 이벤트 루프 block → self-call 불가. `asyncio.to_thread` 필수.
