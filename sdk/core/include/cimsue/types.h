// libcimsue — 공개 타입 (docs/design/features/ue_sdk.md §4.2)
//
// 이 헤더는 pjsua2 타입을 include 하지 않는다 — 플랫폼 SDK·바인딩(SWIG)의 정본 표면이다.
// 식별자: 계정=accountId(코어 발급), 호=callId(엔진 발급). 앱은 이 id 만 되돌려 쓴다.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
    int udpPort = 0;                  // 0 = 임의 포트
    int tcpPort = 0;
    int tlsPort = 0;
    /** 미디어 클럭·프레임 — pjsua 기본(16kHz/20ms). AMR-WB 정합. */
    unsigned clockRate = 16000;
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

    std::string aor() const { return "sip:" + msisdn + "@" + domain; }
    std::string effectiveMcpttId() const { return mcpttId.empty() ? "tel:" + msisdn : mcpttId; }
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
};

/** 착신 INVITE 의 mcptt-info(TS 24.379 §F.1) 요약. */
struct McpttInfo {
    bool present = false;
    std::string sessionType;          // prearranged/chat/broadcast/private
    std::string requestUri, callingUserId, callingGroupId;
    bool emergency = false, imminentPeril = false;
    bool privateCall = false;
    bool noFloorCtrl = false;         // fmtp mc_no_floor_ctrl — 전이중 1:1
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
    int lastCode = 0;
    std::string lastReason;
    std::vector<MediaSource> sources;
    // ── MCPTT ──
    bool isMcptt = false;             // 그룹콜/사설콜 세션(floor 평면 있음 또는 mc_no_floor_ctrl)
    std::string groupId;              // 그룹 id(bare) 또는 사설콜 상대(bare)
    McpttInfo mcptt;
    bool halfDuplex = false;          // floor 로 마이크를 게이트한다(Granted 에서만 송신)
    bool listenOnly = false;          // a=recvonly 청취 leg (PTT 청취·감청 Join)
    std::string joinedDialog;         // INVITE-Join 으로 합류한 대상 dialog 의 Call-ID
};

// ── floor (TS 24.380 participant) ──
enum class FloorState { Idle, Requesting, Speaking, Listening, Queued };

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
    long token = 0;
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
    std::string groupUri;             // mcdata-info request-uri (그룹 SDS)
    std::string convId, msgId;        // UUID hex32
    long timeSec = 0;
    int dispositionReq = 0;           // 0 없음 / 1 delivery / 2 read / 3 both
    std::string text;
    bool notification = false;        // SDS NOTIFICATION
    int notifType = 0;                // 1 undelivered / 2 delivered / 3 read / 4 delivered+read
    bool fd = false;                  // FD SIGNALLING (파일 URL)
    std::string fileUrl, fileName, fileType;
    long fileSize = 0;
};

struct StreamStats {
    unsigned rxPackets = 0, rxBytes = 0, rxLoss = 0, rxDiscard = 0;
    unsigned txPackets = 0, txBytes = 0;
    bool valid = false;
};

struct AudioDeviceInfo {
    int id = -1;
    std::string name;
    std::string driver;
    unsigned inputCount = 0;
    unsigned outputCount = 0;
};

const char* toString(RegState s);
const char* toString(CallState s);
const char* toString(Transport t);
const char* toString(FloorState s);
const char* toString(FloorEvent::Kind k);

}  // namespace cimsue
