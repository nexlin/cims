// libcimsue — 이벤트 리스너 (ue_sdk.md §4.2·§4.3)
//
// 모든 콜백은 코어의 **이벤트 스레드**에서 온다(pjsip 스레드가 아니다). 콜백 안에서 Engine 의 어떤
// 명령을 다시 불러도 교착하지 않는다. 플랫폼 SDK 는 필요 시 UI 스레드로 마샬링한다.
#pragma once

#include "cimsue/types.h"

namespace cimsue {

class Listener {
public:
    virtual ~Listener() = default;
    /** pjsip/코어 로그 한 줄. level 은 pjsip 레벨(1=error … 6=trace). */
    virtual void onLog(int level, const std::string& msg) { (void)level; (void)msg; }
    virtual void onRegState(const RegInfo& info) { (void)info; }
    /** 착신 — 180 은 코어가 이미 보냈다. 앱은 answer()/reject() 로 응답한다. */
    virtual void onIncomingCall(const CallInfo& info) { (void)info; }
    virtual void onCallState(const CallInfo& info) { (void)info; }
    /** 미디어 활성/보류/소스 변화(SSRC 라벨 포함). */
    virtual void onCallMedia(const CallInfo& info) { (void)info; }
    virtual void onEngineStopped() {}
};

}  // namespace cimsue
