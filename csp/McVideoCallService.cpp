// MCVideo 그룹 호 — 참여·제어 기능 겸임 (TS 24.281 §9.2.1 prearranged · §9.2.2 chat). 설계 정본 mcvideo.md §5.2.1.
//   규격을 읽은 방식(mcvideo.md §5.2.1·§9 에 같이 적는다):
//  - prearranged 새 세션의 개시자 200 OK 는 첫 초대 멤버의 200 OK(또는 첫 합류) 뒤에 보낸다(§9.2.1.4.2 — 미디어
//    버퍼링이 없는 MCVideo 는 확인 없는 200 을 먼저 주지 않는다). 그 뒤 개시자 JOIN ② 에서 다른 참가자가 있으므로
//    암묵 송출 요청이 곧바로 허가된다(TS 24.581 §6.3.2.2).
//  - 참가자 수 해제(§6.3.8.1 2) «only one or no participants»)는 prearranged 에만 건다 — chat 은 참가자가 모이기를
//    기다리는 세션이라 마지막 참가자가 나가면(0 명) 해제한다.
//  - 재합류 대상 = Request-URI 의 gr(MCVideo 세션 식별자, TS 24.281 §4.5·§9.2.1.4.5.1) — 진행 중 세션이 아니면
//    404(Warning 137).
//  - 팬아웃 INVITE 의 Session-Expires 는 refresher 를 싣지 않는다(§6.3.3.1.2 6) «The refresher parameter shall be
//    omitted») — 단말이 200 OK 에서 refresher=uas 로 정한다(§6.2.3.1.1 5)).
//  - CMP 로스터는 붙는 멤버만 싣는다(PTT_GROUP_ADD members = 그 멤버 하나) — CMP 가 로스터 멤버마다 포트 유닛을
//    잡으므로 그룹 전원을 실으면 참가하지 않는 멤버의 유닛까지 점유된다. CMP updateRoster 는 병합이라 누적된다.
#include "McVideoCallService.h"

#include <strings.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include "CallDir.h"
#include "CspAddressing.h"
#include "CspPttGroup.h"
#include "CspServiceMap.h"
#include "CspUser.h"
#include "DbManager.h"
#include "GroupMap.h"
#include "Log.h"
#include "McVideoInfo.h"
#include "McVideoSdp.h"
#include "ModuleDispatcher.h"
#include "RtpMap.h"
#include "SipCodecTable.h"
#include "SipMessageLogger.h"
#include "SipServer.h"
#include "SipUserAgent.h"
#include "UserMap.h"

CMcVideoCallService gclsMcVideoCallService;

extern void SendAffiliationNotify( const std::string &strUserId, const std::string &strPid,
                                   EMcService eService = EMcService::Mcptt );
extern void EmitAffiliationChanged( const std::string &strGroupId, const char *pszAction, const std::string &strUserId,
                                    EMcService eService = EMcService::Mcptt );

namespace {

    const char *const kMcVideoPsiUser =
        "mcvideo_psi";  // 참여 MCVideo 기능 PSI (CSC ue-init-config MCVideo-Service-Details)
    const char *const kMcVideoAudioCodec = "AMR-WB/16000";  // MCVideo 음성 성분 (TS 26.281 — mcvideo.md §1.4 K4)
    const int kMcvStreamCap = 16;  // CMP 가 받는 max_transmitters·max_rx_streams 상한 (1~16, cmp_media_api.md §7.9)

    std::string PttDomain() {
        return gclsServiceMap.GetDomainByKind( "ptt" );
    }

    // 음성 성분 코덱 — 코덱 테이블의 AMR-WB(테이블 첫 엔트리가 다른 코덱이어도), 없으면 서비스 코덱
    const CSipCodecEntry &McvAudioCodec() {
        const CSipCodecEntry *p = CSipCodecTable::FindByRtpmap( kMcVideoAudioCodec );
        return p ? *p : CSipCodecTable::GetTop();
    }

    // SDP 읽기 — 부품은 McVideoSdp.h(단위시험 csp_mcvideo_sdp_test), 여기서는 psip 호 정보(CSipCallRtp)에서 목록만
    //   꺼낸다
    bool McvControlOf( CSipCallRtp *pclsRtp, int &iPort, CMcVideoFmtp &clsFmtp ) {
        iPort = 0;
        clsFmtp = CMcVideoFmtp();
        return pclsRtp && McvControlChannel( pclsRtp->m_clsMediaList, iPort, clsFmtp );
    }
    void McvMediaOf( CSipCallRtp *pclsRtp, const char *pszMedia, const char *pszEncoding, unsigned int &uSsrc,
                     int &iPt ) {
        uSsrc = 0;
        iPt = 0;
        if ( pclsRtp ) McvMediaSsrcPt( pclsRtp->m_clsMediaList, pszMedia, pszEncoding, uSsrc, iPt );
    }
    int McvVideoFbOf( CSipCallRtp *pclsRtp, int iVideoPt ) {
        return pclsRtp ? McvVideoFeedback( pclsRtp->m_clsMediaList, iVideoPt ) : 0;
    }

    // 영상 성분을 뺀 선언 — 서버 SDP 가 m=video 0 이거나 영상 SRTP 협상이 깨졌다(RFC 3264 §6: 그 성분만 거절)
    void McvDropVideo( CmpMcvMemberDecl &d ) {
        d.iVideoPort = 0;
        d.iVideoPt = 0;
        d.uVideoSsrc = 0;
        d.iVideoFb = -1;
        d.clsVideoCrypto = CmpMediaCrypto();
    }

    // 서버가 SDP 에 싣는 SRTP 키 한 m= 라인 (psip — audio 는 m_strLocalCrypto*, video 는 m_strLocalVideoCrypto*)
    void McvApplyLocalCrypto( CSipCallRtp &clsRtp, const RelaySdesLeg &clsSdes, bool bVideo ) {
        const RelaySdesMedia &a = clsSdes.clsAudio;
        if ( a.bSrtp ) {
            clsRtp.m_strLocalCryptoTag = a.strTag.empty() ? "1" : a.strTag;
            clsRtp.m_strLocalCryptoSuite = a.strSuite;
            clsRtp.m_strLocalCryptoKey = a.strSrvKey;
        }
        const RelaySdesMedia &v = clsSdes.clsVideo;
        if ( bVideo && v.bSrtp ) {
            clsRtp.m_strLocalVideoCryptoTag = v.strTag.empty() ? "1" : v.strTag;
            clsRtp.m_strLocalVideoCryptoSuite = v.strSuite;
            clsRtp.m_strLocalVideoCryptoKey = v.strSrvKey;
        }
    }

    // 서버 offer 의 SRTP 한 m= 라인 — 기본 제안 suite(media_security.md §2), 키는 m= 라인마다 따로
    bool McvOfferSrtp( RelaySdesMedia &m ) {
        m.bSrtp = true;
        m.strTag = "1";
        m.strSuite = "AES_CM_128_HMAC_SHA1_80";
        m.strProto = "RTP/SAVP";
        m.strSrvKey = MediaSdes::GenerateInlineKeyB64();
        return !m.strSrvKey.empty();
    }

    bool IsMember( const CspPttGroup &clsGroup, const std::string &strUser, int *piPrio = nullptr,
                   std::string *pstrRole = nullptr ) {
        for ( const auto &p : clsGroup._pusers ) {
            if ( p && ( p->_id == strUser || McpttBareId( p->_mcpttId ) == strUser ) ) {
                if ( piPrio ) *piPrio = (int)p->_priority;
                if ( pstrRole ) *pstrRole = p->_role;
                return true;
            }
        }
        return false;
    }

    std::string HeaderValues( CSipMessage *pclsMessage, const char *pszName ) {
        std::string s;
        for ( const auto &h : pclsMessage->m_clsHeaderList )
            if ( strcasecmp( h.m_strName.c_str(), pszName ) == 0 ) s += h.m_strValue + ",";
        return s;
    }

}  // namespace

std::string CMcVideoCallService::_NewSessionToken() {
    static unsigned int s_uSeq = 0;
    struct timespec ts;
    clock_gettime( CLOCK_REALTIME, &ts );
    return std::to_string( (unsigned long long)ts.tv_sec * 1000000ULL + (unsigned long long)ts.tv_nsec / 1000ULL ) +
           "-" + std::to_string( ++s_uSeq );
}

void CMcVideoCallService::_Reject( const char *pszCallId, int iStatus, int iWarnCode, const char *pszWarnText,
                                   const std::string &strInfoBody ) {
    std::vector<std::pair<std::string, std::string>> vecHdr;
    if ( iWarnCode > 0 && pszWarnText )
        vecHdr.emplace_back( "Warning", McpttWarning( iWarnCode, pszWarnText, PttDomain() ) );
    // Warning = TS 24.281 §4.4 · 본문 = 미인가 우선순위 요청의 mcvideo-info(§6.3.3.1.13)
    gclsUserAgent.StopCall( pszCallId, iStatus, NULL, vecHdr, "application/vnd.3gpp.mcvideo-info+xml", strInfoBody );
}

void CMcVideoCallService::_CloseAttempt( Session &clsSes, bool bEstablished, const char *pszReason,
                                         const char *pszCause, int iStatus ) {
    if ( !clsSes.bAttemptOpen ) return;
    clsSes.bAttemptOpen = false;
    if ( bEstablished ) clsSes.bEstablished = true;
    if ( !gclsCallDir.IsEnabled() ) return;
    gclsCallDir.PttAttempt( clsSes.strGroupId, clsSes.strGroupKey, clsSes.strInitiator,
                            bEstablished ? "established" : "failed", bEstablished ? "" : pszReason,
                            bEstablished ? "" : pszCause, bEstablished ? 0 : iStatus,
                            bEstablished ? clsSes.strSesId : "", "mcvideo" );
}

bool CMcVideoCallService::IsMcVideoCall( const std::string &strCallId ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    return m_mapCallGroup.count( strCallId ) != 0;
}

int CMcVideoCallService::_EstablishedCount( const Session &clsSes ) const {
    int n = 0;
    for ( const auto &kv : clsSes.mapLegs )
        if ( kv.second.bEstablished ) ++n;
    return n;
}

// N6(동시 MCVideo 그룹 호) — 참가 중이거나 스스로 연 leg 가 있는 세션 수. 응답 전 초대 leg 는 아직 호가 아니다.
int CMcVideoCallService::_ActiveCallsOf( const std::string &strMember ) const {
    int n = 0;
    for ( const auto &s : m_mapSession )
        for ( const auto &kv : s.second.mapLegs )
            if ( kv.second.strMember == strMember && ( kv.second.bEstablished || kv.second.eRole != E_LEG_INVITED ) ) {
                ++n;
                break;
            }
    return n;
}

bool CMcVideoCallService::_CmpAddMember( Session &clsSes, const CspPttGroup &clsGroup, const std::string &strMember ) {
    CmpMcvGroupSpec spec;
    spec.strGroupType = clsSes.bPrearranged ? "prearranged" : "chat";
    spec.iMaxTransmitters = std::max( 1, std::min( kMcvStreamCap, clsGroup._mcvideoAttrs.iMaxTransmitters ) );
    spec.bReceptionAutomatic = false;  // 1차 = 일반 호 manual 수신 (TS 24.581 §6.3.6.3.3)
    spec.strCallType = "normal";
    // T1 = 그룹 hang timer(TS 24.581 §11.1.3 — MCPTT on-network-hang-timer 와 같은 요소, 0 = 미사용), T5 = reception
    //   hang timer
    if ( clsGroup._hangTimerSec >= 0 ) spec.iT1Ms = clsGroup._hangTimerSec * 1000;
    if ( clsGroup._mcvideoAttrs.iReceptionHangTimerSec >= 0 )
        spec.iT5Ms = clsGroup._mcvideoAttrs.iReceptionHangTimerSec * 1000;
    int iPrio = 0;
    std::string strRole;
    IsMember( clsGroup, strMember, &iPrio, &strRole );
    spec.strMembers = strMember + ":" + std::to_string( iPrio ) + ":" + ( strRole.empty() ? "participant" : strRole );
    // 녹취 자리 — 같은 값을 매 ADD 에 싣는다(CMP setRecording 은 멱등). 세그먼트 = 송출 구간,
    //   type mcvideo(recording.md §3.3.1)
    spec.strRecordDir = clsSes.strRecordDir;
    spec.strSessionDir = clsSes.strSessionDir;
    std::map<std::string, CmpMcvPorts> mapPorts;
    std::string strIp;
    if ( !gclsCmpClient.McvAddGroup( clsSes.strGroupId, spec, clsSes.strSesId, strIp, mapPorts ) ) return false;
    if ( !strIp.empty() ) clsSes.strCmpIp = strIp;
    return true;
}

std::string CMcVideoCallService::_RecordDescriptor( const CspPttGroup &clsGroup, bool bPrearranged ) {
    // PTT 그룹 디스크립터(CGroupCallService::BuildGroupDescriptor)와 같은 편성·멤버 필드 + MCVideo 몫. MCPTT floor 축
    //   (floor_control·floor_policy·max_talkers)은 싣지 않는다 — MCVideo 는 송출 제어(TS 24.581)라 이력이
    //   반이중·무전으로 읽으면 안 된다. 서비스 축은 session.json type(CallDir)이 가른다.
    std::ostringstream oss;
    oss << "{\"id\":" << clsGroup._dbId << ",\"mcptt_group_id\":\"" << CCallDir::JsonEsc( clsGroup._id ) << "\""
        << ",\"name\":\"" << CCallDir::JsonEsc( clsGroup._name ) << "\"";
    if ( clsGroup._alias.empty() )
        oss << ",\"alias\":null";
    else
        oss << ",\"alias\":\"" << CCallDir::JsonEsc( clsGroup._alias ) << "\"";
    oss << ",\"group_type\":\"" << CCallDir::JsonEsc( clsGroup._groupType ) << "\""
        << ",\"priority\":" << clsGroup._priority << ",\"org_code\":\"" << CCallDir::JsonEsc( clsGroup._orgCode )
        << "\"";
    oss << ",\"mcvideo\":{\"session_type\":\"" << ( bPrearranged ? "prearranged" : "chat" )
        << "\",\"max_transmitters\":"
        << std::max( 1, std::min( kMcvStreamCap, clsGroup._mcvideoAttrs.iMaxTransmitters ) )
        << ",\"reception_mode\":\"manual\"";
    if ( clsGroup._mcvideoAttrs.iMaxDurationSec > 0 )
        oss << ",\"max_duration_sec\":" << clsGroup._mcvideoAttrs.iMaxDurationSec;
    oss << "}";
    int iCount = 0;
    std::ostringstream members;
    for ( const auto &pUser : clsGroup._pusers ) {
        if ( !pUser ) continue;
        members << ( iCount++ ? "," : "" ) << "{\"user_id\":\"" << CCallDir::JsonEsc( pUser->_id ) << "\""
                << ",\"priority\":" << pUser->_priority << ",\"role\":\"" << CCallDir::JsonEsc( pUser->_role ) << "\"";
        if ( pUser->_mcpttId.empty() )
            members << ",\"mcptt_id\":null}";
        else
            members << ",\"mcptt_id\":\"" << CCallDir::JsonEsc( pUser->_mcpttId ) << "\"}";
    }
    oss << ",\"member_count\":" << iCount << ",\"members\":[" << members.str() << "]}";
    return oss.str();
}

void CMcVideoCallService::_RecordSessionStart( Session &clsSes, const CspPttGroup &clsGroup,
                                               const std::string &strCallId ) {
    if ( !gclsCallDir.IsEnabled() ) return;
    // 같은 폴더(D4) — 저장 키 = ptt_groups.id(surrogate). 같은 그룹의 MCPTT 세션과는 세션 디렉터리만
    //   다르다(sesid 가 다르다)
    clsSes.strRecKey = CCallDir::PttSessionKey( "mcvideo", clsSes.strGroupId );
    const std::string strStorage = clsGroup._dbId > 0 ? std::to_string( clsGroup._dbId ) : clsSes.strGroupId;
    clsSes.strRecordDir = gclsCallDir.GetPttSessionDir( clsSes.strRecKey, clsSes.strSesId, strStorage, "mcvideo" );
    gclsCallDir.PttSessionStart( clsSes.strRecKey, strCallId, clsSes.strInitiator,
                                 _RecordDescriptor( clsGroup, clsSes.bPrearranged ) );
    clsSes.strSessionDir = gclsCallDir.GetPttSessionName( clsSes.strRecKey );
}

void CMcVideoCallService::_FillDecl( CmpMcvMemberDecl &d, const std::string &strMember, CSipCallRtp *pclsRtp,
                                     int iRosterPrio, bool bServerOffered ) {
    d.strIp = pclsRtp->m_strIp;
    d.iPort = pclsRtp->GetAudioPort() > 0 ? pclsRtp->GetAudioPort() : pclsRtp->m_iPort;
    int iCtl = 0;
    CMcVideoFmtp clsFmtp;
    McvControlOf( pclsRtp, iCtl, clsFmtp );
    d.iControlPort = iCtl;
    unsigned int uA = 0, uV = 0;
    int iAPt = 0, iVPt = 0;
    McvMediaOf( pclsRtp, "audio", "AMR-WB", uA, iAPt );
    McvMediaOf( pclsRtp, "video", "H264", uV, iVPt );
    // 영상 성분 = m=video 포트가 살아 있고 H.264 PT 가 있을 때만(m=video 0 = 영상 없음, RFC 3264 §6)
    const int iVideoPort = pclsRtp->GetVideoPort();
    d.iVideoPort = ( iVideoPort > 0 && iVPt > 0 ) ? iVideoPort : 0;
    d.uAudioSsrc = uA;
    d.uVideoSsrc = d.iVideoPort > 0 ? uV : 0;
    d.iPt = iAPt;
    // CMP ingress 분류 PT = 단말이 보내는 PT — 서버 offer leg 면 서버 offer 의 PT, 단말 offer leg 면 서버 answer 가
    //   echo 한 offer PT(psip AddSdp 규칙) = user_pt (GroupCallService::GetLegPt 와 같은 규칙)
    d.iSrcPt = bServerOffered ? McvAudioCodec().m_iPt : iAPt;
    d.iVideoPt = d.iVideoPort > 0 ? iVPt : 0;
    if ( d.iVideoPort > 0 ) d.iVideoFb = McvVideoFbOf( pclsRtp, iVPt );
    d.strCodec = kMcVideoAudioCodec;
    d.strUri = McpttIdUri( strMember );
    if ( clsFmtp.bHasTcSsrc ) d.uTcSsrc = clsFmtp.uTcSsrc;
    d.iQueueing = clsFmtp.bPresent ? ( clsFmtp.bQueueing ? 1 : 0 ) : -1;
    // 협상 송출 우선순위 상한 = min(offer mc_priority, <user-priority>)(TS 24.581 §14.3.3)
    if ( clsFmtp.iPriority >= 0 ) d.iMaxPriority = std::min( clsFmtp.iPriority, iRosterPrio );
    // 협상 수신 우선순위 상한 = min(offer mc_reception_priority, <user-reception-priority>)(§14.3.6) — 그룹 문서에 그
    // 요소를
    //   두지 않으므로 offer 값(answer 와 같은 값 — BuildMcVideoAnswerFmtp)
    if ( clsFmtp.iReceptionPriority >= 0 ) d.iMaxReceptionPriority = clsFmtp.iReceptionPriority;
    // NAT — SDP 선언 IP vs 등록 바인딩(received/rport latch), MCPTT leg 과 같은 규칙(ue_nat_traversal.md §5)
    ServiceInfo clsSvc = gclsServiceMap.GetForUser( strMember, "ptt" );
    CUserInfo clsInfo;
    std::string strSigIp, strGuard;
    if ( gclsUserMap.Select( strMember.c_str(), clsInfo ) ) strSigIp = clsInfo.m_strIp;
    if ( CCspServiceMap::EvalMediaNat( clsSvc, d.strIp, strSigIp, strGuard ) ) {
        d.iNat = 1;
        d.strSigIp = strGuard;
    }
    // C9 — 동시 수신 스트림 상한 = user profile MaxSimultaneousVideoStreams (CMP 범위 1~16)
    CspMcVideoProfile clsProf;
    if ( gclsDbManager.SelectMcVideoProfile( strMember, clsProf ) == 1 && clsProf.m_iMaxVideoStreams > 0 )
        d.iMaxRxStreams = std::min( kMcvStreamCap, clsProf.m_iMaxVideoStreams );
}

bool CMcVideoCallService::_AcceptLeg( Session &clsSes, const std::string &strCallId, const std::string &strMember,
                                      CSipCallRtp *pclsOffer, bool bImplicit, int iWarnCode, const char *pszWarnText ) {
    CspPttGroup clsGroup;
    int iPrio = 0;
    std::string strRole;
    if ( !gclsGroupMap.Select( clsSes.strGroupId.c_str(), clsGroup ) ) return false;
    IsMember( clsGroup, strMember, &iPrio, &strRole );
    // 로스터 등록(그룹이 없으면 수립) → JOIN ① — 멤버 전용 포트·tc_ssrc
    if ( !_CmpAddMember( clsSes, clsGroup, strMember ) ) return false;
    CmpMcvJoinResult r1;
    if ( !gclsCmpClient.McvJoin( clsSes.strGroupId, strMember, nullptr, clsSes.strSesId, r1 ) ) return false;
    // JOIN ② — 주소·협상 값. 암묵 요청은 여기서 허가된다(다른 참가자가 있으면 곧바로, TS 24.581 §6.3.2.2).
    int iCtl = 0;
    CMcVideoFmtp clsOffer;
    McvControlOf( pclsOffer, iCtl, clsOffer );
    auto itLeg = clsSes.mapLegs.find( strCallId );
    if ( itLeg == clsSes.mapLegs.end() ) return false;
    Leg &leg = itLeg->second;
    CmpMcvMemberDecl d;
    _FillDecl( d, strMember, pclsOffer, iPrio, false );
    d.strRole = strRole.empty() ? "participant" : strRole;
    d.bImplicit = bImplicit;
    // 미디어 SRTP — offer 때 평가한 m= 라인별 상태(OnIncomingInvite)로 CMP 키 (rx = 단말 키, tx = 서버 키)
    if ( !leg.bVideo ) McvDropVideo( d );
    if ( leg.clsSdes.clsAudio.bSrtp &&
         !MediaSdes::BuildCmpKeys( leg.clsSdes.clsAudio.strSuite, leg.clsSdes.clsAudio.strUeKey,
                                   leg.clsSdes.clsAudio.strSrvKey, d.clsAudioCrypto ) )
        return false;
    if ( d.iVideoPort > 0 && leg.clsSdes.clsVideo.bSrtp &&
         !MediaSdes::BuildCmpKeys( leg.clsSdes.clsVideo.strSuite, leg.clsSdes.clsVideo.strUeKey,
                                   leg.clsSdes.clsVideo.strSrvKey, d.clsVideoCrypto ) )
        McvDropVideo( d );
    CmpMcvJoinResult r2;
    if ( !gclsCmpClient.McvJoin( clsSes.strGroupId, strMember, &d, clsSes.strSesId, r2 ) ) return false;

    // answer (골든 04·06 — TS 24.281 §6.3.3.2.1 · TS 24.581 §14.3): 멤버 CMP 포트, 영상 성분이 없으면 m=video 0,
    //   udp MCVideo + fmtp
    CSipCallRtp clsAns;
    clsAns.SetIpPort( r1.strIp.empty() ? clsSes.strCmpIp.c_str() : r1.strIp.c_str(), r1.clsPorts.iPort,
                      SOCKET_COUNT_PER_MEDIA );
    const CSipCodecEntry &clsCodec = McvAudioCodec();
    clsAns.m_iCodec = clsCodec.m_iPt;
    clsAns.m_clsCodecList.push_back( clsCodec.m_iPt );
    clsAns.m_iVideoPort = ( d.iVideoPort > 0 && r1.clsPorts.iVideoPort > 0 ) ? r1.clsPorts.iVideoPort : -1;
    clsAns.m_eMcMediaProfile = E_MC_MEDIA_MCVIDEO;
    clsAns.m_iApplicationPort = r1.clsPorts.iControlPort;
    clsAns.m_strApplicationFmtp =
        BuildMcVideoAnswerFmtp( clsOffer, iPrio, r1.uTcSsrc, bImplicit, r2.bGranted, r2.uAudioSsrc, r2.uVideoSsrc );
    McvApplyLocalCrypto( clsAns, leg.clsSdes, clsAns.m_iVideoPort > 0 );
    leg.bVideo = clsAns.m_iVideoPort > 0;

    // 제어 기능의 200 OK (TS 24.281 §6.3.3.2.3.2) — Contact = 세션 식별자 + 포커스 태그,
    //   세션 갱신은 단말(refresher=uac), PAI = 참여 MCVideo 기능 PSI(골든 04), Supported = kMcFocusOkSupported(8)~10)).
    //   Require: timer 는 스택이 세션 타이머 협상으로 싣는다. Warning = 122(정원 — 첫 2xx 단계)·123(진행 중 prearranged
    //   세션 합류 — §9.2.1.4.2 14)j)).
    const std::string strDomain = PttDomain();
    if ( !strDomain.empty() ) gclsUserAgent.SetCallDomain( strCallId.c_str(), strDomain.c_str() );
    gclsUserAgent.SetContactParams( strCallId.c_str(), kMcVideoFocusContactParams );
    gclsUserAgent.SetContactUriParams( strCallId.c_str(), ( "gr=" + clsSes.strGr ).c_str() );
    gclsUserAgent.SetSessionRefresher( strCallId.c_str(), E_SESSION_REFRESHER_REMOTE );
    CSipMessage *pclsOk = NULL;
    if ( !gclsUserAgent.AcceptCall( strCallId.c_str(), &clsAns, &pclsOk ) || !pclsOk ) return false;
    pclsOk->AddHeader( "Supported", kMcFocusOkSupported );
    if ( iWarnCode > 0 && pszWarnText )
        pclsOk->AddHeader( "Warning", McpttWarning( iWarnCode, pszWarnText, strDomain ).c_str() );
    pclsOk->AddHeader( "P-Asserted-Identity",
                       ( std::string( "<sip:" ) + kMcVideoPsiUser + "@" + strDomain + ">" ).c_str() );
    if ( !gclsUserAgent.m_clsSipStack.SendSipMessage( pclsOk ) ) return false;

    leg.bEstablished = true;
    leg.bJoined = true;
    leg.uTcSsrc = r1.uTcSsrc;
    // 세션 이력의 참가자 — 개시자는 PttSessionStart 가 남겼다
    if ( leg.eRole != E_LEG_INITIATOR && !clsSes.strRecKey.empty() )
        gclsCallDir.PttMemberJoin( clsSes.strRecKey, strMember, strCallId );
    CLog::Print( LOG_INFO, "MCVIDEO: accept group(%s) member(%s) call(%s) audio=%d video=%d control=%d tc_ssrc=%u%s",
                 clsSes.strGroupId.c_str(), strMember.c_str(), strCallId.c_str(), r1.clsPorts.iPort,
                 clsAns.m_iVideoPort, r1.clsPorts.iControlPort, r1.uTcSsrc,
                 bImplicit ? ( r2.bGranted ? " implicit granted" : " implicit pending" ) : "" );
    return true;
}

void CMcVideoCallService::_ResolvePendingInitiator( const std::string strGroupId ) {
    auto itS = m_mapSession.find( strGroupId );
    if ( itS == m_mapSession.end() || !itS->second.bInitiatorPending ) return;
    Session &clsSes = itS->second;
    clsSes.bInitiatorPending = false;
    const std::string strInit = clsSes.strInitiatorCallId;
    CSipCallRtp *pOffer = clsSes.pclsInitiatorOffer;
    clsSes.pclsInitiatorOffer = nullptr;
    const bool bOk = pOffer && _AcceptLeg( clsSes, strInit, clsSes.strInitiator, pOffer, clsSes.bInitiatorImplicit,
                                           clsSes.bInviteCapped ? 122 : 0, kMcVideoWarn122 );
    delete pOffer;
    if ( bOk ) {
        _CloseAttempt( clsSes, true );  // 개시자 200 OK = 성립(sip_statistics.md §2.1)
        return;
    }
    CLog::Print( LOG_ERROR, "MCVIDEO: group(%s) initiator accept 실패 → 500, 세션 해제", strGroupId.c_str() );
    _CloseAttempt( clsSes, false, "error", "accept_failed", SIP_INTERNAL_SERVER_ERROR );
    gclsUserAgent.StopCall( strInit.c_str(), SIP_INTERNAL_SERVER_ERROR );
    clsSes.mapLegs.erase( strInit );
    m_mapCallGroup.erase( strInit );
    _ReleaseSession( strGroupId, "initiator accept failed" );
}

void CMcVideoCallService::_FailPendingIfNoInvitee( const std::string strGroupId ) {
    auto itS = m_mapSession.find( strGroupId );
    if ( itS == m_mapSession.end() || !itS->second.bInitiatorPending ) return;
    Session &clsSes = itS->second;
    for ( const auto &kv : clsSes.mapLegs )
        if ( kv.second.eRole == E_LEG_INVITED ) return;
    const std::string strInit = clsSes.strInitiatorCallId;
    CLog::Print( LOG_INFO, "MCVIDEO: group(%s) 초대가 모두 실패 → 개시자 480", strGroupId.c_str() );
    _CloseAttempt( clsSes, false, "no_answer", "no_member_answered", SIP_TEMPORARILY_UNAVAILABLE );
    gclsUserAgent.StopCall( strInit.c_str(), SIP_TEMPORARILY_UNAVAILABLE );
    m_mapCallGroup.erase( strInit );
    clsSes.mapLegs.erase( strInit );
    _ReleaseSession( strGroupId, "all invites failed" );
}

bool CMcVideoCallService::_InviteMember( Session &clsSes, const CspPttGroup &clsGroup, const std::string &strMember ) {
    CUserInfo clsInfo;
    if ( !gclsUserMap.Select( strMember.c_str(), clsInfo ) || !clsInfo.m_bMcVideo ) return false;
    int iPrio = 0;
    IsMember( clsGroup, strMember, &iPrio );
    if ( !_CmpAddMember( clsSes, clsGroup, strMember ) ) return false;
    CmpMcvJoinResult r1;
    if ( !gclsCmpClient.McvJoin( clsSes.strGroupId, strMember, nullptr, clsSes.strSesId, r1 ) ) {
        gclsCmpClient.McvLeave( clsSes.strGroupId, strMember, clsSes.strSesId );
        return false;
    }

    // offer (골든 07) — 멤버 CMP 포트 audio·video·control, fmtp = mc_priority(<user-priority>) + mc_transmission_ssrc
    CSipCallRtp clsOffer;
    clsOffer.SetIpPort( r1.strIp.empty() ? clsSes.strCmpIp.c_str() : r1.strIp.c_str(), r1.clsPorts.iPort,
                        SOCKET_COUNT_PER_MEDIA );
    const CSipCodecEntry &clsCodec = McvAudioCodec();
    clsOffer.m_iCodec = clsCodec.m_iPt;
    clsOffer.m_clsCodecList.push_back( clsCodec.m_iPt );
    clsOffer.m_iVideoPort = r1.clsPorts.iVideoPort > 0 ? r1.clsPorts.iVideoPort : -1;
    clsOffer.m_eMcMediaProfile = E_MC_MEDIA_MCVIDEO;
    clsOffer.m_iApplicationPort = r1.clsPorts.iControlPort;
    clsOffer.m_strApplicationFmtp = BuildMcVideoInviteFmtp( iPrio, r1.uTcSsrc );
    // 미디어 SRTP offer (media_security.md §4 표·§4.1) — required = SAVP, optional = 이 바인딩이 등록 때
    //   mediasec(sdes-srtp) 능력을 선언했을 때만. audio·video 는 m= 라인마다 키를 따로 만든다(RFC 4568 §6.1). MCPTT
    //   멤버 초대와 같은 규칙.
    RelaySdesLeg clsSdes;
    {
        const ServiceInfo clsSvc = gclsServiceMap.GetForUser( strMember, "ptt" );
        if ( clsSvc.media_srtp == "required" || ( clsSvc.media_srtp == "optional" && clsInfo.m_bMediaSecSdes ) ) {
            if ( !McvOfferSrtp( clsSdes.clsAudio ) ||
                 ( clsOffer.m_iVideoPort > 0 && !McvOfferSrtp( clsSdes.clsVideo ) ) ) {
                CLog::Print( LOG_ERROR, "MCVIDEO: invite member(%s) SRTP key generation failed", strMember.c_str() );
                gclsCmpClient.McvLeave( clsSes.strGroupId, strMember, clsSes.strSesId );
                return false;
            }
            McvApplyLocalCrypto( clsOffer, clsSdes, clsOffer.m_iVideoPort > 0 );
        }
    }

    CSipCallRoute clsRoute;
    clsInfo.GetCallRoute( clsRoute );
    const std::string strDomain = PttDomain();
    std::string strCallId;
    CSipMessage *pclsInvite = NULL;
    if ( !gclsUserAgent.CreateCall( clsSes.strGroupId.c_str(), strMember.c_str(), &clsOffer, &clsRoute, strCallId,
                                    &pclsInvite, strDomain.empty() ? NULL : strDomain.c_str() ) ||
         !pclsInvite ) {
        gclsCmpClient.McvLeave( clsSes.strGroupId, strMember, clsSes.strSesId );
        return false;
    }
    gclsSipLogger.SetCallSesId( strCallId, clsSes.strSesId );
    // Request-URI = 등록 Contact(RFC 3261 §16.5), 도달 = 바인딩(IP·포트·transport) — MCPTT 팬아웃과 같은 규칙
    if ( !clsInfo.m_strContactUri.empty() )
        pclsInvite->m_clsReqUri.Parse( clsInfo.m_strContactUri.c_str(), (int)clsInfo.m_strContactUri.length() );
    else
        pclsInvite->m_clsReqUri.Set( SIP_PROTOCOL, strMember.c_str(), strDomain.c_str(), 0 );
    pclsInvite->m_clsRouteList.clear();
    pclsInvite->m_strSendDestIp = clsRoute.m_strDestIp;
    pclsInvite->m_iSendDestPort = clsRoute.m_iDestPort;
    pclsInvite->m_eTransport = clsRoute.m_eTransport;
    // TS 24.281 §6.3.3.1.2 — Accept-Contact 둘(require;explicit), P-Asserted-Service = MCVideo ICSI(RFC 6050), 포커스
    //   Contact + gr, Session-Expires 는 refresher 생략(6))
    pclsInvite->AddHeader( "Accept-Contact", "*;+g.3gpp.mcvideo;require;explicit" );
    pclsInvite->AddHeader(
        "Accept-Contact",
        ( std::string( "*;+g.3gpp.icsi-ref=\"" ) + kMcVideoIcsiEnc + "\";require;explicit" ).c_str() );
    pclsInvite->AddHeader( "P-Asserted-Service", kMcVideoIcsi );
    // 참여 기능의 단말 INVITE (§6.3.2.2.3 5)·6)) — Supported: tdialog·norefersub(timer 는 스택이 싣는다).
    //   Answer-Mode (§6.3.2.2.5.2 8)) — 그룹 호 개시자는 Answer-Mode 를 싣지 않고(§10.2.2.2.1 은 개별 호만) 참여 기능이
    //   단말의 poc-settings Answer-Mode Indication 으로 정한다. CIMS 는 poc-settings 를 받지 않아(mcvideo.md §5.2) 자동
    //   개시로 본다 — MCPTT 팬아웃과 같은 값. 단말은 자기 설정에 따라 수동으로 받을 수 있다(§9.2.1.2.1.2 7)·8)).
    pclsInvite->AddHeader( "Supported", kMcMemberInviteSupported );
    pclsInvite->AddHeader( "Answer-Mode", "Auto" );
    // P-Asserted-Identity = 제어 기능 PSI (TS 24.281 §9.2.1.4.1.1 3) — 개시자 200 OK 의 PAI 와 같은 신원, 골든 07).
    //   스택이 From(그룹)으로 먼저 넣은 PAI 는 지운다(RFC 3325 §9.1 — SIP URI 하나)
    McvReplaceHeader( pclsInvite->m_clsHeaderList, "P-Asserted-Identity",
                      std::string( "<sip:" ) + kMcVideoPsiUser + "@" + strDomain + ">" );
    McStripSessionRefresher( pclsInvite->m_clsHeaderList );
    gclsUserAgent.SetContactParams( strCallId.c_str(), kMcVideoFocusContactParams );
    gclsUserAgent.SetContactUriParams( strCallId.c_str(), ( "gr=" + clsSes.strGr ).c_str() );
    {
        CSipFrom clsContact;
        CspAddressing::FillSelfContact( clsContact, clsRoute.m_eTransport, clsSes.strGroupId.c_str() );
        if ( !clsRoute.m_strOutboundLocalIp.empty() ) clsContact.m_clsUri.m_strHost = clsRoute.m_strOutboundLocalIp;
        clsContact.m_clsUri.InsertParam( "gr", clsSes.strGr.c_str() );
        clsContact.HeaderListParamParse( kMcVideoFocusContactParams, (int)strlen( kMcVideoFocusContactParams ) );
        pclsInvite->m_clsContactList.clear();
        pclsInvite->m_clsContactList.push_back( clsContact );
    }
    // 본문 = SDP + mcvideo-info (골든 07 — session-type prearranged · request-uri 멤버 · calling-user-id 개시자 ·
    //   calling-group-id)
    {
        const std::string strInfo =
            McVideoInfoDocument( "    <session-type>prearranged</session-type>\r\n" +
                                 McVideoInfoUri( "mcvideo-request-uri", McpttIdUri( strMember ) ) +
                                 McVideoInfoUri( "mcvideo-calling-user-id", McpttIdUri( clsSes.strInitiator ) ) +
                                 McVideoInfoUri( "mcvideo-calling-group-id", McpttGroupUri( clsSes.strGroupId ) ) );
        const std::string strBoundary = "mcv-fan-" + _NewSessionToken();
        std::ostringstream oss;
        oss << "--" << strBoundary << "\r\nContent-Type: application/sdp\r\n\r\n"
            << pclsInvite->m_strBody << "\r\n--" << strBoundary << "\r\nContent-Type: application/"
            << kMcVideoInfoSubtype << "\r\n\r\n"
            << strInfo << "\r\n--" << strBoundary << "--\r\n";
        pclsInvite->m_strBody = oss.str();
        pclsInvite->m_iContentLength = (int)pclsInvite->m_strBody.size();
        pclsInvite->m_clsContentType.Set( "multipart", "mixed" );
        pclsInvite->m_clsContentType.InsertParam( "boundary", strBoundary.c_str() );
    }
    Leg leg;
    leg.strCallId = strCallId;
    leg.strMember = strMember;
    leg.eRole = E_LEG_INVITED;
    leg.tDeadline = time( NULL ) + kInviteAnswerSec;
    leg.clsSdes = clsSdes;
    leg.uTcSsrc = r1.uTcSsrc;
    clsSes.mapLegs[strCallId] = leg;
    m_mapCallGroup[strCallId] = clsSes.strGroupId;
    if ( !gclsUserAgent.StartCall( strCallId.c_str(), pclsInvite ) ) {
        clsSes.mapLegs.erase( strCallId );
        m_mapCallGroup.erase( strCallId );
        gclsCmpClient.McvLeave( clsSes.strGroupId, strMember, clsSes.strSesId );
        return false;
    }
    CLog::Print( LOG_INFO, "MCVIDEO: invite group(%s) member(%s) call(%s) audio=%d video=%d control=%d",
                 clsSes.strGroupId.c_str(), strMember.c_str(), strCallId.c_str(), r1.clsPorts.iPort,
                 r1.clsPorts.iVideoPort, r1.clsPorts.iControlPort );
    return true;
}

void CMcVideoCallService::OnIncomingInvite( const char *pszCallId, const char *pszFrom, const char *pszTo,
                                            CSipCallRtp *pclsRtp, CSipMessage *pclsMessage ) {
    (void)pszTo;
    const std::string strCallId = pszCallId ? pszCallId : "";
    const std::string strFrom = pszFrom ? pszFrom : "";
    const std::string strCtype =
        pclsMessage->m_clsContentType.m_strType + "/" + pclsMessage->m_clsContentType.m_strSubType;
    const CMcVideoInfo clsMvi =
        ParseMcVideoInfo( McVideoBodyPart( pclsMessage->m_strBody, strCtype, kMcVideoInfoSubtype ) );

    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    // 대상 — Request-URI 의 gr(재합류, §9.2.1.4.5.1) 이면 그 세션, 아니면 <mcvideo-request-uri>
    std::string strGroupId;
    const char *pszGr = SearchSipParameter( pclsMessage->m_clsReqUri.m_clsUriParamList, "gr" );
    const bool bRejoin = pszGr && *pszGr;
    if ( bRejoin ) {
        for ( const auto &kv : m_mapSession )
            if ( kv.second.strGr == pszGr ) strGroupId = kv.first;
    } else {
        strGroupId = McpttBareId( clsMvi.strRequestUri );
    }
    auto itSes = strGroupId.empty() ? m_mapSession.end() : m_mapSession.find( strGroupId );
    // 시도 판정 — 진행 중 세션이 없는 그룹으로의 INVITE 가 개시 시도 1건이다(sip_statistics.md §2.1·§2.3). 진행 중
    //   세션에 붙는 합류·재합류(gr)는 시도가 아니다(참여율이 본다). 세션을 열기 전에 거절하면 그 자리에서 장부에
    //   남기고, 세션을 연 뒤의 결말은 _CloseAttempt 가 남긴다.
    const bool bAttempt = !bRejoin && !strGroupId.empty() && itSes == m_mapSession.end();
    CspPttGroup clsGroup;
    const bool bGroup = !strGroupId.empty() && gclsGroupMap.Select( strGroupId.c_str(), clsGroup );
    const bool bMcvGroup = bGroup && clsGroup._mcvideo;
    const std::string strGroupKey = bGroup && clsGroup._dbId > 0 ? std::to_string( clsGroup._dbId ) : "";
    auto reject = [&]( int iStatus, int iWarnCode, const char *pszWarnText, const char *pszReason, const char *pszCause,
                       const std::string &strInfoBody = std::string() ) {
        if ( bAttempt && gclsCallDir.IsEnabled() )
            gclsCallDir.PttAttempt( strGroupId, strGroupKey, strFrom, "failed", pszReason, pszCause, iStatus, "",
                                    "mcvideo" );
        _Reject( pszCallId, iStatus, iWarnCode, pszWarnText, strInfoBody );
    };
    // 호 방식 — 재합류는 그 세션의 것, 아니면 그룹 속성(mcvideo-on-network-invite-members). 참여 기능은 mcvideo-info
    //   session-type 으로 절차를 고른다(§9.2.1.3.1.1 prearranged · §9.2.2.3.1.1 chat) — 없으면 그룹 속성.
    const bool bPrearranged =
        itSes != m_mapSession.end() ? itSes->second.bPrearranged : bMcvGroup && clsGroup._mcvideoAttrs.bInviteMembers;
    const bool bPrearrangedAsked =
        !clsMvi.strSessionType.empty() ? clsMvi.strSessionType == "prearranged" : bPrearranged;

    // ── 참여 MCVideo 기능 (§9.2.1.3.1.1 · §9.2.2.3.1.1 — 재합류 §9.2.1.3.5.1 도 같은 단계). CSP 가 참여·제어 기능을
    //    겸하므로 참여 기능의 검사(자원 → 이용 자격 → 미디어 → N6)를 먼저 하고 제어 기능의 검사로 넘어간다.
    // 1) 자원 — CMP 가 MCVideo 멤버 풀을 광고하지 않으면 받지 않는다 — 500
    if ( !gclsCmpClient.SupportsMcVideo() ) {
        CLog::Print( LOG_INFO, "MCVIDEO: INVITE from(%s) — CMP resource.mcvideo 없음 → 500", strFrom.c_str() );
        return reject( SIP_INTERNAL_SERVER_ERROR, 0, NULL, "error", "media_unavailable" );
    }
    // 3) 이용 자격 (MCVideo user profile) — 403 108(chat)·109(prearranged)
    CspMcVideoProfile clsProf;
    if ( gclsDbManager.SelectMcVideoProfile( strFrom, clsProf ) != 1 )
        return bPrearrangedAsked ? reject( SIP_FORBIDDEN, 109, kMcVideoWarn109, "denied", "not_entitled" )
                                 : reject( SIP_FORBIDDEN, 108, kMcVideoWarn108, "denied", "not_entitled" );
    // 4) 미디어 — 제어 채널(m=application udp MCVideo)과 음성 AMR-WB 가 있어야 한다 — 488
    int iCtl = 0;
    CMcVideoFmtp clsOffer;
    const bool bCtl = McvControlOf( pclsRtp, iCtl, clsOffer );
    unsigned int uSsrc = 0;
    int iAmrPt = 0;
    McvMediaOf( pclsRtp, "audio", "AMR-WB", uSsrc, iAmrPt );
    if ( !pclsRtp || !bCtl || iCtl <= 0 || iAmrPt <= 0 ) {
        CLog::Print( LOG_INFO, "MCVIDEO: INVITE from(%s) — SDP 에 udp MCVideo/AMR-WB 없음 → 488", strFrom.c_str() );
        return reject( SIP_NOT_ACCEPTABLE_HERE, 0, NULL, "error", "codec_mismatch" );
    }
    // 미디어 SRTP (SDES — media_security.md §4·§5): 접속서비스 정책 × offer crypto, m= 라인마다. 음성은 필수 성분이라
    //   협상이 깨지면 488, 영상은 그 성분만 거절한다(answer m=video 0 — RFC 3264 §6). 서버 키는 m= 라인마다 따로(RFC
    //   4568 §6.1).
    RelaySdesLeg clsSdes;
    bool bVideoOk = true;
    {
        const ServiceInfo clsSvc = gclsServiceMap.GetForUser( strFrom, "ptt" );
        if ( MediaSdes::EvalRelayOfferSdes( clsSvc.media_srtp, pclsRtp->m_clsMediaList, "audio", clsSdes.clsAudio ) <
             0 ) {
            CLog::Print( LOG_INFO, "MCVIDEO: INVITE from(%s) — audio SRTP 협상 불가(policy=%s) → 488", strFrom.c_str(),
                         clsSvc.media_srtp.c_str() );
            return reject( SIP_NOT_ACCEPTABLE_HERE, 0, NULL, "error", "srtp_failed" );
        }
        if ( MediaSdes::EvalRelayOfferSdes( clsSvc.media_srtp, pclsRtp->m_clsMediaList, "video", clsSdes.clsVideo ) <
             0 ) {
            CLog::Print( LOG_INFO, "MCVIDEO: INVITE from(%s) — video SRTP 협상 불가(policy=%s) → 영상 성분 거절",
                         strFrom.c_str(), clsSvc.media_srtp.c_str() );
            clsSdes.clsVideo = RelaySdesMedia();
            bVideoOk = false;
        }
        // 음성이 SRTP 인데 영상이 평문이면 psip 가 영상을 거절한다(평문 영상을 SRTP leg 에 섞지 않는다) — 같은 판단을
        //   여기서 둔다
        if ( clsSdes.clsAudio.bSrtp && !clsSdes.clsVideo.bSrtp ) bVideoOk = false;
    }
    // 5) N6 — 동시 MCVideo 호 상한 (이 세션에 이미 있는 멤버의 재합류는 새 호가 아니다) — 486 103
    bool bInThis = false;
    if ( itSes != m_mapSession.end() )
        for ( const auto &kv : itSes->second.mapLegs )
            if ( kv.second.strMember == strFrom ) bInThis = true;
    if ( !bInThis && _ActiveCallsOf( strFrom ) >= clsProf.m_iMaxCallsN6 )
        return reject( SIP_BUSY_HERE, 103, kMcVideoWarn103, "denied", "max_calls_exceeded" );
    // 6)·7) N2 — chat 은 제휴하지 않은 그룹으로의 개시·합류가 곧 암묵적 제휴다(§9.2.2.3.1.1 6) · §8.2.2.2.12). 이미 N2
    // 개
    //   그룹에 MCVideo 제휴해 있으면 486 102. prearranged 는 일반 호에 암묵적 제휴가 없어(아래 403 120) 해당 없다
    const bool bAffiliated = gclsDbManager.IsAffiliated( strGroupId, strFrom, EMcService::McVideo );
    if ( !bAffiliated && !bPrearranged ) {
        std::vector<CDbManager::CAffiliationRow> vecRows;
        gclsDbManager.SelectActiveAffiliationsByUser( strFrom, vecRows, EMcService::McVideo );
        std::set<std::string> setHeld;
        for ( const auto &r : vecRows ) setHeld.insert( r.strGroupId );
        if ( (int)setHeld.size() >= clsProf.m_iMaxAffiliationsN2 ) {
            CLog::Print( LOG_INFO, "MCVIDEO: INVITE from(%s) group(%s) — 제휴 %d 개 = N2 → 486 102", strFrom.c_str(),
                         strGroupId.c_str(), (int)setHeld.size() );
            return reject( SIP_BUSY_HERE, 102, kMcVideoWarn102, "denied", "max_affiliations_exceeded" );
        }
    }

    // ── 제어 MCVideo 기능 (prearranged §9.2.1.4.2 · chat §9.2.2.4.1.1 · 재합류 §9.2.1.4.5.1)
    // 재합류 2) — gr 이 가리키는 세션이 없으면 404
    if ( bRejoin && strGroupId.empty() ) {
        CLog::Print( LOG_INFO, "MCVIDEO: rejoin gr=%s from(%s) — 진행 중 세션 없음 → 404 137", pszGr, strFrom.c_str() );
        return _Reject( pszCallId, SIP_NOT_FOUND, 137, kMcVideoWarn137 );
    }
    // Accept-Contact 의 g.3gpp.mcvideo·MCVideo icsi-ref, Contact 에 isfocus 가 없어야 한다 — 403
    const std::string strAccept = HeaderValues( pclsMessage, "Accept-Contact" );
    std::string strContactParams;
    if ( !pclsMessage->m_clsContactList.empty() )
        for ( const auto &p : pclsMessage->m_clsContactList.front().m_clsParamList )
            strContactParams += ";" + p.m_strName;
    if ( !McVideoFeatureIn( strAccept ) || !McVideoIcsiIn( strAccept ) ||
         strContactParams.find( ";isfocus" ) != std::string::npos ) {
        CLog::Print( LOG_INFO, "MCVIDEO: INVITE from(%s) — Accept-Contact/isfocus 조건 불일치 → 403", strFrom.c_str() );
        return reject( SIP_FORBIDDEN, 0, NULL, "denied", "invalid_request" );
    }
    int iPrio = 0;
    if ( bRejoin ) {
        // 재합류 6) — 합류 규칙(§6.3.5.3: 목록 entry · join-handling · MCVideo 서비스)을 못 지키면 403 121. 그룹 문서
        //   초기 처리(§6.3.5.2 — 113·116)는 재합류 절차에 없다
        if ( !bMcvGroup || !IsMember( clsGroup, strFrom, &iPrio ) ) {
            CLog::Print( LOG_INFO, "MCVIDEO: rejoin group(%s) from(%s) — 합류 규칙 불충족 → 403 121",
                         strGroupId.c_str(), strFrom.c_str() );
            return _Reject( pszCallId, SIP_FORBIDDEN, 121, kMcVideoWarn121 );
        }
    } else {
        // 그룹 문서 초기 처리 (§6.3.5.2) — 없음 404 113 · 비멤버 403 116 · session-type 불일치 404 117/118
        if ( !bMcvGroup ) {
            CLog::Print( LOG_INFO, "MCVIDEO: INVITE from(%s) group(%s) — MCVideo 그룹 문서 없음 → 404 113",
                         strFrom.c_str(), strGroupId.c_str() );
            return reject( SIP_NOT_FOUND, 113, kMcVideoWarn113, "denied", "group_not_found" );
        }
        // 5)a) 그룹 문서 <on-network-disabled>(콘솔 on-network 끔 — CSC 그룹 문서가 그 요소를 낸다) → 403 115
        if ( !clsGroup._onNetwork ) {
            CLog::Print( LOG_INFO, "MCVIDEO: INVITE from(%s) group(%s) — on-network 꺼짐 → 403 115", strFrom.c_str(),
                         strGroupId.c_str() );
            return reject( SIP_FORBIDDEN, 115, kMcVideoWarn115, "denied", "group_disabled" );
        }
        if ( !IsMember( clsGroup, strFrom, &iPrio ) )
            return reject( SIP_FORBIDDEN, 116, kMcVideoWarn116, "denied", "not_member" );
        if ( !clsMvi.strSessionType.empty() && clsMvi.strSessionType != clsGroup._mcvideoAttrs.SessionType() ) {
            return bPrearranged ? reject( SIP_NOT_FOUND, 117, kMcVideoWarn117, "denied", "session_type_mismatch" )
                                : reject( SIP_NOT_FOUND, 118, kMcVideoWarn118, "denied", "session_type_mismatch" );
        }
    }
    // 미인가 긴급·임박·경보 (prearranged §9.2.1.4.2 10)·11) · chat §9.2.2.4.1.1 6)·7)) — 403 + mcvideo-info. 그룹 문서
    //   초기 처리 뒤·제휴 판정 앞(규격 단계 순서). 긴급 영상 호(V8)가 없어 지시자를 true 로 실은 요청은 늘 미인가다.
    {
        const std::string strPrioReject = McVideoPriorityRejectBody( clsMvi );
        if ( !strPrioReject.empty() ) {
            CLog::Print( LOG_INFO,
                         "MCVIDEO: INVITE from(%s) group(%s) — 긴급·임박·경보 미인가(emergency=%d imminent=%d "
                         "alert=%d) → 403",
                         strFrom.c_str(), strGroupId.c_str(), clsMvi.bEmergency, clsMvi.bImminent, clsMvi.bAlert );
            return reject( SIP_FORBIDDEN, 0, NULL, "denied", "policy_denied", strPrioReject );
        }
    }
    // 제휴 — prearranged(개시·합류·재합류)는 제휴된 사용자만(§9.2.1.4.2 13)a)·14)a) · §9.2.1.4.5.1 8) — 일반 호에
    // 암묵적
    //   affiliation 없음, 403 120). chat 은 멤버면 암묵적 affiliation 적격(§9.2.2.4.1.1 5) · §8.2.2.3.6) — 제휴는 정원
    //   검사를 지난 뒤에 한다(아래, 12)).
    if ( !bAffiliated && bPrearranged ) {
        CLog::Print( LOG_INFO, "MCVIDEO: INVITE from(%s) group(%s) prearranged — 제휴 안 됨 → 403 120", strFrom.c_str(),
                     strGroupId.c_str() );
        return reject( SIP_FORBIDDEN, 120, kMcVideoWarn120, "denied", "not_affiliated" );
    }
    // 정원 — 진행 중 세션이 그룹 문서 <on-network-max-participant-count>(TS 24.481 — 그룹 max_members, 0 = 상한 없음)에
    //   찼으면 486 122(§9.2.1.4.2 14)d) · §9.2.2.4.1.1 11) · §9.2.1.4.5.1 10)). 우선순위로 기존 참가자를 내보내는 선택
    //   (로컬 정책)은 두지 않는다. 이미 이 세션에 있는 멤버의 재합류는 자리를 새로 차지하지 않는다
    if ( itSes != m_mapSession.end() && !bInThis && clsGroup._maxMembers > 0 &&
         _EstablishedCount( itSes->second ) >= clsGroup._maxMembers ) {
        CLog::Print( LOG_INFO, "MCVIDEO: INVITE from(%s) group(%s) — 정원 %d 참 → 486 122", strFrom.c_str(),
                     strGroupId.c_str(), clsGroup._maxMembers );
        return _Reject( pszCallId, SIP_BUSY_HERE, 122, kMcVideoWarn122 );
    }

    // chat 합류의 암묵적 affiliation (§9.2.2.4.1.1 12) · §8.2.2.3.7) — 실패면 403 120
    if ( !bAffiliated ) {
        const std::string strClient = clsMvi.strClientId.empty() ? strFrom : clsMvi.strClientId;
        if ( !gclsDbManager.InsertAffiliation( strGroupId, strFrom, strClient, 0, EMcService::McVideo ) )
            return reject( SIP_FORBIDDEN, 120, kMcVideoWarn120, "denied", "affiliation_failed" );
        EmitAffiliationChanged( strGroupId, "affiliate", strFrom, EMcService::McVideo );
        SendAffiliationNotify( strFrom, "", EMcService::McVideo );
        CLog::Print( LOG_INFO, "MCVIDEO: implicit affiliation group(%s) user(%s)", strGroupId.c_str(),
                     strFrom.c_str() );
    }

    // 세션 — CMP 그룹은 첫 멤버의 로스터 등록(_CmpAddMember)에서 선다
    const bool bNew = ( itSes == m_mapSession.end() );
    if ( bNew ) {
        Session s;
        s.strGroupId = strGroupId;
        s.strSesId = CSipMessageLogger::IssueSesId( strFrom, "csp" );
        s.strGr = _NewSessionToken();
        s.bPrearranged = bPrearranged;
        s.strInitiator = strFrom;
        s.tStart = time( NULL );
        s.iMaxDurationSec = clsGroup._mcvideoAttrs.iMaxDurationSec;
        s.bAttemptOpen = true;
        s.strGroupKey = strGroupKey;
        itSes = m_mapSession.emplace( strGroupId, s ).first;
        _RecordSessionStart( itSes->second, clsGroup, strCallId );
        CLog::Print( LOG_INFO, "MCVIDEO: session start group(%s) type=%s initiator(%s) sesid=%s gr=%s",
                     strGroupId.c_str(), bPrearranged ? "prearranged" : "chat", strFrom.c_str(), s.strSesId.c_str(),
                     s.strGr.c_str() );
    }
    Session &clsSes = itSes->second;
    gclsSipLogger.SetCallSesId( strCallId, clsSes.strSesId );
    // 같은 멤버의 옛 leg(BYE 없는 재합류·응답 전 초대 leg) — SIP 다이얼로그만 끝낸다. CMP 멤버 키가 (group, 멤버)라
    //   LEAVE 하면 새 leg 까지 끊긴다. 대기 중 개시자가 다시 INVITE 하면 대기 leg·offer 를 새것으로 바꾼다.
    bool bReplacePending = false;
    for ( auto it = clsSes.mapLegs.begin(); it != clsSes.mapLegs.end(); ) {
        if ( it->second.strMember == strFrom && it->first != strCallId ) {
            const std::string strOld = it->first;
            const bool bPendingInit = clsSes.bInitiatorPending && strOld == clsSes.strInitiatorCallId;
            m_mapCallGroup.erase( strOld );
            it = clsSes.mapLegs.erase( it );
            gclsUserAgent.StopCall( strOld.c_str(), bPendingInit ? SIP_REQUEST_TERMINATED : 0 );
            if ( bPendingInit ) bReplacePending = true;
            CLog::Print( LOG_INFO, "MCVIDEO: group(%s) member(%s) rejoined — stale leg(%s) closed%s",
                         strGroupId.c_str(), strFrom.c_str(), strOld.c_str(),
                         bPendingInit ? " (pending initiator)" : "" );
        } else {
            ++it;
        }
    }
    Leg leg;
    leg.strCallId = strCallId;
    leg.strMember = strFrom;
    leg.eRole = ( bNew || bReplacePending ) ? E_LEG_INITIATOR : E_LEG_JOINER;
    leg.clsSdes = clsSdes;
    leg.bVideo = bVideoOk;
    clsSes.mapLegs[strCallId] = leg;
    m_mapCallGroup[strCallId] = strGroupId;

    if ( bReplacePending ) {
        delete clsSes.pclsInitiatorOffer;
        clsSes.pclsInitiatorOffer = new CSipCallRtp( *pclsRtp );
        clsSes.strInitiatorCallId = strCallId;
        clsSes.bInitiatorImplicit = clsOffer.bImplicit;
        return;
    }
    if ( bNew && bPrearranged ) {
        // 새 prearranged 세션 — 제휴된 MCVideo 등록 멤버를 초대하고, 개시자 200 OK 는 첫 멤버가 붙은 뒤(§9.2.1.4.2).
        //   정원(<on-network-max-participant-count>)이 있으면 그 수까지만 — 개시자도 참가자라 초대는 정원 − 1 명
        //   (§6.3.5.5 — 필수 멤버 우선은 MCVideo 확인 통화와 함께 1차 범위 밖)
        std::vector<std::string> vecAff;
        gclsDbManager.SelectAffiliatedMembers( strGroupId, vecAff, EMcService::McVideo );
        //   다 초대하지 못하면 개시자 200 OK 에 Warning 122(§9.2.1.4.2 첫 2xx 단계 «more than
        //   <on-network-max-participant-count>» — MCPTT §10.1.1.4.2 와 같은 판정: 초대하지 못한 제휴 멤버가 남았다).
        const int iInviteCap = clsGroup._maxMembers > 0 ? std::max( 0, clsGroup._maxMembers - 1 ) : -1;
        int iInvited = 0;
        for ( const auto &strMember : vecAff ) {
            if ( strMember == strFrom || !IsMember( clsGroup, strMember ) ) continue;
            if ( iInviteCap >= 0 && iInvited >= iInviteCap ) {
                clsSes.bInviteCapped = true;
                break;
            }
            if ( _InviteMember( clsSes, clsGroup, strMember ) ) ++iInvited;
        }
        if ( clsSes.bInviteCapped )
            CLog::Print( LOG_INFO, "MCVIDEO: group(%s) — 정원 %d: 초대 %d 명으로 줄임 (Warning 122)",
                         strGroupId.c_str(), clsGroup._maxMembers, iInvited );
        if ( iInvited == 0 ) {
            CLog::Print( LOG_INFO, "MCVIDEO: group(%s) prearranged — 초대할 제휴·등록 멤버 없음 → 480",
                         strGroupId.c_str() );
            _Reject( pszCallId, SIP_TEMPORARILY_UNAVAILABLE, 0, NULL );
            _CloseAttempt( clsSes, false, "no_answer", "no_member_available", SIP_TEMPORARILY_UNAVAILABLE );
            m_mapCallGroup.erase( strCallId );
            clsSes.mapLegs.erase( strCallId );
            _ReleaseSession( strGroupId, "no invitee" );
            return;
        }
        clsSes.bInitiatorPending = true;
        clsSes.strInitiatorCallId = strCallId;
        clsSes.tInitiatorDeadline = time( NULL ) + kInitiateWaitSec;
        clsSes.pclsInitiatorOffer = new CSipCallRtp( *pclsRtp );
        clsSes.bInitiatorImplicit = clsOffer.bImplicit;
        return;
    }
    // chat · prearranged 합류 — 곧바로 수락. 암묵 요청은 새 prearranged 세션 개시만(TS 24.581 §14.3.5).
    //   진행 중 prearranged 세션에 그룹 URI 로 합류하면 200 OK 에 Warning 123(§9.2.1.4.2 14)j)) — 재합류(세션 식별자,
    //   §9.2.1.4.5.1)·chat(§9.2.2.4.1.1)에는 그 단계가 없다.
    const bool bJoinExisting = leg.eRole == E_LEG_JOINER && clsSes.bPrearranged && !bRejoin;
    if ( !_AcceptLeg( clsSes, strCallId, strFrom, pclsRtp, false, bJoinExisting ? 123 : 0, kMcVideoWarn123 ) ) {
        CLog::Print( LOG_ERROR, "MCVIDEO: group(%s) member(%s) accept 실패 → 500", strGroupId.c_str(),
                     strFrom.c_str() );
        _Reject( pszCallId, SIP_INTERNAL_SERVER_ERROR, 0, NULL );
        if ( leg.eRole == E_LEG_INITIATOR )
            _CloseAttempt( clsSes, false, "error", "accept_failed", SIP_INTERNAL_SERVER_ERROR );
        _DropLeg( strGroupId, strCallId, "accept failed" );
        return;
    }
    // chat 개시자 — 200 OK 를 보냈다 = 성립(MCPTT 장부와 같은 정의, sip_statistics.md §2.1)
    if ( leg.eRole == E_LEG_INITIATOR ) _CloseAttempt( clsSes, true );
    // 개시 대기 중인 prearranged 세션에 멤버가 스스로 붙었다 — 개시자에게 이제 답한다
    _ResolvePendingInitiator( strGroupId );
}

bool CMcVideoCallService::OnCallRinging( const std::string &strCallId ) {
    // 초대 leg 의 18x 는 소비한다(개시자에게 옮기지 않는다 — 개시자는 200 OK 를 기다린다)
    return IsMcVideoCall( strCallId );
}

bool CMcVideoCallService::OnCallStarted( const std::string &strCallId, CSipCallRtp *pclsRtp ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    auto itG = m_mapCallGroup.find( strCallId );
    if ( itG == m_mapCallGroup.end() ) return false;
    const std::string strGroupId = itG->second;
    auto itS = m_mapSession.find( strGroupId );
    if ( itS == m_mapSession.end() ) return true;
    Session &clsSes = itS->second;
    auto itL = clsSes.mapLegs.find( strCallId );
    if ( itL == clsSes.mapLegs.end() || itL->second.eRole != E_LEG_INVITED || itL->second.bJoined ) return true;
    Leg &leg = itL->second;
    // 멤버 200 OK — answer 의 주소·포트·a=ssrc 로 JOIN ②(TS 24.281 §9.2.1.4.1.1 — «interact with the media plane»)
    CspPttGroup clsGroup;
    int iPrio = 0;
    std::string strRole;
    if ( gclsGroupMap.Select( strGroupId.c_str(), clsGroup ) ) IsMember( clsGroup, leg.strMember, &iPrio, &strRole );
    bool bJoined = false;
    if ( pclsRtp ) {
        CmpMcvMemberDecl d;
        _FillDecl( d, leg.strMember, pclsRtp, iPrio, true );
        d.strRole = strRole.empty() ? "participant" : strRole;
        // 미디어 SRTP answer — 서버 offer 가 SAVP 였던 m= 라인은 같은 suite 의 유효 crypto 가 있어야 한다(평문 폴백
        //   금지). 음성이 깨지면 참가시키지 않고(BYE), 영상은 그 성분만 뺀다.
        bool bSrtpOk =
            MediaSdes::EvalRelayAnswerSdes( pclsRtp->m_clsMediaList, "audio", leg.clsSdes.clsAudio, d.clsAudioCrypto );
        if ( d.iVideoPort > 0 && !MediaSdes::EvalRelayAnswerSdes( pclsRtp->m_clsMediaList, "video",
                                                                  leg.clsSdes.clsVideo, d.clsVideoCrypto ) )
            McvDropVideo( d );
        if ( !bSrtpOk )
            CLog::Print( LOG_ERROR, "MCVIDEO: group(%s) member(%s) answer SRTP 불일치", strGroupId.c_str(),
                         leg.strMember.c_str() );
        leg.bVideo = d.iVideoPort > 0;
        CmpMcvJoinResult r2;
        bJoined = bSrtpOk && gclsCmpClient.McvJoin( strGroupId, leg.strMember, &d, clsSes.strSesId, r2 );
    }
    if ( !bJoined ) {
        // 미디어 평면에 붙이지 못한 참가자는 둘 수 없다 — BYE 하고 뺀다
        CLog::Print( LOG_ERROR, "MCVIDEO: group(%s) member(%s) JOIN 실패 → BYE (invited leg %s)", strGroupId.c_str(),
                     leg.strMember.c_str(), strCallId.c_str() );
        gclsUserAgent.StopCall( strCallId.c_str() );
        _DropLeg( strGroupId, strCallId, "join failed" );
        _FailPendingIfNoInvitee( strGroupId );
        return true;
    }
    leg.bJoined = true;
    leg.bEstablished = true;
    leg.tDeadline = 0;
    if ( !clsSes.strRecKey.empty() ) gclsCallDir.PttMemberJoin( clsSes.strRecKey, leg.strMember, strCallId );
    CLog::Print( LOG_INFO, "MCVIDEO: group(%s) member(%s) joined (invited leg %s)", strGroupId.c_str(),
                 leg.strMember.c_str(), strCallId.c_str() );
    // 개시자 대기 중이면 이제 답한다 — 첫 멤버가 붙었으므로 암묵 요청은 JOIN ② 에서 곧바로 허가된다
    _ResolvePendingInitiator( strGroupId );
    return true;
}

bool CMcVideoCallService::OnCallEnded( const std::string &strCallId, int iSipStatus ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    auto itG = m_mapCallGroup.find( strCallId );
    if ( itG == m_mapCallGroup.end() ) return false;
    const std::string strGroupId = itG->second;
    auto itS = m_mapSession.find( strGroupId );
    if ( itS == m_mapSession.end() ) {
        m_mapCallGroup.erase( itG );
        return true;
    }
    Session &clsSes = itS->second;
    // 대기 중 개시자의 CANCEL — 세션 개시 중단(초대 leg 도 끝낸다)
    if ( clsSes.bInitiatorPending && strCallId == clsSes.strInitiatorCallId ) {
        _CloseAttempt( clsSes, false, "canceled", "initiator_canceled" );
        m_mapCallGroup.erase( strCallId );
        clsSes.mapLegs.erase( strCallId );
        _ReleaseSession( strGroupId, "initiator cancelled" );
        return true;
    }
    _DropLeg( strGroupId, strCallId, iSipStatus >= 300 ? "invite failed" : "bye" );
    _FailPendingIfNoInvitee( strGroupId );
    return true;
}

bool CMcVideoCallService::OnReInvite( const std::string &strCallId, CSipCallRtp *pclsRemoteRtp,
                                      CSipCallRtp *pclsLocalRtp, bool bRefresh ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    auto itG = m_mapCallGroup.find( strCallId );
    if ( itG == m_mapCallGroup.end() ) return false;
    const std::string strGroupId = itG->second;
    auto itS = m_mapSession.find( strGroupId );
    if ( itS == m_mapSession.end() || !pclsRemoteRtp ) return true;
    Session &clsSes = itS->second;
    auto itL = clsSes.mapLegs.find( strCallId );
    if ( itL == clsSes.mapLegs.end() || !itL->second.bJoined ) return true;
    Leg &leg = itL->second;
    CspPttGroup clsGroup;
    int iPrio = 0;
    std::string strRole;
    if ( gclsGroupMap.Select( strGroupId.c_str(), clsGroup ) ) IsMember( clsGroup, leg.strMember, &iPrio, &strRole );
    // answer 는 스택이 직전 로컬 선언(멤버 CMP 포트·제어 채널·SRTP 서버 키)으로 낸다. 단 fmtp:MCVideo 는 이 re-offer
    //   에 있던 파라미터만 싣는다(TS 24.581 §14.3.1) — 직전 선언을 되풀이하면 개시 answer 의
    //   mc_implicit_request·mc_granted·mc_*_ssrc 가 갱신 answer 에 남는다(암묵 요청은 새 세션 개시에서만, §14.3.5).
    //   세션 갱신(미디어 무변경)도 같다.
    if ( pclsLocalRtp && pclsLocalRtp->m_iApplicationPort > 0 ) {
        int iCtl = 0;
        CMcVideoFmtp clsReOffer;
        McvControlOf( pclsRemoteRtp, iCtl, clsReOffer );
        pclsLocalRtp->m_strApplicationFmtp = BuildMcVideoAnswerFmtp( clsReOffer, iPrio, leg.uTcSsrc );
    }
    if ( bRefresh ) return true;  // 미디어 무변경 — CMP 주소 등록은 그대로(leg_liveness.md §6.3)
    // 미디어 변경 — CMP 주소 등록을 바꾼다. 단말 offer 라 단말 송신 PT = 서버 answer 가 echo 한 offer
    //   PT(bServerOffered=false). SRTP leg 는 단말 재키잉만 반영하고 서버 키는 유지한다(media_security.md §5.2 — 직전
    //   answer 의 서버 키가 그대로 나간다).
    CmpMcvMemberDecl d;
    _FillDecl( d, leg.strMember, pclsRemoteRtp, iPrio, false );
    d.strRole = strRole.empty() ? "participant" : strRole;
    if ( !leg.bVideo )
        McvDropVideo( d );  // 협상에서 거절한 영상은 re-offer 에 있어도 되살리지 않는다(answer 도 m=video 0)
    MediaSdes::ReadReinviteSdes( pclsRemoteRtp->m_clsMediaList, "audio", 0, leg.clsSdes.clsAudio, d.clsAudioCrypto );
    if ( d.iVideoPort > 0 )
        MediaSdes::ReadReinviteSdes( pclsRemoteRtp->m_clsMediaList, "video", 0, leg.clsSdes.clsVideo,
                                     d.clsVideoCrypto );
    CmpMcvJoinResult r2;
    if ( !gclsCmpClient.McvJoin( strGroupId, leg.strMember, &d, clsSes.strSesId, r2 ) )
        CLog::Print( LOG_ERROR, "MCVIDEO: group(%s) member(%s) re-INVITE JOIN 갱신 실패 (call %s)", strGroupId.c_str(),
                     leg.strMember.c_str(), strCallId.c_str() );
    else
        CLog::Print( LOG_INFO, "MCVIDEO: group(%s) member(%s) re-INVITE — media %s:%d video=%d control=%d",
                     strGroupId.c_str(), leg.strMember.c_str(), d.strIp.c_str(), d.iPort, d.iVideoPort,
                     d.iControlPort );
    return true;
}

void CMcVideoCallService::_DropLeg( const std::string strGroupId, const std::string strCallId, const char *pszWhy ) {
    auto itS = m_mapSession.find( strGroupId );
    m_mapCallGroup.erase( strCallId );
    if ( itS == m_mapSession.end() ) return;
    Session &clsSes = itS->second;
    auto itL = clsSes.mapLegs.find( strCallId );
    if ( itL == clsSes.mapLegs.end() ) return;
    const Leg leg = itL->second;
    clsSes.mapLegs.erase( itL );
    bool bOther = false;  // 같은 멤버의 다른 leg (재합류) — CMP 멤버는 남긴다
    for ( const auto &kv : clsSes.mapLegs )
        if ( kv.second.strMember == leg.strMember ) bOther = true;
    if ( !bOther ) gclsCmpClient.McvLeave( strGroupId, leg.strMember, clsSes.strSesId );
    if ( !bOther && leg.bJoined && !clsSes.strRecKey.empty() )
        gclsCallDir.PttMemberLeave( clsSes.strRecKey, leg.strMember );
    CLog::Print( LOG_INFO, "MCVIDEO: group(%s) member(%s) left (%s) — remaining %d", strGroupId.c_str(),
                 leg.strMember.c_str(), pszWhy, _EstablishedCount( clsSes ) );
    // 해제 정책(§6.3.8.1 2)) — prearranged 는 참가자 1명 이하, chat 은 0 명(파일 머리말). 개시 대기 중은 개시 쪽 판정.
    if ( clsSes.bInitiatorPending ) return;
    const int n = _EstablishedCount( clsSes );
    if ( ( clsSes.bPrearranged && n <= 1 ) || n == 0 ) _ReleaseSession( strGroupId, "participants" );
}

void CMcVideoCallService::_ReleaseSession( const std::string strGroupId, const char *pszWhy ) {
    // 값으로 받는다 — 호출자가 세션 안의 문자열을 넘겨도 아래에서 세션을 지운 뒤 쓸 수 있게
    auto itS = m_mapSession.find( strGroupId );
    if ( itS == m_mapSession.end() ) return;
    // 결말을 안 남긴 개시 시도 — 위 경로들이 사유를 남기므로 여기 오면 기록이 빠진 새 경로다. 사유 없이 남겨
    //   집계의 «사유 모름» 으로 드러나게 한다(sip_statistics.md §2.3)
    _CloseAttempt( itS->second, false, "", "", itS->second.bInitiatorPending ? SIP_TEMPORARILY_UNAVAILABLE : 0 );
    Session clsSes = itS->second;
    m_mapSession.erase( itS );
    // 남은 leg — 확립 = BYE, 응답 전 초대 = CANCEL, 대기 중 개시자 = 480 (StopCall 은 EventCallEnd 를 부르지 않는다)
    for ( const auto &kv : clsSes.mapLegs ) {
        m_mapCallGroup.erase( kv.first );
        gclsUserAgent.StopCall( kv.first.c_str(), clsSes.bInitiatorPending && kv.first == clsSes.strInitiatorCallId
                                                      ? SIP_TEMPORARILY_UNAVAILABLE
                                                      : 0 );
    }
    delete clsSes.pclsInitiatorOffer;
    gclsCmpClient.McvRemove( strGroupId, clsSes.strSesId );
    // 녹취 세션 끝 — 성립하지 못한 세션(개시자 200 OK 전에 해제 — 초대 무응답·수락 실패·개시자 취소)은 setup_failed:
    //   세션 디렉터리는 CMP 녹취 자리라 개시 때 서지만 통화는 없었다 — 집계가 세션으로 세지 않는다(sip_statistics.md
    //   §3). 성립한 세션의 해제(참가자·T1·TNG3)는 normal, CMP 회수는 error(OnCmpEvent)
    if ( !clsSes.strRecKey.empty() )
        gclsCallDir.PttSessionEnd( clsSes.strRecKey, clsSes.bEstablished ? "normal" : "setup_failed" );
    CLog::Print( LOG_INFO, "MCVIDEO: session end group(%s) (%s) legs=%d", strGroupId.c_str(), pszWhy,
                 (int)clsSes.mapLegs.size() );
}

void CMcVideoCallService::OnCmpEvent( const std::string &strCmd, const std::string &strGroupId,
                                      const std::string &strSesId, const SimpleJson::JsonNode &payload ) {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    auto itS = m_mapSession.find( strGroupId );
    const bool bThis = itS != m_mapSession.end() && ( strSesId.empty() || itS->second.strSesId == strSesId );
    if ( strCmd == "TRANSMISSION_INACTIVITY" ) {
        const std::string strTimer = payload.GetString( "timer" );
        CLog::Print( LOG_INFO, "MCVIDEO: group(%s) %s expired%s", strGroupId.c_str(), strTimer.c_str(),
                     bThis ? "" : " (지난 세션)" );
        // prearranged 의 T1(Inactivity) — 해제(§6.3.8.1 1)). chat 은 참가자가 끝낸다.
        if ( bThis && strTimer == "T1" && itS->second.bPrearranged ) _ReleaseSession( strGroupId, "T1 inactivity" );
    } else if ( strCmd == "TRANSMITTERS" ) {
        SimpleJson::JsonNode arr = payload.Get( "transmitters" );
        CLog::Print( LOG_INFO, "MCVIDEO: group(%s) transmitters=%d", strGroupId.c_str(),
                     arr.type == SimpleJson::JSON_ARRAY ? (int)arr.Size() : 0 );
    } else if ( strCmd == "PTT_GROUP_ABORTED" ) {
        // CMP 가 그룹을 회수했다(주소 등록 멤버 0 + 무활동) — 미디어 평면이 없으니 남은 leg 를 끝내고 캐시를 지운다.
        //   CMP 그룹은 이미 없으므로 REMOVE 는 보내지 않는다.
        if ( bThis ) {
            if ( itS->second.bInitiatorPending )
                _CloseAttempt( itS->second, false, "error", "media_aborted", SIP_TEMPORARILY_UNAVAILABLE );
            Session clsSes = itS->second;
            m_mapSession.erase( itS );
            for ( const auto &kv : clsSes.mapLegs ) {
                m_mapCallGroup.erase( kv.first );
                gclsUserAgent.StopCall( kv.first.c_str(),
                                        clsSes.bInitiatorPending && kv.first == clsSes.strInitiatorCallId
                                            ? SIP_TEMPORARILY_UNAVAILABLE
                                            : 0 );
            }
            delete clsSes.pclsInitiatorOffer;
            if ( !clsSes.strRecKey.empty() )
                gclsCallDir.PttSessionEnd( clsSes.strRecKey, clsSes.bEstablished ? "error" : "setup_failed" );
            CLog::Print( LOG_INFO, "MCVIDEO: group(%s) aborted by CMP — session cache cleared", strGroupId.c_str() );
        }
    }
}

void CMcVideoCallService::Tick() {
    std::lock_guard<std::recursive_mutex> lock( m_mutex );
    const time_t tNow = time( NULL );
    // 초대 응답 한도 — 응답 없는 초대 leg 는 CANCEL 하고 뺀다
    std::vector<std::pair<std::string, std::string>> vecExpired;  // (group, call)
    for ( const auto &kv : m_mapSession )
        for ( const auto &l : kv.second.mapLegs )
            if ( l.second.eRole == E_LEG_INVITED && !l.second.bEstablished && l.second.tDeadline &&
                 tNow >= l.second.tDeadline )
                vecExpired.emplace_back( kv.first, l.first );
    for ( const auto &e : vecExpired ) {
        CLog::Print( LOG_INFO, "MCVIDEO: group(%s) invite(%s) — %d s 안에 응답 없음 → CANCEL", e.first.c_str(),
                     e.second.c_str(), kInviteAnswerSec );
        gclsUserAgent.StopCall( e.second.c_str() );
        _DropLeg( e.first, e.second, "invite timeout" );
        _FailPendingIfNoInvitee( e.first );
    }
    // 개시 대기 한도 · TNG3
    std::vector<std::pair<std::string, const char *>> vecRelease;
    for ( auto &kv : m_mapSession ) {
        Session &s = kv.second;
        if ( s.bInitiatorPending && s.tInitiatorDeadline && tNow >= s.tInitiatorDeadline ) {
            _CloseAttempt( s, false, "no_answer", "answer_timeout", SIP_TEMPORARILY_UNAVAILABLE );
            vecRelease.emplace_back( kv.first, "no invited member answered in time" );
        } else if ( s.iMaxDurationSec > 0 && tNow - s.tStart >= s.iMaxDurationSec )
            vecRelease.emplace_back( kv.first, "TNG3 max duration" );  // §6.3.8.1 5)
    }
    for ( const auto &r : vecRelease ) _ReleaseSession( r.first, r.second );
}
