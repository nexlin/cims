/**
 *
 **/
#include <stdlib.h>
#include <poll.h>
#include "amtsock.h"
#include "amtatmcomm.h"
#include "humterr.h"
#include "util.h"

#define AMT_ATM_TICKER_INTERVAL  10
#define HAV_ATM_TICKER_DEBUG     0
namespace AMT
{
int CATMComm::m_nMaxRecvPacketLen = CATMComm::MAX_UDP_PACKET_SIZE;
int CATMComm::m_nMaxSendPacketLen = CATMComm::MAX_UDP_PACKET_SIZE;

CATMComm::CATMComm()
{
   m_pRecvPacket = new unsigned char[CATMComm::MAX_UDP_PACKET_SIZE];
   m_bStarted  = false;
   m_pRtpReceived = NULL;
   memset(&m_sock,  0, sizeof(BIND));
   m_sock.sockfd = INVALID_SOCKET;
   m_sock.sockfd6 = INVALID_SOCKET;
}

CATMComm::~CATMComm()
{
   Clear();
   
   if(m_pRecvPacket) {
      delete [] m_pRecvPacket;
      m_pRecvPacket = NULL;
   } 
}

int CATMComm::Clear()
{
   ClearSession();
	if(m_sock.sockfd != INVALID_SOCKET)
   	CAmtSock::Close(&m_sock.sockfd);
	if(m_sock.sockfd6 != INVALID_SOCKET)
   	CAmtSock::Close(&m_sock.sockfd6);
   memset(&m_sock,  0, sizeof(BIND));
   return 0;
}

int CATMComm::OpenSocket(const PEER *pLocalRecvInfo)
{
   int rc = 0;

	if(!pLocalRecvInfo)	return -1;
   //m_Mutex.Lock();
   Clear();
	
	switch(pLocalRecvInfo->ver)
	{
	case 3://ipv4 && ipv6 socket open
		strncpy(m_sock.local.ip,pLocalRecvInfo->ip,MAX_IP_ADDR);
		m_sock.sockfd = CAmtSock::Socket(CAmtSock::UDP);
		if(m_sock.sockfd == INVALID_SOCKET)
		{
			rc = -1;
			break;
		}
		if(CAmtSock::Bind(m_sock.sockfd, m_sock.local.ip,m_sock.local.port) == false) {
			rc = -2;
			break;
		}
		
		CAmtSock::SetRecvBufferSize(m_sock.sockfd,MAX_UDP_PACKET_SIZE);

		strncpy(m_sock.local.ip6,pLocalRecvInfo->ip6,MAX_IP6_ADDR);
		m_sock.sockfd6 = CAmtSock6::Socket(CAmtSock::UDP);
		if(m_sock.sockfd6 == INVALID_SOCKET)
		{
			rc = -1;
			break;
		}
		if(CAmtSock::Bind(m_sock.sockfd6, m_sock.local.ip6,m_sock.local.port) == false) {
			rc = -2;
			break;
		}
		
		CAmtSock::SetRecvBufferSize(m_sock.sockfd6,MAX_UDP_PACKET_SIZE);
	case 2://ipv6
		strncpy(m_sock.local.ip6,pLocalRecvInfo->ip6,MAX_IP6_ADDR);
		m_sock.sockfd6 = CAmtSock6::Socket(CAmtSock::UDP);
		if(m_sock.sockfd6 == INVALID_SOCKET)
		{
			rc = -1;
			break;
		}
		if(CAmtSock::Bind(m_sock.sockfd6, m_sock.local.ip6,m_sock.local.port) == false) {
			rc = -2;
			break;
		}
		
		CAmtSock::SetRecvBufferSize(m_sock.sockfd6,MAX_UDP_PACKET_SIZE);
	case 1://ipv4
	default:
		strncpy(m_sock.local.ip,pLocalRecvInfo->ip,MAX_IP_ADDR);
		m_sock.sockfd = CAmtSock::Socket(CAmtSock::UDP);
		if(m_sock.sockfd == INVALID_SOCKET)
		{
			rc = -1;
			break;
		}
		if(CAmtSock::Bind(m_sock.sockfd, m_sock.local.ip,m_sock.local.port) == false) {
			rc = -2;
			break;
		}
		
		CAmtSock::SetRecvBufferSize(m_sock.sockfd,MAX_UDP_PACKET_SIZE);
		break;
	}

   if(rc != 0) {
      Clear();
   }
   //m_Mutex.Unlock();

   return rc;
}

int CATMComm::StartSession(const ATM_PARAM *pSendParam)
{
   int rc = 0;
   ClearSession();
   do {
      m_Params = *pSendParam;
      // realloc recv-packet buffer 
      if(m_pRecvPacket) delete [] m_pRecvPacket;
      m_pRecvPacket = new unsigned char[m_nMaxRecvPacketLen];
      if(m_pSendPacket) delete [] m_pSendPacket;
      m_pSendPacket = new unsigned char[m_nMaxSendPacketLen];

		memcpy(&m_sock.remote,&m_Params.remote,sizeof(PEER));
   } while(false);

   if(rc == 0) {
      m_bStarted = true;
   } else {
      printf("Error CATMComm::StartSession(), rc=%d\n", rc); 
      ClearSession();
   }
   return rc;
}

int CATMComm::StartSession_DUMP(const ATM_PARAM *pSendParam,const DUMP_PARAM* pDumpParam)
{//dump file name must has struct path + base filename --> "../dump/rtp"
	if(!pDumpParam) return hutcStsInvalidArgument;
	m_nDumpMode = pDumpParam->nDumpMode;
	memcpy(&m_DumpRcvRelay,&pDumpParam->rcv_relay,sizeof(PEER));
	memcpy(&m_DumpSndRelay,&pDumpParam->snd_relay,sizeof(PEER));
	
	switch(m_nDumpMode & 0xf0)
	{
	case DUMP_FILESRC:
	do
	{
		std::ostringstream rcvfilename;

		rcvfilename<<pDumpParam->szFileBase<<".precv";
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
		sndfilename<<pDumpParam->szFileBase<<".psend";
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
		rcvfilename<<pDumpParam->szFileBase<<".precv";
		sndfilename<<pDumpParam->szFileBase<<".psend";
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

int CATMComm::ModifySession(const ATM_PARAM *pSendParam)
{//after StartSession if need modify SendParam then call this func
   //m_Mutex.Lock();
   if(!m_bStarted) return -1;

   int rc = 0;
   do {
      m_Params = *pSendParam;
		memcpy(&m_sock.remote,&m_Params.remote,sizeof(PEER));
   } while(false);

   if(rc != 0) {
      printf("Error CATMComm::ModifySession(), rc=%d\n", rc); 
   }
   //m_Mutex.Unlock();
   return rc;
}

int CATMComm::ModifySession_DUMP(const ATM_PARAM *pSendParam,const DUMP_PARAM* pDumpParam)
{//dump file name must has struct path + base filename --> "../dump/rtp"
	if(!pDumpParam) return hutcStsInvalidArgument;
	m_nDumpMode = pDumpParam->nDumpMode;
	memcpy(&m_DumpRcvRelay,&pDumpParam->rcv_relay,sizeof(PEER));
	memcpy(&m_DumpSndRelay,&pDumpParam->snd_relay,sizeof(PEER));
	
	switch(m_nDumpMode & 0xf0)
	{
	case DUMP_FILESRC:
	do
	{
		std::ostringstream rcvfilename;
		rcvfilename<<pDumpParam->szFileBase<<".precv";
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
		sndfilename<<pDumpParam->szFileBase<<".psend";
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
		sndfilename<<pDumpParam->szFileBase<<".psend";
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

int CATMComm::ClearSession()
{
   m_pRtpReceived = NULL;
   m_bStarted = false;
   return 0;
}

int CATMComm::EndSession()
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
   ClearSession();
   //m_Mutex.Unlock();
   return 0;
}

int CATMComm::CloseSocket()
{
   //m_Mutex.Lock();
   Clear();
   //m_Mutex.Unlock();
   return 0;
}

int CATMComm::SockSelect(int nTimeout)
{
	int sockfd = INVALID_SOCKET;
	int rc=-1;
   do {
      if(m_sock.sockfd == INVALID_SOCKET)
         break;
      fd_set  fd_read;
      timeval sel_timeout;
      int     selnum;

      FD_ZERO(&fd_read);
		if(m_sock.remote.ver == 1)
			sockfd = m_sock.sockfd;
		else
			sockfd = m_sock.sockfd6;

      if(sockfd != INVALID_SOCKET) {
         FD_SET((unsigned)sockfd, &fd_read);
      }

      if(nTimeout >= 0) {
         sel_timeout.tv_sec = nTimeout / 1000;
         sel_timeout.tv_usec = (nTimeout % 1000) * 1000;
         selnum = select(sockfd + 1, &fd_read, 0, 0, &sel_timeout);
      } else {
         selnum = select(sockfd + 1, &fd_read, 0, 0, NULL);
      }
		rc = selnum;
   } while(false);
   return rc;
}
#ifdef USING_POLL
int CATMComm::SockPoll(int nTimeout)
{
	int rc = 0;
	int sockfd = (m_sock.remote.ver == 1)?m_sock.sockfd:m_sock.sockfd6;
   do {
      if(sockfd == INVALID_SOCKET)
         break;
		struct pollfd fds[1];
      int     selnum;

		fds[0].fd = sockfd;
		fds[0].events = POLLIN | POLLPRI;

		selnum = poll(fds,1,nTimeout);
		rc = selnum;
   } while(false);
   return rc;
}
#endif //USING_POLL

int CATMComm::DirectRecv(int nTimeout,bool bepoll)
{
   int nRecvLen, rc=1;
   if(!bepoll)
   {
#ifdef USING_POLL
   rc = SockPoll(nTimeout);
#else
   rc = SockSelect(nTimeout);
#endif
	}
	
	if(m_sock.remote.ver == 1)
	{
		if(rc>0)
		{
			nRecvLen = CAmtSock::RecvFrom(m_sock.sockfd, m_pRecvPacket, m_nMaxRecvPacketLen, 0, m_sock.remote.ip, &m_sock.remote.port);
			if(nRecvLen < 0) return nRecvLen;
			if(nRecvLen > 0 && (m_nDumpMode&DUMP_RELAYSRC)==DUMP_RELAYSRC)
			{
				CAmtSock::SendTo(m_sock.sockfd, m_DumpRcvRelay.ip, m_DumpRcvRelay.port, (char *)m_pRecvPacket, nRecvLen);
			}
			//LOCKREM m_Mutex.Unlock();
			if(nRecvLen > 0 && (m_nDumpMode&DUMP_FILESRC)==DUMP_FILESRC && m_RcvFBuf.is_open())
			{
				unsigned int tmprecvlen = htonl(nRecvLen);
				m_RcvFBuf.write((char*)&tmprecvlen,sizeof(int));
				m_RcvFBuf.write((char*)m_pRecvPacket,nRecvLen);
			}
			m_pRtpReceived = m_pRecvPacket;
			m_nRtpRecvSize = nRecvLen;
		} 
	}
	else
	{
			nRecvLen = CAmtSock6::RecvFrom(m_sock.sockfd6, m_pRecvPacket, m_nMaxRecvPacketLen, 0, m_sock.remote.ip6, &m_sock.remote.port);
			if(nRecvLen < 0) return nRecvLen;
			if(nRecvLen > 0 && (m_nDumpMode&DUMP_RELAYSRC)==DUMP_RELAYSRC)
			{
				CAmtSock6::SendTo(m_sock.sockfd6, m_DumpRcvRelay.ip6, m_DumpRcvRelay.port, (char *)m_pRecvPacket, nRecvLen);
			}
			//LOCKREM m_Mutex.Unlock();
			if(nRecvLen > 0 && (m_nDumpMode&DUMP_FILESRC)==DUMP_FILESRC && m_RcvFBuf.is_open())
			{
				unsigned int tmprecvlen = htonl(nRecvLen);
				m_RcvFBuf.write((char*)&tmprecvlen,sizeof(int));
				m_RcvFBuf.write((char*)m_pRecvPacket,nRecvLen);
			}
			m_pRtpReceived = m_pRecvPacket;
			m_nRtpRecvSize = nRecvLen;
	} 
   return nRecvLen;
}

unsigned char * CATMComm::GetPacket(int *pnOutLength)
{
   unsigned char * ptr = NULL;
   //m_Mutex.Lock();
   if(m_bStarted) {
      ptr = m_pRtpReceived;
      *pnOutLength = m_nRtpRecvSize;
   }
   //m_Mutex.Unlock();
   return ptr;
}

unsigned char * CATMComm::GetSendPacket(int *pnOutLength)
{
   unsigned char * ptr = NULL;
   //m_Mutex.Lock();
   if(m_bStarted) {
      ptr = m_pSendPacket;
      *pnOutLength = m_nSendPacketSize;
   }
   //m_Mutex.Unlock();
   return ptr;
}

int CATMComm::PutPacket(const unsigned char* pInData, int nInLength)
{
   int nSendLen = 0;
   //LOCKREM m_Mutex.Lock();
   if(m_bStarted) {
		if(m_sock.remote.ver == 1)
		{
			nSendLen = CAmtSock::SendTo(m_sock.sockfd, m_sock.remote.ip, m_sock.remote.port, (char *)pInData, nInLength);
			if(nSendLen > 0 && (m_nDumpMode&DUMP_RELAYDST)==DUMP_RELAYDST)
			{
				CAmtSock::SendTo(m_sock.sockfd, m_DumpSndRelay.ip, m_DumpSndRelay.port, (char *)pInData, nInLength);
			}
			if(nSendLen > 0 && (m_nDumpMode&DUMP_FILEDST)==DUMP_FILEDST && m_SndFBuf.is_open())
			{
				unsigned int tmpsendlen = htonl(nSendLen);
				m_SndFBuf.write((char*)&tmpsendlen,sizeof(int));
				m_SndFBuf.write((char*)pInData,nSendLen);
			}
		}
		else
		{
			nSendLen = CAmtSock6::SendTo(m_sock.sockfd6, m_sock.remote.ip6, m_sock.remote.port, (char *)pInData, nInLength);
			if(nSendLen > 0 && (m_nDumpMode&DUMP_RELAYDST)==DUMP_RELAYDST)
			{
				CAmtSock::SendTo(m_sock.sockfd6, m_DumpSndRelay.ip6, m_DumpSndRelay.port, (char *)pInData, nInLength);
			}
			if(nSendLen > 0 && (m_nDumpMode&DUMP_FILEDST)==DUMP_FILEDST && m_SndFBuf.is_open())
			{
				unsigned int tmpsendlen = htonl(nSendLen);
				m_SndFBuf.write((char*)&tmpsendlen,sizeof(int));
				m_SndFBuf.write((char*)pInData,nSendLen);
			}

		}
   }
   //LOCKREM m_Mutex.Unlock();
   memcpy(m_pSendPacket,pInData, nInLength);
   m_nSendPacketSize = nSendLen;
   return nSendLen;

}
}; //namespace
////@}
