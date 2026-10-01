#ifndef _MC_SERVICE_H_
#define _MC_SERVICE_H_

#include <strings.h>

#include <string>

#include "SipHeader.h"

// ── MC 서비스 축 (TS 23.280 §3 — 한 그룹 = 서비스 집합) — docs/design/features/mcvideo.md §3 3 ──
//  호·affiliation·세션·로그의 서비스를 명시한다. MCPTT 와 MCVideo 는 같은 그룹 id·같은 MC service ID(TS 23.280
//  §10.1.4.1)를 쓰지만 ICSI·affiliation·호·미디어 제어가 따로다(TS 23.280 §5.2.5, TS 24.281 §7.1).

enum class EMcService { Mcptt, McVideo };

inline const char *McServiceName( EMcService e ) {
    return e == EMcService::McVideo ? "mcvideo" : "mcptt";
}

/** 서비스별 affiliation 표 (sql/migrate_mcvideo.sql — 공유 DB 의 옛 코드와 섞이지 않게 표를 나눴다) */
inline const char *McAffiliationTable( EMcService e ) {
    return e == EMcService::McVideo ? "mcvideo_affiliations" : "ptt_affiliations";
}

/** 요청의 Session-Expires 에서 refresher 파라미터를 뺀다 — 스택은 로컬 정책대로 refresher 를 제안하므로
 * (RFC 4028 §7.1) 규격이 생략을 정한 제어 기능의 단말 초대(MCPTT TS 24.379 §6.3.3.1.2 6) · MCVideo TS 24.281
 * §6.3.3.1.2 6) «The refresher parameter shall be omitted»)는 만든 뒤 지운다 — 단말이 200 OK 에서
 * refresher=uas 로 정한다(두 규격 §6.2.3.1.1 5)). 다른 파라미터는 둔다. */
inline void McStripSessionRefresher( SIP_HEADER_LIST &clsHeaders ) {
    for ( auto &h : clsHeaders ) {
        if ( strcasecmp( h.m_strName.c_str(), "Session-Expires" ) != 0 ) continue;
        const size_t k = h.m_strValue.find( ";refresher=" );
        if ( k == std::string::npos ) continue;
        const size_t e = h.m_strValue.find( ';', k + 1 );
        h.m_strValue.erase( k, e == std::string::npos ? std::string::npos : e - k );
    }
}

#endif  // _MC_SERVICE_H_
