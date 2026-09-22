/** S1-UNIT-CSP — 착신전환 History-Info(RFC 7044)·cause(RFC 4458) 조립·판정 (csp/CspDiversion.cpp, TS 24.604 §4.5.2.6).
 *  빌드: g++ -std=c++17 -I csp tests/csp_diversion_test.cpp csp/CspDiversion.cpp */
#include <cstdio>
#include <string>

#include "CspDiversion.h"

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

using namespace CspDiversion;

int main() {
    // 첫 전환 — served 항목 index=1, 대상 index=1.1 mp=1 cause=302
    {
        std::vector<Hop> hops = { { "+821011112222", CAUSE_UNCONDITIONAL } };
        std::string hi = BuildHistoryInfo( "", "cims.example.kr", "+821000000001", hops );
        CHECK( hi ==
                   "<sip:+821000000001@cims.example.kr>;index=1, "
                   "<sip:+821011112222@cims.example.kr;cause=302>;index=1.1;mp=1",
               "first hop history-info" );
        CHECK( CountDiversions( hi ) == 1, "count after first hop" );
        CHECK( LastIndex( hi ) == "1.1", "last index 1.1" );
    }
    // 연쇄 전환 B→C→D — index 1.1.1, mp=1.1
    {
        std::vector<Hop> hops = { { "C", 302 }, { "D", 302 } };
        std::string hi = BuildHistoryInfo( "", "d", "B", hops );
        CHECK( hi == "<sip:B@d>;index=1, <sip:C@d;cause=302>;index=1.1;mp=1, <sip:D@d;cause=302>;index=1.1.1;mp=1.1",
               "chained hops" );
        CHECK( CountDiversions( hi ) == 2, "count chained" );
        CHECK( ChainLabel( "B", hops ) == "B → C → D", "chain label" );
    }
    // 수신 INVITE 가 이미 History-Info 를 실었다(피어가 한 번 전환) — 항목 보존 + 이어 붙이기, 전환 수 누적
    {
        std::string existing = "<sip:+8210A@peer.example>;index=1,<sip:+8210B@peer.example;cause=486>;index=1.1;mp=1";
        std::vector<Hop> hops = { { "+8210C", CAUSE_UNCONDITIONAL } };
        std::string hi = BuildHistoryInfo( existing, "cims.example.kr", "+8210B", hops );
        CHECK( hi ==
                   "<sip:+8210A@peer.example>;index=1, <sip:+8210B@peer.example;cause=486>;index=1.1;mp=1, "
                   "<sip:+8210C@cims.example.kr;cause=302>;index=1.1.1;mp=1.1",
               "append to existing" );
        CHECK( CountDiversions( existing ) == 1, "existing count" );
        CHECK( CountDiversions( hi ) == 2, "count after append" );
    }
    // 항목 분리 — URI 안의 쉼표·공백은 나누지 않는다, cause 없는 항목은 전환으로 세지 않는다
    {
        std::string s = "<sip:a@d?Reason=SIP%3Bcause%3D302,x>;index=1 , <sip:b@d>;index=1.1";
        std::vector<std::string> v = SplitEntries( s );
        CHECK( v.size() == 2 && v[1] == "<sip:b@d>;index=1.1", "split entries" );
        CHECK( CountDiversions( s ) == 0, "no cause = no diversion" );
        CHECK( LastIndex( s ) == "1.1", "last index of split" );
        CHECK( CountDiversions( "" ) == 0 && LastIndex( "" ).empty(), "empty header" );
    }
    // MakeUri — 이미 URI 면 그대로, tel 도 그대로
    CHECK( MakeUri( "tel:+82212345678", "d" ) == "tel:+82212345678", "tel uri kept" );
    CHECK( MakeUri( "sip:x@y", "d" ) == "sip:x@y", "sip uri kept" );
    CHECK( MakeUri( "1001", "pbx.local" ) == "sip:1001@pbx.local", "user → sip uri" );

    printf( "csp_diversion_test: %d checks passed\n", g_iPass );
    return 0;
}
