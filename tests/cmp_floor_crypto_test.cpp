// cmp_floor_crypto_test.cpp — floor RTCP SRTCP 보호(PFloorCrypto.cpp) 단위 검증.
//
// 빌드: g++ -std=c++17 -I../cmp tests/cmp_floor_crypto_test.cpp cmp/PFloorCrypto.cpp -lcrypto -o /tmp/floorcrypto
// (PFloorCrypto.cpp 의 외부 의존은 OpenSSL 뿐이라 단독 링크 가능)
//
// 검증 항목:
//   kdfRfc3711Vectors  — RFC 3711 §B.3 키 파생 시험벡터 (세션 키/salt/인증키)
//   roundTrip          — protect → unprotect 가 원본 RTCP 를 복원
//   headerInClear      — RTCP 헤더 8B 는 평문, 본문은 변조(암호화)됨
//   forgeryRejected    — 태그/본문 1비트 변조 시 거부
//   replayRejected     — 동일 패킷 재수신 거부, 이후 정상 패킷은 통과
//   mkiMismatch        — MKI 불일치 거부
//   badParams          — 키/salt 길이·alg 검증

#include "PFloorCrypto.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static std::string hex2bin(const std::string& h) {
    std::string out;
    PFloorCrypto::DecodeHex(h, out);
    return out;
}
static std::string bin2hex(const void* p, int n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    const unsigned char* b = (const unsigned char*)p;
    for (int i = 0; i < n; ++i) { s.push_back(d[b[i] >> 4]); s.push_back(d[b[i] & 0xF]); }
    return s;
}
static std::string b64(const std::string& bin) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < bin.size(); i += 3) {
        unsigned v = (unsigned char)bin[i] << 16;
        if (i + 1 < bin.size()) v |= (unsigned char)bin[i + 1] << 8;
        if (i + 2 < bin.size()) v |= (unsigned char)bin[i + 2];
        o.push_back(T[(v >> 18) & 63]);
        o.push_back(T[(v >> 12) & 63]);
        o.push_back(i + 1 < bin.size() ? T[(v >> 6) & 63] : '=');
        o.push_back(i + 2 < bin.size() ? T[v & 63] : '=');
    }
    return o;
}

// 12B RTCP APP 헤더 + 임의 본문 (floor 메시지 형태)
static int makeFloorPkt(char* buf, unsigned int ssrc, const char* body, int bodyLen) {
    buf[0] = (char)0x80;
    buf[1] = (char)204;
    int total = 12 + bodyLen;
    int words = total / 4 - 1;
    buf[2] = (char)((words >> 8) & 0xFF);
    buf[3] = (char)(words & 0xFF);
    buf[4] = (char)((ssrc >> 24) & 0xFF);
    buf[5] = (char)((ssrc >> 16) & 0xFF);
    buf[6] = (char)((ssrc >> 8) & 0xFF);
    buf[7] = (char)(ssrc & 0xFF);
    memcpy(buf + 8, "MCPT", 4);
    memcpy(buf + 12, body, bodyLen);
    return total;
}

// RFC 3711 §B.3 시험벡터로 KDF 검증 — PFloorCrypto 는 SRTCP 라벨(3/4/5)을 쓰므로
// 여기서는 동일 KDF 를 SRTP 라벨(0/1/2)로 재현해 대조한다(파생 함수 자체의 정합).
static void _kdfIvLocal(const unsigned char* masterSalt, unsigned char label, unsigned char iv[16]) {
    memset(iv, 0, 16);
    memcpy(iv, masterSalt, 14);
    iv[7] ^= label;
}
#include <openssl/evp.h>
static bool _ctr(const unsigned char* key, const unsigned char iv[16], unsigned char* out, int len) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    std::vector<unsigned char> zero(len, 0);
    int ol = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key, iv) == 1 &&
              EVP_EncryptUpdate(ctx, out, &ol, zero.data(), len) == 1 && ol == len;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

int main() {
    // 1) RFC 3711 §B.3 — master key/salt 에서 파생된 세션 키/salt/인증키
    {
        std::string mk = hex2bin("E1F97A0D3E018BE0D64FA32C06DE4139");
        std::string ms = hex2bin("0EC675AD498AFEEBB6960B3AABE6");
        CHECK(mk.size() == 16 && ms.size() == 14, "rfc3711 vector decodes");

        unsigned char iv[16], out[20];
        _kdfIvLocal((const unsigned char*)ms.data(), 0x00, iv);
        _ctr((const unsigned char*)mk.data(), iv, out, 16);
        CHECK(bin2hex(out, 16) == "c61e7a93744f39ee10734afe3ff7a087", "KDF label0 cipher key");

        _kdfIvLocal((const unsigned char*)ms.data(), 0x02, iv);
        _ctr((const unsigned char*)mk.data(), iv, out, 14);
        CHECK(bin2hex(out, 14) == "30cbbc08863d8c85d49db34a9ae1", "KDF label2 session salt");

        _kdfIvLocal((const unsigned char*)ms.data(), 0x01, iv);
        _ctr((const unsigned char*)mk.data(), iv, out, 20);
        CHECK(bin2hex(out, 20) == "cebe321f6ff7716b6fd4ab49af256a156d38baa4", "KDF label1 auth key(20B)");
    }

    // 공통 키 재료 (제어평면 인코딩: key/salt=base64, mki=hex)
    std::string key = hex2bin("E1F97A0D3E018BE0D64FA32C06DE4139");
    std::string salt = hex2bin("0EC675AD498AFEEBB6960B3AABE6");
    std::string err;

    // 2) 왕복 + 헤더 평문 + 길이 증가분
    char plain[256], sec[512], back[512];
    int plainLen = makeFloorPkt(plain, 0x0A0B0C0Du, "\x06\x10tel:+82571900001\x00\x00", 20);
    {
        PFloorCrypto c;
        CHECK(c.init("AES_CM_128_HMAC_SHA1_80", key, salt, "", err), "init ok");
        CHECK(c.enabled(), "enabled");

        int secLen = 0, backLen = 0;
        CHECK(c.protect(plain, plainLen, sec, sizeof(sec), secLen), "protect ok");
        CHECK(secLen == plainLen + 4 + 10, "SRTCP overhead = 4(E+index) + 10(tag)");
        CHECK(memcmp(sec, plain, 8) == 0, "RTCP header stays in clear");
        CHECK(memcmp(sec + 8, plain + 8, plainLen - 8) != 0, "body is encrypted");
        CHECK(((unsigned char)sec[plainLen] & 0x80) != 0, "E flag set");

        PFloorCrypto d;
        d.init("AES_CM_128_HMAC_SHA1_80", key, salt, "", err);
        CHECK(d.unprotect(sec, secLen, back, sizeof(back), backLen), "unprotect ok");
        CHECK(backLen == plainLen && memcmp(back, plain, plainLen) == 0, "round trip restores plaintext");
    }

    // 3) 위조 거부 (본문 1비트 / 태그 1비트)
    {
        PFloorCrypto c, d;
        c.init("", key, salt, "", err);
        d.init("", key, salt, "", err);
        int secLen = 0, backLen = 0;
        c.protect(plain, plainLen, sec, sizeof(sec), secLen);

        char tampered[512];
        memcpy(tampered, sec, secLen);
        tampered[12] ^= 0x01;
        CHECK(!d.unprotect(tampered, secLen, back, sizeof(back), backLen), "body tamper rejected");

        memcpy(tampered, sec, secLen);
        tampered[secLen - 1] ^= 0x01;
        CHECK(!d.unprotect(tampered, secLen, back, sizeof(back), backLen), "tag tamper rejected");

        CHECK(!d.unprotect(sec, 8, back, sizeof(back), backLen), "truncated packet rejected");
    }

    // 4) 재전송 거부 + 후속 패킷 통과
    {
        PFloorCrypto c, d;
        c.init("", key, salt, "", err);
        d.init("", key, salt, "", err);
        int len1 = 0, len2 = 0, backLen = 0;
        char sec2[512];
        c.protect(plain, plainLen, sec, sizeof(sec), len1);
        c.protect(plain, plainLen, sec2, sizeof(sec2), len2);
        CHECK(memcmp(sec, sec2, len1) != 0, "index advances per packet");

        CHECK(d.unprotect(sec, len1, back, sizeof(back), backLen), "first accepted");
        CHECK(!d.unprotect(sec, len1, back, sizeof(back), backLen), "replay rejected");
        CHECK(d.unprotect(sec2, len2, back, sizeof(back), backLen), "next index accepted");
    }

    // 5) MKI 동봉/불일치
    {
        PFloorCrypto c, d, e;
        std::string mkiA = hex2bin("deadbeef"), mkiB = hex2bin("cafebabe");
        c.init("", key, salt, mkiA, err);
        d.init("", key, salt, mkiA, err);
        e.init("", key, salt, mkiB, err);
        int secLen = 0, backLen = 0;
        CHECK(c.protect(plain, plainLen, sec, sizeof(sec), secLen), "protect with MKI");
        CHECK(secLen == plainLen + 4 + 4 + 10, "MKI included in packet");
        CHECK(d.unprotect(sec, secLen, back, sizeof(back), backLen), "matching MKI accepted");
        CHECK(!e.unprotect(sec, secLen, back, sizeof(back), backLen), "mismatching MKI rejected");
    }

    // 6) 잘못된 파라미터
    {
        PFloorCrypto c;
        CHECK(!c.init("AES_CM_256_HMAC_SHA1_80", key, salt, "", err), "unknown alg rejected");
        CHECK(!c.init("", key.substr(0, 15), salt, "", err), "short key rejected");
        CHECK(!c.init("", key, salt.substr(0, 13), "", err), "short salt rejected");
        CHECK(!c.enabled(), "stays disabled after failures");
        CHECK(c.init("AES_CM_128_HMAC_SHA1_32", key, salt, "", err), "32bit tag profile ok");
        int secLen = 0;
        c.protect(plain, plainLen, sec, sizeof(sec), secLen);
        CHECK(secLen == plainLen + 4 + 4, "32bit tag overhead");
    }

    // 7) 제어평면 인코딩 디코더
    {
        std::string out;
        CHECK(PFloorCrypto::DecodeBase64(b64(key), out) && out == key, "base64 round trip");
        CHECK(PFloorCrypto::DecodeHex("00ff10", out) && out.size() == 3 && (unsigned char)out[1] == 0xFF, "hex decode");
        CHECK(!PFloorCrypto::DecodeHex("abc", out), "odd-length hex rejected");
        CHECK(!PFloorCrypto::DecodeBase64("****", out), "invalid base64 rejected");
    }

    printf("\ncmp_floor_crypto_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
