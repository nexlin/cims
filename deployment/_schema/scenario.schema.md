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

#### `routes` / `route_sets`
도메인/user → node 매핑.

```yaml
csp_config:
  routes:
    - id:       default-ims
      match:    { domain: "ims.mnc033.mcc450.3gppnetwork.org" }
      target:   csp-main
    - id:       default-ptt
      match:    { domain: "ptt.mnc033.mcc450.3gppnetwork.org" }
      target:   csp-main
  route_sets:
    - id:       default
      routes:   [default-ims, default-ptt]
```

#### `rules` / `rule_sets`
ACL / normalize / header manipulation.

```yaml
csp_config:
  rules:
    - id:       allow-local
      match:    { source_cidr: "10.0.0.0/8" }
      action:   allow
  rule_sets:
    - id:       default
      rules:    [allow-local]
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
| `routes[].target` 가 local_nodes 또는 remote_nodes 에 존재 | 라우팅 dangling 방지 |

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
