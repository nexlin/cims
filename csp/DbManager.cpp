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
#include <cstdio>

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

    // Try call users first (query by subscription MSISDN = id)
    std::string strSql =
        "SELECT cu.id, u.name, u.org_id, cu.auth_id, cu.passwd, cu.dnd, cu.forward_id, u.id AS person_id "
        "FROM cims_call_users cu JOIN cims_users u ON cu.user_id = u.id "
        "WHERE cu.id='" + Escape(strUserId) + "'";

    MYSQL_RES* pRes = ExecuteSelect(strSql);
    if (!pRes) return false;

    MYSQL_ROW row = mysql_fetch_row(pRes);
    if (!row) {
        mysql_free_result(pRes);

        // Try PTT users
        strSql =
            "SELECT pu.id, u.name, u.org_id, pu.auth_id, pu.passwd, pu.dnd, pu.forward_id, u.id AS person_id "
            "FROM cims_ptt_users pu JOIN cims_users u ON pu.user_id = u.id "
            "WHERE pu.id='" + Escape(strUserId) + "'";

        pRes = ExecuteSelect(strSql);
        if (!pRes) return false;

        row = mysql_fetch_row(pRes);
        if (!row) {
            mysql_free_result(pRes);
            return false;
        }
    }

    clsUser.m_strId             = row[0] ? row[0] : "";
    clsUser.m_strName           = row[1] ? row[1] : "";
    clsUser.m_strOrganizationId = row[2] ? row[2] : "";
    clsUser.m_strAuthId         = row[3] ? row[3] : "";
    clsUser.m_strPassWord       = row[4] ? row[4] : "";
    clsUser.m_bDnd              = row[5] ? (atoi(row[5]) != 0) : false;
    clsUser.m_strForward        = row[6] ? row[6] : "";
    clsUser._loadTime           = time(nullptr);
    // row[7] = person_id (cims_users.id) used for reject list lookup
    std::string strPersonId     = row[7] ? row[7] : strUserId;

    mysql_free_result(pRes);

    // 착신 거부 목록 로드 (person_id는 INT이므로 따옴표 없이 사용)
    clsUser.m_vecReject.clear();
    strSql = "SELECT reject_id FROM cims_user_rejects WHERE user_id=" + strPersonId;
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

    ExecuteQuery("UPDATE cims_call_users SET register_time=NOW() WHERE id='" + Escape(strUserId) + "'");
    ExecuteQuery("UPDATE cims_ptt_users  SET register_time=NOW() WHERE id='" + Escape(strUserId) + "'");
    return true;
}

bool CDbManager::UpdateLogoutTime( const std::string& strUserId )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    ExecuteQuery("UPDATE cims_call_users SET logout_time=NOW() WHERE id='" + Escape(strUserId) + "'");
    ExecuteQuery("UPDATE cims_ptt_users  SET logout_time=NOW() WHERE id='" + Escape(strUserId) + "'");
    return true;
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
        "SELECT id, name FROM cims_ptt_groups WHERE id='" + Escape(strGroupId) + "'";

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
        "SELECT user_id, priority FROM cims_ptt_group_members "
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
    MYSQL_RES* pRes = ExecuteSelect("SELECT id FROM cims_ptt_groups");
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

// ─────────────────────────────────────────────
//  Call log operations
// ─────────────────────────────────────────────

static std::string TimeToSql( time_t t )
{
    if ( t == 0 ) return "NULL";
    char buf[48];
    struct tm tm_val;
    localtime_r( &t, &tm_val );
    snprintf( buf, sizeof(buf), "'%04d-%02d-%02d %02d:%02d:%02d'",
              tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
              tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec );
    return buf;
}

bool CDbManager::InsertCallLog( const std::string& strCallId, bool bPtt,
                                  const std::string& strGroupId,
                                  const std::string& strInitiator,
                                  const std::string& strCallee )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    std::string strType    = bPtt ? "ptt" : "voip";
    std::string strGroupVal = strGroupId.empty() ? "NULL" : "'" + Escape(strGroupId) + "'";

    std::string strSql =
        "INSERT IGNORE INTO cims_call_logs "
        "(call_id, call_type, group_id, initiator, callee, state, invite_time) VALUES ('"
        + Escape(strCallId) + "','" + strType + "'," + strGroupVal + ",'"
        + Escape(strInitiator) + "','" + Escape(strCallee) + "','ringing',NOW())";

    return ExecuteQuery(strSql);
}

bool CDbManager::UpdateCallLogEnded( const std::string& strCallId,
                                      time_t tAnswer, time_t tEnd, int iSipStatus )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    std::string strAnswer = TimeToSql(tAnswer);
    std::string strEnd    = tEnd ? TimeToSql(tEnd) : "NOW()";
    int iDuration = (tAnswer > 0 && tEnd > tAnswer) ? (int)(tEnd - tAnswer) : 0;

    // map SIP status to end_reason
    const char* pszReason = "normal";
    if      (iSipStatus == 486)       pszReason = "busy";
    else if (iSipStatus == 487)       pszReason = "cancel";
    else if (iSipStatus == 408)       pszReason = "timeout";
    else if (iSipStatus >= 400)       pszReason = "error";

    char szDur[16];
    snprintf(szDur, sizeof(szDur), "%d", iDuration);

    std::string strState = (tAnswer > 0) ? "ended" : "ended";

    std::string strSql =
        "UPDATE cims_call_logs SET state='ended',"
        " answer_time=" + strAnswer + ","
        " end_time=" + strEnd + ","
        " duration=" + szDur + ","
        " sip_status=" + std::to_string(iSipStatus) + ","
        " end_reason='" + pszReason + "'"
        " WHERE call_id='" + Escape(strCallId) + "'";

    // also mark all participants left
    ExecuteQuery(
        "UPDATE cims_call_participants cp "
        "JOIN cims_call_logs cl ON cl.id = cp.log_id "
        "SET cp.leave_time=NOW() "
        "WHERE cl.call_id='" + Escape(strCallId) + "' AND cp.leave_time IS NULL"
    );

    return ExecuteQuery(strSql);
}

bool CDbManager::UpdateCallLogActivePtt( const std::string& strGroupId )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    return ExecuteQuery(
        "UPDATE cims_call_logs SET state='active', answer_time=NOW() "
        "WHERE group_id='" + Escape(strGroupId) + "' AND state='ringing' "
        "ORDER BY invite_time DESC LIMIT 1"
    );
}

bool CDbManager::EndGroupCallLog( const std::string& strGroupId )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    // duration = answer_time → NOW()
    ExecuteQuery(
        "UPDATE cims_call_logs "
        "SET state='ended', end_time=NOW(), "
        "duration=TIMESTAMPDIFF(SECOND, answer_time, NOW()), "
        "sip_status=200, end_reason='normal' "
        "WHERE group_id='" + Escape(strGroupId) + "' AND state IN ('ringing','active') "
        "ORDER BY invite_time DESC LIMIT 1"
    );

    // mark all remaining participants left
    return ExecuteQuery(
        "UPDATE cims_call_participants cp "
        "JOIN cims_call_logs cl ON cl.id = cp.log_id "
        "SET cp.leave_time=NOW() "
        "WHERE cl.group_id='" + Escape(strGroupId) + "' AND cp.leave_time IS NULL"
    );
}

bool CDbManager::InsertParticipant( const std::string& strCallId,
                                     const std::string& strMsisdn,
                                     const std::string& strRole,
                                     bool bJoinNow )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    std::string strJoin = bJoinNow ? "NOW()" : "NULL";
    return ExecuteQuery(
        "INSERT IGNORE INTO cims_call_participants (log_id, msisdn, role, join_time) "
        "SELECT id,'" + Escape(strMsisdn) + "','" + Escape(strRole) + "'," + strJoin + " "
        "FROM cims_call_logs WHERE call_id='" + Escape(strCallId) + "' LIMIT 1"
    );
}

bool CDbManager::InsertGroupParticipant( const std::string& strGroupId,
                                          const std::string& strMsisdn )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    return ExecuteQuery(
        "INSERT IGNORE INTO cims_call_participants (log_id, msisdn, role, join_time) "
        "SELECT id,'" + Escape(strMsisdn) + "','member',NULL "
        "FROM cims_call_logs "
        "WHERE group_id='" + Escape(strGroupId) + "' AND state IN ('ringing','active') "
        "ORDER BY invite_time DESC LIMIT 1"
    );
}

bool CDbManager::UpdateParticipantJoined( const std::string& strGroupId,
                                           const std::string& strMsisdn )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    return ExecuteQuery(
        "UPDATE cims_call_participants cp "
        "JOIN cims_call_logs cl ON cl.id = cp.log_id "
        "SET cp.join_time=NOW() "
        "WHERE cl.group_id='" + Escape(strGroupId) + "' "
        "AND cp.msisdn='" + Escape(strMsisdn) + "' AND cp.join_time IS NULL "
        "AND cl.state IN ('ringing','active')"
    );
}

bool CDbManager::UpdateParticipantLeft( const std::string& strGroupId,
                                         const std::string& strMsisdn )
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_pMysql && !Reconnect()) return false;

    return ExecuteQuery(
        "UPDATE cims_call_participants cp "
        "JOIN cims_call_logs cl ON cl.id = cp.log_id "
        "SET cp.leave_time=NOW() "
        "WHERE cl.group_id='" + Escape(strGroupId) + "' "
        "AND cp.msisdn='" + Escape(strMsisdn) + "' AND cp.leave_time IS NULL"
    );
}
