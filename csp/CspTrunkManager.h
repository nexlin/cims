#ifndef __CSP_TRUNK_MANAGER_H__
#define __CSP_TRUNK_MANAGER_H__

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <ctime>
#include <atomic>

class CSipMessage;

/**
 * CspTrunkManager — CspConfigCache(trunk) 기반 원격 SIP 서버(트렁크) 관리.
 *
 *   - 캐시 sync: 트렁크 정의 로드 → 메모리 상태 초기화
 *   - 헬스 체크: 설정된 주기(options_ping_sec) 마다 OPTIONS 전송
 *                → 응답/타임아웃에 따라 alive/dead 상태 전이
 *   - Call-ID 로 pending OPTIONS 매칭: RecvResponse 가 이 매니저로 전달되도록
 *     ModuleDispatcher 에서 훅 호출
 *   - 상태 조회: CSC STATS_REQUEST 에 trunk 요약 포함
 *
 *   P3 범위: UDP 트렁크. TCP/TLS 는 stack 단일 소켓 제약 때문에 추후.
 */

struct TrunkRuntime {
    int         id;
    std::string name;
    bool        enabled;
    int         serviceId = 0;          // P7: 소속 서비스 (0 = 미지정)
    int         failoverPriority = 100; // P7: 같은 서비스 내 순위
    std::string remoteIp;
    int         remotePort;
    std::string remoteDomain;
    std::string protocol;           // "UDP" 만 실제 처리
    int         optionsPingSec;     // 0 = 비활성
    int         deadThreshold;
    // 상태
    std::atomic<bool> alive{false};
    std::atomic<int>  consecutiveFailures{0};
    std::atomic<time_t> lastPingAt{0};
    std::atomic<time_t> lastReplyAt{0};
    std::atomic<int>  lastRttMs{-1};   // -1 = 미측정
    std::atomic<int>  optionsSeq{0};
    // OPTIONS 중 pending(응답 대기) Call-ID. 빈 문자열이면 없음.
    std::string       pendingCallId;
    time_t            pendingSentAt = 0;
    std::mutex        mtx;            // pendingCallId / pendingSentAt 전용
};

class CCspTrunkManager {
public:
    CCspTrunkManager() = default;
    ~CCspTrunkManager();

    /** 기동 시 1회. 설정 캐시에서 trunk 정의 로드 + 헬스 체크 스레드 시작. */
    bool Start();

    /** 종료. 헬스 스레드 종료 대기 후 메모리 정리. */
    void Stop();

    /** 캐시에서 최신 trunk 설정을 다시 읽어 메모리 상태 동기화.
     *  LISTENER/TRUNK_CHANGED 이벤트나 CSC_RESTART 시 호출. */
    bool Sync();

    /** OPTIONS 응답 수신 시 호출 (ModuleDispatcher::RecvResponse → 여기).
     *  Call-ID 로 pending 매칭 → alive + lastRttMs 업데이트.
     *  반환 true: 매칭된 트렁크 응답이었음(다른 처리 불필요) / false: 관심 없음. */
    bool OnSipResponse(const std::string& callId, int statusCode);

    /** 상태 스냅샷 (CSC STATS_REQUEST 응답 구성용). */
    struct StatusEntry {
        int id;
        std::string name;
        std::string remote;    // ip:port
        bool enabled;
        bool alive;
        int  last_rtt_ms;
        time_t last_ping;
        time_t last_reply;
        int  fail_count;
        int  service_id = 0;
        int  failover_priority = 100;
    };
    void GetStatus(std::vector<StatusEntry>& out);

    /** 서비스별 alive 트렁크를 우선순위 순으로 나열.
     *  RouteEngine 이 target.mode=service 일 때 호출. */
    struct TrunkRef {
        int id;
        std::string remote_ip;
        int         remote_port;
        std::string protocol;
        bool        alive;
        int         failover_priority;
    };
    void GetTrunksByService(int service_id, std::vector<TrunkRef>& out);

private:
    void _healthLoop();
    void _sendOptions(TrunkRuntime& t);
    void _checkTimeouts(time_t now);
    void _loadFromCache();

    std::mutex m_mutex;
    std::map<int, TrunkRuntime*> m_mapTrunks;   // id → runtime
    std::map<std::string, int>    m_mapCallIdToTrunk; // pending Call-ID → trunk id

    std::atomic<bool> m_bStop{false};
    // pthread_t 대신 간단한 std::thread 사용 (소유 권한 단순)
    // psip 가 pthread 기반이라 std::thread 혼용해도 무방
};

extern CCspTrunkManager gclsTrunkManager;

#endif // __CSP_TRUNK_MANAGER_H__
