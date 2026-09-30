# MCVideo — 그룹 영상 서비스 (TS 24.281 · TS 24.581)

> **그룹 영상의 정본 설계.** 3GPP 는 영상을 MCPTT 세션에 얹지 않고 별도 MC 서비스 **MCVideo** 로 둔다. 한 그룹을
> MCPTT 와 MCVideo 두 서비스용으로 설정하면(TS 23.280 §3 «MC service group … configured for the use with one or more MC services»),
> 단말은 같은 그룹에서 **음성만 = MCPTT 그룹 호**, **음성+영상 = MCVideo 그룹 호**를 골라 쓰고 둘 사이를 오간다.
> 이 문서는 규격 모델, 현행 «PTT 영상»(MCPTT 세션의 `m=video` — 비규격)과의 차이, 규격형으로 옮기는 개발 항목·결정 사항을 정한다 —
> **설계 정본.** 구현된 것 — 계약(전송 제어 정의 테이블 [mcvideo_tc_defs.yaml](mcvideo_tc_defs.yaml)(생성 헤더 양 끝, §5.3·§5.4) · DB 표(§5.1) ·
> 설정 문서 골든 `tests/fixtures/mcvideo/` · SDP 프로파일(§1.4) · CSP↔CMP 제어 API([cmp_media_api.md](../../api/cmp_media_api.md) §7.9) · 단말 SDK 공개
> 표면([ue_sdk.md](ue_sdk.md) §4.6)), 양 끝 전송 제어 코덱(CMP `PTransmissionCodec` · SDK `mcvideo/tc_codec`, 교차 시험), 단말 전송 제어 참여자 상태 머신
> (SDK `mcvideo/tc_participant`), V0 전부, CSC 설정 평면·관리 API(§5.1 — 콘솔 제외)과 그 문서들의 SDK 해석(§5.4), CSP 호 제어 부품·모듈·서비스 판별·
> 등록 능력·서비스별 affiliation(§5.2), CMP 그룹 종류·멤버 포트·제어 명령·송출·수신 제어 상태 머신·미디어 분배·보호(SRTP·전송 제어 SRTCP)·영상 RTCP 키프레임 요청(§5.3·§5.3.1),
> 단말 SDK 등록 태그·affiliation·그룹 호(개시·재합류·멤버 초대 수락)·전송 제어 결선·송출 게이트(§5.4 — 루프백 시험, 실서버 미연동), CSP 그룹 호
> (§5.2.1 — chat·prearranged 개시·합류·재합류·해제, 실측 전). 녹취, 단말 영상 송출·송출별 렌더(C6)는 미구현(바인딩 C7 은 구현 —
> .NET 빌드·시험은 Windows).
>
> 규격 판본: TS 24.281 V18.14.0 · TS 24.581 V18.8.0 · TS 23.281 V18.12.0 · TS 24.481 V19.3.0 · TS 24.484 V20.0.0 · TS 23.280 V20.4.0 ·
> TS 33.180 V20.0.0. 관계 문서: 로드맵 표 [mcptt_standard_conformance.md](mcptt_standard_conformance.md) R3·R6, 현행 PTT 영상 협상
> [ptt_flows.md](ptt_flows.md), CMP 영상 분배 [../modules/cmp.md](../modules/cmp.md), 단말 영상 [ue_sdk.md](ue_sdk.md) §4.5, 신원·scope
> [mcx_identity_scope.md](mcx_identity_scope.md), 종단간 보안 [mcx_e2e_security.md](mcx_e2e_security.md).

## 1. 규격 모델 요약

### 1.1 서비스 구조

| 항목 | MCPTT (음성) | MCVideo (음성+영상) |
|---|---|---|
| 호 제어 | TS 24.379 | TS 24.281 |
| 미디어 제어 | floor control (TS 24.380, RTCP APP `MCPT`) | transmission control · reception control (TS 24.581, RTCP APP `MCV0`·`MCV1`·`MCV2`) |
| ICSI | `urn:urn-7:3gpp-service.ims.icsi.mcptt` | `urn:urn-7:3gpp-service.ims.icsi.mcvideo` (TS 24.281 Annex E.2.1) |
| 특성 태그 | `+g.3gpp.mcptt` | `+g.3gpp.mcvideo` (Annex D.2) |
| SIP 본문 | `application/vnd.3gpp.mcptt-info+xml` (`urn:3gpp:ns:mcpttInfo:1.0`) | `application/vnd.3gpp.mcvideo-info+xml` (`urn:3gpp:ns:mcvideoInfo:1.0`, 루트 `<mcvideoinfo>` — Annex F.1) |
| 세션 미디어 | `m=audio`(speech) + `m=application … udp MCPTT` | `m=audio`(`i=audio component of MCVideo`) + `m=video`(`i=video component of MCVideo`) + `m=application … udp MCVideo` (TS 24.281 §6.2.1, TS 24.581 §4.3.3.1) |
| affiliation | presence PUBLISH, `mcpttPresInfo` | 같은 방식, ICSI mcvideo · `urn:3gpp:ns:mcvideoPresInfo:1.0` (TS 24.281 §8.2·§8.3.1) — **서비스별**(TS 23.280 §5.2.5) |
| 참여 기능 PSI | ue-init-config `MCPTT-Service-Details/Server-URI` | `MCVideo-Service-Details/Server-URI` (TS 24.484 §7.2.2.1) |

두 서비스 사이를 옮기는 절차는 **규격에 없다** — ICSI·세션·제어 기능·affiliation 이 모두 따로다. MCPTT 호와 MCVideo 호는 하나의
REGISTER 를 공유하는 독립 다이얼로그이고(TS 24.281 §7.1 «shares the same SIP registration»), 두 호를 함께 드는 것도, 하나를
나와 다른 하나로 가는 것도 단말 동작이다. 두 호의 소리를 함께 들을 때의 처리도 규격이 정하지 않는다(단말 정책 — §7 D6).

### 1.2 신원 · 등록 · 서비스 인가

- **MC service ID 하나** — MC 서비스 제공자가 전 서비스에 한 ID 를 쓰면 MCVideo ID = MCPTT ID 이고, 요청은 서비스 표시(ICSI)로
  가른다(TS 23.280 §10.1.4.1). CIMS 는 이미 `mcdata_id = mcptt_id` 이므로 MCVideo 도 같은 값이다(§7 D1).
- **REGISTER** — Contact 에 `+g.3gpp.mcvideo` 와 `+g.3gpp.icsi-ref` 의 mcvideo ICSI 를 MCPTT 것과 함께 싣는다. MCVideo 에서 로그오프 =
  태그를 뺀 재-REGISTER(TS 24.281 §7.2.1). 서비스 인가 = REGISTER 의 mcvideo-info `<mcvideo-access-token>`·`<mcvideo-client-id>`(§7.2.1) 또는
  `Event: poc-settings` PUBLISH(§7.2.1A·§7.2.2). 서버는 MCVideo ID·client ID 를 IMPU 에 묶는다(§7.3.2).
- **요청마다** — Contact 태그, Accept-Contact `+g.3gpp.mcvideo`·ICSI(require;explicit), P-Preferred-Service, Request-URI = 참여 MCVideo
  기능 PSI(§4.2, §9.2.1.2.1.1).
- **토큰 scope**(TS 33.180 Annex B) — `3gpp:mc:video_service` · `3gpp:mc:video_group_management_service` ·
  `3gpp:mc:video_config_management_service` · `3gpp:mc:video_key_management_service`.

### 1.3 그룹 문서 (TS 24.481)

- 한 그룹 문서가 MCPTT 그룹이자 MCVideo 그룹일 수 있다(§7.2.8 «any combination»). `<supported-services>` 에 서비스마다 `<service>` 하나:
  - MCPTT — `enabler` = **MCPTT ICSI**, `<group-media>` 에 `<mcptt-speech>`(§7.2.2).
  - MCVideo — `enabler` = **MCVideo ICSI**, `<group-media>` 에 `<mcvideo-video-media>`(§7.2.2).
- MCVideo `<list-service>` 속성(§7.2.2, 의미 §7.2.8, 네임스페이스 `urn:3gpp:ns:mcpttGroupInfo:1.0`): `mcvideo-on-network-invite-members`
  (true = prearranged, false·없음 = chat) · `mcvideo-on-network-maximum-duration` · `mcvideo-protect-media` · `mcvideo-protect-transmission-control` ·
  `mcvideo-preferred-audio-encodings` · `mcvideo-preferred-video-encodings` · `mcvideo-preferred-video-resolutions` · `mcvideo-preferred-video-frame-rate` ·
  실시간 모드 넷(urgent·non-urgent·non-real-time·active) · `mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members` ·
  `mcvideo-on-network-minimum-number-to-start` · `mcvideo-on-network-group-priority` · `on-network-reception-hang-timer`.
- 멤버 entry — `<mcvideo-mcvideo-id uri>`(필수) · `mcvideo-on-network-required` · `user-priority` · `user-reception-priority` · `participant-type`.
- 규칙 action — `mcvideo-allow-emergency-call`·`-emergency-alert`·`-imminent-peril-call` · `mcvideo-on-network-allow-conference-state` ·
  `-getting-affiliation-list`.
- ⚠️ **보호 기본값** — `mcvideo-protect-media`·`mcvideo-protect-transmission-control` 은 **요소가 없으면 "true"**(GMK 로 미디어·제어를
  보호해야 한다, §7.2.8). 종단간 키가 없는 동안은 **"false" 를 명시**해야 규격 단말이 호를 연다(§7 D7).

### 1.4 호 제어 · SDP (TS 24.281)

| 호 | 조항 | 요지 |
|---|---|---|
| prearranged 그룹 호 | §9.2.1 (단말 .2, 참여 .3, 제어 .4) | INVITE `session-type` prearranged, 제어 기능이 affiliated 멤버를 초대(§6.3.5.5), 나가기 = BYE(§6.2.4.1), 재합류 = 세션 식별자로 INVITE(§9.2.1.2.4) |
| chat 그룹 호 | §9.2.2 | INVITE `session-type` chat — 원하는 사람만 들어온다, 합류가 암묵적 affiliation(§8.1) |
| 그룹 종류 검사 | §6.3.5.2 | invite-members true 는 prearranged 만(아니면 404 "117"), false 는 chat 만(404 "118") |
| conference 이벤트 | §9.2.3 | SUBSCRIBE — 선택(may) |
| 긴급·임박·경보 | §4.6, §6.2.8.1.x, §11.2 | re-INVITE 상향·해제, MESSAGE 경보 — MCPTT 와 같은 모양, mcvideo-info 지시자 |
| 1:1 · 방송 · pull · push · ambient viewing · ad hoc | §10.2 · §6.2.8.2 · §12.2 · §13.2 · §15 · §22 | 후속(§6 V8) |

- **SDP offer**(§6.2.1): `m=audio`(코덱 = 그룹 `mcvideo-preferred-audio-encodings`, 구현 기본 AMR-WB) · `m=video`(코덱 = `mcvideo-preferred-video-encodings`,
  구현 기본 H.264) · 전송 제어를 쓰면 `m=application <RTCP 포트> udp MCVideo` + `a=fmtp:MCVideo …`(TS 24.581 §4.3.3.1). answer 규칙 §6.2.2,
  참여 기능의 IP·포트·`mc_transmission_ssrc` 재작성 §6.3.2.1.1.1.
- **fmtp**(TS 24.581 §12.1.2·§14): `mc_queueing` · `mc_priority`(1~255) · `mc_reception_priority` · `mc_granted` · `mc_implicit_request` · `mc_audio_ssrc` ·
  `mc_video_ssrc` · `mc_transmission_ssrc`. answer 는 파라미터를 더하지 않는다(§14.3.1), 제어 기능 `mc_priority` = min(offer, `<user-priority>`, 계층 수)
  (§14.3.3), 암묵적 요청은 chat 합류·진행 중 prearranged 합류에서 받지 않는다(§14.3.5).
- **CIMS SDP 프로파일(계약 K4)** — 골든 = `tests/fixtures/mcvideo/sip/`(K3 메시지 안의 SDP).
  - **단말 offer**(개시·합류·재합류): m-line 순서 audio → video → application. audio = `i=audio component of MCVideo` · AMR-WB(그룹 선호 코덱) ·
    telephone-event. video = `i=video component of MCVideo` · H.264(`packetization-mode=1`) · `a=rtcp-fb:<pt> nack pli`·`a=rtcp-fb:<pt> ccm fir`.
    제어 채널 = `m=application <RTCP 포트> udp MCVideo`(proto 소문자 `udp`, TS 24.581 표 4.3.3.1-1) + `a=fmtp:MCVideo …` — 구분자 `;`(§9),
    **`mc_transmission_ssrc` 를 값과 함께 싣는다**(서버가 이 단말에게 보내는 전송 제어 RTCP 헤더 SSRC). 암묵적 송출 요청이면 `mc_implicit_request`·
    `mc_granted`, 선택으로 `a=ssrc`(RFC 5576).
  - **서버 answer**: m-line 수·순서 = offer(RFC 3264 §6), 주소 = CMP 멤버 포트(`port`·`video_port`·`control_port` — cmp_media_api.md §7.9), PT·코덱 fmtp·
    `i=` echo. fmtp 는 offer 에 있던 것만(§14.3.1): `mc_priority` = min(offer, `<user-priority>`)(§14.3.3 — 계층 수 요소는 off-network 전용이라 쓰지
    않는다), **`mc_transmission_ssrc` = CMP `tc_ssrc` 를 늘 싣는다**(TS 24.281 §6.3.3.2.1 2)b) "shall" — §9), `mc_queueing` 은 싣지 않는다(1차 송출
    큐 없음 — §14.3.2 "지원할 때"), 암묵적 요청을 받아들인 새 prearranged 세션에만 `mc_implicit_request` + `mc_audio_ssrc`·`mc_video_ssrc`(+ 허가 시 `mc_granted`).
  - **서버 fan-out offer**(prearranged 초대, TS 24.281 §6.3.3.1.1): 같은 세 m-line·`i=`, fmtp = `mc_priority=<user-priority>`(§14.2.3) ·
    `mc_transmission_ssrc=<tc_ssrc>`(§6.3.3.1.1 4)). `mc_granted`·`mc_implicit_request` 는 싣지 않는다.
  - **SRTP**: TLS 접속 + 접속서비스 `media_srtp` 면 audio·video 둘 다 `RTP/SAVP` + m-line 별 `a=crypto`(RFC 4568 — 영상 SRTP 를 처음부터, B7). 제어 채널
    보호(SRTCP)는 CSK·GMK 기반이라(TS 33.180) 1차는 평문 RTCP다(`mcvideo-protect-transmission-control` false — §7 D7).

### 1.5 전송 제어 · 수신 제어 (TS 24.581)

MCPTT floor 는 «한 사람이 말하면 모두 듣는다». MCVideo 는 **송출 허가**와 **수신 허가**를 따로 둔다.

- **송출** — 참여자가 Transmission Request → 서버가 Granted / Rejected / Queue Position Info. 동시 송출 상한 = 그룹
  `mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members`(§4.1.1.1·§6.3.4). 상한에서 더 높은 우선순위 요청이면 가장 낮은 송출을
  Revoked(§4.1.1.2). 한 송출 = RTP 두 흐름 — Granted 에 송출자의 **Audio SSRC 와 Video SSRC** 가 함께 실린다(필드 14·23, §9.2.3).
- **수신** — 새 송출은 Media Transmission Notification 으로 알린다. Reception Mode '1'(manual)이면 사용자가 [영상 받기] → Receive Media Request
  → Receive Media Response, 그만 보기 = Media Reception End Request. 서버는 수신자마다 **Active SSRC List** 를 둔다(§6.3.7). 긴급·임박·방송·
  system 호는 Reception Mode '0'(automatic — 곧바로 수신, §6.3.6.3.3). 수신자 동시 스트림 상한 = user profile `<MaxSimultaneousVideoStreams>`.
- **RTCP APP 이름**(§9.1.2): `MCV0` 단말→서버(Transmission Request 0000·Release 0010·Queue Position Request 0011·Receive Media Request 0100·
  Remote Transmission Request 0111·Remote Transmission Cancel 1000) · `MCV1` 서버→단말(Granted 0000·Rejected 0001·Revoked 0100·Queue Position Info 0101·
  Media Transmission Notification 0110·Receive Media Response 0111·Media Reception Notification 1000·Transmission End Notify 1110·Transmission Idle 1111 …) ·
  `MCV2` 양방향(Transmission End Request 0000·Response 0001·Media Reception End Request 0010·Response 0011·Transmission Control Ack). 첫 비트 = 확인 요구.
- **상태 머신** — 참여자 송출(§6.2.4)·수신(§6.2.5), 서버 전체 송출(§6.3.4 `G: Transmit Idle/Taken/pending Revoke`)·참여자별(§6.3.5)·수신(§6.3.6·§6.3.7),
  참여 기능 전달·SSRC 재작성(§6.4.2).
- **타이머**(§11) — 단말 T100~T104·C100~C104, 서버 T1 비활성(그룹 hang timer 30 s)·T2·T3 revoke 1 s·T4 granted 1 s·T5 수신 비활성
  (`on-network-reception-hang-timer` 30 s)·T6·T11 스트림 수신 유휴 10 s, 카운터 C2·C4·C6·C7·C9·C11. 서버 값 정본 = service configuration `<tc-timers-counters-R14>`.
- **정의 정본** — 메시지 subtype·메시지별 필드·field id·값 모양·원인·타이머/카운터 기본값·fmtp 이름은 [mcvideo_tc_defs.yaml](mcvideo_tc_defs.yaml) 한 곳이다.
  `scripts/gen_mcvideo_tc_defs.py` 가 CMP(`cmp/PTransmissionDefs.h`)·SDK(`sdk/core/src/mcvideo/tc_defs.h`) 헤더를 함께 내고 `--check`(S1
  `S1-UE-MCVIDEO-TC-DEFS`)가 최신성을 본다. 규격에 기본값이 없는 T100~T104·T2 는 CIMS 값(1 s)을 테이블에 `origin: cims` 로 적었다.

### 1.6 설정 문서 (TS 24.484)

| 문서 | 루트 · AUID · MIME | 쓰는 것 |
|---|---|---|
| user profile (§9.3) | `<mcvideo-user-profile>` · `org.3gpp.mcvideo.user-profile` · `application/vnd.3gpp.mcvideo-user-profile+xml` (`urn:3gpp:ns:mcvideo:user-profile:1.0`) | `<MCVideoUserID>` · `<MCVideoGroupInfo>` · `<MaxAffiliationsN2>` · `<ImplicitAffiliations>` · `<MaxSimultaneousCallsN6>` · `<MaxSimultaneousVideoStreams>` · ruleset(allow-private-call · 긴급·임박·경보 발령/해제 · allow-revoke-transmit …, anyExt ambient viewing · ad hoc) |
| service configuration (§9.4) | `<service-configuration-info>` · `org.3gpp.mcvideo.service-config` · `vnd.3gpp.mcvideo-service-config+xml` (`urn:3gpp:ns:mcvideoServiceConfig:1.0`) | on-network 긴급·임박·일반 resource-priority(필수) · `<tc-timers-counters-R14>` · 보호 요소 |
| UE initial configuration (§7.2) | 기존 `org.3gpp.mcptt.ue-init-config` | `<on-network><anyExt><MCVideo-Service-Details>` — `<IPv6-Required>` · `<Server-URI>` |

그룹 호 개시 인가는 user profile 요소가 아니라 local policy + 그룹 문서 규칙이다(TS 24.281 §9.2.1.3.1.1 3), §6.3.5.3~.4).
service configuration 에서 `<confidentiality-protection>`·`<integrity-protection>` 은 빠지면 켜진 것으로 읽힌다(TS 24.281 §6.6.2.1·§6.6.3.1) — 명시 false.

### 1.7 이용 형태 — 사용 시나리오 (TS 23.281 §7)

현장 요원 = PTT 단말 앱, 관제사 = 관제 앱. 1차 범위(V2~V5)는 그룹 영상 호 두 가지, 나머지는 V8.

| 시나리오 | 흐름 요지 | 수신 | 규격 | 단계 |
|---|---|---|---|---|
| 현장 영상 공유 — 무전 중 영상을 켠다 | 음성 무전(MCPTT 호) 중 [영상 참여] = 그룹 MCVideo chat 호 합류(암묵적 affiliation) → [영상 보내기] = 송출 요청·허가 → 동료·관제는 «새 영상» 알림에 [받기] → [영상 나가기] 뒤 음성 무전 유지 | manual | TS 24.281 §9.2.2 · TS 23.281 §7.7.1.3.1~.2 | V2~V5 |
| 여러 카메라 동시 송출 — 관제가 골라 본다 | 동시 송출 상한(그룹 속성) 안이면 모두 허가, 상한이면 거절 또는 우선순위 revoke. 관제는 볼 스트림만 [받기]·[그만 보기], 수신 상한에서 새 영상이 오면 교체 선택 | manual | TS 24.581 §4.1.1.2 · TS 23.281 §7.7.1.3.2C~D·§7.7.1.3.3 | V3 · V5 |
| 원격 송출 요청 | 그룹 영상 호 중 관제가 특정 요원에게 송출을 원격 요청·종료 | manual | TS 23.281 §7.7.1.3.7 (MCV0 Remote Transmission request) | V8 |
| 영상 가져오기(1:1 pull) | 관제가 요원 한 명의 영상을 요청 — 상대만 송출하는 1:1 호, 송출이 끝나면 종료 | — | TS 23.281 §7.3.2.3 · TS 24.281 §12.2 | V8 |
| 원격 영상 보기(ambient viewing) | 권한자(관제)가 요원 단말 카메라를 연다 — 단말은 표시 없이 자동 수락·송출. 반대 방향(요원이 표시 없이 관제에게)도 있다 | — | TS 23.281 §7.6 · TS 24.281 §15 (user profile 인가) | V8 |
| 그룹에 원격 영상 보내기 | 관제가 원천 사용자의 영상을 그룹 전체에 뿌리도록 요청 — 원천만 송출하는 방송형 그룹 호 | — | TS 23.281 §7.4.2.6 | V8 |
| 긴급 영상 호 | SOS → 긴급 그룹 영상 호 또는 진행 중 호 긴급 상향, 그룹원은 [받기] 없이 바로 수신, 긴급 우선순위 | automatic | TS 23.281 §7.1.2.5 · TS 24.581 §6.3.6.3.3 | V8 |
| 방송 영상 호 | 지휘관 일방 송출, 그룹원 바로 수신 | automatic | TS 23.281 §7.1.2.4 · TS 24.281 §6.2.8.2 | V8 |
| 1:1 영상 통화 · 영상 보내기 | 1:1 영상 호(자동·수동 수락), 한 방향 보내기(1:1 push — 끝나면 종료) | — | TS 23.281 §7.2 · §7.4.2.3 | V8 |
| 서버에 올리기 · 저장 영상 보기 | 서버가 받은 영상을 파일로 기록(one-to-server push), 저장 파일 스트리밍(one-from-server pull) — CIMS 녹취와 연결 | — | TS 23.281 §7.4.2.4 · §7.3.2.4 | V8 |
| 망 상태에 맞춘 화질 조정 | 손실·지연 감지 → 코덱·해상도 등 통신 파라미터 변경 요청 | — | TS 23.281 §7.17 | V8 |

## 2. 현재 구현 — «PTT 영상»과 규격의 차이

현행은 영상을 **MCPTT 그룹 세션 안의 `m=video`** 로 싣는다([ptt_flows.md](ptt_flows.md) «영상 협상»). floor 보유자의 영상을 멤버별 영상 포트로
나누고 녹취한다. MCVideo 는 전 구간 미구현이다.

| 구간 | 현행 | 규격(MCVideo) |
|---|---|---|
| 그룹 설정 | DB `ptt_groups.video_enabled` 하나가 «PTT 영상»을 켠다. 그룹 문서 `<mcpttgi:mcptt-video>` — **TS 24.481 스키마에 없는 요소를 3GPP 네임스페이스에** 싣는다(V7 까지 전환기 요소). MCVideo 설정 평면(§5.1)은 그와 따로 선다 | MCPTT·MCVideo `<service>` 각각 ICSI enabler + `<mcvideo-*>` 속성 |
| 영상 유무 | 호 개시 때 한 번 — 앱은 늘 `video=true` 로 제안, 서버가 `video_enabled` 그룹만 받는다. 진행 중 추가·제거 없음 | 음성 = MCPTT 호, 음성+영상 = MCVideo 호(따로 합류·퇴장) |
| 송출 | floor 보유자만(single/dual/multi-talker 최대 8). 앱은 «내 영상 보내기» 켜기/끄기만(`setVideoSend`, 재협상 없음) | 송출 요청·허가(Transmission Request/Granted), 동시 송출 상한 |
| 수신 | 영상 그룹 멤버 전원 자동 수신 | 수신자가 스트림을 골라 받는다(manual), 긴급·방송은 자동 |
| 서비스 신원 | CSC 설정 평면(scope·user profile·service config·ue-init-config)은 있다(§5.1). CSP 가 REGISTER·요청의 MCVideo ICSI·특성 태그를 아직 보지 않는다 | §1.2·§1.6 |

**현행 PTT 영상의 알려진 결함**(코드 조사 — 전환 기간에도 남는다):

- 그룹 영상은 평문이다 — CSP `CmpClient::JoinGroup` 이 PTT_JOIN 에 `media_crypto_video` 를 싣지 않고, psip 합성 SDP 는 SRTP(SAVP) leg 의 video 를
  port 0 으로 거절한다(`ext/psip/SipDialog.cpp` 350~381). CMP 쪽 영상 SRTP 는 준비돼 있지만 쓰이지 않는다.
- CMP 가 PTT_GROUP_ADD `video_enabled` 를 읽지 않는다 — 분배 여부는 PTT_JOIN `user_video_port` 유무로만 정해진다([../api/cmp_media_api.md](../api/cmp_media_api.md) §7.1).
- 그룹 경로에 영상 RTCP 소켓이 없어 수신자의 PLI 가 송출자에게 가지 않는다(cmp.md — 키프레임은 송출 개시 IDR 에만 기댄다).
- C API·.NET 의 그룹 호 옵션에 영상이 없다(`cimsue_group_call_options_t`·`GroupCallOptions`, `set_video_send` 없음) — Kotlin 만 있다.

## 3. 설계 원칙

1. **규격 구조 그대로** — MCVideo 는 MCPTT 의 확장이 아니라 **나란한 서비스**다. 한 그룹 = 서비스 집합(MCPTT·MCVideo·MCData), 서비스마다 자기
   ICSI·문서 요소·affiliation·호·미디어 제어를 갖는다. MCPTT 세션에 영상을 얹는 방식은 새로 넓히지 않는다.
2. **재사용은 구현 층에서** — CMP 멤버별 포트 단위·SRTP·PT 재스탬프·NAT latch·슬롯 녹취, CSP 그룹 세션·fan-out·CMP 명령·구독, CSC XCAP·
   토큰·scope·문서 생성 틀, SDK 영상 캡처·렌더·PLI 는 그대로 쓰고, **규격 경계(ICSI·본문·SDP·RTCP APP·문서)는 서비스별로** 둔다.
3. **서비스 축을 명시** — 모듈 역할(`Setup.Roles.MCVIDEO`), CMP 그룹 종류(`service: mcvideo`), 로그·통계·녹취 색인의 서비스 축(`mcvideo`),
   DB 의 서비스별 표를 둔다. `video_enabled` 같은 겸용 플래그는 만들지 않는다.
4. **단계마다 규격 단말과 붙는다** — 각 단계의 산출은 규격 요소만으로 동작해야 한다(자체 확장 없음). 규격 불일치(§9)는 판본 원문을 따르고
   편차 표에 적는다.

## 4. 목표 흐름 — 같은 그룹에서 음성과 영상

```
단말(MCPTT+MCVideo 클라이언트)            CSP (PTT-AS · MCVIDEO-AS)                 CMP
  │ REGISTER Contact +g.3gpp.mcptt;+g.3gpp.mcvideo;icsi-ref="…mcptt","…mcvideo"
  │ (mcptt-info + mcvideo-info access token)  ─────────▶ 200 (MCPTT ID = MCVideo ID)
  │ PUBLISH affiliation g002 (mcptt)           ─────────▶ 200     ← 음성 호를 받는다
  │ PUBLISH affiliation g002 (mcvideo, mcvideoPresInfo) ▶ 200     ← (prearranged 면) 영상 호 초대 대상
  │
  │ ── [PTT] = MCPTT 그룹 호 (현행과 같다) ──────────────────────────────────────────
  │
  │ ── [영상 참여] ───────────────────────────────────────────────────────────────
  │ INVITE sip:<mcvideo PSI>  mcvideo-info session-type=chat, request-uri=tel:g002
  │   SDP: m=audio(AMR-WB) m=video(H.264) m=application udp MCVideo
  │                                             ─ MCVIDEO_GROUP_ADD/JOIN(audio·video·control) ▶
  │ ◀──────────────── 200 (SDP: 멤버 전용 CMP 포트)
  │
  │ [영상 보내기] RTCP APP MCV0 Transmission Request ─────────────────────────────▶
  │ ◀──────────────────────────────── MCV1 Transmission Granted (Audio SSRC · Video SSRC)
  │ RTP audio + video ───────────────────────────────────────────────────────────▶ 수신 허가된 멤버에게만
  │                                                        다른 멤버 ◀ MCV1 Media Transmission Notification
  │                                                        다른 멤버 ─ MCV0 Receive Media Request ▶ (manual)
  │                                                        다른 멤버 ◀ MCV1 Receive Media Response → 수신 시작
  │ [보내기 끝] MCV2 Transmission End Request ─▶  ◀ MCV2 Transmission End Response
  │
  │ ── [영상 나가기] BYE (MCVideo 다이얼로그) → 음성 호는 그대로 ─────────────────────
```

- 음성 호와 영상 호는 **서로 독립**이다 — 영상 참여 중에도 [PTT] 는 MCPTT 호의 floor 를 잡는다(§7 D6 의 기본 정책).
- chat 그룹은 원하는 사람만 들어오고, prearranged 는 제어 기능이 MCVideo 로 affiliate 한 멤버를 모두 초대한다(§7 D5).

## 5. 구성요소별 설계

### 5.1 CSC (설정 평면)

구현 = `csc/src/services/mcvideo.py`(서비스 경계 — 그룹 문서 조각·user profile·service config·속성 적재/해석/쓰기) + `services/mcptt.py`(문서 틀·
적재·XCAP·CMS 라우트·IdMS). 골든·시험 = `tests/fixtures/mcvideo/*.xml`(계약 K2 — `tests/test_csc_mcvideo.py` 가 «생성 = 골든», `tests/mcvideo_fixture_check.py`
가 TS 24.481·24.484 XSD 엄격 검증).

- **DB**(계약 K1, `sql/migrate_mcvideo.sql` — 표 추가만) — `mcvideo_group_attrs`(행 = 그 그룹이 MCVideo 그룹. 열 = invite_members·max_duration_sec·
  max_transmitters·audio/video_encodings·video_resolutions·video_frame_rate·reception_hang_timer_sec·min_number_to_start·group_priority·protect_media(0)·
  protect_transmission_control(0)·allow_conference_state), `mcvideo_user_profile`(PTT 회선 — 행 = MCVideo 이용 자격. 열 = max_video_streams(C9)·
  max_calls_n6), `mcvideo_affiliations`(서비스별 affiliation — `ptt_affiliations` 와 같은 모양. 따로 둔 이유 = 공유 DB 의 옛 CSP 가 dereg 때
  `ptt_affiliations` 를 사용자 단위로 지우고 옛 OAM 이 그 표를 MCPTT 로 센다). 멤버 entry 의 `mcvideo-mcvideo-id` 는 MCPTT ID 와 같은 값이라
  열을 두지 않는다(§7 D1). N2·우선순위·조직명은 MCPTT 와 같은 service config·설정. 마이그레이션이 `video_enabled=1` 그룹과 기존 PTT 회선 전부에 행을 만든다(§8).
- **그룹 문서(GMS)** — `get_group_xml`: MCPTT `<service enabler>` = MCPTT ICSI, 규칙 = `<is-list-member>` 조건에 `<allow-initiate-conference>`·`<join-handling>`
  true(제어 기능의 개시·합류 인가 근거 — TS 24.281·24.379 §6.3.5.3·§6.3.5.4). MCVideo 그룹이면 MCVideo `<service>`(ICSI enabler, `<mcvideo-video-media>`) +
  `<list-service>` MCVideo 속성(TS 24.481 §7.2.2 목록 순, 보호 둘 false 명시, 실시간 모드 = 비긴급 실시간 고정) + 규칙 action `mcvideo-*`(긴급·임박·경보
  false — V8) + entry `<mcvideo-mcvideo-id uri>`. MCData 서비스가 있으면 entry `<mcdata-mcdata-id uri>`(§7.2.2 MCData entry c)). XCAP PUT 해석
  (`parse_group_document_xml`) = MCVideo `<service>` 가 있으면 켜고 속성 반영(보호 true·범위 밖 400). **전환기 규칙** — MCVideo `<service>` 가 없는 PUT 은
  MCVideo 상태를 건드리지 않는다(MCVideo 를 모르는 옛 단말의 PUT 이 서비스를 지우지 않게. 끄기는 관리 API, V7 에서 «부재 = 끔» 으로 바꾼다).
- **CMS** — `CMSXCAPROOT/org.3gpp.mcvideo.user-profile/users/<MCVideo ID>/mcvideo-user-profile-<n>.xml`(본인만·scope `video_config_management_service`·
  자격 행 없으면 404) · `CMSXCAPROOT/org.3gpp.mcvideo.service-config/global/mcvideo-service-config.xml`(전역 문서, TS 24.484 §9.4.2.9). user profile 의
  그룹 목록 = 이 사용자가 멤버인 MCVideo 그룹(`<MCVideoGroupInfo>` 하나에 하나), 상한 = `MaxSimultaneousVideoStreams`·N6·N2. service config =
  `<signalling-protection>` false 명시 · Resource-Priority(MCPTT 네임스페이스 재사용, TS 24.281 §6.2.8.1.16) · `<tc-timers-counters-R14>` 17요소 전부
  (설정 `McVideoServiceConfig.*`, 기본값 정본 = [mcvideo_tc_defs.yaml](mcvideo_tc_defs.yaml)). CSP 는 같은 문서를 `/internal/mcvideo/service-config` 로 받는다.
  xcap-diff 통지 축에 두 문서를 싣는 것은 CSP 몫(§5.2).
- **ue-init-config** — `<anyExt><MCVideo-Service-Details>`(MCPTT → MCVideo → MCData 순, 설정 `UeInitConfig.ServiceDetails.McVideo.{Enable,ServerUri}` —
  기본 끔(CSP `Roles.MCVIDEO` 를 켠 사이트만), 기본 PSI `sip:mcvideo_psi@<PTT 도메인>`).
- **IdMS** — `SCOPE_CATALOG` 에 `3gpp:mc:video_*` 넷. **사용자 단위 인가** — MCVideo 넷은 자격 행이 있는 사용자에게만 준다(`grant_scope(…, mcptt_id)`).
  자격이 있으면 ID·access 토큰에 `mcvideo_id`(= MCPTT ID, TS 33.180 B.2.1.3·B.2.2.3). 리소스 서버 = GMS(ptt·video·data GMS 중 하나)·MCVideo CMS 문서
  (video CMS)·KMS(ptt·video·data KMS 중 하나).
- **관리 API** (구현 — A6, [admin_api.md](../../api/admin_api.md) §5.4·§6) — 그룹 `mcvideo`(조회 = 속성 객체 또는 null · 쓰기 = 키 없음 그대로 / null 끔 /
  객체 켬·갱신(준 키만), 검사 `services/mcvideo.api_group_attrs` = XCAP PUT 과 같은 범위·보호 true 400, 쓰기 전에 검사) · PTT 회선 MCVideo 자격
  `GET/PUT/DELETE /api/v1/users/{pid}/ptt/{msisdn}/mcvideo`(행 = 자격, 상한 C9·N6 1~16, 캐시 `MCVIDEO_PROFILES` 즉시 갱신 → 문서·scope·claim). 표가 없으면
  400 `schema_not_migrated`. 변경 통지는 기존 경로(`GROUP_CHANGED`·`USER_CHANGED`). 시험 `tests/test_csc_mcvideo.py` AdminApiTest.
- **콘솔** — 그룹 편집 «서비스» 절(MCPTT·MCVideo 켜기, MCVideo 속성), 가입자 PTT 회선 카드 옆 «MCVideo» 자격·상한 — 미구현(위 API 를 쓴다).

### 5.2 CSP (호 제어)

구현된 부품 — 호 제어 경계 코덱 `csp/McVideoInfo.h`(mcvideo-info 해석·생성 · ICSI·특성 태그 판별 `McVideoIcsiIn`·`McVideoFeatureIn`·
`McVideoContactCapable` · 제어 채널 fmtp 해석 `ParseMcVideoFmtp`·answer/offer 조립 `BuildMcVideoAnswerFmtp`·`BuildMcVideoInviteFmtp`(§1.4 K4
규칙) · Warning 문구 — 단위시험 `tests/csp_mcvideo_info_test.cpp` 가 K3 골든을 직접 읽는다), 서비스 축 `csp/McService.h`(`EMcService`, affiliation 표
선택), DB 계층(`CspPttGroup::_mcvideo`·`_mcvideoAttrs` 적재 · `SelectMcVideoProfile` · affiliation 함수의 서비스 인자 · dereg 때 두 표 정리),
psip 합성 SDP 프로파일(`CSipCallRtp::m_eMcMediaProfile = E_MC_MEDIA_MCVIDEO` — `i=` 성분 표시·video rtcp-fb 광고/되돌림·
`m=application … udp MCVideo` + `a=fmtp:MCVideo`, re-INVITE 에도 유지 — `S1-UNIT-PSIP` I·J). 아래 모듈·호 처리는 진행 중.

- **모듈** (구현) — `CMcVideoAsModule`(`csp/McVideoAsModule.{h,cpp}`, `IModule`, `Setup.Roles.MCVIDEO` 기본 off) — 참여·제어 기능 겸임(PTT-AS·
  MCDATA-AS 와 같은 구성). **서비스 판별** `IsMcVideoRequest` = P-Asserted-Service·P-Preferred-Service·Accept-Contact 의 MCVideo ICSI · Accept 의
  mcvideo 문서 형식 · mcvideo-info 본문 · pidf `mcvideoPresInfo` 네임스페이스(`McVideoRequestIndicated`, 표시가 없으면 MCPTT). `ModuleDispatcher::
  EventIncomingCall` 이 MCPTT 판정(mcptt-info·PSI·그룹 R-URI)보다 **먼저** 부른다 — 역할 off 면 404(PSI 미할당, TS 24.281 §6.3.7.1), 그룹 호(A10) 전까지는 480.
- **등록** (구현 — §7.2.1AA 모양) — Contact 의 `+g.3gpp.mcvideo` 와 icsi-ref MCVideo ICSI **둘 다**를 바인딩 능력 `CUserInfo::m_bMcVideo` 로 기록,
  재-REGISTER 마다 다시 판정(태그를 빼면 MCVideo 로그오프, §7.2.1 NOTE 1). 서비스 인가 본문(mcvideo-info 토큰·client-id, §7.2.1)과 `Event:
  poc-settings` PUBLISH(§7.2.2)는 읽지 않는다 — CSP 토큰 검증([mcx_identity_scope.md](mcx_identity_scope.md) §10)과 한 짝으로 MCPTT·MCVideo 를 함께 넣는다.
- **affiliation** (구현 — §8.2.2.2.3~§8.2.2.2.5) — 규격형 PUBLISH·SUBSCRIBE(`Event: presence`)를 서비스로 먼저 갈라 MCVideo 는 표 `mcvideo_affiliations`
  (§5.1)에 쓴다: served ID = mcvideo-info `<mcvideo-request-uri>`(요청자와 다르면 403 — 남의 제휴를 바꾸는 권한은 두지 않는다), Expires 없음·0 이 아닌데
  2^32-1 미만 423(Min-Expires 4294967295), 이용 자격(`mcvideo_user_profile` 행) 없음 403, 대상 = MCVideo 서비스를 가진 그룹, 만료 없음(등록 해제가 행을 지운다),
  200 OK `Expires: 4294967295`, `mcvideo_affiliation` 구독 NOTIFY(`mcvideoPresInfo`), 감사 이벤트 E-AUD-009 `service`. 남은 것 = N2(`<MaxAffiliationsN2>` —
  `mcvideo_user_profile` 에 열이 없어 한도를 걸지 않는다)·user profile `<ImplicitAffiliations>`(§8.2.2.2.15)·chat 합류 암묵적 affiliation(§8.2.2.3.7 — A10).
- **그룹 호** (구현 — §5.2.1) — `McVideoCallService`: chat·prearranged 개시·합류·재합류·퇴장·해제(참가자 수·T1·TNG3), 그룹 종류 검사(§6.3.5.2), 멤버
  fan-out(prearranged), mcvideo-info, SDP(audio·video·`udp MCVideo` — psip `E_MC_MEDIA_MCVIDEO`), Warning(TS 24.281 §4.4). MCPTT `GroupCallService` 와는
  부품(leg PT·그룹/사용자 맵·psip 호 API·`McVideoInfo.h` 코덱)만 나누고 세션·CMP 키는 따로 둔다.
- **CMP 연동** (구현 — A11) — 기존 PTT 명령에 `service: mcvideo`(§7 D3, [cmp_media_api.md](../../api/cmp_media_api.md) §7.9) — 멤버마다 audio·video·control 포트,
  JOIN 응답 `tc_ssrc` 를 answer 의 `mc_transmission_ssrc` 로. 미디어 SRTP 는 접속서비스 `media_srtp` 대로 m= 라인마다 협상해 JOIN
  `media_crypto`·`media_crypto_video` 로 내린다(§5.2.1 · [media_security.md](media_security.md) §5.2). `tc_crypto`(전송 제어 SRTCP)는 CMP 가
  받지만 E2E(CSK) 트랙 전까지 CSP 는 싣지 않는다(§7 D7). CMP 가 `resource.mcvideo` 를 광고하지 않으면(멤버 풀 0) MCVideo 그룹 호를 받지 않는다(500).

#### 5.2.1 그룹 호 (A10 `McVideoCallService` · A11 `CmpClient` MCVideo 명령)

> **구현 — 실측 전.** `csp/McVideoCallService.{h,cpp}` · `csp/CmpClientMcvideo.cpp`. 실측은 공유 DB `sql/migrate_mcvideo.sql` 적용·`Setup.Roles.MCVIDEO`
> 켜기·CSP·CMP 배포 뒤 `cimsue-cli video-call`(.45 C8)로 — 아래 표가 기대 동작이다.

**구조** — `csp/McVideoCallService.{h,cpp}` 전역 하나(`gclsMcVideoCallService`). MCPTT `CGroupCallService` 와 **따로** 선다 — 세션 캐시·CMP
명령이 모두 서비스 키 `(mcvideo, group_id)` 라 같은 그룹 id 의 MCPTT 그룹 호와 동시에 선다(§7 D6). 공유하는 것은 부품뿐이다: 그룹·사용자 맵, psip 호 API,
`McVideoInfo.h` 코덱, leg PT 규칙(`CGroupCallService::GetLegPt` 와 같은 규칙 — 단말 송신 PT = 서버 offer leg 이면 코덱 테이블 PT, 단말 offer leg 이면 answer
가 echo 한 offer PT). leg 는 CallMap 밖에서 이 서비스가 수명을 관리한다(MCData media plane 과 같은 방식) — `CMcVideoAsModule::OnIncomingCall` 이 INVITE 를
넘기고, `ModuleDispatcher` 가 `EventCallStart`·`EventCallEnd`·`EventCallRing`·`EventReInvite`(세션 갱신 판정 뒤) 맨 앞에서 서비스에 먼저 묻는다. psip 의
`StopCall` 은 `EventCallEnd` 를 부르지 않으므로(로컬 종료) 서비스가 끝낸 leg 는 그 자리에서 정리한다. 1 s 틱(`ModuleDispatcher::Tick`, 역할 on 일 때)이 한도·TNG3 를 본다.

**세션** — 그룹마다 하나: `sesid`(CMP·로그 상관) · MCVideo 세션 식별자 토큰 `gr`(TS 24.281 §4.5 — 포커스 Contact 의 GRUU, 재합류 Request-URI) · 종류
chat|prearranged(그룹 속성 `invite_members`) · 개시자 · 시작 시각(TNG3 = `max_duration_sec`) · leg 표(Call-ID → 멤버·역할 initiator|joiner|invited·확립
여부·CMP 주소 등록 여부·초대 응답 한도) · prearranged 개시 대기(개시자 Call-ID·offer 사본·암묵 요청 여부·대기 한도). CMP 로스터는 **붙는 멤버만** 싣는다 —
멤버가 붙을 때마다(수락·초대 직전) `PTT_GROUP_ADD service:mcvideo`(members = 그 멤버 `id:prio:role` 하나 · `group_type` · `max_transmitters`(1~16) ·
`reception_mode manual` · `call_type normal` · `tc_timers{t1_ms = 그룹 hang timer(0 = 미사용), t5_ms = reception hang timer}`)를 보낸다. CMP 는 로스터 멤버마다
포트 유닛을 잡고 로스터를 병합하므로(`updateRoster`) 첫 ADD 가 그룹을 세우고 뒤 ADD 는 멤버를 더한다 — 그룹 전원을 실으면 참가하지 않는 멤버의 유닛까지 점유된다.
세션이 끝나면 `PTT_GROUP_REMOVE`.

| 입력 | 처리 (근거 절) |
|---|---|
| INVITE — 검사 | 순서대로: CMP 가 `resource.mcvideo` 를 광고하지 않으면 500(§9.2.2.4.1.1 1)) · Accept-Contact 에 `g.3gpp.mcvideo`·MCVideo icsi-ref 가 없거나 Contact 에 isfocus → 403(2)) · 대상 = Request-URI 의 `gr` 이 진행 중 세션이면 그 세션(재합류 — 없는 세션이면 404 Warning 137, §9.2.1.4.5.1), 아니면 mcvideo-info `<mcvideo-request-uri>` · 그룹 없음·MCVideo 그룹 아님 → 404 113 · 멤버 아님 → 403 116 · session-type 불일치 → 404 117/118(§6.3.5.2 5)) · 이용 자격(`mcvideo_user_profile`) 없음 → 403 109/108 · N6(동시 MCVideo 호 — 참가 중이거나 스스로 연 leg 가 있는 세션 수, 응답 전 초대 leg 는 세지 않는다) 초과 → 486 103(§9.2.2.3.1.1 5)) · 제휴 안 됨 → prearranged(개시·합류·재합류)는 403 120(§9.2.1.4.2 13)a)·14)a) — 일반 호에는 암묵적 affiliation 이 없다, §8.2 머리말), chat 은 멤버면 SDP 검사 뒤 암묵적 affiliation(§9.2.2.4.1.1 5)·12) · §8.2.2.3.6·§8.2.2.3.7 — `mcvideo_affiliations` + NOTIFY), 실패면 403 120 · SDP 에 전송 제어 채널(`m=application <port≠0> udp MCVideo` — `a=fmtp:MCVideo` 는 선택) 또는 음성 AMR-WB 가 없으면 488. 영상 성분은 H.264 PT 가 있을 때만 받는다(없으면 answer `m=video 0`) |
| INVITE — chat | 세션이 없으면 만든다. 개시자·합류자 모두 같은 절차: 로스터 등록(ADD) → CMP JOIN ①(포트·`tc_ssrc`) → JOIN ②(offer 주소·`user_control_port`·`user_tc_ssrc` = offer `mc_transmission_ssrc`·`user_audio/video_ssrc` = offer `a=ssrc`·PT·`queueing`·`max_priority` = min(offer `mc_priority`, `<user-priority>`)·`max_rx_streams` = user profile 동시 수신 스트림(1~16)) → 200 OK. **팬아웃 없음**(chat — §7 D5) |
| INVITE — prearranged 새 세션 | **affiliated 멤버 팬아웃**(MCVideo 등록 바인딩 `m_bMcVideo` 가 있는 멤버만 — 멤버마다 로스터 등록 → JOIN ① → INVITE). 초대가 하나도 나가지 못하면 개시자에 480. 개시자 200 OK 는 **첫 멤버가 붙은 뒤**(초대 멤버의 200 OK 또는 멤버의 스스로 합류 — §9.2.1.4.2, 미디어 버퍼링 없음 → 확인 없는 200 을 먼저 주지 않는다): 그 멤버 JOIN ② 다음에 개시자 로스터 등록·JOIN ①·②(암묵 요청이면 `implicit_request` → 곧바로 허가·SSRC 쌍) → 개시자 200 OK(answer `mc_implicit_request`·`mc_granted`·`mc_audio/video_ssrc`). 초대가 모두 실패하거나 개시 대기 한도(10 s — CIMS 값, 규격은 확인 통화의 TNG1 만 정한다) 안에 아무도 붙지 않으면 개시자에 480. 대기 중 개시자가 CANCEL 하면 세션 해제(초대 leg 는 CANCEL), 새 INVITE 를 보내면(BYE 없는 재시도) 옛 INVITE 에 487 을 주고 대기 leg·offer 를 새것으로 바꾼다 |
| INVITE — prearranged 진행 중 세션 | 합류(재합류 `gr` 포함): chat 합류와 같은 절차, 암묵 요청은 받지 않는다(TS 24.581 §14.3.5). 개시 대기 중이면 합류 뒤 개시자에게 답한다 |
| 같은 멤버의 새 INVITE (BYE 없는 재합류) | 옛 leg 는 SIP 다이얼로그만 끝낸다(확립 = BYE, 응답 전 초대 = CANCEL) — CMP 멤버 키가 (group, 멤버)라 LEAVE 하지 않는다 |
| 멤버 팬아웃 INVITE (골든 07) | From = 그룹 URI, Contact = 포커스(`g.3gpp.mcvideo`·icsi-ref·isfocus + `gr`), Accept-Contact 두 줄, `P-Asserted-Service` MCVideo ICSI, `P-Asserted-Identity` = 제어 기능 PSI(`mcvideo_psi` — §9.2.1.4.1.1 3), 200 OK 와 같은 신원), `Session-Expires: 1800` — **refresher 생략**(§6.3.3.1.2 6); 스택이 로컬 정책으로 싣는 refresher 를 지운다 — 단말이 200 OK 에서 `refresher=uas` 로 정한다, §6.2.3.1.1 5)), multipart = SDP(offer: 멤버 CMP 포트 audio·video·control, 음성 = 코덱 테이블 AMR-WB, `mc_priority` = `<user-priority>`, `mc_transmission_ssrc` = 이 멤버 `tc_ssrc`) + mcvideo-info(session-type prearranged · request-uri = 멤버 · calling-user-id = 개시자 · calling-group-id = 그룹). 초대 응답 한도 30 s(CIMS 값 — 규격은 정하지 않는다) 안에 200 OK 가 없으면 CANCEL |
| 미디어 SRTP (SDES — [media_security.md](media_security.md) §5.2) | 접속서비스 `media_srtp` × offer crypto, **m= 라인마다**(SDES 키는 m= 라인마다 다르다 — RFC 4568 §6.1). 단말 offer: 음성 협상이 깨지면 488, 영상이 깨지면(또는 음성 SRTP 인데 영상 평문) 영상 성분만 거절(answer `m=video 0`). 서버 offer(멤버 초대): required 또는 optional + 바인딩 mediasec 능력이면 audio·video 각각 서버 키로 `RTP/SAVP` + `a=crypto`, 멤버 200 OK 의 crypto 가 어긋나면 음성 = BYE·영상 = 영상만 뺀다. 키 = JOIN ② `media_crypto`·`media_crypto_video`(rx = 단말 키, tx = 서버 키), re-INVITE = 단말 재키잉만 |
| 200 OK answer (골든 04·06) | 멤버 CMP 포트로 audio(코덱 테이블 AMR-WB — offer PT echo)·video(영상 성분이 없으면 port 0 — RFC 3264 §6, JOIN 에 video 포트 없음)·`m=application <control_port> udp MCVideo` + `a=fmtp:MCVideo` = `BuildMcVideoAnswerFmtp`(offer 에 있던 것 + `mc_transmission_ssrc` = JOIN 응답 `tc_ssrc` · 암묵 요청이면 `mc_implicit_request`·(허가) `mc_granted`·`mc_audio/video_ssrc`). 헤더 = Contact 포커스 + `gr` · `Require: timer` · `Session-Expires: …;refresher=uac`(단말 갱신, §6.3.3.2.3.2 2)) · `Supported: tdialog` · `P-Asserted-Identity` = MCVideo PSI(§6.3.3.2.3.2) |
| 멤버 200 OK | JOIN ②(answer 주소·포트·`a=ssrc`, 단말 송신 PT = 서버 offer PT) → prearranged 개시 대기면 개시자 수락. JOIN ② 실패 → BYE·leg 정리. 4xx~6xx → leg 정리 |
| re-INVITE | answer = 스택의 직전 로컬 선언(멤버 CMP 포트·제어 채널·SRTP 서버 키)이되 `a=fmtp:MCVideo` 는 **re-offer 로 다시 짓는다**(offer 에 있던 파라미터 + `mc_transmission_ssrc` — TS 24.581 §14.3.1; 개시 answer 의 `mc_implicit_request`·`mc_granted`·`mc_*_ssrc` 가 갱신 answer 에 남지 않게 — 암묵 요청은 새 세션 개시에서만, §14.3.5). 미디어 변경(망 전환·주소 변경)이면 JOIN ② 재선언(새 주소·PT·`a=ssrc`·SRTP 단말 재키잉 — CMP 는 협상 값만 갱신하고 전송 제어 상태는 둔다), 세션 갱신(미디어 무변경)은 CMP 를 부르지 않는다(leg_liveness.md §6.3). psip 부품 시험 S1-UNIT-PSIP [N] |
| BYE / 이탈 | CMP LEAVE(같은 멤버의 다른 leg 가 남으면 하지 않는다), leg 제거. **prearranged 는 참가자 1명 이하, chat 은 0명** → 세션 해제(§6.3.8.1 2) — chat 은 참가자가 모이기를 기다리는 세션이다) — 남은 leg 에 BYE/CANCEL, CMP REMOVE. 개시 대기 중에는 개시 쪽 판정(위)만 |
| CMP `TRANSMISSION_INACTIVITY{T1}` | prearranged 면 해제(§6.3.8.1 1)), chat 은 로그만(세션은 참가자가 끝낸다). `T5` 는 로그만 |
| TNG3 (`max_duration_sec`) | 해제(§6.3.8.1 5)) |
| CMP `PTT_GROUP_ABORTED` (service mcvideo) | CMP 가 그룹을 회수했다 — 남은 leg 를 끝내고 캐시를 지운다(REMOVE 는 보내지 않는다). **MCPTT 서비스의 같은 그룹 id 를 건드리지 않는다**(A11 에서 서비스로 가른다) |
| CMP `TRANSMITTERS` (service mcvideo) | 로그(세션 송출 축은 A12) |

**CmpClient (A11)** — `McvAddGroup`·`McvJoin`·`McvLeave`·`McvRemove`(payload `service:"mcvideo"` → hdr.service). 세션·끝점 캐시 키 = `mcvideo|<group>`(MCPTT 키와
겹치지 않게). JOIN 응답에서 `port`·`video_port`·`control_port`·`tc_ssrc`·`granted`·`audio_ssrc`·`video_ssrc` 를 읽는다. `HandleEvent` 는 hdr.service 가
mcvideo 인 `PTT_GROUP_ABORTED`·`TRANSMITTERS`·`TRANSMISSION_INACTIVITY` 를 MCVideo 서비스로 보낸다.

**1차 범위 밖** — 긴급·임박·방송·ad hoc·private(V8), 확인 통화(required 멤버·TNG1 확인·min-number-to-start), conference 이벤트 NOTIFY, 녹취(B8),
전송 제어 SRTCP 키(`tc_crypto` — E2E CSK 트랙), 참가자 수 상한(`on-network-max-participant-count` 속성 없음).

### 5.3 CMP (미디어 · 전송 제어)

- **그룹 종류** (구현) — `PMcvideoGroup` — `PMcpttGroup` 과 따로 선 그룹 종류(floor 없음). 서버 자원 키 = (service, group_id) — 같은 그룹 id 의 MCPTT
  그룹 호와 동시에 선다(§7 D6). 멤버 단위 = 전용 유닛 `PMcvMemberPort` — 6포트 블록(audio RTP · video RTP · **video RTCP**(PLI·FIR 를 받는다 — 현행
  PTT 영상 결함 해소) · 전송 제어), 그룹 공유 포트 없음. 멤버 두 단계(선할당 = 유닛 + 전송 제어 SSRC `tc_ssrc` · 주소 등록), 소스 판정·NAT latch 는 MCPTT
  멤버와 같은 규칙, 전역 유일 SSRC 할당기(`AllocSsrc` — 송출 SSRC·`tc_ssrc` 공용). 제어 명령 `PTT_GROUP_ADD/MODIFY/REMOVE`·`PTT_JOIN/LEAVE` +
  `service:"mcvideo"`(`cmp/PCmpServerMcvideo.cpp`), `resource.mcvideo`·STATS `mcvideo_groups`·sweeper 회수(`PTT_GROUP_ABORTED` service mcvideo).
  보호(구현 — B7) — 멤버 SRTP(`media_crypto`·`media_crypto_video` — 상향 멤버 키로 풀고 하향 받는 멤버 키로 SSRC·PT 찍기 뒤 보호) · 전송 제어 SRTCP
  (`tc_crypto` — 멤버 CSK > 그룹 키 > 평문, `PFloorCrypto` 재사용, 같은 구성 재선언은 컨텍스트 유지), 키는 참가 등록 전에 걸어 첫 Idle 부터
  보호. 스모크 `tests/cmp_smoke_mcvideo_ports.py`(시험용 CMP 를 직접 띄운다 — SRTP/SRTCP 는 스모크의 독립 파이썬 구현으로 교차 확인).
  허가 없는 미디어는 분배하지 않는다(`no_grant_drop`). 송출 SSRC 할당은 멤버 offer 의 `a=ssrc`(JOIN `user_audio_ssrc`·`user_video_ssrc`)가
  전역에서 쓰이지 않으면 그 값(§14.3.7·§14.3.8).
- **전송 제어 서버** (구현 — §5.3.1) — TS 24.581 §6.3.4~§6.3.7: 동시 송출 상한(그룹 속성), 우선순위 선점(Revoked #4), 큐(`mc_queueing`), 참여자별
  상태, T1~T6·T11. `cmp/PMcvControl.{h,cpp}` — 단위시험 `tests/cmp_mcvideo_control_test.cpp`(S1-UNIT-CMP), 스모크 `tests/cmp_smoke_mcvideo_ports.py`.
- **수신 제어** (구현) — 수신자별 Active SSRC List: 허가된 송출의 audio·video 만 그 수신자에게, SSRC = 송출 할당값 · PT = 수신 leg 값으로 찍어
  보낸다. manual/automatic 모드, 수신자 동시 스트림 상한(C9 — #7).
- **영상 RTCP** (구현 — B6) — 수신자의 PLI(RFC 4585 §6.3.1)·FIR(RFC 5104 §4.3.1)을 가리키는 송출자에게: 대상 = 할당 video SSRC → 송출자 원래 SSRC 로
  되돌려 CMP 가 복합 패킷(RR + SDES CNAME + PLI/FIR)으로 다시 보낸다(요청자가 그 송출을 받을 때만, 송출자마다 500 ms 에 하나, SRTCP leg 는 풀고 다시
  보호). 종류는 송출자가 협상한 것만(RFC 4585 §4.2 — CSP 가 멤버 영상 SDP 의 `a=rtcp-fb` 를 JOIN `user_video_fb` 로 옮긴다. pjmedia 단말은 `nack pli`
  만이라 FIR 도 PLI 로). 수신이 시작될 때(Active SSRC List 추가) CMP 가 스스로 PLI — manual [받기] 뒤 영상이 다음 주기 키프레임까지 멈추지 않게. 송출자 SR 은
  옮기지 않는다. [cmp_media_api.md](../../api/cmp_media_api.md) §7.9.

#### 5.3.1 송출·수신 제어 상태 머신 (B4·B5)

**구조** — 호 하나에 `PMcvControl`(`cmp/PMcvControl.{h,cpp}`) 하나. 소켓·락 없는 순수 로직이다 — 입력 = 참가자 추가/제거·해석된 메시지
(`ParsedTransmission`)·미디어 도착 표시·시각(ms) 틱, 출력 = 훅(`send(member, app, subtype, fields)` · `inactivity("T1"|"T5")` · `transmittersChanged()` ·
`receptionStarted(receiver, sender)` — 영상 키프레임 요청 계기, B6).
`PMcvideoGroup` 이 그룹 락 아래 부르고, `send` 를 `BuildTransmissionMessage` → 멤버 유닛 `sendTo(MCV_CH_CONTROL)` 로 잇는다(헤더 SSRC = 멤버
`user_tc_ssrc`, 없으면 `tc_ssrc`). 단위시험 `tests/cmp_mcvideo_control_test.cpp`(S1-UNIT-CMP — `PMcvControl.cpp` + `PTransmissionCodec.cpp` 만 링크).
SSRC 할당기(`AllocSsrc`·`FreeSsrc`)는 `PMcvControl` 로 옮겨 그룹·시험이 같이 쓴다. 틱 = 100 ms(`mediaBufferLoop` 20 ms 클록에서 MCVideo 그룹이 있을 때만,
그룹 수는 atomic) — 1 s 틱으로는 T2~T6(기본 1 s)의 오차가 너무 크다.

**상태** — 규격의 네 기계를 그대로 둔다: 일반 송출(G — Idle / Taken, Cx = 허가된 송출 수, 상한 = `max_transmitters`) · 참가자 송출(U — not permitted
and Transmit Idle / not permitted and Transmit Taken / permitted / pending Transmit Revoke / not permitted but sends media / Releasing) · 일반 수신(Gr — Reception
Idle / Reception accepted, C7 = 송출별 C11 의 합) · 참가자 수신(U — not permitted to receive / permitted to receive, C9 = Active SSRC List 의 송출 수, 상한
`max_rx_streams`). 'G: pending Transmission Revoke' 는 송출마다의 표식(`revoking`)으로 둔다 — 동시 송출에서 회수는 송출 하나에 걸리기 때문이다.

| 입력 | 처리 (근거 절) |
|---|---|
| 참가자 추가(JOIN ② — 주소 등록) | 진행 중 송출이 없으면 Transmission Idle 1회(§6.3.5.2.2 2a·4b, Message Sequence Number +1), 있으면 'not permitted and Transmit Taken' + 송출마다 Media Transmission Notification(§6.3.7.2.2 2b) |
| JOIN `implicit_request`(새 prearranged 세션) | SSRC 쌍을 JOIN 때 예약(응답 `audio_ssrc`·`video_ssrc` — §14.3.7·§14.3.8 «irrespective of mc_granted»). 주소 등록된 다른 참가자가 있으면 곧바로 허가(응답 `granted` 1 — CSP 가 offer 의 `mc_granted` 가 있었으면 answer 에 싣는다), 없으면 첫 초대 참가자가 등록될 때 허가하고 Transmission Granted 를 보낸다(§6.3.2.2 «granted … when the first invited MCVideo client accepts» — 미디어 버퍼링 없음). 기다린 요청이라 T4/C4 로 첫 미디어까지 재송신하고, 기다리는 동안 온 명시 요청(단말 T100 재요청)은 같은 요청으로 본다. 기다림은 참여자 T100×C100(기본 3 s — 참여자는 그 뒤 'U: has no permission' 이라 늦은 Granted 를 버린다, §6.2.4.4.4)까지 — 넘으면 예약을 풀고 Transmission Idle. CSP 는 암묵 요청 개시의 200 OK 를 첫 초대 멤버의 200 OK 뒤에 보내므로(A10) 보통은 JOIN 때 곧바로 허가된다 |
| Transmission Request (MCV0 0) | 수신 전용(`recv_only` — 그룹 문서 `<on-network-recvonly>`)이면 Rejected #5. G: Idle 에서 참가자 1명이면 #3(§6.3.4.3.3). Cx < 상한이면 허가(§6.3.4.4.7A·§6.3.4.4.2): SSRC 쌍 할당(선호 = offer `a=ssrc`) → 요청자 Granted(Transmission Priority·Audio/Video SSRC) · 다른 참가자 Media Transmission Notification(Transmitting User ID·SSRC 쌍·Message Sequence Number·Permission 1·Reception Mode 0/1). 상한이면 선점 판정 — 선점이면 가장 약한 송출에 Revoked #4 + 요청을 큐 맨 앞(§6.3.4.4.7), 아니면 queueing 협상 시 큐(Queue Position Info), 미협상이면 Rejected #1(§6.3.5.4.4). 이미 허가된 참가자의 재요청 = Granted 재송신(§6.3.4.4.8) |
| 유효 우선순위(§4.1.1.4 local policy) | MCPTT floor 와 같은 서열(`PMcpttGroup::_preempts`) — ① tier(긴급 > 임박 > 일반, **CSP 지시로만** 바뀐다 — 요청의 Transmission Indicator 는 호 단위 표식이라 판정에 쓰지 않는다) ② chair ③ 수치 우선순위 = `members` prio, 요청의 Transmission Priority 는 `mc_priority` 를 협상했을 때만 min(요청, 협상 상한)(§6.3.5.4.4 1a). 선점 = 요청 서열 > 가장 약한 송출 서열 |
| Transmission End Request (MCV2 0) | ack 비트면 Ack. permitted/pending revoke → 송출 끝: Transmission End Response · 분배 중지 · SSRC 반환 · 다른 참가자 Transmission End Notify(User ID + SSRC 쌍) · Cx−1 → 0 이면 G: Idle(큐 맨 앞이 있으면 그것을 허가, 없으면 Transmission Idle 전원 · T2/C2 · T1)(§6.3.4.4.6·§6.3.4.5.4). not permitted(큐 대기) → 큐에서 빼고 Idle 또는 Notification(§6.3.5.3.7·§6.3.5.4.5) |
| Transmission End Response (MCV2 1) | 서버가 보낸 End Request(T11 만료 — #8)의 응답 — T3 정지, 송출 끝(§6.3.5.6.8·§6.3.4.5.7) |
| Queue Position Request (MCV0 3) | Queue Position Info(위치·우선순위, 큐에 없으면 254)(§6.3.5.4.7) |
| Receive Media Request (MCV0 4) | 가리키는 송출 = Transmitting User ID, 없으면 Audio/Video SSRC. C9 상한이면 Receive Media Response Result 0 · cause #7, 없는 송출이면 #1. 허가 → Result 1 + 송출 식별자(User ID + SSRC 쌍) · Active SSRC List 추가 · C11+1(T11 정지)·C7+1(T5 정지)(§6.3.6.3.6·§6.3.6.4.3·§6.3.7.3.6·§6.3.7.4.10) |
| Media Reception End Request (MCV2 2) | Media Reception End Response(송출 식별자) · Active SSRC List 제거 · C9−1 · C11−1(0 이면 T11 시작) · C7−1(0 이면 Gr: Idle — T5)(§6.3.6.4.4·§6.3.7.4.9) |
| Transmission Control Ack (MCV2 4) | 그 메시지의 재송신 타이머 정지(T6 — 아래) |
| ack 비트가 선 수신 메시지 | Transmission Control Ack(Message Type = 받은 subtype · Source 2 · **Message Name** — subtype 값이 name 사이에서 겹친다) |
| 미디어(payload 있는 RTP — 헤더만 = keepalive 는 판정 밖) | permitted/pending revoke 송출자 → 분배(T4 정지): 수신자마다 Active SSRC List 에 그 송출이 있으면 SSRC 를 할당값으로·PT 를 수신 leg 값으로 찍어 보낸다. 허가 없는 참가자 → 버림 + Revoked #3 → 'not permitted but sends media'(T3 재송신)(§6.3.5.3.8·§6.3.5.4.6) |
| 참가자 제거(LEAVE) | 송출 중이면 송출 끝(End Notify 전원 · Cx−1 · Idle/큐), 큐에서 빼고, 수신 몫(C11·C7) 정리(§6.3.3 · §6.3.4.4.11 · §6.3.5.8.2) |

**타이머** — T1(Inactivity — G: Idle 동안, 만료 = `TRANSMISSION_INACTIVITY{timer:"T1"}`, 해제는 CSP) · T2/C2(Idle 재송신) · T3(Revoke/서버 End Request 재송신 —
포기 = 5회 뒤 서버에서 송출을 끝낸다, 규격은 구현 선택·연결 해제 권고) · T4/C4(큐에서 허가한 Granted·늦게 내린 암묵 허가의 재송신 — 첫 미디어에 정지) · T5(Reception Inactivity — Gr: Idle
동안, 이벤트 `"T5"`) · T6/C6(Receive Media Response(Granted) 재송신 — ack 비트를 세워 보내고 Ack 에 정지) · T11(Stream Reception Idle — manual 에서 Notification 뒤
받는 이 없이 지나면 그 송출에 서버 End Request #8, §6.3.4.4.13).

**규격을 읽은 방식(§9 에 같이 적는다)** — T1·T5 는 호 시작(첫 참가자)부터 돈다(§6.3.4.3.2·§6.3.6.3.2 는 'Start-stop' 에서 들어갈 때 시작을 적지 않지만 hang timer
뜻대로라면 무송출 호도 풀려야 한다). T6 은 허가마다 시작한다(§6.3.6.3.6 에는 없고 §6.3.6.4.3 에만 있다 — 첫 수신자만 재송신이 없는 것은 편집 누락으로 본다).
MCV2 End Request/Response·Media Reception End Response 에 Transmission Indicator 허용(K5 — .45 B2). not permitted 상태(요청·대기 취소)의 End Request 에도
End Response 를 함께 보낸다 · Ack 의 Message Type 은 받은 subtype · Notification 에 Message Sequence Number 를 싣지 않는다 · 없는 송출의 수신 요청은 #255
(모두 §9). C7·C11 은 수로만 쓴다 — 두 카운터의 상한으로 거절하는 절차가 규격에 없어 목록 크기로 두고, 수신 허가의 상한은 C9 다(§6.3.7.4.10 1a).
무허가 미디어는 'Transmit Taken' 에서 늘 Revoked #3, 'Transmit Idle' 에서는 직전에 허가 송출을 끝낸 참가자만(§6.3.5.3.8) — 송출이 끝난 뒤 500 ms
(`kEndGraceMs`) 안에 온 RTP 는 End Request 와 엇갈려 떠난 것으로 보고 회수 없이 버린다.

**K6 키**(구현) — JOIN `recv_only`(그룹 문서 `<on-network-recvonly>` → Rejected #5) · ADD `call_type`(normal/emergency/imminent — tier 와 Transmission
Indicator, automatic 수신; 1차 CSP 는 normal) · JOIN 응답 `audio_ssrc`·`video_ssrc`(암묵 요청 — 허가 전에도 예약한 쌍) · 이벤트 `TRANSMITTERS`(송출 집합이
바뀔 때마다)·`TRANSMISSION_INACTIVITY`(T1·T5) · STATS `transmitters`·`receptions` — [cmp_media_api.md](../../api/cmp_media_api.md) §7.9·§8.
- **코덱** — RTCP APP `MCV0`·`MCV1`·`MCV2` 부호화·해석(`PFloorCodec` 과 나란한 `PTransmissionCodec`, 필드 표 §9.2.3), 제어 SRTCP 는 `PFloorCrypto` 재사용.
  상수는 생성 헤더 `cmp/PTransmissionDefs.h`(정본 [mcvideo_tc_defs.yaml](mcvideo_tc_defs.yaml) — 단말 코어와 같은 테이블, §1.5).
  코덱 `cmp/PTransmissionCodec.{h,cpp}`(단말 코덱과 교차 시험 — [ue_sdk.md](ue_sdk.md) §4.6)는 CMP 빌드에 들어 있고, 멤버 제어 채널이 이것으로 푼다
  (compound RTCP 를 나눠 APP 만 — 빈 RR keepalive 는 버림) 뒤 `PMcvControl` 에 넘긴다.
- **녹취** (CMP 몫 구현 — B8) — ADD `record_dir`·`session_dir` 가 있으면 `PSyncRtpRecorder`(type `mcvideo` — PTT 와 같은 세션 레이아웃:
  시간버킷 › 세션 디렉터리 › shard, [recording.md](recording.md) §3.3)로 기록한다. 세그먼트 = 송출이 이어지는 구간(송출자 0 → 1 에서 열고 다시 0 이면
  닫는다), 송출자마다 슬롯 하나(`audio`/`video`, 동시 송출이면 `audioK`/`videoK` — 가장 낮은 빈 슬롯), 트랙 = 받은 그대로(평문, SSRC·PT 찍기 전),
  화자 구간 = 송출자, 음성 PT/코덱 = 그 leg 의 ingress 값. 계기 = 송출자 집합 변경(`transmittersChanged`). **남은 것** = CSP 가 `record_dir`·
  `session_dir` 를 싣는 경로(그룹 녹취 디렉터리·세션 디스크립터)·OAM 이력의 서비스 축 `mcvideo` — 녹취 레이아웃(`recordings/ptt/{id}` 공용 대 서비스
  영역 분리)을 정한 뒤. 그때까지 CSP 는 싣지 않으므로 MCVideo 는 녹취되지 않는다.

### 5.4 단말 SDK (`libcimsue`)

- **계정·등록**(구현) — `AccountConfig.mcvideoServerUri`(ue-init-config), `mcvideoEnabled` 면 REGISTER Contact 에 `+g.3gpp.mcvideo` + icsi-ref 목록의
  mcvideo ICSI(TS 24.281 §7.2.1AA — 서비스 인가 본문 없음, CSP 판정 = Contact 태그 + 이용 자격). 서비스 태그는 REGISTER 에만 모으고 icsi-ref 는 한 목록 —
  서비스 호 Contact 는 호가 자기 태그를 싣는다([ue_sdk.md](ue_sdk.md) §4.6). 서비스 인가 본문(mcptt-info·mcvideo-info 토큰)은 CSP 토큰 검증과 한 짝으로 뒤에.
- **설정 해석**(구현 — [ue_sdk.md](ue_sdk.md) §4.2) — `UeInitConfigDoc.mcvideoServerUri` · 그룹 문서 `GroupDoc.mcvideo`(`McVideoGroupAttrs`, 생성도 골든과
  같은 순서) · `McVideoUserProfileDoc`·`McVideoServiceConfigDoc`(`CscClient::fetchMcVideoUserProfile`·`fetchMcVideoServiceConfig`) · 토큰 scope 에
  MCVideo 넷. 시험 = K2 골든을 CSC 생성 시험과 같은 파일로 읽는다.
- **affiliation**(구현) — `affiliate(groupId, on, McVideo)` = 관심 그룹 집합을 바꿔 **전부**를 한 PUBLISH 로(§8.2.1.2 — 골든 02, `Expires` 2^32-1/0,
  pidf tuple id = MC client ID, 게시마다 유일 `p-id`). ⚠️ MCVideo 제휴를 서비스로 가르는 CSP(A9 — ICSI·mcvideo-info 로 갈라 `mcvideo_affiliations`)에만
  보낸다 — 그 전 CSP 는 `Event: presence` PUBLISH 를 MCPTT affiliation 으로 읽어 `ptt_affiliations` 를 덮는다.
- **호**(구현) — `joinVideoGroupCall(groupId, {chat|prearranged, …})` → `CallInfo.service = McVideo`·`sessionUri`(제어 기능 Contact 의 세션 식별자), 재합류
  = `VideoGroupCallOptions.sessionUri`(§9.2.1.2.4), 제어 기능 멤버 초대(§9.2.1.3) = mcvideo-info 로 가려 자동 수락(`autoAnswerMcvideo`), 나가기 = `hangup`.
  INVITE·answer 모양은 K3 골든 03·05·08(만드는 모양)과 04·06·07·09(읽는 모양)으로 대조한다 — SDK 산출 메시지는 `tests/mcvideo_fixture_check.py` 도 통과한다.
- **전송 제어 참여자**(구현·결선) — `requestTransmission`·`releaseTransmission`(§6.2.4 상태 머신, T100·T101), 이벤트 `onTransmission`(Granted·Rejected·Revoked·Idle·
  Media Transmission Notification — 송출자·SSRC). 호 성립 = 개시 CONFIRMED(협상된 answer) · 착신 200 OK 송신, 성립 뒤에 `Active` 를 알린다.
  CMP 와 맞붙인 교차 스모크 `tests/mcvideo_cmp_sdk_xcheck.sh`(시험용 CMP + SDK 참여자 둘 — chat 송출·알림·[받기]·상한 거절·End Notify, 대기열, 암묵 요청 즉시·늦은 허가).
- **송출 게이트**(구현) — 마이크·카메라는 송출 허가에서만. 허가 밖에서는 오디오 인코더를 멈춰 무음 프레임도 내지 않는다(서버는 허가 없는 payload RTP 에
  회수 #3 — §5.3.1). 빈 RTP keep-alive·RTCP·제어 채널 빈 RR 은 계속 나가 NAT·latch 를 연다.
- **수신 제어** — `acceptReception(callId, transmitterId)`·`endReception`(§6.2.5, T103·T104)은 결선됐다. 스트림별 렌더 창(현행 «호별 수신 창» 과제와 합친다 —
  ue_sdk.md §11)과 송출 영상 결선은 C6.
- **바인딩**(구현 — C7) — C API·.NET·Kotlin 같은 이름(현행 그룹 영상 옵션 누락도 메웠다 — [ue_sdk.md](ue_sdk.md) §4.6). MCVideo 설정 문서 해석(C2)도 셋 다 —
  그룹 문서 MCVideo 몫(없음 = MCVideo 그룹 아님 · PUT 에 싣지 않아 서버 MCVideo 설정 유지)·user profile·service config·ue-init-config MCVideo PSI.
- **cimsue-cli**(구현 — C8) — `video-call <g> [--prearranged] [--implicit] [--transmit-at S --transmit-len S] [--accept]` · `video-answer`(멤버 초대 대기) ·
  구동 명령 `video_call`·`transmit_request`·`transmit_release`·`reception_accept`·`reception_end` + 이벤트 `transmission`·`reception`([ue_sdk.md](ue_sdk.md) §4.7).
  명시 affiliation = 계정 옵션 `--affiliate-mcvideo G[,G2]`(prearranged 팬아웃을 받을 멤버).
- **세션 타이머**(구현) — 착신 200 OK `refresher=uas` + `Require: timer`(§6.2.3.1.1 2)·5) — 단말이 갱신), 발신은 서버 200 OK 의 `refresher=uac` 를 따라 단말이
  갱신, 갱신 re-INVITE 등 이어지는 offer 에는 `mc_granted`·`mc_implicit_request` 를 싣지 않는다(TS 24.581 §14.5 — [ue_sdk.md](ue_sdk.md) §4.6).
- **영상 없는 엔진 빌드** — Linux 헤드리스·Windows 1차(config_site `PJMEDIA_HAS_VIDEO 0`)는 offer 의 m=video 를 port 0 자리로 싣는다(RFC 3264 §5.1 — 음성·
  전송 제어만 협상, ue_sdk.md §4.6 편차 표). M2 의 신호·음성·전송 제어는 이 빌드로 되고, 영상 RTP·PLI 확인은 Linux 엔진 영상(H.264 인코더·합성 캡처)
  이나 Android 실기(C6)가 필요하다.
- 전송 제어 상수는 생성 헤더 `mcvideo/tc_defs.h`, 코덱·참여자 빌더는 `mcvideo/tc_codec`, 호 제어 경계 코덱은 `mcvideo/mcvideo_sip`.
- **참여자 구현**(`mcvideo/tc_participant`) — 규격이 비워 둔 곳은 이렇게 읽는다: ① Transmission Revoked 는 원인 #7(Queue the transmission)이면
  Queue Position Request → 'U: queued', 그 밖은 Transmission End Request → 'U: pending end'(§6.2.4.5.5 4 는 #5·#7 만 적었지만 서버는 회수 뒤 End
  Request 를 기다린다 — §6.3.5.6) ② 암묵적 송출 요청을 서버가 받지 않으면(answer 에 `mc_implicit_request` 없음 — chat 합류·진행 중 합류, §14.3.5)
  T100 만료를 기다리지 않고 곧바로 명시 Transmission Request ③ Transmission End Notify(§6.2.5.3.4)는 그 송출의 수신 인스턴스도 닫는다 ④ 'basic
  reception control' 인스턴스가 끝나면('U: terminated' — 거절·[그만 보기]·서버 종료·시한) 송출은 알림 받은 상태로 남아 다시 [받기] 할 수 있다 ⑤ 'U: has
  permission' 에서 다시 온 Granted(answer `mc_granted` 뒤 서버가 따로 보낸 것)는 확인만 하고 SSRC 가 다르면 새 값으로 송출한다.
- **송출 SSRC** — 규격상 송출자는 Transmission Granted 의 Audio·Video SSRC 를 자기 RTP 에 쓴다(TS 24.581 §6.2.4.4.6 2). pjmedia 스트림 SSRC 는 스트림을
  만들 때 정해지고(호 중 바꾸는 API 가 없다) pjsua 는 offer 의 m-line 마다 `a=ssrc`(RFC 5576)를 광고한다. CMP 는 송출자를 멤버 전용 포트로 판별해
  내보낼 때 할당 SSRC 를 찍으므로([cmp_media_api.md](../../api/cmp_media_api.md) §7.9 SSRC 규칙) 단말이 SSRC 를 바꾸지 않아도 분배·수신자 구분은 맞다.
  CMP 는 멤버 offer 의 `a=ssrc`(JOIN `user_audio_ssrc`·`user_video_ssrc`)가 프로세스 전역에서 쓰이지 않으면 그 값을 할당하므로(§12.1.2.2 «equal to provided
  values … or different if the collision is detected») 충돌이 없는 한 단말 쪽도 규격 문언 그대로다.

### 5.5 앱

- **PTT 단말(ptt-client)** — 그룹 화면: 그룹이 MCVideo 를 지원하면 [영상 참여]/[영상 나가기], 참여 중 [영상 보내기](전송 요청 — PTT 버튼과 따로),
  새 송출 알림 → [받기](manual). 영상 오버레이(`VideoCallOverlay`)는 MCVideo 호의 수신 스트림을 그린다. [PTT] 는 MCPTT 호 그대로.
- **관제 앱(Windows·태블릿)** — 다중 스트림 수신(스트림 격자·선택 수신), 그룹 편집의 서비스 절 — Windows 쪽 몫.

### 5.6 콘솔 · OAM · 계측기

- 콘솔 그룹 편집 서비스 절·가입자 MCVideo 자격, 녹취·세션 이력 서비스 축 `mcvideo`, 통계(sip_statistics 의 서비스 축에 `mcvideo`).
- 계측기 — libcsim MCVideo 단말(REGISTER 태그·affiliation·chat 합류·MCV0/1/2), 시나리오 `MCVIDEO-GROUP-CHAT`·`MCVIDEO-TRANSMIT-RECEIVE`·
  `MCVIDEO-MAX-TRANSMITTERS`, 지표(송출 허가 시간·수신 허가 시간·영상 RTP 도달율). 계측기 코드는 팀원 트랙이라 요구만 넘긴다.

## 6. 개발 항목 (WP)

| WP | 대상 | 내용 |
|---|---|---|
| **V0 규격 정합 즉시분** | CSC·SDK | ① MCPTT `<service enabler>` = MCPTT ICSI(GMS 생성·XCAP PUT 해석·SDK `group_doc.cpp` 생성) ② 비규격 `<mcpttgi:mcptt-video>` 의 지위 정리 — 전환기 요소로 편차 표에 적고 V7 에서 걷는다(3GPP 네임스페이스에 새 비규격 요소를 더하지 않는다) |
| **V1 설정 평면** | CSC·DB·콘솔 | DB `mcvideo_group_attrs`·`mcvideo_user_profile` + 마이그레이션(`video_enabled`=1 그룹 → MCVideo 지원 행, §8) · 그룹 문서 MCVideo `<service>`·`<mcvideo-*>`·`<mcvideo-mcvideo-id>`(보호 false 명시) · CMS user profile·service config · ue-init-config `MCVideo-Service-Details` · scope 넷 · 관리 API·콘솔 서비스 절 · xcap-diff |
| **V2 CSP 호 제어** | CSP·psip | `CMcVideoAsModule`·`Roles.MCVIDEO`, ICSI 분기, 등록 태그·서비스 인가, 서비스별 affiliation·`mcvideoPresInfo`, mcvideo-info 코덱, chat·prearranged 그룹 호(개시·합류·재합류·퇴장·해제), SDP `udp MCVideo`, Warning 코드, CMP 명령 |
| **V3 CMP 전송·수신 제어** | CMP | MCVideo 그룹 종류, MCV0/1/2 코덱, 서버 상태 머신(§6.3.4~§6.3.7), 동시 송출 상한·우선순위 revoke, Active SSRC List 분배, 영상 RTCP(PLI), 영상 SRTP, 녹취 슬롯, HEARTBEAT 자원 |
| **V4 단말 SDK** | `libcimsue`·바인딩 | 계정·REGISTER 태그, 서비스별 affiliation, MCVideo 그룹 호, 전송 제어 참여자·수신 제어, 스트림별 렌더, C API·.NET·Kotlin, cimsue-cli |
| **V5 앱** | ptt-client · (관제 앱 = Windows 쪽) | [영상 참여/나가기]·[영상 보내기]·수신 알림 [받기], 음성 호와의 공존 정책(D6) |
| **V6 검증** | cspsim·계측기·verify | 단위(`S1-UNIT-CMP` MCV 코덱·상태 머신, `S1-UE-UNIT` mcvideo-info·전송 제어 참여자), S3 `S3-SCN-MCVIDEO-CHAT`·`-TRANSMIT`·`-RECEPTION`·`-MAX-TX`, 계측기 시나리오 요구(팀원 트랙) |
| **V7 현행 PTT 영상 제거** | 전 구간 | 1차 배포와 **같은 창**(전환 기간 없음 — §7 D9): CSP 가 MCPTT 세션의 `m=video` 를 port 0 으로 거절(RFC 3264 §6 — MCPTT 는 speech 만), 그룹 문서 `<mcpttgi:mcptt-video>`·`X-Video-Port`·CMP PTT 영상 분배·콘솔 «영상» 토글 제거, PTT 앱은 MCPTT 호에 영상을 제안하지 않는다. DB `video_enabled` 열 DROP 은 공유 DB 를 쓰는 전 사이트가 새 빌드가 된 뒤(§8) |
| **V8 후속** | — | 긴급·임박·경보(automatic 수신), 1:1(전송 제어 유무), 방송, video pull(단말·저장소)·push, ambient viewing, 송출 큐, ad hoc, conference 이벤트, pre-established(규격 미완 — §9), E2E([mcx_e2e_security.md](mcx_e2e_security.md) — protect true 전환), MBMS·off-network |

순서: V0 → V1 → (V2 ∥ V3) → V4 → V5 → V6 → V7. V1 만으로 규격 단말이 그룹의 MCVideo 지원을 읽고, V2·V3 가 서면 cimsue-cli 로 서버를 단독 검증할 수 있다.
2~3명 분담(트랙 A 제어·설정 / B 미디어 / C 단말)·먼저 합의할 계약 K1~K7·마일스톤은 [../../dev/mcvideo_dev_plan.md](../../dev/mcvideo_dev_plan.md).

## 7. 결정 사항

| # | 쟁점 | 권고 |
|---|---|---|
| D1 | MCVideo ID | **MCPTT ID 와 같은 값**(TS 23.280 §10.1.4.1 단일 MC service ID) — `mcdata_id = mcptt_id` 와 같은 규약, 요청은 ICSI 로 가른다 |
| D2 | 제어 기능의 자리 | CSP 안 새 모듈 `CMcVideoAsModule`(참여·제어 겸임). 그룹 세션 부품은 PTT-AS 와 공유하되 MCVideo 규칙(ICSI·본문·SDP·Warning)은 모듈 안에 |
| D3 | CMP 명령 | 서비스를 명시한 명령 — 새 명령(`MCVIDEO_*`) 또는 기존 PTT 명령의 `service` 필드. 권고 = **기존 명령 + `service` 필드**(포트·SRTP·녹취 필드를 한 번만 정의), 제어 차이는 CMP 그룹 종류가 가진다 |
| D4 | 그룹 모델 | 한 그룹 id 에 서비스 집합. DB = 서비스별 표(`mcvideo_group_attrs` 행 = 지원 · `mcvideo_user_profile` 행 = 자격 · `mcvideo_affiliations`) — 가입 표의 kind 별 분리(`ptt_subscriptions` 등)와 같은 구조. `video_enabled` 는 V7 까지 전환기로만 |
| D5 | 기본 호 종류 | **확정 — chat**(`mcvideo-on-network-invite-members` false). 원하는 사람만 영상에 들어오고, 합류가 곧 affiliation. prearranged(전원 초대)는 그룹 속성으로 고른다 |
| D6 | 음성 호와 영상 호의 공존 | **확정 — 둘 다 유지.** 규격 미정(§1.1)이라 단말 정책: 영상 참여 중에도 MCPTT 호에 남아 [PTT](하드웨어 PTT 키 포함)는 음성 호, [영상 보내기]는 영상 호. 두 호의 소리가 겹치면 **영상 호 송출 음성 우선**(무전 음성은 줄이거나 끈다). «영상만 쓰기»는 사용자가 MCPTT 호를 나가는 선택 |
| D7 | 보호 요소 | E2E 가 설 때까지 `mcvideo-protect-media`·`mcvideo-protect-transmission-control`·service config 보호 요소 **false 명시**(없으면 true 로 읽힌다). 구간 보호는 SRTP/SRTCP(media_security.md) |
| D8 | 수신 모드 | 일반 호 **manual**(TS 24.581 §6.3.6.3.3 — 서버가 '1'), 긴급·임박·방송·system 은 automatic. 관제 앱은 스트림 선택 UI 로 manual 을 쓴다 |
| D9 | 현행 PTT 영상의 전환 기간 | **확정 — 전환 기간 없음.** 검증 뒤 서버와 APK 를 한 번에 배포하고 같은 창에서 현행 PTT 영상을 걷는다(V7). 옛 APK 는 영상만 안 되고 음성 무전은 그대로 |

## 8. 전환 — 현행 PTT 영상 → MCVideo (전환 기간 없음)

1. **V1 마이그레이션** — `video_enabled = 1` 그룹마다 `mcvideo_group_attrs` 행을 만든다(= 그 그룹은 MCPTT + MCVideo). 표 추가만이라 공유 DB 의 옛 코드는 영향이 없다.
2. **1차 배포(한 창)** — 검증(§10)이 끝나면 서버(CSC·CSP·CMP·OAM)와 PTT·VoLTE APK 를 같은 창에 올린다. 이 배포부터 CSP 는 MCPTT 세션의 `m=video` 를 port 0 으로
   거절하고(RFC 3264 §6 — MCPTT 는 speech 만), 그룹 문서는 `<mcpttgi:mcptt-video>` 를 싣지 않으며, 콘솔·관제 앱의 «영상» 토글은 MCVideo 서비스 켜기로 바뀐다.
   새 PTT 앱은 MCPTT 호를 음성만으로 열고 영상은 MCVideo 호로 간다. 아직 바꾸지 않은 옛 APK 는 영상만 안 되고 음성 무전은 그대로다.
3. **열 정리** — 공유 DB(.45·.48·.135)를 쓰는 전 사이트가 새 빌드가 된 뒤 `video_enabled` 열을 DROP 한다(옛 CSP·CSC 가 그 열을 SELECT 한다).

## 9. 규격 판본 · 불일치 메모

원문끼리 어긋나는 곳 — 구현은 괄호의 쪽을 따르고 편차 표에 적는다.

- TS 24.281 §6.2.1 은 그룹 문서 `<preferred-voice-encodings>`·`<on-network-invite-members>` 등 **MCPTT 이름**을 참조하지만 TS 24.481 의 MCVideo 요소는
  `<mcvideo-preferred-audio-encodings>`·`<mcvideo-on-network-invite-members>` 다(→ TS 24.481 의 MCVideo 요소).
- mcvideo-info 루트 — 본문 설명의 `<mcvideo-info>` vs 스키마 `<mcvideoinfo>`(→ 스키마).
- `session-type` — §12.2.2.x 의 «one-to-one video pull» 등이 Annex F.1.3 값 목록에 없다(→ V8 에서 판본 재확인).
- 전송 요청 본문 MIME — F.5.1 `vnd.3gpp.transmission-request+xml` vs IANA 템플릿 `vnd.3gpp.mcvideo-transmission-request+xml`(→ V8, 1차 범위 밖).
- TS 24.581 §12.1.2 ABNF — `mc_priority` 1*2DIGIT(값 범위 1~255 와 어긋남), `mc_transmission_ssrc` 값 표기 누락(→ 본문 설명과 예시).
- 동시 송출 카운터 «Cx» 가 §11.2.3 목록에 없다(→ 그룹 속성 `mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members` 가 상한).
- TS 24.581 §12.1.2.3 ABNF 의 fmtp 파라미터 구분자 `COLON` vs §4.3.3.1 예시 `;`(→ `;` — MCPTT fmtp 와 같다).
- TS 24.581 Table 9.2.3.1-1 에 field ID 가 없는 필드 — Remote ID(§9.2.22)·SSRC of queued transmission participant(§9.2.5·§9.2.12)·Granted Party's
  Identity·SSRC of granted transmission participant(§9.2.9)·SSRC of transmission control server(§9.2.9)(→ 부호화하지 않는다. 모두 off-network·원격 송출 —
  1차 범위 밖).
- TS 24.581 Transmission Revoked(§9.2.10)·Transmission End Request(§9.2.20) 표의 «Reject Cause value» vs 본문 Reject Cause field(→ 필드). Queue Position Info
  의 Transmission Indicator 참조 절 9.2.3.15(→ 9.2.3.11). Transmission control ack subtype `00100`(x 자리 없음 — 값 4).
- TS 24.581 Transmission End Request/Response·Media Reception End Response 의 필드 표(§9.2.20·§9.2.21·§9.2.27)에 Transmission Indicator 가 없는데 참여자
  절차가 싣고 읽는다(§6.2.4.5.3 1·§6.2.4.6.4 3·§6.2.5.6.4 3)(→ 절차 — 세 메시지에 Indicator 를 허용). Ack 을 보내는 절차 대부분이 Message Type·Source 만
  말하지만 subtype 값이 MCV1·MCV2 사이에서 겹친다(→ Message Name 을 늘 싣는다 — §6.2.5.5.5 1c·§9.2.31 표).
- TS 24.581 원인 #4 가 가리키는 T9(Retry-after)가 §11 서버 타이머 표에 없다(→ 1차 범위에서 쓰지 않는다). 단말 T100~T104 와 서버 T2 는 규격 기본값이
  없다 — TS 24.484 는 T100~T104 를 초 단위 unsignedByte 로 둔다(→ CIMS 1 s, [mcvideo_tc_defs.yaml](mcvideo_tc_defs.yaml) `origin: cims`).
- TS 24.581 §14.2.7·§14.3.9 는 `mc_transmission_ssrc` 를 «다중화를 지원하면» 싣게 하지만 TS 24.281 §6.3.3.1.1 4)·§6.3.3.2.1 2)b)(제어 기능 offer·answer)는
  «shall include»(→ 서버는 늘 싣는다 — 다중화 여부와 무관하게 해가 없다).
- TS 24.581 §14.3.1 «answer 의 fmtp 는 offer 에 없던 파라미터를 싣지 않는다» vs §12.1.2.2·§14.3.7·§14.3.8·§14.4 의 `mc_audio_ssrc`·`mc_video_ssrc`(offer 에 없어도
  암묵 요청을 받아들인 answer 가 싣고 offerer 가 쓴다)(→ 두 값은 answer 전용 예외).
- TS 24.281 §6.3.3.1.2 3) «P-Asserted-Service-Id header field»(→ RFC 6050 의 헤더 이름 `P-Asserted-Service` — MCPTT 쪽 구현과 같다).
- TS 24.281 §6.3.8.1 1)·§6.3.8.2 1) «T4 (Inactivity)» vs TS 24.581 §6.3.4.3.5 «T1 (Inactivity)»(T4 = Transmission Granted) — MCPTT(TS 24.380) 이름을 옮긴
  흔적(→ TS 24.581 의 T1 — CMP 이벤트 `TRANSMISSION_INACTIVITY{timer:"T1"}` 에서 prearranged 해제).
- §6.3.8.1 2) «only one or no participants» 는 호 종류를 가리지 않지만, chat 은 참가자가 모이기를 기다리는 세션이라 **0명**일 때만 해제한다(편차 — CIMS
  MCPTT chat 과 같은 규칙: on-demand 호만 1명 이하 해제, `GroupCallService::OnCallTerminated`). 3)(개시자 이탈 해제 — 로컬 정책)은 두지 않고, 4)(최소
  제휴 인원)는 확인 통화와 함께 1차 범위 밖.
- 세션 갱신 주체 — 제어 기능의 멤버 INVITE 는 refresher 를 «shall be omitted»(TS 24.281 §6.3.3.1.2 6)), 단말의 그 200 OK 는 `refresher=uas`
  (§6.2.3.1.1 5)), 제어 기능의 200 OK(단말 개시·합류)는 `uac`(§6.3.3.2.3.2 2)) — 어느 쪽이든 단말이 갱신한다. MCPTT(TS 24.379 §6.3.3.1.2)는 «생략 권고,
  싣는다면 uac» 라 CIMS 가 `uac` 를 싣는다(mcptt_standard_conformance.md C4a) — MCVideo 는 규격대로 생략하고, psip 가 로컬 정책으로 싣는 값을 지운다
  (`McvStripSessionRefresher`). pjsip UAS 기본(요청에 refresher 가 없으면 uac)은 SDK 가 uas 로 고친다(.45 e027c0b5).
- TS 24.484 MCVideo service configuration — XSD 요소 `C7-reception-accpeted` vs 본문 `C7-reception-accepted`(§9.4.2.1·§9.4.2.7), 본문 구조의
  `T103-receive-media-requset` vs XSD `T103-receive-media-request`(→ XSD 표기 — 스키마 검증·XSD 기반 단말과 맞는다), MIME 이름 «vnd.3gpp.mcvideo-service-config+xml»
  (§9.4.2.5 — `application/` 누락, → `application/vnd.3gpp.mcvideo-service-config+xml`).
- TS 24.484 MCVideo user profile — 문서 이름 §9.3.2.6 «mcvideouserprofile<index>.xml» vs 같은 절 phrase·§9.3.1A «mcvideo-user-profile-<index>.xml»
  (→ 후자, CSC 는 이름을 가리지 않는다) · `<RemoteGroupSelectionURIList>` 본문 «one or more entry» vs XSD entry 0 개 허용(→ 원격 선택 권한이 없으면 빈 목록).
- pre-established session — TS 24.281 §22.2.2.2 Editor's Note «will be defined in the future»(→ V8 까지 on-demand 만).
- 동시 세션 — TS 23.281 §7.11 만 있고 TS 24.281 §6 Editor's Note(→ 두 서비스의 독립 다이얼로그로 충분, 단일 다이얼로그 다중화는 하지 않는다).
- TS 24.581 T1·T5 시작 — §6.3.4.3.2·§6.3.6.3.2 는 'Start-stop' 밖에서 Idle 로 들어갈 때만 T1(Inactivity)·T5(Reception Inactivity)를 시작한다(→ 호 시작부터
  돌린다 — 무송출 호도 hang timer 뒤 풀린다, §5.3.1).
- TS 24.581 T6 — 'Gr: Reception accepted' 의 허가(§6.3.6.4.3 f)에만 있고 'Gr: Reception Idle' 의 첫 허가(§6.3.6.3.6)에는 없다(→ 허가마다 시작, 멈춤 = Transmission
  Control Ack·수신 종료 — 규격은 멈춤 조건을 End Request/Response 만 적는다).
- TS 24.581 not permitted 상태의 Transmission End Request(요청·대기 취소 — §6.3.5.3.7·§6.3.5.4.5·§6.3.5.7.4)에 서버 절차는 Idle·Notification 만 적지만
  참여자는 'U: pending end of transmission' 에서 End Response 만 기다린다(§6.2.4.4.7·§6.2.4.9.4·§6.2.4.6.4)(→ End Response 를 함께 보낸다).
- TS 24.581 Transmission control Ack 절차의 «Message Type field set to '4' (Transmission End Request)»(§6.3.5.3.7 등) vs 부호화 §9.2.3.10 «5 bit message
  subtype»(End Request = 0)(→ 부호화 절 — 받은 subtype).
- TS 24.581 Media Transmission Notification 에 Message Sequence Number 를 싣게 하는 절차(§6.3.4.4.2 3c·§6.3.5.4.5 2c) vs 메시지 표 Table 9.2.13-1(그 필드
  없음)(→ 표 — 코덱이 표 밖 필드를 거절한다. Transmission Idle 에만 싣는다).
- TS 24.581 Receive Media Response 거절 원인 — §6.3.7.3.4·§6.3.7.4.10 의 cause #0(Insufficient downlink bandwidth)·#1(No permission to receive)이 원인 표
  §9.2.15.2(2·4·5·6·7·255)에 없다(→ 표 — 없는 송출을 가리킨 요청은 #255, C9 상한은 #7).
- TS 24.581 §6.3.4.3.3 «only one participant» #3 vs 암묵적 송출 요청 — 개시자 혼자인 prearranged 개시에서 문언대로면 늘 거절된다(→ §6.3.2.2 대로 첫 초대 참가자가
  수락할 때 허가, 미디어 버퍼링 없음 — §5.3.1). 참여자는 암묵 요청이 받아들여진 뒤 Granted 를 T100×C100(CIMS 3 s) 기다리고 만료되면 'U: has no permission'
  (§6.2.4.4.4) — 그 뒤 온 Granted 는 규격상 처리 절차가 없어 버린다.
- RFC 3261 §19.1.1 표 1 — To·From URI 에는 port·transport 파라미터를 둘 수 없다. 재합류 INVITE 의 To 는 세션 식별자에서 둘을 뺀 값이다(pjsip — 골든 08
  의 To 는 둘을 싣는다) → 제어 기능은 세션을 R-URI(또는 To 의 사용자부·`gr`)로 가른다.

## 10. 검증 기준

- **V0** — 그룹 문서 `enabler` = ICSI(`tests/test_csc_gms_group_crud.py`·`csc_test`), 옛 앱·새 SDK 가 그룹 문서를 그대로 읽는다.
- **V1** — MCVideo 지원 그룹의 문서가 TS 24.481 스키마로 검증된다(두 `<service>`, `<mcvideo-video-media>`, 보호 false), user profile·service config 스키마 검증,
  ue-init-config `MCVideo-Service-Details`, scope 요청 ∩ 카탈로그.
- **V2·V3** — cimsue-cli 두 대: chat 합류 200(`udp MCVideo`) · 송출 요청 Granted(Audio·Video SSRC) · 다른 멤버 Media Transmission Notification →
  Receive Media Request 전에는 영상 RTP 0, 뒤에는 도달 · 동시 송출 상한 초과 Rejected · 퇴장 BYE 뒤 MCPTT 호 영향 없음 · 영상 SRTP · PLI 가 송출자에게 도달.
- **V5** — 사내 단말(MF52·W999) 실기: 같은 그룹에서 [PTT] 음성과 [영상 참여]·[영상 보내기]·[받기]를 번갈아, 영상 나간 뒤 음성 호 유지.
- **V7** — MCPTT 세션 `m=video` 오퍼에 port 0 answer, 그룹 문서에 `mcptt-video` 없음, 회귀 = PTT·MCData 계측기 시나리오 전부(`PTT-GROUP-CALL-VIDEO`·`S6-SCN-PTT-VIDEO` 는 MCVideo 시나리오로 대체).
