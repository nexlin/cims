// 생성 파일 — 손으로 고치지 않는다. 정본: docs/design/features/mcptt_floor_defs.yaml,
// 생성기: scripts/gen_floor_defs.py (--check 가 cmp/android/probe 상수와 대조).
// 3GPP TS 24.380 §8 — MCPTT floor control 메시지 정의.
#pragma once
#include <cstdint>

namespace cimsue {
namespace floor {

constexpr uint8_t kRtcpPtApp = 204;
constexpr char kRtcpName[4] = {'M', 'C', 'P', 'T'};
constexpr uint8_t kAckRequiredBit = 0x10;

/** Table 8.2.2-1 — RTCP APP subtype(메시지 타입). */
enum class Op : uint8_t {
    REQUEST = 0x00,  // Floor Request (ue2srv)
    GRANTED = 0x01,  // Floor Granted (srv2ue)
    TAKEN = 0x02,  // Floor Taken (srv2ue)
    DENY = 0x03,  // Floor Deny (srv2ue)
    RELEASE = 0x04,  // Floor Release (ue2srv)
    IDLE = 0x05,  // Floor Idle (srv2ue)
    REVOKE = 0x06,  // Floor Revoke (srv2ue)
    QUEUE_POS_REQ = 0x08,  // Floor Queue Position Request (ue2srv)
    QUEUE_POS_INFO = 0x09,  // Floor Queue Position Info (srv2ue)
    ACK = 0x0A,  // Floor Ack (both)
    MEDIA_FLOW = 0x0B,  // Unicast Media Flow Control (ue2srv)
    QUEUED_CANCEL = 0x0E,  // Queued Floor Requests (both)
    RELEASE_MULTI = 0x0F,  // Floor Release Multi Talker (srv2ue)
};
inline const char* opName(uint8_t op) {
    switch (op & 0x0F) {
        case 0x00: return "REQUEST";
        case 0x01: return "GRANTED";
        case 0x02: return "TAKEN";
        case 0x03: return "DENY";
        case 0x04: return "RELEASE";
        case 0x05: return "IDLE";
        case 0x06: return "REVOKE";
        case 0x08: return "QUEUE_POS_REQ";
        case 0x09: return "QUEUE_POS_INFO";
        case 0x0A: return "ACK";
        case 0x0B: return "MEDIA_FLOW";
        case 0x0E: return "QUEUED_CANCEL";
        case 0x0F: return "RELEASE_MULTI";
        default: return "UNKNOWN";
    }
}

/** §8.2.3 — floor control specific field ID. */
enum class Field : uint8_t {
    PRIORITY = 0,
    DURATION = 1,
    REJECT_CAUSE = 2,
    QUEUE_INFO = 3,
    GRANTED_PARTY = 4,
    PERMISSION = 5,
    USER_ID = 6,
    QUEUE_SIZE = 7,
    MSG_SEQ = 8,
    QUEUED_USER_ID = 9,
    SOURCE = 10,
    TRACK_INFO = 11,
    MSG_TYPE = 12,
    FLOOR_INDICATOR = 13,
    SSRC = 14,
    GRANTED_USERS = 15,
    SSRC_LIST = 16,
    QUEUED_PURPOSE = 21,
    QUEUED_USERS = 22,
    QUEUED_RESULT = 23,
    MEDIA_FLOW = 24,
};
/** 가변 길이(문자열) 필드 — 4옥텟 정렬 패딩 대상. */
inline bool isStringField(uint8_t id) {
    switch (id) {
        case 4:
        case 6:
        case 9:
        case 11:
            return true;
        default: return false;
    }
}

/** §8.2.3.13 Floor Indicator 비트. */
namespace indicator {
constexpr uint16_t NORMAL = 0x8000;
constexpr uint16_t BROADCAST_GROUP = 0x4000;
constexpr uint16_t SYSTEM = 0x2000;
constexpr uint16_t EMERGENCY = 0x1000;
constexpr uint16_t IMMINENT_PERIL = 0x0800;
constexpr uint16_t QUEUEING = 0x0400;
constexpr uint16_t DUAL_FLOOR = 0x0200;
constexpr uint16_t TEMPORARY_GROUP = 0x0100;
constexpr uint16_t MULTI_TALKER = 0x0080;
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

enum class QueuedPurpose : uint16_t {
    CANCEL_REQUEST = 0,
    CANCEL_RESULT = 1,
    CANCEL_NOTIFY = 2,
};

inline const char* rejectCauseText(int v) {
    switch (v) {
        case 1: return "Another MCPTT client has permission";
        case 2: return "Internal floor control server error";
        case 3: return "Only one participant";
        case 4: return "Retry-after timer has not expired";
        case 5: return "Receive only";
        case 6: return "No resources available";
        case 7: return "Queue full";
        case 255: return "Other reason";
        default: return nullptr;
    }
}

inline const char* revokeCauseText(int v) {
    switch (v) {
        case 1: return "Only one MCPTT client";
        case 2: return "Media burst too long";
        case 3: return "No permission to send a Media Burst";
        case 4: return "Media Burst pre-empted";
        case 6: return "No resources available";
        case 255: return "Other reason";
        default: return nullptr;
    }
}

inline const char* queuedResultText(int v) {
    switch (v) {
        case 0: return "Cancelled";
        case 2: return "Queue empty";
        case 3: return "No queued request";
        case 5: return "Partially cancelled";
        default: return nullptr;
    }
}

constexpr uint8_t kMediaFlowResumeBit = 0x80;

}  // namespace floor
}  // namespace cimsue
