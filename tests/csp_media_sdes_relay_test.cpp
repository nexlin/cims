/*
 * csp_media_sdes_relay_test — MediaSdes 의 VoLTE relay SDP 리스트 조작 단위시험
 * (ReadOfferCrypto / StripCrypto / ApplyCrypto — media_security.md §5.2)
 *
 * 빌드 (레포 루트):
 *   g++ -std=c++17 -Icsp -Iext/psip/SdpParser -Iext/psip/SipPlatform \
 *       tests/csp_media_sdes_relay_test.cpp csp/MediaSdes.cpp \
 *       ext/psip/SdpParser/SdpAttributeCrypto.cpp ext/psip/SdpParser/SdpMedia.cpp \
 *       ext/psip/SdpParser/SdpAttribute.cpp ext/psip/SdpParser/SdpConnection.cpp \
 *       ext/psip/SdpParser/SdpBandWidth.cpp ext/psip/SipPlatform/Base64.cpp \
 *       ext/psip/SipPlatform/Log.cpp ext/psip/SipPlatform/SipMutex.cpp \
 *       ext/psip/SipPlatform/FileUtility.cpp ext/psip/SipPlatform/StringUtility.cpp \
 *       ext/psip/SipPlatform/TimeUtility.cpp ext/psip/SipPlatform/TimeString.cpp \
 *       ext/psip/SipPlatform/Directory.cpp \
 *       -lcrypto -lpthread -o /tmp/csp_media_sdes_relay_test && /tmp/csp_media_sdes_relay_test
 */
#include <cassert>
#include <cstdio>
#include <cstring>

#include "MediaSdes.h"

static int g_iPass = 0;

#define CHECK( cond, name )                          \
    do {                                             \
        if ( cond ) {                                \
            ++g_iPass;                               \
            printf( "PASS %s\n", name );             \
        } else {                                     \
            printf( "FAIL %s (line %d)\n", name, __LINE__ ); \
            return 1;                                \
        }                                            \
    } while ( 0 )

static CSdpMedia _makeAudio( int iPort, const char *pszProto ) {
    return CSdpMedia( "audio", iPort, pszProto );
}

int main() {
    const std::string strKey = MediaSdes::GenerateInlineKeyB64();
    CHECK( !strKey.empty() && MediaSdes::ValidInlineKeyB64( strKey ), "keygen" );

    // ── ReadOfferCrypto: SAVP + 유효 crypto ──
    {
        SDP_MEDIA_LIST clsList;
        CSdpMedia clsAudio = _makeAudio( 4000, "RTP/SAVP" );
        clsAudio.AddAttribute( "crypto", ( "1 AES_CM_128_HMAC_SHA1_80 inline:" + strKey ).c_str() );
        clsList.push_back( clsAudio );
        std::string strTag, strSuite, strInline, strProto;
        CHECK( MediaSdes::ReadOfferCrypto( clsList, "audio", strTag, strSuite, strInline, strProto ) == 1 &&
                   strTag == "1" && strSuite == "AES_CM_128_HMAC_SHA1_80" && strInline == strKey &&
                   strProto == "RTP/SAVP",
               "read_savp_valid" );
    }

    // ── ReadOfferCrypto: SAVP 인데 지원 불가 suite 만 → -1 (성립 불가) ──
    {
        SDP_MEDIA_LIST clsList;
        CSdpMedia clsAudio = _makeAudio( 4000, "RTP/SAVP" );
        clsAudio.AddAttribute( "crypto", ( "1 AEAD_AES_256_GCM inline:" + strKey ).c_str() );
        clsList.push_back( clsAudio );
        std::string strTag, strSuite, strInline, strProto;
        CHECK( MediaSdes::ReadOfferCrypto( clsList, "audio", strTag, strSuite, strInline, strProto ) == -1,
               "read_savp_unsupported_suite" );
    }

    // ── ReadOfferCrypto: AVP + crypto(best-effort) → 1, AVP 단독 → 0 ──
    {
        SDP_MEDIA_LIST clsList;
        CSdpMedia clsAudio = _makeAudio( 4000, "RTP/AVP" );
        clsAudio.AddAttribute( "crypto", ( "2 AES_CM_128_HMAC_SHA1_32 inline:" + strKey ).c_str() );
        clsList.push_back( clsAudio );
        std::string strTag, strSuite, strInline, strProto;
        CHECK( MediaSdes::ReadOfferCrypto( clsList, "audio", strTag, strSuite, strInline, strProto ) == 1 &&
                   strTag == "2" && strSuite == "AES_CM_128_HMAC_SHA1_32",
               "read_avp_besteffort" );

        SDP_MEDIA_LIST clsPlain;
        clsPlain.push_back( _makeAudio( 4000, "RTP/AVP" ) );
        CHECK( MediaSdes::ReadOfferCrypto( clsPlain, "audio", strTag, strSuite, strInline, strProto ) == 0 &&
                   strProto == "RTP/AVP",
               "read_avp_plain" );
    }

    // ── ReadOfferCrypto: 비활성(port 0)/부재 미디어 → 0 + 빈 proto ──
    {
        SDP_MEDIA_LIST clsList;
        clsList.push_back( _makeAudio( 0, "RTP/AVP" ) );
        std::string strTag, strSuite, strInline, strProto;
        CHECK( MediaSdes::ReadOfferCrypto( clsList, "audio", strTag, strSuite, strInline, strProto ) == 0 &&
                   strProto.empty(),
               "read_inactive" );
        CHECK( MediaSdes::ReadOfferCrypto( clsList, "video", strTag, strSuite, strInline, strProto ) == 0 &&
                   strProto.empty(),
               "read_absent" );
    }

    // ── StripCrypto + ApplyCrypto 왕복 (구간 종단 재작성) ──
    {
        SDP_MEDIA_LIST clsList;
        CSdpMedia clsAudio = _makeAudio( 4000, "RTP/SAVP" );
        clsAudio.AddAttribute( "crypto", ( "1 AES_CM_128_HMAC_SHA1_80 inline:" + strKey ).c_str() );
        clsAudio.AddAttribute( "rtpmap", "96 AMR-WB/16000" );
        clsList.push_back( clsAudio );

        MediaSdes::StripCrypto( clsList );
        std::string strTag, strSuite, strInline, strProto;
        CHECK( MediaSdes::ReadOfferCrypto( clsList, "audio", strTag, strSuite, strInline, strProto ) == -1,
               "strip_removes_crypto" );  // SAVP 인데 crypto 없음 = -1

        // 새 leg 키로 재작성 (offer: SAVP + 서버 키)
        const std::string strSrvKey = MediaSdes::GenerateInlineKeyB64();
        MediaSdes::ApplyCrypto( clsList, "audio", "RTP/SAVP", "1", "AES_CM_128_HMAC_SHA1_80", strSrvKey );
        CHECK( MediaSdes::ReadOfferCrypto( clsList, "audio", strTag, strSuite, strInline, strProto ) == 1 &&
                   strInline == strSrvKey && strProto == "RTP/SAVP",
               "apply_rewrites_key" );
        // rtpmap 등 여타 속성은 보존
        bool bRtpmap = false;
        for ( const auto &a : clsList.front().m_clsAttributeList )
            if ( a.m_strName == "rtpmap" ) bRtpmap = true;
        CHECK( bRtpmap, "apply_preserves_other_attrs" );

        // 평문 정규화 (protocol 강등 + crypto 미부여)
        MediaSdes::StripCrypto( clsList );
        MediaSdes::ApplyCrypto( clsList, "audio", "RTP/AVP", "", "", "" );
        CHECK( MediaSdes::ReadOfferCrypto( clsList, "audio", strTag, strSuite, strInline, strProto ) == 0 &&
                   strProto == "RTP/AVP",
               "apply_plaintext_normalize" );
    }

    // ── BuildCmpKeys 왕복 (기존 계약 회귀) ──
    {
        CmpMediaCrypto clsOut;
        CHECK( MediaSdes::BuildCmpKeys( "AES_CM_128_HMAC_SHA1_80", strKey, MediaSdes::GenerateInlineKeyB64(),
                                        clsOut ) &&
                   clsOut.bEnabled,
               "build_cmp_keys" );
        CHECK( !MediaSdes::BuildCmpKeys( "AES_CM_128_HMAC_SHA1_80", "notbase64!", strKey, clsOut ),
               "build_cmp_keys_reject" );
    }

    printf( "ALL %d PASS\n", g_iPass );
    return 0;
}
