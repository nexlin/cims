#include "MediaSdes.h"

#include <openssl/rand.h>

#include "Base64.h"
#include "Log.h"

namespace MediaSdes {

    // SRTP master key/salt 길이 (RFC 3711 §8.2 기본 프로파일)
    static const int kKeyLen = 16;
    static const int kSaltLen = 14;

    bool IsSupportedSuite( const std::string &strSuite ) {
        return strSuite == "AES_CM_128_HMAC_SHA1_80" || strSuite == "AES_CM_128_HMAC_SHA1_32";
    }

    std::string GenerateInlineKeyB64() {
        unsigned char arrKey[kKeyLen + kSaltLen];
        if ( RAND_bytes( arrKey, sizeof( arrKey ) ) != 1 ) {
            CLog::Print( LOG_ERROR, "MediaSdes: RAND_bytes failed — no SRTP key" );
            return "";
        }
        std::string strOut;
        if ( !Base64Encode( (const char *)arrKey, sizeof( arrKey ), strOut ) ) return "";
        return strOut;
    }

    /** base64 인라인 키 디코드 — 정확히 30B 면 out 에 담고 true */
    static bool _decodeInline( const std::string &strB64, std::string &strOut ) {
        if ( strB64.empty() || strB64.size() > 128 ) return false;
        char szBuf[128];
        int iLen = Base64Decode( strB64.c_str(), (int)strB64.size(), szBuf, sizeof( szBuf ) );
        if ( iLen != kKeyLen + kSaltLen ) return false;
        strOut.assign( szBuf, iLen );
        return true;
    }

    bool ValidInlineKeyB64( const std::string &strInlineB64 ) {
        std::string strTmp;
        return _decodeInline( strInlineB64, strTmp );
    }

    /** 30B 평문 → key/salt 각각 base64 */
    static bool _splitToB64( const std::string &strRaw, std::string &strKeyB64, std::string &strSaltB64 ) {
        if ( (int)strRaw.size() != kKeyLen + kSaltLen ) return false;
        if ( !Base64Encode( strRaw.data(), kKeyLen, strKeyB64 ) ) return false;
        if ( !Base64Encode( strRaw.data() + kKeyLen, kSaltLen, strSaltB64 ) ) return false;
        return true;
    }

    bool BuildCmpKeys( const std::string &strSuite, const std::string &strUeInlineB64,
                       const std::string &strSrvInlineB64, CmpMediaCrypto &clsOut ) {
        clsOut = CmpMediaCrypto();
        if ( !IsSupportedSuite( strSuite ) ) return false;
        std::string strUeRaw, strSrvRaw;
        if ( !_decodeInline( strUeInlineB64, strUeRaw ) || !_decodeInline( strSrvInlineB64, strSrvRaw ) ) return false;
        if ( !_splitToB64( strUeRaw, clsOut.strRxKey, clsOut.strRxSalt ) ) return false;
        if ( !_splitToB64( strSrvRaw, clsOut.strTxKey, clsOut.strTxSalt ) ) return false;
        clsOut.strAlg = strSuite;
        clsOut.bEnabled = true;
        return true;
    }

}  // namespace MediaSdes
