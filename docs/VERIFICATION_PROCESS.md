# CIMS 검증 절차

> **목적**: CIMS 패키지를 개발/보완한 뒤 배포 전 / 배포 과정 / 배포 이후 3단계로 검증한다.
> **적용 범위**: agent · csc · csp · cmp · cwrtc · console · phone · simulator 중 한 모듈이라도 변경되면 본 절차를 따른다.
> **원본 SSOT**: 이 문서. 진행 중 보완은 검증 리포트에 기록 후 본 문서에 반영.

---

## 0. 공통

### 0.1 3단계 구조 (한눈에)

| Phase | 단계 | 목적 | 장소 | 기능 검증 |
|---|---|---|---|---|
| **1** | 배포 **전** | 개발/보완 확인 + 기본 회귀 | `build/dist/<모듈>/` (직접 기동) | ✅ 기본 4시나리오 + 추가 3시나리오 + 보완 사항 |
| **2** | 배포 **과정** | tarball → TB-CSC → agent 배포 메커니즘 | `build/dist/csc-server/` | ✗ (배포 메커니즘 한정) |
| **3** | 배포 **이후** | 배포본에서 기본 회귀 + 보완 재확인 | `build/dist/{csc,csp,cmp,sim}-server/` | ✅ 기본 4시나리오 + 보완 사항 (Phase 1 과 동일 범위) |

**Phase 3 의 의의**: Phase 1 의 단순 반복이 아니라 "실제 배포 환경에서 같은 결과가 나오는지" 를 검증. 배포 실수·환경 의존 버그는 여기서만 잡힌다.

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
| **Phase 2/3 배포본** | csc | 4420 | Phase 2 배포, Phase 3 배포 주체 |
| | console | 80 | Phase 3 UI 진입점 (cap_net_bind 또는 reverse proxy) |
| | csp / cmp / sim | 5060·5061 / 9000 / sync | Phase 3 배포 체인 |
| | Test-agent | sync 9903 | per-host agent (csc-server-local 등) |

> **명명 규칙**
> - **TB-\*** — 상시 테스트베드 (환경 제어용). Phase 진행 중 내리지 않는다.
> - **Test-\*** — Phase 1 의 `build/dist/<모듈>/` 직접 기동 인스턴스 별칭.
> - **배포본** (무접두) — Phase 2/3 에서 agent 경유로 설치된 운영 인스턴스. `<x>-server/<모듈>/` 경로.
> - **Test-agent** ≠ **TB-agent** — Test-agent 는 per-host (csc-server-local 등), TB-agent 는 sync 9902 로 상시 동작하는 환경 제어용.
> - **Console 3분화**: 같은 `cims-console/` 코드베이스가 Dev-Console (소스 vite, 3001) / Test-Console (dist HTTPS, 8080) / 배포본 console (80) 으로 분기. `cims.sh start console` 이 SRC_CONSOLE 존재 여부로 자동 분기.

### 0.3 포트 공존 설계

포트 번호 전체는 §0.2 표 참조. 핵심 원칙:

- **Phase 1 Test-\*** (dev/debug 포트) 와 **Phase 2·3 배포본** (운영 포트) 은 번호가 달라 동일 호스트에서 공존 가능 — Test-CSC 4421 vs 배포본 csc 4420, Dev-Console 3001 vs 배포본 console 80 등.
- **TCP 4421 ↔ UDP 4421** 은 완전히 다른 서비스 (Test-CSC admin TCP / CSP CscInterface UDP, `csp/CspServer.cpp:259`). proto 다름이라 무충돌.
- **`verify phase2` 자동화** 는 csc Start job 을 `Server.Port=4445` overlay 로 기동 → Phase 1 Test-CSC 4421 / 배포본 4420 과 모두 분리.
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

가입자 정보 기반, Console 에서 실행. Phase 1 은 Dev/Test-Console (3001/8080), Phase 3 은 배포본 console (80) 에서 수행.

| # | 시나리오 | 확인 포인트 |
|---|---|---|
| (1) | **VoLTE 음성 2자 통화** (B2BUA) | REGISTER × 2 성공 · INVITE 통화 연결 · 녹취 `seg_*.rtp` · 양 leg 동일 `sesid` · `session.json` 의 `call_ids` 쌍 |
| (2) | **VoLTE 영상 2자 통화** (B2BUA) | (1) + 영상 RTP m-line 협상 · 양방향 비디오 패킷 흐름 |
| (3) | **PTT 그룹 음성 통화** (5인) | multipart INVITE (SDP + OMA POC XML) · Conference NOTIFY · floor port 협상 · `m=application` 분리 · 플로어 요청/그랜트 동작 |
| (4) | **PTT 그룹 영상 통화** (5인) | (3) + 영상 stream + 그룹 멤버 간 영상 분배 |

### 0.10 Phase 1 전용 추가 시나리오

Phase 1 에서만 수행 (Phase 3 는 §0.9 4시나리오만):

- **CSC 가입자/그룹 변경 → NOTIFY** — admin API CRUD → `notify_csp` → `CspUserMap`/`CGroupMap` 캐시 갱신 → GMS/CMS NOTIFY 발송
- **SUBSCRIBE/NOTIFY 전체 흐름** (IdMS / GMS / CMS) — TB-Console Flow 페이지 nodes 순서 정상
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
- Dev-Console (3001) 또는 Test-Console (8080) 에서 TB-Console `/testbed/phase1` → ▶ 실행 — §0.9 4시나리오 + §0.10 Phase 1 추가 3시나리오 순차 수행
- 개발/보완 사항 직접 조작 (§1.1 에서 식별된 기능)
- Console > 테스트베드 > 모듈관리 — 버전/설정 템플릿/overlay 반영 확인

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

## 2. Phase 2 — 배포 과정 검증

Phase 1 에서 확인된 구성이 tarball 로 묶여 TB-CSC 를 거쳐 대상 호스트에 정확히 배치되는지 — **배포 메커니즘 자체만** 검증. 기능 회귀는 반복하지 않는다.

### 2.1 단계

**(1) tarball 생성** — `cims.sh pkg --no-bump` → `build/dist/packages/*.tar.gz`. tarball 루트에 `meta.json` + `config_template.json` 포함. Phase 1 에서 Console 이 표시/편집한 바로 그 파일이 1:1 반영됨.

**(2) TB-CSC 업로드** — TB-Console(`https://<ens160>:3000/`) → 배포 > 패키지 → `cims-csc-<ver>.tar.gz` + `cims-console-<ver>.tar.gz` 업로드. TB-CSC 가 `cims_package.config_template_json` 자동 채움.

**(3) Test-agent enroll** — TB-Console 에서 csc-server 호스트 등록 → `cims_agent.py --name csc-server-local --sync-port 9903` 기동 → TB-CSC 에 enroll/approve. `verify phase2` 자동화가 `--heartbeat-sec 3` 으로 구동.

**(4) 모듈 배포** — Test-agent 가 tarball 수령 → `build/dist/csc-server/{csc, console}/` 에 설치.

**(5) config overlay 검증** — `POST /api/v1/deployments` body 의 `config` 가 `install_path/config.json` 에 반영되는지 확인. `agent_deployment.config_json` 컬럼을 경유.

**(6) Start / Health / Stop** — csc 만 자동 (`Server.Port=4445` overlay 로 기동 → Phase 1 Test-CSC 4421 / 배포본 운영 4420 모두와 분리). 포트 LISTEN + `tcp:4445=open` + stop cleanup 확인.

> **console 은 install-only** — 운영 console:80 기동은 Phase 3 진입 시 설계 (cap_net_bind vs reverse proxy) 확정 후 자동화.

### 2.2 자동 명령

```bash
cims.sh verify phase2 [--skip-build] [--skip-pkg] [--keep-agent]
```

내부: `cmd_reset --keep-processes` (Phase 1 모듈 유지) → build → configure → pkg → admin login → agent enroll → tarball upload → install → overlay 검증 → start (4445) → health → stop → Test-agent 종료. 16단계 상세는 `verify_reports/<ts>_phase2.md` 참조.

### 2.3 합격 조건

- Agent enroll OK (mTLS 모드면 cert 유효성)
- Tarball 해시 일치 (tarball → install_path)
- Install 완료
- Config overlay 반영
- CSC Start (4445 LISTEN) + Health (tcp:4445=open) + Stop cleanup 전부 OK

### 2.4 이슈 처리

- 배포 메커니즘 이슈 → Phase 2 내 보완 후 재수행 (Phase 1 재수행 불필요)
- 기능 이슈 발견 → Phase 1 으로 회귀 (Phase 1 검증 미흡 신호)

---

## 3. Phase 3 — 배포 이후 검증

Phase 2 로 배포된 csc 를 주체로 csp/cmp/sim 배포 체인을 완성하고, **실제 운영 포트에서 Phase 1 과 동일한 기능 결과가 나오는지** 재확인. 배포 실수·환경 의존 버그 검출.

### 3.1 진입 조건

- **Phase 1 모듈 일부만 유지**: Console (Dev-Console 3001 또는 Test-Console 8080) 만 유지 — 사용자 UI 세션 유지용. **서버 모듈 (csp / cmp / cwrtc / phone / cspsim) 은 전부 중지**. 포트 충돌 (5060/9000/8443 등) 및 공유 로그/DB wipe 시 충돌 방지.
- Phase 2 완료: `build/dist/csc-server/csc` 기동 상태 (4420 운영 또는 4445 overlay)
- TB 3종 유지
- Test-CSC (4421) 는 중지 또는 유지 (배포본 csc 와 포트 분리되므로 공존 가능. 단 Console 의 proxy 대상이 Test-CSC 면 유지)

### 3.2 단계

**(1) csc → csp/cmp/sim 배포 체인 완성**
Console 80 (배포본) 또는 TB-Console (3000) 에서:
- `csp-server` / `cmp-server` / `sim-server` 호스트 등록 → 각 agent enroll
  - 동일 호스트 다중 agent 는 `CIMS_AGENT_SYNC_PORT` env 로 sync 포트 분리
- 각 agent 가 csc 로부터 tarball 수령 → `build/dist/<x>-server/<모듈>/` 에 설치

**(2) 배포본 모듈 설정**
Console 모듈관리에서 각 모듈 scalar overlay + collection 편집. agent heartbeat 로 수집 → `config.json` / `config/*.jsonl` 내려감.
- csp: listen IP/포트, realm, routing rules, SIP trunk
- cmp: RtpStartPort, PttRtpStartPort, PttFloorStartPort, CSP address
- sim: csp server IP, 테스트 계정/그룹

설정 원칙: Phase 1 과 동일한 실제 시험 환경을 재현 (IP·포트·realm·도메인·그룹 ID).

**(3) 로그/DB wipe**
`cims.sh reset` 실행 — §0.5 범위 (가입자 보존, 로그 폴더·서비스로그·녹취·DB 런타임 테이블 전부 wipe, TB 유지). Phase 1 서버 모듈은 §3.1 에서 이미 중지됐으므로 파일 잠금·포트 충돌 없음.

> 로그 폴더와 DB 는 Phase 1/2/3 간 공유 구조 (`build/dist/log/`, `ext_mnt/{service_log,msg_log}/`, `cims` DB) — Phase 3 에서의 깨끗한 데이터 확인을 위해 이 단계 필수.

**(4) 배포본 실행**
순서: csc (이미 기동) → csp → cmp → sim → console:80.
- Console 에서 각 모듈 start
- 리슨 확인: csp(5060/5061/25061), cmp(9000 + RTP 풀), sim (sync only), console (80)
- TB-CSC → 배포본 csc HEARTBEAT 정상

**(5) 검증** — 배포본 console (80) 에 접속 → §0.9 **기본 4시나리오 재수행** (VoLTE 음성/영상, PTT 그룹 음성/영상) + 개발/보완 사항 재확인.
- Phase 1 과 동일 결과 (Flow 메시지, sesid 일관, 녹취, NOTIFY 등) 확인
- 차이 발생 시 → 배포 설정 누락 또는 환경 의존 버그 의심

**(6) 리포팅** — `verify_reports/<ts>_phase3.md`. 배포 체인 + §0.9 4시나리오 결과 + 보완 사항 결과.

### 3.3 자동 명령

```bash
cims.sh verify phase3 [--skip-build] [--skip-pkg] [--keep-agent]
```

**현재 범위 (v3, 2026-04-24)**:
- Phase 1 서버 모듈 중지 (cmp/csp/cwrtc/phone/cspsim) — Console·TB 유지
- `cmd_reset --keep-processes` 로 로그/DB/배포본 wipe
- 3개 Test-agent (csp/cmp/sim-server-local, sync 9904·9905·9906) enroll
- csp/cmp/cspsim tarball 업로드 + deployment 생성
- Install job 폴링 + 설치 파일 검증 (meta.json + config/)
- **csp·cmp**: Start job (포트 LISTEN 대기) → Health check
- **배포본 csp jsonlDir 에 access_services.jsonl 시드 + SIGUSR1 reload**
- **4시나리오 실행 (cspsim → 배포본 csp, §0.9)**:
  - 14.1 VoLTE 음성 2자 통화 (-no_video)
  - 14.2 VoLTE 영상 2자 통화 (기본 video 포함)
  - 14.3 PTT 그룹 음성 통화 5인 (-no_video)
  - 14.4 PTT 그룹 영상 통화 5인
  - 판정: 각 시나리오 실행 후 `seg_*.rtp` 녹취 파일 +1 이상 생성 → PASS
- 시나리오 후 csp·cmp Stop + Test-agent 종료
- sim: install-only (cspsim 은 4시나리오에서 cmd_sim 경유 단발 실행)

**Console Phase 3 UI**: 별도 작업. 사용자가 배포본 기동 후 Console 에서 모듈관리 + 4시나리오 실행하는 워크플로우 지원 (현재는 `verify phase3` CLI 자동화로 대체).

**주의 (함정)**: `--skip-pkg` 사용 시 tarball 속 config (csp.json LocalIp 등) 가 stale 하면 start 실패 (UdpListen error). ens160 IP 변경 후에는 configure + pkg 재실행 필수.

### 3.4 이슈 처리

- **배포 체인 이슈** (enroll 실패, config 미전달, 리슨 실패) → Phase 3 내 보완 후 재수행
- **기능 이슈** (4시나리오 또는 보완 사항 실패) — 원인 판별:
  - Phase 1 에서 같은 증상 재현됨 → 코드 이슈, Phase 1 부터 재수행
  - Phase 1 재현 안 됨 → 배포 경로·설정·환경 의존 버그, Phase 2 배포 설계 재검토

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

# Phase 2 — 배포 과정 검증
cims.sh build
cims.sh configure --local-ip <ens160_ip>
cims.sh pkg --no-bump
cims.sh verify phase2 [--skip-build] [--skip-pkg] [--keep-agent]

# Phase 3 — 배포 이후 검증
cims.sh verify phase3 [--skip-build] [--skip-pkg] [--keep-agent]
                                 # v3 (2026-04-24): install + csp/cmp start/health/stop +
                                 # 4시나리오 자동 실행 (VoLTE 음성/영상 + PTT 그룹 음성/영상)
                                 # 판정: 각 시나리오 seg_*.rtp 녹취 +1 이상 생성 → PASS
```

## 부록 C. 문서 관리

- 본 문서는 검증 절차의 SSOT. 변경 이력은 git 관리.
- 진행 중 보완은 검증 리포트에 먼저 기록 후 본 문서에 반영.
- 사용자 용어 정의 변경 시 §0.2 명명 규칙과 §0.9/§0.10 시나리오 정의 동기화 필수.
