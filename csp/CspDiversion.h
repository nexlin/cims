#ifndef _CSP_DIVERSION_H_
#define _CSP_DIVERSION_H_

#include <string>
#include <vector>

/**
 * @ingroup CspServer
 * @brief 착신전환(Communication Diversion, TS 24.604) — History-Info(RFC 7044)·cause(RFC 4458) 순수 헬퍼.
 *
 *   서버측 전환의 시그널링 표현: 전환을 수행한 AS 가 B-leg INVITE 에 History-Info 를 실어 재타게팅 이력을 남긴다.
 *   hi-entry = <hi-targeted-to-uri>;index=…[;mp=…]. 전환 대상 URI 는 `cause` URI 파라미터(RFC 4458 §3 — 302 CFU ·
 *   486 CFB · 408 CFNR · 404 CFNL · 503 CFNRc · 480/487 deflection)를 갖고, `mp`(mapped-from, RFC 7044 §9.2)가
 *   전환 전 항목의 index 를 가리킨다. 전환 횟수 판정(TS 24.604 §4.5.2.6 — 상한 초과는 486)은 cause 를 가진 항목 수.
 *   SIP 스택·전역 상태에 의존하지 않는다(S1-UNIT-CSP tests/csp_diversion_test.cpp).
 */
namespace CspDiversion {

    enum ECause {
        CAUSE_UNCONDITIONAL = 302,         // CFU
        CAUSE_BUSY = 486,                  // CFB
        CAUSE_NO_ANSWER = 408,             // CFNR
        CAUSE_NOT_REGISTERED = 404,        // CFNL(not logged-in)
        CAUSE_NOT_REACHABLE = 503,         // CFNRc
        CAUSE_DEFLECTION_IMMEDIATE = 480,  // CD (immediate response)
        CAUSE_DEFLECTION_ALERTING = 487,   // CD (during alerting)
    };

    /** 전환 한 홉 — 전환 대상(사용자 부분)과 원인 */
    struct Hop {
        std::string strUser;
        int iCause = CAUSE_UNCONDITIONAL;
    };

    /** 헤더 값의 hi-entry 열(`,` 분리 — `<…>` 안의 쉼표는 나누지 않는다). 빈 값이면 빈 열 */
    std::vector<std::string> SplitEntries( const std::string &strHistoryInfo );
    /** 전환 수 = hi-targeted-to-uri 에 `cause=` 를 가진 항목 수 */
    int CountDiversions( const std::string &strHistoryInfo );
    /** 마지막 항목의 index 파라미터(예 "1.1"). 없으면 빈 값 */
    std::string LastIndex( const std::string &strHistoryInfo );
    /** sip:<user>@<domain> (user 가 이미 URI 이면 그대로) */
    std::string MakeUri( const std::string &strUser, const std::string &strDomain );
    /** History-Info 값 조립 — strExisting(수신 INVITE 의 값, 비면 served 항목 index=1 부터) 뒤에 hop 열을 이어 붙인다.
     *  각 hop = <sip:target@domain;cause=N>;index=<parent>.1;mp=<parent>. strServed = 이 CSP 가 받은 착신(첫 diverting
     * user). */
    std::string BuildHistoryInfo( const std::string &strExisting, const std::string &strDomain,
                                  const std::string &strServed, const std::vector<Hop> &vecHops );
    /** 로그·CDR 용 요약 "B → C → D" */
    std::string ChainLabel( const std::string &strServed, const std::vector<Hop> &vecHops );

}  // namespace CspDiversion

#endif
