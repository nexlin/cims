// DeviceLink — 시험 모드 계측 링크 (cimsue/drive.h, ue_voice_quality.md §5).
//   단말이 계측기 워커(Device.Listen)에 TLS 로 먼저 연결 → hello → welcome 이면 DriveSession 으로 워커 명령을 실행한다.
//   시험 대상 서버를 거치지 않는다. 입출력은 링크 스레드 하나가 한다 — OpenSSL SSL 객체는 동시 읽기·쓰기에 안전하지 않으므로,
//   다른 스레드(이벤트·통계)가 낸 줄은 큐에 넣고 링크 스레드가 읽기 사이사이 비운다.
#include "cimsue/drive.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "net/tls_stream.h"
#include "util/json_lite.h"

namespace cimsue {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kProto = 1;
constexpr int kHelloTimeoutMs = 10000;
constexpr int kPingIdleMs = 15000;      // 이만큼 받은 것이 없으면 ping
constexpr int kDeadIdleMs = 45000;      // 이만큼 받은 것이 없으면 끊는다
constexpr int kPollMs = 10;             // 읽기 대기 — 쓰기 큐를 비우는 주기이기도 하다
constexpr size_t kMaxLine = 64 * 1024;

/** 다른 스레드가 낸 줄을 링크 스레드가 보낼 때까지 모은다. */
class QueueSink : public LineSink {
public:
    void writeLine(const std::string& line) override {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.size() < 10000) q_.push_back(line);         // 링크가 막혀도 메모리를 무한히 쓰지 않는다
    }
    std::deque<std::string> take() { std::lock_guard<std::mutex> lk(m_); std::deque<std::string> o; o.swap(q_); return o; }
    void clear() { std::lock_guard<std::mutex> lk(m_); q_.clear(); }

private:
    std::mutex m_;
    std::deque<std::string> q_;
};

std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

struct DeviceLink::Impl {
    Engine& eng;
    DeviceLinkConfig cfg;
    DriveOptions opts;
    DeviceLinkListener* listener = nullptr;
    std::atomic<LinkState> state{LinkState::Idle};
    std::atomic<bool> running{false};
    std::thread th;
    std::mutex waitM;
    std::condition_variable waitCv;
    net::Stream stream;
    QueueSink sink;

    explicit Impl(Engine& e) : eng(e) {}

    void setState(LinkState s, const std::string& detail) {
        state = s;
        if (listener) listener->onLinkState(s, detail);
    }
    /** 멈춤 요청이 오면 일찍 깨는 잠. */
    void sleepMs(int ms) {
        std::unique_lock<std::mutex> lk(waitM);
        waitCv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return !running.load(); });
    }

    std::string helloLine() const {
        using drive::jsonEscape;
        std::string accs;
        for (auto& a : opts.accounts) {
            bool reg = eng.regInfo(a.accountId).state == RegState::Registered;
            accs += std::string(accs.empty() ? "" : ",") + "{\"service\":\"" + jsonEscape(a.service) + "\",\"aor\":\"" + jsonEscape(a.aor) +
                    "\",\"msisdn\":\"" + jsonEscape(a.msisdn) + "\",\"registered\":" + (reg ? "true" : "false") + "}";
        }
        return "{\"event\":\"hello\",\"proto\":" + std::to_string(kProto) + ",\"device_id\":\"" + jsonEscape(cfg.deviceId) + "\",\"app\":\"" + jsonEscape(cfg.app) +
               "\",\"version\":\"" + jsonEscape(cfg.appVersion) + "\",\"platform\":\"" + jsonEscape(cfg.platform) + "\",\"model\":\"" + jsonEscape(cfg.model) +
               "\",\"pair_key\":\"" + jsonEscape(cfg.pairKey) + "\",\"accounts\":[" + accs + "],\"test_mode\":true,\"engine\":\"" +
               jsonEscape(Engine::version()) + "\"}";
    }

    /** 최초 지문 고정(TOFU) — 검증을 끈 링크에서 워커 인증서가 바뀌면 거절한다. */
    bool checkPin(std::string& why) {
        if (cfg.verifyServer || cfg.pinFile.empty()) return true;
        const std::string fp = stream.peer().sha256;
        if (fp.empty()) { why = "no_peer_cert"; return false; }
        std::string pinned = readFile(cfg.pinFile);
        if (pinned.empty()) {
            std::ofstream f(cfg.pinFile, std::ios::binary | std::ios::trunc);
            f << fp << "\n";
            return true;
        }
        if (pinned != fp) { why = "pin_mismatch"; return false; }
        return true;
    }

    bool send(const std::string& line) { return stream.writeAll(line + "\n"); }
    bool flush() {
        for (auto& l : sink.take()) if (!send(l)) return false;
        return true;
    }

    /** 줄 하나를 기다린다(buf 에 남은 것 먼저). 시한·끊김 = false. */
    bool readLine(std::string& buf, std::string& line, int timeoutMs) {
        auto t0 = Clock::now();
        for (;;) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) { line = buf.substr(0, nl); buf.erase(0, nl + 1); if (!line.empty() && line.back() == '\r') line.pop_back(); return true; }
            if (buf.size() > kMaxLine) return false;
            int left = timeoutMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            if (left <= 0 || !running) return false;
            int w = stream.waitReadable(left < 100 ? left : 100);
            if (w < 0) return false;
            if (w == 0) continue;
            char tmp[4096];
            int n = stream.read(tmp, sizeof tmp);
            if (n <= 0) return false;
            buf.append(tmp, n);
        }
    }

    /** 연결 하나의 수명. 돌아오면 끊긴 것. 연결 키 거절이면 refused=true. */
    void session(bool& refused) {
        std::string err;
        net::TlsOptions to;
        to.tls = true; to.verify = cfg.verifyServer; to.caPem = cfg.caPem;
        setState(LinkState::Connecting, cfg.host + ":" + std::to_string(cfg.port));
        if (!stream.open(cfg.host, cfg.port, 10, to, err)) { setState(LinkState::Disconnected, err); return; }
        std::string why;
        if (!checkPin(why)) { stream.close(); refused = true; setState(LinkState::Refused, why); return; }
        std::string buf, line;
        if (!send(helloLine()) || !readLine(buf, line, kHelloTimeoutMs)) { stream.close(); setState(LinkState::Disconnected, "no_welcome"); return; }
        {
            jsonlite::Json j(line);
            const std::string ev = jsonlite::Json::str(j.root, "event");
            if (ev == "bye" || (ev == "welcome" && !jsonlite::Json::boolean(j.root, "accepted", true))) {
                std::string reason = jsonlite::Json::str(j.root, "reason", "refused");
                stream.close();
                refused = reason == "pair_key";
                setState(refused ? LinkState::Refused : LinkState::Disconnected, reason);
                return;
            }
            if (ev != "welcome") { stream.close(); setState(LinkState::Disconnected, "bad_welcome"); return; }
            setState(LinkState::Connected, jsonlite::Json::str(j.root, "worker"));
        }

        sink.clear();
        DriveOptions o = opts;
        o.appOwnedRegistration = true;
        o.hangupOnStop = DriveOptions::Hangup::Driven;
        std::unique_ptr<DriveSession> ds(new DriveSession(eng, sink, o));
        ds->start(false);
        auto lastRx = Clock::now();
        bool pinged = false;
        std::string reason = "closed";
        while (running) {
            if (!flush()) { reason = "write_failed"; break; }
            int w = stream.waitReadable(kPollMs);
            if (w < 0) { reason = "read_failed"; break; }
            if (w > 0) {
                char tmp[4096];
                int n = stream.read(tmp, sizeof tmp);
                if (n <= 0) { reason = "closed"; break; }
                buf.append(tmp, n);
                lastRx = Clock::now(); pinged = false;
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    line = buf.substr(0, nl); buf.erase(0, nl + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty() || line == "pong") continue;
                    if (line == "ping") { sink.writeLine("{\"event\":\"pong\"}"); continue; }
                    ds->handleLine(line);
                    if (ds->quitRequested()) {                // 워커가 이 단말을 놓았다 — 구동 호를 정리하고 새 세션으로 대기
                        ds->stop();
                        ds.reset(new DriveSession(eng, sink, o));
                        ds->start(false);
                    }
                }
                if (buf.size() > kMaxLine) { reason = "line_too_long"; break; }
            } else {
                long long idle = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastRx).count();
                if (idle > kDeadIdleMs) { reason = "idle_timeout"; break; }
                if (idle > kPingIdleMs && !pinged) { sink.writeLine("{\"event\":\"ping\"}"); pinged = true; }
            }
        }
        ds->stop();                                          // 이 세션이 만든 호만 끊는다
        flush();
        stream.close();
        setState(LinkState::Disconnected, reason);
    }

    void loop() {
        int backoff = 1;
        while (running) {
            bool refused = false;
            auto t0 = Clock::now();
            session(refused);
            if (refused || !running) break;
            // 한동안 붙어 있었으면 백오프를 처음부터
            if (std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t0).count() > 60) backoff = 1;
            sleepMs(backoff * 1000);
            backoff = backoff * 2 > cfg.reconnectMaxSec ? cfg.reconnectMaxSec : backoff * 2;
        }
    }
};

DeviceLink::DeviceLink(Engine& engine) : impl_(new Impl(engine)) {}
DeviceLink::~DeviceLink() { stop(); }

Result DeviceLink::start(const DeviceLinkConfig& cfg, const DriveOptions& opts, DeviceLinkListener* listener) {
    if (impl_->running) return Result::fail(-1, "already running");
    if (cfg.host.empty() || cfg.port <= 0) return Result::fail(-1, "host/port");
    if (opts.accounts.empty()) return Result::fail(-1, "no account");
    impl_->cfg = cfg;
    if (impl_->cfg.reconnectMaxSec < 1) impl_->cfg.reconnectMaxSec = 1;
    impl_->opts = opts;
    impl_->listener = listener;
    impl_->running = true;
    impl_->th = std::thread([this] { impl_->loop(); });
    return Result::success();
}

void DeviceLink::stop() {
    if (!impl_->running.exchange(false)) { if (impl_->th.joinable()) impl_->th.join(); return; }
    { std::lock_guard<std::mutex> lk(impl_->waitM); }
    impl_->waitCv.notify_all();
    if (impl_->th.joinable()) impl_->th.join();
    impl_->state = LinkState::Idle;
}

LinkState DeviceLink::state() const { return impl_->state.load(); }

}  // namespace cimsue
