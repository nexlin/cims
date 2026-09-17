#include "SipStackThread.h"
#include "ServerUtility.h"
#include "StunDefine.h"
#include "StunMessage.h"
#include "Log.h"
#include "MemoryDebug.h"

/** 수신 스레드별로 현재 처리 중인 UDP listener id 를 저장.
 *  CSP 의 CscfModule 등이 inbound 정책 판정 시 조회한다. 처리 완료 후 0 으로 리셋. */
thread_local int t_iCurrentListenerId = 0;


/** CR/LF 로만 이루어진 짧은 패킷 = NAT 바인딩 유지용 keepalive (RFC 5626 §4.4.1).
 *  ping 은 CRLF 2개(4바이트), pong 은 CRLF 1개(2바이트)다. 스택이 poll 을 깨우려고
 *  자기 자신에게 쏘는 패킷도 CRLF 1개이므로, pong 을 2개짜리에만 보내면 서로 되받아
 *  치는 고리가 생기지 않는다. */
static bool IsCrLfKeepAlive( const char * pszBuf, int iLen )
{
	if( iLen < 1 || iLen > 4 ) return false;

	for( int i = 0; i < iLen; ++i )
	{
		if( pszBuf[i] != '\r' && pszBuf[i] != '\n' ) return false;
	}

	return true;
}

/** STUN Binding Request 인가 (RFC 5389 §6). SIP 리스너로 들어오는 keepalive 를 STUN 으로
 *  보내는 단말(RFC 5626 §4.4.2)을 받기 위한 판별이다.
 *  첫 2비트가 0 이어야 하고 매직 쿠키가 있어야 한다 — SIP 요청은 메서드 이름(대문자 ASCII)으로
 *  시작하므로 첫 바이트가 0x40 이상이라 이 조건에 걸리지 않는다. */
static bool IsStunBindingRequest( const char * pszBuf, int iLen )
{
	static const unsigned char arrCookie[4] = { 0x21, 0x12, 0xA4, 0x42 };
	const unsigned char * p = (const unsigned char *)pszBuf;

	if( iLen < STUN_HEADER_SIZE ) return false;
	if( ( p[0] & 0xC0 ) != 0 ) return false;
	if( memcmp( p + 4, arrCookie, 4 ) ) return false;

	// 메시지 종류 = Binding Request (class 0b00, method 0x001)
	return ( ( ( p[0] << 8 ) | p[1] ) == ( STUN_MT_REQUEST | STUN_MT_BINDING ) );
}

/** STUN Binding Request 에 XOR-MAPPED-ADDRESS 로 답한다 (RFC 5389 §15.2).
 *  단말은 이 값을 자기가 등록에 쓴 주소와 대조해 NAT 매핑이 옮겨간 것을 알아채고 재등록한다
 *  (RFC 5626 §4.4.2). 서버는 상태를 남기지 않는다 — 보낸 곳으로 사실만 돌려줄 뿐이다. */
static void StunBindingRespond( Socket hSocket, const char * pszBuf, int iBufLen, const char * pszIp,
	unsigned short sPort )
{
	CStunMessage clsRequest;

	if( clsRequest.Parse( pszBuf, iBufLen ) <= 0 ) return;

	CStunMessage * pclsResponse = clsRequest.CreateResponse( true );
	if( pclsResponse == NULL ) return;

	char szPacket[256];
	int  iLen = 0;

	if( pclsResponse->AddXorMappedAddress( pszIp, sPort ) )
	{
		iLen = pclsResponse->ToString( szPacket, sizeof(szPacket) );
	}

	delete pclsResponse;

	if( iLen > 0 ) UdpSend( hSocket, szPacket, iLen, pszIp, sPort );
}

static bool SipMessageProcess( CSipStack * pclsSipStack, int iThreadId, const char * pszBuf, int iBufLen, const char * pszIp, unsigned short iPort )
{
	// 억제 소스(toll-fraud 스캐너 등 상위가 drop 확정한 IP)는 원본 패킷 덤프를 생략.
	//   처리(RecvSipMessage)는 그대로 수행 — 로깅만 건너뛴다.
	if( !CLog::IsNetworkSourceSuppressed( pszIp ) )
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
		// per-listener mutex — 다른 리스너의 recv 스레드를 차단하지 않음
		pListener->m_clsRecvMutex.acquire();
		n = poll( arrPoll, 1, 1000 );
		if( n > 0 && pListener->m_hSocket != INVALID_SOCKET )
		{
			iPacketSize = sizeof(szPacket);
			bRes = UdpRecv( pListener->m_hSocket, szPacket, &iPacketSize, szIp, sizeof(szIp), &sPort, pListener->m_bIpv6 );
			pListener->m_clsRecvMutex.release();

			if( bRes )
			{
				// SIP 메시지가 될 수 없는 짧은 패킷. 종전에는 조용히 버렸으나, keepalive 는
				//   UDP 도달 경로의 생존을 알리는 유일한 신호라 응용에 올린다.
				if( iPacketSize < SIP_PACKET_MIN_SIZE )
				{
					if( IsCrLfKeepAlive( szPacket, iPacketSize ) )
					{
						if( iPacketSize >= 4 ) UdpSend( pListener->m_hSocket, "\r\n", 2, szIp, sPort );
						pclsSipStack->EventKeepAlive( szIp, sPort, E_SIP_UDP );
					}
					else if( IsStunBindingRequest( szPacket, iPacketSize ) )
					{
						StunBindingRespond( pListener->m_hSocket, szPacket, iPacketSize, szIp, sPort );
						pclsSipStack->EventKeepAlive( szIp, sPort, E_SIP_UDP );
					}
					continue;
				}

				if( szPacket[0] == '\0' || szPacket[0] == '\r' || szPacket[0] == '\n' ) continue;

				// P8: 현재 수신 listener id 노출 (CspServiceMap 의 inbound_policy 체크용)
				t_iCurrentListenerId = pListener->m_iId;
				SipMessageProcess( pclsSipStack, iThreadId, szPacket, iPacketSize, szIp, sPort );
				t_iCurrentListenerId = 0;
			}
		}
		else
		{
			pListener->m_clsRecvMutex.release();
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
