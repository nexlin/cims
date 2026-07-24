# SIP 설정 모델 (Local/Remote Node + Rule/RuleSet + Policy)

> telco IBCF 계열의 **LocalNode / RemoteNode / Route / RouteSet + Rule / RuleSet + Policy**
> 9-collection 모델. Access(UE facing) 와 Peering(IMS 연동) 을 하이브리드로 구분.
>
> 이 문서는 **데이터 모델/의미** 만 다루며, 저장/반영 경로는 `sip_runtime_config.md`,
> 패키지 형식은 `package_and_template.md` 를 참조.

---

## 0. 핵심 개념 (Telco IBCF 관례)

```
Local Node         CSP 가 수신/송신하는 엔드포인트(bind 포트)
Remote Node        외부 피어(IMS/PBX 서버)의 접속 정보 (순수 transport)
Route              Local Node ↔ Remote Node 간 1:1 연결 파라미터
                   (auth, proxy, REGISTER 옵션, 동시호 제한 등)
                   (local_node_ref, remote_node_ref) unique
Route Set          Route 여러 개 묶어 분배/failover 정책 적용 (cluster)
Rule               SIP 메시지 1개 필드에 대한 단일 조건
Rule Set           Rule 들을 flat AND/OR 로 조합
Routing Policy     Rule Set match → Route Set 또는 Access Service 로 전달
ACL Policy         Rule Set match → allow/deny (scope: global/local_node/route/route_set)
Access Service     UE 가 직접 붙는 서비스(voip/ptt) — domain + auth_realm + 허용 LN
```

재사용성이 원칙이다:
- 같은 Remote Node 가 여러 Route 에 속할 수 있다 (LN 만 다른 경우)
- 같은 Rule 이 Routing 과 ACL 양쪽 Rule Set 에 속할 수 있다 (`tags[]` 로 용도 힌트)
- 같은 Route 가 여러 Route Set 에 속할 수 있고, 각 Set 마다 다른 priority/weight

---

## 1. 엔티티 관계도

```
 ┌──────────────┐          ┌──────────────┐
 │  LocalNode   │          │  RemoteNode  │   (순수 transport)
 │ (listener)   │          │              │
 │  edge:       │          │              │
 │   access     │          │              │
 │   peering    │          │              │
 │   mgmt       │          │              │
 └──────┬───────┘          └──────┬───────┘
        │                         │
        │ 1             1         │
        └──────►┌───────────┐◄────┘
                │   Route   │  (LN, RN) unique
                │  + auth   │
                │  + proxy  │
                │  + limits │
                └─────┬─────┘
                      │ N
                      ▼
               ┌────────────┐    distribution_policy:
               │  RouteSet  │      failover | round_robin |
               │            │      weighted | hash_by_caller
               └─────┬──────┘    health_check (OPTIONS ping)
                     │
                     │ target_ref
                     ▼
          ┌──────────────────────┐
          │   RoutingPolicy      │── match ── RuleSet ── N rules
          │   priority 순 평가    │                       (tags)
          │   target: route_set  │
          │         | access_svc │
          │         | reject     │
          └──────────────────────┘

          ┌──────────────────────┐
          │   AclPolicy          │── match ── RuleSet  (Rule 공유)
          │   scope:             │
          │     global           │
          │     local_node       │
          │     route            │
          │     route_set        │
          │   action: allow/deny │
          └──────────────────────┘

          ┌──────────────────────────┐
          │   AccessService          │── listeners[] → LocalNode
          │   kind: voip | ptt       │        (restricted 일 때)
          │   domain, auth_realm     │
          │   inbound_policy         │
          └──────────────────────────┘
```

---

## 2. 각 엔티티

### 2-1. LocalNode

CSP 가 수신하는 bind 포트. `edge` 분류를 가진다.

| 필드 | 의미 |
|---|---|
| `edge` | `access` (UE 수신) / `peering` (IMS 피어링) / `mgmt` (관리) |
| `bind_ip`, `bind_port`, `protocol` | 기본 transport |
| `tls_*` | TLS 전용 (protocol=TLS 일 때) |
| `tags[]` | 자유 태그 (예: `volte`, `peering-kt`) |

### 2-2. RemoteNode

피어의 접속 정보만 보관. **auth 정보는 여기 두지 않는다** (Route 에서 관리).

| 필드 | 의미 |
|---|---|
| `ip`, `port`, `protocol` | transport |
| `remote_domain` | SIP URI host (outgoing Request-URI/To 의 host 로 사용) |
| `srv_lookup`, `dns_fallback`, `tls_verify` | 고급 transport 옵션 |

### 2-3. Route — (LocalNode, RemoteNode) unique

LN 과 RN 을 묶은 실제 연결. auth 및 outbound 파라미터는 전부 여기.

| 필드 | 의미 |
|---|---|
| `local_node_ref`, `remote_node_ref` | 필수. pair unique. |
| `outbound_proxy_ip`, `outbound_proxy_port` | RN 앞에 proxy 가 있을 때 |
| `register_to_remote`, `register_expires` | trunk REGISTER 가 필요한 경우 |
| `auth_user`, `auth_password`, `auth_realm` | REGISTER/challenge 대응용 |
| `max_concurrent_calls`, `cps_limit` | 용량 제한 |

### 2-4. RouteSet — Route 묶음 + 분배 정책

Peering cluster (1:1:1, 1 active + N standby 등) 를 표현.

| 필드 | 의미 |
|---|---|
| `distribution_policy` | `failover` / `round_robin` / `weighted` / `hash_by_caller` |
| `members[].route_ref` | 포함할 Route |
| `members[].priority` | failover 순서 (낮을수록 우선) |
| `members[].weight` | weighted 분배 비율 |
| `health_check_*` | OPTIONS ping 주기 / dead 임계 / recovery 임계 |
| `fallback_policy` | 전체 dead 시 `reject` / `next_policy` |

같은 Route 가 다른 RouteSet 에 다른 priority 로 속할 수 있다.

### 2-5. Rule — 원자 조건

SIP 메시지의 한 필드 + 연산자 + 값.

지원 필드(1차):

```
from_uri_host   from_uri_user
to_uri_host     to_uri_user
req_uri_host    req_uri_user
src_ip          dst_ip
user_agent      method
p_asserted_identity   via_host
```

지원 연산자(1차):

```
eq        ne        prefix    suffix    contains
regex     in_cidr   in_list   exists    not_exists
```

`tags[]` 가 중요: 같은 Rule 이 다양한 RuleSet 에 재사용될 때 용도를 구분한다.

권장 태그 규약:

| 카테고리 | 값 | 의미 |
|---|---|---|
| 용도 | `acl`, `routing`, `transform`, `inbound-filter`, `audit` | RuleSet 의 맥락 힌트 |
| 도메인 | `volte`, `ptt`, `ibcf`, `peering-<name>` | 어떤 서비스군에 관계 |
| 환경 | `prod`, `test`, `experimental` | 배포 스테이지 |

UI 는 tag filter chip 으로 목록을 필터. Policy 가 다른 용도 태그의 RuleSet 을 참조해도 **경고만** 띄우며 막지는 않는다 (재사용 유지).

### 2-6. RuleSet — flat AND/OR 조합

`combinator` = `AND` | `OR`. 1차 버전은 중첩 없음.

| 필드 | 의미 |
|---|---|
| `combinator` | 전체 조합 방식 |
| `members[].rule_ref` | 포함할 Rule |
| `members[].negate` | 해당 Rule 의 결과 반전 (NOT) |

평가 규칙:
- `AND`: 모든 member 가 true (negate 고려) → RuleSet true
- `OR`: 하나라도 true → RuleSet true
- member 0개 → `true` (catch-all 용이하게)

### 2-7. RoutingPolicy — match → target

Rule Set 이 match 하면 target 으로 호를 routing.

| 필드 | 의미 |
|---|---|
| `priority` | 낮을수록 먼저 평가. 첫 match 적용. |
| `match_rule_set_ref` | 비우면 항상 match (catch-all) |
| `target_type` | `route_set` / `access_service` / `reject` |
| `target_ref` | target_type 에 따른 참조 대상 |
| `transform_rule_set_refs[]` | **예약 필드** — 1차에서 런타임 미구현 |
| `fail_action` | target=route_set 이 모두 dead 일 때 `reject` / `next_policy` |

**런타임 적용 흐름:**

RoutingPolicy 매칭 결과가 실제 메시지 forward 로 반영되는 경로:

```
ModuleDispatcher
  ├─ routing_policies 매칭 (RoutingPolicyEngine)
  ├─ RouteMap / RemoteNodeMap 조회 → RemoteNode.protocol(transport) 확인
  ├─ PendingRouteMap (Call-ID 키) 에 적재
  └─ B2BUA 가 B-leg dialog 생성 시 그 peer ip/port/transport 로 forward
```

- 내부 가입자(등록 단말) 콜 라우팅은 별도 경로로, `AddRoute(ip, port, transport)` 로 Route 헤더를 직접 주입한다.

### 2-8. AclPolicy — match → allow/deny

Rule Set 이 match 하면 action.

| 필드 | 의미 |
|---|---|
| `priority` | 낮을수록 먼저 평가 |
| `match_rule_set_ref` | 필수 |
| `scope` | `global` / `local_node` / `route` / `route_set` |
| `scope_ref` | scope ≠ global 일 때 해당 collection 의 name |
| `action` | `allow` / `deny` |

Rule 은 Routing 과 ACL 이 공유. RuleEvaluator 하나가 양쪽을 처리.

### 2-9. AccessService — UE 서비스

UE 가 직접 REGISTER 하는 서비스 도메인.

| 필드 | 의미 |
|---|---|
| `kind` | `voip` / `ptt` (IBCF 는 이 collection 에 없음) |
| `domain` | IMPU/IMPI 조립용. Digest username = `imsi@<domain>` |
| `auth_realm` | 비우면 domain 상속 |
| `inbound_policy` | `any` / `restricted` |
| `allowed_local_node_refs[]` | restricted 일 때 허용 LocalNode 목록 |
| `server_identity_uri` | (optional) OPTIONS/keepalive 등에서 서버 From identity 를 IMS 규격(`sip:cspserver@<domain>`)으로 조립할 때 사용. 미지정 시 service.domain 기반 자동 조립. 저장 위치는 `access_services.jsonl` file-store (SQL 테이블 아님). |
| `priority` | 같은 domain 중복 시 먼저 매칭될 순서 |

---

## 3. 인증 흐름 (Access 경로)

Access Service 가 이 흐름의 "service" 역할.

```
UE → REGISTER
  Authorization: Digest username="<imsi>@<access_service.domain>"
                 realm="<access_service.auth_realm or domain>"
                 response=MD5(...)

CSP (CscfModule):
  1. From URI user + host → AccessServiceMap.GetByDomain(host)
     (inbound_policy=restricted 이면 수신 Local Node 가 allowed_local_node_refs 에 있는지 확인)
  2. voip_subscriptions/ptt_subscriptions 에서 service_id 로 가입자 조회
  3. 기대 username = imsi + "@" + service.domain 비교
  4. HA1 = MD5(username : realm : password)
  5. 일치 → 200 OK + Contact 저장 + (binding_key → service_id) 맵 유지
```

domain/auth_realm 의 SOT 는 `access_services.*`.

---

## 4. 서비스 판별 우선순위 (INVITE 등 후속 요청)

From URI 단독 매칭은 **fallback 으로 강등**. IMS 표준을 참고한 계층적 판별:

```
1. 수신 Local Node 가 restricted AccessService 1개와만 연결
      → 그 서비스 확정. 끝.

2. src_addr/Call-ID → REGISTER binding 조회 (CspUserMap)
      → 인증된 UE 면 binding.service_id 확정. 끝.

3. 수신 Local Node 가 edge=peering + 어떤 RoutingPolicy 가 선평가
      → 해당 RoutingPolicy.target 기반 라우팅 (service 개념 없이 RouteSet 로 직행)

4. From URI host 가 AccessService.domain 과 일치 (best-effort fallback)

5. Request-URI host 가 AccessService.domain 과 일치

6. 매칭 실패 → 403 reject 또는 RoutingPolicy 의 catch-all
```

1/2 가 주 경로. 3 은 IBCF incoming. 4/5 는 의심스러운 fallback (로그에 `svc_source=from_header_fallback` 표식).

---

## 5. 배치 패턴

### 5-1. 최소 (테스트)

```
LocalNode   :5060 UDP (access)
RemoteNode  없음
AccessService volte-test (domain=csp, allowed LN = :5060)
```

### 5-2. 표준 (VoLTE + PTT + 1 peering)

```
LocalNodes:
  lb-access-udp  :5060 UDP  edge=access
  lb-access-tls  :5061 TLS  edge=access
  lb-peering     :5070 UDP  edge=peering

RemoteNodes:
  kt-sbc-1 10.0.0.1:5060
  kt-sbc-2 10.0.0.2:5060
  kt-sbc-3 10.0.0.3:5060

Routes:
  r-kt-1  (lb-peering, kt-sbc-1)
  r-kt-2  (lb-peering, kt-sbc-2)
  r-kt-3  (lb-peering, kt-sbc-3)

RouteSets:
  rs-kt  round_robin  [r-kt-1 w=1, r-kt-2 w=1, r-kt-3 w=1]

Rules:
  rule-kt-domain  to_uri_host suffix "kt.co.kr"

RuleSets:
  rs-kt-outbound  AND [rule-kt-domain]

RoutingPolicies:
  rp-kt-out  priority=50  match=rs-kt-outbound  target=route_set:rs-kt
  rp-default priority=1000 match=""  target=access_service:volte-main (catch-all)

AccessServices:
  volte-main  voip  domain=ims.mnc001... allowed_ln=[lb-access-udp, lb-access-tls]
  ptt-main    ptt   domain=ptt.mnc001... allowed_ln=[lb-access-udp]
```

### 5-3. 다중 peering + restricted access

위 구성 + peering-skt 추가. AclPolicy 로 특정 CIDR 만 허용.

---

## 5a. 식별자 (LocalNode / listener_id)

수신 엔드포인트의 3가지 식별자:

- **`LocalNode.id`** (UUID 문자열) — jsonl 영구 PK
- **`LocalNode.name`** (문자열) — 다른 collection 의 참조 대상
- **`listener_id`** (양의 int) — psip 내부 소켓 id. `CspUuidToIntId(LocalNode.id)` 로 파생

`AccessService.allowed_local_node_refs[]` 는 **name 배열**. 내부 파생 필드 `listeners[]` 는 **int
배열** (psip 에 맞춘 해시값). 런타임 매칭은 listener_id 기준.

수신 SIP 메시지의 `CSipMessage.m_iListenerId` 가 이 int id. ACL scope=local_node 와 restricted 정책
모두 이 값으로 매칭.

## 6. 서비스 판별 소스 코드 위치

| 기능 | 클래스 | 비고 |
|---|---|---|
| Rule 평가 엔진 | `CRuleEvaluator` | ACL/Routing 공용 |
| Rule/RuleSet 캐시 | `CspConfigCache` (CACHE_RULE, CACHE_RULE_SET) | jsonl 로더 |
| Routing 결정 | `CRoutingPolicyEngine` | |
| ACL 결정 | `CAclPolicyEngine` | |
| RemoteNode/Route/RouteSet 캐시 | `CspRemoteNodeMap`, `CspRouteMap`, `CspRouteSetMap` | |
| Access service 캐시 | `CspAccessServiceMap` (voip/ptt 만) | |

---

## 7. csp.json 구성

- `Setup.Sip.AuthRealm` / `Setup.Realm[]` 없음 — realm/domain 의 SOT 는 AccessService.
- `Setup.Roles` 사용.
- `Setup.MediaServer`(구 `Setup.RtpRelay` 는 fallback), `Setup.Database`, `Setup.Log`, `Setup.Monitor`, `Setup.Security`, `Setup.ServiceLogging`, `Setup.Cdr`, `Setup.DataFolder`, `Setup.SystemId` 사용.

---

## 8. 관련 파일

### CSP (C++)

| 파일 | 역할 |
|------|------|
| `csp/CspConfigCache.{h,cpp}` | 9 collection 로딩 (entity enum) |
| `csp/CspLocalNodeMap.{h,cpp}` | LocalNode 캐시 |
| `csp/CspRemoteNodeMap.{h,cpp}` | RemoteNode 캐시 |
| `csp/CspRouteMap.{h,cpp}` | Route 캐시 + (LN, RN) 조회 |
| `csp/CspRouteSetMap.{h,cpp}` | RouteSet + 분배 상태 |
| `csp/CspRuleEvaluator.{h,cpp}` | Rule/RuleSet 평가 엔진 |
| `csp/CspRoutingPolicyEngine.{h,cpp}` | routing 결정 |
| `csp/CspAclPolicyEngine.{h,cpp}` | ACL 결정 |
| `csp/CspAccessServiceMap.{h,cpp}` | Access service (voip/ptt) |
| `csp/SipServerSetup.{h,cpp}` | Setup.Sip 파싱 |

### Console

| 파일 | 역할 |
|------|------|
| `ems/core/console/src/components/module/ModuleConfigEditor.tsx` | 9개 탭 렌더, tag filter chip, ref 필드 dropdown |

### 문서

| 파일 | 역할 |
|------|------|
| `docs/design/features/sip_service_model.md` | 이 문서 |
| `docs/design/features/sip_runtime_config.md` | collection 목록 / Init 시그니처 |

---

## 9. 미구현/예약

| 항목 | 예정 |
|------|------|
| RuleSet 중첩 (tree AND/OR/NOT) | 2차 |
| `routing_policies.transform_rule_set_refs` 런타임 적용 (메시지 변환) | 2차 |
| 헬스체크 `invite_response` 모드 | 2차 (1차는 `options_ping` 만) |
| `hash_by_caller` 분배 | 2차 |
| Rule field: `record_route`, `p_charging_vector` 등 | 필요시 추가 |
