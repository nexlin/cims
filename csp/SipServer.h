#ifndef _SIP_SERVER_H_
#define _SIP_SERVER_H_

#include "SipUserAgent.h"

class CSipServer : public ISipStackCallBack, ISipUserAgentCallBack, ISipStackSecurityCallBack {
public:
    CSipServer();
    ~CSipServer();

    bool Start( CSipStackSetup &clsSetup );

    bool CheckAuthrization( CSipMessage *pclsMessage );

    // ISipStackCallBack
    virtual bool RecvRequest( int iThreadId, CSipMessage *pclsMessage );
    virtual bool RecvResponse( int iThreadId, CSipMessage *pclsMessage );
    virtual bool SendTimeout( int iThreadId, CSipMessage *pclsMessage );

    // ISipUserAgentCallBack
    virtual void EventRegister( CSipServerInfo *pclsInfo, int iStatus );
    virtual bool EventIncomingRequestAuth( CSipMessage *pclsMessage );
    virtual void EventIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                    CSipCallRtp *pclsRtp, CSipMessage *pclsMessage = NULL );
    virtual void EventCallRing( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRtp );
    virtual void EventCallStart( const char *pszCallId, CSipCallRtp *pclsRtp );
    virtual void EventCallEnd( const char *pszCallId, int iSipStatus );
    virtual void EventReInvite( const char *pszCallId, CSipCallRtp *pclsRemoteRtp, CSipCallRtp *pclsLocalRtp );
    virtual void EventPrack( const char *pszCallId, CSipCallRtp *pclsRtp );
    virtual bool EventTransfer( const char *pszCallId, const char *pszReferToCallId, bool bScreenedTransfer );
    virtual bool EventBlindTransfer( const char *pszCallId, const char *pszReferToId );
    virtual bool EventMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage );

    // ISipStackSecurityCallBack
    virtual bool IsAllowUserAgent( const char *pszSipUserAgent );
    virtual bool IsDenyUserAgent( const char *pszSipUserAgent );
    virtual bool IsAllowIp( const char *pszIp );
    virtual bool IsDenyIp( const char *pszIp );

private:
    CSipMutex m_clsMutex;

    bool SendResponse( CSipMessage *pclsMessage, int iStatusCode );
    void SaveCdr( const char *pszCallId, int iSipStatus );
    void StopCall( const char *pszCallId, int iResponseCode );

    // SipServerRegister.hpp
    bool RecvRequestRegister( int iThreadId, CSipMessage *pclsMessage );

    // SipServerPickUp.hpp
    void PickUp( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp );
};

extern CSipServer gclsSipServer;
extern CSipUserAgent gclsUserAgent;

#endif
