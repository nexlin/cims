#ifndef __PUDPHDL_H__
#define __PUDPHDL_H__

#include "phandler.h"
#include "pevent.h"
#include "pudpaio.h"



class PUdpHandler : public PHandler
{
public:
   PUdpHandler(const std::string & name, int nSockAioId); 

   virtual ~PUdpHandler() { cout << "~PUdpHandler(): name: " << name() <<  endl; }
   bool setPeer(bool bEnable, const std::string& strAddr, const int nPort); //locking

protected:
   virtual bool proc(int id, const std::string & name, PEvent * spEvent) = 0; //is event

   PUdpAio * getUdpAio() { return _spUdpAio; }

private:
   PUdpAio * _spUdpAio;

   bool _bEnable;
   int _nPort;
   int _nSockAioId; // _nClassifyId

   std::string _strAddr;
   PMutex _mutex;
};

#endif //  __PUDPHDL_H__
