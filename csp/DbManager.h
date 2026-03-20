/*
 * CIMS Database Manager Header
 * MariaDB C API wrapper for user and group data access
 */

#ifndef _DB_MANAGER_H_
#define _DB_MANAGER_H_

#include <string>
#include <mutex>
#include <mariadb/mysql.h>

class CspUser;
class CspPttGroup;
class CGroupMap;

/**
 * @ingroup CspServer
 * @brief MariaDB 연결 및 쿼리 실행을 담당하는 싱글턴 클래스
 */
class CDbManager {
public:
    CDbManager();
    ~CDbManager();

    /**
     * @brief MariaDB 에 연결한다
     * @return 성공하면 true
     */
    bool Connect( const std::string& strHost, const std::string& strUser,
                  const std::string& strPasswd, const std::string& strDb, int iPort = 3306 );
    void Disconnect();
    bool IsConnected() const;

    // ─────────────────────────────────────────────
    //  User operations
    // ─────────────────────────────────────────────

    /** 가입자 정보를 DB에서 조회한다 */
    bool SelectUser( const std::string& strUserId, CspUser& clsUser );

    /** 등록 시간을 갱신한다 */
    bool UpdateRegisterTime( const std::string& strUserId );

    /** 로그아웃 시간을 갱신한다 */
    bool UpdateLogoutTime( const std::string& strUserId );

    // ─────────────────────────────────────────────
    //  Group operations
    // ─────────────────────────────────────────────

    /** 단일 그룹을 DB에서 조회한다 */
    bool SelectGroup( const std::string& strGroupId, CspPttGroup& clsGroup );

    /** 전체 그룹을 DB에서 읽어 맵에 로드한다 */
    bool LoadAllGroups( CGroupMap& clsMap );

private:
    MYSQL*  m_pMysql;
    mutable std::recursive_mutex m_mutex;

    // 접속 정보 (재접속용)
    std::string m_strHost;
    std::string m_strUser;
    std::string m_strPasswd;
    std::string m_strDb;
    int         m_iPort;

    bool      Reconnect();
    bool      ExecuteQuery( const std::string& strSql );
    MYSQL_RES* ExecuteSelect( const std::string& strSql );

    /** SQL 인젝션 방지 이스케이프 */
    std::string Escape( const std::string& str );
};

extern CDbManager gclsDbManager;

#endif
