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
    /** 즉시 재-REGISTER(서버 재기동 등으로 등록을 잃은 경우 복구). 망이 바뀐 경우는 `handleNetworkChange()`. */
    Result refreshRegistration(int accountId);
    /**
     * 망이 바뀌었다(기본 망 전환·끊겼다 복귀) — 플랫폼이 알리고 **코어가 등록을 되살린다**.
     *
     * TCP/TLS 연결을 닫아(옛 망의 연결을 재사용하지 않게) 등록을 켠 계정마다 다시 REGISTER 한다. 앞 등록 트랜잭션이 걸려
     * 있으면(PJSIP_EBUSY) 겹쳐 보내지 않고 그것이 끝난 뒤 한 번 더 보낸다(RFC 3261 §10.2). 일반 등록 경로라 실패하면 계정의
     * 자동 재시도가 그대로 돈다. UDP 는 닫지 않는다 — 낡은 Via/Contact 는 rport·Contact 재작성이 고친다. 진행 중 호는
     * 건드리지 않는다(호 유지 정책은 별도, ue_sdk.md §11). 여러 번 불러도 계정마다 미뤄 둔 재등록은 하나다.
     * `refreshRegistration()` 은 망은 그대로인데 등록만 잃은 경우(서버 재기동)의 복구다.
     */
    Result handleNetworkChange();
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
    /** 마이크 → 호 송신 차단/복구. 반이중 MCPTT 세션에서는 floor 가 마이크를 게이트하므로 무시되고, 전이중 사설콜
     *  (mc_no_floor_ctrl)에서는 앱의 PTT 로컬 게이트로 쓴다(누르면 승인 톤 뒤 false, 떼면 true). MCVideo 호는 송출 허가 중
     *  음성 송신만 멈춘다(오디오 인코더 정지 — 영상은 계속, 마이크 경합 정책 mcvideo.md §7 D12). */
    Result setMuted(int callId, bool muted);
    /** 호 → 스피커 청취 on/off (멀티 채널 듣기 정책). */
    Result setListen(int callId, bool listen);
    /** 수신 음량 — 이 호에서 **듣는** 크기(1.0=원음, 0=무음). 보내는 크기는 바꾸지 않는다(마이크 레벨은
     *  엔진 AGC 가 맞춘다 — ue_audio_level.md). 값은 호에 기억되어 오디오가 아직 없거나(성립 전·보류) 재협상으로 스트림이
     *  바뀌어도 다음 결선에 걸린다(채널 음량 — 앱이 매번 다시 걸 필요가 없다). 음수는 실패. */
    Result setRxLevel(int callId, float level);
    Result sendDtmf(int callId, const std::string& digits);
    CallInfo callInfo(int callId) const;
    std::vector<int> calls() const;
    /** 오디오 스트림 RTP/RTCP 통계(동기 조회). 종료된 호는 소멸 시점의 최종 통계. */
    StreamStats streamStats(int callId) const;
    /** 호 품질(손실·폐기·지터·RTD·E-model MOS — ue_voice_quality.md §3). 동기 조회, 종료된 호는 마지막 값. 오디오가 없으면 valid=false. */
    CallQuality callQuality(int callId) const;
    /** 송출 원천 — 빈 문자열 = 마이크, 경로 = WAV(PCM 16-bit) 반복 재생을 마이크 대신 모든 호로(시험 모드 기준 음원, ue_voice_quality.md §4.2).
     *  진행 중 호에도 즉시 적용된다. 음소거·floor 게이트는 원천과 무관하게 그대로다. */
    Result setTxSource(const std::string& wavPath);

    // ── 관찰자 ──
    /** 주 리스너(start 인자) 뒤에 같은 이벤트를 받는 관찰자 — 구동 세션·계측 링크(ue_voice_quality.md §5.3). 이벤트 스레드에서 불린다.
     *  removeObserver 는 진행 중 전달이 끝날 때까지 기다린다(이벤트 콜백 안에서 불러도 된다). */
    void addObserver(Listener* observer);
    void removeObserver(Listener* observer);

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
    /** 내 대기 요청 취소 = Floor Release(TS 24.380 §6.2.4.9.6) — 대기 중이 아니면 아무것도 안 한다. */
    Result floorQueueCancel(int callId);
    /** 대기열 위치 요청(Floor Queue Position Request, §6.2.4.9.9) — 대기 중일 때. 답은 floor 이벤트 QueuePosition. */
    Result floorQueuePosition(int callId);
    /** 계정의 발언권 참여자 타이머를 바꾼다(UE initial configuration 이 바뀌었을 때 — AccountConfig.floorTimers). 다음 MCPTT 호부터. */
    Result setFloorTimers(int accountId, const FloorTimers& timers);
    /** 계정의 MCVideo 전송 제어 참여자 타이머를 바꾼다(MCVideo service configuration 이 바뀌었을 때 — AccountConfig.tcTimers). 다음 MCVideo 호부터. */
    Result setTcTimers(int accountId, const McVideoTcTimers& timers);
    FloorInfo floorInfo(int callId) const;

    /** 진행 중 그룹콜의 조건 상향·하향(TS 24.379 §10.1.1.2.1.3~5) — in-dialog re-INVITE: multipart mcptt-info(바뀐 지시자를
     *  `emergency-ind`/`imminentperil-ind` true·false 로 명시) + Resource-Priority(§6.2.8.1.2·§6.2.8.1.12 — AccountConfig.rp*), SDP 는
     *  협상된 그대로(floor 섹션 재주입). 조건은 보내면서 반영하고(onMcpttCondition Local), 2xx = Confirmed, 4xx~6xx = 이전 값으로
     *  되돌려 Denied — 재-INVITE 거절은 호를 끊지 않는다. emergency·imminentPeril 을 함께 true 로 줄 수 없다(긴급이 임박을 대체).
     *  지시자 조합은 §6.3.3.1.17 대로 — 긴급 상향은 `emergency-ind` true + `alert-ind` false(§6.2.8.1.1 4))만 싣고 임박 지시자는 싣지
     *  않는다(임박 → 긴급이면 제어 기능이 임박을 내린다, §6.3.3.1.6 3)d)). 긴급 중 임박 상향은 실패(긴급을 먼저 해제 — §6.2.8.1.9 1)).
     *  바뀐 것이 없으면 보내지 않는다. 사설콜·응답 대기 중·성립 전 호는 실패. 해제 인가(user profile allow-cancel-*)는 앱이
     *  Capabilities 로 선차단하고 서버가 판정한다(비인가 = 403 → Denied). */
    Result setCallCondition(int callId, bool emergency, bool imminentPeril);
    /** 긴급 경보 발신·취소(TS 24.379 §12.1.1.1·§12.1.1.2) — SIP MESSAGE, mcptt-info `alert-ind`(+ `mcptt-client-id`) + ICSI mcptt
     *  (P-Preferred-Service·Accept-Contact). groupId bare. originatedBy = 다른 사용자의 경보를 취소할 때 그 사용자 MCPTT ID
     *  (§12.1.1.2 4)e)), cancelGroupEmergency = 취소와 함께 그룹의 진행 중 긴급 상태도 해제(§12.1.1.2 5) — `emergency-ind` false).
     *  반환 token(onRequestResult MESSAGE 상관), 실패 -1. 경보 인가는 서버가 판정한다(미인가 = 전파 없음). */
    int64_t sendEmergencyAlert(int accountId, const std::string& groupId, bool activate,
                               const std::string& originatedBy = std::string(), bool cancelGroupEmergency = false);
    /** affiliation PUBLISH — 서비스마다 따로다(TS 23.280 §5.2.5). Mcptt = TS 24.379 §9(Event: mcptt), McVideo = TS 24.281 §8.2
     *  (ICSI mcvideo · `urn:3gpp:ns:mcvideoPresInfo:1.0`, Request-URI = AccountConfig.mcvideoServerUri). on=false 면 Expires:0.
     *  반환 token(onRequestResult 상관), 실패 -1. */
    int64_t affiliate(int accountId, const std::string& groupId, bool on, McService service = McService::Mcptt);
    /** 그룹 로스터 구독(RFC 4575 conference, 엔진 패치 evsub) — 확인 신호는 onRoster NOTIFY. */
    Result subscribeConference(int accountId, const std::string& groupId, bool on);
    /** 문서 변경 구독(RFC 5875 xcap-diff) — psiUri 예 sip:gms_psi@domain. 본문은 onMessage 로. */
    Result subscribeXcapDiff(int accountId, const std::string& psiUri, bool on);
    /** 임의 SIP 요청(MESSAGE/PUBLISH/SUBSCRIBE …). 반환 token. */
    int64_t sendRequest(int accountId, const std::string& method, const std::string& targetUri,
                        const std::string& contentType, const std::string& body,
                        const std::map<std::string, std::string>& headers = {});

    // ── MCVideo 그룹 호 (TS 24.281 호 · TS 24.581 전송 제어, mcvideo.md §5.4) — MCPTT 호와 독립 다이얼로그 ──
    /** MCVideo 그룹 호 개시·합류 — INVITE Request-URI = AccountConfig.mcvideoServerUri(참여 MCVideo 기능 PSI), multipart
     *  mcvideo-info(session-type chat|prearranged, request-uri = 그룹) + SDP m=audio·m=video·`m=application <RTCP 포트> udp MCVideo`
     *  (TS 24.281 §6.2.1·§9.2.1.2.1.1, TS 24.581 §4.3.3.1). chat 합류가 곧 affiliation(§8.1). groupId bare. 반환 callId
     *  (CallInfo.service = McVideo), 실패 -1. 나가기 = hangup(BYE) — MCPTT 호는 그대로. */
    int joinVideoGroupCall(int accountId, const std::string& groupId,
                           const VideoGroupCallOptions& opts = VideoGroupCallOptions());
    /** [영상 보내기] — Transmission Request(MCV0, TS 24.581 §6.2.4.3.2 — T100·C100). 결과는 onTransmission(Granted·Rejected·
     *  QueuePosition). priority<0 = 미기재(기본 우선순위), 아니면 협상한 mc_priority 이하. */
    Result requestTransmission(int callId, int priority = -1);
    /** [보내기 끝] — Transmission End Request(MCV2, §6.2.4.5.3·§6.2.4.4.7·§6.2.4.9.4 — T101·C101). 대기·요청 중이면 요청을 거둔다.
     *  완료 = onTransmission(Ended). */
    Result releaseTransmission(int callId);
    /** 대기 끝에 허가된 송출의 사용자 확인(TS 24.581 §6.2.4.5.1 NOTE — AccountConfig.confirmQueuedTransmission 일 때
     *  TransmissionEvent.awaitingConfirmation). accept = 송출 시작, 아니면 허가를 거둔다(Transmission End Request). */
    Result confirmTransmission(int callId, bool accept);
    /** 대기 중인 송출 요청의 순번을 묻는다(Queue Position Request — §6.2.4.9.3). 답은 TransmissionEvent QueuePosition. */
    Result requestQueuePosition(int callId);
    /** [받기] — Receive Media Request(MCV0, §6.2.5.3.3 — T103·C103). transmitterId = onReception(Notified) 의 송출자 MCVideo ID.
     *  결과는 onReception(Granted·Rejected). priority<0 = 미기재, 아니면 협상한 mc_reception_priority 이하. */
    Result acceptReception(int callId, const std::string& transmitterId, int priority = -1);
    /** [그만 보기] — Media Reception End Request(MCV2, §6.2.5.5 — T104·C104). 완료 = onReception(Released). */
    Result endReception(int callId, const std::string& transmitterId);
    TransmissionInfo transmissionInfo(int callId) const;

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
    /** 그룹 SDS 발신(MESSAGE multipart). 반환 SdsSend{ok, msgId(UUID hex32), token}.
     *  최종 응답은 onRequestResult(MESSAGE, token) 으로 오므로 앱은 이 token 으로 상관한다(disposition 통지 발신과 구분). */
    SdsSend sendGroupSds(int accountId, const std::string& groupId, const std::string& text,
                         bool requestDelivery = true, const std::string& msgId = std::string());
    /** (sendGroupSds·sendSds 공통) msgId = 재전송이면 처음의 message ID(UUID hex32 — SDS SIGNALLING PAYLOAD 의 Message ID 라 수신 측이 같은 메시지로 대조한다),
     *  비면 새로 만든다. 앱이 저장을 먼저 하고 보낼 때도 그 ID 를 넘긴다. hex32 가 아니면 실패. */
    /** 1:1 SDS 발신(MESSAGE multipart, request-type one-to-one-sds). peer 는 상대 bare 번호.
     *  그룹과 다른 것은 셋 — request-type·request-uri(상대)·conversation ID(쌍 정렬). 서버는 등록
     *  바인딩으로 본문 그대로 전달한다(그룹 게이트 없음, mcdata_messaging.md §4 표).
     *  반환·상관 규약은 sendGroupSds 와 같다. */
    SdsSend sendSds(int accountId, const std::string& peer, const std::string& text,
                    bool requestDelivery = true, const std::string& msgId = std::string());
    /** SDS disposition 통지(TS 24.282 §12.2.1.1) — 받은 SDS 의 fromUri(= `<mcdata-calling-user-id>`)를 peer 로, groupUri(=
     *  `<mcdata-calling-group-id>`)를 groupId 로 넘긴다(1:1 이면 빈 값, bare·URI 모두 받는다). notifType 1~4.
     *  AccountConfig.mcdataServerUri 가 있으면 규격형 — Request-URI = 그 PSI, 대상 = resource-lists entry 하나, 그룹이면
     *  mcdata-info `<mcdata-calling-group-id>`, ICSI mcdata.sds Accept-Contact·P-Preferred-Service(§6.2.4.1). 없으면 SDS NOTIFICATION
     *  한 파트를 원 발신자 AoR 로 곧장(전환기 — CSP 0.2.180 전 서버). 반환 SdsSend — msgId 는 입력이므로 비어 있고 token 으로 최종
     *  응답을 상관한다. */
    SdsSend sendSdsNotification(int accountId, const std::string& peer, const std::string& convId,
                                const std::string& msgId, int notifType, const std::string& groupId = std::string());

    // ── MCData FD (TS 24.282 §10.2 — HTTP 콘텐츠 서버 경유, mcdata_messaging.md §4.5) ──
    /** 그룹 FD 알림 발신 — file 은 먼저 CscClient::uploadFd(groupId 지정)로 올린 결과. MESSAGE 본문 = mcdata-info(group-fd)
     *  + FD SIGNALLING PAYLOAD(FILEURL·Metadata). 서버가 그룹 allow_fd 로 게이트·팬아웃한다. 반환·상관 규약은 sendGroupSds 와 같다. */
    SdsSend sendGroupFd(int accountId, const std::string& groupId, const FdFile& file);
    /** 1:1 FD 알림 발신(request-type one-to-one-fd) — file 은 groupId 없이 올린 결과. peer 는 상대 bare 번호. */
    SdsSend sendFd(int accountId, const std::string& peer, const FdFile& file);

    // ── 장치 ──
    std::vector<AudioDeviceInfo> audioDevices() const;
    /** 장치 목록 재열거(핫플러그 뒤). 플랫폼 SDK 가 장치 변경 통지(WM_DEVICECHANGE 등)에서 부른다. */
    Result refreshAudioDevices();
    /** 캡처/재생 장치 선택(pjmedia 장치 id). -1=기본 캡처, -2=기본 재생. */
    Result setAudioDevices(int captureDev, int playbackDev);
    /** 캡처 게이트(§4.5) — false = 캡처 스트림을 열지 않는다(재생만 — pjsua SPEAKER_ONLY). 마이크를 다른 앱에 양보하는 구간
     *  (VoLTE 통화 중 PTT 발언 — 앱 간 양보)이나 PTT 유휴·청취에서 OS 동시 캡처 중재에서 빠질 때 쓴다. true = 전이중 복귀(기본).
     *  사운드 장치가 닫혀 있으면 모드만 두어 다음 결선에 적용된다(NO_IMMEDIATE_OPEN). 호 단위 음소거(setMuted)·floor 게이트와
     *  별개인 장치 단위 스위치이고, 장치 선택(setAudioDevices)을 넘어 유지된다. 헤드리스(nullAudioDevice)면 상태만 둔다. */
    Result setCaptureEnabled(bool on);
    bool captureEnabled() const;
    /** 장치 단 음량(ue_audio_level.md §2·§6) — speaker = 스피커 배율(bridge slot 0 → 장치 — pjsua2 slot 0 `adjustRxLevel`, 1 = 원음),
     *  micTargetDbov = 마이크 AGC 목표(-40..-10, 기본 kMicAgcTargetDbov). AGC 는 늘 켜 두고 마이크 배율은 1 로 고정한다(AGC 뒤에 곱하면
     *  목표를 흔든다). 코어가 값을 기억해 캡처 게이트 전환·장치 재오픈·호 미디어 결선 뒤 다시 건다(재오픈은 slot 0 레벨을 초기화한다). */
    Result setDeviceAudioLevels(float speaker, double micTargetDbov);
    /** 오디오 라우트 — output = 출력(스피커폰·수화기·기본), input = 입력 마이크(장치 재오픈에도 유지). PTT 는 단말 스피커·수화기로 들을 때
     *  입력을 Earpiece(내장 기본 마이크)로 고정한다 — 통화 입력은 출력이 스피커면 정책이 후면 마이크를 골라 입에 대고 말하는 무전이
     *  ~20 dB 작아진다(§3). 전이중 스피커폰(VoLTE)은 에코 때문에 Default 를 둔다. 라우트를 무시하는 단말은 플랫폼 AudioRouter 를 병행한다. */
    Result setAudioRoute(AudioRoute output, AudioRoute input);
    /** 사운드 장치 재오픈 — 열려 있으면 닫고 곧바로 다시 열어 재생·캡처 트랙을 새로 만든다(브리지 결선 유지, 짧은 공백). 게이트 모드·
     *  라우트·장치 단 음량은 그대로 이어진다. 라우팅 중이던 출력 장치가 사라질 때 재생 트랙에 시스템 뮤트가 남는 단말 대응
     *  (android_ue_client.md — 볼륨 변경으로 안 풀리고 트랙 재생성만 푼다). 닫혀 있으면 아무것도 하지 않는다(다음 개방이 새 트랙). */
    Result reopenAudioDevice();

    // ── 영상 (§4.5 — 코어는 창을 열지 않는다. Android 는 Surface 에서 얻은 창에 pjmedia 렌더러가 그리고, Windows 는 디코드 프레임을
    //    Listener::onVideoFrame 으로 넘긴다 — 창 없는 프레임 렌더 장치) ──
    /** 수신 영상 렌더 대상 — 플랫폼 창 핸들(Android = ANativeWindow*, 참조 하나를 코어가 넘겨받는다 — 파사드가 Surface 에서 얻는다).
     *  nullptr = 해제. 활성 영상 호에 곧바로 결선하고 뒤에 영상이 활성되는 호에도 쓴다. 영상 없는 빌드면 실패. 프레임 렌더 빌드(Windows)도
     *  실패한다 — 활성 영상 호마다 프레임이 onVideoFrame 으로 온다(창 없음). */
    Result setVideoWindow(void* nativeWindow);
    /** 셀프뷰 프레임(프레임 렌더 빌드만) — on 이면 내 영상을 보내는 동안 카메라 프레임을 onVideoFrame(callId -1)으로도 넘긴다.
     *  카메라는 송출이 연다 — 셀프뷰만으로 카메라를 열지 않고, 송출이 모두 멈추면 셀프뷰도 멈춘다. 다른 빌드는 실패
     *  (Android 셀프뷰 = setPreviewSurface). */
    Result setVideoPreview(bool on);
    /** 캡처 카메라 선택 — videoDevices() 의 캡처 장치 id(-1 = 기본: 이름에 front 가 있는 카메라, 없으면 첫 카메라). 다음 송출부터
     *  쓰고, 지금 송출 중인 호는 곧바로 바꾼다. 캡처 장치가 아니면 실패. */
    Result setVideoCaptureDevice(int deviceId);
    /** 캡처 카메라 전환(전면↔후면) — 활성 영상 호의 송신 장치를 다음 카메라로 바꾸고, 이후 호의 기본 장치로도 쓴다. */
    Result switchCamera(int callId);
    /** 캡처 영상 회전 — 카메라 devId(videoDevices 의 capture)의 프레임을 시계 방향 degrees(0·90·180·270) 돌려 세워 보낸다. 인코딩 크기는
     *  그대로(480x640 세로). 화면 방향과 카메라 센서 방향이 다른 단말(휴대폰 — 센서는 가로)은 걸지 않으면 가로 그림을 세로 틀에 줄여 넣어
     *  위아래가 검게 간다. Android 파사드는 기동 때 Camera2 센서 방향으로 카메라마다 계산해 건다(CimsUe.setCaptureRotation). 열린 카메라에
     *  곧바로, 이후 호에도 쓴다. 영상 없는 빌드면 실패. */
    Result setCaptureRotation(int devId, int degrees);
    /** 내 영상 송출 허용(CallInfo.videoSend) — 1:1 영상 호는 곧바로 송출을 시작·정지하고, MCVideo 호는 허용이면서 송출 허가를 가진
     *  동안만 보낸다. 재협상(re-INVITE) 없음. 영상 없는 빌드·호면 실패. 그룹 영상은 MCVideo 호다 — MCPTT 호는 음성만(mcvideo.md §8). */
    Result setVideoSend(int callId, bool on);
    /** 통화 중 영상 전환(1:1 호 — re-INVITE, RFC 3264 §8.1·§8.2). on = 영상 추가 **요청** — 상대가 받으면 onVideoRequest(Accepted)와
     *  CallInfo.video, 거절이면 Declined(음성은 그대로). off = 영상 제거 — 묻지 않는다(m=video port 0). 성립(Active) 전·보류 중·
     *  진행 중인 요청이 있으면 실패, 이미 그 상태면 그대로 성공. MC 호(MCPTT·MCVideo)는 실패 — 그룹 영상은 MCVideo 호다. */
    Result setCallVideo(int callId, bool on);
    /** 상대의 영상 추가 요청(onVideoRequest Received)에 답한다 — accept = 영상을 받는 200 OK(송출은 계정 videoAutoTransmit·
     *  setVideoSend 를 따른다), 거절 = m=video port 0 인 200 OK(음성은 그대로, RFC 3264 §6). 받은 요청이 없으면 실패. */
    Result answerVideoRequest(int callId, bool accept);
    std::vector<VideoDeviceInfo> videoDevices() const;
    /** 추가 재생 라우트 — 두 번째 재생 장치를 재생 전용으로 브리지에 연다(관제석 헤드셋+스피커 분리 출력,
     *  ue_sdk.md §6). 마이크는 기본 캡처 장치 하나만 쓴다. 반환 routeId ≥ 1, 실패 -1. 기본 재생 장치 = 라우트 0. */
    int addPlaybackRoute(int playbackDev);
    /** 라우트 닫기. 이 라우트에 붙은 호는 라우트 0 으로 되돌아간다. */
    Result removePlaybackRoute(int routeId);
    /** 호의 수신 음성을 재생할 라우트 선택(0=기본). 활성 호면 즉시 재결선. */
    Result setCallRoute(int callId, int routeId);

    /** SIP TLS 서버 인증서 만료 관측 — 마지막 성공 TLS 핸드셰이크의 peer 인증서(pjsip onTransportState).
     *  TLS 계정이 없거나 아직 연결이 없으면 valid=false. 관제조작반 요약 띠 경고의 입력(§8.6.2). */
    TlsPeerExpiry tlsPeerExpiry() const;

    static std::string version();

    /** 내부 구현(pImpl). 앱·플랫폼 SDK 는 사용하지 않는다. */
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace cimsue
