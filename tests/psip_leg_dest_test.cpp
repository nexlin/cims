// psip 서버 발신 in-dialog 요청 목적지 재해석 하네스 — NAT 뒤 단말이 대형 INVITE 를 TCP 로 승격해 보낸 뒤
//   그 연결이 닫히면(단말 pjsip 유휴 33초) 다이얼로그가 기억한 소스(TCP)로는 서버가 다시 연결할 수 없다.
//   서버 발신 BYE·re-INVITE 가 생성 직전 EventGetLegDest 로 살아있는 등록 바인딩을 다시 골라 도달하는지,
//   그리고 연결이 살아있는 TCP/TLS 등록 단말에는 동작이 바뀌지 않는지 검증한다
//   (ext/psip/SipUserAgent/SipUserAgentLegDest.hpp, leg_liveness.md §6.3).
//   psip UA 를 UAS 로 127.0.0.1 에 띄우고(UDP·TCP 같은 포트 + TLS 포트+1), 원시 소켓이 단말을 흉내낸다.
//
//   A. UDP 등록 + 승격 TCP 닫힘, 응용이 UDP 바인딩을 답함 → StopCall 의 BYE 가 UDP 바인딩에 도착 (Route = 그 주소)
//   B. 같은 상황, 응용이 false 를 답함                     → BYE 는 옛 TCP 소스로 감(닫힌 포트 → 660) — 기존 동작 보존
//   C. Record-Route 있는 다이얼로그                         → 응용에 묻지 않고 Route 를 그대로 따름
//   D. SendReInvite 도 같은 경로                            → re-INVITE 가 UDP 바인딩에 도착
//   E. TCP 등록(연결 유지), 응용이 그 TCP 바인딩을 답함     → BYE 가 살아있는 같은 TCP 연결로 도착 (무변경)
//   F. TLS 등록(연결 유지), 응용이 그 TLS 바인딩을 답함     → BYE 가 살아있는 같은 TLS 연결로 도착 (무변경)
//      F 는 openssl CLI 로 만든 자가서명 인증서를 쓴다 — 생성 실패 시 SKIP.
//
//   빌드(csp 빌드 뒤 — psip 정적 라이브러리 사용, 라이브 서비스와 무관한 127.0.0.1 포트만 사용):
//     g++ -std=c++17 -D__LINUX__ -D_REENTRANT -I ext/psip/SipUserAgent -I ext/psip/SipStack -I ext/psip/SipParser \
//         -I ext/psip/SdpParser -I ext/psip/StunParser -I ext/psip/XmlParser -I ext/psip/SipPlatform \
//         -I ext/psip/ServerPlatform tests/psip_leg_dest_test.cpp \
//         build/csp/psip_build/libSipUserAgent.a build/csp/psip_build/libSipStack.a build/csp/psip_build/libSdpParser.a \
//         build/csp/psip_build/libSipParser.a build/csp/psip_build/libStunParser.a build/csp/psip_build/libXmlParser.a \
//         build/csp/psip_build/libSipPlatform.a build/csp/psip_build/libServerPlatform.a \
//         -lssl -lcrypto -lpthread -o build/psip_leg_dest_test
//     build/psip_leg_dest_test [--port 27060] [--verbose]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
static int g_iUaPort = 27060;   // UDP·TCP 같은 번호 (CSP 15060 겸용 구성과 동일), TLS = +1
static int g_iFail = 0;
static int g_iSeq = 0;

#define CHECK( cond, msg )                                        \
	do {                                                           \
		if( cond ) printf( "  PASS  %s\n", msg );                  \
		else { printf( "  FAIL  %s\n", msg ); ++g_iFail; }         \
	} while( 0 )

static long NowMs( ) { timeval tv; gettimeofday( &tv, NULL ); return tv.tv_sec * 1000L + tv.tv_usec / 1000; }

// ── 시험용 응용 콜백 ─────────────────────────────────────────────────────────
class CTestCallBack : public ISipUserAgentCallBack
{
public:
	CSipUserAgent * m_pclsUa = NULL;
	std::mutex m_clsMutex;

	bool m_bAnswerDest = false;			// EventGetLegDest 응답 여부
	std::string m_strDestIp;
	int m_iDestPort = 0;
	ESipTransport m_eDestTransport = E_SIP_UDP;
	std::atomic<int> m_iAsked{ 0 };
	std::string m_strAskedPeer;

	void EventRegister( CSipServerInfo *, int ) override {}
	void EventIncomingCall( const char * pszCallId, const char *, const char *, CSipCallRtp *, CSipMessage * ) override
	{
		CSipCallRtp clsLocal;
		clsLocal.m_strIp = UA_IP;
		clsLocal.m_iPort = 40000;
		clsLocal.m_iCodec = 0;
		m_pclsUa->AcceptCall( pszCallId, &clsLocal );
	}
	void EventCallRing( const char *, int, CSipCallRtp * ) override {}
	void EventCallStart( const char *, CSipCallRtp * ) override {}
	void EventCallEnd( const char *, int ) override {}
	bool EventGetLegDest( const char *, const char * pszPeerId, std::string & strIp, int & iPort,
		ESipTransport & eTransport ) override
	{
		++m_iAsked;
		{
			std::lock_guard<std::mutex> lk( m_clsMutex );
			m_strAskedPeer = pszPeerId ? pszPeerId : "";
		}
		if( m_bAnswerDest == false ) return false;
		strIp = m_strDestIp;
		iPort = m_iDestPort;
		eTransport = m_eDestTransport;
		return true;
	}
	void SetDest( const char * pszIp, int iPort, ESipTransport eTransport )
	{
		m_bAnswerDest = true; m_strDestIp = pszIp; m_iDestPort = iPort; m_eDestTransport = eTransport;
	}
};

// ── 단말 흉내: 연결(TCP/TLS) ─────────────────────────────────────────────────
struct CConn
{
	int fd = -1;
	SSL * ssl = NULL;
	SSL_CTX * ctx = NULL;
	int iLocalPort = 0;			// psip 이 보는 peer 포트 = 소켓맵 키
	bool bTls = false;
	const char * Transport( ) const { return bTls ? "TLS" : "TCP"; }
	const char * transport( ) const { return bTls ? "tls" : "tcp"; }
};

static bool ConnOpen( CConn & c, int iPort, bool bTls )
{
	c.bTls = bTls;
	c.fd = socket( AF_INET, SOCK_STREAM, 0 );
	sockaddr_in addr; memset( &addr, 0, sizeof(addr) );
	addr.sin_family = AF_INET; addr.sin_addr.s_addr = inet_addr( UA_IP ); addr.sin_port = htons( iPort );
	if( connect( c.fd, (sockaddr *)&addr, sizeof(addr) ) != 0 ) { perror( "connect" ); close( c.fd ); c.fd = -1; return false; }
	sockaddr_in loc; socklen_t len = sizeof(loc); getsockname( c.fd, (sockaddr *)&loc, &len );
	c.iLocalPort = ntohs( loc.sin_port );
	if( bTls )
	{
		c.ctx = SSL_CTX_new( TLS_client_method() );
		SSL_CTX_set_verify( c.ctx, SSL_VERIFY_NONE, NULL );
		c.ssl = SSL_new( c.ctx );
		SSL_set_fd( c.ssl, c.fd );
		if( SSL_connect( c.ssl ) <= 0 ) { ERR_print_errors_fp( stdout ); return false; }
	}
	return true;
}

static void ConnClose( CConn & c )
{
	if( c.ssl ) { SSL_shutdown( c.ssl ); SSL_free( c.ssl ); c.ssl = NULL; }
	if( c.ctx ) { SSL_CTX_free( c.ctx ); c.ctx = NULL; }
	if( c.fd >= 0 ) { close( c.fd ); c.fd = -1; }
}

static int ConnSend( CConn & c, const char * p, int n ) { return c.bTls ? SSL_write( c.ssl, p, n ) : (int)send( c.fd, p, n, 0 ); }

static bool ConnRecvUntil( CConn & c, const char * pszNeedle, int iTimeoutMs, std::string & strOut )
{
	long lEnd = NowMs() + iTimeoutMs;
	char buf[8192];
	for( ;; )
	{
		if( strOut.find( pszNeedle ) != std::string::npos ) return true;
		long lLeft = lEnd - NowMs();
		if( lLeft <= 0 ) return false;
		if( !( c.bTls && SSL_pending( c.ssl ) > 0 ) )
		{
			fd_set rs; FD_ZERO( &rs ); FD_SET( c.fd, &rs );
			timeval tv; tv.tv_sec = lLeft / 1000; tv.tv_usec = ( lLeft % 1000 ) * 1000;
			if( select( c.fd + 1, &rs, NULL, NULL, &tv ) <= 0 ) return false;
		}
		int n = c.bTls ? SSL_read( c.ssl, buf, sizeof(buf) - 1 ) : (int)recv( c.fd, buf, sizeof(buf) - 1, 0 );
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

// 수신 요청에 대한 200 OK (INVITE 면 SDP 동봉). iContactPort = 단말이 광고하는 Contact 포트.
static std::string Build200( const std::string & strReq, int iContactPort, const char * pszTransportParam )
{
	std::string strMsg = "\r\n" + strReq;
	std::string strBody;
	if( strReq.compare( 0, 6, "INVITE" ) == 0 )
		strBody = "v=0\r\no=ue 1 2 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 40002 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n";
	char szRsp[4096];
	int iLen = snprintf( szRsp, sizeof(szRsp),
		"SIP/2.0 200 OK\r\nVia: %s\r\nFrom: %s\r\nTo: %s;tag=ue-%d\r\nCall-ID: %s\r\nCSeq: %s\r\n"
		"Contact: <sip:ue@%s:%d%s;ob>\r\n%sContent-Length: %d\r\n\r\n%s",
		HeaderOf( strMsg, 0, "Via" ).c_str(), HeaderOf( strMsg, 0, "From" ).c_str(), HeaderOf( strMsg, 0, "To" ).c_str(),
		g_iSeq, HeaderOf( strMsg, 0, "Call-ID" ).c_str(), HeaderOf( strMsg, 0, "CSeq" ).c_str(), UA_IP, iContactPort, pszTransportParam,
		strBody.empty() ? "" : "Content-Type: application/sdp\r\n", (int)strBody.size(), strBody.c_str() );
	return std::string( szRsp, iLen );
}

// 단말: 연결 c 로 INVITE(Contact = 자기 바인딩) → 200 OK 대기 → ACK. 연결은 닫지 않는다(호출자가 결정).
//   UDP 등록 단말은 Contact 를 UDP 바인딩(;ob)으로, TCP/TLS 등록 단말은 자기 연결 포트+transport 로 광고한다.
static bool UeInvite( CConn & c, const std::string & strCallId, const std::string & strContact, const char * pszRecordRoute )
{
	int iUaPort = c.bTls ? g_iUaPort + 1 : g_iUaPort;
	const char * pszSdp = "v=0\r\no=ue 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
		"m=audio 40002 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n";
	char szRr[256] = "";
	if( pszRecordRoute ) snprintf( szRr, sizeof(szRr), "Record-Route: %s\r\n", pszRecordRoute );
	char szInvite[4096];
	int iLen = snprintf( szInvite, sizeof(szInvite),
		"INVITE sip:svc@%s:%d SIP/2.0\r\n"
		"Via: SIP/2.0/%s %s:%d;rport;branch=z9hG4bK-legdest-%d\r\n"
		"Max-Forwards: 70\r\n"
		"From: <sip:ue@test.local>;tag=ue-%d\r\n"
		"To: <sip:svc@test.local>\r\n"
		"Call-ID: %s\r\n"
		"CSeq: 1 INVITE\r\n"
		"Contact: %s\r\n"
		"%s"
		"Content-Type: application/sdp\r\n"
		"Content-Length: %d\r\n\r\n%s",
		UA_IP, iUaPort, c.Transport(), UA_IP, c.iLocalPort, ++g_iSeq, g_iSeq, strCallId.c_str(), strContact.c_str(), szRr,
		(int)strlen( pszSdp ), pszSdp );
	ConnSend( c, szInvite, iLen );

	std::string strRx;
	if( ConnRecvUntil( c, "SIP/2.0 200", 3000, strRx ) == false ) { printf( "  (no 200 OK to INVITE)\n" ); return false; }
	size_t p200 = strRx.find( "SIP/2.0 200" );
	std::string strTo = HeaderOf( strRx, p200, "To" );
	for( int i = 0; i < 20 && strTo.find( "tag=" ) == std::string::npos; ++i )
	{
		std::string more; ConnRecvUntil( c, "\r\n\r\n", 200, more ); strRx += more;
		strTo = HeaderOf( strRx, p200, "To" );
	}
	if( strTo.find( "tag=" ) == std::string::npos ) { printf( "  (200 OK without To tag)\n" ); return false; }

	char szAck[1024];
	iLen = snprintf( szAck, sizeof(szAck),
		"ACK sip:svc@%s:%d;transport=%s SIP/2.0\r\n"
		"Via: SIP/2.0/%s %s:%d;rport;branch=z9hG4bK-legdest-ack-%d\r\n"
		"Max-Forwards: 70\r\n"
		"From: <sip:ue@test.local>;tag=ue-%d\r\n"
		"To: %s\r\n"
		"Call-ID: %s\r\n"
		"CSeq: 1 ACK\r\n"
		"Content-Length: 0\r\n\r\n",
		UA_IP, iUaPort, c.transport(), c.Transport(), UA_IP, c.iLocalPort, g_iSeq, g_iSeq, strTo.c_str(), strCallId.c_str() );
	ConnSend( c, szAck, iLen );
	usleep( 200 * 1000 );
	return true;
}

// UDP 등록 단말 흉내: 승격 TCP 로 INVITE 를 보내고 ACK 뒤 그 연결을 닫는다 (pjsip 유휴 종료 모사).
static bool UeInviteOverPromotedTcp( const std::string & strCallId, int iUeUdpPort, const char * pszRecordRoute )
{
	CConn c;
	if( ConnOpen( c, g_iUaPort, false ) == false ) return false;
	char szContact[128]; snprintf( szContact, sizeof(szContact), "<sip:ue@%s:%d;ob>", UA_IP, iUeUdpPort );
	bool bRes = UeInvite( c, strCallId, szContact, pszRecordRoute );
	ConnClose( c );		// 이후 서버가 이 주소로 새 연결을 걸면 실패한다
	usleep( 300 * 1000 );
	return bRes;
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

// UDP 바인딩에 pszMethod 요청이 오기를 기다린다. 다른 요청(re-INVITE 뒤의 ACK 등)은 건너뛴다. 오면 200 OK 로 응답.
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
		if( strReq.compare( 0, 3, "ACK" ) != 0 ) printf( "  (skipping UDP message: %.40s)\n", buf );
	}
	sockaddr_in me; socklen_t ml = sizeof(me); getsockname( fd, (sockaddr *)&me, &ml );
	std::string strRsp = Build200( strReq, ntohs( me.sin_port ), "" );
	sendto( fd, strRsp.data(), strRsp.size(), 0, (sockaddr *)&from, fl );
	return true;
}

// 살아있는 TCP/TLS 연결에 pszMethod 요청이 오기를 기다린다 (BYE 는 본문 없음 — 헤더 끝까지). 오면 200 OK 로 응답.
static bool ConnExpect( CConn & c, const char * pszMethod, int iTimeoutMs, std::string & strReq )
{
	std::string strRx;
	long lEnd = NowMs() + iTimeoutMs;
	for( ;; )
	{
		long lLeft = lEnd - NowMs();
		if( lLeft <= 0 ) return false;
		if( ConnRecvUntil( c, "\r\n\r\n", (int)lLeft, strRx ) == false ) return false;
		size_t e = strRx.find( "\r\n\r\n" ) + 4;
		strReq = strRx.substr( 0, e ); strRx.erase( 0, e );
		if( strReq.compare( 0, strlen( pszMethod ), pszMethod ) == 0 ) break;
		printf( "  (skipping %s message: %.40s)\n", c.Transport(), strReq.c_str() );
	}
	char szTp[32]; snprintf( szTp, sizeof(szTp), ";transport=%s", c.transport() );
	std::string strRsp = Build200( strReq, c.iLocalPort, szTp );
	ConnSend( c, strRsp.data(), (int)strRsp.size() );
	return true;
}

static std::string RouteOf( const std::string & strReq ) { return HeaderOf( "\r\n" + strReq, 0, "Route" ); }

// 자가서명 인증서 (F 케이스). 실패 시 false.
static bool MakeSelfSignedCert( std::string & strDir, std::string & strCert, std::string & strKey )
{
	char szDir[] = "/tmp/psip_leg_dest.XXXXXX";
	if( mkdtemp( szDir ) == NULL ) return false;
	strDir = szDir; strCert = strDir + "/cert.pem"; strKey = strDir + "/key.pem";
	std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=127.0.0.1 -keyout " + strKey +
		" -out " + strCert + " >/dev/null 2>&1";
	if( system( cmd.c_str() ) != 0 ) return false;
	struct stat st;
	return stat( strCert.c_str(), &st ) == 0 && stat( strKey.c_str(), &st ) == 0;
}

int main( int argc, char * argv[] )
{
	bool bVerbose = false;
	for( int i = 1; i < argc; ++i )
	{
		if( !strcmp( argv[i], "--port" ) && i + 1 < argc ) g_iUaPort = atoi( argv[++i] );
		else if( !strcmp( argv[i], "--verbose" ) ) bVerbose = true;
	}
	if( bVerbose ) CLog::SetLevel( LOG_ERROR | LOG_INFO | LOG_DEBUG | LOG_NETWORK );
	else CLog::SetLevel( 0 );

	std::string strCertDir, strCert, strKey;
	bool bTlsReady = MakeSelfSignedCert( strCertDir, strCert, strKey );

	CSipStackSetup clsSetup;
	clsSetup.m_strLocalIp = UA_IP;
	clsSetup.m_iLocalUdpPort = g_iUaPort;
	clsSetup.m_iLocalTcpPort = g_iUaPort;
	clsSetup.m_iTcpThreadCount = 1;
	clsSetup.m_strDomain = "test.local";
	if( bTlsReady )
	{
		clsSetup.m_iLocalTlsPort = g_iUaPort + 1;
		clsSetup.m_strCertFile = strCert;
		clsSetup.m_strKeyFile = strKey;
	}

	CSipUserAgent clsUa;
	CTestCallBack clsCb;
	clsCb.m_pclsUa = &clsUa;
	if( clsUa.Start( clsSetup, &clsCb ) == false ) { printf( "UA start failed (port %d in use?)\n", g_iUaPort ); return 2; }
	usleep( 300 * 1000 );

	int iUePort = 0, iProxyPort = 0;
	int fdUe = UdpOpen( iUePort );			// UDP 등록 단말의 등록 바인딩
	int fdProxy = UdpOpen( iProxyPort );	// C 케이스의 Record-Route 홉
	std::string strReq;
	char szPort[16]; snprintf( szPort, sizeof(szPort), ":%d", iUePort );

	// ── A. UDP 등록 + 승격 TCP 닫힘: 응용이 UDP 바인딩을 답하면 BYE 가 그 주소로 간다 ──
	printf( "[A] UDP-registered, promoted TCP closed: StopCall → BYE to live UDP binding\n" );
	clsCb.SetDest( UA_IP, iUePort, E_SIP_UDP );
	CHECK( UeInviteOverPromotedTcp( "legdest-A@test", iUePort, NULL ), "INVITE over TCP established, TCP closed" );
	int iAsked = clsCb.m_iAsked;
	clsUa.StopCall( "legdest-A@test" );
	bool bGot = UdpExpect( fdUe, "BYE", 3000, strReq );
	CHECK( bGot, "BYE arrived on UDP binding" );
	CHECK( clsCb.m_iAsked > iAsked, "EventGetLegDest was asked" );
	CHECK( clsCb.m_strAskedPeer == "ue", "peer id = remote user (From) of the dialog" );
	if( bGot )
	{
		std::string strRoute = RouteOf( strReq );
		CHECK( strRoute.find( szPort ) != std::string::npos && strRoute.find( "transport=tcp" ) == std::string::npos,
			( "Route points at UDP binding: " + strRoute ).c_str() );
		CHECK( strReq.find( "BYE sip:ue@127.0.0.1" ) == 0 && strReq.find( ";ob" ) != std::string::npos,
			"R-URI = remote Contact (target unchanged)" );
	}

	// ── B. 응용이 false 를 답하면 기존 동작(수신 당시 소스) ──
	printf( "[B] callback false → BYE goes to the remembered (dead) TCP source, not to UDP\n" );
	clsCb.m_bAnswerDest = false;
	CHECK( UeInviteOverPromotedTcp( "legdest-B@test", iUePort, NULL ), "INVITE over TCP established, TCP closed" );
	iAsked = clsCb.m_iAsked;
	clsUa.StopCall( "legdest-B@test" );
	CHECK( UdpExpect( fdUe, "BYE", 1500, strReq ) == false, "no BYE on UDP binding (legacy path preserved)" );
	CHECK( clsCb.m_iAsked > iAsked, "EventGetLegDest was asked (guard passed)" );

	// ── C. Record-Route 다이얼로그는 묻지 않고 Route 를 따른다 ──
	printf( "[C] Record-Route dialog → callback not asked, BYE follows the route set\n" );
	clsCb.SetDest( UA_IP, iUePort, E_SIP_UDP );
	char szRr[128]; snprintf( szRr, sizeof(szRr), "<sip:%s:%d;lr>", UA_IP, iProxyPort );
	CHECK( UeInviteOverPromotedTcp( "legdest-C@test", iUePort, szRr ), "INVITE over TCP with Record-Route established" );
	iAsked = clsCb.m_iAsked;
	clsUa.StopCall( "legdest-C@test" );
	CHECK( UdpExpect( fdProxy, "BYE", 3000, strReq ), "BYE arrived at the Record-Route hop" );
	CHECK( clsCb.m_iAsked == iAsked, "EventGetLegDest not asked for Record-Route dialog" );

	// ── D. re-INVITE 도 같은 경로 ──
	printf( "[D] SendReInvite → re-INVITE to live UDP binding\n" );
	CHECK( UeInviteOverPromotedTcp( "legdest-D@test", iUePort, NULL ), "INVITE over TCP established, TCP closed" );
	iAsked = clsCb.m_iAsked;
	CSipCallRtp clsRtp; clsRtp.m_strIp = UA_IP; clsRtp.m_iPort = 40000; clsRtp.m_iCodec = 0;
	clsUa.SendReInvite( "legdest-D@test", &clsRtp );
	bGot = UdpExpect( fdUe, "INVITE", 3000, strReq );
	CHECK( bGot, "re-INVITE arrived on UDP binding" );
	CHECK( clsCb.m_iAsked > iAsked, "EventGetLegDest was asked" );
	usleep( 300 * 1000 );		// ACK 소화
	clsUa.StopCall( "legdest-D@test" );
	CHECK( UdpExpect( fdUe, "BYE", 3000, strReq ), "BYE after re-INVITE also on UDP binding" );

	// ── E. TCP 등록 단말(연결 유지): 응용이 같은 TCP 바인딩을 답하면 살아있는 그 연결로 나간다 ──
	printf( "[E] TCP-registered, connection alive → BYE over the same live TCP connection\n" );
	{
		CConn c;
		CHECK( ConnOpen( c, g_iUaPort, false ), "TCP connection opened (registration flow)" );
		char szContact[128]; snprintf( szContact, sizeof(szContact), "<sip:ue@%s:%d;transport=tcp;ob>", UA_IP, c.iLocalPort );
		CHECK( UeInvite( c, "legdest-E@test", szContact, NULL ), "INVITE over TCP established, connection kept open" );
		clsCb.SetDest( UA_IP, c.iLocalPort, E_SIP_TCP );		// CSP 라면 등록 바인딩 = 이 연결의 peer 주소
		iAsked = clsCb.m_iAsked;
		clsUa.StopCall( "legdest-E@test" );
		bGot = ConnExpect( c, "BYE", 3000, strReq );
		CHECK( bGot, "BYE arrived on the live TCP connection" );
		CHECK( clsCb.m_iAsked > iAsked, "EventGetLegDest was asked" );
		if( bGot ) CHECK( RouteOf( strReq ).find( "transport=tcp" ) != std::string::npos, ( "Route keeps TCP: " + RouteOf( strReq ) ).c_str() );
		CHECK( UdpExpect( fdUe, "BYE", 500, strReq ) == false, "nothing leaked to the UDP socket" );
		ConnClose( c );
	}

	// ── F. TLS 등록 단말(연결 유지): 같은 TLS 연결로 나간다 ──
	printf( "[F] TLS-registered, connection alive → BYE over the same live TLS connection\n" );
	if( bTlsReady == false )
	{
		printf( "  SKIP  openssl 자가서명 인증서 생성 실패 — TLS 케이스 생략\n" );
	}
	else
	{
		CConn c;
		bool bOpen = ConnOpen( c, g_iUaPort + 1, true );
		CHECK( bOpen, "TLS connection opened (registration flow)" );
		if( bOpen )
		{
			char szContact[128]; snprintf( szContact, sizeof(szContact), "<sip:ue@%s:%d;transport=tls;ob>", UA_IP, c.iLocalPort );
			CHECK( UeInvite( c, "legdest-F@test", szContact, NULL ), "INVITE over TLS established, connection kept open" );
			clsCb.SetDest( UA_IP, c.iLocalPort, E_SIP_TLS );
			iAsked = clsCb.m_iAsked;
			clsUa.StopCall( "legdest-F@test" );
			bGot = ConnExpect( c, "BYE", 3000, strReq );
			CHECK( bGot, "BYE arrived on the live TLS connection" );
			CHECK( clsCb.m_iAsked > iAsked, "EventGetLegDest was asked" );
			if( bGot ) CHECK( RouteOf( strReq ).find( "transport=tls" ) != std::string::npos, ( "Route keeps TLS: " + RouteOf( strReq ) ).c_str() );
			CHECK( UdpExpect( fdUe, "BYE", 500, strReq ) == false, "nothing leaked to the UDP socket" );
		}
		ConnClose( c );
	}

	usleep( 200 * 1000 );
	clsUa.Stop();
	close( fdUe ); close( fdProxy );
	if( !strCertDir.empty() ) { unlink( strCert.c_str() ); unlink( strKey.c_str() ); rmdir( strCertDir.c_str() ); }

	printf( "%s (%d failure%s)\n", g_iFail == 0 ? "ALL PASS" : "FAILED", g_iFail, g_iFail == 1 ? "" : "s" );
	return g_iFail == 0 ? 0 : 1;
}
