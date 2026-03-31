// RtpThreadArg.hpp - CRtpThreadArg implementation (included by RtpThread.cpp)
#include "CwrtcSetup.h"

CRtpThreadArg::CRtpThreadArg()
    : m_hWebRtcUdp(INVALID_SOCKET), m_hPbxUdp(INVALID_SOCKET)
    , m_hPbxRtcpUdp(INVALID_SOCKET)
    , m_iWebRtcUdpPort(0), m_iPbxUdpPort(0), m_iPbxRtcpPort(0)
    , m_hVideoWebRtcUdp(INVALID_SOCKET), m_hVideoPbxUdp(INVALID_SOCKET)
    , m_iVideoWebRtcUdpPort(0), m_iVideoPbxUdpPort(0)
    , m_iCmpVideoPort(0), m_bVideoEnabled(false)
    , m_bStop(false), m_iCmpPort(0)
    , m_iWsPort(0)
{
}

CRtpThreadArg::~CRtpThreadArg()
{
    Close();
}

bool CRtpThreadArg::CreateSocket(int iDtlsPort, int iRtpPort)
{
    // 오디오: dtls=base+0, rtp=base+1, rtcp=base+2
    m_hWebRtcUdp  = UdpListen(iDtlsPort,      NULL);
    m_hPbxUdp     = UdpListen(iRtpPort,        NULL);
    m_hPbxRtcpUdp = UdpListen(iRtpPort + 1,   NULL);  // CMP RTCP 수신용

    if (m_hWebRtcUdp == INVALID_SOCKET || m_hPbxUdp == INVALID_SOCKET ||
        m_hPbxRtcpUdp == INVALID_SOCKET) {
        Close();
        return false;
    }

    m_iWebRtcUdpPort = iDtlsPort;
    m_iPbxUdpPort    = iRtpPort;
    m_iPbxRtcpPort   = iRtpPort + 1;
    m_strIcePwd      = "FNPRfT4qUaVOKa0ivkn64mMY";

    // 비디오: video_dtls=base+3, video_rtp=base+4 (base = iDtlsPort)
    if (m_bVideoEnabled) {
        int iVideoDtlsPort = iDtlsPort + 3;
        int iVideoRtpPort  = iDtlsPort + 4;
        m_hVideoWebRtcUdp = UdpListen(iVideoDtlsPort, NULL);
        m_hVideoPbxUdp    = UdpListen(iVideoRtpPort,  NULL);

        if (m_hVideoWebRtcUdp == INVALID_SOCKET || m_hVideoPbxUdp == INVALID_SOCKET) {
            Close();
            return false;
        }

        m_iVideoWebRtcUdpPort = iVideoDtlsPort;
        m_iVideoPbxUdpPort    = iVideoRtpPort;
    }

    return true;
}

void CRtpThreadArg::Close()
{
    if (m_hWebRtcUdp != INVALID_SOCKET) {
        closesocket(m_hWebRtcUdp);
        m_hWebRtcUdp = INVALID_SOCKET;
    }
    if (m_hPbxUdp != INVALID_SOCKET) {
        closesocket(m_hPbxUdp);
        m_hPbxUdp = INVALID_SOCKET;
    }
    if (m_hPbxRtcpUdp != INVALID_SOCKET) {
        closesocket(m_hPbxRtcpUdp);
        m_hPbxRtcpUdp = INVALID_SOCKET;
    }
    if (m_hVideoWebRtcUdp != INVALID_SOCKET) {
        closesocket(m_hVideoWebRtcUdp);
        m_hVideoWebRtcUdp = INVALID_SOCKET;
    }
    if (m_hVideoPbxUdp != INVALID_SOCKET) {
        closesocket(m_hVideoPbxUdp);
        m_hVideoPbxUdp = INVALID_SOCKET;
    }
    m_iWebRtcUdpPort      = 0;
    m_iPbxUdpPort         = 0;
    m_iPbxRtcpPort        = 0;
    m_iVideoWebRtcUdpPort = 0;
    m_iVideoPbxUdpPort    = 0;
}

void CRtpThreadArg::SendFloorViaCmp(uint8_t opcode)
{
    if (m_hPbxRtcpUdp == INVALID_SOCKET || m_strCmpIp.empty() || m_iCmpPort <= 0)
        return;

    // RTCP APP 패킷: 고정 헤더(12) + opcode+id_len+reserved(4) = 16 bytes (speaker ID 없음)
    uint8_t pkt[16];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x80;        // V=2
    pkt[1] = 204;         // PT=APP
    uint16_t len = htons(sizeof(pkt) / 4 - 1);
    memcpy(pkt + 2, &len, 2);
    // ssrc = 0 (CMP는 source IP:port로 sender 매칭)
    memcpy(pkt + 8, "MCPT", 4);
    pkt[12] = opcode;
    // id_len = 0, reserved = 0

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(m_strCmpIp.c_str());
    addr.sin_port        = htons((uint16_t)(m_iCmpPort + 1));  // CMP RTCP 포트 = RTP + 1

    sendto((int)m_hPbxRtcpUdp, pkt, sizeof(pkt), 0,
           (struct sockaddr*)&addr, sizeof(addr));
}

void CRSAKeyCert::Clear()
{
    if (m_psttKey)  { EVP_PKEY_free(m_psttKey);  m_psttKey  = nullptr; }
    if (m_psttCert) { X509_free(m_psttCert);      m_psttCert = nullptr; }
}
