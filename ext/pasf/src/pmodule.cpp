#include <string>
#include <map>

#include "pworker.h"

#include "pmodule.h"



struct PModuleCtx {
   std::map <std::string, PWorker*> _wmap;
};

#define WMAP (((PModuleCtx*)_ctx)->_wmap)

PModule::PModule(const std::string & name) 
   : _name(name)
{
   _ctx = new PModuleCtx;
}

PModule::~PModule() 
{
   _mutex.lock();
   delete (PModuleCtx *)_ctx;;
   _mutex.unlock();
}

bool PModule::addWorker(const std::string & workerName, int period, int nStackSize, bool bDetached) 
{
   PWorker * pworker = new PWorker(workerName, period, nStackSize, bDetached);

   _mutex.lock();
   WMAP[workerName] = pworker;
   _mutex.unlock();

   return true;
}

unsigned int PModule::getCountHandler(const std::string& workerName)
{
   std::map<std::string, PWorker *>::iterator it;
   unsigned int count = 0;
   _mutex.lock();
   it = WMAP.find(workerName);
   if (it == WMAP.end()) 
   {
	  _mutex.unlock();
	  return false;
   }
   count = it->second->getSize();
   _mutex.unlock();
   return count;
}


bool PModule::addHandler(std::string workerName, PHandler * spHandler) 
{
   std::map<std::string, PWorker*>::iterator it;

   _mutex.lock();
   it = WMAP.find(workerName);
   if(it == WMAP.end()) 
   {
	  _mutex.unlock();
	  return false;
   }
   it->second->addHandler(spHandler);
   _mutex.unlock();
   return true;
}

bool PModule::delHandler(std::string workerName, PHandler * spHandler) 
{
   std::map<std::string, PWorker*>::iterator it;

   _mutex.lock();
   it = WMAP.find(workerName); 
   if(it == WMAP.end())
   {
	  _mutex.unlock();
	  return false;
   }
   it->second->delHandler(spHandler);
   _mutex.unlock();

   return true;
}

void PModule::clear() 
{

   _mutex.lock();
   WMAP.clear();
   _mutex.unlock();
}

std::string PModule::info() 
{
   std::stringstream ss;
   ss << "PModule(" << _name << ")" << std::endl;

   std::map<std::string, PWorker*>::iterator it;

   _mutex.lock();
   for(it = WMAP.begin(); it!= WMAP.end(); it++) {
      ss << it->second->info(); 
   }
   _mutex.unlock();
   
   return ss.str();
}

