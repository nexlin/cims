#include <iostream>
#include <unistd.h>
#include "pevent.h"
#include "paio.h"
#include "phandler.h"
#include "pmodule.h"
#include "psocket.h"
#include "ptcpcliaio.h"

using namespace std;

class PTbrmIfHandler : public PHandler
{
public:
   PTbrmIfHandler(const std::string & name) : PHandler(name) {}
   ~PTbrmIfHandler() {};
///
   bool init() 
   {
      _cliAio = new PTcpCliAio("CliAio", "127.0.0.1", 14355 );
      addAio(_cliAio);

      return true;
   }


   bool proc(PEvent::Ptr & spEvent) {
      if(!spEvent) {
         std::cout << "## event is null" << std::endl;   
        // exit(1);
      }
      else {
        PJsonEvent::Ptr psrc_event = spEvent.cast<PJsonEvent>(); 

        //std::cout << "Proc :" << psrc_event->getString() << std::endl;   

        std::string strSrc; 
        if(!psrc_event->getParam("source", strSrc)) {
          std::cout << "Invalid Message!" << psrc_event->getString() << std::endl;   
        }

        if(strSrc.find("tbrm") != string::npos) {
          std::cout << "#####spEvent-data: " << psrc_event->getString() << std::endl;
          procIF(spEvent);
        }
        else {
          std::cout << "#####test " << std::endl;
        }
      }
      return true;
   }

   bool procIF(PEvent::Ptr & spEvent) {
      PJsonEvent::Ptr psrc_event = spEvent.cast<PJsonEvent>(); 
      //std::cout << "ProcTestIF :" << psrc_event->getString() << std::endl;   

      std::string asid;
      std::string type;
      std::string board;
      std::string trunk;
      std::string channel;


      psrc_event->getParam("asid", asid);
      psrc_event->getParam("type", type);
      psrc_event->getParam("sesinfo board", board);
      psrc_event->getParam("sesinfo trunk", trunk);
      psrc_event->getParam("sesinfo channel", channel);

      psrc_event->addParam("type", "DONE");
      _cliAio->write(psrc_event);

      return true;
   }

   PTcpCliAio::Ptr _cliAio;
};


int main(int argc, char ** argv) 
{
   PTbrmIfHandler* pHandler1 = new PTbrmIfHandler("PTbrmIfHandler#1");
   cerr << "test1 " << endl;

   PModule module1("TbaSimModule1");
   cerr << "test1 " << endl;

   module1.addWorker("TbrmIfWorker#1");
   cerr << "test1 " << endl;

   module1.addHandler("TbrmIfWorker#1", (PHandler *)pHandler1);

   std::cout << module1.info() << std::endl;

   while(true) {
      msleep(10000);
   }

   return 0;
}

