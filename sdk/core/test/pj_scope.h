// 시험용 pjlib 수명 — pj_init / pj_shutdown 을 시험마다 짝지운다(S1-UE-UNIT).
//
// pjlib 은 초기화 횟수를 센다. 이미 초기화돼 있으면 pj_init 은 횟수만 올리고 **호출 스레드를 등록하지 않는다**
// (os_core_unix.c·os_core_win32.c 의 pj_init). 시험이 pj_init 만 하고 남겨 두면, 같은 프로세스에서 뒤에 도는
// Engine::start 의 제어 스레드(ue-ctl)가 libCreate 에서 등록되지 않은 채 pjlib 을 불러 "unknown thread" assert 로
// 죽는다(EngineRoute·CApi.EngineLifecycleHeadless — 각자 단독으로는 통과). 그래서 시험 몸체 첫 줄에 두어
// 소멸이 마지막(소켓·참가자 해제 뒤)이 되게 하고, pj_shutdown 으로 횟수를 되돌린다.
#pragma once

#include <pjlib.h>

namespace cimsue_test {

class PjScope {
public:
    explicit PjScope(const char* name = "cimsue-test") {
        ok_ = pj_init() == PJ_SUCCESS;
        // 처음 초기화면 pj_init 이 호출 스레드를 이미 등록한다. 다른 곳이 먼저 초기화해 둔 경우만 여기서 등록한다 —
        // 서술자는 스레드 수명이어야 하므로 thread_local 로 둔다(등록이 이 객체보다 오래 남을 수 있다).
        if (ok_ && !pj_thread_is_registered()) {
            static thread_local pj_thread_desc desc;
            pj_thread_t* th = nullptr;
            pj_bzero(desc, sizeof(desc));
            pj_thread_register(name, desc, &th);
        }
    }
    ~PjScope() { if (ok_) pj_shutdown(); }
    PjScope(const PjScope&) = delete;
    PjScope& operator=(const PjScope&) = delete;

    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

}  // namespace cimsue_test
