# 13. Flow 로깅 및 서비스 상관관계 (sesid) 설계

본 문서는 CIMS 3모듈(CSP/CMP/CSC)의 **Flow 로그 포맷, Realm 설정, sesid 발행/계승 규칙, 서비스별 분류**에 대한 단일 레퍼런스이다. 2026-04 Phase A/B 리팩토링 결과물.

## 1. 목적

한 건의 호(VoLTE/MCPTT)가 발생하면 CSP의 SIP 메시지, CSP↔CMP JSON 제어 메시지, CSC↔CSP notify, CMP 내부 이벤트(Floor/DTMF/RTCP), 녹취 메타가 모두 **동일한 `sesid`** 로 묶여 Console UI에서 하나의 Flow로 조회되도록 한다.

## 2. 로그 파일 배치

```
{ServiceLogDir}/{YYYY}/{MM}/{DD}/{HH}/
    csp_01.flow.jsonl           ← CSP flow 이벤트 (SIP/JSON/CSC, compact, body 없음)
    csp_01_sip.msg.jsonl        ← CSP-UE SIP 원문
    csp_01_cmp.msg.jsonl        ← CSP→CMP JSON 원문
    csp_01_csc.msg.jsonl        ← CSP←CSC notify 원문
    cmp_01.flow.jsonl           ← CMP flow (JSON/INT/MCPTT/DTMF/RTCP)
    cmp_01_csp.msg.jsonl        ← CMP↔CSP JSON 원문
    csc_01.flow.jsonl           ← CSC flow (MCPTT/console/system)
    csc_01_csp.msg.jsonl        ← CSC→CSP notify 원문
    csc_01_ue.msg.jsonl         ← UE↔CSC HTTPS(IdMS/GMS/CMS) 원문
```

`{node_id}.flow.jsonl` — 경량 flow 이벤트 인덱스. `{node_id}_{iface}.msg.jsonl` — 원문 메시지. Flow 엔트리의 `seq`+`iface`로 원문을 역조회한다.

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
- ⚠ MCPTT 키워드(Accept-Contact) fallback은 **제거**됨. 비준수 UE는 미지원.

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
- **PTT 그룹 단위 통일 sesid**: `GroupCallService::GetOrIssueGroupSesId(group_id)` — 한 그룹 세션이 존재하는 동안 ADD/JOIN/LEAVE/REMOVE_PTT_GROUP + 모든 그룹 INVITE가 같은 sesid 사용. caller 자리에 group_id 포함시켜 Flow 검색에서 `group_id in sesid` 로 매칭 가능

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
| `method` | INVITE, 200, ADD_SESSION, FLOOR_GRANT, DTMF 등 |
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
| `system` | HEARTBEAT, CSC_RESTART, STATS 등 세션 개념 없는 이벤트 | |
| `console` | CSC admin API 트리거 이벤트 (/api/v1/* HTTP, USER_CHANGED/GROUP_CHANGED notify) | pi_http post_hook 로 자동 분류 |
| `""` | 미매칭 (Realm에 등록 안 된 도메인) | key 자체 생략 |

## 7. Inter-module 프로토콜

### 7.1 CSP → CMP JSON payload 공통 필드
```json
{
  "cmd": "ADD_SESSION",
  "service": "volte",
  "sesid": "+82...::csp::ts::1",
  "caller": "+82...",
  "callee": "+82...",
  "session_id": "cmp_sess_N",
  ...
}
```
CMP는 `payload.service` 로 `_serviceMap[key]` 채움, `payload.sesid` 로 `_sesidMap[key]` 채움. 응답(OK/ERROR)에도 같은 값 계승.

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
- proto=`MCPTT`, method=`FLOOR_REQUEST` | `GRANT` | `REJECT` | `RELEASE` | `IDLE` | `TAKEN` | `REVOKE`
- detail JSON: `{"op":"GRANT","user":"+82...","ssrc":N,"prio":P}`
- RX: `PMcpttGroup::onRtcpPacket` — UE→CMP 방향 모든 op-code 기록
- TX: `PMcpttGroup::broadcastFloorStatus` / `handleFloorRequest` — CMP→UE 방향
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
GET /api/v1/flow/{call_id}?date=YYYY-MM-DD
```
반환: `{call_id, date, nodes: {csp: [msgs...], cmp: [msgs...]}}`

내부 동작:
1. `call_id` → `.d` 디렉터리 조회 → `session.json`에서 `call_ids` (B2BUA 2 legs) 추출
2. SIP msg 검색: `_search_sip_messages(call_ids)` — `subid` 필드 매칭
3. 해당 sesid 집합 수집 → CMP msg 검색 시 **sesid 기반 필터** (이전 시간 범위 방식 → 타 호 섞임 방지)

### 10.2 PTT
```
GET /api/v1/ptt/history/{group_id}/{session}/flow?date=YYYY-MM-DD
```
반환: `{call_id: group_id, date, nodes}`

내부 동작:
1. `events.jsonl` 로부터 session 시작/종료 시각 추출 (±버퍼)
2. 해당 시간 범위의 mcptt flow 로드
3. 매칭 조건: `detail` | `sesid` | `subid` 중 하나에 `group_id` 포함 → 그룹 INVITE/ADD/JOIN/LEAVE/REMOVE + Floor control + DTMF/RTCP 모두 포함

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

### CSC
- `csc/bin/csc_pihttp/src/csc_logger.py` — `issue_sesid`, `get_or_issue_sesid`, `log_flow`, `log_console`
- `csc/bin/csc_pihttp/src/csc_service.py` — `notify_csp(service, sesid)` 확장
- `csc/bin/csc_pihttp/src/util/pi_http/http_server_controller.py` — `DynamicRouteProc.set_request_hooks`
- `csc/bin/csc_pihttp/src/app.py` — `_post_hook` 등록 + base_path→service 매핑

### UI
- `cims-console/src/api/flow.ts` — `flowApi.getBody(date, hour, seq, ts, dir, proto, iface, node)`
- `cims-console/src/pages/FlowPage.tsx` — proto 색상, nodes 구조 처리
