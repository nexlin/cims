#ifndef __AMT_CRTCP_H__
#define __AMT_CRTCP_H__

#ifndef WIN32
#include <sys/time.h>
#include <arpa/inet.h>
#include <stddef.h>
#include <fcntl.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#else
#include <afxcoll.h>
#include <time.h>
#include <ws2tcpip.h> 
#include "base.h"
#endif

namespace AMT 
{

#define MIN_SEQUENTIAL  ((unsigned short)2)
#define MAX_DROPOUT     ((unsigned short)3000)
#define MAX_MISORDER    ((unsigned short)100 )
#define RTP_SEQ_MOD     (1 << 16)
#define RTCP_MAXSDES_LEN 255
#define RTCP_MIN_BUFF  sizeof(RTCPPacketHeader_T)+sizeof(RTCPSReport_T)+sizeof(RTCPRReport_T)+RTCP_MAXSDES_LEN

/*
* RTCP common header.
*/
typedef struct RTCPPacketHeader_T{
#if(HUMT_GCONF_BIG_ENDIAN)
   unsigned    version:2; /* packet type            */
   unsigned    p:1;       /* padding flag           */
   unsigned    count:5;   /* varies by payload type */
   unsigned    pt:8;      /* payload type           */
#else
   unsigned    count:5;   /* varies by payload type */
   unsigned    p:1;       /* padding flag           */
   unsigned    version:2; /* packet type            */
   unsigned    pt:8;      /* payload type           */
#endif
   unsigned    length:16; /* packet length          */
   unsigned int      ssrc;      /* SSRC identification    */
   void hton(void){
      length = htons(length);
      ssrc   = htonl(ssrc);
   }

   void ntoh(void){
      length = ntohs(length);
      ssrc   = ntohl(ssrc);
   }
};

/*
 * RTCP sender report.
*/
typedef struct RTCPSReport_T{
   unsigned int ntp_sec;       /* NTP time, seconds part. */
   unsigned int ntp_frac;      /* NTP time, fractions part.  */
   unsigned int rtp_ts;        /* RTP timestamp.       */
   unsigned int sender_pcount; /* Sender packet cound.      */
   unsigned int sender_bcount; /* Sender octet/bytes count. */

   void ntoh(void){
      ntp_sec       = ntohl(ntp_sec);
      ntp_frac      = ntohl(ntp_frac);
      rtp_ts        = ntohl(rtp_ts);
      sender_pcount = ntohl(sender_pcount);
      sender_bcount = ntohl(sender_bcount);
   }
   void hton(void){
      ntp_sec       = htonl(ntp_sec);
      ntp_frac      = htonl(ntp_frac);
      rtp_ts        = htonl(rtp_ts);
      sender_pcount = htonl(sender_pcount);
      sender_bcount = htonl(sender_bcount);
   }
};

/*
* RTCP sender report.
*/
typedef struct RTCPRReport_T{
   unsigned int  ssrc;           /* SSRC identification.    */
#if(HUMT_GCONF_BIG_ENDIAN)
   unsigned int  fract_lost:8;   /* Fraction lost.          */
   unsigned int  total_lost_2:8; /* Total lost, bit 16-23.  */
   unsigned int  total_lost_1:8; /* Total lost, bit 8-15.   */
   unsigned int  total_lost_0:8; /* Total lost, bit 0-7.    */
#else
   unsigned int  fract_lost:8;   /* Fraction lost.          */
   unsigned int  total_lost_2:8; /* Total lost, bit 0-7.    */
   unsigned int  total_lost_1:8; /* Total lost, bit 8-15.   */
   unsigned int  total_lost_0:8; /* Total lost, bit 16-23.  */
#endif
   unsigned int  last_seq;       /* Last sequence number.   */
   unsigned int  jitter;         /* Jitter.                 */
   unsigned int  lsr;            /* Last SR.                */
   unsigned int  dlsr;           /* Delay since last SR.    */

   void ntoh(void){
      ssrc    =ntohl(ssrc);
      last_seq=ntohl(last_seq);
      jitter  =ntohl(jitter);
      lsr     =ntohl(lsr);
      dlsr    =ntohl(dlsr);
   }
   void hton(void){
      ssrc    =htonl(ssrc);
      last_seq=htonl(last_seq);
      jitter  =htonl(jitter);
      lsr     =htonl(lsr);
      dlsr    =htonl(dlsr);
   }
};

/*
* RTCP SDES.
*/
struct RTCPSDES_T{
   unsigned char type;
   unsigned char length;
   //char value[RTCP_MAXSDES_LENGTH + 1];
};

struct RtpChkStatus_T {
   union {
      struct flag{
         int  bad:1      ;/* General flag to indicate that sequence is
                             bad, and application should not process
                             this packet. More information will be given
                             in other flags.                             */
         int  badpt:1    ;/* Bad payload type.                           */
         int  badssrc:1  ;/* Bad SSRC                                    */
         int  dup:1      ;/* Indicates duplicate packet                  */
         int  outorder:1 ;/* Indicates out of order packet               */
         int  probation:1;/* Indicates that session is in probation
                             until more packets are received.           */
         int  restart:1;  /* Indicates that sequence number has made
                             a large jump, and internal base sequence
                             number has been adjusted.                  */
         int  reserved:1; /* Reserved field                              */
      } flag;
      unsigned short value;    /* Status value, to conveniently address all flags.*/
   } status;            /* Status information union.*/

   unsigned short   diff;       /* Sequence number difference from previous
                                   packet. Normally the value should be 1.
                                   Value greater than one may indicate packet
                                   loss. If packet with lower sequence is
                                   received, the value will be set to zero.
                                   If base sequence has been restarted, the
                                   value will be one.            
                                 */
};

struct RtpSeqCtrl_T{
   unsigned short max_seq  ; /* received sequence number           */
   unsigned int cycles   ; /* Shifted count of seq number cycles */
   unsigned int base_seq ; /* Base seq number                    */
   unsigned int bad_seq  ; /* Last 'bad' seq number + 1          */
   unsigned int probation; /* Sequ. packets till source is valid */

   void Reset(void){
      max_seq = 0;
      cycles  = 0;
      base_seq= 0;
      bad_seq = 0;
      probation=0;
   }

   void Restart(unsigned short seq){
      base_seq = seq ;
      max_seq  = seq ;
      bad_seq  = RTP_SEQ_MOD + 1;
      cycles   = 0 ;
   }
   void Init(unsigned short seq){
      Restart(seq);
      max_seq  =(unsigned short)(seq-1) ;
      probation= MIN_SEQUENTIAL ;
   }
   void Update(unsigned short seq,RtpChkStatus_T*status){
      unsigned short udelta = (unsigned short)(seq - max_seq);
      RtpChkStatus_T st ;

      /* Init status */
      st.status.value = 0;
      st.diff = 0;

      /*
       * Source is not valid until MIN_SEQUENTIAL packets with
       * sequential sequence numbers have been received.
      */
      // 최초 2번 만 sequence 검사
      if (probation) {
         st.status.flag.probation = 1;
         //정상인 case
         if (seq == (unsigned short)(max_seq+1)) {
            /* packet is in sequence */
            st.diff = 1;
            probation--;
            max_seq = seq;
            if (probation == 0) {
               st.status.flag.probation = 0;
            }
            //비정상 case
         } else {
            st.diff = 0;
            st.status.flag.bad = 1;

            if (seq == max_seq){
               st.status.flag.dup = 1;
            }else{
               st.status.flag.outorder = 1;
            }

            probation = MIN_SEQUENTIAL - 1;
            max_seq = seq;
         }
         //중복 sequence number일 경우
      } else if (udelta == 0) {
         //st.status.flag.bad = 1; // added by mind
         st.status.flag.dup = 1; //set duplicated
         //sequence차이가 , 3000
      } else if (udelta < MAX_DROPOUT) {
         // forward jump in  max dropouot range.
         /* in order, with permissible gap */
         if (seq < max_seq) {
            /* Sequence number wrapped - count another 64K cycle. */
            cycles += RTP_SEQ_MOD;
            //LOGGER(PL_CRITICAL,"cycles increased\n");
         }
         max_seq = seq    ;
         st.diff = udelta ;
         //sequence차이가, 65536-100 = 65,436
      } else if (udelta <= (RTP_SEQ_MOD - MAX_MISORDER)) {
         /* the sequence number made a very large jump */
         if (seq == bad_seq) {
            /*
             * Two sequential packets -- assume that the other side
             * restarted without telling us so just re-sync
             * (i.e., pretend this was the first packet).
             */
            Restart(seq);
            st.status.flag.restart = 1;
            st.status.flag.probation = 1;
            st.diff = 1;
         }else {
            // not update max_seq.
            // min 2 packet's sequence match, restart and probation.
            bad_seq = (seq + 1) & (RTP_SEQ_MOD-1);
            st.status.flag.bad      = 1;
            st.status.flag.outorder = 1;
         }
      } else {
         // backward jump
         /* old duplicate or reordered packet.
          * Not necessarily bad packet (?)
          */
         st.status.flag.outorder = 1;
      }

      status->diff = st.diff;
      status->status.value = st.status.value;
   }
};

////CRtcp main context
struct CRtcpCtx 
{
//RTCP Build Mode
   enum RTCPBUILD_MODE{    
      SR_ONLY=1, 
      SR_SDES=2,
      RR_ONLY=3, 
      RR_SDES=4,
   };   

   typedef enum {
      RT_SENDER_REPORT   = 200,
      RT_RECEIVER_REPORT = 201,
      RT_SOURCE_DESC     = 202,
      RT_GOOD_BYE        = 203,
      RT_APP_DEFINED     = 204,
      RT_EMPTY_RR        = 1,
   } ReportType;

   typedef enum {
      RT_SDES_END        = 0,
      RT_SDES_CNAME      = 1,
      RT_SDES_NAME       = 2,
      RT_SDES_EMAIL      = 3,
      RT_SDES_PHONE      = 4,
      RT_SDES_LOC        = 5,
      RT_SDES_TOOL       = 6,
      RT_SDES_NOTE       = 7,
      RT_SDES_PRIV       = 8,
   } ReportSDESType;

   unsigned int         m_TotalRxPackets;
   unsigned int         m_TotalRxBytes  ;
   unsigned int         m_TotalRxLoss   ;

   unsigned int         m_LastTxSeq     ;
   unsigned int         m_LastTxTS      ;
   unsigned int         m_TotalTxPackets;
   unsigned int         m_TotalTxBytes  ;

   unsigned int         m_LastRxSSRC    ;
   unsigned int         m_LastRxTS    ;

   struct timeval       m_RxFirstRTP ;// for jitter
   unsigned int         m_FirstTS   ; // for receive report
   struct timeval       m_RxLSRTime ;// for receive report rx peer's SR receive time.
   unsigned int         m_ThemLSR   ;// rx peer's SR NTP time.
   unsigned int         m_Received ; // for receive report
   unsigned int         m_ExpPrior ; // for receive report 
   unsigned int         m_RxPrior  ; // for receive report
   unsigned int         m_Transit  ; // for receive report
   unsigned int         m_Jitter   ; // for receive report
   double               m_RoundTrip;

   // sequence related information
   RtpSeqCtrl_T         m_SeqCtrl  ;
   RTCPSReport_T        m_LastSR   ; 
   RTCPRReport_T        m_LastRR   ;

public:
   void Reset();
   void SeqRestarted( );
   void RTPReceived(unsigned int seq, unsigned int ts, unsigned int ssrc, unsigned int len_payload, 
                     unsigned int nclock_rate,RtpChkStatus_T* oseq_st);
   //must be rtp payload length
   void RTPSent(unsigned int seq,unsigned int ts,unsigned int len_payload);
   void RTCPReceived(unsigned char *pData,unsigned int nSize);
   int  BuildRTCPPacket(char*packet,unsigned int packetlen,unsigned int myssrc,int mode,char*cname);
protected:
   //static
   static unsigned int GetTimeDiff(struct timeval *p_tvstart, struct timeval *p_tvend=NULL);
   static void Timeval2NTPTime(struct timeval *p_tv, unsigned int *p_msw, unsigned int *p_lsw);
   static void NTPTime2Timeval(struct timeval *p_tv, unsigned int msw, unsigned int lsw);
protected:
   void ProcessRTCPPacket(RTCPPacketHeader_T* pHdr, unsigned char* pBody, unsigned int uiLen);
   int BuildSR(char*packet,unsigned int myssrc);
   int BuildSDES(char*packet,unsigned int myssrc,char*cname);
   int BuildRR(char*packet,unsigned int myssrc);
}; 



}; //namespace AMT


#endif 

