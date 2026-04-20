#ifndef __CSP_ACCESS_CONTROL_H__
#define __CSP_ACCESS_CONTROL_H__

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <ctime>

/**
 * CspAccessControl — SIP 수신 메시지의 접근제어 + rate limit.
 *
 *   - routing_access_list (캐시) 에서 allow/deny 규칙 로드
 *     scope: global / listener / trunk
 *     match_type: ip / cidr / ua_regex
 *     priority 낮을수록 먼저 평가 (first-match-wins)
 *     kind=deny 먼저 매칭하면 차단, kind=allow 매칭하면 허용
 *     둘 다 매칭 없으면 기본 허용
 *
 *   - Rate limit: per-source-IP 토큰 버킷. cmp.json 의 Security.RateLimit 또는
 *     P5 후속 DB 필드로 설정. 현재는 하드코딩된 기본값(100rps/ip, burst 200)
 *     을 사용하고 0 설정 시 비활성.
 *
 *   P5 범위: IP + CIDR 매칭, UA regex (optional). token bucket 간단 구현.
 */

struct AccessEntry {
    int         id = 0;
    std::string scope;          // "global" | "listener" | "trunk"
    int         scope_ref_id = 0;
    std::string kind;           // "allow" | "deny"
    std::string match_type;     // "ip" | "cidr" | "ua_regex"
    std::string value;
    bool        enabled = true;
    int         priority = 100;
};

class CCspAccessControl {
public:
    CCspAccessControl() = default;

    /** 캐시에서 규칙 로드 (캐시 갱신 후 호출). */
    bool Sync();

    /** per-IP 체크. listener_id 는 해당 요청이 수신된 리스너의 DB id(bootstrap 은 0). */
    struct Decision {
        bool allowed = true;        // true 면 통과
        std::string reason;         // 차단 사유 (로그용)
        int  http_code = 0;         // 차단 시 응답 SIP status (403 / 429)
    };
    Decision Check(const std::string& source_ip, int listener_id, const std::string& user_agent);

    /** 초기값 지정: rps_per_ip=0 이면 rate limit 비활성. */
    void SetRateLimit(int rps_per_ip, int burst);

private:
    std::mutex m_mutex;
    std::vector<AccessEntry> m_acl;    // priority 정렬

    // token bucket: IP → {tokens, last_refill_ts}
    struct Bucket {
        double tokens = 0;
        double last_ts = 0;
    };
    std::unordered_map<std::string, Bucket> m_buckets;
    std::atomic<int> m_rpsPerIp{0};    // 0 = disabled
    std::atomic<int> m_burst{0};

    bool _evaluateAcl(const std::string& ip, int listener_id, const std::string& ua,
                      std::string& outReason);
    bool _consumeToken(const std::string& ip);
};

extern CCspAccessControl gclsAccessControl;

#endif // __CSP_ACCESS_CONTROL_H__
