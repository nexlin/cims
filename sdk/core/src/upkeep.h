// libcimsue 내부 — 등록에 묶인 게시·구독의 유지(ue_sdk.md §4.2 «등록에 묶인 것의 유지»).
// 단위시험이 엔진 부팅 없이 판정을 고정한다 — «언제 무엇을 다시 싣나».
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace cimsue {
namespace detail {

/** 유지하는 것의 종류 — 제휴 게시(PUBLISH, RFC 3903)와 구독(SUBSCRIBE, RFC 6665). */
enum class UpkeepKind { McpttAffiliation, McVideoAffiliation, XcapDiff, Dialog };

/** 유지 단위. 제휴 게시는 집합 하나가 게시 하나라 target 이 비고(TS 24.379 §9.2.1.2·TS 24.281 §8.2.1.2), 구독은 구독 대상
 *  (PSI·감시 AoR)이다. conference 구독은 세션에 묶여 여기 없다(엔진 conferenceSubs). */
struct UpkeepKey {
    int account = -1;
    UpkeepKind kind = UpkeepKind::McpttAffiliation;
    std::string target;
    bool operator<(const UpkeepKey& o) const {
        return std::tie(account, kind, target) < std::tie(o.account, o.kind, o.target);
    }
    bool operator==(const UpkeepKey& o) const {
        return account == o.account && kind == o.kind && target == o.target;
    }
};

/**
 * 앱이 켠 게시·구독(목표 집합)을 들고, 서버가 잃었거나 곧 잃을 것을 다시 싣는다. 서버는 등록이 끝나면 그 가입자의 제휴를 내리고
 * (TS 24.379 §9 — 제휴는 등록에 묶인다), 구독·게시는 수명이 있다. 그래서 다시 싣는 계기는 셋이다.
 *
 *  ① 등록이 끊겼다 다시 섰다 — 등록 **이벤트** 기준(스냅샷은 같은 값을 합쳐 Registered→Registered 를 못 본다)
 *  ② 망이 바뀐 뒤 첫 등록 성공 — 상태는 «등록됨» 그대로여도 서버의 바인딩은 새것일 수 있다(Engine::handleNetworkChange)
 *  ③ 부여된 수명의 절반 — 게시 갱신(RFC 3903 §4.1)·구독 갱신(RFC 6665 §4.1.2.2). 만료 없는 부여(2^32-1)는 갱신하지 않는다
 *
 * 거절·응답 없음 뒤에는 1분부터 배로, 최대 30분 물러나 다시 싣는다(멤버가 아닌 그룹 게시를 매 분 보내지 않는다).
 * ue-ctl 에서만 쓴다(잠금 없음).
 */
class Upkeep {
public:
    static constexpr int64_t kTickMs = 60000;                // 갱신 시각을 보는 주기
    static constexpr int64_t kDefaultLifetimeSec = 3600;     // 응답에 Expires 가 없을 때 — 코어가 요청하는 값
    static constexpr int64_t kNoExpirySec = 4294967295LL;    // 2^32-1 = 사실상 무기한(TS 24.379 §9.2.1.2 NOTE 3)
    static constexpr int64_t kInflightTimeoutMs = 40000;     // 최종 응답이 없을 때(타이머 F ≈ 32 s 보다 넉넉히) 실패로 본다
    static constexpr int64_t kRetryBaseMs = 60000;
    static constexpr int64_t kRetryMaxMs = 30 * 60000;

    /** 거절·응답 없음 failures 번째 뒤 다음 시도까지 — 1분부터 배로, 최대 30분. */
    static int64_t retryDelayMs(int failures) {
        int shift = failures - 1;
        if (shift < 0) shift = 0;
        if (shift > 5) shift = 5;
        int64_t d = kRetryBaseMs << shift;
        return d > kRetryMaxMs ? kRetryMaxMs : d;
    }

    /** 앱이 켰다 / 껐다. 끈 것은 유지하지 않는다(해지 요청 결과도 무시). */
    void want(const UpkeepKey& k) { items_[k]; }
    void unwant(const UpkeepKey& k) { items_.erase(k); }
    bool wanted(const UpkeepKey& k) const { return items_.count(k) > 0; }

    /** 요청을 보냈다(앱 요청이든 유지 요청이든). */
    void sent(const UpkeepKey& k, int64_t nowMs) {
        auto it = items_.find(k);
        if (it == items_.end()) return;
        it->second.inflight = true;
        it->second.sentMs = nowMs;
    }

    /** 최종 응답 — 2xx 면 grantedSec(응답 Expires, 0 이하 = 없음) 로 다음 갱신 시각을, 그 밖이면 물러남을 잡는다. */
    void result(const UpkeepKey& k, int code, int64_t grantedSec, int64_t nowMs) {
        auto it = items_.find(k);
        if (it == items_.end()) return;
        Item& i = it->second;
        i.inflight = false;
        if (code / 100 == 2) {
            i.confirmed = true;
            i.confirmedMs = nowMs;
            i.lifetimeSec = grantedSec > 0 ? grantedSec : kDefaultLifetimeSec;
            i.failures = 0;
            i.retryAtMs = 0;
        } else {
            i.confirmed = false;
            ++i.failures;
            i.retryAtMs = nowMs + retryDelayMs(i.failures);
        }
    }

    /** 망이 바뀌었다 — 이 계정들의 다음 등록 성공에 전부 다시 싣는다(②). */
    void networkChanged(const std::vector<int>& accounts) { networkChanged_.insert(accounts.begin(), accounts.end()); }

    /**
     * 등록 이벤트(REGISTER 응답마다 — 갱신 포함). 이 이벤트로 전부 다시 실어야 하면 true(①·②).
     * 계정의 첫 이벤트는 다시 싣지 않는다 — 처음 것은 앱이 건다.
     */
    bool onRegEvent(int account, bool registered) {
        auto prev = registered_.find(account);
        const bool hadPrev = prev != registered_.end();
        const bool was = hadPrev && prev->second;
        registered_[account] = registered;
        if (!registered) return false;
        const bool changed = networkChanged_.erase(account) > 0;
        return (hadPrev && !was) || changed;
    }
    bool registered(int account) const {
        auto it = registered_.find(account);
        return it != registered_.end() && it->second;
    }

    /** 이 계정이 켠 것 전부(①·② 의 다시 싣기 대상). */
    std::vector<UpkeepKey> wantedFor(int account) const {
        std::vector<UpkeepKey> out;
        for (const auto& kv : items_) if (kv.first.account == account) out.push_back(kv.first);
        return out;
    }

    /** nowMs 에 다시 실을 것 — 등록된 계정의 것만. 갱신(③)은 확인된 것, 재시도는 물러남이 끝난 것, 응답 없이 시한을 넘긴 것. */
    std::vector<UpkeepKey> due(int64_t nowMs) {
        std::vector<UpkeepKey> out;
        for (auto& kv : items_) {
            if (!registered(kv.first.account)) continue;
            Item& i = kv.second;
            if (i.inflight) {
                if (nowMs - i.sentMs < kInflightTimeoutMs) continue;
                i.inflight = false;                          // 최종 응답 없음 = 실패
                i.confirmed = false;
                ++i.failures;
                i.retryAtMs = i.sentMs + retryDelayMs(i.failures);
            }
            if (i.confirmed) {
                if (i.lifetimeSec >= kNoExpirySec) continue;
                if (nowMs - i.confirmedMs >= i.lifetimeSec * 500) out.push_back(kv.first);
            } else if (i.failures > 0) {
                if (nowMs >= i.retryAtMs) out.push_back(kv.first);
            }
        }
        return out;
    }

    /** 이 게시·구독의 마지막 시도가 실패였나(재시도라면 조건부 게시가 아니라 초기 게시로). */
    bool failing(const UpkeepKey& k) const {
        auto it = items_.find(k);
        return it != items_.end() && it->second.failures > 0;
    }

    void dropAccount(int account) {
        for (auto it = items_.begin(); it != items_.end();) it = it->first.account == account ? items_.erase(it) : std::next(it);
        registered_.erase(account);
        networkChanged_.erase(account);
    }
    void clear() { items_.clear(); registered_.clear(); networkChanged_.clear(); }
    size_t size() const { return items_.size(); }

private:
    struct Item {
        bool inflight = false;
        int64_t sentMs = 0;
        bool confirmed = false;                              // 최근 시도가 2xx 였다
        int64_t confirmedMs = 0;
        int64_t lifetimeSec = kDefaultLifetimeSec;
        int failures = 0;
        int64_t retryAtMs = 0;
    };
    std::map<UpkeepKey, Item> items_;
    std::map<int, bool> registered_;                         // 계정별 마지막 등록 이벤트가 «등록됨» 이었나
    std::set<int> networkChanged_;                           // 망이 바뀐 뒤 등록 결과를 아직 보지 못한 계정
};

}  // namespace detail
}  // namespace cimsue
