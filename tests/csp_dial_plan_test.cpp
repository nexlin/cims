/*
 * csp_dial_plan_test — CspDialPlan 착신 번호 번역 단위시험
 * (sip_service_model.md §2-10, TS 24.229 §5.4.3.2 · RFC 3966)
 *
 * 빌드 (레포 루트, S1-UNIT-CSP 가 같은 명령을 낸다):
 *   g++ -std=c++17 -Icsp -Iext/psip/SipParser -Iext/psip/SipPlatform
 * tests/csp_dial_plan_test.cpp csp/CspDialPlan.cpp \
 *       build/csp/psip_build/libSipParser.a
 * build/csp/psip_build/libSipPlatform.a -lpthread -o build/csp_dial_plan_test \
 *       && build/csp_dial_plan_test
 */
#include <cstdio>
#include <cstring>
#include <string>

#include "CspDialPlan.h"
#include "SipUri.h"

static int g_iPass = 0;

#define CHECK( cond, name )                                  \
    do {                                                     \
        if ( cond ) {                                        \
            ++g_iPass;                                       \
            printf( "PASS %s\n", name );                     \
        } else {                                             \
            printf( "FAIL %s (line %d)\n", name, __LINE__ ); \
            return 1;                                        \
        }                                                    \
    } while ( 0 )

static DialPlan _kr() {
    DialPlan p;
    p.country_code = "82";
    p.national_prefix = "0";
    p.international_prefix = "00";
    return p;
}

static bool _norm( const char *pszIn, const char *pszCtx, const DialPlan &plan, EDialPlanResult eWant,
                   const char *pszWantOut ) {
    std::string out;
    EDialPlanResult e = CspDialPlan::Normalize( pszIn, pszCtx, plan, out );
    if ( e != eWant || out != pszWantOut ) {
        printf( "   in=%s ctx=%s → %s/%s (want %s/%s)\n", pszIn, pszCtx, CspDialPlan::ResultName( e ), out.c_str(),
                CspDialPlan::ResultName( eWant ), pszWantOut );
        return false;
    }
    return true;
}

int main() {
    const DialPlan kr = _kr();
    DialPlan off;  // country_code 없음 — 번역 비활성

    // ── 국내형 → +E.164 ─────────────────────────────────────────
    CHECK( _norm( "0210001010", "", kr, DIAL_PLAN_TRANSLATED, "+82210001010" ),
           "national 02-1000-1010 → +82210001010" );
    CHECK( _norm( "01012345678", "", kr, DIAL_PLAN_TRANSLATED, "+821012345678" ), "national mobile 010 → +8210…" );
    CHECK( _norm( "02-1000-1010", "", kr, DIAL_PLAN_TRANSLATED, "+82210001010" ),
           "visual separators stripped (RFC 3966)" );
    CHECK( _norm( "(02) 1000.1010", "", kr, DIAL_PLAN_TRANSLATED, "+82210001010" ), "parens/space/dot stripped" );

    // ── 국제 접두 00 → + ────────────────────────────────────────
    CHECK( _norm( "0018005551212", "", kr, DIAL_PLAN_TRANSLATED, "+18005551212" ), "international prefix 00 → +" );

    // ── 이미 국제형 ─────────────────────────────────────────────
    CHECK( _norm( "+82210001010", "", kr, DIAL_PLAN_UNCHANGED, "+82210001010" ), "global number unchanged" );
    CHECK( _norm( "+82-2-1000-1010", "", kr, DIAL_PLAN_TRANSLATED, "+82210001010" ),
           "global with separators → cleaned" );
    CHECK( CspDialPlan::IsGlobalNumber( "+82210001010" ) && !CspDialPlan::IsGlobalNumber( "0210001010" ) &&
               !CspDialPlan::IsGlobalNumber( "+" ),
           "IsGlobalNumber" );

    // ── 번호가 아닌 식별자(그룹 id·adhoc·별칭)는 그대로 ─────────────
    CHECK( _norm( "g001", "", kr, DIAL_PLAN_UNCHANGED, "g001" ), "group id unchanged" );
    CHECK( _norm( "adhoc-1-2", "", kr, DIAL_PLAN_UNCHANGED, "adhoc-1-2" ),
           "adhoc id with dash unchanged (not a number)" );
    CHECK( _norm( "priv-+821-+822", "", kr, DIAL_PLAN_UNCHANGED, "priv-+821-+822" ), "private session id unchanged" );

    // ── 피처코드·긴급번호 ─────────────────────────────────────────
    CHECK( _norm( "**", "", kr, DIAL_PLAN_SERVICE_CODE, "**" ), "feature code ** untouched" );
    CHECK( _norm( "**1003", "", kr, DIAL_PLAN_SERVICE_CODE, "**1003" ), "feature code with extension untouched" );
    CHECK( _norm( "#31#0210001010", "", kr, DIAL_PLAN_SERVICE_CODE, "#31#0210001010" ), "# service code untouched" );
    CHECK( _norm( "112", "", kr, DIAL_PLAN_SERVICE_CODE, "112" ), "emergency 112 untouched (default list)" );
    CHECK( _norm( "119", "", kr, DIAL_PLAN_SERVICE_CODE, "119" ), "emergency 119 untouched (default list)" );
    {
        DialPlan p = kr;
        p.emergency_numbers = { "911" };
        CHECK( _norm( "911", "", p, DIAL_PLAN_SERVICE_CODE, "911" ), "configured emergency list" );
        CHECK( _norm( "112", "", p, DIAL_PLAN_INCOMPLETE, "112" ), "112 not in configured list → incomplete" );
    }

    // ── 번역 불가 → 484 ─────────────────────────────────────────
    CHECK( _norm( "1003", "", kr, DIAL_PLAN_INCOMPLETE, "1003" ), "bare extension → incomplete (484)" );
    CHECK( _norm( "21000101", "", kr, DIAL_PLAN_INCOMPLETE, "21000101" ), "digits without trunk prefix → incomplete" );

    // ── 플랜 비활성(country_code 없음) — 레거시 그대로 ────────────────
    CHECK( _norm( "0210001010", "", off, DIAL_PLAN_DISABLED, "0210001010" ), "plan disabled → unchanged national" );
    CHECK( _norm( "+82210001010", "", off, DIAL_PLAN_UNCHANGED, "+82210001010" ), "plan disabled → global still fine" );
    CHECK( _norm( "1003", "", off, DIAL_PLAN_DISABLED, "1003" ), "plan disabled → no 484" );

    // ── phone-context (RFC 3966 §5.1.5) ───────────────────────────
    CHECK( _norm( "10001010", "+822", kr, DIAL_PLAN_TRANSLATED, "+82210001010" ), "phone-context=+822 global prefix" );
    CHECK( _norm( "10001010", "+82-2", kr, DIAL_PLAN_TRANSLATED, "+82210001010" ), "phone-context with separator" );
    CHECK( _norm( "0210001010", "volte.cims.example.kr", kr, DIAL_PLAN_TRANSLATED, "+82210001010" ),
           "domain phone-context → caller's plan (resolved by dispatcher)" );
    CHECK( _norm( "10001010", "+822", off, DIAL_PLAN_TRANSLATED, "+82210001010" ),
           "global phone-context works even without plan" );

    // ── 접두 없는 국가(national_prefix 빈 값) ─────────────────────────
    {
        DialPlan p;
        p.country_code = "1";
        p.national_prefix = "";
        p.international_prefix = "011";
        CHECK( _norm( "8005551212", "", p, DIAL_PLAN_TRANSLATED, "+18005551212" ),
               "no trunk prefix country → +cc+digits" );
        CHECK( _norm( "01182210001010", "", p, DIAL_PLAN_TRANSLATED, "+82210001010" ), "international prefix 011" );
    }

    // ── URI 추출·재작성 — sip: user / tel: host(psip 파서) ─────────────
    {
        CSipUri uri;
        const char *psz = "sip:0210001010@volte.cims.example.kr;user=phone";
        CHECK( uri.Parse( psz, (int)strlen( psz ) ) > 0, "parse sip uri" );
        std::string num, ctx;
        CHECK( CspDialPlan::ExtractNumber( uri, num, ctx ) && num == "0210001010" && ctx.empty(), "extract sip user" );
        CspDialPlan::RewriteNumber( uri, "+82210001010" );
        char buf[256];
        uri.ToString( buf, sizeof( buf ) );
        CHECK( strncmp( buf, "sip:+82210001010@volte.cims.example.kr", 38 ) == 0, "rewrite sip user" );
    }
    {
        CSipUri uri;
        const char *psz = "tel:10001010;phone-context=+822";
        CHECK( uri.Parse( psz, (int)strlen( psz ) ) > 0, "parse tel uri" );
        std::string num, ctx;
        CHECK( CspDialPlan::ExtractNumber( uri, num, ctx ) && num == "10001010" && ctx == "+822",
               "extract tel host + phone-context" );
        std::string out;
        CHECK( CspDialPlan::Normalize( num, ctx, kr, out ) == DIAL_PLAN_TRANSLATED && out == "+82210001010",
               "tel local number + context → global" );
        CspDialPlan::RewriteNumber( uri, out );
        CHECK( uri.m_strHost == "+82210001010" && uri.m_strUser.empty(), "rewrite tel host" );
    }
    {
        CSipUri uri;
        const char *psz = "tel:+82210001010";
        CHECK( uri.Parse( psz, (int)strlen( psz ) ) > 0, "parse tel global" );
        std::string num, ctx;
        CHECK( CspDialPlan::ExtractNumber( uri, num, ctx ) && num == "+82210001010", "extract tel global" );
    }
    {
        CSipUri uri;
        const char *psz = "sip:volte.cims.example.kr";
        CHECK( uri.Parse( psz, (int)strlen( psz ) ) > 0, "parse domain-only uri" );
        std::string num, ctx;
        CHECK( !CspDialPlan::ExtractNumber( uri, num, ctx ), "domain-only sip uri has no number" );
    }

    // ── 번호 대역(in_range 규칙 연산자) ────────────────────────────
    CHECK( CspDialPlan::InNumberRange( "+82210001012", "+82210001010-+82210001014" ), "in_range inside" );
    CHECK( CspDialPlan::InNumberRange( "+82210001010", "+82210001010-+82210001014" ), "in_range lo inclusive" );
    CHECK( CspDialPlan::InNumberRange( "+82210001014", "+82210001010-+82210001014" ), "in_range hi inclusive" );
    CHECK( !CspDialPlan::InNumberRange( "+82210001015", "+82210001010-+82210001014" ),
           "in_range outside (split block)" );
    CHECK( CspDialPlan::InNumberRange( "82210001012", "+82210001010~+82210001014" ),
           "in_range ~ separator, + ignored" );
    CHECK( CspDialPlan::InNumberRange( "0212345050", "0212345000-0212345099" ), "in_range national DID range" );
    CHECK( !CspDialPlan::InNumberRange( "+82210001012", "0210001010-0210001019" ),
           "in_range different digit length → false" );
    CHECK( !CspDialPlan::InNumberRange( "g001", "+82210001010-+82210001019" ), "in_range non-number → false" );
    CHECK( !CspDialPlan::InNumberRange( "+82210001012", "+82210001010" ), "in_range malformed value → false" );

    printf( "csp_dial_plan_test: %d checks passed\n", g_iPass );
    return 0;
}
