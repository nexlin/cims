// libcimsue 내부 — 망 변경 뒤 재등록의 줄(Engine::handleNetworkChange, ue_sdk.md §4.2).
// 단위시험이 엔진 부팅 없이 판정을 고정한다 — «누구를 다시 등록하나» 와 «앞 등록이 걸려 있으면 언제 보내나».
#pragma once

#include <set>
#include <vector>

namespace cimsue {
namespace detail {

/**
 * 계정마다 «다시 등록해야 한다» 를 들고, **앞 등록 트랜잭션이 끝난 뒤에만** 보낸다(RFC 3261 §10.2 — 앞 REGISTER 의 최종
 * 응답이나 타임아웃 전에는 새 등록을 보내지 않는다).
 *
 * pjsip 는 등록 트랜잭션이 걸려 있으면 새 REGISTER 를 `PJSIP_EBUSY` 로 거절한다. 그때 regc 를 부수고 새로 만들어 보내면
 * (pjsua 의 IP 변경 처리가 그렇게 한다) 옛 트랜잭션이 살아 있는 채로 새 등록이 나간다 — UDP 에서는 옛 트랜잭션을 끝낼
 * 전송 종료도 없다. 그래서 거절되면 표시만 해 두고, 그 계정의 등록 결과(`onRegState`)가 오면 한 번 더 보낸다.
 * 끝난 트랜잭션이 실패였다면 pjsua 의 자동 재시도도 살아 있다(일반 등록 경로라 IP 변경 모드의 재시도 제외가 없다).
 */
class RegRecovery {
public:
    /** 앱이 등록을 켠 계정(registerAccount). */
    void want(int id) { wanted_.insert(id); }
    bool wanted(int id) const { return wanted_.count(id) > 0; }
    /** 등록을 끈 계정(unregisterAccount·removeAccount) — 망이 바뀌어도 다시 등록하지 않는다. */
    void unwant(int id) { wanted_.erase(id); pending_.erase(id); }
    /** 망이 바뀌었다 — 다시 등록할 계정. */
    std::vector<int> targets() const { return {wanted_.begin(), wanted_.end()}; }
    /** 재등록이 거절됐다(앞 등록 진행 중). 그 트랜잭션이 끝나면 다시 보낸다. */
    void busy(int id) { if (wanted_.count(id)) pending_.insert(id); }
    /** 이 계정의 등록 트랜잭션이 끝났다 — 미뤄 둔 재등록을 지금 보내야 하면 true(한 번만). */
    bool settled(int id) { return pending_.erase(id) > 0 && wanted_.count(id) > 0; }
    /** 엔진 정지. */
    void clear() { wanted_.clear(); pending_.clear(); }

private:
    std::set<int> wanted_;
    std::set<int> pending_;
};

}  // namespace detail
}  // namespace cimsue
