
#include <stdlib.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include "amtsock.h"
#include "amrtpbuffer.h"
#include "amtrtpcomm.h"
//#include "humterr.h"
#include "util.h"

#define AMT_RTP_TICKER_INTERVAL  10
#define HAV_RTP_TICKER_DEBUG     0
#define RTCP_INTERVAL_MS			5000


namespace AMT
{


#ifndef SAFE_DELETE_OBJECT
#define SAFE_DELETE_OBJECT(p) \
   if(p != NULL) { \
      delete p; \
      p = NULL; \
   }
#endif

#ifndef SAFE_DELETE_ARRAY
#define SAFE_DELETE_ARRAY(p) \
   if(p != NULL) { \
      delete[] p; \
      p = NULL; \
   }
#endif

static int InitRtpComm()
{
   srand(time(NULL)+36219371);
   return 0;
}

static int rcInitRtpComm = InitRtpComm();


CRtpComm::CRtpComm()
{
   m_nMaxRecvPacketLen = CRtpComm::MAX_UDP_PACKET_SIZE;
   m_nMaxSendPacketLen = CRtpComm::MAX_UDP_PACKET_SIZE;

   m_pRecvPacket = new unsigned char[CRtpComm::MAX_UDP_PACKET_SIZE];
   m_pSendPacket = new unsigned char[CRtpComm::MAX_UDP_PACKET_SIZE];
   m_pRtcpRecvPacket = new unsigned char[CRtpComm::MAX_UDP_PACKET_SIZE];
   m_pRtcpSendPacket = new unsigned char[CRtpComm::MAX_UDP_PACKET_SIZE];
   
   m_pRecvBuffer =  new CAmRtpBuffer;
   //m_pRtcpReport = new CRtcpReport;
   m_bSockOpen = false;
   m_bStarted  = false;
   m_pRtpReceived = NULL;
   memset(&m_Rtp,  0, sizeof(CHANNEL));
   m_Rtp.snd.sockfd = INVALID_SOCKET;
   m_Rtp.rcv.sockfd = INVALID_SOCKET;
   m_Rtp.snd.sockfd6 = INVALID_SOCKET;
   m_Rtp.rcv.sockfd6 = INVALID_SOCKET;
   memset(&m_Rtcp, 0, sizeof(CHANNEL));
   m_Rtcp.snd.sockfd = INVALID_SOCKET;
   m_Rtcp.rcv.sockfd = INVALID_SOCKET;
   m_Rtcp.snd.sockfd6 = INVALID_SOCKET;
   m_Rtcp.rcv.sockfd6 = INVALID_SOCKET;
   m_nRtcpSendIntervalMs = 5000;
  	m_usRtcpMode = 0;	 
   m_bAutoSetSendTimestamp = false;
	m_uSendTimestamp = 0;
   memset(&m_RtpTarget, 0, sizeof(PEER));
   memset(&m_RtcpTarget, 0, sizeof(PEER));
   m_uRtpCount = 0;
   m_uRtcpCount = 0;
}

CRtpComm::~CRtpComm()
{
   Clear();
#if 0   //abbriate delete procedurre when proc down
   if(m_pRecvPacket) {
      delete [] m_pRecvPacket;
      m_pRecvPacket = NULL;
   } 
   if(m_pSendPacket) {
      delete [] m_pSendPacket;
      m_pSendPacket = NULL;
   } 
   if(m_pRecvBuffer) delete m_pRecvBuffer;
#endif
   //if(m_pRtcpReport) delete m_pRtcpReport;
}

int CRtpComm::Clear()
{
   ClearSession();
   CAmtSock::Close(&m_Rtp.rcv.sockfd);
   CAmtSock6::Close(&m_Rtp.rcv.sockfd6);
   if(m_Rtp.snd.sockfd != m_Rtp.rcv.sockfd) {
      CAmtSock::Close(&m_Rtp.snd.sockfd);
   }
   if(m_Rtp.snd.sockfd6 != m_Rtp.rcv.sockfd6) {
      CAmtSock6::Close(&m_Rtp.snd.sockfd6);
   }
   CAmtSock::Close(&m_Rtcp.rcv.sockfd);
   CAmtSock6::Close(&m_Rtcp.rcv.sockfd6);
   if(m_Rtcp.snd.sockfd != m_Rtcp.rcv.sockfd) {
      CAmtSock::Close(&m_Rtcp.snd.sockfd);
   }
   if(m_Rtcp.snd.sockfd6 != m_Rtcp.rcv.sockfd6) {
      CAmtSock6::Close(&m_Rtcp.snd.sockfd6);
   }
   memset(&m_Rtp,  0, sizeof(CHANNEL));
   m_Rtp.snd.sockfd = INVALID_SOCKET;
   m_Rtp.rcv.sockfd = INVALID_SOCKET;
   m_Rtp.snd.sockfd6 = INVALID_SOCKET;
   m_Rtp.rcv.sockfd6 = INVALID_SOCKET;
   memset(&m_Rtcp, 0, sizeof(CHANNEL));
   m_Rtcp.snd.sockfd = INVALID_SOCKET;
   m_Rtcp.rcv.sockfd = INVALID_SOCKET;
   m_Rtcp.snd.sockfd6 = INVALID_SOCKET;
   m_Rtcp.rcv.sockfd6 = INVALID_SOCKET;
	m_usRtcpMode = 0;
   m_pRtpReceived = NULL;
   m_bSockOpen = false;
	m_uSendTimestamp = 0;
	m_RtcpSRSendLastMS = m_RtcpRRSendLastMS = 0;
   memset(&m_RtpTarget, 0, sizeof(PEER));
   memset(&m_RtcpTarget, 0, sizeof(PEER));
   m_uRtpCount = 0;
   m_uRtcpCount = 0;
   return 0;
}

int CRtpComm::OpenSocket(const IP_PORT *pLocalRecvInfo, const IP_PORT *pLocalSendInfo,char* devname)
{
   int rc = 0;

	if(!pLocalRecvInfo)	return -1;
   //m_Mutex.Lock();
   Clear();
	
	switch(pLocalRecvInfo->ver)
	{
	case 3: //ipv6 & 4
      if(pLocalRecvInfo->bRtpOn) 
      {
         strcpy(m_Rtp.rcv.local.ip6, pLocalRecvInfo->szAddr6);
         m_Rtp.rcv.local.port = pLocalRecvInfo->nRtpPort;
         m_Rtp.rcv.sockfd6 = CAmtSock6::Socket(CAmtSock::UDP);
         if(m_Rtp.rcv.sockfd6 == INVALID_SOCKET) 
         {
            rc = -1;
            break;
         }
         if(CAmtSock6::Bind(m_Rtp.rcv.sockfd6, devname, m_Rtp.rcv.local.ip6, m_Rtp.rcv.local.port) == false) 
         {
            rc = -2;
            break;
         }
         CAmtSock6::SetRecvBufferSize(m_Rtp.rcv.sockfd6, pLocalRecvInfo->nRtpBufLen);
      } 
      if(pLocalRecvInfo->bRtcpOn) 
      {
         strcpy(m_Rtcp.rcv.local.ip6, pLocalRecvInfo->szAddr6);
         m_Rtcp.rcv.local.port = pLocalRecvInfo->nRtcpPort;
         m_Rtcp.rcv.sockfd6 = CAmtSock6::Socket(CAmtSock::UDP);
         if(m_Rtcp.rcv.sockfd6 == INVALID_SOCKET) 
         {
            rc = -3;
            break;
         }
         if(CAmtSock6::Bind(m_Rtcp.rcv.sockfd6, devname, m_Rtcp.rcv.local.ip6, m_Rtcp.rcv.local.port) == false) 
         {
            rc = -4;
            break;
         }
         CAmtSock6::SetRecvBufferSize(m_Rtcp.rcv.sockfd6, pLocalRecvInfo->nRtcpBufLen);
      }
      if(pLocalRecvInfo->bRtpOn) 
      {
         strcpy(m_Rtp.rcv.local.ip, pLocalRecvInfo->szAddr);
         m_Rtp.rcv.local.port = pLocalRecvInfo->nRtpPort;
         m_Rtp.rcv.sockfd = CAmtSock::Socket(CAmtSock::UDP);
         if(m_Rtp.rcv.sockfd == INVALID_SOCKET) 
         {
            rc = -5;
            break;
         }
         if(CAmtSock::Bind(m_Rtp.rcv.sockfd, m_Rtp.rcv.local.ip, m_Rtp.rcv.local.port) == false) 
         {
            rc = -6;
            break;
         }
         CAmtSock::SetRecvBufferSize(m_Rtp.rcv.sockfd, pLocalRecvInfo->nRtpBufLen);
      } 
      if(pLocalRecvInfo->bRtcpOn) 
      {
         strcpy(m_Rtcp.rcv.local.ip, pLocalRecvInfo->szAddr);
         m_Rtcp.rcv.local.port = pLocalRecvInfo->nRtcpPort;
         m_Rtcp.rcv.sockfd = CAmtSock::Socket(CAmtSock::UDP);
         if(m_Rtcp.rcv.sockfd == INVALID_SOCKET) 
         {
            rc = -7;
            break;
         }
         if(CAmtSock::Bind(m_Rtcp.rcv.sockfd, m_Rtcp.rcv.local.ip, m_Rtcp.rcv.local.port) == false) 
         {
            rc = -8;
            break;
         }
         CAmtSock::SetRecvBufferSize(m_Rtcp.rcv.sockfd, pLocalRecvInfo->nRtcpBufLen);
      }

	break;
	case 2: // ipv6
      if(pLocalRecvInfo->bRtpOn) 
      {
         strcpy(m_Rtp.rcv.local.ip6, pLocalRecvInfo->szAddr6);
         m_Rtp.rcv.local.port = pLocalRecvInfo->nRtpPort;
         m_Rtp.rcv.sockfd6 = CAmtSock6::Socket(CAmtSock::UDP);
         if(m_Rtp.rcv.sockfd6 == INVALID_SOCKET) 
         {
            rc = -9;
            break;
         }
         if(CAmtSock6::Bind(m_Rtp.rcv.sockfd6, devname,m_Rtp.rcv.local.ip6, m_Rtp.rcv.local.port) == false) 
         {
            printf("ip6 %ld\n",m_Rtp.rcv.local.ip6);
            rc = -10;
            break;
         }
         CAmtSock6::SetRecvBufferSize(m_Rtp.rcv.sockfd6, pLocalRecvInfo->nRtpBufLen);
      } 
      if(pLocalRecvInfo->bRtcpOn) 
      {
         strcpy(m_Rtcp.rcv.local.ip6, pLocalRecvInfo->szAddr6);
         m_Rtcp.rcv.local.port = pLocalRecvInfo->nRtcpPort;
         m_Rtcp.rcv.sockfd6 = CAmtSock6::Socket(CAmtSock::UDP);
         if(m_Rtcp.rcv.sockfd6 == INVALID_SOCKET) 
         {
            rc = -11;
            break;
         }
         if(CAmtSock6::Bind(m_Rtcp.rcv.sockfd6, devname,m_Rtcp.rcv.local.ip6, m_Rtcp.rcv.local.port) == false) 
         {
            rc = -12;
            break;
         }
         CAmtSock6::SetRecvBufferSize(m_Rtcp.rcv.sockfd6, pLocalRecvInfo->nRtcpBufLen);
      }

	break;
	case 1: // ipv4
	default:
      if(pLocalRecvInfo->bRtpOn) 
      {
         strcpy(m_Rtp.rcv.local.ip, pLocalRecvInfo->szAddr);
         m_Rtp.rcv.local.port = pLocalRecvInfo->nRtpPort;
         m_Rtp.rcv.sockfd = CAmtSock::Socket(CAmtSock::UDP);
         if(m_Rtp.rcv.sockfd == INVALID_SOCKET) 
         {
            rc = -13;
            break;
         }
         if(CAmtSock::Bind(m_Rtp.rcv.sockfd, m_Rtp.rcv.local.ip, m_Rtp.rcv.local.port) == false) 
         {
            rc = -14;
            break;
         }
         CAmtSock::SetRecvBufferSize(m_Rtp.rcv.sockfd, pLocalRecvInfo->nRtpBufLen);
      } 
      if(pLocalRecvInfo->bRtcpOn) 
      {
         strcpy(m_Rtcp.rcv.local.ip, pLocalRecvInfo->szAddr);
         m_Rtcp.rcv.local.port = pLocalRecvInfo->nRtcpPort;
         m_Rtcp.rcv.sockfd = CAmtSock::Socket(CAmtSock::UDP);
         if(m_Rtcp.rcv.sockfd == INVALID_SOCKET) 
         {
            rc = -15;
            break;
         }
         if(CAmtSock::Bind(m_Rtcp.rcv.sockfd, m_Rtcp.rcv.local.ip, m_Rtcp.rcv.local.port) == false) 
         {
            rc = -16;
            break;
         }
         CAmtSock::SetRecvBufferSize(m_Rtcp.rcv.sockfd, pLocalRecvInfo->nRtcpBufLen);
      }

	break;
	}

	if(pLocalSendInfo)
	{
		switch(pLocalSendInfo->ver)
		{
		case 3:
			if(pLocalSendInfo->bRtpOn) 
         {
				//strcpy(m_Rtp.snd.local.ip, pLocalSendInfo->szAddr);
				strcpy(m_Rtp.snd.local.ip6, pLocalRecvInfo->szAddr6);      
			   m_Rtp.snd.local.port = pLocalSendInfo->nRtpPort;
				if(m_Rtp.snd.local.port == -1) 
            { // use recv socket.
					if(m_Rtp.rcv.sockfd6 != INVALID_SOCKET) 
               {
						m_Rtp.snd.sockfd6 = m_Rtp.rcv.sockfd6;
					} 
               else 
               {
						rc = -17;
						break;
					}
				} 
            else 
            {
					m_Rtp.snd.sockfd6 = CAmtSock6::Socket(CAmtSock::UDP);
					if(m_Rtp.snd.sockfd6 == INVALID_SOCKET) 
               {
						rc = -18;
						break;
					}
					if(m_Rtp.snd.local.port > 0) 
               { //else no binding
						if(CAmtSock6::Bind(m_Rtp.snd.sockfd6, devname,m_Rtp.snd.local.ip6, m_Rtp.snd.local.port) == false) 
                  {
							rc = -19;
							break;
						}
					} 
				}
				if(m_Rtp.snd.sockfd6 != INVALID_SOCKET) 
            {
					CAmtSock::SetSendBufferSize(m_Rtp.snd.sockfd6, pLocalSendInfo->nRtpBufLen);
				}
			} 

			if(pLocalSendInfo->bRtcpOn) 
         {
				//strcpy(m_Rtcp.snd.local.ip, pLocalSendInfo->szAddr);
				strcpy(m_Rtcp.snd.local.ip6, pLocalRecvInfo->szAddr6);
				m_Rtcp.snd.local.port = pLocalSendInfo->nRtcpPort;
				if(m_Rtcp.snd.local.port == -1) 
            { // use recv socket.
					if(m_Rtcp.rcv.sockfd6 != INVALID_SOCKET) 
               {
						m_Rtcp.snd.sockfd6 = m_Rtcp.rcv.sockfd6;
					} 
               else 
               {
						rc = -20;
						break;
					}
				} 
            else 
            {
					m_Rtcp.snd.sockfd6 = CAmtSock::Socket(CAmtSock::UDP);
					if(m_Rtcp.snd.sockfd6 == INVALID_SOCKET) 
               {
						rc = -21;
						break;
					}
					if(m_Rtcp.snd.local.port > 0) 
               { //else no binding
						if(CAmtSock6::Bind(m_Rtcp.snd.sockfd6, devname,m_Rtcp.snd.local.ip6, m_Rtcp.snd.local.port) == false) 
                  {
							rc = -22;
							break;
						}
					}
				}
				if(m_Rtcp.snd.sockfd6 != INVALID_SOCKET) 
            {
					CAmtSock::SetSendBufferSize(m_Rtcp.snd.sockfd6, pLocalSendInfo->nRtcpBufLen);
				}
			} 
			if(pLocalSendInfo->bRtpOn) 
         {
				//strcpy(m_Rtp.snd.local.ip, pLocalSendInfo->szAddr);
				strcpy(m_Rtp.snd.local.ip, pLocalRecvInfo->szAddr);      
			   m_Rtp.snd.local.port = pLocalSendInfo->nRtpPort;
				if(m_Rtp.snd.local.port == -1) 
            { // use recv socket.
					if(m_Rtp.rcv.sockfd != INVALID_SOCKET) 
               {
						m_Rtp.snd.sockfd = m_Rtp.rcv.sockfd;
					} 
               else 
               {
						rc = -23;
						break;
					}
				} 
            else 
            {
					m_Rtp.snd.sockfd = CAmtSock::Socket(CAmtSock::UDP);
					if(m_Rtp.snd.sockfd == INVALID_SOCKET) 
               {
						rc = -24;
						break;
					}
					if(m_Rtp.snd.local.port > 0) 
               { //else no binding
						if(CAmtSock::Bind(m_Rtp.snd.sockfd, m_Rtp.snd.local.ip, m_Rtp.snd.local.port) == false) 
                  {
							rc = -25;
							break;
						}
					} 
				}
				if(m_Rtp.snd.sockfd != INVALID_SOCKET) 
            {
					CAmtSock::SetSendBufferSize(m_Rtp.snd.sockfd, pLocalSendInfo->nRtpBufLen);
				}
			} 

			if(pLocalSendInfo->bRtcpOn) 
         {
				//strcpy(m_Rtcp.snd.local.ip, pLocalSendInfo->szAddr);
				strcpy(m_Rtcp.snd.local.ip, pLocalRecvInfo->szAddr);
				m_Rtcp.snd.local.port = pLocalSendInfo->nRtcpPort;
				if(m_Rtcp.snd.local.port == -1) 
            { // use recv socket.
					if(m_Rtcp.rcv.sockfd != INVALID_SOCKET) 
               {
						m_Rtcp.snd.sockfd = m_Rtcp.rcv.sockfd;
					} 
               else 
               {
						rc = -26;
						break;
					}
				} 
            else 
            {
					m_Rtcp.snd.sockfd = CAmtSock::Socket(CAmtSock::UDP);
					if(m_Rtcp.snd.sockfd == INVALID_SOCKET) 
               {
						rc = -27;
						break;
					}
					if(m_Rtcp.snd.local.port > 0) 
               { //else no binding
						if(CAmtSock::Bind(m_Rtcp.snd.sockfd, m_Rtcp.snd.local.ip, m_Rtcp.snd.local.port) == false) 
                  {
							rc = -28;
							break;
						}
					}
				}
				if(m_Rtcp.snd.sockfd != INVALID_SOCKET) 
            {
					CAmtSock::SetSendBufferSize(m_Rtcp.snd.sockfd, pLocalSendInfo->nRtcpBufLen);
				}
			} 
		break;
		case 2:
			if(pLocalSendInfo->bRtpOn) 
         {
				//strcpy(m_Rtp.snd.local.ip, pLocalSendInfo->szAddr);
				strcpy(m_Rtp.snd.local.ip6, pLocalRecvInfo->szAddr6);      
			    m_Rtp.snd.local.port = pLocalSendInfo->nRtpPort;
				if(m_Rtp.snd.local.port == -1) 
            { // use recv socket.
					if(m_Rtp.rcv.sockfd6 != INVALID_SOCKET) 
               {
						m_Rtp.snd.sockfd6 = m_Rtp.rcv.sockfd6;
					} 
               else 
               {
						rc = -29;
						break;
					}
				} 
            else 
            {
					m_Rtp.snd.sockfd6 = CAmtSock6::Socket(CAmtSock::UDP);
					if(m_Rtp.snd.sockfd6 == INVALID_SOCKET) 
               {
						rc = -30;
						break;
					}
					if(m_Rtp.snd.local.port > 0) 
               { //else no binding
						if(CAmtSock6::Bind(m_Rtp.snd.sockfd6, devname,m_Rtp.snd.local.ip6, m_Rtp.snd.local.port) == false) 
                  {
							rc = -31;
							break;
						}
					} 
				}
				if(m_Rtp.snd.sockfd6 != INVALID_SOCKET) 
            {
					CAmtSock::SetSendBufferSize(m_Rtp.snd.sockfd6, pLocalSendInfo->nRtpBufLen);
				}
			} 

			if(pLocalSendInfo->bRtcpOn) 
         {
				//strcpy(m_Rtcp.snd.local.ip, pLocalSendInfo->szAddr);
				strcpy(m_Rtcp.snd.local.ip6, pLocalRecvInfo->szAddr6);
				m_Rtcp.snd.local.port = pLocalSendInfo->nRtcpPort;
				if(m_Rtcp.snd.local.port == -1) 
            { // use recv socket.
					if(m_Rtcp.rcv.sockfd6 != INVALID_SOCKET) 
               {
						m_Rtcp.snd.sockfd6 = m_Rtcp.rcv.sockfd6;
					} 
               else 
               {
						rc = -32;
						break;
					}
				} 
            else 
            {
					m_Rtcp.snd.sockfd6 = CAmtSock::Socket(CAmtSock::UDP);
					if(m_Rtcp.snd.sockfd6 == INVALID_SOCKET) 
               {
						rc = -33;
						break;
					}
					if(m_Rtcp.snd.local.port > 0) 
               { //else no binding
						if(CAmtSock6::Bind(m_Rtcp.snd.sockfd6, devname,m_Rtcp.snd.local.ip6, m_Rtcp.snd.local.port) == false) 
                  {
							rc = -34;
							break;
						}
					}
				}
				if(m_Rtcp.snd.sockfd6 != INVALID_SOCKET) 
            {
					CAmtSock::SetSendBufferSize(m_Rtcp.snd.sockfd6, pLocalSendInfo->nRtcpBufLen);
				}
			} 
		break;
		case 1:
		default:
			if(pLocalSendInfo->bRtpOn) 
         {
				//strcpy(m_Rtp.snd.local.ip, pLocalSendInfo->szAddr);
				strcpy(m_Rtp.snd.local.ip, pLocalRecvInfo->szAddr);      
			   m_Rtp.snd.local.port = pLocalSendInfo->nRtpPort;
				if(m_Rtp.snd.local.port == -1) 
            { // use recv socket.
					if(m_Rtp.rcv.sockfd != INVALID_SOCKET) 
               {
						m_Rtp.snd.sockfd = m_Rtp.rcv.sockfd;
					} 
               else 
               {
						rc = -35;
						break;
					}
				} 
            else 
            {
					m_Rtp.snd.sockfd = CAmtSock::Socket(CAmtSock::UDP);
					if(m_Rtp.snd.sockfd == INVALID_SOCKET) 
               {
						rc = -36;
						break;
					}
					if(m_Rtp.snd.local.port > 0) 
               { //else no binding
						if(CAmtSock::Bind(m_Rtp.snd.sockfd, m_Rtp.snd.local.ip, m_Rtp.snd.local.port) == false) 
                  {
							rc = -37;
							break;
						}
					} 
				}
				if(m_Rtp.snd.sockfd != INVALID_SOCKET) 
            {
					CAmtSock::SetSendBufferSize(m_Rtp.snd.sockfd, pLocalSendInfo->nRtpBufLen);
				}
			} 

			if(pLocalSendInfo->bRtcpOn) 
         {
				//strcpy(m_Rtcp.snd.local.ip, pLocalSendInfo->szAddr);
				strcpy(m_Rtcp.snd.local.ip, pLocalRecvInfo->szAddr);
				m_Rtcp.snd.local.port = pLocalSendInfo->nRtcpPort;
				if(m_Rtcp.snd.local.port == -1) 
            { // use recv socket.
					if(m_Rtcp.rcv.sockfd != INVALID_SOCKET) 
               {
						m_Rtcp.snd.sockfd = m_Rtcp.rcv.sockfd;
					} 
               else 
               {
						rc = -38;
						break;
					}
				} 
            else 
            {
					m_Rtcp.snd.sockfd = CAmtSock::Socket(CAmtSock::UDP);
					if(m_Rtcp.snd.sockfd == INVALID_SOCKET) 
               {
						rc = -39;
						break;
					}
					if(m_Rtcp.snd.local.port > 0) 
               { //else no binding
						if(CAmtSock::Bind(m_Rtcp.snd.sockfd, m_Rtcp.snd.local.ip, m_Rtcp.snd.local.port) == false) 
                  {
							rc = -40;
							break;
						}
					}
				}
				if(m_Rtcp.snd.sockfd != INVALID_SOCKET) 
            {
					CAmtSock::SetSendBufferSize(m_Rtcp.snd.sockfd, pLocalSendInfo->nRtcpBufLen);
				}
			} 
		break;
		}//switch
	}
	else
	{
		do {
			if(pLocalRecvInfo->ver == 1 || pLocalRecvInfo->ver == 3)
			{
				if(m_Rtp.rcv.sockfd != INVALID_SOCKET) 
            {
					m_Rtp.snd.sockfd = m_Rtp.rcv.sockfd;
				} 
            else 
            {
					rc = -41;
					break;
				}
				if(m_Rtp.snd.sockfd != INVALID_SOCKET) 
            {
					CAmtSock::SetSendBufferSize(m_Rtp.snd.sockfd, 0);
				}
			}

			if(pLocalRecvInfo->ver == 2 || pLocalRecvInfo->ver == 3)
			{
				if(m_Rtp.rcv.sockfd6 != INVALID_SOCKET) 
            {
					m_Rtp.snd.sockfd6 = m_Rtp.rcv.sockfd6;
				} 
            else 
            {
					rc = -42;
					break;
				}
				if(m_Rtp.snd.sockfd6 != INVALID_SOCKET) 
            {
					CAmtSock::SetSendBufferSize(m_Rtp.snd.sockfd6, 0);
				}
			}
		}while(false);
	}

   if(rc == 0) 
   {
      m_bSockOpen = true;
   } 
   else 
   {
      Clear();
   }
   //m_Mutex.Unlock();
   return rc;
}

int CRtpComm::SetDSCP(char rtpcode,char rtcpcode)
{
	int retc = 0;
   unsigned int tos ;
	int rcv_sockfd = (m_Rtp.rcv.remote.ver == 1)?m_Rtp.rcv.sockfd:m_Rtp.rcv.sockfd6;
	int snd_sockfd = (m_Rtp.snd.remote.ver == 1)?m_Rtp.snd.sockfd:m_Rtp.snd.sockfd6;

    if(rtpcode)
    {
        if(rcv_sockfd>=0)
        {
            tos = rtpcode ;
            tos = tos <<2 ;
            retc|=setsockopt(
                     rcv_sockfd,
#ifndef WIN32
                     IPPROTO_IP, IP_TOS, (const void *)&tos, sizeof(tos));
#else
					 IPPROTO_IP, IP_TOS, (const char *)&tos, sizeof(tos));
#endif
        }
        if(snd_sockfd>=0)
        {
            tos = rtpcode ;
            tos = tos <<2 ;
            retc|=setsockopt(
                     snd_sockfd,
#ifndef WIN32
                     IPPROTO_IP, IP_TOS, (const void *)&tos, sizeof(tos));
#else
					 IPPROTO_IP, IP_TOS, (const char *)&tos, sizeof(tos));
#endif
        }
    }

	rcv_sockfd = (m_Rtcp.rcv.remote.ver == 1)?m_Rtcp.rcv.sockfd:m_Rtcp.rcv.sockfd6;
	snd_sockfd = (m_Rtcp.snd.remote.ver == 1)?m_Rtcp.snd.sockfd:m_Rtcp.snd.sockfd6;
    if(rtcpcode)
    {
        if(rcv_sockfd>=0)
        {
            tos = rtcpcode ;
            tos = tos <<2 ;
            retc|=setsockopt(
                     rcv_sockfd,
#ifndef WIN32
                     IPPROTO_IP, IP_TOS, (const void *)&tos, sizeof(tos));
#else
					 IPPROTO_IP, IP_TOS, (const char *)&tos, sizeof(tos));
#endif
        }
        if(snd_sockfd>=0)
        {
            tos = rtcpcode ;
            tos = tos <<2 ;
            retc|=setsockopt(
                     snd_sockfd,
#ifndef WIN32
                     IPPROTO_IP, IP_TOS, (const void *)&tos, sizeof(tos));
#else
					 IPPROTO_IP, IP_TOS, (const char *)&tos, sizeof(tos));
#endif
        }
    }
    return retc ;
}

int CRtpComm::StartSession(const RTP_PARAM *pSendParam)
{
   //m_Mutex.Lock();

   int rc = 0;
   ClearSession();
   do {
      m_Params = *pSendParam;
      // realloc recv-packet buffer 
#if 0
      if(m_nMaxRecvPacketLen < (int)(m_Params.nRcvPayloadSize + sizeof(RTPHEADER))){
         m_nMaxRecvPacketLen = m_Params.nRcvPayloadSize + sizeof(RTPHEADER);
         if(m_pRecvPacket) delete [] m_pRecvPacket;
         m_pRecvPacket = new unsigned char[m_nMaxRecvPacketLen];
         m_pRtcpRecvPacket = new unsigned char[m_nMaxRecvPacketLen];
      }

      // realloc send-packet buffer 
      if(m_nMaxSendPacketLen < (int)(m_Params.nSndPayloadSize + sizeof(RTPHEADER))){
         m_nMaxSendPacketLen = m_Params.nSndPayloadSize + sizeof(RTPHEADER);
         if(m_pSendPacket) delete [] m_pSendPacket;
         m_pSendPacket = new unsigned char[m_nMaxSendPacketLen];
         m_pRtcpSendPacket = new unsigned char[m_nMaxSendPacketLen];
      }
#endif

      if(m_Params.nSSRC == -1) 
      {
         m_Params.nSSRC = rand();
      }
      m_nSendSeqNumber = m_Params.nInitSequence;
      if(m_Params.remote.bRtpOn) 
      {
         m_RtpTarget.port = m_Rtp.snd.remote.port = m_Params.remote.nRtpPort;
			if(m_Params.remote.ver == 1)
			{
				m_RtpTarget.ver = m_Rtp.snd.remote.ver = m_Rtp.rcv.remote.ver = 1;
         	strcpy(m_Rtp.snd.remote.ip, m_Params.remote.szAddr);
         	strcpy(m_RtpTarget.ip, m_Params.remote.szAddr);
			}
			else
			{
				m_RtpTarget.ver = m_Rtp.snd.remote.ver = m_Rtp.rcv.remote.ver = 2;
         	strcpy(m_Rtp.snd.remote.ip6, m_Params.remote.szAddr6);
         	strcpy(m_RtpTarget.ip6, m_Params.remote.szAddr6);
			}
#ifdef DEBUG_MODE
         printf("DEBUG: RtpComm, StartSession :: rtp set(ver:%d ip:%s, port:%d)\n", 
            m_RtpTarget.ver, m_RtpTarget.ip, m_RtpTarget.port);
#endif
      }
      if(m_Params.remote.bRtcpOn) 
      {
			m_usRtcpMode = m_Params.remote.usRtcpMode;
         m_RtcpTarget.port = m_Rtcp.snd.remote.port = m_Params.remote.nRtcpPort;
			if(m_Params.remote.ver == 1)
			{
				m_RtcpTarget.ver = m_Rtcp.snd.remote.ver = m_Rtcp.rcv.remote.ver = 1;
         	strcpy(m_Rtcp.snd.remote.ip, m_Params.remote.szAddr);
         	strcpy(m_RtcpTarget.ip, m_Params.remote.szAddr);
				if(m_Rtcp.rcv.sockfd != INVALID_SOCKET || m_Params.remote.bRtcpOn == true) 
            {
					//rc = m_pRtcpReport->Open(m_Params.cname, m_Params.nSSRC, m_Params.nTimeScale);
					m_RtcpCtx.Reset();
				}
			}
			else
			{
				m_RtcpTarget.ver = m_Rtcp.snd.remote.ver = m_Rtcp.rcv.remote.ver = 2;
         	strcpy(m_Rtcp.snd.remote.ip6, m_Params.remote.szAddr6);
         	strcpy(m_RtcpTarget.ip6, m_Params.remote.szAddr6);
				if(m_Rtcp.rcv.sockfd6 != INVALID_SOCKET || m_Params.remote.bRtcpOn == true) 
            {
					//rc = m_pRtcpReport->Open(m_Params.cname, m_Params.nSSRC, m_Params.nTimeScale);
					m_RtcpCtx.Reset();
				}
			}
#ifdef DEBUG_MODE
         printf("DEBUG: RtpComm, StartSession :: rtcp set(ver:%d ip:%s, port:%d)\n", 
            m_RtcpTarget.ver, m_RtcpTarget.ip, m_RtcpTarget.port);
#endif
      }


 		if(m_Params.nTimeScale == 0) m_Params.nTimeScale = 8000;

      if(m_Params.nRcvBuffMode == BM_SEQ && m_Params.nDelayTime > 0) 
      {
         m_bBuffered = true;
         rc = m_pRecvBuffer->Open(m_Params.nMaxBufferCount, m_nMaxRecvPacketLen, 
                                  m_Params.nDelayTime, m_Params.nTimeScale, 
                                  CAmRtpBuffer::MD_SEQ);
         if(rc != 0) 
         {
            rc = -1;
            break;
         }
      } 
      else if(m_Params.nRcvBuffMode == BM_TIME && m_Params.nDelayTime > 0) 
      {
         m_bBuffered = true;
         rc = m_pRecvBuffer->Open(m_Params.nMaxBufferCount, m_nMaxRecvPacketLen, 
                                  m_Params.nDelayTime, m_Params.nTimeScale, 
                                  CAmRtpBuffer::MD_TIME);
         if(rc != 0) {
            rc = -1;
            break;
         }
      } 
      else 
      {
         m_bBuffered = false;
      }

      // Set default header.
      RTPHEADER * pHdr = (RTPHEADER *) m_pSendPacket;
      memset(pHdr, 0, sizeof(RTPHEADER));
      pHdr->version = 2;
      pHdr->ssrc    = htonl(m_Params.nSSRC);
      pHdr->payloadtype = m_Params.nSndPayloadType;
   } while(false);

   if(rc == 0) 
   {
      m_bStarted = true;
      gettimeofday(&m_tvRtcpPrevSendTime, NULL);
   } 
   else 
   {
      printf("Error CRTPComm::StartSession(), rc=%d\n", rc); 
      ClearSession();
   }

   gettimeofday(&m_tvFIRSendTime, NULL); 

   gettimeofday(&m_tvRtpFirstSendTime, NULL); 

   //m_Mutex.Unlock();
   return rc;
}

int CRtpComm::StartSession_DUMP(const RTP_PARAM *pSendParam,const DUMP_PARAM* pDumpParam)
{ //dump file name must has struct path + base filename --> "../dump/rtp"
	if(!pDumpParam) return -1; //hutcStsInvalidArgument;
	//char szbuf[6];
	m_nDumpMode = pDumpParam->nDumpMode;
	memcpy(&m_DumpRcvRelay,&pDumpParam->rcv_relay,sizeof(PEER));
	memcpy(&m_DumpSndRelay,&pDumpParam->snd_relay,sizeof(PEER));
	
	switch(m_nDumpMode & 0xf0)
	{
	case DUMP_FILESRC:
	do
	{
		std::ostringstream rcvfilename;
		rcvfilename<<pDumpParam->szFileBase<<".precv"<<pSendParam->nRcvPayloadType;

		m_RcvFBuf.open(rcvfilename.str().c_str(),std::ios::binary | std::ios::out | std::ios::trunc);
		if(m_RcvFBuf.bad())
		{
			m_RcvFBuf.close();
			m_nDumpMode &= 0x0f;
			break;
		}
	}while(false);
	break;
	case DUMP_FILEDST:
	do
	{
		std::ostringstream sndfilename;
		sndfilename<<pDumpParam->szFileBase<<".psend"<<pSendParam->nRcvPayloadType;
		
		m_SndFBuf.open(sndfilename.str().c_str(),std::ios::binary | std::ios::out | std::ios::trunc);
		if(m_SndFBuf.bad())
		{
			m_SndFBuf.close();
			m_nDumpMode &= 0x0f;
			break;
		}
	}while(false);
	break;
	case DUMP_FILEALL:
	do
	{
		std::ostringstream sndfilename,rcvfilename;
		rcvfilename<<pDumpParam->szFileBase<<".precv"<<pSendParam->nRcvPayloadType;
		sndfilename<<pDumpParam->szFileBase<<".psend"<<pSendParam->nRcvPayloadType;
		
		m_SndFBuf.open(sndfilename.str().c_str(),std::ios::binary | std::ios::out | std::ios::trunc);
		m_RcvFBuf.open(rcvfilename.str().c_str(),std::ios::binary | std::ios::out | std::ios::trunc);
		if((m_SndFBuf.bad()) || (m_RcvFBuf.bad()))
		{
			m_SndFBuf.close();
			m_RcvFBuf.close();
			m_nDumpMode &= 0x0f;
			break;
		}
	}while(false);
	break;
	default:
	break;
	}
	return StartSession(pSendParam);
}

int CRtpComm::ModifySession(const RTP_PARAM *pSendParam)
{//after StartSession if need modify SendParam then call this func
   //m_Mutex.Lock();
   if(!m_bStarted) return -1;

   int rc = 0;
   do {
      m_Params = *pSendParam;
      // realloc recv-packet buffer 
      if(m_Params.remote.bRtpOn) 
      {
         m_RtpTarget.port = m_Rtp.snd.remote.port = m_Params.remote.nRtpPort;
			if(m_Params.remote.ver == 1)
			{
				m_RtpTarget.ver = m_Rtp.snd.remote.ver = m_Rtp.rcv.remote.ver = 1;
         	strcpy(m_Rtp.snd.remote.ip, m_Params.remote.szAddr);
         	strcpy(m_RtpTarget.ip, m_Params.remote.szAddr);
			}
			else
			{
				m_RtpTarget.ver = m_Rtp.snd.remote.ver = m_Rtp.rcv.remote.ver = 2;
         	strcpy(m_Rtp.snd.remote.ip6, m_Params.remote.szAddr6);
         	strcpy(m_RtpTarget.ip6, m_Params.remote.szAddr6);
			}
#ifdef DEBUG_MODE
         printf("DEBUG: RtpComm, ModifySession :: rtp set(ver:%d ip:%s, port:%d)\n", 
            m_RtpTarget.ver, m_RtpTarget.ip, m_RtpTarget.port);
#endif
      }
      if(m_Params.remote.bRtcpOn) 
      {
			m_usRtcpMode = m_Params.remote.usRtcpMode;
         m_Rtcp.snd.remote.port = m_Params.remote.nRtcpPort;
         m_RtcpTarget.port = m_Rtcp.snd.remote.port = m_Params.remote.nRtcpPort;
			if(m_Params.remote.ver == 1)
			{
				m_RtcpTarget.ver = m_Rtcp.snd.remote.ver = m_Rtcp.rcv.remote.ver = 1;
         	strcpy(m_Rtcp.snd.remote.ip, m_Params.remote.szAddr);
         	strcpy(m_RtcpTarget.ip, m_Params.remote.szAddr);
				if(m_Rtcp.rcv.sockfd != INVALID_SOCKET || m_Params.remote.bRtcpOn == true) 
            {
					//rc = m_pRtcpReport->Open(m_Params.cname, m_Params.nSSRC, m_Params.nTimeScale);
					m_RtcpCtx.Reset();
				}
			}
			else
			{
				m_RtcpTarget.ver = m_Rtcp.snd.remote.ver = m_Rtcp.rcv.remote.ver = 2;
         	strcpy(m_Rtcp.snd.remote.ip6, m_Params.remote.szAddr6);
         	strcpy(m_RtcpTarget.ip6, m_Params.remote.szAddr6);
				if(m_Rtcp.rcv.sockfd6 != INVALID_SOCKET || m_Params.remote.bRtcpOn == true) 
            {
					//rc = m_pRtcpReport->Open(m_Params.cname, m_Params.nSSRC, m_Params.nTimeScale);
					m_RtcpCtx.Reset();
				}
			}
#ifdef DEBUG_MODE
         printf("DEBUG: RtpComm, ModifySession :: rtcp set(ver:%d ip:%s, port:%d)\n", 
            m_RtcpTarget.ver, m_RtcpTarget.ip, m_RtcpTarget.port);
#endif
      }
      // Set default header.
      RTPHEADER * pHdr = (RTPHEADER *) m_pSendPacket;
      memset(pHdr, 0, sizeof(RTPHEADER));
      pHdr->version = 2;
      pHdr->ssrc    = htonl(m_Params.nSSRC);
      pHdr->payloadtype = m_Params.nSndPayloadType;
   } while(false);

   if(rc == 0) 
   {
      gettimeofday(&m_tvRtcpPrevSendTime, NULL);
   } 
   else 
   {
      printf("Error CRTPComm::ModifySession(), rc=%d\n", rc); 
   }
   //m_Mutex.Unlock();
   return rc;
}

int CRtpComm::ModifySession_DUMP(const RTP_PARAM *pSendParam,const DUMP_PARAM* pDumpParam)
{ //dump file name must has struct path + base filename --> "../dump/rtp"
	if(!pDumpParam) return -1; //hutcStsInvalidArgument;
	//char szbuf[10];
	m_nDumpMode = pDumpParam->nDumpMode;
	memcpy(&m_DumpRcvRelay,&pDumpParam->rcv_relay,sizeof(PEER));
	memcpy(&m_DumpSndRelay,&pDumpParam->snd_relay,sizeof(PEER));
	
	switch(m_nDumpMode & 0xf0)
	{
	case DUMP_FILESRC:
	do
	{
		std::ostringstream rcvfilename;
	    rcvfilename<<pDumpParam->szFileBase<<".precv"<<pSendParam->nRcvPayloadType;
		
		m_RcvFBuf.open(rcvfilename.str().c_str(),std::ios::binary | std::ios::out | std::ios::trunc);
		if(m_RcvFBuf.bad())
		{
			m_RcvFBuf.close();
			m_nDumpMode &= 0x0f;
			break;
		}
	}while(false);
	break;
	case DUMP_FILEDST:
	do
	{
		std::ostringstream sndfilename;
		sndfilename<<pDumpParam->szFileBase<<".psend"<<pSendParam->nRcvPayloadType;
		
		m_SndFBuf.open(sndfilename.str().c_str(),std::ios::binary | std::ios::out | std::ios::trunc);
		if(m_SndFBuf.bad())
		{
			m_SndFBuf.close();
			m_nDumpMode &= 0x0f;
			break;
		}
	}while(false);
	break;
	case DUMP_FILEALL:
	do
	{
		std::ostringstream sndfilename,rcvfilename;
		sndfilename<<pDumpParam->szFileBase<<".psend"<<pSendParam->nRcvPayloadType;
		rcvfilename<<pDumpParam->szFileBase<<".precv"<<pSendParam->nRcvPayloadType;
		
		m_SndFBuf.open(sndfilename.str().c_str(),std::ios::binary | std::ios::out | std::ios::trunc);
		m_RcvFBuf.open(rcvfilename.str().c_str(),std::ios::binary | std::ios::out | std::ios::trunc);
		if((m_SndFBuf.bad()) || (m_RcvFBuf.bad()))
		{
			m_SndFBuf.close();
			m_RcvFBuf.close();
			m_nDumpMode &= 0x0f;
			break;
		}
	}while(false);
	break;
	default:
	break;
	}

	return ModifySession(pSendParam);
}

int CRtpComm::ClearSession()
{
   m_pRtpReceived = NULL;
   m_bStarted = false;

   m_pRecvBuffer->Close();
   //m_pRtcpReport->Close();
   m_RtcpCtx.Reset();
   ClearRecvBuffer(0);
	m_RtcpSRSendLastMS = m_RtcpRRSendLastMS = 0;
   m_uRtpCount = m_uRtcpCount = 0;
   return 0;
}

int CRtpComm::EndSession()
{
   //m_Mutex.Lock();
   m_nDumpMode = 0;
   if(m_SndFBuf.is_open())
   {
   	m_SndFBuf.flush();
   	m_SndFBuf.close();
   }
   if(m_RcvFBuf.is_open())
   {
   	m_RcvFBuf.flush();
   	m_RcvFBuf.close();
   }
	memset(&m_DumpRcvRelay,0x00,sizeof(PEER));
	memset(&m_DumpSndRelay,0x00,sizeof(PEER));
   memset(&m_RtpTarget,0x00,sizeof(PEER));
   ClearSession();
   m_RtcpSRSendLastMS = m_RtcpRRSendLastMS = 0;
   //m_Mutex.Unlock();
   return 0;
}

int CRtpComm::CloseSocket()
{
   //m_Mutex.Lock();
   Clear();
   //m_Mutex.Unlock();
   return 0;
}

int CRtpComm::ClearRecvBuffer(int nTimeout)
{
   int rc = 0;
   int nRecvLen;
   PACKET_TYPE nType;
   do {
#ifdef USING_POLL
      nType = SockPoll(nTimeout);
#else
      nType = SockSelect(nTimeout);
#endif
      if(nType == PT_RTP) 
      {
			if(m_Rtp.rcv.remote.ver == 1)
         {
         	nRecvLen = CAmtSock::RecvFrom(m_Rtp.rcv.sockfd, m_pRecvPacket, m_nMaxRecvPacketLen, 0, m_RtpTarget.ip, &m_RtpTarget.port);
         }
			else
         {
            nRecvLen = CAmtSock6::RecvFrom(m_Rtp.rcv.sockfd6, m_pRecvPacket, m_nMaxRecvPacketLen, 0, m_RtpTarget.ip6, &m_RtpTarget.port);
         }
         if(nRecvLen < 0) return PT_TIMEOUT;
           
#if   0
         if(nRecvLen > 0) { 
            printf("# Clear RTP RecvBuffer(), ip:%s, port:%d, len:%d\n", m_RtpTarget.ip, &m_RtpTarget.port, nRecvLen);
         } else {
            printf("# Clear RTP RecvBuffer(), len:%d\n",  nRecvLen);
            break;
         }
#endif
      } 
      else if(nType == PT_RTCP) 
      {
         if(m_Rtcp.rcv.remote.ver == 1)
         {
            m_nRtcpRecvSize = CAmtSock::RecvFrom(m_Rtcp.rcv.sockfd, m_pRecvPacket, m_nMaxRecvPacketLen, 0, m_RtcpTarget.ip, &m_RtcpTarget.port);
         }
         else
         {
            m_nRtcpRecvSize = CAmtSock6::RecvFrom(m_Rtcp.rcv.sockfd6, m_pRecvPacket, m_nMaxRecvPacketLen, 0, m_RtcpTarget.ip6, &m_RtcpTarget.port);
         }
         if(m_nRtcpRecvSize < 0) return PT_TIMEOUT;
#if   0
         if(nRecvLen > 0) { 
            printf("# Clear RTCP RecvBuffer(), ip:%s, port:%d, len:%d\n", m_RtpTarget.ip, &m_RtpTarget.port, nRecvLen);
         } else {
            printf("# Clear RTCP RecvBuffer(), len:%d\n",  nRecvLen);
            break;
         }
#endif
      } 

   } while(nType == PT_RTP || nType == PT_RTCP);

   return rc;
}

CRtpComm::PACKET_TYPE CRtpComm::SockSelect(int nTimeout,int mode)
//mode 1:rtp only 2:rtcp only 3:rtp & rtcp
{
   PACKET_TYPE nType = PT_ERROR;
	int nMaxFd;
	fd_set  fd_read;
	timeval sel_timeout;
	int     selnum;
	int sockfd = (m_Rtp.rcv.remote.ver == 1)?m_Rtp.rcv.sockfd:m_Rtp.rcv.sockfd6;
	int rtcp_sockfd = (m_Rtcp.rcv.remote.ver == 1)?m_Rtcp.rcv.sockfd:m_Rtcp.rcv.sockfd6;
	switch(mode)
	{
	case SELMODE_NONE://always read rtp(0)
	case NOSEL_RTP:
		return PT_RTP;
	case NOSEL_RTCP:
		return PT_RTCP;
	case SELMODE_RTP://rtp only (1)
   do {
      if(sockfd == INVALID_SOCKET) 
      {
         break;
      }

      FD_ZERO(&fd_read);
      if(sockfd != INVALID_SOCKET) 
      {
         FD_SET((unsigned)sockfd, &fd_read);
      }

      if(nTimeout >= 0) 
      {
         sel_timeout.tv_sec = nTimeout / 1000;
         sel_timeout.tv_usec = (nTimeout % 1000) * 1000;
         selnum = select(sockfd + 1, &fd_read, 0, 0, &sel_timeout);
      } 
      else 
      {
         selnum = select(sockfd + 1, &fd_read, 0, 0, NULL);
      }

      if (selnum == -1) 
      {
         nType = PT_ERROR;
         break;
      }
      if(selnum == 0) 
      {
         nType = PT_TIMEOUT;
         break;
      }

      if (sockfd != INVALID_SOCKET && FD_ISSET(sockfd, &fd_read)) 
      {
         nType = PT_RTP;
         break;
      }
   } while(false);
	break;
	case SELMODE_RTCP://rtcp only (2)
   do {
      if(rtcp_sockfd == INVALID_SOCKET) 
      {
         break;
      }

      FD_ZERO(&fd_read);
      if(rtcp_sockfd != INVALID_SOCKET) 
      {
         FD_SET((unsigned)rtcp_sockfd, &fd_read);
      } 

      if(nTimeout >= 0) 
      {
         sel_timeout.tv_sec = nTimeout / 1000;
         sel_timeout.tv_usec = (nTimeout % 1000) * 1000;
         selnum = select(rtcp_sockfd + 1, &fd_read, 0, 0, &sel_timeout);
      } 
      else 
      {
         selnum = select(rtcp_sockfd + 1, &fd_read, 0, 0, NULL);
      }

      if (selnum == -1) 
      {
         break;
      }
      if(selnum == 0) {
         nType = PT_TIMEOUT;
         break;
      }

      if (rtcp_sockfd != INVALID_SOCKET && FD_ISSET(rtcp_sockfd, &fd_read)) 
      {
         nType = PT_RTCP;
         break;
      }
   } while(false);
	break;
	case SELMODE_BOTH://rtp + rtcp
   do {
      if(sockfd == INVALID_SOCKET &&
         rtcp_sockfd == INVALID_SOCKET ) 
      {
         break;
      }

      FD_ZERO(&fd_read);
      if(sockfd != INVALID_SOCKET) 
      {
         FD_SET((unsigned)sockfd, &fd_read);
      }
      if(rtcp_sockfd != INVALID_SOCKET) 
      {
         FD_SET((unsigned)rtcp_sockfd, &fd_read);
      } 

      nMaxFd = MAX(sockfd, rtcp_sockfd);
      if(nTimeout >= 0) 
      {
         sel_timeout.tv_sec = nTimeout / 1000;
         sel_timeout.tv_usec = (nTimeout % 1000) * 1000;
         selnum = select(nMaxFd + 1, &fd_read, 0, 0, &sel_timeout);
      } 
      else 
      {
         selnum = select(nMaxFd + 1, &fd_read, 0, 0, NULL);
      }

      if (selnum == -1) 
      {
         nType = PT_ERROR;
         break;
      }
      if(selnum == 0) 
      {
         nType = PT_TIMEOUT;
         break;
      }

      if (sockfd != INVALID_SOCKET && FD_ISSET(sockfd, &fd_read)) 
      {
         nType = PT_RTP;
         break;
      }
      if (rtcp_sockfd != INVALID_SOCKET && FD_ISSET(rtcp_sockfd, &fd_read)) 
      {
         nType = PT_RTCP;
         break;
      }
   } while(false);
	break;
	default:
	break;
	}//switch
   return nType;
}
#ifdef USING_POLL
CRtpComm::PACKET_TYPE CRtpComm::SockPoll(int nTimeout,int mode)
{
   PACKET_TYPE nType = PT_ERROR;
   int nMaxFd;
	struct pollfd fds[1];
	int     selnum;
	int sockfd = (m_Rtp.rcv.remote.ver == 1)?m_Rtp.rcv.sockfd:m_Rtp.rcv.sockfd6;
	int rtcp_sockfd = (m_Rtcp.rcv.remote.ver ==1)?m_Rtcp.rcv.sockfd:m_Rtcp.rcv.sockfd6;
	switch(mode)
	{
	case SELMODE_NONE:
	case NOSEL_RTP:
		return PT_RTP;
	case NOSEL_RTCP:
		return PT_RTCP;
	case SELMODE_RTP://rtp only (1)
   do {
      if(sockfd == INVALID_SOCKET) 
      {
         break;
      }

		fds[0].fd = sockfd;
		fds[0].events = POLLIN | POLLPRI;

		selnum = poll(fds,1,nTimeout);

      if (selnum == -1) 
      {
         nType = PT_ERROR;
         break;
      }
      if(selnum == 0) 
      {
         nType = PT_TIMEOUT;
         break;
      }

      if (sockfd != INVALID_SOCKET && selnum > 0)
		{
         nType = PT_RTP;
         break;
      }
   } while(false);
	break;
	case SELMODE_RTCP://rtcp only (2)
	
   do {
      if(rtcp_sockfd == INVALID_SOCKET) 
      {
         nType = PT_RTP;
         break;
      }

		fds[0].fd = rtcp_sockfd;
		fds[0].events = POLLIN | POLLPRI;

		selnum = poll(fds,1,nTimeout);

      if (selnum == -1) 
      {
         break;
      }
      if(selnum == 0) {
         nType = PT_TIMEOUT;
         break;
      }

      if (rtcp_sockfd != INVALID_SOCKET && selnum > 0)
		{
         nType = PT_RTCP;
         break;
      }
   } while(false);
	break;
	case SELMODE_BOTH://rtp + rtcp

   do {
      if(sockfd == INVALID_SOCKET &&
         rtcp_sockfd == INVALID_SOCKET) 
      {
         break;
      }

      nMaxFd = MAX(sockfd, rtcp_sockfd);

		fds[0].fd = nMaxFd;
		fds[0].events = POLLIN | POLLPRI;

		selnum = poll(fds,1,nTimeout);

      if (selnum == -1) 
      {
         nType = PT_ERROR;
         break;
      }
      if(selnum == 0) 
      {
         nType = PT_TIMEOUT;
         break;
      }

      if (sockfd != INVALID_SOCKET && selnum > 0)
		{
         nType = PT_RTP;
         break;
      }
      if (rtcp_sockfd != INVALID_SOCKET && selnum > 0)
		{
         nType = PT_RTCP;
         break;
      }
   } while(false);
	break;
	default:
	break;
	}//switch
   return nType;
}
#endif //USING_POLL
CRtpComm::PACKET_TYPE CRtpComm::Buffering(int nTimeout,int mode)
{
   int nRecvLen;
   PEER peer;
   memset(&peer,0,sizeof(PEER));
   PACKET_TYPE nType;
   do {
#ifdef USING_POLL
      nType = SockPoll(nTimeout,mode);
#else
      nType = SockSelect(nTimeout,mode);
#endif
      if(nType == PT_ERROR || nType == PT_TIMEOUT) 
      {
         break;
      }
      if(nType == PT_TIMEOUT) 
      {
         break;
      }

      if(nType == PT_RTP) 
      {
         RTPHEADER * pRtpHdr = (RTPHEADER*)m_pRecvPacket;
   		//LOCKREM m_Mutex.Lock();
			if(m_Rtp.rcv.remote.ver == 1)
         {
         	nRecvLen = CAmtSock::RecvFrom(m_Rtp.rcv.sockfd, m_pRecvPacket, m_nMaxRecvPacketLen, 0, peer.ip, &peer.port);
            if(m_uRtpCount < (unsigned int)(m_Params.ucRmtNatCount)) // remote nat enable case
            {
#ifdef DEBUG_MODE
               printf("DEBUG: RtpComm, Buffering :: rtp packet received(ver:%d ip:%s, port:%d), Count:%u/%u, 0x%p \n", 
                  peer.ver, peer.ip, peer.port, m_uRtpCount, (unsigned int)(m_Params.ucRmtNatCount), this);
#endif
               //compare ip/port
               if(strncmp(m_RtpTarget.ip, peer.ip, MAX_IP_ADDR)!=0 || m_RtpTarget.port != peer.port) 
         	      m_RtpTarget = peer;
            }
         }
			else
         {
        	   nRecvLen = CAmtSock6::RecvFrom(m_Rtp.rcv.sockfd6, m_pRecvPacket, m_nMaxRecvPacketLen, 0, m_RtpTarget.ip6, &m_RtpTarget.port);
         }

         if(nRecvLen < 0) return PT_TIMEOUT;
         else 
         {
            m_uRtpCount++;
#ifdef DEBUG_MODE
            printf("m_uRtpCount++ (%u), 0x%p \n", m_uRtpCount, this);
#endif
         }
			if(nRecvLen > 0 && (m_nDumpMode&DUMP_RELAYSRC)==DUMP_RELAYSRC)
			{
				int sockfd;
				if(m_Rtp.snd.remote.ver == 1)
				{
					if(m_Rtp.snd.sockfd == -1)
						sockfd = m_Rtp.rcv.sockfd;
					else
						sockfd = m_Rtp.snd.sockfd;
					CAmtSock::SendTo(m_Rtp.rcv.sockfd, m_DumpRcvRelay.ip, m_DumpRcvRelay.port, (char *)m_pRecvPacket, nRecvLen);
				}
				else
				{
					if(m_Rtp.snd.sockfd6 == -1)
						sockfd = m_Rtp.rcv.sockfd6;
					else
						sockfd = m_Rtp.snd.sockfd6;
					CAmtSock6::SendTo(m_Rtp.rcv.sockfd6, m_DumpRcvRelay.ip6, m_DumpRcvRelay.port, (char *)m_pRecvPacket, nRecvLen);
				}
				
			}
			//LOCKREM m_Mutex.Unlock();
			if(nRecvLen > 0 && (m_nDumpMode& DUMP_FILESRC)==DUMP_FILESRC && m_RcvFBuf.is_open())
         {
				unsigned int tmprecvlen = htonl(nRecvLen);
         	m_RcvFBuf.write((char*)&tmprecvlen,sizeof(int));
         	m_RcvFBuf.write((char*)m_pRecvPacket,nRecvLen);
         }
         nType = CheckRtpPacket(m_pRecvPacket, nRecvLen);      
         if(nType == PT_RTP && (pRtpHdr->payloadtype == m_Params.nRcvPayloadType || 
         pRtpHdr->payloadtype == m_Params.nRcvPayloadType2)) 
         { 
            m_pRecvBuffer->Put(m_pRecvPacket, nRecvLen);
            return PT_TIMEOUT;
         } 
         m_pRtpReceived = m_pRecvPacket;
         m_nRtpRecvSize = nRecvLen;
#ifdef DEBUG_MODE
         printf("DEBUG: RtpComm, no media packet received(type:%d, len:%d))\n", nType, nRecvLen);
#endif
      } 
      else if(nType == PT_RTCP) 
      {
			if(m_Rtcp.rcv.remote.ver == 1)
         {
            m_nRtcpRecvSize = CAmtSock::RecvFrom(m_Rtcp.rcv.sockfd, m_pRtcpRecvPacket, m_nMaxRecvPacketLen, 0, peer.ip, &peer.port);
            if(m_uRtcpCount < (unsigned int)(m_Params.ucRmtNatCount)) // remote nat enable case
            {
#ifdef DEBUG_MODE
               printf("DEBUG: RtpComm, Buffering :: rtcp packet received(ver:%d ip:%s, port:%d), Count:%u/%u, 0x%p \n", 
                  peer.ver, peer.ip, peer.port, m_uRtcpCount, (unsigned int)(m_Params.ucRmtNatCount), this);
#endif
               //compare ip/port
               if(strncmp(m_RtcpTarget.ip, peer.ip, MAX_IP_ADDR)!=0 || m_RtcpTarget.port != peer.port) 
                  m_RtcpTarget = peer;
            }
         }
			else
         {
         	m_nRtcpRecvSize = CAmtSock6::RecvFrom(m_Rtcp.rcv.sockfd6, m_pRtcpRecvPacket, m_nMaxRecvPacketLen, 0, m_RtcpTarget.ip6, &m_RtcpTarget.port);
         }

			if(m_nRtcpRecvSize < 0) return PT_TIMEOUT;
         else
         {
            m_uRtcpCount++;
#ifdef DEBUG_MODE
            //printf("m_uRtcpCount++ (%u)\n", m_uRtcpCount);
#endif
         }

			if(m_usRtcpMode)
			{
/*			
				nType = CheckRtcpPacket(m_pRecvPacket, nRecvLen);      
				if(nType == PT_RTCP) { 
					rc = m_pRtcpReport->ParseReport(m_pRecvPacket, nRecvLen);
					if(rc != 0) {
						printf("WARN: RtpComm, invalid rtcp received\n");
						return PT_RTCP_UNKNOWN;
					} 
				}
*/
				m_RtcpCtx.RTCPReceived((unsigned char*)m_pRecvPacket,nRecvLen);
			}
      } 
   } while (false);

   return nType;
}

CRtpComm::PACKET_TYPE CRtpComm::FlushRecv(int nTimeout,int mode)
{
	int nRecvLen;
	PACKET_TYPE nType;
#ifdef USING_POLL
	nType = SockPoll(nTimeout,mode);
#else
	nType = SockSelect(nTimeout,mode);
#endif
	if(nType == PT_RTP) 
   {
		if(m_Rtp.rcv.remote.ver == 1)
      {
         nRecvLen = CAmtSock::RecvFrom(m_Rtp.rcv.sockfd, m_pRecvPacket, m_nMaxRecvPacketLen, 0, m_RtpTarget.ip, &m_RtpTarget.port);
      }
		else
      {
         nRecvLen = CAmtSock6::RecvFrom(m_Rtp.rcv.sockfd6, m_pRecvPacket, m_nMaxRecvPacketLen, 0, m_RtpTarget.ip6, &m_RtpTarget.port);
      }
      if(nRecvLen < 0)
         nType = PT_TIMEOUT;
      else
      {
      }
	}
	if(nType == PT_RTCP) 
   {
		if(m_Rtp.rcv.remote.ver == 1)
      {
         nRecvLen = CAmtSock::RecvFrom(m_Rtp.rcv.sockfd, m_pRtcpRecvPacket, m_nMaxRecvPacketLen, 0, m_RtcpTarget.ip, &m_RtcpTarget.port);
      }
		else
      {
			nRecvLen = CAmtSock::RecvFrom(m_Rtcp.rcv.sockfd6, m_pRtcpRecvPacket, m_nMaxRecvPacketLen, 0, m_RtcpTarget.ip6, &m_RtcpTarget.port);
      }
      if(nRecvLen < 0)
         nType = PT_TIMEOUT;
	} 
	return nType;
}

CRtpComm::PACKET_TYPE CRtpComm::DirectRecv(int nTimeout,int mode)
{
   int nRecvLen;
   PEER peer;
   memset(&peer, 0, sizeof(PEER));
   PACKET_TYPE nType;

#ifdef USING_POLL
   nType = SockPoll(nTimeout,mode);
#else
   nType = SockSelect(nTimeout,mode);
#endif
		
   if(nType == PT_RTP) 
   {
      RTPHEADER * pRtpHdr = (RTPHEADER*)m_pRecvPacket;
		//LOCKREM m_Mutex.Lock();
		if(m_Rtp.rcv.remote.ver == 1)
      {
         nRecvLen = CAmtSock::RecvFrom(m_Rtp.rcv.sockfd, m_pRecvPacket, m_nMaxRecvPacketLen, 0, peer.ip, &peer.port);
         if((m_uRtpCount < (unsigned int)(m_Params.ucRmtNatCount)) && (peer.port != 0)) // remote nat enable case
         {
#ifdef DEBUG_MODE
            printf("DEBUG1: RtpComm, DirectRecv : rtp packet received(ver:%d ip:%s, port:%d, pt:%d, seq:%d, timestamp:%d), Count:%u/%u, 0x%p \n", 
               peer.ver, peer.ip, peer.port, pRtpHdr->payloadtype, pRtpHdr->seqnumber, pRtpHdr->timestamp, m_uRtpCount, (unsigned int)(m_Params.ucRmtNatCount), this);
#endif
            //compare ip/port
            if(strncmp(m_RtpTarget.ip, peer.ip, MAX_IP_ADDR)!=0 || m_RtpTarget.port != peer.port) 
               m_RtpTarget = peer;
         }
      }
		else
      {
         nRecvLen = CAmtSock6::RecvFrom(m_Rtp.rcv.sockfd6, m_pRecvPacket, m_nMaxRecvPacketLen, 0, m_RtpTarget.ip6, &m_RtpTarget.port);
      }

      if(nRecvLen < 0) return PT_TIMEOUT;
      else
      {
         m_uRtpCount++;
#ifdef DEBUG_MODE
         printf("DEBUG1: RtpComm, DirectRecv : rtp packet received"
			    "(ver:%d ip:%s, port:%d, pt:%d, seq:%d, timestamp:%d), Count:%u/%u, 0x%p \n", 
               peer.ver, peer.ip, peer.port, 
			   pRtpHdr->payloadtype, pRtpHdr->seqnumber, pRtpHdr->timestamp, 
			   m_uRtpCount, (unsigned int)(m_Params.ucRmtNatCount), this);
         printf("m_uRtpCount++1 (%u), 0x%p rtcp_mode:%d seq :%d \n", m_uRtpCount, this,m_usRtcpMode, (unsigned int)htons(pRtpHdr->seqnumber));
#endif
      }

	   int sockfd; // using rtp & rtcp socket
		if(nRecvLen > 0 && (m_nDumpMode&DUMP_RELAYSRC)==DUMP_RELAYSRC)
      {
			if(m_Rtp.snd.remote.ver == 1)
			{
				if(m_Rtp.snd.sockfd == -1)
					sockfd = m_Rtp.rcv.sockfd;
				else
					sockfd = m_Rtp.snd.sockfd;
         	CAmtSock::SendTo(sockfd, m_DumpRcvRelay.ip, m_DumpRcvRelay.port, (char *)m_pRecvPacket, nRecvLen);
			}
			else
			{
				if(m_Rtp.snd.sockfd6 == -1)
					sockfd = m_Rtp.rcv.sockfd6;
				else
					sockfd = m_Rtp.snd.sockfd6;
         	CAmtSock6::SendTo(sockfd, m_DumpRcvRelay.ip6, m_DumpRcvRelay.port, (char *)m_pRecvPacket, nRecvLen);
			}
      }
		//LOCKREM m_Mutex.Unlock();
      if(nRecvLen > 0 && (m_nDumpMode&DUMP_FILESRC)==DUMP_FILESRC && m_RcvFBuf.is_open())
      {
			unsigned int tmprecvlen = htonl(nRecvLen);
      	m_RcvFBuf.write((char*)&tmprecvlen,sizeof(int));
      	m_RcvFBuf.write((char*)m_pRecvPacket,nRecvLen);
      }
      nType = CheckRtpPacket(m_pRecvPacket, nRecvLen);
      if(nType == PT_RTP 
         && (m_Rtcp.snd.sockfd != INVALID_SOCKET ||  m_Rtcp.rcv.sockfd != INVALID_SOCKET)) 
      { 
	      RtpChkStatus_T seq_st ;
	      if(m_usRtcpMode)
         {
            int nBody = nRecvLen-sizeof(RTPHEADER);
            if(nBody<=0) nBody = 0;
           
	      	m_RtcpCtx.RTPReceived((unsigned short)htons(pRtpHdr->seqnumber), (unsigned int)htonl(pRtpHdr->timestamp), 
               (unsigned int)htonl(pRtpHdr->ssrc), nBody, m_Params.nTimeScale, &seq_st);
         }
      } 
      m_pRtpReceived = m_pRecvPacket;
      m_nRtpRecvSize = nRecvLen;

   } 
   else if(nType == PT_RTCP) 
   {
		if(m_Rtcp.rcv.remote.ver == 1)
      {
         nRecvLen = CAmtSock::RecvFrom(m_Rtcp.rcv.sockfd, m_pRtcpRecvPacket, m_nMaxRecvPacketLen, 0, peer.ip, &peer.port);
         if((m_uRtcpCount < (unsigned int)(m_Params.ucRmtNatCount)) && (peer.port != 0)) // remote nat enable case
         {
#ifdef DEBUG_MODE
            printf("DEBUG: RtpComm, DirectRecv :: rtcp packet received(ver:%d ip:%s, port:%d), Count:%u/%u, 0x%p \n", 
               peer.ver, peer.ip, peer.port, m_uRtcpCount, (unsigned int)(m_Params.ucRmtNatCount), this);
#endif
            //compare ip/port
            if(strncmp(m_RtcpTarget.ip, peer.ip, MAX_IP_ADDR)!=0 || m_RtcpTarget.port != peer.port) 
               m_RtcpTarget = peer;
         }
      }
		else
      {
      	nRecvLen = CAmtSock6::RecvFrom(m_Rtcp.rcv.sockfd6, m_pRtcpRecvPacket, m_nMaxRecvPacketLen, 0, m_RtcpTarget.ip6, &m_RtcpTarget.port);
      }

      m_nRtcpRecvSize = nRecvLen;

		if(m_nRtcpRecvSize < 0) return PT_TIMEOUT;
      else 
      { 
         m_uRtcpCount++;
#ifdef DEBUG_MODE
         //printf("m_uRtcpCount++ (%u)\n", m_uRtcpCount);
#endif
      }

		if(m_usRtcpMode)
		{
/*		
			nType = CheckRtcpPacket(m_pRecvPacket, nRecvLen);      
			if(nType == PT_RTCP) { 
				rc = m_pRtcpReport->ParseReport(m_pRecvPacket, nRecvLen);
				if(rc != 0) {
					printf("WARN: RtpComm, invalid rtcp received\n");
					nType = PT_RTCP_UNKNOWN;
				}
			}
*/
			m_RtcpCtx.RTCPReceived((unsigned char*)m_pRtcpRecvPacket,nRecvLen);
		}
   } 
   return nType;
}


int CRtpComm::WaitForPacket(PACKET_TYPE *pnType, int nTimeout)
{
   PACKET_TYPE nType = PT_TIMEOUT;
   //int rc = 0;
   m_nRtpRecvSize = 0;
   do {
      if(!m_bStarted) 
      {
         ClearRecvBuffer(0);
         //ClearRecvBuffer(10);
         //*pnType = PT_ERROR;
         nType = PT_NOT_STARTED;
         break;
      }
      //LOCKREM m_Mutex.Lock();
#if 0      
      if(m_Params.remote.bRtcpOn && m_usRtcpMode) {
         CheckSendRtcpReport();
      }
#endif      
      //LOCKREM m_Mutex.Unlock();

      if(m_bBuffered == false) 
      { 
			if(m_Params.remote.bRtcpOn && m_usRtcpMode)
         	nType = DirectRecv(nTimeout,SELMODE_BOTH);
			else
				nType = DirectRecv(nTimeout,SELMODE_RTP);
      } 
      else 
      {
			if(m_Params.remote.bRtcpOn && m_usRtcpMode)
         	nType = Buffering(0,SELMODE_BOTH);
			else
         	nType = Buffering(0,SELMODE_RTP);
         if(nType == PT_TIMEOUT) 
         {
            // Check Receive Buffer & Get-Out
            m_pRtpReceived = m_pRecvBuffer->TryGet(m_nRtpRecvSize);
            if(m_pRtpReceived) 
            {
               nType = PT_RTP;
               break;
            }
				if(m_Params.remote.bRtcpOn && m_usRtcpMode)
            	nType = Buffering(nTimeout,SELMODE_BOTH);
				else
            	nType = Buffering(nTimeout,SELMODE_BOTH);
         }
      }
   } while (false);

   // for FIR 
   if(m_bVideo && m_nFirMode != FIRM_MANUAL && nType == PT_RTP) 
   {
      CheckVideoPacketLoss();
   }

   *pnType = nType;

   return 0;
}

// if PT_RTCP.
int CRtpComm::GetReportCount()
{
   int n = 0;
   //m_Mutex.Lock();
#if 0   
   if(m_bStarted) 
   {
      n = m_pRtcpReport->GetReportCount();
   }
#endif   
   //m_Mutex.Unlock();
   return n;
}

int CRtpComm::GetReport(int nIndex, CRtcpReport::Report *pReport)
{
   int n = 0;
   //m_Mutex.Lock();
#if 0   
   if(m_bStarted) 
   {
      n = m_pRtcpReport->GetReport(nIndex, pReport);
   }
#endif   
   //m_Mutex.Unlock();
   return n;
}

////
unsigned char * CRtpComm::GetPacket(int *pnOutLength)
{
   unsigned char * ptr = NULL;
   //m_Mutex.Lock();
   if(m_bStarted) 
   {
      ptr = m_pRtpReceived;
      *pnOutLength = m_nRtpRecvSize;
   }
   //m_Mutex.Unlock();
   return ptr;
}

int CRtpComm::GetPacket(unsigned char* pOutData, int nInMax, int *pnOutLength)
{
   int rc = 0;
   //m_Mutex.Lock();
   if(m_bStarted) 
   {
      if(nInMax < m_nRtpRecvSize) 
      {
         *pnOutLength = 0;
         rc = -1;
      } else {
         *pnOutLength = m_nRtpRecvSize;
         memcpy(pOutData, m_pRtpReceived, m_nRtpRecvSize);
      }
   }
   //m_Mutex.Unlock();
   return rc;
}
/////
unsigned char * CRtpComm::GetSendPacket(int *pnOutLength)
{
   unsigned char * ptr = NULL;
   //m_Mutex.Lock();
   if(m_bStarted) 
   {
      ptr = m_pSendPacket;
      *pnOutLength = m_nSendPacketSize;
   }
   //m_Mutex.Unlock();
   return ptr;
}

int CRtpComm::GetSendPacket(unsigned char* pOutData, int nInMax, int *pnOutLength)
{
   int rc = 0;
   //m_Mutex.Lock();
   if(m_bStarted) 
   {
      if(nInMax < m_nRtpRecvSize) 
      {
         *pnOutLength = 0;
         rc = -1;
      } 
      else 
      {
         *pnOutLength = m_nSendPacketSize;
         memcpy(pOutData, m_pSendPacket, m_nSendPacketSize);
      }
   }
   //m_Mutex.Unlock();
   return rc;
}

unsigned char * CRtpComm::GetRtcpPacket(int *pnOutLength)
{
   unsigned char * ptr = NULL;
   //m_Mutex.Lock();
   if(m_bStarted) 
   {
      ptr = m_pRtcpRecvPacket;
      *pnOutLength = m_nRtcpRecvSize;
   }
   //m_Mutex.Unlock();
   return ptr;
}

unsigned char * CRtpComm::GetRtcpSendPacket(int *pnOutLength)
{
   unsigned char * ptr = NULL;
   //m_Mutex.Lock();
   if(m_bStarted) {
      ptr = m_pRtcpSendPacket;
      *pnOutLength = m_nRtcpSendPacketSize;
   }
   //m_Mutex.Unlock();
   return ptr;
}

// if PT_RTP.
int CRtpComm::GetPayload(unsigned char* pOutData, int nInMax, int *pnOutLength, int *pnMaker)
{
   int rc = 0;
   //m_Mutex.Lock();
   if(m_bStarted && m_nRtpRecvSize > (int)sizeof(RTPHEADER)) 
   {
      RTPHEADER * pHdr = (RTPHEADER *)m_pRtpReceived;
      int nHeaderSize = RTP_HEADER_SIZE + pHdr->cc * 4;
      int nPayloadSize = m_nRtpRecvSize
                        - nHeaderSize
                        - ((pHdr->padding==0)?0:(m_pRtpReceived[m_nRtpRecvSize-1] & 0x03)); // padding size <4
      if(nInMax < nPayloadSize) {
         *pnOutLength = 0;
         rc = -1;
      } else {
         *pnOutLength = nPayloadSize;
         *pnMaker = ((RTPHEADER *)m_pRtpReceived)->marker;
         memcpy(pOutData, m_pRtpReceived+nHeaderSize, nPayloadSize);
      }
   } else {
      *pnOutLength = 0;
      rc = -2;
   }
   //m_Mutex.Unlock();
   return rc;
}

int CRtpComm::GetPayload_Hdr(unsigned char* pOutData, int nInMax, int *pnOutLength, RTPHEADER *pDstHdr)
{
   int rc = 0;
   //m_Mutex.Lock();
   if(m_bStarted && m_nRtpRecvSize > (int)sizeof(RTPHEADER)) 
   {
      RTPHEADER * pHdr = (RTPHEADER *)m_pRtpReceived;
      int nHeaderSize = RTP_HEADER_SIZE + pHdr->cc * 4;
      int nPayloadSize = m_nRtpRecvSize 
                        - nHeaderSize 
                        - ((pHdr->padding==0)?0:(m_pRtpReceived[m_nRtpRecvSize-1] & 0x03)); // padding size <4
      if(nInMax < nPayloadSize) 
      {
         *pnOutLength = 0;
         rc = -1;
      } 
      else 
      {
         *pnOutLength = nPayloadSize;
         //*pnMaker = ((RTPHEADER *)m_pRtpReceived)->marker;
         memcpy(pDstHdr,pHdr,sizeof(RTPHEADER));
         memcpy(pOutData, m_pRtpReceived+nHeaderSize, nPayloadSize);
      }
   } 
   else 
   {
      *pnOutLength = 0;
      rc = -2;
   }
   //m_Mutex.Unlock();
   return rc;
}

// Information of the last-received(out) packet
int CRtpComm::GetTimestamp()  
{
   int n = 0;
   //m_Mutex.Lock();
   if(m_bStarted) 
   {
      n = htonl(((RTPHEADER *)m_pRtpReceived)->timestamp);
   }
   //m_Mutex.Unlock();
   return n;
}

int CRtpComm::GetSequence()
{
   int n = 0;
   //m_Mutex.Lock();
   if(m_bStarted) 
   {
      n = htons(((RTPHEADER *)m_pRtpReceived)->seqnumber);
   }
   //m_Mutex.Unlock();
   return n;
}

int CRtpComm::GetPayloadType()
{
   int n = 0;
   //m_Mutex.Lock();
   if(m_bStarted) 
   {
      n = ((RTPHEADER *)m_pRtpReceived)->payloadtype;
   };
   //m_Mutex.Unlock();
   return n;
}

unsigned int CRtpComm::GetSSRC()
{
   int n = 0;
   //m_Mutex.Lock();
   if(m_bStarted) 
   {
      n = htonl(((RTPHEADER *)m_pRtpReceived)->ssrc);
   };
   //m_Mutex.Unlock();
   return n;
}

void CRtpComm::SetSendTimestampMode(bool bAuto) 
{
   m_bAutoSetSendTimestamp = bAuto;
}

//int CRtpComm::SourceChanged(int nSSRC, int nSndPayloadType, int nRcvPayloadType)
//int CRtpComm::SourceChanged(int nRcvSSRC, CODEC_TYPE nSndCodecID, int nSndPayloadType, CODEC_TYPE nRcvCodecID, int nRcvPayloadType,int nRcvPayloadType2)
int CRtpComm::SourceChanged(int nRcvSSRC, int nSndPayloadType, int nRcvPayloadType,int nRcvPayloadType2)
{
   //m_Mutex.Lock();
   if(nRcvSSRC == -1) 
   {
      m_Params.nSSRC = rand();
   } 
   else 
   {
      m_Params.nSSRC = nRcvSSRC;
   }
   //m_Params.nSndCodecID = nSndCodecID;
   m_Params.nSndPayloadType = nSndPayloadType;
   //m_Params.nRcvCodecID = nRcvCodecID;
   m_Params.nRcvPayloadType = nRcvPayloadType;
   m_Params.nRcvPayloadType = nRcvPayloadType2;

   gettimeofday(&m_tvFIRSendTime, NULL); 
   gettimeofday(&m_tvRtpFirstSendTime, NULL); 

   //m_Mutex.Unlock();
   return 0;
}

int CRtpComm::PutPacketWithLastTimestamp(const unsigned char* pInData, int nInLength)
{
   int nSendLen = 0;
   PEER peer;
   memset(&peer, 0, sizeof(PEER));
   peer = m_RtpTarget;
   //LOCKREM m_Mutex.Lock();
   if(m_bStarted) 
   {
      RTPHEADER * pHdr = (RTPHEADER *) pInData;
      m_nSendSeqNumber = htons(pHdr->seqnumber);
      unsigned int uTimestamp = htonl(pHdr->timestamp);
      m_uSendTimestamp = uTimestamp;
		int sockfd;
		if(m_Rtp.snd.remote.ver == 1)
		{
			if(m_Rtp.snd.sockfd == -1)
				sockfd = m_Rtp.rcv.sockfd;
			else
				sockfd = m_Rtp.snd.sockfd;
         // TODO:: remote nat
         if(peer.port!=0)
            nSendLen = CAmtSock::SendTo(sockfd, peer.ip, peer.port, (char *)pInData, nInLength);

			if(nSendLen > 0 && (m_nDumpMode&DUMP_RELAYDST)==DUMP_RELAYDST)
			{
				CAmtSock::SendTo(sockfd, m_DumpSndRelay.ip, m_DumpSndRelay.port, (char *)pInData, nInLength);
			}
		}
		else
		{
			if(m_Rtp.snd.sockfd6 == -1)
				sockfd = m_Rtp.rcv.sockfd6;
			else
				sockfd = m_Rtp.snd.sockfd6;
      	nSendLen = CAmtSock6::SendTo(sockfd, m_Rtp.snd.remote.ip6, m_Rtp.snd.remote.port, (char *)pInData, nInLength);
			if(nSendLen > 0 && (m_nDumpMode&DUMP_RELAYDST)==DUMP_RELAYDST)
			{
				CAmtSock6::SendTo(sockfd, m_DumpSndRelay.ip6, m_DumpSndRelay.port, (char *)pInData, nInLength);
			}
		}

      if(nSendLen > 0 && (m_nDumpMode&DUMP_FILEDST)==DUMP_FILEDST && m_SndFBuf.is_open())
      {
      	unsigned int tmpsendlen = htonl(nSendLen);
      	m_SndFBuf.write((char*)&tmpsendlen,sizeof(int));
      	m_SndFBuf.write((char*)pInData,nSendLen);
      }
      if(m_Params.remote.bRtcpOn && m_usRtcpMode) 
      {
         int nBody = nSendLen - sizeof(RTPHEADER);
         if(nBody <= 0) nBody  = 0;
         //m_pRtcpReport->PacketSent(nSendLen, m_nSendSeqNumber, uTimestamp);
         //CheckSendRtcpReport();
         //m_RtcpCtx.RTPSent(pHdr->seqnumber,pHdr->timestamp,nSendLen-sizeof(RTPHEADER));
         //m_RtcpCtx.RTPSent(m_nSendSeqNumber,m_uSendTimestamp,nSendLen-sizeof(RTPHEADER));
         m_RtcpCtx.RTPSent(m_nSendSeqNumber,m_uSendTimestamp,nBody);
      }
      m_nSendSeqNumber++;
   }
   //LOCKREM m_Mutex.Unlock();
   memcpy(m_pSendPacket,pInData, nInLength);
   m_nSendPacketSize = nSendLen;
   return nSendLen;

}

int CRtpComm::PutPacket(const unsigned char* pInData, int nInLength)
{
   int nSendLen = 0;
   PEER peer;
   memset(&peer, 0, sizeof(PEER));
   peer = m_RtpTarget;
   //LOCKREM m_Mutex.Lock();
   if(m_bStarted) 
   {
      RTPHEADER * pHdr = (RTPHEADER *) pInData;
      m_nSendSeqNumber = htons(pHdr->seqnumber);
      unsigned int uTimestamp = htonl(pHdr->timestamp);
      m_uSendTimestamp = uTimestamp;
		int sockfd;
		if(m_Rtp.snd.remote.ver == 1)
		{
			if(m_Rtp.snd.sockfd == -1)
				sockfd = m_Rtp.rcv.sockfd;
			else
				sockfd = m_Rtp.snd.sockfd;
         // TODO:: remote nat
         if(peer.port!=0)
            nSendLen = CAmtSock::SendTo(sockfd, peer.ip, peer.port, (char *)pInData, nInLength);

			if(nSendLen > 0 && (m_nDumpMode&DUMP_RELAYDST)==DUMP_RELAYDST)
			{
				CAmtSock::SendTo(sockfd, m_DumpSndRelay.ip, m_DumpSndRelay.port, (char *)pInData, nInLength);
			}
		}
		else
		{
			if(m_Rtp.snd.sockfd6 == -1)
				sockfd = m_Rtp.rcv.sockfd6;
			else
				sockfd = m_Rtp.snd.sockfd6;
			nSendLen = CAmtSock6::SendTo(sockfd, m_Rtp.snd.remote.ip6, m_Rtp.snd.remote.port, (char *)pInData, nInLength);
			if(nSendLen > 0 && (m_nDumpMode&DUMP_RELAYDST)==DUMP_RELAYDST)
			{
				CAmtSock::SendTo(sockfd, m_DumpSndRelay.ip6, m_DumpSndRelay.port, (char *)pInData, nInLength);
			}
		}
      if(nSendLen > 0 && (m_nDumpMode&DUMP_FILEDST)==DUMP_FILEDST && m_SndFBuf.is_open())
      {
      	unsigned int tmpsendlen = htonl(nSendLen);
      	m_SndFBuf.write((char*)&tmpsendlen,sizeof(int));
      	m_SndFBuf.write((char*)pInData,nSendLen);
      }
      if(m_Params.remote.bRtcpOn && m_usRtcpMode) 
      {
         int nBody = nSendLen - sizeof(RTPHEADER);
         if(nBody <= 0) nBody  = 0;
         //m_pRtcpReport->PacketSent(nSendLen, m_nSendSeqNumber, uTimestamp);
         //CheckSendRtcpReport();
         //m_RtcpCtx.RTPSent(pHdr->seqnumber,pHdr->timestamp,nSendLen-sizeof(RTPHEADER));
         //m_RtcpCtx.RTPSent(m_nSendSeqNumber,m_uSendTimestamp,nSendLen-sizeof(RTPHEADER));
         m_RtcpCtx.RTPSent(m_nSendSeqNumber,m_uSendTimestamp,nBody);
      }
      m_nSendSeqNumber++;
   }
   //LOCKREM m_Mutex.Unlock();
   memcpy(m_pSendPacket,pInData, nInLength);
   m_nSendPacketSize = nSendLen;
   return nSendLen;

}

// pInData : rtp payload only.
int CRtpComm::PutPayloadWithLastTimestamp(const unsigned char* pInData, int nInLength, int nMaker, int nPayloadType)
{
   return PutPayload_(pInData, nInLength, m_uSendTimestamp, nMaker, m_Params.nSndPayloadType);
}

// pInData : rtp payload only.
int CRtpComm::PutPayload(const unsigned char* pInData, int nInLength, unsigned int uTimestamp, int nMaker,int nSeqNumber)
{
   return PutPayload_(pInData, nInLength, uTimestamp, nMaker, m_Params.nSndPayloadType,nSeqNumber);
}

int CRtpComm::PutPayload_(const unsigned char* pInData, int nInLength, unsigned int uTimestamp, int nMaker, int nPayloadType,int nSeqNumber)
{
   PEER peer;
   memset(&peer, 0, sizeof(PEER));
   peer = m_RtpTarget;

   if(nInLength > CRtpComm::MAX_UDP_PACKET_SIZE)
      return -1;
   int nSendLen = 0;
   unsigned int uSendTimestamp = uTimestamp;
   //LOCKREM m_Mutex.Lock();
   if(m_bStarted) 
   {
      memcpy(m_pSendPacket+RTP_HEADER_SIZE, pInData, nInLength);

      RTPHEADER * pHdr = (RTPHEADER *) m_pSendPacket;

      // Make RTP Header..
      // auto timestamp
      if(m_bAutoSetSendTimestamp) 
      {
         struct timeval tvCurTime;
         gettimeofday(&tvCurTime, NULL);
         int nDiffSysTimeMs = TvGetDiffMS(&tvCurTime, &m_tvRtpFirstSendTime);
         uSendTimestamp = nDiffSysTimeMs *  (m_Params.nTimeScale / 1000);
      }
      pHdr->payloadtype = nPayloadType;
#if 0// LGT_DISORDER_RTP_SEQ
      if(m_nSendSeqNumber%10==0) 
         pHdr->seqnumber   = htons(m_nSendSeqNumber+1);
      else if(m_nSendSeqNumber%10==1)
         pHdr->seqnumber   = htons(m_nSendSeqNumber-1);
      else
         pHdr->seqnumber   = htons(m_nSendSeqNumber);
#else
		if(nSeqNumber>0) m_nSendSeqNumber = nSeqNumber;
      pHdr->seqnumber   = htons(m_nSendSeqNumber);
#endif
      m_uSendTimestamp = uSendTimestamp;
      pHdr->timestamp   = htonl(uSendTimestamp);
      pHdr->marker      =   nMaker ? 1 : 0; // 0 or 1

      // SendPacket
      int sockfd;
		if(m_Rtp.snd.remote.ver == 1)
		{
			if(m_Rtp.snd.sockfd == -1)
				sockfd = m_Rtp.rcv.sockfd;
			else
				sockfd = m_Rtp.snd.sockfd;
         // TODO:: remote nat
         if(peer.port!=0)
      	   nSendLen = CAmtSock::SendTo(sockfd, peer.ip, peer.port, (char *)m_pSendPacket, nInLength+RTP_HEADER_SIZE);

			if(nSendLen > 0 && (m_nDumpMode&DUMP_RELAYDST)==DUMP_RELAYDST)
			{
				CAmtSock::SendTo(sockfd, m_DumpSndRelay.ip, m_DumpSndRelay.port, (char *)m_pSendPacket, nInLength+RTP_HEADER_SIZE);
			}
		}
		else
		{
			if(m_Rtp.snd.sockfd6 == -1)
				sockfd = m_Rtp.rcv.sockfd6;
			else
				sockfd = m_Rtp.snd.sockfd6;
			nSendLen = CAmtSock6::SendTo(sockfd, m_Rtp.snd.remote.ip6, m_Rtp.snd.remote.port, (char *)m_pSendPacket, nInLength+RTP_HEADER_SIZE);
			if(nSendLen > 0 && (m_nDumpMode&DUMP_RELAYDST)==DUMP_RELAYDST)
			{
				CAmtSock6::SendTo(sockfd, m_DumpSndRelay.ip6, m_DumpSndRelay.port, (char *)m_pSendPacket, nInLength+RTP_HEADER_SIZE);
			}
		}
      if(nSendLen > 0 && (m_nDumpMode&DUMP_FILEDST)==DUMP_FILEDST && m_SndFBuf.is_open())
      {
      	unsigned int tmpsendlen = htonl(nSendLen);
	     	m_SndFBuf.write((char*)&tmpsendlen,sizeof(int));
      	m_SndFBuf.write((char*)m_pSendPacket,nSendLen);
      }

      // Set RTCP Sender Report
      if(m_Params.nSndPayloadType==nPayloadType ||
         m_Params.nSndPayloadType2==nPayloadType) 
      {
         if(m_Params.remote.bRtcpOn && m_usRtcpMode) 
         {
            int nBody = nSendLen - sizeof(RTPHEADER);
            if(nBody <= 0) nBody  = 0;
            //m_pRtcpReport->PacketSent(nSendLen, m_nSendSeqNumber, uTimestamp);
            //CheckSendRtcpReport();
            //m_RtcpCtx.RTPSent(pHdr->seqnumber,pHdr->timestamp,nSendLen-sizeof(RTPHEADER));
            //m_RtcpCtx.RTPSent(nSeqNumber,uTimestamp,nSendLen-sizeof(RTPHEADER));
            m_RtcpCtx.RTPSent(nSeqNumber,uTimestamp,nBody);
         }
         m_nSendSeqNumber++;
      }
   }
   //else
   //LOCKREM m_Mutex.Unlock();
   m_nSendPacketSize = nSendLen;
   return nSendLen;
}

int CRtpComm::CheckSendRtcpReport()
{
   // Check RTCP Send & Send RTCP Report...
   return 0;
#if 0   
   int nPktSize = 0;
   int nSentLen;
   struct timeval tvCurTime;
   gettimeofday(&tvCurTime, NULL);
   int nDiffSysTimeMs = TvGetDiffMS(&tvCurTime, &m_tvRtcpPrevSendTime);

   if(m_Rtcp.snd.sockfd != INVALID_SOCKET && nDiffSysTimeMs > m_nRtcpSendIntervalMs) {
      m_pRtcpReport->MakeReport(m_pRecvPacket, m_nMaxRecvPacketLen, &nPktSize);
      if(nPktSize>0) {
         m_tvRtcpPrevSendTime = tvCurTime;
			int sockfd;
			if(m_Rtcp.snd.sockfd == -1)
				sockfd = m_Rtcp.rcv.sockfd;
			else
				sockfd = m_Rtcp.snd.sockfd;
         nSentLen = CAmtSock::SendTo(sockfd, m_Rtcp.snd.remote.ip, m_Rtcp.snd.remote.port, (char *)m_pRecvPacket, nPktSize);
         //printf("# RTCP MakeReport ==> len:(%d/%d), ip:%s, port:%d, diff_time:%d\n", nSentLen, nPktSize, 
         //               m_Rtcp.snd.remote.ip, m_Rtcp.snd.remote.port, nDiffSysTimeMs);
         return nSentLen;
      }
   }
   return -1;
#endif   
}


int CRtpComm::CheckVideoPacketLoss()
{
   unsigned int uSeq = m_uPrevRtpRcvSeq + 1;
   m_uPrevRtpRcvSeq = (unsigned int)htons(((RTPHEADER*)m_pRecvPacket)->seqnumber);
   if(uSeq != m_uPrevRtpRcvSeq) 
   {
      m_bReceivedIFrame = false;
      SendFIR(m_nFirMode);
      return 0;
   } 

   if(m_bReceivedIFrame == true) return 0;

   // Check I-Frame


   m_bReceivedIFrame = false;

   return false;
}

int CRtpComm::SetFIR(FIR_MODE nMode)
{
   m_nFirMode = nMode;
   return 0;
}

int CRtpComm::SendFIR(FIR_MODE nMode)
{
   struct timeval tvCurTime;
   gettimeofday(&tvCurTime, NULL);
   int nDiffSysTimeMs = TvGetDiffMS(&tvCurTime, &m_tvFIRSendTime);
   if(!m_bReceivedIFrame && nDiffSysTimeMs > 1000) 
   {
      if (m_nFirMode == FIRM_RTCP_FB_PLI) 
      {
         SendRtcpFbPLI();
      } 
      else 
      {
         SendRtcpH261FIR();
      }
      m_tvFIRSendTime = tvCurTime;
      m_bReceivedIFrame = false;
   } 
   return 0;
}


/*
    5.2.1.  Full INTRA-frame Request (FIR) packet

     0                   1                   2                   3
     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |V=2|P|   MBZ   |  PT=RTCP_FIR  |           length              |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                              SSRC                             |
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

   SSRC  : the synchronization source identifier for the sender of this packet
   PT    : RTCP_FIR(192).

  */

struct RTPH261FIR
{
#if(HUMT_GCONF_BIG_ENDIAN)
   unsigned char  version:2;
   unsigned char  padding:1;
   unsigned char  mbz:5;
   unsigned short length;
#else
	unsigned char  mbz:5;
	unsigned char  padding:1;
	unsigned char  version:2;
	unsigned char  payloadtype:8;
	unsigned short length;
#endif
   unsigned int   ssrc;
};


/*
6.1.   Common Packet Format for Feedback Messages

   All FB messages MUST use a common packet format that is depicted in
   Figure 3:

    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |V=2|P|   FMT   |       PT      |          length               |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                  SSRC of packet sender                        |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                  SSRC of media source                         |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   :            Feedback Control Information (FCI)                 :
   :                                                               :

           Figure 3: Common Packet Format for Feedback Messages


   version (V): The current version is 2.

   Feedback message type (FMT):
      0:     unassigned
      1:     Picture Loss Indication (PLI)
      2:     Slice Loss Indication (SLI)
      3:     Reference Picture Selection Indication (RPSI)
      4-14:  unassigned
      15:    Application layer FB (AFB) message
      16-30: unassigned
      31:    reserved for future expansion of the sequence number space

   Payload type (PT): 

            Name   | Value | Brief Description
         ----------+-------+------------------------------------
            RTPFB  |  205  | Transport layer FB message
            PSFB   |  206  | Payload-specific FB message

   Length: 
      The length of this packet in 32-bit words minus one, including the
      header and any padding.  This is in line with the definition of
      the length field used in RTCP sender and receiver reports [3].

   SSRC of packet sender: 32 bits
      The synchronization source identifier for the originator of this
      packet.

   SSRC of media source: 32 bits
      The synchronization source identifier of the media source that
      this piece of feedback information is related to.

   Feedback Control Information (FCI): variable length


6.3.1.  Picture Loss Indication (PLI)

   The PLI FB message is identified by PT=PSFB and FMT=1.

  */

#define PT_RTCP_H261FIR 192 // rfc 2032 : RTP Payload Format for H.261 Video Streams 
#define PT_RTCP_FB		205 // RFC 4585 : Real-time Transport Control Protocol (RTCP)-Based Feedback (RTP/AVPF)
#define PT_RTCP_PSFB	206 // RFC 4585 : Real-time Transport Control Protocol (RTCP)-Based Feedback (RTP/AVPF) 

int CRtpComm::SendRtcpH261FIR() 
{
   //nMode : H261 FIR,   RTCP-fb PLI

   int nSendLen = 0;
   //m_Mutex.Lock();
   if(m_bStarted) {
      RTPH261FIR msg;
      msg.version       = 2;
      msg.mbz           = 0;
      msg.padding       = 0;
      msg.payloadtype   = PT_RTCP_H261FIR;
      msg.length        = 1;
      msg.ssrc          = m_Params.nSSRC;
		int sockfd;
		if(m_Rtp.snd.remote.ver == 1)
		{
			if(m_Rtp.snd.sockfd == -1)
				sockfd = m_Rtp.rcv.sockfd;
			else
	  			sockfd = m_Rtp.snd.sockfd;
         // TODO:: remote nat
         if(m_RtpTarget.port!=0)
            nSendLen = CAmtSock::SendTo(sockfd, m_RtpTarget.ip, m_RtpTarget.port, (char *)&msg, sizeof(msg));
		}
		else
		{
			if(m_Rtp.snd.sockfd6 == -1)
				sockfd = m_Rtp.rcv.sockfd6;
			else
				sockfd = m_Rtp.snd.sockfd6;
			nSendLen = CAmtSock6::SendTo(sockfd, m_Rtp.snd.remote.ip6, m_Rtp.snd.remote.port, (char *)&msg, sizeof(msg));
		}
   }
   //m_Mutex.Unlock();
   return nSendLen;
   
} 

int CRtpComm::SendRtcpFbPLI() 
{
   int nSendLen = 0;
   //m_Mutex.Lock();
   if(m_bStarted) {
      RTCPFBPLI msg;
      msg.version       = 2;
      msg.fmt           = 1;
      msg.padding       = 0;
      msg.payloadtype   = PT_RTCP_PSFB;
      msg.length        = 2;
      msg.mssrc         = m_Params.nSSRC;  // media source ssrc  ???
      msg.pssrc         = m_Params.nSSRC;  // packet sender ssrc
      int sockfd;
		if(m_Rtp.snd.remote.ver == 1)
		{
			if(m_Rtp.snd.sockfd == -1)
				sockfd = m_Rtp.rcv.sockfd;
			else
				sockfd = m_Rtp.snd.sockfd;
         // TODO:: remote nat
         if(m_RtpTarget.port!=0)
            nSendLen = CAmtSock::SendTo(sockfd, m_RtpTarget.ip, m_RtpTarget.port, (char *)&msg, sizeof(msg));
		}
		else
		{
			if(m_Rtp.snd.sockfd6 == -1)
				sockfd = m_Rtp.rcv.sockfd6;
			else
				sockfd = m_Rtp.snd.sockfd6;
			nSendLen = CAmtSock6::SendTo(sockfd, m_Rtp.snd.remote.ip6, m_Rtp.snd.remote.port, (char *)&msg, sizeof(msg));
		}
   }
   //m_Mutex.Unlock();
   return nSendLen;   
} 

CRtpComm::PACKET_TYPE CRtpComm::CheckRtpPacket(const unsigned char * pPkt, int nLen)
{
   RTPHEADER * pHdr = (RTPHEADER *)pPkt;
   if(nLen > (int)sizeof(RTPHEADER) ) {
      if(pHdr->payloadtype == m_Params.nRcvPayloadType || 
         pHdr->payloadtype == m_Params.nRcvPayloadType2) {
         return PT_RTP;
     } else if (pHdr->payloadtype == (PT_RTCP_PSFB&0x7f) //???
                  && ((RTCPFBPLI*)pPkt)->fmt == 1 
                  && ((RTCPFBPLI*)pPkt)->fmt == 2) {
         return PT_FB_PLE;
      } else {
         return PT_RTP_UNKNOWN;
      }
   } else {
      if(nLen == 8 && pHdr->payloadtype == (PT_RTCP_H261FIR&0x7f)) {//???
         return PT_H261FIR;
      }
   }
   return PT_RTP_DROP;
}


CRtpComm::PACKET_TYPE CRtpComm::CheckRtcpPacket(const unsigned char * pPkt, int nLen)
{
//   RTPHEADER * pHdr = (RTPHEADER *)pPkt;
   if(nLen > (int)sizeof(RTPHEADER) ) {
      if(/*pHdr->payloadtype == PT_RTCP_PSFB //remove wrong clause..this is always false
         &&*/ ((RTCPFBPLI*)pPkt)->fmt == 1 
         && ((RTCPFBPLI*)pPkt)->fmt == 2) {
         return PT_FB_PLE;
      } else {
         return PT_RTCP;
      }
   } else {
      if(nLen == 8 /*&& pHdr->payloadtype == PT_RTCP_H261FIR*/ ) {//remove wrong clause..this is alway false
         return PT_H261FIR;
      }
   }

   return PT_RTCP_DROP;
}

int CRtpComm::RTCPSendSR(int mode)
{
   PEER peer;
   memset(&peer, 0, sizeof(PEER));
   peer = m_RtcpTarget;

   unsigned int pktlen=0;

   pktlen = m_RtcpCtx.BuildRTCPPacket((char*)m_pRtcpSendPacket,m_nMaxSendPacketLen,m_Params.nSSRC,
      mode,(char*)(m_Params.cname));
#ifdef DEBUG_MODE
   fprintf(stderr,"%s pktlen:%d mode:%d \n",__func__, pktlen,mode);
#endif
   int sockfd;
   if(pktlen<=0)
   {
      //LOGGER(PL_CRITICAL,"m_RtcpCtx.BuildSRPacket Failed.retc=%d\n",pktlen);
      //UPDSTATS(total.rtcp_sr_buildfail);
      return -1 ;
   }

   if(m_Rtcp.snd.remote.ver == 1)
   {
      if(m_Rtcp.snd.sockfd == -1)
         sockfd = m_Rtcp.rcv.sockfd;
      else
         sockfd = m_Rtcp.snd.sockfd;
      // TODO:: remote nat
      if(peer.port!=0)
         m_nRtcpSendPacketSize = CAmtSock::SendTo(sockfd, peer.ip, peer.port, (char *)m_pRtcpSendPacket, pktlen);
   }
   else
   {
      if(m_Rtcp.snd.sockfd == -1)
         sockfd = m_Rtcp.rcv.sockfd6;
      else
         sockfd = m_Rtcp.snd.sockfd6;
      m_nRtcpSendPacketSize = CAmtSock6::SendTo(sockfd, m_Rtcp.snd.remote.ip6, m_Rtcp.snd.remote.port,
            (char*)m_pRtcpSendPacket, pktlen);
   }
#ifdef DEBUG_MODE
   fprintf(stderr,"%s ==>%s:%d len:%d %d fd:%d\n",__func__,m_Rtcp.snd.remote.ip,m_Rtcp.snd.remote.port,
         pktlen,m_nRtcpSendPacketSize,sockfd);
#endif
   if(m_nRtcpSendPacketSize<0)
   {
      //UPDSTATS(total.tx_sockerr);
      /*        
                LOGGER(PL_ERROR,"RTCP_SR sendto %d.%d.%d.%d:%d send failed.errno=%d",
                PRINTF_IPV_NO(m_RtcpChannel.remoteaddr.sin_addr.s_addr),
                ntohs(m_RtcpChannel.remoteaddr.sin_port),errno);
                UPDSTATS(total.rtcp_tx_sockerr);
       */        
      return -2 ;
   }

   //UPDSTATS(total.rtcp_tx_sr);
   /*
      LOGGER(PL_API,"RTCPSendSR %d.%d.%d.%d:%d %d bytes success",
      PRINTF_IPV_NO(m_RtcpChannel.remoteaddr.sin_addr.s_addr),
      ntohs(m_RtcpChannel.remoteaddr.sin_port),pktlen);

      LoggingDumpRtcpTx((pibyte*)packet,pktlen);
    */
   return 1 ;
}

int CRtpComm::RTCPSendRR(int mode)
{
   PEER peer;
   memset(&peer, 0, sizeof(PEER));
   peer = m_RtcpTarget;

   unsigned int pktlen=0;

   pktlen = m_RtcpCtx.BuildRTCPPacket((char*)m_pRtcpSendPacket,m_nMaxSendPacketLen,m_Params.nSSRC,
      mode,(char*)(m_Params.cname));
#ifdef DEBUG_MODE
   fprintf(stderr,"%s pktlen:%d mode:%d \n",__func__, pktlen,mode);
#endif
   int sockfd;
   if(pktlen<=0)
   {
      //LOGGER(PL_CRITICAL,"m_RtcpCtx.BuildRRPacket Failed.retc=%d\n",pktlen);
      //UPDSTATS(total.rtcp_sr_buildfail);
      return -1 ;
   }

   if(m_Rtcp.snd.remote.ver == 1)
   {
      if(m_Rtcp.snd.sockfd == -1)
         sockfd = m_Rtcp.rcv.sockfd;
      else
         sockfd = m_Rtcp.snd.sockfd;
      // TODO:: remote nat
      if(peer.port!=0)
         m_nRtcpSendPacketSize = CAmtSock::SendTo(sockfd, peer.ip, peer.port, (char *)m_pRtcpSendPacket, pktlen);
   }
   else
   {
      if(m_Rtcp.snd.sockfd6 == -1)
         sockfd = m_Rtcp.rcv.sockfd6;
      else
         sockfd = m_Rtcp.snd.sockfd6;
      m_nRtcpSendPacketSize = CAmtSock6::SendTo(sockfd, m_Rtcp.snd.remote.ip6, m_Rtcp.snd.remote.port, 
            (char*)m_pRtcpSendPacket, pktlen);
   }
#ifdef DEBUG_MODE
   fprintf(stderr,"%s ==>%s:%d len:%d %d fd:%d\n",__func__,m_Rtcp.snd.remote.ip,m_Rtcp.snd.remote.port,
         pktlen,m_nRtcpSendPacketSize,sockfd);
#endif
   if(m_nRtcpSendPacketSize<0)
   {
      //UPDSTATS(total.tx_sockerr);
      /*        
                LOGGER(PL_ERROR,"RTCP_RR sendto %d.%d.%d.%d:%d send failed.errno=%d",
                PRINTF_IPV_NO(m_RtcpChannel.remoteaddr.sin_addr.s_addr),
                ntohs(m_RtcpChannel.remoteaddr.sin_port),errno);
                UPDSTATS(total.rtcp_tx_sockerr);
       */        
      return -2 ;
   }
   /*
      UPDSTATS(total.rtcp_tx_rr);

      LOGGER(PL_API,"RTCPSendRR %d.%d.%d.%d:%d %d bytes success",
      PRINTF_IPV_NO(m_RtcpChannel.remoteaddr.sin_addr.s_addr),
      ntohs(m_RtcpChannel.remoteaddr.sin_port),pktlen);

      LoggingDumpRtcpTx((pibyte*)packet,pktlen);

   // Monitor Trace
   g_CallMonPtr->RtcpTraceCheck(
   &m_TraceKey,
   (pibyte*)packet,(unsigned int)pktlen,
   1,
   &m_RtcpChannel.bindaddr,
   &m_RtcpChannel.remoteaddr);
    */
   return 1 ;
}

int CRtpComm::Timming_RtcpSR(unsigned long timestamp,int rtcpmode,int interval){
   if(!m_usRtcpMode ||interval == 0) return 0;
   int rc=0;
   if(m_RtcpSRSendLastMS)
   {
      if(timestamp>=m_RtcpSRSendLastMS)
      {
         rc = RTCPSendSR(rtcpmode);
#ifdef DEBUG_MODE
         fprintf(stderr,"SendSR rc=%d ts:%u mode:%d interval:%d \n",
            rc, timestamp, rtcpmode, interval);
#endif
         m_RtcpSRSendLastMS = timestamp + interval;//interval ms
      }
   }
   else m_RtcpSRSendLastMS = timestamp + interval;
   return rc;
}

int CRtpComm::Timming_RtcpRR(unsigned long timestamp,int rtcpmode,int interval){
   if(!m_usRtcpMode || interval == 0) return 0;
   int rc=0;
   if(m_RtcpRRSendLastMS)
   {
      if(timestamp>=m_RtcpRRSendLastMS)
      {
         rc = RTCPSendRR(rtcpmode);
#ifdef DEBUG_MODE
         fprintf(stderr,"SendRR rc=%d ts:%u mode:%d interval:%d \n",
            rc, timestamp, rtcpmode, interval);
#endif
         m_RtcpRRSendLastMS = timestamp + interval;//interval ms
      }
   }
   else m_RtcpRRSendLastMS = timestamp + interval;
   return rc ;
}

}; //namespace

