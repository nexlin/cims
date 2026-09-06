// libcimsue — GMS 그룹 문서(OMA list-service + TS 24.481 mcpttgi) 직렬화/파서 (csc.h GroupDoc).
// 서버(csc/src/services/mcptt.py get_group_xml)가 내는 문서와 같은 요소·네임스페이스를 낸다. CscClient 와 같은 이유로
// pjlib(pj_xml)에 의존하지 않고 문자열 스캔으로 처리한다 — 요소는 접두사 무관하게 로컬 이름으로 찾는다.
#include <cctype>
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
    x += "    <mcpttgi:session-type>" + esc(sessionType.empty() ? "prearranged" : sessionType) + "</mcpttgi:session-type>\n";
    x += std::string("    <mcpttgi:mcdata-allow-short-data-service>") + bs(allowSds) + "</mcpttgi:mcdata-allow-short-data-service>\n";
    x += std::string("    <mcpttgi:mcdata-allow-file-distribution>") + bs(allowFd) + "</mcpttgi:mcdata-allow-file-distribution>\n";
    x += std::string("    <mcpttgi:mcptt-video>") + bs(videoEnabled) + "</mcpttgi:mcptt-video>\n";
    x += "    <mcpttgi:on-network-invite-members>true</mcpttgi:on-network-invite-members>\n";
    if (maxParticipants > 0) x += "    <mcpttgi:on-network-max-participant-count>" + std::to_string(maxParticipants) + "</mcpttgi:on-network-max-participant-count>\n";
    x += std::string("    <mcpttgi:on-network-require-affiliation>") + bs(requireAffiliation) + "</mcpttgi:on-network-require-affiliation>\n";
    x += "    <mcpttgi:on-network-group-priority>" + std::to_string(priority) + "</mcpttgi:on-network-group-priority>\n";
    x += std::string("    <mcpttgi:on-network-encryption>") + bs(encryption) + "</mcpttgi:on-network-encryption>\n";
    x += "    <cp:ruleset>\n      <cp:rule id=\"a7c\">\n         <cp:actions>\n";
    x += std::string("          <mcpttgi:allow-MCPTT-emergency-call>") + bs(emergencyCall) + "</mcpttgi:allow-MCPTT-emergency-call>\n";
    x += std::string("          <mcpttgi:allow-imminent-peril-call>") + bs(emergencyCall) + "</mcpttgi:allow-imminent-peril-call>\n";
    x += std::string("          <mcpttgi:allow-MCPTT-emergency-alert>") + bs(emergencyAlert) + "</mcpttgi:allow-MCPTT-emergency-alert>\n";
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
    v = elemText(xml, "session-type", &f, after); if (f && !v.empty()) d.sessionType = v;
    v = elemText(xml, "mcdata-allow-short-data-service", &f, after); if (f) d.allowSds = isTrue(v);
    v = elemText(xml, "mcdata-allow-file-distribution", &f, after); if (f) d.allowFd = isTrue(v);
    v = elemText(xml, "mcptt-video", &f, after); if (f) d.videoEnabled = isTrue(v);
    v = elemText(xml, "on-network-max-participant-count", &f, after); if (f) d.maxParticipants = std::atoi(v.c_str());
    v = elemText(xml, "on-network-require-affiliation", &f, after); if (f) d.requireAffiliation = isTrue(v);
    v = elemText(xml, "on-network-group-priority", &f, after); if (f) d.priority = std::atoi(v.c_str());
    v = elemText(xml, "on-network-encryption", &f, after); if (f) d.encryption = isTrue(v);
    v = elemText(xml, "allow-MCPTT-emergency-call", &f, after); if (f) d.emergencyCall = isTrue(v);
    v = elemText(xml, "allow-MCPTT-emergency-alert", &f, after); if (f) d.emergencyAlert = isTrue(v);
    d.orgCode = elemText(xml, "org-code", nullptr, after);
    d.authorizedUser = elemText(xml, "authorized-user", nullptr, after);
    d.etag = out.etag;                                     // 호출자가 헤더에서 채운 값 유지
    out = d;
    return true;
}

}  // namespace cimsue
