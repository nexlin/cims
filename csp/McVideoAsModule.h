#ifndef _MCVIDEO_AS_MODULE_H_
#define _MCVIDEO_AS_MODULE_H_

#include "IModule.h"

/**
 * @brief MCVideo-AS — MCVideo 참여·제어 기능 겸임 (TS 24.281, docs/design/features/mcvideo.md §5.2)
 *
 * MCVideo 는 MCPTT 의 확장이 아니라 나란한 MC 서비스다(TS 23.280 §5.2.5) — 같은 메서드(INVITE·PUBLISH·SUBSCRIBE)·같은
 * 그룹 id·같은 MC service ID 를 쓰므로 요청을 **서비스 표시**로 가른다(IsMcVideoRequest). 역할
 * `Setup.Roles.MCVIDEO`(기본 off)가 꺼져 있으면 MCVideo 요청은 참여 MCVideo 기능 PSI 미할당 404(§6.3.7.1) — MCPTT 로
 * 읽지 않는다.
 *
 * 제휴(PUBLISH·SUBSCRIBE Event: presence, TS 24.281 §8.2.2)는 CSCF 경로가 서비스 인자로 처리한다(mcvideo_affiliations).
 * 이 모듈은 MCVideo 호(INVITE)를 받는다 — ModuleDispatcher::EventIncomingCall 이 MCPTT 판정보다 먼저 부른다.
 */
class CMcVideoAsModule : public IModule {
public:
    const char *GetName() const override {
        return "MCVIDEO-AS";
    }
    bool IsEnabled() const override;

    EModuleRouteResult OnIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                       CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) override;

    /** MCVideo 요청인가 — P-Asserted-Service·P-Preferred-Service·Accept-Contact 의 MCVideo ICSI(원 표기·퍼센트 표기),
     * Accept 의 mcvideo 문서 형식, mcvideo-info 본문, pidf 의 mcvideoPresInfo 네임스페이스(McVideoRequestIndicated —
     * TS 24.281 §8.2.2.2.3 3)·§9.2.2.4.1.1 2)). 표시가 하나도 없으면 MCPTT(기존 단말). */
    static bool IsMcVideoRequest( CSipMessage *pclsMessage );
};

#endif
