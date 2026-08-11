# 알람(이벤트) 표준화 설계 — X.733 / 3GPP TS 32.111

CIMS 알람을 임의 스키마(`critical`/`warning` 2단계)에서 **IMS 망관리 표준**에 맞춰 체계화하기 위한 설계.
구현은 단계(P0/P1)로 분리하며, 본 문서는 표준 매핑·제안 모델·이행 계획을 확정한다(코드 변경 전 SoT).

## 1. 참조 표준

- **3GPP TS 32.111-2** — *Telecommunication management; Fault Management; Part 2: Alarm Integration
  Reference Point (IRP): Information Service (IS).* IMS 포함 3GPP 망요소(NE/EM)가 관리객체의 알람을
  Manager(NMS)로 보고하는 인터페이스. CIMS 가 향후 상위 NMS 와 연동할 때의 정합 기준.
- **ITU-T X.733** — *Systems Management: Alarm reporting function.* 위 IRP 가 기반하는 알람(장애) 속성 모델.
- **ITU-T X.730 / X.731** — *Object Management / State Management.* 정상 동작 통지(객체 생성·삭제, **stateChangeNotification**) — 알람과 구분되는 **이벤트** 규격(§3.6).
- **IETF RFC 3877** — *Alarm Management Information Base (MIB).* X.733 의 SNMP 표준화 — 알람 정의 카탈로그(`alarmModelTable`) + 활성 알람(`alarmActiveTable`) 분리. 향후 SNMP/NMS northbound 연동 기준(§7.3).
- (참고) **ITU-T M.3100 / X.736 / X.740** — 관리객체 모델 / 보안 알람 / 보안감사(audit) 통지.

### 1.1 X.733 핵심 알람 속성

| 속성 | 설명 | 값 |
|---|---|---|
| **perceivedSeverity** | 인지 심각도 | Critical · Major · Minor · Warning · Indeterminate · **Cleared** |
| **eventType** | 알람 유형(분류) | communications · qualityOfService · processingError · equipment · environmental |
| **probableCause** | 표준 추정 원인 | softwareError · underlyingResourceUnavailable · thresholdCrossed · storageCapacityProblem · applicationSubsystemFailure · resourceAtOrNearingCapacity … (X.733 Annex) |
| **specificProblem** | 구체 문제(자유 텍스트/세분) | 메시지 |
| **managedObjectInstance** | 알람 발생 객체(source) | service/module/host instance (DN) |
| **eventTime / clearTime** | 발생 / 해제 시각 | UTC ISO8601 |
| **additionalText / additionalInformation** | 부가 정보 | — |
| **ackState / ackTime / ackUserId** | 승인 상태/시각/주체 (Alarm-list) | acknowledged · unacknowledged |
| **alarmId / correlatedNotifications** | 알람 식별 / 연관 알람 | — |

해제는 별도 삭제가 아니라 **perceivedSeverity=Cleared** 통지로 표현(현재 CIMS 의 `action=close` 와 동치).

## 2. 현재 CIMS 알람 모델 (인벤토리)

- **규칙** (`service_descriptors_seed/cims.json` `alert_rules[]`, `service_registry._CORE_ALERT_RULES`):
  `{ type, severity(critical|warning), check, target?, threshold?, unit?, metric, msg_open, msg_close, scope? }`
- **평가/발생** — 코어는 `ems/core/oam/src/services/alarm_sweeper.py`(emit/transition/서비스 규칙
  평가). 소유 분리(oam_base_service_split §4): **서비스 계열**(csp/cmp/db/rtp, scope≠`agent`)은
  **oam-svc** sweeper 가 발화(`oam_svc_app.py`, `detected_by='oam-svc'`; `--role all` 단일 프로세스
  에서만 base 가 대행 `detected_by='oam'`), **agent 계열**(disk/module)은 base(`oam_app.py`
  `_sweep_alerts`→`_eval_agent_rule`) 잔류. 기동 시 open-state 복원도 소유 계열별
  (`restore_open_state` — 서비스=`cims/*` mo, agent=그 외).
- **이벤트 레코드** (`ems/core/oam/src/services/alert_log.py`, `{ServiceLogDir}/alerts/YYYY/MM/DD.jsonl`):
  `{ ts, type, severity, action(open|close), message }`
- **API** (`ems/core/oam/src/handlers/alerts.py`): `GET /alerts`, `/types`, `/summary`, `/rules`.
- **UI**: `AlertsPage.tsx`(이력/통계/규칙) · `AlertBannerWidget`(활성 배너). 색은 critical/warning 2색.

### 2.1 격차

| 표준 | 현재 | 격차 |
|---|---|---|
| perceivedSeverity (6) | critical/warning (2) | 🔴 Major/Minor/Indeterminate 부재 |
| eventType (5) | 없음 | 🔴 분류/필터 기준 부재 |
| probableCause | 없음 | 🔴 근본원인/자동대응 기반 부재 |
| managedObject(source) | type 문자열에 암묵 | 🟡 명시 객체 부재 |
| clearTime | close 이벤트 ts 로 추론 | 🟡 |
| ackState/ackTime/ackUser | 없음 | 🟡 운영 감사추적 부재 |

## 3. 제안 — CIMS 표준 알람 모델

### 3.1 규칙(alert_rules) 확장 스키마

```jsonc
{
  "type": "service_unresponsive",      // 알람 **클래스** 슬러그 (프로세스명 박지 않음 — §3.5)
  "code": "CIMS-PRC-004",              // 알람 클래스 코드(카탈로그) — §3.4
  "perceived_severity": "major",       // critical|major|minor|warning|indeterminate  (기존 severity 대체)
  "event_type": "processingError",     // communications|qualityOfService|processingError|equipment|environmental
  "probable_cause": "responseTimeExcessive",   // X.733 Annex 코드
  "mo_class": "service",               // managedObject class: software|service|equipment|host|network
  "check": "service_unresponsive", "target": "csp",   // 무엇을 점검할지(탐지) — 어느 프로세스는 여기서, 알람 type 엔 안 박음
  "mo_instance": "cims/csp",           // (선택) 소스 instance 명시 — 없으면 target/host 로 런타임 합성
  "threshold": null, "unit": null,
  "thresholds": null,                  // (선택) 단계 임계 {severity: value} — 예 {"minor":80,"major":90,"critical":95}
                                       //   도달한 최고 단계가 severity 가 되고, 승격/완화는 action=change 로 통지.
                                       //   있으면 threshold/perceived_severity 단일값 대신 사용 (disk·rtp 기본 적용)
  "metric": "서비스 응답성",
  "msg_open": "{mo} 관리 프로브(STATS) 무응답",   // → specificProblem ({mo}=mo_instance)
  "msg_close": "{mo} 응답 정상화",
  "effect": "제어/관측 불가 — hang·과부하 의심",        // (선택) 영향 — 운영 runbook (Clearwater 벤치마크 §7.1)
  "recommended_action": "프로세스 상태·부하 확인 / 로그 확인",  // (선택) 권장 조치 (이벤트의 action=open/close 와 구분)
  "scope": "service"                   // service|agent (유지)
}
```
- **type/code 는 알람 클래스** (service_unresponsive). 어느 프로세스인지는 `source.mo_instance`(§3.4/§3.5). `csp_down`/`cmp_down` 처럼 프로세스명을 type 에 박지 않음.
- `perceived_severity` 가 기존 `severity` 를 대체. **하위호환**: `severity` 만 있으면 perceived_severity 로 승격(critical/warning 표준 값 유효). 신규 major/minor/indeterminate 가능.
- managedObject **instance** 는 `mo_instance` 명시 또는 런타임 합성(§3.4): service 규칙 = `cims/<target>`, agent 규칙 = `<host>/<module|disk|rtp>`. CMP 는 다중 미디어 노드(AA)를 개별 관측하므로 endpoint 별 `cims/cmp/<ip>:<port>` 로 합성.

### 3.2 이벤트 레코드(alert_log) 확장

```jsonc
{
  "ts": "2026-05-30T09:31:05",         // eventTime
  "alarm_id": "CIMS-PRC-001@Media-Server-01/csp@1748590265",  // 발생 인스턴스 고유 id (occurrence) — §3.4
  "type": "process_down",              // 정의 슬러그
  "code": "CIMS-PRC-001",              // 정의 코드
  "perceived_severity": "critical",    // open=규칙 severity, close=cleared
  "event_type": "processingError",
  "probable_cause": "softwareError",
  "source": {                          // 발생 소스 (§3.4)
    "mo_class": "software",            //   managedObjectClass
    "mo_instance": "Media-Server-01/csp",  //   managedObjectInstance (DN-유사)
    "detected_by": "agent"            //   탐지 주체 클래스 (agent|self|oam-svc|oam) — §3.4(b)
  },
  "action": "open",                    // open|close (close = Cleared)
  "message": "Media-Server-01 모듈 csp 프로세스 응답 없음",  // specificProblem
  "raised_time": "2026-05-30T09:31:05",   // 32.111 alarmRaisedTime — open 레코드는 ts 와 동일,
                                          //   close 레코드는 alarm_id 의 occurrence epoch 에서 복원
  "ack_state": "unacknowledged"        // P1: acknowledged 시 ack_time/ack_user 추가
}
```
- 기존 필드 보존(`ts/type/action/message`) → 이력/통계/배너 무중단. `severity`→`perceived_severity`, `managed_object`(평면 문자열)→`source.mo_instance` 로 점진 치환(읽기 시 둘 다 허용).
- `alarm_id` 로 open↔close 상관 및 ack 대상 지정. 동일 활성알람 식별 = `(code, source.mo_instance)`.
- **발생/해제/변경 시각 명시** (32.111 alarmRaisedTime/ClearedTime/ChangedTime): open 레코드는
  `raised_time`, close 는 `clear_time` + `raised_time`, change 는 `change_time` + `raised_time`
  을 명시 — 레코드 단독으로 지속시간 산출 가능 (open 레코드 역추적 불요).
- **severity 변경 = `action: "change"`** (32.111 notifyChangedAlarm): 같은 alarm_id 에
  새 `perceived_severity` + `trend_indication`(moreSevere|lessSevere). open/close 카운트에
  포함되지 않는다 (§3.4(d)).
- **코멘트 = `action: "comment"`** (32.111 setComment/notifyComments): `POST /alerts/comment
  {alarm_id, text}` → `{comment, comment_user, comment_time}` 레코드. ack 과 같이 통계
  미집계, 판독측은 해당 활성 행에 누적 표시 (AlertsPage 💬).
- **임계 계열 구조화** (X.733 thresholdInfo): threshold_crossed 계열 레코드는
  `threshold_info: {observed, threshold, unit}` 를 동반 — 관측값/임계값이 message 문자열에만
  있지 않고 기계 판독 가능 (rtp 사용률·disk·ha_flap).

### 3.3 현재 CIMS 알람 → 표준 매핑 (확정)

기존 6개(csp_down/cmp_down/module_down/db_down/rtp_high/disk_high) → **조건 클래스**로 정규화.
어느 프로세스/리소스/호스트인지는 `source.mo_instance`, 심각도/임계/원인은 **rule 속성**(클래스 정체성 아님).

| code | type(클래스) | eventType | probableCause (rule별) | mo_class | mo_instance 예시 | severity(rule별) | detected_by |
|---|---|---|---|---|---|---|---|
| `CIMS-PRC-001` | `process_down` | processingError | softwareError | software | `<host>/<module>` (전 모듈 — csp/cmp 포함) | critical | agent |
| `CIMS-PRC-004` | `service_unresponsive` | processingError | responseTimeExcessive | service | `cims/csp` · `cims/cmp/<ip>:<port>`(미디어 노드별) | major | oam-svc / oam |
| `CIMS-COM-001` | `connection_lost` | communications | communicationsSubsystemFailure / underlyingResourceUnavailable | service | `cims/db` (모듈 관점은 `cims/<mod>/<node>/db` — 자기보고 §4) | critical | oam-svc / oam / self |
| `CIMS-QOS-001` | `threshold_crossed` | qualityOfService | thresholdCrossed / storageCapacityProblem / resourceAtOrNearingCapacity | service·host | `cims/rtp_ports` · `<host>/disk` · `<host>/ha/<svc>`(check=ha_flap, 전이 빈도 임계) | 단계 임계(disk·rtp: minor 80 / major 90 / critical 95, 승격=action change) · ha_flap warning | oam-svc·oam / agent |
| `CIMS-PRC-003` | `config_out_of_sync` | processingError | configurationOrCustomizationError | software | `<host>/<module>/config` · `cims/ha/g<gid>/<collection>`(HA fan-out) | warning | agent / oam |

`CIMS-PRC-003` 은 배포기록 실체화본(config_template default + overlay)의 canonical hash 와
agent 가 보고하는 노드 실파일(`metric.cfg_hashes`) hash 의 불일치 = 설정 드리프트를 노출한다.
`ha_flap`(QOS-001 rule) 은 agent 가 cims-notify 로그에서 집계한 `metric.ha_transitions`
(최근 10분 keepalived 전이 수, 기본 임계 6회)로 VIP flap 을 노출한다 — 전이 개별 건은
§3.6 대로 이벤트(로그)일 뿐이며, 알람은 빈도 임계 초과라는 *조건*이다.

> **통합 원리**(§3.5): 같은 *조건*은 한 클래스. `module_down`→`process_down` / `rtp_high`+`disk_high`(+cpu/mem/network)→`threshold_crossed` / `db_down`→`connection_lost`. 구 `csp_down`/`cmp_down` 은 "프로세스 생존"과 "관리 응답성" 두 *조건*의 혼합이었다 — 생존은 `process_down`(agent 관측), 응답성은 `service_unresponsive`(OAM probe)로 분리한다(§3.4(b) 감지 3계층). 어느 리소스인지는 **source**, 임계값·단위·probableCause·severity 는 **rule** 이 보유 → 새 리소스(cpu/mem/network) 추가 시 **type/code 신설 없이 rule 만 추가**.
> 같은 클래스라도 rule 별로 probableCause/severity 가 다를 수 있음(disk→storageCapacityProblem/warning, rtp→resourceAtOrNearingCapacity/warning, 단계별 minor→major).
> `process_down` 은 전 모듈을 agent 관측으로 판정한다 — agent module 점검에서 csp/cmp 를 제외하던 규칙(proc_down_targets)은 두지 않는다. 같은 모듈에 `process_down`(L1)과 `service_unresponsive`(L3)가 함께 열리는 것은 중복이 아니라 별개 조건이며, correlatedNotifications(P2)로 상관을 명시한다.

### 3.4 알람 코드 체계 · 발생 소스 · occurrence id

**(a) 알람 코드 (클래스 카탈로그 식별자)** — `type`(클래스) 슬러그와 1:1, 안정적·불변. 운영 alarm dictionary / 상위 NMS 연동 키.
포맷 `**<SERVICE>-<DOMAIN>-<SEQ>**`:
- `SERVICE` = 서비스 pack 네임스페이스 (CIMS, 타 서비스는 자기 prefix → 코드 충돌 없음).
  단일 서비스 문맥 화면에서는 표시상 생략 가능(UI 재량) — 저장·연동 키는 항상 풀 코드.
- `DOMAIN` = eventType 약어: **PRC**(processingError) · **COM**(communications) · **QOS**(qualityOfService) · **EQP**(equipment) · **ENV**(environmental).
- `SEQ` = 3자리, **조건 클래스당 1개**(객체 인스턴스마다 부여 ❌ — 인스턴스는 source). 예: PRC-001 process_down, COM-001 connection_lost, QOS-001 threshold_crossed. 같은 도메인 내 새 *조건* 클래스가 생기면 002,003…
- 코드 카탈로그 = descriptor 의 alert_rules 클래스 집합(코어 + 서비스) + 모듈 자기보고 등록분. `GET /alerts/catalog` 로 노출.

**코드 문법 규칙 (신설 시 준수)**:
- `SEQ` 는 **무의미 일련번호** — 조건 클래스의 정체성은 code↔type 1:1 뿐, 번호에 의미를 싣지 않는다.
- eventType(DOMAIN) 외의 분류는 코드에 **인코딩하지 않는다** — probableCause 는 rule 속성(같은
  클래스 안에서 rule 별로 다름 — disk→storageCapacityProblem, rtp→resourceAtOrNearingCapacity),
  specificProblem 은 발생 건의 message(자유 서술)라 열거 불가. 코드에 박으면 속성 정정 =
  코드 개정(NMS 사전 키 단절)이 되고, 클래스가 원인 수만큼 쪼개진다(§3.5 안티패턴의 원인 축 재현).
  eventType 만은 클래스 정체성의 일부(재판정 없음)라 코드에 넣어도 안전.
- **코드는 불변** — northbound 연동 전에 한해 개정 가능하며, 개정은
  `service_registry._CODE_REVISIONS`(옛→현행)에 기록한다. 옛 code 규칙은 read 시 alias,
  옛 code 로 열려 있던 활성 알람은 스윕이 이행 종결(close)하고 지속 조건은 현행 code 로
  재발행한다(`alarm_sweeper.close_legacy_code`). 개정 이력: `CIMS-CFG-001`→`CIMS-PRC-003`
  (CFG 는 DOMAIN=eventType 약어 규칙 위반 — processingError 의 PRC 로 정정).

**(b) 발생 소스 (managedObject + detected-by)** — 알람이 "무엇에서/어디서" 났는지 표준화.
- `mo_class`: software | service | equipment | host | network (managedObjectClass).
- `mo_instance`: DN-유사 경로. service 규칙 = `cims/<target>` · agent 규칙 = `<host>/<module|disk|rtp>` · 자기보고 = `cims/<module>/<node>[/<component>]`. (계층: `<service|host>/<component>[/<instance>]`) **발생 노드/호스트는 mo_instance 가 유일 보유자다** — detected_by 에 중복하지 않는다.
- `detected_by`: 탐지 주체 **클래스** — `agent` | `self` | `oam-svc`(분리 배포) | `oam`(단일 프로세스 `--role all` 대행, HA fan-out drift 등 OAM 자체 판정). 인스턴스 접미(`agent:<host>`·`self:<node>`)는 두지 않는다 — 노드는 mo_instance 와 (자기보고는 wire 의) envelope `hdr.node` 가 보유하고, detected_by 는 ① 발행 주체별 open-state **소유 파티션**(복원·스윕·stale close 의 scope 필터) ② **감지 계층 식별**(아래 표, correlatedNotifications 상관 근거) 에 쓰인다. 고장 객체(mo_instance)와 탐지 주체가 다를 수 있음(예: db_down 은 oam-svc 탐지, 객체는 cims/db).

**감지 주체 3계층** — 모듈 상태는 서로 다른 사실을 보는 세 주체가 나눠 감지하며, 한 주체의
관측을 다른 계층의 의미로 쓰지 않는다(원격 probe 무응답 ≠ 프로세스 사망).

| 계층 | 주체 (detected_by) | 보는 사실 | 알람 / 이벤트 |
|---|---|---|---|
| **L1 프로세스 생존** | agent (`agent`) | 노드 로컬 프로세스의 기동/종료 — 모듈을 배포·실행하는 주체가 생존도 판정 (ha_service_model §1 "판정은 노드 로컬") | `CIMS-PRC-001` process_down (**전 모듈**) · 이벤트 `process_died`(전이 관측 — SIGKILL 등으로 모듈 자기보고가 유실되는 종료 보완) |
| **L2 모듈 내부 상태** | 모듈 자기보고 (`self`) | 살아 있는 프로세스의 내부 이상 — DB 연결·자원 풀·스토어 | `CIMS-COM-001` · `CIMS-QOS-002` · `CIMS-PRC-002` · 이벤트 `process_started`/`process_stopping` ([alarm_self_reporting.md](alarm_self_reporting.md)) |
| **L3 서비스 응답성** | OAM 원격 probe (`oam-svc`/`oam`) | 살아 있어도 응답하지 못하는 상태 — hang·과부하 (STATS 무응답, DB SELECT 실패) | `CIMS-PRC-004` service_unresponsive · `CIMS-COM-001`(`cims/db`) |

호스트 자원(disk)·HA 전이 빈도(ha_flap)·설정 정합(config drift)은 agent 원시 metric 을 OAM 이
평가하는 기존 경로(detected_by=`agent`)이고, HA fan-out drift 는 OAM 자체 판정(`oam`)이다.
L1 과 L3 는 같은 모듈에 함께 열릴 수 있다(프로세스 사망 시 probe 도 무응답) — L3 발화 시 같은
모듈의 활성 L1 이 있으면 correlatedNotifications(P2)로 참조하는 것이 첫 실사용처다.

**(c) alarm_id (발생 인스턴스 id, occurrence / X.733 notificationIdentifier)**
- 활성 알람 식별 = `(code, mo_instance)` (동일 객체의 동일 알람은 하나만 active).
- `alarm_id` = `f"{code}@{mo_instance}@{open_epoch}"` — open 시 생성, close/ack 가 동일 alarm_id 참조. 재발(clear 후 재open)은 새 alarm_id.
- 현재 `_alert_open` 의 키(`type` / `type:host:module`)가 이미 `(code, mo_instance)` 와 동형 → 이행 시 키를 `code@mo_instance` 로 정규화하고 open_epoch 만 부가하면 alarm_id 완성.

**(d) 재통지 — clear 없는 연속 open**

같은 활성키(`code@mo_instance`, 구 레코드는 `type`)로 **close 없이 open 이 다시 들어오면
같은 알람의 재통지**다. 새 occurrence 가 아니며 새 행·새 alarm_id 를 만들지 않는다 —
`(c)` 의 "재발 = clear 후 재open" 정의의 대우(對偶)다.

단, 재통지의 **severity 가 기존 활성 알람과 다르면 `action=change`** 로 발행한다
(32.111 notifyChangedAlarm — 같은 alarm_id 유지, `trend_indication: moreSevere|lessSevere`
+ `change_time` 동반). 단계 임계(§3.1 `thresholds`)의 승격/완화가 이 경로다. 판독측은
change 를 활성 행의 현재값 갱신으로 처리한다 (새 행 ❌, close 오인 ❌ — 미지 action 은 무시).

- **판독측**(콘솔 `AlertsPage.pairEvents`): 미해소 open 이 이미 있으면 기존 행을 갱신하고
  `occurrences` 를 증가시킨다(발생시각은 최초 유지, `last_open_ts` 로 최근 수신 시각 기록,
  화면에 `×N` 배지). 연속 open 마다 행을 새로 만들면 **뒤따르는 close 1건이 마지막 행만
  닫고 앞선 행은 영구 미해소로 남아 활성 알람에 유령이 생긴다.**
- **발행측**: 열림상태를 잃은 채(프로세스 교체·복원 실패) 재발행하지 않도록, 발행 주체는
  in-memory 상태가 비면 alert_log 에서 재도출한 뒤 판정한다
  (`drift_sweeper._reseed_if_empty`, `alert_log.compute_open_state`).
- **판정 불가의 종결**: 관측이 연속 실패해 open/close 어느 쪽도 판정할 수 없으면 알람이
  영원히 닫히지 않는다. 발행 주체는 연속 실패 임계(drift 스위퍼 3회)에서 "판정 불가" 사유로
  close 를 발행한다. 반대로 **관측 대상 자체가 0건이면 아무 판정도 하지 않는다** — 절체 직후
  standby 처럼 세상이 안 보이는 상태에서 열린 알람을 일괄 오종결하는 것을 막는다.

### 3.5 원칙 — 알람 type 은 "조건 클래스", 객체/리소스/임계는 그 밖 (★ 핵심)

알람 `type`/`code` 는 **조건(condition) 클래스**만 표현한다. 객체·리소스·임계·심각도는 type 에 넣지 않는다.

| type 에 ❌ 넣지 말 것 | 어디로 |
|---|---|
| 프로세스명 (csp/cmp/isp) | `source.mo_instance` |
| 리소스명 (disk/rtp/cpu/mem/network) | `source.mo_instance` (+ `mo_class`) |
| 임계 조건 값/방향 (high/80%) | rule 의 `threshold`/`unit`/`metric` |
| 심각도 (critical/warning) | rule 의 `perceived_severity` |

- ❌ 안티패턴: `csp_down`/`cmp_down`(프로세스), `rtp_high`/`disk_high`/`network_high`(리소스+조건) → 객체 수만큼 type/code 폭증.
- ✅ 표준(X.733): `type`/`code` = 조건 클래스. 동일 클래스의 다른 객체는 `(code, mo_instance)` 로 구분되는 별개 활성 알람. **새 객체/리소스(cpu/mem/trunk…) 추가 = type/code 신설 없이 rule 만 추가.**
- 다중 객체 점검은 같은 `code` 의 rule 을 객체별로(권장, mo_instance 명시) 또는 `targets:[...]` 다중 지정 → sweeper 가 객체별로 펼쳐 source 부여. 메시지는 `{mo}` 치환.
- **하위호환**: 옛 type(`csp_down`/`rtp_high`/…)은 read 시 `(class, mo_instance)` alias 표로 매핑.

### 3.6 알람(Alarm) vs 이벤트(Event/Notification) 분리

표준은 **장애 알람**과 **정상 통지**를 명확히 분리한다 — 둘을 같은 스트림에 섞지 않는다.

| | 알람(Alarm) | 이벤트/통지(Event/Notification) |
|---|---|---|
| 의미 | **비정상/장애** (조치 필요) | **정상 동작** 알림 (정보/감사) |
| 표준 | X.733 · 3GPP 32.111 | X.730(object)·**X.731(stateChange)**·X.740(audit) |
| 속성 | perceivedSeverity 有, 발생→**Cleared** 라이프사이클, ack | severity 無(또는 informational), 활성상태 無 |
| 활성목록/배너 | 올라감 | 안 올라감(이력/감사 로그만) |

- **정상 기동/실행 중인 프로세스는 알람이 아니다.** "프로세스 start/stop", "config 변경", "배포", "로그인" 등은 **이벤트**(stateChange/audit) → 이벤트 로그에만, 알람 카탈로그/배너엔 미포함.
- 프로세스 복구는 새 알람이 아니라 기존 `process_down` 의 **Cleared(close)** 통지.
- 단, **실행 중이어도** 다른 클래스 알람은 발생(threshold_crossed/connection_lost/처리오류 등) — "프로세스 생존 ≠ 정상".
- CIMS 현황: 알람 스트림은 fault-only(정상 통지를 알람화하지 않음). 정상 라이프사이클/감사는 별도 **이벤트 스트림**(`event_log` — `{ServiceLogDir}/events/`, `GET /events`, 모듈 자기보고 FM_EVENT 가 공급 — [alarm_self_reporting.md](alarm_self_reporting.md) §6)으로 흐른다. 콘솔 표시도 알람/이벤트 탭으로 스트림을 구분 — **모델 분리 유지**.

## 4. 전파 경로(구현 시 변경 지점)

1. **규칙 데이터**: `cims.json` + `_CORE_ALERT_RULES` 에 `code`/event_type/probable_cause/mo_class 추가, severity→perceived_severity. **type 을 클래스로 통합·분리** (module_down → `process_down`(agent, 전 모듈) / csp_down·cmp_down probe → `service_unresponsive`, target/scope 로 인스턴스 구분, mo_instance 명시 — §3.4(b)).
2. **sweeper** (`oam_app.py`): `_transition` 키를 `code@mo_instance` 로 정규화 + open 시 `alarm_id` 생성. `_emit` 가 code/표준필드/`source`(mo_class·mo_instance·detected_by) 동반 기록.
3. **alert_log** (`alert_log.py`): record/read 가 신규 필드 통과(free-form JSONL 호환). open↔close 상관을 `alarm_id` 기반으로(현 type 페어링 대체). summary 에 event_type/severity 분포 추가(선택).
4. **API** (`alerts.py`): `/alerts` 이벤트에 code/source/alarm_id 노출, `/rules` 에 code/event_type/probable_cause/severity(6), **신규 `GET /alerts/catalog`**(코드 카탈로그).
5. **UI**: AlertsPage 심각도 6색 배지 + code/eventType/cause/source 컬럼 + 상세에 **effect/action(runbook)** 표시 · 필터(심각도/유형/소스). AlertBannerWidget 심각도색. ServiceDescriptors 폼(ServiceForm)의 알람 규칙 입력에 code/severity(6)/event_type(5)/probable_cause/mo_class + effect/action 추가.
6. **타입**: `serviceDescriptors.AlertRule` + `alerts.AlertEvent/AlertRule` 확장(code/source/alarm_id).

## 5. 단계 계획

- **P0 — 분류 체계 + 코드/소스** (본 설계의 §3.1~3.4): `code`(카탈로그) · perceived_severity(6) · event_type(5) · probable_cause · source(mo_class/mo_instance/detected_by) · `alarm_id`(occurrence) · (선택) `effect`/`action`(runbook, §7.1). 규칙/이벤트/API(+/catalog)/UI/폼 전파. 하위호환.
- **P1 — 라이프사이클**: ackState/ackTime/ackUser + clearTime 명시 + `POST /alerts/ack {alarm_id}` API + UI 승인 버튼 + 코멘트(`POST /alerts/comment`, §3.2). 운영 감사추적.
- **P2 — 상관/연동**: correlatedNotifications(연관 알람, alarm_id 참조 — 첫 실사용처: L1 `process_down` ↔ L3 `service_unresponsive`, §3.4(b)), **SNMP/NMS northbound**(§7.3, RFC3877 alarmModel ↔ code 매핑 + 32.111 IRP / VES alarmCondition 매핑).

## 6. 하위호환·이행

- 이벤트 JSONL 은 free-form → 신규 필드는 누적만, 기존 reader 무영향.
- `severity` 읽는 곳은 `perceived_severity ?? severity` 로 폴백 → 점진 전환.
- 규칙은 `event_type`/`probable_cause` 누락 시 기본값(processingError/—) 부여하는 정규화 헬퍼로 흡수(데이터 미보강 descriptor 안전).
- 옛 per-process type(`csp_down`/`cmp_down`/`module_down`)은 read 시 `process_down` 클래스 + `source.mo_instance` 로 매핑하는 alias 표로 흡수 → 기존 이력/배너 무중단.
- probe 계열 규칙의 code 교체(csp/cmp 규칙 `CIMS-PRC-001`→`CIMS-PRC-004`)는 **코드 개정이
  아니다** — PRC-001 은 process_down 으로 존속하므로 `_CODE_REVISIONS` 를 쓰지 않고, 스윕이
  구 활성키(`CIMS-PRC-001@cims/csp`·`@cims/cmp/<ip>:<port>`)를 이행 종결(close)하고 지속
  조건은 `CIMS-PRC-004` 로 재발화한다 (CMP endpoint 재편 시 stale mo 강제 close 와 동일 관례).
- detected_by 인스턴스 접미(`agent:<host>`·`self:<node>`)는 구 레코드에만 남는다 — read·scope
  필터는 클래스 매칭(`self` = `self`·`self:*` 접두)으로 양쪽을 흡수하고, 신규 기록은 클래스만 쓴다.

## 7. 벤치마크 — 다른 통신 서버/규격의 알람 코드 정의

본 설계의 정합성 확인 + 보강점 도출을 위해 실제 통신 서버/규격의 알람 정의 방식을 조사.

| 항목 | RFC 3877 (Alarm MIB) | Project Clearwater (Metaswitch IMS) | 3GPP 32.111-2 | ONAP VES / ETSI NFV | **CIMS(본 설계)** |
|---|---|---|---|---|---|
| 코드(카탈로그) | `alarmModelIndex`(int) | numeric OID, 컴포넌트별 범위 | alarmType | `alarmCondition`(str) | `code` `CIMS-PRC-001` |
| 활성/occurrence | `alarmActiveTable` | active alarm | alarmId | eventId | `alarm_id` |
| severity | X.733 6 | X.733 6 | X.733 6 | 5(NORMAL 포함) | 6 |
| 분류 | eventType | 조건명(PROCESS_FAIL) | eventType | eventName | event_type 5 |
| 원인 | probableCause | cause | probableCause | — | probable_cause |
| **영향/조치** | — | **effect + action 보유** | — | vfStatus | (선택) effect/action ← §7.1 |
| 코드↔메시지 분리 | ✅ | ✅ | ✅ | ✅(alarmCondition↔specificProblem) | ✅(code/type↔message) |

관찰:
- **알람 코드(카탈로그) + 활성 알람(occurrence) 분리**, **severity X.733 6단계**, **조건 기반 명명**(객체는 source) — 4종 모두 공통. 본 설계와 정합.
- Clearwater 는 각 알람 정의에 **effect(영향) + recommended action(조치)** 까지 포함(운영 runbook). SIP 서버(Kamailio/OpenSIPS)는 SNMP/syslog/HEP 캡처 중심으로, 풍부한 알람 카탈로그는 IMS/NFV(위 4종)가 기준.

### 7.1 보강 — effect / recommended action (운영 runbook)

알람 정의에 `effect`(영향)·`action`(권장 조치)를 선택 필드로(§3.1). NOC 가 알람만 보고 즉시 대응 + AlertsPage/배너에 표시. 예:

| code / type | effect | recommended action |
|---|---|---|
| `CIMS-PRC-001` process_down | 해당 인스턴스 호처리/기능 중단 | 프로세스 재기동, 로그/코어 확인, HA 절체 점검 |
| `CIMS-PRC-004` service_unresponsive | 관리/제어 응답 불가 — hang·과부하 의심, 호처리 영향 가능 | 프로세스 상태·부하 확인(L1 process_down 동반 여부), 필요 시 재기동 |
| `CIMS-COM-001` connection_lost | 의존 자원(DB/트렁크) 사용 기능 저하 | 연결성/방화벽/원격 노드 상태 확인 |
| `CIMS-QOS-001` threshold_crossed | 용량 임계 근접 — 추가 부하 시 실패 위험 | 사용량 원인 파악, 자원 증설/정리 |

### 7.2 보강 — 사내 벤치마크: vIBCF/TrGW POD 대조

사내 타 시스템 vIBCF/TrGW 의 알람 정의서(POD)를 md 로 변환해 보존한다 —
[vibcf_pod_alarms.md](vibcf_pod_alarms.md) (알람 32종 + Fault/상태 메시지 79종, 코드별
조치사항 포함). CIMS 모델과의 대조와 채용 포인트:

| 관점 | vIBCF/TrGW | CIMS | 판단 |
|---|---|---|---|
| 스트림 분리 | **A**(알람)/**F**(Fault — 호처리 이상 로그, FAULT/WARNING/INFO)/**S**(상태·통계 알림) 3분리 | 알람 vs 이벤트(stateChange/audit) 2분리, 요청 단위 이상은 로그/PM 소관(§3.6) | 동형. 단 vIBCF 는 요청 단위 이상(F 계열 — SIP syntax error, dialog 초과 등)도 **코드화해 운영자 스트림으로 노출**한다 — CIMS 가 "로그 소관"으로 제외한 영역의 운영 노출 필요성 검토 여지(이벤트 kind 확장 또는 PM 위임 유지) |
| 조건 클래스 | 타입 인덱스(CommunicationFail/DatabaseError/QueueFull…) — code 와 분리 | type/code 1:1 (§3.4) | 동형. 단 vIBCF 는 리소스별 분리(CPUOverflow/DiskOverload/MemoryOverflow) — CIMS §3.5 가 안티패턴으로 정리한 축(threshold_crossed 하나 + mo)이 맞다 |
| severity | CLEARED+MINOR/MAJOR/CRITICAL "가변", 단계 임계 70/80/90 을 설명 템플릿에 명시(`CPU load is A% (CRI:B,MAJ:C,MIN:D)`) | thresholds 단계(80/90/95) + action=change + threshold_info(§3.2) | 동형 — 관측값·임계 동반 표기는 threshold_info 로 이미 수용 |
| 발생 위치 | `서버명/프로세스명`·`서버명/DISK/파티션명` 경로 | mo_instance DN-유사 경로(§3.4(b)) | 동형 |
| **조치사항** | **전 알람 코드에 다단계 조치 절차**(확인 경로·설정 파일·해제 메뉴까지) | effect/recommended_action 선택 필드(§7.1) — 현행 카탈로그는 1줄 수준 | **채용** — 카탈로그(fm_catalog·rule)의 recommended_action 을 절차 수준으로 강화하고 콘솔 상세에 노출. POD 문서 산출물(코드별 1페이지) 자체가 운영 이관 규격이라는 점도 참고(카탈로그 CSV → POD 생성 후보) |
| 감시 항목 | NTP 3종(Delay/Offset/Status), 성공률(SuccessFailError), CPS/세션/채널 과부하 세분, Manual Block 상태 알람, 절체 알림(S3001~5) | NTP 없음(검사 코드 부재), 성공률 없음, overload 1클래스, 수동 개입은 node_maintenance 이벤트+redundancy_degraded reason, ha_switchover 이벤트 후보 | **채용 2건**: ①NTP 시각 동기 이상 — vIBCF 가 3클래스로 운영할 만큼 실운영 가치 검증됨 → AGENT 후보(선행 필요)로 카탈로그 편입(CIMS 는 threshold_crossed 로 통합) ②호 성공률/응답률 임계(SuccessFailError 상당) — §3.6 원칙상 "율 임계 전이"로 허용 가능하나 CSP 통계 카운터 관측 지점 확인 선행 — 검토 후보로만 기록 |

> vIBCF POD 원문의 A0000 "메시지 설명"이 CPU 문구로 오기재된 것 등 원문 결함은 변환본에
> 그대로 보존했다(원문 보존 원칙).

### 7.3 보강 — SNMP / NMS northbound 매핑 (P2)

상위 NMS 연동 시 `code` 를 표준 식별자로 매핑:
- **RFC 3877 alarmModel**: `code` → `alarmModelIndex`(정수, 도메인별 범위 예약) + perceived_severity → `alarmModelState`. 활성 알람 → `alarmActiveTable`(alarm_id).
- **3GPP 32.111-2 IRP / VES**: code → alarmType/`alarmCondition`, source.mo_instance → managedObjectInstance, message → specificProblem, perceived_severity → eventSeverity.
- 매핑은 별도 테이블(code↔int OID)로 관리 — CIMS 내부 모델은 문자열 code 유지, northbound 게이트웨이에서 변환.

## 관련
- [alarm_self_reporting.md](alarm_self_reporting.md) — 모듈 자기보고(FM push) 경로 — 본 모델 위의 발생 경로 확장 + 이벤트 스트림
- `console_platform.md` (Service Descriptor: modules/alert_rules/data_sources) · `features/monitoring.md`
- 3GPP TS 32.111-2 (Alarm IRP) · ITU-T X.733 (Alarm reporting) · IETF RFC 3877 (Alarm MIB) · ONAP VES (fault) / ETSI NFV · Project Clearwater (IMS 알람 사례)
