#include "SipMutex.h"
#include "MemoryDebug.h"


CSipMutex::CSipMutex( )
{
#ifdef WIN32
	InitializeCriticalSection( &m_sttMutex );
#else
	pthread_mutex_init( &m_sttMutex, NULL );
#endif
}


CSipMutex::~CSipMutex()
{
#ifdef WIN32
	DeleteCriticalSection( &m_sttMutex );
#else
	pthread_mutex_destroy( &m_sttMutex );
#endif
}


bool CSipMutex::acquire()
{
#ifdef WIN32
	EnterCriticalSection( &m_sttMutex );
#else
	if( pthread_mutex_lock( &m_sttMutex ) != 0 )
	{
		return false;
	}
#endif

	return true;
}


bool CSipMutex::release()
{
#ifdef WIN32
	LeaveCriticalSection( &m_sttMutex );
#else
	if( pthread_mutex_unlock( &m_sttMutex ) != 0 )
	{
		return false;
	}
#endif

	return true;
}


CSipMutexSignal::CSipMutexSignal()
{
#ifdef WIN32
	InitializeConditionVariable( &m_sttCond );
#else
	pthread_cond_init( &m_sttCond, NULL );
#endif
}


CSipMutexSignal::~CSipMutexSignal()
{
#ifdef WIN32
#else
	pthread_cond_destroy( &m_sttCond );
#endif
}


bool CSipMutexSignal::wait()
{
#ifdef WIN32
	SleepConditionVariableCS( &m_sttCond, &m_sttMutex, INFINITE );

	return true;
#else
	int n = pthread_cond_wait( &m_sttCond, &m_sttMutex );
	if( n != 0 )
	{
		return false;
	}
#endif

	return true;
}


bool CSipMutexSignal::signal()
{
#ifdef WIN32
	WakeConditionVariable( &m_sttCond );
#else
	int n = pthread_cond_signal( &m_sttCond );
	if( n != 0 )
	{
		return false;
	}
#endif

	return true;
}


bool CSipMutexSignal::broadcast()
{
#ifdef WIN32
	WakeAllConditionVariable( &m_sttCond );
#else
	int n = pthread_cond_broadcast( &m_sttCond );
	if( n != 0 )
	{
		return false;
	}
#endif

	return true;
}
