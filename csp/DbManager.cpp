/*
 * CIMS Database Manager
 * MariaDB C API wrapper for user and group data access
 */

#include "DbManager.h"
#include "CspUser.h"
#include "CspPttGroup.h"
#include "GroupMap.h"
#include "Log.h"
#include <cstring>
#include <ctime>

CDbManager gclsDbManager;

CDbManager::CDbManager()
    : m_pMysql(nullptr), m_iPort(3306)
{
}

CDbManager::~CDbManager()
{
    Disconnect();
}

// ─────────────────────────────────────────────
//  연결 관리
// ─────────────────────────────────────────────

bool CDbManager::Connect( const std::string& strHost, const std::string& strUser,
                           const std::string& strPasswd, const std::string& strDb, int iPort )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    m_strHost   = strHost;
    m_strUser   = strUser;
    m_strPasswd = strPasswd;
    m_strDb     = strDb;
    m_iPort     = iPort;

    if (m_pMysql) {
        mysql_close(m_pMysql);
        m_pMysql = nullptr;
    }

    m_pMysql = mysql_init(nullptr);
    if (!m_pMysql) {
        CLog::Print(LOG_ERROR, "[DB] mysql_init failed");
        return false;
    }

    // 자동 재접속 옵션
    my_bool bReconnect = 1;
    mysql_options(m_pMysql, MYSQL_OPT_RECONNECT, &bReconnect);

    // utf8mb4 설정
    mysql_options(m_pMysql, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (!mysql_real_connect(m_pMysql,
                            strHost.c_str(),
                            strUser.c_str(),
                            strPasswd.c_str(),
                            strDb.c_str(),
                            iPort, nullptr, 0))
    {
        CLog::Print(LOG_ERROR, "[DB] Connect failed: %s", mysql_error(m_pMysql));
        mysql_close(m_pMysql);
        m_pMysql = nullptr;
        return false;
    }

    CLog::Print(LOG_INFO, "[DB] Connected to %s:%d/%s", strHost.c_str(), iPort, strDb.c_str());
    return true;
}

void CDbManager::Disconnect()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_pMysql) {
        mysql_close(m_pMysql);
        m_pMysql = nullptr;
    }
}

bool CDbManager::IsConnected() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_pMysql != nullptr;
}

bool CDbManager::Reconnect()
{
    if (m_strHost.empty()) return false;
    return Connect(m_strHost, m_strUser, m_strPasswd, m_strDb, m_iPort);
}

// ─────────────────────────────────────────────
//  내부 유틸
// ─────────────────────────────────────────────

bool CDbManager::ExecuteQuery( const std::string& strSql )
{
    if (!m_pMysql) return false;
    if (mysql_query(m_pMysql, strSql.c_str()) != 0) {
        CLog::Print(LOG_ERROR, "[DB] Query error: %s | SQL: %s",
                    mysql_error(m_pMysql), strSql.c_str());
        return false;
    }
    return true;
}

MYSQL_RES* CDbManager::ExecuteSelect( const std::string& strSql )
{
    if (!m_pMysql) return nullptr;
    if (mysql_query(m_pMysql, strSql.c_str()) != 0) {
        CLog::Print(LOG_ERROR, "[DB] Select error: %s | SQL: %s",
                    mysql_error(m_pMysql), strSql.c_str());
        return nullptr;
    }
    return mysql_store_result(m_pMysql);
}

std::string CDbManager::Escape( const std::string& str )
{
    if (!m_pMysql) return str;
    std::string out(str.size() * 2 + 1, '\0');
    unsigned long len = mysql_real_escape_string(m_pMysql, &out[0], str.c_str(), str.size());
    out.resize(len);
    return out;
}

// ─────────────────────────────────────────────
//  User operations
// ─────────────────────────────────────────────

bool CDbManager::SelectUser( const std::string& strUserId, CspUser& clsUser )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    std::string strSql =
        "SELECT id, auth_id, passwd, org_id, dnd, forward_id "
        "FROM cims_users WHERE id='" + Escape(strUserId) + "'";

    MYSQL_RES* pRes = ExecuteSelect(strSql);
    if (!pRes) return false;

    MYSQL_ROW row = mysql_fetch_row(pRes);
    if (!row) {
        mysql_free_result(pRes);
        return false;
    }

    clsUser.m_strId             = row[0] ? row[0] : "";
    clsUser.m_strAuthId         = row[1] ? row[1] : "";
    clsUser.m_strPassWord       = row[2] ? row[2] : "";
    clsUser.m_strOrganizationId = row[3] ? row[3] : "";
    clsUser.m_bDnd              = row[4] ? (atoi(row[4]) != 0) : false;
    clsUser.m_strForward        = row[5] ? row[5] : "";
    clsUser._loadTime           = time(nullptr);

    mysql_free_result(pRes);

    // 착신 거부 목록 로드
    clsUser.m_vecReject.clear();
    strSql = "SELECT reject_id FROM cims_user_rejects WHERE user_id='" + Escape(strUserId) + "'";
    pRes = ExecuteSelect(strSql);
    if (pRes) {
        while ((row = mysql_fetch_row(pRes)) != nullptr) {
            if (row[0]) clsUser.m_vecReject.push_back(row[0]);
        }
        mysql_free_result(pRes);
    }

    return true;
}

bool CDbManager::UpdateRegisterTime( const std::string& strUserId )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    std::string strSql =
        "UPDATE cims_users SET register_time=NOW() WHERE id='" + Escape(strUserId) + "'";
    return ExecuteQuery(strSql);
}

bool CDbManager::UpdateLogoutTime( const std::string& strUserId )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    std::string strSql =
        "UPDATE cims_users SET logout_time=NOW() WHERE id='" + Escape(strUserId) + "'";
    return ExecuteQuery(strSql);
}

// ─────────────────────────────────────────────
//  Group operations
// ─────────────────────────────────────────────

bool CDbManager::SelectGroup( const std::string& strGroupId, CspPttGroup& clsGroup )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    // 그룹 기본 정보
    std::string strSql =
        "SELECT id, name FROM cims_groups WHERE id='" + Escape(strGroupId) + "'";

    MYSQL_RES* pRes = ExecuteSelect(strSql);
    if (!pRes) return false;

    MYSQL_ROW row = mysql_fetch_row(pRes);
    if (!row) {
        mysql_free_result(pRes);
        return false;
    }

    clsGroup.Clear();
    clsGroup._id   = row[0] ? row[0] : "";
    clsGroup._name = row[1] ? row[1] : "";
    mysql_free_result(pRes);

    // 멤버 목록
    strSql =
        "SELECT user_id, priority FROM cims_group_members "
        "WHERE group_id='" + Escape(strGroupId) + "' ORDER BY priority";

    pRes = ExecuteSelect(strSql);
    if (pRes) {
        while ((row = mysql_fetch_row(pRes)) != nullptr) {
            if (!row[0]) continue;
            std::string uid = row[0];
            int prio = row[1] ? atoi(row[1]) : 0;
            auto pUser = std::make_shared<CspPttUser>(uid, prio);
            pUser->_groups.push_back(clsGroup._id);
            clsGroup._pusers.push_back(pUser);
        }
        mysql_free_result(pRes);
    }

    CLog::Print(LOG_INFO, "[DB] SelectGroup(%s) %d members", strGroupId.c_str(), (int)clsGroup._pusers.size());
    return true;
}

bool CDbManager::LoadAllGroups( CGroupMap& clsMap )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    // 전체 그룹 ID 목록 조회
    MYSQL_RES* pRes = ExecuteSelect("SELECT id FROM cims_groups");
    if (!pRes) return false;

    std::vector<std::string> vecGroupIds;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(pRes)) != nullptr) {
        if (row[0]) vecGroupIds.push_back(row[0]);
    }
    mysql_free_result(pRes);

    // 그룹별 멤버 로드 및 맵 삽입
    clsMap.Clear();
    for (const auto& strId : vecGroupIds) {
        CspPttGroup clsGroup;
        if (SelectGroup(strId, clsGroup)) {
            clsMap.Insert(clsGroup);
        }
    }

    CLog::Print(LOG_INFO, "[DB] LoadAllGroups: %d groups loaded", (int)vecGroupIds.size());
    return true;
}
