# OAM self-upgrade (자기 교체)

> 배경: 2026-06-15 부트스트랩에서 OAM 을 root systemd → **agent(cims-svc) 감독**으로 전환
> ([[oam_csc_split]] §감독구조). 이때 남긴 미해결 설계 = "OAM 이 자기 자신을 업그레이드"
> 하는 경우의 특별취급. 본 문서가 그 설계다.

OAM(`oam_app.py`)은 다른 모듈(csp/cmp/csc/console)을 업그레이드·재기동하는 **제어면**
이면서, 동시에 자기 자신도 agent 가 감독하는 **하나의 supervised 모듈**이다. csp 를
업그레이드할 때와 달리, OAM 을 업그레이드하면 "업그레이드를 지휘하는 주체"가 업그레이드
대상과 같아진다. 이 자기참조를 안전하게 만드는 것이 본 설계의 목적이다.

---

## 1. 먼저 — 통념 교정: "OAM 이 자기 업그레이드를 서빙하다 죽는다"는 **틀렸다**

자주 빠지는 오해: *"OAM 이 자기 restart 요청을 HTTP 로 처리하는 도중 죽어 응답을 못 준다."*
실제 실행 모델은 그렇지 않다. **job 실행 주체는 OAM 이 아니라 agent 다.**

```
 Console ──POST /deployments/{id}/jobs {job_type:"restart"}──▶ OAM
                                                               │  _queue_job(): file_store 에
                                                               │  job 한 줄 기록 후 즉시 200 반환
 Console ◀────────────────── 200 (큐잉됨) ──────────────────────┘   (OAM 은 여기서 안 죽는다)

 Agent ──heartbeat──▶ OAM        (heartbeat 응답에 [restart job] 동봉; 이미 agent 손에 들어옴)
 Agent: execute_job(restart oam) ──▶ cims-svc restart oam ──▶ (구 OAM kill + 신 OAM start)
 Agent ──POST /api/agent/report──▶ (신) OAM       (지속 file_store 에서 job 이어받아 마감)
```

근거:
- OAM `_queue_job` 은 job 을 file_store 에 적고 **즉시 반환**(동기 재기동 없음).
  `oam/src/handlers/agents.py` `_queue_job` (job 생성 → deployment status 전이만).
- agent 가 heartbeat **응답**으로 job 을 수령한 뒤(`cims_agent.py:2396`) 별도로
  `execute_job`(`:2399`)에서 실행한다 → job 전달은 OAM 사망 **전에 원자적으로 끝남**.
- job/deployment 레코드는 **file_store(영속)** — 신 OAM 이 같은 파일을 읽어 이어받는다.

즉, 제어 루프는 OAM 인스턴스 수명과 **분리**되어 있다. 따라서 OAM self-upgrade 는
"불가능"이 아니라 "**거의 동작하나 거친 모서리(rough edge) 4가지**"가 있는 상태다.
본 설계는 그 4가지를 메운다.

---

## 2. 현행 경로를 그대로 태웠을 때의 동작 (baseline)

csp 업그레이드와 동일한 2-step(`upgrade` job → `restart` job)을 OAM 에 적용하면:

| 단계 | 실행 | OAM 생존? | 비고 |
|---|---|---|---|
| 1. `upgrade` job | agent `job_install()` → `modules/oam/<신버전>/` 전개 | ✅ | 버전 단위 병렬 설치 (`_versioned_install_path`) |
| 2. report | agent → OAM, `deployment.install_path` 갱신 | ✅ | OAM 아직 구버전 |
| 3. `restart` job | agent `job_process_control(restart, oam)` | **죽음→부활** | `cims-svc restart oam`: 구 kill + 신 start(sleep 1.5s) |
| 3b. supervised 갱신 | `_mark_supervised("oam", 신경로)` (rc==0 시) | — | watchdog 가 신버전 보호 (`cims_agent.py:1855`) |
| 4. report | agent → **신** OAM `/api/agent/report` | ✅(신) | 신 OAM 이 file_store 에서 job 이어받아 마감 |

이 경로는 **대개 성공**한다. 하지만 다음 4개 모서리에서 깨질 수 있다.

---

## 3. 메워야 할 4개 모서리 (gap)

### G1. report 가 "아직 안 뜬 신 OAM" 에 도달 → 결과 유실
`cims-svc restart oam` 의 `start_oam` 은 `sleep 1.5s` 후 떴는지만 확인
(`agent/lib/lifecycle.sh` `start_oam`). 그러나 Python OAM 콜드스타트는 config 로드 +
file_store v2 마이그레이션(P2/P3) + cert 로드 + bind 로 **1.5s 를 넘길 수 있다**.
이때 단계 4의 report POST(`cims_agent.py:2400`)는 connection refused →
`http_post` 예외 → **그 job 의 결과가 한 번에 유실**된다(현행 report 는 1-shot, 재시도 없음).
→ job 이 "running/dispatched" 상태로 영구 잔류, deployment status 가 `deploying` 에 멈춤.

### G2. 신 OAM 부팅 시 stale 상태 복구(reconcile) 없음
G1 또는 임의 사망으로 report 가 끝내 안 닿으면, 신 OAM 이 떠도 deployment/job 이
`deploying`/`restarting` 에 멈춘 채다. 신 OAM 은 "내가 곧 그 업그레이드의 결과물"임을
스스로 인지·정정하지 않는다.

### G3. pre-flight 검증 없음 → 깨진 패키지로도 구 OAM 을 죽인다
`restart` 의 stop 은 신 패키지 기동 가능 여부와 무관하게 구 OAM 을 먼저 죽인다
(`job_process_control` 의 prev-stop + `cims-svc restart` 의 stop_oam).
신 OAM 이 import/config 오류로 못 뜨면 **다운타임이 "잠깐"이 아니라 watchdog 백오프
(5→10→…→300s) 동안 지속**된다.

### G4. 롤백이 암묵적·취약
신 start 실패 시 `_mark_supervised` 가 호출되지 않아(rc≠0) supervised.json 은 **구 경로
유지** → watchdog 가 구버전을 재기동 → 사실상 자동 롤백이 *되긴 한다*. 그러나
(a) prev-stop 이 구 경로를 이미 내렸고, (b) 신 start 실패 진단이 어디에도 안 남으며,
(c) deployment.install_path 는 이미 신버전으로 갱신돼 있어 **레코드와 실제가 어긋난다**.
명시적·관측가능한 롤백이 필요하다.

---

## 4. 설계 원칙

**P1. OAM 은 절대 자기 자신을 kill/execv 하지 않는다.**
agent 자가업그레이드는 `os.execv`(같은 PID, 무중단; `cims_agent.py:2410`)로 가능하지만,
OAM 은 그 패턴을 **쓰지 않는다**. 이유: agent 가 단일 감독자(supervisor)이고, OAM 이
스스로를 다루기 시작하면 "감독자 vs 자기관리"의 이중 권위가 생긴다. **재기동 권한은
오직 agent(watchdog/cims-svc)에 둔다.** OAM 은 job 을 큐잉할 뿐 자기 프로세스를 만지지 않는다.

**P2. 짧은 계획된 다운타임을 수용한다 (무중단 아님).**
OAM 재기동 = 콘솔/제어 API 가 2~5s 불통. 이는 **허용**한다. 그 동안에도 csp/cmp 등
서비스면은 영향 없고(트래픽은 OAM 비경유), 각 노드 agent 는 로컬에서 모듈을 계속 감독
(`supervise_tick`)하며 heartbeat 는 백오프 재시도한다. OAM HA(active/standby)로 이 창을
0 에 가깝게 줄이는 것은 별도 과제(§7)로 분리한다.

**P3. 영속 상태(file_store)를 단일 진실원으로, 신 OAM 이 이어받는다.**
구 OAM 이 남긴 job/deployment 레코드를 신 OAM 이 읽어 마감·정정한다. agent 는 report 를
**끈질기게** 전달(재시도)해 결과가 유실되지 않게 한다.

---

## 5. 제안 설계 (gap 별 처방)

### D1 (→G1): agent report 재시도 + OAM 모듈에 한해 충분한 기동 대기
- agent `execute_job` 후 report POST 를 **재시도 가능**하게 한다(예: 신 OAM `/health`
  200 까지 최대 N=30s 폴링 후 report, 또는 report 실패 시 지수백오프로 6회 재시도).
  최소 침습안: report 실패 시 결과를 `run/pending_report.jsonl` 에 적어두고 다음
  heartbeat 직후 재전송. (csp 등 일반 모듈에도 무해한 일반화)
- 보강: OAM 모듈의 `restart`/`start` 는 `cims-svc` 가 sleep 1.5s 대신
  **포트 LISTEN + `/health` 200** 을 최대 T(기본 20s)까지 폴링 후 성공 판정.
  (`lifecycle.sh start_oam` 에 health-gate 추가; 다른 모듈은 기존 동작 유지)

### D2 (→G2): 신 OAM 부팅 시 self-reconcile
- OAM 기동 직후(현 마이그레이션 단계 옆, `oam_app.py` start 경로) 1회 실행:
  - 자기 실행 경로(`__file__` → `modules/oam/<ver>/`)에서 **현재 가동 버전**을 도출.
  - 자기 deployment 레코드가 `deploying`/`restarting` 이고 그 `package_version` ==
    가동 버전이면 → **`running` 으로 정정**하고 대응 job 을 `success(reconciled)` 로 마감.
  - 가동 버전 ≠ 레코드 목표 버전이면 → `failed(version_mismatch)` 마킹(롤백된 상태).
- 효과: report 가 끝내 유실돼도 "콘솔이 영원히 deploying" 상태가 자가 치유된다.

### D3 (→G3): kill 이전 pre-flight 검증
agent `job_install`(oam) 또는 별도 `validate` 단계에서 **구 OAM 을 내리기 전에**
신 패키지를 검증:
- 구문/임포트 스모크: `python3 -c "import sys; sys.path.insert(0,'<신>/oam/src'); import oam_app"`
  (csc/src·vendor sys.path 동일 구성으로). 실패 시 restart 자체를 **거부**(구 OAM 유지, job=failed).
- (선택, 더 강함) 신 OAM 을 **대체 포트로 카나리 기동** → `/health` 200 확인 → 종료 →
  그제서야 정포트 전환. 다운타임 최소화 + "뜨는 것 확인 후 전환".

### D4 (→G4): 명시적 롤백 + 레코드 정합
- `restart`(oam) 결과가 health-gate(D1) 안에 성공 못 하면 agent 가:
  1. supervised.json 을 **직전 버전 경로로 되돌리고**(prev_path 보존),
  2. `cims-svc start oam`(구버전)로 복구,
  3. report 에 `rolled_back: true` + 사유 첨부.
- 신 OAM self-reconcile(D2)가 version_mismatch 를 잡으면 deployment.install_path 도
  실제 가동 버전으로 되돌려 레코드-실제 어긋남(G4-c)을 해소.

---

## 6. job 흐름 (최종)

```
[Console] "OAM 업그레이드" (단일 액션, 내부적으로 upgrade→(validate)→restart 체인)
    │
    ▼ OAM._queue_job  (file_store 기록 후 즉시 200; OAM 안 죽음)
[Agent heartbeat] 수령
    │
    ├─ job:upgrade(oam)   → job_install: modules/oam/<신>/ 전개            (구 OAM 생존)
    │      └─ report → 구 OAM: deployment.install_path=신, status=deploying
    │
    ├─ job:validate(oam)  → import 스모크 / 카나리 기동 (D3)               (구 OAM 생존)
    │      └─ 실패 시: 구 OAM 유지, job=failed, 종료 (다운타임 0)
    │
    └─ job:restart(oam)   → cims-svc restart oam (health-gate, D1)
           ├─ 성공: _mark_supervised(oam,신), report(신 OAM, 재시도 D1)
           │         └─ 신 OAM self-reconcile(D2): status=running 확정
           └─ 실패: supervised=구 복원 + start 구버전(D4), report rolled_back
                     └─ 신/구 어느 OAM이든 self-reconcile 로 레코드 정정
```

watchdog 는 전 과정에서 **안전망**으로 동작: 어떤 사유로든 OAM 이 죽고 안 떠 있으면
`supervise_tick` 이 supervised.json 의 경로로 재기동(`cims_agent.py:1786`).

---

## 7. 범위 밖 (후속 과제)
- **OAM HA(active/standby)** 로 self-upgrade 다운타임을 ~0 으로: standby 먼저 업그레이드 →
  VIP 전환 → 구 active 업그레이드. 본 설계(P2)는 단일 OAM 의 짧은 계획 다운타임을 수용하는
  단계까지다. HA 는 [[ha_design]] 확장으로 별도 설계.
- **다운그레이드(롤백 버튼)**: 버전 단위 설치가 구버전 디렉터리를 (3개까지) 보존하므로
  `restart` 대상 경로만 구버전으로 지정하면 기술적으로 가능 — UX/검증은 별도.

---

## 8. 구현 체크리스트 (2026-06-16 구현 완료)
- [x] `agent/lib/lifecycle.sh`: `start_oam` 에 `_oam_health_gate`(프로세스 생존 + `/health`
      200 폴링, `CIMS_OAM_HEALTH_TIMEOUT` 기본 20s; python urllib probe — curl 부재 대비) — D1
- [x] `agent/cims_agent.py`: `_deliver_report`(재시도 4회 backoff) + `_enqueue_pending_report`
      /`_flush_pending_reports`(`run/pending_reports.jsonl`, heartbeat 성공 시 flush) — D1
- [x] `oam/src/oam_app.py`: 부팅 self-reconcile (실행 install_path == deployment.install_path
      인 oam·`deploying` 만 `running` 으로 정정 + stuck job 마감; 타 노드 오염 방지) — D2
- [x] OAM `/health` — httpsrv 내장 무인증 200 `{"status":"ok"}` (추가 불요, 확인 완료) — D1/D3
- [x] `oam/src/oam_app.py`: `--preflight`(import+config 스모크, bind 없이 종료 0/2) — D3
- [x] `agent/cims_agent.py`: `_oam_preflight` + `job_process_control` 에서 oam start/restart 전
      pre-flight 게이트(실패 시 구 OAM 유지, kill 안 함) — D3
      *(설계 원안의 'job_install 내 import-스모크' 대신 **restart 직전 seam** 에 배치 —
      구 OAM kill 을 직접 가드하므로 더 정확)*
- [x] `job_process_control`(start/restart, oam): health-gate 실패(rc≠0) 시 명시적 롤백
      (구 버전 start + supervised.json 구 경로 복원) — D4
- [ ] (선택) 카나리 대체포트 기동 경로 — D3 강화 (미구현, 후속)
- [ ] Console: "OAM 업그레이드" 단일 액션이 upgrade→restart 체인 큐잉 (미구현 — 현재는
      콘솔에서 upgrade·restart 를 순차 트리거하면 동작; 단일 버튼 UX 는 후속)

**검증(dev, TB-OAM)**: `--preflight` 정상 0 / 깨진 config 2 · `/health` 200 probe · D2
self-reconcile(stuck `deploying`→`running`, install_path 일치만; 불일치는 미정정 확인).
agent 측 D1/D3/D4 는 단위 동작 검증(헬스 probe·preflight 호출) — 전체 self-upgrade
E2E(실제 버전 전환)는 4노드 production 적용 시 라이브 확인 대상.

> ⚠️ 핵심 불변식: **재기동 권한은 agent 단독.** OAM 코드 어디에도 자기 프로세스를
> kill/exec/restart 하는 경로를 추가하지 않는다(P1). — 구현에서 준수(OAM 은 `--preflight`
> 로 자기검증만, 실제 kill/start 는 전적으로 agent `job_process_control`/watchdog).
