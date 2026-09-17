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
    /** MESSAGE 처리 훅. 반환값 = 이 모듈이 처리했는가.
     *  처리했으면 iStatus 에 psip 가 보낼 최종 응답 코드를 담는다 — 0 이면 모듈이 응답을
     *  이미 보냈다는 뜻이라 psip 는 보내지 않는다(최종 응답 중복 방지). */
    virtual bool OnMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage, int &iStatus ) {
        return false;
    }
};

#endif
