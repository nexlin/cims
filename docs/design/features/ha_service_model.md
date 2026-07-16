# HA 서비스 운영 모델 — 선언적 의도 · 모듈 운영 명세 · 절체 판정

> **상태: 구현 완료(실서버 절체 검증 대기).** 본 문서가 이 모델의 정본이다.
> OAM 렌더/제어(`ha_groups.py`), agent 상태·watchdog(`cims_agent.py`), 판정
> (`cims-health`), 콘솔(`ServersPage.tsx`)에 반영됨. keepalived·cims-notify 계약은
> 불변. §9 "현행 대비 변경점"은 이전 record-유추 모델과의 대조로 유지한다.

## 1. 목적과 원칙

현행 HA 는 운영자 의도를 **노드별 배포 record status(running/stopped)에서 유추**한다.
절체가 한 번 일어나면 record 와 실제가 어긋나는 것이 정상 상태(standby 는 record
stopped 인 채 notify 로 기동)라서, 유추 기반의 무장/해제 판정은 절체 이후 필연적으로
깨진다. 또한 감시(watchdog)와 헬스체크(cims-health)가 서로를 모른 채 경쟁하고,
운영자의 정지 조작이 장애와 구분되지 않아 절체를 유발한다.

본 설계는 유추를 제거하고 **선언적 상태 모델**(systemd unit / Kubernetes desired
state / Pacemaker 리소스 정의와 동일 계열)로 전환한다. 원칙:

1. **절체는 "장애" 또는 "명시적 절체 명령"으로만 일어난다. 운영자 조작은 장애가 아니다.**
2. **운영자 의도는 유추하지 않고 명시적으로 저장한다.** 배포 record 에서 의도를
   역산하지 않는다.
3. **로컬 복구(재기동)가 절체보다 먼저다.** 재기동 시도가 소진됐을 때만 절체한다
   (Pacemaker migration-threshold, systemd StartLimitBurst 계열).
4. **앱 설정과 운영 설정은 물리적으로 다른 파일이다.** 모듈 config.json(앱이 읽음)과
   운영·감시 명세(agent/HA 가 읽음)는 저장·전파 경로를 공유하지 않는다.

## 2. 상태 모델 — 3층 분리

| 층 | 의미 | 정본 | 갱신 주체 |
|---|---|---|---|
| **서비스 의도** (그룹 스코프) | "이 그룹에서 모듈 M 은 떠 있어야 한다/내려야 한다" | 그룹 record `service_intent` (모듈별 `running`/`stopped`) | 콘솔 일괄 제어(시작/중지), 서버별 start(승격 — §6) |
| **노드 오버라이드** (노드 스코프) | "이 노드에서는 지금 내려 둔다" (유지보수) | 노드 로컬 `run/ha/desired.json` | 서버별 stop/start |
| **배치** (어느 노드에서 도는가) | cold 모듈의 실행 위치 | **저장하지 않음** — keepalived/VRRP 가 결정 (VIP 보유자 = 배치 위치) | keepalived |
| **실측** | 지금 실제로 떠 있는가 | live_state(metric)·VIP 관측 | agent 보고 |

파생 규칙:

- **무장(armed)** = `service_intent[m] == running` 인 데몬 모듈이 존재하고 VIP
  바인딩이 있으면 vrrp_instance 렌더. 둘 중 하나라도 없으면 `enabled:false` →
  keepalived 정지 유지. record status·개시 래치·유추 일절 없음.
- **VIP 적용 시점은 자유.** VIP 와 의도는 독립 축 — 설치 전이든 start 전이든 VIP
  저장/적용 가능 (현행 409 `no_started_modules` 게이트 삭제). 미개시(의도 stopped)
  그룹은 VIP 가 저장돼 있어도 비무장이므로 아무 일도 일어나지 않고, 콘솔이 그
  상태를 명시 표기한다 (§8).
- **재설치·runtime store 유실·로드 예외로 record 가 어떻게 되든 무장에 영향 없다.**
  의도가 running 인 한 keepalived 는 무장 유지 → 장애 시 승격이 cold 모듈을
  재기동(자가 회복).
- deployment record 의 running/stopped 는 의도 겸직에서 해제되어 설치/버전 이력
  관리로 역할이 줄어든다. 콘솔 상태 표시는 실측 단일 원칙 유지.

## 3. 데이터 모델

### 3.1 OAM (SoT)

- **그룹 record** (`ha_groups` 도메인) 추가 필드:
  - `service_intent: { "<module>": "running" | "stopped" }` — 서비스 의도.
  - `failover_options` 는 **그룹(시스템) 스코프만 남긴다**: `advert_int`, `preempt`,
    `health{interval,fall,rise,timeout,grace_sec}`, `track_interface` +
    신규 `restart_limit: { max_fails: 3, window_sec: 300 }` (재기동 임계 — §5).
    `module_modes`/`tracked_modules` 는 모듈 운영 명세로 이관(제거).
- **모듈 운영 명세** (그룹×패키지 설정 도메인 확장): 모듈 스코프 운영 설정의 SoT.
  콘솔 [패키지 설정](그룹 선택)에서 편집.

### 3.2 노드 파일 레이아웃

| 파일 | 성격 | 쓰는 쪽 | 읽는 쪽 |
|---|---|---|---|
| `modules/<mod>/<ver>/…/config.json` | 앱 설정 (기존, 불변) | OAM 패키지 설정 경로 | 모듈 프로세스 |
| `modules/<mod>/service.json` | **모듈 운영 명세** (신규) | OAM → `update_module_spec` job | agent(watchdog·제어 게이팅), (렌더는 OAM 이 자기 SoT 사용) |
| `run/keepalived/ha.json` | 그룹 정책의 노드 복제본 (기존 확장) | OAM → `update_ha` job | agent(cold 게이트·임계), cims-health, cims-notify |
| `run/ha/desired.json` | 노드 오버라이드 (신규, 런타임 상태) | agent (서버별 start/stop job) | agent watchdog, cims-health |
| `run/ha/fail_<mod>` | 재기동 실패 카운터 (신규, 런타임 상태) | agent watchdog | cims-health |
| `run/ha/op_grace_<mod>` | 조작 유예 마커 (신규, 런타임 상태) | agent (제어 job 시작 시 touch) | cims-health |

`modules/<mod>/service.json` 스키마:

```json
{
  "supervision": { "watchdog": true },
  "ha": {
    "failover_mode": "cold",        // "cold" | "hot"
    "failover_relevant": true       // 이 모듈 실패가 절체 사유인가 (구 tracked 의 승계)
  },
  "health": { "port": 4421, "proto": "tcp", "config_key": "Server.Port" }
}
```

- 위치가 버전 트리 밖(모듈 루트)이므로 업그레이드에 안전하고, uninstall 시 모듈과
  함께 철거된다. 기존 `supervised.json` 의 역할은 `service.json(supervision)` +
  `run/ha/desired.json` 으로 대체·정리한다.
- ha.json `services.<svc>` 에는 렌더 결과로 `restart_limit` 가 추가되고,
  `cold_modules`/`tracked`/헬스 힌트는 지금처럼 내려가되 그 원천이
  `failover_options` 가 아니라 모듈 운영 명세 + `service_intent` 로 바뀐다.

## 4. 절체 판정 체계

### 4.1 판정 표

| 사건 | 동작 | 감지·소요 |
|---|---|---|
| 노드/keepalived 사망 | 즉시 절체 | advert 소실 ~3s (정상 종료는 priority-0 로 ~1s) |
| 감시 모듈 crash | **watchdog 재기동 → 연속 `max_fails`회 실패 시 절체** | N×backoff (기본 3회 ≈ 15~20s) |
| 포트 무응답 (프로세스 생존 — 좀비) | health fall 후 절체 (현행 유지 — watchdog 이 도울 수 없는 케이스) | fall ~4s |
| 운영자 **서버별 stop** (active 포함) | **절체 없음** — 검사 제외, VIP 유지, 서비스 중단은 운영자 책임 (콘솔 경고 표기) | — |
| 운영자 **서버별 restart / 일괄 재시작** | **절체 없음** — 조작 유예 마커로 health 유예 | — |
| 운영자 **일괄 중지** | 비무장 + 전체 정지 (의도 stopped) | — |
| 운영자 **수동 절체** | 즉시 스위치오버 (§7) | ~1s + cold 기동 |
| 미개시(의도 stopped) 그룹 | keepalived 정지 — 무장 자체가 없음 | — |

### 4.2 watchdog ↔ cims-health 협조 (경쟁 제거)

역할을 명시적으로 나눈다: **watchdog = 복구 주체, cims-health = watchdog 의
성적표를 읽는 심판.**

- watchdog: `service.json(supervision.watchdog)` && `desired.json` 상 running 인
  모듈이 죽으면 재기동. 시도마다 `run/ha/fail_<mod>` 카운터 증가(윈도우
  `restart_limit.window_sec` 밖의 기록은 만료), **성공 생존 확인 시 리셋**.
  cold-standby 게이트(VIP 미보유 + keepalived 가동 중이면 보류)는 현행 유지.
- cims-health (VIP 보유 노드 검사): 판정 순서 —
  1. `desired.json[m] == stopped` → **검사 제외** (의도적 정지 ≠ 장애)
  2. `op_grace_<mod>` mtime 이 유예창 이내 → PASS (제어 job 진행 중)
  3. 프로세스/포트 정상 → PASS (카운터는 watchdog 이 리셋)
  4. 프로세스 다운 && `fail_<mod> < max_fails` → **PASS 유예** (재기동 진행 중 —
     현행의 fall 4s vs watchdog 5s 경쟁이 구조적으로 소멸)
  5. 프로세스 다운 && `fail_<mod> ≥ max_fails` → FAIL → FAULT → 절체
  6. 프로세스 생존 && 포트 무응답 → FAIL (fall 누적, 현행 좀비 경로)
- cims-health 는 `ha.json.cims_home` 으로 `run/ha/` 경로를 유도한다 (root 실행,
  기존 grace 마커 `master_at_<svc>` 와 동일한 mtime 패턴).
- 판정 대상은 `failover_relevant: true` 모듈만. 부가 모듈의 실패는 절체 사유가
  아니다 (알람으로만 노출).
- 트레이드오프(의도된 것): 진짜 죽은 모듈의 절체가 재기동 소진만큼 늦어진다.
  일시 crash 1회로 VIP 를 옮기지 않기 위한 표준적 선택이며, `restart_limit` 로
  운영자가 민감도를 조절한다.

## 5. 재기동 임계 (restart_limit)

- **정책은 그룹(시스템) 스코프** — 절체 조건 UI 에서 편집, `failover_options.
  restart_limit` 로 저장, ha.json 으로 렌더. 근거: 이 정책의 결과(VIP 이동)가
  그룹 전체의 사건이고, 판정의 의미가 "이 노드에서 이 시스템을 계속 살릴
  것인가"라는 노드 포기 결정이기 때문. (Pacemaker 도 per-resource 파라미터지만
  실무에선 클러스터 기본값 하나로 쓰는 것과 같은 선택. 모듈별 오버라이드는
  필요해질 때 명세 확장으로.)
- 기본값 `max_fails: 3, window_sec: 300`.
- **관여 여부는 모듈 스코프** — `service.json ha.failover_relevant`.
- 카운팅은 노드×모듈 로컬 메커니즘 (`run/ha/fail_<mod>`), 설정이 아니다.

## 6. 운영자 액션 시맨틱

| 액션 | 의도 변화 | job 시퀀스 |
|---|---|---|
| **[▶ 일괄 시작]** (그룹) | `service_intent[*]=running`, 노드 오버라이드 클리어 | 재렌더(무장) — 개시 스태거: 기준 멤버(priority 최대 또는 지정) 선행, 나머지 지연 → 각 멤버 start (cold 는 기준 멤버만 실기동, standby 는 게이트가 억제) |
| **[■ 일괄 중지]** (그룹) | `service_intent[*]=stopped`, 오버라이드 클리어 | 전 멤버 **[비무장 update_ha → stop]** 순서 큐잉 — agent 는 큐를 순서 처리하므로 마지막 말이 항상 stop (절체 레이스가 중간에 모듈을 살려도 최종 상태 결정적) |
| **[⟳ 일괄 재시작]** (그룹, AS) | 불변 | standby 먼저 → active 나중. active 재시작 전 `op_grace` 마커로 health 유예 → **절체 없음** (순단 1회). AA 는 한 대씩 rolling |
| **[⇄ 수동 절체]** (그룹, AS) | 불변 | §7 |
| **서버별 start** | 그룹 의도가 stopped 였으면 `running` 으로 승격 + 해당 노드 오버라이드 클리어 | 해당 노드 우선 무장(스태거 선행) → start. "start 누른 서버가 Active" 합의 유지 |
| **서버별 stop** | 그룹 의도 불변, 해당 노드 `desired[m]=stopped` | stop job (agent 가 desired 기록 → watchdog·health 즉시 제외). **active 노드여도 절체 없음** — VIP 잔류, 콘솔이 "서비스 중단·절체 안 함" 경고 |
| **서버별 restart** | 불변 | `op_grace` 마커 → restart. active 여도 절체 없음 |

노드 오버라이드와 절체의 상호작용: `desired[m]=stopped` 인 노드는 해당 모듈이
`failover_relevant` 이면 **승격 자격이 없다** (cims-health 비보유 검사에서 FAIL).
운영자가 명시적으로 내려 둔 노드에서 절체가 서비스를 켜는 것은 원칙 1 위반.
양쪽 모두 오버라이드로 내려가 있으면 서비스 전면 다운이 의도된 결과다.

## 7. 수동 절체 (스위치오버)

keepalived 에는 네이티브 스위치오버 명령이 없다. 정상 종료의 priority-0 advert
(peer 가 ~1s 내 인수)를 이용한다:

```
POST /ha-groups/{gid}/failover        (admin, AS 전용)
 1. 검증: 그룹 무장 상태, 현 Active 확정(VIP 관측), 대상 standby 의 승격 자격
    (모듈 설치, desired 오버라이드 없음, agent online)
 2. job → Active 노드: keepalived stop
      → STOP notify 는 서비스 유지(A 모듈 계속 실행), B 가 priority-0 수신
        → 즉시 MASTER → VIP 인수 → notify 가 cold 모듈 기동 (grace 30s)
 3. OAM: VIP 관측(heartbeat 2s)으로 B 인수 확인 (timeout 시 실패 보고 + A keepalived 재기동)
 4. job → 구 Active 노드: keepalived start
      → nopreempt 라 BACKUP 으로 복귀 → BACKUP notify 가 A 의 cold 모듈 정지
```

2~4 사이 양쪽 모듈이 동시에 떠 있는 수 초의 창이 있다(VIP 는 B — 트래픽은 정상,
A 는 nonlocal_bind 로 유휴). 4단계 BACKUP notify 가 정리한다.

## 8. 콘솔 UI 재배치

- **절체 조건** (그룹 편집): 그룹 스코프만 — advert_int, preempt, 헬스 타이밍
  (interval/fall/rise/timeout/grace), track_interface, **재기동 임계(N회/윈도우)**.
  프로세스 감시·모듈 절체 모드 항목 제거.
- **패키지 설정** (그룹 선택 → 모듈별): **모듈 운영 명세** 편집 — 프로세스 감시
  (watchdog), 절체 모드(cold/hot), 절체 관여(failover_relevant), 헬스 프로브
  오버라이드. 저장 = 전 멤버 `update_module_spec` push + `update_ha` 재렌더.
  기존 앱 설정(config) 편집과 별개 섹션으로 구분 표기.
- **패키지 제어** (그룹 선택): 멤버×모듈 매트릭스 상단에 **그룹 일괄 제어 바**
  [▶ 일괄 시작] [⟳ 일괄 재시작] [■ 일괄 중지] [⇄ 수동 절체]. 개별 서버 버튼
  유지 — 의미는 "이 노드만" (절체를 유발하지 않음, active 노드 정지 시 경고).
- **상태 표기**: 미개시(의도 stopped) 그룹은 Active/Standby 대신
  "미개시 (비무장 — 서비스 시작 시 무장)" 배지. active 노드에 desired 오버라이드가
  있으면 "Active·모듈 정지 중 (절체 안 함)" 경고.

## 9. 현행 대비 변경점 · 마이그레이션

| 현행 | 변경 |
|---|---|
| 무장 = record status running 유추 (`_group_started_modules`) | 무장 = `service_intent` 명시 값 |
| VIP 적용 게이트 (409 `no_started_modules`) | 삭제 — VIP 적용 시점 자유 |
| `failover_options.module_modes/tracked_modules` (그룹 record) | 모듈 운영 명세로 이관 — 마이그레이션: 기존 값을 그룹×패키지 명세로 1회 변환 |
| `supervised.json` (start/stop job 이 암묵 기록) | `service.json(supervision)` + `run/ha/desired.json` 으로 분해 |
| watchdog 과 cims-health 무관계 (fall 4s vs backoff 5s 경쟁) | 카운터 파일 협조 — 재기동 소진 후에만 절체 |
| 운영자 stop → health FAIL → 절체 유발 | desired 오버라이드로 검사 제외 — 절체 없음 |
| 그룹 일괄 제어 없음 | 일괄 시작/중지/재시작 + 수동 절체 |
| 개시 국면 스태거 (start 멤버 선행) | 유지 — 입력만 record 유추 → 의도/기준멤버로 교체 |
| agent watchdog backoff 리셋 결함 (수 초 생존 후 사망 시 시도 1 로 리셋) | 카운터 윈도우 도입으로 함께 해소 |

구현 매핑 (완료): (1) 데이터 모델·렌더 — `ha_groups.py` (service_intent/module_specs
마이그레이션·정규화, 렌더가 intent 기반 무장 + restart_limit/relevant_modules 출력,
VIP 409 삭제) → (2) agent — `cims_agent.py` (`run/ha/desired.json`·`fail_<mod>`·
`op_grace_<mod>`, watchdog 협조, `update_module_spec`·`ha_keepalived` job) →
(3) `cims-health` 판정 재작성 → (4) 일괄 제어·수동 절체 API (`_control_group`/
`_failover_group`) → (5) 콘솔 `ServersPage.tsx` (절체조건 슬림화+재기동임계,
ModuleSpecSection, GroupControlMatrix 일괄 바+수동절체) → (6) 본 문서·ha_design 현행화.
하위 호환: 신 ha.json 필드를 구 cims-health 는 무시(REQ/tracked fallback), 구 ha.json
을 신 cims-health 는 relevant fallback·restart_limit default 로 수용 — 롤링 안전.

## 10. 미결 · 후속 과제

- **hot 모듈의 자가 회복 한계**: hot 은 notify 관리 대상이 아니므로 양쪽 동시
  다운 시 FAULT↔FAULT flap — `CIMS-QOS-001`(ha_flap) 알람으로 노출 (현행 유지).
  watchdog 이 hot 모듈은 VIP 무관하게 감시하므로 실질 위험은 낮음.
- **all_active 그룹**: 일괄 제어는 동일 적용, 수동 절체·재기동 임계는 AS 전용.
- **노드 유지보수 모드** (Pacemaker standby node 상당 — 노드 단위 일시 비무장):
  서버별 stop 오버라이드로 대부분 대체되므로 도입 보류.
- csp `LocalIp=VIP` 모듈의 VIP 선행 bind 는 `_ensure_nonlocal_bind`(agent 기동 시)
  가 전제 — 순서 자유화의 기반으로 유지.
