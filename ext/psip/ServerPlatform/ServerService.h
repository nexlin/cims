#ifndef _SERVER_SERVICE_H_
#define _SERVER_SERVICE_H_

#include <string>
#include "MonitorCallBack.h"

class CServerService
{
public:
	void SetBuildDate( const char * pszDate, const char * pszTime )
	{
		m_strBuildDate = pszDate;
		m_strBuildDate.append( " " );
		m_strBuildDate.append( pszTime );
	}

	std::string m_strName;
	std::string m_strDisplayName;
	std::string m_strDescription;
	std::string m_strVersion;
	std::string m_strConfigFileName;
	std::string	m_strBuildDate;
};

typedef int (*ServerFunc)();

extern bool gbStop;

int ServerMain( int argc, char * argv[], CServerService & clsService, ServerFunc pFunc );
const char * GetConfigFileName();
void ServerSignal();

bool IsMonitorThreadRun();

bool StartMonitorServerThread( IMonitorCallBack * pclsCallBack );

#endif
