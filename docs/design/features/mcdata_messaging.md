# MCData 그룹 메시징 (SDS)

그룹 문자 메시징을 3GPP MCData 규격(TS 24.282 SDS·TS 24.481 그룹문서)에 정합하게 구현한
정본 설계 문서. 대상 규격: **TS 24.282**(SDS 시그널링·§15 메시지 포맷), **TS 24.481**(그룹별
게이트), **TS 23.282**(아키텍처). TS 24.379(MCPTT)에는 사용자 메시징이 없으며, MESSAGE 용례는
긴급경보(alert-ind)뿐이다 — 그 경로는 본 기능과 별개로 유지된다
([mcptt_emergency_modes.md](mcptt_emergency_modes.md)).

> **보완 목록** — 규격 원문(TS 24.282 V18.13·24.481·24.484)과 코드를 절차 단계별로 대조해 나온 미정합 지점은
> [../../dev/mcdata_conformance_gaps.md](../../dev/mcdata_conformance_gaps.md) 에 모았다(§7 편차 표·§8 잔여 과제에 없는 것). 그 목록 §5 가 짚은
> 서술은 실제와 다르다 — 항목이 반영되면 이 문서를 고치고 목록에서 지운다.

## 1. 아키텍처

```
앱(MCData client) ── SIP MESSAGE (multipart/mixed) ──→ CSP MCDATA-AS (participating+controlling 통합)
                                                          │ ① 게이트: allow-SDS·발신자 멤버십·max-data-size
                                                          │ ② affiliation 정책 필터
                                                          └── 멤버별 MESSAGE fan-out (Content-Type 보존)
수신 앱 ──(disposition 요청 시) SDS NOTIFICATION(DELIVERED) ──→ CSP ──→ 발신 앱 (✓ 표시)
          규격형: R-URI = MCData PSI + resource-lists[발신자] → MCDATA-AS 상관·중계(§4.4)
          옛 형식: R-URI = 발신자 AoR → 1:1 경로(전환기)
```

- **CSP `MCDATA-AS` 모듈** (`csp/McDataAsModule.{h,cpp}`) — 그룹 대상 MESSAGE 의 controlling
  function. 역할 플래그 `Roles.MCDATA` (기본 ON, `csp/SipServerSetup.cpp`).
- participating/controlling 통합 배치는 CIMS PTT 와 동일한 배치(deployment) 선택으로 규격 위반이
  아니다. 단말은 그룹 URI(`sip:<gid>@domain`)로 직행 전송한다(표준의 participating PSI 라우팅
  단순화 — §7 편차 참조).
- CMP 는 관여하지 않는다. **대용량 SDS 의 미디어평면(MSRP)은 별도 프로세스 `cmdp` 가 종단한다
  (§4.7)** — C-plane 게이트(`allow_sds`·멤버십)와 보관은 두 평면이 공용이다.
- **1:1 SDS** 는 같은 본문 구조에 `request-type=one-to-one-sds`·`mcdata-request-uri=상대 URI` 로
  상대 AoR 에 직행하고, CSP 는 상대의 등록 바인딩으로 본문 그대로 전달한다(§4 ③ — 게이트·보관
  없음, §8). DELIVERED 통지는 규격형(§4.4)이 MCDATA-AS 로, 옛 형식(Request-URI = 원 발신자)이 같은 1:1 경로로 간다.

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
- MCData 그룹(SDS 또는 FD 허용)이면 **서버가 정하는 값**을 더 싣는다(TS 24.481 §7.2.2 MCData 목록·§7.2.8):
  `<mcdata-protect-media>`·`<mcdata-protect-transmission-control>` = false(없으면 «GDK 로 보호 필수» 로 읽힌다 — E2E 미구현) ·
  규칙 actions `<mcdata-allow-transmit-data-in-this-group>` = true(없으면 false = «이 그룹에는 아무도 못 보낸다», TS 24.282 §11.1 2) —
  송신 권한은 멤버 단위로 가르지 않는다) · `<mcdata-on-network-group-priority>` = 그룹 우선순위(MCPTT 와 같은 값) ·
  `<mcdata-default-charset>` = 106(UTF-8 의 IANA MIBenum — 그룹 SDS TEXT payload 의 문자 집합, TS 24.282 §6.2.2.1).
  멤버별 `<mcdata-max-data-in-single-request>` 는 싣지 않는다 — 멤버 단위 상한을 두지 않는다(그룹 상한 = `max_sds_size`).
  FD 를 허용한 그룹은 `<mcdata-on-network-max-data-size-for-FD>`(§7.2.2 l)) = 콘텐츠 서버의 FD 상한 `McDataFd.MaxBytes`(§4.5 —
  단말이 미리 아는 값이자 업로드 413 의 기준. 그룹마다 다른 값은 두지 않는다).
- admin API(`/api/v1/ptt/groups`)로 네 필드 CRUD 가능. PUT 시 기존 `GROUP_CHANGED` notify 로
  CSP 가 무중단 재적재(`CDbManager::SelectGroup`).
- **콘솔 그룹 편집 폼**(`ems/service/console/src/pages/PttGroupsWorkbenchPage.tsx`)에서 메시징/
  파일전송 토글 + 메시지 최대/자동수신 최대(byte) 입력으로 편집한다.
- CSP JSON fallback(`csp/Group/*.json`)도 `allow_sds`/`allow_fd`/`max_sds_size` 키를 지원.

## 3. 메시지 포맷 — TS 24.282 §15

SIP MESSAGE 본문 = `multipart/mixed;boundary=…` 3파트:

| 파트 Content-Type | 내용 |
|---|---|
| `application/vnd.3gpp.mcdata-info+xml` | `<mcdatainfo><mcdata-Params><request-type>group-sds｜one-to-one-sds</request-type><mcdata-request-uri type="Normal"><mcdataURI>tel:<gid 또는 상대></mcdataURI>…` — 수신측 스레드 귀속 근거: 그룹은 request-uri(그룹 ID), 1:1 은 발신자(request-uri 는 수신자 자신) |
| `application/vnd.3gpp.mcdata-signalling` | **SDS SIGNALLING PAYLOAD** (type 0x01) TLV: Date-time(5B, UTC초) + Conversation ID(16B UUID) + Message ID(16B UUID) + [disposition request TV `0x8N`] |
| `application/vnd.3gpp.mcdata-payload` | **DATA PAYLOAD** (type 0x03) TLV: payload 수(1B) + Payload IE(IEI 0x78, TLV-E, content-type TEXT=0x01) |

- **Conversation ID** = 그룹당 결정적 UUID(`UUID.nameUUIDFromBytes("cims-mcdata:<gid>")`) —
  "기존 대화 지속 시 Conversation ID 재사용" 규정을 그룹=상시 대화 1개로 프로파일링(기기 간 동일).
  1:1 은 사용자 쌍당 결정적 UUID(`"cims-mcdata:1to1:<a>:<b>"`, 쌍 정렬 — 양쪽 단말 동일).
- **Message ID** = 발신 시 신규 UUID. delivered 통지 대사·로컬 저장 키.
- **Disposition**: 발신 시 `DELIVERY`(0x81) 요청 → 수신 앱이 **SDS NOTIFICATION**(type 0x05,
  DELIVERED=0x02)을 원 발신자에게 1:1 MESSAGE 로 회신 → 발신 앱 말풍선에 ✓ 표시.
- **mcdata-info 의 client ID**: 그룹 SDS·그룹 FD 는 `<mcdata-client-id type="Normal"><mcdataString>` 로 MCData client ID(단일 MC client ID —
  `AccountConfig.effectiveMcpttClientId()`)를 싣는다(TS 24.282 §9.2.2.2.1 3)b)iv) · §10.2.4.2.1 3)b)iii) — 제어 기능의 클라이언트 단위 판정). 1:1 은 싣지 않는다.
- **Payload 여러 개**(§15.2.13 · §9.2.1.2 6)d)): DATA PAYLOAD 의 Payload IE 가 여럿이면 TEXT(0x01)·HYPERLINKS(0x03)를 온 순서대로 줄을 바꿔 이어
  `SdsMessage.text` 로 올린다. FILEURL(0x04)은 파일. BINARY·LOCATION·CODED TEXT 는 넘긴다(미구현).
- **선택 IE 와 응용 대상 메시지**(표 15.1.2.1-1 · §9.2.1.2 7)·8)): SDS SIGNALLING PAYLOAD 의 선택 IE 는 InReplyTo `0x21` → Application ID `0x22`
  → disposition 요청 `0x8N` → Extended application ID `0x7D` → User location `0x7E` → Sender MCData user ID `0x51` → Application metadata container
  `0x53`(뒤 넷은 TLV-E) 순서로 읽는다. Application ID 나 Extended application ID 가 있는 메시지는 **사용자용이 아니다** — 단말은 사용자에게
  알리지 않고, 받을 응용을 모르면 버린다. SDK 에는 응용을 등록하는 길이 없어 그런 메시지는 모두 버린다(`onSds` 로 올리지 않는다 — 시그널링·
  미디어 평면 같다). FD 의 Application ID 도 같다.
- 코덱 구현: 단말 = SDK 코어 `sdk/core/src/mcdata/sds_codec.{h,cpp}`(단위시험 `sdk/core/test/sds_codec_test.cpp` — 현장 앱·관제 앱이
  같은 코덱을 쓴다), CSP 파서 `csp/McDataCodec.{h,cpp}` (게이트·flow 로깅용 필드만).

## 4. CSP 처리 흐름

`CModuleDispatcher::EventMessage` 순서: ① 긴급경보 — mcptt-info `<alert-ind>` 는 MCPTT 경보(기존 경로), mcdata-info
`<alert-ind>` 는 MCData 경보(§8 — `McEmergencyAlertServiceOf`, mcdata-info 파트가 있는 MESSAGE 는 MCPTT 경보로 읽지 않는다) → ② **MCDATA-AS
`OnMessage`** (규격형 disposition 통지 §4.4, 그룹 대상 SDS) → ③ 1:1 전달 (Content-Type 보존). 1:1 SDS/FD·옛 형식
SDS NOTIFICATION 은 ③ — 상대의 등록 바인딩으로 본문 그대로 전달하며 게이트·보관은 없다(§8). 1:1 SDS 는 ③ 에서 disposition
상관 색인(§4.4)에 오른다.

MCDATA-AS 게이트 (모두 controlling function 검사 — SDS TS 24.282 §9.2.2.4.2 · media plane §9.2.3.4.4 · FD §10.2.4.4.2,
`McDataGates`):
0. FD 만 — Payload IE 가 하나가 아니면 **403 `210`**, FILEURL 이 아니면 **403 `211`**, FILEURL 이 이 서버의 콘텐츠 서버 파일이 아니면
   **403 `212 file referenced by file URL does not exist`**(§10.2.4.4.2 6)·7) — 1:1 FD 도 ③ 에서 같은 검사, `McDataFdPayloadCheck`).
   «이 서버의 파일» = 콘텐츠 서버 base(`Setup.McData.FdUrlBase`, 비면 CSC PublicUrl — CSP 가 FD URL 을 만들 때와 같은 값)와 scheme·
   host·port 가 같고 경로가 `/mcdata/fd/<32 hex>`(`McDataFdUrlIsOurs`). 규격 단말은 받은 URL 로 Bearer 토큰을 실어 GET 하므로(§10.2.3.1)
   다른 호스트의 URL 은 배포하지 않는다. 그 다음 그 URL 에 HEAD(§6.7.3.1 — CSC 내부 토큰, §4.5)로 파일이 있는지 보고 404 면 `212`, 그 밖의
   실패(401·연결 실패·옛 CSC 405)는 확인하지 못한 것이라 배포하고 로그를 남긴다(psip `CHttpClient::DoHead`).
1. 발신자가 그룹 멤버가 아님 → **403 `116 user is not part of the MCData group`**(6)e) · 7)c) · 12)c))
2. `allow_sds`=false → **403 `206 short data service not allowed for this group`**, FD 는 `allow_fd`=false → **403 `213 file distribution
   not allowed for this group`**(6)f) · 7)d) · 12)d))
3. 발신자가 그 그룹에 제휴하지 않음 → **403 `120 user is not affiliated to this group`**(6)j) · 7)g) · 12)g))
4. payload 크기(MCData 는 TLV payload 합, text/plain 은 본문 길이) > `max_sds_size` → **403 `217 user not authorised for SDS communications
   on this group identity due to message size`**(6)i)iii) · §11.1 5)) — media plane 은 cmdp 가 MSRP 413 + BYE(아래 §4.7)
5. 배포 대상(발신자 제외 제휴 멤버, §6.3.4)이 없음 → **403 `198 no users are affiliated to this group`**(6)k)ii) · 7)i) · 12)i)) —
   media plane 은 INVITE 를 받을 때 3·5 를 본다

제휴는 `require_affiliation` 그룹만 본다 — 그 밖의 그룹은 멤버 전원을 초대하는 그룹이라 멤버십이 곧 제휴다(긴급경보·그룹 호와 같은
규칙). 제휴 저장소(DB)에 닿지 못하면 제휴를 판정할 수 없어 **500**(§9.2.2.4.2 1) — 그룹 전원에게 보내지 않는다). 통과 시 배포 대상에게 fan-out. 원본 본문·Content-Type(boundary 포함) 그대로 전달(`SendSms` 5-인자
오버로드, `ext/psip/SipUserAgent/SipUserAgentSms.hpp`). `text/plain` 그룹 문자(구버전 앱)도
같은 게이트·fan-out 을 통과한다 — 규격은 mcdata-info·mcdata-signalling·mcdata-payload 가 없는 MESSAGE 를 403 `199 expected MIME bodies not in
the request` 로 거절한다(§9.2.2.4.2 2)). 앱이 규격형으로 바뀔 때까지 이 검사는 엄격 검사 스위치 `Setup.Mcptt.StrictCheck`(기본 `log` —
로그만, `enforce` 면 403 199) 아래 둔다(결정 D5).

- 이벤트 로깅: 그룹 `events.jsonl` 에 `message_sent`(actor·conv_id·msg_id·payload_size·fanout)
  — 녹취/이력과 동일한 서비스 로그 경로. SIP 원문은 기존 SipMessageLogger jsonl 에 남는다.
- 미참여(비affiliated) 멤버는 규격상 배포 대상이 아니다. 부재중 수신(late entry/message store)은
  본 증분 범위 밖(§8).
- **등록 flow 밖에서 온 MESSAGE 의 재인증** — SDS MESSAGE 는 mcdata-info·base64 TLV·Accept 헤더로
  본문이 짧아도 약 1.6KB 라, UDP 등록 단말(pjsip)은 RFC 3261 §18.1.1 에 따라 TCP 로 승격해 보낸다.
  CSP 는 등록 바인딩과 다른 flow 의 요청을 Digest 로 재인증하고 바인딩은 만들지 않는다
  ([registration_binding_set.md](registration_binding_set.md) §3). 단말은 401 에 자격을 붙여 CSeq+1 로
  재발행해야 통과한다 — pjsua 의 `Account::sendRequest` 경로에는 이 재발행이 없어 엔진 소스 정본
  `ext/pjproject` 의 `pjsua_acc.c`(`cims_send_request_reauth`)로 보강했다(인벤토리
  `ext/pjproject/README.CIMS.md`, [android_ue_m1_pjsip_integration.md](android_ue_m1_pjsip_integration.md) §2.5).
  TLS/TCP 등록 단말은 승격이 없어 해당 없음. 서버 변경 없음.

### 4.1 메시지 보관·콘솔 모니터링

- **보관 SoT**: fan-out 성공 시 CSP 가
  녹취 영역 `{Recording.Dir}/message/{gid}/{YYYY}/{MM}/{DD}/{HH}/messages.jsonl` 에 1줄 append
  ([site_directory_layout.md](site_directory_layout.md))
  (`CCallDir::McDataMessageLog` — PTT 세션 여부와 무관). 레코드:
  `ts·group·from·msg_type(sds|fd|text)·conv_id·msg_id·text·size·disposition_req·fanout`
  (+FD: `file_name·file_url·file_size·file_type`). NAS 공유라 oam-svc 가 직접 스캔한다.
- **조회 API**: oam-svc `GET /api/v1/messages?date=YYYY-MM-DD[&group_id=&hour=&q=&limit=&offset=]`
  (`ems/core/oam/src/services/flow_logger.py _handle_messages`, gateway route 는 oam-svc
  pkg.json 에 self-register). `q` 는 본문·발신자·파일명 검색.
- **콘솔**: 서비스 > **그룹 메시지 이력** (`/service/messages`,
  `ems/service/console/src/pages/GroupMessagesPage.tsx`) — 날짜·그룹·검색 필터 테이블.

### 4.4 disposition 통지 — 규격 경로 (TS 24.282 §12.2.1.1·§12.2.2.1·§12.2.3)

단말은 DELIVERED(·READ) 통지를 Request-URI = MCData 참여 기능 PSI(ue-init-config `MCData-Service-Details/Server-URI`, 명목값
`sip:mcdata_psi@<도메인>`)로, 대상 MCData ID 를 `application/resource-lists+xml` entry 하나로(§12.2.1.1 3)), 그룹 SDS 의 통지면
mcdata-info `<mcdata-calling-group-id>` 를 실어 보낸다(5)). CSP `CMcDataAsModule::OnDispositionNotification` 이 참여·제어 기능을
겸해 처리한다 — 본문에 resource-lists 와 SDS NOTIFICATION 이 함께 있을 때만(없으면 옛 형식이라 ③ 1:1 경로):

| 단계 | 검사 | 거절 |
|---|---|---|
| §12.2.3 2) | Accept-Contact 에 ICSI `urn:urn-7:3gpp-service.ims.icsi.mcdata.sds` | 403 |
| 3) | resource-lists entry 가 정확히 하나 | 403 Warning `145 unable to determine called party` |
| 4)·5) | 대화·메시지 ID 가 이 서버가 전달한 SDS 이고 그 발신자가 통지 대상 — 상관 색인 `McDataCorrelateSds`(그룹 SDS 는 `McDataArchiveMessage`, 1:1 SDS 는 ③ 전달이 올린다. 최근 24 시간·최대 20000 건 인메모리, CSP 재기동 전 발신분은 상관 불가) | 403 Warning `216 unable to correlate the disposition notification` |
| 4) | 통지의 그룹 문맥(`<mcdata-calling-group-id>`, 1:1 이면 없음)이 원 SDS 의 것과 같다 — 그룹 SDS 의 통지인데 그룹이 없거나 다른 그룹이면 다른 대화의 통지다 | 403 Warning `216` |
| 15)b) | 그룹 통지면 통지자가 그 그룹 멤버 | 403 Warning `116 user is not part of the MCData group` |

통과하면 원 발신자에게 새 MESSAGE 로 중계한다 — mcdata-info `<mcdata-request-uri>` = 원 발신자(14)), `<mcdata-calling-user-id>` =
통지자(§12.2.2.1 10)), 그룹이면 `<mcdata-calling-group-id>`, 받은 mcdata-signalling 파트 원문 그대로(15)d)·16) — 집계(TDC1)는
하지 않는다). 헤더 = `Accept-Contact`(g.3gpp.mcdata.sds · ICSI mcdata.sds, require;explicit — 8))·`P-Asserted-Service`(11))·
`P-Asserted-Identity` = 제어 기능 PSI(13) — 그룹 통지는 그룹 URI, 1:1 은 `mcdata_psi`). 원 발신자가 등록돼 있지 않으면 480(가입자
모름 404 — 1:1 전달과 같은 구분). 검증 = `tests/sds_disposition_spec.py`(중계·145·216·ICSI 거절).

### 4.3 1:1 SDS/SMS 보관 — 관제 데스크 이력

1:1 SDS/SMS(Request-URI=상대 AoR 직행, §4 표)는 기본적으로 CSP 가 등록 바인딩으로 본문만 전달하고 보관하지
않는다. 관제 데스크 통합 이력([dispatch_center.md §5.7a](dispatch_center.md))이 1:1 메시지 모니터링을 요구하면
`Setup.McData.StoreOneToOneSds`(기본 off)를 켠다:

- **보관 SoT**: CSP 가 1:1 MESSAGE 릴레이 시점(`CModuleDispatcher::EventMessage` 의 1:1 분기, SendSms 앞)에
  `{Recording.Dir}/message_direct/{YYYY}/{MM}/{DD}/{HH}/messages.jsonl` 에 1줄 append(`CCallDir::McData1to1Log`).
  레코드: `ts·from·to·msg_type(sds|text|fd)·conv_id·msg_id·text·size·disposition_req`. disposition 통지
  (`SDS NOTIFICATION`)는 이력이 아니라 제외한다.
- **전량 보관, 조회 시 게이트**: 범위 한정 보관은 역할 배정 변동 시 이력 결손·정책 불투명을 낳으므로
  전량 보관하고, **열람 권한은 조회 시점에 역할 `monitor_call` 로 게이트**한다(감시 멤버가 발신 또는 수신인
  1:1 메시지만, [dispatch_center.md §5.7a](dispatch_center.md)). 조회 = `GET /provisioning/history?kind=message`
  (그룹 SDS + 1:1 병합, [android_ue_provisioning.md §3-2](android_ue_provisioning.md)), 감사 `E-AUD-016 tap_mode=history`.
- 개인정보 영향이 크므로 기본 off — 합법감청 운용(고지·동의 전제, [dispatch_center.md §5.8](dispatch_center.md))에서만 켠다.

## 4.5 대용량 파일 — FD via HTTP (TS 23.282)

```
발신 앱 ── HTTPS POST /mcdata/fd (mcdata-info + 파일) ──→ CSC 콘텐츠 서버(4430) ──→ 201 Location: {url}
발신 앱 ── SIP MESSAGE: FD SIGNALLING PAYLOAD(0x02, Payload IE=FILEURL + Metadata IE) ──→ CSP(allow_fd 게이트) ── fan-out
수신 앱 ── (size ≤ auto-recv 면 자동) HTTPS GET {url} ──→ CSC ──→ 파일
```

- **콘텐츠 서버 = CSC MCPTT 서버(4430) 동봉** (`csc/src/services/mcdata_fd.py` — TS 23.282 media storage function) — 단말이 이미
  쓰는 포트·Bearer 토큰(IdMS) 그대로 — 토큰 scope `3gpp:mc:data_service` 검사(TS 33.180 B.10, `IdMs.ScopeEnforcement`),
  업로더 신원 = 토큰 `mcdata_id`(= `mcptt_id`, 단일 MC service ID — [mcx_identity_scope.md](mcx_identity_scope.md) §1).
- **업로드**(TS 24.282 §10.2.2) — 두 말투를 받는다.

  | 말투 | 요청 | 그룹·발신자 |
  |---|---|---|
  | 규격형(§10.2.2.1 4)~8)) | `POST /mcdata/fd`, `Content-Type: multipart/mixed` — `application/vnd.3gpp.mcdata-info+xml` + `application/octet-stream`(Content-Length = 파일 크기) | mcdata-info `<request-type>` `one-to-one-fd`\|`group-fd` · `<mcdata-request-uri>`(그룹, group-fd 필수) · `<mcdata-calling-user-id>` |
  | 간이형(자체 단말·계측기) | `POST /mcdata/fd?name=&group=&type=`, 본문 `application/octet-stream`(또는 multipart/form-data `file`) | query `group`(있으면 그룹 FD) |

  응답 = **201 Created + `Location`**(저장한 파일의 URL — 단말은 이 값을 FD 의 FILEURL 로 쓴다, §10.2.2.2 2)b)) + JSON
  `{id, url, size, name}`(`url` = Location). **URL 의 base = CSC 공개 base URL**(`McpttServer.PublicUrl`, 없으면 요청 Host) — CSP 가
  FILEURL 을 같은 base 와 대조하므로(§4 게이트 0, 403 `212`) 단말이 CSC 에 붙은 이름과 무관하게 한 값이어야 한다. 파일 이름은 octet-stream
  파트의 `Content-Disposition filename`(없으면 query `name`), 형식은 query `type`.
- **업로드 판정**(§10.2.2.2 1)): 토큰 없음 403 / 무효 401 / scope 부족 403 `insufficient_scope` · `<mcdata-calling-user-id>` 가 토큰의
  MCData ID 와 다르면 403 · 그룹 FD — 모르는 그룹 404, 그룹 `allow_fd` 꺼짐 403, 올리는 사람이 멤버 아님 403(전송 제어) · 크기 —
  그룹 FD 는 그룹 문서 `<mcdata-on-network-max-data-size-for-FD>`, 1:1 은 service configuration `<max-data-size-fd-bytes>` 자리의 값을
  넘으면 **413**. 두 값 모두 `McDataFd.MaxBytes`(기본 50 MB — `mcdata_fd.max_bytes()`). 형식 오류 400, `message/external-body`
  (network-stored file — MCData message store 없음) 501.
- **다운로드**(§10.2.3) `GET /mcdata/fd/{id}` — 수신 제어(§10.2.3.2 1)): **그룹에 올린 파일은 그 그룹의 지금 멤버와 올린 사람만**
  (그 밖 403). 1:1 파일은 업로드에 수신자가 실리지 않으므로(§10.2.2.1 5)) MCData scope 토큰과 URL(추측 불가 id)로 받는다. 파일 이름은
  `Content-Disposition`(RFC 6266 — ASCII 대체 이름 + `filename*=UTF-8''…`).
- **존재 확인**(§6.7.3) `HEAD /mcdata/fd/{id}` — 200(본문 없음, `Content-Length`·`Content-Type`) / 404. 단말 토큰이면 GET 과 같은 수신
  제어, **제어 기능(CSP)은 내부 토큰**(`Authorization: Bearer <InternalApi.Token>` — `/internal/*` 과 같은 값, HEAD 에만 통한다)으로
  부르고 그때 응답에 `X-Cims-Fd-Group`(올린 그룹, 1:1 은 빈 값)·`X-Cims-Fd-Uploader` 가 실린다 — FILEURL 을 다른 그룹에 다시 돌리는 것을
  거를 때 쓸 수 있다(CSP 는 쓰지 않는다 — 그 그룹 밖 수신자는 GET 이 403 이다).
- 저장: `{McDataFd.Dir | {Content.Dir}/mcdata_fd}/{YYYY}/{MM}/{DD}/{id}.bin` +
  `index/{id}.json`(메타 — name·size·type·group·uploader·ts). CMDP 의 media plane 저장분(§4.7)도 같은 스키마라 같은 수신 제어를 받는다.
- **FD SIGNALLING PAYLOAD** (TS 24.282 §15.1.3): Payload IE(0x78)=FILEURL(0x04, URL 문자열),
  Metadata IE(0x79)=RFC 5547 file-selector 부분집합 `name:"…" size:N type:MIME`.
- CSP MCDATA-AS 는 FD 를 `allow_fd` 로 게이트하고(SDS 크기 게이트 제외 — payload=URL),
  보관 레코드에 file_* 필드를 남긴다. 파일 크기 상한의 실효 강제 지점은 CSC 업로드 단.
- 수신 앱: 그룹문서 `max-data-size-auto-recv` 이내면 자동 다운로드, 초과분은 말풍선 탭으로
  수동 다운로드 → FileProvider ACTION_VIEW 로 열기 (`files/mcdata/`).
- **단말 SDK(`libcimsue`, [ue_sdk.md](ue_sdk.md))**: `CscClient::uploadFd`(그룹 FD 면 `group` 지정 → 서버 게이트, 1:1 은 없음) →
  `Engine::sendGroupFd`/`sendFd`(request-type `group-fd`/`one-to-one-fd`, 두 파트) · 수신 `onSds(fd=true, fileUrl·fileName·fileSize·fileType)` ·
  `CscClient::downloadFd`(FILEURL 의 **경로만** 취해 자기 CSC 로 — Bearer 를 FILEURL 의 호스트로 보내지 않고, 발신자가 다른 주소로 올렸어도 같은
  NAS 저장소에서 받는다). C API `cimsue_csc_upload_fd/download_fd`·`cimsue_engine_send_group_fd/send_fd`, .NET `CscClient.UploadFd/DownloadFd`·
  `Account.SendGroupFd/SendFd`. 관제 데스크톱은 자동 다운로드를 하지 않고 [받기]로 받는다([dispatch_desktop_ui.md](dispatch_desktop_ui.md) §4.4).
- **시험(계측기)**: libcsim `SimSession::SendFd`(IdMS 토큰 → `POST /mcdata/fd` → FD SIGNALLING MESSAGE)·`DownloadFd`(`GET`, Bearer) +
  `McDataSds::buildGroupFd|buildOneToOneFd`/`parse`(FILEURL·Metadata) — 워커 단계 `fd_send`/`fd_recv`, 지표 `fd_upload_ms`·`fd_delay_ms`·
  `fd_download_ms`·`fd_download_pct`, 동봉 `MCDATA-FD-GROUP`·`MCDATA-FD-1TO1`([test_instrument.md](test_instrument.md) §3.1 ⑪″). 신원의
  IdMS 로그인은 creds `login/loginPw`(`cims-tester creds-from-db` 가 `users.login_id/passwd` 를 싣는다).

## 4.7 대용량 SDS — media plane (MSRP, TS 24.282 §9.2.3)

SDS payload 가 `<max-payload-size-sds-cplane-bytes>`(TS 24.484 서비스 설정) 를 초과하면
단말은 **standalone SDS over media plane(MSRP, RFC 4975)** 을 써야 하고, CSP participating
검사는 초과 C-plane MESSAGE 를 **403 + Warning `203 "message too large to send over
signalling control plane"`** 으로 거절한다(TS 24.282 §9.2.2.3.1 8); `McDataAsModule` 게이트 0).
임계 미설정(0)이면 무제한 — TS 24.484 "요소 미포함 = 제한 없음" 프로파일로 규격 적합.

```
발신 UE ── INVITE (SDP: 더미 m=audio + m=message TCP/MSRP a=sendonly a=setup:actpass) ──→ CSP
   │  McDataMediaService: 게이트(allow_sds·멤버십, C-plane 과 공용) → CmdpClient
   │  ADD_MSRP_RECV_SESSION (UDP JSON 9100) → 200 OK (a=path=cmdp, a=setup:passive, a=recvonly)
발신 UE ── TCP connect → MSRP SEND (raw TLV: signalling+payload 2건 또는 multipart 1건) ──→ cmdp
cmdp: 종단·조립 → TLV 파싱(McDataCodec 공용) → FD 스토어 기록 → MSG_RECEIVED event → CSP
CSP fan-out (하이브리드):
   ├─ MSRP 광고 단말(REGISTER Contact +g.3gpp.icsi-ref 에 icsi.mcdata) → 서버발 INVITE
   │    (multipart/mixed: **mcdata-info**(request-uri=그룹, calling-user-id=원발신자 — 수신
   │    단말의 스레드 귀속·발신자 표시·disposition 회신 대상) + SDP(더미 audio PCMU/PCMA
   │    + m=message a=sendonly)) + cmdp 송신 세션 → 수신 UE 가 out-connect 후 수신
   └─ 그 외 → FD SIGNALLING FILEURL MESSAGE (§4.5 HTTP 다운로드 경로 재사용)
→ 보관(messages.jsonl, via=msrp·file_url 포함) → 발신 레그 BYE
```

- **배포는 수신 완료 통지(`MSRP_MSG_RECEIVED`) 하나로 한다** — cmdp 는 저장을 마친 뒤 통지하고 통지 payload 가 배포에 필요한
  값(file_id·그룹·발신자·크기·TLV 요약)을 다 싣는다. 발신 단말은 Success-Report 를 받은 뒤 곧바로 BYE 할 수 있으므로
  (RFC 4975 §7.1.2 — 세션 종료는 발신자 재량) 통지보다 BYE 가 먼저 CSP 에 닿아도 배포한다: 끝난 수신 leg 의 cmdp 세션을 30 s
  동안 기억해(`McDataMediaService::m_mapEndedRecv`) 그 세션의 통지를 한 번 받는다. 배포를 마친 세션은 기억에서 빠지므로
  cmdp 의 이벤트 재전송(1 s × 5)은 중복으로 걸러진다.

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
- **제어 프로토콜** (CSP `CmdpClient` ↔ cmdp) — cmp 와 동일한 **envelope v2** `{hdr,payload}`
  ([cmp_media_api.md](../../api/cmp_media_api.md) §2·§9 의 hdr 필드·에러 코드 준용,
  `hdr` 없는 패킷은 `BAD_REQUEST` 거절). function prefix 는 `MSRP`, `hdr.service` 는
  `mcdata` 고정. 명령 정본:

  | cmd | payload | 비고 |
  |---|---|---|
  | `MSRP_ADD` | `mode`(`recv`\|`send`) + `session_id`, recv: `caller`/`group_id`/`remote_path`/`max_size`, send: `file_id`/`caller`/`callee`/`content_type` | 멱등 (동일 session_id 재요청 = 기존 경로 반환). 응답 `msrp_path`/`local_ip`/`local_port`. send 의 file_id 부재는 `NOT_FOUND` |
  | `MSRP_MODIFY` | `session_id`, `remote_path` (수신자 answer 후 확정) | 소실 세션 부활 금지 → `NOT_FOUND` (RELAY_MODIFY 와 동일 계약) |
  | `MSRP_REMOVE` | `session_id` | 자연 멱등 — 소실 세션 재전송도 OK |
  | `HEARTBEAT` / `STATS` | CORE — sesid/service 미포함. HEARTBEAT 응답 `resource.msrp` = 기능 광고 | |

  **비동기 이벤트**(cmdp→CSP, `type:"event"` — cmp_media_api.md §8 과 동형. ack 는 동일
  trans_id 의 `type:"response"`, 미ack 시 1s×5 재전송 후 폐기): `MSRP_MSG_RECEIVED`
  (file_id·conv/msg id·disposition·text 요약) / `MSRP_SEND_RESULT`(status ok/failed·bytes) /
  `MSRP_ABORTED` (size_exceeded·orphan·idle·conn_reset·parse_error·store_error).
  이벤트 trans_id 는 cmdp 가 발행(부팅 ms 시드 — 재시작 ack 오매칭 방지).
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
    (`FdUrlBase` 비면 CSC 가 알려주는 단말용 서비스 URL = `McpttServer.PublicUrl`) —
    `mcdata_media` 섹션. Enable=false 면 기존 C-plane 만 동작(현행 무영향).
  - cmdp.json `ServerIp/ServerPort(9100)/MsrpIp/MsrpPort(2855)/MaxMessageBytes(10MB)/
    SessionTimeout/OrphanReclaimSec/McDataFd.Dir(CSC 와 공유 — 비면 `<Content.Dir>/mcdata_fd`)/ServiceLogging.Dir/Content.Dir/SystemId`
    (영역 경로 `ServiceLogging.Dir`·`Content.Dir` 은 배포 때 사이트 디렉터리에서 유도).
  - csc.json `Provisioning.McData.MaxPayloadSdsCplaneBytes` → `/provisioning/me` 의 ptt
    프로파일 `mcdata.maxPayloadSdsCplaneBytes` 로 단말에 전달. **CSP 값과 운영자 동기 유지.**
- **단말 SDK 코어**(`libcimsue` `mcdata/msrp`, [ue_sdk.md](ue_sdk.md) §4.2 «media plane SDS»): 그룹 SDS 가 `AccountConfig.maxSdsCplaneBytes`
  (= 프로비저닝 `mcdata.maxPayloadSdsCplaneBytes`)를 넘으면 `sendGroupSds` 가 위 발신 절차로 보내고(16 KB 청크 stop-and-wait, 최종 결과
  = `onRequestResult` method `MSRP`), `AccountConfig.mcdataMsrp` 면 REGISTER Contact `+g.3gpp.icsi-ref` 에 mcdata.sds 를 합쳐 서버발 배포를
  받는다(`onSds` `mediaPlane`). 1:1 은 시그널링 평면 그대로.
- **시험**: `tests/cmdp_msrp_parser_test.cpp`(프레이머 단위, 단독 g++),
  `tests/msrp_sds_client.py`(sender/receiver/fallback/negative — 라이브 CSP+cmdp 대상 E2E),
  계측기 = libcsim `cspsim/McDataMsrp.{h,cpp}` + `SimSession::SendSdsMedia`(발신)·`AnswerMsrp`(수신, UE 풀 `msrp`) — 단계 `sds_send plane: media`,
  동봉 `MCDATA-SDS-GROUP-MEDIA`([test_instrument.md](test_instrument.md) §3.1 ⑪′).
- 패키징/수명주기: `cims.sh pkg` 대상·`cims-svc`·`cims-health(9100/udp)`·verify S4 EXPECTED 에
  cmdp 등록. agent 계약은 cmp 와 동일(`bin/cmdp config/cmdp.json`).

## 5. 앱 동작

본문 조립·평면 선택(시그널링 / media plane)·수신 해석은 SDK 코어(`libcimsue` — [ue_sdk.md](ue_sdk.md) §4.2)가 하고, 앱은 저장·표시·통지
판단만 한다. 아래 이름은 현장 앱(`android/ptt-client`)의 것이다 — 관제 앱 두 벌은 같은 SDK API 를 쓴다.

- 발신: `PttService.sendMessage(peer)` 가 msgId 를 발급해 `MessageStore`(OUT, PENDING) 에 먼저
  저장하고 `PttController.sendSds(peer, text, msgId)`(`PttMessaging.kt` `MessagingPlane.sendSds`)로 발신 — peer 가 받아 둔 편성
  그룹이면 SDK `Account.sendGroupSds`(group-sds), 아니면 `Account.sendSds`(one-to-one-sds, 항상 C-plane).
  갈림 판정은 앱이 «받아 둔 편성 그룹 목록에 그 키가 있는가» 로 한다(번호 모양으로 추측하면
  숫자 그룹 id 에서 틀린다). 관제 앱은 이 판정을 세션 한 곳에 둔다
  (`DispatchSession.sendSdsTo`, [android_dispatch_tablet.md](android_dispatch_tablet.md) §6.9a). 그룹 SDS 의 payload(UTF-8 텍스트 바이트, 서버
  게이트와 동일 기준)가 프로비저닝 임계 `mcdata.maxPayloadSdsCplaneBytes`(SDK `AccountConfig.maxSdsCplaneBytes`, 0=무제한)를 초과하면
  **코어가** C-plane MESSAGE 대신 **MSRP 미디어평면**(§4.7)으로 보낸다 — INVITE(`m=message TCP/MSRP`, Accept-Contact mcdata ICSI) →
  200 OK `a=path` 로 TCP out-connect(`sdk/core/src/mcdata/msrp.{h,cpp}`) → SIGNALLING/PAYLOAD TLV(raw, base64 CTE 없음) 청크 SEND →
  서버 BYE 로 완료. 최종 결과는 시그널링 평면과 같은 token 으로 method `MSRP` 가 온다(`MessagingPlane.onSendResult`).
  - **전송 상태 말풍선**: C-plane·MSRP 모두 PENDING(🕓, media plane 은 진행률 — `PttService.sendProgress`) → 성공 SENT(✓)/실패 FAILED(⚠, 탭=같은 msgId 재전송 —
    `PttService.resendMessage`) — `MessageStore.sendState` + `PttController.sendResult`. C-plane 의 결과는 MESSAGE 트랜잭션 최종 응답
    (token 상관 — 2xx=SENT, 그 밖=FAILED). 401/407 재인증은 코어가 하므로 앱에는 최종 결과만 온다. DELIVERED 통지 수신 시 ✓✓.
    서비스 재기동 시 잔존 PENDING 은 FAILED 로 마감(재전송 유도). 첨부(FD) 발신은 업로드 성공 시 SENT.
- 수신(MSRP 미디어평면): 계정 설정 `AccountConfig.mcdataMsrp` 이면 코어가 REGISTER Contact 의 `+g.3gpp.icsi-ref` 목록에 ICSI
  `mcdata.sds` 를 싣고, 서버발 배포 INVITE(`TCP/MSRP`)를 통화로 올리지 않고 받아(answer `m=message a=setup:active`) 서버 `a=path` 로
  out-connect 해 청크를 조립한다 → 코덱 파싱 → 아래 C-plane 수신과 같은 `onSds`(`SdsMessage.mediaPlane = true`).
  그룹/발신자는 INVITE mcdata-info(request-uri/calling-user-id) — 발신자를 알 수 없으면(발신자 = 그룹) DELIVERED 회신을 억제한다.
- 수신(`PttService.onSds` ← `PttController.incomingSds`): 코어가 해석한 `SdsMessage` 한 건 —
  - SDS 메시지: 스레드 키 = `groupUri` 가 있으면(group-*) 그룹 ID(발신자는 `sender` 필드), 비어 있으면(one-to-one-*) **발신자**
    (1:1 의 request-uri 는 수신자 자신이라 키로 쓰지 않는다). disposition 요청 시 DELIVERED 통지 자동 회신
    (`MessagingPlane.sendSdsNotification` — §4.4).
  - SDS NOTIFICATION(DELIVERED): 해당 msgId 발신 문자 `delivered` 마킹 → ✓ 표시.
  - FD: 첨부 말풍선으로 저장하고, 크기가 그룹 문서 `max-data-size-auto-recv` 이내면 바로 내려받는다.
- UI(`MessagesScreen`): 그룹 스레드 수신 말풍선 위 발신자 라벨, 발신 말풍선 delivered ✓.
- 첨부: 입력바 클립 버튼(포토 피커) → `PttService.sendAttachment` → `MessagingPlane.sendAttachment`(SDK `CscClient.uploadFd` 업로드 +
  `Account.sendGroupFd`/`sendFd` FD MESSAGE). 첨부 말풍선 = 📎 이름·크기·(받기/열기), 탭으로 다운로드(`downloadAttachment`)/열기.
- `android/core-sip` 에는 SDK 이전의 MSRP 호 조립(`SipController.makeMsrpInvite`·`acceptMsrpCall`)이 남아 있다 — 현장 앱은 쓰지 않는다.

## 6. 배포 순서

1. DB: `sql/migrate_mcdata_sds.sql` (csp 보다 먼저 — SelectGroup 이 새 컬럼 참조)
2. csc 0.2.7 (그룹문서·admin API·FD 콘텐츠 서버) → 3. csp 0.2.6 (MCDATA-AS·메시지 보관)
   → 4. oam-svc 0.2.13 (/messages API) + 콘솔 dist → 5. 앱 APK 배포
- **media plane(§4.7) 추가 배포**: cmdp 0.1.0 을 먼저 기동(FD 스토어 = CSC 와 동일 NAS 경로 — 둘 다 비워 두면 콘텐츠 영역 `mcdata_fd/`)
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
| 1:1 SDS | participating → 상대 participating 경유, 서버 보관 | Request-URI=상대 AoR 직행 + `one-to-one-sds` mcdata-info. CSP 는 등록 바인딩으로 본문 그대로 전달(게이트 없음, 보관은 `Setup.McData.StoreOneToOneSds` 시 §4.3) | 통합 배치 단순화. 관제 이력은 §4.3 |
| 착신 도달 불가 응답 | — | **480 Temporarily Unavailable** (가입자는 알지만 유효한 등록 바인딩 없음) / **404 Not Found** (가입자 자체를 모름) / **500** (전달 자체 실패) | RFC 3261 §21.4.18 이 "가입자는 알지만 유효한 전달 위치가 없음"을 480 으로 규정. TS 24.229 의 미등록 처리와 같은 구분. 603 Decline 은 "착신자가 거부했다"는 전역 실패라 포크·재시도까지 막으므로 쓰지 않는다 |
| media plane SDS 대상 | 그룹·1:1 모두 | **그룹만** (`McDataMediaService` 가 그룹 조회 필수) — 1:1 은 크기와 무관하게 C-plane, C-plane 임계 게이트(§4.7)도 그룹 대상만 | 1:1 standalone 은 §8 |
| FD 콘텐츠 서버 | media storage function (absolute URI discovery 등) | CSC 4430 `/mcdata/fd` 고정 경로 + IdMS Bearer | 단일 도메인. URL 은 FD SIGNALLING 으로 전달되므로 discovery 불필요 |
| FD 크기 상한 | 그룹 FD = 그룹 문서 `<mcdata-on-network-max-data-size-for-FD>`, 1:1 = service configuration `<max-data-size-fd-bytes>` | 둘 다 콘텐츠 서버의 한 값 `McDataFd.MaxBytes` — 그룹 문서는 그 값을 싣는다 | 그룹마다 다른 상한을 두지 않는다(그룹 속성·DB 컬럼 없음). MCData service configuration 문서는 내지 않는다([mcx_identity_scope.md](mcx_identity_scope.md) §10) |
| FD 1:1 수신 제어 | 수신 제어 정책상 받을 수 없는 사용자는 403(§10.2.3.2 1) — 적용 방법은 Editor's Note FFS) | 그룹 파일만 멤버십으로 가른다. 1:1 파일은 MCData scope 토큰 + URL | 규격 업로드에 1:1 수신자가 실리지 않는다(§10.2.2.1 5)) — 콘텐츠 서버가 수신자를 알 길이 없다 |
| network-stored file 업로드 | `message/external-body`(MCData message store 의 파일을 가져와 저장, §10.2.2.2) | 501 | MCData message store 미구현 |
| FD 통지 | FD NOTIFICATION(다운로드 완료 등) | 미사용 | 최소 프로파일 — 필요 시 후속 |
| ICSI feature tag | Accept-Contact/P-Asserted-Service 로 요청 구분 | Content-Type 로 구분 | 단일 서비스 도메인이라 불필요 |
| 성공 응답 | 참여기능 202/200 | 200 OK | psip `RecvMessageRequest` 는 `EventMessage` 가 **반환한 상태코드**로 응답한다 — 응용이 도달 가능성을 아는 유일한 주체이므로 코드 선택도 응용이 한다. 0 을 반환하면 콜백이 직접 응답했다는 뜻이라 psip 는 보내지 않는다(최종 응답 중복 방지) |
| E2E 보안 (TS 33.180) | Protected Payload | 미적용 (TLS + 서버측 RBAC) | 서버 보관·관리자 모니터링 요구와 상충 |
| READ 통지·InReplyTo | 지원 | 미사용 (DELIVERED 만; 파서는 IE skip 지원) | 최소 프로파일 |
| disposition 통지 — 단말 | 대상 MCData ID 의 `resource-lists` + 그룹 통지면 mcdata-info `<mcdata-calling-group-id>` 를 싣고 participating PSI 로 (TS 24.282 V18.13.0 §12.2.1.1) | 코어 `sendSdsNotification` 은 계정에 MCData PSI(ue-init-config `MCData-Service-Details/Server-URI`, TS 24.484 §7.2.2.1 14))가 있으면 규격형(ue_sdk.md §4.2 «SDS disposition 통지»), 없으면 옛 형식(원 발신자 AoR 직행, SDS NOTIFICATION 한 파트). CSC 는 `UeInitConfig.ServiceDetails.McData.Enable`(기본 off)일 때만 MCData-Service-Details 를 싣는다. CSP 는 두 형식을 다 받는다 | 전환기 — CSP 0.2.180 전 서버가 PSI 통지를 상관하지 못해서다. 모든 사이트 CSP 가 0.2.180 이상이 되면 CSC 에서 MCData 를 광고하고, 단말 전환 뒤 옛 형식 수용을 걷는다 |
| disposition 집계 | 그룹 통지는 TDC1 동안 모아 한 MESSAGE 로(§12.2.3 15)c)) | 받는 대로 하나씩 중계(15)d)) | 집계는 선택 — 후속 |
| media plane SDS 의 SDP | `m=message` 단독 | **더미 `m=audio` 라인 동반** — 서버는 포트≠0(9) + `a=inactive` 로 응답/오퍼 (CMP 할당·RTP 없음). 서버발 오퍼의 더미 오디오는 **PCMU+PCMA(0 8)** 병기 | pjsua2 는 알려진 미디어가 포트≠0 으로 협상돼야 콜 유지 (`got_media` 규칙). 앱 코덱 정책이 PCMU 를 비활성(PCMA 안전망만 유지)하므로 PCMU 단독 오퍼는 자동 488 — 실기기 확인 | 
| media plane 수신 배포 | 전 수신자 INVITE+MSRP | **하이브리드** — MSRP 광고 단말만 INVITE+MSRP, 그 외는 FD FILEURL MESSAGE 폴백 (§4.5 HTTP 다운로드) | 전환기 호환 (현재 앱은 MSRP 미지원). 폴백 수신자에겐 장문이 첨부(`sds_*.txt`)로 보임 |
| 단말 a=path 포트 | 단말이 해당 포트 리슨 가능 | 광고용 (단말은 항상 out-connect, 서버 상시 `a=setup:passive`) | NAT 관통 — RTP relay 와 동일한 방향성 |
| c-plane 임계 | 서비스 설정 문서로 전파 | csp.json + CSC provisioning `mcdata.maxPayloadSdsCplaneBytes` 이중 설정 (운영자 동기) | CMS 서비스설정 문서 미구현 — provisioning 채널 재사용 |

## 8. 잔여 과제

- **임계 활성화**: 앱(송신+수신, §5 구현 완료) 배포 후 csp
  `Setup.McData.MaxPayloadSizeSdsCplaneBytes` 와 csc
  `Provisioning.McData.MaxPayloadSdsCplaneBytes` 를 함께 설정(현재 라이브 0=무제한).
  검증용 표준 단말 대역 = `tests/msrp_sds_client.py`.
- 1:1 standalone SDS over media plane (현재 그룹 대상만 — 비그룹 타겟 MSRP INVITE 는 403)
- FD over media plane (TS 24.282 §10.2.5 — cmdp 기계 동일, RFC 5547 file-selector SDP)
- MSRPS(TLS)·배포 레그 실패 시 FILEURL 재시도 정책·media-plane disposition
- Late entry(부재중 수신): 서버 보관분(§4.1 messages.jsonl) 기반 단말 pull API — 규격
  message store(IMAP)는 비실용, 자체 정의
- 멤버 단위 송신권한 — 수신전용 멤버(지금은 멤버 전원 `<mcdata-allow-transmit-data-in-this-group>` true)·멤버별 `<mcdata-max-data-in-single-request>`
- 메시지·FD 파일 retention/purge (녹취와 공통 정리 메커니즘)
- FD NOTIFICATION(다운로드 완료)·READ 통지
- **단말 SDK 의 규격형 업로드** — `CscClient::uploadFd` 는 간이형(query)으로 올린다. multipart/mixed + mcdata-info·`Location` 사용과
  Metadata 의 `file-selector:` 접두(TS 24.282 §15.2.17 · RFC 5547)는 conformance_gap_plan.md U05
- **MCData 긴급 경보**(TS 24.282 §16.2 · 애드혹 그룹 경보 §16.2A — 미지원): 그룹 문서가 `<mcdata-allow-emergency-alert>` 를 싣지 않으므로 발령은 늘 미인가다
  (§6.3.7.2.1) — CSP `CMcDataAsModule::OnEmergencyAlert` 가 **403 + mcdata-info `<alert-ind>` false**(§16.2.3.1 4)a))로 답하고 배포하지
  않는다. 취소(`<alert-ind>` false, §16.2.3.2)는 남은 MCData 경보가 없어 지울 것도 보낼 통지도 없다 → 200. 지원할 때 = 그룹 문서 요소·
  MCData user profile `<EmergencyAlert>`·발령자 제휴(§16.2.3.1 4)b)i))·경보 캐시·수신 확인(§6.3.7.1.5)

**규격에 있으나 구현하지 않은 기능** (TS 24.282 — 위 항목 밖)

| 기능 | 규격 | 지금 |
|---|---|---|
| MCData 서비스 인가 — REGISTER `<mcdata-access-token>`·MCData ID 바인딩·다중 단말·404 `141` | §7.3.2 | 없음([mcx_identity_scope.md](mcx_identity_scope.md) §10) |
| 서비스 설정 PUBLISH·구독(`Event: poc-settings`) | §7.2.2~§7.2.4 · §7.3.3~§7.3.6 | 489 (conformance_gap_plan.md S25) |
| MCData 제휴 — 서비스별 제휴 표·`mcdataPresInfo` NOTIFY·제휴 구독·암묵 제휴 | §8 | MCPTT 제휴로 읽는다 |
| SDS 세션(one-to-one·group SDS session) | §9.2.4 | 없음 |
| 애드혹 그룹 SDS(`ad-hoc-group-sds`) · functional alias 대상(300 Multiple Choices) · regroup·TGI(`<associated-group-id>`) | §9.2.2.2.1 3A) · §9.2.2.4.2 5)b)ii)·6)b)·6)l) | 없음 |
| UNDELIVERED 재전달(TD1) | §12.2.2.1 5)·6) | 통지를 중계만 한다 |
| 파일 가용 시간(TDC2 · `<default-file-availability>`·`<max-file-availability>`) · FD NETWORK NOTIFICATION(만료) | §10.2.4.4.2 9)·13)·14) · §12.4 | 없음 — 위 «retention/purge» 와 같은 자리 |
| FD HTTP 종료(FD HTTP TERMINATION) · 통신 해제 | §6.2.2.4 · §10.2.4.4.2 11)·17) · §13 | 없음 |
| 연기한 FD 목록 조회(DEFERRED DATA REQUEST·RESPONSE) | §11.3 | 없음 |
| network-stored file 업로드(`message/external-body` — MCData message store) | §10.2.2.2 | 501 (§7) |
| Enhanced Status · 위치 보고 · pre-established session · IP connectivity · MBMS/MBS 배포 | §14 · §17 · §18 · §7.2.1 4) · §9.2.6·§10.2.6 | 없음 |
