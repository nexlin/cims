// libcimsue — 공개 타입 (docs/design/features/ue_sdk.md §4.2)
//
// 이 헤더는 pjsua2 타입을 include 하지 않는다 — 플랫폼 SDK·바인딩(SWIG)의 정본 표면이다.
// 식별자: 계정=accountId(코어 발급), 호=callId(엔진 발급). 앱은 이 id 만 되돌려 쓴다.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cimsue/export.h"

namespace cimsue {

enum class Transport { UDP, TCP, TLS };
enum class AuthScheme { Digest, Aka };
/** 미디어 SRTP(SDES) 정책 — 접속서비스 media_srtp 와 같은 값 (media_security.md §7.2). */
enum class MediaSecurity { Off, Optional, Required };
enum class RegState { Unregistered, Registering, Registered, Failed };
enum class CallState { Null, Outgoing, Incoming, Active, Held, Disconnected };
enum class CallDir { Outgoing, Incoming };

/** 명령의 즉시 결과 — 인자·상태 오류. 프로토콜 결과는 Listener 이벤트로 온다. */
struct Result {
    bool ok = true;
    int code = 0;             // 0 = ok, 그 외 = 코어/pjsua 오류 코드
    std::string reason;
    static Result success() { return Result{}; }
    static Result fail(int code, const std::string& reason) { return Result{false, code, reason}; }
};

/** 엔진(프로세스당 1개) 설정. */
struct EngineConfig {
    std::string userAgent = "CIMS-UE/libcimsue";
    int logLevel = 4;                 // pjsip 로그 레벨 (0~6) → Listener::onLog
    /** SIP TLS·HTTPS 공용 신뢰 앵커(PEM). 비면 시스템 기본/검증 불가. */
    std::string tlsCaPem;
    bool tlsVerifyServer = true;
    /** 오디오 장치 없이 동작(헤드리스 — cimsue-cli·CI). 브리지는 null 장치가 구동한다. */
    bool nullAudioDevice = false;
    /** VAD(무음 억제) 비활성 — 침묵 중에도 RTP 연속 송신(NAT flow 상태 유지). */
    bool noVad = true;
    /** RFC 3261 §18.1.1 UDP→TCP 자동 승격(요청 ≥1300 B 면 TCP 로 송신) 비활성 — true 면 큰 요청도 UDP 로 보낸다(IP 프래그먼트).
     *  통제된 망(프래그먼트 통과) 전용 사이트 옵션(sip_tls_signaling.md §3). 프로세스 전역(pjsip_cfg). */
    bool udpNoTcpSwitch = false;
    int udpPort = 0;                  // 0 = 임의 포트
    int tcpPort = 0;
    int tlsPort = 0;
    /** 미디어 클럭·프레임 — pjsua 기본(16kHz/20ms). AMR-WB 정합. */
    unsigned clockRate = 16000;
    /** Floor Granted 뒤 마이크를 여는 지연(ms, 0 = 즉시) — 앱이 승인 톤을 재생하는 길이(android_ue_client.md «삑 후 말하기»,
     *  원천 앱 350 ms). 그 사이 놓거나(Release)·회수(Revoke)·시한으로 발언을 잃으면 열지 않는다. 200 OK 승인(암묵 요청)에도 같다. */
    int grantMicDelayMs = 0;
};

/** 계정(접속서비스 kind 당 1개) 설정 — 프로비저닝 프로파일에서 채운다 (android_ue_provisioning.md). */
struct AccountConfig {
    std::string serverHost;           // CSP 접속점 IP/FQDN
    int serverPort = 5060;            // transport 의 포트 (transport 마다 다르다)
    Transport transport = Transport::UDP;
    std::string domain;               // 서비스 도메인 (AOR·IMPI 도메인부)
    std::string msisdn;               // 공개 ID(AOR user part)
    std::string imsi;                 // Digest username(IMPI) 합성용 — imsi@domain
    std::string authId;               // 전체 IMPI 직접 지정(고급). 비면 imsi@domain 합성
    std::string displayName;
    /** 인증 자료 — H(A1) 우선(평문 불요), 없으면 평문, AKA 면 K/OPc. */
    std::string ha1;                  // MD5(IMPI:realm:pw) hex32
    std::string password;
    AuthScheme authScheme = AuthScheme::Digest;
    std::string akaK, akaOpc, akaAmf = "8000";
    /** 서버 제시 채널 보호 목록(RFC 3329) — "tls" 포함 + TLS 접속이면 sec-agree 제안. */
    std::vector<std::string> secMechanisms;
    MediaSecurity mediaSecurity = MediaSecurity::Off;
    int expiresSec = 3600;
    /** REGISTER Contact 부가 파라미터(feature tag 등). */
    std::string contactParams;
    /** 영상 발신 시 카메라 자동 송신(Android). 헤드리스는 false. */
    bool videoAutoTransmit = false;
    /** MCPTT ID (TS 24.379) — floor User ID·mcptt-info calling-user-id. 비면 "tel:"+msisdn. */
    std::string mcpttId;
    /** MCPTT 착신 INVITE(mcptt-info: 그룹·private) 자동 수락 — PTT 단말 기본 동작(ptt_ue.md §12.3). */
    bool autoAnswerMcptt = true;
    /** 단말 인스턴스 ID — REGISTER Contact `+sip.instance`(TS 24.229 §5.1.1.2 · RFC 5626 §4.1), 꺾쇠 없이 URN.
     *  IMEI 를 아는 단말은 `imeiUrn()`(RFC 7254), 모르면 기기 고유 `urn:uuid:…`(RFC 4122). 비면 pjsip 기본값 —
     *  호스트명 해시라 기기마다 같을 수 있다(registration_binding_set.md §8). */
    std::string instanceId;
    /** MCPTT client ID(TS 24.379 §4.10) — 단말이 처음 쓸 때 만든 UUID URN(RFC 9562 §4.2)을 보존해 넘긴다. 긴급 경보 mcptt-info 의
     *  `<mcptt-client-id>`(§12.1.1.1 4)c). 비면 instanceId 가 `urn:uuid:` 일 때 그것을 쓰고, 아니면 싣지 않는다. */
    std::string mcpttClientId;
    /** Resource-Priority(RFC 8101 "namespace.priority") — 긴급·임박·일반 그룹콜(TS 24.379 §6.2.8.1.2·§6.2.8.1.12·§6.2.8.1.15). 값의 정본은
     *  service-config OnNetwork 의 *-resource-priority(ServiceConfigDoc) — 문서에 없으면 기본값(CSP fan-out 과 같은 mcpttp 서열)을 둔다. */
    std::string rpEmergency = "mcpttp.15";
    std::string rpImminentPeril = "mcpttp.8";
    std::string rpNormal = "mcpttp.0";
    /** 그룹 SDS 의 시그널링 평면 상한(octet) — TS 24.484 `max-payload-size-sds-cplane-bytes`(프로비저닝 `mcdata.maxPayloadSdsCplaneBytes`).
     *  본문이 넘으면 sendGroupSds 가 media plane(MSRP, TS 24.282 §9.2.3)으로 보낸다 — 서버는 초과 MESSAGE 를 403 으로 거절한다(§9.2.2 8)).
     *  0 = 제한 없음. 1:1 SDS 는 늘 시그널링 평면이다(서버 media plane 이 그룹만 받는다 — mcdata_messaging.md §4.7). */
    int maxSdsCplaneBytes = 0;
    /** 서버발 MSRP 배포를 받는다 — REGISTER Contact 의 `+g.3gpp.icsi-ref` 에 ICSI mcdata.sds 를 싣는다(코어가 contactParams 의
     *  기존 icsi-ref 목록에 합친다). false 면 서버가 큰 그룹 SDS 를 FILEURL(FD)로 폴백해 보낸다. */
    bool mcdataMsrp = false;

    std::string aor() const { return "sip:" + msisdn + "@" + domain; }
    std::string effectiveMcpttId() const { return mcpttId.empty() ? "tel:" + msisdn : mcpttId; }
    std::string effectiveMcpttClientId() const {
        if (!mcpttClientId.empty()) return mcpttClientId;
        return instanceId.rfind("urn:uuid:", 0) == 0 ? instanceId : std::string();
    }
    /** Digest username = 전체 IMPI. msisdn 폴백 없음(서버는 불일치 시 즉시 403). */
    std::string digestUsername() const {
        if (!authId.empty()) return authId;
        return imsi.empty() ? std::string() : imsi + "@" + domain;
    }
    bool isComplete() const {
        bool cred = !ha1.empty() || !password.empty() ||
                    (authScheme == AuthScheme::Aka && !akaK.empty());
        return !serverHost.empty() && serverPort > 0 && serverPort < 65536 && !domain.empty() &&
               !msisdn.empty() && !digestUsername().empty() && cred;
    }
};

/** IMEI → RFC 7254 instance URN `urn:gsma:imei:TTTTTTTT-SSSSSS-0`(TS 23.003 §13.8). 셋째 칸은 검사 숫자가 아니라 spare 라
 *  단말이 보낼 때는 항상 0 이다(RFC 7254 §4.2.3). 입력 = TAC 8 · SNR 6 에 Luhn 검사 숫자를 붙인 15자리(검증), 14자리, 또는
 *  전송 형식 15자리(끝 0). 자릿수·검사 숫자가 틀리면 빈 문자열(그 값은 기기 식별자가 아니다). */
std::string imeiUrn(const std::string& imei);

/** REGISTER `User-Agent`(RFC 3261 §20.41) 규약 — `<제품>/<앱 버전> (<OS>; <모델>)` (mcptt_management_views.md §4.1).
 *  예: `CIMS-PTT/1.4.2 (Android 15; SM-S921N)`. 빈 os·model 은 괄호 안에서 빠진다. os·model 은 comment 규칙으로 정리한다 —
 *  괄호·역슬래시는 빼고 공백·제어 문자는 공백 하나로, os 의 `;`(OS·모델 구분자)도 뺀다(기기 문자열 `Standard PC (Q35 …)` 등). */
std::string userAgentOf(const std::string& product, const std::string& version, const std::string& os,
                        const std::string& model);

struct RegInfo {
    int accountId = -1;
    RegState state = RegState::Unregistered;
    int code = 0;
    std::string reason;
    int expiresSec = 0;
};

struct CallOptions {
    bool video = false;
    bool emergency = false;
};

/** 그룹콜/사설콜(MCPTT) 개시 옵션 (TS 24.379). */
struct GroupCallOptions {
    bool emergency = false;           // mcptt-info emergency-ind=true
    bool imminentPeril = false;       // mcptt-info imminentperil-ind=true
    /** 청취 전용 합류(a=recvonly) — 관제 PTT 청취(dispatch_center.md §5.6). floor 요청 불가. */
    bool listenOnly = false;
    /** 전이중 1:1(mc_no_floor_ctrl) — floor 없이 마이크 상시 개방. startPrivateCall 전용. */
    bool fullDuplex = false;
    /** 애드혹 임시 그룹 멤버(tel: URI) — resource-lists 로 실린다. joinGroupCall 전용. */
    std::vector<std::string> members;
    /** 일제 통화 개시(TS 24.379 §4.12·§6.2.8.2) — mcptt-info `<broadcast-ind>true`. 개시자의 Floor Request 는 B-bit 를
     *  싣고(TS 24.380 §6.2.4.3.5), 개시자가 발언을 놓은 뒤 B-bit Floor Idle 을 받으면 코어가 호를 해제한다(§6.2.4.6.4).
     *  joinGroupCall 전용 — 진행 중 세션에 합류하는 INVITE 면 서버는 합류로만 다룬다(개시자 불변). */
    bool broadcast = false;
    /** 암묵적 발언 요청(TS 24.380 §14.2.5) — 개시 INVITE 의 floor SDP 에 `mc_implicit_request` 와 200 OK 승인 표시 수용
     *  `mc_granted`(§14.2.4)를 싣고, floor 는 호 성립 전부터 요청 중(`Requesting` = 'U: pending Request', §6.2.4.2.2)이다.
     *  answer 의 `mc_granted`(§14.3.4) 또는 이어 오는 Floor Granted 로 `Speaking`, 서버가 받지 않으면(진행 중 호 합류·chat — §14.3.5)
     *  코어가 명시 Floor Request 로 잇는다. 승인 전·호 성립 전에 floorRelease 하면 발언권을 돌려준다(Release 는 answer 에서).
     *  누르는 동안 개시하고 말하는 한 버튼 발신(일제 통화 등)용. listenOnly·fullDuplex 에는 뜻이 없어 무시한다. */
    bool implicitFloorRequest = false;
};

/** 착신 INVITE 의 mcptt-info(TS 24.379 §F.1) 요약. */
struct McpttInfo {
    bool present = false;
    std::string sessionType;          // prearranged/chat/private/first-to-answer/ambient-listening/adhoc (TS 24.379 Annex F.1)
    std::string requestUri, callingUserId, callingGroupId;
    bool emergency = false, imminentPeril = false;
    bool broadcast = false;           // <broadcast-ind> — 일제 통화(호 속성, 그룹 종류 아님)
    bool privateCall = false;
    bool noFloorCtrl = false;         // fmtp mc_no_floor_ctrl — 전이중 1:1
};

/** MCPTT 세션 조건 — 그룹의 긴급·임박 상태(TS 24.379 §6.2.8.1 MEG/MIG)를 이 호에서 본 값. 개시 mcptt-info 로 시작해
 *  Engine::setCallCondition(상향·하향 re-INVITE, §10.1.1.2.1.3~5)과 서버 재광고(수신 re-INVITE §10.1.1.2.1.6 · 조인 200 OK 동봉)로 바뀐다.
 *  긴급이 임박을 대체한다(둘이 함께 true 가 되지 않는다). */
struct McpttCondition {
    bool emergency = false;
    bool imminentPeril = false;
    bool mine = false;                // 이 단말이 올린 조건(개시 옵션·상향) — 하향·서버 해제로 내려간다
    bool pending = false;             // 상향·하향 re-INVITE 응답 대기
    int lastCode = 0;                 // 마지막 상향·하향 re-INVITE 의 최종 응답(Confirmed·Denied — 로컬 송신 실패 = 0)
};
/** onMcpttCondition 의 계기. */
enum class ConditionCause {
    Local,                            // setCallCondition — 보내면서 곧바로 반영(응답 전)
    Confirmed,                        // 그 re-INVITE 의 2xx
    Denied,                           // 그 re-INVITE 의 4xx~6xx — 이전 값으로 되돌렸다(§6.2.8.1.5, 미인가 상향 = 403 §6.3.3.1.14)
    Advertised                        // 서버 재광고(수신 re-INVITE·200 OK 의 mcptt-info, §6.3.3.1.15·§6.3.3.1.16)
};

/** 긴급 경보·긴급 통지 수신(TS 24.379 §12.1.1.3 — SIP MESSAGE, mcptt-info). 지시자는 1 = true, -1 = false, 0 = 요소 없음. */
struct EmergencyAlert {
    int accountId = -1;
    std::string groupId;              // bare — <mcptt-calling-group-id>, 없으면 <mcptt-request-uri>(서버가 원본 본문을 중계하는 경우)
    std::string userId;               // bare 발신자 — <mcptt-calling-user-id>, 없으면 From
    std::string originatedBy;         // bare — 제3자 취소가 가리키는 원 경보 발신자(<originated-by>)
    std::string mcOrg;                // 발신자 조직(<mc-org>)
    int alertInd = 0;                 // 1 경보 · -1 경보 취소 · 0 경보 요소 없는 그룹 상태 통지
    int emergencyInd = 0;             // 그룹의 진행 중 긴급 상태(§12.1.1.3 3)·4))
    int imminentPerilInd = 0;         // 그룹의 진행 중 임박 상태(§12.1.1.3 5)·6))
    bool self = false;                // 발신자가 이 계정 — 에코(앱은 보통 무시)
};

/** 한 호 안의 RTP 소스(SSRC) — U10 디먹스 산출. 감청 leg 는 RFC 5576 label 로 화자 귀속. */
struct MediaSource {
    uint32_t ssrc = 0;
    std::string label;
    bool active = false;
    float level = 0.f;
};

struct CallInfo {
    int callId = -1;
    int accountId = -1;
    CallDir dir = CallDir::Outgoing;
    CallState state = CallState::Null;
    std::string remoteUri;
    /** 착신 INVITE 의 P-Called-Party-ID(RFC 3455) — 대표번호 착신 식별(dispatch_center.md §4.3). */
    std::string calledParty;
    bool video = false;
    bool mediaActive = false;
    bool muted = false;
    bool listen = true;
    /** 수신 음성 재생 라우트 — 0=기본 재생 장치, 그 외 Engine::addPlaybackRoute 가 준 id(관제석 스피커 등). */
    int playbackRoute = 0;
    /** 이 호에서 듣는 크기(setRxLevel, 1 = 원음) — 코어가 기억해 미디어 결선(재협상·보류 해제)마다 다시 건다. */
    float rxLevel = 1.f;
    int lastCode = 0;
    std::string lastReason;
    std::vector<MediaSource> sources;
    // ── MCPTT ──
    bool isMcptt = false;             // 그룹콜/사설콜 세션(floor 평면 있음 또는 mc_no_floor_ctrl)
    std::string groupId;              // 그룹 id(bare) 또는 사설콜 상대(bare)
    McpttInfo mcptt;                  // 개시·착신 INVITE 의 mcptt-info(호 종류 — 이후 불변)
    McpttCondition condition;         // 세션 조건의 현재값(긴급·임박 — 바뀌면 onMcpttCondition)
    bool halfDuplex = false;          // floor 로 마이크를 게이트한다(Granted 에서만 송신)
    bool listenOnly = false;          // a=recvonly 청취 leg (PTT 청취·감청 Join)
    std::string joinedDialog;         // INVITE-Join 으로 합류한 대상 dialog 의 Call-ID
};

// ── floor (TS 24.380 participant) ──
enum class FloorState { Idle, Requesting, Speaking, Listening, Queued };
/** 오디오 라우트(pjmedia OUTPUT_ROUTE·INPUT_ROUTE) — 입력의 Earpiece = 내장 기본(하단) 마이크 고정, Default = 정책(고정 해제). */
enum class AudioRoute { Default, Earpiece, Loudspeaker };
/** 마이크 AGC 기본 목표 — ITU-T P.56 활성 레벨 -26 dBov(ue_audio_level.md §4). */
constexpr double kMicAgcTargetDbov = -26.0;

struct Talker {
    std::string id;                   // MCPTT ID (서버 표기)
    uint32_t ssrc = 0;                // 화자 RTP SSRC (U10 디먹스 키), 0=미상
    bool self = false;
};

/** floor participant 이벤트 — 상태 전이와 함께 온다. */
struct FloorEvent {
    enum class Kind {
        Granted, Denied, Idle, Taken, TalkerLeft, Revoked, QueuePosition, QueueCancelled,
        RequestTimeout,               // 요청 후 응답 없음(코어 타이머) → Idle 복귀
        TalkLimit,                    // Granted Duration 마감 임박/도달 — 코어가 스스로 Release
        Other
    };
    Kind kind = Kind::Other;
    int callId = -1;
    FloorState state = FloorState::Idle;
    int durationSec = -1;             // Granted
    int cause = -1;                   // Denied/Revoked/QueueCancelled(result)
    std::string causeText;
    int indicator = 0;                // Floor Indicator 비트
    int permission = -1;              // Taken: Permission to Request the Floor (0=요청 불가)
    int queuePosition = -1;
    bool meSpeaking = false;
    std::vector<Talker> talkers;      // 현재 화자 집합
    int rawType = -1;
};

struct FloorInfo {
    FloorState state = FloorState::Idle;
    std::vector<Talker> talkers;
    bool canRequest = true;           // Taken Permission=0 이면 false
    int indicator = 0;
    int queuePosition = -1;
    int localPort = 0;                // SDP m=application 에 광고한 포트
    std::string remoteIp;             // CMP floor 목적지(SDP 학습)
    int remotePort = 0;
    unsigned grantedCount = 0, takenCount = 0, denyCount = 0;   // 누계 — 검증용
};

/** 임의 SIP 요청(PUBLISH 등)의 최종 응답. */
struct RequestResult {
    int accountId = -1;
    int64_t token = 0;
    std::string method;
    int code = 0;
    std::string reason;
    std::string etag;                 // SIP-ETag (PUBLISH)
};

/** 감시 대상의 dialog 상태 (RFC 4235 dialog-info) — 관제 BLF·INVITE-Join 대상 식별 (dispatch_center.md §5.2·§5.3). */
struct DialogInfo {
    int accountId = -1;
    std::string watched;              // dialog-info entity(감시 대상 AoR)
    std::string id, callId, localTag, remoteTag;
    std::string direction;            // initiator|recipient
    std::string state;                // trying|proceeding|early|confirmed|terminated
    std::string remoteIdentity;
    bool full = false;
    /** Join 헤더 값 — cspsim/CSP 규약: <call-id>;to-tag=<remote-tag>;from-tag=<local-tag> (MatchDialog 는 양방향 대조). */
    std::string joinHeader() const {
        std::string j = callId;
        if (!remoteTag.empty()) j += ";to-tag=" + remoteTag;
        if (!localTag.empty()) j += ";from-tag=" + localTag;
        return j;
    }
};

/** 회의 로스터 항목 (RFC 4575 conference-info). */
struct RosterEntry {
    std::string uri;
    std::string status;               // connected/disconnected/…
};

/** MCData SDS (TS 24.282) — 수신 메시지·disposition 통지·FD. */
struct SdsMessage {
    int accountId = -1;
    std::string fromUri;
    std::string groupUri;             // 그룹 SDS·FD 의 mcdata-info request-uri. 1:1(request-type one-to-one-*)은 빈 값 — request-uri 가 받는 사람(나)이다
    std::string convId, msgId;        // UUID hex32
    int64_t timeSec = 0;
    int dispositionReq = 0;           // 0 없음 / 1 delivery / 2 read / 3 both
    std::string text;
    bool notification = false;        // SDS NOTIFICATION
    int notifType = 0;                // 1 undelivered / 2 delivered / 3 read / 4 delivered+read
    bool fd = false;                  // FD SIGNALLING (파일 URL)
    bool mediaPlane = false;          // media plane(MSRP) 배포로 받았다(TS 24.282 §9.2.3)
    std::string fileUrl, fileName, fileType;
    int64_t fileSize = 0;
};

/** MCData FD 로 알릴 파일 — FD SIGNALLING PAYLOAD 의 Payload(FILEURL)·Metadata(name/size/type) (TS 24.282 §15.1.3·
 *  mcdata_messaging.md §4.5). url 은 콘텐츠 서버 업로드 결과(CscClient::uploadFd 의 FdUpload.url). */
struct FdFile {
    std::string url, name, type;      // type = MIME(비면 application/octet-stream)
    int64_t size = 0;
};

/** SDS 발신의 즉시 결과. 최종 응답은 onRequestResult(MESSAGE, token) 으로 오므로 앱이 token 으로 상관한다
 *  (disposition 통지 발신과 구분). 출력 인자를 두지 않는 이유는 ue_sdk.md / android_dispatch_tablet.md §3.3. */
struct SdsSend {
    bool ok = false;
    int code = 0;                     // 0 = ok, 그 외 = 코어 오류 코드
    std::string reason;
    std::string msgId;                // 그룹 SDS 가 생성한 UUID hex32. disposition 통지는 빈 문자열
    int64_t token = -1;               // onRequestResult 상관 키. 요청을 만들지 못했으면 -1
};

struct StreamStats {
    unsigned rxPackets = 0, rxBytes = 0, rxLoss = 0, rxDiscard = 0;
    unsigned txPackets = 0, txBytes = 0;
    unsigned rxJitterUs = 0;          // 수신 지터 평균(µs, RFC 3550 A.8 — pjmedia rtcp rxStat.jitter.mean)
    bool valid = false;
};

/** 호 품질의 한 방향(ue_voice_quality.md §3.1). rx = 내가 받은 스트림(자기 측정), remote = 상대가 받은 내 스트림
 *  (상대 RTCP RR — RFC 3550 §6.4.2 — 에 XR VoIP Metrics(RFC 3611 §4.7)가 있으면 폐기율·버스트/갭까지). 비율은 %, 값이 없으면 -1. */
struct QualityDirection {
    bool valid = false;
    unsigned packets = 0;             // rx = 받은 패킷 · remote = 내가 보낸 패킷
    unsigned lost = 0;                // 망 손실(RFC 3550 A.3) — remote 는 상대 RR 의 누적 손실
    unsigned discarded = 0;           // 지터버퍼 폐기(늦음·넘침) — remote 는 XR 폐기율에서 환산
    double lossPct = -1;              // lost / (packets + lost)
    double discardPct = -1;
    double jitterMs = -1;             // 평균 지터(RFC 3550 A.8)
    double jitterMaxMs = -1;
    double burstDensityPct = -1;      // XR burst density(Gmin 16) — 손실이 몰린 구간의 손실·폐기 밀도
    double gapDensityPct = -1;        // XR gap density
    int burstMs = -1, gapMs = -1;     // XR 평균 버스트/갭 길이
    int signalDbm = 127, noiseDbm = 127;   // XR 신호·잡음 레벨(dBm0), 127 = 없음
};

/** 호 품질 스냅샷(ue_voice_quality.md §3.3) — Engine::callQuality. MOS 는 ITU-T G.107/G.107.1 E-model 추정(POLQA/PESQ 아님).
 *  종료된 호는 마지막 스트림 소멸 시점의 값이고, 전달·재협상으로 스트림이 바뀌면 패킷·손실·폐기를 누적한다. */
struct CallQuality {
    bool valid = false;               // 오디오 스트림을 한 번이라도 가졌다
    std::string codec;                // 협상 코덱(rtpmap encoding name — "AMR-WB")
    unsigned clockRate = 0;
    bool wideband = false;            // G.107.1 광대역 척도(AMR-WB·G.722)
    QualityDirection rx;
    QualityDirection remote;
    double rtdMs = -1;                // 왕복 지연(RTCP LSR/DLSR, RFC 3550 §6.4.1) — CMP relay 가 RTCP 를 중계하므로 종단 RTT
    double esdMs = -1;                // 자기 단말 지연 = 지터버퍼 평균 지연 + 코덱 프레임·lookahead + 장치 추정
    double oneWayMs = -1;             // E-model 단방향 입→귀 지연 Ta = RTD/2 + 자기 ESD + 상대 ESD 추정
    double rLq = -1, rCq = -1;        // R — 코덱 대역의 원 척도(광대역 0~129). LQ = 지연 손상 제외, CQ = 지연 포함
    double mosLq = -1, mosCq = -1;    // MOS 1.0~4.5 (광대역은 R/1.29 로 협대역 척도에 옮겨 계산)
    int64_t startEpochMs = 0;         // 첫 스트림 시작(UTC epoch ms)
    int64_t durationMs = 0;           // 스트림이 있던 누적 시간
};

/** 서버(peer) 인증서 만료 관측 — 마지막 성공 TLS 핸드셰이크에서 본 상대 인증서의 notAfter
 *  (sip_tls_signaling.md §8.6.2 관제조작반 경고). SIP TLS(Engine)·HTTPS(CscClient) 각각 관측한다.
 *  임계는 서버와 같다: 잔여 ≤ 30일 = 경고(자동 갱신 실패 신호), ≤ 7일 = 위험. */
struct TlsPeerExpiry {
    bool valid = false;               // 관측 있음
    int64_t notAfterEpoch = 0;        // UTC epoch 초
    int64_t observedEpoch = 0;        // 관측 시각(UTC epoch 초)
    std::string subject;              // peer 인증서 subject(한 줄)
    std::string remote;               // 관측한 상대(host:port)
    /** 잔여 일수(now 기준, 음수 = 만료). valid 아니면 0 — 호출자가 valid 를 본다. */
    int daysLeft(int64_t now) const { return valid ? (int)((notAfterEpoch - now) / 86400) : 0; }
};

struct AudioDeviceInfo {
    int id = -1;
    std::string name;
    std::string driver;
    unsigned inputCount = 0;
    unsigned outputCount = 0;
};

/** 영상 장치(pjmedia videodev) — 캡처(카메라)·렌더. Android 카메라 driver = "Android"(Camera2), 합성 장치(Colorbar)는 driver 로 가린다. */
struct VideoDeviceInfo {
    int id = -1;
    std::string name;
    std::string driver;
    bool capture = false;
    bool render = false;
};

CIMSUE_API const char* toString(RegState s);
CIMSUE_API const char* toString(CallState s);
CIMSUE_API const char* toString(Transport t);
CIMSUE_API const char* toString(FloorState s);
CIMSUE_API const char* toString(FloorEvent::Kind k);
CIMSUE_API const char* toString(ConditionCause c);

}  // namespace cimsue
