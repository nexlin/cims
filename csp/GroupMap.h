/*
 * Group Map Header
 */

#ifndef _GROUP_MAP_H_
#define _GROUP_MAP_H_

#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "CspPttGroup.h"

typedef std::map<std::string, CspPttGroup> GROUP_MAP;

/**
 * @ingroup CspServer
 * @brief Singleton class to manage Group definitions
 */
class CGroupMap {
public:
    CGroupMap();
    ~CGroupMap();

    /** Load all groups from directory (file mode) */
    bool Load( const char *pszDirName );

    /** Load all groups from DB */
    bool LoadFromDb();

    /** DB에서 특정 그룹 id 하나만 조회·로드 (전체 재로드 없음). 그룹이면 맵 삽입 후 true. */
    bool LoadOneFromDb( const char *pszGroupId );

    /** Insert a group */
    void Insert( CspPttGroup &clsGroup );

    /** Remove a group by ID (ad hoc 임시 그룹 정리용) */
    void Remove( const char *pszGroupId );

    /** DB 전체 재적재 결과로 맵을 한 번에 바꾼다 — 비우고 하나씩 채우면 그사이 조회가 멀쩡한 그룹을 못 찾는다
     *  (갓 만든 그룹의 MCVideo INVITE 404 Warning 113·affiliation 누락 등). ephemeral(_isAdhoc — ad hoc/private
     *  즉석 세션) 그룹은 DB 에 없으므로 같은 락 안에서 그대로 둔다 (재구축이 지우면 CheckMemberState 가
     *  "Group removed" 로 진행 중 호를 끊는다). */
    void ReplaceDbGroups( std::vector<CspPttGroup> &vecGroups );

    /** Select a group by ID */
    bool Select( const char *pszGroupId, CspPttGroup &clsGroup );

    /** Check if a group ID exists */
    bool Contains( const char *pszGroupId );

    // find group list by user-ID
    void IterateInternal( std::function<void( const CspPttGroup & )> fnCallback );

    bool FindGroupsByUser( std::string strUserId );

    /** Clear all groups */
    void Clear();

private:
    std::map<std::string, CspPttGroup> m_clsMap;
    std::recursive_mutex m_clsMutex;

    bool ReadDir( const char *pszDirName );
};

extern CGroupMap gclsGroupMap;

#endif
