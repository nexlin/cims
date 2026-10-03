// libcimsue 내부 — MCPTT XML 본문 빌더/파서 (TS 24.379 call/affiliation, RFC 4575 conference-info).
// 원천: android/ptt-client mcptt/McpttXml.kt. 네임스페이스는 서버(csp/GroupCallService.cpp, csc)와 정합.
#pragma once

#include <string>
#include <vector>

#include "cimsue/types.h"

namespace cimsue {
namespace mcptt {

constexpr const char* kNsMcpttInfo = "urn:3gpp:ns:mcpttInfo:1.0";
constexpr const char* kNsGroupInfo = "urn:3gpp:ns:mcpttGroupInfo:1.0";
constexpr const char* kNsResourceLists = "urn:ietf:params:xml:ns:resource-lists";
constexpr const char* kNsAffiliation = "urn:3gpp:ns:mcpttAffiliation:1.0";
constexpr const char* kCtMcpttInfo = "application/vnd.3gpp.mcptt-info+xml";
constexpr const char* kCtResourceLists = "application/resource-lists+xml";
constexpr const char* kCtAffiliation = "application/vnd.3gpp.mcptt-affiliation-command+xml";
constexpr const char* kCtConferenceInfo = "application/conference-info+xml";
constexpr const char* kIcsiMcptt = "urn:urn-7:3gpp-service.ims.icsi.mcptt";
constexpr const char* kNsPresInfo = "urn:3gpp:ns:mcpttPresInfo:1.0";          // TS 24.379 §9.3.1.1
constexpr const char* kCtPidf = "application/pidf+xml";
constexpr const char* kAffiliationExpires = "4294967295";                      // 2^32-1 — §9.2.1.2 5)a) NOTE 3

/** mcptt-info (TS 24.379 §F.1). emergency/imminent/alert: 0=미기재, 1=true, -1=false(명시 하향).
 *  broadcast = 일제 통화 개시 `<broadcast-ind>true`(§6.2.8.2) — session-type 은 prearranged 그대로.
 *  지시자 조합(§6.3.3.1.17 — emergency-ind true 면 imminentperil-ind 없이 alert-ind 동반, imminentperil-ind 면 둘 다 없음)은
 *  호출자가 맞춘다 — 이 함수는 받은 대로 싣는다. */
std::string mcpttInfo(const std::string& sessionType, const std::string& requestUri,
                      const std::string& callingUserId, const std::string& callingGroupId,
                      int emergency = 0, int imminentPeril = 0, bool broadcast = false, int alert = 0);
/** 단말이 여는 호의 mcptt-info — 그룹 호(편성·chat·애드혹, TS 24.379 §10.1.1.2.1.1 14) · §10.1.2.2.1.1 13) · §17.2.2.1.1 10)) =
 *  session-type · `<mcptt-request-uri>` = 그룹 ID · `<mcptt-client-id>` · 지시자, 개별 호(§11.1.1.2.1.1 14)c)) = session-type private · 지시자
 *  (requestUri·clientId 를 비우면 그 요소를 싣지 않는다). 발신자 MCPTT ID 는 싣지 않는다(NOTE 2 — 참여 기능이 정한다). */
std::string mcpttInfoOriginating(const std::string& sessionType, const std::string& requestUri, const std::string& clientId,
                                 int emergency = 0, int imminentPeril = 0, bool broadcast = false, int alert = 0);
/** MCPTT 호 다이얼로그 Contact 의 서비스 특성 태그 — `;+g.3gpp.mcptt;+g.3gpp.icsi-ref="…mcptt"`(§10.1.1.2.1.1 4) · §6.2.3.1.1 3)·4)). */
std::string contactFeatureParams();
/** resource-lists (애드혹 멤버). uri 는 tel:/sip: URI. */
std::string resourceLists(const std::vector<std::string>& memberUris);
/** 규격형 문서 변경 구독의 문서 목록(RFC 5875 · TS 24.481 §6.3.13.2.1 a) · TS 24.484 §6.3.13.2.2 b)1)) — `<entry uri>` 마다 문서 하나
 *  (XCAP root 기준 상대 경로). */
std::string xcapDiffResourceLists(const std::vector<std::string>& documents);
/** 같은 구독의 mcptt-info — `<mcptt-access-token>`(§6.3.13.2.1 c) · TS 24.484 §6.3.13.2.2 c) — 인증에서 받은 액세스 토큰). */
std::string accessTokenInfo(const std::string& accessToken);
/** affiliation-command (TS 24.379 §F.3). */
std::string affiliationCommand(const std::string& groupUri, bool affiliate);
/** 제휴 게시의 mcptt-info — `<mcptt-request-uri>` = 대상 MCPTT ID 만(TS 24.379 §9.2.1.2 2)). */
std::string affiliationInfo(const std::string& targetMcpttId);
/** 제휴 게시의 pidf(TS 24.379 §9.3.1 per-user affiliation information) — entity = 대상 MCPTT ID, tuple id = MCPTT client ID,
 *  관심 그룹 전부(§9.2.1.2 5)b)i)·ii)), `<affiliation>` 에 status·expires 없음(iii)), 유일 p-id(iv)). */
std::string affiliationPidf(const std::string& entity, const std::string& clientId, const std::vector<std::string>& groupUris,
                            const std::string& pid);

/** 긴급 경보 MESSAGE 본문(TS 24.379 §12.1.1.1·§12.1.1.2, 요소 순서 = §F.1 mcptt-ParamsType). callingUserId 는 규격상 서버가
 *  채우는 값이지만 이 CSP 는 원본 본문을 그대로 팬아웃하므로 수신자가 발신자를 알 수 있게 싣는다. clientId 가 비면 요소를 뺀다.
 *  emergency = 1/-1 이면 `emergency-ind` 를 싣는다(취소와 함께 그룹 긴급 해제 = -1, §12.1.1.2 5)). */
std::string alertInfo(const std::string& groupUri, const std::string& callingUserId, const std::string& clientId,
                      bool activate, const std::string& originatedBy = std::string(), int emergency = 0);
/** mcptt-info 불리언 지시자 — 1 true / -1 false / 0 요소 없음(접두사 무관). */
int indicator(const std::string& xml, const std::string& local);
/** 긴급 경보·통지 MESSAGE 본문 해석(§12.1.1.3) — alert-ind·emergency-ind·imminentperil-ind 가 하나도 없으면 false.
 *  accountId·self 와 userId 폴백(From)은 호출자가 채운다. */
bool parseEmergencyAlert(const std::string& body, EmergencyAlert& out);

/** mcptt-info 의 `<non-acknowledged-user>` 값 전부(bare id, 출현 순) — TS 24.379 §6.3.3.3, 요소는 `<anyExt>` 안(§F.1). */
std::vector<std::string> nonAcknowledgedUsers(const std::string& xml);
/** 수신 SIP 원문(INVITE 등)에서 mcptt-info 요약 추출 — 없으면 present=false. */
McpttInfo parseMcpttInfo(const std::string& wholeMsg);
/** RFC 4575 conference-info 파싱 — users(entity, status), full(state="full"). */
bool parseConferenceInfo(const std::string& xml, std::vector<RosterEntry>& users, bool& full);

/** RFC 4235 dialog-info 파싱 — dialog 마다 DialogInfo(accountId 는 호출자가 채움). 없으면 false. */
bool parseDialogInfo(const std::string& xml, std::vector<DialogInfo>& out);
/** SDP 의 a=ssrc:<ssrc> label:<name> (RFC 5576) → MediaSource 목록(active=true). */
std::vector<MediaSource> sdpSsrcLabels(const std::string& sdp);

/** floor 평면 SDP m=application 섹션(ptt_ue.md) — `a=fmtp:MCPTT`(TS 24.380 §14.2).
 *  implicitRequest = 이 offer 를 싣는 개시 INVITE 가 암묵적 발언 요청이다 — `mc_implicit_request`(§14.2.5) 와 200 OK 승인 표시
 *  수용 `mc_granted`(§14.2.4)를 함께 싣는다. `mc_granted` 는 암묵 요청과만 싣는다: 200 OK 승인은 암묵 요청에만 있는 절차라 요청 없는
 *  offer 에선 뜻이 없고, 이어지는 offer(re-INVITE)에는 둘 다 싣지 않는다(§14.5·§14.2.5). */
std::string floorSdp(int localPort, bool fullDuplex, bool implicitRequest = false);

/** 상대 SDP 의 m=application `a=fmtp:MCPTT` 협상 결과(TS 24.380 §12.1.2.2·§14.3). present=false = fmtp:MCPTT 없음.
 *  answer 의 implicitRequest = 서버가 암묵 요청을 받아들였다(§14.3.5 — 승인은 아님, §12.1.2.2 NOTE 4),
 *  granted = 200 OK 로 발언권을 승인했다(§14.3.4). */
struct FloorFmtp {
    bool present = false;
    bool queueing = false, implicitRequest = false, granted = false, noFloorCtrl = false;
};
FloorFmtp parseFloorFmtp(const std::string& sdp);

/** SDP 가 MCPTT 호 SDP 인가 — floor 제어 채널 `m=application <port> UDP MCPTT`(TS 24.380 §12.1.2)가 있다(proto 대소문자 무시). */
bool isMcpttSdp(const std::string& sdp);
/** 이어지는 offer(re-INVITE·UPDATE — pjsip 세션 갱신 포함)의 `a=fmtp:MCPTT` — `mc_granted` 는 싣지 않고(TS 24.380 §14.5),
 *  `mc_implicit_request` 는 긴급·임박 격상 re-INVITE 에서만 뜻이 있어(§14.5 · TS 24.379 §6.4) 뺀다(격상은 mcptt-info 를 싣는
 *  multipart 라 이 보정을 거치지 않는다). pjsip 세션 갱신 re-INVITE 는 활성 로컬 SDP(개시 offer)를 그대로 보내므로 송신 직전에
 *  적용한다. 남는 파라미터가 없으면 fmtp 줄을 지운다. MCPTT SDP 가 아니면 그대로. */
std::string forSubsequentOffer(const std::string& sdp);
/** MCPTT speech 미디어의 `i=speech`(TS 24.379 §6.2.1 2)d) · §6.2.2 3)e)) — m=audio 바로 뒤에 넣는다(RFC 4566 순서). 이미 있으면 그대로,
 *  MCPTT SDP 가 아니면 그대로. pjmedia SDP 는 미디어 i= 를 담지 못해 송신 직전에 넣는다. */
std::string withSpeechInfo(const std::string& sdp);

/** URI → bare id ("tel:+82..@d" / "sip:x@d" / "<...>" → "+82.."). */
std::string bareId(const std::string& uri);
std::string xmlEscape(const std::string& s);

}  // namespace mcptt
}  // namespace cimsue
