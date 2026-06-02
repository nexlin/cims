#ifndef _CALL_DIR_H_
#define _CALL_DIR_H_

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

class CCallDir {
public:
    void Init( const std::string &strBaseDir, const std::string &strComponent ) {
        if ( strBaseDir.empty() ) return;
        m_strCallsDir = strBaseDir;
        m_strComponent = strComponent;
        MkdirP( m_strCallsDir );
        MkdirP( m_strCallsDir + "/state/volte" );
        MkdirP( m_strCallsDir + "/state/ptt" );
        CleanupStaleStates();
    }

    bool IsEnabled() const {
        return !m_strCallsDir.empty();
    }

    /** 기동 시 stale 가입자 상태 파일 일괄 제거.
     *  CSP 재시작 후에는 SIP 다이얼로그가 모두 소실되므로 이전 state 는 무조건 stale. */
    void CleanupStaleStates() {
        if ( m_strCallsDir.empty() ) return;
        _purgeStateDir( m_strCallsDir + "/state/volte" );
        _purgeStateDir( m_strCallsDir + "/state/ptt" );
    }

    // ── Session-ID 관리 ────────────────────────────────

    /** Session-ID 생성 (통화 세션 고유 ID) */
    static std::string GenerateSessionId() {
        char buf[64];
        struct timespec ts;
        clock_gettime( CLOCK_REALTIME, &ts );
        struct tm t;
        localtime_r( &ts.tv_sec, &t );
        snprintf( buf, sizeof( buf ), "S%04d%02d%02d%02d%02d%02d%06ld", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000 );
        return buf;
    }

    /** Call-ID → Session-ID 매핑 등록 (B2BUA에서 두 leg 모두 등록) */
    void MapCallToSession( const std::string &strCallId, const std::string &strSessionId ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        m_mapCallSession[strCallId] = strSessionId;
    }

    /** Call-ID에서 Session-ID 조회 (없으면 빈 문자열) */
    std::string GetSessionId( const std::string &strCallId ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        auto it = m_mapCallSession.find( strCallId );
        return ( it != m_mapCallSession.end() ) ? it->second : "";
    }

    /** Session-ID에서 log_dir 조회 */
    std::string GetSessionDir( const std::string &strSessionId ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        return _dir( strSessionId );
    }

    /** Write session mapping (session.json) to the .d directory */
    void WriteSessionMapping( const std::string &strSessionId, const std::string &strCallIdA,
                              const std::string &strCallIdB, const std::string &strSesId = "" ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        std::string dir = _dir( strSessionId );
        if ( dir.empty() ) {
            // Try via call-id
            dir = _dir( strCallIdA );
        }
        if ( dir.empty() ) return;
        std::string path = dir + "/session.json";
        FILE *f = fopen( path.c_str(), "w" );
        if ( !f ) return;
        if ( strSesId.empty() ) {
            fprintf( f, "{\"session_id\":\"%s\",\"call_ids\":[\"%s\",\"%s\"]}\n", Esc( strSessionId ).c_str(),
                     Esc( strCallIdA ).c_str(), Esc( strCallIdB ).c_str() );
        } else {
            fprintf( f, "{\"session_id\":\"%s\",\"sesid\":\"%s\",\"call_ids\":[\"%s\",\"%s\"]}\n",
                     Esc( strSessionId ).c_str(), Esc( strSesId ).c_str(), Esc( strCallIdA ).c_str(),
                     Esc( strCallIdB ).c_str() );
        }
        fclose( f );
    }

    // ── VoIP ─────────────────────────────────────────────

    /** VoIP 세션 디렉터리 생성. Session-ID 기반. */
    std::string GetVoipDir( const std::string &strCallId, const std::string &strCaller,
                            const std::string &strCallee = "" ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        // Call-ID → Session-ID 매핑이 있으면 해당 세션 디렉터리 반환
        auto itSess = m_mapCallSession.find( strCallId );
        if ( itSess != m_mapCallSession.end() ) {
            auto itDir = m_mapDir.find( itSess->second );
            if ( itDir != m_mapDir.end() ) {
                m_mapDir[strCallId] = itDir->second;
                return itDir->second;
            }
            // Session-ID는 있지만 디렉터리 없음 — 발신 leg으로 다시 조회
            for ( auto &[cid, sid] : m_mapCallSession ) {
                if ( sid == itSess->second && cid != strCallId ) {
                    auto itDir2 = m_mapDir.find( cid );
                    if ( itDir2 != m_mapDir.end() ) {
                        m_mapDir[itSess->second] = itDir2->second;
                        m_mapDir[strCallId] = itDir2->second;
                        return itDir2->second;
                    }
                }
            }
        }
        // 기존 Call-ID로도 조회
        auto it = m_mapDir.find( strCallId );
        if ( it != m_mapDir.end() ) return it->second;

        // 새 디렉터리 생성 (Session-ID가 있으면 사용, 없으면 Call-ID)
        std::string key = strCallId;
        if ( itSess != m_mapCallSession.end() ) key = itSess->second;

        std::string yyyy, mm, dd, hh;
        DateHour( yyyy, mm, dd, hh );
        std::string sc = San( strCaller, 20 );
        std::string dir = m_strCallsDir + "/volte/" + yyyy + "/" + mm + "/" + dd + "/" + hh + "/" + Prefix( sc ) + "/" +
                          sc + "/" + San( key, 80 ) + ".d";
        MkdirP( dir );
        m_mapDir[key] = dir;
        // Call-ID로도 같은 디렉터리를 찾을 수 있게
        if ( key != strCallId ) m_mapDir[strCallId] = dir;
        return dir;
    }

    void VoipCallStart( const std::string &strCallId, const std::string &strCaller, const std::string &strCallee,
                        bool bVideo = false ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        std::string dir = _dir( strCallId );
        if ( dir.empty() ) return;
        std::string path = dir + "/call.json";
        struct stat st;
        if ( stat( path.c_str(), &st ) == 0 ) return;
        char ts[32];
        IsoNow( ts, sizeof( ts ) );
        const char *pszCallType = bVideo ? "volte_video" : "volte";
        FILE *f = fopen( path.c_str(), "w" );
        if ( !f ) return;
        fprintf( f,
                 "{\"call_id\":\"%s\",\"call_type\":\"%s\","
                 "\"initiator\":\"%s\",\"callee\":\"%s\","
                 "\"state\":\"ringing\",\"invite_time\":\"%s\","
                 "\"answer_time\":null,\"end_time\":null,"
                 "\"duration\":0,\"end_reason\":null}\n",
                 Esc( strCallId ).c_str(), pszCallType, Esc( strCaller ).c_str(), Esc( strCallee ).c_str(), ts );
        fclose( f );

        // 가입자별 실시간 상태 파일 — 발/착신 각각
        std::string sessId = _sessionIdByCallId( strCallId );
        _writeVoipState( strCaller, strCallId, sessId, strCallee, "caller", "ringing", ts, dir, bVideo );
        _writeVoipState( strCallee, strCallId, sessId, strCaller, "callee", "ringing", ts, dir, bVideo );
    }

    void VoipCallAnswer( const std::string &strCallId ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        std::string dir = _dir( strCallId );
        if ( dir.empty() ) return;
        std::string path = dir + "/call.json";
        std::string c = _readFile( path );
        if ( c.empty() ) return;
        char ts[32];
        IsoNow( ts, sizeof( ts ) );
        _replace( c, "\"state\":\"ringing\"", "\"state\":\"active\"" );
        _replace( c, "\"answer_time\":null", std::string( "\"answer_time\":\"" ) + ts + "\"" );
        FILE *f = fopen( path.c_str(), "w" );
        if ( f ) {
            fputs( c.c_str(), f );
            fclose( f );
        }

        // state/volte/ 하위 해당 call_id 매칭 파일 active 로 승격
        _promoteVoipStates( strCallId, ts );
    }

    void VoipCallEnd( const std::string &strCallId, const std::string &strReason = "normal", int iDur = 0 ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        std::string dir = _dir( strCallId );
        if ( dir.empty() ) return;
        _updateCallJson( dir, strReason, iDur );
        _appendIndex( dir, strCallId, "volte" );
        m_mapDir.erase( strCallId );

        // state/volte/ 하위 해당 call_id 매칭 파일 제거
        _removeVoipStatesByCallId( strCallId );
    }

    void VoipAddParticipant( const std::string &strCallId, const std::string &strMsisdn, const std::string &strRole ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        std::string dir = _dir( strCallId );
        if ( dir.empty() ) return;
        char ts[32];
        IsoNow( ts, sizeof( ts ) );
        FILE *f = fopen( ( dir + "/participants.jsonl" ).c_str(), "a" );
        if ( f ) {
            fprintf( f, "{\"msisdn\":\"%s\",\"role\":\"%s\",\"join_time\":\"%s\",\"leave_time\":null}\n",
                     Esc( strMsisdn ).c_str(), strRole.c_str(), ts );
            fclose( f );
        }
    }

    // ── PTT ──────────────────────────────────────────────

    /** PTT 그룹 base 디렉터리 — ptt/{group_id}.
     *  고빈도 데이터(녹취 세그먼트·floor)는 CMP 가 이 base 하위에
     *  {YYYY}/{MM}/{DD}/{HH}/ 시간버킷 + seg/{NNN}/ shard 로 회전 기록한다.
     *  (옛 sessions/{key}.d 단일 누적 구조 폐지 → 시간검색·디렉터리 엔트리수 상한)
     *  반환값이 CMP record_dir(base)로 전달된다. */
    std::string GetPttSessionDir( const std::string &strGroupId, const std::string &strSessionStart = "",
                                  const std::string &strStorageId = "" ) {
        std::lock_guard<std::mutex> lock( m_mtx );

        // 저장 경로 키: ptt_groups.id(surrogate) 우선 — mcptt_group_id 변경에도 불변.
        //   storageId 없거나 "0" 이면 groupId(mcptt) fallback.
        std::string key = ( !strStorageId.empty() && strStorageId != "0" ) ? strStorageId : strGroupId;
        std::string sg = San( key, 32 );
        std::string base = m_strCallsDir + "/ptt/" + sg;
        struct stat st;
        if ( stat( base.c_str(), &st ) != 0 ) MkdirP( base );

        // 런타임 맵은 mcptt_group_id 로 키잉(다른 PTT 메서드가 group._id 로 조회).
        m_mapPttSession[strGroupId] = base;
        m_mapPttSessionId[strGroupId] = _sessionKey( strSessionStart );
        return base;
    }

    /** 그룹 base 하위 현재 시각 시간버킷 ptt/{gid}/{YYYY}/{MM}/{DD}/{HH} (생성 후 반환) */
    std::string _pttHourDir( const std::string &base ) {
        std::string yyyy, mm, dd, hh;
        DateHour( yyyy, mm, dd, hh );
        std::string dir = base + "/" + yyyy + "/" + mm + "/" + dd + "/" + hh;
        struct stat st;
        if ( stat( dir.c_str(), &st ) != 0 ) MkdirP( dir );
        return dir;
    }

    /** strGroupJson = CGroupCallService::BuildGroupDescriptor 가 만든 자기완결 디스크립터
     *  (계획서 §5). group.json 은 디스크립터 그대로 + state/updated_at 만 주입해 기록한다
     *  (옛 placeholder session_id/call_id/initiator 제거). strCallId/strInitiator 는
     *  가입자 state 파일 기록용으로만 사용. */
    void PttSessionStart( const std::string &strGroupId, const std::string &strCallId, const std::string &strInitiator,
                          const std::string &strGroupJson = "{}" ) {
        // GetPttSessionDir가 이미 호출된 상태여야 함
        std::string dir, sessId;
        {
            std::lock_guard<std::mutex> lock( m_mtx );
            auto it = m_mapPttSession.find( strGroupId );
            if ( it == m_mapPttSession.end() ) return;
            dir = it->second;  // 그룹 base

            // group.json (자기완결 그룹 디스크립터) — base 루트에 1개. 없으면 생성.
            std::string path = dir + "/group.json";
            struct stat st;
            bool bNew = ( stat( path.c_str(), &st ) != 0 );

            sessId = m_mapPttSessionId[strGroupId];
            char ts[32];
            IsoNow( ts, sizeof( ts ) );

            if ( bNew ) {
                // 디스크립터 객체에 state/updated_at 주입 (말미 '}' 직전에 삽입).
                std::string desc = strGroupJson;
                if ( desc.empty() || desc.back() != '}' ) desc = "{}";
                desc.pop_back();
                if ( !desc.empty() && desc.back() != '{' ) desc += ",";
                desc += "\"state\":\"active\",\"updated_at\":\"" + std::string( ts ) + "\"}";

                FILE *f = fopen( path.c_str(), "w" );
                if ( f ) {
                    fprintf( f, "%s\n", desc.c_str() );
                    fclose( f );
                }
            }

            // 초기 가입자(발신자) 상태 파일 기록 (state/ptt/{sub}.json — 버킷과 독립)
            if ( !strInitiator.empty() && strInitiator != "autojoin" ) {
                _writePttState( strInitiator, strGroupId, sessId, strCallId, "initiator", ts, dir );
            }
        }
    }

    void PttSessionEnd( const std::string &strGroupId ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        _endSessionLocked( strGroupId );
        _removePttStatesByGroupId( strGroupId );
    }

    /** 멤버 join — member_join 이벤트 로그 + 상태 파일 기록 */
    void PttMemberJoin( const std::string &strGroupId, const std::string &strMemberId,
                        const std::string &strCallId = "" ) {
        // 이벤트 로그 (기존 PttLogEvent 경로 재사용)
        PttLogEvent( strGroupId, "member_join", "{\"member\":\"" + Esc( strMemberId ) + "\"}" );

        std::lock_guard<std::mutex> lock( m_mtx );
        auto it = m_mapPttSession.find( strGroupId );
        if ( it == m_mapPttSession.end() ) return;
        std::string dir = it->second;
        std::string sessId = m_mapPttSessionId[strGroupId];
        char ts[32];
        IsoNow( ts, sizeof( ts ) );
        _writePttState( strMemberId, strGroupId, sessId, strCallId, "member", ts, dir );
    }

    /** 멤버 leave — member_leave 이벤트 로그 + 상태 파일 제거 */
    void PttMemberLeave( const std::string &strGroupId, const std::string &strMemberId ) {
        PttLogEvent( strGroupId, "member_leave", "{\"member\":\"" + Esc( strMemberId ) + "\"}" );

        std::lock_guard<std::mutex> lock( m_mtx );
        _removePttState( strMemberId );
    }

    void PttLogEvent( const std::string &strGroupId, const std::string &strType, const std::string &strJsonData ) {
        if ( m_strCallsDir.empty() ) return;
        std::lock_guard<std::mutex> lock( m_mtx );
        auto it = m_mapPttSession.find( strGroupId );
        if ( it == m_mapPttSession.end() ) return;
        std::string dir = it->second;
        char ts[32];
        IsoNow( ts, sizeof( ts ) );

        // Build merged line: {"ts":"...","type":"...", ...data fields}
        std::string line = "{\"ts\":\"" + std::string( ts ) + "\",\"type\":\"" + Esc( strType ) + "\"";
        // Merge jsonData fields (strip outer braces)
        if ( !strJsonData.empty() && strJsonData.front() == '{' && strJsonData.back() == '}' ) {
            std::string inner = strJsonData.substr( 1, strJsonData.size() - 2 );
            if ( !inner.empty() ) {
                line += "," + inner;
            }
        }
        line += "}";

        // 시간버킷 events.jsonl 에 append (dir=그룹 base → {YYYY}/{MM}/{DD}/{HH}/events.jsonl)
        std::string hourDir = _pttHourDir( dir );
        FILE *f = fopen( ( hourDir + "/events.jsonl" ).c_str(), "a" );
        if ( f ) {
            fprintf( f, "%s\n", line.c_str() );
            fclose( f );
        }
    }

    // ── Flow 메시지 기록 ────────────────────────────
    void LogVoip( const std::string &strCallId, const char *from, const char *to, const char *proto, const char *label,
                  const char *body ) {
        if ( m_strCallsDir.empty() ) return;
        std::string dir = GetVoipDir( strCallId, "unknown" );
        if ( dir.empty() ) return;
        _writeJsonlSafe( dir, from, to, proto, label, body );
    }

    void _writeJsonlSafe( const std::string &dir, const char *from, const char *to, const char *proto,
                          const char *label, const char *body ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        _writeJsonl( dir, from, to, proto, label, body );
    }

    void LogPtt( const std::string &strGroupId, const char *from, const char *to, const char *proto, const char *label,
                 const char *body ) {
        if ( m_strCallsDir.empty() ) return;
        std::string base = GetPttSessionDir( strGroupId );
        if ( base.empty() ) return;
        std::lock_guard<std::mutex> lock( m_mtx );
        _writeJsonl( _pttHourDir( base ), from, to, proto, label, body );
    }

    /** JSON string escaper (public for external callers) */
    static std::string JsonEsc( const std::string &s ) {
        return Esc( s );
    }

private:
    std::string m_strCallsDir;
    std::string m_strComponent;
    std::mutex m_mtx;
    std::map<std::string, std::string> m_mapDir;           // key(sessionId or callId) → dir path
    std::map<std::string, std::string> m_mapCallSession;   // callId → sessionId
    std::map<std::string, std::string> m_mapPttSession;    // groupId → active session dir
    std::map<std::string, std::string> m_mapPttSessionId;  // groupId → session start time string

    std::string _dir( const std::string &key ) {
        // 1) 직접 조회
        auto it = m_mapDir.find( key );
        if ( it != m_mapDir.end() ) return it->second;
        // 2) Call-ID → Session-ID → dir 간접 조회 (B2BUA 착신 leg 대응)
        auto itSess = m_mapCallSession.find( key );
        if ( itSess != m_mapCallSession.end() ) {
            auto itDir = m_mapDir.find( itSess->second );
            if ( itDir != m_mapDir.end() ) {
                m_mapDir[key] = itDir->second;  // 캐시
                return itDir->second;
            }
        }
        return "";
    }

    /** session_start → 디렉터리 키 변환: "2026-04-11T09:00:00" → "20260411_090000", 빈값 → "permanent" */
    static std::string _sessionKey( const std::string &sessionStart ) {
        if ( sessionStart.empty() ) return "permanent";
        std::string k;
        for ( char c : sessionStart ) {
            if ( c >= '0' && c <= '9' ) k += c;
        }
        // "20260411090000" → "20260411_090000"
        if ( k.size() >= 14 ) return k.substr( 0, 8 ) + "_" + k.substr( 8, 6 );
        if ( k.size() >= 8 ) return k.substr( 0, 8 ) + "_000000";
        return k.empty() ? "permanent" : k;
    }

    /** 세션 종료 (lock 이미 획득된 상태에서 호출).
     *
     *  session.json state="ended" + end_time 업데이트만 수행.
     *  m_mapPttSession / m_mapPttSessionId 의 entry 는 의도적으로 유지하여
     *  바로 뒤따르는 멤버 leave/join 의 PttLogEvent 가 같은 디렉토리에
     *  events.jsonl 을 누적할 수 있게 한다. 새 _sessionStart 로 호 재시작
     *  시에는 GetPttSessionDir 의 sessKey 비교(line 234)에서 _endSessionLocked
     *  가 다시 호출된 직후 line 253 에서 entry 가 overwrite 되므로 누수 없음.
     */
    void _endSessionLocked( const std::string &strGroupId ) {
        auto it = m_mapPttSession.find( strGroupId );
        if ( it == m_mapPttSession.end() ) return;
        std::string dir = it->second;
        std::string path = dir + "/group.json";
        std::string c = _readFile( path );
        if ( !c.empty() ) {
            char ts[32];
            IsoNow( ts, sizeof( ts ) );
            _replace( c, "\"state\":\"active\"", "\"state\":\"ended\"" );
            size_t lb = c.rfind( '}' );
            if ( lb != std::string::npos ) c.insert( lb, std::string( ",\"end_time\":\"" ) + ts + "\"" );
            FILE *f = fopen( path.c_str(), "w" );
            if ( f ) {
                fputs( c.c_str(), f );
                fclose( f );
            }
        }
        // NOTE: m_mapPttSession / m_mapPttSessionId 는 erase 하지 않음.
        // GetPttSessionDir 가 새 sessKey 진입 시 자동 overwrite.
    }

    void _writeJsonl( const std::string &dir, const char *from, const char *to, const char *proto, const char *label,
                      const char *body ) {
        char ts[32];
        Timestamp( ts, sizeof( ts ) );
        std::string line = std::string( "{\"ts\":\"" ) + ts +
                           "\","
                           "\"from\":\"" +
                           ( from ? from : "" ) +
                           "\","
                           "\"to\":\"" +
                           ( to ? to : "" ) +
                           "\","
                           "\"proto\":\"" +
                           ( proto ? proto : "" ) +
                           "\","
                           "\"label\":\"" +
                           Esc( label ? label : "" ) +
                           "\","
                           "\"body\":\"" +
                           Esc( body ? body : "" ) + "\"}";
        FILE *f = fopen( ( dir + "/" + m_strComponent + ".jsonl" ).c_str(), "a" );
        if ( f ) {
            fprintf( f, "%s\n", line.c_str() );
            fclose( f );
        }
    }

    void _updateCallJson( const std::string &dir, const std::string &reason, int dur ) {
        std::string path = dir + "/call.json";
        std::string c = _readFile( path );
        if ( c.empty() ) return;
        char ts[32];
        IsoNow( ts, sizeof( ts ) );
        _replace( c, "\"state\":\"ringing\"", "\"state\":\"ended\"" );
        _replace( c, "\"state\":\"active\"", "\"state\":\"ended\"" );
        _replace( c, "\"end_time\":null", std::string( "\"end_time\":\"" ) + ts + "\"" );
        _replace( c, "\"duration\":0", "\"duration\":" + std::to_string( dur ) );
        _replace( c, "\"end_reason\":null", std::string( "\"end_reason\":\"" ) + reason + "\"" );
        FILE *f = fopen( path.c_str(), "w" );
        if ( f ) {
            fputs( c.c_str(), f );
            fclose( f );
        }
    }

    void _appendIndex( const std::string &dir, const std::string &id, const std::string &type ) {
        std::string hhDir = dir;
        int levels = ( type == "volte" ) ? 3 : 2;
        for ( int i = 0; i < levels; ++i ) {
            size_t pos = hhDir.rfind( '/' );
            if ( pos != std::string::npos ) hhDir = hhDir.substr( 0, pos );
        }
        char ts[32];
        IsoNow( ts, sizeof( ts ) );
        FILE *f = fopen( ( hhDir + "/index.json" ).c_str(), "a" );
        if ( f ) {
            std::string dn = dir.substr( dir.rfind( '/' ) + 1 );
            fprintf( f, "{\"dir\":\"%s\",\"type\":\"%s\",\"id\":\"%s\",\"time\":\"%s\"}\n", Esc( dn ).c_str(),
                     type.c_str(), Esc( id ).c_str(), ts );
            fclose( f );
        }
    }

    static std::string _readFile( const std::string &p ) {
        std::ifstream f( p );
        if ( !f ) return "";
        return std::string( ( std::istreambuf_iterator<char>( f ) ), std::istreambuf_iterator<char>() );
    }
    static void _replace( std::string &s, const std::string &a, const std::string &b ) {
        auto p = s.find( a );
        if ( p != std::string::npos ) s.replace( p, a.size(), b );
    }
    static void DateHour( std::string &y, std::string &m, std::string &d, std::string &h ) {
        time_t now = time( nullptr );
        struct tm t;
        localtime_r( &now, &t );
        char b[8];
        snprintf( b, 8, "%04d", t.tm_year + 1900 );
        y = b;
        snprintf( b, 8, "%02d", t.tm_mon + 1 );
        m = b;
        snprintf( b, 8, "%02d", t.tm_mday );
        d = b;
        snprintf( b, 8, "%02d", t.tm_hour );
        h = b;
    }
    static void IsoNow( char *b, int l ) {
        time_t n = time( nullptr );
        struct tm t;
        localtime_r( &n, &t );
        snprintf( b, l, "%04d-%02d-%02dT%02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
                  t.tm_sec );
    }
    static void Timestamp( char *b, int l ) {
        struct timespec ts;
        clock_gettime( CLOCK_REALTIME, &ts );
        struct tm t;
        localtime_r( &ts.tv_sec, &t );
        snprintf( b, l, "%02d:%02d:%02d.%06ld", t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000 );
    }
    static std::string Prefix( const std::string &s ) {
        return s.size() <= 2 ? s : s.substr( 0, s.size() - 2 );
    }
    static std::string San( const std::string &s, int mx ) {
        std::string r;
        r.reserve( s.size() );
        for ( char c : s ) {
            r += ( c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
                   c == '|' || c == ' ' )
                     ? '_'
                     : c;
        }
        return (int)r.size() > mx ? r.substr( 0, mx ) : r;
    }
    static std::string Esc( const std::string &s ) {
        std::string r;
        r.reserve( s.size() + 16 );
        for ( unsigned char c : s ) {
            switch ( c ) {
                case '"':
                    r += "\\\"";
                    break;
                case '\\':
                    r += "\\\\";
                    break;
                case '\n':
                    r += "\\n";
                    break;
                case '\r':
                    r += "\\r";
                    break;
                case '\t':
                    r += "\\t";
                    break;
                default:
                    if ( c < 0x20 ) {
                        char h[8];
                        snprintf( h, 8, "\\u%04x", c );
                        r += h;
                    } else
                        r += (char)c;
            }
        }
        return r;
    }
    static bool MkdirP( const std::string &p ) {
        struct stat st;
        if ( stat( p.c_str(), &st ) == 0 ) return true;
        size_t pos = p.rfind( '/' );
        if ( pos != std::string::npos && pos > 0 ) MkdirP( p.substr( 0, pos ) );
        return mkdir( p.c_str(), 0755 ) == 0 || errno == EEXIST;
    }

    // ── 가입자 실시간 상태 파일 ──────────────────────────
    //   경로: {CallsDir}/state/{volte|ptt}/{sanitized_subscriber_id}.json
    //   원자 쓰기: .tmp 로 쓰고 rename (POSIX atomic)

    std::string _sessionIdByCallId( const std::string &strCallId ) {
        auto it = m_mapCallSession.find( strCallId );
        return ( it != m_mapCallSession.end() ) ? it->second : "";
    }

    std::string _stateFilePath( const std::string &kind, const std::string &subId ) {
        return m_strCallsDir + "/state/" + kind + "/" + San( subId, 64 ) + ".json";
    }

    void _atomicWrite( const std::string &path, const std::string &content ) {
        std::string tmp = path + ".tmp";
        FILE *f = fopen( tmp.c_str(), "w" );
        if ( !f ) return;
        fwrite( content.data(), 1, content.size(), f );
        fclose( f );
        rename( tmp.c_str(), path.c_str() );
    }

    void _writeVoipState( const std::string &subId, const std::string &callId, const std::string &sessionId,
                          const std::string &peerId, const std::string &role, const std::string &state, const char *ts,
                          const std::string &recordDir, bool bVideo ) {
        if ( m_strCallsDir.empty() || subId.empty() ) return;
        std::string body = std::string( "{\"kind\":\"volte\"" ) + ",\"subscriber_id\":\"" + Esc( subId ) + "\"" +
                           ",\"session_id\":\"" + Esc( sessionId ) + "\"" + ",\"call_id\":\"" + Esc( callId ) + "\"" +
                           ",\"peer_id\":\"" + Esc( peerId ) + "\"" + ",\"role\":\"" + Esc( role ) + "\"" +
                           ",\"state\":\"" + Esc( state ) + "\"" + ",\"video\":" + ( bVideo ? "true" : "false" ) +
                           ",\"started_at\":\"" + ts + "\"" + ",\"answered_at\":null" + ",\"record_dir\":\"" +
                           Esc( recordDir ) + "\"}\n";
        _atomicWrite( _stateFilePath( "volte", subId ), body );
    }

    void _writePttState( const std::string &subId, const std::string &groupId, const std::string &sessionId,
                         const std::string &callId, const std::string &role, const char *ts,
                         const std::string &recordDir ) {
        if ( m_strCallsDir.empty() || subId.empty() ) return;
        std::string body = std::string( "{\"kind\":\"ptt\"" ) + ",\"subscriber_id\":\"" + Esc( subId ) + "\"" +
                           ",\"session_id\":\"" + Esc( sessionId ) + "\"" + ",\"call_id\":\"" + Esc( callId ) + "\"" +
                           ",\"group_id\":\"" + Esc( groupId ) + "\"" + ",\"role\":\"" + Esc( role ) + "\"" +
                           ",\"state\":\"active\"" + ",\"started_at\":\"" + ts + "\"" + ",\"record_dir\":\"" +
                           Esc( recordDir ) + "\"}\n";
        _atomicWrite( _stateFilePath( "ptt", subId ), body );
    }

    void _removePttState( const std::string &subId ) {
        if ( m_strCallsDir.empty() || subId.empty() ) return;
        ::unlink( _stateFilePath( "ptt", subId ).c_str() );
    }

    /** 특정 call_id 에 매칭되는 volte state 파일 삭제 (caller + callee 동시 정리).
     *  B2BUA 두 leg 의 state 파일은 동일 session_id 로 저장되므로 session_id 로 매칭한다 —
     *  어느 leg 의 call_id 로 호출되어도 양쪽 파일이 모두 정리된다. */
    void _removeVoipStatesByCallId( const std::string &strCallId ) {
        if ( m_strCallsDir.empty() || strCallId.empty() ) return;
        std::string sessId = _sessionIdByCallId( strCallId );
        std::string needle =
            sessId.empty() ? "\"call_id\":\"" + Esc( strCallId ) + "\"" : "\"session_id\":\"" + Esc( sessId ) + "\"";
        std::string dir = m_strCallsDir + "/state/volte";
        DIR *d = opendir( dir.c_str() );
        if ( !d ) return;
        struct dirent *ent;
        while ( ( ent = readdir( d ) ) != nullptr ) {
            if ( ent->d_name[0] == '.' ) continue;
            std::string path = dir + "/" + ent->d_name;
            std::string c = _readFile( path );
            if ( c.find( needle ) != std::string::npos ) {
                ::unlink( path.c_str() );
            }
        }
        closedir( d );
    }

    /** VoIP Answer 시 state 파일 state=ringing → active + answered_at 업데이트.
     *  session_id 매칭 (leg A/B 어느 call_id 로 호출되든 양쪽 state 파일 갱신). */
    void _promoteVoipStates( const std::string &strCallId, const char *ts ) {
        if ( m_strCallsDir.empty() || strCallId.empty() ) return;
        std::string sessId = _sessionIdByCallId( strCallId );
        std::string needle =
            sessId.empty() ? "\"call_id\":\"" + Esc( strCallId ) + "\"" : "\"session_id\":\"" + Esc( sessId ) + "\"";
        std::string dir = m_strCallsDir + "/state/volte";
        DIR *d = opendir( dir.c_str() );
        if ( !d ) return;
        struct dirent *ent;
        while ( ( ent = readdir( d ) ) != nullptr ) {
            if ( ent->d_name[0] == '.' ) continue;
            std::string path = dir + "/" + ent->d_name;
            std::string c = _readFile( path );
            if ( c.find( needle ) == std::string::npos ) continue;
            _replace( c, "\"state\":\"ringing\"", "\"state\":\"active\"" );
            _replace( c, "\"answered_at\":null", std::string( "\"answered_at\":\"" ) + ts + "\"" );
            _atomicWrite( path, c );
        }
        closedir( d );
    }

    /** 그룹 세션 종료 시 해당 그룹의 잔여 state 파일 일괄 제거 */
    void _removePttStatesByGroupId( const std::string &strGroupId ) {
        if ( m_strCallsDir.empty() || strGroupId.empty() ) return;
        std::string dir = m_strCallsDir + "/state/ptt";
        DIR *d = opendir( dir.c_str() );
        if ( !d ) return;
        std::string needle = "\"group_id\":\"" + Esc( strGroupId ) + "\"";
        struct dirent *ent;
        while ( ( ent = readdir( d ) ) != nullptr ) {
            if ( ent->d_name[0] == '.' ) continue;
            std::string path = dir + "/" + ent->d_name;
            std::string c = _readFile( path );
            if ( c.find( needle ) != std::string::npos ) {
                ::unlink( path.c_str() );
            }
        }
        closedir( d );
    }

    /** state 디렉토리 내 모든 *.json 제거 (기동 시 호출) */
    void _purgeStateDir( const std::string &dir ) {
        DIR *d = opendir( dir.c_str() );
        if ( !d ) return;
        struct dirent *ent;
        while ( ( ent = readdir( d ) ) != nullptr ) {
            if ( ent->d_name[0] == '.' ) continue;
            std::string path = dir + "/" + ent->d_name;
            ::unlink( path.c_str() );
        }
        closedir( d );
    }
};

extern CCallDir gclsCallDir;

#endif
