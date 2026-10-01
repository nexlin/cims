#include "cimsue/types.h"

namespace cimsue {

const char* toString(RegState s) {
    switch (s) {
        case RegState::Unregistered: return "unregistered";
        case RegState::Registering: return "registering";
        case RegState::Registered: return "registered";
        case RegState::Failed: return "failed";
    }
    return "?";
}

const char* toString(CallState s) {
    switch (s) {
        case CallState::Null: return "null";
        case CallState::Outgoing: return "outgoing";
        case CallState::Incoming: return "incoming";
        case CallState::Active: return "active";
        case CallState::Held: return "held";
        case CallState::Disconnected: return "disconnected";
    }
    return "?";
}

const char* toString(Transport t) {
    switch (t) {
        case Transport::UDP: return "udp";
        case Transport::TCP: return "tcp";
        case Transport::TLS: return "tls";
    }
    return "?";
}

const char* toString(FloorState s) {
    switch (s) {
        case FloorState::Idle: return "idle";
        case FloorState::Requesting: return "requesting";
        case FloorState::Speaking: return "speaking";
        case FloorState::Listening: return "listening";
        case FloorState::Queued: return "queued";
    }
    return "?";
}

const char* toString(FloorEvent::Kind k) {
    switch (k) {
        case FloorEvent::Kind::Granted: return "granted";
        case FloorEvent::Kind::Denied: return "denied";
        case FloorEvent::Kind::Idle: return "idle";
        case FloorEvent::Kind::Taken: return "taken";
        case FloorEvent::Kind::TalkerLeft: return "talker_left";
        case FloorEvent::Kind::Revoked: return "revoked";
        case FloorEvent::Kind::QueuePosition: return "queue_position";
        case FloorEvent::Kind::QueueCancelled: return "queue_cancelled";
        case FloorEvent::Kind::RequestTimeout: return "request_timeout";
        case FloorEvent::Kind::TalkLimit: return "talk_limit";
        case FloorEvent::Kind::Other: return "other";
    }
    return "?";
}

const char* toString(ConditionCause c) {
    switch (c) {
        case ConditionCause::Local: return "local";
        case ConditionCause::Confirmed: return "confirmed";
        case ConditionCause::Denied: return "denied";
        case ConditionCause::Advertised: return "advertised";
    }
    return "?";
}

const char* toString(McService s) {
    switch (s) {
        case McService::Mcptt: return "mcptt";
        case McService::McVideo: return "mcvideo";
    }
    return "?";
}

const char* toString(TransmissionState s) {
    switch (s) {
        case TransmissionState::NoPermission: return "no_permission";
        case TransmissionState::PendingRequest: return "pending_request";
        case TransmissionState::Permitted: return "permitted";
        case TransmissionState::PendingEnd: return "pending_end";
        case TransmissionState::Queued: return "queued";
    }
    return "?";
}

const char* toString(ReceptionState s) {
    switch (s) {
        case ReceptionState::Notified: return "notified";
        case ReceptionState::PendingRequest: return "pending_request";
        case ReceptionState::Receiving: return "receiving";
        case ReceptionState::PendingRelease: return "pending_release";
        case ReceptionState::Ended: return "ended";
    }
    return "?";
}

const char* toString(TransmissionEvent::Kind k) {
    switch (k) {
        case TransmissionEvent::Kind::Granted: return "granted";
        case TransmissionEvent::Kind::Rejected: return "rejected";
        case TransmissionEvent::Kind::Revoked: return "revoked";
        case TransmissionEvent::Kind::QueuePosition: return "queue_position";
        case TransmissionEvent::Kind::EndRequested: return "end_requested";
        case TransmissionEvent::Kind::Ended: return "ended";
        case TransmissionEvent::Kind::ReceiverJoined: return "receiver_joined";
        case TransmissionEvent::Kind::Idle: return "idle";
        case TransmissionEvent::Kind::QueueCancelled: return "queue_cancelled";
        case TransmissionEvent::Kind::RequestTimeout: return "request_timeout";
        case TransmissionEvent::Kind::Other: return "other";
    }
    return "?";
}

const char* toString(ReceptionEvent::Kind k) {
    switch (k) {
        case ReceptionEvent::Kind::Notified: return "notified";
        case ReceptionEvent::Kind::Granted: return "granted";
        case ReceptionEvent::Kind::Rejected: return "rejected";
        case ReceptionEvent::Kind::Ended: return "ended";
        case ReceptionEvent::Kind::Released: return "released";
        case ReceptionEvent::Kind::EndRequested: return "end_requested";
        case ReceptionEvent::Kind::RequestTimeout: return "request_timeout";
        case ReceptionEvent::Kind::Other: return "other";
    }
    return "?";
}

const char* toString(VideoRequestState s) {
    switch (s) {
        case VideoRequestState::None: return "none";
        case VideoRequestState::Sent: return "sent";
        case VideoRequestState::Received: return "received";
    }
    return "?";
}

const char* toString(VideoRequestEvent::Kind k) {
    switch (k) {
        case VideoRequestEvent::Kind::Received: return "received";
        case VideoRequestEvent::Kind::Accepted: return "accepted";
        case VideoRequestEvent::Kind::Declined: return "declined";
        case VideoRequestEvent::Kind::Failed: return "failed";
        case VideoRequestEvent::Kind::Withdrawn: return "withdrawn";
    }
    return "?";
}

}  // namespace cimsue
