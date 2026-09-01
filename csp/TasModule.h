#ifndef _TAS_MODULE_H_
#define _TAS_MODULE_H_

#include <string>

#include "IModule.h"

class CspUser;

/**
 * @brief TAS — VoLTE 보조 서비스 모듈 (volte_supplementary_services.md)
 *
 * DND/착신거부/착신전환·당겨받기(피처코드/INVITE-Replaces)·호 전달(REFER blind/attended)·
 * dialog 이벤트 패키지(RFC 4235 BLF 통지)를 소유한다. B2BUA 골격(라우팅·relay 수명)은
 * ModuleDispatcher 가 유지하고, 보조 서비스 판정·재고정은 이 모듈이 수행한다.
 *
 * IModule 훅 (ModuleDispatcher 가 해당 이벤트 시점에 호출):
 *  - OnSipRequest      REFER(호 전달) 게이트 — transfer_allowed=false 403 (§6.3)
 *  - OnIncomingCall    수신 INVITE 의 Replaces(RFC 3891) — BLF 픽업·attended 완결 (§6.2)
 *  - OnCallRing        dialog-event early 통지 + blind transfer 진행 NOTIFY
 *  - OnCallStart       dialog-event confirmed 통지 + blind transfer 완결(재고정·재결합)
 *  - OnCallEnd         dialog-event terminated 통지 + 전달 leg 실패 NOTIFY·trans 정리
 *  - OnTransfer        attended transfer — 원 통화 relay 유지·합류 leg RELAY_MODIFY (§6.2)
 *  - OnBlindTransfer   blind transfer — 지시자 leg index·포트 승계 INVITE (§6.1)
 *
 * 순서 의존 삽입점 (디스패처 라우팅 골격의 정해진 위치에서 호출):
 *  - ScreenInvite               RecvRequest INVITE 조기 스크린 — DND/착신거부·ptt 전용 모드
 *  - TryPickupDial              미등록 착신의 픽업 피처코드 소비 (§5.2)
 *  - ApplyTerminationServices   착신 가입자 DND/착신거부 603·착신전환 302
 */
class CTasModule : public IModule {
public:
    const char *GetName() const override {
        return "TAS";
    }
    bool IsEnabled() const override;

    bool OnSipRequest( int iThreadId, CSipMessage *pclsMessage ) override;
    EModuleRouteResult OnIncomingCall( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                       CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) override;
    bool OnCallRing( const char *pszCallId, int iSipStatus, CSipCallRtp *pclsRtp ) override;
    bool OnCallStart( const char *pszCallId, CSipCallRtp *pclsRtp ) override;
    bool OnCallEnd( const char *pszCallId, int iSipStatus ) override;
    bool OnTransfer( const char *pszCallId, const char *pszReferToCallId, bool bScreened ) override;
    bool OnBlindTransfer( const char *pszCallId, const char *pszReferToId ) override;

    /** INVITE 조기 스크린 (RecvRequest — 다이얼로그 생성 전) — 착신 가입자 DND/착신거부 603,
     *  ptt 전용 서비스 모드 403. true=응답 발신·소비. */
    bool ScreenInvite( CSipMessage *pclsMessage, const char *pszFrom, const char *pszTo );

    /** 픽업 다이얼(피처코드) 판정·수행 — 미등록 착신에서만 호출된다 (§5.2).
     *  true=픽업 다이얼로 소비(응답 완료), false=픽업 다이얼 아님(호출자가 404). */
    bool TryPickupDial( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp );

    /** 착신 가입자 종단 서비스 — DND/착신거부 603, 착신전환 302 (수신 listener 주소로 Contact).
     *  true=응답 발신·소비, false=일반 B2BUA 진행. */
    bool ApplyTerminationServices( const char *pszCallId, const char *pszFrom, const CspUser &clsUser );

private:
    /** 당겨받기 (volte_supplementary_services.md §5). pszTarget: 지정 픽업 대상 내선
     *  (NULL/빈 값 = 그룹 픽업 — 발신자의 픽업 그룹에서 링 중인 아무 호). */
    void PickUp( const char *pszCallId, const char *pszFrom, const char *pszTarget, CSipCallRtp *pclsRtp );

    /** 픽업 다이얼 판정 (§5.2) — 발신 가입자 접속서비스의 pickup_feature_code (필드 미지정 시
     *  전역 Setup.Sip.CallPickupId 폴백, 빈 값=비활성). "<code>"=그룹 픽업(strTarget 빈 값),
     *  "<code><내선>"=지정 픽업(strTarget=내선). */
    bool IsPickupDial( const char *pszFrom, const char *pszTo, std::string &strTarget );

    /** 당겨받기 재고정 코어 — 링잉/대상 leg(strOldCallId)를 pszCallId(신규 단말)로 재키잉하고
     *  relay 를 RELAY_MODIFY 로 재고정한다. PickUp(피처코드)과 INVITE-Replaces(RFC 3891) 공용.
     *  반환 0=성공(양측 200 발신 완료), >0=실패 SIP 코드(호출자가 pszCallId 에 응답). */
    int PickUpLeg( const char *pszCallId, const char *pszFrom, CSipCallRtp *pclsRtp, const std::string &strOldCallId );

    /** 수신 INVITE 의 Replaces(RFC 3891) 처리 — 헤더가 있으면 대상 다이얼로그를 찾아 pszCallId 로
     *  교체(당겨받기/attended 완결)하고 true. 헤더가 없으면 false(정상 호 처리 계속). */
    bool HandleIncomingReplaces( const char *pszCallId, const char *pszFrom, CSipCallRtp *pclsRtp,
                                 CSipMessage *pclsMessage );

    /** dialog-event(RFC 4235) 상태 통지 — 한 호의 두 당사자(caller/callee) 각각을 감시하는 구독자에게
     *  그 당사자의 CSP 측 leg Call-ID 로 partial NOTIFY 를 낸다(당겨받기 BLF, §6.2). */
    void NotifyDialogState( const char *pszCallId, const char *pszState );
};

#endif
