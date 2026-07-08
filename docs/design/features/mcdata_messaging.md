# MCData 그룹 메시징 (SDS)

그룹 문자 메시징을 3GPP MCData 규격(TS 24.282 SDS·TS 24.481 그룹문서)에 정합하게 구현한
정본 설계 문서. 대상 규격: **TS 24.282**(SDS 시그널링·§15 메시지 포맷), **TS 24.481**(그룹별
게이트), **TS 23.282**(아키텍처). TS 24.379(MCPTT)에는 사용자 메시징이 없으며, MESSAGE 용례는
긴급경보(alert-ind)뿐이다 — 그 경로는 본 기능과 별개로 유지된다
([mcptt_emergency_modes.md](mcptt_emergency_modes.md)).

## 1. 아키텍처

```
앱(MCData client) ── SIP MESSAGE (multipart/mixed) ──→ CSP MCDATA-AS (participating+controlling 통합)
                                                          │ ① 게이트: allow-SDS·발신자 멤버십·max-data-size
                                                          │ ② affiliation 정책 필터
                                                          └── 멤버별 MESSAGE fan-out (Content-Type 보존)
수신 앱 ──(disposition 요청 시) SDS NOTIFICATION(DELIVERED) ──→ CSP 1:1 경로 ──→ 발신 앱 (✓ 표시)
```

- **CSP `MCDATA-AS` 모듈** (`csp/McDataAsModule.{h,cpp}`) — 그룹 대상 MESSAGE 의 controlling
  function. 역할 플래그 `Roles.MCDATA` (기본 ON, `csp/SipServerSetup.cpp`).
- participating/controlling 통합 배치는 CIMS PTT 와 동일한 배치(deployment) 선택으로 규격 위반이
  아니다. 단말은 그룹 URI(`sip:<gid>@domain`)로 직행 전송한다(표준의 participating PSI 라우팅
  단순화 — §7 편차 참조).
- CMP 는 관여하지 않는다. **대용량 SDS 의 미디어평면(MSRP)은 별도 프로세스 `cmdp` 가 종단한다
  (§4.7)** — C-plane 게이트(`allow_sds`·멤버십)와 보관은 두 평면이 공용이다.

## 2. 그룹별 게이트 — TS 24.481 그룹문서

DB `ptt_groups` 컬럼이 SoT (마이그레이션 `sql/migrate_mcdata_sds.sql`, csp 신버전 배포 **전** 적용):

| 컬럼 | 그룹문서 요소 (mcpttgi NS) | 기본 | 의미 |
|---|---|---|---|
| `allow_sds` | `<mcdata-allow-short-data-service>` | 1 | 그룹 SDS 메시징 허용 |
| `allow_fd` | `<mcdata-allow-file-distribution>` | 0 | 그룹 파일전송(FD) 허용 |
| `max_sds_size` | `<mcdata-on-network-max-data-size-for-SDS>` | 10000 | SDS payload 최대 octets (0=무제한) |
| `max_auto_recv` | `<mcdata-on-network-max-data-size-auto-recv>` | 1048576 | 수신 단말 파일 자동 다운로드 임계 octets |

- 모든 `mcdata-*` 요소는 기존 `urn:3gpp:ns:mcpttGroupInfo:1.0`(mcpttgi) 네임스페이스의 표준
  요소다 (TS 24.481 §7.2.4.2 — 별도 NS 불필요).
- CSC 그룹문서 생성(`csc/src/services/mcptt.py get_group_xml`)이 위 요소 + `supported-services`
  의 MCData 서비스 enabler(`urn:urn-7:3gpp-service.ims.icsi.mcdata.sds` / `.fd`, allow 시에만)를
  방출한다.
- admin API(`/api/v1/ptt/groups`)로 네 필드 CRUD 가능. PUT 시 기존 `GROUP_CHANGED` notify 로
  CSP 가 무중단 재적재(`CDbManager::SelectGroup`).
- **콘솔 그룹 편집 폼**(`ems/service/console/src/pages/PttGroupsWorkbenchPage.tsx`)에서 메시징/
  파일전송 토글 + 메시지 최대/자동수신 최대(byte) 입력으로 편집한다.
- CSP JSON fallback(`csp/Group/*.json`)도 `allow_sds`/`allow_fd`/`max_sds_size` 키를 지원.

## 3. 메시지 포맷 — TS 24.282 §15

SIP MESSAGE 본문 = `multipart/mixed;boundary=…` 3파트:

| 파트 Content-Type | 내용 |
|---|---|
| `application/vnd.3gpp.mcdata-info+xml` | `<mcdatainfo><mcdata-Params><request-type>group-sds</request-type><mcdata-request-uri type="Normal"><mcdataURI>tel:<gid></mcdataURI>…` — 수신측 그룹 스레드 귀속 키 |
| `application/vnd.3gpp.mcdata-signalling` | **SDS SIGNALLING PAYLOAD** (type 0x01) TLV: Date-time(5B, UTC초) + Conversation ID(16B UUID) + Message ID(16B UUID) + [disposition request TV `0x8N`] |
| `application/vnd.3gpp.mcdata-payload` | **DATA PAYLOAD** (type 0x03) TLV: payload 수(1B) + Payload IE(IEI 0x78, TLV-E, content-type TEXT=0x01) |

- **Conversation ID** = 그룹당 결정적 UUID(`UUID.nameUUIDFromBytes("cims-mcdata:<gid>")`) —
  "기존 대화 지속 시 Conversation ID 재사용" 규정을 그룹=상시 대화 1개로 프로파일링(기기 간 동일).
- **Message ID** = 발신 시 신규 UUID. delivered 통지 대사·로컬 저장 키.
- **Disposition**: 발신 시 `DELIVERY`(0x81) 요청 → 수신 앱이 **SDS NOTIFICATION**(type 0x05,
  DELIVERED=0x02)을 원 발신자에게 1:1 MESSAGE 로 회신 → 발신 앱 말풍선에 ✓ 표시.
- 코덱 구현: 앱 `android/ptt-client/src/main/java/com/cims/ue/ptt/mcdata/McDataCodec.kt`
  (단위테스트 `McDataCodecTest.kt`), CSP 파서 `csp/McDataCodec.{h,cpp}` (게이트·flow 로깅용 필드만).

## 4. CSP 처리 흐름

`CModuleDispatcher::EventMessage` 순서: ① 긴급경보(alert-ind, 기존 경로) → ② **MCDATA-AS
`OnMessage`** (그룹 대상일 때) → ③ 1:1 전달 (Content-Type 보존).

MCDATA-AS 게이트 (모두 controlling function 검사, TS 24.282 §9.2.2):
1. `allow_sds`=false → **403 Forbidden**
2. 발신자가 그룹 멤버가 아님 → **403 Forbidden**
3. payload 크기(MCData 는 TLV payload 합, text/plain 은 본문 길이) > `max_sds_size` → **413**

통과 시 발신자 제외 멤버에게 fan-out — `require_affiliation` 그룹은 affiliate 멤버만(긴급경보
경로와 동일 규칙). 원본 본문·Content-Type(boundary 포함) 그대로 전달(`SendSms` 5-인자
오버로드, `ext/psip/SipUserAgent/SipUserAgentSms.hpp`). `text/plain` 그룹 문자(구버전 앱)도
같은 게이트·fan-out 을 통과한다.

- 이벤트 로깅: 그룹 `events.jsonl` 에 `message_sent`(actor·conv_id·msg_id·payload_size·fanout)
  — 녹취/이력과 동일한 서비스 로그 경로. SIP 원문은 기존 SipMessageLogger jsonl 에 남는다.
- 미참여(비affiliated) 멤버는 규격상 배포 대상이 아니다. 부재중 수신(late entry/message store)은
  본 증분 범위 밖(§8).

### 4.1 메시지 보관·콘솔 모니터링

- **보관 SoT**: fan-out 성공 시 CSP 가
  `{ServiceLogDir}/message/{gid}/{YYYY}/{MM}/{DD}/{HH}/messages.jsonl` 에 1줄 append
  (`CCallDir::McDataMessageLog` — PTT 세션 여부와 무관). 레코드:
  `ts·group·from·msg_type(sds|fd|text)·conv_id·msg_id·text·size·disposition_req·fanout`
  (+FD: `file_name·file_url·file_size·file_type`). NAS 공유라 oam-svc 가 직접 스캔한다.
- **조회 API**: oam-svc `GET /api/v1/messages?date=YYYY-MM-DD[&group_id=&hour=&q=&limit=&offset=]`
  (`ems/core/oam/src/services/flow_logger.py _handle_messages`, gateway route 는 oam-svc
  pkg.json 에 self-register). `q` 는 본문·발신자·파일명 검색.
- **콘솔**: 서비스 > **그룹 메시지 이력** (`/service/messages`,
  `ems/service/console/src/pages/GroupMessagesPage.tsx`) — 날짜·그룹·검색 필터 테이블.

## 4.5 대용량 파일 — FD via HTTP (TS 23.282)

```
발신 앱 ── HTTPS POST /mcdata/fd (bytes) ──→ CSC 콘텐츠 서버(4430) ──→ {url}
발신 앱 ── SIP MESSAGE: FD SIGNALLING PAYLOAD(0x02, Payload IE=FILEURL + Metadata IE) ──→ CSP(allow_fd 게이트) ── fan-out
수신 앱 ── (size ≤ auto-recv 면 자동) HTTPS GET {url} ──→ CSC ──→ 파일
```

- **콘텐츠 서버 = CSC MCPTT 서버(4430) 동봉** (`csc/src/services/mcdata_fd.py`) — 단말이 이미
  쓰는 포트·Bearer 토큰(IdMS) 그대로. 업로드 시 서버가 게이트: 토큰 401 / 그룹 `allow_fd` 403 /
  업로더 멤버십 403 / `McDataFd.MaxBytes`(기본 50MB) 413.
- 저장: `{McDataFd.Dir | {ServiceLogging.Dir}/mcdata_fd}/{YYYY}/{MM}/{DD}/{id}.bin` +
  `index/{id}.json`(메타). 다운로드는 `GET /mcdata/fd/{id}` FileResponse 스트리밍.
- **FD SIGNALLING PAYLOAD** (TS 24.282 §15.1.3): Payload IE(0x78)=FILEURL(0x04, URL 문자열),
  Metadata IE(0x79)=RFC 5547 file-selector 부분집합 `name:"…" size:N type:MIME`.
- CSP MCDATA-AS 는 FD 를 `allow_fd` 로 게이트하고(SDS 크기 게이트 제외 — payload=URL),
  보관 레코드에 file_* 필드를 남긴다. 파일 크기 상한의 실효 강제 지점은 CSC 업로드 단.
- 수신 앱: 그룹문서 `max-data-size-auto-recv` 이내면 자동 다운로드, 초과분은 말풍선 탭으로
  수동 다운로드 → FileProvider ACTION_VIEW 로 열기 (`files/mcdata/`).

## 4.7 대용량 SDS — media plane (MSRP, TS 24.282 §9.2.3)

SDS payload 가 `<max-payload-size-sds-cplane-bytes>`(TS 24.484 서비스 설정) 를 초과하면
단말은 **standalone SDS over media plane(MSRP, RFC 4975)** 을 써야 하고, CSP participating
검사는 초과 C-plane MESSAGE 를 **403 + Warning `203 "message too large to send over
signalling control plane"`** 으로 거절한다(TS 24.282 §9.2.2 step 8; `McDataAsModule` 게이트 0).
임계 미설정(0)이면 무제한 — TS 24.484 "요소 미포함 = 제한 없음" 프로파일로 규격 적합.

```
발신 UE ── INVITE (SDP: 더미 m=audio + m=message TCP/MSRP a=sendonly a=setup:actpass) ──→ CSP
   │  McDataMediaService: 게이트(allow_sds·멤버십, C-plane 과 공용) → CmdpClient
   │  ADD_MSRP_RECV_SESSION (UDP JSON 9100) → 200 OK (a=path=cmdp, a=setup:passive, a=recvonly)
발신 UE ── TCP connect → MSRP SEND (raw TLV: signalling+payload 2건 또는 multipart 1건) ──→ cmdp
cmdp: 종단·조립 → TLV 파싱(McDataCodec 공용) → FD 스토어 기록 → MSG_RECEIVED event → CSP
CSP fan-out (하이브리드):
   ├─ MSRP 광고 단말(REGISTER Contact +g.3gpp.icsi-ref 에 icsi.mcdata) → 서버발 INVITE
   │    (더미 audio + m=message a=sendonly) + cmdp 송신 세션 → 수신 UE 가 out-connect 후 수신
   └─ 그 외(현재 앱) → FD SIGNALLING FILEURL MESSAGE (§4.5 HTTP 다운로드 경로 재사용)
→ 보관(messages.jsonl, via=msrp·file_url 포함) → 발신 레그 BYE
```

- **cmdp** (`cmdp/`, 별도 프로세스·패키지 0.1.0) — MCData media plane. TS 23.282 media storage
  function 에 해당: MSRP 를 **종단**하고(릴레이 아님) 수신 본문을 CSC FD 스토어
  (`McDataFd.Dir`, §4.5 와 동일 디렉터리·인덱스 스키마)에 기록한다 → FILEURL 폴백 수신자는
  기존 `GET /mcdata/fd/{id}`(Bearer) 로 그대로 내려받는다. 재전달용 MSRP 원문은 `{id}.msrp`.
  - 프로세스 골격 = cmp 클론: UDP JSON 제어채널(기본 **9100**), epoll 리액터(TCP 동적 fd
    + 지연 삭제), 비동기 배치 jsonl 로거(5분 버킷), deployment overlay, 스위퍼(orphan 60s /
    idle 300s). MSRP TCP 리슨 기본 **2855**, 광고 IP `MsrpIp`(단말 도달 가능해야 함).
  - MSRP 스코프: SEND 청킹(Byte-Range·end-line `$/+/#`)·응답·REPORT(Success-Report)·
    To-Path 세션 바인딩. 릴레이(RFC 4976)·MSRPS(TLS) 미지원(후속).
  - 소스 공유: `csp/McDataCodec.cpp`(TLV 파서)·`Base64.cpp` 를 직접 컴파일.
- **제어 프로토콜** (CSP `CmdpClient` ↔ cmdp, cmp envelope 동일): `ADD_MSRP_RECV_SESSION` /
  `ADD_MSRP_SEND_SESSION`(file_id 재전달) / `SET_REMOTE_PATH`(수신자 answer 후) /
  `REMOVE_MSRP_SESSION` / `HEARTBEAT` / `STATS`. 명령은 session_id 멱등.
  **비동기 이벤트**(cmdp→CSP, ack `{"event_ack":id}` + 1s×5 재전송): `MSG_RECEIVED`
  (file_id·conv/msg id·disposition·text 요약) / `SEND_RESULT` / `SESSION_ABORTED`
  (size_exceeded·orphan·idle·conn_reset·parse_error).
- **CSP `McDataMediaService`** (`csp/McDataMediaService.{h,cpp}`) — INVITE 의 m=message 감지
  (`ModuleDispatcher::EventIncomingCall` 훅, PTT-AS 그룹 분기보다 선행)·SDP answer/offer 생성
  (psip 무수정: `CSipCallRtp::m_clsMediaList` → `CSipDialog::AddSdp`)·레그 수명
  (`EventCallStart`/`EventCallEnd` 훅)·하이브리드 fan-out. 크기 게이트는 cmdp 가
  `min(그룹 max_sds_size, MaxMessageBytes)` 로 강제 — 초과 시 MSRP 413 + 세션 중단 + BYE.
- **capability 판정**: REGISTER Contact 의 `+g.3gpp.icsi-ref` 값에 `icsi.mcdata` 포함 시
  `CUserInfo::m_bMcDataMsrp`(등록 단위, 바인딩 만료와 소멸). 배포 INVITE 에는
  `Accept-Contact: *;+g.3gpp.icsi-ref="...mcdata.sds";require;explicit` 부여.
- **설정**:
  - csp.json `Setup.McDataMedia.{Enable(기본 false),Host,ControlPort(9100),LocalPort(9101)}`,
    `Setup.McData.{MaxPayloadSizeSdsCplaneBytes(기본 0=무제한),FdUrlBase}` — 콘솔
    `mcdata_media` 섹션. Enable=false 면 기존 C-plane 만 동작(현행 무영향).
  - cmdp.json `ServerIp/ServerPort(9100)/MsrpIp/MsrpPort(2855)/MaxMessageBytes(10MB)/
    SessionTimeout/OrphanReclaimSec/McDataFd.Dir(CSC 와 공유)/ServiceLogging.Dir/SystemId`.
  - csc.json `Provisioning.McData.MaxPayloadSdsCplaneBytes` → `/provisioning/me` 의 ptt
    프로파일 `mcdata.maxPayloadSdsCplaneBytes` 로 단말에 전달. **CSP 값과 운영자 동기 유지.**
- **시험**: `tests/cmdp_msrp_parser_test.cpp`(프레이머 단위, 단독 g++),
  `tests/msrp_sds_client.py`(sender/receiver/fallback/negative — 라이브 CSP+cmdp 대상 E2E).
- 패키징/수명주기: `cims.sh pkg` 대상·`cims-svc`·`cims-health(9100/udp)`·verify S4 EXPECTED 에
  cmdp 등록. agent 계약은 cmp 와 동일(`bin/cmdp config/cmdp.json`).

## 5. 앱 동작

- 발신: `PttController.sendGroupMessage` — MCData multipart 생성, msgId 반환 →
  `MessageStore`(OUT, msgId) 저장. payload(UTF-8 텍스트 바이트, 서버 게이트와 동일 기준)가
  프로비저닝 임계 `mcdata.maxPayloadSdsCplaneBytes`(0=무제한)를 초과하면 C-plane MESSAGE
  대신 **MSRP 미디어평면 발신**(§4.7) — `SipController.makeMsrpInvite`(더미 m=audio +
  `m=message TCP/MSRP` SDP 주입, Accept-Contact mcdata ICSI) → 200 OK `a=path` 로 TCP
  out-connect(`mcdata/msrp/MsrpSession`) → SIGNALLING/PAYLOAD TLV(raw, base64 CTE 없음)
  16KB 청크 SEND → 서버 BYE 로 완료. MSRP 호 상태는 `MsrpEvent` 플로우로 일반 통화
  상태와 격리(그룹 URI 동일로 인한 PTT 세션 callId 오염 방지).
- 수신(`PttService`): `multipart/mixed` → 코덱 파싱 —
  - SDS 메시지: `mcdata-info` 그룹 URI 로 **그룹 스레드 귀속**(스레드 키=그룹 ID, 발신자는
    `sender` 필드), disposition 요청 시 DELIVERED 통지 자동 회신.
  - SDS NOTIFICATION(DELIVERED): 해당 msgId 발신 문자 `delivered` 마킹 → ✓ 표시.
  - `text/plain`(구버전 앱): 종전대로 발신자 스레드 저장 (전환기 호환).
- UI(`MessagesScreen`): 그룹 스레드 수신 말풍선 위 발신자 라벨, 발신 말풍선 delivered ✓.
- 첨부: 입력바 클립 버튼(포토 피커) → `PttService.sendGroupAttachment`(업로드+FD MESSAGE).
  첨부 말풍선 = 📎 이름·크기·(받기/열기), 탭으로 다운로드/열기.

## 6. 배포 순서

1. DB: `sql/migrate_mcdata_sds.sql` (csp 보다 먼저 — SelectGroup 이 새 컬럼 참조)
2. csc 0.2.7 (그룹문서·admin API·FD 콘텐츠 서버) → 3. csp 0.2.6 (MCDATA-AS·메시지 보관)
   → 4. oam-svc 0.2.13 (/messages API) + 콘솔 dist → 5. 앱 APK 배포
- **media plane(§4.7) 추가 배포**: cmdp 0.1.0 을 먼저 기동(McDataFd.Dir=CSC 와 동일 NAS 경로)
  → csp 에 `Setup.McDataMedia.Enable=true` + 재기동. Enable=false 상태에서는 무영향이므로
  cmdp 없이도 기존 기능 정상. C-plane 임계는 csp `MaxPayloadSizeSdsCplaneBytes` 와 csc
  provisioning 값을 함께 설정(앱 MSRP 지원 배포 전에는 0=무제한 유지 권장).
- 구앱↔신서버: 구앱 text/plain 그룹 문자도 fan-out 됨(이전에는 603 Decline — 신규 동작).
- 신앱↔구서버: multipart 그룹 문자가 603 Decline (서버 먼저 배포할 것).
- 구앱이 신앱의 multipart 수신 시 무시(표시 안 됨) — 전 단말 동시 업데이트 권장.

## 7. 규격 대비 편차 (자체 프로파일)

| 항목 | 규격 | CIMS 프로파일 | 사유 |
|---|---|---|---|
| TLV 전송 인코딩 | 파트에 raw binary | **Content-Transfer-Encoding: base64** | PJSIP Java 바인딩이 본문을 String 으로만 취급 — raw binary 가 UTF-8 재인코딩에 손상. MIME 적합 인코딩이므로 표준 단말 interop 시 네이티브 바이트 경로로 교체 필요 |
| 수신 본문 취득(앱) | pjsua2 `OnInstantMessageParam.msgBody` | **`multipart/mixed` 는 msgBody 가 빈 문자열·contentType 에 boundary 누락** → 착신 INVITE 와 동일하게 `rdata.wholeMsg` 원문에서 Content-Type(boundary 포함) 헤더·본문 직접 추출 (`core/…/sip/CimsAccount.kt`) | pjsua2 Java 바인딩이 multipart body 를 String 으로 재구성하지 않음 — 이 우회 없이는 그룹 SDS/FD 수신·delivered 통지가 앱에 반영 안 됨. text/plain 등 단일 파트는 msgBody 사용 |
| 라우팅 | participating PSI 로 송신, 그룹은 mcdata-info 로 | Request-URI=그룹 URI 직행 (mcdata-info 도 포함) | 통합 배치 단순화. 서버는 양쪽 모두 수용 |
| FD 콘텐츠 서버 | media storage function (absolute URI discovery 등) | CSC 4430 `/mcdata/fd` 고정 경로 + IdMS Bearer | 단일 도메인. URL 은 FD SIGNALLING 으로 전달되므로 discovery 불필요 |
| FD 통지 | FD NOTIFICATION(다운로드 완료 등) | 미사용 | 최소 프로파일 — 필요 시 후속 |
| ICSI feature tag | Accept-Contact/P-Asserted-Service 로 요청 구분 | Content-Type 로 구분 | 단일 서비스 도메인이라 불필요 |
| 성공 응답 | 참여기능 202/200 | 200 OK (거부 403/413 은 선행 송신 — 후행 200 은 트랜잭션상 무시됨) | psip RecvMessageRequest 계약(긴급경보 경로와 동일) |
| E2E 보안 (TS 33.180) | Protected Payload | 미적용 (TLS + 서버측 RBAC) | 서버 보관·관리자 모니터링 요구와 상충 |
| READ 통지·InReplyTo | 지원 | 미사용 (DELIVERED 만; 파서는 IE skip 지원) | 최소 프로파일 |
| media plane SDS 의 SDP | `m=message` 단독 | **더미 `m=audio` 라인 동반** — 서버는 포트≠0(9) + `a=inactive` 로 응답/오퍼 (CMP 할당·RTP 없음) | pjsua2 는 알려진 미디어가 포트≠0 으로 협상돼야 콜 유지 (`got_media` 규칙). MCPTT `m=audio`+`m=application` 기존 패턴과 동일 |
| media plane 수신 배포 | 전 수신자 INVITE+MSRP | **하이브리드** — MSRP 광고 단말만 INVITE+MSRP, 그 외는 FD FILEURL MESSAGE 폴백 (§4.5 HTTP 다운로드) | 전환기 호환 (현재 앱은 MSRP 미지원). 폴백 수신자에겐 장문이 첨부(`sds_*.txt`)로 보임 |
| 단말 a=path 포트 | 단말이 해당 포트 리슨 가능 | 광고용 (단말은 항상 out-connect, 서버 상시 `a=setup:passive`) | NAT 관통 — RTP relay 와 동일한 방향성 |
| c-plane 임계 | 서비스 설정 문서로 전파 | csp.json + CSC provisioning `mcdata.maxPayloadSdsCplaneBytes` 이중 설정 (운영자 동기) | CMS 서비스설정 문서 미구현 — provisioning 채널 재사용 |

## 8. 잔여 과제

- **앱 MSRP 수신**(송신은 §5 구현 완료 — 태그 미광고라 수신은 FILEURL 폴백으로 동작):
  incoming INVITE 의 `TCP/MSRP` 감지 → `msrpMode` 격리 → answer SDP 패치(pjsua2 UAS 답
  주입은 Phase 0 스파이크로 go/no-go, 폴백=`CallOpParam.sdp`) → 서버 path 로 out-connect
  수신 → `MessageStore` 저장 + DELIVERED. feature tag(`+g.3gpp.icsi-ref` mcdata,
  `AccountSipConfig.contactParams`)는 수신 구현 후에만 광고.
  검증용 표준 단말 대역 = `tests/msrp_sds_client.py`.
- **임계 활성화**: 앱(송신+수신) 배포 후 csp `Setup.McData.MaxPayloadSizeSdsCplaneBytes` 와
  csc `Provisioning.McData.MaxPayloadSdsCplaneBytes` 를 함께 설정(현재 라이브 0=무제한).
- 1:1 standalone SDS over media plane (현재 그룹 대상만 — 비그룹 타겟 MSRP INVITE 는 403)
- FD over media plane (TS 24.282 §10.2.5 — cmdp 기계 동일, RFC 5547 file-selector SDP)
- MSRPS(TLS)·배포 레그 실패 시 FILEURL 재시도 정책·media-plane disposition
- Late entry(부재중 수신): 서버 보관분(§4.1 messages.jsonl) 기반 단말 pull API — 규격
  message store(IMAP)는 비실용, 자체 정의
- 멤버 단위 송신권한 `<mcdata-allow-transmit-data-in-this-group>` (수신전용 멤버)
- 메시지·FD 파일 retention/purge (녹취와 공통 정리 메커니즘)
- FD NOTIFICATION(다운로드 완료)·READ 통지
