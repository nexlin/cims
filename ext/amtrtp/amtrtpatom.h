#ifndef  __AMT_RTP_ATOM_H_
#define  __AMT_RTP_ATOM_H_

#if defined(_MSC_VER)
//#include "pdkutil.h"
#else
#include <sys/time.h>
#include "util.h"
#endif

//#include "jthread.h"
#include "amtrtcp.h"
#include "mtcodectype.h"


namespace AMT
{

class CAmRtpBuffer;

// RTP Communicator.
/***********************************************************************
SEQUENCE:
   {OpenSocket 
     {StartSession 
        {WaitForPacket | PutPayload}... 
     EndSeccion}... 
   CloseSocket}...
***********************************************************************/
class CRtpComm
{
public:
   enum PACKET_TYPE {
      PT_TIMEOUT      = 0x00,
      PT_RTP          = 0x10,
      PT_RTCP         = 0x20,

      PT_H261FIR = 0x21,
      PT_FB_PLE  = 0x22,

      PT_RTP_DROP     = 0x1D,
      PT_RTCP_DROP    = 0x2D,

      PT_RTP_UNKNOWN  = 0x1E,
      PT_RTCP_UNKNOWN = 0x2E,

      PT_NOT_STARTED  = 0xFE,
      PT_ERROR        = 0xFF,
   };

   enum { 
      MAX_IP_ADDR   = 16, 
      MAX_CNAME_LEN = 64
   }; 

   enum FIR_MODE {
      FIRM_MANUAL       = 0,   // default
      FIRM_RTCP_H261FIR = 1,
      FIRM_RTCP_FB_PLI  = 2,
   };
   
   enum BUFF_MODE {
      BM_NONE    = 0,      // non-buffering
      BM_SEQ     = 1,    
      BM_TIME    = 2,
   };

   struct IP_PORT {
      bool bRtpOn;                 // true: on
      bool bRtcpOn;                // true: on
      char szAddr[MAX_IP_ADDR];    // "" : ANY
      int  nRtpPort;               // 0: ANY, -1: use recv-socket.
      int  nRtcpPort;              // 0: ANY, -1: use recv-socket.
      int  nRtpBufLen;             // RTP Sender/Recever Socket buffer size.
                                   //  * 0: default=16384, else: min=256, max=262142 bytes
      int  nRtcpBufLen;             // RTCP Sender/Recever Socket buffer size.
                                   //  * 0: default:16384, else: min=256, max=262142 bytes
   };

   struct RTP_PARAM {
      // send-info
      CODEC_TYPE nSndCodecID;
      int nSndPayloadType;   // RTP payload-type
      int nSndPayloadSize;   // Maximum Send RTP payload size.
      int nInitSequence;     // Initial RTP Sequence
      int nSSRC;             // SSRC, -1 for auto-assignment.

      // recv-info 
      CODEC_TYPE nRcvCodecID;
      int nRcvPayloadType;   // -1 for SendPayloadType
      int nRcvPayloadSize;   // Maximum Recv RTP payload size

      // jitter buffer info
      BUFF_MODE nRcvBuffMode; // 
      int nDelayTime;         // in millisecond.  if 0, non-buffering
      int nTimeScale;         // 8000 for voice, 90000 for video and audio.
      int nMaxBufferCount;    // Of internal jitter buffer
      //int nMaxBufferSize;   // Of internal jitter buffer

      char cname[MAX_CNAME_LEN]; // for RTCP.
      IP_PORT remote;            // remote ip/port
   };

public:
   CRtpComm();
   ~CRtpComm();

   // pLocalRecvInfo: Local Recv Socket Info.
   // pLocalSendInfo: Local Send Socket Info.
   int OpenSocket(const IP_PORT *pLocalRecvInfo, const IP_PORT *pLocalSendInfo);
   int StartSession(const RTP_PARAM *pSendParam);
   int EndSession();
   int CloseSocket();

   void SetSendTimestampMode(bool bAuto=false); // default false

   // Wait for a incoming packet.
   //   - pnType   : type of packet, RTP or RTCP
   //   - nTimeout : wait-timeout in millisecond. 0=immediate, -1=unlimited.
   int WaitForPacket(PACKET_TYPE *pnType, int nTimeout);

   // of RTCP packet Info
   int GetReportCount();
   int GetReport(int nIndex, CRtcpReport::Report *pReport);

   // of RTP packet & Packet Info
   unsigned char * GetPacket(int *pnOutLength);
   int GetPacket(unsigned char* pOutData, int nInMax, int *pnOutLength);
   int GetPayload(unsigned char* pOutData, int nInMax, int *pnOutLength, int *pnMaker);
   // Information of the last-received RTP packet 
   int GetTimestamp();
   int GetSequence();
   int GetPayloadType();
   unsigned int GetSSRC();

   
   // rtp source (file or camera) has changed: needs to adjust timestamp.
   // nSSRC: new SSRC, -1 for auto-assignment.
   // bSetSendTimestamp : Sender RTP Audio Payload Type(auto-assignment timestamp)
   //int SourceChanged(int nSSRC, int nSndPayloadType, int nRcvPayloadType);
   int SourceChanged(int nRcvSSRC, CODEC_TYPE nSndCodecID, int nSndPayloadType, CODEC_TYPE nRcvCodecID, int nRcvPayloadType);

   // Sends RTP packet.
   int PutPacket(const unsigned char* pInData, int nInLength);
   int PutPayload(const unsigned char* pInData, int nInLength, unsigned int uTimestamp, int nMaker);

   // if bSetSendTimestamp  && nPayloadType == sendParam.nPayloadType, then auto-assignment timestamp
   int PutPayload(const unsigned char* pInData, int nInLength, unsigned int uTimestamp, int nMaker, int nPayloadType);


   // Full Intra-Frame Request
   // for manual
   int SetFIR(FIR_MODE nMode=FIRM_RTCP_H261FIR); 
   int SendFIR(FIR_MODE nMode=FIRM_RTCP_H261FIR);

protected:
   PACKET_TYPE CheckRtpPacket(const unsigned char * pPkt, int nLen);
   PACKET_TYPE CheckRtcpPacket(const unsigned char * pPkt, int nLen);
   //PACKET_TYPE ReceivePacket(int nTimeout); // block
   int CheckSendRtcpReport();
   int Clear();
   int ClearSession();
   int ClearRecvBuffer(int nTimeout);
   PACKET_TYPE Buffering(int nTimeout);
   PACKET_TYPE DirectRecv(int nTimeout);
   PACKET_TYPE SockSelect(int nTimeout);

   //int CheckVideoPacketLoss(const unsigned char * pPkt, int nLen);
   int CheckVideoPacketLoss();
   int SendRtcpH261FIR();
   int SendRtcpFbPLI();
protected:
   typedef struct {
      char ip[MAX_IP_ADDR];
      int  port;
   } PEER;

   typedef struct {
      int  sockfd;
      PEER local;
      PEER remote;
   } BIND;

   typedef struct {
      BIND snd;
      BIND rcv;
   } CHANNEL;

   CHANNEL m_Rtp;
   CHANNEL m_Rtcp;

   //CCritSec        m_Mutex;
   RTP_PARAM       m_Params;
   bool            m_bSockOpen;
   bool            m_bStarted;
   bool            m_bBuffered;

   unsigned char  *m_pRtpReceived;
   int             m_nRtpRecvSize;

   int             m_nSendSeqNumber;

   // RTP Receiver
   CAmRtpBuffer     *m_pRecvBuffer;
   // last received packet remote port & ip
   PEER            m_RtpRecvPeer;

   // RTCP 
   char            m_szCName[CRtcpReport::SDES_DATA_LEN_MAX];
   CRtcpReport    *m_pRtcpReport;
   struct timeval  m_tvRtcpPrevSendTime;
   int             m_nRtcpSendIntervalMs;
   PEER            m_RtcpRecvPeer;

   // QoS(FIR)
   FIR_MODE        m_nFirMode;
   bool            m_bVideo;
   unsigned int    m_uPrevRtpRcvSeq;
   bool            m_bReceivedIFrame;

   struct timeval  m_tvFIRSendTime;

   // udp temporary recv, send packet buffer 
   int             m_nMaxRecvPacketLen;
   int             m_nMaxSendPacketLen;
   unsigned char  *m_pRecvPacket;
   unsigned char  *m_pSendPacket;

   struct timeval  m_tvRtpFirstSendTime;
   bool            m_bAutoSetSendTimestamp; //auto set - timestamp of send-rtp packet 

};


}; //namespace

#endif   //__AMT_RTP_COMMON_H__


