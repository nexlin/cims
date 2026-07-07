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

    bool OnMessage( const char *pszFrom, const char *pszTo, CSipMessage *pclsMessage ) override;
};

#endif
