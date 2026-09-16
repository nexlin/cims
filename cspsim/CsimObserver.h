#ifndef _CSIM_OBSERVER_H_
#define _CSIM_OBSERVER_H_

#include <string>

class SimSession;

/** libcsim 이벤트 관측자 — 계측기 워커가 RFC 6076 지표(RRD·SRD·SDD)와 실패 개별 건을 세는 훅.
 *  psip 스택 스레드에서 호출되므로 구현은 짧게(큐에 넣기) 끝내야 한다. cspsim CLI 는 등록하지 않는다. */
struct ICsimObserver {
    virtual ~ICsimObserver() {}
    /** 최초 REGISTER 최종 응답 — iStatus 200 이면 rrdMs = 첫 송신→200 (401 왕복 포함). 주기 재등록은 통지하지 않는다. */
    virtual void OnRegister(SimSession* /*s*/, int /*iStatus*/, long long /*rrdMs*/) {}
    /** INVITE 수신(착신) — 응답 전. deferred 모드면 AnswerCall/RejectCall 은 관측자 측 스케줄러가 부른다. */
    virtual void OnIncomingCall(SimSession* /*s*/, const std::string& /*callId*/, const std::string& /*from*/) {}
    /** 발신 INVITE 의 2xx(확립) — srdMs = StartCall → 200. */
    virtual void OnCallStart(SimSession* /*s*/, const std::string& /*callId*/, long long /*srdMs*/) {}
    /** 다이얼로그 종료 — 발신 실패 최종 응답(4xx/5xx/6xx)·상대 BYE(200)·CANCEL(487)·타이머 만료. */
    virtual void OnCallEnd(SimSession* /*s*/, const std::string& /*callId*/, int /*iSipStatus*/) {}
    /** 로컬 BYE 의 최종 응답 — sddMs = StopCall → 응답. */
    virtual void OnByeResponse(SimSession* /*s*/, const std::string& /*callId*/, int /*iSipStatus*/, long long /*sddMs*/) {}
};

#endif
