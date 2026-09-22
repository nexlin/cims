#include "CspAnnouncement.h"

#include <atomic>
#include <cstring>

#include "CallDir.h"
#include "CallMap.h"
#include "CmpClient.h"
#include "CspAddressing.h"
#include "CspLocalNodeMap.h"
#include "CspRemoteNodeMap.h"
#include "CspRouteMap.h"
#include "CspServiceMap.h"
#include "CspUser.h"
#include "GroupCallService.h"
#include "Log.h"
#include "MediaSdes.h"
#include "ModuleDispatcher.h"
#include "MonitorString.h"
#include "RelayCodec.h"
#include "RtpMap.h"
#include "SimpleJson.h"
#include "SipMessageLogger.h"
#include "SipServerSetup.h"
#include "SipStatusCode.h"
#include "UserMap.h"

CCspAnnouncementService gclsAnnouncement;

extern CSipServerSetup gclsSetup;
extern CCallMap gclsCallMap;
extern CUserMap gclsUserMap;
extern CspUserMap gclsCspUserMap;
extern CCspServiceMap gclsServiceMap;
extern CCspRouteMap gclsRouteMap;
extern CCspRemoteNodeMap gclsRemoteNodeMap;
extern CCspLocalNodeMap gclsLocalNodeMap;
extern CSipMessageLogger gclsSipLogger;
extern CCallDir gclsCallDir;

// ──────────────────────────────────────────────────────────────
//  정책 표
// ──────────────────────────────────────────────────────────────

std::string CAnnAction::Label() const {
    std::string s = strMode;
    if ( !strTone.empty() ) s += " " + strTone + "/" + std::to_string( iToneMs );
    if ( !strMedia.empty() ) s += " " + strMedia + ( bLoop ? "(loop)" : "x" + std::to_string( iRepeat ) );
    return s;
}

static const struct {
    EAnnSituation e;
    const char *name;
} kSituations[] = {
    { ANN_SIT_RINGBACK, "ringback" },
    { ANN_SIT_BUSY, "busy" },
    { ANN_SIT_NO_ANSWER, "no_answer" },
    { ANN_SIT_UNREACHABLE, "unreachable" },
    { ANN_SIT_NOT_FOUND, "not_found" },
    { ANN_SIT_INVALID, "invalid" },
    { ANN_SIT_DECLINED, "declined" },
    { ANN_SIT_CONGESTION, "congestion" },
    { ANN_SIT_FORBIDDEN, "forbidden" },
    { ANN_SIT_HOLD, "hold" },
    { ANN_SIT_CALL_WAITING, "call_waiting" },
    { ANN_SIT_FORWARDED, "forwarded" },
    { ANN_SIT_CALL_WAITING_ALERT, "call_waiting_alert" },
};

const char *CCspAnnouncementService::SituationName( EAnnSituation e ) {
    for ( const auto &s : kSituations )
        if ( s.e == e ) return s.name;
    return "none";
}

EAnnSituation CCspAnnouncementService::SituationOf( const std::string &strName ) {
    for ( const auto &s : kSituations )
        if ( strName == s.name ) return s.e;
    return ANN_SIT_NONE;
}

// 내장 기본 표 (announcements.md §6.1 `default`·`trunk`·`ringback`·`cw_inband` 프로파일) — 항상 깔리고, 운영자 Rules 가
// (profile, situation) 키로 덮어쓴다
static const char *kDefaultRules =
    "[{\"profile\":\"default\",\"situation\":\"ringback\",\"mode\":\"none\"},"
    "{\"profile\":\"default\",\"situation\":\"busy\",\"mode\":\"tone_then_announce\",\"tone\":\"sys:busy_kr\",\"tone_"
    "ms\":4000,\"media\":\"sys:ann_busy\"},"
    "{\"profile\":\"default\",\"situation\":\"no_answer\",\"mode\":\"announce\",\"media\":\"sys:ann_no_answer\"},"
    "{\"profile\":\"default\",\"situation\":\"unreachable\",\"mode\":\"announce\",\"media\":\"sys:ann_no_answer\"},"
    "{\"profile\":\"default\",\"situation\":\"not_found\",\"mode\":\"announce\",\"media\":\"sys:ann_invalid_number\"},"
    "{\"profile\":\"default\",\"situation\":\"invalid\",\"mode\":\"announce\",\"media\":\"sys:ann_invalid_number\"},"
    "{\"profile\":\"default\",\"situation\":\"declined\",\"mode\":\"tone\",\"tone\":\"sys:busy_kr\",\"tone_ms\":6000},"
    "{\"profile\":\"default\",\"situation\":\"congestion\",\"mode\":\"tone\",\"tone\":\"sys:congestion_kr\",\"tone_"
    "ms\":6000},"
    "{\"profile\":\"default\",\"situation\":\"forbidden\",\"mode\":\"none\"},"
    "{\"profile\":\"default\",\"situation\":\"hold\",\"mode\":\"media\",\"media\":\"sys:moh_simple\",\"loop\":true},"
    "{\"profile\":\"default\",\"situation\":\"call_waiting\",\"mode\":\"none\"},"
    // 착신전환(TS 24.604) — 전환 안내 1회 뒤 전환 대상이 응답할 때까지 링백음(발신 단말은 183+SDP 뒤 로컬 링백을 내지
    // 않는다)
    "{\"profile\":\"default\",\"situation\":\"forwarded\",\"mode\":\"announce_then_tone\",\"media\":\"sys:ann_"
    "forwarded\",\"tone\":\"sys:ringback_kr\"},"
    "{\"profile\":\"trunk\",\"situation\":\"busy\",\"mode\":\"none\"},"
    "{\"profile\":\"trunk\",\"situation\":\"no_answer\",\"mode\":\"none\"},"
    "{\"profile\":\"trunk\",\"situation\":\"unreachable\",\"mode\":\"none\"},"
    "{\"profile\":\"trunk\",\"situation\":\"not_found\",\"mode\":\"none\"},"
    "{\"profile\":\"trunk\",\"situation\":\"invalid\",\"mode\":\"none\"},"
    "{\"profile\":\"trunk\",\"situation\":\"declined\",\"mode\":\"none\"},"
    "{\"profile\":\"trunk\",\"situation\":\"congestion\",\"mode\":\"none\"},"
    "{\"profile\":\"trunk\",\"situation\":\"hold\",\"mode\":\"none\"},"
    "{\"profile\":\"trunk\",\"situation\":\"forwarded\",\"mode\":\"none\"},"
    // `cw_inband` = 망 in-band 통화중대기음(TS 24.615 — 단말이 Alert-Info 대기음을 못 내는 배치) 스위치: 착신자
    // 서비스의 hold_profile 로 고른다. 나머지 상황은 default 로
    "{\"profile\":\"cw_inband\",\"situation\":\"call_waiting_alert\",\"mode\":\"tone\",\"tone\":\"sys:call_waiting_"
    "kr\"},"
    // `ringback` = 서버 링백 스위치(§3.4) — ringback 행 하나만 두고 나머지 상황은 default 로 떨어진다. 접속서비스
    // announcement_profile 로 고른다
    "{\"profile\":\"ringback\",\"situation\":\"ringback\",\"mode\":\"media\",\"media\":\"sys:ringback_kr\",\"loop\":"
    "true}]";

CCspAnnouncementService::CCspAnnouncementService() {
}

void CCspAnnouncementService::Init() {
    std::map<std::string, std::map<int, CAnnAction>> mapRules;
    // 내장 기본 표를 먼저 깔고, 운영자 Rules 행을 (profile, situation) 키로 그 위에 덮어쓴다(announcements.md §6.1).
    //   운영자는 바꿀 행만 두면 되고, 내장 프로파일(default·trunk·ringback·cw_inband)은 이름 참조가 끊기지 않는다.
    //   내장 행을 끄려면 같은 키에 mode none 을 둔다.
    int nBuiltin = 0, nOperator = 0;
    auto loadRows = [&]( const SimpleJson::JsonNode &arr, bool bOperator, int &nCount ) {
        for ( size_t i = 0; i < arr.Size(); ++i ) {
            SimpleJson::JsonNode row = arr.At( i );
            if ( row.type != SimpleJson::JSON_OBJECT ) continue;
            std::string strProfile = row.GetString( "profile", "default" );
            EAnnSituation eSit = SituationOf( row.GetString( "situation" ) );
            if ( eSit == ANN_SIT_NONE ) {
                if ( bOperator )
                    CLog::Print( LOG_ERROR, "Announcement: rule #%zu unknown situation '%s' — skipped", i,
                                 row.GetString( "situation" ).c_str() );
                continue;
            }
            CAnnAction a;
            a.strMode = row.GetString( "mode", "none" );
            a.strTone = row.GetString( "tone" );
            a.iToneMs = (int)row.GetInt( "tone_ms", 4000 );
            a.strMedia = row.GetString( "media" );
            a.iRepeat = (int)row.GetInt( "repeat", 1 );
            a.bLoop = row.GetString( "loop" ) == "true";
            if ( a.strMode != "none" && a.strMode != "tone" && a.strMode != "announce" &&
                 a.strMode != "tone_then_announce" && a.strMode != "announce_then_tone" && a.strMode != "media" ) {
                CLog::Print( LOG_ERROR, "Announcement: rule %s/%s unknown mode '%s' → none", strProfile.c_str(),
                             SituationName( eSit ), a.strMode.c_str() );
                a.strMode = "none";
            }
            // 검증 — 톤 모드에 톤 id, 안내 모드에 음원 id 가 없으면 none 으로 낮추고 로그(설정 오류가 통화 장애로
            // 번지지 않게)
            if ( ( a.strMode == "tone" || a.strMode == "tone_then_announce" ) && a.strTone.empty() ) a.strMode = "none";
            if ( ( a.strMode == "announce" || a.strMode == "tone_then_announce" || a.strMode == "media" ||
                   a.strMode == "announce_then_tone" ) &&
                 a.strMedia.empty() )
                a.strMode = "none";
            // announce_then_tone 의 신호음이 비면 안내만(2 단계는 ringback 규칙으로)
            mapRules[strProfile][(int)eSit] = a;
            ++nCount;
        }
    };
    loadRows( SimpleJson::JsonNode::Parse( kDefaultRules ), false, nBuiltin );
    std::string strRules = gclsSetup.m_strAnnRulesJson;
    if ( strRules.find_first_not_of( " []\t\r\n" ) == std::string::npos ) strRules.clear();
    if ( !strRules.empty() ) {
        SimpleJson::JsonNode arr = SimpleJson::JsonNode::Parse( strRules );
        if ( arr.type == SimpleJson::JSON_ARRAY )
            loadRows( arr, true, nOperator );
        else
            CLog::Print( LOG_ERROR, "Announcement: Setup.Announcement.Rules is not an array — built-in table only" );
    }
    std::lock_guard<std::mutex> lock( m_mtx );
    m_mapRules = mapRules;
    m_bEnabled = gclsSetup.m_bAnnEnable;
    m_iMaxPlayMs = gclsSetup.m_iAnnMaxPlayMs > 0 ? gclsSetup.m_iAnnMaxPlayMs : 30000;
    m_strDefaultProfile = gclsSetup.m_strAnnDefaultProfile.empty() ? "default" : gclsSetup.m_strAnnDefaultProfile;
    CLog::Print( LOG_SYSTEM,
                 "Announcement: %s — built-in %d + operator %d rule(s), %zu profile(s), default='%s', max=%dms",
                 m_bEnabled ? "enabled" : "disabled", nBuiltin, nOperator, m_mapRules.size(),
                 m_strDefaultProfile.c_str(), m_iMaxPlayMs );
}

bool CCspAnnouncementService::IsEnabled() const {
    if ( !m_bEnabled ) return false;
    if ( !gclsCmpClient.SupportsAnn() ) {
        // CMP 가 resource.ann 을 광고하지 않는다(AnnPlayers=0 또는 구 CMP) — 안내 없이 응답 코드만. 전이 때 1회만 기록
        static std::atomic<bool> s_bLogged{ false };
        if ( !s_bLogged.exchange( true ) )
            CLog::Print( LOG_INFO,
                         "Announcement: CMP does not advertise resource.ann — announcements disabled until it does" );
        return false;
    }
    return true;
}

// SIP 코드 + Reason Q.850 cause → 상황 (announcements.md §2). cause 우선 — 피어·MGCF 가 PSTN 원인을 실어 준다.
EAnnSituation CCspAnnouncementService::Classify( int iSipStatus, const char *pszReason ) {
    int iCause = 0;
    if ( pszReason && *pszReason ) {
        const char *p = strstr( pszReason, "cause=" );
        if ( p && strncasecmp( pszReason, "Q.850", 5 ) == 0 ) iCause = atoi( p + 6 );
    }
    switch ( iCause ) {
        case 17:
            return ANN_SIT_BUSY;
        case 18:
        case 19:
            return ANN_SIT_NO_ANSWER;
        case 20:
            return ANN_SIT_UNREACHABLE;  // subscriber absent
        case 21:
            return ANN_SIT_DECLINED;
        case 1:
            return ANN_SIT_NOT_FOUND;  // unallocated number
        case 28:
            return ANN_SIT_INVALID;  // invalid number format
        case 34:
        case 38:
        case 41:
        case 42:
        case 44:
        case 47:
            return ANN_SIT_CONGESTION;
        default:
            break;
    }
    if ( iSipStatus == SIP_BUSY_HERE || iSipStatus == 600 ) return ANN_SIT_BUSY;
    if ( iSipStatus == SIP_REQUEST_TIME_OUT || iSipStatus == SIP_TEMPORARILY_UNAVAILABLE ) return ANN_SIT_NO_ANSWER;
    if ( iSipStatus == SIP_NOT_FOUND || iSipStatus == SIP_GONE ) return ANN_SIT_NOT_FOUND;
    if ( iSipStatus == SIP_ADDRESS_INCOMPLETE ) return ANN_SIT_INVALID;
    if ( iSipStatus == SIP_DECLINE || iSipStatus == 607 ) return ANN_SIT_DECLINED;
    if ( iSipStatus == SIP_FORBIDDEN ) return ANN_SIT_FORBIDDEN;
    if ( iSipStatus == SIP_NOT_ACCEPTABLE_HERE || iSipStatus == 606 ) return ANN_SIT_CONGESTION;
    if ( iSipStatus >= 500 && iSipStatus < 600 ) return ANN_SIT_CONGESTION;
    return ANN_SIT_NONE;
}

CAnnAction CCspAnnouncementService::Resolve( EAnnSituation eSit, const std::string &strProfile ) const {
    std::lock_guard<std::mutex> lock( m_mtx );
    if ( eSit == ANN_SIT_NONE ) return CAnnAction();
    if ( !strProfile.empty() ) {
        auto itP = m_mapRules.find( strProfile );
        if ( itP != m_mapRules.end() ) {
            auto itS = itP->second.find( (int)eSit );
            if ( itS != itP->second.end() ) return itS->second;
        }
    }
    auto itD = m_mapRules.find( m_strDefaultProfile );
    if ( itD != m_mapRules.end() ) {
        auto itS = itD->second.find( (int)eSit );
        if ( itS != itD->second.end() ) return itS->second;
    }
    return CAnnAction();
}

// 발신자 프로파일 — 피어(inbound Route 로 식별)면 RemoteNode.announcement_profile, 가입자면 접속서비스
// announcement_profile
std::string CCspAnnouncementService::ProfileForCaller( const std::string &strCaller, CSipMessage *pclsMessage ) {
    if ( pclsMessage && !pclsMessage->m_strClientIp.empty() ) {
        std::string strLn;
        if ( pclsMessage->m_iListenerId > 0 ) {
            LocalNodeInfo ln = gclsLocalNodeMap.GetByIntId( pclsMessage->m_iListenerId );
            if ( ln.IsValid() ) strLn = ln.name;
        }
        int iSrcPort = ( pclsMessage->m_eTransport == E_SIP_UDP ) ? pclsMessage->m_iClientPort : 0;
        RouteConfig rc = gclsRouteMap.FindInbound( strLn, pclsMessage->m_strClientIp, iSrcPort,
                                                   SipGetTransport( pclsMessage->m_eTransport ) );
        if ( rc.IsValid() ) {
            RemoteNodeInfo rn = gclsRemoteNodeMap.GetByName( rc.remote_node_ref );
            return rn.announcement_profile;
        }
    }
    ServiceInfo svc = gclsServiceMap.GetForUser( strCaller, "volte" );
    return svc.announcement_profile;
}

std::string CCspAnnouncementService::HoldProfileFor( const std::string &strUser ) {
    ServiceInfo svc = gclsServiceMap.GetForUser( strUser, "volte" );
    return svc.hold_profile.empty() ? svc.announcement_profile : svc.hold_profile;
}

std::string CCspAnnouncementService::NewPlayId( const char *pszCallId ) {
    // play_id 는 시도마다 새 키 — 같은 호에 링백 뒤 실패 안내가 이어져도 앞선 DONE 이 새 재생을 걷지 않게
    // (cmp_media_api §6.7)
    static std::atomic<unsigned> s_uSeq{ 0 };
    char sz[192];
    snprintf( sz, sizeof( sz ), "ann-%s-%u", pszCallId ? pszCallId : "",
              s_uSeq.fetch_add( 1, std::memory_order_relaxed ) + 1 );
    return sz;
}

std::vector<CCmpClient::AnnItem> CCspAnnouncementService::ItemsOf( const CAnnAction &a, int &iRepeat,
                                                                   int &iMaxMs ) const {
    std::vector<CCmpClient::AnnItem> v;
    iRepeat = 1;
    iMaxMs = m_iMaxPlayMs;
    if ( a.strMode == "tone" ) {
        CCmpClient::AnnItem t;
        t.strId = a.strTone;
        t.iRepeat = 0;
        t.iMaxMs = a.iToneMs > 0 ? a.iToneMs : 4000;
        v.push_back( t );
    } else if ( a.strMode == "announce" || a.strMode == "announce_then_tone" ) {
        // announce_then_tone 의 신호음(2 단계)은 안내가 끝난 뒤 서비스가 새 RELAY_PLAY(loop)로 잇는다(OnPlayDone)
        CCmpClient::AnnItem m;
        m.strId = a.strMedia;
        m.iRepeat = a.iRepeat > 0 ? a.iRepeat : 1;
        v.push_back( m );
    } else if ( a.strMode == "tone_then_announce" ) {
        CCmpClient::AnnItem t;
        t.strId = a.strTone;
        t.iRepeat = 0;
        t.iMaxMs = a.iToneMs > 0 ? a.iToneMs : 4000;
        CCmpClient::AnnItem m;
        m.strId = a.strMedia;
        m.iRepeat = a.iRepeat > 0 ? a.iRepeat : 1;
        v.push_back( t );
        v.push_back( m );
    } else if ( a.strMode == "media" ) {
        CCmpClient::AnnItem m;
        m.strId = a.strMedia;
        m.iRepeat = 1;
        v.push_back( m );
        iRepeat = a.bLoop ? 0 : ( a.iRepeat > 0 ? a.iRepeat : 1 );
        if ( a.bLoop ) iMaxMs = 0;  // STOP 까지
    }
    return v;
}

// ──────────────────────────────────────────────────────────────
//  early media answer 조립 (§3.1) — A 의 오퍼로 CSP 가 answer 를 만든다
// ──────────────────────────────────────────────────────────────

static bool _playable( const RelayCodec::CodecDesc &c ) {
    return c.name == "AMR-WB" || c.name == "PCMU" || c.name == "PCMA" || c.name == "G722";
}

bool CCspAnnouncementService::BuildEarlyAnswer( const char *pszACallId, const RelaySdesLeg &clsSdesA,
                                                const RelayCodec::LegCodecs &clsCodecsA, const std::string &strRelayIp,
                                                int iRelayPort, CSipCallRtp &clsAns, RelayCodec::CodecDesc &clsChosen,
                                                int &iTePt ) {
    // A 의 offer(다이얼로그가 기억) — 미디어 목록 그대로 시작해 오디오 한 코덱으로 좁힌다
    if ( !gclsUserAgent.GetRemoteCallRtp( pszACallId, &clsAns ) ) return false;
    std::vector<RelayCodec::CodecDesc> vecOffered = clsCodecsA.offered;
    int iOffTePt = clsCodecsA.tePt;
    std::string strTeRtpmap = clsCodecsA.teRtpmap, strTeFmtp = clsCodecsA.teFmtp;
    if ( vecOffered.empty() )
        vecOffered = RelayCodec::AudioCodecs( clsAns.m_clsMediaList, &iOffTePt, &strTeRtpmap, &strTeFmtp );
    clsChosen = RelayCodec::CodecDesc();
    for ( const RelayCodec::CodecDesc &c : vecOffered ) {
        if ( _playable( c ) && c.Valid() ) {
            clsChosen = c;
            break;
        }
    }
    if ( !clsChosen.Valid() ) return false;
    iTePt = iOffTePt;
    // 오디오 = 고른 코덱(+telephone-event echo), 그 밖 m= 라인은 port 0(안내는 오디오만 — 영상은 열지 않는다)
    RelayCodec::RewriteAudio( clsAns.m_clsMediaList, clsChosen, iOffTePt, strTeRtpmap, strTeFmtp );
    // relay 주소(A 전용 포트) — SetIpPort 가 모든 m= 에 포트를 배정하므로 그 뒤 비오디오를 0 으로
    clsAns.SetIpPort( strRelayIp.c_str(), iRelayPort, SOCKET_COUNT_PER_MEDIA );
    for ( auto &m : clsAns.m_clsMediaList )
        if ( m.m_strMedia != "audio" ) m.m_iPort = 0;
    // SDES — A leg 의 offer tag/suite echo + 서버 키(a=crypto) 재광고 (media_security.md §5.2)
    MediaSdes::RewriteRelaySdpForLeg( clsAns.m_clsMediaList, clsSdesA, false );
    // 방향 sendrecv — sendonly 로 답하면 NAT 뒤 단말이 RTP 를 보내지 않아 latch 가 안 된다(§3.1). 방향 지시는
    // P-Early-Media 헤더
    clsAns.SetDirection( E_RTP_SEND_RECV );
    return true;
}

// 183(+SDP) 송신·CMP leg 코덱 통지·RELAY_PLAY — 실패 안내 공통 (bEarlyAlready = A 에 이미 SDP 를 낸 뒤라 183 생략)
bool CCspAnnouncementService::StartFailurePlay( CAnnCall &c, const CAnnAction &a, const CSipCallRtp *pclsAns,
                                                const RelayCodec::CodecDesc &clsCodec, int iTePt, bool bEarlyAlready ) {
    if ( !bEarlyAlready ) {
        if ( pclsAns == NULL ) return false;
        // CMP 에 A leg 의 재생 코덱을 알린다 — 주소 미변경(remote_port 0), remote_pt/remote_codec 만 (재생 파일 선택
        // 근거). 183 은 RELAY_PLAY 가 받아들여진 뒤에 낸다 — 재생기가 거절(음원 없음·슬롯 소진)하면 A 에 answer 를
        // 남기지 않아야 폴백이 깨끗하다(SDP 만 받은 단말은 로컬 링백을 내지 않고 무음을 듣는다).
        gclsCmpClient.ModifySession( c.strRelaySessionId, "", 0, 0, c.iPeerIdx, c.strCaller, c.strCallee, c.strSesId, 0,
                                     "", clsCodec.pt, clsCodec.pt, iTePt > 0 ? iTePt : 0, iTePt > 0 ? iTePt : 0,
                                     clsCodec.Label() );
    }
    int iRepeat = 1, iMaxMs = m_iMaxPlayMs;
    std::vector<CCmpClient::AnnItem> vecItems = ItemsOf( a, iRepeat, iMaxMs );
    if ( vecItems.empty() ) return false;
    c.strPlayId = NewPlayId( c.strACallId.c_str() );
    c.strMedia.clear();
    for ( const auto &it : vecItems ) c.strMedia += ( c.strMedia.empty() ? "" : "," ) + it.strId;
    c.iMaxMs = iMaxMs;
    int iDurationMs = 0;
    std::string strErr;
    if ( !gclsCmpClient.PlayAnnouncement( c.strRelaySessionId, c.iPeerIdx, c.strPlayId, vecItems, iRepeat, 0, iMaxMs,
                                          c.strSesId, c.strService, iDurationMs, strErr ) ) {
        CLog::Print( LOG_ERROR, "Announcement: RELAY_PLAY rejected (%s) — fallback to plain %d (CallId=%s media=%s)",
                     strErr.c_str(), c.iFinalStatus, c.strACallId.c_str(), c.strMedia.c_str() );
        ++m_lFallback;
        return false;
    }
    if ( !bEarlyAlready ) {
        std::vector<std::pair<std::string, std::string>> vecHdr;
        vecHdr.push_back( std::make_pair( "P-Early-Media", "sendonly" ) );  // RFC 5009 — 망→단말 early media 인가
        CSipCallRtp clsCopy = *pclsAns;
        if ( !gclsUserAgent.RingCall( c.strACallId.c_str(), SIP_SESSION_PROGRESS, &clsCopy, vecHdr ) ) {
            CLog::Print( LOG_ERROR, "Announcement: 183 send failed (CallId=%s)", c.strACallId.c_str() );
            gclsCmpClient.StopAnnouncement( c.strRelaySessionId, c.iPeerIdx, c.strPlayId, c.strSesId, c.strService );
            return false;
        }
        c.bEarlySent = true;
    }
    ++m_lStarted;
    time( &c.tStart );
    CLog::Print(
        LOG_SYSTEM, "Announcement: %s → %s play=%s media=[%s] dur=%dms final=%d %s (CallId=%s relay=%s peer%d)",
        SituationName( c.eSit ), a.Label().c_str(), c.strPlayId.c_str(), c.strMedia.c_str(), iDurationMs,
        c.iFinalStatus, c.strFinalReason.c_str(), c.strACallId.c_str(), c.strRelaySessionId.c_str(), c.iPeerIdx );
    return true;
}

// ──────────────────────────────────────────────────────────────
//  실패 안내 — B 실패 (§3.1)
// ──────────────────────────────────────────────────────────────

bool CCspAnnouncementService::OnLegFailed( const char *pszBCallId, const CCallInfo &clsB, int iSipStatus,
                                           const char *pszReason ) {
    if ( !IsEnabled() ) return false;
    if ( clsB.m_bRecv || clsB.m_bEstablished || clsB.m_strPeerCallId.empty() || clsB.m_strRelaySessionId.empty() )
        return false;
    const std::string strACallId = clsB.m_strPeerCallId;
    CCallInfo clsA;
    if ( !gclsCallMap.Select( strACallId.c_str(), clsA ) || clsA.m_bEstablished ) return false;
    if ( gclsUserAgent.IsConnected( strACallId.c_str() ) ) return false;  // 확립된 A 는 안내 대상이 아니다(BYE)

    const int iFinal = CModuleDispatcher::RelayEndStatus( iSipStatus );
    if ( iFinal < 400 ) return false;
    const EAnnSituation eSit = Classify( iSipStatus, pszReason );
    const CAnnAction a = Resolve( eSit, clsA.m_strAnnProfile );
    if ( a.IsNone() ) return false;

    CAnnCall c;
    c.strACallId = strACallId;
    c.strRelaySessionId = clsB.m_strRelaySessionId;
    c.strSesId = clsB.m_strRelaySesId;
    c.iPeerIdx = clsA.m_bRecv ? 0 : 1;
    c.eSit = eSit;
    c.iFinalStatus = iFinal;
    c.strFinalReason = pszReason ? pszReason : "";
    c.strCaller = clsB.m_strRelayCaller;
    c.strCallee = clsB.m_strRelayCallee;

    // A 에 이미 SDP 를 냈는가 — B 의 18x+SDP(C1a) 또는 서버 링백(§3.4). 그러면 183 을 다시 내지 않고 재생만.
    bool bEarly = false;
    RelayCodec::CodecDesc clsCodec;
    int iTePt = -1;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        auto it = m_mapCalls.find( strACallId );
        if ( it != m_mapCalls.end() && it->second.bEarlySent ) {
            bEarly = true;
            m_mapCalls.erase( it );  // 링백 상태를 실패 안내 상태로 바꾼다(재생기는 RELAY_PLAY 교체)
        }
    }
    if ( !bEarly ) {
        CSipCallRtp clsLocal;
        if ( gclsUserAgent.GetLocalCallRtp( strACallId.c_str(), &clsLocal ) && clsLocal.m_iPort > 0 ) bEarly = true;
    }
    CSipCallRtp clsAns;
    if ( !bEarly ) {
        const std::string strRelayIp =
            clsA.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress() : clsA.m_strRelayLocalIp;
        // A 에게 광고하는 relay 포트 = B entry 의 m_iPeerRtpPort (leg 별 포트 — CallMap::Insert 규약)
        if ( clsB.m_iPeerRtpPort <= 0 ||
             !BuildEarlyAnswer( strACallId.c_str(), clsA.m_clsSdesLeg[c.iPeerIdx], clsA.m_clsCodecLeg[c.iPeerIdx],
                                strRelayIp, clsB.m_iPeerRtpPort, clsAns, clsCodec, iTePt ) ) {
            CLog::Print( LOG_INFO, "Announcement: cannot build early answer for %s → plain %d", strACallId.c_str(),
                         iFinal );
            ++m_lFallback;
            return false;
        }
    }
    if ( !StartFailurePlay( c, a, bEarly ? NULL : &clsAns, clsCodec, iTePt, bEarly ) ) return false;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        m_mapCalls[strACallId] = c;
        m_mapPlayToCall[c.strPlayId] = strACallId;
    }
    (void)pszBCallId;
    return true;
}

// ──────────────────────────────────────────────────────────────
//  실패 안내 — B leg 이전 자체 거절 (§3.2)
// ──────────────────────────────────────────────────────────────

bool CCspAnnouncementService::Reject( const char *pszCallId, CSipCallRtp *pclsRtp, const char *pszFrom,
                                      const char *pszTo, int iSipStatus, const char *pszReason,
                                      CSipMessage *pclsMessage ) {
    if ( !IsEnabled() || pclsRtp == NULL || pszCallId == NULL ) return false;
    if ( !gclsSetup.m_bUseRtpRelay ) return false;
    EAnnSituation eSit = Classify( iSipStatus, pszReason );
    if ( eSit == ANN_SIT_NO_ANSWER && iSipStatus == SIP_TEMPORARILY_UNAVAILABLE )
        eSit = ANN_SIT_UNREACHABLE;  // CSP 판정 480 = 미등록
    const std::string strProfile = ProfileForCaller( pszFrom ? pszFrom : "", pclsMessage );
    const CAnnAction a = Resolve( eSit, strProfile );
    if ( a.IsNone() ) return false;

    int iAudioPort = pclsRtp->GetAudioPort();
    if ( iAudioPort <= 0 && pclsRtp->m_iPort > 0 ) iAudioPort = pclsRtp->m_iPort;
    if ( iAudioPort <= 0 ) return false;

    // A leg SDES(정책 × offer) — required 인데 offer 에 없으면 안내 없이 원코드
    ServiceInfo clsSvc = gclsServiceMap.GetForUser( pszFrom ? pszFrom : "", "volte" );
    RelaySdesLeg clsSdesA;
    CmpMediaCrypto clsCrypto;
    if ( MediaSdes::EvalRelayOfferSdes( clsSvc.media_srtp, pclsRtp->m_clsMediaList, "audio", clsSdesA.clsAudio ) < 0 )
        return false;
    if ( clsSdesA.clsAudio.bSrtp && !MediaSdes::BuildCmpKeys( clsSdesA.clsAudio.strSuite, clsSdesA.clsAudio.strUeKey,
                                                              clsSdesA.clsAudio.strSrvKey, clsCrypto ) )
        return false;
    // NAT 판정 — SDP IP vs 시그널링 실소스
    int iNat = 0;
    std::string strGuardIp;
    {
        std::string strSigIp;
        int iSigPort = 0;
        if ( pclsMessage ) pclsMessage->GetTopViaIpPort( strSigIp, iSigPort );
        if ( strSigIp.empty() && pszFrom ) {
            CUserInfo clsFromInfo;
            if ( gclsUserMap.Select( pszFrom, clsFromInfo ) ) strSigIp = clsFromInfo.m_strIp;
        }
        if ( CCspServiceMap::EvalMediaNat( clsSvc, pclsRtp->m_strIp, strSigIp, strGuardIp ) ) iNat = 1;
    }
    RelayCodec::LegCodecs clsCodecsA;
    clsCodecsA.offered =
        RelayCodec::AudioCodecs( pclsRtp->m_clsMediaList, &clsCodecsA.tePt, &clsCodecsA.teRtpmap, &clsCodecsA.teFmtp );
    RelayCodec::CodecDesc clsChosen;
    for ( const RelayCodec::CodecDesc &cd : clsCodecsA.offered )
        if ( _playable( cd ) && cd.Valid() ) {
            clsChosen = cd;
            break;
        }
    if ( !clsChosen.Valid() ) return false;

    // relay — A leg(peer0)만. CallMap 에 A 단독 entry 를 두어 A 종료 때 RELAY_REMOVE 가 나가게 한다
    const std::string strSesId = gclsSipLogger.GetOrIssueSesId( pszCallId, pszFrom ? pszFrom : "" );
    const std::string strRelayId = CCmpClient::IssueSessionId();
    std::string strRelayIp;
    int iLocalPort = 0, iLocalVideoPort = 0, iLocalPortB = 0, iLocalVideoPortB = 0;
    if ( !gclsCmpClient.AddSession( strRelayId, strRelayIp, iLocalPort, iLocalVideoPort, iLocalPortB, iLocalVideoPortB,
                                    "", pszFrom ? pszFrom : "", pszTo ? pszTo : "", pclsRtp->m_strIp, iAudioPort, 0,
                                    strSesId, iNat, strGuardIp, clsChosen.pt, clsChosen.pt,
                                    clsCodecsA.tePt > 0 ? clsCodecsA.tePt : 0,
                                    clsCodecsA.tePt > 0 ? clsCodecsA.tePt : 0, clsChosen.Label(),
                                    clsCrypto.bEnabled ? &clsCrypto : NULL, NULL ) ) {
        CLog::Print( LOG_ERROR, "Announcement: RELAY_ADD failed — fallback to plain %d (CallId=%s)", iSipStatus,
                     pszCallId );
        ++m_lFallback;
        return false;
    }
    if ( strRelayIp.empty() ) strRelayIp = CspAddressing::GetLocalRtpAddress();
    CCallInfo clsInfo;
    clsInfo.m_bRecv = true;
    clsInfo.m_iPeerRtpPort = iLocalPortB;
    clsInfo.m_strAnnProfile = strProfile;
    gclsCallMap.Insert( pszCallId, clsInfo );
    gclsCallMap.SetRelayInfo( pszCallId, strRelayId, strSesId, strRelayIp, pszFrom ? pszFrom : "", pszTo ? pszTo : "" );
    gclsCallMap.SetRelaySdesLeg( pszCallId, 0, clsSdesA );
    gclsCallMap.SetRelayCodecLeg( pszCallId, 0, clsCodecsA );

    CAnnCall c;
    c.strACallId = pszCallId;
    c.strRelaySessionId = strRelayId;
    c.strSesId = strSesId;
    c.iPeerIdx = 0;
    c.eSit = eSit;
    c.iFinalStatus = iSipStatus;
    c.strFinalReason = pszReason ? pszReason : "";
    c.strCaller = pszFrom ? pszFrom : "";
    c.strCallee = pszTo ? pszTo : "";
    CSipCallRtp clsAns;
    RelayCodec::CodecDesc clsCodec;
    int iTePt = -1;
    if ( !BuildEarlyAnswer( pszCallId, clsSdesA, clsCodecsA, strRelayIp, iLocalPort, clsAns, clsCodec, iTePt ) ||
         !StartFailurePlay( c, a, &clsAns, clsCodec, iTePt, false ) ) {
        // 되돌린다 — relay·CallMap entry 를 걷고 호출자가 원코드를 낸다
        gclsCallMap.DeleteOne( pszCallId );
        gclsCmpClient.RemoveSession( strRelayId, pszFrom ? pszFrom : "", pszTo ? pszTo : "", strSesId );
        return false;
    }
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        m_mapCalls[pszCallId] = c;
        m_mapPlayToCall[c.strPlayId] = pszCallId;
    }
    return true;
}

// 최종 응답 — 재생 끝(completed/max/stopped)·상한·CANCEL. 맵에서 뺀 뒤 SIP 를 부른다(EventCallEnd 재진입 → OnCallEnd 락
// 회피).
void CCspAnnouncementService::FinishEarly( const std::string &strACallId, CAnnCall &c, const char *pszResult,
                                           int iPlayedMs ) {
    if ( gclsCallDir.IsEnabled() )
        gclsCallDir.VoipCallAnnouncement( strACallId, c.strCaller, c.strCallee, SituationName( c.eSit ), c.strMedia,
                                          iPlayedMs, pszResult );
    if ( c.iFinalStatus > 0 ) {
        CLog::Print( LOG_SYSTEM, "Announcement: %s done(%s, %dms) → final %d %s (CallId=%s)", SituationName( c.eSit ),
                     pszResult, iPlayedMs, c.iFinalStatus, c.strFinalReason.c_str(), strACallId.c_str() );
        // 미응답 UAS 다이얼로그를 CSP 자신이 거절하면 psip 는 EventCallEnd 를 올리지 않는다 — B 실패 경로가 건너뛴 마감
        //   (CDR·DB 종료·relay 회수·소유권)을 여기서 한다. 순서: CDR/DB(다이얼로그 살아 있을 때) → 최종 응답 → CallMap
        //   Delete(RELAY_REMOVE)
        if ( gclsCallDir.IsEnabled() )
            gclsCallDir.VoipCallEnd( strACallId, CCallDir::_ReasonOfStatus( c.iFinalStatus ), 0, c.iFinalStatus );
        gclsDispatcher.OnCallEnded( strACallId.c_str(), c.iFinalStatus );
        gclsUserAgent.StopCall( strACallId.c_str(), c.iFinalStatus,
                                c.strFinalReason.empty() ? NULL : c.strFinalReason.c_str() );
        gclsCallMap.Delete( strACallId.c_str(), true );
        gclsDispatcher.RemoveCallOwner( strACallId.c_str() );
    }
}

void CCspAnnouncementService::OnPlayDone( const std::string &strSessionId, int iPeerIdx, const std::string &strPlayId,
                                          const std::string &strReason, int iPlayedMs ) {
    (void)strSessionId;
    (void)iPeerIdx;
    CAnnCall c;
    std::string strACallId;
    bool bFound = false;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        auto itP = m_mapPlayToCall.find( strPlayId );
        if ( itP != m_mapPlayToCall.end() ) {
            strACallId = itP->second;
            m_mapPlayToCall.erase( itP );
            auto itC = m_mapCalls.find( strACallId );
            if ( itC != m_mapCalls.end() && itC->second.strPlayId == strPlayId ) {
                c = itC->second;
                bFound = true;
                if ( c.iFinalStatus > 0 ) {
                    m_mapCalls.erase( itC );  // 링백(finalStatus 0)은 entry 를 남긴다(bEarlySent 표식)
                } else {
                    itC->second.strPlayId.clear();
                    // 전환 안내 1 단계 완료 — 2 단계를 붙이는 동안의 표식(락 밖 RELAY_PLAY 와 B 응답의 경합 방어)
                    if ( c.eSit == ANN_SIT_FORWARDED && ( strReason == "completed" || strReason == "max" ) )
                        itC->second.bPhase2 = true;
                }
            }
        }
        if ( !bFound ) {
            for ( auto it = m_mapHold.begin(); it != m_mapHold.end(); ++it )
                if ( it->second.strPlayId == strPlayId ) {
                    m_mapHold.erase( it );
                    break;
                }
        }
    }
    if ( !bFound ) return;
    if ( c.iFinalStatus > 0 ) {
        FinishEarly( strACallId, c, strReason == "stopped" ? "stopped" : strReason.c_str(), iPlayedMs );
        return;
    }
    if ( c.eSit == ANN_SIT_FORWARDED && ( strReason == "completed" || strReason == "max" ) ) {
        // 전환 안내(1 단계)가 끝났다 — CDR 에 남기고 2 단계: announce_then_tone 의 신호음, 없으면 ringback 규칙(media
        // 면 loop).
        //   B 가 아직 응답하지 않았을 때만(entry 가 남아 있다 = OnRingbackEnd/OnLegFailed/OnCallEnd 가 지우지 않았다).
        if ( gclsCallDir.IsEnabled() )
            gclsCallDir.VoipCallAnnouncement( strACallId, c.strCaller, c.strCallee, SituationName( c.eSit ), c.strMedia,
                                              iPlayedMs, strReason );
        CAnnAction next;
        if ( !c.strNextTone.empty() ) {
            next.strMode = "media";
            next.strMedia = c.strNextTone;
        } else {
            next = Resolve( ANN_SIT_RINGBACK, c.strProfile );
            if ( next.strMode != "media" && next.strMode != "announce" ) next.strMode = "none";
        }
        if ( next.IsNone() || next.strMedia.empty() ) {
            CLog::Print( LOG_INFO, "Announcement: forwarded play %s ended (%s) — no ringback phase CallId=%s",
                         strPlayId.c_str(), strReason.c_str(), strACallId.c_str() );
            std::lock_guard<std::mutex> lock( m_mtx );
            auto itC = m_mapCalls.find( strACallId );
            if ( itC != m_mapCalls.end() ) itC->second.bPhase2 = false;
            return;
        }
        next.bLoop = true;
        CAnnCall c2 = c;
        c2.eSit = ANN_SIT_RINGBACK;
        c2.strNextTone.clear();
        c2.bPhase2 = false;
        RelayCodec::CodecDesc clsUnused;
        if ( !StartFailurePlay( c2, next, NULL, clsUnused, -1, true ) ) return;
        bool bStale = false;
        {
            std::lock_guard<std::mutex> lock( m_mtx );
            auto itC = m_mapCalls.find( strACallId );
            // 사이에 B 가 응답했거나(OnRingbackEnd 가 bPhase2 를 지웠다) 호가 끝났거나(entry 삭제) B 실패 안내로
            // 바뀌었으면(entry 교체) 방금 붙인 링백은 걷는다 — 확립 통화에 신호음이 섞이지 않게
            if ( itC == m_mapCalls.end() || !itC->second.bPhase2 ) {
                bStale = true;
            } else {
                itC->second = c2;
                m_mapPlayToCall[c2.strPlayId] = strACallId;
            }
        }
        if ( bStale ) {
            gclsCmpClient.StopAnnouncement( c2.strRelaySessionId, c2.iPeerIdx, c2.strPlayId, c2.strSesId,
                                            c2.strService );
            CLog::Print( LOG_INFO, "Announcement: forwarded ringback phase dropped — call moved on (CallId=%s)",
                         strACallId.c_str() );
        }
        return;
    }
    CLog::Print( LOG_INFO, "Announcement: %s play %s ended (%s) CallId=%s", SituationName( c.eSit ), strPlayId.c_str(),
                 strReason.c_str(), strACallId.c_str() );
}

void CCspAnnouncementService::Tick() {
    std::vector<std::pair<std::string, CAnnCall>> vecExpired;
    time_t now = time( NULL );
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        for ( auto it = m_mapCalls.begin(); it != m_mapCalls.end(); ) {
            CAnnCall &c = it->second;
            if ( c.iFinalStatus > 0 && c.tStart > 0 && now - c.tStart > ( c.iMaxMs / 1000 ) + 3 ) {
                vecExpired.push_back( std::make_pair( it->first, c ) );
                m_mapPlayToCall.erase( c.strPlayId );
                it = m_mapCalls.erase( it );
            } else {
                ++it;
            }
        }
    }
    for ( auto &e : vecExpired ) {
        gclsCmpClient.StopAnnouncement( e.second.strRelaySessionId, e.second.iPeerIdx, e.second.strPlayId,
                                        e.second.strSesId, e.second.strService );
        FinishEarly( e.first, e.second, "timeout", e.second.iMaxMs );
    }
}

// ──────────────────────────────────────────────────────────────
//  종료·교체 — 재생 회수
// ──────────────────────────────────────────────────────────────

void CCspAnnouncementService::OnCallEnd( const char *pszCallId ) {
    if ( pszCallId == NULL ) return;
    OnCallWaitingEnd( pszCallId );  // 이 leg 가 대기 호였다면 착신자 leg 의 대기음을 걷는다
    CAnnCall c;
    bool bEarly = false;
    std::vector<CHoldPlay> vecHold;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        auto it = m_mapCalls.find( pszCallId );
        if ( it != m_mapCalls.end() ) {
            c = it->second;
            bEarly = true;
            if ( !c.strPlayId.empty() ) m_mapPlayToCall.erase( c.strPlayId );
            m_mapCalls.erase( it );
        }
        for ( auto ih = m_mapHold.begin(); ih != m_mapHold.end(); ) {
            if ( ih->first == pszCallId || ih->second.strHeldCallId == pszCallId ) {
                vecHold.push_back( ih->second );
                ih = m_mapHold.erase( ih );
            } else {
                ++ih;
            }
        }
    }
    if ( bEarly && !c.strPlayId.empty() ) {
        // A 가 CANCEL(또는 다른 경로 종료) — 재생만 걷는다. 최종 응답은 종료 경로가 낸다(487 등)
        gclsCmpClient.StopAnnouncement( c.strRelaySessionId, c.iPeerIdx, c.strPlayId, c.strSesId, c.strService );
        if ( gclsCallDir.IsEnabled() && c.iFinalStatus > 0 )
            gclsCallDir.VoipCallAnnouncement( pszCallId, c.strCaller, c.strCallee, SituationName( c.eSit ), c.strMedia,
                                              0, "cancelled" );
        CLog::Print( LOG_INFO, "Announcement: play %s stopped — leg ended before final (CallId=%s)",
                     c.strPlayId.c_str(), pszCallId );
    }
    for ( const CHoldPlay &h : vecHold )
        gclsCmpClient.StopAnnouncement( h.strRelaySessionId, h.iPeerIdx, h.strPlayId, h.strSesId, h.strService );
}

void CCspAnnouncementService::OnLegReplaced( const std::string &strRelaySessionId ) {
    std::vector<CHoldPlay> vecHold;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        for ( auto ih = m_mapHold.begin(); ih != m_mapHold.end(); ) {
            if ( ih->second.strRelaySessionId == strRelaySessionId ) {
                vecHold.push_back( ih->second );
                ih = m_mapHold.erase( ih );
            } else {
                ++ih;
            }
        }
    }
    for ( const CHoldPlay &h : vecHold )
        gclsCmpClient.StopAnnouncement( h.strRelaySessionId, h.iPeerIdx, h.strPlayId, h.strSesId, h.strService );
}

// ──────────────────────────────────────────────────────────────
//  보류 음악 (§3.3)
// ──────────────────────────────────────────────────────────────

bool CCspAnnouncementService::OnHold( const char *pszHolderCallId, const CCallInfo &clsHolder ) {
    if ( !IsEnabled() || clsHolder.m_strRelaySessionId.empty() || pszHolderCallId == NULL ) return false;
    const int iHeldIdx = clsHolder.m_bRecv ? 1 : 0;
    const std::string strHeldUser = clsHolder.m_bRecv ? clsHolder.m_strRelayCallee : clsHolder.m_strRelayCaller;
    const CAnnAction a = Resolve( ANN_SIT_HOLD, HoldProfileFor( strHeldUser ) );
    if ( a.IsNone() || a.strMedia.empty() ) return false;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        if ( m_mapHold.count( pszHolderCallId ) ) return true;  // 같은 보류의 재-INVITE(갱신) — 멱등
    }
    CHoldPlay h;
    h.strRelaySessionId = clsHolder.m_strRelaySessionId;
    h.strSesId = clsHolder.m_strRelaySesId;
    h.iPeerIdx = iHeldIdx;
    h.strPlayId = NewPlayId( pszHolderCallId );
    h.strHeldCallId = clsHolder.m_strPeerCallId;
    int iRepeat = 0, iMaxMs = 0;
    std::vector<CCmpClient::AnnItem> vecItems = ItemsOf( a, iRepeat, iMaxMs );
    if ( a.strMode != "media" ) {
        iRepeat = 0;
        iMaxMs = 0;
    }  // hold 는 항상 STOP 까지
    int iDur = 0;
    std::string strErr;
    if ( !gclsCmpClient.PlayAnnouncement( h.strRelaySessionId, h.iPeerIdx, h.strPlayId, vecItems, 0, 0, 0, h.strSesId,
                                          h.strService, iDur, strErr ) ) {
        CLog::Print( LOG_ERROR, "Announcement: hold music RELAY_PLAY rejected (%s) relay=%s peer%d", strErr.c_str(),
                     h.strRelaySessionId.c_str(), h.iPeerIdx );
        ++m_lFallback;
        return false;
    }
    ++m_lStarted;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        m_mapHold[pszHolderCallId] = h;
    }
    CLog::Print( LOG_SYSTEM, "Announcement: hold music %s → peer%d(%s) play=%s (holder CallId=%s relay=%s)",
                 a.Label().c_str(), iHeldIdx, strHeldUser.c_str(), h.strPlayId.c_str(), pszHolderCallId,
                 h.strRelaySessionId.c_str() );
    return true;
}

void CCspAnnouncementService::OnResume( const char *pszHolderCallId, const CCallInfo &clsHolder ) {
    (void)clsHolder;
    if ( pszHolderCallId == NULL ) return;
    CHoldPlay h;
    bool bFound = false;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        auto it = m_mapHold.find( pszHolderCallId );
        if ( it != m_mapHold.end() ) {
            h = it->second;
            bFound = true;
            m_mapHold.erase( it );
        }
    }
    if ( !bFound ) return;
    int iPlayed = 0;
    gclsCmpClient.StopAnnouncement( h.strRelaySessionId, h.iPeerIdx, h.strPlayId, h.strSesId, h.strService, &iPlayed );
    CLog::Print( LOG_SYSTEM, "Announcement: hold music stopped (resume) play=%s played=%dms (holder CallId=%s)",
                 h.strPlayId.c_str(), iPlayed, pszHolderCallId );
}

// ──────────────────────────────────────────────────────────────
//  서버 링백 (§3.4) — 프로파일 ringback.mode=media 일 때만
// ──────────────────────────────────────────────────────────────

bool CCspAnnouncementService::OnRingback( const char *pszBCallId, const CCallInfo &clsB, CSipCallRtp **ppclsAnswerForA,
                                          EAnnSituation eSit ) {
    (void)pszBCallId;
    if ( ppclsAnswerForA ) *ppclsAnswerForA = NULL;
    if ( !IsEnabled() || clsB.m_bRecv || clsB.m_strPeerCallId.empty() || clsB.m_strRelaySessionId.empty() )
        return false;
    const std::string strACallId = clsB.m_strPeerCallId;
    {
        // 이미 A 에 CSP answer 를 냈다(링백 중·두 번째 18x·전환 안내 §3.5) — 프로파일과 무관하게 18x 는 같은 SDP 로
        // 나간다
        std::lock_guard<std::mutex> lock( m_mtx );
        auto it = m_mapCalls.find( strACallId );
        if ( it != m_mapCalls.end() && it->second.bEarlySent ) return true;
    }
    CCallInfo clsA;
    if ( !gclsCallMap.Select( strACallId.c_str(), clsA ) ) return false;
    CAnnAction a = Resolve( eSit, clsA.m_strAnnProfile );
    // 통화중대기(TS 24.615) 규칙이 없으면 일반 링백 규칙으로
    if ( eSit == ANN_SIT_CALL_WAITING && a.IsNone() ) a = Resolve( ANN_SIT_RINGBACK, clsA.m_strAnnProfile );
    if ( a.IsNone() || a.strMedia.empty() ) return false;
    if ( a.strMode != "media" && a.strMode != "announce" ) return false;
    // 가입자 링백(§6.3) — 피착신 가입자가 고른 음원. 스위치는 서비스 프로파일(none 이면 여기 오지 않는다), 음원은
    // 가입자
    if ( eSit == ANN_SIT_RINGBACK && !clsB.m_strRelayCallee.empty() ) {
        CspUser clsCallee;
        if ( gclsCspUserMap.Select( clsB.m_strRelayCallee.c_str(), clsCallee ) &&
             !clsCallee.m_strRingbackMedia.empty() )
            a.strMedia = clsCallee.m_strRingbackMedia;
    }
    CAnnCall c;
    c.strACallId = strACallId;
    c.strRelaySessionId = clsB.m_strRelaySessionId;
    c.strSesId = clsB.m_strRelaySesId;
    c.iPeerIdx = clsA.m_bRecv ? 0 : 1;
    c.eSit = eSit;
    c.iFinalStatus = 0;
    c.strCaller = clsB.m_strRelayCaller;
    c.strCallee = clsB.m_strRelayCallee;
    c.strProfile = clsA.m_strAnnProfile;
    const std::string strRelayIp =
        clsA.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress() : clsA.m_strRelayLocalIp;
    CSipCallRtp clsAns;
    RelayCodec::CodecDesc clsCodec;
    int iTePt = -1;
    if ( clsB.m_iPeerRtpPort <= 0 ||
         !BuildEarlyAnswer( strACallId.c_str(), clsA.m_clsSdesLeg[c.iPeerIdx], clsA.m_clsCodecLeg[c.iPeerIdx],
                            strRelayIp, clsB.m_iPeerRtpPort, clsAns, clsCodec, iTePt ) )
        return false;
    CAnnAction loop = a;
    loop.bLoop = true;
    if ( !StartFailurePlay( c, loop, &clsAns, clsCodec, iTePt, false ) ) return false;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        m_mapCalls[strACallId] = c;
        m_mapPlayToCall[c.strPlayId] = strACallId;
    }
    return true;
}

// ──────────────────────────────────────────────────────────────
//  착신전환 안내 (§3.5, TS 24.604) — B-leg 를 내기 직전, A 에 183+SDP + 전환 안내
// ──────────────────────────────────────────────────────────────

bool CCspAnnouncementService::OnForwarded( const char *pszACallId, const char *pszBCallId ) {
    if ( !IsEnabled() || pszACallId == NULL || pszBCallId == NULL ) return false;
    CCallInfo clsB, clsA;
    if ( !gclsCallMap.Select( pszBCallId, clsB ) || clsB.m_bRecv || clsB.m_strRelaySessionId.empty() ) return false;
    if ( !gclsCallMap.Select( pszACallId, clsA ) ) return false;
    const std::string strACallId = pszACallId;
    // A 에 이미 CSP answer 를 냈는가 — 원착신 B 의 링백(entry) 또는 B 의 18x+SDP(early media 앵커링). 그러면 183 을
    // 다시 내지
    //   않고 재생만 교체한다(조건부 전환 CFB/CFNR 이 이 경로다). 진행 중 재생은 교체(RELAY_PLAY replaced).
    bool bEarly = false;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        auto it = m_mapCalls.find( strACallId );
        if ( it != m_mapCalls.end() ) {
            if ( it->second.iFinalStatus > 0 ) return false;  // 실패 안내 진행 중 — 있을 수 없다
            bEarly = it->second.bEarlySent;
            if ( !it->second.strPlayId.empty() ) m_mapPlayToCall.erase( it->second.strPlayId );
            m_mapCalls.erase( it );
        }
    }
    if ( !bEarly ) {
        CSipCallRtp clsLocal;
        if ( gclsUserAgent.GetLocalCallRtp( strACallId.c_str(), &clsLocal ) && clsLocal.m_iPort > 0 ) bEarly = true;
    }
    CAnnAction a = Resolve( ANN_SIT_FORWARDED, clsA.m_strAnnProfile );
    if ( a.IsNone() ) return bEarly;
    if ( a.strMode == "media" && a.bLoop ) a.bLoop = false;  // 전환 안내는 1 회 — loop 는 2 단계(신호음/링백)가 한다
    CAnnCall c;
    c.strACallId = strACallId;
    c.strRelaySessionId = clsB.m_strRelaySessionId;
    c.strSesId = clsB.m_strRelaySesId;
    c.iPeerIdx = clsA.m_bRecv ? 0 : 1;
    c.eSit = ANN_SIT_FORWARDED;
    c.iFinalStatus = 0;
    c.strCaller = clsB.m_strRelayCaller;
    c.strCallee = clsB.m_strRelayCallee;
    c.strProfile = clsA.m_strAnnProfile;
    if ( a.strMode == "announce_then_tone" ) c.strNextTone = a.strTone;
    c.bEarlySent = bEarly;
    const std::string strRelayIp =
        clsA.m_strRelayLocalIp.empty() ? CspAddressing::GetLocalRtpAddress() : clsA.m_strRelayLocalIp;
    CSipCallRtp clsAns;
    RelayCodec::CodecDesc clsCodec;
    int iTePt = -1;
    if ( !bEarly &&
         ( clsB.m_iPeerRtpPort <= 0 ||
           !BuildEarlyAnswer( strACallId.c_str(), clsA.m_clsSdesLeg[c.iPeerIdx], clsA.m_clsCodecLeg[c.iPeerIdx],
                              strRelayIp, clsB.m_iPeerRtpPort, clsAns, clsCodec, iTePt ) ) ) {
        CLog::Print( LOG_INFO, "Announcement: cannot build early answer for forwarded call %s — 181 only",
                     strACallId.c_str() );
        ++m_lFallback;
        return false;
    }
    if ( !StartFailurePlay( c, a, bEarly ? NULL : &clsAns, clsCodec, iTePt, bEarly ) ) return bEarly;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        m_mapCalls[strACallId] = c;
        m_mapPlayToCall[c.strPlayId] = strACallId;
    }
    return true;
}

void CCspAnnouncementService::OnRingbackEnd( const char *pszBCallId, const CCallInfo &clsB ) {
    (void)pszBCallId;
    if ( clsB.m_strPeerCallId.empty() ) return;
    CAnnCall c;
    bool bFound = false;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        auto it = m_mapCalls.find( clsB.m_strPeerCallId );
        if ( it != m_mapCalls.end() && it->second.iFinalStatus == 0 ) {
            it->second.bPhase2 = false;  // 전환 안내 2 단계가 붙는 중이었다면 그쪽이 곧바로 걷는다
            if ( !it->second.strPlayId.empty() ) {
                c = it->second;
                bFound = true;
                m_mapPlayToCall.erase( c.strPlayId );
                it->second.strPlayId.clear();  // bEarlySent 표식은 남긴다 — 뒤따르는 실패 안내가 183 을 다시 내지 않게
            }
        }
    }
    if ( !bFound ) return;
    gclsCmpClient.StopAnnouncement( c.strRelaySessionId, c.iPeerIdx, c.strPlayId, c.strSesId, c.strService );
    CLog::Print( LOG_INFO, "Announcement: %s stopped (answer/early media from B) play=%s CallId=%s",
                 SituationName( c.eSit ), c.strPlayId.c_str(), c.strACallId.c_str() );
}

// ──────────────────────────────────────────────────────────────
//  통화중대기 in-band 대기음 (§3.6, TS 24.615) — 착신자 활성 leg 에 mode=mix
// ──────────────────────────────────────────────────────────────

bool CCspAnnouncementService::OnCallWaitingAlert( const std::string &strCallee, const char *pszCwACallId,
                                                  const char *pszCwBCallId ) {
    if ( !IsEnabled() || strCallee.empty() || pszCwACallId == NULL ) return false;
    CAnnAction a = Resolve( ANN_SIT_CALL_WAITING_ALERT, HoldProfileFor( strCallee ) );
    if ( a.IsNone() ) return false;
    const std::string strMedia = ( a.strMode == "tone" || a.strMode == "tone_then_announce" ) ? a.strTone : a.strMedia;
    if ( strMedia.empty() ) return false;
    std::string strLegCallId;
    CCallInfo clsLeg;
    int iPeerIdx = 0;
    if ( !gclsCallMap.FindEstablishedLegFor( strCallee, strLegCallId, clsLeg, iPeerIdx ) ) return false;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        if ( m_mapCw.count( pszCwACallId ) ) return true;  // 멱등
    }
    CHoldPlay h;
    h.strRelaySessionId = clsLeg.m_strRelaySessionId;
    h.strSesId = clsLeg.m_strRelaySesId;
    h.iPeerIdx = iPeerIdx;
    h.strHeldCallId = strLegCallId;
    h.strPlayId = NewPlayId( pszCwACallId ) + "-cw";
    std::vector<CCmpClient::AnnItem> vecItems( 1 );
    vecItems[0].strId = strMedia;
    vecItems[0].iRepeat = 1;
    int iDurationMs = 0;
    std::string strErr;
    if ( !gclsCmpClient.PlayAnnouncement( h.strRelaySessionId, h.iPeerIdx, h.strPlayId, vecItems, 0, 0, 0, h.strSesId,
                                          h.strService, iDurationMs, strErr, "mix" ) ) {
        CLog::Print( LOG_ERROR, "Announcement: call_waiting_alert RELAY_PLAY(mix) rejected (%s) callee=%s (CallId=%s)",
                     strErr.c_str(), strCallee.c_str(), pszCwACallId );
        ++m_lFallback;
        return false;
    }
    ++m_lStarted;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        m_mapCw[pszCwACallId] = h;
        if ( pszCwBCallId && pszCwBCallId[0] ) m_mapCw[pszCwBCallId] = h;
    }
    CLog::Print( LOG_SYSTEM, "Announcement: call_waiting_alert → mix %s on %s peer%d (callee=%s, waiting CallId=%s)",
                 strMedia.c_str(), h.strRelaySessionId.c_str(), h.iPeerIdx, strCallee.c_str(), pszCwACallId );
    return true;
}

void CCspAnnouncementService::OnCallWaitingEnd( const char *pszCallId ) {
    if ( pszCallId == NULL ) return;
    CHoldPlay h;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        auto it = m_mapCw.find( pszCallId );
        if ( it == m_mapCw.end() ) return;
        h = it->second;
        for ( auto i2 = m_mapCw.begin(); i2 != m_mapCw.end(); )
            if ( i2->second.strPlayId == h.strPlayId )
                i2 = m_mapCw.erase( i2 );
            else
                ++i2;
    }
    gclsCmpClient.StopAnnouncement( h.strRelaySessionId, h.iPeerIdx, h.strPlayId, h.strSesId, h.strService );
    CLog::Print( LOG_INFO, "Announcement: call_waiting_alert stopped play=%s (waiting CallId=%s)", h.strPlayId.c_str(),
                 pszCallId );
}

void CCspAnnouncementService::GetString( CMonitorString &strBuf ) const {
    size_t active = 0, hold = 0, cw = 0;
    {
        std::lock_guard<std::mutex> lock( m_mtx );
        active = m_mapCalls.size();
        hold = m_mapHold.size();
        cw = m_mapCw.size() / 2;
    }
    strBuf.AddCol( "ann_started" );
    strBuf.AddRow( (uint32_t)m_lStarted );
    strBuf.AddCol( "ann_fallback" );
    strBuf.AddRow( (uint32_t)m_lFallback );
    strBuf.AddCol( "ann_active_early" );
    strBuf.AddRow( (uint32_t)active );
    strBuf.AddCol( "ann_active_hold" );
    strBuf.AddRow( (uint32_t)hold );
    strBuf.AddCol( "ann_active_cw" );
    strBuf.AddRow( (uint32_t)cw );
}
