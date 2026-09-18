// RealUe — 실단말(real-ue) 프로세스 (test_instrument.md §3.3).
//   신원 하나 = `cimsue-cli … drive` 자식 프로세스 하나(libcimsue/pjsua2 실스택). 워커는 stdin 으로 한 줄 명령을 보내고
//   stdout 의 한 줄 JSON 이벤트를 받는다(ue_sdk.md §4.7 구동 모드). 명령의 동기 결과(`result` 이벤트)는 request() 가 기다려
//   돌려주고, 나머지 이벤트(reg·incoming·call·floor·request·stats·exit)는 콜백으로 올린다 — 콜백은 **리더 스레드**에서 불리므로
//   워커는 큐에만 넣는다(psip 관측자 훅과 같은 규약).
//   프로세스 수명 = 풀 수명(POST /pools 에서 스폰·ready 대기, DELETE/교체·워커 정지에서 quit → SIGTERM → SIGKILL).
#ifndef _CIMS_TESTER_REAL_UE_H_
#define _CIMS_TESTER_REAL_UE_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

#include "Json.h"

class RealUeProcess {
public:
    using EventFn = std::function<void(const Json&)>;
    RealUeProcess(const std::string& tag, EventFn onEvent);
    ~RealUeProcess();

    /** 스폰 + 리더 스레드. stderr(pjsip 로그)는 logFile 로(비면 /dev/null). 실패 = false + err. */
    bool start(const std::vector<std::string>& argv, const std::string& logFile, std::string& err);
    /** `ready` 이벤트 대기(엔진 기동 완료). */
    bool waitReady(int timeoutMs);
    /** 명령 한 줄 → 그 `result` 이벤트. 시한·프로세스 종료면 ok=false + reason. 명령은 호출 순서대로 직렬(FIFO). */
    Json request(const std::string& cmd, int timeoutMs);
    /** 결과를 기다리지 않는 송신(quit 등). */
    bool send(const std::string& cmd);
    /** quit → graceMs 대기 → SIGTERM → SIGKILL. 리더 스레드 join. */
    void stop(int graceMs = 2000);
    bool alive() const { return m_alive.load(); }
    pid_t pid() const { return m_pid; }
    const std::string& tag() const { return m_tag; }

private:
    void readerLoop();
    void deliver(const Json& ev);

    std::string m_tag;
    EventFn m_onEvent;
    pid_t m_pid = -1;
    int m_in = -1;                  // 자식 stdin (우리가 쓴다)
    int m_out = -1;                 // 자식 stdout (우리가 읽는다)
    std::thread m_reader;
    std::atomic<bool> m_alive{false};
    std::atomic<bool> m_ready{false};
    std::mutex m_mtx;               // 결과 큐·ready·송신
    std::condition_variable m_cv;
    std::deque<Json> m_results;
    int m_exitStatus = -1;
};

#endif
