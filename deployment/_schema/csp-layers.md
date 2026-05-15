# CSP 설정 layer 모델

CSP 의 설정은 **csp.json (시스템 기본) + 10개 jsonl (동적)** 의 다층 구조. 운영 시 jsonl 이 SOT 이고 csp.json 은 fallback. 환경별 자동화 (`deployment/bin/render.py`) 가 env.yaml + scenario.yaml → 이 layer 들의 값을 산출.

## 1. Layer 전체

| # | Layer | 파일 | 역할 | SOT 우선순위 |
|---|---|---|---|---|
| 0 | **_infra** | `csp/config/csp.json` (`Setup.*`) | 시스템 기본 — UDP/TCP/TLS thread, 인증서, DB, Roles, MediaServer endpoint | local_nodes 비어있을 때 fallback |
| 1 | **local_nodes** | `csp/config/local_nodes.jsonl` | **CSP 자기 자신 SIP listener** (bind_ip/bind_port/protocol). `is_primary=true` 가 가장 강함 | ⭐ 최상위 (primary 있으면 _infra 무시) |
| 2 | **remote_nodes** | `csp/config/remote_nodes.jsonl` | peer endpoint (다른 csp / IBCF / IP-PBX) | - |
| 3 | **access_services** | `csp/config/access_services.jsonl` | Realm × kind (volte/ptt) 매핑 — 인증/등록 가능 도메인 정의 | - |
| 4 | **routes** | `csp/config/routes.jsonl` | 라우팅 rule — match → target (local_node / remote_node) | - |
| 5 | **route_sets** | `csp/config/route_sets.jsonl` | routes 묶음 | - |
| 6 | **routing_policies** | `csp/config/routing_policies.jsonl` | 라우팅 정책 (Match → Route Set 매핑) | - |
| 7 | **rules** | `csp/config/rules.jsonl` | 단위 rule — ACL/normalize/header manipulation | - |
| 8 | **rule_sets** | `csp/config/rule_sets.jsonl` | rules 묶음 | - |
| 9 | **acl_policies** | `csp/config/acl_policies.jsonl` | ACL 정책 (rule_sets 와 결합) | - |

소스: `csp/CspConfigCache.cpp` line 21~25 (jsonl 파일명 배열).

## 2. local_nodes 의 R1 우선순위 (가장 중요)

소스: `csp/CspServer.cpp:121-160`, `csp/CspLocalNodeMap.cpp`

```
1. local_nodes.jsonl 의 enabled=true && is_primary=true && protocol="UDP" row
   ├─ bind_ip / bind_port → gclsSetup.m_strLocalIp / m_iUdpPort 덮어씀
   └─ bind_ip 가 "0.0.0.0" / "" / "::" 면 GetLocalIp() 로 자동 치환 (host 의 default outgoing IP)
2. primary 없음 → _infra Setup.Sip.LocalIp / UdpPort 그대로 사용 ← 현재 NetNS 의 상태
3. Setup.Sip.LocalIp 도 비어있으면 GetLocalIp() 호출
```

TCP / TLS 도 동일 (`GetPrimaryByProtocol("TCP"/"TLS")`).

⚡ **이슈 #1 의 진짜 원인** (어제 발견):
> csp 가 `Setup.Sip.LocalIp=0.0.0.0` 무시하고 mgmt IP 로 bind

= local_nodes 가 비어있어서 _infra fallback → `Setup.Sip.LocalIp="0.0.0.0"` 이지만 SIP stack 의 bind 단계에서 "0.0.0.0" 이 host 의 default route 의 outgoing NIC (= mgmt) 으로 매핑. 결과: VIP 10.0.1.13 으로 SIP 트래픽 수신 불가.

**해결**: local_nodes.jsonl 에 명시적 primary row 추가:
```json
{"id":"csp-main-udp", "name":"csp-main", "bind_ip":"10.0.1.13", "bind_port":5060, "protocol":"UDP", "is_primary":true, "enabled":true, "edge":"access"}
```

## 3. 각 layer 의 schema (필드)

### 1. local_nodes (CspLocalNodeMap.cpp:34-49)

```json
{
  "id":            "csp-main-udp",      // required, 식별자
  "name":          "csp-main",          // human-friendly
  "edge":          "access",            // access | core
  "bind_ip":       "0.0.0.0",           // bind IP. 0.0.0.0/::/empty → GetLocalIp()
  "bind_port":     5060,                // bind port
  "protocol":      "UDP",               // UDP | TCP | TLS
  "thread_count":  2,                   // 0 = fallback to _infra
  "enabled":       true,
  "is_primary":    true,                // protocol 별 primary 1개. csp identity 결정
  "tls_cert_path": "",
  "tls_key_path":  "",
  "tls_ca_path":   "",
  "tls_verify_peer": false,
  "max_connections": 0,
  "tags":          [],
  "note":          ""
}
```

### 2. remote_nodes (CspRemoteNodeMap.cpp:34-45)

```json
{
  "id":            "ibcf-trunk-1",
  "name":          "IP-PBX Trunk #1",
  "ip":            "203.0.113.10",
  "port":          5060,
  "protocol":      "UDP",               // UDP | TCP | TLS
  "remote_domain": "pbx.example.com",
  "srv_lookup":    false,               // SRV record lookup
  "dns_fallback":  true,
  "tls_verify":    false,
  "enabled":       true,
  "tags":          [],
  "note":          ""
}
```

### 3. access_services (csp_runtime.py `_DOM_SERVICE = sip_service`)

```json
{
  "name":             "volte-basic",
  "kind":             "volte",          // volte | ptt
  "domain":           "ims.mnc033.mcc450.3gppnetwork.org",
  "auth_realm":       "ims.mnc033.mcc450.3gppnetwork.org",
  "inbound_policy":   "default",        // routing_policies.id 참조
  "outbound_policy":  null,
  "priority":         100,
  "enabled":          true,
  "listener_ids":     ["csp-main-udp"], // local_nodes.id 참조
  "note":             ""
}
```

### 4-6. routes / route_sets / routing_policies (csp_runtime.py `_DOM_ROUTE = routing_rule`)

```json
// routes.jsonl
{
  "id":         "default-ims",
  "match":      {
    "domain":   "ims.mnc033.mcc450.3gppnetwork.org",
    "user_pattern": "+82*"
  },
  "transform":  {
    "rewrite_host": null
  },
  "target":     "csp-main-udp",         // local_nodes.id OR remote_nodes.id
  "priority":   100,
  "enabled":    true
}

// route_sets.jsonl
{
  "id":      "default",
  "routes":  ["default-ims", "default-ptt"],
  "enabled": true
}

// routing_policies.jsonl
{
  "id":         "default-inbound",
  "match":      { "direction": "inbound" },
  "route_set":  "default",
  "priority":   100,
  "enabled":    true
}
```

### 7-9. rules / rule_sets / acl_policies

```json
// rules.jsonl
{
  "id":      "allow-internal",
  "match":   { "source_cidr": "10.0.0.0/8" },
  "action":  "allow",                   // allow | deny | normalize
  "enabled": true
}

// rule_sets.jsonl
{
  "id":      "default",
  "rules":   ["allow-internal"],
  "enabled": true
}

// acl_policies.jsonl (csp_runtime.py `_DOM_ACCESS = routing_access_list`)
{
  "id":         "default-acl",
  "rule_set":   "default",
  "applied_to": ["csp-main-udp"],       // local_nodes.id
  "priority":   100,
  "enabled":    true
}
```

## 4. 의존성 graph

```
            ┌─────────────────────────────┐
            │  csp.json (Setup.* = _infra) │
            │  ─ Roles (CSCF/TAS/PTT/IBCF) │
            │  ─ MediaServer endpoint      │
            │  ─ Database                  │
            └────────┬────────────────────┘
                     │ FALLBACK
                     ↓
            ┌─────────────────────────────┐
   ⭐ R1 ──→│  local_nodes (primary)      │  ←─ SIP listener 결정 SOT
            └────────┬────────────────────┘
                     ↑
            referenced by
                     │
   ┌─────────────────┼──────────────────┐
   │                 │                  │
access_services    routes ←────── route_sets ←── routing_policies
  (인증 realm)        │
                  target ──→ remote_nodes
                                 │
                            (외부 peer)

   rules ←──── rule_sets ←──── acl_policies ──→ applied_to (local_nodes)
```

## 5. 빈 jsonl 일 때 동작표

| jsonl 빈 상태 | csp 동작 |
|---|---|
| local_nodes.jsonl | _infra Setup.Sip.* fallback (mgmt IP / 5060). **이슈 #1 의 원인** |
| remote_nodes.jsonl | peer 목록 없음 — IBCF 등 외부 라우팅 불가 |
| routes.jsonl | 라우팅 매핑 없음 — default 라우팅으로 처리 (target = primary local_node) |
| route_sets.jsonl | 빈 묶음 |
| routing_policies.jsonl | 정책 없음 — routes 가 직접 적용 |
| rules.jsonl | ACL/normalize 없음 — 모두 allow |
| rule_sets.jsonl | 빈 묶음 |
| acl_policies.jsonl | ACL 정책 없음 |
| access_services.jsonl | 등록 가능 realm 없음 → REGISTER 거부 가능 |
| sip_trunk (file_store 별도) | IBCF trunk 없음 (Roles.IBCF=false 면 무관) |

## 6. csp_runtime.py 와의 매핑

csc backend (`csc/src/handlers/csp_runtime.py`) 가 admin API + UI 진입점. file_store 의 domain ↔ csp 의 jsonl ↔ Layer:

| csp 의 jsonl | file_store domain | csp_runtime.py 상수 | Layer |
|---|---|---|---|
| local_nodes.jsonl | `csp_listener` | `_DOM_LISTENER` | 1 |
| remote_nodes.jsonl + sip_trunk | `sip_trunk` | `_DOM_TRUNK` | 2 |
| routes.jsonl + route_sets / routing_policies | `routing_rule` | `_DOM_ROUTE` | 4-6 |
| acl_policies.jsonl + rules / rule_sets | `routing_access_list` | `_DOM_ACCESS` | 7-9 |
| access_services.jsonl | `sip_service` | `_DOM_SERVICE` | 3 |
| (audit log) | `csp_config_audit` | - | - |

file_store → jsonl 동기화는 agent 가 처리 (config_cache 의 push 흐름). 운영 시 UI → CSC API → file_store 변경 → agent 가 모듈 디렉토리의 jsonl 갱신 → csp 가 `CspConfigCache::Refresh()` 로 reload.

## 7. scenario.yaml 의 `csp_config` ↔ 이 Layer 매핑

`scenario.yaml` 의 `csp_config.local_nodes.auto: true` 면 generator 가 env 의 ha_group 으로부터 다음 row 를 자동 생성:

```
env.ha_groups[].vips[] 또는 멤버의 svc NIC IP
  ↓ generator
local_nodes.jsonl row:
  - A/S 그룹: VIP 1개 row (bind_ip=VIP, is_primary=true, protocol=UDP/TCP/TLS 각 1)
  - AA 그룹: 멤버별 svc IP row (멤버 노드의 install dir 에 각각 다른 jsonl)
  - Standalone: 멤버의 svc 또는 mgmt IP row
```

자동 결정 룰 (generator 가 구현, Phase 4):

| HA mode | local_nodes 자동 생성 |
|---|---|
| `active_standby` | VIP 를 bind_ip, primary=true. ha_group 의 모든 멤버 install dir 에 동일 jsonl 배포 (멤버 자체는 자기 NIC 에 bind 시도, VIP 보유한 master 가 실제 LISTEN) |
| `all_active` | 멤버별로 자기 svc IP 를 bind_ip. 각 멤버 install dir 에 자기 IP row 만 |
| `standalone` | 멤버의 svc IP (또는 mgmt — env 에 명시) |

⚠ **active_standby + VIP bind 시 host 단의 `net.ipv4.ip_nonlocal_bind=1` 설정 필요** (BACKUP 노드도 VIP 로 bind 가능해야 fail-over 즉시 처리). 이 사전 조건은 env.yaml 에서 명시하거나 NetNS up 스크립트가 sysctl 설정.

## 8. 이슈 #1 fix 의 실제 적용 단계

NetNS 환경에서 즉시 적용 가능 (generator 없이도):

```bash
# ctrl-a 의 local_nodes.jsonl 작성
cat > /home/nex/work/cims/build/dist/netns-agents/ctrl-a/install/modules/csp/0.0.1/CSP/csp/config/local_nodes.jsonl <<'EOF'
{"id":"csp-main-udp","name":"csp-main","edge":"access","bind_ip":"10.0.1.13","bind_port":5060,"protocol":"UDP","thread_count":2,"enabled":true,"is_primary":true,"tags":[],"note":""}
{"id":"csp-main-tcp","name":"csp-main","edge":"access","bind_ip":"10.0.1.13","bind_port":25061,"protocol":"TCP","thread_count":2,"enabled":true,"is_primary":true,"tags":[],"note":""}
{"id":"csp-main-tls","name":"csp-main","edge":"access","bind_ip":"10.0.1.13","bind_port":5061,"protocol":"TLS","thread_count":2,"enabled":true,"is_primary":true,"tls_cert_path":"cert/csp.pem","tags":[],"note":""}
EOF
# ctrl-b 도 동일 (master 가 죽으면 backup 이 VIP 인수 후 bind)

# host: nonlocal bind 허용 (BACKUP 노드가 VIP 없을 때도 bind 가능)
echo 1 | sudo tee /proc/sys/net/ipv4/ip_nonlocal_bind

# 각 ns 도 동일
sudo ip netns exec ctrl-a sysctl -w net.ipv4.ip_nonlocal_bind=1
sudo ip netns exec ctrl-b sysctl -w net.ipv4.ip_nonlocal_bind=1

# csp restart (CSC API 통해)
for dep in 27 28; do
  curl -sk -X POST -H "Content-Type: application/json" \
    -d '{"job_type":"restart"}' \
    https://127.0.0.1:4419/api/v1/deployments/$dep/job
done

# LISTEN 검증 — 이번엔 10.0.1.13 으로 보여야
sudo ip netns exec ctrl-a ss -tulnp | grep csp
```

이 적용은 Phase 3 시나리오 작성 + Phase 4 generator 의 자동화 대상.

## 관련

- [env.schema.md](env.schema.md) — env.yaml schema
- [scenario.schema.md](scenario.schema.md) — scenario.yaml schema
- `csp/CspConfigCache.cpp`, `csp/CspLocalNodeMap.cpp`, `csp/CspServer.cpp` — 본체 코드
- `csc/src/handlers/csp_runtime.py` — admin API + file_store ↔ jsonl 매핑
