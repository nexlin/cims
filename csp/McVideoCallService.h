#ifndef _MCVIDEO_CALL_SERVICE_H_
#define _MCVIDEO_CALL_SERVICE_H_

#include <ctime>
#include <map>
#include <mutex>
#include <string>

#include "CmpClient.h"

class CSipCallRtp;
class CSipMessage;
class CspPttGroup;

/**
 * @brief MCVideo 그룹 호 — 참여·제어 기능 겸임 (TS 24.281 §9.2.1 prearranged · §9.2.2 chat,
 * docs/design/features/mcvideo.md §5.2.1)
 *
 * MCPTT CGroupCallService 와 따로 선다 — 세션 캐시·CMP 명령이 서비스 키 (mcvideo, group_id) 라 같은 그룹 id 의 MCPTT
 * 그룹 호와 동시에 선다(§7 D6). leg 는 CallMap 밖에서 이 서비스가 수명을 관리한다(MCData media plane 과 같은 방식) —
 * ModuleDispatcher 가 EventCallStart·EventCallEnd·EventCallRing·EventReInvite 맨 앞에서 먼저 묻는다.
 *
 * psip 규약(이 서비스가 기대는 것): StopCall 은 EventCallEnd 를 부르지 않는다(로컬 종료) · EventCallStart 는 나가는
 * INVITE 의 2xx 에서만 온다(AcceptCall 한 들어오는 leg 는 오지 않는다).
 *
 * 1차 범위: 일반 호(normal)의 chat·prearranged 개시·합류·재합류·이탈·해제 + 미디어 SRTP·녹취(recording.md §3.3)·
 * 시도 장부(sip_statistics.md §2.3). 긴급·임박·방송·ad hoc·private·확인 통화·conference NOTIFY 는 뒤(§5.2.1
 * «1차 범위 밖»).
 */
class CMcVideoCallService {
public:
    /** MCVideo INVITE(역할 on) — 검사·세션·answer·팬아웃까지. 거절도 여기서 끝낸다. */
    void OnIncomingInvite( const char *pszCallId, const char *pszFrom, const char *pszTo, CSipCallRtp *pclsRtp,
                           CSipMessage *pclsMessage );
    /** 이 서비스의 leg 인가 */
    bool IsMcVideoCall( const std::string &strCallId );
    /** 18x — 이 서비스의 초대 leg 면 소비(true) */
    bool OnCallRinging( const std::string &strCallId );
    /** 200 OK(초대 leg 확립) — 이 서비스의 leg 면 처리하고 true */
    bool OnCallStarted( const std::string &strCallId, CSipCallRtp *pclsRtp );
    /** 호 종료(상대 BYE·CANCEL·최종 실패·타임아웃) — 이 서비스의 leg 면 정리하고 true */
    bool OnCallEnded( const std::string &strCallId, int iSipStatus );
    /** re-INVITE — 이 서비스의 leg 면 answer 의 fmtp:MCVideo 를 re-offer 로 다시 짓고(pclsLocalRtp — TS 24.581
     * §14.3.1), 미디어가 바뀌었으면(bRefresh false — 망 전환·주소 변경) CMP 주소를 다시 등록한 뒤 true */
    bool OnReInvite( const std::string &strCallId, CSipCallRtp *pclsRemoteRtp, CSipCallRtp *pclsLocalRtp,
                     bool bRefresh );
    /** CMP 이벤트 (hdr.service mcvideo — PTT_GROUP_ABORTED · TRANSMITTERS · TRANSMISSION_INACTIVITY) */
    void OnCmpEvent( const std::string &strCmd, const std::string &strGroupId, const std::string &strSesId,
                     const SimpleJson::JsonNode &payload );
    /** 1 s — 개시 대기 한도 · 초대 응답 한도 · TNG3(최대 통화 시간) */
    void Tick();

    /** 개시 대기 한도(초) — prearranged 새 세션에서 첫 멤버가 붙기를 기다리는 시간(CIMS 값, §5.2.1). */
    static const int kInitiateWaitSec = 10;
    /** 초대 응답 한도(초) — 초대 leg 가 이 안에 200 OK 를 주지 않으면 CANCEL(CIMS 값 — 규격은 정하지 않는다). */
    static const int kInviteAnswerSec = 30;

private:
    enum ELegRole { E_LEG_INITIATOR, E_LEG_JOINER, E_LEG_INVITED };
    struct Leg {
        std::string strCallId;
        std::string strMember;  // 가입자 id (MCVideo ID = MCPTT ID)
        ELegRole eRole = E_LEG_JOINER;
        bool bEstablished = false;  // 200 OK 송수신
        bool bJoined = false;       // CMP JOIN ② (주소 등록)
        // 협상된 영상 성분이 살아 있다(서버 SDP m=video ≠ 0) — false 면 JOIN 에 video 포트를 싣지 않는다
        bool bVideo = true;
        time_t tDeadline = 0;  // 초대 leg 응답 한도
        // 미디어 SRTP(SDES) 협상 상태 — audio·video 가 m= 라인마다 키가 다르다(media_security.md §5)
        RelaySdesLeg clsSdes;
        // CMP 가 이 멤버에게 준 tc_ssrc — answer mc_transmission_ssrc (re-INVITE answer 를 다시 지을 때)
        unsigned int uTcSsrc = 0;
    };
    struct Session {
        std::string strGroupId;
        std::string strSesId;  // CMP·로그 상관
        std::string strGr;     // MCVideo 세션 식별자 토큰(TS 24.281 §4.5 — 포커스 Contact 의 gr)
        bool bPrearranged = false;
        std::string strInitiator;
        time_t tStart = 0;
        int iMaxDurationSec = 0;  // TNG3 (0 = 무제한)
        std::string strCmpIp;
        std::map<std::string, Leg> mapLegs;  // Call-ID → leg
        // prearranged 개시자 200 OK 대기 — 첫 멤버가 붙은 뒤에 답한다(§9.2.1.4.2)
        bool bInitiatorPending = false;
        std::string strInitiatorCallId;
        time_t tInitiatorDeadline = 0;
        CSipCallRtp *pclsInitiatorOffer = nullptr;  // 소유 — 대기 중 개시자 offer 사본
        bool bInitiatorImplicit = false;            // 개시자 offer 의 mc_implicit_request (TS 24.581 §14.2.5)
        // 녹취(recording.md §3.3 — 같은 폴더 recordings/ptt/{id}, session.json type mcvideo). 비면 녹취 없음
        std::string strRecKey;      // CallDir 세션 키 (PttSessionKey("mcvideo", 그룹))
        std::string strRecordDir;   // 그룹 base — PTT_GROUP_ADD record_dir
        std::string strSessionDir;  // 세션 디렉터리 이름 S{ts}_{n} — PTT_GROUP_ADD session_dir
        // 시도 장부(sip_statistics.md §2.3 — 서비스 mcvideo). 세션을 연 INVITE 가 개시 시도 1건이고, 결말(개시자 200 OK
        //   = 성립 · 개시 실패)을 한 번만 남긴다. bAttemptOpen = 결말이 아직 안 남았다
        bool bAttemptOpen = false;
        bool bEstablished = false;  // 개시자에게 200 OK 를 보냈다 — 아니면 끝날 때 end_reason setup_failed
        std::string strGroupKey;    // 장부 group_key = ptt_groups.id (surrogate)
    };

    /** 녹취 세션 시작 — CallDir 세션 디렉터리·session.json(그룹 디스크립터 + MCVideo 속성, type mcvideo)을
     * 세우고 record_dir·session_dir 를 세션에 둔다(CMP 는 PTT_GROUP_ADD 로 받는다). CallDir 가 꺼져 있으면
     * 아무것도 하지 않는다. */
    void _RecordSessionStart( Session &clsSes, const CspPttGroup &clsGroup, const std::string &strCallId );
    /** session.json 의 디스크립터 — PTT 그룹 디스크립터와 같은 편성·멤버 필드 + MCVideo 몫(호 방식·동시 송출
     * 상한·수신 모드). MCPTT floor 축은 싣지 않는다 */
    static std::string _RecordDescriptor( const CspPttGroup &clsGroup, bool bPrearranged );
    /** 검사 실패 응답 (Warning 은 비면 싣지 않는다) · strInfoBody = 응답에 실을 mcvideo-info 문서(비면 본문 없음) */
    void _Reject( const char *pszCallId, int iStatus, int iWarnCode, const char *pszWarnText,
                  const std::string &strInfoBody = std::string() );
    /** 개시 시도의 결말을 시도 장부에 한 번 남긴다 — 성립(개시자 200 OK) 또는 실패(pszReason·pszCause·iStatus 는
     * sip_statistics.md §2.3 어휘). 이미 남겼으면(bAttemptOpen false) 아무것도 하지 않는다. 호출자가 m_mutex 보유. */
    void _CloseAttempt( Session &clsSes, bool bEstablished, const char *pszReason = "", const char *pszCause = "",
                        int iStatus = 0 );
    /** 개시자·합류자 수락 — 로스터 등록 → JOIN ①·② → 200 OK(answer). 성공이면 true. 호출자가 m_mutex 보유. */
    bool _AcceptLeg( Session &clsSes, const std::string &strCallId, const std::string &strMember,
                     CSipCallRtp *pclsOffer, bool bImplicit );
    /** 대기 중 개시자에게 답한다(첫 멤버가 붙었다) — 실패면 세션 해제. 호출자가 m_mutex 보유. */
    void _ResolvePendingInitiator( const std::string strGroupId );
    /** 대기 중 개시자 — 남은 초대 leg 가 없으면 480 으로 개시를 끝낸다(§9.2.1.4.2). 호출자가 m_mutex 보유. */
    void _FailPendingIfNoInvitee( const std::string strGroupId );
    /** 멤버 팬아웃 INVITE (골든 07) — 호출자가 m_mutex 보유 */
    bool _InviteMember( Session &clsSes, const CspPttGroup &clsGroup, const std::string &strMember );
    /** 세션 해제 — 남은 leg 에 BYE/CANCEL, CMP REMOVE (§6.3.8.1). 호출자가 m_mutex 보유. */
    void _ReleaseSession( const std::string strGroupId, const char *pszWhy );
    /** leg 하나를 세션에서 뺀다(CMP LEAVE) — 해제 정책에 걸리면 세션 해제. 호출자가 m_mutex 보유. */
    void _DropLeg( const std::string strGroupId, const std::string strCallId, const char *pszWhy );
    int _EstablishedCount( const Session &clsSes ) const;
    int _ActiveCallsOf( const std::string &strMember ) const;
    /** CMP 그룹 수립·로스터 추가 (PTT_GROUP_ADD service:mcvideo — members = 이번에 붙는 사람만. CMP 는 로스터 멤버마다
     * 포트 유닛을 잡으므로 그룹 전원을 싣지 않는다) — 호출자가 m_mutex 보유 */
    bool _CmpAddMember( Session &clsSes, const CspPttGroup &clsGroup, const std::string &strMember );
    /** 멤버 선언 (JOIN ②) — 단말 SDP(offer 또는 answer)의 주소·포트·PT·a=ssrc·fmtp. bServerOffered = 서버가 offer 한
     * leg (단말 송신 PT = 서버 offer 의 코덱 테이블 PT, RFC 3264 §5.1) */
    static void _FillDecl( CmpMcvMemberDecl &d, const std::string &strMember, CSipCallRtp *pclsRtp, int iRosterPrio,
                           bool bServerOffered );
    static std::string _NewSessionToken();

    std::recursive_mutex m_mutex;
    std::map<std::string, Session> m_mapSession;        // group id → 세션
    std::map<std::string, std::string> m_mapCallGroup;  // Call-ID → group id
};

extern CMcVideoCallService gclsMcVideoCallService;

#endif
