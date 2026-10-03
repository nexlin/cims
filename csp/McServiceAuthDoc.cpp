// MC 서비스 인가·서비스 설정의 문서·판정 부품 (McServiceAuth.h) — SIP 스택·전역 상태 없이 쓰는 순수 함수.
//   단위시험(tests/csp_mcptt_request_test.cpp)이 이 파일만 링크한다.

#include <cstdlib>
#include <set>
#include <sstream>

#include "McServiceAuth.h"
#include "McpttInfo.h"
#include "SimpleJson.h"

namespace {
    /** 요소 이름(접두사 뗀 것)이 name 인 다음 시작 태그의 위치 — 없으면 npos */
    size_t _FindStart( const std::string &s, size_t from, const char *name, size_t *pEnd ) {
        const std::string n = name;
        for ( size_t p = s.find( '<', from ); p != std::string::npos; p = s.find( '<', p + 1 ) ) {
            const size_t q = p + 1;
            if ( q >= s.size() || s[q] == '/' || s[q] == '?' || s[q] == '!' ) continue;
            size_t e = q;
            while ( e < s.size() && s[e] != ' ' && s[e] != '\t' && s[e] != '\r' && s[e] != '\n' && s[e] != '/' &&
                    s[e] != '>' )
                ++e;
            std::string tag = s.substr( q, e - q );
            const size_t c = tag.find( ':' );
            if ( c != std::string::npos ) tag = tag.substr( c + 1 );
            if ( tag == n ) {
                if ( pEnd ) *pEnd = e;
                return p;
            }
        }
        return std::string::npos;
    }

    /** 요소 이름이 name 인 다음 끝 태그의 위치 */
    size_t _FindEnd( const std::string &s, size_t from, const char *name ) {
        const std::string n = name;
        for ( size_t p = s.find( "</", from ); p != std::string::npos; p = s.find( "</", p + 2 ) ) {
            size_t e = p + 2;
            while ( e < s.size() && s[e] != '>' && s[e] != ' ' ) ++e;
            std::string tag = s.substr( p + 2, e - p - 2 );
            const size_t c = tag.find( ':' );
            if ( c != std::string::npos ) tag = tag.substr( c + 1 );
            if ( tag == n ) return p;
        }
        return std::string::npos;
    }

    std::string _Attr( const std::string &s, size_t tagStart, const char *attr ) {
        const size_t gt = s.find( '>', tagStart );
        const std::string head = s.substr( tagStart, gt == std::string::npos ? std::string::npos : gt - tagStart );
        const std::string a = std::string( " " ) + attr + "=";
        size_t p = head.find( a );
        if ( p == std::string::npos ) return "";
        p += a.size();
        if ( p >= head.size() || ( head[p] != '"' && head[p] != '\'' ) ) return "";
        const size_t e = head.find( head[p], p + 1 );
        return e == std::string::npos ? "" : head.substr( p + 1, e - p - 1 );
    }

    std::string _Trim( const std::string &v ) {
        const size_t a = v.find_first_not_of( " \t\r\n" );
        if ( a == std::string::npos ) return "";
        return v.substr( a, v.find_last_not_of( " \t\r\n" ) - a + 1 );
    }
}  // namespace

std::vector<McPocEntity> ParsePocSettings( const std::string &strXml ) {
    std::vector<McPocEntity> vec;
    size_t e = 0;
    for ( size_t p = _FindStart( strXml, 0, "entity", &e ); p != std::string::npos;
          p = _FindStart( strXml, e, "entity", &e ) ) {
        const size_t gt = strXml.find( '>', p );
        if ( gt == std::string::npos ) break;
        const size_t close = strXml[gt - 1] == '/' ? gt : _FindEnd( strXml, gt, "entity" );
        const std::string inner =
            close == std::string::npos ? strXml.substr( gt + 1 ) : strXml.substr( gt + 1, close - gt - 1 );
        McPocEntity ent;
        ent.strId = _Attr( strXml, p, "id" );
        std::string v;
        if ( McpttElemValue( inner, "answer-mode", v ) ) ent.strAnswerMode = _Trim( v );
        if ( McpttElemValue( inner, "user-profile-index", v ) && !v.empty() &&
             v.find_first_not_of( "0123456789" ) == std::string::npos )
            ent.iUserProfileIndex = atoi( v.c_str() );
        if ( McpttElemValue( inner, "multiplex-support", v ) ) {
            ent.bHasMultiplex = true;
            ent.bMultiplex = v == "true" || v == "1";
        }
        vec.push_back( ent );
        if ( close == std::string::npos ) break;
        e = close;
    }
    return vec;
}

std::string BuildPocSettingsDoc( const std::vector<McPocEntity> &vecEntities ) {
    // 확장 요소는 XML 스키마 표 7.4.1.2.2-2 의 이름공간(urn:3gpp:mcsSettings:1.0, qualified)에 둔다
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
      << "<poc-settings xmlns=\"urn:oma:params:xml:ns:poc:poc-settings\" "
         "xmlns:mcs10Set=\"urn:3gpp:mcsSettings:1.0\">\r\n";
    for ( const auto &ent : vecEntities ) {
        o << "  <entity id=\"" << McpttXmlEsc( ent.strId ) << "\">\r\n";
        if ( !ent.strAnswerMode.empty() )
            o << "    <am-settings><answer-mode>" << McpttXmlEsc( ent.strAnswerMode )
              << "</answer-mode></am-settings>\r\n";
        if ( ent.iUserProfileIndex >= 0 )
            o << "    <mcs10Set:selected-user-profile-index><mcs10Set:user-profile-index>" << ent.iUserProfileIndex
              << "</mcs10Set:user-profile-index></mcs10Set:selected-user-profile-index>\r\n";
        if ( ent.bHasMultiplex )
            o << "    <mcs10Set:multiplex-support>" << ( ent.bMultiplex ? "true" : "false" )
              << "</mcs10Set:multiplex-support>\r\n";
        o << "  </entity>\r\n";
    }
    o << "</poc-settings>\r\n";
    return o.str();
}

const char *McServiceScope( EMcService e ) {
    return e == EMcService::McVideo ? "3gpp:mc:video_service"
                                    : ( e == EMcService::McData ? "3gpp:mc:data_service" : "3gpp:mc:ptt_service" );
}

const char *McInfoSubtype( EMcService e ) {
    return e == EMcService::McVideo  ? "vnd.3gpp.mcvideo-info+xml"
           : e == EMcService::McData ? "vnd.3gpp.mcdata-info+xml"
                                     : "vnd.3gpp.mcptt-info+xml";
}

const char *McInfoPrefix( EMcService e ) {
    return e == EMcService::McVideo ? "mcvideo" : ( e == EMcService::McData ? "mcdata" : "mcptt" );
}

std::string McMultipleDevicesDoc( EMcService e ) {
    const std::string p = McInfoPrefix( e );
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<" + p + "info xmlns=\"urn:3gpp:ns:" + p + "Info:1.0\"><" +
           p + "-Params><multiple-devices-ind type=\"Normal\"><" + p + "Boolean>true</" + p +
           "Boolean></multiple-devices-ind></" + p + "-Params></" + p + "info>\r\n";
}

EMcAuthResult McAuthVerdict( const std::string &strIntrospectJson, EMcService e, const std::string &strImpuUser,
                             std::string &strWhy ) {
    const std::string j = _Trim( strIntrospectJson );
    if ( j.empty() || j[0] != '{' ) {
        strWhy = "introspection 응답이 JSON 이 아니다";
        return EMcAuthResult::Unavailable;
    }
    const SimpleJson::JsonNode n = SimpleJson::JsonNode::Parse( j );
    if ( n.GetString( "active" ) != "true" ) {
        strWhy = "토큰 비활성(무효·만료)";
        return EMcAuthResult::Failed;
    }
    // scope — 공백 구분 문자열(RFC 7662 §2.2), 낱말 단위로 대조
    std::set<std::string> setScope;
    {
        std::istringstream is( n.GetString( "scope" ) );
        std::string w;
        while ( is >> w ) setScope.insert( w );
    }
    if ( !setScope.count( McServiceScope( e ) ) ) {
        strWhy = std::string( "scope 에 " ) + McServiceScope( e ) + " 없음";
        return EMcAuthResult::Failed;
    }
    // MC ID — 단일 MC service ID(MCData = mcdata_id, MCVideo 는 MCPTT ID 와 같은 값 — mcx_identity_scope.md §1)
    std::string strMcId = n.GetString( e == EMcService::McData ? "mcdata_id" : "mcptt_id" );
    if ( strMcId.empty() && e == EMcService::McData ) strMcId = n.GetString( "mcptt_id" );
    if ( McpttBareId( strMcId ) != strImpuUser ) {
        strWhy = "토큰의 MC ID(" + strMcId + ") ≠ 요청 IMPU(" + strImpuUser + ")";
        return EMcAuthResult::Failed;
    }
    return EMcAuthResult::Ok;
}
