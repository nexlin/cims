#ifndef _MODULE_DISPATCHER_H_
#define _MODULE_DISPATCHER_H_

#include <map>
#include <string>

#include "CscfModule.h"
#include "IModule.h"
#include "IbcfModule.h"
#include "McDataAsModule.h"
#include "PttAsModule.h"
#include "SipMutex.h"
#include "SipUserAgent.h"
#include "TasModule.h"

/**
 * @brief 중앙 디스패처 — CSipServer 를 대체
 *
 * ISipStackCallBack: REGISTER, SUBSCRIBE, Proxy INVITE 처리
 * ISipUserAgentCallBack: B2BUA 호 이벤트 처리 (모듈별 분배)
 * ISipStackSecurityCallBack: 보안 정책
 *
 * RecvRequest 콜백 순서: [ModuleDispatcher, CSipUserAgent]
 *  → Proxy 대상 INVITE 는 ModuleDispatcher 가 직접 처리 (return true)
 *  → B2BUA 대상 INVITE 는 CSipUserAgent 로 전달 (return false)
 */
class CCallInfo;

class CModuleDispatcher : public ISipStackCallBack, ISipUserAgentCallBack, ISipStackSecurityCallBack {
public:
    CModuleDispatcher();
    ~CModuleDispatcher();

    bool Start( CSipStackSetup &clsSetup );
    void InitModules();

    // 콜 소유권 추적
    void SetCallOwner( const char *pszCallId, IModule *pModule );
    IModule *GetCallOwner( const char *pszCallId );
    void RemoveCallOwner( const char *pszCallId );

    // 공유 헬퍼
    bool SendResponse( CSipMessage *pclsMessage, int iStatusCode );
    /** 피어 B-leg 의 5xx·타임아웃 재라우팅(RouteSet 다음 멤버, sip_service_model.md §2-4). true = 새 B-leg 로 이어
     * 감(이 leg 종료 처리 끝). */
    bool TryRerouteLeg( const char *pszCallId, const CCallInfo &clsB, int iSipStatus );
    void StopCall( const char *pszCallId, int iResponseCode );
    void OnCallEnded( const char *pszCallId, int iSipStatus );

    // 모듈 접근자
    CCscfModule *GetCscf() {
        return &m_clsCscf;
    }
    CTasModule *GetTas() {
        return &m_clsTas;
    }
    CPttAsModule *GetPttAs() {
        return &m_clsPttAs;
    }
    CIbcfModule *GetIbcf() {
        return &m_clsIbcf;
    }
    CMcDataAsModule *GetMcDataAs() {
        return &m_clsMcDataAs;
    }

    // ISipStackCallBack
    bool RecvRequest( int iThreadId, CSipMessage *pclsMessage ) override;
    bool RecvResponse( int iThreadId, CSipMessage *pclsMessage ) override;
    void EventKeepAlive( const char *pszIp, int iPort, ESipTransport eTransport ) override;
    bool SendTimeout( int iThreadId, CSipMessage *pclsMessage ) override;

    // ISipUserAgentCallBack
    void EventRegister( CSipServerInfo *pclsInfo, int iStatus ) override;
    bool EventIncomingRequestAuth( CSipMessage *pclsMessage ) override;
    void EventIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp,
                            CSipMessage *pclsMessage = NULL ) override;
    void EventCallRing( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRtp ) override;
    void EventCallStart( const char *pszCallId, CSipCallRtp *pclsRtp ) override;
    void EventCallEnd( const char *pszCallId, int iSipStatus ) override;
    /** 종료 이벤트 + 상대 leg 가 실은 Reason(RFC 3326). B2BUA 는 종료 사유(BYE/CANCEL 의 Reason)와 최종 응답
     *  코드를 다른 leg 로 그대로 옮긴다 — 코드 매핑은 RelayEndStatus(). */
    void EventCallEnd( const char *pszCallId, int iSipStatus, const char *pszReason ) override;
    /** 한 leg 의 종료 상태를 상대 leg 의 최종 응답 코드로 옮긴다(상대 leg 가 미응답 INVITE 일 때만 쓰인다 —
     *  확립된 leg 는 BYE, 미응답 발신 leg 는 CANCEL 이라 코드가 무시된다).
     *  4xx/5xx/6xx 는 그대로(RFC 3261 §16.7 — 503 을 사용자 거절 603 으로 바꾸지 않는다),
     *  401/407 은 자격증명 없는 401 을 낼 수 없어 403, 3xx 는 B2BUA 가 재귀하지 않으므로 480,
     *  410 은 psip 의 전송 타임아웃 표지(SendTimeout)라 408, 2xx(BYE)는 0. */
    static int RelayEndStatus( int iSipStatus );
    void EventReInvite( const char *pszCallId, CSipCallRtp *pclsRemoteRtp, CSipCallRtp *pclsLocalRtp ) override;
    /** 서버가 전달한 re-INVITE 의 최종 응답 — relay SRTP leg 의 재-answer 재키잉을 CMP 에 반영
     *  (media_security.md §5.2). 주소/PT 갱신은 기존 EventReInvite→MODIFY 경로가 담당. */
    void EventReInviteResponse( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRemoteRtp ) override;
    /** 서버 발신 in-dialog 요청(BYE·re-INVITE·NOTIFY·REFER·INFO, 세션 갱신 포함)의 현재 도달 주소 —
     *  등록 바인딩(latch)을 돌려준다. fan-out INVITE·NOTIFY 가 쓰는 것과 같은 (IP, 포트, transport) 한 세트다. */
    bool EventGetLegDest( const char *pszCallId, const char *pszPeerId, std::string &strIp, int &iPort,
                          ESipTransport &eTransport ) override;
    void EventPrack( const char *pszCallId, CSipCallRtp *pclsRtp ) override;
    bool EventTransfer( const char *pszCallId, const char *pszReferToCallId, bool bScreenedTransfer ) override;
    bool EventBlindTransfer( const char *pszCallId, const char *pszReferToId ) override;
    int EventMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage ) override;

    // ISipStackSecurityCallBack
    bool IsAllowUserAgent( const char *pszSipUserAgent ) override;
    bool IsDenyUserAgent( const char *pszSipUserAgent ) override;
    bool IsAllowIp( const char *pszIp ) override;
    bool IsDenyIp( const char *pszIp ) override;

    // Proxy 포워딩 (CSCF 모드)
    bool ProxyInvite( CSipMessage *pclsMessage, const char *pszDestIp, int iDestPort, ESipTransport eTransport );

private:
    CCscfModule m_clsCscf;
    CTasModule m_clsTas;
    CPttAsModule m_clsPttAs;
    CIbcfModule m_clsIbcf;
    CMcDataAsModule m_clsMcDataAs;

    std::map<std::string, IModule *> m_mapCallOwner;
    CSipMutex m_clsOwnerMutex;
    CSipMutex m_clsMutex;

    // Proxy 모드: CallId → 발신자 정보 (Via 제거용)
    struct ProxyCallInfo {
        std::string strClientIp;
        int iClientPort;
        ESipTransport eTransport;
        int iRtpPort;  // CMP RTP relay 포트 (0 = 미사용)
        ProxyCallInfo() : iClientPort( 0 ), eTransport( E_SIP_UDP ), iRtpPort( 0 ) {
        }
    };
    std::map<std::string, ProxyCallInfo> m_mapProxyCall;
    CSipMutex m_clsProxyMutex;

    void SetProxyCall( const std::string &strCallId, const ProxyCallInfo &info );
    bool GetProxyCall( const std::string &strCallId, ProxyCallInfo &info );
    void RemoveProxyCall( const std::string &strCallId );
};

extern CModuleDispatcher gclsDispatcher;
extern CSipUserAgent gclsUserAgent;

#endif
