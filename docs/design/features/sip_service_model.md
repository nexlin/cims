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
Access Service     UE 가 직접 붙는 서비스(volte/ptt) — domain + auth_realm + 허용 LN
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
          │   kind: volte | ptt      │        (restricted 일 때)
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
| `register_to_remote`, `register_expires` | trunk REGISTER 가 필요한 경우. **런타임 미구현** — 값만 보관하며 REGISTER 를 보내지 않는다 (§9) |
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
| `health_check_*` | OPTIONS ping 주기 / dead 임계 / recovery 임계. **런타임 미구현** — 프로브를 보내지 않아 모든 Route 가 alive 로 취급된다 (§9) |
| `fallback_policy` | 전체 dead 시 `reject` / `next_policy` |

같은 Route 가 다른 RouteSet 에 다른 priority 로 속할 수 있다.

선택 규칙 (`CCspRouteSetMap::SelectRoute`):

- `failover` — `priority` 오름차순으로 첫 alive Route
- `round_robin` — 커서 순환, dead 는 건너뜀
- `weighted` — weight 비율 분배 (deficit-round-robin 근사)
- `hash_by_caller` — `{from_uri_user}@{from_uri_host}` 해시로 고정, dead 면 다음 member 순회
- `weight: 0` 인 member 는 모든 정책에서 **분배 제외**
- 참조가 깨진 member 는 RouteSet 을 유지한 채 선택에서만 skip

### 2-5. Rule — 원자 조건

SIP 메시지의 한 필드 + 연산자 + 값.

지원 필드:

```
from_uri_host   from_uri_user
to_uri_host     to_uri_user
req_uri_host    req_uri_user
src_ip          user_agent
method
```

스키마(`config_template.json`)에는 `dst_ip` / `p_asserted_identity` / `via_host` 도 열거되어 있으나
`MessageCtx` 에 채워지지 않아 항상 빈 값으로 평가된다 (§9). `dst_ip` 로 수신 인터페이스를 구분하려면
ACL `scope=local_node` 를 쓴다.

지원 연산자:

```
eq        ne        prefix    suffix    contains
regex     in_cidr   in_list   exists    not_exists
```

- `regex` — `std::regex` **ECMAScript** 문법 (`\d`, `\w`, 비탐욕 `*?` 사용 가능). 컴파일 실패 시 false + ERROR 로그
- `in_cidr` — IPv4 전용
- `in_list` — 콤마 구분 문자열
- `exists` / `not_exists` — 값이 빈 문자열이면 "없음"으로 본다

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
- Rule 이 `enabled=false` → 그 member 는 **false** (skip 이 아니다). `AND` 셋에서는 셋 전체가 false 가 된다
- 존재하지 않는 `rule_ref` → 그 member 는 false. 로드 시 ERROR 로그로 알린다
- 참조하는 RuleSet 이름 자체가 없으면 → **no-match**(false). 반면 참조가 **빈 문자열**이면 catch-all(true) 이므로
  Routing/ACL 정책에서 오타와 catch-all 은 정반대로 동작한다

### 2-7. RoutingPolicy — match → target

Rule Set 이 match 하면 target 으로 호를 routing.

| 필드 | 의미 |
|---|---|
| `priority` | 낮을수록 먼저 평가. 첫 match 적용. |
| `match_rule_set_ref` | 비우면 항상 match (catch-all) |
| `target_type` | `route_set` / `access_service` / `reject` |
| `target_ref` | target_type 에 따른 참조 대상 |
| `transform_rule_set_refs[]` | **예약 필드** — 런타임 미구현 |
| `fail_action` | target=route_set 이 모두 dead 일 때 `reject` / `next_policy` |

`target_type` 별 런타임 동작:

| target_type | 동작 |
|---|---|
| `route_set` | RouteSet 에서 Route 선택 → `PendingRouteMap` 적재 → B2BUA B-leg 가 그 peer 로 forward |
| `reject` | 즉시 403 |
| `access_service` | 매칭·로그까지만. 이후는 기존 TAS/B2BUA 경로가 처리한다 (명시적 분기 없음, §9) |

**런타임 적용 흐름:**

RoutingPolicy 매칭 결과가 실제 메시지 forward 로 반영되는 경로:

```
ModuleDispatcher
  ├─ routing_policies 매칭 (RoutingPolicyEngine)
  ├─ RouteMap / RemoteNodeMap 조회 → RemoteNode.protocol(transport) 확인
  ├─ PendingRouteMap (Call-ID 키) 에 적재
  └─ B2BUA 가 B-leg dialog 생성 시 그 peer ip/port/transport 로 forward
```

- 평가 대상은 **INVITE 뿐**이며, **PTT 그룹 판정 이후**에 실행된다. 따라서 PTT 그룹 대상 INVITE 는
  RoutingPolicy 로 외부 peer 로 돌릴 수 없다 (그룹 경로가 먼저 확정된다).
- 내부 가입자(등록 단말) 콜 라우팅은 별도 경로로, `AddRoute(ip, port, transport)` 로 Route 헤더를 직접 주입한다.

### 2-8. AclPolicy — match → allow/deny

Rule Set 이 match 하면 action.

| 필드 | 의미 |
|---|---|
| `priority` | 낮을수록 먼저 평가. 같은 priority 면 name 사전순. 첫 match 적용 |
| `match_rule_set_ref` | **필수** — 비어 있으면 그 정책은 로드에서 제외된다 (ERROR 로그) |
| `scope` | `global` / `local_node` / `route` / `route_set` |
| `scope_ref` | scope ≠ global 일 때 해당 collection 의 name |
| `action` | `allow` / `deny` |

Rule 은 Routing 과 ACL 이 공유. RuleEvaluator 하나가 양쪽을 처리.

적용 범위와 기본값:

- ACL 은 **모든 inbound SIP 요청**에 대해 `ModuleDispatcher::RecvRequest` 진입 직후 평가된다
  (REGISTER 포함). deny 면 403 을 보내고 이후 처리를 하지 않는다.
- **매칭되는 정책이 없으면 기본 ALLOW.** 화이트리스트로 쓰려면 "허용 대상이 아님 → deny" 를
  `negate` 로 표현한다.
- 동작하는 scope 는 `global` 과 `local_node` 다. `route` / `route_set` 은 inbound 시점에 outbound
  route 가 아직 결정되지 않아 빈 문자열로 매칭되므로 실질 미동작이다 (§9).

### 2-9. AccessService — UE 서비스

UE 가 직접 REGISTER 하는 서비스 도메인.

| 필드 | 의미 |
|---|---|
| `kind` | `volte` / `ptt` (IBCF 는 이 collection 에 없음). 다른 값이면 해당 레코드를 skip + ERROR 로그 |
| `domain` | IMPU/IMPI 조립용. Digest username = `imsi@<domain>`. 비면 레코드 제외 |
| `auth_realm` | 비우면 domain 상속 |
| `inbound_policy` | `any` / `restricted` |
| `allowed_local_node_refs[]` | restricted 일 때 허용 LocalNode 목록 |
| `server_identity_uri` | (optional) OPTIONS/keepalive 등에서 서버 From identity 를 IMS 규격(`sip:cspserver@<domain>`)으로 조립할 때 사용. 미지정 시 service.domain 기반 자동 조립. 저장 위치는 `access_services.jsonl` file-store (SQL 테이블 아님). |
| `media_nat_mode` | `off`(기본) / `auto` / `force` — 단말 NAT 미디어 정책. leg 별 판정 결과를 CMP 자원할당 명령에 전달 ([ue_nat_traversal.md](ue_nat_traversal.md) §4) |
| `latch_ip_guard` | `strict`(기본) / `off` — NAT latch 소스 IP 를 그 leg 의 SIP 실소스로 제한 (스푸핑 방어) |
| `priority` | 같은 domain 중복 시 먼저 매칭될 순서 |

---

## 3. 인증 흐름 (Access 경로)

Access Service 가 이 흐름의 "service" 역할.

```
UE → REGISTER (Request-URI: sip:<domain>)
  │
  ├─ Authorization 헤더 없음
  │   └─ 401 + WWW-Authenticate, realm = Request-URI host
  │
  └─ Authorization 헤더 있음  (CCscfModule::CheckAuthorization)
      1. nonce 검증 (NonceMap) — 미발견이면 401 + stale=true
      2. From URI user → CspUserMap 에서 가입자 조회
      3. 가입자의 service_ref → access_services 를 name 으로 조회
         (service_ref 가 비어 있으면 그 자리에서 거부)
      4. inbound_policy=restricted 면 수신 listener_id 가 svc.listeners[] 에 있는지 확인
      5. 기대 username = "{가입자 imsi}@{svc.domain}" 를 Authorization username 과 문자열 비교
         (imsi 가 비어 있으면 거부)
      6. HA1 = MD5(username : Authorization.realm : password) → response 검증
      7. 성공 → 200 OK + Contact 저장
```

- **REGISTER 챌린지의 realm 은 Request-URI host** 다. 즉 단말이 `sip:<domain>` 으로 보낸 도메인이
  그대로 realm 이 된다.
- `auth_realm` (없으면 `domain`, = `EffectiveRealm()`) 은 **Request-URI 기반 realm 이 없는 챌린지의
  fallback** 으로 쓰인다 (SUBSCRIBE 등 비-REGISTER 경로). 이때 대상은 `kind=volte` 인 첫 서비스다.
- 서비스 조회는 `domain` 이 아니라 **가입자의 `service_ref` (= access_service name)** 로 한다.
  `domain` 은 기대 username 조립과 realm fallback 에 쓰인다.

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
  volte-main  volte  domain=ims.mnc001... allowed_ln=[lb-access-udp, lb-access-tls]
  ptt-main    ptt    domain=ptt.mnc001... allowed_ln=[lb-access-udp]
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
| Access service 캐시 | `CCspServiceMap` (`gclsServiceMap`, volte/ptt 만) | |

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
| `csp/CspServiceMap.{h,cpp}` | Access service (volte/ptt) — `CCspServiceMap` |
| `csp/CspListenerManager.{h,cpp}` | local_nodes → psip 리스너 add/remove (무중단 rebind) |
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

스키마에는 존재하지만 런타임이 아직 소비하지 않는 항목들. 설정해도 동작에 영향이 없다.

| 항목 | 상태 |
|------|------|
| RouteSet 헬스체크 (`health_check_*`) | 프로브 송신 코드가 없다. `RouteRuntime.alive` 가 항상 true 이므로 dead peer 도 계속 선택된다. 손절체는 `routes.enabled=false` 로 한다 |
| `routes.register_to_remote` / `register_expires` | 트렁크 REGISTER 워커 미구현 — 값만 보관 |
| Rule field `dst_ip` / `p_asserted_identity` / `via_host` | `MessageCtx` 에 채워지지 않아 항상 빈 값. 수신 인터페이스 구분은 ACL `scope=local_node` 로 대체 |
| `routing_policies.target_type=access_service` | 매칭·로그까지만. 이후는 기존 TAS/B2BUA 경로가 처리 |
| ACL `scope=route` / `route_set` | inbound 시점에 outbound route 미결정 → 빈 문자열로 매칭되어 실질 미동작 |
| `routing_policies.transform_rule_set_refs` (메시지 변환) | 예약 필드 |
| RuleSet 중첩 (tree AND/OR/NOT) | 2차 |
| 헬스체크 `invite_response` 모드 | 2차 |
| Rule field: `record_route`, `p_charging_vector` 등 | 필요시 추가 |
| listener_id 전파 | UDP 수신 경로만. TCP/TLS 는 `-1` → ACL `scope=local_node` 와 `restricted` 는 UDP 리스너에서만 매칭된다 |
