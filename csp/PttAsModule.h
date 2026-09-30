#ifndef _PTT_AS_MODULE_H_
#define _PTT_AS_MODULE_H_

#include <string>

#include "IModule.h"
#include "McpttInfo.h"

class CSipMessage;

class CPttAsModule : public IModule {
public:
    const char *GetName() const override {
        return "PTT-AS";
    }
    bool IsEnabled() const override;

    EModuleRouteResult OnIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                       CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) override;
    bool OnCallStart( const char *pszCallId, CSipCallRtp *pclsRtp ) override;
    bool OnCallEnd( const char *pszCallId, int iSipStatus ) override;

    /**
     * MCPTT 긴급 경보·경보 취소·그룹 긴급 상태 해제 MESSAGE (TS 24.379 §12.1) — 참여 기능(§12.1.2.1)과 제어 기능
     * (§12.1.3.1·§12.1.3.2·§12.1.3.3)을 겸한다. 대상 그룹 = 본문 <mcptt-request-uri>(Request-URI = 참여 기능 PSI),
     * Request-URI 가 그룹이면 그 그룹(전환기). 인가(발령 §6.3.3.1.13.1·경보 취소 §6.3.3.1.13.3·긴급 해제
     * §6.3.3.1.13.4)가 없으면 403 + 현재 상태 지시자. 제휴 멤버마다 §6.3.3.1.11 형식의 MESSAGE 를 새로 만들어
     * 보낸다(발신자 제외).
     * @return 수신 MESSAGE 에 줄 최종 응답 코드 (0 = 본문이 필요한 거절이라 여기서 보냈다)
     */
    int OnEmergencyAlert( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage, const CMcpttInfo &clsMi );

private:
    /** 경보·경보 취소를 제휴 멤버(발신자 제외)에게 §6.3.3.1.11 형식 MESSAGE 로 새로 만들어 보낸다. 보낸 수. */
    int FanoutAlert( const std::string &strFrom, const std::string &strGroupId, const class CspPttGroup &clsGroup,
                     CSipMessage *pclsMessage, bool bActivate, const std::string &strOriginatedBy );
};

#endif
