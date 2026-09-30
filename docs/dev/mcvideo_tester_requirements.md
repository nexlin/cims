# MCVideo 계측기 요구서 (B10 — 계측기 트랙에 전달)

[mcvideo_dev_plan.md](mcvideo_dev_plan.md) B10. 계측기(`oam-cims-tester` — [test_instrument.md](../design/features/test_instrument.md))가 MCVideo 그룹 호를
부하·기능 시험하려면 무엇이 있어야 하는지 적는다. 서버 동작의 정본은 [mcvideo.md](../design/features/mcvideo.md) §5.2.1(호 제어)·§5.3·§5.3.1(전송·수신 제어),
와이어 모양의 정본은 계약 골든 `tests/fixtures/mcvideo/`(K2 설정 문서·K3 SIP)과 K4 SDP 프로파일(mcvideo.md §1.4)이다. 현행 «PTT 영상»
시나리오(`PTT-GROUP-CALL-VIDEO` · `S6-SCN-PTT-VIDEO`)는 V7(현행 PTT 영상 제거)과 함께 아래 MCVideo 시나리오로 바뀐다.

## 1. 단말 (libcsim — UE 풀 `service: mcvideo`)

| 능력 | 규격 · 모양 |
|---|---|
| 등록 | Contact 에 `+g.3gpp.mcvideo` + `+g.3gpp.icsi-ref` 목록에 MCVideo ICSI(`urn:urn-7:3gpp-service.ims.icsi.mcvideo`) — 골든 01. MCPTT 태그와 함께 실어도 된다 |
| affiliation | PUBLISH `Event: presence`, R-URI = MCVideo PSI, multipart(mcvideo-info `<mcvideo-request-uri>` = 자기 MCVideo ID + pidf `mcvideoPresInfo` 그룹 목록), `Expires: 4294967295`(골든 02 — TS 24.281 §8.2.2.2.3). 해제 = Expires 0 |
| 그룹 호 개시·합류 | INVITE R-URI = PSI, Accept-Contact 둘(require;explicit), `P-Preferred-Service` MCVideo ICSI, mcvideo-info(session-type chat/prearranged · request-uri 그룹 · client-id), SDP = audio(AMR-WB, `i=`)·video(H.264 + `rtcp-fb nack pli`)·`m=application <port> udp MCVideo` + `a=fmtp:MCVideo mc_priority=…;mc_transmission_ssrc=…`(골든 03·05). 암묵 요청 = fmtp `mc_implicit_request;mc_granted`(prearranged 새 세션만) |
| 재합류 | R-URI = 포커스 Contact 의 세션 식별자(`gr`) — 골든 08 |
| 초대 수락 | 제어 기능의 INVITE(골든 07 — Session-Expires refresher 없음)에 200 OK: 자기 SDP + 200 의 `Session-Expires: …;refresher=uas`(TS 24.281 §6.2.3.1.1 5)) |
| 전송 제어 참여자 | RTCP APP `MCV0`/`MCV1`/`MCV2`(TS 24.581 §9) — Transmission Request·End Request·Receive Media Request·Media Reception End Request·Ack 를 내고 Granted·Rejected·Revoked·Idle·Media Transmission Notification·Receive Media Response·Queue Position Info 를 읽는다. 상수·필드 = 전송 제어 정의 테이블 `docs/design/features/mcvideo_tc_defs.yaml`(생성기 `scripts/gen_mcvideo_tc_defs.py` — CMP·SDK 와 같은 헤더를 만든다). 제어 채널에 빈 RR keepalive(NAT) |
| 미디어 | 허가(Granted) 동안만 audio·video 송출(동봉 샘플 `sample_voice.amrwb`·`sample_video.h264`), 수신 = Receive Media Request 허가 뒤(manual) — 받은 RTP 의 SSRC = Notification·Response 의 SSRC(CMP 할당), PT = 자기 SDP 값. 수신자는 키프레임이 없으면 PLI(`rtcp-fb nack pli`)를 낸다 |
| SRTP | 접속서비스 `media_srtp` 사이트 — audio·video m= 라인마다 따로 SDES 키(RFC 4568 §6.1). 제어 채널은 SDES 대상이 아니다 |

## 2. 단계 어휘 (시나리오 YAML)

| 단계 | 뜻 · 인자 | 대응 drive 명령(`cimsue-cli`, .45 C8) |
|---|---|---|
| `video_call` | from 이 그룹 호 개시·합류 — `group`, `payload: chat\|prearranged`, `implicit: true`(prearranged 새 세션), `expect.code`(4xx 거절 기대 — 404 113/117/118, 403 116/108/109, 486 103, 488) | `video_call <group> [prearranged] [queueing] [implicit]` |
| `video_answer` | 역할이 초대(골든 07)를 받아 수락 — 시한 안에 오지 않으면 실패. 단말은 MCVideo 초대를 자동 수락한다(자동 개시 TS 24.281 §6.2.3.1) | (명령 없음 — 시한 안에 `call{service:mcvideo,state:active,dir:incoming}` 을 본다. 코어 `autoAnswerMcvideo` 기본 참) |
| `transmit_request` | who 가 Transmission Request — `payload` = 기대 결과 `granted`(기본)·`rejected`·`queued`·`any`, `priority`(선택) | `transmit_request <call> [priority]` |
| `transmit_release` | who 가 Transmission End Request | `transmit_release <call>` |
| `reception_accept` | who 가 송출 하나를 받기 시작(Receive Media Request — `from` = 송출 역할) | `reception_accept <call> <userId>` |
| `reception_end` | who 가 받기를 끝냄(Media Reception End Request) | `reception_end <call> <userId>` |
| `bye` | who 가 떠남 — 해제 규칙 판정은 `expect`(prearranged 1명 이하·chat 0명이면 서버 BYE) | `hangup` |
| (기동 절차) | MCVideo affiliation — prearranged 는 개시자·멤버 모두 먼저 제휴한다(미제휴 개시·합류 = 403 120, TS 24.281 §9.2.1.4.2 13)a)) | `affiliate <group> on\|off mcvideo` (결과 = `request{method:PUBLISH,op:affiliate}`) |

그룹 세션 규약은 PTT `group_call` 과 같다(인스턴스 하나 = 그룹 하나, 단일 역할에 멤버 하나씩·`multi: true` 역할에 나머지). `real-ue` 풀은 위 drive 명령과
이벤트(`transmission`·`reception`, `incoming`·`call` 의 `service`)로 가상 단말과 같은 Event 로 풀어 `REAL_UE_STEPS` 에 더한다.

## 3. 지표

| 지표 | 정의 |
|---|---|
| `mcv_affiliate_ms` | PUBLISH → 200 |
| `mcv_setup_ms` | INVITE → 200 (prearranged 는 첫 멤버가 붙은 뒤 200 — 서버 동작) |
| `mcv_fanout_ms` | prearranged: 개시 INVITE → 멤버 쪽 초대 도착 |
| `tx_grant_ms` · `tx_grant_pct` | Transmission Request → Granted · 기대 `granted` 중 허가 비율 |
| `tx_notify_ms` | Granted → 다른 참가자의 Media Transmission Notification |
| `rx_grant_ms` | Receive Media Request → Response(granted) |
| `video_rx_pct` | 수신 허가 뒤 영상 RTP 가 온 수신자 비율 |
| `keyframe_ms` | 수신 허가 → 첫 IDR (CMP 가 수신 시작 때 송출자에게 PLI — mcvideo.md §5.3 B6) |
| `tc_retx` | 서버 메시지 재송신 관측 수(T2·T4·T6 — 0 이 정상) |

## 4. 동봉 시나리오 (제안)

| 이름 | 흐름 | 서버 확인 (mcvideo.md §5.2.1) |
|---|---|---|
| `MCVIDEO-CHAT-BASIC` | A·B chat 합류 → A 송출 → B [받기] → 영상 도달 → A 끝 → 둘 다 떠남 | 수신 전 영상 0 · 뒤 도달 · 해제(0명) |
| `MCVIDEO-PREARRANGED-IMPLICIT` | B affiliation → A prearranged + implicit → B 초대 수락 | A 200 이 B 뒤 · answer `mc_implicit_request;mc_granted` |
| `MCVIDEO-TX-LIMIT` | 상한 1 그룹 — A 송출 중 C 요청 | Rejected #1 (queueing 이면 Queue Position Info) |
| `MCVIDEO-REJOIN` | 진행 중 prearranged 에 `gr` 로 재합류 | 같은 세션, 암묵 요청 없음 |
| `MCVIDEO-RELEASE-RULES` | prearranged 2명 중 1명 BYE · 송출 없이 T1 | 서버 BYE · TRANSMISSION_INACTIVITY 해제 |
| `MCVIDEO-WITH-MCPTT` | 같은 그룹에서 MCPTT 그룹 호와 MCVideo 그룹 호 동시 | 서로 무영향(CMP 자원 키 `(service, group)`) |
| `MCVIDEO-TYPE-MISMATCH` | chat 그룹에 prearranged INVITE / 반대 | 404 Warning 118 / 117 |

## 5. 전제 (시나리오 `fixtures:`)

- MCVideo 그룹 = CSC 관리 API `POST/PUT /api/v1/ptt/groups` 의 `mcvideo` 객체(admin_api.md §6), 이용 자격 = `PUT /api/v1/users/{pid}/ptt/{msisdn}/mcvideo`
  (§5.4). 계측기 컨트롤러 `tester_fixtures` 에 fixture 종류 `mcvideo_group`·`mcvideo_entitlement` 를 더해 적용·확인·역순 복원한다(DB 직접 쓰기 없음).
- 대상 CSP `Setup.Roles.MCVIDEO` 켜짐 · CMP `resource.mcvideo` 광고 · 공유 DB `sql/migrate_mcvideo.sql` — 계획 미리보기(`runs/plan`)가 셋을 확인한다.

## 6. 받아들이는 기준

- libcsim 단위시험: MCV0/1/2 부호화·해석이 생성 헤더를 쓴다(CMP `PTransmissionCodec`·SDK `tc_codec` 과 교차 — 같은 정의 테이블) · 골든 01~08 모양 생성.
- 동봉 7종이 .48 배포본에서 pass, `PTT-GROUP-CALL-VIDEO`·`S6-SCN-PTT-VIDEO` 다리가 `MCVIDEO-CHAT-BASIC` 로 옮겨진다(V7 과 같은 창).
