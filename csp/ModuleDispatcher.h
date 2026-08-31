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
    bool SendTimeout( int iThreadId, CSipMessage *pclsMessage ) override;

    // ISipUserAgentCallBack
    void EventRegister( CSipServerInfo *pclsInfo, int iStatus ) override;
    bool EventIncomingRequestAuth( CSipMessage *pclsMessage ) override;
    void EventIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp,
                            CSipMessage *pclsMessage = NULL ) override;
    void EventCallRing( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRtp ) override;
    void EventCallStart( const char *pszCallId, CSipCallRtp *pclsRtp ) override;
    void EventCallEnd( const char *pszCallId, int iSipStatus ) override;
    void EventReInvite( const char *pszCallId, CSipCallRtp *pclsRemoteRtp, CSipCallRtp *pclsLocalRtp ) override;
    /** 서버가 전달한 re-INVITE 의 최종 응답 — relay SRTP leg 의 재-answer 재키잉을 CMP 에 반영
     *  (media_security.md §5.2). 주소/PT 갱신은 기존 EventReInvite→MODIFY 경로가 담당. */
    void EventReInviteResponse( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRemoteRtp ) override;
    /** 서버 발신 in-dialog 요청(세션 갱신·만료 BYE)의 현재 도달 주소 — 등록 바인딩(latch)을
     *  돌려준다. fan-out INVITE·NOTIFY 가 쓰는 것과 같은 (IP, 포트, transport) 한 세트다. */
    bool EventGetLegDest( const char *pszCallId, const char *pszPeerId, std::string &strIp, int &iPort,
                          ESipTransport &eTransport ) override;
    void EventPrack( const char *pszCallId, CSipCallRtp *pclsRtp ) override;
    bool EventTransfer( const char *pszCallId, const char *pszReferToCallId, bool bScreenedTransfer ) override;
    bool EventBlindTransfer( const char *pszCallId, const char *pszReferToId ) override;
    bool EventMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage ) override;

    // ISipStackSecurityCallBack
    bool IsAllowUserAgent( const char *pszSipUserAgent ) override;
    bool IsDenyUserAgent( const char *pszSipUserAgent ) override;
    bool IsAllowIp( const char *pszIp ) override;
    bool IsDenyIp( const char *pszIp ) override;

    // Proxy 포워딩 (CSCF 모드)
    bool ProxyInvite( CSipMessage *pclsMessage, const char *pszDestIp, int iDestPort, ESipTransport eTransport );

private:
    void PickUp( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp );

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
