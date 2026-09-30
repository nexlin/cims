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
    // cims:provisioning=부트스트랩(/provisioning/me), 3gpp:mc:*=MC 서비스 8종(TS 33.180 B.4.2.2 — 서버 카탈로그와 정합)
    //   + MCVideo 4종(mcvideo.md §1.2 — 서버는 MCVideo 이용 자격이 있는 사용자에게만 준다, 옛 서버는 모르는 값을 버린다)
    std::string scope = "openid cims:provisioning 3gpp:mc:ptt_service 3gpp:mc:data_service 3gpp:mc:ptt_group_management_service 3gpp:mc:ptt_config_management_service 3gpp:mc:ptt_key_management_service 3gpp:mc:data_group_management_service 3gpp:mc:data_config_management_service 3gpp:mc:data_key_management_service 3gpp:mc:video_service 3gpp:mc:video_group_management_service 3gpp:mc:video_config_management_service 3gpp:mc:video_key_management_service";
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
    std::string kind;                     // volte(이동) | voip(유선) | ptt — 접속환경 클래스(sip_service_model.md §2-9)
    std::string sipHost; int sipPort = 5060; Transport transport = Transport::UDP;
    struct Endpoint { Transport transport; int port; };
    std::vector<Endpoint> transports;
    bool enforced = false;
    MediaSecurity mediaSecurity = MediaSecurity::Off;
    std::string domain, msisdn, imsi, authId, sipHa1, mcpttId;
    AuthScheme authScheme = AuthScheme::Digest;
    std::string akaK, akaOpc, akaAmf = "8000";
    std::vector<std::string> secMechanisms;
    /** UDP→TCP 승격 비활성(`sip.udpNoTcpSwitch`, RFC 3261 §18.1.1) — 통제된 망 전용 사이트 옵션. 엔진 전역 설정(EngineConfig.udpNoTcpSwitch)이라
     *  여러 서비스에 걸칠 때 무엇을 넣을지는 앱이 정한다. 구 서버 응답이면 false. */
    bool udpNoTcpSwitch = false;
    /** 외부망 SMS/LMS 게이트웨이 연결(`capabilities.smsGateway` — IBCF→SMSC TS 24.341 또는 SMPP). 관제 앱의 외부 번호 [문자] 활성 조건.
     *  등록 가입자 간 MESSAGE 는 이 값과 무관하다. 구 서버 응답이면 false. */
    bool smsGateway = false;
    /** 그룹 SDS 시그널링 평면 상한(`mcdata.maxPayloadSdsCplaneBytes`, TS 24.484) — toAccount 가 AccountConfig.maxSdsCplaneBytes 로 옮긴다. */
    int maxPayloadSdsCplaneBytes = 0;
    /** 이 서비스로 등록할 AccountConfig — 프로파일 값 그대로(loginPw 는 sipHa1 부재 시 평문 폴백). */
    CIMSUE_API AccountConfig toAccount(const std::string& loginPw = std::string()) const;
};

/** 관제 그룹원(dispatch 블록 members[]) — dialog 구독·그룹원 상태 띠 대상. */
/** 관제 그룹원(dispatch members[]) — groupId = 그 가입자의 관제 그룹(무소속 ""). 앱의 그룹원 띠 = groupId == dispatch.groupId, 감시 대상 = 전원. */
struct DispatchMember { std::string userId, name, volteAor, pttId, extension, groupId; };
/** 청취 대상 PTT 그룹(dispatch 블록 pttTargets[] — 서버가 ptt_listen 범위를 해석한 결과). */
struct DispatchTarget { std::string id, uri, name; };

/** 관제 데스크(dispatch_center.md §8.4) — 없으면 present=false. members/pttTargets 는 서버가 주지 않으면 빈 배열. */
struct DispatchProfile {
    bool present = false;
    std::string groupId, groupName, pilotId;
    std::string monitorScope = "none";    // none|own|listed|all
    std::string pttListen = "none";
    std::string listenVisibility = "hidden";
    std::string directoryAdmin = "none";  // 관제 앱 조직/구성원/번호·PTT 그룹 관리 범위 none|own|all (dispatch_center.md §3.4)
    std::string orgCode;                  // 관제 그룹 소속 조직 코드 — own 의 루트("" = 없음)
    std::vector<DispatchMember> members;
    std::vector<DispatchTarget> pttTargets;
};

struct Profile {
    std::string displayName, loginId, countryCode;
    std::string cscHost; int cscPort = 4430;
    std::vector<ServiceProfile> services;
    DispatchProfile dispatch;
    /** GMS 그룹 생성 자격(ptt_user_profile.allow_create_group — 프로비저닝 `ptt.allowCreateGroup`). */
    bool allowGroupCreation = false;
    CIMSUE_API const ServiceProfile* service(const std::string& kind) const;
    /** 전화 회선 — 유선 `voip` 우선, 없으면 이동 `volte`(android_ue_provisioning.md §3). 관제 앱의 전화 계정 선택 규칙.
     *  이동 앱은 service("volte") 를 그대로 쓴다. */
    CIMSUE_API const ServiceProfile* phoneService() const;
};

/** GMS 목록 항목. isOwner = 토큰 주체가 authorized user(편집·삭제 가능). */
struct GroupSummary { std::string uri, displayName, etag; int memberCount = -1; bool isOwner = false; };
struct XcapDoc { std::string body, etag; bool notModified = false; };
/** 임의 HTTP 요청 산출(request) — status 는 HTTP 상태(0 = 전송 실패), body 는 바이트 그대로(이진 가능). */
struct HttpResult { int status = 0; std::string contentType, etag, body; };
/** MCData FD 업로드 결과(POST /mcdata/fd 201) — url 이 FD SIGNALLING 의 FILEURL(Engine::sendGroupFd/sendFd 의 FdFile.url). */
struct FdUpload { std::string id, url, name; int64_t size = 0; };

/** 그룹 문서 멤버(list/entry). role = chair | participant (mcpttgi:participant-type). */
struct GroupMember {
    std::string uri, name;
    std::string role = "participant";
    int priority = 5;
    std::string title;                         // 직함 <cims:user-title>(사이트 확장) — 읽기 전용, PUT 에 싣지 않는다(서버가 읽지 않는다)
    bool required = false;                     // 필수 멤버 <mcpttgi:on-network-required>(TS 24.481 §7.2.4.2) — 개시자 응답 전에 이 멤버의
                                               //   200 을 기다린다(TNG1, TS 24.379 §6.3.3.3). 읽은 값을 그대로 되돌려야 콘솔 설정이 남는다
    std::string mcvideoId;                     // <mcpttgi:mcvideo-mcvideo-id uri>(TS 24.481 §7.2.2 MCVideo entry) — MCVideo 그룹의 멤버 MCVideo ID
                                               //   (= MCPTT ID, mcvideo.md §7 D1). 비면 uri 와 같게 본다
};

/** GMS 그룹 문서의 MCVideo 몫(TS 24.481 §7.2.2·§7.2.8) — `<supported-services>` 에 MCVideo ICSI `<service>`(`<mcvideo-video-media/>`)가 있으면
 *  그 그룹은 MCVideo 그룹이다(한 그룹 = 서비스 집합, mcvideo.md §1.3). 정수·시간은 GroupDoc 과 같은 미기재(kUnset = -1) 규약, 삼중값 불리언은
 *  -1 = 미기재 / 0 / 1. */
struct McVideoGroupAttrs {
    bool present = false;                      // MCVideo `<service enabler="urn:urn-7:3gpp-service.ims.icsi.mcvideo">` 가 있다
    /** mcvideo-on-network-invite-members — true = prearranged, false·없음 = chat(§7.2.8, mcvideo.md §7 D5). 호 종류 검사(TS 24.281 §6.3.5.2). */
    bool inviteMembers = false;
    int maxDurationSec = -1;                   // mcvideo-on-network-maximum-duration
    /** mcvideo-protect-media·mcvideo-protect-transmission-control — **요소가 없으면 true**(GMK 보호, §7.2.8). CIMS 는 false 를 명시한다(D7). */
    bool protectMedia = true;
    bool protectTransmissionControl = true;
    std::vector<std::string> audioEncodings;   // mcvideo-preferred-audio-encodings/encoding@name (예 AMR-WB)
    std::vector<std::string> videoEncodings;   // mcvideo-preferred-video-encodings/encoding@name (예 H264)
    std::string videoResolutions;              // mcvideo-preferred-video-resolutions (예 "1280x720,640x480")
    std::string videoFrameRate;                // mcvideo-preferred-video-frame-rate (예 "30,15")
    int urgentRealTimeVideoMode = -1;          // mcvideo-urgent-real-time-video-mode
    int nonUrgentRealTimeVideoMode = -1;       // mcvideo-non-urgent-real-time-video-mode
    int nonRealTimeVideoMode = -1;             // mcvideo-non-real-time-video-mode
    std::string activeRealTimeVideoMode;       // mcvideo-active-real-time-video-mode (예 non-urgent-real-time)
    int maxTransmitters = -1;                  // mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members (동시 송출 상한)
    int minNumberToStart = -1;                 // mcvideo-on-network-minimum-number-to-start
    int groupPriority = -1;                    // mcvideo-on-network-group-priority
    int receptionHangTimerSec = -1;            // on-network-reception-hang-timer (T5, TS 24.581 §11.1.3)
    int allowConferenceState = -1;             // 규칙 mcvideo-on-network-allow-conference-state
    int allowEmergencyCall = -1;               // 규칙 mcvideo-allow-emergency-call
    int allowEmergencyAlert = -1;              // 규칙 mcvideo-allow-emergency-alert
    int allowImminentPerilCall = -1;           // 규칙 mcvideo-allow-imminent-peril-call
};

/** GMS 그룹 문서(OMA list-service + TS 24.481 mcpttgi 확장) — GET 응답·PUT 본문의 단일 모델.
 *  서버가 내는 문서와 같은 요소만 다룬다(mcptt_api.md §2). 모르는 요소는 파싱에서 무시, 직렬화에는 넣지 않는다. */
struct GroupDoc {
    std::string uri, displayName, etag;
    std::vector<GroupMember> members;
    std::string sessionType = "prearranged";   // 그룹 종류 prearranged | chat — 문서의 on-network-invite-members(TS 24.481 §7.2.2 a)
    bool videoEnabled = false;
    bool encryption = false;
    bool emergencyCall = true;                 // allow-MCPTT-emergency-call (imminent-peril 은 서버가 미러)
    bool emergencyAlert = true;
    bool allowSds = true;
    bool allowFd = false;
    bool requireAffiliation = true;
    int priority = 5;                          // on-network-group-priority
    int maxParticipants = 0;                   // on-network-max-participant-count (0 = 미기재)
    std::string orgCode, authorizedUser;       // authorized-user 는 서버 산출(읽기 전용)
    /** MCVideo 몫 — present 면 toXml 이 MCVideo `<service>`·속성·규칙·entry `<mcvideo-mcvideo-id>` 를 함께 낸다. present 가 아니면 싣지 않고,
     *  서버는 MCVideo `<service>` 가 없는 PUT 으로 그 그룹의 MCVideo 설정을 바꾸지 않는다(전환기 — mcvideo.md §5.1). */
    McVideoGroupAttrs mcvideo;

    // ── 그룹 호 타이머 · 참가자 정보 · MCData 크기 한도 (TS 24.481) — **미기재(kUnset)가 기본값**이다.
    //   미기재면 PUT 에 싣지 않고, 서버는 기존값을 유지한다(mcptt_api.md §2 «없는 요소는 갱신 시 기존값 유지»).
    //   기본값을 실제 값으로 두면 폼에서 이 칸을 다루지 않는 앱이 저장할 때마다 콘솔이 정한 값을 그 기본값으로
    //   덮어쓴다 — 그래서 «값이 없음» 을 표현할 수 있어야 한다. 0 은 뜻이 있는 값이다(아래 각 줄).
    static constexpr int kUnset = -1;
    int hangTimerSec = kUnset;                 // on-network-hang-timer (T4 Inactivity) — 0 = 미사용, 서버 상한 3600
    int maxDurationSec = kUnset;               // on-network-maximum-duration (TNG3) — 0 = 무제한, 서버 상한 86400
    int allowConferenceState = kUnset;         // on-network-allow-conference-state — 0 불허 / 1 허용 (§7.2.4.2)
    int maxSdsSize = kUnset;                   // mcdata-on-network-max-data-size-for-SDS (octet) — 0 = 무제한
    int maxAutoRecv = kUnset;                  // mcdata-on-network-max-data-size-auto-recv (octet) — 0 = 무제한
    // 확인 통화 설정(TS 24.481 §7.2.2 s)t)u), TS 24.379 §6.3.3.3·§10.1.1.4.2) — 같은 미기재 규약.
    int minNumberToStart = kUnset;             // on-network-minimum-number-to-start — 개시자 200 OK 전 멤버 200 수(0 = 기다리지 않음)
    int ackTimeoutSec = kUnset;                // on-network-timeout-for-acknowledgement-of-required-members (TNG1, 초)
    std::string ackAction;                     // on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members
                                               //   proceed | abandon (빈 값 = 미기재)
    /** 문서 → XML(PUT 본문). */
    CIMSUE_API std::string toXml() const;
    /** XML → 문서. 실패면 false(err 에 사유). */
    CIMSUE_API static bool parse(const std::string& xml, GroupDoc& out, std::string* err = nullptr);
};
constexpr const char* kCtGroupDoc = "application/vnd.oma.poc.groups+xml";

/** CMS 문서의 대상 항목 — EntryType(TS 24.484 §8.3.2.7): uri = `<uri-entry>`, mode = `entry-info` 속성
 *  (그룹 = DedicatedGroup | UseCurrentlySelectedGroup, 사설 수신자 = UsePreConfigured | LocallyDetermined).
 *  서버는 미결정 모드에도 폴백 uri 를 채운다(mcptt_emergency_modes.md) — 대상 선택 정책은 앱 몫이다. */
struct CmsEntry { std::string uri, mode; };

/** MCPTT user profile(TS 24.484 §8.3.2, CMS XCAP `application/vnd.3gpp.mcptt-user-profile+xml`) — 코어가 해석하는 요소만.
 *  인가는 `<cp:ruleset>` 의 allow-*(RFC 4745 actions)이고 **요소가 없으면 허용**으로 읽는다 — 서버가 최종 판정(403·Floor Deny)하므로
 *  앱은 UX 선차단만 한다(ue_sdk.md §4.2, android_ue_client.md §7). */
struct UserProfileDoc {
    std::string etag;                          // 호출자가 XcapDoc.etag 로 채운다(parse 는 유지)
    bool notModified = false;                  // fetchUserProfile 이 304 를 받았다 — 나머지 필드는 호출자 사본 그대로
    std::string userUri;                       // 루트 XUI-URI
    CmsEntry emergencyGroup;                   // MCPTT-group-call/EmergencyCall/MCPTTGroupInitiation/entry (§8.3.2.1 8e)
    CmsEntry imminentPerilGroup;               // MCPTT-group-call/ImminentPerilCall/MCPTTGroupInitiation/entry
    CmsEntry emergencyAlertGroup;              // MCPTT-group-call/EmergencyAlert/entry
    CmsEntry emergencyPrivateRecipient;        // PrivateCall/EmergencyCall/MCPTTPrivateRecipient/entry (§8.3.2.1 8d)
    std::vector<std::string> groups;           // OnNetwork/MCPTTGroupInfo — 제휴 가능 그룹 URI
    std::vector<std::string> implicitAffiliations;   // OnNetwork/ImplicitAffiliations
    int maxAffiliationsN2 = -1;                // OnNetwork/MaxAffiliationsN2 (-1 = 미기재)
    bool allowPrivateCall = true;              // allow-private-call (§8.3.2.7 — 1:1 통화 인가)
    bool allowEmergencyGroupCall = true;       // allow-emergency-group-call
    bool allowCancelGroupEmergency = true;     // allow-cancel-group-emergency (§8.3.2.1 11)xiv) — 그룹의 진행 중 긴급 해제)
    bool allowImminentPerilCall = true;        // allow-imminent-peril-call
    bool allowCancelImminentPeril = true;      // allow-cancel-imminent-peril (§8.3.2.1 11)xvii) — 진행 중 임박 해제)
    bool allowActivateEmergencyAlert = true;   // allow-activate-emergency-alert
    bool allowCancelEmergencyAlert = true;     // allow-cancel-emergency-alert
    bool allowEmergencyPrivateCall = true;     // allow-emergency-private-call
    bool allowAdhocGroupCall = true;           // anyExt/allow-adhoc-group-call (§8.3.2.1 11)xxxviii)R), Rel-18 — 옛 서버 cims: 별칭도 같은 이름)
    /** XML → 문서. 루트가 mcptt-user-profile 이 아니면 false. */
    CIMSUE_API static bool parse(const std::string& xml, UserProfileDoc& out, std::string* err = nullptr);
};

/** MCPTT service configuration(TS 24.484 §8.4, `application/vnd.3gpp.mcptt-service-config+xml`) — 시스템 전역 문서.
 *  구조 = <service-configuration-info> › <service-configuration-params domain> › <common>·<on-network>. 인가 요소는 없다 —
 *  인가는 user profile ruleset·그룹 문서가 정본이다. 요소가 없으면 빈 값/-1. */
struct ServiceConfigDoc {
    std::string etag;
    bool notModified = false;                  // fetchServiceConfig 이 304 를 받았다
    std::string domain;                        // service-configuration-params@domain
    int numLevelsGroupHierarchy = -1;          // common/broadcast-group/num-levels-group-hierarchy (-1 = 미기재)
    int numLevelsUserHierarchy = -1;           // common/broadcast-group/num-levels-user-hierarchy
    /** on-network *-resource-priority 의 Resource-Priority r-value(RFC 4412 "<namespace>.<priority>", 예 mcpttp.15 —
     *  TS 24.379 §6.2.8.1.15). 비면 미기재 — AccountConfig.rp* 기본값을 그대로 쓴다. */
    std::string rpEmergency;
    std::string rpImminentPeril;
    std::string rpNormal;
    CIMSUE_API static bool parse(const std::string& xml, ServiceConfigDoc& out, std::string* err = nullptr);
};

/** MCS UE initial configuration(TS 24.484 §7.2, `application/vnd.3gpp.mcptt-ue-init-config+xml`) — 로그인 전 부트스트랩 문서.
 *  코어가 쓰는 것은 참여 기능 PSI 둘이다 — `<on-network><anyExt>` 의 `*-Service-Details/Server-URI`(§7.2.2.1 10)·14)).
 *  서비스를 광고하지 않으면 그 요소가 없다(빈 값) — 계정의 해당 PSI 도 비워 둔다(AccountConfig.mcpttServerUri·mcdataServerUri). */
struct UeInitConfigDoc {
    std::string etag;
    bool notModified = false;                  // fetchUeInitConfig 이 304 를 받았다
    std::string domain;                        // mcptt-UE-initial-configuration@domain
    std::string mcpttServerUri;                // MCPTT-Service-Details/Server-URI — 참여 MCPTT 기능 PSI
    std::string mcdataServerUri;               // MCData-Service-Details/Server-URI — 참여 MCData 기능 PSI
    std::string mcvideoServerUri;              // MCVideo-Service-Details/Server-URI — 참여 MCVideo 기능 PSI(AccountConfig.mcvideoServerUri)
    CIMSUE_API static bool parse(const std::string& xml, UeInitConfigDoc& out, std::string* err = nullptr);
};

/** MCVideo user profile(TS 24.484 §9.3, CMS XCAP `application/vnd.3gpp.mcvideo-user-profile+xml`) — 코어가 해석하는 요소만. 문서가 있으면
 *  MCVideo 이용 자격이 있다(없으면 서버 404 — mcvideo.md §5.1). 인가 규약은 UserProfileDoc 과 같다 — ruleset allow-* 는 **요소가 없으면 허용**
 *  (서버가 최종 판정하고 앱은 UX 선차단만). */
struct McVideoUserProfileDoc {
    std::string etag;
    bool notModified = false;                  // fetchMcVideoUserProfile 이 304 를 받았다
    std::string userUri;                       // 루트 XUI-URI
    std::string mcvideoId;                     // Common/MCVideoUserID/uri-entry
    std::vector<std::string> groups;           // OnNetwork/MCVideoGroupInfo/MCVideo-Group-ID — MCVideo 로 affiliate 할 수 있는 그룹
    std::vector<std::string> implicitAffiliations;   // OnNetwork/ImplicitAffiliations
    int maxAffiliationsN2 = -1;                // OnNetwork/MaxAffiliationsN2
    /** OnNetwork/MaxSimultaneousVideoStreams — 동시 수신 스트림 상한(서버 C9, TS 24.581 §11.2.3). 1차 CIMS 값 1(한 m=video 의 여러 SSRC 분리 전). */
    int maxSimultaneousVideoStreams = -1;
    int maxSimultaneousCallsN6 = -1;           // Common/MCVideo-group-call/MaxSimultaneousCallsN6
    CmsEntry emergencyGroup;                   // Common/MCVideo-group-call/EmergencyCall/MCVideoGroupInitiation/entry
    CmsEntry imminentPerilGroup;               // …/ImminentPerilCall/MCVideoGroupInitiation/entry
    CmsEntry emergencyAlertGroup;              // …/EmergencyAlert/entry
    bool allowPrivateCall = true;              // allow-private-call
    bool allowEmergencyGroupCall = true;       // allow-emergency-group-call
    bool allowEmergencyPrivateCall = true;     // allow-emergency-private-call
    bool allowImminentPerilCall = true;        // allow-imminent-peril-call
    bool allowActivateEmergencyAlert = true;   // allow-activate-emergency-alert
    bool allowRevokeTransmit = true;           // allow-revoke-transmit — 다른 송출 회수(TS 24.581 §4.1.1.2)
    bool allowRemoteAmbientViewing = true;     // anyExt/allow-request-remote-initiated-ambient-viewing (TS 24.281 §15)
    bool allowLocalAmbientViewing = true;      // anyExt/allow-request-locally-initiated-ambient-viewing
    bool allowAdhocGroupCall = true;           // anyExt/allow-adhoc-group-call
    /** XML → 문서. 루트가 mcvideo-user-profile 이 아니면 false. */
    CIMSUE_API static bool parse(const std::string& xml, McVideoUserProfileDoc& out, std::string* err = nullptr);
};
constexpr const char* kCtMcVideoUserProfile = "application/vnd.3gpp.mcvideo-user-profile+xml";

/** MCVideo service configuration(TS 24.484 §9.4, `application/vnd.3gpp.mcvideo-service-config+xml`) — 시스템 전역 문서(§9.4.2.9).
 *  코어가 쓰는 것 = 참여자 전송 제어 타이머 T100~T104(`<on-network><anyExt><tc-timers-counters-R14>` — 초, TS 24.581 §11.1.1)·RP·신호 보호.
 *  서버 타이머·카운터(T1~T11·C2~C11)는 전송 제어 서버 몫이라 읽지 않는다. 요소가 없으면 빈 값/-1(참여자는 K5 기본값을 쓴다). */
struct McVideoServiceConfigDoc {
    std::string etag;
    bool notModified = false;                  // fetchMcVideoServiceConfig 이 304 를 받았다
    std::string domain;                        // service-configuration-params@domain
    std::string rpEmergency, rpImminentPeril, rpNormal;   // on-network *-resource-priority("<namespace>.<priority>", 예 mcpttp.15)
    /** on-network/signalling-protection — **요소가 없으면 켜진 것**(TS 24.281 §6.6.2.1·§6.6.3.1). CIMS 는 false 를 명시한다(mcvideo.md §1.6). */
    bool confidentialityProtection = true;
    bool integrityProtection = true;
    int t100Sec = -1, t101Sec = -1, t102Sec = -1, t103Sec = -1, t104Sec = -1;   // tc-timers-counters-R14 T100~T104 (-1 = 미기재)
    CIMSUE_API static bool parse(const std::string& xml, McVideoServiceConfigDoc& out, std::string* err = nullptr);
};
constexpr const char* kCtMcVideoServiceConfig = "application/vnd.3gpp.mcvideo-service-config+xml";
constexpr const char* kCtUeInitConfig = "application/vnd.3gpp.mcptt-ue-init-config+xml";

/** 정책 게이트 스냅샷(ue_sdk.md §4.2) — user profile ruleset 인가. **받지 못한 문서는 허용**으로 둔다(게이트를 걸지 않는다).
 *  UX 선차단(버튼 숨김·안내)용이며 최종 판정은 서버다. service config 는 인가를 담지 않는다(TS 24.484 §8.4) — 받았는지만 기록. */
struct Capabilities {
    bool userProfileKnown = false, serviceConfigKnown = false;
    bool privateCall = true;                   // up.allow-private-call
    bool emergencyGroupCall = true;            // up.allow-emergency-group-call
    /** up.allow-cancel-group-emergency — 그룹 긴급 해제의 인가(TS 24.379 §6.2.8.1.7 은 local policy, 서버 판정 = 개시자 ∨ 이 값,
     *  §6.3.3.1.13.4). 앱은 «내가 올린 조건(McpttCondition.mine)» 과 OR 해서 [긴급 해제] 를 연다. */
    bool cancelGroupEmergency = true;
    bool imminentPerilCall = true;             // up.allow-imminent-peril-call
    bool cancelImminentPeril = true;           // up.allow-cancel-imminent-peril (§6.2.8.1.10 — 개시자 예외 없음)
    bool emergencyPrivateCall = true;          // up.allow-private-call ∧ up.allow-emergency-private-call
    bool emergencyAlert = true;                // up.allow-activate-emergency-alert
    bool cancelEmergencyAlert = true;          // up.allow-cancel-emergency-alert
    bool adhocGroupCall = true;                // up.allow-adhoc-group-call
    int maxAffiliationsN2 = 0;                 // up.MaxAffiliationsN2, 0 = 미지정(N2 는 앱이 경고만 — 강제하지 않는다)
    /** nullptr = 그 문서를 아직 못 받음. */
    CIMSUE_API static Capabilities of(const UserProfileDoc* userProfile, const ServiceConfigDoc* serviceConfig);
};

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
    /** 코어가 모델링하지 않은 CSC 엔드포인트용 범용 요청(Bearer) — 관제 관리 API(/provisioning/directory/*)·녹취
     *  (/provisioning/recordings/*, 이진 응답)·이력 창 조회 등. method = GET|POST|PUT|DELETE, body 는 contentType 과 함께
     *  바이트 그대로 보낸다(비면 본문 없음). ifMatch/ifNoneMatch 는 비면 생략. 2xx·304 = success(out.status 로 구분),
     *  그 밖의 HTTP 상태 = fail(code=status, reason=본문 앞부분)이되 out 은 채워진다(앱이 오류 JSON 을 읽는다). 전송 실패 = -1. */
    Result request(const std::string& accessToken, const std::string& method, const std::string& path,
                   const std::string& contentType, const std::string& body, const std::string& accept,
                   const std::string& ifMatch, const std::string& ifNoneMatch, HttpResult& out);
    // ── MCData FD 콘텐츠 서버(TS 24.282 §10.2 — mcdata_messaging.md §4.5, 토큰 scope 3gpp:mc:data_service) ──
    /** 파일 업로드(octet-stream). groupId 가 비지 않으면 그룹 FD — 서버가 그룹 allow_fd·업로더 멤버십으로 게이트(403, 없는 그룹 404).
     *  비면 1:1. 413 = 서버 상한(McDataFd.MaxBytes) 초과. 실패 code = HTTP 상태(전송 실패 -1). */
    Result uploadFd(const std::string& accessToken, const std::string& data, const std::string& name,
                    const std::string& mime, const std::string& groupId, FdUpload& out);
    /** 파일 다운로드 — url 은 받은 FD 의 FILEURL. 경로(/mcdata/fd/{id})만 취해 **이 CSC** 에 요청한다: Bearer 를 다른 호스트로
     *  보내지 않고, 발신자가 다른 주소(FQDN·다른 노드)로 올렸어도 같은 사이트 저장소에서 받는다. 경로가 FD 가 아니면 fail(-2).
     *  out.body = 파일 바이트, out.contentType = 저장 MIME. */
    Result downloadFd(const std::string& accessToken, const std::string& url, HttpResult& out);
    /** FILEURL → 콘텐츠 서버 경로(/mcdata/fd/{id}). FD 경로가 아니면 빈 문자열(시험용 공개). */
    static std::string fdPathOf(const std::string& url);
    /** HTTPS 서버 인증서 만료 관측 — 마지막 성공 TLS 요청에서 본 CSC 인증서(§8.6.2). 아직 요청이 없으면 valid=false. */
    TlsPeerExpiry tlsPeerExpiry() const;
    Result getUserProfile(const std::string& accessToken, const std::string& userUri, const std::string& etag, XcapDoc& out) {
        return xcapGet(accessToken, "/org.3gpp.mcptt.user-profile/users/" + enc(userUri) + "/user-profile",
                       "application/vnd.3gpp.mcptt-user-profile+xml", etag, out);
    }
    Result getServiceConfig(const std::string& accessToken, const std::string& userUri, const std::string& etag, XcapDoc& out) {
        return xcapGet(accessToken, "/org.3gpp.mcptt.service-config/users/" + enc(userUri) + "/service-config",
                       "application/vnd.3gpp.mcptt-service-config+xml", etag, out);
    }
    /** user-profile GET + 해석(If-None-Match = etag). 304 면 out.notModified=true 만 세우고 나머지는 건드리지 않는다(앱이 가진
     *  사본 유지 — XcapDoc 과 같은 규약). 해석 실패는 fail(-2) — getGroup 과 같다. */
    Result fetchUserProfile(const std::string& accessToken, const std::string& userUri, const std::string& etag,
                            UserProfileDoc& out);
    /** service-config GET + 해석 — fetchUserProfile 과 같은 규약. */
    Result fetchServiceConfig(const std::string& accessToken, const std::string& userUri, const std::string& etag,
                              ServiceConfigDoc& out);
    // ── MCVideo CMS 문서(TS 24.484 §9.3·§9.4 — 토큰 scope 3gpp:mc:video_config_management_service) ──
    /** MCVideo user profile GET + 해석 — `/org.3gpp.mcvideo.user-profile/users/<MCVideo ID>/mcvideo-user-profile-1.xml`(§9.3.1A 문서 이름).
     *  mcvideoId = MCPTT ID 와 같은 값(D1). 404 = MCVideo 이용 자격 없음(fail code 404). 304·해석 실패 규약은 fetchUserProfile 과 같다. */
    Result fetchMcVideoUserProfile(const std::string& accessToken, const std::string& mcvideoId, const std::string& etag,
                                   McVideoUserProfileDoc& out);
    /** MCVideo service configuration GET + 해석 — 전역 문서 `/org.3gpp.mcvideo.service-config/global/mcvideo-service-config.xml`(§9.4.2.9). */
    Result fetchMcVideoServiceConfig(const std::string& accessToken, const std::string& etag, McVideoServiceConfigDoc& out);
    static std::string mcvideoUserProfilePath(const std::string& mcvideoId) {
        return "/org.3gpp.mcvideo.user-profile/users/" + enc(mcvideoId) + "/mcvideo-user-profile-1.xml";
    }
    static std::string mcvideoServiceConfigPath() { return "/org.3gpp.mcvideo.service-config/global/mcvideo-service-config.xml"; }
    /** UE initial configuration GET + 해석 — XCAP URI = `<XCAP root>/org.3gpp.mcptt.ue-init-config/users/sip:<MCS UE ID>/<MCS UE ID>`
     *  (TS 24.484 §7.2.1.1). mcsUeId = 단말 instance ID(AccountConfig.instanceId, 예 urn:uuid:…). 로그인 전 문서라 토큰 없이 부른다.
     *  304·해석 실패 규약은 fetchUserProfile 과 같다. */
    Result fetchUeInitConfig(const std::string& mcsUeId, const std::string& etag, UeInitConfigDoc& out);
    static std::string ueInitConfigPath(const std::string& mcsUeId) {
        return "/org.3gpp.mcptt.ue-init-config/users/" + enc("sip:" + mcsUeId) + "/" + enc(mcsUeId);
    }

    // ── GMS 그룹 관리(TS 24.481 — 그룹 생성·수정·삭제 주체 = authorized user, XCAP PUT/DELETE, PKCE 토큰) ──
    /** 그룹 문서 GET → GroupDoc(etag 포함). userUri 는 자기 XCAP 트리(토큰 mcptt_id). */
    Result getGroup(const std::string& accessToken, const std::string& userUri, const std::string& groupUri, GroupDoc& out);
    /** 그룹 생성(신규 uri)/수정(기존 uri) — PUT 본문 = doc.toXml(). ifMatch 가 비지 않으면 조건부(412 = 충돌).
     *  성공 시 out = 서버가 확정한 문서(etag·authorizedUser 채워짐). 실패 code = HTTP(403 자격/소유, 409 타인 소유, 412). */
    Result putGroup(const std::string& accessToken, const std::string& userUri, const GroupDoc& doc, const std::string& ifMatch, GroupDoc& out);
    /** 그룹 삭제 — 본인 소유만(403). */
    Result deleteGroup(const std::string& accessToken, const std::string& userUri, const std::string& groupUri);
    /** XCAP 그룹 문서 경로(/org.openmobilealliance.groups/users/{user}/{group}). */
    static std::string groupPath(const std::string& userUri, const std::string& groupUri) {
        return "/org.openmobilealliance.groups/users/" + enc(userUri) + "/" + enc(groupUri);
    }

    /** /provisioning/me 응답 JSON → Profile (시험용 공개). */
    static bool parseProfile(const std::string& json, Profile& out, std::string* err = nullptr);
    static std::string enc(const std::string& s);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cimsue
