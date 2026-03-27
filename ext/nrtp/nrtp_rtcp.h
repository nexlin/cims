#ifndef NRTP_RTCP_H
#define NRTP_RTCP_H

#include <cstdint>
#include <vector>
#include "nrtp_def.h"

namespace nrtp {

enum class RtcpType : uint8_t {
    SR = 200,
    RR = 201,
    SDES = 202,
    BYE = 203,
    APP = 204
};

struct RtcpCommonHeader {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    uint8_t count : 5;      // Report count
    uint8_t padding : 1;
    uint8_t version : 2;
#else
    uint8_t version : 2;
    uint8_t padding : 1;
    uint8_t count : 5;
#endif
    uint8_t packetType;     // SR/RR/etc
    uint16_t length;        // Length in 32-bit words - 1
}; // 4 bytes

struct RtcpSenderInfo {
    uint32_t ntpSec;
    uint32_t ntpFrac;
    uint32_t rtpTimestamp;
    uint32_t packetCount;
    uint32_t octetCount;
};

struct RtcpAppInfo {
    char name[4]; // 4 ASCII characters
    // variable length data follows
};

// Simplified Wrapper
class NRtcpPacket {
public:
    NRtcpPacket();
    void SetType(RtcpType type);
    void SetSSRC(uint32_t ssrc);
    
    // For SR
    void SetSenderInfo(uint32_t ntpSec, uint32_t ntpFrac, uint32_t rtpTs, uint32_t pktCount, uint32_t octCount);
    
    // For APP
    void SetAppData(uint8_t subtype, const char* name, const uint8_t* data, size_t len);

    const uint8_t* Serialize(size_t& outSize);
    
private:
    std::vector<uint8_t> m_buffer;
    uint32_t m_ssrc;
    RtcpSenderInfo m_senderInfo;
    
    // App Data
    uint8_t m_subtype;
    char m_appName[4];
    std::vector<uint8_t> m_appData;

    RtcpType m_type;
};

}

#endif // NRTP_RTCP_H
