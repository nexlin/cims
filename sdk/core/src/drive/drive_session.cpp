// DriveSession — 계측기 구동 줄 프로토콜 해석기 (cimsue/drive.h, ue_voice_quality.md §5.3, ue_sdk.md §4.7).
//   cimsue-cli drive(stdin/stdout)와 DeviceLink(TLS 계측 링크)가 이 한 구현을 쓴다. Engine 관찰자로 이벤트를 받아 줄로 내고,
//   명령 줄을 Engine 명령으로 옮긴다. 시각(rrd/srd/sdd·floor t_us)은 이 프로세스 안에서 잰다.
#include "cimsue/drive.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace cimsue {

namespace drive {

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

std::string statsFields(const StreamStats& st) {
    return ",\"rx_pkts\":" + std::to_string(st.rxPackets) + ",\"tx_pkts\":" + std::to_string(st.txPackets) + ",\"rx_loss\":" + std::to_string(st.rxLoss) +
           ",\"rx_bytes\":" + std::to_string(st.rxBytes) + ",\"jitter_us\":" + std::to_string(st.rxJitterUs) + ",\"stats_valid\":" + (st.valid ? "true" : "false");
}

std::string qualityFields(const CallQuality& q) {
    if (!q.valid) return "";
    auto num = [](double v) { char b[32]; std::snprintf(b, sizeof(b), v < 0 ? "%.0f" : "%.2f", v < 0 ? -1.0 : v); return std::string(b); };
    return ",\"codec\":\"" + jsonEscape(q.codec) + "\",\"discard\":" + std::to_string(q.rx.discarded) + ",\"loss_pct\":" + num(q.rx.lossPct) +
           ",\"discard_pct\":" + num(q.rx.discardPct) + ",\"jitter_max_ms\":" + num(q.rx.jitterMaxMs) +
           ",\"remote_loss_pct\":" + num(q.remote.valid ? q.remote.lossPct : -1) + ",\"remote_jitter_ms\":" + num(q.remote.valid ? q.remote.jitterMs : -1) +
           ",\"rtd_ms\":" + num(q.rtdMs) + ",\"esd_ms\":" + num(q.esdMs) + ",\"one_way_ms\":" + num(q.oneWayMs) +
           ",\"r_lq\":" + num(q.rLq) + ",\"r_cq\":" + num(q.rCq) + ",\"mos_lq\":" + num(q.mosLq) + ",\"mos_cq\":" + num(q.mosCq);
}

}  // namespace drive

namespace {

using Clock = std::chrono::steady_clock;
long long nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
long long msSince(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }
const char* b(bool v) { return v ? "true" : "false"; }

}  // namespace

struct DriveSession::Impl : public Listener {
    Engine& eng;
    LineSink& out;
    DriveOptions opt;
    int acc = -1;                          // 현재 회선(use)
    std::atomic<bool> quit{false};
    std::atomic<bool> running{false};
    std::thread stats;
    std::mutex stopM;                      // stats 스레드 깨우기
    std::condition_variable stopCv;

    std::mutex m;
    std::map<int, Clock::time_point> tReg;          // accountId → register 명령 시각
    std::map<int, Clock::time_point> tDial, tHangup;
    struct Tok { std::string op; bool on; Clock::time_point t; };
    std::map<int64_t, Tok> tokens;
    std::set<int> active;                  // 진행 중 호(stats 대상)
    std::set<int> driven;                  // 이 세션이 만들거나 받은 호(Hangup::Driven)

    Impl(Engine& e, LineSink& s, const DriveOptions& o) : eng(e), out(s), opt(o) {
        if (!opt.accounts.empty()) acc = opt.accounts.front().accountId;
    }

    std::string serviceOf(int accountId) const {
        for (auto& a : opt.accounts) if (a.accountId == accountId) return a.service;
        return "";
    }
    void emit(const std::string& line) { out.writeLine(line); }
    void result(const std::string& op, bool ok, int call, int code, const std::string& reason) {
        emit("{\"event\":\"result\",\"op\":\"" + op + "\",\"ok\":" + b(ok) + ",\"call\":" + std::to_string(call) + ",\"code\":" + std::to_string(code) +
             ",\"reason\":\"" + drive::jsonEscape(reason) + "\"}");
    }
    void res(const std::string& op, const Result& r, int call = -1) { result(op, r.ok, call, r.code, r.reason); }
    void markDial(int id) { std::lock_guard<std::mutex> lk(m); tDial[id] = Clock::now(); active.insert(id); driven.insert(id); }

    // ── Listener (이벤트 스레드) ──
    void onRegState(const RegInfo& r) override {
        long long ms = -1;
        {
            std::lock_guard<std::mutex> lk(m);
            auto it = tReg.find(r.accountId);
            if (it != tReg.end()) { ms = msSince(it->second); if (r.state != RegState::Registering) tReg.erase(it); }
        }
        const char* st = r.state == RegState::Registered ? "registered" : r.state == RegState::Failed ? "failed" : r.state == RegState::Registering ? "registering" : "unregistered";
        emit("{\"event\":\"reg\",\"service\":\"" + serviceOf(r.accountId) + "\",\"state\":\"" + st + "\",\"code\":" + std::to_string(r.code) + ",\"reason\":\"" +
             drive::jsonEscape(r.reason) + "\",\"expires\":" + std::to_string(r.expiresSec) + ",\"rrd_ms\":" + std::to_string(ms) + "}");
    }
    void onServiceAuth(const ServiceAuthInfo& i) override {
        emit("{\"event\":\"service_auth\",\"service\":\"" + std::string(toString(i.service)) + "\",\"state\":\"" + toString(i.state) +
             "\",\"code\":" + std::to_string(i.code) + ",\"warning\":" + std::to_string(i.warningCode) + ",\"multiple_devices\":" +
             b(i.multipleDevices) + "}");
    }
    void onIncomingCall(const CallInfo& c) override {
        { std::lock_guard<std::mutex> lk(m); active.insert(c.callId); }
        emit("{\"event\":\"incoming\",\"call\":" + std::to_string(c.callId) + ",\"from\":\"" + drive::jsonEscape(c.remoteUri) + "\",\"called\":\"" +
             drive::jsonEscape(c.calledParty) + "\",\"video\":" + b(c.video) + ",\"mcptt\":" + b(c.isMcptt) + ",\"service\":\"" + toString(c.service) +
             "\",\"group\":\"" + drive::jsonEscape(c.groupId) + "\"}");
    }
    void onCallState(const CallInfo& c) override {
        const char* st = c.state == CallState::Outgoing ? "outgoing" : c.state == CallState::Incoming ? "incoming" : c.state == CallState::Active ? "active"
                       : c.state == CallState::Held ? "held" : c.state == CallState::Disconnected ? "disconnected" : "null";
        std::string extra;
        bool byUs = false;
        {
            std::lock_guard<std::mutex> lk(m);
            auto d = tDial.find(c.callId);
            if (c.state == CallState::Active && d != tDial.end()) { extra += ",\"srd_ms\":" + std::to_string(msSince(d->second)); tDial.erase(d); }
            if (c.state == CallState::Disconnected) {
                auto h = tHangup.find(c.callId);
                if (h != tHangup.end()) { byUs = true; extra += ",\"sdd_ms\":" + std::to_string(msSince(h->second)); tHangup.erase(h); }
                tDial.erase(c.callId);
                active.erase(c.callId);
                driven.erase(c.callId);
            }
        }
        std::string q;
        if (c.state == CallState::Disconnected) {
            CallQuality cq = eng.callQuality(c.callId);                                  // 소멸 시점의 최종 통계·품질
            extra += drive::statsFields(eng.streamStats(c.callId)) + drive::qualityFields(cq);
            if (cq.valid) q = "{\"event\":\"quality\",\"call\":" + std::to_string(c.callId) + ",\"kind\":\"callTerm\"" + drive::qualityFields(cq) + "}";
        }
        emit("{\"event\":\"call\",\"call\":" + std::to_string(c.callId) + ",\"dir\":\"" + (c.dir == CallDir::Outgoing ? "out" : "in") + "\",\"state\":\"" + st +
             "\",\"code\":" + std::to_string(c.lastCode) + ",\"reason\":\"" + drive::jsonEscape(c.lastReason) + "\",\"media\":" + b(c.mediaActive) +
             ",\"mcptt\":" + b(c.isMcptt) + ",\"service\":\"" + toString(c.service) + "\",\"video\":" + b(c.video) + ",\"by_us\":" + b(byUs) +
             ",\"group\":\"" + drive::jsonEscape(c.groupId) + "\"" + extra + "}");
        if (!q.empty()) emit(q);
    }
    void onFloor(const FloorEvent& ev) override {
        // TS 24.380 §8.2 subtype 로도 낸다(워커가 가상 단말과 같은 표로 센다) — Granted 1 · Taken 2 · Deny 3 · Idle 5 · Revoke 6 · Queue Position Info 9
        const char* k = "other"; int sub = -1;
        switch (ev.kind) {
        case FloorEvent::Kind::Granted: k = "granted"; sub = 1; break;
        case FloorEvent::Kind::Taken: k = "taken"; sub = 2; break;
        case FloorEvent::Kind::Denied: k = "denied"; sub = 3; break;
        case FloorEvent::Kind::Idle: k = "idle"; sub = 5; break;
        case FloorEvent::Kind::Revoked: k = "revoked"; sub = 6; break;
        case FloorEvent::Kind::QueuePosition: k = "queue"; sub = 9; break;
        case FloorEvent::Kind::QueueCancelled: k = "queue_cancelled"; break;
        case FloorEvent::Kind::RequestTimeout: k = "request_timeout"; break;
        case FloorEvent::Kind::TalkerLeft: k = "talker_left"; break;
        case FloorEvent::Kind::TalkLimit: k = "talk_limit"; break;
        default: break;
        }
        emit("{\"event\":\"floor\",\"call\":" + std::to_string(ev.callId) + ",\"kind\":\"" + k + "\",\"subtype\":" + std::to_string(sub) + ",\"t_us\":" +
             std::to_string(nowUs()) + ",\"cause\":" + std::to_string(ev.cause) + ",\"queue_position\":" + std::to_string(ev.queuePosition) + ",\"duration\":" +
             std::to_string(ev.durationSec) + "}");
    }
    void onTransmission(const TransmissionEvent& ev) override {
        emit("{\"event\":\"transmission\",\"call\":" + std::to_string(ev.callId) + ",\"kind\":\"" + toString(ev.kind) + "\",\"state\":\"" +
             toString(ev.state) + "\",\"cause\":" + std::to_string(ev.cause) + ",\"t_us\":" + std::to_string(nowUs()) + "}");
    }
    void onReception(const ReceptionEvent& ev) override {
        emit("{\"event\":\"reception\",\"call\":" + std::to_string(ev.callId) + ",\"kind\":\"" + toString(ev.kind) + "\",\"from\":\"" +
             drive::jsonEscape(ev.transmitter.userId) + "\",\"state\":\"" + toString(ev.transmitter.state) + "\",\"auto\":" + b(ev.transmitter.automatic) +
             ",\"cause\":" + std::to_string(ev.cause) + ",\"t_us\":" + std::to_string(nowUs()) + "}");
    }
    void onRoster(int, const std::string& g, const std::vector<RosterEntry>& users, bool full) override {
        emit("{\"event\":\"roster\",\"group\":\"" + drive::jsonEscape(g) + "\",\"full\":" + b(full) + ",\"users\":" + std::to_string(users.size()) + "}");
    }
    void onDialogInfo(const DialogInfo& d) override {
        emit("{\"event\":\"dialog\",\"watched\":\"" + drive::jsonEscape(d.watched) + "\",\"state\":\"" + d.state + "\",\"call_id\":\"" + drive::jsonEscape(d.callId) +
             "\",\"direction\":\"" + d.direction + "\",\"remote\":\"" + drive::jsonEscape(d.remoteIdentity) + "\"}");
    }
    void onSds(const SdsMessage& msg) override {
        emit("{\"event\":\"sds\",\"from\":\"" + drive::jsonEscape(msg.fromUri) + "\",\"group\":\"" + drive::jsonEscape(msg.groupUri) + "\",\"msg_id\":\"" +
             drive::jsonEscape(msg.msgId) + "\",\"notification\":" + b(msg.notification) + ",\"text\":\"" + drive::jsonEscape(msg.text) + "\"}");
    }
    void onRequestResult(const RequestResult& r) override {
        std::string op, on;
        long long ms = -1;
        {
            std::lock_guard<std::mutex> lk(m);
            auto it = tokens.find(r.token);
            if (it != tokens.end()) { op = it->second.op; on = b(it->second.on); ms = msSince(it->second.t); tokens.erase(it); }
            else if (opt.appOwnedRegistration) return;      // 링크: 앱이 낸 요청(PUBLISH 등)의 결과는 앱 몫
            else on = "null";
        }
        emit("{\"event\":\"request\",\"method\":\"" + drive::jsonEscape(r.method) + "\",\"op\":\"" + op + "\",\"on\":" + on + ",\"code\":" + std::to_string(r.code) +
             ",\"reason\":\"" + drive::jsonEscape(r.reason) + "\",\"ms\":" + std::to_string(ms) + ",\"token\":" + std::to_string((long long)r.token) + "}");
    }
    void onEngineStopped() override { emit("{\"event\":\"engine_stopped\"}"); }

    std::vector<int> activeCalls() { std::lock_guard<std::mutex> lk(m); return std::vector<int>(active.begin(), active.end()); }

    void statsLoop() {
        std::unique_lock<std::mutex> lk(stopM);
        while (running) {
            stopCv.wait_for(lk, std::chrono::milliseconds(opt.statsIntervalMs > 0 ? opt.statsIntervalMs : 1000), [this] { return !running.load(); });
            if (!running) break;
            lk.unlock();
            for (int id : activeCalls()) {
                StreamStats st = eng.streamStats(id);
                if (st.valid) emit("{\"event\":\"stats\",\"call\":" + std::to_string(id) + drive::statsFields(st) + drive::qualityFields(eng.callQuality(id)) + "}");
            }
            lk.lock();
        }
    }

    void handle(const std::string& line) {
        std::vector<std::string> tk;
        { std::stringstream ss(line); std::string t; while (ss >> t) tk.push_back(t); }
        if (tk.empty()) return;
        const std::string& op = tk[0];
        auto arg = [&](size_t i) { return i < tk.size() ? tk[i] : std::string(); };
        auto argi = [&](size_t i, int def) { return i < tk.size() ? std::atoi(tk[i].c_str()) : def; };
        auto has = [&](const char* flag) { for (size_t i = 1; i < tk.size(); ++i) if (tk[i] == flag) return true; return false; };
        if (op == "quit") { quit = true; result(op, true, -1, 0, ""); }
        else if (op == "register" || op == "unregister") {
            if (opt.appOwnedRegistration) { result(op, false, -1, 0, "app_owned"); return; }
            if (op == "register") { { std::lock_guard<std::mutex> lk(m); tReg[acc] = Clock::now(); } res(op, eng.registerAccount(acc)); }
            else res(op, eng.unregisterAccount(acc));
        } else if (op == "use") {
            for (auto& a : opt.accounts) if (a.service == arg(1)) { acc = a.accountId; result(op, true, -1, 0, ""); return; }
            result(op, false, -1, 0, "no_account");
        } else if (op == "dial") {
            CallOptions co; co.video = has("video");
            int id = eng.dial(acc, arg(1), co);
            if (id >= 0) markDial(id);
            result(op, id >= 0, id, 0, id >= 0 ? "" : "dial refused");
        } else if (op == "answer") {
            CallOptions co; co.video = has("video");
            Result r = eng.answer(argi(1, -1), co);
            if (r.ok) { std::lock_guard<std::mutex> lk(m); driven.insert(argi(1, -1)); }
            res(op, r, argi(1, -1));
        } else if (op == "reject") { res(op, eng.reject(argi(1, -1), argi(2, 486)), argi(1, -1)); }
        else if (op == "hangup") { { std::lock_guard<std::mutex> lk(m); tHangup[argi(1, -1)] = Clock::now(); } res(op, eng.hangup(argi(1, -1)), argi(1, -1)); }
        else if (op == "hold") { res(op, eng.hold(argi(1, -1)), argi(1, -1)); }
        else if (op == "resume") { res(op, eng.resume(argi(1, -1)), argi(1, -1)); }
        else if (op == "dtmf") { res(op, eng.sendDtmf(argi(1, -1), arg(2)), argi(1, -1)); }
        else if (op == "transfer") { res(op, eng.transfer(argi(1, -1), arg(2)), argi(1, -1)); }
        else if (op == "group_call") {
            GroupCallOptions go; go.listenOnly = has("listen"); go.emergency = has("emergency"); go.broadcast = has("broadcast");
            go.implicitFloorRequest = has("implicit");                   // 암묵적 발언 요청(TS 24.380 §14.2.5)
            int id = eng.joinGroupCall(acc, arg(1), go);
            if (id >= 0) markDial(id);
            result(op, id >= 0, id, 0, id >= 0 ? "" : "group call refused");
        } else if (op == "video_call") {
            // MCVideo 그룹 호(TS 24.281 §9.2.1·§9.2.2) — chat 합류가 곧 affiliation(§8.1). prearranged 팬아웃을 받으려면 먼저
            //   `affiliate <group> on mcvideo`(§8.2)
            VideoGroupCallOptions vo; vo.prearranged = has("prearranged"); vo.queueing = has("queueing");
            vo.implicitTransmissionRequest = has("implicit");
            int id = eng.joinVideoGroupCall(acc, arg(1), vo);
            if (id >= 0) markDial(id);
            result(op, id >= 0, id, 0, id >= 0 ? "" : "video call refused");
        } else if (op == "transmit_request") { res(op, eng.requestTransmission(argi(1, -1), argi(2, -1)), argi(1, -1)); }
        else if (op == "transmit_release") { res(op, eng.releaseTransmission(argi(1, -1)), argi(1, -1)); }
        else if (op == "reception_accept") { res(op, eng.acceptReception(argi(1, -1), arg(2)), argi(1, -1)); }
        else if (op == "reception_end") { res(op, eng.endReception(argi(1, -1), arg(2)), argi(1, -1)); }
        else if (op == "floor_request") { res(op, eng.floorRequest(argi(1, -1)), argi(1, -1)); }
        else if (op == "floor_release") { res(op, eng.floorRelease(argi(1, -1)), argi(1, -1)); }
        else if (op == "affiliate") {
            bool on = arg(2) != "off";
            // 서비스 — 기본 MCPTT, `mcvideo` 면 MCVideo affiliation(관심 그룹 전부를 한 PUBLISH 로, TS 24.281 §8.2.1.2)
            const McService svc = has("mcvideo") ? McService::McVideo : McService::Mcptt;
            int64_t tok = eng.affiliate(acc, arg(1), on, svc);
            if (tok >= 0) { std::lock_guard<std::mutex> lk(m); tokens[tok] = { op, on, Clock::now() }; }
            result(op, tok >= 0, -1, 0, tok >= 0 ? "" : "affiliate refused");
        } else if (op == "mcvideo") {
            // MCVideo 서비스만 켜고 끈다 — 등록은 유지한 채 Contact 의 MCVideo 태그만 바꾼 REGISTER(TS 24.281 §7.2.1AA NOTE)
            res(op, eng.setMcVideoEnabled(acc, arg(1) != "off"));
        } else if (op == "pickup") {
            int id = eng.pickup(acc, arg(1), arg(2));
            if (id >= 0) markDial(id);
            result(op, id >= 0, id, 0, id >= 0 ? "" : "pickup refused");
        } else if (op == "media") {
            if (arg(1) == "mic") res(op, eng.setTxSource(""));
            else if (arg(1) == "sample") {
                std::string f = arg(2).empty() ? opt.sampleFile : arg(2);
                if (f.empty()) result(op, false, -1, 0, "no_sample");
                else res(op, eng.setTxSource(f));
            } else result(op, false, -1, 0, "mic|sample");
        } else if (op == "stats") {
            std::vector<int> ids = tk.size() > 1 ? std::vector<int>{ argi(1, -1) } : activeCalls();
            for (int id : ids) emit("{\"event\":\"stats\",\"call\":" + std::to_string(id) + drive::statsFields(eng.streamStats(id)) + drive::qualityFields(eng.callQuality(id)) + "}");
            result(op, true, -1, 0, "");
        } else if (op == "quality") {
            CallQuality q = eng.callQuality(argi(1, -1));
            if (q.valid) emit("{\"event\":\"quality\",\"call\":" + std::to_string(argi(1, -1)) + ",\"kind\":\"snapshot\"" + drive::qualityFields(q) + "}");
            result(op, q.valid, argi(1, -1), 0, q.valid ? "" : "no_quality");
        } else result(op, false, -1, 0, "unknown command");
    }
};

DriveSession::DriveSession(Engine& engine, LineSink& sink, const DriveOptions& opts) : impl_(new Impl(engine, sink, opts)) {}
DriveSession::~DriveSession() { stop(); }

void DriveSession::start(bool emitReady) {
    if (impl_->running.exchange(true)) return;
    impl_->eng.addObserver(impl_.get());
    if (emitReady) {
        std::string aor = impl_->opt.accounts.empty() ? std::string() : impl_->opt.accounts.front().aor;
        impl_->emit("{\"event\":\"ready\",\"version\":\"" + drive::jsonEscape(Engine::version()) + "\",\"aor\":\"" + drive::jsonEscape(aor) + "\"}");
    }
    impl_->stats = std::thread([this] { impl_->statsLoop(); });
}

void DriveSession::handleLine(const std::string& line) { impl_->handle(line); }
bool DriveSession::quitRequested() const { return impl_->quit.load(); }

void DriveSession::stop() {
    if (!impl_->running.exchange(false)) return;
    { std::lock_guard<std::mutex> lk(impl_->stopM); }
    impl_->stopCv.notify_all();
    if (impl_->stats.joinable()) impl_->stats.join();
    impl_->eng.removeObserver(impl_.get());
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        if (impl_->opt.hangupOnStop == DriveOptions::Hangup::All) ids.assign(impl_->active.begin(), impl_->active.end());
        else if (impl_->opt.hangupOnStop == DriveOptions::Hangup::Driven) ids.assign(impl_->driven.begin(), impl_->driven.end());
    }
    for (int id : ids) impl_->eng.hangup(id);
}

}  // namespace cimsue
