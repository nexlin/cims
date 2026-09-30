// 생성 파일 — 손으로 고치지 않는다. 정본: docs/design/features/mcvideo_tc_defs.yaml,
// 생성기: scripts/gen_mcvideo_tc_defs.py (--check 가 sdk·cmp 두 생성물의 최신성을 대조 — S1-UE-MCVIDEO-TC-DEFS).
// 3GPP TS 24.581 §9.2·§11·§12.1.2 — MCVideo 전송 제어 메시지·필드·원인·타이머·fmtp 정의(단말 코어).
#pragma once
#include <cstdint>
#include <cstring>

namespace cimsue {
namespace mcvideo {

constexpr uint8_t kRtcpPtApp = 204;
constexpr uint8_t kAckRequiredBit = 0x10;
constexpr uint8_t kSubtypeMask = 0x0F;

/** RTCP APP name(§9.1.2) — MCV0 참여자→서버 · MCV1 서버→참여자 · MCV2 양방향. */
enum class AppName : uint8_t { MCV0 = 0, MCV1 = 1, MCV2 = 2 };
constexpr char kNameMcv0[4] = {'M', 'C', 'V', '0'};
constexpr char kNameMcv1[4] = {'M', 'C', 'V', '1'};
constexpr char kNameMcv2[4] = {'M', 'C', 'V', '2'};
/** 헤더의 name 4옥텟 → AppName. 전송 제어(MCV0~2)가 아니면 false. */
inline bool appNameOf(const char* name4, AppName& out) {
    if (std::memcmp(name4, kNameMcv0, 4) == 0) { out = AppName::MCV0; return true; }
    if (std::memcmp(name4, kNameMcv1, 4) == 0) { out = AppName::MCV1; return true; }
    if (std::memcmp(name4, kNameMcv2, 4) == 0) { out = AppName::MCV2; return true; }
    return false;
}
inline const char* appNameText(AppName a) {
    switch (a) {
        case AppName::MCV0: return "MCV0";
        case AppName::MCV1: return "MCV1";
        case AppName::MCV2: return "MCV2";
    }
    return "";
}

/** name MCV0 의 subtype(메시지 타입, 첫 비트 = ack 요구). */
enum class Mcv0 : uint8_t {
    TRANSMISSION_REQUEST = 0x00,  // Transmission Request (§9.2.4)
    TRANSMISSION_RELEASE = 0x02,  // Transmission Release (§9.2.7)
    QUEUE_POSITION_REQUEST = 0x03,  // Queue Position Request (§9.2.11)
    RECEIVE_MEDIA_REQUEST = 0x04,  // Receive Media Request (§9.2.14)
    REMOTE_TRANSMISSION_REQUEST = 0x07,  // Remote Transmission Request (§9.2.22)
    REMOTE_TRANSMISSION_CANCEL_REQUEST = 0x08,  // Remote Transmission Cancel Request (§9.2.24)
};
/** name MCV1 의 subtype(메시지 타입, 첫 비트 = ack 요구). */
enum class Mcv1 : uint8_t {
    TRANSMISSION_GRANTED = 0x00,  // Transmission Granted (§9.2.5)
    TRANSMISSION_REJECTED = 0x01,  // Transmission Rejected (§9.2.6)
    TRANSMISSION_ARBITRATION_TAKEN = 0x02,  // Transmission Arbitration Taken (§9.2.8)
    TRANSMISSION_ARBITRATION_RELEASE = 0x03,  // Transmission Arbitration Release (§9.2.9)
    TRANSMISSION_REVOKED = 0x04,  // Transmission Revoked (§9.2.10)
    QUEUE_POSITION_INFO = 0x05,  // Queue Position Info (§9.2.12)
    MEDIA_TRANSMISSION_NOTIFICATION = 0x06,  // Media Transmission Notification (§9.2.13)
    RECEIVE_MEDIA_RESPONSE = 0x07,  // Receive Media Response (§9.2.15)
    MEDIA_RECEPTION_NOTIFICATION = 0x08,  // Media Reception Notification (§9.2.16)
    TRANSMISSION_CANCEL_REQUEST_NOTIFY = 0x0A,  // Transmission Cancel Request Notify (§9.2.19)
    REMOTE_TRANSMISSION_RESPONSE = 0x0B,  // Remote Transmission Response (§9.2.23)
    REMOTE_TRANSMISSION_CANCEL_RESPONSE = 0x0C,  // Remote Transmission Cancel Response (§9.2.25)
    MEDIA_RECEPTION_OVERRIDE_NOTIFICATION = 0x0D,  // Media Reception Override Notification (§9.2.28)
    TRANSMISSION_END_NOTIFY = 0x0E,  // Transmission End Notify (§9.2.29)
    TRANSMISSION_IDLE = 0x0F,  // Transmission Idle (§9.2.30)
};
/** name MCV2 의 subtype(메시지 타입, 첫 비트 = ack 요구). */
enum class Mcv2 : uint8_t {
    TRANSMISSION_END_REQUEST = 0x00,  // Transmission End Request (§9.2.20)
    TRANSMISSION_END_RESPONSE = 0x01,  // Transmission End Response (§9.2.21)
    MEDIA_RECEPTION_END_REQUEST = 0x02,  // Media Reception End Request (§9.2.26)
    MEDIA_RECEPTION_END_RESPONSE = 0x03,  // Media Reception End Response (§9.2.27)
    TRANSMISSION_CONTROL_ACK = 0x04,  // Transmission Control Ack (§9.2.31)
};
/** (name, subtype) → 메시지 식별자 이름(로그용). 모르면 "UNKNOWN". */
inline const char* msgName(AppName app, uint8_t subtype) {
    switch (app) {
        case AppName::MCV0:
            switch (subtype & kSubtypeMask) {
                case 0x00: return "TRANSMISSION_REQUEST";
                case 0x02: return "TRANSMISSION_RELEASE";
                case 0x03: return "QUEUE_POSITION_REQUEST";
                case 0x04: return "RECEIVE_MEDIA_REQUEST";
                case 0x07: return "REMOTE_TRANSMISSION_REQUEST";
                case 0x08: return "REMOTE_TRANSMISSION_CANCEL_REQUEST";
                default: return "UNKNOWN";
            }
        case AppName::MCV1:
            switch (subtype & kSubtypeMask) {
                case 0x00: return "TRANSMISSION_GRANTED";
                case 0x01: return "TRANSMISSION_REJECTED";
                case 0x02: return "TRANSMISSION_ARBITRATION_TAKEN";
                case 0x03: return "TRANSMISSION_ARBITRATION_RELEASE";
                case 0x04: return "TRANSMISSION_REVOKED";
                case 0x05: return "QUEUE_POSITION_INFO";
                case 0x06: return "MEDIA_TRANSMISSION_NOTIFICATION";
                case 0x07: return "RECEIVE_MEDIA_RESPONSE";
                case 0x08: return "MEDIA_RECEPTION_NOTIFICATION";
                case 0x0A: return "TRANSMISSION_CANCEL_REQUEST_NOTIFY";
                case 0x0B: return "REMOTE_TRANSMISSION_RESPONSE";
                case 0x0C: return "REMOTE_TRANSMISSION_CANCEL_RESPONSE";
                case 0x0D: return "MEDIA_RECEPTION_OVERRIDE_NOTIFICATION";
                case 0x0E: return "TRANSMISSION_END_NOTIFY";
                case 0x0F: return "TRANSMISSION_IDLE";
                default: return "UNKNOWN";
            }
        case AppName::MCV2:
            switch (subtype & kSubtypeMask) {
                case 0x00: return "TRANSMISSION_END_REQUEST";
                case 0x01: return "TRANSMISSION_END_RESPONSE";
                case 0x02: return "MEDIA_RECEPTION_END_REQUEST";
                case 0x03: return "MEDIA_RECEPTION_END_RESPONSE";
                case 0x04: return "TRANSMISSION_CONTROL_ACK";
                default: return "UNKNOWN";
            }
    }
    return "UNKNOWN";
}
/** 이 판본이 정한 메시지인가 — 모르는 subtype 이면 메시지 전체를 버린다(§9.1.4 1). */
inline bool knownMessage(AppName app, uint8_t subtype) {
    switch (app) {
        case AppName::MCV0:
            switch (subtype & kSubtypeMask) {
                case 0x00: case 0x02: case 0x03: case 0x04: case 0x07: case 0x08: return true;
                default: return false;
            }
        case AppName::MCV1:
            switch (subtype & kSubtypeMask) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: return true;
                default: return false;
            }
        case AppName::MCV2:
            switch (subtype & kSubtypeMask) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: return true;
                default: return false;
            }
    }
    return false;
}
/** 메시지가 실을 수 있는 필드 집합 — 비트 i = field ID i(§9.2.4~§9.2.31 의 표). 모르는 메시지 = 0. */
inline uint32_t allowedFields(AppName app, uint8_t subtype) {
    switch (app) {
        case AppName::MCV0:
            switch (subtype & kSubtypeMask) {
                case 0x00: return 0x00202841u;  // TRANSMISSION_REQUEST
                case 0x02: return 0x00002840u;  // TRANSMISSION_RELEASE
                case 0x03: return 0x00000840u;  // QUEUE_POSITION_REQUEST
                case 0x04: return 0x00A86850u;  // RECEIVE_MEDIA_REQUEST
                case 0x07: return 0x00000840u;  // REMOTE_TRANSMISSION_REQUEST
                case 0x08: return 0x00000840u;  // REMOTE_TRANSMISSION_CANCEL_REQUEST
                default: return 0;
            }
        case AppName::MCV1:
            switch (subtype & kSubtypeMask) {
                case 0x00: return 0x00806ACBu;  // TRANSMISSION_GRANTED
                case 0x01: return 0x00002844u;  // TRANSMISSION_REJECTED
                case 0x02: return 0x00806170u;  // TRANSMISSION_ARBITRATION_TAKEN
                case 0x03: return 0x00002140u;  // TRANSMISSION_ARBITRATION_RELEASE
                case 0x04: return 0x00002804u;  // TRANSMISSION_REVOKED
                case 0x05: return 0x00002A48u;  // QUEUE_POSITION_INFO
                case 0x06: return 0x00E06830u;  // MEDIA_TRANSMISSION_NOTIFICATION
                case 0x07: return 0x0080E814u;  // RECEIVE_MEDIA_RESPONSE
                case 0x08: return 0x00A04840u;  // MEDIA_RECEPTION_NOTIFICATION
                case 0x0A: return 0x00000800u;  // TRANSMISSION_CANCEL_REQUEST_NOTIFY
                case 0x0B: return 0x00000800u;  // REMOTE_TRANSMISSION_RESPONSE
                case 0x0C: return 0x00000800u;  // REMOTE_TRANSMISSION_CANCEL_RESPONSE
                case 0x0D: return 0x00060040u;  // MEDIA_RECEPTION_OVERRIDE_NOTIFICATION
                case 0x0E: return 0x00804010u;  // TRANSMISSION_END_NOTIFY
                case 0x0F: return 0x00002100u;  // TRANSMISSION_IDLE
                default: return 0;
            }
        case AppName::MCV2:
            switch (subtype & kSubtypeMask) {
                case 0x00: return 0x00806814u;  // TRANSMISSION_END_REQUEST
                case 0x01: return 0x00806810u;  // TRANSMISSION_END_RESPONSE
                case 0x02: return 0x00806810u;  // MEDIA_RECEPTION_END_REQUEST
                case 0x03: return 0x00806810u;  // MEDIA_RECEPTION_END_RESPONSE
                case 0x04: return 0x00011C00u;  // TRANSMISSION_CONTROL_ACK
                default: return 0;
            }
    }
    return 0;
}

/** Table 9.2.3.1-1 — 전송 제어 필드 ID. */
enum class Field : uint8_t {
    TRANSMISSION_PRIORITY = 0,  // §9.2.3.2
    DURATION = 1,  // §9.2.3.3
    REJECT_CAUSE = 2,  // §9.2.3.4
    QUEUE_INFO = 3,  // §9.2.3.5
    TRANSMITTING_USER_ID = 4,  // §9.2.3.6
    PERMISSION = 5,  // §9.2.3.7
    USER_ID = 6,  // §9.2.3.8
    QUEUE_SIZE = 7,  // §9.2.3.15
    MSG_SEQ = 8,  // §9.2.3.9
    QUEUED_USER_ID = 9,  // §9.2.3.14
    SOURCE = 10,  // §9.2.3.12
    TRACK_INFO = 11,  // §9.2.3.13
    MSG_TYPE = 12,  // §9.2.3.10
    TRANSMISSION_INDICATOR = 13,  // §9.2.3.11
    AUDIO_SSRC = 14,  // §9.2.3.16
    RESULT = 15,  // §9.2.3.17
    MESSAGE_NAME = 16,  // §9.2.3.18
    OVERRIDING_ID = 17,  // §9.2.3.8
    OVERRIDDEN_ID = 18,  // §9.2.3.8
    RECEPTION_PRIORITY = 19,  // §9.2.3.19
    GROUP_ID = 20,  // §9.2.3.20
    FUNCTIONAL_ALIAS = 21,  // §9.2.3.21
    RECEPTION_MODE = 22,  // §9.2.3.22
    VIDEO_SSRC = 23,  // §9.2.3.23
};
/** 필드 값의 모양 — 정본 테이블 kind(u8·u16·pair = Length 2, ssrc·name = Length 6, uri·cause·track = 가변·4옥텟 경계 패딩). */
enum class FieldKind : uint8_t { U8, U16, Pair, Ssrc, Name, Uri, Cause, Track, Unknown };
inline FieldKind fieldKind(uint8_t id) {
    switch (id) {
        case 0: return FieldKind::U8;
        case 1: return FieldKind::U16;
        case 2: return FieldKind::Cause;
        case 3: return FieldKind::Pair;
        case 4: return FieldKind::Uri;
        case 5: return FieldKind::U16;
        case 6: return FieldKind::Uri;
        case 7: return FieldKind::U16;
        case 8: return FieldKind::U16;
        case 9: return FieldKind::Uri;
        case 10: return FieldKind::U16;
        case 11: return FieldKind::Track;
        case 12: return FieldKind::U8;
        case 13: return FieldKind::U16;
        case 14: return FieldKind::Ssrc;
        case 15: return FieldKind::U16;
        case 16: return FieldKind::Name;
        case 17: return FieldKind::Uri;
        case 18: return FieldKind::Uri;
        case 19: return FieldKind::U8;
        case 20: return FieldKind::Uri;
        case 21: return FieldKind::Uri;
        case 22: return FieldKind::U16;
        case 23: return FieldKind::Ssrc;
        default: return FieldKind::Unknown;
    }
}
/** 고정 길이 kind 의 Length 값, 가변(uri·cause·track)·미지는 -1. */
inline int fixedLength(FieldKind k) {
    switch (k) {
        case FieldKind::U8: return 2;
        case FieldKind::U16: return 2;
        case FieldKind::Pair: return 2;
        case FieldKind::Ssrc: return 6;
        case FieldKind::Name: return 6;
        default: return -1;
    }
}

/** §9.2.3.11 Transmission Indicator 비트. */
namespace indicator {
constexpr uint16_t NORMAL = 0x8000;
constexpr uint16_t BROADCAST_GROUP = 0x4000;
constexpr uint16_t SYSTEM = 0x2000;
constexpr uint16_t EMERGENCY = 0x1000;
constexpr uint16_t IMMINENT_PERIL = 0x0800;
}  // namespace indicator

enum class Source : uint16_t {
    PARTICIPANT = 0,
    PARTICIPATING = 1,
    CONTROLLING = 2,
    NON_CONTROLLING = 3,
};
enum class Permission : uint16_t {
    DENIED = 0,
    ALLOWED = 1,
};
enum class ReceiveResult : uint16_t {
    REJECTED = 0,
    GRANTED = 1,
};
enum class ReceptionMode : uint16_t {
    AUTOMATIC = 0,
    MANUAL = 1,
};
/** §9.2.3.5 Queue Position Info 특수값 · §9.2.3.13 Queueing Capability · 기본 우선순위(§9.2.3.2·§9.2.3.19). */
namespace queue {
constexpr uint8_t NOT_QUEUED = 254;
constexpr uint8_t POSITION_UNKNOWN = 255;
constexpr uint8_t CAPABILITY_NONE = 0;
constexpr uint8_t CAPABILITY_QUEUEING = 1;
constexpr uint8_t DEFAULT_PRIORITY = 0;
}  // namespace queue

enum class RejectCause : uint16_t {
    TRANSMISSION_LIMIT = 1,
    INTERNAL_ERROR = 2,
    ONLY_ONE_PARTICIPANT = 3,
    RETRY_AFTER = 4,
    RECEIVE_ONLY = 5,
    NO_RESOURCES = 6,
    OTHER = 255,
};
inline const char* rejectCauseText(int v) {
    switch (v) {
        case 1: return "Transmission limit reached";
        case 2: return "Internal transmission control server error";
        case 3: return "Only one participant";
        case 4: return "Retry-after timer has not expired";
        case 5: return "Receive only";
        case 6: return "No resources available";
        case 255: return "Other reason";
        default: return nullptr;
    }
}
enum class RevokeCause : uint16_t {
    ONLY_ONE_CLIENT = 1,
    MEDIA_BURST_TOO_LONG = 2,
    NO_PERMISSION = 3,
    PREEMPTED = 4,
    TERMINATE_STREAM = 5,
    NO_RESOURCES = 6,
    QUEUE_TRANSMISSION = 7,
    NO_RECEIVER = 8,
    OTHER = 255,
};
inline const char* revokeCauseText(int v) {
    switch (v) {
        case 1: return "Only one MCVideo client";
        case 2: return "Media burst too long";
        case 3: return "No permission to send a Media Burst";
        case 4: return "Media Burst pre-empted";
        case 5: return "Terminate the RTP stream";
        case 6: return "No resources available";
        case 7: return "Queue the transmission";
        case 8: return "No receiving participant";
        case 255: return "Other reason";
        default: return nullptr;
    }
}
enum class ReceiveRejectCause : uint16_t {
    INTERNAL_ERROR = 2,
    RETRY_AFTER = 4,
    SEND_ONLY = 5,
    NO_RESOURCES = 6,
    MAX_STREAMS = 7,
    OTHER = 255,
};
inline const char* receiveRejectCauseText(int v) {
    switch (v) {
        case 2: return "Internal transmission control server error";
        case 4: return "Retry-after timer has not expired";
        case 5: return "Send only";
        case 6: return "No resources available";
        case 7: return "Max no of simultaneous stream to receive is reached";
        case 255: return "Other reason";
        default: return nullptr;
    }
}

/** §11 타이머(ms)·카운터 기본값 — 값의 정본은 service configuration `<tc-timers-counters-R14>`·그룹 문서, 없을 때 이 값. */
namespace timer {
constexpr int T100_MS = 1000;  // Transmission Request (cims, <T100-transmission-request>)
constexpr int T101_MS = 1000;  // Transmission End Request (cims, <T101-transmission-end-request>)
constexpr int T102_MS = 1000;  // Transmission Queue Position Request (cims, <T102-queue-position-request>)
constexpr int T103_MS = 1000;  // Receive Media Request (cims, <T103-receive-media-request>)
constexpr int T104_MS = 1000;  // Receive Media Release (cims, <T104-receive-media-release>)
constexpr int T1_MS = 30000;  // Inactivity (spec, <on-network-hang-timer>)
constexpr int T2_MS = 1000;  // Transmission Idle (cims, <T2-transmission-idle>)
constexpr int T3_MS = 1000;  // Transmission Revoke (spec, <T3-transmission-revoke>)
constexpr int T4_MS = 1000;  // Transmission Granted (spec, <T4-transmission-granted>)
constexpr int T5_MS = 30000;  // Reception Inactivity (spec, <on-network-reception-hang-timer>)
constexpr int T6_MS = 1000;  // Reception Granted (spec, <T6-reception-granted>)
constexpr int T11_MS = 10000;  // Stream Reception Idle (spec, <T11-stream-reception-idle>)
constexpr int C100 = 3;  // Transmission Request
constexpr int C101 = 3;  // Transmission End Request
constexpr int C102 = 3;  // Transmission Queue Position Request
constexpr int C103 = 3;  // Receive Media Request
constexpr int C104 = 3;  // Receive Media Release
constexpr int C2 = 10;  // Transmission Idle
constexpr int C4 = 3;  // Transmission Granted
constexpr int C6 = 3;  // Reception Granted
constexpr int C7 = 2;  // Reception Accepted
constexpr int C9 = 4;  // Per-Participant Reception Accepted
constexpr int C11 = 4;  // Count of active receivers for the stream
}  // namespace timer

/** §4.3.3.1·§12.1.2 — 제어 채널 `m=application <RTCP 포트> udp MCVideo` 와 `a=fmtp:MCVideo` 파라미터. */
constexpr const char* kSdpProto = "udp";
constexpr const char* kSdpFmt = "MCVideo";
constexpr const char* kFmtpSeparator = ";";
namespace fmtp {
constexpr const char* QUEUEING = "mc_queueing";
constexpr const char* PRIORITY = "mc_priority";
constexpr const char* RECEPTION_PRIORITY = "mc_reception_priority";
constexpr const char* GRANTED = "mc_granted";
constexpr const char* IMPLICIT_REQUEST = "mc_implicit_request";
constexpr const char* AUDIO_SSRC = "mc_audio_ssrc";
constexpr const char* VIDEO_SSRC = "mc_video_ssrc";
constexpr const char* TRANSMISSION_SSRC = "mc_transmission_ssrc";
}  // namespace fmtp

}  // namespace mcvideo
}  // namespace cimsue
