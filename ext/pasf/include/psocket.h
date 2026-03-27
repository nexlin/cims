#ifndef __PSOCKET_H__
#define __PSOCKET_H__

#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/un.h>
#include <iostream>
#include <string>

#include <memory>
#include <memory>


//////////////////////////////////////////////////////////////////////////////

typedef struct sockaddr_in       SOCKADDR_IN; 
typedef struct sockaddr          SOCKADDR;
typedef struct hostent *         LPHOSTENT;
typedef struct in_addr *         LPIN_ADDR;
typedef int                      SOCKET;

#define INVALID_SOCKET           (-1)
#define SOCKET_ERROR             (-1)

struct ISocket
{
  virtual int close() = 0;
};

//////////////////////////////////////////////////////////////////////////////
// PNbSocket
class PNbSocket : public ISocket
{
public:
    PNbSocket();
    virtual ~PNbSocket();

    virtual int close();
    virtual int recv(void* lpBuf, int nBufLen, int nFlags = 0);
    virtual int send(const void* lpBuf, int nBufLen, int nFlags = 0);

    virtual int recvFrom(void* lpBuf, int nBufLen,
                        char *szSocketAddress, unsigned int& rSocketPort, 
                        int nFlags = 0);
    virtual int sendTo(const void* lpBuf, int nBufLen,
                      unsigned int nHostPort, char *szHostAddress=NULL, 
                      int nFlags = 0);

    int connect(const std::string& strAddr, const unsigned int nPort);
    //int connect(const string& strLAddr, const unsigned int nLPort, const string& strRAddr, const unsigned int nRPort); // add by prodhkang local port bind

    bool  getSockName(std::string& strSocketAddress, unsigned int& rSocketPort);
    bool  getPeerName(std::string& strPeerAddress, unsigned int& rPeerPort);

    const int&  getAddrFormat()	  { return _nAddrFormat; }
    const int&  getSocket()       { return _hSocket; }   

    const std::string& getConnectAddr(){ return _strAddr; }
    const bool  isConnect()       { return _bConnect; }

    bool bind(const std::string& strAddr, const unsigned int nPort);
    static bool setSockaddr(SOCKADDR_IN& sockAddr, const std::string& strAddr,
                 const unsigned int nPort, const int nAddrFormat = AF_INET); 

    void setNonBlock();
    bool assignSocket(const SOCKET hSocket, const int nAddrForamt);

    operator SOCKET() const { return _hSocket; }

protected:
    bool createSocket(const int nAddrFormat, const int nSocketType, const int nProtocolType = 0);

protected:
    SOCKET   _hSocket;
    std::string   _strAddr;
    unsigned int _nPort;
    bool     _bConnect;
    int	     _nAddrFormat;
};

//////////////////////////////////////////////////////////////////////////////
class PNbSrvSocket : public PNbSocket
{
  public:
    PNbSrvSocket();
    virtual ~PNbSrvSocket();

    virtual int close();
    bool createServer(const std::string& strAddr, const unsigned int nPort, const int nConnectionBacklog = 1);
   bool accept(PNbSocket * rspSocket, const bool bNonBlock = true);

  protected:
    bool open(const std::string& strAddr, const unsigned int nPort);
    bool listen(const int nConnectionBacklog);

};

class PUdpSocket : public PNbSocket
{
public:
   PUdpSocket();
   virtual ~PUdpSocket();

   bool open(const std::string& rStrAddr, const unsigned int nPort);
};

#endif //__PSOCKET_H__
