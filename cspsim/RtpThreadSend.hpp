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


/** RFC 4733 송신 상태 — 20 ms 틱마다 DtmfTick 이 본다. 이벤트 중이면 오디오 대신 이벤트 패킷을 낸다. in-band 모드(DtmfInbandTick)도 같은 상태를 쓴다. */
struct DtmfTxState {
    bool active = false;
    int event = 0;          // 0-9, 10='*', 11='#', 12-15='A'-'D'
    char digit = 0;         // in-band — 톤을 내는 숫자 문자
    int elapsedMs = 0;
    int endSent = 0;
    int gapLeftMs = 0;
    uint32_t ts = 0;        // 이벤트 시작 타임스탬프(이벤트 동안 고정)
    csim_dtmf::ToneGen tone;
};

/** in-band DTMF 틱 — telephone-event 미협상 + m_bDtmfInband + G.711 이면 이 틱의 오디오 페이로드(160 B)를 톤(또는 간격 무음)으로 채운다.
 *  true = payload 를 채웠다(호출자는 원천 프레임 대신 이것을 보낸다 — 시퀀스·타임스탬프는 오디오처럼 흐른다). 톤이 끝나면 m_iDtmfSent++. */
static bool DtmfInbandTick(CRtpThread* pRtpThread, DtmfTxState& st, char* payload, int iPt) {
    if (pRtpThread->m_iDtmfPt >= 0 || !pRtpThread->m_bDtmfInband || !(iPt == 0 || iPt == 8)) return false;
    short pcm[160];
    if (st.gapLeftMs > 0) {
        // 숫자 사이 간격 — 무음(검출기가 톤 끝을 확정하는 구간)
        st.gapLeftMs -= 20;
        memset(pcm, 0, sizeof(pcm));
    } else {
        if (!st.active) {
            char c = 0;
            {
                std::lock_guard<std::mutex> lk(pRtpThread->m_mtxDtmf);
                if (pRtpThread->m_dtmfQueue.empty()) return false;
                c = pRtpThread->m_dtmfQueue.front();
                pRtpThread->m_dtmfQueue.pop_front();
            }
            if (!st.tone.Set(c)) return false;
            st.active = true; st.digit = c; st.elapsedMs = 0;
        }
        st.tone.Fill(pcm, 160);
        st.elapsedMs += 20;
        if (st.elapsedMs >= pRtpThread->m_iDtmfDurationMs) {
            st.active = false;
            st.gapLeftMs = pRtpThread->m_iDtmfGapMs > 0 ? pRtpThread->m_iDtmfGapMs : 40;   // Q.24 최소 간격
            pRtpThread->m_iDtmfSent++;
        }
    }
    if (iPt == 8) PcmToAlaw((const char*)pcm, 320, payload, 160);
    else PcmToUlaw((const char*)pcm, 320, payload, 160);
    return true;
}

static int DtmfEventCode(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == '*') return 10;
    if (c == '#') return 11;
    if (c >= 'A' && c <= 'D') return 12 + (c - 'A');
    if (c >= 'a' && c <= 'd') return 12 + (c - 'a');
    return -1;
}

/** 20 ms 틱 — 이벤트 패킷을 냈으면 true(호출자는 이 틱의 오디오를 건너뛴다). iAudioTs = 이 틱의 오디오 타임스탬프(이벤트 시작값).
 *  RFC 4733 §2.5: 같은 SSRC/시퀀스 공간, 이벤트 동안 타임스탬프 고정, 마커는 첫 패킷, duration 누적, 종료는 E 비트 패킷 3회. */
static bool DtmfTick(CRtpThread* pRtpThread, DtmfTxState& st, RtpHeader* pHdr, char* szPacket, uint16_t& sSeq, uint32_t iAudioTs,
                     int iAudioPt) {
    if (pRtpThread->m_iDtmfPt < 0) return false;
    if (st.gapLeftMs > 0) { st.gapLeftMs -= 20; return false; }
    if (!st.active) {
        char c = 0;
        {
            std::lock_guard<std::mutex> lk(pRtpThread->m_mtxDtmf);
            if (pRtpThread->m_dtmfQueue.empty()) return false;
            c = pRtpThread->m_dtmfQueue.front();
            pRtpThread->m_dtmfQueue.pop_front();
        }
        int ev = DtmfEventCode(c);
        if (ev < 0) return false;
        st.active = true; st.event = ev; st.elapsedMs = 0; st.endSent = 0; st.ts = iAudioTs;
    }
    bool bFirst = st.elapsedMs == 0;
    bool bEnd = st.elapsedMs >= pRtpThread->m_iDtmfDurationMs;
    int iDurTicks = (bEnd ? pRtpThread->m_iDtmfDurationMs : st.elapsedMs + 20) * pRtpThread->m_iDtmfClock / 1000;
    if (iDurTicks > 0xFFFF) iDurTicks = 0xFFFF;
    unsigned char* p = (unsigned char*)(szPacket + sizeof(RtpHeader));
    p[0] = (unsigned char)st.event;
    p[1] = (unsigned char)((bEnd ? 0x80 : 0x00) | 10);   // E|R|volume(10 dBm0 감쇠)
    p[2] = (unsigned char)(iDurTicks >> 8);
    p[3] = (unsigned char)(iDurTicks & 0xFF);
    pHdr->SetPT((uint8_t)pRtpThread->m_iDtmfPt);
    pHdr->SetMarker(bFirst ? 1 : 0);
    pHdr->SetSeq(sSeq);
    pHdr->SetTimeStamp(st.ts);
    ++sSeq;
    int iSendLen = (int)sizeof(RtpHeader) + 4;
    if (!pRtpThread->SrtpEnabled() || pRtpThread->SrtpProtect(szPacket, iSendLen, 1500))
        UdpSend(pRtpThread->m_hSocket, szPacket, iSendLen, pRtpThread->m_strDestIp.c_str(), pRtpThread->m_iDestPort);
    pHdr->SetPT((uint8_t)iAudioPt);
    pHdr->SetMarker(0);
    if (bEnd) {
        if (++st.endSent >= 3) { st.active = false; st.gapLeftMs = pRtpThread->m_iDtmfGapMs; pRtpThread->m_iDtmfSent++; }
    } else {
        st.elapsedMs += 20;
    }
    return true;
}

/** 고정 크기 프레임 파일 공유 캐시 — 부하 시험에서 호마다 같은 파일을 다시 읽지 않는다(프로세스 수명, 경로+프레임 크기 키).
 *  AMR-WB raw = 61 B/프레임, G.711 raw = 160 B(20 ms @ 8 kHz, 끝의 짧은 조각은 버린다). */
typedef std::vector<std::vector<char>> RtpFrameVec;
static std::shared_ptr<const RtpFrameVec> LoadFramesCached(const std::string& strPath, int iFrameSize) {
    static std::mutex s_mtx;
    static std::map<std::string, std::shared_ptr<const RtpFrameVec>> s_cache;
    std::string strKey = strPath + "#" + std::to_string(iFrameSize);
    std::lock_guard<std::mutex> lk(s_mtx);
    auto it = s_cache.find(strKey);
    if (it != s_cache.end()) return it->second;
    std::shared_ptr<RtpFrameVec> pFrames = std::make_shared<RtpFrameVec>();
    if (iFrameSize == 61) {
        int iSize = 0;
        if (!LoadAmrWbFrames(strPath, *pFrames, iSize)) return nullptr;
    } else {
        FILE* fp = fopen(strPath.c_str(), "rb");
        if (!fp) return nullptr;
        std::vector<char> vecBuf(iFrameSize);
        while (fread(vecBuf.data(), 1, iFrameSize, fp) == (size_t)iFrameSize) pFrames->push_back(vecBuf);
        fclose(fp);
        if (pFrames->empty()) return nullptr;
    }
    printf("[RTP] Loaded %d frames(%d B) from %s\n", (int)pFrames->size(), iFrameSize, strPath.c_str());
    s_cache[strKey] = pFrames;
    return pFrames;
}

/** 송신 원천 — 합의 코덱과 (기본|MediaSend 지정) 파일로 정한다. 파일이 없거나 못 읽으면 그 코덱의 합성.
 *  G.722(PT 9, RFC 3551 §4.5.2 — RTP 클록 8000 표기·64 kbit/s = 160 B/20 ms)는 G.711 과 같은 프레임 크기의 파일 또는 합성(상수 코드워드). */
struct RtpTxSource {
    enum Kind { SYNTH_G711, SYNTH_AMRWB, FILE_AMRWB, FILE_G711, SYNTH_G722, FILE_G722 } kind = SYNTH_G711;
    std::shared_ptr<const RtpFrameVec> frames;
    std::string path;
    int pt = 0;
    uint32_t tsStep = 160;
    bool loop = true;
    size_t idx = 0;
};

static void ResolveTxSource(CRtpThread* pRtpThread, RtpTxSource& src) {
    bool bOverride; bool bLoop; std::string strAmrWb, strPcmu, strPcma, strG722;
    {
        std::lock_guard<std::mutex> lk(pRtpThread->m_mtxSource);
        bOverride = pRtpThread->m_bSourceOverride;
        strAmrWb = pRtpThread->m_strSrcAmrWb; strPcmu = pRtpThread->m_strSrcPcmu; strPcma = pRtpThread->m_strSrcPcma; strG722 = pRtpThread->m_strSrcG722;
        bLoop = pRtpThread->m_bSrcLoop;
    }
    int iPt = pRtpThread->m_iAudioPt;
    bool bG711 = (iPt == 0 || iPt == 8);
    bool bG722 = (iPt == 9);
    bool bAmrWb = !bG711 && !bG722 && pRtpThread->m_bUseMediaFile;   // m_bUseMediaFile = AMR-WB 합의
    RtpTxSource clsNew;
    clsNew.loop = bLoop;
    if (bG722) {
        // G.722 — 파일(MediaSend 지정) 또는 합성. 기본 원천(AMR-WB 파일)은 이 코덱에 쓸 수 없다
        clsNew.kind = RtpTxSource::SYNTH_G722; clsNew.pt = 9; clsNew.tsStep = 160; clsNew.path = bOverride ? strG722 : "";
        if (!clsNew.path.empty() && (clsNew.frames = LoadFramesCached(clsNew.path, 160))) clsNew.kind = RtpTxSource::FILE_G722;
        else if (!clsNew.path.empty()) printf("[RTP] Failed to load media sample: %s (falling back to synthetic)\n", clsNew.path.c_str());
    } else if (bOverride) {
        if (bAmrWb) {
            clsNew.path = strAmrWb; clsNew.kind = RtpTxSource::SYNTH_AMRWB; clsNew.pt = iPt >= 0 ? iPt : 99; clsNew.tsStep = 320;
            if (!strAmrWb.empty() && (clsNew.frames = LoadFramesCached(strAmrWb, 61))) clsNew.kind = RtpTxSource::FILE_AMRWB;
        } else {
            const std::string& strFile = (iPt == 8) ? strPcma : strPcmu;
            clsNew.path = strFile; clsNew.kind = RtpTxSource::SYNTH_G711; clsNew.pt = (iPt == 8) ? 8 : 0; clsNew.tsStep = 160;
            if (bG711 && !strFile.empty() && (clsNew.frames = LoadFramesCached(strFile, 160))) clsNew.kind = RtpTxSource::FILE_G711;
        }
        if (!clsNew.path.empty() && !clsNew.frames)
            printf("[RTP] Failed to load media sample: %s (falling back to synthetic)\n", clsNew.path.c_str());
    } else if (!pRtpThread->m_strMediaFile.empty() && pRtpThread->m_bUseMediaFile) {
        // 기본 원천 — AMR-WB 파일(PT = SDP 협상값, 미협상 시 레거시 99)
        clsNew.path = pRtpThread->m_strMediaFile; clsNew.pt = iPt >= 0 ? iPt : 99; clsNew.tsStep = 320;
        if ((clsNew.frames = LoadFramesCached(clsNew.path, 61))) clsNew.kind = RtpTxSource::FILE_AMRWB;
        else {
            printf("[RTP] Failed to load media file: %s (falling back to synthetic)\n", clsNew.path.c_str());
            clsNew.kind = RtpTxSource::SYNTH_G711; clsNew.pt = 0; clsNew.tsStep = 160;
        }
    } else if (bAmrWb && iPt > 0) {
        // 기본 합성 — AMR-WB 합의인데 파일이 없다: 협상 PT 의 NO_DATA 프레임(세션 코덱과 다른 PCMU PT 0 을 보내지 않는다)
        clsNew.kind = RtpTxSource::SYNTH_AMRWB; clsNew.pt = iPt; clsNew.tsStep = 320;
    } else {
        // 기본 합성 — PCMU(PT 0). PCMA 합의면 PT 8(페이로드는 그대로 — 계측기는 흐름·손실만 본다)
        clsNew.kind = RtpTxSource::SYNTH_G711; clsNew.pt = (iPt == 8) ? 8 : 0; clsNew.tsStep = 160;
    }
    // 같은 원천을 다시 고른 것이면(183 → 200 재시작 등) 재생 위치를 잇는다
    if (clsNew.kind == src.kind && clsNew.path == src.path && clsNew.frames == src.frames) clsNew.idx = src.idx;
    src = clsNew;
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

  DtmfTxState sttDtmf;
  RtpTxSource clsSrc;
  int iGen = -1;
  bool bTalkStart = true;   // 발화 시작(첫 패킷·정지 뒤 재개·원천 교체) — 마커 비트(RFC 3551 §4.1)
  char szPcm[320];

  while (pRtpThread->m_bStopEvent == false) {
      MiliSleep(20);

      // 정지 플래그를 먼저, 원천 세대를 나중에 읽는다 — MediaSend 는 세대++ 뒤에 정지를 푼다. 순서가 반대면 정지가 풀린 것만 보고
      //   이전 원천으로 한 패킷을 흘린다.
      bool bPaused = pRtpThread->m_bSendPaused.load() || pRtpThread->m_bHoldPaused.load();
      int iCurGen = pRtpThread->m_iSourceGen.load();
      if (iCurGen != iGen) {
          iGen = iCurGen;
          size_t iPrevIdx = clsSrc.idx;
          ResolveTxSource(pRtpThread, clsSrc);
          if (clsSrc.idx != iPrevIdx || clsSrc.idx == 0) bTalkStart = true;
          psttRtpHeader->SetPT((uint8_t)clsSrc.pt);
      }

      if (DtmfTick(pRtpThread, sttDtmf, psttRtpHeader, szPacket, sSeq, iTimeStamp, clsSrc.pt)) {
          iTimeStamp += clsSrc.tsStep;
          continue;
      }
      if (bPaused) {
          // 정지 — 타임스탬프만 흐른다(시퀀스는 그대로라 수신 측 손실 계산에 공백이 없다)
          iTimeStamp += clsSrc.tsStep;
          bTalkStart = true;
          continue;
      }

      char* payload = szPacket + sizeof(RtpHeader);
      int payloadLen = 0;
      bool bFile = (clsSrc.kind == RtpTxSource::FILE_AMRWB || clsSrc.kind == RtpTxSource::FILE_G711 || clsSrc.kind == RtpTxSource::FILE_G722);
      if (DtmfInbandTick(pRtpThread, sttDtmf, payload, clsSrc.pt)) {
          // in-band DTMF — 이 틱은 톤(또는 간격 무음) 160 B. 파일 원천의 재생 위치는 그대로(톤 뒤 이어서 재생)
          payloadLen = 160;
          bFile = false;
      } else if (clsSrc.kind == RtpTxSource::FILE_AMRWB) {
          // AMR-WB RTP: RFC 4867 octet-aligned — [CMR(4bit)+0000] + [ToC] + [frame data]
          //   CMR = 0x80 (mode 8 = 23.85 kbps) · ToC = F=0, FT=8, Q=1 → 0x44. 원본 프레임의 첫 바이트는 ToC 라 건너뛴다.
          const std::vector<char>& vecFrame = (*clsSrc.frames)[clsSrc.idx];
          payload[0] = (char)0x80;
          payload[1] = 0x44;
          memcpy(payload + 2, vecFrame.data() + 1, vecFrame.size() - 1);
          payloadLen = 2 + (int)vecFrame.size() - 1;
      } else if (clsSrc.kind == RtpTxSource::FILE_G711 || clsSrc.kind == RtpTxSource::FILE_G722) {
          const std::vector<char>& vecFrame = (*clsSrc.frames)[clsSrc.idx];
          memcpy(payload, vecFrame.data(), vecFrame.size());
          payloadLen = (int)vecFrame.size();
      } else if (clsSrc.kind == RtpTxSource::SYNTH_G722) {
          // 합성 G.722 — 상수 코드워드 160 B(64 kbit/s). 흐름·손실·지터 계측용(디코더 무음 근사)
          memset(payload, 0x55, 160);
          payloadLen = 160;
      } else if (clsSrc.kind == RtpTxSource::SYNTH_AMRWB) {
          // 합성 AMR-WB — NO_DATA 프레임(FT=15, Q=1): CMR 15(요청 없음) + ToC 0x7C. 흐름·손실·지터 계측용
          payload[0] = (char)0xF0;
          payload[1] = 0x7C;
          payloadLen = 2;
      } else {
          memset(szPcm, 0x12, sizeof(szPcm));
          PcmToUlaw(szPcm, 320, payload, 160);
          payloadLen = 160;
      }

      psttRtpHeader->SetSeq(sSeq);
      psttRtpHeader->SetTimeStamp(iTimeStamp);
      psttRtpHeader->SetMarker((bTalkStart || (bFile && clsSrc.idx == 0)) ? 1 : 0);
      bTalkStart = false;
      ++sSeq;
      iTimeStamp += clsSrc.tsStep;

      {
          // 미디어 SRTP — 협상된 세션이면 protect 후 송신 (media_security.md §8.2)
          int iSendLen = (int)sizeof(RtpHeader) + payloadLen;
          if (!pRtpThread->SrtpEnabled() ||
              pRtpThread->SrtpProtect(szPacket, iSendLen, (int)sizeof(szPacket)))
              UdpSend(pRtpThread->m_hSocket, szPacket, iSendLen,
                      pRtpThread->m_strDestIp.c_str(), pRtpThread->m_iDestPort);
          pRtpThread->m_ullSentTotal++;
      }

      if (bFile && ++clsSrc.idx >= clsSrc.frames->size()) {
          clsSrc.idx = 0;
          if (!clsSrc.loop) {   // 한 번 재생 — 끝에서 멈춘다(MediaSend 가 다시 부를 때까지)
              pRtpThread->m_bSendPaused = true;
              pRtpThread->m_bSourceEnded = true;
          }
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

            // 미디어 SRTP — 비디오 m-line 협상 세션이면 protect 후 송신 (media_security.md §8.2)
            int iSendLen = (int)sizeof(RtpHeader) + iNalSize;
            if (!pRtpThread->VideoSrtpEnabled() ||
                pRtpThread->SrtpVideoProtect(szPacket, iSendLen, (int)sizeof(szPacket)))
                UdpSend(pRtpThread->m_hVideoSocket, szPacket, iSendLen,
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

                int iSendLen = (int)sizeof(RtpHeader) + 2 + iChunk;
                if (!pRtpThread->VideoSrtpEnabled() ||
                    pRtpThread->SrtpVideoProtect(szPacket, iSendLen, (int)sizeof(szPacket)))
                    UdpSend(pRtpThread->m_hVideoSocket, szPacket, iSendLen,
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
