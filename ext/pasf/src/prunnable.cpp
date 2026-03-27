//
#include <time.h>
#include <iostream>
#include <thread>
#include <chrono>

#include <pthread.h>   // pthread...
#include <semaphore.h> // sem_...

//#include "PMPBase.h"
#include "pbase.h"
#include "prunnable.h"



static double GetCpuUsage()
{
   struct timespec ts;
   clockid_t cid;

   pthread_t tid = pthread_self();
   pthread_getcpuclockid(tid, &cid);
   clock_gettime(cid,&ts);
   return ts.tv_sec + (((double)ts.tv_nsec)*0.000000001);
}

// 
class PThread
{
enum { PTHREAD_STACK_SIZE = (1024*10) };
public:
   PThread(const std::string & name, int period) : _name(name), _period(period)
   { 
      sem_init(&_semStopped, 0, 0); 
   };

   virtual ~PThread() {
      _bstop = true;

      sem_trywait(&_semStopped);
      sem_destroy(&_semStopped);
      //wait for exit thread signal
   };

   bool start(PRunnable *prunner, int nStackSize = 0, bool bDetached = false) {
	  _lastTimeSlot = 0;
	  gettimeofday(&_tvTimeStart, NULL);
      _bstop  = false;

      _prunner = prunner;

      pthread_attr_t tattr;
      pthread_attr_init(&tattr);

      if (nStackSize > PTHREAD_STACK_SIZE)
         pthread_attr_setstacksize(&tattr, nStackSize);

      if (bDetached)
         pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);

      if (pthread_create(&_thread,
               &tattr,
               PThread::_sthreadProc,
               (void *)this) == 0) {

         return false;
      }	
      pthread_attr_destroy(&tattr);


      return true;

   };

   static void * _sthreadProc(void * pParam) {
      PThread * pthread = (PThread *)pParam;
      return pthread->threadProc();
   }

   virtual void * threadProc() {
      while(!_bstop) {
		 if(_period>0) 
		 {
			struct timeval tvCurTime;
			gettimeofday(&tvCurTime, NULL);
			unsigned int nDiffTimeMs = PDIFFTIME(tvCurTime, _tvTimeStart);

			unsigned int procCnt = nDiffTimeMs / _period;
			unsigned int loopCnt  = (procCnt>_lastTimeSlot)?(procCnt-_lastTimeSlot):0;

			if(loopCnt > 5) 
			{
			   _lastTimeSlot = 0;
			   gettimeofday(&_tvTimeStart, NULL);
			   loopCnt=5;
			}
		 
			for(int i=0; i<loopCnt; i++)
			{
			   _prunner->run();
			   _lastTimeSlot++;
			   //if(_lastTimeSlot % 500 == 0)
			   //  LOG(LINF, "## %s -> CPU :%f\n", _name.c_str(), GetCpuUsage());
			   //printf("## %s -> LOOP:%d, period=%d, slot=%u/%u\n", _name.c_str(), loopCnt, _period, _lastTimeSlot, procCnt);
			}

			if(loopCnt > 0) 
			{
			   if(loopCnt > 1) 
			   {
				  //LOG(LINF, "## %s -> LOOP:%d\n", _name.c_str(), loopCnt);
				  //printf("## %s -> LOOP:%d\n", _name.c_str(), loopCnt);
			   }	
			} 
			else 
			{
			   std::this_thread::sleep_for(std::chrono::microseconds(1000));
			} 
		 }
		 else 
		 {
			bool bRes = _prunner->run();
			if(!bRes) std::this_thread::sleep_for(std::chrono::microseconds(1000));
		 }


	  }
	  sem_trywait(&_semStopped);
	  //send exit thread signal; 
	  return NULL;
   }

   std::string _name;
   pthread_t   _thread;
   sem_t       _semStopped;
   bool        _bstop;

   unsigned long _loop;
   int			 _period; //nano sec
   unsigned int  _lastTimeSlot;
   struct timeval _tvTimeStart;

   PRunnable *  _prunner;

   
   //unsigned _mstime; // if no working, sleep (nano-second) 
};


PRunnable::PRunnable(const std::string & name, int period, int nStackSize, bool bDetached)
{
   _ctx = new PThread(name, period);
   _nStackSize = nStackSize;
   _bDetached = bDetached;
}

void PRunnable::start() 
{
   ((PThread *)_ctx)->start(this, _nStackSize, _bDetached);
}


PRunnable::~PRunnable() 
{
   delete (PThread *)_ctx;
}

#if 0
bool Runnable::start()
{
   return _ctx->start(this);
}
#endif

