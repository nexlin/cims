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
>
> **보완 목록** — 규격 원문(R19)과 코드를 절차 단계별로 다시 대조해 나온 미정합 지점은
> [../../dev/mcptt_conformance_gaps.md](../../dev/mcptt_conformance_gaps.md) 에 모았다. 아래 «✅ 정합» 서술 가운데 그 목록 §5 가
> 짚은 곳(C1·C4·C4g·C4h·F1·F2·F4·F5·S3·S4)은 실제와 다르다 — 항목이 반영되면 이 문서를 고치고 목록에서 지운다.

---

## 0. 정합 상태 요약

| # | 항목 | 컴포넌트 | 규격 | 상태 |
|---|---|---|---|---|
| F1 | Floor 패킷 = subtype(메시지타입) + TLV | CMP | TS 24.380 §8.1~8.2 | ✅ 정합 |
| F2 | Reject/Revoke Cause·Floor Indicator·Duration·Queue TLV | CMP | TS 24.380 §8.2.3 | ✅ 정합 |
| F3 | Floor Ack / Queue Position(큐잉) | CMP | TS 24.380 §8.2.12~8.2.13 | ✅ 정합 |
| F4 | floor 상태머신(T1/T2/T3/T7/T8/T20, pending Floor Revoke, 재요청·큐 안정성) | CMP | TS 24.380 §6.3.4 | ✅ 정합 |
| F5 | 멤버 프로파일(MCPTT ID·mc_queueing·초기 발언권)·Unicast Media Flow Control·Queued Floor Requests | CMP | TS 24.380 §6.3.5, §8.2.15~8.2.16 | ✅ 정합 |
| F6 | floor SRTCP — 유니캐스트 leg 별 클라이언트 키(CSK) | CMP | TS 33.180 §9.4 / TS 24.380 §13.3.2 | ✅ 정합 (키 배포는 CSC KMS 연동 대기) |
| C1 | affiliation PUBLISH — 규격형(Event: presence + pidf 집합 교체, 만료 없음) + 구형(Event: mcptt + affiliation-command) 양립 | CSP·SDK | TS 24.379 §9.2.1.2, §9.2.2.2.3, §9.3.1.2 | ✅ 정합 (SDK 규격형 — 구형은 PSI 없는 단말의 전환기 한시) |
| C2 | 제휴 상태 SUBSCRIBE/NOTIFY (presence, pidf) | CSP | TS 24.379 §9.2.2.2.4·§9.2.2.2.5·§9.3.1 | ✅ 정합 (편차 C2 참조) |
| C3 | Resource-Priority namespace 정규화(단일값) | CSP | RFC 4412 | ✅ 정합 |
| C4 | floor SDP `m=application` + `mcptt-floor-request-uri` | CSP | TS 24.380 §12 | ✅ 정합 |
| C4b~C4e | 멤버 leg INVITE·개시자 응답(Contact·PAI·Warning 전달)·확인 통화 설정(TNG1·최소 인원·미응답 멤버 INFO)·세션 식별자 GRUU·Warning 형식 | CSP | TS 24.379 §4.4·§4.5·§6.3.3.1.2·§6.3.3.2.3.2·§6.3.3.3 | ✅ 정합 (Supported norefersub/explicitsub 미광고 — §C4g 남은 편차) |
| C4f | 멤버 확인 전 수락 `P-Answer-State: Unconfirmed` + 미디어 버퍼링(CMP) · 멤버 183 Unconfirmed · 신뢰성 18x PRACK | CSP/CMP | TS 24.379 §10.1.1.4.2·§11.1.1.4.2 / RFC 4964·3262 | ✅ 정합 |
| C4g | 개시·합류·재합류 affiliation 검사 403 `120` · 긴급/임박·chat 암묵적 affiliation | CSP | TS 24.379 §10.1.1.4.2 14)a)·§10.1.1.4.5.1 8)·§9.2.2.3.7 | ✅ 정합 (`require_affiliation` 그룹) |
| C4h | 개시 INVITE 대상 — Request-URI = 참여 기능 PSI + mcptt-info `<mcptt-request-uri>`(그룹콜·개별 통화), Request-URI 에 대상을 직접 싣는 구형 단말 양립 | CSP | TS 24.379 §10.1.1.2.1.1 1)·2) · §11.1.1.2.1.1 | ✅ 정합 |
| C6 | conference 이벤트 구독 인가 — 그룹 문서 `<on-network-allow-conference-state>` 판정, 불허 403 `Warning: 138` / 일제 통화 480 `Warning: 105` (비멤버 관제사 청취 범위는 CIMS 해석, [dispatch_center.md §5.6](dispatch_center.md)) | CSP/CSC | TS 24.379 §10.1.3.4.1 / TS 24.481 §7.2.4.2 | ✅ 정합 |
| C7 | broadcast group call 발언권 — 개시자 외 Floor Request Deny #5(긴급 포함)·Floor Taken Permission 0·Floor Indicator B-bit | CMP | TS 24.380 §6.3.5.3.4·§6.3.5.4.4·§8.2.3.15 | ✅ 정합 |
| C8 | broadcast group call 호 모델 — 호 단위 `<broadcast-ind>` 개시, 개시자 고정, 그룹 문서 그룹 종류(`on-network-invite-members`), 해제 정책(T4·참가자 1명 이하·TNG3) | CSP/CSC | TS 24.379 §4.12·§6.2.8.2·§6.3.8.1 / TS 24.481 §7.2.8 | ✅ 정합(서버) — 개시 단말의 발언 종료 후 호 해제(TS 24.380 §6.2.4.6.4)·B-bit Floor Request 는 단말 몫(미구현). 정본 [mcptt_broadcast_group_call.md](mcptt_broadcast_group_call.md) |
| C9 | 설정 그룹 암시적 제휴 — user profile `<ImplicitAffiliations>` = 멤버별 `implicit_affiliation` 설정, PTT 서비스 인가(REGISTER) 때 참여 기능이 제휴 기록 · ad hoc 초대 = 제휴 | CSP/CSC | TS 24.379 §7.3.3 13)·§7.3.4 13) · §9.2.2.2.15 · §17.4.2.2 16) · TS 24.484 §8.3.2 | ✅ 정합 (편차 C9 참조) |
| S1 | OIDC `/.well-known/openid-configuration` 디스커버리 | CSC | TS 33.180 / OIDC | ✅ 정합 |
| S2 | access_token 클레임(`sub`/`iss`/`iat`/`client_id`/`scope` 문자열 + `mcptt_id`/`mcdata_id`) + nonce, scope 카탈로그 `3gpp:mc:*`(B.4.2.2) 요청∩카탈로그 발급, 리소스 서버 scope 검사(B.10, `IdMs.ScopeEnforcement`) — 구 `3gpp:mcptt:ptt_server` 전환기 별칭 | CSC | TS 33.180 Annex B | ✅ 정합 — 정본 [mcx_identity_scope.md](mcx_identity_scope.md) |
| S3 | XCAP-diff SUBSCRIBE/NOTIFY(GMS/CMS 변경통지) | CSC/CSP | TS 24.481/484 §8 | ✅ 정합 |
| S4 | service-config (전역 정책 SoT + 문서 산출 — 전역 문서 `…/global/service-config.xml`) | CSC | TS 24.484 §8.4 | ✅ 정합 |
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
| **Private call (1:1)** — on-demand | TS 24.379 §11.1 | ✓ 구현 — mcptt-info `session-type=private` INVITE(상대 MCPTT ID 직접, affiliation 불요) → CSP 가 2인 세션(`private:<from>-<to>`)을 CMP `PTT_GROUP_ADD group_type:"private"` + `floor_control`(on/off — fmtp `mc_no_floor_ctrl`)로 세운다([mcptt_csp_cmp_roadmap_contract.md](mcptt_csp_cmp_roadmap_contract.md) §A.1). 착신 미등록 480 |
| **Broadcast group call** — 호 단위 개시(`<broadcast-ind>`)·개시자 고정·개시자 발언 종료 후 호 해제 | TS 24.379 §4.12·§6.2.8.2, TS 24.380 §6.2.4.6.4 | ✓ 서버(C7·C8)·단말 코어(SDK·Android PTT — 일제 통화 발신·B-bit Floor Request·발언 종료 후 호 해제)·관제 앱 Windows(U6)·Android 태블릿(코드 반영, 빌드 미확인 — [mcptt_broadcast_group_call.md](mcptt_broadcast_group_call.md) §7) |
| **Broadcast adhoc group call** — 애드혹(ad hoc) 호에 `<broadcast-ind>` | TS 24.379 §17.2.2.1.1 9)·§17.1 | ✓ 단말(SDK)·관제 앱·CSP(`IsBroadcastCapable` — 개시자 고정·Deny #5·B-bit·구독 480/105·개시자 이탈 해제, [mcptt_broadcast_group_call.md](mcptt_broadcast_group_call.md) R13). ad hoc 그룹 ID 서버 부여(§17.1)는 남음 |
| **세션 해제 정책** — 그룹 호(T4(Inactivity) 만료·참가자 1명 이하·TNG3) · 개별 호(T4·최대 통화 시간) | TS 24.379 §6.3.8.1·§6.3.8.2 / TS 24.380 §6.3.4.3.5 | ✓ 편성·일제 그룹 호 — T4 = 그룹 `hang_timer_sec`(CMP `PTT_FLOOR_INACTIVITY`)·TNG3 = `max_duration_sec` · 애드혹 그룹 호 — T4·TNG3 = service configuration `<adhoc-group-call>`(일제면 `<broadcast-hang-time>`, 긴급·임박 개시 호는 TNG3 없음 — §17.4.2.2 13)) · 개별 호 — T4(발언권 제어 있는 호)·최대 통화 시간 = `<private-call>`. 정본 [mcptt_timers.md](mcptt_timers.md). 최소 affiliation 인원 미달 해제는 미구현 |
| **Private call — pre-established session** | TS 24.379 §11.2 | ✗ |
| **Private call call-back** (요청/취소) | TS 24.379 §11.3 | ✗ |
| **Private emergency call** / 통화 중 emergency upgrade | TS 24.379 §11 | ✓ 개시 인가 구현 — 사용자 프로파일 `allow-emergency-private-call` + `MCPTTPrivateRecipient`(UsePreConfigured 모드는 사전 지정 수신자 일치까지, `IsConditionInitAuthorized` private 분기). 그룹콜 emergency 는 [mcptt_emergency_modes.md](mcptt_emergency_modes.md) |
| **First-to-answer call** | TS 24.379 | ✗ |
| **Ambient listening call** (원격 감청) | TS 24.379 | △ 그룹콜 청취(관제사가 `a=recvonly` 로 진행 중 그룹콜에 합류 — `allow_ambient_listening` 자격 + 역할 `ptt_listen` 범위, CMP `recv_only`)는 구현([dispatch_center.md](dispatch_center.md) §5.6). 규격의 remote-init 1:1 ambient listening(`session-type=ambient-listening`, 단말 무표시 자동응답)은 미구현(§10) |
| **Remotely initiated call** (원격 개시) | TS 24.379 | ✗ |
| **User/Group regroup** (임시 그룹) | TS 24.379 + GMS(TS 24.481) | ✗ |
| **Functional alias** 활성/비활성 | TS 24.379 / TS 24.484 | ✗ |
| **서비스 설정** PUBLISH·구독(`Event: poc-settings` — Answer-Mode·선택한 user profile) | TS 24.379 §7.2.2~§7.2.4 · §7.3.3~§7.3.6 | ✗ — 489 (conformance_gap_plan.md S25) |
| **협상 모드 제휴 변경**(타인 제휴 MESSAGE) · **규칙 기반 제휴** | TS 24.379 §9.2.1.4·§9.2.1.5 · §9.2.1.7 | ✗ |
| **그룹 동적 데이터 구독**(그룹 상태·호 진행·제휴 멤버) | TS 24.379 §9.2.1.6 · §9.2.2.3.9~10 | ✗ — 그 구독을 제휴 구독으로 받는다(mcptt_conformance_gaps.md AFF-10) |
| **XML 기밀성·무결성 보호**(mcptt-info 요소 암호화·서명) | TS 24.379 §4.8 · §6.6 | ✗ — service configuration 이 «꺼짐»(false)을 알린다 |
| **우선순위 공유** · **MCPTT gateway server** | TS 24.379 §6.7 · §5.5·§6.8 | ✗ |
| **호 없는 임박 위험 상태 해제**(MESSAGE) | TS 24.379 §10.1.6 | ✗ |
| **애드혹 그룹 긴급 경보** | TS 24.379 §12.1A | ✗ |
| **애드혹 참가자 변경** · **기준 기반 참가자 결정** | TS 24.379 §17.2.6·§17.4.5 · §17.3.6·§17.4.6 | ✗ — 참가자 변경 요청에 200 을 주고 아무것도 하지 않는다(mcptt_conformance_gaps.md ADH-6) |
| **원격 긴급 발언 요청 트리거** | TS 24.379 §18 | ✗ |

> 구현됨: prearranged/chat 그룹콜, 일제 통화(호 단위 `<broadcast-ind>` — C8), private call(on-demand), affiliation(C1/C2), emergency/imminent 게이팅·선점, ad-hoc.

### R2. Floor Control (TS 24.380)

| 기능 | 규격 | 상태 |
|---|---|---|
| **Pre-established session floor** | TS 24.380 | ✗ (Call Control 파트의 세션 2단 수명과 함께 착수) |
| **Floor Revoke Request**(남의 발언 회수) | TS 24.380 §6.2.4.3.10 · §6.3.5.4.15 | ✗ — 받으면 버린다(mcptt_conformance_gaps.md FCS-12) |
| **audio cut-in 그룹** · **수신 전용 멤버** · **동시 발언 허용 목록** | TS 24.380 §6.3.2.2 · TS 24.481 §7.2.2 | ✗ (mcptt_conformance_gaps.md FCS-13·FCS-14) |

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
| **MCPTT 그룹 세션의 영상** (`m=video`) | — (MCPTT 는 speech 만) | ✅ 싣지 않는다 — 개시자 offer 의 `m=video` 는 같은 자리에 port 0 으로 거절(RFC 3264 §6), 멤버 fan-out 은 audio + floor 만([ptt_flows.md](ptt_flows.md)). 그룹 영상 = MCVideo 호 — 정본 [mcvideo.md](mcvideo.md), R6 |

### R4. 부가 서비스 / 인접 규격

| 기능 | 규격 | 상태 |
|---|---|---|
| **위치 정보 보고/관리** (Location management) | TS 23.280 / TS 24.379 | ✗ |
| **MCData MSRP relay / MSRPS(TLS)** | TS 24.282 / RFC 4976 | ✗ 후속 ([mcdata_messaging.md](mcdata_messaging.md)) |
| **MCData media plane 서비스 설정 문서** | TS 24.484 | ✗ (provisioning 채널 재사용) |
| **MCVideo** | TS 24.281 | ✗ — 세부 R6 |

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

**단말별 문서** — XCAP URI `…/org.3gpp.mcptt.ue-init-config/users/sip:<MCS UE ID>/<MCS UE ID>` 의 문서 이름이 그 단말의 UE ID 다. CMS 는
master 문서에서 그 단말의 문서를 만들고 `<mcptt-UE-id><Instance-ID-URN>` 을 그 UE ID 로 채운다(TS 24.484 §7.2.1.1 — `ue_init_config_for`).
ETag 는 그 문서의 것이다(master 가 바뀌면 같이 바뀐다).

**ue-init-config 값의 3계층** — 상용은 고객사 단말 외 다른 규격 단말과도 호환돼야 하므로, 고객사
필수 요소 외 규격 요소는 사용자지정으로 관리한다(`get_ue_init_config_xml`).

| 계층 | 요소 | 출처 |
|---|---|---|
| ① 토폴로지 유도 | `domain`·PLMN(도메인 `mnc<3자리>.mcc<3자리>` — TS 23.003 §13 대로 세 자리 MNC 의 앞자리 0 하나만 떼어 두 자리로 읽는다(`mnc008` → `45008`). 앞자리 0 인 세 자리 MNC 는 도메인으로 가를 수 없어 ② 수동 지정, 지정값은 PLMN 코드(5·6자리)인지 검사)·idms-auth/token-endpoint·gms/cms/kms·GMS/CMS-XCAP-root-URI·GMS-URI(`sip:gms_psi@도메인`) | `Provisioning.Services.ptt.domain`/`IdMs.Domain` + 공개 base URL = **`McpttServer.PublicUrl`**(비면 요청 Host 유도). CSP 가 NOTIFY 로 광고하는 `xcap-root` 도 같은 값(내부 API 취득) |
| ② 규격 파라미터값 | `<name>`·Timers T100/T101/T103/T104/T132(TS 24.380 단말 floor 타이머, unsignedByte — 기본 1/1/4/4/2 초, 표 11.1.1-1 NOTE 1·2 의 «재전송 총 시간 6초 미만» 안)·HPLMN PLMN 수동 지정·`*-to-con-ref`(APN/DNN)·`http-proxy`(비우면 공개 base URL — 단말은 XCAP 을 home HTTP proxy 로 보낸다, TS 24.482 A.2.1.2)·`mutual-authentication`·`group-creation-XUI`·`integrity/confidentiality-protection-enabled` | csc `config_template.json` 섹션 **"MCS UE 초기 설정 문서"** = `UeInitConfig.*`(scope=service, `restart:false` — SIGUSR1 리로드, ETag 내용파생이라 자동 갱신). 빈 값 = 유도값/기본값 |
| ③ 확장 요소 | `<on-network><anyExt>` 의 `MCPTT-Service-Details`(기본 on, Server-URI 기본 `sip:mcptt_psi@도메인` = CSP 의 MCPTT 서버 PSI) · `MCData-Service-Details`(기본 off) — `IPv6-Required` 는 false 고정 | `UeInitConfig.ServiceDetails.{Mcptt,McData}.{Enable,ServerUri}` |

산출물은 값 `html.escape` 후 minidom well-formed 검사 — 실패하면 경고를 남기고 **마지막 정상
문서**를 계속 서빙한다(설정 실수가 부트스트랩을 끊지 않게). 변경 구독(§7.2.2.12 → §6.3.13.3) — 재적재로 문서가 바뀌면 CSC 가
`UE_INIT_CONFIG_CHANGED` 를 보내고 CSP 가 cms 구독 단말마다 그 단말의 문서 선택자
(`org.3gpp.mcptt.ue-init-config/users/sip:<instance ID>/<instance ID>`, §7.2.1.1)로 xcap-diff NOTIFY 를 보낸다
([mcptt_timers.md](mcptt_timers.md) §4.3). 자유 XML 조각 주입(ExtraXml)은 두지
않는다. 규격 사슬 회귀 = `tests/csc_bootstrap_conformance.py`, 생성기 단위시험 =
`tests/csc_idms_authreq_unit.py` §A.

### R4-2. GMS · CMS — XCAP 절차 범위

| 기능 | 규격 | 상태 |
|---|---|---|
| **GMS** — global tree(`byGroupID`) · 요소 단위 XCAP · 멤버 제외 조회(POST) | TS 24.481 §6.2.2.2 · §6.3.6~§6.3.12 · §6.3.16 | ✗ — users tree 의 그룹 문서 통째 GET·PUT·DELETE 만(mcptt_conformance_gaps.md GMS-1·GMS-5·GMS-6) |
| **CMS** — 문서 생성·수정·삭제 · 요소 단위 절차 | TS 24.484 §6.3.2~§6.3.12 | ✗ — 문서 GET 만(mcptt_conformance_gaps.md CMS-9) |

### R5. 시그널링 세부 (RFC/구독) — 부분 미반영

- **NOTIFY 최종 실패(timeout/481) 시 구독 종료** (RFC 6665 MUST) — 미구현 (§C5 참조)
- **Subscription-State reason 구분** — 현재 timeout 고정
- **reg-event 다중 바인딩 / tel URI registration** — 미구현
- **ICE** (RFC 8445) — symmetric NAT 미해소 ([ue_nat_traversal.md](ue_nat_traversal.md) §9)

### R6. MCVideo (TS 22.281 / 23.281 / 24.281 / 24.581)

설계 정본 = [mcvideo.md](mcvideo.md)(규격 모델·개발 항목 V0~V8·결정 사항). MCVideo 는 MCPTT 에 영상을 얹은 것이 아니라 별도 MC 서비스다. MCPTT 그룹 세션은 음성만이고(R3), 그룹 영상은
같은 그룹의 MCVideo 호가 전송 제어(송출 요청·허가·동시 송출 상한)와 수신 제어(스트림 선택)로 다룬다 — 여러 현장
카메라를 관제가 스트림 단위로 골라 보고 제어하는 영상 관제 용도. 재사용 = PMP 멤버별 포트 단위·SRTP·녹취, PSP 그룹 세션 처리, SPS 그룹·설정 관리.

| 기능 | 규격 | 상태 |
|---|---|---|
| **MCVideo 서비스 신원 · 사용자 프로파일 · 서비스 설정 문서** | TS 23.281 / TS 24.484 | ✗ |
| **전송 제어 (Transmission Control)** — 그룹 내 동시 다중 송출, 최대 동시 송출 수 | TS 24.581 | ✗ |
| **수신 제어 (Reception Control)** — 수신자가 스트림 선택 · 거절 | TS 24.581 | ✗ |
| **그룹 · 1:1 · 방송 · 긴급 / 임박 위험 영상 호** | TS 24.281 | ✗ (1:1 영상은 현행 VoLTE 경로) |
| **Video pull** (단말 · 저장소 영상 가져오기) / **Video push** (영상 보내기) | TS 24.281 | ✗ |
| **Ambient viewing** (원격 카메라 개시) | TS 24.281 | ✗ |
| **원격 카메라 제어 · 영상 메타데이터**(위치 · 시각) | TS 22.281 / TS 24.281 | ✗ |
| **단말** — libcimsue MCVideo 클라이언트 · 관제 앱 다중 영상 수신 화면 | TS 24.281 | ✗ |

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
  Permission to Request the Floor=0 변형을 보낸다. 일제 통화 세션도 0 이다.
- Floor Indicator 는 owner tier 로 매핑: emergency→`0x1000`, imminent→`0x0800`, else normal `0x8000`
  (`_indicatorFor`). 일제 통화 세션은 `0x4000`, multi 정책은 `0x0080`, dual 은 화자 2명일 때 `0x0200`.
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
  0 을 실어 보내는 단말의 요청을 우선순위 0 으로 해석하면 선점 서열이 무너진다. 협상값은 CSP 가
  min(offer `mc_priority`, 그룹 문서 `<user-priority>`, service config `<num-levels-priority-hierarchy>`)로 정한다(TS 24.380
  §14.3.3 — 문서에 계층 수가 없으면 스키마 최솟값 4) — 단말이 offer 에 큰 값을 적어도 그룹 문서 우선순위를 넘지 못한다.
- **선점 요청자의 Queue Position Info**(§6.3.4.4.7 2)f)): 그 요청자가 큐잉을 협상했을 때만(멤버 `PTT_JOIN.queueing`).
- **초기 발언권**(`PTT_JOIN.granted` = CSP 가 개시 INVITE 의 암묵적 발언 요청 `mc_implicit_request` 를 받아들였다, §14.3.5):
  참가 시점에 발언자가 없으면 그 멤버에게 Floor Granted+Taken 을 보낸다(§6.3.4.2.2 3)·§6.3.4.4.2 1.).
- **1인 세션**: 참가자가 한 명뿐인 세션의 요청은 Deny **#3**(Only one participant).
- **Unicast Media Flow Control**(0x0B): 멤버가 자기 하향 미디어 중단/재개를 요청한다 —
  중단 상태 멤버에게는 audio/video 를 보내지 않는다(§6.3.4.4.14~15).
- **발언자가 아닌 참가자의 Floor Release**: 그 참가자의 대기 요청이 있으면 지우고(§6.3.5.3.7 5) · §6.3.5.4.5 3)),
  지금 상태로 답한다 — 화자 없음 = Floor Idle(§6.3.5.3.7 2)), 화자 있음 = Floor Taken(§6.3.5.4.5 4)). 단말은 PTT 를 떼면
  Floor Release 로 자기 대기 요청을 거둔다(§6.2.4.9.6). Floor Idle 을 놓쳐 Release 를 재전송하는 단말도 이 답으로 멈춘다.
- **Queued Floor Requests**(0x0E): 남의 대기 요청을 지우는 절차라 **인가된 사용자만** 쓴다(§6.3.5.4.12) — CIMS 는 그룹
  문서의 멤버 역할 `chair` 로 판정한다(참가자 유형 dispatcher·dispatch supervisor·MC service administrator 는 CMP 에
  전달되지 않는다 — 편차). 인가되지 않은 요청에는 Cancel Result(1) + Result **1**(Not authorized)만 돌려준다. 인가된
  요청은 지정 사용자(List of Queued Users)의 대기 요청을, **목록이 없으면 전체**를 지우고(§6.3.4.4.13 2)a)), 제거된 대기자에게
  Cancel Notification(2), 요청자에게 Cancel Result(1)+Result 값을 보낸 뒤 남은 대기자에게 위치를 다시 알린다.
- **회수 유예**: 회수 중(pending Floor Revoke)인 화자에게 선점 요청이 다시 와도(요청자의 T101 재전송) T3·T8·cause 를 다시
  잡지 않는다 — T3 는 회수에 들어갈 때 한 번이다(§6.3.4.5.2 · §6.3.5.6.3).

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

### C1. affiliation PUBLISH — 규격형·구형 양립

`Event` 헤더로 두 형태를 가른다(`CscfModule.cpp` `RecvRequestPublish`). 그 외 값은 489 Bad Event(RFC 6665 §8.2.1).

**규격형 `Event: presence`** (TS 24.379 §9.2.2.2.3, `RecvPublishAffiliationPidf`) — Request-URI 는 참여 MCPTT
기능의 PSI 라 대상 그룹을 본문에서 읽는다. 본문은 mcptt-info(`<mcptt-request-uri>` = served MCPTT ID, §9.2.1.2 2))와
`application/pidf+xml`(§9.3.1.2)의 multipart 다(pidf 하나만 실은 본문도 받는다). pidf 는 **그 클라이언트의 제휴 그룹
집합 전체**를 싣는다 — 증분이 아니라 **교체**다.

- 파싱 `ParsePidfAffiliation`(`McpttInfo.h`): `<presence entity>`=MCPTT ID · `<tuple id>`=MCPTT client ID ·
  `<affiliation group>` 집합. namespace prefix 무관, 태그·속성 경계를 확인해 유사 이름(`<affiliationX>`,
  `groupStatus=`)과 종료태그를 배제한다(외부 XML 파서 비의존).
- served MCPTT ID(mcptt-info `<mcptt-request-uri>`)가 요청자와 다르면 403(§9.2.2.2.3 1)~4) — 남의 제휴를 바꾸는 권한은 두지 않는다).
- 적용: 멤버인 그룹을 훑어 목록에 있으면 제휴, 없으면 해제. `Expires: 0` 은 그 사용자의 제휴 전부 해제.
  `entity` 가 요청자와 다르면 상태를 바꾸지 않고 200(§9.2.2.2.3 9). pidf 본문이 없으면 415. 감사(E-AUD-009)는 새로 선 제휴·해제만
  낸다 — 집합 교체라 같은 집합을 다시 실어도(등록 재성립) 요청마다 모든 그룹이 온다.
- **만료** — `Expires: 4294967295`(규격형 요청, §9.2.1.2 5)a))는 그대로 부여한다: 제휴에 시간 만료가 없고(DB `expires_at` NULL,
  200 OK `Expires: 4294967295`), 끝은 해제 PUBLISH·등록 종료다. 그룹 호 해제(T4 Inactivity·TNG3, §6.3.8.1)는 세션만 끝내고 제휴는 남긴다.
- **의도적 완화 둘** — 규격 클라이언트는 그대로 통과하고, 받아들이는 범위만 넓힌다:
  §9.2.2.2.3 5) 의 "Expires 가 4294967295 미만이면 423" 을 적용하지 않고 RFC 3903 §6 대로 서버가 짧게
  부여한다(min(요청, 상한) — 짧은 Expires 를 싣는 옛 단말). N2(`MaxAffiliationsN2`) 상한도 적용하지 않는다 — 우리 인가 축은 그룹 멤버십이다.

**구형 `Event: mcptt`** — 규격에 없는 자체 규약이며 **전환기 한시**다. Request-URI 가 그룹이고 본문은
`application/vnd.3gpp.mcptt-affiliation-command+xml`(아니면 415). `ParseAffiliationCommand` 가 `<actions>` 안의
`<affiliate>`/`<de-affiliate>` **액션 요소**(시작태그 앵커)와 `group` 속성을 추출한다. Expires:0 또는
de-affiliate 액션 → 해제, group 속성은 Req-URI 그룹과 교차검증.

> `application/vnd.3gpp.mcptt-affiliation-command+xml` 은 규격상 **다른 절차**의 본문이다(§9.2.1.4·§9.2.1.5 —
> 협상 모드로 *타인*의 제휴를 바꾸라고 보내는 SIP MESSAGE, Annex F.4). 구형이 이 이름을 빌려 쓰고 있으므로,
> 그 절차를 구현하기 전에 구형을 제거해야 한다.

**이행 순서**: ①서버 양립(완료) → ②우리 SDK 를 규격형으로(완료 — `sdk/core/src/engine.cpp` `sendMcpttAffiliationSet`,
PSI·MCPTT client ID 가 있는 계정. [ue_sdk.md](ue_sdk.md) §4.2) → ③구형 제거(옛 APK·PSI 없는 단말이 사라진 뒤).

- 보존: 멤버십 게이트(비멤버 affiliate 거절 — 규격형은 건너뛰고 로그, 구형은 403), REGISTER Expires:0 시 affiliation 정리.
- 검증: `tests/csp_pidf_affiliation_test.cpp`(S1-UNIT-CSP).

### C2. 제휴 상태 SUBSCRIBE/NOTIFY (presence — TS 24.379 §9.2.1.3·§9.2.2.2.4·§9.2.2.2.5)

- SUBSCRIBE: 단말은 R-URI = 참여 기능 PSI, `Event: presence`, `Accept: application/pidf+xml`, mcptt-info
  `<mcptt-request-uri>` = 대상 MCPTT ID 로 보낸다(§9.2.1.3). CSP 는 `Event: presence`(또는 옛 단말의
  `Accept: …mcptt-affiliation-info…`)를 이벤트 타입 `affiliation` 으로 분류한다(`CscfModule.cpp` RecvRequestSubscribe).
- NOTIFY 본문 = **`application/pidf+xml` per-user affiliation information**(§9.3.1.2 첫 목록, §9.2.2.2.5 3)) —
  `CspServer.cpp` `BuildAffiliationInfoBody` → `McpttInfo.h` `BuildPidfAffiliationInfo`:

  ```xml
  <presence xmlns="urn:ietf:params:xml:ns:pidf" xmlns:mcpttPI10="urn:3gpp:ns:mcpttPresInfo:1.0" entity="tel:+82500000006">
    <tuple id="RoT_MCX_2c08e90a-…">                         <!-- MCPTT client ID — 클라이언트마다 하나 -->
      <status>
        <mcpttPI10:affiliation group="tel:g001" status="affiliated" expires="2026-09-30T05:00:00Z"/>
      </status>
    </tuple>
    <mcpttPI10:p-id>…</mcpttPI10:p-id>                      <!-- 부른 PUBLISH 의 p-id (있을 때만) -->
  </presence>
  ```

  - `entity` = 가입자의 MCPTT ID(`tel:+msisdn` — 토큰·user-profile 과 같은 표기, `McpttIdUri`).
  - `group` = **MCPTT group ID**(`tel:g001` — user-profile·GMS 그룹 문서·그룹 INVITE `mcptt-calling-group-id` 와 같은 표기,
    CSC `_group_uri` 와 같은 규칙의 `McpttGroupUri`). 그룹 세션 URI(`sip:g001@<PTT 도메인>`)와 다르다.
  - `tuple id` = `ptt_affiliations.client_id` — 규격형 PUBLISH 는 pidf `tuple@id`(MCPTT client ID, §9.2.2.2.3 10))를,
    구형 PUBLISH(`Event: mcptt`)는 Contact URI 를 저장한다. 규격형 PUBLISH 는 같은 단말이 Contact URI 키로 남긴 행을 지워
    한 단말이 tuple 하나로 보이게 한다.
  - `expires` = 제휴 만료(`expires_at`, UTC xs:dateTime). 만료 없는 제휴는 속성 생략. 만료·해제된 제휴는 싣지 않는다(§9.2.2.2.5 3) a)·b)).
  - `p-id` = 규격형 PUBLISH 가 `<p-id>` 를 실었으면 그 PUBLISH 가 부른 NOTIFY 에 되돌린다(§9.2.2.2.5 3) d)). pidf 확장 요소라
    RFC 3863 스키마 순서대로 tuple 뒤에 둔다.
  - DB 미연결이면 제휴 정본을 읽을 수 없으므로 tuple 없는 문서를 낸다(fan-out 판정 `IsAffiliated` 도 그때는 거짓).
- 초기 NOTIFY(`SendInitialNotify`) + 제휴 변경 시 PUBLISH 경로에서 `SendAffiliationNotify(user, p-id)` 로 푸시(구독자 모두 같은 문서).
- **규격 대비 편차**

  | 항목 | 규격 | CIMS |
  |---|---|---|
  | 통지 대상 MCPTT ID | `<mcptt-request-uri>` 의 MCPTT ID, 남의 ID 면 권한 없을 때 403(§9.2.2.2.4 1)~4)) | 구독자 자신(From)만 — 타인 제휴 구독 미구현 |
  | 클라이언트별 제한 | `application/simple-filter+xml` 로 한 클라이언트만(§9.2.2.2.5 3) c), §9.3.2) | 필터 미적용 — 모든 클라이언트 tuple |
  | `status` 값 | affiliating·affiliated·deaffiliating | `affiliated` 만 — 서버가 PUBLISH 를 즉시 확정해 중간 상태가 없다 |
  | NOTIFY From URI | 대화 local URI = SUBSCRIBE 의 To URI(RFC 3261 §12.1.1) | `sip:mcptt_psi@<CSP IP:port>` — 대화 식별은 태그로 된다 |

### C3. Resource-Priority namespace 정규화

- INVITE 당 단일값 — 값은 service-config `<emergency-/imminent-peril-/normal-resource-priority>` 의 namespace·priority(TS 24.379 §6.3.3.1.19 —
  CSP `CCspServiceConfig::ResourcePriorityOf`, 멤버 fan-out·조건 재광고 공용). 문서에 없으면 emergency `mcpttp.15` / imminent `mcpttp.8` /
  normal `mcpttp.0`(RFC 8101 namespace `mcpttp` — `.0` 최저 ~ `.15` 최고).

### C4. floor SDP 토큰

- `m=application {port} UDP MCPTT` + `c=IN IP4 ...` + `a=floorid:0 mstrm:audio` +
  `a=fmtp:MCPTT mc_queueing;mc_priority=<그 멤버의 그룹 우선순위>` + **`a=mcptt-floor-request-uri:sip:{group}@{domain}`**
  (`GroupCallService.cpp` `MemberFloorOfferFmtp`). 단말은 floor 목적지를 이 `m=application` 포트에서 학습.
  이 fmtp 는 서버 **offer**(fan-out INVITE)의 값이다 — TS 24.380 §14.2.3 `mc_priority` = 그 멤버 entry 의 `<user-priority>`,
  §14.2.2 `mc_queueing` 은 큐잉을 지원하는 호만(개별 호는 CMP 가 큐를 끄므로 개별 호 offer 에는 floor fmtp 를 싣지 않는다).
- **개시자 200 OK answer**(psip `CSipDialog::AddSdp`, CSP 가 `CSipCallRtp::m_strApplicationFmtp` 로 정한다) — offer 에 있던 파라미터만(§14.3.1): `mc_queueing`
  은 offer 가 실었고 큐잉을 지원하는 호일 때(개별 호는 아니다 · fmtp 없는 구단말 offer 에는 종전대로 광고), `mc_priority` 는 offer 에
  있었으면 협상값(min(offer, `<user-priority>`, `<num-levels-priority-hierarchy>`), §14.3.3 — CMP `max_priority` 와 같은 값),
  **암묵적 발언 요청**을 받아들였으면 `mc_implicit_request` 를 되돌린다(§14.3.5 — 승인 뜻은
  아니다, §12.1.2.2 NOTE 4). 요청은 offer 의 `mc_implicit_request` 이고 `mc_granted` 는 200 OK 승인 표시를 받을 수 있다는 능력이라 요청으로 읽지 않는다
  (§14.2.4·§14.2.5·§12.1.2.2 NOTE 2). 받아들이는 것은 새 세션 개시뿐 — chat·진행 중 세션 합류·청취 합류는 아니다. 승인은 Floor Granted 로만 알린다(answer
  `mc_granted` 는 선택 "may", §14.3.4). 단말(SDK)은 두 속성을 함께 싣는다([mcptt_broadcast_group_call.md](mcptt_broadcast_group_call.md) R14).
- **역방향(멤버 SDP → CMP)**: 멤버가 광고한 `a=fmtp:MCPTT` 는 CSP 가 파싱해 `PTT_JOIN` 의
  `queueing`/`max_priority` 로, 개시자 offer 의 암묵적 발언 요청 수락은 `granted` 로 전달한다 (U14 서버 절반 —
  [../modules/csp.md](../modules/csp.md) 「멤버별 floor 협상 전달」).
- 보존: multipart(mcptt-info+SDP), `urn:3gpp:ns:mcpttInfo:1.0` 등 namespace.

### C4a. 멤버 leg INVITE (제어 기능 → 멤버) — §6.3.3.1.2

부록 A.1.3-7 예시와 같은 모양이다.

| 요소 | 규격 | 동작 |
|---|---|---|
| 본문 | §6.3.3.1.2 — mcptt-info + SDP | multipart = mcptt-info + SDP. **멤버 명단(`resource-lists`)은 싣지 않는다** — 명단은 conference 이벤트 패키지(§10.1.3)·GMS 그룹 문서. 본문이 멤버 수와 무관해 UDP 경로 MTU(RFC 3261 §18.1.1) 안에 든다(g001 = 2.2 KB) |
| Contact | §6.3.3.1.2 1) — 세션 식별자 + `g.3gpp.mcptt`·`g.3gpp.icsi-ref`·`isfocus` | `<sip:<그룹>@<CSP>;gr=<세션 토큰>>;+g.3gpp.mcptt;+g.3gpp.icsi-ref="urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt";isfocus` (RFC 3840 §9 — 확장 태그 `+`). 세션 식별자 = C4d |
| 서비스 식별 | §6.3.3.1.2 3) — P-Asserted-Service(RFC 6050) | `P-Asserted-Service: urn:urn-7:3gpp-service.ims.icsi.mcptt` (본문의 "P-Asserted-Service-Id" 는 표기 — 와이어 헤더 이름은 RFC 6050 §4.1, 부록 예시도 같다. 경보 팬아웃 MESSAGE 도 같은 이름) |
| Accept-Contact | §6.3.3.1.2 2)·4) | `*;+g.3gpp.icsi-ref=…;+g.3gpp.mcptt;require;explicit` |
| 세션 타이머 | §6.3.3.1.2 6) — Session-Expires 권고, «The refresher parameter shall be omitted» · 단말 200 OK = `refresher=uas`(§6.2.3.1.1 5)) | `Session-Expires` 는 refresher 없이 싣는다(psip 이 붙인 것을 지운다 — `McStripSessionRefresher`). 규격 단말은 `uas` 로 답해 스스로 갱신하고 CSP 는 만료를 감시한다. refresher 를 정하지 않는 단말(pjsip 기본 = `uac`)이면 CSP 가 갱신한다([leg_liveness.md](leg_liveness.md) §5.3) |
| floor 선언 유지 | RFC 3264 §8 — 이어지는 offer·answer 의 m= 는 처음과 같다(포트 0 = 스트림 끔) | floor `m=application`·`a=fmtp:MCPTT`(`MemberFloorOfferFmtp`)는 본문에 덧붙이는 줄이라 다이얼로그에도 같은 선언을 둔다(psip `SetLocalApplicationMedia`) — 스택이 만드는 세션 갱신 offer·멤버 re-INVITE answer 가 floor 를 그대로 싣는다 |

### C4b. 개시자 응답 (제어 기능 → 개시자) — §6.3.3.2.3

| 요소 | 규격 | 동작 |
|---|---|---|
| Contact | §6.3.3.2.3.2 5)·6) — 세션 식별자 + `g.3gpp.mcptt`·`g.3gpp.icsi-ref`·`isfocus` | C4a 와 같은 태그. 주소는 psip 이 수신 listener 로 정하고 CSP 는 태그만 준다(`SetContactParams`) — 갱신 re-INVITE 2xx·이후 in-dialog 요청(조건 재광고·BYE)에도 실린다 |
| 세션 타이머 | 2) refresher = `uac` · 3) `Require: timer` | 개시자가 refresher 를 지정하지 않았으면 `uac`(단말 갱신, CSP 만료 감시) + `Require: timer`. 개시자가 지정했거나 timer 미지원이면 RFC 4028 §9 Table 2(미지원 = `uas`) — [leg_liveness.md](leg_liveness.md) §5.3 |
| re-INVITE answer fmtp | TS 24.380 §14.3.1 — answer 의 fmtp 는 offer 에 없던 파라미터를 싣지 않는다 · §14.3.5 암묵 요청은 새 세션 개시에서만 | 개시자·멤버 leg 의 re-INVITE(세션 갱신 포함) answer 는 `a=fmtp:MCPTT` 를 그 re-offer 로 다시 짓는다(`RebuildReInviteFloorFmtp` — 개시 answer 의 `mc_implicit_request`·초대 offer 의 `mc_priority` 를 되풀이하지 않는다). 내용이 바뀌면 `o=` 버전을 올린다(RFC 3264 §8). 제어 기능의 조건 재광고 offer 는 개시 전용 `mc_granted`·`mc_implicit_request` 를 뺀다(§14.5) |
| P-Asserted-Identity | 4) 제어 기능 PSI | 그룹 URI(`<sip:<그룹>@<PTT 도메인>>`) — 멤버 leg INVITE 의 PAI 와 같은 신원 |
| Supported | 8) `tdialog`(RFC 4538) | `Supported: tdialog` |
| Warning | 7) 받은 응답의 Warning 을 옮긴다 · 확인 통화 설정의 111 (C4c) | 개시자 응답 게이트 동안 멤버 초대 leg 의 응답(18x·최종)에 실린 Warning 값을 모아 200 OK 에 싣는다(중복 제외, 제어 기능 자신의 111 이 앞 — RFC 3261 §20.43 쉼표 연결). 필수 멤버 없이 진행하면 `Warning: 399 <agent> "111 group call proceeded without all required group members"`. 게이트 없이 곧바로 수락한 200 은 받은 응답이 아직 없다 |
| 응답 시점 | §10.1.1.4.2 · §6.3.3.3 · §11.1.1.4.2 | 새 세션 개시의 200 OK 는 **개시자 응답 게이트** 뒤다 — C4c. 진행 중 세션 합류·청취·chat 은 곧바로 |
| P-Answer-State | §10.1.1.4.2 · RFC 4964 | 멤버 확인 전 수락이면 `P-Answer-State: Unconfirmed` — C4f |

### C4c. 확인 통화 설정 (acknowledged call setup) — TS 24.379 §6.3.3.3·§10.1.1.4.2·§11.1.1.4.2

그룹 문서의 값(TS 24.481 §7.2.2 s)t)u)·§7.2.4.2)으로 제어 기능이 개시자 200 OK 를 멤버 응답에 맞춘다. 값은 CSC DB
(`ptt_groups.min_number_to_start`·`ack_timeout_sec`·`ack_action`, `ptt_group_members.on_network_required`) → GMS 그룹 문서 · CSP 그룹 캐시가 같은 값을 쓴다.

| 요소 | 그룹 문서 | 동작 |
|---|---|---|
| 필수 멤버 | `<entry>` 의 `<on-network-required>` — 필수 멤버에만 싣는다 | affiliated·초대 대상인 필수 멤버가 있으면 **초대 전에 TNG1** 을 켜고, 그 멤버 전원의 200 과 누계 ≥ 최소 인원에 수락한다(TNG1 정지) |
| 시작 최소 인원 | `<on-network-minimum-number-to-start>` (xs:unsignedShort, 기본 0) | 멤버 200 누계(§10.1.1.4.1.1 3))가 이 값에 닿으면 수락. 0 = 기다리지 않는다(개시 즉시) |
| TNG1 | `<on-network-timeout-for-acknowledgement-of-required-members>` (xs:duration, 기본 5 s, 1~300) | 부록 B.2.1 — 만료 때 누계가 최소 인원 미만이면 닿을 때까지 기다린 뒤 만료 동작 |
| 만료 동작 | `<on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>` proceed \| abandon (정의 밖 값 = abandon) | proceed = 200 + `Warning: 399 <agent> "111 …"`. abandon = 480 + `"112 group call abandoned due to required group members not part of the group session"`, 확립 leg BYE·미확립 CANCEL |
| 필수 멤버 거절 | — | TNG1 중 필수 멤버의 4xx~6xx: abandon 이면 그 코드를 `"112 … required group member …"` 와 함께 개시자에게, proceed 면 나머지 전원이 200 일 때 200 + 111 (그 전엔 TNG1 계속) |
| 전원 거절 | — | 수락 전에 초대한 멤버 전원이 최종 응답했으면 캐시한 최종 응답(6xx 우선)을 개시자에게 (§10.1.1.4.2 1)) |
| 사설 호 | — | 최소 1(착신자) — 착신자 180 을 개시자에게 옮기고 착신자 200 뒤에 수락한다(§11.1.1.4.2) |
| 개시자 CANCEL | — | 게이트를 거두고 초대 leg·세션을 해제한다 |

| 미응답 멤버 알림 | §6.3.3.3 — 200 OK 에 111 이 실렸고 개시자 user profile `<allow-to-receive-non-acknowledged-users-information>`(TS 24.484 anyExt) 가 true | ACK 뒤(1초 주기 점검) 개시자 다이얼로그에 INFO — `Info-Package: g.3gpp.mcptt-info`, 본문 mcptt-info(F.1) `<mcptt-Params>` 의 `<anyExt>` 안에 200 을 보내지 않은 초대 멤버마다 `<non-acknowledged-user type="Normal"><mcpttURI>tel:<id></mcpttURI>` (RFC 6086 §4.2.2 `Content-Disposition: Info-Package`). 자격 = `ptt_user_profile.allow_non_ack_users_info`(기본 0, 콘솔 가입자 PTT 프로파일) |

정원(`on-network-max-participant-count`)은 필수 멤버 수보다 작을 수 없다 — 관리 API 가 저장 단계에서 막는다(§6.3.5.5 NOTE 4). 시도 장부 cause = `no_member_answered`·
`ack_timeout_abandoned`·`ack_required_rejected`·`initiator_canceled`([sip_statistics.md](sip_statistics.md) §2.3). 구현 = CSP `CGroupCallService::AckGate*`.

### C4d. MCPTT 세션 식별자 — TS 24.379 §4.5 (GRUU)

세션을 가리키는 URI = `sip:<그룹>@<CSP>;gr=<세션 토큰>`(RFC 5627 GRUU 형 — 토큰 = 세션 sesid 의 시각·순번, 세션마다 새로 나고 세션이 끝나면 사라진다).
멤버 leg INVITE·개시자 응답·이후 in-dialog 요청과 응답의 Contact 에 싣는다(psip `SetContactUriParams`). 재합류 INVITE 의 Request-URI 가
세션 식별자면 그 세션이 진행 중이어야 한다 — 아니면 404(§10.1.1.4.5.1 2)).

### C4e. Warning 헤더 형식 — TS 24.379 §4.4 · TS 24.282 §4.9

`Warning: 399 <agent> "<mcptt-warn-code> <text>"` — RFC 3261 §20.43 warning-value(warn-code 399 = 기타, warn-agent = PTT 도메인) 안의 warn-text 가
MCPTT/MCData 경고 코드와 문구다(`McpttWarning`). conference 구독 거절 105·138, 확인 통화 설정 111·112, MCData 203 이 같은 형식이다. 받는 쪽
(libcsim)은 따옴표 안 앞 세 자리를 경고 코드로 읽는다.

### C4f. 멤버 확인 전 수락 · 미디어 버퍼링 — TS 24.379 §10.1.1.4.2 · §11.1.1.4.2 · RFC 4964

CSP 는 멤버 쪽 참여 기능(participating function)을 겸한다. 멤버 단말은 자동 응답(automatic commencement, §6.3.2.2.5.2)이라
참여 기능이 멤버를 대신해 183 `P-Answer-State: Unconfirmed` 를 낸 것과 같고, 제어 기능은 **미디어 버퍼링을 할 때만** 그 183 으로
개시자에게 200 OK 를 줄 수 있다. 버퍼링 = CMP([cmp.md](../modules/cmp.md) §3.5 «미디어 버퍼링» — 첫 수신자 합류 전 화자 음성을 담았다가 원래 간격으로 재생),
광고 = HEARTBEAT `resource.media_buffer`.

| 경우 | 동작 |
|---|---|
| 새 세션 개시, 필수 멤버 없음·최소 인원 0, CMP 버퍼링 광고 | 멤버 초대와 함께 곧바로 200 OK + `P-Answer-State: Unconfirmed`(초대할 멤버가 없으면 헤더 없이) |
| 위와 같은데 CMP 가 버퍼링을 광고하지 않음 | 최소 인원 1 로 보고 첫 멤버의 200 뒤에 수락(개시자 응답 게이트 — C4c). 초대한 멤버가 모두 거절하면 그 최종 응답 |
| 게이트 중 멤버 leg 의 183 + `P-Answer-State: Unconfirmed`(외부 참여 기능·단말), TNG1 이 돌지 않음(없었거나 필수 멤버 전원 응답으로 멈춤), 버퍼링 광고 | 개시자에게 200 OK + `P-Answer-State: Unconfirmed`(모은 Warning 동봉). 사설 호도 같다(§11.1.1.4.2) |
| 멤버 leg 의 신뢰성 18x(`Require: 100rel` + `RSeq`) | 제어 기능(UAC)이 PRACK(RFC 3262) |

psip 은 최초 INVITE 응답 원문을 `EventInviteResponse` 로 응용에 올린다(Ring/Start/End 보다 먼저) — Warning 수집·`P-Answer-State` 판독·PRACK 이
여기서 난다(`CGroupCallService::OnMemberInviteResponse`).

### C4g. affiliation 검사 — TS 24.379 §10.1.1.4.2 14)a)·15)a) · §10.1.1.4.5.1 8) · §10.1.2.4.1.1 6)·13) · §9.2.2.3.6~8

개시·합류·재합류 INVITE 의 개시자가 그 그룹에 affiliate 했는지(§6.3.6)를 본다. 판정 대상은 **affiliation 을 쓰는 그룹**(`require_affiliation` —
fan-out 이 affiliated 멤버만 초대하는 그룹)이다. 그 밖의 그룹은 멤버 전원을 초대하므로 멤버십이 곧 affiliation 이다. 순서 = 멤버십 403·코덱 488·SRTP
488·긴급/임박 인가 403 뒤, 세션 생성 전.

| 요청 | affiliate 안 함 |
|---|---|
| 편성 그룹 호 일반 개시·합류·재합류 | 403 + `Warning: 399 <agent> "120 user is not affiliated to this group"` (시도 장부 cause `not_affiliated`) |
| 인가된 긴급·임박 위험 개시·합류 | 암묵적 affiliation(§9.2.2.3.7) — 자격(§9.2.2.3.6 → §9.2.2.3.8 = 그룹 존재 + 멤버)이 있으면 affiliation 을 기록(client id = 등록 Contact, 만료 3600 s)하고 affiliation-info 구독자 NOTIFY(§9.2.2.3.5)·감사 E-AUD-009 뒤 진행 |
| chat 그룹 합류 | 암묵적 affiliation(§10.1.2.4.1.1 13)) |
| 청취 leg(비멤버 관제사)·사설 호 | 대상 아님 — 청취는 2단 인가([dispatch_center.md](dispatch_center.md) §5.6) |

DB 단절이면 fan-out 과 같이 검사를 건너뛴다(affiliation 원천 = `ptt_affiliations`).

남은 편차
- 개시자 200 OK 의 `Supported` 에 `norefersub`(RFC 4488)·`explicitsub`/`nosub`(RFC 7614) 을 싣지 않는다(§6.3.3.2.3.2 9)·10)) — 그룹 세션
  다이얼로그의 REFER 를 지원하지 않아 광고하지 않는다.
- 필수 멤버 없이 진행한 뒤의 in-dialog MESSAGE 안내(§6.3.3.3 1)b)·c) "may")는 보내지 않는다.
- 개시 전 affiliation 인원 검사(§10.1.1.4.2 14)g)i) — `<on-network-minimum-number-of-affiliated-members>`·`<on-network-affiliation-to-group-required>`
  미달 시 480 + 112)는 그룹 문서 요소가 없어 하지 않는다.
- 멤버별 응답 방식(poc-settings Answer-Mode, §6.3.2.2.5·§6.3.2.2.6)을 받지 않는다 — 모든 멤버를 자동 응답으로 본다(C4f).

### C4h. 개시 INVITE 대상 — 참여 기능 PSI + `<mcptt-request-uri>` (TS 24.379 §10.1.1.2.1.1 · §11.1.1.2.1.1)

- 규격형 개시 INVITE 는 **Request-URI = 원발 참여 MCPTT 기능의 PSI**, 대상은 mcptt-info `<mcptt-request-uri>`
  (prearranged·chat 그룹콜 = MCPTT group ID `tel:g006`, 개별 통화 = 상대 MCPTT ID `tel:+8250…`)다.
- `ModuleDispatcher::EventIncomingCall` 이 mcptt-info 해석 직후 대상을 정한다 — `<mcptt-request-uri>` 의 식별자
  (`McpttPsiTarget`, `McpttInfo.h`)가 Request-URI user 와 다르고 **Request-URI 가 그룹도 가입자도 아니면** PSI 로 보고
  대상을 그 식별자로 바꾼다. 이후 경로(그룹 lazy-load·개별 통화·그룹콜 fan-out·affiliation 검사 C4g)는 같다.
- PSI 이름은 대조하지 않는다 — CSC ue-init-config 가 알리는 `sip:mcptt_psi@<PTT 도메인>` 도, 단말 설정 PSI(예
  `mcptt1_opf_psi`)도 같은 규칙으로 받는다.
- 구형 단말(Request-URI = `sip:<그룹>@<PTT 도메인>` 또는 상대 번호)은 두 값이 같거나 `<mcptt-request-uri>` 가 없어
  종전 경로 그대로다. mcptt-info 가 없는 VoLTE 호는 판정 자체를 하지 않는다(가입자 조회 없음).
- 검증: `tests/csp_mcptt_info_test.cpp`(`McpttPsiTarget` — HM-TRCP 실측 본문 포함, S1-UNIT-CSP).

### C9. 암시적 제휴 — 설정 그룹(TS 24.379 §7.3.3 13)·§7.3.4 13) → §9.2.2.2.15) · ad hoc(§17.4.2.2 16))

규격의 암시적 제휴는 셋이다 — ① 관리자가 사용자별로 정한 **설정 그룹**(user profile `<OnNetwork><ImplicitAffiliations>`)을
서비스 인가 때, ② 제휴 없이 긴급·임박 위험 개시·chat 합류할 때(§9.2.2.2.12 — C4g), ③ ad hoc 그룹콜에 초대될 때. 이 절은 ①·③ 이다.

- **설정** = 멤버 행 `ptt_group_members.implicit_affiliation`(사람×그룹, 기본 0 — `sql/migrate_ptt_group_members_implicit_affiliation.sql`).
  `<ImplicitAffiliations>` 가 사용자 프로파일 요소라 멤버마다 정한다. 관리 API `members[].implicit_affiliation`·
  `POST …/members`(보낸 경우에만 변경), 콘솔 그룹 워크벤치 멤버 행 «자동 제휴».
  멤버를 통째로 다시 쓰는 경로(관리 API 그룹 갱신 `members`·GMS XCAP 그룹 문서 PUT)는 값이 없는 멤버의 설정을 잇는다 —
  그룹 문서(TS 24.481)에는 이 요소가 없다.
- **CSC** user profile `<ImplicitAffiliations>` = 그 사용자의 멤버 행에 설정이 켜진 그룹만(없으면 요소 생략).
- **CSP** `_ApplyImplicitAffiliations`(`CscfModule.cpp`) — PTT REGISTER 200 뒤, 설정이 켜진 멤버 그룹마다 제휴를 기록하고
  새로 생긴 제휴가 있으면 제휴 상태 NOTIFY(C2). 클라이언트 ID = REGISTER mcptt-info `<mcptt-client-id>`(§9.2.2.2.15 2)) >
  Contact `+sip.instance` > Contact URI.
- **규격 대비 편차**

  | 항목 | 규격 | CIMS |
  |---|---|---|
  | 만료 | candidate expiration interval(암시적 제휴용 값은 정의되지 않음 — PUBLISH Expires 로만 정의) · 이미 제휴된 그룹은 새로 넣지 않음(§9.2.2.2.15 8) b)) | **등록 수명**(부여 등록 만료)으로 기록하고 재등록마다 갱신 — 등록이 살아 있는 동안 끊기지 않게. 해지 REGISTER 는 제휴 전부 해제(종전 그대로) |
  | N2 상한 | 초과분을 정책으로 줄임(§9.2.2.2.15 9) c)) | 적용하지 않음 — PUBLISH 경로(C1)와 같다 |
  | 확정 | "affiliating" → 제어 기능 PUBLISH(§9.2.2.2.6) → affiliated | 참여·제어 기능이 한 서버라 곧바로 affiliated 로 기록 |
  | 계기 | 서비스 인가·서비스 설정 PUBLISH 수신(§7.3.3 13) · §7.3.4 13)) — REGISTER 절차(§7.3.2)에는 이 단계가 없다 | **PTT REGISTER 200 뒤** — 서비스 설정 PUBLISH(poc-settings)를 받지 않는다(489 — §0-R «서비스 설정») |

- **③ ad hoc** — 제어 기능은 초대한 멤버를 그 ad hoc 그룹에 암시적으로 제휴된 것으로 본다(§17.4.2.2 16), 참여자 변경 §17.4.5.1.1 vi)·
  §17.4.5.2.1 d)). CSP 는 ad hoc 그룹을 통화 때 만들며 `require_affiliation = false` 로 둬(`ModuleDispatcher.cpp`) 명단 전원을
  초대하고, 명단 밖 재합류는 멤버십 검사로 막으며, 제휴 멤버 최소 인원 조건(§17.4.3.1.1.1 1))은 쓰지 않는다 — 결과가 같다.
  ad hoc 그룹은 DB 그룹이 아니라(임시) `ptt_affiliations` 행을 남기지 않는다.
- 우리 단말 앱은 소속 그룹 전부에 스스로 PUBLISH 한다(C1) — 설정과 무관하게 동작이 같다.

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
  ad-hoc/chat/prearranged·일제 통화, GMS/CMS xcap-diff NOTIFY.

---

## 3. CSC — IdMS / GMS / CMS / KMS

근거: `csc/src/services/mcptt.py`(라우트 `CSC_HANDLER_LIST`).

### IdMS (TS 33.180 / OIDC)
- **S1 디스커버리**: `GET /.well-known/openid-configuration`(`handle_openid_config`) — issuer/authorization·
  token·introspection endpoint·`jwks_uri`·`code_challenge_methods_supported=[S256]`·grant types·claims 광고.
- **토큰 서명**(TS 33.180 B.2.2.1 «JSON web digital signature» RFC 7515 · OIDC Core §15.1): ID token·access token = **RS256**, JWS 헤더
  `kid`(RFC 7638), 공개 키 `GET /idms/jwks`. 서명 키는 runtime store 에 영속(HA 쌍 공유). 검증은 `alg` 별 키 하나로만 — HS256 은
  전환기 스위치 `IdMs.AcceptHs256` 일 때만. 상세 = [mcx_identity_scope.md](mcx_identity_scope.md) §2.1.
- **S2a access_token 클레임**: `sub`(=login_id)/`iss`/`iat`/`exp`/`aud`/`client_id`/`scope`(공백 구분 문자열)/
  `mcptt_id`/`mcdata_id`(`create_tokens`). scope = 요청 ∩ 카탈로그(`grant_scope`, B.4.2.2 `3gpp:mc:*`), 리소스 서버 검사
  `require_scope`(B.10). 상세·별칭·롤아웃 = [mcx_identity_scope.md](mcx_identity_scope.md).
  **MC 신원·MC scope 는 PTT 가입이 있는 계정에만**(TS 24.482 §4.1 — scope 는 그 사용자가 인가된 서비스) — 전화 전용 계정의 토큰에는
  `mcptt_id`·`mcdata_id`·`3gpp:mc:*` 가 없고, MCPTT user profile·KMS 키 요청은 403 이다.
- **리소스 서버 응답**(TS 24.482 A.2.3): Bearer 토큰 없음 = **403**, 토큰 검증 실패 = 401 `invalid_token`(`unauthorized`).
- **토큰 응답**(TS 33.180 B.4.2.5 · RFC 6749 §5.1): `Cache-Control: no-store`·`Pragma: no-cache`. **refresh**(B.5.3)는 계정을 다시
  본다 — 계정이 없거나 비밀번호가 바뀌었으면 회수하고 `invalid_grant`.
- **TLS**(B.12): 단말 접속점은 인증서가 없으면 뜨지 않는다(평문 폴백 없음, 시험용 `McpttServer.AllowPlaintext`).
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
    전역 `redirect_uri` 제한 `IdMs.RedirectUriAllow`(비면 전부 허용, 정확 일치 RFC 6749 §3.1.2.3, 위반 400).
  - **클라이언트 등록·필수 파라미터**(TS 33.180 B.3·B.4.2.2·B.4.2.4): 등록 = `IdMs.Clients`(`client_id` → 허용 `redirect_uri`).
    인증 요청은 `response_type`·`client_id`·`state`·`scope`(openid)·`redirect_uri`·`acr_values`(`3gpp:acr:password`) 필수,
    토큰 요청은 `client_id`·`redirect_uri` 필수·인증 요청과 일치·등록 대조. 집행은 `IdMs.ClientEnforcement`(`enforce|log|off`,
    기본 `log` — 위반을 `[IdMS][client] would-reject` 로 남기고 통과; 앱이 전부 맞춘 뒤 `enforce`) —
    [mcx_identity_scope.md](mcx_identity_scope.md) §4.1.
  - 회귀: 규격 사슬 `tests/csc_bootstrap_conformance.py` Step 3(간이형)·3b(규격), 오프라인 단위
    `tests/csc_idms_authreq_unit.py` §B.
- 보존: **PKCE S256 강제**(plain/누락 400), refresh 회전/취소.

### GMS (TS 24.481)
- 그룹문서 XML(`urn:oma:xml:poc:list-service`+`urn:3gpp:ns:mcpttGroupInfo:1.0`), ETag/If-None-Match 304,
  수평/수직 권한(403).
- **S3 변경통지**: 그룹 CRUD 시 `notify_csp("GROUP_CHANGED")`(`handlers/admin.py`) → CSP `CscInterface`
  → `OnGroupConfigChanged` → `ReloadGroupMap`(그룹 맵 재적재 **뒤**, 재적재 전·후 멤버 합집합) → `SendGroupDocNotify`
  → GMS 구독자에 **xcap-diff NOTIFY**(RFC 5875). 60초 주기 재적재도 같은 전후 비교로 놓친 변경을 통지한다.
- **그룹 문서 값**(`get_group_xml`, TS 24.481 §7.2.2·§7.2.8) — 이름·직함·조직 코드·URI 는 텍스트·속성 모두 escape(RFC 4825 well-formed).
  `<protect-media>`·`<protect-floor-control-signalling>` 은 없으면 true(GMK 필수·floor 보호 필수)라 **false 를 명시**한다(E2E 미구현 —
  [mcx_e2e_security.md](mcx_e2e_security.md)). 정원 `<on-network-max-participant-count>` 는 `max_members` 그대로, **0(무제한)은 요소
  생략**(0 은 «0명» 으로 읽힌다). CSP 는 정원을 집행한다 — 새 세션은 정원 − 1 명(개시자 포함 정원)까지만 초대하고 필수 멤버를 먼저 두며, 다 초대하지
  못하면 개시자 200 OK 에 Warning `122 too many participants`(TS 24.379 §6.3.5.5 · §10.1.1.4.2), 참가 leg(청취 제외)이 찬 진행 중 세션에 들어오는
  합류·재합류는 486 + `122`(§10.1.1.4.2 15)d) · chat §10.1.2.4.1.1 12) · §10.1.1.4.5.1 10)). 청취 leg 은 정원에 세지도 막지도 않고, 우선순위로
  기존 참가자를 내보내는 선택(local policy)은 두지 않는다. `<preferred-voice-encodings>` = CSP 서비스 코덱 AMR-WB(`SERVICE_VOICE_ENCODING` — CSP
  `Setup.Media.Codecs` 첫 항목과 같아야 한다; 단말은 그룹 호 offer 에 넣는다 TS 24.379 §6.2.1 2)b), SDK 는 지원 코덱을 늘 offer 해 이미
  따른다). on-network 를 끈 그룹(`on_network`=0)은 `<on-network-disabled/>`(§7.2.2 g)) — XCAP PUT 은 요소가 있으면 끄고 없으면 그대로
  둔다. 호의 판정은 CSP 가 한다 — 그 그룹의 개시·합류 INVITE 는 403 + `115 group is disabled`(TS 24.379 §6.3.5.2 5)a) — 멤버십보다 먼저, MCVideo 호도 같다). 멤버 규칙 actions 에 `<on-network-allow-getting-member-list>true`(없으면 false — 멤버가
  명단을 못 읽는다, §7.2.12.1). 그룹·멤버 우선순위는 priorityType 0~255(§7.2.4.2, 값이 클수록 높다) — 관리 API·XCAP PUT 이 범위 밖을
  400 으로 거절하고, 범위 밖 저장값은 문서에서 경계로 자른다. MCData 그룹(SDS·FD 허용)의 서버 결정 값(보호 둘 false·송신 인가 true·
  그룹 우선순위·charset 106) = [mcdata_messaging.md](mcdata_messaging.md) §2.
- **그룹 호 타이머 요소**(TS 24.481 §7.2.2 o)p)·§7.2.7) — 문서에 0 을 싣지 않는다: T4 0 = `<on-network-hang-timer>` 생략(요소가
  없으면 T4 를 걸지 않는다), chat 그룹 = `<on-network-maximum-duration>` 생략(TNG3 를 돌리지 않는다 — TS 24.379 §6.3.3.5.1 은
  요소가 있을 때만 켜고 chat 은 선택). XCAP PUT 은 요소가 없으면 기존값을 둔다.
- **규격 대비 편차 — TNG3 «무제한»**: 편성 그룹은 `<on-network-maximum-duration>` 에 값이 필수인데(§7.2.7) 규격에 «무제한» 표기가
  없다. CIMS 의 0(무제한)은 무제한 표기 `PT2147483647S`(`GROUP_MAX_DURATION_UNLIMITED` — 32비트 초 카운터 최댓값, 약 68년)로
  싣고, XCAP PUT 은 그 값 이상을 0 으로 되읽는다. 사유 = xs:duration 에 상한이 없어(XML Schema Part 2 §3.2.6) 수신 측이 초를
  32비트 정수로 들어도 넘치지 않는 가장 큰 값이자, 설정 범위(0~86400초)와 겹치지 않아 왕복이 DB 값을 바꾸지 않는 값이다.
  CSP 는 0 이면 TNG3 를 돌리지 않는다([mcptt_timers.md](mcptt_timers.md) §7 D8).

### CMS (TS 24.484)
- user-profile XML(ns = 규격 §8.3.2.4 정본 `urn:3gpp:mcptt:user-profile:1.0`), self-access 권한(신원 표기 tel:/sip:/sip:@도메인 관용), ETag.
  **내용은 §8.3.2 XSD 대로**(`get_user_profile_xml`): 루트 `XUI-URI`·`user-profile-index`, `<Common>` = UserAlias·
  MCPTTUserID(uri-entry)·PrivateCall(PrivateCallList = 내 그룹 동료 멤버, EmergencyCall = MCPTTPrivateRecipient entry + ProSeUserID-entry User-Info-ID 영값 — ProSe 미지원)·
  MCPTT-group-call(MaxSimultaneousCallsN6·EmergencyCall/ImminentPerilCall/EmergencyAlert·Priority)·MissionCriticalOrganization,
  `<cp:ruleset>`(RFC 4745) 사용자 인가 — **인가 요소는 없으면 false**(표 8.3.2.7)라 쓰는 것을 전부 싣는다(§8.3.2.1 11) 목록 순): 개별 호
  `<allow-private-call>`(`ptt_user_profile.allow_private_call`)·수동/자동 개시(같은 값)·`<allow-force-auto-answer>` false · 긴급·임박·경보 개시와
  해제(`<allow-imminent-peril-call>` = 긴급 그룹콜 인가, `<allow-cancel-private-emergency-call>` = 긴급 사설콜 인가) ·
  `<allow-private-call-to-any-user>`(발신 인가 ∧ `allow_private_call_to_any_user`)·`<allow-private-call-participation>`(`allow_private_call_participation`) ·
  `<anyExt>` K `<allow-to-receive-private-call-from-any-user>`(= 착신 참가 — IncomingPrivateCallList 없음)·L·R·S
  `<allow-adhoc-group-call-participation>` true · AA `<allow-to-modify-adhoc-group-call-participants-info>` false(그 절차 없음).
  CSP 의 판정(107·144·127·188 등)은 미구현 — 갭 PRV-2·PRV-8·ADH-5. `<OnNetwork>` = **MCPTTGroupInfo(소속 그룹 = 규격 단말의 그룹 목록 소스, 소유 소속 그룹은
  anyExt `cims:authorized-user`)**·MaxAffiliationsN2(`mcptt_service_config.max_affiliations_n2`)·ImplicitAffiliations(멤버 `implicit_affiliation` 이 켜진 그룹만 — C9)·
  MaxSimultaneousTransmissionsN7·PrivateEmergencyAlert. 상수는 `UserProfile.*` 설정. 루트 `<Status>true</Status>`(§8.3.2.1 3)·alias-entry `index` 병기.
  **N6**(`<MaxSimultaneousCallsN6>`, §8.3.2.1 8)e)i) — 동시 그룹 호 상한)는 사용자마다의 값이다: 그 PTT 회선의 사람에게 역할 배정
  (`role_assignments`, [mcptt_authorization.md](mcptt_authorization.md))이 있으면 «관제» = `mcptt_service_config.max_calls_n6_dispatch`(기본 10),
  아니면 `max_calls_n6`(기본 5) — `user_max_calls_n6`. 판정 데이터는 역할 배정 한 곳이고, CSP 의 역할 맵(`CCspRoleMap::SelectForLine`)과
  같은 펼침이라 집행(486 + `103`, TS 24.379 §10.1.1.3.1.1 5) · chat §10.1.2.3.1.1 5) · 애드혹 §17.3.2.1.1 6))도 같은 판정을 쓴다 — CSP 는
  개시·합류 INVITE 에서 그 사용자가 들어 있는 그룹 호 수(확립 leg 또는 그 사용자가 개시한 leg 이 있는 그룹 — 개별 호 제외·청취 leg 포함·같은
  그룹 재합류는 새 호가 아니다)가 N6 이상이면 486 103, 인가된 긴급·임박 요청은 예외(NOTE 3). 값은 같은 열을 `CCspServiceConfig::GetMaxCallsN6`
  이 읽는다(기동·`SERVICE_CONFIG_CHANGED`). 역할 배정·해제로 판정이
  바뀌면 CSC 가 그 사람의 PTT 회선마다 `USER_CHANGED` 를 보내 user-profile xcap-diff 가 나간다. 모든 `<entry>` 에 `index`(§8.3.2.1 —
  목록 안에서 유일)·`<ProSeUserID-entry>` 에도 `index`, 소속 그룹이 없어도 `<MCPTTGroupInfo>` 를 싣는다(10)b), 빈 목록은 XSD 가 허용).
  PTT 그룹 호 청취 자격은 `<cims:allow-ambient-listening>`(CIMS 확장) — 비멤버 관제사의 recvonly 합류 자격([dispatch_center.md](dispatch_center.md) §5.6)이라
  규격 ambient listening(TS 24.379 원격·로컬 개시 1:1 호, anyExt `<allow-request-remote-/locally-initiated-ambient-listening>`)과 다른 것이다.
  §8.3.2.1 이 "shall" 로 요구하는 긴급 요소(8d ii·8e ii~iv·10f)는 **대상 미지정에도 항상 싣고**, 미지정은 entry-info 로 표현한다
  (그룹 `UseCurrentlySelectedGroup` + 폴백 uri-entry, 사설 `LocallyDetermined` + 폴백 uri-entry); 개시 인가는 요소 유무가 아니라
  `<cp:ruleset>` allow-* 가 말한다(DedicatedGroup 모드 긴급그룹 미지정 → 그룹 긴급·경보 false, UsePreConfigured 모드 수신자 미지정 →
  긴급 사설콜 false — CSP 403 판정과 일치). 해제 인가(`allow-cancel-group-emergency`·`allow-cancel-imminent-peril`·
  `allow-cancel-emergency-alert` — TS 24.379 §6.3.3.1.13.3·.4·.6)는 대상 결정과 AND 하지 않고 `ptt_user_profile.allow_cancel_*` 그대로 싣는다.
  규격상 **선택**이지만 필수로 읽는 단말이 있어 항상 싣는 것 = alias-entry 의 `index`·`xml:lang`, `<ParticipantType>`(§8.3.2.1 f). 값은 `UserProfile.ParticipantType`·`UserProfile.Language` 설정이고, `xml:lang` 은 `<Name>` 과 같은 값을 써 한 문서 안에서 어긋나지 않는다. 소유-비멤버 그룹과 자체 JSON 목록(`GET …/groups/users/{me}`)은
  전환기 공존 — 클라이언트가 MCPTTGroupInfo 로 옮기면 JSON 목록 제거([mcx_identity_scope.md](mcx_identity_scope.md) 와 같은 방식).
- **XCAP 문서 주소**(`tests/test_csc_xcap_addresses.py`) — 규격 주소와 CIMS 단말의 옛 주소를 둘 다 받고 같은 문서를 준다:
  service configuration = 전역 문서 `…/org.3gpp.mcptt.service-config/global/[<mc-org-name>/]service-config.xml`(TS 24.484 §8.4.2.8·§8.4.2.9 —
  옛 주소 `…/users/<XUI>/service-config`), user profile = `…/users/sip:<MCPTT ID>/mcptt-user-profile-1.xml`(§8.3.1A·§8.3.2.8 — 옛 이름
  `user-profile`, 다른 이름·index 404), 그룹 문서 = `…/org.openmobilealliance.groups/global/byGroupID/<그룹 ID>`(TS 24.481 §7.2.10.2 — users tree 와
  같은 문서·같은 인가) + **멤버를 뺀 조회** POST GMOP `<get-excluding-memberlist>`(§6.3.16 — `<list>` 없이). 설정 문서는 읽기 전용 —
  GET 밖의 메서드는 405(문서 생성·수정·삭제는 §0-R R4-2). xcap-diff NOTIFY 의 `sel` 은 CSP 가 싣는다(옛 주소 — conformance_gap_plan.md S19).
- **S4 service-config**: 문서 = TS 24.484 §8.4.2.1·§8.4.2.3 스키마 — `<service-configuration-info>` ›
  `<service-configuration-params domain=<PTT 도메인>>` › `<common><broadcast-group>`(계층 수) · `<on-network>`
  (`<emergency-call><group-time-limit>` 선택 — 첫 자식, 진행 중 긴급 그룹 호 시한 = CSP TNG2(TS 24.379 §6.3.3.1.16), 값이 0 이면 생략 ·
  `<private-call>` 선택 — 개별 호 T4 `<hang-time>`·`<max-duration-with-floor-control>`·`<max-duration-without-floor-control>`(TS 24.379
  §6.3.8.2), 0 인 자식 생략 · `<transmit-time><time-limit>` · `<fc-timers-counters>` 17 요소 필수 · `<emergency-/imminent-peril-/normal-resource-priority>`
  필수, 각 namespace·priority · `<signalling-protection>` 의 `<confidentiality-protection>`·`<integrity-protection>` = false(없으면 true 로 읽혀
  단말이 mcptt-info 를 CSK 로 암호화·서명한다 — §8.4.2.6·TS 24.379 §6.6.2.3.1·§6.6.3.3.1. CIMS 는 시그널링 XML 보호를 하지 않고 구간 보호는
  SIP TLS) · `<protection-between-mcptt-servers>` 의 `<allow-signalling-protection>`·`<allow-floor-control-protection>` = false(서버 간 보호 —
  없으면 true, 서버 간 연동 없음) · `<anyExt><adhoc-group-call>` — 필수 `<allow-adhoc-group-call-support>`·`<max-no-participants>` 뒤 T4
  `<hang-time>`·일제 T4 `<broadcast-hang-time>`·TNG3 `<max-duration-of-call>`(§17.4.2.2 13)). 요소가 없으면 «애드혹 미지원»(§8.4.2.6)이라
  늘 싣는다). 값 = CSC 설정 `ServiceConfig.PrivateCall.*`·`ServiceConfig.AdhocGroupCall.*`(시간 ms, 0 = 요소 생략 = 미가동).
  CSP 는 `<private-call>`·`<adhoc-group-call>` 의 시간 값을 개별·애드혹 세션의 T4(`floor_timers.t4_inactivity`)·최대 시간으로 쓴다.
  `<allow-adhoc-group-call-support>`·`<max-no-participants>` 는 단말에 알리는 값이고 CSP 판정에는 아직 쓰지 않는다 — 애드혹 개시
  게이트는 csp.json `Setup.PttAdhocEnabled`(두 값을 같게 둔다), 인원 상한 403 + Warning `189`(§17.4.2.2 6))·미지원 403 + `186`(§17.4.2.2 5))
  은 미구현.
  값의 정본은 두 곳 — DB `mcptt_service_config` **단일 행**(id=1: N2 = user-profile `MaxAffiliationsN2` 기본값·N6 두 값(관제/그 밖 —
  user-profile `MaxSimultaneousCallsN6`, 열은 `sql/migrate_mcptt_n6.sql`, 열이 없는 DB 는 10·5)·계층 수, 관리 API
  `GET/PUT /api/v1/mcptt/service-config`·콘솔 **구성 > MCPTT 정책**)과 CSC 설정 `ServiceConfig.*`(`EmergencyCall.GroupTimeLimit` →
  `<emergency-call><group-time-limit>`(ms, 기본 0 = 없음) · `<transmit-time><time-limit>`·
  `<fc-timers-counters>` — floor 제어 서버 파라미터, Resource-Priority — RFC 8101 `mcpttp` 15/8/0 = CSP fan-out). `get_service_config_xml`
  이 둘을 산출한다(내용 파생 ETag). **인가 요소는 없다** — 1:1·긴급·경보·그룹 생성 인가는 `user-profile` 의 `ruleset`·그룹 문서가 규격 자리다.
  **floor 파라미터는 이 문서가 정본이다** — MCPTT 서버(CSP)가 문서를 CMS(CSC)에서 받아(Annex A.2.3, 내부 API
  `GET /internal/mcptt/service-config` — 기동·SIGUSR1·CSC_RESTART·SERVICE_CONFIG_CHANGED, `CCspServiceConfig`) 그룹 세션의
  `PTT_GROUP_ADD/MODIFY.floor_timers`(T1·T2·T3·T7·T8·T20·C7·C20)로 CMP 에 싣는다. CMP 설정 `Floor*Sec` 는 문서를 못 받았을 때의 폴백이다.
  CSC 설정 재적재(SIGUSR1)로 문서가 바뀌어도 `SERVICE_CONFIG_CHANGED` 가 나간다.
  전역 변경은 CSC 가 `SERVICE_CONFIG_CHANGED` 를 발행하고 CSP 가 cms 구독자 **전원**에게
  xcap-diff NOTIFY 를 push 한다(`GetSubscriptionsByEvent("cms")` — 전역 문서라 사용자/자원 키가
  없는 유일한 전체 조회). 구독이 없는 단말은 목록 갱신·재로그인 계기의 재조회로 반영된다.
- **S3 변경통지**: 가입자(번호) CRUD 시 `notify_csp("USER_CHANGED")` → CSP `SendUserDocNotify`
  → CMS 구독자에 xcap-diff NOTIFY(user-profile/service-config sel). UE initial configuration 이 바뀌면 `UE_INIT_CONFIG_CHANGED` →
  `SendUeInitConfigNotify` → cms 구독 단말마다 그 단말의 ue-init-config 선택자(§R4-1).
- **단말 소비**: PTT 단말은 `sip:cms_psi@<domain>` 으로 cms 축을 구독하고 NOTIFY 의 sel 대로 두 문서를
  `If-None-Match` 재조회한 뒤, 사용자별 인가(`user-profile` 의 `ruleset`)로 게이트한다(발신·개시만, 착신은 서버 판정).
  `service-config` 에서는 Resource-Priority 값을 쓴다. 소비 지점 표는
  [android_ue_client.md §7](android_ue_client.md) "CMS 문서 소비".

### KMS (TS 33.180 §F)
- **S5 가입자별 프로비저닝**: `KmsInit`(KMS 공개 인증서) + `KmsKeyProv`(가입자별 KmsKeySet —
  UserDecryptKey/UserSigningKeySSK/UserPubTokenPVT, `get_kms_keyprov_xml`). 키 material 은 KMS master
  secret 에서 가입자별로 파생(HKDF 유사, 재현 가능·사용자마다 상이).
- ⚠ **후속**: 파생값은 구조적 placeholder 이며 참 ECCSI/SAKKE(RFC 6507/6508) 점이 아니다. 실제
  pairing 기반 키파생은 전용 암호 라이브러리가 필요하며 E2E 암호화 도입 시 진행한다.
  남은 개발 항목(KMS 실구현·GMK·CSK·E2E 미디어·단말) 정본 = [mcx_e2e_security.md](mcx_e2e_security.md).

---

## 4. 단말 정합

각 항목은 **단말(`android/ptt-client`)과 동일 규약**으로 맞춘다(코덱·XML·엔드포인트는 단말 구현이
TS 기준). 변경 시 본 문서와 [android_ue_client.md](android_ue_client.md)·[ptt_flows.md](ptt_flows.md) 를 함께 갱신한다.

> **배포 메모**: OAM 게이트웨이 뒤 배포 시, S1 디스커버리(`/.well-known/openid-configuration`)는
> 게이트웨이 라우트로 csc 에 프록시되어야 단말이 off-box 에서 발견할 수 있다(standalone csc 는 직접 서빙).
