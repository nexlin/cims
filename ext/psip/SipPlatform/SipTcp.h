#ifndef _SIP_TCP_H_
#define _SIP_TCP_H_

#include "SipUdp.h"

bool GetIpByName( const char * szHostName, char * szIp, int iLen );
Socket TcpConnect( const char * pszIp, int iPort, int iTimeout = 0 );
int TcpSend( Socket fd, const char * szBuf, int iBufLen );
int TcpRecv( Socket fd, char * szBuf, int iBufLen, int iSecond );
int TcpRecvSize( Socket fd, char * szBuf, int iBufLen, int iSecond );
Socket TcpListen( int iPort, int iListenQ, const char * pszIp = NULL, bool bIpv6 = false );
Socket TcpAccept( Socket hListenFd, char * pszIp, int iIpSize, int * piPort, bool bIpv6 = false );
bool GetLocalIpPort( Socket hSocket, std::string & strIp, int & iPort );

#ifdef WIN32
int pipe( Socket filedes[2] );
#endif

#endif
