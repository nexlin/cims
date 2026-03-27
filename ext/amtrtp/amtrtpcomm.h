#ifndef  __AMT_RTP_COMMON_H__
#define  __AMT_RTP_COMMON_H__

#include <sys/time.h>

#include <sstream>
#include <fstream>
#include <iostream>
#include "amtrtcp.h"
#include "amtrtp.h"
#include "CRtcp.h"
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
struct RTCPFBPLI
{
#if(HUMT_GCONF_BIG_ENDIAN)
   unsigned char  version:2;
   unsigned char  padding:1;
   unsigned char  fmt:5;
   unsigned char  payloadtype;
   unsigned short length;
#else
   unsigned char  fmt:5;
   unsigned char  padding:1;
   unsigned char  version:2;
   unsigned char  payloadtype;
   unsigned short length;
#endif
   unsigned int   pssrc;
   unsigned int   mssrc;
};

class CRtpComm
{
public:
   enum PACKET_TYPE 
   {
      PT_TIMEOUT      = 0x00,
      PT_RTP          = 0x10,
      PT_RTCP         = 0x20,

      PT_H261FIR      = 0x21,
      PT_FB_PLE       = 0x22,

      PT_RTP_DROP     = 0x1D,
      PT_RTCP_DROP    = 0x2D,

      PT_RTP_UNKNOWN  = 0x1E,
      PT_RTCP_UNKNOWN = 0x2E,

      PT_NOT_STARTED  = 0xFE,
      PT_ERROR        = 0xFF,
   };

   enum 
   { 
      MAX_IP_ADDR    = 16, 
      MAX_IP6_ADDR   = 128, 
      MAX_CNAME_LEN  = 256,
      MAX_UDP_PACKET_SIZE = 1600
   }; 

   enum FIR_MODE 
   {
      FIRM_MANUAL       = 0,   // default
      FIRM_RTCP_H261FIR = 1,
      FIRM_RTCP_FB_PLI  = 2,
   };
   
   enum BUFF_MODE 
   {
      BM_NONE    = 0,      // non-buffering
      BM_SEQ     = 1,    
      BM_TIME    = 2,
   };

   enum DUMP_MODE 
   {
      DUMP_NONE   = 0x0,
      DUMP_RELAYSRC = 0x1,
      DUMP_RELAYDST = 0x2,
      DUMP_RELAYALL = 0x3,
      DUMP_FILESRC  = 0x10,
      DUMP_FILEDST  = 0x20,
      DUMP_FILEALL  = 0x30,
      DUMP_ALLSRC   = DUMP_RELAYSRC | DUMP_FILESRC,
      DUMP_ALLDST   = DUMP_RELAYDST | DUMP_FILEDST,
      DUMP_ALL      = DUMP_RELAYALL | DUMP_FILEALL,
      DUMP_MAX
   };

   enum SELMODE 
   {
      SELMODE_NONE   =0,//default non select, read always RTP
      SELMODE_RTP    =1,
      SELMODE_RTCP   =2,
      SELMODE_BOTH   =3,
      NOSEL_RTP      =4,//non select, read always RTP
      NOSEL_RTCP     =5//non select, read always RTCP
   };//when select ,decision which socket inspect
   typedef struct 
   {
      int ver;
      char ip[MAX_IP_ADDR];
      char ip6[MAX_IP6_ADDR];
      int  port;
   } PEER;

   typedef struct 
   {
      int  sockfd;
      int  sockfd6;
      PEER local;
      PEER remote;
   } BIND;

   typedef struct 
   {
      BIND snd;
      BIND rcv;
   } CHANNEL;

   struct IP_PORT 
   {
      bool bRtpOn;                 // true: on
      bool bRtcpOn;                // true: on
      unsigned short usRtcpMode;   // 0:only socket open , 1:Send Report,Receive Report
      int ver;//1:ipv4 2:ipv6 3:ipv4 && ipv6
      char szAddr[MAX_IP_ADDR];    // "" : ANY
      char szAddr6[MAX_IP6_ADDR];    // "" : ANY
      int  nRtpPort;               // 0: ANY, -1: use recv-socket.
      int  nRtcpPort;              // 0: ANY, -1: use recv-socket.
      int  nRtpBufLen;             // RTP Sender/Recever Socket buffer size.
                                   //  * 0: default=16384, else: min=256, max=262142 bytes
      int  nRtcpBufLen;             // RTCP Sender/Recever Socket buffer size.
                                   //  * 0: default:16384, else: min=256, max=262142 bytes
   };

   struct RTP_PARAM 
   {
      // send-info
      //CODEC_TYPE nSndCodecID;
      int nSndPayloadType;   // RTP payload-type
      int nSndPayloadType2;  // RTP payload-type
      int nSndPayloadSize;   // Maximum Send RTP payload size.
      int nInitSequence;     // Initial RTP Sequence
      int nSSRC;             // SSRC, -1 for auto-assignment.

      // recv-info 
      //CODEC_TYPE nRcvCodecID;
      int nRcvPayloadType;   // -1 for SendPayloadType
      int nRcvPayloadType2;  // for dmtf rcv
      int nRcvPayloadSize;   // Maximum Recv RTP payload size

      // jitter buffer info
      BUFF_MODE nRcvBuffMode; // 
      int nDelayTime;         // in millisecond.  if 0, non-buffering
      int nTimeScale;         // 8000 for voice, 90000 for video and audio.
      int nMaxBufferCount;    // Of internal jitter buffer
      //int nMaxBufferSize;   // Of internal jitter buffer

      char cname[MAX_CNAME_LEN]; // for RTCP.
      unsigned char ucRmtNatCount; // remote nat count
      IP_PORT remote;            // remote ip/port
   };

   struct DUMP_PARAM 
   {
      int nDumpMode;
      PEER rcv_relay;
      PEER snd_relay;
      char szFileBase[128];
   };

public:
   CRtpComm();
   ~CRtpComm();

   // pLocalRecvInfo: Local Recv Socket Info.
   // pLocalSendInfo: Local Send Socket Info.
   int OpenSocket(const IP_PORT *pLocalRecvInfo, const IP_PORT *pLocalSendInfo=NULL, char* devname="");
   int StartSession(const RTP_PARAM *pSendParam);
   int StartSession_DUMP(const RTP_PARAM *pSendParam,const DUMP_PARAM *pDumpParam);
   int ModifySession(const RTP_PARAM *pSendParam);
   int ModifySession_DUMP(const RTP_PARAM *pSendParam,const DUMP_PARAM *pDumpParam);
   int EndSession();
   int CloseSocket();
   int SetDSCP(char rtpcode,char rtcpcode);
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
   unsigned char * GetSendPacket(int *pnOutLength);
   int GetSendPacket(unsigned char* pOutData, int nInMax, int *pnOutLength);
   int GetPayload(unsigned char* pOutData, int nInMax, int *pnOutLength,int *pnMarker);

   int GetPayload_Hdr(unsigned char* pOutData, int nInMax, int *pnOutLength,RTPHEADER *pDstHdr);

   // Information of the last-received RTP packet 
   int GetTimestamp();
   int GetSequence();
   unsigned char * GetRtcpPacket(int *pnOutLength);
   unsigned char * GetRtcpSendPacket(int *pnOutLength);

// Add by Cho for GUI
   inline int GetSendSequence(){ return m_nSendSeqNumber;}

   int GetPayloadType();
   unsigned int GetSSRC();

   
   // rtp source (file or camera) has changed: needs to adjust timestamp.
   // nSSRC: new SSRC, -1 for auto-assignment.
   // bSetSendTimestamp : Sender RTP Audio Payload Type(auto-assignment timestamp)
   //int SourceChanged(int nSSRC, int nSndPayloadType, int nRcvPayloadType);
   //int SourceChanged(int nRcvSSRC, CODEC_TYPE nSndCodecID, int nSndPayloadType, CODEC_TYPE nRcvCodecID, int nRcvPayloadType,int nRcvPayloadType2=0);
   int SourceChanged(int nRcvSSRC, int nSndPayloadType, int nRcvPayloadType,int nRcvPayloadType2=0);

   // Sends RTP packet.
   int PutPacket(const unsigned char* pInData, int nInLength);
   int PutPacketWithLastTimestamp(const unsigned char* pInData, int nInLength);
   int PutPayload(const unsigned char* pInData, int nInLength, unsigned int uTimestamp, int nMaker,int nSeqNumber=0);

   // if bSetSendTimestamp  && nPayloadType == sendParam.nPayloadType, then auto-assignment timestamp
   int PutPayload_(const unsigned char* pInData, int nInLength, unsigned int uTimestamp, int nMaker, int nPayloadType,int nSeqNumber=0);
   int PutPayloadWithLastTimestamp(const unsigned char* pInData, int nInLength, int nMaker, int nPayloadType);

   // Full Intra-Frame Request
   // for manual
   int SetFIR(FIR_MODE nMode=FIRM_RTCP_H261FIR); 
   int SendFIR(FIR_MODE nMode=FIRM_RTCP_H261FIR);
   inline bool IsStarted(){return m_bStarted;}
   PACKET_TYPE Buffering(int nTimeout,int selmode=0);//mode 0:none return PT_RTP 1:rtp only 2:rtcp only 3:rtp + rtcp
   PACKET_TYPE FlushRecv(int nTimeout,int selmode=0);//mode 0:none return PT_RTP 1:rtp only 2:rtcp only 3:rtp + rtcp
   PACKET_TYPE DirectRecv(int nTimeout,int selmode=0);//mode 0:none return PT_RTP 1:rtp only 2:rtcp only 3:rtp + rtcp
   //PACKET_TYPE DirectRecv(char* int nTimeout,bool int mode=0);
   PACKET_TYPE SockSelect(int nTimeout,int mode=3);//mode 0:none return PT_RTP 1:rtp only 2:rtcp only 3:rtp + rtcp
   int GetRTPRcvSocketFd(){return m_Rtp.rcv.sockfd;}
   int GetRTPSndSocketFd(){return m_Rtp.snd.sockfd;}
   int GetRTCPRcvSocketFd(){return m_Rtcp.rcv.sockfd;}
   int GetRTCPSndSocketFd(){return m_Rtcp.snd.sockfd;}
   unsigned int GetSendTimestamp(){return m_uSendTimestamp;}
   int Timming_RtcpSR(unsigned long timestamp,int rtcpmode,int interval=0);
   int Timming_RtcpRR(unsigned long timestamp,int rtcpmode,int interval=0);
protected:
   PACKET_TYPE CheckRtpPacket(const unsigned char * pPkt, int nLen);
   PACKET_TYPE CheckRtcpPacket(const unsigned char * pPkt, int nLen);
   //PACKET_TYPE ReceivePacket(int nTimeout); // block
   int CheckSendRtcpReport();
   int Clear();
   int ClearSession();
   int ClearRecvBuffer(int nTimeout);
#ifdef USING_POLL
   PACKET_TYPE SockPoll(int nTimeout,int mode=3);//mode 0:none return PT_RTP 1:rtp only 2:rtcp only 3:rtp + rtcp
#endif

   //int CheckVideoPacketLoss(const unsigned char * pPkt, int nLen);
   int CheckVideoPacketLoss();
   int SendRtcpH261FIR();
   int SendRtcpFbPLI();
   int RTCPSendSR(int mode);
   int RTCPSendRR(int mode);
protected:

   CHANNEL m_Rtp;
   CHANNEL m_Rtcp;

   //CCritSec        m_Mutex;
   RTP_PARAM       m_Params;
   bool            m_bSockOpen;
   bool            m_bStarted;
   bool            m_bBuffered;

   unsigned char  *m_pRtpReceived;
   int             m_nRtpRecvSize;
   int             m_nSendPacketSize;
   int             m_nRtcpRecvSize;
   int             m_nRtcpSendPacketSize;
   int             m_nSendSeqNumber;
   unsigned int    m_uSendTimestamp;

   // RTP Receiver
   CAmRtpBuffer     *m_pRecvBuffer;
   // last received packet remote port & ip
   PEER            m_RtpTarget; // rtp target ip/port
   unsigned int    m_uRtpCount; // rtc remote nat count

   // RTCP 
   unsigned short  m_usRtcpMode;//0:just receive 1:SR,RR
   char            m_szCName[CRtcpReport::SDES_DATA_LEN_MAX];
   //CRtcpReport    *m_pRtcpReport;
   CRtcpCtx        m_RtcpCtx;
   unsigned long   m_RtcpSRSendLastMS;
   unsigned long   m_RtcpRRSendLastMS;

   struct timeval  m_tvRtcpPrevSendTime;
   int             m_nRtcpSendIntervalMs;
   //PEER            m_RtcpRecvPeer;
   PEER            m_RtcpTarget; // rtcp target ip/port
   unsigned int    m_uRtcpCount; // rtcp remote nat count

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
   unsigned char  *m_pRtcpRecvPacket;
   unsigned char  *m_pRtcpSendPacket;
   struct timeval  m_tvRtpFirstSendTime;
   bool            m_bAutoSetSendTimestamp; //auto set - timestamp of send-rtp packet 
//-- rtp packet dump with rff format
   int      m_nDumpMode;
   PEER     m_DumpRcvRelay;
   PEER     m_DumpSndRelay;
   std::string m_strRcvDmpFileName;
   std::string m_strSndDmpFileName;
   std::ofstream m_RcvFBuf;
   std::ofstream m_SndFBuf;
//--

};


}; //namespace

#endif   //__AMT_RTP_COMMON_H__


