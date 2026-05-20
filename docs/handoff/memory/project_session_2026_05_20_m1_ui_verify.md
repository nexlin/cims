---
name: project_session_2026_05_20_m1_ui_verify
description: 2026-05-20 — M1 Console UI 검증 (HaServicesPage / drift 배너 / A/S system scope 통합 편집) API+코드 레벨 LIVE PASS. 회기 중 csc 다운 복구 (CIMS_CSC_CONFIG=csc-tb.json 으로 직접 spawn). 코드 변경 없음.
metadata: 
  node_type: memory
  type: project
  originSessionId: 651104f7-1b02-477a-8ff9-416e86932311
---

# 2026-05-20 — M1 Console UI 검증

[[project_session_2026_05_19_l1_l6]] 후속. "M1 Console UI 검증 진행" 지시.

## 환경 상태 (시작 시점)

- csc 4419: **DOWN** (이유 불명, 5/19 세션 끝에 재기동 안 된 듯)
- vite 3000: UP
- csp ctrl-a/b PID 2270316/2270317 (092047d) — 그대로
- cmp media-a/b PID 640474/640475 — 그대로

**csc 재기동 방법** (cims.sh start 는 agent/bin 으로 이전됨, TB-CSC 직접 spawn):
```bash
cd /home/nex/work/cims/build/dist/csc/src
CIMS_CSC_CONFIG=/home/nex/work/cims/build/dist/csc/config/csc-tb.json \
  nohup python3 csc_app.py > /tmp/csc-tb.log 2>&1 &
```
HTTPS 4419 — `https://127.0.0.1:4419/api/v1/...` (curl `-sk`). `/api/v1/health` 는 없음.

## 검증 4 시나리오 — 모두 LIVE PASS

### S1. HaServicesPage 데이터 매핑

- `GET /api/v1/ha-groups` → 2 그룹 (gid=4 Control-Server A/S, gid=5 Media-Server AA)
- `GET /api/v1/agents` → 4 agents (ctrl-a/b online, media-a/b approved-not-online)
- `GET /api/v1/deployments` → 2 deployments (dep 36/37 csp v0.0.2 on ctrl-a/b)
- HaServicesPage.tsx 의 services 빌드 로직 (line 338-383): ha_group → member agents → deployment.agent_id 매칭 → package_id 추출
- Control-Server gid=4 카드: csp dep 36/37, packageIds=[17]
- Media-Server gid=5 카드: deployment 없음, packageIds=[] (cmp media-a/b 가 csc 에 deployment 미등록 = **별개 이슈**, M1 범주 외)

### S2. drift 배너 — GET 응답 정합

`GET /api/v1/deployments/36/collection/local_nodes` 응답에:
- `drift_detected: boolean`
- `peers: [{deployment_id, agent_id, status, ok, count, hash, error}]`
- `ha_group_mode`, `scope`, `ha_group_id`

ModuleConfigEditor.tsx line 217-238:
- `drift.detected=true` → 노란 배너
- `drift.detected=false && peers.length>1` → 초록 한 줄 "✓ HA 그룹 멤버 정합"

LIVE drift inject (`ctrl-b/.../local_nodes.jsonl` 에 row 추가) → GET 응답 drift_detected=true, dep#36 count=3 hash=465098, dep#37 count=4 hash=ea8ee7 — UI 노란 배너 트리거 조건 만족 ✓

### S3. drift sweeper API

`GET /api/v1/csp/drift?drift_only=1`:
- 9 컬렉션 scan, drift 컬렉션만 반환
- items[].drift, base_hash, members[] (deployment_id, count, hash)
- drift inject → count=1/total=9 / drift 해소 후 count=0/total=9

### S4. A/S system scope 통합 편집 (핵심 시나리오)

PUT round-trip — drift 가 있는 상태에서:
```
PUT /api/v1/deployments/36/collection/local_nodes?propagate=true
body: {"records": [...]}   # master records (3건)
```
응답:
- `ok=true count=3 signaled=[2270316] propagated=true sync_id=22`
- `peers=[{dep#36 200 ok signaled=[2270316]}, {dep#37 200 ok signaled=[2270317]}]`
- `ha_group_mode=active_standby scope=system`

후속 검증:
- `GET /api/v1/csp/sync/22` → status=success, 양 멤버 ack
- `GET /api/v1/csp/drift?drift_only=1` → count=0 (drift 해소)
- ctrl-b 의 local_nodes.jsonl 4줄→3줄 (drift row 자동 제거)

## 사용자 브라우저 클릭 시 기대 동작 (참고)

`https://10.0.0.1:3000` (Vite) → `/deploy/services` (HaServicesPage):
1. Control-Server 그룹 카드 (vrid=51, A/S 10.0.1.15) + Media-Server 카드 (AA, 빈 패키지)
2. Control-Server 카드 클릭 → 모듈 설정 모달 → local_nodes 컬렉션 선택
3. **초록 한 줄** "✓ HA 그룹 멤버 정합 (mode=active_standby, 2 멤버)" 표시
4. row 편집 후 [저장] → 양 csp 멤버 signaled, 토스트 "(3개, signal: 2270316,2270317)"
5. (시연용) drift inject 상태로 모달 열면 **노란 배너** "⚠️ HA 그룹 멤버 간 정합 불일치 — ... (dep#36: 3건 (465098) / dep#37: 4건 (ea8ee7))" 표시

## 새로 안 사실

- `cims.sh start/stop` 은 더 이상 안 됨 (agent/bin/cims-svc 로 이전). **TB-CSC 는 어디로 갔는지 명시된 곳 없음** → 직접 `python3 csc_app.py` 으로 spawn 해야 함. **다음 세션 시작 전에 csc 살아있는지 확인 필수** (HTTPS 4419 not HTTP).
- 4 시나리오 모두 API + 코드 정합 PASS — UI 브라우저 검증 시 동일 흐름 보일 것

## M1.1 — TB-CSC 시작 절차 표준화 (같은 세션, 후속 작업)

`cims.sh tb <action> [target]` 명령 신설. cims.sh 만 수정 (코드 1곳, 무빌드 무재기동).

- `tb start|stop|restart [csc|console|all]` (기본 all)
- `tb status` — 4419/3000 port LISTEN PID 확인
- `tb help` — 사용법

설계 포인트:
- PID 추적은 port → ss → pid 방식 (PID 파일은 보조용). vite 가 npm 자식이라 PID 파일 1개로는 부족.
- TB-CSC = `cd $DIST_DIR/csc/src && CIMS_CSC_CONFIG=$DIST_DIR/csc/config/csc-tb.json nohup python3 csc_app.py`
- TB-Console = `cd $SRC_CONSOLE && npm run dev -- --mode tb --port 3000`. dist 트리에서는 비활성 (`SRC_CONSOLE` 비어있음)
- vite listen 까지 첫 빌드 수초 → 최대 15s polling
- console stop 시 npm pid `pkill -P` + vite pid 둘 다 kill (npm/vite 분리 대비)
- 옛 `cims.sh start` 에러 메시지에 "TB 는 `cims.sh tb start` 사용" 안내 추가
- preflight 의 옛 안내 `cims.sh start tb` → `cims.sh tb start` 갱신

LIVE 검증 (csc round-trip):
- status → stop csc → status → start csc → status → API probe (services 2건) → restart csc → idempotency → invalid args
- 모두 PASS. console 은 사용자 작업 중일 수 있어 건드리지 않음 (이미 LIVE pid=4773)

다음 세션 진입 시 csc 다운이면: `./cims.sh tb start csc`

## M1.2 — ServiceIp [적용] 동기화 (큐잉 → 즉시 응답)

commit `8fb6f55`. 사용자가 "큐잉됐다고만 뜨고 결과 안 보임" 지적 → 옛 흐름 완전 폐기.

옛: UI → CSC → file_store job 큐잉 → 응답 (202 job_id) → UI 30s polling
- 첫 toast "큐잉 처리 중" 2초 → 사라짐 → 결과 toast 가 5~15s 후 (heartbeat pickup + 실행) → 사용자가 시선 옮긴 사이 못 봄
- agent offline 시 30s timeout 후 fail

새: UI → CSC → agent `/apply-ip-config` (sync REST HTTPS) → `ip addr add` → 즉시 응답
- agent 가 이미 sync REST 서버 띄우고 있음 (port 9900). `do_POST` 에 `/apply-ip-config` 라우트만 추가 (기존 `job_apply_ip_config` 함수 그대로 호출)
- CSC `_apply_ip_config` 본체를 `_agent_proxy_call("POST", ..., "/apply-ip-config", body=...)` 로 교체. 폴백 없음, 단절 시 502 agent_unreachable
- frontend: polling 제거, `BindingStatus` 에 `applying`/`fail` 추가 → 행 인라인 ⏳ 스피너
- 응답 시간: 5~15s → 1초 미만 (LIVE round-trip 32ms 측정)

**중요 정책 결정 (사용자)**:
- agent 다운 시 폴백 없이 그냥 실패 처리 (간결)
- 대부분 명령 1초 이내 — timeout 짧게 (csc→agent 15s)

**미완 단계 (사용자 LIVE)**:
- agent 재기동만 사용자 몫. `build/dist/netns-agents/{ctrl-a,ctrl-b}/agent/cims_agent.py` 는 cp 로 갱신해 둠. PID 1695914/1695923 가 옛 코드 도는 중.
- 재기동 cheat sheet:
  ```bash
  sudo pkill -f 'cims_agent.py.*Control-Server-0[12]'
  sudo ip netns exec ctrl-a bash -c 'cd /home/nex/work/cims/build/dist/netns-agents/ctrl-a && nohup ./run.sh > agent.log 2>&1 &'
  sudo ip netns exec ctrl-b bash -c 'cd /home/nex/work/cims/build/dist/netns-agents/ctrl-b && nohup ./run.sh > agent.log 2>&1 &'
  ```
- 재기동 후 UI 의 [적용] 버튼이 즉시 결과 표시되어야 함. 옛 agent 일 때는 32ms 만에 "agent_error / not_found" 메시지 표시 (옛 30s timeout 보다 큰 UX 개선).

## 다음 진입 후보

| 후보 | 작업 | 상태 |
|---|---|---|
| **M1-UI** | 사용자 브라우저 검증 + agent 재기동 후 진짜 ip-add LIVE | 사용자 몫 |
| (옛 동일 큐잉 흐름) | VipPanel [적용] (`/agents/{id}/apply-ha`) — keepalived reload | 큐잉 방식 — 별도 회기 |
| M2 | L6 cspsim 실 호 + sip.jsonl tx 검증 (sudo netns 필요) | 사용자 LIVE |
| M3 | `AutoResyncDrift` default true 검토 | 운영 데이터 후 |
| M4 | `csp_runtime.py` 파일 + migrate 스크립트 완전 삭제 | 1 릴리스 안정화 후 |
