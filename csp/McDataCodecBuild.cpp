/*
 * MCData 본문 생성기 (CSP 전용 — cmdp 는 McDataCodec.cpp 의 파서만 공유).
 *
 * FILEURL 폴백 배포용 FD SIGNALLING PAYLOAD multipart 를 앱 코덱
 * (android McDataCodec.kt buildGroupFd)과 바이트 호환으로 생성한다:
 * base64 CTE(레포 편차 — PJSIP Java String 본문 제약), mcdata-info + signalling 2파트.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "Base64.h"
#include "McDataCodec.h"
#include "SipMd5.h"

// 그룹 상시 대화 Conversation ID — Java UUID.nameUUIDFromBytes("cims-mcdata:<gid>") 동형:
// MD5 후 version(3)/variant 비트 세팅 (docs/design/features/mcdata_messaging.md §3)
std::string McDataConversationIdOf( const std::string &strGroupId ) {
    std::string strSeed = "cims-mcdata:" + strGroupId;
    unsigned char digest[16];
    SipMd5Byte( strSeed.c_str(), digest );
    digest[6] = (unsigned char)( ( digest[6] & 0x0f ) | 0x30 );  // version 3
    digest[8] = (unsigned char)( ( digest[8] & 0x3f ) | 0x80 );  // IETF variant
    char szHex[33];
    for ( int i = 0; i < 16; ++i ) snprintf( szHex + i * 2, 3, "%02x", digest[i] );
    return std::string( szHex, 32 );
}

std::string McDataNewMessageId() {
    unsigned char raw[16];
    bool bOk = false;
    FILE *fp = fopen( "/dev/urandom", "rb" );
    if ( fp ) {
        bOk = ( fread( raw, 1, sizeof( raw ), fp ) == sizeof( raw ) );
        fclose( fp );
    }
    if ( !bOk ) {
        struct timespec ts;
        clock_gettime( CLOCK_REALTIME, &ts );
        srandom( (unsigned)( ts.tv_nsec ^ ts.tv_sec ) );
        for ( size_t i = 0; i < sizeof( raw ); ++i ) raw[i] = (unsigned char)( random() & 0xff );
    }
    raw[6] = (unsigned char)( ( raw[6] & 0x0f ) | 0x40 );  // version 4
    raw[8] = (unsigned char)( ( raw[8] & 0x3f ) | 0x80 );  // IETF variant
    char szHex[33];
    for ( int i = 0; i < 16; ++i ) snprintf( szHex + i * 2, 3, "%02x", raw[i] );
    return std::string( szHex, 32 );
}

static void _hexToBytes16( const std::string &strHex, unsigned char out[16] ) {
    memset( out, 0, 16 );
    for ( int i = 0; i < 16 && (size_t)( i * 2 + 1 ) < strHex.size(); ++i ) {
        unsigned int v = 0;
        sscanf( strHex.c_str() + i * 2, "%2x", &v );
        out[i] = (unsigned char)v;
    }
}

std::string McDataBuildFdSignallingBody( std::string &strContentTypeOut, const std::string &strGroupUri,
                                         const std::string &strFileUrl, const std::string &strFileName,
                                         long long llFileSize, const std::string &strFileType,
                                         const std::string &strConvId, const std::string &strMsgId ) {
    // ── FD SIGNALLING PAYLOAD TLV (TS 24.282 §15.1.3, 앱 buildGroupFd 동형) ──
    std::string strName = strFileName;
    for ( size_t p; ( p = strName.find( '"' ) ) != std::string::npos; ) strName.erase( p, 1 );
    std::string strMeta = "name:\"" + strName + "\" size:" + std::to_string( llFileSize ) + " type:" + strFileType;

    std::string tlv;
    tlv.reserve( 38 + 4 + strFileUrl.size() + 3 + strMeta.size() );
    tlv += (char)MCDATA_MSG_FD_SIGNALLING;
    time_t tNow = time( NULL );
    for ( int i = 4; i >= 0; --i ) tlv += (char)( ( (long long)tNow >> ( i * 8 ) ) & 0xff );  // Date-time 5B
    unsigned char uuid[16];
    _hexToBytes16( strConvId, uuid );
    tlv.append( (const char *)uuid, 16 );
    _hexToBytes16( strMsgId, uuid );
    tlv.append( (const char *)uuid, 16 );
    int iUrlLen = 1 + (int)strFileUrl.size();  // content-type(FILEURL) + url
    tlv += (char)0x78;                         // Payload IEI (TLV-E)
    tlv += (char)( ( iUrlLen >> 8 ) & 0xff );
    tlv += (char)( iUrlLen & 0xff );
    tlv += (char)0x04;  // FILEURL
    tlv += strFileUrl;
    tlv += (char)0x79;  // Metadata IEI (TLV-E)
    tlv += (char)( ( strMeta.size() >> 8 ) & 0xff );
    tlv += (char)( strMeta.size() & 0xff );
    tlv += strMeta;

    std::string strTlvB64;
    Base64Encode( tlv.data(), (int)tlv.size(), strTlvB64 );

    // ── mcdata-info+xml (앱 mcDataInfoXml 동형) ──
    std::string strInfo =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<mcdatainfo xmlns=\"urn:3gpp:ns:mcdataInfo:1.0\">\n"
        "  <mcdata-Params>\n"
        "    <request-type>group-sds</request-type>\n"
        "    <mcdata-request-uri type=\"Normal\"><mcdataURI>" +
        strGroupUri +
        "</mcdataURI></mcdata-request-uri>\n"
        "  </mcdata-Params>\n"
        "</mcdatainfo>";

    std::string strBoundary = "mcdata-fd-" + strMsgId.substr( 0, 14 );
    std::string strBody;
    strBody += "--" + strBoundary + "\r\nContent-Type: application/vnd.3gpp.mcdata-info+xml\r\n\r\n" + strInfo + "\r\n";
    strBody += "--" + strBoundary +
               "\r\nContent-Type: application/vnd.3gpp.mcdata-signalling\r\n"
               "Content-Transfer-Encoding: base64\r\n\r\n" +
               strTlvB64 + "\r\n";
    strBody += "--" + strBoundary + "--\r\n";

    strContentTypeOut = "multipart/mixed;boundary=" + strBoundary;
    return strBody;
}
