// Worker — cims-tester-worker 본체: 풀(가상 단말 집합)·run 실행기·관측 스트림 (test_instrument.md §3·§6.1).
//
//  · 풀(POST /pools): 신원마다 libcsim SimSession 하나(스택 미기동). kind=ue 만 (peer/real-ue 는 C·F 단계).
//  · run(POST /runs): 컴파일된 단계 목록을 셋으로 나눈다 —
//      prelude  = 앞쪽의 register(+wait) 단계: 역할 슬라이스의 단말 전부를 한 번 등록(간격 두고 Start)
//      body     = 나머지: 시나리오 인스턴스 하나가 실행하는 단위. 인스턴스는 rate_saps 로 발생(SApS),
//                 역할마다 free 단말을 하나씩 잡고 단계를 차례로 실행한 뒤 단말을 돌려준다.
//      epilogue = 끝의 deregister 단계: run 종료 시 단말 정지(REGISTER Expires=0)
//    단계 실행은 스케줄러 스레드(10 ms 틱) 하나가 한다. psip 콜백(ICsimObserver)은 이벤트를 큐에만 넣는다.
//  · 지표: Metrics 1초 버킷 → StreamClient(TCP JSONL). 실패 개별 건은 event 레코드.
#ifndef _CIMS_TESTER_WORKER_H_
#define _CIMS_TESTER_WORKER_H_

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "CsimObserver.h"
#include "HttpServer.h"
#include "Json.h"
#include "Metrics.h"
#include "StreamClient.h"

class SimSession;

struct WorkerConfig {
    std::string name = "worker";
    std::string bindIp = "0.0.0.0";
    int port = 7100;
    std::string localIp;            // SIP/RTP 로컬 IP — 비면 자동 탐지
    int sipPortBase = 0;            // 0 = OS 자동. >0 이면 base + 2*idx (IPsec/TLS 고정 포트가 필요할 때)
    std::string mediaFile;          // AMR-WB raw 프레임 파일 — 비면 합성 PCMU
    std::string videoFile;          // H.264 Annex B — 비면 비디오 없음
    int registerIntervalMs = 20;    // prelude 등록 간격
    int registerTimeoutS = 60;      // prelude 전원 등록 대기 상한
    int inviteTimeoutMs = 32000;    // INVITE 최종 응답 대기(Timer B 상당)
    int byeTimeoutMs = 8000;
    int maxEndpointsPerCore = 200;  // 용량 선언(cspsim 실측 기준)
    double maxSapsPerCore = 10;
    std::string version = "0.1.0";
};

struct Identity {
    std::string user, domain, ha1, password, display, authScheme, akaK, akaOpc;
};

struct Instance;

struct Endpoint {
    int idx = 0;
    std::string pool;
    Identity id;
    SimSession* s = nullptr;
    bool started = false;           // Start() 호출됨(등록 진행/완료)
    bool registered = false;
    Instance* inst = nullptr;       // 지금 이 단말을 쓰는 인스턴스
    bool pendingInvite = false;     // deferred 착신 대기 중
    bool inCall = false;
    long long tStartCallMs = 0;     // 발신 시각(SRD 기점 — SimSession 도 갖지만 인스턴스 판정용)
};

struct Pool {
    std::string name;
    std::string kind;
    std::string transport = "udp";
    std::string srtp = "off";
    std::string targetIp;
    int targetPort = 5060;
    std::vector<std::unique_ptr<Endpoint>> eps;
};

struct CompiledStep {
    int idx = 0;
    std::string step;
    std::vector<std::string> who;
    std::string from, to;
    int afterMs = 0;
    int seconds = 0;
    std::string group, payload;
    Json media, expect;
};

struct RunSpec {
    std::string runId, scenarioId, stream;
    std::map<std::string, std::string> roles;                 // 역할 → 풀
    std::map<std::string, std::pair<int, int>> slices;        // 역할 → [begin,end)
    std::vector<CompiledStep> steps;
    double rate = 0;
    long long maxInstances = 0;     // >0 이면 단발 — 그만큼 발생 뒤 인스턴스가 다 끝나면 run 을 닫는다
};

struct Instance {
    long long id = 0;
    std::map<std::string, Endpoint*> actors;   // 역할 → 단말
    size_t stepIdx = 0;                        // body 안 인덱스
    enum Phase { RUNNING, WAIT_EVENT, WAIT_TIME, DONE } phase = RUNNING;
    std::string awaitKind;                     // "callstart:<role>" 등
    long long waitUntilMs = 0, deadlineMs = 0, tStartMs = 0;
    enum Pending { NONE, ANSWER, REJECT } pending = NONE;
    int pendingCode = 0;
    std::string pendingRole;
    bool failed = false;
};

class Worker : public ICsimObserver {
public:
    explicit Worker(const WorkerConfig& cfg);
    ~Worker();

    bool start(std::string& err);
    void stop();

    // ICsimObserver — 스택 스레드: 큐에만 넣는다
    void OnRegister(SimSession* s, int iStatus, long long rrdMs) override;
    void OnIncomingCall(SimSession* s, const std::string& callId, const std::string& from) override;
    void OnCallStart(SimSession* s, const std::string& callId, long long srdMs) override;
    void OnCallEnd(SimSession* s, const std::string& callId, int iSipStatus) override;
    void OnByeResponse(SimSession* s, const std::string& callId, int iSipStatus, long long sddMs) override;

private:
    struct Event {
        enum Kind { REGISTER, INCOMING, CALLSTART, CALLEND, BYERESP } kind;
        SimSession* s;
        int status;
        long long ms;
        std::string callId;
    };

    WorkerConfig m_cfg;
    HttpServer m_http;
    Metrics m_metrics;
    StreamClient m_stream;
    std::mutex m_mtx;                       // 풀·run 상태 (HTTP 스레드 ↔ 스케줄러)
    std::map<std::string, std::unique_ptr<Pool>> m_pools;
    std::map<SimSession*, Endpoint*> m_bySession;
    std::unique_ptr<RunSpec> m_run;
    std::string m_runState;                 // idle|prelude|running|draining|stopped
    std::vector<CompiledStep> m_prelude, m_body, m_epilogue;
    std::vector<std::unique_ptr<Instance>> m_instances;
    std::map<std::string, std::vector<Endpoint*>> m_free;    // 역할 → free 단말
    std::atomic<double> m_rate{0};
    double m_credit = 0;
    long long m_nextInstanceId = 1;
    long long m_launched = 0;
    long long m_stopAtMs = 0;               // draining 종료 시각
    bool m_stopRequested = false;

    std::mutex m_evMtx;
    std::deque<Event> m_events;
    std::thread m_sched;
    std::atomic<bool> m_stop{false};
    std::atomic<double> m_cpuPct{0};
    long long m_cpuPrevTotal = 0, m_cpuPrevIdle = 0;

    // HTTP
    HttpResponse handle(const HttpRequest& req);
    HttpResponse health();
    HttpResponse poolCreate(const Json& doc);
    HttpResponse poolDelete(const std::string& name);
    HttpResponse runStart(const Json& doc);
    HttpResponse runRate(const std::string& id, const Json& doc);
    HttpResponse runStop(const std::string& id, const Json& doc);
    HttpResponse runGet(const std::string& id);

    // 스케줄러
    void schedLoop();
    void drainEvents();
    void onEvent(const Event& e);
    void tickPrelude(long long nowMs);
    void tickBody(long long nowMs);
    void launchInstance(long long nowMs);
    void execStep(Instance& in, long long nowMs);
    void advance(Instance& in, long long nowMs) { in.stepIdx++; in.phase = Instance::RUNNING; in.awaitKind.clear(); execStep(in, nowMs); }
    void finishInstance(Instance& in, bool failed, const std::string& why, long long nowMs);
    void releaseEndpoint(Endpoint* ep);
    void sampleRtp(Endpoint* ep);
    void endRun(const std::string& state);
    void emitEvent(const std::string& detail, Endpoint* ep, const std::string& step, int code, const std::string& callId = "");
    void sampleCpu();
    long long m_preludeDeadlineMs = 0;
    size_t m_preludeCursor = 0;
    std::vector<Endpoint*> m_preludeList;
    long long m_lastFlushS = 0;
    long long m_runStartedMs = 0;

    Endpoint* endpointOf(SimSession* s);
    bool startEndpoint(Endpoint* ep);
    static long long nowMs();
};

#endif
