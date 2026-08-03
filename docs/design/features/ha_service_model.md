# HA 서비스 운영 모델 — 책임 분리 · 선언적 verdict · 절체 판정

> 이 문서는 CIMS HA 의 **무장 판정·장애 감지·로컬 복구·절체 결정·계획 절체**의 정본
> 설계다. keepalived 인프라(파일 구조·렌더·apply)의 운영 상세는
> [../ha_design.md](../ha_design.md) §11 을, 토폴로지 개요는 ha_design.md §1~8 을 본다.
>
> **단일 모델**: legacy 경로·모드 플래그·단계적 이행은 없다. 새 supervisor 모델이 유일한
> 동작이며, 유일한 escape 는 비상 밸브 `CIMS_HA_DISABLE`(§9·§18) 하나다.
>
> **적용 범위**: 아래 verdict / eligible / promotion / 절체 모델은 **VIP 를 가진
> Active/Standby(AS) 그룹 전용**이다. All-Active(AA)는 VIP·keepalived 가 없어 이
> 모델의 대상이 아니며, §3 에 그 경계를 명시한다.

## 1. 목적과 원칙

HA 의 책임은 **모듈/노드 이상 시 서비스를 다른 노드로 넘기는 것** 하나다. "이상이
있는지 없는지"를 판단하고 복구를 시도하는 것은 별개 책임이며, HA(keepalived)는 그
판정 결과만 입력으로 받는다. 이 분리를 어기면 판정 주체와 복구 주체가 서로의 상태를
넘겨짚어 교착·flap 이 발생한다.

원칙:

1. **절체는 장애 또는 명시적 절체 명령으로만 일어난다. 운영자 조작은 장애가 아니다.**
2. **운영자 의도는 유추하지 않고 명시적으로 저장한다.** 배포 record 에서 역산하지 않는다.
3. **로컬 복구(재기동)가 절체보다 먼저다.** 복구 시도가 소진됐을 때만 절체한다
   (Pacemaker migration-threshold, systemd StartLimitBurst 계열).
4. **앱 설정과 운영 설정은 물리적으로 다른 파일이다.** 모듈 config.json(앱이 읽음)과
   운영·감시 명세(HA 가 읽음)는 저장·전파 경로를 공유하지 않는다.
5. **판정은 노드 로컬 상태로만 계산한다.** eligibility 계산에 크로스노드 시각을 쓰지
   않는다(§15).

## 2. 책임 분리

```text
┌──────────────────────────── 노드 로컬 (cims-agent) ────────────────────────────┐
│                                                                                │
│   Health Checker ──(liveness/readiness/preflight 결과 캐시)──> Recovery         │
│                                                                Supervisor       │
│                                                                   │             │
│                                                   verdict(vrrp_eligible) 생성    │
│                                                   role reconcile → 모듈 기동/정지 │
│                                                                   │             │
│                                                                   ▼             │
│                                                          Process Manager        │
│                                                          (cims-svc start/stop)   │
│                                                                                 │
│   role 파일 관측 ◀──── cims-notify (keepalived 상태 → role 파일 기록)           │
└──────────────────────────────────┬─────────────────────────────────────────────┘
                                    │ verdict.vrrp_eligible (track_script)
                                    ▼
┌────────────────────────────── keepalived ──────────────────────────────────────┐
│  VRRP 광고/단절 감지 · MASTER/BACKUP 선출 · VIP 할당/반납 · notify 로 역할 통보    │
└─────────────────────────────────────────────────────────────────────────────────┘

        계획 절체(스위치오버)만: OAM 이 PREPARE→RELEASE→관측→VERIFY→COMMIT 오케스트레이션
```

| 역할 | 구현 | 책임 | 하지 않는 것 |
|---|---|---|---|
| **keepalived** | `keepalived.conf.tpl`, `cims-ha` | VRRP 선출, VIP 할당/반납, track_script 결과 반영, notify 로 역할 전달 | 노드 간 서비스 준비 조율, 재기동 |
| **cims-notify** | `agent/bin/cims-notify` | keepalived 상태를 role 파일에 원자적 기록 후 즉시 종료 | 모듈 직접 기동/정지, 재기동 판단, readiness 확인, 네트워크 호출 |
| **Health Checker** | agent 내 Health Scheduler | 프로세스·포트·health API·DB·핵심기능 검사 → 정상/비정상 결과를 캐시에 기록 | VIP 이동, 프로세스 재시작 (결과만 산출) |
| **Recovery Supervisor** | agent 내 HA Evaluator | health 집계·복구 정책·재기동 판단·verdict 생성·role reconcile·래치·운영자 의도 관리 | 실제 프로세스 exec (Process Manager 에 위임) |
| **Process Manager** | `cims-svc`(모듈), `cims-ha`(keepalived) | start/stop/restart 실행만 | 판단 |
| **OAM** | `ems/core/oam` | 계획 절체 오케스트레이션(대상 준비→반납→관측→검증→롤백), 관측/표시 | 자동 장애 절체(로컬 agent+keepalived 몫) |

핵심: **keepalived 의 역할 전이는 cims-notify 가 role 파일로 기록하고, Supervisor 는
매 평가 주기마다 role 을 reconcile 해 모듈을 기동/정지한다.** 일회성 이벤트에 의존하지
않고 "현재 역할에서 기대되는 모듈 상태 vs 실제 상태"를 매 주기 비교하므로, 역할 전환
이벤트를 한 번 놓쳐도 다음 주기에 자가 복구된다.

## 3. 적용 범위 — AS vs AA

| | Active/Standby | All-Active |
|---|---|---|
| VIP | 있음 (단일/다중 VIP, 한 vrrp_instance) | **없음** |
| keepalived | 사용 | 미사용 (렌더 시 services 비어 cims-ha uninstall) |
| verdict / eligible / 절체 | **적용** | 미적용 |
| 승격 grace / 계획 절체 | 적용 | 미적용 |
| 장애 시 트래픽 분산 | VIP 이관 (keepalived) | **CSP hash ring 제외** (ha_design.md §7) |
| 로컬 복구(재기동) | 적용 | **적용** — Supervisor 의 프로세스 감시는 mode 무관 공통 |

AA 노드의 유일한 HA 관련 동작은 Supervisor 의 **로컬 복구**(죽은 모듈 재기동)이며,
이것은 "절체"가 아니라 프로세스 감시다. 장애 노드(CMP)의 요청 제외는 CSP 가 자체
`KeepAliveLoop`(endpoint 별 HEARTBEAT/STATS)로 감지해 consistent hash ring 에서 빼는 것으로
처리되며(DEAD 노드는 그 노드의 진행중 호를 BYE 로 정리), 로컬 HA agent 의 책임이 아니다.
상세는 ha_design.md §5.4 / §7.2.

## 4. 상태 모델과 verdict

단일 enum 을 늘리지 않고 **입력 축을 분리한 뒤 최종 verdict 를 합성**한다.

```text
운영자 의도(desired_runtime, ha_intent)
+ 현재 역할(role: MASTER/BACKUP/FAULT/UNKNOWN)
+ 모듈별 관측 상태(liveness/readiness/preflight)
+ 모듈별 정책(failover_relevant, standby_mode hot/cold)
+ 복구 진행 상태(restart_count, recovery deadline)
+ failover latch
+ verdict 신선도
= 최종 verdict
```

파생 표시 상태(화면·로그용): `STARTING`, `STANDBY_READY`, `ACTIVE_HEALTHY`,
`PROMOTING`, `DEGRADED`, `RECOVERING`, `FAILOVER_LATCHED`, `RECOVERY_VALIDATING`,
`CONTROL_STALE`, `MAINTENANCE`, `INTENTIONALLY_DOWN`.

**verdict 입도**: 그룹 1개 = vrrp_instance 1개 = verdict 1개이며, 그 그룹의 데몬
모듈 N개를 §7 규칙으로 집계한 결과다(모듈별 verdict 아님).

verdict 스키마 (`run/ha/verdict/<service>.json`):

```json
{
  "service": "csp",
  "role": "ACTIVE",
  "desired_runtime": "RUNNING",
  "ha_intent": "NORMAL",
  "service_state": "DEGRADED",
  "recovery_state": "RESTARTING",
  "latch_state": "NONE",
  "vrrp_eligible": true,
  "service_available": false,
  "standby_ready": false,
  "sequence": 18201,
  "updated_at": 1784185205,
  "expires_at": 1784185211,
  "boot_id": "7b4c...",
  "reason_codes": ["MODULE_RESTARTING:csp-main"]
}
```

- `vrrp_eligible`: keepalived track_script 가 실제로 사용하는 값.
- `updated_at`/`expires_at`: stale 판정 (§9).
- `sequence`: 단조 증가 — 갱신 정지 확인. `boot_id`(`/proc/sys/kernel/random/boot_id`):
  재부팅 전 verdict 재사용 방지.
- `service_available`(실제 서비스 제공 여부)·`standby_ready`(승격 준비)·`reason_codes`:
  운영/디버깅·화면 표시용.

파일은 임시 파일 기록 → fsync → `rename()` 원자 교체로만 갱신한다.

## 5. 데이터 · 파일 레이아웃

앱 설정과 운영 설정을 분리하고, 영속 상태와 휘발 상태를 분리한다. 모든 HA 상태는
agent 설치 루트(`_PREFIX`) 아래에 둔다 — agent 는 **user systemd 유닛**(cims 계정)이라
`RuntimeDirectory`/`StateDirectory`(→ `/run/user/<uid>`·`~/.local/state`)를 쓰면 root 로
도는 keepalived track_script·cims-notify 와 경로가 어긋난다. root 컴포넌트는 ha.json 의
`cims_home`(=_PREFIX)으로 경로를 유도해 cims 가 쓴 파일을 읽는다(이미 cims-health 가
쓰는 방식). 경로는 `_PREFIX` 상대로 표기한다.

| 파일 | 성격 | 쓰는 쪽 | 읽는 쪽 |
|---|---|---|---|
| `modules/<mod>/<ver>/…/config.json` | 앱 설정 | OAM 패키지 설정 | 모듈 프로세스 |
| `modules/<mod>/service.json` | **모듈 운영 명세** (감시·hot/cold·relevant·health 프로파일·safety) | OAM `update_module_spec` job | Supervisor |
| `run/keepalived/ha.json` | 그룹 정책 노드 복제본 (VRRP 파라미터·cold_modules·restart_limit) | OAM `update_ha` job | Supervisor, cims-ha 렌더 |
| `run/ha/verdict/<svc>.json` | verdict (휘발) | Supervisor (cims) | track_script (root) |
| `run/ha/role/<svc>.json` | 현재 VRRP 역할 (휘발) | cims-notify (root) | Supervisor (cims), track_script (root) |
| `run/ha/health/<check>.json` | health 검사 결과 캐시 (휘발) | Health Scheduler | Supervisor |
| `run/ha/promotion/<svc>.json` | 승격 grace 상태 (휘발) | Supervisor | Supervisor |
| `run/ha/recovery/<mod>` | 재기동 카운터·deadline (휘발) | Supervisor | Supervisor |
| `run/ha/operations/<id>` | 계획 절체 operation·prepare token (휘발) | OAM/Agent | OAM/Agent |
| `run/ha/disabled` | **비상 정지 밸브 마커** (휘발) — `CIMS_HA_DISABLE` | agent | track_script |
| `run/ha/desired.json` | **서버별 정지 의도** — `{module: 'stopped'}` (HOLD_VIP) | Agent `job_process_control` | Supervisor |
| `state/ha/maintenance/<svc>` | **유지보수 마커** (영속) — `EXCLUDE_NODE` | OAM `ha_maintenance` job | Supervisor |
| `state/ha/planned_release/<svc>` | 계획 절체 반납 마커 (영속) | OAM job | Supervisor |
| _(failover 래치)_ | **절체 확정 래치** — 현재 in-memory(`_EVAL_LATCH`), 운영자 start/restart 로 해제. 영속화는 §19 | Supervisor/운영자 | Supervisor |

**영속 vs 휘발**: 유지보수(`maintenance`)·계획절체(`planned_release`) 마커는 재부팅 후에도
남아야 안전(점검/절체 의도가 재부팅으로 사라지면 안 됨) → `state/ha/`. verdict·카운터·역할은
재부팅 시 초기화돼야 한다 → `run/ha/`. 래치는 현재 in-memory 라 재부팅에 사라지지만,
`nopreempt` + verdict `boot_id`(§13) 가 자동 승격을 막아 안전하다(영속화는 §19). `_PREFIX`
는 tmpfs 가 아니라 재부팅에도 파일이 남으므로, 휘발 의미는 (a) **agent 기동 시 `run/ha/`
하위를 초기화**하고 (b) verdict 의 `boot_id` 로 재부팅 전 값 재사용을 차단해 달성한다.

**소유권**: agent(cims)가 `run/ha/`·`state/ha/` 를 생성·소유한다. 교차 사용자 파일은
읽기 전용 방향뿐이라 충돌이 없다 — `role`(root 쓰기 → cims 읽기), `verdict`(cims 쓰기 →
root 읽기). root 는 DAC 를 우회해 cims 소유 파일을 읽고, cims-notify(root)가 cims 소유
`run/ha/role/` 에 쓰는 것도 가능하다. desired/latch/health/promotion/recovery 는 cims
전용. 디렉토리는 설치 스크립트(setup-sudoers 단계, root)가 선생성·chown 하고, agent 도
기동 시 없으면 생성(비-systemd·기존 설치본 대비 self-heal)한다.

### 모듈 운영 명세 (`service.json`)

```yaml
module_specs:
  csp-main:
    failover_relevant: true          # 실패가 절체 사유인가
    standby_mode: hot                # hot: 양쪽 상시 / cold: standby 정지·승격 시 기동
    health_profile: csp-main-health
    restart_policy: default
    safety:
      class: stateless               # stateless | read_only | shared_writer | unknown
      writes_without_vip: false
      requires_leader_lease: false
      latch_clear_mode: auto         # auto | manual
```

`health_module`/`health_config_key`/service.json health 는 이 명세(health_profile)의
입력으로 연결된다.

## 6. Health Checker

검사는 세 종류로 나눈다.

| 종류 | 목적 | 예 |
|---|---|---|
| **liveness** | 재기동 필요 판단 | 프로세스 존재, PID 상태, 간단한 포트 응답 |
| **readiness** | 요청 처리 준비 판단 | TCP listen, HTTP health API, 관리 API, DB 연결, 핵심 기능 |
| **preflight** | 미기동 cold 모듈의 승격 가능성 판단 | 실행 파일·설정 존재, 포트 사용 가능, 의존성 접근, 인증서 존재 |

### 6.1 readiness 검사 포트의 해석 (실효 설정 추종)

readiness 는 "그 모듈이 **실제로 리슨하고 있는 포트**" 를 찔러야 한다. descriptor 의 상수
포트를 그대로 쓰면, 운영자가 리슨 포트를 바꾼 순간 readiness 가 영구 FAIL → 양 노드
FAULT 래치 → 전 노드 정지로 이어진다. 따라서 포트는 아래 우선순위로 해석한다.

| 우선순위 | 출처 | 위치 |
|---|---|---|
| 1 | HA 그룹 수동 오버라이드 `failover_options.health.port` | OAM (`ha_groups.py`) |
| 2 | 모듈 운영 명세 `module_specs.<mod>.health` | OAM (그룹×모듈) |
| 3 | **service descriptor 의 `modules[].health`** | OAM (`service_registry.module_health_specs`) |
| 4 | descriptor `modules[].port` 상수 | 폴백 |

3번의 `health` 블록은 "노드의 실제 설정 파일에서 유도하라"는 선언이며, OAM 은 이를
`ha.json` 의 `health_config_key` / `health_collection` 힌트로 내려보낸다. **해석은 agent 가
검사 시점에 노드 로컬 파일을 직접 읽어 수행**한다 — 배포기록↔실파일 드리프트가 나도 HA 는
실제 bind 포트를 본다(드리프트 자체는 `config_out_of_sync` 알람이 노출).

```jsonc
// 스칼라 config.json 의 단일 키 — 포트가 설정키 하나로 정해지는 모듈 (csc)
{ "name": "csc", "port": 4421, "proto": "tcp",
  "health": { "config_key": "Server.Port" } }

// 컬렉션 jsonl 의 match 레코드 — 리슨 엔드포인트가 컬렉션에 있는 모듈 (csp/psp/isp)
{ "name": "csp", "port": 5060, "proto": "udp",
  "health": { "collection_file": "config/local_nodes.jsonl", "field": "bind_port",
              "match": { "enabled": true, "is_primary": true, "protocol": "UDP" } } }
```

csp 계열의 대표 포트는 `local_nodes` 의 **primary UDP 레코드**다 — CSP 자신이 인스턴스
identity(`Setup.Sip.LocalIp/UdpPort`)를 유도하는 레코드와 동일하므로(`csp/CspServer.cpp`),
"CSP 가 살아 있다" 의 판정 기준으로 일관된다. 서비스 그룹당 대표 포트는 1개이므로 **비-primary
리스너(예: PTT 전용 local_node)는 readiness 대상이 아니다** — 그 리스너만 죽은 상태는
readiness 로 잡히지 않는다. 파일·레코드가 없으면 4번(descriptor 상수)으로 폴백한다.

모든 검사를 2초 HA 루프에서 동기 실행하지 않는다. **검사별 독립 주기로 비동기 실행하고
결과를 캐시**하며, HA Evaluator 는 캐시와 신선도만 읽는다.

| 검사 | 주기 | 타임아웃 |
|---|---|---|
| 프로세스 | 1~2초 | 200ms |
| 로컬 포트 | 2초 | 300~500ms |
| 로컬 health API | 3~5초 | 1초 |
| DB 연결 | 5~10초 | 1~2초 |
| 외부 연계 | 10~30초 | 서비스별 |

실행 방식: I/O 중심(포트/HTTP/프로세스)은 bounded thread pool 또는 asyncio, hang 가능성
있는 것(외부 스크립트·DB CLI·사용자 health_module)은 subprocess 로 격리하고 timeout →
SIGTERM → grace → SIGKILL. 검사별 동시 실행 1개만(이전 실행 중이면 skip, 기존 결과의
`expires_at` 으로 stale 판단).

검사별 stale 중요도:

```yaml
checks:
  - id: process       { criticality: critical, stale_policy: fail }
  - id: metrics-api   { criticality: warning,  stale_policy: degrade }
  - id: database      { criticality: critical, stale_policy: fail_after_grace }
```

health 결과 캐시 예:

```json
{ "check_id": "csc-db-connectivity", "status": "SUCCESS",
  "checked_at": 1784185205, "expires_at": 1784185215, "duration_ms": 312 }
```

## 7. Recovery Supervisor — reconcile + 복구 정책

### 7.1 역할 기반 reconcile (모듈 기동/정지)

매 평가 주기마다 기대 상태와 실제 상태를 비교한다.

```text
expected_running(module, role, cold, desired, excluded, latched):
  excluded (유지보수·EXCLUDE_NODE)  → false   # 승격 제외 노드 — hot·cold 전부 정지
  desired == STOPPED               → false   # 운영자 서버별 정지 (HOLD_VIP)
  latched (절체 확정)               → false   # 탈락 노드 — hot·cold 전부 정지(kill), gap2
  role == MASTER                   → true    # hot·cold 모두 실행
  role == BACKUP/FAULT/UNKNOWN     → (module is hot)   # hot 상시, cold 정지

if expected_running and not running:  Process Manager start
if not expected_running and running:  Process Manager stop  # excluded/latched 는 즉시, 그 외 op_grace 존중
```

**정지(kill)는 keepalived FAULT 상태가 아니라 "절체 확정 래치(latched)"에 묶는다.** 소진
(restart_limit 초과)·좀비로 Evaluator 가 절체를 확정(§13)하면 그 노드의 hot·cold 를 전부
정지한다 — 상대가 이미 서비스를 인수했으므로 재기동 churn·이중 active 를 막는다. **FAULT
여도 래치가 풀린(운영자 start/restart 로 복구) 상태면** role 기준(hot 상시·cold 정지)으로
동작해 standby 로 재합류한다. 이렇게 하지 않고 "FAULT=무조건 정지"로 두면, 정지된 모듈이
readiness 를 못 채워 영원히 FAULT 에 갇히는 데드락이 생긴다.

**cold 모듈은 마스터에서만, reconcile 로만 기동한다("마스터 먼저 → 모듈 나중").** 개별/서버
start 로 cold 모듈을 눌러도 마스터가 아닌 노드에서는 **직접 기동하지 않고**(agent
`_cold_standby_module` 억제), 서비스 무장(arm)만 트리거한다 → 그 노드가 마스터로 승격되면
reconcile(role==MASTER)이 기동한다. 마스터 아닌 노드에서 잠깐 켜졌다 걷어내지는 flap 이
원천 차단된다. 이미 마스터(VIP 보유)면 즉시 직접 기동(crash 복구). keepalived arm 이
실패하면 그 사실이 update_ha job 실패로 노출된다(cold 모듈이 조용히 안 뜨는 게 아니라).

**일괄 시작(`_control_group start`)의 cold 모듈은 지정 마스터에게만 start job 을 보낸다** —
마스터는 deploying→running 표시·desired 해제·실제 기동을 하고, 백업엔 start 대신 홀드 해제
(`ha_clear_holds`: desired=stopped·latch·planned_release 정리)만 보낸다. 백업에 start 를 보내면
ha.json 렌더 전(stagger 창) 억제 판정을 못 해 잠깐 기동돼 **dual-active** 가 되므로 차단한다.
AS 의 hot 모듈·AA 모듈은 양쪽 상시라 직접 start. 마스터 사망 시엔 절체로 백업(신 마스터)
reconcile 이 기동한다. 개별/서버 start 한 대만 눌러도 `note_module_started` 가 그룹 의도를
승격시켜 양 노드가 무장되므로, 그 뒤 자동/수동 절체가 정상 동작한다.

**승격 엣지에서 타겟의 기동 차단 홀드를 해제한다 (정본 경로).** BACKUP/FAULT→MASTER 로 막
승격하면 그 노드가 서비스를 인수하므로, agent 가 `_clear_holds_on_promotion` 으로 그 서비스
모듈의 `desired=stopped`·재기동 카운터·**재기동 backoff**·`_EVAL_LATCH`·`planned_release` 를
해제한다. 승격
시점엔 그 노드가 VIP 를 보유해 ha.json 이 무장돼 있어 **모듈 목록이 확실**하다. 이 경로가
**자동·수동 절체 모두**에서 타겟 기동을 보장한다. INTENTIONALLY_DOWN(이미 MASTER 인 활성
노드에서 운영자가 stop)은 승격 엣지가 아니라 해제되지 않는다. (OAM 이 절체/일괄시작 시
보내는 `ha_clear_holds` job(§12)은 무장 상태에서만 유효한 proactive 보조 — ha.json 미무장
시엔 모듈 목록이 비어 무효라, 정본은 위 승격-엣지 해제다.)

**op_grace 는 기동 직후 바인드 유예용(기본 3s)이다.** 갓 켠 모듈이 포트를 바인드하는 짧은
창 동안만 좀비 오판·reconcile 재기동 경쟁을 막는다. 길게 두면 크래시 재기동·승격 후 기동이
그만큼 지연되므로 짧게(3s) 유지한다. **실제로 아무것도 안 켠 억제(cold on non-master) start 는
op_grace 를 찍지 않는다** — 안 그러면 그 노드가 마스터로 승격됐을 때 reconcile 의 진짜 기동이
남은 op_grace 때문에 지연된다(배치시작 후 cold 모듈이 늦게 뜨던 원인).

**재기동 backoff 는 복구 의도가 있는 지점에서 반드시 해제한다.** reconcile 의 start 실패는
`min(300, 5·2^n)` 초 지수 backoff 로 스로틀된다(모듈당, agent 메모리). 이 스로틀은 reconcile
안에서 "정상 기동 확인" 또는 "돌던 것을 정지"로만 자연 해제되는데 **둘 다 프로세스가 떠 있어야
한다** — 설정 오류 등으로 한 번도 못 뜬 모듈은 어느 쪽에도 안 걸려 상한 300초가 그대로 남는다.
그래서 운영자 start/restart(`job_process_control`)·`ha_clear_holds`·승격 엣지·uninstall 에서
`_clear_reconcile_backoff` 로 함께 지운다(`_fail_reset` 과 짝). 이게 없으면 원인을 고치고 start
를 눌러도 직전 실패 창이 만료될 때까지(최대 5분) 기동되지 않고, 승격 경로에서는 그만큼 절체
인수가 지연된다. **reconcile start 실패는 rc 와 함께 stderr 앞부분을 로그에 남긴다** — backoff
가 벌어지면 재시도가 드물어져 rc 만으론 원인을 추적할 단서가 없다.

**콘솔 모듈 상태 표시는 실측(live_state)이 정본이다.** 배포 status(job 결과=의도)가 아니라
agent 가 주기(≈2s) 보고하는 실제 프로세스 상태로 표시한다(`depEffectiveStatus`): 실제로 떠
있으면 `running`, 아니면 `stopped` — **실제로 안 도는데 running 으로 보이는 일은 없다.** cold
모듈을 reconcile 이 마스터에서 켜면 실측이 곧 `running` 을 반영하고, 백업은 실측 down 이라
`stopped` 로 정확히 보인다(패키지 제어 탭·설치 탭 동일). deploying/pending/failed(진행/실패)는
실측이 up 이 아닐 때 그대로 노출해 "명령 수행 중"을 알린다.

이 방식은 cims-notify 실행 실패·Supervisor 일시 중단·역할 이벤트 유실·기동 중 Agent
재시작에도 다음 주기에 정합을 회복한다. 역할 감지는 role 파일(cims-notify) + 평가 루프
(정합 보장)를 함께 쓰며, 정합의 기준은 평가 루프다.

**stale MASTER 보정**: `_current_role` 은 role 파일이 `MASTER` 여도 이 노드가 서비스 VIP 를
실제로 보유하지 않으면 `BACKUP` 으로 본다. keepalived 정지 시 cims-notify 는 STOP 상태를
role 에 쓰지 않아(§9) role 파일이 MASTER 로 남는데, keepalived 는 정지하며 VIP 를 반납하므로
"VIP 없는 MASTER" 는 실제 마스터가 아니다. 이 보정이 없으면 정지 노드가 cold 모듈을 계속
붙들어 상대 노드와 dual-active 가 된다. (승격 시 keepalived 는 VIP 를 붙인 뒤 MASTER notify
하므로 정상 승격을 stale 로 오판하지 않는다.)

### 7.2 재기동 정책 상태 머신 (`restart_limit`)

**ACTIVE relevant 모듈:** 재기동 카운터는 **크래시(직전 tick 살아있다가 죽음) 횟수를
window(`restart_limit.window_sec`, 기본 300s) 안에서 누적**한다. 최초 기동·승격 기동은
크래시가 아니므로 세지 않는다(`_LAST_UP` 로 판별). 재기동 성공만으로 카운터를 리셋하지
않는다 — 그래야 kill→복구→kill 반복이 누적돼 한도에 도달한다.

```text
HEALTHY → (모듈 크래시) → reconcile 재기동 + fail_count++ (window 내 누적)
  window 내 fail_count <  restart_limit → 로컬 복구 계속 (RECOVERING)
  window 내 fail_count >= restart_limit → FAILOVER_LATCHED → vrrp_eligible=false
       ↑ 현재 떠 있어도(=flapping) 절체. 죽어 있어도(=소진) 절체. 둘 다 같은 판정.
  window 무크래시 경과 → fail_count 자연 만료(stale 무시) / 운영자 start·restart → 리셋+래치 해제
```

**핵심**: "300s 내 크래시 N회면 절체"가 실제로 성립하려면 (a) 카운터를 매 정상 tick 마다
리셋하지 않고, (b) Evaluator 가 **현재 readiness 와 무관하게** window 내 카운트만으로
판정해야 한다(모듈이 죽었다 살았다를 반복하면 스냅샷상 살아 보여도 절체).좀비(프로세스
생존+readiness 실패)는 이와 별개로 op_grace 이후 즉시 절체.

**BACKUP relevant hot 모듈:** (트래픽 미처리라 복구 중엔 승격 자격 없음)

```text
readiness 실패 → STANDBY_RECOVERING → vrrp_eligible=false
  재기동 성공 → STANDBY_READY → vrrp_eligible=true
  한도 초과 → STANDBY_INELIGIBLE (안전등급에 따라 래치)
```

**BACKUP relevant cold 모듈:** 실행 안 하므로 restart 카운트 미사용. `preflight` 실패 →
`STANDBY_INELIGIBLE`, preflight 연속 성공 → `STANDBY_READY`(단 래치 존재 시 preflight
성공만으로 해제하지 않음).

**Non-relevant 모듈:** 한도 초과해도 `service_state=DEGRADED` 까지만, `vrrp_eligible`
불변.

## 8. 자격(vrrp_eligible) 계산식

역할별로 다르며, 전부 노드 로컬 값으로 계산한다.

**UNKNOWN / 부트스트랩:**
```text
initial_evaluation_completed AND verdict_fresh AND !latch
  AND ha_intent ∉ {EXCLUDE_NODE, MAINTENANCE}
  AND all_relevant_hot_ready AND all_relevant_cold_preflight_ready
```

**BACKUP:**
```text
verdict_fresh AND !latch
  AND ha_intent ∉ {EXCLUDE_NODE, MAINTENANCE, HOLD_VIP}
  AND all_relevant_hot_ready AND all_relevant_cold_preflight_ready
```

**MASTER promotion grace 중** (cold runtime readiness 제외):
```text
verdict_fresh AND !latch AND !operator_excluded
  AND all_relevant_hot_ready AND all_relevant_cold_preflight_ready
```

**MASTER 정상/복구 중:**
```text
verdict_fresh AND !latch AND !operator_excluded
  AND 모든 relevant 모듈이 { readiness 정상 | promotion grace 대상
                          | restart_limit 이내 RECOVERING } 중 하나
```

`vrrp_eligible=false` 인 상태: `FAILOVER_LATCHED`, `CONTROL_STALE`(grace 초과),
`EXCLUDE_NODE`/`MAINTENANCE`, ACTIVE relevant 모듈 restart_limit 초과,
`total_recovery_timeout` 초과.

## 9. cims-notify(role writer) · track_script(verdict reader)

### cims-notify

keepalived 상태 인자를 받아 role 파일만 원자적으로 기록하고 종료한다. 모듈 기동/정지,
재기동 판단, readiness, OAM 호출, peer 확인, 절체 판단을 하지 않는다.

```json
{ "service": "csp", "role": "MASTER", "sequence": 1042,
  "updated_at": 1784185205, "boot_id": "7b4c..." }
```

`sequence` 는 cims-notify 가 단조 증가시키고, Supervisor 는 role 값 변화로도 전이를
감지한다(이중 안전). 기록 실패는 syslog 로만 남긴다.

### track_script

verdict 를 읽어 rc 0/1 만 반환한다. raw health 폴백(프로세스/포트/HTTP/DB 검사)을 하지
않는다.

```text
if boot_id mismatch:                         fail
if role == BACKUP and now > expires_at:       fail        # BACKUP stale 즉시 실패
if role == MASTER and now > expires_at + master_grace: fail
return verdict.vrrp_eligible
```

현재 역할은 cims-notify 가 쓴 role 파일에서 읽고, 읽을 수 없으면 BACKUP 과 동일한 무유예
fail-safe 를 적용한다. **단일 모델**이라 track_script 에 legacy 포트검사 폴백은 없다 —
verdict 가 유일한 판정 입력이다.

### 비상 정지 밸브 (`CIMS_HA_DISABLE`)

verdict 의 유일 생산자(Supervisor)가 오작동해 절체가 폭주하거나 양 노드가 동시에 자격을
잃는 것을 막는 운영용 kill-switch. legacy 로 되돌아가는 게 아니라 **판정을 얼린다**:
- env `CIMS_HA_DISABLE=1` + agent 재기동 → agent 가 Supervisor 스레드를 띄우지 않고
  `run/ha/disabled` 마커를 기록.
- **track_script**: 마커 있으면 verdict 를 보지 않고 **무조건 rc0(PASS)** → keepalived 가
  health 로는 절체하지 않음(현 VIP 위치 고정). 노드 사망(VRRP advert 소실)만 절체.
- **cims-notify**: 마커 있으면 role 만 기록(모듈 자동 제어 없음 — 운영자 수동).
결과: HA 판정을 얼리고 서비스 현상 유지 → 운영자가 원인 수습 후 env 제거·재기동으로 복귀.

**stale 정책 (역할 비대칭):** BACKUP 은 stale 즉시 승격 자격 제거(제어 모듈이 죽은
노드가 승격되면 위험). MASTER 는 짧은 grace(Supervisor 일시 재시작이 곧 절체가 되지
않도록) 후 실패.

```yaml
verdict:      { update_interval: 2s, ttl: 6s }
stale_policy: { backup_grace: 0s, master_grace: 6s, unknown_role_grace: 0s }
```

Agent 는 systemd 로 빠르게 복구시킨다:

```ini
[Service]
Type=notify
Restart=always
RestartSec=1
WatchdogSec=15
```

`WatchdogSec` 는 별도 Watchdog Coordinator 가 핵심 스레드(HA Evaluator·Health
Scheduler·role observer·verdict writer) heartbeat 를 종합해서만 `sd_notify` 를 보낸다.
OAM 연결/heartbeat 응답/job 완료/외부 응답은 watchdog 조건에서 **제외**한다(OAM 이 끊겨도
로컬 HA 는 살아 있어야 하므로). 장시간 job 은 subprocess/별도 worker 로 분리해 Evaluator·
watchdog 이 영향받지 않게 한다.

## 10. 승격 grace

BACKUP→MASTER 승격 직후 cold 모듈은 기동에 수 초가 걸려 readiness 가 실패한다. 이를
즉시 `vrrp_eligible=false` 로 반영하면 방금 얻은 VIP 를 반납하는 flap 이 발생한다.

**규칙**: role=MASTER 전환을 감지한 **첫 평가에서, cold 모듈 start 를 실행하기 전에**
`PROMOTING` 진입 + `grace_until` 설정. grace 동안 cold relevant 모듈의 down/readiness
실패/기동 진행은 eligible 에서 제외한다.

grace 중에도 즉시 실패로 처리하는 것: 래치 존재, `EXCLUDE_NODE`/`MAINTENANCE`, verdict
stale, 필수 hot 모듈 장애, cold 모듈 **preflight** 자체 불가, 시스템 오류(실행 파일 없음·
권한·잘못된 unit — 단 첫 실패로 즉시 반납하지 않고 Recovery Policy 적용).

상태(`run/ha/promotion/<svc>.json`):

```json
{ "service": "csp", "state": "PROMOTING", "role_sequence": 1042,
  "started_at": 1784185205, "grace_until": 1784185225, "boot_id": "7b4c..." }
```

grace 종료: 전 cold relevant 모듈 readiness 성공 → `ACTIVE_HEALTHY`. 실패 잔존 →
`RECOVERING`(이때부터 readiness/재기동 실패를 `restart_limit` 에 반영).

grace 값은 cold 모듈 기동 시간보다 충분히 길게 서비스별 설정:

```yaml
services:
  csp: { promotion_grace_sec: 15 }
  csc: { promotion_grace_sec: 30, restart_limit: 3, restart_interval_sec: 5,
         readiness_timeout_sec: 15, total_recovery_timeout_sec: 90 }
```

## 11. 자동 장애 절체 흐름

OAM 에 의존하지 않는다(로컬 agent + keepalived).

```text
0s   relevant 모듈 health 실패
~4s  연속 실패 확정 → DEGRADED → RECOVERING (vrrp_eligible=true 유지)
     Process Manager restart, readiness 대기
...  재기동 성공 → ACTIVE_HEALTHY (절체 없음, 로컬 복구 완료)
     재기동 한도/deadline 초과
       → FAILOVER_LATCHED → vrrp_eligible=false
       → track_script 실패 → keepalived FAULT → VIP 반납
       → 상대 노드 role=MASTER → Supervisor 가 role 감지 → cold 모듈 기동(§10 grace)
       → readiness 성공 → ACTIVE_HEALTHY
```

노드 자체 사망은 이 경로와 별개로 keepalived VRRP advert 소실(대략 광고주기×3)로 감지된다.
좀비(프로세스 생존·포트 무응답)는 liveness 통과·readiness 실패로 잡혀 재기동 → 소진 시 절체.

`verdict=false` 는 즉시 VIP 를 반납시키지 않는다: track_script 다음 폴링(interval×fall)에서
실패가 누적돼 FAULT 로 전이한다. 따라서 계획 절체는 고정 sleep 이 아니라 실제 role/VIP
관측으로 진행해야 한다(§12).

## 12. 계획 절체 (스위치오버) — OAM 오케스트레이션

노드 간 직접 통신 채널을 추가하지 않고 OAM 이 오케스트레이션한다. keepalived 프로세스를
직접 stop/start 하지 않고 **Agent 의 `vrrp_eligible` 을 바꿔** VIP 를 반납시킨다. OAM 은
operation 상태를 영속 저장해 재시작 후 이어서 처리한다.

```text
PREPARE_TARGET → TARGET_PREPARED → REQUEST_SOURCE_RELEASE → WAIT_SOURCE_RELEASE
  → WAIT_TARGET_MASTER → START_TARGET_COLD_MODULES → VERIFY_TARGET → COMMIT
```

- **PREPARE**: 대상 B 가 준비 확인(HA Agent 정상, 래치 없음, 설정·실행파일·의존성·
  인증서, hot health, cold **preflight**) → `PREPARED` + prepare token(만료 있음).
  cold 모듈은 VIP 취득 전 완전 readiness 확인 불가라 `PREPARED`(preflight)와
  `SERVING_READY`(실기동·readiness)를 구분한다.
- **RELEASE**: token 확인 후 A 가 planned_release·`vrrp_eligible=false` → track_script
  실패 → VIP 반납 (keepalived 는 계속 실행).
- **관측**: OAM 이 A role≠MASTER·A VIP 제거·B role=MASTER·B VIP 존재를 실제 관측(고정
  sleep 금지).
- **VERIFY**: B cold 모듈 기동(§10 grace)·readiness → `SERVING_READY` → `COMMIT`.

### 실패·롤백

| 실패 지점 | 처리 |
|---|---|
| PREPARE 실패 | A 유지, 서비스 영향 없음 (`FAILED_PREPARE`) |
| PREPARED 후 RELEASE 전 실패 | B prepare token 폐기, A 유지 (`ABORTED`) |
| RELEASE 요청 후 A 가 아직 VIP 보유 중 실패 | A verdict 복원, A role=MASTER 유지 확인 |
| A 반납했으나 B 가 MASTER 못 됨 | 자동 롤백: A verdict 복원 → A VIP 복귀 → cold 기동 → readiness (`ROLLED_BACK`, 순단 발생) |
| B MASTER 됐으나 SERVING_READY 실패 | B 로컬 짧은 복구 시도 → 실패 시 조율된 역절체(추가 순단) |

A 는 COMMIT 전까지 `ROLLBACK_READY` 유지(설정·실행파일 유지, 래치 영구설정 금지, 재기동
카운터 초기화 금지). OAM 이 절체 중 죽으면: B 가 VIP 미취득이면 A release 에 TTL 을 둬
자동 복원, **B 가 이미 VIP 취득했으면 A 는 자동 롤백하지 않고**(`nopreempt` 로 B 유지)
OAM 복구/운영자 개입 대기.

**계획 절체 타임아웃 관계**: OAM VERIFY 타임아웃 ≥ VRRP 반납·승격 + `promotion_grace` +
cold `readiness_timeout` + 허용 restart 시간. grace 중 readiness 실패를 즉시 절체 실패로
보지 않는다.

**planned_release 생명주기 (중요)**: source 의 `planned_release` 마커는 **모든 종결
전이에서 반드시 해제**한다 — 롤백/실패뿐 아니라 **COMMIT 성공 시에도** 해제해야 한다.
COMMIT 후엔 `nopreempt` 로 target 이 계속 MASTER 를 유지하지만, source 의 자격
(`vrrp_eligible`)은 정상 복원돼야 **역방향 계획 절체(target→source)가 가능**하다. COMMIT
에서 해제하지 않으면 source 가 영구 부적격으로 갇혀 다시는 절체 대상이 되지 못한다. OAM
상태머신은 각 종결 전이(COMMITTED/ROLLED_BACK/FAILED) 시점에 해제 job 을 인라인 전송한다
(종결 후 "다음 호출이 처리"에 의존하지 않는다 — 종결 op 은 sweep 이 skip 하므로). 이중
안전망: (1) 운영자가 해당 모듈을 start/restart 하면 agent 가 planned_release 를 함께
해제하고(정상 운영 의도와 모순), (2) 마커가 절체 최대시간을 크게 초과(기본 180s)해 남아
있으면 agent 가 stale 로 보고 자가 제거한다(해제 job 유실·OAM 중단 대비).

**타겟 홀드 선해제**: 절체를 시작할 때 OAM 은 **타겟**에 `ha_clear_holds`(서비스) job 을
먼저 보내 타겟의 `desired=stopped`·재기동 카운터·latch·planned_release 를 지운다. 타겟에
이전 stop 이 고착돼 있으면 승격돼도 reconcile 이 모듈을 못 켜기 때문이다(그 노드가 서비스를
인수해야 하므로 "이 노드에서 정지" 오버라이드는 절체 의도와 모순). 일괄 시작도 백업 멤버에
같은 job 을 보내 향후 절체에 대비한다(§7.1).

## 13. failover 래치 · 부트스트랩

### 래치

`nopreempt` 는 "복구된 옛 MASTER 가 VIP 를 도로 안 가져감"까지이고, **노드를 승격 불가로
만드는 것은 별도 상태**(`FAILOVER_LATCHED`)다. 현재 구현의 래치 설정 조건(ACTIVE 판정):
relevant 모듈이 (a) 재기동 한도 초과(exhausted) 또는 (b) 좀비(프로세스 생존 + readiness
실패 + op_grace 경과). 래치는 Supervisor 프로세스 내 in-memory(`_EVAL_LATCH`)다.

래치 중: `vrrp_eligible=false`(자동 Active 전환 금지) + **reconcile 이 그 노드의 hot·cold
모듈을 전부 정지(kill)한다**(§7.1) — 절체당한 노드는 재기동 경쟁 없이 완전히 내려간다.
track_script 실패 → keepalived FAULT → VIP 는 peer 로.

해제(재합류): 운영자가 콘솔에서 해당 모듈을 **start/restart** 하면 래치가 풀린다
(`_clear_failover_latch`) — 원인을 고친 뒤 올리라는 명시적 re-arm. 해제되면 role 기반
reconcile 이 hot 을 기동 → readiness 회복 → track_script PASS → keepalived FAULT→BACKUP
로 standby 재합류한다(`nopreempt` 라 곧바로 MASTER 는 아님). agent 재기동/노드 재부팅도
in-memory 래치를 비우지만, boot_id 무효화 + `nopreempt` 로 자동 승격이 아니라 standby
재합류에 그친다.

> 안전등급별 자동 해제(stateless/read_only 는 연속 성공 창 기반 자동, shared_writer/
> unknown 은 수동)와 래치 영속화(`state/ha/latch/`)는 §19 후속 과제다. 현재는 in-memory
> + 운영자 start/restart 해제로 단일화돼 있다.

### 부트스트랩

verdict 없음/파싱 실패/boot_id 불일치/만료 → track_script 실패(fail-safe: verdict 없음 =
`eligible=false`). Agent 기동 순서:

```text
1. cims-agent 시작 → run/ha·state/ha 준비
2. 보수적 verdict=false(STARTING) 즉시 기록
3. desired(서버별 정지)·maintenance·module_specs 로드 (래치는 in-memory 라 빈 상태로 시작)
4. hot readiness / cold preflight 평가
5. 초기 승격 후보 자격 계산 → 정상이면 verdict=true 갱신
6. keepalived 가 track_script 반영해 선출
```

첫 verdict 생성을 OAM 연결/전체 health 완료까지 미루지 않는다. role 파일이 아직 없으면
`role=UNKNOWN` 이며 MASTER 로 가정하지 않는다(무유예 fail-safe). 양쪽 동시 부팅 시 초기
검사 완료까지 잠시 VIP 공백이 있을 수 있으나, 미검증 노드가 MASTER 가 되는 것보다 안전.

## 14. 잔여 위험 — split-brain / fencing

본 설계는 각 노드 로컬 상태로 `vrrp_eligible` 을 계산하므로, **노드 간 VRRP 통신 단절 시
dual-MASTER 를 방지하지 못한다**. 양쪽이 서로의 advert 를 못 받으면 각자 MASTER 로 판단해
dual-VIP 가 될 수 있다(Supervisor/verdict 는 quorum·fencing 을 제공하지 않는다).

- **감내 가능**: VIP 요청 처리만 하고 상태 없음/읽기 중심이며 동시 실행에 데이터 손상이
  없는 서비스. (dual-VIP 에서 ARP/네트워크 경로에 따라 클라이언트가 분산될 수 있음.)
- **별도 보호 필요(shared_writer)**: DB write·MQ consume·스케줄러·공유 파일 write·외부
  명령·VIP 무관 background 작업. 이들은 STONITH/노드 fencing, DB/etcd/Consul leader
  lease, fencing token, 앱 단일 writer 보장 중 하나가 필요하다. 수동 래치는 "절체된
  노드의 재승격"만 막을 뿐 "기존 프로세스가 계속 쓰는 것"은 못 막는다.

모듈 명세 `safety.class`: `stateless` | `read_only` | `shared_writer` | `unknown`.
`unknown` 은 확인 전까지 `shared_writer` 로 보수 처리(`latch_clear_mode: manual`). 확인
항목: BACKUP 에서도 DB write 하는가, VIP 없이 background 작업 하는가, 두 인스턴스 동시
실행 시 중복 처리 되는가, DB 에 leader/owner 개념이 있는가.

이보다 강한 클러스터 정합성이 필요하면 OAM 조율 확장보다 Pacemaker·Corosync + fencing
또는 제3자 witness 도입이 맞다.

## 15. 시계 정책 — 노드 로컬

verdict 신선도·grace·deadline 판정은 **전부 같은 노드에서 생성·소비**된다(role·health
cache·verdict·expires_at·track_script·promotion grace·restart deadline). 따라서 "현재
로컬 시간 vs 로컬 expires_at" 비교만 하며 A 노드 timestamp 를 B 가 직접 비교하지 않고, OAM
전달 시각도 eligibility 계산에 쓰지 않는다. 이로써 노드 간 clock skew·NTP 보정 중 시간차·
과거 크로스노드 timestamp 비교 문제가 재발하지 않는다. 프로세스 내부 timeout/grace 는
가능하면 monotonic(`CLOCK_MONOTONIC`/`BOOTTIME`)을 쓰고, 파일에는 운영 확인용 wall clock
+ 동일 부팅 비교용 boot_id 를 병행한다. 노드 간 타이밍 조율은 자동 절체=VRRP/keepalived,
계획 절체=OAM operation 상태 머신에만 맡긴다.

## 16. 운영자 의도

의도는 상태가 아니라 별도 입력 두 축으로 저장한다. 서버별 정지(desired_runtime)는
모듈 단위라 `run/ha/desired.json`, 노드 유지보수(ha_intent=EXCLUDE_NODE)는 노드×서비스
단위라 `state/ha/maintenance/<svc>` 마커로 각각 둔다(둘은 관심사가 달라 분리).

```text
run/ha/desired.json          { "csp": "stopped" }        # 서버별 정지 = HOLD_VIP
state/ha/maintenance/<svc>   존재 = EXCLUDE_NODE(유지보수, 영속)
state/ha/planned_release/<svc>  존재 = PLANNED_FAILOVER 반납(계획 절체 §12)
```

| desired_runtime | ha_intent | 실현 | 의미 |
|---|---|---|---|
| RUNNING | NORMAL | 마커 없음 | 일반 운영 |
| STOPPED | HOLD_VIP | `desired.json[mod]=stopped` | 서비스만 중지, 자동 절체 금지 (ACTIVE 에서만 의미) |
| — | EXCLUDE_NODE | `maintenance/<svc>` | 유지보수 — 승격 대상에서 제외 + 모듈 정지 |
| RUNNING | PLANNED_FAILOVER | `planned_release/<svc>` | 대상 준비 확인 후 계획 절체 |

역할별 처리:
- `ACTIVE + HOLD_VIP` → 프로세스 중지·재기동 안 함·`vrrp_eligible=true`·VIP 유지(서비스
  의도적 중단, 파생 표시 `INTENTIONALLY_DOWN`).
- `EXCLUDE_NODE`(유지보수) → 역할 무관 `vrrp_eligible=false`. **이 노드는 승격 대상에서
  제외** — 상대가 죽어도 이 노드로 절체되지 않는다(점검 중 노드로 서비스가 올라오는 것
  방지, 다운 감수). 모듈도 정지.
- `BACKUP + HOLD_VIP` → `EXCLUDE_NODE` 로 정규화(정지된 노드 승격 방지).

즉 의도는 파생 상태 하나로 고정하지 않고 (desired_runtime, ha_intent, 현재 role)을 함께
넣어 계산한다.

콘솔 매핑: 서버별 stop = `STOPPED/HOLD_VIP`(active 절체 안 함), 일괄 중지 = 전 멤버 STOPPED
+ 비무장, 서버별/일괄 start = `RUNNING/NORMAL`, **유지보수(노드) = `EXCLUDE_NODE`**.

## 17. 콘솔 배치

- **절체 조건(그룹 편집, AS)**: advert_int, health 타이밍(interval/fall/rise/timeout),
  승격 grace, 재기동 임계(restart_limit), track_interface, preempt. 모듈별 값은 여기 없음.
- **패키지 설정(그룹 → 모듈별)**: 모듈 운영 명세(감시·hot/cold·relevant·health 프로파일·
  safety 등급) 편집 → `update_module_spec` push. 앱 설정(config)과 별 섹션.
- **패키지 제어(그룹)**: 멤버×모듈 매트릭스 + 일괄 시작/재시작/중지 + 수동 절체(계획
  절체 오케스트레이션 진입) + **노드 유지보수 토글(EXCLUDE_NODE)**. 개별 서버 버튼은 "이 노드만".
- **상태 표시**: 미개시=비무장, `INTENTIONALLY_DOWN`, `FAILOVER_LATCHED`, `MAINTENANCE`,
  `PROMOTING` 등 파생 상태 노출. active_agent 관측은 표시·계획절체용(자동 절체 판정 근거 아님).
- HA 모드 legacy/supervisor 토글은 **없다** — 단일 모델이므로.

## 18. 단일 모델 · 비상 밸브

**legacy 경로는 존재하지 않는다.** cims-health 는 항상 verdict reader, cims-notify 는 항상
role writer, agent 는 HA 그룹이 있으면 항상 Supervisor(Health Checker + Evaluator +
reconcile)를 구동한다. 모드 플래그(verdict_source/ha_mode/notify_mode)·shadow·서비스별
컷오버·`flags.json`·구 cims-health 포트검사·구 cims-notify 직접 start/stop 은 전부
제거한다. legacy watchdog(`supervise_tick`)은 **HA 그룹에 속하지 않은 standalone 모듈
전용**으로만 남긴다(HA 관리 모듈은 Supervisor reconcile 이 소유).

**개시 국면 선착(stagger)은 유지한다.** cold 모듈은 개시 시 양 노드의 승격 자격(preflight)이
대칭이라, 두 노드를 동시에 arm 하면 우선순위 높은 노드(놀던 standby 여도)가 선착해 "운영자가
start 누른 노드가 Active" 기대가 깨진다. 따라서 개시(아무도 VIP 미보유) 시 기준 멤버
(prefer_first = 개별/서버별 start 누른 노드 > record running > 지정 마스터)에게 update_ha 를
먼저 내리고 나머지는 `_STAGGER_DELAY_SEC` 지연시켜, 기준 멤버가 arm→VIP 선점→nopreempt
유지로 Active 가 되게 한다. VIP 보유자가 이미 있으면 지연 없음(apply 멱등).

**유일한 escape 는 비상 밸브 `CIMS_HA_DISABLE`**(§9) 하나뿐이다 — legacy 로 되돌아가는
이중 판정 엔진이 아니라, 판정을 얼려 현상 유지하는 운영용 kill-switch.

부트스트랩: verdict 가 아직 없으면 track_script 는 rc1(not eligible) — Supervisor 가 첫
verdict 를 쓸 때까지 그 노드는 승격 대상이 아니다(fail-safe, §13). 즉 "verdict 없음 =
자격 없음"이 유일한 규칙이고, legacy 포트검사로의 폴백은 없다.

## 19. 미구현 · 후속 과제

- **failover 래치 영속화 + 안전등급별 자동 해제** — 현재 래치는 in-memory 이고 해제는
  운영자 start/restart 단일 경로다(§13). 향후 `state/ha/latch/<svc>.json` 영속 + 안전등급
  기반 자동 해제(stateless/read_only 는 연속 성공 창 후 `STANDBY_READY` 까지만 자동 복귀,
  shared_writer/unknown 은 수동)로 확장.
- shared_writer(특히 csc DB) fencing/leader-lease — 미도입. 확인 전 `safety.class=unknown`
  = 수동 래치 보수 처리.
- CSP hash ring 의 endpoint 헬스체크는 CSP `KeepAliveLoop`(3초, endpoint 별 HEARTBEAT
  연속 3회 실패 ≈9초 → `MarkUnhealthy`; STATS 포화 → 신규 제외; DEAD 노드 호는 BYE 정리)로
  동작 — 본 HA 재설계(keepalived/verdict)와 별개 축의 CSP 미디어평면 로직. 상세 ha_design.md §5.4.
- 양 노드가 동시에 자격을 잃으면(같은 장애·양쪽 FAULT) VIP 공백 = 서비스 중단. "잘못된
  노드가 Active" 보다 안전한 fail-safe 이며, 반복 시 `CIMS-QOS-001`(ha_flap) 알람으로 노출.
  근본 회피는 비상 밸브(`CIMS_HA_DISABLE`)로 판정을 얼려 수동 수습.
