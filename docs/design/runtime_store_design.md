# 파일 기반 Runtime Store — 설계

> 2026-05-13 결정: **가입자 정보/상태 외 모든 데이터는 DB 가 아닌 파일로 관리**.
> 외부 이중화 DB 인계 부담 최소화. 운영/배포/HA/런타임 설정 등은 모두 file-store.

## 1. 범위 (단계별)

| Phase | 대상 도메인 | 옛 테이블 |
|---|---|---|
| 1 | **패키지** | `cims_package` |
| 2 | **에이전트 + 인스턴스** | `cims_agent`, `cims_instance` |
| 3 | **배포 + 작업 큐 + 메트릭** | `agent_deployment`, `agent_job`, `agent_metric` |
| 4 | **HA 그룹** | `ha_groups`, `ha_group_members` |
| 5 | **CSP 런타임 설정** | `csp_listener`, `sip_trunk`, `routing_rule(+match+transform)`, `routing_access_list`, `sip_service`, `sip_service_listener`, `csp_config_audit` |
| 6 | **모니터링 집계** | `stats_daily`, `stats_monthly`, `stats_yearly` |
| 7 | **녹취 메타** | `recordings`, `recording_segments` |
| 8 | **IdMS 토큰** | `auth_codes`, `refresh_tokens` |
| 9 | **조직** | `organizations` |

**DB 유지 (영구)**: 가입자 도메인만
- `users`, `volte_subscriptions`, `ptt_subscriptions`, `user_rejects`, `ptt_groups`, `ptt_group_members`, `ptt_session_seq`

## 2. 디렉토리 레이아웃

```
{CimsRuntimeDir}/                    # 기본: {ServiceLogDir}/../runtime/ 또는 config 의 CimsRuntimeDir
  packages/
    <name>__<version>.json           # 한 패키지 = 1 파일. 파일명 = uk(name,version)
    .seq                             # 다음 ID (단조 증가, 파일 lock 으로 보호)
  agents/
    <id>.json
    .seq
  instances/
    <id>.json
    .seq
  deployments/
    <id>.json
    .seq
  ha_groups/
    <id>.json                        # 멤버 배열을 같은 JSON 안에 임베드
    .seq
  jobs/
    <agent_id>/
      YYYY/MM/DD.jsonl               # 일별 회전. status 변경은 새 라인 append (이벤트 소싱)
      open.json                      # in-flight job 인덱스 (id → 파일 경로)
  metrics/
    <agent_id>/
      YYYY/MM/DD.jsonl               # 메트릭 시계열 (원본)
      latest.json                    # 최신 1건 quick-read
  sip_service/<id>.json + .seq
  sip_trunk/<id>.json + .seq
  routing_rule/<id>.json + .seq      # match/transform 은 rule json 에 임베드
  ...
```

## 3. 단일 엔티티 파일 포맷

```json
{
  "id": 7,
  "name": "csp",
  "version": "0.1.4",
  "...": "...",
  "create_time": "2026-05-13T17:30:00",
  "update_time": "2026-05-13T17:30:00"
}
```

- 키 = 의미 있는 자연키 (`<name>__<version>.json`) 가 있으면 그것, 없으면 `<id>.json`.
- `id` 필드는 항상 포함 (호환성 — 옛 DB row 와 동일한 응답 형태 유지).
- `create_time` / `update_time` = ISO-8601, atomic write 시 mtime 으로 검증.

## 4. ID 할당

각 도메인 디렉토리에 `.seq` 파일 두고 단조 증가:
- `next_id(domain)`: flock(.seq) → read int → write int+1 → return.
- 빈 디렉토리는 1부터. 최초 부팅 시 디렉토리 스캔으로 max id 발견 시 .seq 동기화.
- 옛 DB 마이그레이션 데이터는 원래 id 그대로 보존 (.seq 는 max(id)+1 로 시드).

## 5. Atomic Write

표준 패턴 (모든 write 가 따름):
```python
def _atomic_write(path, content):
    tmp = path + ".tmp"
    with open(tmp, 'w') as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    os.rename(tmp, path)
```

- JSONL append 는 `O_APPEND | O_WRONLY` 로 단일 write — POSIX append-atomic.
- 다 프로세스 동시 쓰기 가능성 있는 .seq / open.json 등은 `fcntl.flock` 으로 보호.

## 6. 조회 패턴

- **list**: `glob(domain/*.json)` → 각 파일 load. 정렬은 메모리에서. 100건 이하 = OK.
- **get by id**: `<id>.json` 직접 read.
- **get by 자연키**: `<key>.json` 직접 read.
- **filter**: list 후 메모리 필터 (인덱스 불필요 — 모두 100~1000 row 미만 도메인).

**예외**: jobs / metrics 는 시계열 → 일별 jsonl 스캔 (call.json 패턴과 동일).

## 7. FK / JOIN 대체

- DB JOIN → handler 가 두 도메인을 각각 load 한 뒤 메모리에서 attach.
- 예: `agent_deployment` list 시 `cims_agent`, `cims_package` 캐시 후 enrich.
- "캐시" 는 단일 요청 scope (handler 함수 내 dict 1번 빌드). 디스크 read 횟수 = O(도메인 수).

## 8. 마이그레이션 절차 (도메인 단위)

```
A. 데이터 마이그레이션 스크립트:
   scripts/migrate_to_file_store.py --domain <name>
   → DB 에서 SELECT → file-store 에 write → DROP TABLE (옵션, 안전상 _legacy 로 rename 권장)

B. handler 수정:
   - SELECT → file_store.load_all() / load(id)
   - INSERT → file_store.save(new_id, data)
   - UPDATE → file_store.save(id, merged)
   - DELETE → file_store.delete(id)
   - FK CASCADE → handler 가 명시적 처리 (또는 lazy)

C. 검증:
   - S1 (lint/typecheck/format/unit)
   - curl smoke (handler 별 list/get/create/delete)
   - LIVE: TB-CSC 재기동 후 Console UI 기존 페이지 회기능 확인
```

## 9. 호환 정책 (전환기)

- 마이그레이션 완료 도메인은 DB 테이블 DROP **하지 않고** `_legacy` 로 rename → 1 릴리스 후 DROP.
- handler 는 file_store 만 읽음 (DB 우회). 옛 DB row 는 마이그레이션 스크립트가 옮긴 뒤 무시.
- 외부 DB 인계 시점에는 이미 가입자 도메인만 남음.

## 10. 백업 / 보존

- `CimsRuntimeDir` 전체를 tar 로 백업 (compose 기반 배포의 volume 와 동일 단순성).
- 일별 jsonl (jobs/metrics) 는 운영 정책으로 30~90일 보존 후 자동 삭제 (cron + `find -mtime`).

## 11. 진척 추적

| Phase | 상태 | 노트 |
|---|---|---|
| 1. packages | 🟢 **완료** (2026-05-13) | file_store 헬퍼 + cims_package 마이그레이션 (`csc/scripts/migrate_packages_db_to_file.py`). agent_deployment.package_id JOIN 4건 client-side enrich (`_enrich_deploy_with_pkg`). 9 패키지 LIVE 마이그레이션 확인. |
| 2. agents/instances | 🟢 **완료** (2026-05-13) | `csc/scripts/migrate_agents_db_to_file.py` (0 instance + 9 agent). agents.py CRUD / agent_api.py 핫패스(enroll/heartbeat/cert/metric) / ha_groups.py 멤버 enrich (3 JOIN 제거) / csc_app.py sweeper(stale offline + cert rotate) 모두 file_store. agent_deployment JOIN 은 `_enrich_deploy` 로 통합 (pkg + agent + instance). LIVE: agents/deployments/ha-groups 정상 응답. |
| 3. deployments/jobs/metrics | 🟢 **완료** (2026-05-13) | `csc/scripts/migrate_deployments_jobs_metrics_db_to_file.py` (21 deploy + 42 job + 427 metric). agent_deployment CRUD + agent_job CRUD + JSONL 시계열 metric. `_job_pick_pending` (heartbeat 큐 pick), `_metric_append`/`_metric_load_recent` (시계열). agent_job INSERT 5건 모두 `_job_create` 호출. report 핸들러: agent_job 갱신 + agent_deployment 상태 hook (install_path 추출 포함). LIVE: deployments/agent_metrics/packages/agents/alerts 회기능 정상. |
| 4. ha_groups | 🟢 **완료** (2026-05-13) | `csc/scripts/migrate_ha_groups_db_to_file.py` (0 row). ha_groups.py 전면 재작성 — members 배열을 그룹 JSON 안에 임베드. CRUD + vrid 자동 할당 (file_store 순회 기반) + agent_name enrich. agents.py 의 `_ha_group_map_for_agents` / `_check_ha_capability` 도 file_store. LIVE smoke: AS 그룹 생성 → vrid=51 자동, 멤버 priority 정렬 OK, delete 성공. |
| 5. csp runtime config | ⚪ 대기 | 독립 가능 (병렬) |
| 6. monitoring stats | ⚪ 대기 | 독립 |
| 7. recordings | ⚪ 대기 | 독립 |
| 8. auth tokens | ⚪ 대기 | 독립 (보안 검토 필요) |
| 9. organizations | ⚪ 대기 | 가입자와 FK — 마지막 |
