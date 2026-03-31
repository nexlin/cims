#include "SipClient.h"
#include <time.h>
#include "Log.h"
#include "RtpThread.h"
#include "MemoryDebug.h"




void CSipClient::EventRegister( CSipServerInfo * pclsInfo, int iStatus )
{
	CLog::Print( LOG_DEBUG, "%s(%d)", __FUNCTION__, iStatus );
	printf( "EventRegister(%s) : %d\n", pclsInfo->m_strUserId.c_str(), iStatus );
}




void CSipClient::EventIncomingCall( const char * pszCallId, const char * pszFrom, const char * pszTo, CSipCallRtp * pclsRtp, CSipMessage * pclsMessage )
{
	printf( "EventIncomingCall(%s,%s)\n", pszCallId, pszFrom );

	if (m_pInviteId) *m_pInviteId = pszCallId;

	if( pclsRtp )
	{
		printf( "=> RTP(%s:%d) codec(%d)\n", pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort, pclsRtp->m_iCodec );

		m_clsDestRtp = *pclsRtp;
	}

    if (!m_pUserAgent || !m_pRtpThread) return;

    if (m_bPttMode) {
        printf("[PTT] Auto-Answering Call...\n");
        // Immediate 200 OK
        CSipCallRtp clsLocalRtp;
        clsLocalRtp.m_strIp = m_pUserAgent->m_clsSipStack.m_clsSetup.m_strLocalIp;
        clsLocalRtp.m_iPort = m_pRtpThread->m_iPort;
        clsLocalRtp.m_iCodec = 0; // PCMU

        m_pUserAgent->AcceptCall( pszCallId, &clsLocalRtp );

        if( pclsRtp )
        {
            m_pRtpThread->Start( pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort );
        }
    } else {
        // [DELAYED RESPONSE LOGIC for Call Mode]
        // 1. Send 180 Ringing
        m_pUserAgent->RingCall( pszCallId, 180, NULL );
        
        // 2. Sleep 5 seconds
        printf( "Sleeping 5 seconds...\n" );
        sleep( 5 );

        // 3. Send 200 OK
        CSipCallRtp clsLocalRtp;
        clsLocalRtp.m_strIp = m_pUserAgent->m_clsSipStack.m_clsSetup.m_strLocalIp;
        clsLocalRtp.m_iPort = m_pRtpThread->m_iPort;
        clsLocalRtp.m_iCodec = 0; // PCMU

        m_pUserAgent->AcceptCall( pszCallId, &clsLocalRtp );

        if( pclsRtp )
        {
            m_pRtpThread->Start( pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort );
        }
    }
}


void CSipClient::EventCallRing( const char * pszCallId, int iSipStatus, CSipCallRtp * pclsRtp )
{
	printf( "EventCallRing(%s,%d)\n", pszCallId, iSipStatus );

	if( pclsRtp )
	{
		printf( "=> RTP(%s:%d) codec(%d)\n", pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort, pclsRtp->m_iCodec );
	}
}


void CSipClient::EventCallStart( const char * pszCallId, CSipCallRtp * pclsRtp )
{
	printf( "EventCallStart(%s)\n", pszCallId );

	if( pclsRtp && m_pRtpThread )
	{
		printf( "=> RTP(%s:%d) codec(%d)\n", pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort, pclsRtp->m_iCodec );

		m_pRtpThread->Start( pclsRtp->m_strIp.c_str(), pclsRtp->m_iPort );
	}
}


void CSipClient::EventCallEnd( const char * pszCallId, int iSipStatus )
{
	printf( "EventCallEnd(%s,%d)\n", pszCallId, iSipStatus );

	if (m_pRtpThread) m_pRtpThread->Stop( );
}


void CSipClient::EventReInvite( const char * pszCallId, CSipCallRtp * pclsRemoteRtp, CSipCallRtp * pclsLocalRtp )
{
	printf( "EventReInvite(%s)\n", pszCallId );

	if( pclsRemoteRtp )
	{
		printf( "=> RTP(%s:%d) codec(%d)\n", pclsRemoteRtp->m_strIp.c_str(), pclsRemoteRtp->m_iPort, pclsRemoteRtp->m_iCodec );
		if (m_pRtpThread) m_pRtpThread->Start( pclsRemoteRtp->m_strIp.c_str(), pclsRemoteRtp->m_iPort );
	}
}

bool CSipClient::EventTransfer( const char * pszCallId, const char * pszReferToCallId, bool bScreenedTransfer )
{
	printf( "EventTransfer(%s,%s)\n", pszCallId, pszReferToCallId );

	return false;
}

bool CSipClient::EventBlindTransfer( const char * pszCallId, const char * pszReferToId )
{
	printf( "EventBlindTransfer(%s,%s)\n", pszCallId, pszReferToId );

	return false;
}
 



bool CSipClient::EventMessage( const char * pszFrom, const char * pszTo, CSipMessage * pclsMessage )
{
	char	szContentType[255];

	memset( szContentType, 0, sizeof(szContentType) );
	pclsMessage->m_clsContentType.ToString( szContentType, sizeof(szContentType) );

	printf( "EventMessage(%s,%s)\n", pszFrom, pszTo );
	printf( "content-type[%s]\n", szContentType );
	printf( "body[%s]\n", pclsMessage->m_strBody.c_str() );

	return true;
}
