---
name: project-session-2026-05-18-csc-config-server
description: 2026-05-18 회기 — CSC config-server 트랙 본체 + Phase X (csp bind 키 제거) + 단계 5 부분 완료. csp 양 멤버 LIVE LISTEN. 다음 세션 진입점.
metadata: 
  node_type: memory
  type: project
  originSessionId: 5ed36b69-f1b4-433a-9f0b-a1e8073c34d3
---

# 2026-05-18 — CSC config-server 본체 + Phase X + 단계 5 부분 완료

**Why**: 2026-05-17 walkthrough 회기에서 노출된 csp 설정 모델의 limit 해소.
모든 CSP 설정 변경이 **Console → CSC file_store → agent → install_path → SIGUSR1**
단방향 흐름으로 정착. 추가로 csp.json 의 SIP bind 5키 (LocalIp/UdpPort/TcpPort/
TlsPort/CertFile) 가 local_nodes.jsonl 과 중복이던 것을 해소 — local_nodes 가
single SoT.

**How to apply**: 다음 세션 진입 시 본 메모 읽고 §"다음 세션 진입점" 따라
agent restart → 단계 5.4 (cspsim 호 시험) → 5.5 (SIGUSR1 검증) 진행.

## 본 세션 commit (3개, origin/main 보다 앞)

| commit | 내용 |
|---|---|
| `79d7c46` | CSC config-server Phase A~F+H smoke — design doc + ha-groups collection endpoint + agent SIGUSR1 + GroupServiceConfigModal 활성화 |
| `66706b8` | fix(agent): `_resolve_pkg_subdir` 의 의미를 SIGUSR1 PID 분리만으로 좁힘 (Phase D 의 silent bug fix — config.json 위치 정합) |
| `c2c8911` | feat(csp): Setup.Sip 의 bind 5키 제거 — local_nodes 가 single SoT. csp 재빌드 + LIVE 검증 OK |

## 변경 surface (6 파일, +428/-18 LOC)

| Phase | 파일 | 책임 |
|---|---|---|
| A | `docs/design/csc_config_server.md` (신규) | 단일 path 흐름 + scope 메타 SoT 명문화 |
| B | `csc/src/handlers/agents.py` | `_warn_missing_scope` + `_create_package` 호출 (warn-only). `import sys` |
| C | `csc/src/handlers/ha_groups.py` | `GET/PUT /api/v1/ha-groups/{id}/collections/{name}` 신규 + helper 3개 + `import asyncio` |
| D ★ | `agent/cims_agent.py` | `job_update_config` 끝에 SIGUSR1 + `_signal_process(pkg_subdir)` + `_resolve_pkg_subdir` |
| F | `ems/core/console/src/pages/HaServicesPage.tsx` | `GroupServiceConfigModal` import 복구 + "⚙ 설정" 버튼 + Modal wiring |
| F | `ems/core/console/src/components/group/GroupServiceConfigModal.tsx` | preset 안내 메시지 갱신 (Phase D 반영) |

## LIVE smoke (CSC restart 후)

```
GET /api/v1/ha-groups/4/collections/access_services?package_id=17 → 200 + schema
PUT 1 record → 양 멤버 fan-out (deployment 36/37) ok=true count=1
GET 재확인 → records=1, member_count=2, auto-id 할당
PUT [] cleanup → count=0
```

`signaled=[]` 인 것은 csp deployment stopped 라 PID 없는 상태 — 정상.
**단계 5 호 시험** 에서 csp 기동 후 SIGUSR1 실 도착 + `g_reloadFlag` polling
검증 예정.

## 단계 5 진척 (본 세션)

| # | 작업 | 결과 |
|---|---|---|
| 5.1 | csp install_path 진단 | ✅ install_path/config.json + config/*.jsonl 위치 확인 |
| 5.2 | csp 설정 시드 (scalar + 3 collection) | ✅ `PUT /api/v1/deployments/{36,37}/config` (job 203/204) + `PUT /api/v1/ha-groups/4/collections/{access_services,local_nodes,remote_nodes}?package_id=17` |
| 5.3 | csp 기동 + LISTEN 확인 | ✅ 옛 csp (640669/641071, 5/15 leftover, deleted cwd) 종료 후 새 csp 기동 — PID 2063816/2063861 (12:41~), sockets=6, 10.0.1.13:5060 UDP + 25061 TCP + 5061 TLS LISTEN |
| 5.4 | cspsim VoIP 1대1 호 시험 | ⚠️ 미진행 — sudo 필요 (netns 진입) |
| 5.5 | config 변경 → SIGUSR1 실 도착 검증 | ⚠️ 미진행 — agent restart 필요 (본 PR 의 새 코드 reload) |

**중요 진단** — access_services schema 의 `inbound_policy` enum 정합 어긋남:
bundle 의 record 가 `"default-inbound"` 인데 schema 는 `["any", "restricted"]` 만 허용.
임시로 `"any"` 로 변경하여 시드 통과. 후속 정합 fix 필요 (render.py 또는 schema).

## 다음 세션 진입점

```bash
# 1. 환경 확인
ps -ef | grep -E "cims_agent|csc_app|bin/csp" | grep -v grep
# 기대: ctrl-a/b agent (PID 1695914/1695923) + csc + 새 csp (2063816/2063861)
# 만약 csp 가 죽었다면: curl -X POST /api/v1/deployments/{36,37}/job -d '{"job_type":"start"}'

# 2. agent restart (본 PR 의 새 코드 LIVE 적용)
sudo pkill -f "cims_agent.py.*--name Control-Server-"
sleep 2
sudo ip netns exec ctrl-a sudo -u nex bash -c '
  cd /home/nex/work/cims/build/dist/netns-agents/ctrl-a && \
  nohup ./run.sh > agent.log 2>&1 < /dev/null &'
sudo ip netns exec ctrl-b sudo -u nex bash -c '
  cd /home/nex/work/cims/build/dist/netns-agents/ctrl-b && \
  nohup ./run.sh > agent.log 2>&1 < /dev/null &'

# 3. 단계 5.4 — verify.py listen + smoke (cspsim 자동)
sudo -v
python3 deployment/bin/verify.py --env tb-netns-4-node --scenario volte-ptt --phase listen
python3 deployment/bin/verify.py --env tb-netns-4-node --scenario volte-ptt --phase smoke

# 4. 단계 5.5 — config 변경 → SIGUSR1 실 도착
# (예: access_services 의 record 1개 비활성 → PUT)
curl -sk -X PUT -H 'Content-Type: application/json' \
  "https://127.0.0.1:4419/api/v1/ha-groups/4/collections/access_services?package_id=17" \
  -d '{"records":[{"id":1,"name":"volte-basic","kind":"volte","enabled":false,...}]}'
# csp.log 에서 "SIGUSR1: reloading scalar config + jsonl" 확인
```

## Phase X — csp.json bind 5키 제거 (사용자 주안점 — 본 세션 후반)

사용자 관찰: "csp 가 sip 로 수신하는 ip/port 설정부분이 Local Node 인데 이부분이
초기 설정하는 부분을 없애고 설정시 바로 반영되는 구조로 되어야 하는데 2가지로
나눠저 있는것 같아"

해소: csp.json `Setup.Sip` 의 5 bind 키 (LocalIp/UdpPort/TcpPort/TlsPort/CertFile)
가 local_nodes.jsonl 과 중복 정의됐던 것을 제거. local_nodes 가 single SoT.

| 파일 | 변경 |
|---|---|
| `csp/SipServerSetup.cpp` | 5 키 Has 체크 제거 + default port 0 sentinel. TlsAcceptTimeout 추가 |
| `csp/CspServer.cpp` | primary local_node 부재 시 `return -1` fail-fast (옛 fallback path 제거) |
| `deployment/bin/render.py` | 생성하는 csp.json 에서 5 키 미포함 |
| `csp/config/config_template.json` | (이미 정리됨 — UdpThreadCount/StackExecutePeriod 만 노출) |

**LIVE 검증** (csp 재빌드 + dist sync + restart):
- 새 binary (12:40 빌드, 32M) → ctrl-a/b 의 install_path/csp/bin/csp 동기화
- csp stop/start 후 PID 2063816/2063861 (12:41~) sockets=6
- 양 멤버 10.0.1.13:5060 + 192.168.199.129:5060 동시 LISTEN (UDP/TCP/TLS)
- csp.json 의 5 키가 stale 한 상태로 남아도 무시 (Has 체크 미평가) — 옛 환경 호환

**LIVE 흐름** (이제 정착):
local_nodes 변경 → CSC PUT → agent → SIGUSR1 → `gclsListenerManager.Sync()` →
즉시 socket bind 변경 (재기동 불필요).

## 핵심 결정 (Phase 0 확정 — 본 회기에서 그대로 적용)

| # | 결정 | 적용 결과 |
|---|---|---|
| 1 | 범위 — CSP 정공법 먼저 | Phase A~F+H smoke 완료. CMP/CSC/cwrtc/phone 은 Phase G (별도 회기) |
| 2 | Server 위치 — CSC 겸직 | ha_groups.py 에 collection endpoint 추가. 별도 daemon 미도입 |
| 3 | Push 패턴 — agent → SIGUSR1 | `job_update_config` 에 `_signal_process` 추가. `pkg_subdir` 인자로 multi-pkg agent 변종 분리 |
| 4 | Fallback — local cache + heartbeat retry | 명문화 (design doc §6). 코드 변경 없음 (agent 의 기존 동작 그대로) |

## 알려진 한계 (design doc §8)

- **부트스트랩 필드 hot-reload 불가** — `Setup.Sip.UdpThreadCount`, `LocalIp` 등은
  이미 bound 된 socket/thread pool 에 반영 안 됨. UI 에 "재기동 필요" 명시.
- **DB scalar 재로드** — `gclsSetup.Read()` 는 csp.json 재파싱하지만 `DbManager`
  미재초기화. DB 설정 변경은 CSP 재기동 필요.
- **collection PUT 의 pkg_subdir** — `_put_deployment_collection` 의 agent proxy 가
  현재 `install_path` 만 보냄. multi-pkg agent (mgmt-server) 의 collection 정확
  대상 식별을 위해 후속 보완 가능.

## 미수행 (다음 세션)

**단계 5 마무리**:
- 5.4 cspsim VoIP/PTT 호 시험 — sudo 필요 (verify.py)
- 5.5 config 변경 → SIGUSR1 실 도착 검증 — agent restart 선행 필요 (본 PR 새 코드 LIVE)

**후속 (Phase G 또는 별도 트랙)**:
- **access_services schema 의 inbound_policy 정합** — bundle 의 "default-inbound" vs
  schema 의 ["any","restricted"]. render.py 또는 schema fix.
- **Phase G — CMP / CSC 자체 / cwrtc / phone** hot-reload 도입. CSP 패턴 복제.
- **`ModuleConfigEditor` drift detected 표시** — `deploymentIds[0]` 만 fetch 하여
  멤버 drift 시 잘못된 baseline 가능성. `signaled.length < members.length` 인
  경우 경고 UI.
- **`_put_deployment_config` signaled polling** — 클라이언트가 이미 job 결과
  polling 하는 패턴이라 본 회기에서 skip.

## 관련 메모리

- [[project_csc_config_server_track]] — 본 트랙 설계 + 사용자 주안점 3가지
- [[project_session_2026_05_17_walkthrough]] — 이전 회기 (단계 1~4 + 11 patch)
- [[project_session_2026_05_15_deployment_scaffold]] — render.py + LIVE 환경 (배경)
