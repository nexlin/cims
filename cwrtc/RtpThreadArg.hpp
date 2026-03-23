// RtpThreadArg.hpp - CRtpThreadArg implementation (included by RtpThread.cpp)
#include "CwrtcSetup.h"

CRtpThreadArg::CRtpThreadArg()
    : m_hWebRtcUdp(INVALID_SOCKET), m_hPbxUdp(INVALID_SOCKET)
    , m_iWebRtcUdpPort(0), m_iPbxUdpPort(0)
    , m_bStop(false), m_iCmpPort(0)
{
}

CRtpThreadArg::~CRtpThreadArg()
{
    Close();
}

bool CRtpThreadArg::CreateSocket(int iDtlsPort, int iRtpPort)
{
    m_hWebRtcUdp = UdpListen(iDtlsPort, NULL);
    m_hPbxUdp    = UdpListen(iRtpPort,  NULL);

    if (m_hWebRtcUdp == INVALID_SOCKET || m_hPbxUdp == INVALID_SOCKET) {
        Close();
        return false;
    }

    m_iWebRtcUdpPort = iDtlsPort;
    m_iPbxUdpPort    = iRtpPort;
    m_strIcePwd      = "FNPRfT4qUaVOKa0ivkn64mMY";
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
    m_iWebRtcUdpPort = 0;
    m_iPbxUdpPort    = 0;
}

void CRSAKeyCert::Clear()
{
    if (m_psttKey)  { EVP_PKEY_free(m_psttKey);  m_psttKey  = nullptr; }
    if (m_psttCert) { X509_free(m_psttCert);      m_psttCert = nullptr; }
}
