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

### 보존 — 정합/유지
- Digest(username=`IMSI@domain`, MD5, qop=auth), emergency/imminent 게이팅·re-INVITE condition,
  conference-info NOTIFY(RFC 4575), ad-hoc/chat/broadcast/prearranged, GMS/CMS xcap-diff NOTIFY.

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
