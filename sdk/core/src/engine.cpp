// libcimsue — Engine 구현 (ue_sdk.md §4.3 스레딩·수명 규칙)
//
//  - 모든 pjsua2 호출은 제어 스레드 `ue-ctl` 에서만 한다. libCreate 도 이 스레드에서 하므로 pjlib 의
//    "메인 스레드" 가 곧 ue-ctl 이다. 공개 명령은 runSync 로 ue-ctl 에 넘기고 결과를 받아 돌려준다.
//  - pjsua 콜백(pjsip 워커 스레드)은 상태 스냅샷을 갱신하고 이벤트를 큐에 넣기만 한다. 리스너는
//    이벤트 스레드 `ue-evt` 가 부른다 — 리스너 안에서 명령을 다시 불러도(ue-ctl 로 감) 교착 없음.
//  - pj::Account/pj::Call 은 엔진이 강참조 테이블로 보관하고 DISCONNECTED 뒤 ue-ctl 에서 해제한다.
//    콜백 안에서 자기 객체를 지우지 않는다.
//  - MCPTT 세션(그룹콜·사설콜)은 호마다 floor participant(별도 UDP 소켓)를 갖고, SDP 의 m=application 을
//    송신 SDP 에 주입·수신 SDP 에서 학습한다(android SipController/CimsCall 의 규칙 승계).
#include "cimsue/engine.h"

#include <pjsua2.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#if defined(__ANDROID__)
#include <android/native_window.h>                        // setVideoWindow 의 창 참조(ANativeWindow)
#endif

#include "account_map.h"
#include "floor/floor_participant.h"
#include "mcdata/msrp.h"
#include "mcdata/sds_codec.h"
#include "mcptt/mcptt_xml.h"
#include "quality/call_quality.h"

#define CIMSUE_VERSION "0.2.0"

namespace cimsue {

namespace {

/** 단일 워커 스레드 + 작업 큐. */
class Worker {
public:
    void start() {
        stop_ = false;
        th_ = std::thread([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
                    if (stop_ && q_.empty()) return;
                    job = std::move(q_.front());
                    q_.pop_front();
                }
                try { job(); } catch (...) {}
            }
        });
    }
    void post(std::function<void()> fn) {
        { std::lock_guard<std::mutex> lk(m_); q_.push_back(std::move(fn)); }
        cv_.notify_one();
    }
    template <typename F>
    auto runSync(F&& fn) -> decltype(fn()) {
        using R = decltype(fn());
        if (std::this_thread::get_id() == th_.get_id()) return fn();   // 재진입 — 직접 실행
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        auto fut = task->get_future();
        post([task] { (*task)(); });
        return fut.get();
    }
    void stop() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }

private:
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
};

Result fromError(const pj::Error& e) { return Result::fail((int)e.status, e.info(false)); }

/** SDP 에서 m=application 의 (ip, port). 섹션 c= 우선, 없으면 세션 c=. */
bool parseApplication(const std::string& sdp, std::string& ip, int& port) {
    size_t m = sdp.find("m=application ");
    if (m == std::string::npos) return false;
    port = std::atoi(sdp.c_str() + m + 14);
    if (port <= 0) return false;
    size_t next = sdp.find("\nm=", m + 1);
    std::string section = sdp.substr(m, next == std::string::npos ? std::string::npos : next - m);
    auto conn = [](const std::string& s) -> std::string {
        size_t c = s.find("c=IN IP4 ");
        if (c == std::string::npos) return std::string();
        size_t e = s.find_first_of("\r\n", c);
        return s.substr(c + 9, e == std::string::npos ? std::string::npos : e - c - 9);
    };
    ip = conn(section);
    if (ip.empty()) ip = conn(sdp);
    return !ip.empty();
}

/** 주입 섹션에 c= 라인 보장 — pjmedia_sdp_validate EMISSINGCONN 방지. */
std::string withConnLine(const std::string& whole, const std::string& extra) {
    std::string section = extra;
    while (!section.empty() && (section.back() == '\r' || section.back() == '\n')) section.pop_back();
    if (section.find("c=IN ") != std::string::npos) return section;
    size_t c = whole.find("c=IN IP4 ");
    if (c == std::string::npos) return section;
    size_t e = whole.find_first_of("\r\n", c);
    std::string cline = whole.substr(c, e == std::string::npos ? std::string::npos : e - c);
    size_t nl = section.find("\r\n");
    if (nl == std::string::npos) return section + "\r\n" + cline;
    return section.substr(0, nl + 2) + cline + section.substr(nl);
}

/** [whole] 의 [prefix] 미디어 섹션(다음 m= 전까지)을 [extra] 로 교체 — media_count 불변(med_prov_cnt 정합).
 *  prefix 가 없으면 끝에 덧붙인다. */
std::string replaceMediaSection(const std::string& whole, const std::string& prefix, const std::string& extra) {
    size_t s = whole.find(prefix);
    std::string body = withConnLine(whole, extra) + "\r\n";
    if (s == std::string::npos) {
        std::string w = whole;
        while (!w.empty() && (w.back() == '\r' || w.back() == '\n')) w.pop_back();
        return w + "\r\n" + body;
    }
    size_t e = whole.find("\nm=", s + 1);
    if (e == std::string::npos) return whole.substr(0, s) + body;
    return whole.substr(0, s) + body + whole.substr(e + 1);
}

uint32_t ssrcOf(const std::string& id) {
    uint32_t v = (uint32_t)(std::hash<std::string>{}(id) & 0xffffffffu);
    return v ? v : 1;
}

std::string sipBody(const std::string& whole) {
    size_t p = whole.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : whole.substr(p + 4);
}

class PjAccount;
class PjCall;
class PjLog;
struct MsrpLeg;

/** 이벤트 fan-out — 주 리스너(start 인자) 뒤에 관찰자들(addObserver — 구동 세션·계측 링크, ue_voice_quality.md §5.3)에게 같은
 *  이벤트를 같은 이벤트 스레드에서 차례로 준다. 재진입 락이라 콜백 안에서 관찰자를 빼도 된다. removeObserver 는 진행 중 전달이
 *  끝날 때까지 기다리므로, 돌아온 뒤에는 그 관찰자가 다시 불리지 않는다. */
class FanoutListener : public Listener {
public:
    Listener* primary = nullptr;
    void add(Listener* l) { std::lock_guard<std::recursive_mutex> lk(m_); if (l) obs_.push_back(l); }
    void remove(Listener* l) {
        std::lock_guard<std::recursive_mutex> lk(m_);
        for (auto it = obs_.begin(); it != obs_.end();) it = *it == l ? obs_.erase(it) : it + 1;
    }
    void onLog(int level, const std::string& msg) override { each([&](Listener* l) { l->onLog(level, msg); }); }
    void onRegState(const RegInfo& i) override { each([&](Listener* l) { l->onRegState(i); }); }
    void onIncomingCall(const CallInfo& i) override { each([&](Listener* l) { l->onIncomingCall(i); }); }
    void onCallState(const CallInfo& i) override { each([&](Listener* l) { l->onCallState(i); }); }
    void onCallMedia(const CallInfo& i) override { each([&](Listener* l) { l->onCallMedia(i); }); }
    void onFloor(const FloorEvent& e) override { each([&](Listener* l) { l->onFloor(e); }); }
    void onRoster(int a, const std::string& g, const std::vector<RosterEntry>& u, bool f) override {
        each([&](Listener* l) { l->onRoster(a, g, u, f); });
    }
    void onDialogInfo(const DialogInfo& d) override { each([&](Listener* l) { l->onDialogInfo(d); }); }
    void onMcpttCondition(const CallInfo& i, ConditionCause c) override { each([&](Listener* l) { l->onMcpttCondition(i, c); }); }
    void onEmergencyAlert(const EmergencyAlert& a) override { each([&](Listener* l) { l->onEmergencyAlert(a); }); }
    void onSds(const SdsMessage& m) override { each([&](Listener* l) { l->onSds(m); }); }
    void onRequestResult(const RequestResult& r) override { each([&](Listener* l) { l->onRequestResult(r); }); }
    void onMessage(int a, const std::string& f, const std::string& ct, const std::string& b) override {
        each([&](Listener* l) { l->onMessage(a, f, ct, b); });
    }
    void onEngineStopped() override { each([&](Listener* l) { l->onEngineStopped(); }); }

private:
    template <class F> void each(F fn) {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (primary) fn(primary);
        std::vector<Listener*> snap = obs_;              // 콜백 안의 remove 가 순회를 깨지 않게
        for (Listener* l : snap) {
            if (std::find(obs_.begin(), obs_.end(), l) != obs_.end()) fn(l);
        }
    }
    std::recursive_mutex m_;
    std::vector<Listener*> obs_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct Engine::Impl {
    EngineConfig cfg;
    Listener* listener = nullptr;     // = &fanout (start 가 연결) — emit 은 이것만 부른다
    FanoutListener fanout;
    std::atomic<bool> running{false};
    std::atomic<bool> captureOn{true};  // 캡처 게이트(setCaptureEnabled) — 기동마다 전이중
    // 장치 단 음량(setDeviceAudioLevels) — 앱이 한 번이라도 걸었을 때만 다시 건다(기본 = 엔진 기본값 그대로)
    bool levelsSet = false;
    float spkLevel = 1.f;
    double micTarget = kMicAgcTargetDbov;
    void* videoWindow = nullptr;        // setVideoWindow — 코어가 참조 하나를 소유(Android ANativeWindow). ue-ctl·콜백이 읽는다(videoM)
    std::mutex videoM;
    int camDev = -1;                    // 캡처 카메라 — -1 = 처음 쓸 때 전면 카메라로 정한다

    Worker ctl;                 // ue-ctl — pjsua2 전용
    Worker evt;                 // ue-evt — 리스너 전용
    std::unique_ptr<pj::Endpoint> ep;
    /** pjsua2 소유 — libInit 에 넘긴 뒤에는 Endpoint::libDestroy 가 delete 한다(여기서 지우면 이중 해제). */
    pj::LogWriter* logWriter = nullptr;

    // ue-ctl 에서만 접근
    std::map<int, std::unique_ptr<pj::Account>> accounts;
    std::map<int, AccountConfig> accountCfgs;
    std::map<int, std::unique_ptr<pj::Call>> calls;        // pjsua call id → Call
    /** 추가 재생 라우트(routeId ≥ 1) — 재생 전용 ExtraAudioDevice. 라우트 0 은 기본 재생 장치. */
    std::map<int, std::unique_ptr<pj::ExtraAudioDevice>> routes;
    std::unique_ptr<pj::AudioMediaPlayer> txPlayer;       // 송출 원천 = WAV 반복 재생(setTxSource) — 없으면 마이크
    int nextRouteId = 1;
    int nextAccountId = 0;
    std::atomic<int64_t> nextToken{1};

    // 스냅샷 — 콜백(pjsip 스레드)이 쓰고 조회(임의 스레드)가 읽는다
    std::mutex snapM;
    TlsPeerExpiry tlsPeer;                                 // 마지막 성공 TLS 핸드셰이크의 서버 인증서 만료(onTransportState)
    std::map<int, RegInfo> regInfos;
    std::map<int, CallInfo> callInfos;                     // 종료된 호도 잠시 보존(조회·최종 통계) — pruneFinished
    std::map<int, StreamStats> finalStats;                 // onStreamDestroyed 시점의 최종 RTP 통계
    std::map<int, CallQuality> finalQuality;               // 소멸한 오디오 스트림들의 누적 품질(quality::merge)
    /** 응답을 기다리는 affiliation PUBLISH — 내부 token 별. 412 초기 재발행은 새 내부 token 이고 앱에는 appToken 으로 알린다. */
    struct PendingPublish {
        int accountId = -1;
        std::string groupId;
        bool on = false;
        bool conditional = false;                          // SIP-If-Match 를 실었다(ETag 조건부 갱신)
        int64_t appToken = -1;                             // affiliate() 가 돌려준 token
    };
    std::map<int64_t, PendingPublish> publishPending;
    // media plane SDS(MSRP) 입출력 스레드 — 분리 실행, stop() 이 취소하고 모두 끝날 때까지 기다린다.
    std::mutex msrpM;
    std::condition_variable msrpCv;
    int msrpActive = 0;
    std::vector<std::weak_ptr<std::atomic<bool>>> msrpCancels;
    std::map<std::string, std::string> publishEtag;               // "accountId:group" → SIP-ETag
    static constexpr size_t kKeepFinished = 64;
    void pruneFinished() {                                 // snapM 잡은 상태에서 호출
        while (callInfos.size() > kKeepFinished) {
            auto it = callInfos.begin();
            for (; it != callInfos.end(); ++it) if (it->second.state == CallState::Disconnected) break;
            if (it == callInfos.end()) break;
            finalStats.erase(it->first);
            finalQuality.erase(it->first);
            callInfos.erase(it);
        }
    }
    static StreamStats fromPj(const pj::StreamStat& st) {
        StreamStats s;
        s.rxPackets = st.rtcp.rxStat.pkt; s.rxBytes = st.rtcp.rxStat.bytes;
        s.rxLoss = st.rtcp.rxStat.loss; s.rxDiscard = st.rtcp.rxStat.discard;
        s.txPackets = st.rtcp.txStat.pkt; s.txBytes = st.rtcp.txStat.bytes;
        s.rxJitterUs = (unsigned)st.rtcp.rxStat.jitterUsec.mean;
        s.valid = true;
        return s;
    }

    /** 오디오 스트림 idx 의 품질 — pjsua2 RTCP·지터버퍼 통계 + RTCP-XR(pjsua_call_get_stream_stat_xr) → quality::compute
     *  (ue_voice_quality.md §3). pjsua 락 아래에서 부르는 것은 안전하다(재진입 락). 실패하면 valid=false. */
    static CallQuality measure(pj::Call* c, unsigned idx) {
        quality::QualityInput in;
        pj::StreamStat st = c->getStreamStat(idx);
        try {
            pj::StreamInfo si = c->getStreamInfo(idx);
            in.codec = si.codecName;
            in.clockRate = si.codecClockRate;
        } catch (...) {}
        const pj::RtcpStreamStat& rx = st.rtcp.rxStat;
        const pj::RtcpStreamStat& tx = st.rtcp.txStat;
        in.rxPackets = rx.pkt; in.rxLost = rx.loss; in.rxDiscard = rx.discard;
        if (rx.jitterUsec.n > 0) { in.rxJitterMeanUs = rx.jitterUsec.mean; in.rxJitterMaxUs = rx.jitterUsec.max; }
        in.remoteReports = tx.updateCount;
        in.txPackets = tx.pkt; in.remoteLost = tx.loss;
        if (tx.jitterUsec.n > 0) { in.remoteJitterMeanUs = tx.jitterUsec.mean; in.remoteJitterMaxUs = tx.jitterUsec.max; }
        if (st.rtcp.rttUsec.n > 0) in.rttMeanUs = st.rtcp.rttUsec.mean;
        in.jbAvgDelayMs = st.jbuf.avgDelayMsec;
        in.startEpochMs = (int64_t)st.rtcp.start.sec * 1000 + st.rtcp.start.msec;
        pj_time_val now;
        pj_gettimeofday(&now);
        in.nowEpochMs = (int64_t)now.sec * 1000 + now.msec;
        pjmedia_rtcp_xr_stat xr;
        if (pjsua_call_get_stream_stat_xr(c->getId(), idx, &xr) == PJ_SUCCESS) {
            auto take = [](quality::XrMetrics& m, const pjmedia_rtcp_xr_stream_stat& d) {
                if (d.voip_mtc.update.sec == 0 && d.voip_mtc.update.msec == 0) return;   // 아직 계산·수신 전
                m.valid = true;
                m.lossRate = d.voip_mtc.loss_rate; m.discardRate = d.voip_mtc.discard_rate;
                m.burstDensity = d.voip_mtc.burst_den; m.gapDensity = d.voip_mtc.gap_den;
                m.burstMs = d.voip_mtc.burst_dur; m.gapMs = d.voip_mtc.gap_dur;
                m.signalDbm = d.voip_mtc.signal_lvl; m.noiseDbm = d.voip_mtc.noise_lvl;
            };
            take(in.xrRx, xr.rx);         // 자기 수신 — XR 보고를 만들 때 계산된 값
            take(in.xrRemote, xr.tx);     // 상대가 보낸 XR(내 스트림에 대한 보고)
            if (in.rttMeanUs < 0 && xr.rtt.n > 0) in.rttMeanUs = xr.rtt.mean;   // DLRR 로만 RTT 가 잡힌 경우
        }
        return quality::compute(in);
    }

    void emit(std::function<void()> fn) {
        if (listener) evt.post(std::move(fn));
    }
    void log(int level, const std::string& msg) {
        emit([this, level, msg] { listener->onLog(level, msg); });
    }
    CallInfo snapshotCall(int callId) {
        std::lock_guard<std::mutex> lk(snapM);
        auto it = callInfos.find(callId);
        return it == callInfos.end() ? CallInfo{} : it->second;
    }
    void updateCall(int callId, const std::function<void(CallInfo&)>& f, CallInfo* out = nullptr) {
        std::lock_guard<std::mutex> lk(snapM);
        CallInfo& ci = callInfos[callId];
        ci.callId = callId;
        f(ci);
        if (out) *out = ci;
    }

    /**
     * **새 호가 시작될 때 낡은 항목을 비운다.**
     *
     * `callInfos` 는 종료된 호도 64건까지 보존하는데(조회·최종 통계) pjsua 는 call id 를 순환
     * 재사용한다(`pjsua_call.c` `alloc_call_id`). 그래서 같은 id 를 다시 쓰면 새 호가 **옛 호의 필드를
     * 물려받는다** — 새 호 경로가 명시적으로 덮지 않는 `listenOnly`·`joinedDialog`·`isMcptt`·`groupId`·
     * `mcptt`·`muted`·`video` 가 그대로 남는다.
     *
     * 실제로 이것이 관제 앱에서 «감청을 끊은 뒤 걸려 온 전화가 감청 leg 으로 분류돼 응답 버튼이
     * 사라지는» 증상을 만들었다. 새 호는 언제나 깨끗한 항목에서 시작한다.
     */
    void resetCall(int callId) {
        std::lock_guard<std::mutex> lk(snapM);
        CallInfo fresh;
        fresh.callId = callId;
        callInfos[callId] = fresh;
        finalStats.erase(callId);
    }
    void applyCodecPolicy();
    PjCall* findCall(int callId);
    static bool rxOnlyLeg(PjCall* call);
    pj::AudioMedia* activeAudio(PjCall* call, unsigned* idxOut = nullptr);
    void wireMedia(PjCall* call, int callId);
    int64_t doSendRequest(int accountId, const std::string& method, const std::string& targetUri,
                       const std::string& contentType, const std::string& body,
                       const std::map<std::string, std::string>& headers, int64_t token);
    /** affiliation PUBLISH(TS 24.379 §9) — ue-ctl 에서. allowConditional 이면 저장된 ETag 로 SIP-If-Match(RFC 3903 §4.4). */
    int64_t sendAffiliation(int accountId, const std::string& groupId, bool on, int64_t token, int64_t appToken, bool allowConditional);
    /** 영상 미디어가 활성된 호 — 수신 창 결선 + (계정 videoAutoTransmit 면) 카메라 송신 개시. 영상 없는 빌드면 아무것도 안 한다. */
    void attachVideo(PjCall* call, int accountId);
    /** 기억한 장치 단 음량을 slot 0 에 다시 건다 — 게이트 전환·재오픈·미디어 결선 뒤(재오픈은 slot 0 레벨을 초기화한다). */
    void applyDeviceLevels();
    /** media plane SDS 입출력 스레드(분리 실행) — 결과는 onRequestResult(MSRP)·onSds 로, 끝나면 호를 정리한다. */
    void startMsrpSend(int callId, int accountId, const MsrpLeg& leg);
    void startMsrpRecv(int callId, int accountId, const MsrpLeg& leg);
    void runMsrpThread(std::shared_ptr<std::atomic<bool>> cancel, std::function<void()> body);
    /** 큰 그룹 SDS — MSRP 발신 INVITE(ue-ctl). */
    bool startMsrpInvite(int accountId, const std::string& groupId, int64_t token, const std::string& sigTlv, const std::string& payTlv);
    /** 캡처 카메라 목록(합성 장치 제외)과 전면 카메라. */
    std::vector<int> cameras();
    int frontCamera();
};

namespace {

/** MCPTT 세션(그룹콜/사설콜) 부속 상태 — PjCall 소유. */
struct McpttSession {
    std::string groupId;                 // bare id (그룹) 또는 상대 번호(사설콜)
    bool isPrivate = false;
    bool fullDuplex = false;             // mc_no_floor_ctrl — floor 없이 마이크 상시
    bool listenOnly = false;
    bool emergency = false, imminentPeril = false;   // 세션 조건 현재값 — 개시 옵션·착신 mcptt-info 로 시작(CallInfo.condition 의 원본)
    bool condMine = false;               // 이 단말이 올린 조건
    bool condPending = false;            // 상향·하향 re-INVITE 응답 대기 — 끝나면 prev* 로 되돌리거나(Denied) 확정(Confirmed)
    bool prevEmergency = false, prevImminent = false, prevMine = false;
    int condLastCode = 0;
    bool broadcast = false;              // 일제 통화 개시(<broadcast-ind>) — 이 단말이 개시자
    bool micOpen = false;                // floor Granted 로 열림
    bool implicitAwaitAnswer = false;    // 개시 INVITE 가 암묵적 발언 요청 — 200 OK answer 의 fmtp 로 판정(TS 24.380 §14.3.4·§14.3.5)
    std::string pendingAppSdp;           // 송신 SDP 에 주입할 m=application 섹션
    std::unique_ptr<floor::Participant> floor;
    bool remoteLearned = false;
};

/** media plane SDS(MSRP, TS 24.282 §9.2.3) 호 — 앱 호 목록에 나오지 않는다(CallInfo 없음). 발신 = 큰 그룹 SDS, 수신 = 서버발 배포. */
struct MsrpLeg {
    bool outgoing = true;
    std::string sessionId = msrp::newSessionId();
    std::string localPath;                 // 첫 SDP 를 만들 때 c= 주소로 정한다 — 주입 섹션·MSRP From-Path 가 같은 값
    std::string serverPath;                // cmdp a=path — 발신 = 200 OK answer, 수신 = offer
    bool started = false;                  // 입출력 스레드를 띄웠다
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    // 발신
    int64_t token = -1;
    std::string signallingTlv, payloadTlv;
    // 수신
    std::string fromUri, groupUri;
};

/** Endpoint — transport 상태 콜백으로 TLS 서버 인증서 만료를 관측한다(sip_tls_signaling.md §8.6.2). */
class PjEndpoint : public pj::Endpoint {
public:
    explicit PjEndpoint(Engine::Impl* o) : o_(o) {}
    void onTransportState(const pj::OnTransportStateParam& prm) override {
        if (prm.state != PJSIP_TP_STATE_CONNECTED || prm.tlsInfo.isEmpty()) return;
        const pj::SslCertInfo& rc = prm.tlsInfo.remoteCertInfo;
        if (rc.isEmpty() || rc.validityEnd.sec <= 0) return;
        TlsPeerExpiry e;
        e.valid = true;
        e.notAfterEpoch = (int64_t)rc.validityEnd.sec;
        e.observedEpoch = (int64_t)std::time(nullptr);
        e.subject = rc.subjectInfo.empty() ? rc.subjectCn : rc.subjectInfo;
        e.remote = prm.tlsInfo.remoteAddr;
        std::lock_guard<std::mutex> lk(o_->snapM);
        o_->tlsPeer = e;
    }
private:
    Engine::Impl* o_;
};

class PjLog : public pj::LogWriter {
public:
    explicit PjLog(Engine::Impl* o) : o_(o) {}
    void write(const pj::LogEntry& e) override {
        std::string m = e.msg;
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
        o_->log(e.level, m);
    }
private:
    Engine::Impl* o_;
};

class PjCall : public pj::Call {
public:
    PjCall(Engine::Impl* o, pj::Account& acc, int accountId, int callId = PJSUA_INVALID_ID)
        : pj::Call(acc, callId), o_(o), accountId_(accountId) {}

    std::unique_ptr<McpttSession> mcptt;
    std::unique_ptr<MsrpLeg> msrp;       // media plane SDS 호 — 앱에 나오지 않는다
    bool recvOnly = false;               // 감청 Join 등 청취 전용 평문 leg (a=recvonly, 마이크 없음)

    /**
     * 이 호가 **낡은 스냅샷을 아직 비우지 않았다**.
     *
     * pjsua 는 call id 를 순환 재사용하므로(`alloc_call_id`) 새 호는 같은 id 의 옛 항목을 물려받는다.
     * `PjCall` 은 호마다 새로 만들어지므로, 그 객체가 처음 스냅샷을 건드릴 때 한 번 비우면 된다.
     * 발신은 `makeCall` 이 **동기적으로** `onCallState(CALLING)` 을 부르므로 그 콜백이 첫 지점이고,
     * 착신은 `onIncomingCall` 이 첫 지점이다.
     */
    bool needsReset_ = true;
    void claimFresh(int id) { if (needsReset_) { needsReset_ = false; o_->resetCall(id); } }
    int accountId() const { return accountId_; }

    /** MCPTT 세션 신원을 CallInfo 에 투영. 발신은 makeCall 이 동기적으로 onCallState(CALLING) 를 부르므로
     *  첫 스냅샷부터 isMcptt/groupId 가 실려야 앱이 호 종류를 잠시라도 VoLTE 로 읽지 않는다 — startMcptt 의
     *  사후 기록과 onCallState 가 같은 값을 쓴다. 착신은 INVITE 의 mcptt-info(mi) 가 이미 채웠으므로 건드리지 않는다. */
    void projectMcptt(CallInfo& c) const {
        if (!mcptt || c.isMcptt) return;
        c.isMcptt = true; c.groupId = mcptt->groupId;
        c.mcptt.present = true; c.mcptt.sessionType = mcptt->isPrivate ? "private" : "prearranged";
        c.mcptt.privateCall = mcptt->isPrivate; c.mcptt.noFloorCtrl = mcptt->fullDuplex;
        c.mcptt.emergency = mcptt->emergency; c.mcptt.imminentPeril = mcptt->imminentPeril;
        c.mcptt.broadcast = mcptt->broadcast;
        c.halfDuplex = !mcptt->fullDuplex; c.listenOnly = mcptt->listenOnly;
        c.condition.emergency = mcptt->emergency; c.condition.imminentPeril = mcptt->imminentPeril;
        c.condition.mine = mcptt->condMine;
    }

    /** 세션 조건을 스냅샷에 옮기고 onMcpttCondition 을 낸다. */
    void publishCondition(ConditionCause cause) {
        if (!mcptt) return;
        CallInfo snap;
        o_->updateCall(getId(), [&](CallInfo& c) {
            c.condition.emergency = mcptt->emergency; c.condition.imminentPeril = mcptt->imminentPeril;
            c.condition.mine = mcptt->condMine; c.condition.pending = mcptt->condPending;
            c.condition.lastCode = mcptt->condLastCode;
        }, &snap);
        o_->emit([o = o_, snap, cause] { o->listener->onMcpttCondition(snap, cause); });
    }

    /** 서버 재광고(TS 24.379 §10.1.1.2.1.6) — emergency-ind true 는 임박을 내린다(1)d)), false 는 긴급만, imminentperil-ind 는 임박만.
     *  둘 다 내려가면 이 단말이 올린 조건도 끝이다. 바뀌었으면 true. */
    bool applyAdvertised(int e, int i) {
        if (!mcptt || (!e && !i)) return false;
        bool ne = mcptt->emergency, ni = mcptt->imminentPeril;
        if (e > 0) { ne = true; ni = false; }
        else if (e < 0) ne = false;
        if (i > 0 && e <= 0) ni = true;
        else if (i < 0) ni = false;
        if (ne == mcptt->emergency && ni == mcptt->imminentPeril) return false;
        mcptt->emergency = ne; mcptt->imminentPeril = ni;
        if (!ne && !ni) mcptt->condMine = false;
        return true;
    }

    /** 청취 전용 leg — 로컬 SDP 의 audio 방향을 recvonly 로 (서버가 PTT_JOIN recv_only / tap 으로 해석). */
    static std::string forceRecvOnly(const std::string& w) {
        size_t a = w.find("a=sendrecv");
        if (a != std::string::npos) return w.substr(0, a) + "a=recvonly" + w.substr(a + 10);
        size_t ma = w.find("m=audio ");
        if (ma == std::string::npos) return w;
        size_t eol = w.find("\r\n", ma);
        return eol == std::string::npos ? w : w.substr(0, eol + 2) + "a=recvonly\r\n" + w.substr(eol + 2);
    }

    /** floor participant 생성·바인드 + 콜백 배선. 이벤트 콜백 안의 callId 는 나중에(makeCall 뒤) 정해질 수
     *  있어 참조로 들고 있다가 sealCallId 로 확정한다. 마이크 게이트는 ue-ctl 로 넘겨 pjsua 를 만진다. */
    bool openFloor(const std::string& userId) {
        floor::Participant::Callbacks cb;
        Engine::Impl* o = o_;
        auto idRef = floorCallId_;
        cb.onEvent = [o, idRef](FloorEvent ev) {
            ev.callId = *idRef;
            o->emit([o, ev] { o->listener->onFloor(ev); });
        };
        cb.onMic = [o, idRef](bool on) {
            int id = *idRef;
            o->ctl.post([o, id, on] {
                PjCall* c = o->findCall(id);
                if (!c || !c->mcptt) return;
                c->mcptt->micOpen = on;
                try { o->wireMedia(c, id); } catch (pj::Error& e) { o->log(2, std::string("floor mic: ") + e.info(false)); }
            });
        };
        cb.log = [o](int level, const std::string& m) { o->log(level, m); };
        // 일제 통화 개시자: 발언을 놓은 뒤 B-bit Floor Idle → 호 해제(BYE) — TS 24.380 §6.2.4.6.4, TS 24.379 §4.12.
        //   세션은 서버가 T4 로도 거두지만, 개시 단말이 먼저 나가는 것이 규격 절차다.
        cb.onBroadcastEnd = [o, idRef] {
            int id = *idRef;
            o->ctl.post([o, id] {
                PjCall* c = o->findCall(id);
                if (!c) return;
                o->log(3, "broadcast call " + std::to_string(id) + ": floor idle after release → release call");
                try { pj::CallOpParam prm; c->hangup(prm); } catch (pj::Error& e) { o->log(2, std::string("broadcast release: ") + e.info(false)); }
            });
        };
        mcptt->floor.reset(new floor::Participant(-1, ssrcOf(userId), userId, cb));
        if (!mcptt->floor->open(0)) { mcptt->floor.reset(); return false; }
        mcptt->floor->setMicOpenDelay(o_->cfg.grantMicDelayMs);
        if (mcptt->listenOnly) mcptt->floor->setListenOnly(true);
        return true;
    }
    void sealCallId(int id) { *floorCallId_ = id; }

    void learnFloorRemote(const std::string& sdp) {
        if (!mcptt || !mcptt->floor || mcptt->remoteLearned) return;
        std::string ip; int port = 0;
        if (parseApplication(sdp, ip, port)) {
            mcptt->remoteLearned = true;
            mcptt->floor->setRemote(ip, port);
        }
    }

    void onCallSdpCreated(pj::OnCallSdpCreatedParam& prm) override {
        if (msrp) {
            // m=message 섹션 — 발신 offer 는 pjsua 의 m=text 슬롯 자리에, 수신 answer 는 pjsua 가 포트 0 으로 만든 섹션을 교체한다
            //   (media_count 불변). 수신의 더미 오디오는 inactive(서버 계약 — 포트 9 inactive 와 짝).
            try {
                std::string whole = prm.sdp.wholeSdp;
                if (whole.empty()) { o_->log(1, "msrp: empty wholeSdp — skip inject"); return; }
                if (msrp->localPath.empty()) {
                    std::string ip = msrp::connAddrOf(whole);
                    msrp->localPath = msrp::localPath(ip.empty() ? "127.0.0.1" : ip, msrp->sessionId);
                }
                // 미디어 수 불변(pjsua med_prov_cnt ≥ SDP media_count) — floor 주입과 같이 m=text 슬롯을 쓴다. 덧붙이면 assert.
                const char* slot = whole.find("m=message") != std::string::npos ? "m=message"
                                 : whole.find("m=text") != std::string::npos ? "m=text" : "\x01";
                whole = replaceMediaSection(whole, slot, msrp->outgoing ? msrp::sdpSection(msrp->localPath, "actpass", "sendonly")
                                                                        : msrp::sdpSection(msrp->localPath, "active", "recvonly"));
                if (!msrp->outgoing) whole = msrp::audioInactive(whole);
                prm.sdp.wholeSdp = whole;
            } catch (...) {}
            return;
        }
        if (!mcptt && !recvOnly) return;
        try {
            if (recvOnly && !prm.sdp.wholeSdp.empty()) prm.sdp.wholeSdp = forceRecvOnly(prm.sdp.wholeSdp);
            if (!mcptt) return;
            if (!mcptt->pendingAppSdp.empty()) {
                std::string whole = prm.sdp.wholeSdp;
                if (whole.empty()) {
                    o_->log(1, "onCallSdpCreated: empty wholeSdp (SDP print buffer overflow) — skip floor inject");
                } else if (whole.find("m=application") != std::string::npos) {
                    prm.sdp.wholeSdp = replaceMediaSection(whole, "m=application", mcptt->pendingAppSdp);
                } else if (whole.find("m=text") != std::string::npos) {
                    prm.sdp.wholeSdp = replaceMediaSection(whole, "m=text", mcptt->pendingAppSdp);
                } else {
                    prm.sdp.wholeSdp = replaceMediaSection(whole, "\x01", mcptt->pendingAppSdp);   // append
                }
            }
            // 청취 전용 합류(a=recvonly) — 관제 PTT 청취(dispatch_center.md §5.6): 서버가 PTT_JOIN recv_only 로 변환.
            if (mcptt->listenOnly && !prm.sdp.wholeSdp.empty()) prm.sdp.wholeSdp = forceRecvOnly(prm.sdp.wholeSdp);
            if (!prm.remSdp.wholeSdp.empty()) learnFloorRemote(prm.remSdp.wholeSdp);          // UAS: 상대 offer
        } catch (...) {}
    }

    void onCallTsxState(pj::OnCallTsxStateParam& prm) override {
        try {
            if (prm.e.type != PJSIP_EVENT_TSX_STATE) return;
            const pj::SipTransaction& tsx = prm.e.body.tsxState.tsx;
            // 상향·하향 re-INVITE 의 최종 응답(§10.1.1.2.1.3~5) — 수신 응답·타이머(408) 모두. 2xx = 확정, 그 밖 = 이전 값(§6.2.8.1.5).
            //   성립 전 호에는 보내지 않으므로(setCallCondition) 대기 중인 UAC INVITE 최종 응답은 그 re-INVITE 의 것이다.
            if (mcptt && mcptt->condPending && tsx.role == PJSIP_ROLE_UAC && tsx.method == "INVITE" && tsx.statusCode >= 200) {
                mcptt->condPending = false;
                mcptt->condLastCode = tsx.statusCode;
                const bool ok = tsx.statusCode / 100 == 2;
                if (!ok) { mcptt->emergency = mcptt->prevEmergency; mcptt->imminentPeril = mcptt->prevImminent; mcptt->condMine = mcptt->prevMine; }
                o_->log(3, "call " + std::to_string(getId()) + " condition re-INVITE → " + std::to_string(tsx.statusCode));
                publishCondition(ok ? ConditionCause::Confirmed : ConditionCause::Denied);
            }
            if (prm.e.body.tsxState.type != PJSIP_EVENT_RX_MSG) return;
            const std::string& msg = prm.e.body.tsxState.src.rdata.wholeMsg;
            if (msg.empty()) return;
            if (msrp) {
                // 발신 200 OK answer 의 cmdp a=path → 입출력 스레드(TS 24.282 §9.2.3)
                if (msrp->outgoing && !msrp->started && tsx.role == PJSIP_ROLE_UAC && tsx.method == "INVITE" && msg.rfind("SIP/2.0 2", 0) == 0) {
                    msrp->serverPath = msrp::pathOfSdp(sipBody(msg));
                    msrp->started = true;
                    o_->startMsrpSend(getId(), accountId_, *msrp);
                }
                return;
            }
            if (mcptt && msg.rfind("SIP/2.0 2", 0) == 0 && msg.find("m=application") != std::string::npos) {
                const std::string body = sipBody(msg);
                learnFloorRemote(body);                                                       // UAC: 200 OK answer
                // 암묵적 발언 요청의 결과(§14.3.4 mc_granted = 승인 · §14.3.5 mc_implicit_request = 받아들임) — 목적지를 안 뒤에
                if (mcptt->implicitAwaitAnswer && mcptt->floor && prm.e.body.tsxState.tsx.method == "INVITE") {
                    mcptt->implicitAwaitAnswer = false;
                    const mcptt::FloorFmtp f = mcptt::parseFloorFmtp(body);
                    mcptt->floor->onInitialAnswer(f.granted, f.implicitRequest);
                }
            }
            if (msg.rfind("SIP/2.0 2", 0) == 0 && msg.find("a=ssrc:") != std::string::npos) {
                // 감청 leg 200 OK — a=ssrc label:caller/callee (RFC 5576) → 소스 귀속(U10 디먹스 라벨)
                std::vector<MediaSource> src = mcptt::sdpSsrcLabels(sipBody(msg));
                if (!src.empty()) {
                    CallInfo snap;
                    o_->updateCall(getId(), [&](CallInfo& c) { c.sources = src; }, &snap);
                    o_->emit([o = o_, snap] { o->listener->onCallMedia(snap); });
                }
            }
            // 세션 조건 재광고(TS 24.379 §6.3.3.1.15·§6.3.3.1.16) — 서버가 멤버 leg 에 보내는 re-INVITE, 조인 200 OK 동봉.
            //   rdata 는 수신 원문만이므로 "INVITE " = 수신 (re-)INVITE, "SIP/2.0 200" = 내 INVITE 의 응답. 바뀐 경우만 이벤트.
            if (mcptt && (msg.rfind("INVITE ", 0) == 0 || (msg.rfind("SIP/2.0 200", 0) == 0 && tsx.method == "INVITE")) &&
                msg.find("mcpttinfo") != std::string::npos) {
                if (applyAdvertised(mcptt::indicator(msg, "emergency-ind"), mcptt::indicator(msg, "imminentperil-ind")))
                    publishCondition(ConditionCause::Advertised);
            }
            if (msg.rfind("NOTIFY ", 0) == 0 && msg.find("conference-info") != std::string::npos) {
                std::vector<RosterEntry> users; bool full = false;
                if (mcptt::parseConferenceInfo(sipBody(msg), users, full)) {
                    std::string gid = mcptt ? mcptt->groupId : std::string();
                    int acc = accountId_;
                    o_->emit([o = o_, acc, gid, users, full] { o->listener->onRoster(acc, gid, users, full); });
                }
            }
        } catch (...) {}
    }

    void onCallState(pj::OnCallStateParam&) override {
        pj::CallInfo ci = getInfo();
        const int id = getId();
        if (msrp) {                                                       // 앱 호 목록 밖 — 끝나면 정리만
            if (ci.state != PJSIP_INV_STATE_DISCONNECTED) return;
            msrp->cancel->store(true);
            if (msrp->outgoing && !msrp->started) {                        // INVITE 가 거절됐다(403 게이트·488 등) — 발신 결과로 알린다
                RequestResult r;
                r.accountId = accountId_; r.token = msrp->token; r.method = "MSRP";
                r.code = ci.lastStatusCode ? ci.lastStatusCode : 500; r.reason = ci.lastReason;
                o_->emit([o = o_, r] { o->listener->onRequestResult(r); });
            }
            o_->ctl.post([o = o_, id] { o->calls.erase(id); });
            return;
        }
        claimFresh(id);                  // 재사용된 call id 의 낡은 상태를 물려받지 않는다
        CallInfo snap;
        bool changed = false;
        o_->updateCall(id, [&](CallInfo& c) {
            c.accountId = accountId_;
            projectMcptt(c);
            c.remoteUri = ci.remoteUri;
            c.lastCode = ci.lastStatusCode;
            c.lastReason = ci.lastReason;
            CallState ns = c.state;
            switch (ci.state) {
                case PJSIP_INV_STATE_CALLING:
                case PJSIP_INV_STATE_EARLY:
                    if (ci.role == PJSIP_ROLE_UAC) ns = CallState::Outgoing;
                    break;
                case PJSIP_INV_STATE_CONNECTING:
                case PJSIP_INV_STATE_CONFIRMED:
                    if (c.state != CallState::Held) ns = CallState::Active;
                    break;
                case PJSIP_INV_STATE_DISCONNECTED:
                    ns = CallState::Disconnected;
                    c.mediaActive = false;
                    break;
                default: break;
            }
            changed = ns != c.state;
            c.state = ns;
        }, &snap);
        if (changed) o_->emit([o = o_, snap] { o->listener->onCallState(snap); });
        if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
            o_->ctl.post([o = o_, id] {                    // 콜백 안에서 자기 객체를 지우지 않는다
                o->calls.erase(id);                        // ~PjCall → floor participant close
                std::lock_guard<std::mutex> lk(o->snapM);
                o->pruneFinished();
            });
        }
    }

    void onStreamDestroyed(pj::OnStreamDestroyedParam& prm) override {
        if (msrp) return;
        try {
            StreamStats s = Engine::Impl::fromPj(getStreamStat(prm.streamIdx));
            CallQuality q;
            bool audio = false;
            try {
                pj::CallInfo ci = getInfo();
                audio = prm.streamIdx < ci.media.size() && ci.media[prm.streamIdx].type == PJMEDIA_TYPE_AUDIO;
                if (audio) q = Engine::Impl::measure(this, prm.streamIdx);
            } catch (...) { audio = false; }
            std::lock_guard<std::mutex> lk(o_->snapM);
            o_->finalStats[getId()] = s;
            if (audio && q.valid) o_->finalQuality[getId()] = quality::merge(o_->finalQuality[getId()], q);
        } catch (...) {}
    }

    void onCallMediaState(pj::OnCallMediaStateParam&) override {
        if (msrp) return;                                                 // 더미 오디오 — 결선하지 않는다
        const int id = getId();
        pj::CallInfo ci = getInfo();
        bool held = false, active = false;
        const bool rxOnly = recvOnly || (mcptt && mcptt->listenOnly);
        for (auto& m : ci.media) {
            if (m.type != PJMEDIA_TYPE_AUDIO) continue;
            if (m.status == PJSUA_CALL_MEDIA_ACTIVE) active = true;
            else if (m.status == PJSUA_CALL_MEDIA_REMOTE_HOLD && rxOnly) active = true;     // 서버 sendonly ↔ 우리 recvonly
            else if (m.status == PJSUA_CALL_MEDIA_LOCAL_HOLD || m.status == PJSUA_CALL_MEDIA_REMOTE_HOLD) held = true;
        }
        try { if (active) o_->wireMedia(this, id); } catch (pj::Error& e) { o_->log(2, std::string("wireMedia: ") + e.info(false)); }
        o_->attachVideo(this, accountId_);
        CallInfo snap;
        bool stateChanged = false;
        o_->updateCall(id, [&](CallInfo& c) {
            c.mediaActive = active;
            CallState ns = c.state;
            if (held) ns = CallState::Held;
            else if (active && c.state == CallState::Held) ns = CallState::Active;
            stateChanged = ns != c.state;
            c.state = ns;
        }, &snap);
        o_->emit([o = o_, snap, stateChanged] {
            o->listener->onCallMedia(snap);
            if (stateChanged) o->listener->onCallState(snap);
        });
    }

private:
    Engine::Impl* o_;
    int accountId_;
    std::shared_ptr<int> floorCallId_ = std::make_shared<int>(-1);
};

class PjAccount : public pj::Account {
public:
    PjAccount(Engine::Impl* o, int accountId) : o_(o), accountId_(accountId) {}

    void onRegState(pj::OnRegStateParam& prm) override {
        bool active = false;
        int expires = 0;
        try { pj::AccountInfo ai = getInfo(); active = ai.regIsActive; expires = ai.regExpiresSec; } catch (...) {}
        RegInfo ri;
        ri.accountId = accountId_;
        ri.code = prm.code;
        ri.reason = prm.reason;
        ri.expiresSec = expires;
        if (active && prm.code / 100 == 2) ri.state = RegState::Registered;
        else if (!active && prm.code / 100 == 2) ri.state = RegState::Unregistered;
        else ri.state = RegState::Failed;
        { std::lock_guard<std::mutex> lk(o_->snapM); o_->regInfos[accountId_] = ri; }
        o_->emit([o = o_, ri] { o->listener->onRegState(ri); });
    }

    void onIncomingCall(pj::OnIncomingCallParam& prm) override {
        auto* call = new PjCall(o_, *this, accountId_, prm.callId);
        call->sealCallId(prm.callId);
        call->claimFresh(prm.callId);                   // 재사용된 call id 의 낡은 상태를 물려받지 않는다
        std::string whole;
        try { whole = prm.rdata.wholeMsg; } catch (...) {}
        std::string remote;
        try { remote = call->getInfo().remoteUri; } catch (...) {}
        const AccountConfig& cfg = o_->accountCfgs[accountId_];
        // MCData media plane 배포 INVITE(TS 24.282 §9.2.3 — m=message TCP/MSRP + a=path) — 통화가 아니다: 앱에 알리지 않고 받아
        //   cmdp 에 붙어 본문을 받는다(onSds, mediaPlane). 발신자·그룹은 mcdata-info(1:1 이면 request-uri 가 나 자신).
        if (whole.find("TCP/MSRP") != std::string::npos && whole.find("a=path:") != std::string::npos) {
            { std::lock_guard<std::mutex> lk(o_->snapM); o_->callInfos.erase(prm.callId); }   // claimFresh 가 만든 빈 항목 — 앱 호가 아니다
            call->msrp.reset(new MsrpLeg);
            call->msrp->outgoing = false;
            call->msrp->serverPath = msrp::pathOfSdp(whole);
            call->msrp->fromUri = msrp::mcdataInfoUri(whole, "mcdata-calling-user-id");
            if (call->msrp->fromUri.empty()) call->msrp->fromUri = remote;
            std::string req = msrp::mcdataInfoUri(whole, "mcdata-request-uri");
            if (mcptt::bareId(req) != mcptt::bareId(cfg.effectiveMcpttId()) && mcptt::bareId(req) != cfg.msisdn) call->msrp->groupUri = req;
            o_->ctl.post([o = o_, call, id = prm.callId] {
                o->calls[id].reset(call);
                try {
                    pj::CallOpParam p(true);
                    p.statusCode = PJSIP_SC_OK;
                    p.opt.audioCount = 1;
                    p.opt.videoCount = 0;
                    call->answer(p);                                     // answer SDP 는 여기서 만든다(onCallSdpCreated 주입)
                } catch (pj::Error& e) { o->log(1, std::string("msrp answer: ") + e.info(false)); return; }
                call->msrp->started = true;
                o->startMsrpRecv(id, call->accountId(), *call->msrp);
            });
            return;
        }
        McpttInfo mi = mcptt::parseMcpttInfo(whole);
        bool autoAnswer = false;
        if (mi.present) {
            // MCPTT 착신 — floor 소켓은 **180 전에** 바인드해야 한다(pjsua 는 여기서 응답 SDP 를 한 번 만들고
            // 200 에 재사용하므로, 늦으면 m=application 0 이 나가 CSP 가 착신 leg 의 floor 포트를 모른다).
            call->mcptt.reset(new McpttSession);
            call->mcptt->isPrivate = mi.privateCall;
            call->mcptt->fullDuplex = mi.noFloorCtrl;
            call->mcptt->groupId = mi.privateCall ? mcptt::bareId(mi.callingUserId) : mcptt::bareId(remote);
            call->mcptt->emergency = mi.emergency;
            call->mcptt->imminentPeril = mi.imminentPeril && !mi.emergency;
            if (!mi.noFloorCtrl) {
                if (call->openFloor(cfg.effectiveMcpttId()))
                    call->mcptt->pendingAppSdp = mcptt::floorSdp(call->mcptt->floor->localPort(), false);
            } else {
                call->mcptt->micOpen = true;                                                 // 전이중 — 마이크 상시
            }
            autoAnswer = cfg.autoAnswerMcptt;
        }
        CallInfo snap;
        o_->updateCall(prm.callId, [&](CallInfo& c) {
            c.accountId = accountId_;
            c.dir = CallDir::Incoming;
            c.state = CallState::Incoming;
            c.remoteUri = remote;
            c.video = whole.find("m=video") != std::string::npos;
            c.calledParty = detail::uriUser(detail::headerValue(whole, "P-Called-Party-ID"));
            if (mi.present) {
                c.isMcptt = true; c.mcptt = mi; c.groupId = call->mcptt->groupId;
                c.halfDuplex = !mi.noFloorCtrl;
                c.condition.emergency = call->mcptt->emergency; c.condition.imminentPeril = call->mcptt->imminentPeril;
            }
        }, &snap);
        o_->ctl.post([o = o_, call, id = prm.callId] { o->calls[id].reset(call); });
        try {
            pj::CallOpParam p;
            p.statusCode = PJSIP_SC_RINGING;
            call->answer(p);
        } catch (pj::Error& e) { o_->log(2, std::string("180 failed: ") + e.info(false)); }
        o_->emit([o = o_, snap] { o->listener->onIncomingCall(snap); });
        if (autoAnswer) {
            o_->ctl.post([o = o_, id = prm.callId] {
                PjCall* c = o->findCall(id);
                if (!c) return;
                try {
                    pj::CallOpParam p(true);
                    p.statusCode = PJSIP_SC_OK;
                    p.opt.audioCount = 1;
                    p.opt.videoCount = 0;
                    c->answer(p);
                } catch (pj::Error& e) { o->log(1, std::string("mcptt auto-answer: ") + e.info(false)); }
            });
        }
    }

    /** sendRequest 트랜잭션 최종 응답(≥200) — 같은 tsx 가 COMPLETED/TERMINATED 로 두 번 올 수 있다. */
    void onSendRequest(pj::OnSendRequestParam& prm) override {
        try {
            if (prm.e.type != PJSIP_EVENT_TSX_STATE) return;
            auto& ts = prm.e.body.tsxState;
            if (ts.tsx.statusCode < 200) return;
            RequestResult r;
            r.accountId = accountId_;
            r.token = (int64_t)(intptr_t)prm.userData;
            r.method = ts.tsx.method;
            r.code = ts.tsx.statusCode;
            r.reason = ts.tsx.statusText;
            if (ts.type == PJSIP_EVENT_RX_MSG) r.etag = detail::headerValue(ts.src.rdata.wholeMsg, "SIP-ETag");
            Engine::Impl::PendingPublish retry;
            int64_t retryToken = -1;
            {
                std::lock_guard<std::mutex> lk(o_->snapM);
                auto it = o_->publishPending.find(r.token);
                if (it != o_->publishPending.end()) {
                    const Engine::Impl::PendingPublish p = it->second;
                    const std::string key = std::to_string(p.accountId) + ":" + p.groupId;
                    r.token = p.appToken;                  // 앱은 affiliate() 의 token 으로 상관한다
                    if (r.code == PJSIP_SC_CONDITIONAL_REQUEST_FAILED) {
                        // RFC 3903 §5 — 412 를 낸 entity-tag 는 버리고(MUST) 같은 요청을 다시 보내지 않는다(MUST NOT).
                        // 상태는 SIP-If-Match 없는 초기 PUBLISH 로 다시 알린다(SHOULD, §4.2) — 한 번만, 결과는 그 응답으로.
                        o_->publishEtag.erase(key);
                        if (p.conditional) { retry = p; retryToken = o_->nextToken++; }
                    } else if (!r.etag.empty() && r.code / 100 == 2) {
                        o_->publishEtag[key] = r.etag;
                    }
                    o_->publishPending.erase(it);
                }
            }
            if (retryToken >= 0) {
                o_->ctl.post([o = o_, retry, retryToken, r] {
                    if (o->sendAffiliation(retry.accountId, retry.groupId, retry.on, retryToken, retry.appToken, false) < 0)
                        o->emit([o, r] { o->listener->onRequestResult(r); });   // 재발행을 못 만들면 412 를 그대로
                });
                return;
            }
            o_->emit([o = o_, r] { o->listener->onRequestResult(r); });
        } catch (...) {}
    }

    /** MESSAGE/NOTIFY 본문 — MCData SDS → onSds, conference-info → onRoster, 그 외 onMessage.
     *  multipart 는 pjsua2 msgBody 가 비거나 boundary 가 빠지므로 원문에서 Content-Type·본문을 직접 뽑는다. */
    void onInstantMessage(pj::OnInstantMessageParam& prm) override {
        std::string ct = prm.contentType, body = prm.msgBody, from = prm.fromUri;
        bool multipart = ct.rfind("multipart/", 0) == 0;
        if (body.empty() || (multipart && ct.find("boundary") == std::string::npos)) {
            std::string whole;
            try { whole = prm.rdata.wholeMsg; } catch (...) {}
            if (!whole.empty()) {
                std::string h = detail::headerValue(whole, "Content-Type");
                if (!h.empty()) ct = h;
                body = sipBody(whole);
            }
        }
        int acc = accountId_;
        if (body.find("mcdata-signalling") == std::string::npos && body.find("mcpttinfo") != std::string::npos) {
            // 긴급 경보·취소·그룹 긴급 통지(TS 24.379 §12.1.1.3) — 200 OK 는 pjsua 가 이미 보냈다(7)·8)).
            EmergencyAlert a; a.accountId = acc;
            if (mcptt::parseEmergencyAlert(body, a)) {
                if (a.userId.empty()) a.userId = mcptt::bareId(from);
                auto ic = o_->accountCfgs.find(acc);
                a.self = ic != o_->accountCfgs.end() && a.userId == mcptt::bareId(ic->second.effectiveMcpttId());
                o_->emit([o = o_, a] { o->listener->onEmergencyAlert(a); });
                return;
            }
        }
        if (body.find("mcdata-signalling") != std::string::npos) {
            SdsMessage m;
            if (mcdata::parse(ct, body, m)) {
                m.accountId = acc; m.fromUri = from;
                o_->emit([o = o_, m] { o->listener->onSds(m); });
                return;
            }
        }
        if (ct.find("dialog-info") != std::string::npos) {
            std::vector<DialogInfo> dl;
            if (mcptt::parseDialogInfo(body, dl)) {
                if (dl.empty()) {                                   // 초기 full 스냅샷에 dialog 없음 — 구독 성립 신호(callId 빈 값)
                    DialogInfo none; none.accountId = acc; none.watched = mcptt::bareId(from); none.full = true;
                    o_->emit([o = o_, none] { o->listener->onDialogInfo(none); });
                }
                for (auto& d : dl) { d.accountId = acc; o_->emit([o = o_, d] { o->listener->onDialogInfo(d); }); }
                return;
            }
        }
        if (ct.find("conference-info") != std::string::npos) {
            std::vector<RosterEntry> users; bool full = false;
            if (mcptt::parseConferenceInfo(body, users, full)) {
                std::string gid = mcptt::bareId(from);
                o_->emit([o = o_, acc, gid, users, full] { o->listener->onRoster(acc, gid, users, full); });
                return;
            }
        }
        o_->emit([o = o_, acc, from, ct, body] { o->listener->onMessage(acc, from, ct, body); });
    }

private:
    Engine::Impl* o_;
    int accountId_;
};

}  // namespace

// ── Impl 헬퍼 ──

bool Engine::Impl::rxOnlyLeg(PjCall* call) { return call->recvOnly || (call->mcptt && call->mcptt->listenOnly); }

pj::AudioMedia* Engine::Impl::activeAudio(PjCall* call, unsigned* idxOut) {
    pj::CallInfo ci = call->getInfo();
    for (auto& m : ci.media) {
        // 청취 전용 leg(a=recvonly)는 서버가 sendonly 로 답하므로 pjsua 가 REMOTE_HOLD 로 분류한다 — 미디어는 흐른다.
        bool ok = m.status == PJSUA_CALL_MEDIA_ACTIVE || (m.status == PJSUA_CALL_MEDIA_REMOTE_HOLD && rxOnlyLeg(call));
        if (m.type == PJMEDIA_TYPE_AUDIO && ok) {
            pj::Media* med = call->getMedia(m.index);          // 비활성(hold 등)이면 NULL 일 수 있다
            if (!med) continue;
            if (idxOut) *idxOut = m.index;
            return pj::AudioMedia::typecastFromMedia(med);
        }
    }
    return nullptr;
}

PjCall* Engine::Impl::findCall(int callId) {
    auto it = calls.find(callId);
    return it == calls.end() ? nullptr : static_cast<PjCall*>(it->second.get());
}

void Engine::Impl::applyCodecPolicy() {
    // 음성: AMR-WB 최우선 + fmtp octet-align=1; mode-set=0,1,2 (enc/dec). G.711 은 안전망으로 낮은 우선순위,
    // 그 외는 0(협상 표면 축소). codecId 는 실제 열람 결과에서 부분일치로 찾는다(백엔드 표기 차이 흡수).
    std::string amrwb;
    std::vector<std::string> ids;
    for (auto& c : ep->codecEnum2()) {
        ids.push_back(c.codecId);
        if (amrwb.empty() && c.codecId.find("AMR-WB") != std::string::npos) amrwb = c.codecId;
    }
    for (auto& id : ids) {
        unsigned char prio = 0;
        if (id == amrwb) prio = 254;
        else if (id.rfind("PCMU", 0) == 0 || id.rfind("PCMA", 0) == 0) prio = 100;
        try { ep->codecSetPriority(id, prio); } catch (...) {}
    }
    if (!amrwb.empty()) {
        try {
            pj::CodecParam cp = ep->codecGetParam(amrwb);
            pj::CodecFmtpVector f;
            pj::CodecFmtp oa; oa.name = "octet-align"; oa.val = "1";
            pj::CodecFmtp ms; ms.name = "mode-set"; ms.val = "0,1,2";
            f.push_back(oa); f.push_back(ms);
            cp.setting.encFmtp = f;
            cp.setting.decFmtp = f;
            ep->codecSetParam(amrwb, cp);
        } catch (pj::Error& e) { log(2, std::string("AMR-WB fmtp: ") + e.info(false)); }
    } else {
        log(2, "AMR-WB codec not found — 음성 협상은 G.711 안전망만 가능");
    }
    std::string all;
    for (auto& id : ids) all += id + " ";
    log(3, "codecs: " + all + (amrwb.empty() ? "" : "(AMR-WB first)"));
#if PJSUA_HAS_VIDEO
    // 영상: H.264 최우선 + 인코딩 480x640(세로)·15 fps·평균 400 / 최대 500 kbit/s — 기존 VoLTE 앱(CodecConfig.kt)과 같은 값.
    try {
        for (auto& c : ep->videoCodecEnum2()) {
            if (c.codecId.find("H264") == std::string::npos) continue;
            ep->videoCodecSetPriority(c.codecId, 254);
            pj::VidCodecParam vp = ep->getVideoCodecParam(c.codecId);
            vp.encFmt.width = 480; vp.encFmt.height = 640;
            vp.encFmt.fpsNum = 15; vp.encFmt.fpsDenum = 1;
            vp.encFmt.avgBps = 400000; vp.encFmt.maxBps = 500000;
            ep->setVideoCodecParam(c.codecId, vp);
            log(3, "video codec " + c.codecId + " first, enc 480x640 15fps 400k/500k");
            break;
        }
    } catch (pj::Error& e) { log(2, std::string("video codec: ") + e.info(false)); }
#endif
}

// ── 영상 ──
#if defined(__ANDROID__)
static void windowAcquire(void* w) { if (w) ANativeWindow_acquire(static_cast<ANativeWindow*>(w)); }
static void windowRelease(void* w) { if (w) ANativeWindow_release(static_cast<ANativeWindow*>(w)); }
#else
static void windowAcquire(void*) {}                     // 참조 수를 세지 않는 창(HWND 등)
static void windowRelease(void*) {}
#endif

std::vector<int> Engine::Impl::cameras() {
    std::vector<int> v;
#if PJSUA_HAS_VIDEO
    try {
        pj::VideoDevInfoVector2 devs = ep->vidDevManager().enumDev2();
        for (auto& d : devs) {
            if (!(d.dir & PJMEDIA_DIR_CAPTURE)) continue;
            if (d.driver == "Colorbar") continue;           // 합성 장치(시험용 색 막대)
            v.push_back(d.id);
        }
    } catch (pj::Error& e) { log(2, std::string("camera enum: ") + e.info(false)); }
#endif
    return v;
}

int Engine::Impl::frontCamera() {
#if PJSUA_HAS_VIDEO
    int first = -1;
    try {
        for (auto& d : ep->vidDevManager().enumDev2()) {
            if (!(d.dir & PJMEDIA_DIR_CAPTURE) || d.driver == "Colorbar") continue;
            if (first < 0) first = d.id;
            std::string n = d.name;
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
            if (n.find("front") != std::string::npos) return d.id;
        }
    } catch (...) {}
    return first >= 0 ? first : (int)PJMEDIA_VID_DEFAULT_CAPTURE_DEV;
#else
    return -1;
#endif
}

void Engine::Impl::attachVideo(PjCall* call, int accountId) {
#if PJSUA_HAS_VIDEO
    bool autoTx = false;
    { auto it = accountCfgs.find(accountId); if (it != accountCfgs.end()) autoTx = it->second.videoAutoTransmit; }
    pj::CallInfo ci;
    try { ci = call->getInfo(); } catch (...) { return; }
    for (auto& m : ci.media) {
        if (m.type != PJMEDIA_TYPE_VIDEO || m.status != PJSUA_CALL_MEDIA_ACTIVE) continue;
        if (m.dir & PJMEDIA_DIR_DECODING) {
            std::lock_guard<std::mutex> lk(videoM);
            if (videoWindow) {
                try {
                    pj::VideoWindow vw = m.videoWindow;
                    if (vw.getInfo().winHandle.handle.window != videoWindow) {
                        pj::VideoWindowHandle h;
#if defined(__ANDROID__)
                        h.type = PJMEDIA_VID_DEV_HWND_TYPE_ANDROID;
#endif
                        h.handle.window = videoWindow;
                        windowAcquire(videoWindow);             // 렌더러가 이 참조를 가진다(교체·스트림 소멸 때 푼다)
                        vw.setWindow(h);
                    }
                    vw.Show(true);
                } catch (pj::Error& e) { log(2, std::string("video window: ") + e.info(false)); }
            }
        }
        if (autoTx) {
            // 계정 autoTransmitOutgoing 만으로는 협상 방향에 따라 캡처가 열리지 않을 수 있다 — 송신 방향이 없으면 sendrecv 로, 있으면 송신 개시.
            try {
                pj::CallVidSetStreamParam p;
                p.medIdx = (int)m.index;
                pjsua_call_vid_strm_op op = PJSUA_CALL_VID_STRM_START_TRANSMIT;
                if (!(m.dir & PJMEDIA_DIR_ENCODING)) { op = PJSUA_CALL_VID_STRM_CHANGE_DIR; p.dir = PJMEDIA_DIR_ENCODING_DECODING; }
                call->vidSetStream(op, p);
            } catch (pj::Error& e) { log(3, std::string("video transmit: ") + e.info(false)); }
        }
    }
#else
    (void)call; (void)accountId;
#endif
}

void Engine::Impl::wireMedia(PjCall* call, int callId) {
    // conference bridge 결선 — 호 → 스피커(listen), 마이크 → 호. MCPTT 반이중은 floor Granted(micOpen)에서만
    // 마이크를 결선한다. 장치 미디어는 Endpoint 소유라 보관하지 않고 매번 재취득.
    pj::AudioMedia* aud = activeAudio(call);
    if (!aud) return;
    CallInfo snap = snapshotCall(callId);
    pj::AudDevManager& adm = ep->audDevManager();
    pj::AudioMedia& spk = adm.getPlaybackDevMedia();
    pj::AudioMedia& mic = adm.getCaptureDevMedia();
    // 송출 원천 — 마이크 또는 기준 음원 재생기(setTxSource, ue_voice_quality.md §4.2). 재생기를 쓰는 동안 마이크는 호에서 뗀다.
    pj::AudioMedia& src = txPlayer ? static_cast<pj::AudioMedia&>(*txPlayer) : mic;
    if (txPlayer) mic.stopTransmit(*aud);
    // 재생 sink — 라우트 0 = 기본 재생 장치, 그 외 = 추가 재생 라우트. 선택되지 않은 sink 와의 결선은 끊는다
    // (미결선 쌍의 disconnect 는 no-op). 라우트가 사라졌으면 기본 장치로 폴백.
    pj::AudioMedia* sink = &spk;
    auto rt = routes.find(snap.playbackRoute);
    if (snap.playbackRoute != 0 && rt != routes.end()) sink = rt->second.get();
    if (sink != &spk) aud->stopTransmit(spk);
    for (auto& kv : routes) if (kv.second.get() != sink) aud->stopTransmit(*kv.second);
    if (snap.listen) aud->startTransmit(*sink); else aud->stopTransmit(*sink);
    // 듣는 크기 = 통화 포트→bridge 유입. pjsua2 AudioMedia 방향은 미디어 관점이라 유입은 adjustTxLevel 이다 — adjustRxLevel 은
    //   bridge→통화(= 상대에게 보내는 내 음성)를 바꾼다(ue_audio_level.md §2). 재협상으로 포트가 새로 생기면 1 로 돌아가므로 매 결선.
    aud->adjustTxLevel(snap.rxLevel);
    bool micOn;
    // 반이중 = floor 가 게이트(Granted), 전이중(mc_no_floor_ctrl) = 앱의 음소거(setMuted — PTT 로컬 게이트, 원천 앱 동작)
    if (call->mcptt) micOn = !call->mcptt->listenOnly && (call->mcptt->fullDuplex ? !snap.muted : call->mcptt->micOpen);
    else micOn = !snap.muted && !call->recvOnly;
    if (micOn) src.startTransmit(*aud); else src.stopTransmit(*aud);
    applyDeviceLevels();                                   // 결선으로 장치가 막 열렸을 수 있다 — 장치 단 음량 재적용
}

void Engine::Impl::applyDeviceLevels() {
    if (!levelsSet || cfg.nullAudioDevice) return;
    pj::AudDevManager& adm = ep->audDevManager();
    // ue_audio_level.md §2 — slot 0 은 캡처·재생 미디어가 같은 객체이고 pjsua2 방향은 미디어 관점이다:
    // adjustRxLevel = bridge → 장치 = 스피커, adjustTxLevel = 장치 → bridge = 마이크. 두 축은 따로 건다(한 축 실패가 다른 축을 지우지 않게).
    try { adm.getPlaybackDevMedia().adjustRxLevel(spkLevel); }
    catch (pj::Error& e) { log(4, std::string("speaker level: ") + e.info(false)); }
    try {
        adm.getCaptureDevMedia().adjustTxLevel(1.f);
        adm.setCaptureAgc(true, (float)micTarget);
    } catch (pj::Error& e) { log(4, std::string("mic agc: ") + e.info(false)); }   // 장치 지연 개방 중 — 다음 결선에서 다시
}

int64_t Engine::Impl::doSendRequest(int accountId, const std::string& method, const std::string& targetUri,
                                 const std::string& contentType, const std::string& body,
                                 const std::map<std::string, std::string>& headers, int64_t token) {
    auto it = accounts.find(accountId);
    if (it == accounts.end()) return -1;
    try {
        pj::SipTxOption tx;
        tx.targetUri = targetUri;
        if (!contentType.empty()) tx.contentType = contentType;
        if (!body.empty()) tx.msgBody = body;
        for (auto& kv : headers) { pj::SipHeader h; h.hName = kv.first; h.hValue = kv.second; tx.headers.push_back(h); }
        pj::SendRequestParam prm;
        prm.method = method;
        prm.txOption = tx;
        prm.userData = (pj::Token)(intptr_t)token;
        it->second->sendRequest(prm);
        return token;
    } catch (pj::Error& e) {
        log(1, method + " " + targetUri + ": " + e.info(false));
        return -1;
    }
}

// ── Engine 공개 API ──

Engine::Engine() : impl_(new Impl) {}
Engine::~Engine() { stop(); }

TlsPeerExpiry Engine::tlsPeerExpiry() const {
    std::lock_guard<std::mutex> lk(impl_->snapM);
    return impl_->tlsPeer;
}

std::string Engine::version() { return std::string(CIMSUE_VERSION) + " (pjproject " + pj_get_version() + ")"; }

bool Engine::running() const { return impl_->running; }

Result Engine::start(const EngineConfig& cfg, Listener* listener) {
    if (impl_->running) return Result::fail(-1, "already running");
    impl_->cfg = cfg;
    impl_->fanout.primary = listener;
    impl_->listener = &impl_->fanout;
    impl_->evt.start();
    impl_->ctl.start();
    Result r = impl_->ctl.runSync([this]() -> Result {
        Impl* o = impl_.get();
        try {
            pj_log_set_level(o->cfg.logLevel);               // libInit 전(writer 미설정) pjlib 기본 sink 는 stdout
            o->ep.reset(new PjEndpoint(o));
            o->ep->libCreate();
            pj::EpConfig epc;
            epc.uaConfig.userAgent = o->cfg.userAgent;
            epc.logConfig.level = o->cfg.logLevel;
            epc.logConfig.consoleLevel = o->cfg.logLevel;    // pjsua 는 앱 writer 호출도 console_level 로 게이트한다
            std::unique_ptr<pj::LogWriter> writer(new PjLog(o));
            epc.logConfig.writer = writer.get();
            epc.medConfig.noVad = o->cfg.noVad;
            epc.medConfig.clockRate = o->cfg.clockRate;
            // UDP→TCP 승격 스위치 — pjsip 전역, 송신 시점에 읽히므로 libInit 전 설정으로 충분하다.
            pjsip_cfg()->endpt.disable_tcp_switch = o->cfg.udpNoTcpSwitch ? PJ_TRUE : PJ_FALSE;
            o->ep->libInit(epc);
            o->logWriter = writer.release();                 // 이제 pjsua2 소유
            {
                pj::TransportConfig tc; tc.port = o->cfg.udpPort;
                o->ep->transportCreate(PJSIP_TRANSPORT_UDP, tc);
            }
            try {
                pj::TransportConfig tc; tc.port = o->cfg.tcpPort;
                o->ep->transportCreate(PJSIP_TRANSPORT_TCP, tc);
            } catch (pj::Error& e) { o->log(2, std::string("TCP transport: ") + e.info(false)); }
            try {
                pj::TransportConfig tc; tc.port = o->cfg.tlsPort;
                tc.tlsConfig.CaBuf = o->cfg.tlsCaPem;
                tc.tlsConfig.verifyServer = o->cfg.tlsVerifyServer;
                o->ep->transportCreate(PJSIP_TRANSPORT_TLS, tc);
            } catch (pj::Error& e) { o->log(2, std::string("TLS transport: ") + e.info(false)); }
            if (o->cfg.nullAudioDevice) o->ep->audDevManager().setNullDev();
            o->ep->libStart();
            o->applyCodecPolicy();
            o->captureOn = true;
            o->running = true;
            o->log(3, std::string("libcimsue ") + version() + " started");
            return Result::success();
        } catch (pj::Error& e) {
            try { if (o->ep) o->ep->libDestroy(); } catch (...) {}
            o->logWriter = nullptr;
            o->ep.reset();
            return fromError(e);
        }
    });
    if (!r.ok) { impl_->ctl.stop(); impl_->evt.stop(); }
    return r;
}

void Engine::stop() {
    if (!impl_->running) return;
    {
        std::lock_guard<std::mutex> lk(impl_->msrpM);                   // media plane 입출력 취소(소켓 대기는 ≤200 ms 안에 본다)
        for (auto& w : impl_->msrpCancels) if (auto c = w.lock()) c->store(true);
    }
    impl_->ctl.runSync([this] {
        Impl* o = impl_.get();
        o->calls.clear();                        // ~Call → hangup, floor participant close
        o->txPlayer.reset();                     // ~AudioMediaPlayer → bridge 포트 해제 (libDestroy 전)
        o->routes.clear();                       // ~ExtraAudioDevice → close (libDestroy 전)
        o->accounts.clear();                     // ~Account → shutdown
        try { o->ep->libDestroy(); } catch (...) {}               // LogWriter 도 여기서 pjsua2 가 delete
        o->logWriter = nullptr;
        o->ep.reset();
        o->running = false;
        return 0;
    });
    {
        std::unique_lock<std::mutex> lk(impl_->msrpM);                  // 분리 실행한 입출력 스레드가 Impl 을 더 쓰지 않게
        impl_->msrpCv.wait_for(lk, std::chrono::seconds(10), [&] { return impl_->msrpActive == 0; });
        impl_->msrpCancels.clear();
    }
    impl_->emit([o = impl_.get()] { o->listener->onEngineStopped(); });
    impl_->ctl.stop();
    impl_->evt.stop();
    std::lock_guard<std::mutex> lk(impl_->snapM);
    impl_->regInfos.clear();
    impl_->callInfos.clear();
    impl_->finalStats.clear();
    impl_->finalQuality.clear();
    impl_->publishPending.clear();
    impl_->publishEtag.clear();
}

int Engine::addAccount(const AccountConfig& cfg) {
    if (!impl_->running) return -1;
    if (!cfg.isComplete()) { impl_->log(1, "addAccount: config incomplete (host/domain/msisdn/IMPI/cred)"); return -1; }
    return impl_->ctl.runSync([this, cfg]() -> int {
        Impl* o = impl_.get();
        const int id = o->nextAccountId++;
        try {
            std::string note;
            pj::AccountConfig ac = detail::buildPjAccountConfig(cfg, &note);
#if PJSUA_HAS_VIDEO
            if (o->camDev < 0) o->camDev = o->frontCamera();
            ac.videoConfig.defaultCaptureDevice = (pjmedia_vid_dev_index)o->camDev;   // 셀프뷰 = 전면 카메라
#endif
            auto acc = std::make_unique<PjAccount>(o, id);
            o->accountCfgs[id] = cfg;                        // onIncomingCall 이 읽으므로 create 전에
            acc->create(ac, o->accounts.empty());
            o->accounts[id] = std::move(acc);
            { std::lock_guard<std::mutex> lk(o->snapM); o->regInfos[id] = RegInfo{id, RegState::Unregistered, 0, "", 0}; }
            o->log(3, "account " + std::to_string(id) + " " + cfg.aor() + " via " + cfg.serverHost + ":" +
                          std::to_string(cfg.serverPort) + "/" + toString(cfg.transport) + " user=" + cfg.digestUsername() +
                          " mcptt=" + cfg.effectiveMcpttId() + " " + note);
            return id;
        } catch (pj::Error& e) {
            o->accountCfgs.erase(id);
            o->log(1, std::string("addAccount: ") + e.info(false));
            return -1;
        }
    });
}

static Result withAccount(Engine::Impl* o, int id, const std::function<void(pj::Account&)>& f) {
    return o->ctl.runSync([o, id, f]() -> Result {
        auto it = o->accounts.find(id);
        if (it == o->accounts.end()) return Result::fail(-2, "no such account");
        try { f(*it->second); return Result::success(); } catch (pj::Error& e) { return fromError(e); }
    });
}

Result Engine::registerAccount(int id) {
    if (!impl_->running) return Result::fail(-1, "not running");
    Result r = withAccount(impl_.get(), id, [](pj::Account& a) { a.setRegistration(true); });
    if (r.ok) {
        std::lock_guard<std::mutex> lk(impl_->snapM);
        impl_->regInfos[id].state = RegState::Registering;
    }
    return r;
}
Result Engine::unregisterAccount(int id) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return withAccount(impl_.get(), id, [](pj::Account& a) { a.setRegistration(false); });
}
Result Engine::refreshRegistration(int id) { return registerAccount(id); }

Result Engine::removeAccount(int id) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, id]() -> Result {
        Impl* o = impl_.get();
        if (!o->accounts.erase(id)) return Result::fail(-2, "no such account");
        o->accountCfgs.erase(id);
        std::lock_guard<std::mutex> lk(o->snapM);
        o->regInfos.erase(id);
        return Result::success();
    });
}

RegInfo Engine::regInfo(int id) const {
    std::lock_guard<std::mutex> lk(impl_->snapM);
    auto it = impl_->regInfos.find(id);
    return it == impl_->regInfos.end() ? RegInfo{} : it->second;
}

std::vector<int> Engine::accounts() const {
    std::lock_guard<std::mutex> lk(impl_->snapM);
    std::vector<int> v;
    for (auto& kv : impl_->regInfos) v.push_back(kv.first);
    return v;
}

int Engine::dial(int accountId, const std::string& target, const CallOptions& opts) {
    if (!impl_->running) return -1;
    return impl_->ctl.runSync([this, accountId, target, opts]() -> int {
        Impl* o = impl_.get();
        auto it = o->accounts.find(accountId);
        if (it == o->accounts.end()) { o->log(1, "dial: no such account"); return -1; }
        const std::string dst = detail::normalizeTarget(target, o->accountCfgs[accountId].domain);
        auto call = std::make_unique<PjCall>(o, *it->second, accountId);
        try {
            pj::CallOpParam prm(true);
            prm.opt.audioCount = 1;
            prm.opt.videoCount = opts.video ? 1 : 0;
            call->makeCall(dst, prm);
        } catch (pj::Error& e) {
            o->log(1, std::string("dial ") + dst + ": " + e.info(false));
            return -1;
        }
        const int id = call->getId();
        call->sealCallId(id);
        o->updateCall(id, [&](CallInfo& c) {
            c.accountId = accountId; c.dir = CallDir::Outgoing; c.state = CallState::Outgoing;
            c.remoteUri = dst; c.video = opts.video;
        });
        o->calls[id] = std::move(call);
        o->log(3, "dial " + dst + " → call " + std::to_string(id));
        return id;
    });
}

static Result withCall(Engine::Impl* o, int callId, const std::function<void(PjCall&)>& f) {
    if (!o->running) return Result::fail(-1, "not running");
    return o->ctl.runSync([o, callId, f]() -> Result {
        PjCall* c = o->findCall(callId);
        if (!c) return Result::fail(-2, "no such call");
        try { f(*c); return Result::success(); } catch (pj::Error& e) { return fromError(e); }
    });
}

Result Engine::answer(int callId, const CallOptions& opts) {
    return withCall(impl_.get(), callId, [&](pj::Call& c) {
        pj::CallOpParam prm(true);
        prm.statusCode = PJSIP_SC_OK;
        prm.opt.audioCount = 1;
        prm.opt.videoCount = opts.video ? 1 : 0;
        c.answer(prm);
    });
}
Result Engine::reject(int callId, int statusCode) {
    return withCall(impl_.get(), callId, [&](pj::Call& c) {
        pj::CallOpParam prm;
        prm.statusCode = (pjsip_status_code)statusCode;
        c.hangup(prm);
    });
}
Result Engine::hangup(int callId) {
    return withCall(impl_.get(), callId, [](pj::Call& c) { pj::CallOpParam prm; c.hangup(prm); });
}
Result Engine::hold(int callId) {
    return withCall(impl_.get(), callId, [](pj::Call& c) { pj::CallOpParam prm; c.setHold(prm); });
}
Result Engine::resume(int callId) {
    return withCall(impl_.get(), callId, [](pj::Call& c) {
        pj::CallOpParam prm(true);
        prm.opt.flag |= PJSUA_CALL_UNHOLD;
        c.reinvite(prm);
    });
}
/**
 * 명령이 바꾼 스냅샷을 **앱에 알린다**.
 *
 * `setMuted`·`setListen`·`setRxLevel`·`setCallRoute` 는 `CallInfo` 를 바꾸지만 SIP 상태가 바뀌지 않아
 * `onCallState` 가 뒤따르지 않는다. 알리지 않으면 앱은 명령 전 스냅샷을 그대로 들고 있어 **토글이
 * 화면에 반영되지 않고**, 다음 누름이 같은 값을 다시 보내 해제되지 않는다(관제 앱 음소거 증상).
 * 미디어 이벤트 축으로 낸다 — 상태 전이가 아니라 미디어 배치의 변화이기 때문이다.
 */
static void emitMediaSnapshot(Engine::Impl* o, int callId) {
    CallInfo snap = o->snapshotCall(callId);
    o->emit([o, snap] { o->listener->onCallMedia(snap); });
}

Result Engine::setMuted(int callId, bool muted) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, muted](PjCall& c) {
        o->updateCall(callId, [&](CallInfo& ci) { ci.muted = muted; });
        o->wireMedia(&c, callId);
        emitMediaSnapshot(o, callId);
    });
}
Result Engine::setListen(int callId, bool listen) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, listen](PjCall& c) {
        o->updateCall(callId, [&](CallInfo& ci) { ci.listen = listen; });
        o->wireMedia(&c, callId);
        emitMediaSnapshot(o, callId);
    });
}
Result Engine::setRxLevel(int callId, float level) {
    if (level < 0) return Result::fail(-2, "negative level");
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, level](PjCall& c) {
        o->updateCall(callId, [&](CallInfo& ci) { ci.rxLevel = level; });
        // 오디오가 있으면 곧바로, 없으면(성립 전·보류) 다음 결선(wireMedia)에서 건다.
        if (pj::AudioMedia* aud = o->activeAudio(&c)) aud->adjustTxLevel(level);
        emitMediaSnapshot(o, callId);
    });
}
Result Engine::sendDtmf(int callId, const std::string& digits) {
    return withCall(impl_.get(), callId, [&](pj::Call& c) { c.dialDtmf(digits); });
}

CallInfo Engine::callInfo(int callId) const { return impl_->snapshotCall(callId); }

std::vector<int> Engine::calls() const {
    std::lock_guard<std::mutex> lk(impl_->snapM);
    std::vector<int> v;
    for (auto& kv : impl_->callInfos) v.push_back(kv.first);
    return v;
}

StreamStats Engine::streamStats(int callId) const {
    StreamStats s;
    if (!impl_->running) return s;
    auto finalOf = [this, callId]() {
        std::lock_guard<std::mutex> lk(impl_->snapM);
        auto it = impl_->finalStats.find(callId);
        return it == impl_->finalStats.end() ? StreamStats{} : it->second;
    };
    return impl_->ctl.runSync([this, callId, s, finalOf]() mutable -> StreamStats {
        Impl* o = impl_.get();
        PjCall* c = o->findCall(callId);
        if (!c) return finalOf();
        try {
            unsigned idx = 0;
            if (!o->activeAudio(c, &idx)) return finalOf();
            return Impl::fromPj(c->getStreamStat(idx));
        } catch (...) { return finalOf(); }
    });
}

void Engine::addObserver(Listener* l) { impl_->fanout.add(l); }
void Engine::removeObserver(Listener* l) { impl_->fanout.remove(l); }

Result Engine::setTxSource(const std::string& wavPath) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, wavPath]() -> Result {
        Impl* o = impl_.get();
        std::unique_ptr<pj::AudioMediaPlayer> next;
        if (!wavPath.empty()) {
            try {
                next.reset(new pj::AudioMediaPlayer());
                next->createPlayer(wavPath, 0);              // 0 = 끝나면 처음부터 반복
            } catch (pj::Error& e) { return Result::fail(-1, "tx source: " + e.info(false)); }
        }
        // 지금 원천을 모든 호에서 떼고 바꾼 뒤 다시 결선한다(마이크 ↔ 재생기).
        pj::AudioMedia& mic = o->ep->audDevManager().getCaptureDevMedia();
        for (auto& kv : o->calls) {
            PjCall* c = static_cast<PjCall*>(kv.second.get());
            pj::AudioMedia* aud = nullptr;
            try { aud = o->activeAudio(c); } catch (...) {}
            if (!aud) continue;
            try { if (o->txPlayer) o->txPlayer->stopTransmit(*aud); else mic.stopTransmit(*aud); } catch (pj::Error&) {}
        }
        o->txPlayer = std::move(next);
        for (auto& kv : o->calls) {
            try { o->wireMedia(static_cast<PjCall*>(kv.second.get()), kv.first); } catch (pj::Error&) {}
        }
        o->log(3, wavPath.empty() ? "tx source: microphone" : "tx source: " + wavPath);
        return Result::success();
    });
}

CallQuality Engine::callQuality(int callId) const {
    if (!impl_->running) return CallQuality{};
    auto finalOf = [this, callId]() {
        std::lock_guard<std::mutex> lk(impl_->snapM);
        auto it = impl_->finalQuality.find(callId);
        return it == impl_->finalQuality.end() ? CallQuality{} : it->second;
    };
    return impl_->ctl.runSync([this, callId, finalOf]() -> CallQuality {
        Impl* o = impl_.get();
        PjCall* c = o->findCall(callId);
        if (!c) return finalOf();
        try {
            unsigned idx = 0;
            if (!o->activeAudio(c, &idx)) return finalOf();
            return quality::merge(finalOf(), Impl::measure(c, idx));   // 소멸한 앞 스트림 + 현재 스트림
        } catch (...) { return finalOf(); }
    });
}

// ── MCPTT ──

static int startMcptt(Engine::Impl* o, int accountId, const std::string& id, bool isPrivate, const GroupCallOptions& opts) {
    auto it = o->accounts.find(accountId);
    if (it == o->accounts.end()) { o->log(1, "mcptt: no such account"); return -1; }
    const AccountConfig& cfg = o->accountCfgs[accountId];
    for (auto& kv : o->calls) {                                          // 같은 세션 중복 방지
        PjCall* c = static_cast<PjCall*>(kv.second.get());
        if (c->mcptt && c->mcptt->groupId == id && c->mcptt->isPrivate == isPrivate) return kv.first;
    }
    auto call = std::make_unique<PjCall>(o, *it->second, accountId);
    call->mcptt.reset(new McpttSession);
    call->mcptt->groupId = id;
    call->mcptt->isPrivate = isPrivate;
    call->mcptt->fullDuplex = isPrivate && opts.fullDuplex;
    call->mcptt->listenOnly = opts.listenOnly;
    call->mcptt->emergency = opts.emergency;
    call->mcptt->imminentPeril = opts.imminentPeril && !opts.emergency;       // 긴급이 임박을 대체
    call->mcptt->condMine = opts.emergency || opts.imminentPeril;
    call->mcptt->broadcast = !isPrivate && opts.broadcast;               // 일제 통화는 그룹 호 속성(TS 24.379 §4.12)
    const std::string mcpttId = cfg.effectiveMcpttId();
    // floor 소켓은 makeCall 전에 — makeCall 이 동기적으로 onCallSdpCreated 를 부르며 로컬 offer 에 포트를 광고한다.
    if (!call->mcptt->fullDuplex) {
        if (!call->openFloor(mcpttId)) { o->log(1, "floor socket bind failed"); return -1; }
        if (call->mcptt->broadcast) call->mcptt->floor->setBroadcastInitiator(true);
        // 암묵적 발언 요청(TS 24.380 §14.2.5) — 개시 INVITE 가 요청을 싣고 floor 는 'U: pending Request'(§6.2.4.2.2 4.)
        const bool implicitReq = opts.implicitFloorRequest && !opts.listenOnly;
        if (implicitReq) { call->mcptt->floor->armImplicitRequest(opts.emergency); call->mcptt->implicitAwaitAnswer = true; }
        call->mcptt->pendingAppSdp = mcptt::floorSdp(call->mcptt->floor->localPort(), false, implicitReq);
    } else {
        call->mcptt->micOpen = true;
    }
    try {
        pj::CallOpParam prm(true);
        prm.opt.audioCount = 1;
        prm.opt.videoCount = 0;
        prm.txOption.multipartContentType.type = "multipart";
        prm.txOption.multipartContentType.subType = "mixed";
        pj::SipMultipartPart p1;
        p1.contentType.type = "application"; p1.contentType.subType = "vnd.3gpp.mcptt-info+xml";
        p1.body = mcptt::mcpttInfo(isPrivate ? "private" : "prearranged", "tel:" + id, mcpttId, "tel:" + id,
                                   opts.emergency ? 1 : 0, opts.imminentPeril ? 1 : 0, call->mcptt->broadcast);
        prm.txOption.multipartParts.push_back(p1);
        // 우선 그룹콜의 Resource-Priority(TS 24.379 §6.2.8.1.2·§6.2.8.1.12) — 값 = service-config(§6.2.8.1.15, AccountConfig.rp*)
        if (call->mcptt->emergency || call->mcptt->imminentPeril) {
            pj::SipHeader rp; rp.hName = "Resource-Priority";
            rp.hValue = call->mcptt->emergency ? cfg.rpEmergency : cfg.rpImminentPeril;
            if (!rp.hValue.empty()) prm.txOption.headers.push_back(rp);
        }
        if (!opts.members.empty()) {
            pj::SipMultipartPart p2;
            p2.contentType.type = "application"; p2.contentType.subType = "resource-lists+xml";
            p2.body = mcptt::resourceLists(opts.members);
            prm.txOption.multipartParts.push_back(p2);
        }
        call->makeCall("sip:" + id + "@" + cfg.domain, prm);
        // makeCall 이 개시 offer 를 동기적으로 만들었다 — 이어지는 offer(re-INVITE)·answer 는 암묵 요청·mc_granted 없이(§14.5)
        if (call->mcptt->floor) call->mcptt->pendingAppSdp = mcptt::floorSdp(call->mcptt->floor->localPort(), false);
    } catch (pj::Error& e) {
        o->log(1, std::string("mcptt invite ") + id + ": " + e.info(false));
        return -1;
    }
    const int callId = call->getId();
    call->sealCallId(callId);
    o->updateCall(callId, [&](CallInfo& c) {
        c.accountId = accountId; c.dir = CallDir::Outgoing; c.state = CallState::Outgoing;
        c.remoteUri = "sip:" + id + "@" + cfg.domain;
        call->projectMcptt(c);                                            // onCallState(CALLING) 가 먼저 투영했으면 no-op
    });
    const bool broadcast = call->mcptt->broadcast;
    o->calls[callId] = std::move(call);
    o->log(3, std::string(isPrivate ? "private call " : broadcast ? "broadcast group call " : "group call ") + id + " → call " +
                  std::to_string(callId));
    return callId;
}

int Engine::joinGroupCall(int accountId, const std::string& groupId, const GroupCallOptions& opts) {
    if (!impl_->running) return -1;
    return impl_->ctl.runSync([this, accountId, groupId, opts] { return startMcptt(impl_.get(), accountId, groupId, false, opts); });
}
int Engine::startPrivateCall(int accountId, const std::string& peer, const GroupCallOptions& opts) {
    if (!impl_->running) return -1;
    return impl_->ctl.runSync([this, accountId, peer, opts] { return startMcptt(impl_.get(), accountId, mcptt::bareId(peer), true, opts); });
}

Result Engine::floorRequest(int callId, int priority) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, priority](PjCall& c) {
        if (!c.mcptt) throw pj::Error(PJ_EINVALIDOP, "floorRequest", "not an MCPTT session", __FILE__, __LINE__);
        if (c.mcptt->fullDuplex) return;                                     // 전이중 — 마이크 상시, floor 없음
        if (!c.mcptt->floor) throw pj::Error(PJ_EINVALIDOP, "floorRequest", "no floor participant", __FILE__, __LINE__);
        c.mcptt->floor->request(priority, c.mcptt->emergency);             // 세션 조건 현재값(상향·재광고 반영)
    });
}
Result Engine::floorRelease(int callId) {
    return withCall(impl_.get(), callId, [](PjCall& c) {
        if (c.mcptt && c.mcptt->floor) c.mcptt->floor->release();
    });
}
Result Engine::floorQueueCancel(int callId) {
    return withCall(impl_.get(), callId, [](PjCall& c) {
        if (c.mcptt && c.mcptt->floor) c.mcptt->floor->cancelQueued();
    });
}
FloorInfo Engine::floorInfo(int callId) const {
    FloorInfo fi;
    if (!impl_->running) return fi;
    return impl_->ctl.runSync([this, callId, fi]() mutable {
        PjCall* c = impl_->findCall(callId);
        if (c && c->mcptt && c->mcptt->floor) fi = c->mcptt->floor->info();
        return fi;
    });
}

Result Engine::setCallCondition(int callId, bool emergency, bool imminentPeril) {
    if (emergency && imminentPeril) return Result::fail(-2, "emergency and imminent peril are exclusive");
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, emergency, imminentPeril](PjCall& c) {
        if (!c.mcptt || c.mcptt->isPrivate)
            throw pj::Error(PJ_EINVALIDOP, "setCallCondition", "not a group call", __FILE__, __LINE__);
        McpttSession& m = *c.mcptt;
        if (m.condPending) throw pj::Error(PJ_EBUSY, "setCallCondition", "condition change pending", __FILE__, __LINE__);
        if (o->snapshotCall(callId).state != CallState::Active)
            throw pj::Error(PJ_EINVALIDOP, "setCallCondition", "call not active", __FILE__, __LINE__);
        if (m.emergency == emergency && m.imminentPeril == imminentPeril) return;           // 바뀐 것 없음 — 보내지 않는다
        // 바뀐 지시자만 true/false 로 명시(§6.2.8.1.1·§6.2.8.1.3·§6.2.8.1.9·§6.2.8.1.11)
        const int e = m.emergency == emergency ? 0 : (emergency ? 1 : -1);
        const int i = m.imminentPeril == imminentPeril ? 0 : (imminentPeril ? 1 : -1);
        const AccountConfig& cfg = o->accountCfgs[c.accountId()];
        m.prevEmergency = m.emergency; m.prevImminent = m.imminentPeril; m.prevMine = m.condMine;
        m.emergency = emergency; m.imminentPeril = imminentPeril;
        m.condMine = emergency || imminentPeril;
        m.condPending = true;
        m.condLastCode = 0;
        c.publishCondition(ConditionCause::Local);
        try {
            pj::CallOpParam prm(true);
            prm.opt.audioCount = 1;
            prm.opt.videoCount = 0;
            prm.txOption.multipartContentType.type = "multipart";
            prm.txOption.multipartContentType.subType = "mixed";
            pj::SipMultipartPart p1;
            p1.contentType.type = "application"; p1.contentType.subType = "vnd.3gpp.mcptt-info+xml";
            p1.body = mcptt::mcpttInfo("prearranged", "tel:" + m.groupId, cfg.effectiveMcpttId(), "tel:" + m.groupId, e, i);
            prm.txOption.multipartParts.push_back(p1);
            pj::SipHeader rp; rp.hName = "Resource-Priority";                           // §6.2.8.1.2 — 하향은 normal 값(§6.2.8.1.15)
            rp.hValue = emergency ? cfg.rpEmergency : imminentPeril ? cfg.rpImminentPeril : cfg.rpNormal;
            if (!rp.hValue.empty()) prm.txOption.headers.push_back(rp);
            c.reinvite(prm);                                                             // SDP = 협상 그대로 + floor 섹션 재주입
        } catch (pj::Error&) {
            m.emergency = m.prevEmergency; m.imminentPeril = m.prevImminent; m.condMine = m.prevMine;
            m.condPending = false;
            c.publishCondition(ConditionCause::Denied);
            throw;
        }
        o->log(3, "call " + std::to_string(callId) + " condition → emergency=" + (emergency ? "1" : "0") +
                      " imminent=" + (imminentPeril ? "1" : "0"));
    });
}

int64_t Engine::sendEmergencyAlert(int accountId, const std::string& groupId, bool activate,
                                   const std::string& originatedBy, bool cancelGroupEmergency) {
    if (!impl_->running) return -1;
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=]() -> int64_t {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return -1;
        const AccountConfig& cfg = ic->second;
        const std::string gid = mcptt::bareId(groupId);
        // ICSI mcptt(§12.1.1.1 1)·2)). Request-URI 는 그룹 — 이 CSP 는 To(그룹)로 경보를 게이트·팬아웃한다
        //   (규격 = 참여 기능 PSI + 본문 mcptt-request-uri — 편차는 mcptt_emergency_modes.md §4.3).
        std::map<std::string, std::string> h;
        h["P-Preferred-Service"] = mcptt::kIcsiMcptt;
        h["Accept-Contact"] = std::string("*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcptt\";require;explicit");
        std::string ob = originatedBy.empty() ? std::string()
                       : (originatedBy.find(':') == std::string::npos ? "tel:" + originatedBy : originatedBy);
        std::string body = mcptt::alertInfo("tel:" + gid, cfg.effectiveMcpttId(), cfg.effectiveMcpttClientId(), activate, ob,
                                            (!activate && cancelGroupEmergency) ? -1 : 0);
        return o->doSendRequest(accountId, "MESSAGE", "sip:" + gid + "@" + cfg.domain, mcptt::kCtMcpttInfo, body, h, token);
    });
}

int64_t Engine::sendRequest(int accountId, const std::string& method, const std::string& targetUri,
                            const std::string& contentType, const std::string& body,
                            const std::map<std::string, std::string>& headers) {
    if (!impl_->running) return -1;
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=] { return impl_->doSendRequest(accountId, method, targetUri, contentType, body, headers, token); });
}

int64_t Engine::Impl::sendAffiliation(int accountId, const std::string& groupId, bool on, int64_t token, int64_t appToken,
                                      bool allowConditional) {
    auto ic = accountCfgs.find(accountId);
    if (ic == accountCfgs.end()) return -1;
    std::map<std::string, std::string> h;
    h["Event"] = "mcptt";                                              // TS 24.379 §9 — 없으면 CSP 489
    h["Expires"] = on ? "3600" : "0";
    {
        std::lock_guard<std::mutex> lk(snapM);
        PendingPublish p;
        p.accountId = accountId; p.groupId = groupId; p.on = on; p.appToken = appToken;
        auto et = publishEtag.find(std::to_string(accountId) + ":" + groupId);
        if (allowConditional && et != publishEtag.end()) { h["SIP-If-Match"] = et->second; p.conditional = true; }
        publishPending[token] = p;
    }
    int64_t r = doSendRequest(accountId, "PUBLISH", "sip:" + groupId + "@" + ic->second.domain, mcptt::kCtAffiliation,
                              mcptt::affiliationCommand("tel:" + groupId, on), h, token);
    if (r < 0) { std::lock_guard<std::mutex> lk(snapM); publishPending.erase(token); }
    return r < 0 ? -1 : appToken;
}

int64_t Engine::affiliate(int accountId, const std::string& groupId, bool on) {
    if (!impl_->running) return -1;
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=]() -> int64_t { return impl_->sendAffiliation(accountId, groupId, on, token, token, true); });
}

Result Engine::subscribeConference(int accountId, const std::string& groupId, bool on) {
    if (!impl_->running) return Result::fail(-1, "not running");
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=]() -> Result {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return Result::fail(-2, "no such account");
        std::map<std::string, std::string> h{{"Event", "conference"}, {"Expires", on ? "3600" : "0"}};
        int64_t r = o->doSendRequest(accountId, "SUBSCRIBE", "sip:" + groupId + "@" + ic->second.domain, "", "", h, token);
        return r < 0 ? Result::fail(-3, "subscribe failed") : Result::success();
    });
}

Result Engine::subscribeXcapDiff(int accountId, const std::string& psiUri, bool on) {
    if (!impl_->running) return Result::fail(-1, "not running");
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=]() -> Result {
        std::map<std::string, std::string> h{{"Event", "xcap-diff"}, {"Expires", on ? "3600" : "0"}};
        int64_t r = impl_->doSendRequest(accountId, "SUBSCRIBE", psiUri, "", "", h, token);
        return r < 0 ? Result::fail(-3, "subscribe failed") : Result::success();
    });
}

// ── 관제 ──

Result Engine::dialogWatch(int accountId, const std::string& targetAor, bool on) {
    if (!impl_->running) return Result::fail(-1, "not running");
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=]() -> Result {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return Result::fail(-2, "no such account");
        std::map<std::string, std::string> h{{"Event", "dialog"}, {"Expires", on ? "3600" : "0"}};
        int64_t r = o->doSendRequest(accountId, "SUBSCRIBE", detail::normalizeTarget(targetAor, ic->second.domain), "", "", h, token);
        return r < 0 ? Result::fail(-3, "subscribe failed") : Result::success();
    });
}

int Engine::join(int accountId, const std::string& targetUri, const DialogInfo& dlg) {
    if (!impl_->running || dlg.callId.empty()) return -1;
    return impl_->ctl.runSync([this, accountId, targetUri, dlg]() -> int {
        Impl* o = impl_.get();
        auto it = o->accounts.find(accountId);
        if (it == o->accounts.end()) return -1;
        const std::string dst = detail::normalizeTarget(targetUri, o->accountCfgs[accountId].domain);
        auto call = std::make_unique<PjCall>(o, *it->second, accountId);
        call->recvOnly = true;
        try {
            pj::CallOpParam prm(true);
            prm.opt.audioCount = 1;
            prm.opt.videoCount = 0;
            pj::SipHeader hj; hj.hName = "Join"; hj.hValue = dlg.joinHeader();
            pj::SipHeader hs; hs.hName = "Supported"; hs.hValue = "join";
            prm.txOption.headers.push_back(hj);
            prm.txOption.headers.push_back(hs);
            call->makeCall(dst, prm);
        } catch (pj::Error& e) {
            o->log(1, std::string("join ") + dst + ": " + e.info(false));
            return -1;
        }
        const int id = call->getId();
        call->sealCallId(id);
        o->updateCall(id, [&](CallInfo& c) {
            c.accountId = accountId; c.dir = CallDir::Outgoing; c.state = CallState::Outgoing;
            c.remoteUri = dst; c.listenOnly = true; c.joinedDialog = dlg.callId;
        });
        o->calls[id] = std::move(call);
        o->log(3, "join " + dst + " (Join: " + dlg.joinHeader() + ") → call " + std::to_string(id));
        return id;
    });
}

int Engine::pickup(int accountId, const std::string& featureCode, const std::string& number) {
    if (featureCode.empty()) return -1;
    return dial(accountId, featureCode + number);
}

Result Engine::transfer(int callId, const std::string& target) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, target](PjCall& c) {
        int acc = o->snapshotCall(callId).accountId;
        std::string dst = detail::normalizeTarget(target, o->accountCfgs[acc].domain);
        pj::CallOpParam prm;
        c.xfer(dst, prm);
    });
}

Result Engine::transferAttended(int callId, int consultCallId) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, consultCallId](PjCall& c) {
        PjCall* d = o->findCall(consultCallId);
        if (!d) throw pj::Error(PJ_ENOTFOUND, "transferAttended", "no consult call", __FILE__, __LINE__);
        pj::CallOpParam prm;
        c.xferReplaces(*d, prm);
    });
}

// ── MCData media plane SDS (MSRP, TS 24.282 §9.2.3 — mcdata/msrp.h) ──

void Engine::Impl::runMsrpThread(std::shared_ptr<std::atomic<bool>> cancel, std::function<void()> body) {
    {
        std::lock_guard<std::mutex> lk(msrpM);
        msrpActive++;
        msrpCancels.erase(std::remove_if(msrpCancels.begin(), msrpCancels.end(),
                                         [](const std::weak_ptr<std::atomic<bool>>& w) { return w.expired(); }), msrpCancels.end());
        msrpCancels.push_back(cancel);
    }
    std::thread([this, body] {
        try { body(); } catch (...) {}
        std::lock_guard<std::mutex> lk(msrpM);
        msrpActive--;
        msrpCv.notify_all();
    }).detach();
}

/** 입출력이 끝난 MSRP 호 — 서버가 저장·전달 뒤 BYE 한다(mcdata_messaging.md §4.7). 5 s 안에 끊기지 않으면 우리가 끊는다. */
static void finishMsrpCall(Engine::Impl* o, int callId, const std::atomic<bool>& cancel) {
    for (int i = 0; i < 25 && !cancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (cancel) return;
    o->ctl.post([o, callId] {
        PjCall* c = o->findCall(callId);
        if (!c) return;
        try { pj::CallOpParam p; c->hangup(p); } catch (pj::Error&) {}
    });
}

void Engine::Impl::startMsrpSend(int callId, int accountId, const MsrpLeg& leg) {
    auto cancel = leg.cancel;
    const std::string sp = leg.serverPath, lp = leg.localPath, sig = leg.signallingTlv, pay = leg.payloadTlv;
    const int64_t token = leg.token;
    runMsrpThread(cancel, [this, callId, accountId, cancel, sp, lp, sig, pay, token] {
        std::string err;
        const int code = sp.empty() ? 488 : msrp::sendSds(sp, lp, sig, pay, 10, *cancel, nullptr, err);
        log(3, "msrp send call " + std::to_string(callId) + " " + sp + " → " + std::to_string(code) + (err.empty() ? "" : " (" + err + ")"));
        RequestResult r;
        r.accountId = accountId; r.token = token; r.method = "MSRP"; r.code = code;
        r.reason = !err.empty() ? err : code == 200 ? "OK" : sp.empty() ? "no a=path in answer" : "";
        emit([this, r] { listener->onRequestResult(r); });
        finishMsrpCall(this, callId, *cancel);
    });
}

void Engine::Impl::startMsrpRecv(int callId, int accountId, const MsrpLeg& leg) {
    auto cancel = leg.cancel;
    const std::string sp = leg.serverPath, lp = leg.localPath, from = leg.fromUri, group = leg.groupUri;
    runMsrpThread(cancel, [this, callId, accountId, cancel, sp, lp, from, group] {
        std::string ct, body, err;
        if (!sp.empty() && msrp::receiveSds(sp, lp, 15, *cancel, ct, body, err)) {
            SdsMessage m;
            if (mcdata::parse(ct, body, m)) {
                m.accountId = accountId;
                m.fromUri = from;
                if (m.groupUri.empty()) m.groupUri = group;       // 본문에 mcdata-info 가 없다 — 배포 INVITE 의 것
                m.mediaPlane = true;
                log(3, "msrp recv call " + std::to_string(callId) + " msg=" + m.msgId + " bytes=" + std::to_string(m.text.size()));
                emit([this, m] { listener->onSds(m); });
            } else {
                log(2, "msrp recv call " + std::to_string(callId) + ": body is not MCData SDS (" + ct + ")");
            }
        } else {
            log(2, "msrp recv call " + std::to_string(callId) + " " + sp + ": " + (sp.empty() ? "no a=path" : err));
        }
        finishMsrpCall(this, callId, *cancel);
    });
}

bool Engine::Impl::startMsrpInvite(int accountId, const std::string& groupId, int64_t token, const std::string& sigTlv,
                                   const std::string& payTlv) {
    auto it = accounts.find(accountId);
    auto ic = accountCfgs.find(accountId);
    if (it == accounts.end() || ic == accountCfgs.end()) return false;
    const AccountConfig& cfg = ic->second;
    auto call = std::make_unique<PjCall>(this, *it->second, accountId);
    call->msrp.reset(new MsrpLeg);
    call->msrp->outgoing = true;
    call->msrp->token = token;
    call->msrp->signallingTlv = sigTlv;
    call->msrp->payloadTlv = payTlv;
    try {
        pj::CallOpParam prm(true);
        prm.opt.audioCount = 1;                                          // 더미 오디오(서버는 포트 9 inactive 로 답한다)
        prm.opt.videoCount = 0;
        auto hdr = [&](const char* n, const std::string& v) { pj::SipHeader h; h.hName = n; h.hValue = v; prm.txOption.headers.push_back(h); };
        hdr("Accept-Contact", std::string("*;+g.3gpp.icsi-ref=\"urn%3Aurn-7%3A3gpp-service.ims.icsi.mcdata.sds\";require;explicit"));
        hdr("P-Preferred-Service", msrp::kIcsiMcDataSds);
        hdr("P-Preferred-Identity", "<" + cfg.aor() + ">");
        call->makeCall("sip:" + groupId + "@" + cfg.domain, prm);        // offer = pjsua audio + m=message(onCallSdpCreated)
    } catch (pj::Error& e) {
        log(1, "msrp invite " + groupId + ": " + e.info(false));
        return false;
    }
    const int callId = call->getId();
    log(3, "msrp sds " + groupId + " (" + std::to_string(payTlv.size()) + " bytes) → call " + std::to_string(callId));
    calls[callId] = std::move(call);
    return true;
}

SdsSend Engine::sendGroupSds(int accountId, const std::string& groupId, const std::string& text, bool requestDelivery) {
    SdsSend out;
    if (!impl_->running) { out.code = -1; out.reason = "not running"; return out; }
    if (text.empty())    { out.code = -2; out.reason = "empty text";  return out; }
    std::string msgId = mcdata::newMessageId();
    int64_t token = impl_->nextToken++;
    out.token = token;
    bool ok = impl_->ctl.runSync([=]() -> bool {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return false;
        const int64_t now = (int64_t)std::time(nullptr);
        // 시그널링 평면 상한을 넘으면 media plane(MSRP) — 서버는 초과 MESSAGE 를 403 Warning 203 으로 거절한다(TS 24.282 §9.2.2 8)).
        const int cap = ic->second.maxSdsCplaneBytes;
        if (cap > 0 && (int)text.size() > cap)
            return o->startMsrpInvite(accountId, groupId, token,
                                      mcdata::sdsSignallingTlv(mcdata::conversationIdOf(groupId), msgId, requestDelivery, now),
                                      mcdata::sdsPayloadTlv(text));
        mcdata::Body b = mcdata::buildGroupSds("tel:" + groupId, text, mcdata::conversationIdOf(groupId), msgId, requestDelivery, now);
        return o->doSendRequest(accountId, "MESSAGE", "sip:" + groupId + "@" + ic->second.domain, b.contentType, b.body, {}, token) >= 0;
    });
    if (!ok) { out.code = -3; out.reason = "send failed"; return out; }
    out.ok = true;
    out.msgId = msgId;
    return out;
}

SdsSend Engine::sendSds(int accountId, const std::string& peer, const std::string& text, bool requestDelivery) {
    SdsSend out;
    if (!impl_->running) { out.code = -1; out.reason = "not running"; return out; }
    if (text.empty())    { out.code = -2; out.reason = "empty text";  return out; }
    std::string to = mcptt::bareId(peer);
    if (to.empty())      { out.code = -2; out.reason = "empty peer";  return out; }
    std::string msgId = mcdata::newMessageId();
    int64_t token = impl_->nextToken++;
    out.token = token;
    bool ok = impl_->ctl.runSync([=]() -> bool {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return false;
        // conversation ID 는 **나와 상대의 쌍**으로 짓는다 — 상대가 답장할 때 같은 값이 나와야 한 대화다.
        std::string me = mcptt::bareId(ic->second.effectiveMcpttId());
        mcdata::Body b = mcdata::buildOneToOneSds("tel:" + to, text, mcdata::conversationIdOneToOne(me, to),
                                                  msgId, requestDelivery, (int64_t)std::time(nullptr));
        return o->doSendRequest(accountId, "MESSAGE", "sip:" + to + "@" + ic->second.domain, b.contentType, b.body, {}, token) >= 0;
    });
    if (!ok) { out.code = -3; out.reason = "send failed"; return out; }
    out.ok = true;
    out.msgId = msgId;
    return out;
}

SdsSend Engine::sendGroupFd(int accountId, const std::string& groupId, const FdFile& file) {
    SdsSend out;
    if (!impl_->running) { out.code = -1; out.reason = "not running"; return out; }
    if (file.url.empty()) { out.code = -2; out.reason = "empty file url"; return out; }
    std::string msgId = mcdata::newMessageId();
    int64_t token = impl_->nextToken++;
    out.token = token;
    bool ok = impl_->ctl.runSync([=]() -> bool {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return false;
        // 그룹 SDS 와 같은 대화(conversation ID) — 파일도 그 그룹 스레드에 놓인다.
        mcdata::Body b = mcdata::buildGroupFd("tel:" + groupId, file, mcdata::conversationIdOf(groupId), msgId,
                                              (int64_t)std::time(nullptr));
        return o->doSendRequest(accountId, "MESSAGE", "sip:" + groupId + "@" + ic->second.domain, b.contentType, b.body, {}, token) >= 0;
    });
    if (!ok) { out.code = -3; out.reason = "send failed"; return out; }
    out.ok = true;
    out.msgId = msgId;
    return out;
}

SdsSend Engine::sendFd(int accountId, const std::string& peer, const FdFile& file) {
    SdsSend out;
    if (!impl_->running) { out.code = -1; out.reason = "not running"; return out; }
    if (file.url.empty()) { out.code = -2; out.reason = "empty file url"; return out; }
    std::string to = mcptt::bareId(peer);
    if (to.empty())       { out.code = -2; out.reason = "empty peer";     return out; }
    std::string msgId = mcdata::newMessageId();
    int64_t token = impl_->nextToken++;
    out.token = token;
    bool ok = impl_->ctl.runSync([=]() -> bool {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return false;
        std::string me = mcptt::bareId(ic->second.effectiveMcpttId());
        mcdata::Body b = mcdata::buildOneToOneFd("tel:" + to, file, mcdata::conversationIdOneToOne(me, to), msgId,
                                                 (int64_t)std::time(nullptr));
        return o->doSendRequest(accountId, "MESSAGE", "sip:" + to + "@" + ic->second.domain, b.contentType, b.body, {}, token) >= 0;
    });
    if (!ok) { out.code = -3; out.reason = "send failed"; return out; }
    out.ok = true;
    out.msgId = msgId;
    return out;
}

SdsSend Engine::sendSdsNotification(int accountId, const std::string& peer, const std::string& convId,
                                    const std::string& msgId, int notifType) {
    SdsSend out;
    if (!impl_->running) { out.code = -1; out.reason = "not running"; return out; }
    int64_t token = impl_->nextToken++;
    out.token = token;
    Result r = impl_->ctl.runSync([=]() -> Result {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return Result::fail(-2, "no such account");
        mcdata::Body b = mcdata::buildNotification(convId, msgId, notifType, (int64_t)std::time(nullptr));
        int64_t rc = o->doSendRequest(accountId, "MESSAGE", "sip:" + mcptt::bareId(peer) + "@" + ic->second.domain, b.contentType, b.body, {}, token);
        return rc < 0 ? Result::fail(-3, "send failed") : Result::success();
    });
    out.ok = r.ok; out.code = r.code; out.reason = r.reason;
    return out;
}

std::vector<AudioDeviceInfo> Engine::audioDevices() const {
    std::vector<AudioDeviceInfo> v;
    if (!impl_->running) return v;
    return impl_->ctl.runSync([this, v]() mutable {
        try {
            for (auto& d : impl_->ep->audDevManager().enumDev2()) {
                AudioDeviceInfo i; i.id = d.id; i.name = d.name; i.driver = d.driver;
                i.inputCount = d.inputCount; i.outputCount = d.outputCount;
                v.push_back(i);
            }
        } catch (...) {}
        return v;
    });
}

Result Engine::refreshAudioDevices() {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this]() -> Result {
        try { impl_->ep->audDevManager().refreshDevs(); return Result::success(); }
        catch (pj::Error& e) { return fromError(e); }
    });
}

Result Engine::setAudioDevices(int captureDev, int playbackDev) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, captureDev, playbackDev]() -> Result {
        try {
            impl_->ep->audDevManager().setCaptureDev(captureDev);
            impl_->ep->audDevManager().setPlaybackDev(playbackDev);
            return Result::success();
        } catch (pj::Error& e) { return fromError(e); }
    });
}

Result Engine::setCaptureEnabled(bool on) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, on]() -> Result {
        Impl* o = impl_.get();
        if (o->captureOn == on) return Result::success();
        // null 장치는 모드를 받지 않고 다시 만들어질 뿐이다(pjsua_set_snd_dev2) — 상태만 둔다.
        if (!o->cfg.nullAudioDevice) {
            // 모드는 장치 선택을 넘어 유지된다(setCaptureDev·setPlaybackDev 가 pjsua_get_snd_dev2 로 현재 모드를 이어받는다).
            // NO_IMMEDIATE_OPEN — 장치가 닫혀 있으면 열지 않고 모드만, 열려 있으면 곧바로 다시 연다(브리지 결선은 유지).
            const unsigned mode = (on ? 0u : (unsigned)PJSUA_SND_DEV_SPEAKER_ONLY) | (unsigned)PJSUA_SND_DEV_NO_IMMEDIATE_OPEN;
            try { o->ep->audDevManager().setSndDevMode(mode); }
            catch (pj::Error& e) { return fromError(e); }
        }
        o->captureOn = on;
        o->applyDeviceLevels();                              // 재오픈은 slot 0 레벨을 초기화한다
        o->log(4, std::string("capture ") + (on ? "enabled (full duplex)" : "disabled (speaker only)"));
        return Result::success();
    });
}

Result Engine::setDeviceAudioLevels(float speaker, double micTargetDbov) {
    if (!impl_->running) return Result::fail(-1, "not running");
    if (!(speaker >= 0.f)) return Result::fail(-1, "speaker level");
    return impl_->ctl.runSync([this, speaker, micTargetDbov]() -> Result {
        Impl* o = impl_.get();
        o->spkLevel = speaker;
        o->micTarget = std::min(-10.0, std::max(-40.0, micTargetDbov));   // 엔진 목표 범위(§4)
        o->levelsSet = true;
        o->applyDeviceLevels();
        o->log(4, "device levels speaker=" + std::to_string(speaker) + " mic_target=" + std::to_string(o->micTarget) + " dBov");
        return Result::success();
    });
}

static pjmedia_aud_dev_route pjRoute(AudioRoute r) {
    switch (r) {
        case AudioRoute::Earpiece: return PJMEDIA_AUD_DEV_ROUTE_EARPIECE;
        case AudioRoute::Loudspeaker: return PJMEDIA_AUD_DEV_ROUTE_LOUDSPEAKER;
        default: return PJMEDIA_AUD_DEV_ROUTE_DEFAULT;
    }
}

Result Engine::setAudioRoute(AudioRoute output, AudioRoute input) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, output, input]() -> Result {
        Impl* o = impl_.get();
        if (o->cfg.nullAudioDevice) return Result::success();   // null 장치는 라우트 능력이 없다
        pj::AudDevManager& adm = o->ep->audDevManager();
        try { adm.setOutputRoute(pjRoute(output), true); }
        catch (pj::Error& e) { return fromError(e); }
        // 입력 라우트를 모르는 백엔드도 있다 — 출력은 이미 걸렸으므로 실패는 기록만(keep = 발언마다 장치가 다시 열려도 유지)
        try { adm.setInputRoute(pjRoute(input), true); }
        catch (pj::Error& e) { o->log(3, std::string("input route: ") + e.info(false)); }
        return Result::success();
    });
}

Result Engine::reopenAudioDevice() {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this]() -> Result {
        Impl* o = impl_.get();
        if (o->cfg.nullAudioDevice) return Result::success();
        pj::AudDevManager& adm = o->ep->audDevManager();
        if (!adm.sndIsActive()) return Result::success();       // 닫혀 있다 — 다음 개방이 새 트랙이다
        // 같은 장치·같은 모드면 pjsua_set_snd_dev2 는 "No changes" 로 돌아간다(열려 있거나 NO_IMMEDIATE_OPEN 이면). 모드에서
        // NO_IMMEDIATE_OPEN 만 빼 값이 달라지게 하면 장치를 닫고 곧바로 다시 연다 — 게이트(SPEAKER_ONLY)는 그대로 둔다.
        const unsigned mode = o->captureOn ? 0u : (unsigned)PJSUA_SND_DEV_SPEAKER_ONLY;
        try { adm.setSndDevMode(mode); }
        catch (pj::Error& e) { return fromError(e); }
        o->applyDeviceLevels();
        o->log(3, "sound device reopened");
        return Result::success();
    });
}

bool Engine::captureEnabled() const { return impl_->captureOn; }

Result Engine::setVideoWindow(void* nativeWindow) {
#if PJSUA_HAS_VIDEO
    if (!impl_->running) { windowRelease(nativeWindow); return Result::fail(-1, "not running"); }
    return impl_->ctl.runSync([this, nativeWindow]() -> Result {
        Impl* o = impl_.get();
        void* old = nullptr;
        {
            std::lock_guard<std::mutex> lk(o->videoM);
            old = o->videoWindow;
            o->videoWindow = nativeWindow;
        }
        // 활성 영상 호에 곧바로 — 해제(nullptr)면 렌더러에서 창을 뗀다(렌더러가 자기 참조를 푼다).
        for (auto& kv : o->calls) {
            auto* call = static_cast<PjCall*>(kv.second.get());
            if (nativeWindow) { o->attachVideo(call, -1); continue; }
            try {
                for (auto& m : call->getInfo().media) {
                    if (m.type != PJMEDIA_TYPE_VIDEO || m.status != PJSUA_CALL_MEDIA_ACTIVE) continue;
                    pj::VideoWindow vw = m.videoWindow;
                    pj::VideoWindowHandle h;
                    h.handle.window = nullptr;
                    vw.setWindow(h);
                }
            } catch (...) {}
        }
        if (old && old != nativeWindow) windowRelease(old);
        return Result::success();
    });
#else
    windowRelease(nativeWindow);                         // 넘겨받은 참조 — 쓰지 않으니 바로 돌려준다
    return Result::fail(-3, "video not built");
#endif
}

Result Engine::switchCamera(int callId) {
#if PJSUA_HAS_VIDEO
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, callId]() -> Result {
        Impl* o = impl_.get();
        PjCall* call = o->findCall(callId);
        if (!call) return Result::fail(-2, "no such call");
        std::vector<int> cams = o->cameras();
        if (cams.size() < 2) return Result::fail(-3, "single camera");
        if (o->camDev < 0) o->camDev = o->frontCamera();
        int next = cams[0];
        for (size_t i = 0; i < cams.size(); ++i)
            if (cams[i] == o->camDev) { next = cams[(i + 1) % cams.size()]; break; }
        bool done = false;
        try {
            for (auto& m : call->getInfo().media) {
                if (m.type != PJMEDIA_TYPE_VIDEO || m.status != PJSUA_CALL_MEDIA_ACTIVE) continue;
                pj::CallVidSetStreamParam p;
                p.medIdx = (int)m.index;
                p.capDev = (pjmedia_vid_dev_index)next;
                call->vidSetStream(PJSUA_CALL_VID_STRM_CHANGE_CAP_DEV, p);
                done = true;
            }
        } catch (pj::Error& e) { return fromError(e); }
        if (!done) return Result::fail(-4, "no active video");
        o->camDev = next;
        o->log(3, "camera -> " + std::to_string(next));
        return Result::success();
    });
#else
    (void)callId;
    return Result::fail(-3, "video not built");
#endif
}

std::vector<VideoDeviceInfo> Engine::videoDevices() const {
    std::vector<VideoDeviceInfo> v;
#if PJSUA_HAS_VIDEO
    if (!impl_->running) return v;
    return impl_->ctl.runSync([this]() {
        std::vector<VideoDeviceInfo> out;
        try {
            for (auto& d : impl_->ep->vidDevManager().enumDev2()) {
                VideoDeviceInfo i;
                i.id = d.id; i.name = d.name; i.driver = d.driver;
                i.capture = (d.dir & PJMEDIA_DIR_CAPTURE) != 0;
                i.render = (d.dir & PJMEDIA_DIR_RENDER) != 0;
                out.push_back(i);
            }
        } catch (...) {}
        return out;
    });
#else
    return v;
#endif
}

int Engine::addPlaybackRoute(int playbackDev) {
    if (!impl_->running) return -1;
    return impl_->ctl.runSync([this, playbackDev]() -> int {
        Impl* o = impl_.get();
        try {
            // recDev = PJMEDIA_AUD_INVALID_DEV → 재생 전용(엔진 패치, ExtraAudioDevice::open). 브리지 포맷(16k mono) 으로 연다.
            std::unique_ptr<pj::ExtraAudioDevice> dev(new pj::ExtraAudioDevice(playbackDev, PJMEDIA_AUD_INVALID_DEV));
            dev->open();
            int id = o->nextRouteId++;
            o->routes[id] = std::move(dev);
            o->log(3, "playback route " + std::to_string(id) + " ← dev " + std::to_string(playbackDev));
            return id;
        } catch (pj::Error& e) {
            o->log(1, "addPlaybackRoute dev " + std::to_string(playbackDev) + ": " + e.info(false));
            return -1;
        }
    });
}

Result Engine::removePlaybackRoute(int routeId) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, routeId]() -> Result {
        Impl* o = impl_.get();
        auto it = o->routes.find(routeId);
        if (it == o->routes.end()) return Result::fail(-1, "no such route");
        // 이 라우트에 붙은 호는 기본 장치로 되돌리고 재결선 — 결선을 먼저 끊은 뒤 장치를 닫는다
        for (auto& kv : o->calls) {
            int callId = kv.first;
            if (o->snapshotCall(callId).playbackRoute != routeId) continue;
            o->updateCall(callId, [](CallInfo& c) { c.playbackRoute = 0; });
            try { o->wireMedia(static_cast<PjCall*>(kv.second.get()), callId); } catch (pj::Error&) {}
        }
        o->routes.erase(it);                     // ~ExtraAudioDevice → close
        return Result::success();
    });
}

Result Engine::setCallRoute(int callId, int routeId) {
    if (!impl_->running) return Result::fail(-1, "not running");
    return impl_->ctl.runSync([this, callId, routeId]() -> Result {
        Impl* o = impl_.get();
        if (routeId != 0 && o->routes.find(routeId) == o->routes.end()) return Result::fail(-1, "no such route");
        PjCall* c = o->findCall(callId);
        if (!c) return Result::fail(-1, "no such call");
        CallInfo snap;
        o->updateCall(callId, [routeId](CallInfo& ci) { ci.playbackRoute = routeId; }, &snap);
        if (snap.mediaActive) {
            try { o->wireMedia(c, callId); } catch (pj::Error& e) { return fromError(e); }
        }
        emitMediaSnapshot(o, callId);
        return Result::success();
    });
}

}  // namespace cimsue
