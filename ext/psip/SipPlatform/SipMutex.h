#ifndef _SIP_MUTEX_H_
#define _SIP_MUTEX_H_

#ifdef WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/** 
 * @ingroup SipPlatform
 * @brief mutex 기능을 수행하는 클래스
 */
class CSipMutex
{
public:
	CSipMutex();
	~CSipMutex();
	
	bool acquire();
	bool release();

protected:
#ifdef WIN32
	CRITICAL_SECTION m_sttMutex;
#else
	pthread_mutex_t	 m_sttMutex;
#endif
};

/** 
 * @ingroup SipPlatform
 * @brief mutex 기능 및 wait/signal 기능을 수행하는 클래스. 리눅스에 최적화되어 있고 윈도우에는 최적화되어 있지 않음.
 */
class CSipMutexSignal : public CSipMutex
{
public:
	CSipMutexSignal();
	~CSipMutexSignal();
	
	bool wait();
	bool signal();
	bool broadcast();

private:
#ifdef WIN32
	CONDITION_VARIABLE		m_sttCond;
#else
	pthread_cond_t		m_sttCond;
#endif
};

#endif
