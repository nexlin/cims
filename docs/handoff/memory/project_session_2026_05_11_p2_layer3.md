---
name: 2026-05-11 P2 Layer 3 — IBCF routing seed 인프라
description: P2 Layer 3 라운드 — routing_policies/routes seed helper + cspsim -no_register + S6-SCN-IBCF-TRUNK 시나리오. LIVE pipeline-full 35/33 PASS (FAIL 0). 시나리오는 CSP G10 미구현으로 SKIP.
type: project
originSessionId: d333f5ca-670f-4d60-ab37-e7d660ea1c93
---
# P2 Layer 3 — IBCF routing seed 인프라 (2026-05-11)

**commit `54fc82a`** — origin/main 동기화 완료.

## 결과

LIVE pipeline-full **35/33 PASS / FAIL 0 / SKIP 2** in 370.3s.
- baseline (34/33, 333s) + 1 SKIP 추가 — S6-SCN-IBCF-TRUNK 의도적 SKIP.
- 33 PASS / 2 SKIP (S6-SCN-IBCF-TRUNK + S6-SCN-CERT-ROTATE).

## Deliverable (7 파일, +411/-14)

1. **`verify/lib/common/ibcf_routing.py`** (신규, 118줄) — 6 collection 시드:
   - local_nodes (lb-ibcf-peering, edge=peering, ISP IP)
   - remote_nodes (rn-mock-peer, ip=127.0.0.1, port=6800, domain=trunk.peer.test)
   - routes, route_sets (failover, health_check_mode=none)
   - rules (`req_uri_user contains "trunk.peer.test"` — caller 의 cspsim 이
     m_strDomain="csp" 라 RequestURI host 가 ISP IP 로 박히고 외부 도메인은
     user 부분에 박힘. host suffix 매칭 안 됨 → user contains 로 우회.)
   - rule_sets (combinator=AND)
   - routing_policies (priority=50, target_type=route_set, fail_action=reject)

2. **cspsim `-no_register`** — `SimSession::SetNoRegister(bool)` setter +
   m_bNoRegister flag. constructor 의 `InsertRegisterInfo` 호출을 `Start()` 로
   이동 + flag false 일 때만 호출. RunScenario 의 register 대기 loop 도
   m_bNoRegister 면 즉시 ready 로 처리.

3. **`verify/lib/items/stage6/scn_ibcf_trunk.py`** (신규, 175줄) — S6-SCN-IBCF-TRUNK
   verify item. 현재 본체는 SKIP 반환. `_scn_ibcf_trunk_full` 함수에 실제
   LIVE 검증 코드 보존 (다음 라운드 G10 구현 후 본체로 swap).

4. **`verify/lib/items/stage6/seed.py`** — ISP variant 만 access_services
   시드 skip + `seed_ibcf_routing` 호출 + reload signal.

5. **`verify/lib/items/stage6/summary.py`** — S6-SCN-IBCF-TRUNK 를
   depends_on 에 추가.

## CSP G10 미구현 — SKIP 사유

caller cspsim 이 `-no_register` 로 INVITE 송신 → ISP 가 받음 →
`ModuleDispatcher::RecvRequest` L389~403 에서 From URI user 가 ISP 의 user
map 에 없음 → `CCscfModule::CheckAuthrization` 호출 → **무조건 401
Unauthorized 응답** (CSCF role 비활성이어도 이 흐름은 동작).

`ModuleDispatcher.cpp` L383 주석 명시:
> G10 (2026-04-23): IBCF XML trunk 기반 incoming auth skip/routing 제거.
> 외부 peer 인바운드는 AclPolicy (remote_nodes 기반) 에서 평가되어야 함
> — **추후 확장**.

즉 401 challenge 흐름이 routing 평가보다 먼저 실행되어, routing path 까지
도달 못함. routing_policies 시드 자체는 정상 (csp log: "RoutingPolicyEngine:
sync complete, 1 policies").

## 다음 라운드 진입 옵션 (3개)

### 옵션 A — CSP G10 정석 구현 (권장)
`ModuleDispatcher::RecvRequest` 의 user map check 직전에 분기:
1. `gclsPendingRouteMap.Has(callId)` 가 true → routing 으로 결정된 호 → auth skip
2. 또는 src_ip 가 `gclsRemoteNodeMap` 에 등록된 외부 peer → auth skip

### 옵션 B — routing_policies 매칭 시 user check skip
L387~ 의 user map check 자체에 PendingRouteMap.Has() 분기 추가. 가장 단순
(1~2줄 변경).

### 옵션 C — cspsim pre-emptive Digest auth
우회책. 본질적 fix 아님.

→ G10 구현 후 `_scn_ibcf_trunk_full` 본체로 swap (line edit) →
S6-SCN-IBCF-TRUNK 가 LIVE PASS 로 전환.

## 환경 노트

- Mock peer: `127.0.0.1:6800` (loopback alias 추가 부담 회피). 127.0.0.5 는
  ISP 만 사용.
- cspsim peer 종료: subprocess.Popen(stdin=PIPE) + "q\n" 전송 + close →
  cspsim fgets EOF → 정상 종료.
- caller cspsim 호출은 `cims.sh sim` 우회하고 `bin/cspsim` 직접 호출.
  `cims.sh sim` 의 후처리 ls 출력이 tail 100 줄을 점거하는 문제 회피.

## 진행 중 발견 (참고)

- `seed.py` 가 모든 csp variant (csp/psp/isp) 에 access_services 를 시드 →
  ISP 에 시드 시 inbound 401 흐름 트리거 (역사적 동작). ISP 는 IBCF only 라
  access_services 시드 의미 없음 → 빈 파일로 덮어쓰기.
- `SipServerSetup.cpp` 의 `ConfigJsonlDir` fallback (csp.json 부모×3 +
  `/config`) 이 ISP 에서도 정상 동작. 명시적 주입 불필요.
