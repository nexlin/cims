#ifndef _MCPTT_INFO_H_
#define _MCPTT_INFO_H_

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// ── MCPTT call-control info 경량 파서 (application/vnd.3gpp.mcptt-info+xml, TS 24.379) ──
//  수신 INVITE/MESSAGE 의 multipart 바디에서 condition 지시자만 추출한다.
//  namespace prefix(mcpttinfo:/mcpttgi: 등) 무관하게 태그 substring 으로 매칭 — 외부 XML 파서 의존 없음.
//  emergency/imminent·broadcast 는 session-type(그룹 종류)과 직교하는 호 단위 표식이다.

struct CMcpttInfo {
    // session-type (TS 24.379 Annex F.1 의미 2) — chat|prearranged|private|first-to-answer|ambient-listening|adhoc
    std::string strSessionType;
    bool bBroadcast = false;  // <broadcast-ind>true</broadcast-ind> — 일제 통화 (TS 24.379 §6.2.8.2, §4.12)
    bool bEmergency = false;  // <emergency-ind>true</emergency-ind>
    bool bImminent = false;   // <imminentperil-ind>true</imminentperil-ind>
    bool bAlert = false;      // <alert-ind>true</alert-ind>
    // FloorTier 정합 condition: 2=emergency, 1=imminent, 0=normal
    int Condition() const {
        return bEmergency ? 2 : ( bImminent ? 1 : 0 );
    }
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
    info.bImminent = _McpttIndTrue( body, "imminentperil-ind" );
    info.bAlert = _McpttIndTrue( body, "alert-ind" );
    info.bBroadcast = _McpttIndTrue( body, "broadcast-ind" );
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
    bool bValid = false;        // 액션 요소를 하나라도 찾음
    bool bDeaffiliate = false;  // de-affiliate 액션
    std::string strGroup;       // group 속성값(있으면)
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

// ── per-user affiliation pidf 파싱 (application/pidf+xml, TS 24.379 §9.2.2.2.3 / §9.3.1.2) ──
//  규격형 제휴 PUBLISH(Event: presence)의 본문. 구형 affiliation-command 와 달리 **그 클라이언트의
//  제휴 그룹 집합 전체**를 싣는다 — 증분이 아니라 교체다(목록에 없는 기존 그룹은 해제 대상).
//
//  <presence xmlns="urn:ietf:params:xml:ns:pidf" entity="sip:user@domain">
//    <tuple id="<MCPTT client ID>">
//      <status>
//        <affiliation xmlns="urn:3gpp:ns:mcpttPresInfo:1.0" group="sip:g001@domain"/>
//        <affiliation group="sip:g002@domain"/>
//      </status>
//    </tuple>
//  </presence>
//
//  entity = MCPTT ID, tuple@id = MCPTT client ID, affiliation@group = 그룹 URI.
//  namespace prefix(mcpttPI10: 등) 무관 — 시작태그 이름을 경계까지 확인해 매칭한다.
struct CMcpttPidfAffiliation {
    bool bValid = false;                 // <presence> 루트를 찾음
    std::string strEntity;               // <presence entity="...">  = MCPTT ID
    std::string strClientId;             // <tuple id="...">         = MCPTT client ID
    std::vector<std::string> vecGroups;  // <affiliation group="..."> 전체 (원하는 제휴 집합)
};

/** prefix 무관 시작태그 검색 — "<name" 또는 "<ns:name". 반환 = '<' 위치(없으면 npos).
 *  종료태그("</name")와 접두가 겹치는 다른 이름("<affiliationX")은 배제한다. */
inline size_t _McpttFindStartTag( const std::string &body, size_t from, const char *name ) {
    const std::string n = name;
    size_t p = from;
    while ( ( p = body.find( n, p ) ) != std::string::npos ) {
        const size_t after = p + n.size();
        // 태그 이름의 끝 경계여야 한다 (속성 구분 공백 · '>' · 빈 요소 '/')
        if ( after < body.size() && !( body[after] == ' ' || body[after] == '>' || body[after] == '/' ||
                                       body[after] == '\t' || body[after] == '\r' || body[after] == '\n' ) ) {
            p = after;
            continue;
        }
        // 앞쪽은 '<' 이거나 "<prefix:" 여야 한다 (종료태그 '</' 는 배제된다)
        size_t q = p;
        while ( q > 0 && ( std::isalnum( (unsigned char)body[q - 1] ) || body[q - 1] == '_' || body[q - 1] == '-' ||
                           body[q - 1] == '.' || body[q - 1] == ':' ) )
            q--;
        if ( q > 0 && body[q - 1] == '<' ) return q - 1;
        p = after;
    }
    return std::string::npos;
}

/** 시작태그 구간(tagStart='<' 위치)에서 attr="value" 추출. attr 앞은 공백·뒤는 '=' 를 요구해
 *  접두가 겹치는 다른 속성(group vs groupStatus)을 오매칭하지 않는다. 작은따옴표도 수용. */
inline std::string _McpttTagAttr( const std::string &body, size_t tagStart, const char *attr ) {
    size_t gt = body.find( '>', tagStart );
    if ( gt == std::string::npos ) return "";
    const std::string tag = body.substr( tagStart, gt - tagStart );
    const std::string name = attr;
    size_t p = 0;
    while ( ( p = tag.find( name, p ) ) != std::string::npos ) {
        const bool bLeftOk =
            ( p > 0 && ( tag[p - 1] == ' ' || tag[p - 1] == '\t' || tag[p - 1] == '\r' || tag[p - 1] == '\n' ) );
        size_t q = p + name.size();
        while ( q < tag.size() && ( tag[q] == ' ' || tag[q] == '\t' ) ) q++;
        if ( bLeftOk && q < tag.size() && tag[q] == '=' ) {
            size_t q1 = tag.find_first_of( "\"'", q );
            if ( q1 == std::string::npos ) return "";
            const char cQuote = tag[q1];
            size_t q2 = tag.find( cQuote, q1 + 1 );
            if ( q2 == std::string::npos ) return "";
            return tag.substr( q1 + 1, q2 - q1 - 1 );
        }
        p += name.size();
    }
    return "";
}

inline CMcpttPidfAffiliation ParsePidfAffiliation( const std::string &body ) {
    CMcpttPidfAffiliation out;
    if ( body.empty() ) return out;
    const size_t pres = _McpttFindStartTag( body, 0, "presence" );
    if ( pres == std::string::npos ) return out;
    out.bValid = true;
    out.strEntity = _McpttTagAttr( body, pres, "entity" );
    const size_t tup = _McpttFindStartTag( body, pres, "tuple" );
    if ( tup != std::string::npos ) out.strClientId = _McpttTagAttr( body, tup, "id" );
    size_t p = pres;
    while ( ( p = _McpttFindStartTag( body, p, "affiliation" ) ) != std::string::npos ) {
        const std::string g = _McpttTagAttr( body, p, "group" );
        if ( !g.empty() && std::find( out.vecGroups.begin(), out.vecGroups.end(), g ) == out.vecGroups.end() )
            out.vecGroups.push_back( g );
        p += 12;  // strlen("affiliation") + 1 — 같은 태그 재매칭 방지
    }
    return out;
}

/** 그룹/사용자 URI 에서 식별자(user part)만 뽑는다 — "sip:g001@d"·"tel:+8250…"·"g001" 모두 수용. */
inline std::string McpttBareId( const std::string &uri ) {
    std::string s = uri;
    const size_t a = s.find_first_not_of( " \t\r\n<" );
    if ( a != std::string::npos ) s = s.substr( a );
    const size_t z = s.find_last_not_of( " \t\r\n>" );
    if ( z != std::string::npos ) s = s.substr( 0, z + 1 );
    const size_t c = s.find( ':' );
    if ( c != std::string::npos &&
         ( s.compare( 0, 4, "sip:" ) == 0 || s.compare( 0, 5, "sips:" ) == 0 || s.compare( 0, 4, "tel:" ) == 0 ) )
        s = s.substr( c + 1 );
    const size_t at = s.find( '@' );
    if ( at != std::string::npos ) s = s.substr( 0, at );
    const size_t sc = s.find( ';' );
    if ( sc != std::string::npos ) s = s.substr( 0, sc );
    return s;
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
