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
#include "HttpCallBack.h"
#include "SimpleJson.h"

// Floor Control 패킷 헤더 (CMP McpttGroup과 동일)
#pragma pack(push, 1)
struct CwrtcFloorHdr {
    uint8_t  ver_subtype;  // 0x80 (V=2)
    uint8_t  pt;           // 204 (APP)
    uint16_t length;
    uint32_t ssrc;
    char     name[4];      // "MCPT"
    uint8_t  opcode;
    uint8_t  id_len;       // speaker identity 문자열 길이
    uint16_t reserved;
};
#pragma pack(pop)

enum CwrtcFloorOp {
    CWRTC_FLOOR_REQUEST = 1,
    CWRTC_FLOOR_GRANT   = 2,
    CWRTC_FLOOR_REJECT  = 3,
    CWRTC_FLOOR_RELEASE = 4,
    CWRTC_FLOOR_IDLE    = 5,
    CWRTC_FLOOR_TAKEN   = 6,
};

// 패킷에서 speaker identity 추출
static std::string ExtractSpeakerId(const char* buf, int len) {
    if (len < (int)sizeof(CwrtcFloorHdr)) return "";
    const CwrtcFloorHdr* h = (const CwrtcFloorHdr*)buf;
    int idLen = h->id_len;
    if (idLen <= 0 || (int)sizeof(CwrtcFloorHdr) + idLen > len) return "";
    return std::string(buf + sizeof(CwrtcFloorHdr), idLen);
}

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
    pollfd sttPoll[3];  // [0]=DTLS(브라우저), [1]=RTP(CMP), [2]=RTCP(CMP)

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
    TcpSetPollIn(sttPoll[2], pclsArg->m_hPbxRtcpUdp);

    CLog::Print(LOG_INFO, "RtpThread[%s] Start dtls=%d rtp=%d rtcp=%d cmp=%s:%d ws=%s:%d",
        pclsArg->m_strCallId.c_str(),
        pclsArg->m_iWebRtcUdpPort, pclsArg->m_iPbxUdpPort, pclsArg->m_iPbxRtcpPort,
        pclsArg->m_strCmpIp.c_str(), pclsArg->m_iCmpPort,
        pclsArg->m_strWsIp.c_str(), pclsArg->m_iWsPort);

    while (!pclsArg->m_bStop) {
        n = poll(sttPoll, 3, 1000);
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

        // ── CMP 오디오 RTP ──
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

        // ── CMP RTCP (Floor Control) ──
        if (sttPoll[2].revents & POLLIN) {
            char szRtcp[1024];
            char szRtcpIp[64];
            unsigned short iRtcpPort = 0;
            int iRtcpLen = sizeof(szRtcp);
            UdpRecv(pclsArg->m_hPbxRtcpUdp, szRtcp, &iRtcpLen, szRtcpIp, sizeof(szRtcpIp), &iRtcpPort);

            if (iRtcpLen >= (int)sizeof(CwrtcFloorHdr)) {
                CwrtcFloorHdr* pHdr = (CwrtcFloorHdr*)szRtcp;
                if (pHdr->pt == 204 && memcmp(pHdr->name, "MCPT", 4) == 0) {
                    uint8_t opcode = pHdr->opcode;
                    std::string strSpeaker = ExtractSpeakerId(szRtcp, iRtcpLen);

                    CLog::Print(LOG_INFO, "RtpThread[%s] Floor RTCP opcode=%d speaker=%s from CMP %s:%d",
                        pclsArg->m_strCallId.c_str(), opcode, strSpeaker.c_str(), szRtcpIp, iRtcpPort);

                    if (!pclsArg->m_strWsIp.empty() && pclsArg->m_iWsPort > 0) {
                        SimpleJson::JsonNode wsMsg;
                        wsMsg.Set("call_id", pclsArg->m_strCallId);

                        if (opcode == CWRTC_FLOOR_GRANT || opcode == CWRTC_FLOOR_TAKEN) {
                            wsMsg.Set("type", "ptt_floor");
                            wsMsg.Set("speaker", strSpeaker);
                            CLog::Print(LOG_INFO, "RtpThread[%s] → ptt_floor speaker=%s → WS[%s:%d]",
                                pclsArg->m_strCallId.c_str(), strSpeaker.c_str(),
                                pclsArg->m_strWsIp.c_str(), pclsArg->m_iWsPort);
                        } else if (opcode == CWRTC_FLOOR_IDLE) {
                            wsMsg.Set("type", "ptt_idle");
                            CLog::Print(LOG_INFO, "RtpThread[%s] → ptt_idle → WS[%s:%d]",
                                pclsArg->m_strCallId.c_str(),
                                pclsArg->m_strWsIp.c_str(), pclsArg->m_iWsPort);
                        } else if (opcode == CWRTC_FLOOR_REJECT) {
                            wsMsg.Set("type", "ptt_floor");
                            wsMsg.Set("speaker", std::string(""));
                            CLog::Print(LOG_INFO, "RtpThread[%s] → ptt_floor(reject) → WS[%s:%d]",
                                pclsArg->m_strCallId.c_str(),
                                pclsArg->m_strWsIp.c_str(), pclsArg->m_iWsPort);
                        }

                        if (wsMsg.Has("type"))
                            gclsHttpCallBack.SendText(pclsArg->m_strWsIp.c_str(),
                                                      pclsArg->m_iWsPort,
                                                      wsMsg.ToString().c_str());
                    }
                }
            }
        }
    }

    if (psttSrtpTx) srtp_dealloc(psttSrtpTx);
    if (psttSrtpRx) srtp_dealloc(psttSrtpRx);

    CLog::Print(LOG_INFO, "RtpThread[%s] Stopped", pclsArg->m_strCallId.c_str());

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    ERR_remove_thread_state(NULL);
#endif
    // pclsArg는 EventCallEnd에서 삭제 (floor control 등 외부 참조 유지)
    return 0;
}

bool StartRtpThread(CRtpThreadArg* pclsArg)
{
    return StartThread("RtpThread", RtpThread, pclsArg);
}
