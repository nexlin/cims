---
name: 2026-05-08 naming refactor — mgmt-server + 4 service-server (commit 11a58b0)
description: 토폴로지 server name 정립 — agent name + dist 디렉토리를 server-level 명명으로 재구성. 사용자가 다음 세션에서 적용 결과 확인 예정.
type: project
originSessionId: be82a7dc-18c2-4a0c-acb2-72d9bf3fd592
---
# 2026-05-08 naming refactor — committed 11a58b0 (push 완료)

**시작 base**: 390cda1. **commit 11a58b0**: 16 파일, 345 insertions / 243 deletions.

## 변경 매핑 (사용자 컨펌)

| 이전 agent name | display name (UI) | dir/agent name |
|---|---|---|
| csc-server-local | CIMS 관리 서버 | `mgmt-server` |
| csp-server-local | VoLTE SIP Server | `volte-sip-server` |
| cmp-server-local | VoLTE Media Server | `volte-media-server` |
| psp-server-local | PTT SIP Server | `ptt-sip-server` |
| pmp-server-local | PTT Media Server | `ptt-media-server` |
| sim-server-local | (mgmt-server 흡수) | `dist/mgmt-server/sim/` |

**사용자 의도**: simulator/cwrtc/phone 은 임시 테스트 모듈 — mgmt-server 에 같이 install 후 테스트 끝나면 삭제. 그래서 sim 을 별도 agent 가 아닌 mgmt-server agent (TB-CSC chain) 가 처리.

## 핵심 변경

### `_INSTANCES` (4 entry)
- 각 entry 에 `display_name` + `agent_name` 필드 추가.
- sim entry 제거.
- helper `_install_root(dist, inst)` / `_install_path(dist, inst)` 도입 — `dist/<agent_name>/<dir>/`.
- `_AGENT_NAME_MOD` 호환 dict 추가.

### `_CSC_PACKAGES` 에 sim 추가
- `("csc", "console", "sim")` — TB-CSC chain 이 mgmt-server agent 로 3 모듈 모두 install.
- `_CSC_PKG_TARBALL` (sim → cspsim), `_CSC_PKG_PROCESS` (sim → CSPSIM) 매핑.
- step_08 / step_09 / step_10 / step_11 모두 _CSC_PACKAGES iterate (자동 일반화).
- step_11 의 sim: cspsim tarball 구조상 config/ 없을 수 있어 meta.json 만 검사.

### step_17~22 단순화
- 4 service-server 만 처리 (sim 제외, _MODULES = 4).
- step_18 _all_modules_online: `inst["agent_name"]` 사용.
- step_22 finalize:
  - keep-running: instance 별 host:port 표기 (lines from _INSTANCES.listen).
  - --stop-after: stop 7 (3 mgmt + 4 service), kill 5 agents.

### cims.sh cmd_reset
- 5 agent dir + pkill 패턴 통일 (mgmt-server + 4 service).
- pkill -f `cims_agent.py.*--name $AGENT` (각 agent name).

### stage6
- summary.py: log_paths = `_INSTANCES` 의 `dist/<agent_name>/<dir>/<dir>/log/<dir>_*.log` glob.
- seed.py / scn_db_sync.py: install_path = `inst["agent_name"]/<dir>` (하드코딩 `f"{inst['id']}-server"` 제거).
- scn_cert_rotate.py: agent_name = "mgmt-server", state_crt path 도 `dist/mgmt-server/agent/state/`.

### UI display
- 신규 `ems/core/console/src/components/agentDisplay.ts` — kebab → "VoLTE SIP Server" 매핑.
- ServersPage / PackagesPage: `agentDisplayName(agent_name)` 표시 + 괄호 안 raw kebab.

## 검증 결과

- ✓ unit test **161/161 PASS** (0.6s)
- ✓ cims.sh syntax OK
- ✓ TypeScript typecheck (ems/core/console) OK
- ✓ stage1 verify **5/5 PASS** (11.1s)

stale reference 0 (`csc-server-local` 등 패턴 모두 제거됨).

## 다음 세션 의제 — 적용 결과 확인 + 잔여 작업

### 우선 — 사용자 확인
사용자가 LIVE pipeline 한 회차 돌려서 server name 이 의도대로 나오는지 검증 예정:
- ServersPage UI: "VoLTE SIP Server (volte-sip-server)" 형식 노출
- dist 디렉토리: `build/dist/mgmt-server/`, `build/dist/volte-sip-server/` 등 생성
- DB cims_agent.name: 5 새 이름으로 enroll
- pkg / install_path / log path 모두 정상 동작

### 회귀 위험 (LIVE 시 점검)
1. **DB cleanup**: 직전 회차의 `csc-server-local` 등 옛 이름 row 가 cims_agent 에 잔존 가능. cmd_reset 이 `pkill -f` 만 하고 DB row 는 안 지우므로 — 회차 시작 시 옛 이름 + 새 이름 double row 가능. step_06 의 register 가 새 이름으로 신규 row 생성 → 옛 row 는 무관 (offline). 단, 누적은 됨.
2. **dist 디렉토리 경로 변경**: `build/dist/csc-server/` 등 옛 디렉토리는 cmd_reset 이 새 이름만 wipe — `--keep-deployed` 가 아닌 일반 reset 에서도 옛 디렉토리는 살아남을 수 있음. 사용자가 직접 `rm -rf build/dist/{csc,csp,cmp,sim,psp,pmp}-server/` 1회 정리 필요.
3. **MTLS cert path**: scn_cert_rotate 가 `dist/mgmt-server/agent/state/agent_mtls.crt` 검색 — 회차 1번째 reset 후 정상 발급되는지 확인.
4. **cims.sh sync scripts**: dist 안 cims.sh 가 새 cmd_reset 갖는지 확인 (`./cims.sh sync scripts`).

### 후속 작업 후보
1. **환경 변동성 안정화** (이전 의제) — 좀비 처리, _kill_own_install_listener 일반화. 18회차 1 FAIL 의 본질.
2. **P2 — ISP/IMP 활성** — 패키지 9종 빌드 완료, S5 배포/S6 시나리오 미적용. _INSTANCES 에 isp/imp entry 추가.
3. **mgmt-server 흡수 모듈 확장** — cwrtc/phone 도 _CSC_PACKAGES 추가하면 4 → 6 모듈. 사용자 의도 ("임시 테스트 모듈") 와 일치하지만 P1 범위 밖.

## LIVE 검증 절차 (다음 세션)
```bash
cd /home/nex/work/cims
git log -3 --oneline      # 11a58b0 가 최신
python3 -m unittest tests.test_verify_lib  # 161 OK
ss -ulnp 2>/dev/null | grep -E ":(5060|9000|4421)" | head

# 옛 dist 디렉토리 정리 (1회만)
rm -rf build/dist/{csc,csp,cmp,sim,psp,pmp}-server 2>/dev/null

./configure.sh --psp-ip 127.0.0.3 --pmp-ip 127.0.0.3
./cims.sh sync scripts csc
./cims.sh restart tb-csc cmp
./cims.sh verify run --preset pipeline-full --enable-mtls
```

기대: 새 이름으로 인스턴스 생성 + UI 에 display name 노출 + 32/1/1 PASS 베이스라인 유지.
