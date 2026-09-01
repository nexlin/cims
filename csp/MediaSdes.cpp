#include "MediaSdes.h"

#include <openssl/rand.h>
#include <strings.h>

#include <cstring>

#include "Base64.h"
#include "Log.h"
#include "SdpAttributeCrypto.h"

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

    /** pszMedia 의 첫 active(port>0) m= 라인. 없으면 NULL. */
    static CSdpMedia *_findActiveMedia( SDP_MEDIA_LIST &clsList, const char *pszMedia ) {
        for ( SDP_MEDIA_LIST::iterator it = clsList.begin(); it != clsList.end(); ++it ) {
            if ( strcasecmp( it->m_strMedia.c_str(), pszMedia ) ) continue;
            if ( it->m_iPort <= 0 ) continue;
            return &( *it );
        }
        return NULL;
    }

    int ReadOfferCrypto( const SDP_MEDIA_LIST &clsList, const char *pszMedia, std::string &strTag,
                         std::string &strSuite, std::string &strInline, std::string &strProto ) {
        strTag.clear();
        strSuite.clear();
        strInline.clear();
        strProto.clear();
        CSdpMedia *pclsMedia = _findActiveMedia( const_cast<SDP_MEDIA_LIST &>( clsList ), pszMedia );
        if ( pclsMedia == NULL ) return 0;
        strProto = pclsMedia->m_strProtocol;
        const bool bSavp = strncasecmp( strProto.c_str(), "RTP/SAVP", 8 ) == 0;
        for ( SDP_ATTRIBUTE_LIST::const_iterator it = pclsMedia->m_clsAttributeList.begin();
              it != pclsMedia->m_clsAttributeList.end(); ++it ) {
            if ( strcasecmp( it->m_strName.c_str(), "crypto" ) ) continue;
            CSdpAttributeCrypto clsCrypto;
            if ( clsCrypto.Parse( it->m_strValue.c_str(), (int)it->m_strValue.size() ) <= 0 ) continue;
            if ( !IsSupportedSuite( clsCrypto.m_strCryptoSuite ) ) continue;
            if ( !ValidInlineKeyB64( clsCrypto.m_strKey ) ) continue;
            strTag = clsCrypto.m_strTag;
            strSuite = clsCrypto.m_strCryptoSuite;
            strInline = clsCrypto.m_strKey;
            return 1;
        }
        // 유효 crypto 없음: SAVP 는 crypto 가 필수(RFC 4568)라 성립 불가, AVP 병기는 무시(평문).
        return bSavp ? -1 : 0;
    }

    void StripCrypto( SDP_MEDIA_LIST &clsList ) {
        for ( SDP_MEDIA_LIST::iterator it = clsList.begin(); it != clsList.end(); ++it ) {
            it->DeleteAttribute( "crypto" );
        }
    }

    void ApplyCrypto( SDP_MEDIA_LIST &clsList, const char *pszMedia, const std::string &strProto,
                      const std::string &strTag, const std::string &strSuite, const std::string &strInline ) {
        CSdpMedia *pclsMedia = _findActiveMedia( clsList, pszMedia );
        if ( pclsMedia == NULL ) return;
        if ( !strProto.empty() ) pclsMedia->m_strProtocol = strProto;
        if ( !strInline.empty() ) {
            std::string strValue = ( strTag.empty() ? "1" : strTag ) + " " + strSuite + " inline:" + strInline;
            pclsMedia->AddAttribute( "crypto", strValue.c_str() );
        }
    }

    // ── VoLTE relay leg 종단 — leg 별 SDES 평가·재작성 (media_security.md §5.2) ──

    int EvalRelayOfferSdes( const std::string &strSrtpPolicy, const SDP_MEDIA_LIST &clsList, const char *pszMedia,
                            RelaySdesMedia &clsOut ) {
        std::string strTag, strSuite, strInline, strProto;
        int iRet = ReadOfferCrypto( clsList, pszMedia, strTag, strSuite, strInline, strProto );
        clsOut.strProto = strProto;        // answer protocol echo 근거 (평문 포함)
        if ( strProto.empty() ) return 0;  // 미디어 부재/비활성
        if ( iRet < 0 ) return -1;         // SAVP 인데 유효 crypto 없음 — 폴백 불가
        if ( strSrtpPolicy == "off" ) {
            // off = a=crypto 무시(평문). 단 SAVP 단독 offer 는 평문 answer 가 불가(RFC 4568) → 488.
            return iRet == 1 && strncasecmp( strProto.c_str(), "RTP/SAVP", 8 ) == 0 ? -1 : 0;
        }
        if ( iRet == 0 ) {
            // crypto 없는 offer: required 는 488(SAVP 단일 정책), optional 은 평문 leg 허용.
            return strSrtpPolicy == "required" ? -1 : 0;
        }
        clsOut.bSrtp = true;
        clsOut.strTag = strTag;
        clsOut.strSuite = strSuite;
        clsOut.strUeKey = strInline;
        clsOut.strSrvKey = GenerateInlineKeyB64();
        return clsOut.strSrvKey.empty() ? -1 : 1;
    }

    bool ApplyRelayLegOffer( SDP_MEDIA_LIST &clsList, const char *pszMedia, bool bSrtp, RelaySdesMedia &clsOut ) {
        std::string strTag, strSuite, strInline, strProto;
        ReadOfferCrypto( clsList, pszMedia, strTag, strSuite, strInline, strProto );  // strip 후 — active 판정용
        if ( strProto.empty() ) return true;
        if ( !bSrtp ) {
            ApplyCrypto( clsList, pszMedia, "RTP/AVP", "", "", "" );
            return true;
        }
        clsOut.bSrtp = true;
        clsOut.strTag = "1";
        clsOut.strSuite = "AES_CM_128_HMAC_SHA1_80";  // 기본 제안 (§2)
        clsOut.strProto = "RTP/SAVP";
        clsOut.strSrvKey = GenerateInlineKeyB64();
        if ( clsOut.strSrvKey.empty() ) return false;
        ApplyCrypto( clsList, pszMedia, "RTP/SAVP", clsOut.strTag, clsOut.strSuite, clsOut.strSrvKey );
        return true;
    }

    bool EvalRelayAnswerSdes( const SDP_MEDIA_LIST &clsList, const char *pszMedia, RelaySdesMedia &clsLeg,
                              CmpMediaCrypto &clsOut ) {
        if ( !clsLeg.bSrtp ) return true;
        std::string strTag, strSuite, strInline, strProto;
        int iRet = ReadOfferCrypto( clsList, pszMedia, strTag, strSuite, strInline, strProto );
        if ( strProto.empty() ) {  // 미디어 거절 — 평문(비활성)화
            clsLeg = RelaySdesMedia();
            return true;
        }
        if ( iRet != 1 || strSuite != clsLeg.strSuite ) return false;
        clsLeg.strUeKey = strInline;
        return BuildCmpKeys( clsLeg.strSuite, clsLeg.strUeKey, clsLeg.strSrvKey, clsOut );
    }

    void ReadReinviteSdes( const SDP_MEDIA_LIST &clsList, const char *pszMedia, int iPeerIdx, RelaySdesMedia &clsLeg,
                           CmpMediaCrypto &clsOut ) {
        if ( !clsLeg.bSrtp ) return;
        std::string strTag, strSuite, strInline, strProto;
        int iRet = ReadOfferCrypto( clsList, pszMedia, strTag, strSuite, strInline, strProto );
        if ( iRet != 1 || strSuite != clsLeg.strSuite ) {
            CLog::Print( LOG_ERROR, "MediaSdes: peer%d %s SRTP leg re-offer without matching crypto — 기존 키 유지",
                         iPeerIdx, pszMedia );
            return;
        }
        if ( strInline != clsLeg.strUeKey ) {
            clsLeg.strUeKey = strInline;
            CLog::Print( LOG_INFO, "MediaSdes: peer%d %s SRTP UE rekey", iPeerIdx, pszMedia );
        }
        BuildCmpKeys( clsLeg.strSuite, clsLeg.strUeKey, clsLeg.strSrvKey, clsOut );
    }

    void RewriteRelaySdpForLeg( SDP_MEDIA_LIST &clsList, const RelaySdesLeg &clsLeg, bool bOffer ) {
        StripCrypto( clsList );
        const RelaySdesMedia *arr[2] = { &clsLeg.clsAudio, &clsLeg.clsVideo };
        const char *arrName[2] = { "audio", "video" };
        for ( int i = 0; i < 2; ++i ) {
            const RelaySdesMedia &m = *arr[i];
            std::string strProto = bOffer ? std::string( m.bSrtp ? "RTP/SAVP" : "RTP/AVP" ) : m.strProto;
            ApplyCrypto( clsList, arrName[i], strProto, m.strTag, m.strSuite, m.bSrtp ? m.strSrvKey : std::string() );
        }
    }

}  // namespace MediaSdes
