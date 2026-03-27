#include "SipStackThread.h"
#include "ServerUtility.h"
#include "Log.h"
#include "MemoryDebug.h"


static bool SipMessageProcess( CSipStack * pclsSipStack, int iThreadId, const char * pszBuf, int iBufLen, const char * pszIp, unsigned short iPort )
{
	CLog::Print( LOG_NETWORK, "UdpRecv(%s:%d) \n[%s]", pszIp, iPort, pszBuf );

	return pclsSipStack->RecvSipMessage( iThreadId, pszBuf, iBufLen, pszIp, iPort, E_SIP_UDP );
}

THREAD_API SipUdpThread( LPVOID lpParameter )
{
	CSipStack * pclsSipStack = (CSipStack *)lpParameter;
	struct pollfd arrPoll[1];
	int		iThreadId, n, iPacketSize;
	char	szPacket[SIP_PACKET_MAX_SIZE], szIp[INET6_ADDRSTRLEN];
	unsigned short sPort;
	bool	bRes;

	pclsSipStack->IncreateUdpThreadCount( iThreadId );
	TcpSetPollIn( arrPoll[0], pclsSipStack->m_hUdpSocket );

	while( pclsSipStack->m_bStopEvent == false )
	{
		pclsSipStack->m_clsUdpRecvMutex.acquire();
		n = poll( arrPoll, 1, 1000 );
		if( n > 0 )
		{
			iPacketSize = sizeof(szPacket);
			bRes = UdpRecv( pclsSipStack->m_hUdpSocket, szPacket, &iPacketSize, szIp, sizeof(szIp), &sPort, pclsSipStack->m_clsSetup.m_bIpv6 );
			pclsSipStack->m_clsUdpRecvMutex.release();

			if( bRes )
			{
				if( iPacketSize < SIP_PACKET_MIN_SIZE || szPacket[0] == '\0' || szPacket[0] == '\r' || szPacket[0] == '\n' ) continue;

				SipMessageProcess( pclsSipStack, iThreadId, szPacket, iPacketSize, szIp, sPort );
			}
		}
		else
		{
			pclsSipStack->m_clsUdpRecvMutex.release();
		}
	}

	pclsSipStack->ThreadEnd( iThreadId );
	pclsSipStack->DecreateUdpThreadCount();

	return 0;
}


bool StartSipUdpThread( CSipStack * pclsSipStack )
{
	for( int i = 0; i < pclsSipStack->m_clsSetup.m_iUdpThreadCount; ++i )
	{
		char	szMsg[101];

		snprintf( szMsg, sizeof(szMsg), "SipUdpThread(%d)", i + 1 );
		if( StartThread( szMsg, SipUdpThread, pclsSipStack ) == false )
		{
			return false;
		}
	}

	return true;
}
