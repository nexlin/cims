#ifndef __AMT_RTCP_H__
#define __AMT_RTCP_H__


#ifdef _WIN32
#include "base.h"
#endif

namespace AMT
{

class CRtcpReportContext;

class CRtcpReport
{
public:
   typedef enum {
      RT_SENDER_REPORT   = 200,
      RT_RECEIVER_REPORT = 201,
      RT_SOURCE_DESC     = 202,
      RT_GOOD_BYE        = 203,
      RT_APP_DEFINED
   } ReportType;

   typedef enum {
      DT_END,
      DT_CNAME,
      DT_NAME,
      DT_EMAIL,
      DT_PHONE,
      DT_LOC,
      DT_TOOL,
      DT_NOTE,
      DT_PRIV,
      NumDescriptionTypes
   } DescriptionType;

   typedef struct {
      unsigned int ntp_sec;   /* NTP timestamp */
      unsigned int ntp_frac;
      unsigned int rtp_ts;    /* RTP timestamp */
      unsigned int psent;     /* packets sent */
      unsigned int osent;     /* octets sent */
   } SenderReport;

   typedef struct {
      unsigned int  ssrc;      /* data source being reported */
      unsigned char fraction;  /* fraction lost since last SR/RR */
      unsigned int  lost;	    /* cumulative number of packets lost (signed!) */
      unsigned int  last_seq;  /* extended last sequence number received */
      unsigned int  jitter;    /* interarrival jitter */
      unsigned int  lsr;       /* last SR packet from this source */
      unsigned int  dlsr;      /* delay since last SR packet */
   } ReceiverReport;

   enum {SDES_DATA_LEN_MAX = 128};

   typedef struct {
      unsigned char type;           /* type of SDES item (enum DescriptionTypes) */
      unsigned char length;         /* length of SDES item (in octets) */
      char data[SDES_DATA_LEN_MAX]; /* text, not zero-terminated */
   } SourceDescription;

   typedef struct {
      ReportType type;
      union {
         SenderReport      sr;
         ReceiverReport    rr;
         SourceDescription sd;
      };
   } Report;

public:
   CRtcpReport();
   ~CRtcpReport();

   int  Open(const char* pCName, unsigned int nSSRC, int nTimescale); // timestamp frequency.
   int  PacketSent(int nPayloadSize, unsigned int nSeq, unsigned int nTimestamp);
   int  PacketReceived(unsigned int nSSRC, int nPayloadSize, unsigned int nSeq, unsigned int nTimestamp);
   int  MakeReport(unsigned char *pPacket, int nInLen, int *pnOutLen);
   int  Close();

   int ParseReport(unsigned char* pPacket, int nLength);
   unsigned int GetSenderSSRC();
   int GetReportCount();
   int GetReport(int nIndex, Report *pReport);

protected:
   CRtcpReportContext *m_pCtx;

};

}; //namespace AMT


#endif //__AMT_RTCP_H__


