#ifndef _MCPTT_INFO_H_
#define _MCPTT_INFO_H_

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

// ── MCPTT call-control info 경량 파서 (application/vnd.3gpp.mcptt-info+xml, TS 24.379) ──
//  수신 INVITE/MESSAGE 의 multipart 바디에서 지시자·식별자를 추출하고(McpttElemValue — 두 인코딩), 송신 요소를
//  만든다(McpttInfo*). namespace prefix(mcpttinfo:/mcpttgi: 등) 무관하게 태그 substring 으로 매칭 — 외부 XML 파서 의존
//  없음. emergency/imminent·broadcast 는 session-type(그룹 종류)과 직교하는 호 단위 표식이다.

struct CMcpttInfo {
    // session-type (TS 24.379 Annex F.1 의미 2) — chat|prearranged|private|first-to-answer|ambient-listening|adhoc
    std::string strSessionType;
    bool bBroadcast = false;        // <broadcast-ind>true</broadcast-ind> — 일제 통화 (TS 24.379 §6.2.8.2, §4.12)
    bool bEmergency = false;        // <emergency-ind>true</emergency-ind>
    bool bImminent = false;         // <imminentperil-ind>true</imminentperil-ind>
    bool bAlert = false;            // <alert-ind>true</alert-ind>
    bool bHasAlertInd = false;      // <alert-ind> 요소 있음 (경보·경보 취소 판별 — TS 24.379 §12.1)
    bool bHasEmergencyInd = false;  // <emergency-ind> 요소 있음 (경보 취소에 동봉된 그룹 긴급 해제 판별 — §12.1.3.2)
    bool bHasImminentInd = false;   // <imminentperil-ind> 요소 있음 — 요소 없는 re-INVITE 는 조건 요청이 아니다
    std::string strRequestUri;      // <mcptt-request-uri> — 경보는 대상 그룹 (§12.1.1.1 4)a))
    std::string strCallingUserId;   // <mcptt-calling-user-id>
    std::string strOriginatedBy;    // <originated-by> — 제3자 경보 취소의 원 경보 발신자 (§12.1.3.2 2)a))
    std::string strClientId;        // <mcptt-client-id>
    // FloorTier 정합 condition: 2=emergency, 1=imminent, 0=normal
    int Condition() const {
        return bEmergency ? 2 : ( bImminent ? 1 : 0 );
    }
};

/**
 * mcptt-info 요소 값 — 접두사 무관, 태그 이름 경계까지 일치하는 첫 요소.
 *  TS 24.379 Annex F.1 의 contentType 요소(mcptt-request-uri·alert-ind 등)는 값을 자식 <mcpttURI>/<mcpttString>/
 *  <mcpttBoolean> 에 싣는다 — 그 형식과, 값을 요소에 바로 적는 형식(현행 단말·CSP 송신) 둘 다 읽는다.
 * @return 요소가 있으면 true (빈 요소면 out = "")
 */
inline bool McpttElemValue( const std::string &body, const char *tag, std::string &out ) {
    const std::string n = tag;
    size_t p = 0;
    while ( ( p = body.find( '<', p ) ) != std::string::npos ) {
        const size_t q = p + 1;
        if ( q < body.size() && ( body[q] == '/' || body[q] == '?' || body[q] == '!' ) ) {
            p = q;
            continue;
        }
        size_t e = q;
        while ( e < body.size() && body[e] != ' ' && body[e] != '\t' && body[e] != '\r' && body[e] != '\n' &&
                body[e] != '/' && body[e] != '>' )
            ++e;
        std::string name = body.substr( q, e - q );
        const size_t c = name.find( ':' );
        if ( c != std::string::npos ) name = name.substr( c + 1 );
        if ( name != n ) {
            p = e;
            continue;
        }
        const size_t gt = body.find( '>', e );
        if ( gt == std::string::npos ) return false;
        out.clear();
        if ( body[gt - 1] == '/' ) return true;
        size_t lt = body.find( '<', gt + 1 );
        if ( lt == std::string::npos ) return false;
        std::string val = body.substr( gt + 1, lt - gt - 1 );
        if ( lt + 1 < body.size() && body[lt + 1] != '/' ) {  // contentType 자식
            const size_t cgt = body.find( '>', lt );
            if ( cgt == std::string::npos ) return false;
            if ( body[cgt - 1] == '/' ) return true;
            const size_t clt = body.find( '<', cgt + 1 );
            if ( clt == std::string::npos ) return false;
            val = body.substr( cgt + 1, clt - cgt - 1 );
        }
        const size_t a = val.find_first_not_of( " \t\r\n" );
        const size_t b = val.find_last_not_of( " \t\r\n" );
        if ( a == std::string::npos ) return true;
        val = val.substr( a, b - a + 1 );
        // XML 기본 엔티티
        static const char *const kEnt[][2] = {
            { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" }, { "&amp;", "&" } };
        for ( const auto &ent : kEnt ) {
            size_t k = 0;
            while ( ( k = val.find( ent[0], k ) ) != std::string::npos ) {
                val.replace( k, strlen( ent[0] ), ent[1] );
                k += strlen( ent[1] );
            }
        }
        out = val;
        return true;
    }
    return false;
}

// ── mcptt-info 요소 생성 (TS 24.379 Annex F.1) ──
//  contentType 요소(mcptt-request-uri·mcptt-calling-user-id·mcptt-called-party-id·mcptt-calling-group-id·originated-by·
//  associated-group-id → <mcpttURI>, mcptt-access-token·mcptt-client-id → <mcpttString>, emergency-ind·alert-ind·
//  imminentperil-ind·alert-ind-rcvd → <mcpttBoolean>)는 암호화하지 않으면 type="Normal" + 자식 요소로 싣는다(F.1 의미
//  2)). session-type·broadcast-ind·mc-org 는 단순 값 요소다. mcptt-ParamsType 은 sequence 라 호출자가 스키마 순서로
//  부른다.
inline std::string McpttXmlEsc( const std::string &s ) {
    std::string o;
    o.reserve( s.size() );
    for ( char c : s ) {
        switch ( c ) {
            case '&':
                o += "&amp;";
                break;
            case '<':
                o += "&lt;";
                break;
            case '>':
                o += "&gt;";
                break;
            case '"':
                o += "&quot;";
                break;
            default:
                o += c;
        }
    }
    return o;
}
inline std::string McpttInfoUri( const char *tag, const std::string &uri ) {
    return std::string( "    <" ) + tag + " type=\"Normal\"><mcpttURI>" + McpttXmlEsc( uri ) + "</mcpttURI></" + tag +
           ">\r\n";
}
inline std::string McpttInfoString( const char *tag, const std::string &v ) {
    return std::string( "    <" ) + tag + " type=\"Normal\"><mcpttString>" + McpttXmlEsc( v ) + "</mcpttString></" +
           tag + ">\r\n";
}
inline std::string McpttInfoBool( const char *tag, bool v ) {
    return std::string( "    <" ) + tag + " type=\"Normal\"><mcpttBoolean>" + ( v ? "true" : "false" ) +
           "</mcpttBoolean></" + tag + ">\r\n";
}
inline std::string McpttInfoValue( const char *tag, const std::string &v ) {
    return std::string( "    <" ) + tag + ">" + McpttXmlEsc( v ) + "</" + tag + ">\r\n";
}

/** 조건 지시자 묶음 — 제어 기능이 보내는 mcptt-info(재광고 re-INVITE·상태 통지 MESSAGE·403·INFO)의 emergency-ind·
 *  alert-ind·imminentperil-ind·originated-by. -1 = 싣지 않음, 0 = false, 1 = true. */
struct McpttIndicators {
    int iEmergency = -1;
    int iAlert = -1;
    int iImminent = -1;
    std::string strOriginatedBy;  ///< 제3자 경보 취소의 원 경보 발신자 (§6.3.3.1.6 4)b)ii)B)·§12.1.3.2 2)c)iii))
};

/** 지시자 요소 — 스키마 순서(emergency-ind · alert-ind · imminentperil-ind). originated-by 는
 * McpttIndicatorOriginatedBy. */
inline std::string McpttIndicatorElems( const McpttIndicators &ind ) {
    std::string s;
    if ( ind.iEmergency >= 0 ) s += McpttInfoBool( "emergency-ind", ind.iEmergency > 0 );
    if ( ind.iAlert >= 0 ) s += McpttInfoBool( "alert-ind", ind.iAlert > 0 );
    if ( ind.iImminent >= 0 ) s += McpttInfoBool( "imminentperil-ind", ind.iImminent > 0 );
    return s;
}

/** originated-by 요소 — 스키마에서 broadcast-ind·mc-org 뒤. 값이 bare 면 tel: 을 붙인다. */
inline std::string McpttIndicatorOriginatedBy( const McpttIndicators &ind ) {
    if ( ind.strOriginatedBy.empty() ) return std::string();
    const std::string v =
        ind.strOriginatedBy.find( ':' ) == std::string::npos ? "tel:" + ind.strOriginatedBy : ind.strOriginatedBy;
    return McpttInfoUri( "originated-by", v );
}

/** mcptt-info 문서 — <mcptt-Params> 자식 원문을 감싼다. */
inline std::string McpttInfoDocument( const std::string &strParams ) {
    return std::string(
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
               "<mcpttinfo xmlns=\"urn:3gpp:ns:mcpttInfo:1.0\">\r\n"
               "  <mcptt-Params>\r\n" ) +
           strParams +
           "  </mcptt-Params>\r\n"
           "</mcpttinfo>\r\n";
}

/** MCPTT/MCData Warning 헤더 값 (TS 24.379 §4.4·TS 24.282 §4.4) — RFC 3261 §20.43 warning-value =
 *  warn-code(399 — 기타 경고) SP warn-agent SP warn-text, warn-text = DQUOTE mcptt-warn-code SP mcptt-warn-text DQUOTE.
 *  pszAgent = 경고를 붙이는 서버(hostport 또는 pseudonym). 비면 "cims". */
inline std::string McpttWarning( int iCode, const char *pszText, const std::string &strAgent = "" ) {
    char szCode[8];
    snprintf( szCode, sizeof( szCode ), "%03d", iCode );
    return std::string( "399 " ) + ( strAgent.empty() ? "cims" : strAgent ) + " \"" + szCode + " " + pszText + "\"";
}

// <tag> 값이 true/1 인지. tag 미존재 시 false.
inline bool _McpttIndTrue( const std::string &body, const char *tag ) {
    std::string val;
    if ( !McpttElemValue( body, tag, val ) ) return false;
    std::transform( val.begin(), val.end(), val.begin(), ::tolower );
    return val == "true" || val == "1";
}

inline CMcpttInfo ParseMcpttInfo( const std::string &body ) {
    CMcpttInfo info;
    if ( body.empty() ) return info;
    std::string strTmp;
    info.bEmergency = _McpttIndTrue( body, "emergency-ind" );
    info.bHasEmergencyInd = McpttElemValue( body, "emergency-ind", strTmp );
    info.bImminent = _McpttIndTrue( body, "imminentperil-ind" );
    info.bHasImminentInd = McpttElemValue( body, "imminentperil-ind", strTmp );
    info.bAlert = _McpttIndTrue( body, "alert-ind" );
    info.bHasAlertInd = McpttElemValue( body, "alert-ind", strTmp );
    info.bBroadcast = _McpttIndTrue( body, "broadcast-ind" );
    McpttElemValue( body, "session-type", info.strSessionType );
    McpttElemValue( body, "mcptt-request-uri", info.strRequestUri );
    McpttElemValue( body, "mcptt-calling-user-id", info.strCallingUserId );
    McpttElemValue( body, "originated-by", info.strOriginatedBy );
    McpttElemValue( body, "mcptt-client-id", info.strClientId );
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
    std::string strPid;                  // <presence><p-id>…</p-id> = 이 PUBLISH 의 식별자(선택, §9.3.1.2 4))
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
    const size_t pid = _McpttFindStartTag( body, pres, "p-id" );
    if ( pid != std::string::npos ) {
        const size_t gt = body.find( '>', pid );
        const size_t lt = ( gt == std::string::npos ) ? std::string::npos : body.find( '<', gt + 1 );
        if ( lt != std::string::npos && body[gt - 1] != '/' ) {
            std::string v = body.substr( gt + 1, lt - gt - 1 );
            const size_t a = v.find_first_not_of( " \t\r\n" );
            const size_t z = v.find_last_not_of( " \t\r\n" );
            out.strPid = ( a == std::string::npos ) ? std::string() : v.substr( a, z - a + 1 );
        }
    }
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

/** MCPTT ID(사용자) URI — 가입 번호 "+8250…" → "tel:+8250…". 이미 URI 면 그대로.
 *  CSC 토큰·user-profile 의 mcptt_id 와 같은 표기(tel:+msisdn)다. */
inline std::string McpttIdUri( const std::string &strUserId ) {
    if ( strUserId.compare( 0, 4, "tel:" ) == 0 || strUserId.compare( 0, 4, "sip:" ) == 0 ||
         strUserId.compare( 0, 5, "sips:" ) == 0 )
        return strUserId;
    return "tel:" + strUserId;
}

/** MCPTT group ID URI — CSC GMS/user-profile 의 그룹 URI 와 같은 규칙(csc services/mcptt.py `_group_uri`):
 *  '+' 로 시작하면 "tel:" 만, 숫자뿐이면 "tel:+", 그 외(g001 등)는 "tel:". 이미 URI 면 그대로. */
inline std::string McpttGroupUri( const std::string &strGroupId ) {
    if ( strGroupId.compare( 0, 4, "tel:" ) == 0 || strGroupId.compare( 0, 4, "sip:" ) == 0 ||
         strGroupId.compare( 0, 5, "sips:" ) == 0 )
        return strGroupId;
    if ( !strGroupId.empty() && strGroupId[0] == '+' ) return "tel:" + strGroupId;
    const bool bDigits = !strGroupId.empty() && std::all_of( strGroupId.begin(), strGroupId.end(),
                                                             []( unsigned char c ) { return std::isdigit( c ); } );
    return bDigits ? "tel:+" + strGroupId : "tel:" + strGroupId;
}

/** 참여 기능 PSI 로 온 개시 요청의 대상 후보 — 규격형 개시 INVITE 는 Request-URI = 원발 참여 MCPTT 기능의 PSI,
 *  대상 = mcptt-info <mcptt-request-uri>(그룹콜 TS 24.379 §10.1.1.2.1.1 1)·2), 개별 통화 §11.1.1.2.1.1).
 *  mcptt-request-uri 의 식별자가 Request-URI user 와 다르면 그것을 돌려준다(없거나 같으면 빈 값 — Request-URI 에
 *  대상을 직접 싣는 구형 단말). Request-URI 가 그룹·가입자로 알려진 식별자인지는 호출자가 판정한다. */
inline std::string McpttPsiTarget( const std::string &strRuriUser, const std::string &strRequestUri ) {
    if ( strRequestUri.empty() ) return "";
    const std::string t = McpttBareId( strRequestUri );
    return ( t.empty() || t == strRuriUser ) ? std::string() : t;
}

// ── 제휴 상태 NOTIFY 본문 — per-user affiliation information (TS 24.379 §9.3.1.2 첫 목록, §9.2.2.2.5 3)) ──
//
//  <presence xmlns="urn:ietf:params:xml:ns:pidf" xmlns:mcpttPI10="urn:3gpp:ns:mcpttPresInfo:1.0" entity="<MCPTT ID>">
//    <tuple id="<MCPTT client ID>">                       ← 클라이언트마다 하나
//      <status>
//        <mcpttPI10:affiliation group="<MCPTT group ID>" status="affiliated" expires="<xs:dateTime>"/>
//      </status>
//    </tuple>
//    <mcpttPI10:p-id>…</mcpttPI10:p-id>                   ← 이 NOTIFY 를 부른 PUBLISH 의 p-id (있을 때만)
//  </presence>
//
//  p-id 는 pidf 확장 요소라 RFC 3863 스키마상 tuple·note 뒤(##other)에 둔다. 제휴 그룹이 없는 클라이언트는
//  tuple 을 싣지 않는다(§9.2.2.2.5 3) a)·b) — 만료·deaffiliated 항목 제외).
struct CMcpttAffGroup {
    std::string strGroupUri;  // MCPTT group ID (McpttGroupUri)
    std::string strExpires;   // xs:dateTime (예 2026-09-30T05:00:00Z). 비면 속성 생략(만료 없음)
};
struct CMcpttAffClient {
    std::string strClientId;  // MCPTT client ID (규격형 PUBLISH 의 tuple@id, 구형 PUBLISH 는 Contact URI)
    std::vector<CMcpttAffGroup> vecGroups;
};

inline std::string BuildPidfAffiliationInfo( const std::string &strEntity,
                                             const std::vector<CMcpttAffClient> &vecClients,
                                             const std::string &strPid ) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    s += "<presence xmlns=\"urn:ietf:params:xml:ns:pidf\" xmlns:mcpttPI10=\"urn:3gpp:ns:mcpttPresInfo:1.0\" entity=\"" +
         McpttXmlEsc( strEntity ) + "\">\r\n";
    for ( const auto &c : vecClients ) {
        if ( c.vecGroups.empty() ) continue;
        s += "  <tuple id=\"" + McpttXmlEsc( c.strClientId ) + "\">\r\n";
        s += "    <status>\r\n";
        for ( const auto &g : c.vecGroups ) {
            s += "      <mcpttPI10:affiliation group=\"" + McpttXmlEsc( g.strGroupUri ) + "\" status=\"affiliated\"";
            if ( !g.strExpires.empty() ) s += " expires=\"" + McpttXmlEsc( g.strExpires ) + "\"";
            s += "/>\r\n";
        }
        s += "    </status>\r\n";
        s += "  </tuple>\r\n";
    }
    if ( !strPid.empty() ) s += "  <mcpttPI10:p-id>" + McpttXmlEsc( strPid ) + "</mcpttPI10:p-id>\r\n";
    s += "</presence>\r\n";
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
