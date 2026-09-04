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
#include <set>
#include <string>

#include "FmReporter.h"
#include "Log.h"
#include "StoreOpWriter.h"

/**
 * 호 이력(CDR)/실시간 상태 파일 기록 — 저장 경로 무의존(non-blocking) 계약.
 *
 * ServiceLogDir(NAS 가능) 아래 call.json/session.json/events.jsonl/state 파일을 기록한다.
 * 원자 rewrite·rename·디렉터리 스캔이 섞여 있어 append 스풀(ServiceLogWriter) 재생이
 * 불가능하므로, StoreOpWriter 로 격리한다:
 *   - 생산자(SIP 스레드)는 m_mtx 아래 맵 북키핑 + 경로/내용 문자열 조립만 하고 op 를
 *     적재한다 — 파일시스템 무접촉. 디렉터리 생성도 worker 가 기록 직전에 수행한다.
 *   - worker 스레드 하나가 op 를 FIFO 실행 — 같은 파일의 RMW(예: call.json
 *     ringing→active→ended) 순서가 보존된다.
 *   - 저장 경로 실패/정체(StallSec)/큐 포화 시 op 드롭(장애 구간 이력 유실 수용) +
 *     A-PRC-013 storage_failure 자기보고, 회복 시 자연 해소. 상세: flow_logging.md §2.
 */
class CCallDir {
public:
    static const int kProbeIntervalSec = 60;  // 유휴 write probe 간격

    void Init( const std::string &strBaseDir, const std::string &strComponent, int iStallSec = 5 ) {
        if ( strBaseDir.empty() ) return;
        m_strCallsDir = strBaseDir;
        m_strComponent = strComponent;

        std::string strPath = m_strCallsDir;
        std::string strProbePath = m_strCallsDir + "/state/.probe";
        m_worker.Init(
            iStallSec, 20000, 64LL * 1024 * 1024,
            []( EnumSowLogLevel eLevel, const std::string &strMsg ) {
                CLog::Print( eLevel == SOW_LOG_ERROR ? LOG_ERROR : LOG_SYSTEM, "call_dir %s", strMsg.c_str() );
            },
            [strPath]( const SowDegradeInfo &clsInfo ) {
                // A-PRC-013 storage_failure — 폴백 진입 시 open, 회복 시 close
                if ( !gclsFmReporter.IsEnabled() ) return;
                const std::string strMo = gclsFmReporter.Node() + "/csp/call_dir";
                if ( clsInfo.bDegraded ) {
                    SimpleJson::JsonNode nodeParams;
                    nodeParams.Set( "path", strPath.c_str() );
                    nodeParams.Set( "reason", clsInfo.strReason.c_str() );
                    nodeParams.Set( "dropped", (int)clsInfo.ulDroppedOps );
                    gclsFmReporter.AlarmOpen( "A-PRC-013", strMo, nodeParams );
                } else {
                    gclsFmReporter.AlarmClose( "A-PRC-013", strMo );
                }
            },
            // 유휴 write probe (60s) — 무호 구간의 마운트 소실/권한 상실도 실쓰기로 선제
            //   감지한다 (경로 문자열만으로는 IsEnabled 가 true 라 소실을 못 본다).
            kProbeIntervalSec,
            [strProbePath]() {
                if ( !_writeFileS( strProbePath, "probe\n" ) ) return false;
                ::unlink( strProbePath.c_str() );
                return true;
            } );

        // 기동 작업: base/state 디렉터리 보장 + stale 상태 파일 정리 (worker — 저장 경로 최초 접촉)
        std::string base = m_strCallsDir;
        m_worker.Enqueue( [base]() {
            bool bOk = MkdirP( base );
            bOk = MkdirP( base + "/state/volte" ) && bOk;
            bOk = MkdirP( base + "/state/ptt" ) && bOk;
            _purgeStateDirS( base + "/state/volte" );
            _purgeStateDirS( base + "/state/ptt" );
            return bOk;
        } );
    }

    bool IsEnabled() const {
        return !m_strCallsDir.empty();
    }

    /** 기동 시 stale 가입자 상태 파일 일괄 제거.
     *  CSP 재시작 후에는 SIP 다이얼로그가 모두 소실되므로 이전 state 는 무조건 stale. */
    void CleanupStaleStates() {
        if ( m_strCallsDir.empty() ) return;
        std::string base = m_strCallsDir;
        m_worker.Enqueue( [base]() {
            bool bOk = _purgeStateDirS( base + "/state/volte" );
            return _purgeStateDirS( base + "/state/ptt" ) && bOk;
        } );
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
        std::string dir;
        {
            std::lock_guard<std::mutex> lock( m_mtx );
            dir = _dir( strSessionId );
            if ( dir.empty() ) dir = _dir( strCallIdA );  // Try via call-id
        }
        if ( dir.empty() ) return;
        std::string content;
        if ( strSesId.empty() ) {
            content = "{\"session_id\":\"" + Esc( strSessionId ) + "\",\"call_ids\":[\"" + Esc( strCallIdA ) + "\",\"" +
                      Esc( strCallIdB ) + "\"]}\n";
        } else {
            content = "{\"session_id\":\"" + Esc( strSessionId ) + "\",\"sesid\":\"" + Esc( strSesId ) +
                      "\",\"call_ids\":[\"" + Esc( strCallIdA ) + "\",\"" + Esc( strCallIdB ) + "\"]}\n";
        }
        std::string path = dir + "/session.json";
        m_worker.Enqueue( [path, content]() { return _writeFileS( path, content ); }, content.size() );
    }

    // ── VoIP ─────────────────────────────────────────────

    /** VoIP 세션 디렉터리 경로 결정 (Session-ID 기반 — 순수 계산, 생성은 worker 가 기록 직전에). */
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

        // 새 디렉터리 경로 (Session-ID가 있으면 사용, 없으면 Call-ID)
        std::string key = strCallId;
        if ( itSess != m_mapCallSession.end() ) key = itSess->second;

        std::string yyyy, mm, dd, hh;
        DateHour( yyyy, mm, dd, hh );
        std::string sc = San( strCaller, 20 );
        std::string dir = m_strCallsDir + "/volte/" + yyyy + "/" + mm + "/" + dd + "/" + hh + "/" + Prefix( sc ) + "/" +
                          sc + "/" + San( key, 80 ) + ".d";
        m_mapDir[key] = dir;
        // Call-ID로도 같은 디렉터리를 찾을 수 있게
        if ( key != strCallId ) m_mapDir[strCallId] = dir;
        return dir;
    }

    void VoipCallStart( const std::string &strCallId, const std::string &strCaller, const std::string &strCallee,
                        bool bVideo = false, const std::string &strMediaNode = "" ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        std::string dir = _dir( strCallId );
        if ( dir.empty() ) return;
        // 세션당 1회만 기록 (re-INVITE 멱등) — 메모리 셋으로 판정 (구 stat() 프로브 대체)
        if ( m_setCallJson.size() > 50000 ) m_setCallJson.clear();
        if ( !m_setCallJson.insert( dir ).second ) return;
        char ts[32];
        IsoNow( ts, sizeof( ts ) );
        const char *pszCallType = bVideo ? "volte_video" : "volte";
        std::string content = "{\"call_id\":\"" + Esc( strCallId ) + "\",\"call_type\":\"" + pszCallType +
                              "\",\"initiator\":\"" + Esc( strCaller ) + "\",\"callee\":\"" + Esc( strCallee ) +
                              "\",\"state\":\"ringing\",\"invite_time\":\"" + ts +
                              "\",\"answer_time\":null,\"end_time\":null,\"duration\":0,\"end_reason\":null,\"end_status\":0}\n";
        std::string path = dir + "/call.json";
        m_worker.Enqueue( [path, content]() { return _writeFileS( path, content ); }, content.size() );

        // 가입자별 실시간 상태 파일 — 발/착신 각각
        std::string sessId = _sessionIdByCallId( strCallId );
        _writeVoipState( strCaller, strCallId, sessId, strCallee, "caller", "ringing", ts, dir, bVideo, strMediaNode );
        _writeVoipState( strCallee, strCallId, sessId, strCaller, "callee", "ringing", ts, dir, bVideo, strMediaNode );
    }

    /** 응답코드 → 종료 사유 어휘 (sip_statistics.md §2.3). 정확한 코드는 `end_status` 가 갖는다. */
    static const char *_ReasonOfStatus( int iStatus ) {
        if ( iStatus == 486 || iStatus == 600 ) return "busy";
        if ( iStatus == 408 || iStatus == 480 ) return "no_answer";
        if ( iStatus == 488 || iStatus == 606 ) return "error";   // 코덱·SDP 협상 실패
        if ( iStatus >= 500 && iStatus < 600 ) return "error";
        if ( iStatus >= 400 ) return "rejected";                  // 403·404·603 …
        return "error";
    }

    /** CSP 가 **응답 전에 자체 거절한 호**를 시도로 남긴다 (F-54).
     *
     *  `VoipCallStart` 는 착신 leg 으로 INVITE 를 넘기기로 결정한 뒤에 불린다. 그 전에 거절한
     *  호는 아무 기록도 남지 않아 성공률·NER 의 분모에서 조용히 빠졌다 — 없는 번호로 거는 호가
     *  많은 구간에서 성공률이 실제보다 좋게 나온다.
     *
     *  응답 전에 끝났으므로 `answer_time` 은 null, `duration` 0 이고 `state` 는 처음부터
     *  `ended` 다. 이미 기록된 세션(정상 경로가 만든 호)은 건드리지 않는다 — 재INVITE·인증
     *  재시도로 같은 시도를 두 번 세지 않게.
     */
    void VoipCallRejected( const std::string &strCallId, const std::string &strCaller,
                           const std::string &strCallee, int iStatus ) {
        // GetVoipDir 이 m_mtx 를 스스로 잡는다 — 아래 lock 안에서 부르면 교착이다.
        std::string dir = GetVoipDir( strCallId, strCaller, strCallee );
        if ( dir.empty() ) return;
        std::lock_guard<std::mutex> lock( m_mtx );
        if ( m_setCallJson.count( dir ) > 0 ) return;   // 정상 경로가 이미 기록한 세션
        char ts[32];
        IsoNow( ts, sizeof( ts ) );
        std::string tsStr = ts;
        std::string content = "{\"call_id\":\"" + Esc( strCallId ) +
                              "\",\"call_type\":\"volte\",\"initiator\":\"" + Esc( strCaller ) +
                              "\",\"callee\":\"" + Esc( strCallee ) +
                              "\",\"state\":\"ended\",\"invite_time\":\"" + tsStr +
                              "\",\"answer_time\":null,\"end_time\":\"" + tsStr +
                              "\",\"duration\":0,\"end_reason\":\"" + _ReasonOfStatus( iStatus ) +
                              "\",\"end_status\":" + std::to_string( iStatus ) + "}\n";
        std::string path = dir + "/call.json";
        std::string callId = strCallId;
        m_worker.Enqueue(
            [path, content, dir, callId, tsStr]() {
                bool bOk = _writeFileS( path, content );
                return _appendIndexS( dir, callId, "volte", tsStr ) && bOk;
            },
            content.size() );
        m_mapDir.erase( strCallId );
    }

    void VoipCallAnswer( const std::string &strCallId ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        std::string dir = _dir( strCallId );
        if ( dir.empty() ) return;
        char ts[32];
        IsoNow( ts, sizeof( ts ) );
        std::string path = dir + "/call.json";
        std::string tsStr = ts;
        m_worker.Enqueue( [path, tsStr]() { return _updateAnswerS( path, tsStr ); } );

        // state/volte/ 하위 해당 call_id 매칭 파일 active 로 승격
        _promoteVoipStates( strCallId, ts );
    }

    void VoipCallEnd( const std::string &strCallId, const std::string &strReason = "normal", int iDur = 0,
                      int iStatus = 0 ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        std::string dir = _dir( strCallId );
        if ( dir.empty() ) return;
        // call.json 을 기록한 세션의 첫 종료만 마감+인덱스를 남긴다 — B2BUA 양 leg 의
        //   VoipCallEnd 가 각각 인덱스를 append 해 같은 호가 2줄로 남던 중복 제거.
        if ( m_setCallJson.erase( dir ) > 0 ) {
            char ts[32];
            IsoNow( ts, sizeof( ts ) );
            std::string tsStr = ts;
            std::string reason = strReason;
            std::string callId = strCallId;
            m_worker.Enqueue( [dir, reason, iDur, iStatus, tsStr, callId]() {
                bool bOk = _updateCallJsonS( dir, reason, iDur, iStatus, tsStr );
                return _appendIndexS( dir, callId, "volte", tsStr ) && bOk;
            } );
        }
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
        std::string line = "{\"msisdn\":\"" + Esc( strMsisdn ) + "\",\"role\":\"" + strRole + "\",\"join_time\":\"" +
                           ts + "\",\"leave_time\":null}\n";
        std::string path = dir + "/participants.jsonl";
        m_worker.Enqueue( [path, line]() { return _appendLineS( path, line ); }, line.size() );
    }

    // ── PTT ──────────────────────────────────────────────

    /** PTT 그룹 base 디렉터리 — ptt/{group_id}. 반환값이 CMP record_dir 로 전달된다.
     *
     *  기록 단위는 **세션** 이다 — 세션 산출물(녹취 세그먼트·floor·events·session.json)은
     *  base 하위 {YYYY}/{MM}/{DD}/{HH}/{sesdir}/ 에 모인다. sesdir = 세션 sesid 에서
     *  유도한 S{yyyymmddHHMMSSuuuuuu}_{n} (GetPttSessionName).
     *
     *  시간버킷을 세션 위에 둔 이유: chat 그룹 세션은 상시 유지라 종료가 없어 세션만으로는
     *  디렉터리 엔트리수·시간범위 스캔의 상한이 사라진다. 대신 세션이 시간을 넘기면 다음
     *  버킷에 **같은 이름** 의 sesdir 이 생기고, 이력 API 가 이름으로 이어붙여 한 세션으로
     *  제시한다 (sesdir 이름에 시작 시각이 박혀 있어 시작 버킷을 바로 찾는다).
     *
     *  strSesId 가 직전과 같으면 같은 sesdir 을 이어 쓴다 — 멤버 추가 INVITE 마다 호출되는
     *  멱등 경로다. 새 sesid = 새 세션 = 새 sesdir (같은 시간대의 두 번째 통화가 앞 통화에
     *  섞이지 않는 근거). */
    std::string GetPttSessionDir( const std::string &strGroupId, const std::string &strSesId = "",
                                  const std::string &strStorageId = "" ) {
        std::lock_guard<std::mutex> lock( m_mtx );

        // 저장 경로 키: ptt_groups.id(surrogate) 우선 — mcptt_group_id 변경에도 불변.
        //   storageId 없거나 "0" 이면 groupId(mcptt) fallback.
        std::string key = ( !strStorageId.empty() && strStorageId != "0" ) ? strStorageId : strGroupId;
        std::string sg = San( key, 32 );
        std::string base = m_strCallsDir + "/ptt/" + sg;

        // 런타임 맵은 mcptt_group_id 로 키잉(다른 PTT 메서드가 group._id 로 조회).
        m_mapPttSession[strGroupId] = base;
        if ( !strSesId.empty() && m_mapPttSessionId[strGroupId] != strSesId ) {
            m_mapPttSessionId[strGroupId] = strSesId;
            m_mapPttSesName[strGroupId] = _sesDirName( strSesId );
            // 새 세션 = session.json 미기록 상태 — PttSessionStart 가 1회만 쓰도록 신호.
            m_mapPttSessionDesc.erase( strGroupId );
        } else if ( m_mapPttSesName[strGroupId].empty() ) {
            // sesid 없이 진입한 로그 전용 경로 — 기록을 잃지 않도록 이름을 만들어 둔다.
            m_mapPttSesName[strGroupId] = _sesDirName( m_mapPttSessionId[strGroupId] );
        }
        return base;
    }

    /** 현재 세션의 디렉터리 이름 (CMP 에 session_dir 로 전달) — 세션 미개시면 빈 문자열. */
    std::string GetPttSessionName( const std::string &strGroupId ) {
        std::lock_guard<std::mutex> lock( m_mtx );
        auto it = m_mapPttSesName.find( strGroupId );
        return it == m_mapPttSesName.end() ? std::string() : it->second;
    }

    /** 그룹 base 하위 현재 시각 시간버킷 ptt/{gid}/{YYYY}/{MM}/{DD}/{HH} — 순수 계산 */
    std::string _pttHourDir( const std::string &base ) {
        std::string yyyy, mm, dd, hh;
        DateHour( yyyy, mm, dd, hh );
        return base + "/" + yyyy + "/" + mm + "/" + dd + "/" + hh;
    }

    /** 현재 시각 버킷의 세션 디렉터리 경로 (순수 계산 — 생성은 worker 가 기록 직전에).
     *  m_mtx 획득 상태에서 호출한다. 세션이 시간을 넘기면 다음 버킷에 같은 이름으로 이어진다. */
    std::string _pttSessionDirLocked( const std::string &strGroupId, const std::string &base ) {
        std::string hourDir = _pttHourDir( base );
        auto it = m_mapPttSesName.find( strGroupId );
        if ( it == m_mapPttSesName.end() || it->second.empty() ) return hourDir;  // 이름 미상 — 버킷 직행
        return hourDir + "/" + it->second;
    }

    /** strGroupJson = CGroupCallService::BuildGroupDescriptor 가 만든 자기완결 디스크립터
     *  (계획서 §5). group.json 은 디스크립터 그대로 + state/updated_at 만 주입해 기록한다
     *  (옛 placeholder session_id/call_id/initiator 제거). strCallId/strInitiator 는
     *  가입자 state 파일 기록용으로만 사용. */
    void PttSessionStart( const std::string &strGroupId, const std::string &strCallId, const std::string &strInitiator,
                          const std::string &strGroupJson = "{}" ) {
        // GetPttSessionDir가 이미 호출된 상태여야 함
        {
            std::lock_guard<std::mutex> lock( m_mtx );
            auto it = m_mapPttSession.find( strGroupId );
            if ( it == m_mapPttSession.end() ) return;
            std::string dir = it->second;  // 그룹 base

            std::string sessId = m_mapPttSessionId[strGroupId];
            char ts[32];
            IsoNow( ts, sizeof( ts ) );

            // 디스크립터 객체에 state/updated_at 주입 (말미 '}' 직전에 삽입).
            std::string desc = strGroupJson;
            if ( desc.empty() || desc.back() != '}' ) desc = "{}";
            desc.pop_back();
            if ( !desc.empty() && desc.back() != '{' ) desc += ",";
            std::string body = "\"state\":\"active\",\"updated_at\":\"" + std::string( ts ) + "\"";

            // group.json (자기완결 그룹 디스크립터) — base 루트에 1개 = 최신 편성 스냅샷.
            //   매 세션 시작마다 재작성한다 — 최초 1회만 기록하면 이후 추가된 필드
            //   (floor_control/floor_policy/max_talkers)·편성 변경이 영영 반영되지 않는다.
            std::string groupPath = dir + "/group.json";
            std::string groupContent = desc + body + "}\n";
            m_worker.Enqueue( [groupPath, groupContent]() { return _writeFileS( groupPath, groupContent ); },
                              groupContent.size() );

            // session.json — 세션 디렉터리(시작 버킷)에 당시 디스크립터 + 세션 사실을 남긴다.
            //   반이중/전이중·동시 발언 정원 표시는 이것이 정본 — 루트 group.json 은 최신
            //   스냅샷이라 과거 세션에 소급되면 이력이 왜곡된다 (private 은 floor_control 이
            //   세션마다 SDP 협상으로 달라질 수 있다).
            //   sesid/initiator/call_id/start_time 은 통화형 이력(1:1·임시)의 행 자체를
            //   이루는 값이라 세션 쪽에만 넣는다 — 그룹 편성 스냅샷과 성격이 다르다.
            //   세션당 1회만 쓴다 — 두 번째 멤버의 INVITE 도 같은 세션의 ProcessGroupCall 을
            //   타므로, 매번 쓰면 개시자·시작시각이 마지막 참가자 값으로 덮인다.
            std::string sesDir = _pttSessionDirLocked( strGroupId, dir );
            if ( m_mapPttSessionDesc.find( strGroupId ) == m_mapPttSessionDesc.end() ) {
                std::string sessBody = body + ",\"sesid\":\"" + Esc( sessId ) + "\"" + ",\"initiator\":\"" +
                                       Esc( strInitiator ) + "\"" + ",\"call_id\":\"" + Esc( strCallId ) + "\"" +
                                       ",\"start_time\":\"" + std::string( ts ) + "\"";
                std::string sessPath = sesDir + "/session.json";
                std::string sessContent = desc + sessBody + "}\n";
                m_worker.Enqueue( [sessPath, sessContent]() { return _writeFileS( sessPath, sessContent ); },
                                  sessContent.size() );
                m_mapPttSessionDesc[strGroupId] = sessPath;
            }

            // 초기 가입자(발신자) 상태 파일 기록 (state/ptt/{sub}.json — 버킷과 독립)
            if ( !strInitiator.empty() && strInitiator != "autojoin" ) {
                _writePttState( strInitiator, strGroupId, sessId, strCallId, "initiator", ts, sesDir );
            }
        }

        // 개시자(발신 단말)도 입장 이력(events.jsonl)에 기록 — member_join 은 CSP 가 초대한
        // 멤버의 응답(OnCallStarted) 경로에서만 남아 개시자가 세션이력 참가자에서 누락됐다.
        // (PttLogEvent 가 m_mtx 를 잡으므로 반드시 lock 밖에서 호출)
        if ( !strInitiator.empty() && strInitiator != "autojoin" ) {
            PttLogEvent( strGroupId, "member_join",
                         "{\"member\":\"" + Esc( strInitiator ) + "\",\"role\":\"initiator\"}" );
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
        _writePttState( strMemberId, strGroupId, sessId, strCallId, "member", ts,
                        _pttSessionDirLocked( strGroupId, dir ) );
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
        line += "}\n";

        // 세션 디렉터리 events.jsonl 에 append (dir=그룹 base → {YYYY}/{MM}/{DD}/{HH}/{sesdir}/)
        std::string path = _pttSessionDirLocked( strGroupId, dir ) + "/events.jsonl";
        m_worker.Enqueue( [path, line]() { return _appendLineS( path, line ); }, line.size() );
    }

    // ── MCData 그룹 메시지 보관 ────────────────────────
    //   경로: {ServiceLogDir}/message/{gid}/{YYYY}/{MM}/{DD}/{HH}/messages.jsonl
    //   PTT 세션 여부와 무관 (그룹콜 없이 문자만 오가도 기록). open-append-close 는
    //   PttLogEvent 와 동일 방침 (사람 메시징 볼륨 전제).
    void McDataMessageLog( const std::string &strGroupId, const std::string &strJsonData ) {
        if ( m_strCallsDir.empty() ) return;
        char ts[32];
        IsoNow( ts, sizeof( ts ) );
        std::string line = "{\"ts\":\"" + std::string( ts ) + "\"";
        if ( !strJsonData.empty() && strJsonData.front() == '{' && strJsonData.back() == '}' ) {
            std::string inner = strJsonData.substr( 1, strJsonData.size() - 2 );
            if ( !inner.empty() ) line += "," + inner;
        }
        line += "}\n";

        time_t now = time( NULL );
        struct tm t;
        localtime_r( &now, &t );
        char sub[64];
        snprintf( sub, sizeof( sub ), "/%04d/%02d/%02d/%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour );
        std::string path = m_strCallsDir + "/message/" + San( strGroupId, 64 ) + sub + "/messages.jsonl";
        m_worker.Enqueue( [path, line]() { return _appendLineS( path, line ); }, line.size() );
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
        std::lock_guard<std::mutex> lock( m_mtx );
        // 세션 개시 전(GetPttSessionDir 미호출)이면 기록할 세션이 없다 — 새로 만들지 않는다.
        auto it = m_mapPttSession.find( strGroupId );
        if ( it == m_mapPttSession.end() || it->second.empty() ) return;
        _writeJsonl( _pttSessionDirLocked( strGroupId, it->second ), from, to, proto, label, body );
    }

    /** JSON string escaper (public for external callers) */
    static std::string JsonEsc( const std::string &s ) {
        return Esc( s );
    }

private:
    std::string m_strCallsDir;
    std::string m_strComponent;
    std::mutex m_mtx;
    CStoreOpWriter m_worker;                                 // 저장 경로 연산 전담 (SIP 스레드 무접촉)
    std::map<std::string, std::string> m_mapDir;             // key(sessionId or callId) → dir path
    std::map<std::string, std::string> m_mapCallSession;     // callId → sessionId
    std::map<std::string, std::string> m_mapPttSession;      // groupId → 그룹 base 디렉터리
    std::map<std::string, std::string> m_mapPttSessionId;    // groupId → 현재 세션 sesid
    std::map<std::string, std::string> m_mapPttSesName;      // groupId → 세션 디렉터리 이름 S{ts}_{n}
    std::map<std::string, std::string> m_mapPttSessionDesc;  // groupId → 세션 시작 버킷 session.json 경로
    std::set<std::string> m_setCallJson;                     // call.json 기록 완료 dir (멱등 가드)

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

    /** sesid → 세션 디렉터리 이름. "{caller}::csp::{yyyymmddHHMMSSuuuuuu}::{n}" → "S{ts}_{n}".
     *
     *  이름에 시작 시각(µs)이 그대로 박히므로 (a) 시간순 정렬, (b) 이름만으로 시작 버킷
     *  직접 접근, (c) VoLTE 의 S{ts}.d 와 같은 모양이 한 번에 성립한다. 카운터는 같은 µs
     *  에 두 세션이 열릴 때의 충돌 방지용(sesid 발행 규칙과 동일).
     *  형식이 다르거나 비었으면 현재 시각으로 만든다 — 기록을 잃지 않는다. */
    static std::string _sesDirName( const std::string &sesId ) {
        std::string ts, ctr;
        size_t p2 = sesId.size() >= 2 ? sesId.rfind( "::" ) : std::string::npos;
        if ( p2 != std::string::npos && p2 >= 2 ) {
            size_t p1 = sesId.rfind( "::", p2 - 1 );
            if ( p1 != std::string::npos ) {
                ts = sesId.substr( p1 + 2, p2 - p1 - 2 );
                ctr = sesId.substr( p2 + 2 );
            }
        }
        if ( ts.size() < 14 || ts.find_first_not_of( "0123456789" ) != std::string::npos ) {
            struct timespec tsp;
            clock_gettime( CLOCK_REALTIME, &tsp );
            struct tm t;
            localtime_r( &tsp.tv_sec, &t );
            char b[32];
            snprintf( b, sizeof( b ), "%04d%02d%02d%02d%02d%02d%06ld", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                      t.tm_hour, t.tm_min, t.tm_sec, tsp.tv_nsec / 1000 );
            ts = b;
            ctr.clear();
        }
        if ( ctr.empty() || ctr.find_first_not_of( "0123456789" ) != std::string::npos ) ctr = "0";
        return "S" + ts + "_" + ctr;
    }

    /** 세션 종료 (lock 이미 획득된 상태에서 호출).
     *
     *  group.json/session.json state="ended" + end_time 업데이트만 수행.
     *  m_mapPttSession / m_mapPttSessionId 의 entry 는 의도적으로 유지하여
     *  바로 뒤따르는 멤버 leave/join 의 PttLogEvent 가 같은 디렉토리에
     *  events.jsonl 을 누적할 수 있게 한다. 새 세션 시작 시에는
     *  PttSessionStart 가 두 파일을 재작성하므로 잔존값 걱정이 없다.
     */
    void _endSessionLocked( const std::string &strGroupId ) {
        auto it = m_mapPttSession.find( strGroupId );
        if ( it == m_mapPttSession.end() ) return;
        char ts[32];
        IsoNow( ts, sizeof( ts ) );
        std::string tsStr = ts;
        std::string groupPath = it->second + "/group.json";
        m_worker.Enqueue( [groupPath, tsStr]() { return _finalizeDescriptorS( groupPath, tsStr ); } );
        auto itDesc = m_mapPttSessionDesc.find( strGroupId );
        if ( itDesc != m_mapPttSessionDesc.end() ) {
            std::string sessPath = itDesc->second;
            m_worker.Enqueue( [sessPath, tsStr]() { return _finalizeDescriptorS( sessPath, tsStr ); } );
            m_mapPttSessionDesc.erase( itDesc );
        }
    }

    // ── 저장 경로 연산 (worker 스레드 전용 — 정적, 캡처된 값만 사용) ──────────

    /** 파일 전체 재작성 (w) — 상위 디렉터리 보장 포함 */
    static bool _writeFileS( const std::string &path, const std::string &content ) {
        MkdirP( path.substr( 0, path.rfind( '/' ) ) );
        FILE *f = fopen( path.c_str(), "w" );
        if ( !f ) return false;
        bool bOk = fwrite( content.data(), 1, content.size(), f ) == content.size();
        fclose( f );
        return bOk;
    }

    /** 한 줄 append — 상위 디렉터리 보장 포함 */
    static bool _appendLineS( const std::string &path, const std::string &line ) {
        MkdirP( path.substr( 0, path.rfind( '/' ) ) );
        FILE *f = fopen( path.c_str(), "a" );
        if ( !f ) return false;
        bool bOk = fwrite( line.data(), 1, line.size(), f ) == line.size();
        fclose( f );
        return bOk;
    }

    /** call.json ringing→active + answer_time (RMW — worker FIFO 가 순서 보장) */
    static bool _updateAnswerS( const std::string &path, const std::string &ts ) {
        std::string c = _readFile( path );
        if ( c.empty() ) return false;
        _replace( c, "\"state\":\"ringing\"", "\"state\":\"active\"" );
        _replace( c, "\"answer_time\":null", std::string( "\"answer_time\":\"" ) + ts + "\"" );
        return _writeFileS( path, c );
    }

    /** 종료 마킹 — state/end_time/duration/end_reason/end_status 를 한 번에 채운다.
     *  치환 방식이라 생성 템플릿에 그 키가 있어야 한다(없으면 조용히 아무것도 안 바뀐다). */
    static bool _updateCallJsonS( const std::string &dir, const std::string &reason, int dur, int status,
                                  const std::string &ts ) {
        std::string path = dir + "/call.json";
        std::string c = _readFile( path );
        if ( c.empty() ) return false;
        _replace( c, "\"state\":\"ringing\"", "\"state\":\"ended\"" );
        _replace( c, "\"state\":\"active\"", "\"state\":\"ended\"" );
        _replace( c, "\"end_time\":null", std::string( "\"end_time\":\"" ) + ts + "\"" );
        _replace( c, "\"duration\":0", "\"duration\":" + std::to_string( dur ) );
        _replace( c, "\"end_reason\":null", std::string( "\"end_reason\":\"" ) + reason + "\"" );
        // 응답코드는 `error` 한 칸으로 뭉개진 실패를 코드별로 가르는 유일한 근거다
        //   (488 코덱 불일치 / 503 서비스 불가 / 5xx 서버 오류 …). BYE 정상 종료는 200.
        _replace( c, "\"end_status\":0", "\"end_status\":" + std::to_string( status ) );
        return _writeFileS( path, c );
    }

    static bool _appendIndexS( const std::string &dir, const std::string &id, const std::string &type,
                               const std::string &ts ) {
        std::string hhDir = dir;
        int levels = ( type == "volte" ) ? 3 : 2;
        for ( int i = 0; i < levels; ++i ) {
            size_t pos = hhDir.rfind( '/' );
            if ( pos != std::string::npos ) hhDir = hhDir.substr( 0, pos );
        }
        std::string dn = dir.substr( dir.rfind( '/' ) + 1 );
        std::string line = "{\"dir\":\"" + Esc( dn ) + "\",\"type\":\"" + type + "\",\"id\":\"" + Esc( id ) +
                           "\",\"time\":\"" + ts + "\"}\n";
        return _appendLineS( hhDir + "/index.json", line );
    }

    /** 디스크립터 종료 마킹 — state=ended + end_time. end_time 은 이미 있으면 값만
     *  교체한다 (종전엔 종료마다 말미에 삽입해 중복 키가 무한 누적됐다). */
    static bool _finalizeDescriptorS( const std::string &path, const std::string &ts ) {
        std::string c = _readFile( path );
        if ( c.empty() ) return false;
        _replace( c, "\"state\":\"active\"", "\"state\":\"ended\"" );
        static const char kKey[] = "\"end_time\":\"";
        size_t k = c.find( kKey );
        if ( k != std::string::npos ) {
            size_t vs = k + sizeof( kKey ) - 1;
            size_t ve = c.find( '"', vs );
            if ( ve != std::string::npos ) c.replace( vs, ve - vs, ts );
        } else {
            size_t lb = c.rfind( '}' );
            if ( lb != std::string::npos ) c.insert( lb, std::string( ",\"end_time\":\"" ) + ts + "\"" );
        }
        return _writeFileS( path, c );
    }

    /** 원자 쓰기: .tmp 로 쓰고 rename (POSIX atomic) — 상위 디렉터리 보장 포함 */
    static bool _atomicWriteS( const std::string &path, const std::string &content ) {
        std::string tmp = path + ".tmp";
        if ( !_writeFileS( tmp, content ) ) return false;
        return rename( tmp.c_str(), path.c_str() ) == 0;
    }

    /** state 디렉터리에서 needle 매칭 파일 제거 */
    static bool _removeStatesMatchingS( const std::string &dir, const std::string &needle ) {
        DIR *d = opendir( dir.c_str() );
        if ( !d ) return false;
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
        return true;
    }

    /** VoIP Answer 시 state 파일 state=ringing → active + answered_at 업데이트 */
    static bool _promoteVoipStatesS( const std::string &dir, const std::string &needle, const std::string &ts ) {
        DIR *d = opendir( dir.c_str() );
        if ( !d ) return false;
        bool bOk = true;
        struct dirent *ent;
        while ( ( ent = readdir( d ) ) != nullptr ) {
            if ( ent->d_name[0] == '.' ) continue;
            std::string path = dir + "/" + ent->d_name;
            std::string c = _readFile( path );
            if ( c.find( needle ) == std::string::npos ) continue;
            _replace( c, "\"state\":\"ringing\"", "\"state\":\"active\"" );
            _replace( c, "\"answered_at\":null", std::string( "\"answered_at\":\"" ) + ts + "\"" );
            if ( !_atomicWriteS( path, c ) ) bOk = false;
        }
        closedir( d );
        return bOk;
    }

    /** state 디렉토리 내 모든 *.json 제거 (기동 시 호출) */
    static bool _purgeStateDirS( const std::string &dir ) {
        DIR *d = opendir( dir.c_str() );
        if ( !d ) return false;
        struct dirent *ent;
        while ( ( ent = readdir( d ) ) != nullptr ) {
            if ( ent->d_name[0] == '.' ) continue;
            std::string path = dir + "/" + ent->d_name;
            ::unlink( path.c_str() );
        }
        closedir( d );
        return true;
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
                           Esc( body ? body : "" ) + "\"}\n";
        std::string path = dir + "/" + m_strComponent + ".jsonl";
        m_worker.Enqueue( [path, line]() { return _appendLineS( path, line ); }, line.size() );
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
    //   원자 쓰기: .tmp 로 쓰고 rename (POSIX atomic) — worker 가 수행

    std::string _sessionIdByCallId( const std::string &strCallId ) {
        auto it = m_mapCallSession.find( strCallId );
        return ( it != m_mapCallSession.end() ) ? it->second : "";
    }

    std::string _stateFilePath( const std::string &kind, const std::string &subId ) {
        return m_strCallsDir + "/state/" + kind + "/" + San( subId, 64 ) + ".json";
    }

    void _writeVoipState( const std::string &subId, const std::string &callId, const std::string &sessionId,
                          const std::string &peerId, const std::string &role, const std::string &state, const char *ts,
                          const std::string &recordDir, bool bVideo, const std::string &mediaNode = "" ) {
        if ( m_strCallsDir.empty() || subId.empty() ) return;
        std::string body = std::string( "{\"kind\":\"volte\"" ) + ",\"subscriber_id\":\"" + Esc( subId ) + "\"" +
                           ",\"session_id\":\"" + Esc( sessionId ) + "\"" + ",\"call_id\":\"" + Esc( callId ) + "\"" +
                           ",\"peer_id\":\"" + Esc( peerId ) + "\"" + ",\"role\":\"" + Esc( role ) + "\"" +
                           ",\"state\":\"" + Esc( state ) + "\"" + ",\"video\":" + ( bVideo ? "true" : "false" ) +
                           ",\"media_node\":\"" + Esc( mediaNode ) + "\"" + ",\"started_at\":\"" + ts + "\"" +
                           ",\"answered_at\":null" + ",\"record_dir\":\"" + Esc( recordDir ) + "\"}\n";
        std::string path = _stateFilePath( "volte", subId );
        m_worker.Enqueue( [path, body]() { return _atomicWriteS( path, body ); }, body.size() );
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
        std::string path = _stateFilePath( "ptt", subId );
        m_worker.Enqueue( [path, body]() { return _atomicWriteS( path, body ); }, body.size() );
    }

    void _removePttState( const std::string &subId ) {
        if ( m_strCallsDir.empty() || subId.empty() ) return;
        std::string path = _stateFilePath( "ptt", subId );
        m_worker.Enqueue( [path]() {
            ::unlink( path.c_str() );
            return true;
        } );
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
        m_worker.Enqueue( [dir, needle]() { return _removeStatesMatchingS( dir, needle ); } );
    }

    /** VoIP Answer 시 state 파일 state=ringing → active + answered_at 업데이트.
     *  session_id 매칭 (leg A/B 어느 call_id 로 호출되든 양쪽 state 파일 갱신). */
    void _promoteVoipStates( const std::string &strCallId, const char *ts ) {
        if ( m_strCallsDir.empty() || strCallId.empty() ) return;
        std::string sessId = _sessionIdByCallId( strCallId );
        std::string needle =
            sessId.empty() ? "\"call_id\":\"" + Esc( strCallId ) + "\"" : "\"session_id\":\"" + Esc( sessId ) + "\"";
        std::string dir = m_strCallsDir + "/state/volte";
        std::string tsStr = ts;
        m_worker.Enqueue( [dir, needle, tsStr]() { return _promoteVoipStatesS( dir, needle, tsStr ); } );
    }

    /** 그룹 세션 종료 시 해당 그룹의 잔여 state 파일 일괄 제거 */
    void _removePttStatesByGroupId( const std::string &strGroupId ) {
        if ( m_strCallsDir.empty() || strGroupId.empty() ) return;
        std::string needle = "\"group_id\":\"" + Esc( strGroupId ) + "\"";
        std::string dir = m_strCallsDir + "/state/ptt";
        m_worker.Enqueue( [dir, needle]() { return _removeStatesMatchingS( dir, needle ); } );
    }
};

extern CCallDir gclsCallDir;

#endif
