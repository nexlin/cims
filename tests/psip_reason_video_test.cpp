// psip 종료 사유(Reason, RFC 3326) 전달 + 합성 SDP 영상(m=video) 협상 하네스.
//   ① B2BUA 가 종료 사유와 최종 응답 코드를 다른 leg 로 옮기려면 psip 이 수신 BYE/CANCEL/최종 응답의 Reason 을
//      응용에 올리고(EventCallEnd 3-인자), StopCall(code, reason) 이 그 값을 실어 보내야 한다.
//   ② PTT-AS 의 합성 SDP(미디어 리스트 없는 경로)가 RFC 3264 §6 을 지키는지 — answer 의 m= 수·순서 = offer,
//      쓰지 않는 video 는 port 0, 쓰는 video 는 offer 의 PT·fmtp echo, offer 는 local video 포트가 있을 때만 m=video.
//   psip UA 를 127.0.0.1 UDP 에 띄우고 원시 UDP 소켓이 단말/피어를 흉내낸다.
//
//   A. UAS: INVITE 수신 → 응용 StopCall(503, "Q.850;cause=38")           → 503 에 Reason 헤더
//   B. UAS: 통화 확립 뒤 단말 BYE + Reason                                 → EventCallEnd(200, "Q.850;cause=16")
//   C. UAS: INVITE 뒤 단말 CANCEL + Reason                                 → EventCallEnd(487, Reason 값)
//   D. UAC: 피어가 INVITE 에 503 + Reason                                  → EventCallEnd(503, Reason 값)
//   E. UAS answer: offer audio+video(96 H264)+application, local video 有  → m=video <port> RTP/AVP 96 + rtpmap/fmtp echo, 순서 유지
//   F. UAS answer: 같은 offer, local video 無                              → m=video 0 RTP/AVP 96
//   G. UAS answer: offer 에 video 없음                                     → answer 에 m=video 없음
//   H. UAC offer: local video 포트 설정                                    → INVITE 에 m=video <port> RTP/AVP 97 + a=rtpmap:97 H264/90000
//
//   빌드/실행은 verify S1-UNIT-PSIP (verify/lib/items/stage1/unit_psip.py) 가 한다 — 명령은 psip_leg_dest_test.cpp 서두와 같다.
//     build/psip_reason_video_test [--port 27080] [--verbose]

#include <arpa/inet.h>
#include <netinet/in.h>
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
static int g_iUaPort = 27080;
static int g_iFail = 0;
static int g_iSeq = 0;

#define CHECK( cond, msg )                                        \
	do {                                                           \
		if( cond ) printf( "  PASS  %s\n", msg );                  \
		else { printf( "  FAIL  %s\n", msg ); ++g_iFail; }         \
	} while( 0 )

// ── 시험용 응용 콜백 ─────────────────────────────────────────────────────────
class CTestCallBack : public ISipUserAgentCallBack
{
public:
	CSipUserAgent * m_pclsUa = NULL;
	std::mutex m_clsMutex;

	// 착신 처리 방식
	int m_iRejectCode = 0;					// >0 이면 StopCall(code, m_strRejectReason)
	std::string m_strRejectReason;
	bool m_bIgnoreIncoming = false;			// true 면 착신에 아무것도 하지 않는다(벨 울림 유지 — CANCEL 시험)
	int m_iLocalVideoPort = -1;				// AcceptCall 의 m_iVideoPort
	int m_iLocalAppPort = -1;				// AcceptCall 의 m_iApplicationPort

	// 관측
	std::atomic<int> m_iEnded{ 0 };
	int m_iEndStatus = 0;
	std::string m_strEndReason;
	std::string m_strLastCallId;

	void EventRegister( CSipServerInfo *, int ) override {}
	void EventIncomingCall( const char * pszCallId, const char *, const char *, CSipCallRtp *, CSipMessage * ) override
	{
		{
			std::lock_guard<std::mutex> lk( m_clsMutex );
			m_strLastCallId = pszCallId;
		}
		if( m_bIgnoreIncoming ) return;
		if( m_iRejectCode > 0 )
		{
			m_pclsUa->StopCall( pszCallId, m_iRejectCode, m_strRejectReason.empty() ? NULL : m_strRejectReason.c_str() );
			return;
		}
		CSipCallRtp clsLocal;
		clsLocal.m_strIp = UA_IP;
		clsLocal.m_iPort = 40000;
		clsLocal.m_iCodec = 0;
		clsLocal.m_iVideoPort = m_iLocalVideoPort;
		clsLocal.m_iApplicationPort = m_iLocalAppPort;
		m_pclsUa->AcceptCall( pszCallId, &clsLocal );
	}
	void EventCallRing( const char *, int, CSipCallRtp * ) override {}
	void EventCallStart( const char *, CSipCallRtp * ) override {}
	void EventCallEnd( const char *, int ) override {}
	void EventCallEnd( const char * pszCallId, int iSipStatus, const char * pszReason ) override
	{
		std::lock_guard<std::mutex> lk( m_clsMutex );
		m_strLastCallId = pszCallId;
		m_iEndStatus = iSipStatus;
		m_strEndReason = pszReason ? pszReason : "";
		++m_iEnded;
	}
	void Reset( )
	{
		std::lock_guard<std::mutex> lk( m_clsMutex );
		m_iEnded = 0; m_iEndStatus = 0; m_strEndReason.clear(); m_strLastCallId.clear();
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

static void UdpSendTo( int fd, const std::string & strBuf )
{
	sockaddr_in a; memset( &a, 0, sizeof(a) );
	a.sin_family = AF_INET; a.sin_addr.s_addr = inet_addr( UA_IP ); a.sin_port = htons( g_iUaPort );
	sendto( fd, strBuf.data(), strBuf.size(), 0, (sockaddr *)&a, sizeof(a) );
}

/** 응답을 iWaitMs 안에 받는다. 받으면 문자열, 못 받으면 빈 문자열. */
static std::string UdpRecvWait( int fd, int iWaitMs )
{
	char szBuf[8192];
	timeval tv; tv.tv_sec = iWaitMs / 1000; tv.tv_usec = ( iWaitMs % 1000 ) * 1000;
	setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv) );
	ssize_t n = recv( fd, szBuf, sizeof(szBuf) - 1, 0 );
	if( n <= 0 ) return "";
	return std::string( szBuf, (size_t)n );
}

/** 원하는 시작 문자열(예 "SIP/2.0 503")의 메시지를 받을 때까지 읽는다 (100 Trying 등은 건너뛴다). */
static std::string UdpRecvUntil( int fd, const char * pszPrefix, int iWaitMs )
{
	for( int i = 0; i < 10; ++i )
	{
		std::string s = UdpRecvWait( fd, iWaitMs );
		if( s.empty() ) return "";
		if( s.compare( 0, strlen( pszPrefix ), pszPrefix ) == 0 ) return s;
	}
	return "";
}

static std::string HeaderOf( const std::string & strMsg, const char * pszName )
{
	std::string key = std::string( "\r\n" ) + pszName + ":";
	size_t p = strMsg.find( key );
	if( p == std::string::npos ) return "";
	p += key.size();
	size_t e = strMsg.find( "\r\n", p );
	std::string v = strMsg.substr( p, e - p );
	while( !v.empty() && v[0] == ' ' ) v.erase( 0, 1 );
	return v;
}

static std::string BodyOf( const std::string & strMsg )
{
	size_t p = strMsg.find( "\r\n\r\n" );
	return p == std::string::npos ? "" : strMsg.substr( p + 4 );
}

static const char * SDP_AUDIO_ONLY =
	"v=0\r\no=ue 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
	"m=audio 40002 RTP/AVP 0 101\r\na=rtpmap:0 PCMU/8000\r\na=rtpmap:101 telephone-event/8000\r\n";
static const char * SDP_AUDIO_VIDEO_APP =
	"v=0\r\no=ue 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
	"m=audio 40002 RTP/AVP 0 101\r\na=rtpmap:0 PCMU/8000\r\na=rtpmap:101 telephone-event/8000\r\n"
	"m=video 40004 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 profile-level-id=42e01f;packetization-mode=1\r\n"
	"m=application 40006 UDP MCPTT\r\na=fmtp:MCPTT mc_queueing\r\n";

static std::string BuildInvite( int iUePort, const std::string & strCallId, const char * pszSdp )
{
	char szInvite[4096];
	int iLen = snprintf( szInvite, sizeof(szInvite),
		"INVITE sip:svc@%s:%d SIP/2.0\r\n"
		"Via: SIP/2.0/UDP %s:%d;rport;branch=z9hG4bK-rv-%d\r\n"
		"Max-Forwards: 70\r\n"
		"From: <sip:ue@test.local>;tag=ue-%d\r\n"
		"To: <sip:svc@test.local>\r\n"
		"Call-ID: %s\r\n"
		"CSeq: 1 INVITE\r\n"
		"Contact: <sip:ue@%s:%d>\r\n"
		"Content-Type: application/sdp\r\n"
		"Content-Length: %d\r\n\r\n%s",
		UA_IP, g_iUaPort, UA_IP, iUePort, ++g_iSeq, g_iSeq, strCallId.c_str(), UA_IP, iUePort,
		(int)strlen( pszSdp ), pszSdp );
	return std::string( szInvite, iLen );
}

/** 수신 최종 응답에 대한 ACK (RFC 3261 §17.1.1.3 — 같은 branch, To tag 포함). */
static std::string BuildAck( const std::string & strInvite, const std::string & strFinal )
{
	std::string strMsg = "\r\n" + strInvite;
	std::string strRsp = "\r\n" + strFinal;
	char szAck[2048];
	int iLen = snprintf( szAck, sizeof(szAck),
		"ACK sip:svc@%s:%d SIP/2.0\r\nVia: %s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n",
		UA_IP, g_iUaPort, HeaderOf( strMsg, "Via" ).c_str(), HeaderOf( strMsg, "From" ).c_str(), HeaderOf( strRsp, "To" ).c_str(),
		HeaderOf( strMsg, "Call-ID" ).c_str() );
	return std::string( szAck, iLen );
}

/** 확립된 다이얼로그의 BYE/CANCEL — 200 OK 의 To tag 사용(BYE), INVITE 헤더 그대로(CANCEL). */
static std::string BuildInDialog( const char * pszMethod, const std::string & strInvite, const std::string & strFinal,
	int iCSeq, const char * pszReason )
{
	std::string strMsg = "\r\n" + strInvite;
	std::string strRsp = strFinal.empty() ? "" : "\r\n" + strFinal;
	const std::string strTo = strFinal.empty() ? HeaderOf( strMsg, "To" ) : HeaderOf( strRsp, "To" );
	const std::string strVia = strFinal.empty() ? HeaderOf( strMsg, "Via" ) : "";
	char szVia[256];
	if( strFinal.empty() ) snprintf( szVia, sizeof(szVia), "%s", strVia.c_str() );
	else snprintf( szVia, sizeof(szVia), "SIP/2.0/UDP 127.0.0.1;rport;branch=z9hG4bK-rv-%s-%d", pszMethod, ++g_iSeq );
	char szReq[2048];
	int iLen = snprintf( szReq, sizeof(szReq),
		"%s sip:svc@%s:%d SIP/2.0\r\nVia: %s\r\nMax-Forwards: 70\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %d %s\r\n"
		"%s%s%sContent-Length: 0\r\n\r\n",
		pszMethod, UA_IP, g_iUaPort, szVia, HeaderOf( strMsg, "From" ).c_str(), strTo.c_str(),
		HeaderOf( strMsg, "Call-ID" ).c_str(), iCSeq, pszMethod,
		pszReason ? "Reason: " : "", pszReason ? pszReason : "", pszReason ? "\r\n" : "" );
	return std::string( szReq, iLen );
}

/** 앞 시험이 남긴 메시지(ACK 등)를 비운다. */
static void UdpDrain( int fd )
{
	while( !UdpRecvWait( fd, 150 ).empty() ) {}
}

/** 단말: INVITE → 최종 응답(2xx 는 ACK) 을 돌려준다. */
static std::string UeInvite( int fdUe, int iUePort, const std::string & strCallId, const char * pszSdp, std::string & strInviteOut )
{
	UdpDrain( fdUe );
	strInviteOut = BuildInvite( iUePort, strCallId, pszSdp );
	UdpSendTo( fdUe, strInviteOut );
	std::string strFinal;
	for( int i = 0; i < 10; ++i )
	{
		std::string s = UdpRecvWait( fdUe, 2000 );
		if( s.empty() ) break;
		if( s.compare( 0, 11, "SIP/2.0 100" ) == 0 || s.compare( 0, 11, "SIP/2.0 180" ) == 0 ) continue;
		strFinal = s;
		break;
	}
	if( strFinal.compare( 0, 9, "SIP/2.0 2" ) == 0 ) UdpSendTo( fdUe, BuildAck( strInviteOut, strFinal ) );
	return strFinal;
}

static bool WaitEnded( CTestCallBack & cb, int iWaitMs )
{
	for( int i = 0; i < iWaitMs / 50; ++i )
	{
		if( cb.m_iEnded > 0 ) return true;
		usleep( 50 * 1000 );
	}
	return cb.m_iEnded > 0;
}

/** SDP 본문의 m= 라인 순서를 "audio,video,application" 처럼 이어 붙인다. */
static std::string MediaOrder( const std::string & strBody )
{
	std::string out;
	size_t p = 0;
	while( ( p = strBody.find( "m=", p ) ) != std::string::npos )
	{
		if( p != 0 && strBody[p - 1] != '\n' ) { p += 2; continue; }
		size_t e = strBody.find( ' ', p );
		if( !out.empty() ) out += ",";
		out += strBody.substr( p + 2, e - p - 2 );
		p = e;
	}
	return out;
}

int main( int argc, char * argv[] )
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

	CSipUserAgent clsUa;
	CTestCallBack clsCb;
	clsCb.m_pclsUa = &clsUa;
	if( clsUa.Start( clsSetup, &clsCb ) == false ) { printf( "UA start failed (port %d in use?)\n", g_iUaPort ); return 2; }
	usleep( 300 * 1000 );

	int iUePort = 0;
	int fdUe = UdpOpen( iUePort );
	if( fdUe < 0 ) { printf( "socket open failed\n" ); clsUa.Stop(); return 2; }

	// ── A. 응용의 거절에 Reason 을 싣는다 ──
	printf( "[A] StopCall(503, Reason) → 최종 응답에 Reason 헤더\n" );
	{
		clsCb.Reset();
		clsCb.m_iRejectCode = 503;
		clsCb.m_strRejectReason = "Q.850;cause=38;text=\"Network out of order\"";
		std::string strInvite;
		std::string strFinal = UeInvite( fdUe, iUePort, "rv-a@test.local", SDP_AUDIO_ONLY, strInvite );
		CHECK( strFinal.compare( 0, 11, "SIP/2.0 503" ) == 0, "503 수신" );
		CHECK( HeaderOf( "\r\n" + strFinal, "Reason" ).find( "Q.850;cause=38" ) == 0, "Reason 헤더가 그대로 실렸다" );
		if( !strFinal.empty() ) UdpSendTo( fdUe, BuildAck( strInvite, strFinal ) );
		clsCb.m_iRejectCode = 0;
		clsCb.m_strRejectReason.clear();
	}

	// ── B. 확립된 통화의 BYE + Reason → EventCallEnd(200, reason) ──
	printf( "[B] 단말 BYE + Reason → 응용에 사유 전달\n" );
	{
		clsCb.Reset();
		std::string strInvite;
		std::string strFinal = UeInvite( fdUe, iUePort, "rv-b@test.local", SDP_AUDIO_ONLY, strInvite );
		CHECK( strFinal.compare( 0, 11, "SIP/2.0 200" ) == 0, "통화 확립(200 OK)" );
		UdpSendTo( fdUe, BuildInDialog( "BYE", strInvite, strFinal, 2, "Q.850;cause=16;text=\"Normal call clearing\"" ) );
		std::string strByeRsp = UdpRecvUntil( fdUe, "SIP/2.0 200", 2000 );
		CHECK( !strByeRsp.empty(), "BYE 200 OK" );
		CHECK( WaitEnded( clsCb, 2000 ) && clsCb.m_iEndStatus == 200, "EventCallEnd(200)" );
		CHECK( clsCb.m_strEndReason.find( "Q.850;cause=16" ) == 0, "Reason 값이 콜백에 올라온다" );
	}

	// ── C. CANCEL + Reason → EventCallEnd(487, reason) ──
	printf( "[C] 단말 CANCEL + Reason → EventCallEnd(487, Reason)\n" );
	{
		clsCb.Reset();
		clsCb.m_bIgnoreIncoming = true;	// 응용이 응답하지 않는다(벨 울림 상태 유지)
		std::string strInvite = BuildInvite( iUePort, "rv-c@test.local", SDP_AUDIO_ONLY );
		UdpDrain( fdUe );
		UdpSendTo( fdUe, strInvite );
		std::string s100 = UdpRecvUntil( fdUe, "SIP/2.0 100", 2000 );
		CHECK( !s100.empty(), "100 Trying" );
		UdpSendTo( fdUe, BuildInDialog( "CANCEL", strInvite, "", 1, "SIP;cause=200;text=\"Call completed elsewhere\"" ) );
		std::string s487 = UdpRecvUntil( fdUe, "SIP/2.0 487", 2000 );
		CHECK( !s487.empty(), "INVITE 487" );
		if( !s487.empty() ) UdpSendTo( fdUe, BuildAck( strInvite, s487 ) );
		CHECK( WaitEnded( clsCb, 2000 ) && clsCb.m_iEndStatus == 487, "EventCallEnd(487)" );
		CHECK( clsCb.m_strEndReason.find( "SIP;cause=200" ) == 0, "CANCEL 의 Reason 값이 콜백에 올라온다" );
		clsCb.m_bIgnoreIncoming = false;
	}

	// ── D. UAC: 피어의 503 + Reason → EventCallEnd(503, reason) ──
	printf( "[D] 발신 INVITE 에 피어 503 + Reason → EventCallEnd(503, Reason)\n" );
	{
		clsCb.Reset();
		CSipCallRtp clsRtp;
		clsRtp.m_strIp = UA_IP; clsRtp.m_iPort = 40000; clsRtp.m_iCodec = 0;
		CSipCallRoute clsRoute;
		clsRoute.m_strDestIp = UA_IP; clsRoute.m_iDestPort = iUePort; clsRoute.m_eTransport = E_SIP_UDP;
		std::string strCallId;
		UdpDrain( fdUe );
		CHECK( clsUa.StartCall( "svc", "peer", &clsRtp, &clsRoute, strCallId ), "StartCall" );
		std::string strInv = UdpRecvUntil( fdUe, "INVITE", 2000 );
		CHECK( !strInv.empty(), "피어가 INVITE 수신" );
		if( !strInv.empty() )
		{
			std::string strMsg = "\r\n" + strInv;
			char szRsp[2048];
			int iLen = snprintf( szRsp, sizeof(szRsp),
				"SIP/2.0 503 Service Unavailable\r\nVia: %s\r\nFrom: %s\r\nTo: %s;tag=peer-1\r\nCall-ID: %s\r\nCSeq: %s\r\n"
				"Reason: Q.850;cause=41;text=\"Temporary failure\"\r\nContent-Length: 0\r\n\r\n",
				HeaderOf( strMsg, "Via" ).c_str(), HeaderOf( strMsg, "From" ).c_str(), HeaderOf( strMsg, "To" ).c_str(),
				HeaderOf( strMsg, "Call-ID" ).c_str(), HeaderOf( strMsg, "CSeq" ).c_str() );
			UdpSendTo( fdUe, std::string( szRsp, iLen ) );
		}
		CHECK( WaitEnded( clsCb, 2000 ) && clsCb.m_iEndStatus == 503, "EventCallEnd(503)" );
		CHECK( clsCb.m_strEndReason.find( "Q.850;cause=41" ) == 0, "응답의 Reason 값이 콜백에 올라온다" );
	}

	// ── E. answer: video 수락 (PT/fmtp echo, 순서 유지) ──
	printf( "[E] offer audio+video+application, local video 有 → m=video 수락\n" );
	{
		clsCb.Reset();
		clsCb.m_iLocalVideoPort = 40014;
		clsCb.m_iLocalAppPort = 40016;
		std::string strInvite;
		std::string strFinal = UeInvite( fdUe, iUePort, "rv-e@test.local", SDP_AUDIO_VIDEO_APP, strInvite );
		std::string strBody = BodyOf( strFinal );
		CHECK( strFinal.compare( 0, 11, "SIP/2.0 200" ) == 0, "200 OK" );
		CHECK( MediaOrder( strBody ) == "audio,video,application", ( "m= 순서 = offer (" + MediaOrder( strBody ) + ")" ).c_str() );
		CHECK( strBody.find( "m=video 40014 RTP/AVP 96\r\n" ) != std::string::npos, "m=video 로컬 포트 + offer PT 96 echo" );
		CHECK( strBody.find( "a=rtpmap:96 H264/90000\r\n" ) != std::string::npos, "rtpmap echo" );
		CHECK( strBody.find( "a=fmtp:96 profile-level-id=42e01f;packetization-mode=1\r\n" ) != std::string::npos, "fmtp 는 offer 값 echo" );
		CHECK( strBody.find( "m=application 40016 UDP MCPTT\r\n" ) != std::string::npos, "floor 라인 유지" );
		UdpSendTo( fdUe, BuildInDialog( "BYE", strInvite, strFinal, 2, NULL ) );
		UdpRecvUntil( fdUe, "SIP/2.0 200", 1000 );
	}

	// ── F. answer: local video 없음 → port 0 거절 ──
	printf( "[F] 같은 offer, local video 無 → m=video 0\n" );
	{
		clsCb.Reset();
		clsCb.m_iLocalVideoPort = -1;
		clsCb.m_iLocalAppPort = 40016;
		std::string strInvite;
		std::string strFinal = UeInvite( fdUe, iUePort, "rv-f@test.local", SDP_AUDIO_VIDEO_APP, strInvite );
		std::string strBody = BodyOf( strFinal );
		CHECK( MediaOrder( strBody ) == "audio,video,application", "m= 순서 = offer" );
		CHECK( strBody.find( "m=video 0 RTP/AVP 96\r\n" ) != std::string::npos, "m=video 0 (거절, 라인 유지)" );
		UdpSendTo( fdUe, BuildInDialog( "BYE", strInvite, strFinal, 2, NULL ) );
		UdpRecvUntil( fdUe, "SIP/2.0 200", 1000 );
	}

	// ── G. answer: offer 에 video 없음 → answer 도 없음 ──
	printf( "[G] offer 에 video 없음, local video 有 → answer 에 m=video 없음\n" );
	{
		clsCb.Reset();
		clsCb.m_iLocalVideoPort = 40014;
		clsCb.m_iLocalAppPort = -1;
		std::string strInvite;
		std::string strFinal = UeInvite( fdUe, iUePort, "rv-g@test.local", SDP_AUDIO_ONLY, strInvite );
		std::string strBody = BodyOf( strFinal );
		CHECK( MediaOrder( strBody ) == "audio", ( "m= 는 audio 만 (" + MediaOrder( strBody ) + ")" ).c_str() );
		UdpSendTo( fdUe, BuildInDialog( "BYE", strInvite, strFinal, 2, NULL ) );
		UdpRecvUntil( fdUe, "SIP/2.0 200", 1000 );
		clsCb.m_iLocalVideoPort = -1;
	}

	// ── H. offer: local video 포트 → INVITE 에 m=video ──
	printf( "[H] 발신 offer 에 local video → m=video RTP/AVP 97 H264\n" );
	{
		clsCb.Reset();
		CSipCallRtp clsRtp;
		clsRtp.m_strIp = UA_IP; clsRtp.m_iPort = 40020; clsRtp.m_iCodec = 0;
		clsRtp.m_clsCodecList.push_back( 0 );
		clsRtp.m_iVideoPort = 40022;
		CSipCallRoute clsRoute;
		clsRoute.m_strDestIp = UA_IP; clsRoute.m_iDestPort = iUePort; clsRoute.m_eTransport = E_SIP_UDP;
		std::string strCallId;
		UdpDrain( fdUe );
		CHECK( clsUa.StartCall( "svc", "peer", &clsRtp, &clsRoute, strCallId ), "StartCall" );
		std::string strInv = UdpRecvUntil( fdUe, "INVITE", 2000 );
		std::string strBody = BodyOf( strInv );
		CHECK( MediaOrder( strBody ) == "audio,video", ( "offer m= = audio,video (" + MediaOrder( strBody ) + ")" ).c_str() );
		CHECK( strBody.find( "m=video 40022 RTP/AVP 97\r\n" ) != std::string::npos, "m=video 로컬 포트, PT 97" );
		CHECK( strBody.find( "a=rtpmap:97 H264/90000\r\n" ) != std::string::npos, "a=rtpmap:97 H264/90000" );
		clsUa.StopCall( strCallId.c_str() );
		UdpRecvUntil( fdUe, "CANCEL", 1000 );
	}

	close( fdUe );
	clsUa.Stop();
	printf( "%s (%d failure%s)\n", g_iFail ? "FAIL" : "PASS", g_iFail, g_iFail == 1 ? "" : "s" );
	return g_iFail ? 1 : 0;
}
