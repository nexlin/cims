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
    /** 통화 중 영상 전환(1:1 호 — RFC 3264 §8.1) — 상대의 요청(Received → Engine::answerVideoRequest)·내 요청의 결과. */
    virtual void onVideoRequest(const VideoRequestEvent& ev) { (void)ev; }
    /** floor participant 상태 전이 (TS 24.380 §6.2.4). 마이크 게이트는 코어가 이미 처리했다. */
    virtual void onFloor(const FloorEvent& ev) { (void)ev; }
    /** 그룹 세션 참가자(RFC 4575 conference-info) — conference 구독의 NOTIFY(Engine::subscribeConference). full=전체 스냅샷. */
    virtual void onRoster(int accountId, const std::string& groupId, const std::vector<RosterEntry>& users,
                          bool full) { (void)accountId; (void)groupId; (void)users; (void)full; }
    /** 감시 대상 dialog 상태(RFC 4235 NOTIFY) — dialog 하나당 1회. Join 대상 선택의 입력. */
    virtual void onDialogInfo(const DialogInfo& d) { (void)d; }
    /** MCVideo 송출 제어(TS 24.581 §6.2.4) — 허가·거절·회수·대기·종료. 송출(마이크·카메라) 게이트는 코어가 이미 처리했다. */
    virtual void onTransmission(const TransmissionEvent& ev) { (void)ev; }
    /** MCVideo 수신 제어(§6.2.5) — 새 송출 알림(manual 이면 앱이 [받기])·수신 허가·종료. 수신 결선은 코어가 이미 처리했다. */
    virtual void onReception(const ReceptionEvent& ev) { (void)ev; }
    /** MCPTT 세션 조건 변화(긴급·임박, TS 24.379 §10.1.1.2.1.3~6) — info.condition 이 새 값, cause 가 계기. */
    virtual void onMcpttCondition(const CallInfo& info, ConditionCause cause) { (void)info; (void)cause; }
    /** 개시 호의 미응답 멤버 알림(TS 24.379 §6.3.3.3 — INFO g.3gpp.mcptt-info) — info.nonAcknowledgedUsers. 200 OK 는 코어가 이미 보냈다. */
    virtual void onNonAcknowledgedUsers(const CallInfo& info) { (void)info; }
    /** 긴급 경보·취소·긴급 통지 수신(TS 24.379 §12.1.1.3). 200 OK 는 코어가 이미 보냈다. */
    virtual void onEmergencyAlert(const EmergencyAlert& alert) { (void)alert; }
    /** MCData SDS 수신(메시지·disposition 통지·FD). */
    virtual void onSds(const SdsMessage& msg) { (void)msg; }
    /** 임의 요청(PUBLISH/SUBSCRIBE 등)의 최종 응답 — affiliation 확인·ETag. */
    virtual void onRequestResult(const RequestResult& r) { (void)r; }
    /** MCData 가 아닌 MESSAGE/NOTIFY 본문(xcap-diff 등) — 앱이 해석. */
    virtual void onMessage(int accountId, const std::string& fromUri, const std::string& contentType,
                           const std::string& body) { (void)accountId; (void)fromUri; (void)contentType; (void)body; }
    virtual void onEngineStopped() {}
    /** 영상 프레임 — 창 없는 프레임 렌더 빌드(Windows, ue_sdk.md §4.5)만. **예외: 이벤트 스레드가 아니라 영상 스레드**(영상 회의
     *  브리지 클럭)에서 프레임마다(초당 15~30회) 곧바로 불린다. 화소를 복사하고 곧 돌아간다 — 이 콜백 안에서 Engine 명령을 부르지
     *  않는다(엔진이 영상 포트를 멈추며 이 스레드를 기다리는 중이면 교착한다). 수신 영상 = 그 호의 callId, 셀프뷰 = callId -1. */
    virtual void onVideoFrame(const VideoFrame& frame) { (void)frame; }
};

}  // namespace cimsue
