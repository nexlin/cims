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
#include <vector>

#include "MediaSdes.h"
#include "RelayCodec.h"
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

    /** relay leg 별 오디오 코덱 상태 — [0]=수신(peer0), [1]=발신(peer1). 오퍼 코덱 목록(코덱 삽입 뒤)·telephone-event
     * PT·answer 뒤 협상 코덱· 변환 여부(cmp.md §11 — 다르면 CMP media_codec, A-leg answer/re-offer 는 자기 코덱으로
     * 재작성). 양 leg entry 에 동일 기록. */
    RelayCodec::LegCodecs m_clsCodecLeg[2];

    /** B-leg 가 RoutingPolicy 로 고른 피어 Route 일 때의 라우팅 상태(sip_service_model.md §2-4 재라우팅) — 양 leg entry
     * 동일. B-leg 가 5xx·타임아웃으로 끝나면 같은 RouteSet 의 시도하지 않은 alive 멤버로 새 B-leg 를 낸다(RFC 3261
     * §16.7 순차 forking, TS 24.229 §5.10 alternative routing). 비어 있으면 피어 leg 가 아니다(가입자 B-leg). */
    std::string m_strRouteSet;                  // RouteSet name
    std::string m_strRouteName;                 // 현재 B-leg 의 Route
    std::string m_strRoutePolicy;               // 고른 RoutingPolicy(로그)
    std::string m_strRouteHashKey;              // hash_by_caller 키(발신자 from@host)
    std::vector<std::string> m_vecRoutesTried;  // 이미 실패한 Route(현재 것 제외)

    /** 발신자의 안내음성 프로파일 이름(announcements.md §6.2 — 피어면 RemoteNode, 가입자면 접속서비스). INVITE 때 정해
     * 양 leg entry 에 기록. 비면 Setup.Announcement.DefaultProfile */
    std::string m_strAnnProfile;
    /** 통화중대기 착신(TS 24.615) — 착신자가 이미 확립 호 중이라 B-leg INVITE 에 Alert-Info
     * urn:alert:service:call-waiting 을 실었다. 발신자 링백은 프로파일 `call_waiting` 상황을 본다(announcements.md §11
     * → P1 후속 구현). 양 leg entry 동일 */
    bool m_bCallWaiting = false;

    /** 착신전환 상태(TS 24.604, volte_supplementary_services.md §6A) — 양 leg entry 동일. m_strHistoryInfo = B-leg
     * INVITE 에 실은 History-Info 값(수신 INVITE 의 것 + 이번 전환들; 조건부 전환이 이어 붙인다), m_iCdivHops = 그 안의
     * 전환 수(상한 판정), m_strCdivServed = 이 B-leg 착신이 전환된 원착신(비면 전환 없음). */
    std::string m_strHistoryInfo;
    int m_iCdivHops = 0;
    std::string m_strCdivServed;
    /** CFNR 무응답 시한(epoch 초, 0 = 없음) — 가입자 B-leg 의 첫 18x 에 착신 가입자 forward_no_reply_id 가 있으면
     * 잡는다. 디스패처 Tick 이 만료를 보고 CANCEL + 전환 대상으로 새 B-leg (§6A.4). B-leg entry 에만 */
    time_t m_iNoReplyDeadline = 0;

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
    /** relay leg(iLeg) 의 코덱 상태를 양 leg entry 에 기록 (RelayCodec, cmp.md §11). */
    void SetRelayCodecLeg( const char *pszCallId, int iLeg, const RelayCodec::LegCodecs &clsLeg );
    /** 발신자 안내 프로파일을 양 leg entry 에 기록 (announcements.md §6.2) */
    void SetAnnProfile( const char *pszCallId, const std::string &strProfile );
    /** 통화중대기 표식을 양 leg entry 에 기록 */
    void SetCallWaiting( const char *pszCallId, bool bCw );
    /** 이 가입자가 당사자(발/착)인 확립 호가 있는가 — 통화중대기 판정(TS 24.615 §4.5.2.1) */
    bool HasEstablishedCallFor( const std::string &strUser );
    /** 이 가입자의 확립 호 leg — 그 가입자 쪽 leg 의 Call-ID·entry 와 relay peer index(0 = relay caller 쪽, 1 = callee
     * 쪽). 통화중대기 in-band 대기음(announcements.md §3.6)이 붙을 leg. 없으면 false */
    bool FindEstablishedLegFor( const std::string &strUser, std::string &strCallId, CCallInfo &clsInfo, int &iPeerIdx );
    /** 착신전환 상태를 양 leg entry 에 기록 (§6A) */
    void SetCdivInfo( const char *pszCallId, const std::string &strHistoryInfo, int iHops,
                      const std::string &strServed );
    /** CFNR 시한 — 그 leg entry 에만 */
    void SetNoReplyDeadline( const char *pszCallId, time_t tDeadline );
    /** 피어 B-leg 의 라우팅 상태(RouteSet·Route·정책·해시키·실패 Route 목록)를 양 leg entry 에 기록 — 재라우팅의 근거.
     */
    void SetRouteInfo( const char *pszCallId, const std::string &strRouteSet, const std::string &strRoute,
                       const std::string &strPolicy, const std::string &strHashKey,
                       const std::vector<std::string> &vecTried );
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
