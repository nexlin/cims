#ifndef _MCPTT_INFO_H_
#define _MCPTT_INFO_H_

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// ── MCPTT call-control info 경량 파서 (application/vnd.3gpp.mcptt-info+xml, TS 24.379) ──
//  수신 INVITE/MESSAGE 의 multipart 바디에서 condition 지시자만 추출한다.
//  namespace prefix(mcpttinfo:/mcpttgi: 등) 무관하게 태그 substring 으로 매칭 — 외부 XML 파서 의존 없음.
//  emergency/imminent 는 prearranged/chat/broadcast(session-type)와 직교하는 런타임 조건.

struct CMcpttInfo {
    std::string strSessionType;          // prearranged|chat|broadcast (선택)
    bool bEmergency = false;             // <emergency-ind>true</emergency-ind>
    bool bImminent  = false;             // <imminentperil-ind>true</imminentperil-ind>
    bool bAlert     = false;             // <alert-ind>true</alert-ind>
    // FloorTier 정합 condition: 2=emergency, 1=imminent, 0=normal
    int Condition() const { return bEmergency ? 2 : ( bImminent ? 1 : 0 ); }
};

// <...tag...>VALUE</...> 에서 VALUE 가 true/1 인지. tag 미존재 시 false.
inline bool _McpttIndTrue( const std::string &body, const char *tag ) {
    size_t p = body.find( tag );
    if ( p == std::string::npos ) return false;
    size_t gt = body.find( '>', p );
    if ( gt == std::string::npos ) return false;
    size_t lt = body.find( '<', gt );
    std::string val = body.substr( gt + 1, ( lt == std::string::npos ? body.size() : lt ) - ( gt + 1 ) );
    size_t a = val.find_first_not_of( " \t\r\n" );
    size_t b = val.find_last_not_of( " \t\r\n" );
    if ( a == std::string::npos ) return false;
    val = val.substr( a, b - a + 1 );
    std::transform( val.begin(), val.end(), val.begin(), ::tolower );
    return val == "true" || val == "1";
}

inline CMcpttInfo ParseMcpttInfo( const std::string &body ) {
    CMcpttInfo info;
    if ( body.empty() ) return info;
    info.bEmergency = _McpttIndTrue( body, "emergency-ind" );
    info.bImminent  = _McpttIndTrue( body, "imminentperil-ind" );
    info.bAlert     = _McpttIndTrue( body, "alert-ind" );
    size_t p = body.find( "session-type" );
    if ( p != std::string::npos ) {
        size_t gt = body.find( '>', p );
        size_t lt = ( gt != std::string::npos ) ? body.find( '<', gt ) : std::string::npos;
        if ( gt != std::string::npos && lt != std::string::npos )
            info.strSessionType = body.substr( gt + 1, lt - gt - 1 );
    }
    return info;
}

// ── affiliation-command 파싱 (application/vnd.3gpp.mcptt-affiliation-command+xml, TS 24.379 §9) ──
//  <affiliation-command xmlns="urn:3gpp:ns:mcpttAffiliation:1.0">
//    <actions> <affiliate group="sip:g@d"/> | <de-affiliate group="sip:g@d"/> </actions>
//  </affiliation-command>
//  액션 요소(시작태그 '<' 앵커)로 affiliate/de-affiliate 를 판정하고 group 속성을 추출한다.
//  단순 텍스트 substring 이 아니라 **요소 기반** 판정(group 속성값에 "de-affiliate" 가 들어 있어도
//  '<' 앵커라 오판 없음). 단말 McpttXml.affiliationCommand 와 정합. namespace prefix 무관.
struct CMcpttAffiliation {
    bool bValid       = false;   // 액션 요소를 하나라도 찾음
    bool bDeaffiliate = false;   // de-affiliate 액션
    std::string strGroup;        // group 속성값(있으면)
};

// 시작태그(예: "affiliate") 의 group="..." 속성 추출.
inline std::string _McpttTagGroupAttr( const std::string &body, size_t tagStart ) {
    size_t gt = body.find( '>', tagStart );
    if ( gt == std::string::npos ) return "";
    std::string tag = body.substr( tagStart, gt - tagStart );
    size_t g = tag.find( "group" );
    if ( g == std::string::npos ) return "";
    size_t q1 = tag.find( '"', g );
    if ( q1 == std::string::npos ) return "";
    size_t q2 = tag.find( '"', q1 + 1 );
    if ( q2 == std::string::npos ) return "";
    return tag.substr( q1 + 1, q2 - q1 - 1 );
}

inline CMcpttAffiliation ParseAffiliationCommand( const std::string &body ) {
    CMcpttAffiliation out;
    if ( body.empty() ) return out;
    // <actions> 구간으로 한정(없으면 본문 전체).
    size_t scan = body.find( "<actions" );
    if ( scan == std::string::npos ) scan = 0;
    size_t deaff = body.find( "<de-affiliate", scan );
    if ( deaff == std::string::npos ) deaff = body.find( "<deaffiliate", scan );
    size_t aff = body.find( "<affiliate", scan );  // '<affiliate' 는 '<de-affiliate' 에 매칭 안 됨
    if ( deaff != std::string::npos ) {
        out.bValid = true;
        out.bDeaffiliate = true;
        out.strGroup = _McpttTagGroupAttr( body, deaff );
    } else if ( aff != std::string::npos ) {
        out.bValid = true;
        out.bDeaffiliate = false;
        out.strGroup = _McpttTagGroupAttr( body, aff );
    }
    return out;
}

// 멀티파트 바디의 resource-lists+xml part 에서 멤버 식별자(tel: 뒤 숫자/+) 추출.
//  ad hoc 그룹콜(TS 22.179 Rel-18): 개시자가 INVITE 에 동적 멤버 목록을 실어 보냄.
//  mcptt-info part 의 tel: 는 제외(resource-lists 구간만 스캔).
inline std::vector<std::string> ParseResourceListUsers( const std::string &body ) {
    std::vector<std::string> out;
    size_t rl = body.find( "resource-lists" );
    if ( rl == std::string::npos ) return out;
    size_t end = body.find( "\r\n--", rl );  // resource-lists part 끝(다음 boundary)
    std::string seg = body.substr( rl, ( end == std::string::npos ? body.size() : end ) - rl );
    size_t p = 0;
    while ( ( p = seg.find( "tel:", p ) ) != std::string::npos ) {
        p += 4;
        size_t e = p;
        while ( e < seg.size() && ( std::isdigit( (unsigned char)seg[e] ) || seg[e] == '+' ) ) e++;
        if ( e > p ) {
            std::string id = seg.substr( p, e - p );
            if ( std::find( out.begin(), out.end(), id ) == out.end() ) out.push_back( id );
        }
        p = e;
    }
    return out;
}

#endif  // _MCPTT_INFO_H_
