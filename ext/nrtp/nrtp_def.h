#ifndef NRTP_DEF_H
#define NRTP_DEF_H

#include <cstdint>
#include <cstddef>
#include <netinet/in.h> // For byte order macros if available, else handle manually?
// Windows specific
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace nrtp {

    // RTP Fixed Header
    struct RtpHeader {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32) // Assuming Windows is Little Endian usually
        uint8_t cc : 4;         // CSRC count
        uint8_t extension : 1;  // Extension bit
        uint8_t padding : 1;    // Padding bit
        uint8_t version : 2;    // Version
        
        uint8_t payloadType : 7;// Payload type
        uint8_t marker : 1;     // Marker bit
#else
        uint8_t version : 2;    // Version
        uint8_t padding : 1;    // Padding bit
        uint8_t extension : 1;  // Extension bit
        uint8_t cc : 4;         // CSRC count
        
        uint8_t marker : 1;     // Marker bit
        uint8_t payloadType : 7;// Payload type
#endif
        uint16_t sequenceNumber;
        uint32_t timestamp;
        uint32_t ssrc;
    };

    // Constants
    constexpr size_t kRtpHeaderSize = sizeof(RtpHeader);
    constexpr size_t kMaxMtu = 1500;
    
    // Helper to handle endianness for header fields that span byte boundaries? 
    // Bitfields are compiler dependent, usually discouraged for portable binary protocols, 
    // but common in existing RTP stacks.
    // Ideally we serialize/deserialize using bit shifts, but for now struct is convenient if we assume packing.
    
}

#endif // NRTP_DEF_H
