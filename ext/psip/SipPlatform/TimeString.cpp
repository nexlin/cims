#include "SipPlatformDefine.h"
#include <stdio.h>
#include "TimeString.h"
#include "StringUtility.h"
#include "MemoryDebug.h"


void LocalTime( time_t iTime, struct tm & sttTm )
{
#ifdef WIN32
	localtime_s( &sttTm, &iTime );
#else
	localtime_r( &iTime, &sttTm );	
#endif
}

void GetDateTimeString( time_t iTime, char * pszTime, int iTimeSize )
{
	struct tm	sttTm;

	LocalTime( iTime, sttTm );

	snprintf( pszTime, iTimeSize, "%04d%02d%02d%02d%02d%02d", sttTm.tm_year + 1900, sttTm.tm_mon + 1, sttTm.tm_mday
		, sttTm.tm_hour, sttTm.tm_min, sttTm.tm_sec );
}

void GetDateTimeString( char * pszTime, int iTimeSize )
{
	time_t		iTime;

	time( &iTime );

	GetDateTimeString( iTime, pszTime, iTimeSize );
}

void GetDateString( time_t iTime, char * pszDate, int iDateSize )
{
	struct tm	sttTm;

	LocalTime( iTime, sttTm );

	snprintf( pszDate, iDateSize, "%04d%02d%02d", sttTm.tm_year + 1900, sttTm.tm_mon + 1, sttTm.tm_mday );
}

void GetDateString( char * pszDate, int iDateSize )
{
	time_t		iTime;

	time( &iTime );

	GetDateString( iTime, pszDate, iDateSize );
}

void GetTimeString( time_t iTime, char * pszTime, int iTimeSize )
{
	struct tm	sttTm;

	LocalTime( iTime, sttTm );

	snprintf( pszTime, iTimeSize, "%02d%02d%02d", sttTm.tm_hour, sttTm.tm_min, sttTm.tm_sec );
}

void GetTimeString( char * pszTime, int iTimeSize )
{
	time_t		iTime;

	time( &iTime );

	GetTimeString( iTime, pszTime, iTimeSize );
}

time_t ParseDateTimeString( const char * pszTime )
{
	struct tm	sttTm;
	int iLen = (int)strlen( pszTime );

	if( iLen < 14 ) return 0;

	memset( &sttTm, 0, sizeof(sttTm) );

	sttTm.tm_year = GetInt( pszTime, 4 ) - 1900;
	sttTm.tm_mon = GetInt( pszTime+4, 2 ) - 1;
	sttTm.tm_mday = GetInt( pszTime+6, 2 );
	sttTm.tm_hour = GetInt( pszTime+8, 2 );
	sttTm.tm_min = GetInt( pszTime+10, 2 );
	sttTm.tm_sec = GetInt( pszTime+12, 2 );

	return mktime( &sttTm );
}
