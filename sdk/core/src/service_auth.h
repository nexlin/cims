// libcimsue 내부 — MC 서비스 인가·서비스 설정(TS 24.379 §7.2 · TS 24.282 §7.2 · TS 24.281 §7.2, ue_sdk.md §4.2 «서비스 인가»).
// 단위시험이 엔진 부팅 없이 판정을 고정한다 — «언제 인가 PUBLISH 를 (다시) 보내나», 본문의 모양.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cimsue/types.h"

namespace cimsue {
namespace detail {

/** 인가를 보낼 계기. */
enum class AuthTrigger {
    Registered,     // 등록 이벤트(갱신 포함) — 이 등록에서 아직 보내지 않았으면
    Renewed,        // 등록이 끊겼다 다시 섰거나 망이 바뀐 뒤 첫 등록 — 서버의 묶임이 새것일 수 있다(upkeep.h ①·②)
    TokenChanged,   // 앱이 새 토큰을 줬다 — 인가 안 된 서비스만
    BindingLost,    // MC 요청이 404 `141` — 인가된 줄 알았는데 서버가 묶임을 잃었다(재기동 등)
    Retry,          // 일시 실패(5xx·408) 뒤 물러남이 끝났다
};

/**
 * 계정 × MC 서비스의 인가 상태. 서버의 (MC ID, client ID, IMPU) 묶임은 인가 PUBLISH 로 생기고 등록과 함께 산다(§7.3.3 5) ·
 * §7.3.5) — 그래서 등록이 설 때마다(첫 등록·재성립·망 변경) 인가하고, 거절(403 `101` 인가 실패 · 486 `164` 동시 인가 상한 등)은
 * 새 토큰을 받을 때까지 다시 보내지 않으며(§7.2.2 끝 — 인가 안 됨으로 본다), 5xx·408·응답 없음은 물러나 다시 보낸다.
 * Pending 동안 엔진은 그 서비스의 제휴 게시를 내보내지 않는다(인가 뒤 실음 — 서버의 141 경합 방지). ue-ctl 에서만 쓴다(잠금 없음).
 */
class ServiceAuth {
public:
    using Key = std::pair<int, McService>;
    /** 141 로 다시 인가하는 간격 하한 — 인가 직후 거절(인가 전에 보낸 요청의 응답)로 되풀이하지 않는다. 몰린 141 은 첫 것만 인가를
     *  다시 열고(그동안 Pending) 나머지는 버린다. */
    static constexpr int64_t kBindingLostMinMs = 2000;

    struct Entry {
        ServiceAuthInfo info;
        bool attempted = false;        // 이 등록에서 판정을 냈다(보냈거나 토큰이 없어 못 보냈다)
        bool transient = false;        // 마지막 실패가 일시적(물러나 다시 보낸다)
        int failures = 0;              // 연속 일시 실패
        int64_t retryAtMs = 0;
        int64_t sentMs = INT64_MIN / 2;
        int64_t token = -1;            // 응답을 기다리는 인가 PUBLISH
        std::string etag;              // 서버가 준 SIP-ETag(RFC 3903) — 설정 제거(Expires 0)의 SIP-If-Match
    };

    /** 이 계기로 지금 인가를 보내야 하나(계정이 등록돼 있다는 전제 — 엔진이 본다). */
    bool shouldAuthorize(const Key& k, AuthTrigger t, int64_t nowMs) const {
        auto it = items_.find(k);
        if (it == items_.end()) return t != AuthTrigger::Retry && t != AuthTrigger::BindingLost;
        const Entry& e = it->second;
        if (e.info.state == ServiceAuthState::Pending) return false;
        switch (t) {
            case AuthTrigger::Registered: return !e.attempted;
            case AuthTrigger::Renewed: return true;
            case AuthTrigger::TokenChanged: return e.info.state == ServiceAuthState::Unauthorized;
            case AuthTrigger::BindingLost:
                return e.info.state == ServiceAuthState::Authorized && nowMs - e.sentMs >= kBindingLostMinMs;
            case AuthTrigger::Retry: return e.info.state == ServiceAuthState::Unauthorized && e.transient && nowMs >= e.retryAtMs;
        }
        return false;
    }

    /** 인가 PUBLISH 를 보냈다. */
    void sent(const Key& k, int64_t token, int64_t nowMs) {
        Entry& e = items_[k];
        e.info.accountId = k.first;
        e.info.service = k.second;
        e.info.state = ServiceAuthState::Pending;
        e.attempted = true;
        e.sentMs = nowMs;
        e.token = token;
    }

    /** 보내지 못했다 — 토큰·client ID 가 없다(transient = false — 앱이 새 토큰을 줄 때까지) 또는 스택이 보내지 못했다(일시). */
    void notSent(const Key& k, bool transient, int64_t nowMs) {
        Entry& e = items_[k];
        e.info.accountId = k.first;
        e.info.service = k.second;
        e.info.state = ServiceAuthState::Unauthorized;
        e.info.code = 0;
        e.info.warningCode = 0;
        e.info.warningText.clear();
        e.info.multipleDevices = false;
        e.attempted = true;
        e.token = -1;
        fail(e, transient, 0, nowMs);
    }

    /** 인가 PUBLISH 의 최종 응답. retryAfterSec = Retry-After(없으면 0). 이 서비스의 인가 응답이 아니면 false. */
    bool result(const Key& k, int64_t token, int code, int warningCode, const std::string& warningText, bool multipleDevices,
                const std::string& etag, int retryAfterSec, int64_t nowMs) {
        auto it = items_.find(k);
        if (it == items_.end() || it->second.token != token) return false;
        Entry& e = it->second;
        e.token = -1;
        e.info.code = code;
        e.info.warningCode = warningCode;
        e.info.warningText = warningText;
        e.info.multipleDevices = code / 100 == 2 && multipleDevices;
        if (code / 100 == 2) {
            e.info.state = ServiceAuthState::Authorized;
            e.transient = false;
            e.failures = 0;
            if (!etag.empty()) e.etag = etag;
            return true;
        }
        e.info.state = ServiceAuthState::Unauthorized;
        e.etag.clear();
        fail(e, code == 408 || code / 100 == 5 || code == 0, retryAfterSec, nowMs);
        return true;
    }

    /** 등록이 끊겼다 — 서버는 등록과 함께 묶임을 지운다. 다음 등록에서 다시 인가한다. 바뀐 서비스(알릴 것) 목록. */
    std::vector<Key> unregistered(int account) {
        std::vector<Key> changed;
        for (auto& kv : items_) {
            if (kv.first.first != account) continue;
            Entry& e = kv.second;
            const bool was = e.info.state != ServiceAuthState::Unauthorized || e.info.code != 0;
            e.info.state = ServiceAuthState::Unauthorized;
            e.info.code = 0;
            e.info.warningCode = 0;
            e.info.warningText.clear();
            e.info.multipleDevices = false;
            e.attempted = false;
            e.transient = false;
            e.failures = 0;
            e.token = -1;
            e.etag.clear();
            if (was) changed.push_back(kv.first);
        }
        return changed;
    }

    /** 설정 제거(Expires 0 — §7.2.1A 4) · NOTE 3 = 그 서비스 로그오프)를 보낸다 — 실을 SIP-ETag(없으면 빈 값, 보낼 것이 없다). */
    std::string removed(const Key& k) {
        auto it = items_.find(k);
        if (it == items_.end()) return std::string();
        const std::string etag = it->second.etag;
        items_.erase(it);
        return etag;
    }

    bool pending(const Key& k) const {
        auto it = items_.find(k);
        return it != items_.end() && it->second.info.state == ServiceAuthState::Pending;
    }
    const Entry* find(const Key& k) const {
        auto it = items_.find(k);
        return it == items_.end() ? nullptr : &it->second;
    }
    /** 응답 token → 그 인가(없으면 nullptr). */
    const Key* keyOfToken(int64_t token) const {
        for (const auto& kv : items_) if (kv.second.token == token) return &kv.first;
        return nullptr;
    }
    void dropAccount(int account) {
        for (auto it = items_.begin(); it != items_.end();) it = it->first.first == account ? items_.erase(it) : std::next(it);
    }
    void clear() { items_.clear(); }

    /** 일시 실패의 물러남 — Retry-After 가 있으면 그것, 없으면 5 s 부터 배로 최대 5분(인가 없이는 MC 서비스를 못 쓴다 — 제휴 유지의
     *  1분보다 짧게 시작한다). */
    static int64_t retryDelayMs(int failures, int retryAfterSec) {
        if (retryAfterSec > 0) return (int64_t)retryAfterSec * 1000;
        int shift = failures - 1;
        if (shift < 0) shift = 0;
        if (shift > 6) shift = 6;
        const int64_t d = (int64_t)5000 << shift;
        return d > 300000 ? 300000 : d;
    }

private:
    static void fail(Entry& e, bool transient, int retryAfterSec, int64_t nowMs) {
        e.transient = transient;
        if (!transient) { e.failures = 0; e.retryAtMs = 0; return; }
        ++e.failures;
        e.retryAtMs = nowMs + retryDelayMs(e.failures, retryAfterSec);
    }
    std::map<Key, Entry> items_;
};

constexpr const char* kCtPocSettings = "application/poc-settings+xml";   // RFC 4354 — 서비스 설정

/** 인가 PUBLISH 의 본문 하나 — multipart/mixed. */
struct ServiceAuthBody {
    std::string contentType;
    std::string body;
};

/**
 * 서비스 인가 + 서비스 설정 PUBLISH 본문(TS 24.379 §7.2.2 5)·6) · TS 24.282 §7.2.2 5)·6) · TS 24.281 §7.2.2 5)·6)) — 암호화하지 않는
 * 형식: `<svc>-info`(`<…-access-token>` · `<…-client-id>`) + poc-settings(entity id = client 의 Instance ID URN — §7.2.1A NOTE 2).
 * MCPTT·MCVideo 의 poc-settings = Answer-Mode(automatic|manual) · 선택 user profile · multiplex 지원, MCData = 선택 user profile 만.
 * 계약 골든 tests/fixtures/mcptt/sip/14 의 모양.
 */
ServiceAuthBody serviceAuthBody(McService s, const std::string& accessToken, const std::string& clientId, const std::string& entityId,
                                bool autoAnswer, const std::string& boundary);
/** poc-settings 문서(RFC 4354 · TS 24.379 §7.4.1.2 — 확장 요소는 `urn:3gpp:mcsSettings:1.0`). answerMode 가 비면 am-settings 를,
 *  multiplex < 0 이면 multiplex-support 를 싣지 않는다. CIMS 의 user profile 은 하나(index 1 — csc mcptt.py)다. */
std::string pocSettings(const std::string& entityId, const std::string& answerMode, int userProfileIndex, int multiplex);
/** 인가 200 OK 본문의 `<multiple-devices-ind>` 가 true 인가(§7.3.3 9)a) — 서비스 info 문서, multipart 안이어도). */
bool multipleDevicesInd(const std::string& body);
/** 서비스의 ICSI(P-Preferred-Service — §7.2.1A 2)). */
const char* serviceIcsi(McService s);

}  // namespace detail
}  // namespace cimsue
