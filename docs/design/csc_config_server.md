# CSC Config-Server — CSP 설정 단방향 흐름

> 버전: 1.0 (2026-05-18)
>
> CIMS 의 모든 CSP 설정 변경이 **Console → CSC file_store → agent →
> install_path → SIGUSR1** 단일 경로로 흐르도록 정착시키는 설계.
> 본 문서는 CSC 가 config-server 역할을 겸직하는 방식과 그 의도된
> 데이터 흐름을 명문화하여 후속 drift 를 감지 가능하게 한다.
>
> 트랙 plan: `~/.claude/plans/csp-bubbly-sketch.md` (Phase A).

## 1. 배경

2026-05-17 walkthrough 회기에서 다음이 노출되었다.

- `csp.json` (scalar Setup) + `config/*.jsonl` (9 collection) 두 source 가
  분산되어 있고, 운영자가 어느 한 쪽을 수동 편집했을 때 다른 쪽과의
  정합이 깨질 위험.
- HA 그룹 멤버 (예: csp@ctrl-a / csp@ctrl-b) 의 그룹 공통 설정
  (`access_services`, `routes`, role flags 등) 이 두 멤버에 일관되게
  적용되어야 하나, 멤버별 deployment.config 가 따로 저장되어 drift 발생.
- agent 의 `job_update_config` 가 `config.json` 만 쓰고 `SIGUSR1` 을
  안 보내, scalar 변경이 다음 재기동 전까지 반영되지 않는 gap.

본 설계의 목표:

1. CSC file_store 를 **단일 SoT** 로 명문화. 모든 설정 변경이 CSC 를
   경유. install_path 의 on-disk 파일은 running CSP 가 읽는 캐시.
2. HA 그룹의 **scope=service** collection 변경 시 모든 멤버에 자동 fan-out.
3. **`SIGUSR1` 발송이 scalar / collection 양쪽 PUT 모두에서 동작**하도록
   agent 의 한 줄 gap (Phase D) 을 닫음.

## 2. 데이터 모델

### 2.1 CSC file_store SoT

| 도메인 | 경로 | 용도 |
|---|---|---|
| `deployments/<id>.json` | scalar config + 메타 (`config_applied_at`, `install_path`, `package_id`) | `_put_deployment_config` 의 저장소. `csc/src/handlers/agents.py:1310`. |
| `packages/<key>.json` | `config_template` (scope 메타 포함) | UI 가 어떤 section/collection 을 보일지 결정. |
| `ha_groups/<id>.json` | `members[]` + `vip` + `vrid` | scope=service collection 변경 시 fan-out 대상. `csc/src/handlers/ha_groups.py:38`. |

### 2.2 collection 의 위치

JSONL collection 의 실제 record 는 CSC 자체에 저장되지 않고,
**agent 가 install_path/config/<name>.jsonl 로 보유**.
CSC 의 `_get/put_deployment_collection` (`csc/src/handlers/agents.py:1708,1737`)
은 agent 에 proxy 호출하여 read/write 한다. agent 의 endpoint 는
`/collection` (`agent/cims_agent.py:889-921`).

이 듀얼 구조 (CSC = scalar SoT, agent = collection on-disk) 는 의도된 것:
- collection 은 CSP 가 실제로 fopen 하는 파일 — agent local 이 자연스러움.
- scalar (csp.json) 도 install_path 에 존재하지만, 변경 의도는 CSC 에 박힘.

### 2.3 scope 메타 — A/S 일괄 vs 개별 spec

`csp/config/config_template.json` 의 각 section / collection 에
`"scope": "system" | "service"` 가 명시되어 있다. **이것이 SoT**.

**T4 (commit `9b5699b` 후속) — 의미 재정의**:

| scope | 의미 | 분배 결정 | UI 위치 |
|---|---|---|---|
| `service` | 그룹 공통 — 항상 양 멤버 동일 | 모든 mode 에서 fan-out | 그룹 카드 (GroupServiceConfigModal) |
| `system` | mode 따라 분배 — *멤버 분리가 필요할 때만* 분리 | active_standby → fan-out (서비스와 동일), all_active → 멤버별, standalone → 단일 | A/S 면 그룹 카드, AA 면 서버 카드 |

옛 정의 (~2026-05-18 이전): `system` = 무조건 멤버별 분리. A/S 모드에서도 멤버별
편집을 강제하여 split-brain config 위험. csc 의 `_put_deployment_collection`
(`csc/src/handlers/agents.py`) 가 새 정의로 자동 fan-out 결정.

UI 가 scope 메타로 자동 분류:
- `cims-console/src/components/group/GroupServiceConfigModal.tsx:60` —
  `scope === undefined || scope === 'service'` 만 표시.
- `cims-console/src/components/module/ModuleConfigModal.tsx` —
  scope=service collection 은 멤버 단일 모드에서 🔒.
- A/S 그룹 + scope=system 의 새 분류는 T3 에서 ha_group 단위 보기 토글로 노출.

## 3. Push 흐름

```
[Console]
   ↓ PUT /api/v1/deployments/{id}/config
   ↓ PUT /api/v1/deployments/{id}/collections/{name}
   ↓ PUT /api/v1/ha-groups/{id}/collections/{name}  ← Phase C 신규
[CSC]
   ↓ file_store.save (deployments/<id>.json)            // scalar 변경 시
   ↓ _job_create('update_config', params)              // scalar
   ↓ agent_proxy_call PUT /collection                   // collection 시 즉시 proxy
[agent]
   ↓ job_update_config → install_path/config.json
   ↓ _write_jsonl_atomic → install_path/config/<name>.jsonl
   ↓ _signal_process(install_path, 'usr1')              ★ Phase D 의 한 줄
[CSP]
   ↓ SIGUSR1 → g_reloadFlag = 1
   ↓ main loop → gclsSetup.Read() + ReloadFromJsonl() + 9 map Sync()
   ↓ ValidateRefs
```

## 4. API 스펙

### 4.1 기존 (변경 없음 또는 응답 보강)

| Endpoint | 동작 | 비고 |
|---|---|---|
| `GET /api/v1/deployments/{id}/config` | scalar config + template 반환 | Phase C 에서 `collections: {<name>: {records, schema}}` 통합 view 추가 (옵션 — 기존 응답 보존). |
| `PUT /api/v1/deployments/{id}/config` | scalar 저장 + update_config job 큐잉 | Phase D 이후 job stdout 의 `signaled` 가 채워짐. |
| `GET /api/v1/deployments/{id}/collections/{name}` | 해당 deployment + ha_group 멤버 records 비교. drift_detected / peers[] 포함. | T2 후속 — 옛 응답 (records/schema) 호환 유지. |
| `PUT /api/v1/deployments/{id}/collections/{name}` | records 저장 + ha_group fan-out (scope+mode 자동 결정). 응답에 sync_id / peers / propagated. body 의 `propagate_to_ha_peers` 로 override 가능. | T1 후속 — `agents.py:_put_deployment_collection`. |

### 4.2 신규 (Phase C)

| Endpoint | 동작 |
|---|---|
| `GET /api/v1/ha-groups/{id}/collections/{name}` | 그룹 멤버의 첫 deployment 에서 records fetch (정합 가정). schema 도 함께 반환. |
| `PUT /api/v1/ha-groups/{id}/collections/{name}` | 모든 멤버의 해당 패키지 deployment 에 fan-out PUT. per-member status array (`results: [{deployment_id, count, signaled, error?}]`) 반환. |

요청 body 는 deployment 버전과 동일 (`{"records": [...]}`).
응답 형식:
```json
{
  "ok": true,
  "members": [
    {"deployment_id": 36, "count": 3, "signaled": [12345]},
    {"deployment_id": 37, "count": 3, "signaled": [12346]}
  ]
}
```

## 5. 부트스트랩

agent 가 CSC URL 을 아는 경로는 변경 없음 — `install-agent.sh` 가 enrollment
시 `CSC_URL` 을 inject (현 패턴). dev (NetNS) 와 prod (`/opt/cims`) 모두
이 경로가 동작하는 것은 walkthrough 단계 1~4 에서 검증됨.

## 6. Fallback

config-server (= CSC) 가 다운된 시점의 모듈 동작:

```
[CSC down]
   ↓
[CSP 재기동 발생] (운영자 수동 또는 OS reboot)
   ↓
agent 가 install_path/config.json 과 install_path/config/*.jsonl 그대로 사용
   → CSP 정상 기동, 기존 설정으로 운영 (변경 못 받음)
[CSC 복구]
   ↓
agent heartbeat 가 다음 pending job 받음 → 변경 일괄 적용
```

핵심: agent 가 install_path 의 마지막 config 를 그대로 운영 캐시로 들고 있어,
config-server 가 잠시 다운돼도 CSP 기동 / 운영 자체는 무중단. retry 는
agent 의 기존 heartbeat 루프가 자연스럽게 수행 — 추가 코드 불필요.

## 7. 마이그레이션

### 7.1 deprecate

- `csp.json` 직접 편집 (e.g. vi) → deprecated. 변경은 모두 Console 또는
  CSC API 경유. 운영 매뉴얼 (`docs/user-manual/deployment_workflow.md`) 에 명기.
- 옛 `agent_deployment` DB 테이블 — 이미 file_store 로 이전됨 (2026-05-13).

### 7.2 유지

- `install_path/csp.json`, `install_path/config/*.jsonl` — running CSP 가 읽는
  on-disk 파일. agent 가 PUT 받을 때마다 갱신.
- `deployment/bin/render.py` (env+scenario → bundle 생성) — 초기 설치 시
  bundle 생산용. 운영 중 PUT 은 render.py 미경유.

### 7.3 새 PR 의 schema 검증

Phase B 에서 `csc/src/handlers/agents.py` 의 `_collection_schema` 가
패키지 업로드 시 `scope` 누락을 경고. 1 릴리스 후 fatal 로 승격.

## 8. 위험 / 알려진 한계

- **Bootstrap 필드 hot-reload 불가**: `Setup.Sip.UdpThreadCount`,
  `Setup.Sip.LocalIp` 등은 SIGUSR1 후에도 이미 bound 된 socket / thread pool
  에 반영되지 않음. UI 에서 "재기동 필요" 명시 (Phase F).
- **Stale pid**: CSP 가 crash 후 수동 재기동 시 pid 파일이 defunct PID
  가리킬 수 있음. `_signal_process` 의 except 블록이 swallow (`agent/cims_agent.py:846-847`).
- **Multi-pkg agent (mgmt-server: csc+console+phone)**: pid 파일이
  `install_path/<pkg>/run/<pkg>.pid` 에 있음. `_signal_process` 가 인자
  `pkg_subdir` 받아 우선 탐색 (Phase D).
- **DB scalar 재로드**: `gclsSetup.Read()` 는 csp.json 재파싱하지만
  `DbManager` 가 connection pool 을 재초기화하지 않음. DB 설정 변경은
  CSP 재기동 필요 — 알려진 한계.

## 9. 코드 참조

| 책임 | 파일 | 라인 |
|---|---|---|
| CSP reload chain | `csp/CspServer.cpp` | 25-29 / 277-279 / 336-354 |
| JSONL loader | `csp/CspConfigCache.cpp` | 50-106 |
| scope 메타 (SoT) | `csp/config/config_template.json` | (전체) |
| scalar PUT | `csc/src/handlers/agents.py` (`_put_deployment_config`) | 1298-1333 |
| collection PUT | `csc/src/handlers/agents.py` (`_put_deployment_collection`) | 1737-1790 |
| HA group dispatch | `csc/src/handlers/ha_groups.py` (`handle_ha_groups`) | 246-300 |
| agent `/collection` | `agent/cims_agent.py` | 889-921 |
| `_signal_process` | `agent/cims_agent.py` | 831-848 |
| `job_update_config` (gap) | `agent/cims_agent.py` | 498-507 |
| group fan-out 클라이언트 | `cims-console/src/components/module/ModuleConfigEditor.tsx` | 88-107 |
| GroupServiceConfigModal | `cims-console/src/components/group/GroupServiceConfigModal.tsx` | (전체) |

## 10. 후속 (범위 밖 — Phase G)

- **CMP** — `cmp.json` 의 hot-reload. CMP 코드에 SIGUSR1 reload 로직
  신설 필요. 본 트랙의 CSP 패턴을 복제.
- **CSC 자체** — `csc.json` 의 hot-reload. 현재 재기동 필요.
- **cwrtc / phone** — 동일 패턴 복제. 우선순위 낮음.
