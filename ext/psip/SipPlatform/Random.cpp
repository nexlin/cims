#include "Random.h"

#define _CRT_RAND_S
#include <stdlib.h>
#include "TimeUtility.h"

static CRandom gclsRandom;

CRandom::CRandom()
{
#ifndef WIN32
	m_iSeed = time(NULL);
#endif
}

CRandom::~CRandom()
{
}

/**
 * @ingroup SipPlatform
 * @brief random 정수를 리턴한다.
 * @returns random 정수를 리턴한다.
 */
int CRandom::Get()
{
	unsigned int iRand;

#ifdef WIN32
	rand_s( &iRand );
#else

#ifdef ANDROID
	iRand = rand();
#else
	iRand = rand_r( &m_iSeed );
#endif

#endif

	if( iRand > 2000000000 )
	{
		iRand = iRand % 2000000000;
	}

	return iRand;
}

/**
 * @ingroup SipPlatform
 * @brief random 정수를 리턴한다.
 * @returns random 정수를 리턴한다.
 */
int RandomGet()
{
	return gclsRandom.Get();
}
