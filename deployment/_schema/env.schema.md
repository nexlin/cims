# env.yaml schema

배포 환경의 **인프라 SoT**. NIC / IP / VIP / 노드 / DB / CSC endpoint 등 인프라 layer 정의. 서비스 의도 (역할/패키지) 는 `scenarios/*.yaml` 에 분리.

## 최상위 필드

```yaml
name:            <환경 식별자 — 디렉토리명과 일치>
description:     <한 줄 설명>
kind:            single-host | multi-host   # 인프라 종류
csc:             <CSC endpoint — agent 가 통신할 곳>
database:        <외부 DB 연결 정보 — 없으면 null>
networks:        <망(브릿지/VLAN) 정의>
nodes:           <노드 정의 — agent 단위>
ha_groups:       <HA 그룹 정의 — 노드 묶음 + VIP>
```

## 필드별 명세

### `csc`
agent 가 통신할 CSC endpoint. agent install 시 `--csc-url` 로 전달.

```yaml
csc:
  url:    https://10.0.0.1:4419    # required
  notes:  agent 가 enroll/heartbeat/job 수신할 OAM(CSC) endpoint
```

### `database`
가입자 도메인 (users / volte_subscriptions / ptt_subscriptions / organizations) 의 외부 DB. dev 환경처럼 DB 미연결이면 `null`.

```yaml
database:
  host:     127.0.0.1
  port:     3306
  user:     cims
  password: cims1234
  dbname:   cims
# 또는 미연결:
database: null
```

### `service_logging` (선택)
CSP/CMP 의 `ServiceLogging` 섹션을 채울 환경별 값. 환경마다 마운트 위치가 달라 host 측 절대 경로를 한 곳에 둠.

```yaml
service_logging:
  dir:            /home/nex/work/cims/build/dist/ext_mnt/service_log
  enable:         [sip, cmp, csc]      # csp 의 ServiceLogging.Enable
  enable_for_cmp: [csp]                # cmp 의 ServiceLogging.Enable
  recording:      true
```

생략 시 `/var/log/cims/service_log` + 기본 enable 리스트가 사용됨. scenario.yaml 의 `csp_config.setup.service_logging` / `cmp_config.overrides.service_logging` 으로 시나리오별 override 도 가능.

### `networks`
망 정의. 각 망은 명명된 식별자 + CIDR + 용도.

```yaml
networks:
  - id:     mgmt                    # 망 식별자 (이후 nodes 의 nic 에서 참조)
    cidr:   10.0.0.0/24
    purpose: agent ↔ CSC 통신 (관리)
  - id:     svc
    cidr:   10.0.1.0/24
    purpose: 서비스 트래픽 (SIP / RTP)
  - id:     int
    cidr:   10.0.2.0/24
    purpose: 노드 간 내부 통신 (sync / DB replication 등)
```

### `nodes`
배포 단위 = agent 한 개. 각 노드의 NIC 별 IP 매핑.

```yaml
nodes:
  - id:       ctrl-a               # 노드 식별자 (kebab-case)
    agent_id: 1                    # file_store agents/<n>.json 의 id
    role_hint: control-server      # human-friendly 역할 (HA group 과 무관)
    nics:
      - iface: mgmt                # 노드 내부 NIC 이름 (= ip 출력의 ifname)
        net:   mgmt                # networks[].id 참조
        ip:    10.0.0.11
        mask:  24
      - iface: svc
        net:   svc
        ip:    10.0.1.11
        mask:  24
      - iface: int
        net:   int
        ip:    10.0.2.11
        mask:  24
```

### `ha_groups`
HA 그룹 정의. file_store `ha_groups/<n>.json` 과 1:1 매핑.

```yaml
ha_groups:
  - id:       1                    # file_store ha_groups/<n>.json id
    name:     Control-Server
    mode:     active_standby       # active_standby | all_active | standalone
    members:
      - node:     ctrl-a           # nodes[].id 참조
        role:     master           # master | backup (AA 면 모두 backup)
        priority: 100
      - node:     ctrl-b
        role:     backup
        priority: 90
    vips:                           # AA 면 보통 비움
      - slot:      서비스           # 운영자 라벨
        ip:        10.0.1.13       # VIP 주소
        net:       svc              # networks[].id
        vrid:      51              # VRRP virtual_router_id
        auth_pass: '00000000'
```

## 검증 규칙 (generator 가 확인)

| 규칙 | 메시지 |
|---|---|
| `nodes[].nics[].net` 가 `networks[].id` 에 존재 | "node X 의 nic Y 가 알 수 없는 net=Z 참조" |
| `ha_groups[].members[].node` 가 `nodes[].id` 에 존재 | "ha_group X 의 멤버 Y 가 알 수 없는 노드" |
| `ha_groups[].vips[].net` 가 모든 멤버의 nics 에 매칭 (같은 mask) | "vip slot=X 의 net=Y 가 멤버 NIC 와 mismatch" |
| `nodes[].agent_id` 가 file_store 에 존재 | "agent_id=X 미등록 — enroll 필요" |
| `agent_id` 중복 없음 | "agent_id X 가 노드 A 와 B 에 동시 매핑" |

## 예시 — 최소 환경 (dev-single-host)

```yaml
name: dev-single-host
description: 단일 host 에 csc/csp/cmp 동거 (smoke test)
kind: single-host
csc:
  url: https://127.0.0.1:4419
database: null
networks:
  - id: loopback
    cidr: 127.0.0.0/8
    purpose: 모든 트래픽 (single host)
nodes:
  - id: self
    agent_id: 1
    role_hint: all-in-one
    nics:
      - { iface: lo, net: loopback, ip: 127.0.0.1, mask: 8 }
ha_groups:
  - id: 1
    name: Standalone
    mode: standalone
    members:
      - { node: self, role: master, priority: 100 }
    vips: []
```
