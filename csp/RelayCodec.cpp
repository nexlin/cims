#include "RelayCodec.h"

#include <strings.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "Log.h"
#include "SipCodecTable.h"

namespace RelayCodec {

    std::string CodecDesc::Label() const {
        return name + "/" + std::to_string( rate );
    }

    namespace {
        std::string _upper( const std::string &s ) {
            std::string o;
            for ( char c : s ) o += (char)toupper( (unsigned char)c );
            return o;
        }
        CSdpMedia *_audio( SDP_MEDIA_LIST &clsList ) {
            for ( SDP_MEDIA_LIST::iterator it = clsList.begin(); it != clsList.end(); ++it )
                if ( strcasecmp( it->m_strMedia.c_str(), "audio" ) == 0 && it->m_iPort > 0 ) return &( *it );
            return NULL;
        }
        const CSdpMedia *_audio( const SDP_MEDIA_LIST &clsList ) {
            for ( SDP_MEDIA_LIST::const_iterator it = clsList.begin(); it != clsList.end(); ++it )
                if ( strcasecmp( it->m_strMedia.c_str(), "audio" ) == 0 && it->m_iPort > 0 ) return &( *it );
            return NULL;
        }
        /** RFC 3551 §6 정적 PT — rtpmap 없이 쓰는 코덱 */
        bool _static( int pt, std::string &name, int &rate ) {
            switch ( pt ) {
                case 0:
                    name = "PCMU";
                    rate = 8000;
                    return true;
                case 3:
                    name = "GSM";
                    rate = 8000;
                    return true;
                case 4:
                    name = "G723";
                    rate = 8000;
                    return true;
                case 8:
                    name = "PCMA";
                    rate = 8000;
                    return true;
                case 9:
                    name = "G722";
                    rate = 8000;
                    return true;
                case 18:
                    name = "G729";
                    rate = 8000;
                    return true;
                default:
                    return false;
            }
        }
        /** "96 AMR-WB/16000/1" → pt·name·rate */
        bool _rtpmap( const std::string &v, int &pt, std::string &name, int &rate ) {
            const char *p = v.c_str();
            pt = atoi( p );
            const char *sp = strchr( p, ' ' );
            if ( !sp ) return false;
            ++sp;
            const char *sl = strchr( sp, '/' );
            if ( !sl ) return false;
            name = _upper( std::string( sp, sl - sp ) );
            rate = atoi( sl + 1 );
            return !name.empty();
        }
        std::string _rtpmapOf( const CodecDesc &c ) {
            return std::to_string( c.pt ) + " " + c.name + "/" + std::to_string( c.rate ) +
                   ( c.IsAmrWb() || c.name == "AMR" ? "/1" : "" );
        }
    }  // namespace

    std::vector<CodecDesc> AudioCodecs( const SDP_MEDIA_LIST &clsList, int *piTePt, std::string *pstrTeRtpmap,
                                        std::string *pstrTeFmtp ) {
        std::vector<CodecDesc> out;
        if ( piTePt ) *piTePt = -1;
        const CSdpMedia *m = _audio( clsList );
        if ( !m ) return out;
        std::map<int, std::pair<std::string, int>> mapRtpmap;
        std::map<int, std::string> mapFmtp;
        for ( SDP_ATTRIBUTE_LIST::const_iterator a = m->m_clsAttributeList.begin(); a != m->m_clsAttributeList.end();
              ++a ) {
            if ( strcasecmp( a->m_strName.c_str(), "rtpmap" ) == 0 ) {
                int pt, rate;
                std::string name;
                if ( _rtpmap( a->m_strValue, pt, name, rate ) ) mapRtpmap[pt] = std::make_pair( name, rate );
            } else if ( strcasecmp( a->m_strName.c_str(), "fmtp" ) == 0 ) {
                const char *sp = strchr( a->m_strValue.c_str(), ' ' );
                if ( sp ) mapFmtp[atoi( a->m_strValue.c_str() )] = sp + 1;
            }
        }
        for ( SDP_FMT_LIST::const_iterator f = m->m_clsFmtList.begin(); f != m->m_clsFmtList.end(); ++f ) {
            if ( f->empty() || !isdigit( (unsigned char)( *f )[0] ) ) continue;
            CodecDesc c;
            c.pt = atoi( f->c_str() );
            std::map<int, std::pair<std::string, int>>::iterator r = mapRtpmap.find( c.pt );
            if ( r != mapRtpmap.end() ) {
                c.name = r->second.first;
                c.rate = r->second.second;
            } else if ( !_static( c.pt, c.name, c.rate ) ) {
                continue;  // 동적 PT 인데 rtpmap 없음 — 해석 불가
            }
            if ( mapFmtp.count( c.pt ) ) c.fmtp = mapFmtp[c.pt];
            if ( c.name == "TELEPHONE-EVENT" ) {
                if ( piTePt && *piTePt < 0 ) {
                    *piTePt = c.pt;
                    if ( pstrTeRtpmap ) *pstrTeRtpmap = "telephone-event/" + std::to_string( c.rate );
                    if ( pstrTeFmtp ) *pstrTeFmtp = c.fmtp;
                }
                continue;
            }
            if ( c.name == "CN" ) continue;
            out.push_back( c );
        }
        return out;
    }

    CodecDesc ServiceCodec() {
        const CSipCodecEntry &top = CSipCodecTable::GetTop();
        CodecDesc c;
        c.name = _upper( top.m_strName );
        c.rate = top.m_iClockRate;
        c.pt = top.m_iPt;
        c.fmtp = top.m_strFmtp;
        return c;
    }

    CodecDesc ByName( const std::string &strName ) {
        CodecDesc c;
        std::string n = _upper( strName );
        if ( n == "AMRWB" || n == "AMR_WB" ) n = "AMR-WB";
        if ( n == "G711U" ) n = "PCMU";
        if ( n == "G711A" ) n = "PCMA";
        if ( n == "PCMU" ) {
            c.name = n;
            c.rate = 8000;
            c.pt = 0;
            return c;
        }
        if ( n == "PCMA" ) {
            c.name = n;
            c.rate = 8000;
            c.pt = 8;
            return c;
        }
        if ( n == "G722" ) {
            c.name = n;
            c.rate = 8000;
            c.pt = 9;
            return c;
        }
        // 코덱 테이블(AMR-WB·AMR …)에서 PT/rate/fmtp
        for ( const CSipCodecEntry &e : CSipCodecTable::GetList() ) {
            if ( _upper( e.m_strName ) == n ) {
                c.name = n;
                c.rate = e.m_iClockRate;
                c.pt = e.m_iPt;
                c.fmtp = e.m_strFmtp;
                return c;
            }
        }
        return c;  // 모르는 이름 — name 비움
    }

    const CodecDesc *Find( const std::vector<CodecDesc> &vec, const CodecDesc &c ) {
        for ( const CodecDesc &x : vec )
            if ( x.Same( c ) ) return &x;
        return NULL;
    }

    int InsertCodecs( SDP_MEDIA_LIST &clsList, const std::vector<CodecDesc> &vecAdd ) {
        CSdpMedia *m = _audio( clsList );
        if ( !m ) return 0;
        std::set<int> used;
        for ( SDP_FMT_LIST::const_iterator f = m->m_clsFmtList.begin(); f != m->m_clsFmtList.end(); ++f )
            used.insert( atoi( f->c_str() ) );
        std::vector<CodecDesc> have = AudioCodecs( clsList );
        int n = 0;
        for ( CodecDesc c : vecAdd ) {
            if ( c.name.empty() || Find( have, c ) ) continue;
            const bool bStatic = c.pt >= 0 && c.pt < 96;
            if ( !bStatic && ( c.pt < 0 || used.count( c.pt ) ) ) {
                int pt = 96;
                while ( pt <= 127 && used.count( pt ) ) ++pt;
                if ( pt > 127 ) continue;
                c.pt = pt;
            } else if ( bStatic && used.count( c.pt ) ) {
                continue;  // 정적 PT 가 이미 쓰인다(같은 코덱이면 Find 가 걸렀다) — 건너뛴다
            }
            used.insert( c.pt );
            m->AddFmt( c.pt );
            if ( !bStatic || !c.fmtp.empty() ) m->AddAttribute( "rtpmap", _rtpmapOf( c ).c_str() );
            if ( !c.fmtp.empty() ) m->AddAttribute( "fmtp", ( std::to_string( c.pt ) + " " + c.fmtp ).c_str() );
            have.push_back( c );
            ++n;
        }
        return n;
    }

    bool RewriteAudio( SDP_MEDIA_LIST &clsList, const CodecDesc &clsCodec, int iTePt, const std::string &strTeRtpmap,
                       const std::string &strTeFmtp ) {
        CSdpMedia *m = _audio( clsList );
        if ( !m || !clsCodec.Valid() ) return false;
        // 기존 코덱 속성 제거(rtpmap·fmtp 전부) — 방향·ptime·crypto 등은 그대로
        SDP_ATTRIBUTE_LIST keep;
        for ( SDP_ATTRIBUTE_LIST::iterator a = m->m_clsAttributeList.begin(); a != m->m_clsAttributeList.end(); ++a ) {
            if ( strcasecmp( a->m_strName.c_str(), "rtpmap" ) == 0 || strcasecmp( a->m_strName.c_str(), "fmtp" ) == 0 )
                continue;
            keep.push_back( *a );
        }
        m->m_clsAttributeList = keep;
        m->m_clsFmtList.clear();
        m->AddFmt( clsCodec.pt );
        const bool bStatic = clsCodec.pt < 96;
        if ( !bStatic || !clsCodec.fmtp.empty() ) m->AddAttribute( "rtpmap", _rtpmapOf( clsCodec ).c_str() );
        if ( !clsCodec.fmtp.empty() )
            m->AddAttribute( "fmtp", ( std::to_string( clsCodec.pt ) + " " + clsCodec.fmtp ).c_str() );
        if ( iTePt >= 0 ) {
            m->AddFmt( iTePt );
            m->AddAttribute( "rtpmap", ( std::to_string( iTePt ) + " " +
                                         ( strTeRtpmap.empty() ? std::string( "telephone-event/8000" ) : strTeRtpmap ) )
                                           .c_str() );
            m->AddAttribute(
                "fmtp",
                ( std::to_string( iTePt ) + " " + ( strTeFmtp.empty() ? std::string( "0-15" ) : strTeFmtp ) ).c_str() );
        }
        return true;
    }

    bool TranscodablePair( const CodecDesc &a, const CodecDesc &b ) {
        if ( !a.Valid() || !b.Valid() || a.Same( b ) ) return false;
        return ( a.IsAmrWb() && b.IsG711() ) || ( a.IsG711() && b.IsAmrWb() );
    }

    bool HasTranscodableSource( const std::vector<CodecDesc> &vecOffered, const CodecDesc &d ) {
        for ( const CodecDesc &c : vecOffered ) {
            if ( TranscodablePair( c, d ) ) return true;
        }
        return false;
    }

    int DecideLeg( const std::vector<CodecDesc> &vecOfferedA, const CodecDesc &negB, CodecDesc &codecA ) {
        if ( const CodecDesc *same = Find( vecOfferedA, negB ) ) {
            codecA = *same;
            return 0;
        }
        for ( const CodecDesc &c : vecOfferedA ) {
            if ( TranscodablePair( c, negB ) ) {
                codecA = c;
                return 1;
            }
        }
        return -1;
    }

}  // namespace RelayCodec
