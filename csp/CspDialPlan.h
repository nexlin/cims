#ifndef __CSP_DIAL_PLAN_H__
#define __CSP_DIAL_PLAN_H__

#include <string>
#include <vector>

class CSipUri;

/**
 * CspDialPlan — 착신 번호 번역(다이얼 플랜). sip_service_model.md §2-10.
 *
 *   TS 24.229 §5.4.3.2: S-CSCF 는 발신 요청의 Request-URI 가 국제형이 아닌 번호(tel URI 의 local number,
 *   `phone-context`)면 로컬 정책으로 E.164 국제형으로 번역하고, 번역할 수 없으면 484 Address Incomplete 를 낸다.
 *   단말은 누른 숫자를 그대로 보낼 수 있으므로(§5.1.2A.1.5 — 국제형 변환은 단말의 선택) 번역 책임은 홈 망(CSP)에 있다.
 *
 *   플랜은 **접속서비스**(발신 가입자의 access_services 레코드)와 **피어 Route**(인바운드 Route — IP-PBX 가 국내형 DID
 * 를 보낼 때)에 둔다. country_code 가 비면 그 원천의 번역은 비활성(국제형만 받는 망 — TS 29.163/29.165 피어의 기본).
 *
 *   번역 결과는 Request-URI(라우팅 키, RFC 3261 §16.6)에만 쓴다 — To 는 표시용이라 손대지 않는다(§8.2.6.2 응답의 To 는
 *   요청 그대로). 이후 가입자 조회·라우팅 규칙(`req_uri_user prefix`)·B-leg 착신은 전부 번역된 번호를 본다.
 */
struct DialPlan {
    std::string country_code;           // E.164 국가코드 digits(예 "82"). 비면 번역 비활성
    std::string national_prefix = "0";  // 국내 트렁크 접두 — 떼고 +국가코드 를 붙인다. 비면 접두 없이 국가코드만 붙인다
    std::string international_prefix = "00";     // 국제 접두 — 떼고 + 를 붙인다
    std::vector<std::string> emergency_numbers;  // 번역하지 않는 긴급번호(비면 기본 112·119)

    bool Enabled() const {
        return !country_code.empty();
    }
};

enum EDialPlanResult {
    DIAL_PLAN_UNCHANGED = 0,     // 이미 global(+E.164) 이거나 번호가 아닌 식별자(그룹 id·adhoc-…) — 그대로
    DIAL_PLAN_TRANSLATED = 1,    // 국내형·국제 접두·phone-context → +E.164 (Request-URI 재작성)
    DIAL_PLAN_SERVICE_CODE = 2,  // 피처코드(*·#)·긴급번호 — 번역 대상이 아니다
    DIAL_PLAN_INCOMPLETE = 3,    // 번역 불가(접두 없는 숫자열 등) → 484 Address Incomplete
    DIAL_PLAN_DISABLED = 4,      // 플랜 미설정(country_code 없음) — 그대로(국제형만 받는 원천)
};

namespace CspDialPlan {

    /** URI 에서 착신 번호를 꺼낸다 — sip: 은 user, tel: 은 host(psip 파서는 '@' 가 없는 tel URI 의 번호를 host 에 둔다)
     *  + `phone-context` 파라미터(RFC 3966 §5.1.5). user 도 host 도 없으면 false. */
    bool ExtractNumber( const CSipUri &clsUri, std::string &strNumber, std::string &strPhoneContext );

    /** URI 의 번호를 바꿔 쓴다 — sip: user / tel: host. */
    void RewriteNumber( CSipUri &clsUri, const std::string &strNumber );

    /** 번호 정규화. strOut 은 결과가 UNCHANGED/DISABLED/SERVICE_CODE/INCOMPLETE 면 입력 그대로, TRANSLATED 면 +E.164.
     *  순서: 피처코드(*·#) → 시각 구분자 제거(RFC 3966 visual-separator) → 번호 아님(문자 포함)은 그대로 → +global →
     *  긴급번호 → phone-context(+접두는 그대로 결합, 도메인 컨텍스트는 호출자가 플랜으로 풀었다) → 플랜(국제 접두 →
     *  국내 접두 → 접두 없는 국가) → 그 외 INCOMPLETE. */
    EDialPlanResult Normalize( const std::string &strDialed, const std::string &strPhoneContext,
                               const DialPlan &clsPlan, std::string &strOut );

    /** "+" 뒤 digits 만인 국제형인가. */
    bool IsGlobalNumber( const std::string &strNumber );

    /** 긴급번호인가 — 플랜 목록(비면 기본 112·119). */
    bool IsEmergencyNumber( const std::string &strDigits, const DialPlan &clsPlan );

    const char *ResultName( EDialPlanResult eResult );

    /** 번호 대역 판정 — 라우팅/ACL 규칙 연산자 `in_range`(sip_service_model.md §2-5). strRange = "lo-hi"(또는 "lo~hi"),
     *  양끝·대상 모두 `+`·시각 구분자를 뗀 digits 로 비교하고 **자릿수가 같을 때만** lo ≤ 번호 ≤ hi. 접두(prefix)로
     * 표현되지 않는 대역(+82210001010~+82210001014 등)을 정확히 가른다. */
    bool InNumberRange( const std::string &strNumber, const std::string &strRange );

}  // namespace CspDialPlan

#endif  // __CSP_DIAL_PLAN_H__
