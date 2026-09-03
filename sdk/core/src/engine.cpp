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

#include "account_map.h"
#include "floor/floor_participant.h"
#include "mcdata/sds_codec.h"
#include "mcptt/mcptt_xml.h"

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

/** SDP m=application 섹션 텍스트 (floor 평면, ptt_ue.md). */
std::string floorSdp(int localPort, bool fullDuplex) {
    return "m=application " + std::to_string(localPort) + " UDP MCPTT\r\n"
           "a=floorid:0 mstrm:audio\r\n" +
           std::string(fullDuplex ? "a=fmtp:MCPTT mc_queueing;mc_no_floor_ctrl" : "a=fmtp:MCPTT mc_queueing");
}

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

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct Engine::Impl {
    EngineConfig cfg;
    Listener* listener = nullptr;
    std::atomic<bool> running{false};

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
    int nextRouteId = 1;
    int nextAccountId = 0;
    std::atomic<int64_t> nextToken{1};

    // 스냅샷 — 콜백(pjsip 스레드)이 쓰고 조회(임의 스레드)가 읽는다
    std::mutex snapM;
    std::map<int, RegInfo> regInfos;
    std::map<int, CallInfo> callInfos;                     // 종료된 호도 잠시 보존(조회·최종 통계) — pruneFinished
    std::map<int, StreamStats> finalStats;                 // onStreamDestroyed 시점의 최종 RTP 통계
    std::map<int64_t, std::pair<int, std::string>> publishPending;   // token → (accountId, groupId)
    std::map<std::string, std::string> publishEtag;               // "accountId:group" → SIP-ETag
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
    PjCall* findCall(int callId);
    static bool rxOnlyLeg(PjCall* call);
    pj::AudioMedia* activeAudio(PjCall* call, unsigned* idxOut = nullptr);
    void wireMedia(PjCall* call, int callId);
    int64_t doSendRequest(int accountId, const std::string& method, const std::string& targetUri,
                       const std::string& contentType, const std::string& body,
                       const std::map<std::string, std::string>& headers, int64_t token);
};

namespace {

/** MCPTT 세션(그룹콜/사설콜) 부속 상태 — PjCall 소유. */
struct McpttSession {
    std::string groupId;                 // bare id (그룹) 또는 상대 번호(사설콜)
    bool isPrivate = false;
    bool fullDuplex = false;             // mc_no_floor_ctrl — floor 없이 마이크 상시
    bool listenOnly = false;
    bool micOpen = false;                // floor Granted 로 열림
    std::string pendingAppSdp;           // 송신 SDP 에 주입할 m=application 섹션
    std::unique_ptr<floor::Participant> floor;
    bool remoteLearned = false;
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
    bool recvOnly = false;               // 감청 Join 등 청취 전용 평문 leg (a=recvonly, 마이크 없음)
    int accountId() const { return accountId_; }

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
        mcptt->floor.reset(new floor::Participant(-1, ssrcOf(userId), userId, cb));
        if (!mcptt->floor->open(0)) { mcptt->floor.reset(); return false; }
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
            if (prm.e.body.tsxState.type != PJSIP_EVENT_RX_MSG) return;
            const std::string& msg = prm.e.body.tsxState.src.rdata.wholeMsg;
            if (msg.empty()) return;
            if (mcptt && msg.rfind("SIP/2.0 2", 0) == 0 && msg.find("m=application") != std::string::npos)
                learnFloorRemote(sipBody(msg));                                               // UAC: 200 OK answer
            if (msg.rfind("SIP/2.0 2", 0) == 0 && msg.find("a=ssrc:") != std::string::npos) {
                // 감청 leg 200 OK — a=ssrc label:caller/callee (RFC 5576) → 소스 귀속(U10 디먹스 라벨)
                std::vector<MediaSource> src = mcptt::sdpSsrcLabels(sipBody(msg));
                if (!src.empty()) {
                    CallInfo snap;
                    o_->updateCall(getId(), [&](CallInfo& c) { c.sources = src; }, &snap);
                    o_->emit([o = o_, snap] { o->listener->onCallMedia(snap); });
                }
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
        const bool rxOnly = recvOnly || (mcptt && mcptt->listenOnly);
        for (auto& m : ci.media) {
            if (m.type != PJMEDIA_TYPE_AUDIO) continue;
            if (m.status == PJSUA_CALL_MEDIA_ACTIVE) active = true;
            else if (m.status == PJSUA_CALL_MEDIA_REMOTE_HOLD && rxOnly) active = true;     // 서버 sendonly ↔ 우리 recvonly
            else if (m.status == PJSUA_CALL_MEDIA_LOCAL_HOLD || m.status == PJSUA_CALL_MEDIA_REMOTE_HOLD) held = true;
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
        std::string whole;
        try { whole = prm.rdata.wholeMsg; } catch (...) {}
        std::string remote;
        try { remote = call->getInfo().remoteUri; } catch (...) {}
        const AccountConfig& cfg = o_->accountCfgs[accountId_];
        McpttInfo mi = mcptt::parseMcpttInfo(whole);
        bool autoAnswer = false;
        if (mi.present) {
            // MCPTT 착신 — floor 소켓은 **180 전에** 바인드해야 한다(pjsua 는 여기서 응답 SDP 를 한 번 만들고
            // 200 에 재사용하므로, 늦으면 m=application 0 이 나가 CSP 가 착신 leg 의 floor 포트를 모른다).
            call->mcptt.reset(new McpttSession);
            call->mcptt->isPrivate = mi.privateCall;
            call->mcptt->fullDuplex = mi.noFloorCtrl;
            call->mcptt->groupId = mi.privateCall ? mcptt::bareId(mi.callingUserId) : mcptt::bareId(remote);
            if (!mi.noFloorCtrl) {
                if (call->openFloor(cfg.effectiveMcpttId()))
                    call->mcptt->pendingAppSdp = floorSdp(call->mcptt->floor->localPort(), false);
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
            {
                std::lock_guard<std::mutex> lk(o_->snapM);
                auto it = o_->publishPending.find(r.token);
                if (it != o_->publishPending.end()) {
                    if (!r.etag.empty() && r.code / 100 == 2)
                        o_->publishEtag[std::to_string(it->second.first) + ":" + it->second.second] = r.etag;
                    o_->publishPending.erase(it);
                }
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
    // 재생 sink — 라우트 0 = 기본 재생 장치, 그 외 = 추가 재생 라우트. 선택되지 않은 sink 와의 결선은 끊는다
    // (미결선 쌍의 disconnect 는 no-op). 라우트가 사라졌으면 기본 장치로 폴백.
    pj::AudioMedia* sink = &spk;
    auto rt = routes.find(snap.playbackRoute);
    if (snap.playbackRoute != 0 && rt != routes.end()) sink = rt->second.get();
    if (sink != &spk) aud->stopTransmit(spk);
    for (auto& kv : routes) if (kv.second.get() != sink) aud->stopTransmit(*kv.second);
    if (snap.listen) aud->startTransmit(*sink); else aud->stopTransmit(*sink);
    bool micOn;
    if (call->mcptt) micOn = !call->mcptt->listenOnly && (call->mcptt->fullDuplex || call->mcptt->micOpen);
    else micOn = !snap.muted && !call->recvOnly;
    if (micOn) mic.startTransmit(*aud); else mic.stopTransmit(*aud);
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

std::string Engine::version() { return std::string(CIMSUE_VERSION) + " (pjproject " + pj_get_version() + ")"; }

bool Engine::running() const { return impl_->running; }

Result Engine::start(const EngineConfig& cfg, Listener* listener) {
    if (impl_->running) return Result::fail(-1, "already running");
    impl_->cfg = cfg;
    impl_->listener = listener;
    impl_->evt.start();
    impl_->ctl.start();
    Result r = impl_->ctl.runSync([this]() -> Result {
        Impl* o = impl_.get();
        try {
            pj_log_set_level(o->cfg.logLevel);               // libInit 전(writer 미설정) pjlib 기본 sink 는 stdout
            o->ep.reset(new pj::Endpoint);
            o->ep->libCreate();
            pj::EpConfig epc;
            epc.uaConfig.userAgent = o->cfg.userAgent;
            epc.logConfig.level = o->cfg.logLevel;
            epc.logConfig.consoleLevel = o->cfg.logLevel;    // pjsua 는 앱 writer 호출도 console_level 로 게이트한다
            std::unique_ptr<pj::LogWriter> writer(new PjLog(o));
            epc.logConfig.writer = writer.get();
            epc.medConfig.noVad = o->cfg.noVad;
            epc.medConfig.clockRate = o->cfg.clockRate;
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
        o->calls.clear();                        // ~Call → hangup, floor participant close
        o->routes.clear();                       // ~ExtraAudioDevice → close (libDestroy 전)
        o->accounts.clear();                     // ~Account → shutdown
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
Result Engine::setMuted(int callId, bool muted) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, muted](PjCall& c) {
        o->updateCall(callId, [&](CallInfo& ci) { ci.muted = muted; });
        o->wireMedia(&c, callId);
    });
}
Result Engine::setListen(int callId, bool listen) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, callId, listen](PjCall& c) {
        o->updateCall(callId, [&](CallInfo& ci) { ci.listen = listen; });
        o->wireMedia(&c, callId);
    });
}
Result Engine::setRxLevel(int callId, float level) {
    Impl* o = impl_.get();
    return withCall(o, callId, [o, level](PjCall& c) {
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
        PjCall* c = o->findCall(callId);
        if (!c) return finalOf();
        try {
            unsigned idx = 0;
            if (!o->activeAudio(c, &idx)) return finalOf();
            return Impl::fromPj(c->getStreamStat(idx));
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
    const std::string mcpttId = cfg.effectiveMcpttId();
    // floor 소켓은 makeCall 전에 — makeCall 이 동기적으로 onCallSdpCreated 를 부르며 로컬 offer 에 포트를 광고한다.
    if (!call->mcptt->fullDuplex) {
        if (!call->openFloor(mcpttId)) { o->log(1, "floor socket bind failed"); return -1; }
        call->mcptt->pendingAppSdp = floorSdp(call->mcptt->floor->localPort(), false);
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
                                   opts.emergency ? 1 : 0, opts.imminentPeril ? 1 : 0);
        prm.txOption.multipartParts.push_back(p1);
        if (!opts.members.empty()) {
            pj::SipMultipartPart p2;
            p2.contentType.type = "application"; p2.contentType.subType = "resource-lists+xml";
            p2.body = mcptt::resourceLists(opts.members);
            prm.txOption.multipartParts.push_back(p2);
        }
        call->makeCall("sip:" + id + "@" + cfg.domain, prm);
    } catch (pj::Error& e) {
        o->log(1, std::string("mcptt invite ") + id + ": " + e.info(false));
        return -1;
    }
    const int callId = call->getId();
    call->sealCallId(callId);
    o->updateCall(callId, [&](CallInfo& c) {
        c.accountId = accountId; c.dir = CallDir::Outgoing; c.state = CallState::Outgoing;
        c.remoteUri = "sip:" + id + "@" + cfg.domain; c.isMcptt = true; c.groupId = id;
        c.mcptt.present = true; c.mcptt.sessionType = isPrivate ? "private" : "prearranged";
        c.mcptt.emergency = opts.emergency; c.mcptt.imminentPeril = opts.imminentPeril;
        c.mcptt.privateCall = isPrivate; c.mcptt.noFloorCtrl = call->mcptt->fullDuplex;
        c.halfDuplex = !call->mcptt->fullDuplex; c.listenOnly = opts.listenOnly;
    });
    o->calls[callId] = std::move(call);
    o->log(3, std::string(isPrivate ? "private call " : "group call ") + id + " → call " + std::to_string(callId));
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
        bool emergency = o->snapshotCall(callId).mcptt.emergency;
        c.mcptt->floor->request(priority, emergency);
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

int64_t Engine::sendRequest(int accountId, const std::string& method, const std::string& targetUri,
                            const std::string& contentType, const std::string& body,
                            const std::map<std::string, std::string>& headers) {
    if (!impl_->running) return -1;
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=] { return impl_->doSendRequest(accountId, method, targetUri, contentType, body, headers, token); });
}

int64_t Engine::affiliate(int accountId, const std::string& groupId, bool on) {
    if (!impl_->running) return -1;
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=]() -> int64_t {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return -1;
        std::map<std::string, std::string> h;
        h["Event"] = "mcptt";                                              // TS 24.379 §9 — 없으면 CSP 489
        h["Expires"] = on ? "3600" : "0";
        {
            std::lock_guard<std::mutex> lk(o->snapM);
            auto et = o->publishEtag.find(std::to_string(accountId) + ":" + groupId);
            if (et != o->publishEtag.end()) h["SIP-If-Match"] = et->second;
            o->publishPending[token] = {accountId, groupId};
        }
        return o->doSendRequest(accountId, "PUBLISH", "sip:" + groupId + "@" + ic->second.domain, mcptt::kCtAffiliation,
                                mcptt::affiliationCommand("tel:" + groupId, on), h, token);
    });
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

std::string Engine::sendGroupSds(int accountId, const std::string& groupId, const std::string& text, bool requestDelivery) {
    if (!impl_->running || text.empty()) return std::string();
    std::string msgId = mcdata::newMessageId();
    int64_t token = impl_->nextToken++;
    bool ok = impl_->ctl.runSync([=]() -> bool {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return false;
        mcdata::Body b = mcdata::buildGroupSds("tel:" + groupId, text, mcdata::conversationIdOf(groupId), msgId,
                                               requestDelivery, (int64_t)std::time(nullptr));
        return o->doSendRequest(accountId, "MESSAGE", "sip:" + groupId + "@" + ic->second.domain, b.contentType, b.body, {}, token) >= 0;
    });
    return ok ? msgId : std::string();
}

Result Engine::sendSdsNotification(int accountId, const std::string& peer, const std::string& convId,
                                   const std::string& msgId, int notifType) {
    if (!impl_->running) return Result::fail(-1, "not running");
    int64_t token = impl_->nextToken++;
    return impl_->ctl.runSync([=]() -> Result {
        Impl* o = impl_.get();
        auto ic = o->accountCfgs.find(accountId);
        if (ic == o->accountCfgs.end()) return Result::fail(-2, "no such account");
        mcdata::Body b = mcdata::buildNotification(convId, msgId, notifType, (int64_t)std::time(nullptr));
        int64_t r = o->doSendRequest(accountId, "MESSAGE", "sip:" + mcptt::bareId(peer) + "@" + ic->second.domain, b.contentType, b.body, {}, token);
        return r < 0 ? Result::fail(-3, "send failed") : Result::success();
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
        return Result::success();
    });
}

}  // namespace cimsue
