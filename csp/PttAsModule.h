#ifndef _PTT_AS_MODULE_H_
#define _PTT_AS_MODULE_H_

#include "IModule.h"

class CPttAsModule : public IModule {
public:
    const char* GetName() const override {
        return "PTT-AS";
    }
    bool IsEnabled() const override;

    EModuleRouteResult OnIncomingCall( const char* pszCallId, const char* pszFrom, const char* pszTo,
                                       CSipCallRtp* pclsRtp, CSipMessage* pclsMessage ) override;
    bool OnCallStart( const char* pszCallId, CSipCallRtp* pclsRtp ) override;
    bool OnCallEnd( const char* pszCallId, int iSipStatus ) override;
};

#endif
