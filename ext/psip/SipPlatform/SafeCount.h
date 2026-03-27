#ifndef _SAFE_COUNT_H_
#define _SAFE_COUNT_H_

#include "SipMutex.h"


class CSafeCount
{
public:
	CSafeCount();
	~CSafeCount();

	void Increase();
	void Decrease();

	void SetCount( int iCount );
	int GetCount();

private:
	int m_iCount;
	CSipMutex m_clsMutex;
};

#endif
