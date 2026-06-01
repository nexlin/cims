# Agent API (Agent ↔ CSC)

Agent 데몬이 CSC 와 통신하는 프로토콜. **Agent 가 CSC 에 요청하는** 측면만 여기서 다룹니다. CSC 가 Agent 에 요청하는 Collection 프록시는 `collection_api.md` 참조.

**Base URL**: `https://<CSC>:4420/api/agent`
**인증**:
- `/enroll` 은 `enrollment_token` (1회용, body 에 전달)
- 나머지는 `X-Agent-Token` 헤더 (enroll 응답의 `session_token`)

---

## POST /api/agent/enroll

최초 기동 시 enrollment 토큰을 session 토큰으로 교환.

**Request**
```json
{
  "enrollment_token": "81c9...",
  "hostname": "host01",
  "os_info": "Linux 6.x",
  "cpu_cores": 8,
  "memory_mb": 15945,
  "disk_gb": 100,
  "agent_version": "0.1.0"
}
```

**Response 200**
```json
{
  "agent_id": 50,
  "session_token": "abda...",
  "name": "test-server-1",
  "status": "approved"
}
```

**상태 머신**: `pending`/`approved`/`online`/`offline` 모두 enroll 허용 (re-install / host 복구 시나리오). `revoked` 만 차단 — 명시적으로 폐기된 record 는 token 재발급 받아도 enroll 불가.

**Response 401**:
- `invalid_enrollment_token` — token 매치되는 record 없음 또는 record status='revoked'
- `enrollment_token_expired` — TTL (기본 600s) 만료 → Console 에서 재발급 필요

---

## POST /api/agent/heartbeat

30초 주기 호출. pending job 반환.

**Headers**: `X-Agent-Token: <session_token>`

**Request**
```json
{
  "sync_port": 9900
}
```

- `sync_port` (선택): Agent 가 노출하는 Sync REST 포트. CSC 는 이 값을 `cims_agent.sync_port` 에 저장하고 collection 프록시 시 사용

**Response 200**
```json
{
  "ok": true,
  "jobs": [
    {
      "id": 48,
      "type": "install",
      "params": {
        "deployment_id": 43,
        "package_id": 98,
        "package_name": "csp",
        "package_version": "0.0.1",
        "process_name": "CSP",
        "service_functions": ["volte","ptt","ibcf"],
        "install_path": null,
        "config": { "SipServer.Realm": "csp", ... }
      }
    }
  ]
}
```

**Response 401**: `session_token` 유효하지 않음 (폐기됨) — Agent 는 종료해야 함

---

## POST /api/agent/report

Job 실행 결과 보고.

**Headers**: `X-Agent-Token`

**Request**
```json
{
  "job_id": 48,
  "status": "succeeded",       // succeeded | failed | cancelled
  "result_code": 0,
  "stdout": "installed pkg_id=98 at /path/... (12345 bytes)",
  "stderr": ""
}
```

**Response 200**
```json
{"ok": true, "updated": 1}
```

CSC 는 결과에 따라 다음을 자동 처리:
- `install` 성공: `agent_deployment.install_path` 를 stdout 에서 파싱해 저장, `status=stopped`
- `start/restart` 성공: `status=running`
- `stop` 성공: `status=stopped`
- `uninstall` 성공: `status=removed`

---

## GET /api/agent/package/{id}

패키지 tar.gz 다운로드 (install job 수행 중 Agent 가 호출).

**Headers**: `X-Agent-Token`

**Response 200**: `application/octet-stream` (tarball raw bytes)
- Header `X-Package-Sha256`: 기대 SHA256 (Agent 가 검증)

---

## POST /api/agent/metric

리소스 메트릭 주기 보고 (**2초 주기** — `DEFAULT_METRIC_SEC`). 대시보드 "시스템 리소스" 위젯의 실시간 추이 소스.

**Headers**: `X-Agent-Token`

**Request**
```json
{
  "cpu_pct": 12.5, "mem_pct": 40.0, "disk_pct": 33.2,
  "load_avg": "0.1,0.2,0.3",
  "per_iface": [
    {"iface":"ens3","rx_bytes":12345,"tx_bytes":6789,"rx_rate":1024,"tx_rate":512}
  ],
  "mounts": [
    {"mount":"/","device":"/dev/sda1","total":107374182400,"used":35433480192,"pct":33.2}
  ],
  "modules": [
    {"name":"csp","pid":12345,"cpu_pct":2.1,"mem_mb":150}
  ],
  "processes": [
    {"name":"csp","pid":12345,"cmdline":"..."}
  ]
}
```

- `cpu_pct` — 호스트 CPU% (`/proc/stat` aggregate cpu 라인 2 sample delta, `_host_cpu_pct`). psutil 무의존. 첫 sample 은 `null`, 다음부터 값.
- `load_avg` — `/proc/loadavg` 1/5/15분, 쉼표 구분 문자열.
- `per_iface` — `/proc/net/dev` delta. iface 별 누적 bytes + rate(B/s).
- `mounts` — `/proc/mounts` 순회(가상 fs·bind·중복 device 제외, `/dev/*` 만) + `statvfs` 마운트별 사용률. 기존 root `disk_pct` 와 별개.
- `modules` — 실행 중 CIMS 모듈 (pid/cpu_pct/mem_mb). 탐지 대상은 `_metric_module_names()` (아래 ⚠ 참조).
- `processes` — 하위호환 필드 (modules 와 중복, name/pid/cmdline).

> ⚠️ CSC(OAM) 의 `agent_api.py _metric()` 는 record 화이트리스트로 필드를 거른다 — **신규 metric 필드(`mounts` 등)는 화이트리스트에 명시 추가하지 않으면 저장 시 버려진다**. 마찬가지로 응답 직렬화(`_agent_metrics._row`)에서도 `per_iface`/`mounts` 를 노출해야 대시보드에 전달된다.

> ⚠️ 모듈 liveness 탐지(`_metric_module_names()`)는 `_DEFAULT_METRIC_MODULES`(csp/cmp/csc/cwrtc) ∪ `DEFAULT_INSTALL_ROOT` listdir ∪ **`supervised.json` 키**. install_path 가 agent 트리 밖(`/opt/cims-agent/<module>`)인 모듈은 listdir 로 안 잡혀 OAM `module_down` 알람이 false 로 뜨므로, 워치독 등록(supervised.json)을 합쳐 경로 독립적으로 탐지한다.

**Response 200**: `{"ok": true}`

---

## Job 타입 참조

| 타입 | 용도 | 필수 params |
|---|---|---|
| `install` | 패키지 설치 | `package_id`, `process_name` (또는 `install_path`) |
| `upgrade` | 재설치 (install 과 동일) | 위와 동일 |
| `uninstall` | 디렉토리 제거 | `install_path` |
| `start` / `stop` / `restart` | 프로세스 제어 | `install_path`, `process_name` |
| `update_config` | config.json 재기록 | `install_path`, `config` |
| `upgrade_agent` | agent 바이너리 교체 (`install-agent.sh --update-only` → bundle 다운로드 → `agent/` 트리 교체: old→`agent.old`→삭제) | (없음) |
| `health_check` | 포트 probe | `process_name` |
| `collect_log` | 로그 일부 반환 | `log_path` |

> ⚠️ `upgrade_agent` 는 `agent/` 트리를 통째로 교체하므로 **모듈 install_path 는 agent/ 트리 밖**(`/opt/cims-agent/<module>`)이어야 한다. 안에 두면 upgrade 시 모듈이 deleted-inode 좀비로 파괴된다 — 상세는 `docs/design/02_deployment.md` §2 install_path durability 제약.
