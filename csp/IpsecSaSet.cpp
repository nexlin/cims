#include "IpsecSaSet.h"

#include <openssl/rand.h>

#include "CspLocalNodeMap.h"
#include "Log.h"
#include "SecAgree.h"
#include "SipServerSetup.h"

CIpsecSaSetMap gclsIpsecSaSetMap;

/** IPsec 접속점의 SA 로컬 주소 — bind_ip 가 any 면 advertised primary IP */
static std::string _localSaIp( const LocalNodeInfo &n ) {
    if ( n.bind_ip.empty() || n.bind_ip == "0.0.0.0" ) return gclsSetup.m_strLocalIp;
    return n.bind_ip;
}

void CIpsecSaSetMap::Init() {
    m_bAvailable = false;
    m_iNextReqId = gclsSetup.m_iIpsecReqIdBase;
    LocalNodeInfo n = gclsLocalNodeMap.GetIpsecNode();
    if ( !n.IsValid() ) {
        CLog::Print( LOG_INFO, "ipsec: no IPSEC local node — ipsec-3gpp is not offered" );
        return;
    }
    std::string strError;
    // 이전 프로세스가 남긴 state/policy 회수 — reqid 범위가 소유 표식이다
    const int iFlushed =
        CXfrmSa::FlushByReqId( gclsSetup.m_iIpsecReqIdBase, gclsSetup.m_iIpsecReqIdBase + 0xFFFF, strError );
    if ( iFlushed < 0 ) {
        CLog::Print( LOG_ERROR, "ipsec: XFRM flush failed (%s) — CAP_NET_ADMIN? ipsec-3gpp is not offered",
                     strError.c_str() );
        return;
    }
    if ( iFlushed > 0 ) CLog::Print( LOG_SYSTEM, "ipsec: flushed %d stale SA(s) from previous run", iFlushed );
    if ( !CXfrmSa::SelfCheck( gclsSetup.m_iIpsecReqIdBase + 0xFFFF, strError ) ) {
        CLog::Print( LOG_ERROR, "ipsec: XFRM self-check failed (%s) — CAP_NET_ADMIN? ipsec-3gpp is not offered",
                     strError.c_str() );
        return;
    }
    m_bAvailable = true;
    CLog::Print( LOG_SYSTEM, "ipsec: available — node '%s' port_ps=%d port_pc=%d spi=[0x%x,0x%x] reqid base=0x%x",
                 n.name.c_str(), n.bind_port, n.client_port, gclsSetup.m_iIpsecSpiMin, gclsSetup.m_iIpsecSpiMax,
                 gclsSetup.m_iIpsecReqIdBase );
}

void CIpsecSaSetMap::Shutdown() {
    if ( !m_bAvailable ) return;
    std::string strError;
    const int n = CXfrmSa::FlushByReqId( gclsSetup.m_iIpsecReqIdBase, gclsSetup.m_iIpsecReqIdBase + 0xFFFF, strError );
    CLog::Print( LOG_SYSTEM, "ipsec: shutdown — %d SA(s) flushed%s%s", n < 0 ? 0 : n, n < 0 ? " (error: " : "",
                 n < 0 ? strError.c_str() : "" );
    m_clsMutex.acquire();
    m_clsMap.clear();
    m_clsSpiInUse.clear();
    m_clsMutex.release();
}

uint32_t CIpsecSaSetMap::_allocReqIdLocked() {
    // 하위 16 비트를 순환 — 자기점검용 +0xFFFF 는 비워 둔다
    for ( int i = 0; i < 0xFFFF; ++i ) {
        uint32_t r = gclsSetup.m_iIpsecReqIdBase + ( ( m_iNextReqId - gclsSetup.m_iIpsecReqIdBase + 1 + i ) % 0xFFFF );
        if ( r == gclsSetup.m_iIpsecReqIdBase ) continue;  // 0 오프셋은 쓰지 않는다
        if ( m_clsMap.find( r ) == m_clsMap.end() ) {
            m_iNextReqId = r;
            return r;
        }
    }
    return 0;
}

uint32_t CIpsecSaSetMap::_allocSpiLocked() {
    const uint32_t iMin = gclsSetup.m_iIpsecSpiMin, iMax = gclsSetup.m_iIpsecSpiMax;
    if ( iMax <= iMin ) return 0;
    for ( int i = 0; i < 64; ++i ) {
        uint32_t r = 0;
        RAND_bytes( (unsigned char *)&r, sizeof( r ) );
        r = iMin + ( r % ( iMax - iMin + 1 ) );
        if ( r != 0 && m_clsSpiInUse.find( r ) == m_clsSpiInUse.end() ) {
            m_clsSpiInUse.insert( r );
            return r;
        }
    }
    return 0;
}

void CIpsecSaSetMap::_eraseLocked( std::map<uint32_t, CIpsecSaSetInfo>::iterator it, const char *pszWhy ) {
    std::string strError;
    if ( !CXfrmSa::Delete( it->second.clsSet, strError ) )
        CLog::Print( LOG_ERROR, "ipsec: sa set delete error user=%s reqid=0x%x (%s)", it->second.strUser.c_str(),
                     it->first, strError.c_str() );
    CLog::Print( LOG_INFO, "ipsec: sa set released user=%s reqid=0x%x (%s)", it->second.strUser.c_str(), it->first,
                 pszWhy );
    m_clsSpiInUse.erase( it->second.clsSet.iSpiLocalS );
    m_clsSpiInUse.erase( it->second.clsSet.iSpiLocalC );
    m_clsMap.erase( it );
}

bool CIpsecSaSetMap::CreateTemp( const std::string &strUser, const SecAgreeIpsecOffer &clsOffer,
                                 const std::string &strUeIp, const std::string &strIk, const std::string &strCk,
                                 CIpsecSaSetInfo &clsOut, std::string &strError ) {
    if ( !m_bAvailable ) {
        strError = "ipsec unavailable";
        return false;
    }
    LocalNodeInfo n = gclsLocalNodeMap.GetIpsecNode();
    if ( !n.IsValid() ) {
        strError = "no IPSEC local node";
        return false;
    }

    m_clsMutex.acquire();
    // 같은 user 의 기존 임시 셋은 교체한다 (재챌린지)
    for ( std::map<uint32_t, CIpsecSaSetInfo>::iterator it = m_clsMap.begin(); it != m_clsMap.end(); ) {
        if ( it->second.strUser == strUser && !it->second.bEstablished ) {
            std::map<uint32_t, CIpsecSaSetInfo>::iterator itDel = it++;
            _eraseLocked( itDel, "replaced by new challenge" );
        } else {
            ++it;
        }
    }
    CIpsecSaSetInfo info;
    info.iReqId = _allocReqIdLocked();
    info.strUser = strUser;
    info.clsSet.strLocalIp = _localSaIp( n );
    info.clsSet.strRemoteIp = strUeIp;
    info.clsSet.iLocalPortS = n.bind_port;
    info.clsSet.iLocalPortC = n.client_port;
    info.clsSet.iRemotePortC = clsOffer.iPortC;
    info.clsSet.iRemotePortS = clsOffer.iPortS;
    info.clsSet.iSpiLocalS = _allocSpiLocked();
    info.clsSet.iSpiLocalC = _allocSpiLocked();
    info.clsSet.iSpiRemoteC = clsOffer.iSpiC;
    info.clsSet.iSpiRemoteS = clsOffer.iSpiS;
    info.clsSet.strAuthAlg = clsOffer.strAlg;
    info.clsSet.strEncAlg = clsOffer.strEalg;
    info.clsSet.strIk = strIk;
    info.clsSet.strCk = strCk;
    info.clsSet.iReqId = info.iReqId;
    info.clsSet.iLifetimeSec = gclsSetup.m_iIpsecTempSaTimeoutSec + 5;  // 커널 이중 안전장치
    time( &info.iCreateTime );
    if ( info.iReqId == 0 || info.clsSet.iSpiLocalS == 0 || info.clsSet.iSpiLocalC == 0 ) {
        m_clsSpiInUse.erase( info.clsSet.iSpiLocalS );
        m_clsSpiInUse.erase( info.clsSet.iSpiLocalC );
        m_clsMutex.release();
        strError = "reqid/spi exhausted";
        return false;
    }
    if ( !CXfrmSa::Add( info.clsSet, strError ) ) {
        m_clsSpiInUse.erase( info.clsSet.iSpiLocalS );
        m_clsSpiInUse.erase( info.clsSet.iSpiLocalC );
        m_clsMutex.release();
        return false;
    }
    m_clsMap[info.iReqId] = info;
    clsOut = info;
    m_clsMutex.release();
    CLog::Print( LOG_INFO,
                 "ipsec: temp sa set user=%s reqid=0x%x ue=%s uc=%d us=%d spi_uc=0x%x spi_us=0x%x "
                 "ps=%d pc=%d spi_ps=0x%x spi_pc=0x%x alg=%s ealg=%s",
                 strUser.c_str(), info.iReqId, strUeIp.c_str(), clsOffer.iPortC, clsOffer.iPortS, clsOffer.iSpiC,
                 clsOffer.iSpiS, n.bind_port, n.client_port, info.clsSet.iSpiLocalS, info.clsSet.iSpiLocalC,
                 clsOffer.strAlg.c_str(), clsOffer.strEalg.c_str() );
    return true;
}

bool CIpsecSaSetMap::MatchTemp( const std::string &strUser, const std::string &strIp, int iPort,
                                CIpsecSaSetInfo *pclsOut ) {
    bool bRes = false;
    m_clsMutex.acquire();
    for ( std::map<uint32_t, CIpsecSaSetInfo>::iterator it = m_clsMap.begin(); it != m_clsMap.end(); ++it ) {
        const CIpsecSaSetInfo &s = it->second;
        if ( s.bEstablished || s.strUser != strUser ) continue;
        if ( s.clsSet.strRemoteIp == strIp && s.clsSet.iRemotePortC == iPort ) {
            if ( pclsOut ) *pclsOut = s;
            bRes = true;
            break;
        }
    }
    m_clsMutex.release();
    return bRes;
}

bool CIpsecSaSetMap::Establish( const std::string &strUser, uint32_t iReqId, int iLifetimeSec ) {
    bool bRes = false;
    m_clsMutex.acquire();
    std::map<uint32_t, CIpsecSaSetInfo>::iterator it = m_clsMap.find( iReqId );
    if ( it != m_clsMap.end() && it->second.strUser == strUser && !it->second.bEstablished ) {
        // 기존 확정 셋은 retiring — 새 셋 위 첫 요청 또는 64×T1 뒤 회수 (TS 24.229 §5.2.2.1)
        time_t iNow;
        time( &iNow );
        for ( std::map<uint32_t, CIpsecSaSetInfo>::iterator o = m_clsMap.begin(); o != m_clsMap.end(); ++o ) {
            if ( o->first != iReqId && o->second.strUser == strUser && o->second.bEstablished &&
                 o->second.iDeleteAt == 0 )
                o->second.iDeleteAt = iNow + IPSEC_TEMP_SA_TIMEOUT_SEC;
        }
        it->second.clsSet.iLifetimeSec = iLifetimeSec;
        std::string strError;
        if ( CXfrmSa::Update( it->second.clsSet, strError ) ) {
            it->second.bEstablished = true;
            it->second.iCreateTime = iNow;
            bRes = true;
            CLog::Print( LOG_INFO, "ipsec: sa set established user=%s reqid=0x%x lifetime=%ds", strUser.c_str(), iReqId,
                         iLifetimeSec );
        } else {
            CLog::Print( LOG_ERROR, "ipsec: sa set lifetime update failed user=%s reqid=0x%x (%s)", strUser.c_str(),
                         iReqId, strError.c_str() );
        }
    }
    m_clsMutex.release();
    return bRes;
}

bool CIpsecSaSetMap::MatchEstablished( const std::string &strUser, const std::string &strIp, int iPort,
                                       CIpsecSaSetInfo *pclsOut ) {
    bool bRes = false;
    m_clsMutex.acquire();
    for ( std::map<uint32_t, CIpsecSaSetInfo>::iterator it = m_clsMap.begin(); it != m_clsMap.end(); ++it ) {
        CIpsecSaSetInfo &s = it->second;
        if ( !s.bEstablished || s.strUser != strUser ) continue;
        if ( s.clsSet.strRemoteIp == strIp && s.clsSet.iRemotePortC == iPort ) {
            if ( pclsOut ) *pclsOut = s;
            bRes = true;
            if ( s.iDeleteAt == 0 ) {
                // 현행 셋 위의 요청 — retiring 셋이 있으면 회수를 앞당긴다
                time_t iNow;
                time( &iNow );
                for ( std::map<uint32_t, CIpsecSaSetInfo>::iterator o = m_clsMap.begin(); o != m_clsMap.end(); ++o ) {
                    if ( o->first != it->first && o->second.strUser == strUser && o->second.bEstablished &&
                         o->second.iDeleteAt > iNow + IPSEC_RELEASE_GRACE_SEC )
                        o->second.iDeleteAt = iNow + IPSEC_RELEASE_GRACE_SEC;
                }
            }
            break;
        }
    }
    m_clsMutex.release();
    return bRes;
}

bool CIpsecSaSetMap::Extend( uint32_t iReqId, int iLifetimeSec ) {
    bool bRes = false;
    m_clsMutex.acquire();
    std::map<uint32_t, CIpsecSaSetInfo>::iterator it = m_clsMap.find( iReqId );
    if ( it != m_clsMap.end() && it->second.bEstablished ) {
        it->second.clsSet.iLifetimeSec = iLifetimeSec;
        std::string strError;
        bRes = CXfrmSa::Update( it->second.clsSet, strError );
        if ( bRes ) {
            time( &it->second.iCreateTime );
            it->second.iDeleteAt = 0;
        } else {
            CLog::Print( LOG_ERROR, "ipsec: sa set extend failed reqid=0x%x (%s)", iReqId, strError.c_str() );
        }
    }
    m_clsMutex.release();
    return bRes;
}

void CIpsecSaSetMap::Release( uint32_t iReqId, int iGraceSec ) {
    m_clsMutex.acquire();
    std::map<uint32_t, CIpsecSaSetInfo>::iterator it = m_clsMap.find( iReqId );
    if ( it != m_clsMap.end() ) {
        if ( iGraceSec <= 0 ) {
            _eraseLocked( it, "released" );
        } else {
            time_t iNow;
            time( &iNow );
            if ( it->second.iDeleteAt == 0 || it->second.iDeleteAt > iNow + iGraceSec )
                it->second.iDeleteAt = iNow + iGraceSec;
        }
    }
    m_clsMutex.release();
}

void CIpsecSaSetMap::ReleaseTemp( const std::string &strUser ) {
    m_clsMutex.acquire();
    for ( std::map<uint32_t, CIpsecSaSetInfo>::iterator it = m_clsMap.begin(); it != m_clsMap.end(); ) {
        if ( it->second.strUser == strUser && !it->second.bEstablished ) {
            std::map<uint32_t, CIpsecSaSetInfo>::iterator itDel = it++;
            _eraseLocked( itDel, "negotiation failed" );
        } else {
            ++it;
        }
    }
    m_clsMutex.release();
}

void CIpsecSaSetMap::ReleaseUser( const std::string &strUser, int iGraceSec ) {
    time_t iNow;
    time( &iNow );
    m_clsMutex.acquire();
    for ( std::map<uint32_t, CIpsecSaSetInfo>::iterator it = m_clsMap.begin(); it != m_clsMap.end(); ) {
        if ( it->second.strUser != strUser ) {
            ++it;
            continue;
        }
        if ( iGraceSec <= 0 ) {
            std::map<uint32_t, CIpsecSaSetInfo>::iterator itDel = it++;
            _eraseLocked( itDel, "user released" );
        } else {
            if ( it->second.iDeleteAt == 0 || it->second.iDeleteAt > iNow + iGraceSec )
                it->second.iDeleteAt = iNow + iGraceSec;
            ++it;
        }
    }
    m_clsMutex.release();
}

bool CIpsecSaSetMap::Select( uint32_t iReqId, CIpsecSaSetInfo &clsOut ) {
    bool bRes = false;
    m_clsMutex.acquire();
    std::map<uint32_t, CIpsecSaSetInfo>::iterator it = m_clsMap.find( iReqId );
    if ( it != m_clsMap.end() ) {
        clsOut = it->second;
        bRes = true;
    }
    m_clsMutex.release();
    return bRes;
}

void CIpsecSaSetMap::SetSecurityServer( uint32_t iReqId, const std::string &strList ) {
    m_clsMutex.acquire();
    std::map<uint32_t, CIpsecSaSetInfo>::iterator it = m_clsMap.find( iReqId );
    if ( it != m_clsMap.end() ) it->second.strSecurityServer = strList;
    m_clsMutex.release();
}

void CIpsecSaSetMap::Sweep( time_t iNow ) {
    m_clsMutex.acquire();
    for ( std::map<uint32_t, CIpsecSaSetInfo>::iterator it = m_clsMap.begin(); it != m_clsMap.end(); ) {
        const CIpsecSaSetInfo &s = it->second;
        const char *pszWhy = NULL;
        if ( s.iDeleteAt > 0 && s.iDeleteAt <= iNow )
            pszWhy = s.bEstablished ? "retired" : "released";
        else if ( !s.bEstablished && s.iCreateTime + gclsSetup.m_iIpsecTempSaTimeoutSec <= iNow )
            pszWhy = "temp timeout";
        else if ( s.bEstablished && s.clsSet.iLifetimeSec > 0 && s.iCreateTime + s.clsSet.iLifetimeSec <= iNow )
            pszWhy = "lifetime expired";
        if ( pszWhy ) {
            std::map<uint32_t, CIpsecSaSetInfo>::iterator itDel = it++;
            _eraseLocked( itDel, pszWhy );
        } else {
            ++it;
        }
    }
    m_clsMutex.release();
}

int CIpsecSaSetMap::Size() {
    m_clsMutex.acquire();
    int n = (int)m_clsMap.size();
    m_clsMutex.release();
    return n;
}
