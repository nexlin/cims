# MCPTT·MCData 계측기 요구서 — 규격형 요청·서비스 인가 (계측기 트랙에 전달)

[conformance_gap_plan.md](conformance_gap_plan.md) K4 — 계측기(`oam-cims-tester` · `tester/worker` · libcsim(`cspsim/`) · cspsim)는 팀원 트랙이라 .48 은
고치지 않고 요구만 넘긴다. CSP 는 규격 형식**만** 받는다(전환기 없음). 그래서 지금 계측기 가상 단말(`ue` 풀 `service: ptt`·MCData 단계)의 MC 요청은
.48 에서 거절된다. 이 문서는 무엇을 맞춰야 다시 도는지 적는다. 서버 동작의 정본은 [mcptt_standard_conformance.md](../design/features/mcptt_standard_conformance.md)
C4a·C4d·C6·C10 · [mcdata_messaging.md](../design/features/mcdata_messaging.md) §4 이다. 와이어 모양의 정본은 계약 골든 `tests/fixtures/mcptt/sip/`
(01~19 — SDK 와 CSP 가 같은 파일로 시험한다)이다. MCVideo 는 [mcvideo_tester_requirements.md](mcvideo_tester_requirements.md) 가 따로 있다.

## 1. 지금 거절되는 것 (libcsim 코드 읽기 — `cspsim/SimSession.cpp`)

| libcsim 지금 | CSP 응답 | 규격 · 골든 |
|---|---|---|
| 서비스 인가를 보내지 않는다 — REGISTER 뒤 바로 제휴·호·SDS | MCPTT INVITE·경보 MESSAGE · MCData MESSAGE·SDS MSRP INVITE 전부 404 + `141 user unknown to the participating function` | TS 24.379 §7.3 · §10.1.1.3.1.1 2a) · TS 24.282 §9.2.2.3.1 3) — 골든 14·19 |
| Answer-Mode Indication(poc-settings)을 보내지 않는다 | 그 단말은 그룹 호 초대를 받지 못한다(480 + `146` 으로 센다), 개별 호 착신이면 발신자가 480 146 | TS 24.379 §10.1.1.3.2 3) · §11.1.1.3.2 7a) |
| 그룹 호 INVITE — Request-URI = 그룹 URI, Accept-Contact·`P-Preferred-Service`·mcptt-info 없음(긴급·일제·애드혹만 mcptt-info) | 403(Accept-Contact) · 404 | TS 24.379 §10.1.1.2.1.1 · §10.1.1.4.2 3) — 골든 12(개시와 같은 모양) |
| 제휴 = `Event: mcptt` + `application/vnd.3gpp.mcptt-affiliation-command+xml` 를 그룹 URI 로 | 아직 받는다 — 규격 갭 WP S12·S13 에서 걷는다 | TS 24.379 §9.2.1.2 (PUBLISH `Event: presence` · pidf `mcpttPI10:affiliation`) |
| 1:1 SDS·FD — 대상 = Request-URI | 403 204·205 | TS 24.282 §9.2.2.2.1 2) — 골든 05·06·08 |
| conference 구독 — Request-URI = 그룹 URI | 404 137 | TS 24.379 §10.1.3.2 — 골든 10·11 |

## 2. 단말 (libcsim) — 해야 할 것

| 능력 | 규격 · 모양 |
|---|---|
| 등록 | Contact = `+g.3gpp.mcptt` + `+g.3gpp.icsi-ref` 한 목록(MCPTT ICSI, MCData 를 쓰면 `…icsi.mcdata` 도) + `+sip.instance`(MC client 의 Instance ID URN). 서비스 태그를 뺀 재등록은 그 서비스의 로그오프다(§7.2.1 NOTE 1) |
| 서비스 인가 + 서비스 설정 | REGISTER 200 뒤 서비스마다 PUBLISH — Request-URI = 참여 기능 PSI(`sip:mcptt_psi@<PTT 도메인>` · `mcdata_psi`), `P-Preferred-Service` = 서비스 ICSI, `Event: poc-settings`, `Expires: 4294967295`, multipart = `<…-info>`(`<…-access-token>` = IdMS 접근 토큰 · `<…-client-id>`) + `application/poc-settings+xml`(`<entity id>` = Instance ID · `<am-settings><answer-mode>automatic\|manual` · `mcs10Set:selected-user-profile-index` 1 · `mcs10Set:multiplex-support` false — MCData 는 answer-mode 없음) — 골든 14. 200 을 받은 뒤에 제휴·호를 보낸다. 403 101 = 토큰 문제(scope·만료·MC ID ≠ 등록 신원) |
| 접근 토큰 | 이미 있는 `AcquireXcapToken`(IdMS `/idms/authreq` → `/idms/tokenreq`, PKCE)을 쓴다. scope 에 `3gpp:mc:ptt_service`·`3gpp:mc:data_service` (MCVideo 면 `3gpp:mc:video_service`). 토큰의 `mcptt_id`(MCData `mcdata_id`)가 등록 IMPU 사용자부와 같아야 한다(단일 MC service ID — [mcx_identity_scope.md](../design/features/mcx_identity_scope.md) §1) |
| 재인가 | 어느 MC 요청이든 404 141 이면 인가 PUBLISH 를 다시 보낸다(CSP 재기동으로 바인딩이 사라진 경우 — 바인딩은 메모리). 망 변경·재등록(새 등록)도 다시 보낸다 |
| 로그오프 | 등록 해제 전 `Expires: 0` + `SIP-If-Match`(200 의 `SIP-ETag`) — 골든 16. 서버는 그 서비스의 설정·제휴·바인딩을 지운다 |
| 제휴 | PUBLISH `Event: presence`, Request-URI = PSI, multipart(mcptt-info `<mcptt-request-uri>` = 자기 MCPTT ID + pidf `mcpttPI10:affiliation group=…`), `Expires: 4294967295` (TS 24.379 §9.2.1.2) — 인가 200 뒤 |
| 그룹 호 개시·합류 | INVITE Request-URI = PSI, Accept-Contact 둘(`+g.3gpp.mcptt` · MCPTT icsi-ref, require;explicit), `P-Preferred-Service`, mcptt-info(`session-type` prearranged\|chat · `<mcptt-request-uri>` = 그룹 · `<mcptt-client-id>`), SDP 음성 `i=speech` + `m=application <port> udp MCPTT`. 재합류 = 개시 200 OK·멤버 INVITE Contact 의 세션 식별자(gr)를 Request-URI 로 — 골든 12·13 |
| 개별 호 | 골든 01·02 — 착신자 = resource-lists entry 하나 |
| 경보 | MESSAGE Request-URI = PSI, Accept-Contact icsi-ref, mcptt-info `<alert-ind>`·`<mcptt-request-uri>` = 그룹 |
| conference 구독 | 진행 중 세션에 참가한 동안만 — Request-URI = 세션 식별자, `Expires: 4294967295` — 골든 10 |
| MCData | 그룹·1:1 SDS·FD MESSAGE = 골든 04·05·07·08, 미디어 평면 SDS INVITE = 골든 09 |
| 착신 | 초대 INVITE 의 `Answer-Mode` = 자기가 보낸 poc-settings 의 answer-mode(manual → Manual) |

## 3. 시나리오 · 지표

- 기동 절차(`startPtt`)에 «서비스 인가» 단계를 등록과 제휴 사이에 둔다 — 결과를 지표로(`service_auth_ms` = PUBLISH 송신 → 200, `service_auth_pct`).
- 동봉 시나리오의 기대 응답 코드 — 인가하지 않은 역할(음성 시험의 비멤버 등)이 MC 요청을 내면 404 141 이 정상이다.
- 실측 도구 대조 = .48 의 S25 실측(인가 전 141 네 갈래 · 설정 없는 착신 480 146 · 인가 뒤 초대 · 인가 PUBLISH 의 설정 그룹 암시적 제휴).
