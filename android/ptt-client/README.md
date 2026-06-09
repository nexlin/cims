# ptt-client — MCPTT 그룹 PTT 앱

3GPP MCPTT 그룹 PTT 단말. VoLTE 코어(PJSIP + MediaCodec) 위에 **affiliation · floor control · 그룹 · CSC 설정**을 얹는다.

## 범위

- CSC 인증/조회: IdMS **OAuth2 PKCE** → GMS/CMS **XCAP**(HTTPS 4430)
- affiliation: SIP **PUBLISH** (`application/vnd.3gpp.mcptt-affiliation-command+xml`)
- 그룹콜: 키업 **INVITE**(멀티파트: `mcptt-info+xml` + `resource-lists+xml` + SDP) — on-demand(prearranged/broadcast) / chat
- **Floor**: `"MCPT"` RTCP-APP(별도 UDP 소켓) — REQUEST/GRANT/REJECT/RELEASE/IDLE/TAKEN/REVOKE
- emergency / imminent-peril, conference 멤버 상태(NOTIFY)

## 내부 구성 (계획)

공유 **`core`**(SIP/미디어/PJSIP/코덱) 의존 + `app` · `floor`(MCPT RTCP-APP) · `csc`(PKCE + XCAP) · `group`/`affiliation`.

## 상태

스캐폴드 대기 (M2 ~ M4). 설계: [../../docs/design/features/android_ue_client.md](../../docs/design/features/android_ue_client.md) (마일스톤 M2~M4)
