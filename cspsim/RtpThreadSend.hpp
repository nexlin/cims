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

/**
 * @brief AMR-WB raw 프레임 파일에서 프레임 데이터를 로드한다.
 * @param strPath  AMR-WB raw 프레임 파일 경로
 * @param vecFrames  (출력) 프레임 데이터 벡터
 * @param iFrameSize (출력) 프레임 크기 (고정 61 바이트)
 * @return 성공 시 true
 *
 * AMR-WB raw 프레임 파일은 61 바이트 프레임이 연속 저장된 형태.
 * (3GP extract_frames.py로 추출)
 */
static bool LoadAmrWbFrames(const std::string& strPath,
                            std::vector<std::vector<char>>& vecFrames, int& iFrameSize) {
    FILE* fp = fopen(strPath.c_str(), "rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long lFileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // AMR-WB 23.85 kbps: 61 bytes/frame
    iFrameSize = 61;
    int iFrameCount = (int)(lFileSize / iFrameSize);

    vecFrames.resize(iFrameCount);
    for (int i = 0; i < iFrameCount; ++i) {
        vecFrames[i].resize(iFrameSize);
        if (fread(vecFrames[i].data(), 1, iFrameSize, fp) != (size_t)iFrameSize) {
            vecFrames.resize(i);
            break;
        }
    }

    fclose(fp);
    return !vecFrames.empty();
}


THREAD_API RtpThreadSend(LPVOID lpParameter) {
  CRtpThread *pRtpThread = (CRtpThread *)lpParameter;
  char szPacket[1500];
  RtpHeader *psttRtpHeader = (RtpHeader *)szPacket;
  uint16_t sSeq = 0;
  uint32_t iTimeStamp = 0;

  pRtpThread->m_bSendThreadRun = true;

  psttRtpHeader->SetVersion(2);
  psttRtpHeader->SetPadding(0);
  psttRtpHeader->SetExtension(0);
  psttRtpHeader->SetCC(0);
  psttRtpHeader->SetMarker(0);
  psttRtpHeader->ssrc = htonl(200);

  // 미디어 파일이 설정된 경우 AMR-WB(PT=99) 전송, 아니면 합성 PCMU(PT=0)
  std::vector<std::vector<char>> vecFrames;
  int iFrameSize = 0;
  bool bFileMedia = false;

  if (!pRtpThread->m_strMediaFile.empty()) {
      bFileMedia = LoadAmrWbFrames(pRtpThread->m_strMediaFile, vecFrames, iFrameSize);
      if (bFileMedia) {
          printf("[RTP] Loaded %d AMR-WB frames from %s\n",
                 (int)vecFrames.size(), pRtpThread->m_strMediaFile.c_str());
      } else {
          printf("[RTP] Failed to load media file: %s (falling back to synthetic)\n",
                 pRtpThread->m_strMediaFile.c_str());
      }
  }

  if (bFileMedia) {
      // ── AMR-WB 파일 기반 전송 (PT=99, 16kHz, 20ms/frame) ──
      psttRtpHeader->SetPT(99);
      int iFrameIdx = 0;
      int iTotalFrames = (int)vecFrames.size();

      while (pRtpThread->m_bStopEvent == false) {
          // AMR-WB RTP: RFC 4867 octet-aligned
          // payload = [CMR(4bit)+0000(4bit)] + [ToC(8bit)] + [frame data]
          char* payload = szPacket + sizeof(RtpHeader);
          int payloadLen = 0;

          // CMR (Codec Mode Request) = 0x80 (mode 8 = 23.85kbps, 4-bit MSB + 4-bit pad)
          payload[0] = (char)0x80;
          payloadLen = 1;

          // ToC (Table of Contents): F=0 (last frame), FT=8 (23.85kbps), Q=1 (good)
          // FT=8 → 01000 | Q=1 → 010001 | F=0 → 0 | pad=0 → 0100 0100 = 0x44
          payload[1] = 0x44;
          payloadLen = 2;

          // Frame data
          memcpy(payload + payloadLen, vecFrames[iFrameIdx].data(), vecFrames[iFrameIdx].size());
          payloadLen += vecFrames[iFrameIdx].size();

          psttRtpHeader->SetSeq(sSeq);
          psttRtpHeader->SetTimeStamp(iTimeStamp);
          psttRtpHeader->SetMarker(iFrameIdx == 0 ? 1 : 0);

          ++sSeq;
          iTimeStamp += 320;  // 20ms @ 16kHz

          UdpSend(pRtpThread->m_hSocket, szPacket, (int)sizeof(RtpHeader) + payloadLen,
                  pRtpThread->m_strDestIp.c_str(), pRtpThread->m_iDestPort);

          ++iFrameIdx;
          if (iFrameIdx >= iTotalFrames) iFrameIdx = 0;  // 루프 재생

          MiliSleep(20);
      }
  } else {
      // ── 합성 RTP (기존 PCMU PT=0) ──
      char szRead[320];
      psttRtpHeader->SetPT(0);

      while (pRtpThread->m_bStopEvent == false) {
          memset(szRead, 0x12, sizeof(szRead));
          MiliSleep(20);

          psttRtpHeader->SetSeq(sSeq);
          psttRtpHeader->SetTimeStamp(iTimeStamp);

          ++sSeq;
          iTimeStamp += 160;

          PcmToUlaw(szRead, 320, szPacket + sizeof(RtpHeader), 160);

          UdpSend(pRtpThread->m_hSocket, szPacket, 160 + (int)sizeof(RtpHeader),
                  pRtpThread->m_strDestIp.c_str(), pRtpThread->m_iDestPort);
      }
  }

  pRtpThread->m_bSendThreadRun = false;

  return 0;
}
