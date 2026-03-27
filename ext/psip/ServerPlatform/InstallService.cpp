#ifdef WIN32

#include "SipPlatformDefine.h"
#include "ServerServicePrivate.h"
#include <windows.h>
#include <stdio.h>
#include "MemoryDebug.h"

bool IsExistService()
{
	SC_HANDLE	hScm, hSrv;
	bool		fisResult = false;

	hScm = OpenSCManager( NULL, NULL, SC_MANAGER_CREATE_SERVICE );
	if( hScm )
	{
		hSrv = OpenService( hScm, gclsService.m_strName.c_str(), SERVICE_ALL_ACCESS );
		if( hSrv ) 
		{
			CloseServiceHandle( hSrv );
			fisResult = true;
		}

		CloseServiceHandle(hScm);
	}

	return fisResult;
}

bool InstallService( )
{
	if( IsExistService() )
	{
		printf( "%s is already installed.\n", gclsService.m_strName.c_str() );
		return false;
	}

	SC_HANDLE hScm, hSrv;
	char SrvPath[MAX_PATH];
	SERVICE_DESCRIPTION lpDes;

	hScm=OpenSCManager( NULL, NULL, SC_MANAGER_CREATE_SERVICE );
	if (hScm==NULL) 
	{
		printf( "Can not open SCM. Start dos with administrator authority\n" );
		return false;
	}

	::GetModuleFileName( NULL, SrvPath, sizeof(SrvPath) );

	hSrv=CreateService( hScm
		        , gclsService.m_strName.c_str()
						, gclsService.m_strDisplayName.c_str()
					  , SERVICE_PAUSE_CONTINUE | SERVICE_CHANGE_CONFIG
					  ,	SERVICE_WIN32_OWN_PROCESS
					  , SERVICE_AUTO_START
					  , SERVICE_ERROR_IGNORE
					  , SrvPath
					  ,	NULL,NULL,NULL,NULL,NULL);
	
	if( hSrv == NULL ) 
	{
		printf( "Install is failed.\n" );
	} 
	else 
	{
		lpDes.lpDescription = (LPSTR)gclsService.m_strDescription.c_str();
		ChangeServiceConfig2( hSrv, SERVICE_CONFIG_DESCRIPTION, &lpDes );

		printf( "Installed.\n" );
		CloseServiceHandle(hSrv);
	}

	CloseServiceHandle(hScm);

	return true;
}

bool UninstallService( )
{
	SC_HANDLE hScm, hSrv;
	SERVICE_STATUS	ss;

	hScm = OpenSCManager( NULL, NULL, SC_MANAGER_CREATE_SERVICE );
	if( hScm == NULL ) 
	{
		printf( "Can not open SCM.\n" );
		return false;
	}

	hSrv = OpenService( hScm, gclsService.m_strName.c_str(), SERVICE_ALL_ACCESS );
	if( hSrv == NULL ) 
	{
		CloseServiceHandle(hScm);
		printf( "%s is not installed.\n", gclsService.m_strName.c_str() );
		return false;
	}

	QueryServiceStatus( hSrv, &ss );
	if( ss.dwCurrentState != SERVICE_STOPPED ) 
	{
		ControlService( hSrv, SERVICE_CONTROL_STOP, &ss );
		Sleep( 2000 );
	}

	if( DeleteService( hSrv ) ) 
	{
		printf( "Service is deleted.\n" );
	} 
	else 
	{
		printf( "Deleting service is failed.\n" );
	}

	CloseServiceHandle( hSrv );
	CloseServiceHandle( hScm );

	return true;
}

#endif
