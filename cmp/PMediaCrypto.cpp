#include "PMediaCrypto.h"
#include "PLog.h"
#include <cstring>
#include <mutex>
#include <srtp2/srtp.h>

// srtp_init() 은 프로세스 1회 (media_security.md §6.1). 실패는 치명 — 이후 모든
// init() 이 실패해 SRTP leg 명령이 거부된다(평문 조용 폴백 없음).
static bool _srtpLibReady() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        srtp_err_status_t st = srtp_init();
        ok = (st == srtp_err_status_ok);
        if (!ok) LOG_ERROR("PMediaCrypto", "srtp_init failed (status=%d)", (int)st);
    });
    return ok;
}

bool PMediaCrypto::IsSupportedAlg(const std::string& alg) {
    return alg.empty() ||   // 미지정 = 기본 AES_CM_128_HMAC_SHA1_80
           alg == "AES_CM_128_HMAC_SHA1_80" || alg == "AES_CM_128_HMAC_SHA1_32";
}

PMediaCrypto::~PMediaCrypto() { clear(); }

void PMediaCrypto::clear() {
    if (_rx) { srtp_dealloc((srtp_t)_rx); _rx = nullptr; }
    if (_tx) { srtp_dealloc((srtp_t)_tx); _tx = nullptr; }
    _alg.clear();
    _rxKey.clear(); _rxSalt.clear(); _txKey.clear(); _txSalt.clear();
}

bool PMediaCrypto::_alloc(const std::string& who, bool inbound, const std::string& key,
                          const std::string& salt, srtp_ctx_t_** out, std::string& err) {
    // libsrtp policy key = master key(16B) || master salt(14B)
    unsigned char master[SRTP_AES_ICM_128_KEY_LEN_WSALT];
    memcpy(master, key.data(), SRTP_AES_128_KEY_LEN);
    memcpy(master + SRTP_AES_128_KEY_LEN, salt.data(), SRTP_SALT_LEN);

    srtp_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    if (_alg == "AES_CM_128_HMAC_SHA1_32") {
        // _32 는 SRTP 인증 태그만 4B — SRTCP 는 suite 정의상 80bit 유지 (RFC 4568 §6.2)
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&policy.rtp);
        srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
    } else {
        srtp_crypto_policy_set_rtp_default(&policy.rtp);
        srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
    }
    policy.ssrc.type = inbound ? ssrc_any_inbound : ssrc_any_outbound;
    policy.key = master;
    policy.window_size = 128;
    policy.allow_repeat_tx = 0;
    policy.next = nullptr;

    srtp_t session = nullptr;
    srtp_err_status_t st = srtp_create(&session, &policy);
    if (st != srtp_err_status_ok) {
        err = who + " srtp_create failed (status=" + std::to_string((int)st) + ")";
        return false;
    }
    *out = session;
    return true;
}

bool PMediaCrypto::init(const std::string& alg,
                        const std::string& rxKey, const std::string& rxSalt,
                        const std::string& txKey, const std::string& txSalt,
                        std::string& err) {
    if (!_srtpLibReady()) { err = "srtp library init failed"; return false; }
    if (!IsSupportedAlg(alg)) { err = "unsupported alg: " + alg; return false; }
    if (rxKey.size() != SRTP_AES_128_KEY_LEN || txKey.size() != SRTP_AES_128_KEY_LEN) {
        err = "key must be 16 bytes (AES-128)";
        return false;
    }
    if (rxSalt.size() != SRTP_SALT_LEN || txSalt.size() != SRTP_SALT_LEN) {
        err = "salt must be 14 bytes";
        return false;
    }
    std::string effAlg = alg.empty() ? "AES_CM_128_HMAC_SHA1_80" : alg;

    // 동일 구성 재선언(refresh/MODIFY 재전송) — 세션 유지. 재생성하면 ROC·재전송 창이
    // 리셋되어 상대의 연속 스트림이 인증 실패한다 (키 유지 규칙, media_security.md §5.2).
    if (_rx && _tx && _alg == effAlg &&
        _rxKey == rxKey && _rxSalt == rxSalt && _txKey == txKey && _txSalt == txSalt)
        return true;

    clear();
    _alg = effAlg;
    if (!_alloc("rx", true, rxKey, rxSalt, &_rx, err) ||
        !_alloc("tx", false, txKey, txSalt, &_tx, err)) {
        clear();
        return false;
    }
    _rxKey = rxKey; _rxSalt = rxSalt; _txKey = txKey; _txSalt = txSalt;
    return true;
}

bool PMediaCrypto::protectRtp(char* buf, int& len, int cap) {
    if (!_tx || len < 12 || len + kMaxOverhead > cap) return false;
    int n = len;
    if (srtp_protect((srtp_t)_tx, buf, &n) != srtp_err_status_ok) return false;
    len = n;
    return true;
}

bool PMediaCrypto::unprotectRtp(char* buf, int& len) {
    if (!_rx || len < 12) return false;
    int n = len;
    if (srtp_unprotect((srtp_t)_rx, buf, &n) != srtp_err_status_ok) return false;
    len = n;
    return true;
}

bool PMediaCrypto::protectRtcp(char* buf, int& len, int cap) {
    if (!_tx || len < 8 || len + kMaxOverhead + 4 > cap) return false;   // +4 = E/SRTCP index
    int n = len;
    if (srtp_protect_rtcp((srtp_t)_tx, buf, &n) != srtp_err_status_ok) return false;
    len = n;
    return true;
}

bool PMediaCrypto::unprotectRtcp(char* buf, int& len) {
    if (!_rx || len < 8) return false;
    int n = len;
    if (srtp_unprotect_rtcp((srtp_t)_rx, buf, &n) != srtp_err_status_ok) return false;
    len = n;
    return true;
}
