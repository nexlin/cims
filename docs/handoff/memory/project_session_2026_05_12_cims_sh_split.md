---
name: 2026-05-12-cims-sh
description: "cims.sh 의 운영 명령 (start/stop/restart/status/log/ha) 을 agent/bin/cims-{svc,ha,health,notify} 로 분리. B 옵션 통합 (단일 keepalived.conf.tpl + cims@.service.tpl). 6 commits push."
metadata: 
  node_type: memory
  type: project
  originSessionId: 0f0f28b3-7806-4f46-bc85-efea242cdfbd
---

## 결과 (2026-05-12)

cims.sh 가 개발 도구로 한정됨 — 배포본 운영 의존성 끊김. **6 commits push (origin/main bbd277e)**.

LIVE pipeline-full **38 / PASS 34 / FAIL 0 / SKIP 4 / 268.1s** — 회기능 무영향.

## 신규 구조

```
agent/
├── bin/                       # 운영 진입점 (운영자 / cims_agent 호출)
│   ├── cims-svc       lifecycle (start/stop/restart/status/log)
│   ├── cims-ha        HA (install/config/check/apply/start/stop/status)
│   ├── cims-health    listen probe (keepalived 가 호출)
│   └── cims-notify    state hook (keepalived 가 호출)
├── lib/                       # source-only library
│   ├── lifecycle.sh           # ~400줄, cims.sh 에서 이전
│   └── ha.sh                  # cmd_ha + B 통합 render
├── keepalived/
│   ├── keepalived.conf.tpl    # 단일 generic (services 반복)
│   ├── ha.json.example
│   └── (out/, ha.json — .gitignore)
└── systemd/cims@.service.tpl  # instantiated (%i = svc slug)
```

옛 서비스별 12 파일 (keepalived_{csc,csp,psp}.conf.tpl + check_{csc,csp,psp}.sh + notify_{csc,csp,psp}.sh + cims-{csc,csp,psp}.service.tpl) 모두 삭제. 신규 서비스 추가 = `ha.json.services` 한 줄 추가만.

## cims.sh 변경

- 운영 함수 ~855줄 제거 → 1435 줄 (원본 2290)
- 운영 명령 dispatcher: "agent/bin/cims-svc 로 이전됨" 안내 + exit 2 (silent breakage 차단)
- usage() 운영 섹션 → agent/bin/* 안내
- 잔여: init/build/configure/clean/reset/preflight/verify/sim/pkg/sync

## 외부 호출처 변경

- `csc/src/handlers/service_control.py` — `_invoke_cims_sh` → `_invoke_cims_svc`. driver=cims_svc, env CIMS_SVC_PATH, systemd unit cims@<svc>.service
- `agent/cims_agent.py:job_process_control` — install_path/agent/bin/cims-svc 호출 + `CIMS_DIST_DIR=install_path` 환경변수 전달 (cims-svc 가 install_path 기준으로 DIST_DIR 결정)
- `verify/lib/shell.py` — `run_cims_svc` 신규 (운영 명령 전용)
- `verify/lib/items/stage3/start.py:19` — `run_cims_svc("start")`
- `verify/lib/items/stage5/_native_steps.py:453` — `run_cims_svc("restart","tb-csc")`

## cims-svc 의 DIST_DIR 결정

우선순위:
1. `CIMS_DIST_DIR` 환경변수 (cims_agent 등 caller 명시 시) — install_path 기준
2. 소스 트리 직접 (`<repo>/agent/bin/cims-svc`) → `<repo>/build/dist`
3. 소스 트리의 dist (`<repo>/build/dist/agent/bin/cims-svc`) → `<repo>/build/dist`
4. 배포본 → `_AGENT_PARENT` (agent 의 부모)

## 6 Commits

```
bbd277e docs(ha): Stage 6 — ha_design.md §11 운영 가이드 갱신
7361534 refactor(cims.sh): Stage 5 — 운영 명령 완전 제거 + cims_agent → cims-svc
802b35b refactor(agent): Stage 4 — 외부 호출처를 agent/bin/cims-svc 로 전환
a52eed4 refactor(agent): Stage 3 — agent/systemd cims@.service.tpl (instantiated)
d81b51b refactor(agent): Stage 2 — agent/keepalived B 옵션 통합 (단일 tpl)
9867679 refactor(agent): Stage 1 — agent/bin + agent/lib 신규 (운영 도구 분리)
```

## 디버깅 메모 (Stage 4 → 5 사이)

Stage 4 시도에서 S5-CSC-RUN FAIL. 원인 — cims_agent 가 install_path/agent/bin/cims-svc 호출 시 install_path/agent/bin/ 가 install-agent.sh 의 working dir 라서 cims-svc 없음 → dev fallback (`/home/nex/work/cims/build/dist/agent/bin/cims-svc`) 사용 → cims-svc 의 자동 감지가 build/dist 를 DIST_DIR 로 잡음 → install_path (mgmt-server) 와 mismatch.

해결: cims-svc 에 `CIMS_DIST_DIR` override + cims_agent 가 install_path 전달.

## 후속 / 미완

- `docs/user-manual/deployment_workflow.md` — cims.sh 운영 명령 참조 정리 (별도 라운드)
- `docs/VERIFICATION_MANUAL.md` — 동일
- agent install-agent.sh — install_path/agent/bin/ 디렉토리 생성 + symlink 또는 PATH 추가 (운영자 편의)
- HA 인프라 자체의 LIVE 검증 (2-node 환경 마련 후)
