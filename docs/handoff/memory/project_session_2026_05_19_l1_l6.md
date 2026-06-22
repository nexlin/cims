---
name: project_session_2026_05_19_l1_l6
description: "2026-05-19 — L1~L6 순차 진행 완료. commit 6개 (bab4173..13a3423). L1 LIVE 검증 중 _fetch_deployment_for_proxy hidden 버그 발견·수정. L2 sync_txn 폴링 endpoint, L3 timeout sweeper, L4 drift sweeper+resync, L5 csp_runtime.py 시스템 1 폐기, L6 IBCF routes LIVE 셋업 자동화."
metadata: 
  node_type: memory
  type: project
  originSessionId: 6d85790c-078d-4b0b-8c7f-034f5da3a824
---

# 2026-05-19 — L1~L6 순차 진행

[[project_session_2026_05_18_ha_fanout]] 후속. 사용자 "L1~L6 순차 진행, 이후 Console UI 검증 결과 보겠다".

## commit 시퀀스 (origin/main cc3ed63 → 13a3423)

| commit | track | 핵심 |
|---|---|---|
| `bab4173` | L1 fix | `_fetch_deployment_for_proxy` 가 `package_name` 도 enrich — T1/T2 가 LIVE 에서 effective NO-OP 였던 hidden 버그 |
| `65a288b` | L2 | `GET /api/v1/csp/sync[/<sid>]` — sync_txn 폴링 endpoint |
| `09a3150` | L3 | sync_txn timeout sweeper (15s interval, `SyncTxnSweepSec`) + `POST /sync/sweep` 수동 트리거 |
| `8b84fa8` | L4 | `services/drift_sweeper.py` + `GET/POST /api/v1/csp/drift[/resync]` + main loop integration |
| `d5e2dae` | L5 | csp_runtime.py 라우터 등록 제거, GET /csp/services 가 진짜 SoT (deployment.collection/access_services) 읽음. cspRuntime.ts 313→90 line |
| `13a3423` | L6 | `scripts/verify_ibcf_outbound_smoke.sh` — setup/smoke/verify/teardown 자동화 |

## L1 — LIVE 검증 + 버그 발견

5 시나리오 LIVE PASS (ctrl-a/b 2-node, ha_group 4 Control-Server, active_standby):
1. GET drift_detected/peers/ha_group_id 필드 노출 ✓
2. drift inject (ctrl-b priority 변형) → drift_detected=true, hash 다름 ✓
3. PUT propagate=true (default) → 양 멤버 PUT, signaled 양 csp PID, drift 해소 ✓
4. PUT propagate=false → 단일 deployment (peers=1), drift 잔존 (backward compat) ✓
5. PUT local_nodes (scope=system + active_standby) → propagated=true (T4 의미 재정의 LIVE) ✓

**Hidden 버그**: LIVE 의 deployment file_store row 는 `package_name=null` (package_id 만 보존). `_get_deployment_collection` / `_put_deployment_collection` 이 `dep.get("package_name")` 으로 ha_group lookup 분기 → 항상 None → fan-out + drift 가 LIVE 에서 NO-OP. fix 1줄 (`pkg.get('name')` 으로 enrich).

## L2 — sync_txn 폴링 endpoint

- `GET /api/v1/csp/sync?limit=&status=` — 최근 N건 (status pending|partial|success|failed 필터)
- `GET /api/v1/csp/sync/<sid>` — 단일 트랜잭션 + 멤버 ack 슬롯
- 405 method_not_allowed / 404 not_found / 400 invalid_id 모두 정상

## L3 — sync_txn timeout sweeper

- `_sweep_sync_txn()` main loop 통합, default 15s (`SyncTxnSweepSec`)
- `POST /api/v1/csp/sync/sweep` 수동 트리거 (응답 `{ok, timed_out}`)
- LIVE 검증: ttl=1 pending txn → 17s 대기 → 자동 sweep log "timed out 1 transaction(s)" → status=failed

## L4 / F2 — drift sweeper + resync

- 새 `services/drift_sweeper.py` — `scan_group_collection` / `scan_all` / `emit_drift_alerts` / `auto_resync`
- `scope=service` 또는 `scope=system+active_standby` 컬렉션만 비교 (`ha_lookup.should_propagate`)
- `GET /api/v1/csp/drift?drift_only=1` — drift list
- `POST /api/v1/csp/drift/resync` — drift 있는 컬렉션 master records 로 자동 PUT
- main loop interval=300s (`DriftSweepSec`) + `AutoResyncDrift=true` 시 자동 PUT (default false)
- LIVE 검증: 9 컬렉션 scan, drift inject → resync → 재scan drift_count=0
- alert: `config_drift::g{gid}::{collection}` open/close 이벤트 → alerts/YYYY/MM/DD.jsonl

**자동 재동기화 default off**: master 선정 모호성 (어느 쪽이 "정답"?) → alert 만 발행, 수동 confirm 후 POST /resync 권장.

## L5 — csp_runtime.py 폐기

**LIVE 진단으로 드러난 hidden 문제**:
- csp_runtime 의 5 file_store 도메인 (csp_listener / sip_trunk / routing_rule / routing_access_list / sip_service) 모두 비어있음 (옛 DB 시대 캐시 잔존)
- UI 의 VolteMsisdnPage/PttMsisdnPage 가 `cspRuntimeApi.listServices()` 호출 → 빈 응답 → service dropdown 이 비어 있는 채로 동작 중
- 진짜 SoT 는 첫 csp deployment 의 install_path/config/access_services.jsonl

**변경**:
1. `agents.py` 에 `handle_sip_services` 추가 — GET /csp/services 가 첫 csp deployment 의 access_services 컬렉션 → SipService 형식으로 변환 반환
2. `csc_app.py` — `CIMS_CSP_RUNTIME_HANDLER_LIST` import + 등록 제거. listener/trunk/route/access endpoint 는 404 No route
3. `csp_runtime.py` — 파일은 보존 (migrate 스크립트 의존), 모듈 docstring 을 RETIRED 격상
4. `ems/core/console/src/api/cspRuntime.ts` — listServices() 만 남기고 25개 미사용 함수 + types 삭제. `SipService.listeners` 타입 `number[]` → `string[]` (allowed_local_node_refs)

LIVE PASS: GET /csp/services 이제 volte-basic/ptt-basic 실제 데이터, GET 999 → 404, POST → 410, /csp/listeners → 404 no route. TS check + Vite build + Python 176 tests 모두 PASS.

## L6 — IBCF 멀티 피어 routes LIVE 자동화

cspsim 실행에 sudo (netns exec) 필요하므로 본 세션에서 끝까지 LIVE 못 감. 자동화 스크립트 + setup/teardown LIVE 만:

`scripts/verify_ibcf_outbound_smoke.sh` 4 단계:
- `setup` — remote_nodes(외부 peer)/rules/rule_sets/routes/route_sets/routing_policies 1행씩 PUT
- `smoke` — cspsim 외부 user 호 (외부 peer 없음 → INVITE tx 만)
- `verify` — sip.jsonl 의 outbound INVITE Via/Contact 가 csp-main-tcp 25061 인지 검증
- `teardown` — 모든 컬렉션 빈 records 로 PUT

LIVE 검증된 부분:
- setup → HA fan-out 양 멤버 ctrl-a/b 의 jsonl 동기
- csp SIGUSR1 reload → log "RoutingPolicyEngine: sync complete, 1 policies" + "RouteMap: sync complete, 1 routes" + "RemoteNodeMap: sync complete, 3 nodes"
- teardown → 0 records 복원 + remote_nodes 의 cmp-media-a/b 만 남음

다음 사용자 LIVE 실행 cheat sheet:
```bash
./scripts/verify_ibcf_outbound_smoke.sh setup
sudo ip netns exec sim-a ./scripts/verify_ibcf_outbound_smoke.sh smoke
./scripts/verify_ibcf_outbound_smoke.sh verify
./scripts/verify_ibcf_outbound_smoke.sh teardown
```

## 환경 상태 (다음 세션 진입 시)

- csc 4419 LIVE (bab4173 ~ 13a3423 빌드 — PID 변동: 본 세션 4번 재기동)
- csp ctrl-a/b PID 2270316/2270317 (변동 없음 — 092047d 빌드 그대로)
- cmp media-a/b PID 640474/640475 (옛 5/15 빌드 — cmp 코드 변경 없음)
- VIP 10.0.1.13 ctrl-a MASTER
- TB-CSC 4419 + Vite 3000

새 admin endpoint:
- `/api/v1/csp/sync` (L2/L3)
- `/api/v1/csp/sync/sweep` (L3)
- `/api/v1/csp/drift` (L4)
- `/api/v1/csp/drift/resync` (L4)
- `/api/v1/csp/services` 이제 진짜 SoT 읽음 (L5)

새 csc.json 설정 키:
- `SyncTxnSweepSec` (default 15)
- `DriftSweepSec` (default 300)
- `AutoResyncDrift` (default false)

## 다음 진입 후보

| 후보 | 작업 | 비고 |
|---|---|---|
| **(M1) Console UI 검증** | 사용자 의도된 트랙. HaServicesPage 그룹 카드 → 서비스 설정 모달, drift 배너, A/S system scope 통합 편집 | 사용자 진행 예정 |
| (M2) L6 cspsim 실 호 + sip.jsonl tx 검증 | netns exec sudo 필요 | smoke script ready |
| (M3) auto_resync default on 정책 검토 | L4 의 자동 동기화를 default true 로 전환할지 (drift 가 평소에 거의 안 생기면) | 운영 데이터 보고 결정 |
| (M4) csp_runtime.py 파일 + migrate 스크립트 완전 삭제 | 1 릴리스 안정화 후 | 옛 트랙 (L5 후속) |
| (M5) Drift sweeper 의 records 본문 전송 최적화 | scan_all 이 매번 모든 records 본문을 가져옴 — etag/마지막 hash 비교로 단축 | 부하 측정 후 |
| (M6) IBCF role 활성화 + L6 본격 LIVE | scope 큼 — IBCF proxy + Record-Route 적극 push (E 트랙) | 별도 회기 |
