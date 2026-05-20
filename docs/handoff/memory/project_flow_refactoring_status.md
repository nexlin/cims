---
name: Flow 리팩토링 진행 상태
description: ServiceLogging 통합 + Flow 키 통일 작업 완료 상태 (모든 초기 이슈 해결됨)
type: project
originSessionId: 497b2867-5a2f-4a04-86bf-33c0b0a12e0e
---
## 완료된 작업 (누적)
- CMP 코드 리팩토링 (PRtpSocket/PRtpRelay/PRtpMulticast/PSyncRtpRecorder 분리)
- CMP 파일명 P 접두사 통일 (PCmpServer, PMcpttGroup, PLog 등)
- Flow 키 통일 (ts, node, service, from, to, proto, method, detail, mid, sesid, subid, seq, iface)
- ServiceLogging 설정 구조 (CMP/CSP/CSC config template)
- 디렉토리 통합 (Flow + Msg 로그를 하나의 Dir에)
- 파일명 규칙 ({node}.flow.jsonl, {node}_{iface}.msg.jsonl)
- PTT 커맨드 분리 (ADD_PTT_GROUP, JOIN_PTT_GROUP 등)
- PTT subid(session_seq) DB + CSP 구현
- CMP 원문 기록 (cmp_01_csp.msg.jsonl)

## 2026-04-17 세션 — 미해결 이슈 5건 모두 점검/해결
1. **VoLTE 이력 페이지 (해결)**: `/api/v1/volte/history` 404 메모는 구버전. 현재 프런트는 `/api/v1/call/logs?call_type=voip`를 사용하며 정상 응답.
2. **PTT 이력 페이지 세션 메타 (해결)**: `session.json` 부재 시 `events.jsonl`에서 start/end/state/initiator/member_count를 유도하도록 `csc_flow.py::_derive_session_meta_from_events` 추가. 세션 상세에서는 `participants.jsonl`도 events로 fallback.
3. **VoLTE Flow nodes (해결)**: flow.jsonl의 SIP Call-ID가 `subid` 필드에 저장되는데 `_search_sip_messages` / `_flow_msg_from_log`는 `call_id`를 보고 있어 매칭 실패 → 모든 CMP 메시지가 섞여 들어왔음. `subid` 우선, `call_id` fallback으로 수정.
4. **CSC flow/msg 파일 생성 (해결)**: `csc_logger.py` 재작성. 신규 통합 포맷(`csc_01.flow.jsonl` + `csc_01_{iface}.msg.jsonl`)으로 Flow/Msg 이중 기록. 레거시 `log_msg`/`log_ptt_service` 호출은 새 `log_flow`로 redirect.
5. **Recording 경로 통합 (확인)**: `ServiceLogging.Recording: true` 파싱 정상 (SimpleJson이 boolean을 "true" 문자열로 취급). `m_strRecordDir`이 `ServiceLogDir`로 통합되며, VoIP/PTT 모두 실제로 `{ServiceLogDir}/voip|ptt/...` 하위에 seg_*.rtp가 저장됨을 확인.

## 2026-04-17 세션 — VoLTE Flow 보완 (추가 3건 해결)
6. **CSP↔CMP 메시지 caller/callee 규격화**: VoLTE SESSION 계열(ADD/MODIFY/REMOVE) 메시지에 `caller`/`callee` 필드를 항상 포함하도록 CSP `CmpClient::{ModifySession,UpdateSession,RemoveSession}` 시그니처에 `strCaller`/`strCallee` 파라미터 추가. `CRtpInfo`에 `m_strCaller`/`m_strCallee` 저장하여 SetIpPort/Delete 경로에서도 전파.
7. **CMP Flow '(body 없음)' 해결**: 두 가지 버그 발견/수정.
   - `PCmpServer::ensureFlowHourDir`의 `_msgFile`이 `"a"` 모드로 열려 `fgets`로 라인 수 카운트가 동작 안 함 → 재시작 시 `_msgSeq` 항상 0에서 시작 → flow의 `seq`가 실제 파일 라인과 불일치 → `_lookup_body_by_seq`가 엉뚱한 라인 반환. `"a+"` 모드로 변경.
   - `writeMsgLine`이 `_msgFile == NULL`일 때 조용히 0 리턴하여 초기 HEARTBEAT 등이 seq 카운터 누락. `writeMsgLine` 진입 시 `ensureFlowHourDir()` 호출하여 lazy init.
   - 또한 CMP `logFlow` 호출(processAdd/processRemove/processModify)에 `service="volte"`, `sesid=session_id`, `seq=_lastRxSeq/txSeq`, `iface="csp"` 파라미터 모두 전달하도록 수정 → CMP flow 엔트리에서도 body 조회 가능.
8. **detail 포맷 CSP↔CMP 일치**: 기존에 CMP는 detail을 전부 `session_id`로 기록해서 발/착신 정보 없었음. 수정:
   - `ADD_SESSION`: `caller→callee` (양쪽 동일)
   - `MODIFY_SESSION`: `peer_index==0 → caller`, `peer_index==1 → callee` (수정 대상 쪽만)
   - `REMOVE_SESSION`: `caller→callee`
   - CSP 쪽(`CmpClient::SendRequestAndWait`)과 CMP 쪽(`PCmpServer::processAdd/Remove`) 모두 동일 규칙 적용하여 양 노드 detail이 일치.

## 2026-04-17 세션 — 통합 sesid 포맷 도입
**포맷**: `{caller}::{module}::{yyyymmddHHMMSSuuuuuu}::{counter}` (caller 없으면 leading `::`)

**범위**: 로그/디버깅 상관관계 전용 ID. SIP 외부 프로토콜은 무변경. CSC/CSP/CMP 모든 내부 메시지와 flow/msg 로그에 필수 포함.

**발행/계승 규칙**:
- 각 모듈의 "신규 트랜잭션 진입점"이 발행
- SIP Call-ID로 매핑 유지 → REGISTER/INVITE/OPTIONS/SUBSCRIBE/BYE 등 모든 SIP 메시지 같은 Call-ID면 같은 sesid
- B2BUA leg A → leg B sesid 계승 (`SetCallSesId(legB_CallId, legA_sesid)`)
- CSP→CMP JSON payload에 `sesid` 필드 포함, CMP가 받아 `_sesidMap`에 저장, 응답에도 포함
- CMP 내부 이벤트(SESSION_START/END, GROUP_START/END, SESSION_TIMEOUT 등)는 `_sesidMap[key]` 참조
- CSC→CSP notify_csp payload에 `sesid` 필드 포함, CSP `CscInterface`가 파싱하여 logFlow에 전달
- HEARTBEAT/STATS_REQUEST도 매 트랜잭션마다 sesid 발행 (디버깅 일관성)

**주요 파일 변경**:
- `csp/SipMessageLogger.h/.cpp` — `IssueSesId/GetOrIssueSesId/GetSesIdByCallId` 추가, SIP Print()에서 매 메시지 sesid 발행/조회
- `csp/CmpClient.h/.cpp` — Add/Modify/Update/RemoveSession·Group에 `strSesId` 파라미터, `m_mapKeyToSesid` 캐시, `Alive()` 자체 발행
- `csp/RtpMap.h/.cpp` — `CRtpInfo::m_strSesId`, `CreatePort(strSesId)` 파라미터
- `csp/ModuleDispatcher.cpp` — INVITE에서 `GetOrIssueSesId`, leg B에 계승, `CallDir::WriteSessionMapping(strSesId)` 전달
- `csp/CscInterface.cpp` — payload.sesid 파싱, 없으면 자체 발행, STATS_RESPONSE에 계승
- `csp/CallDir.h` — `WriteSessionMapping(strSesId)` → `session.json`에 `"sesid"` 필드 기록
- `cmp/PCmpServer.h/.cpp` — `_sesidMap[key]`, `issueSesid/getOrIssueSesid`, 모든 process* 함수(Add/Remove/Modify/AddGroup/Join/Leave/RemoveGroup/Alive/Stats)에서 payload.sesid 수신/자체 발행/응답 계승, 내부 타임아웃 이벤트에서 캐시 참조
- `csc/bin/csc_pihttp/src/csc_logger.py` — `issue_sesid/get_or_issue_sesid/clear_sesid`, `log_flow`에 sesid 필수화, `log_msg/log_ptt_service`에 caller 파라미터
- `csc/bin/csc_pihttp/src/csc_service.py` — `notify_csp(sesid, caller)` 확장, URI에서 caller 자동 추출, 자체 flow 기록

**검증 결과**:
- VoLTE 한 통화의 SIP(REGISTER/INVITE/200/ACK/BYE) + JSON(ADD/MODIFY/REMOVE_SESSION) 모두 동일 sesid `+82...::csp::{ts}::1`
- HEARTBEAT는 매 3초마다 신규 sesid, 요청/응답 쌍은 같은 값
- CSC_RESTART notify → CSC 발행 `::csc::{ts}::1` → CSP가 같은 sesid로 flow 기록
- session.json에 `sesid` 필드 정상 기록

## 2026-04-17 세션 — Phase A: Realm 재구조화 + Flow/Msg 필드 통일

**목표**: 로그에서 service 값의 판단 주체를 단일화하고(CSP config 기반), 포맷을 표준화

**확정 사항**
- `Realm` 설정: 배열 `[{"service":"volte","domains":[...]}, ...]` — 한 service에 다중 domain 지원
- 도메인 중복(서로 다른 service 에 같은 도메인) → config 로드 시 ERROR
- 예약 service 키워드: `volte` | `mcptt` | `system` | `console`
- `AuthRealm` 분리 (IMS Digest 인증용 단일 realm, 미지정 시 `Realm[0].domains[0]`)
- TX=From 도메인, RX=Request-URI→To 순서로 매칭. 미매칭 → `service=""` (key 생략)
- MCPTT 키워드(Accept-Contact `+g.3gpp.mcptt`) fallback 제거 — 규격 준수 강제
- 빈 값은 모든 key 생략 (Flow/Msg 공통)

**포맷**
- Flow: `ts, service, caller, callee, sesid, subid, node, from, to, proto, method, detail, mid, seq, iface`
- Msg: `ts, dir, peer, caller, callee, proto, msg`

**Inter-module 메시지 service 전달**
- CSP → CMP JSON payload에 `"service"` 필드 필수 (`volte`/`mcptt`/`system`)
- CSP → CMP 응답에도 `"service"` 계승
- CSC → CSP notify_csp payload에 `"service"` 필드
- CMP `_serviceMap[key]` 저장 후 내부 이벤트(SESSION_END, GROUP_TIMEOUT 등)에서 계승
- CSC 자체 flow: `mcptt`(MCPTT 엔드포인트) / `console`(admin API 트리거 event) / `system`(CSC_RESTART/HEARTBEAT)

**주요 파일 변경**
- CSP: `SipServerSetup.h/.cpp` — `Realm` 배열 파싱, `m_mapDomainToService`, `GetDomainForService`/`GetServiceForDomain`, `AuthRealm` 파싱
- CSP: `SipMessageLogger.h/.cpp` — `SetDomainServiceMap`, `ClassifyService` 재작성(도메인 lookup, 락 재획득 금지), `WriteFlowLine`/`WriteInterfaceLine` 필드 순서 통일 + caller/callee
- CSP: `GroupCallService.cpp`, `CscfModule.cpp`, `SipServerRegister.hpp`, `CspServer.cpp` — `m_strPttRealm`/`m_strVoipRealm` 참조 제거, `GetDomainForService(...)` 헬퍼 사용
- CSP: `CmpClient.cpp` — Transaction.service payload 기반 결정, payload 에 자동 주입, TX/RX 로그에 caller/callee 전달
- CSP: `CscInterface.cpp` — payload `service`/`sesid` 파싱, LogMessage 에 전달, uri → caller 추출
- CMP: `PCmpServer.h/.cpp` — `_serviceMap`, 모든 processXxx 에서 payload.service 수신/저장/응답 계승, logFlow/writeMsgLine 필드 순서 + caller/callee, 내부 timeout 이벤트에서도 계승
- CSC: `csc_logger.py` — `log_flow` 필드 순서(빈값 key 생략), `log_console` 유틸, `log_msg/log_ptt_service` service=mcptt 고정
- CSC: `csc_service.py` — `notify_csp(service=)` 시그니처, event 기반 자동 분류(CSC_RESTART→system / USER_CHANGED/GROUP_CHANGED→console / 그 외→mcptt), uri→caller 자동 추출
- Config: `csp.json.template` — `VoipRealm`/`PttRealm` 제거, `Realm` 배열, `AuthRealm` 분리

**검증 결과**
- VoLTE 호출: caller/callee 모든 관련 메시지에 포함, service=volte 일관. CSP↔CMP 동일 sesid
- HEARTBEAT: service=system, caller/callee 없어 key 생략 확인
- CSC_RESTART: CSC→CSP 양측 모두 service=system, sesid 계승
- OK 응답 등 caller/callee 없는 메시지: key 자동 생략 확인
- 데드락 이슈(1회) — `ClassifyService`가 `Print` 내부 락 하에서 호출되므로 재진입 락 제거

## 2026-04-17 세션 — Phase B.1 + B.3 완료

**B.1 SIP 헤더 High 우선순위**
- `P-Asserted-Identity` 추가: REGISTER 200 OK(CscfModule, SipServerRegister) + B2BUA 발신 INVITE(ModuleDispatcher)
- `P-Preferred-Service` 추가: MCPTT INVITE(GroupCallService) — `urn:urn-7:3gpp-service.ims.icsi.mcptt`
- `P-Asserted-Service`(응답측): psip 라이브러리 `CSipUserAgent::AcceptCall` 훅 필요 → Phase B.2 로 이관

**B.3 CSC 로깅 자동화 (pi_http 레벨 훅)**
- `util/pi_http/http_server_controller.py::DynamicRouteProc`에 `set_request_hooks(pre, post)` 클래스 메서드 추가
- `app.py`에서 `_post_hook` 등록:
  - base_path prefix → service 매핑: `/idms/*, /org.openmobilealliance*, /org.3gpp.mcptt*, /keymanagement/*` → `mcptt`, `/api/v1/*` → `console`
  - JWT Bearer 토큰에서 `login_id` 추출 (`cims_auth.extract_token`), 없으면 query/body에서 `login_id`/`user_name`
  - IdMS/GMS/CMS/KMS sub-function prefix 자동 부여 (예: `IdMS/GET /idms/authreq`)
  - detail 에 `status=<code>` 기록
- `csc_service.py` 의 개별 `log_msg` 호출 제거 (훅에서 자동 처리) — PTT 그룹별 `participants.jsonl` 기록은 유지

**검증 결과**
- VoLTE REGISTER 200 OK + B2BUA INVITE: P-Asserted-Identity 확인
- MCPTT INVITE: P-Preferred-Service 확인
- CSC admin login: `service=console`, caller=`admin` (JSON body에서 추출), detail=`status=401`
- CSC IdMS authreq: `service=mcptt`, caller=`test`, method=`IdMS/GET /idms/authreq`

**남은 작업 (Phase B.2/이후)**
- SIP 헤더 Medium: `Allow`, `Allow-Events`, `Supported` 확장 (INVITE에 timer/replaces 추가)
- SIP 헤더 Low (Carrier/CDR): P-Access-Network-Info, P-Charging-*, Privacy, Path — 운영 배포 전 재검토
- MCPTT `P-Asserted-Service` 응답 주입 — psip 라이브러리 확장 필요
- MCPTT 비준수 UE 대응 — 요구사항 발생 시 재검토

## 배포 메모
- 수정 후 `cp csc/bin/csc_pihttp/src/*.py build/dist/csc/src/` + `./cims.sh restart csc` 필요.
- 현재 dist와 src가 수동 동기화 중 — CMake 빌드 파이프라인에 csc Python 소스 자동 복사 규칙이 없는 듯.

## 핵심 파일 참조
- `csc/bin/csc_pihttp/src/csc_flow.py` — Flow API, _derive_session_meta_from_events, _search_sip_messages(subid 기반)
- `csc/bin/csc_pihttp/src/csc_logger.py` — CSC flow/msg 통합 포맷 로거 (log_flow)
- `csc/bin/csc_pihttp/src/app.py` — CSC 초기화, ServiceLogging 설정 파싱
- `csc/bin/csc_pihttp/src/cims_recording.py` — 녹취 API
- `csp/SipMessageLogger.cpp` / `csp/CallDir.h` — CSP flow/msg/녹취 디렉터리
- `cmp/PCmpServer.cpp` — CMP flow/msg 기록
- `cims-console/src/api/ptt.ts` / `pages/PttHistoryPage.tsx` — PTT 이력 UI (nodes 구조 대응)
- `cims-console/src/pages/FlowPage.tsx` — Flow UI (노드 필터, detail 표시)
