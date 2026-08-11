# IMS 기능 관점 필요 알람/이벤트 카탈로그 — 설명서

목록 정본은 **[alarm_function_catalog.csv](alarm_function_catalog.csv)** 이고, 본 문서는 그
범위·컬럼 정의·수록 원칙·기능별 설계 메모를 기술한다. 목록 자체는 여기에 중복하지 않는다.

이 카탈로그는 **현재 구현 여부와 무관하게**, CSCF·IBCF·TAS·PTT-AS·MRF 기능을 가지는 IMS
서버가 운영 관점에서 **갖춰야 할** 알람/이벤트를 기능별로 정리한 **요구(요건) 정본**이다.
구현은 이 카탈로그를 기준으로 추후 별도 진행한다.

## 1. 문서 가족에서의 위치

| 문서 | 축 | 담는 것 |
|---|---|---|
| [alarm_standardization.md](alarm_standardization.md) | 모델 | 알람/이벤트 모델·severity·code 체계·감지 3계층·벤치마크 대조(§7.2 vIBCF) |
| **본 카탈로그** | **요구(what)** | 기능(IMS 역할) 관점에서 필요한 알람/이벤트 — 구현 무관 |
| [alarm_module_catalog.md](alarm_module_catalog.md)/.csv | 구현(how·현황) | 모듈 자기감지 관점 전수 목록 — 감지 주체·코드 근거·구현 상태 추적 |
| [alarm_self_reporting.md](alarm_self_reporting.md) | 경로 | 자기보고(FM push)·이벤트 스트림 wire/저장 규격 |
| [alarm_pipeline.md](alarm_pipeline.md) | 절차(end-to-end) | 발생→전달→수집/보관→가시화 구간별 계약·수렴 보장 |

요구 항목이 구현 채택되면: 감지 주체(L1/L2/L3)를 배정하고 모듈 카탈로그에 행이 생기며
(선행 구현 포함), fm_catalog/rule 로 구현된다 — 본 카탈로그는 그 기준선으로 유지한다.

### 1.1 기능(IMS 역할) ↔ CIMS 실체 매핑

기능 축이 역할 관점인 이유는 CIMS 모듈이 IMS 노드 역할을 겸장하기 때문이다. **역할 명칭은
`기능` 컬럼(절)에만 쓰고, `대상`/`component` 등 실체 지시는 CIMS 모듈/객체 명칭을 쓴다.**

| IMS 역할 (기능 축) | CIMS 실체 |
|---|---|
| CSCF · IBCF · TAS · PTT-AS | **CSP** |
| HSS(가입자 원천) · CSC(IdMS/GMS/CMS/XCAP) | **CSC** (+ 서비스 DB) |
| MRF (미디어 릴레이·floor·믹스·녹취 + MCData 미디어) | **CMP** (+ CMDP) |

## 2. 컬럼 정의

| 컬럼 | 의미 |
|---|---|
| `기능` | IMS 기능 역할 — `공통`(전 기능/노드 공유) / `CSCF`(P/I/S — 등록·세션 제어·라우팅) / `IBCF`(경계·NNI) / `TAS`(부가서비스·호 이력) / `PTT-AS`(MCPTT AS) / `MRF`(미디어 자원 — relay·floor·믹스·녹취·MCData 미디어 포함) |
| `구분` | 알람(지속 상태, open/close) / 이벤트(전이·감사 통지) |
| `code` | **정의 코드** `A-<DOMAIN>-NNN` / `E-<STC\|AUD>-NNN` — **행당 유일**(알람·이벤트 공통), flat(표준화 §3.4(a)). 운영 사전(dictionary)·코드별 조치서(POD)·NMS 연동의 키. `NNN` 은 스트림+도메인 내 무의미 일련. 도메인: 알람 = eventType 약어(PRC/COM/QOS/SEC…), 이벤트 = kind 약어(STC/AUD). 구현 기성 클래스의 대표 정의는 구 클래스 코드 번호 승계(§3.4(a) 표) |
| `type` | 조건/성격 **클래스** 슬러그 — 알람은 [alarm_module_catalog.md](alarm_module_catalog.md) §2.3 의 20클래스, 이벤트는 표준화 §3.6 의 9클래스. 클래스는 코드를 갖지 않는다(슬러그가 식별자 — 분류 정정은 코드 불변인 채 가능). 이벤트의 **정의 슬러그**(wire type — `ha_switchover` 등)는 `조건` 서두의 `event=` 표기가 보유(정의당 유일) |
| `severity` | 권고 perceivedSeverity(단계 임계는 `minor~critical(단계)` 표기). 이벤트는 `-` |
| `대상` / `component` | 모듈 카탈로그 §2.1/§2.2 규약 준용 — 대상=외부 의존 객체만, component=활성키 세그먼트(`<...>`=런타임 치환·개별 발화). 런타임 mo_instance 실체화는 표준화 §3.4(b) 소유 주체 루트(서버명/그룹명) 규약을 따른다. **값은 CIMS 실체 명칭**(DB·CSC·CMP·CSP·PEER — §1.1 매핑)이다. 규격 노드명(HSS/MRF/TrGW/AS)은 대상에 쓰지 않는다 — 역할 대응은 `조건`/`근거` 의 괄호 주석("HSS 역할" 등)이 보유. CIMS 에 실체가 없는 외부 시스템 연동(과금 등)은 수록하지 않는다 — 도입 시 행 신설 |
| `조건` | 무엇이 일어나고 있는가(specificProblem). 이벤트는 `event=<정의 슬러그>` 를 앞에 표기(kind 는 code 도메인 STC/AUD 에서 도출) |
| `message` | **발화 시 레코드 message 의 영문 템플릿**(vIBCF POD "장애 설명" 관례 채용 — §7.2). `{param}` 치환(구현 fm_catalog `msg_open`/rule `msg` 와 같은 관례), 단계 임계 계열은 vIBCF 식으로 관측값·단계 임계 동반 표기(`(CRI:{crit}, MAJ:{maj}, MIN:{min})`). 알람 close 메시지는 구현 시 `msg_close` 로 확정("... restored/cleared" 형) — 카탈로그는 open 템플릿만 보유. 이벤트는 통지 메시지 템플릿 |
| `영향` / `권장 조치` | effect / recommended action — 표준화 §7.1 의 운영 runbook 필드. vIBCF POD 의 "전 코드 조치사항" 관례 채용 |
| `근거` | 표준(TS/RFC/X-series) 또는 벤치마크(vIBCF 코드) 참조 |

## 3. 수록 원칙

- **기능 고유 축만 수록한다.** 전 기능이 공유하는 조건(프로세스 생존·호스트 자원·설정·이중화·
  관측 등)은 `공통` 절이 담당하고 각 기능 절에 재수록하지 않는다. 단 같은 조건이라도 기능에
  따라 영향·severity 가 본질적으로 다르면 기능 절에 둔다(예: TAS/PTT-AS 의 CMP 두절).
  이렇게 **여러 기능 절이 같은 CIMS 실체를 향하는 행들**(IBCF·TAS·PTT-AS 의 CMP 두절 —
  런타임 mo 동일)은 요구 축이 다른 별개 정의다 — 구현 채택 시 감지 경로는 하나로 통합될
  수 있고, 그 경우 발화는 대표 정의 하나로 수렴시킨다(같은 mo 에 중복 발화 금지 —
  영향/조치는 역할 합집합으로 기술).
- **code 유일 불변식**: 정의 코드는 행당 유일하다(알람·이벤트 공통) — 정의 하나 = 코드
  하나. 같은 정의의 다중 런타임 객체(대국·접속점·풀·그룹)는 코드를 늘리지 않고
  mo(`대상`/`component`)로 구분한다(개별 발화). 활성 알람 식별키 = (정의 코드, mo_instance).
  결번은 재사용하지 않는다(표준화 §3.4(a)) — 현재 결번: `E-AUD-001`.
- **알람 = 상태 전이로 표현 가능한 지속 조건**, 요청 단위 일회성 이상은 로그/PM 소관
  (표준화 §3.6). 반복 오류는 카운터/율 임계로 전이를 만들어 수용한다(성공률·rx_error 등) —
  vIBCF F 계열 대조의 확정 판정(표준화 §7.2.2, 제3 스트림 미신설).
- 한 조건 = 한 행. 객체 다수(접속점·대국·풀·그룹)는 행을 늘리지 않고 "개별 발화"로 서술.
- 쌍 이벤트(added/removed·entered/exited·blocked/unblocked)는 `*_changed` 1행으로 적는다 —
  행은 대표 표기이며, wire `type` 은 방향별 슬러그로 분리될 수 있다(모듈 카탈로그의
  listener_added/removed 등). 요구 정의↔wire 슬러그 매핑은 구현 이행 시 확정(§6).
- **이벤트도 알람과 대칭으로 분류·코드화한다**(표준화 §3.6) — kind(STC/AUD 도메인) →
  성격 클래스(type, 9종) → 정의 코드(`E-<STC|AUD>-NNN`) → 인스턴스(mo). 정의 슬러그
  (`event=`, wire type)는 카탈로그 전체에서 정의당 유일해야 한다(같은 슬러그 = 같은 정의).

## 4. type 체계 — 신설 제안 1건

20클래스 체계(모듈 카탈로그 §2.3)를 그대로 쓰되, 본 카탈로그가 **1클래스 신설을 제안**한다:

| type | 정의 | 근거 |
|---|---|---|
| `security_violation` | **보안 이상 징후의 급증**(인증 실패 폭주·사기 호/스캐너 탐지·비인가 트래픽) — 율 임계 기반 open/close | X.736(Security Alarm). 20클래스 중 흡수처 없음 — threshold_crossed 는 기계적으로 가능하나 보안 알람은 X.733 계열과 통지 성격(대응 주체·민감도)이 달라 클래스 분리가 표준 정합. DOMAIN `SEC` 신설 동반(표준화 §3.4(a) — 정의 코드 `A-SEC-NNN`) |

채택 시 모듈 카탈로그 §2.3 과 표준화 §3.3 매핑 표에 편입한다.

## 5. 기능별 설계 메모

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

## 6. 구현 이행 절차 (추후)

1. 항목별 **감지 주체 배정** — L1(agent)/L2(자기보고)/L3(OAM probe), 표준화 §3.4(b).
2. **선행 구현 식별** — 카운터/상한/수집 필드 신설이 필요한 항목 구분(예: 성공률·CPS·세션
   상한·NTP·operstate — 실사 결과는 표준화 §7.2.1 대조표).
3. **모듈 카탈로그 등록** — 감지 방식·코드 근거와 함께 행 추가, 구현 완료 시 `구현` 표기.
4. fm_catalog/rule 구현 + 콘솔 노출. `영향`/`권장 조치` 는 카탈로그(fm_catalog·rule)의
   effect/recommended_action 으로 탑재(표준화 §7.1) — 코드별 조치 절차서(POD 형식) 생성의
   원천으로도 사용.

## 관련

- [alarm_function_catalog.csv](alarm_function_catalog.csv) — **목록 정본**
- [alarm_standardization.md](alarm_standardization.md) — 모델·code 체계·vIBCF 대조(§7.2)
- [alarm_module_catalog.md](alarm_module_catalog.md) — 모듈 자기감지 구현 추적
- [vibcf_pod_alarms.md](vibcf_pod_alarms.md) — 사내 vIBCF/TrGW POD 참고자료
