#include "CspDialPlan.h"

#include <cctype>
#include <cstring>

#include "SipParameter.h"
#include "SipUri.h"

namespace CspDialPlan {

    static bool _AllDigits( const std::string &s, size_t iFrom = 0 ) {
        if ( s.size() <= iFrom ) return false;
        for ( size_t i = iFrom; i < s.size(); ++i ) {
            if ( !isdigit( (unsigned char)s[i] ) ) return false;
        }
        return true;
    }

    static bool _StartsWith( const std::string &s, const std::string &prefix ) {
        return !prefix.empty() && s.size() > prefix.size() && s.compare( 0, prefix.size(), prefix ) == 0;
    }

    /** RFC 3966 visual-separator: "-" / "." / "(" / ")" — 사람이 적는 구분자. 공백도 같이 버린다. */
    static std::string _StripVisualSeparators( const std::string &s ) {
        std::string out;
        out.reserve( s.size() );
        for ( char c : s ) {
            if ( c == '-' || c == '.' || c == '(' || c == ')' || c == ' ' ) continue;
            out.push_back( c );
        }
        return out;
    }

    bool IsGlobalNumber( const std::string &strNumber ) {
        return strNumber.size() > 1 && strNumber[0] == '+' && _AllDigits( strNumber, 1 );
    }

    bool IsEmergencyNumber( const std::string &strDigits, const DialPlan &clsPlan ) {
        if ( clsPlan.emergency_numbers.empty() ) return strDigits == "112" || strDigits == "119";
        for ( const std::string &e : clsPlan.emergency_numbers ) {
            if ( !e.empty() && e == strDigits ) return true;
        }
        return false;
    }

    bool ExtractNumber( const CSipUri &clsUri, std::string &strNumber, std::string &strPhoneContext ) {
        strNumber.clear();
        strPhoneContext.clear();
        const bool bTel = strcasecmp( clsUri.m_strProtocol.c_str(), "tel" ) == 0;
        if ( bTel ) {
            strNumber = clsUri.m_strHost;  // psip: '@' 가 없어 번호가 host 에 파싱된다
        } else {
            strNumber = clsUri.m_strUser;
        }
        if ( strNumber.empty() ) return false;
        // phone-context 는 URI 파라미터 (tel URI · sip URI user=phone 둘 다 같은 자리)
        SIP_PARAMETER_LIST &clsParams = const_cast<SIP_PARAMETER_LIST &>( clsUri.m_clsUriParamList );
        const char *pszCtx = SearchSipParameter( clsParams, "phone-context" );
        if ( pszCtx && pszCtx[0] ) strPhoneContext = pszCtx;
        return true;
    }

    void RewriteNumber( CSipUri &clsUri, const std::string &strNumber ) {
        if ( strcasecmp( clsUri.m_strProtocol.c_str(), "tel" ) == 0 ) {
            clsUri.m_strHost = strNumber;
        } else {
            clsUri.m_strUser = strNumber;
        }
    }

    EDialPlanResult Normalize( const std::string &strDialed, const std::string &strPhoneContext,
                               const DialPlan &clsPlan, std::string &strOut ) {
        strOut = strDialed;
        if ( strDialed.empty() ) return DIAL_PLAN_UNCHANGED;

        // 피처코드 — 도메인 번호계획의 서비스 코드(당겨받기 ** 등). 번역 대상이 아니다
        if ( strDialed[0] == '*' || strDialed[0] == '#' ) return DIAL_PLAN_SERVICE_CODE;

        std::string s = _StripVisualSeparators( strDialed );
        if ( s.empty() ) return DIAL_PLAN_UNCHANGED;

        // 번호가 아닌 식별자(그룹 id "g001"·"adhoc-…"·"priv-…"·별칭) — 그대로. 라우팅 규칙·그룹 조회가 원문으로 본다
        const bool bGlobalForm = ( s[0] == '+' );
        if ( !_AllDigits( s, bGlobalForm ? 1 : 0 ) ) return DIAL_PLAN_UNCHANGED;

        if ( bGlobalForm ) {
            // 이미 국제형 — 시각 구분자만 있었다면 정리한 값을 쓴다
            if ( s == strDialed ) return DIAL_PLAN_UNCHANGED;
            strOut = s;
            return DIAL_PLAN_TRANSLATED;
        }

        // 긴급번호 — TS 24.229 §5.1.6 긴급 절차의 몫. 번역하지 않는다
        if ( IsEmergencyNumber( s, clsPlan ) ) return DIAL_PLAN_SERVICE_CODE;

        // phone-context 가 global-number-digits(+…) 이면 local number 를 그 뒤에 붙인 것이 global number 다
        //   (RFC 3966 §5.1.5). 도메인 컨텍스트는 호출자가 그 도메인의 접속서비스 플랜으로 풀어 clsPlan 에 넘겼다
        if ( !strPhoneContext.empty() && strPhoneContext[0] == '+' ) {
            std::string ctx = _StripVisualSeparators( strPhoneContext );
            if ( IsGlobalNumber( ctx ) ) {
                strOut = ctx + s;
                return DIAL_PLAN_TRANSLATED;
            }
        }

        if ( !clsPlan.Enabled() ) return DIAL_PLAN_DISABLED;

        // 국제 접두(00) → +
        if ( _StartsWith( s, clsPlan.international_prefix ) ) {
            strOut = "+" + s.substr( clsPlan.international_prefix.size() );
            return DIAL_PLAN_TRANSLATED;
        }
        // 국내 접두(0) → +국가코드
        if ( clsPlan.national_prefix.empty() ) {
            strOut = "+" + clsPlan.country_code + s;
            return DIAL_PLAN_TRANSLATED;
        }
        if ( _StartsWith( s, clsPlan.national_prefix ) ) {
            strOut = "+" + clsPlan.country_code + s.substr( clsPlan.national_prefix.size() );
            return DIAL_PLAN_TRANSLATED;
        }
        // 접두 없는 숫자열(내선 라벨·단축) — 이 플랜으로는 global number 를 만들 수 없다
        return DIAL_PLAN_INCOMPLETE;
    }

    static std::string _Digits( const std::string &s ) {
        std::string d = _StripVisualSeparators( s );
        if ( !d.empty() && d[0] == '+' ) d.erase( 0, 1 );
        return d;
    }

    bool InNumberRange( const std::string &strNumber, const std::string &strRange ) {
        size_t iSep = strRange.find( '-' );
        if ( iSep == std::string::npos ) iSep = strRange.find( '~' );
        if ( iSep == std::string::npos ) return false;
        std::string lo = _Digits( strRange.substr( 0, iSep ) ), hi = _Digits( strRange.substr( iSep + 1 ) );
        std::string n = _Digits( strNumber );
        if ( lo.empty() || hi.empty() || n.empty() ) return false;
        if ( !_AllDigits( lo ) || !_AllDigits( hi ) || !_AllDigits( n ) ) return false;
        if ( lo.size() != hi.size() || n.size() != lo.size() ) return false;  // 같은 자릿수 안에서만 대역이 성립
        return lo <= n && n <= hi;
    }

    const char *ResultName( EDialPlanResult eResult ) {
        switch ( eResult ) {
            case DIAL_PLAN_UNCHANGED:
                return "unchanged";
            case DIAL_PLAN_TRANSLATED:
                return "translated";
            case DIAL_PLAN_SERVICE_CODE:
                return "service_code";
            case DIAL_PLAN_INCOMPLETE:
                return "incomplete";
            case DIAL_PLAN_DISABLED:
                return "disabled";
        }
        return "?";
    }

}  // namespace CspDialPlan
