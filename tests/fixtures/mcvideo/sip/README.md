# MCVideo 계약 골든 — K3 SIP 메시지 · K4 SDP (docs/dev/mcvideo_dev_plan.md §3)

`*.txt` 는 **전송 바이트 그대로**(CRLF · 정확한 Content-Length — `.gitattributes` 가 줄 끝 변환을 막는다)다. 정본은 `build_goldens.py` 이고
`--check` 가 최신성을 본다(S1 `S1-MCVIDEO-CONTRACT` 가 부른다). 본문을 바꾸려면 스크립트를 고쳐 다시 낸다 — **계약 변경**이라 .45 와 합의하고
server45_handoff §11 에 적은 뒤에만.

- 생성 쪽 시험 — CSP(A8·A9·A10·A11)는 «이 메시지를 받으면 이렇게 답한다 / 이 모양으로 낸다», 해석 쪽 — SDK(C3·C4)는 «이 메시지를 만든다 / 이 응답을
  읽어 이 값». 헤더 순서·`o=`/`s=`·태그·branch 는 비교하지 않는다(의미 비교).
- 검사 — `python3 tests/mcvideo_fixture_check.py`: 본문 XSD(mcvideo-info Annex F.1 · pidf mcvideoPresInfo §8.3.1.2) + K3·K4 규칙(ICSI 헤더 · Accept-Contact 둘 ·
  Contact 태그 · m-line 순서 audio → video → application · `udp MCVideo` · `i=` · fmtp `;`·`mc_transmission_ssrc` · answer 가 offer 에 없던 파라미터를 더하지
  않음(`mc_audio_ssrc`·`mc_video_ssrc` 는 answer 전용) · Content-Length).

## 시나리오

K2 시나리오(../README.md)와 같다 — PTT 도메인 `ptt.cims.example.kr`, CSP `csp.ptt.cims.example.kr`(TLS 5061), CMP 미디어 `10.10.0.20`, 참여 MCVideo 기능 PSI
`sip:mcvideo_psi@ptt.cims.example.kr`(ue-init-config `MCVideo-Service-Details`). UE A `+82510002001`(10.10.1.21, client ID `urn:uuid:2f6b8c4e-…`),
UE B `+82510002002`(10.10.1.22). 그룹 `tel:g101` = MCVideo **chat**(K2), `tel:g103` = MCVideo **prearranged**(K3 에서만 쓰는 그룹 — 같은 세 멤버).

## 메시지

| 파일 | 방향 | 규격 | 요지 |
|---|---|---|---|
| `01_register.txt` | UE → CSP | TS 24.281 §7.1·§7.2.1 | MCPTT·MCVideo 한 REGISTER — Contact `+g.3gpp.mcptt;+g.3gpp.mcvideo;+g.3gpp.icsi-ref="…mcptt,…mcvideo"`, multipart = mcptt-info + mcvideo-info(`mcvideo-access-token`·`mcvideo-client-id`). CIMS SIP 평면은 Digest — 토큰은 싣되 CSP 가 아직 검증하지 않는다(mcx_identity_scope.md §10) |
| `02_publish_affiliation.txt` | UE → CSP | §8.2.1.2·§8.3.1 | PUBLISH PSI · `P-Preferred-Service` MCVideo ICSI · `Event: presence` · `Expires: 4294967295` · mcvideo-info(`mcvideo-request-uri` = 자기 MCVideo ID) + pidf(tuple id = client ID, `mcvideoPI10:affiliation group` g101·g103, `p-id`) |
| `03_chat_join_invite.txt` | UE → CSP | §9.2.2.2.1.1 · TS 24.581 §14.2 | chat 합류(첫 합류 = 세션 개시) — R-URI PSI · Accept-Contact 둘 · mcvideo-info `session-type` chat · SDP offer(fmtp `mc_queueing;mc_priority=5;mc_transmission_ssrc=…`) |
| `04_chat_join_200.txt` | CSP → UE | §9.2.2.4.1.1 15)~20) · §6.3.3.2.1 | Contact = 세션 식별자(`sip:g101@csp…;gr=…`) + MCVideo 태그 + isfocus · Require timer · Supported tdialog · PAI = PSI · answer = CMP 멤버 포트, fmtp `mc_priority=5;mc_transmission_ssrc=<CMP tc_ssrc>`(`mc_queueing` 없음 — 1차 송출 큐 없음) |
| `05_prearranged_initiate_invite.txt` | UE → CSP | §9.2.1.2.1.1 · TS 24.581 §14.2.4·§14.2.5 | prearranged 새 세션 + 암묵적 송출 요청(`mc_granted;mc_implicit_request`) |
| `06_prearranged_initiate_200.txt` | CSP → UE | TS 24.581 §14.3.4·§14.3.5·§14.3.7·§14.3.8 | 암묵 요청 수락 + 허가 — fmtp `mc_granted;mc_implicit_request;mc_audio_ssrc=…;mc_video_ssrc=…`(CMP 가 송출에 할당한 SSRC 쌍 — cmp_media_api.md §7.9) |
| `07_prearranged_member_invite.txt` | CSP → UE B | §6.3.3.1.1·§6.3.3.1.2·§9.2.1.4.1.1 | 멤버 초대 — Contact 세션 식별자 + isfocus · Accept-Contact 둘 · `P-Asserted-Service` MCVideo ICSI · mcvideo-info request-uri(B)·calling-user-id(A)·calling-group-id(g103) · offer fmtp `mc_priority=<user-priority>;mc_transmission_ssrc=…`(`mc_granted`·`mc_implicit_request` 없음) |
| `08_prearranged_rejoin_invite.txt` | UE → CSP | §9.2.1.2.4.1 | 재합류 — R-URI = 세션 식별자(06 의 Contact), 암묵 요청 없음(진행 중 세션 — TS 24.581 §14.3.5) |
| `09_reject_404_117.txt` | CSP → UE | §6.3.5.2 5)c) · §4.4 | chat INVITE 가 prearranged 그룹(g103)에 — 404 + `Warning: 399 ptt.cims.example.kr "117 the group identity indicated in the request is a prearranged group"` |
| `10_reject_404_118.txt` | CSP → UE | §6.3.5.2 5)d) · §4.4 | prearranged INVITE 가 chat 그룹(g101)에 — 404 + `"118 the group identity indicated in the request is a chat group"` |
