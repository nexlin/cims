# 분산 배포 아키텍처 (Agent / Package / Deployment)

> 버전: 1.0 (2026-04-21, P10 + Phase B)

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
- **Package**: CSC 에 업로드된 모듈 tarball. `meta.json` + `config_template.json` 포함
- **Deployment**: "특정 Agent 에 특정 Package 를 배치한 인스턴스". `process_name` (CSP/PSP/...) + `service_functions` (volte/ptt/...) 필드로 변종 구분
- **Collection**: 각 Deployment 의 `install_path/config/*.jsonl` — listener, trunk, route, acl 등 행 단위 설정

## 2. 배포 디렉토리 구조

```
<agent 설치 디렉토리>/modules/
└── csp/
    ├── 0.0.1/
    │   ├── CSP/                  ← process=CSP (VoLTE+PTT+IBCF 통합)
    │   │   ├── bin/csp
    │   │   ├── cims.sh
    │   │   ├── config.json       ← scalar 설정 (template sections)
    │   │   └── config/
    │   │       ├── listeners.jsonl
    │   │       ├── trunks.jsonl
    │   │       ├── routes.jsonl
    │   │       ├── acl.jsonl
    │   │       └── services.jsonl
    │   └── PSP/                  ← process=PSP (PTT 전용, 같은 버전 공존)
    └── 0.0.2/                    ← 새 버전은 병렬 설치 → 롤백 가능
```

- 버전 업그레이드 시 기존 `config/` 와 `config.json` 을 새 버전 디렉토리로 자동 복사 (Agent `job_install`)
- 같은 모듈의 여러 프로세스 변종(CSP/PSP/ISP 등) 공존 가능
- 롤백: `agent_deployment.install_path` 를 이전 버전 경로로 전환

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
  - **TODO (Phase C)**: 클라이언트 인증서 기반 mTLS 로 강화

## 7. 관련 문서

- `api/admin_api.md` — packages/agents/deployments REST
- `api/agent_api.md` — Agent 프로토콜
- `api/collection_api.md` — Collection 프록시
- `design/modules/agent.md` — Agent 상세
- `design/features/package_and_template.md` — 패키지 포맷
- `design/features/sip_runtime_config.md` — jsonl 런타임 설정
