#ifndef NRTP_PACKET_H
#define NRTP_PACKET_H

#include <vector>
#include <cstdint>
#include "nrtp_def.h"

namespace nrtp {

class NRtpPacket {
public:
    NRtpPacket();
    ~NRtpPacket() = default;

    // Setters
    void SetVersion(uint8_t version);
    void SetPadding(bool padding);
    void SetExtension(bool extension);
    void SetMarker(bool marker);
    void SetPayloadType(uint8_t pt);
    void SetSequenceNumber(uint16_t seq);
    void SetTimestamp(uint32_t ts);
    void SetSSRC(uint32_t ssrc);

    void SetPayload(const uint8_t* data, size_t size);
    
    // Parser
    bool Parse(const uint8_t* data, size_t size);

    // Getters
    uint8_t GetVersion() const;
    bool HasPadding() const;
    bool HasExtension() const;
    bool GetMarker() const;
    uint8_t GetPayloadType() const;
    uint16_t GetSequenceNumber() const;
    uint32_t GetTimestamp() const;
    uint32_t GetSSRC() const;
    
    // Get pointer to payload
    uint8_t* GetPayloadData();
    size_t GetPayloadSize() const;

    // Serialize to buffer for sending
    // Returns pointer to internal buffer and total size
    const uint8_t* Serialize(size_t& outSize);

private:
    RtpHeader m_header;
    std::vector<uint8_t> m_payload;
    std::vector<uint8_t> m_buffer; // Full packet buffer cache
};

}

#endif // NRTP_PACKET_H
