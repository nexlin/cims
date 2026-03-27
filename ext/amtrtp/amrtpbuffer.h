#ifndef __RTP_BUFFER_H__
#define __RTP_BUFFER_H__

#ifdef _WIN32
#include "base.h"
#endif

namespace AMT
{

inline int TvGetDiffMS(struct timeval* p1, struct timeval* p2) {
	return (p1->tv_sec - p2->tv_sec) * 1000 + (p1->tv_usec - p2->tv_usec) / 1000;
}


class CAmRtpBuffer 
{
public:
   enum MODE {
      MD_SEQ = 1,
      MD_TIME = 2,
   };

   enum { 
      MAX_SEQ = 0xFFFF,
      DIF_SEQ = 0xFF00,
   };
   typedef struct {
      int timestamp;
      int len;
      struct timeval rcvTime;
      unsigned char *data;
   } APACKET;

public:
   static void SetLimitationMs(int nLimit) { ms_nLimitationMs = nLimit; }

   CAmRtpBuffer();
   virtual ~CAmRtpBuffer();

   virtual int Open(int nMaxPackets, int nMaxPacketSize, int nDelayTime, int nTimeScale, MODE nMode = MD_SEQ);
   virtual int Close();

   virtual int Put(const unsigned char * pRtpPkt, int nPktSize); 
   
   //non-block
   virtual int TryGet(unsigned char * pRtpPkt, int nInLen); //copy packet & return packet length
   virtual unsigned char * TryGet(int &rnOutLen); //copy packet & return packet length
   
   //block
   //virtual int Get(unsigned char * pRtpPkt, int &rnOutLen); // block
   //virtual unsigned char * Get(int &rnOutLen); // block

protected:
   int Clean();

   //non-block
   //by sequence
   unsigned char * TryGetBuffer(int &rnPktSize);        //return packet buffer pointer 
   //by timestamp
   unsigned char * TryGetBuffer2(int &rnPktSize);        //return packet buffer pointer 
   
   //block
   //unsigned char * GetBuffer(int &rnPktSize); // block

protected:
   static int       ms_nLimitationMs; // 

   MODE             m_nMode;                             // default MD_SEQ              

   APACKET         *m_pPackets;
   //CCritSec m_Mutex;

   int              m_nPacketSize;     
   int              m_nPacketCount;

   int              m_nDelayTimeMs; //milli-sec
   int              m_nTimeScale;
   double           m_dMSecPerTimescale;

   unsigned int     m_uBaseSeq;
   unsigned int     m_uInSeq;
   unsigned int     m_uOutSeq;
   unsigned int     m_uOutTimestamp;
   unsigned int     m_uInTimestamp;

   bool             m_bFirstPacket;

   struct timeval   m_tvBestTimeVal;
   struct timeval   m_tvLastRecvTimeVal;
   unsigned int     m_uBaseTimestamp;
   unsigned int     m_uBestTimestamp;

   int              m_nLastSSRC;
   bool             m_bOpen;

};


}; //namespace

#endif
////@}
