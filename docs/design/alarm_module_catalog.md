# 모듈 자기감지 알람/이벤트 카탈로그 — 설명서

목록 정본은 **[alarm_module_catalog.csv](alarm_module_catalog.csv)** 이고, 본 문서는 그 CSV 의
**범위·컬럼 정의·판정 기준·모듈별 메모**를 기술한다. 목록 자체는 여기에 중복하지 않는다.

CSV 는 각 모듈이 **스스로 인지(감지)해 발생시킬 수 있는** 알람/이벤트를 전수 열거한다.
용도는 두 가지다: ① 현행 감지 대비 **누락 알람 후보의 식별** ② 운영/NMS 연동용 알람
사전(dictionary)의 원천.

## 1. 범위 원칙

- **행의 소속 = 감지 주체.** 모듈이 **대상(mo)** 인 알람 전체 목록이 아니라, 그 모듈이
  **감지해서 발화하는** 목록이다. 감지 3계층([alarm_standardization.md](alarm_standardization.md)
  §3.4(b))과의 대응: 서비스 모듈(CSP/CMP/CMDP/CSC) 행 = L2 자기보고(detected_by=`self`),
  AGENT 행 = L1 생존 + 호스트 자원/드리프트(detected_by=`agent` — 원시 관측은 agent,
  임계 평가는 OAM base 가 수행해도 감지 주체 클래스는 agent), OAM/OAM-SVC 행 = L3 원격
  probe·OAM 자체 판정·OAM 자신의 내부 상태. 예컨대 "CSP 프로세스 사망"은 CSP 행이 아니라
  AGENT 행이다 — 죽은 프로세스는 자기보고를 못 한다.
- **알람 후보의 기준은 상태 전이(open/close)로 표현 가능한 지속 조건.** 요청 단위 일회성
  오류(SIP 파싱 실패·인증 실패·호별 처리 실패)는 알람 부적합이며 로그/PM metric 소관이다.
  전이 없이 반복되는 오류는 카운터/율(rate) 임계로 전이를 만들 수 있을 때만 후보가 된다.
- **기동 자체가 불가한 조건은 제외.** 포트 bind 실패로 즉시 종료하는 경우처럼 프로세스가
  사망하는 조건은 L1(agent `process_down`) 소관이다. 반대로 *기동은 됐는데 일부 기능만
  불가*한 상태(리스너 일부 실패 등)는 L2 후보다 — 이 경계가 CSV 판정의 핵심이다.
- **자기보고 경로 자체의 장애는 자기보고할 수 없다.** FM 채널 두절·카탈로그 부재는 OAM 의
  "FM_SYNC 3회 누락 → 판정 불가 close"([alarm_self_reporting.md](alarm_self_reporting.md) §5)가
  담당한다.

## 2. 컬럼 정의

| 컬럼 | 의미 | 값 규약 |
|---|---|---|
| `구분` | 알람 / 이벤트 | X.733 알람 vs X.730/731/740 통지 — 표준화 §3.6 의 스트림 분리 |
| `code` | 알람 클래스 코드(카탈로그 식별자) | `<SERVICE>-<DOMAIN>-<SEQ>` (표준화 §3.4(a)). 이벤트는 `-`(코드 없음). 신규 조건 클래스 후보는 `(신규)` — 코드는 채택 시점에 부여한다 |
| `type` | 조건 클래스 슬러그 | code 와 1:1. 프로세스명·리소스명·임계를 넣지 않는다(표준화 §3.5) |
| `severity` | perceivedSeverity | critical/major/minor/warning/indeterminate. **이벤트는 `-`** (통지는 severity 없음) |
| `source_system` | **발신 노드**(호스트/논리 노드) | CSV 값은 대표 배치의 노드 예시이며, 실제 값은 모듈 SystemId(= FM envelope `hdr.node`) |
| `instance` | **발신 모듈** | CSP/CMP/CMDP/CSC/AGENT/OAM/OAM-SVC |
| `index` | 해당 노드 내 발신 모듈의 인스턴스 번호 | 통상 `1`. 한 노드에 같은 모듈을 다중 기동하는 배치에서만 2 이상 |
| `대상` | 알람이 가리키는 **외부 의존 객체의 인스턴스** | 아래 "대상 표기 규칙" 참조. 해당 객체가 없으면 **비움** |
| `알람/이벤트 내용` | specificProblem — 무엇이 발생했는지 | 괄호 안은 메시지에 실릴 파라미터(`params` 치환값) |
| `현재 구현 여부` | `구현` / `후보` / `후보(선행 필요)` | `후보(선행 필요)` = 감지 코드나 전이 지점 자체가 없어 **선행 구현 없이는 알람화 불가** |
| `감지 방식 설명` | 어떤 관측으로 open/close 를 판정하는가 | 주기·임계·연속 실패 횟수 + 코드 위치(`파일:라인`). 이벤트는 `kind=stateChange\|audit` 를 앞에 표기 |

`code` 가 `(신규)` 인 행은 기존 클래스로 흡수되지 않는 **새 조건**임을 뜻한다. 같은 조건이면
기존 code 를 재사용하고 객체만 `대상`으로 구분하는 것이 원칙이다(표준화 §3.5) — 그래서
"CMP 두절"과 "DB 두절"이 같은 `CIMS-COM-001` 로 나타난다.

### 2.1 대상 표기 규칙 (★)

`대상` 은 **그 자체로 식별되는 객체가 있을 때만** 적는다 — 즉 모듈 외부의 의존 시스템/피어
노드다. 기록·조회 관점에서 값이 명확할 때만 컬럼을 채우는 것이 원칙이며, 억지로 채우면
컬럼의 의미가 흐려진다.

| 채운다 | 비운다 |
|---|---|
| 외부 의존 시스템·피어 노드: `DB` · `CMP_01` · `CMDP_01` · `CSC_01` · `PEER_01` | 모듈 자기 내부 구성요소: 로그/호이력 파일, 자기 listen 포트, 설정, 큐, HA 역할, 과부하 상태 |
| 인스턴스가 여럿이면 인스턴스명으로 구분(`CMP_01`, `CMP_02`) | 프로세스 자체가 객체인 것(기동/종료 이벤트) |
| 집합 전체가 조건의 객체면 클래스명(`CMP` — "전체 CMP 두절") | 객체 식별이 무의미하거나 파라미터로 충분한 것(그룹 gid, proto/port) |

- **비운 경우 그 객체는 `알람/이벤트 내용` 에 서술한다** — 예: "호이력(CDR) 파일 쓰기 실패",
  "CSC 연동용 UDP 포트 바인딩 실패(4421)", "SIP 리스너 개설 실패(proto, port)".
- 같은 조건이 객체마다 개별 발화하는 경우(CMP endpoint·리스너)는 CSV 행을 늘리지 않고
  대표 1행으로 적고, "endpoint 마다 개별 발화" 를 `감지 방식 설명` 에 남긴다.

## 3. mo_instance 매핑

CSV 컬럼이 wire/저장 레코드의 `source.mo_instance` 를 구성한다
(alarm_self_reporting §4 의 `cims/<module>/<node>[/<component>]`):

```
cims / <instance> / <source_system> / <대상>      ← 대상 있음
cims / <instance> / <source_system>               ← 대상 비움 (모듈 자체가 객체)
  예) cims/csp/SIG_SVR_01/db
      cims/csp/SIG_SVR_01/cmp/192.168.1.10:9000   ← 대상 CMP_01 의 실제 endpoint 키
      cims/csp/SIG_SVR_01                          ← 리스너/설정/로그 계열
```

- 활성 알람 식별키는 `(code, mo_instance)` 다. 따라서 **대상을 비운 조건들은 code 가 서로 달라야
  활성키가 겹치지 않는다** — 같은 code 로 여러 내부 조건을 발화시키려면 component 세그먼트를
  부여해야 한다(예: `…/SIG_SVR_01/call_dir`). CSV 의 `(신규)` 클래스 3종이 이 사유로 분리됐다.
- 노드 분리를 빠뜨리면 HA 다중 인스턴스에서 활성키가 충돌한다 (cmp 변종 pmp/imp 의
  SystemId overlay 필수 사유와 동일).
- **AGENT/OAM 행의 mo 는 위 규칙이 아니라 sweeper 합성 규칙을 따른다** — agent 계열
  `<host>/<module|disk|…>`, 서비스 probe `cims/<target>`, HA fan-out `cims/ha/g<gid>/<collection>`
  (표준화 §3.4(b)). CSV 의 `instance` 컬럼은 어느 쪽이든 **발신(감지) 모듈**이다.
- `detected_by` 는 CSV 에 컬럼을 두지 않는다 — `instance`(감지 모듈)가 주체 클래스를
  결정한다 (CSP/CMP/CMDP/CSC→`self`, AGENT→`agent`, OAM-SVC→`oam-svc`, OAM→`oam`).
- `source_system` 예시 표기: `SIG_SVR_01`(csp 계열) · `MED_SVR_01`(cmp/cmdp) ·
  `SUB_SVR_01`(csc) · `MGMT_SVR_01`(oam/oam-svc) · agent 는 상주 호스트(예시 `SIG_SVR_01`).

## 4. CSV 취급 규약

- UTF-8, 헤더 1행. 쉼표를 포함하는 필드는 `"..."` 로 인용한다(엑셀/`csv` 모듈 호환).
- 한 조건 = 한 행. 같은 조건의 객체 확장(endpoint·리스너 다수)은 행을 늘리지 않는다(§2.1).
- 구현 상태(`현재 구현 여부`)는 코드 정본과 함께 갱신한다 — 후보가 구현되면 `구현` 으로
  바꾸고 `code`/`severity` 를 확정값으로 채운다. 카탈로그 선언(`fm_catalog.json`)과 CSV 의
  `구현` 행은 항상 일치해야 한다.

## 5. 모듈별 메모

CSV 에 담기지 않는 판정 근거·경계·동반 결함을 모듈별로 남긴다.

### 5.1 CSP

**현행**: 알람 1종(COM-001 DB 두절) + 이벤트 2종(process_started/stopping). fm_catalog.json
선언과 구현이 일치한다.

**자기보고 대상이 아닌 것 (경계 확정)**
- **SIP 수신 소켓 버퍼 overflow / 스레드풀 포화**: 프로세스 내부 관측 지점이 없다(커널·psip
  영역) — host metric(agent) 소관.
- **세션/트랜잭션/nonce/구독 맵 포화**: 상한·포화 분기가 없고 시간 기반 청소만 있다 — 크기
  임계를 신설하기 전까지 알람화 불가.
- **TLS 인증서 만료 예고**: 모듈·스택에 X509 notAfter 검사가 없다. 파일 기준 검사는
  agent/OAM 경로가 적절(리스너 개설 실패는 별개로 CSV 에 있음).
- **CMP/CMDP 요청 단건 타임아웃**, per-call 실패, 파싱 실패: 일회성 → 알람 부적합. 부하
  지표로 쓰려면 율 카운터 신설이 선행.

**동반 발견 결함** (알람화와 별개로 수정 대상)
1. **SIGUSR1/mtime scalar 설정 재적재가 항상 무동작** — 무인자 `CSipServerSetup::Read()`
   (`SipServerSetup.cpp:573-584`)가 JSON 설정 파일을 `CXmlElement::ParseFile` 로 읽어 항상
   false 를 반환하고, 호출자(`CspServer.cpp:446`, `:512`)는 반환값을 버린다(로그 0).
   jsonl 컬렉션 reload 경로는 정상.
2. CMP 응답 status≠OK 가 무로그 실패(`CmpClient.cpp:584-593`) — 포화 미탐 구간의 실패 원인
   규명 불가. 최소 로그 + status code 기록 필요.
3. `CCscInterface::Start()` 가 bind 결과와 무관하게 true 반환(`CscInterface.cpp:44-50`) —
   `CspServer.cpp:355` 의 체크가 무의미.
4. CDR/flow 로그 쓰기 실패 전 지점 silent + `m_ulDroppedLogs` 카운터 소비자 0.
5. fm_catalog.json 의 COM-001 `effect` 가 "파일 fallback 범위로 축소"라고 서술하나, 런타임
   fallback 전환이 실제로 없다(기동 시 1회 평가) — 문구 정정 또는 전환 구현 중 하나가 필요.
6. route `max_concurrent_calls`/`cps_limit` 가 파싱만 되고 사용처 없음
   (`CspRouteMap.cpp:51-52`) — 과부하 알람의 선행 조건과 연관.

**구현 우선순위** (훅 비용 대비 가치)

| 순위 | 항목 | 근거 |
|---|---|---|
| 1 | CMP endpoint 두절 (COM-001) | 전이 훅 기성, 미디어 전면 장애 직결 |
| 2 | service_log/CDR 쓰기 실패 (PRC-002) + 큐 drop 병합 | 현재 완전 silent, 과금·이력 유실 |
| 3 | 리스너 개설 실패 (신규 클래스) | TLS 포함 접속점 상실, reload 재평가로 전이 자연 |
| 4 | CMDP 두절 (COM-001) | 콜백 배선 1곳 |
| 5 | CSC 리스너 개설 실패 (COM-001) | 결함 3 수정 동반 |
| 6 | DB 쿼리 지속 실패·재적재 실패 (PRC-002) | COM-001 이 못 잡는 구간 |
| 7 | 설정 무결성 위반 (신규 클래스) | 산재한 7개 지점을 1클래스로 통합 |
| 8 | 이벤트 ha_role_changed · config_reloaded | 전이 훅 기성 |

### 5.2 CMP

**현행**: 알람 1종(QOS-002 자원 풀 완전 고갈 — rtp/ptt_floor/ptt_member 풀별) + 이벤트 2종.

**자기보고 대상이 아닌 것**: 코덱/믹서 이상(CMP 는 투명 relay — 코덱·믹싱 코드 자체가 없음,
PT 는 헤더 1바이트 스탬프), HA 역할 전이(All-Active — keepalived/standby 코드 없음, 절체
관측은 CSP CmpClient 소관), 사용률 단계 임계(OAM sweeper QOS-001 역할 분담), floor 타이머
만료·대기열 Deny(정상 규격 동작), 디스크 여유(agent 소관).
`config_reloaded` 이벤트도 N/A — **런타임 설정 재적재 경로 자체가 없다**(SIGHUP/SIGUSR1
핸들러 없음, 전 필드 restart 요구).

**동반 발견 결함**: ①QOS-002 의 relay 분모가 설정값(`_rtpPoolSize`)이라 실측 풀과 불일치 +
`total<=0` 풀 판정 제외로 전량 bind 실패가 무알람(`PCmpServer.cpp:168,181`) ②`_rtpStartPort`
등 5개 멤버 미초기화(생성자 목록·헤더 기본값 없음) ③설정 `CspIp/CspPort` 파싱 안 됨(통지
대상은 학습값 전용) ④녹취 인덱스 read 실패와 파일 부재를 구분 못 해 seq 재시작 → 기존
세그먼트 덮어쓰기 위험(`PSyncRtpRecorder.cpp:142-145`) ⑤제어 소켓 recvfrom 오류 무처리
(무로그 tight loop) ⑥SIGINT/SIGQUIT 미처리로 process_stopping 유실 ⑦mkdir/rename/fclose
반환값 전면 미검사.

**우선순위**: ①풀 부분 bind 실패(capacity_degraded — QOS-002 사각 커버) ②flow/msg 로그
쓰기 실패+drop 병합(PRC-002) ③녹취 쓰기 실패(PRC-002 — self_reporting §9 후보 그대로)
④리액터 사망(worker_unavailable — L1/L3 모두 미검출) ⑤CSP 무통신(COM-001, 선행 필요)
⑥sweeper 회수 지속(resource_leak).

### 5.3 CMDP

**현행**: 알람 1종(PRC-002 FD 스토어 저장 실패) + 이벤트 2종. 현행 PRC-002 의 한계 2건이
핵심 — **트래픽 구동 판정**(무트래픽 구간 미탐·stale)과 **write 결과 미검사**(ENOSPC 가
저장 성공으로 보고 — `PFdStore.cpp:71-83`).

**자기보고 대상이 아닌 것**: 리스너 bind 실패(즉시 종료 → L1 소관 — cmdp 는 런타임 재개설
경로가 없어 런타임 bind 실패 자체가 없음), MSRPS/TLS(미구현 — 도입 시 listener_unavailable
클래스에 흡수), 세션 맵/송신 큐 포화(상한·관측 지점 없음 — 선행 필요), fm_catalog 부재
(자기보고 경로 자기장애 — OAM 소관), 413/415/TLV 파싱 실패(요청 단위).

**동반 발견 결함**: ①FD 스토어 ofstream write/flush 미검사(위) ②로그 버킷 mkdirP 반환
무시 후 경로 확정(`PCmdpServer.cpp:1089`) ③제어 recvfrom 오류 무한 busy spin + 스레드
detach 라 생존 확인 불가(`:176-184`) ④커넥션이 워커 수와 무관하게 항상 `_reactors[0]` 에
등록 — MsrpWorkerCount 사실상 무효(`:589,593,789`) ⑤send 오류 시 markClosed 미호출 —
좀비 연결이 연결 상한 잠식(`PMsrpConnection.cpp:85`) ⑥`Fm.Enable` 문자열 비교 — boolean
설정 시 자기보고 무음 정지(`:1009`) ⑦PRC-002 통지에 errno/path params 없음(runbook 이
구분을 요구하는데 재료 미탑재).

**우선순위**: ①리스너/리액터 사망(listener_unavailable) ②서비스 로그 쓰기 실패(PRC-002)
③CSP ack 소진(COM-001 — cmdp 유일의 피어 두절 관측점) ④스토어 무트래픽 probe + 결함 수정
⑤스토어 용량 임계(NAS 라 host metric 미커버) ⑥MSRP 연결 포화(QOS-002).

### 5.4 CSC

**현행**: 알람 1종(COM-001 DB probe) + 이벤트 3종 — 단 `config_change` 는 **선언만 있고
호출자 0건**(영구 미발화). 원인: csp 런타임 설정 CUD 가 OAM 으로 이관돼 잔재만 남음.
배선처는 CSC 가 실제 소유한 CUD(가입자/그룹/조직 18개 지점) + mo 규약 교정
(`cims/csp/config/*` 오기재 → `cims/csc/<node>/config/<entity>`).

**자기보고 대상이 아닌 것**: 통화이력/flow API(CSC 미서빙 — OAM 소관), FM 채널 자기장애,
요청 단위 4xx/5xx(PKCE/토큰/XCAP 인가 실패 등), 소켓 backlog/메모리(호스트 소관).

**동반 발견 결함**: ①**더미 가입자/그룹 주입이 상용 경로에 상주**(`csc_app.py:243-249`) —
DB 장애를 정상 응답으로 위장(적재 실패 알람의 최대 방해물) ②`'Data' in config` 안에서만
`load_shared_data`/`apply_config` 실행 — Data 섹션 없는 overlay 배포에서 시크릿 랜덤·notify
목적지 127.0.0.1·provisioning 영구 503 이 무통보(`csc_app.py:235-240`) ③리스너 bind 실패
은폐(`_ready_event` 가 serve 전 set — `httpsrv/server.py:54`) + 거짓 "started" 로그 ④PKCE
성공 로그가 return 뒤 도달 불가 코드 ⑤IdMS 만료 정리 호출자 0(무한 누적) + cleanup CLI 의
낡은 경로가 엉뚱한 디렉터리 청소 위험 ⑥`KMS_MASTER_SECRET` 설정 항목 자체가 없어 재기동마다
랜덤 ⑦죽은 코드 다수(config_cache/notify_config_change/HttpClient).

**우선순위**: ①HTTP 리스너 사망(listener_unavailable) ②CSP/PSP notify 미도달(COM-001 —
무성 장애, open 훅 기성) ③service_log 쓰기 실패(PRC-002) ④DB 쿼리 지속 실패(PRC-002)
⑤IdMS 스토어 쓰기 실패 ⑥인증 시크릿 미설정(config_invalid) ⑦가입자/그룹 적재 실패
(+더미 제거 선행) ⑧config_change 배선.

### 5.5 AGENT

agent 는 L1(프로세스 생존)·호스트 자원·드리프트·HA 판정의 감지 주체다. 원시 관측은 agent,
임계 평가는 OAM base 가 수행해도 detected_by 는 `agent`. **구현 4알람 + 1이벤트**(CSV)의
공통 경로는 metric(2s) → `_CORE_ALERT_RULES` 평가이며, **신규 metric 필드는
`agent_api.py:834-845` 화이트리스트에 추가해야 저장된다**(미추가 시 조용히 폐기).

**구조적 발견**: agent 가 이미 감지하는 전이·실패의 대부분이 **print(journald)로만 소멸**
한다 — watchdog 재기동, HA 역할 전이, 절체 래치, 배포 실패/자동 롤백, 공유 store 부적격.
후보들의 공통 선행 작업은 ①metric 필드+rule 추가(수집·저장이 이미 완비된 cpu/mem/mounts 는
**rule 만 추가**) ②`module_events` 를 `node_events[]` 로 일반화(이벤트 후보 전량의 통로).

**자기보고 대상이 아닌 것**: agent 자신의 heartbeat/metric POST 실패(자기보고 경로 자기장애
— OAM 의 agent_unreachable 이 정본, §5.6), NTP/시각 동기(검사 코드 자체가 없음 — 선행 필요).

**동반 발견 결함/주의**: ①install root 오해석 시 false module_down·config_drift 미평가를
유발하는데 자기검증 없음(`_resolve_prefix` 조용한 폴백) ②sudo 미등록 시 rc=0 graceful skip —
조작이 무시됐는데 성공 보고 ③pending report 큐 무한 append(상한 없음) ④cims-svc/cims-priv
부재 시 조용한 실패 — 모든 재기동·절체 기동 불가.

### 5.6 OAM (base) / OAM-SVC

oam-svc 는 별도 감지 로직이 없다 — base 코어(`alarm_sweeper`)를 공유하고 **소유**(서비스
계열 sweeper + FM ingest)만 다르다. `--role all` 에서는 base 가 대행(detected_by=`oam`).

**이번 조사의 최대 발견 — "알람 없음"이 "감지 불능"을 뜻하게 되는 침묵 실패 3종**:
1. **agent offline**: 8s 무heartbeat 시 offline 마킹만 하고, 그 노드의 agent 계열 열린
   알람을 "관측 불가"로 **전부 자동 close** 한다(`oam_app.py:1336-1345`) — 노드 사망이
   "전 알람 해소"로 보인다. drift sweeper 의 "관측 대상 공집합이면 판정하지 않는다" 방어가
   agent 경로에는 없다. → `agent_unreachable` 신설 + auto-close 의 판정 불가 의미화.
2. **리스 상실/획득 실패**: read-only 강등 + 전 sweeper 정지가 로그로만 남는다 — 상실 중엔
   다른 알람 판정도 전부 멈춘다. → `store_ownership_lost` 신설.
3. **FM ingest 불능/카탈로그 무력화**: ingest 기동 실패 시 그냥 진행, 수신 스레드는 소켓
   오류에 조용히 종료, 빈 카탈로그·UNKNOWN_CODE 거절은 카운터/로그 0 — L2 자기보고 전
   계층이 침묵한다.

**이벤트 스트림 공백**: 현재 event_log 에 실리는 것은 `service_control`·`process_died`·
FM_EVENT 중계뿐. HA 절체 사건, 배포/프로비저닝 job 결과, 인증 감사(로그인 실패·역할 거부·
join 토큰 오사용)가 전부 앱 로그/레코드에만 있다 — "OAM 이 한 일"이 이벤트 스트림에 거의
없다. 배포 job 실패 이력은 retention(2일)으로 소멸까지 한다.

**동반 발견 결함**: ①`transition` 이 state 를 emit 전에 갱신 — 쓰기 실패 시 open 레코드
유실·재기동 복원 깨짐(`alarm_sweeper.py:137-143`) ②`service_unresponsive` 가 "무응답"과
"대상 미설정"을 구분 안 함 — CspNotify 미설정 시 기본 127.0.0.1 probe 로 영구 오탐
③CMP endpoint 공집합 시 관측 통째 비활성 + 열린 알람 stale reap(설정 소실이 알람 정리로
위장) ④agent 계열 `restore_open_state` 실패 시 drift 의 `_reseed_if_empty` 등가 자가복구
없음 ⑤offline 자동 close 가 `msg_close`(정상화 문구)를 그대로 사용 — 오해 유발.

## 관련

- [alarm_module_catalog.csv](alarm_module_catalog.csv) — **목록 정본**
- [alarm_standardization.md](alarm_standardization.md) — 알람 모델·감지 3계층·code 체계 정본
- [alarm_self_reporting.md](alarm_self_reporting.md) — 자기보고(FM push) 경로·wire 규격 정본
