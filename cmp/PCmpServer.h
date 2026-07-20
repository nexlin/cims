#ifndef __CMP_SERVER_H__
#define __CMP_SERVER_H__

#include <string>
#include <map>
#include <vector>
#include <iostream>
#include <thread>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_set>
#include <sys/epoll.h>
//#include "pbase.h"
#include "pmodule.h"
#include "PRtpRelay.h"
#include "PRtpMulticast.h"
#include "PPttMemberPort.h"
#include "PMcpttGroup.h"
#include "SimpleJson.h"

class PCmpServer : public PModule {
public:
    PCmpServer(const std::string& name, const std::string& configFile = "cmp.conf");
    virtual ~PCmpServer();

    bool startServer();
    void stopServer();

    void runControlLoop(); // Main loop for UDP control

protected:
    void handlePacket(char* buf, int len, const std::string& ip, int port);
    void processAdd(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processRemove(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processModify(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processAlive(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);

    // Group Management
    void processAddGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processModifyGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processRemoveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processJoinGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processLeaveGroup(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    // PTT_FLOOR_TIER {group_id, session_id, tier} — 긴급/임박 floor tier 런타임 갱신(업그레이드/취소)
    void processSetFloorTier(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processStats(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);

    int sendResponse(const std::string& ip, int port, const std::string& msg,
                     const char* caller = "", const char* callee = "");
    // v2 응답 빌더 (envelope: docs/api/cmp_media_api.md). sesid/svc 빈 값이면 hdr 에서 생략.
    int sendOk(const std::string& ip, int port, int transId, const std::string& cmd,
               const std::string& sesid, const std::string& svc,
               const SimpleJson::JsonNode* body = NULL,
               const char* caller = "", const char* callee = "");
    int sendErr(const std::string& ip, int port, int transId, const std::string& cmd,
                const std::string& sesid, const std::string& svc,
                const char* code, const char* reason);
    // HEARTBEAT/STATS 공통 자원 요약 (호출측이 _mutex 보유)
    SimpleJson::JsonNode buildResourceSummary();

    // Resource Management
    void loadConfig();
    void initResourcePool();
    void initPttResourcePool();
    void initPttMemberPool();
    PRtpRelay* allocResource(std::string& rtpIp, int& rtpPort, int& videoPort);
    void freeResource(PRtpRelay* rtp);
    PRtpMulticast* allocPttResource(std::string& rtpIp, int& floorPort);
    void freePttResource(PRtpMulticast* ptt);
    // 멤버 전용 포트 유닛 — (groupId, sessionId) 키로 할당/재사용 (멱등). 호출자가 _mutex 보유.
    PPttMemberPort* ensureMemberUnit(const std::string& groupId, const std::string& sessionId, PMcpttGroup* group);
    void freeMemberUnit(const std::string& groupId, const std::string& sessionId);
    void freeGroupMemberUnits(const std::string& groupId);

private:
    int _udpFd;
    bool _running;
    
    std::map<std::string, PRtpRelay*> _sessions;
    std::map<std::string, PMcpttGroup*> _groups;
    std::map<std::string, std::string> _groupSubId;  // groupId → subid(session_seq)
    std::map<std::string, std::string> _sesidMap;    // sessionId 또는 groupId → sesid (flow 상관용)
    std::map<std::string, std::string> _serviceMap;  // sessionId 또는 groupId → service (payload 계승)
    // 미협상 소스 드롭 전역 누적 — 자원 해제 시 이월 (STATS rtp_src_drop 단조 증가 보장)
    long long _srcDropTotal = 0;
    PMutex _mutex;

    // sesid 발행 유틸: {caller}::cmp::{us_ts}::{counter}
    static std::string issueSesid(const std::string& caller);

    // CMP flow 로그 (통합 디렉터리: {ServiceLogDir}/YYYY/MM/DD/HH/cmp_01_{service}.flow.jsonl)
    std::string _serviceLogDir;
    std::string _systemId;      // 파일명용 (cmp_01)
    std::string _nodeName;      // flow node 필드용 (cmp)
    // 5분 버킷 + open-per-write (구 시간당 persistent handle → .nfs 고아·데이터유실·대용량검색 해소; CSP SipMessageLogger 와 동일)
    std::string _currentBucketKey;   // {hourDir}/{mm5} — 버킷 회전 감지
    std::string _currentFlowHourDir;
    std::string _currentMsgHourDir;
    int _msgSeq;          // 현재 버킷 _csp.msg 줄 수 (버킷 전환 시 -1 → 다음 write 가 lazy 계수)
    int _lastRxSeq;       // 현재 요청의 원문 seq (logFlow에서 사용)

    // HEARTBEAT 로그 샘플링 (3초 주기 노이즈 억제 — N 회당 1회만 msg/flow 기록).
    //   단일 control 스레드 전제의 per-packet 플래그 (handlePacket 이 설정, sendResponse 가 참조).
    static const int kHbLogSampleN = 100;  // 3s × 100 ≈ 5분당 1건
    unsigned long _hbCount = 0;
    bool _hbLogSuppress = false;

    void logFlow(const std::string& key, const char* from, const char* to,
                 const char* proto, const char* label, const char* detail = "",
                 const char* txId = "", const char* service = "",
                 const char* sesid = "", const char* subid = "",
                 int seq = 0, const char* iface = "",
                 const char* caller = "", const char* callee = "");
    int writeMsgLine(const char* ts, const char* dir, const char* peer, const char* proto, const char* msg,
                     const char* caller = "", const char* callee = "");
    void ensureBucket();             // 디렉터리 보장 + 버킷 전환 시 seq 리셋 (핸들 미유지)
    std::string bucketSuffix();      // (tm_min/5)*5 → "00".."55"
    std::string flowFilePath();      // {hourDir}/{systemId}.flow.{mm5}.jsonl
    std::string msgFilePath();       // {hourDir}/{systemId}_csp.msg.{mm5}.jsonl
    static int countFileLines(const std::string& path);
    std::string getFlowHourDir();
    std::string getMsgHourDir();
    static std::string getTimestamp();
    static bool mkdirP(const std::string& path);

    // ── 비동기 배치 로그 writer (CSP SipMessageLogger 와 동일 패턴) ──────────────
    //   생산자(writeMsgLine/logFlow/writeLeakReclaim)는 _logMtx 보유 중 한 줄을
    //   포맷·seq 부여까지만 하고 enqueueLine 으로 큐에 적재 후 즉시 반환(NFS I/O 없음).
    //   단일 control 스레드가 NFS open-per-write 동기 I/O 로 막히던 HOL 블로킹 제거.
    //   단일 writer 스레드가 flush 주기/큐 임계마다 큐를 비워 파일경로별로 라인을 합쳐
    //   경로당 1회 open→append→close(open-per-write → open-per-batch). 단일 writer FIFO 라
    //   파일 줄순서 = enqueue(=seq) 순서 정합 유지.
    struct LogItem {
        std::string path;
        std::string line;
    };
    std::mutex _logMtx;                       // producer 상태(_msgSeq/bucket/dir) + 포맷+enqueue 직렬화
    std::deque<LogItem> _logQueue;            // 기록 대기 (파일경로 + 한 줄)
    std::mutex _logQMtx;                       // _logQueue 보호 (항상 _logMtx → _logQMtx 순서)
    std::condition_variable _logQCv;
    std::thread _logWriterThread;
    std::atomic<bool> _logWriterRunning{false};
    std::atomic<unsigned long> _logDropped{0};  // 큐 상한 초과로 버려진 줄 수
    void enqueueLine(const std::string& path, std::string&& line);
    void logWriterLoop();
    void flushLogBatch(std::deque<LogItem>& batch);
    void startLogWriter();
    void stopLogWriter();

    // msg_log body
    std::string _msgLogDir;

    // VoIP Resource Pool
    int _rtpStartPort;
    int _rtpPoolSize;
    std::string _rtpIp;

    // PTT Resource Pool
    int _pttRtpStartPort;     // 멤버 유닛 audio RTP 대역 시작 (stride 2)
    int _pttRtpPoolSize;      // 그룹(floor) 풀 크기
    int _pttFloorStartPort;   // 그룹 floor 대역 시작 (stride 2)
    int _pttVideoStartPort;   // 멤버 유닛 video RTP 대역 시작 (stride 2)
    int _pttMemberPoolSize;   // 멤버 유닛 풀 크기 (동시 참가 멤버 수)

    // Server Config
    std::string _serverIp;
    int _serverPort;

    // DTMF PTT Config
    bool _dtmfPttEnable;
    std::string _dtmfPushDigit;
    std::string _dtmfReleaseDigit;

    std::string _configFile;

    // VoIP 리소스 (PRtpRelay, 4포트 블록)
    std::vector<PRtpRelay*> _resourcePool;
    std::vector<PRtpRelay*> _freeResources;

    // PTT 그룹 리소스 (PRtpMulticast, 그룹 공유 floor control)
    std::vector<PRtpMulticast*> _pttPool;
    std::vector<PRtpMulticast*> _freePttResources;

    // PTT 멤버 전용 포트 유닛 (PPttMemberPort, audio+video RTP)
    std::vector<PPttMemberPort*> _pttMemberPool;
    std::vector<PPttMemberPort*> _freePttMembers;
    std::map<std::string, PPttMemberPort*> _memberUnits;  // "groupId|sessionId" → unit

    // Worker config
    int _rtpWorkerCount;

    // ── RTP epoll 리액터 ──────────────────────────────────────────────
    // 구: RtpWorker 4개가 1ms period 로 풀 전체(논블로킹 소켓 ~550개)를 1000Hz busy-poll
    //     → 진행 호 0 에도 코어 1개 상시 점유. 이를 이벤트 구동(epoll)으로 교체.
    // 풀 소켓 fd 는 init 때 워커별 epoll 에 1회 등록(소켓은 프로세스 내내 유지, alloc/free 무관).
    // 트래픽 없으면 epoll_wait 블록 → idle CPU ≈ 0. 패킷 도착 시에만 해당 relay 의 proc() 호출.
    struct RtpReactor {
        int epfd = -1;
        std::thread thread;
    };
    std::vector<RtpReactor> _reactors;
    std::atomic<bool> _reactorRunning{false};
    void reactorLoop(int widx);
    void epollAddHandler(int widx, PHandler* h, const std::vector<int>& fds);

    // Recording config
    bool _recordEnable;
    std::string _recordDir;
    int _segmentIntervalSec;  // VoLTE 세그먼트 회전 간격 (초, 기본 60)

    // Session timeout (seconds, 0=disabled)
    int _sessionTimeout;
    // 고아(RTP 무수신) relay 회수 시간(초) — setup 실패/실패호 누수 방지. 활성/홀드 호는 _sessionTimeout 적용.
    int _orphanReclaimSec;
    // PTT floor 무활동 자동 회수 시간(초, 0=비활성) — owner 가 RELEASE 없이 RTP 송출을 멈추면
    //   (예: 검증 마지막 발언자가 RELEASE 없이 호 종료) 마지막 RTP 후 이 시간이 지나면 floor 강제 해제.
    //   무수신 시에만 발동(실발언 중엔 RTP 가 갱신하므로 끊기지 않음). 기본 10초.
    int _floorIdleSec;

    // 누수 회수(leak reclaim) 관측 — sweeper 가 고아 relay 를 회수한 누적 카운터(STATS 노출) +
    //   회수 세션 상세를 {ServiceLogDir}/leak_reclaim/YYYY/MM/DD/reclaim.jsonl 에 기록(콘솔 조회용).
    //   reason=orphan_no_rtp(setup 실패/무RTP, _orphanReclaimSec 회수) | hold_timeout(RTP 받았으나 owner 가
    //   REMOVE 미발행 = CSP crash/BYE 누락, _sessionTimeout 회수). RtpMap fix 후 이 카운터 증가=새 버그 신호.
    long _leakReclaimTotal;
    long _leakReclaimOrphan;
    long _leakReclaimHold;
    void writeLeakReclaim( const std::string& sessionId, const std::string& sesid, const std::string& service,
                           const char* reason, int heldSec );

    // Flow 로깅 enable flags (cmp.json: Logging.Flow.{floor,dtmf,rtcp})
    bool _logFlowFloor;   // MCPTT floor control RTCP APP 메시지 기록 여부
    bool _logFlowDtmf;    // RFC 2833/4733 DTMF 이벤트 기록 여부
    bool _logFlowRtcp;    // 일반 RTCP(SR/RR/SDES/BYE) 기록 여부
public:
    bool getLogFlowFloor() const { return _logFlowFloor; }
    bool getLogFlowDtmf()  const { return _logFlowDtmf; }
    bool getLogFlowRtcp()  const { return _logFlowRtcp; }

    // Timeout check thread
    std::thread _timeoutThread;
    void timeoutLoop();
};

#endif // __CMP_SERVER_H__
