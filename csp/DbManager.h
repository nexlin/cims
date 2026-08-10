/*
 * CIMS Database Manager Header
 * MariaDB C API wrapper for user and group data access
 */

#ifndef _DB_MANAGER_H_
#define _DB_MANAGER_H_

#include <mariadb/mysql.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CspUser;
class CspUserMap;
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
    bool Connect( const std::string &strHost, const std::string &strUser, const std::string &strPasswd,
                  const std::string &strDb, int iPort = 3306 );
    void Disconnect();
    bool IsConnected() const;

    // DB 연결 상태 probe — FM 자기보고(alarm_self_reporting.md) 파일럿 훅.
    //   호출 경로와 mutex/연결을 공유하지 않는 **전용 연결**로 mysql_ping 하고,
    //   3연속 실패 시 down / 성공 시 up — 전이 시에만 콜백 (CmpClient KeepAliveLoop 관례).
    //   Connect() 이후 호출 (접속 정보 필요 — 최초 Connect 실패여도 접속 정보는 저장됨).
    void StartHealthProbe( std::function<void( bool )> fnStateChange );
    void StopHealthProbe();

    // ─────────────────────────────────────────────
    //  User operations
    // ─────────────────────────────────────────────

    /** 가입자 정보를 DB에서 조회한다 */
    bool SelectUser( const std::string &strUserId, CspUser &clsUser );

    /** 등록 시간을 갱신한다 */
    bool UpdateRegisterTime( const std::string &strUserId );

    /** 로그아웃 시간을 갱신한다 */
    bool UpdateLogoutTime( const std::string &strUserId );

    // ─────────────────────────────────────────────
    //  Group operations
    // ─────────────────────────────────────────────

    /** 단일 그룹을 DB에서 조회한다 */
    bool SelectGroup( const std::string &strGroupId, CspPttGroup &clsGroup );

    /** 전체 그룹을 DB에서 읽어 맵에 로드한다 */
    bool LoadAllGroups( CGroupMap &clsMap );

    /** 특정 사용자가 속한 그룹 ID 목록을 조회한다 */
    bool SelectGroupsByUser( const std::string &strUserId, std::vector<std::string> &vecGroupIds );

    // ─────────────────────────────────────────────
    //  Affiliation operations (TS 24.379 §9) — strGroupId = mcptt_group_id 식별자
    // ─────────────────────────────────────────────

    /** affiliation 등록(또는 갱신). iExpiresSec<=0 이면 만료 NULL */
    bool InsertAffiliation( const std::string &strGroupId, const std::string &strUserId, const std::string &strClientId,
                            int iExpiresSec );

    /** affiliation 해제 (clientId 가 비면 해당 user 전체) */
    bool RemoveAffiliation( const std::string &strGroupId, const std::string &strUserId,
                            const std::string &strClientId );

    /** 해당 그룹에 user 가 active affiliation(미만료) 을 1개라도 가지는지 */
    bool IsAffiliated( const std::string &strGroupId, const std::string &strUserId );

    /** 그룹의 affiliate 된 멤버 user_id 목록 */
    bool SelectAffiliatedMembers( const std::string &strGroupId, std::vector<std::string> &vecUserIds );

    /** 가입자 de-register/logout 시 전 affiliation 제거 */
    bool RemoveAffiliationsByUser( const std::string &strUserId );

    /** 전체 가입자를 DB에서 읽어 맵에 로드한다 */
    bool LoadAllUsers( CspUserMap &clsMap );

    // ─────────────────────────────────────────────
    //  Call log operations
    // ─────────────────────────────────────────────

    /** 통화 세션을 DB에 기록한다 (INVITE 시점, state=ringing) */
    bool InsertCallLog( const std::string &strCallId, bool bPtt, const std::string &strGroupId,
                        const std::string &strInitiator, const std::string &strCallee );

    /** 현재 활성 VoIP 통화 수 (state IN ('ringing','active')) */
    int GetActiveVoipCallCount();

    /** VoIP 통화 응답 시 state=active 로 변경 */
    bool UpdateCallLogActive( const std::string &strCallId );

    /** VoIP 통화 종료 시 CDR 정보로 업데이트 */
    bool UpdateCallLogEnded( const std::string &strCallId, time_t tAnswer, time_t tEnd, int iSipStatus );

    /** PTT: session_seq 증가 후 새 값 반환 (그룹 세션 시작 시) */
    int IncrementSessionSeq( const std::string &strGroupId );

    /** PTT: 해당 그룹에 활성 세션(ringing/active)이 존재하는지 확인 */
    bool HasActiveGroupCall( const std::string &strGroupId );

    /** PTT: 그룹 세션을 active 로 변경 (최초 멤버 응답 시) */
    bool UpdateCallLogActivePtt( const std::string &strGroupId );

    /** PTT: 그룹 세션 종료 (마지막 멤버 이탈 시) */
    bool EndGroupCallLog( const std::string &strGroupId );

    /** 녹취 레코드를 recordings 테이블에 삽입한다 */
    bool InsertRecording( const std::string &strCallId, const std::string &strCallType, const std::string &strGroupId,
                          const std::string &strCaller, const std::string &strCallee, const std::string &strRecordDir,
                          bool bHasVideo );

    /** 통화 참여자를 추가한다 */
    bool InsertParticipant( const std::string &strCallId, const std::string &strMsisdn, const std::string &strRole,
                            bool bJoinNow );

    /** PTT: group_id 기준 활성 세션에 참여자를 추가한다 (invited, join_time=NULL) */
    bool InsertGroupParticipant( const std::string &strGroupId, const std::string &strMsisdn );

    /** PTT: 참여자 연결 완료 (OnCallStarted) */
    bool UpdateParticipantJoined( const std::string &strGroupId, const std::string &strMsisdn );

    /** PTT: 참여자 이탈 (OnCallTerminated) */
    bool UpdateParticipantLeft( const std::string &strGroupId, const std::string &strMsisdn );

private:
    MYSQL *m_pMysql;
    mutable std::recursive_mutex m_mutex;

    // 접속 정보 (재접속용)
    std::string m_strHost;
    std::string m_strUser;
    std::string m_strPasswd;
    std::string m_strDb;
    int m_iPort;

    bool Reconnect();
    bool ExecuteQuery( const std::string &strSql );
    MYSQL_RES *ExecuteSelect( const std::string &strSql );

    void HealthProbeLoop();
    std::thread m_threadProbe;
    std::atomic<bool> m_bProbeRunning{ false };
    std::function<void( bool )> m_fnProbeCallback;

    /** SQL 인젝션 방지 이스케이프 */
    std::string Escape( const std::string &str );
};

extern CDbManager gclsDbManager;

#endif
