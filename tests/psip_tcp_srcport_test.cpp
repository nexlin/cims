// psip TCP 발신 소스포트 bind 하네스 — IMS AKA+IPsec 보호 포트쌍 위 TCP (sip_access_security.md §8.3, TS 33.203 §7.1).
//   TcpConnectFrom(srcIp, srcPort, ...) 이 지정 소스 포트에서 connect 하는지, 같은 소스 포트에서 다른 목적지로
//   연속 connect 가 되는지(SO_REUSEADDR), 리스너와 같은 포트를 소스로 쓸 수 있는지를 루프백으로 본다.
//   빌드(csp 빌드 뒤 — psip 정적 라이브러리 사용): g++ -std=c++17 -I ext/psip/SipPlatform tests/psip_tcp_srcport_test.cpp \
//         build/csp/psip_build/libSipPlatform.a build/csp/psip_build/libServerPlatform.a -lpthread \
//         -o build/psip_tcp_srcport_test
//   실행: build/psip_tcp_srcport_test
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

#include "SipTcp.h"

static int g_fail = 0;
#define CHECK( name, cond )                                        \
    do {                                                           \
        if ( !( cond ) ) {                                         \
            printf( "FAIL %s (%s:%d)\n", name, __FILE__, __LINE__ ); \
            ++g_fail;                                              \
        } else {                                                   \
            printf( "ok   %s\n", name );                           \
        }                                                          \
    } while ( 0 )

static int LocalPortOf( Socket fd ) {
    std::string strIp;
    int iPort = 0;
    return GetLocalIpPort( fd, strIp, iPort ) ? iPort : -1;
}

int main() {
    const char *pszIp = "127.0.0.1";
    const int iSrc = 47011;  // 단말 port_uc 역할
    Socket hListenA = TcpListen( 47021, 5, pszIp );  // 서버 port_ps 역할
    Socket hListenB = TcpListen( 47022, 5, pszIp );  // 다른 접속점
    CHECK( "listen A/B", hListenA != INVALID_SOCKET && hListenB != INVALID_SOCKET );

    // 1) 소스 포트 지정 connect → 로컬 포트 = 지정값
    Socket hC1 = TcpConnectFrom( pszIp, iSrc, pszIp, 47021, 2 );
    CHECK( "connect from src port", hC1 != INVALID_SOCKET );
    CHECK( "local port == src port", LocalPortOf( hC1 ) == iSrc );
    char szPeer[64];
    int iPeerPort = 0;
    Socket hA1 = TcpAccept( hListenA, szPeer, sizeof( szPeer ), &iPeerPort );
    CHECK( "server sees src port", hA1 != INVALID_SOCKET && iPeerPort == iSrc );

    // 2) 같은 소스 포트에서 다른 목적지로 연속 connect (SO_REUSEADDR — 4-tuple 이 다르면 허용)
    Socket hC2 = TcpConnectFrom( pszIp, iSrc, pszIp, 47022, 2 );
    CHECK( "second connect same src port, other dst", hC2 != INVALID_SOCKET && LocalPortOf( hC2 ) == iSrc );
    Socket hB1 = TcpAccept( hListenB, szPeer, sizeof( szPeer ), &iPeerPort );
    CHECK( "server B sees src port", hB1 != INVALID_SOCKET && iPeerPort == iSrc );

    // 3) 소스 IP 만(포트 0) — 종전 동작 유지 (OS 자동 포트)
    Socket hC3 = TcpConnectFrom( pszIp, 0, pszIp, 47021, 2 );
    CHECK( "connect with src ip only", hC3 != INVALID_SOCKET && LocalPortOf( hC3 ) > 0 && LocalPortOf( hC3 ) != iSrc );
    Socket hA2 = TcpAccept( hListenA, szPeer, sizeof( szPeer ), &iPeerPort );

    // 4) 같은 포트에 UDP 리스너가 있어도 TCP 소스 포트로 쓸 수 있다 — 서버 port_pc / 단말 port_uc 의 실제 모양
    //    (UDP 리스너 + TCP 발신). TCP LISTEN 소켓이 있는 포트는 Linux 가 SO_REUSEADDR 로도 bind 를 거부하므로
    //    그 조합(port_ps/port_us 에서 발신)은 설계상 쓰지 않는다.
    Socket hUdp = UdpListen( 47031, pszIp );
    Socket hC4 = TcpConnectFrom( pszIp, 47031, pszIp, 47022, 2 );
    CHECK( "src port shared with own udp listener", hUdp != INVALID_SOCKET && hC4 != INVALID_SOCKET &&
                                                        LocalPortOf( hC4 ) == 47031 );
    Socket hB2 = hC4 != INVALID_SOCKET ? TcpAccept( hListenB, szPeer, sizeof( szPeer ), &iPeerPort ) : INVALID_SOCKET;
    Socket hListenS = hUdp;

    // 5) 4-arg 호환 시그니처
    Socket hC5 = TcpConnectFrom( pszIp, pszIp, 47021, 2 );
    CHECK( "legacy 4-arg TcpConnectFrom", hC5 != INVALID_SOCKET );
    Socket hA3 = TcpAccept( hListenA, szPeer, sizeof( szPeer ), &iPeerPort );

    Socket arr[] = { hC1, hC2, hC3, hC4, hC5, hA1, hA2, hA3, hB1, hB2, hListenA, hListenB, hListenS };
    for ( Socket h : arr )
        if ( h != INVALID_SOCKET ) close( h );
    printf( "%s (%d fail)\n", g_fail ? "FAILED" : "ALL PASS", g_fail );
    return g_fail ? 1 : 0;
}
