# MCPTT 서버 표준규격(3GPP TS) 정합

> **목적·전제**
>
> 안드로이드 단말(UE)을 **3GPP TS 규격대로** 구현하기로 결정함에 따라([android_ue_client.md](android_ue_client.md)),
> 서버 3개 컴포넌트(**CSC·CSP·CMP**)를 규격에 맞춰 정합시킨다. 단말(`android/ptt-client`)의 코덱·XML·
> 엔드포인트가 TS 기준 정본이며, 서버는 그와 **동일 규약**으로 동작한다. 본 문서는 컴포넌트별 **현재
> 동작(규격 정합 상태)** 을 근거 `file:line` 과 함께 정리한다.
>
> | 규격 | 범위 |
> |---|---|
> | **TS 24.380** | Media Plane Control (floor control) — **CMP** |
> | **TS 24.379** | MCPTT call control / affiliation — **CSP** |
> | **TS 33.180** | Identity Management (OIDC) / KMS — **CSC(IdMS/KMS)** |
> | **TS 24.481** | Group Management (GMS, XCAP) — **CSC(GMS)** |
> | **TS 24.484** | Configuration Management (CMS, XCAP) — **CSC(CMS)** |

---

## 0. 정합 상태 요약

| # | 항목 | 컴포넌트 | 규격 | 상태 |
|---|---|---|---|---|
| F1 | Floor 패킷 = subtype(메시지타입) + TLV | CMP | TS 24.380 §8 | ✅ 정합 |
| F2 | Reject/Revoke Cause·Floor Indicator·Duration·Queue TLV | CMP | TS 24.380 §8.3 | ✅ 정합 |
| F3 | Floor Ack / Queue Position(큐잉) | CMP | TS 24.380 §8.3.2 | ✅ 정합 |
| C1 | affiliation PUBLISH = affiliation-command XML 파싱 + Content-Type | CSP | TS 24.379 §9 | ✅ 정합 |
| C2 | affiliation-info SUBSCRIBE/NOTIFY (presence) | CSP | TS 24.379 §9.3 | ✅ 정합 |
| C3 | Resource-Priority namespace 정규화(단일값) | CSP | RFC 4412 | ✅ 정합 |
| C4 | floor SDP `m=application` + `mcptt-floor-request-uri` | CSP | TS 24.380 §12 | ✅ 정합 |
| S1 | OIDC `/.well-known/openid-configuration` 디스커버리 | CSC | TS 33.180 / OIDC | ✅ 정합 |
| S2 | access_token 표준 클레임(`sub`/`iss`/`iat`) + nonce | CSC | TS 33.180 / OIDC | ✅ 정합 |
| S3 | XCAP-diff SUBSCRIBE/NOTIFY(GMS/CMS 변경통지) | CSC/CSP | TS 24.481/484 §8 | ✅ 정합 |
| S4 | service-config 동적화(기본값 + 가입자별 override) | CSC | TS 24.484 §10.3 | ✅ 정합 |
| S5 | KMS 가입자별 키 프로비저닝 | CSC | TS 33.180 §F | ⚠ 구조적 정합(참 ECCSI/SAKKE 후속) |

> **interop 최소 조건 = F1**(+ C4) — 단말 `FloorCodec` 규약과 1:1 정합. S5 의 참값 ECCSI/SAKKE
> (RFC 6507/6508) 는 pairing 암호 라이브러리가 필요한 후속 과제로, E2E 암호화 도입 시 진행한다.

---

## 0-R. 미반영 로드맵 (규격 대비 공백)

정합(§1~§4)은 **구현된 항목**의 규격 정합을 다룬다. 여기서는 3GPP MCPTT 규격에 정의돼 있으나
CIMS 에 **아직 구현되지 않은** 기능을 규격 위치와 함께 나열한다 — 향후 과제 목록이다. 각 항목의
설계·변경점은 착수 시 별도 정본 문서로 분리한다.

> **CSP↔CMP 연동 계약**: 아래 기능을 2인(Call Control & Signaling / Media Plane & Floor)으로
> 분담하기 위한 CSP↔CMP 메시지 규격은 [mcptt_csp_cmp_roadmap_contract.md](mcptt_csp_cmp_roadmap_contract.md) 가 정본이다.

### R1. Call Control (TS 24.379) — 통화 유형/절차

| 기능 | 규격 | 상태 |
|---|---|---|
| **Private call (1:1)** — on-demand | TS 24.379 §11.1 | ✗ 미구현 (`session-type` 이 prearranged/chat/broadcast 3종만, `csp/McpttInfo.h`) |
| **Private call — pre-established session** | TS 24.379 §11.2 | ✗ |
| **Private call call-back** (요청/취소) | TS 24.379 §11.3 | ✗ |
| **Private emergency call** / 통화 중 emergency upgrade | TS 24.379 §11 | ✗ (그룹콜 emergency 는 [mcptt_emergency_modes.md](mcptt_emergency_modes.md) 로 구현) |
| **First-to-answer call** | TS 24.379 | ✗ |
| **Ambient listening call** (원격 감청) | TS 24.379 | ✗ 시그널링 미구현 (미디어 평면의 청취 leg 플래그 `recv_only`/`floor_suppress` 는 구현 — [../../api/cmp_media_api.md](../../api/cmp_media_api.md) §7.4) |
| **Remotely initiated call** (원격 개시) | TS 24.379 | ✗ |
| **User/Group regroup** (임시 그룹) | TS 24.379 + GMS(TS 24.481) | ✗ |
| **Functional alias** 활성/비활성 | TS 24.379 / TS 24.484 | ✗ |

> 구현됨: prearranged/chat/broadcast 그룹콜, affiliation(C1/C2), emergency/imminent 게이팅·선점, ad-hoc.

### R2. Floor Control (TS 24.380)

| 기능 | 규격 | 상태 |
|---|---|---|
| **Pre-established session floor** | TS 24.380 | ✗ (Call Control 파트의 세션 2단 수명과 함께 착수) |

> 구현됨: subtype+TLV 인코딩, Cause/Indicator/Duration/Queue, 큐잉, tier 선점, inactivity
> auto-revoke (F1~F3), **dual floor / multi-talker(Floor Release Multi Talker 포함) /
> private-call floor(§7) / floor SRTCP 보호(TS 33.180)** —
> 정본 [../../api/cmp_media_api.md](../../api/cmp_media_api.md) §7.7~§7.8.
>
> **단말 정합 대기**: dual/multi-talker 를 실호로 쓰려면 UE 가 Floor Indicator 의
> Multi-talker(0x0080)/Dual floor(0x0200) 비트와 Floor Release Multi Talker(subtype 0x0F)를
> 해석하고 **슬롯별 SSRC 로 오는 동시 스트림을 함께 재생**해야 한다. 현재 cspsim·Android UE 는
> 단일 화자 전제라 서버측만 CMP 프로브로 검증돼 있다
> ([../../VERIFICATION_MANUAL.md](../../VERIFICATION_MANUAL.md) 「floor 정책 시험」).
> 마찬가지로 CSP 가 `floor_policy`/`group_type:"private"` 를 아직 발행하지 않는다(Call Control 파트).

### R3. 미디어 평면 / 전송

| 기능 | 규격 | 상태 |
|---|---|---|
| **E2E 미디어 암호화** (SRTP + MIKEY-SAKKE, PCK/GMK/CSK) | TS 33.180 | ⚠ 구조만 — opensrtp 링크·SRTP 플래그 존재하나 참 ECCSI/SAKKE(RFC 6507/6508) 미구현 (S5 placeholder). **floor control(RTCP) SRTCP 보호는 구현** — 키는 제어평면 inline 전달(`floor_crypto`), 미디어는 투명 relay |
| **MBMS/멀티캐스트 베어러** 그룹 배포 | TS 23.379 | ✗ (unicast RTP relay 만) |
| **Off-network (ProSe/PC5 직접통신)** | TS 24.379 off-network | ✗ (서버 기반 on-network 만) |
| **PTT 비디오** | — | ✗ ([../modules/cmp.md](../modules/cmp.md) "향후 확장") |

### R4. 부가 서비스 / 인접 규격

| 기능 | 규격 | 상태 |
|---|---|---|
| **위치 정보 보고/관리** (Location management) | TS 23.280 / TS 24.379 | ✗ |
| **MCData MSRP relay / MSRPS(TLS)** | TS 24.282 / RFC 4976 | ✗ 후속 ([mcdata_messaging.md](mcdata_messaging.md)) |
| **MCData media plane 서비스 설정 문서** | TS 24.484 | ✗ (provisioning 채널 재사용) |
| **MCVideo** | TS 24.281 | ✗ |

### R5. 시그널링 세부 (RFC/구독) — 부분 미반영

- **NOTIFY 최종 실패(timeout/481) 시 구독 종료** (RFC 6665 MUST) — 미구현 (§C5 참조)
- **Subscription-State reason 구분** — 현재 timeout 고정
- **reg-event 다중 바인딩 / tel URI registration** — 미구현
- **ICE** (RFC 8445) — symmetric NAT 미해소 ([ue_nat_traversal.md](ue_nat_traversal.md) §9)

---

## 1. CMP — Floor Control (TS 24.380)

Floor 코덱은 `cmp/PFloorCodec.cpp` 에 분리되어 있고(단말 `ptt-client/floor/FloorCodec.kt` 와
**바이트 호환**, 단위테스트 `tests/cmp_floor_codec_test.cpp`), floor 상태머신은 `cmp/PMcpttGroup.cpp` 에 있다.

### F1. 패킷 인코딩: subtype + TLV

- **RTCP APP "MCPT"** (PT=204). 메시지 타입 = **5비트 subtype** (`BuildFloorMessage`/`ParseFloorMessage`,
  `PFloorCodec.cpp`). subtype 값은 규격 정렬: Request=0/Granted=1/Taken=2/Deny=3/Release=4/Idle=5/
  Revoke=6/QueuePosReq=8/QueuePosInfo=9/Ack=10 (`PMcpttGroup.h` `FloorOpCode`).
- **본문 = TLV**: `Field ID(8) + Length(8) + value`. 가변 길이 문자열 필드(Granted Party 4/User ID 6/
  Queued User ID 9/Track Info 11)만 32비트 경계로 패딩, 전체 패킷도 32비트 정렬.
- 수신 REQUEST 의 **Floor Priority**(필드 0)·**Floor Indicator**(필드 13)를 파싱한다(`onFloorPacket`/`onRtcpPacket`).

### F2. Cause / Indicator / Duration / Queue TLV

| 메시지 | 송신 필드(TLV) |
|---|---|
| Granted | Duration(1) + Granted Party(4) + Floor Indicator(13) (`_grantFloorTo`) |
| Taken | Granted Party(4) + Floor Indicator(13) (`broadcastFloorStatus`) |
| Deny | Reject Cause(2) — receive-only(5)/queue-full(7)/another-client(1) (`_sendDeny`) |
| Revoke | Reject Cause(2) — pre-empted(4)/other(255) |
| Queue Position Info | Queue Info(3: position+prio) + Queue Size(7) (`_sendQueuePos`) |

- Floor Indicator 는 owner tier 로 매핑: emergency→`0x1000`, imminent→`0x0800`, else normal `0x8000`
  (`_indicatorFor`). 수신 REQUEST 의 Indicator emergency/imminent 비트는 tier 로 승격된다.

### F3. Floor Ack / Queue Position(큐잉)

- **큐잉**(SDP `mc_queueing` 광고): floor 점유 중 비선점 REQUEST 는 Deny 대신 우선순위
  (tier>chair>prio>ts) 대기열에 넣고 **Queue Position Info**(subtype 9)를 회신한다. RELEASE/REVOKE/
  owner-leave 시 최우선 대기자에게 자동 grant(`_advanceFloorOrIdle`/`_popBestQueued`). 큐 포화 시 Deny(queue full).
- **Floor Queue Position Request**(subtype 8) 수신 → 현재 위치 회신.
- **Floor Ack**(subtype 10) 수신 처리(재전송 안 함 → no-op 확인).

### 보존 — 정합/유지
- 선점/tier(emergency>imminent>chair>numeric priority, TS 24.380 §8.2), SSRC 순차할당,
  inactivity auto-revoke(REVOKE cause=other + IDLE/큐 승계), DTMF(PT=101) fallback.

---

## 2. CSP — Call Control / Affiliation (TS 24.379)

근거: `csp/CscfModule.cpp`(affiliation/REGISTER/SUBSCRIBE), `csp/GroupCallService.cpp`(group call/SDP),
`csp/CspServer.cpp`(NOTIFY), `csp/McpttInfo.h`(MCPTT 본문 파서).

### C1. affiliation PUBLISH = affiliation-command XML 파싱

- **Event 헤더** `mcptt` 검증(불일치 489 Bad Event, `CscfModule.cpp` RecvRequestPublish).
- **Content-Type** 강제: 본문이 있으면 `application/vnd.3gpp.mcptt-affiliation-command+xml` 아닌 경우 415.
- **본문 파싱**: `ParseAffiliationCommand`(`McpttInfo.h`)가 `<actions>` 안의 `<affiliate>`/`<de-affiliate>`
  **액션 요소**(시작태그 앵커)와 `group` 속성을 추출 — 텍스트 substring 이 아닌 요소 기반 판정. Expires:0
  또는 de-affiliate 액션 → 해제. group 속성은 Req-URI 그룹과 교차검증.
- 보존: 멤버십 게이트(비멤버 affiliate 403), REGISTER Expires:0 시 affiliation 정리.

### C2. affiliation-info SUBSCRIBE/NOTIFY (presence)

- SUBSCRIBE 의 `Event: presence` 또는 `Accept: application/vnd.3gpp.mcptt-affiliation-info+xml` →
  이벤트 타입 `affiliation` 으로 분류(`CscfModule.cpp` RecvRequestSubscribe).
- NOTIFY: `Event: presence` + `application/vnd.3gpp.mcptt-affiliation-info+xml` 본문(가입자의 affiliated
  그룹 목록, `CspServer.cpp` `BuildAffiliationInfoBody`). 초기 NOTIFY(`SendInitialNotify`) + 상태변경 시
  PUBLISH 경로에서 `SendAffiliationNotify` 로 푸시.

### C3. Resource-Priority namespace 정규화

- INVITE 당 단일값: emergency `mcpttp.4` / imminent `mcpttp.2` / normal `mcpttp.6`
  (`GroupCallService.cpp`, RFC 4412 namespace `mcpttp`).

### C4. floor SDP 토큰

- `m=application {port} UDP MCPTT` + `c=IN IP4 ...` + `a=floorid:0 mstrm:audio` +
  `a=fmtp:MCPTT mc_queueing;mc_priority=3` + **`a=mcptt-floor-request-uri:sip:{group}@{domain}`**
  (`GroupCallService.cpp`). 단말은 floor 목적지를 이 `m=application` 포트에서 학습.
- 보존: multipart(mcptt-info+resource-lists+SDP), `urn:3gpp:ns:mcpttInfo:1.0` 등 namespace.

### C5. 등록/구독 SIP 메시지 — 실망(상용 IMS) 패킷 형태 정합

REGISTER/SUBSCRIBE/NOTIFY 의 헤더·본문을 상용 IMS 캡처 기준으로 맞춘다
(`CscfModule.cpp`, `CspServer.cpp`, psip `SipStackComm.hpp`/`SipMessage`).

- **REGISTER 401/200 OK**: `Allow`(전체 메서드 목록, `SIP_ALLOW_METHODS`) 포함. 401 에는 Contact 없음
  (psip 가 REGISTER 응답에는 Contact 자동생성 안 함). 200 OK Contact = **요청 Contact 원본 에코**
  (RFC 3261 §10.3 — URI·feature tag 보존, expires 파라미터만 부여값으로 교체). 부여값은 Contact
  `;expires` 파라미터와 `Expires` 헤더 양쪽에 동일하게 포함. 요청 Expires 는 그대로 수락(무지정 시 3600).
- **응답 공통**: Max-Forwards 는 요청 전용(RFC 3261 §8.1.1.6) — 응답에는 없음.
- **SUBSCRIBE 2xx**: `Expires` 필수(RFC 6665 §4.2.1.1 — 부여값, 해지 시 0), Allow/Supported 포함,
  Contact = user 없는 서버 자기 주소(`<sip:ip:port>`, dialog remote target). To tag 단일
  (구독 저장 tag 로 교체 — CreateResponseWithToTag 생성분 위에 중복 삽입 금지).
- **NOTIFY**: Route 헤더 없음 — NAT 뒤 단말 도달은 psip `m_strSendDestIp/m_iSendDestPort`
  전송 목적지 오버라이드(등록 바인딩 received/rport latch)로 처리. Contact = 서버 자기 주소(user 없음).
- **reg-event reginfo (RFC 3680)**: version 은 구독 내 0 부터 순증. 구독 직후 initial 은 `state="full"`
  (contact `event="registered"`), 등록 상태 변경은 `state="partial"` 로 바뀐 바인딩만 통지 —
  재등록 `refreshed`, 잔존 구독 하 신규 등록 `created`, Expires:0 해제 `unregistered`,
  sweep 만료 `expired`(모두 `SendRegEventNotify`, 종료 통지는 삭제 직전 바인딩을 expires=0 로 실음).
  `<contact>` 속성 `duration-registered`/`expires`(잔여초)/`cseq`, 등록 Contact 의 feature 파라미터는
  `<unknown-param>` 으로 나열(%XX 디코딩). `<uri>` = as-registered Contact(`CUserInfo.m_strContactUri`).
- **등록 바인딩 수명**: 재등록(REGISTER 갱신)이 `m_iLoginTime/m_iLoginTimeout` 을 리셋 — 만료 sweep 은
  마지막 재등록 기준으로만 발동.
- 미구현/향후: NOTIFY 최종 실패(타임아웃/481) 시 구독 종료(RFC 6665 MUST), 명시적 구독해지의
  Subscription-State reason 구분(현재 timeout 고정), reginfo 다중 바인딩·tel URI registration 블록.

- **conference-info NOTIFY (RFC 4575, 그룹콜 참가자)**: 로스터 변경을 `Event: conference` /
  `application/conference-info+xml` NOTIFY 로 통지한다. 경로는 멤버 단위로 갈린다 —
  **구독(SUBSCRIBE `Event: conference`)을 건 단말은 그 구독 dialog 로**(RFC 6665 정합, 단말이 200 OK),
  구독이 없는 확립 leg 는 통화 dialog in-dialog NOTIFY 폴백으로 받는다(구독 미구현 단말 호환,
  전 단말 구독 구현 후 제거). 구독 취급 규칙(갱신 시 자원·이벤트 Call-ID 승계, To tag 유지, notifier
  신원 고정, 제휴 불변)은 [ptt_flows.md](ptt_flows.md) "참가자 로스터 통지 경로"가 정본.
  통지 대상은 —
  **개시자(caller) 조인·fan-out 멤버(callee) 조인·이탈 모두** 통지한다(개시자 조인 누락 시 늦은
  발신 참여자가 기존 단말 화면에 안 뜨는 증상 방지). 본문은 **항상 `state="full"`(변경 반영 후 현재
  로스터 전체 스냅샷)** — UDP NOTIFY 유실에도 매 통지가 자가치유(증분 partial 은 유실 시 목록이
  어긋난 채 잔존). 변경 멤버는 `state`(added/deleted)+`status`(connected/disconnected)로, 나머지는
  `full`/`connected` 로 싣고, 이탈자는 로스터에서 이미 빠졌으므로 `deleted` 엔트리를 명시 부가.
  version 은 그룹별 순증. 변경 인자 없는 순수 스냅샷(구독 수락 직후 초기 NOTIFY)에는 이탈자 엔트리를
  싣지 않는다. UE(pjsip)는 구독 경로 본문을 `Account.onInstantMessage` 로,
  폴백 경로 본문을 invite usage tsx 이벤트 원문에서 읽어 같은 파서로 반영
  (→ `PttController.onConferenceInfo`).

### 보존 — 정합/유지
- Digest(username=`IMSI@domain`, MD5, qop=auth), emergency/imminent 게이팅·re-INVITE condition,
  ad-hoc/chat/broadcast/prearranged, GMS/CMS xcap-diff NOTIFY.

---

## 3. CSC — IdMS / GMS / CMS / KMS

근거: `csc/src/services/mcptt.py`(라우트 `CSC_HANDLER_LIST`).

### IdMS (TS 33.180 / OIDC)
- **S1 디스커버리**: `GET /.well-known/openid-configuration`(`handle_openid_config`) — issuer/authorization·
  token·introspection endpoint·`code_challenge_methods_supported=[S256]`·grant types·claims 광고.
- **S2a access_token 클레임**: `sub`(=user)/`iss`/`iat`/`exp`/`aud`/`scope`(`create_tokens`).
- **S2b nonce**: authreq `nonce` 저장(`handle_auth_req`) → id_token `nonce` 클레임 반영(OIDC Core §3.1.2.1).
- 보존: **PKCE S256 강제**(plain/누락 400), refresh 회전/취소.

### GMS (TS 24.481)
- 그룹문서 XML(`urn:oma:xml:poc:list-service`+`urn:3gpp:ns:mcpttGroupInfo:1.0`), ETag/If-None-Match 304,
  수평/수직 권한(403).
- **S3 변경통지**: 그룹 CRUD 시 `notify_csp("GROUP_CHANGED")`(`handlers/admin.py`) → CSP `CscInterface`
  → `SendSipNotify`(group_change) → GMS 구독자에 **xcap-diff NOTIFY**.

### CMS (TS 24.484)
- user-profile XML(`urn:3gpp:ns:mcpttUserProfile:1.0`), self-access 권한, ETag.
- **S4 service-config 동적화**: `SERVICE_CONFIG_DEFAULTS` + 가입자별 `USERS[uri]['service_config']`
  override 로 생성(`get_service_config_xml`). 내용 파생 ETag 라 값 변경 시 자동 갱신.
- **S3 변경통지**: 가입자(번호) CRUD 시 `notify_csp("USER_CHANGED")` → CSP `SendSipNotify`(user_change)
  → CMS 구독자에 xcap-diff NOTIFY(user-profile/service-config sel).

### KMS (TS 33.180 §F)
- **S5 가입자별 프로비저닝**: `KmsInit`(KMS 공개 인증서) + `KmsKeyProv`(가입자별 KmsKeySet —
  UserDecryptKey/UserSigningKeySSK/UserPubTokenPVT, `get_kms_keyprov_xml`). 키 material 은 KMS master
  secret 에서 가입자별로 파생(HKDF 유사, 재현 가능·사용자마다 상이).
- ⚠ **후속**: 파생값은 구조적 placeholder 이며 참 ECCSI/SAKKE(RFC 6507/6508) 점이 아니다. 실제
  pairing 기반 키파생은 전용 암호 라이브러리가 필요하며 E2E 암호화 도입 시 진행한다.

---

## 4. 단말 정합

각 항목은 **단말(`android/ptt-client`)과 동일 규약**으로 맞춘다(코덱·XML·엔드포인트는 단말 구현이
TS 기준). 변경 시 본 문서와 [android_ue_client.md](android_ue_client.md)·[ptt_flows.md](ptt_flows.md) 를 함께 갱신한다.

> **배포 메모**: OAM 게이트웨이 뒤 배포 시, S1 디스커버리(`/.well-known/openid-configuration`)는
> 게이트웨이 라우트로 csc 에 프록시되어야 단말이 off-box 에서 발견할 수 있다(standalone csc 는 직접 서빙).
