#ifndef _MCDATA_AS_MODULE_H_
#define _MCDATA_AS_MODULE_H_

#include "IModule.h"

/**
 * @brief MCData-AS — 그룹 SDS 메시징 controlling function (TS 24.282 §9.2.2)
 *
 * 그룹 대상 SIP MESSAGE 를 게이트(allow-SDS·발신자 멤버십·max-data-size)하고
 * affiliation 정책에 따라 멤버에게 fan-out 한다. participating/controlling 통합 배치.
 */
class CMcDataAsModule : public IModule {
public:
    const char *GetName() const override {
        return "MCDATA-AS";
    }
    bool IsEnabled() const override;

    bool OnMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage, int &iStatus ) override;

    /** MCData 긴급 경보·경보 취소 MESSAGE (TS 24.282 §16.2.3) — mcdata-info 파트(strInfo)에 <alert-ind> 가 있는 요청.
     *  디스패처가 MCPTT 경보(mcptt-info)와 가른 뒤 부른다. 돌려주는 값 = 보낼 최종 응답 코드(0 = 여기서 보냈다). */
    int OnEmergencyAlert( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage,
                          const std::string &strInfo );

private:
    /** SDS disposition 통지의 규격 경로 (TS 24.282 §12.2.2.1·§12.2.3) — 본문에 resource-lists(대상 MCData ID 하나)와
     *  SDS NOTIFICATION 이 있으면 참여·제어 기능으로 처리해 원 발신자에게 새 MESSAGE 로 중계하고 true. 대상을
     *  Request-URI 에 싣는 옛 형식(resource-lists 없음)은 false — 디스패처 1:1 경로가 그대로 전달한다(전환기). */
    bool OnDispositionNotification( const char *pszFrom, CSipMessage *pclsMessage, int &iStatus );
};

#endif
