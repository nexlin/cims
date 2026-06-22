---
name: CIMS TB 3종 인프라 (TB-CSC / TB-Console / TB-agent)
description: 검증 Phase 진행 중 UI 세션 유지용 "임시 기동" 인프라 요약. 포트/명령/파일 경로/설정 SoT. 구축 세션 2026-04-24 완료.
type: project
originSessionId: cd6b1a78-c813-4476-94ff-9e4c03c1033d
---
## 역할 (docs/VERIFICATION_PROCESS.md §0.1 / §0.10)

### TB 3종 (상시 동작)

| 서비스 | 포트 | 구동 방식 | 설정 SoT |
|---|---|---|---|
| **TB-CSC**     | 4419 (admin), 4431 (mcptt) | `python3 csc_app.py` + `CIMS_CSC_CONFIG=.../csc-tb.json` | `build/dist/csc/config/csc-tb.json` (configure.sh 에서 생성) |
| **TB-Console** | 3000 | `npm run dev -- --mode tb --port 3000 --host` | `ems/core/console/.env.tb.local` (VITE_ADMIN_TARGET=https://127.0.0.1:4419) |
| **TB-agent**   | sync 9902 | `python3 cims_agent.py --csc-url https://127.0.0.1:4419 --name tb-agent-local --state-dir /tmp/cims-tb-agent/state` | state 자체에 저장 (session_token + agent.crt/key) |

### Test-agent (Phase 2/3 verify 시 일시 기동)

`cims.sh verify phase2` 가 spawn → install 후 kill (--keep-agent 로 유지 가능).

| 설정 | 값 |
|---|---|
| name | `csc-server-local` (Phase 3 시 csp/cmp/sim-server-local 도 예정) |
| sync port | **9903** (TB-agent 9902 와 분리해 상시 공존) |
| csc-url | `https://127.0.0.1:4419` (TB-CSC) |
| state-dir | `build/dist/csc-server/agent/state/` |
| install-root | `build/dist/csc-server/` (env `CIMS_AGENT_INSTALL_ROOT`) |
| 개체 차이 | TB-agent ≠ Test-agent. TB-agent 는 검증 환경 제어용 상시, Test-agent 는 per-host (csc-server) 배포용 일시 |

### 검증 대상 측 포트 (현행 코드)

CSC 4420 · Console 3001 · Phone **3002** (이전 3000 → 3002).

**신규 설계 (docs §0.10 footnote, 전환은 Phase 2 실측 구현 시점)**:
- Phase 1 Test-CSC: **4421**, Test-Console: **8080** (dev/debug 포트, 운영과 분리)
- Phase 2 배포본 csc: **4420**, console: **80** (운영 포트, 동일 호스트 공존 가능)

## 핵심 명령

```bash
cims.sh start tb            # 3종 모두 (tb-csc → tb-console → tb-agent)
cims.sh start tb-csc        # 개별
cims.sh stop tb             # 3종 모두 중지 (tb-agent → tb-console → tb-csc)
cims.sh status              # "[검증 대상]" / "[TB ...]" 섹션 분리 출력
cims.sh preflight           # 검증 대상: 가용, TB 3종: 동작중 기준으로 보고
cims.sh reset [--all|--db|--files]  # TB 보존, cims_agent name=tb-agent-local 보존(I1 fix)
cims.sh verify phase2 [--skip-build] [--skip-pkg] [--keep-agent]
                            # Phase 2 install-only v1 — admin login → Test-agent enroll →
                            # csc/console tarball upload → deployment + install job →
                            # 설치 파일 검증 → verify_reports/<ts>_phase2.md
```

## TB-agent 자동 enrollment

`start tb-agent` 시 state 없으면 `_tb_issue_enrollment_token()` 이 자동:
1. TB-CSC(4419) `/api/v1/auth/login` 에 `{login_id:admin, password:1234}` → access token
2. `POST /api/v1/agents {name:tb-agent-local}` → enrollment_token (기존 레코드 있으면 DELETE 후 재생성)
3. `POST /api/v1/agents/{id}/approve` → approved 전환
4. agent 가 enrollment_token 으로 `/api/agent/enroll` 호출 → session_token + cert 수령

admin 계정 override: `CIMS_TB_ADMIN_ID` / `CIMS_TB_ADMIN_PASSWORD` (기본 admin/1234, `tests/test_env.json` 기준).

## 설계 근거

- **DB 공유** (`cims`): users/organizations/cims_agent 등 전부 공유. 별도 DB 분리 안 함 (사용자 결정 2026-04-24).
- **TB-Console dev 모드 전용**: `serve dist` 는 `/api` proxy 안 함 → `vite dev` proxy 로 `/api → https://127.0.0.1:4419` 라우팅. dist 빌드 불필요.
- **csc-tb.json overlay**: `csc.json` 을 기반으로 `Server.Port=4419`, `McpttServer.Port=4431`, `Log.File=log/csc_tb.log`, `Packages.Dir=packages_tb`, `Packages.BackupDir=packages_tb_trash`, `ConfigCacheDir=cache_tb` 만 치환. 시크릿/DB/도메인 공유.
- **csc_app.py 변경 최소화**: `_CONFIG_PATH = os.environ.get('CIMS_CSC_CONFIG') or os.path.join(_COMPONENT_ROOT, 'config', 'csc.json')` 한 줄만. CLI 인자 없이 ENV 로 분기.
- **reset 방침**: TB 3종은 건드리지 않음 (cmd_stop all / 포트 정리 목록 전부 제외). 단 DB `cims_agent` TRUNCATE 는 §0.3 정의대로 수행 → TB-agent 재-enroll 필요 (이슈 참조).

## 변경된 파일

- `cims.sh`: start_tb_csc / start_tb_console / start_tb_agent / _tb_issue_enrollment_token / _svc_port_proto 확장 / cmd_status 섹션 분리 / cmd_preflight TB 구분 / cmd_reset TB 유지 안내
- `configure.sh`: apply_csc_tb_overlay 추가, .env.tb.local 생성
- `csc/src/csc_app.py`: CIMS_CSC_CONFIG ENV override 한 줄
- `ems/core/console/.gitignore`: `.env.*.local` 추가
- `cims-phone/{vite.config.ts, nginx.conf}`: 3000 → 3002 이전
