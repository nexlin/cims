#include "nrtp_session.h"
#include <cstdlib>
#include <ctime>

namespace nrtp {

NRtpSession::NRtpSession() 
    : m_localPort(0), m_remotePort(0), 
      m_ssrc(0), m_sequenceNumber(0), m_payloadType(96)
{
    UdpSocket::InitSockets();
    std::srand((unsigned int)std::time(nullptr));
    m_ssrc = std::rand();
    m_sequenceNumber = std::rand() % 65535;
}

NRtpSession::~NRtpSession() {
    Close();
}

bool NRtpSession::Init(const std::string& localIp, uint16_t localPort) {
    m_localPort = localPort;

    if (!m_rtpSocket.Open() || !m_rtpSocket.Bind(localIp, localPort)) {
        return false;
    }

    if (!m_rtcpSocket.Open() || !m_rtcpSocket.Bind(localIp, localPort + 1)) {
        m_rtpSocket.Close();
        return false;
    }

    return true;
}

void NRtpSession::SetRemoteAddress(const std::string& ip, uint16_t remotePort) {
    m_remoteIp = ip;
    m_remotePort = remotePort;
}

bool NRtpSession::SendRtpPacket(NRtpPacket& packet) {
    // Populate packet fields if not set? 
    // Usually Session manages SeqNum and SSRC if they are default.
    if (packet.GetSSRC() == 0) packet.SetSSRC(m_ssrc);
    
    size_t size = 0;
    const uint8_t* data = packet.Serialize(size);
    if (!data || size == 0) return false;

    return m_rtpSocket.SendTo(data, size, m_remoteIp, m_remotePort) > 0;
}

bool NRtpSession::SendRtpData(const uint8_t* data, size_t size, uint32_t timestamp, bool marker) {
    NRtpPacket packet;
    packet.SetPayloadType(m_payloadType);
    packet.SetSequenceNumber(m_sequenceNumber++);
    packet.SetTimestamp(timestamp);
    packet.SetSSRC(m_ssrc);
    packet.SetMarker(marker);
    packet.SetPayload(data, size);

    return SendRtpPacket(packet);
}

int NRtpSession::ReceiveRtpPacket(NRtpPacket& outPacket) {
    uint8_t buffer[kMaxMtu];
    std::string senderIp;
    uint16_t senderPort;

    int bytes = m_rtpSocket.RecvFrom(buffer, kMaxMtu, senderIp, senderPort);
    if (bytes > 0) {
        if (outPacket.Parse(buffer, bytes)) {
            // Update last recv info
            m_lastRecvIp = senderIp;
            m_lastRecvPort = senderPort;
            return bytes;
        }
    }
    return -1;
}

bool NRtpSession::SendRtcpSr(uint32_t ntpSec, uint32_t ntpFrac, uint32_t octetCount, uint32_t packetCount) {
    NRtcpPacket rtcp;
    rtcp.SetType(RtcpType::SR);
    rtcp.SetSSRC(m_ssrc);
    // Rough Timestamp mapping for now
    rtcp.SetSenderInfo(ntpSec, ntpFrac, ntpFrac /* ts proxy*/, packetCount, octetCount);
    
    size_t size = 0;
    const uint8_t* data = rtcp.Serialize(size);
    
    // RTP port + 1
    return m_rtcpSocket.SendTo(data, size, m_remoteIp, m_remotePort + 1) > 0;
}

void NRtpSession::Close() {
    m_rtpSocket.Close();
    m_rtcpSocket.Close();
}

}
