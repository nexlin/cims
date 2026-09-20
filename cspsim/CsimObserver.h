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
    /** 다이얼로그 종료 — 발신 실패 최종 응답(4xx/5xx/6xx)·상대 BYE(200)·CANCEL(487)·타이머 만료.
     *  iQ850 = 상대 BYE/최종 응답의 Reason `Q.850;cause=`(RFC 3326, 0 = 없음). */
    virtual void OnCallEnd(SimSession* /*s*/, const std::string& /*callId*/, int /*iSipStatus*/, int /*iQ850*/) {}
    /** 발신 INVITE 의 1xx — bHasSdp = early media answer(183), bPrackSent = RSeq 가 있어 PRACK 을 냈다(RFC 3262). */
    virtual void OnCallRing(SimSession* /*s*/, const std::string& /*callId*/, int /*iSipStatus*/, bool /*bHasSdp*/, bool /*bPrackSent*/) {}
    /** 상대 re-INVITE 수신(psip 이 200 answer) — bRemoteHold = a=sendonly/inactive. */
    virtual void OnReInvite(SimSession* /*s*/, const std::string& /*callId*/, bool /*bRemoteHold*/) {}
    /** 자기 re-INVITE(Hold/Resume) 최종 응답. */
    virtual void OnReInviteResponse(SimSession* /*s*/, const std::string& /*callId*/, int /*iSipStatus*/) {}
    /** 자기 REFER 최종 응답(202 정상). */
    virtual void OnReferResponse(SimSession* /*s*/, const std::string& /*callId*/, int /*iSipStatus*/) {}
    /** 로컬 BYE 의 최종 응답 — sddMs = StopCall → 응답. */
    virtual void OnByeResponse(SimSession* /*s*/, const std::string& /*callId*/, int /*iSipStatus*/, long long /*sddMs*/) {}
    // ── PTT(MCPTT) — 계측기 워커 단계 group_call/floor_request/floor_release (test_instrument.md §4) ──
    /** 그룹 affiliation PUBLISH(TS 24.379 §9, RFC 3903) 최종 응답 — affMs = PUBLISH 송신 → 응답. bDeaffiliate = 해제(Expires 0) 명령의 응답. */
    virtual void OnAffiliate(SimSession* /*s*/, const std::string& /*group*/, int /*iSipStatus*/, long long /*affMs*/, bool /*bDeaffiliate*/) {}
    // ── 이벤트 구독·다이얼로그 이벤트 — 계측기 워커 단계 subscribe/replaces/join (test_instrument.md §4) ──
    /** out-of-dialog SUBSCRIBE(RFC 6665) 최종 응답 — event = 패키지 토큰(dialog·reg·…), resource = 감시 자원 AoR 사용자부.
     *  401 Digest 재전송의 중간 응답은 통지하지 않는다(재전송 뒤의 최종 응답만). */
    virtual void OnSubscribeResponse(SimSession* /*s*/, const std::string& /*event*/, const std::string& /*resource*/, int /*iSipStatus*/) {}
    /** dialog 이벤트 NOTIFY(RFC 4235) — watched = 감시 대상 AoR 사용자부, state = early|confirmed|terminated(dialog 요소가 없으면 빈 값),
     *  callId = 그 dialog 의 Call-ID. INVITE-Replaces/Join 은 이 값으로 대상 다이얼로그를 가리킨다(RFC 3891/3911). */
    virtual void OnDialogNotify(SimSession* /*s*/, const std::string& /*watched*/, const std::string& /*state*/, const std::string& /*callId*/) {}
    /** PTT 착신(그룹 fan-out INVITE)에 자동응답 200 을 냈다 — 이 단말이 그룹 세션에 합류한 시각. */
    virtual void OnCallAnswered(SimSession* /*s*/, const std::string& /*callId*/) {}
    /** floor 제어 메시지 수신(TS 24.380 §8.2 subtype: 1 Granted · 2 Taken · 3 Deny · 5 Idle · 6 Revoke · 9 Queue Position Info).
     *  tUs = 수신 시각(µs, system_clock) — floor 수신 스레드에서 불린다. 지연 지표는 스케줄러 틱이 아니라 이 시각으로 잰다. */
    virtual void OnFloor(SimSession* /*s*/, int /*iSubtype*/, long long /*tUs*/) {}
    // ── MCData SDS(TS 24.282 — SIP MESSAGE multipart) — 계측기 워커 단계 sds_send/sds_recv (test_instrument.md §4) ──
    /** 자기 SDS MESSAGE 의 최종 응답(401 Digest 재전송 뒤의 최종만) — msMs = 송신 → 응답. */
    virtual void OnSdsResponse(SimSession* /*s*/, const std::string& /*msgId*/, int /*iSipStatus*/, long long /*ms*/) {}
    /** SDS MESSAGE 수신(200 은 libcsim 이 낸다) — from = 발신자 사용자부, group = 그룹 SDS 면 그룹 id(1:1 이면 빈 값), dispReq = disposition 요청(0/1/2/3). */
    virtual void OnSdsRecv(SimSession* /*s*/, const std::string& /*from*/, const std::string& /*msgId*/, const std::string& /*group*/,
                           const std::string& /*text*/, int /*dispReq*/) {}
    /** 자기가 보낸 SDS 에 대한 SDS NOTIFICATION 수신(notifType 2 = delivered) — disposition 회신율의 분자. */
    virtual void OnSdsNotification(SimSession* /*s*/, const std::string& /*msgId*/, int /*notifType*/) {}
    // ── MCData FD(파일 배포 — TS 23.282 §7.4 HTTP 콘텐츠 서버 + TS 24.282 §15.1.3 FD SIGNALLING MESSAGE) — 계측기 워커 단계 fd_send/fd_recv ──
    /** 자기 FD 의 콘텐츠 업로드(IdMS 토큰 → POST /mcdata/fd) 결과 — iHttpStatus 201 정상(그 뒤 FD SIGNALLING MESSAGE 를 내고 최종 응답은 OnSdsResponse),
     *  0 = 접속 실패, 401 토큰 취득 실패, 403 scope·allow_fd·멤버십, 413 크기. ms = 토큰 취득 포함 업로드 시간, bytes = 올린 크기. */
    virtual void OnFdUpload(SimSession* /*s*/, const std::string& /*msgId*/, int /*iHttpStatus*/, long long /*ms*/, long long /*bytes*/) {}
    /** FD SIGNALLING MESSAGE 수신(200 은 libcsim 이 낸다) — fileUrl/fileName/fileSize/fileType = Payload FILEURL·Metadata IE. group = 그룹 FD 면 그룹 id.
     *  기본 구현은 OnSdsRecv(text = URL, dispReq 0) 로 넘긴다 — media SDS 의 FILEURL 폴백을 SDS 도착으로 보는 관측자와 호환. */
    virtual void OnFdRecv(SimSession* s, const std::string& from, const std::string& msgId, const std::string& group, const std::string& fileUrl,
                          const std::string& /*fileName*/, long long /*fileSize*/, const std::string& /*fileType*/) { OnSdsRecv(s, from, msgId, group, fileUrl, 0); }
    /** DownloadFd 결과 — iHttpStatus 200 정상(bytes = 받은 크기), 0 = 접속 실패·URL 오류, 401 토큰 취득 실패. ms = 요청 → 완료. */
    virtual void OnFdDownload(SimSession* /*s*/, const std::string& /*msgId*/, int /*iHttpStatus*/, long long /*bytes*/, long long /*ms*/) {}
    /** media plane(MSRP, TS 24.282 §9.2.3)으로 도착한 SDS — 기본은 OnSdsRecv 와 같게 다룬다(계측기는 경로를 따로 센다). */
    virtual void OnSdsMediaRecv(SimSession* s, const std::string& from, const std::string& msgId, const std::string& group,
                                const std::string& text, int dispositionReq) { OnSdsRecv(s, from, msgId, group, text, dispositionReq); }
};

#endif
