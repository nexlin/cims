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

/** mcptt-info (TS 24.379 §F.1). emergency/imminent: 0=미기재, 1=true, -1=false(명시 하향). */
std::string mcpttInfo(const std::string& sessionType, const std::string& requestUri,
                      const std::string& callingUserId, const std::string& callingGroupId,
                      int emergency = 0, int imminentPeril = 0);
/** resource-lists (애드혹 멤버). uri 는 tel:/sip: URI. */
std::string resourceLists(const std::vector<std::string>& memberUris);
/** affiliation-command (TS 24.379 §F.3). */
std::string affiliationCommand(const std::string& groupUri, bool affiliate);

/** 수신 SIP 원문(INVITE 등)에서 mcptt-info 요약 추출 — 없으면 present=false. */
McpttInfo parseMcpttInfo(const std::string& wholeMsg);
/** RFC 4575 conference-info 파싱 — users(entity, status), full(state="full"). */
bool parseConferenceInfo(const std::string& xml, std::vector<RosterEntry>& users, bool& full);

/** URI → bare id ("tel:+82..@d" / "sip:x@d" / "<...>" → "+82.."). */
std::string bareId(const std::string& uri);
std::string xmlEscape(const std::string& s);

}  // namespace mcptt
}  // namespace cimsue
