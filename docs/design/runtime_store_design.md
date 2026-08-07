# 파일 기반 Runtime Store — 설계

> 본 문서 §2 평면 레이아웃은 [runtime_store_v2_module_namespacing.md](runtime_store_v2_module_namespacing.md)
> 의 모듈/버전 귀속 네임스페이스로 개정되었다. 현행 레이아웃은 v2 를 참조.

원칙: **가입자 정보/상태 외 모든 데이터는 DB 가 아닌 파일로 관리**한다. 외부 이중화 DB 인계
부담을 최소화하기 위해 운영/배포/HA/런타임 설정 등은 모두 file-store 에 둔다.

> **규칙 (거버넌스):** 신규 데이터는 **DB 테이블을 새로 만들지 않고 file-store(collection/jsonl)로 시작**한다. DB 는 가입자(person/VoLTE/PTT) 도메인과 조직 트리 등 **관계형이 본질적으로 필요한 데이터에 한정**한다. 새 테이블이 정말 필요하다고 판단되면 먼저 file-store 로 해결되지 않는 이유(JOIN·트랜잭션·FK 무결성 등)를 검토한 뒤에만 추가한다.

## 1. 범위 (도메인별)

| 대상 도메인 | 옛 테이블 |
|---|---|
| **패키지** | `cims_package` |
| **에이전트** | `cims_agent` |
| **배포 + 작업 큐 + 메트릭** | `agent_deployment`, `agent_job`, `agent_metric` |
| **HA 그룹** | `ha_groups`, `ha_group_members` |
| **CSP 런타임 설정** | `csp_listener`, `sip_trunk`, `routing_rule(+match+transform)`, `routing_access_list`, `sip_service`, `sip_service_listener`, `csp_config_audit` |
| **녹취 메타** | `recordings`, `recording_segments` |
| **IdMS 토큰** | `auth_codes`, `refresh_tokens` |

**DB 유지 (영구)**: 가입자 도메인 + 조직
- `users`, `volte_subscriptions`, `ptt_subscriptions`, `user_rejects`, `ptt_groups`, `ptt_group_members`, `ptt_session_seq`, `organizations`

`organizations` 는 `users.org_id` FK 대상이라 가입자와 함께 외부 이중화 DB 에 인계한다.

## 2. 디렉토리 레이아웃

```
{CimsRuntimeDir}/                    # 기본: {ServiceLogDir}/../runtime/ 또는 config 의 CimsRuntimeDir
  packages/
    <name>__<version>.json           # 한 패키지 = 1 파일. 파일명 = uk(name,version)
    .seq                             # 다음 ID (단조 증가, 파일 lock 으로 보호)
  agents/
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

## 4.1 소유권 리스 (단일 writer)

관리 store 는 **단일 writer** 다. 이중화(관리평면 A/S)에서 두 OAM 이 같은 store 를 쓰면
손상되므로, 모든 write 진입점(`save`/`delete`/`next_id`/`jsonl_append`)이 소유권 리스를
확인하고 없으면 `LeaseLostError` 를 던진다(HTTP **409 `not_lease_owner`**). 조회(`load`/
`load_all`/`by_id`)는 제약이 없다 — 소유권이 없으면 **read-only 로 강등**된다.

- 획득: `<runtime>/.owner.lock` 에 배타 `flock`(프로세스 수명 유지) + `<runtime>/.owner.json`
  의 `epoch` +1 기록. OAM 은 store 접근(마이그레이션·seed) 전에 획득한다.
- 펜싱: write 직전(1초 캐시) `.owner.json` 의 `node_id`/`epoch` 가 자기 것과 다르면 소유권을
  잃은 것으로 보고 read-only 로 내려간다 — **시각 비교를 하지 않는다**(노드 간 clock skew
  무관, ha_service_model.md §15).
- 상태 노출: `GET /api/v1/gateway/health` 의 `lease`/`read_only`.
- 구현: `services/lease.py`. 설계·3층 방어(파일시스템 펜싱 → mount guard → 리스):
  [features/oam_ha.md](features/oam_ha.md) §4.

> 단일 노드 구성에서도 같은 경로를 지난다(획득이 성공하므로 무영향). `--preflight` 실행은
> 잠금을 잡지 않는다 — 살아있는 OAM 의 소유권을 건드리면 안 되므로.

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

## 8. 도메인 매핑 (구현 위치)

각 도메인 핸들러는 file_store(`load_all()`/`load(id)`/`save(id, data)`/`delete(id)`)만 읽고
쓴다. DB JOIN 은 handler 가 명시적으로 처리(또는 lazy)한다.

- **packages**: cims_package. `agent_deployment.package_id` JOIN 은 client-side enrich(`_enrich_deploy_with_pkg`).
- **agents**: agents.py CRUD / agent_api.py 핫패스(enroll/heartbeat/cert/metric) / ha_groups.py 멤버 enrich / csc_app.py sweeper(stale offline + cert rotate). agent_deployment JOIN 은 `_enrich_deploy` 로 pkg + agent 통합.
- **deployments/jobs/metrics**: agent_deployment CRUD + agent_job CRUD + JSONL 시계열 metric. `_job_pick_pending`(heartbeat 큐 pick), `_metric_append`/`_metric_load_recent`(시계열), `_job_create`(INSERT). report 핸들러는 agent_job 갱신 + agent_deployment 상태 hook(install_path 추출 포함).
- **ha_groups**: members 배열을 그룹 JSON 안에 임베드. CRUD + vrid 자동 할당(file_store 순회 기반) + agent_name enrich. agents.py 의 `_ha_group_map_for_agents`/`_check_ha_capability` 도 file_store.
- **csp runtime config**: csp_runtime.py 5 entities(listener/trunk/route/access/service) + audit JSONL. config_cache.py 도 file_store 로드. routing_rule 의 match/transform, sip_service 의 listeners 는 그룹 JSON 안에 임베드. CSP C++ 는 jsonl 파일이 SoT(access_services.jsonl 등) — 본 도메인은 Console 측 정리.
- **recordings**: CSC handlers 가 파일 기반(call.json + recordings/). CSP `InsertRecording` no-op.
- **auth tokens**: `csc/src/services/idms_storage.py` 의 auth_codes/refresh_tokens 도메인.
- **organizations**: DB 유지 — `users.org_id` FK 대상이라 가입자 도메인과 함께 외부 이중화 DB 인계(`csc/src/handlers/org.py` 가 DB CRUD).

## 9. 백업 / 보존

- `CimsRuntimeDir` 전체를 tar 로 백업 (compose 기반 배포의 volume 와 동일 단순성).
- 일별 jsonl (jobs/metrics) 는 운영 정책으로 30~90일 보존 후 자동 삭제 (cron + `find -mtime`).

## 공유 스토리지(NFS)에서의 접근 비용

관리 store 를 공유 스토리지로 옮기면 **파일 1건 읽기가 로컬의 ~100배**가 된다(실측: NFS
약 5ms/파일). 콘솔은 2초 폴링, agent 는 2초 heartbeat 이므로 접근 횟수가 그대로 체감 지연이
된다. 그래서 다음 규칙을 지킨다.

| 규칙 | 이유 |
|---|---|
| `by_id` 는 **파일명 직접 조회(`<id>.json`) 우선**, 실패 시에만 전체 스캔 | 옛 구현은 무조건 디렉터리 전체를 읽었다. job 120건 store 에서 **단건 조회 1회 = 120파일** — heartbeat·조회마다 반복돼 콘솔 전체가 느려졌다(실측 최대 병목) |
| 대기 job 은 **agent 별 인덱스**(`control/job_index/<agent_id>.json`)로 찾는다 | `_job_pick_pending` 이 전 job 을 읽어 필터하던 것이 heartbeat×agent수 만큼 반복됐다. 인덱스는 캐시이므로 부재·손상 시 전체 스캔으로 재구축한다(정본은 job 파일) |
| 완료 job 은 **정리한다** (`JobRetentionDays`=2, `JobRetentionCount`=200) | 무한 누적이 상시 비용이 된다. 미완(queued/running)은 절대 지우지 않는다 |
| 여러 레코드를 훑는 응답은 **한 번만 읽어 공유**한다 | 그룹 목록에서 그룹마다 배포·agent 를 다시 읽으면 O(그룹수 × 레코드수)가 된다. 같은 실수를 배포·agent 두 번 했다 — 테스트가 "그룹 N개 조회 시 스캔 1회"를 단정한다 |
| 무거운 조회는 **폴링 주기를 분리**한다 | 콘솔 시스템/인프라는 실측 상태(agents·deployments)만 2초, 거의 안 바뀌는 packages·ha-groups 는 6초. 변이 직후에는 전체를 즉시 갱신한다 |
