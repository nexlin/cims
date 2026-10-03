#ifndef _MCDATA_AS_MODULE_H_
#define _MCDATA_AS_MODULE_H_

#include "IModule.h"

/**
 * @brief MCData-AS — SDS·FD 메시징 participating/controlling function (TS 24.282 §9.2.2 · §10.2.4)
 *
 * MCData 요청 MESSAGE 의 대상을 본문으로 정하고(Request-URI = 참여 기능 PSI — request-type 이 그룹이면
 * <mcdata-request-uri>, 1:1 이면 resource-lists), 그룹 요청은 게이트(allow-SDS·발신자 멤버십·max-data-size)한 뒤
 * affiliation 정책에 따라 멤버에게 fan-out 한다. participating/controlling 통합 배치.
 */
class CMcDataAsModule : public IModule {
public:
    const char *GetName() const override {
        return "MCDATA-AS";
    }
    bool IsEnabled() const override;

    bool OnMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage, int &iStatus ) override;

    /** MCData 요청 MESSAGE (TS 24.282 §9.2.2.3.1 4) · §9.2.2.4.2 5)·6) · §10.2.4.4.2 10)·12)). 처리했으면(그룹 배포·
     *  거절) true 와 보낼 응답 코드(0 = 여기서 보냈다). false 면 디스패처 1:1 경로가 전달한다 — 1:1 요청
     *  (one-to-one-sds·one-to-one-fd)은 strOneToOneTarget 에 resource-lists 의 MCData ID 를 채운다(비면 Request-URI
     *  대상 그대로 — MCData 가 아닌 MESSAGE). */
    bool OnMcDataMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage, int &iStatus,
                          std::string &strOneToOneTarget );

    /** MCData 긴급 경보·경보 취소 MESSAGE (TS 24.282 §16.2.3) — mcdata-info 파트(strInfo)에 <alert-ind> 가 있는 요청.
     *  디스패처가 MCPTT 경보(mcptt-info)와 가른 뒤 부른다. 돌려주는 값 = 보낼 최종 응답 코드(0 = 여기서 보냈다). */
    int OnEmergencyAlert( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage,
                          const std::string &strInfo );

private:
    /** SDS disposition 통지 (TS 24.282 §12.2.2.1·§12.2.3) — signalling 이 SDS NOTIFICATION 이면 참여·제어 기능으로
     *  처리하고 true: Accept-Contact SDS·FD icsi-ref(2) — 없으면 403), 대상 = resource-lists 의 entry 하나(3) — 없거나
     *  둘이면 403 145), 원 SDS 와 상관(4)·5) — 216) 뒤 원 발신자에게 새 MESSAGE 로 중계. */
    bool OnDispositionNotification( const char *pszFrom, CSipMessage *pclsMessage, int &iStatus );
};

#endif
