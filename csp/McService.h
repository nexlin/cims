#ifndef _MC_SERVICE_H_
#define _MC_SERVICE_H_

#include <string>

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

#endif  // _MC_SERVICE_H_
