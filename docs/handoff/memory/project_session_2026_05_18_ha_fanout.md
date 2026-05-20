---
name: project_session_2026_05_18_ha_fanout
description: 2026-05-18 오후 — HA fan-out 절차 정상화. csc 컬렉션 변경이 ha_group 멤버 양쪽에 자동 동기화. T1~T5 트랙 완료. commits 9b5699b → 2cfd794 → cc3ed63.
metadata: 
  node_type: memory
  type: project
  originSessionId: b850a9f4-e5da-4247-86ce-ec5451c0e8ed
---

# HA fan-out 절차 정상화 (T1~T5) — 2026-05-18 오후 세션

## 사용자 의심의 출발

오전 SIP outbound 트랙 후 사용자 질문 "Local Node 설정 도 A/S 인 경우 한번에 설정해야". 그 후 Console UI 확인하니 여전히 멤버별로 보임.

## 추적 결과 (commit 전 분석)

csc 에 **세 가지** 컬렉션 편집 API 가 공존:

| API | 위치 | 데이터 | UI 사용 |
|---|---|---|---|
| `/api/v1/csp/listeners` 등 5 | `csp_runtime.py` (file_store) | csc 마스터 file_store | ❌ UI 가 안 부름 |
| `/api/v1/deployments/<did>/collection/<name>` | `agents.py:_put_deployment_collection` | agent install_path/config/\<name\>.jsonl | ✅ **진짜 사용 경로** |
| `/api/v1/modules/<name>/collection/<key>` | `modules.py` | csc 호스트 로컬 dist | ⚠️ Phase 1 (DEV) |

→ commit `9b5699b` 의 fan-out 은 (1) 에만 들어갔는데 UI 는 (2) 만 부름. 따라서 효과 안 보이던 게 사용자 직감의 정체.

## 적용된 트랙 (T1~T5)

### T1 — `_put_deployment_collection` 의 HA fan-out (`2cfd794`)
- ha_group 의 모든 csp deployment 에 동시 PUT (asyncio.gather)
- propagate 결정: scope=service or (scope=system + active_standby) → 자동
- body.propagate_to_ha_peers 로 override 가능
- sync_txn 트랜잭션 (멤버 2명 이상)
- 응답에 peers / sync_id / ha_group_id / propagated / scope

### T2 — collection drift 감지 (`2cfd794`)
- `_get_deployment_collection` 가 양 멤버 records hash 비교
- 응답에 drift_detected / peers[{count, hash, ...}]
- UI 가 노란 경고 배너 (T3)

### T4 — config_template scope 의미 재정의 (`2cfd794`)
- 옛 system = 무조건 멤버별 (split-brain 위험)
- 새 system = mode 인식
  - active_standby → 양 멤버 동일 (VIP)
  - all_active → 멤버별 svc IP
  - standalone → 단일
- `ha_lookup.should_propagate(scope, mode, override)` 헬퍼 일원화
- `csp/config/config_template.json` local_nodes 에 scope_note
- `docs/design/csc_config_server.md` §2.3 + API 표 갱신
- `deployment.ts` ConfigScope 주석에 새 정의

### T5 — `csp_runtime.py` (시스템 1) deprecate 마킹 (`2cfd794`)
- 모듈 docstring 에 DEPRECATED 표시 + 진짜 경로 명시 + 차후 정리 트랙 노트
- 폐기는 별도 트랙 (단위 테스트/migration 참조 의존성 때문)

### T3 — UI: drift 배너 + A/S 의 system scope 통합 편집 (`cc3ed63`)
- `GroupServiceConfigModal.haMode` prop 추가, scope=system + active_standby 도 그룹 카드에 포함
- `ModuleConfigEditor` 의 drift 배너 (노란 ⚠️ / 초록 ✓)
- group 케이스의 saveCollection 을 첫 deployment 1회 PUT 으로 단순화 (csc 자동 fan-out 활용)
- API 타입 보강 (peers/sync_id/drift_detected 등)

## 검증

- syntax: 모든 .py / .ts PASS
- Python: `tests.test_ha_fanout` + `tests.test_verify_lib` 176/176 PASS
  - 단위 테스트 4건 추가 (ShouldPropagateTests) + 기존 12건 + verify_lib 161건
  - test pollution fix: `tearDownModule()` 에서 httpsrv/services/handlers sys.modules cleanup
- Vite build: PASS (1782 modules, 0 errors)
- C++ build: 변경 없음

## commit 그래프 (origin/main `..cc3ed63`)

```
cc3ed63 feat(console): T3 — drift 배너 + A/S 의 system scope 통합 편집
2cfd794 feat(csc+docs): T1+T2+T4+T5 — deployment.collection fan-out + drift + scope 재정의
9b5699b feat(csc+agent): HA fan-out 절차 정상화 — 컬렉션/모듈 변경 시 그룹 멤버 일괄 동기
```

## 사용자 검증 가이드

내일 Console UI 에서 확인할 것:

1. **HaServicesPage 의 그룹 카드 진입** → "서비스 설정" 모달
   - A/S 그룹: 옛 scope=service collection (access_services, routes 등) + 새로 scope=system 인 local_nodes 도 그룹 카드에서 편집 가능해야 함
   - AA 그룹: scope=system 은 여전히 멤버별 (각 서버 카드)

2. **저장 후 응답**: 양 멤버에 자동 PUT. signaled 가 양쪽 PID 보여줘야 함

3. **drift 시뮬레이션**: ctrl-a 에만 수동으로 jsonl 변경 후 UI 로 다시 GET → 상단에 노란 ⚠️ 배너 + 멤버별 hash 표시. 저장 한번 누르면 양쪽 동기화 + ✓ 초록.

4. **propagate_to_ha_peers=false** body 로 옛 동작 (단일 deployment) 보장 — backward compat 검증.

## 다음 진입 후보 (사용자 합의 필요)

- (L1) LIVE 검증 회기 — ctrl-a/ctrl-b 2-node 환경에서 실제 sync_config job 흐름 + drift 시뮬레이션 + recovery
- (L2) `_put_deployment_config` (흐름 A, 흐름 B 와 별개 — 모듈 통째 config) 의 sync_txn 폴링 admin endpoint `GET /api/v1/csp/sync/<sid>` 추가
- (L3) sync_txn timeout sweeper 도입 (별도 task / cron)
- (L4) F2 — 멤버 jsonl checksum 비교 sweeper 자동 재동기화
- (L5) `csp_runtime.py` 시스템 1 실제 폐기 마이그레이션 (T5 의 차후 트랙)
- (L6) (G) IBCF 멀티 피어 routes LIVE — 옛 트랙 (오전 SIP outbound 후속)
