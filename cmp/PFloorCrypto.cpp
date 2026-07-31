// PFloorCrypto.cpp — floor control RTCP(MCPT)의 SRTCP 보호 (RFC 3711 / 3GPP TS 33.180).
//
// 외부 의존은 OpenSSL(EVP/HMAC)뿐이라 단독 링크가 가능하다
// (단위테스트: tests/cmp_floor_crypto_test.cpp — RFC 3711 §B.3 KDF 벡터 + 왕복/위조/재전송).

#include "PFloorCrypto.h"
#include <cstring>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>

// AES-128 카운터 모드 — RFC 3711 의 AES-CM 키스트림과 동일(블록 카운터 증가 규칙 동일).
static bool _aesCtr(const unsigned char* key, const unsigned char iv[16],
                    const unsigned char* in, unsigned char* out, int len) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int ol = 0;
    bool ok = (EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key, iv) == 1) &&
              (EVP_EncryptUpdate(ctx, out, &ol, in, len) == 1) && (ol == len);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

// RFC 3711 §4.3.1 — x = (label ‖ index DIV kdr) XOR master_salt (우측 정렬),
//   IV = x * 2^16. KDR=0 이므로 index DIV kdr = 0 이고 label 만 salt[7] 에 XOR 된다.
static void _kdfIv(const unsigned char* masterSalt, unsigned char label, unsigned char iv[16]) {
    memset(iv, 0, 16);
    memcpy(iv, masterSalt, 14);
    iv[7] ^= label;
}

static bool _deriveKey(const unsigned char* masterKey, const unsigned char* masterSalt,
                       unsigned char label, unsigned char* out, int outLen) {
    unsigned char iv[16];
    _kdfIv(masterSalt, label, iv);
    unsigned char zeros[64];
    if (outLen > (int)sizeof(zeros)) return false;
    memset(zeros, 0, outLen);
    return _aesCtr(masterKey, iv, zeros, out, outLen);
}

bool PFloorCrypto::_deriveSessionKeys(std::string& err) {
    const unsigned char* mk = (const unsigned char*)_masterKey.data();
    const unsigned char* ms = (const unsigned char*)_masterSalt.data();
    // label 0x03=SRTCP 암호화, 0x04=SRTCP 인증, 0x05=SRTCP salt (RFC 3711 §4.3.2)
    if (!_deriveKey(mk, ms, 0x03, _sessKey, sizeof(_sessKey)) ||
        !_deriveKey(mk, ms, 0x04, _sessAuth, sizeof(_sessAuth)) ||
        !_deriveKey(mk, ms, 0x05, _sessSalt, sizeof(_sessSalt))) {
        err = "session key derivation failed";
        return false;
    }
    return true;
}

bool PFloorCrypto::init(const std::string& alg, const std::string& key, const std::string& salt,
                        const std::string& mki, std::string& err) {
    std::lock_guard<std::mutex> lock(_mtx);
    std::string a = alg.empty() ? "AES_CM_128_HMAC_SHA1_80" : alg;
    if (a == "AES_CM_128_HMAC_SHA1_80")      _tagLen = 10;
    else if (a == "AES_CM_128_HMAC_SHA1_32") _tagLen = 4;
    else { err = "unsupported floor_crypto alg (AES_CM_128_HMAC_SHA1_80|_32)"; return false; }

    if (key.size() != 16)  { err = "floor_crypto key must be 16 bytes (AES-128)"; return false; }
    if (salt.size() != 14) { err = "floor_crypto salt must be 14 bytes"; return false; }
    if (mki.size() > (size_t)kMaxMki) { err = "floor_crypto mki too long"; return false; }

    _alg = a;
    _masterKey = key;
    _masterSalt = salt;
    _mki = mki;
    if (!_deriveSessionKeys(err)) return false;
    _txIndex.clear();
    _rxReplay.clear();
    _enabled = true;
    return true;
}

// SRTCP IV (RFC 3711 §4.1.1, index 는 SRTCP index):
//   IV = (session_salt * 2^16) XOR (SSRC * 2^64) XOR (index * 2^16)
static void _srtcpIv(const unsigned char* sessSalt, uint32_t ssrc, uint32_t index, unsigned char iv[16]) {
    memset(iv, 0, 16);
    memcpy(iv, sessSalt, 14);
    iv[4]  ^= (unsigned char)((ssrc >> 24) & 0xFF);
    iv[5]  ^= (unsigned char)((ssrc >> 16) & 0xFF);
    iv[6]  ^= (unsigned char)((ssrc >> 8) & 0xFF);
    iv[7]  ^= (unsigned char)(ssrc & 0xFF);
    iv[10] ^= (unsigned char)((index >> 24) & 0xFF);
    iv[11] ^= (unsigned char)((index >> 16) & 0xFF);
    iv[12] ^= (unsigned char)((index >> 8) & 0xFF);
    iv[13] ^= (unsigned char)(index & 0xFF);
}

static uint32_t _be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void _putBe32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
}

bool PFloorCrypto::protect(const char* in, int inLen, char* out, int outCap, int& outLen) {
    if (!_enabled) return false;
    if (inLen < 8) return false;
    std::lock_guard<std::mutex> lock(_mtx);

    int total = inLen + 4 + (int)_mki.size() + _tagLen;
    if (total > outCap) return false;

    uint32_t ssrc = _be32((const unsigned char*)in + 4);
    uint32_t& next = _txIndex[ssrc];
    uint32_t index = next & 0x7FFFFFFFu;
    next = (index + 1) & 0x7FFFFFFFu;

    unsigned char* o = (unsigned char*)out;
    memcpy(o, in, 8);                                   // RTCP 헤더는 평문
    unsigned char iv[16];
    _srtcpIv(_sessSalt, ssrc, index, iv);
    if (!_aesCtr(_sessKey, iv, (const unsigned char*)in + 8, o + 8, inLen - 8)) return false;

    _putBe32(o + inLen, index | 0x80000000u);           // E=1(암호화) + SRTCP index
    int p = inLen + 4;
    if (!_mki.empty()) { memcpy(o + p, _mki.data(), _mki.size()); p += (int)_mki.size(); }

    // 인증 범위 = 헤더+본문+E/index (MKI·tag 제외)
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int macLen = 0;
    if (!HMAC(EVP_sha1(), _sessAuth, (int)sizeof(_sessAuth), o, inLen + 4, mac, &macLen)) return false;
    memcpy(o + p, mac, _tagLen);
    outLen = p + _tagLen;
    return true;
}

bool PFloorCrypto::unprotect(const char* in, int inLen, char* out, int outCap, int& outLen) {
    if (!_enabled) return false;
    int mkiLen = (int)_mki.size();
    int minLen = 8 + 4 + mkiLen + _tagLen;
    if (inLen < minLen) return false;
    std::lock_guard<std::mutex> lock(_mtx);

    const unsigned char* i = (const unsigned char*)in;
    int tagOff = inLen - _tagLen;
    int mkiOff = tagOff - mkiLen;
    int idxOff = mkiOff - 4;
    int bodyLen = idxOff - 8;
    if (bodyLen < 0 || idxOff > outCap) return false;   // 복호 결과 길이 = idxOff (=8+bodyLen)

    if (mkiLen > 0 && CRYPTO_memcmp(i + mkiOff, _mki.data(), mkiLen) != 0) return false;

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int macLen = 0;
    if (!HMAC(EVP_sha1(), _sessAuth, (int)sizeof(_sessAuth), i, idxOff + 4, mac, &macLen)) return false;
    if (CRYPTO_memcmp(mac, i + tagOff, _tagLen) != 0) return false;

    uint32_t eIndex = _be32(i + idxOff);
    bool encrypted = (eIndex & 0x80000000u) != 0;
    uint32_t index = eIndex & 0x7FFFFFFFu;

    // 재전송 방지 (RFC 3711 §3.3.2) — 최고 index 기준 64 슬롯 윈도우.
    //   비트 k = "index(highest-k) 수신함" (비트 0 = 최고 index 자신).
    ReplayCtx& rc = _rxReplay[_be32(i + 4)];
    if (rc.seen) {
        if (index > rc.highest) {
            uint32_t shift = index - rc.highest;
            rc.bitmap = (shift >= 64) ? 0 : (rc.bitmap << shift);
            rc.bitmap |= 1ULL;                       // 새 최고 index 수신 표시
            rc.highest = index;
        } else {
            uint32_t back = rc.highest - index;
            if (back >= 64) return false;            // 윈도우 밖 — 재전송/지연 폐기
            uint64_t bit = 1ULL << back;
            if (rc.bitmap & bit) return false;       // 이미 수신
            rc.bitmap |= bit;
        }
    } else {
        rc.seen = true;
        rc.highest = index;
        rc.bitmap = 1ULL;
    }

    memcpy(out, in, 8);
    if (encrypted) {
        unsigned char iv[16];
        _srtcpIv(_sessSalt, _be32(i + 4), index, iv);
        if (!_aesCtr(_sessKey, iv, i + 8, (unsigned char*)out + 8, bodyLen)) return false;
    } else {
        memcpy(out + 8, in + 8, bodyLen);
    }
    outLen = 8 + bodyLen;
    return true;
}

// ── 제어평면 인코딩 디코더 ────────────────────────────────────────────────

bool PFloorCrypto::DecodeBase64(const std::string& s, std::string& out) {
    static const signed char kTbl[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, 52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, 15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, 41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    out.clear();
    int acc = 0, bits = 0;
    for (unsigned char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        signed char v = kTbl[c];
        if (v < 0) return false;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((acc >> bits) & 0xFF));
        }
    }
    return true;
}

bool PFloorCrypto::DecodeHex(const std::string& s, std::string& out) {
    out.clear();
    if (s.size() % 2 != 0) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t k = 0; k + 1 < s.size(); k += 2) {
        int hi = nib(s[k]), lo = nib(s[k + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((char)((hi << 4) | lo));
    }
    return true;
}
