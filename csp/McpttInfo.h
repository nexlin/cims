#ifndef _MCPTT_INFO_H_
#define _MCPTT_INFO_H_

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <string>
#include <vector>

// ── MCPTT call-control info 경량 파서 (application/vnd.3gpp.mcptt-info+xml, TS 24.379) ──
//  수신 INVITE/MESSAGE 의 multipart 바디에서 지시자·식별자를 추출하고(McpttElemValue — 두 인코딩), 송신 요소를
//  만든다(McpttInfo*). namespace prefix(mcpttinfo:/mcpttgi: 등) 무관하게 태그 substring 으로 매칭 — 외부 XML 파서 의존
//  없음. emergency/imminent·broadcast 는 session-type(그룹 종류)과 직교하는 호 단위 표식이다.

// ── Supported 옵션 태그 — MCPTT·MCVideo 공통(두 규격의 절 번호가 같다) ──
// 제어 기능의 200 OK (TS 24.379·24.281 §6.3.3.2.3.2 8)~10)) — tdialog(RFC 4538)·norefersub(RFC 4488)·explicitsub·nosub
//   (RFC 7614). timer 는 Require 로 스택이 싣는다(3)).
static const char *const kMcFocusOkSupported = "tdialog, norefersub, explicitsub, nosub";
// 참여 기능이 단말에 보내는 INVITE (§6.3.2.2.3 5)·6)) — timer(3))는 스택이 Supported 줄 하나로 따로 싣는다.
static const char *const kMcMemberInviteSupported = "tdialog, norefersub";

// ── 서비스 표시 — ICSI·특성 태그 (TS 24.379 Annex D·E · TS 24.282 Annex D — RFC 3840/3841, RFC 6050) ──
static const char *const kMcpttIcsi = "urn:urn-7:3gpp-service.ims.icsi.mcptt";
static const char *const kMcpttIcsiEnc = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt";  // 특성 태그 값 표기
static const char *const kMcDataSdsIcsi = "urn:urn-7:3gpp-service.ims.icsi.mcdata.sds";
static const char *const kMcDataSdsIcsiEnc = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds";
static const char *const kMcDataFdIcsi = "urn:urn-7:3gpp-service.ims.icsi.mcdata.fd";
static const char *const kMcDataFdIcsiEnc = "urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.fd";

/** 헤더 값에 특성 태그(예 "+g.3gpp.mcptt")가 파라미터로 있는가 — 다른 태그의 접두사 일치는 아니다(뒤가 ';' ',' '='
 *  공백·'>'·줄 끝·끝). */
inline bool McFeatureTagIn( const std::string &v, const char *tag ) {
    const std::string t = tag;
    for ( size_t p = v.find( t ); p != std::string::npos; p = v.find( t, p + 1 ) ) {
        const size_t e = p + t.size();
        if ( e >= v.size() || v[e] == ';' || v[e] == ',' || v[e] == '=' || v[e] == ' ' || v[e] == '>' || v[e] == '\r' ||
             v[e] == '\n' )
            return true;
    }
    return false;
}

/** 헤더 값에 ICSI 가 있는가 — P-Preferred-Service 는 원 표기, Accept-Contact·Contact 의 icsi-ref 는 퍼센트 표기(RFC
 * 3840)라 둘 다 본다. 더 긴 ICSI 의 접두사 일치(예 mcdata 와 mcdata.sds)는 아니다. */
inline bool McIcsiIn( const std::string &v, const char *icsi, const char *icsiEnc ) {
    for ( const char *t : { icsi, icsiEnc } ) {
        const size_t n = strlen( t );
        for ( size_t p = v.find( t ); p != std::string::npos; p = v.find( t, p + 1 ) ) {
            const char c = p + n < v.size() ? v[p + n] : '\0';
            if ( !std::isalnum( (unsigned char)c ) && c != '.' && c != '-' && c != '_' ) return true;
        }
    }
    return false;
}

/** REGISTER Contact 의 MCPTT 특성 태그 둘(g.3gpp.mcptt · MCPTT icsi-ref — TS 24.379 §7.2.1 · §7.2.1AA). 빼고 다시
 * 등록하면 MCPTT 로그오프다(§7.1 · §7.2.1 NOTE 1). contactParams = Contact 헤더 파라미터를 이은 문자열. */
inline bool McpttContactCapable( const std::string &contactParams ) {
    return McFeatureTagIn( contactParams, "+g.3gpp.mcptt" ) && McIcsiIn( contactParams, kMcpttIcsi, kMcpttIcsiEnc );
}

/** 그룹 호 제어 기능의 Accept-Contact 검사 — `g.3gpp.mcptt` 특성 태그와 MCPTT icsi-ref 가 둘 다 있어야 한다. 없으면 403
 *  (TS 24.379 §10.1.1.4.2 3) 편성 개시·합류 · §10.1.1.4.5.1 4) 재합류 · §10.1.2.4.1.1 2)a)b) chat · §17.4.2.2 3) 애드혹
 * 개시 · §17.4.4.1.1 4) 애드혹 재합류). acceptContacts = Accept-Contact 헤더 값들을 이은 문자열. */
inline bool McpttAcceptContactOk( const std::string &acceptContacts ) {
    return McFeatureTagIn( acceptContacts, "+g.3gpp.mcptt" ) && McIcsiIn( acceptContacts, kMcpttIcsi, kMcpttIcsiEnc );
}

/** 미디어 평면 SDS INVITE 의 Accept-Contact 검사 — `g.3gpp.mcdata.sds` 특성 태그와 SDS icsi-ref 가 둘 다 있어야 한다.
 * 없으면 403 (TS 24.282 §9.2.3.4.4 3)). */
inline bool McDataSdsAcceptContactOk( const std::string &acceptContacts ) {
    return McFeatureTagIn( acceptContacts, "+g.3gpp.mcdata.sds" ) &&
           McIcsiIn( acceptContacts, kMcDataSdsIcsi, kMcDataSdsIcsiEnc );
}

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

/** MCPTT/MCData Warning 헤더 값 (TS 24.379 §4.4·TS 24.282 §4.9) — RFC 3261 §20.43 warning-value =
 *  warn-code(399 — 기타 경고) SP warn-agent SP warn-text, warn-text = DQUOTE mcptt-warn-code SP mcptt-warn-text DQUOTE.
 *  pszAgent = 경고를 붙이는 서버(hostport 또는 pseudonym). 비면 "cims". */
inline std::string McpttWarning( int iCode, const char *pszText, const std::string &strAgent = "" ) {
    char szCode[8];
    snprintf( szCode, sizeof( szCode ), "%03d", iCode );
    return std::string( "399 " ) + ( strAgent.empty() ? "cims" : strAgent ) + " \"" + szCode + " " + pszText + "\"";
}

/** multipart 본문에서 Content-Type 이 subtype(예 "vnd.3gpp.mcptt-info+xml")인 파트 본문 — 없으면 "". 단일 본문이면
 *  bodyCtype 가 그 subtype 일 때 본문 전체. 경계 문자열은 헤더 파라미터가 아니라 본문의 첫 "--" 줄에서 읽는다. */
inline std::string McBodyPart( const std::string &body, const std::string &bodyCtype, const char *subtype ) {
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

/** floor 우선순위 협상값 (TS 24.380 §14.3.3 2)a)) = min(offer mc_priority, <user-priority>,
 * <num-levels-priority-hierarchy>). iOffered <= 0(offer 에 없음 — 미협상)이면 0. */
inline int McpttNegotiatedFloorPriority( int iOffered, int iUserPriority, int iLevels ) {
    if ( iOffered <= 0 ) return 0;
    return std::min( { iOffered, iUserPriority, iLevels } );
}

/** 초대 대상을 정원 안으로 (TS 24.379 §6.3.5.5) — iSlots(개시자를 뺀 자리)를 넘으면 필수 멤버(<on-network-required>)를
 *  먼저 두고 나머지는 받은 순서대로 자른다. 잘랐으면 bCapped. iSlots < 0 = 상한 없음. */
inline std::vector<std::string> McpttCapInvitees( const std::vector<std::string> &vecIn,
                                                  const std::set<std::string> &setRequired, int iSlots,
                                                  bool &bCapped ) {
    bCapped = false;
    if ( iSlots < 0 || (int)vecIn.size() <= iSlots ) return vecIn;
    std::vector<std::string> vecOut;
    for ( const auto &m : vecIn )
        if ( (int)vecOut.size() < iSlots && setRequired.count( m ) ) vecOut.push_back( m );
    for ( const auto &m : vecIn )
        if ( (int)vecOut.size() < iSlots && !setRequired.count( m ) ) vecOut.push_back( m );
    bCapped = true;
    return vecOut;
}

/** 긴급 경보 MESSAGE 의 서비스 — McEmergencyAlertServiceOf. */
enum class EMcAlertService { None, Mcptt, McData };

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

/** 긴급 경보 MESSAGE 의 서비스 (TS 24.379 §12.1 · TS 24.282 §16.2) — 지시자는 서비스마다 그 info 문서 안에 있다.
 *  mcdata-info 파트가 있으면 MCData 요청이다: <alert-ind> 가 있으면 MCData 경보, 없으면 경보가 아니다(SDS·FD). MCData
 *  본문은 MCPTT 경보로 읽지 않는다(같은 요소 이름 <alert-ind> 를 쓴다). 그 밖은 mcptt-info 의 <alert-ind>(경보·경보
 *  취소) 또는 <emergency-ind>false(호 없는 그룹 긴급 상태 해제, §12.1.3.3)면 MCPTT 경보 — 지시자는 mcptt-info 파트
 *  (application/vnd.3gpp.mcptt-info+xml)에서만 읽는다. ctype = "type/subtype". */
inline EMcAlertService McEmergencyAlertServiceOf( const std::string &body, const std::string &ctype ) {
    const std::string strMcData = McBodyPart( body, ctype, "vnd.3gpp.mcdata-info+xml" );
    if ( !strMcData.empty() ) {
        std::string v;
        return McpttElemValue( strMcData, "alert-ind", v ) ? EMcAlertService::McData : EMcAlertService::None;
    }
    const CMcpttInfo mi = ParseMcpttInfo( McBodyPart( body, ctype, "vnd.3gpp.mcptt-info+xml" ) );
    return ( mi.bHasAlertInd || ( mi.bHasEmergencyInd && !mi.bEmergency ) ) ? EMcAlertService::Mcptt
                                                                            : EMcAlertService::None;
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
        // 이 시작태그 뒤로 — p 는 '<' 위치라 고정 길이만큼 옮기면 접두사가 긴 태그(<mcvideoPI10:affiliation …>)에서
        // 같은 태그를
        //   다시 찾아 끝나지 않는다. 태그의 '>' 다음부터 찾는다.
        const size_t gt = body.find( '>', p );
        if ( gt == std::string::npos ) break;
        p = gt + 1;
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

// bMcVideo = MCVideo 서비스의 제휴 상태(TS 24.281 §8.2.2.2.5 · §8.3.1) — 같은 모양에 네임스페이스만 mcvideoPresInfo 다.
inline std::string BuildPidfAffiliationInfo( const std::string &strEntity,
                                             const std::vector<CMcpttAffClient> &vecClients, const std::string &strPid,
                                             bool bMcVideo = false ) {
    const std::string pfx = bMcVideo ? "mcvideoPI10" : "mcpttPI10";
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
    s += "<presence xmlns=\"urn:ietf:params:xml:ns:pidf\" xmlns:" + pfx +
         "=\"urn:3gpp:ns:" + ( bMcVideo ? "mcvideoPresInfo" : "mcpttPresInfo" ) + ":1.0\" entity=\"" +
         McpttXmlEsc( strEntity ) + "\">\r\n";
    for ( const auto &c : vecClients ) {
        if ( c.vecGroups.empty() ) continue;
        s += "  <tuple id=\"" + McpttXmlEsc( c.strClientId ) + "\">\r\n";
        s += "    <status>\r\n";
        for ( const auto &g : c.vecGroups ) {
            s += "      <" + pfx + ":affiliation group=\"" + McpttXmlEsc( g.strGroupUri ) + "\" status=\"affiliated\"";
            if ( !g.strExpires.empty() ) s += " expires=\"" + McpttXmlEsc( g.strExpires ) + "\"";
            s += "/>\r\n";
        }
        s += "    </status>\r\n";
        s += "  </tuple>\r\n";
    }
    if ( !strPid.empty() ) s += "  <" + pfx + ":p-id>" + McpttXmlEsc( strPid ) + "</" + pfx + ":p-id>\r\n";
    s += "</presence>\r\n";
    return s;
}

/** resource-lists 항목 하나 (RFC 4826 `<entry uri>`) — MCPTT ID(맨 값, McpttBareId) + SIP URI 파라미터 `method`
 *  (애드혹 참가자 변경 re-INVITE 의 INVITE·BYE — TS 24.379 §17.4.5.1.1 2)·3)). */
struct McpttListEntry {
    std::string strId;
    std::string strMethod;  ///< 대문자 (INVITE·BYE) — 없으면 빈 값
};

// 멀티파트 바디의 resource-lists+xml part 에서 `<entry uri="…">` 를 모두 읽는다 — 애드혹 그룹 호의 초대 명단(TS 24.379
//  §17.4.2.2 12)a) «each entry»)과 참가자 변경(§17.4.5.1.1). uri 는 tel:·sip: 어느 형이든 MCPTT ID 맨 값으로
//  줄이고(같은 ID 는 한 번), `<entry-ref>`·mcptt-info part 의 URI 는 읽지 않는다.
inline std::vector<McpttListEntry> ParseResourceListEntries( const std::string &body ) {
    std::vector<McpttListEntry> out;
    const size_t rl = body.find( "resource-lists" );
    if ( rl == std::string::npos ) return out;
    const size_t end = body.find( "\r\n--", rl );  // resource-lists part 끝(다음 boundary)
    const std::string seg = body.substr( rl, ( end == std::string::npos ? body.size() : end ) - rl );
    size_t p = 0;
    while ( ( p = seg.find( "entry", p ) ) != std::string::npos ) {
        const size_t q = p + 5;
        const bool bTag = p > 0 && ( seg[p - 1] == '<' || seg[p - 1] == ':' ) && q < seg.size() &&
                          ( std::isspace( (unsigned char)seg[q] ) || seg[q] == '/' || seg[q] == '>' );
        if ( !bTag ) {
            p = q;
            continue;
        }
        const size_t gt = seg.find( '>', q );
        const std::string tag = seg.substr( q, ( gt == std::string::npos ? seg.size() : gt ) - q );
        p = ( gt == std::string::npos ) ? seg.size() : gt;
        size_t u = tag.find( "uri" );
        while ( u != std::string::npos && ( u + 3 >= tag.size() || ( tag[u + 3] != '=' && tag[u + 3] != ' ' ) ) )
            u = tag.find( "uri", u + 3 );
        if ( u == std::string::npos ) continue;
        const size_t qs = tag.find_first_of( "\"'", u );
        if ( qs == std::string::npos ) continue;
        const size_t qe = tag.find( tag[qs], qs + 1 );
        if ( qe == std::string::npos ) continue;
        std::string uri = tag.substr( qs + 1, qe - qs - 1 );
        for ( size_t a; ( a = uri.find( "&amp;" ) ) != std::string::npos; ) uri.replace( a, 5, "&" );
        McpttListEntry e;
        std::string low = uri;
        std::transform( low.begin(), low.end(), low.begin(), ::tolower );
        const size_t m = low.find( ";method=" );
        if ( m != std::string::npos ) {
            const size_t v = m + 8;
            const size_t ve = low.find_first_of( ";?&", v );
            e.strMethod = uri.substr( v, ( ve == std::string::npos ? uri.size() : ve ) - v );
            std::transform( e.strMethod.begin(), e.strMethod.end(), e.strMethod.begin(), ::toupper );
        }
        e.strId = McpttBareId( uri );
        if ( e.strId.empty() ) continue;
        bool bDup = false;
        for ( const auto &x : out ) bDup = bDup || x.strId == e.strId;
        if ( !bDup ) out.push_back( e );
    }
    return out;
}

// 애드혹 그룹 호 개시 INVITE 의 초대 명단 — resource-lists 항목의 MCPTT ID (ParseResourceListEntries).
inline std::vector<std::string> ParseResourceListUsers( const std::string &body ) {
    std::vector<std::string> out;
    for ( const auto &e : ParseResourceListEntries( body ) ) out.push_back( e.strId );
    return out;
}

/** 개별 호 개시 INVITE 의 착신자 (TS 24.379 §11.1.1.2.1.1 9) — application/resource-lists+xml 의 entry 하나,
 * Request-URI 는 참여 기능 PSI). resource-lists 가 없거나 entry 가 하나가 아니면 false — 참여 기능은 403 + `145 unable
 * to determine called party`(§11.1.1.3.1.1 8)·9)). 착신자를 Request-URI·<mcptt-request-uri> 로 읽지 않는다(결정 D10).
 */
inline bool McpttPrivateCalledParty( const std::string &body, std::string &strCallee ) {
    strCallee.clear();
    const std::vector<McpttListEntry> vec = ParseResourceListEntries( body );
    if ( vec.size() != 1 ) return false;
    strCallee = vec[0].strId;
    return !strCallee.empty();
}

/** BYE 의 Reason(RFC 3326, 첫 값)이 애드혹 호 해제 요청인가 — `SIP;cause=200;text="User requested release"`
 *  (TS 24.379 §17.2.3.1.1 · §6.3.3.2.4 3A)). 프로토콜·cause·text 를 본다(대소문자·공백 무관). */
inline bool McpttIsUserRequestedRelease( const std::string &strReason ) {
    std::string s;
    for ( char c : strReason )
        if ( !std::isspace( (unsigned char)c ) ) s += (char)std::tolower( (unsigned char)c );
    if ( s.compare( 0, 4, "sip;" ) != 0 ) return false;
    const size_t c = s.find( ";cause=" );
    if ( c == std::string::npos || s.compare( c + 7, 3, "200" ) != 0 ||
         ( c + 10 < s.size() && std::isdigit( (unsigned char)s[c + 10] ) ) )
        return false;
    const size_t t = s.find( ";text=" );
    if ( t == std::string::npos ) return false;
    std::string text = s.substr( t + 6 );
    const size_t semi = text.find( ';', text.size() > 0 && text[0] == '"' ? text.find( '"', 1 ) : 0 );
    if ( semi != std::string::npos ) text = text.substr( 0, semi );
    if ( text.size() >= 2 && text.front() == '"' && text.back() == '"' ) text = text.substr( 1, text.size() - 2 );
    return text == "userrequestedrelease";
}

#endif  // _MCPTT_INFO_H_
