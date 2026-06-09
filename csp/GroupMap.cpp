/*
 * Group Map Source
 */

#include "GroupMap.h"

#include <set>

#include "CmpClient.h"
#include "DbManager.h"
#include "Directory.h"
#include "Log.h"
#include "UserMap.h"

CGroupMap gclsGroupMap;

CGroupMap::CGroupMap() {
}

CGroupMap::~CGroupMap() {
    Clear();
}

/**
 * @brief Load groups from a directory
 */
bool CGroupMap::Load( const char *pszDirName ) {
    return ReadDir( pszDirName );
}

/**
 * @brief DB에서 전체 그룹을 로드한다
 */
bool CGroupMap::LoadFromDb() {
    return gclsDbManager.LoadAllGroups( *this );
}

/**
 * @brief DB에서 특정 그룹 id 하나만 조회하여 로드한다 (전체 LoadAllGroups 재로드 없음).
 *        그룹이면 맵에 삽입하고 true, 해당 id 가 그룹으로 존재하지 않으면 false.
 *        (cache miss 시 VoLTE 1:1 착신이 매 호 전체 그룹 재로드를 유발하던 문제 해소 — 단건 조회.)
 */
bool CGroupMap::LoadOneFromDb( const char *pszGroupId ) {
    if ( !pszGroupId || !pszGroupId[0] ) return false;
    CspPttGroup clsGroup;
    if ( gclsDbManager.SelectGroup( pszGroupId, clsGroup ) ) {
        Insert( clsGroup );
        return true;
    }
    return false;
}

/**
 * @brief Read directory and parse JSON files
 */
bool CGroupMap::ReadDir( const char *pszDirName ) {
    FILE_LIST clsFileList;
    FILE_LIST::iterator itFL;
    std::string strFileName;
    std::set<std::string> setFoundIds;

    if ( CDirectory::FileList( pszDirName, clsFileList ) == false ) {
        CLog::Print( LOG_ERROR, "GroupMap ReadDir(%s) failed", pszDirName );
        return false;
    }

    for ( itFL = clsFileList.begin(); itFL != clsFileList.end(); ++itFL ) {
        CspPttGroup clsGroup;

        strFileName = pszDirName;
        CDirectory::AppendName( strFileName, itFL->c_str() );

        if ( clsGroup.load( strFileName.c_str() ) ) {
            Insert( clsGroup );
            setFoundIds.insert( clsGroup._id );

            std::string strMembers, strRegMembers;
            for ( const auto &pUser : clsGroup._pusers ) {
                if ( !strMembers.empty() ) strMembers += ", ";
                strMembers += pUser->_id;

                if ( gclsUserMap.Select( pUser->_id.c_str() ) ) {
                    if ( !strRegMembers.empty() ) strRegMembers += ", ";
                    strRegMembers += pUser->_id;
                }
            }
            CLog::Print( LOG_INFO, "GroupMap Loaded Group(%s: %s) Members[%s] RegMembers[%s]", clsGroup._id.c_str(),
                         clsGroup._name.c_str(), strMembers.c_str(), strRegMembers.c_str() );
        }
    }

    // Sync Logic: Remove groups not found in current directory scan
    m_clsMutex.lock();
    for ( auto it = m_clsMap.begin(); it != m_clsMap.end(); ) {
        if ( setFoundIds.find( it->first ) == setFoundIds.end() ) {
            CLog::Print( LOG_INFO, "GroupMap Removed Group(%s)", it->first.c_str() );
            m_clsMap.erase( it++ );
        } else {
            ++it;
        }
    }
    m_clsMutex.unlock();

    CLog::Print( LOG_INFO, "GroupMap: ReadDir finished. Total %d groups in map.", (int)m_clsMap.size() );
    return true;
}

/**
 * @brief Insert group into map
 */
void CGroupMap::Insert( CspPttGroup &clsGroup ) {
    GROUP_MAP::iterator itMap;

    m_clsMutex.lock();
    itMap = m_clsMap.find( clsGroup._id );
    if ( itMap == m_clsMap.end() ) {
        m_clsMap.insert( GROUP_MAP::value_type( clsGroup._id, clsGroup ) );
    } else {
        itMap->second = clsGroup;
    }
    m_clsMutex.unlock();
}

void CGroupMap::Remove( const char *pszGroupId ) {
    if ( pszGroupId == NULL ) return;
    m_clsMutex.lock();
    m_clsMap.erase( pszGroupId );
    m_clsMutex.unlock();
}

bool CGroupMap::Select( const char *pszGroupId, CspPttGroup &clsGroup ) {
    bool bRes = false;
    GROUP_MAP::iterator it;

    m_clsMutex.lock();
    it = m_clsMap.find( pszGroupId );
    if ( it != m_clsMap.end() ) {
        clsGroup = it->second;
        bRes = true;
    }
    m_clsMutex.unlock();

    return bRes;
}

bool CGroupMap::Contains( const char *pszGroupId ) {
    std::lock_guard<std::recursive_mutex> lock( m_clsMutex );
    return m_clsMap.find( pszGroupId ) != m_clsMap.end();
}

/**
 * @brief Clear map
 */
void CGroupMap::Clear() {
    m_clsMutex.lock();
    m_clsMap.clear();
    m_clsMutex.unlock();
}

void CGroupMap::IterateInternal( std::function<void( const CspPttGroup & )> fn ) {
    m_clsMutex.lock();
    for ( auto const &[key, group] : m_clsMap ) {
        fn( group );
    }
    m_clsMutex.unlock();
}

bool CGroupMap::FindGroupsByUser( std::string strUserId ) {
    bool bRet = false;
    m_clsMutex.lock();
    for ( auto const &[key, group] : m_clsMap ) {
        for ( auto const &user : group._pusers ) {
            if ( user->_id == strUserId ) {
                bRet = true;
                break;
            }
        }
        if ( bRet ) break;
    }
    m_clsMutex.unlock();

    return bRet;
}