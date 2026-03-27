/**
  *
 **/
#ifndef  __AMT_ATM_COMMON_H__
#define  __AMT_ATM_COMMON_H__

#if defined(_MSC_VER)
//#include "pdkutil.h"
#else
#include <sys/time.h>
//#include "util.h"
#endif

#include <sstream>
#include <fstream>
#include <iostream>
//#include "jthread.h"
#include "mtcodectype.h"

namespace AMT
{

class CATMComm
{
public:
   enum { 
      MAX_IP_ADDR   = 16,
      MAX_IP6_ADDR   = 128,
		MAX_UDP_PACKET_SIZE = 1600
   }; 

	enum DUMP_MODE {
		DUMP_NONE	= 0x0,
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

   typedef struct {
		int ver;//1: ipv4 2:ipv6 3:ipv4 && ipv6
      char ip[MAX_IP_ADDR];
      char ip6[MAX_IP6_ADDR];
      int  port;
   } PEER;

   typedef struct {
      int  sockfd;
      int  sockfd6;
      PEER local;
      PEER remote;
   } BIND;

   typedef struct {
     	PEER remote; 
   } ATM_PARAM;

	struct DUMP_PARAM {
		int nDumpMode;
		PEER rcv_relay;
		PEER snd_relay;
		char szFileBase[128];
	};

public:
   CATMComm();
   ~CATMComm();

   // pLocalRecvInfo: Local Recv Socket Info.
   // pLocalSendInfo: Local Send Socket Info.
   int OpenSocket(const PEER *pLocalRecvInfo);
   int StartSession(const ATM_PARAM *pSendParam);
   int StartSession_DUMP(const ATM_PARAM *pSendParam,const DUMP_PARAM *pDumpParam);
   int ModifySession(const ATM_PARAM *pSendParam);
   int ModifySession_DUMP(const ATM_PARAM *pSendParam,const DUMP_PARAM *pDumpParam);
   int EndSession();
   int CloseSocket();

   // of ATM packet & Packet Info
   unsigned char * GetPacket(int *pnOutLength);
	unsigned char * GetSendPacket(int *pnOutLength);
   // Information of the last-received ATM packet 
   int PutPacket(const unsigned char* pInData, int nInLength);
   inline bool IsStarted(){return m_bStarted;}
   int DirectRecv(int nTimeout,bool bepoll=false);
   int SockSelect(int nTimeout);
	inline int GetSocket(int ver=1){if(ver==1) return m_sock.sockfd; else return m_sock.sockfd6;}
protected:
   int Clear();
   int ClearSession();
#ifdef USING_POLL
   int SockPoll(int nTimeout);
#endif
protected:

	BIND m_sock;
   //CCritSec        m_Mutex;
   ATM_PARAM       m_Params;
   bool            m_bStarted;

   unsigned char  *m_pRtpReceived;
   int             m_nRtpRecvSize;
	int				 m_nSendPacketSize;

   // udp temporary recv, send packet buffer 
   static int             m_nMaxRecvPacketLen;
   static int             m_nMaxSendPacketLen;
   unsigned char  *m_pRecvPacket;
   unsigned char  *m_pSendPacket;

//-- rtp packet dump with rff format
   int	m_nDumpMode;
	PEER 		m_DumpRcvRelay;
	PEER		m_DumpSndRelay;
   std::string m_strRcvDmpFileName;
   std::string	m_strSndDmpFileName;
   std::ofstream m_RcvFBuf;
   std::ofstream m_SndFBuf;
//--

};


}; //namespace

#endif   //__AMT_ATM_COMMON_H__

////@}
