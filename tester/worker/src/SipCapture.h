// SipCapture — 워커 프로세스의 SIP 송수신을 Call-ID 별로 붙잡아 두는 링 버퍼 (test_instrument.md §5 `sip/` 덤프).
//
//  psip 은 모든 transport 의 송수신을 CLog(LOG_NETWORK) 로 남긴다("UdpSend(ip:port) \n[메시지]" — Udp/Tcp/Tls × Send/Recv).
//  워커는 그 로그 콜백(ILogCallBack)을 받아 메시지의 Call-ID 로 묶어 둔다. 실패한(또는 mode=all 이면 모든) 인스턴스가 끝날 때
//  그 인스턴스의 Call-ID 들을 꺼내 관측 스트림의 `sip` 레코드로 올린다. 성공한 호의 것은 그때 버린다.
//  상한: 호당 메시지 수·바이트, 전체 호 수(오래된 것부터 버림 — 인스턴스에 묶이지 않는 REGISTER/OPTIONS 가 쌓이지 않게).
//  psip 로그 버퍼가 8 KB 라 그보다 긴 메시지는 잘려 온다.
#ifndef _CIMS_TESTER_SIP_CAPTURE_H_
#define _CIMS_TESTER_SIP_CAPTURE_H_

#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Log.h"

struct SipCapturedMessage {
    double t = 0;                 // unix 초(µs 정밀)
    bool tx = false;
    std::string transport;        // udp|tcp|tls
    std::string peer;             // ip:port
    std::string text;
};

class SipCapture : public ILogCallBack {
public:
    enum Mode { OFF, FAILED, ALL };
    static Mode ParseMode(const std::string& s) { return s == "off" ? OFF : s == "all" ? ALL : FAILED; }

    /** psip 로그 콜백으로 등록하고 LOG_NETWORK 를 켠다(OFF 면 아무것도 하지 않는다). 프로세스에 하나. */
    void install(Mode mode);
    Mode mode() const { return m_mode; }

    /** 그 Call-ID 의 메시지를 꺼내고 버퍼에서 지운다(없으면 빈 벡터). */
    std::vector<SipCapturedMessage> take(const std::string& callId);
    void drop(const std::string& callId);
    size_t calls();

    // ILogCallBack — 스택 스레드
    void Print(EnumLogLevel eLevel, const char* fmt, ...) override;

    /** 로그 한 줄 → 메시지(형식이 아니면 false). 단위시험용으로 공개. */
    static bool Parse(const char* line, SipCapturedMessage& out, std::string& callId);

private:
    static const size_t kMaxMessagesPerCall = 60;
    static const size_t kMaxBytesPerCall = 256 * 1024;
    static const size_t kMaxCalls = 5000;
    struct Entry {
        std::vector<SipCapturedMessage> msgs;
        size_t bytes = 0;
        std::list<std::string>::iterator lru;
    };
    Mode m_mode = OFF;
    std::mutex m_mtx;
    std::map<std::string, Entry> m_calls;
    std::list<std::string> m_order;      // 앞 = 가장 오래된 호
};

#endif
