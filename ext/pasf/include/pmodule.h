#ifndef __PMODULE_H__
#define __PMODULE_H__

#include <string>

#include <memory>
#include <memory>

#include "pbase.h"
#include "phandler.h"

class PModule
{
public:
   PModule(const std::string & name);
   ~PModule();
   
   bool addWorker(const std::string & workerName, 
                  int period=0,                             //proc interval milli sec 
                  int nStackSize=0, bool bDetached=false);  //thread option
   
   bool addHandler(std::string workerName, PHandler * phandler);
   bool delHandler(std::string workerName, PHandler * phandler);
   void clear();

   unsigned int getCountHandler(const std::string& workerName);

   std::string & name() { return _name; }
   std::string info(); 
   
//   virtual bool run() = 0;
private:
   void *                     _ctx;
   PMutex                     _mutex;
   std::string                _name;
};

#endif // ___PMODULE_H__

