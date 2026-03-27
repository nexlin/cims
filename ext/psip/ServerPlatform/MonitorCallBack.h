#ifndef _MONITOR_CALLBACK_H_
#define _MONITOR_CALLBACK_H_

#include "MonitorString.h"

class IMonitorCallBack
{
public:
	IMonitorCallBack() : m_iMonitorPort(0), m_bIpv6(false)
	{
	}

	virtual ~IMonitorCallBack(){};

	virtual bool RecvRequest( const char * pszRequest, CMonitorString & strResponse ) = 0;

	virtual bool IsMonitorIp( const char * pszIp ) = 0;

	int		m_iMonitorPort;

	bool	m_bIpv6;
};

#endif
