#include "SipTcp.h"
#include "Log.h"
#include "MonitorCallBack.h"
#include "ServerService.h"
#include "ServerUtility.h"
#include "MemoryDebug.h"

static int giMonitorThreadCount = 0;
CSipMutex gclsCountMutex;

class CMonitorSocket
{
public:
	Socket			hSocket;
	std::string	m_strIp;
	int					m_iPort;
	IMonitorCallBack * m_pclsCallBack;
};

static bool MonitorCommand( CMonitorSocket * pclsArg, const char * pszPacket )
{
	CMonitorString	strBuf;
	int	iPacketLen, n;

	if( pclsArg->m_pclsCallBack->RecvRequest( pszPacket, strBuf ) == false )
	{
		CLog::Print( LOG_DEBUG, "MonitorThread(%s:%d) command(%s) is not defined", pclsArg->m_strIp.c_str(), pclsArg->m_iPort, pszPacket );
		return false;
	}

	iPacketLen = htonl( strBuf.GetLength() );

	n = TcpSend( pclsArg->hSocket, (char *)&iPacketLen, sizeof(iPacketLen) );
	if( n != sizeof(iPacketLen) )
	{
		CLog::Print( LOG_DEBUG, "MonitorThread(%s:%d) send header error(%d)", pclsArg->m_strIp.c_str(), pclsArg->m_iPort, GetError() );
		return false;
	}

	n = TcpSend( pclsArg->hSocket, strBuf.GetString(), strBuf.GetLength() );
	if( n != strBuf.GetLength() )
	{
		CLog::Print( LOG_DEBUG, "MonitorThread(%s:%d) send body error(%d)", pclsArg->m_strIp.c_str(), pclsArg->m_iPort, GetError() );
		return false;
	}

	return true;
}

THREAD_API MonitorThread( LPVOID lpParameter )
{
	CMonitorSocket * pclsArg = ( CMonitorSocket * )lpParameter;
	char	szPacket[1024];
	int		iPacketLen, n, iNoCommandSecond = 0;

	pollfd sttPoll[1];

	sttPoll[0].fd = pclsArg->hSocket;
	sttPoll[0].events = POLLIN;
	sttPoll[0].revents = 0;

	gclsCountMutex.acquire();
	++giMonitorThreadCount;
	gclsCountMutex.release();

	CLog::Print( LOG_INFO, "MonitorThread(%s:%d) is started", pclsArg->m_strIp.c_str(), pclsArg->m_iPort );

	while( gbStop == false )
	{
		n = poll( sttPoll, 1, 1000 );
		if( n < 0 )
		{
			CLog::Print( LOG_ERROR, "MonitorThread(%s:%d) poll error(%d)", pclsArg->m_strIp.c_str(), pclsArg->m_iPort, GetError() );
			break;
		}
		else if( n == 0 )
		{
			++iNoCommandSecond;
			if( iNoCommandSecond == 600 ) 
			{
				CLog::Print( LOG_DEBUG, "MonitorThread(%s:%d) no command received in 600 second", pclsArg->m_strIp.c_str(), pclsArg->m_iPort );
				break;
			}

			continue;
		}

		iNoCommandSecond = 0;

		n = recv( pclsArg->hSocket, (char *)&iPacketLen, sizeof(iPacketLen), 0 );
		if( n <= 0 )
		{
			CLog::Print( LOG_DEBUG, "MonitorThread(%s:%d) recv header(%d)", pclsArg->m_strIp.c_str(), pclsArg->m_iPort, n );
			break;
		}

		iPacketLen = ntohl( iPacketLen );
		if( iPacketLen <= 0 || iPacketLen >= (int)sizeof(szPacket) )
		{
			CLog::Print( LOG_DEBUG, "MonitorThread(%s:%d) recv command length(%d)", pclsArg->m_strIp.c_str(), pclsArg->m_iPort, iPacketLen );
			break;
		}

		n = TcpRecv( pclsArg->hSocket, szPacket, iPacketLen, 5 );
		if( n <= 0 )
		{
			CLog::Print( LOG_DEBUG, "MonitorThread(%s:%d) recv command body(%d)", pclsArg->m_strIp.c_str(), pclsArg->m_iPort, n );
			break;
		}

		szPacket[n] = '\0';
		if( MonitorCommand( pclsArg, szPacket ) == false )
		{
			break;
		}
	}

	closesocket( pclsArg->hSocket );

	CLog::Print( LOG_INFO, "MonitorThread(%s:%d) is terminated", pclsArg->m_strIp.c_str(), pclsArg->m_iPort );
	delete pclsArg;

	gclsCountMutex.acquire();
	--giMonitorThreadCount;
	gclsCountMutex.release();

	return 0;
}

bool StartMonitorThread( Socket hSocket, const char * pszIp, int iPort, IMonitorCallBack * pclsCallBack )
{
	CMonitorSocket * pclsArg = new CMonitorSocket();
	if( pclsArg == NULL ) return false;

	pclsArg->hSocket = hSocket;
	pclsArg->m_strIp = pszIp;
	pclsArg->m_iPort = iPort;
	pclsArg->m_pclsCallBack = pclsCallBack;

	if( StartThread( "MonitorThread", MonitorThread, pclsArg ) == false ) return false;

	return true;
}

bool IsMonitorThreadRun()
{
	if( giMonitorThreadCount > 0 ) return true;

	return false;
}

