#include <sys/time.h>
#include <chrono>
/*
 * Copyright (C) 2012 Yee Young Han <websearch@naver.com>
 * (http://blog.naver.com/websearch)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/** RTCP compound(RFC 3550 §6.4) 수신 통계 — SR(200)/RR(201) 의 보고 블록에서 상대가 본 우리 스트림의 fraction lost·jitter 를 기록한다.
 *  SDES/BYE/APP 등 다른 패킷은 세지 않는다(floor APP 은 별도 소켓). 상대가 RTCP 를 내지 않으면 값이 없다(-1). */
static void RtcpRecvStats(CRtpThread* pRtpThread, const unsigned char* p, int iLen) {
  while (iLen >= 8) {
    int iPt = p[1];
    int iLenWords = ((int)p[2] << 8 | p[3]) + 1;
    int iBytes = iLenWords * 4;
    if (iBytes > iLen) break;
    if (iPt == 200 || iPt == 201) {
      pRtpThread->m_iRtcpRecv.fetch_add(1, std::memory_order_relaxed);
      int iRc = p[0] & 0x1F;
      int iOff = iPt == 200 ? 28 : 8;   // SR: 헤더4 + SSRC4 + sender info 20 · RR: 헤더4 + SSRC4
      for (int k = 0; k < iRc && iOff + 24 <= iBytes; ++k, iOff += 24) {
        const unsigned char* b = p + iOff;
        pRtpThread->m_iRtcpRrBlocks.fetch_add(1, std::memory_order_relaxed);
        pRtpThread->m_iRtcpRrFractionLost.store(b[4], std::memory_order_relaxed);
        pRtpThread->m_uRtcpRrJitter.store(((unsigned)b[12] << 24) | ((unsigned)b[13] << 16) | ((unsigned)b[14] << 8) | b[15], std::memory_order_relaxed);
      }
    }
    p += iBytes;
    iLen -= iBytes;
  }
}

THREAD_API RtpThreadRecv(LPVOID lpParameter) {
  csim_dtmf::Detector clsInbandDetector;   // in-band DTMF — 이 호(수신 스레드 수명) 동안의 검출 상태
  CRtpThread *pRtpThread = (CRtpThread *)lpParameter;
  pollfd sttPoll[2];
  char szPacket[320], szPCM[320], szIp[21];
  int iPacketLen;
  unsigned short sPort;

  pRtpThread->m_bRecvThreadRun = true;

  TcpSetPollIn(sttPoll[0], pRtpThread->m_hSocket);
  const bool bRtcp = pRtpThread->m_hRtcpSocket != INVALID_SOCKET;
  if (bRtcp) TcpSetPollIn(sttPoll[1], pRtpThread->m_hRtcpSocket);   // RTCP(RTP+1) 수신 통계 — SR/RR

/*
#if !defined(WIN32) && !defined(NO_ALSA)
  int n;
  snd_pcm_t *psttSound = NULL;
  snd_pcm_hw_params_t *psttParam;
  unsigned int iValue;

  n = snd_pcm_open(&psttSound, gclsSetupFile.m_strSpeaker.c_str(),
                   SND_PCM_STREAM_PLAYBACK, 0);
  if (CheckError(n, "snd_pcm_open"))
    goto FUNC_END;

  snd_pcm_hw_params_alloca(&psttParam);

  snd_pcm_hw_params_any(psttSound, psttParam);

  n = snd_pcm_hw_params_set_access(psttSound, psttParam,
                                   SND_PCM_ACCESS_RW_INTERLEAVED);
  if (CheckError(n, "snd_pcm_hw_params_set_access"))
    goto FUNC_END;

  n = snd_pcm_hw_params_set_format(psttSound, psttParam, SND_PCM_FORMAT_S16_LE);
  if (CheckError(n, "snd_pcm_hw_params_set_access"))
    goto FUNC_END;

  n = snd_pcm_hw_params_set_channels(psttSound, psttParam, 1);
  if (CheckError(n, "snd_pcm_hw_params_set_channels"))
    goto FUNC_END;

  iValue = 8000;
  n = snd_pcm_hw_params_set_rate_near(psttSound, psttParam, &iValue, 0);
  if (CheckError(n, "snd_pcm_hw_params_set_rate_near"))
    goto FUNC_END;

  n = snd_pcm_hw_params(psttSound, psttParam);
  if (CheckError(n, "snd_pcm_hw_params"))
    goto FUNC_END;
#endif
*/

    // [RTP STATS VARS]
    time_t tLastTime = time(NULL);
    unsigned long long ullPacketCount = 0;
    unsigned long long ullByteCount = 0;
    time_t tCurrentTime;


  while (pRtpThread->m_bStopEvent == false) {
    if (poll(sttPoll, bRtcp ? 2 : 1, 200) <= 0) {
      continue;
    }
    if (bRtcp && (sttPoll[1].revents & POLLIN)) {
      char szRtcp[1500];
      int iRtcpLen = sizeof(szRtcp);
      char szRtcpIp[21];
      unsigned short sRtcpPort;
      if (UdpRecv(pRtpThread->m_hRtcpSocket, szRtcp, &iRtcpLen, szRtcpIp, sizeof(szRtcpIp), &sRtcpPort))
        RtcpRecvStats(pRtpThread, (const unsigned char*)szRtcp, iRtcpLen);
      if (!(sttPoll[0].revents & POLLIN)) continue;
    }

    iPacketLen = sizeof(szPacket);
    if (UdpRecv(pRtpThread->m_hSocket, szPacket, &iPacketLen, szIp,
                sizeof(szIp), &sPort) == false) {
      continue;
    }

    // 미디어 SRTP — 협상된 세션이면 unprotect (인증 실패/재전송 = 드롭, §8.2)
    if (pRtpThread->SrtpEnabled() && !pRtpThread->SrtpUnprotect(szPacket, iPacketLen)) {
      continue;
    }

    if (iPacketLen == 160 + sizeof(RtpHeader)) {
      UlawToPcm(szPacket + sizeof(RtpHeader), 160, szPCM, sizeof(szPCM));

/*
#if !defined(WIN32) && !defined(NO_ALSA)
      n = snd_pcm_writei(psttSound, szPCM, sizeof(szPCM) / 2);
      if (n == -EPIPE) {
        snd_pcm_prepare(psttSound);
      } else if (CheckError(n, "snd_pcm_writei"))
        break;
#endif
*/
    }

    // [RTP STATS LOGIC]
    ullPacketCount++;
    ullByteCount += iPacketLen;
    pRtpThread->m_ullRecvTotal.fetch_add(1, std::memory_order_relaxed);  // 누적(전달/픽업 미디어 검증)
    // 수신 SSRC 집합 — 감청 leg 는 한 m-line 에서 caller/callee SSRC 2개를 받는다(S3-SCN-MONITOR).
    if (iPacketLen >= (int)sizeof(RtpHeader)) {
        const RtpHeader* pHdr = (const RtpHeader*)szPacket;
        unsigned int uSsrc = ntohl(pHdr->ssrc);
        {
            std::lock_guard<std::mutex> lk(pRtpThread->m_mtxSsrc);
            if (pRtpThread->m_setRecvSsrc.size() < 16) pRtpThread->m_setRecvSsrc.insert(uSsrc);
        }
        // 손실·지터 (RFC 3550 A.1 시퀀스 확장 + A.8 지터) — 단일 스트림 기준. 다른 SSRC 가 섞이면(감청 leg)
        //   기준을 그 SSRC 로 다시 잡는다(통계는 대표 스트림 하나만 본다).
        unsigned short usSeq = ntohs(pHdr->sSeq);
        unsigned int uTs = ntohl(pHdr->iTimeStamp);
        struct timeval tvNow; gettimeofday(&tvNow, NULL);
        long long llArrivalUs = (long long)tvNow.tv_sec * 1000000LL + tvNow.tv_usec;
        int iPt = pHdr->cMpt & 0x7F;
        // RFC 4733 telephone-event — 이벤트 수(E 비트 패킷, 같은 (ts,event) 의 반복 종료 패킷은 한 번)·숫자열.
        //   지터 계산에서는 제외한다(이벤트 동안 타임스탬프가 고정이라 A.8 이 흔들린다).
        bool bTelEvent = pRtpThread->m_iDtmfPt >= 0 && iPt == pRtpThread->m_iDtmfPt;
        if (!bTelEvent) pRtpThread->m_iRecvPt.store(iPt, std::memory_order_relaxed);
        // in-band DTMF 검출(DtmfInband.h) — telephone-event 미협상 + m_bDtmfInband + G.711 20 ms 프레임
        if (!bTelEvent && pRtpThread->m_bDtmfInband && pRtpThread->m_iDtmfPt < 0 && (iPt == 0 || iPt == 8) &&
            iPacketLen == (int)sizeof(RtpHeader) + 160) {
            short pcm[160];
            if (iPt == 8) AlawToPcm(szPacket + sizeof(RtpHeader), 160, (char*)pcm, sizeof(pcm));
            else UlawToPcm(szPacket + sizeof(RtpHeader), 160, (char*)pcm, sizeof(pcm));
            char cDigit = clsInbandDetector.Feed(pcm, 160);
            if (cDigit) {
                std::lock_guard<std::mutex> lk(pRtpThread->m_mtxDtmf);
                pRtpThread->m_strDtmfRecv.push_back(cDigit);
                pRtpThread->m_iDtmfRecv++;
            }
        }
        if (bTelEvent && iPacketLen >= (int)sizeof(RtpHeader) + 4) {
            const unsigned char* pEv = (const unsigned char*)(szPacket + sizeof(RtpHeader));
            if (pEv[1] & 0x80) {
                int iEv = pEv[0];
                if (!(pRtpThread->m_uDtmfRecvLastTs == uTs && pRtpThread->m_iDtmfRecvLastEvent == iEv)) {
                    pRtpThread->m_uDtmfRecvLastTs = uTs;
                    pRtpThread->m_iDtmfRecvLastEvent = iEv;
                    static const char* kDigits = "0123456789*#ABCD";
                    std::lock_guard<std::mutex> lk(pRtpThread->m_mtxDtmf);
                    pRtpThread->m_strDtmfRecv.push_back(iEv >= 0 && iEv < 16 ? kDigits[iEv] : '?');
                    pRtpThread->m_iDtmfRecv++;
                }
            }
        }
        // RTP 클록 — 정적 협대역 PT(0/8/18)와 G.722(9 — RFC 3551 §4.5.2 는 16 kHz 표본이지만 클록을 8000 으로 표기)는 8 kHz, 그 외(AMR-WB) 16 kHz
        double dClock = (iPt == 0 || iPt == 8 || iPt == 9 || iPt == 18) ? 8000.0 : 16000.0;
        if (!pRtpThread->m_bRecvSeqInit || pRtpThread->m_uRecvSsrc != uSsrc) {
            pRtpThread->m_bRecvSeqInit = true;
            pRtpThread->m_uRecvSsrc = uSsrc;
            pRtpThread->m_uRecvExtSeq = usSeq;
            pRtpThread->m_dRecvJitter = 0;
        } else {
            unsigned short usPrev = (unsigned short)(pRtpThread->m_uRecvExtSeq & 0xFFFF);
            short sDelta = (short)(usSeq - usPrev);
            if (sDelta > 1) pRtpThread->m_ullRecvLost.fetch_add((unsigned long long)(sDelta - 1), std::memory_order_relaxed);
            if (sDelta > 0) pRtpThread->m_uRecvExtSeq += (unsigned int)sDelta;
            // A.8: D = (Rj - Ri) - (Sj - Si), J += (|D| - J) / 16 (클록 틱 단위)
            if (bTelEvent) { pRtpThread->m_uRecvLastTs = uTs; pRtpThread->m_llRecvLastArrivalUs = llArrivalUs; continue; }
            double dArrivalTicks = (double)(llArrivalUs - pRtpThread->m_llRecvLastArrivalUs) * dClock / 1000000.0;
            double dTsTicks = (double)(int)(uTs - pRtpThread->m_uRecvLastTs);
            double dD = dArrivalTicks - dTsTicks;
            if (dD < 0) dD = -dD;
            pRtpThread->m_dRecvJitter += (dD - pRtpThread->m_dRecvJitter) / 16.0;
            pRtpThread->m_llRecvJitterUs.store((long long)(pRtpThread->m_dRecvJitter * 1000000.0 / dClock),
                                               std::memory_order_relaxed);
        }
        pRtpThread->m_uRecvLastTs = uTs;
        pRtpThread->m_llRecvLastArrivalUs = llArrivalUs;
    }
    tCurrentTime = time(NULL);
    if( tCurrentTime - tLastTime >= 10 )
    {
        printf( "[RTP STATS] Time: %lld, Packets: %llu, Bytes: %llu\n", (long long)tCurrentTime, ullPacketCount, ullByteCount );
        tLastTime = tCurrentTime;
        ullPacketCount = 0;
        ullByteCount = 0;
    }

  }


/*
#if !defined(WIN32) && !defined(NO_ALSA)
FUNC_END:
  if (psttSound) {
    snd_pcm_drain(psttSound);
    snd_pcm_close(psttSound);
  }
#endif
*/

  pRtpThread->m_bRecvThreadRun = false;

  return 0;
}

// Floor control 수신 스레드: m=application 소켓에서 RTCP APP 패킷 수신 후 opcode 파싱
THREAD_API RtpThreadFloorRecv(LPVOID lpParameter) {
  CRtpThread *pRtpThread = (CRtpThread *)lpParameter;

  pRtpThread->m_bFloorRecvThreadRun = true;

  pollfd sttPoll[1];
  TcpSetPollIn(sttPoll[0], pRtpThread->m_hFloorRecvSocket);

  char buf[512];
  char szIp[21];
  unsigned short sPort;

  while (pRtpThread->m_bStopEvent == false) {
    if (poll(sttPoll, 1, 200) <= 0) {
      continue;
    }

    int iLen = sizeof(buf);
    if (UdpRecv(pRtpThread->m_hFloorRecvSocket, buf, &iLen, szIp, sizeof(szIp), &sPort) == false) {
      continue;
    }

    // RTCP APP 최소 크기: 12바이트 (헤더4 + SSRC4 + Name4). TLV 본문은 선택.
    if (iLen < 12) continue;

    // PT = buf[1] == 204 (RTCP APP)
    unsigned char pt = (unsigned char)buf[1];
    if (pt != 204) continue;

    // name 필드: buf[8..11] = "MCPT"
    if (buf[8] != 'M' || buf[9] != 'C' || buf[10] != 'P' || buf[11] != 'T') continue;

    // TS 24.380 §8.2: 메시지 타입 = 5비트 subtype.
    unsigned char opcode = (unsigned char)buf[0] & 0x1F;
    long long tUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    pRtpThread->m_iLastFloorOp.store(opcode);
    if (opcode == 1) pRtpThread->m_bGrantReceived.store(true);  // GRANTED(subtype=1, TS 24.380) — TAKEN이 즉시 덮어써도 보존
    if (opcode == 2) pRtpThread->m_iFloorTakenCount++;
    if (opcode == 3) pRtpThread->m_iFloorDenyCount++;
    if (pRtpThread->m_pFloorSink) pRtpThread->m_pFloorSink->OnFloorMessage(opcode, tUs);

    const char* opName = "UNKNOWN";
    switch (opcode) {
      case 0:  opName = "REQUEST";        break;
      case 1:  opName = "GRANTED";        break;
      case 2:  opName = "TAKEN";          break;
      case 3:  opName = "DENY";           break;
      case 4:  opName = "RELEASE";        break;
      case 5:  opName = "IDLE";           break;
      case 6:  opName = "REVOKE";         break;
      case 9:  opName = "QUEUE_POS_INFO"; break;
      case 10: opName = "ACK";            break;
    }
    printf("[FLOOR] Received opcode=%d (%s) from %s:%d\n", opcode, opName, szIp, sPort);
  }

  pRtpThread->m_bFloorRecvThreadRun = false;
  return 0;
}
