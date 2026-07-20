/*
 * Copyright (C) 2012 Yee Young Han <websearch@naver.com>
 (http://blog.naver.com/websearch)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "CallMap.h"

#include "CmpClient.h"
#include "Log.h"
#include "MemoryDebug.h"
#include "SipServer.h"

CCallMap gclsCallMap;
CCallMap gclsTransCallMap;

CCallInfo::CCallInfo() : m_bRecv( false ), m_iPeerRtpPort( -1 ), m_iLastActivityTime( 0 ), m_bEstablished( false ) {
    time( &m_iLastActivityTime );
}

CCallMap::CCallMap() {
}

CCallMap::~CCallMap() {
}

/**
 * @ingroup CspServer
 * @brief 통화 요청 Call-ID 와 전달된 통화 요청 Call-ID 를 자료구조에 저장한다.
 * @param pszRecvCallId 통화 요청 Call-ID
 * @param pszSendCallId 전달된 통화 요청 Call-ID
 * @param iStartRtpPort	생성된 RTP 포트에서 시작 포트 번호
 *
 pszSendCallId 와 연동하는 RTP 포트 번호이다.
 * @returns true 를 리턴한다.
 */
bool CCallMap::Insert( const char *pszRecvCallId, const char *pszSendCallId, int iStartRtpPort ) {
    return Insert( pszRecvCallId, pszSendCallId, iStartRtpPort, iStartRtpPort );
}

/**
 * @ingroup CspServer
 * @brief leg 별 relay 포트 저장 — 각 entry 의 m_iPeerRtpPort 는 그 leg 의 peer 에게
 *        광고할 relay 포트다 (수신 leg entry = 발신 leg SDP 포트, 발신 leg entry = 수신 leg SDP 포트).
 */
bool CCallMap::Insert( const char *pszRecvCallId, const char *pszSendCallId, int iRecvRtpPort, int iSendRtpPort ) {
    CALL_MAP::iterator itMap;
    m_clsMutex.acquire();
    // INVITE 메시지를 수신한 Dialog 를 저장한다.
    itMap = m_clsMap.find( pszRecvCallId );
    if ( itMap == m_clsMap.end() ) {
        CCallInfo clsCallInfo;
        clsCallInfo.m_strPeerCallId = pszSendCallId;
        clsCallInfo.m_bRecv = true;
        if ( iRecvRtpPort > 0 ) {
            clsCallInfo.m_iPeerRtpPort = iRecvRtpPort;
        }
        m_clsMap.insert( CALL_MAP::value_type( pszRecvCallId, clsCallInfo ) );
    }
    // INVITE 메시지를 전송한 Dialog 를 저장한다.
    itMap = m_clsMap.find( pszSendCallId );
    if ( itMap == m_clsMap.end() ) {
        CCallInfo clsCallInfo;
        clsCallInfo.m_strPeerCallId = pszRecvCallId;
        clsCallInfo.m_bRecv = false;
        if ( iSendRtpPort > 0 ) {
            clsCallInfo.m_iPeerRtpPort = iSendRtpPort;
        }
        m_clsMap.insert( CALL_MAP::value_type( pszSendCallId, clsCallInfo ) );
    }
    m_clsMutex.release();
    return true;
}

/**
 * @ingroup CspServer
 * @brief SIP Call-ID 와 이와 연관된 통화 정보를 자료구조에 저장한다.
 * @param pszCallId		SIP Call-ID
 * @param clsCallInfo 통화 정보
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CCallMap::Insert( const char *pszCallId, CCallInfo &clsCallInfo ) {
    CALL_MAP::iterator itMap;
    bool bRes = false;
    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszCallId );
    if ( itMap == m_clsMap.end() ) {
        m_clsMap.insert( CALL_MAP::value_type( pszCallId, clsCallInfo ) );
        bRes = true;
    }
    m_clsMutex.release();
    return bRes;
}

/**
 * @ingroup CspServer
 * @brief 상대방 Call-ID 를 수정한다.
 * @param pszCallId			SIP Call-ID
 * @param pszPeerCallId 상대방 SIP Call-ID
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CCallMap::Update( const char *pszCallId, const char *pszPeerCallId ) {
    CALL_MAP::iterator itMap;
    bool bRes = false;
    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszCallId );
    if ( itMap != m_clsMap.end() ) {
        itMap->second.m_strPeerCallId = pszPeerCallId;
        bRes = true;
    }
    m_clsMutex.release();
    return bRes;
}

/**
 * @ingroup CspServer
 * @brief Call-ID 와 연결된 Call-ID 를 검색한다.
 * @param pszCallId SIP Call-ID
 * @param strCallId 연결된 SIP Call-ID
 * @returns 검색되면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CCallMap::Select( const char *pszCallId, std::string &strCallId ) {
    CALL_MAP::iterator itMap;
    bool bRes = false;
    strCallId.clear();
    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszCallId );
    if ( itMap != m_clsMap.end() ) {
        strCallId = itMap->second.m_strPeerCallId;
        bRes = true;
    }
    m_clsMutex.release();
    return bRes;
}

/**
 * @ingroup CspServer
 * @brief Call-ID 에 대한 정보를 저장한다.
 * @param pszCallId SIP Call-ID
 * @param clsCallInfo Call-ID 정보를 저장할 객체
 * @returns 검색되면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CCallMap::Select( const char *pszCallId, CCallInfo &clsCallInfo ) {
    CALL_MAP::iterator itMap;
    bool bRes = false;
    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszCallId );
    if ( itMap != m_clsMap.end() ) {
        clsCallInfo = itMap->second;
        bRes = true;
    }
    m_clsMutex.release();
    return bRes;
}

/**
 * @ingroup CspServer
 * @brief Call-ID 에 대한 정보를 저장한다.
 * @param pszCallId SIP Call-ID
 * @returns 검색되면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CCallMap::Select( const char *pszCallId ) {
    CALL_MAP::iterator itMap;
    bool bRes = false;
    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszCallId );
    if ( itMap != m_clsMap.end() ) {
        bRes = true;
    }
    m_clsMutex.release();
    return bRes;
}

/**
 * @brief INVITE 메시지를 전송하고 통화 Ring 중인 Call ID 를 검색한다.
 * @param pszTo			SIP TO 아이디
 * @param strCallId SIP Call-ID 를 저장할 변수
 * @returns INVITE 메시지를 전송하고 통화 Ring 중인 Call ID 가 검색되면 true 를
 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CCallMap::SelectToRing( const char *pszTo, std::string &strCallId ) {
    CALL_MAP::iterator itMap;
    bool bRes = false;
    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        if ( itMap->second.m_bRecv ) continue;
        if ( gclsUserAgent.IsRingCall( itMap->first.c_str(), pszTo ) == false ) continue;
        strCallId = itMap->first;
        bRes = true;
        break;
    }
    m_clsMutex.release();
    return bRes;
}

/**
 * @ingroup CspServer
 * @brief Call-ID 와 연결된 Call-ID 를 자료구조에서 삭제한다.
 * @param pszCallId SIP Call-ID
 * @param bStopPort	RTP 포트 사용을 중지시킬 것인가?
 * @returns 성공하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CCallMap::Delete( const char *pszCallId, bool bStopPort ) {
    CALL_MAP::iterator itMap;
    bool bRes = false;
    std::string strCallId;
    // teardown 대상 CMP relay 세션을 포트가 아닌 session_id(전역 유일)로 지목 → 노드간 포트충돌 오지목/누수 차단.
    std::string strRelaySid, strRelaySesId, strRelayCaller, strRelayCallee;
    auto captureRelay = [&]( const CCallInfo &ci ) {
        if ( strRelaySid.empty() && !ci.m_strRelaySessionId.empty() ) {
            strRelaySid = ci.m_strRelaySessionId;
            strRelaySesId = ci.m_strRelaySesId;
            strRelayCaller = ci.m_strRelayCaller;
            strRelayCallee = ci.m_strRelayCallee;
        }
    };
    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszCallId );
    if ( itMap != m_clsMap.end() ) {
        strCallId = itMap->second.m_strPeerCallId;
        captureRelay( itMap->second );
        m_clsMap.erase( itMap );
        bRes = true;
    }
    if ( bRes ) {
        itMap = m_clsMap.find( strCallId );
        if ( itMap != m_clsMap.end() ) {
            captureRelay( itMap->second );
            m_clsMap.erase( itMap );
        }
    }
    m_clsMutex.release();
    // CMP 호출은 lock 해제 후(네트워크 I/O). PTT 그룹 호는 relay session 이 없어(LeaveGroup 경로) skip.
    if ( bStopPort && !strRelaySid.empty() ) {
        gclsCmpClient.RemoveSession( strRelaySid, strRelayCaller, strRelayCallee, strRelaySesId );
        CLog::Print( LOG_DEBUG, "CallMap::Delete(%s) -> RemoveSession session=%s", pszCallId, strRelaySid.c_str() );
    } else {
        CLog::Print( LOG_DEBUG, "CallMap::Delete(%s) SKIPPED RemoveSession (relaySid='%s', bStopPort=%d)", pszCallId,
                     strRelaySid.c_str(), bStopPort );
    }
    return bRes;
}

void CCallMap::SetRelayInfo( const char *pszCallId, const std::string &strSessionId, const std::string &strSesId,
                             const std::string &strLocalIp, const std::string &strCaller,
                             const std::string &strCallee ) {
    m_clsMutex.acquire();
    auto apply = [&]( CALL_MAP::iterator it ) {
        if ( it == m_clsMap.end() ) return;
        it->second.m_strRelaySessionId = strSessionId;
        it->second.m_strRelaySesId = strSesId;
        it->second.m_strRelayLocalIp = strLocalIp;
        it->second.m_strRelayCaller = strCaller;
        it->second.m_strRelayCallee = strCallee;
    };
    auto it = m_clsMap.find( pszCallId );
    std::string strPeer;
    if ( it != m_clsMap.end() ) {
        strPeer = it->second.m_strPeerCallId;
        apply( it );
    }
    if ( !strPeer.empty() ) apply( m_clsMap.find( strPeer ) );  // B2BUA 양 leg 동일 relay 공유
    m_clsMutex.release();
}

bool CCallMap::DeleteOne( const char *pszCallId ) {
    CALL_MAP::iterator itMap;
    bool bRes = false;
    m_clsMutex.acquire();
    itMap = m_clsMap.find( pszCallId );
    if ( itMap != m_clsMap.end() ) {
        m_clsMap.erase( itMap );
        bRes = true;
    }
    m_clsMutex.release();
    return bRes;
}

/**
 * @ingroup CspServer
 * @brief 마지막 activity 이후 iTimeoutSec 초가 지난 stale 통화를 종료한다.
 * @param iTimeoutSec 타임아웃 시간 (초)
 */
void CCallMap::SetEstablished( const char *pszCallId ) {
    time_t iNow;
    time( &iNow );
    m_clsMutex.acquire();
    auto itMap = m_clsMap.find( pszCallId );
    if ( itMap != m_clsMap.end() ) {
        itMap->second.m_bEstablished = true;
        itMap->second.m_iLastActivityTime = iNow;
        // peer leg 도 동일 표시
        auto itPeer = m_clsMap.find( itMap->second.m_strPeerCallId );
        if ( itPeer != m_clsMap.end() ) {
            itPeer->second.m_bEstablished = true;
            itPeer->second.m_iLastActivityTime = iNow;
        }
    }
    m_clsMutex.release();
}

void CCallMap::DeleteTimeout( int iTimeoutSec ) {
    // 미확립(pending) 호: INVITE 트랜잭션 사망(~32s) 후 확실히 정리 → relay 누수 방지.
    //   (호 실패(4xx-6xx) 시 psip 가 EventCallEnd 통보를 놓치는 경우의 안전망.)
    // 확립(established) 호: BYE(EventCallEnd)로만 종료. 장시간 호를 강제종료하지 않도록
    //   매우 긴 안전 상한(기본 6h)에서만 회수. (구버전은 단일 timeout 으로 5분 호도 끊던 잠재버그)
    if ( iTimeoutSec <= 0 ) iTimeoutSec = 300;
    int iPendingTimeout = ( iTimeoutSec < 60 ) ? iTimeoutSec : 60;
    int iEstablishedCap = 21600;  // 6h 안전 상한

    std::list<std::string> clsStaleList;
    time_t iNow;
    time( &iNow );

    m_clsMutex.acquire();
    for ( auto itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        if ( itMap->second.m_iLastActivityTime <= 0 ) continue;
        time_t age = iNow - itMap->second.m_iLastActivityTime;
        bool bStale = itMap->second.m_bEstablished ? ( age >= iEstablishedCap ) : ( age >= iPendingTimeout );
        if ( bStale ) clsStaleList.push_back( itMap->first );
    }
    m_clsMutex.release();

    for ( const auto &strCallId : clsStaleList ) {
        // 정상 환경에선 여기 도달 자체가 비정상(고아 teardown 미완) — WARN 성격으로 남긴다.
        CLog::Print( LOG_INFO, "Stale call reclaim: CallId(%s) — StopCall + relay free (teardown 누락 신호)",
                     strCallId.c_str() );
        gclsUserAgent.StopCall( strCallId.c_str() );
        Delete( strCallId.c_str() );  // bStopPort=true → session_id 로 CmpClient::RemoveSession (relay 회수)
    }
}

// audit 수준2 — 보유 중인 relay 세션ID 집합 수집. B2BUA 양 leg 가 동일 relay 를 공유해
//   중복 등장하나 set 이 dedup 한다. PTT(그룹) 호는 relay 세션ID 가 비어 제외된다.
void CCallMap::CollectRelaySessionIds( std::set<std::string> &setOut ) {
    m_clsMutex.acquire();
    for ( auto itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        if ( !itMap->second.m_strRelaySessionId.empty() )
            setOut.insert( itMap->second.m_strRelaySessionId );
    }
    m_clsMutex.release();
}

// audit zombie teardown — CMP 에 없는(=미디어 소실) relay 를 가진 호를 StopCall+Delete.
//   setLiveOnCmp = CMP 가 실제 보유한 relay 세션ID. 그에 없으면 좀비. 회수 상한 iMaxCount.
int CCallMap::ReclaimZombieBySessionId( const std::set<std::string> &setLiveOnCmp, int iMaxCount ) {
    std::list<std::pair<std::string, std::string>> clsZombie;  // (callId, sessionId)
    m_clsMutex.acquire();
    for ( auto itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        const std::string &sid = itMap->second.m_strRelaySessionId;
        if ( sid.empty() ) continue;
        if ( setLiveOnCmp.find( sid ) == setLiveOnCmp.end() )
            clsZombie.push_back( std::make_pair( itMap->first, sid ) );
    }
    m_clsMutex.release();

    int iDone = 0;
    for ( const auto &z : clsZombie ) {
        if ( iDone >= iMaxCount ) break;
        CLog::Print( LOG_INFO, "Audit zombie teardown: CallId(%s) relay=%s (CMP 미보유 — 미디어 소실)",
                     z.first.c_str(), z.second.c_str() );
        gclsUserAgent.StopCall( z.first.c_str() );
        Delete( z.first.c_str() );  // bStopPort=true → RemoveSession (이미 CMP 소실이라 no-op 멱등)
        ++iDone;
    }
    return iDone;
}

/**
 * @ingroup CspServer
 * @brief 모든 통화를 종료시킨다.
 */
void CCallMap::StopCallAll() {
    CALL_MAP::iterator itMap;
    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        gclsUserAgent.StopCall( itMap->first.c_str() );
    }
    m_clsMutex.release();
}

/**
 * @ingroup CspServer
 * @brief 통화 개수를 리턴한다.
 * @returns 통화 개수를 리턴한다.
 */
int CCallMap::GetCount() {
    int iCount;
    m_clsMutex.acquire();
    iCount = (int)m_clsMap.size();
    m_clsMutex.release();
    return iCount;
}

/**
 * @ingroup CspServer
 * @brief 통화 맵 모니터링용 문자열을 생성한다.
 * @param strBuf 통화 맵 모니터링용 문자열 저장 변수
 */
void CCallMap::GetString( CMonitorString &strBuf ) {
    CALL_MAP::iterator itMap;
    strBuf.Clear();
    m_clsMutex.acquire();
    for ( itMap = m_clsMap.begin(); itMap != m_clsMap.end(); ++itMap ) {
        strBuf.AddCol( itMap->first );
        strBuf.AddCol( itMap->second.m_strPeerCallId );
        strBuf.AddCol( itMap->second.m_bRecv ? "recv" : "" );
        strBuf.AddRow( itMap->second.m_iPeerRtpPort );
    }
    m_clsMutex.release();
}
