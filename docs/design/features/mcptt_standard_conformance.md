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
| F1 | Floor 패킷 = subtype(메시지타입) + TLV | CMP | TS 24.380 §8.1~8.2 | ✅ 정합 |
| F2 | Reject/Revoke Cause·Floor Indicator·Duration·Queue TLV | CMP | TS 24.380 §8.2.3 | ✅ 정합 |
| F3 | Floor Ack / Queue Position(큐잉) | CMP | TS 24.380 §8.2.12~8.2.13 | ✅ 정합 |
| F4 | floor 상태머신(T1/T2/T3/T7/T8/T20, pending Floor Revoke, 재요청·큐 안정성) | CMP | TS 24.380 §6.3.4 | ✅ 정합 |
| F5 | 멤버 프로파일(MCPTT ID·mc_queueing·mc_granted)·Unicast Media Flow Control·Queued Floor Requests | CMP | TS 24.380 §6.3.5, §8.2.15~8.2.16 | ✅ 정합 |
| F6 | floor SRTCP — 유니캐스트 leg 별 클라이언트 키(CSK) | CMP | TS 33.180 §9.4 / TS 24.380 §13.3.2 | ✅ 정합 (키 배포는 CSC KMS 연동 대기) |
| C1 | affiliation PUBLISH = affiliation-command XML 파싱 + Content-Type | CSP | TS 24.379 §9 | ✅ 정합 |
| C2 | affiliation-info SUBSCRIBE/NOTIFY (presence) | CSP | TS 24.379 §9.3 | ✅ 정합 |
| C3 | Resource-Priority namespace 정규화(단일값) | CSP | RFC 4412 | ✅ 정합 |
| C4 | floor SDP `m=application` + `mcptt-floor-request-uri` | CSP | TS 24.380 §12 | ✅ 정합 |
| C6 | conference 이벤트 구독 인가 — 그룹 문서 `<on-network-allow-conference-state>` 판정, 불허 403 `Warning: 138` / 브로드캐스트 480 `Warning: 105` (비멤버 관제사 청취 범위는 CIMS 해석, [dispatch_center.md §5.6](dispatch_center.md)) | CSP/CSC | TS 24.379 §10.1.3.4.1 / TS 24.481 §7.2.4.2 | ✅ 정합 |
| S1 | OIDC `/.well-known/openid-configuration` 디스커버리 | CSC | TS 33.180 / OIDC | ✅ 정합 |
| S2 | access_token 표준 클레임(`sub`/`iss`/`iat`) + nonce | CSC | TS 33.180 / OIDC | ✅ 정합 |
| S3 | XCAP-diff SUBSCRIBE/NOTIFY(GMS/CMS 변경통지) | CSC/CSP | TS 24.481/484 §8 | ✅ 정합 |
| S4 | service-config (전역 정책 SoT + 문서 산출) | CSC | TS 24.484 §10.3 | ✅ 정합 |
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
| **Ambient listening call** (원격 감청) | TS 24.379 | △ 그룹콜 청취(관제사가 `a=recvonly` 로 진행 중 그룹콜에 합류 — `allow_ambient_listening` 자격 + 관제 그룹 범위, CMP `recv_only`)는 구현([dispatch_center.md](dispatch_center.md) §5.6). 규격의 remote-init 1:1 ambient listening(`session-type=ambient-listening`, 단말 무표시 자동응답)은 미구현(§10) |
| **Remotely initiated call** (원격 개시) | TS 24.379 | ✗ |
| **User/Group regroup** (임시 그룹) | TS 24.379 + GMS(TS 24.481) | ✗ |
| **Functional alias** 활성/비활성 | TS 24.379 / TS 24.484 | ✗ |

> 구현됨: prearranged/chat/broadcast 그룹콜, affiliation(C1/C2), emergency/imminent 게이팅·선점, ad-hoc.

### R2. Floor Control (TS 24.380)

| 기능 | 규격 | 상태 |
|---|---|---|
| **Pre-established session floor** | TS 24.380 | ✗ (Call Control 파트의 세션 2단 수명과 함께 착수) |

> 구현됨: subtype+TLV 인코딩(ack 변종 포함), Cause/Indicator/Duration/Queue, 큐잉, tier 선점,
> 타이머 상태머신(T1/T2/T3/T7/T8/T20)과 pending Floor Revoke, **dual floor / multi-talker
> (Floor Release Multi Talker 포함) / 2인(private) floor / floor SRTCP 보호(그룹 키 + 멤버별 CSK)
> / Unicast Media Flow Control / Queued Floor Requests(취소)** —
> 정본 [../../api/cmp_media_api.md](../../api/cmp_media_api.md) §7.7~§7.8.
>
> **단말 정합**: 규격상 **믹싱은 단말의 media mixer 몫**이다(TS 24.380 §4.2.2, §6.2.4.3.4
> NOTE — 서버는 media distributor 로서 화자별 스트림을 SSRC 로 구분해 전달하고, 믹싱 방식은
> out of scope). Android UE 는 Floor Indicator 의 Multi-talker(0x0080)/Dual floor(0x0200) 비트와
> Floor Release Multi Talker(subtype 0x0F)를 해석하고, **슬롯별 SSRC 로 오는 동시 스트림을 병렬
> 디코드·합성 재생**하는 미디어 평면(U10)을 반영했다 — pjproject 패치로 `pjmedia_stream` 안에서
> SSRC 를 디먹스해 `get_frame` 에서 PCM 을 합산한다(정본 [mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md)).
> 이 밖의 단말 구현 항목(ack 요구 변종·Revoke 응답·Taken 신규 필드·SDP fmtp·floor SRTCP)은
> [android_ue_client.md](android_ue_client.md) §5.4 에 U1~U18 로 정리했다. 서버측은 CMP 프로브로
> 검증돼 있고([../../VERIFICATION_MANUAL.md](../../VERIFICATION_MANUAL.md) 「floor 정책 시험」),
> 단말측 dual/multi 실호는 WSL2 빌드 + 실기기 3대 검증이 남아 있다(cspsim 은 단일 화자 전제).
> CSP 는 그룹 `floor_policy`/`max_talkers` 를 발행한다(DB `ptt_groups` 원천). `group_type:"private"`
> 는 아직 발행하지 않는다(Call Control 파트).

### R2-1. Floor Control — 구현 항목의 규격 편차

구현돼 동작하지만 **TS 24.380 V17.7.0 원문과 어긋나는** 지점이다(2026-07 원문 대조). 위 R2 가
"미구현 기능"이라면 여기는 "구현됐으나 규격과 다른 동작"으로, 3rd-party 단말 interop 의 실제
장애 지점이다. 근거는 모두 TS 24.380 클라우즈.

| # | 규격 | 규격 요구 | 현재 동작 |
|---|---|---|---|
| G2 | §6.3.4.4.2-1e | **원격 개시 ambient listening** 의 Floor Granted 는 ack 요구 변종으로 보내야 한다(`shall`). 그 밖의 서버 메시지는 `may` | 서버 송신은 항상 ack 비트 0 — 도달 보장은 T20(Granted)·T7(Idle) 재송신으로 대신한다. ambient 원격 개시 여부는 CSP 가 알려주지 않는다 |

> **인용 정정**: TS 24.380 **클라우즈 7은 off-network floor control** 이다. 온넷 private call 은
> 클라우즈 6.3 의 일반 floor 절차를 그대로 쓰며, 별도 private-call floor 절차는 없다. 이전
> 문서·주석의 "TS 24.380 §7 private-call floor" 인용은 잘못된 것으로 §6.3 + fmtp
> (`mc_granted`/`mc_no_floor_ctrl`) 기준으로 대체한다.

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

### R4-1. CMS 문서함 — UE 겹 2종 미서빙 (규격 순정 단말 interop 갭)

TS 24.484 의 CMS 문서는 4겹(기기 초기/기기/사용자/시스템 + OMA 그룹)인데, CIMS 는 사용자·시스템·
그룹 3종만 서빙한다. 기기 겹 2종의 역할은 자체 `GET /provisioning/me` 가 흡수했다
([android_ue_provisioning.md](android_ue_provisioning.md) — VoLTE 병행 구성·SIP 자격 배포(ISIM 대체)·
transport 목록/선택 등 규격 문서에 없는 요구 때문). 자체 단말에는 문제가 없으나, **규격 순서대로
부트스트랩하는 외부 MCX 단말은 첫 요청(ue-init-config)에서 404** 를 만난다(고객사 단말 실측, 08-13).

| AUID | 규격 | 상태 |
|---|---|---|
| `org.3gpp.mcptt.ue-init-config` | TS 24.484 §7.2 (로그인 전 — IdMS/KMS/CMS/GMS 주소·참여 서버) | ✅ 서빙 — **§7.2.2.3 XSD 정본 스키마 그대로**(ns `urn:3gpp:mcptt:mcpttUEinitConfig:1.0`, on-network sequence 필수 요소 전부, GMS-URI=gms_psi PSI, `<anyExt>` 에 `MCPTT-Service-Details`). 익명 GET·전역 문서·내용파생 ETag. 값은 아래 **3계층** |
| `org.3gpp.mcptt.ue-config` | TS 24.484 (기기 단위 파라미터) | ✗ 미서빙 (일부 항목은 user-profile XML·provisioning/me 에 분산 — 외부 단말이 요구하면 착수) |

**확정 방침 = 병행 서빙**: 자체 단말은 `/provisioning/me`(전화+무전 병행·자격 배포 — 규격 문서에
없는 요구), 외부 규격 단말은 규격 문서함 — 각 단말은 자기가 구현한 경로만 탄다(대체 아님).
서빙 규칙: **SoT 공유**(산출이 `Provisioning.Services.*`/`IdMs.*` 를 읽는다 — 별도 상수 금지),
**익명 GET**(로그인 전 문서라 토큰이 없다 — 내용은 공개 주소뿐), base URL 은 요청 Host 에서 유도
(openid-configuration 과 동일 규칙). 외부 단말의 SIP 등록 자격 전달은 규격 밖(ISIM 몫)이라
문서함과 별개로 합의가 필요하다.

**ue-init-config 값의 3계층** — 상용은 고객사 단말 외 다른 규격 단말과도 호환돼야 하므로, 고객사
필수 요소 외 규격 요소는 사용자지정으로 관리한다(`get_ue_init_config_xml`).

| 계층 | 요소 | 출처 |
|---|---|---|
| ① 토폴로지 유도 | `domain`·PLMN(도메인 mnc/mcc)·idms-auth/token-endpoint·gms/cms/kms·GMS/CMS-XCAP-root-URI·GMS-URI(`sip:gms_psi@도메인`) | `Provisioning.Services.ptt.domain`/`IdMs.Domain` + 공개 base URL = **`McpttServer.PublicUrl`**(비면 요청 Host 유도). CSP 가 NOTIFY 로 광고하는 `xcap-root` 도 같은 값(내부 API 취득) |
| ② 규격 파라미터값 | `<name>`·Timers T100/T101/T103/T104/T132(TS 24.380 단말 floor 타이머, unsignedByte)·HPLMN PLMN 수동 지정·`*-to-con-ref`(APN/DNN)·`http-proxy`·`mutual-authentication`·`group-creation-XUI`·`integrity/confidentiality-protection-enabled` | csc `config_template.json` 섹션 **"MCS UE 초기 설정 문서"** = `UeInitConfig.*`(scope=service, `restart:false` — SIGUSR1 리로드, ETag 내용파생이라 자동 갱신). 빈 값 = 유도값/기본값 |
| ③ 확장 요소 | `<on-network><anyExt>` 의 `MCPTT-Service-Details`(기본 on, Server-URI 기본 `sip:mcptt_psi@도메인` = CSP 의 MCPTT 서버 PSI) · `MCData-Service-Details`(기본 off) — `IPv6-Required` 는 false 고정 | `UeInitConfig.ServiceDetails.{Mcptt,McData}.{Enable,ServerUri}` |

산출물은 값 `html.escape` 후 minidom well-formed 검사 — 실패하면 경고를 남기고 **마지막 정상
문서**를 계속 서빙한다(설정 실수가 부트스트랩을 끊지 않게). 자유 XML 조각 주입(ExtraXml)은 두지
않는다. 규격 사슬 회귀 = `tests/csc_bootstrap_conformance.py`, 생성기 단위시험 =
`tests/csc_idms_authreq_unit.py` §A.

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
  Revoke=6/QueuePosReq=8/QueuePosInfo=9/Ack=10/ReleaseMultiTalker=0x0F (`PMcpttGroup.h` `FloorOpCode`).
- subtype **첫 비트(0x10)=Acknowledgment 요구**(Table 8.2.2.1-1). 수신 시 비트를 걷어내 기본 타입으로
  처리하고 **Floor Ack**(Source=controlling(2) + Message Type)로 회신한다. 규격이 ack 변종을 정의하지
  않은 subtype 에 이 비트가 서 있거나 미정의 subtype 이면 §8.1.4 대로 메시지 전체를 무시한다.
- **본문 = TLV**: `Field ID(8) + Length(8) + value`. **모든 필드는 패딩을 포함해 4옥텟 배수**(§8.1.3)
  이므로 미지·가변 필드도 건너뛸 수 있다. Field ID ≥192 는 Length 가 2옥텟.
- 수신 REQUEST 의 **Floor Priority**(필드 0)·**Floor Indicator**(필드 13)를 파싱한다(`onFloorPacket`).
  단말이 floor 헤더에 쓰는 SSRC 를 학습해(`Peer.uaSsrc`) SSRC 필드에 되싣는다.

### F2. 메시지별 필드 / Cause / Indicator

서버 발신 메시지의 RTCP 헤더 SSRC 는 **floor control server 의 SSRC**(`_serverSsrc`)이고,
화자 SSRC 는 SSRC 필드(14) 또는 List of SSRCs(16)로 싣는다.

| 메시지 | 송신 필드(TLV) |
|---|---|
| Granted | Duration(1) + SSRC(14) + Floor Priority(0) + Floor Indicator(13) (`_grantFloorTo`) |
| Taken | Granted Party(4) + Permission to Request the Floor(5) + Message Seq Number(8) + Floor Indicator(13) + SSRC(14) — 동시 발언이면 SSRC 대신 List of Granted Users(15) + List of SSRCs(16) (`broadcastFloorStatus`) |
| Idle | Message Seq Number(8) + Floor Indicator(13) |
| Deny | Reject Cause(2) — receive-only(5)/queue-full(7)/another-client(1) + Floor Indicator(13) (`_sendDeny`) |
| Revoke | Reject Cause(2) — pre-empted(4)/other(255) + Floor Indicator(13) (`_sendRevoke`) |
| Release Multi Talker | SSRC(14) + User ID(6) + Floor Indicator(13) (`_sendReleaseMultiTalker`) |
| Queue Position Info | Queue Info(3: position+prio) + Floor Indicator(13) (`_sendQueuePos`) |
| Ack | Source(10)=controlling + Message Type(12)=확인 대상 subtype (`_sendFloorAck`) |

- Floor Taken 은 **화자 본인을 제외한** 참가자에게 보내고, ambient 청취(`recv_only`) leg 에는
  Permission to Request the Floor=0 변형을 보낸다. broadcast 그룹도 0 이다.
- Floor Indicator 는 owner tier 로 매핑: emergency→`0x1000`, imminent→`0x0800`, else normal `0x8000`
  (`_indicatorFor`). broadcast 그룹은 `0x4000`, multi 정책은 `0x0080`, dual 은 화자 2명일 때 `0x0200`.
  수신 REQUEST 의 Indicator emergency/imminent 비트는 tier 로 승격된다.

### F3. Floor Ack / Queue Position(큐잉) / 동시 발언 해제

- **큐잉**(SDP `mc_queueing` 광고): floor 점유 중 비선점 REQUEST 는 Deny 대신 우선순위
  (tier>chair>prio>ts) 대기열에 넣고 **Queue Position Info**(subtype 9)를 회신한다. RELEASE/REVOKE/
  owner-leave 시 최우선 대기자에게 자동 grant(`_advanceFloorOrIdle`/`_popBestQueued`). 큐 포화 시 Deny(queue full).
- **Floor Queue Position Request**(subtype 8) 수신 → 현재 위치 회신.
- **Floor Ack**(subtype 10): ack 요구 메시지에 대한 회신으로 **송신**하고, 수신은 no-op 이다
  (단말이 NAT 매핑 유지용으로 주기 송신한다).
- **Floor Release Multi Talker**(0x0F): 동시 발언 중 한 화자가 빠지면 **나머지 참가자에게 통지**
  한다(`_dropTalker` → `_sendReleaseMultiTalker`). 잔여 화자가 있으면 Floor Idle 은 보내지 않는다.

### 보존 — 정합/유지
- 선점/tier(emergency>imminent>chair>numeric priority, TS 24.380 §6.3.4), SSRC 순차할당,
  T1 만료 시 발언 종료 처리(IDLE/0x0F + 큐 승계), DTMF(PT=101) fallback.

### F4. 타이머와 회수 상태 (§6.3.4 / §11.1.3)

값은 CMP 설정(`FloorIdleSec`/`FloorStopTalkSec`/`FloorRevokeGraceSec`/`FloorRevokeRetxSec`)이
기본이고, 그룹별로 `PTT_GROUP_ADD.floor_timers` 가 덮어쓴다. 점검은 `tickFloorTimers()` 가
1초 주기로 화자마다 독립 수행한다.

| 타이머 | 기본 | 동작 |
|---|---|---|
| **T1** End of RTP media | 4초 | 마지막 RTP 후 무수신이면 **발언 완료**로 보고 회수한다 — Revoke 를 보내지 않고, 잔여 화자가 있으면 0x0F, 없으면 Floor Idle |
| **T2** Stop talking | 30초 | 첫 RTP 부터의 최대 발언시간. Floor Granted 의 Duration 으로 광고하고, 초과하면 Revoke cause **#2**(Media burst too long). 긴급/임박 tier 화자는 제외(로컬 정책) |
| **T3** Stop talking grace | 3초 | Revoke 를 보낸 뒤 Floor Release 를 기다리는 유예. 그 동안 그 화자의 미디어는 **계속 중계**되고, 유예가 끝나면 강제 회수한다. 0 이면 즉시 회수(audio cut-in) |
| **T8** Floor Revoke | 1초 | 유예 중 Floor Release 가 올 때까지 Revoke 재전송 |
| **T7** Floor Idle | 0(비활성) | 발언자가 없는 동안 Floor Idle 을 C7(3)회까지 재송신 — 무선 유실 대비, 설정으로 활성 |
| **T20** Floor Granted | 1초 | **큐에서 승급한** 화자에게 첫 RTP 가 올 때까지 Granted 를 C20(3)회까지 재송신 |

**선점**(§6.3.4.4.7)은 즉시 교체가 아니라 위 유예를 거친다: 최약 화자에게 Revoke → 요청자는
**대기열 맨 앞**에 넣고 Queue Position Info 회신 → 그 화자의 Release(또는 T3 만료) 후 승급.
`PTT_GROUP_MODIFY` 로 정원이 줄어 초과 화자를 회수할 때는 정책과 상태를 즉시 맞춰야 하므로
유예 없이 회수한다.

**재요청·큐 안정성** — 이미 발언 중인 참가자가 Floor Request 를 재전송하면 Floor Granted 를
다시 보내고(§6.3.4.4.8, Duration 은 남은 T2), 이미 대기 중인 요청의 재전송은 **큐 위치를
유지**한 채 Queue Position Info 만 재회신한다(§6.3.5.4.4-4).

### F5. 멤버 프로파일 · 부가 메시지

- **MCPTT ID**: `PTT_JOIN.user_uri` 로 받은 URI 를 User ID(6)/Granted Party(4)/리스트 필드에
  싣는다(§8.2.3.8). 없으면 sessionId(가입자 번호)로 대체한다.
- **큐잉 협상**(`PTT_JOIN.queueing`): 미협상 멤버의 비선점 요청은 큐잉하지 않고 Deny **#1**
  (§6.3.5.4.4).
- **유효 우선순위**(§6.3.5.4.4-1a): 기본값은 제어평면이 준 멤버 우선순위(default priority).
  `PTT_JOIN.max_priority`(= SDP `mc_priority` 협상값)가 있는 멤버만 요청에 실린 Floor Priority
  로 낮출 수 있고(둘 중 낮은 쪽), **미협상 멤버의 Floor Priority 필드는 무시**한다 — 관례적으로
  0 을 실어 보내는 단말의 요청을 우선순위 0 으로 해석하면 선점 서열이 무너진다.
- **초기 발언권**(`PTT_JOIN.granted` = fmtp `mc_granted`): 참가 시점에 발언자가 없으면 그
  멤버에게 Floor Granted+Taken 을 보낸다(§6.3.4.2.2-3b).
- **1인 세션**: 참가자가 한 명뿐인 세션의 요청은 Deny **#3**(Only one participant).
- **Unicast Media Flow Control**(0x0B): 멤버가 자기 하향 미디어 중단/재개를 요청한다 —
  중단 상태 멤버에게는 audio/video 를 보내지 않는다(§6.3.4.4.14~15).
- **Queued Floor Requests**(0x0E): Cancel Request(purpose 0)를 받으면 지정 사용자(List of
  Queued Users)의 대기 요청을 제거하고, 제거된 대기자에게 Cancel Notification(2),
  요청자에게 Cancel Result(1)+Result 값을 보낸 뒤 남은 대기자에게 위치를 다시 알린다.
  **목록이 없으면 요청자 본인의 요청만** 제거한다(§6.3.4.4.13 — 참가자에게 남의 대기 요청을
  지울 권한은 없다). 단말은 PTT 버튼을 뗄 때 이 목록 없는 형태로 자기 취소를 보낸다 —
  Floor Release 는 발언 중이 아닌 leg 에서 무시되므로 대기 요청을 지우지 못한다.

### F6. floor SRTCP 키 범위 (TS 33.180 §9.4)

유니캐스트 floor 는 **클라이언트별 키**(CSK 유도값)로 보호한다 — `PTT_JOIN.floor_crypto` 로
멤버마다 넣고, 그 멤버의 송·수신에만 쓴다. 그룹 단위 `PTT_GROUP_ADD.floor_crypto` 는 모든
멤버가 같은 키를 쓰는 경우(멀티캐스트/MBMS MuSiK 대응)와 멤버 키 미설정 시의 기본값이다.
수신은 **주소로 멤버를 먼저 식별한 뒤 그 멤버 키로만** 해제하고, NAT 로 주소가 바뀐 첫
패킷은 그룹 키 → 각 멤버 키 순으로 시도한다(인증 태그가 오인을 막는다). 멤버 키를 쓰는
그룹의 브로드캐스트(Taken/Idle)는 leg 마다 따로 보호한다.

### 규격 밖 수용(관대 처리) — 의도된 예외
- **Floor Ack 수신**: 서버가 ack 를 요구하지 않아도 단말이 NAT 매핑 유지용으로 주기 송신한다 —
  상태를 바꾸지 않고 수용한다([ue_nat_traversal.md](ue_nat_traversal.md)).
- **User ID 기반 주소 latch**: 소스 주소가 등록 floor 포트와 다르면 규격상 미협상 소스지만,
  제어평면이 `nat` 로 지정한 멤버에 한해 User ID(6)로 식별하고 관측 주소를 학습한다(IP guard 적용).
- 손상 TLV 는 그 지점에서 파싱을 멈추고 앞서 읽은 필드만 사용한다(메시지 폐기 대신).

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
  개시자 200 OK answer(psip `CSipDialog::AddSdp`)도 `a=fmtp:MCPTT mc_queueing` 을 광고한다.
- **역방향(멤버 SDP → CMP)**: 멤버가 광고한 `a=fmtp:MCPTT` 는 CSP 가 파싱해 `PTT_JOIN` 의
  `queueing`/`max_priority`/`granted` 로 전달한다 (U14 서버 절반 —
  [../modules/csp.md](../modules/csp.md) 「멤버별 floor 협상 전달」).
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
- **인증 요청 두 말투 병행**(`handle_auth_req` 한 핸들러 안 분기 — 검증·인증·코드 발급은 공유, 응답 표현만 다름):
  - *자체 단말 간이형*: `GET /idms/authreq?user_name&user_password&…` → `200 JSON {code,state,Location}`.
    규격 폼 왕복을 생략한 CIMS 앱·cspsim 경로(변경 없음).
  - *규격 흐름*(TS 24.482 §6.3.1 / OIDC Core §3.1.2): 자격 없는 `GET`(OIDC Authentication Request) →
    `200 text/html` 로그인 폼 → `POST`(form-urlencoded: 입력칸 + hidden 문맥) → **`302 Location:
    redirect_uri?code&state`** → `POST /idms/tokenreq`(form-urlencoded) → JSON. 폼은 **무상태**(서버
    세션 없음 — client_id·redirect_uri·state·scope·nonce·code_challenge(+method)·response_type 을
    hidden input 으로 이월). 인증 실패 = 폼 재표시+오류(200). 입력칸 이름 = `IdMs.FormLoginField`/
    `IdMs.FormPasswordField`(기본 `username`/`password` — 외부 SDK 의 헤드리스 폼 자동화가 찾는 이름,
    벤더 설정과 맞춘다). 폼 `action` 은 요청 Host 유도 절대 URL.
  - 공통 검증: PKCE S256 필수, `response_type` 은 있으면 `code`, 미지 scope 비거절,
    **`redirect_uri` 허용목록 `IdMs.RedirectUriAllow`**(비면 전부 허용 — 상용 전 등록·활성, 정확 일치
    RFC 6749 §3.1.2.3, 위반 400). 폼 경로는 redirect_uri 필수(302 목적지), 간이형은 선택(종전 호환).
  - 회귀: 규격 사슬 `tests/csc_bootstrap_conformance.py` Step 3(간이형)·3b(규격), 오프라인 단위
    `tests/csc_idms_authreq_unit.py` §B.
- 보존: **PKCE S256 강제**(plain/누락 400), refresh 회전/취소.

### GMS (TS 24.481)
- 그룹문서 XML(`urn:oma:xml:poc:list-service`+`urn:3gpp:ns:mcpttGroupInfo:1.0`), ETag/If-None-Match 304,
  수평/수직 권한(403).
- **S3 변경통지**: 그룹 CRUD 시 `notify_csp("GROUP_CHANGED")`(`handlers/admin.py`) → CSP `CscInterface`
  → `SendSipNotify`(group_change) → GMS 구독자에 **xcap-diff NOTIFY**.

### CMS (TS 24.484)
- user-profile XML(ns = 규격 §8.3.2.4 정본 `urn:3gpp:mcptt:user-profile:1.0`), self-access 권한(신원 표기 tel:/sip:/sip:@도메인 관용), ETag.
- **S4 service-config**: 값의 SoT 는 DB `mcptt_service_config` **단일 행**(id=1)이다. 기동 시
  `load_shared_data` 가 `SERVICE_CONFIG` 캐시로 읽고, `get_service_config_xml` 이 그 캐시를 XML 로
  산출한다(내용 파생 ETag — 값이 바뀌면 자동 갱신). 편집은 관리 API
  `GET/PUT /api/v1/mcptt/service-config`(monitor 조회 / manager 변경)와 콘솔 **구성 > MCPTT 정책**
  이며, PUT 이 DB UPSERT + 캐시 갱신을 함께 하므로 다음 XCAP GET 이 곧 새 값이다.
  service-config 은 **시스템 전역 문서 1건**이라 가입자별 오버라이드를 두지 않는다 — 사용자 단위
  인가는 `user-profile` 의 `ruleset` 이 규격 자리이고, 단말이 두 축을 AND 로 게이트한다.
  전역 변경은 CSC 가 `SERVICE_CONFIG_CHANGED` 를 발행하고 CSP 가 cms 구독자 **전원**에게
  xcap-diff NOTIFY 를 push 한다(`GetSubscriptionsByEvent("cms")` — 전역 문서라 사용자/자원 키가
  없는 유일한 전체 조회). 구독이 없는 단말은 목록 갱신·재로그인 계기의 재조회로 반영된다.
- **S3 변경통지**: 가입자(번호) CRUD 시 `notify_csp("USER_CHANGED")` → CSP `SendSipNotify`(user_change)
  → CMS 구독자에 xcap-diff NOTIFY(user-profile/service-config sel).
- **단말 소비**: PTT 단말은 `sip:cms_psi@<domain>` 으로 cms 축을 구독하고 NOTIFY 의 sel 대로 두 문서를
  `If-None-Match` 재조회한 뒤, 사용자별 인가(`user-profile` 의 `ruleset`)와 시스템 정책
  (`service-config`)을 **AND** 로 게이트한다(발신·개시만, 착신은 서버 판정). 소비 지점 표는
  [android_ue_client.md §7](android_ue_client.md) "CMS 문서 소비".

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
