#ifndef NRTP_SESSION_H
#define NRTP_SESSION_H

#include <string>
#include <memory>
#include "nrtp_def.h"
#include "nrtp_socket.h"
#include "nrtp_packet.h"
#include "nrtp_rtcp.h"

namespace nrtp {

class NRtpSession {
public:
    NRtpSession();
    ~NRtpSession();

    // Initialize sockets (RTP port, RTCP port = port + 1)
    bool Init(const std::string& localIp, uint16_t localPort);

    // Set Destination
    void SetRemoteAddress(const std::string& ip, uint16_t remotePort);

    // Send RTP
    bool SendRtpPacket(NRtpPacket& packet);
    bool SendRtpData(const uint8_t* data, size_t size, uint32_t timestamp, bool marker);

    // Receive RTP
    // Returns bytes read, or -1 on error. 0 if no data (and non-blocking?)
    // Currently implementation is blocking or standard socket behavior
    int ReceiveRtpPacket(NRtpPacket& outPacket);

    // RTCP (Basic)
    bool SendRtcpSr(uint32_t ntpSec, uint32_t ntpFrac, uint32_t octetCount, uint32_t packetCount);

    void Close();

    uint16_t GetLocalPort() const { return m_localPort; }
    uint32_t GetCurrentSSRC() const { return m_ssrc; }

private:
    uint16_t m_localPort;
    std::string m_remoteIp;
    uint16_t m_remotePort; // RTP port. RTCP is +1
    
    UdpSocket m_rtpSocket;
    UdpSocket m_rtcpSocket;

    uint32_t m_ssrc;
    uint16_t m_sequenceNumber;
    uint8_t m_payloadType;
    
    // Remote Info Cache
    std::string m_lastRecvIp;
    uint16_t m_lastRecvPort;
};

}

#endif // NRTP_SESSION_H
