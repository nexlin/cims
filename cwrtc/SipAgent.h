#pragma once
#include "SipUserAgent.h"
#include "SipUserAgentCallBack.h"
#include "SipServerInfo.h"
#include <string>

class CSipAgent : public ISipUserAgentCallBack
{
public:
    bool Start();
    void Stop();

    bool RegisterUser(const std::string& userId, const std::string& password,
                      const std::string& domain, const std::string& authId);
    bool UnregisterUser(const std::string& userId, const std::string& domain);

    bool StartOutgoingCall(const std::string& fromUser, const std::string& toUser,
                           const std::string& browserSdp,
                           const std::string& wsIp, int wsPort,
                           std::string& strCallIdOut);

    bool AcceptIncomingCall(const std::string& callId, const std::string& browserSdp);
    bool HangupCall(const std::string& callId);

    // ISipUserAgentCallBack (정확한 시그니처)
    void EventRegister(CSipServerInfo* pclsInfo, int iStatus) override;
    void EventIncomingCall(const char* pszCallId, const char* pszFrom, const char* pszTo,
                           CSipCallRtp* pclsRtp) override;
    void EventCallRing(const char* pszCallId, int iSipStatus, CSipCallRtp* pclsRtp) override;
    void EventCallStart(const char* pszCallId, CSipCallRtp* pclsRtp) override;
    void EventCallEnd(const char* pszCallId, int iSipStatus) override;

private:
    bool AllocPorts(int& iDtlsPort, int& iRtpPort);
    std::string BuildDtlsSdp(int iDtlsPort, int iAudioPt, bool bVideoEnabled);
    std::string BuildPlainRtpSdp(int iRtpPort, int iAudioPt);
    void SendWsJson(const std::string& wsIp, int wsPort, const std::string& json);
};

extern CSipUserAgent gclsSipUserAgent;
extern CSipAgent     gclsSipAgent;
