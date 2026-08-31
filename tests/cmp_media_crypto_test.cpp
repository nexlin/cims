// cmp_media_crypto_test.cpp — 미디어 SRTP leg 컨텍스트(PMediaCrypto.cpp, libsrtp2) 단위 검증.
//
// 빌드: g++ -std=c++17 -Icmp -Ipkg/libsrtp/include tests/cmp_media_crypto_test.cpp \
//         cmp/PMediaCrypto.cpp pkg/libsrtp/lib/libsrtp2.a -lcrypto -o /tmp/mediacrypto
// (레포 루트에서. ext/libsrtp 는 빌드 1회 필요 — make libsrtp)
//
// 검증 항목 (media_security.md §9 S1):
//   roundTrip          — UE→CMP protect → unprotect 가 원본 RTP 복원 (양방향)
//   rtcpRoundTrip      — SRTCP protect/unprotect 왕복 (relay 경로)
//   seqWrapRoc         — seq 65534→2 랩어라운드 관통 (ROC 증가를 libsrtp 가 추적)
//   forgeryRejected    — 태그/본문 1비트 변조 거부
//   replayRejected     — 동일 패킷 재수신 거부, 이후 정상 패킷 통과
//   multiSsrc          — 하향 슬롯 SSRC 다중화 (any_outbound 템플릿 자동 스트림)
//   rekeyRecreate      — 키 변경 재협상 = 세션 재생성 (구 키 암호문 거부)
//   sameKeyKeep        — 동일 구성 재선언 = 세션 유지 (ROC/seq 연속성 보존)
//   badParams          — 키/salt 길이·alg 검증 (fail-fast)

#include "PMediaCrypto.h"
#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

// 임의 RTP 패킷 생성 (12B 헤더 + 페이로드)
static int makeRtp(char* buf, unsigned int ssrc, unsigned short seq, unsigned int ts,
                   const char* body, int bodyLen) {
    buf[0] = (char)0x80;               // V=2
    buf[1] = (char)96;                 // PT=96
    buf[2] = (char)(seq >> 8); buf[3] = (char)(seq & 0xFF);
    buf[4] = (char)(ts >> 24); buf[5] = (char)(ts >> 16); buf[6] = (char)(ts >> 8); buf[7] = (char)ts;
    buf[8] = (char)(ssrc >> 24); buf[9] = (char)(ssrc >> 16); buf[10] = (char)(ssrc >> 8); buf[11] = (char)ssrc;
    memcpy(buf + 12, body, bodyLen);
    return 12 + bodyLen;
}

// 임의 RTCP 패킷 (8B 헤더 이상)
static int makeRtcp(char* buf, unsigned int ssrc, const char* body, int bodyLen) {
    int total = 8 + bodyLen;
    buf[0] = (char)0x80;
    buf[1] = (char)200;  // SR
    unsigned short words = (unsigned short)(total / 4 - 1);
    buf[2] = (char)(words >> 8); buf[3] = (char)(words & 0xFF);
    buf[4] = (char)(ssrc >> 24); buf[5] = (char)(ssrc >> 16); buf[6] = (char)(ssrc >> 8); buf[7] = (char)ssrc;
    memcpy(buf + 8, body, bodyLen);
    return total;
}

static const std::string K1(16, 'A'), S1v(14, 'B');   // UE→CMP 상향 키
static const std::string K2(16, 'C'), S2v(14, 'D');   // CMP→UE 하향 키
static const std::string K3(16, 'E'), S3v(14, 'F');   // 재키잉용

// UE 측 / CMP 측 컨텍스트 쌍 — UE tx == CMP rx, UE rx == CMP tx
static bool makePair(PMediaCrypto& ue, PMediaCrypto& cmp, std::string& err) {
    // UE: 자기 송신(tx)=K1, 수신(rx)=K2
    if (!ue.init("AES_CM_128_HMAC_SHA1_80", K2, S2v, K1, S1v, err)) return false;
    // CMP: 상향 수신(rx)=K1, 하향 송신(tx)=K2
    return cmp.init("AES_CM_128_HMAC_SHA1_80", K1, S1v, K2, S2v, err);
}

static void roundTrip() {
    printf("roundTrip\n");
    PMediaCrypto ue, cmp;
    std::string err;
    CHECK(makePair(ue, cmp, err), "pair init");
    char pkt[512];
    const char* body = "hello-srtp-payload-0123456789";
    int len = makeRtp(pkt, 0x1234, 100, 16000, body, (int)strlen(body));
    const int orig = len;
    char plain[512];
    memcpy(plain, pkt, len);

    CHECK(ue.protectRtp(pkt, len, sizeof(pkt)), "ue protect");
    CHECK(len == orig + 10, "auth tag 10B appended");
    CHECK(memcmp(pkt + 12, plain + 12, orig - 12) != 0, "payload encrypted");
    CHECK(memcmp(pkt, plain, 12) == 0, "rtp header in clear");
    CHECK(cmp.unprotectRtp(pkt, len), "cmp unprotect");
    CHECK(len == orig && memcmp(pkt, plain, len) == 0, "plaintext restored");

    // 하향 (CMP tx → UE rx)
    len = makeRtp(pkt, 0x10001234, 55, 3200, body, (int)strlen(body));
    int dOrig = len;
    memcpy(plain, pkt, len);
    CHECK(cmp.protectRtp(pkt, len, sizeof(pkt)), "cmp protect (down)");
    CHECK(ue.unprotectRtp(pkt, len), "ue unprotect (down)");
    CHECK(len == dOrig && memcmp(pkt, plain, len) == 0, "downlink plaintext restored");
}

static void rtcpRoundTrip() {
    printf("rtcpRoundTrip\n");
    PMediaCrypto ue, cmp;
    std::string err;
    CHECK(makePair(ue, cmp, err), "pair init");
    char pkt[512];
    const char* body = "rtcp-sr-body-123";  // 16B (4B 정렬)
    int len = makeRtcp(pkt, 0x1234, body, 16);
    int orig = len;
    char plain[512];
    memcpy(plain, pkt, len);
    CHECK(ue.protectRtcp(pkt, len, sizeof(pkt)), "ue protect rtcp");
    CHECK(len > orig, "srtcp trailer appended");
    CHECK(cmp.unprotectRtcp(pkt, len), "cmp unprotect rtcp");
    CHECK(len == orig && memcmp(pkt, plain, len) == 0, "rtcp plaintext restored");
}

static void seqWrapRoc() {
    printf("seqWrapRoc\n");
    PMediaCrypto ue, cmp;
    std::string err;
    CHECK(makePair(ue, cmp, err), "pair init");
    char pkt[512];
    const char* body = "wrap";
    bool ok = true;
    // seq 65530 → 랩 → 5 : ROC 증가를 양측 libsrtp 가 추적해야 관통한다
    unsigned int ts = 0;
    for (int i = 0; i < 12; ++i) {
        unsigned short seq = (unsigned short)(65530u + i);   // 자연 랩
        int len = makeRtp(pkt, 0x7777, seq, ts += 160, body, 4);
        if (!ue.protectRtp(pkt, len, sizeof(pkt))) { ok = false; break; }
        if (!cmp.unprotectRtp(pkt, len)) { ok = false; break; }
    }
    CHECK(ok, "seq wraparound (ROC) survived");
}

static void forgeryRejected() {
    printf("forgeryRejected\n");
    PMediaCrypto ue, cmp;
    std::string err;
    CHECK(makePair(ue, cmp, err), "pair init");
    char pkt[512];
    int len = makeRtp(pkt, 0x1234, 7, 1120, "abcd", 4);
    CHECK(ue.protectRtp(pkt, len, sizeof(pkt)), "protect");
    pkt[len - 1] ^= 0x01;  // 태그 변조
    int l2 = len;
    CHECK(!cmp.unprotectRtp(pkt, l2), "tampered tag rejected");
    pkt[len - 1] ^= 0x01;
    pkt[13] ^= 0x01;       // 본문 변조
    l2 = len;
    CHECK(!cmp.unprotectRtp(pkt, l2), "tampered body rejected");
}

static void replayRejected() {
    printf("replayRejected\n");
    PMediaCrypto ue, cmp;
    std::string err;
    CHECK(makePair(ue, cmp, err), "pair init");
    char pkt[512], dup[512];
    int len = makeRtp(pkt, 0x1234, 42, 6720, "abcd", 4);
    CHECK(ue.protectRtp(pkt, len, sizeof(pkt)), "protect");
    memcpy(dup, pkt, len);
    int dupLen = len;
    CHECK(cmp.unprotectRtp(pkt, len), "first copy accepted");
    CHECK(!cmp.unprotectRtp(dup, dupLen), "replay rejected");
    // 이후 정상 패킷은 통과
    len = makeRtp(pkt, 0x1234, 43, 6880, "efgh", 4);
    CHECK(ue.protectRtp(pkt, len, sizeof(pkt)) && cmp.unprotectRtp(pkt, len), "next packet ok");
}

static void multiSsrc() {
    printf("multiSsrc\n");
    PMediaCrypto ue, cmp;
    std::string err;
    CHECK(makePair(ue, cmp, err), "pair init");
    char pkt[512];
    bool ok = true;
    // 하향 슬롯 SSRC 다중화 (0x10000000+/0x40000000+…) — any_outbound 템플릿이 SSRC 별
    // 스트림을 자동 생성, 수신(any_inbound)도 동일
    const unsigned int ssrcs[] = { 0x10000001, 0x40000001, 0x41000001, 0x10000002 };
    for (unsigned int ssrc : ssrcs) {
        for (unsigned short seq = 1; seq <= 3; ++seq) {
            int len = makeRtp(pkt, ssrc, seq, seq * 160u, "slot", 4);
            if (!cmp.protectRtp(pkt, len, sizeof(pkt))) { ok = false; break; }
            if (!ue.unprotectRtp(pkt, len)) { ok = false; break; }
        }
    }
    CHECK(ok, "multiple downlink slot SSRCs");
}

static void rekeyRecreate() {
    printf("rekeyRecreate\n");
    PMediaCrypto ue, cmp;
    std::string err;
    CHECK(makePair(ue, cmp, err), "pair init");
    char pkt[512];
    int len = makeRtp(pkt, 0x1234, 9, 1440, "abcd", 4);
    CHECK(ue.protectRtp(pkt, len, sizeof(pkt)), "protect with old key");
    // CMP 재키잉 (rx 키 교체 = 재협상) — 구 키 암호문은 인증 실패해야 한다
    CHECK(cmp.init("AES_CM_128_HMAC_SHA1_80", K3, S3v, K2, S2v, err), "rekey");
    int l2 = len;
    CHECK(!cmp.unprotectRtp(pkt, l2), "old-key ciphertext rejected after rekey");
    // 새 키 쌍으로 재정렬한 UE 는 통과
    PMediaCrypto ue2;
    CHECK(ue2.init("AES_CM_128_HMAC_SHA1_80", K2, S2v, K3, S3v, err), "ue rekey");
    len = makeRtp(pkt, 0x1234, 1, 160, "abcd", 4);
    CHECK(ue2.protectRtp(pkt, len, sizeof(pkt)) && cmp.unprotectRtp(pkt, len), "new-key roundtrip");
}

static void sameKeyKeep() {
    printf("sameKeyKeep\n");
    PMediaCrypto ue, cmp;
    std::string err;
    CHECK(makePair(ue, cmp, err), "pair init");
    char pkt[512];
    // 스트림 진행 (seq 10..12)
    for (unsigned short seq = 10; seq <= 12; ++seq) {
        int len = makeRtp(pkt, 0x1234, seq, seq * 160u, "abcd", 4);
        CHECK(ue.protectRtp(pkt, len, sizeof(pkt)) && cmp.unprotectRtp(pkt, len), "stream progress");
    }
    // 동일 구성 재선언(refresh) — 세션 유지: 재전송 창이 보존되어 직전 seq 재사용이 거부된다
    CHECK(cmp.init("AES_CM_128_HMAC_SHA1_80", K1, S1v, K2, S2v, err), "same-config re-init");
    int len = makeRtp(pkt, 0x1234, 13, 13 * 160u, "abcd", 4);
    CHECK(ue.protectRtp(pkt, len, sizeof(pkt)) && cmp.unprotectRtp(pkt, len), "continuity after refresh");
    // 세션이 재생성됐다면 replay 창이 리셋되어 seq 11 재수신이 통과해 버린다 — 유지 검증
    PMediaCrypto ue3;
    std::string e3;
    CHECK(ue3.init("AES_CM_128_HMAC_SHA1_80", K2, S2v, K1, S1v, e3), "fresh ue for stale seq");
    int l3 = makeRtp(pkt, 0x1234, 11, 11 * 160u, "abcd", 4);
    CHECK(ue3.protectRtp(pkt, l3, sizeof(pkt)), "protect stale seq");
    CHECK(!cmp.unprotectRtp(pkt, l3), "stale seq still in replay window (session kept)");
}

static void badParams() {
    printf("badParams\n");
    PMediaCrypto c;
    std::string err;
    CHECK(!c.init("AES_CM_128_HMAC_SHA1_80", std::string(15, 'A'), S1v, K2, S2v, err), "short rx key rejected");
    CHECK(!c.init("AES_CM_128_HMAC_SHA1_80", K1, std::string(13, 'B'), K2, S2v, err), "short rx salt rejected");
    CHECK(!c.init("AES_CM_256_HMAC_SHA1_80", K1, S1v, K2, S2v, err), "unsupported alg rejected");
    CHECK(!c.enabled(), "failed init leaves context disabled");
    CHECK(c.init("", K1, S1v, K2, S2v, err), "empty alg = default suite");
    CHECK(c.alg() == "AES_CM_128_HMAC_SHA1_80", "default suite name");
    // _32 suite — RTP 태그 4B
    PMediaCrypto c32, p32;
    std::string e32;
    CHECK(c32.init("AES_CM_128_HMAC_SHA1_32", K1, S1v, K2, S2v, e32), "_32 init");
    CHECK(p32.init("AES_CM_128_HMAC_SHA1_32", K2, S2v, K1, S1v, e32), "_32 peer init");
    char pkt[512];
    int len = makeRtp(pkt, 0x9, 1, 160, "abcd", 4);
    int orig = len;
    CHECK(p32.protectRtp(pkt, len, sizeof(pkt)), "_32 protect");
    CHECK(len == orig + 4, "_32 tag is 4B");
    CHECK(c32.unprotectRtp(pkt, len), "_32 unprotect");
}

int main() {
    roundTrip();
    rtcpRoundTrip();
    seqWrapRoc();
    forgeryRejected();
    replayRejected();
    multiSsrc();
    rekeyRecreate();
    sameKeyKeep();
    badParams();
    printf("\n%s — pass=%d fail=%d\n", g_fail == 0 ? "OK" : "FAILED", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
