# 13. Flow 로깅 및 서비스 상관관계 (sesid) 설계

본 문서는 CIMS 3모듈(CSP/CMP/CSC)의 **Flow 로그 포맷, Realm 설정, sesid 발행/계승 규칙, 서비스별 분류**에 대한 단일 레퍼런스다.

**원칙**: CSC 의 flow_logger 는 **세션 식별 (sesid) 기준 raw-data 액세스 계층**.
시간 범위 + group_id substring / cross-service method 블랙리스트 같은 표시 레벨
처리는 console 책임. CSC 는 HEARTBEAT 만 무조건 제외하고 나머지는 sesid_set
매칭 일치만으로 필터한다 (sesid 누락 로그 회귀를 위한 legacy substring fallback 유지).

## 1. 목적

한 건의 호(VoLTE/MCPTT)가 발생하면 CSP의 SIP 메시지, CSP↔CMP JSON 제어 메시지, CSC↔CSP notify, CMP 내부 이벤트(Floor/DTMF/RTCP), 녹취 메타가 모두 **동일한 `sesid`** 로 묶여 Console UI에서 하나의 Flow로 조회되도록 한다.

## 2. 로그 파일 배치

```
{ServiceLogDir}/{YYYY}/{MM}/{DD}/{HH}/
    # 전 노드 공통: open-per-write + 5분 버킷 (mm5 = (분/5)*5 = 00/05/.../55)
    csp_01.flow.{mm5}.jsonl         ← CSP flow 이벤트 (SIP/JSON/CSC, compact, body 없음)
    csp_01_sip.msg.{mm5}.jsonl      ← CSP-UE SIP 원문
    csp_01_cmp.msg.{mm5}.jsonl      ← CSP→CMP JSON 원문
    csp_01_csc.msg.{mm5}.jsonl      ← CSP←CSC notify 원문
    cmp_01.flow.{mm5}.jsonl         ← CMP flow (JSON/INT/MCPTT/DTMF/RTCP)
    cmp_01_csp.msg.{mm5}.jsonl      ← CMP↔CSP JSON 원문
    csc_01.flow.{mm5}.jsonl         ← CSC flow (MCPTT/console/system)
    csc_01_csp.msg.{mm5}.jsonl      ← CSC→CSP notify 원문
    csc_01_ue.msg.{mm5}.jsonl       ← UE↔CSC HTTPS(IdMS/GMS/CMS) 원문
```

`{node}.flow.{mm5}.jsonl` — 경량 flow 이벤트 인덱스. `{node}_{iface}.msg.{mm5}.jsonl` — 원문 메시지.
Flow 엔트리의 `seq`+`iface`로 원문을 역조회한다(`seq`=msg 파일 줄번호).

**HEARTBEAT 샘플링**: CSP↔CMP HEARTBEAT(3초 주기)는 양측 모두 msg/flow 에 **100회당
1회**(≈5분당 1건)만 기록한다 — 생존 신호가 로그를 지배(하루 ~5.7만 줄)하는 것을 방지.
요청을 기록한 교환의 응답만 함께 기록해 쌍 정합을 유지한다. 나머지 명령은 전량 기록.

**원문 역조회 = seq 빠른 경로 + sesid 검증/내용 폴백** (`flow_logger.py _lookup_body_by_seq`):
리더는 seq번째 줄을 읽되 **그 줄의 `sesid` 가 flow 엔트리의 `sesid` 와 일치할 때만 신뢰**한다.
불일치하면 같은 버킷 후보 파일들에서 `sesid`(1차키) + `mid`(trans_id, JSON 원문 본문 매칭) +
`dir`(TX/RX — 같은 trans_id 의 요청/응답 구분) + `ts`(CSP 는 flow/msg 에 동일 타임스탬프 문자열
기록 → 정확 일치 우선)로 재검색해 복원한다. `seq` 는 기록 프로세스의 in-memory 카운터라,
system_id 가 겹치는 노드 둘이 공유 스토리지의 같은 파일에 기록하면 줄번호와 어긋난다 —
이때도 sesid 는 노드별 µs 타임스탬프+카운터라 충돌하지 않으므로 내용 매칭이 정답을 찾는다.
콘솔(`flow.ts getBody`)은 seq 조회 시 `sesid`/`mid`/`dir` 를 함께 전달한다.

**nodeId (기록 주체)** — flow 라인의 `node` 필드는 인스턴스 접미사가 없으므로(`csp`),
리더는 flow **파일명**에서 소유자 system_id 를 파생해(`csp_01.flow.* → csp_01`) 각 메시지에
`nodeId` 로 실어준다. 콘솔은 이것으로 모듈 컬럼(`CSP_01`)·노드 필터·TX/RX(`from/to` 와
nodeId 비교 — msg 파일 `dir` 과 동일 관점)를 구분하고, 원문 역조회의 msg 파일 선택에도 쓴다.
같은 CSP↔CMP 메시지는 CSP 기록분(TX)과 CMP 기록분(RX) 두 줄로 존재하는 것이 정상이다.

> **system_id 는 공유 스토리지(NFS) 전역에서 유일해야 한다.** 두 노드가 같은 system_id 로 같은
> 경로에 기록하면 flow/msg 파일이 인터리브되어 seq 정합이 깨지고(위 폴백으로 원문 조회는 복원되지만),
> security 로그·통계·버킷 seq 재계수도 오염된다. 노드 추가 시 `csp_02` 처럼 id 를 분리한다.

**CSP 5분 버킷·open-per-batch** (`SipMessageLogger`): 파일명에 5분 접미사, 파일 핸들 비유지.
이로써 `.nfs` 고아·운영중 로그삭제 데이터유실·대용량 검색 부담을 피한다.
`seq` 는 5분 버킷별로 리셋되므로 **원문 역조회 시 flow 엔트리 `ts`(HH:MM:SS)로 버킷(mm5)을 도출**해 해당 파일을 연다.
리더(`flow_logger.py`)는 `.msg.jsonl` + `.msg.{mm5}.jsonl` glob 을 모두 매칭한다.

**CMP/CSC 도 동일하게 5분 버킷**(`cmp_0N`/`csc_01`/`oam_01` SystemId 분리). **CSP·CMP·CSC 모두
비동기 배치 writer** 사용: 생산자(로깅 호출부)는 한 줄을 포맷·seq 부여 후 큐에 적재만 하고 즉시 반환(파일 I/O 없음),
writer 가 flush 주기(100ms)·큐 임계마다 큐를 비워 **파일경로별로 라인을 합쳐 경로당 1회 open→append→close**
(open-per-batch). writer FIFO 라 파일 줄순서=enqueue(=seq) 순서가 유지되어 `seq↔원문 줄번호` 정합 보존.
목적: NFS 동기 I/O 가 **단일 수신/디스패치 스레드**(csp CmpClient RecvLoop, cmp control loop)를 막던 HOL 블로킹 제거
(상세: [csp_control_plane_load_hardening.md](../csp_control_plane_load_hardening.md)). 구현: csp `CSipMessageLogger`,
cmp/cmdp 공용 `CServiceLogWriter`(include/ServiceLogWriter.h), csc `logger.py`.

**저장 경로 무의존(스풀 폴백)** — `ServiceLogging.Dir` 가 NAS(NFS hard mount)일 때 NFS 가
행이면 쓰기는 실패 대신 **무기한 블록**된다. **CSP/CMP/CMDP/CSC 전 모듈**이 이를 2단 writer 로
격리한다 (구현: csp `CSipMessageLogger` 내장, cmp/cmdp 공용 `include/ServiceLogWriter.h`
(`CServiceLogWriter`), csc `logger.py` 파이썬 구현 — 계약 동일):

- **생산자(SIP/제어/요청 스레드)**: 파일시스템 무접촉 — 큐 적재만. 버킷 회전도 북키핑만 하고
  디렉터리 생성·기존 줄 계수는 하지 않는다 (디렉터리 생성은 flusher 가 기록 직전에 수행).
- **dispatch 스레드**: 큐를 소비해 목적지 결정. 저장소 건강 + 스풀 잔량 없음이면 flusher 큐로
  직행, 아니면 **로컬 스풀**(`ServiceLogging.SpoolDir`, 기본 `spool`)의 미러 파일
  (`{spool}/abs{목적경로}`)에 append. dispatch 도 NFS 를 만지지 않아 항상 살아 있다.
- **NAS flusher 스레드**: 저장 경로 I/O 전담 — 갇혀도 이 스레드 하나뿐. 무응답 판정은
  ① 쓰기 실패(fail-fast) ② in-flight 정체 `ServiceLogging.StallSec`(기본 5s) 초과
  ③ flusher 큐 포화. 회복되면 스풀을 **오래된 파일부터 순서대로 재생(replay)** 한 뒤 직행
  복귀 — 경로별 줄 순서(=seq 정합)가 보존된다. 재생 중 crash 는 재생분 중복(at-least-once)
  가능 — 리더의 sesid/내용 매칭 폴백이 흡수.
- **seq 시딩**: 재기동 시 기동 버킷의 기존 줄 계수(저장 파일 + 스풀 잔량)는 flusher 가
  비동기로 수행해 합류한다. 합류 전에 첫 write 가 오면 0 부터 시작 (리더 폴백 흡수).
- **정지**: 저장소가 건강하면 flusher 드레인을 기다리고, 죽어 있으면 잔량을 스풀로 회수 후
  flusher 를 detach 한다 (NFS killable 대기라 프로세스 종료가 회수). 다음 기동이 재생한다.
- **자기보고**: 폴백 진입 시 알람 `A-PRC-006 storage_failure`(mo=`<시스템ID>/<모듈>/service_log`)
  open, 스풀 드레인 완료 시 close — 모듈별 감지 행은 [alarm_catalog.csv](../alarm_catalog.csv).
  스풀 용량 상한 `ServiceLogging.SpoolMaxMb`(기본 1024) 초과 시 오래된 스풀 파일부터
  폐기(폐기 줄 수는 알람 params `dropped` 로 노출).
- **설정**: 모듈별 `ServiceLogging.{SpoolDir,StallSec,SpoolMaxMb}` (기본 `spool`/5/1024 —
  SpoolDir 는 반드시 로컬 디스크 경로). CSP 는 `Setup.ServiceLogging.*`.

> 남은 별도 축: **CallDir**(call.json/session.json — SIP 스레드 동기 쓰기, 원자 rewrite 라
> append 스풀 불가 — 별도 설계 필요)과 **CMP 녹취**(`PSyncRtpRecorder` — RTP 리액터 스레드가
> `RecordDir`(NAS 가능)에 직접 fwrite. NFS 행 시 미디어 평면 리액터가 갇힐 수 있어 최우선
> 후속 설계 대상).

## 3. Realm 설정

서비스-도메인 매핑을 `csp.json`의 **`Realm` 배열**에 선언한다. 각 서비스에 다중 도메인 허용.

```json
"Sip": {
  "LocalIp": "...",
  "UdpPort": 5060,
  "AuthRealm": "ims.mnc001.mcc001.3gppnetwork.org",
  ...
},
"Realm": [
  { "service": "volte", "domains": ["ims.mnc001.mcc001.3gppnetwork.org"] },
  { "service": "mcptt", "domains": ["ptt.mnc001.mcc001.3gppnetwork.org"] }
]
```

### 규칙
- **`service`** 는 예약 키워드: `volte` | `mcptt` | `system` | `console` (향후 `mcvideo`, `mcdata`)
- **한 service 에 다중 domains**: 배열
- **도메인 중복 불허**: 서로 다른 service 에 같은 도메인이 나오면 config 로드 시 ERROR
- **상용 분리 배포**: CSP/CMP 인스턴스는 보통 하나의 service entry만 가짐
- **`AuthRealm`** (SIP Digest 401의 `realm` 파라미터): IMS home domain 단일값. 미지정 시 `Realm[0].domains[0]` fallback

### 도메인 → service 매칭 (CSP SIP 분류)
`CSipMessageLogger::ClassifyService`가 SIP 메시지에서 도메인 추출 후 lookup:
- **RX**: Request-URI → To 순서로 첫 매치
- **TX**: From 도메인
- **응답(SIP/2.0 ...)**: Call-ID 캐시(`m_mapCallService`)에서 요청 시점 service 계승
- 미매칭 → `service=""` (log에 key 생략)
- 분류는 도메인 기반만 사용한다 (MCPTT Accept-Contact 키워드 fallback 없음 — 비준수 UE 미지원).

### per-call 도메인 override (MCPTT)
PTT 그룹 INVITE는 전역 도메인(보통 volte) 대신 mcptt 도메인을 써야 하므로 psip `CSipDialog::m_strOverrideDomain` 필드 + `CreateCall(..., pszOverrideDomain)` 로 주입. `GroupCallService::InviteMember` 에서 `GetDomainForService("mcptt")` 를 전달.

## 4. sesid 발행 규칙

### 포맷
```
{caller_msisdn}::{module}::{yyyymmddHHMMSSuuuuuu}::{counter}
```
- `caller_msisdn`: E.164 발신자 번호 (PTT 그룹은 `group_id`, 없으면 빈 문자열로 `::module::ts::n`)
- `module`: `csp` | `cmp` | `csc`
- `yyyymmddHHMMSSuuuuuu`: us 정밀도 발행 시각
- `counter`: 동일 us_ts 내 증가 카운터 (락 하에서 atomic)

예시:
- VoLTE: `+821357007001::csp::20260417132634346789::1`
- PTT 그룹: `+82571910001::csp::20260417211416784874::1`
- 시스템: `::csp::20260417185601123456::1` (HEARTBEAT 등)

### 발행/계승
- **발행 주체**: 해당 트랜잭션/세션의 진입 모듈. 이후 모든 연관 메시지가 계승
- **SIP**: `CSipMessageLogger::GetOrIssueSesId(callId, caller)` — Call-ID별 캐시
- **B2BUA leg B**: leg A의 sesid를 새 Call-ID에도 등록 (`SetCallSesId`)
- **CSP → CMP**: JSON payload의 `"sesid"` 필드. CMP의 `_sesidMap[sessionId]` 에 저장 후 내부 이벤트(SESSION_START, GROUP_TIMEOUT 등)에서 계승
- **CMP 응답**: 같은 sesid를 response payload에도 포함
- **CSC → CSP notify**: JSON payload의 `"sesid"` 필드
- **PTT 그룹 단위 통일 sesid**: `GroupCallService::GetOrIssueGroupSesId(group_id)` — 한 그룹 세션이 존재하는 동안 그룹 명령(PTT_GROUP_ADD/PTT_JOIN/PTT_LEAVE/PTT_GROUP_REMOVE) + 모든 그룹 INVITE가 같은 sesid 사용. caller 자리에 group_id 포함시켜 Flow 검색에서 `group_id in sesid` 로 매칭 가능

## 5. Flow 엔트리 포맷

### 필드 순서 (빈 값은 key 생략)
```
ts, service, caller, callee, sesid, subid, node, from, to, proto, method, detail, mid, seq, iface
```

| 필드 | 설명 |
|---|---|
| `ts` | `HH:MM:SS.uuuuuu` |
| `service` | `volte` / `mcptt` / `system` / `console` / `""` |
| `caller` | 발신 MSISDN (SIP From URI, JSON payload.caller) |
| `callee` | 착신 MSISDN |
| `sesid` | § 4 참고 |
| `subid` | 하위 식별 (VoLTE=Call-ID, PTT=session_seq) |
| `node` | `csp` / `cmp` / `csc` |
| `from` / `to` | `ue`, `csp`, `cmp`, `csc`, `ue_o` (caller leg), `ue_t` (callee leg) |
| `proto` | `SIP`, `JSON`, `CSC`, `INT`, `MCPTT`, `DTMF`, `RTCP` |
| `method` | INVITE, 200, RELAY_ADD, FLOOR_GRANT, DTMF 등 |
| `detail` | 사람 읽기용 요약. MCPTT/DTMF/RTCP는 **JSON 문자열** 사용 |
| `mid` | trans_id (CMP JSON), CSeq (SIP) — cross-node 상관 |
| `seq` | 해당 iface의 msg.jsonl 라인 번호 (원문 역조회 키) |
| `iface` | `sip`, `cmp`, `csc`, `ue` |

### Msg 엔트리 포맷 (원문 파일)
```
ts, dir, peer, caller, callee, proto, msg
```

## 6. 서비스 분류 정책

| service | 용도 | 비고 |
|---|---|---|
| `volte` | VoLTE 호 (SIP + CSP↔CMP SESSION 계열) | SIP 도메인이 volte 항목과 매치 |
| `mcptt` | PTT 그룹호 (SIP + CSP↔CMP PTT_GROUP 계열 + CSC IdMS/GMS/CMS) | 도메인 mcptt 매치 또는 MCPTT 엔드포인트 경로 |
| `system` | HEARTBEAT, CSC_RESTART, STATS, SESSION_LIST(audit 재조정) 등 세션 개념 없는 이벤트 | |
| `console` | CSC admin API 트리거 이벤트 (/api/v1/* HTTP, USER_CHANGED/GROUP_CHANGED notify) | pi_http post_hook 로 자동 분류 |
| `""` | 미매칭 (Realm에 등록 안 된 도메인) | key 자체 생략 |

## 7. Inter-module 프로토콜

### 7.1 CSP → CMP 상관 메타 필드 (envelope v2 — 정본: [../../api/cmp_media_api.md](../../api/cmp_media_api.md))
```json
{
  "hdr": {
    "ver": 2, "trans_id": 1024, "node": "csp_01",
    "cmd": "RELAY_ADD", "type": "request",
    "sesid": "+82...::csp::ts::1", "service": "volte"
  },
  "payload": { "session_id": "csp_20260720143012345_1", "caller": "+82...", "callee": "+82...", "...": "..." }
}
```
CMP는 `hdr.service` 로 `_serviceMap[key]` 채움, `hdr.sesid` 로 `_sesidMap[key]` 채움. 응답 hdr 에도 같은 값 계승.

### 7.2 CSC → CSP notify (UDP JSON)
```json
{
  "trans_id": "42",
  "event": "USER_CHANGED",
  "uri": "tel:+82...",
  "action": "PUT",
  "etag": "...",
  "sesid": "+82...::csc::ts::1",
  "service": "console"
}
```
이벤트 타입 → service 자동 매핑:
- `CSC_RESTART` / `HEARTBEAT` / `STATS_*` → `system`
- `USER_CHANGED` / `GROUP_CHANGED` → `console` (admin API 트리거)
- 그 외 → `mcptt`

## 8. CMP 내부 이벤트

### 8.1 SESSION/GROUP 내부 상태
- `SESSION_START` / `SESSION_END` / `SESSION_TIMEOUT` — VoLTE
- `GROUP_START` / `GROUP_END` / `GROUP_TIMEOUT` — MCPTT
- proto=`INT`, `_sesidMap[key]`에서 sesid/service 계승

### 8.2 Floor Control (RTCP APP, MCPT)
- proto=`MCPTT`, method=`FLOOR_REQUEST` | `FLOOR_GRANT` | `FLOOR_DENY` | `FLOOR_RELEASE` |
  `FLOOR_IDLE` | `FLOOR_TAKEN` | `FLOOR_REVOKE` | `FLOOR_RELEASE_MULTI` | `FLOOR_END_OF_RTP` |
  `FLOOR_QUEUE_POS_INFO` | `FLOOR_MEDIA_FLOW`
- detail JSON: `{"op":"GRANT","user":"+82...","ssrc":N,"prio":P}`
- RX: `PMcpttGroup::onFloorPacket` — UE→CMP 방향 REQUEST/RELEASE 기록(QUEUE_POS/ACK 은 노이즈라 제외)
- TX: `PMcpttGroup::broadcastFloorStatus` / `handleFloorRequest` / `tickFloorTimers` — CMP→UE 방향
- cmp.json `ServiceLogging.Flow.Floor=true` 로 제어

### 8.3 DTMF (RFC 2833/4733)
- PT=101 telephone-event. END bit 시점에만 기록 (중복 방지)
- proto=`DTMF`, method=`DTMF`
- detail JSON: `{"digit":"1","duration_ms":160,"volume":10,"user":"+82..."}`
- cmp.json `ServiceLogging.Flow.Dtmf=true`

### 8.4 RTCP SR/RR/SDES/BYE (옵션)
- proto=`RTCP`, method=`SR` | `RR` | `SDES` | `BYE`
- detail JSON: `{"type":200,"pt":"SR","ssrc":N,"len":L,"user":"..."}`
- 트래픽 많아 기본 OFF. `ServiceLogging.Flow.Rtcp=true` 시 활성화

## 9. CSC 로깅 자동화 (pi_http middleware)

`util/pi_http/http_server_controller.py::DynamicRouteProc` 에 `set_request_hooks(pre, post)` API. `app.py` 에서 `_post_hook` 등록하여:
- base_path prefix → service 자동 매핑:
  - `/idms/*`, `/org.openmobilealliance*`, `/org.3gpp.mcptt*`, `/keymanagement/*` → `mcptt`
  - `/api/v1/*` → `console`
- JWT Bearer 토큰에서 `login_id` 추출 → `caller`
- method 에 IdMS/GMS/CMS/KMS prefix 자동 부여 (예: `IdMS/GET /idms/authreq`)
- detail 에 `status=<code>` 기록

## 10. Flow 조회 API

### 10.1 VoLTE
```
GET /api/v1/flow/{call_id}?date=YYYY-MM-DD&call_type=volte
```
반환: `{call_id, date, nodes: {csp: [msgs...], cmp: [msgs...]}}`

내부 동작:
1. `call_id` + `call_type` → `_find_d_dir_by_callid(volte)` 한 type 안에서만 검색 → `.d` 디렉터리 조회 → `session.json`에서 `call_ids` (B2BUA 2 legs) 추출
2. SIP msg.jsonl (raw SIP 메시지) 에서 `call_ids` 매칭 라인의 sesid 추출 — `_extract_sesids_from_msg_jsonl`. flow.jsonl 의 SIP 라인은 Call-ID 필드를 가지지 않으므로 (caller/callee/sesid/method/seq/iface 만) flow.jsonl 단독 매칭으로는 0건이 나옴.
3. SIP/CMP msg 검색을 **sesid 매칭 우선** + legacy substring fallback (sesid 누락 로그 회귀용). HEARTBEAT 만 무조건 제외.
4. console 은 항상 `call_type` 을 명시한다 (`flowApi.get(callId, date, callType)`) — 한 type 안에서만 검색해 VoLTE call_id 와 PTT group_id 의 prefix 충돌을 피한다.

### 10.2 PTT
```
GET /api/v1/ptt/history/{group_id}/{session}/flow?date=YYYY-MM-DD
```
반환: `{call_id: group_id, date, nodes}`

내부 동작:
1. session 시간 범위 내 group_id 매칭 메시지에서 sesid 모음
2. **sesid 매칭으로 전체 메시지 필터** — startup-time `PTT_GROUP_ADD`, 종료 후 `PTT_GROUP_REMOVE`, member join/leave 등 라이프사이클이 시간 범위와 무관하게 자연 포함됨
3. sesid 매칭 0 건 시 fallback: legacy substring 매칭 (`detail` | `sesid` | `subid` 중 하나에 `group_id` 포함)
4. **HEARTBEAT 만 제외**. cross-service method 필터 등 UX 결정은 console 책임.

> CSP `_endSessionLocked` 는 `m_mapPttSession.erase` 를 수행하지 않는다 — 첫 멤버 OnCallTerminated 시점에 session map 이 비면 이후 멤버의 join/leave 가 events.jsonl 에 기록되지 않으므로, 그룹의 모든 멤버가 member_join + member_leave 를 정상 기록하도록 세션 엔트리를 유지한다.

> 세션 **개시자(발신 단말)** 는 CSP 가 초대한 멤버의 응답(`OnCallStarted`) 경로를 타지 않으므로,
> `PttSessionStart` 가 개시자를 `member_join`(`role:"initiator"`) 으로 events.jsonl 에 직접 기록한다.
> OAM `_derive_session_meta_from_events` 는 이 명시 role 을 "첫 join 추정"보다 우선해 세션
> initiator 로 삼고, 콘솔 타임라인은 해당 입장 행에 "개시자" 배지를 표시한다.

### 10.3 body 조회
```
GET /api/v1/flow/body?date=&hour=&seq=&iface=&node=
```
- `node` 파라미터 중요: 여러 노드가 같은 iface에 msg 파일을 쓸 때(`*_csp.msg.jsonl` 는 cmp/csc 둘 다 생성) 정확히 매칭

## 11. Console UI 표시

`FlowPage.tsx` 의 proto 색상 매핑:
- `SIP` 파랑 / `JSON` 주황 / `CSC` 초록 / `WS` 연두
- `RTP` 핑크 / `RTCP` 보라
- `MCPTT` 황갈 / `DTMF` 빨강 / `INT` 회색

## 12. 관련 파일

### CSP
- `csp/SipServerSetup.h/.cpp` — Realm 배열 파싱, `m_mapDomainToService`, `GetDomainForService`
- `csp/SipMessageLogger.h/.cpp` — `IssueSesId`, `GetOrIssueSesId`, `ClassifyService`, `WriteFlowLine`
- `csp/CmpClient.h/.cpp` — payload sesid/service/caller/callee 필드, Transaction
- `csp/GroupCallService.h/.cpp` — `m_mapGroupSesId`, `GetOrIssueGroupSesId`
- `csp/CscInterface.cpp` — CSC notify payload의 sesid/service 파싱
- `csp/CallDir.h` — `WriteSessionMapping(strSesId)` → session.json 의 `sesid`
- `ext/psip/SipUserAgent/SipDialog.h/.cpp` — `m_strOverrideDomain`
- `ext/psip/SipUserAgent/SipUserAgentCall.hpp` — `CreateCall(..., pszOverrideDomain)`
- `ext/psip/SipUserAgent/SipUserAgentUtil.hpp` — `SetCallDomain`

### CMP
- `cmp/PCmpServer.h/.cpp` — `_sesidMap`, `_serviceMap`, `_logFlowFloor/Dtmf/Rtcp`, `logFlow`, `writeMsgLine`
- `cmp/PMcpttGroup.h/.cpp` — Floor control logFlow (onRtcpPacket + broadcastFloorStatus), DTMF `_dtmfFlowLog`, RTCP SR/RR/SDES/BYE 옵션 로깅

### CSC / OAM-svc
- `ems/core/oam/src/services/flow_logger.py` — `issue_sesid`, `get_or_issue_sesid`, `log_flow`, `log_console` + sesid 매칭 검색 (oam-svc 모듈이 서빙)
- `csc/src/services/mcptt.py` — `notify_csp(service, sesid)` + `_notify_targets` (CSP/PSP 분기)
- `csc/src/httpsrv/controller.py` — `DynamicRouteProc.set_request_hooks` (pre/post hook)
- `csc/src/csc_app.py` — `_post_hook` 등록 + base_path → service 매핑

### UI
- `ems/core/console/src/api/flow.ts` — `flowApi.getBody(date, hour, seq, ts, dir, proto, iface, node)`
- `ems/core/console/src/pages/FlowPage.tsx` — proto 색상, nodes 구조 처리
