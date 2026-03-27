#ifndef NRTP_TBCP_H
#define NRTP_TBCP_H

#include <cstdint>
#include <string>
#include <cstring>
#include "nrtp_packet.h"

// TBCP (Talk Burst Control Protocol) for MCPTT
// Usually uses RTCP APP packets with Name="TBCP"
// Subtypes map to specific TBCP messages.

namespace nrtp {

namespace tbcp {

    constexpr const char* APP_NAME = "TBCP";

    enum class MsgType : uint8_t {
         FloorRequest = 1,
         FloorGranted = 2,
         FloorDeny = 3,
         FloorRelease = 4,
         FloorIdle = 5,
         FloorRevoke = 6,
         QueueStatus = 7,
         Unknown = 0
    };

    // Helper to check if a packet is a valid TBCP packet
    // Note: To use this check, one would likely inspect the raw packet or the NRtcpPacket state.
    // Since NRtcpPacket doesn't expose getters for Name/Subtype publicly in our simple implementation yet,
    // we would rely on the application logic constructing it. 
    // Ideally we'd add Getters to NRtcpPacket.

    // TBCP Message Construction Helpers
    struct TbcpBuilder {
        static void BuildFloorRequest(NRtcpPacket& outPacket, uint32_t ssrc, uint8_t priority) {
             outPacket.SetSSRC(ssrc);
             uint8_t payload[1] = { priority };
             outPacket.SetAppData((uint8_t)MsgType::FloorRequest, APP_NAME, payload, 1);
        }

        static void BuildFloorGranted(NRtcpPacket& outPacket, uint32_t ssrc, uint32_t duration, uint32_t grantTimestamp) {
             outPacket.SetSSRC(ssrc);
             // Example Payload: Duration (2 bytes), Timestamp (4 bytes)? 
             // Simplified generic payload for demo
             uint8_t payload[8]; 
             // simple copy
             std::memcpy(payload, &duration, 4);
             std::memcpy(payload + 4, &grantTimestamp, 4);
             outPacket.SetAppData((uint8_t)MsgType::FloorGranted, APP_NAME, payload, 8);
        }
        
        static void BuildFloorRelease(NRtcpPacket& outPacket, uint32_t ssrc) {
             outPacket.SetSSRC(ssrc);
             outPacket.SetAppData((uint8_t)MsgType::FloorRelease, APP_NAME, nullptr, 0);
        }

        static void BuildFloorIdle(NRtcpPacket& outPacket, uint32_t ssrc) {
             outPacket.SetSSRC(ssrc);
             outPacket.SetAppData((uint8_t)MsgType::FloorIdle, APP_NAME, nullptr, 0);
        }
    };
}

}

#endif // NRTP_TBCP_H
