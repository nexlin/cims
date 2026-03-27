#ifndef __PRUNNABLE_H__
#define __PRUNNABLE_H__

class PRunnable
{
public:	
	PRunnable(const std::string& name, int period=0, int nStackSize=0, bool bDetached=false); // if no run, call sleep(msec_sleep)
	virtual ~PRunnable();
   //void setSleepTime(unsigned msec_sleep);
   void  start();
	
	virtual bool run() = 0; // true : be working, false : idle

   //void start();
   
private:
   void * _ctx;
   std::string _name;
   int   _nStackSize;
   bool  _bDetached;
      //CThread _thread;
};

#endif // __PRUNNABLE_H__

