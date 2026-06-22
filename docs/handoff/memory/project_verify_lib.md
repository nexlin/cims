---
name: verify.lib 검증 도구 구조 (2026-04-28, c-마이그레이션 완료)
description: verify/lib/ — 검증 인프라 + items/{phase1,phase2,phase3} 카테고리 분리. 옛 run_all.py/test_*.py/MODULE-*/test_run/ 모두 제거. 검증 항목은 다음 세션부터 console 과 함께 하나씩 추가/보완.
type: project
originSessionId: 2a2a88aa-9279-4c02-8dd9-e4ebc65cc4d2
---
## SOT — 2026-04-28 c-마이그레이션 후 상태 (미커밋)

사용자 결정으로 옛 `tests/test_*.py`(9개) + `run_all.py` + `clean_env.py` + `conftest.py` + `test_run/` 삭제,
`MODULE-*` bridge 와 phase1-modules/main preset 도 제거. `tests/verify_lib/` 디렉토리는
**`verify/lib/` 로 이동**. 인프라/구조만 남기고 **검증 항목은 다음 세션부터 console 과 함께 하나씩 추가/보완**.

## 디렉토리 구조

```
verify/                              # ⭐ 검증 도구 패키지 (NEW location)
├── __init__.py
└── lib/
    ├── registry.py / runner.py / context.py / reporting.py / shell.py / presets.py
    ├── common/                      # ⭐ 공통 helper (NEW)
    │   ├── db.py                    # csp.json Setup.Database 추출 + pymysql connect
    │   ├── subscribers.py           # select_subscribers() + VOLTE_DOMAIN/MCPTT_DOMAIN
    │   ├── access_services.py       # seed_access_services() + signal_csp_reload()
    │   ├── cspsim.py                # run_cspsim() (cims.sh sim wrapper subprocess)
    │   ├── recordings.py            # count_recordings(since=mtime)
    │   └── cmp_client.py            # cmp_request() / remove_group()
    └── items/
        ├── __init__.py              # ⭐ pkgutil.iter_modules 재귀 자동 import
        ├── phase1/                  # 환경(8) + 시나리오(2) = 10
        │   ├── env/{preflight,reset,build,configure,start,pkg_upload,health,seed}.py
        │   └── scenario/{regress_voip,regress_ptt}.py + _helpers.py
        ├── phase2/run_all.py        # P2-RUN-ALL (cims.sh _verify_phase2 wrapping)
        └── phase3/                  # 환경(2) + 시나리오(4) + 검증(1) = 7
            ├── env/{entry_check,seed}.py
            ├── scenario/{volte_voice,volte_video,ptt_voice,ptt_video}.py + _helpers.py
            └── verification/summary.py

tests/
├── cims_verify.py                   # CLI entrypoint (`python3 -m tests.cims_verify`)
└── test_verify_lib.py               # 31 unit test (registry/runner/presets/markers/items_progress)
```

총 18 항목 (Phase 1: 10, Phase 2: 1, Phase 3: 7).

## 핵심 원칙

- **파일 1개 = `@verify_item` 1개** — 추가/삭제/보완 시 파일 한 개 작업으로 완결
- 카테고리는 디렉토리 구조 (`env/`, `scenario/`, `verification/`)
- `items/__init__.py` 가 `pkgutil.iter_modules` 로 재귀 자동 import — 새 항목 추가 시 명시 import 불필요
- 공통 helper 는 `verify/lib/common/` 에 분리 — 항목 파일은 `from ....common.X import Y` 로 호출
- `_` prefix 파일 (`_helpers.py`) 도 import 됨 (의존 모듈)

## 프리셋 (6종)

| 이름 | 항목 수 | 용도 |
|---|---|---|
| phase1-full | 10 | Phase 1 전체 |
| phase1-quick | 2 | preflight + health (sanity) |
| phase2-full | 1 | P2-RUN-ALL |
| phase3-full | 7 | Phase 3 전체 |
| phase3-volte | 5 | VoLTE 시나리오 |
| phase3-ptt | 5 | PTT 시나리오 |

`phase1-modules`, `phase1-main` 은 c-마이그레이션과 함께 제거 (모듈 항목 0개).

## 진행 상태 표시 (이미 완성)

`runner.py` 의 stdout 마커 → backend `_parse_items_progress` → Console UI ProgressTable (1.5초 폴링):
- `[VERIFY] run-start: total=N ids=...`
- `[VERIFY] item-start: ID idx=k/N name=...`
- `[VERIFY] item-end: ID status=PASS|FAIL|SKIP elapsed_ms=...`
- `[VERIFY] child-result: PARENT.CHILD status=... elapsed_ms=... name=...` (Phase 2 22단계만)
- `[VERIFY] run-end: total=N pass=p fail=f skip=s`

자식 항목이 정식 `@verify_item` 으로 등록되면 위 item-start/end 가 실시간 출력 → UI 자동 노출.
ProgressTable: 번호/항목/진행률 bar+ms/결과 status icon, 자식 들여쓰기. window.print() PDF 저장.

## 신규 검증 항목 추가 방법

```python
# verify/lib/items/phase1/env/my_check.py  (파일만 생성하면 끝)
from ....registry import verify_item, ItemResult, ItemStatus
from ....context import VerifyContext

@verify_item(
    id="P1-MY-CHECK", phase=1, category="환경",
    name="내 검증 항목", depends_on=["P1-START"],
    presets=["phase1-full"], side_effects=["read-only"],
    timeout_s=30,
)
def my_check(ctx: VerifyContext) -> ItemResult:
    ...
    return ItemResult(id="P1-MY-CHECK", name="...", status=ItemStatus.PASS, phase=1)
```

`__init__.py` 추가 import 불필요. 다음 console mount 때 UI 가 자동으로 체크박스 노출.

**카테고리 옵션**: `환경` / `시나리오` / `검증` / `배포` (또는 신규 카테고리). 디렉토리도 카테고리에 맞게 (`env/` / `scenario/` / `verification/` 등).

## 다음 세션 작업 방향 (사용자 명시)

**"각 검증 단계별 검증 항목을 하나하나 추가하면서 console 과 함께 확인하면서 보완"**

진행 사이클:
1. **항목 1개 정의** → 적합한 디렉토리에 파일 생성, `@verify_item` + 함수
2. **자동 등록 확인** → `python3 -m tests.cims_verify list --phase N`
3. **Console UI 확인** → TB-Console (`http://<ens160>:3000/testbed/verify`) 에서:
   - 체크박스 자동 노출
   - 실행 시 ProgressTable 에 진행률 실시간 표시
   - 결과 (미진행 / 진행중 / 성공 / 실패) 정확히 표기
4. **PASS 확인 후 다음 항목**

각 항목 정의 시 고려:
- `id`, `phase`, `category`, `name`, `description`
- `depends_on` (선행 항목)
- `presets` (어느 프리셋에 포함할지)
- `side_effects` (read-only / fs-write / db-write / db-truncate / service-start / sim-call / process-kill / network 등)
- `timeout_s` (실행 시간 한계)

## CLI 사용

```bash
python3 -m tests.cims_verify list [--phase N] [--json]
python3 -m tests.cims_verify list-presets [--json]
python3 -m tests.cims_verify describe ITEM_ID [--json]
python3 -m tests.cims_verify run --phase 1
python3 -m tests.cims_verify run --items P1-PREFLIGHT,P1-HEALTH
python3 -m tests.cims_verify run --preset phase3-volte --json

./cims.sh verify list --phase 3 --json
./cims.sh verify phase1
./cims.sh verify phase2 --legacy   # _verify_phase2 본체 직접 호출 (Phase 2 만 의미)
```

## Backend API

- `GET /api/v1/verification/items?phase=N` — 등록 항목 트리 (60s 캐시). UI 동적 체크박스용.
- `GET /api/v1/verification/presets` — 프리셋 목록.
- `POST /api/v1/verification/phases/<N>` body 에 `items: string[]` / `preset` / `only_children` 추가 — 부분 실행. `async=true` 시 job_id 즉시 반환.
- `GET /api/v1/verification/jobs/<id>` — async job 폴링. 응답에 `items_progress` 포함 (실시간 진행).

c-마이그레이션 시 옛 `/run` (POST), `/report` (GET) endpoint 제거됨 (run_all.py 의존이라).

## 단위 테스트

```bash
python3 -m unittest tests.test_verify_lib -v
# 31 tests in 0.28s — OK
```

## c-마이그레이션 (2026-04-28, 미커밋) 변경 요약

**삭제** (옛 검증 코드):
- `tests/run_all.py` / `clean_env.py` / `conftest.py`
- `tests/test_csp.py` / `test_csc.py` / `test_cmp.py` / `test_e2e.py` / `test_volte_service.py` / `test_ptt_service.py` / `test_media.py` / `test_sip_runtime.py` / `test_agent_deployment.py`
- `tests/test_env.json` / `test_env.json.template` / `test_spec.md` / `verification_plan.md` / `verification_report.md` / `media/`
- `tests/verify_lib/items/modules/` (run_all_bridge.py)
- `tests/verify_lib/items/phase1/regression.py` → env/seed.py + scenario/regress_*.py 분해
- `tests/verify_lib/items/phase3/{seed,scenarios}.py` → env/seed.py + scenario/*.py 분해
- `test_run/` 디렉토리 (옛 수동 테스트 fixture, 코드베이스 참조 0건)

**삭제 — backend endpoint** (옛 run_all.py 호출):
- `csc/src/handlers/verification.py` 의 `/api/v1/verification/run` (POST), `/report` (GET)

**삭제 — UI** (ems/core/console/src/pages/VerificationPage.tsx):
- "Phase 1 상세 검증 (run_all.py)" 섹션 + `runDetail()` / `loadLegacyReport()` 함수
- `VerResult` 타입, `detailRunning/detailResult/expandedModule/legacyReportMd` state
- VerifyMode `'main'`, `'modules'` → `'full' | 'quick' | 'volte' | 'ptt'`

**디렉토리 이동**:
- `tests/verify_lib/` → `verify/lib/`
- 모든 import: `from verify_lib` → `from verify.lib`, `import verify_lib` → `import verify.lib`
- docs/cims.sh/CLAUDE.md 의 경로 참조 갱신

**유지된 인프라 fix** (이전 세션 검증 시 나온 정합성):
- `cims.sh cmd_reset`: `volte_subscriptions` / `ptt_subscriptions` 의 `register_time = NULL, logout_time = NULL` UPDATE 추가 (등록 잔여 정리)
- `verify/lib/items/phase1/scenario/_helpers.py`: `count_recordings(since=mtime)` — 모듈 검증 잔여와 격리

## 문서 동기화

- `docs/VERIFICATION_PROCESS.md` §1.3.1 — 디렉토리 구조 / 18 항목 / 6 프리셋 / 신규 항목 추가 방법 / 진행 상태 표시 갱신
- `docs/VERIFICATION_MANUAL.md` §1.1 — phase1 자동 실행 옵션 + verify.lib 구조 안내
- `CLAUDE.md` — `test_run/` 한 줄 제거

## 중요 함정

- **TB-CSC env 누수** (`CIMS_CSP_CONFIG=csc-tb.json`): subprocess 로 누수 → 자식 csc 가 TB 포트 충돌. 해결됨 (`_sanitized_env` in service_control.py / verification.py / verify/lib/context.py).
- **cims.sh 의 sed 위험**: `sed '932,934d;d'` 같이 두 번째 `d` 가 모든 라인 삭제. 멀티 라인 범위 삭제 시 단일 `d` 만 사용.
- **Phase 2 의 reset**: `_verify_phase2` 가 시작 시 `cmd_reset --keep-processes` 실행 — 환경 wipe 됨. 다른 검증과 동시 실행 금지.
- **legacy 폴백**: `cims.sh verify phase2 --legacy` 만 의미 있음. phase 1/3 의 `--legacy` 는 본체 제거됨이라 무효.
- **PTT 그룹콜 conflict**: 같은 group 에 짧은 시간 내 반복 INVITE 시 410 Gone 발생. PTT 시나리오 항목 새로 정의할 때 `verify.lib.common.cmp_client.remove_group()` 사전 호출 또는 다른 group ID 사용 권장.
