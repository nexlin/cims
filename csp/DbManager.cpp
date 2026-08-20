/*
 * CIMS Database Manager
 * MariaDB C API wrapper for user and group data access
 */

#include "DbManager.h"

#include <unistd.h>

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

bool CDbManager::Connect( const std::string &strHost, const std::string &strUser, const std::string &strPasswd,
                          const std::string &strDb, int iPort ) {
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

    // ⚠️데드락 방지 핵심: connect/read/write 타임아웃 설정.
    //   단일 연결을 recursive_mutex 로 직렬화하므로, 연결이 half-open 으로 멈추면
    //   mysql_query 가 무한 블록 → 락 영구 보유 → 모든 SIP 처리 스레드(REGISTER/group)가
    //   m_mutex 에서 wedge → SIP UDP 소켓 미드레인 → csp 전체 데드락(재기동 외 복구불가).
    //   타임아웃을 두면 stall 쿼리가 (무한 대신) 유한 시간 후 실패 → 락 해제 → 회복 가능
    //   (MYSQL_OPT_RECONNECT 가 다음 쿼리에서 재접속). PTT 40 동시 REGISTER 버스트서 노출됨.
    unsigned int uConnTimeout = 5;   // 초
    unsigned int uReadTimeout = 5;   // 초 (쿼리 응답 대기 상한)
    unsigned int uWriteTimeout = 5;  // 초
    mysql_options( m_pMysql, MYSQL_OPT_CONNECT_TIMEOUT, &uConnTimeout );
    mysql_options( m_pMysql, MYSQL_OPT_READ_TIMEOUT, &uReadTimeout );
    mysql_options( m_pMysql, MYSQL_OPT_WRITE_TIMEOUT, &uWriteTimeout );

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
    ProbeSchema();
    return true;
}

void CDbManager::ProbeSchema() {
    // ha1 은 volte/ptt 두 테이블에 같은 마이그레이션으로 들어가므로 한쪽만 본다.
    MYSQL_RES *pRes = ExecuteSelect( "SHOW COLUMNS FROM volte_subscriptions LIKE 'ha1'" );
    m_bHasHa1Column = pRes && mysql_num_rows( pRes ) > 0;
    if ( pRes ) mysql_free_result( pRes );
    if ( !m_bHasHa1Column )
        CLog::Print( LOG_ERROR,
                     "[DB] subscriptions.ha1 column absent — migrate_subscription_ha1.sql 미적용. Digest 는 passwd "
                     "fallback 으로 동작한다 (sip_access_security.md §4.7 ①)" );
}

std::string CDbManager::Ha1Col( const char *pszAlias ) const {
    if ( !m_bHasHa1Column ) return "''";
    return std::string( "COALESCE(" ) + pszAlias + ".ha1,'')";
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
//  Health probe (FM 자기보고 파일럿 — alarm_self_reporting.md)
// ─────────────────────────────────────────────

static const int kDbProbePeriodSec = 10;  // probe 주기
static const int kDbProbeMaxFail = 3;     // 연속 실패 임계 — 초과 시 down 전이

void CDbManager::StartHealthProbe( std::function<void( bool )> fnStateChange ) {
    if ( m_bProbeRunning ) return;
    m_fnProbeCallback = fnStateChange;
    m_bProbeRunning = true;
    m_threadProbe = std::thread( &CDbManager::HealthProbeLoop, this );
}

void CDbManager::StopHealthProbe() {
    if ( !m_bProbeRunning ) return;
    m_bProbeRunning = false;
    if ( m_threadProbe.joinable() ) m_threadProbe.join();
}

void CDbManager::HealthProbeLoop() {
    // 전용 probe 연결 — 호출 경로의 m_pMysql/m_mutex 와 완전 분리. 쿼리 경로가
    //   half-open 으로 5s stall 하는 동안에도 probe 는 독립적으로 상태를 판정한다.
    MYSQL *pProbe = nullptr;
    int iFailCount = 0;
    bool bUp = true;  // 기동 시 up 가정 — 3연속 실패 후 첫 down 전이
    int iTick = 0;

    while ( m_bProbeRunning ) {
        sleep( 1 );
        if ( ++iTick < kDbProbePeriodSec ) continue;
        iTick = 0;

        bool bOk = false;
        if ( !pProbe ) {
            std::string strHost, strUser, strPasswd, strDb;
            int iPort;
            {
                std::lock_guard<std::recursive_mutex> lock( m_mutex );
                strHost = m_strHost;
                strUser = m_strUser;
                strPasswd = m_strPasswd;
                strDb = m_strDb;
                iPort = m_iPort;
            }
            if ( strHost.empty() ) continue;  // Connect() 전 — 판정 보류
            pProbe = mysql_init( nullptr );
            if ( pProbe ) {
                unsigned int uTimeout = 3;
                mysql_options( pProbe, MYSQL_OPT_CONNECT_TIMEOUT, &uTimeout );
                mysql_options( pProbe, MYSQL_OPT_READ_TIMEOUT, &uTimeout );
                mysql_options( pProbe, MYSQL_OPT_WRITE_TIMEOUT, &uTimeout );
                if ( mysql_real_connect( pProbe, strHost.c_str(), strUser.c_str(), strPasswd.c_str(), strDb.c_str(),
                                         iPort, nullptr, 0 ) ) {
                    bOk = true;
                } else {
                    mysql_close( pProbe );
                    pProbe = nullptr;
                }
            }
        } else {
            if ( mysql_ping( pProbe ) == 0 ) {
                bOk = true;
            } else {
                mysql_close( pProbe );
                pProbe = nullptr;
            }
        }

        if ( bOk ) {
            iFailCount = 0;
            if ( !bUp ) {
                bUp = true;
                CLog::Print( LOG_SYSTEM, "[DB] health probe RECOVERED" );
                if ( m_fnProbeCallback ) m_fnProbeCallback( true );
            }
        } else {
            iFailCount++;
            if ( bUp && iFailCount >= kDbProbeMaxFail ) {
                bUp = false;
                CLog::Print( LOG_ERROR, "[DB] health probe DOWN (%d연속 실패)", iFailCount );
                if ( m_fnProbeCallback ) m_fnProbeCallback( false );
            }
        }
    }

    if ( pProbe ) mysql_close( pProbe );
}

// ─────────────────────────────────────────────
//  내부 유틸
// ─────────────────────────────────────────────

bool CDbManager::ExecuteQuery( const std::string &strSql ) {
    if ( !m_pMysql ) return false;
    if ( mysql_query( m_pMysql, strSql.c_str() ) != 0 ) {
        CLog::Print( LOG_ERROR, "[DB] Query error: %s | SQL: %s", mysql_error( m_pMysql ), strSql.c_str() );
        return false;
    }
    return true;
}

MYSQL_RES *CDbManager::ExecuteSelect( const std::string &strSql ) {
    if ( !m_pMysql ) return nullptr;
    if ( mysql_query( m_pMysql, strSql.c_str() ) != 0 ) {
        CLog::Print( LOG_ERROR, "[DB] Select error: %s | SQL: %s", mysql_error( m_pMysql ), strSql.c_str() );
        return nullptr;
    }
    return mysql_store_result( m_pMysql );
}

std::string CDbManager::Escape( const std::string &str ) {
    if ( !m_pMysql ) return str;
    std::string out( str.size() * 2 + 1, '\0' );
    unsigned long len = mysql_real_escape_string( m_pMysql, &out[0], str.c_str(), str.size() );
    out.resize( len );
    return out;
}

// ─────────────────────────────────────────────
//  User operations
// ─────────────────────────────────────────────

bool CDbManager::SelectUser( const std::string &strUserId, CspUser &clsUser ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    // Try call users first (query by subscription MSISDN = id)
    //  v3 (2026-04-22): service_id INT → service_ref VARCHAR (access_services.name 참조)
    std::string strSql =
        "SELECT cu.id, u.name, u.org_id, cu.passwd, cu.dnd, cu.forward_id, u.id AS person_id, "
        "       COALESCE(cu.service_ref,''), COALESCE(cu.imsi,''), " +
        Ha1Col( "cu" ) +
        ", COALESCE(cu.sip_transport,'') "
        "FROM volte_subscriptions cu JOIN users u ON cu.user_id = u.id "
        "WHERE cu.id='" +
        Escape( strUserId ) + "'";

    MYSQL_RES *pRes = ExecuteSelect( strSql );
    if ( !pRes ) return false;

    std::string strServiceType = "volte";
    MYSQL_ROW row = mysql_fetch_row( pRes );
    if ( !row ) {
        mysql_free_result( pRes );

        // Try PTT users
        strSql =
            "SELECT pu.id, u.name, u.org_id, pu.passwd, pu.dnd, pu.forward_id, u.id AS person_id, "
            "       COALESCE(pu.service_ref,''), COALESCE(pu.imsi,''), " +
            Ha1Col( "pu" ) +
            ", COALESCE(pu.sip_transport,'') "
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
    clsUser.m_strHa1 = row[9] ? row[9] : "";
    clsUser.m_strSipTransport = row[10] ? row[10] : "";
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

int CDbManager::SelectUserProfile( const std::string &strUserId, CspUserProfile &clsProfile ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    clsProfile = CspUserProfile();
    if ( !m_pMysql && !Reconnect() ) return -1;

    std::string strSql =
        "SELECT allow_emergency_call, allow_emergency_alert, allow_adhoc_call, "
        "       emergency_group_mode, COALESCE(emergency_group_id,'') "
        "FROM ptt_user_profile WHERE ptt_id='" +
        Escape( strUserId ) + "'";

    MYSQL_RES *pRes = ExecuteSelect( strSql );
    if ( !pRes ) return -1;  // 마이그레이션 전 테이블 부재 포함 — 호출측 fail-open

    MYSQL_ROW row = mysql_fetch_row( pRes );
    if ( !row ) {
        mysql_free_result( pRes );
        return 0;
    }
    clsProfile.m_bAllowEmergencyCall = row[0] ? ( atoi( row[0] ) != 0 ) : true;
    clsProfile.m_bAllowEmergencyAlert = row[1] ? ( atoi( row[1] ) != 0 ) : true;
    clsProfile.m_bAllowAdhocCall = row[2] ? ( atoi( row[2] ) != 0 ) : true;
    if ( row[3] && row[3][0] ) clsProfile.m_strEmergencyGroupMode = row[3];
    clsProfile.m_strEmergencyGroupId = row[4] ? row[4] : "";
    mysql_free_result( pRes );
    return 1;
}

bool CDbManager::UpdateRegisterTime( const std::string &strUserId ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    ExecuteQuery( "UPDATE volte_subscriptions SET register_time=NOW() WHERE id='" + Escape( strUserId ) + "'" );
    ExecuteQuery( "UPDATE ptt_subscriptions  SET register_time=NOW() WHERE id='" + Escape( strUserId ) + "'" );
    return true;
}

bool CDbManager::UpdateLogoutTime( const std::string &strUserId ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    ExecuteQuery( "UPDATE volte_subscriptions SET logout_time=NOW() WHERE id='" + Escape( strUserId ) + "'" );
    ExecuteQuery( "UPDATE ptt_subscriptions  SET logout_time=NOW() WHERE id='" + Escape( strUserId ) + "'" );
    // de-register 시 affiliation 해제 (TS 24.379 §9 — 제휴는 등록에 묶인다).
    //   가입자의 **전 그룹 제휴를 한 번에 지우는 유일한 경로**이므로 반드시 흔적을 남긴다.
    //   종전엔 무로그였고, 그래서 "제휴 테이블이 비었다" 를 조사할 때 지운 주체를 특정할 수
    //   없었다(로그·binlog 모두 없음). 지운 행 수까지 남겨 no-op 과 실제 회수를 구분한다.
    if ( ExecuteQuery( "DELETE FROM ptt_affiliations WHERE user_id='" + Escape( strUserId ) + "'" ) ) {
        const unsigned long long ullRows = mysql_affected_rows( m_pMysql );
        if ( ullRows > 0 ) {
            CLog::Print( LOG_INFO, "[Affiliation] de-register 회수 user=%s rows=%llu", strUserId.c_str(), ullRows );
        }
    }
    return true;
}

// ─────────────────────────────────────────────
//  Group operations
// ─────────────────────────────────────────────

bool CDbManager::SelectGroup( const std::string &strGroupId, CspPttGroup &clsGroup ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    // 그룹 기본 정보 (확장 필드 포함). strGroupId = mcptt_group_id 식별자.
    std::string strSql =
        "SELECT g.id, g.mcptt_group_id, g.name, g.video_enabled, g.priority, g.encryption, g.emergency_call, "
        "g.org_code, UNIX_TIMESTAMP(g.session_start) AS ss, UNIX_TIMESTAMP(g.session_end) AS se, "
        "g.session_seq, g.group_type, g.on_network, g.max_members, g.require_affiliation, COALESCE(g.alias,''), "
        "COALESCE(g.authorized_user_id,0), "
        "COALESCE(DATE_FORMAT(g.created_at,'%Y-%m-%dT%H:%i:%s'),''), "
        "COALESCE(ps.id,''), "
        "g.emergency_alert, "
        "g.allow_sds, g.allow_fd, g.max_sds_size, "
        "g.floor_policy, g.max_talkers "
        "FROM ptt_groups g "
        "LEFT JOIN ptt_subscriptions ps ON ps.user_id = g.authorized_user_id "
        "WHERE g.mcptt_group_id='" +
        Escape( strGroupId ) + "'";

    MYSQL_RES *pRes = ExecuteSelect( strSql );
    if ( !pRes ) return false;

    MYSQL_ROW row = mysql_fetch_row( pRes );
    if ( !row ) {
        mysql_free_result( pRes );
        return false;
    }

    clsGroup.Clear();
    clsGroup._dbId = row[0] ? atoll( row[0] ) : 0;
    clsGroup._id = row[1] ? row[1] : "";
    clsGroup._name = row[2] ? row[2] : "";
    clsGroup._videoEnabled = row[3] ? ( atoi( row[3] ) != 0 ) : false;
    clsGroup._priority = row[4] ? atoi( row[4] ) : 5;
    clsGroup._encryption = row[5] ? ( atoi( row[5] ) != 0 ) : false;
    clsGroup._emergencyCall = row[6] ? ( atoi( row[6] ) != 0 ) : false;
    clsGroup._orgCode = row[7] ? row[7] : "";
    clsGroup._sessionStart = row[8] ? (time_t)atoll( row[8] ) : 0;
    clsGroup._sessionEnd = row[9] ? (time_t)atoll( row[9] ) : 0;
    clsGroup._sessionSeq = row[10] ? atoi( row[10] ) : 1;
    clsGroup._groupType = row[11] ? row[11] : "prearranged";
    clsGroup._onNetwork = row[12] ? ( atoi( row[12] ) != 0 ) : true;
    clsGroup._maxMembers = row[13] ? atoi( row[13] ) : 0;
    clsGroup._requireAffiliation = row[14] ? ( atoi( row[14] ) != 0 ) : true;
    clsGroup._alias = row[15] ? row[15] : "";
    clsGroup._authorizedUserId = row[16] ? atoi( row[16] ) : 0;
    clsGroup._createdAt = row[17] ? row[17] : "";
    clsGroup._authorizedUser = row[18] ? row[18] : "";  // 소유자 PTT MSISDN (파생 MCPTT ID)
    clsGroup._emergencyAlert = row[19] ? ( atoi( row[19] ) != 0 ) : true;
    clsGroup._allowSds = row[20] ? ( atoi( row[20] ) != 0 ) : true;
    clsGroup._allowFd = row[21] ? ( atoi( row[21] ) != 0 ) : false;
    clsGroup._maxSdsSize = row[22] ? atoi( row[22] ) : 10000;
    clsGroup._floorPolicy = row[23] ? row[23] : "single";
    clsGroup._maxTalkers = row[24] ? atoi( row[24] ) : 2;
    mysql_free_result( pRes );

    // 멤버 목록 — group_id 는 surrogate ptt_groups.id 참조
    char szDbId[32];
    snprintf( szDbId, sizeof( szDbId ), "%lld", clsGroup._dbId );
    strSql =
        "SELECT user_id, priority, role, COALESCE(mcptt_id,'') FROM ptt_group_members "
        "WHERE group_id=" +
        std::string( szDbId ) + " ORDER BY priority";

    pRes = ExecuteSelect( strSql );
    if ( pRes ) {
        while ( ( row = mysql_fetch_row( pRes ) ) != nullptr ) {
            if ( !row[0] ) continue;
            std::string uid = row[0];
            int prio = row[1] ? atoi( row[1] ) : 0;
            std::string role = row[2] ? row[2] : "participant";
            std::string mcpttId = row[3] ? row[3] : "";
            auto pUser = std::make_shared<CspPttUser>( uid, prio, role, mcpttId );
            pUser->_groups.push_back( clsGroup._id );
            clsGroup._pusers.push_back( pUser );
        }
        mysql_free_result( pRes );
    }

    CLog::Print( LOG_INFO, "[DB] SelectGroup(%s) dbId=%lld %d members", strGroupId.c_str(), clsGroup._dbId,
                 (int)clsGroup._pusers.size() );
    return true;
}

bool CDbManager::LoadAllUsers( CspUserMap &clsMap ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    int count = 0;
    const char *aTables[] = { "volte_subscriptions", "ptt_subscriptions" };
    const char *aTypes[] = { "volte", "ptt" };

    for ( int i = 0; i < 2; ++i ) {
        // v3 (2026-04-22): service_id INT → service_ref VARCHAR (access_services.name 참조)
        std::string strSql = std::string(
                                 "SELECT s.id, u.name, u.org_id, s.passwd, s.dnd, s.forward_id, u.id, "
                                 "       COALESCE(s.service_ref, ''), COALESCE(s.imsi, ''), "
                                 "       " ) +
                             Ha1Col( "s" ) + ", COALESCE(s.sip_transport, '') FROM " + aTables[i] +
                             " s JOIN users u ON s.user_id = u.id";

        MYSQL_RES *pRes = ExecuteSelect( strSql );
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
            clsUser.m_strHa1 = row[9] ? row[9] : "";
            clsUser.m_strSipTransport = row[10] ? row[10] : "";
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

bool CDbManager::SelectGroupsByUser( const std::string &strUserId, std::vector<std::string> &vecGroupIds ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    // mcptt_group_id 식별자 반환 (멤버 group_id surrogate → ptt_groups JOIN)
    std::string strSql =
        "SELECT g.mcptt_group_id FROM ptt_group_members m "
        "JOIN ptt_groups g ON m.group_id = g.id WHERE m.user_id='" +
        Escape( strUserId ) + "'";
    MYSQL_RES *pRes = ExecuteSelect( strSql );
    if ( !pRes ) return false;

    MYSQL_ROW row;
    while ( ( row = mysql_fetch_row( pRes ) ) != nullptr ) {
        if ( row[0] ) vecGroupIds.push_back( row[0] );
    }
    mysql_free_result( pRes );
    return true;
}

// ─────────────────────────────────────────────
//  Affiliation operations (TS 24.379 §9)
//  strGroupId = mcptt_group_id 식별자 → 내부에서 surrogate id 서브쿼리로 해석
// ─────────────────────────────────────────────

bool CDbManager::InsertAffiliation( const std::string &strGroupId, const std::string &strUserId,
                                    const std::string &strClientId, int iExpiresSec ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    // mcptt_group_id → surrogate id 를 **먼저 확정**한다.
    //   종전 구현은 `INSERT ... SELECT ... FROM ptt_groups WHERE mcptt_group_id=..` 한 방이었는데,
    //   서브쿼리가 비면 **에러 없이 0행**을 써서 호출자가 "제휴 등록됨" 으로 오판했다(침묵 실패 —
    //   DB 는 비어 있는데 로그만 affiliate 로 남아 제휴 소실 추적이 불가능했다).
    //   affected_rows 로도 구분할 수 없다: ON DUPLICATE KEY UPDATE 는 갱신값이 기존과 같으면
    //   (같은 초에 재발행·PUBLISH 재전송 등) 0 을 반환하므로 "미발견" 과 "무변경" 이 겹친다.
    //   그래서 그룹 조회를 분리해 그 결과로 성공/실패를 판정한다 (제휴는 빈발 경로가 아니라
    //   쿼리 1회 추가 비용은 무의미하다).
    std::string strGroupPk;
    {
        MYSQL_RES *pRes =
            ExecuteSelect( "SELECT id FROM ptt_groups WHERE mcptt_group_id='" + Escape( strGroupId ) + "' LIMIT 1" );
        if ( !pRes ) return false;
        MYSQL_ROW row = mysql_fetch_row( pRes );
        if ( row && row[0] ) strGroupPk = row[0];
        mysql_free_result( pRes );
    }
    if ( strGroupPk.empty() ) return false;   // 그룹 미발견 = 기록 불가

    std::string strExpires =
        ( iExpiresSec > 0 ) ? ( "DATE_ADD(NOW(), INTERVAL " + std::to_string( iExpiresSec ) + " SECOND)" ) : "NULL";

    // UPSERT (status 재활성). group_id 는 자기 테이블 BIGINT 조회값이므로 숫자다.
    std::string strSql = "INSERT INTO ptt_affiliations (group_id, user_id, client_id, expires_at, status) VALUES (" +
                         strGroupPk + ", '" + Escape( strUserId ) + "', '" + Escape( strClientId ) + "', " +
                         strExpires + ", 'affiliated') ON DUPLICATE KEY UPDATE affiliated_at=NOW(), expires_at=" +
                         strExpires + ", status='affiliated'";
    return ExecuteQuery( strSql );
}

bool CDbManager::RemoveAffiliation( const std::string &strGroupId, const std::string &strUserId,
                                    const std::string &strClientId ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    std::string strSql =
        "DELETE a FROM ptt_affiliations a JOIN ptt_groups g ON a.group_id=g.id "
        "WHERE g.mcptt_group_id='" +
        Escape( strGroupId ) + "' AND a.user_id='" + Escape( strUserId ) + "'";
    if ( !strClientId.empty() ) strSql += " AND a.client_id='" + Escape( strClientId ) + "'";
    return ExecuteQuery( strSql );
}

bool CDbManager::IsAffiliated( const std::string &strGroupId, const std::string &strUserId ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    std::string strSql =
        "SELECT 1 FROM ptt_affiliations a JOIN ptt_groups g ON a.group_id=g.id "
        "WHERE g.mcptt_group_id='" +
        Escape( strGroupId ) + "' AND a.user_id='" + Escape( strUserId ) +
        "' AND a.status='affiliated' "
        "AND (a.expires_at IS NULL OR a.expires_at > NOW()) LIMIT 1";
    MYSQL_RES *pRes = ExecuteSelect( strSql );
    if ( !pRes ) return false;
    bool bFound = ( mysql_fetch_row( pRes ) != nullptr );
    mysql_free_result( pRes );
    return bFound;
}

bool CDbManager::SelectAffiliatedMembers( const std::string &strGroupId, std::vector<std::string> &vecUserIds ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    std::string strSql =
        "SELECT DISTINCT a.user_id FROM ptt_affiliations a JOIN ptt_groups g ON a.group_id=g.id "
        "WHERE g.mcptt_group_id='" +
        Escape( strGroupId ) +
        "' AND a.status='affiliated' "
        "AND (a.expires_at IS NULL OR a.expires_at > NOW())";
    MYSQL_RES *pRes = ExecuteSelect( strSql );
    if ( !pRes ) return false;
    MYSQL_ROW row;
    while ( ( row = mysql_fetch_row( pRes ) ) != nullptr ) {
        if ( row[0] ) vecUserIds.push_back( row[0] );
    }
    mysql_free_result( pRes );
    return true;
}

bool CDbManager::RemoveAffiliationsByUser( const std::string &strUserId ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    return ExecuteQuery( "DELETE FROM ptt_affiliations WHERE user_id='" + Escape( strUserId ) + "'" );
}

bool CDbManager::LoadAllGroups( CGroupMap &clsMap ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return false;

    // 전체 그룹 식별자(mcptt_group_id) 목록 조회
    MYSQL_RES *pRes = ExecuteSelect( "SELECT mcptt_group_id FROM ptt_groups" );
    if ( !pRes ) return false;

    std::vector<std::string> vecGroupIds;
    MYSQL_ROW row;
    while ( ( row = mysql_fetch_row( pRes ) ) != nullptr ) {
        if ( row[0] ) vecGroupIds.push_back( row[0] );
    }
    mysql_free_result( pRes );

    // ephemeral(ad hoc/private 즉석 세션) 그룹은 DB 에 없다 — 전체 재구축이 지우면
    // CheckMemberState 가 "Group removed" 로 진행 중 호를 끊는다(60초 재로드마다). 보존·재삽입.
    std::vector<CspPttGroup> vecEphemeral;
    clsMap.CollectEphemeral( vecEphemeral );

    // 그룹별 멤버 로드 및 맵 삽입
    clsMap.Clear();
    for ( const auto &strId : vecGroupIds ) {
        CspPttGroup clsGroup;
        if ( SelectGroup( strId, clsGroup ) ) {
            clsMap.Insert( clsGroup );
        }
    }
    for ( auto &clsEph : vecEphemeral ) clsMap.Insert( clsEph );

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

bool CDbManager::InsertCallLog( const std::string &, bool, const std::string &, const std::string &,
                                const std::string & ) {
    return true;  // no-op, file-based CallDir 가 담당
}

int CDbManager::GetActiveVoipCallCount() {
    return 0;  // no-op, 파일 기반 조회는 CSC 가 수행
}

bool CDbManager::UpdateCallLogActive( const std::string & ) {
    return true;
}

bool CDbManager::UpdateCallLogEnded( const std::string &, time_t, time_t, int ) {
    return true;  // no-op
}

bool CDbManager::HasActiveGroupCall( const std::string & ) {
    return false;  // no-op, 그룹 상태는 GroupCallService 내 메모리/파일 기반
}

bool CDbManager::UpdateCallLogActivePtt( const std::string & ) {
    return true;
}

bool CDbManager::EndGroupCallLog( const std::string & ) {
    return true;
}

bool CDbManager::InsertRecording( const std::string &, const std::string &, const std::string &, const std::string &,
                                  const std::string &, const std::string &, bool ) {
    // v3 후속: recordings 메타데이터는 파일 기반 (CallDir 의 call.json + recordings/ 디렉토리).
    // CSC `/api/v1/recordings` 가 파일 스캔으로 응답. DB 기록 no-op.
    return true;
}

// v3: 참가자 기록은 파일 (participants.jsonl) 기반 SOT.
bool CDbManager::InsertParticipant( const std::string &, const std::string &, const std::string &, bool ) {
    return true;
}
bool CDbManager::InsertGroupParticipant( const std::string &, const std::string & ) {
    return true;
}
bool CDbManager::UpdateParticipantJoined( const std::string &, const std::string & ) {
    return true;
}
bool CDbManager::UpdateParticipantLeft( const std::string &, const std::string & ) {
    return true;
}

int CDbManager::IncrementSessionSeq( const std::string &strGroupId ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    if ( !m_pMysql && !Reconnect() ) return 1;

    ExecuteQuery( "UPDATE ptt_groups SET session_seq = session_seq + 1 WHERE mcptt_group_id='" + Escape( strGroupId ) +
                  "'" );

    MYSQL_RES *pRes =
        ExecuteSelect( "SELECT session_seq FROM ptt_groups WHERE mcptt_group_id='" + Escape( strGroupId ) + "'" );
    if ( !pRes ) return 1;
    MYSQL_ROW row = mysql_fetch_row( pRes );
    int seq = row && row[0] ? atoi( row[0] ) : 1;
    mysql_free_result( pRes );
    return seq;
}
