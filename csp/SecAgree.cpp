#include "SecAgree.h"

#include <string.h>

#include "Log.h"

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

std::string CSecAgreeMap::Issue( const std::string &strUser ) {
    Entry e;
    e.strServer = SEC_AGREE_SERVER_LIST;
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
