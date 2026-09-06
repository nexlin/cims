#include "cimsue/csc.h"

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "http/https_client.h"
#include "mcdata/sds_codec.h"   // base64Encode

namespace cimsue {

namespace {

std::string base64Url(const std::string& raw) {
    std::string b = mcdata::base64Encode(raw);
    for (auto& c : b) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
    while (!b.empty() && b.back() == '=') b.pop_back();
    return b;
}
std::string randomBytes(int n) {
    std::string s(n, '\0');
    RAND_bytes((unsigned char*)&s[0], n);
    return s;
}
std::string sha256(const std::string& s) {
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)s.data(), s.size(), md);
    return std::string((const char*)md, SHA256_DIGEST_LENGTH);
}

/** 최소 JSON 파서 — CscClient 는 Engine(pjlib)과 독립·임의 스레드에서 쓰이므로 pj_json 을 쓰지 않는다.
 *  객체/배열/문자열(escape·\uXXXX)/숫자/불/null. 값은 트리로 들고 이름으로 찾는다. */
struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false; double n = 0; std::string s;
    std::vector<std::pair<std::string, JVal>> kids;     // Obj: name/value, Arr: name 빈 문자열
    const JVal* get(const char* k) const { if (t != Obj) return nullptr; for (auto& kv : kids) if (kv.first == k) return &kv.second; return nullptr; }
};
struct JParser {
    const std::string& s; size_t p = 0; bool ok = true;
    explicit JParser(const std::string& in) : s(in) {}
    void ws() { while (p < s.size() && std::isspace((unsigned char)s[p])) ++p; }
    bool lit(const char* l) { size_t n = std::strlen(l); if (s.compare(p, n, l) == 0) { p += n; return true; } return false; }
    JVal parse() { ws(); JVal v;
        if (p >= s.size()) { ok = false; return v; }
        char c = s[p];
        if (c == '{') { v.t = JVal::Obj; ++p; ws(); if (p < s.size() && s[p] == '}') { ++p; return v; }
            while (ok) { ws(); JVal k = parse(); if (k.t != JVal::Str) { ok = false; break; } ws(); if (!lit(":")) { ok = false; break; }
                JVal val = parse(); v.kids.emplace_back(k.s, val); ws(); if (lit(",")) continue; if (lit("}")) break; ok = false; }
        } else if (c == '[') { v.t = JVal::Arr; ++p; ws(); if (p < s.size() && s[p] == ']') { ++p; return v; }
            while (ok) { JVal val = parse(); v.kids.emplace_back(std::string(), val); ws(); if (lit(",")) continue; if (lit("]")) break; ok = false; }
        } else if (c == '"') { v.t = JVal::Str; ++p;
            while (p < s.size() && s[p] != '"') {
                if (s[p] == '\\' && p + 1 < s.size()) { ++p; char e = s[p];
                    if (e == 'n') v.s += '\n'; else if (e == 't') v.s += '\t'; else if (e == 'r') v.s += '\r'; else if (e == 'b') v.s += '\b'; else if (e == 'f') v.s += '\f';
                    else if (e == 'u' && p + 4 < s.size()) { unsigned cp = (unsigned)std::strtoul(s.substr(p + 1, 4).c_str(), nullptr, 16); p += 4;
                        if (cp < 0x80) v.s += (char)cp; else if (cp < 0x800) { v.s += (char)(0xC0 | (cp >> 6)); v.s += (char)(0x80 | (cp & 0x3F)); }
                        else { v.s += (char)(0xE0 | (cp >> 12)); v.s += (char)(0x80 | ((cp >> 6) & 0x3F)); v.s += (char)(0x80 | (cp & 0x3F)); } }
                    else v.s += e; ++p; }
                else v.s += s[p++]; }
            if (p < s.size()) ++p; else ok = false;
        } else if (lit("true")) { v.t = JVal::Bool; v.b = true; }
        else if (lit("false")) { v.t = JVal::Bool; v.b = false; }
        else if (lit("null")) { v.t = JVal::Null; }
        else { size_t e = p; while (e < s.size() && (std::isdigit((unsigned char)s[e]) || s[e] == '-' || s[e] == '+' || s[e] == '.' || s[e] == 'e' || s[e] == 'E')) ++e;
            if (e == p) { ok = false; return v; } v.t = JVal::Num; v.n = std::atof(s.substr(p, e - p).c_str()); p = e; }
        return v; }
};
struct Json {
    JVal rootVal; const JVal* root = nullptr;
    explicit Json(const std::string& text) { JParser jp(text); rootVal = jp.parse(); if (jp.ok) root = &rootVal; }
    static const JVal* child(const JVal* obj, const char* name) { return obj ? obj->get(name) : nullptr; }
    static std::string str(const JVal* obj, const char* name, const std::string& dflt = std::string()) {
        const JVal* e = child(obj, name); if (!e) return dflt;
        if (e->t == JVal::Str) return e->s; if (e->t == JVal::Num) return std::to_string((long)e->n); if (e->t == JVal::Bool) return e->b ? "true" : "false";
        return dflt; }
    static int num(const JVal* obj, const char* name, int dflt) { const JVal* e = child(obj, name); if (!e) return dflt;
        if (e->t == JVal::Num) return (int)e->n; if (e->t == JVal::Str) return std::atoi(e->s.c_str()); return dflt; }
    static bool boolean(const JVal* obj, const char* name, bool dflt) { const JVal* e = child(obj, name); return (e && e->t == JVal::Bool) ? e->b : dflt; }
    template <typename F> static void each(const JVal* arr, F f) { if (!arr || arr->t != JVal::Arr) return; for (auto& kv : arr->kids) f(&kv.second); }
};

Transport transportOf(const std::string& s) {
    if (s == "TLS" || s == "tls") return Transport::TLS;
    if (s == "TCP" || s == "tcp") return Transport::TCP;
    return Transport::UDP;
}

}  // namespace

// ── ServiceProfile / Profile ──

AccountConfig ServiceProfile::toAccount(const std::string& loginPw) const {
    AccountConfig a;
    a.serverHost = sipHost; a.serverPort = sipPort; a.transport = transport;
    a.domain = domain; a.msisdn = msisdn; a.imsi = imsi; a.authId = authId;
    a.ha1 = sipHa1; if (sipHa1.empty()) a.password = loginPw;
    a.authScheme = authScheme; a.akaK = akaK; a.akaOpc = akaOpc; a.akaAmf = akaAmf;
    a.secMechanisms = secMechanisms; a.mediaSecurity = mediaSecurity;
    a.mcpttId = mcpttId;
    return a;
}

const ServiceProfile* Profile::service(const std::string& kind) const {
    for (auto& s : services) if (s.kind == kind) return &s;
    return nullptr;
}

// ── CscClient ──

struct CscClient::Impl {
    CscEndpoint ep;
    std::shared_ptr<http::ITransport> tp;
};

CscClient::CscClient(const CscEndpoint& ep, std::shared_ptr<http::ITransport> transport) : impl_(new Impl) {
    impl_->ep = ep;
    impl_->tp = transport ? transport : std::make_shared<http::OpenSslTransport>(ep.caPem, ep.verifyServer);
}
CscClient::~CscClient() = default;

std::string CscClient::enc(const std::string& s) { return http::urlEncode(s); }

static Result httpFail(const http::Response& r, const char* what) {
    if (r.status == 0) return Result::fail(-1, std::string(what) + ": " + r.error);
    return Result::fail(r.status, std::string(what) + " " + std::to_string(r.status) + ": " + r.body.substr(0, 200));
}

static bool parseToken(const std::string& body, TokenSet& t) {
    Json j(body);
    if (!j.root) return false;
    t.accessToken = Json::str(j.root, "access_token");
    t.tokenType = Json::str(j.root, "token_type", "Bearer");
    t.refreshToken = Json::str(j.root, "refresh_token");
    t.idToken = Json::str(j.root, "id_token");
    t.scope = Json::str(j.root, "scope");
    t.expiresInSec = Json::num(j.root, "expires_in", 3600);
    return !t.accessToken.empty();
}

Result CscClient::login(const std::string& userName, const std::string& password, TokenSet& out) {
    // PKCE S256 (RFC 7636): verifier = base64url(32B 난수), challenge = base64url(SHA-256(verifier))
    std::string verifier = base64Url(randomBytes(32));
    std::string challenge = base64Url(sha256(verifier));
    std::string state = base64Url(randomBytes(16));
    std::string url = impl_->ep.baseUrl() + "/idms/authreq?user_name=" + enc(userName) + "&user_password=" + enc(password) +
                      "&client_id=" + enc(impl_->ep.clientId) + "&redirect_uri=" + enc(impl_->ep.redirectUri) +
                      "&code_challenge=" + challenge + "&code_challenge_method=S256&scope=" + enc(impl_->ep.scope) +
                      "&state=" + state;
    http::Response r = impl_->tp->request("GET", url, {}, "");
    if (r.status / 100 != 2) return httpFail(r, "authreq");
    std::string code;
    { Json j(r.body); if (!j.root) return Result::fail(-2, "authreq: bad json"); code = Json::str(j.root, "code"); }
    if (code.empty()) return Result::fail(-2, "authreq: no code");
    std::string form = "grant_type=authorization_code&code=" + enc(code) + "&client_id=" + enc(impl_->ep.clientId) +
                       "&redirect_uri=" + enc(impl_->ep.redirectUri) + "&code_verifier=" + verifier;
    r = impl_->tp->request("POST", impl_->ep.baseUrl() + "/idms/tokenreq",
                           {{"Content-Type", "application/x-www-form-urlencoded"}}, form);
    if (r.status / 100 != 2) return httpFail(r, "tokenreq");
    if (!parseToken(r.body, out)) return Result::fail(-2, "tokenreq: bad json");
    return Result::success();
}

Result CscClient::refresh(const std::string& refreshToken, TokenSet& out) {
    std::string form = "grant_type=refresh_token&refresh_token=" + enc(refreshToken) + "&client_id=" + enc(impl_->ep.clientId);
    http::Response r = impl_->tp->request("POST", impl_->ep.baseUrl() + "/idms/tokenreq",
                                          {{"Content-Type", "application/x-www-form-urlencoded"}}, form);
    if (r.status / 100 != 2) return httpFail(r, "refresh");
    if (!parseToken(r.body, out)) return Result::fail(-2, "refresh: bad json");
    return Result::success();
}

bool CscClient::parseProfile(const std::string& json, Profile& out, std::string* err) {
    Json j(json);
    if (!j.root) { if (err) *err = "bad json"; return false; }
    const JVal* user = Json::child(j.root, "user");
    out.displayName = Json::str(user, "displayName");
    out.loginId = Json::str(user, "loginId");
    out.countryCode = Json::str(j.root, "countryCode");
    const JVal* csc = Json::child(j.root, "csc");
    out.cscHost = Json::str(csc, "host"); out.cscPort = Json::num(csc, "port", 4430);
    out.services.clear();
    Json::each(Json::child(j.root, "services"), [&](const JVal* s) {
        ServiceProfile sp;
        sp.kind = Json::str(s, "kind");
        const JVal* sip = Json::child(s, "sip");
        sp.sipHost = Json::str(sip, "host");
        sp.sipPort = Json::num(sip, "port", 5060);
        sp.transport = transportOf(Json::str(sip, "default", Json::str(sip, "transport", "UDP")));
        Json::each(Json::child(sip, "transports"), [&](const JVal* t) {
            sp.transports.push_back({transportOf(Json::str(t, "transport")), Json::num(t, "port", 0)});
        });
        // 기본 transport 의 유효 포트 — 목록에서 다시 맞춘다(transport 와 포트는 쌍)
        for (auto& t : sp.transports) if (t.transport == sp.transport && t.port > 0) sp.sipPort = t.port;
        sp.enforced = Json::boolean(sip, "enforced", false);
        std::string ms = Json::str(sip, "mediaSecurity", "off");
        sp.mediaSecurity = ms == "required" ? MediaSecurity::Required : ms == "optional" ? MediaSecurity::Optional : MediaSecurity::Off;
        sp.domain = Json::str(sip, "domain");
        Json::each(Json::child(sip, "security"), [&](const JVal* m) {
            if (m->t == JVal::Str) sp.secMechanisms.push_back(m->s);
        });
        const JVal* acc = Json::child(s, "account");
        sp.msisdn = Json::str(acc, "msisdn"); sp.imsi = Json::str(acc, "imsi"); sp.authId = Json::str(acc, "authId");
        sp.sipHa1 = Json::str(acc, "sipHa1"); sp.mcpttId = Json::str(acc, "mcpttId");
        sp.authScheme = Json::str(acc, "authScheme", "digest") == "aka" ? AuthScheme::Aka : AuthScheme::Digest;
        const JVal* aka = Json::child(acc, "aka");
        sp.akaK = Json::str(aka, "k"); sp.akaOpc = Json::str(aka, "opc"); sp.akaAmf = Json::str(aka, "amf", "8000");
        sp.maxPayloadSdsCplaneBytes = Json::num(s, "maxPayloadSdsCplaneBytes", 0);
        out.services.push_back(sp);
    });
    const JVal* d = Json::child(j.root, "dispatch");
    out.dispatch = DispatchProfile{};
    if (d) {
        out.dispatch.present = true;
        out.dispatch.groupId = Json::str(d, "groupId"); out.dispatch.groupName = Json::str(d, "groupName");
        out.dispatch.pilotId = Json::str(d, "pilotId"); out.dispatch.monitorScope = Json::str(d, "monitorScope", "none");
        out.dispatch.pttListen = Json::str(d, "pttListen", "none"); out.dispatch.listenVisibility = Json::str(d, "listenVisibility", "hidden");
        // 발견(discovery) 확장 — 서버가 아직 주지 않으면 빈 배열(앱은 로컬 폴백).
        Json::each(Json::child(d, "members"), [&](const JVal* m) {
            DispatchMember dm;
            dm.userId = Json::str(m, "userId"); dm.name = Json::str(m, "name"); dm.volteAor = Json::str(m, "volteAor");
            dm.pttId = Json::str(m, "pttId"); dm.extension = Json::str(m, "extension"); dm.groupId = Json::str(m, "groupId");
            if (!dm.volteAor.empty() || !dm.extension.empty() || !dm.pttId.empty()) out.dispatch.members.push_back(dm);
        });
        Json::each(Json::child(d, "pttTargets"), [&](const JVal* t) {
            DispatchTarget dt;
            dt.id = Json::str(t, "id"); dt.uri = Json::str(t, "uri"); dt.name = Json::str(t, "name");
            if (dt.id.empty() && !dt.uri.empty()) {           // id 생략 시 uri user part
                size_t c = dt.uri.find(':'), a = dt.uri.find('@');
                dt.id = dt.uri.substr(c == std::string::npos ? 0 : c + 1, a == std::string::npos ? std::string::npos : a - (c == std::string::npos ? 0 : c + 1));
            }
            if (!dt.id.empty()) out.dispatch.pttTargets.push_back(dt);
        });
    }
    // 그룹 생성 자격 — 최상위 ptt 블록(정본) 또는 ptt 서비스 항목(호환).
    out.allowGroupCreation = Json::boolean(Json::child(j.root, "ptt"), "allowCreateGroup", false);
    if (!out.allowGroupCreation)
        Json::each(Json::child(j.root, "services"), [&](const JVal* s) {
            if (Json::str(s, "kind") == "ptt" && Json::boolean(s, "allowCreateGroup", false)) out.allowGroupCreation = true;
        });
    return true;
}

Result CscClient::fetchProfile(const std::string& accessToken, Profile& out) {
    http::Response r = impl_->tp->request("GET", impl_->ep.baseUrl() + "/provisioning/me",
                                          {{"Authorization", "Bearer " + accessToken}}, "");
    if (r.status / 100 != 2) return httpFail(r, "provisioning/me");
    std::string err;
    if (!parseProfile(r.body, out, &err)) return Result::fail(-2, "provisioning/me: " + err);
    return Result::success();
}

Result CscClient::listGroups(const std::string& accessToken, const std::string& userUri, std::vector<GroupSummary>& out) {
    http::Response r = impl_->tp->request("GET", impl_->ep.baseUrl() + "/org.openmobilealliance.groups/users/" + enc(userUri),
                                          {{"Authorization", "Bearer " + accessToken}}, "");
    if (r.status / 100 != 2) return httpFail(r, "listGroups");
    Json j(r.body);
    if (!j.root) return Result::fail(-2, "listGroups: bad json");
    out.clear();
    Json::each(j.root, [&](const JVal* g) {
        GroupSummary s; s.uri = Json::str(g, "uri"); s.displayName = Json::str(g, "display_name"); s.etag = Json::str(g, "etag");
        s.memberCount = Json::num(g, "member_count", -1);
        s.isOwner = Json::boolean(g, "is_owner", false);
        out.push_back(s);
    });
    return Result::success();
}

Result CscClient::getGroup(const std::string& accessToken, const std::string& userUri, const std::string& groupUri, GroupDoc& out) {
    XcapDoc doc;
    Result r = xcapGet(accessToken, groupPath(userUri, groupUri), kCtGroupDoc, "", doc);
    if (!r.ok) return r;
    std::string err;
    GroupDoc d; d.etag = doc.etag;
    if (!GroupDoc::parse(doc.body, d, &err)) return Result::fail(-2, "group doc: " + err);
    out = d;
    return Result::success();
}

Result CscClient::putGroup(const std::string& accessToken, const std::string& userUri, const GroupDoc& doc, const std::string& ifMatch, GroupDoc& out) {
    if (doc.uri.empty()) return Result::fail(-2, "group uri required");
    std::map<std::string, std::string> h{{"Authorization", "Bearer " + accessToken}, {"Content-Type", kCtGroupDoc}, {"Accept", kCtGroupDoc}};
    if (!ifMatch.empty()) h["If-Match"] = ifMatch;
    http::Response r = impl_->tp->request("PUT", impl_->ep.baseUrl() + groupPath(userUri, doc.uri), h, doc.toXml());
    if (r.status / 100 != 2) return httpFail(r, "putGroup");
    GroupDoc d; d.etag = http::header(r, "etag");
    std::string err;
    if (r.body.empty() || !GroupDoc::parse(r.body, d, &err)) { d = doc; d.etag = http::header(r, "etag"); }   // 본문 없는 2xx — 보낸 문서로
    out = d;
    return Result::success();
}

Result CscClient::deleteGroup(const std::string& accessToken, const std::string& userUri, const std::string& groupUri) {
    http::Response r = impl_->tp->request("DELETE", impl_->ep.baseUrl() + groupPath(userUri, groupUri),
                                          {{"Authorization", "Bearer " + accessToken}}, "");
    if (r.status / 100 != 2) return httpFail(r, "deleteGroup");
    return Result::success();
}

Result CscClient::xcapGet(const std::string& accessToken, const std::string& path, const std::string& accept,
                          const std::string& ifNoneMatch, XcapDoc& out) {
    std::map<std::string, std::string> h{{"Authorization", "Bearer " + accessToken}, {"Accept", accept}};
    if (!ifNoneMatch.empty()) h["If-None-Match"] = ifNoneMatch;
    http::Response r = impl_->tp->request("GET", impl_->ep.baseUrl() + path, h, "");
    if (r.status == 304) { out.notModified = true; out.etag = ifNoneMatch; return Result::success(); }
    if (r.status / 100 != 2) return httpFail(r, "xcap");
    out.body = r.body; out.etag = http::header(r, "etag"); out.notModified = false;
    return Result::success();
}

}  // namespace cimsue
