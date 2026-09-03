// libcimsue — Engine 구현 (ue_sdk.md §4.3 스레딩·수명 규칙)
//
//  - 모든 pjsua2 호출은 제어 스레드 `ue-ctl` 에서만 한다. libCreate 도 이 스레드에서 하므로 pjlib 의
//    "메인 스레드" 가 곧 ue-ctl 이다. 공개 명령은 runSync 로 ue-ctl 에 넘기고 결과를 받아 돌려준다.
//  - pjsua 콜백(pjsip 워커 스레드)은 상태 스냅샷을 갱신하고 이벤트를 큐에 넣기만 한다. 리스너는
//    이벤트 스레드 `ue-evt` 가 부른다 — 리스너 안에서 명령을 다시 불러도(ue-ctl 로 감) 교착 없음.
//  - pj::Account/pj::Call 은 엔진이 강참조 테이블로 보관하고 DISCONNECTED 뒤 ue-ctl 에서 해제한다.
//    콜백 안에서 자기 객체를 지우지 않는다.
#include "cimsue/engine.h"

#include <pjsua2.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "account_map.h"

#define CIMSUE_VERSION "0.1.0"

namespace cimsue {

namespace {

/** 단일 워커 스레드 + 작업 큐. */
class Worker {
public:
    void start(const std::string& name, std::function<void()> onStart = nullptr) {
        stop_ = false;
        th_ = std::thread([this, name, onStart] {
            if (onStart) onStart();
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
    bool isCurrent() const { return std::this_thread::get_id() == th_.get_id(); }

private:
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
};

Result fromError(const pj::Error& e) { return Result::fail((int)e.status, e.info(false)); }

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct Engine::Impl;

namespace {
class PjAccount;
class PjCall;
class PjLog;
}  // namespace

struct Engine::Impl {
    EngineConfig cfg;
    Listener* listener = nullptr;
    std::atomic<bool> running{false};

    Worker ctl;                 // ue-ctl — pjsua2 전용
    Worker evt;                 // ue-evt — 리스너 전용
    std::unique_ptr<pj::Endpoint> ep;
    /** pjsua2 소유 — libInit 에 넘긴 뒤에는 Endpoint::libDestroy 가 delete 한다(여기서 지우면 이중 해제). */
    pj::LogWriter* logWriter = nullptr;
    std::string domainDefault;

    // ue-ctl 에서만 접근
    std::map<int, std::unique_ptr<pj::Account>> accounts;
    std::map<int, AccountConfig> accountCfgs;
    std::map<int, std::unique_ptr<pj::Call>> calls;        // pjsua call id → Call
    int nextAccountId = 0;

    // 스냅샷 — 콜백(pjsip 스레드)이 쓰고 조회(임의 스레드)가 읽는다
    std::mutex snapM;
    std::map<int, RegInfo> regInfos;
    std::map<int, CallInfo> callInfos;                     // 종료된 호도 잠시 보존(조회·최종 통계) — pruneFinished
    std::map<int, StreamStats> finalStats;                 // onStreamDestroyed 시점의 최종 RTP 통계
    static constexpr size_t kKeepFinished = 64;
    void pruneFinished() {                                 // snapM 잡은 상태에서 호출
        while (callInfos.size() > kKeepFinished) {
            auto it = callInfos.begin();
            for (; it != callInfos.end(); ++it) if (it->second.state == CallState::Disconnected) break;
            if (it == callInfos.end()) break;
            finalStats.erase(it->first);
            callInfos.erase(it);
        }
    }
    static StreamStats fromPj(const pj::StreamStat& st) {
        StreamStats s;
        s.rxPackets = st.rtcp.rxStat.pkt; s.rxBytes = st.rtcp.rxStat.bytes;
        s.rxLoss = st.rtcp.rxStat.loss; s.rxDiscard = st.rtcp.rxStat.discard;
        s.txPackets = st.rtcp.txStat.pkt; s.txBytes = st.rtcp.txStat.bytes;
        s.valid = true;
        return s;
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
    void applyCodecPolicy();
    pj::Call* findCall(int callId) {
        auto it = calls.find(callId);
        return it == calls.end() ? nullptr : it->second.get();
    }
    pj::AudioMedia* activeAudio(pj::Call* call, unsigned* idxOut = nullptr) {
        pj::CallInfo ci = call->getInfo();
        for (auto& m : ci.media) {
            if (m.type == PJMEDIA_TYPE_AUDIO && m.status == PJSUA_CALL_MEDIA_ACTIVE) {
                if (idxOut) *idxOut = m.index;
                return pj::AudioMedia::typecastFromMedia(call->getMedia(m.index));
            }
        }
        return nullptr;
    }
    void wireMedia(pj::Call* call, int callId);
};

namespace {

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

    void onCallState(pj::OnCallStateParam&) override {
        pj::CallInfo ci = getInfo();
        const int id = getId();
        CallInfo snap;
        bool changed = false;
        o_->updateCall(id, [&](CallInfo& c) {
            c.accountId = accountId_;
            c.remoteUri = ci.remoteUri;
            c.lastCode = ci.lastStatusCode;
            c.lastReason = ci.lastReason;
            CallState ns = c.state;
            switch (ci.state) {
                case PJSIP_INV_STATE_CALLING:
                case PJSIP_INV_STATE_EARLY:
                    // 착신(UAS)의 EARLY(자동 180)는 Incoming 을 덮어쓰지 않는다 — 발신(UAC)만 Outgoing.
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
            // 콜백 안에서 자기 객체를 지우지 않는다 — ue-ctl 에서 해제.
            o_->ctl.post([o = o_, id] {
                o->calls.erase(id);
                std::lock_guard<std::mutex> lk(o->snapM);
                o->pruneFinished();
            });
        }
    }

    /** 스트림 소멸 직전 — 이 시점이 RTP 통계를 읽을 수 있는 마지막 기회다(DISCONNECTED 콜백에서는 이미 없다). */
    void onStreamDestroyed(pj::OnStreamDestroyedParam& prm) override {
        try {
            StreamStats s = Engine::Impl::fromPj(getStreamStat(prm.streamIdx));
            std::lock_guard<std::mutex> lk(o_->snapM);
            o_->finalStats[getId()] = s;
        } catch (...) {}
    }

    void onCallMediaState(pj::OnCallMediaStateParam&) override {
        const int id = getId();
        pj::CallInfo ci = getInfo();
        bool held = false, active = false;
        for (auto& m : ci.media) {
            if (m.type != PJMEDIA_TYPE_AUDIO) continue;
            if (m.status == PJSUA_CALL_MEDIA_ACTIVE) active = true;
            if (m.status == PJSUA_CALL_MEDIA_LOCAL_HOLD || m.status == PJSUA_CALL_MEDIA_REMOTE_HOLD) held = true;
        }
        try { if (active) o_->wireMedia(this, id); } catch (pj::Error& e) { o_->log(2, std::string("wireMedia: ") + e.info(false)); }
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
        else if (!active && prm.code / 100 == 2) ri.state = RegState::Unregistered;   // de-REGISTER 200
        else ri.state = RegState::Failed;
        { std::lock_guard<std::mutex> lk(o_->snapM); o_->regInfos[accountId_] = ri; }
        o_->emit([o = o_, ri] { o->listener->onRegState(ri); });
    }

    void onIncomingCall(pj::OnIncomingCallParam& prm) override {
        // Call 객체 생성은 pjsua 콜백 안에서 해야 한다(call id 가 이 시점에만 유효). 테이블 삽입은
        // ue-ctl 소유 자료구조라 ue-ctl 로 넘긴다 — 그 사이 pjsua 는 이 call 을 우리 것으로 안다.
        auto* call = new PjCall(o_, *this, accountId_, prm.callId);
        std::string whole;
        try { whole = prm.rdata.wholeMsg; } catch (...) {}
        std::string remote;
        try { remote = call->getInfo().remoteUri; } catch (...) {}
        CallInfo snap;
        o_->updateCall(prm.callId, [&](CallInfo& c) {
            c.accountId = accountId_;
            c.dir = CallDir::Incoming;
            c.state = CallState::Incoming;
            c.remoteUri = remote;
            c.video = whole.find("m=video") != std::string::npos;
            c.calledParty = detail::uriUser(detail::headerValue(whole, "P-Called-Party-ID"));
        }, &snap);
        o_->ctl.post([o = o_, call, id = prm.callId] { o->calls[id].reset(call); });
        // 180 Ringing 은 코어가 즉시 — 200 OK 는 앱의 answer().
        try {
            pj::CallOpParam p;
            p.statusCode = PJSIP_SC_RINGING;
            call->answer(p);
        } catch (pj::Error& e) { o_->log(2, std::string("180 failed: ") + e.info(false)); }
        o_->emit([o = o_, snap] { o->listener->onIncomingCall(snap); });
    }

private:
    Engine::Impl* o_;
    int accountId_;
};

}  // namespace

// ── Impl 헬퍼 ──

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
}

void Engine::Impl::wireMedia(pj::Call* call, int callId) {
    // conference bridge 결선 — 호 → 스피커(listen), 마이크 → 호(!muted). 장치 미디어는 Endpoint 소유라
    // 보관하지 않고 매번 재취득한다.
    pj::AudioMedia* aud = activeAudio(call);
    if (!aud) return;
    CallInfo snap = snapshotCall(callId);
    pj::AudDevManager& adm = ep->audDevManager();
    pj::AudioMedia& spk = adm.getPlaybackDevMedia();
    pj::AudioMedia& mic = adm.getCaptureDevMedia();
    if (snap.listen) aud->startTransmit(spk); else aud->stopTransmit(spk);
    if (!snap.muted) mic.startTransmit(*aud); else mic.stopTransmit(*aud);
}

// ── Engine 공개 API ──

Engine::Engine() : impl_(new Impl) {}
Engine::~Engine() { stop(); }

std::string Engine::version() { return std::string(CIMSUE_VERSION) + " (pjproject " + pj_get_version() + ")"; }

bool Engine::running() const { return impl_->running; }

Result Engine::start(const EngineConfig& cfg, Listener* listener) {
    if (impl_->running) return Result::fail(-1, "already running");
    impl_->cfg = cfg;
    impl_->listener = listener;
    impl_->evt.start("ue-evt");
    impl_->ctl.start("ue-ctl");
    Result r = impl_->ctl.runSync([this]() -> Result {
        Impl* o = impl_.get();
        try {
            pj_log_set_level(o->cfg.logLevel);               // libInit 전(writer 미설정) pjlib 기본 sink 는 stdout
            o->ep.reset(new pj::Endpoint);
            o->ep->libCreate();
            pj::EpConfig epc;
            epc.uaConfig.userAgent = o->cfg.userAgent;
            epc.logConfig.level = o->cfg.logLevel;
            epc.logConfig.consoleLevel = o->cfg.logLevel;         // pjsua 는 앱 writer 호출도 console_level 로 게이트한다
            std::unique_ptr<pj::LogWriter> writer(new PjLog(o));
            epc.logConfig.writer = writer.get();
            epc.medConfig.noVad = o->cfg.noVad;
            epc.medConfig.clockRate = o->cfg.clockRate;
            o->ep->libInit(epc);
            o->logWriter = writer.release();                       // 이제 pjsua2 소유
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
    impl_->ctl.runSync([this] {
        Impl* o = impl_.get();
        o->calls.clear();                        // ~Call → hangup
        o->accounts.clear();                     // ~Account → shutdown(de-REGISTER 시도)
        try { o->ep->libDestroy(); } catch (...) {}               // LogWriter 도 여기서 pjsua2 가 delete
        o->logWriter = nullptr;
        o->ep.reset();
        o->running = false;
        return 0;
    });
    impl_->emit([o = impl_.get()] { o->listener->onEngineStopped(); });
    impl_->ctl.stop();
    impl_->evt.stop();
    std::lock_guard<std::mutex> lk(impl_->snapM);
    impl_->regInfos.clear();
    impl_->callInfos.clear();
    impl_->finalStats.clear();
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
            auto acc = std::make_unique<PjAccount>(o, id);
            acc->create(ac, o->accounts.empty());
            o->accounts[id] = std::move(acc);
            o->accountCfgs[id] = cfg;
            if (o->domainDefault.empty()) o->domainDefault = cfg.domain;
            { std::lock_guard<std::mutex> lk(o->snapM); o->regInfos[id] = RegInfo{id, RegState::Unregistered, 0, "", 0}; }
            o->log(3, "account " + std::to_string(id) + " " + cfg.aor() + " via " + cfg.serverHost + ":" +
                          std::to_string(cfg.serverPort) + "/" + toString(cfg.transport) + " user=" + cfg.digestUsername() + " " + note);
            return id;
        } catch (pj::Error& e) {
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
        o->updateCall(id, [&](CallInfo& c) {
            c.accountId = accountId; c.dir = CallDir::Outgoing; c.state = CallState::Outgoing;
            c.remoteUri = dst; c.video = opts.video;
        });
        o->calls[id] = std::move(call);
        o->log(3, "dial " + dst + " → call " + std::to_string(id));
        return id;
    });
}

static Result withCall(Engine::Impl* o, int callId, const std::function<void(pj::Call&)>& f) {
    if (!o->running) return Result::fail(-1, "not running");
    return o->ctl.runSync([o, callId, f]() -> Result {
        pj::Call* c = o->findCall(callId);
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
Result Engine::setMuted(int callId, bool muted) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, muted](pj::Call& c) {
        o->updateCall(callId, [&](CallInfo& ci) { ci.muted = muted; });
        o->wireMedia(&c, callId);
    });
}
Result Engine::setListen(int callId, bool listen) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, listen](pj::Call& c) {
        o->updateCall(callId, [&](CallInfo& ci) { ci.listen = listen; });
        o->wireMedia(&c, callId);
    });
}
Result Engine::setRxLevel(int callId, float level) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, level](pj::Call& c) {
        pj::AudioMedia* aud = o->activeAudio(&c);
        if (!aud) throw pj::Error(PJ_EINVALIDOP, "setRxLevel", "no active audio", __FILE__, __LINE__);
        aud->adjustRxLevel(level);
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
        pj::Call* c = o->findCall(callId);
        if (!c) return finalOf();                                  // 종료된 호 — 소멸 시점의 최종 통계
        try {
            unsigned idx = 0;
            if (!o->activeAudio(c, &idx)) return finalOf();
            return Impl::fromPj(c->getStreamStat(idx));
        } catch (...) { return finalOf(); }
    });
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

}  // namespace cimsue
