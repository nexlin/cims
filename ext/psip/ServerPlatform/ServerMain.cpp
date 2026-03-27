#include "SipUdp.h"
#include "ServerServicePrivate.h"
#include "ServerUtility.h"
#include "FileUtility.h"
#include <stdio.h>
#include "MemoryDebug.h"

CServerService gclsService;
ServerFunc gpServerFunc;

int ServerMain( int argc, char * argv[], CServerService & clsService, ServerFunc pFunc )
{
	gclsService = clsService;
	gpServerFunc = pFunc;

#ifdef WIN32
	if( argc == 1 )
	{
		const char * pszConfigFileName = GetConfigFileName();
		if( IsExistFile( pszConfigFileName ) == false )
		{
			printf( "setup file(%s) is not exist", pszConfigFileName );
			return -1;
		}

		ServiceStart();
		return 0;
	}
#endif
	 
	if( argc == 1 )
	{
		printf( "[Usage] %s {config filename}\n", argv[0] );
		return -1;
	}

	gclsService.m_strConfigFileName = argv[1];
	if( !strcmp( argv[1], "-h" ) || !strcmp( argv[1], "-v" ) )
	{
		printf( "%s version-%s ( build %s )\n", argv[0], gclsService.m_strVersion.c_str(), gclsService.m_strBuildDate.c_str() );
		printf( "[Usage] %s {config filename}\n", argv[0] );
#ifdef WIN32
		printf( "        %s -i : install service\n", argv[0] );
		printf( "        %s -u : uninstall service\n", argv[0] );
#endif
		return 0;
	}
#ifdef WIN32
	if( !strcmp( argv[1], "-i" ) )
	{
		InstallService();
		return 0;
	}
	else if( !strcmp( argv[1], "-u" ) )
	{
		InitNetwork();
		UninstallService();
		return 0;
	}
#endif

	gpServerFunc( );

	return 0;
}
