/*
 * MSRP TCP 연결 1건 — cmdp epoll 리액터의 PHandler.
 *
 * cmp 리액터와 달리 fd 가 동적이다: accept 시 EPOLLIN|EPOLLRDHUP 로 등록되고,
 * 종료 시 서버가 epoll DEL + tombstone(지연 삭제) 처리한다. 송신은 write queue 로
 * 백프레셔를 흡수한다 — EWOULDBLOCK 시 EPOLLOUT 을 arm 하고, 큐가 비면 disarm
 * (level-triggered 라 빈 큐로 EPOLLOUT 을 켜두면 busy-notify 가 된다).
 *
 * 스레딩: proc() 은 리액터 스레드 전용. queueWrite() 는 제어 스레드에서도 호출되므로
 * (SET_REMOTE_PATH → 즉시 송신 개시) 출력 큐만 자체 뮤텍스로 보호한다.
 */

#ifndef _P_MSRP_CONNECTION_H_
#define _P_MSRP_CONNECTION_H_

#include <mutex>
#include <string>
#include "phandler.h"
#include "PMsrpParser.h"

class PCmdpServer;
class PMsrpSession;

class PMsrpConnection : public PHandler {
public:
    PMsrpConnection(int fd, int epfd, PCmdpServer* server, const std::string& peer);
    virtual ~PMsrpConnection();

    virtual bool proc();  // 리액터 콜백: 쓰기 flush + 읽기 drain + 프레임 디스패치
    virtual bool proc(int, const std::string&, PEvent::Ptr) { return false; }

    /** 프레임 송신 (제어/리액터 스레드 공용) */
    void queueWrite(const std::string& data);

    int fd() const { return _fd; }
    const std::string& peer() const { return _peer; }
    bool closed() const { return _closed; }

    /** 바인딩된 세션 (첫 요청 To-Path 로 서버가 설정) */
    PMsrpSession* _session = nullptr;

private:
    int _fd;
    int _epfd;
    PCmdpServer* _server;
    std::string _peer;  // "ip:port"
    PMsrpParser _parser;

    std::mutex _outMtx;
    std::string _outBuf;
    bool _epollOutArmed = false;
    bool _closed = false;

    void flushWrite();          // _outMtx 내부 획득
    void armEpollOut(bool on);  // _outMtx 보유 중 호출
    void markClosed();
};

#endif
