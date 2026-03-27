

#ifndef _SIP_CLIENT_H_
#define _SIP_CLIENT_H_

#include "SipUserAgent.h"


class CSipClient : public ISipUserAgentCallBack
{
public:
	virtual ~CSipClient(){}
	CSipClient() : m_bPttMode(false) {}
	bool m_bPttMode;

	virtual void EventRegister( CSipServerInfo * pclsInfo, int iStatus );
	virtual void EventIncomingCall( const char * pszCallId, const char * pszFrom, const char * pszTo, CSipCallRtp * pclsRtp );
	virtual void EventCallRing( const char * pszCallId, int iSipStatus, CSipCallRtp * pclsRtp );
	virtual void EventCallStart( const char * pszCallId, CSipCallRtp * pclsRtp );
	virtual void EventCallEnd( const char * pszCallId, int iSipStatus );
	virtual void EventReInvite( const char * pszCallId, CSipCallRtp * pclsRemoteRtp, CSipCallRtp * pclsLocalRtp );
	virtual bool EventTransfer( const char * pszCallId, const char * pszReferToCallId, bool bScreenedTransfer );
	virtual bool EventBlindTransfer( const char * pszCallId, const char * pszReferToId );
	virtual bool EventMessage( const char * pszFrom, const char * pszTo, CSipMessage * pclsMessage );

	CSipCallRtp m_clsDestRtp;
};

#endif
