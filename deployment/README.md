# deployment/ — 배포 환경 + 시나리오 SoT

CIMS 의 **검증/상용 공통 배포 자료**를 모은 디렉토리. 한 환경의 인프라 정의 + 그 환경에서 돌릴 시나리오들이 한 곳에 모임.

## 구조

```
deployment/
├── README.md                       (이 파일)
├── _schema/                        YAML schema 명세
│   ├── env.schema.md               env.yaml 필드 명세
│   └── scenario.schema.md          scenario.yaml 필드 명세
├── <env-name>/                     배포 환경 (예: tb-netns-4-node)
│   ├── env.yaml                    인프라 SoT — NIC/IP/VIP/노드/DB
│   ├── README.md                   이 환경 안내
│   └── scenarios/                  이 환경에서 돌릴 시나리오
│       ├── <scenario-1>.yaml       예: volte-only
│       ├── <scenario-2>.yaml       예: volte-ptt
│       └── ...
└── bin/
    ├── render.py                   generator — scenario → config bundle
    ├── apply.py                    bundle → install dir 복사 (+ CSC API restart)
    ├── verify.py                   verify.expected_listen / smoke / failover 자동 실행
    ├── check-all.sh                모든 env × scenario render --check-only 회기
    ├── deploy-modules.sh           모듈 일괄 deploy (수동 TB-CSC API 호출)
    └── verify-modules.sh           sim-a 에서 cspsim 으로 호출 검증 (수동)
```

## 컨벤션

| 항목 | 규칙 |
|---|---|
| 환경 디렉토리명 | `<배포-환경-식별자>` (kebab-case). 예: `tb-netns-4-node`, `dev-single-host`, `prod-multi-host` |
| `env.yaml` | 환경 당 1개. 인프라 SoT (변경 빈도 낮음 — 환경 셋업 1회) |
| `scenarios/*.yaml` | env 위에서 돌릴 서비스 의도 + 패키지 배포 매핑 (한 환경에서 여러 시나리오 운용 가능) |
| 시나리오명 | `<서비스-의도>.yaml` (kebab-case). 예: `volte-only`, `volte-ptt`, `full`, `smoke` |
| `_schema/` 시작의 underscore | "디렉토리지만 환경 아님" 을 표시. generator 가 enumerate 시 skip |

## 사용 흐름

1. **환경 등록** — `deployment/<env-name>/env.yaml` 작성 (NIC/IP/VIP/노드/DB)
2. **시나리오 작성** — `deployment/<env-name>/scenarios/<x>.yaml` (역할 + 패키지 + 배포 매핑)
3. **render** — `./bin/render.py --env <env-name> --scenario <scn> [--out <dir>] [--check-only]`
   → 산출: `<out>/<node>/csp.json` + `<out>/<node>/config/*.jsonl` (9종) + `<out>/<node>/user/<sip_id>.json` (CSP 노드), `<out>/<node>/cmp.json` (CMP 노드), `<out>/manifest.json`
4. **배포** — `./bin/apply.py --env <env> --scenario <scn>` 로 bundle 을 install dir 에 복사 (또는 agent install params 로 전달)
5. **검증** — `./bin/verify.py --env <env> --scenario <scn> [--phase listen|smoke|failover|all]`
   → expected_listen (포트 LISTEN 확인) / smoke (cspsim REGISTER/call) / failover (VIP 인수 + followup smoke)

## 현재 등록된 환경 + 시나리오

| 환경 | 시나리오 | 비고 |
|---|---|---|
| [tb-netns-4-node](tb-netns-4-node/) | `volte-ptt` / `volte-only` / `full` | NetNS 5 ns (ctrl-a/b A/S + media-a/b AA + sim-a). VIP 10.0.1.13. LIVE 검증 환경 |
| [dev-single-host](dev-single-host/) | `smoke` | 단일 host loopback (csc/csp/cmp 동거). 빠른 로컬 smoke |
| [prod-multi-host](prod-multi-host/) | `volte-ptt` | 2-host A/S (Control) + 2-host AA (Media) + 외부 DB. 사이트별 IP/암호 patch 필요 |

회기 (전체 일괄): `./bin/check-all.sh` — 5/5 PASS

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
