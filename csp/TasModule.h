#ifndef _TAS_MODULE_H_
#define _TAS_MODULE_H_

#include "IModule.h"

class CTasModule : public IModule {
public:
    const char *GetName() const override {
        return "TAS";
    }
    bool IsEnabled() const override;

    EModuleRouteResult OnIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                       CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) override;
    bool OnCallRing( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRtp ) override;
    bool OnCallStart( const char *pszCallId, CSipCallRtp *pclsRtp ) override;
    bool OnCallEnd( const char *pszCallId, int iSipStatus ) override;
    bool OnReInvite( const char *pszCallId, CSipCallRtp *pclsRemoteRtp, CSipCallRtp *pclsLocalRtp ) override;
    bool OnPrack( const char *pszCallId, CSipCallRtp *pclsRtp ) override;
    bool OnTransfer( const char *pszCallId, const char *pszReferToCallId, bool bScreened ) override;
    bool OnBlindTransfer( const char *pszCallId, const char *pszReferToId ) override;
    bool OnMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage ) override;
};

#endif
