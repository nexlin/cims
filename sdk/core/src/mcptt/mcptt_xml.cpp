#include "mcptt_xml.h"

#include <cctype>
#include <cstdlib>

namespace cimsue {
namespace mcptt {

std::string xmlEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
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

std::string mcpttInfo(const std::string& sessionType, const std::string& requestUri,
                      const std::string& callingUserId, const std::string& callingGroupId,
                      int emergency, int imminentPeril) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += std::string("<mcpttinfo xmlns=\"") + kNsMcpttInfo + "\">\n  <mcptt-Params>\n";
    s += "    <session-type>" + sessionType + "</session-type>\n";
    s += "    <mcptt-request-uri>" + xmlEscape(requestUri) + "</mcptt-request-uri>\n";
    s += "    <mcptt-calling-user-id>" + xmlEscape(callingUserId) + "</mcptt-calling-user-id>\n";
    s += "    <mcptt-calling-group-id>" + xmlEscape(callingGroupId) + "</mcptt-calling-group-id>\n";
    if (emergency) s += std::string("    <emergency-ind>") + (emergency > 0 ? "true" : "false") + "</emergency-ind>\n";
    if (imminentPeril) s += std::string("    <imminentperil-ind>") + (imminentPeril > 0 ? "true" : "false") + "</imminentperil-ind>\n";
    s += "  </mcptt-Params>\n</mcpttinfo>\n";
    return s;
}

std::string resourceLists(const std::vector<std::string>& members) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += std::string("<resource-lists xmlns=\"") + kNsResourceLists + "\" xmlns:mcpttgi=\"" + kNsGroupInfo + "\">\n  <list>\n";
    for (auto& m : members) {
        s += "    <entry uri=\"" + xmlEscape(m) + "\">\n";
        s += "      <mcpttgi:participant-type>participant</mcpttgi:participant-type>\n";
        s += "      <mcpttgi:user-priority>0</mcpttgi:user-priority>\n";
        s += "    </entry>\n";
    }
    s += "  </list>\n</resource-lists>\n";
    return s;
}

std::string affiliationCommand(const std::string& groupUri, bool affiliate) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += std::string("<affiliation-command xmlns=\"") + kNsAffiliation + "\">\n  <actions>\n";
    s += std::string("    <") + (affiliate ? "affiliate" : "de-affiliate") + " group=\"" + xmlEscape(groupUri) + "\"/>\n";
    s += "  </actions>\n</affiliation-command>\n";
    return s;
}

static std::string elemText(const std::string& s, const std::string& name) {
    size_t p = s.find("<" + name);
    if (p == std::string::npos) return std::string();
    size_t gt = s.find('>', p);
    if (gt == std::string::npos) return std::string();
    size_t e = s.find("</" + name, gt);
    if (e == std::string::npos) return std::string();
    std::string v = s.substr(gt + 1, e - gt - 1);
    size_t b = v.find_first_not_of(" \t\r\n"), t = v.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : v.substr(b, t - b + 1);
}

static bool textIsTrue(const std::string& v) {
    return v == "true" || v == "1" || v == "TRUE" || v == "True";
}

McpttInfo parseMcpttInfo(const std::string& whole) {
    McpttInfo mi;
    size_t p = whole.find("<mcpttinfo");
    if (p == std::string::npos) return mi;
    std::string x = whole.substr(p);
    mi.present = true;
    mi.sessionType = elemText(x, "session-type");
    mi.requestUri = elemText(x, "mcptt-request-uri");
    mi.callingUserId = elemText(x, "mcptt-calling-user-id");
    mi.callingGroupId = elemText(x, "mcptt-calling-group-id");
    mi.emergency = textIsTrue(elemText(x, "emergency-ind"));
    mi.imminentPeril = textIsTrue(elemText(x, "imminentperil-ind"));
    mi.privateCall = mi.sessionType == "private";
    mi.noFloorCtrl = whole.find("mc_no_floor_ctrl") != std::string::npos;
    return mi;
}

bool parseConferenceInfo(const std::string& xml, std::vector<RosterEntry>& users, bool& full) {
    users.clear();
    size_t ci = xml.find("<conference-info");
    if (ci == std::string::npos) return false;
    size_t gt = xml.find('>', ci);
    std::string head = xml.substr(ci, gt == std::string::npos ? std::string::npos : gt - ci);
    full = head.find("state=\"full\"") != std::string::npos;
    size_t pos = 0;
    while ((pos = xml.find("<user", pos)) != std::string::npos) {
        // "<user " 또는 "<user>" 만 (users 컨테이너 제외)
        char nc = pos + 5 < xml.size() ? xml[pos + 5] : '\0';
        if (nc != ' ' && nc != '>' && nc != '\t' && nc != '\n') { pos += 5; continue; }
        size_t end = xml.find("</user>", pos);
        if (end == std::string::npos) break;
        std::string u = xml.substr(pos, end - pos);
        RosterEntry e;
        size_t ent = u.find("entity=\"");
        if (ent != std::string::npos) {
            size_t q = u.find('"', ent + 8);
            if (q != std::string::npos) e.uri = u.substr(ent + 8, q - ent - 8);
        }
        e.status = elemText(u, "status");
        if (!e.uri.empty()) users.push_back(e);
        pos = end + 7;
    }
    return true;
}

static std::string attrOf(const std::string& tag, const char* name) {
    std::string key = std::string(name) + "=\"";
    size_t p = tag.find(key);
    if (p == std::string::npos) return std::string();
    size_t e = tag.find('"', p + key.size());
    return e == std::string::npos ? std::string() : tag.substr(p + key.size(), e - p - key.size());
}

bool parseDialogInfo(const std::string& xml, std::vector<DialogInfo>& out) {
    out.clear();
    size_t di = xml.find("<dialog-info");
    if (di == std::string::npos) return false;
    size_t gt = xml.find('>', di);
    std::string head = xml.substr(di, gt == std::string::npos ? std::string::npos : gt - di + 1);
    std::string entity = attrOf(head, "entity");
    bool full = attrOf(head, "state") == "full";
    size_t pos = gt == std::string::npos ? di : gt;
    while ((pos = xml.find("<dialog", pos)) != std::string::npos) {
        char nc = pos + 7 < xml.size() ? xml[pos + 7] : '\0';
        if (nc != ' ' && nc != '>' && nc != '\t' && nc != '\n') { pos += 7; continue; }
        size_t tgt = xml.find('>', pos);
        if (tgt == std::string::npos) break;
        std::string tag = xml.substr(pos, tgt - pos + 1);
        size_t end = xml.find("</dialog>", tgt);
        std::string body = xml.substr(tgt + 1, end == std::string::npos ? std::string::npos : end - tgt - 1);
        DialogInfo d;
        d.watched = entity; d.full = full;
        d.id = attrOf(tag, "id"); d.callId = attrOf(tag, "call-id");
        d.localTag = attrOf(tag, "local-tag"); d.remoteTag = attrOf(tag, "remote-tag");
        d.direction = attrOf(tag, "direction");
        d.state = elemText(body, "state");
        size_t r = body.find("<remote");
        if (r != std::string::npos) d.remoteIdentity = elemText(body.substr(r), "identity");
        out.push_back(d);
        pos = end == std::string::npos ? xml.size() : end + 9;
    }
    return true;
}

std::vector<MediaSource> sdpSsrcLabels(const std::string& sdp) {
    std::vector<MediaSource> out;
    size_t pos = 0;
    while ((pos = sdp.find("a=ssrc:", pos)) != std::string::npos) {
        size_t eol = sdp.find_first_of("\r\n", pos);
        std::string line = sdp.substr(pos + 7, eol == std::string::npos ? std::string::npos : eol - pos - 7);
        MediaSource m;
        m.ssrc = (uint32_t)std::strtoul(line.c_str(), nullptr, 10);
        size_t l = line.find("label:");
        if (l != std::string::npos) { size_t e = line.find_first_of(" \t", l); m.label = line.substr(l + 6, e == std::string::npos ? std::string::npos : e - l - 6); }
        m.active = true;
        bool dup = false;
        for (auto& x : out) if (x.ssrc == m.ssrc) { dup = true; if (x.label.empty()) x.label = m.label; }
        if (!dup && m.ssrc) out.push_back(m);
        pos = eol == std::string::npos ? sdp.size() : eol;
    }
    return out;
}

std::string bareId(const std::string& uri) {
    std::string s = uri;
    size_t lt = s.find('<');
    if (lt != std::string::npos) {
        size_t gt = s.find('>', lt);
        s = s.substr(lt + 1, gt == std::string::npos ? std::string::npos : gt - lt - 1);
    }
    for (const char* sch : {"tel:", "sips:", "sip:"}) {
        size_t p = s.find(sch);
        if (p != std::string::npos) { s = s.substr(p + std::string(sch).size()); break; }
    }
    size_t e = s.find_first_of("@>; \t\r\n");
    if (e != std::string::npos) s = s.substr(0, e);
    return s;
}

}  // namespace mcptt
}  // namespace cimsue
