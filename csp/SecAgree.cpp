#include "SecAgree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Log.h"
#include "XfrmSa.h"

CSecAgreeMap gclsSecAgreeMap;

static std::string _trim( const std::string &s ) {
    size_t b = s.find_first_not_of( " \t" );
    if ( b == std::string::npos ) return "";
    size_t e = s.find_last_not_of( " \t" );
    return s.substr( b, e - b + 1 );
}

/** 콤마 목록 헤더 값에 option-tag 가 있는가 (대소문자 무시) */
static bool _hasOptionTag( CSipMessage *pclsMessage, const char *pszHeader, const char *pszTag ) {
    CSipHeader *pclsHeader = pclsMessage->GetHeader( pszHeader );
    if ( pclsHeader == NULL ) return false;
    const std::string &v = pclsHeader->m_strValue;
    size_t pos = 0;
    while ( pos <= v.size() ) {
        size_t comma = v.find( ',', pos );
        std::string item = _trim( v.substr( pos, comma == std::string::npos ? std::string::npos : comma - pos ) );
        if ( strcasecmp( item.c_str(), pszTag ) == 0 ) return true;
        if ( comma == std::string::npos ) break;
        pos = comma + 1;
    }
    return false;
}

SecAgreeRequest ParseSecAgree( CSipMessage *pclsMessage ) {
    SecAgreeRequest r;
    r.bRequire = _hasOptionTag( pclsMessage, "Require", "sec-agree" ) ||
                 _hasOptionTag( pclsMessage, "Proxy-Require", "sec-agree" );
    CSipHeader *p = pclsMessage->GetHeader( "Security-Client" );
    if ( p ) {
        r.bHasClient = true;
        r.strClient = _trim( p->m_strValue );
    }
    p = pclsMessage->GetHeader( "Security-Verify" );
    if ( p ) {
        r.bHasVerify = true;
        r.strVerify = _trim( p->m_strValue );
    }
    return r;
}

/** "name; k=v; k=v" 항목 하나를 파싱 — 첫 토큰이 메커니즘 이름 */
static void _parseMechanism( const std::string &strItem, std::string &strName,
                             std::map<std::string, std::string> &clsParams ) {
    strName.clear();
    clsParams.clear();
    size_t pos = 0;
    bool bFirst = true;
    while ( pos <= strItem.size() ) {
        size_t semi = strItem.find( ';', pos );
        std::string tok = _trim( strItem.substr( pos, semi == std::string::npos ? std::string::npos : semi - pos ) );
        if ( bFirst ) {
            strName = tok;
            bFirst = false;
        } else if ( !tok.empty() ) {
            size_t eq = tok.find( '=' );
            if ( eq == std::string::npos )
                clsParams[tok] = "";
            else
                clsParams[_trim( tok.substr( 0, eq ) )] = _trim( tok.substr( eq + 1 ) );
        }
        if ( semi == std::string::npos ) break;
        pos = semi + 1;
    }
}

static std::string _lower( std::string s ) {
    for ( size_t i = 0; i < s.size(); ++i )
        if ( s[i] >= 'A' && s[i] <= 'Z' ) s[i] = (char)( s[i] - 'A' + 'a' );
    return s;
}

SecAgreeIpsecOffer SelectIpsecOffer( const std::string &strClient, const std::string &strEalgPref, bool &bAnyIpsec ) {
    SecAgreeIpsecOffer best;
    bAnyIpsec = false;
    size_t pos = 0;
    while ( pos <= strClient.size() ) {
        size_t comma = strClient.find( ',', pos );
        std::string item =
            _trim( strClient.substr( pos, comma == std::string::npos ? std::string::npos : comma - pos ) );
        if ( comma == std::string::npos )
            pos = strClient.size() + 1;
        else
            pos = comma + 1;
        if ( item.empty() ) continue;
        std::string name;
        std::map<std::string, std::string> p;
        _parseMechanism( item, name, p );
        if ( strcasecmp( name.c_str(), "ipsec-3gpp" ) != 0 ) continue;
        bAnyIpsec = true;
        SecAgreeIpsecOffer o;
        o.strAlg = _lower( p["alg"] );
        o.strEalg = p.count( "ealg" ) ? _lower( p["ealg"] ) : "null";  // RFC 3329 Annex — ealg 기본 null
        if ( p.count( "prot" ) && _lower( p["prot"] ) != "esp" ) continue;
        if ( p.count( "mod" ) && _lower( p["mod"] ) != "trans" ) continue;  // Annex M(UDP-enc-tun) 미지원
        if ( !CXfrmSa::IsAlgSupported( o.strAlg, o.strEalg ) ) continue;
        o.iSpiC = (uint32_t)strtoul( p["spi-c"].c_str(), NULL, 10 );
        o.iSpiS = (uint32_t)strtoul( p["spi-s"].c_str(), NULL, 10 );
        o.iPortC = atoi( p["port-c"].c_str() );
        o.iPortS = atoi( p["port-s"].c_str() );
        if ( o.iSpiC == 0 || o.iSpiS == 0 || o.iSpiC == o.iSpiS || o.iPortC <= 0 || o.iPortS <= 0 || o.iPortC > 65535 ||
             o.iPortS > 65535 || o.iPortC == o.iPortS )
            continue;  // TS 33.203 §7.1 — SPI·포트는 각각 달라야 한다
        o.dQ = p.count( "q" ) ? atof( p["q"].c_str() ) : 1.0;
        o.bValid = true;
        // 선택: q → sha1 우선 → ealg 선호
        bool bBetter = !best.bValid || o.dQ > best.dQ;
        if ( !bBetter && o.dQ == best.dQ ) {
            if ( o.strAlg != best.strAlg )
                bBetter = ( o.strAlg == XFRM_AUTH_HMAC_SHA1_96 );
            else if ( o.strEalg != best.strEalg )
                bBetter = ( o.strEalg == strEalgPref );
        }
        if ( bBetter ) best = o;
    }
    return best;
}

std::string BuildIpsecServerList( const SecAgreeIpsecOffer &clsOffer, uint32_t iSpiPc, uint32_t iSpiPs, int iPortPc,
                                  int iPortPs ) {
    char sz[256];
    snprintf( sz, sizeof( sz ), "ipsec-3gpp;q=0.2;alg=%s;ealg=%s;spi-c=%u;spi-s=%u;port-c=%d;port-s=%d,%s",
              clsOffer.strAlg.c_str(), clsOffer.strEalg.c_str(), iSpiPc, iSpiPs, iPortPc, iPortPs,
              SEC_AGREE_SERVER_LIST );
    return sz;
}

bool SecAgreeListIsIpsec( const std::string &strList ) {
    return strncasecmp( _trim( strList ).c_str(), "ipsec-3gpp", 10 ) == 0;
}

std::string CSecAgreeMap::Issue( const std::string &strUser ) {
    return Issue( strUser, SEC_AGREE_SERVER_LIST );
}

std::string CSecAgreeMap::Issue( const std::string &strUser, const std::string &strList ) {
    Entry e;
    e.strServer = strList;
    time( &e.iTime );
    m_clsMutex.acquire();
    m_clsMap[strUser] = e;
    m_clsMutex.release();
    return e.strServer;
}

ESecAgreeVerify CSecAgreeMap::Verify( const std::string &strUser, const std::string &strVerify ) {
    ESecAgreeVerify eRes = E_SECAGREE_NONE;
    m_clsMutex.acquire();
    std::map<std::string, Entry>::iterator it = m_clsMap.find( strUser );
    if ( it != m_clsMap.end() ) {
        // RFC 3329 §2.3 — 서버가 보낸 목록과 **동일**해야 한다. 파싱·재조립 없이 원문 대조.
        eRes = ( _trim( strVerify ) == it->second.strServer ) ? E_SECAGREE_OK : E_SECAGREE_MISMATCH;
    }
    m_clsMutex.release();
    return eRes;
}

void CSecAgreeMap::Delete( const std::string &strUser ) {
    m_clsMutex.acquire();
    m_clsMap.erase( strUser );
    m_clsMutex.release();
}

void CSecAgreeMap::DeleteTimeout( int iSecond ) {
    time_t iNow;
    time( &iNow );
    m_clsMutex.acquire();
    for ( std::map<std::string, Entry>::iterator it = m_clsMap.begin(); it != m_clsMap.end(); ) {
        if ( it->second.iTime + iSecond < iNow )
            m_clsMap.erase( it++ );
        else
            ++it;
    }
    m_clsMutex.release();
}
