#pragma once
#include "SipStackDefine.h"
#include "SipUdp.h"
#include "TlsFunction.h"
#include <string>

class CRtpThreadArg
{
public:
    CRtpThreadArg();
    ~CRtpThreadArg();
    bool CreateSocket(int iDtlsPort, int iRtpPort);
    void Close();

    // 오디오 소켓
    Socket        m_hWebRtcUdp;
    Socket        m_hPbxUdp;
    Socket        m_hPbxRtcpUdp;   // CMP RTCP 수신 소켓 (iRtpPort+1)
    int           m_iWebRtcUdpPort;
    int           m_iPbxUdpPort;
    int           m_iPbxRtcpPort;   // = m_iPbxUdpPort + 1

    // 비디오 소켓 (bVideoEnabled == true일 때만 유효)
    Socket        m_hVideoWebRtcUdp;   // 비디오 DTLS (브라우저)
    Socket        m_hVideoPbxUdp;      // 비디오 RTP (CMP)
    int           m_iVideoWebRtcUdpPort;
    int           m_iVideoPbxUdpPort;
    int           m_iCmpVideoPort;     // CMP 비디오 RTP 포트
    bool          m_bVideoEnabled;

    volatile bool m_bStop;

    std::string   m_strCallId;
    std::string   m_strUserId;
    std::string   m_strCmpIp;
    int           m_iCmpPort;
    std::string   m_strBrowserSdp;
    std::string   m_strIcePwd;
    std::string   m_strWsIp;        // 브라우저 WS IP (floor event 전달용)
    int           m_iWsPort;        // 브라우저 WS 포트

    // Floor control: RtpThread의 RTCP 소켓으로 CMP에 전송
    void SendFloorViaCmp(uint8_t opcode);
};

class CRSAKeyCert
{
public:
    CRSAKeyCert() : m_psttKey(nullptr), m_psttCert(nullptr) {}
    void Clear();
    EVP_PKEY*   m_psttKey;
    X509*       m_psttCert;
    std::string m_strFingerPrint;
};

void InitDtls();
void FinalDtls();
bool StartRtpThread(CRtpThreadArg* pclsArg);

extern CRSAKeyCert gclsKeyCert;
