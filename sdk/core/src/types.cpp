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

}  // namespace cimsue
