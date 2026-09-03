// libcimsue — Engine (ue_sdk.md §4.2)
//
// 프로세스당 1개. 명령은 어느 스레드에서 불러도 되며 코어 제어 스레드(`ue-ctl`)에서 직렬 실행된 뒤
// 즉시 결과(Result/id)를 돌려준다. 프로토콜 진행은 Listener 이벤트로 온다.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cimsue/export.h"
#include "cimsue/listener.h"
#include "cimsue/types.h"

namespace cimsue {

class CIMSUE_API Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /** 엔진 기동 — transport(UDP/TCP/TLS) 생성·코덱 정합·장치 준비. listener 는 stop() 까지 유효해야 한다. */
    Result start(const EngineConfig& cfg, Listener* listener);
    /** 모든 호·계정 정리 후 종료. 이후 start() 로 재기동 가능. */
    void stop();
    bool running() const;

    // ── 계정 ──
    /** 계정 추가(등록은 하지 않음). 반환 accountId ≥ 0, 실패 -1. */
    int addAccount(const AccountConfig& cfg);
    Result registerAccount(int accountId);
    Result unregisterAccount(int accountId);
    /** 즉시 재-REGISTER(서버 재기동 등으로 등록을 잃은 경우 복구). */
    Result refreshRegistration(int accountId);
    Result removeAccount(int accountId);
    RegInfo regInfo(int accountId) const;
    std::vector<int> accounts() const;

    // ── 호 (VoLTE 1:1) ──
    /** 발신. target 은 번호(도메인 자동 결합) 또는 sip: URI. 반환 callId ≥ 0, 실패 -1. */
    int dial(int accountId, const std::string& target, const CallOptions& opts = CallOptions());
    Result answer(int callId, const CallOptions& opts = CallOptions());
    Result reject(int callId, int statusCode = 486);
    Result hangup(int callId);
    Result hold(int callId);
    Result resume(int callId);
    /** 마이크 → 호 송신 차단/복구. MCPTT 세션에서는 floor 가 마이크를 게이트하므로 무시된다. */
    Result setMuted(int callId, bool muted);
    /** 호 → 스피커 청취 on/off (멀티 채널 듣기 정책). */
    Result setListen(int callId, bool listen);
    /** 수신 음량(1.0=원음, 0=무음). */
    Result setRxLevel(int callId, float level);
    Result sendDtmf(int callId, const std::string& digits);
    CallInfo callInfo(int callId) const;
    std::vector<int> calls() const;
    /** 오디오 스트림 RTP/RTCP 통계(동기 조회). 종료된 호는 소멸 시점의 최종 통계. */
    StreamStats streamStats(int callId) const;

    // ── MCPTT 그룹콜·사설콜 (TS 24.379) ──
    /** 그룹콜 참여(발신 INVITE, multipart mcptt-info[+resource-lists], SDP m=application floor).
     *  groupId 는 bare id(예 "g001"). 반환 callId. 이미 같은 그룹 세션이 있으면 그 callId. */
    int joinGroupCall(int accountId, const std::string& groupId, const GroupCallOptions& opts = GroupCallOptions());
    /** 1:1 사설콜(session-type=private). peer 는 bare 번호. fullDuplex 면 mc_no_floor_ctrl. */
    int startPrivateCall(int accountId, const std::string& peer, const GroupCallOptions& opts = GroupCallOptions());
    /** 세션 이탈(BYE). */
    Result leaveGroupCall(int callId) { return hangup(callId); }
    /** PTT down — Floor Request. 응답은 onFloor(Granted/Denied/QueuePosition). priority<0 = 미기재. */
    Result floorRequest(int callId, int priority = -1);
    /** PTT up — Floor Release(대기 중이면 Queued Cancel 선행). */
    Result floorRelease(int callId);
    Result floorQueueCancel(int callId);
    FloorInfo floorInfo(int callId) const;

    /** affiliation PUBLISH(TS 24.379 §9, Event: mcptt). on=false 면 Expires:0. 반환 token(onRequestResult 상관). */
    long affiliate(int accountId, const std::string& groupId, bool on);
    /** 그룹 로스터 구독(RFC 4575 conference, 엔진 패치 evsub) — 확인 신호는 onRoster NOTIFY. */
    Result subscribeConference(int accountId, const std::string& groupId, bool on);
    /** 문서 변경 구독(RFC 5875 xcap-diff) — psiUri 예 sip:gms_psi@domain. 본문은 onMessage 로. */
    Result subscribeXcapDiff(int accountId, const std::string& psiUri, bool on);
    /** 임의 SIP 요청(MESSAGE/PUBLISH/SUBSCRIBE …). 반환 token. */
    long sendRequest(int accountId, const std::string& method, const std::string& targetUri,
                     const std::string& contentType, const std::string& body,
                     const std::map<std::string, std::string>& headers = {});

    // ── 관제 (dispatch_center.md §5, volte_supplementary_services.md §5·§6) ──
    /** 대상 AoR 의 dialog 이벤트 구독(RFC 4235, 인가 = 관제 그룹 monitor_scope). NOTIFY → onDialogInfo. */
    Result dialogWatch(int accountId, const std::string& targetAor, bool on);
    /** 통화 청취 합류 — INVITE-with-Join(RFC 3911) + a=recvonly. dlg 는 onDialogInfo 로 학습한 대상 dialog.
     *  200 OK 의 a=ssrc label(caller/callee) 이 CallInfo.sources 로 온다(U10 디먹스 라벨). 반환 callId. */
    int join(int accountId, const std::string& targetUri, const DialogInfo& dlg);
    /** 당겨받기 — 피처코드 다이얼(그룹 픽업 = code, 지정 픽업 = code+number). 결과는 호 상태(200/403/404/489). */
    int pickup(int accountId, const std::string& featureCode, const std::string& number = std::string());
    /** 호 전달 blind — REFER(RFC 3515). target 은 번호 또는 URI. 진행은 onCallState(REFER 수락 후 서버가 BYE). */
    Result transfer(int callId, const std::string& target);
    /** 호 전달 attended — Refer-To 에 Replaces(consultCallId 의 dialog). */
    Result transferAttended(int callId, int consultCallId);

    // ── MCData SDS (TS 24.282 §9.2.2 C-plane) ──
    /** 그룹 SDS 발신(MESSAGE multipart). 반환 msgId(UUID hex32), 실패 빈 문자열. */
    std::string sendGroupSds(int accountId, const std::string& groupId, const std::string& text,
                             bool requestDelivery = true);
    /** SDS disposition 통지(1:1 대상 peer bare 번호). notifType 1~4. */
    Result sendSdsNotification(int accountId, const std::string& peer, const std::string& convId,
                               const std::string& msgId, int notifType);

    // ── 장치 ──
    std::vector<AudioDeviceInfo> audioDevices() const;
    /** 장치 목록 재열거(핫플러그 뒤). 플랫폼 SDK 가 장치 변경 통지(WM_DEVICECHANGE 등)에서 부른다. */
    Result refreshAudioDevices();
    /** 캡처/재생 장치 선택(pjmedia 장치 id). -1=기본 캡처, -2=기본 재생. */
    Result setAudioDevices(int captureDev, int playbackDev);
    /** 추가 재생 라우트 — 두 번째 재생 장치를 재생 전용으로 브리지에 연다(관제석 헤드셋+스피커 분리 출력,
     *  ue_sdk.md §6). 마이크는 기본 캡처 장치 하나만 쓴다. 반환 routeId ≥ 1, 실패 -1. 기본 재생 장치 = 라우트 0. */
    int addPlaybackRoute(int playbackDev);
    /** 라우트 닫기. 이 라우트에 붙은 호는 라우트 0 으로 되돌아간다. */
    Result removePlaybackRoute(int routeId);
    /** 호의 수신 음성을 재생할 라우트 선택(0=기본). 활성 호면 즉시 재결선. */
    Result setCallRoute(int callId, int routeId);

    static std::string version();

    /** 내부 구현(pImpl). 앱·플랫폼 SDK 는 사용하지 않는다. */
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace cimsue
