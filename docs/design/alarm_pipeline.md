# 알람/이벤트 파이프라인 — 발생·전달·수집·보관·가시화

알람/이벤트가 **감지 주체에서 발생해 운영자 화면에 닿기까지의 전 구간**(발생 → 전달 →
수집·정규화 → 보관 → 조회 → 가시화)의 단계별 계약과 책임 주체를 정의하는 정본이다.
알람 모델(X.733 속성·정의 코드·활성키·재통지)은
[alarm_standardization.md](alarm_standardization.md), L2 자기보고 wire 상세는
[alarm_self_reporting.md](alarm_self_reporting.md) 가 정본이며 여기서 중복하지 않는다 —
본 문서는 그 조각들을 **하나의 절차로 잇고**, 구간별로 흩어져 있던 계약(전달 경로·수렴점·
보존·API·화면)을 확정한다.

## 1. 문서 가족에서의 위치

| 문서 | 축 | 담는 것 |
|---|---|---|
| [alarm_standardization.md](alarm_standardization.md) | 모델 | X.733 속성·severity·정의 코드 체계·감지 3계층·재통지 규율 |
| [alarm_function_catalog.md](alarm_function_catalog.md)/.csv | 요구(what) | 기능(IMS 역할) 관점 필요 알람/이벤트 — 구현 무관 정본 |
| [alarm_module_catalog.md](alarm_module_catalog.md)/.csv | 구현(how·현황) | 모듈 자기감지 전수 목록 — 감지 방식·코드 근거·구현 추적 |
| [alarm_self_reporting.md](alarm_self_reporting.md) | L2 wire | 자기보고(FM push) envelope·카탈로그 등록·동기화 규격 |
| **본 문서** | **절차(end-to-end)** | 발생→전달→수집→보관→가시화 구간별 계약·책임 주체·수렴 보장 |

## 2. 파이프라인 개요

```
[발생 — 감지 4주체]          [전달]                        [수집·정규화 — OAM]        [보관 — JSONL 스트림]          [조회·가시화]
L1 agent (노드 관측) ──── HTTPS POST /api/agent/metric ──→ agent 수신부·sweeper ─┐
L2 모듈 (FmReporter) ──── UDP :9010 envelope v2 (FM_*) ──→ FM ingest ───────────┤   ┌ alerts/YYYY/MM/DD.jsonl ──→ GET /api/v1/alerts ──→ 콘솔(활성·이력·카탈로그·위젯)
L3 OAM probe (원격) ───── (OAM 내부 — rule 평가) ─────────→ service sweeper ────┼──▶┤ events/YYYY/MM/DD.jsonl ──→ GET /api/v1/events ──→ 콘솔(이벤트 탭)
OAM 자체판정 (drift 등) ── (OAM 내부) ────────────────────→ drift sweeper ──────┘   └ fm_catalog/<node>.json ───→ GET /alerts/catalog ──→ (P2) NMS northbound
                                                            ↑ 알람 수렴점 = alarm_sweeper.transition / 이벤트 수렴점 = event_log.record_event
```

| 구간 | 책임 주체 | 계약(정본 절) |
|---|---|---|
| 발생 | 감지 주체(L1/L2/L3/OAM 자체) | §3 — 조건 판정·전이는 감지 주체 소관 |
| 전달 | 발생 주체 → OAM | §4 — 경로별 wire·신뢰성 계약 |
| 수집·정규화 | OAM(fm_ingest·sweeper) | §5 — 검증·dedup·렌더 후 수렴점 통과 |
| 보관 | OAM(alert_log·event_log) | §6 — 스트림 구조·SoT·보존·복원 |
| 조회 | OAM handlers | §7 — API 계약·인증 |
| 가시화 | 콘솔 | §8 — 뷰 계약·판독 규율 |

## 3. 발생 (Detection → Transition)

### 3.1 감지 주체별 발생 절차

감지 3계층의 정의·경계는 표준화 §3.4(b) 정본. 발생 절차 관점 정리:

| 주체 (detected_by) | 관측 | 판정 위치 | 발생물 | 발화 절차 |
|---|---|---|---|---|
| **L1 `agent`** | 노드 로컬 — 프로세스 생존, 호스트 자원, 설정 해시, HA 전이 | **OAM**(agent 는 원시 관측만 보고) | process_down·threshold_crossed(host)·config_out_of_sync·ha_flap + `process_died` 이벤트 | agent 가 metric 에 관측 필드 동봉 → OAM `_eval_agent_rule` 이 rule 평가·전이 |
| **L2 `self`** | 모듈 내부 — DB 연결, 자원 풀, 스토어, 내부 상태 | **모듈**(전이 판정까지 모듈 소관) | fm_catalog 등재 알람 + 이벤트 | 모듈이 open/close 전이 시점에 FM_ALARM/FM_EVENT 발신 (조건·임계는 모듈 설정 소유) |
| **L3 `oam-svc`/`oam`** | 원격 응답성 — STATS probe, DB SELECT, RTP 사용률 | **OAM**(sweeper rule) | service_unresponsive·connection_lost(db)·threshold_crossed(rtp) | 주기 스윕(30s)이 probe 후 전이 |
| **OAM 자체 (`oam`)** | 관리평면 정합 — HA fan-out drift 등 | **OAM** | config_out_of_sync(HA) | drift sweeper 가 판정·전이 |

- **판정은 감지 주체 소관, 기록은 OAM 소관.** L2 만 전이 판정이 모듈에 있고(활성 알람 SoT =
  모듈, OAM 은 미러 — self_reporting §1), L1 은 관측/판정이 agent/OAM 으로 분리된다.
- 발화 시 감지 주체가 싣는 것은 **(code, mo_instance, params)** 뿐이다 — severity·eventType·
  probableCause·메시지 템플릿·effect/조치는 카탈로그(fm_catalog·rule)가 보유한다(RFC 3877
  alarmModel/alarmActive 분리).

### 3.2 발생 규율 (표준화 정본 참조)

- 활성키 = `(code, mo_instance)`, `alarm_id` = occurrence — 표준화 §3.4(c).
- 재통지·severity 변경(action=change)·판정 불가 종결·관측 공백 0건 무판정 — 표준화 §3.4(d).
- 알람 = 지속 조건(open/close), 정상 전이·감사 = 이벤트(kind=stateChange|audit) — 표준화 §3.6.
- 요구 정의의 목록 정본 = [alarm_function_catalog.csv](alarm_function_catalog.csv). 구현 채택 시
  감지 주체 배정 → 모듈 카탈로그 등록 → fm_catalog/rule 등재 순(기능 카탈로그 §6).

## 4. 내부 연동 규격 (전달 계약)

### 4.1 경로별 규격

| 경로 | 전송 | 규격 정본 | 신뢰성 |
|---|---|---|---|
| **L2 FM push** | UDP `FmIngest.Ip:Port`(기본 0.0.0.0:9010), envelope v2, cmd `FM_REGISTER/FM_ALARM/FM_EVENT/FM_SYNC` | self_reporting §3 (payload·boot_id/seq·카탈로그 등록) | ack(trans_id 매칭 response) 미수신 시 1s×5 재전송, 유실은 FM_SYNC(60s)가 수렴. 데이터그램 4KB 상한 |
| **L1 agent 보고** | HTTPS `POST /api/agent/metric`(2s)·`/heartbeat`(2s), `X-Agent-Token` | [../api/agent_api.md](../api/agent_api.md). 알람성 필드: `modules[]`(생존)·`module_events[]`(process_died 전이)·`cfg_hashes`(drift)·`ha_transitions`(flap)·`mounts`/자원 | best-effort 주기 보고 — 유실은 다음 tick 이 흡수(전이성 `module_events` 는 발생 tick 1회라 유실 가능 — L1 알람은 상태 기반이라 무영향, 이벤트만 결손) |
| **L3 probe** | OAM → 모듈 STATS UDP probe·DB SELECT (rule 의 check 정의) | descriptor `alert_rules`(표준화 §3.1 스키마) | 주기 스윕(`AlertSweepSec` 30s) 재평가 자체가 수렴 |
| **OAM 내부** | 함수 호출(sweeper→transition) | — | 재평가 수렴 |

보안 모델: L2/L3 는 내부 제어평면 평문 UDP(cmp_media_api 와 동일 신뢰 모델), L1 은
mTLS+토큰. OAM 이중화 환경의 목적지는 관리평면 VIP(oam_ha) — 절체 후 불변.

### 4.2 단일 수렴점 (불변식)

**알람 레코드는 `alarm_sweeper.transition`(akey=`code@mo_instance`) 를 통해서만,
이벤트 레코드는 `event_log.record_event` 를 통해서만 생성된다.** 발화 경로(FM ingest·
agent 규칙·서비스 스윕·drift)가 늘어도 alarm_id 발급·change 판정·open-state 관리는 이
수렴점 하나가 보장한다 — 스트림 파일에 직접 append 하는 경로를 만들지 않는다
(ack/comment 는 상태 전이가 아닌 부가 레코드로, handlers 가 alert_log 에 기록하는 기존
경로를 유지한다).

### 4.3 소유 파티션 (detected_by)

`detected_by` 는 발행 주체별 open-state **소유 파티션**이다(표준화 §3.4(b)) — 복원·스윕·
stale close 는 자기 파티션만 다룬다.

| detected_by | 발행 주체 | 복원 scope | stale 정리 책임 |
|---|---|---|---|
| `agent` | base(oam_app agent 규칙) | agent 파티션 | 미평가 대상 정리 — 단 §9 관측 두절 규율 준수 |
| `self` | FM ingest(oam-svc, `--role all` 은 base 대행) | self 파티션 | FM_SYNC reconcile + sync 3회 두절 시 판정 불가 종결 |
| `oam-svc` / `oam` | 서비스 sweeper·drift sweeper | 서비스 파티션 | 스윕 재평가 |

파티션 키는 **detected_by 가 유일**하다 — mo 루트가 소유 주체(서버명/그룹명, 표준화
§3.4(b))로 통일되면서 mo 접두는 파티션을 구분하지 않는다(현행 구현의 `cims/*` scope
필터는 구 레코드 흡수용).

## 5. 수집·정규화 (OAM ingest)

FM ingest 의 수신 처리 순서 (구현: `ems/core/oam/src/services/fm_ingest.py`):

1. **envelope 검증** — ver=2·type=event·cmd∈FM_*·node·trans_id. 위반 = `BAD_REQUEST`.
2. **재전송 dedup** — `(node, trans_id)` LRU(1024) 로 재전송 흡수(동일 응답 재송).
3. **등록 검사** — 미등록 node/boot_id 불일치 = `UNREGISTERED` → 모듈이 재등록.
4. **카탈로그 검증** — 등록 카탈로그에 없는 code = `UNKNOWN_CODE` 거부(오염 차단).
   sweeper 발화 중인 `code@mo` 공간과의 충돌은 등록 시 거부.
5. **순서 보정** — 활성키별 seq 역전 폐기(UDP 재정렬 방지).
6. **정규화** — severity: payload → 카탈로그 → `indeterminate` 순. 메시지: 카탈로그
   msg_open/msg_close 를 params 로 렌더(발신은 데이터만 — 콘솔 표기 일관성).
7. **수렴점 통과** — transition(alarm) / record_event(event, detected_by=self 부여).
8. **동기화** — FM_REGISTER `active[]`·FM_SYNC 로 reconcile(§9).

L1 수신부(`handlers/agent_api.py _metric`)는 `module_events` 를 event_log 로 기록하고
관측 필드를 저장한다 — **metric 필드는 수신 화이트리스트 등재가 계약의 일부**다(미등재
필드는 무성 폐기 — agent_api.md 주의 사항). rule 평가는 sweeper 주기에서 수행한다.

## 6. 보관 (Storage·Retention)

### 6.1 스트림 구조 — SoT 규정

| 저장물 | 경로 | 내용 | SoT 성격 |
|---|---|---|---|
| 알람 스트림 | `{ServiceLogDir}/alerts/YYYY/MM/DD.jsonl` | action=open/close/change/ack/comment 레코드(스키마 = 표준화 §3.2) | **알람 이력의 SoT.** 활성 알람 상태는 스트림 replay 의 파생물 |
| 이벤트 스트림 | `{ServiceLogDir}/events/YYYY/MM/DD.jsonl` | `{ts,type,kind,source,message,params}` | 이벤트 이력의 SoT |
| 모듈 카탈로그 보존 | `{ServiceLogDir}/fm_catalog/<node>.json` | 노드별 마지막 FM_REGISTER 카탈로그(원자적 교체) | 모듈 다운 중에도 `/alerts/catalog` 병합·복원용 |

- DB 테이블은 두지 않는다(현행 유지) — 스트림은 공유 서비스 로그 영역(`ServiceLogDir`,
  그룹 공유 스토리지 — oam_ha)에 있어 OAM 절체 후에도 연속된다. 관리 store(file_store)를
  쓰지 않는 이유는 self_reporting §4(단일 writer 리스와 쓰기 소유 충돌).
- 일별 파일 분리가 회전 단위다.

### 6.2 보존 정책 (확정 — 현행 공백 해소)

현행은 alerts/events 에 purge 가 없다(무한 증가 — metric 만 purge). 다음으로 확정한다:

- **보존 스위퍼 신설**(일 1회): 파일 날짜 기준으로 알람 스트림 **180일**, 이벤트 스트림
  **365일**(감사 요건 — service_control/auth_audit 포함) 초과 일자 파일 삭제.
- 설정: `ServiceLogging.{AlertRetainDays: 180, EventRetainDays: 365}` (0 = 무제한).
- fm_catalog 는 노드당 최신 1본이라 보존 대상 아님. 조회 API 의 `days` 클램프(≤90)는
  조회 상한일 뿐 보존과 무관하다.

### 6.3 복원 — "replay 는 시드, 수렴은 재평가" (원칙)

OAM 재기동 시 `restore_open_state`(30일 replay, detected_by 파티션별)로 활성 상태를
**시드**하고, 진실 수렴은 각 파티션의 발행 주체가 보장한다 — self 는 FM_SYNC, L1/L3/drift
는 다음 스윕 재평가. 따라서 replay 윈도(30일)를 넘겨 열려 있던 알람도 발행 주체의 다음
동기화/재평가가 다시 open 한다(replay 는 재기동 직후의 공백을 줄이는 장치이지 정합의
근거가 아니다). 열린 알람이 있는 채로 스트림 파일을 삭제하면 시드가 깨진다 — 보존
스위퍼(§6.2)의 윈도가 replay 윈도보다 충분히 큰 이유.

## 7. 조회 API (계약)

베이스 `/api/v1` (구현: `handlers/alerts.py`·`events.py`):

| 엔드포인트 | 파라미터 | 반환 |
|---|---|---|
| `GET /alerts` | `days`(1~90)·`type`·`limit`(≤5000) | 알람 레코드 desc |
| `GET /alerts/summary` | `days`(1~90) | by_type 집계 + daily (ack/comment 미집계) |
| `GET /alerts/types` · `/alerts/rules` | — | type 목록 / 평가 규칙(read-only) |
| `GET /alerts/catalog` | — | 코드 카탈로그 — rule(origin=rule) + 모듈 등록분(origin=module:*) 병합 |
| `POST /alerts/ack` · `/alerts/comment` | `{alarm_id}` / `{alarm_id, text≤500}` | 부가 레코드 append (32.111 ack/setComment) |
| `GET /events` | `days`·`type`·`kind`·`limit` | 이벤트 레코드 desc |
| `GET /events/types` | — | 이벤트 type 목록 |

**인증 (확정 — 현행 공백 해소)**: 현행 alerts/events 핸들러는 무인증이다(토큰 없이 조회·
ack·comment 가능 — ack 사용자만 best-effort 조회 후 `operator` 폴백). 다음으로 확정한다:
**전 엔드포인트 `require_auth` 적용**, ack/comment 는 **토큰의 actor 를 필수**로 기록한다
(폴백 기명 제거 — X.740 감사추적은 주체 불명을 허용하지 않는다). 콘솔은 이미 토큰을
보내므로 화면 영향 없다.

## 8. 가시화 (콘솔 계약)

### 8.1 뷰 구성

| 뷰 | 라우트/위젯 | 소비 API | 내용 |
|---|---|---|---|
| 활성 알람 | `/alerts/active` | `/alerts`(openOnly 접기) | 열린 알람 목록 — ack/comment 조작 |
| 알람·이벤트 이력 | `/alerts/history` | `/alerts`·`/events` | 알람 탭 + 이벤트 탭(type/kind 필터) — 스트림을 화면에서도 분리(표준화 §3.6) |
| 활성 알람 위젯 | `cims.active-alarms`(기본 대시보드 최상단) | `/alerts` | `computeActive` 접기 재사용 |
| 배너 | AlertBanner(활성 위젯에 흡수) | `/alerts` | critical/major 만 |
| 토폴로지 상태색 | SystemTopologyWidget | `/alerts` | mo_instance 별 최고 severity 로 노드/모듈 칩 채색 |
| **알람 카탈로그(신설)** | 관리 화면 1식 | `/alerts/catalog` | 코드 사전 — code·type·severity·effect·recommended_action 열람(운영 사전·POD 의 화면 대응물). API 는 기성이나 UI 미노출 — 이행 대상 |

### 8.2 전역 통지 — 셸 상주 (어느 페이지에서든 보인다)

알람 표시는 페이지가 아니라 **콘솔 셸(레이아웃) 소유**다 — 라우트 전환과 무관하게 상주한다.
운영자가 어느 화면에 있든 신규 critical/major 를 놓치지 않는 것이 목적(NOC 알람 배너 관례).

1. **전역 알람 store (구독 1원화)** — 셸 레벨 Provider 가 `/alerts` 를 주기 폴링(10s —
   스윕 30s·FM push 실시간의 절충)하고 §8.3 접기 규율로 활성 상태를 유지한다. 위젯
   (활성 알람·배너·토폴로지)도 개별 fetch 를 버리고 이 store 를 구독한다 — 표시 일관성 +
   요청 수 절감.
2. **헤더 상시 인디케이터** — 활성 요약 배지(최고 severity 색 + 건수). 클릭 시 드로어
   (활성 알람 목록 + 최근 이벤트 탭 — ack/이동 가능). **0건이어도 회색 배지를 상시
   표시한다** — "표시 없음 = 정상"과 "표시 없음 = 표시 고장"을 구분(observability_lost 와
   같은 철학).
3. **전이 토스트** — store 가 신규 발생을 감지하면 팝업. 소음 통제 규칙:

| 대상 | 통지 |
|---|---|
| 알람 open — critical/major (change moreSevere 로의 승격 포함) | 토스트(수동 닫기) + 선택: 경고음·브라우저 Notification |
| 알람 open — minor 이하 / close | 배지 갱신만 |
| 이벤트 | 토스트 없음 — 드로어 "최근 이벤트" 카운트만 (정상 동작 통지를 알람 채널로 팝업하지 않는다 — §3.6 스트림 분리의 표시단 유지) |

- **신규 판정은 alarm_id 기준 high-water mark** — 폴링 중복·재통지(×N)를 새 알람으로
  오인하지 않는다. 토스트/배지도 §8.3 판독 규율(접기·미지 action 무시)을 공유한다.
- **전송 단계**: P0 = 위 폴링 구조(서버 무변경). P1 = SSE `GET /alerts/stream` — 수렴점
  (§4.2)에 구독자 hook 을 달아 push, 폴링은 fallback 유지(재연결·프록시 대비). 단방향
  통지라 WebSocket 은 두지 않는다. OAM httpsrv 의 long-lived 응답 지원 확인이 선행.

### 8.3 판독 규율 (표시단 공통)

- **활성 판정은 스트림 접기**: akey(alarm_id 의 `@epoch` 제거)별로 open→활성, close→해소,
  change→현재 severity/값 갱신(새 행 ❌ close 오인 ❌), ack/comment→활성 행에 누적 표시,
  **미지 action 은 무시**(전방 호환). 연속 open 은 기존 행 갱신 + `×N` 배지(표준화 §3.4(d)).
- severity 6단계 색 체계(critical/major/minor/warning/indeterminate/cleared) 고정 —
  구 2색(critical/warning)은 하위호환 읽기만.
- 레코드의 `message` 는 수집측 렌더 결과를 그대로 표시한다(표시단 재조립 금지 —
  전 화면 동일 문자열).

## 9. 정합·수렴 (장애 시 수렴 경로)

| 상황 | 수렴 경로 |
|---|---|
| FM_ALARM/close 유실(5회 재전송 소진) | 다음 FM_SYNC(60s)가 활성 목록 대조로 open/close 복구 |
| 모듈 재기동 | boot_id 변경 감지 → 첫 FM_SYNC 로 reconcile(소멸 조건은 close, 지속 조건은 새 occurrence) |
| FM_SYNC 3회 연속 두절 | 해당 노드 self 알람 "판정 불가" 종결 — 생존·응답성은 L1/L3 가 별도 판정 |
| OAM 재기동/절체 | restore_open_state 시드(§6.3) + 파티션별 재평가/sync 수렴. 스트림은 공유 스토리지라 절체 무손실 |
| **agent 관측 두절** | **해당 노드 agent 파티션 알람을 "판정 불가" 종결하고 node 두절 알람(connection_lost, 대상 AGENT)을 연다** — 현행의 "agent offline 시 그 노드 열린 알람 자동 close"(노드 사망이 전 알람 해소로 위장되는 침묵 실패 — 모듈 카탈로그 OAM 절)는 이 규율로 정정한다 |
| 시각 정합 | 전 구간 ts 는 NTP 동기 전제 — 시각 이상은 그 자체가 알람(A-QOS-003/A-COM-014) |

## 10. 구현 이행 (현행 대비 갭)

카탈로그 구현 이행(기능 카탈로그 §6: 감지 주체 배정 → 선행 구현 → 모듈 카탈로그 등록 →
fm_catalog/rule)과 별개로, 파이프라인 자체의 본 정본 대비 갭:

1. **정의 코드 이행** — wire·저장·rule·fm_catalog.json 의 구 포맷(`CIMS-<DOMAIN>-<SEQ>`
    클래스 코드)을 flat 정의 코드(`A-<DOMAIN>-NNN`)로 — `_CODE_REVISIONS` alias + 스윕
   이행 종결(표준화 §3.4(a) 번호 승계 표). `storage_failure` 개명(구 `resource_failure`) 동반.
   **mo 루트 개편**(`cims/...` → 서버명/그룹명 소유 주체 루트, 표준화 §3.4(b)·§6)도 같은
   이행에서 수행 — 활성키가 함께 바뀌므로 두 번 나누지 않는다.
2. **API 인증**(§7) — alerts/events 전 엔드포인트 require_auth + ack/comment actor 필수.
3. **보존 스위퍼**(§6.2) — alerts 180일/events 365일 purge + 설정 키.
4. **agent 두절 규율**(§9) — 자동 close 를 판정 불가 종결 + node 두절 알람으로 정정.
5. **알람 카탈로그 화면**(§8.1) — `/alerts/catalog` 소비 UI 신설.
6. **전역 통지**(§8.2) — 셸 상주 store/인디케이터/토스트 (P0 폴링) + 위젯 fetch 의 store
   구독 전환. SSE 스트림은 P1.
7. 구현 노트: FmReporter.cpp 3벌 동일 복제(csp/cmp/cmdp)는 공통화 후보,
   `read_recent` 의 전량 적재는 보존 기간 확대 시 병목 후보 — 계약 무관, 이행 시 함께 검토.

## 관련

- [alarm_standardization.md](alarm_standardization.md) — 알람 모델 정본
- [alarm_self_reporting.md](alarm_self_reporting.md) — L2 자기보고 wire 정본
- [alarm_function_catalog.md](alarm_function_catalog.md) — 요구 카탈로그 / [alarm_module_catalog.md](alarm_module_catalog.md) — 구현 추적
- [../api/agent_api.md](../api/agent_api.md) — L1 agent↔OAM API / [../api/cmp_media_api.md](../api/cmp_media_api.md) — envelope v2
- [features/oam_base_service_split.md](features/oam_base_service_split.md) — 소유 분리 / [features/oam_ha.md](features/oam_ha.md) — 관리평면 VIP·공유 스토리지
- 3GPP TS 32.111-2 · RFC 3877 · ITU-T X.733/X.730/X.731/X.740
