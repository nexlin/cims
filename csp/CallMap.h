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

#include <functional>
#include <map>
#include <set>
#include <string>

#include "MediaSdes.h"
#include "SipMutex.h"

class CMonitorString;

/**
 * @ingroup CspServer
 * @brief B2BUA 한 leg 이 대표하는 dialog 의 당사자 — dialog 이벤트(RFC 4235)·Replaces/Join 인가 판정의 공통 해석 단위.
 *
 *   psip CSipDialog 의 From/To 는 "CSP 가 요청을 보내는 입장" 이라 수신 leg 에서는 From=다이얼된 번호·To=발신자로
 *   뒤집힌다. 그래서 caller/callee 를 GetFromId/GetToId 로 읽으면 수신 leg(A) 에서 당사자가 바뀐다. 이 구조체는
 *   방향과 무관하게 옳은 두 사실만 담는다: 당사자 = 그 leg 의 원단 사용자(GetToId), 개시자 여부 = CSP 수신 leg
 *   (그 당사자가 INVITE 를 보냈다 = RFC 4235 direction "initiator").
 */
struct CallLegParty {
    std::string strCallId;    ///< 당사자가 가진 dialog = CSP 측 leg Call-ID (dialog-info id, Replaces/Join 대상)
    std::string strUser;      ///< 당사자(leg 원단 사용자)
    bool bInitiator = false;  ///< 당사자가 dialog 를 개시(INVITE 송신)했는가
};

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

    /** CMP relay 세션 식별자(csp_{yyyymmddHHMMSSmmm}_{n}, 재시작 경계 포함 전역 유일). teardown/MODIFY 가 포트가 아닌
     * 이 키로 CMP 세션을 지목한다 — 멀티 미디어노드에서 포트는 노드별 비유일이라 포트키로는 오지목/누수가 발생했다(구
     * CRtpMap 버그). PTT(그룹) 호는 group teardown(LeaveGroup)을 쓰므로 비어 있다. */
    std::string m_strRelaySessionId;
    std::string m_strRelaySesId;    // Flow 상관 sesid
    std::string m_strRelayLocalIp;  // CMP relay IP (SDP 광고 / answer MODIFY 에 사용, 구 GetLocalIp 대체)
    std::string m_strRelayCaller;
    std::string m_strRelayCallee;

    /** relay leg 별 미디어 SRTP(SDES) 협상 상태 — [0]=수신(caller/peer0), [1]=발신(callee/peer1).
     *  answer 재작성(offer echo)·re-INVITE 키 유지/갱신·CMP media_crypto 조립의 원천
     *  (media_security.md §5.2). 양 leg entry 에 동일하게 기록된다(SetRelaySdesLeg). */
    RelaySdesLeg m_clsSdesLeg[2];

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

    /** relay leg(iLeg: 0=수신/peer0, 1=발신/peer1)의 SDES 상태를 양 leg entry 에 기록. */
    void SetRelaySdesLeg( const char *pszCallId, int iLeg, const RelaySdesLeg &clsLeg );
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

    /** 특정 CMP relay session(cmp_sess_N)을 쓰던 B2BUA 양 leg 에 BYE 를 보내고 로컬 레코드를 정리한다.
     *  미디어 노드(CMP) 다운으로 relay 가 이미 소실된 호의 능동 종료용 — dead node 이므로
     *  CmpClient::RemoveSession(blocking) 은 호출하지 않는다(bStopPort=false). 종료한 호 수를 반환. */
    int TerminateByRelaySession( const std::string &strRelaySessionId );
    int GetCount();

    /** audit 수준2 — 현재 보유 중인 CMP relay 세션 식별자 집합 수집(비어있지 않은 것만).
     *  CSP 측 세션집합 지문/ diff 의 원천. (CmpClient AuditCycle 이 CMP digest 와 대조) */
    void CollectRelaySessionIds( std::set<std::string> &setOut );
    /** audit zombie teardown — relay 세션ID 로 호를 지목해 StopCall+Delete(호 강제 종료).
     *  CMP 에 해당 relay 가 소실(재기동 등)돼 미디어가 죽은 좀비 호 정리. 회수 건수 반환. */
    int ReclaimZombieBySessionId( const std::set<std::string> &setLiveOnCmp, int iMaxCount );
    /** RELAY_ABORTED 이벤트 처리 — 단일 relay 세션ID 를 가진 호를 즉시 종료(StopCall+Delete).
     *  CMP sweeper 가 이미 relay 를 회수했으므로 미디어가 죽은 호. 찾아 종료했으면 true(멱등). */
    bool TeardownByRelaySessionId( const std::string &strSessionId );

    void GetString( CMonitorString &strBuf );

    /** 활성 호 read-only 순회 (dialog 초기 full 스냅샷 — RFC 4235 §3.2). 콜백에서 map 을 수정하지 말 것. */
    void Iterate( const std::function<void( const std::string &, const CCallInfo & )> &fn );

    /** 호 양 당사자 해석 — clsThis = pszCallId leg 의 당사자, clsPeer = 상대 leg 의 당사자 (CallLegParty 참조).
     *  leg 가 CallMap 에 없으면 false. peer leg 가 아직 없으면 clsPeer 는 빈 값. */
    bool ResolveLegParties( const char *pszCallId, CallLegParty &clsThis, CallLegParty &clsPeer );
    /** Iterate 콜백처럼 CCallInfo 를 이미 쥔 자리용(맵 락 재진입 없음). */
    static void ResolveLegParties( const std::string &strCallId, const CCallInfo &clsCallInfo, CallLegParty &clsThis,
                                   CallLegParty &clsPeer );

private:
    CALL_MAP m_clsMap;
    CSipMutex m_clsMutex;
};

extern CCallMap gclsCallMap;
extern CCallMap gclsTransCallMap;

#endif
