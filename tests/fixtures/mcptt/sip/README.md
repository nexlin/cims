# MCPTT·MCData 요청 형식 계약 — 규격형 SIP 요청 골든 (docs/dev/conformance_gap_plan.md §7)

`*.txt` 는 **전송 바이트 그대로**(CRLF · 정확한 Content-Length — `.gitattributes` 가 줄 끝 변환을 막는다)다. 정본은 `build_goldens.py` 이고
`--check` 가 최신성을 본다(S1 `S1-MCX-REQUEST-CONTRACT`). 본문을 바꾸려면 스크립트를 고쳐 다시 낸다 — **계약 변경**이라 .45(SDK)와 dev_share 로
합의한 뒤에만.

- 해석 쪽 — CSP: `tests/csp_mcptt_request_test.cpp`(S1-UNIT-CSP)가 이 파일을 읽어 «착신자·floor 유무·MCData 대상» 을 본다.
- 생성 쪽 — SDK(U04·U05): «이 요청을 만든다». 헤더 순서·`o=`·태그·branch·boundary 는 비교하지 않는다(의미 비교).
- 서버는 이 형식**만** 받는다(결정 D10 — Request-URI·`<mcptt-request-uri>`·`<mcdata-request-uri>` 를 1:1 대상으로 읽는 옛 형식의 전환기 없음).

## 시나리오

MCVideo K3(../../mcvideo/sip/README.md)와 같다 — PTT 도메인 `ptt.cims.example.kr`, UE A `+82510002001`(10.10.1.21, client ID `urn:uuid:2f6b8c4e-…`),
UE B `+82510002002`, 그룹 `tel:g101`. 참여 기능 PSI = MCPTT `sip:mcptt_psi@ptt.cims.example.kr` · MCData `sip:mcdata_psi@ptt.cims.example.kr`
(ue-init-config `*-Service-Details/Server-URI`, TS 24.484 §7.2.2.1). MCData 이진 파트(signalling·payload)는 CIMS 와이어 형식대로 base64 전송 인코딩이다
([mcdata_messaging.md](../../../../docs/design/features/mcdata_messaging.md) «편차»).

## 메시지

| 파일 | 방향 | 규격 | 요지 |
|---|---|---|---|
| `01_private_invite.txt` | UE → CSP | TS 24.379 §11.1.1.2.1.1 | 개별 호(floor 있음) — Request-URI = MCPTT PSI(1)) · Contact `+g.3gpp.mcptt`·MCPTT icsi-ref(5)) · Accept-Contact 둘(6)·8)) · `P-Preferred-Service`(7)) · `Answer-Mode`(14)b)) · multipart = SDP(음성 `i=speech` + `m=application <port> udp MCPTT`, fmtp `mc_queueing;mc_priority=5` — 12), TS 24.380 표 4.3.3.1-1) + mcptt-info `session-type private`(14)c)) + **resource-lists entry 하나 = 착신자 MCPTT ID**(9)) |
| `02_private_full_duplex_invite.txt` | UE → CSP | §11.1.2.2 | floor 없는 개별 호 — 01 에서 **`m=application` 이 없다**(1)). 서버·착신 단말은 이것으로 floor 없는 호를 안다(§11.1.2.3.1·§11.1.2.2 끝 문단). `mc_no_floor_ctrl` 은 쓰지 않는다(pre-established 용, TS 24.380 §14.2.6) |
| `03_private_reject_403_145.txt` | CSP → UE | §11.1.1.3.1.1 8)·9) · §4.4 | resource-lists 가 없거나 entry 가 둘 이상 — 403 + `Warning: 399 ptt.cims.example.kr "145 unable to determine called party"` |
| `04_sds_group_message.txt` | UE → CSP | TS 24.282 §6.2.4.1 · §9.2.2.2.1 3) | 그룹 SDS — Request-URI = MCData PSI · Accept-Contact `g.3gpp.mcdata.sds`·SDS icsi-ref · `P-Preferred-Service` SDS ICSI · mcdata-info `request-type group-sds` + `<mcdata-request-uri>` = 그룹 + `<mcdata-client-id>` · SDS SIGNALLING(delivery 요청) · DATA PAYLOAD(TEXT) |
| `05_sds_one_to_one_message.txt` | UE → CSP | §9.2.2.2.1 2) | 1:1 SDS — mcdata-info `request-type one-to-one-sds` + **resource-lists entry 하나 = 대상 MCData ID** |
| `06_sds_reject_403_204.txt` | CSP → UE | §9.2.2.4.2 5)b)i) · §4.9 | 1:1 SDS 에 resource-lists 가 없다(대상을 Request-URI·`<mcdata-request-uri>` 에 실은 옛 형식) — 403 + `"204 unable to determine targeted user for one-to-one SDS"`. FD 는 `205 … one-to-one FD` |
| `07_fd_group_message.txt` | UE → CSP | §6.2.4.1 2) · §10.2.4.2.1 | 그룹 FD(신호 평면) — FD 특성 태그·ICSI · `request-type group-fd` · FD SIGNALLING(FILEURL + Metadata file-selector) |
| `08_fd_one_to_one_message.txt` | UE → CSP | §10.2.4.4.2 10) | 1:1 FD — `request-type one-to-one-fd` + resource-lists entry 하나 |
| `09_sds_media_group_invite.txt` | UE → CSP | §9.2.3.2.1 · §9.2.3.2.3 | 미디어 평면 그룹 SDS — Request-URI = MCData PSI · Contact `g.3gpp.mcdata.sds`·icsi-ref · Accept-Contact 둘 · `P-Preferred-Service` · timer · mcdata-info `group-sds`·그룹·client ID · SDP = `m=message <port> TCP/MSRP *` + sendonly·path·accept-types·`setup:actpass` |
| `10_conference_subscribe.txt` | UE → CSP | TS 24.379 §10.1.3.2 | conference 구독 — Request-URI = 진행 중 세션 식별자(개시 200 OK·멤버 INVITE Contact 의 그룹 AoR + `gr`) · `P-Preferred-Service` · Accept-Contact icsi-ref · `Expires: 4294967295` · `Accept: application/conference-info+xml` · mcptt-info `<mcptt-request-uri>` = 그룹. 구독자는 그 세션의 참가자. 200 OK·NOTIFY Contact = 같은 세션 식별자, 세션이 끝나면 `terminated;reason=noresource`(RFC 4575 §3.3) |
| `11_conference_reject_404_137.txt` | CSP → UE | §10.1.3.3 2) · §4.4 | 진행 중 세션으로 풀리지 않는 구독(그룹 URI 만·끝난 세션의 gr·다른 그룹의 `<mcptt-request-uri>`) — 404 + `"137 the indicated group call does not exist"`. 참가자가 아니면 403 + `138` |
| `12_rejoin_invite.txt` | UE → CSP | TS 24.379 §10.1.1.2.4.1 · §10.1.1.2.1.1 | 재합류 — 편성 그룹 호 개시 INVITE 와 같고 **Request-URI = 진행 중 세션 식별자**(개시 200 OK·멤버 INVITE Contact 의 URI 그대로 — 그룹 AoR + `gr`). Contact 특성 태그 · Accept-Contact 둘 · `P-Preferred-Service` · timer · mcptt-info `session-type prearranged` + `<mcptt-request-uri>` = 그룹 + `<mcptt-client-id>` · SDP offer(발언권 제어 채널 포함) |
| `13_rejoin_reject_404.txt` | CSP → UE | §10.1.1.4.5.1 2) | 세션 식별자가 가리키는 그룹 호가 없다(끝난 세션의 gr · `<mcptt-request-uri>` 가 다른 그룹) — 404, **Warning 없음**. 새 세션을 열지 않는다(단말은 그룹 호 개시로 다시 건다) |
| `14_poc_settings_publish_auth.txt` | UE → CSP | TS 24.379 §7.2.2 · §7.2.1A | 서비스 인가 + 서비스 설정 — Request-URI = MCPTT PSI · `P-Preferred-Service` · `Event: poc-settings` · `Expires: 4294967295` · multipart = mcptt-info `<mcptt-access-token>`·`<mcptt-client-id>` + poc-settings(entity id = Instance ID URN · `<am-settings><answer-mode>` · `mcs10Set:selected-user-profile-index` · `mcs10Set:multiplex-support`) |
| `15_poc_settings_publish_settings.txt` | UE → CSP | §7.2.3 | 서비스 설정만 — mcptt-info `<mcptt-request-uri>` = 자기 MCPTT ID · `<mcptt-client-id>` + poc-settings, `SIP-If-Match`(RFC 3903 갱신) |
| `16_poc_settings_publish_remove.txt` | UE → CSP | §7.2.1A 4) NOTE 3 · §7.3.5 | 설정 제거 = MCPTT 로그오프 — `Expires: 0` + `SIP-If-Match`, 본문 없음. 서버는 설정·제휴·바인딩을 지운다 |
| `17_poc_settings_subscribe.txt` | UE → CSP | §7.2.4 · §7.3.6 | 서비스 설정 구독 — Request-URI = PSI · mcptt-info `<mcptt-request-uri>` = 자기 MCPTT ID · `Accept: application/poc-settings+xml` · `Expires: 4294967295`(0 = fetch) |
| `18_poc_settings_reject_403_101.txt` | CSP → UE | §7.3.3 6) | 서비스 인가 실패 — 403 + `"101 service authorisation failed"` |
| `19_poc_settings_reject_404_141.txt` | CSP → UE | §7.3.4 6) | 바인딩 없는 설정만 PUBLISH — 404 + `"141 user unknown to the participating function"` |

서버가 정하는 값(CSP — [csp.md](../../../../docs/design/modules/csp.md) «Private call» · [mcdata_messaging.md](../../../../docs/design/features/mcdata_messaging.md) §4):

- 개별 호 착신자 = resource-lists entry 하나(없거나 둘이면 403 145). Request-URI·`<mcptt-request-uri>` 는 착신자로 읽지 않는다.
- conference 구독 = 진행 중 세션 식별자(gr)로, 참가자만 — 아니면 404 137 · 403 138. Contact = 세션 식별자, 세션 끝 = noresource 종료.
- 세션 식별자 = 모든 leg(개시·합류 200 OK, 멤버 INVITE, in-dialog)의 Contact 에서 같은 URI `sip:<그룹>@<CSP>;gr=<토큰>` — PSI 로 개시해도 사용자부는 그룹.
- 서비스 인가 = 접근 토큰(PUBLISH poc-settings §7.3.3 또는 REGISTER 본문 §7.3.2)을 IdMS 에 introspection(RFC 7662)으로 검증 — 활성 ·
  scope `3gpp:mc:ptt_service` · 토큰의 MCPTT ID = 요청 IMPU 사용자부(단일 MC service ID) — 이면 (MCPTT ID, client ID, IMPU) 바인딩. 아니면 403 101.
  설정만 PUBLISH 는 바인딩이 있어야 한다(404 141). Expires 0 = 설정·제휴·바인딩 제거. 단말의 Answer-Mode 는 그 단말에게 가는 초대의 `Answer-Mode` 다.
- 재합류 = Request-URI 의 gr 로 진행 중 세션을 찾고 그 세션의 그룹으로 처리한다(사용자부로 가르지 않는다). 없거나 `<mcptt-request-uri>` 가 다른 그룹이면 404(Warning 없음).
- 개별 호 floor = offer 에 `m=application <port≠0> udp MCPTT` 가 있으면 있음(반이중), 없으면 없음(전이중 — CMP `floor_control:"off"`, 착신 offer 에도 `m=application` 없음).
- MCData 대상 = `request-type` — `group-sds`·`group-fd` → `<mcdata-request-uri>`(없으면 404 142, 그룹 문서가 없으면 404 113) · `one-to-one-sds`·`one-to-one-fd` → resource-lists entry 하나(아니면 403 204·205) · 그 밖(request-type 없음·`ad-hoc-group-sds`) → 404 142.
