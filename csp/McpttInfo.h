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
