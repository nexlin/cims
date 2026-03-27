
#include <stdio.h>

//#include "../../src/include/pevent.h"
#include "pevent.h"
#include "paio.h"
#include "pipcaio.h"
#include "phandler.h"
#include "pmodule.h"


class PRMCliAio : public PAio
{
public:
   PRMCliAio(const std::string &name) : PAio(0, name) { _cnt=0;}
   ~PRMCliAio() {};

   bool read(PEvent::Ptr & spEvent) {
      //if(_cnt++%10 != 0) return false; 
      //std::cout << name() << " : PEvent ptr :" << spEvent.get() << std::endl;   
      //msleep(100);
      PJsonEvent::Ptr spDstEvent = std::make_shared<PJsonEvent>(name());

      spDstEvent->addParam("source", name());
      spDstEvent->addParam("asid",   name());
      spDstEvent->addParam("type", "ADD");

      spDstEvent->addParam("sesinfo board", "1");
    

      std::stringstream trunk;
      //trunk << (_cnt/10)%4 + 1;

      trunk << _cnt % 4 + 1;

      spDstEvent->addParam("sesinfo trunk", trunk.str());
      //std::cout << "msg :" << spDstEvent->getString() << std::endl;
      spEvent = spDstEvent;

      return true;
   }

   bool write(PEvent::Ptr spEvent) {
      //PJsonEvent::Ptr spSrcEvent = spEvent.cast<PJsonEvent>();

      std::cout << "PRMCliAio(" << name() << ")->write() : " << spEvent->getMsg() << std::endl;
      //std::cout << name() << "Write : " << spSrcEvent->getMsg() << std::endl;

      return true;
   }
   unsigned int    _cnt;
};

class PLoopAio : public PAio
{
public:
   PLoopAio(const std::string & name) : PAio(0, name) {}
   ~PLoopAio() {};

   bool read(PEvent::Ptr & spEvent) {
      if(!_pstoredEvent) return false;
      //std::cout << name() << " : PEvent ptr :" << spEvent.get() << std::endl;   
      
      PJsonEvent::Ptr spSrcEvent = std::dynamic_pointer_cast<PJsonEvent>(_pstoredEvent); 
      PJsonEvent::Ptr spDstEvent = std::make_shared<PJsonEvent>(name() + "->write");

      std::string asid;
      std::string type;
      std::string board;
      std::string trunk;
      std::string channel;
      
      spSrcEvent->getParam("type", type);
      spSrcEvent->getParam("sesinfo board", board);
      spSrcEvent->getParam("sesinfo trunk", trunk);
      spSrcEvent->getParam("sesinfo channel", channel);

      type += "_RES";
      spDstEvent->addParam("source", name());
      spDstEvent->addParam("asid", asid);
      spDstEvent->addParam("type", type);
      spDstEvent->addParam("sesinfo board", board);
      spDstEvent->addParam("sesinfo trunk", trunk);
      spDstEvent->addParam("sesinfo channel", channel);

      _pstoredEvent = NULL; // clear

      //spEvent = spDstEvent.cast<PEvent>();
      spEvent = spDstEvent; 
      return true;
   }

   bool write(PEvent::Ptr spEvent) {
      //PJsonEvent * spSrcEvent = (PJsonEvent *)(PEvent *)spEvent;
      PJsonEvent::Ptr spSrcEvent = std::dynamic_pointer_cast<PJsonEvent>(spEvent); 
      PJsonEvent::Ptr spDstEvent = std::make_shared<PJsonEvent>(name() + "->write");

      std::string asid;
      std::string type;
      std::string board;
      std::string trunk;
      std::string channel;
      
      spSrcEvent->getParam("asid", asid);
      spSrcEvent->getParam("type", type);
      spSrcEvent->getParam("sesinfo board", board);
      spSrcEvent->getParam("sesinfo trunk", trunk);
      spSrcEvent->getParam("sesinfo channel", channel);


      type += "_RES";
      spDstEvent->addParam("asid", asid);
      spDstEvent->addParam("type", type);
      spDstEvent->addParam("sesinfo board", board);
      spDstEvent->addParam("sesinfo trunk", trunk);
      spDstEvent->addParam("sesinfo channel", channel);

      //std::cout << " ### stored event ptr : " << _pstoredEvent.get() << std::endl;
      _pstoredEvent = spDstEvent;

      return true;
   }

   PEvent::Ptr _pstoredEvent;
};



class PRMIFHandler : public PHandler
{
public:
   PRMIFHandler(const std::string & name) : PHandler(name) {}
   ~PRMIFHandler() {};
///
   bool init() {

      _aioRmIf= new PRMCliAio("RMIF");
      addAio(_aioRmIf);

      //PIPCAio * pTmpAio = new PIPCAio("RMIF");

      //pTmpAio
      _ipcTrunk1 = new PIPCAio(1, "RMIF");
      _ipcTrunk2 = new PIPCAio(2, "RMIF");
      _ipcTrunk3 = new PIPCAio(3, "RMIF");
      _ipcTrunk4 = new PIPCAio(4, "RMIF");

      _ipcTrunk1->open("RMIF", 100, "Trunk1", 100);
      _ipcTrunk2->open("RMIF", 100, "Trunk2", 100);
      _ipcTrunk3->open("RMIF", 100, "Trunk3", 100);
      _ipcTrunk4->open("RMIF", 100, "Trunk4", 100);

      addAio(_ipcTrunk1);
      addAio(_ipcTrunk2);
      addAio(_ipcTrunk3);
      addAio(_ipcTrunk4);

      return true;
   }


#if 0
   bool proc() {
      return false;
   }
#endif  

   bool proc() { return true; }

   bool proc(int id, const std::string & eventName, PEvent::Ptr spEvent) 
   {
      //std::cout << name() << " : PEvent ptr :" << spEvent.get() << std::endl;   

      if(!spEvent) {
         std::cout << "## event is null" << std::endl;   
         
         exit(1);
      }

      std::string strSrc;
      PJsonEvent::Ptr spSrcEvent = std::dynamic_pointer_cast<PJsonEvent>(spEvent); 

      //std::cout << "PRMIFHandler(" << name() << ")" 
      //          << "->proc(spEvent=" << spSrcEvent->getString() << std::endl;
      
      if(!spSrcEvent->getParam("source", strSrc)) {
         std::cout << "Invalid Event! : PRMIFHandler(" << name() << ")" 
                   << "->proc(spEvent=" << spSrcEvent->getString() << std::endl;
      }

      if(strSrc == "RMIF") 
      {
         procRMIF(spEvent);
      } else if(strSrc == "Trunk1" 
             || strSrc == "Trunk2"
             || strSrc == "Trunk3"
             || strSrc == "Trunk4")
      {
         procTrunk(spEvent);
      }
      return true;
   }


   bool procRMIF(PEvent::Ptr spEvent) 
   {
      //std::cout << "ProcTestIF spEvent :" << spEvent->getMsg() << std::endl;   
      PJsonEvent::Ptr spSrcEvent = std::dynamic_pointer_cast<PJsonEvent>(spEvent); 
      //std::cout << "ProcTestIF spSrcEvent:" << spSrcEvent->getString() << std::endl;   

      std::string asid;
      std::string type;
      std::string board;
      std::string trunk;
      std::string channel;

      spSrcEvent->getParam("asid", asid);
      spSrcEvent->getParam("type", type);
      spSrcEvent->getParam("sesinfo board", board);
      spSrcEvent->getParam("sesinfo trunk", trunk);
      spSrcEvent->getParam("sesinfo channel", channel);

      
      bool bRes = false;
      if(trunk == "1") {
         bRes = _ipcTrunk1->write(spEvent);
      } else if (trunk == "2") {
         bRes = _ipcTrunk2->write(spEvent);
      } else if (trunk == "3") {
         bRes = _ipcTrunk3->write(spEvent);
      } else if (trunk == "4") {
         bRes = _ipcTrunk4->write(spEvent);
      } 
      
      if(!bRes) {
         type += "_RES";
         spSrcEvent->addParam("source", name());
         spSrcEvent->addParam("type", type);
         spSrcEvent->addParam("reason", "invalid param"); 

         _aioRmIf->write(spEvent);

         std::cout << "## Failed to sending(PRMIFHandler)!" << std::endl;  
      }

      return true;
   }


   bool procTrunk(PEvent::Ptr spEvent) {
      PJsonEvent::Ptr spSrcEvent = std::dynamic_pointer_cast<PJsonEvent>(spEvent);

      std::string asid;
      std::string type;
      std::string board;
      std::string trunk;
      std::string channel;


      spSrcEvent->getParam("asid", asid);
      spSrcEvent->getParam("type", type);
      spSrcEvent->getParam("sesinfo board", board);
      spSrcEvent->getParam("sesinfo trunk", trunk);
      spSrcEvent->getParam("sesinfo channel", channel);

      bool bRes = false;
      if(asid == "1") {
        bRes |= _aioRmIf->write(spSrcEvent);
      }

      if(bRes) {
         std::cout << "## Error Response : " << spSrcEvent->getString() << std::endl;
      }

      return true;
   }

   PRMCliAio * _aioRmIf;
   PIPCAio *   _ipcTrunk1;
   PIPCAio *   _ipcTrunk2;
   PIPCAio *   _ipcTrunk3;
   PIPCAio *   _ipcTrunk4;
};



class PTrunkHandler : public PHandler
{
public: 
   PTrunkHandler(const std::string & name) : PHandler(name) {}
   ~PTrunkHandler() {};
///
   bool init() {
      bool bRes;

      _ipcRmIf = new PIPCAio(10, name());
      
      bRes = _ipcRmIf->open(name(), 100, "RMIF", 100);
      if(!bRes) {
         std::cout << "## failed open(_ipcRmIf)!" << std::endl;
         exit(1);
      }
      addAio(_ipcRmIf);

      _aioNms = new PLoopAio(name());
      addAio(_aioNms);

      return true;
   }


#if 0
   bool proc() {
      return false;
   }
#endif  

   bool proc() { return true; }

   bool proc(int id, const std::string & eventName, PEvent::Ptr spEvent) {
      //std::cout << name() << " : PEvent ptr :" << spEvent.get() << std::endl;   


      if(!spEvent) {
         std::cout << "## evnet is null" << std::endl;   
         
         exit(1);
      }
      PJsonEvent::Ptr spSrcEvent = std::dynamic_pointer_cast<PJsonEvent>(spEvent); 

      //std::cout <<  __FILE__ << "(" <<  __LINE__ <<  ")" << "PTrunkHandler(" << name() << ")" 
      //          << "->proc(spEvent=" << spSrcEvent->getString() << std::endl;
      //std::cout << "Proc :" << spSrcEvent->getString() << std::endl;   
      
      std::string strSrc; 
      if(!spSrcEvent->getParam("source", strSrc)) {
         std::cout << "Invalid Message!" << spSrcEvent->getString() << std::endl;   
      }

      if(strSrc == "RMIF") 
      {
         procRMIF(spEvent);
      } else if(strSrc == "Trunk1" 
             || strSrc == "Trunk2"
             || strSrc == "Trunk3"
             || strSrc == "Trunk4")
      {
         procTrunk(spEvent);
      }
      return true;
   }


   bool procRMIF(PEvent::Ptr spEvent) {
      PJsonEvent::Ptr spSrcEvent = std::dynamic_pointer_cast<PJsonEvent>(spEvent); 
      //std::cout << "ProcTestIF :" << spSrcEvent->getString() << std::endl;   

      std::string asid;
      std::string type;
      std::string board;
      std::string trunk;
      std::string channel;


      spSrcEvent->getParam("asid", asid);
      spSrcEvent->getParam("type", type);
      spSrcEvent->getParam("sesinfo board", board);
      spSrcEvent->getParam("sesinfo trunk", trunk);
      spSrcEvent->getParam("sesinfo channel", channel);

      
      bool bRes = _aioNms->write(spEvent);
      
      if(!bRes) {
         type += "_RES";
         spSrcEvent->addParam("source", name());
         spSrcEvent->addParam("type", type);
         spSrcEvent->addParam("reason", "invalid param"); 

         _ipcRmIf->write(spEvent);
      }

      return true;
   }


   bool procTrunk(PEvent::Ptr spEvent) {
      PJsonEvent::Ptr spSrcEvent = std::dynamic_pointer_cast<PJsonEvent>(spEvent);

      std::string asid;
      std::string type;
      std::string board;
      std::string trunk;
      std::string channel;


      spSrcEvent->getParam("asid", asid);
      spSrcEvent->getParam("type", type);
      spSrcEvent->getParam("sesinfo board", board);
      spSrcEvent->getParam("sesinfo trunk", trunk);
      spSrcEvent->getParam("sesinfo channel", channel);
      
      bool bRes = _ipcRmIf->write(spSrcEvent);

      if(!bRes) {
         std::cout << "## Error Response : " << spSrcEvent->getString() << std::endl;
      }
      return true;
   }

   // PHandler doesn't own AIOs in current design, but we use raw pointers for members to match addAio
   // If PLoopAio/PIPCAio inherited from enable_shared_from_this or similar, we could use shared_ptr here too.
   // For now, keeping these as raw pointers is fine as long as we pass shared_ptr events.
   PIPCAio *   _ipcRmIf;
   PLoopAio *  _aioNms;
};


int main(int argc, char ** argv) 
{
   PHandler * pRMIF = new PRMIFHandler("RMIF");

   PHandler * pTrunk1 = new PTrunkHandler("Trunk1");
   PHandler * pTrunk2 = new PTrunkHandler("Trunk2");
   PHandler * pTrunk3 = new PTrunkHandler("Trunk3");
   PHandler * pTrunk4 = new PTrunkHandler("Trunk4");

   PModule module1("TBAModule");

   module1.addWorker("RMIF");
   module1.addWorker("Trunk1");
   module1.addWorker("Trunk2");
   module1.addWorker("Trunk3");
   module1.addWorker("Trunk4");
   //module1.addWorker("TestWorker#2");

   module1.addHandler("RMIF", pRMIF);
   module1.addHandler("Trunk1", pTrunk1);
   module1.addHandler("Trunk2", pTrunk2);
   module1.addHandler("Trunk3", pTrunk3);
   module1.addHandler("Trunk4", pTrunk4);

   std::cout << module1.info() << std::endl;
   while(true) {
      msleep(10000);
   }



#if 0
   PJsonEvent json;
   std::string key = "a b c";
   std::string var;

   json.addParam(key, "1");

   json.getParam(key, var);

   PEvent * spEvent = &json;


   key = "a b a";
   json.addParam(key, "1");
   key = "a b b";
   json.addParam(key, "2");
   key = "a b c";
   json.addParam(key, "3");
   key = "a b d";
   json.addParam(key, "4");
   key = "a b e";
   json.addParam(key, "5");

   std::cout << "# key(" << key << ") : " << var << std::endl;
   std::cout << "# string : " << spEvent->getMsg() << std::endl;
#endif 
   return 0;
}
