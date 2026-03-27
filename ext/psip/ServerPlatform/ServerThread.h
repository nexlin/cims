#ifndef _SERVER_THREAD_H_
#define _SERVER_THREAD_H_

#include "SipTcp.h"
#include "MonitorCallBack.h"

// MonitorThread.cpp
bool StartMonitorThread( Socket hSocket, const char * pszIp, int iPort, IMonitorCallBack * pclsCallBack );

#endif
