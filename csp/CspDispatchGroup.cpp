/*
 * CspDispatchGroup — 관제 그룹 모델 + 인메모리 맵 (docs/design/features/dispatch_center.md §3)
 */

#include "CspDispatchGroup.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "CspUser.h"
#include "DbManager.h"
#include "Directory.h"
#include "Log.h"
#include "SimpleJson.h"

CCspDispatchGroupMap gclsDispatchGroupMap;

// ──────────────────────────────────────────────────────────────
//  CspDispatchGroup
// ──────────────────────────────────────────────────────────────

void CspDispatchGroup::Clear() {
    m_strId.clear();
    m_strName.clear();
    m_strPilotId.clear();
    m_strServiceRef.clear();
    m_strAlertMode = "parallel";
    m_iNoAnswerSec = 30;
    m_strBusyMembers = "skip";
    m_strOverflowTarget.clear();
    m_strMonitorScope = "none";
    m_strPttListen = "none";
    m_strListenVisibility = "hidden";
    m_strOrgId.clear();
    m_vecMembers.clear();
    m_setMonitorTargets.clear();
    m_setPttTargets.clear();
}

bool CspDispatchGroup::IsMember( const std::string &strUserId ) const {
    for ( const auto &m : m_vecMembers )
        if ( m.strUserId == strUserId ) return true;
    return false;
}

// JSON fallback — csp/DispatchGroup/<id>.json (User/Group 관례). id 는 파일명(확장자 제외).
//   { "name": "...", "pilot_id": "7000", "service_ref": "volte", "alert_mode": "parallel",
//     "no_answer_sec": 30, "busy_members": "skip", "overflow_target": "", "monitor_scope": "none",
//     "ptt_listen": "none", "listen_visibility": "hidden", "org_id": "1",
//     "members": [ {"user_id": "+8210...", "alert_order": 0}, "+8210..." ],
//     "monitor_targets": ["dg-..."], "ptt_targets": ["g001"] }
bool CspDispatchGroup::LoadFile( const std::string &strPath ) {
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
    m_strPilotId = root.GetString( "pilot_id" );
    m_strServiceRef = root.GetString( "service_ref" );
    m_strAlertMode = root.GetString( "alert_mode", "parallel" );
    m_iNoAnswerSec = (int)root.GetInt( "no_answer_sec", 30 );
    m_strBusyMembers = root.GetString( "busy_members", "skip" );
    m_strOverflowTarget = root.GetString( "overflow_target" );
    m_strMonitorScope = root.GetString( "monitor_scope", "none" );
    m_strPttListen = root.GetString( "ptt_listen", "none" );
    m_strListenVisibility = root.GetString( "listen_visibility", "hidden" );
    m_strOrgId = root.GetString( "org_id" );

    SimpleJson::JsonNode members = root.Get( "members" );
    if ( members.type == SimpleJson::JSON_ARRAY ) {
        for ( size_t i = 0; i < members.Size(); ++i ) {
            SimpleJson::JsonNode m = members.At( i );
            CspDispatchMember clsMember;
            if ( m.type == SimpleJson::JSON_OBJECT ) {
                clsMember.strUserId = m.GetString( "user_id" );
                clsMember.iAlertOrder = (int)m.GetInt( "alert_order", (int)i );
            } else {
                clsMember.strUserId = m.AsString();
                clsMember.iAlertOrder = (int)i;
            }
            if ( !clsMember.strUserId.empty() ) m_vecMembers.push_back( clsMember );
        }
    }
    auto readSet = [&]( const char *pszKey, std::set<std::string> &setOut ) {
        SimpleJson::JsonNode arr = root.Get( pszKey );
        if ( arr.type != SimpleJson::JSON_ARRAY ) return;
        for ( size_t i = 0; i < arr.Size(); ++i ) {
            std::string v = arr.At( i ).AsString();
            if ( !v.empty() ) setOut.insert( v );
        }
    };
    readSet( "monitor_targets", m_setMonitorTargets );
    readSet( "ptt_targets", m_setPttTargets );
    std::sort( m_vecMembers.begin(), m_vecMembers.end(),
               []( const CspDispatchMember &a, const CspDispatchMember &b ) { return a.iAlertOrder < b.iAlertOrder; } );
    return true;
}

// ──────────────────────────────────────────────────────────────
//  CCspDispatchGroupMap
// ──────────────────────────────────────────────────────────────

bool CCspDispatchGroupMap::LoadFromDb() {
    return gclsDbManager.LoadAllDispatchGroups( *this );
}

bool CCspDispatchGroupMap::LoadOneFromDb( const char *pszGroupId ) {
    if ( pszGroupId == NULL || pszGroupId[0] == '\0' ) return false;
    CspDispatchGroup clsGroup;
    if ( gclsDbManager.SelectDispatchGroup( pszGroupId, clsGroup ) == false ) return false;
    Insert( clsGroup );
    return true;
}

bool CCspDispatchGroupMap::Load( const char *pszDirName ) {
    FILE_LIST clsFileList;
    if ( CDirectory::FileList( pszDirName, clsFileList ) == false ) {
        CLog::Print( LOG_ERROR, "DispatchGroupMap ReadDir(%s) failed", pszDirName );
        return false;
    }
    std::set<std::string> setFound;
    for ( FILE_LIST::iterator it = clsFileList.begin(); it != clsFileList.end(); ++it ) {
        if ( it->size() < 5 || it->compare( it->size() - 5, 5, ".json" ) != 0 ) continue;
        std::string strFileName = pszDirName;
        CDirectory::AppendName( strFileName, it->c_str() );
        CspDispatchGroup clsGroup;
        if ( clsGroup.LoadFile( strFileName ) ) {
            Insert( clsGroup );
            setFound.insert( clsGroup.m_strId );
            CLog::Print( LOG_INFO, "DispatchGroupMap Loaded Group(%s: %s) pilot=%s members=%d monitor=%s ptt_listen=%s",
                         clsGroup.m_strId.c_str(), clsGroup.m_strName.c_str(), clsGroup.m_strPilotId.c_str(),
                         (int)clsGroup.m_vecMembers.size(), clsGroup.m_strMonitorScope.c_str(),
                         clsGroup.m_strPttListen.c_str() );
        }
    }
    // 디렉터리에서 사라진 그룹 제거 (GroupMap::ReadDir 동형)
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    for ( auto it = m_clsMap.begin(); it != m_clsMap.end(); ) {
        if ( setFound.find( it->first ) == setFound.end() ) {
            _unindex( it->second );
            it = m_clsMap.erase( it );
        } else {
            ++it;
        }
    }
    CLog::Print( LOG_INFO, "DispatchGroupMap: ReadDir finished. Total %d groups", (int)m_clsMap.size() );
    return true;
}

void CCspDispatchGroupMap::_index( const CspDispatchGroup &clsGroup ) {
    if ( !clsGroup.m_strPilotId.empty() ) m_clsPilotIndex[clsGroup.m_strPilotId] = clsGroup.m_strId;
    for ( const auto &m : clsGroup.m_vecMembers ) m_clsMemberIndex[m.strUserId] = clsGroup.m_strId;
}

void CCspDispatchGroupMap::_unindex( const CspDispatchGroup &clsGroup ) {
    if ( !clsGroup.m_strPilotId.empty() ) {
        auto it = m_clsPilotIndex.find( clsGroup.m_strPilotId );
        if ( it != m_clsPilotIndex.end() && it->second == clsGroup.m_strId ) m_clsPilotIndex.erase( it );
    }
    for ( const auto &m : clsGroup.m_vecMembers ) {
        auto it = m_clsMemberIndex.find( m.strUserId );
        if ( it != m_clsMemberIndex.end() && it->second == clsGroup.m_strId ) m_clsMemberIndex.erase( it );
    }
}

void CCspDispatchGroupMap::Insert( const CspDispatchGroup &clsGroup ) {
    if ( clsGroup.m_strId.empty() ) return;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMap.find( clsGroup.m_strId );
    if ( it != m_clsMap.end() ) {
        _unindex( it->second );
        it->second = clsGroup;
    } else {
        m_clsMap[clsGroup.m_strId] = clsGroup;
    }
    _index( clsGroup );
}

void CCspDispatchGroupMap::Remove( const char *pszGroupId ) {
    if ( pszGroupId == NULL ) return;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMap.find( pszGroupId );
    if ( it == m_clsMap.end() ) return;
    _unindex( it->second );
    m_clsMap.erase( it );
}

void CCspDispatchGroupMap::Clear() {
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    m_clsMap.clear();
    m_clsPilotIndex.clear();
    m_clsMemberIndex.clear();
}

bool CCspDispatchGroupMap::Select( const char *pszGroupId, CspDispatchGroup &clsGroup ) {
    if ( pszGroupId == NULL || pszGroupId[0] == '\0' ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMap.find( pszGroupId );
    if ( it == m_clsMap.end() ) return false;
    clsGroup = it->second;
    return true;
}

bool CCspDispatchGroupMap::Contains( const char *pszGroupId ) {
    if ( pszGroupId == NULL || pszGroupId[0] == '\0' ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    return m_clsMap.find( pszGroupId ) != m_clsMap.end();
}

bool CCspDispatchGroupMap::SelectByPilot( const char *pszPilotId, CspDispatchGroup &clsGroup ) {
    if ( pszPilotId == NULL || pszPilotId[0] == '\0' ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsPilotIndex.find( pszPilotId );
    if ( it == m_clsPilotIndex.end() ) return false;
    auto itG = m_clsMap.find( it->second );
    if ( itG == m_clsMap.end() ) return false;
    clsGroup = itG->second;
    return true;
}

bool CCspDispatchGroupMap::IsPilot( const char *pszId ) {
    if ( pszId == NULL || pszId[0] == '\0' ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    return m_clsPilotIndex.find( pszId ) != m_clsPilotIndex.end();
}

bool CCspDispatchGroupMap::SelectForUser( const char *pszUserId, CspDispatchGroup &clsGroup ) {
    if ( pszUserId == NULL || pszUserId[0] == '\0' ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMemberIndex.find( pszUserId );
    if ( it == m_clsMemberIndex.end() ) return false;
    auto itG = m_clsMap.find( it->second );
    if ( itG == m_clsMap.end() ) return false;
    clsGroup = itG->second;
    return true;
}

std::string CCspDispatchGroupMap::GroupIdForUser( const char *pszUserId ) {
    if ( pszUserId == NULL || pszUserId[0] == '\0' ) return "";
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMemberIndex.find( pszUserId );
    return it == m_clsMemberIndex.end() ? "" : it->second;
}

std::string CCspDispatchGroupMap::EffectiveGroupOf( const char *pszUserId ) {
    std::string strGroup = GroupIdForUser( pszUserId );
    if ( !strGroup.empty() ) return strGroup;
    CspUser clsUser;
    if ( pszUserId && gclsCspUserMap.Select( pszUserId, clsUser ) ) return clsUser.EffectivePickupGroup();
    return "";
}

bool CCspDispatchGroupMap::CanWatch( const std::string &strWatcherGroup, const std::string &strTargetGroup ) {
    // 1. 같은 픽업 그룹(= 같은 관제 그룹) — 현행 규칙 보존 (volte_supplementary_services.md §6.2)
    if ( !strWatcherGroup.empty() && strWatcherGroup == strTargetGroup ) return true;
    if ( strWatcherGroup.empty() ) return false;
    // 2. watcher 의 관제 그룹 monitor_scope (dispatch_center.md §5.2)
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMap.find( strWatcherGroup );
    if ( it == m_clsMap.end() ) return false;  // 관제 그룹이 아닌 픽업 축 값(org 폴백 등) — 확장 범위 없음
    const CspDispatchGroup &g = it->second;
    if ( g.m_strMonitorScope == "all" ) return true;
    if ( g.m_strMonitorScope == "own" ) return strTargetGroup == g.m_strId;
    if ( g.m_strMonitorScope == "listed" )
        return !strTargetGroup.empty() && g.m_setMonitorTargets.find( strTargetGroup ) != g.m_setMonitorTargets.end();
    return false;
}

bool CCspDispatchGroupMap::CanListenPtt( const std::string &strWatcherGroup, const char *pszPttGroupId ) {
    if ( strWatcherGroup.empty() || pszPttGroupId == NULL ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMap.find( strWatcherGroup );
    if ( it == m_clsMap.end() ) return false;
    const CspDispatchGroup &g = it->second;
    if ( g.m_strPttListen == "all" ) return true;
    if ( g.m_strPttListen == "listed" ) return g.m_setPttTargets.find( pszPttGroupId ) != g.m_setPttTargets.end();
    return false;
}

int CCspDispatchGroupMap::GetCount() {
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    return (int)m_clsMap.size();
}
