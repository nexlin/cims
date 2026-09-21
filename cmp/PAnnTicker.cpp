#include "PAnnTicker.h"

#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "PLog.h"
#include "PRtpRelay.h"

static const long kTickNs = 20L * 1000L * 1000L;   // 20 ms

PAnnTicker::PAnnTicker(const std::string& name) : PHandler(name) {}

PAnnTicker::~PAnnTicker() {
    if (_tfd >= 0) close(_tfd);
}

bool PAnnTicker::init() {
    _tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (_tfd < 0) {
        LOG_ERROR("PAnnTicker", "timerfd_create failed: %s", strerror(errno));
        return false;
    }
    return true;
}

int64_t PAnnTicker::nowUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

void PAnnTicker::_arm(bool on) {
    if (_tfd < 0 || _armed == on) return;
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    if (on) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        its.it_interval.tv_nsec = kTickNs;
        its.it_value = now;
        its.it_value.tv_nsec += kTickNs;
        if (its.it_value.tv_nsec >= 1000000000L) { its.it_value.tv_sec += 1; its.it_value.tv_nsec -= 1000000000L; }
        if (timerfd_settime(_tfd, TFD_TIMER_ABSTIME, &its, nullptr) < 0) {
            LOG_ERROR("PAnnTicker", "timerfd_settime(arm) failed: %s", strerror(errno));
            return;
        }
    } else {
        timerfd_settime(_tfd, 0, &its, nullptr);   // 전부 0 = disarm
    }
    _armed = on;
}

void PAnnTicker::add(PRtpRelay* relay) {
    if (!relay) return;
    PAutoLock lock(_mtx);
    _relays.insert(relay);
    _arm(true);
}

size_t PAnnTicker::size() const {
    PAutoLock lock(const_cast<PMutex&>(_mtx));
    return _relays.size();
}

bool PAnnTicker::proc() {
    if (_tfd < 0) return false;
    uint64_t expirations = 0;
    while (read(_tfd, &expirations, sizeof(expirations)) > 0) {}   // drain (여러 번 만료돼도 틱은 한 번 — now 기준으로 따라잡는다)

    std::vector<PRtpRelay*> snapshot;
    {
        PAutoLock lock(_mtx);
        snapshot.assign(_relays.begin(), _relays.end());
    }
    if (snapshot.empty()) {
        PAutoLock lock(_mtx);
        if (_relays.empty()) _arm(false);
        return false;
    }
    const int64_t now = nowUs();
    std::vector<Done> done;
    std::vector<PRtpRelay*> finished;
    for (PRtpRelay* r : snapshot) {
        if (!r->annTick(now, done)) finished.push_back(r);
    }
    if (!finished.empty()) {
        PAutoLock lock(_mtx);
        for (PRtpRelay* r : finished) _relays.erase(r);
        if (_relays.empty()) _arm(false);
    }
    if (_onDone) for (const Done& d : done) _onDone(d);
    return false;
}
