# MCVideo 계약 골든 — K2 설정 문서 (docs/dev/mcvideo_dev_plan.md §3)

K3 SIP 메시지·K4 SDP 골든은 [sip/](sip/README.md).

생성 쪽(CSC)은 «이 파일과 같게», 해석 쪽(단말 SDK C2)은 «이 파일을 읽어 아래 값» 으로 시험한다. 두 쪽이 **같은 파일**을 읽는다.

- 생성 시험: `python3 -m unittest tests.test_csc_mcvideo` (S1-UNIT-CSC) — 아래 시나리오로 CSC 가 낸 문서 = 이 파일(정규화 비교).
- 스키마 검사: `python3 tests/mcvideo_fixture_check.py` — `xsd/` 로 엄격 검증(xmlschema 없으면 SKIP, `CIMS_PYLIB=<dir>`).
- 재기록: `python3 tests/test_csc_mcvideo.py --write-fixtures` — **계약 변경** 이므로 .45 와 합의하고 server45_handoff §11 에 적은 뒤에만.

## 시나리오

PTT 도메인 `ptt.cims.example.kr`, CSC 공개 base `https://csc.ptt.cims.example.kr:4430`.

| 신원 | 이름 | MCVideo 자격(`mcvideo_user_profile`) |
|---|---|---|
| `tel:+82510002001` | 영상요원1 | streams 1 · N6 1 |
| `tel:+82510002002` | 영상요원2 | streams 1 · N6 1 |
| `tel:+82510002003` | 관제1 (chair, priority 1) | streams 4 · N6 2 |

그룹 `tel:g101` «현장영상 1팀» = MCPTT(prearranged) + MCVideo(chat) · 그룹 `tel:g102` «음성 전용» = MCPTT 만. 멤버는 두 그룹 모두 위 셋.

## 파일과 해석 값

| 파일 | 문서 | 해석 쪽이 읽어야 할 값 |
|---|---|---|
| `group_g101.xml` | GMS 그룹 문서(TS 24.481) | `<service>` 셋: enabler MCPTT ICSI(`<mcptt-speech/>`) · **MCVideo ICSI**(`<mcvideo-video-media/>`) · MCData SDS. MCVideo = chat(`mcvideo-on-network-invite-members` false) · 최대 시간 1800 s · 보호 false/false · 선호 AMR-WB / H264 · 해상도 `1280x720,640x480` · 프레임률 `30,15` · 동시 송출 2 · 시작 인원 0 · 그룹 우선순위 100 · 수신 hang timer 30 s · conference 구독 허용. 모든 entry 에 `<mcvideo-mcvideo-id uri>` = entry uri. MCPTT 쪽에는 영상 요소가 없다(그룹 영상 = MCVideo `<service>`). MCPTT 몫 = 정원 요소 없음(무제한) · `<preferred-voice-encodings>` AMR-WB · 보호 둘 false · 규칙 `<on-network-allow-getting-member-list>` true. MCData 몫 = 보호 둘 false · `<mcdata-on-network-group-priority>` 5 · `<mcdata-default-charset>` 106(UTF-8) · 규칙 `<mcdata-allow-transmit-data-in-this-group>` true |
| `group_g102_mcptt_only.xml` | 같은 형식, MCVideo 없음 | MCVideo `<service>`·`mcvideo-*` 요소 없음 → MCVideo 그룹 아님 |
| `mcvideo_user_profile.xml` | MCVideo user profile(TS 24.484 §9.3) — `tel:+82510002001` | `XUI-URI` = 본인 · `<MCVideoGroupInfo>` 하나 = `tel:g101`(g102 는 없다) · `MaxSimultaneousVideoStreams` 1 · N6 1 · N2 10 · 긴급 대상 entry = `UseCurrentlySelectedGroup` tel:g101 · 모든 `<entry>` 에 `index` · `<ProSeUserID-entry index>` = `<DiscoveryGroupID>` 000000 + `<User-Info-ID>` 영값 · 인가 전부 false(1:1·긴급·임박·경보·원격 회수·ambient viewing·ad hoc — 1차 범위 밖) |
| `mcvideo_service_config.xml` | MCVideo service configuration(TS 24.484 §9.4, 전역) | domain · `<signalling-protection>` false/false · `<protection-between-mcvideo-servers>` false/false · RP `mcpttp` 15/8/0 · `<tc-timers-counters-R14>` 17요소 = [mcvideo_tc_defs.yaml](../../../docs/design/features/mcvideo_tc_defs.yaml) 기본값(T100~T104 = 1 s, 1:1 T1·T5 = 30 s, T2·T3·T4·T6 = 1 s, T11 = 10 s, C2 10·C4 3·C6 3·C7 2·C11 4). C7 요소 이름은 XSD 표기 `C7-reception-accpeted` |
| `ue_init_config.xml` | MCS UE initial configuration(TS 24.484 §7.2) | `<anyExt>` = MCPTT → **MCVideo** → MCData Service-Details. MCVideo Server-URI = `sip:mcvideo_psi@ptt.cims.example.kr` |

## xsd/

3GPP 원문에서 그대로 옮긴 스키마 — `mcpttGroupInfo.xsd`(TS 24.481 V19.3.0 §7.2.4.2) · `ue-init-config.xsd`·`mcvideo-user-profile.xsd`·
`mcvideo-service-config.xsd`(TS 24.484 V20.0.0 §7.2.2.3·§9.3.2.3·§9.4.2.3) · `mcvideoinfo.xsd`·`mcvideoPresInfo.xsd`(TS 24.281 V18.14.0 Annex F.1·§8.3.1.2).
`aux-*` 는 원문이 반입되지 않은 OMA·IETF 틀(list-service·resource-lists·common-policy·xdm extensions)과 1차 범위 밖 이름공간의 보조 스키마다 —
3GPP 확장 요소를 엄격(strict)하게 검증하도록 자식 자리를 `##other strict` 로 둔다.
