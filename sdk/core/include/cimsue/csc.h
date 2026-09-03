// libcimsue — CSC 설정 평면 클라이언트 (ue_sdk.md §4.1 csc·§4.4). IdMS OAuth2 PKCE(TS 33.180) · 자동 프로비저닝
// `/provisioning/me`(android_ue_provisioning.md §3, dispatch 블록 = dispatch_center.md §8.4) · GMS/CMS XCAP(TS 24.481/24.484).
// Engine 과 독립(pjsua2 비의존)·동기 호출. 전송은 http::ITransport 로 주입 가능(기본 OpenSSL).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cimsue/export.h"
#include "cimsue/types.h"

namespace cimsue {

namespace http { class ITransport; }

struct CscEndpoint {
    std::string host;
    int port = 4430;
    std::string clientId = "MCPTT_UE";
    std::string redirectUri = "https://localhost/callback";
    std::string scope = "openid cims:provisioning 3gpp:mcptt:ptt_server";
    std::string caPem;                    // 신뢰 앵커(비면 시스템 기본)
    bool verifyServer = true;
    std::string baseUrl() const { return "https://" + host + ":" + std::to_string(port); }
};

struct TokenSet {
    std::string accessToken, tokenType = "Bearer", refreshToken, idToken, scope;
    int expiresInSec = 3600;
};

/** 프로비저닝 프로파일의 서비스 1개 → AccountConfig 로 변환 가능(toAccount). */
struct ServiceProfile {
    std::string kind;                     // volte | ptt
    std::string sipHost; int sipPort = 5060; Transport transport = Transport::UDP;
    struct Endpoint { Transport transport; int port; };
    std::vector<Endpoint> transports;
    bool enforced = false;
    MediaSecurity mediaSecurity = MediaSecurity::Off;
    std::string domain, msisdn, imsi, authId, sipHa1, mcpttId;
    AuthScheme authScheme = AuthScheme::Digest;
    std::string akaK, akaOpc, akaAmf = "8000";
    std::vector<std::string> secMechanisms;
    int maxPayloadSdsCplaneBytes = 0;
    /** 이 서비스로 등록할 AccountConfig — 프로파일 값 그대로(loginPw 는 sipHa1 부재 시 평문 폴백). */
    AccountConfig toAccount(const std::string& loginPw = std::string()) const;
};

/** 관제 데스크(dispatch_center.md §8.4) — 없으면 present=false. */
struct DispatchProfile {
    bool present = false;
    std::string groupId, groupName, pilotId;
    std::string monitorScope = "none";    // none|own|listed|all
    std::string pttListen = "none";
    std::string listenVisibility = "hidden";
};

struct Profile {
    std::string displayName, loginId, countryCode;
    std::string cscHost; int cscPort = 4430;
    std::vector<ServiceProfile> services;
    DispatchProfile dispatch;
    const ServiceProfile* service(const std::string& kind) const;
};

struct GroupSummary { std::string uri, displayName, etag; int memberCount = -1; };
struct XcapDoc { std::string body, etag; bool notModified = false; };

class CIMSUE_API CscClient {
public:
    explicit CscClient(const CscEndpoint& ep, std::shared_ptr<http::ITransport> transport = nullptr);
    ~CscClient();

    /** IdMS PKCE(S256) 로그인 → 토큰. */
    Result login(const std::string& userName, const std::string& password, TokenSet& out);
    Result refresh(const std::string& refreshToken, TokenSet& out);
    /** GET /provisioning/me */
    Result fetchProfile(const std::string& accessToken, Profile& out);
    /** GMS 그룹 목록 (userUri 예 tel:+8250...). */
    Result listGroups(const std::string& accessToken, const std::string& userUri, std::vector<GroupSummary>& out);
    /** XCAP GET(GMS 그룹 문서·CMS user-profile/service-config) — ifNoneMatch 로 304 캐시. */
    Result xcapGet(const std::string& accessToken, const std::string& path, const std::string& accept,
                   const std::string& ifNoneMatch, XcapDoc& out);
    Result getUserProfile(const std::string& accessToken, const std::string& userUri, const std::string& etag, XcapDoc& out) {
        return xcapGet(accessToken, "/org.3gpp.mcptt.user-profile/users/" + enc(userUri) + "/user-profile",
                       "application/vnd.3gpp.mcptt-user-profile+xml", etag, out);
    }
    Result getServiceConfig(const std::string& accessToken, const std::string& userUri, const std::string& etag, XcapDoc& out) {
        return xcapGet(accessToken, "/org.3gpp.mcptt.service-config/users/" + enc(userUri) + "/service-config",
                       "application/vnd.3gpp.mcptt-service-config+xml", etag, out);
    }

    /** /provisioning/me 응답 JSON → Profile (시험용 공개). */
    static bool parseProfile(const std::string& json, Profile& out, std::string* err = nullptr);
    static std::string enc(const std::string& s);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cimsue
