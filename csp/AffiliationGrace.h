// 연결 실패로 풀린 등록의 제휴 유예 회수 (registration_binding_set.md §4.4)
//
// 스트림 transport 의 flow 실패(계기 2)는 순단에도 마지막 바인딩을 지워 등록 해제로 이어진다. 그때 제휴를 곧바로
// 지우면 단말이 다시 등록해도 제휴가 없어 편성 그룹 [참여] 가 403(Warning 120)으로 거절되고 그 사이 그룹콜 초대도
// 놓친다. 규격에서 제휴를 한꺼번에 내리는 것은 로그오프·등록 종료다(TS 24.379 §7.3.5 NOTE — 제휴는 등록에 묶인다).
// 연결 하나가 끊긴 것은 등록 종료가 아니므로(IMS 등록은 만료·해지까지 산다), flow 실패로 풀린 등록의 제휴는
// **그 등록의 수명(등록 시각 + Expires + grace)까지 남긴다**. 그 안에 다시 등록하면 그대로 잇고, 넘기면 그때 회수한다.
// 해지 REGISTER·등록 만료는 지금대로 즉시다.
//
// 대기열은 메모리에만 있다 — CSP 가 재기동하면 유예 중이던 행은 회수하지 않은 채 남는다(재기동 중 등록이 끝난
// 가입자와 같은 처지). 그 사람이 다시 등록하면 단말이 제휴를 다시 싣는다(ue_sdk.md §4.2 «등록에 묶인 것의 유지»).
#pragma once

#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class CAffiliationGrace {
public:
    /** flow 실패로 등록이 풀렸다 — 제휴 회수를 iDeadline(등록 수명의 끝, epoch 초)까지 미룬다.
     *  이미 미뤄 둔 것은 늦은 쪽을 남긴다. */
    void Defer( const std::string &strUserId, time_t iDeadline ) {
        std::lock_guard<std::mutex> lock( m_mutex );
        auto it = m_mapDeadline.find( strUserId );
        if ( it == m_mapDeadline.end() || it->second < iDeadline ) m_mapDeadline[strUserId] = iDeadline;
    }

    /** 다시 등록했다(또는 해지·만료로 즉시 회수한다) — 미뤄 둔 회수를 거둔다. 미뤄 둔 것이 있었으면 true. */
    bool Cancel( const std::string &strUserId ) {
        std::lock_guard<std::mutex> lock( m_mutex );
        return m_mapDeadline.erase( strUserId ) > 0;
    }

    /** 미뤄 둔 회수가 있나. */
    bool Pending( const std::string &strUserId ) const {
        std::lock_guard<std::mutex> lock( m_mutex );
        return m_mapDeadline.count( strUserId ) > 0;
    }

    /** iNow 에 시한이 지난 가입자 — 대기열에서 빼서 돌려준다(회수는 부른 쪽이 한다). */
    std::vector<std::string> TakeDue( time_t iNow ) {
        std::vector<std::string> vecDue;
        std::lock_guard<std::mutex> lock( m_mutex );
        for ( auto it = m_mapDeadline.begin(); it != m_mapDeadline.end(); ) {
            if ( it->second <= iNow ) {
                vecDue.push_back( it->first );
                it = m_mapDeadline.erase( it );
            } else {
                ++it;
            }
        }
        return vecDue;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock( m_mutex );
        return m_mapDeadline.size();
    }

private:
    mutable std::mutex m_mutex;
    std::map<std::string, time_t> m_mapDeadline;  // 가입자 → 회수 시한
};

extern CAffiliationGrace gclsAffiliationGrace;
