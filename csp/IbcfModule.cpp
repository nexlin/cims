#include "IbcfModule.h"
#include "SipServerSetup.h"

bool CIbcfModule::IsEnabled() const {
    return gclsSetup.m_bRoleIbcf;
}

// Phase 4 에서 IP-PBX 트렁크 라우팅 로직 이동 예정
// 현재는 ModuleDispatcher가 기존 로직을 직접 호출

EModuleRouteResult CIbcfModule::OnIncomingCall(const char* pszCallId, const char* pszFrom,
    const char* pszTo, CSipCallRtp* pclsRtp, CSipMessage* pclsMessage) {
    return E_ROUTE_PASS;
}

bool CIbcfModule::OnCallRing(const char* pszCallId, int iSipStatus, CSipCallRtp* pclsRtp) { return false; }
bool CIbcfModule::OnCallStart(const char* pszCallId, CSipCallRtp* pclsRtp) { return false; }
bool CIbcfModule::OnCallEnd(const char* pszCallId, int iSipStatus) { return false; }
