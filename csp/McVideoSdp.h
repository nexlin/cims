#ifndef _MCVIDEO_SDP_H_
#define _MCVIDEO_SDP_H_

// MCVideo 그룹 호의 SDP·헤더 읽기 부품 (mcvideo.md §1.4 K4 · §5.2.1) — 헤더 전용, psip SDP 목록(SDP_MEDIA_LIST)을
//   읽는다. McVideoCallService 가 offer/answer 에서 JOIN ② 선언을 만들 때 쓰고, tests/csp_mcvideo_sdp_test.cpp 가 K3
//   골든 SDP 로 검증한다.

#include <strings.h>

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "McVideoInfo.h"
#include "SdpMedia.h"
#include "SipHeader.h"

/** 키프레임 요청 비트 — `a=rtcp-fb:<pt|*> nack pli`(RFC 4585 §4.2) · `ccm fir`(RFC 5104 §7.1) */
static const int kMcvFbPli = 1;
static const int kMcvFbFir = 2;

/** 전송 제어 채널 — `m=application <port> udp MCVideo`(TS 24.581 §12.1.2 · 표 4.3.3.1-1). `a=fmtp:MCVideo` 는
 * 선택(없으면 clsFmtp.bPresent = false). 채널이 있으면 true, 포트는 m= 줄 값(0 = 거절, RFC 3264 §6). */
inline bool McvControlChannel( const SDP_MEDIA_LIST &clsList, int &iPort, CMcVideoFmtp &clsFmtp ) {
    iPort = 0;
    clsFmtp = CMcVideoFmtp();
    for ( const auto &clsMedia : clsList ) {
        if ( strcasecmp( clsMedia.m_strMedia.c_str(), "application" ) != 0 ) continue;
        if ( strcasecmp( clsMedia.m_strProtocol.c_str(), "udp" ) != 0 ) continue;
        bool bMcv = false;
        for ( const auto &strFmt : clsMedia.m_clsFmtList )
            if ( strcasecmp( strFmt.c_str(), "MCVideo" ) == 0 ) bMcv = true;
        if ( !bMcv ) continue;
        iPort = clsMedia.m_iPort;
        for ( const auto &clsAttr : clsMedia.m_clsAttributeList ) {
            if ( strcasecmp( clsAttr.m_strName.c_str(), "fmtp" ) != 0 ) continue;
            if ( strncasecmp( clsAttr.m_strValue.c_str(), "MCVideo", 7 ) != 0 ) continue;
            clsFmtp = ParseMcVideoFmtp( clsAttr.m_strValue.substr( 7 ) );
            break;
        }
        return true;
    }
    return false;
}

/** 첫 m=<pszMedia> 의 a=ssrc 첫 값(RFC 5576) · rtpmap 인코딩 이름이 pszEncoding(예 "AMR-WB", "H264")인 PT — 없으면 0 */
inline void McvMediaSsrcPt( const SDP_MEDIA_LIST &clsList, const char *pszMedia, const char *pszEncoding,
                            unsigned int &uSsrc, int &iPt ) {
    uSsrc = 0;
    iPt = 0;
    const size_t nEnc = strlen( pszEncoding );
    for ( const auto &clsMedia : clsList ) {
        if ( strcasecmp( clsMedia.m_strMedia.c_str(), pszMedia ) != 0 ) continue;
        for ( const auto &clsAttr : clsMedia.m_clsAttributeList ) {
            if ( uSsrc == 0 && strcasecmp( clsAttr.m_strName.c_str(), "ssrc" ) == 0 )
                uSsrc = (unsigned int)strtoul( clsAttr.m_strValue.c_str(), nullptr, 10 );
            if ( iPt == 0 && strcasecmp( clsAttr.m_strName.c_str(), "rtpmap" ) == 0 ) {
                // "<pt> <enc>/<clock>[/<ch>]" — 인코딩 이름은 '/' 앞까지 정확히 같아야 한다(AMR-WB 와 AMR-WB+ 를
                //   가른다)
                const std::string &v = clsAttr.m_strValue;
                const size_t sp = v.find( ' ' );
                if ( sp != std::string::npos && strncasecmp( v.c_str() + sp + 1, pszEncoding, nEnc ) == 0 &&
                     ( v.size() == sp + 1 + nEnc || v[sp + 1 + nEnc] == '/' ) )
                    iPt = atoi( v.c_str() );
            }
        }
        return;
    }
}

/** 첫 m=video 의 a=rtcp-fb 가운데 이 PT(또는 *)에 걸린 키프레임 요청 비트(kMcvFbPli·kMcvFbFir). CMP 는 송출자에게
 * 협상한 것만 보낸다(RFC 4585 §4.2 — JOIN user_video_fb). 일반 `nack`(재전송 요청)·`trr-int` 는 세지 않는다. */
inline int McvVideoFeedback( const SDP_MEDIA_LIST &clsList, int iVideoPt ) {
    int fb = 0;
    for ( const auto &clsMedia : clsList ) {
        if ( strcasecmp( clsMedia.m_strMedia.c_str(), "video" ) != 0 ) continue;
        for ( const auto &clsAttr : clsMedia.m_clsAttributeList ) {
            if ( strcasecmp( clsAttr.m_strName.c_str(), "rtcp-fb" ) != 0 ) continue;
            std::istringstream is( clsAttr.m_strValue );
            std::string strPt, strType, strParam;
            is >> strPt >> strType >> strParam;
            if ( strPt != "*" && atoi( strPt.c_str() ) != iVideoPt ) continue;
            if ( strcasecmp( strType.c_str(), "nack" ) == 0 && strcasecmp( strParam.c_str(), "pli" ) == 0 )
                fb |= kMcvFbPli;
            if ( strcasecmp( strType.c_str(), "ccm" ) == 0 && strcasecmp( strParam.c_str(), "fir" ) == 0 )
                fb |= kMcvFbFir;
        }
        break;
    }
    return fb;
}

/** 요청의 Session-Expires 에서 refresher 파라미터를 뺀다 — 스택은 로컬 정책대로 refresher 를 제안하므로(RFC 4028 §7.1)
 * 규격이 생략을 정한 요청(TS 24.281 §6.3.3.1.2 6) «The refresher parameter shall be omitted»)은 만든 뒤 지운다. 다른
 * 파라미터는 둔다. */
inline void McvStripSessionRefresher( SIP_HEADER_LIST &clsHeaders ) {
    for ( auto &h : clsHeaders ) {
        if ( strcasecmp( h.m_strName.c_str(), "Session-Expires" ) != 0 ) continue;
        const size_t k = h.m_strValue.find( ";refresher=" );
        if ( k == std::string::npos ) continue;
        const size_t e = h.m_strValue.find( ';', k + 1 );
        h.m_strValue.erase( k, e == std::string::npos ? std::string::npos : e - k );
    }
}

#endif
