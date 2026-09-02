#ifndef _TAS_MODULE_H_
#define _TAS_MODULE_H_

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "IModule.h"
#include "MediaSdes.h"
#include "SipUserAgentCallBack.h"  // CSipCallRtp (포크 집합의 B-leg 공통 offer 보관)

class CspUser;
class CspDispatchGroup;

/**
 * @brief 대표번호 병렬 호출 포크 집합 (dispatch_center.md §4.4) — A-leg 하나에 대기 B-leg N 개.
 *
 * 대기 leg 는 승자 확정 전까지 CCallMap 밖(TAS 소유)에 있다 — CCallMap 은 leg 쌍(1:1) 모델이라 포크 중에는
 * peer 가 정해지지 않는다. 승자 확정 시 (A, 승자) 쌍을 CCallMap 에 넣고 이후는 기존 1:1 경로가 이어받는다.
 * 패자 leg 는 CANCEL 후 최종 응답(487)이 올 때까지 m_mapForkLeg 에 남아 이벤트를 흡수한다.
 */
struct CTasForkSet {
    std::string strACallId;                          ///< 발신(A) leg Call-ID
    std::string strCaller;                           ///< 발신자 id
    std::string strGroupId;                          ///< 관제 그룹 id
    std::string strPilot;                            ///< 대표번호
    std::string strDomain;                           ///< 대표번호 도메인 (P-Called-Party-ID)
    std::set<std::string> setPending;                ///< 대기 B-leg Call-ID
    std::map<std::string, std::string> mapLegUser;   ///< B-leg → 그룹원 id
    std::map<std::string, RelaySdesLeg> mapLegSdes;  ///< B-leg 별 서버 offer SDES 상태 (leg 전용 키)
    std::vector<std::string> vecAlerted;             ///< 호출한 그룹원(call.json alerted[])
    bool bRelay = false;
    std::string strRelaySessionId, strRelaySesId, strRelayLocalIp, strMediaNode;
    int iPortA = -1;           ///< A 에게 광고하는 relay 포트(peer0)
    int iPortB = -1;           ///< 대기 leg 전원에게 광고하는 relay 포트(peer1 — 승자만 MODIFY 로 고정)
    RelaySdesLeg clsSdesA;     ///< A leg SDES 협상 상태
    CSipCallRtp clsBaseOffer;  ///< B-leg 공통 offer(crypto strip·relay 주소) — overflow 재시도 원본
    bool bRang = false;        ///< A 에게 180 을 전달했는가(첫 180 만)
    bool bBusySeen = false;    ///< 대기 leg 중 486 이 있었는가(전원 실패 시 486 우세)
    time_t tStart = 0;
    int iNoAnswerSec = 30;
    int iDepth = 0;            ///< overflow 재귀 깊이(1단계까지)
    std::string strOverflow;   ///< 남은 overflow_target (소진 시 빈 값)
    std::string strSessionId;  ///< CallDir 세션 id
};

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

    /** 대표번호(pilot) 해석·병렬 호출 (dispatch_center.md §4) — 미등록 착신에서 TryPickupDial 앞에 호출된다.
     *  pszTo 가 관제 그룹 대표번호면 등록 그룹원 전원에게 포크하고 true(소비). 아니면 false. */
    bool TryDispatchPilot( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp,
                           CSipMessage *pclsMessage );

    /** 1초 주기 — 포크 집합 무응답(no_answer_sec) 판정 → overflow 또는 480 (§4.4). */
    void Tick();

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

    // ── 대표번호 병렬 호출 (dispatch_center.md §4) ──
    /** 그룹원 → 포크 대상 결정: 등록·(busy_members=skip) 비통화·발신자 제외, alert_order 순, MaxForkTargets 절삭. */
    void ResolveForkTargets( const CspDispatchGroup &clsGroup, const std::string &strCaller,
                             std::vector<std::string> &vecTargets );
    /** 대기 leg 생성 — 대상 각각에 leg 전용 SDES offer 로 INVITE(P-Called-Party-ID=대표번호). 생성 수 반환.
     *  m_mutexFork 를 잡은 상태에서 호출한다. */
    int ForkAlert( CTasForkSet &clsSet, const std::vector<std::string> &vecTargets );
    /** 전원 실패/무응답/취소 — 대기 leg CANCEL, A 에게 iSipCode(0=응답 없음: A 가 취소한 경우), relay 회수, 집합 제거.
     */
    void FailFork( const std::string &strACallId, int iSipCode );
    /** 무응답 → overflow_target 으로 재시도(1단계). 대상이 없으면 FailFork(480). */
    void OverflowFork( const std::string &strACallId );
    /** 대표번호 AoR 감시자에게 dialog 이벤트(§4.5) — 대기/승자 leg Call-ID 로 통지. */
    void NotifyPilotDialog( const CTasForkSet &clsSet, const std::string &strLegCallId, const char *pszState,
                            const std::string &strRemote );
    /** 가입자가 확립/진행 중 다이얼로그를 갖는가 (busy_members=skip 판정). */
    static bool IsUserBusy( const std::string &strUserId );
    bool OnForkRing( const char *pszCallId, int iSipStatus );
    bool OnForkStart( const char *pszCallId, CSipCallRtp *pclsRtp );
    bool OnForkEnd( const char *pszCallId, int iSipStatus );

    std::map<std::string, CTasForkSet> m_mapFork;         ///< A-leg Call-ID → 포크 집합
    std::map<std::string, std::string> m_mapForkLeg;      ///< B-leg Call-ID → A-leg Call-ID
    std::map<std::string, std::string> m_mapPilotOfCall;  ///< 확립 후 A/B leg → 대표번호 (종료 통지용)
    std::recursive_mutex m_mutexFork;
};

#endif
