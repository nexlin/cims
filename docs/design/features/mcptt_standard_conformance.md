# MCPTT 서버 표준규격(3GPP TS) 정합 보완 사항

> **목적·전제**
>
> 안드로이드 단말(UE)을 **3GPP TS 규격대로** 구현하기로 결정함에 따라([android_ue_client.md](android_ue_client.md)),
> 서버 3개 컴포넌트(**CSC·CSP·CMP**)를 규격에 맞춰 보완해야 단말과 상호운용(interop)된다.
> 현재 서버는 동작하는 **simplified/custom** 구현이 많아 규격과 어긋난다. 본 문서는 컴포넌트별
> **현재 동작 → 규격 → 보완 → interop 영향**을 정리한다(근거 `file:line`).
>
> | 규격 | 범위 |
> |---|---|
> | **TS 24.380** | Media Plane Control (floor control) — **CMP** |
> | **TS 24.379** | MCPTT call control / affiliation — **CSP** |
> | **TS 33.180** | Identity Management (OIDC) / KMS — **CSC(IdMS/KMS)** |
> | **TS 24.481** | Group Management (GMS, XCAP) — **CSC(GMS)** |
> | **TS 24.484** | Configuration Management (CMS, XCAP) — **CSC(CMS)** |

---

## 0. 우선순위 요약 (interop 영향 기준)

| # | 항목 | 컴포넌트 | 규격 | 영향 | 우선순위 |
|---|---|---|---|---|---|
| F1 | Floor 패킷 = subtype(메시지타입)+TLV 필드로 전환 | CMP | TS 24.380 §8 | **floor 전면 불통** | **P0** |
| F2 | Reject/Revoke Cause·Floor Indicator·Duration·Queue 를 TLV 로 송신 | CMP | TS 24.380 §8.3 | 거부사유/긴급/큐 미표시 | P1 |
| F3 | Floor Ack / Queue Position 메시지 | CMP | TS 24.380 §8.3.2 | 신뢰성/큐잉 미동작 | P2 |
| C1 | affiliation PUBLISH = affiliation-command **XML 파싱** | CSP | TS 24.379 §9 | de-affiliate 오판 가능 | P1 |
| C2 | affiliation-info **SUBSCRIBE/NOTIFY** 제공 | CSP | TS 24.379 §9 | 단말이 제휴상태 못 받음 | P1 |
| C3 | Resource-Priority namespace 정규화 | CSP | RFC 4412 | 우선순위 무시 가능 | P2 |
| C4 | floor SDP(`m=application` proto/fmtp) 토큰 정규화 | CSP | TS 24.380 §12 | floor 포트 협상 모호 | P1 |
| S1 | OIDC `/.well-known/openid-configuration` 디스커버리 | CSC | TS 33.180 / OIDC | 단말 엔드포인트 하드코딩 | P2 |
| S2 | access_token 클레임(`sub`/`iss`/`iat`)·nonce | CSC | TS 33.180 | 토큰 검증 약함 | P2 |
| S3 | XCAP-diff **SUBSCRIBE/NOTIFY**(GMS/CMS 변경통지) | CSC/CSP | TS 24.481/484 §8 | 설정변경 실시간 반영 불가 | P1 |
| S4 | service-config 동적화(현재 하드코딩 stub) | CSC | TS 24.484 §10.3 | 사용자별 정책 미반영 | P2 |
| S5 | KMS MIKEY-SAKKE 실제 구현(현재 mock) | CSC | TS 33.180 §F | E2E 암호화 불가 | P3(후속) |

> **interop 최소 조건 = F1**(+ C4). 나머지는 규격 완성도/고급기능. F1 없이는 단말 floor 가 전혀 동작하지 않는다.

---

## 1. CMP — Floor Control (TS 24.380)

CMP 의 floor 는 `cmp/PMcpttGroup.{h,cpp}` 의 `FloorControlPacket` 구조로, **자체 주석에 "simplified
structure for this implementation"** 이라 명시된 custom 포맷이다.

### F1. 패킷 인코딩: opcode 바이트 → subtype + TLV (**P0**)

| | 현재(CMP) | 규격(TS 24.380 §8) |
|---|---|---|
| 메시지 타입 | RTCP APP **subtype=0 고정**, 본문 첫 바이트 `opcode`(1~7) (`PMcpttGroup.cpp:61,66`) | **subtype(5비트)=메시지타입** (Request=0/Granted=1/Taken=2/Deny=3/Release=4/Idle=5/Revoke=6/QueuePos=8,9/Ack=10) |
| 본문 | `opcode(1)+id_len(1)+reserved(2)+speaker_id(가변,4B정렬)` (`PMcpttGroup.h:43-53`) | **`Field ID(8)+Length(8)+value` TLV** 나열 (가변 문자열 필드만 4B 정렬) |
| 화자 식별 | `speaker_id` raw ASCII (`BuildFloorPacket` `cpp:51-72`) | **User ID(필드 6)** / **Granted Party's Identity(필드 4)** TLV |

- **현재 opcode 값도 규격과 불일치**: CMP `FLOOR_GRANT=2` vs 규격 Granted=subtype 1, CMP `FLOOR_TAKEN=6` vs 규격 Taken=2 등 (`PMcpttGroup.h:22-30`).
- **보완**: `BuildFloorPacket`/`onFloorPacket` 을 subtype 기반 디스패치 + TLV 인코더/디코더로 교체. 단말 코덱(`ptt-client/floor/FloorCodec.kt`)과 동일 규약.
- **영향**: 이 항목 미적용 시 단말의 Request(subtype 0 + Priority/User ID TLV)를 CMP 가 `opcode=0`(미정의)로 해석 → **floor 전면 불통**.

### F2. Cause / Indicator / Duration / Queue 를 TLV 로 송신 (P1)

| 필드 | 현재 | 규격 |
|---|---|---|
| Reject Cause | 패킷에 없음, JSON 로그만(`cpp:476,601`) | **Reject Cause 필드(2)** 코드값 (1~7,255) |
| Revoke Cause | 패킷에 없음, 로그만(`cpp:672`) | **Revoke Cause** 코드값 |
| Floor Indicator(emergency/imminent) | tier 는 CMP 내부 상태·로그만(`cpp:581`) | **Floor Indicator 필드(13)** 비트마스크(emergency 0x1000/imminent 0x0800…) |
| Duration | 패킷에 없음(녹취 메타만, `PSyncRtpRecorder.cpp:222`) | Granted 에 **Duration 필드(1)** |
| Queue Position/Size | 큐 자체 없음 | **Queue Info(3)/Queue Size(7)** |

- **보완**: Granted/Deny/Revoke/Taken 송신 시 해당 TLV 필드 추가. tier→Floor Indicator 매핑.
- **영향**: 단말이 거부사유/긴급표시/잔여시간/큐위치를 못 받음(UX 저하). floor 기본 동작은 F1 만으로 가능.

### F3. Floor Ack / Queue Position 메시지 (P2)

- 현재: Ack 없음, 큐 없음(단일 점유 아니면 즉시 Reject, `cpp:591-610`).
- 규격: Floor Ack(subtype 10), Floor Queue Position Request/Info(8/9).
- **보완**: Ack 수신처리(신뢰성), 우선순위 대기열 도입 시 Queue Position 송신.

### 보존(규격 정합·유지) — 재구현 불필요
- **선점/tier**: emergency>imminent>chair>numeric priority 선점 로직 구현됨(`cpp:526-541`) — TS 24.380 §8.2 정합. F1 의 TLV 화만 얹으면 됨.
- SSRC 순차할당(`cpp:111-115`), inactivity auto-revoke(`cpp:642-693`), DTMF(PT=101) fallback(`cpp:382-414`).

---

## 2. CSP — Call Control / Affiliation (TS 24.379)

근거: `csp/CscfModule.cpp`(affiliation/REGISTER), `csp/GroupCallService.cpp`(group call/SDP),
`csp/ModuleDispatcher.cpp`·`csp/McpttInfo.*`(multipart 파싱).

### C1. affiliation PUBLISH = affiliation-command XML 파싱 (P1)

| | 현재 | 규격(TS 24.379 §9) |
|---|---|---|
| 판정 | `Expires:0` → de-affiliate, 아니면 body 에 `"deaffiliate"`/`"de-affiliate"` substring 검색 (`CscfModule.cpp:468-474`) | `application/vnd.3gpp.mcptt-affiliation-command+xml` **XML 파싱**(affiliate/de-affiliate 액션·대상 그룹) |
| Content-Type | 기대만 하고 미강제(경고 후 진행) (`:456-461`) | 정확 일치 검증 |

- **보완**: affiliation-command XML 파서 도입(액션·group 목록). 단말은 `<affiliate group="…"/>`/`<de-affiliate …/>`(`McpttXml.affiliationCommand`)를 보냄.
- **interop 메모**: 단말이 de-affiliate 시 **Expires:0 동반**하면 현재 휴리스틱과도 호환되나, 규격 파싱이 정본.
- 보존: 멤버십 게이트(비멤버 affiliate 403, `:476-495`), REGISTER Expires:0 시 사용자 affiliation 정리(`:251-264`).

### C2. affiliation-info SUBSCRIBE/NOTIFY (P1)

- 현재: affiliation 상태를 **NOTIFY 로 노출하지 않음**(SUBSCRIBE 는 conference 로만 분류, `:354-369`).
- 규격: `application/vnd.3gpp.mcptt-affiliation-info+xml` 구독 → 상태변경 NOTIFY.
- **보완**: affiliation-info 이벤트 패키지 + NOTIFY 본문. 단말이 자신/타 사용자 제휴상태 추적.

### C3. Resource-Priority namespace 정규화 (P2)

- 현재: `Resource-Priority: mcptt.4`(emergency)/`mcptt.2`(imminent) (`GroupCallService.cpp:653-665`) — namespace 토큰이 비표준.
- 규격: RFC 4412 등록 namespace 형식.
- **보완**: 등록 namespace 로 교정(단말/서버 합의값). 우선순위 미해석 방지.

### C4. floor SDP 토큰 정규화 (P1)

- 현재: `m=application {port} UDP MCPTT` + `a=floorid:0 mstrm:audio` + `a=fmtp:MCPTT mc_queueing;mc_priority=3` (`GroupCallService.cpp:1307-1314`). proto/fmtp 토큰 표기 점검 필요.
- 규격: TS 24.380 §12(SDP) — `m=application` proto·`fmtp` 파라미터 규격 토큰.
- **보완**: proto/fmtp 토큰을 규격값으로 통일하고 단말 SDP 파서와 정합. **단말은 floor 목적지를 이 `m=application` 포트에서 학습**(RTP+1 금지)하므로 포트·c-line 정확성 필수.
- 보존: multipart(mcptt-info+resource-lists+SDP) 구성·namespace 는 규격 정합(`:1183-1233`, `urn:3gpp:ns:mcpttInfo:1.0` 등) — 단말 빌더/파서와 일치 확인됨.

### 보존 — 정합/유지
- Digest(username=`IMSI@domain`, MD5, qop=auth) (`CscfModule.cpp:37-61,130-144`) — 단말 `SipController` 와 정합.
- emergency/imminent 게이팅·re-INVITE condition(`GroupCallService.cpp:87-96`, `:289`), conference-info NOTIFY(RFC 4575), ad-hoc/chat/broadcast/prearranged, GMS/CMS xcap-diff NOTIFY(`CspServer.cpp:568-632`).
- **alert-ind**: 파싱하나 SIP 신호로 미사용(`McpttInfo.h`) — 규격화 시 보완(P2).

---

## 3. CSC — IdMS / GMS / CMS / KMS

근거: `csc/src/services/mcptt.py`(라우트 `:1284-1297`).

### IdMS (TS 33.180 / OIDC)
| # | 현재 | 규격 | 보완 | P |
|---|---|---|---|---|
| S2a | access_token 클레임 `mcptt_id/aud/exp/scope` 만(`:552-558`) — `sub/iss/iat` 없음 | OIDC 표준 클레임 | access_token 클레임 보강 | P2 |
| S2b | `nonce` 미처리(authreq→id_token 전달 안 됨) | OIDC nonce(CSRF) | nonce 저장·id_token 반영 | P2 |
| S1 | `/.well-known/openid-configuration` **없음** | OIDC Discovery | 디스커버리 엔드포인트 제공 | P2 |
| — | token endpoint client 인증 없음(client_id 문자열만, `:913-916`) | client_secret/assertion | 클라이언트 인증 도입 | P2 |
| 보존 | **PKCE S256 강제**(plain 거부, `:838-847`), refresh 회전/취소(`:984-1030`) — 단말 `Pkce`/`CscClient` 와 정합 | | | |

### GMS (TS 24.481)
| # | 현재 | 규격 | 보완 | P |
|---|---|---|---|---|
| S3 | XCAP-diff **SUBSCRIBE/NOTIFY 없음**(서버측 미구현) | TS 24.481 §8.3 | 변경통지 구독/NOTIFY | P1 |
| — | 그룹 **목록=JSON**, 문서=XML(혼용, `:1049-1062` vs `:1112-1117`) | XCAP 문서 일관 | 목록도 XCAP/XML 검토 | P2 |
| 보존 | 그룹문서 XML(`urn:oma:xml:poc:list-service`+`urn:3gpp:ns:mcpttGroupInfo:1.0`, `:619-689`), ETag/If-None-Match 304(`:1114-1117`), 수평/수직 권한(`:1076-1111`) — 단말 `McpttXml.parseGroupDoc`/`CscClient` 와 정합 | | | |

### CMS (TS 24.484)
| # | 현재 | 규격 | 보완 | P |
|---|---|---|---|---|
| S4 | service-config **하드코딩 stub**(고정값 반환, 저장·수정 없음, `:723-739`) | 사용자/시스템별 설정 문서 | DB 연동·동적 생성 | P2 |
| 보존 | user-profile XML(`urn:3gpp:ns:mcpttUserProfile:1.0`, `:691-721`), self-access 권한, ETag | | | |

### KMS (TS 33.180 §F)
| # | 현재 | 규격 | 보완 | P |
|---|---|---|---|---|
| S5 | **mock/stub**(고정 crypto material, `:741-798`) | MIKEY-SAKKE 키관리 | 실제 키파생/프로비저닝(E2E 암호화 도입 시) | P3(후속) |

---

## 4. 권장 작업 순서

1. **F1 (CMP floor subtype+TLV)** — interop 최소 조건. 단말 `FloorCodec` 규약과 1:1 정합. **최우선.**
2. **C4 (floor SDP 토큰) + C1 (affiliation XML)** — 그룹콜 키업·제휴 정상화.
3. **F2 (Cause/Indicator/Duration TLV)** — floor UX 완성.
4. **C2/S3 (affiliation-info·xcap-diff NOTIFY)** — 상태/설정 실시간 반영(단말 M3).
5. **S1/S2/S4 (OIDC 디스커버리·클레임·service-config)** — 규격 완성도.
6. **F3/S5 (Queue/Ack·KMS)** — 고급/후속.

> 각 항목은 **단말(`android/ptt-client`)과 동일 규약**으로 맞춘다(코덱·XML·엔드포인트는 단말 구현이 이미 TS 기준). 변경 시 본 문서와 [android_ue_client.md](android_ue_client.md)·[ptt_flows.md](ptt_flows.md) 를 함께 갱신한다.
