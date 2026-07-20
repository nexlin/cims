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

          // Frame data (원본 프레임의 첫 바이트는 ToC이므로 건너뜀)
          memcpy(payload + payloadLen, vecFrames[iFrameIdx].data() + 1, vecFrames[iFrameIdx].size() - 1);
          payloadLen += vecFrames[iFrameIdx].size() - 1;

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

/**
 * @brief H.264 Annex B 파일에서 NAL 유닛을 파싱하여 로드한다.
 * @param strPath  H.264 Annex B raw 파일 경로 (0x00000001 startcode)
 * @param vecNals  (출력) NAL 유닛 데이터 벡터 (startcode 제외)
 * @return 성공 시 true
 */
static bool LoadH264Nals(const std::string& strPath,
                         std::vector<std::vector<uint8_t>>& vecNals) {
    FILE* fp = fopen(strPath.c_str(), "rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long lFileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (lFileSize <= 4) { fclose(fp); return false; }

    std::vector<uint8_t> buf(lFileSize);
    if ((long)fread(buf.data(), 1, lFileSize, fp) != lFileSize) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    // Find NAL units separated by 0x00000001
    std::vector<long> offsets;
    for (long i = 0; i + 3 < lFileSize; ++i) {
        if (buf[i] == 0x00 && buf[i+1] == 0x00 && buf[i+2] == 0x00 && buf[i+3] == 0x01) {
            offsets.push_back(i);
        }
    }

    for (size_t i = 0; i < offsets.size(); ++i) {
        long nalStart = offsets[i] + 4; // skip startcode
        long nalEnd = (i + 1 < offsets.size()) ? offsets[i + 1] : lFileSize;
        if (nalEnd > nalStart) {
            vecNals.push_back(std::vector<uint8_t>(buf.begin() + nalStart, buf.begin() + nalEnd));
        }
    }

    return !vecNals.empty();
}

THREAD_API RtpThreadVideoSend(LPVOID lpParameter) {
    CRtpThread *pRtpThread = (CRtpThread *)lpParameter;

    pRtpThread->m_bVideoSendThreadRun = true;

    // Load H.264 NAL units from file
    std::vector<std::vector<uint8_t>> vecNals;
    if (!LoadH264Nals(pRtpThread->m_strVideoFile, vecNals)) {
        printf("[VIDEO-RTP] Failed to load H.264 file: %s\n",
               pRtpThread->m_strVideoFile.c_str());
        pRtpThread->m_bVideoSendThreadRun = false;
        return 0;
    }
    printf("[VIDEO-RTP] Loaded %d NAL units from %s\n",
           (int)vecNals.size(), pRtpThread->m_strVideoFile.c_str());

    uint16_t sSeq = 0;
    uint32_t iTimeStamp = 0;
    uint32_t iSsrc = htonl(300);
    int iNalIdx = 0;
    int iTotalNals = (int)vecNals.size();
    const int MAX_RTP_PAYLOAD = 1200;
    // 협상된 video 포트가 없으면 송신하지 않는다 — 구 "audio+2" 관례는 leg 별 포트셋에서
    // 이웃 leg 의 audio 포트를 침범한다 (NAT 모드에선 오-latch 유발).
    if (pRtpThread->m_iDestVideoPort <= 0) {
        printf("[VIDEO-RTP] No negotiated video port — video send disabled\n");
        pRtpThread->m_bVideoSendThreadRun = false;
        return 0;
    }
    int iVideoDestPort = pRtpThread->m_iDestVideoPort;

    while (pRtpThread->m_bStopEvent == false) {
        const std::vector<uint8_t>& nal = vecNals[iNalIdx];
        int iNalSize = (int)nal.size();

        if (iNalSize <= MAX_RTP_PAYLOAD) {
            // Single NAL unit packet - fits in one RTP packet
            char szPacket[1500];
            RtpHeader *psttRtpHeader = (RtpHeader *)szPacket;

            psttRtpHeader->SetVersion(2);
            psttRtpHeader->SetPadding(0);
            psttRtpHeader->SetExtension(0);
            psttRtpHeader->SetCC(0);
            psttRtpHeader->SetMarker(1); // last (only) packet of this NAL = access unit end
            psttRtpHeader->SetPT(96);
            psttRtpHeader->SetSeq(sSeq++);
            psttRtpHeader->SetTimeStamp(iTimeStamp);
            psttRtpHeader->ssrc = iSsrc;

            memcpy(szPacket + sizeof(RtpHeader), nal.data(), iNalSize);

            UdpSend(pRtpThread->m_hVideoSocket, szPacket,
                    (int)sizeof(RtpHeader) + iNalSize,
                    pRtpThread->m_strDestIp.c_str(), iVideoDestPort);
        } else {
            // FU-A fragmentation (RFC 6184)
            uint8_t nalHeader = nal[0];
            uint8_t nalType = nalHeader & 0x1F;
            uint8_t nri = nalHeader & 0x60;
            // FU indicator: same NRI, type=28 (FU-A)
            uint8_t fuIndicator = (nri | 28);

            int iOffset = 1; // skip NAL header byte (included in FU header)
            int iRemaining = iNalSize - 1;
            bool bFirst = true;

            while (iRemaining > 0 && pRtpThread->m_bStopEvent == false) {
                int iChunk = (iRemaining > MAX_RTP_PAYLOAD - 2) ? (MAX_RTP_PAYLOAD - 2) : iRemaining;
                bool bLast = (iRemaining - iChunk <= 0);

                char szPacket[1500];
                RtpHeader *psttRtpHeader = (RtpHeader *)szPacket;

                psttRtpHeader->SetVersion(2);
                psttRtpHeader->SetPadding(0);
                psttRtpHeader->SetExtension(0);
                psttRtpHeader->SetCC(0);
                psttRtpHeader->SetMarker(bLast ? 1 : 0);
                psttRtpHeader->SetPT(96);
                psttRtpHeader->SetSeq(sSeq++);
                psttRtpHeader->SetTimeStamp(iTimeStamp);
                psttRtpHeader->ssrc = iSsrc;

                char* payload = szPacket + sizeof(RtpHeader);
                // FU indicator
                payload[0] = (char)fuIndicator;
                // FU header: S=start, E=end, R=0, Type=nalType
                uint8_t fuHeader = nalType;
                if (bFirst) fuHeader |= 0x80; // S bit
                if (bLast)  fuHeader |= 0x40; // E bit
                payload[1] = (char)fuHeader;

                memcpy(payload + 2, nal.data() + iOffset, iChunk);

                UdpSend(pRtpThread->m_hVideoSocket, szPacket,
                        (int)sizeof(RtpHeader) + 2 + iChunk,
                        pRtpThread->m_strDestIp.c_str(), iVideoDestPort);

                iOffset += iChunk;
                iRemaining -= iChunk;
                bFirst = false;

                // FU-A 패킷 간 대기 (UDP 수신 버퍼 오버플로우 방지)
                if (iRemaining > 0) usleep(500);
            }
        }

        // Advance to next NAL
        ++iNalIdx;
        if (iNalIdx >= iTotalNals) iNalIdx = 0; // loop

        // 프레임 NAL(IDR=5, non-IDR=1)에서만 timestamp 증가 + sleep
        // SPS(7)/PPS(8)/SEI(6)는 비프레임이므로 즉시 전송
        uint8_t curNalType = nal[0] & 0x1F;
        if (curNalType == 1 || curNalType == 5) {
            // 프레임레이트: NAL 파일의 프레임 수에서 자동 계산
            // 기본 ts_inc = 90000/15 = 6000 (15fps)
            static int s_iFrameTsInc = 0;
            static int s_iFrameSleepMs = 0;
            if (s_iFrameTsInc == 0) {
                // 프레임 NAL 수 카운트
                int iFrameCount = 0;
                for (const auto& n : vecNals) {
                    uint8_t t = n[0] & 0x1F;
                    if (t == 1 || t == 5) iFrameCount++;
                }
                // 원본 콘텐츠 길이 (프레임 수 * 기본 간격)에서 역산
                // 원본 15fps 기준: ts_inc = 6000
                s_iFrameTsInc = 6000;
                s_iFrameSleepMs = 67;
                if (iFrameCount > 0) {
                    // 첫 루프 기준으로 실제 fps 추정
                    double fps = (double)iFrameCount / ((double)iFrameCount * 6000.0 / 90000.0);
                    s_iFrameSleepMs = (int)(1000.0 / fps);
                }
                printf("[VIDEO-RTP] Frame NALs=%d, ts_inc=%d, sleep=%dms\n",
                       iFrameCount, s_iFrameTsInc, s_iFrameSleepMs);
            }
            iTimeStamp += s_iFrameTsInc;
            MiliSleep(s_iFrameSleepMs);
        }
    }

    pRtpThread->m_bVideoSendThreadRun = false;

    return 0;
}
