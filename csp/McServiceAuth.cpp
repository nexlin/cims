// MC 서비스 인가·서비스 설정 — 바인딩 저장소와 IdMS introspection (McServiceAuth.h)

#include "McServiceAuth.h"

#include <cctype>

#include "CscEndpointCache.h"
#include "HttpClient.h"
#include "Log.h"
#include "SipServerSetup.h"

CMcServiceAuth gclsMcServiceAuth;

namespace {
    /** application/x-www-form-urlencoded 값 (RFC 7662 §2.1 token 파라미터) */
    std::string _FormEncode( const std::string &v ) {
        static const char *kHex = "0123456789ABCDEF";
        std::string o;
        for ( unsigned char c : v ) {
            if ( isalnum( c ) || c == '-' || c == '_' || c == '.' || c == '~' ) {
                o += (char)c;
            } else {
                o += '%';
                o += kHex[c >> 4];
                o += kHex[c & 15];
            }
        }
        return o;
    }
}  // namespace

std::string CMcServiceAuth::IntrospectUrl() {
    return gclsCscEndpointCache.GetServiceUrlBase() + "/idms/introspect";
}

EMcAuthResult CMcServiceAuth::Authorize( EMcService e, const std::string &strImpuUser, const std::string &strToken,
                                         const std::string &strClientId, std::string &strWhy ) {
    if ( strToken.empty() || strClientId.empty() ) {
        strWhy = "접근 토큰 또는 client ID 없음";
        return EMcAuthResult::Failed;
    }
    // 서비스 인가(TS 24.379 §7.3.3 4) · TS 24.282·24.281 같은 절) — 참여 기능이 IdMS 에 토큰을 묻는다(RFC 7662 §2.1)
    const std::string strUrl = IntrospectUrl();
    CHttpClient clsClient;
    const int iSec = ( gclsSetup.m_iCscTimeoutMs + 999 ) / 1000;
    clsClient.SetRecvTimeout( iSec < 1 ? 1 : iSec );
    std::string strCtype, strBody;
    const std::string strForm = "token=" + _FormEncode( strToken );
    clsClient.DoPost( strUrl.c_str(), "application/x-www-form-urlencoded", strForm.c_str(), strCtype, strBody );
    const int iStatus = clsClient.GetStatusCode();
    if ( iStatus != 200 ) {
        strWhy = "introspection " + strUrl + " → " + std::to_string( iStatus );
        return EMcAuthResult::Unavailable;
    }
    const EMcAuthResult eRes = McAuthVerdict( strBody, e, strImpuUser, strWhy );
    if ( eRes != EMcAuthResult::Ok ) return eRes;

    std::lock_guard<std::mutex> lock( m_mutex );
    auto &vec = m_map[{ (int)e, strImpuUser }];
    for ( const auto &b : vec )
        if ( b.strClientId == strClientId ) return EMcAuthResult::Ok;  // 같은 클라이언트의 재인가 — 바인딩 유지
    McServiceBinding b;
    b.strClientId = strClientId;
    vec.push_back( b );
    CLog::Print( LOG_INFO, "McServiceAuth: %s 서비스 인가 — MC ID %s client %s (바인딩 %d)", McServiceName( e ),
                 strImpuUser.c_str(), strClientId.c_str(), (int)vec.size() );
    return EMcAuthResult::Ok;
}

bool CMcServiceAuth::HasBinding( EMcService e, const std::string &strMcId, const std::string &strClientId ) const {
    std::lock_guard<std::mutex> lock( m_mutex );
    auto it = m_map.find( { (int)e, strMcId } );
    if ( it == m_map.end() || it->second.empty() ) return false;
    if ( strClientId.empty() ) return true;
    for ( const auto &b : it->second )
        if ( b.strClientId == strClientId ) return true;
    return false;
}

int CMcServiceAuth::BindingCount( EMcService e, const std::string &strMcId ) const {
    std::lock_guard<std::mutex> lock( m_mutex );
    auto it = m_map.find( { (int)e, strMcId } );
    return it == m_map.end() ? 0 : (int)it->second.size();
}

bool CMcServiceAuth::SetSettings( EMcService e, const std::string &strMcId, const std::string &strClientId,
                                  const McPocEntity &clsEntity ) {
    std::lock_guard<std::mutex> lock( m_mutex );
    auto it = m_map.find( { (int)e, strMcId } );
    if ( it == m_map.end() ) return false;
    for ( auto &b : it->second ) {
        if ( b.strClientId != strClientId ) continue;
        b.bSettings = true;
        b.clsSettings = clsEntity;
        b.clsSettings.strId = strClientId;
        time( &b.tSettings );
        return true;
    }
    return false;
}

bool CMcServiceAuth::AnswerModeOf( EMcService e, const std::string &strMcId, std::string &strMode ) const {
    std::lock_guard<std::mutex> lock( m_mutex );
    auto it = m_map.find( { (int)e, strMcId } );
    if ( it == m_map.end() ) return false;
    time_t tBest = 0;
    bool bFound = false;
    for ( const auto &b : it->second ) {
        if ( !b.bSettings || b.clsSettings.strAnswerMode.empty() || b.tSettings < tBest ) continue;
        tBest = b.tSettings;
        strMode = b.clsSettings.strAnswerMode;
        bFound = true;
    }
    return bFound;
}

int CMcServiceAuth::Unbind( EMcService e, const std::string &strMcId, const std::string &strClientId ) {
    std::lock_guard<std::mutex> lock( m_mutex );
    auto it = m_map.find( { (int)e, strMcId } );
    if ( it == m_map.end() ) return 0;
    int n = 0;
    for ( auto b = it->second.begin(); b != it->second.end(); ) {
        if ( strClientId.empty() || b->strClientId == strClientId ) {
            b = it->second.erase( b );
            ++n;
        } else {
            ++b;
        }
    }
    if ( it->second.empty() ) m_map.erase( it );
    if ( n > 0 )
        CLog::Print( LOG_INFO, "McServiceAuth: %s 바인딩 제거 — MC ID %s client %s (%d)", McServiceName( e ),
                     strMcId.c_str(), strClientId.empty() ? "*" : strClientId.c_str(), n );
    return n;
}

void CMcServiceAuth::UnbindAll( const std::string &strMcId ) {
    for ( EMcService e : { EMcService::Mcptt, EMcService::McVideo, EMcService::McData } ) Unbind( e, strMcId );
}

std::string CMcServiceAuth::SettingsDocument( EMcService e, const std::string &strMcId ) const {
    std::vector<McPocEntity> vec;
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        auto it = m_map.find( { (int)e, strMcId } );
        if ( it != m_map.end() )
            for ( const auto &b : it->second )
                if ( b.bSettings ) vec.push_back( b.clsSettings );
    }
    return BuildPocSettingsDoc( vec );
}
