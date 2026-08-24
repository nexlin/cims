# 모듈 알람/이벤트 자기보고 (FM Push) 설계

OAM 의 외부 관측(CSP/CMP UDP probe·DB SELECT·agent metric) 폴링 파생 알람만으로는
프로세스가 살아 있는 한 모듈 내부 이상(호처리 실패·자원 고갈·피어 두절)이 알람이 될 수 없다.
본 문서는 모듈이 자기 알람/이벤트를 OAM 으로 **능동 보고(push)** 하는 경로의 정본이다.
알람 모델(X.733 속성·code 체계·활성키·재통지)은
[alarm_standardization.md](alarm_standardization.md) 를 그대로 따르며, 여기서는 **발생 경로와
wire 규격**을 정의한다. 구현: OAM 수신 = `ems/core/oam/src/services/fm_ingest.py`,
발신 = csp/cmp/cmdp 공용 `include/FmReporter.h` + `csc/src/services/fm_reporter.py` (§7).

## 1. 원칙

- **보완이지 대체가 아니다.** 죽은 프로세스는 자기보고를 못 한다 — 감지 3계층(표준화 §3.4(b))
  중 프로세스 생존(L1 — agent 관측 process_down)·서비스 응답성(L3 — OAM probe
  process_unresponsive)·호스트 자원(agent metric)은 기존 관측을 유지한다. 자기보고는
  "프로세스 생존 ≠ 정상"(표준화 §3.6)의 나머지 절반, 즉 **모듈 내부 상태(L2)** 를 채운다.
- **표준 근거**: 3GPP TS 32.111-2 Alarm IRP 의 notification push(NE→Manager) +
  alarm synchronization. 카탈로그(alarmModel)와 발생(alarmActive)의 분리는 RFC 3877 구조 —
  통지에는 (code, mo_instance, params)만 싣고 클래스 속성은 카탈로그가 보유한다.
- **알람과 이벤트는 명령부터 분리**한다(FM_ALARM/FM_EVENT) — 표준화 §3.6 (X.733 알람 vs
  X.730/731/740 통지). 정상 동작(기동/설정 재적재/감사)은 이벤트 스트림으로만 흐른다.
- **활성 알람의 SoT 는 모듈**이고 OAM 은 미러다. 통지 유실·양측 재기동은 주기 동기화(FM_SYNC)로
  수렴한다 — 순간 정합보다 **수렴 보장**을 계약으로 한다.

## 2. 아키텍처

```
CSP ──┐
CMP ──┤  UDP JSON envelope v2 (FM_*)          ┌ alert_log JSONL (알람 — 기존 스트림 합류)
CMDP ─┼──────────→ OAM FM ingest (:9010) ─────┤
CSC ──┘            oam-svc 소유               └ event_log JSONL (이벤트 — 신규 스트림)
```

- **서버**: OAM. `FmIngest.{Ip,Port,SyncSec}`(기본 0.0.0.0:9010, sync 60s — oam-svc
  config_template) UDP 단일 포트. 소유는 **oam-svc**(서비스 관측 소유 —
  oam_base_service_split §4 정합)이며 `--role all` 단일 프로세스에서는 base 가 대행한다 —
  sweeper 소유 규약과 동일.
- **클라이언트**: 각 서비스 모듈(csp/cmp/cmdp/csc). envelope `hdr.node` 논리 노드 ID 로
  식별([cmp_media_api.md](../api/cmp_media_api.md) §1.1 과 동일 모델). OAM 이중화(oam_ha.md)
  환경에서는 관리평면 VIP 로 전송 — 절체 후에도 목적지 불변.
- **agent 는 편입하지 않는다** — agent 계열(disk/module/config/HA)은 기존 원시 metric 보고 →
  base 평가 경로 유지. L1 의 `process_died` 이벤트(표준화 §3.4(b))도 metric 에 동반된 전이
  관측을 OAM 이 event_log 로 기록하는 것이지 FM push 가 아니다.
- 보안 신뢰 모델은 CMP media API(:9001 계열)와 동일 — 내부 제어평면 평문 UDP.

## 3. Wire 규격 — envelope v2 / FM function

[cmp_media_api.md](../api/cmp_media_api.md) 의 envelope v2 를 그대로 쓴다. 방향만 반대
(모듈=발신 client, OAM=수신 서버). `type:"event"` 의 신뢰성 규칙(§8: ack=동일 trans_id 의
`type:"response"`, 미수신 시 1s 간격 최대 5회 재전송)이 그대로 적용된다 — CMDP 이벤트 채널이
실구현 선례.

| cmd | type | 방향 | 용도 |
|---|---|---|---|
| `FM_REGISTER` | event | 모듈→OAM | 기동 시 카탈로그 등록 + boot_id 통지 (+초기 활성목록) |
| `FM_ALARM` | event | 모듈→OAM | 알람 open/close 실시간 통지 |
| `FM_EVENT` | event | 모듈→OAM | 정상 동작 이벤트(stateChange/audit) 통지 |
| `FM_SYNC` | event | 모듈→OAM | 활성 알람 전량 동기화 (주기 — 32.111 alarm sync 대응) |

hdr 는 `{ver:2, trans_id, node, cmd, type:"event", service:"cims"}`. 호 문맥이 아니므로
`sesid` 는 생략한다.

### 3.1 payload

**FM_REGISTER** — 기동 시 1회(및 OAM 요구 시 재송):
```jsonc
{ "boot_id": 1754805000,          // 기동 epoch — 재기동 감지 키
  "module": "csp",
  "catalog": { "alarms": [ /* §4 */ ], "events": [ /* §4 */ ] },
  "active": [] }                  // 초기 활성목록 (= 첫 FM_SYNC 겸용)
```

**FM_ALARM** — 상태 전이 시:
```jsonc
{ "boot_id": 1754805000, "seq": 17,
  "action": "open",                        // open | close
  "code": "A-COM-001", "type": "connection_lost",
  "mo_instance": "<node>/csp/db",          // code@mo_instance = 활성키 (표준화 §3.4 — 서버명 루트)
  "params": { "used": 20, "total": 20 },   // 카탈로그 msg 템플릿 치환 값
  "perceived_severity": "major",           // (선택) 카탈로그 기본 덮기
  "message": "...",                        // (선택) 렌더 결과 직접 지정
  "ts": "2026-08-10T09:31:05" }
```
- 활성키·alarm_id·재통지 의미는 표준화 §3.4 그대로 — alarm_id 는 OAM 이 발급하므로 wire 에
  싣지 않는다. 메시지는 통상 OAM 이 카탈로그의 msg_open/msg_close 를 params 로 렌더한다
  (sweeper 규칙과 동일 관례 — 콘솔 표기 일관성).

**FM_EVENT**:
```jsonc
{ "boot_id": 1754805000, "seq": 18,
  "type": "config_reloaded", "kind": "stateChange",   // kind: stateChange | audit
  "mo_instance": "<node>/csp", "params": { "rev": "r12" }, "ts": "..." }
```

**FM_SYNC** — 주기(기본 60s, FM_REGISTER 응답으로 OAM 이 지시):
```jsonc
{ "boot_id": 1754805000, "seq": 19,
  "active": [ { "code": "A-COM-001", "mo_instance": "<node>/csp/db",
                "open_ts": "2026-08-10T09:31:05" } ] }
```

### 3.2 신뢰성·순서

- **재전송/중복**: ack 미수신 시 1s×5 재전송(envelope §8). OAM 은 `(node, trans_id)` LRU 로
  재전송 중복을 폐기한다.
- **순서**: `seq` 는 boot 당 단조증가. OAM 은 활성키별 마지막 처리 seq 를 기억하고 더 작은
  seq 의 open/close 는 폐기한다(UDP 역전 방지). 이벤트는 순서 무관(append).
- **유실 수렴**: FM_ALARM 이 5회 재전송 후 폐기돼도 모듈 활성목록에는 남으므로 다음 FM_SYNC 가
  open 을 복구한다. close 유실도 sync 가 정리한다. 이벤트는 best-effort(P0 — 유실 허용,
  필요 시 P1 에서 모듈측 ring buffer 재송).
- **패킷 상한**: FM 채널 datagram 은 발신 32KB(`FmReporter` `kFmMaxPacket`, csc
  `fm_reporter.py`) / OAM 수신 64KB(`fm_ingest`) — FM_REGISTER 가 카탈로그 전문을 실으므로
  CMP 미디어 채널의 4KB(envelope §1.2)로는 알람 수 종부터 등록이 영구 실패한다. FM_SYNC
  active 배열도 같은 상한. 배포 스큐 주의: 4KB 초과 카탈로그는 수신측(OAM)을 먼저 올려야
  등록된다.

## 4. 카탈로그 — 모듈이 코드 옆에 선언

[api_docs.md](features/api_docs.md)("모듈이 코드 옆에 자기 API 선언") 패턴과 동형. 각 모듈이
`fm_catalog.json` 을 소스 옆에 두고(예: `csp/config/fm_catalog.json` — dist 의 `config/` 로
설치), 기동 시 FM_REGISTER 로 등록한다. OAM 은 `{ServiceLogDir}/fm_catalog/<node>.json` 에
마지막 카탈로그를 보존해(모듈 다운/절체 중에도) `GET /alerts/catalog` 에 origin=module 로
병합한다. 보존소가 관리 store(file_store)가 아닌 이유: 관리 store 는 소유권 리스(oam_ha §4.4)를
가진 base 단일 writer 라 oam-svc(FM ingest 소유자)가 쓸 수 없다 — alert/event 스트림과 같은
공유 서비스 로그 영역이 쓰기 소유·복제 범위 모두 정합적이다.

```jsonc
{ "alarms": [ {
    "type": "connection_lost", "code": "A-COM-001",   // 정의 코드 — 표준화 §3.4(a)
    "perceived_severity": "major", "event_type": "communications",
    "probable_cause": "underlyingResourceUnavailable", "mo_class": "service",
    "msg_open": "{mo} DB 연결 풀 고갈 ({used}/{total})", "msg_close": "{mo} DB 연결 정상화",
    "effect": "가입자 조회/과금 기능 저하", "recommended_action": "DB 상태·연결수 확인" } ],
  "events": [ { "type": "config_reloaded", "kind": "stateChange",
                "msg": "{mo} 설정 재적재 ({rev})" } ] }
```

- **같은 정의는 기존 code 재사용**(connection_lost(DB)=A-COM-001 등) — 객체는 mo_instance 로
  구분한다(§3.5: 새 객체 추가 = 코드 신설 없음). 새 *조건* 클래스만 코드를 신설한다.
- sweeper 가 발화 중인 `code@mo` 공간과의 충돌은 등록 시 검증해 거부한다.
- `mo_instance` 규칙: `<서버명>/<module>[/<component>]` — **서버명(= envelope `hdr.node`)
  루트, 알람·이벤트 공통** (표준화 §3.4(b) 소유 주체 루트 규약). detected_by 가 주체 클래스만
  담으므로 발생 노드는 mo_instance 가 유일 보유자다. 구 wire 형식
  (`cims/<module>/<node>[/<component>]`, 코드 `CIMS-*`)은 OAM ingest 가 수신 시 현행
  형식으로 정규화해 흡수한다(배포 스큐 — 표준화 §6).
- `detected_by`: **`self`** (주체 클래스 — 표준화 §3.4(b) 감지 3계층의 L2). 발신 노드는
  envelope `hdr.node`(wire)와 mo_instance 가 보유하므로 접미로 중복하지 않는다.
- `perceived_severity` 는 통지 payload → 카탈로그 순으로 취하고, 둘 다 없으면
  **indeterminate** 로 발화한다 (X.733 — 미지정을 warning 으로 임의 판정하지 않음).
- 활성 알람에 severity 가 달라진 open 재통지가 오면 **action=change** 로 기록된다
  (표준화 §3.4(d) notifyChangedAlarm — transition 코어 공통 동작). 단계 임계 자기보고가
  이 경로를 쓴다: FmReporter 는 같은 (code, mo) 라도 severity 가 다르면 재통지하고,
  FM_SYNC/FM_REGISTER 의 active 항목에도 `perceived_severity` 를 실어 유실·재기동 후
  reconcile 시 현재 단계가 보존된다.
- 자기보고 클래스의 임계/발화 조건은 **모듈 설정 소유** — 콘솔 카탈로그에는 read-only 로 노출
  (descriptor `alert_rules` 는 OAM 평가 규칙 전용으로 유지, 혼합하지 않음).

**자기보고 클래스 카탈로그** (모듈별 fm_catalog.json 이 선언하는 현행 집합):

| code | type(클래스) | eventType | 사용처 |
|---|---|---|---|
| `A-COM-001` (재사용) | connection_lost | communications | CSP·CSC 의 DB 연결 두절 (`<서버명>/<mod>/db`) |
| `A-QOS-002` | resource_exhausted | qualityOfService | CMP 자원 풀 완전 고갈 — rtp/ptt_floor/ptt_member (`<서버명>/cmp/<pool>`). 사용률 임계는 OAM sweeper(A-QOS-024, 노드별) 담당 — 역할 분담 |
| `A-PRC-002` | storage_failure | processingError | CMDP FD 스토어 저장 실패 (`<서버명>/cmdp/fd_store`). 후보: 녹취 쓰기 실패 |
| `A-QOS-006/007/009/011` | threshold_crossed | qualityOfService | CSP SIP 신호 통계 — 호/등록 성공률·신규 INVITE CPS·SIP 수신 이상 (`<서버명>/csp/{calls/success_rate, reg/success_rate, cps, sip/rx_error}`). 윈도우/단계 임계는 `Setup.SipStats.*`(모듈 설정 소유), 통지가 `perceived_severity` 를 동반 — 단계 승격/완화는 open 재통지(action=change) |
| `A-SEC-003` | security_violation | securityServiceOrMechanismViolation (X.736) | CSP 채널 정책(TLS 강제) 위반 반복 — 게이트 403 건수의 윈도우 급증 (`<서버명>/csp/channel_policy`, 소스 IP 동반). 임계는 `Setup.SipStats.ChannelPolicyMajor`(기본 10, 0=off), 단일 단계 major — 평가는 SipStatsMonitor 동일 윈도우 |

새 *조건* 클래스 후보(미구현): `overload`(CSP 제어평면 과부하 차단 발동 — 차단 로직 자체가
미구현), 포트 bind 실패(기동 실패 = 프로세스 사망 → L1 process_down(agent) 소관, §1 원칙).

**이벤트**: `process_started`/`process_stopping`(stateChange — cmp/cmdp/csc 는 SIGTERM
graceful stop 핸들러가 이때 신설됨) · `service_control`(audit — OAM 서비스 제어, event_log
직접 기록) · `config_change`(audit — csc `audit_config_change` 가 FM_EVENT 로 발신).
구 감사 JSONL 2종(`service_control_audit`, `csp_config_audit`)은 event_log 로 흡수 완료.

## 5. 라이프사이클

- open/close/재통지 의미는 표준화 §3.4 그대로 — OAM ingest 가 `alarm_sweeper.transition`
  코어를 재사용해 akey=(code@mo_instance)로 alarm_id 를 발급/종결한다.
- **모듈 재기동**: boot_id 변경 감지 → 해당 node 의 self 활성 알람을 첫 FM_SYNC(빈 목록 가능)
  reconcile 로 정리한다. 재기동으로 조건이 소멸했으면 sync 가 닫고, 지속 중이면 모듈이 다시
  open 을 싣는다(새 occurrence).
- **관측 두절**: FM_SYNC 3회 연속 누락 시 해당 node 의 self 활성 알람을 "판정 불가" 사유로
  close 한다(표준화 §3.4(d), drift 스위퍼 3회 임계와 동일 관례). 프로세스 생존은 L1(agent
  process_down)이, 서비스 응답성은 L3(process_unresponsive)가 별도 판정한다.
- **OAM 재기동**: alert_log replay(restore_open_state)로 복원한다. self 계열 복원을 위해
  `compute_open_state` 가 detected_by 를 함께 반환하도록 확장하고, base/oam-svc 소유 분리
  scope 를 mo-prefix 기반에서 **detected_by 기반**으로 일반화한다. 이후 sync 로 수렴.

## 6. 저장·API·콘솔

- **알람**: 기존 alert_log JSONL 스트림에 합류 — 레코드 스키마 동일(표준화 §3.2),
  detected_by 만 `self`. AlertsPage/활성 위젯/ack 이 무변경으로 동작한다.
- **이벤트**: 신규 event_log `{ServiceLogDir}/events/YYYY/MM/DD.jsonl` — alert_log 의 일별
  JSONL 헬퍼를 공용화해 재사용. 레코드:
  `{ts, type, kind, source{mo_class, mo_instance, detected_by}, message, params}`.
- **API**: `GET /events`(days/type/kind 필터) · `GET /events/types` 신설.
  `GET /alerts/catalog` 에 모듈 등록 카탈로그 병합(origin 표기).
- **UI**: AlertsPage 에 "이벤트" 탭 신설 — 라우트 제목 "알람·이벤트 이력"이 비로소 사실이
  된다. 알람·이벤트는 표시단에서도 스트림을 구분한다(통합 타임라인은 후속 과제).

## 7. 구현 지점

1. **OAM 수신**: `ems/core/oam/src/services/fm_ingest.py` — UDP 서버 스레드,
   (node, trans_id) 응답 캐시 dedup, akey 별 seq 역전 폐기, FM_SYNC reconcile,
   `alarm_sweeper.transition` 코어 재사용, sync 두절 판정 불가 종결.
   배선: `oam_svc_app.py`(소유) + `oam_app.py`(`--role all` 대행). config
   `FmIngest{Ip,Port,SyncSec}` (oam-svc config_template).
2. **저장 코어**: `services/daily_jsonl.py`(일별 JSONL 공용) 위에 `alert_log`(alerts/)와
   `event_log`(events/)가 얹힘. `alert_log.compute_open_state(with_meta=True)` 가
   detected_by 를 동반 반환 — `restore_open_state` scope 가 자기보고 계열(detected_by=`self`,
   구 레코드 `self:*` 포함)을 sweeper 소유에서 제외한다.
3. **handlers**: `GET /events`·`/events/types` (`handlers/events.py`), `/alerts/catalog`
   모듈 카탈로그 병합(origin=rule|module:<module>). OAM 서비스 제어 감사
   (`service_control.py`)는 event_log 에 직접 기록(type=service_control, kind=audit).
4. **모듈 발신** — C++ 는 공용 header-only `include/FmReporter.h` (csp/cmp/cmdp 공유 —
   모듈별 로거(CLog/PLog) 차이는 `Init` 의 로그 콜백 주입으로 흡수), Python 은
   `csc/src/services/fm_reporter.py`. 공통: envelope 송신,
   boot_id/seq, ack 대기·1s×5 재전송 큐, sync 타이머, UNREGISTERED 재등록, **미등록 구간
   이벤트 버퍼(32건, 등록 시 flush)** — 부트 순서상 모듈이 OAM 보다 먼저 떠도
   process_started 가 유실되지 않는다. 각 모듈 설정은 config_template `fm` 섹션
   (`Fm.{Enable,OamIp,OamPort,SyncSec}`, csp 만 `Setup.Fm.*`; OamIp 는 `@OAM_IP@` 치환).
   - **csp**: DB 연결 probe(`CDbManager::StartHealthProbe` — 전용 연결 mysql_ping 10s,
     3연속 실패 전이) → A-COM-001 `<node>/csp/db`. SIP 신호 통계는 psip 스택
     카운터(`CSipStackCounter` — 수신 요청·최종응답 수신/송신·파싱 실패, 로컬 합성
     408/660 포함)를 `SipStatsMonitor`(메인 루프 tick)가 `Setup.SipStats.EvalSec` 윈도우로
     차분 평가 → A-QOS-006/007/009/011 (단계 severity 동반, monitor 명령 `sip_stats`).
   - **cmp**: `fmMonitorLoop`(1s) 가 3개 풀(rtp/ptt_floor/ptt_member)의 완전 고갈 전이를
     감시 → A-QOS-002 `<node>/cmp/<pool>`. 변종(pmp/imp)은 SystemId overlay 로 node
     분리 필수 (미분리 시 활성키 충돌).
   - **cmdp**: FD 스토어 저장 실패/성공 전이 → A-PRC-002 `<node>/cmdp/fd_store`.
   - **csc**: DB probe(`DbHealthProbe` — pymysql 전용 연결, csp 동형) → COM-001
     `<node>/csc/db`. `audit_config_change` 는 FM_EVENT(kind=audit, `<node>/csc/config/<entity>`) 발신.
   - cmp/cmdp/csc 는 SIGTERM graceful stop 을 신설해 `process_stopping` 을 통지한다.
     한계: 전 스택 동시 정지에서 OAM 이 먼저 내려간 뒤의 종료 이벤트는 유실(수신자 부재).
5. **콘솔**: AlertsPage 알람/이벤트 탭(`EventsSection`), `api/alerts.ts` `eventsApi`.
   AlertBannerWidget 은 `/alerts` 활성 critical/major 를 소비(자체 임계 판정 제거,
   `ActiveAlarmsWidget.computeActive` 공유).
6. **drift_sweeper**: HA fan-out drift 를 표준 알람(A-PRC-003,
   `<그룹명>/config/<collection>`, transition 코어)으로 발화. 구 포맷
   (`config_drift::…`·구 code·구 mo `cims/ha/…`) open 은 스윕에서 이행 종결 후
   현행 알람으로 재발행.

## 8. 향후 단계

- **P2 — northbound**: 표준화 §7.3 그대로 — self 알람도 동일 code 매핑으로 자동 편입.

## 9. 잔여 논점

- CMP 녹취 쓰기 실패(PRC-002) 훅 — fwrite 반환값 미검사 보강과 함께.
- pmp/imp 변종 배포 시 SystemId overlay 자동 주입(배포기 통합).

## 관련

- [alarm_standardization.md](alarm_standardization.md) — 알람 모델 정본 (X.733 속성·code
  체계·활성키·재통지·이행)
- [alarm_pipeline.md](alarm_pipeline.md) — 발생→전달→수집/보관→가시화 전 구간 절차 정본
  (본 문서는 그중 L2 wire 구간)
- [alarm_catalog.md](alarm_catalog.md) — 알람/이벤트 카탈로그 (감지 행 = 모듈별 자기감지
  가능 조건 전수 — 현행 fm_catalog 대비 누락 알람/이벤트 후보)
- [../api/cmp_media_api.md](../api/cmp_media_api.md) — envelope v2·이벤트 채널 신뢰성 규칙
- [features/oam_base_service_split.md](features/oam_base_service_split.md) — 소유 분리 /
  [features/oam_ha.md](features/oam_ha.md) — 관리평면 VIP
- [features/api_docs.md](features/api_docs.md) — "코드 옆 선언" 패턴 선례
- 3GPP TS 32.111-2 (Alarm IRP notification/sync) · RFC 3877 (alarmModel/alarmActive 분리) ·
  ITU-T X.730/X.731/X.740 (통지)
