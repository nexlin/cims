#include "RealUe.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

RealUeProcess::RealUeProcess(const std::string& tag, EventFn onEvent) : m_tag(tag), m_onEvent(std::move(onEvent)) {}

RealUeProcess::~RealUeProcess() { stop(500); }

bool RealUeProcess::start(const std::vector<std::string>& argv, const std::string& logFile, std::string& err) {
    if (argv.empty()) { err = "argv empty"; return false; }
    int inPipe[2], outPipe[2];
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0) { err = std::string("pipe: ") + strerror(errno); return false; }
    int logFd = -1;
    if (!logFile.empty()) logFd = open(logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (logFd < 0) logFd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) { err = std::string("fork: ") + strerror(errno); close(inPipe[0]); close(inPipe[1]); close(outPipe[0]); close(outPipe[1]); if (logFd >= 0) close(logFd); return false; }
    if (pid == 0) {
        // 자식 — stdin/stdout 을 파이프에, stderr 를 로그 파일에. 부모의 다른 fd 는 닫는다(파이프·소켓 상속 금지)
        dup2(inPipe[0], 0);
        dup2(outPipe[1], 1);
        if (logFd >= 0) dup2(logFd, 2);
        long maxFd = sysconf(_SC_OPEN_MAX);
        if (maxFd < 0 || maxFd > 65536) maxFd = 65536;
        for (int fd = 3; fd < maxFd; ++fd) close(fd);
        std::vector<char*> av;
        for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execv(av[0], av.data());
        // exec 실패 — stdout 으로 알리고 끝낸다(부모 리더가 exit 로 본다)
        const char* msg = "{\"event\":\"exit\",\"error\":\"exec failed\"}\n";
        if (write(1, msg, strlen(msg)) < 0) {}
        _exit(127);
    }
    close(inPipe[0]);
    close(outPipe[1]);
    if (logFd >= 0) close(logFd);
    m_in = inPipe[1];
    m_out = outPipe[0];
    m_pid = pid;
    m_alive = true;
    m_reader = std::thread([this] { readerLoop(); });
    return true;
}

void RealUeProcess::readerLoop() {
    std::string buf;
    char chunk[4096];
    for (;;) {
        ssize_t n = read(m_out, chunk, sizeof(chunk));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        buf.append(chunk, (size_t)n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (line.empty()) continue;
            Json ev;
            std::string perr;
            if (!Json::parse(line, ev, perr) || !ev.isObject()) continue;   // pjsip 가 stdout 에 흘린 줄 등 — 무시
            deliver(ev);
        }
    }
    // EOF — 프로세스 종료
    int status = 0;
    if (m_pid > 0) {
        for (int i = 0; i < 50; ++i) {
            pid_t r = waitpid(m_pid, &status, WNOHANG);
            if (r == m_pid) { m_exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status); break; }
            if (r < 0) break;
            usleep(20000);
        }
    }
    m_alive = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_cv.notify_all();
    }
    Json ex = Json::Object();
    ex["event"] = Json("process_exit");
    ex["status"] = Json((long long)m_exitStatus);
    if (m_onEvent) m_onEvent(ex);
}

void RealUeProcess::deliver(const Json& ev) {
    const std::string kind = ev["event"].asString();
    if (kind == "result") {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_results.push_back(ev);
        m_cv.notify_all();
        return;
    }
    if (kind == "ready") {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_ready = true;
        m_cv.notify_all();
        return;
    }
    if (m_onEvent) m_onEvent(ev);
}

bool RealUeProcess::waitReady(int timeoutMs) {
    std::unique_lock<std::mutex> lk(m_mtx);
    return m_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return m_ready.load() || !m_alive.load(); }) && m_ready;
}

bool RealUeProcess::send(const std::string& cmd) {
    if (!m_alive || m_in < 0) return false;
    std::string line = cmd + "\n";
    std::lock_guard<std::mutex> lk(m_mtx);
    size_t off = 0;
    while (off < line.size()) {
        ssize_t n = write(m_in, line.data() + off, line.size() - off);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        off += (size_t)n;
    }
    return true;
}

Json RealUeProcess::request(const std::string& cmd, int timeoutMs) {
    Json fail = Json::Object();
    fail["ok"] = Json(false);
    fail["call"] = Json(-1);
    if (!send(cmd)) { fail["reason"] = Json("process not running"); return fail; }
    std::unique_lock<std::mutex> lk(m_mtx);
    bool got = m_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !m_results.empty() || !m_alive.load(); });
    if (!got || m_results.empty()) { fail["reason"] = Json(m_alive ? "timeout" : "process exited"); return fail; }
    Json r = m_results.front();
    m_results.pop_front();
    return r;
}

void RealUeProcess::stop(int graceMs) {
    if (m_pid <= 0 && !m_reader.joinable()) return;
    if (m_alive) {
        send("quit");
        for (int waited = 0; m_alive && waited < graceMs; waited += 50) usleep(50000);
        if (m_alive && m_pid > 0) {
            kill(m_pid, SIGTERM);
            for (int waited = 0; m_alive && waited < 1000; waited += 50) usleep(50000);
            if (m_alive) kill(m_pid, SIGKILL);
        }
    }
    if (m_in >= 0) { close(m_in); m_in = -1; }
    if (m_reader.joinable()) m_reader.join();
    if (m_out >= 0) { close(m_out); m_out = -1; }
    if (m_pid > 0) { int st = 0; waitpid(m_pid, &st, WNOHANG); m_pid = -1; }
}
