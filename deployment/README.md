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
    └── render.py                   generator — scenario → config bundle
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

## 현재 등록된 환경

| 환경 | 상태 | 비고 |
|---|---|---|
| [tb-netns-4-node](tb-netns-4-node/) | ✅ 1차 작업 대상 | NetNS 4 ns (ctrl-a/b + media-a/b), VIP 10.0.1.13 |
| dev-single-host | placeholder | 후속 |
| prod-multi-host | placeholder | 후속 |

## 관련 문서

- [_schema/env.schema.md](_schema/env.schema.md) — env.yaml 필드 명세
- [_schema/scenario.schema.md](_schema/scenario.schema.md) — scenario.yaml 필드 명세
- `docs/design/runtime_store_design.md` — file_store SoT (deployment/agent/ha_groups/packages JSON)
- `docs/design/db_schema.md` — 외부 DB 위임 도메인 (users / volte_subscriptions / ptt_subscriptions / organizations)
