#include "SipPlatformDefine.h"
#include "ServerServicePrivate.h"

#ifdef WIN32

#include "Directory.h"
#include <windows.h>
#include <stdio.h>
#include "MemoryDebug.h"

static SERVICE_STATUS_HANDLE	ghServiceStatus;
static DWORD					giNowState;

void ServiceSetStatus( DWORD dwState, DWORD dwAccept = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PAUSE_CONTINUE )
{
	SERVICE_STATUS ss;

	ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	ss.dwCurrentState = dwState;
	ss.dwControlsAccepted = dwAccept;
	ss.dwWin32ExitCode = 0;
	ss.dwServiceSpecificExitCode = 0;
	ss.dwCheckPoint = 0;
	ss.dwWaitHint = 0;

	giNowState = dwState;
	SetServiceStatus( ghServiceStatus, &ss );
}

void ServiceHandler( DWORD fdwControl )
{
	// if current status is equal input status, there is nothing to do.
	if( fdwControl == giNowState ) return;

	switch( fdwControl ) 
	{
	case SERVICE_CONTROL_PAUSE:
		break;
	case SERVICE_CONTROL_CONTINUE:
		break;
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		ServiceSetStatus( SERVICE_STOP_PENDING, 0 );
		gbStop = true;
		break;
	case SERVICE_CONTROL_INTERROGATE:
	default:
		ServiceSetStatus( giNowState );
		break;
	}
}

void ServiceMain( DWORD , LPTSTR * )
{
	// regist service handler.
	ghServiceStatus = RegisterServiceCtrlHandler( gclsService.m_strName.c_str(), (LPHANDLER_FUNCTION)ServiceHandler );
	if( ghServiceStatus == 0 ) 
	{
		return;
	}

	// set status that service is starting.
	ServiceSetStatus(SERVICE_START_PENDING);
	
	// set status that service is working.
	ServiceSetStatus(SERVICE_RUNNING);

	gpServerFunc();

	// QQQ : if this process is terminated with signal, below code must be executed.
	ServiceSetStatus( SERVICE_STOPPED );
}

void ServiceStart()
{
	SERVICE_TABLE_ENTRY ste[]={
		{ (LPSTR)gclsService.m_strName.c_str(), (LPSERVICE_MAIN_FUNCTION)ServiceMain },
		{ NULL        , NULL}
	};

	StartServiceCtrlDispatcher(ste);
}

const char * GetConfigFileName()
{
	static char szConfigFileName[1024];

	if( gclsService.m_strConfigFileName.length() > 3 &&	!strncmp( gclsService.m_strConfigFileName.c_str() + 1, ":\\", 2 ) )
	{
		return gclsService.m_strConfigFileName.c_str();
	}

	snprintf( szConfigFileName, sizeof(szConfigFileName), "%s\\%s", CDirectory::GetProgramDirectory(), gclsService.m_strConfigFileName.c_str() );

	return szConfigFileName;
}

#else

const char * GetConfigFileName()
{
	return gclsService.m_strConfigFileName.c_str();
}

#endif
