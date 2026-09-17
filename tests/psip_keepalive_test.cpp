// psip UDP keepalive 수신 하네스 — NAT 바인딩 유지용 keepalive 는 SIP 메시지가 아니라 종전에는 소켓 계층에서
//   로그도 없이 버려졌다. UDP 는 연결이 없어 스택이 도달 경로의 죽음을 판정할 수 없으므로(IsFlowAlive 가 항상
//   true), 이 keepalive 가 "그 경로가 아직 살아 있다"는 유일한 신호다. 응용까지 올라오는지 검증한다.
//   함께, STUN Binding Request 로 keepalive 를 보내는 단말(RFC 5626 §4.4.2)에 XOR-MAPPED-ADDRESS 로 답하는지도
//   본다 — 단말은 그 값으로 NAT 매핑이 옮겨간 것을 알아채고 재등록한다.
//   (ext/psip/SipStack/SipUdpThread.cpp, registration_binding_set.md §4.1/§4.3)
//
//   A. CRLF 1개(2바이트)            → 응용에 keepalive 통지, 응답 없음(스택 자기깨움과 같은 모양이라 pong 금지)
//   B. CRLF 2개(4바이트) = RFC 5626 ping → 응용에 통지 + pong(CRLF 1개) 회신
//   C. STUN Binding Request          → 응용에 통지 + Binding Success Response 의 XOR-MAPPED-ADDRESS = 보낸 포트
//   D. 짧은 이진 쓰레기               → 통지도 응답도 없음 (종전대로 폐기)
//   E. 정상 SIP 요청(100바이트 이상)  → 종전대로 파서로 전달, keepalive 통지 없음 (회귀)
//
//   빌드(csp 빌드 뒤 — psip 정적 라이브러리 사용, 127.0.0.1 포트만 사용해 라이브 서비스와 무관):
//     g++ -std=c++17 -D__LINUX__ -D_REENTRANT -I ext/psip/SipStack -I ext/psip/SipParser -I ext/psip/SdpParser \
//         -I ext/psip/StunParser -I ext/psip/XmlParser -I ext/psip/SipPlatform -I ext/psip/ServerPlatform \
//         tests/psip_keepalive_test.cpp build/csp/psip_build/libSipStack.a build/csp/psip_build/libSdpParser.a \
//         build/csp/psip_build/libSipParser.a build/csp/psip_build/libStunParser.a build/csp/psip_build/libXmlParser.a \
//         build/csp/psip_build/libSipPlatform.a build/csp/psip_build/libServerPlatform.a \
//         -lssl -lcrypto -lpthread -o build/psip_keepalive_test
//     build/psip_keepalive_test [--port 27070] [--verbose]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Log.h"
#include "SipStack.h"
#include "StunDefine.h"
#include "StunMessage.h"

static const char *UA_IP = "127.0.0.1";
static int g_iUaPort = 27070;
static int g_iFail = 0;

#define CHECK( cond, msg )                                  \
	do {                                                     \
		if( cond ) printf( "  PASS  %s\n", msg );            \
		else { printf( "  FAIL  %s\n", msg ); ++g_iFail; }   \
	} while( 0 )

// ── 시험용 응용 콜백 — keepalive 통지와 SIP 요청 수신을 센다 ──
class CTestCallBack : public ISipStackCallBack
{
public:
	int m_iKeepAlive = 0;
	int m_iRequest = 0;
	std::string m_strLastIp;
	int m_iLastPort = 0;

	bool RecvRequest( int, CSipMessage * ) override { ++m_iRequest; return true; }
	bool RecvResponse( int, CSipMessage * ) override { return false; }
	bool SendTimeout( int, CSipMessage * ) override { return false; }

	void EventKeepAlive( const char * pszIp, int iPort, ESipTransport ) override
	{
		++m_iKeepAlive;
		m_strLastIp = pszIp ? pszIp : "";
		m_iLastPort = iPort;
	}
};

// ── 원시 UDP 소켓 ───────────────────────────────────────
static int UdpOpen( int &iPort )
{
	int fd = socket( AF_INET, SOCK_DGRAM, 0 );
	if( fd < 0 ) return -1;

	sockaddr_in a; memset( &a, 0, sizeof(a) );
	a.sin_family = AF_INET; a.sin_addr.s_addr = inet_addr( UA_IP );
	if( bind( fd, (sockaddr *)&a, sizeof(a) ) ) { close( fd ); return -1; }

	socklen_t n = sizeof(a);
	getsockname( fd, (sockaddr *)&a, &n );
	iPort = ntohs( a.sin_port );
	return fd;
}

static void UdpSendTo( int fd, const char *pszBuf, int iLen )
{
	sockaddr_in a; memset( &a, 0, sizeof(a) );
	a.sin_family = AF_INET; a.sin_addr.s_addr = inet_addr( UA_IP ); a.sin_port = htons( g_iUaPort );
	sendto( fd, pszBuf, iLen, 0, (sockaddr *)&a, sizeof(a) );
}

/** 응답을 iWaitMs 안에 받는다. 받으면 길이, 못 받으면 0. */
static int UdpRecvWait( int fd, char *pszBuf, int iBufSize, int iWaitMs )
{
	timeval tv; tv.tv_sec = iWaitMs / 1000; tv.tv_usec = ( iWaitMs % 1000 ) * 1000;
	setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv) );
	ssize_t n = recv( fd, pszBuf, iBufSize, 0 );
	return n > 0 ? (int)n : 0;
}

int main( int argc, char *argv[] )
{
	bool bVerbose = false;
	for( int i = 1; i < argc; ++i )
	{
		if( !strcmp( argv[i], "--port" ) && i + 1 < argc ) g_iUaPort = atoi( argv[++i] );
		else if( !strcmp( argv[i], "--verbose" ) ) bVerbose = true;
	}
	CLog::SetLevel( bVerbose ? ( LOG_ERROR | LOG_INFO | LOG_DEBUG | LOG_NETWORK ) : 0 );

	CSipStackSetup clsSetup;
	clsSetup.m_strLocalIp = UA_IP;
	clsSetup.m_iLocalUdpPort = g_iUaPort;
	clsSetup.m_iUdpThreadCount = 1;
	clsSetup.m_strDomain = "test.local";

	CSipStack clsStack;
	CTestCallBack clsCb;
	clsStack.AddCallBack( &clsCb );
	if( clsStack.Start( clsSetup ) == false )
	{
		printf( "stack start failed (port %d in use?)\n", g_iUaPort );
		return 2;
	}
	usleep( 300 * 1000 );

	int iUePort = 0;
	int fdUe = UdpOpen( iUePort );
	if( fdUe < 0 ) { printf( "socket open failed\n" ); clsStack.Stop(); return 2; }

	char szBuf[512];

	// ── A. CRLF 1개 — 통지는 하되 pong 은 보내지 않는다 ──
	printf( "[A] CRLF 1개(2바이트) keepalive\n" );
	{
		int iBefore = clsCb.m_iKeepAlive;
		UdpSendTo( fdUe, "\r\n", 2 );
		usleep( 300 * 1000 );
		CHECK( clsCb.m_iKeepAlive == iBefore + 1, "응용에 keepalive 통지" );
		CHECK( clsCb.m_strLastIp == UA_IP && clsCb.m_iLastPort == iUePort, "통지된 출발지가 보낸 소켓과 같다" );
		CHECK( UdpRecvWait( fdUe, szBuf, sizeof(szBuf), 300 ) == 0, "응답 없음 (자기깨움 되받이 고리 방지)" );
	}

	// ── B. CRLF 2개 = RFC 5626 ping → pong ──
	printf( "[B] CRLF 2개(4바이트) ping\n" );
	{
		int iBefore = clsCb.m_iKeepAlive;
		UdpSendTo( fdUe, "\r\n\r\n", 4 );
		usleep( 300 * 1000 );
		CHECK( clsCb.m_iKeepAlive == iBefore + 1, "응용에 keepalive 통지" );
		int n = UdpRecvWait( fdUe, szBuf, sizeof(szBuf), 1000 );
		CHECK( n == 2 && !memcmp( szBuf, "\r\n", 2 ), "pong(CRLF 1개) 회신" );
	}

	// ── C. STUN Binding Request → XOR-MAPPED-ADDRESS ──
	printf( "[C] STUN Binding Request\n" );
	{
		unsigned char arrReq[20] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };
		for( int i = 8; i < 20; ++i ) arrReq[i] = (unsigned char)( 0x30 + i );

		int iBefore = clsCb.m_iKeepAlive;
		UdpSendTo( fdUe, (const char *)arrReq, sizeof(arrReq) );
		usleep( 300 * 1000 );
		CHECK( clsCb.m_iKeepAlive == iBefore + 1, "응용에 keepalive 통지" );

		int n = UdpRecvWait( fdUe, szBuf, sizeof(szBuf), 1000 );
		CHECK( n >= STUN_HEADER_SIZE, "STUN 응답 수신" );

		CStunMessage clsRes;
		bool bParsed = ( n > 0 && clsRes.Parse( szBuf, n ) > 0 );
		CHECK( bParsed, "STUN 응답 파싱" );
		CHECK( bParsed && clsRes.m_clsHeader.m_sMessageType == ( STUN_MT_RESPONSE_SUCCESS | STUN_MT_BINDING ),
			"Binding Success Response" );
		CHECK( bParsed && !memcmp( clsRes.m_clsHeader.m_strTransactionId.c_str(), arrReq + 8, 12 ),
			"트랜잭션 ID 가 요청과 같다" );

		std::string strIp; uint16_t sPort = 0; bool bAddr = false;
		if( bParsed )
		{
			for( STUN_ATTRIBUTE_LIST::iterator it = clsRes.m_clsAttributeList.begin();
				it != clsRes.m_clsAttributeList.end(); ++it )
			{
				if( it->m_sType == STUN_AT_XOR_MAPPED_ADDRESS && it->GetIpPort( strIp, sPort ) ) bAddr = true;
			}
		}
		CHECK( bAddr, "XOR-MAPPED-ADDRESS 포함" );
		CHECK( bAddr && strIp == UA_IP && sPort == iUePort, "관측 주소가 실제 출발지와 같다" );
	}

	// ── D. 짧은 이진 쓰레기 — 종전대로 폐기 ──
	printf( "[D] 짧은 이진 쓰레기\n" );
	{
		int iBefore = clsCb.m_iKeepAlive;
		char arrJunk[20]; memset( arrJunk, (char)0xAA, sizeof(arrJunk) );
		UdpSendTo( fdUe, arrJunk, sizeof(arrJunk) );
		usleep( 300 * 1000 );
		CHECK( clsCb.m_iKeepAlive == iBefore, "keepalive 통지 없음" );
		CHECK( UdpRecvWait( fdUe, szBuf, sizeof(szBuf), 300 ) == 0, "응답 없음" );
	}

	// ── E. 정상 SIP 요청은 종전대로 (회귀) ──
	printf( "[E] 정상 SIP 요청 (회귀)\n" );
	{
		int iKaBefore = clsCb.m_iKeepAlive, iReqBefore = clsCb.m_iRequest;
		char szReq[1024];
		int n = snprintf( szReq, sizeof(szReq),
			"OPTIONS sip:test.local SIP/2.0\r\n"
			"Via: SIP/2.0/UDP %s:%d;branch=z9hG4bK-ka-test\r\n"
			"From: <sip:ka@test.local>;tag=katest\r\n"
			"To: <sip:test.local>\r\n"
			"Call-ID: ka-regression@test.local\r\n"
			"CSeq: 1 OPTIONS\r\n"
			"Max-Forwards: 70\r\n"
			"Content-Length: 0\r\n\r\n", UA_IP, iUePort );
		CHECK( n >= SIP_PACKET_MIN_SIZE, "시험 요청이 100바이트 이상" );
		UdpSendTo( fdUe, szReq, n );
		usleep( 400 * 1000 );
		CHECK( clsCb.m_iRequest == iReqBefore + 1, "SIP 요청이 파서로 전달됨" );
		CHECK( clsCb.m_iKeepAlive == iKaBefore, "keepalive 통지 없음" );
	}

	close( fdUe );
	clsStack.Stop();

	printf( "\n%s (fail=%d)\n", g_iFail ? "FAIL" : "PASS", g_iFail );
	return g_iFail ? 1 : 0;
}
