#include "CspDiversion.h"

namespace CspDiversion {

    std::vector<std::string> SplitEntries( const std::string &s ) {
        std::vector<std::string> v;
        std::string cur;
        int depth = 0;
        for ( char ch : s ) {
            if ( ch == '<' ) ++depth;
            if ( ch == '>' && depth > 0 ) --depth;
            if ( ch == ',' && depth == 0 ) {
                v.push_back( cur );
                cur.clear();
                continue;
            }
            cur += ch;
        }
        v.push_back( cur );
        std::vector<std::string> out;
        for ( auto &e : v ) {
            size_t b = e.find_first_not_of( " \t\r\n" );
            if ( b == std::string::npos ) continue;
            size_t en = e.find_last_not_of( " \t\r\n" );
            out.push_back( e.substr( b, en - b + 1 ) );
        }
        return out;
    }

    int CountDiversions( const std::string &s ) {
        int n = 0;
        for ( const std::string &e : SplitEntries( s ) ) {
            size_t lt = e.find( '<' ), gt = e.find( '>' );
            if ( lt == std::string::npos || gt == std::string::npos || gt < lt ) continue;
            if ( e.substr( lt, gt - lt ).find( ";cause=" ) != std::string::npos ) ++n;
        }
        return n;
    }

    std::string LastIndex( const std::string &s ) {
        std::vector<std::string> v = SplitEntries( s );
        if ( v.empty() ) return "";
        const std::string &e = v.back();
        size_t gt = e.find( '>' );
        std::string params = ( gt == std::string::npos ) ? e : e.substr( gt + 1 );
        size_t k = params.find( "index=" );
        if ( k == std::string::npos ) return "";
        k += 6;
        size_t end = params.find_first_of( ";, \t", k );
        return params.substr( k, end == std::string::npos ? std::string::npos : end - k );
    }

    std::string MakeUri( const std::string &strUser, const std::string &strDomain ) {
        if ( strUser.compare( 0, 4, "sip:" ) == 0 || strUser.compare( 0, 5, "sips:" ) == 0 ||
             strUser.compare( 0, 4, "tel:" ) == 0 )
            return strUser;
        return "sip:" + strUser + "@" + strDomain;
    }

    std::string BuildHistoryInfo( const std::string &strExisting, const std::string &strDomain,
                                  const std::string &strServed, const std::vector<Hop> &vecHops ) {
        std::string out;
        std::string parent;
        std::vector<std::string> existing = SplitEntries( strExisting );
        if ( existing.empty() ) {
            out = "<" + MakeUri( strServed, strDomain ) + ">;index=1";
            parent = "1";
        } else {
            for ( size_t i = 0; i < existing.size(); ++i ) out += ( i ? ", " : "" ) + existing[i];
            parent = LastIndex( strExisting );
            if ( parent.empty() ) parent = std::to_string( existing.size() );  // index 없는 값 — 순번으로 이어 간다
        }
        for ( const Hop &h : vecHops ) {
            const std::string idx = parent + ".1";
            out += ", <" + MakeUri( h.strUser, strDomain ) + ";cause=" + std::to_string( h.iCause ) + ">;index=" + idx +
                   ";mp=" + parent;
            parent = idx;
        }
        return out;
    }

    std::string ChainLabel( const std::string &strServed, const std::vector<Hop> &vecHops ) {
        std::string s = strServed;
        for ( const Hop &h : vecHops ) s += " → " + h.strUser;
        return s;
    }

}  // namespace CspDiversion
