/*
 * Copyright (C) 2012 Yee Young Han <websearch@naver.com> (http://blog.naver.com/websearch)
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

#ifndef _CALL_MAP_H_
#define _CALL_MAP_H_

#include <map>
#include <string>

#include "SipMutex.h"

class CMonitorString;

/**
 * @ingroup CspServer
 * @brief 통화 정보 저장 클래스
 */
class CCallInfo {
public:
    CCallInfo();

    /** 상대 SIP 클라이언트와 연결된 통화 SIP Call-ID */
    std::string m_strPeerCallId;

    /** 최초 INVITE 를 수신하였는가? */
    bool m_bRecv;

    /** 상대 SIP 클라이언트와 연동하는 RTP relay 포트 번호(=CMP 가 할당한 relay local 포트). SDP 광고용.
     *  RTP relay 기능을 사용하지 않으면 -1 이 저장된다. */
    int m_iPeerRtpPort;

    /** CMP relay 세션 식별자(csp_{yyyymmddHHMMSSmmm}_{n}, 재시작 경계 포함 전역 유일). teardown/MODIFY 가 포트가 아닌 이 키로 CMP 세션을
     *  지목한다 — 멀티 미디어노드에서 포트는 노드별 비유일이라 포트키로는 오지목/누수가 발생했다(구 CRtpMap 버그).
     *  PTT(그룹) 호는 group teardown(LeaveGroup)을 쓰므로 비어 있다. */
    std::string m_strRelaySessionId;
    std::string m_strRelaySesId;    // Flow 상관 sesid
    std::string m_strRelayLocalIp;  // CMP relay IP (SDP 광고 / answer MODIFY 에 사용, 구 GetLocalIp 대체)
    std::string m_strRelayCaller;
    std::string m_strRelayCallee;

    /** 마지막 SIP activity 시간 (통화 생성/갱신 시 기록) */
    time_t m_iLastActivityTime;

    /** 200 OK 로 확립(answer)되었는가? — sweeper 가 미확립(pending) 호를 빠르게 회수하고
     *  확립 호는 BYE 로만 종료(장시간 호 강제종료 방지)하기 위함. */
    bool m_bEstablished;
};

/**
 * @ingroup CspServer
 * @brief 연결된 통화 정보를 저장하는 자료구조. key 와 value 는 SIP Call-ID 이다.
 */
typedef std::map<std::string, CCallInfo> CALL_MAP;

/**
 * @ingroup CspServer
 * @brief 연결된 통화 정보를 저장하는 자료구조 클래스
 */
class CCallMap {
public:
    CCallMap();
    ~CCallMap();

    bool Insert( const char *pszRecvCallId, const char *pszSendCallId, int iStartRtpPort );
    // leg 별 포트: entry 별로 다른 relay 포트 저장 — m_iPeerRtpPort = 그 leg 의 peer 에게 광고할 포트.
    bool Insert( const char *pszRecvCallId, const char *pszSendCallId, int iRecvRtpPort, int iSendRtpPort );
    bool Insert( const char *pszCallId, CCallInfo &clsCallInfo );

    /** CMP relay descriptor 를 해당 Call-ID 와 그 peer leg 양쪽에 기록 (B2BUA 양 leg 동일 relay 공유).
     *  teardown(Delete)·answer MODIFY 가 이 정보를 읽어 CMP 세션을 session_id 로 직접 지목한다. */
    void SetRelayInfo( const char *pszCallId, const std::string &strSessionId, const std::string &strSesId,
                       const std::string &strLocalIp, const std::string &strCaller, const std::string &strCallee );
    bool Update( const char *pszCallId, const char *pszPeerCallId );
    bool Select( const char *pszCallId, std::string &strCallId );
    bool Select( const char *pszCallId, CCallInfo &clsCallInfo );
    bool Select( const char *pszCallId );
    bool SelectToRing( const char *pszTo, std::string &strCallId );
    bool Delete( const char *pszCallId, bool bStopPort = true );
    bool DeleteOne( const char *pszCallId );

    /** 호를 확립(answer) 상태로 표시 (해당 callId + peer). EventCallStart 에서 호출. */
    void SetEstablished( const char *pszCallId );

    void DeleteTimeout( int iTimeoutSec );
    void StopCallAll();
    int GetCount();

    void GetString( CMonitorString &strBuf );

private:
    CALL_MAP m_clsMap;
    CSipMutex m_clsMutex;
};

extern CCallMap gclsCallMap;
extern CCallMap gclsTransCallMap;

#endif
