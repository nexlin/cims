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

// mcptt-info contentType 요소(TS 24.379 Annex F.1 의미 2)) — 암호화하지 않으면 type="Normal" + 자식. URI 류 = <mcpttURI>,
//   mcptt-client-id·mcptt-access-token = <mcpttString>, 지시자 = <mcpttBoolean>. session-type·broadcast-ind 는 단순 값.
static std::string infoUri(const char* tag, const std::string& v) {
    return std::string("    <") + tag + " type=\"Normal\"><mcpttURI>" + xmlEscape(v) + "</mcpttURI></" + tag + ">\n";
}
static std::string infoString(const char* tag, const std::string& v) {
    return std::string("    <") + tag + " type=\"Normal\"><mcpttString>" + xmlEscape(v) + "</mcpttString></" + tag + ">\n";
}
static std::string infoBool(const char* tag, bool v) {
    return std::string("    <") + tag + " type=\"Normal\"><mcpttBoolean>" + (v ? "true" : "false") + "</mcpttBoolean></" + tag + ">\n";
}

std::string mcpttInfo(const std::string& sessionType, const std::string& requestUri,
                      const std::string& callingUserId, const std::string& callingGroupId,
                      int emergency, int imminentPeril, bool broadcast, int alert) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += std::string("<mcpttinfo xmlns=\"") + kNsMcpttInfo + "\">\n  <mcptt-Params>\n";
    // 요소 순서 = mcptt-ParamsType sequence(Annex F.1).
    s += "    <session-type>" + xmlEscape(sessionType) + "</session-type>\n";
    s += infoUri("mcptt-request-uri", requestUri);
    s += infoUri("mcptt-calling-user-id", callingUserId);
    s += infoUri("mcptt-calling-group-id", callingGroupId);
    if (emergency) s += infoBool("emergency-ind", emergency > 0);
    if (alert) s += infoBool("alert-ind", alert > 0);
    if (imminentPeril) s += infoBool("imminentperil-ind", imminentPeril > 0);
    if (broadcast) s += "    <broadcast-ind>true</broadcast-ind>\n";
    s += "  </mcptt-Params>\n</mcpttinfo>\n";
    return s;
}

std::string mcpttInfoOriginating(const std::string& sessionType, const std::string& requestUri, const std::string& clientId,
                                 int emergency, int imminentPeril, bool broadcast, int alert) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += std::string("<mcpttinfo xmlns=\"") + kNsMcpttInfo + "\">\n  <mcptt-Params>\n";
    // 요소 순서 = mcptt-ParamsType sequence(Annex F.1) — session-type · mcptt-request-uri · … · mcptt-client-id · 지시자
    s += "    <session-type>" + xmlEscape(sessionType) + "</session-type>\n";
    if (!requestUri.empty()) s += infoUri("mcptt-request-uri", requestUri);
    if (emergency) s += infoBool("emergency-ind", emergency > 0);
    if (alert) s += infoBool("alert-ind", alert > 0);
    if (imminentPeril) s += infoBool("imminentperil-ind", imminentPeril > 0);
    if (broadcast) s += "    <broadcast-ind>true</broadcast-ind>\n";
    if (!clientId.empty()) s += infoString("mcptt-client-id", clientId);
    s += "  </mcptt-Params>\n</mcpttinfo>\n";
    return s;
}

std::string contactFeatureParams() { return ";+g.3gpp.mcptt;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\""; }

std::string alertInfo(const std::string& groupUri, const std::string& callingUserId, const std::string& clientId,
                      bool activate, const std::string& originatedBy, int emergency) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += std::string("<mcpttinfo xmlns=\"") + kNsMcpttInfo + "\">\n  <mcptt-Params>\n";
    s += infoUri("mcptt-request-uri", groupUri);
    if (!callingUserId.empty()) s += infoUri("mcptt-calling-user-id", callingUserId);
    if (emergency) s += infoBool("emergency-ind", emergency > 0);
    s += infoBool("alert-ind", activate);
    if (!originatedBy.empty()) s += infoUri("originated-by", originatedBy);
    if (!clientId.empty()) s += infoString("mcptt-client-id", clientId);
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

std::string xcapDiffResourceLists(const std::vector<std::string>& documents) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += std::string("<resource-lists xmlns=\"") + kNsResourceLists + "\">\n  <list>\n";
    for (auto& d : documents) s += "    <entry uri=\"" + xmlEscape(d) + "\"/>\n";
    s += "  </list>\n</resource-lists>\n";
    return s;
}

std::string accessTokenInfo(const std::string& accessToken) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += std::string("<mcpttinfo xmlns=\"") + kNsMcpttInfo + "\">\n  <mcptt-Params>\n";
    s += infoString("mcptt-access-token", accessToken);
    s += "  </mcptt-Params>\n</mcpttinfo>\n";
    return s;
}

std::string affiliationCommand(const std::string& groupUri, bool affiliate) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += std::string("<affiliation-command xmlns=\"") + kNsAffiliation + "\">\n  <actions>\n";
    s += std::string("    <") + (affiliate ? "affiliate" : "de-affiliate") + " group=\"" + xmlEscape(groupUri) + "\"/>\n";
    s += "  </actions>\n</affiliation-command>\n";
    return s;
}

std::string affiliationInfo(const std::string& targetMcpttId) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += std::string("<mcpttinfo xmlns=\"") + kNsMcpttInfo + "\">\n  <mcptt-Params>\n";
    s += infoUri("mcptt-request-uri", targetMcpttId);
    s += "  </mcptt-Params>\n</mcpttinfo>\n";
    return s;
}

std::string affiliationPidf(const std::string& entity, const std::string& clientId, const std::vector<std::string>& groupUris,
                            const std::string& pid) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += std::string("<presence xmlns=\"urn:ietf:params:xml:ns:pidf\" xmlns:mcpttPI10=\"") + kNsPresInfo + "\"\n";
    s += "  entity=\"" + xmlEscape(entity) + "\">\n";
    s += "  <tuple id=\"" + xmlEscape(clientId) + "\">\n    <status>\n";
    for (const auto& g : groupUris) s += "      <mcpttPI10:affiliation group=\"" + xmlEscape(g) + "\"/>\n";
    s += "    </status>\n  </tuple>\n";
    s += "  <mcpttPI10:p-id>" + xmlEscape(pid) + "</mcpttPI10:p-id>\n</presence>\n";
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

static std::string xmlUnescape(std::string v) {
    static const char* const kEnt[][2] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (auto& e : kEnt) {
        size_t k = 0;
        const std::string a = e[0], b = e[1];
        while ((k = v.find(a, k)) != std::string::npos) { v.replace(k, a.size(), b); k += b.size(); }
    }
    return v;
}

static bool textIsTrue(const std::string& v) {
    return v == "true" || v == "1" || v == "TRUE" || v == "True";
}

/** 접두사 무관 요소 텍스트 — "<[p:]local" 첫 매치. 없으면 found=false. */
static std::string localText(const std::string& s, const std::string& local, bool* found) {
    *found = false;
    size_t p = 0;
    while ((p = s.find('<', p)) != std::string::npos) {
        size_t q = p + 1, n = q;
        while (n < s.size() && s[n] != '>' && s[n] != ' ' && s[n] != '/' && s[n] != '\t' && s[n] != '\r' && s[n] != '\n') ++n;
        std::string tag = s.substr(q, n - q);
        size_t colon = tag.find(':');
        if (colon != std::string::npos) tag = tag.substr(colon + 1);
        if (tag != local) { p = n; continue; }
        size_t gt = s.find('>', p);
        if (gt == std::string::npos) return std::string();
        *found = true;
        if (s[gt - 1] == '/') return std::string();
        size_t lt = s.find('<', gt + 1);
        if (lt == std::string::npos) return std::string();
        size_t from = gt;
        // Annex F.1 contentType — 값이 자식 <mcpttURI>/<mcpttString>/<mcpttBoolean> 에 있다(값 직접 기재 형식도 읽는다).
        if (lt + 1 < s.size() && s[lt + 1] != '/') {
            size_t cgt = s.find('>', lt);
            if (cgt == std::string::npos) return std::string();
            if (s[cgt - 1] == '/') return std::string();
            from = cgt;
            lt = s.find('<', cgt + 1);
            if (lt == std::string::npos) return std::string();
        }
        std::string v = s.substr(from + 1, lt - from - 1);
        size_t b = v.find_first_not_of(" \t\r\n"), t = v.find_last_not_of(" \t\r\n");
        return b == std::string::npos ? std::string() : xmlUnescape(v.substr(b, t - b + 1));
    }
    return std::string();
}

std::vector<std::string> nonAcknowledgedUsers(const std::string& xml) {
    std::vector<std::string> out;
    size_t p = 0;
    for (;;) {
        // 요소마다 localText 로 값(contentType 자식 <mcpttURI> 포함)을 읽고 그 닫는 태그 뒤로 넘어간다
        bool f = false;
        const std::string rest = xml.substr(p);
        std::string v = localText(rest, "non-acknowledged-user", &f);
        if (!f) break;
        if (!v.empty()) out.push_back(bareId(v));
        // 닫는 태그 "</[p:]non-acknowledged-user>" 뒤로 (자기 닫힘 "<… />" 이면 그 태그 뒤로)
        size_t open = rest.find("non-acknowledged-user");
        size_t gt = rest.find('>', open);
        if (gt == std::string::npos) break;
        size_t next = gt + 1;
        if (rest[gt - 1] != '/') {
            size_t c = gt;
            while ((c = rest.find("</", c)) != std::string::npos) {
                size_t n = c + 2, e = rest.find('>', n);
                if (e == std::string::npos) { c = std::string::npos; break; }
                std::string tag = rest.substr(n, e - n);
                size_t colon = tag.find(':');
                if (colon != std::string::npos) tag = tag.substr(colon + 1);
                if (tag == "non-acknowledged-user") { next = e + 1; break; }
                c = e;
            }
            if (c == std::string::npos) break;
        }
        p += next;
    }
    return out;
}

int indicator(const std::string& xml, const std::string& local) {
    bool f = false;
    std::string v = localText(xml, local, &f);
    if (!f) return 0;
    return textIsTrue(v) ? 1 : -1;
}

bool parseEmergencyAlert(const std::string& body, EmergencyAlert& out) {
    size_t p = body.find("mcpttinfo");
    if (p == std::string::npos) return false;
    std::string x = body.substr(p == 0 ? 0 : p - 1);
    EmergencyAlert a;
    a.accountId = out.accountId;
    a.alertInd = indicator(x, "alert-ind");
    a.emergencyInd = indicator(x, "emergency-ind");
    a.imminentPerilInd = indicator(x, "imminentperil-ind");
    if (!a.alertInd && !a.emergencyInd && !a.imminentPerilInd) return false;
    bool f = false;
    std::string g = localText(x, "mcptt-calling-group-id", &f);
    if (g.empty()) g = localText(x, "mcptt-request-uri", &f);
    a.groupId = bareId(g);
    a.userId = bareId(localText(x, "mcptt-calling-user-id", &f));
    a.originatedBy = bareId(localText(x, "originated-by", &f));
    a.mcOrg = localText(x, "mc-org", &f);
    out = a;
    return true;
}

McpttInfo parseMcpttInfo(const std::string& whole) {
    McpttInfo mi;
    size_t p = whole.find("mcpttinfo");                     // 접두사 무관(<mi:mcpttinfo …>)
    if (p == std::string::npos) return mi;
    size_t lt = whole.rfind('<', p);
    std::string x = whole.substr(lt == std::string::npos ? p : lt);
    mi.present = true;
    // 접두사 무관·이름 경계 일치·contentType 자식 풀기(localText) — 서버가 두 인코딩 어느 쪽으로 보내도 같다.
    bool f = false;
    mi.sessionType = localText(x, "session-type", &f);
    mi.requestUri = localText(x, "mcptt-request-uri", &f);
    mi.callingUserId = localText(x, "mcptt-calling-user-id", &f);
    mi.callingGroupId = localText(x, "mcptt-calling-group-id", &f);
    mi.emergency = textIsTrue(localText(x, "emergency-ind", &f));
    mi.imminentPeril = textIsTrue(localText(x, "imminentperil-ind", &f));
    mi.broadcast = textIsTrue(localText(x, "broadcast-ind", &f));
    mi.privateCall = mi.sessionType == "private";
    // floor 없는 개별 호 = offer 에 floor 제어 채널(m=application … MCPTT)이 없다(TS 24.379 §11.1.2.2). `mc_no_floor_ctrl` 은 사전 설정 세션
    //   용이라(TS 24.380 §14.2.6) 이 판정에 쓰지 않는다. SDP 가 없는 초대는 판정하지 않는다.
    mi.noFloorCtrl = mi.privateCall && whole.find("m=audio") != std::string::npos && !isMcpttSdp(whole);
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

std::string floorSdp(int localPort, bool fullDuplex, bool implicitRequest) {
    std::string fmtp = "a=fmtp:MCPTT mc_queueing";
    if (fullDuplex) fmtp += ";mc_no_floor_ctrl";
    else if (implicitRequest) fmtp += ";mc_implicit_request;mc_granted";
    // TS 24.380 표 4.3.3.1-1 — `m=application <port> udp MCPTT`(proto = "udp"). floor 채널은 fmtp 하나로 협상한다(a=floorid 는 규격에 없다).
    return "m=application " + std::to_string(localPort) + " udp MCPTT\r\n" + fmtp;
}

bool isMcpttSdp(const std::string& sdp) {
    for (size_t pos = sdp.find("m=application "); pos != std::string::npos; pos = sdp.find("m=application ", pos + 1)) {
        const size_t eol = sdp.find_first_of("\r\n", pos);
        std::string ln = sdp.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        for (auto& c : ln) c = (char)std::tolower((unsigned char)c);
        if (ln.size() >= 10 && ln.compare(ln.size() - 10, 10, " udp mcptt") == 0) return true;
    }
    return false;
}

std::string withSpeechInfo(const std::string& sdp) {
    if (!isMcpttSdp(sdp)) return sdp;
    std::string out;
    size_t pos = 0;
    while (pos < sdp.size()) {
        size_t nl = sdp.find('\n', pos);
        const size_t end = nl == std::string::npos ? sdp.size() : nl + 1;
        const std::string ln = sdp.substr(pos, end - pos);                 // 줄 끝(CRLF·LF) 포함
        out += ln;
        if (ln.rfind("m=audio ", 0) == 0 && sdp.compare(end, 2, "i=") != 0) {
            const bool crlf = ln.size() >= 2 && ln[ln.size() - 2] == '\r';
            out += std::string("i=speech") + (crlf ? "\r\n" : "\n");
        }
        pos = end;
    }
    return out;
}

std::string forSubsequentOffer(const std::string& sdp) {
    if (!isMcpttSdp(sdp)) return sdp;
    static const std::string pre = "a=fmtp:MCPTT";
    const size_t at = sdp.find(pre);
    if (at == std::string::npos) return sdp;
    size_t eol = sdp.find_first_of("\r\n", at);
    if (eol == std::string::npos) eol = sdp.size();
    size_t next = eol;
    while (next < sdp.size() && (sdp[next] == '\r' || sdp[next] == '\n')) ++next;
    const std::string params = sdp.substr(at + pre.size(), eol - at - pre.size());
    std::string kept;
    for (size_t b = 0; b <= params.size();) {
        size_t e = params.find(';', b);
        if (e == std::string::npos) e = params.size();
        std::string tok = params.substr(b, e - b);
        b = e + 1;
        const size_t x = tok.find_first_not_of(" \t");
        if (x == std::string::npos) continue;
        tok = tok.substr(x, tok.find_last_not_of(" \t") - x + 1);
        std::string low = tok;
        for (auto& c : low) c = (char)std::tolower((unsigned char)c);
        if (low == "mc_granted" || low == "mc_implicit_request") continue;
        kept += (kept.empty() ? "" : ";") + tok;
    }
    const std::string line = kept.empty() ? std::string() : pre + " " + kept + sdp.substr(eol, next - eol);
    return sdp.substr(0, at) + line + sdp.substr(next);
}

FloorFmtp parseFloorFmtp(const std::string& sdp) {
    FloorFmtp f;
    size_t m = sdp.find("m=application ");
    if (m == std::string::npos) return f;
    size_t next = sdp.find("\nm=", m + 1);
    std::string section = sdp.substr(m, next == std::string::npos ? std::string::npos : next - m);
    size_t a = section.find("a=fmtp:MCPTT");
    if (a == std::string::npos) return f;
    f.present = true;
    size_t eol = section.find_first_of("\r\n", a);
    std::string params = section.substr(a + 12, eol == std::string::npos ? std::string::npos : eol - a - 12);
    size_t pos = 0;
    while (pos <= params.size()) {
        size_t end = params.find(';', pos);
        if (end == std::string::npos) end = params.size();
        std::string tok = params.substr(pos, end - pos);
        pos = end + 1;
        size_t b = tok.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        tok = tok.substr(b, tok.find_last_not_of(" \t") - b + 1);
        for (auto& c : tok) c = (char)std::tolower((unsigned char)c);
        if (tok == "mc_queueing") f.queueing = true;
        else if (tok == "mc_implicit_request") f.implicitRequest = true;
        else if (tok == "mc_granted") f.granted = true;
        else if (tok == "mc_no_floor_ctrl") f.noFloorCtrl = true;
    }
    return f;
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
