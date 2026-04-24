# CIMS 기능 보완 검증 절차

> 목적: CIMS 패키지의 빈번한 기능 보완 시 신속한 안정화를 위해 검증을 3단계로 체계화한다.
> 적용 범위: CSP/CMP/CSC/Console/Agent 중 어느 한 모듈이라도 변경되면 본 절차를 따른다.

---

## 0. 공통 원칙

### 0.1 TB-CSC / TB-Console / TB-agent (상시 테스트베드)

검증 대상 모듈이 Phase 1~3 진행 중 반복 재기동되더라도 UI 세션이 끊기지 않고, 개발 단계에서 tarball 메타/설정이 각 모듈에 실제로 반영되는지까지 확인할 수 있도록 **TB 3종** 을 상시 동작시킨다.

| 서비스 | 포트 | 역할 |
|---|---|---|
| TB-CSC | 4419 | 패키지/에이전트/배포/설정 템플릿/검증 실행 관리. 상시 동작 |
| TB-Console | 3000 | TB-CSC UI. 모듈관리 + 배포 + 검증 실행/리포트 뷰 |
| TB-agent | sync 9902 | TB-CSC 에 enroll, 자기 호스트에 검증 대상 모듈을 설치/기동/재설정. install_path: `/tmp/cims-tb-agent/` |
| 검증 대상 CSC | 4420 | Phase 2~3 에서 TB-agent 로 배포되는 대상 |
| 검증 대상 Console | 3001 | Phase 2~3 중 검증용으로 기동 |
| Phone | 3002 | MCPTT UE Web (3000 에서 이전) |

**재기동 규칙**: TB 3종은 Phase 진행 중 내리지 않는다. **소스 변경 후 빌드되어 번들/바이너리 해시가 갱신된 경우에만** 자동 재기동한다. DB 는 공유 (`cims`). TB-agent state(enroll/cert) 는 `/tmp/cims-tb-agent/state/` 에 유지되어 재기동 시 재사용.

### 0.2 Phase 별 역할 구분 (중요)

**Phase 1 — 기능 검증의 중심.** 기능 보완 시 대부분의 검증은 여기서 끝나야 한다.
- 모든 기능·회귀 시나리오 검증
- TB-CSC/TB-Console 을 통한 설정 반영 검증 포함
- 빌드·기동·config overlay·런타임 통합 동작 확인

**Phase 2 — 배포 기능 검증.** 기능 검증은 Phase 1 에서 이미 완료된 전제. Phase 2 는 **새 릴리즈 tarball 을 TB-CSC 로 업로드/배포했을 때 대상 CSC/Console 이 정상 기동**되는지만 검증한다.

**Phase 3 — New-CSC 경유 배포 검증.** TB-CSC 가 배포한 New-CSC(4420) 가 다시 CSP/CMP/Sim 을 배포할 때의 체인 검증. **기능 회귀는 수행하지 않는다** (Phase 1 에서 끝나야 함). Phase 3 는 배포 체인의 무결성만 본다.

> 즉, 같은 회귀 시나리오를 Phase 1/3 에서 반복 실행하지 않는다. Phase 1 이 충분히 검증되었다는 전제에서, Phase 2/3 는 배포 메커니즘 자체만 확인한다.

### 0.3 초기화 범위

검증 전 환경 초기화 시 **가입자 정보는 보존**, 그 외 운영 데이터는 모두 초기화한다. TB-CSC/TB-Console 은 **건드리지 않는다**.

| 구분 | 테이블 / 자원 | 동작 |
|---|---|---|
| 보존 | `users`, `organizations`, `voip_subscriptions`, `ptt_subscriptions`, `ptt_groups`, `ptt_group_members`, `user_rejects` | 그대로 둔다 |
| 보존 (TB) | TB-CSC(4419) 및 TB-Console(3000) 프로세스, 인증서, 로그 | 그대로 둔다 |
| 보존 (TB-agent 레코드) | `cims_agent` 의 `name='tb-agent-local'` 행 (session_token 포함) | 조건부 보존 |
| 초기화 (모듈 설정) | `sip_service`, `sip_service_listener`, `csp_listener`, `sip_trunk`, `routing_rule*`, `routing_access_list`, `csp_config_audit` 등 런타임 config 계열 | TRUNCATE |
| 초기화 (배포 등록) | `cims_instance`, `cims_package`, `agent_deployment`, `agent_job`, `agent_metric` (`cims_agent` 은 `name<>'tb-agent-local'` 만 DELETE) | TRUNCATE / 조건부 DELETE |
| 초기화 (세션/로그) | `auth_codes`, `refresh_tokens`, `voip_call_logs`, `ptt_call_logs`, `*_participants`, `recordings`, `recording_segments`, `stats_*` | TRUNCATE |
| 초기화 (파일) | `build/dist/<모듈>/modules/**` (Phase 1 직접 기동본), `build/dist/{csc,csp,cmp,sim}-server/` (Phase 2/3 배포 대상, 0.10 참조), `service_log/`, `msg_log/`, `cert/agent_mtls/issued` 발급 cert | rm -rf |
| 초기화 (프로세스) | 검증 대상 csc(4420)/csp/cmp/cspsim/agent/console(3001) | `cims.sh reset` |

`cims.sh reset` 은 위 범위를 자동 처리하며 TB 는 제외한다.

### 0.4 외부 연동 IP

- 테스트 서버는 DHCP. 외부 연동용 IP 는 **`ens160` 인터페이스의 IP** 로 가정한다.
- `cims.sh preflight` 가 `ens160` IP 를 자동 감지해 리포트에 기록.
- `configure.sh --local-ip <ens160_ip>` 로 tarball 재구성 (localhost 는 외부 접근 불가).

### 0.5 사전조건 체크리스트

- [ ] `git status` clean, 브랜치/커밋 해시 기록
- [ ] pending DB migration 적용 완료
- [ ] `ens160` IP 확인
- [ ] 포트 충돌 없음 (4419, 4420, 3000, 3001, 3002, 5060, 5061, 9000, 9001, **9902**(TB-agent sync))
- [ ] TB 3종 동작 확인 (`cims.sh status` 에서 tb-csc / tb-console / tb-agent running)
- [ ] TB-agent 가 TB-CSC 에 `approved` 상태로 enrolled (`cims_agent` 테이블)
- [ ] mTLS 모드 검증이면 TB-CSC 의 `Agent.MtlsEnabled: true` 확인

### 0.6 합격 기준 (공통)

- 빌드: warning/error 0
- 대상 모듈 기동 후 로그에 `ERROR`/`FATAL` 없음
- Phase 별 회귀 시나리오 전부 PASS
- Flow/Msg 로그 무결성 (sesid 일관, body seq 매칭)

### 0.7 Phase 1 검증 시나리오 (회귀 6항목)

Phase 1 은 **`build/dist/` 안에서 직접 기동·기능 검증** 하는 단계. tarball 생성/업로드/TB-agent 배포 검증은 **Phase 2** 에서 수행한다. 아래 회귀 6항목이 모두 PASS 해야 합격.

**기존 회귀 시나리오 (고정 — 기능 보호용)**

1. **cspsim REGISTER × N** — `-auth_id "<IMSI>@<domain>"` 필수. 성공률 100%.
2. **VoIP 2자 통화 (B2BUA)** — 녹취 `seg_*.rtp` 생성, 양 leg 동일 `sesid`, `session.json` 의 `call_ids`.
3. **PTT 그룹콜 (5 member)** — multipart INVITE, Conference NOTIFY, floor port 협상, `m=application` 분리.
4. **CSC 가입자/그룹 변경 → NOTIFY** — admin API CRUD → `notify_csp` → 캐시 갱신 + GMS/CMS NOTIFY.
5. **CSC subscribe (IdMS/GMS/CMS)** — TB-Console Flow 페이지 nodes 순서 정상.
6. **(mTLS 모드) Cert rotation e2e** — `cert_rotate_pending=1` → heartbeat `cert_rotate:true` → agent rotate → exit → 재기동 후 새 cert 적용.

`cims.sh verify phase1` 및 TB-Console `/testbed/phase1` 실행 버튼이 위 6항목을 순차 수행하고 결과를 리포트에 기록한다.

> Phase 2 로 넘어가기 전 별도 확인 항목(tarball 메타 무결성, TB-agent 배포, static config overlay, dynamic collection sync)은 2.4 참고.

### 0.8 리포트 양식

경로: `verify_reports/<YYYYMMDD_HHMMSS>_<phase>.md` (TB-Console `/testbed/verify` 에서 자동 저장/조회)

내용:
- 환경: 브랜치/커밋, `ens160` IP, DB migration 버전, 빌드 해시
- Phase 별 체크리스트 PASS/FAIL
- Phase 1: 시나리오 결과 (번호별 PASS/FAIL + 로그 경로)
- Phase 2/3: 배포 단계별 성공 여부 + 검증 대상 모듈 health
- 이슈: severity (Blocker / Major / Minor), 재현 절차, 로그 스니펫 경로

### 0.9 롤백/재시작 지점

**원칙: Phase 2 또는 Phase 3 에서 이슈 발생 시 Phase 1 부터 재수행한다.**
Phase 1 이 충분히 검증되었다면 Phase 2/3 에서 기능 이슈가 발생해서는 안 된다는 전제. Phase 2/3 의 기능 이슈는 Phase 1 검증 미흡. 단 **배포 메커니즘 이슈**(agent enroll 실패, config overlay 누락 등)는 해당 Phase 내에서 보완 후 재수행 허용.

### 0.10 Phase 2/3 배포 대상 디렉토리 및 명명 규칙

Phase 2/3 에서는 agent 와 각 모듈을 **대상 호스트별 디렉토리** 에 분리 설치하여, Phase 1 의 `build/dist/<모듈>/` 직접 기동본과 물리적으로 구분한다. 이로써 배포 기능의 end-to-end 가 독립적으로 검증된다.

**디렉토리 레이아웃**

```
build/dist/
├── csc/           # Phase 1 직접 기동            (Test-CSC, 4421) — 유지
├── csp/           # Phase 1 직접 기동            (Test-CSP) — 유지
├── cmp/           # Phase 1 직접 기동            (Test-CMP) — 유지
├── console/       # Phase 1 직접 기동            (Test-Console, 8080) — 유지
├── phone/         # Phase 1 직접 기동            (Test-Phone, 5060) — 유지 
├── cwrtc/         # Phase 1 직접 기동            (Test-CWRTC, 5061) — 유지
├── cspsim/        # Phase 1 직접 기동            (Test-CSPSIM, 9000) — 시험후 종료
├── csc-server/    # Phase 2: Test-agent & Test-csc를 가 csc(4420), console(80) 모듈 배포
├── csp-server/    # Phase 3: csc 가 agent + csp 모듈 배포
├── cmp-server/    # Phase 3: csc 가 agent + cmp 모듈 배포
└── sim-server/    # Phase 3: csc 가 agent + sim(cspsim) 모듈 배포
```

> ※ 포트 체계 (설계):
> - **Phase 1 (Test-\*, dev/debug 포트)**: Test-CSC 4421 / Test-Console 8080 — 운영 포트와 분리되어 Phase 2 배포본(csc 4420 / console 80)과 동일 호스트에서 공존 가능.
> - **Phase 2 (csc-server 배포본, 운영 포트)**: csc 4420 / console 80.
> - **공용/서비스 포트**: Test-CSP SIP UDP 5060·TCP 25061·TLS 5061, Test-CMP UDP 9000, Test-Phone SIP 5060 (UE client), Test-CWRTC 5061 (TLS/WS), Test-CSPSIM 9000 (시험 종료 후 소켓 해제).
> - 현행 코드는 Phase 1 csc=4420 / console=3001 기준. **신규 설계(4421 / 8080 및 4420 / 80)로의 전환은 Phase 2 실측 구현 시점에 configure.sh + csc.json/.env 갱신 + §0.1 TB 표 반영을 한꺼번에 진행한다.**

**각 `<x>-server/` 내부 규약**

```
<x>-server/
├── agent/         # cims_agent (sync, enroll state, 발급 cert)
├── <모듈>/        # 배포된 모듈 (pkg.json, config.json overlay, modules/**)
└── config/        # agent → 모듈로 전달된 collection (*.jsonl)
```
(csc-server 의 `<모듈>` = `csc/` (+ 옵션 `console/`), csp-server = `csp/`, cmp-server = `cmp/`, sim-server = `sim/`)

**명명 규칙 (문서 내 용어 통일)**

- **csc** — Phase 2 에서 `csc-server/csc/` 로 배포된 CSC 인스턴스 (포트 4420, 운영). Phase 3 배포의 주체. 기존 "검증 대상 CSC" / "New-CSC" 표기와 동의.
- **console** — Phase 2 에서 `csc-server/console/` 로 배포된 Console 인스턴스 (포트 80, 운영). Phase 3 UI 진입점.
- **csp / cmp / sim** — Phase 3 에서 csc 로부터 각각 `csp-server/csp/`, `cmp-server/cmp/`, `sim-server/sim/` 로 배포된 인스턴스.
- **Test-\<X\>** — Phase 1 에서 `build/dist/<모듈>/` 로 직접 기동한 인스턴스의 별칭 (Test-CSC / Test-CSP / Test-CMP / Test-Console / Test-Phone / Test-CWRTC / Test-CSPSIM).
- **Test-agent** — Phase 2 에서 `csc-server/agent/` 로 TB-CSC 가 설치한 **per-host agent**. TB-agent(TB-CSC 자체에 상주하는 검증 환경 제어용 상시 agent, sync 9902)와는 별개 개체.

**초기화 관계**: `cims.sh reset` 은 `build/dist/*-server/` 전체를 초기화 범위에 포함 (0.3 참조).

---

## Phase 0 — 변경 분석 (진입 전 필수)

기능 보완 작업 착수 전 리포트 서두에 기록한다.

- 변경 범위 / 영향 모듈 (CSP / CMP / CSC / Console / Agent)
- 추가/변경 DB 테이블 · migration 스크립트
- 변경된 config template (`csp.json.template`, `config_template.json`, `cmp.json`)
- 회귀 리스크 플래그: Flow/Msg 포맷, `sesid` 규약, B2BUA 라우팅, CMP 포트 풀, mTLS 인증서 발급 경로 등 "건드리면 광범위하게 깨지는" 영역 해당 여부
- TB-Console 모듈관리에서 설정 템플릿 편집이 필요한 변경인지 여부

---

## Phase 1 — 기능 검증 (필수, 대부분의 검증을 여기서 끝냄)

기능 보완의 본 검증 단계. **새로 추가/수정한 기능 + 0.7 회귀 시나리오 전체** 가 여기서 통과해야 한다.

### 1.1 초기화
- `cims.sh reset` — 검증 대상만 초기화, TB 는 유지 (0.3 범위)

### 1.2 빌드 [1/3]
- `cims.sh build`
- C++ + Web UI 빌드 결과가 `build/dist/` 에 적재된다. 환경값 반영은 [2/3] configure 책임.
- 배포 tarball 은 생성하지 않는다 — Phase 1 은 `build/dist/` 안에서 직접 기동·검증한다.
- warning/error 0 확인, 번들 해시 바뀌면 TB 도 자동 재기동.
- (배포 tarball 생성은 Phase 2 의 [3/3] `cims.sh pkg` 단계에서 수행)

### 1.3 Configure [2/3]
- `cims.sh configure --local-ip <ens160_ip>`
- 시험환경 설정. `csp.json`, `cmp.json`, `csc.json` (4420 용), `csc-tb.json` (4419 용) 재생성
- CSP 는 `csp/config/config_template.json` 의 `deploy_value` 를 통해 환경변수 치환 후 `csp.json` 생성
- TB-Console `.env` 분기 적용

### 1.4 수동 기동 (검증 대상 전체 기동, TB 는 이미 동작 중)
순서: CMP → CSP → CWRTC → CSC → Console → Phone

```bash
cims.sh start          # 인자 생략 = 전체 모듈
```

개별 모듈만 재기동할 때는 `cims.sh start <name>` / `cims.sh restart <name>` 사용 (name ∈ cmp/csp/cwrtc/csc/console/phone).

### 1.5 Health Check
- 리슨 포트: 4419 / 4420 / 3000 / 9000 / 5060 / 5061
- 로그 `ERROR`/`FATAL` 없음
- TB-CSC → 검증 대상 CSC HEARTBEAT 정상

### 1.6 기능 검증 (본 단계의 핵심)

TB-Console `/testbed/phase1` 에서 **▶ 실행** 클릭 → 0.7 회귀 6항목 순차 실행 → 리포트 자동 생성.

추가로 수동 확인:
1. **이번 보완된 기능** — Phase 0 변경 분석의 대상 기능 직접 조작.
2. **build/dist 내부 동작** — 검증 대상 모듈이 `build/dist/<모듈>/` 경로에서 직접 기동되어 로그/녹취/Flow 가 정상 출력되는지 확인.
3. **Console > 테스트베드 > 모듈관리** — 각 모듈의 **버전 / 설정 템플릿 / 설정** 이 `build/dist/<모듈>/pkg.json` + `config_template.json` 에서 읽혀 표시되는지 확인. 스칼라 값 편집 시 overlay 가 `build/dist/config.json` 에 저장되고 재기동 후 반영되는지 확인.
4. (tarball 업로드·TB-agent 배포 검증은 Phase 2 에서 수행)

> **정보 흐름**: Phase 1 에서는 `build/dist/` 가 SOT. `/api/v1/packages` 및 `/api/v1/modules` 가 dist 파일을 직접 읽어 Console 에 노출한다. 동일 구조의 파일들이 Phase 2 의 `cims.sh pkg` 단계에서 그대로 tarball 로 묶인다.

### 1.7 결과 리포팅
- TB-Console `/testbed/verify` 또는 `verify_reports/<ts>_phase1.md`
- 0.8 양식 준수

### 1.8 이슈 처리
- Blocker/Major 존재 시 **코드 보완 → Phase 1 의 1.1 부터 재수행**
- Minor 는 리포트 기록 후 진행 여부 판단

**Phase 1 이 모든 항목 PASS 한 이후에만 Phase 2 로 진입한다.**

---

## Phase 2 — 배포 기능 검증 (릴리즈 직전, 배포 기능 자체만)

Phase 1 의 기능 검증이 끝난 전제 하에, **새 릴리즈 tarball 을 TB-CSC 로 배포했을 때 대상 CSC/Console 이 정상 기동되는지**만 확인한다. 기능 회귀는 반복하지 않는다.

### 2.1 초기화
- `cims.sh reset` (TB 유지)

### 2.2 빌드 & 설정 & 패키지 (3단계 분리)
Phase 1 통과 후, **배포 tarball 은 여기서만 생성**한다. 세 단계는 완전 독립:

```bash
cims.sh build                               # [1/3] warning/error 0 → build/dist 갱신 (tarball 없음)
cims.sh configure --local-ip <ens160_ip>    # [2/3] 시험환경 설정 (IP/DB/도메인)
cims.sh pkg --no-bump                       # [3/3] build/dist/<모듈>/ 을 tarball 로 묶음
                                            #       → build/dist/packages/*.tar.gz
```

- `cims.sh build` / `configure` 는 Phase 1 에서 이미 실행되었을 것이므로, 소스·환경 변경이 없다면 생략 가능.
- `cims.sh pkg` 는 `build/dist` 가 있어야 함 — Phase 1 에서 Console 이 표시/편집한 **바로 그 파일**을 tarball 로 묶는다. 즉 Phase 1 에서 확인한 구성이 tarball 에 1:1 반영된다.
- tarball 루트에는 `meta.json` (pkg.json 에서 유도) + `config_template.json` (sibling 복사본) 이 함께 포함되므로 TB-CSC 업로드 시 `cims_package.config_template_json` 이 자동 채워진다.
- 버전 bump 가 필요하면 `cims.sh pkg -v X.Y.Z` 또는 `--no-bump` 생략(자동 patch+1).

### 2.3 TB-Console 에서 배포 (대상: `build/dist/csc-server/`)

Phase 2 의 배포 대상 디렉토리는 `build/dist/csc-server/` (0.10 참조). TB-agent 가 해당 호스트에 먼저 **Test-agent** 를 설치하고, 그 agent 가 이어서 csc / console 모듈을 하위 디렉토리에 배치한다.

1. TB-Console(`https://<ens160_ip>:3000/`) 접속, admin 로그인
2. **배포 > 패키지** 업로드: `cims-csc-<ver>.tar.gz` (필요 시 `cims-console-<ver>.tar.gz` 도)
3. **배포 > 서버** 에서 **csc-server** 호스트 등록 → Test-agent enroll
   - install_path: `build/dist/csc-server/agent/`
4. **csc** 모듈 배포 → `build/dist/csc-server/csc/` 에 설치 · 기동 (포트 4420)
5. **console** 모듈 배포 (Phase 3 UI 진입점) → `build/dist/csc-server/console/` 에 설치 · 기동 (포트 80)
6. 기동 확인: csc(4420) / console(80) 리슨, TB-CSC → csc HEARTBEAT 정상

### 2.4 검증 항목 (배포 기능 한정)
- agent enroll 성공 (인증서 발급, mTLS 모드면 cert 유효성)
- 패키지 해시 일치 (tarball → install_path)
- scalar config overlay 반영 (`install_path/config.json`)
- collection (`config/*.jsonl`) 전달
- 모듈 기동 성공 (프로세스 리슨, health 체크)
- TB-CSC → 검증 대상 CSC HEARTBEAT 정상

### 2.5 결과 리포팅
- `verify_reports/<ts>_phase2.md`
- **배포 체크리스트** (위 2.4 항목) PASS/FAIL

### 2.6 이슈 처리
- 배포 기능 이슈 → Phase 2 내에서 보완 후 재수행 (Phase 1 재수행 불필요)
- 기능 회귀 발견 시 → 반드시 Phase 1 부터 재수행 (Phase 1 검증이 미흡했다는 신호)

---

## Phase 3 — New-CSC 경유 CSP/CMP/Sim 배포 검증 (릴리즈 직전, 배포 체인)

Phase 2 에서 배포된 New-CSC(4420) 가 다시 CSP/CMP/Sim 을 배포할 때의 **체인 동작** 만 확인한다. 기능 회귀는 반복하지 않는다.

### 3.1 배포 (csc 경유, Console 에서 실행)

Phase 2 에서 `csc-server/` 로 배포된 **csc** 가 Phase 3 의 배포 주체가 된다. 대상 디렉토리 구조는 0.10 참조 (`csp-server/`, `cmp-server/`, `sim-server/`).

Console(`http://<ens160_ip>/` — 포트 80) 에서 아래 순서로 실행:

#### (1) agent 설치
각 대상 호스트에 agent 를 먼저 배포. 동일 호스트에 여러 agent 공존 시 `CIMS_AGENT_SYNC_PORT` env 로 sync 포트 분리.

1. **csp-server** 호스트 등록 → agent enroll → `build/dist/csp-server/agent/`
2. **cmp-server** 호스트 등록 → agent enroll → `build/dist/cmp-server/agent/`
3. **sim-server** 호스트 등록 → agent enroll → `build/dist/sim-server/agent/`

#### (2) 모듈 배포
각 agent 가 csc 로부터 tarball 을 수령해 자기 호스트의 모듈 디렉토리에 설치.

1. **csp** 패키지 배포 → `build/dist/csp-server/csp/`
2. **cmp** 패키지 배포 → `build/dist/cmp-server/cmp/`
3. **sim** 패키지 배포 → `build/dist/sim-server/sim/`

#### (3) 모듈 설정
각 모듈의 설정 템플릿에 Phase 1 과 동일한 실제 시험 환경을 반영 (IP·포트·realm·도메인·그룹 ID·CMP 포트 풀). Console 에서 편집하면 agent 가 heartbeat 시 수집해 해당 모듈의 `config.json` / `config/*.jsonl` 로 내려준다. 재기동이 필요한 필드는 Console 에서 restart 배지로 표시.

1. Console > **모듈관리 > csp** → scalar overlay (listen IP/포트, realm) + collection (routing rules, SIP trunk 등)
2. Console > **모듈관리 > cmp** → scalar overlay (RtpStartPort, PttRtpStartPort, PttFloorStartPort, CSP address)
3. Console > **모듈관리 > sim** → scalar overlay (csp server IP, 테스트 계정/그룹)

#### (4) 기동 및 smoke-test
1. csp → cmp → sim 순으로 기동 (의존성 순)
2. 리슨 포트 확인: csp(5060/5061/25061), cmp(9000 + RTP 풀), sim(sync only)
3. **REGISTER 1건 smoke-test** — Console 의 실행 버튼 또는 `build/dist/sim-server/sim/cspsim -count 1 -scenario register` 로 1건만. 배포 체인이 실제 트래픽으로 이어지는지만 확인. **기능 회귀는 Phase 1 에서 끝난 것으로 간주하며 반복하지 않는다**.

> 설정 원칙: Phase 1 과 동일한 실제 시험 환경을 재현 (IP·포트·realm·도메인·그룹 ID).

### 3.2 검증 항목 (배포 체인 한정)
- New-CSC → 각 agent enroll / heartbeat 정상
- 패키지 해시 일치
- config overlay, collection 전달 정상
- CSP/CMP/Sim 프로세스 리슨 포트 정상 (5060/5061/9000)
- 각 모듈 startup 로그 `ERROR`/`FATAL` 없음
- **서비스 smoke-test 1건만** (REGISTER 1개 성공 여부) — 배포 체인이 실제 트래픽으로 이어지는지 확인용. 기능 회귀는 Phase 1 에서 끝났으므로 반복 안 함.

### 3.3 결과 리포팅
- `verify_reports/<ts>_phase3.md`
- 배포 체크리스트 + smoke-test 결과

### 3.4 이슈 처리
- 배포 체인 이슈 → Phase 3 내에서 보완 후 재수행
- smoke-test 실패 시 → Phase 2 의 배포 또는 Phase 1 의 기능 누락 의심, 원점 재수행

---

## 부록 A. 알려진 함정 (2026-04-22 기준 누적)

- `cims.sh pkg` 는 patch +1. 버전 고정은 `--no-bump` 필수.
- `make dist` 이후 `configure.sh` 로 IP 재반영 필수.
- localhost 설정으로는 외부 접근 불가 → 반드시 `ens160` IP 사용.
- cspsim REGISTER 성공은 `-auth_id "IMSI@domain"` 형식 필수. `cims.sh verify phase1` 은 DB 에서 자동 조회.
- TB-CSC 의 mTLS 기능: `Agent.MtlsEnabled: true` overlay 필수.
- 동일 호스트에 여러 agent: `CIMS_AGENT_SYNC_PORT` env 로 주입 (CLI 인자 미지원).
- Agent cert rotate 는 `exit(0)` 만 수행. 재기동은 systemd/supervisor 책임.
- TB-Console 빌드 분기: `VITE_ADMIN_TARGET=https://127.0.0.1:4419 npm run build` → `build/dist/console-tb/dist`.

## 부록 B. 주요 명령어 요약

```bash
# 환경
ip -4 addr show ens160
git rev-parse --short HEAD

# TB 는 항상 돌고 있어야 함 (cims.sh status 에서 tb-csc, tb-console running 확인)
cims.sh status

# Phase 1: 기능 검증 (build/dist 안에서 기동 — tarball 생성 없음)
cims.sh verify phase1         # 자동 (preflight → reset → [1/3] build → [2/3] configure → start all → 시나리오 → 리포트)
# 또는 TB-Console /testbed/verify 에서 UI 로 실행

# Phase 2: 배포 기능 검증 — build/configure/pkg 3단계 분리
cims.sh build                 # [1/3] Phase 1 에서 빌드했다면 생략 가능
cims.sh configure --local-ip <ens160_ip>   # [2/3] 환경 변경 없으면 생략 가능
cims.sh pkg --no-bump         # [3/3] tarball 생성 (버전 고정)
# TB-Console → 배포 > 패키지 업로드 → 대상 호스트에 배포

# Phase 3: New-CSC 경유 배포 체인 (Phase 2 에서 배포된 console 80 에서 UI 진행)
```

## 부록 C. 문서 관리

- 본 문서는 검증 절차의 SSOT. 진행 중 보완은 리포트 후 본 문서에 반영.
- 변경 이력은 git 으로 관리.
