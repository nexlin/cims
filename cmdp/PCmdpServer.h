/*
 * PCmdpServer — CIMS MCData Media Plane 서버 본체.
 *
 * cmp(PCmpServer) 골격을 따른다:
 *  - UDP JSON 제어채널 — cmp 와 동일한 envelope v2 {hdr,payload}
 *    (docs/api/cmp_media_api.md §2, 명령 정본은 mcdata_messaging.md §4.7)
 *  - epoll 리액터 (단, cmp 와 달리 TCP 동적 fd: accept/close 시 등록/해제 + 지연 삭제)
 *  - 비동기 배치 jsonl 로거 (5분 버킷, open-per-batch — NFS HOL 회피)
 *  - deployment overlay config (install_path/config.json dot-path merge)
 *  - 스위퍼 (orphan/idle 세션 회수 + 이벤트 재전송)
 *
 * cmp 와 다른 점 — 비동기 이벤트 채널(type:"event")이 실구현: 수신 완료(MSRP_MSG_RECEIVED)·
 * 전송 결과(MSRP_SEND_RESULT)·세션 중단(MSRP_ABORTED)을 CSP 로 push 하고, 동일 trans_id 의
 * type:"response" ack 가 확인될 때까지 재전송한다 (1s × 5회).
 */

#ifndef __CMDP_SERVER_H__
#define __CMDP_SERVER_H__

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/epoll.h>
#include "pmodule.h"
#include "PFdStore.h"
#include "PMsrpConnection.h"
#include "PMsrpSession.h"
#include "SimpleJson.h"

class PCmdpServer : public PModule {
public:
    PCmdpServer(const std::string& name, const std::string& configFile = "cmdp.json");
    virtual ~PCmdpServer();

    bool startServer();
    void stopServer();

    void runControlLoop();  // UDP 제어채널 수신 루프

    // ── PMsrpConnection 콜백 (리액터 스레드) ───────────────────────────
    /** MSRP 프레임 1건 처리. false 반환 시 연결 종료 */
    bool onMsrpFrame(PMsrpConnection* conn, const PMsrpMessage& msg);
    /** 연결 종료 통지 — epoll DEL + 세션 분리 + tombstone 적재 */
    void onConnectionClosed(PMsrpConnection* conn);

protected:
    void handlePacket(char* buf, int len, const std::string& ip, int port);
    // MSRP_ADD (payload.mode="recv"|"send") — sesid 는 hdr 계승 값
    void processAddRecvSession(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId,
                               const std::string& sesid);
    void processAddSendSession(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId,
                               const std::string& sesid);
    void processModify(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId,
                       const std::string& sesid);
    void processRemoveSession(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId,
                              const std::string& sesid);
    void processAlive(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);
    void processStats(const SimpleJson::JsonNode& payload, const std::string& ip, int port, int transId);

    // v2 응답 빌더 (envelope: cmp_media_api.md §2). sesid 빈 값이면 hdr 에서 생략.
    int sendOk(const std::string& ip, int port, int transId, const std::string& cmd,
               const std::string& sesid, const SimpleJson::JsonNode* body = nullptr,
               const char* caller = "", const char* callee = "");
    int sendErr(const std::string& ip, int port, int transId, const std::string& cmd,
                const std::string& sesid, const char* code, const char* reason);

    int sendResponse(const std::string& ip, int port, const std::string& msg,
                     const char* caller = "", const char* callee = "");

    void loadConfig();

private:
    // ── 제어채널 ───────────────────────────────────────────────────────
    int _udpFd = -1;
    bool _running = false;
    std::string _serverIp = "0.0.0.0";
    int _serverPort = 9100;  // cmp(9000) 와 회피
    std::string _configFile;

    // ── MSRP 리스너/세션 ───────────────────────────────────────────────
    int _listenFd = -1;
    std::string _msrpIp;      // a=path 에 광고할 IP (UE 도달 가능해야 함)
    int _msrpPort = 2855;
    long long _maxMessageBytes = 10 * 1024 * 1024;  // 절대 상한 (세션별 max_size 와 min)
    int _sessionTimeout = 300;
    int _orphanReclaimSec = 60;
    int _msrpWorkerCount = 1;

    std::map<std::string, PMsrpSession*> _sessions;  // session_id → 세션
    std::vector<PMsrpConnection*> _connections;      // 활성 연결
    std::vector<PMsrpConnection*> _tombstones;       // 지연 삭제 대기 (리액터 배치 후 drain)
    PMutex _mutex;

    PFdStore _fdStore;

    std::string localMsrpPath(const std::string& sessionId) const;
    /** RECV 세트 완성 처리: 정규화→TLV 파싱→저장→MSG_RECEIVED (락 보유 중 호출) */
    void finishRecvSession(PMsrpSession* s);
    /** SEND 세션 스트리밍 개시/계속 (락 보유 중 호출) */
    void pumpSendSession(PMsrpSession* s);
    /** 세션 제거 공통 (락 보유 중) — 연결은 shutdown 만 (삭제는 리액터 tombstone) */
    void removeSessionLocked(const std::string& sessionId);

    // ── cmdp→CSP 이벤트 채널 (ack+재전송) ─────────────────────────────
    std::string _cspIp;  // 마지막 제어 datagram 소스 (CmdpClient 소켓)
    int _cspPort = 0;
    struct PendingEvent {
        std::string json;
        int attempts = 0;
        time_t nextAt = 0;
    };
    std::map<long, PendingEvent> _pendingEvents;  // event trans_id → 재전송 상태
    long _eventSeq = 0;  // 이벤트 trans_id 발행 카운터 — 기동 시 ms 시드 (재시작 ack 오매칭 방지)
    void emitEvent(const char* name, const SimpleJson::JsonNode& payload,
                   const std::string& sesid);  // 락 보유 중 — v2 type:"event" envelope
    void retransmitEvents();  // 스위퍼에서

    // ── MSRP epoll 리액터 (동적 fd) ────────────────────────────────────
    struct Reactor {
        int epfd = -1;
        std::thread thread;
    };
    std::vector<Reactor> _reactors;
    std::atomic<bool> _reactorRunning{false};
    void reactorLoop(int widx);
    void drainTombstones();  // 리액터 스레드 전용 (배치 처리 후)

    // listener 를 리액터 PHandler 로 등록하기 위한 래퍼
    class ListenHandler : public PHandler {
    public:
        explicit ListenHandler(PCmdpServer* s) : PHandler("msrp_listen"), _s(s) {}
        virtual bool proc() { _s->doAccept(); return true; }
        virtual bool proc(int, const std::string&, PEvent::Ptr) { return false; }
    private:
        PCmdpServer* _s;
    };
    ListenHandler _listenHandler{this};
    void doAccept();

    // ── 스위퍼 ─────────────────────────────────────────────────────────
    std::thread _timeoutThread;
    void timeoutLoop();

    // ── 서비스 로그 (cmp 와 동일 패턴: 5분 버킷 jsonl + 비동기 배치 writer) ──
    std::string _serviceLogDir;
    std::string _systemId = "cmdp_01";
    std::string _nodeName = "cmdp";
    std::string _currentBucketKey;
    std::string _currentHourDir;
    int _msgSeq = -1;
    int _lastRxSeq = 0;

    void logFlow(const std::string& key, const char* from, const char* to, const char* proto,
                 const char* label, const char* detail = "", const char* txId = "",
                 const char* sesid = "", int seq = 0,
                 const char* caller = "", const char* callee = "");
    int writeMsgLine(const char* dir, const char* peer, const char* proto, const char* msg,
                     const char* caller = "", const char* callee = "");
    void ensureBucket();
    std::string bucketSuffix();
    std::string flowFilePath();
    std::string msgFilePath();
    static int countFileLines(const std::string& path);
    static std::string getTimestamp();
    static bool mkdirP(const std::string& path);

    struct LogItem {
        std::string path;
        std::string line;
    };
    std::mutex _logMtx;
    std::deque<LogItem> _logQueue;
    std::mutex _logQMtx;
    std::condition_variable _logQCv;
    std::thread _logWriterThread;
    std::atomic<bool> _logWriterRunning{false};
    std::atomic<unsigned long> _logDropped{0};
    void enqueueLine(const std::string& path, std::string&& line);
    void logWriterLoop();
    void flushLogBatch(std::deque<LogItem>& batch);
    void startLogWriter();
    void stopLogWriter();

    // ── 통계 ───────────────────────────────────────────────────────────
    long _statRecvMessages = 0;
    long _statSendMessages = 0;
    long _statAborted = 0;
    long _statOrphanReclaim = 0;
};

#endif  // __CMDP_SERVER_H__
