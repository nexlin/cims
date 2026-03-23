/*
 * RtpThread.cpp - DTLS-SRTP (브라우저) ↔ 평문 RTP (CMP) 릴레이
 *
 * 흐름:
 *  1. 브라우저 ICE (STUN) 처리
 *  2. DTLS 핸드셰이크 → SRTP 키 추출
 *  3. 양방향 패킷 릴레이:
 *     브라우저 SRTP → srtp_unprotect → CMP RTP
 *     CMP RTP       → srtp_protect   → 브라우저 SRTP
 */
#include "SipPlatformDefine.h"
#include "ServerUtility.h"
#include "StunMessage.h"
#include "SdpMessage.h"
#include "RtpThread.h"
#include "srtp.h"
#include "Log.h"
#include "MemoryDebug.h"

#include "RtpThreadArg.hpp"
#include "RtpThreadDtls.hpp"

static bool GetIceUserPwd(const char* pszSdp, std::string& strIceUser, std::string& strIcePwd)
{
    CSdpMessage clsSdp;
    if (clsSdp.Parse(pszSdp, strlen(pszSdp)) == -1) return false;

    for (auto& media : clsSdp.m_clsMediaList) {
        if (media.m_strMedia != "audio") continue;
        for (auto& attr : media.m_clsAttributeList) {
            if (attr.m_strName == "ice-ufrag") strIceUser = attr.m_strValue;
            else if (attr.m_strName == "ice-pwd") {
                strIcePwd = attr.m_strValue;
                return true;
            }
        }
    }
    return false;
}

THREAD_API RtpThread(LPVOID lpParameter)
{
    CRtpThreadArg* pclsArg = (CRtpThreadArg*)lpParameter;

    char  szPacket[4096], szWebRTCIp[64], szPbxIp[64];
    unsigned short iWebRTCPort = 0, iPbxPort = 0;
    int   iPacketLen, n;
    pollfd sttPoll[2];

    std::string strIceUser, strIcePwd;
    srtp_t psttSrtpTx = NULL, psttSrtpRx = NULL;
    bool   bDtls = false, bSendRtpToWebRTC = false;

    szWebRTCIp[0] = '\0';
    szPbxIp[0]    = '\0';

    // 브라우저 SDP에서 ICE 정보 추출
    GetIceUserPwd(pclsArg->m_strBrowserSdp.c_str(), strIceUser, strIcePwd);
    strIceUser.append(":lMRb");  // append cwrtc's ufrag

    // CMP가 이미 알려진 경우 PBX IP 초기화 (착신 시)
    if (!pclsArg->m_strCmpIp.empty() && pclsArg->m_iCmpPort > 0) {
        snprintf(szPbxIp, sizeof(szPbxIp), "%s", pclsArg->m_strCmpIp.c_str());
        iPbxPort = (unsigned short)pclsArg->m_iCmpPort;
    }

    TcpSetPollIn(sttPoll[0], pclsArg->m_hWebRtcUdp);
    TcpSetPollIn(sttPoll[1], pclsArg->m_hPbxUdp);

    CLog::Print(LOG_INFO, "RtpThread[%s] Start dtls=%d rtp=%d cmp=%s:%d",
        pclsArg->m_strCallId.c_str(),
        pclsArg->m_iWebRtcUdpPort, pclsArg->m_iPbxUdpPort,
        pclsArg->m_strCmpIp.c_str(), pclsArg->m_iCmpPort);

    while (!pclsArg->m_bStop) {
        n = poll(sttPoll, 2, 1000);
        if (n <= 0) continue;

        // ── 브라우저 측 소켓 ──
        if (sttPoll[0].revents & POLLIN) {
            iPacketLen = sizeof(szPacket);
            UdpRecv(pclsArg->m_hWebRtcUdp, szPacket, &iPacketLen, szWebRTCIp, sizeof(szWebRTCIp), &iWebRTCPort);

            if (iPacketLen >= 20 && (unsigned char)szPacket[0] == 0x00 && (unsigned char)szPacket[1] == 0x01) {
                // ICE STUN Binding Request → 응답 + cwrtc→브라우저 STUN Request 전송
                CStunMessage clsStunReq;
                if (clsStunReq.Parse(szPacket, iPacketLen) == -1) continue;

                CStunMessage* pResp = clsStunReq.CreateResponse(true);
                if (!pResp) continue;

                pResp->m_strPassword = pclsArg->m_strIcePwd;
                pResp->AddXorMappedAddress(szWebRTCIp, iWebRTCPort);
                pResp->AddMessageIntegrity();
                pResp->AddFingerPrint();
                iPacketLen = pResp->ToString(szPacket, sizeof(szPacket));
                delete pResp;
                UdpSend(pclsArg->m_hWebRtcUdp, szPacket, iPacketLen, szWebRTCIp, iWebRTCPort);

                // cwrtc→브라우저 binding request
                clsStunReq.m_clsAttributeList.clear();
                clsStunReq.m_strPassword = strIcePwd;
                clsStunReq.AddUserName(strIceUser.c_str());
                clsStunReq.AddMessageIntegrity();
                clsStunReq.AddFingerPrint();
                iPacketLen = clsStunReq.ToString(szPacket, sizeof(szPacket));
                UdpSend(pclsArg->m_hWebRtcUdp, szPacket, iPacketLen, szWebRTCIp, iWebRTCPort);

            } else if (!bDtls && iPacketLen >= 13 &&
                       (unsigned char)szPacket[0] >= 20 && (unsigned char)szPacket[0] <= 63) {
                // DTLS ClientHello (or other DTLS record)
                SSL* psttSsl;
                struct sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port   = htons(iWebRTCPort);
                inet_pton(AF_INET, szWebRTCIp, &addr.sin_addr.s_addr);

                if (connect(pclsArg->m_hWebRtcUdp, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
                    CLog::Print(LOG_ERROR, "RtpThread[%s] DTLS connect error", pclsArg->m_strCallId.c_str());
                } else if ((psttSsl = SSL_new(gpsttClientCtx)) != NULL) {
                    SSL_set_fd(psttSsl, (int)pclsArg->m_hWebRtcUdp);
                    SSL_set_tlsext_use_srtp(psttSsl, "SRTP_AES128_CM_SHA1_80");

                    if (SSL_connect(psttSsl) != -1) {
                        uint8_t szMaterial[60];
                        SSL_export_keying_material(psttSsl, szMaterial, sizeof(szMaterial),
                                                   "EXTRACTOR-dtls_srtp", 19, NULL, 0, 0);
                        uint8_t* pLocalKey   = szMaterial;
                        uint8_t* pRemoteKey  = pLocalKey   + 16;
                        uint8_t* pLocalSalt  = pRemoteKey  + 16;
                        uint8_t* pRemoteSalt = pLocalSalt  + 14;
                        SrtpCreate(&psttSrtpTx, pLocalKey,  pLocalSalt,  false);
                        SrtpCreate(&psttSrtpRx, pRemoteKey, pRemoteSalt, true);
                        bDtls = true;
                        CLog::Print(LOG_INFO, "RtpThread[%s] DTLS OK, SRTP ready", pclsArg->m_strCallId.c_str());
                    } else {
                        CLog::Print(LOG_ERROR, "RtpThread[%s] SSL_connect failed", pclsArg->m_strCallId.c_str());
                    }
                    SSL_free(psttSsl);
                }

            } else if (iPbxPort > 0 && bDtls) {
                // SRTP 패킷 → 복호화 → CMP로 전달
                if (psttSrtpRx) srtp_unprotect(psttSrtpRx, szPacket, &iPacketLen);
                UdpSend(pclsArg->m_hPbxUdp, szPacket, iPacketLen, szPbxIp, iPbxPort);
                bSendRtpToWebRTC = true;
            }
        }

        // ── CMP 측 소켓 ──
        if (sttPoll[1].revents & POLLIN) {
            iPacketLen = sizeof(szPacket);
            UdpRecv(pclsArg->m_hPbxUdp, szPacket, &iPacketLen, szPbxIp, sizeof(szPbxIp), &iPbxPort);

            if (bSendRtpToWebRTC && iWebRTCPort > 0) {
                // SSRC 고정 (브라우저가 기대하는 SSRC로 교체)
                int32_t iSsrc = htonl(100);
                memcpy(szPacket + 8, &iSsrc, 4);

                if (psttSrtpTx) srtp_protect(psttSrtpTx, szPacket, &iPacketLen);
                UdpSend(pclsArg->m_hWebRtcUdp, szPacket, iPacketLen, szWebRTCIp, iWebRTCPort);
            }
        }
    }

    if (psttSrtpTx) srtp_dealloc(psttSrtpTx);
    if (psttSrtpRx) srtp_dealloc(psttSrtpRx);

    CLog::Print(LOG_INFO, "RtpThread[%s] Stopped", pclsArg->m_strCallId.c_str());

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    ERR_remove_thread_state(NULL);
#endif
    delete pclsArg;
    return 0;
}

bool StartRtpThread(CRtpThreadArg* pclsArg)
{
    return StartThread("RtpThread", RtpThread, pclsArg);
}
