#include "nrtp_rtcp.h"
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace nrtp {

NRtcpPacket::NRtcpPacket() : m_ssrc(0), m_type(RtcpType::SR), m_subtype(0) {
    std::memset(&m_senderInfo, 0, sizeof(m_senderInfo));
    std::memset(m_appName, 0, 4);
}

void NRtcpPacket::SetType(RtcpType type) { m_type = type; }
void NRtcpPacket::SetSSRC(uint32_t ssrc) { m_ssrc = ssrc; }
void NRtcpPacket::SetSenderInfo(uint32_t ntpSec, uint32_t ntpFrac, uint32_t rtpTs, uint32_t pktCount, uint32_t octCount) {
    m_senderInfo.ntpSec = ntpSec;
    m_senderInfo.ntpFrac = ntpFrac;
    m_senderInfo.rtpTimestamp = rtpTs;
    m_senderInfo.packetCount = pktCount;
    m_senderInfo.packetCount = pktCount;
    m_senderInfo.octetCount = octCount;
}

void NRtcpPacket::SetAppData(uint8_t subtype, const char* name, const uint8_t* data, size_t len) {
    m_type = RtcpType::APP;
    m_subtype = subtype; // 5 bits max usually, checking handled by caller or mask likely needed?
    // standard says subtype is 5 bits.
    m_subtype &= 0x1F; 
    
    if (name) {
        std::memcpy(m_appName, name, 4);
    }
    
    if (data && len > 0) {
        m_appData.assign(data, data + len);
    } else {
        m_appData.clear();
    }
}

const uint8_t* NRtcpPacket::Serialize(size_t& outSize) {
    m_buffer.clear();
    
    // Minimal SR or RR serialization
    // Header
    RtcpCommonHeader hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.version = 2;
    hdr.padding = 0;
    
    if (m_type == RtcpType::APP) {
        hdr.count = m_subtype; // Subtype for APP
    } else {
        hdr.count = 0; // RC for others (fixme if needed)
    }
    hdr.packetType = (uint8_t)m_type;
    
    uint16_t lenWords = 0;
    
    if (m_type == RtcpType::SR) {
        // SSCR + SenderInfo
        lenWords = (sizeof(uint32_t) + sizeof(RtcpSenderInfo)) / 4; 
    } else if (m_type == RtcpType::RR) {
        lenWords = (sizeof(uint32_t)) / 4; 
    } else if (m_type == RtcpType::APP) {
        // SSRC + Name(4) + Data
        size_t contentSize = sizeof(uint32_t) + 4 + m_appData.size();
        // Padding check
        size_t remainder = contentSize % 4;
        if (remainder > 0) {
             // Padding handled by logic below? 
             // length in header counts 32-bit words. content must be multiple of 4.
             // We need to add padding bytes to m_appData or account for it.
             // Simplest: just resize m_appData to verify padding logic.
             // BUT Serialize regenerates buffer. 
             // Let's calculate padding needed.
        }
        lenWords = (uint16_t)((contentSize + 3) / 4);
    }
    
    hdr.length = htons(lenWords);
    
    // Header to buffer
    size_t headerSize = sizeof(hdr);
    m_buffer.resize(headerSize + lenWords * 4);
    
    // Manually handle bitfields issue if compiling on other platforms? 
    // Assuming compatible bitfield packing for now as done in previous files.
    std::memcpy(m_buffer.data(), &hdr, headerSize);
    
    uint8_t* ptr = m_buffer.data() + headerSize;
    
    // SSRC
    uint32_t nSsrc = htonl(m_ssrc);
    std::memcpy(ptr, &nSsrc, sizeof(uint32_t));
    ptr += 4;
    
    if (m_type == RtcpType::SR) {
        uint32_t temp;
        temp = htonl(m_senderInfo.ntpSec); std::memcpy(ptr, &temp, 4); ptr += 4;
        temp = htonl(m_senderInfo.ntpFrac); std::memcpy(ptr, &temp, 4); ptr += 4;
        temp = htonl(m_senderInfo.rtpTimestamp); std::memcpy(ptr, &temp, 4); ptr += 4;
        temp = htonl(m_senderInfo.packetCount); std::memcpy(ptr, &temp, 4); ptr += 4;
        temp = htonl(m_senderInfo.octetCount); std::memcpy(ptr, &temp, 4); ptr += 4;
    } else if (m_type == RtcpType::APP) {
        // Name
        std::memcpy(ptr, m_appName, 4); 
        ptr += 4;
        
        // Data
        if (!m_appData.empty()) {
            std::memcpy(ptr, m_appData.data(), m_appData.size());
            ptr += m_appData.size();
        }
        
        // Padding
        size_t totalUnpadded = sizeof(uint32_t) + 4 + m_appData.size();
        size_t padNeeded = (4 - (totalUnpadded % 4)) % 4;
        if (padNeeded > 0) {
             std::memset(ptr, 0, padNeeded);
             
             // If padding bit is set in header?
             // Standard RTCP: if padding bit is set, last octet contains count of padding octets.
             // We didn't set padding bit in header above. 
             // Usually APP packets just fill up to 32bit boundary implicitly or explicit padding?
             // RFC 3550: "The length of this APP packet... in 32-bit words"
             // So we must be 32-bit aligned.
             // If we add padding bytes simply to align, we rely on the receiver knowing the data format 
             // OR we should set the P bit and put the count at the end.
             // For implementation simplicity, let's assume we just pad with zeros to align to 4 bytes
             // and the APP data consumer knows how to read it, OR we implement full P-bit support.
             // Let's implement full P-bit support to be correct.
             
             // Re-write header with P bit
             RtcpCommonHeader* pHdr = (RtcpCommonHeader*)m_buffer.data();
             pHdr->padding = 1;
             
             ptr += padNeeded - 1; // move to last byte
             *ptr = (uint8_t)padNeeded; // set padding count
             ptr++; // end
        }
    }
    
    outSize = m_buffer.size();
    return m_buffer.data();
}

}
