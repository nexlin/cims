// libcimsue — GMS 그룹 문서(OMA list-service + TS 24.481 mcpttgi) 직렬화/파서 (csc.h GroupDoc).
// 서버(csc/src/services/mcptt.py get_group_xml)가 내는 문서와 같은 요소·네임스페이스를 낸다. CscClient 와 같은 이유로
// pjlib(pj_xml)에 의존하지 않고 문자열 스캔으로 처리한다 — 요소는 접두사 무관하게 로컬 이름으로 찾는다.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>

#include "cimsue/csc.h"

namespace cimsue {

namespace {

std::string esc(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string unesc(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") o += '&'; else if (ent == "lt") o += '<'; else if (ent == "gt") o += '>';
        else if (ent == "quot") o += '"'; else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? (unsigned)std::strtoul(ent.c_str() + 2, nullptr, 16)
                                                                              : (unsigned)std::strtoul(ent.c_str() + 1, nullptr, 10);
            if (cp < 0x80) o += (char)cp;
            else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
            else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        } else { o += s.substr(i, e - i + 1); }
        i = e;
    }
    return o;
}

std::string trim(const std::string& v) {
    size_t b = v.find_first_not_of(" \t\r\n"), t = v.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : v.substr(b, t - b + 1);
}

bool nameEnd(char c) { return c == ' ' || c == '>' || c == '/' || c == '\t' || c == '\r' || c == '\n'; }

/** "<[prefix:]local" 시작 위치 — from 이후 첫 매치. 없으면 npos. */
size_t findOpen(const std::string& s, const std::string& local, size_t from = 0) {
    size_t p = from;
    while ((p = s.find('<', p)) != std::string::npos) {
        size_t q = p + 1;
        if (q < s.size() && (s[q] == '/' || s[q] == '?' || s[q] == '!')) { ++p; continue; }
        size_t n = q;
        while (n < s.size() && !nameEnd(s[n])) ++n;
        std::string tag = s.substr(q, n - q);
        size_t colon = tag.find(':');
        if (colon != std::string::npos) tag = tag.substr(colon + 1);
        if (tag == local) return p;
        p = n;
    }
    return std::string::npos;
}

/** 요소 텍스트(첫 매치). 빈 요소(<x/>)나 없음이면 빈 문자열, found 로 존재 여부. */
std::string elemText(const std::string& s, const std::string& local, bool* found = nullptr, size_t from = 0) {
    if (found) *found = false;
    size_t p = findOpen(s, local, from);
    if (p == std::string::npos) return std::string();
    size_t gt = s.find('>', p);
    if (gt == std::string::npos) return std::string();
    if (found) *found = true;
    if (s[gt - 1] == '/') return std::string();
    size_t e = s.find("</", gt);
    while (e != std::string::npos) {                       // 닫는 태그의 로컬 이름이 같은지 확인
        size_t n = e + 2; while (n < s.size() && !nameEnd(s[n])) ++n;
        std::string tag = s.substr(e + 2, n - e - 2);
        size_t colon = tag.find(':'); if (colon != std::string::npos) tag = tag.substr(colon + 1);
        if (tag == local) break;
        e = s.find("</", n);
    }
    if (e == std::string::npos) return std::string();
    return unesc(trim(s.substr(gt + 1, e - gt - 1)));
}

std::string attrOf(const std::string& tag, const std::string& name) {
    size_t p = tag.find(name + "=\"");
    if (p == std::string::npos) return std::string();
    size_t b = p + name.size() + 2, e = tag.find('"', b);
    return e == std::string::npos ? std::string() : unesc(tag.substr(b, e - b));
}

bool isTrue(const std::string& v) { return v == "true" || v == "1" || v == "TRUE" || v == "True"; }
const char* bs(bool b) { return b ? "true" : "false"; }

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
        x += "        <mcpttgi:on-network-required/>\n";
        x += "        <mcpttgi:participant-type>" + esc(m.role.empty() ? "participant" : m.role) + "</mcpttgi:participant-type>\n";
        x += "        <mcpttgi:user-priority>" + std::to_string(m.priority) + "</mcpttgi:user-priority>\n";
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
    x += std::string("    <mcpttgi:mcptt-video>") + bs(videoEnabled) + "</mcpttgi:mcptt-video>\n";
    // 그룹 종류 = on-network-invite-members(TS 24.481 §7.2.2 a — true=prearranged, false=chat). 그룹 문서에 session-type 요소는 없다.
    x += std::string("    <mcpttgi:on-network-invite-members>") + bs(sessionType != "chat") + "</mcpttgi:on-network-invite-members>\n";
    if (maxParticipants > 0) x += "    <mcpttgi:on-network-max-participant-count>" + std::to_string(maxParticipants) + "</mcpttgi:on-network-max-participant-count>\n";
    x += std::string("    <mcpttgi:on-network-require-affiliation>") + bs(requireAffiliation) + "</mcpttgi:on-network-require-affiliation>\n";
    // 그룹 호 타이머(TS 24.481 §7.2.2 o·§7.2.7) — xs:duration. 미기재면 싣지 않는다(0 은 미사용·무제한이라는 값).
    if (hangTimerSec >= 0) x += "    <mcpttgi:on-network-hang-timer>" + xsDuration(hangTimerSec) + "</mcpttgi:on-network-hang-timer>\n";
    if (maxDurationSec >= 0) x += "    <mcpttgi:on-network-maximum-duration>" + xsDuration(maxDurationSec) +
                                  "</mcpttgi:on-network-maximum-duration>\n";
    x += "    <mcpttgi:on-network-group-priority>" + std::to_string(priority) + "</mcpttgi:on-network-group-priority>\n";
    x += std::string("    <mcpttgi:on-network-encryption>") + bs(encryption) + "</mcpttgi:on-network-encryption>\n";
    x += "    <cp:ruleset>\n      <cp:rule id=\"a7c\">\n         <cp:actions>\n";
    x += std::string("          <mcpttgi:allow-MCPTT-emergency-call>") + bs(emergencyCall) + "</mcpttgi:allow-MCPTT-emergency-call>\n";
    x += std::string("          <mcpttgi:allow-imminent-peril-call>") + bs(emergencyCall) + "</mcpttgi:allow-imminent-peril-call>\n";
    x += std::string("          <mcpttgi:allow-MCPTT-emergency-alert>") + bs(emergencyAlert) + "</mcpttgi:allow-MCPTT-emergency-alert>\n";
    // 참가자 정보 구독 허용(§7.2.4.2) — 서버는 cp:actions 안에서 찾는다.
    if (allowConferenceState >= 0)
        x += std::string("          <mcpttgi:on-network-allow-conference-state>") + bs(allowConferenceState != 0) +
             "</mcpttgi:on-network-allow-conference-state>\n";
    x += "        </cp:actions>\n      </cp:rule>\n    </cp:ruleset>\n";
    x += "    <oxe:supported-services>\n     <oxe:service enabler=\"example.mcptt\">\n      <oxe:group-media>\n       <mcpttgi:mcptt-speech/>\n      </oxe:group-media>\n     </oxe:service>\n";
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
        std::string role = elemText(e, "participant-type");
        if (!role.empty()) m.role = role;
        std::string pr = elemText(e, "user-priority");
        if (!pr.empty()) m.priority = std::atoi(pr.c_str());
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
    v = elemText(xml, "mcptt-video", &f, after); if (f) d.videoEnabled = isTrue(v);
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
    d.orgCode = elemText(xml, "org-code", nullptr, after);
    d.authorizedUser = elemText(xml, "authorized-user", nullptr, after);
    d.etag = out.etag;                                     // 호출자가 헤더에서 채운 값 유지
    out = d;
    return true;
}

}  // namespace cimsue
