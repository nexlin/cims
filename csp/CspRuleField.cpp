#include "CspRuleField.h"

#include <strings.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <regex>

#include "CspDialPlan.h"
#include "SipMessage.h"

namespace {

    const char *const LEGACY[] = { "from_uri_host",       "from_uri_user", "to_uri_host", "to_uri_user", "req_uri_host",
                                   "req_uri_user",        "src_ip",        "dst_ip",      "user_agent",  "method",
                                   "p_asserted_identity", "via_host" };

    // compact form (RFC 3261 §7.3.3 · RFC 3515 · RFC 4028 …) → 긴 이름 소문자
    std::string _headerKey( const std::string &s ) {
        std::string k;
        for ( char c : s ) k.push_back( (char)tolower( (unsigned char)c ) );
        if ( k.size() == 1 ) {
            switch ( k[0] ) {
                case 'f':
                    return "from";
                case 't':
                    return "to";
                case 'm':
                    return "contact";
                case 'i':
                    return "call-id";
                case 'v':
                    return "via";
                case 'c':
                    return "content-type";
                case 'l':
                    return "content-length";
                case 'k':
                    return "supported";
                case 's':
                    return "subject";
                case 'e':
                    return "content-encoding";
                case 'o':
                    return "event";
                case 'r':
                    return "refer-to";
                case 'b':
                    return "referred-by";
                case 'u':
                    return "allow-events";
                case 'x':
                    return "session-expires";
                case 'a':
                    return "accept-contact";
                case 'j':
                    return "reject-contact";
                case 'd':
                    return "request-disposition";
                case 'y':
                    return "identity";
            }
        }
        return k;
    }

    // 쉼표로 이어진 여러 값 — 따옴표·<> 안의 쉼표는 값의 일부다
    std::vector<std::string> _splitTop( const std::string &v ) {
        std::vector<std::string> out;
        std::string cur;
        bool q = false;
        int a = 0;
        for ( char c : v ) {
            if ( c == '"' ) q = !q;
            if ( !q && c == '<' ) ++a;
            if ( !q && c == '>' && a > 0 ) --a;
            if ( !q && a == 0 && c == ',' ) {
                out.push_back( cur );
                cur.clear();
                continue;
            }
            cur.push_back( c );
        }
        out.push_back( cur );
        for ( std::string &s : out ) {
            while ( !s.empty() && isspace( (unsigned char)s.front() ) ) s.erase( s.begin() );
            while ( !s.empty() && isspace( (unsigned char)s.back() ) ) s.pop_back();
        }
        std::vector<std::string> ne;
        for ( const std::string &s : out )
            if ( !s.empty() ) ne.push_back( s );
        return ne;
    }

    std::string _unescape( const std::string &s ) {
        std::string o;
        for ( size_t i = 0; i < s.size(); ++i ) {
            if ( s[i] == '%' && i + 2 < s.size() && isxdigit( (unsigned char)s[i + 1] ) &&
                 isxdigit( (unsigned char)s[i + 2] ) ) {
                o.push_back( (char)strtol( s.substr( i + 1, 2 ).c_str(), NULL, 16 ) );
                i += 2;
            } else {
                o.push_back( s[i] );
            }
        }
        return o;
    }

    std::string _lower( std::string s ) {
        for ( char &c : s ) c = (char)tolower( (unsigned char)c );
        return s;
    }

    struct Ref {
        bool ok = false;
        std::string src;    // callee · request_uri · from · to · contact · header · src · local_node · method
        std::string hdr;    // header:<이름> 의 이름
        std::string part;   // user · host · port · scheme · display · param · e164 · ip · (빈 값)
        std::string param;  // param:<이름>
    };

    bool _validPart( const std::string &p ) {
        return p.empty() || p == "user" || p == "host" || p == "port" || p == "scheme" || p == "display" ||
               p == "param";
    }

    Ref _parse( const std::string &f ) {
        Ref r;
        std::string rest;
        if ( f.compare( 0, 7, "header:" ) == 0 ) {
            std::string s = f.substr( 7 );
            size_t dot = s.find( '.' );
            r.src = "header";
            r.hdr = s.substr( 0, dot );
            rest = dot == std::string::npos ? "" : s.substr( dot + 1 );
            if ( r.hdr.empty() ) return r;
            for ( char c : r.hdr )  // token (RFC 3261 §25.1) — '.' 은 부분 구분자라 이름에 쓰지 않는다
                if ( !( isalnum( (unsigned char)c ) || strchr( "-!%*_+`'~", c ) ) ) return r;
        } else {
            size_t dot = f.find( '.' );
            r.src = f.substr( 0, dot );
            rest = dot == std::string::npos ? "" : f.substr( dot + 1 );
        }
        if ( rest.compare( 0, 6, "param:" ) == 0 ) {
            r.part = "param";
            r.param = rest.substr( 6 );
            if ( r.param.empty() ) return r;
        } else {
            r.part = rest;
        }
        if ( r.src == "callee" )
            r.ok = r.part == "e164";
        else if ( r.src == "src" )
            r.ok = r.part == "ip" || r.part == "port";
        else if ( r.src == "local_node" || r.src == "method" )
            r.ok = r.part.empty();
        else if ( r.src == "request_uri" || r.src == "from" || r.src == "to" || r.src == "contact" ||
                  r.src == "header" )
            r.ok = _validPart( r.part );
        return r;
    }

    std::string _paramOf( const SIP_PARAMETER_LIST &lst, const std::string &name, bool &found ) {
        for ( const CSipParameter &p : lst )
            if ( !strcasecmp( p.m_strName.c_str(), name.c_str() ) ) {
                found = true;
                return p.m_strValue;
            }
        found = false;
        return "";
    }

    // 주소형 값의 한 부분 — header param(>뒤) 을 먼저, 없으면 URI param
    bool _partOfAddr( const CSipFrom &a, const Ref &r, std::string &out ) {
        const CSipUri &u = a.m_clsUri;
        if ( r.part == "user" )
            out = _unescape( u.m_strUser );
        else if ( r.part == "host" )
            out = u.m_strHost;
        else if ( r.part == "port" )
            out = u.m_iPort > 0 ? std::to_string( u.m_iPort ) : "";
        else if ( r.part == "scheme" )
            out = _lower( u.m_strProtocol );
        else if ( r.part == "display" ) {
            out = a.m_strDisplayName;
            if ( out.size() >= 2 && out.front() == '"' && out.back() == '"' ) out = out.substr( 1, out.size() - 2 );
        } else if ( r.part == "param" ) {
            bool f;
            out = _paramOf( a.m_clsParamList, r.param, f );
            if ( !f ) out = _paramOf( u.m_clsUriParamList, r.param, f );
            return f;
        } else
            return false;
        return true;
    }

    void _addrValues( const CSipFrom &a, const Ref &r, std::vector<std::string> &out ) {
        std::string v;
        if ( _partOfAddr( a, r, v ) ) out.push_back( v );
    }

    void _rawAddrValues( const std::string &raw, const Ref &r, std::vector<std::string> &out ) {
        for ( const std::string &item : _splitTop( raw ) ) {
            CSipFrom a;
            if ( a.Parse( item.c_str(), (int)item.size() ) == -1 ) continue;
            _addrValues( a, r, out );
        }
    }

    bool _cidrMatch( const std::string &ip, const std::string &cidr ) {
        auto slash = cidr.find( '/' );
        std::string net = cidr;
        int prefix = 32;
        if ( slash != std::string::npos ) {
            net = cidr.substr( 0, slash );
            prefix = atoi( cidr.c_str() + slash + 1 );
        }
        auto toUint = []( const std::string &s, bool &ok ) -> uint32_t {
            int o[4] = { 0, 0, 0, 0 };
            ok = sscanf( s.c_str(), "%d.%d.%d.%d", &o[0], &o[1], &o[2], &o[3] ) == 4;
            uint32_t r = 0;
            for ( int i = 0; i < 4; ++i ) r = ( r << 8 ) | (uint8_t)o[i];
            return r;
        };
        bool a, b;
        uint32_t ipN = toUint( ip, a ), netN = toUint( net, b );
        if ( !a || !b ) return false;
        if ( prefix <= 0 ) return true;
        if ( prefix >= 32 ) return ipN == netN;
        uint32_t mask = 0xFFFFFFFFu << ( 32 - prefix );
        return ( ipN & mask ) == ( netN & mask );
    }

    std::vector<std::string> _splitList( const std::string &s ) {
        std::vector<std::string> out;
        std::string cur;
        for ( size_t i = 0; i <= s.size(); ++i ) {
            if ( i == s.size() || s[i] == ',' ) {
                while ( !cur.empty() && isspace( (unsigned char)cur.front() ) ) cur.erase( cur.begin() );
                while ( !cur.empty() && isspace( (unsigned char)cur.back() ) ) cur.pop_back();
                if ( !cur.empty() ) out.push_back( cur );
                cur.clear();
            } else {
                cur.push_back( s[i] );
            }
        }
        return out;
    }

    bool _one( const std::string &fv, const std::string &op, const std::string &val ) {
        if ( op == "eq" ) return fv == val;
        if ( op == "prefix" ) return fv.size() >= val.size() && fv.compare( 0, val.size(), val ) == 0;
        if ( op == "suffix" )
            return fv.size() >= val.size() && fv.compare( fv.size() - val.size(), val.size(), val ) == 0;
        if ( op == "contains" ) return !val.empty() && fv.find( val ) != std::string::npos;
        if ( op == "regex" ) {
            try {
                return std::regex_search( fv, std::regex( val, std::regex::ECMAScript ) );
            } catch ( const std::regex_error & ) {
                return false;
            }
        }
        if ( op == "in_cidr" ) return _cidrMatch( fv, val );
        if ( op == "in_range" ) return CspDialPlan::InNumberRange( fv, val );
        if ( op == "in_list" ) {
            for ( const std::string &it : _splitList( val ) )
                if ( it == fv ) return true;
            return false;
        }
        return false;
    }

}  // namespace

bool RuleFieldSyntaxOk( const std::string &strField ) {
    for ( const char *p : LEGACY )
        if ( strField == p ) return true;
    return _parse( strField ).ok;
}

const std::string *RuleLegacyFieldValue( const MessageCtx &ctx, const std::string &f ) {
    if ( f == "from_uri_host" ) return &ctx.from_uri_host;
    if ( f == "from_uri_user" ) return &ctx.from_uri_user;
    if ( f == "to_uri_host" ) return &ctx.to_uri_host;
    if ( f == "to_uri_user" ) return &ctx.to_uri_user;
    if ( f == "req_uri_host" ) return &ctx.req_uri_host;
    if ( f == "req_uri_user" ) return &ctx.req_uri_user;
    if ( f == "src_ip" ) return &ctx.src_ip;
    if ( f == "dst_ip" ) return &ctx.dst_ip;
    if ( f == "user_agent" ) return &ctx.user_agent;
    if ( f == "method" ) return &ctx.method;
    if ( f == "p_asserted_identity" ) return &ctx.p_asserted_identity;
    if ( f == "via_host" ) return &ctx.via_host;
    return nullptr;
}

std::string RulePaiUser( const CSipMessage *m ) {
    if ( m == NULL ) return "";
    for ( const CSipHeader &h : m->m_clsHeaderList ) {
        if ( _headerKey( h.m_strName ) != "p-asserted-identity" ) continue;
        Ref r;
        r.part = "user";
        std::vector<std::string> v;
        _rawAddrValues( h.m_strValue, r, v );
        if ( !v.empty() ) return v.front();
    }
    return "";
}

bool RuleFieldValues( const MessageCtx &ctx, const std::string &f, std::vector<std::string> &out, bool &bHost ) {
    out.clear();
    bHost = false;
    if ( const std::string *p = RuleLegacyFieldValue( ctx, f ) ) {  // 옛 이름 — 종전 값 그대로
        out.push_back( *p );
        return true;
    }
    Ref r = _parse( f );
    if ( !r.ok ) return false;
    bHost = r.part == "host";
    if ( r.src == "callee" ) {
        if ( !ctx.callee_e164.empty() ) out.push_back( ctx.callee_e164 );
        return true;
    }
    if ( r.src == "local_node" ) {
        if ( !ctx.local_node.empty() ) out.push_back( ctx.local_node );
        return true;
    }
    const CSipMessage *m = ctx.msg;
    if ( m == NULL ) return true;
    if ( r.src == "method" ) {
        if ( !m->m_strSipMethod.empty() ) out.push_back( m->m_strSipMethod );
        return true;
    }
    if ( r.src == "src" ) {
        if ( r.part == "ip" && !m->m_strClientIp.empty() ) out.push_back( m->m_strClientIp );
        if ( r.part == "port" && m->m_iClientPort > 0 ) out.push_back( std::to_string( m->m_iClientPort ) );
        return true;
    }
    if ( r.src == "request_uri" ) {
        if ( r.part.empty() ) {  // 값 그대로 — URI 문자열
            char buf[1024];
            if ( const_cast<CSipUri &>( m->m_clsReqUri ).ToString( buf, sizeof( buf ) ) > 0 ) out.push_back( buf );
            return true;
        }
        CSipFrom a;
        a.m_clsUri = m->m_clsReqUri;
        _addrValues( a, r, out );
        return true;
    }
    // 이름 있는 헤더 — psip 이 따로 해석해 두는 것은 그 값을, 나머지는 원문 목록에서
    std::string key = r.src == "header" ? _headerKey( r.hdr ) : r.src;
    if ( key == "p-asserted-identity" && !ctx.trusted_peer ) return true;  // 믿지 않는 원천 — 값 없음
    if ( key == "from" || key == "to" ) {
        const CSipFrom &a = key == "from" ? m->m_clsFrom : m->m_clsTo;
        if ( r.part.empty() ) {  // 값 그대로 — 주소 문자열
            char buf[1024];
            if ( const_cast<CSipFrom &>( a ).ToString( buf, sizeof( buf ) ) > 0 ) out.push_back( buf );
        } else {
            _addrValues( a, r, out );
        }
        return true;
    }
    if ( key == "contact" ) {
        for ( const CSipFrom &a : m->m_clsContactList ) {
            if ( r.part.empty() ) {
                char buf[1024];
                if ( const_cast<CSipFrom &>( a ).ToString( buf, sizeof( buf ) ) > 0 ) out.push_back( buf );
            } else {
                _addrValues( a, r, out );
            }
        }
        return true;
    }
    if ( key == "user-agent" ) {
        if ( r.part.empty() && !m->m_strUserAgent.empty() ) out.push_back( m->m_strUserAgent );
        return true;
    }
    if ( key == "via" ) {  // Via 는 name-addr 가 아니다 — host · port 만
        for ( const CSipVia &v : m->m_clsViaList ) {
            if ( r.part == "host" )
                out.push_back( v.m_strHost );
            else if ( r.part == "port" && v.m_iPort > 0 )
                out.push_back( std::to_string( v.m_iPort ) );
        }
        return true;
    }
    for ( const CSipHeader &h : m->m_clsHeaderList ) {
        if ( _headerKey( h.m_strName ) != key ) continue;
        if ( r.part.empty() ) {
            for ( const std::string &item : _splitTop( h.m_strValue ) ) out.push_back( item );
        } else {
            _rawAddrValues( h.m_strValue, r, out );
        }
    }
    return true;
}

bool RuleApplyOp( const std::vector<std::string> &vals, const std::string &op, const std::string &val, bool bHost ) {
    bool any = false;
    for ( const std::string &v : vals )
        if ( !v.empty() ) any = true;
    if ( op == "exists" ) return any;
    if ( op == "not_exists" ) return !any;
    const std::string cv = bHost ? _lower( val ) : val;
    if ( op == "ne" ) {  // 어느 값도 같지 않을 때 — 값이 없으면 참 (종전 단일 값의 "" != v 와 같다)
        for ( const std::string &v : vals )
            if ( ( bHost ? _lower( v ) : v ) == cv ) return false;
        return true;
    }
    for ( const std::string &v : vals )
        if ( _one( bHost ? _lower( v ) : v, op, op == "in_list" && bHost ? _lower( val ) : cv ) ) return true;
    if ( vals.empty() ) return _one( "", op, cv );  // 종전: 빈 값에 대한 평가(대개 false)
    return false;
}
