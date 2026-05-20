---
name: 2026-05-08 세션 변경사항 (uncommitted)
description: 본 세션에서 적용한 검증/배포/Flow 관련 8개 영역 fix + 신규 모듈. 39개 파일 수정 (38 modified + live_store.py 신규). 미커밋 — 다음 세션 시작 시 git status 로 확인 + 의도한 묶음대로 commit 권장.
type: project
originSessionId: 28af9309-57bc-4b49-ba8f-6a19c4447828
---
# 2026-05-08 세션 — uncommitted changes 인덱스

**시작 base**: `fc855d2` (2026-05-07 종료 시점). **종료 시 커밋 없음**.
**테스트**: 161 unit tests OK, pipeline-full --enable-mtls 36/36 PASS (이전 세션 baseline 유지).

## 변경 묶음 (commit 단위로 끊을 때 참고)

### 1. Stage 표시 순서 = 실행 순서 통일 (S1~S6 execution_order 명시)
**Why**: V2 페이지가 항목을 ID 알파벳 순으로 표시했지만 실제 실행은 `depends_on` 위상 정렬 → 순서 mismatch (예: S3 표시 = `CONFIGURE, HEALTH, SCN-PTT, SCN-VOIP, SEED, START`, 실행 = `CONFIGURE → START → SEED → ...`).

**파일**:
- `verify/lib/items/stage1/{py_syntax,frontend_lint,frontend_typecheck,cpp_format,unit_verify_lib}.py` — 10/20/30/40/50
- `verify/lib/items/stage2/{preflight,build}.py` — 10/20
- `verify/lib/items/stage3/{reset,configure,start,seed,voip_smoke,ptt_smoke,health}.py` — 10/20/30/40/50/60/70
- `verify/lib/items/stage4/{pkg_build,pkg_manifest}.py` — 10/20
- `verify/lib/items/stage6/{entry_check,seed,volte_voice,volte_video,ptt_voice,scn_mcptt_floor_grant,ptt_video,scn_subscribe,scn_l7_subscribe_notify,scn_db_sync,scn_cmp_group_sync,scn_cert_rotate,summary}.py` — 10/20/30/40/50/51/60/70/71/80/81/90/100

### 2. configure.sh 의 idempotent guard 버그 fix
**파일**: `configure.sh`
**Why**: `[[ "$(cat "$path")" == "$content" ]]` — bash 의 `$(cat ...)` 가 trailing newline 을 strip 해서 항상 false → mtime 갱신 → vite hot reload → React state reset.
**Fix**: `cmp -s` 로 바이트 정확 비교.

### 3. live_store + /active 엔드포인트 (검증 실행 가시화 통합)
**파일**:
- `verify/lib/live_store.py` (신규) — `verify_runs/live/<id>/{meta.json, stdout.log}` 관리
- `tests/cims_verify.py` — `_TeeStream` + `cmd_run` 시작/종료에 live_store
- `csc/src/handlers/verification.py` — `_start_job`/`_watch_job` live_store 사용, `/api/v1/verification/active` 엔드포인트, `/jobs/<id>` live_store fallback
- `cims-console/src/api/verification.ts` — `getActive` + `ActiveRunSummary`
- `cims-console/src/pages/VerificationV2Page.tsx` — mount 시 active 자동 attach

**효과**: 다른 페이지로 이동 → 복귀 시 진행 중 회차 자동 재부착. CLI 직접 실행 회차도 동일 시야로 표시.

### 4. S5 stale test-agent 정리 + cims_agent ETXTBSY 회피
**파일**:
- `verify/lib/items/stage5/_native_steps.py` — `_prepare_test_agent_slot()` + `_kill_listener_on_port()` helper. step_07 (csc) + `_spawn_one_module_agent` (csp/cmp/sim) spawn 직전 호출. 같은 `--name` cims_agent 종료 + sync_port LISTEN 점유 해제 + state_dir wipe.
- `agent/cims_agent.py` — `job_install` 에서 `tarfile.extractall` 직전 install_path 하위 모든 파일 unlink. 실행 중 binary 의 inode 보존 + 새 파일 교체로 ETXTBSY 회피.

**Why**: pipeline-full 이 RESET 미포함이라 직전 회차 test-agent 가 9903 점유 → 새 agent bind 실패 + state_dir 잔존 → install-poll 60s timeout → S5 5건 도미노 FAIL.

### 5. PTT 사전 cleanup 제거 + cmd UPPERCASE 규약 준수
**파일**:
- `verify/lib/items/stage3/ptt_smoke.py` — `remove_group()` 사전 호출 제거. CSP startup 의 자동 ADD_PTT_GROUP 으로 충분 + CSP 캐시 desync 회피.
- `verify/lib/common/cmp_client.py` — `"cmd": "removeGroup"` → `"REMOVE_PTT_GROUP"` (CSP↔CMP 프로토콜 대문자 규약).

**Why**: ptt_smoke 가 시나리오 시작 전에 CMP 에 직접 removeGroup 보냈지만 CSP 캐시 (m_mapPttSession) 는 그대로 → CSP 가 ADD 안 보내고 곧장 JOIN_PTT_GROUP → CMP 'ERROR Group Not Found'.

### 6. Flow viewer — CSC raw-data 원칙 정립
**파일**: `csc/src/services/flow_logger.py`
- `_search_cmp_messages` — HEARTBEAT 무조건 제외 + cross-service method 블랙리스트 (VoLTE flow 에서 PTT method, PTT flow 에서 SESSION method) **모두 제거**. sesid 매칭만 사용.
- `_handle_ptt_history` flow — events.jsonl 시간 범위 + group_id substring 매칭 + lifecycle 시간 우회 트릭 폐기. **sesid 추출 → sesid 매칭** (시간 무관) 으로 통일. legacy substring 은 fallback 으로 보존.

**Why**: CSC = 디버깅 데이터 액세스 계층. 표시 레벨 처리 (HEARTBEAT 숨김, method 블랙리스트) 는 console 책임. 사용자 명시 원칙: "csc 는 검색 조건 외의 필터링은 하면 안되고 동일 서비스 세션 여부만 확인해서 출력".

**검증**: PTT flow 가 ADD_PTT_GROUP (startup-time) 부터 GROUP_TIMEOUT (종료 후) 까지 전 라이프사이클 표시. VoLTE flow 도 정상.

## 미해결 / 추후 점검
- 2회 이상 연속 pipeline-full 실행 시 csp/cmp 바이너리 ETXTBSY 는 fix 했지만, 다른 잠재 충돌 가능성 (config dir 상태 등) 은 현재 환경에서 미관찰.
- HEARTBEAT 노이즈 — 현재 console 에서 그대로 표시됨. 사용자가 필터 토글 요청하면 console 쪽에 추가.

## 다음 세션 cold-start

```bash
cd /home/nex/work/cims
git status --short | wc -l   # 39 (commit 안 한 상태) — 확인 후 묶어서 commit 권장
git log -5 --oneline          # fc855d2 가 마지막 commit
python3 -m unittest tests.test_verify_lib  # 161 OK
ls verify_runs/live/ | head   # 활성 회차 디렉토리
```

다음 세션 의제는 [project_prod_topology.md](project_prod_topology.md) 참조 — VoLTE/PTT 서비스 서버 분리 + ISP/IMP/PSP/PMP 신규 모듈 검증 파이프라인 반영.
