#include "service_auth.h"

#include "csc/xml_scan.h"
#include "mcdata/sds_codec.h"
#include "mcptt/mcptt_xml.h"
#include "mcvideo/mcvideo_sip.h"

namespace cimsue {
namespace detail {

namespace {

constexpr const char* kIcsiMcdata = "urn:urn-7:3gpp-service.ims.icsi.mcdata";   // TS 24.282 Annex E.2.1

/** 서비스 info 문서(TS 24.379 Annex F.1 · TS 24.282 Annex D.1 · TS 24.281 Annex F.1) — 암호화하지 않은 contentType 요소
 *  (type="Normal" + <…String>), 요소 순서 = …-ParamsType sequence(access-token 이 client-id 보다 앞). */
std::string infoDoc(McService s, const std::string& accessToken, const std::string& clientId) {
    const char* pre = s == McService::McVideo ? "mcvideo" : s == McService::McData ? "mcdata" : "mcptt";
    const char* ns = s == McService::McVideo ? mcvideo::kNsInfo : s == McService::McData ? "urn:3gpp:ns:mcdataInfo:1.0"
                                                                                       : mcptt::kNsMcpttInfo;
    auto elem = [pre](const char* name, const std::string& v) {
        return std::string("    <") + pre + "-" + name + " type=\"Normal\"><" + pre + "String>" + xmlscan::esc(v) + "</" + pre +
               "String></" + pre + "-" + name + ">\n";
    };
    std::string d = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    d += std::string("<") + pre + "info xmlns=\"" + ns + "\">\n  <" + pre + "-Params>\n";
    d += elem("access-token", accessToken);
    if (!clientId.empty()) d += elem("client-id", clientId);
    d += std::string("  </") + pre + "-Params>\n</" + pre + "info>\n";
    return d;
}

const char* infoContentType(McService s) {
    return s == McService::McVideo ? mcvideo::kCtInfo : s == McService::McData ? mcdata::kCtInfo : mcptt::kCtMcpttInfo;
}

}  // namespace

const char* serviceIcsi(McService s) {
    return s == McService::McVideo ? mcvideo::kIcsi : s == McService::McData ? kIcsiMcdata : mcptt::kIcsiMcptt;
}

std::string pocSettings(const std::string& entityId, const std::string& answerMode, int userProfileIndex, int multiplex) {
    // 확장 요소는 XML 스키마(TS 24.379 표 7.4.1.2.2-2 — targetNamespace urn:3gpp:mcsSettings:1.0, qualified)의 이름공간에 둔다.
    std::string d = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    d += "<poc-settings xmlns=\"urn:oma:params:xml:ns:poc:poc-settings\" xmlns:mcs10Set=\"urn:3gpp:mcsSettings:1.0\">\n";
    d += "  <entity id=\"" + xmlscan::esc(entityId) + "\">\n";
    if (!answerMode.empty()) d += "    <am-settings><answer-mode>" + answerMode + "</answer-mode></am-settings>\n";
    d += "    <mcs10Set:selected-user-profile-index><mcs10Set:user-profile-index>" + std::to_string(userProfileIndex) +
         "</mcs10Set:user-profile-index></mcs10Set:selected-user-profile-index>\n";
    if (multiplex >= 0) d += std::string("    <mcs10Set:multiplex-support>") + (multiplex ? "true" : "false") + "</mcs10Set:multiplex-support>\n";
    d += "  </entity>\n</poc-settings>\n";
    return d;
}

ServiceAuthBody serviceAuthBody(McService s, const std::string& accessToken, const std::string& clientId, const std::string& entityId,
                                bool autoAnswer, const std::string& boundary) {
    // §7.2.2 6) — MCPTT·MCVideo: a) Answer-Mode(RFC 4354 am-settings) · b) 선택 user profile · c) multiplex(우리 단말은 다중화하지
    //   않는다 — false). MCData(TS 24.282 §7.2.2 6)) 는 a) 선택 user profile 만.
    const bool mcdata = s == McService::McData;
    const std::string poc = pocSettings(entityId, mcdata ? std::string() : autoAnswer ? "automatic" : "manual", 1, mcdata ? -1 : 0);
    ServiceAuthBody b;
    b.contentType = "multipart/mixed;boundary=" + boundary;
    b.body = "--" + boundary + "\r\nContent-Type: " + infoContentType(s) + "\r\n\r\n" + infoDoc(s, accessToken, clientId) + "\r\n" +
             "--" + boundary + "\r\nContent-Type: " + kCtPocSettings + "\r\n\r\n" + poc + "\r\n" + "--" + boundary + "--\r\n";
    return b;
}

bool multipleDevicesInd(const std::string& body) {
    bool found = false;
    std::string v = xmlscan::elemText(body, "multiple-devices-ind", &found);
    if (!found) return false;
    // contentType 요소면 <…Boolean> 자식(Annex F.1 — 비암호화) — 값만 남긴다
    const size_t gt = v.find('>');
    if (gt != std::string::npos) {
        const size_t lt = v.find('<', gt);
        v = v.substr(gt + 1, lt == std::string::npos ? std::string::npos : lt - gt - 1);
    }
    return xmlscan::isTrue(v);
}

}  // namespace detail
}  // namespace cimsue
