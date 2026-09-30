# MCVideo — 그룹 영상 서비스 (TS 24.281 · TS 24.581)

> **그룹 영상의 정본 설계.** 3GPP 는 영상을 MCPTT 세션에 얹지 않고 별도 MC 서비스 **MCVideo** 로 둔다. 한 그룹을
> MCPTT 와 MCVideo 두 서비스용으로 설정하면(TS 23.280 §3 «MC service group … configured for the use with one or more MC services»),
> 단말은 같은 그룹에서 **음성만 = MCPTT 그룹 호**, **음성+영상 = MCVideo 그룹 호**를 골라 쓰고 둘 사이를 오간다.
> 이 문서는 규격 모델, 현행 «PTT 영상»(MCPTT 세션의 `m=video` — 비규격)과의 차이, 규격형으로 옮기는 개발 항목·결정 사항을 정한다 —
> **설계 정본, 미구현.**
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
| 그룹 설정 | DB `ptt_groups.video_enabled` 하나. 그룹 문서 `<mcpttgi:mcptt-video>` — **TS 24.481 스키마에 없는 요소를 3GPP 네임스페이스에** 싣는다. MCPTT `<service enabler="example.mcptt">` — **ICSI 가 아닌 자리표시 값**(csc `services/mcptt.py` `get_group_xml`, SDK `csc/group_doc.cpp`) | MCPTT·MCVideo `<service>` 각각 ICSI enabler + `<mcvideo-*>` 속성 |
| 영상 유무 | 호 개시 때 한 번 — 앱은 늘 `video=true` 로 제안, 서버가 `video_enabled` 그룹만 받는다. 진행 중 추가·제거 없음 | 음성 = MCPTT 호, 음성+영상 = MCVideo 호(따로 합류·퇴장) |
| 송출 | floor 보유자만(single/dual/multi-talker 최대 8). 앱은 «내 영상 보내기» 켜기/끄기만(`setVideoSend`, 재협상 없음) | 송출 요청·허가(Transmission Request/Granted), 동시 송출 상한 |
| 수신 | 영상 그룹 멤버 전원 자동 수신 | 수신자가 스트림을 골라 받는다(manual), 긴급·방송은 자동 |
| 서비스 신원 | ICSI·특성 태그·scope·user profile·service config·ue-init-config 에 MCVideo 없음(`SCOPE_CATALOG` 는 mcvideo 요청을 버린다) | §1.2·§1.6 |

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

- **DB** — 서비스별 표: `mcvideo_group_attrs`(그룹 id 외래키 — 행이 있으면 그 그룹이 MCVideo 를 지원한다. 열 = invite_members·max_duration·
  max_transmitters·preferred_audio/video_encodings·resolutions·frame_rate·reception_hang_timer·min_number_to_start·group_priority·protect_media(0)·
  protect_tc(0)), `mcvideo_user_profile`(사람 — 행이 MCVideo 이용 자격. 열 = max_simultaneous_video_streams·N2·N6·ruleset allow-*).
  멤버 entry 의 `mcvideo-mcvideo-id` 는 MCPTT ID 와 같은 값이라 열을 두지 않는다(§7 D1).
- **그룹 문서(GMS)** — `get_group_xml`: MCPTT `<service enabler="urn:urn-7:3gpp-service.ims.icsi.mcptt">`(자리표시 값 교체) · MCVideo 지원 그룹이면
  MCVideo `<service>`(ICSI enabler, `<mcvideo-video-media>`) + `<mcvideo-*>` + entry `<mcvideo-mcvideo-id>`. XCAP PUT 해석도 같은 요소를 읽는다
  (`parse_group_document_xml`). SDK `GroupDoc` 도 같은 요소.
- **CMS** — `org.3gpp.mcvideo.user-profile`·`org.3gpp.mcvideo.service-config` 라우트와 문서 생성(기존 MCPTT 문서 생성기와 같은 틀), xcap-diff 통지 축에 두 문서.
- **ue-init-config** — `<anyExt><MCVideo-Service-Details>`(설정 `UeInitConfig.ServiceDetails.McVideo.{Enable,ServerUri}`, 기본 PSI `sip:mcvideo_psi@<PTT 도메인>`).
- **IdMS** — `SCOPE_CATALOG` 에 `3gpp:mc:video_*` 넷, 리소스 서버 `require_scope`(GMS·CMS 의 MCVideo 문서), 토큰 claim 은 [mcx_identity_scope.md](mcx_identity_scope.md) 규약.
- **관리 API·콘솔** — 그룹 편집 «서비스» 절(MCPTT·MCVideo 켜기, MCVideo 속성), 가입자 PTT 회선 카드 옆 «MCVideo» 자격·상한. CSC 가 CSP 에 `GROUP_CHANGED`·
  사용자 변경을 통지하는 경로는 그대로.

### 5.2 CSP (호 제어)

- **모듈** — `CMcVideoAsModule`(`IModule`, `Setup.Roles.MCVIDEO`) — 참여·제어 기능 겸임(PTT-AS·MCDATA-AS 와 같은 구성). `ModuleDispatcher::EventIncomingCall`
  에서 그룹 호 분기 앞에 **ICSI mcvideo(Accept-Contact / P-Preferred-Service) 또는 mcvideo-info 본문**으로 가른다 — 현행은 들어오는 INVITE 의 ICSI 를 보지 않는다.
- **등록** — Contact 의 `+g.3gpp.mcvideo` 를 바인딩 능력으로 기록(`UserMap` 능력 검사 확장), mcvideo-info 토큰·poc-settings PUBLISH 로 MCVideo 서비스 인가(§1.2).
- **affiliation** — 서비스 축을 가진 affiliation 기록(현행 MCPTT affiliation 저장에 service 열), `mcvideoPresInfo` NOTIFY, 암묵적 affiliation(chat 합류).
- **그룹 호** — `McVideoCallService`: chat·prearranged 개시·합류·재합류·퇴장·해제(T1·최대 시간), 그룹 종류 검사(§6.3.5.2), 멤버 fan-out(prearranged), mcvideo-info
  부호화·해석(규격 contentType 자식 형식 — mcptt-info 와 같은 코덱 틀), SDP 합성(audio·video·`udp MCVideo` — psip `CSipCallRtp` 에 제어 채널 프로토콜 이름을
  서비스별로), 응답 Warning 코드(117·118 등 — TS 24.281 §4.4). 그룹 세션 캐시·CMP 명령·구독은 `GroupCallService` 의 부품을 공유 헬퍼로 뽑아 쓴다.
- **CMP 연동** — `MCVIDEO_GROUP_ADD`·`MCVIDEO_JOIN`·`MCVIDEO_LEAVE` 또는 기존 PTT 명령에 `service: mcvideo`(§7 D3), 멤버마다 audio·video·control 포트,
  영상 SRTP(`media_crypto_video`) 를 처음부터 싣는다.

### 5.3 CMP (미디어 · 전송 제어)

- **그룹 종류** — `PMcvideoGroup`(또는 `PMcpttGroup` 과 공통 기반 + 서비스별 제어기): 멤버 단위 = audio·video RTP + 제어(RTCP) — `PPttMemberPort` 풀 재사용,
  영상 RTCP 도 연다(PLI·FIR 를 송출자에게 — 현행 결함 해소).
- **전송 제어 서버** — TS 24.581 §6.3.4~§6.3.7: 동시 송출 상한(그룹 속성), 우선순위 revoke, (후속) 큐. 참여자별 상태, T1~T6·T11.
- **수신 제어** — 수신자별 Active SSRC List: 허가된 송출의 audio·video 만 그 수신자에게 보낸다. manual/automatic 모드, 수신자 동시 스트림 상한.
- **코덱** — RTCP APP `MCV0`·`MCV1`·`MCV2` 부호화·해석(`PFloorCodec` 과 나란한 `PTransmissionCodec`, 필드 표 §9.2.3), 제어 SRTCP 는 `PFloorCrypto` 재사용.
- **녹취** — 송출마다 슬롯 트랙(audio·video) — `PSyncRtpRecorder` 재사용, 색인 서비스 축 `mcvideo`([recording.md](recording.md)).

### 5.4 단말 SDK (`libcimsue`)

- **계정** — `AccountConfig.mcvideoServerUri`(ue-init-config), REGISTER Contact 에 MCVideo 태그(서비스 사용 여부 = `mcvideoEnabled`).
- **affiliation** — `affiliate(groupId, on, service)` 서비스 인자.
- **호** — `joinVideoGroupCall(groupId, {chat|prearranged})` → `CallInfo.service = mcvideo`, 나가기 = 기존 `hangup`.
- **전송 제어 참여자** — `requestTransmission`·`releaseTransmission`(§6.2.4 상태 머신, T100·T101), 이벤트 `onTransmission`(Granted·Rejected·Revoked·Idle·
  Media Transmission Notification — 송출자·SSRC).
- **수신 제어** — `acceptReception(callId, transmitterId)`·`endReception`(§6.2.5, T103·T104), 스트림별 렌더 창(현행 «호별 수신 창» 과제와 합친다 — ue_sdk.md §11).
- **바인딩** — C API·.NET·Kotlin 같은 이름(현행 그룹 영상 옵션 누락도 이때 메운다). `cimsue-cli video-call <g> [--transmit-at S] [--accept]`.

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
| D4 | 그룹 모델 | 한 그룹 id 에 서비스 집합. DB = 서비스별 표(`mcvideo_group_attrs` 행 = 지원) — 가입 표의 kind 별 분리(`ptt_subscriptions` 등)와 같은 구조. `video_enabled` 는 V7 까지 전환기로만 |
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
- pre-established session — TS 24.281 §22.2.2.2 Editor's Note «will be defined in the future»(→ V8 까지 on-demand 만).
- 동시 세션 — TS 23.281 §7.11 만 있고 TS 24.281 §6 Editor's Note(→ 두 서비스의 독립 다이얼로그로 충분, 단일 다이얼로그 다중화는 하지 않는다).

## 10. 검증 기준

- **V0** — 그룹 문서 `enabler` = ICSI(`tests/test_csc_gms_group_crud.py`·`csc_test`), 옛 앱·새 SDK 가 그룹 문서를 그대로 읽는다.
- **V1** — MCVideo 지원 그룹의 문서가 TS 24.481 스키마로 검증된다(두 `<service>`, `<mcvideo-video-media>`, 보호 false), user profile·service config 스키마 검증,
  ue-init-config `MCVideo-Service-Details`, scope 요청 ∩ 카탈로그.
- **V2·V3** — cimsue-cli 두 대: chat 합류 200(`udp MCVideo`) · 송출 요청 Granted(Audio·Video SSRC) · 다른 멤버 Media Transmission Notification →
  Receive Media Request 전에는 영상 RTP 0, 뒤에는 도달 · 동시 송출 상한 초과 Rejected · 퇴장 BYE 뒤 MCPTT 호 영향 없음 · 영상 SRTP · PLI 가 송출자에게 도달.
- **V5** — 사내 단말(MF52·W999) 실기: 같은 그룹에서 [PTT] 음성과 [영상 참여]·[영상 보내기]·[받기]를 번갈아, 영상 나간 뒤 음성 호 유지.
- **V7** — MCPTT 세션 `m=video` 오퍼에 port 0 answer, 그룹 문서에 `mcptt-video` 없음, 회귀 = PTT·MCData 계측기 시나리오 전부(`PTT-GROUP-CALL-VIDEO`·`S6-SCN-PTT-VIDEO` 는 MCVideo 시나리오로 대체).
