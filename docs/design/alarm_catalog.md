# 알람/이벤트 카탈로그 — 설명서

목록 정본은 **[alarm_catalog.csv](alarm_catalog.csv)** 이고, 본 문서는 그 범위·컬럼 정의·
수록/판정 원칙·기능별/모듈별 설계 메모를 기술한다. 목록 자체는 여기에 중복하지 않는다.

CSV 는 두 축을 한 파일에 담는다 (`행` 컬럼 — §2):

- **정의 행** (`행=정의`) — IMS/MC 기능(CSCF·IBCF·TAS·PTT-AS·MRF·CSC)과 관리평면(`관리` —
  OAM/Agent)이 **현재 구현 여부와 무관하게** 운영 관점에서 갖춰야 할 알람/이벤트의
  **요구(요건) 정본**이자, 정의 코드의 **단일 채번 정본**(표준화 §3.4(a) — 관리평면 포함
  전 정의가 여기서 코드를 받는다).
- **감지 행** (`행=감지`) — 각 모듈이 **스스로 인지(감지)해 발생시킬 수 있는** 알람/이벤트의
  전수 열거. 용도는 ① 현행 감지 대비 **누락 알람 후보의 식별** ② 운영/NMS 연동용 알람
  사전(dictionary)의 원천 ③ 감지 주체·코드 근거·**구현 현황 추적**.

## 1. 문서 가족에서의 위치

| 문서 | 축 | 담는 것 |
|---|---|---|
| [alarm_standardization.md](alarm_standardization.md) | 모델 | 알람/이벤트 모델·severity·code 체계·감지 3계층·벤치마크 대조(§7.2 vIBCF) |
| **본 카탈로그** | **요구(what) + 구현(how·현황)** | 정의 행 = 기능 관점 필요 알람/이벤트(구현 무관·채번 정본) / 감지 행 = 모듈 자기감지 전수 목록(감지 주체·구현 추적) |
| [alarm_self_reporting.md](alarm_self_reporting.md) | 경로 | 자기보고(FM push)·이벤트 스트림 wire/저장 규격 |
| [alarm_pipeline.md](alarm_pipeline.md) | 절차(end-to-end) | 발생→전달→수집/보관→가시화 구간별 계약·수렴 보장 |

요구 항목이 구현 채택되면: 감지 주체(L1/L2/L3)를 배정하고 그 정의 아래에 감지 행이 생기며
(선행 구현 포함), fm_catalog/rule 로 구현된다 (§11) — 정의 행은 그 기준선으로 유지한다.

### 1.1 기능(역할) ↔ CIMS 실체 매핑

기능 축이 역할 관점인 이유는 CIMS 모듈이 IMS 노드 역할을 겸장하기 때문이다. **역할 명칭은
`기능` 컬럼(절)에만 쓰고, `대상`/`component` 등 실체 지시는 CIMS 모듈/객체 명칭을 쓴다.**

| 역할 (기능 축) | CIMS 실체 |
|---|---|
| CSCF · IBCF · TAS · PTT-AS | **CSP** |
| CSC — HSS(가입자 원천)·IdMS/GMS/CMS/XCAP (MC 관리 서버, TS 23.280/23.379 기능 요소) | **CSC** (+ 서비스 DB) |
| MRF (미디어 릴레이·floor·믹스·녹취 + MCData 미디어) | **CMP** (+ CMDP) |
| 관리 (배포·프로비저닝·수집/보관·노드 관리·스토어 펜싱) | **OAM + Agent** |

## 2. 카탈로그 구조 — 정의 행과 감지 행

- **정의 1 : 감지 N.** 정의 행은 code 당 1행(요구 마스터), 감지 행은 그 정의를 감지·발화하는
  (모듈, 관측 경로) 인스턴스마다 1행이다 — 같은 code 의 감지 행이 여럿일 수 있다
  (예: `A-PRC-018` worker 정지를 CSP/CMP/CMDP/CSC/OAM 이 각자 감지).
- **배치**: 정의 행 바로 아래에 그 code 의 감지 행들이 연속 블록으로 온다. 정의 행의 순서가
  파일의 골격(기능 절 순서)이다.
- **감지 행이 없는 정의 = 감지 주체 미배정 요구.** 요구는 확정됐으나 어느 모듈이 어떻게
  감지할지 아직 배정되지 않은 정의다 — §11 구현 이행 절차의 1단계 대상이며, CSV 에서
  자식 없는 정의 행으로 즉시 드러난다.
- **관점 필터**: `행=정의` 필터가 기능(요구) 관점 목록, `행=감지` + `instance` 필터가 모듈
  자기감지 관점 목록이다.

## 3. 컬럼 정의

`행` 값에 따라 채우는 컬럼이 다르다 — **공통** 컬럼은 두 행 모두, **정의**/**감지** 표기
컬럼은 해당 행만 채운다.

| 컬럼 | 행 | 의미 |
|---|---|---|
| `기능` | 정의 | 기능 역할 — `공통`(전 기능/노드 공유) / `CSCF`(P/I/S — 등록·세션 제어·라우팅) / `IBCF`(경계·NNI) / `TAS`(부가서비스·호 이력) / `PTT-AS`(MCPTT/MCData AS) / `MRF`(미디어 자원 — relay·floor·믹스·녹취·MCData 미디어 포함) / `CSC`(가입자 원천 HSS·IdMS/GMS/CMS/XCAP — MC 관리 서버) / `관리`(관리평면 — OAM/Agent 의 배포·수집/보관·펜싱·노드 관리) |
| `행` | 공통 | `정의`(요구 마스터 — code 당 1행) / `감지`(감지·구현 인스턴스 — §2) |
| `구분` | 공통 | 알람(지속 상태, open/close) / 이벤트(전이·감사 통지) — X.733 알람 vs X.730/731/740 통지, 표준화 §3.6 의 스트림 분리. 같은 code 의 정의·감지 행은 구분이 일치해야 한다 |
| `code` | 공통 | **정의 코드** `A-<DOMAIN>-NNN` / `E-<STC\|AUD>-NNN` — **정의 행당 유일**(알람·이벤트 공통), flat(표준화 §3.4(a)). 운영 사전(dictionary)·코드별 조치서(POD)·NMS 연동의 키. `NNN` 은 스트림+도메인 내 무의미 일련. 도메인: 알람 = eventType 약어(PRC/COM/QOS/SEC…), 이벤트 = kind 약어(STC/AUD). 구현 기성 클래스의 대표 정의는 구 클래스 코드 번호 승계(§3.4(a) 표). 감지 행의 code 는 대응 정의 코드(중복 허용 — 활성키는 mo 로 분리) |
| `type` | 공통 | 조건/성격 **클래스** 슬러그 — 알람은 §4 의 21클래스, 이벤트는 표준화 §3.6 의 9클래스. 클래스는 코드를 갖지 않는다(슬러그가 식별자 — 분류 정정은 코드 불변인 채 가능). 프로세스명·리소스명·임계를 넣지 않는다(표준화 §3.5). **이벤트 감지 행의 type 은 wire 정의 슬러그**(`process_started` 등 — 방향별 분리 가능, §5 쌍 이벤트 규약), 이벤트 정의 행의 type 은 성격 클래스이고 정의 슬러그는 `조건/내용` 서두의 `event=` 표기가 보유(정의당 유일) |
| `severity` | 공통 | perceivedSeverity — critical/major/minor/warning/indeterminate, 단계 임계는 `minor~critical(단계)` 표기. **이벤트는 `-`** (통지는 severity 없음). 정의 행 값은 **요구 관점의 권고값** — 감지 행(및 fm_catalog/rule)은 인스턴스 맥락(전체 vs 개별 객체·배치 형태)에 따라 다르게 배정할 수 있고, 그 편차 근거는 감지 행의 `감지 방식 설명` 이 보유한다. 단 **구현이 확정 발화 중인 정의는 정의 행 값을 구현과 일치**시킨다(범위 표기 포함 — 예: A-PRC-012 접속점 단위 major, A-COM-007 major~critical) |
| `source_system` | 감지 | **발신 노드**(호스트/논리 노드) — CSV 값은 대표 배치의 노드 예시이며, 실제 값은 모듈 SystemId(= FM envelope `hdr.node`) |
| `instance` | 감지 | **발신(감지) 모듈** — CSP/CMP/CMDP/CSC/AGENT/OAM/OAM-SVC |
| `index` | 감지 | 해당 노드 내 발신 모듈의 인스턴스 번호 — 통상 `1`. 한 노드에 같은 모듈을 다중 기동하는 배치에서만 2 이상 |
| `대상` | 공통 | 알람이 가리키는 **외부 의존 객체의 인스턴스** — §3.1. 해당 객체가 없으면 비움. **값은 CIMS 실체 명칭**(DB·CSC·CMP·CSP·PEER — §1.1 매핑). 규격 노드명(HSS/MRF/TrGW/AS)은 쓰지 않는다 — 역할 대응은 `조건/내용`/`근거` 의 괄호 주석("HSS 역할" 등)이 보유. CIMS 에 실체가 없는 외부 시스템 연동(과금 등)은 수록하지 않는다 — 도입 시 행 신설 |
| `component` | 공통 | mo_instance 의 **component 세그먼트** — 활성키 구분자, §3.2. `<...>` 꺾쇠는 런타임 인스턴스 치환(개별 발화). 런타임 mo_instance 실체화는 표준화 §3.4(b) 소유 주체 루트(서버명/그룹명) 규약을 따른다 (§7) |
| `조건/내용` | 공통 | specificProblem — 무엇이 일어나고 있는가. 정의 행은 요구 조건 서술(이벤트는 `event=<정의 슬러그>` 를 앞에 표기 — kind 는 code 도메인 STC/AUD 에서 도출), 감지 행은 그 인스턴스의 구체 조건(괄호 안은 메시지에 실릴 파라미터 — `params` 치환값) |
| `message` | 정의 | **발화 시 레코드 message 의 영문 템플릿**(vIBCF POD "장애 설명" 관례 채용 — 표준화 §7.2). `{param}` 치환(구현 fm_catalog `msg_open`/rule `msg` 와 같은 관례), 단계 임계 계열은 vIBCF 식으로 관측값·단계 임계 동반 표기(`(CRI:{crit}, MAJ:{maj}, MIN:{min})`). 알람 close 메시지는 구현 시 `msg_close` 로 확정("... restored/cleared" 형) — 카탈로그는 open 템플릿만 보유. 이벤트는 통지 메시지 템플릿 |
| `영향` / `권장 조치` | 정의 | effect / recommended action — 표준화 §7.1 의 운영 runbook 필드. vIBCF POD 의 "전 코드 조치사항" 관례 채용 |
| `근거` | 정의 | 표준(TS/RFC/X-series) 또는 벤치마크(vIBCF 코드) 참조 |
| `현재 구현 여부` | 감지 | `구현` / `후보` / `후보(선행 필요)` — `후보(선행 필요)` = 감지 코드나 전이 지점 자체가 없어 **선행 구현 없이는 알람화 불가** |
| `감지 방식 설명` | 감지 | 어떤 관측으로 open/close 를 판정하는가 — 주기·임계·연속 실패 횟수 + 코드 위치(`파일:라인`). 이벤트는 `kind=stateChange\|audit` 를 앞에 표기 |

같은 조건이면 기존 클래스(type)를 재사용하고 객체만 `대상`/`component` 로 구분하는 것이
원칙이다(표준화 §3.5) — 그래서 "CMP 두절"과 "DB 두절"이 같은 `connection_lost` 로 나타난다.

### 3.1 대상 표기 규칙 (★)

`대상` 은 **그 자체로 식별되는 객체가 있을 때만** 적는다 — 즉 모듈 외부의 의존 시스템/피어
노드다. 기록·조회 관점에서 값이 명확할 때만 컬럼을 채우는 것이 원칙이며, 억지로 채우면
컬럼의 의미가 흐려진다.

| 채운다 | 비운다 |
|---|---|
| 외부 의존 시스템·피어 노드: `DB` · `CMP_01` · `CMDP_01` · `CSC_01` · `PEER_01` | 모듈 자기 내부 구성요소: 로그/호이력 파일, 자기 listen 포트, 설정, 큐, HA 역할, 과부하 상태 |
| 인스턴스가 여럿이면 인스턴스명으로 구분(`CMP_01`, `CMP_02`) | 프로세스 자체가 객체인 것(기동/종료 이벤트) |
| 집합 전체가 조건의 객체면 클래스명(`CMP` — "전체 CMP 두절") | 객체 식별이 무의미하거나 파라미터로 충분한 것(그룹 gid, proto/port) |

- **비운 경우 그 객체는 `조건/내용` 에 서술한다** — 예: "호이력(CDR) 파일 쓰기 실패",
  "CSC 연동용 UDP 포트 바인딩 실패(4421)", "SIP 리스너 개설 실패(proto, port)".
- 같은 조건이 객체마다 개별 발화하는 경우(CMP endpoint·리스너)는 CSV 행을 늘리지 않고
  대표 1행으로 적고, "endpoint 마다 개별 발화" 를 `감지 방식 설명` 에 남긴다.

### 3.2 component 컬럼 (★)

활성 알람 식별키는 `(정의 코드, mo_instance)` 다. 같은 클래스를 여러 내부 조건에 재사용하면
(`storage_failure` "쓰기 실패" 가 로그·CDR·녹취·스토어에 두루 쓰이듯) **component 세그먼트가
없을 때 mo 가 겹친다** — 행의 구분과 활성키 분리를 CSV 차원에서 강제하기 위해
`component` 컬럼을 둔다.

- 값은 mo_instance 의 마지막 세그먼트(들): `service_log`, `call_dir`, `db/query`,
  `cmp/<ep>`, `group_ctx/<gid>` 등. `<...>` 는 런타임 인스턴스 치환(개별 발화)을 뜻한다.
- **불변식** (자기보고 모듈 CSP/CMP/CMDP/CSC 행 대상): `구분=알람` 이고 type 이 같은 감지
  행들은 (instance, 대상, component) 조합이 서로 달라야 한다 — 행이 곧 서로 다른 활성키
  후보임을 보장한다. 유일한 의도적 예외는 CMDP `storage_failure @ fd_store` 2행 —
  트래픽 구동 감지(저장 실패)와 주기 probe(접근 불가)가 **같은 활성 알람의 상보 감지
  경로**라 활성키를 공유한다(probe 가 close 정본). AGENT/OAM 행의 mo 는 sweeper 합성
  규칙(§7)을 따르므로 이 불변식 밖이다 — OAM 의 `config_out_of_sync @
  config/<collection>` 4행(A-PRC-003: drift 결과·sync_txn 전파 실패·버전 deferred·fleet
  misdirect)처럼 같은 정의의 **원인 축**을 행으로 분리해 감지 경로를 추적한다.
- 대상(외부 객체)과 component 는 공존할 수 있다: `대상=DB, component=db/query` →
  `<서버명>/csp/db/query`.
- 프로세스 자체가 객체인 이벤트(process_started 등)는 component 도 비운다.

## 4. type 체계 — 조건 클래스 카탈로그 (★)

알람 `type` 은 아래 **21개 조건 클래스**만 쓴다. 분류 기준은 **조건의 성격**(무엇이
일어나고 있는가)이지 객체·원인·영향이 아니다 — 객체는 `대상`/`component`(mo), 원인은
probableCause(rule 속성), 영향은 effect 로 간다. 새 감지 조건은 먼저 이 표에서 흡수처를
찾고, 어느 성격에도 맞지 않을 때만 클래스를 신설한다.

이벤트는 표준화 §3.6 의 성격 클래스 9종을 그대로 쓴다.

**COM (communications) — 외부와의 소통**

| type | 정의 (open 조건의 성격) | 대표 조건 |
|---|---|---|
| `connection_lost` | 피어/의존 시스템과의 **연결·keepalive·heartbeat·probe 두절** | DB 두절, CMP/CMDP/CSP 두절, Redis, 트렁크 peer, agent heartbeat 두절, FM_SYNC 두절, 외부 시스템 도달 불가 |
| `delivery_failed` | 연결/채널은 있는데 **전달·통지·작업이 지속 실패·적체** | 이벤트 ack 소진, pending_events 적체, notify 미도달, floor 메시지 송수신 실패, job report 적체, 배포 job 적체 |

**QOS (qualityOfService) — 용량/자원**

| type | 정의 | 대표 조건 |
|---|---|---|
| `threshold_crossed` | 수치 지표의 **단계 임계 초과** (승격/완화 = change) | disk/cpu/mem/load/NIC/RTP 사용률, 모듈별 CPU·RSS, FD 파티션 |
| `resource_exhausted` | 자원 **고갈·포화로 신규 수용 불가** | 풀 완전 고갈, 연결 상한, fd 고갈, 로그 큐 포화, endpoint 포화, 발언 슬롯 초과 |
| `capacity_degraded` | **설정 대비 실효 용량 미달** (시스템이 갖춰야 할 용량을 못 갖춤) | 풀 부분/전량 bind 실패, epoll 미등록 소켓 잔류 |
| `resource_leak` | **회수 실패로 자원이 점진 누적·증식** | orphan/idle 회수 지속, PTT 그룹 미회수, 연결 누수, IdMS 스토어 무한 증식 |
| `overload` | **과부하 방어(차단/강등) 발동** | 제어평면 신규 요청 차단 |

**PRC (processingError) — 실행·저장·정합·이중화·관측**

| type | 정의 | 대표 조건 |
|---|---|---|
| `process_down` | 프로세스 소멸 (L1 — agent 관측) | 모듈 프로세스 사망 |
| `process_unresponsive` | 프로세스 생존인데 **서비스 응답 불능** (L3 원격 probe + 노드 로컬 readiness) | STATS 무응답, gateway upstream 무응답, 모듈 zombie |
| `worker_unavailable` | 프로세스 내부 **실행 단위·서브시스템 정지** (스레드/리액터/타이머/스위퍼/엔진) | RTP 리액터·제어 루프·MSRP 리액터 사망, 스위퍼 연속 실패, probe 스레드 사망, provision 엔진 불능 |
| `crash_loop` | **재기동/기동 반복 실패** (불안정 지속) | restart 소진, 한 번도 못 뜨는 start 반복 실패 |
| `listener_unavailable` | **자기 수신 접속점 불능** (listen 포트·소켓·수신 채널 개설 실패/상실) | SIP/HTTP 리스너, 제어 소켓, CSC 연동 포트, Monitor 포트, FM ingest |
| `storage_failure` | **영속 저장소 읽기/쓰기/접근 실패** (파일·DB·스토어 — 유실/기능 정지) | 로그/CDR/녹취/상태파일 쓰기 실패, FD·IdMS·파일 스토어, DB 쿼리/적재 실패, 공유 store(NAS) 접근 불가, 알람 스트림 자기 기록 |
| `config_invalid` | **설정 자체의 결함·미설정·무결성 위반**으로 기능 비활성/오동작 | 참조 무결성, 컬렉션 소실, 타이머 0, 시크릿 미설정, Data 게이트, 스키마 드리프트, TLS 비활성 |
| `config_out_of_sync` | **기대(배포 기록·정본·버전) vs 실제의 불일치** | config drift, HA fan-out drift, sync_txn 전파 실패, 버전 deferred, managed IP/route 소실, fleet misdirect |
| `state_out_of_sync` | 컴포넌트 간 **런타임 상태 정합 실패** | CSP↔CMP 세션집합 불일치, 그룹 컨텍스트 재수립 실패, 알람 open-state 복원 불일치 |
| `redundancy_degraded` | **이중화 실질 소실·승격 불가·절체 메커니즘 이상** | 절체 래치, 승격 부적격, keepalived 미설치/비활성/얼림, VIP 무보유/이중보유, ha_excluded, store 소유권 리스 상실 |
| `dependency_unavailable` | **필수 실행 의존물 부재** (도구/패키지/권한/플랫폼 기능) | cims-svc/priv 미발견, sudo 미등록, base deps, NAS flock 미제공 |
| `observability_lost` | **관측·자기보고 파이프라인의 공백·오염·무력화** (통신 두절 제외) | metric blackout, ip -j 빈 배열 위장, 카탈로그 무력화(UNKNOWN_CODE), 관측 대상 공백, 스위퍼 비활성 |
| `cert_expiring` | **인증서 수명 위험** (만료 임박·회전 실패) | agent mTLS 만료 임박, 회전 실패 |

**SEC (X.736 Security Alarm) — 보안**

| type | 정의 | 대표 조건 |
|---|---|---|
| `security_violation` | **보안 이상 징후의 급증**(인증 실패 폭주·사기 호/스캐너·비인가 트래픽) — 율 임계 기반 open/close. 보안 알람은 X.733 계열과 통지 성격(대응 주체·민감도)이 달라 `threshold_crossed` 로 흡수하지 않는다 | 인증 실패 급증(A-SEC-001), fraud/스캐너 탐지(A-SEC-002) — 미구현 |

- **기존 구현 코드와의 관계**: wire/rule/fm_catalog 는 flat **정의 코드**
  (`A-<DOMAIN>-NNN`, 정의 행이 채번 정본)를 쓴다. 구 포맷 클래스 단위 코드
  (`CIMS-<DOMAIN>-<SEQ>` 7종)는 `_CODE_REVISIONS` read alias + 스윕 이행 종결로
  흡수됐다(표준화 §3.4(a) — 각 클래스의 대표 정의가 구 번호를 승계, 구 QOS-001 의 분할은
  ha_flap=A-QOS-023·rtp 사용률=A-QOS-024). `storage_failure` 는 구 type
  `resource_failure` 의 개명(정의를 "영속 저장소 실패"로 좁힌 것). 미구현 클래스는
  구현 채택 시 표준화 §3.3 매핑 표에 편입한다.
- **경계 규칙** (혼동 잦은 쌍):
  - `connection_lost` vs `delivery_failed`: 연결/생존 신호 자체가 끊기면 전자, 채널은
    살아 있는데 그 위의 전달이 실패·적체하면 후자.
  - `listener_unavailable` vs `worker_unavailable`: 수신 **접속점**(소켓/포트) 불능이면
    전자, 접속점과 무관한 내부 **실행 단위** 정지면 후자.
  - `config_invalid` vs `config_out_of_sync` vs `state_out_of_sync`: 설정 자체가
    잘못됐으면 invalid, 설정이 기대치와 어긋나면(드리프트·전파 실패) out_of_sync,
    설정이 아니라 **런타임 상태**의 정합이 깨졌으면 state_out_of_sync.
  - `capacity_degraded` vs `resource_exhausted`: 갖춰야 할 용량을 **못 갖춘 것**(결함)이
    전자, 갖춘 용량이 **다 쓰인 것**(수요)이 후자.
  - `observability_lost` vs `connection_lost`: 관측 주체와의 통신 자체가 끊기면
    connection_lost(agent 두절·FM_SYNC 두절), 통신은 되는데 관측 데이터/판정이
    공백·오염되면 observability_lost.
  - `dependency_unavailable` vs `connection_lost`/`config_invalid`: 통신 상대가 없으면
    connection_lost, 자기 설정이 잘못됐으면 config_invalid, 통신도 설정도 아닌 **실행
    환경의 전제물**(외부 도구·권한 등록·플랫폼 기능 — cims-svc/sudoers/flock) 부재가
    dependency_unavailable — 프로세스·코드·설정이 전부 정상이어도 열린다.
  - 클래스는 고장 **메커니즘**이 아니라 잃은 **능력**으로 분류한다: 같은 스레드 사망도
    수신 접속점을 잃으면 listener_unavailable(CSC HTTP 리스너), 내부 실행 단위를 잃으면
    worker_unavailable(CMP RTP 리액터)이고, 같은 소켓 오류도 listen 개설 실패 =
    listener_unavailable / 나가는 연결 두절 = connection_lost / accept fd 고갈 =
    resource_exhausted 로 갈린다.

## 5. 수록 원칙 — 정의 행

- **기능 고유 축만 수록한다.** 전 기능이 공유하는 조건(프로세스 생존·호스트 자원·설정·이중화·
  관측 등)은 `공통` 절이 담당하고 각 기능 절에 재수록하지 않는다. 단 같은 조건이라도 기능에
  따라 영향·severity 가 본질적으로 다르면 기능 절에 둔다(예: TAS/PTT-AS 의 CMP 두절).
  이렇게 **여러 기능 절이 같은 CIMS 실체를 향하는 정의들**(IBCF·TAS·PTT-AS 의 CMP 두절 —
  런타임 mo 동일)은 요구 축이 다른 별개 정의다 — 구현 채택 시 감지 경로는 하나로 통합될
  수 있고, 그 경우 발화는 대표 정의 하나로 수렴시킨다(같은 mo 에 중복 발화 금지 —
  영향/조치는 역할 합집합으로 기술). 수렴이 확정되면 **대표 정의 행에 수렴 사실을
  표기**하고, severity 는 흡수한 역할들의 최고치까지 범위로 넓히며, 흡수된 행에는
  대표 코드로의 수렴 포인터를 남긴다 — 현행 확정: CSP 의 CMP 두절은 `A-COM-007` 이
  대표(A-COM-006/A-COM-008 흡수, 구현 기성 — 전체 두절 critical·endpoint 단위 major).
- **code 유일 불변식**: 정의 코드는 정의 행당 유일하다(알람·이벤트 공통) — 정의 하나 = 코드
  하나. 같은 정의의 다중 런타임 객체(대국·접속점·풀·그룹)는 코드를 늘리지 않고
  mo(`대상`/`component`)로 구분한다(개별 발화). 활성 알람 식별키 = (정의 코드, mo_instance).
  결번은 재사용하지 않는다(표준화 §3.4(a)) — 현재 결번: `E-AUD-001`.
- **알람 = 상태 전이로 표현 가능한 지속 조건**, 요청 단위 일회성 이상은 로그/PM 소관
  (표준화 §3.6). 반복 오류는 카운터/율 임계로 전이를 만들어 수용한다(성공률·rx_error 등) —
  vIBCF F 계열 대조의 확정 판정(표준화 §7.2.2, 제3 스트림 미신설).
- 한 조건 = 한 행. 객체 다수(접속점·대국·풀·그룹)는 행을 늘리지 않고 "개별 발화"로 서술.
- 쌍 이벤트(added/removed·entered/exited·blocked/unblocked)는 `*_changed` 1행으로 적는다 —
  정의 행은 대표 표기이며, wire `type` 은 방향별 슬러그로 분리될 수 있다(감지 행의
  listener_added/removed 등). 요구 정의↔wire 슬러그 매핑은 구현 이행 시 확정(§11).
- **이벤트도 알람과 대칭으로 분류·코드화한다**(표준화 §3.6) — kind(STC/AUD 도메인) →
  성격 클래스(type, 9종) → 정의 코드(`E-<STC|AUD>-NNN`) → 인스턴스(mo). 정의 슬러그
  (`event=`, wire type)는 카탈로그 전체에서 정의당 유일해야 한다(같은 슬러그 = 같은 정의).

## 6. 범위 원칙 — 감지 행

- **감지 행의 소속 = 감지 주체.** 모듈이 **대상(mo)** 인 알람 전체 목록이 아니라, 그 모듈이
  **감지해서 발화하는** 목록이다. 감지 3계층(표준화 §3.4(b))과의 대응: 서비스 모듈
  (CSP/CMP/CMDP/CSC) 행 = L2 자기보고(detected_by=`self`), AGENT 행 = L1 생존 + 호스트
  자원/드리프트(detected_by=`agent` — 원시 관측은 agent, 임계 평가는 OAM base 가 수행해도
  감지 주체 클래스는 agent), OAM/OAM-SVC 행 = L3 원격 probe·OAM 자체 판정·OAM 자신의 내부
  상태. 예컨대 "CSP 프로세스 사망"은 CSP 행이 아니라 AGENT 행이다 — 죽은 프로세스는
  자기보고를 못 한다.
- **알람 후보의 기준은 상태 전이(open/close)로 표현 가능한 지속 조건.** 요청 단위 일회성
  오류(SIP 파싱 실패·인증 실패·호별 처리 실패)는 알람 부적합이며 로그/PM metric 소관이다.
  전이 없이 반복되는 오류는 카운터/율(rate) 임계로 전이를 만들 수 있을 때만 후보가 된다.
- **기동 자체가 불가한 조건은 제외.** 포트 bind 실패로 즉시 종료하는 경우처럼 프로세스가
  사망하는 조건은 L1(agent `process_down`) 소관이다. 반대로 *기동은 됐는데 일부 기능만
  불가*한 상태(리스너 일부 실패 등)는 L2 후보다 — 이 경계가 감지 행 판정의 핵심이다.
- **자기보고 경로 자체의 장애는 자기보고할 수 없다.** FM 채널 두절·카탈로그 부재는 OAM 의
  "FM_SYNC 3회 누락 → 판정 불가 close"([alarm_self_reporting.md](alarm_self_reporting.md) §5)가
  담당한다.

## 7. mo_instance 매핑

감지 행의 컬럼이 wire/저장 레코드의 `source.mo_instance` 를 구성한다
(표준화 §3.4(b) 소유 주체 루트 — 자기보고는 `<서버명>/<module>[/<component>]`):

```
<source_system> / <instance> / <component>              ← 대상 비움
<source_system> / <instance> / <대상-키>[/<component>]  ← 대상 있음
  예) SIG_SVR_01/csp/db                       ← 대상 DB, component=db
      SIG_SVR_01/csp/db/query                 ← 대상 DB, component=db/query
      SIG_SVR_01/csp/cmp/192.168.1.10:9000    ← 대상 CMP_01, component=cmp/<ep>
      SIG_SVR_01/csp/call_dir                 ← 대상 비움, 내부 component
      SIG_SVR_01/csp                          ← 이벤트 등 component 도 비움
```

구 wire 형식(`cims/<instance>/<source_system>/...`)은 수신/read 시 현행 형식으로
정규화해 흡수한다(표준화 §6 mo 루트 개편 규율 — 스윕 이행 종결 + 재발화 완료).

- 활성 알람 식별키는 `(정의 코드, mo_instance)` 다. 같은 클래스(type)를 여러 조건에
  재사용하는 행들은 `component` 컬럼(§3.2)이 행과 활성키를 분리한다 — 컬럼 불변식 위반 =
  활성키 충돌.
- 노드 분리를 빠뜨리면 HA 다중 인스턴스에서 활성키가 충돌한다 (cmp 변종 pmp/imp 의
  SystemId overlay 필수 사유와 동일).
- **AGENT/OAM 행의 mo 는 위 규칙이 아니라 sweeper 합성 규칙을 따른다** — agent 계열
  `<서버명>/<module|disk|…>`, 서비스 probe 는 관측 신원 기준 `<서버명>|<그룹명>/<모듈>`,
  HA fan-out `<그룹명>/config/<collection>` (표준화 §3.4(b)). CSV 의 `instance` 컬럼은
  어느 쪽이든 **발신(감지) 모듈**이다.
- `detected_by` 는 CSV 에 컬럼을 두지 않는다 — `instance`(감지 모듈)가 주체 클래스를
  결정한다 (CSP/CMP/CMDP/CSC→`self`, AGENT→`agent`, OAM-SVC→`oam-svc`, OAM→`oam`).
- `source_system` 예시 표기: `SIG_SVR_01`(csp 계열) · `MED_SVR_01`(cmp/cmdp) ·
  `SUB_SVR_01`(csc) · `MGMT_SVR_01`(oam/oam-svc) · agent 는 상주 호스트(예시 `SIG_SVR_01`).

## 8. CSV 취급 규약·불변식

- UTF-8, 헤더 1행. 쉼표를 포함하는 필드는 `"..."` 로 인용한다(엑셀/`csv` 모듈 호환).
- 한 조건 = 한 행. 같은 조건의 객체 확장(endpoint·리스너 다수)은 행을 늘리지 않는다(§3.1).
- **불변식** (기계 검증 가능 — 위반은 편집 오류다):
  1. 정의 행의 `code` 는 유일하다. 결번 재사용 금지(§5).
  2. 감지 행의 `code` 는 반드시 대응 정의 행이 존재한다 — `(미배정)` 같은 예외값은 없다.
     새 감지 조건에 맞는 정의가 없으면 **정의 행 신설(채번)이 선행**이다.
  3. 감지 행은 자기 정의 행 바로 아래 연속 블록으로 온다(§2). 같은 code 의 정의·감지 행은
     `구분` 이 일치한다.
  4. 자기보고 모듈(CSP/CMP/CMDP/CSC)의 `구분=알람` 이고 type 이 같은 감지 행들은
     (instance, 대상, component) 조합이 서로 다르다 — 유일 예외는 CMDP
     `storage_failure @ fd_store` 2행(§3.2). AGENT/OAM 행은 이 불변식 밖(§3.2·§7).
- 구현 상태(`현재 구현 여부`)는 코드 정본과 함께 갱신한다 — 후보가 구현되면 `구현` 으로
  바꾸고 `severity` 를 확정값으로 채우며(정의 행 severity 정합 — §3 severity 규약),
  fm_catalog/rule 에 정의 코드를 탑재한다. 카탈로그 선언(`fm_catalog.json`)과 CSV 의
  `구현` 감지 행은 항상 일치해야 한다. 현행 유일 예외: CSC `config_change` — 카탈로그에
  선언됐으나 호출자 0건이라 CSV 는 `후보`(행에 예외 명시, §10.4).

## 9. 기능별 설계 메모 — 정의 행

- **공통** — 노드/프로세스/플랫폼 축. 핵심은 `observability_lost`(관측 파이프라인 자체의
  공백을 알람화 — "알람 없음 = 정상"의 착시 방지)와 NTP(시각 정합은 CDR·녹취·알람 시각의
  전제 — vIBCF 가 3클래스로 운영할 만큼 실운영 가치 검증). NTP 는 성격이 다른 2정의로
  분리한다 — offset/delay **임계**는 `threshold_crossed`(A-QOS-003), **동기 상실**은 수치
  임계가 아닌 시각원(외부 의존) 상실 상태라 `connection_lost`(A-COM-014 — X.733
  lossOfSynchronisation 은 communications 계열).
- **CSCF** — 3축: ①원천 의존(CSC·DB/DNS — 두절과 stale 을 구분: connection_lost vs
  db/reload) ②수용량(sessions/cps/dialogs — 상한 정의가 전제) ③서비스 품질
  율(calls·reg success_rate — 최종 응답코드별 카운터 기반, **와이어에 나가지 않는 로컬 합성
  응답(트랜잭션 타임아웃)을 반드시 집계에 포함**해야 flow 로그 사각이 메워진다).
- **IBCF** — vIBCF 벤치마크 직결. **대국별(mo 에 peer 세그먼트) 축**이 본질 — 전체 평균은
  특정 대국 이상을 희석한다(성공률·reason·RTT 전부 대국별 발화). 경계 특성상 보안
  축(fraud)과 유입 이상(rx_error — CSCF 행이 경계 접속점 포함)이 최전선.
- **TAS** — 호 이력(CDR) 기록 유실이 최우선(critical) — 서비스는 되는데 기록이 없는 상태가
  최악. 부가서비스 실패율은 가입자 체감 축. **과금 시스템 연동은 CIMS 에 없다** — 온라인/
  오프라인 과금 연동을 도입하면 connection_lost 대상 행을 그때 신설한다.
- **PTT-AS** — MCPTT 특성 2가지: ①**긴급/임박위험 호 실패는 소수 건도 즉시
  알람**(인명 직결 — 일반 성공률과 별도 축·낮은 임계·critical) ②그룹 상태 정합
  (group_ctx — 통화는 되는데 정책이 옛 값인 무성 장애)이 고유 위험. 키 자재(KMS)는
  cert_expiring 클래스로 흡수(수명 위험이라는 같은 성격).
- **MRF** — 4축: 자원(고갈 vs 설정 대비 미달 — exhausted/degraded 구분), 제어
  정합(CSP 와의 채널·이벤트·세션 집합), 미디어 품질(no_flow/quality — RTCP 기반),
  저장(녹취/FD — 법적 보존). 실행 단위(worker) 정지는 "포트는 살아있는데 무중계"라
  프로세스 생존 감시로 잡히지 않는 고유 사각.
- **CSC** — 3축: ①API 접속점(리스너 사망이 기동 로그상 성공으로 위장되는 무성 결함 —
  프로세스 생존 감시 무력) ②변경 전파(notify 미도달 — 프로비저닝이 시그널링에 미반영되는
  무성 장애. CSCF 의 A-COM-002(두절 관점)·E-AUD-006(수신 관점)과 상보) ③스토어(IdMS
  토큰·가입자/그룹 파일 정본 — 기록 실패가 200 응답 뒤에 숨는 축, 토큰 회수 실패는 구
  토큰 잔존이라는 보안 함의 동반).
- **관리** — 관리평면 **기능 고유** 조건만 담는다. 전 노드가 공유하는 조건(프로세스 생존·
  호스트 자원·agent 관측 두절 A-COM-015·관측 파이프라인 A-PRC-010)은 공통 절 소관 그대로.
  축 3가지: ①정본 저장(공유 스토리지 접근·잠금 펜싱 — "손상 위험 상시"는 "접근 상실"과
  다른 상시(sticky) 조건이라 정의 분리) ②배포/작업 전달(agent 발신 report 적체와 OAM 발신
  job 적체의 양방향) ③관측 파이프라인의 보관/복원 구간(alerts 스트림 자기 기록·복원 창 —
  발생/수집 구간 공백은 공통 A-PRC-010 이 보유). `deployment_failed`/`deploy_job` 은
  이벤트 `job_result` 클래스의 첫 적용이다.
- 보안 축: `security_violation`(X.736 Security Alarm — DOMAIN `SEC`, 정의 코드 `A-SEC-NNN`,
  표준화 §3.4(a))은 A-SEC-001/002 가 근거인 클래스다(§4) — 구현 채택 시 표준화 §3.3 매핑
  표에 편입한다(현재 미구현).

## 10. 모듈별 메모 — 감지 행

CSV 에 담기지 않는 판정 근거·경계·동반 결함을 감지 모듈별로 남긴다.

### 10.0 전 모듈 공통 제약·클래스 통일 원칙

- **FM 채널 패킷 상한 = 발신 32KB / OAM 수신 64KB**: FM_REGISTER 에는 카탈로그 전문이
  실린다 — 알람 엔트리 1건이 ~530-630B 라 CMP 미디어 채널의 4KB(envelope §1.2)로는 알람
  6~7종부터 등록이 영구 실패하므로 FM 채널은 상한을 분리 상향했다(`include/FmReporter.h`
  `kFmMaxPacket`, csc `fm_reporter.py`, OAM `fm_ingest.py` 수신 64KB). FM_SYNC 의 active
  배열도 같은 상한을 받는다. 배포 스큐 주의: 4KB 초과 카탈로그는 수신측(OAM)을 먼저
  올려야 등록된다.
- **클래스 배정은 §4 type 체계가 정본** — 같은 조건 = 한 클래스(표준화 §3.5), 감지
  주체가 달라도 클래스는 같다(공유 store 접근 불가 = AGENT/OAM 모두 `storage_failure`,
  승격 부적격 = AGENT 노드 자격/OAM 그룹 제외 모두 `redundancy_degraded`). 자기 listen
  포트 개설 실패는 connection 이 아니라 `listener_unavailable`, 단계 용량 임계는 감지
  주체가 self 여도 `threshold_crossed` 다.
- **로그 계열 공통 3축**: 쓰기 실패(storage_failure `service_log`) / 큐 포화·drop
  (resource_exhausted `log_queue` — fopen 성공인데 소비가 못 따라가는 축, 쓰기 실패 알람이
  안 열리는 사각) / 설정 미설정(config_invalid) — 4개 모듈(csp/cmp/cmdp/csc)이 동형 구조라
  CSV 도 같은 3분할을 쓴다.

### 10.1 CSP

**현행**: 알람 6종(connection_lost — DB·CMP 두절 / threshold_crossed — 호·등록 성공률,
신규 INVITE CPS, SIP 수신 이상) + 이벤트 2종(process_started/stopping). fm_catalog.json
선언과 구현이 일치한다. SIP 통계 축은 psip 스택 카운터(`CSipStackCounter` — 수신 요청·
최종응답 수신/송신·파싱 실패, **와이어에 없는 로컬 합성 408/660 포함**: `RecvResponse`
팬아웃 계측)를 `SipStatsMonitor` 가 `Setup.SipStats.*` 윈도우/단계 임계로 평가해
발화한다(단계 승격/완화 = severity 동반 open 재통지 → OAM change, monitor 명령 `sip_stats`).

**자기보고 대상이 아닌 것 (경계 확정)**
- **SIP 수신 소켓 버퍼 overflow / 스레드풀 포화**: 프로세스 내부 관측 지점이 없다(커널·psip
  영역) — host metric(agent) 소관.
- **nonce/구독 맵 포화**: 상한·포화 분기가 없고 시간 기반 청소만 있다 — 크기 임계를
  신설하기 전까지 알람화 불가. 세션/다이얼로그 축은 정의 행(CSCF `sessions`·`dialogs`)
  요구로 승격 — 분자(`CallMap::GetCount`)는 기성, 상한 설정 신설이 선행(표준화 §7.2.1 A0059).
- **TLS 인증서 만료 예고**: 모듈·스택에 X509 notAfter 검사가 없다. 파일 기준 검사는
  agent/OAM 경로가 적절(리스너 개설 실패는 별개로 CSV 에 있음).
- **CMP/CMDP 요청 단건 타임아웃**, per-call 실패: 일회성 → 알람 부적합. 부하 지표로
  쓰려면 율 카운터 신설이 선행. (SIP 파싱 실패·호별 실패는 율 임계로 전이화해 구현 —
  A-QOS-011/006 행 참조.)

**동반 발견 결함** (알람화와 별개로 수정 대상)
1. CMP 응답 status≠OK 가 무로그 실패(`CmpClient.cpp:584-593`) — 포화 미탐 구간의 실패 원인
   규명 불가. 최소 로그 + status code 기록 필요.
2. `CCscInterface::Start()` 가 bind 결과와 무관하게 true 반환(`CscInterface.cpp:44-50`) —
   `CspServer.cpp:355` 의 체크가 무의미. 잘못된 bind IP 의 무통보 INADDR_ANY fallback
   (`:102-105`)도 동반.
3. CDR/flow 로그 쓰기 실패 전 지점 silent + `m_ulDroppedLogs` 카운터 소비자 0. CDR 은 공용
   쓰기 헬퍼가 없어 append `fopen` 11개 지점 개별 산재(CSV `call_dir` 행) — 실패 카운터
   신설 = 12곳 통합.
4. fm_catalog.json 의 connection_lost(DB) `effect` 가 "파일 fallback 범위로 축소"라고 서술하나, 런타임
   fallback 전환이 실제로 없다(기동 시 1회 평가) — 문구 정정 또는 전환 구현 중 하나가 필요.
5. route `max_concurrent_calls`/`cps_limit` 가 파싱만 되고 사용처 없음
   (`CspRouteMap.cpp:51-52`) — 과부하 알람의 선행 조건과 연관.
6. **CmpClient/CmdpClient `Init` 실패가 기동을 막지 않으면서 keepalive 도 안 뜬다**
   (`CmpClient.cpp:152-166`+`:180-183`, `CmdpClient.cpp:75-92`) — CMP/CMDP 두절 알람의
   판정 루프 자체가 소멸하는 전제 붕괴(CSV `cmp_ctrl`/`cmdp_ctrl` 행).
7. 그룹 60s 재적재가 두 곳에서 중복 실행(`CspServer.cpp:499-505` + `GroupCallService.cpp:
   1090-1098`). 가입자(`LoadAllUsers`)는 주기 호출자가 아예 없다 — 기동 1회 + CSC_RESTART 뿐.
8. jsonl 파일 부재를 "정상 빈 배열"로 취급(`CspConfigCache.cpp:75-85`) + 런타임 reload 에
   sanity 게이트 없음 — 오배포 한 번에 listeners/routes 전량 소거 가능(CSV
   `config/collections` 행).
9. **STATS `active_calls` 가 DB 연결 시 상시 0** — `GetActiveVoipCallCount()` 가 `return 0`
    스텁(`DbManager.cpp:658-660`)인데 `CscInterface.cpp:226-231` 이 DB 연결 시 이 값을 사용
    (미연결 시에만 `CallMap::GetCount()`). OAM 은 call.json 스캔으로 덮어써(`stats.py:642-646`)
    콘솔에선 안 보이는 결함 — 세션 사용률 알람(정의 행 CSCF `sessions`)의 선수정.
10. **CMsgLogger 는 CSP 내 죽은 코드** — `gclsMsgLogger` 호출자가 csp/ 안에 0건(인스턴스
    정의만, 실사용은 재설계 예정인 cwrtc 뿐). msg_log 쓰기 실패는 이 사유로 감지 행
    비대상(발생 불가 조건).
11. `LogSecurity()`/security.jsonl 이 dead code(`SipMessageLogger.cpp:81-103` — 호출처 0) +
    toll-fraud 603 은 `SuppressNetworkSource` 로 flow/msg 의도적 억제(`ModuleDispatcher.cpp:
    284-301`) — 보안 관측이 텍스트 로그 요약 1줄뿐(security_violation 정의의 관측 공백).
12. **Max-Forwards 검사 부재** — 수신 검사·483 응답 없음(`SIP_TOO_MANY_HOPS` 사용처 0) +
    송신 시 부재면 무조건 70 세팅(`SipStackComm.hpp:581-583`) — 루프 방지 무력화(규격 결함).
13. **로컬 합성 응답(트랜잭션 타임아웃 408)은 flow 에 절대 남지 않는다** — Timer B/C 만료 시
    psip 이 합성해 자기주입(`SipICTList.cpp:190-231`), 와이어 미송신. 호 카운터는 포함
    완료(`CSipStackCounter` — `RecvResponse` 팬아웃 계측이 합성 408/660 을 통과시킴,
    A-QOS-006 행). flow `detail` 사유 코드화는 잔여(표준화 §7.2.2 채용 1).

**구현 우선순위** (훅 비용 대비 가치)

| 순위 | 항목 | 근거 |
|---|---|---|
| 1 | CMP endpoint 두절 + **포화** (connection_lost·resource_exhausted `cmp/<ep>`) | 두 전이 훅이 같은 함수에 기성(bLive/bSaturated) — 배선 각 2줄 |
| 2 | CMP/CMDP 제어 소켓 개설 실패 (`cmp_ctrl`/`cmdp_ctrl`) | 1순위·4순위의 판정 루프 존재 자체를 보장하는 전제 |
| 3 | service_log/CDR/상태파일 쓰기 실패 (storage_failure component 3행) | 현재 완전 silent, 호 이력 유실 |
| 4 | CMDP 두절 (connection_lost) | 콜백 배선 1곳 |
| 5 | 리스너 개설/제거 실패 (listener_unavailable) | TLS 포함 접속점 상실 + 유령 리스너, reload 재평가로 전이 자연 |
| 6 | CSC 연동 bind 실패 (listener_unavailable `csc_if`) | 결함 2 수정 동반 |
| 7 | DB 쿼리 지속 실패·재적재 실패 (storage_failure `db/*`) | connection_lost 가 못 잡는 구간 |
| 8 | 설정 무결성 위반 + 컬렉션 0건 전이 (config_invalid) | 산재 지점 1클래스 통합 + 오배포 방어 |
| 9 | 세션 정합 불일치·그룹 컨텍스트 재수립 실패 | 훅 기성(digest 대조·60s 재평가) — 무성 장애 관측 |
| 10 | 워커 스레드 watchdog (worker_unavailable) | L1/L3 공통 사각 — 스레드별 lastTick 신설 |
| 11 | 이벤트 ha_role_changed · config_reloaded · user/group_config_changed | 전이 훅 기성 |

### 10.2 CMP

**현행**: 알람 1종(resource_exhausted — 자원 풀 완전 고갈, rtp/ptt_floor/ptt_member 풀별) + 이벤트 2종.

**자기보고 대상이 아닌 것**: 코덱/믹서 이상(CMP 는 투명 relay — 코덱·믹싱 코드 자체가 없음,
PT 는 헤더 1바이트 스탬프), HA 역할 전이(All-Active — keepalived/standby 코드 없음, 절체
관측은 CSP CmpClient 소관), 사용률 단계 임계(OAM sweeper threshold_crossed 역할 분담), floor 타이머
만료·대기열 Deny(정상 규격 동작), 디스크 여유(agent 소관).
`config_reloaded` 이벤트도 N/A — **런타임 설정 재적재 경로 자체가 없다**(SIGHUP/SIGUSR1
핸들러 없음, 전 필드 restart 요구).

**동반 발견 결함**:
1. **배포 overlay 부분 적용** — `loadConfig` 가 overlay 를 첫 파싱 패스에만 적용
   (`PCmpServer.cpp:1587-1613`)하고 `RecordEnable`/`RecordDir`/`SegmentIntervalSec`/
   `ServiceLogging.*`/`SystemId`/`Fm.*` 는 원본을 재파싱(`:1692-1697` `root2`) — 배포본
   config.json 의 해당 키가 **전부 무시**된다. `Fm.Enable/OamIp` 무시 시 자기보고 자체가
   켜지지 않는다(카탈로그 전체의 전제 붕괴 — 최우선 수정).
2. resource_exhausted 의 relay 분모가 설정값(`_rtpPoolSize`, `:168`)이라 실측 풀과 불일치 —
   전량 bind 실패에도 rtp_pool 은 resource_exhausted 가 열린다(capacity_degraded 상황의 오분류). `total<=0`
   판정 제외(`:180`) 사각은 실측 크기를 쓰는 **ptt 2풀 한정**. params 는 항상
   `used=total`(`:184-185`)로 렌더.
3. `writeMsgLine` 에만 `_serviceLogDir` 빈값 가드 누락(`:2171` — `logFlow` `:2206` 은 있음)
   → 빈 dir 시 루트 절대경로 생성 시도·영구 실패 루프.
4. 로그 큐 포화 시 **가장 오래된 줄** 폐기(`:2322-2326`) — msg seq(줄번호 기반)와의 상관이
   전 구간 파괴(남은 로그도 오독). `_logDropped` 는 선언·증가뿐 노출 전무.
5. 녹취 인덱스 read 실패와 파일 부재를 구분 못 해 seq 재시작 → 기존 세그먼트 덮어쓰기
   (`PSyncRtpRecorder.cpp:142-158`). 트랙 fp=NULL 시 `writePacket` 조용히 return(`:241`).
6. `_rtpStartPort` 등 5개 멤버 미초기화 / 설정 `CspIp/CspPort` 파싱 안 됨(통지 대상은
   학습값 전용) / 제어 recvfrom 오류 무처리(무로그 tight loop `:214-229`) /
   mkdir·rename·fclose 반환값 전면 미검사 / 녹취 루트 `system("mkdir -p")` 반환 미검사
   (`:1738-1741`).
7. SIGINT/SIGQUIT 미처리 + SIGTERM 등록이 `startServer()` 이후(`PMain.cpp:32→:39`) —
   기동 중 SIGTERM 도 process_stopping 유실.
8. epoll 등록 실패 소켓이 풀에 잔류(`:1757-1768` vs `:1808-1856`) — 배정 시 해당 호 무음.

**우선순위**: ⓪결함 1(overlay — 자기보고 전제) ①`SessionTimeout<=0` 판정
(config_invalid `config/timers` — floor·sweeper·재전송 전체 정지) ②풀 부분 bind 실패
(capacity_degraded) + epoll 잔류 ③flow/msg 쓰기 실패 + log_queue 분리(storage_failure·resource_exhausted)
④녹취 쓰기/인덱스 실패(storage_failure — self_reporting §9 후보 그대로) ⑤리액터·제어 스레드 사망
(worker_unavailable ×2) ⑥CSP 이벤트 적체(`csp/events` — pending_events 기성 카운터,
무수신 행은 선행 필요) ⑦sweeper 회수 지속·PTT 그룹 미회수(resource_leak ×2)
⑧floor SRTCP tx/rx(그룹 단위 — 카운터 일부 기성).

### 10.3 CMDP

**현행**: 알람 1종(storage_failure — FD 스토어 저장 실패) + 이벤트 2종. 현행 구현의 한계 3건이
핵심 — **트래픽 구동 판정**(무트래픽 구간 미탐·stale), **write 결과 미검사**(ENOSPC 시
meta 만 성공해 `Store()` true → **close 오발화**, `PFdStore.cpp:71-83`), **params 미탑재**
(errno/path — `AlarmOpen` params 오버로드 기성인데 미사용).

**자기보고 대상이 아닌 것**: 리스너 bind 실패(즉시 종료 → L1 소관 — cmdp 는 런타임 재개설
경로가 없어 런타임 bind 실패 자체가 없음. 단 **epoll 등록 실패·epoll_create1 실패는 기동
성공 + MSRP 전면 불가**라 L2 대상 — CSV `worker/msrp` 행), MSRPS/TLS(미구현 — 도입 시
listener_unavailable 클래스에 흡수), 세션 맵/송신 큐 포화(상한·관측 지점 없음 — 선행 필요),
fm_catalog 부재(자기보고 경로 자기장애 — OAM 소관), 481/501/413/415/TLV 파싱 실패(요청 단위).

**동반 발견 결함**: ①FD 스토어 ofstream write/flush 미검사(위) + `.msrp` 는 open 실패도
무시 + rename 실패 시 `.tmp/.bin/.msrp` 잔존(`PFdStore.cpp:103-115`) + 메시지당 이중
기록(저장량 2배)·GC/retention 전무(unlink 0건) ②로그 버킷 mkdirP 반환 무시 후 경로 확정
(`PCmdpServer.cpp:1089`) + fclose 미검사(`:1202`) + `_currentHourDir` 빈값 시 조용히
return(`:1059,:1064,:1114-1115`) ③제어 recvfrom 오류 무한 busy spin + 스레드 detach 라
생존 확인 불가(`:176-184`) ④커넥션이 워커 수와 무관하게 항상 `_reactors[0]` 에 등록 —
MsrpWorkerCount 사실상 무효(`:589,593,789` — 리액터 사망 판정은 워커 0 단일로 충분)
⑤send 오류 시 markClosed 미호출 — 좀비 연결이 연결 상한 잠식(`PMsrpConnection.cpp:84-86`)
+ `_connections` idle 회수 경로 자체 부재(연결 포화 알람 close 성립의 전제 결함) + 파서
버퍼 연결당 64MB 성장(`PMsrpParser.cpp:117`) ⑥`Fm.Enable` 문자열 비교 — boolean 설정 시
자기보고 무음 정지(`:1009`) ⑦설정 파일 열기 실패 시 전 항목 기본값으로 기동(`:926-930`)
⑧`kMaxConnections` 하드코딩(`:31`)·accept4 실패 카운터 없음(`:570-576`)
⑨SIGTERM 만 등록 + 등록이 startServer 이후(`PMain.cpp:31→:37`).

**우선순위**: ①제어 무수신 타이머(connection_lost `csp` — CSP 가 3s HEARTBEAT 를 보내오므로
`_lastCtrlRxAt` 1줄 + timeoutLoop 평가로 성립, 무트래픽 커버·최저 비용.
`csp_control_peer_changed` 이벤트가 같은 훅에서 무료) ②리액터 사망(worker/msrp — 결함
④의 무로그 skip 분기 포함) ③서비스 로그 쓰기 실패 + log_queue(storage_failure·resource_exhausted)
④CSP ack 소진(delivery_failed `csp/events` — 트래픽 구동 축) ⑤스토어 무트래픽 probe + ENOSPC
결함 수정 ⑥스토어 용량 임계(threshold_crossed — NAS 라 host metric 미커버) ⑦연결 누수(idle 회수
신설 — 연결 포화 행의 선행) ⑧MSRP 연결 포화·fd 고갈(resource_exhausted ×2).

### 10.4 CSC

**현행**: 알람 1종(connection_lost — DB probe) + 이벤트 3종 — 단 `config_change` 는 **선언만 있고
호출자 0건**(영구 미발화 — CSV 는 `후보` 로 분류하고 행에 예외임을 명시. "카탈로그 선언과
CSV `구현` 행 일치" 규약(§8)의 현행 유일 예외). 원인: csp 런타임 설정 CUD 가 OAM 으로
이관돼 잔재만 남음. 배선처는 CSC 가 실제 소유한 CUD(가입자/그룹/조직 — admin 13 + org 5
= 18개 지점 실측) + entity 어휘 신설(현행 `_CONFIG_EVENT_BY_ENTITY` 는
listener/trunk/route/access — CSP 잔재). mo 는 `<서버명>/csc/config/<entity>` 로
교정 완료.

**자기보고 대상이 아닌 것**: 통화이력/flow API(CSC 미서빙 — OAM 소관), FM 채널 자기장애,
요청 단위 4xx/5xx(PKCE/토큰/XCAP 인가 실패 등), 소켓 backlog/메모리(호스트 소관),
XCAP 문서 스토어(별도 스토어 없음 — 문서는 매 요청 동적 생성), 조직 동기화(`org.py` 에
CSP 통지 축 자체가 없음), HA 역할(CSC 코드에 관측 지점 0 — AGENT 소관).

**동반 발견 결함**: ①**더미 가입자/그룹 주입이 상용 경로에 상주**(`csc_app.py:242-249`) —
DB 장애를 정상 응답으로 위장 + `'Data' in config` 게이트 **밖**이라 설정 미적용 시에도
실행(두 장애 동시 위장 — 적재 실패 알람의 최대 방해물) ②`'Data' in config` 안에서만
`load_shared_data`/`apply_config` 실행(`csc_app.py:235-240`) — Data 없는 overlay 배포에서
시크릿 랜덤(경고 로그도 미출력)·notify 127.0.0.1·provisioning 영구 503·GROUP_DIR None
4연쇄 무통보(CSV `config/data` 행) ③리스너 bind 실패 은폐(`_ready_event` 가 serve 전
set — `httpsrv/server.py:47-58`) + 거짓 "started" 로그 + 스레드 사망 시 `_shutdown_event`
미set 으로 stop() 5s 블록 ④PKCE 성공 로그가 return 뒤 도달 불가 코드 ⑤IdMS 만료 정리
호출자 0(무한 누적 — refresh rotation 은 새 파일 + 구 파일 revoked 재저장만) + cleanup
CLI 는 ImportError 로 실행 불가·runtime_root CWD 폴백으로 엉뚱한 디렉터리 청소 위험
⑥`KMS_MASTER_SECRET` 설정 항목 자체가 없어 재기동마다 랜덤(설정 교정 불가 — 코드 수정
선행) ⑦admin CUD 가 in-memory USERS/GROUPS 미갱신·GMS PUT 이 DB 미반영(재기동 시 소실) —
주기 재적재가 장애 대응 겸 정합 수단 ⑧`_get_db` 2곳 산재 ⑨DB probe 의 `import pymysql`
이 run() 내부 — vendor 누락 시 스레드 즉사·connection_lost 영구 무알람 + Host 빈값 전환 시 open
고착 ⑩`/api/v1/users/me` 는 handlers/users.py 부재로 항상 ImportError 500 ⑪죽은 코드
다수(config_cache/notify_config_change/HttpClient/ha_lookup).

**우선순위**: ①HTTP 리스너 사망(listener_unavailable) ②CSP/PSP notify 미도달(delivery_failed —
무성 장애, open 훅 기성·close 는 CSC 측 응답 소비 경로 신설 필요) ③`config/data`·
`config/auth`·`config/logging`(config_invalid 3행 — ②의 원인 축이기도 함) ④service_log
쓰기 실패 + log_queue ⑤DB 쿼리 지속 실패·스키마 드리프트 ⑥IdMS 스토어 쓰기 실패 + 무한
증식 임계 ⑦가입자/그룹 적재 실패(+더미 제거·주기 재적재 선행) ⑧config_change 배선
⑨FD 스토어 probe(CMDP 와 발화 소유 1택 결정 후).

### 10.5 AGENT

agent 는 L1(프로세스 생존)·호스트 자원·드리프트·HA 판정의 감지 주체다. 원시 관측은 agent,
임계 평가는 OAM base 가 수행해도 detected_by 는 `agent`. **구현 4알람 + 1이벤트**(CSV)의
공통 경로는 metric(2s) → `_CORE_ALERT_RULES` 평가이며, **신규 metric 필드는
`agent_api.py:834-845` 화이트리스트에 추가해야 저장된다**(미추가 시 조용히 폐기).

**구조적 발견**: agent 가 이미 감지하는 전이·실패의 대부분이 **print(journald)로만 소멸**
한다 — watchdog 재기동, HA 역할 전이, 절체 래치, 배포 실패/자동 롤백, 공유 store 부적격,
keepalived 미설치 재시도, 관리평면 자기보존 발동. 후보의 3계단: ⓐ**rule 만 추가** —
수집·저장 완비 필드(cpu_pct/mem_pct/load_avg/per_iface[]/modules[].cpu·mem/로컬 mounts[]
/heartbeat ha_state.eligible·latched) ⓑ**metric/heartbeat 필드 + rule** — 판정 로직만
기성인 것(keepalived 설치·crash_loop 카운터·readiness·store probe·tooling 부재)
ⓒ**수집 확장 선행** — NFS/CIFS 마운트 사용률(`collect_per_mount` 의 `/dev/` 필터가 제외,
heartbeat `mount_targets[]` 는 사용률 필드 없음 — "rule 만 추가"는 로컬 마운트 한정),
NTP. 이벤트 후보 전량의 통로는 `module_events` 의 `node_events[]` 일반화.

**자기보고 대상이 아닌 것**: agent 자신의 heartbeat/metric POST 실패(자기보고 경로 자기장애
— OAM 의 connection_lost(AGENT) 가 정본, §10.6), 핵심 스레드 stale systemd self-kill
(`_start_watchdog_coordinator` — WATCHDOG=1 중단 → agent 강제 재기동, OAM 엔 두절로 발현 =
connection_lost(AGENT) 소관). NTP/시각 동기는 사내 vIBCF/TrGW POD 대조(표준화 §7.2)를
근거로 **후보(선행 필요)** — 검사 코드 신설이 선행(CSV `ntp` 행).

**동반 발견 결함/주의**: ①install root 오해석 시 false module_down·config_drift 미평가를
유발하는데 자기검증 없음(`_resolve_prefix` 조용한 폴백) ②sudo 미등록 시 rc=0 graceful skip —
조작이 무시됐는데 성공 보고 ③pending report 큐 무한 append(상한 없음) ④cims-svc/cims-priv
부재 시 조용한 실패 — 모든 재기동·절체 기동 불가 ⑤`ip -j` 실패 시 interfaces[] 를 **빈
배열로 위장 상행** — OAM vip_observation 이 이를 정본으로 써 계획 절체·auto-sync 판정 오염
⑥stale MASTER(role 파일=MASTER, VIP 미보유)를 무로그 BACKUP 재판정 — dual-active 직전
상태가 무관측 ⑦ha_flap 집계의 tail 32KB·200줄 상한(초과 시 과소집계) + 로그 디렉터리 부재
시 `{}` 반환 → auto-close 로 '해소' 위장 ⑧`process_died` 의 `_PREV_RUNNING_MODULES` 가
in-memory — agent 재기동 후 첫 tick·2s 창 내 죽었다 살면 유실 ⑨`_fail_bump` 는 `was_up`
전제 — 한 번도 뜬 적 없는 모듈의 start 반복 실패는 crash_loop 미카운트(CSV `start` 행 분리
사유) ⑩latch heartbeat 에 `latched` bool 만 — `latched_at`/사유 미전달(알람 메시지 재료
부족) ⑪module config.json 파싱 실패 시 hash 보고 생략 — 깨진 설정이 config_out_of_sync 무알람 통과
⑫maintenance(EXCLUDE_NODE) 파일은 TTL 없음(planned_release 만 180s 자가치유) — 해제 망각이
영구 부적격.

### 10.6 OAM (base) / OAM-SVC

oam-svc 는 별도 감지 로직이 없다 — base 코어(`alarm_sweeper`)를 공유하고 **소유**(서비스
계열 sweeper + FM ingest)만 다르다. `--role all` 에서는 base 가 대행(detected_by=`oam`).

**최대 발견 — "알람 없음"이 "감지 불능"을 뜻하게 되는 침묵 실패**:
1. **agent 관측 두절**: 두절 노드(offline·metric 전무)는 `A-COM-015`
   connection_lost(`<서버명>/agent`, check=agent_lost)가 열리고 그 노드의 열린 agent
   알람은 "판정 불가" 메시지로 종결된다(파이프라인 §9 — 노드 사망이 전 알람 해소로
   위장되지 않음. agent 스토어 공백이면 무판정). 잔여 사각: **stale metric**(hb 정상 +
   metric 갱신 정지 — jsonl_last 가 옛 metric 을 계속 반환)과 규칙별 데이터
   미보고(cfg_hashes/ha_transitions 부재)·cold_skip 은 여전히 `msg_close`("정상화")로
   close — `observability_lost`(metric blackout, N주기 stale 전이) 신설이 남은 과제.
2. **리스 상실/획득 실패**: read-only 강등 + **base sweeper 10종 전부 정지**가 로그로만
   남는다(`oam_app.py:625-636`, 게이트 `:1559-1578`). 단 정지 범위는 base 한정 — oam-svc
   서비스 알람(process_unresponsive/connection_lost/threshold_crossed)은 리스 게이트가 없어 계속 발화한다. →
   `redundancy_degraded`(store/lease) 신설(+ NAS flock no-op 는
   `dependency_unavailable`(store/lock) 별행 — 펜싱 부재 = 손상 위험 상시라는 다른
   의미의 sticky 조건).
3. **FM ingest 불능/카탈로그 무력화/자기보고 두절**: ingest 기동 실패 시 그냥 진행, 수신
   스레드는 소켓 오류에 조용히 종료(재기동 없음), 빈 카탈로그·UNKNOWN_CODE·UNREGISTERED
   거절은 카운터/로그 0. FM_SYNC 3회 누락은 "판정 불가 close" 만 하고 **두절 자체는
   무알람**인데 `last_sync=now` 리셋으로 흔적까지 소멸(`fm_ingest.py:307-324`) —
   `connection_lost`(fm_sync/<node>) 가 L2 침묵의 유일 관측점.
4. **HA 실측 판정의 소비처 부재**: VIP holders 0명/2명(`ha_lookup.py:79-113` — 계산 완비,
   active=None 으로 뭉갬), fleet misdirect(`_agents_not_on_vip` 완전 구현 — 콘솔·preflight
   만 소비), ha_excluded(콘솔 직렬화만 — 주석 자체가 실측 사고 언급) — 전부 60s 주기로
   이미 돌고 있어 **rule 소비만 얹으면 되는** 최저비용 고가치 군.

**이벤트 스트림 공백**: 현재 event_log 에 실리는 것은 `service_control`·`process_died`·
FM_EVENT 중계뿐. agent offline/online 전이, HA 절체 사건(+계획 절체 FAILED), 배포/
프로비저닝 job 결과, 인증 감사(로그인 실패·역할 거부·join 토큰 오사용)가 전부 앱 로그/
레코드에만 있다 — "OAM 이 한 일"이 이벤트 스트림에 거의 없다. 배포 job 실패 이력은
retention(2일)으로 소멸까지 한다.

**동반 발견 결함**: ①`transition` 이 state 를 emit 전에 갱신(open 뿐 아니라 close/change
도 — `alarm_sweeper.py:137-143,:147,:155`) + `daily_jsonl.record` 무보호 — ENOSPC/NAS
두절 시 open 레코드 유실·콘솔 영구 미표시, FM 경로는 ack 미발신 + 재전송이 "이미 열림"
분기로 영구 유실 ②`process_unresponsive` 가 "무응답"과 "대상 미설정"을 구분 안 함 —
CspNotify 미설정 시 기본 127.0.0.1 probe 로 영구 오탐 + **연속 실패 디바운스 없음**(1회
miss 즉시 open — DB probe 3연속 표준과 불일치) + `_probe_cmp` 가 config 파라미터 없이
호출돼 `MediaServer.Probe*` 설정 무시·last-good 30s 고정 ③CMP 관측 활성 판정식은
`Endpoints or CmpIp` — 공집합 시 관측 통째 비활성 + 열린 알람 stale reap 의 close 메시지가
`msg_close`("정상화") 우선(폴백 문구 거의 미사용 — 설정 소실이 알람 정리로 위장)
④agent 계열 `restore_open_state` 실패 시 drift 의 `_reseed_if_empty` 등가 자가복구 없음 +
**복원 창 불일치**(기본 30일 vs drift 90일) — 30일+ 지속 알람은 재기동 시 중복 open·앞선
open 영구 미해소 ⑦분리 배포 시 base·oam-svc 가 같은 alerts jsonl 동시 append(`_write_lock` 은
프로세스 로컬 — 교차 프로세스 무보호) ⑧cert_rotate_pending 이 한 번 set 되면 재평가 skip —
agent 미호출 시 재시도·에스컬레이션·흔적 없음.

## 11. 구현 이행 절차

감지 행이 없는 정의(§2 — 감지 주체 미배정)를 구현으로 옮기는 절차:

1. 항목별 **감지 주체 배정** — L1(agent)/L2(자기보고)/L3(OAM probe), 표준화 §3.4(b).
2. **선행 구현 식별** — 카운터/상한/수집 필드 신설이 필요한 항목 구분(예: 성공률·CPS·세션
   상한·NTP·operstate — 실사 결과는 표준화 §7.2.1 대조표).
3. **감지 행 등록** — 그 정의 아래에 감지 방식·코드 근거와 함께 행 추가, 구현 완료 시
   `구현` 표기.
4. fm_catalog/rule 구현 + 콘솔 노출. `영향`/`권장 조치` 는 카탈로그(fm_catalog·rule)의
   effect/recommended_action 으로 탑재(표준화 §7.1) — 코드별 조치 절차서(POD 형식) 생성의
   원천으로도 사용.

## 관련

- [alarm_catalog.csv](alarm_catalog.csv) — **목록 정본**
- [alarm_standardization.md](alarm_standardization.md) — 알람 모델·감지 3계층·code 체계·vIBCF 대조(§7.2)
- [alarm_self_reporting.md](alarm_self_reporting.md) — 자기보고(FM push) 경로·wire 규격 정본
- [alarm_pipeline.md](alarm_pipeline.md) — 발생→가시화 전 구간 절차 정본
- [vibcf_pod_alarms.md](vibcf_pod_alarms.md) — 사내 vIBCF/TrGW POD 변환 참고자료 (대조·채용
  포인트는 표준화 §7.2)
