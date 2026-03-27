#include "rtpchannel.h"

namespace AMT
{

CRtpBuffer::CRtpBuffer()
{
   m_ppPackets = NULL;
   m_nPacketSize = 0;
   m_nPacketCount = 0;
   m_pnPktLength = NULL;
}

CRtpBuffer::~CRtpBuffer()
{
   Close();
}

int CRtpBuffer::Open(int nMaxPackets, int nMaxPacketSize, int nDelayTime, int nTimeScale)
{
   if(m_nPacketCount !=0 || m_ppPackets != NULL || m_nPacketSize != 0) {
      return -1;
   }

   m_ppPackets = new unsigned char * [nMaxPackets];
   m_pnPktLength = new int [nMaxPackets];
   for(int i=0; i<nMaxPackets; i++) {
      m_ppPackets[i] = new unsigned char [nMaxPacketSize];
      m_pnPktLength[i] = 0;
   }

   m_dMSecPerTimescale = 1000. / (double)nTimeScale;
   m_nDelayTimeMs = (int)(double(nDelayTime) * m_dMSecPerTimescale;
   m_nTimeScale = nTimeScale;
   m_nPacketCount = nMaxPackets;
   m_nPacketSize  = nMaxPacketSize;

   m_uInSeq = m_uOutSeq = 0;
   m_uInTimestamp = m_uOutTimestamp = 0;

   m_bFirstPacket = true;
   return 0;
}

int CRtpBuffer::Close()
{
   if(m_ppPackets) {
      for(int i=0; i<m_nPacketCount; i++) {
         if(m_ppPackets[i]) {
            delete [] m_ppPackets[i];
            m_pnPktLength[i] = 0;
         }
      }
      delete [] m_ppPackets;
   }
   if(m_pnPktLength) delete [] m_pnPktLength;

   m_ppPackets = NULL;
   m_pnPktLength = NULL;
   m_nPacketSize = 0;
   m_nPacketCount = 0;
}

static inline int TvGetDiffMS(struct timeval* p1, struct timeval* p2) {
	return (p1->tv_sec - p2->tv_sec) * 1000 + (p1->tv_usec - p2->tv_usec) / 1000;
}


int CRtpBuffer::Put(const unsigned char * pRtpPkt, int nPktSize)
{
   if(nPktSize > m_nPacketSize || pRtpPkt == NULL) {
      return -1;
   }

   RTPHEADER * pRtpHdr = (RTPHEADER *) pRtpPkt;

   int nBufferIndex = pRtpHdr->seqnumber % m_nPacketCount;
   if(m_pnPktLength[nBufferIndex] > 0) {
      // duplication sequence 
      return -2;
   }
   int (pRtpHdr->seqnumber < m_uOutSeq ) {
      //over time
      return -3;
   } 
   
   if (pRtpHdr->seqnumber > m_uInSeq) {
      m_uInSeq = (unsigned int) pRtpHdr->seqnumber;
   } // else insert List

   if(m_bFirstPacket) {
      m_bFirstPacket = false;
      gettimeofday(&m_tvBestTimeVal, NULL);
      m_uBestTimestamp = pRtpHdr->uTimestamp;
   }  

   struct timeval tvCurTime;
   gettimeofday(&tvCurTime, NULL);
   int nDiffSysTimeMs = TvGetDiffMS(&tvCurTime, &m_tvBestTimeVal);
   int nDiffTimestampMs = (int)(double(pRtpHdr->uTimestamp - m_uBestTimestamp) * m_dMSecPerTimescale);
   if(nDiffTimestampMs < nDiffSysTimeMs) {
      m_tvBestTimeVal = tvCurTime;
      m_uBestTimestamp = pRtpHdr->uTimestamp;
   } else if(nDiffTimestampMs > nDiffSysTimeMs + m_nDelayTimeMs) {

      //Drop over time2
      return -4;
   }

   memcpy(m_ppPackets[nBufferIndex], pRtpPkt, nPktSize);
   m_pnPktLength[nBufferIndex] = nPktSize;

   return 0;
}

int CRtpBuffer::TryGet(unsigned char * pRtpPkt, int &rnPktSize)
{
   int nIndex;
   struct timeval tvCurTime;
   gettimeofday(&tvCurTime, NULL);
   int nDiffSysTimeMs = TvGetDiffMS(&tvCurTime, &m_tvBestTimeVal);
   
   unsigned int uOutTimestamp = m_uBestTimestamp + (nDiffSysTimeMs - m_nDelayTimeMs) / m_dMSecPerTimescale;

   if(m_uInSeq <= m_uOutSeq ) {
      rnPktSize = 0;
      return -1;
   }
   
   for(; m_uOutSeq <m_uInSeq; m_uOutSeq++) {
      nIndex = m_uOutSeq%m_nPacketCount;
      if(m_pnPktLength[nIndex] <= 0) {
         continue;
      }

      if((RTPHEADER *)(m_ppPackets[nIndex])->timestamp > uOutTimestamp) {
         rnPktSize = 0;
         break;
      }

      rnPktSize = m_pnPktLength[nIndex];
      memcpy(pRtpPkt, m_pnPktLength[nIndex], rnPktSize);
      m_pnPktLength[nIndex] = 0;
      m_uOutSeq ++;
      break;
   }
   if(rnPktSize <= 0) return -1;
   return 0;
}


} // namespace AMT
