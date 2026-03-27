//////////////////////////////////////////////////////////////////////////////
// psocket.cpp

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/un.h>

#include "pbase.h"

#include "psocket.h"

#define SOCKET_SND_RCV_BUFFER   16384*512

PUdpSocket::PUdpSocket()
{
}

PUdpSocket::~PUdpSocket()
{
}

//////////////////////////////////////////////////////////////////////////////
bool PUdpSocket::open(const std::string& rStrAddr, const unsigned int nPort)
{

   if (createSocket(AF_INET, SOCK_DGRAM, 0))
   {

      if (bind(rStrAddr, nPort))
         return true;

      PDPRINT("port:%d, error:%d:%s", nPort, errno, strerror(errno));

      close();

   }
   return false;
}


PNbSocket::PNbSocket()
: _hSocket(INVALID_SOCKET), _strAddr(""), _nPort(0), _bConnect(false) 
//: _hSocket(INVALID_SOCKET), _strAddr(""), _nPort(0), _bConnect(false), _bBind(false) 
{
   PDPRINT("PNbSocket::PNbSocket() : this=0x%x\n", this);
}

PNbSocket::~PNbSocket()
{
   PDPRINT("PNbSocket::~PNbSocket() : this=0x%x, sock=%d\n", this, _hSocket);
   close();   
}

void PNbSocket::setNonBlock()
{
   int nFlags = fcntl(_hSocket, F_GETFL, 0);
   fcntl(_hSocket, F_SETFL, nFlags | O_NONBLOCK);
}

bool PNbSocket::setSockaddr(SOCKADDR_IN& sockAddr, const std::string& strAddr, const unsigned int nPort, const int nAddrFormat)
{
   memset(&sockAddr,0,sizeof(sockAddr));

   sockAddr.sin_family = nAddrFormat;
   sockAddr.sin_addr.s_addr = inet_addr(strAddr.c_str());

   if (sockAddr.sin_addr.s_addr == INADDR_NONE) {
      LPHOSTENT lphost;
      lphost = gethostbyname(strAddr.c_str());

      if (lphost != NULL) {
         sockAddr.sin_addr.s_addr = ((LPIN_ADDR)lphost->h_addr)->s_addr;
      }
      else {
         PEPRINT("Error in gethostbyname! ip=%s, err=%s(%d)\n", strAddr.c_str(), strerror(errno), errno);
         return false;
      }
   }
   sockAddr.sin_port = htons((unsigned short)nPort); 

   return true;
} 

int PNbSocket::sendTo(const void* lpBuf, int nBufLen, unsigned int nHostPort,
                  char *szHostAddress, int nFlags)
{
   //USES_CONVERSION;
   int      nAddrLen;
   int      nRet = 0;

   switch (_nAddrFormat) {
    case AF_INET:
         SOCKADDR sockAddr;
         struct sockaddr_in   *pInetAddr;
         memset(&sockAddr,0,sizeof(sockAddr));

         if (_hSocket == INVALID_SOCKET)
             return -1;

         pInetAddr = (struct sockaddr_in *) &sockAddr;

         pInetAddr->sin_family = AF_INET;

         if (szHostAddress == NULL) {
            pInetAddr->sin_addr.s_addr = htonl(INADDR_BROADCAST);
         }
         else {
            pInetAddr->sin_addr.s_addr = inet_addr(szHostAddress);
#if 0 // comparison is always false due to limited range of data type
            if (pInetAddr->sin_addr.s_addr == (unsigned long)-1)
            {
               LPHOSTENT lphost;
               lphost = gethostbyname(lpszHostAddress);
               if (lphost != NULL) {
                  pInetAddr->sin_addr.s_addr = 
                                 ((LPIN_ADDR)lphost->h_addr)->s_addr;
               } else {
                  m_pLogger->Log(CLogger::Info, 
                     "Invalid parameters ! - gethostbyname : %d,%s,addr:%s", 
                     errno, strerror(errno), lpszHostAddress);
                  return SOCKET_ERROR;
               }
            }
#endif
         }
         pInetAddr->sin_port = htons((u_short)nHostPort);
         nRet = sendto(_hSocket, lpBuf, nBufLen, nFlags,
                (SOCKADDR*)&sockAddr, sizeof(sockAddr));
         if (nRet == -1) {
            if ( errno != EAGAIN && errno != EWOULDBLOCK) {
               PDPRINT("[Socket] sendto Error = %d(%s)\n", errno, strerror(errno));
               return -1;
            }
         }
   } //switch  
   return nRet; 
}

int PNbSocket::recvFrom(void* lpBuf, int nBufLen,
                      char* rSocketAddress, unsigned int& rSocketPort, int nFlags)
{
   SOCKADDR sockAddr;
   struct sockaddr_in   *pInetAddr;
   struct sockaddr_un   *pUnixAddr;

   memset(&sockAddr, 0, sizeof(sockAddr));
   int nSockAddrLen = sizeof(sockAddr);
   int nRealRecvLen = recvfrom(_hSocket, lpBuf, nBufLen, nFlags,
       (SOCKADDR*)&sockAddr, (socklen_t*)&nSockAddrLen);
   if (nRealRecvLen != SOCKET_ERROR)
   {
      switch (_nAddrFormat) {
         case AF_INET:
         default:
            pInetAddr = (struct sockaddr_in *) &sockAddr;

            rSocketPort = ntohs(pInetAddr->sin_port);
#ifdef TEMP //_REENTRANT
            inet_ntoa_r(pInetAddr->sin_addr, rSocketAddress, 16);
#else
            sprintf(rSocketAddress, "%s", inet_ntoa(pInetAddr->sin_addr));
#endif
            break;
      }
   } else {
      if ( errno != EAGAIN && errno != EWOULDBLOCK) {
         PDPRINT("[Socket] recvfrom Error = %d(%s)\n", errno, strerror(errno));
         return -1;
      }
   }

  return nRealRecvLen;
}

int PNbSocket::connect(const std::string& strAddr, const unsigned int nPort)
{
	int      nRet;
	_nPort   = nPort;
	_strAddr = strAddr;
   
	if (_hSocket != INVALID_SOCKET || createSocket(AF_INET, SOCK_STREAM, 0) != false) 
	{
		setNonBlock();
		SOCKADDR_IN sockAddr;
		if (!setSockaddr(sockAddr, strAddr, nPort)) 
		{
 			return -1;
		} 
  
		if ((nRet = ::connect(_hSocket, (SOCKADDR*)&sockAddr, sizeof(SOCKADDR_IN)) == SOCKET_ERROR)) 
		{
      	if (errno != EINPROGRESS) 
			{
        		PEPRINT("[===> Connect Fail...Addr:%s:%d, (Err:%s)\n", strAddr.c_str(), nPort, strerror(errno));
        		return -2;
      	}
      	PEPRINT("EINPROGRESS\n");
    	}
    	else 
		{
      	PDPRINT("Conn Success\n");
      	_bConnect = true;
      	return 0;
    	}
	}
	return -3;
}


int PNbSocket::close()
{
  if (_hSocket != INVALID_SOCKET) {
    struct linger lg;
    lg.l_onoff  = 1;
    lg.l_linger = 0;
    setsockopt(_hSocket, SOL_SOCKET, SO_LINGER, (char*)&lg, sizeof(lg));

    if (::close(_hSocket) == SOCKET_ERROR) {
      PEPRINT("closesocket error : %s\n", 
          strerror(errno));
    }
    PEPRINT("closesocket \n");
  }

  _bConnect = false;
  _hSocket = INVALID_SOCKET;

  /* Client Local Bind Mode Only
  if(_bBind == true)
  {
     _bBind = false;
  }*/

  return 0;
}

bool PNbSocket::assignSocket(const int hSocket, const int nAddrFormat)
{
  _bConnect = true;
  _hSocket = hSocket;
  _nAddrFormat = nAddrFormat;
  PDPRINT("assignSocket fd:%d\n", _hSocket);
  return true;
}

bool PNbSocket::createSocket(const int nAddrFormat, const int nSocketType, const int nProtocolType)
{
  if (_hSocket != INVALID_SOCKET) {
    PEPRINT("CreateSocket Failed...Already Created..\n");
    return false;
  }

  _nAddrFormat = nAddrFormat;

  _hSocket = socket(nAddrFormat, nSocketType, nProtocolType);
  if (_hSocket == INVALID_SOCKET) {
    PEPRINT("socket creation error:%d:%s\n", 
        errno, strerror(errno));
    return false;
  }

  //Set SO_REUSEADDR before bind
  if (nAddrFormat == AF_INET) {
    int nSockOpt = 1;
    if (setsockopt(_hSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&nSockOpt,
          sizeof(nSockOpt)) != 0) {
      PEPRINT("setsockopt(SO_REUSEADDR) failed...\n");
      ::close(_hSocket);
      _hSocket = INVALID_SOCKET;
      return false;
    }
  }
  int nSockBuffLen = SOCKET_SND_RCV_BUFFER;
  setsockopt(_hSocket, SOL_SOCKET, SO_RCVBUF, (char *)&nSockBuffLen, sizeof(nSockBuffLen));
  setsockopt(_hSocket, SOL_SOCKET, SO_SNDBUF, (char *)&nSockBuffLen, sizeof(nSockBuffLen));

  PEPRINT("createSocket Success %d...\n", _hSocket);
  return true;
}

bool PNbSocket::bind(const std::string& strAddr, const unsigned int nPort)
{
  SOCKADDR_IN sockAddr;
  memset(&sockAddr,0,sizeof(sockAddr));
  sockAddr.sin_family = _nAddrFormat;

  if (strAddr == "")
    sockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  else {
    long lResult = inet_addr(strAddr.c_str());
    if (lResult == -1) {
      PEPRINT("Invalid parameters! ip=%s\n", strAddr.c_str());
      return false;
    }
    sockAddr.sin_addr.s_addr = lResult;
  }

  sockAddr.sin_port = htons((u_short)nPort);
  int ret =  ::bind(_hSocket, (SOCKADDR*)&sockAddr, sizeof(sockAddr));

  PEPRINT("Socket Bind : res=%d, ip=%s, port=%d\n", ret, inet_ntoa(sockAddr.sin_addr), ntohs(sockAddr.sin_port));

  if (SOCKET_ERROR != ret) 
  {
     int nSockBuffLen = SOCKET_SND_RCV_BUFFER;
     setsockopt(_hSocket, SOL_SOCKET, SO_RCVBUF, (char *)&nSockBuffLen, sizeof(nSockBuffLen));
     setsockopt(_hSocket, SOL_SOCKET, SO_SNDBUF, (char *)&nSockBuffLen, sizeof(nSockBuffLen));

#if 0
    //get max size : for test..............................
    int nRetLen;
    nRetLen = sizeof(unsigned int);
    if (getsockopt(_hSocket, 
          SOL_SOCKET,
          SO_SNDBUF, //SO_MAX_MSG_SIZE,
          (char *)&nRet,	
          (socklen_t*)&nRetLen) == 0) {
          cerr << "PNbSocket-Max Send Buffer Size: " << nRet << endl;
    if (getsockopt(_hSocket, 
          SOL_SOCKET,
          SO_RCVBUF, //SO_MAX_MSG_SIZE,
          (char *)&nRet,	
          (socklen_t*)&nRetLen) == 0) {
          cerr << "PNbSocket-Max Recv Buffer Size: " << nRet << endl;
    } 
#endif
     //PEPRINT("Bind %s:%d  Success!!\n", inet_ntoa(sockAddr.sin_addr), ntohs(sockAddr.sin_port));
     //_bBind = true; // Local Port Bind Client Mode Only
     return true;
  } 
  else 
  {
     PEPRINT("Bind %s:%d  Error: %s(%d)\n", inet_ntoa(sockAddr.sin_addr), ntohs(sockAddr.sin_port), strerror(errno), errno);
     return false;
  }
}

int PNbSocket::recv(void* lpBuf, int nBufLen, int nFlags)
{
  int nRet = ::recv(_hSocket, (void*)lpBuf, nBufLen, nFlags);
  if (nRet == -1) {
      if (errno == ECONNRESET || errno == EPIPE) {
          PEPRINT("Broken Connectection in recv()!: error=%d(%s)\n",  errno, strerror(errno));
          close();
      }
      else if (errno == EWOULDBLOCK || errno == EAGAIN) {
#if 0
          PEPRINT("recv EWOULDBLOCK in recv(): error=%d\n",  errno);
#endif
          return -2;
      }
      else {
          PEPRINT("Error in recv(): error=%d(%s)\n",  errno, strerror(errno));
      }
  }
  else if (nRet == 0) {
     PEPRINT("Close Socket in recv(): error=%d(%s)\n",  errno, strerror(errno));
     close();
  }
  return nRet;
}

int PNbSocket::send(const void* lpBuf, int nBufLen, int nFlags)
{
  int nRet = ::send(_hSocket, (void*)lpBuf, nBufLen, nFlags);
  if (nRet == -1) {
      if (errno == ECONNRESET || errno == EPIPE) {
          PEPRINT("Broken Connectection in send()!: error=%d(%s)\n",  errno, strerror(errno));
          close();
          return -2;
      }
      else if (errno == EWOULDBLOCK || errno == EAGAIN) {
          PEPRINT("send EWOULDBLOCK : error=%d(%s)\n",  errno, strerror(errno));
          return -3;
      }
      else {
          PEPRINT("Error in send(): error=%d(%s)\n",  errno, strerror(errno));
      }
  }
  return nRet;
}

bool PNbSocket::getSockName(std::string& strSocketAddress, unsigned int& rSocketPort)
{
  SOCKADDR sockAddr;
  struct sockaddr_in	*pInetAddr;
  memset(&sockAddr, 0, sizeof(sockAddr));

  int nSockAddrLen = sizeof(sockAddr);
  if (getsockname(_hSocket, (SOCKADDR*)&sockAddr, (socklen_t*)&nSockAddrLen) 
      != SOCKET_ERROR)
  {
    pInetAddr = (struct sockaddr_in *) &sockAddr;

    switch (_nAddrFormat) {
      case AF_INET:
      default:
        rSocketPort = ntohs(pInetAddr->sin_port);
        strSocketAddress = inet_ntoa((*pInetAddr).sin_addr);
        return true;
    }
  }
  PEPRINT("Error in getSockName! error=%d(%s)\n",  errno, strerror(errno));
  return false;
}

bool PNbSocket::getPeerName(std::string& strPeerAddress, unsigned int& rPeerPort)
{
  SOCKADDR sockAddr;
  struct sockaddr_in	*pInetAddr;
  memset(&sockAddr, 0, sizeof(sockAddr));

  int nSockAddrLen = sizeof(sockAddr);
  if (getpeername(_hSocket, (SOCKADDR*)&sockAddr, 
        (socklen_t*)&nSockAddrLen) != SOCKET_ERROR)
  {
    pInetAddr = (struct sockaddr_in *) &sockAddr;

    switch (_nAddrFormat) {
      case AF_INET:
      default:
        rPeerPort = ntohs(pInetAddr->sin_port);
        strPeerAddress = inet_ntoa((*pInetAddr).sin_addr);
        return true;
    }
  }
  PEPRINT("Error in getPeerName! errno=%d\n", errno);
  return false;
}

//////////////////////////////////////////////////////////////////////////////////////

PNbSrvSocket::PNbSrvSocket() 
{
}

PNbSrvSocket::~PNbSrvSocket()
{
}

bool PNbSrvSocket::open(const std::string& strAddr, const unsigned int nPort)
{
  if (createSocket(AF_INET, SOCK_STREAM))
  {
    if (bind(strAddr, nPort))
      return true;
    PEPRINT("bind error(Port:%d,Err:%d-%s)\n", nPort, errno, strerror(errno));
    close();
  }
  return false;
}

int PNbSrvSocket::close()
{
  return PNbSocket::close();
}

bool PNbSrvSocket::createServer(const std::string& strAddr, const unsigned int nPort, const int nConnectionBacklog) //1
{
  if (open(strAddr, nPort) != false) {
    PDPRINT("Binding... : ip=%s, port=%d, backlog=%d\n", strAddr.c_str(), nPort, nConnectionBacklog);
    setNonBlock();

    if (listen(nConnectionBacklog) != false) {
      PDPRINT("Listen... \n");
      return true;
    }
  }
  return false;
}

bool PNbSrvSocket::listen(const int nConnectionBacklog)
{
  return (SOCKET_ERROR != ::listen(_hSocket, nConnectionBacklog)); 
}

bool PNbSrvSocket::accept(PNbSocket * rspSocket, const bool bNonBlock) 
{
  SOCKADDR_IN    tSockAddr;
  int            nSockAddrLen = sizeof(SOCKADDR_IN);
  SOCKET      nSocket;

  nSocket = ::accept(_hSocket, (sockaddr*)&tSockAddr, (socklen_t*)&nSockAddrLen);
  if (nSocket == INVALID_SOCKET) {
    if (EAGAIN == errno || EWOULDBLOCK == errno) {
      //PEPRINT("Accept EAGAIN  Errno:%d, %s\n", errno, strerror(errno));
      return false;
    }
    else {
      PEPRINT("Accept Errno:%d, %s\n", errno, strerror(errno));
      return false;
    }
  }

  if (!rspSocket) { 
      PEPRINT("rspSock NULL ERROR\n");
      return false; //rspSocket.reset(new(PNbSocket));
   }

   rspSocket->assignSocket(nSocket, AF_INET);
   rspSocket->setNonBlock();

   return true;
}

/////////////////////////////////////////////////////////////////////////////
// EOF
/////////////////////////////////////////////////////////////////////////////
