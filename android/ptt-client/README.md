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

**M2 비-PJSIP 코어 구현 완료(3GPP TS 규격 정합, JVM 검증).** CMP 의 현행 simplified 포맷이 아니라
TS 규격대로 구현 — interop 위해 서버(CMP/CSP) 의 규격 정렬은 별도 작업.

- `floor/` — **TS 24.380 §8** RTCP-APP `"MCPT"` 코덱(`FloorControl`/`FloorCodec`: subtype=메시지타입,
  TLV 필드, 가변 문자열 4B 정렬) + `FloorClient`(별도 UDP 소켓·상태머신·이벤트). 단위테스트 8/8.
- `csc/` — **TS 33.180** OAuth2 PKCE(S256, `Pkce` — RFC 7636 벡터 검증) + `CscClient`(OkHttp:
  IdMS `/idms/*`, GMS/CMS XCAP, ETag 캐시).
- `mcptt/` — `McpttXml`: mcptt-info / resource-lists / affiliation-command 빌더 + GMS 그룹문서(OMA POC) 파서.

**M2-PJSIP 배선 완료(컴파일 검증, 실기기 미검증):**
- core `SipController` 확장: `sendRequest`(affiliation PUBLISH, targetUri+Expires)·`makeGroupCall`
  (multipart mcptt-info+resource-lists + SDP `m=application` floor 주입)·`onCallSdpCreated`(주입/원격
  floor 포트 파싱→`floorRemote`)·반이중 mic(`setMicEnabled`, GRANT/RELEASE 토글).
- `PttController` — CSC 인증/그룹 → REGISTER → affiliate → 키업 그룹콜 → floor 학습/송수신 → 오디오
  반이중 오케스트레이션. `PttService`(FGS) + PTT UI(누르고 있는 동안 발언).

**실기기 검증 대기**: SDP `m=application` 주입의 PJSIP 동작·multipart INVITE·affiliation PUBLISH
라우팅은 실기기+라이브 서버에서 확인. **서버 TS 규격 정렬 전엔 floor interop 안 됨**
([mcptt_standard_conformance.md](../../docs/design/features/mcptt_standard_conformance.md)).

설계: [../../docs/design/features/android_ue_client.md](../../docs/design/features/android_ue_client.md) (§5 floor=TS 24.380, §7 CSC)
