// libcimsue — GMS 그룹 문서(OMA list-service + TS 24.481 mcpttgi) 직렬화/파서 (csc.h GroupDoc).
// 서버(csc/src/services/mcptt.py get_group_xml)가 내는 문서와 같은 요소·네임스페이스를 낸다. 스캔 도구 = xml_scan.h.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>

#include "cimsue/csc.h"
#include "csc/xml_scan.h"

namespace cimsue {

namespace {

using namespace xmlscan;

const char* bs(bool b) { return b ? "true" : "false"; }

// 서비스 ICSI(TS 24.481 §7.2.2 <service enabler>) — MCPTT = TS 24.379 Annex E.2.1, MCVideo = TS 24.281 Annex E.2.1
constexpr const char* kIcsiMcptt = "urn:urn-7:3gpp-service.ims.icsi.mcptt";
constexpr const char* kIcsiMcvideo = "urn:urn-7:3gpp-service.ims.icsi.mcvideo";

/** 삼중값 불리언 — 요소 없음 = -1. */
int triElem(const std::string& xml, const char* local, size_t from) {
    bool f = false;
    std::string v = elemText(xml, local, &f, from);
    return f ? (isTrue(v) ? 1 : 0) : -1;
}
int intElemFrom(const std::string& xml, const char* local, size_t from) {
    bool f = false;
    std::string v = elemText(xml, local, &f, from);
    return f && !v.empty() ? std::atoi(v.c_str()) : -1;
}
/** encodingsType(§7.2.2) — <encoding name="…"/> 들. */
std::vector<std::string> encodings(const std::string& xml, const char* local, size_t from) {
    std::vector<std::string> out;
    Elem list = elem(xml, local, from);
    if (!list.found) return out;
    size_t p = 0;
    for (;;) {
        Elem e = elem(list.inner, "encoding", p);
        if (!e.found) break;
        std::string n = attrOf(e.openTag, "name");
        if (!n.empty()) out.push_back(n);
        p = e.end;
    }
    return out;
}
std::string encodingsXml(const char* local, const std::vector<std::string>& names) {
    std::string x = std::string("    <mcpttgi:") + local + ">";
    for (auto& n : names) x += "<mcpttgi:encoding name=\"" + esc(n) + "\"/>";
    return x + "</mcpttgi:" + local + ">\n";
}
void triXml(std::string& x, const char* indent, const char* local, int v) {
    if (v >= 0) x += std::string(indent) + "<mcpttgi:" + local + ">" + bs(v != 0) + "</mcpttgi:" + local + ">\n";
}

/** 초 → xs:duration. 서버(csc `xs_duration`)가 내는 형식과 같다. */
std::string xsDuration(int sec) { return "PT" + std::to_string(sec) + "S"; }

/**
 * xs:duration → 초. `PnDTnHnMnS`(소수 초 절사)와 순수 정수(초)를 받는다 — 서버 `parse_xs_duration` 과 같은 관대함.
 * 형식이 아니면 -1(미기재로 둔다 — 알 수 없는 값을 0 으로 읽으면 «미사용·무제한» 이라는 **다른 뜻**이 된다).
 */
int parseXsDuration(const std::string& raw) {
    std::string t;
    for (char c : raw) if (!std::isspace((unsigned char)c)) t += c;
    if (t.empty()) return -1;
    if (std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c); })) return std::atoi(t.c_str());
    if (t[0] != 'P' || t == "P" || t == "PT") return -1;
    long long total = 0;
    bool inTime = false, any = false;
    size_t i = 1;
    while (i < t.size()) {
        if (t[i] == 'T') { if (inTime) return -1; inTime = true; ++i; continue; }
        size_t j = i;
        while (j < t.size() && std::isdigit((unsigned char)t[j])) ++j;
        if (j == i) return -1;
        long long n = std::atoll(t.substr(i, j - i).c_str());
        if (j < t.size() && t[j] == '.') {                      // 소수 초 — 절사. S 앞에만 온다
            ++j;
            while (j < t.size() && std::isdigit((unsigned char)t[j])) ++j;
            if (j >= t.size() || t[j] != 'S' || !inTime) return -1;
        }
        if (j >= t.size()) return -1;
        char u = t[j];
        if (!inTime && u == 'D') total += n * 86400;
        else if (inTime && u == 'H') total += n * 3600;
        else if (inTime && u == 'M') total += n * 60;
        else if (inTime && u == 'S') total += n;
        else return -1;
        any = true;
        i = j + 1;
    }
    if (!any || total > INT32_MAX) return -1;
    return (int)total;
}

}  // namespace

std::string GroupDoc::toXml() const {
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    x += "<group xmlns=\"urn:oma:xml:poc:list-service\"\n"
         "  xmlns:rl=\"urn:ietf:params:xml:ns:resource-lists\"\n"
         "  xmlns:cp=\"urn:ietf:params:xml:ns:common-policy\"\n"
         "  xmlns:ocp=\"urn:oma:xml:xdm:common-policy\"\n"
         "  xmlns:oxe=\"urn:oma:xml:xdm:extensions\"\n"
         "  xmlns:mcpttgi=\"urn:3gpp:ns:mcpttGroupInfo:1.0\"\n"
         "  xmlns:cims=\"urn:cims:groupinfo:1.0\">\n";
    x += "  <list-service uri=\"" + esc(uri) + "\">\n";
    x += "    <display-name xml:lang=\"en-us\">" + esc(displayName) + "</display-name>\n";
    x += "    <list>\n";
    for (const auto& m : members) {
        x += "      <entry uri=\"" + esc(m.uri) + "\">\n";
        if (!m.name.empty()) x += "        <rl:display-name>" + esc(m.name) + "</rl:display-name>\n";
        if (m.required) x += "        <mcpttgi:on-network-required/>\n";   // 필수 멤버만(TS 24.481 §7.2.4.2)
        x += "        <mcpttgi:participant-type>" + esc(m.role.empty() ? "participant" : m.role) + "</mcpttgi:participant-type>\n";
        x += "        <mcpttgi:user-priority>" + std::to_string(m.priority) + "</mcpttgi:user-priority>\n";
        // MCVideo entry(§7.2.2) — MCVideo ID = MCPTT ID(mcvideo.md §7 D1)
        if (mcvideo.present) x += "        <mcpttgi:mcvideo-mcvideo-id uri=\"" + esc(m.mcvideoId.empty() ? m.uri : m.mcvideoId) + "\"/>\n";
        x += "      </entry>\n";
    }
    x += "    </list>\n";
    x += std::string("    <mcpttgi:mcdata-allow-short-data-service>") + bs(allowSds) + "</mcpttgi:mcdata-allow-short-data-service>\n";
    x += std::string("    <mcpttgi:mcdata-allow-file-distribution>") + bs(allowFd) + "</mcpttgi:mcdata-allow-file-distribution>\n";
    // 크기 한도는 **미기재면 싣지 않는다** — 서버가 기존값을 유지한다. 0 은 «무제한» 이라는 값이다.
    if (maxSdsSize >= 0) x += "    <mcpttgi:mcdata-on-network-max-data-size-for-SDS>" + std::to_string(maxSdsSize) +
                              "</mcpttgi:mcdata-on-network-max-data-size-for-SDS>\n";
    if (maxAutoRecv >= 0) x += "    <mcpttgi:mcdata-on-network-max-data-size-auto-recv>" + std::to_string(maxAutoRecv) +
                               "</mcpttgi:mcdata-on-network-max-data-size-auto-recv>\n";
    // 그룹 종류 = on-network-invite-members(TS 24.481 §7.2.2 a — true=prearranged, false=chat). 그룹 문서에 session-type 요소는 없다.
    x += std::string("    <mcpttgi:on-network-invite-members>") + bs(sessionType != "chat") + "</mcpttgi:on-network-invite-members>\n";
    if (maxParticipants > 0) x += "    <mcpttgi:on-network-max-participant-count>" + std::to_string(maxParticipants) + "</mcpttgi:on-network-max-participant-count>\n";
    x += std::string("    <mcpttgi:on-network-require-affiliation>") + bs(requireAffiliation) + "</mcpttgi:on-network-require-affiliation>\n";
    // 그룹 호 타이머(TS 24.481 §7.2.2 o·§7.2.7) — xs:duration. 미기재면 싣지 않는다(0 은 미사용·무제한이라는 값).
    if (hangTimerSec >= 0) x += "    <mcpttgi:on-network-hang-timer>" + xsDuration(hangTimerSec) + "</mcpttgi:on-network-hang-timer>\n";
    if (maxDurationSec >= 0) x += "    <mcpttgi:on-network-maximum-duration>" + xsDuration(maxDurationSec) +
                                  "</mcpttgi:on-network-maximum-duration>\n";
    // 확인 통화 설정(§7.2.2 s)t)u)) — 미기재면 싣지 않는다
    if (minNumberToStart >= 0) x += "    <mcpttgi:on-network-minimum-number-to-start>" + std::to_string(minNumberToStart) +
                                    "</mcpttgi:on-network-minimum-number-to-start>\n";
    if (ackTimeoutSec >= 0) x += "    <mcpttgi:on-network-timeout-for-acknowledgement-of-required-members>" +
                                 xsDuration(ackTimeoutSec) +
                                 "</mcpttgi:on-network-timeout-for-acknowledgement-of-required-members>\n";
    if (!ackAction.empty())
        x += "    <mcpttgi:on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>" +
             esc(ackAction == "proceed" ? "proceed" : "abandon") +
             "</mcpttgi:on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>\n";
    x += "    <mcpttgi:on-network-group-priority>" + std::to_string(priority) + "</mcpttgi:on-network-group-priority>\n";
    x += std::string("    <mcpttgi:on-network-encryption>") + bs(encryption) + "</mcpttgi:on-network-encryption>\n";
    const McVideoGroupAttrs& v = mcvideo;
    if (v.present) {
        // MCVideo 속성(§7.2.2 목록 순) — 보호 둘은 명시한다(없으면 true 로 읽힌다 — §7.2.8, mcvideo.md §7 D7)
        x += std::string("    <mcpttgi:mcvideo-on-network-invite-members>") + bs(v.inviteMembers) + "</mcpttgi:mcvideo-on-network-invite-members>\n";
        if (v.maxDurationSec >= 0)
            x += "    <mcpttgi:mcvideo-on-network-maximum-duration>" + xsDuration(v.maxDurationSec) + "</mcpttgi:mcvideo-on-network-maximum-duration>\n";
        x += std::string("    <mcpttgi:mcvideo-protect-media>") + bs(v.protectMedia) + "</mcpttgi:mcvideo-protect-media>\n";
        x += std::string("    <mcpttgi:mcvideo-protect-transmission-control>") + bs(v.protectTransmissionControl) +
             "</mcpttgi:mcvideo-protect-transmission-control>\n";
        if (!v.audioEncodings.empty()) x += encodingsXml("mcvideo-preferred-audio-encodings", v.audioEncodings);
        if (!v.videoEncodings.empty()) x += encodingsXml("mcvideo-preferred-video-encodings", v.videoEncodings);
        if (!v.videoResolutions.empty())
            x += "    <mcpttgi:mcvideo-preferred-video-resolutions>" + esc(v.videoResolutions) + "</mcpttgi:mcvideo-preferred-video-resolutions>\n";
        if (!v.videoFrameRate.empty())
            x += "    <mcpttgi:mcvideo-preferred-video-frame-rate>" + esc(v.videoFrameRate) + "</mcpttgi:mcvideo-preferred-video-frame-rate>\n";
        triXml(x, "    ", "mcvideo-urgent-real-time-video-mode", v.urgentRealTimeVideoMode);
        triXml(x, "    ", "mcvideo-non-urgent-real-time-video-mode", v.nonUrgentRealTimeVideoMode);
        triXml(x, "    ", "mcvideo-non-real-time-video-mode", v.nonRealTimeVideoMode);
        if (!v.activeRealTimeVideoMode.empty())
            x += "    <mcpttgi:mcvideo-active-real-time-video-mode>" + esc(v.activeRealTimeVideoMode) + "</mcpttgi:mcvideo-active-real-time-video-mode>\n";
        if (v.maxTransmitters >= 0)
            x += "    <mcpttgi:mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members>" + std::to_string(v.maxTransmitters) +
                 "</mcpttgi:mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members>\n";
        if (v.minNumberToStart >= 0)
            x += "    <mcpttgi:mcvideo-on-network-minimum-number-to-start>" + std::to_string(v.minNumberToStart) +
                 "</mcpttgi:mcvideo-on-network-minimum-number-to-start>\n";
        if (v.groupPriority >= 0)
            x += "    <mcpttgi:mcvideo-on-network-group-priority>" + std::to_string(v.groupPriority) + "</mcpttgi:mcvideo-on-network-group-priority>\n";
        if (v.receptionHangTimerSec >= 0)
            x += "    <mcpttgi:on-network-reception-hang-timer>" + xsDuration(v.receptionHangTimerSec) + "</mcpttgi:on-network-reception-hang-timer>\n";
    }
    x += "    <cp:ruleset>\n      <cp:rule id=\"a7c\">\n         <cp:actions>\n";
    x += std::string("          <mcpttgi:allow-MCPTT-emergency-call>") + bs(emergencyCall) + "</mcpttgi:allow-MCPTT-emergency-call>\n";
    x += std::string("          <mcpttgi:allow-imminent-peril-call>") + bs(emergencyCall) + "</mcpttgi:allow-imminent-peril-call>\n";
    x += std::string("          <mcpttgi:allow-MCPTT-emergency-alert>") + bs(emergencyAlert) + "</mcpttgi:allow-MCPTT-emergency-alert>\n";
    // 참가자 정보 구독 허용(§7.2.4.2) — 서버는 cp:actions 안에서 찾는다.
    if (allowConferenceState >= 0)
        x += std::string("          <mcpttgi:on-network-allow-conference-state>") + bs(allowConferenceState != 0) +
             "</mcpttgi:on-network-allow-conference-state>\n";
    if (v.present) {                                       // MCVideo 규칙 action(§7.2.4.2)
        triXml(x, "          ", "mcvideo-allow-emergency-call", v.allowEmergencyCall);
        triXml(x, "          ", "mcvideo-allow-emergency-alert", v.allowEmergencyAlert);
        triXml(x, "          ", "mcvideo-allow-imminent-peril-call", v.allowImminentPerilCall);
        triXml(x, "          ", "mcvideo-on-network-allow-conference-state", v.allowConferenceState);
    }
    x += "        </cp:actions>\n      </cp:rule>\n    </cp:ruleset>\n";
    // 서비스마다 <service> 하나 — enabler = 그 서비스의 ICSI(TS 24.481 §7.2.2 — MCPTT 는 TS 24.379 Annex E.2.1, mcvideo.md §6 V0)
    x += std::string("    <oxe:supported-services>\n     <oxe:service enabler=\"") + kIcsiMcptt +
         "\">\n      <oxe:group-media>\n       <mcpttgi:mcptt-speech/>\n      </oxe:group-media>\n     </oxe:service>\n";
    if (v.present)
        x += std::string("     <oxe:service enabler=\"") + kIcsiMcvideo +
             "\">\n      <oxe:group-media>\n       <mcpttgi:mcvideo-video-media/>\n      </oxe:group-media>\n     </oxe:service>\n";
    if (allowSds) x += "     <oxe:service enabler=\"urn:urn-7:3gpp-service.ims.icsi.mcdata.sds\"/>\n";
    if (allowFd) x += "     <oxe:service enabler=\"urn:urn-7:3gpp-service.ims.icsi.mcdata.fd\"/>\n";
    x += "    </oxe:supported-services>\n";
    if (!orgCode.empty()) x += "    <mcpttgi:org-code>" + esc(orgCode) + "</mcpttgi:org-code>\n";
    if (!authorizedUser.empty()) x += "    <mcpttgi:authorized-user>" + esc(authorizedUser) + "</mcpttgi:authorized-user>\n";
    x += "  </list-service>\n</group>\n";
    return x;
}

bool GroupDoc::parse(const std::string& xml, GroupDoc& out, std::string* err) {
    size_t ls = findOpen(xml, "list-service");
    if (ls == std::string::npos) { if (err) *err = "no list-service"; return false; }
    size_t gt = xml.find('>', ls);
    if (gt == std::string::npos) { if (err) *err = "bad list-service"; return false; }
    GroupDoc d;
    d.uri = attrOf(xml.substr(ls, gt - ls), "uri");
    d.displayName = elemText(xml, "display-name", nullptr, gt);

    // 멤버 — <list> 안의 <entry uri="…">…</entry>
    size_t lp = findOpen(xml, "list", gt);
    size_t lend = lp == std::string::npos ? std::string::npos : xml.find("</list>", lp);
    if (lp != std::string::npos && lend == std::string::npos) lend = xml.size();
    std::string list = lp == std::string::npos ? std::string() : xml.substr(lp, lend - lp);
    size_t p = 0;
    while ((p = findOpen(list, "entry", p)) != std::string::npos) {
        size_t egt = list.find('>', p);
        if (egt == std::string::npos) break;
        size_t eend = list[egt - 1] == '/' ? egt + 1 : list.find("</entry>", egt);
        if (eend == std::string::npos) break;
        std::string e = list.substr(p, eend - p);
        GroupMember m;
        m.uri = attrOf(list.substr(p, egt - p), "uri");
        m.name = elemText(e, "display-name");
        m.title = elemText(e, "user-title");
        std::string role = elemText(e, "participant-type");
        if (!role.empty()) m.role = role;
        std::string pr = elemText(e, "user-priority");
        if (!pr.empty()) m.priority = std::atoi(pr.c_str());
        m.required = findOpen(e, "on-network-required") != std::string::npos;
        Elem mv = elem(e, "mcvideo-mcvideo-id");
        if (mv.found) m.mcvideoId = attrOf(mv.openTag, "uri");
        if (!m.uri.empty()) d.members.push_back(m);
        p = eend;
    }

    // list-service 직속 속성 — <list> 다음부터 찾는다(멤버 요소와 이름이 겹치지 않지만 순서상 안전).
    size_t after = lend == std::string::npos ? gt : lend;
    bool f;
    std::string v;
    // 그룹 종류 — on-network-invite-members(TS 24.481 §7.2.2 a). 없으면 옛 서버의 비규격 <mcpttgi:session-type> 폴백.
    v = elemText(xml, "on-network-invite-members", &f, after);
    if (f) d.sessionType = isTrue(v) ? "prearranged" : "chat";
    else { v = elemText(xml, "session-type", &f, after); if (f && !v.empty()) d.sessionType = v == "chat" ? "chat" : "prearranged"; }
    v = elemText(xml, "mcdata-allow-short-data-service", &f, after); if (f) d.allowSds = isTrue(v);
    v = elemText(xml, "mcdata-allow-file-distribution", &f, after); if (f) d.allowFd = isTrue(v);
    v = elemText(xml, "on-network-max-participant-count", &f, after); if (f) d.maxParticipants = std::atoi(v.c_str());
    v = elemText(xml, "on-network-require-affiliation", &f, after); if (f) d.requireAffiliation = isTrue(v);
    v = elemText(xml, "on-network-group-priority", &f, after); if (f) d.priority = std::atoi(v.c_str());
    v = elemText(xml, "on-network-encryption", &f, after); if (f) d.encryption = isTrue(v);
    v = elemText(xml, "allow-MCPTT-emergency-call", &f, after); if (f) d.emergencyCall = isTrue(v);
    v = elemText(xml, "allow-MCPTT-emergency-alert", &f, after); if (f) d.emergencyAlert = isTrue(v);
    // 없는 요소는 미기재(kUnset)로 남긴다 — «문서에 없었다» 와 «0» 은 다른 뜻이다.
    //   서버 GET 은 크기 한도가 0(무제한)이면 요소를 아예 싣지 않는다 → 여기서는 미기재가 되고, 되돌려 PUT 해도
    //   싣지 않으므로 서버의 0 이 그대로 남는다.
    v = elemText(xml, "on-network-hang-timer", &f, after); if (f) d.hangTimerSec = parseXsDuration(v);
    v = elemText(xml, "on-network-maximum-duration", &f, after); if (f) d.maxDurationSec = parseXsDuration(v);
    v = elemText(xml, "on-network-allow-conference-state", &f, after); if (f) d.allowConferenceState = isTrue(v) ? 1 : 0;
    v = elemText(xml, "mcdata-on-network-max-data-size-for-SDS", &f, after); if (f) d.maxSdsSize = std::atoi(v.c_str());
    v = elemText(xml, "mcdata-on-network-max-data-size-auto-recv", &f, after); if (f) d.maxAutoRecv = std::atoi(v.c_str());
    v = elemText(xml, "on-network-minimum-number-to-start", &f, after); if (f) d.minNumberToStart = std::atoi(v.c_str());
    v = elemText(xml, "on-network-timeout-for-acknowledgement-of-required-members", &f, after);
    if (f) d.ackTimeoutSec = parseXsDuration(v);
    v = elemText(xml, "on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members", &f, after);
    if (f) d.ackAction = v == "proceed" ? "proceed" : "abandon";   // 정의 밖 값 = abandon (§7.2.2 u))
    // MCVideo 몫 — MCVideo ICSI <service> 가 있으면 MCVideo 그룹(§7.2.2, mcvideo.md §1.3). 요소 로컬 이름이 MCPTT 것과 겹치지 않는다
    //   (mcvideo- 접두 — findOpen 은 로컬 이름 전체로 맞춘다).
    Elem ss = elem(xml, "supported-services", after);
    for (size_t sp = 0; ss.found;) {
        Elem svc = elem(ss.inner, "service", sp);
        if (!svc.found) break;
        if (attrOf(svc.openTag, "enabler") == kIcsiMcvideo) d.mcvideo.present = true;
        sp = svc.end;
    }
    McVideoGroupAttrs& mv = d.mcvideo;
    v = elemText(xml, "mcvideo-on-network-invite-members", &f, after); if (f) mv.inviteMembers = isTrue(v);
    v = elemText(xml, "mcvideo-on-network-maximum-duration", &f, after); if (f) mv.maxDurationSec = parseXsDuration(v);
    v = elemText(xml, "mcvideo-protect-media", &f, after); if (f) mv.protectMedia = isTrue(v);
    v = elemText(xml, "mcvideo-protect-transmission-control", &f, after); if (f) mv.protectTransmissionControl = isTrue(v);
    mv.audioEncodings = encodings(xml, "mcvideo-preferred-audio-encodings", after);
    mv.videoEncodings = encodings(xml, "mcvideo-preferred-video-encodings", after);
    mv.videoResolutions = elemText(xml, "mcvideo-preferred-video-resolutions", nullptr, after);
    mv.videoFrameRate = elemText(xml, "mcvideo-preferred-video-frame-rate", nullptr, after);
    mv.urgentRealTimeVideoMode = triElem(xml, "mcvideo-urgent-real-time-video-mode", after);
    mv.nonUrgentRealTimeVideoMode = triElem(xml, "mcvideo-non-urgent-real-time-video-mode", after);
    mv.nonRealTimeVideoMode = triElem(xml, "mcvideo-non-real-time-video-mode", after);
    mv.activeRealTimeVideoMode = elemText(xml, "mcvideo-active-real-time-video-mode", nullptr, after);
    mv.maxTransmitters = intElemFrom(xml, "mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members", after);
    mv.minNumberToStart = intElemFrom(xml, "mcvideo-on-network-minimum-number-to-start", after);
    mv.groupPriority = intElemFrom(xml, "mcvideo-on-network-group-priority", after);
    v = elemText(xml, "on-network-reception-hang-timer", &f, after); if (f) mv.receptionHangTimerSec = parseXsDuration(v);
    mv.allowConferenceState = triElem(xml, "mcvideo-on-network-allow-conference-state", after);
    mv.allowEmergencyCall = triElem(xml, "mcvideo-allow-emergency-call", after);
    mv.allowEmergencyAlert = triElem(xml, "mcvideo-allow-emergency-alert", after);
    mv.allowImminentPerilCall = triElem(xml, "mcvideo-allow-imminent-peril-call", after);
    d.orgCode = elemText(xml, "org-code", nullptr, after);
    d.authorizedUser = elemText(xml, "authorized-user", nullptr, after);
    d.etag = out.etag;                                     // 호출자가 헤더에서 채운 값 유지
    out = d;
    return true;
}

}  // namespace cimsue
