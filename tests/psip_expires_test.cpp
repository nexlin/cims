// psip Expires(delta-seconds) 파싱 하네스 — RFC 3261 §20.19(32bit 무부호)·§10.2.1.1(REGISTER 는 Contact ;expires
//   가 Expires 헤더보다 우선)·§10.2.4(둘 다 없으면 등록자 기본값) 을 파서가 그대로 보존하는지 검증한다.
//   4294967295 가 int 로 넘쳐 -1 → "미지정" 표지와 겹쳐 0(해지) 으로 접히던 결함(외부 MCX SDK 실측, csp 0.2.116
//   이전) 의 회귀 시험. 값의 해석(0=해지, 없음=기본값, 상한, 무효=400)은 핸들러 몫이라 여기서는 다루지 않는다.
//   빌드(csp 빌드 뒤 — psip 정적 라이브러리 사용):
//     g++ -std=c++17 -I ext/psip/SipParser -I ext/psip/SipPlatform tests/psip_expires_test.cpp \
//         build/csp/psip_build/libSipParser.a build/csp/psip_build/libSipPlatform.a -lpthread -o build/psip_expires_test
//   실행: build/psip_expires_test
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "SipMessage.h"

static int g_fail = 0;
#define CHECK( name, cond )                                          \
    do {                                                             \
        if ( !( cond ) ) {                                           \
            printf( "FAIL %s (%s:%d)\n", name, __FILE__, __LINE__ ); \
            ++g_fail;                                                \
        } else {                                                     \
            printf( "ok   %s\n", name );                             \
        }                                                            \
    } while ( 0 )

// REGISTER 텍스트 조립 — pszExpiresHdr: Expires 헤더 값(NULL=헤더 없음), pszContactParam: Contact 뒤에 붙일 ";expires=…"(NULL=없음)
static std::string BuildRegister( const char *pszExpiresHdr, const char *pszContactParam ) {
    std::string s =
        "REGISTER sip:ptt.cims.example.kr SIP/2.0\r\n"
        "Via: SIP/2.0/UDP 10.0.0.2:5060;branch=z9hG4bK1\r\n"
        "From: <sip:+82500000001@ptt.cims.example.kr>;tag=a\r\n"
        "To: <sip:+82500000001@ptt.cims.example.kr>\r\n"
        "Call-ID: c1@10.0.0.2\r\n"
        "CSeq: 1 REGISTER\r\n"
        "Max-Forwards: 70\r\n";
    s += std::string( "Contact: <sip:+82500000001@10.0.0.2:5060>" ) + ( pszContactParam ? pszContactParam : "" ) + "\r\n";
    if ( pszExpiresHdr ) s += std::string( "Expires: " ) + pszExpiresHdr + "\r\n";
    s += "Content-Length: 0\r\n\r\n";
    return s;
}

static bool ParseInto( CSipMessage &m, const std::string &s ) {
    m.Clear();
    return m.Parse( s.c_str(), (int)s.size() ) != -1;
}

// 한 케이스: 파싱 성공 + GetExpires/GetRegisterExpires 결과·값 대조
static void Case( const char *pszName, const char *pszHdr, const char *pszContact, ESipExpiresResult eHdr, uint32_t uiHdr,
                  ESipExpiresResult eReg, uint32_t uiReg ) {
    CSipMessage m;
    std::string strName( pszName );
    CHECK( ( strName + " parse" ).c_str(), ParseInto( m, BuildRegister( pszHdr, pszContact ) ) );
    uint32_t ui = 0xDEADBEEFu;
    ESipExpiresResult e = m.GetExpires( ui );
    CHECK( ( strName + " GetExpires result" ).c_str(), e == eHdr );
    if ( eHdr == E_SIP_EXPIRES_VALID ) CHECK( ( strName + " GetExpires value" ).c_str(), ui == uiHdr );
    ui = 0xDEADBEEFu;
    e = m.GetRegisterExpires( ui );
    CHECK( ( strName + " GetRegisterExpires result" ).c_str(), e == eReg );
    if ( eReg == E_SIP_EXPIRES_VALID ) CHECK( ( strName + " GetRegisterExpires value" ).c_str(), ui == uiReg );
}

int main() {
    const ESipExpiresResult A = E_SIP_EXPIRES_ABSENT, V = E_SIP_EXPIRES_VALID, I = E_SIP_EXPIRES_INVALID;

    // 정상값 — 헤더만
    Case( "hdr 3600", "3600", NULL, V, 3600, V, 3600 );
    Case( "hdr 0 (해지 의미는 호출부)", "0", NULL, V, 0, V, 0 );
    // 회귀: 32bit 무부호 최대값이 그대로 보존돼야 한다 (종전 -1 → 0 오판)
    Case( "hdr 4294967295 (2^32-1)", "4294967295", NULL, V, 4294967295u, V, 4294967295u );
    Case( "hdr 2147483648 (INT_MAX+1)", "2147483648", NULL, V, 2147483648u, V, 2147483648u );
    // 형식 오류 — 2^32 초과·비숫자·음수 → INVALID (파싱은 성공, 판정은 호출부가 400)
    Case( "hdr 4294967296 (2^32)", "4294967296", NULL, I, 0, I, 0 );
    Case( "hdr abc", "abc", NULL, I, 0, I, 0 );
    Case( "hdr -1", "-1", NULL, I, 0, I, 0 );
    Case( "hdr 12ab", "12ab", NULL, I, 0, I, 0 );
    Case( "hdr ' 600 ' (공백 허용)", " 600 ", NULL, V, 600, V, 600 );
    // REGISTER 우선순위 (§10.2.1.1): Contact ;expires > Expires 헤더. GetExpires 는 헤더만 본다.
    Case( "contact only 600", NULL, ";expires=600", A, 0, V, 600 );
    Case( "hdr 3600 + contact 600 (Contact 우선)", "3600", ";expires=600", V, 3600, V, 600 );
    Case( "contact 4294967295", NULL, ";expires=4294967295", A, 0, V, 4294967295u );
    Case( "contact abc → INVALID", NULL, ";expires=abc", A, 0, I, 0 );
    // 둘 다 없음 → ABSENT (등록자 기본값 §10.2.4)
    Case( "neither", NULL, NULL, A, 0, A, 0 );

    // 송신: SetExpires 가 32bit 무부호 그대로 직렬화되는지
    {
        CSipMessage m;
        CHECK( "set parse base", ParseInto( m, BuildRegister( NULL, NULL ) ) );
        m.SetExpires( 4294967295u );
        char szBuf[4096];
        int iLen = m.ToString( szBuf, sizeof( szBuf ) );
        CHECK( "SetExpires(2^32-1) serialize", iLen > 0 && strstr( szBuf, "Expires: 4294967295\r\n" ) != NULL );
        m.SetExpires( 0 );
        iLen = m.ToString( szBuf, sizeof( szBuf ) );
        CHECK( "SetExpires(0) serialize", iLen > 0 && strstr( szBuf, "Expires: 0\r\n" ) != NULL );
        uint32_t ui = 1;
        CHECK( "SetExpires(0) readback", m.GetExpires( ui ) == V && ui == 0 );
        // Clear 후 ABSENT, 그리고 Expires 없는 메시지를 다시 파싱·직렬화하면 Expires 줄이 남지 않아야 한다
        //   (빈 메시지 자체는 시작줄이 없어 직렬화 대상이 아니므로 재파싱 뒤에 본다)
        m.Clear();
        CHECK( "Clear → ABSENT", m.GetExpires( ui ) == A );
        CHECK( "reparse w/o Expires", ParseInto( m, BuildRegister( NULL, NULL ) ) );
        memset( szBuf, 0, sizeof( szBuf ) );
        iLen = m.ToString( szBuf, sizeof( szBuf ) );
        CHECK( "reparse → no Expires line", iLen > 0 && strstr( szBuf, "Expires:" ) == NULL );
    }

    printf( "%s (%d fail)\n", g_fail ? "FAIL" : "PASS", g_fail );
    return g_fail ? 1 : 0;
}
