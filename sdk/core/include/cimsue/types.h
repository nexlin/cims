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

    std::string aor() const { return "sip:" + msisdn + "@" + domain; }
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

}  // namespace cimsue
