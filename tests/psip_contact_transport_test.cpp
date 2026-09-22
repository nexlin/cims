// psip 승격 flow 취급 하네스 — UDP 등록 단말이 RFC 3261 §18.1.1 로 TCP 승격해 보낸 INVITE 를 서버가 어떻게 다루는지.
//   ① 응답 Contact 의 transport: 응용이 SetContactTransport(callId, UDP) 를 넣으면 18x/2xx Contact 가 "받은 transport(TCP)"
//      대신 등록 바인딩의 transport(UDP)·그 listener 포트로 나간다 — 단말의 BYE/PRACK 가 일회성 승격 flow 가 아니라
//      등록 flow 로 오게 하기 위해 (registration_binding_set.md §3, leg_liveness.md §6.3 의 대칭).
//   ② CANCEL: 인증 훅(EventIncomingRequestAuth)을 거치지 않는다(RFC 3261 §22.1 MUST NOT challenge). 같은 트랜잭션
//      (최상위 Via sent-by+branch 일치)이면 200 + INVITE 487, 다른 곳에서 온 CANCEL 이나 대응 INVITE 없음은 481(§9.2).
//   (ext/psip/SipUserAgent/SipUserAgentCancel.hpp, SipUserAgentLegDest.hpp, SipStack/SipStackComm.hpp)
//
//   A. TCP 승격 INVITE + 응용 override UDP → 200 OK Contact: transport=tcp 없음, 포트 = UDP listener 포트
//   B. TCP 승격 INVITE, override 없음        → 200 OK Contact: transport=tcp, 포트 = TCP listener 포트 (종전 동작 회귀)
//   C. A 의 다이얼로그에 StopCall           → 서버 BYE 의 Contact 도 UDP(포트 = UDP listener) — 소켓 주소 덮어쓰기 없음
//   D. 울리는 INVITE 에 같은 연결의 CANCEL, 인증 훅이 CANCEL 에 false 를 답해도 → 200(CANCEL) + 487(INVITE), 훅 미호출
//   E. 울리는 INVITE 에 다른 연결(다른 sent-by)의 CANCEL → 481, INVITE 는 계속 울림(정리는 UAS StopCall)
//   F. 대응 INVITE 가 없는 CANCEL → 481
//   G. 같은 연결이지만 branch 가 다른 CANCEL → 481, 이어서 올바른 CANCEL 은 200 + 487
//
//   빌드(csp 빌드 뒤 — psip 정적 라이브러리 사용, 127.0.0.1 포트만 사용해 라이브 서비스와 무관):
//     g++ -std=c++17 -D__LINUX__ -D_REENTRANT -I ext/psip/SipUserAgent -I ext/psip/SipStack -I ext/psip/SipParser \
//         -I ext/psip/SdpParser -I ext/psip/StunParser -I ext/psip/XmlParser -I ext/psip/SipPlatform -I ext/psip/ServerPlatform \
//         tests/psip_contact_transport_test.cpp build/csp/psip_build/libSipUserAgent.a build/csp/psip_build/libSipStack.a \
//         build/csp/psip_build/libSdpParser.a build/csp/psip_build/libSipParser.a build/csp/psip_build/libStunParser.a \
//         build/csp/psip_build/libXmlParser.a build/csp/psip_build/libSipPlatform.a build/csp/psip_build/libServerPlatform.a \
//         -lssl -lcrypto -lpthread -o build/psip_contact_transport_test
//     build/psip_contact_transport_test [--port 27080] [--verbose]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "Log.h"
#include "SipUserAgent.h"

static const char *UA_IP = "127.0.0.1";
static int g_iUdpPort = 27080;   // UDP listener
static int g_iTcpPort = 27082;   // TCP listener — UDP 와 다른 번호여야 Contact 포트 검사가 의미 있다
static int g_iFail = 0;
static int g_iSeq = 0;

#define CHECK( cond, msg )                                        \
	do {                                                           \
		if( cond ) printf( "  PASS  %s\n", msg );                  \
		else { printf( "  FAIL  %s\n", msg ); ++g_iFail; }         \
	} while( 0 )

static long NowMs( ) { timeval tv; gettimeofday( &tv, NULL ); return tv.tv_sec * 1000L + tv.tv_usec / 1000; }

// ── 시험용 응용 콜백 ──────────────────────────────────────────────────────────────
class CTestCallBack : public ISipUserAgentCallBack
{
public:
	CSipUserAgent * m_pclsUa = NULL;
	bool m_bOverrideUdp = false;		// EventIncomingCall 에서 SetContactTransport(UDP)
	bool m_bRingOnly = false;			// true = 180 만 보내고 기다림 (CANCEL 시험)
	std::atomic<int> m_iAuthCancel{ 0 };	// EventIncomingRequestAuth 가 CANCEL 로 불린 횟수 (0 이어야 한다)
	std::atomic<int> m_iEnded{ 0 };

	void EventRegister( CSipServerInfo *, int ) override {}
	void EventIncomingCall( const char * pszCallId, const char *, const char *, CSipCallRtp *, CSipMessage * ) override
	{
		if( m_bOverrideUdp ) m_pclsUa->SetContactTransport( pszCallId, E_SIP_UDP );
		CSipCallRtp clsLocal;
		clsLocal.m_strIp = UA_IP;
		clsLocal.m_iPort = 40000;
		clsLocal.m_iCodec = 0;
		if( m_bRingOnly ) m_pclsUa->RingCall( pszCallId, 180, &clsLocal );
		else m_pclsUa->AcceptCall( pszCallId, &clsLocal );
	}
	void EventCallRing( const char *, int, CSipCallRtp * ) override {}
	void EventCallStart( const char *, CSipCallRtp * ) override {}
	void EventCallEnd( const char *, int ) override { ++m_iEnded; }
	bool EventIncomingRequestAuth( CSipMessage * pclsMessage ) override
	{
		// CANCEL 이 여기로 오면 규격 위반(§22.1) — 세고, "챌린지" 를 흉내 내 false 를 답한다.
		if( pclsMessage->IsMethod( SIP_METHOD_CANCEL ) ) { ++m_iAuthCancel; return false; }
		return true;
	}
};

// ── 단말 흉내: TCP 연결 ────────────────────────────────────────────────────────────
struct CConn { int fd = -1; int iLocalPort = 0; };

static bool ConnOpen( CConn & c )
{
	c.fd = socket( AF_INET, SOCK_STREAM, 0 );
	sockaddr_in addr; memset( &addr, 0, sizeof(addr) );
	addr.sin_family = AF_INET; addr.sin_addr.s_addr = inet_addr( UA_IP ); addr.sin_port = htons( g_iTcpPort );
	if( connect( c.fd, (sockaddr *)&addr, sizeof(addr) ) != 0 ) { perror( "connect" ); close( c.fd ); c.fd = -1; return false; }
	sockaddr_in loc; socklen_t len = sizeof(loc); getsockname( c.fd, (sockaddr *)&loc, &len );
	c.iLocalPort = ntohs( loc.sin_port );
	return true;
}
static void ConnClose( CConn & c ) { if( c.fd >= 0 ) { close( c.fd ); c.fd = -1; } }
static int ConnSend( CConn & c, const std::string & s ) { return (int)send( c.fd, s.data(), s.size(), 0 ); }

// needle 이 strOut 에 나타날 때까지 읽는다. 시간 안에 못 보면 false (strOut 은 읽은 만큼 남는다).
static bool ConnRecvUntil( CConn & c, const char * pszNeedle, int iTimeoutMs, std::string & strOut )
{
	long lEnd = NowMs() + iTimeoutMs;
	char buf[8192];
	for( ;; )
	{
		if( strOut.find( pszNeedle ) != std::string::npos ) return true;
		long lLeft = lEnd - NowMs();
		if( lLeft <= 0 ) return false;
		fd_set rs; FD_ZERO( &rs ); FD_SET( c.fd, &rs );
		timeval tv; tv.tv_sec = lLeft / 1000; tv.tv_usec = ( lLeft % 1000 ) * 1000;
		if( select( c.fd + 1, &rs, NULL, NULL, &tv ) <= 0 ) return false;
		int n = (int)recv( c.fd, buf, sizeof(buf) - 1, 0 );
		if( n <= 0 ) return false;
		strOut.append( buf, n );
	}
}

static std::string HeaderOf( const std::string & strMsg, size_t iFrom, const char * pszName )
{
	std::string key = std::string( "\r\n" ) + pszName + ":";
	size_t p = strMsg.find( key, iFrom );
	if( p == std::string::npos ) return "";
	p += key.size();
	size_t e = strMsg.find( "\r\n", p );
	std::string v = strMsg.substr( p, e - p );
	while( !v.empty() && v[0] == ' ' ) v.erase( 0, 1 );
	return v;
}

// 응답 strRx 안에서 status 로 시작하는 응답의 Contact 값 (없으면 "")
static std::string ContactOfResponse( const std::string & strRx, const char * pszStatus )
{
	size_t p = strRx.find( pszStatus );
	if( p == std::string::npos ) return "";
	return HeaderOf( strRx, p, "Contact" );
}

static int PortOfContact( const std::string & strContact )
{
	size_t at = strContact.find( '@' );
	if( at == std::string::npos ) return 0;
	size_t colon = strContact.find( ':', at );
	if( colon == std::string::npos ) return 0;
	return atoi( strContact.c_str() + colon + 1 );
}

// UDP 등록 단말 흉내: 승격 TCP 연결 c 로 INVITE (Contact = 자기 UDP 바인딩 ;ob). branch 를 돌려준다.
static std::string SendInvite( CConn & c, const std::string & strCallId, int iUeUdpPort )
{
	const char * pszSdp = "v=0\r\no=ue 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
		"m=audio 40002 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n";
	char szBranch[64]; snprintf( szBranch, sizeof(szBranch), "z9hG4bK-ct-%d", ++g_iSeq );
	char szInvite[4096];
	int iLen = snprintf( szInvite, sizeof(szInvite),
		"INVITE sip:svc@%s:%d SIP/2.0\r\n"
		"Via: SIP/2.0/TCP %s:%d;rport;branch=%s\r\n"
		"Max-Forwards: 70\r\n"
		"From: <sip:ue@test.local>;tag=ue-%d\r\n"
		"To: <sip:svc@test.local>\r\n"
		"Call-ID: %s\r\n"
		"CSeq: 1 INVITE\r\n"
		"Contact: <sip:ue@%s:%d;ob>\r\n"
		"Content-Type: application/sdp\r\n"
		"Content-Length: %d\r\n\r\n%s",
		UA_IP, g_iTcpPort, UA_IP, c.iLocalPort, szBranch, g_iSeq, strCallId.c_str(), UA_IP, iUeUdpPort,
		(int)strlen( pszSdp ), pszSdp );
	ConnSend( c, std::string( szInvite, iLen ) );
	return szBranch;
}

// 연결 c 로 CANCEL — Via sent-by 는 c 의 로컬 포트(= 이 연결로 INVITE 를 보냈다면 같은 hop), branch 는 인자.
static void SendCancel( CConn & c, const std::string & strCallId, const std::string & strBranch, int iFromTag )
{
	char szCancel[1024];
	int iLen = snprintf( szCancel, sizeof(szCancel),
		"CANCEL sip:svc@%s:%d SIP/2.0\r\n"
		"Via: SIP/2.0/TCP %s:%d;rport;branch=%s\r\n"
		"Max-Forwards: 70\r\n"
		"From: <sip:ue@test.local>;tag=ue-%d\r\n"
		"To: <sip:svc@test.local>\r\n"
		"Call-ID: %s\r\n"
		"CSeq: 1 CANCEL\r\n"
		"Content-Length: 0\r\n\r\n",
		UA_IP, g_iTcpPort, UA_IP, c.iLocalPort, strBranch.c_str(), iFromTag, strCallId.c_str() );
	ConnSend( c, std::string( szCancel, iLen ) );
}

static void SendAck( CConn & c, const std::string & strCallId, const std::string & strTo, int iFromTag )
{
	char szAck[1024];
	int iLen = snprintf( szAck, sizeof(szAck),
		"ACK sip:svc@%s:%d;transport=tcp SIP/2.0\r\n"
		"Via: SIP/2.0/TCP %s:%d;rport;branch=z9hG4bK-ct-ack-%d\r\n"
		"Max-Forwards: 70\r\n"
		"From: <sip:ue@test.local>;tag=ue-%d\r\n"
		"To: %s\r\n"
		"Call-ID: %s\r\n"
		"CSeq: 1 ACK\r\n"
		"Content-Length: 0\r\n\r\n",
		UA_IP, g_iTcpPort, UA_IP, c.iLocalPort, ++g_iSeq, iFromTag, strTo.c_str(), strCallId.c_str() );
	ConnSend( c, std::string( szAck, iLen ) );
}

static int UdpOpen( int & iPort )
{
	int fd = socket( AF_INET, SOCK_DGRAM, 0 );
	sockaddr_in addr; memset( &addr, 0, sizeof(addr) );
	addr.sin_family = AF_INET; addr.sin_addr.s_addr = inet_addr( UA_IP ); addr.sin_port = 0;
	if( bind( fd, (sockaddr *)&addr, sizeof(addr) ) != 0 ) { perror( "bind" ); exit( 2 ); }
	socklen_t len = sizeof(addr);
	getsockname( fd, (sockaddr *)&addr, &len );
	iPort = ntohs( addr.sin_port );
	return fd;
}

// UDP 바인딩에 pszMethod 요청이 오기를 기다린다. 오면 200 OK 로 응답하고 요청 원문을 돌려준다.
static bool UdpExpect( int fd, const char * pszMethod, int iTimeoutMs, std::string & strReq )
{
	long lEnd = NowMs() + iTimeoutMs;
	char buf[8192];
	sockaddr_in from; socklen_t fl = sizeof(from);
	for( ;; )
	{
		long lLeft = lEnd - NowMs();
		if( lLeft <= 0 ) return false;
		fd_set rs; FD_ZERO( &rs ); FD_SET( fd, &rs );
		timeval tv; tv.tv_sec = lLeft / 1000; tv.tv_usec = ( lLeft % 1000 ) * 1000;
		if( select( fd + 1, &rs, NULL, NULL, &tv ) <= 0 ) return false;
		fl = sizeof(from);
		int n = recvfrom( fd, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl );
		if( n <= 0 ) return false;
		strReq.assign( buf, n );
		if( strReq.compare( 0, strlen( pszMethod ), pszMethod ) == 0 ) break;
	}
	std::string strMsg = "\r\n" + strReq;
	char szRsp[2048];
	int iLen = snprintf( szRsp, sizeof(szRsp),
		"SIP/2.0 200 OK\r\nVia: %s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\nContent-Length: 0\r\n\r\n",
		HeaderOf( strMsg, 0, "Via" ).c_str(), HeaderOf( strMsg, 0, "From" ).c_str(), HeaderOf( strMsg, 0, "To" ).c_str(),
		HeaderOf( strMsg, 0, "Call-ID" ).c_str(), HeaderOf( strMsg, 0, "CSeq" ).c_str() );
	sendto( fd, szRsp, iLen, 0, (sockaddr *)&from, fl );
	return true;
}

// INVITE 를 보내고 200 OK 까지 받아 ACK. 200 OK 의 Contact 를 돌려준다.
static bool InviteAndAck( CConn & c, const std::string & strCallId, int iUeUdpPort, std::string & strContact200 )
{
	SendInvite( c, strCallId, iUeUdpPort );
	int iFromTag = g_iSeq;
	std::string strRx;
	if( ConnRecvUntil( c, "SIP/2.0 200", 3000, strRx ) == false ) { printf( "  (no 200 OK to INVITE)\n" ); return false; }
	size_t p200 = strRx.find( "SIP/2.0 200" );
	for( int i = 0; i < 20 && strRx.find( "\r\n\r\n", p200 ) == std::string::npos; ++i ) ConnRecvUntil( c, "\r\n\r\n", 200, strRx );
	std::string strTo = HeaderOf( strRx, p200, "To" );
	strContact200 = HeaderOf( strRx, p200, "Contact" );
	SendAck( c, strCallId, strTo, iFromTag );
	usleep( 200 * 1000 );
	return true;
}

int main( int argc, char * argv[] )
{
	bool bVerbose = false;
	for( int i = 1; i < argc; ++i )
	{
		if( !strcmp( argv[i], "--port" ) && i + 1 < argc ) { g_iUdpPort = atoi( argv[++i] ); g_iTcpPort = g_iUdpPort + 2; }
		else if( !strcmp( argv[i], "--verbose" ) ) bVerbose = true;
	}
	if( bVerbose ) CLog::SetLevel( LOG_ERROR | LOG_INFO | LOG_DEBUG | LOG_NETWORK );
	else CLog::SetLevel( 0 );

	CSipStackSetup clsSetup;
	clsSetup.m_strLocalIp = UA_IP;
	clsSetup.m_iLocalUdpPort = g_iUdpPort;
	clsSetup.m_iLocalTcpPort = g_iTcpPort;
	clsSetup.m_iTcpThreadCount = 1;
	clsSetup.m_strDomain = "test.local";

	CSipUserAgent clsUa;
	CTestCallBack clsCb;
	clsCb.m_pclsUa = &clsUa;
	if( clsUa.Start( clsSetup, &clsCb ) == false ) { printf( "UA start failed (port %d/%d in use?)\n", g_iUdpPort, g_iTcpPort ); return 2; }
	usleep( 300 * 1000 );

	int iUePort = 0;
	int fdUe = UdpOpen( iUePort );		// UDP 등록 단말의 등록 바인딩
	std::string strContact, strReq;

	// ── A. 승격 TCP INVITE + 응용 override UDP → 200 OK Contact 가 UDP listener ──
	printf( "[A] promoted-TCP INVITE, app SetContactTransport(UDP): 200 OK Contact advertises UDP\n" );
	clsCb.m_bOverrideUdp = true;
	CConn cA;
	CHECK( ConnOpen( cA ), "TCP connect" );
	CHECK( InviteAndAck( cA, "ct-A@test", iUePort, strContact ), "INVITE over TCP → 200 OK → ACK" );
	printf( "        Contact: %s\n", strContact.c_str() );
	CHECK( strContact.find( "transport=tcp" ) == std::string::npos, "Contact has no transport=tcp" );
	CHECK( PortOfContact( strContact ) == g_iUdpPort, "Contact port = UDP listener port" );

	// ── C. 그 다이얼로그에 StopCall → 서버 BYE(다이얼로그 주소 = 이 TCP 연결) 의 Contact 도 UDP 로 광고, 소켓주소로 덮어쓰지 않음 ──
	//   (BYE 의 목적지를 UDP 바인딩으로 재해석하는 것은 응용의 EventGetLegDest 몫 — psip_leg_dest_test 가 검증한다)
	printf( "[C] StopCall on A: server BYE over the dialog's TCP keeps a UDP Contact (CreateMessage propagation)\n" );
	clsUa.StopCall( "ct-A@test" );
	std::string strByeRx;
	bool bBye = ConnRecvUntil( cA, "BYE sip:", 3000, strByeRx ) && ConnRecvUntil( cA, "\r\n\r\n", 1000, strByeRx );
	CHECK( bBye, "BYE arrived on the dialog's TCP connection" );
	if( bBye )
	{
		std::string strByeContact = HeaderOf( strByeRx, strByeRx.find( "BYE sip:" ), "Contact" );
		printf( "        BYE Contact: %s\n", strByeContact.c_str() );
		CHECK( strByeContact.find( "transport=tcp" ) == std::string::npos, "BYE Contact has no transport=tcp" );
		CHECK( PortOfContact( strByeContact ) == g_iUdpPort, "BYE Contact port = UDP listener port (not the TCP socket)" );
	}
	ConnClose( cA );

	// ── B. 회귀: override 없으면 종전대로 받은 transport(TCP) 를 광고 ──
	printf( "[B] promoted-TCP INVITE, no override: 200 OK Contact keeps transport=tcp (regression)\n" );
	clsCb.m_bOverrideUdp = false;
	CConn cB;
	CHECK( ConnOpen( cB ), "TCP connect" );
	CHECK( InviteAndAck( cB, "ct-B@test", iUePort, strContact ), "INVITE over TCP → 200 OK → ACK" );
	printf( "        Contact: %s\n", strContact.c_str() );
	CHECK( strContact.find( "transport=tcp" ) != std::string::npos, "Contact has transport=tcp" );
	CHECK( PortOfContact( strContact ) == g_iTcpPort, "Contact port = TCP listener port" );
	clsUa.StopCall( "ct-B@test" );
	UdpExpect( fdUe, "BYE", 2000, strReq );
	ConnClose( cB );

	// ── D. 울리는 INVITE + 같은 연결의 CANCEL: 인증 훅이 false 를 답해도 200 + 487, 훅은 불리지 않는다 ──
	printf( "[D] ringing INVITE, CANCEL on the same connection: 200 + 487 without consulting the auth hook\n" );
	clsCb.m_bRingOnly = true;
	CConn cD;
	CHECK( ConnOpen( cD ), "TCP connect" );
	std::string strBranchD = SendInvite( cD, "ct-D@test", iUePort );
	int iTagD = g_iSeq;
	std::string strRxD;
	CHECK( ConnRecvUntil( cD, "SIP/2.0 180", 3000, strRxD ), "180 Ringing received" );
	int iEndedBefore = clsCb.m_iEnded;
	SendCancel( cD, "ct-D@test", strBranchD, iTagD );
	std::string strRxD2;
	bool b200 = ConnRecvUntil( cD, "SIP/2.0 200", 3000, strRxD2 );
	CHECK( b200 && HeaderOf( strRxD2, strRxD2.find( "SIP/2.0 200" ), "CSeq" ).find( "CANCEL" ) != std::string::npos, "200 OK to CANCEL" );
	CHECK( ConnRecvUntil( cD, "SIP/2.0 487", 3000, strRxD2 ), "487 Request Terminated to INVITE" );
	usleep( 200 * 1000 );
	CHECK( clsCb.m_iAuthCancel == 0, "EventIncomingRequestAuth never called for CANCEL (RFC 3261 §22.1)" );
	CHECK( clsCb.m_iEnded > iEndedBefore, "EventCallEnd fired" );
	ConnClose( cD );

	// ── E. 울리는 INVITE + 다른 연결(다른 sent-by)의 CANCEL → 481, INVITE 는 계속 울린다 ──
	//   같은 branch 로 두 번 CANCEL 하면 스택 재전송 캐시가 첫 응답(481)을 재사용하므로, 정리는 UAS 쪽 StopCall(603) 로 한다.
	printf( "[E] ringing INVITE, CANCEL from another connection (other sent-by): 481 and INVITE keeps ringing\n" );
	CConn cE, cE2;
	CHECK( ConnOpen( cE ), "TCP connect (INVITE)" );
	std::string strBranchE = SendInvite( cE, "ct-E@test", iUePort );
	int iTagE = g_iSeq;
	std::string strRxE;
	CHECK( ConnRecvUntil( cE, "SIP/2.0 180", 3000, strRxE ), "180 Ringing received" );
	CHECK( ConnOpen( cE2 ), "TCP connect (rogue CANCEL)" );
	SendCancel( cE2, "ct-E@test", strBranchE, iTagE );		// 같은 branch, 다른 sent-by 포트
	std::string strRxE2;
	CHECK( ConnRecvUntil( cE2, "SIP/2.0 481", 3000, strRxE2 ), "481 to CANCEL from another hop" );
	std::string strRxE3;
	CHECK( ConnRecvUntil( cE, "SIP/2.0 487", 800, strRxE3 ) == false, "INVITE not terminated by the rogue CANCEL" );
	clsUa.StopCall( "ct-E@test", SIP_DECLINE );
	CHECK( ConnRecvUntil( cE, "SIP/2.0 603", 3000, strRxE3 ), "UAS can still end the ringing INVITE (603)" );
	ConnClose( cE2 ); ConnClose( cE );

	// ── G. 울리는 INVITE + 같은 연결이지만 branch 가 다른 CANCEL → 481, 그 뒤 올바른 CANCEL 은 200 + 487 ──
	printf( "[G] ringing INVITE, CANCEL with a foreign branch on the same connection: 481, then the right CANCEL works\n" );
	CConn cG;
	CHECK( ConnOpen( cG ), "TCP connect" );
	std::string strBranchG = SendInvite( cG, "ct-G@test", iUePort );
	int iTagG = g_iSeq;
	std::string strRxG;
	CHECK( ConnRecvUntil( cG, "SIP/2.0 180", 3000, strRxG ), "180 Ringing received" );
	SendCancel( cG, "ct-G@test", "z9hG4bK-ct-foreign", iTagG );
	std::string strRxG2;
	CHECK( ConnRecvUntil( cG, "SIP/2.0 481", 3000, strRxG2 ), "481 to CANCEL with a foreign branch" );
	CHECK( strRxG2.find( "SIP/2.0 487" ) == std::string::npos, "INVITE not terminated by the foreign-branch CANCEL" );
	SendCancel( cG, "ct-G@test", strBranchG, iTagG );
	std::string strRxG3;
	CHECK( ConnRecvUntil( cG, "SIP/2.0 487", 3000, strRxG3 ), "487 after the CANCEL with the INVITE's branch" );
	ConnClose( cG );

	// ── F. 대응 INVITE 없는 CANCEL → 481 ──
	printf( "[F] CANCEL with no matching INVITE: 481\n" );
	CConn cF;
	CHECK( ConnOpen( cF ), "TCP connect" );
	SendCancel( cF, "ct-F-nothing@test", "z9hG4bK-ct-none", 999 );
	std::string strRxF;
	CHECK( ConnRecvUntil( cF, "SIP/2.0 481", 3000, strRxF ), "481 Call/Transaction Does Not Exist" );
	ConnClose( cF );

	close( fdUe );
	clsUa.Stop();
	printf( "\n%s (%d failure%s)\n", g_iFail ? "FAILED" : "ALL PASS", g_iFail, g_iFail == 1 ? "" : "s" );
	return g_iFail ? 1 : 0;
}
