/*
 * CIMS Database Manager
 * MariaDB C API wrapper for user and group data access
 */

#include "DbManager.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "CspPttGroup.h"
#include "CspUser.h"
#include "GroupMap.h"
#include "Log.h"

CDbManager gclsDbManager;

CDbManager::CDbManager() : m_pMysql( nullptr ), m_iPort( 3306 ) {
}

CDbManager::~CDbManager() {
    Disconnect();
}

// ─────────────────────────────────────────────
//  연결 관리
// ─────────────────────────────────────────────

bool CDbManager::Connect( const std::string& strHost, const std::string& strUser, const std::string& strPasswd,
                          const std::string& strDb, int iPort ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );

    m_strHost = strHost;
    m_strUser = strUser;
    m_strPasswd = strPasswd;
    m_strDb = strDb;
    m_iPort = iPort;

    if ( m_pMysql ) {
        mysql_close( m_pMysql );
        m_pMysql = nullptr;
    }

    m_pMysql = mysql_init( nullptr );
    if ( !m_pMysql ) {
        CLog::Print( LOG_ERROR, "[DB] mysql_init failed" );
        return false;
    }

    // 자동 재접속 옵션
    my_bool bReconnect = 1;
    mysql_options( m_pMysql, MYSQL_OPT_RECONNECT, &bReconnect );

    // utf8mb4 설정
    mysql_options( m_pMysql, MYSQL_SET_CHARSET_NAME, "utf8mb4" );

    if ( !mysql_real_connect( m_pMysql, strHost.c_str(), strUser.c_str(), strPasswd.c_str(), strDb.c_str(), iPort,
                              nullptr, 0 ) ) {
        CLog::Print( LOG_ERROR, "[DB] Connect failed: %s", mysql_error( m_pMysql ) );
        mysql_close( m_pMysql );
        m_pMysql = nullptr;
        return false;
    }

    CLog::Print( LOG_INFO, "[DB] Connected to %s:%d/%s", strHost.c_str(), iPort, strDb.c_str() );
    return true;
}

void CDbManager::Disconnect() {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( m_pMysql ) {
        mysql_close( m_pMysql );
        m_pMysql = nullptr;
    }
}

bool CDbManager::IsConnected() const {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    return m_pMysql != nullptr;
}

bool CDbManager::Reconnect() {
    if ( m_strHost.empty() ) return false;
    return Connect( m_strHost, m_strUser, m_strPasswd, m_strDb, m_iPort );
}

// ─────────────────────────────────────────────
//  내부 유틸
// ─────────────────────────────────────────────

bool CDbManager::ExecuteQuery( const std::string& strSql ) {
    if ( !m_pMysql ) return false;
    if ( mysql_query( m_pMysql, strSql.c_str() ) != 0 ) {
        CLog::Print( LOG_ERROR, "[DB] Query error: %s | SQL: %s", mysql_error( m_pMysql ), strSql.c_str() );
        return false;
    }
    return true;
}

MYSQL_RES* CDbManager::ExecuteSelect( const std::string& strSql ) {
    if ( !m_pMysql ) return nullptr;
    if ( mysql_query( m_pMysql, strSql.c_str() ) != 0 ) {
        CLog::Print( LOG_ERROR, "[DB] Select error: %s | SQL: %s", mysql_error( m_pMysql ), strSql.c_str() );
        return nullptr;
    }
    return mysql_store_result( m_pMysql );
}

std::string CDbManager::Escape( const std::string& str ) {
    if ( !m_pMysql ) return str;
    std::string out( str.size() * 2 + 1, '\0' );
    unsigned long len = mysql_real_escape_string( m_pMysql, &out[0], str.c_str(), str.size() );
    out.resize( len );
    return out;
}

// ─────────────────────────────────────────────
//  User operations
// ─────────────────────────────────────────────

bool CDbManager::SelectUser( const std::string& strUserId, CspUser& clsUser ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    // Try call users first (query by subscription MSISDN = id)
    //  v3 (2026-04-22): service_id INT → service_ref VARCHAR (access_services.name 참조)
    std::string strSql =
        "SELECT cu.id, u.name, u.org_id, cu.passwd, cu.dnd, cu.forward_id, u.id AS person_id, "
        "       COALESCE(cu.service_ref,''), COALESCE(cu.imsi,'') "
        "FROM volte_subscriptions cu JOIN users u ON cu.user_id = u.id "
        "WHERE cu.id='" +
        Escape( strUserId ) + "'";

    MYSQL_RES* pRes = ExecuteSelect( strSql );
    if ( !pRes ) return false;

    std::string strServiceType = "volte";
    MYSQL_ROW row = mysql_fetch_row( pRes );
    if ( !row ) {
        mysql_free_result( pRes );

        // Try PTT users
        strSql =
            "SELECT pu.id, u.name, u.org_id, pu.passwd, pu.dnd, pu.forward_id, u.id AS person_id, "
            "       COALESCE(pu.service_ref,''), COALESCE(pu.imsi,'') "
            "FROM ptt_subscriptions pu JOIN users u ON pu.user_id = u.id "
            "WHERE pu.id='" +
            Escape( strUserId ) + "'";

        pRes = ExecuteSelect( strSql );
        if ( !pRes ) return false;

        row = mysql_fetch_row( pRes );
        if ( !row ) {
            mysql_free_result( pRes );
            return false;
        }
        strServiceType = "ptt";
    }

    clsUser.m_strId = row[0] ? row[0] : "";
    clsUser.m_strServiceType = strServiceType;
    clsUser.m_strName = row[1] ? row[1] : "";
    clsUser.m_strOrganizationId = row[2] ? row[2] : "";
    clsUser.m_strPassWord = row[3] ? row[3] : "";
    clsUser.m_bDnd = row[4] ? ( atoi( row[4] ) != 0 ) : false;
    clsUser.m_strForward = row[5] ? row[5] : "";
    // row[6] = person_id (users.id) used for reject list lookup
    std::string strPersonId = row[6] ? row[6] : strUserId;
    clsUser.m_strServiceRef = row[7] ? row[7] : "";
    clsUser.m_strImsi = row[8] ? row[8] : "";
    clsUser._loadTime = time( nullptr );

    mysql_free_result( pRes );

    // 착신 거부 목록 로드 (person_id는 INT이므로 따옴표 없이 사용)
    clsUser.m_vecReject.clear();
    strSql = "SELECT reject_id FROM user_rejects WHERE user_id=" + strPersonId;
    pRes = ExecuteSelect( strSql );
    if ( pRes ) {
        while ( ( row = mysql_fetch_row( pRes ) ) != nullptr ) {
            if ( row[0] ) clsUser.m_vecReject.push_back( row[0] );
        }
        mysql_free_result( pRes );
    }

    return true;
}

bool CDbManager::UpdateRegisterTime( const std::string& strUserId ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    ExecuteQuery( "UPDATE volte_subscriptions SET register_time=NOW() WHERE id='" + Escape( strUserId ) + "'" );
    ExecuteQuery( "UPDATE ptt_subscriptions  SET register_time=NOW() WHERE id='" + Escape( strUserId ) + "'" );
    return true;
}

bool CDbManager::UpdateLogoutTime( const std::string& strUserId ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    ExecuteQuery( "UPDATE volte_subscriptions SET logout_time=NOW() WHERE id='" + Escape( strUserId ) + "'" );
    ExecuteQuery( "UPDATE ptt_subscriptions  SET logout_time=NOW() WHERE id='" + Escape( strUserId ) + "'" );
    return true;
}

// ─────────────────────────────────────────────
//  Group operations
// ─────────────────────────────────────────────

bool CDbManager::SelectGroup( const std::string& strGroupId, CspPttGroup& clsGroup ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    // 그룹 기본 정보 (확장 필드 포함)
    std::string strSql =
        "SELECT id, name, video_enabled, priority, encryption, emergency_call, "
        "org_code, UNIX_TIMESTAMP(session_start) AS ss, UNIX_TIMESTAMP(session_end) AS se, "
        "session_seq "
        "FROM ptt_groups WHERE id='" +
        Escape( strGroupId ) + "'";

    MYSQL_RES* pRes = ExecuteSelect( strSql );
    if ( !pRes ) return false;

    MYSQL_ROW row = mysql_fetch_row( pRes );
    if ( !row ) {
        mysql_free_result( pRes );
        return false;
    }

    clsGroup.Clear();
    clsGroup._id = row[0] ? row[0] : "";
    clsGroup._name = row[1] ? row[1] : "";
    clsGroup._videoEnabled = row[2] ? ( atoi( row[2] ) != 0 ) : false;
    clsGroup._priority = row[3] ? atoi( row[3] ) : 5;
    clsGroup._encryption = row[4] ? ( atoi( row[4] ) != 0 ) : false;
    clsGroup._emergencyCall = row[5] ? ( atoi( row[5] ) != 0 ) : false;
    clsGroup._orgCode = row[6] ? row[6] : "";
    clsGroup._sessionStart = row[7] ? (time_t)atoll( row[7] ) : 0;
    clsGroup._sessionEnd = row[8] ? (time_t)atoll( row[8] ) : 0;
    clsGroup._sessionSeq = row[9] ? atoi( row[9] ) : 1;
    mysql_free_result( pRes );

    // 멤버 목록
    strSql =
        "SELECT user_id, priority FROM ptt_group_members "
        "WHERE group_id='" +
        Escape( strGroupId ) + "' ORDER BY priority";

    pRes = ExecuteSelect( strSql );
    if ( pRes ) {
        while ( ( row = mysql_fetch_row( pRes ) ) != nullptr ) {
            if ( !row[0] ) continue;
            std::string uid = row[0];
            int prio = row[1] ? atoi( row[1] ) : 0;
            auto pUser = std::make_shared<CspPttUser>( uid, prio );
            pUser->_groups.push_back( clsGroup._id );
            clsGroup._pusers.push_back( pUser );
        }
        mysql_free_result( pRes );
    }

    CLog::Print( LOG_INFO, "[DB] SelectGroup(%s) %d members", strGroupId.c_str(), (int)clsGroup._pusers.size() );
    return true;
}

bool CDbManager::LoadAllUsers( CspUserMap& clsMap ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    int count = 0;
    const char* aTables[] = { "volte_subscriptions", "ptt_subscriptions" };
    const char* aTypes[] = { "volte", "ptt" };

    for ( int i = 0; i < 2; ++i ) {
        // v3 (2026-04-22): service_id INT → service_ref VARCHAR (access_services.name 참조)
        std::string strSql = std::string(
                                 "SELECT s.id, u.name, u.org_id, s.passwd, s.dnd, s.forward_id, u.id, "
                                 "       COALESCE(s.service_ref, ''), COALESCE(s.imsi, '') "
                                 "FROM " ) +
                             aTables[i] + " s JOIN users u ON s.user_id = u.id";

        MYSQL_RES* pRes = ExecuteSelect( strSql );
        if ( !pRes ) continue;

        MYSQL_ROW row;
        while ( ( row = mysql_fetch_row( pRes ) ) != nullptr ) {
            CspUser clsUser;
            clsUser.m_strId = row[0] ? row[0] : "";
            clsUser.m_strServiceType = aTypes[i];
            clsUser.m_strName = row[1] ? row[1] : "";
            clsUser.m_strOrganizationId = row[2] ? row[2] : "";
            clsUser.m_strPassWord = row[3] ? row[3] : "";
            clsUser.m_bDnd = row[4] ? ( atoi( row[4] ) != 0 ) : false;
            clsUser.m_strForward = row[5] ? row[5] : "";
            clsUser.m_strServiceRef = row[7] ? row[7] : "";
            clsUser.m_strImsi = row[8] ? row[8] : "";
            clsUser._loadTime = time( nullptr );
            if ( !clsUser.m_strId.empty() ) {
                clsMap.Insert( clsUser );
                ++count;
            }
        }
        mysql_free_result( pRes );
    }

    CLog::Print( LOG_INFO, "[DB] LoadAllUsers: %d users loaded", count );
    return count > 0;
}

bool CDbManager::SelectGroupsByUser( const std::string& strUserId, std::vector<std::string>& vecGroupIds ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    std::string strSql = "SELECT group_id FROM ptt_group_members WHERE user_id='" + Escape( strUserId ) + "'";
    MYSQL_RES* pRes = ExecuteSelect( strSql );
    if ( !pRes ) return false;

    MYSQL_ROW row;
    while ( ( row = mysql_fetch_row( pRes ) ) != nullptr ) {
        if ( row[0] ) vecGroupIds.push_back( row[0] );
    }
    mysql_free_result( pRes );
    return true;
}

bool CDbManager::LoadAllGroups( CGroupMap& clsMap ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    // 전체 그룹 ID 목록 조회
    MYSQL_RES* pRes = ExecuteSelect( "SELECT id FROM ptt_groups" );
    if ( !pRes ) return false;

    std::vector<std::string> vecGroupIds;
    MYSQL_ROW row;
    while ( ( row = mysql_fetch_row( pRes ) ) != nullptr ) {
        if ( row[0] ) vecGroupIds.push_back( row[0] );
    }
    mysql_free_result( pRes );

    // 그룹별 멤버 로드 및 맵 삽입
    clsMap.Clear();
    for ( const auto& strId : vecGroupIds ) {
        CspPttGroup clsGroup;
        if ( SelectGroup( strId, clsGroup ) ) {
            clsMap.Insert( clsGroup );
        }
    }

    CLog::Print( LOG_INFO, "[DB] LoadAllGroups: %d groups loaded", (int)vecGroupIds.size() );
    return true;
}

// ─────────────────────────────────────────────
//  Call log operations
// ─────────────────────────────────────────────

static std::string TimeToSql( time_t t ) {
    if ( t == 0 ) return "NULL";
    char buf[48];
    struct tm tm_val;
    localtime_r( &t, &tm_val );
    snprintf( buf, sizeof( buf ), "'%04d-%02d-%02d %02d:%02d:%02d'", tm_val.tm_year + 1900, tm_val.tm_mon + 1,
              tm_val.tm_mday, tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec );
    return buf;
}

// v3 (2026-04-22): Call/PTT log 는 파일 기반 (service_log/{type}/YYYY/MM/DD/HH/.../*.d/call.json) 이 SOT.
//   CspServer 의 CCallDir 가 파일 작성, CSC 의 /api/v1/call/logs 가 파일 스캔.
//   아래 DbManager 함수들은 레거시 호환 no-op (빌드/링크 유지용).

bool CDbManager::InsertCallLog( const std::string&, bool, const std::string&, const std::string&, const std::string& ) {
    return true;  // no-op, file-based CallDir 가 담당
}

int CDbManager::GetActiveVoipCallCount() {
    return 0;  // no-op, 파일 기반 조회는 CSC 가 수행
}

bool CDbManager::UpdateCallLogActive( const std::string& ) {
    return true;
}

bool CDbManager::UpdateCallLogEnded( const std::string&, time_t, time_t, int ) {
    return true;  // no-op
}

bool CDbManager::HasActiveGroupCall( const std::string& ) {
    return false;  // no-op, 그룹 상태는 GroupCallService 내 메모리/파일 기반
}

bool CDbManager::UpdateCallLogActivePtt( const std::string& ) {
    return true;
}

bool CDbManager::EndGroupCallLog( const std::string& ) {
    return true;
}

bool CDbManager::InsertRecording( const std::string& strCallId, const std::string& strCallType,
                                  const std::string& strGroupId, const std::string& strCaller,
                                  const std::string& strCallee, const std::string& strRecordDir, bool bHasVideo ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    std::string rawA = strRecordDir + "/raw_a.rtp";
    std::string rawB = strRecordDir + "/raw_b.rtp";
    std::string rawVA = bHasVideo ? strRecordDir + "/raw_va.rtp" : "";
    std::string rawVB = bHasVideo ? strRecordDir + "/raw_vb.rtp" : "";

    std::string strSql =
        "INSERT IGNORE INTO recordings "
        "(call_id, call_type, group_id, caller, callee, start_time, "
        " raw_path_a, raw_path_b, raw_path_va, raw_path_vb, has_video, status) "
        "VALUES ('" +
        Escape( strCallId ) + "','" + Escape( strCallType ) + "'," +
        ( strGroupId.empty() ? "NULL" : "'" + Escape( strGroupId ) + "'" ) +
        ","
        "'" +
        Escape( strCaller ) + "','" + Escape( strCallee ) +
        "',NOW(),"
        "'" +
        Escape( rawA ) + "','" + Escape( rawB ) + "'," + ( rawVA.empty() ? "NULL" : "'" + Escape( rawVA ) + "'" ) +
        "," + ( rawVB.empty() ? "NULL" : "'" + Escape( rawVB ) + "'" ) + "," + ( bHasVideo ? "1" : "0" ) + ",'raw')";

    return ExecuteQuery( strSql );
}

// v3: 참가자 기록은 파일 (participants.jsonl) 기반 SOT.
bool CDbManager::InsertParticipant( const std::string&, const std::string&, const std::string&, bool ) {
    return true;
}
bool CDbManager::InsertGroupParticipant( const std::string&, const std::string& ) {
    return true;
}
bool CDbManager::UpdateParticipantJoined( const std::string&, const std::string& ) {
    return true;
}
bool CDbManager::UpdateParticipantLeft( const std::string&, const std::string& ) {
    return true;
}

int CDbManager::IncrementSessionSeq( const std::string& strGroupId ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return 1;

    ExecuteQuery( "UPDATE ptt_groups SET session_seq = session_seq + 1 WHERE id='" + Escape( strGroupId ) + "'" );

    MYSQL_RES* pRes = ExecuteSelect( "SELECT session_seq FROM ptt_groups WHERE id='" + Escape( strGroupId ) + "'" );
    if ( !pRes ) return 1;
    MYSQL_ROW row = mysql_fetch_row( pRes );
    int seq = row && row[0] ? atoi( row[0] ) : 1;
    mysql_free_result( pRes );
    return seq;
}
