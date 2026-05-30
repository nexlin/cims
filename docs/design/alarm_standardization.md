# 알람(이벤트) 표준화 설계 — X.733 / 3GPP TS 32.111

CIMS 알람을 임의 스키마(`critical`/`warning` 2단계)에서 **IMS 망관리 표준**에 맞춰 체계화하기 위한 설계.
구현은 단계(P0/P1)로 분리하며, 본 문서는 표준 매핑·제안 모델·이행 계획을 확정한다(코드 변경 전 SoT).

## 1. 참조 표준

- **3GPP TS 32.111-2** — *Telecommunication management; Fault Management; Part 2: Alarm Integration
  Reference Point (IRP): Information Service (IS).* IMS 포함 3GPP 망요소(NE/EM)가 관리객체의 알람을
  Manager(NMS)로 보고하는 인터페이스. CIMS 가 향후 상위 NMS 와 연동할 때의 정합 기준.
- **ITU-T X.733** — *Systems Management: Alarm reporting function.* 위 IRP 가 기반하는 알람 속성 모델.
- (참고) **ITU-T M.3100 / X.736** — 관리객체 모델 / 보안 알람 분류 보강.

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
  "type": "csp_down",                  // 알람 식별자 (유지)
  "perceived_severity": "critical",    // critical|major|minor|warning|indeterminate  (기존 severity 대체)
  "event_type": "processingError",     // communications|qualityOfService|processingError|equipment|environmental
  "probable_cause": "softwareError",   // X.733 Annex 코드
  "mo_class": "software",              // managedObject class: software|service|equipment|host|network
  "check": "process_down", "target": "csp",
  "threshold": null, "unit": null,
  "metric": "CSP 프로세스",
  "msg_open": "CSP 프로세스 응답 없음",   // → specificProblem
  "msg_close": "CSP 응답 정상화",
  "scope": "service"                   // service|agent (유지)
}
```
- `perceived_severity` 가 기존 `severity` 를 대체. **하위호환**: `severity` 만 있으면 그대로 perceived_severity 로 승격(critical/warning 은 표준 값 그대로 유효), 신규로 major/minor/indeterminate 사용 가능.
- managedObject **instance** 는 런타임 합성: service 규칙 = `cims/<target>` (예 `cims/csp`), agent 규칙 = `<host>/<module|disk|rtp>`.

### 3.2 이벤트 레코드(alert_log) 확장

```jsonc
{
  "ts": "2026-05-30T09:31:05",         // eventTime
  "type": "module_down",
  "perceived_severity": "critical",    // open=규칙 severity, close=cleared
  "event_type": "processingError",
  "probable_cause": "softwareError",
  "managed_object": "Media-Server-01/csp",   // source instance (DN-유사)
  "action": "open",                    // open|close (close = Cleared)
  "message": "Media-Server-01 모듈 csp 프로세스 응답 없음",  // specificProblem
  "ack_state": "unacknowledged"        // P1: acknowledged 시 ack_time/ack_user 추가
}
```
- 기존 필드 보존(`ts/type/action/message`) → 이력/통계/배너 무중단. `severity` 는 `perceived_severity` 로 점진 치환(읽기 시 둘 다 허용).

### 3.3 현재 CIMS 알람 → 표준 매핑 (확정)

| type | perceivedSeverity | eventType | probableCause | mo_class | managedObject(instance) |
|---|---|---|---|---|---|
| `csp_down` | critical | processingError | softwareError | software | cims/csp |
| `cmp_down` | critical | processingError | softwareError | software | cims/cmp |
| `module_down` | critical | processingError | softwareError | software | `<host>/<module>` |
| `db_down` | critical | processingError | underlyingResourceUnavailable | service | cims/db |
| `rtp_high` | warning | qualityOfService | thresholdCrossed (resourceAtOrNearingCapacity) | service | cims/rtp_ports |
| `disk_high` | warning | qualityOfService | storageCapacityProblem | host | `<host>/disk` |

> 운영 정책에 따라 rtp_high/disk_high 는 임계 단계별 minor→major 승격 가능(예: 80% warning, 90% minor, 95% major). 규칙을 임계별로 분리하거나 다단 threshold 확장(P1 후속).

## 4. 전파 경로(구현 시 변경 지점)

1. **규칙 데이터**: `cims.json` + `_CORE_ALERT_RULES` 에 event_type/probable_cause/mo_class 추가, severity→perceived_severity.
2. **sweeper** (`oam_app.py`): `_emit` 가 규칙의 표준 필드 + 합성 managedObject 동반 기록. `_transition` 시그니처에 rule(또는 표준필드) 전달.
3. **alert_log** (`alert_log.py`): record/read 가 신규 필드 통과(스키마 free-form JSONL 이라 호환). summary 에 event_type/severity 분포 추가(선택).
4. **API** (`alerts.py`): `/alerts` 이벤트에 표준 필드 노출, `/rules` 에 event_type/probable_cause/severity(6) 추가.
5. **UI**: AlertsPage 심각도 6색 배지 + eventType/cause/managedObject 컬럼 · 필터(심각도/유형). AlertBannerWidget 심각도색. ServiceDescriptors 폼(ServiceForm)의 알람 규칙 입력에 severity(6)/event_type(5)/probable_cause select 추가.
6. **타입**: `serviceDescriptors.AlertRule` + `alerts.AlertEvent/AlertRule` 확장.

## 5. 단계 계획

- **P0 — 분류 체계** (본 설계의 §3.1~3.3): perceived_severity(6) · event_type(5) · probable_cause · managedObject. 규칙/이벤트/ API/UI/폼 전파. 하위호환(severity→perceived_severity 승격).
- **P1 — 라이프사이클**: ackState/ackTime/ackUser + clearTime 명시 + `PATCH /alerts/{id}/ack` API + UI 승인 버튼. 운영 감사추적.
- **P2 — 상관/연동**: correlatedNotifications(연관 알람), alarmId, 상위 NMS 연동(32.111 IRP 매핑/Northbound).

## 6. 하위호환·이행

- 이벤트 JSONL 은 free-form → 신규 필드는 누적만, 기존 reader 무영향.
- `severity` 읽는 곳은 `perceived_severity ?? severity` 로 폴백 → 점진 전환.
- 규칙은 `event_type`/`probable_cause` 누락 시 기본값(processingError/—) 부여하는 정규화 헬퍼로 흡수(데이터 미보강 descriptor 안전).

## 관련
- `console_platform.md` (Service Descriptor: modules/alert_rules/data_sources) · `features/monitoring.md`
- 3GPP TS 32.111-2 (Alarm IRP), ITU-T X.733 (Alarm reporting)
