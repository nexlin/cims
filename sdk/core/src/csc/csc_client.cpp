#include <ctime>
#include <mutex>
#include "cimsue/csc.h"

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "http/https_client.h"
#include "util/json_lite.h"
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

using namespace jsonlite;

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
    a.maxSdsCplaneBytes = maxPayloadSdsCplaneBytes;                       // 넘는 그룹 SDS 는 media plane(TS 24.282 §9.2.3)
    return a;
}

const ServiceProfile* Profile::service(const std::string& kind) const {
    for (auto& s : services) if (s.kind == kind) return &s;
    return nullptr;
}

const ServiceProfile* Profile::phoneService() const {
    if (const ServiceProfile* s = service("voip")) return s;
    return service("volte");
}

// ── CscClient ──

struct CscClient::Impl {
    CscEndpoint ep;
    std::shared_ptr<http::ITransport> tp;
    mutable std::mutex peerM;
    TlsPeerExpiry peer;                  // 마지막 성공 TLS 요청의 서버 인증서 만료
    /** 모든 전송이 지나는 한 곳 — 응답의 peer 인증서 관측을 갱신한다. */
    http::Response request(const std::string& method, const std::string& url,
                           const std::map<std::string, std::string>& headers, const std::string& body) {
        http::Response r = tp->request(method, url, headers, body);
        if (r.peerNotAfterEpoch > 0) {
            TlsPeerExpiry e;
            e.valid = true; e.notAfterEpoch = r.peerNotAfterEpoch; e.observedEpoch = (int64_t)std::time(nullptr);
            e.subject = r.peerSubject; e.remote = ep.host + ":" + std::to_string(ep.port);
            std::lock_guard<std::mutex> lk(peerM);
            peer = e;
        }
        return r;
    }
};

TlsPeerExpiry CscClient::tlsPeerExpiry() const {
    std::lock_guard<std::mutex> lk(impl_->peerM);
    return impl_->peer;
}

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
    http::Response r = impl_->request("GET", url, {}, "");
    if (r.status / 100 != 2) return httpFail(r, "authreq");
    std::string code;
    { Json j(r.body); if (!j.root) return Result::fail(-2, "authreq: bad json"); code = Json::str(j.root, "code"); }
    if (code.empty()) return Result::fail(-2, "authreq: no code");
    std::string form = "grant_type=authorization_code&code=" + enc(code) + "&client_id=" + enc(impl_->ep.clientId) +
                       "&redirect_uri=" + enc(impl_->ep.redirectUri) + "&code_verifier=" + verifier;
    r = impl_->request("POST", impl_->ep.baseUrl() + "/idms/tokenreq",
                           {{"Content-Type", "application/x-www-form-urlencoded"}}, form);
    if (r.status / 100 != 2) return httpFail(r, "tokenreq");
    if (!parseToken(r.body, out)) return Result::fail(-2, "tokenreq: bad json");
    return Result::success();
}

Result CscClient::refresh(const std::string& refreshToken, TokenSet& out) {
    std::string form = "grant_type=refresh_token&refresh_token=" + enc(refreshToken) + "&client_id=" + enc(impl_->ep.clientId);
    http::Response r = impl_->request("POST", impl_->ep.baseUrl() + "/idms/tokenreq",
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
        sp.udpNoTcpSwitch = Json::boolean(sip, "udpNoTcpSwitch", false);
        Json::each(Json::child(sip, "security"), [&](const JVal* m) {
            if (m->t == JVal::Str) sp.secMechanisms.push_back(m->s);
        });
        const JVal* acc = Json::child(s, "account");
        sp.msisdn = Json::str(acc, "msisdn"); sp.imsi = Json::str(acc, "imsi"); sp.authId = Json::str(acc, "authId");
        sp.sipHa1 = Json::str(acc, "sipHa1"); sp.mcpttId = Json::str(acc, "mcpttId");
        sp.authScheme = Json::str(acc, "authScheme", "digest") == "aka" ? AuthScheme::Aka : AuthScheme::Digest;
        const JVal* aka = Json::child(acc, "aka");
        sp.akaK = Json::str(aka, "k"); sp.akaOpc = Json::str(aka, "opc"); sp.akaAmf = Json::str(aka, "amf", "8000");
        sp.smsGateway = Json::boolean(Json::child(s, "capabilities"), "smsGateway", false);
        // 서버는 PTT 서비스의 `mcdata` 블록에 싣는다(android_ue_provisioning.md §3) — 서비스 최상위 값은 옛 형식 폴백
        sp.maxPayloadSdsCplaneBytes = Json::num(Json::child(s, "mcdata"), "maxPayloadSdsCplaneBytes",
                                                Json::num(s, "maxPayloadSdsCplaneBytes", 0));
        out.services.push_back(sp);
    });
    const JVal* d = Json::child(j.root, "dispatch");
    out.dispatch = DispatchProfile{};
    if (d) {
        out.dispatch.present = true;
        out.dispatch.groupId = Json::str(d, "groupId"); out.dispatch.groupName = Json::str(d, "groupName");
        out.dispatch.pilotId = Json::str(d, "pilotId"); out.dispatch.monitorScope = Json::str(d, "monitorScope", "none");
        out.dispatch.pttListen = Json::str(d, "pttListen", "none"); out.dispatch.listenVisibility = Json::str(d, "listenVisibility", "hidden");
        out.dispatch.directoryAdmin = Json::str(d, "directoryAdmin", "none"); out.dispatch.orgCode = Json::str(d, "orgCode");
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
    http::Response r = impl_->request("GET", impl_->ep.baseUrl() + "/provisioning/me",
                                          {{"Authorization", "Bearer " + accessToken}}, "");
    if (r.status / 100 != 2) return httpFail(r, "provisioning/me");
    std::string err;
    if (!parseProfile(r.body, out, &err)) return Result::fail(-2, "provisioning/me: " + err);
    return Result::success();
}

Result CscClient::listGroups(const std::string& accessToken, const std::string& userUri, std::vector<GroupSummary>& out) {
    http::Response r = impl_->request("GET", impl_->ep.baseUrl() + "/org.openmobilealliance.groups/users/" + enc(userUri),
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

Result CscClient::fetchUserProfile(const std::string& accessToken, const std::string& userUri, const std::string& etag,
                                   UserProfileDoc& out) {
    XcapDoc doc;
    Result r = getUserProfile(accessToken, userUri, etag, doc);
    if (!r.ok) return r;
    if (doc.notModified) { out.notModified = true; return Result::success(); }
    std::string err;
    UserProfileDoc d; d.etag = doc.etag;
    if (!UserProfileDoc::parse(doc.body, d, &err)) return Result::fail(-2, "user-profile: " + err);
    out = d;
    return Result::success();
}

Result CscClient::fetchServiceConfig(const std::string& accessToken, const std::string& userUri, const std::string& etag,
                                     ServiceConfigDoc& out) {
    XcapDoc doc;
    Result r = getServiceConfig(accessToken, userUri, etag, doc);
    if (!r.ok) return r;
    if (doc.notModified) { out.notModified = true; return Result::success(); }
    std::string err;
    ServiceConfigDoc d; d.etag = doc.etag;
    if (!ServiceConfigDoc::parse(doc.body, d, &err)) return Result::fail(-2, "service-config: " + err);
    out = d;
    return Result::success();
}

Result CscClient::fetchMcVideoUserProfile(const std::string& accessToken, const std::string& mcvideoId, const std::string& etag,
                                          McVideoUserProfileDoc& out) {
    if (mcvideoId.empty()) return Result::fail(-2, "mcvideo user-profile: empty MCVideo ID");
    XcapDoc doc;
    Result r = xcapGet(accessToken, mcvideoUserProfilePath(mcvideoId), kCtMcVideoUserProfile, etag, doc);
    if (!r.ok) return r;                                   // 404 = MCVideo 이용 자격 없음
    if (doc.notModified) { out.notModified = true; return Result::success(); }
    std::string err;
    McVideoUserProfileDoc d; d.etag = doc.etag;
    if (!McVideoUserProfileDoc::parse(doc.body, d, &err)) return Result::fail(-2, "mcvideo user-profile: " + err);
    out = d;
    return Result::success();
}

Result CscClient::fetchMcVideoServiceConfig(const std::string& accessToken, const std::string& etag, McVideoServiceConfigDoc& out) {
    XcapDoc doc;
    Result r = xcapGet(accessToken, mcvideoServiceConfigPath(), kCtMcVideoServiceConfig, etag, doc);
    if (!r.ok) return r;
    if (doc.notModified) { out.notModified = true; return Result::success(); }
    std::string err;
    McVideoServiceConfigDoc d; d.etag = doc.etag;
    if (!McVideoServiceConfigDoc::parse(doc.body, d, &err)) return Result::fail(-2, "mcvideo service-config: " + err);
    out = d;
    return Result::success();
}

Result CscClient::fetchUeInitConfig(const std::string& mcsUeId, const std::string& etag, UeInitConfigDoc& out) {
    if (mcsUeId.empty()) return Result::fail(-2, "ue-init-config: empty MCS UE ID");
    // 로그인 전 문서(TS 24.484 §7.2.1.1) — Authorization 없이
    std::map<std::string, std::string> h{{"Accept", kCtUeInitConfig}};
    if (!etag.empty()) h["If-None-Match"] = etag;
    http::Response r = impl_->request("GET", impl_->ep.baseUrl() + ueInitConfigPath(mcsUeId), h, "");
    if (r.status == 304) { out.notModified = true; return Result::success(); }
    if (r.status / 100 != 2) return httpFail(r, "ue-init-config");
    std::string err;
    UeInitConfigDoc d; d.etag = http::header(r, "etag");
    if (!UeInitConfigDoc::parse(r.body, d, &err)) return Result::fail(-2, "ue-init-config: " + err);
    out = d;
    return Result::success();
}

Result CscClient::putGroup(const std::string& accessToken, const std::string& userUri, const GroupDoc& doc, const std::string& ifMatch, GroupDoc& out) {
    if (doc.uri.empty()) return Result::fail(-2, "group uri required");
    std::map<std::string, std::string> h{{"Authorization", "Bearer " + accessToken}, {"Content-Type", kCtGroupDoc}, {"Accept", kCtGroupDoc}};
    if (!ifMatch.empty()) h["If-Match"] = ifMatch;
    http::Response r = impl_->request("PUT", impl_->ep.baseUrl() + groupPath(userUri, doc.uri), h, doc.toXml());
    if (r.status / 100 != 2) return httpFail(r, "putGroup");
    GroupDoc d; d.etag = http::header(r, "etag");
    std::string err;
    if (r.body.empty() || !GroupDoc::parse(r.body, d, &err)) { d = doc; d.etag = http::header(r, "etag"); }   // 본문 없는 2xx — 보낸 문서로
    out = d;
    return Result::success();
}

Result CscClient::deleteGroup(const std::string& accessToken, const std::string& userUri, const std::string& groupUri) {
    http::Response r = impl_->request("DELETE", impl_->ep.baseUrl() + groupPath(userUri, groupUri),
                                          {{"Authorization", "Bearer " + accessToken}}, "");
    if (r.status / 100 != 2) return httpFail(r, "deleteGroup");
    return Result::success();
}

Result CscClient::request(const std::string& accessToken, const std::string& method, const std::string& path,
                          const std::string& contentType, const std::string& body, const std::string& accept,
                          const std::string& ifMatch, const std::string& ifNoneMatch, HttpResult& out) {
    std::map<std::string, std::string> h{{"Authorization", "Bearer " + accessToken}};
    h["Accept"] = accept.empty() ? "*/*" : accept;
    if (!body.empty()) h["Content-Type"] = contentType.empty() ? "application/json" : contentType;
    if (!ifMatch.empty()) h["If-Match"] = ifMatch;
    if (!ifNoneMatch.empty()) h["If-None-Match"] = ifNoneMatch;
    http::Response r = impl_->request(method.empty() ? "GET" : method, impl_->ep.baseUrl() + path, h, body);
    out.status = r.status;
    out.contentType = http::header(r, "content-type");
    out.etag = http::header(r, "etag");
    out.body = r.body;
    if (r.status == 0) return Result::fail(-1, "request: " + r.error);
    if (r.status / 100 == 2 || r.status == 304) return Result::success();
    return httpFail(r, "request");
}

Result CscClient::uploadFd(const std::string& accessToken, const std::string& data, const std::string& name,
                           const std::string& mime, const std::string& groupId, FdUpload& out) {
    if (data.empty()) return Result::fail(-2, "uploadFd: empty file");
    std::string path = "/mcdata/fd?name=" + enc(name.empty() ? "file.bin" : name) +
                       "&type=" + enc(mime.empty() ? "application/octet-stream" : mime);
    if (!groupId.empty()) path += "&group=" + enc(groupId);
    http::Response r = impl_->request("POST", impl_->ep.baseUrl() + path,
                                      {{"Authorization", "Bearer " + accessToken}, {"Content-Type", "application/octet-stream"},
                                       {"Accept", "application/json"}}, data);
    if (r.status / 100 != 2) return httpFail(r, "uploadFd");
    Json j(r.body);
    if (!j.root) return Result::fail(-2, "uploadFd: bad json");
    FdUpload u;
    u.id = Json::str(j.root, "id"); u.url = Json::str(j.root, "url"); u.name = Json::str(j.root, "name", name);
    u.size = Json::num(j.root, "size", (int)data.size());
    if (u.url.empty()) return Result::fail(-2, "uploadFd: no url");
    // 서버가 Host 헤더 없이 상대 경로를 주면 이 CSC 의 절대 URL 로 — 받는 쪽이 FILEURL 을 그대로 쓴다.
    if (u.url[0] == '/') u.url = impl_->ep.baseUrl() + u.url;
    out = u;
    return Result::success();
}

std::string CscClient::fdPathOf(const std::string& url) {
    std::string path = url;
    size_t s = url.find("://");
    if (s != std::string::npos) {
        size_t p = url.find('/', s + 3);
        path = p == std::string::npos ? std::string() : url.substr(p);
    }
    const std::string prefix = "/mcdata/fd/";
    if (path.compare(0, prefix.size(), prefix) != 0) return std::string();
    std::string id = path.substr(prefix.size());
    size_t q = id.find_first_of("?#/");
    if (q != std::string::npos) id = id.substr(0, q);
    if (id.empty()) return std::string();
    for (char c : id) if (!std::isxdigit((unsigned char)c)) return std::string();
    return prefix + id;
}

Result CscClient::downloadFd(const std::string& accessToken, const std::string& url, HttpResult& out) {
    std::string path = fdPathOf(url);
    if (path.empty()) return Result::fail(-2, "downloadFd: not a FD url");
    return request(accessToken, "GET", path, "", "", "*/*", "", "", out);
}

Result CscClient::xcapGet(const std::string& accessToken, const std::string& path, const std::string& accept,
                          const std::string& ifNoneMatch, XcapDoc& out) {
    std::map<std::string, std::string> h{{"Authorization", "Bearer " + accessToken}, {"Accept", accept}};
    if (!ifNoneMatch.empty()) h["If-None-Match"] = ifNoneMatch;
    http::Response r = impl_->request("GET", impl_->ep.baseUrl() + path, h, "");
    if (r.status == 304) { out.notModified = true; out.etag = ifNoneMatch; return Result::success(); }
    if (r.status / 100 != 2) return httpFail(r, "xcap");
    out.body = r.body; out.etag = http::header(r, "etag"); out.notModified = false;
    return Result::success();
}

}  // namespace cimsue
