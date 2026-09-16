// HttpServer — 워커 제어 API 용 최소 HTTP/1.1 서버 (컨트롤러 → 워커, test_instrument.md §6.1).
// 요청은 작고 드물다(풀 생성·run 시작·율 변경·중단·health). 연결마다 스레드 하나, 요청 하나 처리 후 닫는다.
// psip HttpStack(TcpStack 스레드 풀·WebSocket)은 이 용도에 과하다.
#ifndef _CIMS_TESTER_HTTP_SERVER_H_
#define _CIMS_TESTER_HTTP_SERVER_H_

#include <atomic>
#include <functional>
#include <string>
#include <thread>

struct HttpRequest {
    std::string method;
    std::string path;      // query 제외
    std::string query;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
};

class HttpServer {
public:
    typedef std::function<HttpResponse(const HttpRequest&)> Handler;

    HttpServer() : m_fd(-1), m_stop(false) {}
    ~HttpServer() { stop(); }

    bool start(const std::string& ip, int port, Handler h, std::string& err);
    void stop();
    int port() const { return m_port; }

private:
    int m_fd;
    int m_port = 0;
    std::atomic<bool> m_stop;
    std::thread m_thread;
    Handler m_handler;

    void acceptLoop();
    void serve(int cfd);
};

#endif
