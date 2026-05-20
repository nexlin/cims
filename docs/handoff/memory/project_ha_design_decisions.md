---
name: 2026-05-12-ha-phase-1-a-1-b
description: HA 이중화 백로그 1.A + 1.B 완료. 결정사항 + ha_design.md + keepalived 인프라 (templates/probes/notify + cims.sh ha). 다음 라운드는 1.D-1 (Redis register replication) 또는 1.F (CSC mode).
metadata: 
  node_type: memory
  type: project
  originSessionId: 0f0f28b3-7806-4f46-bc85-efea242cdfbd
---

## 진행 상태

- **Phase 1.A 완료 (2026-05-12)** — `docs/design/ha_design.md` 신규 (399줄).
- **Phase 1.B 완료 (2026-05-12)** — `agent/keepalived/` (10 파일) + `cims.sh ha` 6 subcmd.
- **Phase 1.F 완료 (2026-05-12)** — VIP-only cold-spare. `agent/systemd/cims-{csc,csp,psp}.service.tpl` + notify systemctl 연동 + cims.sh ha config|apply 통합.
- **Phase 1.G 완료 (2026-05-12)** — `agent/cims_agent.py:run_loop` exponential backoff (5s→10s→20s→max 60s, 성공 시 reset). VIP target 은 --csc-url 인자만 변경.
- **Phase 1.E 골격 완료 (2026-05-12)** — `csp/ConsistentHashRing.h` 신규 (header-only, SHA1 hash, vnode=128, healthcheck-aware skip) + `csp/CmpClient.{h,cpp}` 의 m_endpoints / m_ring / AddEndpoint / SelectEndpointForSession. 단일 endpoint backward compat 유지. SendRequestAndWait 의 endpoint 인자 받기는 1.E-2 (caller ~10 메서드 인터페이스 확장 필요).
- **Phase 1.D-1 stub 완료 (2026-05-12)** — `csp/RedisStore.{h,cpp}` 신규 (cold-mode no-op, hiredis 미통합) + `csp/CspUser.cpp` registerUser/unregisterUser 에 SetBinding/DelBinding hook (IsConnected()=false 면 no-op). 1.D-2 에서 hiredis 통합 + CMakeLists link 추가하면 활성.
- **Phase 1.H stub 완료 (2026-05-12)** — `verify/lib/items/stage6/scn_failover_{csc,csp,cmp}.py` 3 신규. SKIP body. 환경 감지 (`ha.json` 존재 / `csp.json` Cmp.Endpoints≥2) 시 LIVE 분기 위치만 마련.

LIVE pipeline-full **38 / PASS 34 / FAIL 0 / SKIP 4 / 270.5s** (`20260512_142138_multi`). 회기능 영향 없음. 새 SKIP 3개는 모두 신규 FAILOVER stub.

다음 라운드: **1.E-2 (CmpClient SendRequestAndWait endpoint 인자 받기 + caller 변경)** — multi-CMP LIVE 분배 활성. 또는 **1.D-2 (hiredis 통합)** — Redis hot replication LIVE 활성. 둘 다 본격 C++ 변경 + 환경 의존 검증.

## 사용자 확정 결정사항 (2026-05-12)

| 항목 | 결정 |
|---|---|
| VIP 메커니즘 | **keepalived (VRRP)**, advert 1s + dead 3s, fail-over ~3초 |
| CSP/PSP state | **하이브리드** — register=hot via Redis, dialog=cold (진행 통화 끊김 허용) |
| CMP/PMP 분배 | **Consistent hash on Session-ID**, vnode=128, healthcheck 5s timeout + 30s 격리 |
| DB 이중화 | **별도 트랙** (이번 범위 외) |

## 대상 컴포넌트 매트릭스 (7개)

- CSC (mgmt 4420 + mcptt 4430): Active/Standby
- CSP (volte-sip-server, ISP 공존): Active/Standby
- PSP (ptt-sip-server): Active/Standby
- CMP (volte-media-server, IMP 공존): All Active
- PMP (ptt-media-server): All Active

## 후속 단계 분해 (설계 문서 §10)

| 단계 | 작업 | 주요 영향 |
|---|---|---|
| 1.B | keepalived 인프라 자동화 | ✅ `agent/keepalived/` (9 파일 + example) + `cims.sh ha` 6 subcmd |
| 1.D-1 | Redis register replication 골격 (stub) | ✅ `csp/RedisStore.{h,cpp}` (cold-mode no-op) + `csp/CspUser.cpp` hook |
| 1.D-2 | hiredis 통합 + Sentinel (LIVE 활성) | RedisStore 본체 hiredis 구현 + CMakeLists link |
| 1.E | CMP consistent hash 분배 (골격) | ✅ `csp/ConsistentHashRing.h` + `csp/CmpClient.{h,cpp}` endpoints/ring/AddEndpoint/SelectEndpointForSession |
| 1.E-2 | CMP multi-endpoint LIVE 분배 | SendRequestAndWait 의 endpoint 인자 + AddSession 등 caller ~10 메서드 변경 |
| 1.F | CSC/CSP/PSP A/S (VIP-only cold-spare) | ✅ `agent/systemd/cims-*.service.tpl` + notify systemctl 연동 + cims.sh ha config|apply |
| 1.G | cims_agent VIP target + backoff | ✅ `agent/cims_agent.py:run_loop` exponential backoff (5→10→20→max 60s) |
| 1.H | verify 시나리오 stub | ✅ `verify/lib/items/stage6/scn_failover_*.py` × 3 (SKIP body) |

## 핵심 영향 파일 (참조용)

- `csp/CspUser.h:113-135` — `CspUserMap`, register state 외부화 대상
- `csp/CallMap.h:60-84` — `CCallMap`, dialog cold 유지
- `csp/CmpClient.h:21-123` — 단일 CMP 가정 → multi-endpoint + ring
- `cmp/PCmpServer.h:15-153` — All Active 시 instance 별 독립
- `csc/src/csc_app.py:247-260` — Admin Server endpoint, VIP 뒤로
- `agent/cims_agent.py:862-901` — heartbeat URL → VIP

## 미확정 항목 (설계 문서 §11)

- Redis sentinel/cluster 도입 시점 — 1.D-1 안정화 후 재평가
- CSC standby 의 hot vs cold spare — 1.F 진입 시 RTO 재확정
- CMP all-active 시 RTP 포트 충돌 — 노드 IP 가 c= 라인이라 무문제 예상, 1.E 에서 검증
- TLS 단말 재핸드셰이크 latency — TLS 시나리오 진입 시 평가
- multi-site WAN 이중화 — 본 설계 범위 외

## 진입 절차 (다음 세션)

1. 본 메모리 + `docs/design/ha_design.md` 읽기
2. 사용자 confirm — 다음 단계가 1.B 인지 1.D-1 인지 (병렬 가능 여부 포함)
3. 선택된 단계 진행

## 관련 메모리

- [[2026-05-12-메인-백로그-이중화-설정-ux-console-콘솔-분리-안정화]] — 5개 큰 백로그 가이드
