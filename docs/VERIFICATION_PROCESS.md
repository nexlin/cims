# CIMS 검증 절차

> **목적**: CIMS 패키지를 개발/보완한 뒤 배포 전 / 배포 과정 / 배포 이후 3단계로 검증한다.
> **적용 범위**: agent · csc · csp · cmp · cwrtc · console · phone · simulator 중 한 모듈이라도 변경되면 본 절차를 따른다.
> **원본 SSOT**: 이 문서. 진행 중 보완은 검증 리포트에 기록 후 본 문서에 반영.

---

## 0. 공통

### 0.1 3단계 구조 (한눈에)

| Phase | 단계 | 목적 | 장소 | 결과 |
|---|---|---|---|---|
| **1** | 배포 **전** | 개발/보완 + 회귀 본진 | `build/dist/<모듈>/` (직접 기동) | 기능 PASS |
| **2** | 배포 **과정** + **환경 구축** | tarball → agent 배포 체인 + **전 모듈(csc/console/csp/cmp/cspsim) install + 기동** | `build/dist/{csc,csp,cmp,sim}-server/` | 배포본 전체 기동 상태 유지 |
| **3** | 배포 **이후** | 배포본에서 **서비스 기능 검증** | Phase 2 기동 환경 그대로 | 기본 4시나리오 PASS |

**원칙 (2026-04-25 재설계)**:
- **Phase 2 가 환경 구축까지 책임** — csc/console/csp/cmp/cspsim 을 모두 install + start. Phase 3 진입 조건 충족 상태로 종료.
- **Phase 3 는 검증 전용** — 배포나 agent enroll 안 함. 4시나리오 + 개발/보완 확인만.
- **데이터 wipe 는 Phase 2 시작 시만** — Phase 3 는 wipe 없음 (Phase 2 에서 wipe 한 데이터로 시나리오 실행).
- Phase 3 의 목적은 "실제 배포 환경에서 기능 검증". 배포 실수·환경 의존 버그 검출.

### 0.2 구성 — TB 3종 / Test-* / 배포본

| 구분 | 서비스 | 포트 | 역할 |
|---|---|---|---|
| **TB** (상시) | TB-CSC | 4419 | 패키지·에이전트·배포 관리 (Phase 2/3 제어용) |
| | TB-Console | 3000 | TB-CSC UI (`cims-console/` vite dev, `--mode tb`) |
| | TB-agent | sync 9902 | TB-CSC 에 enroll, 검증 대상 모듈 설치 |
| **Phase 1 Test-\*** (`build/dist/<모듈>/`) | Test-CSC | 4421 | CSC 직접 기동본 |
| | Dev-Console | 3001 | `cims-console/` 소스 vite dev (Test-CSC 4421 proxy) |
| | Test-Console | 8080 | `build/dist/console/dist` 정적 HTTPS 서빙 |
| | Test-CSP | 5060·5061·25061 | SIP 서버 |
| | Test-CMP | 9000 + RTP 풀 | 미디어 relay |
| | Test-CWRTC | 8443 (WSS) | WebRTC 게이트웨이 |
| | Test-Phone | 3002 | MCPTT UE Web |
| | Test-CSPSIM | 9000 | cspsim (시험 중에만) |
| **Phase 2/3 배포본 (verify 환경)** | csc | **4445** | `verify phase2` overlay 기동 — Phase 1 Test-CSC 4421 / 운영 4420 과 분리 |
| | console | **8081** | `verify phase2` overlay 기동 — Test-Console 8080 / 운영 80 과 분리 |
| | csp / cmp / sim | 5060·5061 / 9000 / sync | Phase 3 시나리오 대상 |
| | Test-agent | sync 9903~9906 | per-host agent (csc/csp/cmp/sim-server-local) |
| **(참고) 운영 배포본** | csc | 4420 | 실제 운영 환경 |
| | console | 80 | 운영 UI (cap_net_bind 또는 reverse proxy) |

> **명명 규칙**
> - **TB-\*** — 상시 테스트베드 (환경 제어용). Phase 진행 중 내리지 않는다.
> - **Test-\*** — Phase 1 의 `build/dist/<모듈>/` 직접 기동 인스턴스 별칭.
> - **배포본** (무접두) — Phase 2/3 에서 agent 경유로 설치된 운영 인스턴스. `<x>-server/<모듈>/` 경로.
> - **Test-agent** ≠ **TB-agent** — Test-agent 는 per-host (csc-server-local 등), TB-agent 는 sync 9902 로 상시 동작하는 환경 제어용.
> - **Console 4분화**: 같은 `cims-console/` 코드베이스가 모드/포트별로 분기.
>   - **TB-Console** (3000, `--mode tb`) — TB-CSC(4419) backed, **검증 실행 표준 진입점**
>   - **Dev-Console** (3001, vite dev) — Test-CSC(4421) backed, Phase 1 가입자 화면
>   - **Test-Console** (8080, dist HTTPS) — Test-CSC(4421) backed, Phase 1 dist 검증
>   - **배포본 console** — verify 환경(8081, Phase 2 overlay) / 운영(80) 분기
>   - `cims.sh start console` 이 SRC_CONSOLE 존재 여부로 Dev/Test 자동 분기.

### 0.3 포트 공존 설계

포트 번호 전체는 §0.2 표 참조. 핵심 원칙:

- **Phase 1 Test-\*** (dev/debug 포트) 와 **Phase 2·3 배포본** (운영 포트) 은 번호가 달라 동일 호스트에서 공존 가능 — Test-CSC 4421 vs 배포본 csc 4420(운영) / 4445(verify), Dev-Console 3001 vs Test-Console 8080 vs 배포본 console 80(운영) / 8081(verify).
- **TCP 4421 ↔ UDP 4421** 은 완전히 다른 서비스 (Test-CSC admin TCP / CSP CscInterface UDP, `csp/CspServer.cpp:259`). proto 다름이라 무충돌.
- **`verify phase2` 자동화** 는 overlay 로 csc(4445) + console(8081) 기동 → Phase 1 (4421/3001/8080) / 운영 (4420/80) 과 모두 분리.
- **Phase 3 에서 포트 충돌 발생 지점**: csp (5060 · 5061), cmp (9000), cwrtc (8443), phone (3002), cspsim (9000) — Phase 1 과 배포본이 같은 운영 포트. 그러므로 Phase 3 진입 시 Phase 1 서버 모듈 중지 (§3.1).

### 0.4 디렉토리 레이아웃

```
build/dist/
├── csc/ csp/ cmp/ cwrtc/ console/ phone/ cspsim/   # Phase 1 직접 기동 (Test-*)
├── csc-server/{agent, csc, console, config/}       # Phase 2 배포 대상
├── csp-server/{agent, csp, config/}                # Phase 3 배포 대상
├── cmp-server/{agent, cmp, config/}                # Phase 3 배포 대상
└── sim-server/{agent, sim, config/}                # Phase 3 배포 대상
```

각 `<x>-server/` 내부: `agent/` (cims_agent + state + 발급 cert) · `<모듈>/` (pkg.json + config.json overlay + modules/**) · `config/` (collection jsonl).

### 0.5 초기화 범위

`cims.sh reset` 이 수행. 가입자 정보는 보존, 그 외 운영 데이터는 전부 정리. TB 3종은 건드리지 않는다.

| 구분 | 대상 | 동작 |
|---|---|---|
| **보존 (가입자)** | `users`, `organizations`, `*_subscriptions`, `ptt_groups`, `ptt_group_members`, `user_rejects` | 그대로 |
| **보존 (TB)** | TB-CSC / TB-Console / TB-agent 프로세스 · 인증서 · 로그 | 그대로 |
| **보존 (TB-agent 레코드)** | `cims_agent` 의 `name='tb-agent-local'` | DELETE 제외 |
| **초기화 (DB)** | 런타임 설정 · 배포 등록 · 세션/이력/녹취/통계 테이블, `cims_agent` (TB 외) | TRUNCATE / 조건부 DELETE |
| **초기화 (파일)** | `build/dist/log/*.log`, `ext_mnt/service_log/`, `ext_mnt/msg_log/`, `build/dist/*-server/`, `cert/agent_mtls/issued/` | rm -rf |
| **초기화 (프로세스)** | 검증 대상 모듈 + Test-agent | kill |

**옵션**:
- `cims.sh reset` — 전체 초기화 (TB 제외)
- `cims.sh reset --keep-processes` — Phase 1 모듈 프로세스 유지, 로그/DB/배포본 디렉토리만 wipe (`verify phase2` 자동 흐름 내부에서 사용)

### 0.6 외부 IP

테스트 서버 DHCP. 외부 연동용 IP 는 `ens160` 인터페이스 IP. `cims.sh preflight` 자동 감지. `configure.sh --local-ip <ens160_ip>` 로 반영. localhost 는 외부 접근 불가.

### 0.7 합격 기준 (공통)

- 빌드: warning/error 0
- 런타임 로그: ERROR/FATAL 0
- Phase 별 시나리오 전부 PASS
- Flow/Msg 로그 무결성 (sesid 일관, body seq 매칭)

### 0.8 리포트 양식

경로: `verify_reports/<YYYYMMDD_HHMMSS>_<phase>.md` (TB-Console `/testbed/verify` 자동 저장/조회)

내용: 환경 (브랜치/commit/ens160/migration/해시) · 단계별 PASS/FAIL · 시나리오 결과 · 이슈 (severity + 재현)

### 0.9 기본 검증 4시나리오 (Phase 1/3 공통)

자동 실행은 `cims.sh verify phaseN` (CLI) 또는 TB-Console(3000) `/testbed/verify` 에서 trigger. 가입자 직접 조작 시나리오는 Phase 1 → Dev/Test-Console (3001/8080, Test-CSC 4421 backed), Phase 3 → 배포본 console (8081, 배포본 csc 4445 backed).

| # | 시나리오 | 확인 포인트 |
|---|---|---|
| (1) | **VoLTE 음성 2자 통화** (B2BUA) | REGISTER × 2 성공 · INVITE 통화 연결 · 녹취 `seg_*.rtp` · 양 leg 동일 `sesid` · `session.json` 의 `call_ids` 쌍 |
| (2) | **VoLTE 영상 2자 통화** (B2BUA) | (1) + 영상 RTP m-line 협상 · 양방향 비디오 패킷 흐름 |
| (3) | **PTT 그룹 음성 통화** (5인) | multipart INVITE (SDP + OMA POC XML) · Conference NOTIFY · floor port 협상 · `m=application` 분리 · 플로어 요청/그랜트 동작 |
| (4) | **PTT 그룹 영상 통화** (5인) | (3) + 영상 stream + 그룹 멤버 간 영상 분배 |

### 0.10 Phase 1 전용 추가 시나리오

Phase 1 에서만 수행 (Phase 3 는 §0.9 4시나리오만):

- **CSC 가입자/그룹 변경 → NOTIFY** — admin API CRUD → `notify_csp` → `CspUserMap`/`CGroupMap` 캐시 갱신 → GMS/CMS NOTIFY 발송
- **SUBSCRIBE/NOTIFY 전체 흐름** (IdMS / GMS / CMS) — Console 통화이력(CallLogs/VolteHistory/PttHistory)에서 호 선택 → Flow 모달의 nodes 순서 정상
- **(mTLS 모드) Cert rotation e2e** — `cert_rotate_pending=1` → heartbeat `cert_rotate:true` → agent rotate → `exit(0)` → 재기동 후 새 cert 적용

---

## 1. Phase 1 — 배포 전 검증

개발·보완 완료 후, **`build/dist/` 안에서 직접 기동** 하여 기능 회귀와 보완 사항을 확인. Phase 1 PASS 가 Phase 2 진입 조건.

### 1.1 사전 확인

리포트 서두에 기록:

- 변경 범위 / 영향 모듈 (agent / csc / csp / cmp / cwrtc / console / phone / simulator)
- 추가/변경 DB migration 스크립트
- 변경된 config template (`csp.json.template`, `config_template.json`, `cmp.json`, `cwrtc.json.template` 등)
- 회귀 리스크 플래그: Flow/Msg 포맷, sesid 규약, B2BUA 라우팅, CMP 포트 풀, mTLS cert 발급 경로 등 "건드리면 광범위하게 깨지는" 영역 해당 여부
- `git status` clean, 브랜치/커밋 해시 기록
- `ens160` IP 확인 (`cims.sh preflight`)
- TB 3종 동작 확인

### 1.2 단계

**(1) 빌드** — `cims.sh build` → C++ + Web UI 빌드 결과가 `build/dist/` 에 적재. warning/error 0 확인. 번들 해시 갱신 시 TB 도 자동 재기동.

**(2) 복사/sync** — Python/스크립트만 바꾼 경우 `cims.sh sync <target>` (csc · agent · scripts · pkg-meta · console · phone) 으로 빠른 동기화. C++ 변경은 (1) 에서 자동 배치됨.

**(3) 설정** — `cims.sh configure --local-ip <ens160_ip>` → `csp.json` · `cmp.json` · `csc.json` (Test-CSC 4421) · `csc-tb.json` (TB-CSC 4419) · `cwrtc.json` · Vite `.env.local` 전부 재생성.

**(4) 초기화** — `cims.sh reset` → §0.5 범위 자동 처리 (가입자 보존, 그 외 DB/로그/녹취/service_log/msg_log/발급 cert/배포본 디렉토리 전부 wipe, TB 유지).

**(5) 실행** — `cims.sh start` → 순서 CMP → CSP → CWRTC → CSC → Console → Phone. 개별 재기동은 `cims.sh start <name>` / `restart <name>`.

**(6) 검증**:
- **TB-Console** (`http://<ens160>:3000/testbed/verify`) → [Phase 1] 탭 → ▶ 실행 — §0.9 4시나리오 + §0.10 Phase 1 추가 3시나리오 순차 수행. 검증 자동화 백엔드(`/api/v1/verification/phases/N`)는 TB-CSC(4419) 측이라 TB-Console 에서만 동작.
- Dev-Console (3001) / Test-Console (8080) 은 가입자 화면·시나리오 직접 조작·녹취 확인 용도 (Test-CSC 4421 backed)
- 개발/보완 사항 직접 조작 (§1.1 에서 식별된 기능)
- TB-Console `/testbed/modules` — 버전/설정 템플릿/overlay 반영 확인

**(7) 리포팅** — `verify_reports/<ts>_phase1.md` 자동 생성. §0.8 양식.

**(8) 모듈 유지** — (5) 에서 기동한 모듈을 그대로 둠. 사용자가 추가 시험/Console 확인을 수행할 수 있도록.

### 1.3 자동 명령

```bash
cims.sh verify phase1   # (1)~(7) 자동. (8) 유지는 기본 동작.
```

내부 흐름: preflight → reset → build → configure → start → §0.9/§0.10 시나리오 → 리포트.

### 1.4 이슈 처리

- Blocker / Major → 코드 보완 후 Phase 1 의 (1) 부터 재수행
- Minor → 리포트 기록 후 진행 판단

**Phase 1 전 항목 PASS 이후에만 Phase 2 로 진입.**

---

## 2. Phase 2 — 배포 과정 + 환경 구축

Phase 1 에서 확인된 구성을 tarball 로 묶어 배포 체인을 따라 설치·기동까지 수행한다. **csc/console/csp/cmp/cspsim 전 모듈** 을 `build/dist/{csc,csp,cmp,sim}-server/` 에 배포하고 sim 을 제외한 모든 모듈을 기동 상태로 종료 — Phase 3 진입 조건을 충족한다.

### 2.1 단계 (22단계)

**데이터 wipe** (시작 시, 3단계 중 유일한 wipe 시점):
**(1) Cleanup** — `cmd_reset --keep-processes` (Phase 1 Test-\* 유지, 로그/DB/ext_mnt wipe, 가입자 보존).
**(2) Build / (3) Configure / (4) Pkg** — 옵션 (`--skip-build`, `--skip-pkg` 로 생략 가능).

**csc-server 배포 (TB-CSC 4419 경유)**:
**(5) admin login** (TB-CSC) →
**(6) csc-server-local agent 등록** (`cims_agent.py`, sync **9903**) →
**(7) Test-agent 기동 + enroll 대기** →
**(8) csc + console tarball 업로드** →
**(9) deployment 생성** (csc overlay `{"Server.Port":4445}`, console overlay `{"Port":8081}`) →
**(10) install job 폴링** →
**(11) 설치 파일 검증** →
**(12) config overlay 반영 확인** →
**(13) csc Start (4445 LISTEN)** →
**(14) csc Health** (`tcp:4445=open`).

**console 기동**:
**(15) console Start (8081 HTTPS LISTEN)** — 배포본 console 은 config overlay `Port=8081` 로 `serve dist --ssl-cert` 기동 (Test-Console 8080 과 분리).

**csp/cmp/cspsim 배포 (배포본 csc 4445 경유 — csc 가 Phase 3 배포 주체)**:
**(16) 배포본 csc admin login** (cims DB 공유 → admin/1234 동일) →
**(17) csp + cmp + cspsim tarball 업로드** (배포본 csc) →
**(18) 3개 agent 등록 + Test-agent 기동** (sync **9904/9905/9906**, `--csc-url https://127.0.0.1:4445`) →
**(19) deployment 생성** →
**(20) install job 폴링** →
**(21) csp (5060/udp) + cmp (9000/udp) Start** (sim 은 install-only — cspsim 단발 실행이라 `_start_one` case 없음).

**종료**:
**(22) 기본**: 전 배포본 기동 유지 (Phase 3 진입 조건 충족). `--stop-after` 지정 시 전체 정리.

### 2.2 자동 명령

```bash
cims.sh verify phase2 [--skip-build] [--skip-pkg] [--stop-after]
```

기본: 완료 후 csc(4445) + console(8081) + csp(5060) + cmp(9000) + Test-agent 4개 모두 유지.
`--stop-after`: 검증 후 전부 정리 (배포 메커니즘 cleanup 검증 전용).

### 2.3 합격 조건

- Agent enroll OK × 4 (csc-server-local + csp/cmp/sim-server-local)
- Tarball 업로드 OK × 5 (csc / console / csp / cmp / cspsim)
- Install 완료 × 5
- Config overlay 반영 (`Server.Port=4445` on csc, `Port=8081` on console)
- Start + Health OK × 4 (csc / console / csp / cmp)
- sim: install-only
- 종료 후 모든 배포본 기동 유지

### 2.4 이슈 처리

- 배포 체인 이슈 → Phase 2 내 보완 후 재수행 (Phase 1 재수행 불필요)
- 기능 이슈 발견 → Phase 1 회귀 (Phase 1 검증 미흡 신호)

### 2.5 함정

- **`--skip-pkg` + tarball stale**: 소스 수정 후 `sync all` + `pkg --no-bump` 없이 `--skip-pkg` 사용 시 tarball 속 cims.sh / config 가 과거값. 특히 **start_console overlay 로직** 은 tarball 속 cims.sh 에 반영되어야 동작. ens160 IP 변경 시에도 동일 (csp.json LocalIp stale).
- **TB-CSC 재기동**: verification.py 등 csc 소스 수정 후에는 `cims.sh sync csc` + `restart tb-csc` 필수.

---

## 3. Phase 3 — 서비스 검증 전용

Phase 2 에서 배포·기동된 환경 (csc/console/csp/cmp) 에서 **기본 4시나리오 + 개발/보완 사항** 을 검증. 배포나 agent enroll 은 수행하지 않으며 데이터 wipe 도 없음.

### 3.1 진입 조건

Phase 2 완료 — 다음 4개 포트가 모두 LISTEN 상태여야 함:
- `4445/tcp` — 배포본 csc (API 제공)
- `8081/tcp` — 배포본 console (UI)
- `5060/udp` — 배포본 csp (SIP)
- `9000/udp` — 배포본 cmp (미디어 제어)

미충족 시 즉시 FAIL + "Phase 2 선행 실행 필요" 안내.

### 3.2 단계

**(1) 진입 조건 체크** — 위 4포트 LISTEN 확인.

**(2) 시나리오 준비**:
- DB (`volte_subscriptions` / `ptt_subscriptions` / `ptt_groups`) 에서 가입자·그룹 선택
- **배포본 csp jsonlDir** (`build/dist/csp-server/csp/config/`) 에 `access_services.jsonl` 시드 (volte / ptt kind 분리)
- 배포본 csp 에 `SIGUSR1` — ConfigCache reload

**(3) 4시나리오 실행** (`cspsim` → 배포본 csp 5060):
- **3.1 VoLTE 음성 2자** (`-mode volte -count 2 -no_video`)
- **3.2 VoLTE 영상 2자** (`-mode volte -count 2`, video 포함)
- **3.3 PTT 그룹 음성 5인** (`-mode ptt -scenario group_call -count 5 -no_video`)
- **3.4 PTT 그룹 영상 5인** (`-mode ptt -scenario group_call -count 5`)
- 판정: 각 시나리오 실행 후 `seg_*.rtp` 녹취 파일 +1 이상 → PASS

**(4) 결과 요약** — 녹취(size>0/0바이트), SIP msg/flow 로그, 배포본 csp/cmp 로그 ERROR/FATAL 카운트.

**(5) 리포팅** — `verify_reports/<ts>_phase3.md`.

종료 시 전체 환경 유지 (데이터 wipe 없음, 모듈 stop 없음).

### 3.3 자동 명령

```bash
cims.sh verify phase3
```

옵션 없음 (Phase 2 결과물에 의존).

### 3.4 합격 조건

- 진입 조건 4포트 전부 LISTEN
- 4시나리오 4/4 PASS
- 배포본 csp/cmp ERROR/FATAL 0 (권장)

### 3.5 이슈 처리

- 진입 조건 미충족 → Phase 2 선행 실행
- 시나리오 실패 — 원인 판별:
  - Phase 1 에서 같은 증상 재현 → 코드 이슈, Phase 1 부터 재수행
  - Phase 1 재현 안 됨 → 배포 경로·설정·환경 의존 버그, Phase 2 배포 설계 재검토
- 개발/보완 사항 실패 → 배포본 console (`https://<ens160>:8081/`) 또는 TB-Console (3000) 에서 직접 재현 확인 + 이슈 리포트

### 3.6 Console 진입점

| Console | URL | 백엔드 | 용도 |
|---|---|---|---|
| **TB-Console** | `http://<ens160>:3000/testbed/verify` | TB-CSC 4419 | **검증 실행 + 리포트 조회 (표준)** |
| 배포본 console | `https://<ens160>:8081/testbed/modules` | 배포본 csc 4445 | 배포된 csc/csp/cmp/sim 설정 확인·편집 |
| 배포본 console | `https://<ens160>:8081/testbed/verify` | 배포본 csc 4445 | 같은 화면이지만 backend 가 배포본 csc — verification.py subprocess 가 호스트 위 `cims.sh` 를 호출하므로 동일 호스트 환경에서만 동작 |

검증 실행은 **TB-Console (3000)** 에서 수행하는 것이 표준. 배포본 console (8081) 은 운영 환경 모듈관리 진입점.

---

## 부록 A. 알려진 함정

- `cims.sh pkg` 는 patch +1 자동. 버전 고정은 `--no-bump` 필수.
- `make dist` 이후 반드시 `configure.sh` 재실행 (IP 반영).
- localhost 로는 외부 접근 불가 — 반드시 `ens160` IP.
- cspsim REGISTER: `-auth_id "IMSI@domain"` 필수 (`verify phase1` 은 DB 자동 조회).
- TB-CSC mTLS 모드: `Agent.MtlsEnabled: true` overlay 필수.
- 같은 호스트 다중 agent: `CIMS_AGENT_SYNC_PORT` env 주입 (CLI 미지원).
- Agent cert rotate 는 `exit(0)` 만 수행. 재기동은 systemd/supervisor 책임.
- TB-Console 은 vite dev 모드 전용. dist 정적 서빙은 `/api` proxy 없음 → 별도 nginx conf 필요.
- **Test-Console (8080) 로그인 불가**: `npx serve dist` 정적 서빙이라 `/api` 요청에 404. dist SPA 자체 검증 외에 로그인/조작이 필요하면 **TB-Console (3000)** 또는 **Dev-Console (3001)** 사용. Phase 2 배포본 console (8081) 도 동일 한계 — 모듈관리·검증 UI 진입은 TB-Console 권장.
- **TB-Console 모듈관리에서 csc/console 시작 시 환경 격리** (`service_control.py`): TB-CSC 자체가 `CIMS_CSC_CONFIG=csc-tb.json` 으로 떠있어서, 단순 subprocess 호출로 `cims.sh start csc` 하면 자식 csc_app.py 가 csc-tb.json 을 상속받아 4419/4431 bind 충돌. `_invoke_cims_sh` 가 `CIMS_CSC_CONFIG` / `CIMS_AGENT_SYNC_PORT` 등 TB 전용 env 를 차단(`_sanitized_env`)한 뒤 subprocess 실행. **소스 수정 후 `cims.sh sync csc && restart tb-csc` 필수**.
- **verify phase2 `--skip-pkg` 함정**: 소스 수정 후 `sync all` + `pkg --no-bump` 하지 않으면 tarball 속 `cims.sh` / `cims_agent.py` 가 stale → start/health 가 옛 로직 사용.
- **cwrtc.json LocalIp stale 함정**: `ens160` IP 변경 시 configure 재실행 없으면 cwrtc SIP UA UDP bind 실패 (`UdpListen(5062) error`).

## 부록 B. 주요 명령어

```bash
# 환경 확인
ip -4 addr show ens160
git rev-parse --short HEAD
cims.sh status
cims.sh preflight

# TB 3종 (상시)
cims.sh start tb

# Phase 1 — 배포 전 검증
cims.sh verify phase1

# Phase 2 — 배포 과정 + 환경 구축 (csc/console/csp/cmp/cspsim install + start)
cims.sh build
cims.sh configure --local-ip <ens160_ip>
cims.sh pkg --no-bump                           # tarball 속 cims.sh/config 최신화 필수
cims.sh verify phase2 [--skip-build] [--skip-pkg] [--stop-after]
                                  # 기본: csc 4445 + console 8081 + csp 5060 + cmp 9000 기동 유지
                                  # --stop-after: 검증 후 전체 정리

# Phase 3 — 서비스 검증 전용 (Phase 2 완료 상태 전제)
cims.sh verify phase3             # 4시나리오 (VoLTE 음성/영상 + PTT 그룹 음성/영상)
                                  # 판정: 각 시나리오 seg_*.rtp 녹취 +1 이상 → PASS

# Console 진입점
# http://<ens160>:3000/testbed/verify    — TB-Console (검증 실행 표준, 항상 사용 가능)
# https://<ens160>:8081/testbed/modules  — 배포본 console (Phase 2 이후, 모듈관리)
# http://<ens160>:3001/                  — Dev-Console (Phase 1, 가입자 화면, 소스 vite)
# https://<ens160>:8080/                 — Test-Console (Phase 1, dist HTTPS)
```

## 부록 C. 문서 관리

- 본 문서는 검증 절차의 SSOT. 변경 이력은 git 관리.
- 진행 중 보완은 검증 리포트에 먼저 기록 후 본 문서에 반영.
- 사용자 용어 정의 변경 시 §0.2 명명 규칙과 §0.9/§0.10 시나리오 정의 동기화 필수.
