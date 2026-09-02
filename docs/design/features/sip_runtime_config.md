# SIP 런타임 설정 (jsonl + SIGUSR1)

> 9 collection 모델. 엔티티 의미는 `sip_service_model.md` 를 참조.

Console 에서 CSP 의 수신 엔드포인트/피어 연결/라우팅 정책/ACL 을 **재기동 없이 제어**하는 런타임 설정 계층.

## 1. 기본 원리

```
 ┌──────────┐  PUT    ┌──────────┐  PUT/GET  ┌──────────┐  SIGUSR1  ┌──────────┐
 │ Console  │────────▶│   OAM    │──────────▶│  Agent   │──────────▶│   CSP    │
 │  (React) │  JWT    │          │ mTLS      │ :9900    │  (signal) │  (C++)   │
 └──────────┘         │          │           │          │           │          │
                      │ 템플릿   │           │  jsonl   │           │ jsonl    │
                      │ schema   │           │ atomic   │           │ reload   │
                      │ 검증     │           │  write   │           │  + Sync  │
                      └──────────┘           └──────────┘           └──────────┘
```

- **원천(Source of truth)**: 각 Deployment 의 `<install_path>/config/*.jsonl`
- OAM 은 DB 에 저장하지 않고 Agent 에 프록시만 수행 (jsonl 단일 경로 — DB/HTTP pull 모드 없음)
- CSP 는 시작 시 jsonl 읽어 메모리 캐시로 로드, SIGUSR1 수신 시 재로드

## 2. 데이터 모델

9개 collection = 9개 jsonl 파일:

```
<install_path>/config/
├── local_nodes.jsonl           # 수신 엔드포인트
├── remote_nodes.jsonl          # 피어 서버 (transport)
├── routes.jsonl                # (LN, RN) pair + auth/outbound 파라미터
├── route_sets.jsonl            # Route 클러스터 + 분배 정책
├── rules.jsonl                 # 원자 조건
├── rule_sets.jsonl             # Rule AND/OR 조합
├── routing_policies.jsonl      # RuleSet → RouteSet/AccessService
├── acl_policies.jsonl          # RuleSet → allow/deny
└── access_services.jsonl       # UE 서비스 (volte/ptt)
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

`csp/config/config_template.json` 의 `collections[]` 에 9개 정의. (CMP 용도 유사 — `cmp/config/config_template.json`)

OAM 은 이 스키마로:
- UI 폼을 동적 렌더 (`type`, `enum options`, `required`, `advanced`)
- PUT 시 validation — **`required` / `int` / `bool` / `enum` / `string_list`(`options` 선언 시 원소
  검사)** 를 검사한다 (`_validate_record`)
- `id_type: "uuid"` 인 경우 자동 ID 부여 (`uuid4().hex[:16]`)
- `string_list` 는 기본 자유 입력(콤마 분리 — 예: `tags`). 필드에 `options` 를 선언하면 닫힌 값
  공간으로 간주해 콘솔이 체크박스 다중 선택으로 렌더한다 (예: `access_services.sec_mechanisms`
  `["tls","ipsec-3gpp"]` — 자유 입력 오타가 협상 제시 누락으로 조용히 실패하는 것을 방지)

**콘솔 편집(PUT) 경로는 참조 무결성을 검사하지 않는다.** 존재하지 않는 `local_node_ref` /
`route_ref` / `rule_ref` / `match_rule_set_ref` 도 그대로 저장되며, CSP 가 로드할 때 ERROR 로그로만
드러난다 (§6 표). 확인 방법:

```bash
grep -E "references missing|missing route ref|unsupported kind|duplicate name" log/*.log
```

반면 자동 배포 렌더러(`deployment/bin/render.py`)는 시나리오 YAML → jsonl 생성 시 모든 `*_ref`
(local_node/remote_node/route/rule/rule_set/route_set)의 존재와 `kind ∈ {volte, ptt}` 를 검증하고
위반 시 `RenderError` 로 중단한다. 두 경로의 검증 강도가 다르다는 점을 유의한다.

**PUT 은 파일 전체 교체(full replace)** 다. `records` 배열이 파일 내용을 그대로 대체하므로,
레코드 1개를 수정할 때도 GET 으로 전체를 받아 수정한 뒤 PUT 해야 한다. 또한 PUT 은 해당
deployment 에만 적용되고 **HA 그룹에 자동 전파되지 않는다** — 멤버 간 정합은 그룹 동기화
(`POST /deployments/{id}/sync`) 로 맞추며, 불일치는 GET 응답의 `drift_detected` 로 노출된다.

## 4. CSP 런타임 통합

### 4.1 시작 시

`csp.json` 의 `Setup.ConfigJsonlDir` 가 설정 (또는 자동 fallback) 되면 jsonl 모드:

```cpp
gclsCspConfigCache.Init(jsonlDir);      // 9 collection 로드
gclsCspConfigCache.LoadInitial();

gclsLocalNodeMap.Sync();                // 이름→노드 캐시 (primary 로 LocalIp/UdpPort 유도)
gclsAccessServiceMap_Sync_compat();     // access_services
gclsSipLogger.SetDomainServiceMap( gclsServiceMap.BuildDomainToKindMap() );
  ... (SIP 스택 기동) ...
gclsRemoteNodeMap.Sync();
gclsRouteMap.Sync();      gclsRouteMap.ValidateRefs();
gclsRouteSetMap.Sync();   gclsRouteSetMap.ValidateRefs();
gclsRuleEvaluator.LoadAll();            // rules + rule_sets
gclsRoutingPolicyEngine.Sync();         // routing_policies (priority 순 정렬)
gclsAclPolicyEngine.Sync();             // acl_policies (priority 순 정렬)
gclsListenerManager.Sync();             // ★ local_nodes → psip 소켓 실제 add/remove
```

`local_nodes` 로 실제 소켓을 여닫는 주체는 `CCspListenerManager` 다. `CCspLocalNodeMap` 은 이름/primary
조회용 캐시일 뿐이다.

### 4.2 Reload

CSP 메인 루프가 SIGUSR1 플래그 감지 → 전체 재로드:

```cpp
if (g_reloadFlag) {
    gclsSetup.Read();                   // csp.json 스칼라도 재파싱
    gclsCspConfigCache.ReloadFromJsonl();
    gclsLocalNodeMap.Sync();
    gclsRemoteNodeMap.Sync();
    gclsRouteMap.Sync();      gclsRouteMap.ValidateRefs();
    gclsRouteSetMap.Sync();   gclsRouteSetMap.ValidateRefs();
    gclsRuleEvaluator.LoadAll();
    gclsRoutingPolicyEngine.Sync();
    gclsAclPolicyEngine.Sync();
    gclsAccessServiceMap_Sync_compat();
    gclsListenerManager.Sync();         // 소켓 add / remove / rebind
    // primary 포트가 바뀌었으면 identity 포트도 추종
}
```

**`csp.json` 도 같이 다시 읽는다.** 사용 시점에 값을 읽는 스칼라(`CallPickupId`,
`StaleCallTimeout`, `UserTimeout`, `MinRegisterTimeout`, `SendOptionsPeriod` 등)는 재기동 없이
반영된다. 반면 기동 시 1회만 소비되는 부트스트랩 값(`UdpThreadCount`, `StackExecutePeriod`,
`Database.*`, `Monitor.*`)은 이미 생성된 객체에 적용되지 않으므로 재기동이 필요하다.

**리스너 delta 판정** (`CCspListenerManager::Sync`) — `id` + bind 파라미터
(`bind_port`, `bind_ip`, `protocol`, `thread_count`, TLS cert/key/ca) 를 비교한다.

| 상태 | 동작 |
|------|------|
| 전부 동일 | 유지 (재바인딩하지 않음) |
| 하나라도 변경 | remove + add (= 무중단 rebind) |
| 레코드 삭제 또는 `enabled=false` | remove (소켓 닫힘) |
| 신규 레코드 | add |
| `protocol` = WS/WSS | psip 미지원 → skip (DEBUG 로그) |
| `bind_port<=0` 또는 `id` 빈 값 | skip |

TLS 는 `tls_cert_path`/`tls_key_path`/`tls_ca_path` 가 리스너별로 psip `AddTlsListener` 에 전달된다.
비어 있으면 stack-global cert(`Setup.Sip.CertFile`)를 쓴다.

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
| OAM 다운 | 무관 (CSP 는 이미 로드된 jsonl 로 동작) | 조회/편집 불가 |
| CSP 재기동 | install_path/config/ 다시 로드 → 정상 복구 | — |
| jsonl 없음 / 손상 | 해당 collection 은 빈 배열로 동작. 깨진 줄은 그 줄만 skip (Agent 파서) | 정상 편집 가능 |
| **local_nodes 에 UDP 리스너 없음** | **기동 실패 (fail-fast).** 부트스트랩 UDP 바인딩이 없으므로 UDP 리스너는 전적으로 local_nodes 소유다 — `CspServer.cpp` 가 `ListenerManager.Sync()` 후 UDP 리스너 0개면 ERROR 로그 남기고 종료한다 | 정상 편집 가능 |
| 참조 무결성 위반 | 해당 참조 레코드 skip + ERROR 로그. 서비스는 계속 | 경고 표시 |

TCP/TLS 는 `Setup.Sip.TcpPort` / `TlsPort` 로 부트스트랩 리스너가 생성되며, 기동 시 primary
TCP/TLS local_node 가 있으면 그 포트·인증서로 덮어쓴다. UDP 만 local_nodes 전담이다.

## 7. 관련 소스

- `csp/CspConfigCache.{h,cpp}` — 9 collection 로더 (entity enum)
- `csp/CspServer.cpp` — SIGUSR1 핸들러 + 메인 루프 reload 시퀀스
- `csp/CspLocalNodeMap.cpp` 등 — Sync() 는 캐시 items 읽어 delta 적용
- `csp/CspListenerManager.{h,cpp}` — local_nodes → psip 소켓 add/remove/rebind
- `csp/CspRuleEvaluator.{h,cpp}` — ACL/Routing 공통 평가 엔진
- `csp/CspRoutingPolicyEngine.{h,cpp}` — routing 결정
- `csp/CspAclPolicyEngine.{h,cpp}` — ACL 결정
- `csp/CspServiceMap.{h,cpp}` — UE 서비스 (`CCspServiceMap` / `gclsServiceMap`)
- `agent/cims_agent.py` — Sync REST 서버 (`/collection`, `_write_jsonl_atomic`, `_signal_process`)
- `ems/core/oam/src/handlers/agents.py` — `_get/_put_deployment_collection` 프록시 + `_validate_record`

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

## 9. psip 리스너 식별 확장

수신 메시지에 리스너 식별 정보를 실어주도록 psip stack 을 확장한다.

- **`CSipMessage.m_iListenerId`** — UDP 수신 시 `CSipStackUdpListener.m_iId` 값 복사.
  송신/미지정/TCP/TLS 는 `-1`.
- **수신 경로** (`SipStackComm.hpp`): UDP 수신 스레드가 세팅하는 `t_iCurrentListenerId` (thread-local)
  을 메시지 구성 시점에 복사.
- **AclPolicyEngine**: `ModuleDispatcher.RecvRequest` 에서 메시지의 `m_iListenerId` →
  `LocalNodeMap.GetByIntId` → `LocalNode.name` → `Check(local_node_name, ...)`.
  → scope=local_node 정책 정상 동작.
- **AccessService restricted**: `CCspServiceMap::IsInboundAllowed(svc, listener_int_id)` 로
  `svc.listeners[]` (LocalNode 이름 → hash int 파생) 과 매칭. CscfModule REGISTER 경로에서 적용.

psip 다중 리스너 인프라:
- psip `CSipStack` 은 UDP/TCP/TLS **세 transport 모두 다중 리스너**를 지원한다. 각각 `m_vecUdpListeners` / `m_vecTcpListeners` / `m_vecTlsListeners` 벡터로 보유하며, 런타임 add/remove API(`AddUdpListener` / `AddTcpListener` / `AddTlsListener` + 각 Remove + GetInfo)로 무중단 변경한다.
- 스레딩: UDP 는 per-listener recv 스레드 풀(`m_iThreadCount` = local_nodes 의 thread_count). TCP/TLS 는 per-listener accept 스레드 1개 + 공유 worker 풀.
- TLS 는 per-listener 인증서(`m_strCertFile` / `m_strKeyFile` / `m_strCaCertFile` → `SSLServerCtxCreate` 로 `SSL_CTX* m_pSslCtx`)를 accept 시점에 선택하며, 리스너 ctx 가 NULL 이면 stack-global ctx 로 폴백한다.

한계:
- 다중 리스너 인프라는 3 transport 모두에 존재하지만, listener 기반 ACL(scope=local_node) 및 restricted 매칭에 쓰이는 `m_iListenerId` 전파는 UDP 수신 경로의 thread-local(`t_iCurrentListenerId`)로 설정된다. 즉 listener_id 기반 ACL scope=local_node 매칭은 UDP 수신에 적용된다.

## 10. outbound 자기 주소 동적 선택 (CspAddressing)

§9 의 inbound listener 식별에 대응하여, **발신 SIP 메시지의 자기 주소**(Via/From/Contact/source)도 단일 primary 값이 아니라 수신 맥락·route·서비스에 따라 동적으로 고른다. `csp/CspAddressing.{h,cpp}` 의 헬퍼 4종이 담당한다.

| 헬퍼 | 선택 규칙 | 적용 위치 |
|------|-----------|-----------|
| `GetLocalSipAddress(inbound_listener_id)` / `GetLocalSipPort(...)` | 수신 listener id(>0)면 `LocalNodeMap.GetByIntId` 의 bind_ip/port 를 응답·Contact 주소로 사용 | REGISTER 200 OK Contact, 302 Contact, out-of-dialog NOTIFY 등 |
| `GetLocalSipAddressForOutbound(proto, edge)` | protocol+edge → protocol-only → primary 의 3단 폴백 | 발신 From/Call-ID host |
| `GetServerIdentityForService(kind)` | access_service 의 `server_identity_uri` 가 있으면 그대로, 없으면 `sip:cspserver@{service.domain}` 조립(매칭 실패 시 `sip:cspserver@{LocalIp}`) | 서비스별 서버 identity |

- **B2BUA B-leg outbound 자기 주소**: route 의 `local_node_ref` → bind_ip/port → `clsRoute.m_strOutboundLocalIp` / `m_iOutboundLocalPort` 로 전달.
- **psip 자동 Contact**: 수신 `m_iListenerId` 의 bind_ip:port 를 사용(모든 응답 포함).
- **TCP/TLS outbound source bind**: `TcpConnectFrom(srcIp, ...)`, client thread 의 `m_strSourceIp`(비면 OS 기본)로 발신 소스 주소를 지정할 수 있다.

이 동작은 멀티-listener / 멀티-realm / 피어링 환경에서 inbound 와 outbound 주소의 대칭성을 보장한다.
