// Worker — cims-tester-worker 본체: 풀(가상 단말·피어 신원 집합)·run 실행기·관측 스트림 (test_instrument.md §3·§6.1).
//
//  · 풀(POST /pools): kind=ue — 신원마다 libcsim SimSession 하나(스택 미기동). kind=peer — 풀당 CsimPeer 엔진 하나
//    (고정 수신점, 생성 즉시 bind) + 신원(E.164/DID)마다 Endpoint 하나(스택 없음, 엔진의 호를 Call-ID 로 귀속).
//    kind=real-ue — 신원마다 `cimsue-cli … drive` 자식 프로세스 하나(libcimsue/pjsua2 실스택, RealUe.h) — 풀 생성 때 스폰·ready 대기,
//    stdin 명령/stdout JSON 이벤트로 구동(등록·발신·응답·hold·DTMF·그룹콜·floor·픽업). 미디어는 실스택 것(송출 제어·rtp 모드 없음),
//    RTP 품질은 프로세스가 1 초마다 올리는 stats 를 표본으로 `real_*` 지표에 따로 남긴다(대량 가상 부하 아래 실단말 표본).
//  · run(POST /runs): 컴파일된 단계 목록을 셋으로 나눈다 —
//      prelude  = 앞쪽의 register(+wait) 단계: 역할 슬라이스의 단말 전부를 한 번 등록(간격 두고 Start). 피어 신원은 등록 없음
//      body     = 나머지: 시나리오 인스턴스 하나가 실행하는 단위. 인스턴스는 rate_saps 로 발생(SApS),
//                 역할마다 free 단말을 하나씩 잡고 단계를 차례로 실행한 뒤 단말을 돌려준다.
//      epilogue = 끝의 deregister 단계: run 종료 시 단말 정지(REGISTER Expires=0)
//    단계 실행은 스케줄러 스레드(10 ms 틱) 하나가 한다. psip 콜백(ICsimObserver·ICsimPeerObserver)은 이벤트를 큐에만 넣는다.
//  · PTT(service=ptt 풀): 단말 기동 = REGISTER → GMS/CMS SUBSCRIBE → 그룹 affiliation PUBLISH → conference SUBSCRIBE(MCPTT 기동 절차),
//    affiliation 200 까지가 prelude. body 에 group_call 이 있으면 인스턴스는 **그룹 단위**로 발생한다 — 멤버 전원이 free 인 그룹을
//    하나 잡아 단일 역할에 멤버를 하나씩, multi 역할에 나머지 전부를 배정한다(역할 창·free 목록 대신 그룹 목록).
//    floor 단계(floor_request/floor_release)는 TS 24.380 메시지 수신 시각(µs)으로 지연을 잰다.
//  · 단말 동작은 Endpoint 종류(UE 세션 / 피어 신원 / 실단말 프로세스)에 따라 ep* 헬퍼가 갈라 처리한다 — 단계 실행기는 종류를 모른다.
//  · 지표: Metrics 1초 버킷 → StreamClient(TCP JSONL). 실패 개별 건은 event 레코드.
#ifndef _CIMS_TESTER_WORKER_H_
#define _CIMS_TESTER_WORKER_H_

#include <atomic>
#include <deque>
#include <algorithm>
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
#include "RealUe.h"
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
    int groupReuseGapMs = 1000;     // 그룹 세션을 닫은 뒤 같은 그룹을 다시 잡기까지의 간격(멤버 leg 정리 BYE 가 끝날 시간)
    int floorTimeoutMs = 5000;      // floor 요청 결과(Granted/Deny/Queue)·해제 뒤 Idle 대기 상한
    int maxEndpointsPerCore = 200;  // 용량 선언(cspsim 실측 기준)
    double maxSapsPerCore = 10;
    // real-ue 풀(§3.3) — cimsue-cli 경로·동시 프로세스 상한·프로세스 stderr 로그 레벨/디렉터리·기동 대기·TLS 앵커
    std::string realUeCli;          // 비면 real-ue 풀 생성 거절(400 real_ue_cli_missing)
    int realUeMax = 8;              // 워커당 실단말 프로세스 상한(pjsua2 프로세스 하나 ≈ 수십 MB·스레드 여럿)
    int realUeLogLevel = 2;
    std::string realUeLogDir;       // 프로세스 stderr(pjsip 로그) — <모듈>/log/real-ue
    int realUeStartTimeoutS = 15;
    std::string realUeTlsCaFile;    // 풀 tls_verify 일 때 서버 인증서 앵커(PEM) — 비면 검증 없이 접속
    int realUeCmdTimeoutMs = 5000;  // 명령 동기 결과 대기
    std::string version = "0.1.0";
};

struct Identity {
    std::string user, domain, ha1, password, display, authScheme, akaK, akaOpc;
    std::string pttGroup;           // service=ptt — affiliation 대상 MCPTT 그룹 id
};

struct Instance;
struct Pool;

struct Endpoint {
    enum Kind { K_UE, K_PEER, K_REAL } kind = K_UE;   // 가상 단말(SimSession) / 피어 신원(엔진 호) / 실단말 프로세스(cimsue-cli)
    int idx = 0;
    std::string pool;
    Pool* poolRef = nullptr;
    Identity id;
    SimSession* s = nullptr;        // kind=ue — 가상 단말 스택
    std::string callId;             // kind=peer — 이 신원이 지금 붙어 있는 엔진 호(Call-ID)
    // kind=real-ue — 프로세스(풀이 소유)·현재 호(cli call id)·마지막 RTP 통계(프로세스가 1 초마다 올린다 — media_hold 표본 원천)
    RealUeProcess* real = nullptr;
    int realCall = -1;
    bool realRegFailed = false;     // register 명령 거절 또는 REGISTER 실패 응답
    bool realReinviteWait = false;  // hold/resume 명령을 냈다 — held/active 전이 = re-INVITE 200
    bool realHeld = false;
    bool realVideo = false;
    struct RealStats { unsigned long long rx = 0, tx = 0, lost = 0; long long jitterUs = 0; bool valid = false; } realStats;
    bool started = false;           // Start() 호출됨(등록 진행/완료)
    bool registered = false;
    Instance* inst = nullptr;       // 지금 이 단말을 쓰는 인스턴스
    bool pendingInvite = false;     // deferred 착신 대기 중
    long long holdUntilMs = 0;      // 피어 answer=delay — 이 시각 전엔 응답하지 않는다(오류 주입)
    bool inCall = false;
    long long tStartCallMs = 0;     // 발신 시각(SRD 기점 — SimSession 도 갖지만 인스턴스 판정용)
    bool outPending = false;        // 자기가 낸 INVITE(invite·consult·pickup·replaces·join)의 최종 응답 대기 중
    std::string outKind;            // 그 INVITE 의 종류 — 확립 때 <kind>_ok 로 센다(invite 는 sessions)
    // 전달·합류(volte_supplementary_services.md §5·§6, dispatch_center.md §5) — UE 세션만
    std::string consultCallId;      // attended transfer 상담 통화(두 번째 다이얼로그) Call-ID
    std::string consultTo;          // 상담 통화 상대 역할 — refer.to 와 같으면 attended(Refer-To 에 Replaces)
    bool inConsult = false;
    bool cancelExpected = false;    // 링잉 착신 leg 가 서버 CANCEL 로 끝나는 것이 정상(픽업·Replaces 가 가져간 호)
    bool joined = false;            // INVITE-Join(RFC 3911) 청취 leg — 표본 때 SSRC 2개(양 화자) 도달을 센다
    bool byeByStep = false;         // bye 단계가 낸 BYE — 그 응답만 completed(SCR 분자)로 센다(정리 BYE 는 세지 않는다)
    bool dlgWatching = false;       // dialog 구독(RFC 4235)을 냈다 — 반환 때 Expires 0
    bool evtWatching = false;       // 이벤트 패키지 구독을 냈다 — 반환 때 Expires 0
    // PTT
    bool affStarted = false;        // affiliation PUBLISH 를 냈다
    bool affiliated = false;
    bool affFailed = false;
    enum Floor { F_IDLE, F_REQUESTED, F_QUEUED, F_GRANTED, F_DENIED } floor = F_IDLE;
    long long tFloorReqUs = 0;      // Floor Request 송신 시각(µs)
    bool wasQueued = false;         // 이번 요청이 큐를 거쳤다 — Granted 가 오면 floor_queue_ms
    bool takenSeen = false;         // 이번 floor 요청의 Taken 을 이미 셌다
    bool idleSeen = false;          // 이번 floor 해제의 Idle(또는 다음 발언자의 Taken)을 봤다
    long long tReleasedMs = 0;      // 인스턴스에서 풀려난 시각 — 그룹 재사용 간격(정리 BYE 가 끝날 시간)
    bool talked = false;            // 표본 구간 안에 floor 를 가졌다 — 발언자는 수신 0 이 정상(무음 leg 로 세지 않는다)
    bool isPtt() const;
    bool ready() const { return registered && (!isPtt() || affiliated); }
    bool isPeer() const { return kind == K_PEER; }
    bool isReal() const { return kind == K_REAL; }
    bool isSim() const { return kind == K_UE; }     // SimSession 을 직접 만지는 곳은 이 검사 뒤에만
};

struct Pool {
    std::string name;
    std::string kind;
    std::string transport = "udp";
    std::string srtp = "off";
    std::string targetIp;           // ue: CSP 접속점 · peer: CSP 피어링 접속점(발신 다음 홉)
    int targetPort = 5060;
    std::string targetDomain;       // 대상 홈 도메인(target_csp.domain_volte) — 피어가 역할 없는 번호 리터럴을 부를 때 Request-URI host
    std::string profile;            // peer 프로파일
    std::string service = "volte";  // ue: 접속환경 클래스 volte|voip|ptt — ptt 면 MCPTT 단말(feature tag·기동 절차·floor)
    std::map<std::string, std::vector<Endpoint*>> groups;   // ptt: MCPTT 그룹 id → 이 풀의 멤버(신원 순)
    std::unique_ptr<CsimPeer> peer; // kind=peer 엔진
    int answerDelayMs = 0;          // peer answer=delay — 착신 INVITE 뒤 이 시간 동안 응답을 보류(시나리오 answer/progress 의 after_ms 와 합쳐 늦은 쪽)
    bool regStarted = false;        // peer 트렁크 REGISTER 를 냈다(풀 단위 — 계정 하나가 신원 범위를 대표)
    bool regFailed = false;
    std::map<std::string, Endpoint*> byUser;   // peer: 신원 user → Endpoint (착신 귀속)
    std::map<std::string, Endpoint*> byCall;   // peer: 활성 Call-ID → Endpoint
    std::vector<std::unique_ptr<RealUeProcess>> reals;   // real-ue: 신원 순 프로세스(Endpoint::real 이 가리킨다)
    std::vector<std::unique_ptr<Endpoint>> eps;
};

inline bool Endpoint::isPtt() const { return poolRef && poolRef->service == "ptt"; }

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
    std::vector<std::string> multiRoles;                      // 인스턴스마다 단말 여럿인 역할(그룹 세션의 나머지 멤버)
    std::vector<std::string> guestRoles;                      // 그룹 세션의 그룹 밖 역할(member: false — 청취 관제사·비멤버). 잡은 그룹의 멤버가 아닌 PTT 단말을 free 목록에서
    std::vector<CompiledStep> steps;
    std::map<std::string, std::map<std::string, std::string>> samples;   // 샘플 id → {코덱(amr-wb|pcmu|pcma): 절대 경로 | ""(합성)}
    double rate = 0;
    long long maxInstances = 0;     // >0 이면 단발 — 그만큼 발생 뒤 인스턴스가 다 끝나면 run 을 닫는다
};

struct Instance {
    long long id = 0;
    std::map<std::string, Endpoint*> actors;   // 역할 → 단말
    std::map<std::string, std::vector<Endpoint*>> multi;   // multi 역할 → 단말들(그룹 단위 인스턴스)
    std::string group;                         // 그룹 단위 인스턴스가 잡은 MCPTT 그룹
    long long tGroupCallMs = 0;                // group_call 발신 시각 — group_fanout_ms 기점
    long long tFloorReqUs = 0, tFloorRelUs = 0;   // 마지막 floor 요청/해제 시각(µs) — Taken/Idle 도달 지연 기점
    std::vector<Endpoint*> floorWait;          // floor 단계가 결과를 기다리는 단말
    std::string floorWant;                     // floor_request 기대 결과 granted|denied|queued|any
    std::vector<Endpoint*> byeWait;            // bye 단계가 응답을 기다리는 단말
    std::vector<Endpoint*> respWait;           // subscribe/publish 단계가 최종 응답을 기다리는 단말
    std::string groupTo;                       // group_call.to — 합류를 기다리는 multi 역할(빈 값 = 발신자 확립만)
    bool forkDial = false;                     // invite.to 가 역할 아닌 다이얼 번호(대표번호) — 인스턴스의 다른 UE 역할에 오는 착신은 포크 leg(승자 외 CANCEL 이 정상)
    std::string dialTarget;                    // 그 번호 — P-Called-Party-ID 대조
    long long tLastJoinMs = 0;                 // 마지막 멤버 합류(자동응답 200) 시각
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
    void OnAffiliate(SimSession* s, const std::string& group, int iSipStatus, long long affMs, bool bDeaffiliate) override;
    void OnSubscribeResponse(SimSession* s, const std::string& event, const std::string& resource, int iSipStatus) override;
    void OnDialogNotify(SimSession* s, const std::string& watched, const std::string& state, const std::string& callId) override;
    void OnCallAnswered(SimSession* s, const std::string& callId) override;
    void OnFloor(SimSession* s, int iSubtype, long long tUs) override;
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
    void OnPeerFaultReject(CsimPeer* p, const std::string& callId, const std::string& toUser, int iCode) override;

private:
    struct Event {
        enum Kind { REGISTER, INCOMING, CALLSTART, CALLEND, BYERESP, RING, PRACK, REINVITE, REINVITE_RESP, REFER_RESP,
                    AFFILIATE, ANSWERED, FLOOR, SUBSCRIBE_RESP, DLG_NOTIFY, FAULT_REJECT,
                    REAL_CALL, REAL_STATS, REAL_EXIT } kind;   // REAL_* = 실단말 프로세스 이벤트(onEvent 가 위의 종류로 다시 푼다)
        SimSession* s;
        CsimPeer* peer;
        int status;
        long long ms;
        std::string callId;
        std::string user;   // peer INCOMING: To user
        bool hasPai;        // INCOMING: P-Asserted-Identity 존재 · RING: SDP 있음(early media) · REINVITE: 상대 hold · AFFILIATE: 해제 명령의 응답
        bool prack = false; // RING: PRACK 을 냈다
        int q850 = 0;       // CALLEND: 상대 Reason Q.850 cause
        long long us = 0;   // FLOOR: 수신 시각(µs)
        std::string event;  // SUBSCRIBE_RESP: 이벤트 패키지(user = 자원) · DLG_NOTIFY: dialog 상태(user = 감시 대상, callId = 그 dialog) · REAL_CALL: 호 상태(user = dir in|out, hasPai = by_us, prack = mcptt)
        Endpoint* ep = nullptr;   // real-ue: 리더 스레드가 아는 단말(세션·엔진 없음)
        int rcall = -1;           // real-ue: cli call id
        unsigned long long rx = 0, tx = 0, lost = 0;   // real-ue: RTP 통계(statsValid 일 때)
        long long jit = 0;
        bool statsValid = false;
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
    bool buildRealUePool(Pool* pool, const Json& d, std::string& err);
    void onRealEvent(Endpoint* ep, const Json& ev);   // 리더 스레드 — Event 로 바꿔 큐에만 넣는다
    long long realProcesses() const;

    // 스케줄러
    void schedLoop();
    void drainEvents();
    void onEvent(const Event& e);
    void tickPrelude(long long nowMs);
    void tickBody(long long nowMs);
    void launchInstance(long long nowMs);
    bool pickGroup(std::map<std::string, Endpoint*>& actors, std::map<std::string, std::vector<Endpoint*>>& multi, std::string& group,
                   bool& anyEligible);
    std::vector<Endpoint*> endpointsOf(Instance& in);                          // 인스턴스의 단말 전부(단일 + multi)
    std::vector<Endpoint*> roleEndpoints(Instance& in, const std::string& role);
    void startPtt(Endpoint* ep);                                               // REGISTER 200 뒤 MCPTT 기동 절차
    void onFloor(Endpoint* ep, Instance* in, int subtype, long long us, long long nowMs);
    void checkGroupUp(Instance& in, long long nowMs);
    void checkFloorWait(Instance& in, long long nowMs);
    bool m_groupBound = false;              // body 에 group_call — 인스턴스를 그룹 단위로 발생
    std::string m_groupPool;                // 그룹 단위 인스턴스의 풀
    std::vector<std::string> m_singleRoles; // 그룹 멤버를 하나씩 받는 역할(발신자 먼저, 그다음 body 등장 순)
    std::vector<std::string> m_usedMulti;   // body 가 쓰는 multi 역할
    std::vector<std::string> m_guestRoles;  // 그룹 밖 역할(member: false) — pickGroup 밖에서 free 목록으로 배정(잡은 그룹의 비멤버만)
    bool isGuestRole(const std::string& r) const { return std::find(m_guestRoles.begin(), m_guestRoles.end(), r) != m_guestRoles.end(); }
    std::vector<std::string> m_groupNames;  // 그 풀의 그룹 id(순환 선택)
    size_t m_groupCursor = 0;
    static long long nowUs();
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
    bool epStartCall(Endpoint* from, Endpoint* to, const Json& media, const std::string& dial = "");   // to 없으면 dial(번호 리터럴)을 부른다
    bool epHasCall(Endpoint* ep);
    int epAnswer(Endpoint* ep);                 // 0=성공, 그 외 SIP 코드(488 코덱 불일치 등)
    int epProgress(Endpoint* ep);               // 183 early media — 피어 신원만(UE 는 481)
    bool epReject(Endpoint* ep, int code, int cause = 0);
    bool epBye(Endpoint* ep, int cause = 0);
    bool epHold(Endpoint* ep, bool hold);
    bool epRefer(Endpoint* from, Endpoint* to, bool attended);   // attended = 전달자가 to 와 상담 통화 중(Refer-To 에 Replaces)
    bool epDtmf(Endpoint* ep, const std::string& digits);
    void epSetMediaMode(Endpoint* ep, int mode);
    void epSetVideo(Instance& in, Endpoint* from, bool want);   // invite/group_call 의 media.video — 오퍼 m=video 유무(워커 Media.VideoFile 전제)
    bool epMediaSend(Endpoint* ep, const CompiledStep& st);   // false = SDP 교환 전(RTP 미기동)
    bool epMediaStop(Endpoint* ep);
    bool epGroupCall(Endpoint* from, const std::string& group, bool listen, const Json& media);   // MCPTT 그룹 INVITE(발신자) — false = 스택 거절
    bool epFloorRequest(Endpoint* ep);
    void epFloorRelease(Endpoint* ep, bool held);
    bool epPickup(Endpoint* from, const std::string& code, const std::string& number);   // real-ue 픽업(피처코드 다이얼)
    bool epUnregister(Endpoint* ep);            // epilogue deregister — UE 스택 정지 / 실단말 unregister
    Json realRequest(Endpoint* ep, const std::string& cmd);
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
    std::string pendingCallerRole(Instance& in);       // 자기 INVITE 의 최종 응답을 기다리는 역할(상담 호 포함) — answer/reject 완료·in-dialog 단계의 확립 대기
    void markCancelExpected(Instance& in);             // 픽업·Replaces 직전 — 링잉 중인 착신 leg 들은 서버 CANCEL 로 끝나는 것이 정상
    bool epSubscribe(Endpoint* ep, const std::string& event, const std::string& resource);
    void epUnsubscribe(Endpoint* ep);
    void epClearCall(Endpoint* ep);
    std::string roleOf(Instance* in, Endpoint* ep);
    static long long nowMs();
};

#endif
