/*
 * CspPhoneGroup — 전화 그룹 모델 + 인메모리 맵 (docs/design/features/dispatch_center.md §3.1·§3.5)
 */

#include "CspPhoneGroup.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "CspUser.h"
#include "DbManager.h"
#include "Directory.h"
#include "Log.h"
#include "SimpleJson.h"

CCspPhoneGroupMap gclsPhoneGroupMap;

// ──────────────────────────────────────────────────────────────
//  CspPhoneGroup
// ──────────────────────────────────────────────────────────────

void CspPhoneGroup::Clear() {
    m_strId.clear();
    m_strName.clear();
    m_strPilotId.clear();
    m_strServiceRef.clear();
    m_strAlertMode = "parallel";
    m_iNoAnswerSec = 30;
    m_strBusyMembers = "skip";
    m_strOverflowTarget.clear();
    m_strOrgId.clear();
    m_vecMembers.clear();
}

bool CspPhoneGroup::IsMember( const std::string &strUserId ) const {
    for ( const auto &m : m_vecMembers )
        if ( m.strUserId == strUserId ) return true;
    return false;
}

// JSON fallback — DataFolder.PhoneGroup/<id>.json (User/Group 관례). id 는 파일명(확장자 제외).
//   { "name": "...", "pilot_id": "+8213…", "service_ref": "voip", "alert_mode": "parallel",
//     "no_answer_sec": 30, "busy_members": "skip", "overflow_target": "", "org_id": "1",
//     "members": [ {"user_id": "+8213...", "alert_order": 0}, "+8213..." ] }
bool CspPhoneGroup::LoadFile( const std::string &strPath ) {
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
    m_strOrgId = root.GetString( "org_id" );

    SimpleJson::JsonNode members = root.Get( "members" );
    if ( members.type == SimpleJson::JSON_ARRAY ) {
        for ( size_t i = 0; i < members.Size(); ++i ) {
            SimpleJson::JsonNode m = members.At( i );
            CspPhoneGroupMember clsMember;
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
    std::sort(
        m_vecMembers.begin(), m_vecMembers.end(),
        []( const CspPhoneGroupMember &a, const CspPhoneGroupMember &b ) { return a.iAlertOrder < b.iAlertOrder; } );
    return true;
}

// ──────────────────────────────────────────────────────────────
//  CCspPhoneGroupMap
// ──────────────────────────────────────────────────────────────

bool CCspPhoneGroupMap::LoadFromDb() {
    return gclsDbManager.LoadAllPhoneGroups( *this );
}

bool CCspPhoneGroupMap::LoadOneFromDb( const char *pszGroupId ) {
    if ( pszGroupId == NULL || pszGroupId[0] == '\0' ) return false;
    CspPhoneGroup clsGroup;
    if ( gclsDbManager.SelectPhoneGroup( pszGroupId, clsGroup ) == false ) return false;
    Insert( clsGroup );
    return true;
}

bool CCspPhoneGroupMap::Load( const char *pszDirName ) {
    FILE_LIST clsFileList;
    if ( CDirectory::FileList( pszDirName, clsFileList ) == false ) {
        CLog::Print( LOG_ERROR, "PhoneGroupMap ReadDir(%s) failed", pszDirName );
        return false;
    }
    std::set<std::string> setFound;
    for ( FILE_LIST::iterator it = clsFileList.begin(); it != clsFileList.end(); ++it ) {
        if ( it->size() < 5 || it->compare( it->size() - 5, 5, ".json" ) != 0 ) continue;
        std::string strFileName = pszDirName;
        CDirectory::AppendName( strFileName, it->c_str() );
        CspPhoneGroup clsGroup;
        if ( clsGroup.LoadFile( strFileName ) ) {
            Insert( clsGroup );
            setFound.insert( clsGroup.m_strId );
            CLog::Print( LOG_INFO, "PhoneGroupMap Loaded Group(%s: %s) pilot=%s members=%d", clsGroup.m_strId.c_str(),
                         clsGroup.m_strName.c_str(), clsGroup.m_strPilotId.c_str(), (int)clsGroup.m_vecMembers.size() );
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
    CLog::Print( LOG_INFO, "PhoneGroupMap: ReadDir finished. Total %d groups", (int)m_clsMap.size() );
    return true;
}

void CCspPhoneGroupMap::_index( const CspPhoneGroup &clsGroup ) {
    if ( !clsGroup.m_strPilotId.empty() ) m_clsPilotIndex[clsGroup.m_strPilotId] = clsGroup.m_strId;
    for ( const auto &m : clsGroup.m_vecMembers ) m_clsMemberIndex[m.strUserId] = clsGroup.m_strId;
}

void CCspPhoneGroupMap::_unindex( const CspPhoneGroup &clsGroup ) {
    if ( !clsGroup.m_strPilotId.empty() ) {
        auto it = m_clsPilotIndex.find( clsGroup.m_strPilotId );
        if ( it != m_clsPilotIndex.end() && it->second == clsGroup.m_strId ) m_clsPilotIndex.erase( it );
    }
    for ( const auto &m : clsGroup.m_vecMembers ) {
        auto it = m_clsMemberIndex.find( m.strUserId );
        if ( it != m_clsMemberIndex.end() && it->second == clsGroup.m_strId ) m_clsMemberIndex.erase( it );
    }
}

void CCspPhoneGroupMap::Insert( const CspPhoneGroup &clsGroup ) {
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

void CCspPhoneGroupMap::Remove( const char *pszGroupId ) {
    if ( pszGroupId == NULL ) return;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMap.find( pszGroupId );
    if ( it == m_clsMap.end() ) return;
    _unindex( it->second );
    m_clsMap.erase( it );
}

void CCspPhoneGroupMap::Clear() {
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    m_clsMap.clear();
    m_clsPilotIndex.clear();
    m_clsMemberIndex.clear();
}

bool CCspPhoneGroupMap::Select( const char *pszGroupId, CspPhoneGroup &clsGroup ) {
    if ( pszGroupId == NULL || pszGroupId[0] == '\0' ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMap.find( pszGroupId );
    if ( it == m_clsMap.end() ) return false;
    clsGroup = it->second;
    return true;
}

bool CCspPhoneGroupMap::Contains( const char *pszGroupId ) {
    if ( pszGroupId == NULL || pszGroupId[0] == '\0' ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    return m_clsMap.find( pszGroupId ) != m_clsMap.end();
}

bool CCspPhoneGroupMap::SelectByPilot( const char *pszPilotId, CspPhoneGroup &clsGroup ) {
    if ( pszPilotId == NULL || pszPilotId[0] == '\0' ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsPilotIndex.find( pszPilotId );
    if ( it == m_clsPilotIndex.end() ) return false;
    auto itG = m_clsMap.find( it->second );
    if ( itG == m_clsMap.end() ) return false;
    clsGroup = itG->second;
    return true;
}

bool CCspPhoneGroupMap::IsPilot( const char *pszId ) {
    if ( pszId == NULL || pszId[0] == '\0' ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    return m_clsPilotIndex.find( pszId ) != m_clsPilotIndex.end();
}

bool CCspPhoneGroupMap::SelectForUser( const char *pszUserId, CspPhoneGroup &clsGroup ) {
    if ( pszUserId == NULL || pszUserId[0] == '\0' ) return false;
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMemberIndex.find( pszUserId );
    if ( it == m_clsMemberIndex.end() ) return false;
    auto itG = m_clsMap.find( it->second );
    if ( itG == m_clsMap.end() ) return false;
    clsGroup = itG->second;
    return true;
}

std::string CCspPhoneGroupMap::GroupIdForUser( const char *pszUserId ) {
    if ( pszUserId == NULL || pszUserId[0] == '\0' ) return "";
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    auto it = m_clsMemberIndex.find( pszUserId );
    return it == m_clsMemberIndex.end() ? "" : it->second;
}

std::string CCspPhoneGroupMap::EffectiveGroupOf( const char *pszUserId ) {
    std::string strGroup = GroupIdForUser( pszUserId );
    if ( !strGroup.empty() ) return strGroup;
    CspUser clsUser;
    if ( pszUserId && gclsCspUserMap.Select( pszUserId, clsUser ) ) return clsUser.EffectivePickupGroup();
    return "";
}

int CCspPhoneGroupMap::GetCount() {
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    return (int)m_clsMap.size();
}
