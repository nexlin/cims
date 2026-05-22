#ifndef _IMODULE_H_
#define _IMODULE_H_

#include "SipMessage.h"

class CSipCallRtp;
class CSipCallRoute;

enum EModuleRouteResult { E_ROUTE_HANDLED, E_ROUTE_PASS, E_ROUTE_PROXY };

class IModule {
public:
    virtual ~IModule() {
    }
    virtual const char *GetName() const = 0;
    virtual bool IsEnabled() const = 0;

    virtual bool OnSipRequest( int iThreadId, CSipMessage *pclsMessage ) {
        return false;
    }
    virtual bool OnSipResponse( int iThreadId, CSipMessage *pclsMessage ) {
        return false;
    }

    virtual EModuleRouteResult OnIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                               CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) {
        return E_ROUTE_PASS;
    }
    virtual void OnIncomingRequestAuth( CSipMessage *pclsMessage ) {
    }
    virtual bool OnCallRing( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRtp ) {
        return false;
    }
    virtual bool OnCallStart( const char *pszCallId, CSipCallRtp *pclsRtp ) {
        return false;
    }
    virtual bool OnCallEnd( const char *pszCallId, int iSipStatus ) {
        return false;
    }
    virtual bool OnReInvite( const char *pszCallId, CSipCallRtp *pclsRemoteRtp, CSipCallRtp *pclsLocalRtp ) {
        return false;
    }
    virtual bool OnPrack( const char *pszCallId, CSipCallRtp *pclsRtp ) {
        return false;
    }
    virtual bool OnTransfer( const char *pszCallId, const char *pszReferToCallId, bool bScreened ) {
        return false;
    }
    virtual bool OnBlindTransfer( const char *pszCallId, const char *pszReferToId ) {
        return false;
    }
    virtual bool OnMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage ) {
        return false;
    }
};

#endif
