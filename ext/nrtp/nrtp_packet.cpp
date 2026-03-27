#include "nrtp_packet.h"
#include <cstring>

#ifdef _WIN32
#include <winsock2.h> // for htons
#else
#include <arpa/inet.h>
#endif

namespace nrtp {

NRtpPacket::NRtpPacket() {
    std::memset(&m_header, 0, sizeof(RtpHeader));
    m_header.version = 2;
}

void NRtpPacket::SetVersion(uint8_t version) { m_header.version = version; }
void NRtpPacket::SetPadding(bool padding) { m_header.padding = padding ? 1 : 0; }
void NRtpPacket::SetExtension(bool extension) { m_header.extension = extension ? 1 : 0; }
void NRtpPacket::SetMarker(bool marker) { m_header.marker = marker ? 1 : 0; }
void NRtpPacket::SetPayloadType(uint8_t pt) { m_header.payloadType = pt; }
void NRtpPacket::SetSequenceNumber(uint16_t seq) { m_header.sequenceNumber = seq; } // stored host order here, convert later? Or store net order?
// Decisions: Stores in Host Order in member struct, convert during serialize.
// Note: The struct definition in nrtp_def.h uses bitfields which are memory layout dependent. 
// Standard integer fields like sequenceNumber are 16bit. 
// Let's assume m_header struct stores values in HOST byte order for integer fields, but bitfields are raw. 
// Actually, usually headers are constructed carefully. 
// For simplicity, let's treat the RtpHeader struct as "Memory Representation" and handle endianness during serialize.

void NRtpPacket::SetTimestamp(uint32_t ts) { m_header.timestamp = ts; }
void NRtpPacket::SetSSRC(uint32_t ssrc) { m_header.ssrc = ssrc; }

void NRtpPacket::SetPayload(const uint8_t* data, size_t size) {
    m_payload.assign(data, data + size);
}

bool NRtpPacket::Parse(const uint8_t* data, size_t size) {
    if (size < sizeof(RtpHeader)) return false;

    // Copy header
    std::memcpy(&m_header, data, sizeof(RtpHeader)); // This relies on struct layout matching wire. Bitfields are dangerous here! 
    // To be strictly correct/clean, we should parse byte-by-byte for bitfields.
    
    // Correct Endianness for 16/32 bit fields
    m_header.sequenceNumber = ntohs(m_header.sequenceNumber);
    m_header.timestamp = ntohl(m_header.timestamp);
    m_header.ssrc = ntohl(m_header.ssrc);

    // Payload
    size_t payloadSize = size - sizeof(RtpHeader);
    if (payloadSize > 0) {
        m_payload.assign(data + sizeof(RtpHeader), data + size);
    } else {
        m_payload.clear();
    }
    return true;
}

uint8_t NRtpPacket::GetVersion() const { return m_header.version; }
bool NRtpPacket::HasPadding() const { return m_header.padding; }
bool NRtpPacket::HasExtension() const { return m_header.extension; }
bool NRtpPacket::GetMarker() const { return m_header.marker; }
uint8_t NRtpPacket::GetPayloadType() const { return m_header.payloadType; }
uint16_t NRtpPacket::GetSequenceNumber() const { return m_header.sequenceNumber; }
uint32_t NRtpPacket::GetTimestamp() const { return m_header.timestamp; }
uint32_t NRtpPacket::GetSSRC() const { return m_header.ssrc; }

uint8_t* NRtpPacket::GetPayloadData() { return m_payload.data(); }
size_t NRtpPacket::GetPayloadSize() const { return m_payload.size(); }

const uint8_t* NRtpPacket::Serialize(size_t& outSize) {
    size_t total = sizeof(RtpHeader) + m_payload.size();
    if (m_buffer.size() < total) {
        m_buffer.resize(total);
    }
    
    RtpHeader hdr = m_header;
    // Convert to Network Byte Order
    hdr.sequenceNumber = htons(hdr.sequenceNumber);
    hdr.timestamp = htonl(hdr.timestamp);
    hdr.ssrc = htonl(hdr.ssrc);
    
    // Copy Header
    std::memcpy(m_buffer.data(), &hdr, sizeof(RtpHeader));
    
    // Copy Payload
    if (!m_payload.empty()) {
        std::memcpy(m_buffer.data() + sizeof(RtpHeader), m_payload.data(), m_payload.size());
    }
    
    outSize = total;
    return m_buffer.data();
}

}
