#define _POSIX_SOURCE 1

#ifndef  __AMT_SOCK_H__
#define  __AMT_SOCK_H__

#include <errno.h>

#ifndef WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/un.h>


#endif // WIN32



#ifndef WIN32
#define INVALID_SOCKET           (-1)


#endif //WIN32

namespace AMT
{


typedef const char *LPCSTR;

class CAmtSock
{
public:
   enum {
      UDP = 1,
      TCP = 2,
   };
   
   static bool m_bInitialized;

   static bool InitSock();
   static int  Socket(int nSockType); 
   static bool SetNoDelay(int sockfd, bool bTrue);
   static bool TryAssignConn(int* pSockfd, LPCSTR szAddr, int nPort, int  nSec=-1); // client only
   static bool Close(int* pSockfd);
   static bool Bind(int sockfd, LPCSTR szAddr, int nPort);
   static bool SetSendBufferSize(int sockfd, int nSndSize); // default:16384,  min:256, max:262142 bytes
   static bool SetRecvBufferSize(int sockfd, int nRcvSize); // default:16384,  min:256, max:262142 bytes
   static bool Listen(int sockfd, int nBacklog);
   static int  Accept(int sockfd);
   static int  Send(int sockfd, const char* pBuf, int n, int msTimeout=-1);
   static int  SendTo(int sockfd, LPCSTR pAddr, int nPort, const char* pBuf, int n, int msTimeout=-1);
   static int  ReadyToSend(int sockfd, int msTimeout);
   static int  ReadyToRecv(int sockfd, int msTimeout);
   static int  RecvUntil(int sockfd, char* pBuf, int n, int msTimeout=-1);
   static bool GetPeerName(int sockfd, char* szPeerAddr, int& nPeerPort);
   static int  RecvFrom(int nSockFd, unsigned char * pBuff, int nBuffLen, int nFlags, 
                        char * pRemotIpAddr, int * pnRemotePort);

};

class CAmtSock6
{
public:
   enum {
      UDP = 1,
      TCP = 2,
   };
   
   static bool m_bInitialized;

   static bool InitSock();
   static int  Socket(int nSockType); 
   static bool SetNoDelay(int sockfd, bool bTrue);
   static bool TryAssignConn(int* pSockfd, LPCSTR szAddr, int nPort, int  nSec=-1); // client only
   static bool Close(int* pSockfd);
   static bool Bind(int sockfd, LPCSTR szDevname, LPCSTR szAddr, int nPort);
   static bool SetSendBufferSize(int sockfd, int nSndSize); // default:16384,  min:256, max:262142 bytes
   static bool SetRecvBufferSize(int sockfd, int nRcvSize); // default:16384,  min:256, max:262142 bytes
   static bool Listen(int sockfd, int nBacklog);
   static int  Accept(int sockfd);
   static int  Send(int sockfd, const char* pBuf, int n, int msTimeout=-1);
   static int  SendTo(int sockfd, LPCSTR pAddr, int nPort, const char* pBuf, int n, int msTimeout=-1);
   static int  ReadyToSend(int sockfd, int msTimeout);
   static int  ReadyToRecv(int sockfd, int msTimeout);
   static int  RecvUntil(int sockfd, char* pBuf, int n, int msTimeout=-1);
   static bool GetPeerName(int sockfd, char* szPeerAddr, int& nPeerPort);
   static int  RecvFrom(int nSockFd, unsigned char * pBuff, int nBuffLen, int nFlags, 
                        char * pRemotIpAddr, int * pnRemotePort);
};

class CAmtMultiUdpSock
{
   
};

}; //namespace AMT


#endif
