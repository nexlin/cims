#ifndef _MCPTT_SDP_H_
#define _MCPTT_SDP_H_

// MCPTT 호의 SDP 읽기 부품 — 헤더 전용, psip SDP 목록(SDP_MEDIA_LIST)을 읽는다. ModuleDispatcher 가 개별 호 offer 에서
//   floor 유무를 정할 때 쓰고, tests/csp_mcptt_request_test.cpp 가 요청 형식 골든(tests/fixtures/mcptt/sip/)으로
//   검증한다.

#include <strings.h>

#include "SdpMedia.h"

/** 발언권 제어 채널(media-floor control entity) — `m=application <port> udp MCPTT`(TS 24.380 표 4.3.3.1-1)를 offer 가
 *  제안했는가. 포트 0 은 거절된 스트림이라 제안이 아니다(RFC 3264 §5.1). proto·fmt 는 대소문자를 가리지 않는다.
 *  개별 호 offer 에 이 줄이 없으면 floor 없는 개별 호다(TS 24.379 §11.1.2.2 1) · §11.1.2.3.1) — fmtp `mc_no_floor_ctrl`
 *  은 pre-established session 의 표시(TS 24.380 §14.2.6 · TS 24.379 §11.1.2.3.1 둘째 문단)라 on-demand 호의 floor
 * 유무로 읽지 않는다. */
inline bool McpttFloorChannelOffered( const SDP_MEDIA_LIST &clsList ) {
    for ( const auto &clsMedia : clsList ) {
        if ( strcasecmp( clsMedia.m_strMedia.c_str(), "application" ) != 0 ) continue;
        if ( strcasecmp( clsMedia.m_strProtocol.c_str(), "udp" ) != 0 ) continue;
        for ( const auto &strFmt : clsMedia.m_clsFmtList )
            if ( strcasecmp( strFmt.c_str(), "MCPTT" ) == 0 ) return clsMedia.m_iPort > 0;
    }
    return false;
}

#endif
