#include "PttAsModule.h"
#include "SipServerSetup.h"

bool CPttAsModule::IsEnabled() const {
    return gclsSetup.m_bRolePttAs;
}

// Phase 3 에서 PTT 그룹콜 로직 이동 예정
// 현재는 ModuleDispatcher가 기존 로직을 직접 호출

EModuleRouteResult CPttAsModule::OnIncomingCall(const char* pszCallId, const char* pszFrom,
    const char* pszTo, CSipCallRtp* pclsRtp, CSipMessage* pclsMessage) {
    return E_ROUTE_PASS;
}

bool CPttAsModule::OnCallStart(const char* pszCallId, CSipCallRtp* pclsRtp) { return false; }
bool CPttAsModule::OnCallEnd(const char* pszCallId, int iSipStatus) { return false; }
