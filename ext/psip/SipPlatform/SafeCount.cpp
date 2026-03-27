#include "SafeCount.h"

CSafeCount::CSafeCount() : m_iCount(0)
{
}

CSafeCount::~CSafeCount()
{
}

/**
 * @ingroup SipPlatform
 * @brief 카운트를 1 증가시킨다.
 */
void CSafeCount::Increase()
{
	m_clsMutex.acquire();
	++m_iCount;
	m_clsMutex.release();
}

/**
 * @ingroup SipPlatform
 * @brief 카운트를 1 감소시킨다.
 */
void CSafeCount::Decrease()
{
	m_clsMutex.acquire();
	--m_iCount;
	m_clsMutex.release();
}

/**
 * @ingroup SipPlatform
 * @brief 카운트를 재설정한다.
 * @param iCount 카운트
 */
void CSafeCount::SetCount( int iCount )
{
	m_clsMutex.acquire();
	m_iCount = iCount;
	m_clsMutex.release();
}

/**
 * @ingroup SipPlatform
 * @brief 카운트를 리턴한다.
 * @returns 카운트를 리턴한다.
 */
int CSafeCount::GetCount()
{
	int iCount;

	m_clsMutex.acquire();
	iCount = m_iCount;
	m_clsMutex.release();

	return iCount;
}
