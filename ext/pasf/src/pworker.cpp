#include "pbase.h"

#include "phandler.h"
#include "include/pworker.h"


void PWorker::addHandler(PHandler * spHandler) {
   _mutex.lock();
   spHandler->init();
   _hlist.push_back(spHandler);
   _mutex.unlock();
};

void PWorker::delHandler(PHandler* spHandler) {
   _mutex.lock();
   _hlist.remove(spHandler);
   _mutex.unlock();
}

void PWorker::clear() {
   _mutex.lock();
   _hlist.clear();
   _mutex.unlock();
}

bool PWorker::run() {
   std::list<PHandler*>::iterator it;
   bool bWorking = false;
   _mutex.lock();
   for(it = _hlist.begin(); it!=_hlist.end(); ++it) 
   {  
	  //std::cout << "## " << name() << "::run()->" << (*it)->name() << std::endl << std::endl;
      bool bres = ((PHandler *)(*it))->run();
	  bWorking = bWorking || bres;
   }
   _mutex.unlock();
   return bWorking;
};

const std::string PWorker::info()
{
   std::stringstream ss;
  
   ss << " - ";
   ss << "PWorker(" << _name << "):" << std::endl;

   
   std::list<PHandler*>::iterator it;
   _mutex.lock();
   for(it = _hlist.begin(); it!=_hlist.end(); ++it) {
      ss << (*it)->info();
      ss << std::endl;
   }
   _mutex.unlock();
   return ss.str();
}
