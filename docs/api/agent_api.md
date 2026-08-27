# Agent API (Agent ↔ OAM)

Agent 데몬이 OAM 과 통신하는 프로토콜. **Agent 가 OAM 에 요청하는** 측면만 여기서 다룹니다. OAM 이 Agent 에 요청하는 Collection 프록시는 `collection_api.md` 참조.

**Base URL**: `https://<OAM>:4419/api/agent`
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

주기 호출(기본 2초). pending job 반환 + 현재 네트워크/마운트 상태 보고.

**Headers**: `X-Agent-Token: <session_token>`

**Request**
```json
{
  "sync_port": 9900,
  "agent_version": "0.0.57",
  "interfaces": [{"name":"ens4","ip":"10.0.1.45","mask":24,"managed":true}],
  "routes":     [{"dst":"...","via":"...","dev":"...","managed":true}],
  "mounts":     [{"source":"121.161.164.105:/home/cbm/NAS/cims","target":"/mnt/cims","fstype":"nfs","options":"defaults,_netdev,nofail","mounted":true}]
}
```

- `sync_port` (선택): Agent 가 노출하는 Sync REST 포트. CSC 는 이 값을 `cims_agent.sync_port` 에 저장하고 collection 프록시 시 사용
- `interfaces`/`routes`: NIC IP·route 보고. `managed=true` = cims-priv 가 부여한 `<iface>:cims` 라벨 IP 보유 NIC(편집/삭제 허용). agent 레코드에 저장(`_normalize_interface_roles` 로 mgmt 자동 도출).
- `mounts`: `/etc/fstab` 의 `# cims-managed` 마운트 + 현재 `mounted` 여부(`collect_mounts`). 콘솔 MountPanel 표시용 — agent 레코드에 보존.

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
  ],
  "module_events": [
    {"module":"cmp","event":"process_died"}
  ],
  "cfg_hashes": {"csp": "c40e66e87ce3"},
  "ha_transitions": 0
}
```

- `cpu_pct` — 호스트 CPU% (`/proc/stat` aggregate cpu 라인 2 sample delta, `_host_cpu_pct`). psutil 무의존. 첫 sample 은 `null`, 다음부터 값.
- `load_avg` — `/proc/loadavg` 1/5/15분, 쉼표 구분 문자열.
- `per_iface` — `/proc/net/dev` delta. iface 별 누적 bytes + rate(B/s).
- `mounts` — `/proc/mounts` 순회(가상 fs·bind·중복 device 제외, `/dev/*` 만) + `statvfs` 마운트별 사용률. 기존 root `disk_pct` 와 별개.
- `modules` — 실행 중 CIMS 모듈 (pid/cpu_pct/mem_mb). 탐지 대상은 `_metric_module_names()` (아래 ⚠ 참조).
- `processes` — 하위호환 필드 (modules 와 중복, name/pid/cmdline).
- `module_events` — 직전 tick 대비 모듈 소멸 전이(`process_died`) 관측 — 발생 tick 에만 동봉.
  OAM 이 event_log 로 기록한다(L1 이벤트 — [alarm_pipeline.md](../design/alarm_pipeline.md) §4.1).
- `cfg_hashes` — 설치 모듈별(중지 모듈 포함) 배포 `config.json` canonical hash 12hex —
  OAM 이 배포기록 실체화본 hash 와 비교해 config drift(`A-PRC-003`) 판정. 파일 없음/파싱
  실패 모듈은 생략(OAM 평가 제외).
- `ha_transitions` — 최근 10분 keepalived 전이 수 (ha_flap 임계 판정 입력). 0/무전이 시 생략.

> ⚠️ CSC(OAM) 의 `agent_api.py _metric()` 는 record 화이트리스트로 필드를 거른다 — **신규 metric 필드(`mounts` 등)는 화이트리스트에 명시 추가하지 않으면 저장 시 버려진다**. 마찬가지로 응답 직렬화(`_agent_metrics._row`)에서도 `per_iface`/`mounts` 를 노출해야 대시보드에 전달된다.

> ⚠️ 모듈 liveness 탐지(`_metric_module_names()`)는 `DEFAULT_INSTALL_ROOT`(`<prefix>/modules`) listdir ∪ **`supervised.json` 키** − 비데몬(`agent`·`console`). 고정 기본 집합은 없다 — **agent 가 설치한 모듈만** 보고한다(미설치 모듈까지 pgrep 하면 동거 프로세스 오귀속·유령 전이의 원천). `install_path` 가 agent 트리 밖인 모듈(isp/psp 등)은 supervised 로 합쳐 경로 독립적으로 탐지해 OAM `module_down` 오탐을 막는다.

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
| `upgrade_agent` | agent 업그레이드 (`install-agent.sh --update-only` → bundle 다운로드 → `agent/<신버전>/` 전개 → `current` flip → execv; 구버전 prune 3개 보존) | (없음) |
| `rollback_agent` | agent 롤백 (`current` 를 직전/지정 버전으로 flip → execv; 다운로드 불요) | `version` (생략 시 직전) |
| `health_check` | 포트 probe | `process_name` |
| `collect_log` | 로그 일부 반환 | `log_path` |
| `apply_ip_config` | service IP/route 적용 (cims-priv) — sync REST `/apply-ip-config` 와 동일 경로 | `service_ip_rows[]`, `routes[]` |
| `apply_mounts` | 마운트 적용 (cims-priv mount-add/del → `/etc/fstab` 영속) — sync REST `/apply-mounts` | `mounts[]` (`{op,fstype,source,target,options?}`) |
| `update_ha` | HA 구성 반영 — `<prefix>/run/keepalived/ha.json` 기록 후 `cims-ha` 실행 | `ha_json` (`ha_intent` 포함 — 아래) |

### `update_ha` 의 `ha_intent`

`ha_json.ha_intent` 가 agent 의 동작을 결정한다. **파괴적 동작(무장 해제)은 명시적
의도에서만** 일어난다 — 배포 진행 중의 미정 상태가 해제를 유발하지 않게.

| `ha_intent` | OAM 이 보내는 상황 | agent 동작 |
|---|---|---|
| `armed` | AS 그룹 + VIP 렌더됨 | `cims-ha install → config → apply`. 서비스별 실제 무장은 `services.<svc>.enabled` 가 정한다 |
| `disarmed` | AA 그룹(keepalived 미사용), 그룹 이탈·삭제 | `cims-ha disarm` — CIMS 소유 HA 구성만 제거. **keepalived 패키지는 보존** |
| `unknown` | AS 인데 VIP 미설정 등 아직 미정 | **no-op** — ha.json 만 기록하고 현 상태 보존 |

필드가 없는 구 OAM 호환: `services` 가 있으면 `armed`, 비어 있으면 `unknown` 으로 본다
(안전측 — 해제하지 않는다).

> ⚠️ 모듈은 `/opt/cims-agent/modules/<module>/<ver>/` (agent/ 트리 밖) 에 설치된다. `upgrade_agent`/`rollback_agent` 는 `agent/current` 심볼릭만 flip 하므로 모듈 바이너리에 영향이 없다. agent·state·run·sub-script 가 버전 트리 밖에 있어야 하는 영속(durability) 제약은 `docs/design/02_deployment.md` §2 참조.
