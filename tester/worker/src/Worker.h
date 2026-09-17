// Worker — cims-tester-worker 본체: 풀(가상 단말·피어 신원 집합)·run 실행기·관측 스트림 (test_instrument.md §3·§6.1).
//
//  · 풀(POST /pools): kind=ue — 신원마다 libcsim SimSession 하나(스택 미기동). kind=peer — 풀당 CsimPeer 엔진 하나
//    (고정 수신점, 생성 즉시 bind) + 신원(E.164/DID)마다 Endpoint 하나(스택 없음, 엔진의 호를 Call-ID 로 귀속).
//    real-ue 는 F 단계.
//  · run(POST /runs): 컴파일된 단계 목록을 셋으로 나눈다 —
//      prelude  = 앞쪽의 register(+wait) 단계: 역할 슬라이스의 단말 전부를 한 번 등록(간격 두고 Start). 피어 신원은 등록 없음
//      body     = 나머지: 시나리오 인스턴스 하나가 실행하는 단위. 인스턴스는 rate_saps 로 발생(SApS),
//                 역할마다 free 단말을 하나씩 잡고 단계를 차례로 실행한 뒤 단말을 돌려준다.
//      epilogue = 끝의 deregister 단계: run 종료 시 단말 정지(REGISTER Expires=0)
//    단계 실행은 스케줄러 스레드(10 ms 틱) 하나가 한다. psip 콜백(ICsimObserver·ICsimPeerObserver)은 이벤트를 큐에만 넣는다.
//  · 단말 동작은 Endpoint 종류(UE 세션 / 피어 신원)에 따라 ep* 헬퍼가 갈라 처리한다 — 단계 실행기는 종류를 모른다.
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
#include "CsimPeer.h"
#include "HttpServer.h"
#include "Json.h"
#include "Metrics.h"
#include "SipCapture.h"
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
    std::string peerCertFile;       // 피어 풀 TLS 수신점 인증서(PEM) — 비면 TLS 피어 거절
    std::string sampleDir;          // 미디어 샘플 디렉터리 — media_send 의 sample 파일은 이 안의 상대 경로(§4 미디어 평면)
    std::string sipCapture = "failed"; // SIP 덤프 — off | failed(실패한 인스턴스의 호만 올린다) | all(모든 인스턴스 — 기능 시험용)
    int sipDumpMax = 500;           // run 하나에서 올리는 덤프(Call-ID) 상한
    int maxRtpStreams = 0;          // RTP 를 쓰는 단말 동시 상한(0 = 제한 없음) — 넘으면 인스턴스 발생을 건너뛴다(skipped)
    int dtmfDigitMs = 160;          // RFC 4733 이벤트 길이·간격 — dtmf 단계의 송신 완료 대기 계산
    int dtmfGapMs = 100;
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
struct Pool;

struct Endpoint {
    int idx = 0;
    std::string pool;
    Pool* poolRef = nullptr;
    Identity id;
    SimSession* s = nullptr;        // kind=ue — 가상 단말 스택
    std::string callId;             // kind=peer — 이 신원이 지금 붙어 있는 엔진 호(Call-ID)
    bool started = false;           // Start() 호출됨(등록 진행/완료)
    bool registered = false;
    Instance* inst = nullptr;       // 지금 이 단말을 쓰는 인스턴스
    bool pendingInvite = false;     // deferred 착신 대기 중
    bool inCall = false;
    long long tStartCallMs = 0;     // 발신 시각(SRD 기점 — SimSession 도 갖지만 인스턴스 판정용)
    bool isPeer() const { return s == nullptr; }
};

struct Pool {
    std::string name;
    std::string kind;
    std::string transport = "udp";
    std::string srtp = "off";
    std::string targetIp;           // ue: CSP 접속점 · peer: CSP 피어링 접속점(발신 다음 홉)
    int targetPort = 5060;
    std::string profile;            // peer 프로파일
    std::unique_ptr<CsimPeer> peer; // kind=peer 엔진
    bool regStarted = false;        // peer 트렁크 REGISTER 를 냈다(풀 단위 — 계정 하나가 신원 범위를 대표)
    bool regFailed = false;
    std::map<std::string, Endpoint*> byUser;   // peer: 신원 user → Endpoint (착신 귀속)
    std::map<std::string, Endpoint*> byCall;   // peer: 활성 Call-ID → Endpoint
    std::vector<std::unique_ptr<Endpoint>> eps;
};

struct CompiledStep {
    int idx = 0;
    std::string step;
    std::vector<std::string> who;
    std::string from, to;
    int afterMs = 0;
    int seconds = 0;
    int cause = 0;                  // bye/reject 의 Reason Q.850 cause (0 = 없음)
    std::string group, payload;
    std::string sample;             // media_send — 샘플 id(빈 값 = 풀 기본 원천)
    bool loop = true;               // media_send — false 면 샘플 끝에서 송출 정지
    Json media, expect;
};

struct RunSpec {
    std::string runId, scenarioId, stream;
    std::map<std::string, std::string> roles;                 // 역할 → 풀
    std::map<std::string, std::pair<int, int>> slices;        // 역할 → [begin,end)
    std::vector<CompiledStep> steps;
    std::map<std::string, std::map<std::string, std::string>> samples;   // 샘플 id → {코덱(amr-wb|pcmu|pcma): 절대 경로 | ""(합성)}
    double rate = 0;
    long long maxInstances = 0;     // >0 이면 단발 — 그만큼 발생 뒤 인스턴스가 다 끝나면 run 을 닫는다
};

struct Instance {
    long long id = 0;
    std::map<std::string, Endpoint*> actors;   // 역할 → 단말
    std::vector<std::string> callIds;          // 이 인스턴스에 속한 Call-ID — 끝날 때 SIP 덤프를 올리거나 버린다
    size_t stepIdx = 0;                        // body 안 인덱스
    enum Phase { RUNNING, WAIT_EVENT, WAIT_TIME, DONE } phase = RUNNING;
    std::string awaitKind;                     // "callstart:<role>" 등
    long long waitUntilMs = 0, deadlineMs = 0, tStartMs = 0;
    enum Pending { NONE, ANSWER, REJECT, PROGRESS, MEDIA } pending = NONE;
    int pendingCause = 0;
    int pendingCode = 0;
    std::string pendingRole;
    int rtpMode = 0;                           // CRtpThread::EMediaMode — invite 단계의 media.rtp (auto|none|explicit)
    bool mediaHeld = false;                    // media_hold 를 지났다 — bye 진입 시 RTP 표본
    bool progressTx = false;                   // 피어가 183+SDP 를 냈다 — 발신자 확립(200) 시점에 early media RTP 도달을 표본
    int expectCode = 0;                        // invite 단계 expect.code — 200 이 아니면 그 최종 응답이 성공 조건(ACL 403 등)
    bool failed = false;
};

class Worker : public ICsimObserver, public ICsimPeerObserver {
public:
    explicit Worker(const WorkerConfig& cfg);
    ~Worker();

    bool start(std::string& err);
    void stop();

    // ICsimObserver — 스택 스레드: 큐에만 넣는다
    void OnRegister(SimSession* s, int iStatus, long long rrdMs) override;
    void OnIncomingCall(SimSession* s, const std::string& callId, const std::string& from) override;
    void OnCallStart(SimSession* s, const std::string& callId, long long srdMs) override;
    void OnCallEnd(SimSession* s, const std::string& callId, int iSipStatus, int iQ850) override;
    void OnByeResponse(SimSession* s, const std::string& callId, int iSipStatus, long long sddMs) override;
    void OnCallRing(SimSession* s, const std::string& callId, int iSipStatus, bool bHasSdp, bool bPrackSent) override;
    void OnReInvite(SimSession* s, const std::string& callId, bool bRemoteHold) override;
    void OnReInviteResponse(SimSession* s, const std::string& callId, int iSipStatus) override;
    void OnReferResponse(SimSession* s, const std::string& callId, int iSipStatus) override;
    // ICsimPeerObserver — 스택 스레드
    void OnPeerIncoming(CsimPeer* p, const std::string& callId, const std::string& from, const std::string& to, bool hasPai) override;
    void OnPeerCallStart(CsimPeer* p, const std::string& callId, long long srdMs) override;
    void OnPeerCallEnd(CsimPeer* p, const std::string& callId, int iSipStatus, int iQ850) override;
    void OnPeerByeResponse(CsimPeer* p, const std::string& callId, int iSipStatus, long long sddMs) override;
    void OnPeerRing(CsimPeer* p, const std::string& callId, int iSipStatus, bool bHasSdp, bool bPrackSent) override;
    void OnPeerPrack(CsimPeer* p, const std::string& callId) override;
    void OnPeerReInvite(CsimPeer* p, const std::string& callId, bool bRemoteHold) override;
    void OnPeerReInviteResponse(CsimPeer* p, const std::string& callId, int iSipStatus) override;
    void OnPeerReferResponse(CsimPeer* p, const std::string& callId, int iSipStatus) override;
    void OnPeerRegister(CsimPeer* p, int iSipStatus, long long rrdMs) override;

private:
    struct Event {
        enum Kind { REGISTER, INCOMING, CALLSTART, CALLEND, BYERESP, RING, PRACK, REINVITE, REINVITE_RESP, REFER_RESP } kind;
        SimSession* s;
        CsimPeer* peer;
        int status;
        long long ms;
        std::string callId;
        std::string user;   // peer INCOMING: To user
        bool hasPai;        // INCOMING: P-Asserted-Identity 존재 · RING: SDP 있음(early media) · REINVITE: 상대 hold
        bool prack = false; // RING: PRACK 을 냈다
        int q850 = 0;       // CALLEND: 상대 Reason Q.850 cause
    };

    WorkerConfig m_cfg;
    HttpServer m_http;
    Metrics m_metrics;
    StreamClient m_stream;
    std::mutex m_mtx;                       // 풀·run 상태 (HTTP 스레드 ↔ 스케줄러)
    std::map<std::string, std::unique_ptr<Pool>> m_pools;
    std::map<SimSession*, Endpoint*> m_bySession;
    std::map<CsimPeer*, Pool*> m_byPeer;
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
    void destroyPool(Pool* pool);
    bool buildUePool(Pool* pool, const Json& d, std::string& err);
    bool buildPeerPool(Pool* pool, const Json& d, std::string& err);

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

    // 단말 동작 — Endpoint 종류(UE 세션 / 피어 신원)를 여기서만 가른다
    Endpoint* endpointOf(SimSession* s);
    Endpoint* endpointOfPeerCall(CsimPeer* p, const std::string& callId);
    bool startEndpoint(Endpoint* ep);
    bool epStartCall(Endpoint* from, Endpoint* to, const Json& media);
    bool epHasCall(Endpoint* ep);
    int epAnswer(Endpoint* ep);                 // 0=성공, 그 외 SIP 코드(488 코덱 불일치 등)
    int epProgress(Endpoint* ep);               // 183 early media — 피어 신원만(UE 는 481)
    bool epReject(Endpoint* ep, int code, int cause = 0);
    bool epBye(Endpoint* ep, int cause = 0);
    bool epHold(Endpoint* ep, bool hold);
    bool epRefer(Endpoint* from, Endpoint* to);
    bool epDtmf(Endpoint* ep, const std::string& digits);
    void epSetMediaMode(Endpoint* ep, int mode);
    bool epMediaSend(Endpoint* ep, const CompiledStep& st);   // false = SDP 교환 전(RTP 미기동)
    bool epMediaStop(Endpoint* ep);
    bool resolveSample(const std::string& file, std::string& out, std::string& err) const;
    long long rtpStreams() const;
    // SIP 덤프 — 인스턴스가 끝나고 조금 뒤(정리 BYE/487 까지 담기게) 올린다
    struct SipPending { long long dueMs; bool ship; long long instance; std::vector<std::string> callIds; };
    SipCapture m_sipCapture;
    std::deque<SipPending> m_sipPending;
    long long m_sipShipped = 0;
    void noteCallId(Instance* in, const std::string& callId);
    void flushSipPending(long long nowMs, bool all);
    int m_bodyRtpMode = 0;                  // body 첫 invite 의 media.rtp — 인스턴스 시작 모드·RTP 상한 판정
    void sampleDtmf(Endpoint* ep);
    std::string callerRole(Instance& in);
    void epClearCall(Endpoint* ep);
    std::string roleOf(Instance* in, Endpoint* ep);
    static long long nowMs();
};

#endif
