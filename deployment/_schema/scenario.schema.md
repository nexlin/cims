# scenario.yaml schema

`env.yaml` (인프라) 위에 **무엇을 어떻게 깔지** 를 정의. 서비스 의도 (역할/패키지) + 배포 매핑 + CSP config layer (Local Node / Remote Node / Route / Rule ...) 의 입력값.

generator (`deployment/bin/render.py`) 가 `env.yaml + scenario.yaml` → 산출:
- `csp.json` (Setup.*) 
- `local_nodes.jsonl`, `remote_nodes.jsonl`, `routes.jsonl`, `rules.jsonl`, ... (csp/config/)
- `cmp.json` (CMP 의 ServerIp/RtpIp/CspIp 등)
- 가입자 seed (선택 — `users/`, `groups/`)

## 최상위 필드

```yaml
name:            <시나리오 식별자 — 파일명과 일치>
description:     <한 줄 설명>
env:             <env-name>           # 어느 환경 위에 깔지 (디렉토리명)
services:        <서비스 의도 — 어떤 역할 활성화>
deployments:     <ha_group → 패키지 매핑>
csp_config:      <CSP layer 의 값 — Setup / local_nodes / remote_nodes / routes / rules>
cmp_config:      <CMP 의 값 (env 와 ha_group 으로부터 자동 유도 가능 시 생략)>
subscribers:     <가입자 seed — 선택>
```

## 필드별 명세

### `services`
이 시나리오가 활성화할 서비스. csp 의 Roles 가 여기서 결정.

```yaml
services:
  cscf:    true        # REGISTER / SUBSCRIBE 처리
  tas:     true        # VoIP B2BUA
  ptt_as:  true        # PTT 그룹콜
  ibcf:    false       # IP-PBX 트렁크
```

### `deployments`
HA group → 패키지 매핑. file_store `deployments/<n>.json` 으로 생성.

```yaml
deployments:
  - ha_group: 1                # env.ha_groups[].id 참조 (또는 name)
    packages:                  # 이 그룹의 모든 멤버에 install 할 패키지
      - { name: csp, version: 0.0.1, process_name: CSP }
  - ha_group: 2
    packages:
      - { name: cmp, version: 0.0.1, process_name: CMP }
```

### `csp_config`
CSP 의 다층 설정. layer 별로 분리.

#### `setup`
csp.json 의 `Setup.*` (시스템 기본).

```yaml
csp_config:
  setup:
    sip:
      udp_thread_count:     2
      stack_execute_period: 20
      min_register_timeout: 60
      user_timeout:         3600
      tcp_thread_count:     2
      tcp_recv_timeout:     600
      tls_accept_timeout:   10
      cert_file:            cert/csp.pem
    media_server:
      enable:        true
      control_port:  9000
      local_port:    9001
      # local_ip / host 는 deployments 매핑에서 자동 유도 (멤버 노드의 svc IP + 매칭된 CMP 그룹의 VIP/IP)
    log:
      max_size_mb:    10
      level:          { debug: true, info: true, network: true, sql: false }
    database:
      # null 이면 env.database 그대로. override 가능
      use_env: true
    monitor:
      port:           16000
      client_ip_list: ["10.0.0.1"]    # CSP Monitor.ClientIpList
    security:
      deny_sip_user_agents: [friendly-scanner, sundayddr]
    service_logging:
      # env.service_logging 의 시나리오별 override (보통 생략)
      dir:       /custom/path
      enable:    [sip, cmp, csc]
      recording: true
```

#### `local_nodes`
이 환경에서의 csp 자기 자신 SIP endpoint 정의. **env.yaml + ha_group 으로부터 자동 유도** 가 기본 — 명시는 override 용.

```yaml
csp_config:
  local_nodes:
    auto: true              # env 의 ha_group 멤버 + service net 으로 자동 생성
    # 자동 생성 결과:
    #   ha_group=1 (Control-Server, A/S) 의 master 멤버 ctrl-a 가 svc 망 (VIP 10.0.1.13:5060) 으로 listen
    #   transport: UDP + TCP + TLS
    # override:
    overrides:
      - id:       csp-main
        bind_ip:  10.0.1.13    # VIP
        bind_port: 5060
        transport: udp
```

#### `remote_nodes`
peer 정의 (다른 csp / CMP / IBCF / IP-PBX). 환경 안의 ha_group (CMP) 자동 + 외부 IP 명시.

```yaml
csp_config:
  remote_nodes:
    auto_cmp: true             # env 의 CMP ha_group 자동 등록
    extra:
      - id:       ibcf-trunk
        host:     203.0.113.10
        port:     5060
        transport: udp
        purpose:  외부 IP-PBX
```

#### `routes`
**(local_node_ref, remote_node_ref) pair 가 SOT** (csp/CspRouteMap.cpp). 외부 peer (IBCF trunk 등) 로의 outbound 라우팅을 표현. VoLTE/PTT 내부 호 (B2BUA via CMP) 는 routes 없이도 동작 — REGISTER 인증 + access_services + CmpClient 만 사용. 외부 trunk 필요한 시나리오 (full.yaml) 에만 채움.

```yaml
csp_config:
  routes:
    - name:            csp-to-pbx           # 필수, unique
      local_node_ref:  csp-main-udp         # 필수, local_nodes.id 매칭
      remote_node_ref: ibcf-trunk           # 필수, remote_nodes.name 매칭
      register_to_remote: false             # 외부 trunk 에 REGISTER 보낼지
      register_expires:   3600
      auth_user:         ""
      auth_password:     ""
      auth_realm:        ""
      outbound_proxy_ip:   ""
      outbound_proxy_port: 0
      max_concurrent_calls: 0               # 0 = 무제한
      cps_limit:           0
      enabled:           true
      note:              "demo trunk"
```
(local, remote) pair 중복 금지. routes 가 비어있어도 OK.

#### `route_sets`
outbound 라우팅 후보 묶음 — failover / weighted 분배 (CspRouteSetMap.cpp).

```yaml
csp_config:
  route_sets:
    - name:                  pbx-trunk-set
      distribution_policy:   failover         # failover | weighted | hash
      health_check_mode:     options_ping
      members:
        - { route_ref: csp-to-pbx, priority: 100, weight: 1 }
      enabled:               true
```

#### `routing_policies`
**routing_policy = (rule_set match, target route_set)** (CspRoutingPolicyEngine.cpp).

```yaml
csp_config:
  routing_policies:
    - name:               pbx-outbound
      priority:           100
      match_rule_set_ref: match-pbx-outbound   # rule_sets.name 매칭
      target_type:        route_set            # 보통 route_set
      target_ref:         pbx-trunk-set        # route_sets.name 매칭
      fail_action:        next_policy
      enabled:            true
```

#### `rules` / `rule_sets`
csp 의 진짜 schema (CspRuleEvaluator.cpp): name/field/op/value 트리플.

지원 field: `src_ip` / `dst_ip` / `from_uri_host` / `from_uri_user` / `to_uri_host` / `to_uri_user` / `req_uri_host` / `req_uri_user` / `user_agent` / `method` / `p_asserted_identity` / `via_host`

지원 op: `eq` / `ne` / `prefix` / `suffix` / `contains` / `regex` / `in_cidr` / `in_list` / `exists` / `not_exists`

```yaml
csp_config:
  rules:
    - name:    allow-mgmt-net
      field:   src_ip
      op:      in_cidr
      value:   "10.0.0.0/24"
      enabled: true

  rule_sets:
    - name:       trusted-nets
      combinator: AND                   # AND | OR
      members:
        - { rule_ref: allow-mgmt-net, negate: false }
      enabled:    true
```

#### `acl_policies`
ACL — match_rule_set_ref 매칭 시 action 실행 (CspAclPolicyEngine.cpp). 미정의 시 csp default = allow.

```yaml
csp_config:
  acl_policies:
    - name:               block-external
      priority:           100
      match_rule_set_ref: trusted-nets        # 매칭되면 action
      scope:              global              # global | listener | service
      scope_ref:          ""                  # scope=listener 면 local_node 이름
      action:             allow               # allow | deny
      enabled:            true
```

### `cmp_config`
CMP 의 값. 단순 환경은 env 만으로 자동 유도.

```yaml
cmp_config:
  auto: true                  # env 의 media-server ha_group 의 멤버별 svc IP 자동 매핑
  # 자동 결과:
  #   media-a: ServerIp=10.0.1.21, RtpIp=10.0.1.21, CspIp=<control vip>
  #   media-b: ServerIp=10.0.1.22, RtpIp=10.0.1.22, CspIp=<control vip>
  overrides:
    rtp_pool_size:       20
    ptt_rtp_pool_size:   10
    enable_dtmf_ptt:     true
```

### `subscribers`
가입자 seed (선택). DB 미연결 환경에서 file-based 로 동작 시 사용. 보통은 별도 회차에서 처리.

```yaml
subscribers:
  source: file              # file | db
  users:
    - { sip_id: "+82571900001", auth_id: "4503382571900001", domain: "ptt.mnc033.mcc450.3gppnetwork.org", passwd: "123456" }
  volte_bindings:
    - { user: "+82571900001", service: volte-basic }
  ptt_bindings:
    - { user: "+82571900001", group: "+82571910001" }
```

## generator 동작 (`bin/render.py`)

```
입력:  deployment/<env>/env.yaml + deployment/<env>/scenarios/<scenario>.yaml
출력:  ./bundle/
        ├── ctrl-a/
        │   ├── csp.json
        │   ├── local_nodes.jsonl
        │   ├── remote_nodes.jsonl
        │   ├── routes.jsonl
        │   └── rules.jsonl
        ├── ctrl-b/  (same)
        ├── media-a/
        │   └── cmp.json
        └── media-b/
            └── cmp.json
```

generator 가 검증하는 invariant:

| 검증 | 의미 |
|---|---|
| `deployments[].ha_group` 가 env.ha_groups 에 존재 | 매핑 오타 방지 |
| `csp_config.local_nodes.auto=true` 면 ha_group 의 mode 와 호환 | A/S 면 VIP bind, AA 면 멤버별 svc IP bind |
| `remote_nodes.auto_cmp=true` 면 환경에 cmp ha_group 존재 | CMP 미배포 시 경고 |
| `routes[].local_node_ref` 가 local_nodes 에 존재 | dangling 방지 |
| `routes[].remote_node_ref` 가 remote_nodes 에 존재 | dangling 방지 |
| `(local_node_ref, remote_node_ref)` pair 중복 없음 | RouteMap pair unique constraint |
| `route_sets[].members[].route_ref` 가 routes 에 존재 | dangling |
| `routing_policies[].match_rule_set_ref` 가 rule_sets 에 존재 | dangling |
| `routing_policies[].target_ref` 가 route_sets 에 존재 (target_type=route_set) | dangling |
| `rules[].field` ∈ 지원 set | csp 가 인식 못 하는 field 차단 |
| `rules[].op` ∈ 지원 set | 동일 |
| `rule_sets[].members[].rule_ref` 가 rules 에 존재 | dangling |
| `acl_policies[].match_rule_set_ref` 가 rule_sets 에 존재 | dangling |

## 예시 — 최소 시나리오 (smoke)

```yaml
name: smoke
description: REGISTER + CSP-CMP heartbeat 까지만 검증 (호 X)
env: dev-single-host
services:
  cscf: true
  tas:  false
  ptt_as: false
  ibcf: false
deployments:
  - ha_group: 1
    packages:
      - { name: csp, version: 0.0.1, process_name: CSP }
csp_config:
  setup:
    sip: { udp_thread_count: 1 }
  local_nodes: { auto: true }
  remote_nodes: { auto_cmp: false }
  routes: []
  rules: []
cmp_config: null    # 이 시나리오는 CMP 없음
subscribers:
  source: file
  users:
    - { sip_id: "test1", auth_id: "test1", domain: "csp", passwd: "1234" }
```
