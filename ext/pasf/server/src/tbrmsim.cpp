#include <iostream>
#include <typeinfo> 
#include <unistd.h>
#include "pevent.h"
#include "paio.h"
#include "pmodule.h"
#include "psocket.h"
#include "ptcpsrvhdl.h"

using namespace std;

class PTbaIfHandler : public PTcpCliHandler
{
public:
    PTbaIfHandler(const std::string & name) : PTcpCliHandler(name), _cnt(0) {}
    ~PTbaIfHandler() {};
    ///
    bool init() 
    {
        return true;
    }

    bool proc() 
    {
        //sleep(1);
        if(_cnt++%1000 != 0) return false;
        if (_spTcpAio) {
            /*
            PJsonEvent::Ptr spEvent = new PJsonEvent(name() + "->write");
            spEvent->addParam("source", "tbrm-simul");
            spEvent->addParam("asid", "testAsid");
            spEvent->addParam("type", "testTypr");
            _spTcpAio->write(spEvent);
            */
            cout << "test proc - OK(), " << typeid(*this).name() << endl;
        } 
        else {
            cout << "test proc(), " << typeid(*this).name() << endl;
        }
        return true;
    }


    bool proc(PEvent::Ptr & spEvent) 
    {
        //sleep(1);
        if(_cnt++%1000 != 0) return false;
        std::cout << "## tbaifhandler event is null" << std::endl;   

        if(!spEvent) {
        }
        else {
            if (_spTcpAio) {
                /*
                if (typeid(PJsonEvent) == typeid(PEvent)) {
                 PJsonEvent::Ptr spJsEvent = spEvent.cast<PJsonEvent>(); 
                    std::string type;
                    spJsEvent->getParam("type", type);

                if (type.find("disconnect")){
                    cout << "detachCliAio" << endl;
                    detachCliAio();
                }
                */
                //else { // svcevent
                    procTbaIF(spEvent);
                //}
            }
        }

        return true;
    }

    bool procTbaIF(PEvent::Ptr & spEvent) 
    {
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

        return true;
    }
    
    int _cnt;
};


int main(int argc, char ** argv) 
{
    PModule tbrmSimModule("TbrmSimModule");
    tbrmSimModule.addWorker("TbaIFSrvWorker");

    PTcpSrvHandler::Ptr spSrvHdl = new PTcpSrvHandler("PTcpSrvHandler");

    int nWorkerCnt = 1;
    int nWorkerPerHandler = 1;
    spSrvHdl->createServer("127.0.0.1", 14355, nWorkerCnt, nWorkerPerHandler);

    stringstream ss;
    for (int i=0; i< nWorkerCnt; i++) {
        ss << "CliHandler " << i;
        PTbaIfHandler::Ptr spCliHdl = new PTbaIfHandler(ss.str());
        spSrvHdl->addCliHandler(spCliHdl);
        ss.str("");
    }

    tbrmSimModule.addHandler("TbaIFSrvWorker", spSrvHdl);

    //std::cout << tbrmSimModule.info() << std::endl;

    while(true) {
        msleep(10000);
    }

    return 0;
}

