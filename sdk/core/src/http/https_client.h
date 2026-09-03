// libcimsue 내부 — HTTP(S) 전송 (ue_sdk.md §4.4). 코어가 프로토콜(PKCE·XCAP·프로비저닝)을 소유하고 전송은
// 이 인터페이스로 추상한다. 기본 구현은 OpenSSL(pjproject 가 이미 링크) 위의 최소 HTTP/1.1 클라이언트 —
// 플랫폼 SDK 는 OkHttp/WinHTTP 구현을 주입할 수 있다.
#pragma once

#include <map>
#include <string>

namespace cimsue {
namespace http {

struct Response {
    int status = 0;                      // 0 = 전송 실패
    std::map<std::string, std::string> headers;   // 소문자 키
    std::string body;
    std::string error;
};

class ITransport {
public:
    virtual ~ITransport() = default;
    /** url 은 http(s)://host[:port]/path?query. body 가 비면 Content-Length 없이 보낸다(GET 등). */
    virtual Response request(const std::string& method, const std::string& url,
                             const std::map<std::string, std::string>& headers, const std::string& body) = 0;
};

/** OpenSSL 기본 구현. caPem 이 비고 verify 가 false 면 서버 인증서를 검증하지 않는다(개발 서버 자가서명). */
class OpenSslTransport : public ITransport {
public:
    OpenSslTransport(const std::string& caPem, bool verify, int timeoutSec = 15)
        : caPem_(caPem), verify_(verify), timeoutSec_(timeoutSec) {}
    Response request(const std::string& method, const std::string& url,
                     const std::map<std::string, std::string>& headers, const std::string& body) override;

private:
    std::string caPem_;
    bool verify_;
    int timeoutSec_;
};

std::string urlEncode(const std::string& s);
/** 응답 헤더 값(소문자 키). */
std::string header(const Response& r, const std::string& name);

}  // namespace http
}  // namespace cimsue
