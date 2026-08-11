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
  (`restore_open_state` — 파티션 판정은 detected_by, §3.4(b)).
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
  "code": "A-PRC-004",              // 알람 정의 코드(카탈로그) — §3.4
  "perceived_severity": "major",       // critical|major|minor|warning|indeterminate  (기존 severity 대체)
  "event_type": "processingError",     // communications|qualityOfService|processingError|equipment|environmental
  "probable_cause": "responseTimeExcessive",   // X.733 Annex 코드
  "mo_class": "service",               // managedObject class: software|service|equipment|host|network
  "check": "service_unresponsive", "target": "csp",   // 무엇을 점검할지(탐지) — 어느 프로세스는 여기서, 알람 type 엔 안 박음
  "mo_instance": "SIG_SVR_01/csp",     // (선택) 소스 instance 명시 — 없으면 target/host 로 런타임 합성 (§3.4(b) 소유 주체 루트)
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
- **type 은 조건 클래스, code 는 정의** (service_unresponsive / A-PRC-004). 어느 프로세스인지는 `source.mo_instance`(§3.4/§3.5). `csp_down`/`cmp_down` 처럼 프로세스명을 type 에 박지 않음.
- `perceived_severity` 가 기존 `severity` 를 대체. **하위호환**: `severity` 만 있으면 perceived_severity 로 승격(critical/warning 표준 값 유효). 신규 major/minor/indeterminate 가능.
- managedObject **instance** 는 `mo_instance` 명시 또는 런타임 합성(§3.4(b) — 루트는 소유 주체 서버명/그룹명): agent 규칙 = `<서버명>/<module|disk|…>`, service 규칙 = 관측 신원 기준 — 노드 주소 관측 = `<서버명>/<모듈>`(CMP 다중 미디어 노드(AA)는 endpoint 소유 서버로 해석해 개별 발화), VIP 관측 = `<그룹명>/<모듈>`. 주소→서버명/그룹명 해석은 인벤토리가 정본.

### 3.2 이벤트 레코드(alert_log) 확장

```jsonc
{
  "ts": "2026-05-30T09:31:05",         // eventTime
  "alarm_id": "A-PRC-001@Media-Server-01/csp@1748590265",  // 발생 인스턴스 고유 id (occurrence) — §3.4
  "type": "process_down",              // 정의 슬러그
  "code": "A-PRC-001",              // 정의 코드
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
`code` 는 정의 코드(§3.4(a)). 한 클래스의 여러 rule 이 서로 다른 정의인 경우는 각자의
코드를 갖는다 — threshold_crossed 의 disk=`A-QOS-001`·ha_flap=`A-QOS-023`(공통)·
RTP 사용률=`A-QOS-024`(MRF, 노드별 발화).

| code | type(클래스) | eventType | probableCause (rule별) | mo_class | mo_instance 예시 | severity(rule별) | detected_by |
|---|---|---|---|---|---|---|---|
| `A-PRC-001` | `process_down` | processingError | softwareError | software | `<host>/<module>` (전 모듈 — csp/cmp 포함) | critical | agent |
| `A-PRC-004` | `service_unresponsive` | processingError | responseTimeExcessive | service | `SIG_SVR_01/csp`(노드 주소 관측) · `<그룹명>/csp`(VIP 관측) — CMP 는 endpoint 소유 서버별 `MED_SVR_01/cmp` | major | oam-svc / oam |
| `A-COM-001` | `connection_lost` | communications | communicationsSubsystemFailure / underlyingResourceUnavailable | service | `<관리그룹>/db`(OAM 관측 — 그룹 공통 신원) · 모듈 관점은 `<서버명>/<모듈>/db`(자기보고 §4) | critical | oam-svc / oam / self |
| `A-QOS-001` | `threshold_crossed` | qualityOfService | storageCapacityProblem | host | `<서버명>/disk` (호스트 자원 — cpu/mem/load 확장 시 rule 만 추가) | 단계 임계(minor 80 / major 90 / critical 95, 승격=action change) | agent |
| `A-QOS-023` | `threshold_crossed` | qualityOfService | thresholdCrossed | service | `<서버명>/ha/<svc>` (check=ha_flap — keepalived 전이 빈도 임계) | warning | agent |
| `A-QOS-024` | `threshold_crossed` | qualityOfService | resourceAtOrNearingCapacity | service | `MED_SVR_01/cmp/rtp_ports` (노드별 발화 — check=rtp_pct_gte) | 단계 임계(minor 80 / major 90 / critical 95) | oam-svc / oam |
| `A-PRC-003` | `config_out_of_sync` | processingError | configurationOrCustomizationError | software | `<서버명>/<모듈>/config` · `<그룹명>/config/<collection>`(HA fan-out) | warning | agent / oam |

`A-PRC-003` 은 배포기록 실체화본(config_template default + overlay)의 canonical hash 와
agent 가 보고하는 노드 실파일(`metric.cfg_hashes`) hash 의 불일치 = 설정 드리프트를 노출한다.
`ha_flap`(A-QOS-023) 은 agent 가 cims-notify 로그에서 집계한 `metric.ha_transitions`
(최근 10분 keepalived 전이 수, 기본 임계 6회)로 VIP flap 을 노출한다 — 전이 개별 건은
§3.6 대로 이벤트(로그)일 뿐이며, 알람은 빈도 임계 초과라는 *조건*이다.

> **통합 원리**(§3.5): 같은 *조건*은 한 클래스. `module_down`→`process_down` / `rtp_high`+`disk_high`(+cpu/mem/network)→`threshold_crossed` / `db_down`→`connection_lost`. 구 `csp_down`/`cmp_down` 은 "프로세스 생존"과 "관리 응답성" 두 *조건*의 혼합이었다 — 생존은 `process_down`(agent 관측), 응답성은 `service_unresponsive`(OAM probe)로 분리한다(§3.4(b) 감지 3계층). 어느 리소스인지는 **source**, 임계값·단위·probableCause·severity 는 **rule** 이 보유 → 새 리소스(cpu/mem/network) 추가 시 **type/code 신설 없이 rule 만 추가**.
> 같은 클래스라도 rule 별로 probableCause/severity 가 다를 수 있음(disk→storageCapacityProblem/warning, rtp→resourceAtOrNearingCapacity/warning, 단계별 minor→major).
> `process_down` 은 전 모듈을 agent 관측으로 판정한다 — agent module 점검에서 csp/cmp 를 제외하던 규칙(proc_down_targets)은 두지 않는다. 같은 모듈에 `process_down`(L1)과 `service_unresponsive`(L3)가 함께 열리는 것은 중복이 아니라 별개 조건이며, correlatedNotifications(P2)로 상관을 명시한다.

### 3.4 알람 코드 체계 · 발생 소스 · occurrence id

**(a) 알람 코드 — 정의 코드 단일 층위 (flat)**

코드는 **정의 코드** 한 층위만 갖는다 — 알람/이벤트 **정의**(카탈로그 행 — "이 알람이
무엇인가")당 1개. 운영 사전(dictionary)·코드별 조치서(POD)·상위 NMS 연동(RFC 3877
alarmModelIndex 의 대응물)의 키다. 조건 **클래스**(분류 축)는 코드를 갖지 않는다 —
`type` 슬러그가 클래스의 식별자다(알람 20종 = 모듈 카탈로그 §2.3, 이벤트 9종 = §3.6).
vIBCF 의 장애코드(A00XX — flat, 정의당 1개) vs 타입 인덱스(분류 — 코드 아님) 분리와
동형(§7.2).

- **정의 코드** `<STREAM>-<DOMAIN>-<NNN>` — 예: `A-PRC-001` process_down, `A-COM-001` connection_lost(DB), `E-STC-001` process_started.
  - `STREAM` = **A**(알람) | **E**(이벤트) — 스트림 소속이 코드에서 즉독된다(vIBCF 의 A/F/S 스트림 문자 관례와 동형).
  - **시스템/서비스 네임스페이스는 코드에 넣지 않는다** — 상위 NMS 통합 시 시스템(CIMS vs 타 시스템) 구분은 northbound 매핑·연동 계약의 소관(§7.3)이고, CIMS 내부에서는 스트림 레코드의 서비스 컨텍스트가 이미 보유한다(mo_instance 도 같은 원칙으로 시스템 접두 없이 서버명/그룹명 루트 — §3.4(b)). 타 서비스 pack 의 카탈로그는 descriptor 소속으로 구분되며, 교차 노출이 실제로 필요해지면 그때 pack 접두를 도입한다(선제 도입 ❌).
  - `DOMAIN` = 알람은 eventType 약어: **PRC**(processingError) · **COM**(communications) · **QOS**(qualityOfService) · **EQP**(equipment) · **ENV**(environmental). X.736 보안 알람 클래스(security_violation — 기능 카탈로그 제안) 채택 시 **SEC** 추가(eventType 은 X.736 계열). **이벤트**의 DOMAIN 은 kind 약어 **STC**(stateChange)/**AUD**(audit).
  - `NNN` = 3자리 **무의미 일련**(스트림+도메인 내). 결번은 재사용하지 않는다.
- **분류(클래스)를 코드에 인코딩하지 않는다** — 정의 코드에 클래스 번호를 내장하는 dotted
  형식(`<클래스코드>.<NN>`)은 정의의 클래스 재배정(분류 정정 — 예: NTP 를 다른 클래스로)이
  곧 코드 개정(불변 규칙 위반·NMS 사전 키 단절)이 되는 자기모순을 낳는다. flat 이면
  `type` 은 카탈로그·rule 의 속성이라 코드 불변인 채 자유롭게 정정할 수 있다.
- 정의 목록의 정본 = [alarm_function_catalog.csv](alarm_function_catalog.csv). **런타임 인스턴스(endpoint·peer·풀·노드)마다는 부여하지 않는다** — 인스턴스는 `source.mo_instance` 소관(§3.5 통합 원리 유지: 새 리소스·새 대국은 코드 신설 없이 mo/rule 만 추가).
- 활성 알람 식별키 = (정의 코드, mo_instance).
- **구 클래스 코드 번호 승계** — 구현 기성 클래스 7종의 대표 정의는 구 클래스 코드의 번호를
  그대로 받았다(이행 혼란 최소화 — 번호 무의미 원칙과 무충돌):

  | 구 wire 클래스 코드 | 승계 정의 코드 | type / 대표 정의 |
  |---|---|---|
  | `CIMS-PRC-001` | `A-PRC-001` | process_down |
  | `CIMS-PRC-002` | `A-PRC-002` | storage_failure — FD/파일 스토어 저장 실패 (CMDP 구현 발화) |
  | `CIMS-PRC-003` | `A-PRC-003` | config_out_of_sync — 배포 정본 드리프트 |
  | `CIMS-PRC-004` | `A-PRC-004` | service_unresponsive |
  | `CIMS-COM-001` | `A-COM-001` | connection_lost — 서비스 DB 두절 (CSP·CSC 구현 발화) |
  | `CIMS-QOS-001` | `A-QOS-001` | threshold_crossed — 호스트 자원 단계 임계 (disk 구현 발화) |
  | `CIMS-QOS-002` | `A-QOS-002` | resource_exhausted — 미디어 자원 풀 완전 고갈 (CMP 구현 발화) |

  승계 기준은 **구 코드로 실제 발화 중인 정의**다 — 이행 시 열린 알람의 코드 연속성이 목적.
  구 클래스 코드 하나가 여러 정의로 갈라지는 경우(threshold_crossed 의 rtp/ha_flap rule,
  storage_failure 로 넓힐 CDR·녹취 등)의 잔여 발화는 이행 시 rule/mo 별로 각자의 정의
  코드에 매핑한다.
- **wire/저장/rule/fm_catalog 는 flat 정의 코드를 사용한다.** 구 포맷 `CIMS-<DOMAIN>-<SEQ>`(서비스 접두 + 클래스 단위 코드)는 `_CODE_REVISIONS` alias 로 read/수신 시 흡수되고, 구 코드로 열려 있던 알람은 스윕이 이행 종결 후 현행 코드로 재발행한다(아래 "코드는 불변" 절차). 구 `CIMS-QOS-001` 이 갈라진 정의는 rule 의 check 가 배정한다 — disk=`A-QOS-001`·ha_flap=`A-QOS-023`·rtp 사용률=`A-QOS-024`.
- 코드 카탈로그 = descriptor 의 alert_rules 클래스 집합(코어 + 서비스) + 모듈 자기보고 등록분. `GET /alerts/catalog` 로 노출.

**코드 문법 규칙 (신설 시 준수)**:
- `NNN` 은 **무의미 일련번호** — 정의의 정체성은 카탈로그 행뿐이고, 클래스의 정체성은 `type` 슬러그뿐이다. 번호에 의미(심각도·우선순위·리소스 종류·클래스)를 싣지 않는다.
- STREAM·DOMAIN 외의 분류는 코드에 **인코딩하지 않는다** — 클래스(type)·probableCause 는
  카탈로그/rule 속성(같은 클래스 안에서 rule 별로 다름 — disk→storageCapacityProblem,
  rtp→resourceAtOrNearingCapacity), specificProblem 은 발생 건의 message(자유 서술)라 열거
  불가. 코드에 박으면 속성 정정 = 코드 개정(NMS 사전 키 단절)이 되고, 코드가 분류 수만큼
  쪼개진다(§3.5 안티패턴의 원인 축 재현). 스트림(알람/이벤트)과 eventType/kind 만은 정의
  정체성의 일부(재판정 없음)라 코드에 넣어도 안전.
- **코드는 불변** — northbound 연동 전에 한해 개정 가능하며, 개정은
  `service_registry._CODE_REVISIONS`(옛→현행)에 기록한다. 옛 code 규칙은 read 시 alias,
  옛 code 로 열려 있던 활성 알람은 스윕이 이행 종결(close)하고 지속 조건은 현행 code 로
  재발행한다(`alarm_sweeper.close_legacy_code` — 활성키(mo)까지 함께 바뀐 이행은
  `close_migrated_keys` 가 원 akey 로 종결). 개정 이력: ①`CIMS-CFG-001`→PRC 정정
  (CFG 는 DOMAIN=eventType 약어 규칙 위반) ②구 클래스 코드 `CIMS-*` 전체 → flat
  정의 코드 `A-*` (위 번호 승계 표).

**(b) 발생 소스 (managedObject + detected-by)** — 알람이 "무엇에서/어디서" 났는지 표준화.
- `mo_class`: software | service | equipment | host | network (managedObjectClass).
- `mo_instance`: DN-유사 경로. **루트 = 고장 객체의 소유 주체(서버명 또는 HA 그룹명)** —
  시스템 네임스페이스(`cims/`)는 쓰지 않는다(코드에 시스템 접두를 넣지 않는 §3.4(a)와 같은
  원칙 — 시스템 구분은 northbound 매핑·레코드 컨텍스트 소관). vIBCF POD 의
  `서버명/프로세스명` 경로 관례와 동형(§7.2).
  | 객체 소유 | 형식 | 예 |
  |---|---|---|
  | 노드(호스트) 자원 | `<서버명>/<자원>` | `SIG_SVR_01/disk` · `SIG_SVR_01/ntp` · `SIG_SVR_01/ha/<svc>` |
  | 노드 위 모듈(+내부 객체·외부 의존 연결) | `<서버명>/<모듈>[/<component>]` | `SIG_SVR_01/csp` · `SIG_SVR_01/csp/db` · `MED_SVR_01/cmp/rtp` · `SIG_SVR_01/csp/peer/<대국>` |
  | **그룹 소유 객체** (A/S 이중화) | `<그룹명>/<객체>` | `csp-g1/vip`(무보유/이중보유) · `csp-g1/config/<collection>`(fan-out drift) · `csp-g1/csp`(VIP 관측 L3 응답성) |
  판별식: **절체와 무관하게 지속되는 조건 = 그룹 소유**(그룹 루트), 특정 노드의
  사실(keepalived 미설치·절체 래치·ha_flap)은 HA 관련이라도 서버 루트. L3 는 관측 주소의
  신원을 따른다 — VIP 관측 = 그룹 루트, 노드 주소 관측 = 서버 루트(비 HA 배포는 자연히
  서버 루트). 서버명·그룹명 어휘는 인벤토리(자동 배포 YAML)가 정본이며 두 네임스페이스는
  겹치지 않아야 한다. 자기보고(L2)는 `<서버명>/<모듈>[/<component>]`(서버명 = envelope
  `hdr.node`). **발생 노드/호스트는 mo_instance 가 유일 보유자다** — detected_by 에
  중복하지 않는다.
- `detected_by`: 탐지 주체 **클래스** — `agent` | `self` | `oam-svc`(분리 배포) | `oam`(단일 프로세스 `--role all` 대행, HA fan-out drift 등 OAM 자체 판정). 인스턴스 접미(`agent:<host>`·`self:<node>`)는 두지 않는다 — 노드는 mo_instance 와 (자기보고는 wire 의) envelope `hdr.node` 가 보유하고, detected_by 는 ① 발행 주체별 open-state **소유 파티션**(복원·스윕·stale close 의 scope 필터) ② **감지 계층 식별**(아래 표, correlatedNotifications 상관 근거) 에 쓰인다. 고장 객체(mo_instance)와 탐지 주체가 다를 수 있음(예: db_down 은 oam-svc 탐지, 객체는 DB). mo 루트가 서버명/그룹명으로 통일되면서 open-state 소유 파티션의 키는 **detected_by 가 유일**하다 — 구현의 구 mo 접두(`cims/*`) scope 필터는 구 레코드 read 흡수용으로만 남는다.

**감지 주체 3계층** — 모듈 상태는 서로 다른 사실을 보는 세 주체가 나눠 감지하며, 한 주체의
관측을 다른 계층의 의미로 쓰지 않는다(원격 probe 무응답 ≠ 프로세스 사망).

| 계층 | 주체 (detected_by) | 보는 사실 | 알람 / 이벤트 |
|---|---|---|---|
| **L1 프로세스 생존** | agent (`agent`) | 노드 로컬 프로세스의 기동/종료 — 모듈을 배포·실행하는 주체가 생존도 판정 (ha_service_model §1 "판정은 노드 로컬") | `A-PRC-001` process_down (**전 모듈**) · 이벤트 `process_died`(전이 관측 — SIGKILL 등으로 모듈 자기보고가 유실되는 종료 보완) |
| **L2 모듈 내부 상태** | 모듈 자기보고 (`self`) | 살아 있는 프로세스의 내부 이상 — DB 연결·자원 풀·스토어 | `A-COM-001` · `A-QOS-002` · `A-PRC-002` · 이벤트 `process_started`/`process_stopping` ([alarm_self_reporting.md](alarm_self_reporting.md)) |
| **L3 서비스 응답성** | OAM 원격 probe (`oam-svc`/`oam`) | 살아 있어도 응답하지 못하는 상태 — hang·과부하 (STATS 무응답, DB SELECT 실패) | `A-PRC-004` service_unresponsive · `A-COM-001`(`<관리그룹>/db`) |

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

**이벤트 분류·코드 체계 — 알람과 대칭 (확정)**. 이벤트도 알람과 마찬가지로 **성격별 조건
클래스**를 갖는다 — X.730/X.731/X.740 이 통지를 objectCreation·stateChange·
attributeValueChange 등 성격 클래스로 식별하고 구체 내용은 속성으로 보내는 구조와 정합.
개별 사건명(`ha_switchover`·`config_reloaded`)을 type 으로 삼으면 분류 축 없이 슬러그가
증식하고 명명이 표류한다 — 사건명은 **정의**(사전 항목) 축이다.

구조(알람과 완전 대칭 — §3.4(a) 코드 체계 공통 적용):

| 축 | 알람 | 이벤트 |
|---|---|---|
| 도메인 | eventType 약어 (PRC/COM/QOS/EQP/ENV/SEC) | **kind 약어** — **STC**(stateChange) / **AUD**(audit) |
| 클래스(type) | 조건 클래스 20종 (모듈 카탈로그 §2.3) | 성격 클래스 9종 (아래) — 코드 없음, 슬러그가 식별자 |
| 정의 | 정의 코드 `A-<DOMAIN>-NNN` — 카탈로그 행 | 정의 코드 `E-<STC\|AUD>-NNN` + 정의 슬러그(wire type — `event=` 표기, 정의당 유일) |
| 인스턴스 | mo_instance | mo_instance |

**이벤트 성격 클래스 (9종)**:

| type | 정의 (성격) | 대표 정의 |
|---|---|---|
| `lifecycle` | 프로세스/객체의 기동·종료·소멸·재기동 (X.730 objectCreation/Deletion 상당) | process_started/stopping/died, module_restarted |
| `ha_transition` | 이중화 역할·절체 전이 | ha_role_changed, ha_switchover |
| `mode_transition` | 운영 모드 전이 | db_fallback_changed, emergency_mode_changed |
| `availability_transition` | 피어/노드 가용성 전이 | peer_state_changed, node_offline, 제어 피어 변경 |
| `config_changed` | 설정/데이터 변경 반영 (X.730 attributeValueChange 상당) | config_reloaded, 가입자/그룹/리스너/자원 구성, affiliation, regroup |
| `admin_action` | 운영자 개입 | service_control, node_maintenance, 대국 수동 차단 |
| `corrective_action` | 시스템 자발 교정·회수·재동기 | session_reclaimed, 전량 재동기(resync) |
| `security_audit` | 인증/접근 감사 (X.740) | auth_audit |
| `job_result` | 배포/작업 결과 | deploy_job, deployment_failed |

- severity 없음·활성목록 미게재는 그대로(표 위) — 클래스는 분류일 뿐 알람화가 아니다.
- **wire/저장 호환**: 현행 레코드의 `type` 은 정의 슬러그다 — 이행 시 `code`(정의 코드)를
  추가하고 `type` 은 정의 슬러그로 유지(API/필터 호환), 클래스·kind 는 code 에서 도출한다
  (`kind` 필드는 존치 — 기존 reader 무영향). 정의 슬러그는 정의당 유일해야 한다.
- 상위 NMS 수치 키는 §7.3 매핑 테이블에서 부여(알람 코드와 같은 자리).
- 새 이벤트는 먼저 9클래스에서 흡수처를 찾고, 어느 성격에도 맞지 않을 때만 클래스를
  신설한다(알람 §3.5 와 같은 규율).

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
- detected_by 인스턴스 접미(`agent:<host>`·`self:<node>`)는 구 레코드에만 남는다 — read·scope
  필터는 클래스 매칭(`self` = `self`·`self:*` 접두)으로 양쪽을 흡수하고, 신규 기록은 클래스만 쓴다.
- **mo 루트 개편**(구 `cims/...` → 서버명/그룹명 루트, §3.4(b))은 활성키 개편이다 — 구 mo 로
  열린 활성 알람은 스윕이 이행 종결(close, `alarm_sweeper.close_migrated_keys` — 정의 코드
  이행과 한 번에)하고 지속 조건을 현행 code@mo 로 재발화한다. 구 wire(구 모듈 + 신 OAM
  배포 스큐)는 FM ingest 가 수신 시 현행 코드·서버명 루트로 정규화해 흡수한다. 구 레코드
  이력은 무수정(read 시 그대로), open-state 소유 파티션 판정은 detected_by 로 일원화
  (`alarm_sweeper.partition_of`)하되 detected_by 없는 구 레코드만 mo 접두(`cims/*`)로
  폴백한다. 콘솔 토폴로지는 서버명 루트(모듈 칩 — component 알람 포함)와 그룹 루트
  (시스템 카드)로 매칭하고 `cims/<모듈>` 은 구 레코드 흡수로만 남는다.

## 7. 벤치마크 — 다른 통신 서버/규격의 알람 코드 정의

본 설계의 정합성 확인 + 보강점 도출을 위해 실제 통신 서버/규격의 알람 정의 방식을 조사.

| 항목 | RFC 3877 (Alarm MIB) | Project Clearwater (Metaswitch IMS) | 3GPP 32.111-2 | ONAP VES / ETSI NFV | **CIMS(본 설계)** |
|---|---|---|---|---|---|
| 코드(카탈로그) | `alarmModelIndex`(int) | numeric OID, 컴포넌트별 범위 | alarmType | `alarmCondition`(str) | `code` `A-PRC-001` |
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
| `A-PRC-001` process_down | 해당 인스턴스 호처리/기능 중단 | 프로세스 재기동, 로그/코어 확인, HA 절체 점검 |
| `A-PRC-004` service_unresponsive | 관리/제어 응답 불가 — hang·과부하 의심, 호처리 영향 가능 | 프로세스 상태·부하 확인(L1 process_down 동반 여부), 필요 시 재기동 |
| `A-COM-001` connection_lost | 의존 자원(DB/트렁크) 사용 기능 저하 | 연결성/방화벽/원격 노드 상태 확인 |
| `A-QOS-001` threshold_crossed | 용량 임계 근접 — 추가 부하 시 실패 위험 | 사용량 원인 파악, 자원 증설/정리 |

### 7.2 보강 — 사내 벤치마크: vIBCF/TrGW POD 대조

사내 타 시스템 vIBCF/TrGW 의 알람 정의서(POD)를 md 로 변환해 보존한다 —
[vibcf_pod_alarms.md](vibcf_pod_alarms.md) (알람 32종 + Fault/상태 메시지 79종, 코드별
조치사항 포함). CIMS 모델과의 대조와 채용 포인트:

| 관점 | vIBCF/TrGW | CIMS | 판단 |
|---|---|---|---|
| 스트림 분리 | **A**(알람)/**F**(Fault — 호처리 이상 로그, FAULT/WARNING/INFO)/**S**(상태·통계 알림) 3분리 | 알람 vs 이벤트(stateChange/audit) 2분리 + flow/msg 로그, 요청 단위 이상은 로그/PM 소관(§3.6) | 동형 — **F 상당의 제3 스트림(이벤트 kind 확장)은 신설하지 않는다**(확정, §7.2.2). vIBCF 의 A/F 분리 자체가 "요청 단위 이상을 알람과 섞지 않는다"는 같은 원칙의 구현이고, CIMS 의 구조적 대응물은 flow 스트림(sesid 호 단위 상관)이다. F 의 실질 가치(실패 **사유의 코드화**)는 flow `detail` 사유 슬러그 보강으로 흡수한다(§7.2.2 채용 1) |
| 코드 층위 | **장애코드**(A00XX — flat, 알람 정의당 1개) + **타입 인덱스**(분류 — CommunicationFail/DatabaseError… 코드 아님) 분리 | **정의 코드**(`A-<DOMAIN>-NNN` — flat, 카탈로그 행당 1개) + **type**(클래스 슬러그 — 코드 없음) — §3.4(a) | 동형(vIBCF 대조가 정의 코드 층위 신설의 근거 — flat 채번까지 동일 관례). 단 vIBCF 는 호스트 리소스도 정의 분리(A0012~14 CPU/DISK/MEM) — CIMS 는 정의 1개 + mo 로 통합(§3.5) |
| severity | CLEARED+MINOR/MAJOR/CRITICAL "가변", 단계 임계 70/80/90 을 설명 템플릿에 명시(`CPU load is A% (CRI:B,MAJ:C,MIN:D)`) | thresholds 단계(80/90/95) + action=change + threshold_info(§3.2) | 동형 — 관측값·임계 동반 표기는 threshold_info 로 이미 수용 |
| 발생 위치 | `서버명/프로세스명`·`서버명/DISK/파티션명` 경로 | mo_instance DN-유사 경로(§3.4(b)) | 동형 |
| **조치사항** | **전 알람 코드에 다단계 조치 절차**(확인 경로·설정 파일·해제 메뉴까지) | effect/recommended_action 선택 필드(§7.1) — 현행 카탈로그는 1줄 수준 | **채용** — 카탈로그(fm_catalog·rule)의 recommended_action 을 절차 수준으로 강화하고 콘솔 상세에 노출. POD 문서 산출물(코드별 1페이지) 자체가 운영 이관 규격이라는 점도 참고(카탈로그 CSV → POD 생성 후보) |
| 감시 항목 | NTP 3종(Delay/Offset/Status), 성공률(SuccessFailError), CPS/세션/채널 과부하 세분, Manual Block 상태 알람, 절체 알림(S3001~5) | NTP 없음, 성공률/CPS/세션 카운터 없음(§7.2.1 실사), overload 1클래스, 수동 개입은 node_maintenance 이벤트+redundancy_degraded reason, ha_switchover 이벤트 후보 | **채용 확정**: NTP·호/등록 성공률·CPS·세션 사용률·SIP 수신 이상 급증을 기능 관점 필요 알람으로 편입 — 정본은 [alarm_function_catalog.csv](alarm_function_catalog.csv)(구현 무관 요구 카탈로그). Manual Block·절체 알림은 기존 이벤트/reason 대응(§7.2.1 A0023·A0063) |

> vIBCF POD 원문의 A0000 "메시지 설명"이 CPU 문구로 오기재된 것 등 원문 결함은 변환본에
> 그대로 보존했다(원문 보존 원칙).

#### 7.2.1 A 계열(알람 32종) 전수 대조

전 코드 대조 결과. "대응" = CIMS 카탈로그(기능 요구 [alarm_function_catalog.csv](alarm_function_catalog.csv)
또는 모듈 자기감지 [alarm_module_catalog.csv](alarm_module_catalog.csv))에 대응 항목 존재.

| vIBCF | 조건 | CIMS 대응 / 판정 |
|---|---|---|
| A0000·A0034 | 물리 NIC/Ethernet down | 대응 없음 — agent `collect_interfaces` 가 operstate 미수집(ifname+IPv4 만). 검토 후보: operstate 수집 확장 + `net/managed` drift 의 reason 확장(별행 신설 보류) |
| A0003 | DB 쿼리 오류 | `db/query` storage_failure 행 — 대응 |
| A0007·A0061 | 내부/외부 연동 두절 | COM-001 계열(CMP/CMDP/CSC/peer/agent/FM_SYNC) — 대응 |
| A0011 | 프로세스 down | A-PRC-001(L1) — 구현 |
| A0012~14 | CPU/DISK/MEM 단계 임계 | QOS-001(disk 구현·cpu/mem/load 후보) — 대응 |
| A0023 | Manual Block 지속 | 전이=`node_maintenance` 이벤트·잔존=redundancy_degraded(MAINTENANCE reason) — 대응(모델 분담 상이) |
| A0030·A0089 | 큐 full/사용률 | QOS-002 `log_queue` 계열 — 부분 대응(SIP 수신 큐는 관측 지점 없음, psip 소관) |
| A0041~43 | NTP Delay/Offset/Status | 2정의 분리 — Delay/Offset=threshold_crossed(A-QOS-003 단계 임계)·Status=connection_lost(A-COM-014 동기 상실, X.733 lossOfSynchronisation) — 기능 카탈로그 공통 `ntp` + 모듈 카탈로그 AGENT 행 |
| A0057 | 과부하 drop | QOS-005 overload — 대응(감지 로직 선행 필요) |
| A0058 | CPS 임계 | 카운터 전무(`cps_limit` 파싱만·소비 0 — `CspRouteMap.cpp:52`) — **기능 카탈로그 편입**(CSCF `cps`) |
| A0059 | 세션 사용률 임계 | 분자 기성(`CallMap::GetCount`)·분모(상한 설정) 부재 + STATS active_calls 상시 0 결함(`DbManager.cpp:658` 스텁) — **기능 카탈로그 편입**(CSCF `sessions`) |
| A0063 | HA 절체 | 사건=`ha_switchover`/`ha_role_changed` 이벤트·비정상 빈도=ha_flap 알람 — 대응(vIBCF 는 절체 자체가 알람) |
| A0075~77 | 성공률/소통률/완료율 하한 | 카운터·주기 집계 전무. 현행 파일 사후 산출(OAM `stats.py:937-990`)은 분모 결함 — 조기 거절(403/404/480/603/500)이 call.json 미생성(`ModuleDispatcher.cpp:972` 단일 기록점) + end_reason `normal\|error` 2치 — **기능 카탈로그 편입**(CSCF `calls/success_rate`·IBCF 대국별) |
| A0078·79 | 미디어 시간/Kbps 하한 | 기능 카탈로그 MRF `media/no_flow`·`media/quality` 로 수용(관측 카운터 신설 선행) |
| A0081 | SIP syntax 오류 | 파싱 실패가 완전 침묵(무응답 delete — `SipStackComm.hpp:199-203`, 카운터 0) — **기능 카탈로그 편입**(CSCF/IBCF `sip/rx_error`) |
| A0083 | RTT 임계 | 기능 카탈로그 IBCF `peer/<n>/rtt` 로 수용(대국별 축 — vIBCF 관례 채용) |
| A0084 | SIP 실패 reason 별 임계 | 응답코드별 카운터(성공률과 공통 선행) 완성 후 params/대국별 행으로 수용 — 기능 카탈로그 IBCF `peer/<n>/reason` |
| A0085 | HA 설정 변경 | `node_maintenance`(HA 얼림·오버라이드) 이벤트 — 대응 |
| A0086 | 프로세스 hang | PRC-004(L3 probe + zombie readiness) — 대응 |
| A0087·88 | NFV 인프라(GM/VM Host) 연동 | 해당 기능 없음(최근접 `ext/<system>` probe) — 비대상 |
| A0090 | CDR 미기록 | PRC-002 `call_dir`·`call_dir/root` — 대응 |
| (타입만) 39·40·44·80 | 성공률/채널과부하/NAS/OPTIONS | 각각 A0075 동일 축 / MRF QOS-002 / store·NAS 행 / peer OPTIONS 행 — 대응 |

#### 7.2.2 F 계열(F4xxx 호처리 · F5xxx 미디어) 판정

**확정 — F 상당의 별도 운영자 스트림은 신설하지 않는다.** 요청 단위 이상은 로그/PM
소관(§3.6)이 유지 원칙이고, CIMS 의 구조적 대응물은 flow 로그다(sesid 로 호 단위 상관,
와이어에 나가는 응답은 전부 flow `method`=상태코드로 남는다). 코드 실사에서 flow 가
F 계열의 정보량에 못 미치는 갭을 확인했고, 아래 2건을 채용한다:

1. **flow 사유 코드화** — 같은 응답 코드에 서로 다른 원인이 섞이고(403 = ACL deny·라우팅
   reject·인증 실패·MCPTT upgrade 거절 / 404 = route 미발견·미등록 착신), 사유는 sesid 없는
   텍스트 CLog 에만 있어 상관 불가. 거절 응답 발신 지점에서 flow `detail` 에 사유 슬러그를
   기록한다(vIBCF F 코드의 "이유의 코드화" 가치를 flow 레코드로 흡수 — flow_logging.md 소관).
2. **호 카운터에 로컬 합성 응답 포함** — Timer B/C 만료 시 psip 이 합성하는 로컬
   408(`SipICTList.cpp:190-224`)은 와이어를 타지 않아 flow 에 절대 남지 않는다 — 성공률
   카운터가 유일 관측이므로 응답코드 집계에 반드시 포함한다.

실사로 확인된 **완전 침묵 사각**(flow·CLog·카운터 모두 부재 — F 대응물 0): SIP 파싱 실패
사실(무응답 delete) · 100B 미만 드롭 · Max-Forwards 검사 부재(F4001 — 483 사용처 0) ·
다이얼로그/세션 상한 부재(F4009/F400A — 상한 자체 없음) · 다이얼로그 미발견 BYE/CANCEL 의
무조건 200(F400B — 정상과 구분 불가) · FSM/CSeq 미검증(F400C) · non-INVITE 타임아웃 응답
폐기 · peer unavailable 탐지 부재(F400E/F4101 — `MarkFail` dead code) · Session-Timer 미구현.
→ 지속/율 전이가 가능한 것은 기능 카탈로그 알람으로 수용, 나머지는 규격 정합 결함으로
모듈 카탈로그 §5.1 에 기록.

F5xxx(eMP/TGAS ≈ 미디어평면) 압축 대조: 자원 부족(F5002/F5011)=QOS-002 · garbage
회수(F5005)=QOS-004 · 세션 불일치(F5006)=PRC-009 · 미디어 무흐름(F5008)=MRF `media/no_flow` ·
제어 응답 타임아웃(F500C~E)=단건은 로그 소관·지속 축은 COM-002 · 미디어평면 연동 불가로 호
거절(F4300~04)=COM-001 `cmp/<ep>`·`cmp_ctrl` · SDP/코덱/Content-Type/offer-answer
(F5004/07/09/0F)=CIMS 에선 시그널링 서버 소관(미디어 relay 는 SDP 비취급) · 풀/그룹 정보
미발견(F5012~15)=PRC-008 config_invalid.

#### 7.2.3 S 계열(상태·통지) 판정

| 군 | 내용 | 판정 |
|---|---|---|
| S1003~S1609 | 통계 적재 완료 알림(분/5분/시/일/주/월/년) | 미채용 — 정기 "성공 알림"은 스트림 비노출 원칙(성공은 침묵, 실패가 알람 — storage_failure 계열이 실패 축) |
| S1610·11 | 보존기간 삭제 알림 | 미채용 — 로그 소관(이벤트 보존/회전 과제는 self_reporting §9) |
| S19xx | 주기 지표 요약(INFO) | 미채용 — agent metric + 콘솔 대시보드가 대응 |
| S21xx·S22xx | PKG/DB 백업·복원 결과(실패는 FAULT) | **기능 갭** — CIMS 는 백업 자동화 자체가 없다. 도입 시 결과=kind:audit 이벤트, 실패 지속=PRC-002 |
| S3001~3 | 절체 수행/실패/ACTIVE 변경 | 대응 — `ha_switchover`(계획 절체 FAILED 포함)·`ha_role_changed` 이벤트 |
| S3004·5 | 자원(CPU/MEM) 기인 절체 발송 알림 | 비대상 — CIMS 절체 트리거 모델 상이(모듈 down/readiness 기반) |

### 7.3 보강 — SNMP / NMS northbound 매핑 (P2)

상위 NMS 연동 시 `code` 를 표준 식별자로 매핑:
- **RFC 3877 alarmModel**: `code`(정의 코드 — §3.4(a)) → `alarmModelIndex`(정수, 도메인별 범위 예약) + perceived_severity → `alarmModelState`. 활성 알람 → `alarmActiveTable`(alarm_id).
- **3GPP 32.111-2 IRP / VES**: code → alarmType/`alarmCondition`, source.mo_instance → managedObjectInstance, message → specificProblem, perceived_severity → eventSeverity.
- 매핑은 별도 테이블(code↔int OID)로 관리 — CIMS 내부 모델은 문자열 code 유지, northbound 게이트웨이에서 변환.

## 관련
- [alarm_function_catalog.md](alarm_function_catalog.md) — IMS 기능(CSCF/IBCF/TAS/PTT-AS/MRF) 관점 필요 알람/이벤트 요구 카탈로그(구현 무관 정본)
- [alarm_self_reporting.md](alarm_self_reporting.md) — 모듈 자기보고(FM push) 경로 — 본 모델 위의 발생 경로 확장 + 이벤트 스트림
- `console_platform.md` (Service Descriptor: modules/alert_rules/data_sources) · `features/monitoring.md`
- 3GPP TS 32.111-2 (Alarm IRP) · ITU-T X.733 (Alarm reporting) · IETF RFC 3877 (Alarm MIB) · ONAP VES (fault) / ETSI NFV · Project Clearwater (IMS 알람 사례)
