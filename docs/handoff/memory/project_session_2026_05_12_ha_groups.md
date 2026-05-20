---
name: 2026-05-12-ha-console-ui
description: agent 운영 설정의 Console UI 통합 (HaGroupsPage) + ha_groups/ha_group_members DB + 모듈 ha_capability 메타 + cims_agent job_update_ha 자동 분배. 6 commits push.
metadata: 
  node_type: memory
  type: project
  originSessionId: 0f0f28b3-7806-4f46-bc85-efea242cdfbd
---

## 결과 (2026-05-12)

운영자가 Console `/deploy/ha-groups` 에서 노드 묶음을 그룹으로 정의 → CSC + cims_agent 가 자동으로 각 노드의 ha.json 분배 + cims-ha apply 트리거. **6 commits push (origin/main a1f6cd4)**.

LIVE pipeline-full **38 / PASS 34 / FAIL 0 / SKIP 4 / 270.5s** — 회기능 무영향.

## 사용자 확정 결정사항

| 항목 | 결정 |
|---|---|
| 그룹 단위 | 노드(agent) 묶음. 1 노드 = 1 그룹 (uk_agent UNIQUE) |
| 모드 | A/S (2 노드) + All Active (N 노드). N-cluster 는 향후 |
| ha_capability enum | `"active_standby"` \| `"all_active"` \| `"standalone"` |
| install 정책 | ha_group 정의 시 strict, 미정의 시 모두 허용 (워크플로 가이드) |
| VRID/VIP | VRID 자동 (51-255 range), VIP 수동 |
| UI 위치 | `/deploy/ha-groups` (deploy 섹션) |

## 모듈 ha_capability 매트릭스

- csp/psp/isp/csc → active_standby
- cmp/pmp/imp → all_active
- cwrtc/cspsim/agent/cims-console/cims-phone → standalone

각 모듈 pkg.json 에 `"ha_capability"` 필드. 변종 (psp/isp/pmp/imp) 는 base (csp/cmp) 의 pkg.json 따라가므로 자동 상속.

## 6 Commits

```
a1f6cd4 docs(ha): §11.6 Console HaGroupsPage flow + 자동 분배 흐름
37110ae fix(ha): _create_deployment 의 ha_capability 검증 — ha_group 정의 시만 strict
686f474 feat(ha): Stage 4 — cims_agent.py job_update_ha (자동 ha.json 분배)
3ffe06f feat(ha): Stage 3 — Console HaGroupsPage + routes
1af137d feat(ha): Stage 2 — CSC API handlers/ha_groups.py + agents.py mismatch 검증
5166ce7 feat(ha): Stage 1 — HA 그룹 DB schema + 모듈 ha_capability 메타
```

## 자동 분배 흐름

1. 운영자 Console `/deploy/ha-groups` 그룹 생성/멤버 추가/제거
2. CSC `handlers/ha_groups.py:_enqueue_update_ha_for_members` 가 멤버별 ha.json render → `agent_job` INSERT (job_type=update_ha)
3. cims_agent heartbeat → `job_update_ha`:
   - `install_path/agent/keepalived/ha.json` 갱신
   - `agent/bin/cims-ha config + apply` 자동 실행
   - sudo / keepalived 미준비 시 graceful skip (config 까지는 OK)

## 핵심 파일

### 신규
- `sql/migrate_ha_groups.sql` — DB schema (ha_groups + ha_group_members)
- `csc/src/handlers/ha_groups.py` — CSC API (CRUD + member + VRID 자동 + enqueue update_ha)
- `cims-console/src/api/ha_groups.ts` — Console API client
- `cims-console/src/pages/HaGroupsPage.tsx` — 카드 grid + 멤버 inline + 생성 모달

### 수정
- 각 base 모듈 `pkg.json` — ha_capability 필드 추가 (8 base)
- `cims.sh:cmd_pkg` — meta.json 에 ha_capability 자동 포함
- `csc/src/handlers/agents.py:_create_deployment` — mismatch 검증 (group 정의 시만)
- `csc/src/csc_app.py` — CIMS_HA_GROUPS_HANDLER_LIST 등록
- `cims-console/src/routes.tsx` — /deploy/ha-groups route
- `agent/cims_agent.py:execute_job` — update_ha job 추가 + `job_update_ha` 신규
- `docs/design/ha_design.md` — §11.6 Console flow 신규

## 미완 / 향후

- **ServersPage 의 agent 카드에 ha_group 표시** — 별도 commit
- **DeploymentCreateModal 의 ha_capability mismatch hint** — 별도 commit (backend 가 이미 400 reject 하므로 UI hint 만 추가)
- **services/ha_render.py 별도 분리** — 현재는 ha_groups.py inline (`_render_ha_for_agent`)
- **VRID exhaustion alert** — 51-255 = 205개 한계, 현재 graceful runtime error
- **HA fail-over LIVE 검증** — 2-node 환경 마련 후 (S6-SCN-FAILOVER-* stub 활성)

## 디버깅 메모

Stage 2 의 strict 검증 (ha_group 미정의 agent 에 active_standby/all_active 모듈 install 거부) 가 verify pipeline 의 stage5 (MODULES-DEPLOY) FAIL 유발. fix commit (37110ae) 에서 ha_group 정의 시만 strict 로 완화. 운영자 워크플로 (agent 등록 직후 그룹 정의 전 임시 install) 도 가능.
