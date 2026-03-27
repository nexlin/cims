#ifndef __PWOKER_H__
#define __PWOKER_H__

#include <list>

#include <memory>
#include <memory>



#include "pbase.h"
#include "prunnable.h"
#include "phandler.h"


class PWorker : public PRunnable
{
public:
   typedef std::shared_ptr<PWorker> Ptr;

   PWorker(const std::string & name, int period=0, int nStackSize=0, bool bDetached=false) 
      : PRunnable(name, period, nStackSize, bDetached), _name(name) { start(); }
   ~PWorker() {};

   void addHandler(PHandler * spHandler);
   void delHandler(PHandler * spHandler);
   void clear();

   unsigned int getSize()  // return count Handlers
   {
      return _hlist.size();
   }

   const std::string & name() { return _name; }
   const std::string info(); 
   
   virtual bool run();
private:
   std::list <PHandler *>  _hlist;
   PMutex                     _mutex;
   std::string                _name;
};

#endif // __PWOKER_H__
