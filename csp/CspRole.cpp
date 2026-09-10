/*
 * CspRole — 역할 모델 + 인메모리 맵 (docs/design/features/mcptt_authorization.md §2, dispatch_center.md §3.3·§3.5)
 */

#include "CspRole.h"

#include <fstream>
#include <sstream>

#include "CspPhoneGroup.h"
#include "DbManager.h"
#include "Directory.h"
#include "Log.h"
#include "SimpleJson.h"

CCspRoleMap gclsRoleMap;

// ──────────────────────────────────────────────────────────────
//  CspRole
// ──────────────────────────────────────────────────────────────

void CspRole::Clear() {
    m_strId.clear();
    m_strName.clear();
    m_strMonitorCall = "none";
    m_strPttListen = "none";
    m_strListenVisibility = "hidden";
    m_setMonitorTargets.clear();
    m_setPttTargets.clear();
    m_vecLines.clear();
}

// JSON fallback — DataFolder.Role/<id>.json. id 는 파일명(확장자 제외).
//   { "name": "...", "monitor_call": "own", "ptt_listen": "listed", "listen_visibility": "hidden",
//     "assignments": ["+8213…", "+8251…"],          // 배정 회선 id (person 의 전 회선을 펼친 것)
//     "monitor_targets": ["pg-…"], "ptt_targets": ["g002"] }
bool CspRole::LoadFile( const std::string &strPath ) {
    std::ifstream t( strPath );
    if ( !t.is_open() ) return false;
    std::stringstream buffer;
    buffer << t.rdbuf();
    SimpleJson::JsonNode root = SimpleJson::JsonNode::Parse( buffer.str() );
    if ( root.type != SimpleJson::JSON_OBJECT ) return false;

    Clear();
    std::string strBase = strPath;
    size_t iSlash = strBase.find_last_of( "/\\" );
    if ( iSlash != std::string::npos ) strBase = strBase.substr( iSlash + 1 );
    size_t iDot = strBase.rfind( ".json" );
    if ( iDot != std::string::npos ) strBase = strBase.substr( 0, iDot );
    m_strId = root.GetString( "id", strBase );
    if ( m_strId.empty() ) return false;

    m_strName = root.GetString( "name", m_strId );
    m_strMonitorCall = root.GetString( "monitor_call", "none" );
    m_strPttListen = root.GetString( "ptt_listen", "none" );
    m_strListenVisibility = root.GetString( "listen_visibility", "hidden" );

    auto readList = [&]( const char *pszKey, std::vector<std::string> &vecOut ) {
        SimpleJson::JsonNode arr = root.Get( pszKey );
        if ( arr.type != SimpleJson::JSON_ARRAY ) return;
        for ( size_t i = 0; i < arr.Size(); ++i ) {
            std::string v = arr.At( i ).AsString();
            if ( !v.empty() ) vecOut.push_back( v );
        }
    };
    std::vector<std::string> vecTmp;
    readList( "assignments", m_vecLines );
    readList( "monitor_targets", vecTmp );
    m_setMonitorTargets.insert( vecTmp.begin(), vecTmp.end() );
    vecTmp.clear();
    readList( "ptt_targets", vecTmp );
    m_setPttTargets.insert( vecTmp.begin(), vecTmp.end() );
    return true;
}

// ──────────────────────────────────────────────────────────────
//  CCspRoleMap
// ──────────────────────────────────────────────────────────────

bool CCspRoleMap::LoadFromDb() {
    return gclsDbManager.LoadAllRoles( *this );
}

bool CCspRoleMap::Load( const char *pszDirName ) {
    FILE_LIST clsFileList;
    if ( CDirectory::FileList( pszDirName, clsFileList ) == false ) {
        CLog::Print( LOG_ERROR, "RoleMap ReadDir(%s) failed", pszDirName );
        return false;
    }
    std::set<std::string> setFound;
    for ( FILE_LIST::iterator it = clsFileList.begin(); it != clsFileList.end(); ++it ) {
        if ( it->size() < 5 || it->compare( it->size() - 5, 5, ".json" ) != 0 ) continue;
        std::string strFileName = pszDirName;
        CDirectory::AppendName( strFileName, it->c_str() );
        CspRole clsRole;
        if ( clsRole.LoadFile( strFileName ) ) {
            Insert( clsRole );
            setFound.insert( clsRole.m_strId );
            CLog::Print( LOG_INFO, "RoleMap Loaded Role(%s: %s) monitor_call=%s ptt_listen=%s lines=%d",
                         clsRole.m_strId.c_str(), clsRole.m_strName.c_str(), clsRole.m_strMonitorCall.c_str(),
                         clsRole.m_strPttListen.c_str(), (int)clsRole.m_vecLines.size() );
        }
    }
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    for ( auto it = m_clsMap.begin(); it != m_clsMap.end(); ) {
        if ( setFound.find( it->first ) == setFound.end() ) {
            _unindex( it->second );
            it = m_clsMap.erase( it );
        } else {
            ++it;
        }
    }
    CLog::Print( LOG_INFO, "RoleMap: ReadDir finished. Total %d roles", (int)m_clsMap.size() );
    return true;
}

void CCspRoleMap::_index( const CspRole &clsRole ) {
    for ( const auto &l : clsRole.m_vecLines ) m_clsLineIndex[l] = clsRole.m_strId;
}

void CCspRoleMap::_unindex( const CspRole &clsRole ) {
    for ( const auto &l : clsRole.m_vecLines ) {
        auto it = m_clsLineIndex.find( l );
        if ( it != m_clsLineIndex.end() && it->second == clsRole.m_strId ) m_clsLineIndex.erase( it );
    }
}

void CCspRoleMap::Insert( const CspRole &clsRole ) {
    if ( clsRole.m_strId.empty() ) return;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMap.find( clsRole.m_strId );
    if ( it != m_clsMap.end() ) {
        _unindex( it->second );
        it->second = clsRole;
    } else {
        m_clsMap[clsRole.m_strId] = clsRole;
    }
    _index( clsRole );
}

void CCspRoleMap::Remove( const char *pszRoleId ) {
    if ( pszRoleId == NULL ) return;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMap.find( pszRoleId );
    if ( it == m_clsMap.end() ) return;
    _unindex( it->second );
    m_clsMap.erase( it );
}

void CCspRoleMap::Clear() {
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    m_clsMap.clear();
    m_clsLineIndex.clear();
}

bool CCspRoleMap::Select( const char *pszRoleId, CspRole &clsRole ) {
    if ( pszRoleId == NULL || pszRoleId[0] == '\0' ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMap.find( pszRoleId );
    if ( it == m_clsMap.end() ) return false;
    clsRole = it->second;
    return true;
}

bool CCspRoleMap::SelectForLine( const char *pszLineId, CspRole &clsRole ) {
    if ( pszLineId == NULL || pszLineId[0] == '\0' ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsLineIndex.find( pszLineId );
    if ( it == m_clsLineIndex.end() ) return false;
    auto itR = m_clsMap.find( it->second );
    if ( itR == m_clsMap.end() ) return false;
    clsRole = itR->second;
    return true;
}

std::string CCspRoleMap::RoleIdForLine( const char *pszLineId ) {
    if ( pszLineId == NULL || pszLineId[0] == '\0' ) return "";
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsLineIndex.find( pszLineId );
    return it == m_clsLineIndex.end() ? "" : it->second;
}

bool CCspRoleMap::CanWatch( const char *pszWatcherLine, const std::string &strTargetGroup ) {
    // 1. 같은 전화 그룹 — 그룹원 BLF·지정 픽업의 근거, 역할 불요 (volte_supplementary_services.md §6.2)
    const std::string strWatcherGroup = gclsPhoneGroupMap.EffectiveGroupOf( pszWatcherLine );
    if ( !strWatcherGroup.empty() && strWatcherGroup == strTargetGroup ) return true;
    // 2. watcher 의 역할 monitor_call (dispatch_center.md §5.2)
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsLineIndex.find( pszWatcherLine ? pszWatcherLine : "" );
    if ( it == m_clsLineIndex.end() ) return false;
    auto itR = m_clsMap.find( it->second );
    if ( itR == m_clsMap.end() ) return false;
    const CspRole &r = itR->second;
    if ( r.m_strMonitorCall == "all" ) return true;
    if ( r.m_strMonitorCall == "own" ) return !strWatcherGroup.empty() && strTargetGroup == strWatcherGroup;
    if ( r.m_strMonitorCall == "listed" )
        return !strTargetGroup.empty() && r.m_setMonitorTargets.find( strTargetGroup ) != r.m_setMonitorTargets.end();
    return false;
}

bool CCspRoleMap::CanListenPtt( const char *pszListenerLine, const char *pszPttGroupId ) {
    if ( pszListenerLine == NULL || pszPttGroupId == NULL ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsLineIndex.find( pszListenerLine );
    if ( it == m_clsLineIndex.end() ) return false;
    auto itR = m_clsMap.find( it->second );
    if ( itR == m_clsMap.end() ) return false;
    const CspRole &r = itR->second;
    if ( r.m_strPttListen == "all" ) return true;
    if ( r.m_strPttListen == "listed" ) return r.m_setPttTargets.find( pszPttGroupId ) != r.m_setPttTargets.end();
    return false;
}

bool CCspRoleMap::ListenHidden( const char *pszListenerLine ) {
    CspRole clsRole;
    if ( !SelectForLine( pszListenerLine, clsRole ) ) return true;
    return clsRole.m_strListenVisibility != "visible";
}

int CCspRoleMap::GetCount() {
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    return (int)m_clsMap.size();
}
