#ifndef _IBCF_MODULE_H_
#define _IBCF_MODULE_H_

#include "IModule.h"

class CIbcfModule : public IModule {
public:
    const char* GetName() const override {
        return "IBCF";
    }
    bool IsEnabled() const override;

    EModuleRouteResult OnIncomingCall( const char* pszCallId, const char* pszFrom, const char* pszTo,
                                       CSipCallRtp* pclsRtp, CSipMessage* pclsMessage ) override;
    bool OnCallRing( const char* pszCallId, int iSipStatus, CSipCallRtp* pclsRtp ) override;
    bool OnCallStart( const char* pszCallId, CSipCallRtp* pclsRtp ) override;
    bool OnCallEnd( const char* pszCallId, int iSipStatus ) override;
};

#endif
