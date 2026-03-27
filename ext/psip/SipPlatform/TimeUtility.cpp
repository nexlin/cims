#include "TimeUtility.h"
#include "MemoryDebug.h"

#ifdef WIN32
#include <sys/timeb.h>

#pragma comment( lib, "Winmm" ) 

int gettimeofday( struct timeval *tv, struct timezone *tz )
{
  struct _timeb timebuffer;

  _ftime( &timebuffer );
  tv->tv_sec = (long) timebuffer.time;
  tv->tv_usec = timebuffer.millitm * 1000;

  return 0;
}

#endif

int DiffTimeval( struct timeval * psttOld, struct timeval * psttNew )
{
	int	iRet;

	if( psttNew->tv_usec >= psttOld->tv_usec )
	{
		iRet = ( psttNew->tv_usec - psttOld->tv_usec ) / 1000;
		iRet += ( psttNew->tv_sec - psttOld->tv_sec ) * 1000;
	}
	else
	{
		iRet = 1000 + ( psttNew->tv_usec - psttOld->tv_usec ) / 1000;
		iRet += ( psttNew->tv_sec - psttOld->tv_sec - 1 ) * 1000;
	}

	return iRet;
}

void MiliSleep( int iMiliSecond )
{
#ifdef WIN32
	Sleep( iMiliSecond );
#else
	struct timespec req;
	struct timespec rem;

	req.tv_sec = (int) iMiliSecond / 1000;
	req.tv_nsec = (int)( iMiliSecond % 1000 ) * 1000000;

	nanosleep( &req, &rem );
#endif
}

uint64_t GetCurrentMiliSecond()
{
#ifdef WIN32
  return timeGetTime();
#else 
  struct timeval mtv;

  gettimeofday(&mtv, NULL);

  return (uint64_t)(mtv.tv_sec) * 1000 + (uint64_t)(mtv.tv_usec) / 1000;
#endif
}
