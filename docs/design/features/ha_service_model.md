# HA 서비스 운영 모델 — 책임 분리 · 선언적 verdict · 절체 판정

> 이 문서는 CIMS HA 의 **무장 판정·장애 감지·로컬 복구·절체 결정·계획 절체**의 정본
> 설계다. keepalived 인프라(파일 구조·렌더·apply)의 운영 상세는
> [../ha_design.md](../ha_design.md) §11 을, 토폴로지 개요는 ha_design.md §1~8 을 본다.
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
이것은 "절체"가 아니라 프로세스 감시다. 장애 노드의 요청 제외는 CSP 가 자체
`KeepAliveLoop`(Alive ping)로 감지해 consistent hash ring 에서 빼는 것으로 처리되며,
로컬 HA agent 의 책임이 아니다.

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
| `state/ha/desired/<svc>.json` | **운영자 의도** (영속) | OAM/Agent | Supervisor |
| `state/ha/latch/<svc>.json` | **failover 래치·유지보수** (영속) | Supervisor/운영자 | Supervisor |

**영속 vs 휘발**: 래치·운영자 의도는 재부팅 후에도 남아야 안전(절체당한 노드가 재부팅으로
자동 승격 자격을 되찾으면 안 됨) → `state/ha/`. verdict·카운터·역할은 재부팅 시 초기화돼야
한다 → `run/ha/`. `_PREFIX` 는 tmpfs 가 아니라 재부팅에도 파일이 남으므로, 휘발 의미는
(a) **agent 기동 시 `run/ha/` 하위를 초기화**하고 (b) verdict 의 `boot_id`(§13) 로 재부팅
전 값 재사용을 차단해 달성한다.

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
    run_on_fault: true               # FAULT 강등 시 hot 모듈 유지 여부
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
expected_running(module, role, desired_runtime, standby_mode, latch):
  role == MASTER            → true
  role == BACKUP, hot       → true
  role == BACKUP, cold      → false
  role == FAULT, hot        → run_on_fault 정책
  role == FAULT, cold       → false
  role == UNKNOWN, cold     → false (안전)
  desired_runtime == STOPPED → false (운영자 정지 — 예외)

if expected_running and not running:  Process Manager start
if not expected_running and running:  Process Manager stop
```

이 방식은 cims-notify 실행 실패·Supervisor 일시 중단·역할 이벤트 유실·기동 중 Agent
재시작에도 다음 주기에 정합을 회복한다. 역할 감지는 inotify(빠른 반응) + 평가 루프
(정합 보장)를 함께 쓸 수 있으며, 정합의 기준은 평가 루프다.

### 7.2 재기동 정책 상태 머신 (`restart_limit`)

**ACTIVE relevant 모듈:**

```text
HEALTHY → (health 실패 확정) → DEGRADED → RECOVERING
  restart_count < restart_limit AND deadline 미초과 → restart
  readiness 연속 성공 → restart_count=0 → ACTIVE_HEALTHY
  restart_count >= restart_limit OR total_recovery_timeout 초과
      → FAILOVER_LATCHED → vrrp_eligible=false
```

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
fail-safe 를 적용한다.

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

## 13. failover 래치 · 부트스트랩

### 래치

`nopreempt` 는 "복구된 옛 MASTER 가 VIP 를 도로 안 가져감"까지이고, **노드를 승격 불가로
만드는 것은 별도 상태**(`FAILOVER_LATCHED`)다. 래치 설정 조건: 재기동 한도 초과, 프로세스
종료 실패, health 지속 실패, Supervisor 내부 오류, 절체 후 옛 Active 뒤늦은 정상화,
split-brain 가능성 감지. 래치는 `state/ha/latch/` 에 영속.

래치 중: `vrrp_eligible=false`, 자동 Active 전환 금지, track_script 실패, OAM 에
`FAILOVER_LATCHED` 표시.

해제 정책 (안전등급별, §14):
- **엄격(수동)**: `ha-control clear-latch <svc>` — shared_writer·unknown.
- **자동**: 아래 모두 충족 시 `STANDBY_READY` 까지만 복귀 — stateless·read_only.
  ```yaml
  latch_clear: { mode: automatic, consecutive_successes: 30,
                 stabilization_time: 120s, require_peer_vip_observation: true }
  ```
  전이: `FAILOVER_LATCHED → RECOVERY_VALIDATING → STANDBY_READY`. 곧바로 ACTIVE 자격을
  주지 않고 STANDBY 로만 복귀 → 현 MASTER 사망 시에만 승격.

### 부트스트랩

verdict 없음/파싱 실패/boot_id 불일치/만료 → track_script 실패(fail-safe: verdict 없음 =
`eligible=false`). Agent 기동 순서:

```text
1. cims-agent 시작 → run/ha·state/ha 준비
2. 보수적 verdict=false(STARTING) 즉시 기록
3. desired·latch·module_specs 로드
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

의도는 상태가 아니라 별도 입력 두 축으로 저장한다(`state/ha/desired/<svc>.json`):

```json
{ "desired_runtime": "STOPPED", "ha_intent": "HOLD_VIP",
  "reason": "operator_stop", "operation_id": "op-...", "updated_at": 1784185205 }
```

| `desired_runtime` | `ha_intent` | 의미 |
|---|---|---|
| RUNNING | NORMAL | 일반 운영 |
| STOPPED | HOLD_VIP | 서비스만 중지, 자동 절체 금지 (ACTIVE 에서만 의미) |
| STOPPED | EXCLUDE_NODE | 유지보수 — 승격 대상에서도 제외 |
| RUNNING | PLANNED_FAILOVER | 대상 준비 확인 후 계획 절체 |
| STOPPED | NORMAL | 모호 — API 에서 거부 |

역할별 처리: `ACTIVE + HOLD_VIP` → 프로세스 중지·재기동 안 함·`vrrp_eligible=true`·VIP
유지(서비스 의도적 중단, 파생 표시 `INTENTIONALLY_DOWN`). `BACKUP + HOLD_VIP` →
`vrrp_eligible=false`(정지된 노드 승격 방지) 또는 `EXCLUDE_NODE` 로 정규화. 즉
INTENTIONALLY_DOWN 을 eligible=true 로 고정하지 않고 현재 역할까지 포함해 계산한다.

콘솔 매핑: 서버별 stop = `desired_runtime=STOPPED, ha_intent=HOLD_VIP`(active 절체 안 함),
일괄 중지 = 전 멤버 STOPPED + 비무장, 서버별/일괄 start = RUNNING, 유지보수 = EXCLUDE_NODE.

## 17. 콘솔 배치

- **절체 조건(그룹 편집, AS)**: advert_int, health 타이밍(interval/fall/rise/timeout),
  승격 grace, 재기동 임계(restart_limit), track_interface, preempt. 모듈별 값은 여기 없음.
- **패키지 설정(그룹 → 모듈별)**: 모듈 운영 명세(감시·hot/cold·relevant·health 프로파일·
  safety) 편집 → `update_module_spec` push. 앱 설정(config)과 별 섹션.
- **패키지 제어(그룹)**: 멤버×모듈 매트릭스 + 일괄 시작/재시작/중지 + 수동 절체(계획
  절체 오케스트레이션 진입). 개별 서버 버튼은 "이 노드만".
- **상태 표시**: 미개시=비무장, `INTENTIONALLY_DOWN`, `FAILOVER_LATCHED`,
  `MAINTENANCE`, PROMOTING 등 파생 상태 노출. active_agent 관측은 표시·계획절체용
  (자동 절체 판정 근거 아님).

## 18. 구현 현황 · 이행

빅뱅 재작성 대신 feature flag + shadow + 서비스별 전환으로 이행한다. 각 축은 기존
동작을 유지한 채 신규 경로를 병행하고, 검증 후 서비스 단위로 전환한다.

```yaml
ha:
  verdict_source: legacy | supervisor    # 서비스별 지정 가능
  notify_mode:    legacy | role_writer
  stagger_enabled: true                  # verdict-driven 전환 시 false
  planned_failover_v2: false
  systemd_watchdog: false                # 전제 충족 후 마지막에 활성
```

이행 순서: (1) `run/ha`·`state/ha` 디렉토리 + 보수적 verdict 스켈레톤 →
(2) desired 를 영속 경로로 migration(구 `run/ha/` 는 호환 전용) → (3) Supervisor shadow
모드(신 verdict 계산·로그 비교, keepalived 미사용) → (4) cims-notify role_writer 전환 +
Supervisor role reconcile → (5) cims-health → track_script verdict reader 축소 →
(6) 서비스별 `verdict_source=supervisor` 전환(영향 적은 것부터: stateless hot → cold 포함
→ 공유자원 사용) → (7) 계획 절체 v2 → (8) systemd watchdog.

기존 자산 재사용: `service_intent`→`desired_runtime`/`ha_intent` 확장, `module_specs`→
`failover_relevant`/`standby_mode`/`health_profile`/`safety`, `restart_limit`→Supervisor
Recovery Policy, `desired.json`→영속 경로 migration, cims-health 판정 로직→Supervisor
단계 이전, `keepalived.conf.tpl`→기존 notify 유지(cims-notify 내부만 role writer 로).

`_STAGGER_DELAY_SEC` 는 verdict-driven 완전 전환 후 제거한다(priority + eligible 이
선출을 결정하므로 중복). `prefer_first` 는 keepalived priority 입력으로만 남기고 프로세스/
평가/기동 지연 용도는 제거한다.

## 19. 미구현 · 후속 과제

- shared_writer(특히 csc DB) fencing/leader-lease — 미도입. 확인 전 `safety.class=unknown`
  = 수동 래치 보수 처리.
- CSP hash ring 의 `MarkUnhealthy` 미구현 — AA 장애 제외가 CSP `KeepAliveLoop`(3초×3회
  ≈9초)에만 의존. 본 HA 재설계와 별개 축의 CSP 과제.
- hot 모듈 양쪽 동시 다운 시 flap — `CIMS-QOS-001`(ha_flap) 알람으로 노출.
