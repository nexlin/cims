/*
 * Group Map Header
 */

#ifndef _GROUP_MAP_H_
#define _GROUP_MAP_H_

#include "CspPttGroup.h"
#include <map>
#include <functional>
#include <mutex>

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

    /** Insert a group */
    void Insert( CspPttGroup &clsGroup );

    /** Select a group by ID */
    bool Select( const char *pszGroupId, CspPttGroup &clsGroup );

    // find group list by user-ID
    void IterateInternal( std::function<void(const CspPttGroup&)> fnCallback );

    bool FindGroupsByUser(std::string strUserId);

    /** Clear all groups */
    void Clear();

private:
    std::map<std::string, CspPttGroup> m_clsMap;
    std::recursive_mutex m_clsMutex;

    bool ReadDir( const char *pszDirName );
};

extern CGroupMap gclsGroupMap;

#endif
