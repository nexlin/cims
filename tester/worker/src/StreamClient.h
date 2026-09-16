// StreamClient — 워커 → 컨트롤러 관측 스트림 (지속 TCP, 한 줄 = JSON 레코드 하나; test_instrument.md §6.1).
// UDP 는 부하 중 유실되어 지표를 왜곡하므로 쓰지 않는다. 끊기면 지수 backoff 로 재접속하고 그동안의
// 레코드는 큐(상한 있음)에 보관한다 — 오래 끊기면 오래된 것부터 버린다(집계는 1초 단위라 손실이 국소적이다).
#ifndef _CIMS_TESTER_STREAM_CLIENT_H_
#define _CIMS_TESTER_STREAM_CLIENT_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

class StreamClient {
public:
    StreamClient() : m_fd(-1), m_stop(false), m_connected(false) {}
    ~StreamClient() { stop(); }

    /** dest = "host:port". hello = 접속 직후 보낼 레코드(빈 문자열이면 없음). */
    void start(const std::string& dest, const std::string& hello);
    void stop();
    void send(const std::string& line);          // 개행 없이 — 여기서 '\n' 을 붙인다
    bool connected() const { return m_connected; }
    size_t backlog();

private:
    std::string m_host;
    int m_port = 0;
    std::string m_hello;
    int m_fd;
    std::atomic<bool> m_stop;
    std::atomic<bool> m_connected;
    std::thread m_thread;
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::string> m_q;

    void loop();
    bool connectOnce();
    bool writeAll(const std::string& s);
};

#endif
