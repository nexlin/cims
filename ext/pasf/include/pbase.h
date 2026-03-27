#ifndef	__PBASE_H__
#define	__PBASE_H__

#include <stdio.h>
#include <stdlib.h>
#include <queue>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <string.h>


#ifdef PDEBUG
   #define  PDPRINT(v...)     fprintf(stdout, v)
#else 
   #define  PDPRINT(v...) 
#endif 


#define  PEPRINT(v...)     fprintf(stderr, v)


#define PDIFFTIME(tvp, uvp) \
    abs((tvp.tv_sec  - uvp.tv_sec) * 1000 + \
    (tvp.tv_usec - uvp.tv_usec) / 1000)

inline void msleep(int msec)
{
#if 0
   timespec req;

   req.tv_sec = 0;
   req.tv_nsec = msec * 1000000;

   nanosleep(&req, NULL);
#else
   struct timeval val;
   val.tv_sec = msec / 1000;
   val.tv_usec = (msec % 1000) * 1000;
   select(0, 0, 0, 0, &val);
#endif
}


class PMutex
{
public:
	PMutex()
	{
		pthread_mutexattr_t _mutexAttr;
		pthread_mutexattr_init(&_mutexAttr);
		pthread_mutexattr_setpshared(&_mutexAttr, PTHREAD_PROCESS_PRIVATE);
		pthread_mutex_init(&_mutex, &_mutexAttr);
		pthread_mutexattr_destroy(&_mutexAttr);
	}
	~PMutex() { pthread_mutex_destroy(&_mutex); }
	void lock() { pthread_mutex_lock(&_mutex); }
	void unlock() { pthread_mutex_unlock(&_mutex); }

   pthread_mutex_t & get() { return _mutex; }
protected:
	pthread_mutex_t _mutex;
};


class PAutoLock
{
public:
	PAutoLock(PMutex & mutex) : _mutex(mutex) { _mutex.lock(); }
	~PAutoLock() { _mutex.unlock(); }

protected:
	PMutex &_mutex;
};


class PCondition
{
public:
   PCondition() {
      pthread_cond_init(&_cond, NULL);
   }

   ~PCondition() {
      pthread_cond_destroy(&_cond);
   }

   void wait(PMutex & mutex) {
      pthread_cond_wait(&_cond, &mutex.get());
   }

   void signal() {
      pthread_cond_signal(&_cond);   
   }

private:
   pthread_cond_t _cond;
};


template<typename T>
class PQueue
{
private:
   PMutex         _mutex;
   PCondition     _cond;
   std::queue<T>  _queue;
   int            _max;

public:
   PQueue(int max) : _max(max) {}

   int  size() 
   {
      PAutoLock lock(_mutex);
      return _queue.size();
   }

   int  max() 
   {
      return _max;
   }

   bool push(T const& data)
   {
      bool bRes = false;
      _mutex.lock();
      if(_queue.size() < _max) {
         _queue.push(data);
         bRes = true;
      }
      _mutex.unlock();
      _cond.signal();
      return bRes;
   }

   bool empty() const
   {
      PAutoLock lock(_mutex);
      return _queue.empty();
   }

   void clear() 
   {
      PAutoLock lock(_mutex);
      _queue = std::queue<T>();
   }

   bool pop(T& data) // non blocking
   {
      PAutoLock lock(_mutex);
      if(_queue.empty())
      {
         return false;
      }

      data = _queue.front();
      _queue.pop();
      return true;
   }

   void wait_pop(T& data)
   {
      PAutoLock lock(_mutex);
      while(_queue.empty())
      {
         _cond.wait(_mutex);
      }

      data = _queue.front();
      _queue.pop();
   }
   
};

#endif //	__PBASE_H__
