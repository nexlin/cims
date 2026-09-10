// json_escape_test.cpp — SimpleJson::JsonNode::Escape 단위시험
//
// 이 이스케이퍼는 서비스 로그(JSONL) 를 쓰는 모든 경로의 공용 정본이다
// (CCallDir::Esc · CSipMessageLogger::JsonEsc 가 위임한다). 두 가지가 깨지면
// 파일이 JSON/UTF-8 이 아니게 되고, **그 파일을 읽는 통계 집계·이력이 통째로 멈춘다**
// — 2026-09-10 에 SIP 포트로 들어온 DTLS 탐침의 0xfe 한 바이트로 전 서비스 통계가
// 6시간 정지한 적이 있다. 그래서 정상 UTF-8 통과와 임의 바이트 차단을 함께 고정한다.
//
// 빌드·실행:
//   g++ -std=c++17 -Iinclude tests/json_escape_test.cpp -o /tmp/jsonesc && /tmp/jsonesc

#include "SimpleJson.h"

#include <cstdio>
#include <string>

static int g_nFail = 0;

static void Check( const char *pszName, const std::string &strGot, const std::string &strWant ) {
    bool bOk = ( strGot == strWant );
    if ( !bOk ) g_nFail++;
    printf( "  %s %s\n", bOk ? "OK  " : "FAIL", pszName );
    if ( !bOk ) printf( "       got =%s\n       want=%s\n", strGot.c_str(), strWant.c_str() );
}

int main() {
    using SimpleJson::JsonNode;

    // ── 정상 경로는 손대지 않는다 ────────────────────────────────
    Check( "ASCII 그대로", JsonNode::Escape( "INVITE sip:a@b SIP/2.0" ), "INVITE sip:a@b SIP/2.0" );
    Check( "한글(3바이트) 그대로", JsonNode::Escape( "홍길동" ), "홍길동" );
    Check( "이모지(4바이트) 그대로", JsonNode::Escape( "\xF0\x9F\x93\x9E" ), "\xF0\x9F\x93\x9E" );

    // ── JSON 필수 이스케이프 ─────────────────────────────────────
    Check( "따옴표·역슬래시", JsonNode::Escape( "a\"b\\c" ), "a\\\"b\\\\c" );
    Check( "CRLF·탭", JsonNode::Escape( "a\r\nb\tc" ), "a\\r\\nb\\tc" );
    Check( "제어문자 → \\uXXXX", JsonNode::Escape( std::string( "a\x01" "b" ) ), "a\\u0001b" );

    // ── 비-UTF-8 바이트는 통과시키지 않는다 ──────────────────────
    // 실제로 통계를 멈춘 입력: DTLS 1.2 ClientHello 머리 (0x16 handshake, 0xfe 0xfd 버전)
    std::string strDtls;
    strDtls += (char)0x16;
    strDtls += (char)0xfe;
    strDtls += (char)0xfd;
    strDtls += ']';
    Check( "DTLS 탐침 바이트", JsonNode::Escape( strDtls ), "\\u0016\\ufffd\\ufffd]" );

    Check( "잘린 다바이트 시퀀스", JsonNode::Escape( "\xED\x99" ), "\\ufffd\\ufffd" );
    Check( "고아 후속바이트", JsonNode::Escape( "\x80" ), "\\ufffd" );
    Check( "overlong 선두(0xC0)", JsonNode::Escape( "\xC0\x80" ), "\\ufffd\\ufffd" );
    Check( "범위 밖 선두(0xF5)", JsonNode::Escape( "\xF5\x80\x80\x80" ),
           "\\ufffd\\ufffd\\ufffd\\ufffd" );

    // ── 실제 SIP 원문 한 줄 (한글 표시이름 포함) ─────────────────
    Check( "SIP 원문 + 한글 표시이름",
           JsonNode::Escape( "INVITE sip:8050001000004@ptt.cims.example.kr SIP/2.0\r\n"
                             "From: \"관제\" <sip:a@b>\r\n" ),
           "INVITE sip:8050001000004@ptt.cims.example.kr SIP/2.0\\r\\n"
           "From: \\\"관제\\\" <sip:a@b>\\r\\n" );

    printf( "\n실패 %d 건\n", g_nFail );
    return g_nFail ? 1 : 0;
}
