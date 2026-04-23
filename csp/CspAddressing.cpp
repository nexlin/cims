#include "CspAddressing.h"
#include "SipServerSetup.h"

namespace CspAddressing {

std::string GetLocalSipAddress() {
    // R5.a: 단일 LocalIp 반환. R5.b 에서 per-route / per-listener 로 확장.
    return gclsSetup.m_strLocalIp;
}

std::string GetLocalRtpAddress() {
    // R5.a: 단일 LocalIp 반환. CMP 가 별도 호스트 배포되면 별도 설정으로 분리 예정.
    return gclsSetup.m_strLocalIp;
}

std::string GetLocalXcapAddress() {
    // R5.a: 단일 LocalIp 반환. R6 에서 access_services.server_identity_uri 로 확장.
    return gclsSetup.m_strLocalIp;
}

} // namespace CspAddressing
