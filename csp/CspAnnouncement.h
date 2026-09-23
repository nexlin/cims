#ifndef _CSP_ANNOUNCEMENT_H_
#define _CSP_ANNOUNCEMENT_H_

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "CmpClient.h"
#include "MediaSdes.h"
#include "RelayCodec.h"
#include "SipUserAgent.h"

class CCallInfo;
class CSipMessage;

/**
 * @ingroup CspServer
 * @brief 안내음성·신호음·보류 음악 — CSP 쪽(MRFC 역할) 정책·상태 머신 (docs/design/features/announcements.md §5).
 *
 *   CMP(MRFP)의 재생기(RELAY_PLAY, cmp_media_api.md §6.7)에 "언제·어느 leg·무엇"을 지시한다.
 *   - 실패 안내(§3.1·§3.2): B-leg 실패 또는 CSP 자체 거절 → 발신 leg 에 183+SDP(CSP 가 만든 answer, a=sendrecv +
 *     P-Early-Media: sendonly) → RELAY_PLAY → 재생 끝(RELAY_PLAY_DONE / 상한) → **원래 최종 코드 그대로**.
 *   - 보류 음악(§3.3): re-INVITE offer 의 sendonly/inactive → 피보류 leg 에 loop 재생, resume·종료·leg 교체에 정지.
 *   - 서버 링백(§3.4): 프로파일 ringback.mode=media 일 때만 — B 의 첫 18x(SDP 없음)에 183+SDP + loop 재생.
 *   정책(§6) = Setup.Announcement.Rules 프로파일 표. 해석 순서 가입자(예약) → 접속서비스/피어 프로파일 →
 * DefaultProfile. CMP 가 resource.ann 을 광고하지 않거나 명령이 거절되면 안내 없이 즉시 원코드(폴백 — 안내가 실패를
 * 가리지 않는다).
 *
 *   상태는 서비스 자체 맵이 갖는다(발신 leg Call-ID 키) — CallMap 은 relay 서술자만. 통계 정의(end_status/end_reason)는
 * 불변.
 */
enum EAnnSituation {
    ANN_SIT_NONE = 0,
    ANN_SIT_RINGBACK,
    ANN_SIT_BUSY,
    ANN_SIT_NO_ANSWER,
    ANN_SIT_UNREACHABLE,
    ANN_SIT_NOT_FOUND,
    ANN_SIT_INVALID,
    ANN_SIT_DECLINED,
    ANN_SIT_CONGESTION,
    ANN_SIT_FORBIDDEN,
    ANN_SIT_HOLD,
    ANN_SIT_CALL_WAITING,
    ANN_SIT_FORWARDED,  // 착신전환 실행 — 발신자에게 전환 안내(TS 24.604, §3.5). 최종 코드 없음(링백처럼 B 의 answer 로
                        // 끝난다)
    ANN_SIT_CALL_WAITING_ALERT,  // 통화중대기 — 통화 중인 착신자의 활성 leg 에 섞는 in-band 대기음(TS 24.615, mode=mix,
                                 // §3.6)
};

/** 프로파일 표의 한 행 — 상황 하나의 동작 */
struct CAnnAction {
    std::string strMode = "none";  // none | tone | announce | tone_then_announce | announce_then_tone | media
    std::string strTone;           // 신호음 id (sys:busy_kr …)
    int iToneMs = 4000;
    std::string strMedia;  // 안내/음원 id
    int iRepeat = 1;
    bool bLoop = false;  // media 모드 STOP 까지 (hold·ringback)

    bool IsNone() const {
        return strMode.empty() || strMode == "none";
    }
    std::string Label() const;  // "tone_then_announce sys:busy_kr/4000 sys:ann_busy"
};

class CCspAnnouncementService {
public:
    CCspAnnouncementService();

    /** Setup.Announcement 해석 — 기동·SIGUSR1 재로드에서 부른다. 내장 기본 표 위에 Rules 행을 (profile, situation) 키로
     * 덮어쓴다(§6.1). */
    void Init();
    bool IsEnabled() const;

    // ── 판정·해석 (순수 — 단위시험 대상) ──
    static const char *SituationName( EAnnSituation e );
    static EAnnSituation SituationOf( const std::string &strName );
    /** SIP 최종 코드 + Reason(RFC 3326 "Q.850;cause=N") → 상황. cause 가 있으면 우선(§2). */
    static EAnnSituation Classify( int iSipStatus, const char *pszReason );
    /** 프로파일 해석 — strProfile(접속서비스/피어) → DefaultProfile → none */
    CAnnAction Resolve( EAnnSituation eSit, const std::string &strProfile ) const;
    /** 발신자 프로파일 이름 — 피어(inbound Route)면 RemoteNode.announcement_profile, 가입자면 접속서비스
     * announcement_profile */
    static std::string ProfileForCaller( const std::string &strCaller, CSipMessage *pclsMessage );
    /** 피보류자 프로파일 — 접속서비스 hold_profile → announcement_profile */
    static std::string HoldProfileFor( const std::string &strUser );

    // ── 훅 (ModuleDispatcher / TAS 가 부른다) ──
    /** B-leg 최종 실패(재라우팅 소진 뒤) → 미응답 A 에 안내. true = 인수(호출자는 A 를 StopCall 하지 않고 B 만 지운다).
     */
    bool OnLegFailed( const char *pszBCallId, const CCallInfo &clsB, int iSipStatus, const char *pszReason );
    /** B leg 이전 자체 거절(§3.2) — relay 를 A 만으로 잡고 안내 뒤 원코드. true = 인수(응답은 이 서비스가 낸다). */
    bool Reject( const char *pszCallId, CSipCallRtp *pclsRtp, const char *pszFrom, const char *pszTo, int iSipStatus,
                 const char *pszReason, CSipMessage *pclsMessage );
    /** 보류 — pszHolderCallId 의 offer 방향이 sendonly/inactive. 피보류 leg 에 음악. 반환 true = 재생
     * 시작(inactive→sendonly 재작성 근거) */
    bool OnHold( const char *pszHolderCallId, const CCallInfo &clsHolder );
    void OnResume( const char *pszHolderCallId, const CCallInfo &clsHolder );
    /** 서버 링백(§3.4) — B 의 SDP 없는 18x. 프로파일 ringback 이 media 일 때만 재생. 반환 true = A 에 이미 SDP 를
     * 냈다(18x 를 SDP 와 함께 전달) */
    /** eSit = ANN_SIT_RINGBACK(일반) 또는 ANN_SIT_CALL_WAITING(착신자가 통화 중 — TS 24.615, 발신자에게 통화중대기
     * 안내/링백). 가입자 링백(§6.3): ringback 상황이고 피착신 가입자의 `ringback_media` 가 있으면 프로파일 mode 가 none
     * 이 아닐 때 그 음원으로 바꾼다 */
    bool OnRingback( const char *pszBCallId, const CCallInfo &clsB, CSipCallRtp **ppclsAnswerForA,
                     EAnnSituation eSit = ANN_SIT_RINGBACK );
    /** 착신전환 안내(§3.5) — 디스패처가 전환 대상으로 B-leg 를 만든 뒤(StartCall 직전) 부른다. 프로파일 `forwarded` 가
     * none 이 아니면 A 에 183+SDP(CSP answer) + 안내 재생. `announce_then_tone` 이면 안내가 끝난 뒤 신호음(링백)을 B 의
     * answer/early media 까지 loop, 그 밖의 모드는 안내가 끝나면 `ringback` 규칙(media 면 loop)으로 이어 간다. B 의 SDP
     * 있는 18x·200 은 OnRingbackEnd 가, B 실패는 OnLegFailed 가(같은 SDP 위에서 실패 안내로 교체), CANCEL 은 OnCallEnd
     * 가 걷는다. 반환 true = A 에 SDP 를 냈다 */
    bool OnForwarded( const char *pszACallId, const char *pszBCallId );
    /** 통화중대기 in-band 대기음(§3.6, TS 24.615) — 통화 중인 착신자(strCallee)의 활성 통화 leg 에 mode=mix 로 신호음을
     * 섞는다. 프로파일 = 착신자 접속서비스 hold_profile → announcement_profile → 기본, 상황 call_waiting_alert(내장
     * default 는 none — 단말 Alert-Info 가 1차; 단말이 못 내는 배치가 `cw_inband` 프로파일로 켠다). 대기
     * 호(pszCwACallId/pszCwBCallId)가 끝나거나 응답되면 OnCallWaitingEnd 가 걷는다. 반환 true = 재생 시작 */
    bool OnCallWaitingAlert( const std::string &strCallee, const char *pszCwACallId, const char *pszCwBCallId );
    /** 대기 호의 어느 leg 든 종료·응답 — 그 대기음 정지(없으면 무해) */
    void OnCallWaitingEnd( const char *pszCallId );
    /** 모니터(MC_SIP_STATS 뒤) — ann_started / ann_fallback / ann_active */
    void GetString( class CMonitorString &strBuf ) const;
    /** B 의 SDP 있는 18x·200 — 링백 정지 */
    void OnRingbackEnd( const char *pszBCallId, const CCallInfo &clsB );
    /** 어느 leg 든 종료(CANCEL·BYE·실패) — 그 호의 재생을 걷는다(early 대기 중이면 최종 응답도 이 종료가 대신한다) */
    void OnCallEnd( const char *pszCallId );
    /** leg 교체(전달·픽업 재키잉) — 그 relay 세션의 재생 정지 */
    void OnLegReplaced( const std::string &strRelaySessionId );
    /** CMP RELAY_PLAY_DONE (CmpClient EventDispatchLoop 스레드) */
    void OnPlayDone( const std::string &strSessionId, int iPeerIdx, const std::string &strPlayId,
                     const std::string &strReason, int iPlayedMs );
    /** 1초 틱 — 상한 지난 early 대기 회수 */
    void Tick();

    // 관측
    long GetStarted() const {
        return m_lStarted;
    }
    long GetFallback() const {
        return m_lFallback;
    }

private:
    struct CAnnCall {
        std::string strACallId;  // 안내를 듣는 leg(발신자)
        std::string strRelaySessionId;
        std::string strSesId;
        std::string strService = "volte";
        int iPeerIdx = 0;  // relay 안의 A leg
        std::string strPlayId;
        EAnnSituation eSit = ANN_SIT_NONE;
        std::string strMedia;  // 로그·CDR 라벨
        int iFinalStatus = 0;  // 0 = 최종 응답 대기 없음(링백)
        std::string strFinalReason;
        time_t tStart = 0;
        int iMaxMs = 0;
        bool bEarlySent = false;  // CSP 가 만든 183 answer 를 냈다
        std::string strCaller, strCallee;
        std::string strProfile;   // 발신자 프로파일 — 2 단계(전환 안내 뒤 링백) 해석
        std::string strNextTone;  // announce_then_tone 의 2 단계 신호음 id(비면 ringback 규칙)
        bool bPhase2 = false;     // 전환 안내 1 단계가 끝나 2 단계 RELAY_PLAY 를 보내는 중 — 그 사이 B 가 응답하면
                                  // OnRingbackEnd 가 이 표식을 지워 늦게 붙은 재생을 곧바로 걷게 한다
    };
    struct CHoldPlay {
        std::string strRelaySessionId;
        std::string strSesId;
        std::string strService = "volte";
        int iPeerIdx = 0;  // 피보류 leg
        std::string strPlayId;
        std::string strHeldCallId;
    };

    static std::string NewPlayId( const char *pszCallId );
    /** A 의 offer 로 CSP answer 를 만든다(§3.1) — 코덱 = A 오퍼 중 CMP 가 재생할 수 있는 첫 코덱, 오디오만, SDES
     * 재광고, relay 주소, a=sendrecv. 반환 false = 만들 수 없음(오퍼 없음·코덱 없음). */
    static bool BuildEarlyAnswer( const char *pszACallId, const RelaySdesLeg &clsSdesA,
                                  const RelayCodec::LegCodecs &clsCodecsA, const std::string &strRelayIp,
                                  int iRelayPort, CSipCallRtp &clsAns, RelayCodec::CodecDesc &clsChosen, int &iTePt );
    bool StartFailurePlay( CAnnCall &clsCall, const CAnnAction &clsAction, const CSipCallRtp *pclsAnsForCodec,
                           const RelayCodec::CodecDesc &clsCodec, int iTePt, bool bEarlyAlready );
    void FinishEarly( const std::string &strACallId, CAnnCall &clsCall, const char *pszResult, int iPlayedMs );
    std::vector<CCmpClient::AnnItem> ItemsOf( const CAnnAction &clsAction, int &iRepeat, int &iMaxMs ) const;

    mutable std::mutex m_mtx;
    // 프로파일 표: profile → situation → action
    std::map<std::string, std::map<int, CAnnAction>> m_mapRules;
    bool m_bEnabled = false;
    int m_iMaxPlayMs = 30000;
    std::string m_strDefaultProfile = "default";

    std::map<std::string, CAnnCall> m_mapCalls;          // A Call-ID → early 안내 상태
    std::map<std::string, std::string> m_mapPlayToCall;  // play_id → A Call-ID
    std::map<std::string, CHoldPlay> m_mapHold;          // holder Call-ID → 보류 재생
    std::map<std::string, CHoldPlay> m_mapCw;            // 대기 호 A·B Call-ID(두 키) → 착신자 활성 leg 의 mix 대기음
    long m_lStarted = 0;
    long m_lFallback = 0;
};

extern CCspAnnouncementService gclsAnnouncement;

#endif
