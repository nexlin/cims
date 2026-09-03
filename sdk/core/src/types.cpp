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

}  // namespace cimsue
