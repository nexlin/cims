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

THREAD_API RtpThreadRecv(LPVOID lpParameter) {
  CRtpThread *pRtpThread = (CRtpThread *)lpParameter;
  pollfd sttPoll[1];
  char szPacket[320], szPCM[320], szIp[21];
  int iPacketLen;
  unsigned short sPort;

  pRtpThread->m_bRecvThreadRun = true;

  TcpSetPollIn(sttPoll[0], pRtpThread->m_hSocket);

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
    if (poll(sttPoll, 1, 200) <= 0) {
      continue;
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
        unsigned int uSsrc = ntohl(((const RtpHeader*)szPacket)->ssrc);
        std::lock_guard<std::mutex> lk(pRtpThread->m_mtxSsrc);
        if (pRtpThread->m_setRecvSsrc.size() < 16) pRtpThread->m_setRecvSsrc.insert(uSsrc);
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
    pRtpThread->m_iLastFloorOp.store(opcode);
    if (opcode == 1) pRtpThread->m_bGrantReceived.store(true);  // GRANTED(subtype=1, TS 24.380) — TAKEN이 즉시 덮어써도 보존
    if (opcode == 2) pRtpThread->m_iFloorTakenCount++;
    if (opcode == 3) pRtpThread->m_iFloorDenyCount++;

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
