// 생성 파일 — 손으로 고치지 않는다. 정본: docs/design/features/mcvideo_tc_defs.yaml,
// 생성기: scripts/gen_mcvideo_tc_defs.py (--check 가 sdk·cmp 두 생성물의 최신성을 대조 — S1-UE-MCVIDEO-TC-DEFS).
// 3GPP TS 24.581 §9.2·§11 — MCVideo 전송 제어 메시지·필드·원인·타이머 정의(CMP 서버 이름).
#ifndef __TRANSMISSION_DEFS_H__
#define __TRANSMISSION_DEFS_H__

#define MCV_RTCP_PT_APP 204
#define MCV_ACK_REQ_BIT 0x10
#define MCV_SUBTYPE(subtype) ((subtype) & 0x0F)
#define MCV_NAME_0 "MCV0"
#define MCV_NAME_1 "MCV1"
#define MCV_NAME_2 "MCV2"

// RTCP APP name (§9.1.2) — MCV0 참여자→서버 · MCV1 서버→참여자 · MCV2 양방향.
enum McvAppName { MCV_APP_0 = 0, MCV_APP_1 = 1, MCV_APP_2 = 2 };

// name MCV0 subtype
enum Mcv0Subtype {
    MCV0_TRANSMISSION_REQUEST = 0x0,  // Transmission Request (§9.2.4)
    MCV0_TRANSMISSION_RELEASE = 0x2,  // Transmission Release (§9.2.7)
    MCV0_QUEUE_POSITION_REQUEST = 0x3,  // Queue Position Request (§9.2.11)
    MCV0_RECEIVE_MEDIA_REQUEST = 0x4,  // Receive Media Request (§9.2.14)
    MCV0_REMOTE_TRANSMISSION_REQUEST = 0x7,  // Remote Transmission Request (§9.2.22)
    MCV0_REMOTE_TRANSMISSION_CANCEL_REQUEST = 0x8  // Remote Transmission Cancel Request (§9.2.24)
};
// name MCV1 subtype
enum Mcv1Subtype {
    MCV1_TRANSMISSION_GRANTED = 0x0,  // Transmission Granted (§9.2.5)
    MCV1_TRANSMISSION_REJECTED = 0x1,  // Transmission Rejected (§9.2.6)
    MCV1_TRANSMISSION_ARBITRATION_TAKEN = 0x2,  // Transmission Arbitration Taken (§9.2.8)
    MCV1_TRANSMISSION_ARBITRATION_RELEASE = 0x3,  // Transmission Arbitration Release (§9.2.9)
    MCV1_TRANSMISSION_REVOKED = 0x4,  // Transmission Revoked (§9.2.10)
    MCV1_QUEUE_POSITION_INFO = 0x5,  // Queue Position Info (§9.2.12)
    MCV1_MEDIA_TRANSMISSION_NOTIFICATION = 0x6,  // Media Transmission Notification (§9.2.13)
    MCV1_RECEIVE_MEDIA_RESPONSE = 0x7,  // Receive Media Response (§9.2.15)
    MCV1_MEDIA_RECEPTION_NOTIFICATION = 0x8,  // Media Reception Notification (§9.2.16)
    MCV1_TRANSMISSION_CANCEL_REQUEST_NOTIFY = 0xA,  // Transmission Cancel Request Notify (§9.2.19)
    MCV1_REMOTE_TRANSMISSION_RESPONSE = 0xB,  // Remote Transmission Response (§9.2.23)
    MCV1_REMOTE_TRANSMISSION_CANCEL_RESPONSE = 0xC,  // Remote Transmission Cancel Response (§9.2.25)
    MCV1_MEDIA_RECEPTION_OVERRIDE_NOTIFICATION = 0xD,  // Media Reception Override Notification (§9.2.28)
    MCV1_TRANSMISSION_END_NOTIFY = 0xE,  // Transmission End Notify (§9.2.29)
    MCV1_TRANSMISSION_IDLE = 0xF  // Transmission Idle (§9.2.30)
};
// name MCV2 subtype
enum Mcv2Subtype {
    MCV2_TRANSMISSION_END_REQUEST = 0x0,  // Transmission End Request (§9.2.20)
    MCV2_TRANSMISSION_END_RESPONSE = 0x1,  // Transmission End Response (§9.2.21)
    MCV2_MEDIA_RECEPTION_END_REQUEST = 0x2,  // Media Reception End Request (§9.2.26)
    MCV2_MEDIA_RECEPTION_END_RESPONSE = 0x3,  // Media Reception End Response (§9.2.27)
    MCV2_TRANSMISSION_CONTROL_ACK = 0x4  // Transmission Control Ack (§9.2.31)
};
// (app, subtype) → 메시지 식별자 이름(로그용).
inline const char* McvMessageName(int app, int subtype) {
    switch (app) {
    case MCV_APP_0:
        switch (MCV_SUBTYPE(subtype)) {
        case MCV0_TRANSMISSION_REQUEST: return "TRANSMISSION_REQUEST";
        case MCV0_TRANSMISSION_RELEASE: return "TRANSMISSION_RELEASE";
        case MCV0_QUEUE_POSITION_REQUEST: return "QUEUE_POSITION_REQUEST";
        case MCV0_RECEIVE_MEDIA_REQUEST: return "RECEIVE_MEDIA_REQUEST";
        case MCV0_REMOTE_TRANSMISSION_REQUEST: return "REMOTE_TRANSMISSION_REQUEST";
        case MCV0_REMOTE_TRANSMISSION_CANCEL_REQUEST: return "REMOTE_TRANSMISSION_CANCEL_REQUEST";
        default: return "UNKNOWN";
        }
    case MCV_APP_1:
        switch (MCV_SUBTYPE(subtype)) {
        case MCV1_TRANSMISSION_GRANTED: return "TRANSMISSION_GRANTED";
        case MCV1_TRANSMISSION_REJECTED: return "TRANSMISSION_REJECTED";
        case MCV1_TRANSMISSION_ARBITRATION_TAKEN: return "TRANSMISSION_ARBITRATION_TAKEN";
        case MCV1_TRANSMISSION_ARBITRATION_RELEASE: return "TRANSMISSION_ARBITRATION_RELEASE";
        case MCV1_TRANSMISSION_REVOKED: return "TRANSMISSION_REVOKED";
        case MCV1_QUEUE_POSITION_INFO: return "QUEUE_POSITION_INFO";
        case MCV1_MEDIA_TRANSMISSION_NOTIFICATION: return "MEDIA_TRANSMISSION_NOTIFICATION";
        case MCV1_RECEIVE_MEDIA_RESPONSE: return "RECEIVE_MEDIA_RESPONSE";
        case MCV1_MEDIA_RECEPTION_NOTIFICATION: return "MEDIA_RECEPTION_NOTIFICATION";
        case MCV1_TRANSMISSION_CANCEL_REQUEST_NOTIFY: return "TRANSMISSION_CANCEL_REQUEST_NOTIFY";
        case MCV1_REMOTE_TRANSMISSION_RESPONSE: return "REMOTE_TRANSMISSION_RESPONSE";
        case MCV1_REMOTE_TRANSMISSION_CANCEL_RESPONSE: return "REMOTE_TRANSMISSION_CANCEL_RESPONSE";
        case MCV1_MEDIA_RECEPTION_OVERRIDE_NOTIFICATION: return "MEDIA_RECEPTION_OVERRIDE_NOTIFICATION";
        case MCV1_TRANSMISSION_END_NOTIFY: return "TRANSMISSION_END_NOTIFY";
        case MCV1_TRANSMISSION_IDLE: return "TRANSMISSION_IDLE";
        default: return "UNKNOWN";
        }
    case MCV_APP_2:
        switch (MCV_SUBTYPE(subtype)) {
        case MCV2_TRANSMISSION_END_REQUEST: return "TRANSMISSION_END_REQUEST";
        case MCV2_TRANSMISSION_END_RESPONSE: return "TRANSMISSION_END_RESPONSE";
        case MCV2_MEDIA_RECEPTION_END_REQUEST: return "MEDIA_RECEPTION_END_REQUEST";
        case MCV2_MEDIA_RECEPTION_END_RESPONSE: return "MEDIA_RECEPTION_END_RESPONSE";
        case MCV2_TRANSMISSION_CONTROL_ACK: return "TRANSMISSION_CONTROL_ACK";
        default: return "UNKNOWN";
        }
    }
    return "UNKNOWN";
}
// 메시지가 실을 수 있는 필드 집합 — 비트 i = field ID i. 모르는 메시지 = 0.
inline unsigned McvAllowedFields(int app, int subtype) {
    switch (app) {
    case MCV_APP_0:
        switch (MCV_SUBTYPE(subtype)) {
        case MCV0_TRANSMISSION_REQUEST: return 0x00202841u;
        case MCV0_TRANSMISSION_RELEASE: return 0x00002840u;
        case MCV0_QUEUE_POSITION_REQUEST: return 0x00000840u;
        case MCV0_RECEIVE_MEDIA_REQUEST: return 0x00A86850u;
        case MCV0_REMOTE_TRANSMISSION_REQUEST: return 0x00000840u;
        case MCV0_REMOTE_TRANSMISSION_CANCEL_REQUEST: return 0x00000840u;
        default: return 0;
        }
    case MCV_APP_1:
        switch (MCV_SUBTYPE(subtype)) {
        case MCV1_TRANSMISSION_GRANTED: return 0x00806ACBu;
        case MCV1_TRANSMISSION_REJECTED: return 0x00002844u;
        case MCV1_TRANSMISSION_ARBITRATION_TAKEN: return 0x00806170u;
        case MCV1_TRANSMISSION_ARBITRATION_RELEASE: return 0x00002140u;
        case MCV1_TRANSMISSION_REVOKED: return 0x00002804u;
        case MCV1_QUEUE_POSITION_INFO: return 0x00002A48u;
        case MCV1_MEDIA_TRANSMISSION_NOTIFICATION: return 0x00E06830u;
        case MCV1_RECEIVE_MEDIA_RESPONSE: return 0x0080E814u;
        case MCV1_MEDIA_RECEPTION_NOTIFICATION: return 0x00A04840u;
        case MCV1_TRANSMISSION_CANCEL_REQUEST_NOTIFY: return 0x00000800u;
        case MCV1_REMOTE_TRANSMISSION_RESPONSE: return 0x00000800u;
        case MCV1_REMOTE_TRANSMISSION_CANCEL_RESPONSE: return 0x00000800u;
        case MCV1_MEDIA_RECEPTION_OVERRIDE_NOTIFICATION: return 0x00060040u;
        case MCV1_TRANSMISSION_END_NOTIFY: return 0x00804010u;
        case MCV1_TRANSMISSION_IDLE: return 0x00002100u;
        default: return 0;
        }
    case MCV_APP_2:
        switch (MCV_SUBTYPE(subtype)) {
        case MCV2_TRANSMISSION_END_REQUEST: return 0x00804814u;
        case MCV2_TRANSMISSION_END_RESPONSE: return 0x00804810u;
        case MCV2_MEDIA_RECEPTION_END_REQUEST: return 0x00806810u;
        case MCV2_MEDIA_RECEPTION_END_RESPONSE: return 0x00804810u;
        case MCV2_TRANSMISSION_CONTROL_ACK: return 0x00011C00u;
        default: return 0;
        }
    }
    return 0;
}

// Table 9.2.3.1-1 — 전송 제어 필드 ID.
enum McvField {
    TF_TRANSMISSION_PRIORITY = 0,  // §9.2.3.2
    TF_DURATION = 1,  // §9.2.3.3
    TF_REJECT_CAUSE = 2,  // §9.2.3.4
    TF_QUEUE_INFO = 3,  // §9.2.3.5
    TF_TRANSMITTING_USER_ID = 4,  // §9.2.3.6
    TF_PERMISSION = 5,  // §9.2.3.7
    TF_USER_ID = 6,  // §9.2.3.8
    TF_QUEUE_SIZE = 7,  // §9.2.3.15
    TF_MSG_SEQ = 8,  // §9.2.3.9
    TF_QUEUED_USER_ID = 9,  // §9.2.3.14
    TF_SOURCE = 10,  // §9.2.3.12
    TF_TRACK_INFO = 11,  // §9.2.3.13
    TF_MSG_TYPE = 12,  // §9.2.3.10
    TF_TRANSMISSION_INDICATOR = 13,  // §9.2.3.11
    TF_AUDIO_SSRC = 14,  // §9.2.3.16
    TF_RESULT = 15,  // §9.2.3.17
    TF_MESSAGE_NAME = 16,  // §9.2.3.18
    TF_OVERRIDING_ID = 17,  // §9.2.3.8
    TF_OVERRIDDEN_ID = 18,  // §9.2.3.8
    TF_RECEPTION_PRIORITY = 19,  // §9.2.3.19
    TF_GROUP_ID = 20,  // §9.2.3.20
    TF_FUNCTIONAL_ALIAS = 21,  // §9.2.3.21
    TF_RECEPTION_MODE = 22,  // §9.2.3.22
    TF_VIDEO_SSRC = 23  // §9.2.3.23
};
// 필드 값의 모양 — u8·u16·pair = Length 2, ssrc·name = Length 6, uri·cause·track = 가변(4옥텟 경계 패딩).
enum McvFieldKind { TFK_U8, TFK_U16, TFK_PAIR, TFK_SSRC, TFK_NAME, TFK_URI, TFK_CAUSE, TFK_TRACK, TFK_UNKNOWN };
inline McvFieldKind McvFieldKindOf(int id) {
    switch (id) {
    case TF_TRANSMISSION_PRIORITY: return TFK_U8;
    case TF_DURATION: return TFK_U16;
    case TF_REJECT_CAUSE: return TFK_CAUSE;
    case TF_QUEUE_INFO: return TFK_PAIR;
    case TF_TRANSMITTING_USER_ID: return TFK_URI;
    case TF_PERMISSION: return TFK_U16;
    case TF_USER_ID: return TFK_URI;
    case TF_QUEUE_SIZE: return TFK_U16;
    case TF_MSG_SEQ: return TFK_U16;
    case TF_QUEUED_USER_ID: return TFK_URI;
    case TF_SOURCE: return TFK_U16;
    case TF_TRACK_INFO: return TFK_TRACK;
    case TF_MSG_TYPE: return TFK_U8;
    case TF_TRANSMISSION_INDICATOR: return TFK_U16;
    case TF_AUDIO_SSRC: return TFK_SSRC;
    case TF_RESULT: return TFK_U16;
    case TF_MESSAGE_NAME: return TFK_NAME;
    case TF_OVERRIDING_ID: return TFK_URI;
    case TF_OVERRIDDEN_ID: return TFK_URI;
    case TF_RECEPTION_PRIORITY: return TFK_U8;
    case TF_GROUP_ID: return TFK_URI;
    case TF_FUNCTIONAL_ALIAS: return TFK_URI;
    case TF_RECEPTION_MODE: return TFK_U16;
    case TF_VIDEO_SSRC: return TFK_SSRC;
    default: return TFK_UNKNOWN;
    }
}
// 고정 길이 kind 의 Length 값, 가변·미지는 -1.
inline int McvFixedLength(McvFieldKind k) {
    switch (k) {
    case TFK_U8: return 2;
    case TFK_U16: return 2;
    case TFK_PAIR: return 2;
    case TFK_SSRC: return 6;
    case TFK_NAME: return 6;
    default: return -1;
    }
}

// §9.2.3.11 Transmission Indicator 비트.
enum McvIndicatorBits {
    TI_NORMAL = 0x8000,
    TI_BROADCAST_GROUP = 0x4000,
    TI_SYSTEM = 0x2000,
    TI_EMERGENCY = 0x1000,
    TI_IMMINENT_PERIL = 0x0800
};
enum McvSource { TC_SRC_PARTICIPANT = 0, TC_SRC_PARTICIPATING = 1, TC_SRC_CONTROLLING = 2, TC_SRC_NON_CONTROLLING = 3 };
enum McvPermission { TC_PERM_DENIED = 0, TC_PERM_ALLOWED = 1 };
enum McvReceiveResult { TC_RESULT_REJECTED = 0, TC_RESULT_GRANTED = 1 };
enum McvReceptionMode { TC_RECEPTION_AUTOMATIC = 0, TC_RECEPTION_MANUAL = 1 };
#define TC_QUEUE_NOT_QUEUED 254
#define TC_QUEUE_POSITION_UNKNOWN 255
#define TC_QUEUE_CAPABILITY_NONE 0
#define TC_QUEUE_CAPABILITY_QUEUEING 1
#define TC_QUEUE_DEFAULT_PRIORITY 0

enum McvRejectCause {
    TC_REJECT_TRANSMISSION_LIMIT = 1,
    TC_REJECT_INTERNAL_ERROR = 2,
    TC_REJECT_ONLY_ONE_PARTICIPANT = 3,
    TC_REJECT_RETRY_AFTER = 4,
    TC_REJECT_RECEIVE_ONLY = 5,
    TC_REJECT_NO_RESOURCES = 6,
    TC_REJECT_OTHER = 255
};
inline const char* McvRejectCauseText(int v) {
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
enum McvRevokeCause {
    TC_REVOKE_ONLY_ONE_CLIENT = 1,
    TC_REVOKE_MEDIA_BURST_TOO_LONG = 2,
    TC_REVOKE_NO_PERMISSION = 3,
    TC_REVOKE_PREEMPTED = 4,
    TC_REVOKE_TERMINATE_STREAM = 5,
    TC_REVOKE_NO_RESOURCES = 6,
    TC_REVOKE_QUEUE_TRANSMISSION = 7,
    TC_REVOKE_NO_RECEIVER = 8,
    TC_REVOKE_OTHER = 255
};
inline const char* McvRevokeCauseText(int v) {
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
enum McvReceiveRejectCause {
    TC_RECV_REJECT_INTERNAL_ERROR = 2,
    TC_RECV_REJECT_RETRY_AFTER = 4,
    TC_RECV_REJECT_SEND_ONLY = 5,
    TC_RECV_REJECT_NO_RESOURCES = 6,
    TC_RECV_REJECT_MAX_STREAMS = 7,
    TC_RECV_REJECT_OTHER = 255
};
inline const char* McvReceiveRejectCauseText(int v) {
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

// §11 타이머(ms)·카운터 기본값 — 값의 정본은 service configuration `<tc-timers-counters-R14>`·그룹 문서.
#define MCV_T100_MS 1000  // Transmission Request (cims)
#define MCV_T101_MS 1000  // Transmission End Request (cims)
#define MCV_T102_MS 1000  // Transmission Queue Position Request (cims)
#define MCV_T103_MS 1000  // Receive Media Request (cims)
#define MCV_T104_MS 1000  // Receive Media Release (cims)
#define MCV_T1_MS 30000  // Inactivity (spec)
#define MCV_T2_MS 1000  // Transmission Idle (cims)
#define MCV_T3_MS 1000  // Transmission Revoke (spec)
#define MCV_T4_MS 1000  // Transmission Granted (spec)
#define MCV_T5_MS 30000  // Reception Inactivity (spec)
#define MCV_T6_MS 1000  // Reception Granted (spec)
#define MCV_T11_MS 10000  // Stream Reception Idle (spec)
#define MCV_C100 3  // Transmission Request
#define MCV_C101 3  // Transmission End Request
#define MCV_C102 3  // Transmission Queue Position Request
#define MCV_C103 3  // Receive Media Request
#define MCV_C104 3  // Receive Media Release
#define MCV_C2 10  // Transmission Idle
#define MCV_C4 3  // Transmission Granted
#define MCV_C6 3  // Reception Granted
#define MCV_C7 2  // Reception Accepted
#define MCV_C9 4  // Per-Participant Reception Accepted
#define MCV_C11 4  // Count of active receivers for the stream

#endif  // __TRANSMISSION_DEFS_H__
