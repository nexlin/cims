---
name: 검증 6단계(S1~S6) 파이프라인 SOT (2026-05-06)
description: registry/runner/items/CLI/Backend/V2/history page/DB 모두 마이그레이션 완료. 5 commits. V2 LIVE polling + 회차 이력 자동 기록.
type: project
originSessionId: cb500d97-8d35-4888-ab9f-bb90080b8fe0
---
## SOT — 2026-05-06 6단계 파이프라인 완성

옛 3단계 (Phase 1/2/3) → 6단계 (S1~S6) 로 전면 재설계 + 백엔드/UI/DB 모두 연결 완료.

### 6단계 정의

| Stage | 이름 | scope | gate | 항목 수 |
|---|---|---|---|---|
| S1 | 정적 검사 | py_compile / eslint / tsc / clang-format / unit | 코드 위생 | 5 |
| S2 | 빌드 | preflight + cmake build | 컴파일 통과 | 2 |
| S3 | 스모크 | dev configure → start → 1콜 VoIP/PTT | sanity | 7 |
| S4 | 패키지화 | tarball 5개 + manifest.json (sha256) | immutability | 2 |
| S5 | 로컬 배포 | TB-CSC → Test-agent → csc-server → csp/cmp 체인 | 배포 회귀 | 7 부모 + 13 자식 |
| S6 | 통합 검증 | VoLTE/PTT 음성·영상 + summary | 상용 진입 | 7 |

**총 43 항목** (30 부모/평면 + 13 자식). validate_registry() 무결성 통과.

### S5 그룹/자식 매핑

```
S5-RESET                      (평면, 단계 1: cleanup)
S5-CSC-DEPLOY [그룹]
  ├ -AGENT-ENROLL  (steps 5,6,7)
  ├ -PKG-UPLOAD    (step 8)
  └ -INSTALL       (steps 9,10)
S5-CSC-VERIFY [그룹]
  ├ -FILES         (step 11)
  └ -OVERLAY       (step 12)
S5-CSC-RUN [그룹]
  ├ -CSC-START     (step 13)
  ├ -CSC-HEALTH    (step 14)
  └ -CONSOLE-START (step 15)
S5-MODULES-DEPLOY [그룹]
  ├ -AUTH          (step 16)
  ├ -PKG-UPLOAD    (step 17)
  ├ -AGENT-ENROLL  (step 18)
  └ -INSTALL       (steps 19,20)
S5-MODULES-RUN [그룹]
  └ -START         (step 21)
S5-FINALIZE                   (평면, step 22)
```

자식 ID 형식: `<부모ID>-<짧은ID>` (dash). backend 의 child-result `<parent>.<child>` 마커 충돌 회피.

### S5 의 _legacy.py 어댑터

`verify/lib/items/stage5/_legacy.py`:
- `get_legacy_results(ctx)`: `cims.sh verify stage5 --legacy` 1회 호출 → ctx.state cache
- `_parse_steps()`: stdout 의 `[VERIFY] step-start/step-end NN status=... elapsed_ms=...` 파싱
- `step_result(by_step, step_nos, ...)`: 자식 함수가 자기 step 결과만 가져와 ItemResult 합산 (worst-status)

**향후 보완 용이성**: 자식 함수 시그니처는 _legacy 의존 — `_verify_phase2` 의 step segment 를 Python 으로 포팅 시 자식 함수 본체만 교체. 인터페이스 동일.

## 핵심 파일 (B 안 전면 재구성)

```
verify/lib/registry.py            — @verify_item(stage, is_group, parent), validate_registry()
verify/lib/runner.py              — group/leaf 펼침, BLOCKED, group-end 마커, expand_to_leaves()
verify/lib/presets.py             — stage1-full ~ stage6-full + pipeline-full + pre-package + post-deploy
verify/lib/context.py             — VerifyContext.create(stage=N, ...)
verify/lib/reporting.py           — *_stageN.md 또는 *_multi.md
verify/lib/shell.py               — run_cims_sh / run / port_listening
verify/lib/common/                — db, subscribers, access_services, cspsim, recordings, cmp_client (옛 그대로)
verify/lib/items/stage{1..6}/     — 항목 정의 (자동 import)

tests/cims_verify.py              — CLI (--stage / --items / --preset)
tests/test_verify_lib.py          — 35 unit tests

cims.sh                            — cmd_verify 가 stage1~stage6 받도록 변경 (phase 제거)

csc/src/handlers/verification.py  — Backend API
   /stages, /stages/<N>, /run, /jobs/<id>, /runs, /runs/<id>, /items, /presets
   _record_run(): job 종료 시 verification_run + verification_run_item INSERT
csc/src/csc_app.py                — ver_init(tests_dir, config) 호출 (config 추가)

ems/core/console/src/api/verification.ts        — verifyApi client
ems/core/console/src/pages/VerificationV2Page.tsx       — LIVE polling (mock 제거)
ems/core/console/src/pages/VerificationHistoryPage.tsx  — 회차 이력 list + DetailModal
ems/core/console/src/routes.tsx                  — /testbed/verify-v2, /testbed/verify-history

sql/migrate_verification_runs.sql  — verification_run + verification_run_item
```

## stdout 마커 형식 (회귀 안전망 — test_verify_lib.py 가 검증)

```
[VERIFY] run-start: total=N ids=...
[VERIFY] item-start: <id> stage=<N> idx=<i>/<N> name=<...>
[VERIFY] item-end:   <id> status=<PASS|FAIL|SKIP|BLOCKED> elapsed_ms=<n>
[VERIFY] child-result: <parent_id>.<child_id> status=... elapsed_ms=... name=...
[VERIFY] group-end:  <parent_id> status=... child_count=<n>
[VERIFY] run-end: total=N pass=n fail=n skip=n blocked=n
```

S5 안에서 cims.sh `_verify_phase2` 가 출력하는 step 마커 (옛 그대로):
```
[VERIFY] step-start: NN <name>
[VERIFY] step-end:   NN status=PASS|FAIL elapsed_ms=...
```
→ `_legacy.py` 가 파싱.

## DB 이력

```sql
verification_run:
  id, started_at, finished_at, elapsed_ms, trigger_type ('user'|'cli'|'ci'),
  scope ('stage<N>' | 'preset:<name>' | 'items'),
  selected_ids (JSON), resume_stage,
  verdict ('PASS'|'FAIL'|'UNKNOWN'), totals (JSON),
  pkg_manifest_hash (sha256 of build/dist/packages/manifest.json),
  git_branch, git_sha, host, ens_ip, report_path, job_id

verification_run_item:
  run_id (FK CASCADE), item_id, stage, parent_id, is_group,
  name, status, elapsed_ms, detail, idx
```

마이그레이션 적용:
```bash
sudo mysql cims < sql/migrate_verification_runs.sql
```

이번 세션 적용 완료.

## 프리셋 12종

```
stage1-full / stage2-full / stage3-full / stage3-quick
stage4-full / stage5-full / stage6-full / stage6-volte / stage6-ptt
pipeline-full   (S1~S6 전체)
pre-package     (S1~S4)
post-deploy     (S5~S6)
```

## CLI 사용

```bash
./cims.sh verify list                            # 항목 트리
./cims.sh verify list-presets                    # 프리셋 목록
./cims.sh verify describe S5-CSC-DEPLOY          # 항목 상세
./cims.sh verify stage3                          # stage3 전체
./cims.sh verify run --stage 6 --items S6-SEED,S6-SCN-VOLTE-VOICE
./cims.sh verify run --preset pipeline-full
python3 -m unittest tests.test_verify_lib -v     # 35 tests
```

## Console UI

- `/testbed/verify-v2` — 6단계 파이프라인 LIVE 페이지
  - 초기 로드: GET /api/v1/verification/stages
  - 실행: POST /api/v1/verification/stages/<N> (단독) 또는 /run (multi-stage)
  - 폴링: GET /api/v1/verification/jobs/<id> (1.5s)
  - 종료 시: 회차 #ID 표시 + 이력 페이지 link
  - LIVE 라벨 (옛 PROTOTYPE 라벨 제거)

- `/testbed/verify-history` — 회차 이력
  - list: 시작시각/scope/verdict/P/F/S/소요/git/pkg hash/trigger
  - 필터: stage / verdict / 페이지네이션 (50건/페이지)
  - 행 클릭 → DetailModal: 메타 + totals + 항목별 표 (부모/자식 트리)
  - 회차 삭제 (cascade)

## 5 commits

1. `dab92b0` refactor(verify): 6단계 파이프라인 — registry/runner/items 재구성 + CLI/Backend API 일원화
2. `e2bc290` feat(verify): 검증 이력 DB 테이블 + /runs API + 종료 시 자동 기록
3. (history page commit) feat(verify): 검증 이력 페이지 신설 (/testbed/verify-history)
4. `d9ba405` feat(verify): V2 페이지 백엔드 연결 — mock 제거 + LIVE 진행 폴링
5. `9e07ef5` test(verify): test_verify_lib.py — stage 체계로 갱신 + 그룹/parent/마커 테스트

## 함정 / 주의

- **TB-CSC 재시작 필요**: 다음 세션에서 csc handler 변경 반영 위해 `./cims.sh restart tb` 또는 tb-csc 재기동.
- **자식 ID 형식**: 옛 mock 페이지는 dot (`S5-CSC-DEPLOY.AGENT-ENROLL`) 사용했지만 backend 마커 파싱 충돌로 dash (`S5-CSC-DEPLOY-AGENT-ENROLL`) 로 통일.
- **S5 자식 단독 실행 X**: _legacy 어댑터가 `_verify_phase2` 22단계 1회 호출 후 결과 분배. 자식만 선택해도 22단계 모두 실행됨. UI 에서 그룹 단위 실행 권장. 향후 Python 포팅 후 자식 단독 실행 가능.
- **immutability gate 미강화**: S4-PKG-MANIFEST 는 sha256 기록만 하고 S6-ENTRY-CHECK 가 매칭 강제하지 않음. 백로그.
- **stage gate 자동 차단 미구현**: 옛 stage FAIL 시 후속 stage 자동 BLOCKED X. 사용자가 각 stage 수동 시작.
- **DB 이력 fallback**: `_record_run()` 은 best-effort — 실패해도 job 자체 영향 X. DB 미연결 환경에서도 run 자체는 동작.
- **/runs API 의 verdict='UNKNOWN'**: backend 가 마커 summary 로 verdict 추정 — multi-stage 는 progress.summary.fail>0 이면 FAIL.
- **docs/VERIFICATION_PROCESS.md** 본문은 옛 체계. 헤더에 새 체계 안내 + 매핑표 추가됨. 본문 통째 재작성은 백로그.
- **PrintReport 분리 안 됨**: V2 페이지의 PrintReport 컴포넌트가 export 안 되어 이력 페이지 detail 에서 재사용 불가. 백로그.
