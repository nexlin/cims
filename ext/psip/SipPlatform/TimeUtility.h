#ifndef _TIME_UTILITY_H_
#define _TIME_UTILITY_H_

#include "SipPlatformDefine.h"
#include <time.h>

#ifdef WIN32
int gettimeofday( struct timeval *tv, struct timezone *tz );
#endif

int DiffTimeval( struct timeval * psttOld, struct timeval * psttNew );
void MiliSleep( int iMiliSecond );
uint64_t GetCurrentMiliSecond();

#endif
