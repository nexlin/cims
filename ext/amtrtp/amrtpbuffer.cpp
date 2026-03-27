/**
 *
 */
#include <stdio.h>
#include <sys/types.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#endif

#if defined(_MSC_VER)
//#include "pdkutil.h"
#else
#include <sys/time.h>
#include "util.h"
#endif

#include "amtrtp.h"

#include "amrtpbuffer.h"

namespace AMT
{

int  CAmRtpBuffer::ms_nLimitationMs = 500; // 500msec   


CAmRtpBuffer::CAmRtpBuffer()
{
   m_pPackets = NULL;
   m_nPacketSize = 0;
   m_nPacketCount = 0;
   m_nLastSSRC = -1;
   m_bOpen = false;
   m_nMode = MD_SEQ;
}

CAmRtpBuffer::~CAmRtpBuffer()
{
   Close();
}

// nDelayTime: millisecond.
int CAmRtpBuffer::Open(int nMaxPackets, int nMaxPacketSize, int nDelayTime, int nTimeScale, MODE nMode)
{
   if(m_nPacketCount !=0 || m_pPackets != NULL || m_nPacketSize != 0) {
      return -1;
   }
   //m_Mutex.Lock();

   m_nMode = nMode;

   m_pPackets = new APACKET[nMaxPackets];
   for(int i=0; i<nMaxPackets; i++) {
      m_pPackets[i].data = new unsigned char [nMaxPacketSize];
      m_pPackets[i].len = 0;
   }

   m_dMSecPerTimescale = 1000. / (double)nTimeScale;
   m_nDelayTimeMs = nDelayTime;
   m_nTimeScale   = nTimeScale;
   m_nPacketCount = nMaxPackets;
   m_nPacketSize  = nMaxPacketSize;

   m_uBaseSeq = m_uInSeq       = m_uOutSeq = 0;
   m_uInTimestamp = m_uOutTimestamp = 0;

   m_bFirstPacket = true;
   m_bOpen = true;
   //m_Mutex.Unlock();
   return 0;
}

int CAmRtpBuffer::Clean()
{
   if(m_pPackets) {
      for(int i=0; i<m_nPacketCount; i++) {
         m_pPackets[i].len = 0;
      }
   }
   m_uBaseSeq = 0;
   m_uInSeq = m_uOutSeq = 0;

   m_uInTimestamp = m_uOutTimestamp = 0;
   memset(&m_tvBestTimeVal, 0, sizeof(struct timeval));
   memset(&m_tvLastRecvTimeVal, 0, sizeof(struct timeval));
   m_bFirstPacket = true;
   return 0;
}

int CAmRtpBuffer::Close()
{
   //m_Mutex.Lock();
   m_bOpen = false;
   if(m_pPackets) {
      for(int i=0; i<m_nPacketCount; i++) {
         if(m_pPackets[i].data) delete [] m_pPackets[i].data;
      }
      delete [] m_pPackets;
   }

   m_pPackets = NULL;
   m_nPacketSize = 0;
   m_nPacketCount = 0;
   m_nLastSSRC = -1;
   //m_Mutex.Unlock();

   return 0;
}


int CAmRtpBuffer::Put(const unsigned char * pRtpPkt, int nPktSize)
{
   int rc=0;
   //m_Mutex.Lock();
   do{
      if(!m_bOpen) {
         rc = -1;
         break;
      }
      if(nPktSize > m_nPacketSize || pRtpPkt == NULL) {
         rc = -1;
         break;
      }
      
      struct timeval tvCurTime;
      gettimeofday(&tvCurTime, NULL);

      unsigned short uInSeq = (unsigned int)htons(((RTPHEADER *)pRtpPkt)->seqnumber);
      unsigned int uTimestamp = (unsigned int)htonl(((RTPHEADER *)pRtpPkt)->timestamp);
      int nSSRC = htonl(((RTPHEADER *)pRtpPkt)->ssrc);

      if(m_nLastSSRC != nSSRC
         || TvGetDiffMS(&tvCurTime, &m_tvLastRecvTimeVal) > ms_nLimitationMs) 
      {  
         Clean();
         m_nLastSSRC = nSSRC;
      }

      if((uInSeq<100) && m_uInSeq> m_uBaseSeq + MAX_SEQ-100) {
         m_uBaseSeq += MAX_SEQ;
         printf("WARN: RtpBuffer overflow sequence number(seq:%u/%u, timestamp:%u)\n", uInSeq, m_uInSeq, uTimestamp);
      } 

      unsigned int uInternalSeq = uInSeq + m_uBaseSeq;
      if( (uInternalSeq > m_uInSeq) && (uInternalSeq-m_uInSeq > (unsigned int)(MAX_SEQ-m_nPacketCount) ) )
      {
         printf("WARN: RtpBuffer overflow sequence number2(seq:%u/%u, timestamp:%u)\n", uInSeq, m_uInSeq, uTimestamp);
         uInternalSeq -= MAX_SEQ;
         rc = -2;
         break;
      } 

      int nBufferIndex = (uInternalSeq) % m_nPacketCount;
      if(uInternalSeq < m_uOutSeq) {
         //over time
         printf("WARN: RtpBuffer, received duplicate packet(seq:%u/%u, timestamp:%u)\n", uInSeq, uInternalSeq, uTimestamp);
         rc = -3;
         break;
      } else if (uInternalSeq < m_uInSeq &&  m_pPackets[nBufferIndex].len > 0) {
         printf("WARN: RtpBuffer, received duplicate packet2(seq:%u/%u, timestamp:%u)\n", uInSeq, uInternalSeq, uTimestamp);
         rc = -4;
         break;
      }

      if(m_bFirstPacket) {
         m_bFirstPacket = false;
         gettimeofday(&m_tvBestTimeVal, NULL);
         m_uBestTimestamp = uTimestamp;
         m_uOutSeq = uInternalSeq;
      }  else {
         int nDiffSysTimeMs = TvGetDiffMS(&tvCurTime, &m_tvBestTimeVal);
         int nDiffTimestampMs = (int)(((double)uTimestamp-(double)m_uBestTimestamp) * m_dMSecPerTimescale);
         if(nDiffTimestampMs < nDiffSysTimeMs) {
            m_tvBestTimeVal = tvCurTime;
            m_uBestTimestamp = uTimestamp;
         }

         int nDiffSeq = (uInternalSeq - m_uOutSeq) - m_nPacketCount;
         if(nDiffSeq>0) { // clean duplicate slot

            printf("WARN: RtpBuffer, duplicate slot, might be buffer full(curseq:%u, count:%d, inseq:%u/%u, outseq:%u)\n", 
                        uInSeq, uInternalSeq, m_nPacketCount, m_uInSeq, m_uOutSeq);

            nDiffSeq %= m_nPacketCount;
            for(int i=0; i<nDiffSeq; i++) { // clear slot
               m_pPackets[(m_uOutSeq++) % m_nPacketCount].len = 0; 
            }
            m_uOutSeq = (uInternalSeq+1) - m_nPacketCount;
         }
      }

      m_tvLastRecvTimeVal = tvCurTime;

      memcpy(m_pPackets[nBufferIndex].data, pRtpPkt, nPktSize);
      m_pPackets[nBufferIndex].len = nPktSize;
      m_pPackets[nBufferIndex].timestamp = uTimestamp;
      m_pPackets[nBufferIndex].rcvTime = tvCurTime; 
      m_uInSeq = uInternalSeq+1; // next seq...
   } while(false);
   //m_Mutex.Unlock();
   return rc;
}

int CAmRtpBuffer::TryGet(unsigned char * pRtpPkt, int nInLen)
{
   unsigned char * ptr = NULL;
   int nLen;
   ptr = TryGetBuffer(nLen);

   if(ptr && nInLen >= nLen) {
      memcpy(pRtpPkt, ptr, nLen);
   }
   return nLen;
}

unsigned char * CAmRtpBuffer::TryGet(int &rnOutLen)
{
   if(m_nMode == MD_SEQ) {
      return TryGetBuffer(rnOutLen);
   } else if(m_nMode == MD_TIME) {
      return TryGetBuffer2(rnOutLen);
   }
   rnOutLen = 0;
   return NULL;
}

unsigned char * CAmRtpBuffer::TryGetBuffer(int &rnPktSize)
{
   int nIndex;
   unsigned char * ptr = NULL;
   struct timeval tvCurTime;

   int nDiffSysTimeMs ;
 
   //m_Mutex.Lock();
   gettimeofday(&tvCurTime, NULL);

   do {
      if(!m_bOpen) {
         break;
      }
      if(!m_pPackets || (m_uInSeq <= m_uOutSeq)) {
         rnPktSize = 0;
         break;
      }
     
      unsigned int uSeq = m_uOutSeq;
      unsigned int uOutSeq = m_uOutSeq;
      for(; uSeq <m_uInSeq; uSeq++) {
         nIndex = uSeq%m_nPacketCount;
         if(m_pPackets[nIndex].len <= 0) { //re-oder
            uOutSeq++;
            continue;
         }

         nDiffSysTimeMs = TvGetDiffMS(&tvCurTime, &m_pPackets[nIndex].rcvTime);
         if(nDiffSysTimeMs < m_nDelayTimeMs) {
            continue;
         }
         
         nIndex = uOutSeq%m_nPacketCount;

         rnPktSize = m_pPackets[nIndex].len;
         ptr = m_pPackets[nIndex].data; 
         m_pPackets[nIndex].len = 0;
         m_uOutSeq += uOutSeq;
      }
   } while(false);

   //m_Mutex.Unlock();

   return ptr;
}
unsigned char * CAmRtpBuffer::TryGetBuffer2(int &rnPktSize)
{
   int nIndex;
   unsigned char * ptr = NULL;
   struct timeval tvCurTime;

   gettimeofday(&tvCurTime, NULL);
   int nDiffSysTimeMs = TvGetDiffMS(&tvCurTime, &m_tvBestTimeVal);
   
   int nOutTimestamp = (int)(m_uBestTimestamp + (nDiffSysTimeMs - m_nDelayTimeMs) / m_dMSecPerTimescale);
   //unsigned int uOutTimestamp = nOutTimestamp>0?nOutTimestamp:0; 
   //m_Mutex.Lock();
   do {
      if(!m_bOpen) {
         break;
      }
      if(!m_pPackets || (m_uInSeq <= m_uOutSeq)) {
         rnPktSize = 0;
         break;
      }
     
      for(; m_uOutSeq <m_uInSeq; m_uOutSeq++) {
         nIndex = m_uOutSeq%m_nPacketCount;
         if(m_pPackets[nIndex].len <= 0) {
            continue;
         }

         //if((unsigned int)m_pPackets[nIndex].timestamp >= uOutTimestamp) {
         if(m_pPackets[nIndex].timestamp > nOutTimestamp) {
            rnPktSize = 0;
            break;
         }

         rnPktSize = m_pPackets[nIndex].len;
         ptr = m_pPackets[nIndex].data; 
         m_pPackets[nIndex].len = 0;
         m_uOutSeq ++;
         //printf("## GetOut [len:%d] [inseq:%u] [outseq:%d] [time:%d/%d]\n", rnPktSize, m_uInSeq, m_uOutSeq, m_pPackets[nIndex].timestamp, nOutTimestamp);
         break;
      }
   } while(false);
   //m_Mutex.Unlock();

   return ptr;
}


#if 0
int CAmRtpBuffer::Get(unsigned char * pRtpPkt, int &rnPktSize) // block
{
   //
   return 0;
}

unsigned char * CAmRtpBuffer::GetBuffer(int &rnPktSize) // block
{
   //
   return NULL;
}
#endif

}; //namespace

