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

    Socket        m_hWebRtcUdp;
    Socket        m_hPbxUdp;
    int           m_iWebRtcUdpPort;
    int           m_iPbxUdpPort;
    volatile bool m_bStop;

    std::string   m_strCallId;
    std::string   m_strUserId;
    std::string   m_strCmpIp;
    int           m_iCmpPort;
    std::string   m_strBrowserSdp;
    std::string   m_strIcePwd;
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
