/******************************************************************************
Reference: RFC3550

        0                   1                   2                   3
        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
header |V=2|P|    RC   |   PT=SR=200   |             length            |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                         SSRC of sender                        |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
sender |              NTP timestamp, most significant word             |
info   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |             NTP timestamp, least significant word             |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                         RTP timestamp                         |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                     sender's packet count                     |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                      sender's octet count                     |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
report |                 SSRC_1 (SSRC of first source)                 |
block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  1    | fraction lost |       cumulative number of packets lost       |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |           extended highest sequence number received           |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                      interarrival jitter                      |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                         last SR (LSR)                         |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                   delay since last SR (DLSR)                  |
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
report |                 SSRC_2 (SSRC of second source)                |
block  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  2    :                               ...                             :
       +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
       |                  profile-specific extensions                  |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


        0                   1                   2                   3
        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
header |V=2|P|    RC   |   PT=SR=200   |             length            |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |                         SSRC of sender                        |
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       |    CNAME=1    |     length    | user and domain name        ...
       +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

******************************************************************************/   
//#include <string.h>
//#include <sys/time.h>
//#include <unistd.h>

#include <vector>
#if !defined(_MSC_VER)
#include <sys/time.h>
#endif


#include "amtrtcp.h"
#include <string.h>
namespace AMT
{

///////////////////////////////////////////////////////////

class CRtcpReportContext
{
public:
   //CCritSec m_Mutex;
   char m_szCName[CRtcpReport::SDES_DATA_LEN_MAX];
   unsigned int m_nSSRC;
   int m_nTimescale;

   int m_nSentCount;
   int m_nSentBytes; // PayloadSize;
   unsigned int m_nLastSentSeq;
   unsigned int m_nLastSentTimestamp;
   unsigned int m_nFirstSentTimestamp;
   struct timeval m_FirstSentTimeVal;

   unsigned int m_nRecvSSRC;
   unsigned int m_nLastRecvSeq;
   unsigned int m_nLastRecvSRTS; // SR Timestamp.
   // Cumulative.
   int m_nRecvCount;
   int m_nRecvLost;

   // Counting since last RR or SR sent.
   int m_nRecvCountTerm;
   int m_nRecvLostTerm;
   
   // For Parsed Report
   int m_nParsedSSRC;
   std::vector <CRtcpReport::Report*> m_Reports;

public:
   //CRtcpReportContext() {};
   //~CRtcpReportContext() {Clear();};
   void Clear();
   void ClearReports();

};

void CRtcpReportContext::Clear()
{
   m_szCName[0] = 0;
   m_nSSRC = 0;
   m_nTimescale = 0;
   m_nSentCount = 0;
   m_nSentBytes = 0;
   m_nLastSentSeq = 0;
   m_nLastSentTimestamp = 0;
   m_nFirstSentTimestamp = 0;
   m_FirstSentTimeVal.tv_sec = 0;
   m_FirstSentTimeVal.tv_usec = 0;

   m_nLastRecvSeq = 0;
   m_nRecvSSRC = 0;
   m_nRecvCount = 0;
   m_nRecvLost = 0;

   m_nRecvCountTerm = 0;
   m_nRecvLostTerm = 0;

   m_nLastRecvSRTS = 0;

   ClearReports();
}

void CRtcpReportContext::ClearReports()
{
   std::vector <CRtcpReport::Report*>::iterator it;
   for(it = m_Reports.begin( ); it != m_Reports.end( ); it++ ) {
      if(*it) delete (CRtcpReport::Report*)(*it);
   }
   m_Reports.clear();
   m_nParsedSSRC = 0;
}

///////////////////////////////////////////////////////////
#define PUT_INT16(p, n) \
   *(p)   = (unsigned char)((n) >> 8); \
   *(p+1) = (unsigned char)((n) );

#define PUT_INT24(p, n) \
   *(p)   = (unsigned char)((n) >> 16); \
   *(p+1) = (unsigned char)((n) >> 8); \
   *(p+2) = (unsigned char)((n) );

#define PUT_INT32(p, n) \
   *(p)   = (unsigned char)((n) >> 24); \
   *(p+1) = (unsigned char)((n) >> 16); \
   *(p+2) = (unsigned char)((n) >> 8); \
   *(p+3) = (unsigned char)((n) );

#define GET_INT32(p) ( ( (p)[0] << 24 ) | ( (p)[1] << 16 ) | ( (p)[2] << 8 ) | (p)[3] )
#define GET_INT24(p) ( ( (p)[0] << 16 ) | ( (p)[1] << 8 ) | (p)[2] )
#define GET_INT16(p) ( ( (p)[0] << 8 ) | (p)[1] )


#define SOURCE_DESC_SIZE 8
#define SENDER_REPORT_SIZE 20
#define RECEIVER_REPORT_SIZE 24


CRtcpReport::CRtcpReport()
{
   m_pCtx = new CRtcpReportContext;
}

CRtcpReport::~CRtcpReport()
{
	if(m_pCtx)
	{
	   m_pCtx->ClearReports();
	   delete m_pCtx;
	}
}

int  CRtcpReport::Open(const char* pCName, unsigned int nSSRC, int nTimescale)
{
   //CAutoLock lock(&m_pCtx->m_Mutex);
   m_pCtx->Clear();
   if(strlen(pCName) >= SDES_DATA_LEN_MAX) {
      memcpy(m_pCtx->m_szCName, pCName, SDES_DATA_LEN_MAX-1);
      m_pCtx->m_szCName[SDES_DATA_LEN_MAX-1] = 0;
   } else {
      strcpy(m_pCtx->m_szCName, pCName);
   }
   m_pCtx->m_nSSRC = nSSRC;
   m_pCtx->m_nTimescale = nTimescale;
   return 0;
}

int  CRtcpReport::PacketSent(int nPayloadSize, unsigned int nSeq, unsigned int nTimestamp)
{
   //CAutoLock lock(&m_pCtx->m_Mutex);
   if(m_pCtx->m_nSentCount == 0) {
      m_pCtx->m_nFirstSentTimestamp = nTimestamp;
      gettimeofday(&m_pCtx->m_FirstSentTimeVal, NULL);
   }
   m_pCtx->m_nSentCount++;
   m_pCtx->m_nSentBytes += nPayloadSize;
   m_pCtx->m_nLastSentSeq = nSeq;
   m_pCtx->m_nLastSentTimestamp = nTimestamp;
   return 0;
}

int  CRtcpReport::PacketReceived(unsigned int nSSRC, int nPayloadSize, unsigned int nSeq, unsigned int nTimestamp)
{
   //CAutoLock lock(&m_pCtx->m_Mutex);
   if( (m_pCtx->m_nRecvCount == 0) ||
       (m_pCtx->m_nRecvSSRC != nSSRC) ) 
   {
      m_pCtx->m_nRecvSSRC = nSSRC;
      m_pCtx->m_nRecvCount = 1;
      m_pCtx->m_nRecvLost = 0;
      m_pCtx->m_nRecvCountTerm = 1;
      m_pCtx->m_nRecvLostTerm = 0;
   } else {
      int nLost = nSeq - m_pCtx->m_nLastRecvSeq - 1;
      if(nLost > 0) {
         m_pCtx->m_nRecvLost += nLost;
         m_pCtx->m_nRecvLostTerm += nLost;
      }
      m_pCtx->m_nRecvCount++;
      m_pCtx->m_nRecvCountTerm++;
   }
   m_pCtx->m_nLastRecvSeq = nSeq;
   return 0;
}


static int MakeSenderReport(CRtcpReportContext *pCtx, unsigned char *pPacket, int nInLen, int *pnOutLen)
{

   if(nInLen < SENDER_REPORT_SIZE) {
      *pnOutLen = 0;
      return -1;
   }

   struct timeval tv;
   gettimeofday(&tv, NULL);

   // NTP timestamp, most significant word.
   PUT_INT32(pPacket, tv.tv_sec + 0x83AA7E80); // 1970 - 1990 = 2208988800 sec.
   pPacket += 4;
   
   // NTP timestamp, least significant word
   PUT_INT32(pPacket, (unsigned int)(0.5 + tv.tv_usec / 1000000.0 * 0xFFFFFFFF) ); // 2^32 / 10^6
   pPacket += 4;
   
   // equivalent RTP timestamp for the above NTP.
   double dTimeDelta = (tv.tv_sec - pCtx->m_FirstSentTimeVal.tv_sec) * pCtx->m_nTimescale +
      (tv.tv_usec - pCtx->m_FirstSentTimeVal.tv_usec) / 1000000.0 * pCtx->m_nTimescale + 0.5;
   PUT_INT32(pPacket, pCtx->m_nFirstSentTimestamp + (unsigned int)dTimeDelta);
   pPacket += 4;

   // sender's packet count.
   PUT_INT32(pPacket, pCtx->m_nSentCount);
   pPacket += 4;
   
   // sender's octet count; only payload octets.
   PUT_INT32(pPacket, pCtx->m_nSentBytes);
   pPacket += 4;

   *pnOutLen = SENDER_REPORT_SIZE;

   return 0;
}


static int MakeReceiverReport(CRtcpReportContext *pCtx, unsigned char *pPacket, int nInLen, int *pnOutLen)
{

   if(nInLen < RECEIVER_REPORT_SIZE) {
      *pnOutLen = 0;
      return -1;
   }

   // SSRC_n (source identifier): 32 bits.
   PUT_INT32(pPacket, pCtx->m_nRecvSSRC);
   pPacket += 4;

   // fraction lost: 8 bits
   if(pCtx->m_nRecvCountTerm + pCtx->m_nRecvLostTerm == 0) {
      *pPacket++ = 0;
   } else {
      *pPacket++ = 255 * pCtx->m_nRecvLostTerm / 
         (pCtx->m_nRecvCountTerm + pCtx->m_nRecvLostTerm);
   }
   
   // cumulative number of packets lost: 24 bits
   PUT_INT24(pPacket, pCtx->m_nRecvLost);
   pPacket += 3;

   // extended highest sequence number received: 32 bits
   PUT_INT32(pPacket, pCtx->m_nLastRecvSeq);
   pPacket += 4;

   // interarrival jitter: 32 bits
   PUT_INT32(pPacket, 0);
   pPacket += 4;
   
   // last SR timestamp (LSR): 32 bits
   PUT_INT32(pPacket, pCtx->m_nLastRecvSRTS);
   pPacket += 4;

   // delay since last SR (DLSR): 32 bits
   PUT_INT32(pPacket, 0);
   pPacket += 4;
   
   *pnOutLen = RECEIVER_REPORT_SIZE;
   return 0;
}


static int AddSDESReport(CRtcpReportContext *pCtx, unsigned char *pPacket, int nInLen, int *pnOutLen)
{
   if(nInLen < SOURCE_DESC_SIZE) {
      *pnOutLen = 0;
      return -1;
   }
   unsigned char* p = pPacket + 8;
   int nLen = strlen(pCtx->m_szCName);
   int nBytes = 4;

   p[0] = CRtcpReport::DT_CNAME;
   p[1] = nLen;
   memcpy(p+2, pCtx->m_szCName, nLen);
   nBytes += 2 + nLen;
   nBytes = (nBytes+3) / 4 * 4; // multiple of four.

   pPacket[0] = (unsigned char)( (2 << 6) | 1 );
   pPacket[1] = (unsigned char)( CRtcpReport::RT_SOURCE_DESC );
   PUT_INT16(pPacket+2, nBytes/4);
   PUT_INT32(pPacket+4, pCtx->m_nSSRC);

   *pnOutLen = 4 + nBytes;
   return 0;
}

int  CRtcpReport::MakeReport(unsigned char *pPacket, int nInLen, int *pnOutLen)
{
   int rc;
   int nLength;
   int nReportType;
   int nReportCount = 0;
   int nPayloadSize = 0;
   unsigned char *p = pPacket + 8;
   //CAutoLock lock(&m_pCtx->m_Mutex);

   if(m_pCtx->m_nSentCount > 0) {
      nReportType = RT_SENDER_REPORT;
      rc = MakeSenderReport(m_pCtx, p, nInLen - (p - pPacket), &nLength);
      if(rc == 0) {
         p += nLength;
      }
   } else {
      nReportType = RT_RECEIVER_REPORT;
   }

   rc = MakeReceiverReport(m_pCtx, p, nInLen - (p - pPacket), &nLength);
   if(rc == 0) {
      p += nLength;
      nReportCount++;
   }

   // nReportCount : excludes sender-report.
   pPacket[0] = (unsigned char)( (2 << 6) | nReportCount );
   pPacket[1] = (unsigned char)( nReportType );
   PUT_INT16(pPacket+2, ((p - pPacket) - 1) / 4);
   PUT_INT32(pPacket+4, m_pCtx->m_nSSRC);

   rc = AddSDESReport(m_pCtx, p, nInLen - (p - pPacket), &nLength);
   if(rc == 0) {
      *pnOutLen = (p - pPacket) + nLength;
   }
   
   m_pCtx->m_nRecvCountTerm = 0;
   m_pCtx->m_nRecvLostTerm = 0;

   return 0;
}


int  CRtcpReport::Close()
{
   m_pCtx->Clear();
   return 0;
}


int CRtcpReport::ParseReport(unsigned char* pPacket, int nInLen)
{
   unsigned int nCount, nType, nLen;
   unsigned char *p = pPacket;
   unsigned int nSSRC;
   Report *pReport = NULL;

   m_pCtx->ClearReports();

   while( nInLen - (p-pPacket) > 12 ) {
      nCount = p[0] & 0x1F;
      nType  = p[1];
      nLen   = 4 * ( (p[2] << 8) | p[3] );
      p += 4;
      if(nLen > nInLen - (p-pPacket)) break;
      if(nLen < 4) break;
      nLen -= 4;

      m_pCtx->m_nParsedSSRC = GET_INT32(p); p += 4;

      if(nType == RT_SENDER_REPORT) {
         if(nLen < SENDER_REPORT_SIZE) break;
         nLen -= SENDER_REPORT_SIZE;
         pReport = new Report;
         memset(pReport, 0, sizeof(Report));
         pReport->type = RT_SENDER_REPORT;
         pReport->sr.ntp_sec  = GET_INT32(p); p+=4;
         pReport->sr.ntp_frac = GET_INT32(p); p+=4;
         pReport->sr.rtp_ts   = GET_INT32(p); p+=4;
         pReport->sr.psent    = GET_INT32(p); p+=4;
         pReport->sr.osent    = GET_INT32(p); p+=4;
         m_pCtx->m_Reports.push_back(pReport);
      }
      if(nType == RT_SENDER_REPORT ||
         nType == RT_RECEIVER_REPORT) {
         int nBlocksSize = nCount * RECEIVER_REPORT_SIZE;
         if(nLen < nBlocksSize) break;
         nLen -= nBlocksSize;
         for(int i=0;i<nCount;i++) {
            nSSRC = GET_INT32(p); p+=4;
#if 0
            if(nSSRC != m_pCtx->m_nSSRC) {
               p+=RECEIVER_REPORT_SIZE-4;
            } else
#endif
            {
               pReport = new Report;
               memset(pReport, 0, sizeof(Report));
               pReport->type = RT_RECEIVER_REPORT;
               pReport->rr.ssrc     = nSSRC;
               pReport->rr.fraction = *p;           p+=1;
               pReport->rr.lost     = GET_INT24(p); p+=3;
               pReport->rr.last_seq = GET_INT32(p); p+=4;
               pReport->rr.jitter   = GET_INT32(p); p+=4;
               pReport->rr.lsr      = GET_INT32(p); p+=4;
               pReport->rr.dlsr     = GET_INT32(p); p+=4;
               m_pCtx->m_Reports.push_back(pReport);
            }
         }
      } else {
         p += nLen;
      }
   }

   return 0;
}

unsigned int CRtcpReport::GetSenderSSRC()
{
   return m_pCtx->m_nParsedSSRC;
}

int CRtcpReport::GetReportCount()
{
   return m_pCtx->m_Reports.size();
}

int CRtcpReport::GetReport(int nIndex, Report *pReport)
{
   if(nIndex >= m_pCtx->m_Reports.size()) return -1;
   std::vector <CRtcpReport::Report*>::iterator it;
   it = m_pCtx->m_Reports.begin( ) + nIndex;
   *pReport = *(*it);

   return 0;
}


}; //namespace

