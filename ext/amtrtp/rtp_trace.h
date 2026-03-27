#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

/*
   RFC 3550 5.1
   0                   1                   2                   3
   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |V=2|P|X|  CC   |M|     PT      |       sequence number         |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                           timestamp                           |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |           synchronization source (SSRC) identifier            |
   +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
   |            contributing source (CSRC) identifiers             |
   |                             ....                              |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   V   = version 2
   CC  = CSRC count 
   P   = padding bit
   X   = extension
   CC  = CSRC count
   M   = marker bit
   PT  = payload type
   seq = sequence number
   ts  = timestamp ( in audio sampling rate, ex:amrnb->8000Hz,amrwb->16000Hz
   SSRC= synchronization source
 */

#if(GCONF_BIG_ENDIAN)
typedef struct
{
  unsigned char  version:2;
  unsigned char  padding:1;
  unsigned char  extension:1;
  unsigned char  cc:4;
  unsigned char  marker:1;
  unsigned char  payloadtype:7;
  unsigned short seqnumber;
  unsigned int   timestamp;
  unsigned int   ssrc;
  unsigned int   csrc[16];
} RTPPacketHeader_T
#else
typedef struct
{
  unsigned char  cc:4;
  unsigned char  extension:1;
  unsigned char  padding:1;
  unsigned char  version:2;
  unsigned char  payloadtype:7;
  unsigned char  marker:1;
  unsigned short seqnumber;
  unsigned int   timestamp;
  unsigned int   ssrc;
  unsigned int   csrc[16];
} RTPPacketHeader_T ;
#endif

typedef struct {
#if(GCONF_BIG_ENDIAN)
  unsigned int event: 8;
  unsigned int edge: 1;
  unsigned int reserved: 1;
  unsigned int volume: 6;
  unsigned int duration: 16;
#else  // LITTLEENDIAN
  unsigned int event: 8;
  unsigned int volume: 6;
  unsigned int reserved: 1;
  unsigned int edge: 1;
  unsigned int duration: 16;
#endif
}RTPPayloadDTMF_T;


//
//// RTCP
////
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
/*
 * RTCP common header.
 */
typedef struct {
#if(GCONF_BIG_ENDIAN)
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
  unsigned int ssrc;      /* SSRC identification    */
} RTCPPacketHeader_T;

/*
 * RTCP sender report.
 */
typedef struct {
  unsigned int ntp_sec;       /* NTP time, seconds part. */
  unsigned int ntp_frac;      /* NTP time, fractions part.  */
  unsigned int rtp_ts;        /* RTP timestamp.       */
  unsigned int sender_pcount; /* Sender packet cound.      */
  unsigned int sender_bcount; /* Sender octet/bytes count. */
} RTCPSReport_T;

/*
 * RTCP receiver report.
 */
typedef struct {
  unsigned int ssrc;           /* SSRC identification.    */
#if(GCONF_BIG_ENDIAN)
  unsigned int   fract_lost:8;   /* Fraction lost.          */
  unsigned int   total_lost_2:8; /* Total lost, bit 16-23.  */
  unsigned int   total_lost_1:8; /* Total lost, bit 8-15.   */
  unsigned int   total_lost_0:8; /* Total lost, bit 0-7.    */
#else
  unsigned int   fract_lost:8;   /* Fraction lost.          */
  unsigned int   total_lost_2:8; /* Total lost, bit 0-7.    */
  unsigned int   total_lost_1:8; /* Total lost, bit 8-15.   */
  unsigned int   total_lost_0:8; /* Total lost, bit 16-23.  */
#endif
  unsigned int   last_seq;       /* Last sequence number.   */
  unsigned int   jitter;         /* Jitter.                 */
  unsigned int   lsr;            /* Last SR.                */
  unsigned int   dlsr;           /* Delay since last SR.    */

} RTCPRReport_T;

#define PT_AMR  98 
#define PT_DTMF 101
#define PT_224MUX 20 

#define LINE_LENGTH 48
#define DIV_LINE "===============================================\n"  
#define LAST_DIV_LINE "...\n===========================================\n"  

#define RTP_LINE_NUM_MINIMUM 14
#define RTCP_LINE_NUM_MINIMUM 10

#define RTP_BUF_MINIMUM (LINE_LENGTH * RTP_LINE_NUM_MINIMUM)
#define RTCP_BUF_MINIMUM (LINE_LENGTH * RTCP_LINE_NUM_MINIMUM)


int _format_rtcp_rr(char *pRtcpPkt, unsigned int unRtcpPktLen,
      char *pszLog, unsigned int unLogBufLen, unsigned int unRcount);

int _format_rtcp_sr(char *pRtcpPkt, unsigned int unRtcpPktLen,
      char *pszLog, unsigned int unLogBufLen, unsigned int unRcount);

int make_rtp_log (char *pRtpPkt, unsigned int unRtpPktLen,
                      char *pszLog, unsigned int unLogBufLen);

int make_rtcp_log (char *pRtcpPkt, unsigned int unRtcpPktLen,
                      char *pszLog, unsigned int unLogBufLen);


