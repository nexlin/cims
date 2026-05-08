# SIP 런타임 설정 (jsonl + SIGUSR1)

> 9 collection 모델. 엔티티 의미는 `sip_service_model.md` 를 참조.

Console 에서 CSP 의 수신 엔드포인트/피어 연결/라우팅 정책/ACL 을 **재기동 없이 제어**하는 런타임 설정 계층.

## 1. 기본 원리

```
 ┌──────────┐  PUT    ┌──────────┐  PUT/GET  ┌──────────┐  SIGUSR1  ┌──────────┐
 │ Console  │────────▶│   CSC    │──────────▶│  Agent   │──────────▶│   CSP    │
 │  (React) │  JWT    │          │ mTLS      │ :9900    │  (signal) │  (C++)   │
 └──────────┘         │          │           │          │           │          │
                      │ 템플릿   │           │  jsonl   │           │ jsonl    │
                      │ schema   │           │ atomic   │           │ reload   │
                      │ 검증     │           │  write   │           │  + Sync  │
                      └──────────┘           └──────────┘           └──────────┘
```

- **원천(Source of truth)**: 각 Deployment 의 `<install_path>/config/*.jsonl`
- CSC 는 DB 에 저장하지 않고 Agent 에 프록시만 수행 (jsonl 단일 경로 — DB/HTTP pull 모드 없음)
- CSP 는 시작 시 jsonl 읽어 메모리 캐시로 로드, SIGUSR1 수신 시 재로드

## 2. 데이터 모델

9개 collection = 9개 jsonl 파일:

```
<install_path>/config/
├── local_nodes.jsonl           # 수신 엔드포인트 (구 listeners.jsonl)
├── remote_nodes.jsonl          # 피어 서버 (구 trunks 의 transport 부분)
├── routes.jsonl                # (LN, RN) pair + auth/outbound 파라미터
├── route_sets.jsonl            # Route 클러스터 + 분배 정책
├── rules.jsonl                 # 원자 조건
├── rule_sets.jsonl             # Rule AND/OR 조합
├── routing_policies.jsonl      # RuleSet → RouteSet/AccessService
├── acl_policies.jsonl          # RuleSet → allow/deny
└── access_services.jsonl       # UE 서비스 (voip/ptt)
```

한 줄 = 한 레코드. 레코드 `id` 는 서버가 생성한 16 hex UUID.

예 (local_nodes.jsonl):
```jsonl
{"id":"d60bccef...","name":"lb-access-udp","edge":"access","bind_ip":"0.0.0.0","bind_port":5060,"protocol":"UDP"}
{"id":"335dc240...","name":"lb-peering","edge":"peering","bind_ip":"0.0.0.0","bind_port":5070,"protocol":"UDP"}
```

예 (routes.jsonl):
```jsonl
{"id":"...","name":"r-kt-1","local_node_ref":"lb-peering","remote_node_ref":"kt-sbc-1","auth_user":"","auth_password":""}
```

예 (route_sets.jsonl):
```jsonl
{"id":"...","name":"rs-kt","distribution_policy":"round_robin","members":[{"route_ref":"r-kt-1","priority":100,"weight":1},{"route_ref":"r-kt-2","priority":100,"weight":1}],"health_check_mode":"options_ping"}
```

## 3. 스키마 선언

`csp/config_template.json` 의 `collections[]` 에 9개 정의. (CMP 용도 유사 — `cmp/config_template.json`)

CSC 는 이 스키마로:
- UI 폼을 동적 렌더 (`type`, `enum options`, `required`, `advanced`)
- PUT 시 validation (type/enum/required/ref 참조 존재 여부)
- `id_type: "uuid"` 인 경우 자동 ID 부여
- `tags` 필드 (`type: string_list`) 는 자유 입력

## 4. CSP 런타임 통합

### 4.1 시작 시

`csp.json` 의 `Setup.ConfigJsonlDir` 가 설정 (또는 자동 fallback) 되면 jsonl 모드:

```cpp
gclsCspConfigCache.Init(jsonlDir);      // 9 collection 로드
gclsCspConfigCache.LoadInitial();

gclsLocalNodeMap.Sync();                // local_nodes → psip rebind
gclsRemoteNodeMap.Sync();
gclsRouteMap.Sync();
gclsRouteSetMap.Sync();
gclsRuleEvaluator.LoadAll();            // rules + rule_sets
gclsRoutingPolicyEngine.Sync();         // routing_policies
gclsAclPolicyEngine.Sync();             // acl_policies
gclsAccessServiceMap.Sync();            // access_services
```

### 4.2 Reload

CSP 메인 루프가 SIGUSR1 플래그 감지 → 전체 재로드:

```cpp
if (g_reloadFlag && gclsCspConfigCache.IsJsonlMode()) {
    gclsCspConfigCache.ReloadFromJsonl();
    gclsLocalNodeMap.Sync();       // bind/unbind 실제 소켓
    gclsRemoteNodeMap.Sync();
    gclsRouteMap.Sync();
    gclsRouteSetMap.Sync();
    gclsRuleEvaluator.LoadAll();
    gclsRoutingPolicyEngine.Sync();
    gclsAclPolicyEngine.Sync();
    gclsAccessServiceMap.Sync();
}
```

### 4.3 로드 순서 의존성

참조 그래프 때문에 순서를 지켜야 한다:
1. `local_nodes`, `remote_nodes` (의존성 없음)
2. `routes` (LN, RN 참조)
3. `route_sets` (Route 참조)
4. `rules` (의존성 없음)
5. `rule_sets` (Rule 참조)
6. `routing_policies` (RuleSet + RouteSet/AccessService 참조)
7. `acl_policies` (RuleSet 참조)
8. `access_services` (LocalNode 참조)

참조 누락 시 해당 레코드는 skip + ERROR 로그.

## 5. Agent 측 책임

- `PUT /collection?install_path=&name=` 수신 시:
  1. 임시 파일(`.tmp`) 쓰기 → `rename()` 으로 원자 치환
  2. `signal=true` 면 `install_path/run/*.pid` 찾아 SIGUSR1 전송
  3. 응답에 signaled pid 목록 포함
- `GET /collection?...` 은 그대로 jsonl 파싱해서 반환

9 collection 모두 동일한 엔드포인트로 처리 (collection name 파라미터로 구분).

## 6. 회복탄력성

| 고장 상태 | CSP 영향 | Console |
|-----------|----------|---------|
| Agent 오프라인 | 이미 로드된 설정으로 동작 | 조회/편집 불가 (502) |
| CSC 다운 | 무관 (CSP ↔ Agent 직접 경로 없음) | 전체 불가 |
| CSP 재기동 | install_path/config/ 다시 로드 → 정상 복구 | — |
| jsonl 없음 / 손상 | 해당 collection 빈 배열로 동작. local_nodes 가 비면 Setup.Sip.UdpPort (bootstrap) 로만 수신 | 정상 편집 가능 |
| 참조 무결성 위반 | 해당 참조 레코드 skip + ERROR 로그. 서비스는 계속 | 경고 표시 |

## 7. 관련 소스

- `csp/CspConfigCache.{h,cpp}` — 9 collection 로더 (entity enum)
- `csp/CspServer.cpp` — SIGUSR1 핸들러 + 메인 루프 reload 시퀀스
- `csp/CspLocalNodeMap.cpp` 등 — Sync() 는 캐시 items 읽어 delta 적용
- `csp/CspRuleEvaluator.{h,cpp}` — ACL/Routing 공통 평가 엔진
- `csp/CspRoutingPolicyEngine.{h,cpp}` — routing 결정
- `csp/CspAclPolicyEngine.{h,cpp}` — ACL 결정
- `csp/CspAccessServiceMap.{h,cpp}` — UE 서비스
- `agent/cims_agent.py` — Sync REST 서버 (`/collection`)
- `csc/src/handlers/agents.py` — `_get/_put_deployment_collection` 프록시

## 8. 식별자 용어 정리

수신 엔드포인트를 가리키는 3가지 식별자 (모두 1:1 대응):

| 용어 | 타입 | 위치 | 용도 |
|------|------|------|------|
| `LocalNode.id` | UUID 문자열 | `local_nodes.jsonl` PK | 영구 식별자 |
| `LocalNode.name` | 문자열 | `local_nodes.jsonl` | 다른 collection 의 `*_ref` 대상 |
| `listener_id` (psip int) | 양의 int | `CSipStackUdpListener.m_iId`, `CSipMessage.m_iListenerId` | psip 내부 소켓 식별, ACL/restricted 정책 판정 |

상호 변환:
- `listener_id = CspUuidToIntId(LocalNode.id)` — 안정 해시 (재기동해도 같은 값)
- `LocalNodeMap.GetByIntId(listener_id)` → `LocalNodeInfo` (name 등 포함)
- `LocalNodeMap.GetByName(name)` → `LocalNodeInfo`

## 9. psip v3 확장 (2026-04-22)

수신 메시지에 리스너 식별 정보를 실어주도록 psip stack 확장.

- **`CSipMessage.m_iListenerId`** (신규 필드) — UDP 수신 시 `CSipStackUdpListener.m_iId` 값 복사.
  송신/미지정/TCP/TLS 는 `-1`.
- **수신 경로** (`SipStackComm.hpp`): UDP 수신 스레드가 세팅하는 `t_iCurrentListenerId` (thread-local)
  을 메시지 구성 시점에 복사.
- **AclPolicyEngine**: `ModuleDispatcher.RecvRequest` 에서 메시지의 `m_iListenerId` →
  `LocalNodeMap.GetByIntId` → `LocalNode.name` → `Check(local_node_name, ...)`.
  → scope=local_node 정책 정상 동작.
- **AccessService restricted**: `CCspServiceMap::IsInboundAllowed(svc, listener_int_id)` 로
  `svc.listeners[]` (LocalNode 이름 → hash int 파생) 과 매칭. CscfModule REGISTER 경로에서 적용.

한계:
- TCP/TLS 수신은 현재 단일 리스너 구조 — listener_id 는 UDP 만 의미. TCP/TLS 다중 리스너 전환은 후속.

## 10. 변경 내역

- **v3.0 (2026-04-22)**: 4 collection → 9 collection 재설계. Rule/RuleSet/Policy 도입.
- **v2.0 (2026-04-21)**: DB + UDP notify 경로 → jsonl + SIGUSR1 로 전환.
