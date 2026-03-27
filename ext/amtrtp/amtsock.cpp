
#define _POSIX_SOURCE 1

#define _XOPEN_SOURCE_EXTENDED 1

#include <stdio.h>
#include <string.h>
#ifdef _MSC_VER
   #include <io.h>
   #include "pdkutil.h"
#else
   #include "util.h"
	#include <unistd.h>
	#include <fcntl.h>
	#include <signal.h>
	#include <poll.h>
	#include <sys/types.h>
	#include <sys/socket.h>
#endif //_MSC_VER
#ifndef WIN32
#include <net/if.h>
#else
#include <WS2tcpip.h>
#endif
#include "amtsock.h"

#define SOCKET_SND_RCV_BUFFER   16384*10

#ifndef SOCKLEN_T 
#define SOCKLEN_T 	size_t 
#endif 

#ifndef TCP_NODELAY
#define TCP_NODELAY     0x1
#endif //TCP_NODELAY


#ifdef WIN32
#define NS_INT16SZ       2
#define NS_INADDRSZ      4
#define NS_IN6ADDRSZ    16
 
static int inet_pton4( const char *src, unsigned char *dst )
{
 static const char digits[] = "0123456789";
  int saw_digit, octets, ch;
  unsigned char tmp[NS_INADDRSZ], *tp;

  saw_digit = 0;
  octets = 0;
  *(tp = tmp) = 0;
  while( (ch = *src++) != '\0' )
  {
    const char *pch;

    if( (pch = strchr(digits, ch)) != NULL )
    {
      unsigned int iNew = (unsigned int)(*tp * 10 + (pch - digits));

      if( iNew > 255 ) return (0);
      *tp = iNew;
      if( !saw_digit )
      {
        if( ++octets > 4 ) return (0);
        saw_digit = 1;
      }
    }
    else if( ch == '.' && saw_digit )
    {
       if( octets == 4 ) return (0);
      *++tp = 0;
       saw_digit = 0;
    }
    else
    {
      return (0);
    }
  }

  if( octets < 4 ) return (0);

  memcpy( dst, tmp, NS_INADDRSZ );

  return (1);
}

static int inet_pton6( const char *src, unsigned char * dst )
{
  static const char xdigits_l[] = "0123456789abcdef",
                    xdigits_u[] = "0123456789ABCDEF";
  unsigned char tmp[NS_IN6ADDRSZ], *tp, *endp, *colonp;
  const char *xdigits, *curtok;
  int ch, saw_xdigit;
  unsigned int val;

  memset((tp = tmp), '\0', NS_IN6ADDRSZ);
  endp = tp + NS_IN6ADDRSZ;
  colonp = NULL;

  // Leading :: requires some special handling.
  if( *src == ':' && *++src != ':' ) return (0);

  curtok = src;
  saw_xdigit = 0;
  val = 0;
  while( ( ch = *src++ ) != '\0' )
  {
    const char *pch;

    if( ( pch = strchr( ( xdigits = xdigits_l ), ch ) ) == NULL )
      pch = strchr( (xdigits = xdigits_u), ch );

    if( pch != NULL )
    {
      val <<= 4;
      val |= (pch - xdigits);
      if( val > 0xffff ) return (0);
      saw_xdigit = 1;
      continue;
    }
 
    if( ch == ':' )
     {
       curtok = src;
      if( !saw_xdigit )
       {
         if( colonp ) return (0);
         colonp = tp;
         continue;
       }
 
       if( tp + NS_INT16SZ > endp ) return (0);
 
       *tp++ = (unsigned char) (val >> 8) & 0xff;
       *tp++ = (unsigned char) val & 0xff;
       saw_xdigit = 0;
       val = 0;
       continue;
    }
 
     if( ch == '.' && ((tp + NS_INADDRSZ) <= endp) && inet_pton4(curtok, tp) > 0 )
     {
		 tp += NS_INADDRSZ;
       saw_xdigit = 0;
       break;  /* '\0' was seen by inet_pton4(). */
     }
     return (0);
   }
 
   if( saw_xdigit )
   {
     if( tp + NS_INT16SZ > endp ) return (0);
 
     *tp++ = (unsigned char) (val >> 8) & 0xff;
    *tp++ = (unsigned char) val & 0xff;
   }
 
   if( colonp != NULL )
   {
     // Since some memmove()'s erroneously fail to handle overlapping regions, we'll do the shift by hand.
     const int n = (int)(tp - colonp);
     int i;

     for( i = 1; i <= n; i++ )
    {
      endp[- i] = colonp[n - i];
       colonp[n - i] = 0;
     }
     tp = endp;
   }
 
   if( tp != endp ) return (0);
   
   memcpy(dst, tmp, NS_IN6ADDRSZ);
   return (1);
 }
 
 int inet_pton( int af, const char *src, void *dst )
 {
   switch (af)
   {
   case AF_INET:
     return inet_pton4( src, (unsigned char *)dst );
   case AF_INET6:
     return inet_pton6( src, (unsigned char *)dst );
   default:
     errno = EAFNOSUPPORT;
     return (-1);
   }
 }

 const char* inet_ntop(int af, const void* src, char* dst, int cnt)
{
    struct sockaddr_in srcaddr;
    memset(&srcaddr, 0, sizeof(struct sockaddr_in));
    memcpy(&(srcaddr.sin_addr), src, sizeof(srcaddr.sin_addr));
    srcaddr.sin_family = af;
    if (WSAAddressToString((struct sockaddr*) &srcaddr, sizeof(struct sockaddr_in), 0, dst, (LPDWORD) &cnt) != 0) 
    {
        DWORD rv = WSAGetLastError();     
        return NULL; 
    }
    return dst;
}

#endif



namespace AMT
{

#ifdef _MSC_VER

typedef unsigned long       DWORD;
typedef int                 BOOL;
typedef unsigned char       BYTE;
typedef unsigned short      WORD;

bool CAmtSock::m_bInitialized = false;
//////////////////////////////////////////////////////////////////////////////
bool bSockInitialized = CAmtSock::InitSock();

bool CAmtSock::InitSock()
{
	WORD wVersionRequested;
	WSADATA wsaData;
	int err;
 
	if(m_bInitialized == false) {
		m_bInitialized = true;
		wVersionRequested = MAKEWORD( 2, 0 );
 
		err = WSAStartup( wVersionRequested, &wsaData );
		if ( err != 0 ) {
			/* Tell the user that we couldn't find a usable */
			/* WinSock DLL.                                  */
	//		ShowMessage("couldn't find a usable WinSock DLL", err);
			return false;
		}
 
		/* Confirm that the WinSock DLL supports 2.0.*/
		/* Note that if the DLL supports versions greater    */
		/* than 2.0 in addition to 2.0, it will still return */
		/* 2.0 in wVersion since that is the version we      */
		/* requested.                                        */
 
		if ( LOBYTE( wsaData.wVersion ) != 2 ||
				HIBYTE( wsaData.wVersion ) != 0 ) {
			/* Tell the user that we couldn't find a usable */
			/* WinSock DLL.                                  */
			WSACleanup( );
			return false; 
		}
	}
	return true; 
}
#endif // _MSC_VER


int CAmtSock::Socket(int nSockType) 
{
   int fdsock = INVALID_SOCKET;
   if(nSockType == UDP) {
      fdsock = socket(AF_INET, SOCK_DGRAM, 0);
   } else if (nSockType == TCP) {
      fdsock = socket(AF_INET, SOCK_STREAM, 0);
   }
   if (fdsock == INVALID_SOCKET) {
      //m_pLogger->Log(CHuLogger::Critical, "socket creation error:%d:%s", errno, strerror(errno));
   } else {
      int nSockOpt = 1;
      if (setsockopt(fdsock, SOL_SOCKET, SO_REUSEADDR, (char*)&nSockOpt, sizeof(nSockOpt)) != 0) {
      //   m_pLogger->Log(CHuLogger::Critical, "Listener setsockopt(SO_REUSEADDR) ERROR(%d,%s)", errno, strerror(errno));
      }
   }
//set non-block mode
#ifdef WIN32
   PDKULONG iMode = 1;
   ioctlsocket(fdsock, FIONBIO, &iMode);
#else // !WIN32
   int     nFlags;
   nFlags = fcntl(fdsock, F_GETFL, 0);
   fcntl(fdsock, F_SETFL, nFlags | O_NONBLOCK);
#endif // WIN32   
   return fdsock;
}

bool CAmtSock::SetNoDelay(int sockfd, bool bTrue) 
{
   int nValue = bTrue?1:0;
   if(setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char*)&nValue, sizeof(nValue)) != 0) {
    //  m_pLogger->Log(CHuLogger::Info, "TCP_NODELAY ERROR(%d,%s)\n", errno, strerror(errno));
      return false;
   }
   return true;
}

typedef void (*SIG_FUNC)(int);

static void hb_connect_alarm(int signo){}

bool CAmtSock::TryAssignConn(int* pSockfd, LPCSTR szAddr, int nPort, int  nSec)
{
   *pSockfd = Socket(TCP);
   if(*pSockfd == INVALID_SOCKET) return false;
   
   struct sockaddr_in sockAddr;
   memset(&sockAddr,0,sizeof(sockAddr));

   sockAddr.sin_family = AF_INET;
   sockAddr.sin_addr.s_addr = inet_addr(szAddr);
   sockAddr.sin_port = htons((unsigned short)nPort);

   int rc = -1;

#ifndef _WIN32
   bool bAlarm = (nSec > 0);
   SIG_FUNC sigfunc;

   if(bAlarm) {
      if(alarm(nSec) == 0) {
         sigfunc = signal(SIGALRM, hb_connect_alarm);
      } else {
         bAlarm = false;
         signal(SIGALRM, sigfunc);
         //DLOG("Alarm was already set in connecting to server");
      }
   }
#endif //_WIN32

//set non-block mode
#ifdef WIN32
		PDKULONG iMode = 1;
		ioctlsocket(*pSockfd, FIONBIO, &iMode);
#else // !WIN32
		int	  nFlags;
		nFlags = fcntl(*pSockfd, F_GETFL, 0);
		fcntl(*pSockfd, F_SETFL, nFlags | O_NONBLOCK);
#endif // WIN32

   rc = connect(*pSockfd, (sockaddr*)&sockAddr, sizeof(sockAddr));
   //DLOG("Connect %d, fd=%d\n", rc, *pSockfd);
   if(rc < 0) {
      Close(pSockfd);
      if(errno == EINTR) {
         errno = ETIMEDOUT;
      }   
   } else {
      SetNoDelay(*pSockfd, true);
   }

#ifndef _WIN32
   if(bAlarm) {
      alarm(0);
      signal(SIGALRM, sigfunc);
   }
#endif //_WIN32
   
   return (rc != -1);
}

bool CAmtSock::Close(int* pSockfd) 
{
   if((*pSockfd) != INVALID_SOCKET) {   
      struct linger lg;
      lg.l_onoff  = 1;
      lg.l_linger = 0;
      setsockopt(*pSockfd, SOL_SOCKET, SO_LINGER, (char*)&lg, sizeof(lg));
#ifdef _WIN32
      closesocket(*pSockfd);
#else
	  close(*pSockfd);
#endif
      (*pSockfd) = INVALID_SOCKET;
      return true;
   }
   return false;
}




bool CAmtSock::Bind(int sockfd, LPCSTR szAddr, int nPort) 
{
   struct sockaddr_in sockAddr;
   sockAddr.sin_family = AF_INET;
   if (szAddr == NULL || szAddr[0] == 0 || strcmp(szAddr, "*") == 0) {
      sockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
   } else {
      long addr = inet_addr(szAddr);
      if((int)addr == -1) {
  //       m_pLogger->Log(CHuLogger::Critical, "invalid ip address(%s)",  szAddr);
         return false;
      }
      sockAddr.sin_addr.s_addr = addr;
   }
   sockAddr.sin_port = htons((u_short)nPort);

   int ret =  bind(sockfd, (sockaddr*)&sockAddr, sizeof(sockAddr));
   if(ret == -1) {
   //   m_pLogger->Log(CHuLogger::Critical, "socket bind error(%d,%s)", errno, strerror(errno));
      return false;
   } else {   
      int nRet;
      nRet = SOCKET_SND_RCV_BUFFER;
      ret = setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (char*)&nRet, sizeof(nRet));
      if(ret != 0) {
   //      m_pLogger->Log(CHuLogger::Info, "setsockopt(SO_RCVBUF) error(%d,%s)", errno, strerror(errno));
      }

      nRet = SOCKET_SND_RCV_BUFFER;
      ret = setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (char*)&nRet, sizeof(nRet));
      if(ret != 0) {
   //      m_pLogger->Log(CHuLogger::Info, "setsockopt(SO_SNDBUF) error(%d,%s)", errno, strerror(errno));
      }
   }
   return true;
}

// default:16384,  min:256, max:262142 bytes
bool  CAmtSock::SetSendBufferSize(int sockfd, int nSndSize)
{

   if (sockfd == INVALID_SOCKET) {
      return false;
   }

   int nRet;
   nRet = nSndSize==0?SOCKET_SND_RCV_BUFFER:nSndSize;
   setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (char*)&nRet, sizeof(nRet));

   return true;
}
// default:16384,  min:256, max:262142 bytes
bool  CAmtSock::SetRecvBufferSize(int sockfd, int nRcvSize)
{

   if (sockfd == INVALID_SOCKET) {
      return false;
   }

   int nRet;
   nRet = nRcvSize==0?SOCKET_SND_RCV_BUFFER:nRcvSize;
   setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (char*)&nRet, sizeof(nRet));
  
   return true;
}

bool CAmtSock::Listen(int sockfd, int nBacklog) 
{
   int rc = listen(sockfd, nBacklog); 
   if(rc == -1) {
      //m_pLogger->Log(CHuLogger::Critical, "socket listen error(%d,%s)", errno, strerror(errno));
      return false;
   }
   return true;
}

int CAmtSock::Accept(int sockfd) 
{
   struct sockaddr_in sockAddr;
   //size_t addrLen = sizeof(sockAddr);
   SOCKLEN_T addrLen = sizeof(sockAddr);

   int newSock = INVALID_SOCKET;
   while(true) {
      newSock = accept(sockfd, (sockaddr*)&sockAddr, &addrLen);
      if(newSock != INVALID_SOCKET) break;
      if(errno == EINTR) {
         continue;
      } else {
        // m_pLogger->Log(CHuLogger::Critical, "socket accept failed (%d,%s)", errno, strerror(errno));
         break;
      }
   }
   return newSock;
}


int CAmtSock::Send(int sockfd, const char* pBuf, int n, int msTimeout) 
{
   int rc;
   if(msTimeout >= 0) {
      rc = ReadyToSend(sockfd, msTimeout);
      if(rc < 1) return rc;
   }
   rc = send(sockfd, pBuf, n, 0);
   return rc;
}

int  CAmtSock::SendTo(int sockfd, LPCSTR pAddr, int nPort, const char* pBuf, int n, int msTimeout)
{
   int rc;
   struct sockaddr_in sockAddr;

   if(msTimeout >= 0) {
      rc = ReadyToSend(sockfd, msTimeout);
      if(rc < 1) return rc;
   }


   memset(&sockAddr,0,sizeof(sockAddr));

   sockAddr.sin_family = AF_INET;
   sockAddr.sin_addr.s_addr = inet_addr(pAddr);
   sockAddr.sin_port = htons((unsigned short)nPort);
   SOCKLEN_T nSockAddrLen = sizeof(sockaddr);
   rc = sendto(sockfd, pBuf, n, 0, (sockaddr*)&sockAddr, nSockAddrLen);
#ifdef DEBUG_MODE
	printf("SendTo : %s/%d \n", pAddr, nPort);
#endif
   return rc;
}
#ifndef USING_POLL
int CAmtSock::ReadyToSend(int sockfd, int msTimeout)
{
   if(sockfd == INVALID_SOCKET) return -1;

   fd_set  fd_send;
   timeval sel_timeout;
   int selnum;

//   do {
      FD_ZERO(&fd_send);
      FD_SET((unsigned int)sockfd, &fd_send);
      sel_timeout.tv_sec  = msTimeout / 1000;
      sel_timeout.tv_usec = (msTimeout % 1000) * 1000;
      selnum = select(sockfd + 1, 0, &fd_send, 0, &sel_timeout);
//   } while(selnum == -1 && errno == EINTR);

   if(selnum == -1) return -1;
   if(selnum == 0) return 0;
   if(FD_ISSET(sockfd, &fd_send)) {
      return 1;
   }
   return -2;   
}

int CAmtSock::ReadyToRecv(int sockfd, int msTimeout)
{
   if(sockfd == INVALID_SOCKET) return -1;

   fd_set  fd_read;
   timeval sel_timeout;
   int     selnum;

//   do {
      FD_ZERO(&fd_read);
      FD_SET((unsigned int)sockfd, &fd_read);
      sel_timeout.tv_sec  = msTimeout / 1000;
      sel_timeout.tv_usec = (msTimeout % 1000) * 1000;
      selnum = select(sockfd + 1, &fd_read, 0, 0, &sel_timeout);
//   } while(selnum == -1 && errno == EINTR);

   if(selnum == -1) return -1;
   if(selnum == 0) return 0;
   if(FD_ISSET(sockfd, &fd_read)) {
      return 1;
   }
   return -2;
}
#else
int CAmtSock::ReadyToSend(int sockfd, int msTimeout)
//timeout:-1 unlimited wait before release write block
{
   if(sockfd == INVALID_SOCKET) return -1;

	struct pollfd fds[1];
   fds[0].fd = sockfd;
   fds[0].events = POLLOUT;
   /*
      poll() return
      >0 descript is readable
      0  timeout
      -1 error
   */
   return poll(fds,1,msTimeout);
}

int CAmtSock::ReadyToRecv(int sockfd, int msTimeout)
//timeout:-1 unlimited wait before read event occur
{
   if(sockfd == INVALID_SOCKET) return -1;
	struct pollfd fds[1];
   fds[0].fd = sockfd;
   fds[0].events = POLLIN | POLLPRI;
   /*
      poll() return
      >0 descript is readable
      0  timeout
      -1 error
   */
   return poll(fds,1,msTimeout);
}
#endif
int CAmtSock::RecvUntil(int sockfd, char* pBuf, int n, int msTimeout)
{
   int rc = 0;
   int nLeft = n;
   int nRecv = 0;
   do {
      if(msTimeout >= 0) {
         rc = ReadyToRecv(sockfd, msTimeout);
         if(rc < 1) break;
      }
      rc = recv(sockfd, pBuf+nRecv, nLeft, 0);
      //CLogger::GetInstance()->Log(CLogger::Info, "RecvUntil--> recv (fd:%d, rc:%d)", sockfd, rc);
      if(rc < 1) {
         break;
      }   
      nRecv += rc;
      nLeft -= rc;
   } while(nLeft > 0);
   return nRecv;
}


bool CAmtSock::GetPeerName(int sockfd, char* szPeerAddr, int& nPeerPort)
{
   struct sockaddr_in sockAddr;
   memset(&sockAddr, 0, sizeof(sockAddr));
   //size_t nSockAddrLen = sizeof(sockAddr);
   SOCKLEN_T nSockAddrLen = sizeof(sockAddr);
   if(getpeername(sockfd, (sockaddr*)&sockAddr, &nSockAddrLen) == 0) {
//      HexaDump((void*)&sockAddr, (int)nSockAddrLen);
//      hbLogPrint("sockaddr_in.sin_addr=%ld", sockAddr.sin_addr);
      strcpy(szPeerAddr, inet_ntoa(sockAddr.sin_addr));
      nPeerPort = ntohs(sockAddr.sin_port);
      return true;
   } else {
//      hbLogPrint("getpeername failed (%d,%s)", errno, strerror(errno));
      return false;
   }
}



int
CAmtSock::RecvFrom(int nSockFd, unsigned char * pBuff, int nBuffLen, int nFlags, char * pRemotIpAddr, int * pnRemotePort)
{
	struct sockaddr sockAddr;
	struct sockaddr_in *pInetAddr;
	int nSockAddrLen;
	int nRealRecvLen;

	if (nSockFd == INVALID_SOCKET)
	{
		return -1;
	}

	memset(&sockAddr, 0, sizeof(sockAddr));
	nSockAddrLen = sizeof(sockAddr);

	nRealRecvLen = recvfrom(nSockFd,
			(char *)pBuff,
			nBuffLen,
         nFlags,
			(sockaddr*) &sockAddr,
			(SOCKLEN_T*)&nSockAddrLen);

	if (nRealRecvLen != -1)
	{
	    pInetAddr = (struct sockaddr_in *) &sockAddr;

       if(pRemotIpAddr != NULL) {
	      sprintf(pRemotIpAddr, "%s", inet_ntoa(pInetAddr->sin_addr));
       }
       if(pnRemotePort != NULL) {
          *pnRemotePort = (int)ntohs(pInetAddr->sin_port);
       }
   } 

	return nRealRecvLen;
}
/********************************************************************************************
ipv6 sock
**********************************************************************************************/

int CAmtSock6::Socket(int nSockType) 
{
   int fdsock = INVALID_SOCKET;
   if(nSockType == UDP) {
      fdsock = socket(AF_INET6, SOCK_DGRAM, 0);
   } else if (nSockType == TCP) {
      fdsock = socket(AF_INET6, SOCK_STREAM, 0);
   }
   if (fdsock == INVALID_SOCKET) {
      //m_pLogger->Log(CHuLogger::Critical, "socket creation error:%d:%s", errno, strerror(errno));
   } else {
      int nSockOpt = 1;
      if (setsockopt(fdsock, SOL_SOCKET, SO_REUSEADDR, (char*)&nSockOpt, sizeof(nSockOpt)) != 0) {
      //   m_pLogger->Log(CHuLogger::Critical, "Listener setsockopt(SO_REUSEADDR) ERROR(%d,%s)", errno, strerror(errno));
      }
   }
//set non-block mode
#ifdef WIN32
   PDKULONG iMode = 1;
   ioctlsocket(fdsock, FIONBIO, &iMode);
#else // !WIN32
   int     nFlags;
   nFlags = fcntl(fdsock, F_GETFL, 0);
   fcntl(fdsock, F_SETFL, nFlags | O_NONBLOCK);
#endif // WIN32   
   return fdsock;
}

bool CAmtSock6::SetNoDelay(int sockfd, bool bTrue) 
{
   int nValue = bTrue?1:0;
   if(setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char*)&nValue, sizeof(nValue)) != 0) {
    //  m_pLogger->Log(CHuLogger::Info, "TCP_NODELAY ERROR(%d,%s)\n", errno, strerror(errno));
      return false;
   }
   return true;
}

bool CAmtSock6::TryAssignConn(int* pSockfd, LPCSTR szAddr, int nPort, int  nSec)
{
   *pSockfd = Socket(TCP);
   if(*pSockfd == INVALID_SOCKET) return false;
   
   struct sockaddr_in6 sockAddr;
   memset(&sockAddr,0,sizeof(sockAddr));

   sockAddr.sin6_family = AF_INET6;
	inet_pton(AF_INET6,szAddr,&sockAddr.sin6_addr);
   sockAddr.sin6_port = htons((unsigned short)nPort);

   int rc = -1;

#ifndef _WIN32
   bool bAlarm = (nSec > 0);
   SIG_FUNC sigfunc;

   if(bAlarm) {
      if(alarm(nSec) == 0) {
         sigfunc = signal(SIGALRM, hb_connect_alarm);
      } else {
         bAlarm = false;
         signal(SIGALRM, sigfunc);
         //DLOG("Alarm was already set in connecting to server");
      }
   }
#endif //_WIN32

//set non-block mode
#ifdef WIN32
		PDKULONG iMode = 1;
		ioctlsocket(*pSockfd, FIONBIO, &iMode);
#else // !WIN32
		int	  nFlags;
		nFlags = fcntl(*pSockfd, F_GETFL, 0);
		fcntl(*pSockfd, F_SETFL, nFlags | O_NONBLOCK);
#endif // WIN32

   rc = connect(*pSockfd, (sockaddr*)&sockAddr, sizeof(sockAddr));
   //DLOG("Connect %d, fd=%d\n", rc, *pSockfd);
   if(rc < 0) {
      Close(pSockfd);
      if(errno == EINTR) {
         errno = ETIMEDOUT;
      }   
   } else {
      SetNoDelay(*pSockfd, true);
   }

#ifndef _WIN32
   if(bAlarm) {
      alarm(0);
      signal(SIGALRM, sigfunc);
   }
#endif //_WIN32
   
   return (rc != -1);
}

bool CAmtSock6::Close(int* pSockfd) 
{
   if((*pSockfd) != INVALID_SOCKET) {   
      struct linger lg;
      lg.l_onoff  = 1;
      lg.l_linger = 0;
      setsockopt(*pSockfd, SOL_SOCKET, SO_LINGER, (char*)&lg, sizeof(lg));
#ifdef _WIN32
      closesocket(*pSockfd);
#else
	  close(*pSockfd);
#endif
      (*pSockfd) = INVALID_SOCKET;
      return true;
   }
   return false;
}

bool CAmtSock6::Bind(int sockfd, LPCSTR szDevName, LPCSTR szAddr,int nPort) 
{
   struct sockaddr_in6 sockAddr;
	memset(&sockAddr,0x00,sizeof(struct sockaddr_in6));
   sockAddr.sin6_family = AF_INET6;
	sockAddr.sin6_flowinfo = 0;
#ifndef WIN32
	sockAddr.sin6_scope_id = if_nametoindex(szDevName);
#endif
   if (szAddr == NULL || szAddr[0] == 0 || strcmp(szAddr, "*") == 0) {
#ifndef WIN32
	   sockAddr.sin6_addr = in6addr_any;
#endif

   } else {
		inet_pton(AF_INET6,szAddr,&sockAddr.sin6_addr);
   }
   sockAddr.sin6_port = htons((u_short)nPort);

   int ret =  bind(sockfd, (sockaddr*)&sockAddr, sizeof(sockAddr));
   if(ret == -1) {
   //   m_pLogger->Log(CHuLogger::Critical, "socket bind error(%d,%s)", errno, strerror(errno));
      return false;
   } else {   
      int nRet;
      nRet = SOCKET_SND_RCV_BUFFER;
      ret = setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (char*)&nRet, sizeof(nRet));
      if(ret != 0) {
   //      m_pLogger->Log(CHuLogger::Info, "setsockopt(SO_RCVBUF) error(%d,%s)", errno, strerror(errno));
      }

      nRet = SOCKET_SND_RCV_BUFFER;
      ret = setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (char*)&nRet, sizeof(nRet));
      if(ret != 0) {
   //      m_pLogger->Log(CHuLogger::Info, "setsockopt(SO_SNDBUF) error(%d,%s)", errno, strerror(errno));
      }
   }
   return true;
}

// default:16384,  min:256, max:262142 bytes
bool  CAmtSock6::SetSendBufferSize(int sockfd, int nSndSize)
{

   if (sockfd == INVALID_SOCKET) {
      return false;
   }

   int nRet;
   nRet = nSndSize==0?SOCKET_SND_RCV_BUFFER:nSndSize;
   setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (char*)&nRet, sizeof(nRet));

   return true;
}
// default:16384,  min:256, max:262142 bytes
bool  CAmtSock6::SetRecvBufferSize(int sockfd, int nRcvSize)
{

   if (sockfd == INVALID_SOCKET) {
      return false;
   }

   int nRet;
   nRet = nRcvSize==0?SOCKET_SND_RCV_BUFFER:nRcvSize;
   setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (char*)&nRet, sizeof(nRet));
  
   return true;
}

bool CAmtSock6::Listen(int sockfd, int nBacklog) 
{
   int rc = listen(sockfd, nBacklog); 
   if(rc == -1) {
      //m_pLogger->Log(CHuLogger::Critical, "socket listen error(%d,%s)", errno, strerror(errno));
      return false;
   }
   return true;
}

int CAmtSock6::Accept(int sockfd) 
{
   struct sockaddr_in6 sockAddr;
   //size_t addrLen = sizeof(sockAddr);
   SOCKLEN_T addrLen = sizeof(sockAddr);

   int newSock = INVALID_SOCKET;
   while(true) {
      newSock = accept(sockfd, (sockaddr*)&sockAddr, &addrLen);
      if(newSock != INVALID_SOCKET) break;
      if(errno == EINTR) {
         continue;
      } else {
        // m_pLogger->Log(CHuLogger::Critical, "socket accept failed (%d,%s)", errno, strerror(errno));
         break;
      }
   }
   return newSock;
}


int CAmtSock6::Send(int sockfd, const char* pBuf, int n, int msTimeout) 
{
   int rc;
   if(msTimeout >= 0) {
      rc = ReadyToSend(sockfd, msTimeout);
      if(rc < 1) return rc;
   }
   rc = send(sockfd, pBuf, n, 0);
   return rc;
}

int  CAmtSock6::SendTo(int sockfd, LPCSTR pAddr, int nPort, const char* pBuf, int n, int msTimeout)
{
   int rc;
   struct sockaddr_in6 sockAddr;

   if(msTimeout >= 0) {
      rc = ReadyToSend(sockfd, msTimeout);
      if(rc < 1) return rc;
   }


   memset(&sockAddr,0,sizeof(sockAddr));

   sockAddr.sin6_family = AF_INET6;
	inet_pton(AF_INET6,pAddr,&sockAddr.sin6_addr);
   sockAddr.sin6_port = htons((unsigned short)nPort);
   SOCKLEN_T nSockAddrLen = sizeof(sockAddr);
   rc = sendto(sockfd, pBuf, n, 0, (sockaddr*)&sockAddr, nSockAddrLen);
   return rc;
}
#ifndef USING_POLL
int CAmtSock6::ReadyToSend(int sockfd, int msTimeout)
{
   if(sockfd == INVALID_SOCKET) return -1;

   fd_set  fd_send;
   timeval sel_timeout;
   int selnum;

//   do {
      FD_ZERO(&fd_send);
      FD_SET((unsigned int)sockfd, &fd_send);
      sel_timeout.tv_sec  = msTimeout / 1000;
      sel_timeout.tv_usec = (msTimeout % 1000) * 1000;
      selnum = select(sockfd + 1, 0, &fd_send, 0, &sel_timeout);
//   } while(selnum == -1 && errno == EINTR);

   if(selnum == -1) return -1;
   if(selnum == 0) return 0;
   if(FD_ISSET(sockfd, &fd_send)) {
      return 1;
   }
   return -2;   
}

int CAmtSock6::ReadyToRecv(int sockfd, int msTimeout)
{
   if(sockfd == INVALID_SOCKET) return -1;

   fd_set  fd_read;
   timeval sel_timeout;
   int     selnum;

//   do {
      FD_ZERO(&fd_read);
      FD_SET((unsigned int)sockfd, &fd_read);
      sel_timeout.tv_sec  = msTimeout / 1000;
      sel_timeout.tv_usec = (msTimeout % 1000) * 1000;
      selnum = select(sockfd + 1, &fd_read, 0, 0, &sel_timeout);
//   } while(selnum == -1 && errno == EINTR);

   if(selnum == -1) return -1;
   if(selnum == 0) return 0;
   if(FD_ISSET(sockfd, &fd_read)) {
      return 1;
   }
   return -2;
}
#else
int CAmtSock6::ReadyToSend(int sockfd, int msTimeout)
//timeout:-1 unlimited wait before release write block
{
   if(sockfd == INVALID_SOCKET) return -1;

	struct pollfd fds[1];
   fds[0].fd = sockfd;
   fds[0].events = POLLOUT;
   /*
      poll() return
      >0 descript is readable
      0  timeout
      -1 error
   */
   return poll(fds,1,msTimeout);
}

int CAmtSock6::ReadyToRecv(int sockfd, int msTimeout)
//timeout:-1 unlimited wait before read event occur
{
   if(sockfd == INVALID_SOCKET) return -1;
	struct pollfd fds[1];
   fds[0].fd = sockfd;
   fds[0].events = POLLIN | POLLPRI;
   /*
      poll() return
      >0 descript is readable
      0  timeout
      -1 error
   */
   return poll(fds,1,msTimeout);
}
#endif
int CAmtSock6::RecvUntil(int sockfd, char* pBuf, int n, int msTimeout)
{
   int rc = 0;
   int nLeft = n;
   int nRecv = 0;
   do {
      if(msTimeout >= 0) {
         rc = ReadyToRecv(sockfd, msTimeout);
         if(rc < 1) break;
      }
      rc = recv(sockfd, pBuf+nRecv, nLeft, 0);
      //CLogger::GetInstance()->Log(CLogger::Info, "RecvUntil--> recv (fd:%d, rc:%d)", sockfd, rc);
      if(rc < 1) {
         break;
      }   
      nRecv += rc;
      nLeft -= rc;
   } while(nLeft > 0);
   return nRecv;
}


bool CAmtSock6::GetPeerName(int sockfd, char* szPeerAddr, int& nPeerPort)
{
   struct sockaddr_in6 sockAddr;
   memset(&sockAddr, 0, sizeof(sockAddr));
   SOCKLEN_T nSockAddrLen = sizeof(sockAddr);
   if(getpeername(sockfd, (sockaddr*)&sockAddr, &nSockAddrLen) == 0) {
		inet_ntop(AF_INET6,&sockAddr.sin6_addr,szPeerAddr,sizeof(szPeerAddr));
      nPeerPort = ntohs(sockAddr.sin6_port);
      return true;
   } else {
      return false;
   }
}

int
CAmtSock6::RecvFrom(int nSockFd, unsigned char * pBuff, int nBuffLen, int nFlags, char * pRemotIpAddr, int * pnRemotePort)
{
	struct sockaddr sockAddr;
	struct sockaddr_in6 *pInetAddr;
	int nSockAddrLen;
	int nRealRecvLen;
	char szTmpAddr[128];szTmpAddr[0]='\0';
	if (nSockFd == INVALID_SOCKET)
	{
		return -1;
	}

	memset(&sockAddr, 0, sizeof(sockAddr));
	nSockAddrLen = sizeof(sockAddr);

	nRealRecvLen = recvfrom(nSockFd,
			(char *)pBuff,
			nBuffLen,
         nFlags,
			(sockaddr*) &sockAddr,
			(SOCKLEN_T*)&nSockAddrLen);

	if (nRealRecvLen != -1)
	{
	    pInetAddr = (struct sockaddr_in6 *) &sockAddr;

       if(pRemotIpAddr != NULL) {
			inet_ntop(AF_INET6,&pInetAddr->sin6_addr,szTmpAddr,sizeof(szTmpAddr));
	      sprintf(pRemotIpAddr, "%s",szTmpAddr);
       }
       if(pnRemotePort != NULL) {
          *pnRemotePort = (int)ntohs(pInetAddr->sin6_port);
       }
   } 

	return nRealRecvLen;
}
}; //namespace AMT
