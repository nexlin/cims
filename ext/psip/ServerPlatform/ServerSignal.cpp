#include "SipPlatformDefine.h"
#include "Log.h"
#include <signal.h>
#include <string>
#include "MemoryDebug.h"

#ifndef WIN32
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#endif

bool gbStop = false;

void LastMethod( int sig )
{
	char	szText[21];

	szText[0] = '\0';
	switch( sig )
	{
	case SIGINT:
		snprintf( szText, sizeof(szText), "SIGINT" );
		break;
	case SIGSEGV:
		snprintf( szText, sizeof(szText), "SIGSEGV" );
		break;
	case SIGTERM:
		snprintf( szText, sizeof(szText), "SIGTERM" );
		break;
#ifndef WIN32
	case SIGQUIT:
		snprintf( szText, sizeof(szText), "SIGQUIT" );
		break;
#endif
	case SIGABRT:
		snprintf( szText, sizeof(szText), "SIGABRT" );
		break;
	}

	CLog::Print( LOG_ERROR, "signal%s%s(%d) is received. terminated", strlen(szText) > 0 ? "-" : "", szText, sig );

#ifndef WIN32
	if( sig == SIGSEGV )
	{
		CLog::PrintCallStack( LOG_ERROR );

		signal( sig, SIG_DFL ); 
    kill( getpid(), sig );
		return;
	}
#endif

	gbStop = true;
}

void ServerSignal()
{
	signal( SIGINT, LastMethod );
	signal( SIGTERM, LastMethod );
	signal( SIGABRT, LastMethod );
#ifndef WIN32
	signal( SIGSEGV, LastMethod );
	signal( SIGKILL, LastMethod );
	signal( SIGQUIT, LastMethod );
	signal( SIGPIPE, SIG_IGN );
#endif
}
