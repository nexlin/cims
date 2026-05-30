# 알람(이벤트) 표준화 설계 — X.733 / 3GPP TS 32.111

CIMS 알람을 임의 스키마(`critical`/`warning` 2단계)에서 **IMS 망관리 표준**에 맞춰 체계화하기 위한 설계.
구현은 단계(P0/P1)로 분리하며, 본 문서는 표준 매핑·제안 모델·이행 계획을 확정한다(코드 변경 전 SoT).

## 1. 참조 표준

- **3GPP TS 32.111-2** — *Telecommunication management; Fault Management; Part 2: Alarm Integration
  Reference Point (IRP): Information Service (IS).* IMS 포함 3GPP 망요소(NE/EM)가 관리객체의 알람을
  Manager(NMS)로 보고하는 인터페이스. CIMS 가 향후 상위 NMS 와 연동할 때의 정합 기준.
- **ITU-T X.733** — *Systems Management: Alarm reporting function.* 위 IRP 가 기반하는 알람(장애) 속성 모델.
- **ITU-T X.730 / X.731** — *Object Management / State Management.* 정상 동작 통지(객체 생성·삭제, **stateChangeNotification**) — 알람과 구분되는 **이벤트** 규격(§3.6).
- **IETF RFC 3877** — *Alarm Management Information Base (MIB).* X.733 의 SNMP 표준화 — 알람 정의 카탈로그(`alarmModelTable`) + 활성 알람(`alarmActiveTable`) 분리. 향후 SNMP/NMS northbound 연동 기준(§7.2).
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
- **평가/발생** (`oam/src/oam_app.py` `_sweep_alerts`→`_eval_alert_rule`/`_eval_agent_rule`→`_transition`→`_emit`).
- **이벤트 레코드** (`csc/src/services/alert_log.py`, `{ServiceLogDir}/alerts/YYYY/MM/DD.jsonl`):
  `{ ts, type, severity, action(open|close), message }`
- **API** (`oam/src/handlers/alerts.py`): `GET /alerts`, `/types`, `/summary`, `/rules`.
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
  "type": "process_down",              // 알람 **클래스** 슬러그 (프로세스명 박지 않음 — §3.5)
  "code": "CIMS-PRC-001",              // 알람 클래스 코드(카탈로그) — §3.4
  "perceived_severity": "critical",    // critical|major|minor|warning|indeterminate  (기존 severity 대체)
  "event_type": "processingError",     // communications|qualityOfService|processingError|equipment|environmental
  "probable_cause": "softwareError",   // X.733 Annex 코드
  "mo_class": "software",              // managedObject class: software|service|equipment|host|network
  "check": "process_down", "target": "csp",   // 무엇을 점검할지(탐지) — 어느 프로세스는 여기서, 알람 type 엔 안 박음
  "mo_instance": "cims/csp",           // (선택) 소스 instance 명시 — 없으면 target/host 로 런타임 합성
  "threshold": null, "unit": null,
  "metric": "프로세스 가용성",
  "msg_open": "{mo} 프로세스 응답 없음",   // → specificProblem ({mo}=mo_instance)
  "msg_close": "{mo} 정상화",
  "effect": "해당 서비스 호처리 중단",            // (선택) 영향 — 운영 runbook (Clearwater 벤치마크 §7.1)
  "recommended_action": "프로세스 재기동 / 로그 확인",  // (선택) 권장 조치 (이벤트의 action=open/close 와 구분)
  "scope": "service"                   // service|agent (유지)
}
```
- **type/code 는 알람 클래스** (process_down). 어느 프로세스인지는 `source.mo_instance`(§3.4/§3.5). `csp_down`/`cmp_down` 처럼 프로세스명을 type 에 박지 않음.
- `perceived_severity` 가 기존 `severity` 를 대체. **하위호환**: `severity` 만 있으면 perceived_severity 로 승격(critical/warning 표준 값 유효). 신규 major/minor/indeterminate 가능.
- managedObject **instance** 는 `mo_instance` 명시 또는 런타임 합성(§3.4): service 규칙 = `cims/<target>`, agent 규칙 = `<host>/<module|disk|rtp>`.

### 3.2 이벤트 레코드(alert_log) 확장

```jsonc
{
  "ts": "2026-05-30T09:31:05",         // eventTime
  "alarm_id": "CIMS-PRC-010@Media-Server-01/csp@1748590265",  // 발생 인스턴스 고유 id (occurrence) — §3.4
  "type": "module_down",               // 정의 슬러그
  "code": "CIMS-PRC-010",              // 정의 코드
  "perceived_severity": "critical",    // open=규칙 severity, close=cleared
  "event_type": "processingError",
  "probable_cause": "softwareError",
  "source": {                          // 발생 소스 (§3.4)
    "mo_class": "software",            //   managedObjectClass
    "mo_instance": "Media-Server-01/csp",  //   managedObjectInstance (DN-유사)
    "detected_by": "oam"              //   탐지 주체 (oam | agent:<host>)
  },
  "action": "open",                    // open|close (close = Cleared)
  "message": "Media-Server-01 모듈 csp 프로세스 응답 없음",  // specificProblem
  "ack_state": "unacknowledged"        // P1: acknowledged 시 ack_time/ack_user 추가
}
```
- 기존 필드 보존(`ts/type/action/message`) → 이력/통계/배너 무중단. `severity`→`perceived_severity`, `managed_object`(평면 문자열)→`source.mo_instance` 로 점진 치환(읽기 시 둘 다 허용).
- `alarm_id` 로 open↔close 상관 및 ack 대상 지정. 동일 활성알람 식별 = `(code, source.mo_instance)`.

### 3.3 현재 CIMS 알람 → 표준 매핑 (확정)

기존 6개(csp_down/cmp_down/module_down/db_down/rtp_high/disk_high) → **조건 클래스 3개**로 정규화.
어느 프로세스/리소스/호스트인지는 `source.mo_instance`, 심각도/임계/원인은 **rule 속성**(클래스 정체성 아님).

| code | type(클래스) | eventType | probableCause (rule별) | mo_class | mo_instance 예시 | severity(rule별) | detected_by |
|---|---|---|---|---|---|---|---|
| `CIMS-PRC-001` | `process_down` | processingError | softwareError | software | `cims/csp` · `cims/cmp` · `<host>/<module>` | critical | oam / agent:<host> |
| `CIMS-COM-001` | `connection_lost` | communications | communicationsSubsystemFailure / underlyingResourceUnavailable | service | `cims/db` (향후 `cims/trunk/<id>`·peer) | critical | oam |
| `CIMS-QOS-001` | `threshold_crossed` | qualityOfService | thresholdCrossed / storageCapacityProblem / resourceAtOrNearingCapacity | service·host | `cims/rtp_ports` · `<host>/disk` (향후 `<host>/cpu`·`mem`·`<iface>`) | warning(minor/major 승격) | oam / agent:<host> |

> **통합 원리**(§3.5): 같은 *조건*은 한 클래스. `csp_down`+`cmp_down`+`module_down`→`process_down` / `rtp_high`+`disk_high`(+cpu/mem/network)→`threshold_crossed` / `db_down`→`connection_lost`. 어느 리소스인지는 **source**, 임계값·단위·probableCause·severity 는 **rule** 이 보유 → 새 리소스(cpu/mem/network) 추가 시 **type/code 신설 없이 rule 만 추가**.
> 같은 클래스라도 rule 별로 probableCause/severity 가 다를 수 있음(disk→storageCapacityProblem/warning, rtp→resourceAtOrNearingCapacity/warning, 단계별 minor→major).
> 중복 발화 방지(agent module 점검에서 중앙 점검 대상 csp/cmp 제외)는 구현에 유지.

### 3.4 알람 코드 체계 · 발생 소스 · occurrence id

**(a) 알람 코드 (클래스 카탈로그 식별자)** — `type`(클래스) 슬러그와 1:1, 안정적·불변. 운영 alarm dictionary / 상위 NMS 연동 키.
포맷 `**<SERVICE>-<DOMAIN>-<SEQ>**`:
- `SERVICE` = 서비스 pack 네임스페이스 (CIMS, 타 서비스는 자기 prefix → 코드 충돌 없음).
- `DOMAIN` = eventType 약어: **PRC**(processingError) · **COM**(communications) · **QOS**(qualityOfService) · **EQP**(equipment) · **ENV**(environmental).
- `SEQ` = 3자리, **조건 클래스당 1개**(객체 인스턴스마다 부여 ❌ — 인스턴스는 source). 예: PRC-001 process_down, COM-001 connection_lost, QOS-001 threshold_crossed. 같은 도메인 내 새 *조건* 클래스가 생기면 002,003…
- 코드 카탈로그 = descriptor 의 alert_rules 클래스 집합(코어 + 서비스). `GET /alerts/catalog`(신규, P0)로 노출.

**(b) 발생 소스 (managedObject + detected-by)** — 알람이 "무엇에서/어디서" 났는지 표준화.
- `mo_class`: software | service | equipment | host | network (managedObjectClass).
- `mo_instance`: DN-유사 경로. service 규칙 = `cims/<target>` · agent 규칙 = `<host>/<module|disk|rtp>`. (계층: `<service|host>/<component>[/<instance>]`)
- `detected_by`: 탐지 주체 — `oam`(중앙 stats poll) 또는 `agent:<host>`(per-agent metric). 고장 객체(mo_instance)와 탐지 주체가 다를 수 있음(예: db_down 은 oam 탐지, 객체는 cims/db).

**(c) alarm_id (발생 인스턴스 id, occurrence / X.733 notificationIdentifier)**
- 활성 알람 식별 = `(code, mo_instance)` (동일 객체의 동일 알람은 하나만 active).
- `alarm_id` = `f"{code}@{mo_instance}@{open_epoch}"` — open 시 생성, close/ack 가 동일 alarm_id 참조. 재발(clear 후 재open)은 새 alarm_id.
- 현재 `_alert_open` 의 키(`type` / `type:host:module`)가 이미 `(code, mo_instance)` 와 동형 → 이행 시 키를 `code@mo_instance` 로 정규화하고 open_epoch 만 부가하면 alarm_id 완성.

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
- CIMS 현황: 알람 스트림은 fault-only(정상 통지를 알람화하지 않음) — 원칙 준수 중. 정상 라이프사이클/감사는 별도 **이벤트 스트림**(예 `mcptt.audit_config_change`)으로 둔다. 알람·이벤트 통합 뷰는 표시단에서 합치되 **모델은 분리**.

## 4. 전파 경로(구현 시 변경 지점)

1. **규칙 데이터**: `cims.json` + `_CORE_ALERT_RULES` 에 `code`/event_type/probable_cause/mo_class 추가, severity→perceived_severity. **type 을 클래스로 통합** (csp_down/cmp_down/module_down → `process_down`, target/scope 로 인스턴스 구분, mo_instance 명시).
2. **sweeper** (`oam_app.py`): `_transition` 키를 `code@mo_instance` 로 정규화 + open 시 `alarm_id` 생성. `_emit` 가 code/표준필드/`source`(mo_class·mo_instance·detected_by) 동반 기록.
3. **alert_log** (`alert_log.py`): record/read 가 신규 필드 통과(free-form JSONL 호환). open↔close 상관을 `alarm_id` 기반으로(현 type 페어링 대체). summary 에 event_type/severity 분포 추가(선택).
4. **API** (`alerts.py`): `/alerts` 이벤트에 code/source/alarm_id 노출, `/rules` 에 code/event_type/probable_cause/severity(6), **신규 `GET /alerts/catalog`**(코드 카탈로그).
5. **UI**: AlertsPage 심각도 6색 배지 + code/eventType/cause/source 컬럼 + 상세에 **effect/action(runbook)** 표시 · 필터(심각도/유형/소스). AlertBannerWidget 심각도색. ServiceDescriptors 폼(ServiceForm)의 알람 규칙 입력에 code/severity(6)/event_type(5)/probable_cause/mo_class + effect/action 추가.
6. **타입**: `serviceDescriptors.AlertRule` + `alerts.AlertEvent/AlertRule` 확장(code/source/alarm_id).

## 5. 단계 계획

- **P0 — 분류 체계 + 코드/소스** (본 설계의 §3.1~3.4): `code`(카탈로그) · perceived_severity(6) · event_type(5) · probable_cause · source(mo_class/mo_instance/detected_by) · `alarm_id`(occurrence) · (선택) `effect`/`action`(runbook, §7.1). 규칙/이벤트/API(+/catalog)/UI/폼 전파. 하위호환.
- **P1 — 라이프사이클**: ackState/ackTime/ackUser + clearTime 명시 + `POST /alerts/ack {alarm_id}` API + UI 승인 버튼. 운영 감사추적.
- **P2 — 상관/연동**: correlatedNotifications(연관 알람, alarm_id 참조), **SNMP/NMS northbound**(§7.2, RFC3877 alarmModel ↔ code 매핑 + 32.111 IRP / VES alarmCondition 매핑).

## 6. 하위호환·이행

- 이벤트 JSONL 은 free-form → 신규 필드는 누적만, 기존 reader 무영향.
- `severity` 읽는 곳은 `perceived_severity ?? severity` 로 폴백 → 점진 전환.
- 규칙은 `event_type`/`probable_cause` 누락 시 기본값(processingError/—) 부여하는 정규화 헬퍼로 흡수(데이터 미보강 descriptor 안전).
- 옛 per-process type(`csp_down`/`cmp_down`/`module_down`)은 read 시 `process_down` 클래스 + `source.mo_instance` 로 매핑하는 alias 표로 흡수 → 기존 이력/배너 무중단.

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
| `CIMS-COM-001` connection_lost | 의존 자원(DB/트렁크) 사용 기능 저하 | 연결성/방화벽/원격 노드 상태 확인 |
| `CIMS-QOS-001` threshold_crossed | 용량 임계 근접 — 추가 부하 시 실패 위험 | 사용량 원인 파악, 자원 증설/정리 |

### 7.2 보강 — SNMP / NMS northbound 매핑 (P2)

상위 NMS 연동 시 `code` 를 표준 식별자로 매핑:
- **RFC 3877 alarmModel**: `code` → `alarmModelIndex`(정수, 도메인별 범위 예약) + perceived_severity → `alarmModelState`. 활성 알람 → `alarmActiveTable`(alarm_id).
- **3GPP 32.111-2 IRP / VES**: code → alarmType/`alarmCondition`, source.mo_instance → managedObjectInstance, message → specificProblem, perceived_severity → eventSeverity.
- 매핑은 별도 테이블(code↔int OID)로 관리 — CIMS 내부 모델은 문자열 code 유지, northbound 게이트웨이에서 변환.

## 관련
- `console_platform.md` (Service Descriptor: modules/alert_rules/data_sources) · `features/monitoring.md`
- 3GPP TS 32.111-2 (Alarm IRP) · ITU-T X.733 (Alarm reporting) · IETF RFC 3877 (Alarm MIB) · ONAP VES (fault) / ETSI NFV · Project Clearwater (IMS 알람 사례)
