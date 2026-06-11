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

## 2. 배포 디렉토리 구조 (버전 단위 설치 — 2026-06-10 확립)

```
/opt/cims-agent/                      ← agent 설치 루트
├── agent/                            ← agent 자신 (self-upgrade 가 통째로 교체)
└── csp/                              ← 모듈 루트 (/opt/cims-agent/<module>)
    ├── 0.0.35/                       ← 버전 디렉토리 = deployment.install_path
    │   ├── csp/                      ←   tarball top dir (변종이면 psp/, isp/)
    │   │   ├── bin/csp
    │   │   ├── config/csp.json       ←   scalar 설정 (tarball 동봉 base)
    │   │   └── config.json           ←   deployment overlay (Roles/LocalIp/Port)
    │   ├── config/                   ←   HA 동기 collection jsonl — **해당 버전에 귀속**
    │   │   ├── local_nodes.jsonl     ←   (CSP jsonlDir = csp.json 부모×3 = 여기)
    │   │   ├── access_services.jsonl
    │   │   └── ...
    │   ├── run/  log/                ←   pid / 로그 (cims-svc DIST_DIR=버전 디렉토리)
    └── 0.0.36/                       ← 새 버전 병렬 설치 → 롤백 가능 (최신 3개 유지)
```

- **버전 경로 파생은 Agent `job_install` 이 수행**: params 의 (install_path, package_name, package_version) 로 `<module_root>/<version>` 을 계산 — deployment 레코드가 legacy 경로(공유 루트/모듈 디렉토리)여도 자동 정규화. 설치 성공 stdout 의 `at <path> (` 를 OAM report 훅이 파싱해 `deployment.install_path` 갱신 + `prev_install_path`/`install_history` 기록.
- **config 이관**: 직전 라이브 경로(legacy 포함) 또는 최신 sibling 버전에서 `config/*.jsonl`(collection) + `<pkg>/config.json`(overlay) 을 새 버전 디렉토리로 복사. `params.config` 가 오면 overlay 는 그 값이 SoT. **버전별 config 스키마가 다를 수 있으므로 collection 동기화(agent `job_sync_config`)도 항상 활성 버전 디렉토리의 `config/` 에 기록**된다.
- **롤백**: `POST /api/v1/deployments/<id>/rollback` (body 생략 시 직전 버전) — 레코드 install_path 전환 → collection 재동기(sync_config; 구버전 설치 후 변경분 stale 방지) → restart 큐잉. Agent 는 supervised 경로 비교로 다른 경로(구 버전)에서 떠 있는 인스턴스를 먼저 stop 한다 (포트 충돌 방지).
- **보존 정책**: 설치 성공 시 모듈 루트의 버전 디렉토리를 mtime 최신 3개만 유지(prune). 버전 패턴(`^\d+(\.\d+){1,3}…`) 디렉토리만 대상 — legacy 평탄 설치 잔재(bin/, config/ 등)는 건드리지 않는다.
- 같은 모듈의 여러 프로세스 변종(CSP/PSP/ISP 등)은 각자 모듈 루트가 분리 (`/opt/cims-agent/psp/<ver>/`).

> ⚠️ **install_path durability 제약** (2026-06-01 확립, 버전 단위 설치와 양립)
> 모듈 루트는 반드시 **`/opt/cims-agent/<module>` (agent/ 트리 밖, `agent/` 와 sibling)** 이어야 한다.
> - **이유**: agent self-upgrade(`install-agent.sh --update-only`)는 `/opt/cims-agent/agent/` 트리 전체를 교체(old → `agent.old` → 삭제)한다. install_path 가 agent/ 안에 있으면 upgrade 마다 모듈 바이너리가 파괴되고, 실행 중 프로세스는 **deleted-inode 좀비**(`/proc/<pid>/exe` → `… (deleted)`)로 남아 재시작 불가가 된다.
> - 모듈 루트가 분리되므로 `jsonlDir = <버전 디렉토리>/config` 도 모듈·버전별로 격리 — 구 공유 루트(`/opt/cims-agent` 직접 지정) 시절의 listener 포트 바인드 충돌·collection 공유 문제가 구조적으로 사라진다.
> - 경로 마이그레이션 후 옛 좀비는 `lifecycle.sh` `kill_deleted_inode_orphans <name>`(start 변종이 호출, deleted-inode 동일 이름만 kill)가 정리한다.

## 2.1 상용(Private) 부트스트랩 — base 인스톨러 (2026-06-11)

상용 반입 절차의 1단계(서비스 모듈과 무관한 base 운영평면 설치)는 빌드 산출물
**`cims-bootstrap-<oam버전>.tar.gz`** (`./cims.sh pkg` 끝에 자동 조립, 단독은
`./cims.sh installer`) 로 수행한다:

```
cims-bootstrap/
├── install.sh            # sudo ./install.sh [--prefix /opt/cims-agent] [--port 4419]
│                         #   [--admin-pass PW] [--no-systemd] [--no-start]
├── packages/             # oam / console / agent tarball 3종 (서비스 모듈 미포함)
└── README.md
```

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
- 설치 레이아웃은 본 문서 §2 의 버전 단위 설치와 동일(`/opt/cims-agent/modules/{oam,console}/<ver>/`,
  runtime store 는 `modules/oam/runtime` 버전 무관) — 이후 agent 배포
  체계가 자연 인수.
- **base 콘솔 프로파일** (2026-06-11): 동봉 콘솔은 `VITE_CONSOLE_PROFILE=base`
  빌드 — 메뉴가 **관리>시스템 + 관리>릴리스(개발자모드)** 만 (서비스 pack
  메뉴/위젯은 번들에서 제외, DCE). 서비스에 필요한 기본 메뉴·위젯(대시보드/
  구성/성능/기록 등)은 3·4단계에서 **풀 프로파일 console 패키지**(동봉본보다
  높은 버전 필수 — 동일 버전은 시드 멱등 skip)로 업데이트 시 나타난다.
  `cims.sh installer` 가 base 빌드(`cims-console/dist-base`)를 자체 수행해
  풀 콘솔 tarball 의 dist 만 교체·동봉 (`meta.json profile=base`).
- **메뉴 편집** (콘솔 사이드바, admin): ① 영역(운용/관리 그룹핑) 라벨 변경·
  커스텀 영역 추가/삭제 ② 섹션 순서/라벨/숨김/영역 이동 — 단 **시스템/릴리스
  섹션은 잠금**(부트스트랩 생명선) ③ 커스텀 메뉴 그룹 + 위젯 합성 페이지
  (`/custom/<slug>`, 빈 EditableLayout 보드) 추가. 저장은 OAM
  `/api/v1/console/menu` (`items` + `custom_sections` + `areas`).

## 3. 제어 평면 (Control Plane)

### 3.1 장기/배치 작업 — CSC → Agent (Pull 모델)

```
Agent ── POST /api/agent/heartbeat ──> CSC
        (X-Agent-Token, sync_port 보고)
Agent <── pending jobs 응답 ──
Agent ── job 실행 ── install/start/stop/restart/update_config/upgrade_agent
Agent ── POST /api/agent/report ──> CSC
```

- 30초 주기 heartbeat 에 pending job 최대 10개 pickup
- 결과는 report 로 보고 → CSC 가 `agent_deployment.status`, `install_path` 등 업데이트

### 3.2 동기 조회/편집 — CSC → Agent (Push 모델, 새로움)

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
