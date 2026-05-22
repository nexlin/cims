#include "TasModule.h"

#include "SipServerSetup.h"

bool CTasModule::IsEnabled() const {
    return gclsSetup.m_bRoleTas;
}

// Phase 5 에서 SipServerUserAgent.hpp 의 VoIP 호 처리 로직 이동 예정
// 현재는 ModuleDispatcher가 기존 로직을 직접 호출

EModuleRouteResult CTasModule::OnIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                               CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) {
    return E_ROUTE_PASS;
}

bool CTasModule::OnCallRing( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRtp ) {
    return false;
}
bool CTasModule::OnCallStart( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    return false;
}
bool CTasModule::OnCallEnd( const char *pszCallId, int iSipStatus ) {
    return false;
}
bool CTasModule::OnReInvite( const char *pszCallId, CSipCallRtp *pclsRemoteRtp, CSipCallRtp *pclsLocalRtp ) {
    return false;
}
bool CTasModule::OnPrack( const char *pszCallId, CSipCallRtp *pclsRtp ) {
    return false;
}
bool CTasModule::OnTransfer( const char *pszCallId, const char *pszReferToCallId, bool bScreened ) {
    return false;
}
bool CTasModule::OnBlindTransfer( const char *pszCallId, const char *pszReferToId ) {
    return false;
}
bool CTasModule::OnMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage ) {
    return false;
}
