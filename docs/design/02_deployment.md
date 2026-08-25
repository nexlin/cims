# 분산 배포 아키텍처 (Agent / Package / Deployment)

> Agent 배포 데몬, 패키지 12종 (base 8 + 변종 4), Deployment overlay,
> Collection 프록시 (jsonl + SIGUSR1), mTLS 의 통합 모델.

CIMS 는 여러 물리 서버에 모듈을 개별 배포하고 **Console 에서 중앙 관리**하는 구조를 제공합니다.

## 1. 핵심 엔티티

```
┌──────────┐   1:N   ┌──────────┐   N:1  ┌──────────┐
│ Server   │ ──────> │Deployment│ <───── │ Package  │
│ (Agent)  │         │ (설치본) │        │ (모듈    │
└──────────┘         └──────────┘        │  tarball)│
                          │              └──────────┘
                          │  1:N collections (jsonl)
                          ↓
                    ┌─────────────┐
                    │ config      │
                    │  listeners  │
                    │  trunks     │
                    │  routes     │
                    │  acl        │
                    └─────────────┘
```

- **Agent**: 각 호스트에서 실행되는 배포 데몬 (`agent/cims_agent.py`). CSC 와 주기적 heartbeat + 명령 수행
- **Package**: CSC 에 업로드된 모듈 tarball. `meta.json` + `config_template.json` 포함. base 8 + 변종 4 = **12종** (csp 의 psp/isp / cmp 의 pmp/imp). 변종은 `cims.sh cmd_pkg` staging 에서 base dist 디렉토리 복사 + 바이너리/`config/<m>.json`/`<m>.sh` rename 후 tar — tarball 내부도 진짜 분리.
- **Deployment**: "특정 Agent 에 특정 Package 를 배치한 인스턴스". `process_name` (CSP/PSP/ISP/CMP/PMP/IMP) + `service_functions` (volte/ptt/ibcf/...) 필드로 변종 구분. `install_path/config.json` 의 deployment overlay 가 `csp.json`/`cmp.json` 시작 직전 머지 → 같은 base 바이너리에 다른 Roles/LocalIp/Port.
- **Collection**: 각 Deployment 의 `install_path/config/*.jsonl` — listener, trunk, route, acl 등 행 단위 설정. 즉시 적용 (SIGUSR1).

## 2. 배포 디렉토리 구조 (버전 단위 설치 — `current` 심볼릭)

agent 와 모든 모듈은 **버전 디렉토리를 병렬로 보존**하고, 활성 버전을 가리키는
**`current` 소프트 링크**로 운영한다. 버전 전환(업그레이드/롤백) = `ln -sfn <ver> current`
(원자적) + 재기동. 프로세스·systemd·sudoers·모니터링은 전부 고정 경로 `.../current` 만
참조하므로 버전 번호와 무관하다.

```
/opt/cims-agent/                      ← 설치 루트 (prefix; CIMS_AGENT_PREFIX)
├── agent/                            ← agent 자신 (버전 단위 + 롤백)
│   ├── 0.0.40/  0.0.41/              ←   버전 디렉토리 (최신 3개 유지)
│   └── current -> 0.0.41             ←   활성 버전 심볼릭 (systemd ExecStart·sudoers 의 고정 경로)
├── modules/<module>/                 ← 모듈 루트 (/opt/cims-agent/modules/<module>)
│   ├── 0.0.35/                       ←   버전 디렉토리 = deployment.install_path (DB 기록)
│   │   ├── <pkg>/                    ←     tarball top dir (변종이면 psp/, isp/)
│   │   │   ├── bin/<pkg>
│   │   │   ├── config/<pkg>.json     ←     scalar 설정 (tarball 동봉 base)
│   │   │   └── config.json           ←     deployment overlay (Roles/LocalIp/Port)
│   │   ├── config/                   ←     HA 동기 collection jsonl — 해당 버전에 귀속
│   │   │   ├── local_nodes.jsonl     ←     (jsonlDir = <module_root>/current/config 로 접근)
│   │   │   └── ...
│   │   └── run/  log/                ←     pid / 로그
│   ├── 0.0.36/                       ←   새 버전 병렬 설치 → 롤백 가능 (최신 3개 유지)
│   ├── current -> 0.0.36             ←   활성 버전 심볼릭 (CIMS_DIST_DIR / jsonlDir 통로)
│   └── (oam 전용) runtime/           ←   버전 무관 영속 store (cert / _secrets / file_store)
├── state/                            ← agent enroll 상태 (state.json, agent.crt/key) — 버전 밖, 영속
├── run/                              ← supervised.json / managed_ips.json / pending_reports.jsonl — 버전 밖
└── update.sh  uninstall.sh  setup-sudoers.sh   ← 버전 밖 (current flip 에 불변)
```

- **활성 버전 지시 = `current` 심볼릭 (운영 통로).** 프로세스는 항상 `.../current` 경로로 기동한다:
  모듈은 `CIMS_DIST_DIR=<module_root>/current`, agent 는 systemd `ExecStart=agent/current/cims_agent.py`,
  sudoers 는 `agent/current/bin/cims-priv|cims-ha`. 버전 번호가 바뀌어도 `current` 경로는 불변이라
  systemd/sudoers/모니터링을 다시 건드리지 않는다.
- **DB 는 기록(SoT 아님).** `deployment.install_path` 는 버전 디렉토리(`modules/csp/0.0.36`),
  `package_version`/`prev_install_path`/`install_history` 와 함께 보존된다. Agent 가 install/start/
  restart/rollback 시 **"DB 가 준 버전 디렉토리로 `current` 를 맞추고(`ln -sfn`) current 로 띄운다"** —
  DB↔심볼릭 동기·번역은 전적으로 agent 내부. (콘솔은 DB 레코드로 배포 목록·버전을 표시.)
- **prefix 앵커.** agent 코드는 `CIMS_AGENT_PREFIX`(systemd 가 주입) 또는 `agent` 디렉토리 컴포넌트
  까지 walk-up 으로 prefix 를 도출한다 — flat/버전화/심볼릭 경유 무관. `state/`·`run/`·`modules/`·
  sub-script 가 **버전 트리 밖**(prefix 직하)이라 업그레이드·롤백·재enroll 에 생존한다.
- **버전 경로 파생은 Agent `job_install` 이 수행**: params 의 (install_path, package_name, package_version) 로 `<module_root>/<version>` 을 계산 — deployment 레코드가 legacy 경로(공유 루트/모듈 디렉토리)여도 자동 정규화. 전개·prune 후 `ln -sfn <version> current`. 설치 성공 stdout 의 `at <path> (` 를 OAM report 훅이 파싱해 `deployment.install_path`(버전 디렉토리) 갱신 + `prev_install_path`/`install_history` 기록.
- **config 이관**: 직전 라이브 경로(legacy 포함) 또는 최신 sibling 버전에서 `config/*.jsonl`(collection) + `<pkg>/config.json`(overlay) 을 새 버전 디렉토리로 복사. `params.config` 가 오면 overlay 는 그 값이 SoT. **버전별 config 스키마가 다를 수 있으므로 collection 동기화도 항상 활성 버전 디렉토리의 `config/` 에 기록**된다 (`current/config` 와 동일 inode).
- **롤백**: `POST /api/v1/deployments/<id>/rollback` (body 생략 시 직전 버전) — DB install_path 를 이전 버전으로 전환 → collection 재동기(구버전 설치 후 변경분 stale 방지) → restart 큐잉. Agent 는 restart 시 flip 직전 `readlink(current)` 로 직전 버전을 보존하고 `current` 를 타겟 버전으로 flip 한 뒤, **`/proc/<pid>/exe` 실경로가 타겟 버전 디렉토리 밖인 인스턴스(=구버전)를 먼저 stop**(포트 충돌 방지) 하고 기동한다.
- **보존 정책**: 설치 성공 시 모듈 루트(및 agent/)의 버전 디렉토리를 mtime 최신 3개만 유지(prune). 버전 패턴(`^\d+(\.\d+){1,3}…`) 디렉토리만 대상 — `current` 심볼릭·legacy 평탄 잔재(bin/, config/ 등)는 건드리지 않는다.
- 같은 모듈의 여러 프로세스 변종(CSP/PSP/ISP 등)은 각자 모듈 루트가 분리 (`/opt/cims-agent/modules/psp/<ver>/`, 각자 `current`).

> ⚠️ **버전 트리 밖 영속(durability) 제약**
> 다음은 **버전 디렉토리 밖**(prefix 직하 또는 모듈 루트 직하)에 둔다 — `current` flip / prune 에 생존해야 하기 때문:
> - agent `state/`(enroll·cert), `run/`(supervised.json·managed_ips·pending_reports), sub-script(update/uninstall/setup-sudoers). 버전 디렉토리 안에 두면 매 업그레이드마다 re-enroll·감독 상태 유실.
> - oam `modules/oam/runtime/`(file_store·`_secrets`·cert·JWT). 버전 안에 두면 업그레이드마다 토큰·계정·배포기록 소실. oam.json `CimsRuntimeDir` 는 절대경로라 `current` 경유 기동에도 동일 store 를 찾는다.
>
> **stale 인스턴스 정리**: `current` 통로 기동에선 신·구 버전 프로세스의 명령 경로가 같으므로(`current/bin/<m>`),
> 경로 문자열이 아니라 **`/proc/<pid>/exe` 실경로**(exec 가 심볼릭을 해소 → 실제 버전 inode)로 구버전을 식별해 stop 한다.
> prune(최신 3개 유지) 전이라 실경로가 유효하며, prune 이후의 deleted-inode 잔재는 `lifecycle.sh`
> `kill_deleted_inode_orphans <name>`(동일 이름 deleted-inode 만 kill)가 정리한다.

> **CSP 계열(csp/psp/isp) start 판정·pidfile** (`lifecycle.sh _start_csp_variant`)
> - **실효 접속점 정본 = `local_nodes.jsonl`.** 좀비 정리·포트 점유 판정은 csp.json `Setup.Sip.UdpPort`
>   (identity fallback — 실제 bind 포트와 다를 수 있음)가 아니라 local_nodes 의 enabled 리스너
>   실포트 전체(UDP/TCP/TLS)로 수행한다. 해석 순서는 CSP 와 동일: `Setup.ConfigJsonlDir` →
>   install 루트 `config/` → 변종 내부 `config/`.
> - **start 성공 = 자기 exe worker 생존 + primary 접속점 bind 확인**(폴링, 기본 20s —
>   `CIMS_CSP_START_TIMEOUT`). 판정 시점에 pidfile 을 실제 **포트 소유 worker** 로 확정한다 —
>   csp 는 daemonize fork 구조라 초기 pid($!)가 기동 중 소멸하고 다른 worker 가 서비스를 이어도
>   pidfile 이 고아가 되지 않는다. 즉사(연속 무프로세스)는 타임아웃 전 조기 실패.
> - **멱등 start**: pidfile 유실/사망이어도 자기 exe worker 가 primary 접속점을 물고 살아있으면
>   그 pid 로 pidfile 을 승계하고 성공 반환 — 건강한 서비스를 죽였다 다시 띄우지 않는다.
> - 자기 좀비 식별은 cmdline 패턴(`kill_stray`, 절대경로 직접 실행)과 `/proc/<pid>/exe` 대조
>   (`_kill_own_exe_strays`, lifecycle 상대경로 기동) 두 축 — lifecycle 는 `cd` 후 `bin/<name>` 으로
>   기동하므로 cmdline 패턴만으로는 자기 프로세스가 잡히지 않는다.
> - **파일 capability 바이너리(csp `setcap cap_net_admin` — IMS AKA+IPsec)의 프로세스는 동일 uid 라도
>   `/proc/<pid>/exe` 읽기·`ss -p` 소켓 귀속이 거부된다**(ptrace 접근 검사 — 대상 caps ⊆ 호출자 caps 요구).
>   lifecycle 의 exe 식별·포트 귀속은 이때 동봉 `cims-priv` 의 읽기 전용 서브커맨드
>   (`proc-exe`/`proc-pids-of`/`port-owner`, sudoers NOPASSWD)로 root 위임해 복원한다 — 위임 불가
>   환경(sudoers 미구성 dev)은 미식별=보수적 no-op 으로 폴백.

## 2.1 상용(Private) 부트스트랩 — base 인스톨러

상용 반입 절차의 1단계(서비스 모듈과 무관한 base 운영평면 설치)는 빌드 산출물
**`cims-bootstrap-<oam버전>.tar.gz`** (`./cims.sh pkg` 끝에 자동 조립, 단독은
`./cims.sh installer`) 로 수행한다:

```
cims-bootstrap/
├── install.sh            # sudo ./install.sh [--prefix /opt/cims-agent] [--port 4419]
│                         #   [--admin-pass PW] [--no-systemd] [--no-start]
│                         #   [--mount-src nas.example:/export/cims]
├── packages/             # oam / console / agent tarball 3종 (서비스 모듈 미포함)
└── README.md
```

> **권한 정책** — 설치 계열(`install.sh`·생성되는 `init` 단계)은
> **반드시 일반 계정에서 `sudo` 로** 실행한다. `install.sh` 는 상단 가드에서
> `EUID≠0` 또는 `SUDO_USER` 가 비어있거나 root(= root 직접 로그인 / sudo 미경유)면
> **즉시 종료** — sudoers/linger/서비스 IP 등 권한 작업만 누락된 채 진행되는 부분
> 설치를 차단. 서비스 계정(agent/OAM 프로세스 소유자)도 root 면 거부(`--user`/
> `--svc-user` 로 일반 계정 지정). **제거(uninstall)는 반대로 root 또는 sudo 둘 다 허용**
> (`uninstall-base.sh`/생성 `uninstall.sh` = `EUID≠0` 거부; 일반계정은 sudo 필요).

- **standalone OAM**: oam 패키지에 csc/src 의 서비스-중립 공유 라이브러리
  (httpsrv/util/services 일부)를 동봉 — csc(서비스 종속 모듈) 없이 단독 기동.
  가입자/조직 핸들러(admin/org, csc 측)는 선택 로드 (서비스 설치 후 자동 활성).
- **HTTPS 단일 오리진**: OAM 이 콘솔 SPA 정적 파일을 직접 서빙(`Console.StaticDir`,
  SPA fallback) — 콘솔+API 가 :4419 HTTPS 하나로 동작 (dev vite/npx serve 불요,
  air-gapped 에서 node 불요). self-signed cert·JwtSecret 은 install.sh 가 생성
  (재설치 시 보존, 상용 인증서는 `<oam>/cert` 교체).
- **시드 패키지 자동 등록**: 동봉 3종 tarball 을 `seed_packages/` 에 배치 → OAM
  첫 부팅 시 패키지 저장소에 멱등 등록 (`Packages.SeedDir`) — 콘솔 패키지
  목록과 `/install-agent.sh`·`/agent-bundle.tar.gz` 가 즉시 동작해 2단계(각 서버
  agent 설치)로 바로 진행 가능. 1단계 구성요소(oam/console/agent)도 패키지로
  보이므로 3~4단계에서 업데이트 가능.
- **base 모듈 deployment 자동 등록**: install.sh 가 로컬 agent 설치
  직후, 이 OAM 노드의 **oam·console 을 `status=running` deployment 로 등록**
  (`_self_deploy`, 멱등) → 콘솔 **"시스템/인프라 > 패키지 설치"**(=배포 목록)에
  oam/console 이 노출된다. `_create_deployment` 가 초기 `status`
  를 honor(기본 pending; running/stopped 시 `deployed_at` 기록). console 은 별도
  프로세스 없이 OAM 이 정적 서빙하므로 `module_down` 알람은 비데몬(console/agent)을
  제외한다(metric.modules 미보고 → running 이어도 오탐 방지).
- 설치 레이아웃은 본 문서 §2 의 버전 단위 설치와 동일(`/opt/cims-agent/modules/{oam,console}/<ver>/`
  + `current` 심볼릭, runtime store 는 `modules/oam/runtime` 버전 무관) — 이후 agent 배포
  체계가 자연 인수. install.sh 는 oam 전개 후 `ln -sfn <oam버전> modules/oam/current` 를 걸고
  `CIMS_DIST_DIR=modules/oam/current` 로 기동·감독(supervised.json)한다.
- **base 콘솔 프로파일**: 동봉 콘솔은 `VITE_CONSOLE_PROFILE=base`
  빌드 — 메뉴가 **관리>시스템 + 관리>릴리스(개발자모드)** 만 (서비스 pack
  메뉴/위젯은 번들에서 제외, DCE). 서비스에 필요한 기본 메뉴·위젯(대시보드/
  구성/성능/기록 등)은 3·4단계에서 **풀 프로파일 console 패키지**(동봉본보다
  높은 버전 필수 — 동일 버전은 시드 멱등 skip)로 업데이트 시 나타난다.
  `cims.sh installer` 가 base 빌드(`ems/core/console/dist-base`)를 자체 수행해
  풀 콘솔 tarball 의 dist 만 교체·동봉 (`meta.json profile=base`).
- **메뉴 편집** (콘솔 사이드바, admin): ① 영역(운용/관리 그룹핑) 라벨 변경·
  커스텀 영역 추가/삭제 ② 섹션 순서/라벨/숨김/영역 이동 — 단 **시스템/릴리스
  섹션은 잠금**(부트스트랩 생명선) ③ 커스텀 메뉴 그룹 + 위젯 합성 페이지
  (`/custom/<slug>`, 빈 EditableLayout 보드) 추가. 저장은 OAM
  `/api/v1/console/menu` (`items` + `custom_sections` + `areas`).

## 2.2 각 서버 agent 설치 — 통일 flow

2단계(각 서버에 agent 설치)는 base 노드의 `sudo ./install.sh` 와 **동일한
"일반 계정 + sudo" 패턴**을 사용한다. `install-agent.sh` 단일 스크립트가 모드로 분기한다:

```
# 콘솔 "시스템/서버 구성" 이 발급하는 install-command (토큰 명령 = 다운로드 전용, sudo 불필요)
curl -fsSLk https://<oam>:4419/install-agent.sh | bash -s -- \
     --oam-url https://<oam>:4419 --enrollment-token <tok> --name <노드명>
#  → 비root 실행이므로 install-agent.sh 가 download-mode 로 동작:
#    install-agent.sh + (토큰/URL/이름 내장된) install.sh 를 현재 디렉터리에 생성하고
#    "이제 설치는 1줄: sudo ./install.sh" 안내만 출력 (설치는 하지 않음).

# 설치 (토큰 재입력 없이 1줄)
sudo ./install.sh
#  → 설치 디렉터리를 대화형으로 질문(엔터=기본 /opt/cims-agent; --install-dir 로 비대화 지정).
#    추출 + sudoers + linger + enroll + systemd --user enable --now 까지 한 번에.
```

- **권한 모델**: `fresh` 설치는 root(sudo) 필수 — 서비스 계정은 `SUDO_USER`(또는
  `--svc-user`). enroll·systemd `--user`·linger 등 **사용자 세션 작업은 `runuser -u <svc>`**
  로 서비스 계정 컨텍스트에서 수행하고, sudoers/파일 소유권 등 root 작업은 직접.
  agent 는 종전대로 **`systemd --user` + linger** 로 동작(재부팅 자동기동·watchdog 유지).
- **`--update-only` (자가업그레이드)** 는 서비스 계정(non-root)으로 실행 — agent 의
  `upgrade_agent` job 경로가 그대로 호출(파일 교체만, 권한작업 없음). `--no-systemd`
  는 systemd 미사용 환경(base install.sh 의 nohup 경로)용으로 enroll 까지만 수행.
- **제거**: `sudo /opt/cims-agent/uninstall.sh` (root/sudo 필수). root 로 동작하되
  `systemd --user`/linger 정리는 서비스 사용자(`SUDO_USER`→없으면 설치 디렉터리 소유자)를
  `runuser` 로 진입해 수행.
- base install.sh 의 로컬 agent 단계도 이 통일된 `install-agent.sh`(root + `--svc-user`
  + `--install-dir` + 필요 시 `--no-systemd`) 호출로 일원화됐다.

> **OAM self-upgrade**: OAM 자기 자신을 업그레이드할 때의 안전 처리(health-gate·
> report 재시도·부팅 self-reconcile·pre-flight `--preflight`·명시 롤백; 불변식=OAM 은
> 자기 프로세스를 직접 kill 하지 않고 agent 가 재기동)는 별도 설계서
> [features/oam_self_upgrade.md](features/oam_self_upgrade.md) 참조.

## 3. 제어 평면 (Control Plane)

### 3.1 장기/배치 작업 — CSC → Agent (Pull 모델)

```
Agent ── POST /api/agent/heartbeat ──> CSC
        (X-Agent-Token, sync_port 보고)
Agent <── pending jobs 응답 ──
Agent ── job 실행 ── install/start/stop/restart/update_config/upgrade_agent
Agent ── POST /api/agent/report ──> CSC
```

- 2초 주기(기본, `--heartbeat-sec`; OAM 불통 시 지수 backoff 최대 60초) heartbeat 에 pending job 최대 10개 pickup
- 결과는 report 로 보고 → CSC 가 `agent_deployment.status`, `install_path` 등 업데이트

### 3.2 동기 조회/편집 — CSC → Agent (Push 모델)

Collection 편집처럼 **즉시 응답이 필요한 경우** Agent 가 노출하는 HTTPS REST 포트로 직접 호출.

```
CSC ── GET /collection?install_path=...&name=listeners ──> Agent :9900
                                                            (HTTPS + X-Agent-Token)
CSC ── PUT /collection?...  body={records:[...], signal:true} ──>
                           Agent → jsonl 원자 쓰기 → SIGUSR1 to CSP
```

- Agent 는 enroll 시 self-signed 인증서 생성, heartbeat 로 `sync_port` 를 CSC 에 보고
- 인증: `X-Agent-Token` (enroll 때 발급된 세션 토큰 동일 사용)

## 4. 데이터 평면 (Data Plane)

### 4.1 scalar 설정 (`config.json`)

- 저장소: `agent_deployment.config_json` (DB)
- 에이전트가 install 시 install_path/config.json 에 렌더링
- `update_config` job 으로 갱신 → CSP 는 재시작 필요 항목의 경우 수동 Restart

### 4.2 collection 설정 (`config/*.jsonl`)

- 저장소: **Agent 호스트의 jsonl 파일** (DB 없음)
- CSC 는 원본 저장하지 않고 Agent 에 프록시만 수행
- CSP 는 시작 시 jsonl 로드 → SIGUSR1 시 재로드

이 분리는 다음을 달성:
- deployment 별 자연스러운 격리 (install_path 다름)
- CSP 런타임 상태와 설정 원천 일치 (같은 디렉토리)
- 백업 = install_path tarball 만 복사

## 5. 상태 머신

```
Deployment.status
 ─ pending      (생성만 됨, 파일 없음)
 └> install job ──> stopped  (설치 완료, 실행 전)
                  └> start ──> running
                  └> stop ───> stopped
                  └> install ─> stopped (재설치)
                  └> uninstall ─> removed

Agent.status
 ─ pending      (Enrollment 토큰 발급됨, enroll 대기)
 └> enroll ──> approved ──┐
                          ↓ heartbeat
                        online  ←→  offline  (heartbeat 끊김 → offline)
```

### 의도(status) vs 실측(live_state) — 실측이 화면의 정본

`Deployment.status` 는 **의도**다(job 이 확정한다). 실제 프로세스 생존은 agent metric 의
`live_modules` 스냅샷에서 온 **`live_state`**(`up`/`down`/`None`)이고, 콘솔은 실측이
확정적이면 그것을 먼저 그린다(`depEffectiveStatus`).

**실측은 과도 상태(`deploying`)에서도 계산한다.** 종전에는 status 가 `running`/`stopped`
일 때만 계산해서, 배포 job 이 큐에 갇히면 프로세스가 멀쩡히 도는데도 화면이 영원히
"배포 중" 이었다(실측 사고). 과도 상태는 끝나지 않을 수 있다는 것을 전제해야 한다 —
실측을 감추면 운영자가 현실을 볼 통로가 없어진다. `pending`(아직 그 노드에 없음)과
`removed` 만 제외한다.

**과도 상태에는 시한이 있다.** `sweep_stuck_deploying`(60초 주기)이 `deploying` 고착을
실제 상태로 정정한다 — 마지막 job 이 성공/실패로 끝났거나 사라졌으면 즉시, 아직
queued/running 이면 5분이 지나고 실측이 확정적일 때만. 진행 중인 배포를 성급히 뒤집지
않으면서, 영구 고착은 없앤다.

### job 큐 인덱스는 캐시다

agent 별 대기 job 목록(`job_index/<agent_id>`)은 픽업 시 전수 스캔을 피하려는 **캐시**이지
정본이 아니다. 정본은 `control/jobs/*` 파일이다. 인덱스가 어긋나면(등록 실패 등)
그 agent 에게 내린 **모든 job 이 조용히 무시**되므로, 신선도를 `jobs/.seq`(마지막 발급 id)
로 O(1) 판정해 불일치 시 재구축한다. 종전에는 인덱스 **파일이 없을 때만** 재구축해서,
`queued: []` 로 어긋난 인덱스가 영구히 자기복구되지 않았다(실측: start job 이 큐에 갇혀
배포가 `deploying` 고착).

## 6. 보안

- Enrollment 토큰: 1회용, 생성 시점에 전달
- Session 토큰: enroll 응답으로 교환, 이후 heartbeat/report 인증
- Agent 의 sync REST: HTTPS (self-signed) + 같은 session 토큰 헤더 검증
- **mTLS** (opt-in, `csc.json` `Agent.MtlsEnabled: true`): CSC 자체 CA + agent 별 server cert 자동 발급. enroll/rotate 응답에 `mtls.{server_cert,server_key,ca_cert}` 포함. CSC proxy 는 `csc_client` cert 로 mTLS 연결. cert rotate 는 `cims_agent.cert_rotate_pending` DB 컬럼 + heartbeat → 재발급 응답 흐름.

## 7. 다중 인스턴스 패턴

같은 base 바이너리 (csp 또는 cmp) 를 여러 deployment 로 두고 deployment overlay (`install_path/config.json`) 의 Roles 토글 + `LocalIp`/포트 분기로 **VoLTE 와 PTT 인스턴스를 분리**할 수 있다.

CSC notify 라우팅 (`csc/src/services/mcptt.py::_notify_targets`):
- `GROUP_CHANGED` → PSP (PTT) 만
- `USER_CHANGED` 등 → CSP + PSP broadcast (dedup)
- `csc.json` 의 `CspNotify` (VoLTE) 와 `PspNotify` (PTT) 두 endpoint 동시 보유

검증 환경의 P1 5-server 토폴로지 (mgmt-server + volte-sip / volte-media / ptt-sip / ptt-media) 정의는 [VERIFICATION_PROCESS.md §1 S5](../VERIFICATION_PROCESS.md) 가 SSOT.

## 8. 관련 문서

- `api/admin_api.md` — packages/agents/deployments REST
- `api/agent_api.md` — Agent 프로토콜
- `api/collection_api.md` — Collection 프록시
- `design/modules/agent.md` — Agent 상세
- `design/features/package_and_template.md` — 패키지 포맷 (12 변종)
- `design/features/build_and_packaging.md` — 빌드/패키징 워크플로우 (콘솔 `/release/package`)
- `design/features/sip_runtime_config.md` — jsonl 런타임 설정
- `VERIFICATION_PROCESS.md` — 6단계 파이프라인 (P1 토폴로지 SoT)

## 설정 계층 — 패키지 기본값 vs 노드 overlay

**공유 스토리지를 가리키는 키는 패키지 기본값에 박지 않는다.** `CimsRuntimeDir`(관리
store)과 `ServiceLogging.Dir`(서비스 로그)이 그렇다. 부트스트랩 직후에는 공유 마운트가
**없는 것이 정상**이다 — 마운트를 붙이는 수단이 그 노드의 OAM 이 서빙하는 콘솔이기
때문이다. 패키지에 공유 경로가 박혀 있으면 새 노드는 반드시 없는 경로를 붙들고 시작한다
(실측: 서비스 로그 기록 실패가 분당 400건씩 17분, 그 로그가 진짜 원인을 덮었다).

두 키 모두 **패키지 기본값은 빈 값**이고, 비었을 때 노드 로컬로 해석한다
(`services/paths.py` — `local_runtime_dir` 하위). 공유 경로는 언제나 **배포 overlay** 가
정한다 — 패키지에는 들어가지 않는다. 로그는 로컬로라도 남긴다 — 비워서 로깅을 끄면
부트스트랩 노드의 진단 통로가 사라진다.

overlay 에 공유 경로가 들어가는 시점은 둘이다: **설치 시점**(부트스트랩 `[6/7]` 이 공유
스토리지 원본과 붙일 위치를 받아 fstype·store·로그 경로를 유도하고, 마운트가 없으면
붙인다 — agent 와 같은 엔진 `cims-priv mount-add`. 전제를 패키지 전개 전에 검사하고 어긋나면
중단), 또는 **나중에 콘솔 이관**(단일 → 이중화 전환). 자세한 조건은 `features/oam_ha.md` §9.4.


모듈 설정은 두 층이다. **노드 종속 값(경로·포트·시크릿·계정)은 언제나 overlay 가 정한다.**

| 파일 | 역할 | 수명 |
|---|---|---|
| `<install>/<comp>/config/<comp>.json` | 패키지 기본값 — 노드 종속 값 없음(빈 값) | 업그레이드가 교체 |
| `<install>/<comp>/config.json` | 노드 overlay (flat dotted) — OAM 배포설정이 SoT | 버전 간 이관 |

부트스트랩도 콘솔 설치도 **overlay 에만** 쓴다. 설치 경로가 다르면 같은 버전 패키지가
노드마다 달라지고, 패키지 기본값의 결함이 한쪽에서만 드러난다(실측 사고 —
[features/oam_ha.md](features/oam_ha.md) §12.5). 패키지에 빌드 머신 절대경로가 들어가는 것은
`S1-CONFIG-PORTABILITY` 가 막는다.
