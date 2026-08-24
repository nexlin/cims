#include "SipIpsec.h"

#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>

#include "Log.h"
#include "SipStack.h"

/** 단말측 리스너 ext id 는 1000 번대 — 응용(cspsim 등)이 쓰는 id 와 겹치지 않게 */
#define SIP_IPSEC_LISTENER_EXT_ID_BASE 1000

static std::string _trim( const std::string &s ) {
    size_t b = s.find_first_not_of( " \t" );
    if ( b == std::string::npos ) return "";
    size_t e = s.find_last_not_of( " \t" );
    return s.substr( b, e - b + 1 );
}

bool SipIpsecParseServer( const std::string &strList, std::string &strAlg, std::string &strEalg, uint32_t &iSpiC,
                          uint32_t &iSpiS, int &iPortC, int &iPortS ) {
    size_t pos = 0;
    while ( pos <= strList.size() ) {
        size_t comma = strList.find( ',', pos );
        std::string item = _trim( strList.substr( pos, comma == std::string::npos ? std::string::npos : comma - pos ) );
        pos = ( comma == std::string::npos ) ? strList.size() + 1 : comma + 1;
        if ( item.empty() ) continue;
        std::map<std::string, std::string> p;
        std::string name;
        size_t t = 0;
        bool bFirst = true;
        while ( t <= item.size() ) {
            size_t semi = item.find( ';', t );
            std::string tok = _trim( item.substr( t, semi == std::string::npos ? std::string::npos : semi - t ) );
            if ( bFirst ) {
                name = tok;
                bFirst = false;
            } else if ( !tok.empty() ) {
                size_t eq = tok.find( '=' );
                if ( eq != std::string::npos ) p[_trim( tok.substr( 0, eq ) )] = _trim( tok.substr( eq + 1 ) );
            }
            if ( semi == std::string::npos ) break;
            t = semi + 1;
        }
        if ( strcasecmp( name.c_str(), "ipsec-3gpp" ) != 0 ) continue;
        strAlg = p["alg"];
        strEalg = p.count( "ealg" ) ? p["ealg"] : XFRM_ENC_NULL;
        iSpiC = (uint32_t)strtoul( p["spi-c"].c_str(), NULL, 10 );
        iSpiS = (uint32_t)strtoul( p["spi-s"].c_str(), NULL, 10 );
        iPortC = atoi( p["port-c"].c_str() );
        iPortS = atoi( p["port-s"].c_str() );
        return iSpiC != 0 && iSpiS != 0 && iPortC > 0 && iPortS > 0;
    }
    return false;
}

static uint32_t _randomSpi() {
    uint32_t r = 0;
    for ( int i = 0; i < 8 && r < 256; ++i ) RAND_bytes( (unsigned char *)&r, sizeof( r ) );
    return r < 256 ? 0x20000000 + r : r;
}

bool CSipIpsecClient::_openPair( CSipStack *pclsStack, CSipIpsecPair &clsPair, std::string &strError ) {
    const int iBase = ( m_iPortBase > 0 ? m_iPortBase : pclsStack->m_clsSetup.m_iLocalUdpPort + 1 ) + 2 * m_iPairSeq;
    ++m_iPairSeq;
    clsPair.iPortC = iBase;
    clsPair.iPortS = iBase + 1;
    clsPair.iSpiC = _randomSpi();
    clsPair.iSpiS = _randomSpi();
    if ( clsPair.iSpiS == clsPair.iSpiC ) clsPair.iSpiS ^= 0x1;
    clsPair.iExtIdC = SIP_IPSEC_LISTENER_EXT_ID_BASE + 2 * m_iPairSeq;
    clsPair.iExtIdS = clsPair.iExtIdC + 1;
    const char *pszIp = pclsStack->m_clsSetup.m_strLocalIp.c_str();
    int iOut = 0;
    if ( !pclsStack->AddUdpListener( clsPair.iExtIdC, pszIp, clsPair.iPortC, 1, iOut ) ) {
        strError = "listener open failed port_uc=" + std::to_string( clsPair.iPortC );
        return false;
    }
    if ( !pclsStack->AddUdpListener( clsPair.iExtIdS, pszIp, clsPair.iPortS, 1, iOut ) ) {
        pclsStack->RemoveUdpListener( clsPair.iExtIdC );
        strError = "listener open failed port_us=" + std::to_string( clsPair.iPortS );
        return false;
    }
    if (m_eTransport == E_SIP_TCP) {
      // TCP (TS 33.203 §7.1): 서버 발신 연결은 port_pc → port_us 로 오므로
      // port_us 에 TCP 리스너, 단말 발신 연결은
      //   port_uc → port_ps 로 맺어야 SA 1 selector 에 걸리므로 port_uc 를 스택
      //   발신 소스 포트로 등록한다.
      if (!pclsStack->AddTcpListener(clsPair.iExtIdS, pszIp, clsPair.iPortS,
                                     iOut)) {
        pclsStack->RemoveUdpListener(clsPair.iExtIdC);
        pclsStack->RemoveUdpListener(clsPair.iExtIdS);
        strError = "tcp listener open failed port_us=" +
                   std::to_string(clsPair.iPortS);
        return false;
      }
      pclsStack->AddTcpSourcePort(clsPair.iPortC);
      clsPair.bTcp = true;
    }
    return true;
}

void CSipIpsecClient::_closePair( CSipStack *pclsStack, CSipIpsecPair &clsPair ) {
    if ( !clsPair.Valid() ) return;
    if ( pclsStack ) {
        pclsStack->RemoveUdpListener( clsPair.iExtIdC );
        pclsStack->RemoveUdpListener( clsPair.iExtIdS );
        if (clsPair.bTcp) {
          pclsStack->RemoveTcpListener(clsPair.iExtIdS);
          pclsStack->RemoveTcpSourcePort(clsPair.iPortC);
        }
    }
    clsPair = CSipIpsecPair();
}

void CSipIpsecClient::_deleteSet( CXfrmSaSet &clsSet, bool &bInstalled, const char *pszWhy ) {
    if ( !bInstalled ) return;
    std::string strError;
    if ( !CXfrmSa::Delete( clsSet, strError ) )
        CLog::Print( LOG_ERROR, "ipsec(ue): sa set delete error (%s)", strError.c_str() );
    else
        CLog::Print( LOG_INFO, "ipsec(ue): sa set released (%s) uc=%d us=%d", pszWhy, clsSet.iLocalPortC,
                     clsSet.iLocalPortS );
    bInstalled = false;
}

bool CSipIpsecClient::EnsureNext( CSipStack *pclsStack, std::string &strError ) {
    if ( !m_bEnabled ) return true;
    if ( m_clsNext.Valid() ) return true;
    if ( m_iOrigLocalPort == 0 ) m_iOrigLocalPort = pclsStack->m_clsSetup.m_iLocalUdpPort;
    if (m_iOrigLocalTcpPort == 0)
      m_iOrigLocalTcpPort = pclsStack->m_clsSetup.m_iLocalTcpPort;
    return _openPair( pclsStack, m_clsNext, strError );
}

std::string CSipIpsecClient::SecurityClient() const {
    if ( !m_clsNext.Valid() ) return "";
    char sz[256];
    snprintf( sz, sizeof( sz ), "ipsec-3gpp;alg=%s;ealg=%s;spi-c=%u;spi-s=%u;port-c=%d;port-s=%d", m_strAlg.c_str(),
              m_strEalg.c_str(), m_clsNext.iSpiC, m_clsNext.iSpiS, m_clsNext.iPortC, m_clsNext.iPortS );
    return sz;
}

bool CSipIpsecClient::OnChallenge( CSipStack *pclsStack, const std::string &strSecurityServer,
                                   const std::string &strServerIp, const std::string &strCk, const std::string &strIk,
                                   int iLifetimeSec, std::string &strError ) {
    std::string strAlg, strEalg;
    uint32_t iSpiPc = 0, iSpiPs = 0;
    int iPortPc = 0, iPortPs = 0;
    if ( !SipIpsecParseServer( strSecurityServer, strAlg, strEalg, iSpiPc, iSpiPs, iPortPc, iPortPs ) ) {
        strError = "Security-Server has no usable ipsec-3gpp";
        return false;
    }
    if ( !m_clsNext.Valid() ) {
        strError = "no proposed port pair";
        return false;
    }
    // 이전 pending(답안 도중 재챌린지)은 버린다
    _deleteSet( m_clsPendingSet, m_bPendingInstalled, "superseded" );
    _closePair( pclsStack, m_clsPending );

    CXfrmSaSet s;
    s.strLocalIp = pclsStack->m_clsSetup.m_strLocalIp;
    s.strRemoteIp = strServerIp;
    s.iLocalPortS = m_clsNext.iPortS;  // port_us
    s.iLocalPortC = m_clsNext.iPortC;  // port_uc
    s.iRemotePortS = iPortPs;
    s.iRemotePortC = iPortPc;
    s.iSpiLocalS = m_clsNext.iSpiS;
    s.iSpiLocalC = m_clsNext.iSpiC;
    s.iSpiRemoteS = iSpiPs;
    s.iSpiRemoteC = iSpiPc;
    s.strAuthAlg = strAlg;
    s.strEncAlg = strEalg;
    s.strIk = strIk;
    s.strCk = strCk;
    s.iReqId = 0x55450000 + ( m_clsNext.iPortC & 0xFFFF );  // "UE" — 단말 프로세스 소유 표식
    s.iLifetimeSec = iLifetimeSec;
    if ( !CXfrmSa::Add( s, strError ) ) return false;
    m_clsPendingSet = s;
    m_bPendingInstalled = true;
    m_clsPending = m_clsNext;
    m_clsNext = CSipIpsecPair();
    // TCP 재인증: 서버 port_ps 로의 기존 연결은 구 port_uc 에서 맺힌 것 —
    // 맵에서 떼어 답안이 새 port_uc 에서 새
    //   연결을 열게 한다 (구 연결은 구 SA 회수 뒤 수신 타임아웃으로 닫힌다).
    if (m_eTransport == E_SIP_TCP && m_bCurInstalled)
      pclsStack->m_clsTcpSocketMap.Delete(strServerIp.c_str(), iPortPs);
    CLog::Print( LOG_INFO, "ipsec(ue): sa set installed uc=%d us=%d ↔ %s ps=%d pc=%d alg=%s ealg=%s",
                 m_clsPending.iPortC, m_clsPending.iPortS, strServerIp.c_str(), iPortPs, iPortPc, strAlg.c_str(),
                 strEalg.c_str() );
    return true;
}

int CSipIpsecClient::SendPortForAnswer() const {
    return m_bPendingInstalled ? m_clsPending.iPortC : 0;
}

int CSipIpsecClient::ServerPort() const {
  if (m_bPendingInstalled)
    return m_clsPendingSet.iRemotePortS;
  if (m_bCurInstalled)
    return m_clsCurSet.iRemotePortS;
  return 0;
}

void CSipIpsecClient::OnRegistered( CSipStack *pclsStack ) {
    if ( !m_bPendingInstalled ) return;
    // 구 셋 회수 (TS 33.203 §7.4.1a — 새 셋 위 200 OK 뒤)
    _deleteSet( m_clsCurSet, m_bCurInstalled, "re-authenticated" );
    _closePair( pclsStack, m_clsCur );
    m_clsCurSet = m_clsPendingSet;
    m_bCurInstalled = true;
    m_clsCur = m_clsPending;
    m_bPendingInstalled = false;
    m_clsPending = CSipIpsecPair();
    // 이후 모든 요청은 port_uc 에서 (Via 자기주소 → UDP 는 그 리스너 소켓, TCP
    // 는 그 소스 포트의 연결, SA 1)
    pclsStack->m_clsSetup.m_iLocalUdpPort = m_clsCur.iPortC;
    if (m_clsCur.bTcp)
      pclsStack->m_clsSetup.m_iLocalTcpPort = m_clsCur.iPortC;
    CLog::Print(LOG_INFO,
                "ipsec(ue): registered over SA — identity port → %d (%s)",
                m_clsCur.iPortC, m_clsCur.bTcp ? "tcp" : "udp");
}

void CSipIpsecClient::Teardown( CSipStack *pclsStack ) {
    _deleteSet( m_clsPendingSet, m_bPendingInstalled, "teardown" );
    _deleteSet( m_clsCurSet, m_bCurInstalled, "teardown" );
    _closePair( pclsStack, m_clsPending );
    _closePair( pclsStack, m_clsCur );
    _closePair( pclsStack, m_clsNext );
    if ( pclsStack && m_iOrigLocalPort > 0 ) pclsStack->m_clsSetup.m_iLocalUdpPort = m_iOrigLocalPort;
    if (pclsStack && m_iOrigLocalTcpPort > 0)
      pclsStack->m_clsSetup.m_iLocalTcpPort = m_iOrigLocalTcpPort;
}
