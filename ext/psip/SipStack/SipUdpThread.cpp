#include "SipStackThread.h"
#include "ServerUtility.h"
#include "Log.h"
#include "MemoryDebug.h"


static bool SipMessageProcess( CSipStack * pclsSipStack, int iThreadId, const char * pszBuf, int iBufLen, const char * pszIp, unsigned short iPort )
{
	CLog::Print( LOG_NETWORK, "UdpRecv(%s:%d) \n[%s]", pszIp, iPort, pszBuf );

	return pclsSipStack->RecvSipMessage( iThreadId, pszBuf, iBufLen, pszIp, iPort, E_SIP_UDP );
}

// ─────────────────────────────────────────────────────────────
//  P2: 리스너 단위 스레드.
//    각 UDP 리스너에 대해 N개 스레드를 생성하고, 각 스레드는 해당 리스너의
//    소켓에 대해서만 poll/recv 한다. drain 이 설정되거나 stack stop 시 종료.
// ─────────────────────────────────────────────────────────────

THREAD_API SipUdpListenerThread( LPVOID lpParameter )
{
	CSipStackUdpListener * pListener = (CSipStackUdpListener *)lpParameter;
	CSipStack * pclsSipStack = pListener->m_pclsStack;
	struct pollfd arrPoll[1];
	int		iThreadId, n, iPacketSize;
	char	szPacket[SIP_PACKET_MAX_SIZE], szIp[INET6_ADDRSTRLEN];
	unsigned short sPort;
	bool	bRes;

	pclsSipStack->IncreateUdpThreadCount( iThreadId );
	pListener->m_iActiveThreads.fetch_add(1);

	while( pclsSipStack->m_bStopEvent == false && pListener->m_bDrain == false )
	{
		// 소켓이 이미 닫혔으면 종료
		if( pListener->m_hSocket == INVALID_SOCKET ) break;

		TcpSetPollIn( arrPoll[0], pListener->m_hSocket );
		pclsSipStack->m_clsUdpRecvMutex.acquire();
		n = poll( arrPoll, 1, 1000 );
		if( n > 0 && pListener->m_hSocket != INVALID_SOCKET )
		{
			iPacketSize = sizeof(szPacket);
			bRes = UdpRecv( pListener->m_hSocket, szPacket, &iPacketSize, szIp, sizeof(szIp), &sPort, pListener->m_bIpv6 );
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
	pListener->m_iActiveThreads.fetch_sub(1);

	return 0;
}


bool StartSipUdpThreadForListener( CSipStack * pclsSipStack, CSipStackUdpListener * pListener, int iCount )
{
	if( iCount < 0 ) iCount = pclsSipStack->m_clsSetup.m_iUdpThreadCount;
	if( iCount <= 0 ) iCount = 1;

	for( int i = 0; i < iCount; ++i )
	{
		char szMsg[101];
		snprintf( szMsg, sizeof(szMsg), "SipUdpThread(id=%d,%d)", pListener->m_iId, i + 1 );
		if( StartThread( szMsg, SipUdpListenerThread, pListener ) == false )
		{
			return false;
		}
	}
	return true;
}

// ─────────────────────────────────────────────────────────────
//  기존 진입점 — Start() 초기화 경로에서 호출됨.
//  첫 리스너(m_vecUdpListeners[0]) 에 대해 setup 의 thread count 만큼 기동.
// ─────────────────────────────────────────────────────────────

bool StartSipUdpThread( CSipStack * pclsSipStack )
{
	// Start() 에서 AddUdpListener 로 기본 리스너가 생성되었을 것
	std::vector<CSipStackUdpListener*> listeners;
	pclsSipStack->GetUdpListenerInfo( listeners );
	if( listeners.empty() ) return false;

	// Start 경로에서는 첫 리스너만 여기서 thread 기동 (이후 추가되는 리스너는
	// AddUdpListener 가 자체적으로 기동한다)
	return StartSipUdpThreadForListener( pclsSipStack, listeners[0], pclsSipStack->m_clsSetup.m_iUdpThreadCount );
}
