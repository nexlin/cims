// psip 단말측 Milenage/AKA 하네스 — TS 35.208 §4 Test Set 1 벡터 + AUTS 왕복 (sip_access_security.md §9).
//   빌드: g++ -std=c++17 -I ext/psip/SipUserAgent -I ext/psip/SipPlatform -I ext/psip/SipParser -I ext/psip/SipStack \
//         tests/psip_aka_test.cpp ext/psip/SipUserAgent/SipAka.cpp ext/psip/SipPlatform/Base64.cpp -lcrypto -o build/psip_aka_test
#include <stdio.h>
#include <string.h>

#include <string>

#include "Base64.h"
#include "SipAka.h"

static std::string Hex( const std::string &b ) {
    std::string o;
    char sz[3];
    for ( size_t i = 0; i < b.size(); ++i ) {
        snprintf( sz, 3, "%02x", (unsigned char)b[i] );
        o += sz;
    }
    return o;
}
static std::string B( const char *h ) {
    std::string o;
    SipAkaHexToBytes( h, o );
    return o;
}
static int g_fail = 0;
#define CHECK( name, got, exp )                                             \
    do {                                                                    \
        if ( ( got ) != ( exp ) ) {                                         \
            printf( "FAIL %s: got=%s exp=%s\n", name, ( got ).c_str(), ( exp ).c_str() ); \
            ++g_fail;                                                       \
        } else                                                              \
            printf( "ok   %s\n", name );                                    \
    } while ( 0 )

int main() {
    const std::string K = B( "465b5ce8b199b49faa5f0a2ee238a6bc" ), OPC = B( "cd63cb71954a9f4e48a5994e37a02baf" ),
                      RAND = B( "23553cbe9637a89d218ae64dae47bf35" ), SQN = B( "ff9bb4d0b607" ), AMF = B( "b9b9" );
    std::string macA, macS, res, ck, ik, ak, akS;
    SipAkaMilenage( K, OPC, RAND, SQN, AMF, macA, macS, res, ck, ik, ak, akS );
    CHECK( "f1  MAC-A", Hex( macA ), std::string( "4a9ffac354dfafb3" ) );
    CHECK( "f1* MAC-S", Hex( macS ), std::string( "01cfaf9ec4e871e9" ) );
    CHECK( "f2  RES", Hex( res ), std::string( "a54211d5e3ba50bf" ) );
    CHECK( "f5  AK", Hex( ak ), std::string( "aa689c648370" ) );
    CHECK( "f3  CK", Hex( ck ), std::string( "b40ba9a3c58b2a05bbf0d987b21bf8cb" ) );
    CHECK( "f4  IK", Hex( ik ), std::string( "f769bcd751044604127672711c6d3441" ) );
    CHECK( "f5* AK*", Hex( akS ), std::string( "451e8beca43b" ) );

    // 서버가 만드는 nonce = base64(RAND ‖ (SQN⊕AK) ‖ AMF ‖ MAC-A) 를 단말이 풀어 답한다
    std::string sqnAk( 6, '\0' );
    for ( int i = 0; i < 6; ++i ) sqnAk[i] = SQN[i] ^ ak[i];
    std::string nonceB64;
    const std::string randAutn = RAND + sqnAk + AMF + macA;
    Base64Encode( randAutn.data(), (int)randAutn.size(), nonceB64 );

    CSipAkaResult r;
    uint64_t sqnMs = 0;
    bool ok = SipAkaCompute( "465b5ce8b199b49faa5f0a2ee238a6bc", "cd63cb71954a9f4e48a5994e37a02baf", nonceB64, sqnMs, r );
    CHECK( "compute ok", std::string( ok ? "1" : "0" ), std::string( "1" ) );
    CHECK( "MAC ok", std::string( r.bMacOk ? "1" : "0" ), std::string( "1" ) );
    CHECK( "SQN fresh", std::string( r.bSqnOk ? "1" : "0" ), std::string( "1" ) );
    CHECK( "RES", Hex( r.strRes ), std::string( "a54211d5e3ba50bf" ) );
    CHECK( "SQN_MS updated", std::to_string( sqnMs ), std::to_string( 0xff9bb4d0b607ULL ) );

    // 같은 챌린지를 다시 받으면 SQN 이 신선하지 않다 → AUTS = (SQN_MS⊕AK*) ‖ MAC-S(AMF*=0000)
    CSipAkaResult r2;
    uint64_t sqnMs2 = 0xff9bb4d0b607ULL;
    SipAkaCompute( "465b5ce8b199b49faa5f0a2ee238a6bc", "cd63cb71954a9f4e48a5994e37a02baf", nonceB64, sqnMs2, r2 );
    CHECK( "resync: SQN stale", std::string( r2.bSqnOk ? "1" : "0" ), std::string( "0" ) );
    std::string auts( 64, '\0' );
    int n = Base64Decode( r2.strAutsB64.c_str(), (int)r2.strAutsB64.size(), &auts[0], 64 );
    auts.resize( n > 0 ? n : 0 );
    CHECK( "AUTS length", std::to_string( auts.size() ), std::string( "14" ) );
    std::string sqnMsBytes( 6, '\0' );
    for ( int i = 0; i < 6; ++i ) sqnMsBytes[i] = auts[i] ^ akS[i];
    CHECK( "AUTS SQN_MS", Hex( sqnMsBytes ), std::string( "ff9bb4d0b607" ) );
    std::string m1, ms0, rr, c2, i2, a2, as2;
    SipAkaMilenage( K, OPC, RAND, SQN, std::string( 2, '\0' ), m1, ms0, rr, c2, i2, a2, as2 );
    CHECK( "AUTS MAC-S(AMF*=0000)", Hex( auts.substr( 6 ) ), Hex( ms0 ) );

    // 변조된 MAC → bMacOk=false
    std::string bad = randAutn;
    bad[31] ^= 1;
    std::string badB64;
    Base64Encode( bad.data(), (int)bad.size(), badB64 );
    CSipAkaResult r3;
    uint64_t s3 = 0;
    SipAkaCompute( "465b5ce8b199b49faa5f0a2ee238a6bc", "cd63cb71954a9f4e48a5994e37a02baf", badB64, s3, r3 );
    CHECK( "tampered MAC rejected", std::string( r3.bMacOk ? "1" : "0" ), std::string( "0" ) );

    printf( "%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail );
    return g_fail ? 1 : 0;
}
