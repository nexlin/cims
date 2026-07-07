/*
 * MCData SDS message codec (TS 24.282 §15)
 */

#include "McDataCodec.h"

#include <stdlib.h>
#include <string.h>

#include <vector>

#include "Base64.h"

// ── 내부 유틸 ──────────────────────────────────────────────

static std::string _lower( const std::string &s ) {
    std::string r = s;
    for ( size_t i = 0; i < r.size(); ++i )
        if ( r[i] >= 'A' && r[i] <= 'Z' ) r[i] = (char)( r[i] - 'A' + 'a' );
    return r;
}

static std::string _trim( const std::string &s ) {
    size_t b = s.find_first_not_of( " \t\r\n" );
    if ( b == std::string::npos ) return "";
    size_t e = s.find_last_not_of( " \t\r\n" );
    return s.substr( b, e - b + 1 );
}

/** Content-Type 원문에서 boundary 파라미터 추출 (없으면 빈 문자열) */
static std::string _boundaryFromContentType( const std::string &strContentType ) {
    std::string low = _lower( strContentType );
    size_t pos = low.find( "boundary=" );
    if ( pos == std::string::npos ) return "";
    std::string v = strContentType.substr( pos + 9 );
    size_t end = v.find_first_of( ";\r\n" );
    if ( end != std::string::npos ) v = v.substr( 0, end );
    v = _trim( v );
    if ( v.size() >= 2 && v[0] == '"' && v[v.size() - 1] == '"' ) v = v.substr( 1, v.size() - 2 );
    return v;
}

/** boundary 를 못 얻었을 때 본문 첫 줄 "--X" 에서 유도 */
static std::string _boundaryFromBody( const std::string &strBody ) {
    if ( strBody.compare( 0, 2, "--" ) != 0 ) return "";
    size_t eol = strBody.find( "\r\n" );
    if ( eol == std::string::npos ) eol = strBody.find( '\n' );
    if ( eol == std::string::npos ) return "";
    std::string b = _trim( strBody.substr( 2, eol - 2 ) );
    return b;
}

struct SMimePart {
    std::string strContentType;  // 소문자
    bool bBase64;
    std::string strContent;  // CTE 디코딩 완료 (base64 → binary)
    SMimePart() : bBase64( false ) {
    }
};

/** multipart 본문 → 파트 목록. 헤더는 CRLFCRLF(관용 LF 도 허용) 로 구분. */
static bool _splitMultipart( const std::string &strBody, const std::string &strBoundary,
                             std::vector<SMimePart> &vecParts ) {
    std::string delim = "--" + strBoundary;
    size_t pos = strBody.find( delim );
    while ( pos != std::string::npos ) {
        size_t partStart = pos + delim.size();
        // 종료 delimiter "--boundary--"
        if ( strBody.compare( partStart, 2, "--" ) == 0 ) break;
        // delimiter 라인 끝까지 스킵
        size_t lineEnd = strBody.find( '\n', partStart );
        if ( lineEnd == std::string::npos ) break;
        partStart = lineEnd + 1;

        size_t next = strBody.find( delim, partStart );
        size_t partEnd = ( next == std::string::npos ) ? strBody.size() : next;
        std::string part = strBody.substr( partStart, partEnd - partStart );

        // 헤더/본문 분리
        size_t hdrEnd = part.find( "\r\n\r\n" );
        size_t bodyOff = hdrEnd + 4;
        if ( hdrEnd == std::string::npos ) {
            hdrEnd = part.find( "\n\n" );
            bodyOff = hdrEnd + 2;
        }
        if ( hdrEnd == std::string::npos ) {
            pos = next;
            continue;
        }

        SMimePart clsPart;
        std::string hdrs = _lower( part.substr( 0, hdrEnd ) );
        size_t ctPos = hdrs.find( "content-type:" );
        if ( ctPos != std::string::npos ) {
            size_t ctEnd = hdrs.find_first_of( "\r\n", ctPos );
            std::string ct = hdrs.substr( ctPos + 13, ( ctEnd == std::string::npos ? hdrs.size() : ctEnd ) - ctPos - 13 );
            size_t semi = ct.find( ';' );
            if ( semi != std::string::npos ) ct = ct.substr( 0, semi );
            clsPart.strContentType = _trim( ct );
        }
        clsPart.bBase64 = hdrs.find( "content-transfer-encoding: base64" ) != std::string::npos ||
                          hdrs.find( "content-transfer-encoding:base64" ) != std::string::npos;

        std::string content = part.substr( bodyOff );
        // 파트 끝 CRLF (다음 delimiter 소속) 제거
        while ( !content.empty() && ( content[content.size() - 1] == '\n' || content[content.size() - 1] == '\r' ) )
            content.erase( content.size() - 1 );

        if ( clsPart.bBase64 ) {
            // base64 는 공백 제거 후 디코딩
            std::string b64;
            b64.reserve( content.size() );
            for ( size_t i = 0; i < content.size(); ++i )
                if ( content[i] != '\r' && content[i] != '\n' && content[i] != ' ' && content[i] != '\t' )
                    b64 += content[i];
            std::vector<char> buf( GetBase64DecodeLength( (int)b64.size() ) + 4 );
            int iLen = Base64Decode( b64.c_str(), (int)b64.size(), &buf[0], (int)buf.size() );
            if ( iLen > 0 ) clsPart.strContent.assign( &buf[0], iLen );
        } else {
            clsPart.strContent = content;
        }

        vecParts.push_back( clsPart );
        pos = next;
    }
    return !vecParts.empty();
}

static std::string _hex( const unsigned char *p, int n ) {
    static const char *h = "0123456789abcdef";
    std::string s;
    s.reserve( n * 2 );
    for ( int i = 0; i < n; ++i ) {
        s += h[p[i] >> 4];
        s += h[p[i] & 0x0F];
    }
    return s;
}

/** SDS SIGNALLING PAYLOAD (§15.1.2) / SDS NOTIFICATION (§15.1.5) 파싱 */
static bool _parseSignalling( const std::string &bin, CMcDataSdsInfo &clsInfo ) {
    const unsigned char *b = (const unsigned char *)bin.data();
    int n = (int)bin.size();
    if ( n < 1 ) return false;

    int iType = b[0] & 0x3F;  // bit7/8 = protected/authenticated 플래그 (§15.2.2)
    if ( iType == MCDATA_MSG_SDS_SIGNALLING ) {
        // [type1][datetime5][conv16][msg16] = 38 octets 최소
        if ( n < 38 ) return false;
        clsInfo.m_iMsgType = iType;
        clsInfo.m_tSentTime = 0;
        for ( int i = 1; i <= 5; ++i ) clsInfo.m_tSentTime = ( clsInfo.m_tSentTime << 8 ) | b[i];
        clsInfo.m_strConvId = _hex( b + 6, 16 );
        clsInfo.m_strMsgId = _hex( b + 22, 16 );
        // optional IEs
        int i = 38;
        while ( i < n ) {
            unsigned char iei = b[i];
            if ( ( iei & 0xF0 ) == 0x80 ) {  // SDS disposition request type (type 1 TV, IEI=8-)
                clsInfo.m_iDispositionReq = iei & 0x0F;
                i += 1;
            } else if ( iei == 0x21 ) {  // InReplyTo message ID (TV 17)
                i += 17;
            } else if ( iei == 0x22 ) {  // Application ID (TV 2)
                i += 2;
            } else if ( iei == 0x7D ) {  // Extended application ID (TLV-E)
                if ( i + 3 > n ) break;
                i += 3 + ( ( b[i + 1] << 8 ) | b[i + 2] );
            } else {
                break;  // 미지의 IE — 무시하고 종료
            }
        }
        return true;
    }

    if ( iType == MCDATA_MSG_FD_SIGNALLING ) {
        // FD SIGNALLING PAYLOAD (§15.1.3): [type1][datetime5][conv16][msg16] + optional IEs
        if ( n < 38 ) return false;
        clsInfo.m_iMsgType = iType;
        clsInfo.m_tSentTime = 0;
        for ( int i = 1; i <= 5; ++i ) clsInfo.m_tSentTime = ( clsInfo.m_tSentTime << 8 ) | b[i];
        clsInfo.m_strConvId = _hex( b + 6, 16 );
        clsInfo.m_strMsgId = _hex( b + 22, 16 );
        int i = 38;
        while ( i < n ) {
            unsigned char iei = b[i];
            if ( ( iei & 0xF0 ) == 0x90 ) {  // FD disposition request type (TV 1)
                clsInfo.m_iDispositionReq = iei & 0x0F;
                i += 1;
            } else if ( ( iei & 0xF0 ) == 0xA0 ) {  // Mandatory download (TV 1)
                i += 1;
            } else if ( iei == 0x21 ) {  // InReplyTo message ID (TV 17)
                i += 17;
            } else if ( iei == 0x22 ) {  // Application ID (TV 2)
                i += 2;
            } else if ( iei == 0x78 || iei == 0x79 ) {  // Payload / Metadata (TLV-E)
                if ( i + 3 > n ) break;
                int iLen = ( b[i + 1] << 8 ) | b[i + 2];
                if ( i + 3 + iLen > n ) break;
                if ( iei == 0x78 && iLen >= 1 ) {
                    clsInfo.m_iPayloadSize += iLen - 1;
                    if ( b[i + 3] == 0x04 )  // FILEURL (§15.2.13)
                        clsInfo.m_strFileUrl.assign( (const char *)b + i + 4, iLen - 1 );
                } else if ( iei == 0x79 ) {
                    // Metadata = RFC 5547 file-selector 문자열: name:"..." size:N type:MIME
                    std::string meta( (const char *)b + i + 3, iLen );
                    size_t np = meta.find( "name:\"" );
                    if ( np != std::string::npos ) {
                        size_t ne = meta.find( '"', np + 6 );
                        if ( ne != std::string::npos ) clsInfo.m_strFileName = meta.substr( np + 6, ne - np - 6 );
                    }
                    size_t sp = meta.find( "size:" );
                    if ( sp != std::string::npos ) clsInfo.m_llFileSize = atoll( meta.c_str() + sp + 5 );
                    size_t tp = meta.find( "type:" );
                    if ( tp != std::string::npos ) {
                        size_t te = meta.find_first_of( " \r\n", tp + 5 );
                        clsInfo.m_strFileType =
                            meta.substr( tp + 5, ( te == std::string::npos ? meta.size() : te ) - tp - 5 );
                    }
                }
                i += 3 + iLen;
            } else {
                break;
            }
        }
        return true;
    }

    if ( iType == MCDATA_MSG_SDS_NOTIFICATION ) {
        // [type1][notif1][datetime5][conv16][msg16] = 39 octets
        if ( n < 39 ) return false;
        clsInfo.m_iMsgType = iType;
        clsInfo.m_iNotifType = b[1];
        clsInfo.m_tSentTime = 0;
        for ( int i = 2; i <= 6; ++i ) clsInfo.m_tSentTime = ( clsInfo.m_tSentTime << 8 ) | b[i];
        clsInfo.m_strConvId = _hex( b + 7, 16 );
        clsInfo.m_strMsgId = _hex( b + 23, 16 );
        return true;
    }

    return false;
}

/** DATA PAYLOAD (§15.1.4) 파싱 — payload 크기 합 + 첫 TEXT payload */
static void _parseDataPayload( const std::string &bin, CMcDataSdsInfo &clsInfo ) {
    const unsigned char *b = (const unsigned char *)bin.data();
    int n = (int)bin.size();
    if ( n < 2 || ( b[0] & 0x3F ) != MCDATA_MSG_DATA_PAYLOAD ) return;

    int i = 2;  // [type][number-of-payloads]
    while ( i + 3 <= n ) {
        unsigned char iei = b[i];
        int iLen = ( b[i + 1] << 8 ) | b[i + 2];
        if ( i + 3 + iLen > n ) break;
        if ( iei == 0x78 ) {  // Payload (TLV-E): [content-type 1][data]
            if ( iLen >= 1 ) {
                clsInfo.m_iPayloadSize += iLen - 1;
                if ( b[i + 3] == 0x01 && clsInfo.m_strText.empty() )  // TEXT
                    clsInfo.m_strText.assign( (const char *)b + i + 4, iLen - 1 );
            }
        } else if ( iei == 0x7A ) {  // Security parameters and Payload (TS 33.180)
            clsInfo.m_iPayloadSize += iLen;
        }
        i += 3 + iLen;
    }
}

/** mcdata-info+xml 에서 <mcdata-request-uri> 의 <mcdataURI> 추출 */
static void _parseMcDataInfo( const std::string &xml, CMcDataSdsInfo &clsInfo ) {
    size_t req = xml.find( "<mcdata-request-uri" );
    if ( req == std::string::npos ) return;
    size_t uriB = xml.find( "<mcdataURI>", req );
    if ( uriB == std::string::npos ) return;
    uriB += 11;
    size_t uriE = xml.find( "</mcdataURI>", uriB );
    if ( uriE == std::string::npos ) return;
    clsInfo.m_strGroupUri = _trim( xml.substr( uriB, uriE - uriB ) );
}

// ── 공개 API ──────────────────────────────────────────────

bool McDataIsMultipartMixed( const std::string &strContentType ) {
    return _lower( strContentType ).find( "multipart/mixed" ) != std::string::npos;
}

bool McDataParseBody( const std::string &strContentType, const std::string &strBody, CMcDataSdsInfo &clsInfo ) {
    std::string strBoundary = _boundaryFromContentType( strContentType );
    if ( strBoundary.empty() ) strBoundary = _boundaryFromBody( strBody );
    if ( strBoundary.empty() ) return false;

    std::vector<SMimePart> vecParts;
    if ( !_splitMultipart( strBody, strBoundary, vecParts ) ) return false;

    bool bSignalling = false;
    for ( size_t i = 0; i < vecParts.size(); ++i ) {
        const std::string &ct = vecParts[i].strContentType;
        if ( ct == "application/vnd.3gpp.mcdata-signalling" ) {
            bSignalling = _parseSignalling( vecParts[i].strContent, clsInfo ) || bSignalling;
        } else if ( ct == "application/vnd.3gpp.mcdata-payload" ) {
            _parseDataPayload( vecParts[i].strContent, clsInfo );
        } else if ( ct == "application/vnd.3gpp.mcdata-info+xml" ) {
            _parseMcDataInfo( vecParts[i].strContent, clsInfo );
        }
    }
    return bSignalling;
}
