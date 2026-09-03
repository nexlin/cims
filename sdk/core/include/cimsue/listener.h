// libcimsue — 이벤트 리스너 (ue_sdk.md §4.2·§4.3)
//
// 모든 콜백은 코어의 **이벤트 스레드**에서 온다(pjsip 스레드가 아니다). 콜백 안에서 Engine 의 어떤
// 명령을 다시 불러도 교착하지 않는다. 플랫폼 SDK 는 필요 시 UI 스레드로 마샬링한다.
#pragma once

#include "cimsue/export.h"
#include "cimsue/types.h"

namespace cimsue {

class CIMSUE_API Listener {
public:
    virtual ~Listener() = default;
    /** pjsip/코어 로그 한 줄. level 은 pjsip 레벨(1=error … 6=trace). */
    virtual void onLog(int level, const std::string& msg) { (void)level; (void)msg; }
    virtual void onRegState(const RegInfo& info) { (void)info; }
    /** 착신 — 180 은 코어가 이미 보냈다. MCPTT 착신은 autoAnswerMcptt 면 코어가 200 까지 보낸다. */
    virtual void onIncomingCall(const CallInfo& info) { (void)info; }
    virtual void onCallState(const CallInfo& info) { (void)info; }
    /** 미디어 활성/보류/소스 변화(SSRC 라벨 포함). */
    virtual void onCallMedia(const CallInfo& info) { (void)info; }
    /** floor participant 상태 전이 (TS 24.380 §6.2.4). 마이크 게이트는 코어가 이미 처리했다. */
    virtual void onFloor(const FloorEvent& ev) { (void)ev; }
    /** 그룹 로스터(RFC 4575 conference-info) — 구독 NOTIFY 또는 in-dialog NOTIFY. full=전체 스냅샷. */
    virtual void onRoster(int accountId, const std::string& groupId, const std::vector<RosterEntry>& users,
                          bool full) { (void)accountId; (void)groupId; (void)users; (void)full; }
    /** 감시 대상 dialog 상태(RFC 4235 NOTIFY) — dialog 하나당 1회. Join 대상 선택의 입력. */
    virtual void onDialogInfo(const DialogInfo& d) { (void)d; }
    /** MCData SDS 수신(메시지·disposition 통지·FD). */
    virtual void onSds(const SdsMessage& msg) { (void)msg; }
    /** 임의 요청(PUBLISH/SUBSCRIBE 등)의 최종 응답 — affiliation 확인·ETag. */
    virtual void onRequestResult(const RequestResult& r) { (void)r; }
    /** MCData 가 아닌 MESSAGE/NOTIFY 본문(xcap-diff 등) — 앱이 해석. */
    virtual void onMessage(int accountId, const std::string& fromUri, const std::string& contentType,
                           const std::string& body) { (void)accountId; (void)fromUri; (void)contentType; (void)body; }
    virtual void onEngineStopped() {}
};

}  // namespace cimsue
