#ifndef __PANN_TICKER_H__
#define __PANN_TICKER_H__

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "PMPBase.h"
#include "pbase.h"
#include "pmodule.h"

class PRtpRelay;

/**
 * 워커(리액터)별 안내 재생 클록 — timerfd 20 ms 하나가 그 워커의 활성 재생기 전부를 구동한다 (announcements.md §4.2).
 *
 *   relay 리액터는 epoll 이벤트 구동이라 클록이 없다. 재생기를 가진 relay 를 add() 로 등록하면 타이머를 arm 하고, 틱마다
 *   relay->annTick() 을 불러 프레임을 내게 한다. 재생기가 모두 끝난 relay 는 자연 제거되고, 집합이 비면 disarm 한다(idle CPU 0).
 *   절대 시각(TFD_TIMER_ABSTIME) 주기라 드리프트가 없다.
 *
 *   스레드 — add() 는 제어 스레드, proc() 은 이 워커의 리액터 스레드(relay proc 과 같은 스레드라 relay 와 경합 없음).
 *   재생 완료 통지는 relay _mutex 를 놓은 뒤 콜백으로 낸다(서버가 _mutex 를 잡고 sesid 를 찾을 수 있게 — 락 순서 역전 방지).
 */
class PAnnTicker : public PHandler {
public:
    struct Done {
        std::string sessionId;
        int peerIdx = 0;
        std::string playId;
        std::string reason;
        int playedMs = 0;
        std::string media;
    };
    typedef std::function<void(const Done&)> DoneCb;

    explicit PAnnTicker(const std::string& name);
    virtual ~PAnnTicker();

    bool init();                    // timerfd 생성(disarmed)
    int fd() const { return _tfd; }
    void setDoneCallback(DoneCb cb) { _onDone = cb; }

    void add(PRtpRelay* relay);     // 재생기가 붙은 relay 등록 + arm
    size_t size() const;

    bool proc();
    bool proc(int, const std::string&, PEvent::Ptr) { return false; }

    static int64_t nowUs();

private:
    void _arm(bool on);

    int _tfd = -1;
    bool _armed = false;
    PMutex _mtx;
    std::set<PRtpRelay*> _relays;
    DoneCb _onDone;
};

#endif  // __PANN_TICKER_H__
