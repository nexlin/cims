# deployment/ — 배포 환경 + 시나리오 SoT

CIMS 의 **검증/상용 공통 배포 자료**를 모은 디렉토리. 한 환경의 인프라 정의 + 그 환경에서 돌릴 시나리오들이 한 곳에 모임.

## 구조

```
deployment/
├── README.md                       (이 파일)
├── _schema/                        YAML schema 명세
│   ├── env.schema.md               env.yaml 필드 명세
│   └── scenario.schema.md          scenario.yaml 필드 명세
├── <env-name>/                     배포 환경 (예: prod-multi-host)
│   ├── env.yaml                    인프라 SoT — NIC/IP/VIP/노드/DB
│   ├── README.md                   이 환경 안내
│   └── scenarios/                  이 환경에서 돌릴 시나리오
│       ├── <scenario-1>.yaml       예: volte-only
│       ├── <scenario-2>.yaml       예: volte-ptt
│       └── ...
└── bin/
    ├── render.py                   generator — scenario → config bundle (--diff 미리보기)
    ├── apply.py                    bundle → install dir (--backup/--restore/--verify/--restart auto)
    ├── verify.py                   verify.expected_listen / smoke / failover 자동 실행 (host-local)
    ├── check-all.sh                모든 env × scenario render --check-only 회기 (또는 `make verify-scenarios`)
    └── health.sh                   빠른 LIVE 진단 (CSC API + csp/cmp 프로세스/로그, sudo 불필요)
```

## 컨벤션

| 항목 | 규칙 |
|---|---|
| 환경 디렉토리명 | `<배포-환경-식별자>` (kebab-case). 예: `dev-single-host`, `prod-multi-host` |
| `env.yaml` | 환경 당 1개. 인프라 SoT (변경 빈도 낮음 — 환경 셋업 1회) |
| `scenarios/*.yaml` | env 위에서 돌릴 서비스 의도 + 패키지 배포 매핑 (한 환경에서 여러 시나리오 운용 가능) |
| 시나리오명 | `<서비스-의도>.yaml` (kebab-case). 예: `volte-only`, `volte-ptt`, `full`, `smoke` |
| `_schema/` 시작의 underscore | "디렉토리지만 환경 아님" 을 표시. generator 가 enumerate 시 skip |

## 사용 흐름

1. **환경 등록** — `deployment/<env-name>/env.yaml` 작성 (NIC/IP/VIP/노드/DB)
2. **시나리오 작성** — `deployment/<env-name>/scenarios/<x>.yaml` (역할 + 패키지 + 배포 매핑)
3. **미리보기** — `./bin/render.py --env <e> --scenario <s> --diff` — apply 시 어떤 파일이 바뀔지 의미적 diff
4. **배포 + 검증** (한 명령 end-to-end):
   ```
   ./bin/apply.py --env <e> --scenario <s> --backup --restart auto --verify
   ```
   - `--backup` 기존 파일 .bak 보호
   - `--restart auto` CSC API 로 (agent_id, package) 매핑하여 csp/cmp restart job 자동 큐잉
   - `--verify` restart 후 listen phase 자동 확인
5. **회기** — `make verify-scenarios` (또는 `./bin/check-all.sh`) — render --check-only 일괄 회기
6. **롤백** — `./bin/apply.py --env <e> --scenario <s> --restore` — 마지막 .bak 일괄 복원

> 실 agent (ctrl01/ctrl02/media01/media02) 로의 바이너리/패키지 전달은 **agent install API** (Console → 패키지 등록 → deployment install job) 가 담당. render/apply 는 config bundle 만 생성·갱신.

## 현재 등록된 환경 + 시나리오

| 환경 | 시나리오 | 비고 |
|---|---|---|
| [dev-single-host](dev-single-host/) | `smoke` | 단일 host loopback (csc/csp/cmp 동거). 빠른 로컬 smoke |
| [prod-multi-host](prod-multi-host/) | `volte-ptt` | 2-host A/S (Control: ctrl01/ctrl02) + 2-host AA (Media: media01/media02) + 외부 DB. 사이트별 IP/암호 patch 필요 |

회기 (전체 일괄): `./bin/check-all.sh`

## 진짜 csp schema 정합 (commits `312c69a` / `ac4cf95`)

scenario yaml 의 모든 csp_config layer 는 csp 의 `.cpp` 헤더 schema 와 1:1 매칭:

| layer | csp 코드 | 진짜 key |
|---|---|---|
| `local_nodes` | `CspLocalNodeMap.cpp` | id/name/edge/bind_ip/bind_port/protocol/thread_count/enabled/is_primary/tls_cert_path |
| `remote_nodes` | `CspRemoteNodeMap.cpp` | id/name/ip/port/protocol/remote_domain/srv_lookup/dns_fallback/tls_verify |
| `access_services` | `CspServiceMap.cpp` | id/name/kind(volte\|ptt)/domain/auth_realm/inbound_policy/priority/enabled/**allowed_local_node_refs**[] |
| `routes` | `CspRouteMap.cpp` | name/**local_node_ref/remote_node_ref** pair SOT (외부 trunk 용) |
| `route_sets` | `CspRouteSetMap.cpp` | name/distribution_policy/members[].route_ref |
| `routing_policies` | `CspRoutingPolicyEngine.cpp` | name/match_rule_set_ref/target_ref |
| `rules` | `CspRuleEvaluator.cpp` | name/**field/op/value** 트리플 |
| `rule_sets` | `CspRuleEvaluator.cpp` | name/combinator(AND\|OR)/members[].rule_ref/negate |
| `acl_policies` | `CspAclPolicyEngine.cpp` | name/match_rule_set_ref/scope/scope_ref/action |

**route 의 의미**: `(local, remote)` pair 가 SOT — 외부 peer (IBCF trunk) 로의 outbound 라우팅 용. VoLTE/PTT **내부 호** (B2BUA via CMP) 는 routes 없이 동작 (access_services + CmpClient 만 사용). 외부 trunk 시나리오 (`full`) 에만 채움.

## CmpClient endpoint 처리 (commit `53c0bbb` 이후 LIVE 활성)

- 기본: csp.json 의 `Setup.MediaServer.Host` 가 primary endpoint
- 추가: `remote_nodes.jsonl` 의 `tags=["cmp"]` 인 row 들은 csp 가 `CmpClient.AddEndpoint` 로 등록 → consistent hash ring 으로 session 분배
- `auto_cmp: true` 면 render.py 가 cmp ha_group 의 모든 멤버를 자동 등록 (Control AS + Media AA 모델에서 multi-cmp endpoint 분배 LIVE 활성)

## 관련 문서

- [_schema/env.schema.md](_schema/env.schema.md) — env.yaml 필드 명세
- [_schema/scenario.schema.md](_schema/scenario.schema.md) — scenario.yaml 필드 명세 (진짜 csp schema)
- [_schema/csp-layers.md](_schema/csp-layers.md) — CSP 10 entity layer 모델
- `docs/design/runtime_store_design.md` — file_store SoT (deployment/agent/ha_groups/packages JSON)
- `docs/design/db_schema.md` — 외부 DB 위임 도메인 (users / volte_subscriptions / ptt_subscriptions / organizations)
