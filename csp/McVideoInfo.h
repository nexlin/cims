#ifndef _MCVIDEO_INFO_H_
#define _MCVIDEO_INFO_H_

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "McpttInfo.h"  // McpttElemValue(접두사·contentType 자식 무관 요소 값) · McpttXmlEsc · McpttBareId · McpttWarning 공용

// ── MCVideo 호 제어 경계 (TS 24.281 · TS 24.581) — docs/design/features/mcvideo.md §1·§5.2, 계약 K3·K4 ──
//  MCVideo 는 MCPTT 의 확장이 아니라 나란한 MC 서비스다. 서비스 표시(ICSI·특성 태그·info 본문)·제어 채널 SDP·Warning 은
//  서비스마다 따로 두고, 요소 값 추출 같은 구현은 mcptt-info 코덱(McpttInfo.h)을 그대로 쓴다. 골든 =
//  tests/fixtures/mcvideo/sip/(단위시험 tests/csp_mcvideo_info_test.cpp 가 그 파일을 직접 읽는다).

static const char *const kMcVideoIcsi = "urn:urn-7:3gpp-service.ims.icsi.mcvideo";         // TS 24.281 Annex E.2.1
static const char *const kMcVideoIcsiEnc = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcvideo";  // 특성 태그 값 표기
static const char *const kMcVideoFeatureTag = "+g.3gpp.mcvideo";                           // Annex D.2
static const char *const kMcVideoInfoSubtype = "vnd.3gpp.mcvideo-info+xml";                // Annex F.1 MIME
/** 제어 기능이 자기 Contact 에 싣는 특성 태그 — MCVideo 세션 식별자(URI 의 gr) + g.3gpp.mcvideo·icsi-ref·isfocus
 *  (TS 24.281 §6.3.3.1.2 1)·§9.2.2.4.1.1 19)). */
static const char *const kMcVideoFocusContactParams =
    "+g.3gpp.mcvideo;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcvideo\";isfocus";

struct CMcVideoInfo {
    bool bPresent = false;          // 본문에 <mcvideoinfo> 가 있다
    std::string strSessionType;     // chat | prearranged | private | broadcast … (Annex F.1.3)
    std::string strRequestUri;      // <mcvideo-request-uri> — 그룹 호의 대상 그룹 · affiliation 의 자기 MCVideo ID
    std::string strCallingUserId;   // <mcvideo-calling-user-id> (참여 기능이 넣는다)
    std::string strCalledPartyId;   // <mcvideo-called-party-id>
    std::string strCallingGroupId;  // <mcvideo-calling-group-id>
    std::string strClientId;        // <mcvideo-client-id>
    std::string strAccessToken;     // <mcvideo-access-token>
    bool bEmergency = false, bHasEmergencyInd = false;
    bool bImminent = false, bHasImminentInd = false;
    bool bAlert = false, bHasAlertInd = false;
    bool bBroadcast = false;  // <broadcast-ind>
};

inline bool _McVideoIndTrue( const std::string &body, const char *tag, bool &bHas ) {
    std::string v;
    bHas = McpttElemValue( body, tag, v );
    return bHas && ( v == "true" || v == "1" );
}

/** mcvideo-info 해석 — 루트 <mcvideoinfo>(스키마 이름, 본문 설명의 <mcvideo-info> 도 받는다 — mcvideo.md §9). multipart
 * 전체를 넘겨도 된다(요소 이름이 mcptt-info 와 겹치지 않는다: mcvideo-request-uri 등 — session-type·*-ind 는 겹치므로
 * 호출자는 mcvideo-info 파트만 넘기는 편이 안전하다. McVideoInfoPart 참고). */
inline CMcVideoInfo ParseMcVideoInfo( const std::string &body ) {
    CMcVideoInfo i;
    if ( body.empty() ) return i;
    std::string tmp;
    i.bPresent = McpttElemValue( body, "mcvideoinfo", tmp ) || McpttElemValue( body, "mcvideo-info", tmp ) ||
                 McpttElemValue( body, "mcvideo-Params", tmp );
    McpttElemValue( body, "session-type", i.strSessionType );
    McpttElemValue( body, "mcvideo-request-uri", i.strRequestUri );
    McpttElemValue( body, "mcvideo-calling-user-id", i.strCallingUserId );
    McpttElemValue( body, "mcvideo-called-party-id", i.strCalledPartyId );
    McpttElemValue( body, "mcvideo-calling-group-id", i.strCallingGroupId );
    McpttElemValue( body, "mcvideo-client-id", i.strClientId );
    McpttElemValue( body, "mcvideo-access-token", i.strAccessToken );
    i.bEmergency = _McVideoIndTrue( body, "emergency-ind", i.bHasEmergencyInd );
    i.bImminent = _McVideoIndTrue( body, "imminentperil-ind", i.bHasImminentInd );
    i.bAlert = _McVideoIndTrue( body, "alert-ind", i.bHasAlertInd );
    bool bHasBroadcast = false;
    i.bBroadcast = _McVideoIndTrue( body, "broadcast-ind", bHasBroadcast );
    return i;
}

/** multipart 본문에서 Content-Type 이 subtype(예 "vnd.3gpp.mcvideo-info+xml")인 파트 본문 — 없으면 "". 단일 본문이면
 *  bodyCtype 가 그 subtype 일 때 본문 전체. 경계 문자열은 헤더 파라미터가 아니라 본문의 첫 "--" 줄에서 읽는다. */
inline std::string McVideoBodyPart( const std::string &body, const std::string &bodyCtype, const char *subtype ) {
    if ( bodyCtype.find( subtype ) != std::string::npos ) return body;
    if ( !bodyCtype.empty() && bodyCtype.find( "multipart" ) == std::string::npos ) return "";
    const size_t b0 = body.find( "--" );
    if ( b0 == std::string::npos ) return "";
    const size_t e0 = body.find_first_of( "\r\n", b0 );
    if ( e0 == std::string::npos ) return "";
    const std::string boundary = body.substr( b0, e0 - b0 );
    for ( size_t p = b0; p != std::string::npos; ) {
        const size_t hs = p + boundary.size();
        if ( body.compare( hs, 2, "--" ) == 0 ) break;  // 닫는 경계
        size_t skip = 4;
        size_t he = body.find( "\r\n\r\n", hs );
        if ( he == std::string::npos ) {
            skip = 2;
            he = body.find( "\n\n", hs );
        }
        if ( he == std::string::npos ) break;
        const size_t next = body.find( boundary, he + skip );
        if ( body.substr( hs, he - hs ).find( subtype ) != std::string::npos ) {
            std::string part = body.substr( he + skip, ( next == std::string::npos ? body.size() : next ) - he - skip );
            while ( !part.empty() && ( part.back() == '\n' || part.back() == '\r' ) ) part.pop_back();
            return part;
        }
        p = next;
    }
    return "";
}

// ── mcvideo-info 생성 (Annex F.1 — contentType 요소는 암호화하지 않으면 type="Normal" + mcvideoURI/String/Boolean
// 자식) ──
inline std::string McVideoInfoUri( const char *tag, const std::string &uri ) {
    return std::string( "    <" ) + tag + " type=\"Normal\"><mcvideoURI>" + McpttXmlEsc( uri ) + "</mcvideoURI></" +
           tag + ">\r\n";
}
inline std::string McVideoInfoString( const char *tag, const std::string &v ) {
    return std::string( "    <" ) + tag + " type=\"Normal\"><mcvideoString>" + McpttXmlEsc( v ) + "</mcvideoString></" +
           tag + ">\r\n";
}
inline std::string McVideoInfoBool( const char *tag, bool v ) {
    return std::string( "    <" ) + tag + " type=\"Normal\"><mcvideoBoolean>" + ( v ? "true" : "false" ) +
           "</mcvideoBoolean></" + tag + ">\r\n";
}
inline std::string McVideoInfoValue( const char *tag, const std::string &v ) {
    return std::string( "    <" ) + tag + ">" + McpttXmlEsc( v ) + "</" + tag + ">\r\n";
}
/** mcvideo-info 문서 — <mcvideo-Params> 자식 원문을 감싼다(mcvideo-ParamsType 은 sequence — 호출자가 스키마 순서로
 * 부른다: access-token → session-type → request-uri → calling-user-id → called-party-id → calling-group-id → … →
 * client-id). */
inline std::string McVideoInfoDocument( const std::string &strParams ) {
    return std::string(
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
               "<mcvideoinfo xmlns=\"urn:3gpp:ns:mcvideoInfo:1.0\">\r\n"
               "  <mcvideo-Params>\r\n" ) +
           strParams +
           "  </mcvideo-Params>\r\n"
           "</mcvideoinfo>\r\n";
}

// ── 서비스 판별 ──

/** 헤더 값에 MCVideo ICSI 가 있는가 — P-Preferred-Service·P-Asserted-Service 는 원 표기, Accept-Contact·Contact 의
 *  icsi-ref 는 퍼센트 표기(RFC 3840)라 둘 다 본다. */
inline bool McVideoIcsiIn( const std::string &v ) {
    return v.find( kMcVideoIcsi ) != std::string::npos || v.find( kMcVideoIcsiEnc ) != std::string::npos;
}

/** 특성 태그 +g.3gpp.mcvideo 가 파라미터로 있는가(다른 태그의 접두사 일치는 아니다 — 뒤가 ';' ',' '=' 공백·끝). */
inline bool McVideoFeatureIn( const std::string &v ) {
    const std::string t = kMcVideoFeatureTag;
    for ( size_t p = v.find( t ); p != std::string::npos; p = v.find( t, p + 1 ) ) {
        const size_t e = p + t.size();
        if ( e >= v.size() || v[e] == ';' || v[e] == ',' || v[e] == '=' || v[e] == ' ' || v[e] == '>' || v[e] == '\r' ||
             v[e] == '\n' )
            return true;
    }
    return false;
}

/** 요청이 MCVideo 서비스의 것인가 — MCPTT 와 같은 메서드·Event(PUBLISH/SUBSCRIBE Event: presence, INVITE)를 쓰므로
 * 서비스 표시로 가른다(TS 24.281 §8.2.2.2.3 3)·§8.2.2.2.4 3) — P-Asserted-Service/P-Preferred-Service 의 MCVideo ICSI,
 * RFC 6050) · mcvideo-info 본문 (§8.2.2.2.3 2)) · pidf 의 mcvideoPresInfo 네임스페이스(§8.3.1) · Accept 의 mcvideo 문서
 * 형식. 하나라도 있으면 MCVideo. services = P-Asserted-Service 와 P-Preferred-Service 값을 이은 문자열, ctype =
 * "type/subtype". */
inline bool McVideoRequestIndicated( const std::string &services, const std::string &accept, const std::string &body,
                                     const std::string &ctype ) {
    if ( McVideoIcsiIn( services ) ) return true;
    if ( accept.find( "mcvideo" ) != std::string::npos ) return true;
    if ( !McVideoBodyPart( body, ctype, kMcVideoInfoSubtype ).empty() ) return true;
    return body.find( "urn:3gpp:ns:mcvideoPresInfo" ) != std::string::npos;
}

/** REGISTER Contact 가 MCVideo 클라이언트를 싣는가 — g.3gpp.mcvideo 와 icsi-ref 의 MCVideo ICSI 가 **둘 다**(TS 24.281
 * §7.2.1 1)·2)). 태그를 뺀 재-REGISTER 는 MCVideo 로그오프다(§7.2.1 NOTE 1). */
inline bool McVideoContactCapable( const std::string &contactParams ) {
    return McVideoFeatureIn( contactParams ) && McVideoIcsiIn( contactParams );
}

// ── 제어 채널 fmtp (TS 24.581 §12.1.2·§14 — 계약 K4, 이름 정본 = docs/design/features/mcvideo_tc_defs.yaml fmtp) ──

struct CMcVideoFmtp {
    bool bPresent = false;   // a=fmtp:MCVideo 줄이 있었다
    bool bQueueing = false;  // mc_queueing
    bool bGranted = false;   // mc_granted
    bool bImplicit = false;  // mc_implicit_request
    int iPriority = -1;      // mc_priority 1~255 (-1 = 없음)
    int iReceptionPriority = -1;
    bool bHasTcSsrc = false, bHasAudioSsrc = false, bHasVideoSsrc = false;
    uint32_t uTcSsrc = 0, uAudioSsrc = 0, uVideoSsrc = 0;
};

/** fmtp 파라미터 목록 해석 — "a=fmtp:MCVideo …" 줄 전체나 파라미터 부분 어느 쪽이든. 구분자는 ';'(§4.3.3.1 예시 —
 * K4)이고 ABNF 의 ':' 도 받는다(수신 관대 — 값에 ':' 가 없다). 모르는 파라미터는 무시. */
inline CMcVideoFmtp ParseMcVideoFmtp( const std::string &line ) {
    CMcVideoFmtp f;
    std::string s = line;
    const size_t k = s.find( "MCVideo" );
    if ( s.compare( 0, 2, "a=" ) == 0 || s.compare( 0, 5, "fmtp:" ) == 0 ) {
        if ( k == std::string::npos ) return f;
        s = s.substr( k + 7 );
    }
    f.bPresent = true;
    size_t p = 0;
    while ( p <= s.size() ) {
        size_t e = s.find_first_of( ";:", p );
        if ( e == std::string::npos ) e = s.size();
        std::string item = s.substr( p, e - p );
        const size_t a = item.find_first_not_of( " \t\r\n" );
        const size_t z = item.find_last_not_of( " \t\r\n" );
        item = a == std::string::npos ? "" : item.substr( a, z - a + 1 );
        const size_t eq = item.find( '=' );
        const std::string name = item.substr( 0, eq );
        const std::string val = eq == std::string::npos ? "" : item.substr( eq + 1 );
        const unsigned long ul = strtoul( val.c_str(), nullptr, 10 );
        if ( name == "mc_queueing" )
            f.bQueueing = true;
        else if ( name == "mc_granted" )
            f.bGranted = true;
        else if ( name == "mc_implicit_request" )
            f.bImplicit = true;
        else if ( name == "mc_priority" && !val.empty() )
            f.iPriority = (int)ul;
        else if ( name == "mc_reception_priority" && !val.empty() )
            f.iReceptionPriority = (int)ul;
        else if ( name == "mc_transmission_ssrc" && !val.empty() )
            f.bHasTcSsrc = true, f.uTcSsrc = (uint32_t)ul;
        else if ( name == "mc_audio_ssrc" && !val.empty() )
            f.bHasAudioSsrc = true, f.uAudioSsrc = (uint32_t)ul;
        else if ( name == "mc_video_ssrc" && !val.empty() )
            f.bHasVideoSsrc = true, f.uVideoSsrc = (uint32_t)ul;
        p = e + 1;
    }
    return f;
}

/** 제어 기능의 answer fmtp (TS 24.581 §14.3, 계약 K4 — mcvideo.md §1.4):
 *  - offer 에 있던 파라미터만(§14.3.1) — mc_audio_ssrc·mc_video_ssrc 는 answer 전용 예외(§14.3.7·§14.3.8·§14.4).
 *  - mc_priority = min(offer, <user-priority>)(§14.3.3 — 계층 수 요소는 off-network 전용이라 쓰지 않는다), 멤버
 * 우선순위가 음수면 offer 값.
 *  - mc_reception_priority = offer 값(§14.3.6 — <user-reception-priority> 를 두지 않는다).
 *  - mc_queueing 은 싣지 않는다(1차 송출 큐 없음 — §14.3.2 «지원할 때»).
 *  - 암묵 요청을 받아들였으면(bImplicitAccepted — 새 prearranged 세션 개시만, §14.3.5) mc_implicit_request + 송출 SSRC
 * 쌍, 허가됐고 offer 에 mc_granted 가 있었으면 mc_granted(§14.3.4).
 *  - mc_transmission_ssrc = CMP tc_ssrc 를 늘 싣는다(TS 24.281 §6.3.3.2.1 2)b)). */
inline std::string BuildMcVideoAnswerFmtp( const CMcVideoFmtp &offer, int iUserPriority, uint32_t uTcSsrc,
                                           bool bImplicitAccepted = false, bool bGranted = false,
                                           uint32_t uAudioSsrc = 0, uint32_t uVideoSsrc = 0 ) {
    std::string s;
    auto add = [&s]( const std::string &p ) {
        if ( !s.empty() ) s += ";";
        s += p;
    };
    if ( offer.iPriority >= 0 ) {
        int prio = offer.iPriority;
        if ( iUserPriority >= 0 && iUserPriority < prio ) prio = iUserPriority;
        add( "mc_priority=" + std::to_string( prio ) );
    }
    if ( offer.iReceptionPriority >= 0 ) add( "mc_reception_priority=" + std::to_string( offer.iReceptionPriority ) );
    if ( bImplicitAccepted && offer.bImplicit ) {
        if ( bGranted && offer.bGranted ) add( "mc_granted" );
        add( "mc_implicit_request" );
        add( "mc_audio_ssrc=" + std::to_string( uAudioSsrc ) );
        add( "mc_video_ssrc=" + std::to_string( uVideoSsrc ) );
    }
    add( "mc_transmission_ssrc=" + std::to_string( uTcSsrc ) );
    return s;
}

/** 제어 기능의 멤버 초대 offer fmtp (TS 24.281 §6.3.3.1.1 4)·5), TS 24.581 §14.2.3): mc_priority = 그룹
 * <user-priority>, mc_transmission_ssrc = CMP tc_ssrc. mc_granted·mc_implicit_request 는 싣지 않는다(초대받는 쪽은
 * 요청자가 아니다). */
inline std::string BuildMcVideoInviteFmtp( int iUserPriority, uint32_t uTcSsrc ) {
    std::string s;
    if ( iUserPriority >= 0 ) s = "mc_priority=" + std::to_string( iUserPriority ) + ";";
    return s + "mc_transmission_ssrc=" + std::to_string( uTcSsrc );
}

// ── Warning 문구 (TS 24.281 §4.4.2 표 4.4.2-2 — 형식은 McpttWarning: 399 <PTT 도메인> "NNN text") ──
static const char *const kMcVideoWarn100 = "function not allowed due to local policy";
static const char *const kMcVideoWarn101 = "service authorisation failed";
static const char *const kMcVideoWarn103 = "maximum simultaneous MCVideo group calls reached";
static const char *const kMcVideoWarn108 = "user not authorised to make chat group calls";
static const char *const kMcVideoWarn109 = "user not authorised to make prearranged group calls";
static const char *const kMcVideoWarn113 = "group document does not exist";
static const char *const kMcVideoWarn116 = "user is not part of the MCVideo group";
static const char *const kMcVideoWarn117 = "the group identity indicated in the request is a prearranged group";
static const char *const kMcVideoWarn118 = "the group identity indicated in the request is a chat group";
static const char *const kMcVideoWarn120 = "user is not affiliated to this group";
static const char *const kMcVideoWarn122 = "too many participants";
static const char *const kMcVideoWarn137 = "the indicated group call does not exist";

#endif  // _MCVIDEO_INFO_H_
